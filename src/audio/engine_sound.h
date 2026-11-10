// DOGFIGHT (not in the original): the sound of the player's engines, made
// here (the game has none for a ship one flies): a low drone and a whine that
// rise with the speed, a roar while the engines push and a falling breath
// while they hold back. Portable C++.
#pragma once

// Game thread, each step of the flight: the speed between the slowest and
// the fastest, 0..1, and how hard the engines push towards the speed asked
// for, -1 (braking) .. 1 (full thrust)
void engine_sound_set(float speed, float push);
// Game thread: the flight is over (it fades out)
void engine_sound_off();
// Sound board thread: added to `frames` frames of `channels` floats (front
// left, right, rear left, right; full scale 1), `gain` its level
void engine_sound_mix(float* out, int frames, int channels, float gain);
