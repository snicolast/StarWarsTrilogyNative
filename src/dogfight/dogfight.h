// DOGFIGHT (extra mode, a preview): the player flies an X-wing in first
// person. The game only sets the stage: it is run, unseen, to Yavin's first
// phase and stands still there, so its textures, models and viewports are
// loaded; from then on the scene is the PC's own (R3DNativeScene: the game's
// models where this simulation puts them, seen by its camera), a frame every
// 1/60 s published as the game's would be.
//
// So far: free flight (pitch, yaw, roll, throttle) among ships standing still.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "../video/r3d_state.h"

// 0 off, 1 asked for (the game is on its way to the stage, unseen), 2 flying
extern std::atomic<int> g_dogfight;

// The controls, each -1..1 (window thread; the game thread reads them)
struct DogfightControls {
    std::atomic<float> pitch{0};      // + nose up
    std::atomic<float> yaw{0};        // + to the right
    std::atomic<float> roll{0};       // + right wing down
    std::atomic<float> throttle{0};   // + faster, - slower (a rate), unless `absolute`
    std::atomic<bool> absolute{false};   // a throttle lever: `throttle` is its position, 0..1
};
extern DogfightControls g_dogfight_controls;

// The simulation itself (game thread): reset to the start of a flight, a step
// of dt seconds with the controls as they are, and the scene as it stands
void dogfight_reset();
void dogfight_step(float dt);
std::shared_ptr<const R3DNativeScene> dogfight_scene();
// Its numbers, for the screen and the tests: speed (units a second), throttle 0..1
float dogfight_speed();
float dogfight_throttle();
int dogfight_ships();
float dogfight_nearest();   // distance to the nearest ship (0: none)
// Its HUD into the game's 2D front layer (w x h BGRA, alpha in the top byte;
// any thread: it reads the numbers above)
void dogfight_draw_hud(uint32_t* front, int w, int h);

// The cockpit round the camera (dogfight_cockpit.cpp), if the user imported
// one (tools/import_cockpit.py: models/cockpit/cockpit.mesh and its texture;
// the game has none of its own). load: read once, false if there is none. add: it,
// its lamps and what its screen shows now, into a scene. Where the eye sits in it, in
// the model's units (centimetres) and degrees:
//   SWT_DOGFIGHT_COCKPIT=right,up,back,tilt,fov   (default 0,3,8,0,52; "off": no cockpit)
// It is dim inside (SWT_DOGFIGHT_LIGHT=floor,sun; default 0.10,0.30), lit by
// its own lamps, and sways with the ship (SWT_DOGFIGHT_SWAY=amount; 0: none)
#include <string>
#include <vector>
bool dogfight_cockpit_load(const char* dir);
// Its image files: read and sent to the GPU while the stage loads (a big one
// takes longer than a frame, and the flight would start with a hitch that
// the frame interpolation takes seconds to settle from)
std::vector<std::string> dogfight_cockpit_textures();
bool dogfight_cockpit_on();
// time: seconds flown (its lamps blink). sway: how the cockpit is thrown about
// now, a turn about the eye as a rotation vector (radians) and a shift
// (units), in the view's axes (x right, y up, -z ahead); null: none
void dogfight_cockpit_add(R3DNativeScene& scene, double time, const float* sway);

// The mode's loop (runtime.cpp, game thread). begin: the stage is loaded
// (called from the unseen run), the first native frame is published. run:
// flies until the mode is left (g_dogfight no longer 2) or the window closes;
// the game stands still meanwhile
void dogfight_begin();
void dogfight_run();
