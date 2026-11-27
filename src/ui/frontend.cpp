#include "frontend.h"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <initializer_list>
#include <iterator>

#include <wincodec.h>

#include "../runtime/env_switches.h"
#include "frontend_style.h"
#ifndef _WIN32
#include "../video/image_io.h"
#endif

using namespace frontend_style;

namespace {

double now_seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

std::vector<char> read_file(const std::string& path) {
    std::vector<char> data;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        fclose(f);
    }
    return data;
}

}  // namespace

// ANTI-ALIASING: settings.aa's values (the order they are stored in, AA_COUNT of
// them), the order the row steps through them and what each does
#ifdef __APPLE__
constexpr int AA_ORDER[AA_COUNT] = {0, 1, 2};
const wchar_t* const AA_NAMES[AA_COUNT] = {L"MSAA 4X", L"MSAA 8X", L"OFF"};   // no DLAA without NVIDIA: 1 is MSAA 8x here
#else
constexpr int AA_ORDER[AA_COUNT] = {2, 7, 0, 1, 3, 4, 5, 6};   // OFF, FXAA, MSAA, DLAA, DLSS QUALITY .. ULTRA PERFORMANCE
const wchar_t* const AA_NAMES[AA_COUNT] = {L"MSAA 4X", L"DLAA", L"OFF", L"DLSS QUALITY", L"DLSS BALANCED",
                                           L"DLSS PERFORMANCE", L"DLSS ULTRA PERFORMANCE", L"FXAA"};
const wchar_t* const AA_HINT[AA_COUNT] = {
    L"Edge smoothing, works on any graphics card.  Applies on the next start",
    L"NVIDIA RTX: AI edge smoothing at full resolution, the sharpest.  Applies on the next start",
    L"No edge smoothing.  Applies on the next start",
    L"NVIDIA RTX: the 3D drawn at 67% and rebuilt sharp, lighter than DLAA.  Applies on the next start",
    L"NVIDIA RTX: the 3D drawn at 58% and rebuilt.  Applies on the next start",
    L"NVIDIA RTX: the 3D drawn at half size, for 4K.  Applies on the next start",
    L"NVIDIA RTX: the 3D drawn at a third, for 8K or slow cards.  Applies on the next start",
    L"Light edge smoothing over the finished picture: very cheap, for older graphics cards.  Applies on the next start",
};
#endif

// ---- Setup ----

bool Frontend::init(const std::string& font_dir) {
    static const char* const FILES[] = {"SairaExtraCondensed-Bold.ttf", "BarlowCondensed-Bold.ttf", "ShareTechMono-Regular.ttf",
                                        "BarlowCondensed-SemiBold.ttf", "BarlowCondensed-Medium.ttf", "Barlow-Regular.ttf"};
    bool ok = true;
    for (const char* name : FILES) {
        font_data_.push_back(read_file(font_dir + "/" + name));
        auto& data = font_data_.back();
        DWORD count = 0;
        HANDLE h = data.empty() ? nullptr : AddFontMemResourceEx(data.data(), DWORD(data.size()), nullptr, &count);
        if (h) {
            font_handles_.push_back(h);
        } else {
            fprintf(stderr, "[menu] could not load the font %s/%s\n", font_dir.c_str(), name);
            ok = false;
        }
    }
    dc_ = CreateCompatibleDC(nullptr);
    // The splash video, else the splash picture; logo, credit and background:
    // your own in third_party/menu/ first, else the ones in res/menu/
    for (const wchar_t* p : {L"third_party/menu/splash_snicolast.mp4", L"res/menu/splash_snicolast.mp4"})
        if (video_.open(p)) {
            printf("[menu] splash video %ls\n", p);
            break;
        }
    auto load = [&](const wchar_t* name, Picture& pic) {
        for (const wchar_t* dir : {L"third_party/menu/", L"res/menu/"}) {
            std::wstring path = std::wstring(dir) + name;
            if (!load_image(path, pic.px, pic.w, pic.h)) continue;
            printf("[menu] %ls (%dx%d)\n", path.c_str(), pic.w, pic.h);
            return true;
        }
        return false;
    };
    Picture splash;
    if (load(L"splash_snicolast.png", splash)) splash_.push_back(std::move(splash));
    load(L"logo.png", logo_);
    load(L"snicolast.png", credit_);
    if (!load(L"background.png", backdrop_)) load(L"background.jpg", backdrop_);
    return ok;
}

// Any picture format Windows can read (WIC), as BGRA with straight alpha
bool Frontend::load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w_out, int& h_out) {
#ifndef _WIN32
    std::vector<uint8_t> rgba;
    if (!png_load(std::string(path.begin(), path.end()), w_out, h_out, rgba)) {   // the project's paths are ASCII
        px.clear();
        w_out = h_out = 0;
        return false;
    }
    px.resize(rgba.size() / 4);
    for (size_t i = 0; i < px.size(); i++)
        px[i] = uint32_t(rgba[i * 4 + 2]) | uint32_t(rgba[i * 4 + 1]) << 8 | uint32_t(rgba[i * 4]) << 16 | uint32_t(rgba[i * 4 + 3]) << 24;
    return true;
#else
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0;
    if (ok) {
        px.resize(size_t(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data())));
    }
    for (IUnknown* p : std::initializer_list<IUnknown*>{conv, frame, dec, wic})
        if (p) p->Release();
    if (!ok) {
        px.clear();
        w_out = h_out = 0;
        return false;
    }
    w_out = int(w);
    h_out = int(h);
    return true;
#endif
}

void Frontend::shutdown() {
    video_.close();
    for (auto& [key, f] : fonts_) DeleteObject(f);
    fonts_.clear();
    for (HANDLE h : font_handles_) RemoveFontMemResourceEx(h);
    font_handles_.clear();
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
}

// ---- Menu contents and navigation ----

const wchar_t* const Frontend::TAB_NAMES[Frontend::TAB_COUNT] = {L"EXPERIENCE", L"DISPLAY", L"GRAPHICS", L"AUDIO", L"CONTROLS"};

// Every settings option: its section, within the section the order shown, and
// the heading of each group of rows (on its first one). This is the one place
// to put a new option (besides `all` in items()); an option missing here is
// not shown. A section whose rows do not fit the page scrolls.
const Frontend::OptionTab Frontend::OPTION_TABS[] = {
    {OPT_EXPERIENCE, EXPERIENCE_TAB, L"EDITION"}, {OPT_ARCADE_ASPECT, EXPERIENCE_TAB},
    {OPT_TELEMETRY, EXPERIENCE_TAB, L"EXPERIMENTAL"}, {OPT_TELEMETRY_FORMAT, EXPERIENCE_TAB},
    {OPT_DISPLAY, DISPLAY_TAB, L"SCREEN"}, {OPT_RESOLUTION, DISPLAY_TAB}, {OPT_ASPECT, DISPLAY_TAB}, {OPT_SCALE, DISPLAY_TAB}, {OPT_MOTION, DISPLAY_TAB},
    {OPT_VSYNC, DISPLAY_TAB}, {OPT_MAXFPS, DISPLAY_TAB}, {OPT_BGFPS, DISPLAY_TAB}, {OPT_AA, DISPLAY_TAB},
    {OPT_SAT, DISPLAY_TAB, L"PICTURE"}, {OPT_GAMMA, DISPLAY_TAB}, {OPT_SHARPEN, DISPLAY_TAB},
    {OPT_SUN, GRAPHICS_TAB, L"LIGHTING"}, {OPT_SHADOWS, GRAPHICS_TAB}, {OPT_HDR, GRAPHICS_TAB}, {OPT_AO, GRAPHICS_TAB},
    {OPT_BLOOM, GRAPHICS_TAB}, {OPT_HAZE, GRAPHICS_TAB},
    {OPT_RT, GRAPHICS_TAB, L"RAY TRACING"},
    {OPT_EXPLOSIONS, GRAPHICS_TAB, L"EFFECTS"},
    {OPT_VOLUME, AUDIO_TAB, L"VOLUME"}, {OPT_MUSIC, AUDIO_TAB}, {OPT_MENU_MUSIC, AUDIO_TAB},
    {OPT_SPEAKERS, AUDIO_TAB, L"SOUND"}, {OPT_REAR, AUDIO_TAB}, {OPT_SAMPLES, AUDIO_TAB},
    {OPT_CONNECTED, CONTROLS_TAB, L"DEVICES"},
    {OPT_SIGHT, CONTROLS_TAB, L"SIGHT"}, {OPT_POINTER, CONTROLS_TAB},
    {OPT_PAD_AIM, CONTROLS_TAB, L"GAMEPAD"}, {OPT_PAD_SPEED, CONTROLS_TAB}, {OPT_PAD_INVERT, CONTROLS_TAB}, {OPT_ASSIST, CONTROLS_TAB},
    {OPT_GYRO, CONTROLS_TAB, L"GYRO"}, {OPT_GYRO_SPEED, CONTROLS_TAB},
    {OPT_VIBRATION, CONTROLS_TAB, L"RUMBLE"}, {OPT_TRIGGERS, CONTROLS_TAB},
    {OPT_DUEL_INVERT, CONTROLS_TAB, L"DUELS"},
};

// Under each section's name in the column at the left
static const wchar_t* const TAB_ABOUT[] = {L"PC edition or the arcade as it was", L"Screen, smoothing, picture", L"Lighting, ray tracing, effects",
                                           L"Volume, speakers, sound", L"Mouse, light gun, gamepad"};

const wchar_t* Frontend::group_of(Id id) {
    for (const OptionTab& o : OPTION_TABS)
        if (o.id == id) return o.group;
    return nullptr;
}

const wchar_t* const Frontend::RECORD_TAB_NAMES[Frontend::RECORD_TAB_COUNT] = {L"ACHIEVEMENTS", L"STATS"};
static const wchar_t* const RECORD_TAB_ABOUT[] = {L"Unlocked and still to get", L"Totals and best stages"};
static const wchar_t* const RECORD_GROUPS[] = {L"UNLOCKED", L"LOCKED", L"TOTALS", L"BEST STAGES"};

void Frontend::set_tab(int t) {
    tab_ = (t + tab_count()) % tab_count();
    scroll_ = 0;
    // the settings page and the records: their first row (the column of sections is not a row)
    selected_ = screen_ == Screen::Options || screen_ == Screen::Records ? 1 : std::min(selected_, int(items().size()) - 1);
    hovered_ = -1;
    dirty_ = true;
}

// DEBUG: the stages in the game's own order (its state 2, 4, ..., 12; runtime.cpp)
namespace {
struct DebugStage {
    const wchar_t* name;
    const wchar_t* hint;
    int phases;
    const wchar_t* phase[3];
};
const DebugStage DEBUG_STAGES[] = {
    {L"YAVIN", L"Episode IV: the space battle and the Death Star", 3, {L"PHASE 1", L"PHASE 2", L"PHASE 3"}},
    {L"HOTH", L"Episode V: the snowspeeders and Echo Base", 3, {L"SNOWSPEEDERS", L"ECHO BASE TUNNELS", L"HANGAR"}},
    {L"ENDOR", L"Episode VI: the forest moon", 3, {L"PHASE 1", L"PHASE 2", L"PHASE 3"}},
    {L"SPACE STAGE 2", L"A later space stage (state 8)", 3, {L"PHASE 1", L"PHASE 2", L"PHASE 3"}},
    {L"DUEL STAGE 1", L"A later stage (state 10)", 1, {L"START"}},
    {L"DUEL STAGE 2", L"A later stage (state 12)", 1, {L"START"}},
};
constexpr int DEBUG_STAGE_COUNT = int(sizeof(DEBUG_STAGES) / sizeof(DEBUG_STAGES[0]));
}  // namespace

