// macOS: what the platform files share beyond the Win32 names of windows.h.
#pragma once
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#include <string>

SDL_Window* mac_window();                // nullptr until VkPresenter::init
SDL_Scancode mac_scancode(int vk);       // SDL_SCANCODE_UNKNOWN: not one of the game's keys
std::string mac_exe_path();              // this program's file, resolved
void mac_enter_game_folder();            // into the folder the game's relative paths work from
bool mac_https_get(const wchar_t* host, const wchar_t* path, std::string& body);   // at most 256 KB
void mac_set_cursor(bool visible);       // the arrow or nothing; from the main thread
