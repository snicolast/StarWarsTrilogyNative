// Texture packs: replacement images for the game's textures.
//
// Every texture a polygon uses is identified by a hash of its decoded RGBA
// contents, so the same texture is recognised wherever and whenever the game
// uploads it. With dumping enabled each new texture is written once as
// out/textures/<hash>.png; a file with the same name in textures/ replaces
// it, at any resolution and with its own alpha channel.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "image_io.h"
#include "r3d_state.h"

class TexturePack {
public:
    static constexpr uint32_t MAX_SLOTS = 4096;   // replacement images on the GPU (slot 0 = none)

    // dir: folder with replacement PNGs; dump_dir: where new textures are
    // written (empty = no dumping)
    void init(const std::string& dir, const std::string& dump_dir);
    bool active() const { return !available_.empty() || !dump_dir_.empty(); }

    // Replacement slot for a polygon texture word (R3DVertex::tex), 0 = none
    uint32_t lookup(uint32_t tex, R3DTextureRam& ram);

    // An image of the PC's own (not a replacement: DOGFIGHT's cockpit) in a
    // slot of its own, 0 if it cannot be read or no slot is left
    uint32_t add_file(const std::string& path);

    // Replacements loaded since the last call, to be uploaded by the renderer
    std::vector<RgbaImage> take_new_images();

    size_t replaced_count() const { return slots_.size(); }
    // Fewer slots than MAX_SLOTS, when the GPU cannot bind them all (R3DRenderer::replacement_slots)
    void set_slot_limit(uint32_t slots) { slot_limit_ = std::min(slots, MAX_SLOTS); }

private:
    struct Entry {
        uint64_t generation;
        uint32_t slot;
    };
    uint32_t slot_limit_ = MAX_SLOTS;
    uint32_t resolve(uint32_t tex, R3DTextureRam& ram, uint64_t& generation);

    std::string dir_, dump_dir_;
    std::unordered_map<uint32_t, Entry> cache_;            // texture word -> slot
    std::unordered_set<uint64_t> available_;               // hashes with a replacement file
    std::unordered_map<uint64_t, uint32_t> slots_;         // loaded hash -> slot
    std::unordered_set<uint64_t> dumped_;
    std::unordered_map<std::string, uint32_t> files_;      // add_file: path -> slot
    std::vector<RgbaImage> pending_;
    std::vector<uint16_t> texels_;
    std::vector<uint8_t> rgba_;
};
