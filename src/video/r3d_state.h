// Real3D memory as seen by the renderer.
//
// The game thread writes culling/polygon RAM through the emulated bus. When it
// writes the command port (0x88000000, "flush") the current contents are
// published as an immutable FrameState: unchanged 64 KB pages are shared with
// the previous state, so publishing is cheap. The render thread grabs the
// latest FrameState without blocking the game.
//
// See docs/real3d_spec.md for all formats. All device words are the
// little-endian interpretation of the bytes the CPU wrote (spec 1.2).
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using R3DPage = std::array<uint8_t, 0x10000>;

// A scene of the PC's own (DOGFIGHT, src/dogfight): the game stands still and
// its last frame's memory is the set (textures, models, viewports); what is
// drawn is this list of the game's models, each where the native simulation
// puts it, seen by its camera. World axes: x right, y down, z forward (the
// game's ship models: x left, y up, z forward).
struct R3DNativeScene {
    float cam_pos[3] = {0, 0, 0};
    float cam_axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the camera's right, down and forward in the world
    struct Item {
        uint32_t models[4] = {};   // the parts of one object (same origin)
        int count = 0;
        float m[16] = {};          // model to world, column-major
        uint32_t key = 0;          // the same from frame to frame (interpolation)
    };
    std::vector<Item> items;
    // Things of the PC's own fixed to the camera, drawn over the scene (the
    // cockpit of src/dogfight/dogfight_cockpit.cpp). In view space: x right,
    // y up, -z ahead, in the world's units
    struct Vertex {
        float pos[3] = {};
        float nrm[3] = {};
        float uv[2] = {};               // v downwards (the image's rows)
        uint32_t color = 0xFFFFFFFFu;   // RGBA8, R in the low byte
    };
    struct Mesh {
        std::string texture;   // an image file ("": none, the vertices' colour alone)
        std::vector<Vertex> verts;   // three a triangle
    };
    std::shared_ptr<const std::vector<Mesh>> fixed;   // lit by the set's sun; the same object while it does not change
    std::vector<Vertex> lights;                       // glowing, untextured, over them (the cockpit's screen)
    float fov = 0;   // vertical field of view in degrees (0: the set's)
};

struct R3DFrameState {
    // Key = PPC address >> 16 (0x8C00.., 0x8E00.., 0x9800..)
    std::unordered_map<uint32_t, std::shared_ptr<const R3DPage>> pages;
    uint64_t frame = 0;
    // Seconds (performance counter): `time` is when the frame belongs on the
    // game's own timeline (the vblank it was made in, so frames are evenly
    // spaced for the interpolation), `flushed` when the game finished it
    double time = 0, flushed = 0;
    uint64_t vblank = 0;   // the game's frame counter when it was finished
    std::shared_ptr<const R3DNativeScene> native;   // drawn in place of the game's scene (null: the game's)
};

// The culling and polygon RAM of one FrameState as flat page tables, for the
// scene builder: a word read is an array index instead of a hash lookup
// (the builder reads tens of thousands of words a frame). Pages the state
// does not have read as zeros, as before.
class R3DStateReader {
public:
    void reset(const R3DFrameState& state) {
        for (auto& p : cull_lo_) p = zero_page();
        for (auto& p : cull_hi_) p = zero_page();
        for (auto& p : poly_) p = zero_page();
        for (const auto& [key, page] : state.pages) {
            if (key >= 0x8C00 && key < 0x8C00 + kLoPages) cull_lo_[key - 0x8C00] = page->data();
            else if (key >= 0x8E00 && key < 0x8E00 + kHiPages) cull_hi_[key - 0x8E00] = page->data();
            else if (key >= 0x9800 && key < 0x9800 + kPolyPages) poly_[key - 0x9800] = page->data();
        }
    }
    // Culling address space (word addresses): 0x000000-0x0FFFFF low, 0x800000-0x83FFFF high
    static bool cull_valid(uint32_t addr) {
        addr &= 0xFFFFFF;
        return addr < 0x100000 || (addr >= 0x800000 && addr < 0x840000);
    }
    uint32_t cull(uint32_t addr) const {
        addr &= 0xFFFFFF;
        if (addr >= 0x800000 && addr < 0x840000) return word(cull_hi_, addr & 0x3FFFF);
        if (addr < 0x100000) return word(cull_lo_, addr);
        return 0;
    }
    float cullf(uint32_t addr) const {
        uint32_t v = cull(addr);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    uint32_t poly(uint32_t addr) const { return word(poly_, addr & 0xFFFFF); }

private:
    // 0x8C000000 + 0x100000 words, 0x8E000000 + 0x40000 words, 0x98000000 + 0x100000 words
    static constexpr uint32_t kLoPages = 64, kHiPages = 16, kPolyPages = 64;
    static const uint8_t* zero_page() {
        static const R3DPage zeros{};
        return zeros.data();
    }
    // word = index in 32-bit words; a 64 KB page holds 0x4000 of them
    static uint32_t word(const uint8_t* const* pages, uint32_t word) {
        uint32_t v;
        memcpy(&v, pages[word >> 14] + (word & 0x3FFF) * 4, 4);
        return v;   // little-endian device word
    }
    const uint8_t* cull_lo_[kLoPages];
    const uint8_t* cull_hi_[kHiPages];
    const uint8_t* poly_[kPolyPages];
};

// 2048x2048 16-bit texture RAM (two 2048x1024 pages), filled from the
// texture FIFO at each flush (spec 6.1 - 6.4).
class R3DTextureRam {
public:
    R3DTextureRam() : texels_(2048 * 2048, 0), generation_(64 * 64, 0) {}

    // Parses the FIFO words collected since the last flush
    void upload_fifo(const std::vector<uint32_t>& fifo);
    // Texture port (0x90000000): texels read straight from VROM/polygon RAM
    void upload_direct(uint32_t header, const uint16_t* src, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        upload(header, src, count);
    }

    // Debugging: raw 16-bit texture RAM to a file
    void dump(const char* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(texels_.data(), 2, texels_.size(), f);
            fclose(f);
        }
    }
    // Debug snapshots (F9 / --replay): whole texture RAM
    void copy_all(std::vector<uint16_t>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out = texels_;
    }
    void load_all(const std::vector<uint16_t>& in) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (in.size() != texels_.size()) return;
        texels_ = in;
        for (auto& g : generation_) g++;
        dirty_y0_ = 0;
        dirty_y1_ = 2048;
    }
    // Copies rows touched since the last call (returns false if nothing changed)
    bool take_dirty(std::vector<uint16_t>& out, int& y0, int& y1);

    // Texture packs: copies a w x h texture at (x, y) with the same wrapping
    // as the shader (x over the whole RAM, y inside its 1024-row page) and
    // returns a value that changes whenever any texel of it is rewritten
    uint64_t read_texture(int x, int y, int w, int h, std::vector<uint16_t>& out);
    uint64_t texture_generation(int x, int y, int w, int h);

private:
    void upload(uint32_t header, const uint16_t* src, size_t count);
    void store_level(int x, int y, int w, int h, const uint16_t* src, size_t count, bool bit16, uint32_t header,
                     size_t& used);

    std::mutex mutex_;
    std::vector<uint16_t> texels_;
    std::vector<uint32_t> generation_;   // write counter per 32x32 block
    uint64_t generation_locked(int x, int y, int w, int h) const;
    int dirty_y0_ = 2048, dirty_y1_ = 0;
};
