// Star Wars Trilogy, EXPLOSIONS: ENHANCED (not in the original): a low boom
// under the game's own explosion sound, so it is felt more. Synthesized (a
// falling sub tone, a burst of filtered noise and a rumbling tail), mixed
// into the sound board's four channels. Portable C++.
#pragma once

// Any thread: a new explosion, strength 0..1 (how big it is on the screen)
void explosion_boom(float strength);
// Sound board thread: adds the booms playing to `count` frames of
// `channels` floats (full scale 1), at 44.1 kHz; `gain` 1 = as loud as the
// game's own loudest sounds at the default volume
void explosion_boom_mix(float* frames, int count, int channels, float gain, float rear_gain);