std::vector<Frontend::Item> Frontend::items() const {
    auto on_off = [](bool v) { return std::wstring(v ? L"ON" : L"OFF"); };
    static const wchar_t* const BLOOM_LEVELS[4] = {L"OFF", L"LOW", L"MEDIUM", L"HIGH"};
    static const wchar_t* const INVERT_TEXT[4] = {L"OFF", L"HORIZONTAL", L"VERTICAL", L"BOTH"};
    static const wchar_t* const SPEAKERS[3] = {L"STEREO", L"SURROUND 4.0", L"HEADPHONES"};
    static const wchar_t* const SPEAKER_HINTS[3] = {
        L"Front and rear sound mixed on two speakers",
        L"Rear sound on your rear speakers, as on the cabinet (4.0, 5.1 or 7.1)",
        L"Two ears: gentle crossfeed, and the rear sound placed behind you"};
    switch (screen_) {
    case Screen::Main: {
        std::vector<Item> list = {{PLAY, L"PLAY", L"", L"The original arcade game, or new ways to fly"},
                                  {OPTIONS, L"OPTIONS", L"", L"Display, sound and controls"},
                                  {RECORDS, L"RECORDS", L"", L"Achievements and statistics"},
                                  {QUIT, L"QUIT", L"", L"Close the game"}};
        // DEBUG is for testing only: shown while the port is being tested;
        // for the release kShowDebugMenu goes false (SWT_DEBUG_MENU still shows it)
        constexpr bool kShowDebugMenu = false;
        static const bool debug_menu = kShowDebugMenu || swt_env_on("SWT_DEBUG_MENU");
        if (debug_menu) list.insert(list.begin() + 1, {DEBUG, L"DEBUG", L"", L"Straight to any phase of any stage (for testing)"});
        return list;
    }
    case Screen::Play:
        return {{ARCADE, L"ARCADE CLASSIC", L"",
                 pad_prompts ? L"The original arcade game.  Insert a coin with VIEW and press MENU to start"
                             : L"The original arcade game.  Insert a coin with 5 and press 1, or BACK and START on a gamepad"},
                {DOGFIGHT, L"DOGFIGHT", L"COMING SOON", L"An X-wing against TIE fighters: random encounters and objectives", false},
                {BACK, L"BACK", L"", L"Return to the main menu"}};
    case Screen::Pause:
        if (arcade)   // ARCADE ORIGINAL: no main menu to go back to; its own entries last, as a group (draw_side_menu)
            return {{RESUME, L"RESUME", L"", L"Back to the game"},
                    {SETTINGS, L"SETTINGS", L"", L"Display, sound, controls and the experience"},
                    {PAUSE_QUIT, L"QUIT GAME", L"", L"Close the game"},
                    {PAUSE_RESTORE, L"RESTORE ORIGINAL", L"", L"The arcade's original settings (not the screen or the controls)"},
                    {PC_EDITION, L"BACK TO PC EDITION", L"", L"The PC menu comes back on the next start"}};
        return {{RESUME, L"RESUME", L"", L"Back to the game"},
                {SETTINGS, L"SETTINGS", L"", L"Display, sound and controls"},
                {QUIT_TO_MENU, L"QUIT TO MAIN MENU", L"", L"Leave this game and go back to the main menu"}};
    case Screen::Debug: {
        std::vector<Item> list;
        if (debug_stage_ < 0) {
            for (int i = 0; i < DEBUG_STAGE_COUNT; i++)
                list.push_back({DEBUG_STAGE, DEBUG_STAGES[i].name, L"", DEBUG_STAGES[i].hint, true, i});
            list.push_back({BACK, L"BACK", L"", L"Return to the main menu"});
        } else {
            const DebugStage& d = DEBUG_STAGES[debug_stage_];
            for (int i = 0; i < d.phases; i++)
                list.push_back({DEBUG_PHASE, d.phase[i], L"", std::wstring(d.name) + L".  The game plays up to here by itself, unseen", true, i});
            list.push_back({BACK, L"BACK", L"", L"Return to the stages"});
        }
        return list;
    }
    case Screen::Options: {
        std::wstring aa_hint = settings.aa != aa_at_launch
                                   ? L"Edge smoothing.  Applies after a restart (offered when you leave the settings)"
                                   :
#ifdef __APPLE__
                                     L"Edge smoothing (MSAA 4x, 8x or off).  Applies on the next start";
#else
                                     AA_HINT[std::clamp(settings.aa, 0, AA_COUNT - 1)];
#endif
        wchar_t gamma_text[16];
        swprintf(gamma_text, 16, L"%.2f", settings.gamma / 100.0);
        static const wchar_t* const RT_MODES[3] = {L"OFF", L"SHADOWS", L"SHADOWS + AO"};
        static const wchar_t* const ASPECT_NAMES[6] = {L"4:3 (ORIGINAL)", L"16:9", L"21:9", L"32:9", L"48:9", L"AUTO"};
        static const wchar_t* const ASPECT_HINTS[6] = {
            L"The arcade's own 4:3 picture, with bars at the sides",
            L"Widest 3D view 16:9.  Narrower windows show less, never stretched",
            L"Widest 3D view 21:9: fills ultrawide monitors (2560x1080, 3440x1440)",
            L"Widest 3D view 32:9: fills super ultrawide monitors (3840x1080, 5120x1440)",
            L"Widest 3D view 48:9: fills three monitors side by side (5760x1080, 7680x1440)",
            L"The 3D view fills the window, whatever its shape (up to 48:9)"};
        static const wchar_t* const RT_HINTS[3] = {
            L"Shadows from a shadow map and occlusion from the depth, as without ray tracing",
            L"Sun shadows traced through the world: every ship, walker and tree, soft with distance",
            L"Also the ambient occlusion: corners and the ground under things, even off screen"};
        std::wstring connected = L"NONE";   // the pads' names, as short as the value column takes
        if (!gamepads.empty()) {
            connected.clear();
            for (wchar_t c : gamepads) connected += wchar_t(towupper(c));
            if (connected.size() > 24) connected = connected.substr(0, 23) + L"\u2026";
        }
        static const wchar_t* const MODES[3] = {L"WINDOW", L"BORDERLESS", L"FULLSCREEN"};
        static const wchar_t* const MODE_HINTS[3] = {
            L"A window you can move and resize",
            L"Full screen at the desktop's resolution, the monitor left as it is: quick to switch to other windows",
            L"Exclusive full screen: the monitor switches to the RESOLUTION below while the game runs"};
        const int mode = std::clamp(settings.display_mode, 0, 2);
        auto size_text = [](int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); };
        // the chosen mode (the window without one: its inside); BORDERLESS the desktop's
        std::wstring res = mode == 1 ? size_text(desktop_w, desktop_h)
                           : settings.res_w > 0 ? size_text(settings.res_w, settings.res_h)
                           : mode == 0 ? size_text(client_w, client_h) : size_text(desktop_w, desktop_h);
        std::wstring res_hints[3] = {
            L"The size of the window's picture.  Enter: the list",
            L"BORDERLESS always uses the desktop's resolution",
            L"The mode the monitor switches to.  Enter: the list"};
        if (pad_prompts) res_hints[0] = L"The size of the window's picture.  A: the list", res_hints[2] = L"The mode the monitor switches to.  A: the list";
        if (mode == 0 && settings.res_w > 0 && (std::abs(client_w - settings.res_w) > 2 || std::abs(client_h - settings.res_h) > 2))
            res_hints[0] = L"The size of the window's picture: shown at " + size_text(client_w, client_h) +
                           L", the same shape, to fit on the desktop";
        const FrontendSettings& arc = arcade_side();
        std::vector<Item> all = {
                {OPT_EXPERIENCE, L"EXPERIENCE", settings.experience ? L"ARCADE ORIGINAL" : L"PC EDITION",
                 std::wstring(settings.experience
                                  ? L"The cabinet as it was: straight to the SEGA logo and the attract, coin and START to play, the original look and sound; Esc pauses"
                                  : L"The PC remaster: the main menu, records and achievements, widescreen, ray tracing and the new effects") +
                     (settings.experience != (arcade ? 1 : 0) ? L".  Applies after a restart (offered when you leave the settings)" : L"")},
                {OPT_ARCADE_ASPECT, L"ASPECT", ASPECT_NAMES[std::clamp(arc.aspect, 0, ASPECT_COUNT - 1)],
                 L"ARCADE ORIGINAL: the cabinet's own 4:3 picture, or our widescreen (16:9, 21:9, 32:9, 48:9, AUTO) with the arcade look"},
                {OPT_DISPLAY, L"MODE", MODES[mode],
                 std::wstring(MODE_HINTS[mode]) + (pad_prompts ? L"" : L" (F11 or Alt+Enter: window / full screen)")},
                {OPT_RESOLUTION, L"RESOLUTION", res, res_hints[mode], mode != 1 && !resolutions.empty()},
                {OPT_VSYNC, L"VSYNC", settings.vsync == 2 ? L"ADAPTIVE" : settings.vsync ? L"ON" : L"OFF",
                 settings.vsync == 2 ? L"In step with the refreshes while the game keeps up with them; when a picture is late it goes out at once (less stutter, it may tear then)"
                 : settings.vsync    ? L"Pictures in step with the monitor's refreshes: no tearing"
                                     : L"Less latency; the picture may tear without G-SYNC or FreeSync"},
                {OPT_MAXFPS, L"MAX FPS",
                 settings.max_fps < 0    ? std::wstring(L"UNLIMITED")
                 : settings.max_fps == 0 ? L"MONITOR (" + std::to_wstring(refresh_hz) + L")"
                                         : std::to_wstring(settings.max_fps),
                 !settings.smooth ? std::wstring(L"With MOTION ORIGINAL the game's own 57.5 frames are shown: nothing to set")
                 : settings.vsync ? std::wstring(L"Pictures a second at most; with VSYNC in whole refreshes (60 on a 120 Hz monitor: every other one).  Lower: less work for the GPU")
                                  : std::wstring(L"Pictures a second at most (UNLIMITED: as many as the GPU can).  Any number can go in settings.ini, e.g. max_fps = 117 for G-SYNC"),
                 settings.smooth},
                {OPT_BGFPS, L"BACKGROUND FPS", on_off(settings.background_fps),
                 L"On: 30 pictures a second at most while another window has the focus"},
                {OPT_ASPECT, L"WIDESCREEN", ASPECT_NAMES[settings.aspect], ASPECT_HINTS[settings.aspect]},
                {OPT_MOTION, L"MOTION", settings.smooth ? L"SMOOTH" : L"ORIGINAL",
                 L"Smooth: the 3D is also drawn between the game's 57.5 frames, at your monitor's refresh rate"},
                {OPT_VOLUME, L"EFFECTS", std::to_wstring(settings.volume), L"Volume of the sound effects and voices"},
                {OPT_MUSIC, L"MUSIC", std::to_wstring(settings.music), L"Volume of the music"},
                {OPT_MENU_MUSIC, L"MENU MUSIC", on_off(settings.menu_music),
                 L"The game's Main Title in the menus, at the music volume (or your own third_party/menu/music.mp3)"},
                {OPT_SPEAKERS, L"SPEAKERS", SPEAKERS[settings.speakers], SPEAKER_HINTS[settings.speakers]},
                {OPT_REAR, L"REAR BOOST", settings.rear_boost ? L"+" + std::to_wstring(settings.rear_boost) + L" DB" : std::wstring(L"OFF"),
                 L"Louder rear speakers, as the cabinet's seat speakers: engines roaring past and explosions"},
                {OPT_SAMPLES, L"SAMPLE QUALITY", settings.cubic_samples ? L"SMOOTH" : L"ORIGINAL",
                 L"Smooth interpolation of the instrument and voice samples, or the original chip's"},
                {OPT_AA, L"ANTI-ALIASING", AA_NAMES[std::clamp(settings.aa, 0, AA_COUNT - 1)], aa_hint},
                {OPT_SCALE, L"RENDER SCALE", std::to_wstring(settings.render_scale) + L"%",
                 L"The 3D drawn at this share of the picture's size: above 100% sharper and slower, below faster and softer"},
                {OPT_HAZE, L"DISTANCE HAZE", on_off(settings.haze),
                 L"Far scenery on Hoth and Endor fades into the sky instead of appearing suddenly"},
                {OPT_BLOOM, L"BLOOM", BLOOM_LEVELS[settings.bloom], L"Soft glow around lasers, explosions, engines and lights"},
                {OPT_EXPLOSIONS, L"EXPLOSIONS", settings.explosions ? L"ENHANCED" : L"ARCADE",
                 settings.explosions ? L"New fireballs with sparks, burning debris and smoke trails (Yavin for now)"
                                     : L"The arcade's own explosions"},
                {OPT_AO, L"AMBIENT OCCLUSION", on_off(settings.ao),
                 L"Soft shadows in corners, trenches and on the ground around things"},
                {OPT_SUN, L"SUN", on_off(settings.sun), L"Stronger, warmer sunlight on Hoth and Endor"},
                {OPT_SHADOWS, L"SUN SHADOWS", on_off(settings.shadows),
                 L"Walkers, speeder bikes and trees cast shadows away from the sun"},
                {OPT_RT, L"RAY TRACING", !rt_available ? L"N/A" : RT_MODES[settings.ray_tracing],
                 !rt_available ? L"Needs a graphics card with ray tracing (NVIDIA RTX, AMD RX 6000 or newer)"
                               : RT_HINTS[settings.ray_tracing],
                 rt_available},
                {OPT_HDR, L"HDR LIGHTING", on_off(settings.hdr),
                 L"Bright light keeps its detail instead of clipping to white; exposure adapts gently"},
                {OPT_SAT, L"COLOR SATURATION", std::to_wstring(settings.saturation) + L"%",
                 L"How vivid the colours are (100% = original, 80% by default)"},
                {OPT_GAMMA, L"GAMMA", gamma_text, L"Brightness of the mid-tones (1.00 = original)"},
                {OPT_SHARPEN, L"SHARPEN", on_off(settings.sharpen),
                 L"Brings back texture detail after the edge smoothing, without halos"},
                {OPT_SIGHT, L"SIGHT SIZE", std::to_wstring(settings.sight_size) + L"%",
                 L"Size of the game's sight (100% = original)"},
                {OPT_POINTER, L"MOUSE / LIGHT GUN", settings.pointer_sight ? L"POINT" : L"LEVER",
                 settings.pointer_sight ? L"The sight goes right where the mouse or a light gun (Sinden) points"
                                        : L"The mouse moves the flight lever, as on the cabinet"},
                {OPT_PAD_AIM, L"STICK AIM", settings.pad_lever ? L"LEVER" : L"SIGHT",
                 settings.pad_lever ? L"The stick is the flight lever, as on the cabinet: the sight goes back to the centre when you let go"
                                    : L"The sticks move the sight, which stays where you leave it"},
                {OPT_PAD_SPEED, L"STICK SPEED", std::to_wstring(settings.pad_speed),
                 settings.pad_lever ? L"How fast the lever swings over when you move the stick"
                                    : L"How fast the gamepad's sticks move the sight"},
                {OPT_PAD_INVERT, L"INVERT STICK", INVERT_TEXT[settings.pad_invert],
                 L"Turn the sticks' horizontal and / or vertical axis round"},
                {OPT_DUEL_INVERT, L"INVERT IN DUELS", INVERT_TEXT[settings.duel_invert],
                 L"Turn the aim round in the lightsaber duels only (mouse, light gun and sticks), if they feel the wrong way round"},
                {OPT_GYRO, L"GYRO AIM", on_off(settings.gyro),
                 gyro_pad ? L"Point the gamepad to aim, together with the sticks"
                          : L"Point the gamepad to aim (DualSense, DualShock 4, Switch Pro).  None connected"},
                {OPT_GYRO_SPEED, L"GYRO SPEED", std::to_wstring(settings.gyro_speed),
                 L"How far the sight moves when you turn the gamepad"},
                {OPT_VIBRATION, L"VIBRATION", BLOOM_LEVELS[settings.vibration],
                 L"The gamepad rumbles when you fire and when you are hit"},
                {OPT_TRIGGERS, L"TRIGGER RUMBLE", settings.trigger_rumble ? std::to_wstring(settings.trigger_rumble) + L"%" : std::wstring(L"OFF"),
                 L"Xbox One / Series pads: the trigger kicks when you fire, the other when you reload (a share of VIBRATION)"},
                {OPT_ASSIST, L"AIM ASSIST", BLOOM_LEVELS[settings.aim_assist],
                 L"With a gamepad the sight slows down over the enemy ships and follows them a little"},
                {OPT_TELEMETRY, L"TELEMETRY", on_off(settings.telemetry),
                 L"Enable telemetry for use with motion simulators and haptics"},
                {OPT_TELEMETRY_FORMAT, L"TELEMETRY FORMAT", L"OGS-TS",
                 L"Open Game Standards Telemetry (your craft's motion, shield, score and hits) for rigs and haptics that read it, by UDP multicast on port 40123"},
                {OPT_CONNECTED, L"CONNECTED", connected,
                 gamepads.empty() ? L"No gamepad found.  Plug one in: it shows here as soon as the game sees it"
                                  : L"Found: " + gamepads,
                 false}};
        std::vector<Item> list = {{TABS, L"", L"", pad_prompts ? L"LB / RB change the section"
                                                               : L"Q / E or Tab change the section, or click one"}};
        for (const OptionTab& o : OPTION_TABS)   // the open section's rows, in OPTION_TABS' order
            for (const Item& it : all) {
                // ARCADE ORIGINAL's ASPECT under EXPERIENCE only when it is chosen; while it runs
                // DISPLAY's is the same setting, so only the one under EXPERIENCE
                if (it.id == OPT_ARCADE_ASPECT && !settings.experience) continue;
                if (it.id == OPT_ASPECT && arcade) continue;
                if (it.id == o.id && o.tab == tab_) list.push_back(it);
            }
        list.push_back(
            {BACK, L"BACK", L"", options_from_pause_ ? L"Return to the pause menu" : L"Return to the main menu"});
        return list;
    }
    case Screen::Records: {
        int got = 0;
        for (int64_t t : records.unlocked) got += t != 0;
        std::wstring summary = std::to_wstring(got) + L" of " + std::to_wstring(achievement_table().size()) +
                               L" achievements unlocked";
        std::vector<Item> list = {{TABS, L"", L"", summary}};
        for (const RecordRow& r : record_rows()) list.push_back({REC_ROW, r.label, r.value, r.detail, r.lit, r.group});
        list.push_back({BACK, L"BACK", L"", L"Return to the main menu"});
        return list;
    }
    default:
        return {};
    }
}

