// Walks the Real3D scene graph of one frame and produces GPU-ready geometry:
// vertices already transformed to view space (GL convention: camera looks
// down -Z, Y up), grouped by viewport and pass. See docs/real3d_spec.md
// sections 2-5.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "r3d_state.h"
#include "r3d_world.h"
#include "texture_pack.h"

struct R3DVertex {
    float pos[3];     // view space
    float nrm[3];     // view space (divided by model scale)
    float uv[2];      // normalised over the texture rectangle
    uint32_t color;   // RGBA8 (R in the low byte), alpha includes LOD fade
    uint32_t tex;     // x:11 | y:11 (page folded in) | wcode:3 | hcode:3 | format:4
    uint32_t flags;   // R3D_* below
    float shade;      // fixed shading value
    float fog;        // per-polygon fog intensity
    float spec;       // specular coefficient
    uint32_t replace; // texture pack slot (0 = original texture)
    float prev[3];    // view-space position in the previous frame (interpolation)
};
static_assert(sizeof(R3DVertex) == 72, "vertex layout is shared with the shaders");

enum : uint32_t {
    R3D_TEXTURED = 1 << 0,
    R3D_ALPHA_TEST = 1 << 1,
    R3D_MIRROR_U = 1 << 2,
    R3D_MIRROR_V = 1 << 3,
    R3D_SMOOTH_U = 1 << 4,
    R3D_SMOOTH_V = 1 << 5,
    R3D_LUMINOUS = 1 << 6,
    R3D_FIXED_SHADE = 1 << 7,
    R3D_SPECULAR = 1 << 8,
    R3D_TEX_ALPHA = 1 << 9,
    R3D_SHININESS_SHIFT = 10,   // 2 bits
    R3D_INVERT_RGB = 1 << 12,
    R3D_TRANSLATOR = 1 << 13,
    R3D_HUD_LEFT = 1 << 14,    // widescreen: HUD element moved to the left edge
    R3D_HUD_RIGHT = 1 << 15,   // ... or to the right edge
    R3D_WORLD = 1 << 16,       // part of the game world (the atmosphere applies)
    R3D_EXPLOSION = 1 << 17,   // Star Wars Trilogy, EXPLOSIONS: ENHANCED: drawn by r3d.frag's explosion() (r3d_explosions.cpp)
    R3D_ACTOR = 1 << 18,
    R3D_HUD_LOWER = 1 << 21,   // with R3D_HUD_RIGHT: also 24 rows lower (Star Wars Trilogy's shield gauge)
    R3D_NO_SHADOW = 1 << 20,   // receives no sun shadows (the planets and the Death Star: the ships before them are not near)
    R3D_DEATH_STAR = 1 << 19,  // Star Wars Trilogy: the Death Star's surface (a lit and a dark side, lights at night)       // a model of the world turned its own way: dinosaurs, people, vehicles (rim light)
    R3D_SAND = 1 << 22,        // Star Wars Trilogy: the desert of the duel with Boba Fett (grain and ripples)
    R3D_VADER = 1 << 23,       // Star Wars Trilogy: Vader in his duel (gloss on the black of the suit and helmet)
    R3D_FADE_IN_SHIFT = 24,    // 8 bits: scenery that just appeared in the distance, still
                               // dissolved into the backdrop (255 = fully; distance haze)
};

struct R3DViewport {
    int priority = 0;
    float x = 0, y = 0, w = 496, h = 384;   // native pixels, origin top-left
    float proj[16];                          // column-major, Vulkan clip space, reverse Z
    uint32_t node = 0;                       // culling RAM address of the viewport node (debugging)
    uint32_t build_index = 0;                // its list in R3DSceneBuilder::instances()
    uint32_t frustum[12] = {};               // its words 0x08-0x13: projection and frustum planes (debugging)
    float sun[3] = {0, 0, 0};
    float sun_intensity = 0, ambient = 0;
    float fog_color[3] = {0, 0, 0};
    float fog_density = 0, fog_start = 0, fog_ambient = 0;
    // Scroll fog (spec 7.4): the fog colour laid over the viewport before the
    // 3D image, so it veils the 2D layers behind; scroll attenuation and
    // spotlight-on-fog strength only decide whether it is drawn
    float scroll_fog = 0, scroll_attenuation = 0, fog_attenuation = 0;
    float spot_fog_color[3] = {0, 0, 0};
    // Spotlight (spec 7.1): extra light in an ellipse around a point of the
    // screen, like a torch (the night at the trailer). Centre and radii as
    // view-direction slopes (x / -z, y / -z) of this viewport's own projection,
    // so the lobe keeps its place in the scene when the picture is widened
    float spot_color[3] = {0, 0, 0};
    float spot_center[2] = {0, 0}, spot_radius[2] = {1, 1};
    float spot_start = 0, spot_extent = 0;
    uint32_t opaque_first = 0, opaque_count = 0;
    uint32_t trans_first = 0, trans_count = 0;
    // Atmosphere: the viewport showing the game world gets its orientation
    // and a camera position tracked from the scenery (track_world)
    bool world = false;
    float world_axes[9] = {};   // world x, y (up), z axes in view space
    float camera[3] = {};       // camera position in the tracked world frame
    float ground = 0;           // world height of the ground around the camera
    float scenery_far = 0;      // distance of the farthest scenery (most of it is nearer)
    float depth_fade = 0;       // Star Wars Trilogy, the Death Star's trench: the distance where the scenery has faded to black (0 = none)
    // Star Wars Trilogy (r3d_share_world): a world in space, picked without
    // scenery to track (no ground, no haze), and the viewports drawn with the
    // world's camera beside it (its vertices are R3D_WORLD too: they get the
    // sun shadows, cast them and are in the traced world)
    bool space = false;
    bool shares_world = false;
    bool stars = false;   // Star Wars Trilogy: the star field behind everything (r3d_share_world)
    bool hud_left = false;   // Star Wars Trilogy: a HUD emblem that goes to the left edge (r3d_anchor_swt_hud)
};

