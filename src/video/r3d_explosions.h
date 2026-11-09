// Star Wars Trilogy, EXPLOSIONS: ENHANCED (not in the original). The game's
// explosion is a flipbook of 48 frames of 64x64 (texture RAM y 576..767,
// x 0..1023) drawn on a few see-through luminous polygons facing the camera.
// With the option on, that sprite is replaced by a procedural fireball
// (r3d.frag, EXPLOSION) of the same place, size and timing, and each new
// explosion throws sparks, burning debris with smoke trails and tumbling
// pieces of hull. Only in Yavin for now.
#pragma once
#include <vector>

#include "r3d_scene.h"
#include "r3d_world.h"

// After R3DSceneBuilder::link_previous (vertices it adds have no keys: they
// are added past what the next frame pairs with)
void r3d_swt_explosions(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances);
void r3d_set_swt_explosions(bool enhanced);
