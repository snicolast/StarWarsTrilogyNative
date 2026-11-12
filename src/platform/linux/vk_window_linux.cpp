// Linux: VkPresenter's window (SDL3, X11 or Wayland) and the system's Vulkan
// loader (libvulkan.so.1, through SDL so both use the same one). The mouse and
// keys go to input_hook as the messages Windows would send, in window pixels.
// The Win32 names come from src/platform/mac (windows.h, platform_mac.h): they
// are POSIX and SDL3 there, and shared with macOS.
#include "vk_presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "platform_mac.h"

namespace {

SDL_Window* g_window = nullptr;
bool g_background = false;

const struct { int vk; SDL_Scancode sc; } KEYS[] = {
    {VK_BACK, SDL_SCANCODE_BACKSPACE}, {VK_TAB, SDL_SCANCODE_TAB}, {VK_RETURN, SDL_SCANCODE_RETURN},
    {VK_ESCAPE, SDL_SCANCODE_ESCAPE}, {VK_SPACE, SDL_SCANCODE_SPACE}, {VK_PRIOR, SDL_SCANCODE_PAGEUP},
    {VK_NEXT, SDL_SCANCODE_PAGEDOWN}, {VK_END, SDL_SCANCODE_END}, {VK_HOME, SDL_SCANCODE_HOME},
    {VK_LEFT, SDL_SCANCODE_LEFT}, {VK_UP, SDL_SCANCODE_UP}, {VK_RIGHT, SDL_SCANCODE_RIGHT},
    {VK_DOWN, SDL_SCANCODE_DOWN}, {VK_DELETE, SDL_SCANCODE_DELETE},
};

int vk_of(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
    if (sc == SDL_SCANCODE_KP_ENTER) return VK_RETURN;
    for (const auto& k : KEYS)
        if (k.sc == sc) return k.vk;
    return 0;
}

}  // namespace

SDL_Window* mac_window() { return g_window; }

SDL_Scancode mac_scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
    for (const auto& k : KEYS)
        if (k.vk == vk) return k.sc;
    return SDL_SCANCODE_UNKNOWN;
}

bool VkPresenter::create_window(const char* title, int width, int height) {
    g_background = getenv("SWT_BACKGROUND") != nullptr;
    if (g_background) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");   // the click that brings the window up counts
    SDL_SetHint(SDL_HINT_APP_ID, "StarWarsTrilogyNative");  // the Wayland app id and X11 class (desktop file, window rules)
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[window] SDL could not start: %s\n", SDL_GetError());
        return false;
    }
    printf("[window] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    // The size asked for, within the screen (in the desktop's scaled units under Wayland)
    SDL_Rect usable{};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    if (usable.w > 0 && usable.h > 0) {
        float k = std::min({1.0f, float(usable.w) * 0.9f / float(width), float(usable.h) * 0.9f / float(height)});
        width = int(float(width) * k), height = int(float(height) * k);
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (g_background) flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_NOT_FOCUSABLE;
    g_window = SDL_CreateWindow(title, width, height, flags);
    if (!g_window) {
        fprintf(stderr, "[window] could not open: %s\n", SDL_GetError());
        return false;
    }
    hwnd_ = reinterpret_cast<HWND>(g_window);
    if (!g_background) {
        SDL_RaiseWindow(g_window);
        SDL_HideCursor();
    }
    return true;
}

bool VkPresenter::load_vulkan() {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!gipa) {
        fprintf(stderr, "[vulkan] could not load the Vulkan loader (libvulkan.so.1): %s\n", SDL_GetError());
        return false;
    }
    volkInitializeCustom(gipa);
    return true;
}

std::vector<std::string> VkPresenter::surface_extensions() {
    std::vector<std::string> names;
    Uint32 count = 0;
    const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&count);
    for (Uint32 i = 0; ext && i < count; i++) names.push_back(ext[i]);
    return names;
}

bool VkPresenter::create_surface() {
    if (!SDL_Vulkan_CreateSurface(g_window, instance_, nullptr, &surface_)) {
        fprintf(stderr, "[vulkan] could not make the window's surface: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void VkPresenter::set_cursor_visible(bool visible) {
    if (g_background) return;
    if (visible == show_cursor_) return;
    show_cursor_ = visible;
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

void VkPresenter::toggle_fullscreen() {
    if (g_background) return;
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(g_window, fullscreen_);
    resized_ = true;
}

bool VkPresenter::pump() {
    SDL_PumpEvents();
    float density = SDL_GetWindowPixelDensity(g_window);
    auto at = [density](float x, float y) { return MAKELPARAM(int(x * density), int(y * density)); };
    auto hook = [this](UINT msg, WPARAM wp, LPARAM lp) { return input_hook && input_hook(msg, wp, lp); };
    SDL_Event e;
    // Only the window's events: the gamepads take theirs in Gamepads::poll
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_JOYSTICK_AXIS_MOTION - 1) > 0) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quit_ = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resized_ = true;
            break;
        case SDL_EVENT_KEY_DOWN: {
            if (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {   // Alt+Enter
                if (!e.key.repeat) toggle_fullscreen();
                break;
            }
            int vk = vk_of(e.key.scancode);
            if (!vk || hook(WM_KEYDOWN, WPARAM(vk), 0)) break;
            if (vk == VK_ESCAPE) quit_ = true;
            if (vk == VK_F11 && !e.key.repeat) toggle_fullscreen();
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            hook(WM_MOUSEMOVE, 0, at(e.motion.x, e.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT) hook(WM_LBUTTONDOWN, 0, at(e.button.x, e.button.y));
            else if (e.button.button == SDL_BUTTON_RIGHT) hook(WM_RBUTTONDOWN, 0, at(e.button.x, e.button.y));
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            // One message per notch: a touchpad sends fractions of them
            wheel_ += e.wheel.y;
            while (std::fabs(wheel_) >= 1.0f) {
                int dir = wheel_ > 0 ? 1 : -1;
                wheel_ -= float(dir);
                hook(WM_MOUSEWHEEL, WPARAM(uint32_t(WORD(short(dir * 120))) << 16), at(e.wheel.mouse_x, e.wheel.mouse_y));
            }
            break;
        }
        default:
            break;
        }
    }
    return !quit_;
}

void VkPresenter::destroy_window() {
    if (g_window) SDL_DestroyWindow(g_window);
    g_window = nullptr;
    hwnd_ = nullptr;
}