struct R3DScene {
    std::vector<R3DVertex> vertices;
    std::vector<R3DViewport> viewports;   // sorted by priority, then list order
    uint64_t frame = 0;
    size_t polygons = 0;
    size_t moving = 0;   // vertices whose position changed since the previous frame
    // Star Wars Trilogy: the star field's x, y, z axes in view space (the
    // procedural space behind it turns with them); valid when a viewport has stars
    float star_axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool sky_dome_hidden = false;   // Hoth's sky dome was taken out (r3d_hide_sky_dome)
    // Light given off by glowing things of the game (r3d_find_glows): the
    // lightsabres in the duels. A segment in view space, lighting what is near
    struct Glow {
        float a[3], b[3];      // its ends (equal for a point)
        float color[3];
        float radius;          // distance where its light has fallen to half
        float strength;
    };
    static constexpr int MAX_GLOWS = 8;
    std::vector<Glow> glows;
};

class R3DSceneBuilder {
public:
    void set_vrom(const uint8_t* data, size_t size) {
        vrom_ = data;
        vrom_words_ = size / 4;
        vrom_mask_ = vrom_words_ && !(vrom_words_ & (vrom_words_ - 1)) ? vrom_words_ - 1 : 0;
    }
    void build(const R3DFrameState& state, R3DScene& out);
    // The models drawn by the last build, per viewport in build order (R3DViewport::build_index)
    const std::vector<std::vector<R3DInstance>>& instances() const { return viewport_instances_; }
    // Removes the black cinematic bars the game draws over the scene
    void set_remove_letterbox(bool remove) { remove_letterbox_ = remove; }
    // Marks the 3D HUD elements at the sides of the screen (ammo) so the
    // renderer can move them to the edges of a widescreen window
    void set_hud_edges(bool edges) { hud_edges_ = edges; }
    // Star Wars Trilogy: the world's lighting for every viewport drawn with its camera (r3d_share_world)
    void set_share_world(bool on) { share_world_ = on; }
    // The last build's stages in ms (LW_FRAMELOG): the nodes and models, the
    // world tracking, the screen heuristics, the texture pack, the previous frame's pairing
    const float* stage_ms() const { return stage_ms_; }
    // The game's horizontal field of view was widened by this much for
    // full-width viewports (widescreen culling): taken back out of their projection
    void set_fov_widened(float f) { fov_widened_ = f; }
    void set_view_wide(float f) { view_wide_ = f; }   // the 3D view's width over 4:3 (also where the game's culling is not widened)
    // Replacement textures (nullptr = none)
    // Star Wars Trilogy, SKY: PROCEDURAL: Hoth's sky dome is not drawn (the procedural sky instead)
    void set_procedural_sky(bool on) { procedural_sky_ = on; }
    // --view-model (debugging aid): these models alone before the camera, in
    // place of the scene (yaw in degrees, NaN: turning; pitch in degrees)
    void set_view_models(std::vector<uint32_t> models, float yaw, float pitch) {
        view_models_ = std::move(models);
        view_yaw_ = yaw, view_pitch_ = pitch;
    }
    void set_texture_pack(TexturePack* pack, R3DTextureRam* ram) {
        pack_ = pack;
        texture_ram_ = ram;
    }

private:
    float stage_ms_[5] = {};
    struct Mat {
        float m[16];   // column-major
    };
    struct Attribs {
        int tex_x = 0, tex_y = 0, page = 0;
        float model_scale = 1, alpha = 1;
        bool no_cull = false;
    };
    struct PrevVertex {
        float pos[3] = {0, 0, 0};
        float nrm[3] = {0, 0, 0};
        float shade = 0;
        uint16_t u = 0, v = 0;
    };

