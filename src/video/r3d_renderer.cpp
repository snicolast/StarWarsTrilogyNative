#include "r3d_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ao.frag.h"
#include "ao_rt.frag.h"
#include "ao_apply.frag.h"
#include "bloom.frag.h"
#include "compose.frag.h"
#include "compose.vert.h"
#include "r3d.frag.h"
#include "r3d_rt.frag.h"
#include "r3d.vert.h"
#include "rays.frag.h"
#include "tilegen.h"

namespace {

constexpr size_t TEXT_CELLS_BYTES = TextHD::ROW_TEXELS * TextHD::ROWS * 4;
constexpr size_t SKY_COORDS_BYTES = size_t(TileGen::SKY_COORDS_W) * TileGen::SKY_COORDS_H * 4;

struct ViewportPush {
    float proj[16];
    float sun[4];
    float fog_color[4];
    float params[4];    // ambient, fog density, fog start, interpolation of the previous image
    float quality[4];   // MSAA samples (negative: translucent pass; fraction: DLAA alpha noise seed),
                        // max anisotropic taps
};
static_assert(sizeof(ViewportPush) <= 128, "push constant budget");

// Uniform block of atmosphere.glsl
struct AtmosphereUniform {
    float axes[12];
    float camera[4];
    float sun[4];
    float view[4];
    float haze[4];
    float range[4];
    float backdrop[4];
    float scroll_fog[4];
    float scroll_rect[4];
    float shadow_matrix[16];
    float shadow[4];
    float soft[4];
    float mist[4];
    float sunlight[4];
    float spot[4];
    float spot_color[4];
    float spot_fog[4];
    float night[4];
    float relief[4];
    float sky_sun[4];
    float rt[4];
    float stars[12];      // procedural space: the star field's axes in view space; [3] 1 = on
    float stars_view[4];  // its viewport's projection (as view)
    float stars_rect[4];  // its rectangle in the 3D image (fractions)
    float day_sky[4];     // x 1 = Hoth's procedural sky in place of its sky dome
    float day_rect[4];    // the world viewport's rectangle in the 3D image (fractions)
    float glow[R3DScene::MAX_GLOWS * 12];   // lights of glowing things (R3DScene::glows): a + radius, b + strength, colour
    float glow_info[4];   // x: how many
};
static_assert(R3DScene::MAX_GLOWS == 8, "atmosphere.glsl's glow[] holds 8 lights (3 vec4 each)");

#ifdef __APPLE__
// Shadow map, square: without ray tracing all the sun shadows come from it (SWT_SHADOW_SIZE to try others)
const uint32_t SHADOW_SIZE = getenv("SWT_SHADOW_SIZE") ? uint32_t(atoi(getenv("SWT_SHADOW_SIZE"))) : 4096;
#else
constexpr uint32_t SHADOW_SIZE = 2048;   // shadow map, square
#endif
constexpr float SHADOW_RANGE = 220.0f;   // how far in front of the camera shadows are drawn (view units)

VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

}  // namespace

// Push constants of compose.frag (layout(push_constant) uniform Compose), in
// its order; what each field means is documented there
struct R3DRenderer::ComposePush {
    float scale[2], offset[2], crosshair[2], window[2], layer2d[2], filter2d[2];
    float hud[4], sky[4], effects[4], motion[4], crosshair2[4];
};

ScreenLayout compute_layout(float win_w, float win_h, float max_aspect) {
    ScreenLayout l{};
    float aspect = win_h > 0 ? win_w / win_h : 4.0f / 3.0f;
    float max_w = SCREEN_H * std::max(max_aspect, float(SCREEN_W) / SCREEN_H);
    l.view_w = std::clamp(SCREEN_H * aspect, float(SCREEN_W), max_w);
    l.scale = std::min(win_w / l.view_w, win_h / SCREEN_H);
    l.ox = (win_w - l.view_w * l.scale) / 2;
    l.oy = (win_h - SCREEN_H * l.scale) / 2;
    l.game_x0 = (l.view_w - SCREEN_W) / 2;
    return l;
}

bool R3DRenderer::create_buffer(Buffer& b, size_t size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(dev_, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    b.size = size;
    return true;
}

void R3DRenderer::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer{};
}

bool R3DRenderer::create_image(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkSampleCountFlagBits samples) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(dev_, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(dev_, &vi, nullptr, &img.view);
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.w = w;
    img.h = h;
    return true;
}

void R3DRenderer::destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
    if (img.alpha_view) vkDestroyImageView(dev_, img.alpha_view, nullptr);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev_, img.memory, nullptr);
    img = Image{};
}

void R3DRenderer::transition(VkCommandBuffer cmd, Image& img, VkImageLayout to, VkImageAspectFlags aspect) {
    if (img.layout == to) return;
    vk_image_barrier(cmd, img.image, img.layout, to, aspect);
    img.layout = to;
}

