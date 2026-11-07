// macOS: one mouse (raw_mice.h). Player 2's gun is for a gamepad here; macOS
// merges the mice into one pointer. The pointer still stays in the window while
// it aims.
#include "raw_mice.h"

#include <SDL3/SDL.h>

#include "platform_mac.h"

namespace raw_mice {
namespace {
bool g_confine = false, g_grabbed = false;
}

void init(HWND) {}

void on_input(HRAWINPUT) {}

void confine(bool on) { g_confine = on; }

void update(HWND hwnd, bool, bool in_game) {
    bool grab = g_confine && in_game && GetForegroundWindow() == hwnd;
    if (grab != g_grabbed) SDL_SetWindowMouseGrab(mac_window(), grab);
    g_grabbed = grab;
}

bool two_mice() { return false; }

void reset() {
    if (g_grabbed) SDL_SetWindowMouseGrab(mac_window(), false);
    g_grabbed = false;
}

Gun gun(int, HWND) { return Gun{}; }

}  // namespace raw_mice
