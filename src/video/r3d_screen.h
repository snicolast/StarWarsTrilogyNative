// Screen-space heuristics applied to a built scene: geometry the game draws
// over the 3D image that a widescreen window treats differently.
#pragma once
#include "r3d_scene.h"

// Collapses the black cinematic bars the game draws over the scene
void r3d_strip_letterbox(R3DScene& out);
// Marks the 3D HUD elements at the sides of the screen (ammo) so the
// renderer can move them to the edges of a widescreen window
void r3d_anchor_hud(R3DScene& out);
// Star Wars Trilogy draws one world through several viewports of the same
// priority and projection (Yavin: the planets and capital ships in one, the
// fighters in another; Endor: the forest in one, the speeder bikes in
// another). The world's lighting (sun shadows, ray tracing, ambient
// occlusion) goes to all of them; in space, where there is no scenery to
// track, the largest group of ships is the world
void r3d_share_world(R3DScene& out);
// Star Wars Trilogy: glowing things as lights (R3DScene::glows): the
// lightsabres of the duels, laser bolts and explosions. One per model drawn
// (instances: R3DSceneBuilder::instances())
void r3d_find_glows(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances);
// Flat polygons filling a whole overlay viewport (the red flash of a hit)
// cover the 4:3 screen only: widened to the whole widescreen view
void r3d_widen_screen_fills(R3DScene& out, float wide);
// Star Wars Trilogy's shield gauge: a 3D ring near the camera in the top
// viewport, bottom right; tagged to move with the 2D shield to the right edge
void r3d_anchor_swt_hud(R3DScene& out);
void r3d_set_swt_duel(bool on);   // a lightsaber duel: its top HUD is 3D (r3d_anchor_swt_hud)
// The game's top state while playing (-1: not playing, or a replay): the
// Death Star's textures are only looked for in Yavin and the last space stage
void r3d_set_swt_stage(int top_state);
int r3d_swt_stage();
// Indoors, the thin black wire polygons (Hoth's base ceiling) are dropped
void r3d_hide_wires(R3DScene& out);
// Hoth's sky dome (textures 06C00200 and 06C00300 around the camera, in the
// world viewport): collapsed, for the procedural sky. True if there was one
bool r3d_hide_sky_dome(R3DScene& out);