void Frontend::open_splash() {
    if (video_.is_open() && settings.splash) {
        open(Screen::Splash);
        video_on_ = true;
        splash_from_ = now_seconds();
        skip_from_ = -1;
        video_end_ = -1;
        video_level_ = 0;
        video_.set_volume(float(settings.volume) / 10.0f);
        video_.play();
        return;
    }
    video_.close();
    if (splash_.empty() || !settings.splash) {
        open(Screen::Main);
        return;
    }
    open(Screen::Splash);
    splash_on_ = 0;
    splash_from_ = now_seconds();
    skip_from_ = -1;
    skip_next_ = false;
}

void Frontend::open(Screen s) {
    if (screen_ == Screen::Splash && s != Screen::Splash) {   // not needed again
        splash_px_ = {}, splash_px_on_ = -1;
        video_.close();
        video_on_ = false;
        video_px_ = {};
        splash_.clear();
    }
    screen_ = s;
    bool page = s == Screen::Options || s == Screen::Records;
    if (page) tab_ = 0;   // the first section
    drop_open_ = restart_open_ = false;
    selected_ = page ? 1 : 0;   // the settings page and the records: their first row (the sections are not one)
    scroll_ = 0;
    hovered_ = -1;
    hit_.clear();
    dirty_ = true;
}

void Frontend::select(int index) {
    if (index != selected_) dirty_ = true;
    selected_ = index;
}

