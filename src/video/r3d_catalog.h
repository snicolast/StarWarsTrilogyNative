// Model catalogue (debugging aid, SWT_MODEL_CATALOG=1): every model the game
// draws while it runs, by address, written to out/model_catalog.txt. The list
// a native mode needs to draw the game's ships itself (DOGFIGHT): which models
// a stage has, how big they are, the texture offsets their node gives them
// and the nodes above them (a ship made of several models shares those).
// Addresses and numbers only: nothing of the ROM's data is written.
#pragma once
#include <cstdint>

struct R3DVertex;

bool r3d_catalog_on();

struct R3DCatalogModel {
    uint32_t model = 0;          // model address (below 0x100000: polygon RAM, loaded per stage; else VROM)
    uint32_t polygons = 0;
    int priority = 0;            // of the viewport it was drawn in
    uint32_t viewport = 0;       // that viewport's node
    int tex_x = 0, tex_y = 0, page = 0;   // texture offsets from the nodes above
    float model_scale = 1;
    float matrix_scale = 1;      // length of the model matrix's x axis
    float origin[3] = {};        // in view space
    const uint32_t* parents = nullptr;   // the nodes above it, nearest first
    int parent_count = 0;
};

// A model just drawn: its vertices in view space (opaque, then see-through)
void r3d_catalog_add(const R3DCatalogModel& m, const R3DVertex* opaque, uint32_t opaque_count, const R3DVertex* trans,
                     uint32_t trans_count, uint64_t frame);
// Once per frame built: writes the file every few seconds of game time
void r3d_catalog_frame(uint64_t frame);
void r3d_catalog_write();
