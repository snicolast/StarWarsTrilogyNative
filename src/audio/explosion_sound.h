// Star Wars Trilogy, EXPLOSIONS: ENHANCED (not in the original): the game's
// own explosion sound, louder, with more low end and panned to where the
// explosion is on the screen. The game plays it quietly (DISDL 3, and TL up
// to 47 when far) and centred, under its own gunfire. Scsp recognises its
// three variants by their first bytes at key-on (the 68000 streams every
// sound through 8 KB buffers, so the address says nothing). Portable C++.
#pragma once
#include <cstdint>

// Renderer thread, each frame: whether to enhance (option on and in a stage
// where the explosions are replaced)
void explosion_sound_enable(bool on);
bool explosion_sound_enabled();
// Renderer thread: a new explosion, at `pan` -1 (left) .. 1 (right)
void explosion_sound_event(float pan);
// Sound board thread: the oldest event of the last 0.3 s not yet taken
bool explosion_sound_take(float& pan);
// Whether the sample at `base` (key-on) is one of the explosion sounds
bool explosion_sound_is_sample(const uint8_t* ram, uint32_t mask, uint32_t base);