void Frontend::activate(const Item& it, int dir) {
    dirty_ = true;
    switch (it.id) {
    case PLAY: open(Screen::Play); break;
    case OPTIONS: options_from_pause_ = false; open(Screen::Options); break;
    case RECORDS: open(Screen::Records); break;
    case QUIT: push(Action::Quit); break;
    case QUIT_TO_MENU: push(Action::QuitToMenu); break;
    case ARCADE: push(Action::StartArcade); break;
    case DEBUG: debug_stage_ = -1; open(Screen::Debug); break;
    case DEBUG_STAGE: debug_stage_ = it.index; open(Screen::Debug); break;
    case DEBUG_PHASE: debug_choice = debug_stage_ * 4 + it.index; push(Action::StartDebug); break;
    case DOGFIGHT: break;   // not made yet (greyed out)
    case TABS: case REC_ROW: case OPT_CONNECTED: case OPT_TELEMETRY_FORMAT: break;   // nothing to change
    case BACK: back(); break;
    case RESUME: push(Action::Resume); break;
    case PAUSE_QUIT: push(Action::Quit); break;
    case PAUSE_RESTORE:   // RESTORE / CANCEL first (restart_choose)
        restore_ask_ = restart_open_ = true;
        restart_button_ = 0;
        break;
    case PC_EDITION:   // saved at once; RESTART NOW, or LATER (back to the pause)
        settings.experience = 0;
        push(Action::SettingsChanged);
        restart_note_ = L"PC EDITION: the main menu comes back on the next start";
        restart_open_ = true;
        restart_button_ = 0;
        break;
    case OPT_EXPERIENCE:   // (each experience keeps what was changed in it; RESTORE ORIGINAL: the arcade's base values)
        settings.experience ^= 1;
        push(Action::SettingsChanged);
        break;
    case OPT_ARCADE_ASPECT:
        arcade_side().aspect = (arcade_side().aspect + dir + ASPECT_COUNT) % ASPECT_COUNT;
        push(Action::SettingsChanged);
        break;
    case SETTINGS: options_from_pause_ = true; open(Screen::Options); break;
    case OPT_DISPLAY:   // applied at once (app.cpp)
        settings.display_mode = (settings.display_mode + dir + 3) % 3;
        push(Action::DisplayChanged);
        break;
    case OPT_RESOLUTION: {
        if (settings.display_mode == 1 || resolutions.empty()) break;
        int n = int(resolutions.size()), i = (resolution_index() + dir + n) % n;   // the next mode of the monitor's list
        settings.res_w = resolutions[size_t(i)].first;   // applied at once
        settings.res_h = resolutions[size_t(i)].second;
        push(Action::DisplayChanged);
        break;
    }
    case OPT_VSYNC: {   // ON, ADAPTIVE, OFF
        static const int ORDER[3] = {1, 2, 0};
        int i = settings.vsync == 2 ? 1 : settings.vsync ? 0 : 2;
        settings.vsync = ORDER[(i + dir + 3) % 3];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_BGFPS: settings.background_fps = !settings.background_fps; push(Action::SettingsChanged); break;
    case OPT_MAXFPS: {
        if (!settings.smooth) break;
        static const int STEPS[] = {0, 30, 60, 90, 120, 144, 165, 240, -1};   // MONITOR first, UNLIMITED last
        int n = int(std::size(STEPS)), i = -1;
        for (int k = 0; k < n; k++)
            if (STEPS[k] == settings.max_fps) i = k;
        if (i < 0)   // a number set in settings.ini: the step above or below it
            for (i = 1; i < n - 1 && STEPS[i] < settings.max_fps; i++) {}
        settings.max_fps = STEPS[(i + dir + n) % n];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_VOLUME:
        settings.volume = dir > 0 && settings.volume == 10 && wrap_ ? 0 : std::clamp(settings.volume + dir, 0, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_MUSIC:
        settings.music = dir > 0 && settings.music == 10 && wrap_ ? 0 : std::clamp(settings.music + dir, 0, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_ASPECT: settings.aspect = (settings.aspect + dir + ASPECT_COUNT) % ASPECT_COUNT; push(Action::SettingsChanged); break;
    case OPT_MENU_MUSIC: settings.menu_music = !settings.menu_music; push(Action::SettingsChanged); break;
    case OPT_SPEAKERS: settings.speakers = (settings.speakers + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_REAR: settings.rear_boost = (settings.rear_boost + 3 * dir + 15) % 15; push(Action::SettingsChanged); break;
    case OPT_SAMPLES: settings.cubic_samples = !settings.cubic_samples; push(Action::SettingsChanged); break;
    case OPT_AA: {   // in the order of AA_ORDER
        int at = 0;
        for (int i = 0; i < AA_COUNT; i++)
            if (AA_ORDER[i] == settings.aa) at = i;
        for (int n = 0; n < AA_COUNT; n++) {
            at = (at + dir + AA_COUNT) % AA_COUNT;
#ifndef __APPLE__   // DLAA and DLSS only on a card that runs them (dlss_available), FXAA only on one that does not
            bool nvidia_only = AA_ORDER[at] == 1 || (AA_ORDER[at] >= 3 && AA_ORDER[at] <= 6);
            if (dlss_available ? AA_ORDER[at] == 7 : nvidia_only) continue;
#endif
            break;
        }
        settings.aa = AA_ORDER[at];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_SCALE: {
        static const int STEPS[] = {50, 67, 75, 85, 100, 125, 150, 175, 200};
        int n = int(std::size(STEPS)), i = 4;
        for (int k = 0; k < n; k++)
            if (STEPS[k] <= settings.render_scale) i = k;
        settings.render_scale = STEPS[(i + dir + n) % n];
        push(Action::SettingsChanged);
        break;
    }
    case OPT_HAZE: settings.haze = !settings.haze; push(Action::SettingsChanged); break;
    case OPT_BLOOM: settings.bloom = (settings.bloom + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_AO: settings.ao = !settings.ao; push(Action::SettingsChanged); break;
    case OPT_SUN: settings.sun = !settings.sun; push(Action::SettingsChanged); break;
    case OPT_SHADOWS: settings.shadows = !settings.shadows; push(Action::SettingsChanged); break;
    case OPT_RT: settings.ray_tracing = (settings.ray_tracing + dir + 3) % 3; push(Action::SettingsChanged); break;
    case OPT_EXPLOSIONS: settings.explosions = !settings.explosions; push(Action::SettingsChanged); break;
    case OPT_HDR: settings.hdr = !settings.hdr; push(Action::SettingsChanged); break;
    case OPT_SHARPEN: settings.sharpen = !settings.sharpen; push(Action::SettingsChanged); break;
    case OPT_MOTION: settings.smooth = !settings.smooth; push(Action::SettingsChanged); break;
    case OPT_SIGHT:   // 50..100 % in steps of 10; a click past the top starts again from the bottom
        settings.sight_size = dir > 0 && settings.sight_size >= 100 && wrap_ ? 50 : std::clamp(settings.sight_size + 10 * dir, 50, 100);
        push(Action::SettingsChanged);
        break;
    case OPT_POINTER: settings.pointer_sight = !settings.pointer_sight; push(Action::SettingsChanged); break;
    case OPT_PAD_AIM: settings.pad_lever = !settings.pad_lever; push(Action::SettingsChanged); break;
    case OPT_PAD_INVERT: settings.pad_invert = (settings.pad_invert + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_DUEL_INVERT: settings.duel_invert = (settings.duel_invert + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_PAD_SPEED:
        settings.pad_speed = dir > 0 && settings.pad_speed == 10 && wrap_ ? 1 : std::clamp(settings.pad_speed + dir, 1, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_GYRO: settings.gyro = !settings.gyro; push(Action::SettingsChanged); break;
    case OPT_GYRO_SPEED:
        settings.gyro_speed = dir > 0 && settings.gyro_speed == 10 && wrap_ ? 1 : std::clamp(settings.gyro_speed + dir, 1, 10);
        push(Action::SettingsChanged);
        break;
    case OPT_VIBRATION: settings.vibration = (settings.vibration + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_TRIGGERS:   // OFF..100 % in steps of 10; a click past the top starts again from OFF
        settings.trigger_rumble = dir > 0 && settings.trigger_rumble >= 100 && wrap_
                                      ? 0 : std::clamp(settings.trigger_rumble + 10 * dir, 0, 100);
        push(Action::SettingsChanged);
        break;
    case OPT_ASSIST: settings.aim_assist = (settings.aim_assist + dir + 4) % 4; push(Action::SettingsChanged); break;
    case OPT_TELEMETRY: settings.telemetry = !settings.telemetry; push(Action::SettingsChanged); break;
    case OPT_SAT:   // 0..200 % in steps of 10; a click past the top starts again from the bottom
        settings.saturation = dir > 0 && settings.saturation >= 200 && wrap_
                                  ? 0 : std::clamp(settings.saturation + 10 * dir, 0, 200);
        push(Action::SettingsChanged);
        break;
    case OPT_GAMMA:   // 0.50..1.50 in steps of 0.05
        settings.gamma = dir > 0 && settings.gamma >= 150 && wrap_
                             ? 50 : std::clamp(settings.gamma + 5 * dir, 50, 150);
        push(Action::SettingsChanged);
        break;
    }
}

int Frontend::index_of(Id id) const {
    std::vector<Item> list = items();
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].id == id) return int(i);
    return 0;
}

void Frontend::back() {
    switch (screen_) {
    case Screen::Play: open(Screen::Main); break;
    case Screen::Options:
        if (!restart_open_ && !restart_list().empty()) {   // RESTART THE GAME? first (LATER: back as here)
            restart_open_ = true;
            restart_button_ = 0;
            dirty_ = true;
            return;
        }
        open(options_from_pause_ ? Screen::Pause : Screen::Main);
        selected_ = options_from_pause_ ? 1 : index_of(OPTIONS);   // where the options were opened from
        break;
    case Screen::Records: open(Screen::Main); selected_ = index_of(RECORDS); break;
    case Screen::Pause: push(Action::Resume); break;
    case Screen::Main: selected_ = index_of(QUIT); dirty_ = true; break;
    case Screen::Debug:
        if (debug_stage_ >= 0) {
            int from = debug_stage_;
            debug_stage_ = -1;
            open(Screen::Debug);
            selected_ = from;
        } else {
            open(Screen::Main);
            selected_ = index_of(DEBUG);
        }
        break;
    default: break;
    }
}

bool Frontend::key(int vk) {
    if (!active()) return false;
    if (screen_ == Screen::Splash) {   // these hurry it on, the rest do nothing
        if (vk == VK_RETURN || vk == VK_SPACE || vk == VK_ESCAPE || vk == VK_BACK) skip_splash();
        return true;
    }
    if (starting) {   // LOADING: nothing to choose, only Esc to give PLAY up
        if (vk == VK_ESCAPE || vk == VK_BACK) push(Action::CancelPlay);
        return true;
    }
    if (drop_open_) return drop_key(vk);
    if (restart_open_) {
        switch (vk) {
        case VK_LEFT: case 'A': case VK_RIGHT: case 'D': case VK_TAB: restart_button_ ^= 1; dirty_ = true; break;
        case VK_RETURN: case VK_SPACE: restart_choose(restart_button_); break;
        case VK_ESCAPE: case VK_BACK: restart_choose(1); break;   // LATER
        }
        return true;
    }
    auto list = items();
    int n = int(list.size());
    if (screen_ == Screen::Options && (vk == VK_RETURN || vk == VK_SPACE) && selected_ > 0 && selected_ < n &&
        list[selected_].id >= OPT_DISPLAY && list[selected_].enabled && open_drop(list[selected_].id))   // its list
        return true;
    bool option_row = selected_ < n && list[selected_].id >= OPT_DISPLAY;
    bool tabs_row = selected_ < n && list[selected_].id == TABS;
    if (screen_ == Screen::Records) switch (vk) {   // sections and rows as on the settings page; nothing to change
        case 'Q': case VK_PRIOR: case VK_LEFT: case 'A': set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: case VK_RIGHT: case 'D': set_tab(tab_ + 1); return true;
        case VK_TAB: set_tab(tab_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); return true;
        case VK_UP: case 'W': if (n > 1) select(selected_ <= 1 ? n - 1 : selected_ - 1); return true;
        case VK_DOWN: case 'S': if (n > 1) select(selected_ >= n - 1 ? 1 : selected_ + 1); return true;
        case VK_RETURN: case VK_SPACE: if (selected_ == n - 1) back(); return true;
        case VK_ESCAPE: case VK_BACK: back(); return true;
        default: return false;
        }
    if (screen_ == Screen::Options) switch (vk) {   // section keys
        case 'Q': case VK_PRIOR: set_tab(tab_ - 1); return true;
        case 'E': case VK_NEXT: set_tab(tab_ + 1); return true;
        case VK_TAB: set_tab(tab_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); return true;
        }
    if (screen_ == Screen::Options && n > 1) switch (vk) {   // the rows: the column of sections is not one of them
        case VK_UP: case 'W': select(selected_ <= 1 ? n - 1 : selected_ - 1); return true;
        case VK_DOWN: case 'S': select(selected_ >= n - 1 ? 1 : selected_ + 1); return true;
        }
    switch (vk) {
    case VK_UP: case 'W': select((selected_ + n - 1) % n); return true;
    case VK_DOWN: case 'S': select((selected_ + 1) % n); return true;
    case VK_LEFT: case 'A':
        if (tabs_row) set_tab(tab_ - 1);
        else if (option_row) activate(list[selected_], -1);
        return true;
    case VK_RIGHT: case 'D':
        if (tabs_row) set_tab(tab_ + 1);
        else if (option_row) activate(list[selected_], +1);
        return true;
    case VK_RETURN: case VK_SPACE:
        hovered_ = -1;
        if (selected_ < n && list[selected_].enabled) activate(list[selected_], +1);
        return true;
    case VK_ESCAPE: case VK_BACK: back(); return true;
    }
    return false;
}

void Frontend::mouse_move(int x, int y) {
    if (screen_ == Screen::Splash) return;
    mouse_y_ = y;
    if (restart_open_) {   // the button under the pointer
        for (int b = 0; b < 2; b++)
            if (PtInRect(&restart_buttons_[b], POINT{x, y}) && b != restart_button_) restart_button_ = b, dirty_ = true;
        return;
    }
    if (drop_open_) {   // the list's entry under the pointer
        for (int k = 0; k < int(drop_rects_.size()); k++)
            if (PtInRect(&drop_rects_[k], POINT{x, y}) && drop_top_ + k != drop_sel_) drop_sel_ = drop_top_ + k, dirty_ = true;
        return;
    }
    int hit = -1;
    for (size_t i = 0; i < hit_.size(); i++) {
        POINT p{x, y};
        if (PtInRect(&hit_[i], p)) hit = int(i);
    }
    if (hit >= 0) select(hit);
    hovered_ = hit;
}

void Frontend::click(int x, int y) {
    if (screen_ == Screen::Splash) {
        skip_splash();
        return;
    }
    if (starting) return;
    POINT p{x, y};
    if (restart_open_) {   // only its two buttons
        for (int b = 0; b < 2; b++)
            if (PtInRect(&restart_buttons_[b], p)) restart_choose(b);
        return;
    }
    if (drop_open_) {   // a resolution, or away from the list: closed
        for (int k = 0; k < int(drop_rects_.size()); k++)
            if (PtInRect(&drop_rects_[k], p)) { drop_pick(drop_top_ + k); return; }
        drop_open_ = false;
        dirty_ = true;
        return;
    }
    if (PtInRect(&more_up_, p) || PtInRect(&more_down_, p)) {   // the scroll arrows of a long section
        int n = int(items().size());
        int k = std::clamp(selected_ - 1, 0, n - 3) + (PtInRect(&more_up_, p) ? -1 : 1);
        select(1 + std::clamp(k, 0, n - 3));
        return;
    }
    mouse_move(x, y);
    auto list = items();
    if (hovered_ < 0 || hovered_ >= int(list.size()) || !list[hovered_].enabled) return;
    if (list[hovered_].id == TABS) {   // the section under the pointer
        for (int t = 0; t < int(tab_rects_.size()); t++)
            if (PtInRect(&tab_rects_[t], p)) set_tab(t);
        return;
    }
    // Option rows: the arrows step down and up; anywhere else the list of values
    // opens (a row without one steps up and wraps around)
    int dir = 0;
    if (hovered_ < int(values_.size()) && values_[hovered_].left >= 0) {
        const ValueArea& v = values_[size_t(hovered_)];
        if (x >= v.left && x < v.value) dir = -1;
        else if (x >= v.right) dir = +1;
    }
    if (dir == 0 && list[hovered_].id >= OPT_DISPLAY && open_drop(list[hovered_].id)) return;
    wrap_ = dir == 0;
    activate(list[hovered_], dir == 0 ? +1 : dir);
    wrap_ = false;
}

void Frontend::wheel(int x, int y, int delta) {
    if (starting || screen_ == Screen::Splash || restart_open_) return;
    if (drop_open_) {
        drop_key(delta > 0 ? VK_UP : VK_DOWN);
        return;
    }
    mouse_move(x, y);
    auto list = items();
    bool on_value = hovered_ >= 0 && hovered_ < int(values_.size()) && values_[hovered_].left >= 0 &&
                    x >= values_[hovered_].left;
    bool scrolls = (screen_ == Screen::Options || screen_ == Screen::Records) && options_scroll_;
    if (hovered_ >= 0 && hovered_ < int(list.size()) && list[hovered_].id >= OPT_DISPLAY && (on_value || !scrolls)) {
        int keep = hovered_;   // over a value: change it
        hovered_ = -1;   // no wrap-around from the wheel
        activate(list[keep], delta > 0 ? +1 : -1);
        hovered_ = keep;
    } else if (scrolls) {   // a long tab, away from the values: scroll the list
        int k = std::clamp(selected_ - 1, 0, int(list.size()) - 3) + (delta > 0 ? -1 : 1);
        select(1 + std::clamp(k, 0, int(list.size()) - 3));
    }
}

std::vector<Frontend::Action> Frontend::take_actions() {
    std::vector<Action> a;
    a.swap(actions_);
    return a;
}

// ---- Records ----

namespace {

std::wstring grouped(int64_t v) {   // 12,345
    std::wstring d = std::to_wstring(v < 0 ? -v : v), r;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) r += L',';
        r += d[i];
    }
    return v < 0 ? L"-" + r : r;
}

std::wstring clock_time(int64_t frames) {   // 2:31.4
    double s = double(frames) / 57.524;
    wchar_t b[32];
    swprintf(b, 32, L"%d:%04.1f", int(s / 60), s - 60 * int(s / 60));
    return b;
}

std::wstring hours(int64_t frames) {   // 3 H 05 MIN
    int64_t m = int64_t(double(frames) / 57.524 / 60);
    wchar_t b[32];
    swprintf(b, 32, L"%lld H %02lld MIN", (long long)(m / 60), (long long)(m % 60));
    return b;
}

std::wstring percent(int64_t part, int64_t whole) {
    if (whole <= 0) return L"-";
    wchar_t b[16];
    swprintf(b, 16, L"%.1f%%", 100.0 * double(part) / double(whole));
    return b;
}

}  // namespace

std::vector<Frontend::RecordRow> Frontend::record_rows() const {
    std::vector<RecordRow> rows;
    const Stats& s = records.stats;
    if (tab_ == 0) {   // achievements: unlocked first (newest on top), then the rest in the table's order
        const auto& table = achievement_table();
        std::vector<int> order(table.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
        auto when = [&](int i) { return size_t(i) < records.unlocked.size() ? records.unlocked[size_t(i)] : 0; };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return (when(a) != 0) != (when(b) != 0) ? when(a) != 0 : when(a) > when(b);
        });
        for (int i : order) {
            const AchievementInfo& a = table[size_t(i)];
            RecordRow r{a.name, a.text, L"", when(i) != 0, when(i) != 0 ? 0 : 1};
            if (r.lit) {
                time_t t = time_t(when(i));
                wchar_t b[32];
                wcsftime(b, 32, L"%d %b %Y", localtime(&t));
                r.value = b;
            } else if (a.goal > 1) {
                int64_t p = achievement_progress(s, i);
                r.value = std::string(a.key) == "veteran"
                              ? std::to_wstring(int(double(p) / 57.524 / 3600)) + L" / 5 H"
                              : grouped(p) + L" / " + grouped(a.goal);
            } else {
                r.value = L"LOCKED";
            }
            rows.push_back(r);
        }
        return rows;
    }
    int group = 2;   // TOTALS, then BEST STAGES
    auto row = [&](const std::wstring& label, const std::wstring& value, const std::wstring& detail = L"") {
        rows.push_back({label, detail, value, true, group});
    };
    row(L"GAMES PLAYED", grouped(s.games));
    row(L"TIME PLAYED", hours(s.play_frames));
    row(L"BEST SCORE", s.best_score ? grouped(s.best_score) : L"-", L"In one game");
    row(L"ENEMIES DESTROYED", grouped(s.kills), percent(s.kills, s.appeared) + L" of the " + grouped(s.appeared) +
                                                  L" that appeared");
    row(L"PHASES CLEARED", grouped(s.phases_cleared),
        grouped(s.perfect_phases) + L" with every enemy destroyed,  " + grouped(s.untouched_phases) +
            L" without losing any shield");
    group = 3;
    for (int i = 0; i < STAT_STAGES; i++) {
        const Stats::Stage& st = s.stage[i];
        std::wstring detail = st.clears ? grouped(st.clears) + L" clears,  best phase " +
                                              percent(st.best_destroyed, 1000) + L" of the enemies destroyed"
                                        : L"Not cleared yet";
        row(STAT_STAGE_NAMES[i], st.best_frames ? clock_time(st.best_frames) : L"-", detail);
    }
    return rows;
}

// The room for the logo: LOGO_H of the window high from LOGO_TOP, centred
// LOGO_FROM_RIGHT (1080p pixels) from the right edge, above the menu's column.
// The logo picture is at most LOGO_W wide in it, with the tagline under it
constexpr float LOGO_TOP = 0.07f, LOGO_H = 0.34f, LOGO_FROM_RIGHT = 370, LOGO_W = 400;

// The background picture covering the window, darker towards the right where
// the menu goes and at the bottom, with the logo; blurred for the options
void Frontend::backdrop_background() {
    uint32_t* keep = px_;
    bg_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_.data();
    picture(backdrop_, 0, 0, w_, h_, 0.0f);   // narrower windows lose its right side: the ship stays whole
    // The blur: the picture averaged down by 16 in each direction, smoothed a
    // few times and stretched back
    const int F = 16;
    Picture blur;
    blur.w = std::max(4, w_ / F);
    blur.h = std::max(4, h_ / F);
    std::vector<float> acc(size_t(blur.w) * blur.h * 3, 0.0f);
    for (int y = 0; y < blur.h * F && y < h_; y++)
        for (int x = 0; x < blur.w * F && x < w_; x++) {
            uint32_t c = bg_[size_t(y) * w_ + x];
            float* d = &acc[(size_t(y / F) * blur.w + x / F) * 3];
            for (int m = 0; m < 3; m++) d[m] += chan(c, 16 - 8 * m) / (F * F);
        }
    for (int pass = 0; pass < 3; pass++) {   // 3x3 box, three times
        std::vector<float> src = acc;
        for (int y = 0; y < blur.h; y++)
            for (int x = 0; x < blur.w; x++)
                for (int m = 0; m < 3; m++) {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = std::clamp(x + dx, 0, blur.w - 1), yy = std::clamp(y + dy, 0, blur.h - 1);
                            sum += src[(size_t(yy) * blur.w + xx) * 3 + m];
                        }
                    acc[(size_t(y) * blur.w + x) * 3 + m] = sum / 9;
                }
    }
    blur.px.resize(size_t(blur.w) * blur.h);
    for (size_t i = 0; i < blur.px.size(); i++) {
        uint32_t out = 0xFF000000u;
        for (int m = 0; m < 3; m++)
            out |= uint32_t(std::clamp(acc[i * 3 + m], 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * m);
        blur.px[i] = out;
    }
    bg_blur_.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = bg_blur_.data();
    picture(blur, 0, 0, w_, h_);
    fill(0, 0, w_, h_, 0x000000, 0.3f);
    px_ = bg_.data();
    shade_backdrop();
    px_ = keep;
}

void Frontend::shade_backdrop() {
    hgrad(int(w_ * 0.42f), 0, int(w_ * 0.66f), h_, 0x02040A, 0.0f, 0.55f);
    fill(int(w_ * 0.66f), 0, w_, h_, 0x02040A, 0.55f);
    hgrad(int(w_ * 0.66f), 0, w_, h_, 0x02040A, 0.0f, 0.5f);
    vgrad(0, int(h_ * 0.72f), w_, h_, 0x000000, 0.0f, 0.6f);
    if (logo_.w > 0) {
        // As in The Lost World: the logo picture, LOGO_W wide at most, and
        // under it a thin blue rule and what this is, in three lines of pale
        // typewriter capitals; all of it centred in the room above the column
        auto U = [&](float v) { return int(v * u_ + 0.5f); };
        static const wchar_t* const TAGLINE[] = {L"AN UNOFFICIAL, FAN-MADE RECOMPILATION", L"OF THE 1998 ARCADE GAME",
                                                 L"STAR WARS TRILOGY ARCADE"};
        const int lines = int(std::size(TAGLINE));
        const int tag_px = U(20), tag_line = U(28), gap = U(14), rule_gap = U(14);
        const int below = gap + rule_gap + lines * tag_line;
        const int band0 = int(0.05f * h_), band1 = int((LOGO_TOP + LOGO_H) * h_) + U(36) - U(50);
        int lw = U(LOGO_W), lh = lw * logo_.h / logo_.w;
        const int max_h = band1 - band0 - below;   // a tall logo: as high as fits
        if (lh > max_h && max_h > 0) lh = max_h, lw = lh * logo_.w / logo_.h;
        const int cx = int(w_ - LOGO_FROM_RIGHT * u_);
        int top = band0 + std::max(0, (band1 - band0 - (lh + below)) / 2);
        sprite(logo_, cx - lw / 2, top, lw, lh);
        int ty = top + lh + gap, rule_w = U(330);
        fill(cx - rule_w / 2, ty, cx + rule_w / 2, ty + std::max(1, U(1.5f)), SABER, 0.7f);
        ty += rule_gap;
        for (const wchar_t* line : TAGLINE) {
            text(MONO, tag_px, line, cx, ty, 1, {0xC8DCF0, 0xC8DCF0, 0, 0, 1, std::max(1, U(2))});
            ty += tag_line;
        }
    }
}

// Main, Play, Debug and the pause: the entries in a column at the right under
// the logo, the description under them; the records at the top left, the
// credits at the bottom
void Frontend::draw_side_menu(const std::vector<Item>& list) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    int w = w_, h = h_;
    int margin = U(110), col_w = U(720);
    int tx = w - margin, band0 = w - margin - col_w, band1 = w;
    int y0 = int((LOGO_TOP + LOGO_H) * h_) + U(36);
    if (screen_ == Screen::Pause) {   // what this is, over the entries
        text(TITLE, U(64), L"PAUSED", tx, y0 - U(6), 2, {GOLD_TOP, GOLD_BOTTOM, EDGE, U(3), 1, U(4)});
        y0 += U(84);
    }
    const wchar_t* sub = screen_ == Screen::Play ? L"SELECT MODE"
                         : screen_ == Screen::Debug ? (debug_stage_ < 0 ? L"DEBUG:  SELECT STAGE" : L"DEBUG:  SELECT PHASE")
                         : nullptr;
    if (sub) {
        text(MONO, U(24), sub, tx, y0, 2, {0x9ED8FF, 0x6AB8F0});
        y0 += U(50);
    }
    if (screen_ == Screen::Main) {   // how far the player has got, at the top left
        int got = 0;
        for (int64_t t : records.unlocked) got += t != 0;
        int best = records.stats.best_score;
        std::vector<std::pair<std::wstring, std::wstring>> chips = {
            {L"ACHIEVEMENTS", std::to_wstring(got) + L"/" + std::to_wstring(achievement_table().size())}};
        if (best > 0) chips.push_back({L"BEST SCORE", grouped(best)});
        int x = U(70), cy0 = U(56), ch = U(56);
        for (const auto& c : chips) {
            int lw = measure(ITEM, U(21), c.first), vw = measure(ITEM, U(26), c.second);
            int cw = U(22) + lw + U(12) + vw + U(22);
            fill(x, cy0, x + cw, cy0 + ch, PANEL, 0.8f);
            frame(x, cy0, x + cw, cy0 + ch, std::max(1, U(1.5f)), SABER, 0.45f);
            text(ITEM, U(21), c.first, x + U(22), cy0 + ch / 2 - U(13), 0, {PALE_TOP, PALE_BOTTOM});
            text(ITEM, U(26), c.second, x + cw - U(22), cy0 + ch / 2 - U(16), 2, {GOLD_TOP, GOLD_BOTTOM});
            x += cw + U(16);
        }
    }

    // The entries share what is left of the height; the description of the
    // chosen one right under it, the entries below making room. The main menu as The Lost World's and Sega Rally 2's: PLAY big,
    // the rest smaller under it
    int n = int(list.size());
    const int spacing_all = std::clamp((h - U(230) - y0) / std::max(1, n), U(44), U(84));
    const int item_px_all = std::min(U(58), spacing_all * 72 / 100);
    const bool hero_menu = screen_ == Screen::Main || screen_ == Screen::Play || screen_ == Screen::Pause;
    // ARCADE ORIGINAL's pause: its own entries (RESTORE ORIGINAL, BACK TO PC EDITION)
    // smaller, under a thin rule and the group's name
    auto minor = [&](int i) {
        return arcade && screen_ == Screen::Pause && (list[size_t(i)].id == PAUSE_RESTORE || list[size_t(i)].id == PC_EDITION);
    };
    auto row_h = [&](int i) { return minor(i) ? U(50) : !hero_menu ? spacing_all : list[size_t(i)].id == PLAY ? U(104) : U(62); };
    auto row_px = [&](int i) { return minor(i) ? U(36) : !hero_menu ? item_px_all : list[size_t(i)].id == PLAY ? U(92) : U(46); };
    int y = y0;
    for (int i = 0; i < n; i++) {
        const Item& it = list[i];
        if (minor(i) && (i == 0 || !minor(i - 1))) {   // the group's rule and name
            y += U(16);
            fill(tx - U(300), y, tx, y + std::max(1, U(1.5f)), SABER, 0.5f);
            text(MONO, U(20), L"ARCADE ORIGINAL", tx, y + U(10), 2, {0x7FB0D8, 0x6090B8});
            y += U(42);
        }
        bool sel = i == selected_;
        const int spacing = row_h(i), item_px = row_px(i);
        if (sel) {   // lightsaber blue from the right edge, a bright blade along it
            float a = it.enabled ? 0.36f : 0.16f;
            hgrad(band0, y, band1, y + spacing, SABER, 0.0f, a);
            int edge = std::max(3, U(7));
            fill(w - edge - U(4), y, w, y + spacing, SABER, it.enabled ? 0.55f : 0.3f);
            fill(w - edge, y, w, y + spacing, 0xEAF6FF, it.enabled ? 1.0f : 0.5f);
        }
        bool debug_row = it.id == DEBUG || it.id == DEBUG_STAGE || it.id == DEBUG_PHASE;
        const Style st = !it.enabled ? Style{DIM, DIM, EDGE, U(2), 0.9f, U(3)}
                         : sel       ? Style{GOLD_TOP, GOLD_BOTTOM, EDGE, U(2), 1, U(3)}
                         : debug_row ? Style{0xFF8A7A, 0xD8402E, EDGE, U(2), 0.95f, U(3)}
                                     : Style{PALE_TOP, PALE_BOTTOM, EDGE, U(2), 0.95f, U(3)};
        int cap0, cap1;   // the capitals' ink centred in the row, and so in the ribbon and on the chevron
        cap_rows(TITLE, item_px, cap0, cap1);
        int ty = y + spacing / 2 - (cap0 + cap1) / 2;
        int tw = text(TITLE, item_px, it.label, tx, ty, 2, st);
        const bool chevron = hero_menu && sel && it.enabled && tw > 0;   // as in The Lost World and Sega Rally 2
        if (chevron) triangle(tx - tw - U(28), y + spacing / 2, U(22), +1, 0xFFE08A, 0.95f);
        if (!it.value.empty())   // e.g. COMING SOON
            text(MONO, U(20), it.value, tx - tw - U(chevron ? 56.0f : 28.0f), y + spacing / 2 - U(12), 2,
                 {0x7FB0D8, 0x6090B8});
        hit_.push_back({band0, y, band1, y + spacing});
        values_.push_back({-1, 0, 0});
        y += spacing;
        if (sel) {   // its description right under it, the entries below move down
            y += U(4);
            for (const std::wstring& line : wrap(ITEM, U(24), it.hint, col_w - U(20))) {
                text(ITEM, U(24), line, tx, y, 2, {HINT, HINT});
                y += U(32);
            }
            y += U(10);
        }
    }

    // The bottom: the original game's credit at the left, the port's in the
    // middle (main menu), the keys at the right
    vgrad(0, h - U(130), w, h, 0x000000, 0.0f, 0.85f);
    int fy = h - U(58);
    text(ITEM, U(19), L"ORIGINAL ARCADE GAME © 1998 SEGA", U(70), fy - U(14), 0, {0xA8B4C4, 0x8E9AAC});
    text(ITEM, U(19), L"STAR WARS © LUCASFILM LTD.", U(70), fy + U(14), 0, {0x8E9AAC, 0x7A889A});
    if (credit_.w > 0) {   // on every side menu
        int lw = U(250), lh = lw * credit_.h / credit_.w, ly = h - U(34) - lh;
        sprite(credit_, (w - lw) / 2, ly, lw, lh);
        text(ITEM, U(17), L"PC REMASTER BY", w / 2, ly - U(26), 1, {FOOT, FOOT});
    }
    // The keys at the right, each in a keycap (or the gamepad's button), ending at the right edge
    struct Key { const wchar_t *pad, *key, *what; };
    std::vector<Key> keys;
    if (!starting) keys.push_back({L"A", L"ENTER", L"SELECT"});
    if (screen_ != Screen::Main || starting) keys.push_back({L"B", L"ESC", L"BACK"});
    int kh = U(36), ky = fy + U(12) - kh / 2, x = w - U(70) + U(40);
    for (const Key& k : keys) x -= prompt_width(kh, k.pad, k.key, k.what);
    for (const Key& k : keys) prompt(x, ky, kh, k.pad, k.key, k.what);
}

// ---- The settings page (OPTIONS, SETTINGS, RECORDS), as Sega Rally 2's ----

namespace {
constexpr uint32_t PAGE_TEXT = 0xEEF3FA, PAGE_DIM = 0x8A98AC, PAGE_FAINT = 0x56627A, PAGE_LINE = 0x3A4458;
constexpr uint32_t GROUP_BLUE = 0x5FB2FF, VALUE = 0xE8D278, TAG = 0x26324A, TAG_TEXT = 0xAFC6E6;
constexpr uint32_t BLADE = 0xEAF6FF;   // a lightsaber's white-hot core

std::wstring title_case(const wchar_t* s) {   // DISPLAY -> Display
    std::wstring t = s;
    for (size_t i = 1; i < t.size(); i++)
        if (t[i - 1] != L' ') t[i] = wchar_t(towlower(t[i]));
    return t;
}
}  // namespace

void Frontend::page_band(int x0, int y0, int x1, int y1, float a0, float a1, float alpha) {
    hgrad(x0, y0, x1, y1, SABER, a0 * alpha, a1 * alpha);
    int bx = x0 - int(13 * u_ + 0.5f), bw = std::max(3, int(6 * u_ + 0.5f)), core = std::max(1, bw / 3);
    fill(bx, y0, bx + bw, y1, SABER, 0.9f * alpha);
    fill(bx + (bw - core) / 2, y0, bx + (bw - core) / 2 + core, y1, BLADE, 0.9f * alpha);
}

void Frontend::glide(Glide& g, long key, int y, int h, int& gy, int& gh) {
    constexpr double GLIDE = 0.13;   // seconds from one row to the next
    double now = now_seconds();
    auto at = [&](float& cy, float& ch) {
        float k = g.t0 < 0 ? 1.0f : float(std::clamp((now - g.t0) / GLIDE, 0.0, 1.0));
        float e = 1 - (1 - k) * (1 - k) * (1 - k);
        cy = g.from + (g.to - g.from) * e;
        ch = g.from_h + (g.to_h - g.from_h) * e;
        return k;
    };
    if (g.key != key) {   // another page (or size): straight there
        g = Glide{float(y), float(y), float(h), float(h), -1, key};
    } else if (float(y) != g.to || float(h) != g.to_h) {   // a new target: from where it is now (a fast repeat retargets)
        float cy, ch;
        at(cy, ch);
        g.from = cy, g.from_h = ch, g.to = float(y), g.to_h = float(h), g.t0 = now;
    }
    float cy, ch;
    if (at(cy, ch) >= 1) g.t0 = -1;
    else gliding_ = true;
    gy = int(std::lround(cy)), gh = int(std::lround(ch));
}

void Frontend::draw_options_page(const std::vector<Item>& list, bool over_game) {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    const int w = w_, h = h_, n = int(list.size());
    const int n_opts = n - 2;   // list: the sections, the rows of the open one, BACK
    const bool rec = screen_ == Screen::Records;   // the records: their sections and rows, nothing to change
    const int tabs = tab_count();
    auto tab_label = [&](int t) { return rec ? RECORD_TAB_NAMES[t] : TAB_NAMES[t]; };
    auto tab_about = [&](int t) { return rec ? RECORD_TAB_ABOUT[t] : TAB_ABOUT[t]; };
    // The page darkens what is behind it: the blurred backdrop, or the paused game
    // (nearly black: the game's HUD must not show through)
    fill(0, 0, w, h, over_game ? 0x000000 : PANEL, over_game ? 0.9f : 0.6f);

    // Columns: as wide as the window allows, up to 1728 units (1080p: a 96 pixel margin)
    const int mx = std::max(U(56), (w - U(1728)) / 2);
    const bool wide = float(w - 2 * mx) / u_ >= 1600;
    const int rail_w = U(wide ? 300.0f : 250.0f), gap = U(wide ? 48.0f : 32.0f), panel_w = U(wide ? 600.0f : 430.0f);
    const int rail_x = mx, rows_x0 = mx + rail_w + gap, panel_x0 = w - mx - panel_w, rows_x1 = panel_x0 - gap;
    const int top = U(196), bottom = h - U(100);   // the columns, between the title and the key help

    // Title, the saber mark under it, where this is, and the logo at the top right
    text(TITLE, U(80), rec ? L"RECORDS" : over_game ? L"SETTINGS" : L"OPTIONS", mx, U(34), 0, {0xFFFFFF, 0xE2EAF4, 0, 0, 1, U(3)});
    fill(mx + U(2), U(134), mx + U(74), U(139), SABER, 1);
    fill(mx + U(2), U(134), mx + U(74), U(134) + std::max(1, U(1)), BLADE, 0.8f);
    std::wstring crumb = std::wstring(over_game ? L"Pause" : L"Main menu") + L"  ›  " +
                         (rec ? L"Records" : over_game ? L"Settings" : L"Options") + L"  ›  " + title_case(tab_label(tab_));
    text(ROW_MEDIUM, U(24), crumb, mx + U(92), U(120), 0, {PAGE_DIM, PAGE_DIM});
    if (logo_.w > 0) {   // (Star Wars' logo is tall: placed by its height)
        int lh = U(wide ? 150.0f : 120.0f), lw = lh * logo_.w / logo_.h;
        sprite(logo_, w - mx - lw, U(22), lw, lh);
    }

    hit_.assign(size_t(n), RECT{});
    values_.assign(size_t(n), ValueArea{-1, 0, 0});

    // The sections, one under the other; the open one with a saber band
    const int sec_h = U(92);
    tab_rects_.clear();
    {   // the open section's band, gliding there from the last one
        int ty = top + tab_ * sec_h;
        int gy, gh;
        glide(glide_tab_, (long(screen_) * 2 + long(options_from_pause_)) * 100003 + h_, ty - U(8), sec_h - U(8), gy, gh);
        page_band(rail_x - U(20), gy, rail_x + rail_w, gy + gh, 0.55f, 0.0f, 1);
        if (!gliding_) triangle(rail_x + U(8), ty + U(28), U(18), +1, 0xFFE08A, 0.95f);
    }
    for (int t = 0; t < tabs; t++) {
        int y = top + t * sec_h;
        bool open_tab = t == tab_;
        RECT r{rail_x - U(20), y - U(8), rail_x + rail_w, y + sec_h - U(16)};
        if (!open_tab && selected_ == 0 && hovered_ == 0 && mouse_y_ >= r.top && mouse_y_ < r.bottom)
            hgrad(r.left, r.top, r.right, r.bottom, 0xFFFFFF, 0.08f, 0.0f);   // under the pointer
        text(TITLE, U(44), tab_label(t), rail_x + U(26), y - U(6), 0,
             {open_tab ? 0xFFFFFFu : PAGE_DIM, open_tab ? 0xE2EAF4u : PAGE_DIM, 0, 0, 1, open_tab ? U(2) : 0});
        text(BODY, U(17), tab_about(t), rail_x + U(28), y + U(46), 0,
             {open_tab ? 0xD2DAE6u : PAGE_FAINT, open_tab ? 0xD2DAE6u : PAGE_FAINT});
        tab_rects_.push_back(r);
    }
    hit_[0] = {rail_x - U(20), top - U(8), rail_x + rail_w, top + tabs * sec_h - U(16)};
    {   // BACK, under the sections
        int y = top + tabs * sec_h + U(28);
        bool sel = selected_ == n - 1;
        RECT r{rail_x - U(20), y - U(6), rail_x + rail_w, y + U(58)};
        if (sel) page_band(r.left, r.top, r.right, r.bottom, 0.55f, 0.0f, 1);
        triangle(rail_x + U(20), y + U(26), U(16), -1, sel ? GOLD_BOTTOM : PAGE_DIM, 1);
        text(TITLE, U(40), L"BACK", rail_x + U(40), y - U(2), 0,
             sel ? Style{GOLD_TOP, GOLD_BOTTOM, 0, 0, 1, U(2)} : Style{PAGE_DIM, PAGE_DIM});
        hit_[n - 1] = r;
    }

    // The rows in their groups. Rows that do not fit scroll, keeping the chosen one in view
    const int row_h = U(46), head_h = U(38), head_gap = U(10), label_px = U(30);
    std::vector<const wchar_t*> group(size_t(std::max(0, n_opts)), nullptr);   // the heading over each row, if any
    std::vector<const wchar_t*> group_name(size_t(std::max(0, n_opts)), L"");  // the group each row is in
    const wchar_t* current = L"";
    for (int k = 0; k < n_opts; k++) {
        const Item& it = list[size_t(1 + k)];
        const wchar_t* g = rec ? (k > 0 && list[size_t(k)].index == it.index ? nullptr   // records: a heading where the group changes
                                                                             : RECORD_GROUPS[std::clamp(it.index, 0, 3)])
                               : group_of(it.id);
        if (g) current = group[size_t(k)] = g;
        group_name[size_t(k)] = current;
    }
    auto height = [&](int k, bool first) {   // a row with its heading (the first one shown always has one)
        bool head = first ? *group_name[size_t(k)] != 0 : group[size_t(k)] != nullptr;
        return row_h + (head ? head_h + (first ? 0 : head_gap) : 0);
    };
    auto fits = [&](int from, int to) {
        int sum = 0;
        for (int k = from; k <= to; k++) sum += height(k, k == from);
        return sum <= bottom - top;
    };
    int k_sel = selected_ - 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n_opts - 1));
    if (k_sel >= 0 && k_sel < n_opts) {
        if (k_sel < scroll_) scroll_ = k_sel;
        while (scroll_ < k_sel && !fits(scroll_, k_sel)) scroll_++;
    }
    while (scroll_ > 0 && fits(scroll_ - 1, n_opts - 1)) scroll_--;   // no room left empty at the bottom
    options_scroll_ = n_opts > 0 && (scroll_ > 0 || !fits(0, n_opts - 1));
    more_up_ = more_down_ = RECT{};

    const int text_h = mask(ROW, label_px, L"Ag", 0).text_h;
    const Style gold{GOLD_TOP, GOLD_BOTTOM, 0, 0, 1, U(2)};
    bool row_band = false;
    if (k_sel >= scroll_ && k_sel < n_opts) {   // the chosen row's band, gliding there from the last one
        int ry = top, k = scroll_;
        for (; k <= k_sel; k++) {
            if (ry + height(k, k == scroll_) > bottom) break;
            ry += height(k, k == scroll_);
        }
        if (k > k_sel) {   // shown
            ry -= row_h;
            const Item& it = list[size_t(1 + k_sel)];
            int gy, gh;
            glide(glide_row_, (long(screen_) * 8 + long(tab_)) * 100003 + h_ + long(options_from_pause_) * 7, ry, row_h, gy, gh);
            page_band(rows_x0 - U(30), gy, rows_x1 + U(18), gy + gh, 0.22f, 0.5f, it.enabled ? 1.0f : 0.4f);
            if (!gliding_) triangle(rows_x0 - U(16), ry + row_h / 2, U(14), +1, 0xFFE08A, it.enabled ? 0.95f : 0.4f);
            row_band = true;
        }
    }
    if (!row_band) glide_row_.key = -1;   // next time straight there
    int y = top, last_shown = scroll_ - 1;
    for (int k = scroll_; k < n_opts; k++) {
        if (y + height(k, k == scroll_) > bottom) break;
        const Item& it = list[size_t(1 + k)];
        const wchar_t* head = k == scroll_ ? (*group_name[size_t(k)] ? group_name[size_t(k)] : nullptr) : group[size_t(k)];
        if (head) {
            if (k != scroll_) y += head_gap;
            int gw = text(ROW_MEDIUM, U(20), head, rows_x0, y + U(6), 0, {GROUP_BLUE, GROUP_BLUE});
            fill(rows_x0 + gw + U(14), y + U(20), rows_x1, y + U(20) + std::max(1, U(1)), PAGE_LINE, 1);
            y += head_h;
        }
        bool sel = 1 + k == selected_;
        int ty = y + (row_h - text_h) / 2, cy = y + row_h / 2;
        uint32_t lc = it.enabled ? PAGE_TEXT : PAGE_FAINT;
        int lw = text(ROW, label_px, it.label, rows_x0, ty, 0, {lc, lc, 0, 0, 1, it.enabled ? U(2) : 0});
        if (it.id == OPT_AA && settings.aa != aa_at_launch) {   // changed, waiting for a restart
            int tx = rows_x0 + lw + U(14), tw = measure(ROW, U(15), L"RESTART") + U(16);
            fill(tx, cy - U(11), tx + tw, cy + U(11), 0x6A2A24, 1);
            text(ROW, U(15), L"RESTART", tx + tw / 2, cy - U(10), 1, {0xFFD9D2, 0xFFD9D2});
        }
        if (it.id == OPT_RT) {   // needs a graphics card with ray tracing
            int tx = rows_x0 + lw + U(14), tw = measure(ROW, U(15), L"RT") + U(16);
            fill(tx, cy - U(11), tx + tw, cy + U(11), TAG, 1);
            text(ROW, U(15), L"RT", tx + tw / 2, cy - U(10), 1, {TAG_TEXT, TAG_TEXT});
        }
        int right = rows_x1 - U(20), vx0;
        if (it.id == OPT_VOLUME || it.id == OPT_MUSIC) {   // ten segments
            int seg_w = U(15), sgap = U(5), seg_h = U(20);
            vx0 = right - 10 * seg_w - 9 * sgap;
            for (int s = 0; s < 10; s++) {
                int x0 = vx0 + s * (seg_w + sgap);
                bool lit = s < (it.id == OPT_MUSIC ? settings.music : settings.volume);
                uint32_t c = lit ? mix(0xBFE8FF, SABER, float(s) / 9) : 0x2A3240;
                fill(x0 - 1, cy - seg_h / 2 - 1, x0 + seg_w + 1, cy + seg_h / 2 + 1, 0x000000, 0.8f);
                fill(x0, cy - seg_h / 2, x0 + seg_w, cy + seg_h / 2, c, sel || !lit ? 1.0f : 0.8f);
            }
        } else {
            uint32_t vc = it.enabled ? VALUE : PAGE_FAINT;
            vx0 = right - text(ROW, label_px, it.value, right, ty, 2,
                               sel && it.enabled ? gold : Style{vc, vc, 0, 0, 1, it.enabled ? U(2) : 0});
        }
        if (sel && it.enabled && !rec) {   // (a record has nothing to change)
            triangle(vx0 - U(20), cy, U(14), -1, GOLD_BOTTOM, 1);
            triangle(right + U(13), cy, U(14), +1, GOLD_BOTTOM, 1);
        }
        hit_[size_t(1 + k)] = {rows_x0 - U(18), y, rows_x1 + U(4), y + row_h};
        values_[size_t(1 + k)] = {vx0 - U(40), vx0 - U(4), right + U(2)};
        y += row_h;
        last_shown = k;
    }
    if (scroll_ > 0) {   // more rows above / below: arrows past the right end of the rows
        triangle(rows_x1 + U(30), top + U(14), U(18), -2, GOLD_BOTTOM, 1);
        more_up_ = {rows_x1 + U(10), top - U(8), rows_x1 + U(52), top + U(36)};
    }
    if (last_shown < n_opts - 1) {
        triangle(rows_x1 + U(30), bottom - U(14), U(18), 2, GOLD_BOTTOM, 1);
        more_down_ = {rows_x1 + U(10), bottom - U(36), rows_x1 + U(52), bottom + U(8)};
    }

    // The panel at the right: what the chosen row is and does
    const int px1 = w - mx, pad = U(wide ? 36.0f : 26.0f);
    panel_rect_ = {panel_x0, top - U(8), px1, bottom};
    fill(panel_x0, top - U(8), px1, bottom, PANEL, over_game ? 0.95f : 0.62f);
    frame(panel_x0, top - U(8), px1, bottom, std::max(1, U(1)), PAGE_LINE, 1);
    fill(panel_x0, top - U(8), px1, top - U(8) + std::max(2, U(4)), SABER, 1);
    if (selected_ >= 0 && selected_ < n) {
        const Item& it = list[size_t(selected_)];
        bool section = it.id == TABS, back = it.id == BACK;
        bool meter = it.id == OPT_VOLUME || it.id == OPT_MUSIC;
        int x = panel_x0 + pad, py = top + U(22), inner = px1 - pad - x;
        const wchar_t* kicker = section ? L"SECTION" : back || n_opts <= 0 ? L"" : group_name[size_t(std::clamp(selected_ - 1, 0, n_opts - 1))];
        if (*kicker) text(ROW_MEDIUM, U(20), kicker, x, py, 0, {GROUP_BLUE, GROUP_BLUE});
        py += U(30);
        std::wstring title = section ? std::wstring(tab_label(tab_)) : it.label;
        int title_px = U(60);
        while (title_px > U(36) && measure(TITLE, title_px, title) > inner) title_px -= U(4);
        text(TITLE, title_px, title, x, py, 0, {0xFFFFFF, 0xE2EAF4, 0, 0, 1, U(3)});
        py += title_px + U(22);
        if (meter) {
            text(ROW, U(34), std::to_wstring(it.id == OPT_MUSIC ? settings.music : settings.volume) + L" / 10", x, py, 0, gold);
            py += U(52);
        } else if (!section && !back && !it.value.empty()) {
            text(ROW, U(34), it.value, x, py, 0, it.enabled ? gold : Style{PAGE_FAINT, PAGE_FAINT});
            py += U(52);
        }
        fill(x, py, px1 - pad, py + std::max(1, U(1)), PAGE_LINE, 1);
        py += U(24);
        std::wstring about = section ? std::wstring(tab_about(tab_)) + L".  " + it.hint : it.hint;
        for (const std::wstring& line : wrap(BODY, U(24), about, inner)) {
            if (py + U(34) > bottom - U(16)) break;
            text(BODY, U(24), line, x, py, 0, {0xD6DEEA, 0xD6DEEA});
            py += U(34);
        }
    }

    // The key help along the bottom
    fill(0, h - U(76), w, h, 0x000000, 0.5f);
    fill(0, h - U(76), w, h - U(76) + std::max(1, U(1)), PAGE_LINE, 1);
    struct Key { const wchar_t *pad, *key, *what; };
    std::vector<Key> keys = pad_prompts
        ? rec ? std::vector<Key>{{L"LS", nullptr, L"SELECT"}, {L"LB", nullptr, L""}, {L"RB", nullptr, L"SECTION"}, {L"B", nullptr, L"BACK"}}
              : std::vector<Key>{{L"LS", nullptr, L"SELECT / CHANGE"}, {L"A", nullptr, L"LIST"}, {L"LB", nullptr, L""},
                                 {L"RB", nullptr, L"SECTION"}, {L"B", nullptr, L"BACK"}}
        : rec ? std::vector<Key>{{nullptr, L"UP / DOWN", L"SELECT"}, {nullptr, L"Q / E", L"SECTION"}, {nullptr, L"ESC", L"BACK"}}
              : std::vector<Key>{{nullptr, L"UP / DOWN", L"SELECT"}, {nullptr, L"LEFT / RIGHT", L"CHANGE"},
                                 {nullptr, L"ENTER", L"LIST"}, {nullptr, L"Q / E", L"SECTION"}, {nullptr, L"ESC", L"BACK"}};
    int kh = U(34), kx = mx;
    for (const Key& k : keys) prompt(kx, h - U(38) - kh / 2, kh, k.pad, k.key, k.what);
}

// ---- RESTART THE GAME? ----

std::wstring Frontend::restart_list() const {
    std::wstring list;
    if (settings.aa != aa_at_launch)
        list = std::wstring(L"ANTI-ALIASING ") + AA_NAMES[std::clamp(settings.aa, 0, AA_COUNT - 1)];
    if (settings.experience != (arcade ? 1 : 0))
        list += (list.empty() ? L"" : L" and ") + std::wstring(L"EXPERIENCE ") +
                (settings.experience ? L"ARCADE ORIGINAL" : L"PC EDITION");
    return list;
}

void Frontend::restart_choose(int button) {
    dirty_ = true;
    if (restore_ask_) {   // RESTORE ORIGINAL: RESTORE, or CANCEL (back to the pause)
        restore_ask_ = restart_open_ = false;
        if (button != 0) return;
        settings.arcade_base();   // (not the screen, the speakers, the controls)
        push(Action::SettingsChanged);
        if (!restart_list().empty()) {   // e.g. ANTI-ALIASING back to OFF: RESTART NOW / LATER
            restart_open_ = true;
            restart_button_ = 0;
        }
        return;
    }
    if (button == 0) {   // RESTART NOW: app.cpp saves, closes and starts the game again
        restart_open_ = false;
        push(Action::Restart);
        return;
    }
    if (!restart_note_.empty() || screen_ == Screen::Pause) {   // BACK TO PC EDITION or a restore, LATER: the pause again (saved already)
        restart_open_ = false;
        restart_note_.clear();
        return;
    }
    back();   // LATER: on, as BACK (the prompt is not asked again)
}

void Frontend::draw_restart() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    const int w = w_, h = h_;
    fill(0, 0, w, h, 0x000000, 0.55f);
    int pw = std::min(U(900), w - U(80)), ph = U(over_game() ? 360.0f : 320.0f);
    int x0 = (w - pw) / 2, y0 = (h - ph) / 2, x1 = x0 + pw, y1 = y0 + ph, cx = w / 2;
    fill(x0, y0, x1, y1, PANEL, 1);
    frame(x0, y0, x1, y1, std::max(1, U(1)), PAGE_LINE, 1);
    fill(x0, y0, x1, y0 + std::max(2, U(4)), SABER, 1);
    text(TITLE, U(52), restore_ask_ ? L"RESTORE ORIGINAL?" : L"RESTART THE GAME?", cx, y0 + U(30), 1,
         {0xFFFFFF, 0xE2EAF4, 0, 0, 1, U(3)});
    int y = y0 + U(104);
    std::wstring list = restart_list();
    std::wstring why = restore_ask_ ? std::wstring(L"The arcade's original sound, anti-aliasing, graphics, effects, MOTION and ASPECT. "
                                                   L"The screen, the speakers and the controls stay as they are")
                       : !restart_note_.empty() ? restart_note_
                       : list + (list.find(L" and ") != std::wstring::npos ? L" need a restart" : L" needs a restart");
    for (const std::wstring& line : wrap(BODY, U(26), why, pw - U(80))) {
        text(BODY, U(26), line, cx, y, 1, {GOLD_TOP, GOLD_BOTTOM});
        y += U(36);
    }
    if (over_game() && !restore_ask_)
        text(BODY, U(22), L"The game in progress will be lost.  LATER keeps it", cx, y + U(4), 1, {0xFF9A8E, 0xFF9A8E});
    const wchar_t* const NAMES[2] = {restore_ask_ ? L"RESTORE" : L"RESTART NOW", restore_ask_ ? L"CANCEL" : L"LATER"};
    int bw = U(280), bh = U(64), gap = U(40), bx = cx - bw - gap / 2, by = y1 - U(36) - bh;
    for (int b = 0; b < 2; b++) {
        int x = bx + b * (bw + gap);
        bool sel = b == restart_button_;
        if (sel) page_band(x + U(13), by, x + bw, by + bh, 0.5f, 0.2f, 1);
        else frame(x, by, x + bw, by + bh, std::max(1, U(1)), PAGE_LINE, 1);
        text(TITLE, U(38), NAMES[b], x + bw / 2, by + bh / 2 - U(38) * 75 / 100, 1,
             sel ? Style{GOLD_TOP, GOLD_BOTTOM, 0, 0, 1, U(2)} : Style{PAGE_TEXT, PAGE_TEXT, 0, 0, 1, U(2)});
        restart_buttons_[b] = {x, by, x + bw, by + bh};
    }
}

// ---- RESOLUTION's list ----

int Frontend::resolution_index() const {
    int n = int(resolutions.size());
    int cw = settings.res_w > 0 ? settings.res_w : settings.display_mode == 0 ? client_w : desktop_w;
    int ch = settings.res_w > 0 ? settings.res_h : settings.display_mode == 0 ? client_h : desktop_h;
    int best = 0;
    for (int k = 1; k < n; k++)
        if (std::abs(resolutions[size_t(k)].first - cw) + std::abs(resolutions[size_t(k)].second - ch) <
            std::abs(resolutions[size_t(best)].first - cw) + std::abs(resolutions[size_t(best)].second - ch))
            best = k;
    return best;
}

void Frontend::first_value(Id id) {
    FrontendSettings& s = settings;
    switch (id) {
    case OPT_DISPLAY: s.display_mode = 0; break;
    case OPT_VOLUME: s.volume = 0; break;
    case OPT_MUSIC: s.music = 0; break;
    case OPT_SPEAKERS: s.speakers = 0; break;
    case OPT_REAR: s.rear_boost = 0; break;
    case OPT_SAMPLES: s.cubic_samples = true; break;
    case OPT_AA: s.aa = AA_ORDER[0]; break;
    case OPT_HAZE: s.haze = true; break;
    case OPT_BLOOM: s.bloom = 0; break;
    case OPT_AO: s.ao = true; break;
    case OPT_SUN: s.sun = true; break;
    case OPT_SHADOWS: s.shadows = true; break;
    case OPT_HDR: s.hdr = true; break;
    case OPT_SHARPEN: s.sharpen = true; break;
    case OPT_SAT: s.saturation = 0; break;
    case OPT_GAMMA: s.gamma = 50; break;
    case OPT_MOTION: s.smooth = true; break;
    case OPT_PAD_SPEED: s.pad_speed = 1; break;
    case OPT_GYRO: s.gyro = true; break;
    case OPT_GYRO_SPEED: s.gyro_speed = 1; break;
    case OPT_VIBRATION: s.vibration = 0; break;
    case OPT_ASSIST: s.aim_assist = 0; break;
    case OPT_TELEMETRY: s.telemetry = true; break;
    case OPT_RT: s.ray_tracing = 0; break;
    case OPT_MENU_MUSIC: s.menu_music = true; break;
    case OPT_POINTER: s.pointer_sight = true; break;
    case OPT_SIGHT: s.sight_size = 50; break;
    case OPT_PAD_AIM: s.pad_lever = false; break;
    case OPT_PAD_INVERT: s.pad_invert = 0; break;
    case OPT_DUEL_INVERT: s.duel_invert = 0; break;
    case OPT_SCALE: s.render_scale = 50; break;
    case OPT_ASPECT: s.aspect = 0; break;
    case OPT_EXPLOSIONS: s.explosions = true; break;
    case OPT_VSYNC: s.vsync = 1; break;   // ON, ADAPTIVE, OFF
    case OPT_MAXFPS: s.max_fps = 0; break;
    case OPT_BGFPS: s.background_fps = true; break;
    case OPT_TRIGGERS: s.trigger_rumble = 0; break;
    case OPT_EXPERIENCE: s.experience = 0; break;
    case OPT_ARCADE_ASPECT: arcade_side().aspect = 0; break;
    default: break;
    }
}

bool Frontend::open_drop(Id id) {
    auto value_of = [&] {   // the row's value as its row shows it
        for (const Item& it : items())
            if (it.id == id) return it.value;
        return std::wstring();
    };
    drop_.clear();
    drop_in_use_ = -1;
    if (id == OPT_RESOLUTION) {
        if (settings.display_mode == 1 || resolutions.empty()) return false;
        int n = int(resolutions.size());
        for (int k = 0; k < n; k++) {   // largest first
            const auto& r = resolutions[size_t(n - 1 - k)];
            DropEntry e{std::to_wstring(r.first) + L" x " + std::to_wstring(r.second), settings, other};
            e.s.res_w = r.first;
            e.s.res_h = r.second;
            drop_.push_back(e);
        }
        drop_in_use_ = n - 1 - resolution_index();
        drop_action_ = Action::DisplayChanged;
    } else {   // stepped up from its first value until a value comes again
        const FrontendSettings keep = settings, keep_other = other;
        const std::wstring now = value_of();
        std::vector<Action> pushed;
        pushed.swap(actions_);
        first_value(id);
        for (int k = 0; k < 64; k++) {
            std::wstring v = value_of();
            if (v.empty() || std::any_of(drop_.begin(), drop_.end(), [&](const DropEntry& e) { return e.label == v; })) break;
            if (v == now) drop_in_use_ = int(drop_.size());
            drop_.push_back({v, settings, other});
            activate(Item{id}, +1);
        }
        drop_action_ = actions_.empty() ? Action::SettingsChanged : actions_.back();
        actions_.swap(pushed);
        settings = keep;
        other = keep_other;
    }
    if (drop_.size() < 2) {
        drop_.clear();
        return false;
    }
    drop_id_ = id;
    drop_sel_ = std::max(drop_in_use_, 0);
    drop_top_ = 0;
    drop_open_ = dirty_ = true;
    return true;
}

void Frontend::drop_pick(int i) {   // applied at once
    drop_open_ = false;
    dirty_ = true;
    if (i < 0 || i >= int(drop_.size())) return;
    if (i != drop_in_use_) {
        settings = drop_[size_t(i)].s;
        other = drop_[size_t(i)].other;
        push(drop_action_);
    }
    drop_.clear();
}

bool Frontend::drop_key(int vk) {
    int n = int(drop_.size());
    switch (vk) {
    case VK_UP: case 'W': drop_sel_ = (drop_sel_ + n - 1) % n; break;
    case VK_DOWN: case 'S': drop_sel_ = (drop_sel_ + 1) % n; break;
    case VK_PRIOR: drop_sel_ = std::max(0, drop_sel_ - 8); break;
    case VK_NEXT: drop_sel_ = std::min(n - 1, drop_sel_ + 8); break;
    case VK_RETURN: case VK_SPACE: drop_pick(drop_sel_); return true;
    case VK_ESCAPE: case VK_BACK: drop_open_ = false; drop_.clear(); break;
    default: return true;
    }
    dirty_ = true;
    return true;
}

// Over the description panel: the row's name, its values, the one in use marked
void Frontend::draw_drop() {
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    RECT pr = panel_rect_;
    if (pr.right <= pr.left) return;
    int n = int(drop_.size()), row = U(44), head = U(64);
    int x0 = pr.left, x1 = pr.right, y0 = pr.top;
    int shown = std::max(1, std::min(n, int((pr.bottom - y0 - head - U(16)) / row)));
    if (drop_sel_ < drop_top_) drop_top_ = drop_sel_;
    if (drop_sel_ >= drop_top_ + shown) drop_top_ = drop_sel_ - shown + 1;
    int y1 = y0 + head + shown * row + U(16);
    fill(x0, y0, x1, pr.bottom, PANEL, 1);   // (the whole panel: its description must not show through)
    frame(x0, y0, x1, y1, std::max(1, U(1)), PAGE_LINE, 1);
    fill(x0, y0, x1, y0 + std::max(2, U(4)), SABER, 1);
    std::wstring title;
    for (const Item& it : items())
        if (it.id == drop_id_) title = it.label;
    text(ROW_MEDIUM, U(22), title, x0 + U(28), y0 + U(22), 0, {GROUP_BLUE, GROUP_BLUE});
    drop_rects_.clear();
    for (int k = 0; k < shown && drop_top_ + k < n; k++) {
        int i = drop_top_ + k, y = y0 + head + k * row;
        bool sel = i == drop_sel_, in_use = i == drop_in_use_;
        if (sel) page_band(x0 + U(30), y, x1 - U(12), y + row, 0.22f, 0.5f, 1);
        text(ROW, U(30), drop_[size_t(i)].label, x0 + U(52), y + row / 2 - U(30) * 75 / 100, 0,
             sel ? Style{GOLD_TOP, GOLD_BOTTOM, 0, 0, 1, U(2)} : Style{PAGE_TEXT, PAGE_TEXT, 0, 0, 1, U(2)});
        if (in_use) text(ROW_MEDIUM, U(18), L"IN USE", x1 - U(28), y + row / 2 - U(10), 2, {GROUP_BLUE, GROUP_BLUE});
        drop_rects_.push_back({x0, y, x1, y + row});
    }
    if (drop_top_ > 0) triangle((x0 + x1) / 2, y0 + head - U(10), U(16), -2, GOLD_BOTTOM, 1);
    if (drop_top_ + shown < n) triangle((x0 + x1) / 2, y1 - U(8), U(16), 2, GOLD_BOTTOM, 1);
}

// ---- Splash screens ----

namespace {
constexpr double SPLASH_GAP = 0.25;    // black before each one
constexpr double SPLASH_IN = 0.7;      // fading in
constexpr double SPLASH_HOLD = 1.8;    // on screen
constexpr double SPLASH_OUT = 0.7;     // fading out
constexpr double SPLASH_SKIP = 0.25;   // fading out when hurried
constexpr double MENU_IN = 0.8;        // the main menu fading in after them
}  // namespace

void Frontend::skip_splash() {
    if (video_on_) {   // faded out with its sound, unless it is going anyway
        if (skip_from_ < 0 && video_end_ < 0) skip_level_ = video_level_, skip_from_ = now_seconds();
        return;
    }
    if (skip_from_ >= 0) {   // already going: the next one goes too
        skip_next_ = true;
        return;
    }
    double t = now_seconds() - splash_from_;
    if (t >= SPLASH_GAP + SPLASH_IN + SPLASH_HOLD) return;   // fading out anyway
    skip_level_ = float(std::clamp((t - SPLASH_GAP) / SPLASH_IN, 0.0, 1.0));
    skip_from_ = now_seconds();
}

bool Frontend::draw_splash(std::vector<uint32_t>& out) {
    if (video_on_) return draw_splash_video(out);
    double now = now_seconds();
    float level = 0;   // brightness: 0 black .. 1 the picture
    for (;;) {
        double t = now - splash_from_;
        bool ended;
        if (skip_from_ >= 0) {
            double k = (now - skip_from_) / SPLASH_SKIP;
            level = skip_level_ * float(1 - std::min(k, 1.0));
            ended = k >= 1;
        } else {
            double out_at = SPLASH_GAP + SPLASH_IN + SPLASH_HOLD;
            level = float(std::clamp(std::min((t - SPLASH_GAP) / SPLASH_IN, 1 - (t - out_at) / SPLASH_OUT), 0.0, 1.0));
            ended = t >= out_at + SPLASH_OUT;
        }
        if (!ended) break;
        if (++splash_on_ >= int(splash_.size())) return false;
        splash_from_ = now;
        skip_from_ = -1;
        if (skip_next_) {   // pressed twice: this one is gone before it shows
            skip_next_ = false;
            skip_from_ = now - SPLASH_SKIP;
            skip_level_ = 0;
        }
    }
    // The picture as big as fits the window, white around it, scaled once per size
    const Picture& p = splash_[splash_on_];
    if (splash_px_on_ != splash_on_ || splash_px_w_ != w_ || splash_px_h_ != h_) {
        splash_px_.assign(size_t(w_) * h_, 0xFFFFFFFFu);
        float sc = std::min(float(w_) / p.w, float(h_) / p.h);
        int pw = std::max(1, int(p.w * sc + 0.5f)), ph = std::max(1, int(p.h * sc + 0.5f));
        int x0 = (w_ - pw) / 2, y0 = (h_ - ph) / 2;
        px_ = splash_px_.data();
        picture(p, x0, y0, x0 + pw, y0 + ph);
        splash_px_on_ = splash_on_, splash_px_w_ = w_, splash_px_h_ = h_;
    }
    out.resize(splash_px_.size());
    uint32_t k = uint32_t(level * 256 + 0.5f);
    if (k >= 256) out = splash_px_;
    else
        for (size_t i = 0; i < out.size(); i++) {   // towards black, opaque
            uint32_t c = splash_px_[i];
            out[i] = 0xFF000000u | ((((c & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * k) >> 8) & 0x00FF00u);
        }
    px_ = out.data();
    return true;
}

// The video: in from black as it starts, played to the end (Windows plays the
// sound), its last picture faded out. Hurried: picture and sound fade out
// together. As big as fits the window, its own background colour around it
bool Frontend::draw_splash_video(std::vector<uint32_t>& out) {
    double now = now_seconds(), t = now - splash_from_;
    if (video_.failed() || (t > 5 && video_.position() <= 0)) return false;   // it will not play: to the menu
    if (video_end_ < 0 && video_.ended()) video_end_ = now;
    float level = float(std::clamp(t / SPLASH_IN, 0.0, 1.0));
    if (skip_from_ >= 0) {
        double k = std::min((now - skip_from_) / SPLASH_SKIP, 1.0);
        if (k >= 1) return false;
        level = skip_level_ * float(1 - k);
        video_.set_volume(float(settings.volume) / 10.0f * float(1 - k));
    } else if (video_end_ >= 0) {
        double k = (now - video_end_) / SPLASH_OUT;
        if (k >= 1) return false;
        level = std::min(level, float(1 - k));
    }
    video_level_ = level;

    out.assign(size_t(w_) * h_, 0xFF000000u);
    px_ = out.data();
    int vw = 0, vh = 0;
    if (!video_.size(vw, vh)) return true;   // still loading: black
    float sc = std::min(float(w_) / vw, float(h_) / vh);
    int pw = std::max(1, int(vw * sc + 0.5f)), ph = std::max(1, int(vh * sc + 0.5f));
    bool resized = pw != video_pw_ || ph != video_ph_;
    if (video_.frame(pw, ph, video_px_, resized)) video_pw_ = pw, video_ph_ = ph;
    if (video_px_.size() != size_t(video_pw_) * video_ph_ || video_pw_ <= 0 || video_pw_ > w_ || video_ph_ > h_) return true;
    uint32_t k = uint32_t(level * 256 + 0.5f);
    auto dim = [k](uint32_t c) {   // towards black, opaque
        if (k >= 256) return c | 0xFF000000u;
        return 0xFF000000u | ((((c & 0xFF00FFu) * k) >> 8) & 0xFF00FFu) | ((((c & 0x00FF00u) * k) >> 8) & 0x00FF00u);
    };
    uint32_t around = dim(video_px_[video_pw_ / 2]);   // the middle of its top edge: its background
    int x0 = (w_ - video_pw_) / 2, y0 = (h_ - video_ph_) / 2;
    for (int y = 0; y < h_; y++) {
        uint32_t* row = &out[size_t(y) * w_];
        if (y < y0 || y >= y0 + video_ph_) {
            std::fill(row, row + w_, around);
            continue;
        }
        const uint32_t* src = &video_px_[size_t(y - y0) * video_pw_];
        std::fill(row, row + x0, around);
        for (int x = 0; x < video_pw_; x++) row[x0 + x] = dim(src[x]);
        std::fill(row + x0 + video_pw_, row + w_, around);
    }
    return true;
}

bool Frontend::draw(int w, int h, std::vector<uint32_t>& out) {
    if (!active() || w <= 0 || h <= 0) return false;
    if (!dirty_ && w == w_ && h == h_ && out.size() == size_t(w) * h) return false;
    gliding_ = false;
    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        u_ = std::min(h / 1080.0f, w / 1440.0f);
        bg_.clear();
        bg_blur_.clear();
    }
    if (screen_ == Screen::Splash) {
        if (draw_splash(out)) {
            dirty_ = true;   // it fades: drawn again every refresh
            return true;
        }
        open(Screen::Main);   // the last one ended: the menu comes in from black
        menu_in_from_ = now_seconds();
    }
    auto U = [&](float v) { return int(v * u_ + 0.5f); };
    bool over_game = this->over_game();
    bool page = screen_ == Screen::Options || screen_ == Screen::Records;
    if (over_game) out.assign(size_t(w) * h, 0);
    else {
        if (bg_.empty()) backdrop_background();
        out = page ? bg_blur_ : bg_;
    }
    px_ = out.data();
    hit_.clear();
    values_.clear();
    // After the splash: the whole picture towards black, less every refresh
    auto fade_in = [&] {
        if (menu_in_from_ < 0) return;
        double k = (now_seconds() - menu_in_from_) / MENU_IN;
        if (k >= 1 || over_game) {
            menu_in_from_ = -1;
            return;
        }
        fill(0, 0, w, h, 0x000000, float(1 - k * k * (3 - 2 * k)));
        dirty_ = true;
    };
    if (starting) {   // LOADING (as Virtua Cop 2's): the main menu's look, LOADING where PLAY was
        const std::wstring hint = screen_ == Screen::Debug ? L"DEBUG: the game goes on to the chosen phase"
                                                           : L"The credit goes in and the game starts: pick your mission";
        const Screen keep_screen = screen_;
        const int keep = selected_;
        screen_ = Screen::Main, selected_ = 0;
        draw_side_menu({{PLAY, L"LOADING", L"", hint}});
        screen_ = keep_screen, selected_ = keep;
        hit_.clear();   // (nothing to click)
        values_.clear();
        // a lightsaber's glow going to and fro under the description (draw_side_menu's places)
        const int right = w - U(110), left = right - U(600), half = U(60);
        const int y = int((LOGO_TOP + LOGO_H) * h_) + U(36) + U(104) + U(4) +
                      U(32) * int(wrap(ITEM, U(24), hint, U(720) - U(20)).size()) + U(18);
        const double k = 0.5 - 0.5 * std::cos(now_seconds() * 2.6);
        saber(left + half + int(k * double(right - left - 2 * half)), y, half);
        dirty_ = true;   // it moves: drawn again every refresh
        fade_in();
        return true;
    }
    auto list = items();
    if (page) {   // the settings page (and the records in its look)
        draw_options_page(list, over_game);
        if (drop_open_) draw_drop();
        if (restart_open_) draw_restart();
        dirty_ = gliding_;   // a band gliding: drawn again every refresh
        fade_in();
        return true;
    }
    // Main, Play and Debug on the backdrop; the pause over the frozen game, darkened
    if (over_game) {
        fill(0, 0, w, h, 0x000000, 0.62f);
        shade_backdrop();   // the main menu's shade and logo
    }
    draw_side_menu(list);
    if (restart_open_) draw_restart();   // (ARCADE ORIGINAL's pause: RESTORE ORIGINAL, BACK TO PC EDITION)
    dirty_ = false;
    fade_in();
    return true;
}