    void viewport(uint32_t addr, int depth);
    void node(uint32_t addr, const Mat& m, Attribs a, int depth);
    void node_ptr(uint32_t ptr, const Mat& m, const Attribs& a, int depth);
    void pointer_list(uint32_t addr, const Mat& m, const Attribs& a, int depth);
    void model(uint32_t addr, const Mat& m, const Attribs& a);
    void view_models(const R3DViewport& v);
    void native_items();
    void native_fixed(R3DScene& out);
    uint32_t model_word(uint32_t addr) const;
    Mat matrix(uint32_t index) const;
    void link_previous(R3DScene& out);
    uint32_t instance_key(uint32_t model_addr);
    void apply_texture_pack(R3DScene& out);

    R3DStateReader rd_;   // the frame being built
    const uint8_t* vrom_ = nullptr;
    size_t vrom_words_ = 0, vrom_mask_ = 0;
    R3DScene* out_ = nullptr;
    std::vector<R3DVertex> opaque_, trans_;
    uint32_t matrix_base_ = 0, lod_base_ = 0;
    uint32_t color_table_ = 0x400;
    Mat coord_sys_{};
    PrevVertex prev_[4];
    std::unordered_set<uint32_t> visited_viewports_;
    bool remove_letterbox_ = true;
    // Interpolation: every vertex gets a key (scene path + model instance +
    // polygon + corner) to find where it was in the previous frame
    std::vector<uint64_t> keys_, opaque_keys_, trans_keys_;
    // Polygon key (corner bits clear) -> its first vertex: an open-addressing
    // table kept from frame to frame (from Lost World)
    struct KeyIndex {
        static constexpr uint32_t EMPTY = ~0u;
        std::vector<uint64_t> keys;
        std::vector<uint32_t> vertex;   // EMPTY: free slot
        uint32_t mask = 0;
        void reset(size_t count);   // empty, with room for `count` keys
        void insert(uint64_t key, uint32_t v);   // the first vertex with a key stays
        uint32_t find(uint64_t key) const;       // EMPTY if absent
    };
    KeyIndex index_, prev_index_;                        // this frame's, the previous frame's
    std::vector<uint64_t> prev_keys_;                    // the previous frame's vertex keys
    std::vector<std::array<float, 3>> prev_positions_;   // the previous frame's vertices
    struct PrevInstance {
        uint32_t key, path, start;
        float origin[3];
    };
    std::vector<PrevInstance> prev_instances_;   // for pairing identical copies by place
    void pair_copies();
    // pair_copies' and link_previous' tables, kept from frame to frame (cleared, their buckets stay)
    std::unordered_map<uint32_t, std::vector<R3DInstance*>> copies_now_;
    std::unordered_map<uint32_t, std::vector<const PrevInstance*>> copies_before_;
    std::unordered_map<uint32_t, uint32_t> rekey_;
    std::unordered_map<uint32_t, std::array<float, 9>> prev_axes_, axes_;   // model instance -> its axes last frame, this frame
    std::unordered_set<uint32_t> spinning_;   // instances that turned a lot since the previous frame
    std::unordered_map<uint32_t, uint32_t> instance_count_;
    uint32_t node_key_ = 0, viewport_key_ = 0;
    std::vector<uint32_t> node_stack_;   // the nodes above the one being walked (model catalogue, r3d_catalog.h)
    int viewport_priority_ = 0;
    std::vector<uint32_t> view_models_;   // --view-model
    float view_yaw_ = 0, view_pitch_ = 0;
    bool view_drawn_ = false;
    const R3DNativeScene* native_ = nullptr;   // the frame being built is the PC's own scene (R3DFrameState::native)
    uint32_t native_viewport_ = 0;             // the viewport node its objects were drawn in
    std::shared_ptr<const std::vector<R3DNativeScene::Mesh>> fixed_src_;   // native_fixed: the meshes fixed_ was made from
    std::vector<R3DVertex> fixed_;                                         // ... as vertices, kept from frame to frame
    bool hud_edges_ = true;
    bool share_world_ = false;
    bool procedural_sky_ = false;
    float fov_widened_ = 1.0f;
    float view_wide_ = 1.0f;
    TexturePack* pack_ = nullptr;
    R3DTextureRam* texture_ram_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> frame_slots_;   // texture word -> slot, this frame

    // Models drawn in each viewport, for the interpolation (copies of a model
    // are paired by place) and the world tracking
    std::vector<R3DInstance> instances_;                         // viewport being built
    std::vector<std::vector<R3DInstance>> viewport_instances_;   // per viewport, in build order
    R3DWorldTracker world_;
};
