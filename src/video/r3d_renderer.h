// Vulkan renderer for the Real3D scene plus the 2D tilegen layers.
//
// Per frame: the 3D scene is drawn into an offscreen colour+depth target at
// the output resolution (times an optional supersampling factor), with
// optional MSAA resolved into a single-sample image (or NVIDIA DLAA fed with
// depth and per-pixel motion vectors instead), then a full-screen pass composes back 2D layers, the 3D image and front 2D layers
// into the swapchain image with letterboxing.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dlss.h"
#include "r3d_raytrace.h"
#include "r3d_scene.h"
#include "pack2d.h"
#include "sky_pack.h"
#include "text_hd.h"
#include "texture_pack.h"
#include "vk_presenter.h"

// Placement of the picture in the window. The 3D view can be wider than the
// original 4:3 screen (widescreen); the 2D layers and the gun area stay 4:3
// in the middle of it.
struct ScreenLayout {
    float view_w;    // width of the 3D view in Model 3 pixels (496 = original 4:3)
    float scale;     // window pixels per Model 3 pixel
    float ox, oy;    // window position of the 3D view
    float game_x0;   // Model 3 x of the 4:3 area inside the 3D view
};
// max_aspect <= 4/3 keeps the original 4:3 picture
ScreenLayout compute_layout(float win_w, float win_h, float max_aspect);

class R3DRenderer {
public:
    // msaa: samples per pixel (1 = off); aniso: max anisotropic taps (0 = no mipmaps);
    // dlaa: NVIDIA DLAA instead of MSAA (the presenter must have its extensions)
    bool init(VkPresenter& vk, float supersample, int msaa, int aniso, bool dlaa = false);
    int msaa_samples() const { return int(samples_); }
    bool dlaa_active() const { return dlaa_.ready(); }
    // Image slots this GPU can bind (slot 0 = none): the texture pack's and the
    // 2D replacement pictures' (1 = none: the 2D and backdrop packs are off)
    uint32_t replacement_slots() const { return replace_slots_; }
    uint32_t overlay_slots() const { return overlay_slots_; }

    // How much light the scene is given: a multiplier on the sun and on the ambient.
    void set_light_gain(float gain) {
        light_gain_ = gain < 0.1f ? 0.1f : (gain > 8.0f ? 8.0f : gain);
    }
    float light_gain() const { return light_gain_; }
    void shutdown();

    // Thread-safe sources are read by the caller; these just stage data
    // Called once per new game frame
    void set_scene(const R3DScene* scene) {
        scene_ = scene;
        scene_serial_++;
    }
    void set_layers(const uint32_t* back, const uint32_t* front);   // 496x384 BGRA each
    // High-resolution text: the glyph atlas (once) and, with every set_layers,
    // the text cells (TextHD::CELLS_X x CELLS_Y x CELL_TEXELS BGRA texels)
    void set_text_atlas(const TextHD& text);
    void set_text_cells(const uint32_t* cells);
    void upload_texture_rows(const std::vector<uint16_t>& rows, int y0, int y1);
    // Texture pack images, uploaded with a full mip chain into their slots
    void add_replacements(const std::vector<RgbaImage>& images);
    // 2D replacement pictures (those on screen are listed in the text cells,
    // see text_hd.h) and the fade offset applied to them
    void add_overlays(const std::vector<RgbaImage>& images);
    void set_overlay_brightness(float brightness) { overlay_brightness_ = brightness; }
    // Backdrop replacement pictures, and with every set_layers which one each
    // back layer pixel shows (TileGen::SKY_COORDS_W x SKY_COORDS_H BGRA texels)
    void add_sky_pictures(const std::vector<RgbaImage>& images);
    void set_sky_coords(const uint32_t* coords);
    // Extra crosshair at the real gun position of a player (fraction of the game screen)
    void set_crosshair(int player, bool visible, float x, float y) {
        crosshair_visible_[player] = visible;
        crosshair_x_[player] = x;
        crosshair_y_[player] = y;
    }