bool R3DRenderer::init(VkPresenter& vk, float supersample, int msaa, int aniso, bool dlaa) {
    vk_ = &vk;
    dev_ = vk.device();
    supersample_ = std::clamp(supersample, 0.5f, 4.0f);
    aniso_ = float(std::clamp(aniso, 0, 16));
    // DLAA does the anti-aliasing itself from aliased single-sample input
    if (dlaa && dlaa_.init(vk.instance(), vk.gpu(), dev_)) msaa = 1;
    // Highest supported MSAA level not above the requested one
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu(), &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples_ = VK_SAMPLE_COUNT_1_BIT;
    for (int s = 2; s <= std::min(msaa, 8); s *= 2)
        if (counts & s) samples_ = VkSampleCountFlagBits(s);

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(texture_ram_, 2048, 2048, VK_FORMAT_R16_UINT, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(back_, BACK_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(front_, SCREEN_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_buffer(texture_staging_, 2048 * 2048 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    if (!create_image(text_cells_, TextHD::ROW_TEXELS, TextHD::ROWS, VK_FORMAT_B8G8R8A8_UNORM,
                      sampled, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_image(sky_coords_, TileGen::SKY_COORDS_W, TileGen::SKY_COORDS_H, VK_FORMAT_B8G8R8A8_UNORM, sampled,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_buffer(layer_staging_, (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES + SKY_COORDS_BYTES,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    if (!create_buffer(vertices_, 8u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!create_buffer(atmosphere_uniform_, sizeof(AtmosphereUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    memset(atmosphere_uniform_.mapped, 0, sizeof(AtmosphereUniform));

    // Clear the texture RAM and the 2D layers once
    if (!create_image(shadow_map_, SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    if (!create_image(exposure_, 1, 1, VK_FORMAT_R16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    vk.run_once([&](VkCommandBuffer cmd) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkClearColorValue start{};
        start.float32[0] = std::log(0.17f);   // a typical scene: exposure 1
        VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, exposure_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &start, 1, &one);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_}) {
            transition(cmd, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            transition(cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &nearest_);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(dev_, &si, nullptr, &linear_);
    // Replacement textures: trilinear + anisotropic, repeating (mirroring is done in the shader)
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = std::min(16.0f, props.limits.maxSamplerAnisotropy);
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev_, &si, nullptr, &replace_sampler_);
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &overlay_sampler_);

    // Ray tracing, when the GPU has it: its acceleration structure is built once
    // empty here, so the descriptors always point at a valid one
    if (rt_.init(vk)) printf("[video] ray tracing available (sun shadows)\n");

    // Descriptor sets: texture RAM for the 3D pass; 3D image + two 2D layers for composition;
    // both also see the atmosphere parameters
    // (3: the 2D back layer, which the distance haze fades the far scenery into)
    VkDescriptorSetLayoutBinding b0[8] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TexturePack::MAX_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // sun shadow map
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // world depth (soft particles)
        {6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // ray tracing: the world
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // ray tracing: cut-out triangles
    };
    const uint32_t r3d_bindings = rt_.ready() ? 8 : 6;
    // Replacement slots are filled while earlier frames may still be in flight
    VkDescriptorBindingFlags bflags[8] = {
        0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, 0, 0, 0, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = r3d_bindings;
    bfi.pBindingFlags = bflags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.bindingCount = r3d_bindings;
    li.pBindings = b0;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &r3d_set_layout_);
    li.pNext = nullptr;
    // 5: high-resolution text atlas (only bound when there is one), 6: text cells,
    // 7: alpha of the 3D image (in red), 8: bloom, 9: backdrop replacement coordinates,
    // 10: light shafts, 11: exposure, 12: diffusion, 13: tight bloom
    VkDescriptorSetLayoutBinding cb[14];
    for (uint32_t i = 0; i < 3; i++)
        cb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Pack2D::MAX_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[13] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorBindingFlags cflags[14] = {0, 0, 0, bflags[1], 0, bflags[1], 0, 0, 0, 0, 0, 0, 0, 0};
    bfi.bindingCount = 14;
    bfi.pBindingFlags = cflags;
    li.pNext = &bfi;
    li.bindingCount = 14;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &compose_set_layout_);
    li.pNext = nullptr;
    // Bloom passes: one source image each
    li.bindingCount = 1;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &bloom_set_layout_);
    // Ambient occlusion: world depth and the raw occlusion
    li.bindingCount = 2;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &ao_set_layout_);

    VkDescriptorPoolSize ps[4] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36 + TexturePack::MAX_SLOTS + Pack2D::MAX_SLOTS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 10;
    pi.poolSizeCount = rt_.ready() ? 4 : 2;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r3d_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &r3d_set_);
    ai.pSetLayouts = &compose_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &compose_set_);
    ai.pSetLayouts = &bloom_set_layout_;
    for (VkDescriptorSet& s : bloom_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    vkAllocateDescriptorSets(dev_, &ai, &rays_set_);
    for (VkDescriptorSet& s : diff_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    ai.pSetLayouts = &ao_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &ao_set_);

    VkDescriptorImageInfo tex{nearest_, texture_ram_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = r3d_set_;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &tex;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo backdrop{linear_, back_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 3;
    w.pImageInfo = &backdrop;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo shadow{nearest_, shadow_map_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 4;
    w.pImageInfo = &shadow;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    if (rt_.ready()) {
        VkAccelerationStructureKHR tlas = rt_.tlas();
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet aw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        aw.pNext = &as;
        aw.dstSet = r3d_set_;
        aw.dstBinding = 6;
        aw.descriptorCount = 1;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        aw.pNext = nullptr;
        aw.dstBinding = 7;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        aw.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        rt_generation_ = rt_.generation();
    }
    w.dstBinding = 0;
    VkDescriptorBufferInfo atmosphere_info{atmosphere_uniform_.buffer, 0, sizeof(AtmosphereUniform)};
    VkWriteDescriptorSet fw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; i++) {
        fw[i].dstSet = i == 0 ? r3d_set_ : compose_set_;
        fw[i].dstBinding = i == 0 ? 2 : 4;
        fw[i].descriptorCount = 1;
        fw[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        fw[i].pBufferInfo = &atmosphere_info;
    }
    vkUpdateDescriptorSets(dev_, 2, fw, 0, nullptr);
    VkDescriptorImageInfo exposure{nearest_, exposure_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 11;
    w.pImageInfo = &exposure;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo cells{nearest_, text_cells_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 6;
    w.pImageInfo = &cells;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo sky{nearest_, sky_coords_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 9;
    w.pImageInfo = &sky;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);

    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ViewportPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r3d_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &r3d_layout_);
    VkPushConstantRange cpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ComposePush)};
    pli.pSetLayouts = &compose_set_layout_;
    pli.pPushConstantRanges = &cpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &compose_layout_);
    VkPushConstantRange bpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    pli.pSetLayouts = &bloom_set_layout_;
    pli.pPushConstantRanges = &bpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &bloom_layout_);
    VkPushConstantRange apr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    // Traced, the ambient occlusion also sees the 3D pass's set (the world's rays)
    VkDescriptorSetLayout ao_sets[2] = {ao_set_layout_, r3d_set_layout_};
    pli.setLayoutCount = rt_.ready() ? 2 : 1;
    pli.pSetLayouts = ao_sets;
    pli.pPushConstantRanges = &apr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &ao_layout_);

    return create_pipelines();
}

VkPipeline R3DRenderer::make_pipeline(const PipelineDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.fs, "main"};

    VkVertexInputBindingDescription binding{0, sizeof(R3DVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[9] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, nrm)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(R3DVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(R3DVertex, color)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, tex)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, flags)},
        {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, shade)},
        {7, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, replace)},
        {8, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, prev)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.vertex_input) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 9;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // facing is decided on the CPU with the face normal
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse Z
    VkPipelineColorBlendAttachmentState cbas[3]{};
    VkPipelineColorBlendAttachmentState& cba = cbas[0];
    cba.colorWriteMask = 0xF;
    // Motion vectors: written by the opaque pass only, translucent surfaces keep what is behind
    cbas[1].colorWriteMask = d.depth_write ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT : 0;
    if (d.blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.additive) {   // colour = colour + output (the sky's glow added to the bloom)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.ease > 0) {   // colour moves a fraction of the way towards the output (the exposure)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    }
    if (d.multiply) {   // colour = colour * output (ambient occlusion); alpha kept
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    }
    // Bloom source: blended like the colour. With it the motion slot is always
    // there (without an image when DLAA is off)
    cbas[2] = cba;
    if (d.depth_only)
        for (auto& c : cbas) c.colorWriteMask = 0;
    uint32_t attachments = d.glow ? 3 : d.motion ? 2 : 1;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = attachments;
    cb.blendConstants[3] = d.ease;
    cb.pAttachments = cbas;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat formats[3] = {d.color, d.motion ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM};
    rend.colorAttachmentCount = attachments;
    rend.pColorAttachmentFormats = formats;
    rend.depthAttachmentFormat = d.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS)
        fprintf(stderr, "[vulkan] could not create a pipeline\n");
    return p;
}

bool R3DRenderer::create_pipelines() {
    VkShaderModule r3d_vs = make_module(dev_, spv_r3d_vert, sizeof(spv_r3d_vert));
    // With ray queries the 3D pass can also trace the sun shadows
    VkShaderModule r3d_fs = rt_.ready() ? make_module(dev_, spv_r3d_rt_frag, sizeof(spv_r3d_rt_frag))
                                        : make_module(dev_, spv_r3d_frag, sizeof(spv_r3d_frag));
    VkShaderModule c_vs = make_module(dev_, spv_compose_vert, sizeof(spv_compose_vert));
    VkShaderModule c_fs = make_module(dev_, spv_compose_frag, sizeof(spv_compose_frag));
    VkShaderModule b_fs = make_module(dev_, spv_bloom_frag, sizeof(spv_bloom_frag));
    VkShaderModule rays_fs = make_module(dev_, spv_rays_frag, sizeof(spv_rays_frag));
    VkShaderModule ao_fs = rt_.ready() ? make_module(dev_, spv_ao_rt_frag, sizeof(spv_ao_rt_frag))
                                       : make_module(dev_, spv_ao_frag, sizeof(spv_ao_frag));
    VkShaderModule aoa_fs = make_module(dev_, spv_ao_apply_frag, sizeof(spv_ao_apply_frag));
    bool motion = motion_pass_ = dlaa_.ready();
    // The world: 3D geometry into the HDR image, with the motion and glow slots
    PipelineDesc world{.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .depth = true, .depth_write = true, .vertex_input = true, .samples = samples_,
                       .motion = motion, .glow = true};
    opaque_ = make_pipeline(world);
    PipelineDesc translucent = world;
    translucent.depth_write = false;
    translucent.blend = true;
    translucent_ = make_pipeline(translucent);
    PipelineDesc trans_depth = world;
    trans_depth.depth_only = true;
    trans_depth_ = make_pipeline(trans_depth);
    shadow_ = make_pipeline({.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_UNDEFINED,
                             .depth = true, .depth_write = true, .vertex_input = true});
    // Full-screen passes: a triangle over the target, no vertex input
    compose_format_ = vk_->swap_format();
    PipelineDesc compose{.vs = c_vs, .fs = c_fs, .layout = compose_layout_, .color = compose_format_};
    compose_ = make_pipeline(compose);
    PipelineDesc blur{.vs = c_vs, .fs = b_fs, .layout = bloom_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM};
    bloom_ = make_pipeline(blur);
    blur.fs = rays_fs;
    rays_ = make_pipeline(blur);
    // A twentieth of the way per frame: the eye adapts over about a second
    PipelineDesc measure = compose;
    measure.color = VK_FORMAT_R16_SFLOAT;
    measure.ease = 0.05f;
    compose_measure_ = make_pipeline(measure);
    PipelineDesc glow = compose;
    glow.color = VK_FORMAT_R8G8B8A8_UNORM;
    glow.additive = true;
    compose_glow_ = make_pipeline(glow);
    ao_pass_ = make_pipeline({.vs = c_vs, .fs = ao_fs, .layout = ao_layout_, .color = VK_FORMAT_R8_UNORM});
    ao_apply_ = make_pipeline({.vs = c_vs, .fs = aoa_fs, .layout = ao_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                               .samples = samples_, .multiply = true});
    ao_apply_glow_ = make_pipeline({.vs = c_vs, .fs = aoa_fs, .layout = ao_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM,
                                    .samples = samples_, .multiply = true});
    for (auto m : {r3d_vs, r3d_fs, c_vs, c_fs, b_fs, rays_fs, ao_fs, aoa_fs}) vkDestroyShaderModule(dev_, m, nullptr);
    return opaque_ && translucent_ && trans_depth_ && shadow_ && rays_ && compose_measure_ && compose_ && bloom_ && ao_pass_ && ao_apply_ && ao_apply_glow_;
}

void R3DRenderer::ensure_targets(uint32_t w, uint32_t h) {
    if (color_.image && color_.w == w && color_.h == h) return;
    vkDeviceWaitIdle(dev_);
    destroy_image(color_);
    destroy_image(color_ms_);
    destroy_image(depth_);
    destroy_image(motion_);
    destroy_image(dlaa_out_);
    destroy_image(dlaa_alpha_);
    for (Image* img : {&glow_, &glow_ms_, &bloom_a_, &bloom_b_, &bloom_c_, &depth_resolved_, &ao_, &rays_a_, &rays_b_, &diff_a_, &diff_b_})
        destroy_image(*img);
    create_image(color_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo avi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    avi.image = color_.image;
    avi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    avi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    avi.components = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE};
    avi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev_, &avi, nullptr, &color_.alpha_view);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        create_image(color_ms_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    bool dlaa = motion_pass_;
    create_image(depth_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
    // The world's depth, kept after its opaque surfaces (sample 0 of it with
    // MSAA, a copy without) for the ambient occlusion and the soft particles;
    // and the raw occlusion
    create_image(depth_resolved_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The occlusion at half resolution (one pixel in four): ao_apply.frag
    // brings it back to full size along the depth edges
    create_image(ao_, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[2] = {
            {nearest_, depth_resolved_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_, ao_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet aw[2];
        for (int i = 0; i < 2; i++) {
            aw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            aw[i].dstSet = ao_set_;
            aw[i].dstBinding = uint32_t(i);
            aw[i].descriptorCount = 1;
            aw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            aw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 2, aw, 0, nullptr);
        // ... and for the soft particles in the 3D pass
        VkWriteDescriptorSet dw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        dw.dstSet = r3d_set_;
        dw.dstBinding = 5;
        dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw.pImageInfo = &src[0];
        vkUpdateDescriptorSets(dev_, 1, &dw, 0, nullptr);
    }
    if (dlaa) {
        create_image(motion_, w, h, VK_FORMAT_R16G16_SFLOAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_out_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_alpha_, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // Bloom: glow written by the 3D pass, then reduced to a quarter and blurred
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create_image(glow_, w, h, VK_FORMAT_R8G8B8A8_UNORM, target, VK_IMAGE_ASPECT_COLOR_BIT);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        create_image(glow_ms_, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    for (Image* img : {&bloom_a_, &bloom_b_, &bloom_c_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[6] = {{linear_, glow_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_c_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_c_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet bw[6];
        for (int i = 0; i < 6; i++) {
            bw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            bw[i].dstSet = i < 4 ? bloom_sets_[i] : compose_set_;
            bw[i].dstBinding = i < 4 ? 0 : i == 4 ? 8 : 13;
            bw[i].descriptorCount = 1;
            bw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 6, bw, 0, nullptr);
    }
    // Light shafts: the mask and the shafts, a quarter of the size
    for (Image* img : {&rays_a_, &rays_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[2] = {{linear_, rays_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, rays_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet rw[2];
        for (int i = 0; i < 2; i++) {
            rw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            rw[i].dstSet = i == 0 ? rays_set_ : compose_set_;
            rw[i].dstBinding = i == 0 ? 0 : 10;
            rw[i].descriptorCount = 1;
            rw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            rw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 2, rw, 0, nullptr);
    }
    // Diffusion: the picture behind the HUD and its blur, a quarter of the size
    for (Image* img : {&diff_a_, &diff_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[3] = {{linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet dw[3];
        for (int i = 0; i < 3; i++) {
            dw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            dw[i].dstSet = i < 2 ? diff_sets_[i] : compose_set_;
            dw[i].dstBinding = i < 2 ? 0 : 12;
            dw[i].descriptorCount = 1;
            dw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            dw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 3, dw, 0, nullptr);
    }
    composed_view_ = VK_NULL_HANDLE;
    if (dlaa_.ready()) bind_scene_image(dlaa_out_.view, dlaa_alpha_.view);
    else bind_scene_image(color_.view, color_.alpha_view);
    VkDescriptorImageInfo infos[3] = {
        {linear_, color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, front_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, back_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w3[3];
    for (uint32_t i = 1; i < 3; i++) {
        w3[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w3[i].dstSet = compose_set_;
        w3[i].dstBinding = i;
        w3[i].descriptorCount = 1;
        w3[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w3[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev_, 2, w3 + 1, 0, nullptr);
}

// Bindings 0 and 7 of the composition: the plain 3D image or the DLAA output,
// and its alpha
void R3DRenderer::bind_scene_image(VkImageView view, VkImageView alpha) {
    if (view == composed_view_) return;
    VkDescriptorImageInfo info[2] = {{linear_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {linear_, alpha, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = compose_set_;
        w[i].dstBinding = i ? 7 : 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    if (composed_view_) vkDeviceWaitIdle(dev_);   // the previous frame may still be reading it
    vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
    composed_view_ = view;
}

void R3DRenderer::set_layers(const uint32_t* back, const uint32_t* front) {
    uint8_t* dst = static_cast<uint8_t*>(layer_staging_.mapped);
    memcpy(dst, back, BACK_W * SCREEN_H * 4);
    memcpy(dst + BACK_W * SCREEN_H * 4, front, SCREEN_W * SCREEN_H * 4);
    layers_dirty_ = true;
    // Average colour of each row of the back layer, for the horizon colour
    for (int y = 0; y < SCREEN_H; y++) {
        float sum[3] = {0, 0, 0}, n = 0;
        for (int x = 0; x < BACK_W; x += 2) {
            uint32_t p = back[y * BACK_W + x];
            float a = float(p >> 24);
            if (a < 128) continue;
            sum[0] += float((p >> 16) & 255) / a;   // premultiplied
            sum[1] += float((p >> 8) & 255) / a;
            sum[2] += float(p & 255) / a;
            n++;
        }
        for (int i = 0; i < 3; i++) back_rows_[y * 4 + i] = sum[i];
        back_rows_[y * 4 + 3] = n;
    }
    {   // how bright the backdrop is on the whole (a snowfield's pale sky is all bright)
        double l = 0, n = 0;
        for (int y = 0; y < SCREEN_H; y++) {
            l += 0.2126 * back_rows_[y * 4] + 0.7152 * back_rows_[y * 4 + 1] + 0.0722 * back_rows_[y * 4 + 2];
            n += back_rows_[y * 4 + 3];
        }
        back_luma_ = n > 0 ? float(l / n) : 0.0f;
    }
    // Nothing but black (or nothing at all) behind the 3D image (the night at the trailer)
    // in the 4:3 area
    bool black = true;
    for (int y = 0; y < SCREEN_H && black; y++)
        for (int x = BACK_MARGIN; x < BACK_MARGIN + SCREEN_W; x++)
            if (back[y * BACK_W + x] & 0xFFFFFF) {
                black = false;
                break;
            }
    back_black_ = black;
}

void R3DRenderer::set_text_cells(const uint32_t* cells) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4, cells,
           TEXT_CELLS_BYTES);
}

void R3DRenderer::set_sky_coords(const uint32_t* coords) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES, coords,
           SKY_COORDS_BYTES);
    const uint32_t* shift = coords + (SCREEN_H + 1) * TileGen::SKY_COORDS_W;   // see set_back_shift
    sky_shift_.assign(shift, shift + SCREEN_H);
    sky_shift_serial_++;
}

void R3DRenderer::set_text_atlas(const TextHD& text) {
    if (text.atlas_rgba.empty()) return;
    upload_slot(compose_set_, 5, overlay_sampler_, 0, text.atlas_w, text.atlas_h, text.atlas_rgba);
}

void R3DRenderer::upload_texture_rows(const std::vector<uint16_t>& rows, int y0, int y1) {
    memcpy(static_cast<uint8_t*>(texture_staging_.mapped) + size_t(y0) * 2048 * 2, rows.data(), rows.size() * 2);
    if (tex_y0_ >= tex_y1_) {
        tex_y0_ = y0;
        tex_y1_ = y1;
    } else {
        tex_y0_ = std::min(tex_y0_, y0);
        tex_y1_ = std::max(tex_y1_, y1);
    }
}

void R3DRenderer::upload_slot(VkDescriptorSet set, uint32_t binding, VkSampler sampler, uint32_t slot, int w, int h,
                              const std::vector<uint8_t>& rgba, bool replace) {
    {
        uint32_t mips = 1;
        while ((std::max(w, h) >> mips) > 0) mips++;
        Image img;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {uint32_t(w), uint32_t(h), 1};
        ii.mipLevels = mips;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(dev_, &ii, nullptr, &img.image) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev_, &ai, nullptr, &img.memory) != VK_SUCCESS) {
            vkDestroyImage(dev_, img.image, nullptr);
            return;
        }
        vkBindImageMemory(dev_, img.image, img.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &img.view);
        img.w = uint32_t(w);
        img.h = uint32_t(h);

        Buffer staging;
        if (!create_buffer(staging, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
        memcpy(staging.mapped, rgba.data(), rgba.size());
        vk_->run_once([&](VkCommandBuffer cmd) {
            auto barrier = [&](uint32_t level, VkImageLayout from, VkImageLayout to) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img.image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
            };
            barrier(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {img.w, img.h, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            // Mip chain: each level is a filtered blit of the previous one
            for (uint32_t l = 1; l < mips; l++) {
                barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                blit.srcOffsets[1] = {int32_t(std::max(1u, img.w >> (l - 1))), int32_t(std::max(1u, img.h >> (l - 1))), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                blit.dstOffsets[1] = {int32_t(std::max(1u, img.w >> l)), int32_t(std::max(1u, img.h >> l)), 1};
                vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            barrier(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        destroy_buffer(staging);
        img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo info{sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.dstBinding = binding;
        wr.dstArrayElement = slot;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &info;
        if (replace) {   // the slot's previous picture is no longer needed
            auto old = overlay_images_.find(slot);
            if (old != overlay_images_.end()) {
                vkDeviceWaitIdle(dev_);   // frames in flight may still read it
                destroy_image(old->second);
            }
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
            overlay_images_[slot] = img;
            return;
        }
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        replacements_.push_back(img);
    }
}

void R3DRenderer::add_replacements(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(r3d_set_, 1, replace_sampler_, src.slot, src.w, src.h, src.rgba);
}

void R3DRenderer::add_overlays(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, true);
}

void R3DRenderer::add_sky_pictures(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, true);
}

// The background layers continue beside the 4:3 area only while the 3D
// scene is widened too; a small 3D window (camera effects) keeps black sides
bool R3DRenderer::back_layer_extended() const {
    if (!scene_ || scene_->viewports.empty()) return true;
    int scene_priority = scene_->viewports.front().priority;
    for (const R3DViewport& v : scene_->viewports)
        if (v.priority == scene_priority && v.x <= 0.5f && v.w >= SCREEN_W - 0.5f) return true;
    return false;
}

// Scroll fog (spec 7.4): from the lowest priority that has any, the viewport
// with the strongest scroll fog. It is drawn only when a viewport with the
// same fog colour actually fogs something.
static const R3DViewport* scroll_fog_viewport(const std::vector<R3DViewport>& vps) {
    const R3DViewport* best = nullptr;
    for (const R3DViewport& v : vps) {   // sorted by priority
        if (best && v.priority != best->priority) break;
        if (v.scroll_fog > 0 && (!best || v.scroll_fog > best->scroll_fog)) best = &v;
    }
    if (!best) return nullptr;
    auto same = [&](const float* c) { return c[0] == best->fog_color[0] && c[1] == best->fog_color[1] && c[2] == best->fog_color[2]; };
    for (const R3DViewport& v : vps) {
        bool spot = v.fog_attenuation > 0 && (v.spot_fog_color[0] + v.spot_fog_color[1] + v.spot_fog_color[2]) > 0;
        if (same(v.fog_color) && (v.fog_density > 0 || v.fog_start > 0 || v.scroll_attenuation > 0 || spot)) return best;
    }
    return nullptr;
}

// The world viewport's camera between the previous game frame and the latest
// one, as the scenery is drawn (the interpolation); as in the latest frame
// after a cut (a jump of the camera, a sudden turn)
R3DRenderer::WorldPose R3DRenderer::world_pose(const R3DViewport* w) {
    if (pose_serial_ != scene_serial_) {
        pose_serial_ = scene_serial_;
        pose_prev_ = pose_now_;
        pose_now_.valid = w != nullptr;
        if (w) {
            memcpy(pose_now_.axes, w->world_axes, sizeof(pose_now_.axes));
            memcpy(pose_now_.camera, w->camera, sizeof(pose_now_.camera));
            float sl = std::sqrt(w->sun[0] * w->sun[0] + w->sun[1] * w->sun[1] + w->sun[2] * w->sun[2]);
            for (int i = 0; i < 3; i++) pose_now_.sun[i] = sl > 0 ? w->sun[i] / sl : 0;
        }
    }
    const WorldPose &a = pose_prev_, &b = pose_now_;
    auto dot = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    float d[3] = {b.camera[0] - a.camera[0], b.camera[1] - a.camera[1], b.camera[2] - a.camera[2]};
    pose_cut_ = !a.valid || !b.valid || dot(d, d) > 10.0f * 10.0f || dot(a.axes + 6, b.axes + 6) < 0.9f ||
                dot(a.axes + 3, b.axes + 3) < 0.9f;
    if (pose_cut_ || interp_ >= 1.0f) return b;
    WorldPose p = b;
    float t = interp_;
    for (int i = 0; i < 9; i++) p.axes[i] = a.axes[i] + (b.axes[i] - a.axes[i]) * t;
    for (int i = 0; i < 3; i++) p.camera[i] = a.camera[i] + (b.camera[i] - a.camera[i]) * t;
    for (int i = 0; i < 3; i++) p.sun[i] = a.sun[i] + (b.sun[i] - a.sun[i]) * t;
    for (float* v : {p.axes, p.axes + 3, p.axes + 6, p.sun}) {   // back to unit length
        float l = std::sqrt(dot(v, v));
        if (l > 0)
            for (int i = 0; i < 3; i++) v[i] /= l;
    }
    return p;
}

// The sun drawn in the sky and its light shafts (view space, unit length).
// The original's sky is a 2D picture that the game scrolls as the camera
// turns, though not always as far as the 3D world turns: looking up at the
// brachiosaurs in stage 2 the clouds swing from side to side while the
// game's light, high above, hardly moves. So the sun starts where the game's
// light comes from and then goes with the clouds, by the scroll of the row
// it is on; back to the game's light after a cut, where there is no sky
// picture, or once it has gone well outside the picture
void R3DRenderer::step_sky_sun(const WorldPose& pose, const float view[4], float view_w, float out[3]) {
    float light_uv[2];
    const float* L = pose.sun;
    bool in_front = L[2] < -0.05f;
    if (in_front) {
        light_uv[0] = (view[0] * L[0] / -L[2] - view[2]) * 0.5f + 0.5f;
        light_uv[1] = (view[1] * L[1] / -L[2] - view[3]) * 0.5f + 0.5f;
    }
    if (pose_cut_) sky_sun_carried_ = false;
    if (sky_sun_serial_ != sky_shift_serial_) {   // once per 2D frame
        sky_sun_serial_ = sky_shift_serial_;
        sky_sun_step_[0] = sky_sun_step_[1] = 0;
        if (sky_sun_carried_) {
            int row = std::clamp(int(sky_sun_uv_[1] * SCREEN_H), 0, SCREEN_H - 1);
            uint32_t s = row < int(sky_shift_.size()) ? sky_shift_[row] : 0;
            if (s) {   // the picture moves left as the scroll goes up
                sky_sun_step_[0] = -float(int((s >> 16) & 0xFF) - 128) / view_w;
                sky_sun_step_[1] = -float(int((s >> 8) & 0xFF) - 128) / float(SCREEN_H);
                sky_sun_uv_[0] += sky_sun_step_[0];
                sky_sun_uv_[1] += sky_sun_step_[1];
            }
            float outside = std::max({-sky_sun_uv_[0], sky_sun_uv_[0] - 1.0f, -sky_sun_uv_[1], sky_sun_uv_[1] - 1.0f});
            sky_sun_carried_ = s && outside < 0.5f;
        }
        if (!sky_sun_carried_ && in_front) {
            sky_sun_carried_ = true;
            sky_sun_uv_[0] = light_uv[0];
            sky_sun_uv_[1] = light_uv[1];
        }
    }
    if (!sky_sun_carried_ || view[0] == 0 || view[1] == 0) {
        memcpy(out, L, sizeof(float) * 3);
        return;
    }
    // Between 2D frames part of the way back, like the sky (set_back_shift)
    float x = (sky_sun_uv_[0] - sky_sun_step_[0] * back_shift_) * 2.0f - 1.0f;
    float y = (sky_sun_uv_[1] - sky_sun_step_[1] * back_shift_) * 2.0f - 1.0f;
    float d[3] = {(x + view[2]) / view[0], (y + view[3]) / view[1], -1.0f};
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) out[i] = d[i] / l;
}

// Atmosphere parameters for this frame (atmosphere.glsl)
void R3DRenderer::update_atmosphere(const ScreenLayout& lay, uint32_t target_w, uint32_t target_h) {
    AtmosphereUniform u{};
    rt_frame_ = rt_ao_ = false;
    u.backdrop[0] = 1.0f / float(target_w);
    u.backdrop[1] = 1.0f / float(target_h);
    // The original scroll fog, laid over the 2D layers behind the 3D image
    const R3DViewport* sf = scene_ && !scene_->viewports.empty() ? scroll_fog_viewport(scene_->viewports) : nullptr;
    if (sf) {
        for (int i = 0; i < 3; i++) u.scroll_fog[i] = sf->fog_color[i] * sf->fog_ambient;
        u.scroll_fog[3] = sf->scroll_fog;
        // Its rectangle in the 3D image, widened like the viewport when it fills the screen
        bool full = sf->x <= 0.5f && sf->w >= SCREEN_W - 0.5f;
        float x0 = full ? 0.0f : sf->x + lay.game_x0, w = full ? lay.view_w : sf->w;
        u.scroll_rect[0] = x0 / lay.view_w;
        u.scroll_rect[1] = sf->y / SCREEN_H;
        u.scroll_rect[2] = (x0 + w) / lay.view_w;
        u.scroll_rect[3] = (sf->y + sf->h) / SCREEN_H;
    }
    const R3DViewport* w = nullptr;
    if (scene_)   // the sun direction is also used by the sky's sun
        for (const R3DViewport& v : scene_->viewports)
            if (v.world) w = &v;
    WorldPose pose = world_pose(w);
    if (w) {
        // Game time (about 57.5 frames per second), so a replay drifts the same way
        float time = (float(scene_->frame % 100000) + interp_) / 57.5f;
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 3; i++) u.axes[k * 4 + i] = pose.axes[k * 3 + i];
        u.camera[0] = pose.camera[0];
        u.camera[1] = pose.camera[1];
        u.camera[2] = pose.camera[2];
        u.camera[3] = time;
        for (int i = 0; i < 3; i++) u.sun[i] = pose.sun[i];
        u.sun[3] = w->sun_intensity;
        static const char* force_sun = getenv("LW_FOGSUN");   // debugging aid: "x,y,z" in view space
        if (force_sun) {
            float f[3] = {0, 0, -1};
            sscanf(force_sun, "%f,%f,%f", &f[0], &f[1], &f[2]);
            float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            for (int i = 0; i < 3; i++) u.sun[i] = f[i] / fl;
            u.sun[3] = 1;
        }
        // Projection as used for the world viewport (widened when it fills the screen)
        bool full = w->x <= 0.5f && w->w >= SCREEN_W - 0.5f;
        float widen = full ? lay.view_w / SCREEN_W : 1.0f;
        u.view[0] = w->proj[0] / widen;
        u.view[1] = w->proj[5];
        u.view[2] = w->proj[8] / widen;
        u.view[3] = w->proj[9];

        // Distance haze. Horizon colour: a pale haze, tinted by the back
        // layer just above the horizon (where a level view direction meets
        // the screen), which is the sky or the painted far landscape
        float haze[3] = {0.60f, 0.66f, 0.64f};
        float target[3] = {haze[0], haze[1], haze[2]};
        const float* up = pose.axes + 3;
        if (std::fabs(up[1]) > 0.2f && u.view[0] != 0 && u.view[1] != 0) {
            float ndc_y = (up[2] - up[0] * u.view[2] / u.view[0]) * u.view[1] / up[1] - u.view[3];
            int row = int((ndc_y * 0.5f + 0.5f) * SCREEN_H);
            float sum[3] = {0, 0, 0}, n = 0;
            for (int y = std::max(row - 48, 0); y < std::min(row - 4, SCREEN_H); y++) {
                for (int i = 0; i < 3; i++) sum[i] += back_rows_[y * 4 + i];
                n += back_rows_[y * 4 + 3];
            }
            if (n > 500)
                for (int i = 0; i < 3; i++) {
                    float sky = sum[i] / n;
                    sky += (u.scroll_fog[i] - sky) * u.scroll_fog[3];
                    target[i] = 0.3f * sky + 0.7f * haze[i];
                }
        }
        for (int i = 0; i < 3; i++) haze_color_[i] += 0.05f * (target[i] - haze_color_[i]);
        for (int i = 0; i < 3; i++) u.haze[i] = haze_color_[i];
        u.haze[3] = haze_strength_;
        u.range[1] = std::max(w->scenery_far, 50.0f);
        u.range[0] = 0.1f * u.range[1];   // the tint starts past the near scenery
        u.range[2] = 0.6f;          // desaturation
        u.range[3] = back_layer_extended() ? 1.0f : 0.0f;
        u.backdrop[2] = lay.game_x0 / lay.view_w;
        u.backdrop[3] = SCREEN_W / lay.view_w;

        // World up and the camera's height above the ground: what may cast
        // sun shadows (the shadow map, the traced ones)
        for (int i = 0; i < 3; i++) shadow_up_[i] = pose.axes[3 + i];
        shadow_height_ = pose.camera[1] - w->ground;
        if (w->space) {   // no ground: everything casts shadows, and space has no haze
            shadow_height_ = -1e9f;
            u.haze[3] = 0;
            static const bool no_cinema = getenv("SWT_NO_CINEMA") != nullptr;
            u.relief[2] = no_cinema ? 0.0f : 1.0f;   // the films' grey, weathered hulls
        }
        // Ray tracing: the ambient occlusion through the world (all of it)
        if (ray_tracing_ >= 2 && rt_.ready() && ao_strength_ > 0) {
            rt_frame_ = rt_ao_ = true;
            rt_world_ = w;
        }
        // Sun shadows: an orthographic view from the sun covering what the
        // camera sees up to SHADOW_RANGE, plus whatever stands between it and the sun
        // (not indoors: a small world under a roof, Hoth's base, has no sun,
        // and its cables threw shadows onto the ceiling)
        bool indoors = w->scenery_far > 0 && w->scenery_far < 120.0f && !w->space;
        shadow_frame_ = shadow_strength_ > 0 && u.sun[3] > 0 && u.view[0] != 0 && u.view[1] != 0 && !indoors;
        if (shadow_frame_) {
            const float* L = u.sun;   // towards the sun (view space)
            float sun_up[3] = {0, 1, 0};   // any axis not parallel to the sun
            if (std::fabs(L[1]) > 0.95f) sun_up[0] = 1, sun_up[1] = 0;
            auto cross = [](const float* a, const float* b, float* o) {
                o[0] = a[1] * b[2] - a[2] * b[1];
                o[1] = a[2] * b[0] - a[0] * b[2];
                o[2] = a[0] * b[1] - a[1] * b[0];
            };
            float X[3], Y[3];
            cross(sun_up, L, X);
            float xl = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
            for (float& v : X) v /= xl;
            cross(L, X, Y);
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (float z : {1.0f, SHADOW_RANGE})
                for (float nx : {-1.0f, 1.0f})
                    for (float ny : {-1.0f, 1.0f}) {
                        float p[3] = {z * (nx + u.view[2]) / u.view[0], z * (ny + u.view[3]) / u.view[1], -z};
                        float l[3] = {p[0] * X[0] + p[1] * X[1] + p[2] * X[2], p[0] * Y[0] + p[1] * Y[1] + p[2] * Y[2],
                                      p[0] * L[0] + p[1] * L[1] + p[2] * L[2]};
                        for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], l[i]), hi[i] = std::max(hi[i], l[i]);
                    }
            // Square, on whole texels, so the shadows do not crawl as the camera turns
            float size = std::max(hi[0] - lo[0], hi[1] - lo[1]);
            float texel = size / float(SHADOW_SIZE);
            float cx = std::floor((lo[0] + hi[0]) * 0.5f / texel) * texel, cy = std::floor((lo[1] + hi[1]) * 0.5f / texel) * texel;
            float zmin = lo[2], zmax = hi[2] + 400.0f, zr = zmax - zmin;
            float* m = shadow_matrix_;   // column-major
            for (int i = 0; i < 3; i++) {
                m[i * 4 + 0] = X[i] * 2.0f / size;
                m[i * 4 + 1] = Y[i] * 2.0f / size;
                m[i * 4 + 2] = L[i] / zr;
                m[i * 4 + 3] = 0;
            }
            m[12] = -cx * 2.0f / size;
            m[13] = -cy * 2.0f / size;
            m[14] = -zmin / zr;
            m[15] = 1;
            memcpy(u.shadow_matrix, m, sizeof(u.shadow_matrix));
            u.shadow[0] = std::min(shadow_strength_, 1.0f) * 0.75f;
            u.shadow[1] = 1.0f / float(SHADOW_SIZE);
            u.shadow[2] = 2.5f * texel;   // normal offset
            u.shadow[3] = 1.5f * texel / zr;   // depth bias
            u.soft[3] = w->space ? 600.0f / zr : 0.0f;   // in space, ships shadow only what is close to them
            // Ray tracing: the same shadows traced through the world instead (no map drawn)
            if (ray_tracing_ >= 1 && rt_.ready()) {
                rt_frame_ = true;
                rt_world_ = w;
                static const bool split = getenv("LW_RTSPLIT");   // debugging aid: traced left, shadow map right
                shadow_frame_ = split;
                u.rt[0] = split ? 2.0f : 1.0f;
                u.rt[1] = 0.012f;   // a little more than the real sun's 0.27 degrees: softer edges
                u.rt[2] = 4;
                // With DLAA the spread of the rays turns every image, and DLAA averages them
                float golden = float(jitter_index_) * 0.618034f;
                u.rt[3] = dlaa_.ready() ? golden - std::floor(golden) : 0.0f;
            }
        }
        // The sun in the 3D image, for the light shafts
        step_sky_sun(pose, u.view, lay.view_w, u.sky_sun);
        if (force_sun) memcpy(u.sky_sun, u.sun, sizeof(float) * 3);
        const float* S = u.sky_sun;
        sun_light_ = 0;
        if (S[2] < -0.05f && u.sun[3] > 0) {
            sun_uv_[0] = (u.view[0] * S[0] / -S[2] - u.view[2]) * 0.5f + 0.5f;
            sun_uv_[1] = (u.view[1] * S[1] / -S[2] - u.view[3]) * 0.5f + 0.5f;
            // Fades out as it goes far outside the picture
            float out = std::max({-sun_uv_[0], sun_uv_[0] - 1.0f, -sun_uv_[1], sun_uv_[1] - 1.0f, 0.0f});
            sun_light_ = u.sun[3] * std::clamp(1.0f - out / 0.6f, 0.0f, 1.0f);
        }
        u.soft[0] = soft_particles_ ? 1.0f : 0.0f;
        u.soft[1] = 2.0f;
        // Light in the game's fog, with the distance haze: strength, brighter
        // upwards, drifting banks
        if (w->fog_density > 0) {
            u.mist[0] = std::min(haze_strength_, 1.0f);
            u.mist[1] = 0.45f;
            u.mist[2] = 0.12f;
            static const char* tune = getenv("LW_MIST");   // debugging aid: "upwards,banks"
            if (tune) sscanf(tune, "%f,%f", &u.mist[1], &u.mist[2]);
        }
    } else {
        shadow_frame_ = false;
        sun_light_ = 0;
    }
    u.soft[2] = hdr_ ? 1.0f : 0.0f;
    // SUN: more contrast in clear outdoor scenes (sunlit, little of the game's
    // own fog); the fogged, dark and indoor scenes keep the game's light
    if (w && sun_ && !w->space && w->sun_intensity > 0.3f) {
        float fogged = std::clamp(w->scroll_fog * 2.0f + w->fog_density * 300.0f, 0.0f, 1.0f);
        float k = 1.0f - fogged;
        u.sunlight[0] = 1.0f + 0.45f * k;
        u.sunlight[1] = 1.0f - 0.4f * k;
        u.sunlight[2] = k;
    }
    if (relief_) {
        u.relief[0] = 2.5f;
        u.relief[1] = relief_range_;   // detail only where it can be seen (it costs two more texture reads)
    }
    // The world viewport's spotlight (spec 7.1), applied to the game world
    if (w && w->spot_radius[0] > 0 && w->spot_radius[1] > 0) {
        u.spot[0] = w->spot_center[0];
        u.spot[1] = w->spot_center[1];
        u.spot[2] = 1.0f / w->spot_radius[0];
        u.spot[3] = 1.0f / w->spot_radius[1];
        for (int i = 0; i < 3; i++) {
            u.spot_color[i] = w->spot_color[i];
            u.spot_fog[i] = w->spot_fog_color[i] * w->fog_attenuation * w->fog_color[i];
        }
        u.spot_color[3] = w->spot_start;
        u.spot_fog[3] = w->spot_extent;
    }
    // Night (SKY: PROCEDURAL): a dim world on a black backdrop gets a night sky
    // It comes and goes gradually, so a frame or two of a different backdrop
    // (a fade, a flash) cannot make it blink
    bool night = w && night_sky_ && back_black_ && w->sun_intensity < 0.25f && w->ambient < 0.25f;
    night_level_ += 0.06f * ((night ? 1.0f : 0.0f) - night_level_);
    if (!night_sky_) night_level_ = 0;
    u.night[0] = night_level_;
    // Space (SKY: PROCEDURAL): a star field and the Milky Way in place of the
    // game's star field, turning with it (interpolated between game frames)
    const R3DViewport* sv = nullptr;
    if (scene_ && night_sky_)
        for (const R3DViewport& v : scene_->viewports)
            if (v.stars) sv = &v;
    if (sv) {
        if (star_serial_ != scene_serial_) {
            star_serial_ = scene_serial_;
            memcpy(star_prev_, star_valid_ ? star_now_ : scene_->star_axes, sizeof(star_prev_));
            memcpy(star_now_, scene_->star_axes, sizeof(star_now_));
            star_valid_ = true;
        }
        float t = std::clamp(interp_, 0.0f, 1.0f);
        for (int k = 0; k < 3; k++) {
            float a[3], l = 0;
            for (int i = 0; i < 3; i++) {
                a[i] = star_prev_[k * 3 + i] + (star_now_[k * 3 + i] - star_prev_[k * 3 + i]) * t;
                l += a[i] * a[i];
            }
            l = l > 0 ? 1.0f / std::sqrt(l) : 0.0f;
            for (int i = 0; i < 3; i++) u.stars[k * 4 + i] = a[i] * l;
        }
        u.stars[3] = 1.0f;
        bool full = sv->x <= 0.5f && sv->w >= SCREEN_W - 0.5f;
        float widen = full ? lay.view_w / SCREEN_W : 1.0f;
        u.stars_view[0] = sv->proj[0] / widen;
        u.stars_view[1] = sv->proj[5];
        u.stars_view[2] = sv->proj[8] / widen;
        u.stars_view[3] = sv->proj[9];
        float x0 = full ? 0.0f : sv->x + lay.game_x0, sw = full ? lay.view_w : sv->w;
        u.stars_rect[0] = x0 / lay.view_w;
        u.stars_rect[1] = sv->y / SCREEN_H;
        u.stars_rect[2] = (x0 + sw) / lay.view_w;
        u.stars_rect[3] = (sv->y + sv->h) / SCREEN_H;
    } else {
        star_valid_ = false;
    }
    u.day_sky[0] = night_sky_ && w && scene_ && scene_->sky_dome_hidden ? 1.0f : 0.0f;
    {   // the lightsabres' light (r3d_find_glows)
        int n = scene_ ? std::min<int>(int(scene_->glows.size()), R3DScene::MAX_GLOWS) : 0;
        for (int i = 0; i < n; i++) {
            const R3DScene::Glow& g = scene_->glows[i];
            float* o = u.glow + i * 12;
            o[0] = g.a[0], o[1] = g.a[1], o[2] = g.a[2], o[3] = g.radius;
            o[4] = g.b[0], o[5] = g.b[1], o[6] = g.b[2], o[7] = g.strength;
            o[8] = g.color[0], o[9] = g.color[1], o[10] = g.color[2], o[11] = 0;
        }
        u.glow_info[0] = float(n);
    }
    {   // the trench's fade into the dark (r3d_share_world), eased in and out
        float target = w && w->depth_fade > 0 ? 1.0f : 0.0f;
        trench_fade_ += std::clamp(target - trench_fade_, -0.03f, 0.03f);
        if (w && w->depth_fade > 0) trench_far_ = trench_far_ > 0 ? trench_far_ + 0.1f * (w->depth_fade - trench_far_) : w->depth_fade;
        u.day_sky[1] = trench_fade_ > 0 ? trench_far_ : 0.0f;
        u.day_sky[2] = trench_fade_;
    }
    if (u.day_sky[0] > 0) {   // only inside the world's viewport (the intro is letterboxed)
        bool full = w->x <= 0.5f && w->w >= SCREEN_W - 0.5f;
        float x0 = full ? 0.0f : w->x + lay.game_x0, ww = full ? lay.view_w : w->w;
        u.day_rect[0] = x0 / lay.view_w;
        u.day_rect[1] = w->y / SCREEN_H;
        u.day_rect[2] = (x0 + ww) / lay.view_w;
        u.day_rect[3] = (w->y + w->h) / SCREEN_H;
    }
    memcpy(atmosphere_uniform_.mapped, &u, sizeof(u));
}

// What every pass of a frame needs
struct R3DRenderer::Frame {
    VkCommandBuffer cmd;
    VkExtent2D extent;       // the window
    ScreenLayout lay;        // placement of the game screen in it
    uint32_t out_w, out_h;   // internal 3D resolution
    size_t vcount;           // vertices of the scene
    float before;            // motion vectors: how far back the previously displayed image was
    float jitter_x, jitter_y, alpha_seed;   // DLAA: sub-pixel shift and alpha-test noise seed
};

// Texture RAM rows, 2D layers and the scene's vertices changed since the last frame
void R3DRenderer::record_uploads(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    // Uploads
    if (tex_y0_ < tex_y1_) {
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(tex_y0_) * 2048 * 2;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, tex_y0_, 0};
        c.imageExtent = {2048, uint32_t(tex_y1_ - tex_y0_), 1};
        vkCmdCopyBufferToImage(cmd, texture_staging_.buffer, texture_ram_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tex_y0_ = tex_y1_ = 0;
    }
    if (layers_dirty_) {
        Image* imgs[2] = {&back_, &front_};
        for (int i = 0; i < 2; i++) {
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.bufferOffset = VkDeviceSize(i) * BACK_W * SCREEN_H * 4;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {imgs[i]->w, SCREEN_H, 1};
            vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(BACK_W + SCREEN_W) * SCREEN_H * 4;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {text_cells_.w, text_cells_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, text_cells_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        c.bufferOffset += TEXT_CELLS_BYTES;
        c.imageExtent = {sky_coords_.w, sky_coords_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, sky_coords_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        layers_dirty_ = false;
    }
    size_t total = vcount;
    if (total * sizeof(R3DVertex) > vertices_.size) {
        vkDeviceWaitIdle(dev_);
        destroy_buffer(vertices_);
        create_buffer(vertices_, total * sizeof(R3DVertex) * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    if (vcount) memcpy(vertices_.mapped, scene_->vertices.data(), vcount * sizeof(R3DVertex));
}

// Ray tracing: this image's world into the acceleration structure the 3D pass
// traces the sun shadows through. Before anything binds the 3D pass's
// descriptors, which may have to point at a new cut-out buffer
void R3DRenderer::record_ray_tracing(const Frame& f) {
    if (!rt_frame_ || !rt_world_) return;
    rt_.prepare(*scene_, *rt_world_, interp_, shadow_up_, shadow_height_, 35.0f, rt_ao_);
    if (rt_.generation() != rt_generation_) {
        rt_generation_ = rt_.generation();
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = r3d_set_;
        w.dstBinding = 7;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    }
    rt_.record(f.cmd);
}

void R3DRenderer::record_shadows(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    const float before = f.before;
    // Sun shadows: the opaque game world drawn from the sun, depth only
    if (shadow_frame_ && vcount) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo none{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};   // no colour image
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = shadow_map_.view;
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo sr{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sr.renderArea = {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        sr.layerCount = 1;
        sr.colorAttachmentCount = 1;
        sr.pColorAttachments = &none;
        sr.pDepthAttachment = &sd;
        vkCmdBeginRendering(cmd, &sr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_);
        VkViewport sv{0, 0, float(SHADOW_SIZE), float(SHADOW_SIZE), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &sv);
        VkRect2D ss{{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetScissor(cmd, 0, 1, &ss);
        for (const R3DViewport& v : scene_->viewports) {
            if (!(v.world || v.shares_world) || !v.opaque_count) continue;
            ViewportPush pc{};
            memcpy(pc.proj, shadow_matrix_, 64);
            pc.params[3] = before;
            pc.quality[0] = 1.0f;   // opaque rules (alpha test), no MSAA coverage
            pc.quality[1] = aniso_;
            pc.quality[3] = interp_;   // sun.w = 0: no lighting, so no shadow lookup while drawing it
            // Shadow pass marker, and only casters up to this height above the ground
            for (int i = 0; i < 3; i++) pc.sun[i] = shadow_up_[i];
            pc.fog_color[0] = shadow_height_;
            pc.fog_color[1] = 35.0f;
            pc.fog_color[3] = -1.0f;
            vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(cmd, v.opaque_count, 1, v.opaque_first, 0);
        }
        vkCmdEndRendering(cmd);
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

// The game world and HUD into color_ (and glow_, motion_, depth_)
void R3DRenderer::record_world(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const size_t vcount = f.vcount;
    const float before = f.before, jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    // 3D pass
    bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    color_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // contents are cleared every frame
    color_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    motion_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa) transition(cmd, color_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (motion_pass_) transition(cmd, motion_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    glow_.layout = glow_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, glow_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa) transition(cmd, glow_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo cas[3] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    VkRenderingAttachmentInfo& ca = cas[0];
    cas[1].imageView = motion_pass_ ? motion_.view : VK_NULL_HANDLE;
    cas[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cas[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cas[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.imageView = msaa ? color_ms_.view : color_.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // The ambient occlusion splits the 3D pass in two: the first part must keep everything
    bool ao_on = ao_strength_ > 0 && scene_ &&
                 std::any_of(scene_->viewports.begin(), scene_->viewports.end(), [](const R3DViewport& v) { return v.world; });
    // Soft particles need the world's depth too, so they split it as well
    bool split = ao_on || (soft_particles_ && scene_ &&
                           std::any_of(scene_->viewports.begin(), scene_->viewports.end(),
                                       [](const R3DViewport& v) { return v.world; }));
    ca.storeOp = msaa && !split ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {   // samples are averaged into the single-sample image at the end of the pass
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = color_.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    ca.clearValue.color = {{0, 0, 0, 0}};
    cas[2] = ca;   // bloom source, cleared to no glow
    cas[2].imageView = msaa ? glow_ms_.view : glow_.view;
    if (msaa) cas[2].resolveImageView = glow_.view;
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth_.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = motion_pass_ || split ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (split && msaa) {   // sample 0 of the depth, readable without multisampling
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = depth_resolved_.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    da.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {out_w, out_h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = cas;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);

    if (vcount) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
        float widen = lay.view_w / SCREEN_W;
        const auto& vps = scene_->viewports;
        size_t i = 0;
        bool first_priority = true;
        while (i < vps.size()) {
            size_t j = i;
            while (j < vps.size() && vps[j].priority == vps[i].priority) j++;
            if (!first_priority) {   // each priority starts with a cleared depth buffer
                VkClearAttachment clr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
                clr.clearValue.depthStencil = {0.0f, 0};
                VkClearRect rect{{{0, 0}, {out_w, out_h}}, 0, 1};
                vkCmdClearAttachments(cmd, 1, &clr, 1, &rect);
            }
            first_priority = false;
            bool world_here = false;
            for (size_t k = i; k < j; k++) world_here |= vps[k].world;
            // Star Wars Trilogy on the ground: the actors drawn with the world's
            // camera (walkers, troopers: their own viewport beside the world's)
            // go in after the ambient occlusion, which darkened them too much;
            // they keep the sun shadows. SWT_ACTOR_AO keeps it on them
            static const bool actor_ao = getenv("SWT_ACTOR_AO") != nullptr;
            bool in_space = false;
            for (size_t k = i; k < j; k++) in_space |= vps[k].world && vps[k].space;
            auto deferred = [&](const R3DViewport& v) {
                return swt_hud_ && !actor_ao && split && world_here && !in_space && v.shares_world;
            };
            // Steps: opaque, the occlusion then the deferred opaque actors, translucent
            for (int step = 0; step < 3; step++) {
                int pass = step == 2 ? 1 : 0;
                // Between the opaque and the translucent surfaces of the world:
                // the ambient occlusion, from its depth (the HUD clears it later)
                if (step == 1 && split && world_here) {
                    const R3DViewport* w = nullptr;
                    for (size_t k = i; k < j; k++)
                        if (vps[k].world) w = &vps[k];
                    vkCmdEndRendering(cmd);
                    if (gpu_times_ && !gpu_ao_marked_) gpu_time_mark(cmd, GPU_AO_FROM);
                    record_world_depth(f, *w, msaa, ao_on);
                    if (gpu_times_ && !gpu_ao_marked_) gpu_time_mark(cmd, GPU_AO_TO);
                    gpu_ao_marked_ = true;
                    // The 3D pass goes on where it was; the world depth stays
                    // readable (no resolve into it any more this frame)
                    VkRenderingAttachmentInfo again[3] = {cas[0], cas[1], cas[2]};
                    VkRenderingAttachmentInfo dagain = da;
                    for (auto& a : again) a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.resolveMode = VK_RESOLVE_MODE_NONE;
                    dagain.resolveImageView = VK_NULL_HANDLE;
                    VkRenderingInfo rag = ri;
                    rag.pColorAttachments = again;
                    rag.pDepthAttachment = &dagain;
                    vkCmdBeginRendering(cmd, &rag);
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0,
                                            nullptr);
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? opaque_ : translucent_);
                for (size_t k = i; k < j; k++) {
                    const R3DViewport& v = vps[k];
                    if (step < 2 && (step == 1) != deferred(v)) continue;
                    uint32_t count = pass == 0 ? v.opaque_count : v.trans_count;
                    if (!count) continue;
                    if (v.stars && night_sky_) continue;   // the procedural space instead (compose)
                    // Full-screen viewports are widened (wider horizontal field of
                    // view, same vertical one); smaller ones keep their 4:3 place
                    bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
                    bool to_edge = v.hud_left && swt_hud_ && hud_band_ && hud_edges_;
                    float vx = full ? 0.0f : v.x + (to_edge ? 0.0f : lay.game_x0), vw = full ? lay.view_w : v.w;
                    VkViewport viewport{vx * sx + jitter_x, v.y * sy + jitter_y, vw * sx, v.h * sy, 0.0f, 1.0f};
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    int32_t x0 = std::max(0, int32_t(vx * sx)), y0 = std::max(0, int32_t(v.y * sy));
                    VkRect2D sc{{x0, y0},
                                {std::min(out_w - uint32_t(x0), uint32_t(vw * sx + 0.5f)),
                                 std::min(out_h - uint32_t(y0), uint32_t(v.h * sy + 0.5f))}};
                    vkCmdSetScissor(cmd, 0, 1, &sc);
                    ViewportPush pc{};
                    memcpy(pc.proj, v.proj, 64);
                    if (full) {
                        pc.proj[0] /= widen;
                        pc.proj[8] /= widen;
                    }
                    pc.sun[0] = v.sun[0];
                    pc.sun[1] = v.sun[1];
                    pc.sun[2] = v.sun[2];
                    // The --light-gain knob: more light on the sun and on the ambient. It lands here
                    // rather than in the push block because that block is exactly 128 bytes,
                    // the guaranteed minimum, and there is no room left in it.
                    pc.sun[3] = v.sun_intensity * light_gain_;
                    pc.fog_color[0] = v.fog_color[0];
                    pc.fog_color[1] = v.fog_color[1];
                    pc.fog_color[2] = v.fog_color[2];
                    pc.fog_color[3] = v.fog_ambient;
                    pc.params[0] = v.ambient * light_gain_;
                    pc.params[1] = v.fog_density;
                    pc.params[2] = v.fog_start;
                    pc.params[3] = before;
                    pc.quality[0] = pass == 0 ? float(samples_) + alpha_seed : -float(samples_);
                    pc.quality[1] = aniso_;
                    pc.quality[2] = full && (!swt_hud_ || (hud_band_ && hud_edges_)) ? 2.0f * lay.game_x0 / lay.view_w : 0.0f;
                    pc.quality[3] = interp_;
                    // Translucent surfaces that are not effects (a whole model made
                    // see-through, like the Carnotaurus' camouflage): only the
                    // nearest one shows at each pixel, as on the Real3D (spec 7.2),
                    // so their depth goes first. Fire, smoke and other texture-alpha
                    // or luminous effects keep blending in list order.
                    if (pass == 1) {
                        ViewportPush pd = pc;
                        pd.quality[0] = -1000.0f;
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trans_depth_);
                        vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(pd), &pd);
                        vkCmdDraw(cmd, count, 1, v.trans_first, 0);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent_);
                    }
                    vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    vkCmdDraw(cmd, count, 1, pass == 0 ? v.opaque_first : v.trans_first, 0);
                }
            }
            i = j;
        }
    }
    vkCmdEndRendering(cmd);
    transition(cmd, color_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, glow_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Between the opaque and the translucent surfaces of the world: its depth
// made readable (soft particles, ambient occlusion) and, with ao_on, the
// ambient occlusion computed from it and multiplied into the colour so far.
// Called with no rendering in progress; the caller resumes the 3D pass.
void R3DRenderer::record_world_depth(const Frame& f, const R3DViewport& w, bool msaa, bool ao_on) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
    float widen = lay.view_w / SCREEN_W;
    if (!msaa) {   // a copy of the depth (with MSAA its sample 0 was resolved)
        transition(cmd, depth_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        depth_resolved_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {out_w, out_h, 1};
        vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_resolved_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    Image& dsrc = depth_resolved_;
    transition(cmd, dsrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (ao_on) {
    bool full = w.x <= 0.5f && w.w >= SCREEN_W - 0.5f;
    float vx = full ? 0.0f : w.x + lay.game_x0, vw = full ? lay.view_w : w.w;
    // 12 units, or less in a small world: Hoth's base (nearly everything
    // within 52 units) was all occluded, dark, and its lights' glow then
    // stood out as white blots
    // (and gentler there: between close walls everything is occluded, and
    // the stormtroopers went grey)
    float ao_radius = w.scenery_far > 0 ? std::clamp(0.04f * w.scenery_far, 1.5f, 12.0f) : 12.0f;
    float ao_gain = w.scenery_far > 0 ? std::clamp(w.scenery_far / 250.0f, 0.2f, 1.0f) : 1.0f;
    static const bool rt_split = getenv("LW_RTSPLIT") != nullptr;   // debugging aid: traced left, shadow map right
    float ap[16] = {w.proj[0] / (full ? widen : 1.0f), w.proj[5], w.proj[8] / (full ? widen : 1.0f),
                    w.proj[9], vx * sx + jitter_x, w.y * sy + jitter_y, vw * sx, w.h * sy,
                    ao_radius, std::min(ao_strength_, 1.0f) * 0.85f * ao_gain, w.fog_density, w.fog_start,
                    alpha_seed, 0.12f * float(out_h), rt_ao_ ? 6.0f : 0.0f, rt_split ? 1.0f : 0.0f};
    auto fullscreen = [&](VkPipeline pipe, Image& target, VkImageView view, uint32_t tw, uint32_t th) {
        VkRenderingAttachmentInfo at{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        at.imageView = view;
        at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at.loadOp = &target == &ao_ ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ar{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ar.renderArea = {{0, 0}, {tw, th}};
        ar.layerCount = 1;
        ar.colorAttachmentCount = 1;
        ar.pColorAttachments = &at;
        vkCmdBeginRendering(cmd, &ar);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0,
                                nullptr);
        if (rt_.ready())   // the traced ambient occlusion's rays (the layout always has the set)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 1, 1, &r3d_set_, 0, nullptr);
        VkViewport fv{0, 0, float(tw), float(th), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &fv);
        VkRect2D fr{{0, 0}, {tw, th}};
        vkCmdSetScissor(cmd, 0, 1, &fr);
        vkCmdPushConstants(cmd, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ap), ap);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };
    ao_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, ao_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen(ao_pass_, ao_, ao_.view, (out_w + 1) / 2, (out_h + 1) / 2);
    transition(cmd, ao_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Multiplied into the colour as drawn so far (the opaque world)
    Image& ctarget = msaa ? color_ms_ : color_;
    fullscreen(ao_apply_, ctarget, ctarget.view, out_w, out_h);
    // ... and into the glow: what the occlusion darkens glows less (Hoth's
    // base: its pale walls, darkened, still glowed in full as white blots)
    Image& gtarget = msaa ? glow_ms_ : glow_;
    fullscreen(ao_apply_glow_, gtarget, gtarget.view, out_w, out_h);
    }
}

void R3DRenderer::record_dlaa(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y;
    // DLAA: colour, depth and motion vectors in, anti-aliased image out
    if (dlaa_.ready()) {
        transition(cmd, depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dlaa_out_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_GENERAL);
        bool ok = dlaa_.evaluate(cmd, {color_.image, color_.view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                 {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                 {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                 {dlaa_out_.image, dlaa_out_.view, VK_FORMAT_R16G16B16A16_SFLOAT}, out_w, out_h, jitter_x,
                                 jitter_y, false);
        // Its alpha separately: where the 3D image is partly transparent the
        // 2D background shows through, and a jittered alpha would make every
        // edge against it shimmer (the ammo counter over the sky)
        dlaa_alpha_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_GENERAL);
        ok = ok && dlaa_.evaluate(cmd, {color_.image, color_.alpha_view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                  {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                  {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                  {dlaa_alpha_.image, dlaa_alpha_.view, VK_FORMAT_R8G8B8A8_UNORM}, out_w, out_h,
                                  jitter_x, jitter_y, false, 1);
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!ok) bind_scene_image(color_.view, color_.alpha_view);   // stopped working: plain image from now on
    }
}

// The composition's push constants for this frame
R3DRenderer::ComposePush R3DRenderer::compose_push(const Frame& f) const {
    static_assert(sizeof(ComposePush) == 128, "compose.frag push constant block");
    const VkExtent2D extent = f.extent;
    const ScreenLayout& lay = f.lay;
    float W = float(extent.width), H = float(extent.height);
    bool extend_back = back_layer_extended();
    // Drift of the procedural clouds
    static const auto launch = std::chrono::steady_clock::now();
    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - launch).count();
    ComposePush pc{.scale = {lay.view_w * lay.scale / W, SCREEN_H * lay.scale / H},
                   .offset = {lay.ox / W, lay.oy / H},
                   .crosshair = {-1, -1},
                   .window = {W, H},
                   .layer2d = {lay.game_x0 / lay.view_w, SCREEN_W / lay.view_w},
                   .filter2d = {smooth_2d_ ? 1.0f : 0.0f, lay.scale},
                   .hud = {hud_edges_ && hud_band_ ? lay.game_x0 : 0.0f, hdr_ ? 1.0f : 0.0f, overlay_brightness_,
                           extend_back ? 1.0f : 0.0f},
                   .sky = {seconds, 0, sun_ ? 1.0f : 0.0f, 0},
                   .effects = {bloom_strength_, saturation_, gamma_, 0},
                   .motion = {back_shift_, 0, swt_hud_ ? 1.0f : 0.0f, 0},
                   .crosshair2 = {-1, -1, sharpen_, 0}};
    // Space (r3d_share_world) and any world against a black backdrop (the
    // Death Star trench): the glow of lasers and engines does not depend on
    // how much of a big ship or the station's surface fills the view
    // (a star field behind the world, r3d_share_world; not just an empty back
    // layer: Hoth's sky is 3D, and its snow then glowed white all over)
    if (scene_ && std::any_of(scene_->viewports.begin(), scene_->viewports.end(),
                              [](const R3DViewport& v) { return v.world && v.space; }))
        pc.crosshair2[3] = 1.0f;
    // ... and those scenes get the cinematic grade (SWT_NO_CINEMA: off)
    static const bool no_cinema = getenv("SWT_NO_CINEMA") != nullptr;
    if (!no_cinema) pc.motion[1] = pc.crosshair2[3];
    float* at[2] = {pc.crosshair, pc.crosshair2};
    for (int p = 0; p < 2; p++)
        if (crosshair_visible_[p]) {   // game-screen fraction -> window pixels
            at[p][0] = lay.ox + (lay.game_x0 + crosshair_x_[p] * SCREEN_W) * lay.scale;
            at[p][1] = lay.oy + crosshair_y_[p] * SCREEN_H * lay.scale;
        }
    return pc;
}

// The reduced passes (glow, light shaft mask, diffusion) see the whole 3D
// view in their own target, without crosshair or sky glow
R3DRenderer::ComposePush R3DRenderer::reduced(const ComposePush& pc, const Image& to) {
    ComposePush p = pc;
    p.scale[0] = p.scale[1] = 1;
    p.offset[0] = p.offset[1] = 0;
    p.crosshair[0] = p.crosshair[1] = p.crosshair2[0] = p.crosshair2[1] = -1;
    p.filter2d[1] = float(to.h) / SCREEN_H;
    p.sky[1] = 0;
    return p;
}

// Starts rendering into a quarter-size image (light shafts, diffusion)
void R3DRenderer::begin_quarter(VkCommandBuffer cmd, Image& to, VkAttachmentLoadOp load) {
    to.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ra{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ra.imageView = to.view;
    ra.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ra.loadOp = load;
    ra.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri2.renderArea = {{0, 0}, {to.w, to.h}};
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &ra;
    vkCmdBeginRendering(cmd, &ri2);
    VkViewport rv{0, 0, float(to.w), float(to.h), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &rv);
    VkRect2D rr{{0, 0}, {to.w, to.h}};
    vkCmdSetScissor(cmd, 0, 1, &rr);
}

void R3DRenderer::record_bloom(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Bloom: the glow reduced to a quarter of the size, then blurred across and down
    if (bloom_strength_ > 0) {
        struct Step {
            int source;   // bloom_sets_ index
            Image* from;
            Image* to;
            float mode, spread;
        } steps[5] = {{0, &glow_, &bloom_a_, 0, 0},
                      {1, &bloom_a_, &bloom_b_, 1, 2.0f},
                      // the tight glow, kept in bloom_c_: the halo hugging lasers and engines
                      {2, &bloom_b_, &bloom_c_, 2, 2.0f},
                      // A second, wider round: the glow spreads across the screen
                      {3, &bloom_c_, &bloom_b_, 1, 5.0f},
                      {2, &bloom_b_, &bloom_a_, 2, 5.0f}};
        bool world = scene_ && std::any_of(scene_->viewports.begin(), scene_->viewports.end(),
                                           [](const R3DViewport& v) { return v.world; });
        // The sky glows less than fire: it is large and only its brightest part
        // counts. Only behind the game world: title and menu screens are 2D pictures
        // A sky that is bright all over (Hoth's pale sky over the snow) has no
        // bright part to pick out: its glow would wash the whole picture white
        float sky_glow = world ? 0.6f * std::clamp((0.8f - back_luma_) / 0.3f, 0.0f, 1.0f) : 0.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (const Step& st : steps) {
            st.to->layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo ba{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ba.imageView = st.to->view;
            ba.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ba.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ba.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo bi{VK_STRUCTURE_TYPE_RENDERING_INFO};
            bi.renderArea = {{0, 0}, {st.to->w, st.to->h}};
            bi.layerCount = 1;
            bi.colorAttachmentCount = 1;
            bi.pColorAttachments = &ba;
            vkCmdBeginRendering(cmd, &bi);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &bloom_sets_[st.source], 0,
                                    nullptr);
            VkViewport bv{0, 0, float(st.to->w), float(st.to->h), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &bv);
            VkRect2D br{{0, 0}, {st.to->w, st.to->h}};
            vkCmdSetScissor(cmd, 0, 1, &br);
            float bp[4] = {1.0f / float(st.from->w), 1.0f / float(st.from->h), st.mode, st.spread};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            if (st.mode == 0 && sky_glow > 0) {
                // The sky as the composition shows it (procedural or replaced,
                // with the fogs, minus what the 3D covers): its brightest part
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0,
                                        nullptr);
                ComposePush gp = reduced(pc, *st.to);
                gp.sky[1] = sky_glow;   // glow mode
                vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gp), &gp);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
            }
            vkCmdEndRendering(cmd);
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else {
        for (Image* img : {&bloom_a_, &bloom_c_})   // bound, though not read
            if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void R3DRenderer::record_light_shafts(const Frame& f, const ComposePush& pc, bool shafts) {
    VkCommandBuffer cmd = f.cmd;
    if (shafts) {
        begin_quarter(cmd, rays_a_, VK_ATTACHMENT_LOAD_OP_CLEAR);   // cleared: the mask pass adds onto it
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        ComposePush mp = reduced(pc, rays_a_);
        mp.sky[3] = 1;   // mask mode
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mp), &mp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, rays_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        begin_quarter(cmd, rays_b_, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rays_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &rays_set_, 0, nullptr);
        float rp[4] = {sun_uv_[0], sun_uv_[1], 0.5f * std::min(rays_strength_, 2.0f) * sun_light_, 0};
        vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rp), rp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, rays_b_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (rays_b_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, rays_b_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_diffusion(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Diffusion (BLOOM): the picture behind the HUD, then blurred across and down
    if (bloom_strength_ > 0) {
        begin_quarter(cmd, diff_a_, VK_ATTACHMENT_LOAD_OP_CLEAR);   // cleared: the scene pass adds onto it
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        ComposePush dp = reduced(pc, diff_a_);
        dp.sky[3] = 3;       // scene mode
        dp.effects[0] = 0;   // (no diffusion of the diffusion)
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dp), &dp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (int pass = 0; pass < 2; pass++) {
            Image& to = pass == 0 ? diff_b_ : diff_a_;
            begin_quarter(cmd, to, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &diff_sets_[pass], 0,
                                    nullptr);
            float bp[4] = {1.0f / float(to.w), 1.0f / float(to.h), pass == 0 ? 1.0f : 2.0f, 3.0f};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else if (diff_a_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_exposure(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // HDR lighting: the 3D image's average brightness, eased into exposure_
    if (hdr_) {
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // keeps its value
        VkRenderingAttachmentInfo ea{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ea.imageView = exposure_.view;
        ea.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ea.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ea.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo er{VK_STRUCTURE_TYPE_RENDERING_INFO};
        er.renderArea = {{0, 0}, {1, 1}};
        er.layerCount = 1;
        er.colorAttachmentCount = 1;
        er.pColorAttachments = &ea;
        vkCmdBeginRendering(cmd, &er);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_measure_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        VkViewport ev{0, 0, 1, 1, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &ev);
        VkRect2D eb{{0, 0}, {1, 1}};
        vkCmdSetScissor(cmd, 0, 1, &eb);
        ComposePush ep = pc;
        ep.sky[3] = 2;   // measure mode
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ep), &ep);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void R3DRenderer::record_compose(const Frame& f, const ComposePush& pc, VkImageView target) {
    VkCommandBuffer cmd = f.cmd;
    const VkExtent2D extent = f.extent;
    // Composition into the swapchain image
    VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ta.imageView = target;
    ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ta.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ti{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ti.renderArea = {{0, 0}, extent};
    ti.layerCount = 1;
    ti.colorAttachmentCount = 1;
    ti.pColorAttachments = &ta;
    vkCmdBeginRendering(cmd, &ti);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
    VkViewport full{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &full);
    VkRect2D all{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &all);
    vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// A frame, pass by pass (each one in its own function above)
void R3DRenderer::record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    // Placement in the window (widescreen or 4:3) and internal 3D resolution
    Frame f{};
    f.cmd = cmd;
    f.extent = extent;
    f.lay = compute_layout(float(extent.width), float(extent.height), max_aspect_);
    f.out_w = std::max(1u, uint32_t(f.lay.view_w * f.lay.scale * supersample_));
    f.out_h = std::max(1u, uint32_t(SCREEN_H * f.lay.scale * supersample_));
    ensure_targets(f.out_w, f.out_h);
    static const bool gpu_time = getenv("SWT_GPUTIME") || getenv("LW_GPUTIME");
    if (gpu_time && !gpu_times_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = GPU_MARKS;
        vkCreateQueryPool(dev_, &qi, nullptr, &gpu_times_);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk_->gpu(), &props);
        timestamp_period_ = props.limits.timestampPeriod;
    }
    gpu_time_report(f.out_w, f.out_h);
    gpu_ao_marked_ = false;
    if (gpu_times_) {
        vkCmdResetQueryPool(cmd, gpu_times_, 0, GPU_MARKS);
        gpu_time_mark(cmd, GPU_START);
    }
    f.vcount = scene_ ? scene_->vertices.size() : 0;
    record_uploads(f);
    update_atmosphere(f.lay, f.out_w, f.out_h);

    // Motion vectors point to where each surface was in the previously
    // displayed image: the same game frame part way back, or the previous
    // game frame (extrapolated backwards when it was itself interpolated)
    float before = scene_serial_ != drawn_serial_ ? drawn_interp_ - 1.0f : drawn_interp_;
    drawn_serial_ = scene_serial_;
    drawn_interp_ = interp_;
    // DLAA: the image is shifted by a different sub-pixel amount each frame
    // (Halton 2,3 over 8 phases) so it can gather detail between pixels
    float jitter_x = 0, jitter_y = 0, alpha_seed = 0;
    if (dlaa_.ready()) {
        auto halton = [](uint32_t i, uint32_t base) {
            float digit = 1, r = 0;
            for (; i; i /= base) {
                digit /= float(base);
                r += digit * float(i % base);
            }
            return r;
        };
        uint32_t phase = uint32_t(jitter_index_++ % 8) + 1;
        jitter_x = halton(phase, 2) - 0.5f;
        jitter_y = halton(phase, 3) - 0.5f;
        // Alpha-tested edges (foliage): noise threshold that changes every frame
        float golden = float(jitter_index_) * 0.618034f;
        alpha_seed = 0.01f + 0.98f * (golden - std::floor(golden));
    }
    f.before = before;
    f.jitter_x = jitter_x;
    f.jitter_y = jitter_y;
    f.alpha_seed = alpha_seed;

    if (gpu_times_) gpu_time_mark(cmd, GPU_UPLOADS);
    record_ray_tracing(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_RT);
    record_shadows(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_SHADOWS);
    record_world(f);
    if (gpu_times_ && !gpu_ao_marked_) {   // no occlusion pass this frame: it took no time
        gpu_time_mark(cmd, GPU_AO_FROM);
        gpu_time_mark(cmd, GPU_AO_TO);
    }
    if (gpu_times_) gpu_time_mark(cmd, GPU_WORLD);
    record_dlaa(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_DLAA);
    ComposePush pc = compose_push(f);
    record_bloom(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_BLOOM);
    // Light shafts: the visible sky around the sun, then smeared towards it
    bool shafts = rays_strength_ > 0 && sun_light_ > 0;
    pc.effects[3] = shafts ? 1.0f : 0.0f;
    record_light_shafts(f, pc, shafts);
    if (gpu_times_) gpu_time_mark(cmd, GPU_SHAFTS);
    record_diffusion(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_DIFFUSION);
    record_exposure(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_EXPOSURE);
    record_compose(f, pc, target);
    if (gpu_times_) {
        gpu_time_mark(cmd, GPU_COMPOSE);
        gpu_pending_ = true;
    }
}

// The previous frame is done (one frame in flight): its times are added up
void R3DRenderer::gpu_time_report(uint32_t out_w, uint32_t out_h) {
    if (!gpu_times_ || !gpu_pending_) return;
    uint64_t t[GPU_MARKS];
    if (vkGetQueryPoolResults(dev_, gpu_times_, 0, GPU_MARKS, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    auto ms = [&](uint32_t from, uint32_t to) { return double(t[to] - t[from]) * timestamp_period_ * 1e-6; };
    for (uint32_t i = 1; i < GPU_MARKS; i++) gpu_sum_[i] += ms(i - 1, i);
    gpu_sum_[0] += ms(GPU_START, GPU_COMPOSE);   // the whole frame
    if (++gpu_frames_ < 300) return;
    auto avg = [&](uint32_t i) { return gpu_sum_[i] / gpu_frames_; };
    // The 3D pass runs from the shadows to its end, around the occlusion
    double world = avg(GPU_AO_FROM) + avg(GPU_WORLD);
    printf("[gpu] ms/image %ux%u: total %.2f | uploads %.2f  rt build %.2f  shadows %.2f  3D %.2f  AO %.2f  "
           "DLAA %.2f  bloom %.2f  shafts %.2f  diffusion %.2f  exposure %.2f  compose %.2f  (%zu traced triangles)\n",
           out_w, out_h, avg(0), avg(GPU_UPLOADS), avg(GPU_RT), avg(GPU_SHADOWS), world, avg(GPU_AO_TO), avg(GPU_DLAA),
           avg(GPU_BLOOM), avg(GPU_SHAFTS), avg(GPU_DIFFUSION), avg(GPU_EXPOSURE), avg(GPU_COMPOSE),
           rt_frame_ ? rt_.triangles() : size_t(0));
    fflush(stdout);
    std::fill(std::begin(gpu_sum_), std::end(gpu_sum_), 0.0);
    gpu_frames_ = 0;
}

void R3DRenderer::gpu_time_mark(VkCommandBuffer cmd, uint32_t index) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_times_, index);
}

void R3DRenderer::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (auto p : {opaque_, translucent_, trans_depth_, shadow_, rays_, compose_measure_, compose_, bloom_, compose_glow_, ao_pass_, ao_apply_, ao_apply_glow_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    dlaa_.shutdown();
    rt_.shutdown();
    if (gpu_times_) vkDestroyQueryPool(dev_, gpu_times_, nullptr);
    for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_, &color_, &color_ms_, &depth_, &motion_, &dlaa_out_, &dlaa_alpha_,
                        &glow_, &glow_ms_, &bloom_a_, &bloom_b_, &bloom_c_, &depth_resolved_, &ao_, &shadow_map_, &rays_a_, &rays_b_, &exposure_, &diff_a_, &diff_b_})
        destroy_image(*img);
    for (Image& img : replacements_) destroy_image(img);
    for (auto& [slot, img] : overlay_images_) destroy_image(img);
    for (Buffer* b : {&texture_staging_, &layer_staging_, &vertices_, &atmosphere_uniform_}) destroy_buffer(*b);
}