    void record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);
    void set_max_aspect(float aspect) { max_aspect_ = aspect; }
    // true: 2D layers upscaled with xBR; false: plain square pixels
    void set_smooth_2d(bool smooth) { smooth_2d_ = smooth; }
    // true: HUD elements at the sides go to the edges of a widescreen window
    void set_hud_edges(bool edges) { hud_edges_ = edges; }
    // false while the top of the 2D screen is a picture rather than a HUD
    void set_hud_band(bool hud) { hud_band_ = hud; }
    // Position between the previous (0) and the current (1) game frame
    void set_interpolation(float t) { interp_ = t; }
    // Fraction of their scroll since the previous game frame (the second extra
    // row of the sky coordinates) by which the back layers are drawn back
    // towards where they were: 1 - interpolation, 0 = as composed
    void set_back_shift(float f) { back_shift_ = f; }
    // Distance haze (not in the original): the scenery takes the colour of the
    // horizon with distance and fades into the 2D backdrop before it stops
    // being drawn; 0 = off, 1 = default strength
    void set_distance_haze(float strength) { haze_strength_ = strength; }
    // Bloom (not in the original): soft glow around the bright parts of
    // luminous surfaces (fire, explosions, lights); 0 = off, 1 = default
    void set_bloom(float strength) { bloom_strength_ = strength; }
    // Picture settings over the final image: colour saturation (0 = grey,
    // 1 = original) and gamma (1 = original, higher = brighter mid-tones)
    void set_picture(float saturation, float gamma) {
        saturation_ = saturation < 0 ? 0 : (saturation > 2 ? 2 : saturation);
        gamma_ = gamma < 0.3f ? 0.3f : (gamma > 3 ? 3 : gamma);
    }
    // Ambient occlusion (not in the original): corners, creases and the ground
    // around objects darken, from the depth of the game world; 0 = off, 1 = default
    void set_ambient_occlusion(float strength) { ao_strength_ = strength; }
    // A sun in the procedural sky where the game's light comes from (not in the original)
    void set_sun(bool on) { sun_ = on; }
    // Surface detail: the textures' light and dark read as relief by the lighting (not in the original)
    void set_relief(bool on) { relief_ = on; }
    void set_relief_range(float units) { relief_range_ = units; }   // how far the surface detail reaches (world units)
    // Contrast adaptive sharpening of the 3D image: 0 = off, 1 = strongest
    void set_sharpen(float amount) { sharpen_ = amount; }
    // A procedural night sky behind a world the game leaves on a black
    // backdrop in the dark (not in the original; part of SKY: PROCEDURAL)
    void set_night_sky(bool on) { night_sky_ = on; }
    bool back_black() const { return back_black_; }      // debugging aid (LW_SPOTLOG)
    float night_level() const { return night_level_; }
    // Sun shadows (not in the original): the game world casts shadows along
    // the game's own light direction; 0 = off, 1 = default strength
    void set_shadows(float strength) { shadow_strength_ = strength; }
    // Ray tracing (not in the original): 1 = the sun shadows are traced
    // through the game world instead of read from a shadow map, 2 = the
    // ambient occlusion too instead of taken from the depth; 0 = off. Only on
    // GPUs with ray queries (ray_tracing_available)
    void set_ray_tracing(int mode) { ray_tracing_ = mode; }
    bool ray_tracing_available() const { return rt_.ready(); }
    // Light shafts from the sun (not in the original); 0 = off, 1 = default
    void set_light_shafts(float strength) { rays_strength_ = strength; }
    // Soft particles (not in the original): translucent surfaces of the world
    // fade where they meet the geometry behind them
    void set_soft_particles(bool on) { soft_particles_ = on; }
    // HDR lighting (not in the original): the 3D image keeps highlights above
    // white and the composition rolls them off, with a gently adapting exposure
    void set_hdr(bool on) { hdr_ = on; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t size = 0;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageView alpha_view = VK_NULL_HANDLE;   // optional: alpha read as grey (rgb = a, a = 1)
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t w = 0, h = 0;
    };

    bool create_buffer(Buffer& b, size_t size, VkBufferUsageFlags usage);
    void destroy_buffer(Buffer& b);
    bool create_image(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void destroy_image(Image& img);
    void transition(VkCommandBuffer cmd, Image& img, VkImageLayout to,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    bool create_pipelines();
    // What a pipeline differs in; the rest (triangle list, no culling, reverse
    // Z, dynamic viewport and scissor) is the same for all
    struct PipelineDesc {
        VkShaderModule vs, fs;
        VkPipelineLayout layout;
        VkFormat color;                        // UNDEFINED = depth only (shadows)
        bool depth = false, depth_write = false;
        bool blend = false;                    // alpha blending (translucent surfaces)
        bool vertex_input = false;             // R3DVertex attributes (full-screen passes have none)
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        bool motion = false;                   // second attachment: motion vectors (DLAA)
        bool glow = false;                     // third attachment: bloom source
        bool multiply = false;                 // colour *= output (ambient occlusion)
        bool additive = false;                 // colour += output (sky glow)
        float ease = 0;                        // > 0: colour moves this fraction towards the output
        bool depth_only = false;               // no colour writes
    };
    VkPipeline make_pipeline(const PipelineDesc& d);
    void bind_scene_image(VkImageView view, VkImageView alpha);
    void update_atmosphere(const ScreenLayout& lay, uint32_t target_w, uint32_t target_h);
    bool back_layer_extended() const;
    void ensure_targets(uint32_t w, uint32_t h);
    void upload_slot(VkDescriptorSet set, uint32_t binding, VkSampler sampler, uint32_t slot, int w, int h,
                     const std::vector<uint8_t>& rgba,
                     bool replace = false);   // replace: the slot's previous picture is freed

    // record(), pass by pass. Frame: what they all need; ComposePush: the
    // push constants of compose.frag (both defined in r3d_renderer.cpp)
    struct Frame;
    struct ComposePush;
    void record_uploads(const Frame& f);
    void record_shadows(const Frame& f);
    void record_world(const Frame& f);
    void record_world_depth(const Frame& f, const R3DViewport& w, bool msaa, bool ao_on);
    void record_dlaa(const Frame& f);
    ComposePush compose_push(const Frame& f) const;
    static ComposePush reduced(const ComposePush& pc, const Image& to);
    void begin_quarter(VkCommandBuffer cmd, Image& to, VkAttachmentLoadOp load);
    void record_bloom(const Frame& f, const ComposePush& pc);
    void record_light_shafts(const Frame& f, const ComposePush& pc, bool shafts);
    void record_diffusion(const Frame& f, const ComposePush& pc);
    void record_exposure(const Frame& f, const ComposePush& pc);
    void record_compose(const Frame& f, const ComposePush& pc, VkImageView target);

    VkPresenter* vk_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    float supersample_ = 1.0f;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    float aniso_ = 0.0f;
    // Image arrays (init): in update-after-bind sets and/or shrunk to the GPU's
    // limits; the _few shaders take the sizes as specialisation constants 0 and 1
    uint32_t replace_slots_ = 4096, overlay_slots_ = 64;
    bool update_after_bind_ = false, few_images_ = false;
    const R3DScene* scene_ = nullptr;

    Image text_cells_;   // per 8x8 cell of the foreground: high-resolution text slot and colours
    Image sky_coords_;   // per back layer pixel: backdrop replacement picture and position (tilegen.h)
    Image texture_ram_, back_, front_, color_, depth_, color_ms_;   // color_ms_: MSAA target, resolved into color_
    Image motion_, dlaa_out_;   // DLAA: motion vectors (pixels, RG16F) and the anti-aliased image
    Image dlaa_alpha_;          // DLAA of the 3D image's alpha (in red), which the composition uses
    Image glow_, glow_ms_;      // bloom source written by the 3D pass (glow_ms_: MSAA, resolved into glow_)
    Image bloom_a_, bloom_b_, bloom_c_;   // bloom at a quarter of the size: reduced and blurred (wide in bloom_a_, tight in bloom_c_)
    float bloom_strength_ = 0;
    float saturation_ = 1, gamma_ = 1;
    Image depth_resolved_;   // MSAA: sample 0 of the world's depth, for the ambient occlusion
    Image ao_;               // raw ambient occlusion (R8)
    float ao_strength_ = 0;
    bool sun_ = false;
    bool relief_ = false;
    float relief_range_ = 120.0f;
    float sharpen_ = 0;
    bool night_sky_ = false;
    float trench_fade_ = 0, trench_far_ = 0;   // the Death Star trench's fade into the dark: amount, distance
    // Procedural space: the star field's axes in the previous and the latest game frame
    float star_prev_[9] = {}, star_now_[9] = {};
    uint64_t star_serial_ = ~0ull;
    bool star_valid_ = false;
    float night_level_ = 0;   // how much of the night sky shows, eased towards 0 or 1
    bool back_black_ = false;   // the back layer is solid black (no sky drawn)
    Image shadow_map_;                 // depth seen from the sun
    float shadow_strength_ = 0;
    float shadow_matrix_[16] = {};     // view space -> shadow map, this frame
    float shadow_up_[3] = {}, shadow_height_ = 0;   // world up in view space, camera height above the ground
    bool shadow_frame_ = false;        // the shadow map is drawn this frame
    VkPipeline shadow_ = VK_NULL_HANDLE;
    R3DRayTracer rt_;
    int ray_tracing_ = 2;
    bool rt_frame_ = false;            // something is traced this frame (the structures are built)
    bool rt_ao_ = false;               // the ambient occlusion is traced this frame
    const R3DViewport* rt_world_ = nullptr;   // the world viewport they are traced in
    uint32_t rt_generation_ = 0;       // R3DRayTracer::generation() the descriptors point to
    void record_ray_tracing(const Frame& f);
    // Measurement (SWT_GPUTIME, or LW_GPUTIME): GPU time of each pass of the
    // frame, averaged over 300 frames and printed. Timestamps in this order
    enum GpuMark : uint32_t {
        GPU_START, GPU_UPLOADS, GPU_RT, GPU_SHADOWS, GPU_AO_FROM, GPU_AO_TO, GPU_WORLD, GPU_DLAA, GPU_BLOOM,
        GPU_SHAFTS, GPU_DIFFUSION, GPU_EXPOSURE, GPU_COMPOSE, GPU_MARKS
    };
    VkQueryPool gpu_times_ = VK_NULL_HANDLE;
    float timestamp_period_ = 1;
    double gpu_sum_[GPU_MARKS] = {};   // ms of the pass that ends at each mark, summed
    int gpu_frames_ = 0;
    bool gpu_pending_ = false;
    bool gpu_ao_marked_ = false;       // this frame's ambient occlusion has its two marks
    void gpu_time_mark(VkCommandBuffer cmd, uint32_t index);
    void gpu_time_report(uint32_t out_w, uint32_t out_h);
    Image rays_a_, rays_b_;            // light shafts: mask, then smeared towards the sun (quarter size)
    VkDescriptorSet rays_set_ = VK_NULL_HANDLE;   // source: rays_a_
    Image diff_a_, diff_b_;            // diffusion (BLOOM): the picture behind the HUD, blurred, quarter size
    VkDescriptorSet diff_sets_[2] = {};   // sources: diff_a_, diff_b_
    VkPipeline rays_ = VK_NULL_HANDLE;
    float rays_strength_ = 0;
    float sun_uv_[2] = {}, sun_light_ = 0;   // the sun in the 3D image this frame, 0 = behind the camera
    bool soft_particles_ = false;
    bool hdr_ = false;
    Image exposure_;   // 1x1 R16F: average log brightness of the 3D image, eased from frame to frame
    VkPipeline compose_measure_ = VK_NULL_HANDLE;   // the composition in measure mode, blended into exposure_
    dlss::Dlaa dlaa_;
    float light_gain_ = 1.0f;   // the --light-gain multiplier
    bool motion_pass_ = false;   // pipelines and targets carry motion vectors (DLAA started)
    VkImageView composed_view_ = VK_NULL_HANDLE;   // 3D image the composition samples
    uint64_t scene_serial_ = 0, drawn_serial_ = 0, jitter_index_ = 0;
    float drawn_interp_ = 1.0f;   // interpolation of the previously displayed image
    Buffer texture_staging_, layer_staging_, vertices_;
    Buffer atmosphere_uniform_;   // atmosphere.glsl parameters, rewritten every frame
    float haze_strength_ = 0;
    float haze_color_[3] = {0.6f, 0.66f, 0.64f};   // horizon colour, eased from frame to frame
    std::vector<float> back_rows_ = std::vector<float>(384 * 4);   // per back layer row: colour sum, pixels
    float back_luma_ = 0;   // average brightness of the back layer (0..1, set_layers)
    bool swt_hud_ = false;   // Star Wars Trilogy's HUD layout (score/level top, dialogue/shield bottom)
public:
    void set_swt_hud(bool on) { swt_hud_ = on; }
private:
    bool layers_dirty_ = false;
    int tex_y0_ = 0, tex_y1_ = 0;

    std::vector<Image> replacements_;
    std::unordered_map<uint32_t, Image> overlay_images_;   // 2D replacement and backdrop pictures by slot
    VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE, replace_sampler_ = VK_NULL_HANDLE,
              overlay_sampler_ = VK_NULL_HANDLE;
    float overlay_brightness_ = 0;
    VkDescriptorSetLayout r3d_set_layout_ = VK_NULL_HANDLE, compose_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet r3d_set_ = VK_NULL_HANDLE, compose_set_ = VK_NULL_HANDLE;
    VkPipelineLayout r3d_layout_ = VK_NULL_HANDLE, compose_layout_ = VK_NULL_HANDLE;
    VkPipeline opaque_ = VK_NULL_HANDLE, translucent_ = VK_NULL_HANDLE, compose_ = VK_NULL_HANDLE;
    VkPipeline trans_depth_ = VK_NULL_HANDLE;   // nearest translucent surface, depth only (spec 7.2)
    VkDescriptorSetLayout bloom_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bloom_sets_[4] = {};   // sources: glow_, bloom_a_, bloom_b_, bloom_c_
    VkPipelineLayout bloom_layout_ = VK_NULL_HANDLE;
    VkPipeline bloom_ = VK_NULL_HANDLE;
    VkPipeline compose_glow_ = VK_NULL_HANDLE;   // the composition in glow mode: the sky's share of the bloom
    VkDescriptorSetLayout ao_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet ao_set_ = VK_NULL_HANDLE;   // world depth + raw occlusion
    VkPipelineLayout ao_layout_ = VK_NULL_HANDLE;
    VkPipeline ao_pass_ = VK_NULL_HANDLE, ao_apply_ = VK_NULL_HANDLE, ao_apply_glow_ = VK_NULL_HANDLE;
    VkFormat compose_format_ = VK_FORMAT_UNDEFINED;
    bool crosshair_visible_[2] = {};
    float max_aspect_ = 16.0f / 9.0f;
    bool smooth_2d_ = true;
    bool hud_edges_ = true, hud_band_ = true;
    float interp_ = 1.0f;
    // Where the world viewport's camera stood (world axes in view space, camera
    // position, direction of the sun) in the latest game frame and the one
    // before, for the atmosphere and the shadows between game frames
    struct WorldPose {
        float axes[9], camera[3], sun[3];
        bool valid = false;
    };
    WorldPose pose_now_, pose_prev_;
    uint64_t pose_serial_ = 0;
    bool pose_cut_ = false;   // the latest game frame cut to another shot
    WorldPose world_pose(const R3DViewport* w);
    // The sun drawn in the sky, carried along with the sky picture's scroll
    std::vector<uint32_t> sky_shift_;   // each row's scroll in the latest 2D frame (sky coordinates)
    uint64_t sky_shift_serial_ = 0, sky_sun_serial_ = 0;
    bool sky_sun_carried_ = false;
    float sky_sun_uv_[2] = {};     // where it is in the 3D image in the latest 2D frame
    float sky_sun_step_[2] = {};   // how far it moved there with the sky (for the interpolation)
    void step_sky_sun(const WorldPose& pose, const float view[4], float view_w, float out[3]);
    float back_shift_ = 0.0f;
    float crosshair_x_[2] = {}, crosshair_y_[2] = {};
};
