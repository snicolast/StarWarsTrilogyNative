// The window side: command line and settings, the Vulkan window, the
// presentation loop (2D composition, scene building, rendering, captures),
// and the menus. The emulator runs on its
// own thread in runtime.cpp; runtime.h is what this side uses of it.
// Also of this side: app_input.cpp (keyboard, mouse, gamepads, aiming),
// app_files.cpp (ROM loading, snapshots, captures) and settings_bridge.cpp
// (save/settings.ini <-> the command-line options), sharing app_internal.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dwmapi.h>     // DwmGetCompositionTimingInfo
#include <mmsystem.h>   // timeBeginPeriod
#include <windowsx.h>   // GET_X_LPARAM

#include "gamepad.h"
#include "inputs.h"
#include "options.h"
#include "ppc_rt.h"
#include "raw_mice.h"
#include "rom_zip.h"
#include "runtime.h"
#include "stats.h"
#include "../audio/sound_board.h"
#include "../ui/achievement_toast.h"
#include "../ui/frontend.h"
#include "../ui/menu_music.h"
#include "../video/pack2d.h"
#include "../video/r3d_explosions.h"
#include "../video/r3d_renderer.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_screen.h"
#include "../video/r3d_state.h"
#include "../video/sky_pack.h"
#include "../video/text_hd.h"
#include "../video/texture_pack.h"
#include "../video/tilegen.h"
#include "../video/ui_overlay.h"
#include "../video/vk_presenter.h"
#ifdef __APPLE__
#include "../platform/mac/platform_mac.h"
#endif
#include "app_internal.h"
#include "settings_bridge.h"

Options g_opt;   // command line and menu settings (options.h)

// The stage select screen marks the gun with a small textured dot in its top
// viewport; in a stage the game draws an untextured X instead
static bool select_dot_on_screen(const R3DScene& scene) {
    if (scene.viewports.empty()) return false;
    const R3DViewport& v = scene.viewports.back();   // highest priority
    for (uint32_t i = 0; i < v.trans_count; i++)
        if (scene.vertices[v.trans_first + i].tex == 0x625A0340) return true;
    return false;
}

// When the image being recorded will be on screen: with vsync the images go
// out once per refresh, evenly, while this thread wakes up for them at uneven
// moments (it waits for the GPU and the display, then builds the scene). The
// moment is the next vblank of the monitor, which Windows reports (DWM); if
// it does not, a clock that advances one refresh per image and follows the
// earliest wake-ups (never earlier than a vblank). Either way it moves on by
// whole refreshes, and starts again after a stall (a window drag, a slow frame).
struct DisplayClock {
    double at = 0, last = 0, period = 1.0 / 60;
    double tick() {
        double now = qpc_seconds();
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.qpcRefreshPeriod > 0) {
            double f = double(g_qpf.QuadPart), vblank = double(info.qpcVBlank) / f;
            period = double(info.qpcRefreshPeriod) / f;
            double next = vblank + std::ceil((now - vblank) / period) * period;
            // One refresh per image unless one was really skipped
            at = at > 0 && next < at + 0.5 * period && next > at - 2 * period ? at + period : next;
            last = now;
            return at;
        }
        if (last > 0 && now - last < 0.1) period += 0.002 * (std::clamp(now - last, 1.0 / 500, 0.05) - period);
        last = now;
        if (at > 0 && std::abs(now - (at + period)) < 2 * period) {
            at += period;
            at += now < at ? now - at : 0.01 * (now - at);
        } else {
            at = now;
        }
        return at;
    }
};

// Debugging aid: LW_PACING=1 measures how evenly the interpolated motion
// runs. Every 5 s it prints, over the presents, the game time shown per
// present divided by the time between their moments on screen (1.00 =
// perfectly even; "dispersion" is its standard deviation) and the presents
// where the picture stood still or went back, and the same for the previous
// method (frames dated when the game finished them and placed at the moment
// the image is recorded, no delay for late frames)
static const bool g_pacing_log = getenv("LW_PACING") != nullptr;

// Debugging aid: LW_FRAMELOG=path writes one line per present to that file
// (when the image was prepared, which game frames it shows, where the time
// went) and, with VK_KHR_present_wait, one per present when it reached the
// screen; the game thread adds one per game frame (runtime.cpp). Read with
// tools/frame_log.py
static FILE* g_frame_log = getenv("LW_FRAMELOG") ? fopen(getenv("LW_FRAMELOG"), "w") : nullptr;
static std::mutex g_frame_log_mutex;

void frame_log(const char* fmt, ...) {
    if (!g_frame_log) return;
    va_list args;
    va_start(args, fmt);
    std::lock_guard<std::mutex> lock(g_frame_log_mutex);
    vfprintf(g_frame_log, fmt, args);
    va_end(args);
}

static void pacing_sample(double now, double display, double shown, const R3DFrameState& built) {
    struct Method {
        double last_shown = 0, sum = 0, sum2 = 0;
        int n = 0, still = 0, back = 0;
        void add(double dt, double value) {
            if (last_shown != 0 && dt > 0) {
                double rate = (value - last_shown) / dt;
                if (std::abs(rate) < 4) {   // pauses and cuts are left out
                    sum += rate;
                    sum2 += rate * rate;
                    n++;
                    still += rate < 0.05;
                    back += rate < -0.05;
                }
            }
            last_shown = value;
        }
        void print(const char* name) {
            double mean = n ? sum / n : 0, dev = n ? std::sqrt(std::max(sum2 / n - mean * mean, 0.0)) : 0;
            printf("[pacing] %s: speed %.3f  dispersion %.3f  still %d  back %d  (%d presents)\n", name, mean, dev,
                   still, back, n);
            *this = Method{};
        }
    };
    static Method current, previous;
    static double last_display = 0, last_print = now, prev_flushed = 0, prev_time = 0;
    static double prev_period = 1 / 57.524, game_period = 1 / 57.524;
    static uint64_t prev_frame = 0;
    if (built.frame != prev_frame) {   // the previous method's frame period, from the finishing times
        if (prev_frame) {
            prev_period = std::clamp(built.flushed - prev_flushed, 1.0 / 240, 0.1);
            game_period = std::clamp(built.time - prev_time, 1.0 / 240, 0.1);
        }
        prev_frame = built.frame;
        prev_flushed = built.flushed;
        prev_time = built.time;
    }
    // Both shown as game time: the scenes themselves belong to their vblanks
    double t_old = std::clamp((now - built.flushed) / prev_period, 0.0, 1.0);
    double dt = display - last_display;
    current.add(dt, shown);
    previous.add(dt, built.time - game_period * (1 - t_old));
    last_display = display;
    if (now - last_print >= 5) {
        last_print = now;
        current.print("vblank  ");
        previous.print("previous");
        fflush(stdout);
    }
}

#ifndef __APPLE__
// The game is a windowed program (no console window of its own). Started from
// a terminal, its log goes to that terminal; redirected to a file or a pipe
// (the tests), it goes there as it always did
static void attach_parent_console() {
    auto valid = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    bool out_ok = valid(STD_OUTPUT_HANDLE), err_ok = valid(STD_ERROR_HANDLE);
    if (out_ok && err_ok) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // double click: no console at all
    if (!out_ok) freopen("CONOUT$", "w", stdout);
    if (!err_ok) freopen("CONOUT$", "w", stderr);
}
#endif

int main(int argc, char** argv) {
#ifndef __APPLE__
    attach_parent_console();
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep logs even if the process dies
#ifdef __APPLE__
    mac_enter_game_folder();
    // Both cost almost nothing on Apple's GPUs (--msaa and --aniso still win)
    g_opt.msaa = 8;
    g_opt.aniso = 16;
#endif
    // The menus (and the settings they save) are left out of test runs, so
    // those always start from the same defaults
    g_opt.menu = !options_disable_menu(argc, argv);
    FrontendSettings ini;
    if (g_opt.menu) {   // save/settings.ini, written by the options menu; command-line options win
        ini.load("save/settings.ini");
        options_from_settings(g_opt, ini);
        g_pointer_sight = ini.pointer_sight;
        g_duel_invert = ini.duel_invert;
        g_sight_scale = ini.sight_size / 100.0f;
        g_pad_settings = {ini.pad_speed, ini.gyro, ini.gyro_speed, ini.vibration, ini.aim_assist, ini.pad_lever, ini.pad_invert};
    }
    int exit_code = 0;
    const bool menu_allowed = g_opt.menu;   // no test run (--no-menu itself is read just below)
    if (!parse_options(g_opt, argc, argv, nullptr, exit_code)) return exit_code;
    // What the options set outside the struct: emulation globals and the gun
    g_data_dir = g_opt.data_dir;
    g_frame_limit = g_opt.frame_limit;
    for (const std::string& script : g_opt.auto_scripts) g_auto.parse(script);
    // --no-menu: the intro video still plays first, then straight to the game
    bool intro = menu_allowed && !g_opt.menu && !g_opt.headless && g_auto.events.empty() && g_opt.replay.empty();
    g_fixed_aim = g_opt.fixed_aim;
    if (g_opt.fixed_aim) {   // fixed raw gun ADC values (calibration tests)
        g_inputs.p[0].gun_x = g_opt.aim_x;
        g_inputs.p[0].gun_y = g_opt.aim_y;
    }
    if (!g_opt.diff.empty()) {   // FN@FRAME
        const std::string& d = g_opt.diff;
        g_diff_fn = strtoul(d.c_str(), nullptr, 16);
        size_t at = d.find('@');
        if (at != std::string::npos) g_diff_frame = strtoull(d.c_str() + at + 1, nullptr, 10);
        g_trace_hits[g_diff_fn] = 0;
        g_trace_bits[(g_diff_fn >> 5) & 511] |= uint8_t(1 << ((g_diff_fn >> 2) & 7));
    }
    for (const std::string& list : g_opt.traces) {   // comma-separated: --trace 10F650,10D564
        size_t p = 0;
        while (p < list.size()) {
            uint32_t fn = strtoul(list.c_str() + p, nullptr, 16);
            g_trace_hits[fn] = 0;
            g_trace_bits[(fn >> 5) & 511] |= uint8_t(1 << ((fn >> 2) & 7));
            p = list.find(',', p);
            if (p == std::string::npos) break;
            p++;
        }
    }
    if (g_opt.headless && g_frame_limit == INT32_MAX) {
        g_frame_limit = 600;
    }
    g_collect_stats = g_frame_limit != INT32_MAX;   // report() prints them when the run ends
    printf("Star Wars Trilogy Native - part 4 (%s)\n", g_opt.headless ? "no window" : "Vulkan window, 3D + 2D");
    {
        std::vector<uint8_t> program, samples, dsb_program, dsb_mpeg;
        if (!load_game_rom(program, samples, dsb_program, dsb_mpeg)) return 1;
        // The menus' music: the game's Main Title (stream 0 of the music
        // board), decoded once from the ROM into data/ (not distributed)
        if (g_opt.menu && !g_opt.headless && GetFileAttributesA("data/menu_music.wav") == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryA("data", nullptr), Dsb2::extract_track(dsb_program, dsb_mpeg, 0, "data/menu_music.wav");
        if (!getenv("LW_NOSOUND")) {
            g_sound.init(std::move(program), std::move(samples), g_opt.headless ? nullptr : &g_audio);
            // The music board (DSB2): its own 68000 and the MPEG streams
            if (!getenv("SWT_NOMUSIC") && g_sound.init_dsb(std::move(dsb_program), std::move(dsb_mpeg)))
                printf("[sound] music board ready\n");
            else
                printf("[sound] no music board (dsb_program / dsb_mpeg missing)\n");
        }
        if (!g_opt.wav_path.empty()) g_sound.dump_wav(g_opt.wav_path.c_str());
        g_sound.set_volume(g_opt.volume);
        g_sound.set_music_gain(g_opt.music);
        Scsp::set_cubic(g_opt.cubic_samples);
        g_sound.set_rear_boost(g_opt.rear_boost);
        g_audio.set_speakers(AudioStream::Speakers(g_opt.speakers));
    }
    g_ram = static_cast<uint8_t*>(calloc(RAM_SIZE + 8, 1));
    CreateDirectoryA("save", nullptr);
    CreateDirectoryA("out", nullptr);   // dumps, screenshots, out/missing.txt
    g_eeprom.load("save/eeprom.bin", false);

    // The game code uses the original CPU's stack as memory and nests
    // many calls: run it on a thread with 512 MB of native stack.
    if (g_opt.headless) {
        HANDLE t = CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        WaitForSingleObject(t, INFINITE);
        return 0;
    }

    // Window: the game runs on its own thread at real speed; this thread draws
    // the latest image at the monitor's refresh rate (vsync)
    VkPresenter presenter;
    // The window as wide as the 3D view, but within the screen (32:9 and
    // 48:9 are for fullscreen on very wide monitors)
    int window_w = int(SCREEN_H * 2 * std::max(g_opt.max_aspect, 4.0f / 3.0f));
    if (RECT work; SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        window_w = std::max(SCREEN_W * 2, std::min(window_w, int(work.right - work.left) - 64));
    if (!presenter.init("Star Wars Trilogy Native by snicolast", window_w, SCREEN_H * 2, g_opt.dlaa)) {
        fprintf(stderr, "Could not start Vulkan; try --headless\n");
        MessageBoxA(nullptr, "Could not start Vulkan. Update the graphics driver: the game needs Vulkan 1.3.",
                    "Star Wars Trilogy Native", MB_OK | MB_ICONERROR);
        return 1;
    }
    printf("GPU: %s\n", presenter.device_name().c_str());
    g_pads.init();
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_start_time);
    g_realtime = !getenv("LW_FAST");   // debugging aid: a window at full speed
    // --replay on a folder (recorded with LW_FILM): its snapshots one after
    // another, each captured to out/film_out, so effects that depend on the
    // camera motion can be compared frame by frame
    std::vector<std::string> film;
    size_t film_pos = 0;
    DWORD replay_attr = g_opt.replay.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(g_opt.replay.c_str());
    if (replay_attr != INVALID_FILE_ATTRIBUTES && (replay_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((g_opt.replay + "/*.bin").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do film.push_back(g_opt.replay + "/" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(film.begin(), film.end());
        if (film.empty()) {
            fprintf(stderr, "No snapshots in %s\n", g_opt.replay.c_str());
            return 1;
        }
        g_opt.replay = film[0];
        CreateDirectoryA("out/film_out", nullptr);
    }
    if (!g_opt.replay.empty()) {   // --replay: a frozen debug snapshot, the game does not run
        if (!load_snapshot(g_opt.replay.c_str())) {
            fprintf(stderr, "Could not read the snapshot %s\n", g_opt.replay.c_str());
            return 1;
        }
        printf("Snapshot %s (frame %llu)\n", g_opt.replay.c_str(), (unsigned long long)g_video_frame);
    } else {
        // Records and achievements: real games only, never the scripted or cheated test runs
        if (g_opt.menu && g_auto.events.empty()) stats_init();
        g_audio.start();
        g_hold = g_opt.menu || intro;   // the main menu (or the intro video) comes first
        g_skip_attract = g_opt.menu && g_auto.events.empty();   // the menu instead of the attract (runtime.cpp)
        g_boot_skip = g_fast_idle = !g_opt.show_boot;
        g_sound.set_dsp_enabled(!g_boot_skip);
        g_sound.set_muted(g_boot_skip);
        CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }

    R3DRenderer renderer;
    if (!renderer.init(presenter, g_opt.supersample, g_opt.msaa, g_opt.aniso, g_opt.dlaa)) {
        fprintf(stderr, "Could not start the 3D renderer\n");
        MessageBoxA(nullptr, "Could not start the 3D renderer. Update the graphics driver.",
                    "Star Wars Trilogy Native", MB_OK | MB_ICONERROR);
        return 1;
    }
    renderer.set_max_aspect(g_opt.max_aspect);
    renderer.set_smooth_2d(!g_opt.pixel_2d);
    renderer.set_hud_edges(!g_opt.hud_centre);
    renderer.set_swt_hud(kAnalogJoystick);
    renderer.set_distance_haze(g_opt.haze);
    renderer.set_bloom(g_opt.bloom);
    renderer.set_picture(g_opt.saturation, g_opt.gamma);
    renderer.set_ambient_occlusion(g_opt.ao);
    renderer.set_sun(g_opt.sun);
    renderer.set_night_sky(g_opt.sky_procedural);
    renderer.set_shadows(g_opt.shadows ? 1.0f : 0.0f);
    renderer.set_ray_tracing(g_opt.ray_tracing);
    renderer.set_light_shafts(g_opt.shafts ? 1.0f : 0.0f);
    renderer.set_soft_particles(g_opt.soft);
    r3d_set_swt_explosions(g_opt.explosions);
    renderer.set_hdr(g_opt.hdr);
    renderer.set_relief(g_opt.relief);
    // Star Wars Trilogy's worlds are far bigger in its units than Lost World's
    // (the ships in space hundreds to thousands away, Hoth's walkers hundreds):
    // the surface detail reaches that far (a duel is a few units across)
    if (kAnalogJoystick && getenv("SWT_FAR_RELIEF")) renderer.set_relief_range(1200.0f);
    renderer.set_sharpen(g_opt.sharpen ? 0.5f : 0.0f);
    if (g_opt.haze > 0) printf("[video] distance haze on (strength %.2f)\n", g_opt.haze);
    if (g_opt.dlaa && !renderer.dlaa_active()) printf("[video] DLAA not available: using MSAA\n");
    if (renderer.dlaa_active())
        printf("[video] antialiasing NVIDIA DLAA, anisotropic filtering %dx%s\n", g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    else
        printf("[video] antialiasing MSAA %dx, anisotropic filtering %dx%s\n", renderer.msaa_samples(), g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    if (g_opt.light_gain != 1.0f) {
        renderer.set_light_gain(g_opt.light_gain);
        printf("[video] light x%.2f on the sun and the ambient light\n", g_opt.light_gain);
    }
    R3DSceneBuilder builder;
    builder.set_remove_letterbox(!g_opt.letterbox);
    // Lost World's ammo counter is 3D geometry at the sides of an overlay
    // viewport, moved to the window edges in widescreen. Star Wars Trilogy
    // draws its whole world in such a viewport (the stars are the one below
    // it), so that would move pieces of planets and ships out of place
    builder.set_hud_edges(false);
    builder.set_share_world(true);
    builder.set_procedural_sky(g_opt.sky_procedural);
    TexturePack pack;
    pack.init("textures", g_opt.dump_textures ? "out/textures" : "");
    pack.set_slot_limit(renderer.replacement_slots());
    builder.set_texture_pack(&pack, &g_texture_ram);
    // 2D replacements: the foreground (GPU slots 1-27) and the layers below the
    // 3D image (28-55; the backdrops of SkyPack use 56-63)
    // (none of them, procedural sky included, on GPUs that cannot bind the pictures)
    const bool pictures = renderer.overlay_slots() >= Pack2D::MAX_SLOTS;
    Pack2D pack2d, pack2d_back;
    if (pictures) {
        pack2d.init("textures/2d", 1, 27);
        pack2d_back.init("textures/2d_back", 28, SkyPack::FIRST_SLOT - 1);
    }
    SkyPack sky;   // sky and other backdrops below the 3D image
    sky.init("textures/sky", g_opt.dump_textures ? "out/sky" : "");
    if (!pictures) sky.disable();
    sky.set_procedural(g_opt.sky_procedural);
    // Interpolation of the back layers (the sky): where each row's picture was
    // in the last two 2D frames composed, and the vblanks of the 2D frames
    // whose scroll is in sky_coords (the shift applies while the 3D goes
    // between the 3D frames of those same vblanks)
    std::vector<int32_t> back_rows(SCREEN_H * 3), back_rows_prev(SCREEN_H * 3, -1);
    uint64_t back_rows_frame = ~0ull, shift_from = ~0ull, shift_to = ~0ull, built_from = ~0ull;
    TextHD text;   // menus, captions and score drawn from vector fonts
    if ((g_opt.menu || !g_opt.classic_text) && text.init("textures/text")) renderer.set_text_atlas(text);
    std::atomic<bool> capture_2d{false};   // F12 / --shot: the next 2D composition is saved
    bool f12_was_down = false, f9_was_down = false;
    builder.set_vrom(g_vrom.data(), g_vrom.size());
    // The 2D layers of one vblank as the renderer takes them (compose_2d below)
    struct Layers2D {
        uint64_t frame = ~0ull;   // the vblank
        std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);   // its layer memory
        std::vector<uint32_t> back = std::vector<uint32_t>(BACK_W * SCREEN_H),
                              front = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                              text_cells = std::vector<uint32_t>(TextHD::ROW_TEXELS * TextHD::ROWS),
                              sky_coords = std::vector<uint32_t>(TileGen::SKY_COORDS_W * TileGen::SKY_COORDS_H);
        std::vector<RgbaImage> overlays, sky_pictures;   // new pictures for the GPU slots
        bool hud_band = false;
        float overlay_brightness = 1;
        uint64_t shift_from = ~0ull, shift_to = ~0ull;   // the 2D frames of the back layers' shift
    };
    // A game frame made ready for the screen ahead of time: its 3D scene and
    // the 2D layers of its vblank (the preparation thread, below)
    struct Prepared {
        std::shared_ptr<const R3DFrameState> state;
        R3DScene scene;
        Layers2D layers;
        uint64_t serial = 0;
        double ready_at = 0;   // when it was ready
    };
    R3DScene own_scene;   // built on this thread (no interpolation)
    const R3DScene* scene = &own_scene;
    Layers2D own_layers;   // composed on this thread (no 3D, or no interpolation)
    const Layers2D* layers_shown = &own_layers;
    std::shared_ptr<Prepared> built_prep, layers_prep;   // the prepared frames in use (scene, 2D layers)
    std::shared_ptr<const R3DFrameState> built;
    double frame_period = 1.0 / 57.524;   // between the frame built and the one before
    // Interpolation timing: the last frame the game published, the time
    // between its last two frames and how late after its vblank a frame gets
    // here (the game's CPU time plus this loop's wait for it; follows the
    // latest frames, easing down slowly)
    std::shared_ptr<const R3DFrameState> seen;
    double next_period = 1.0 / 57.524, lateness = 0.004;
    DisplayClock display_clock;

    struct Snapshot2D {
        uint64_t frame = ~0ull;
        std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);
    };
    std::array<Snapshot2D, 4> snaps;   // the 2D layers of the last vblanks (snaps_mutex)
    size_t newest_snap = 0;
    std::mutex snaps_mutex;
    // Copies the game's newest 2D layers into the ring if they are new; the
    // caller holds snaps_mutex
    auto poll_snapshots = [&] {
        std::lock_guard<std::mutex> lock(g_video_mutex);
        if (g_video_frame != snaps[newest_snap].frame && !g_boot_skip) {
            newest_snap = (newest_snap + 1) % snaps.size();
            snaps[newest_snap].frame = g_video_frame;
            memcpy(snaps[newest_snap].vram.data(), g_video_vram.data(), TILEGEN_SIZE);
        }
    };
    // Copies the newest 2D layers at or before that vblank into `out` (its
    // frame stays ~0 if there are none); takes snaps_mutex
    auto take_snapshot = [&](uint64_t up_to, Layers2D& out) {
        std::lock_guard<std::mutex> lock(snaps_mutex);
        poll_snapshots();
        const Snapshot2D* pick = nullptr;
        for (const Snapshot2D& s : snaps)
            if (s.frame != ~0ull && s.frame <= up_to && (!pick || s.frame > pick->frame)) pick = &s;
        out.frame = pick ? pick->frame : ~0ull;
        if (pick) memcpy(out.vram.data(), pick->vram.data(), TILEGEN_SIZE);
    };
    // Scratch layers of the 2D composition (compose_2d)
    std::vector<uint32_t> front_raw(SCREEN_W * SCREEN_H), front_raw_top(SCREEN_W * SCREEN_H),
        back_raw(SCREEN_W * SCREEN_H), back_raw_top(SCREEN_W * SCREEN_H), back_under(SCREEN_W * SCREEN_H),
        front_under(SCREEN_W * SCREEN_H);
    std::vector<uint16_t> tex_rows;
    uint64_t shown = ~0ull, presents = 0;
    double scene_total = 0;   // seconds of 2D composition and scene building since the last title
    ULONGLONG last_title = GetTickCount64();

    // PC front end: main menu at launch, pause menu on Esc
    Frontend frontend;
    // The menus are being driven with a gamepad: no mouse pointer until the mouse moves
    bool pad_in_menu = false;
    raw_mice::init(presenter.hwnd());   // a second mouse can be player 2's gun
    presenter.raw_input = raw_mice::on_input;
    UiOverlay ui;
    std::vector<uint32_t> ui_pixels;
    float haze_strength = g_opt.haze > 0 ? g_opt.haze : 1;
    float ao_strength = g_opt.ao > 0 ? g_opt.ao : 1;
    if (g_opt.menu) {
        if (!frontend.init("third_party/fonts")) printf("[menu] fonts missing: using the Windows ones\n");
        if (!ui.init(presenter)) g_opt.menu = false;
    }
    if (intro) {   // --no-menu: only the intro video, the game waits behind it
        frontend.init("third_party/fonts");
        if (ui.init(presenter)) {
            frontend.settings = ini;
            frontend.settings.splash = true;   // always, whatever settings.ini says
            frontend.open_splash();
        }
        if (!frontend.splash()) {   // no video nor picture, or no overlay
            frontend.open(Frontend::Screen::None);
            intro = false;
            g_hold = false;
        } else {
            presenter.input_hook = [&](UINT msg, WPARAM wp, LPARAM) -> bool {
                if (!frontend.splash()) return false;
                if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_SPACE || wp == VK_ESCAPE || wp == VK_BACK))
                    return frontend.key(int(wp));   // hurries it on (Esc quits only once in the game)
                if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) return frontend.click(0, 0), true;
                return false;
            };
        }
    }
    if (g_opt.menu) {
        FrontendSettings& s = frontend.settings;
        s = ini;   // all of save/settings.ini (the controls, the paths, the update check)...
        settings_from_options(s, g_opt);   // ...with the command-line options over it
        frontend.aa_at_launch = s.aa;
        frontend.rt_available = renderer.ray_tracing_available();
        if (s.fullscreen && !presenter.fullscreen()) presenter.toggle_fullscreen();
        // the logo first (not when a debugging aid drives the menu)
        if (getenv("LW_MENU_KEYS") || getenv("LW_MENU_SHOT")) frontend.open(Frontend::Screen::Main);
        else {
            frontend.open_splash();
            frontend.check_for_news();   // news.txt on GitHub while the logo plays
        }
        g_menu_open = true;
        presenter.input_hook = [&](UINT msg, WPARAM wp, LPARAM lp) -> bool {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (!frontend.active()) {   // in the game: Esc pauses
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
                    frontend.open(Frontend::Screen::Pause);
                    g_menu_open = g_hold = true;
                    return true;
                }
                return false;
            }
            switch (msg) {
            case WM_KEYDOWN:
                pad_in_menu = false;   // the keyboard again: its keys in the key help
                return frontend.key(int(wp));
            case WM_MOUSEMOVE: {
                // Windows also sends it when nothing moved (a window or the
                // pointer changing): those neither bring the pointer back nor
                // take the highlight from the pad's choice
                static int last_x = INT_MIN, last_y = INT_MIN;
                bool moved = x != last_x || y != last_y;
                last_x = x, last_y = y;
                if (!moved) return true;
                pad_in_menu = false;
                frontend.mouse_move(x, y);
                return true;
            }
            case WM_LBUTTONDOWN: frontend.click(x, y); return true;
            case WM_RBUTTONDOWN: frontend.key(VK_ESCAPE); return true;   // right button: back
            case WM_MOUSEWHEEL: {
                POINT p{x, y};   // screen coordinates for this one
                ScreenToClient(presenter.hwnd(), &p);
                frontend.wheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
                return true;
            }
            }
            return false;
        };
    }
    auto close_menu = [&] {
        frontend.open(Frontend::Screen::None);
        ui.hide();
        g_menu_open = g_hold = false;
        g_block_buttons = true;
    };

    // PLAY: 1 = join once booted. Star Wars Trilogy goes to its mission select
    // already behind the menu, from the start, so PLAY shows it at once
    int arcade_join = kAnalogJoystick && g_opt.menu ? 1 : 0;
    bool arcade_entered = false;
    bool play_waits = false;   // PLAY pressed while the game still gets to the mission select

    // Achievement notices, over the game and under the menus
    AchievementToast toast;
    UiOverlay toast_layer;
    std::vector<uint32_t> toast_pixels;
    bool toast_shown = toast.init("third_party/fonts") && toast_layer.init(presenter);
    toast.set_volume(g_opt.volume);

    // Composes the 2D layers of a vblank (out.frame, its layer memory in
    // out.vram) for the renderer. One composition at a time (prep_mutex): the
    // 2D packs and the back layers' scroll keep state between them
    auto compose_2d = [&](Layers2D& out) {
        const TextHD* hd_text = g_opt.classic_text ? nullptr : &text;
        if (!hd_text) std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
        bool capture = capture_2d.exchange(false);
        if (capture) sky.capture();   // F12 also saves the backdrops, for textures/sky
        bool sky_on = sky.active();
        // Raw layers for recognising the 2D replacements (and for F12)
        bool raw_front = !pack2d.empty() || capture, raw_back = !pack2d_back.empty() || capture;
        TileGen::compose(out.vram.data(),
                         {.back = out.back.data(),
                          .front = out.front.data(),
                          .front_raw = raw_front ? front_raw.data() : nullptr,
                          .front_raw_top = raw_front ? front_raw_top.data() : nullptr,
                          .front_under = raw_front ? front_under.data() : nullptr,
                          .back_raw = raw_back ? back_raw.data() : nullptr,
                          .back_raw_top = raw_back ? back_raw_top.data() : nullptr,
                          .back_under = raw_back ? back_under.data() : nullptr,
                          .text_cells = out.text_cells.data(),
                          .sky_coords = sky_on ? out.sky_coords.data() : nullptr,
                          .back_rows = back_rows.data()},
                         hd_text, sky_on ? &sky : nullptr);
        // How far each row of the back layers scrolled since the previous
        // 2D frame composed, for drawing them in between (renderer.set_back_shift)
        {
            uint32_t* shift_row = &out.sky_coords[(SCREEN_H + 1) * TileGen::SKY_COORDS_W];
            for (int y = 0; y < SCREEN_H; y++) {
                const int32_t* now_row = &back_rows[3 * y];
                const int32_t* was = &back_rows_prev[3 * y];
                int dx = ((now_row[1] - was[1] + 256) & 511) - 256, dy = ((now_row[2] - was[2] + 256) & 511) - 256;
                bool known = now_row[0] >= 0 && now_row[0] == was[0] && std::abs(dx) < 100 && std::abs(dy) < 100;
                shift_row[y] = known ? 0xFF000000u | 0xFFu | uint32_t(dx + 128) << 16 | uint32_t(dy + 128) << 8 : 0;
            }
            out.shift_from = back_rows_frame;
            out.shift_to = back_rows_frame = out.frame;
            back_rows_prev.swap(back_rows);
        }
        out.sky_pictures = sky.take_new_images();
        if (capture) {   // F12 or --shot: capture for tools/make_2d_replacement.py
            CreateDirectoryA("out/2d", nullptr);
            std::string path = "out/2d/capture_" + std::to_string(out.frame);
            Pack2D::save_capture(front_raw.data(), path);
            Pack2D::save_capture(front_raw_top.data(), path + "_layer_a");   // only the texts' layers
            Pack2D::save_capture(back_raw.data(), path + "_back");          // the layers below the 3D image
            Pack2D::save_capture(back_raw_top.data(), path + "_back_layer_a");
            printf("2D layer saved to %s.png\n", path.c_str());
        }
        // The top band only moves to the screen edges when it looks like a
        // HUD: the score on the left, a gap, the level on the right
        auto lit = [&](int x0, int x1) {
            int n = 0;
            for (int y = 8; y < 38; y++)
                for (int x = x0; x < x1; x++) n += (out.front[y * SCREEN_W + x] >> 24) != 0;
            return n;
        };
        out.hud_band = lit(303, 318) < 8 && lit(20, 290) > 300 && lit(325, 460) > 150;
        // The lightsaber duels: SUCCESS / FAILURE at the left (x 13..138),
        // the enemy's bar and name at the right (316..479)
        // (its HUD is 3D in fact: r3d_anchor_swt_hud tags it, this lets it move)
        // (not on the duel's briefing: 2D text at the top)
        if (g_swt_duel && lit(20, 460) < 20) out.hud_band = true;
        r3d_set_swt_duel(g_swt_duel);
        r3d_set_swt_stage(g_swt_top_state);
        int offset[3];
        TileGen::front_offset(out.vram.data(), offset);
        if (!pack2d.empty())
            pack2d.apply(out.front.data(), SCREEN_W, front_raw.data(), front_raw_top.data(), out.text_cells.data(),
                         false, front_under.data(), out.back.data() + BACK_MARGIN, BACK_W);
        if (!pack2d_back.empty())
            pack2d_back.apply(out.back.data() + BACK_MARGIN, BACK_W, back_raw.data(), back_raw_top.data(),
                              out.text_cells.data(), true, back_under.data());
        out.overlay_brightness = float(offset[0] + offset[1] + offset[2]) / (3 * 255.0f);
        out.overlays = pack2d.take_new_images();
        for (auto& img : pack2d_back.take_new_images()) out.overlays.push_back(std::move(img));
    };
    // Hands composed 2D layers to the renderer (their new pictures go separately)
    auto apply_2d = [&](const Layers2D& l) {
        renderer.set_sky_coords(l.sky_coords.data());
        renderer.set_hud_band(l.hud_band);
        renderer.set_overlay_brightness(l.overlay_brightness);
        renderer.set_text_cells(l.text_cells.data());
        renderer.set_layers(l.back.data(), l.front.data());
        shift_from = l.shift_from;
        shift_to = l.shift_to;
        shown = l.frame;
        layers_shown = &l;
    };

    // Preparation thread: while interpolating, each new game frame is made
    // ready as soon as the game finishes it, its 3D scene and its 2D layers
    // side by side, so this thread only draws and keeps up with every
    // refresh (the two took ~14 ms here, more than a refresh at 120 Hz, and
    // the images went out unevenly). The interpolation delay follows when
    // the frames are ready (lateness), so it includes this work
    std::mutex prep_mutex;    // one composition and one scene build at a time
    std::mutex ready_mutex;   // ready_newest and ready_pictures
    std::shared_ptr<Prepared> ready_newest;
    // New pictures for the GPU slots, in order: handed over when their frame
    // (or a later one) is taken, also those of frames never taken
    struct NewPictures {
        uint64_t serial = 0;
        std::vector<RgbaImage> replacements, overlays, sky;
    };
    std::deque<NewPictures> ready_pictures;
    auto give_pictures = [&](uint64_t serial) {
        std::vector<NewPictures> due;
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            while (!ready_pictures.empty() && ready_pictures.front().serial <= serial) {
                due.push_back(std::move(ready_pictures.front()));
                ready_pictures.pop_front();
            }
        }
        for (NewPictures& n : due) {
            if (!n.replacements.empty()) renderer.add_replacements(n.replacements);
            if (!n.overlays.empty()) renderer.add_overlays(n.overlays);
            if (!n.sky.empty()) renderer.add_sky_pictures(n.sky);
        }
    };
    std::atomic<bool> prep_on{false}, prep_stop{false};
    std::atomic<uint64_t> prep_us{0}, prep_frames{0};   // its work, read once a second
    std::thread prep_thread([&] {
        std::vector<std::shared_ptr<Prepared>> pool;   // reused once nobody else holds them
        std::shared_ptr<const R3DFrameState> done;
        uint64_t serial = 0;
        while (!prep_stop) {
            std::shared_ptr<const R3DFrameState> state;
            {
                std::unique_lock<std::mutex> lock(g_r3d_mutex);
                g_r3d_cv.wait_for(lock, std::chrono::milliseconds(20),
                                  [&] { return prep_stop || (prep_on && g_r3d_latest != done); });
                state = g_r3d_latest;
            }
            if (!prep_on) done.reset();
            if (prep_stop || !prep_on || !state || state == done) continue;
            done = state;
            std::shared_ptr<Prepared> p;
            for (auto& q : pool)
                if (q.use_count() == 1) {
                    p = q;
                    break;
                }
            if (!p) pool.push_back(p = std::make_shared<Prepared>());
            p->state = state;
            p->serial = ++serial;
            NewPictures pictures;
            pictures.serial = p->serial;
            {
                std::lock_guard<std::mutex> lock(prep_mutex);
                take_snapshot(state->vblank, p->layers);   // the 2D layers of the frame's vblank
                std::future<void> layers;
                if (p->layers.frame != ~0ull)
                    layers = std::async(std::launch::async, [&] { compose_2d(p->layers); });
                double from = qpc_seconds();
                builder.set_fov_widened(g_wide_applied);
                builder.set_view_wide(g_opt.max_aspect > 4.0f / 3.0f + 0.01f ? std::max(1.0f, g_wide_view.load()) : 1.0f);
                builder.build(*state, p->scene);
                if (kAnalogJoystick) collect_scene_targets(builder, p->scene);
                double built_at = qpc_seconds();
                pictures.replacements = pack.take_new_images();
                if (layers.valid()) layers.get();
                double took = qpc_seconds() - from;
                prep_us += uint64_t(took * 1e6);
                prep_frames++;
                const float* st = builder.stage_ms();
                frame_log("R %llu %.6f %.3f %.3f  nodes %.2f track %.2f screen %.2f pack %.2f pair %.2f  polys %zu\n",
                          (unsigned long long)state->frame, qpc_seconds(), (built_at - from) * 1000, took * 1000, st[0],
                          st[1], st[2], st[3], st[4], p->scene.polygons);
                pictures.overlays.swap(p->layers.overlays);
                pictures.sky.swap(p->layers.sky_pictures);
                p->layers.overlays.clear();
                p->layers.sky_pictures.clear();
            }
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready_pictures.push_back(std::move(pictures));
            p->ready_at = qpc_seconds();
            if (prep_on) ready_newest = p;
        }
    });
    // LW_FRAMELOG: a thread notes when each present reaches the screen
    std::atomic<uint64_t> presented_id{0};
    std::thread shown_waiter;
    if (g_frame_log && presenter.can_wait_present())
        shown_waiter = std::thread([&] {
            uint64_t next = 1;
            while (!prep_stop) {
                if (next > presented_id) {
                    Sleep(1);
                    continue;
                }
                if (presenter.wait_present(next, 100'000'000))
                    frame_log("S %llu %.6f\n", (unsigned long long)next, qpc_seconds());
                next++;
            }
        });
    // Both threads stop when the loop ends (and with this scope, however it ends)
    struct StopThreads {
        std::atomic<bool>& stop;
        std::thread &a, &b;
        void operator()() {
            stop = true;
            g_r3d_cv.notify_all();
            if (a.joinable()) a.join();
            if (b.joinable()) b.join();
        }
        ~StopThreads() { (*this)(); }
    } stop_threads{prep_stop, prep_thread, shown_waiter};

    // At most this many images wait for the screen (VK_KHR_present_wait): the
    // picture goes out that many refreshes after it is drawn (~17 ms at
    // 120 Hz) instead of after a full swapchain (~32 ms). When refreshes are
    // missed twice within 2 s (the GPU busier than usual), one more for 10 s.
    // Debugging aid: LW_QUEUE=N (0 = the whole swapchain)
    const int queue_depth = getenv("LW_QUEUE") ? atoi(getenv("LW_QUEUE")) : 2;
    int queue_extra = 0;
    double went_out = 0, last_miss = -100;
    for (;;) {
        {   // before the inputs are read
            int depth = queue_depth ? queue_depth + queue_extra : 0;
            double out = presenter.limit_queue(depth, 50'000'000);
            if (out > 0 && went_out > 0 && out - went_out > 1.5 * display_clock.period) {
                if (out - last_miss < 2 && !queue_extra) {
                    queue_extra = 1;
                    printf("[video] refreshes missed: %d images queued for a while\n", depth + 1);
                }
                last_miss = out;
            }
            if (queue_extra && out - last_miss > 10) {
                queue_extra = 0;
                out = 0;   // the next wait skips a refresh: not a miss
            }
            went_out = out;
        }
        if (!presenter.pump()) break;
        double loop_from = qpc_seconds();
        // PLAY: straight to the mission select. Once the game is past its boot
        // it runs on with nothing on screen, through the SEGA logo (which
        // takes no coin), while the coin and START go in for the player
        // (runtime.cpp, arcade_entry_frame)
        if (arcade_join && !g_boot_skip && (kAnalogJoystick || !g_menu_open)) {
            arcade_join = 0;
            if (kAnalogJoystick) {
                arcade_entered = true;
                g_arcade_entry = 1;
                g_boot_skip = g_fast_idle = true;
                g_sound.set_dsp_enabled(false);
                g_sound.set_muted(true);
            } else {
                player_join(0);
            }
        }
        // A game over: the main menu, while the game goes on to the mission
        // select behind it instead of into the attract (runtime.cpp, attract_guard_frame)
        if (g_game_over_menu.exchange(false) && g_opt.menu) {
            arcade_join = 0;
            play_waits = false;
            frontend.starting = false;
            frontend.open(Frontend::Screen::Main);
            g_menu_open = g_hold = true;
        }
        if (play_waits && !arcade_join && !g_arcade_entry && !g_boot_skip) {
            play_waits = false;
            frontend.starting = false;
            close_menu();
        }
        {   // the 3D view's width (widescreen culling and sight, runtime.cpp)
            RECT wrc;
            GetClientRect(presenter.hwnd(), &wrc);
            if (wrc.right > 0 && wrc.bottom > 0)
                g_wide_view = compute_layout(float(wrc.right), float(wrc.bottom), g_opt.max_aspect).view_w / SCREEN_W;
        }
        if (toast_shown) {
            for (int i : stats_take_new_achievements())
                toast.push(achievement_table()[size_t(i)].name, achievement_table()[size_t(i)].text);
            // The author's credit over the attract (seen with the menu off, --no-menu)
            toast.set_credit(L"PC port by snicolast",
                             L"github.com/snicolast/StarWarsTrilogyNative",
                             g_in_attract && !g_menu_open && !g_boot_skip && !g_arcade_entry);
            RECT rc;
            GetClientRect(presenter.hwnd(), &rc);
            if (toast.draw(rc.right, rc.bottom, qpc_seconds(), toast_pixels))
                toast_layer.set_image(toast_pixels.data(), rc.right, rc.bottom);
        }
        if (g_opt.menu) {
            for (Frontend::Action a : frontend.take_actions()) {
                FrontendSettings& s = frontend.settings;
                switch (a) {
                case Frontend::Action::StartArcade:
                    if (!kAnalogJoystick || !arcade_entered) arcade_join = 1;
                    // Star Wars Trilogy: the menu stays until the mission select is up
                    // behind it (the game runs there unseen, at full speed)
                    if (kAnalogJoystick && (arcade_join || g_arcade_entry || g_boot_skip)) {
                        play_waits = true;
                        frontend.starting = true;
                        frontend.invalidate();
                        break;
                    }
                    [[fallthrough]];
                case Frontend::Action::Resume:
                    close_menu();
                    break;
                case Frontend::Action::StartDebug:   // DEBUG: a phase of a stage (runtime.cpp, debug_jump_frame)
                    if (!kAnalogJoystick) break;
                    g_debug_jump = frontend.debug_choice;
                    if (!arcade_entered) {
                        arcade_join = 1;
                    } else if (!g_arcade_entry) {   // a game on: over at once, and on from the mission select
                        g_arcade_entry = 3;
                        g_boot_skip = g_fast_idle = true;
                        g_sound.set_dsp_enabled(false);
                        g_sound.set_muted(true);
                    }
                    play_waits = true;
                    frontend.starting = true;
                    frontend.invalidate();
                    break;
                case Frontend::Action::QuitToMenu:   // the game over and back to the mission select, unseen (runtime.cpp)
                    arcade_join = 0;
                    g_arcade_entry = 3;
                    g_boot_skip = g_fast_idle = true;
                    g_sound.set_dsp_enabled(false);
                    g_sound.set_muted(true);
                    frontend.open(Frontend::Screen::Main);
                    break;
                case Frontend::Action::Quit: presenter.close(); break;
                case Frontend::Action::ToggleFullscreen: presenter.toggle_fullscreen(); break;
                case Frontend::Action::SettingsChanged:
                    if (aspect_level(g_opt.max_aspect) != s.aspect)   // an --aspect between the levels stays until changed
                        renderer.set_max_aspect(g_opt.max_aspect = MAX_ASPECT[s.aspect]);
                    g_sound.set_volume(float(s.volume));
                    g_sound.set_music_gain(float(s.music));
                    toast.set_volume(float(s.volume));
                    Scsp::set_cubic(g_opt.cubic_samples = s.cubic_samples);
                    g_sound.set_rear_boost(g_opt.rear_boost = s.rear_boost);
                    g_audio.set_speakers(AudioStream::Speakers(g_opt.speakers = s.speakers));
                    renderer.set_distance_haze(s.haze ? haze_strength : 0);
                    renderer.set_bloom(g_opt.bloom = BLOOM_STRENGTH[s.bloom]);
                    renderer.set_ambient_occlusion(s.ao ? ao_strength : 0);
                    renderer.set_sun(g_opt.sun = s.sun);
                    renderer.set_shadows((g_opt.shadows = s.shadows) ? 1.0f : 0.0f);
                    renderer.set_ray_tracing(g_opt.ray_tracing = s.ray_tracing);
                    renderer.set_light_shafts((g_opt.shafts = s.shafts) ? 1.0f : 0.0f);
                    renderer.set_soft_particles(g_opt.soft = s.soft);
                    r3d_set_swt_explosions(g_opt.explosions = s.explosions);
                    renderer.set_hdr(g_opt.hdr = s.hdr);
                    renderer.set_relief(g_opt.relief = s.relief);
                    renderer.set_sharpen((g_opt.sharpen = s.sharpen) ? 0.5f : 0.0f);
                    if (g_opt.sky_procedural != s.sky) {
                        g_opt.sky_procedural = s.sky;
                        {
                            std::lock_guard<std::mutex> lock(prep_mutex);   // not while composing
                            sky.set_procedural(s.sky);
                            builder.set_procedural_sky(s.sky);
                        }
                        renderer.set_night_sky(s.sky);
                        shown = ~0ull;   // compose the 2D layer again even while the game stands still
                    }
                    if (g_opt.classic_text != !s.hd_text) {
                        g_opt.classic_text = !s.hd_text;
                        shown = ~0ull;   // compose the 2D layer again even while the game stands still
                    }
                    g_opt.show_crosshair = s.crosshair;
                    if (g_opt.interp != s.smooth) {
                        g_opt.interp = s.smooth;
                        renderer.set_interpolation(1.0f);   // the latest game frame as it is
                        renderer.set_back_shift(0.0f);
                    }
                    g_opt.several_mice = s.second_mouse;
                    g_pointer_sight = s.pointer_sight;
                    g_duel_invert = s.duel_invert;
                    g_sight_scale = s.sight_size / 100.0f;
                    if (!s.second_mouse) raw_mice::reset();
                    renderer.set_picture(s.saturation / 100.0f, s.gamma / 100.0f);
                    g_pad_settings = {s.pad_speed, s.gyro, s.gyro_speed, s.vibration, s.aim_assist, s.pad_lever, s.pad_invert};
                    s.save("save/settings.ini");
                    break;
                default: break;
                }
            }
            if (frontend.settings.fullscreen != presenter.fullscreen()) {   // also F11 / Alt+Enter
                frontend.settings.fullscreen = presenter.fullscreen();
                frontend.settings.save("save/settings.ini");
                frontend.invalidate();
            }
            // Debugging aid: LW_MENU_KEYS="40,13,27" presses those keys (one
            // every 10 presents; Esc in the game pauses; "cX/Y" clicks at window
            // pixel X,Y), then saves out/menu.bmp and quits
            static const char* menu_keys = getenv("LW_MENU_KEYS");
            static int menu_presents = 0;
            if (menu_keys && ++menu_presents % 10 == 0) {
                if (*menu_keys == 'c') {
                    int cx = 0, cy = 0, len = 0;
                    sscanf(menu_keys, "c%d/%d%n", &cx, &cy, &len);
                    menu_keys += len;
                    if (*menu_keys == ',') menu_keys++;
                    frontend.click(cx, cy);
                } else if (*menu_keys) {
                    char* end = nullptr;
                    int vk = int(strtol(menu_keys, &end, 10));
                    menu_keys = *end == ',' ? end + 1 : end;
                    if (frontend.active()) frontend.key(vk);
                    else if (vk == VK_ESCAPE) {
                        frontend.open(Frontend::Screen::Pause);
                        g_menu_open = g_hold = true;
                    }
                } else if (menu_presents % 30 == 0) {
                    presenter.capture_next("out/menu.bmp");
                    menu_keys = nullptr;
                    presenter.close();
                }
            }
            static Frontend::Screen menu_was = Frontend::Screen::None;
            bool counts = frontend.screen() == Frontend::Screen::Records || frontend.screen() == Frontend::Screen::Main;
            if (counts && menu_was != frontend.screen()) {   // the records, and the main menu's achievements and best score
                frontend.records = stats_view();   // the game stands still while the menu is open
                frontend.invalidate();
            }
            menu_was = frontend.screen();
            if (!frontend.active()) pad_in_menu = false;   // a menu opened later starts with the mouse
            {   // the menus' music: over the backdrop, not over a paused game nor during the splash
                static MenuMusic& music = *new MenuMusic;   // never destroyed: the audio thread reads it to the end
                static bool music_tried = false;
                static double music_time = qpc_seconds();
                if (!music_tried) {
                    music_tried = true;
                    bool found = music.open();
#ifndef __APPLE__   // Windows: through the game's own output; the Mac has its own player
                    if (found)
                        g_audio.set_extra([](void* m, float* out, size_t frames, int channels, int rate) {
                            static_cast<MenuMusic*>(m)->mix(out, frames, channels, rate);
                        }, &music);
#else
                    (void)found;
#endif
                }
                double t = qpc_seconds();
                music.update(frontend.active() && !frontend.over_game() && !frontend.splash() && !frontend.starting &&
                                 frontend.settings.menu_music,
                             frontend.settings.music, t - music_time);
                music_time = t;
            }
            presenter.set_cursor_visible(frontend.active() && !pad_in_menu && !frontend.splash());
            static const bool menu_pad = getenv("SWT_MENU_PAD") != nullptr;   // debugging aid: the gamepad's key help
            if (frontend.pad_prompts != (pad_in_menu || menu_pad)) {   // the key help shows the gamepad's buttons or the keys
                frontend.pad_prompts = pad_in_menu || menu_pad;
                frontend.invalidate();
            }
            if (frontend.active()) {
                RECT rc;
                GetClientRect(presenter.hwnd(), &rc);
                double t0 = qpc_seconds();
                if (frontend.draw(rc.right, rc.bottom, ui_pixels)) {
                    ui.set_image(ui_pixels.data(), rc.right, rc.bottom);
                    double ms = (qpc_seconds() - t0) * 1000;
                    if (ms > 20) printf("[menu] slow redraw: %.0f ms\n", ms);
                    static const char* menu_shot = getenv("LW_MENU_SHOT");   // debugging aid: the menu picture
                    if (menu_shot) save_bmp(menu_shot, ui_pixels.data(), rc.right, rc.bottom);
                }
            }
        }
        if (intro) {   // --no-menu: the intro video, then the game
            if (frontend.splash()) {
                RECT rc;
                GetClientRect(presenter.hwnd(), &rc);
                if (frontend.draw(rc.right, rc.bottom, ui_pixels) && frontend.splash())
                    ui.set_image(ui_pixels.data(), rc.right, rc.bottom);
            }
            if (!frontend.splash()) {
                frontend.open(Frontend::Screen::None);
                ui.hide();
                intro = false;
                g_hold = false;
                g_block_buttons = true;
            }
        }
        poll_keyboard_mouse(presenter.hwnd());
        bool pad_pause = poll_gamepads(presenter.hwnd());
        if (g_pads.changed()) {   // the CONTROLS tab names the pads
            std::wstring names;
            bool gyro = false;
            for (int i = 0; i < Gamepads::MAX; i++) {
                const PadState& pad = g_pads.pad(i);
                if (!pad.connected) continue;
                names += (names.empty() ? L"" : L", ") + std::wstring(pad.name.begin(), pad.name.end());
                gyro |= pad.has_gyro;
            }
            frontend.gamepads = names;
            frontend.gyro_pad = gyro;
            frontend.invalidate();
        }
        for (PadKey k : g_pads.take_keys()) {   // the menus, as keys (START resumes from the pause menu)
            static const int VK[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, 'Q', 'E', 'G'};
            bool resume = frontend.screen() == Frontend::Screen::Pause;
            if (frontend.active()) {
                frontend.key(k == PadKey::Start ? (resume ? VK_ESCAPE : VK_RETURN) : VK[int(k)]);
                pad_in_menu = true;
            }
        }
        if (pad_pause && g_opt.menu && !frontend.active()) {
            frontend.open(Frontend::Screen::Pause);
            pad_in_menu = true;
            g_menu_open = g_hold = true;
        }
        join_update();
        bool f12 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (f12 && !f12_was_down) capture_2d = true;
        f12_was_down = f12;
        bool f9 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (f9 && !f9_was_down && built) {   // debug snapshot of the scene on screen
            CreateDirectoryA("out/snap", nullptr);
            std::string base = "out/snap/snap_" + std::to_string(built->frame);
            save_snapshot((base + ".bin").c_str(), *built, layers_shown->vram);
            dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), (base + ".txt").c_str());
            presenter.capture_next(base + ".bmp");
            if (FILE* f = fopen((base + ".ram").c_str(), "wb")) {   // the game's RAM too (taken while it runs)
                fwrite(g_ram, 1, RAM_SIZE, f);
                fclose(f);
            }
            printf("Snapshot saved to %s.bin (lever %d %d)\n", base.c_str(), int(g_inputs.p[0].gun_x), int(g_inputs.p[0].gun_y));
        }
        f9_was_down = f9;
        if (g_opt.shot_frame && shown != ~0ull && shown + 1 >= g_opt.shot_frame) capture_2d = true;
        for (int p = 0; p < 2; p++) {   // player 2's only while that gun is in use
            PlayerAim aim = player_aim(p);
            bool used = p == 0 || g_inputs.p[1].playing;
            renderer.set_crosshair(p, !kAnalogJoystick && g_opt.show_crosshair && used && aim.inside && !g_opt.fixed_aim, aim.x, aim.y);
        }
        // The 3D frame to draw. While interpolating, the picture shows the game
        // as it was at `shown_time`: a frame period plus the frames' lateness
        // before this image's moment on screen, so the next frame is always
        // here before it is needed and the motion stays even; a new frame is
        // taken once the picture reaches the one built. Otherwise at once.
        // While interpolating the frames come from the preparation thread
        // (their lateness includes its work), otherwise straight from the game
        bool interpolating = g_opt.interp && g_opt.replay.empty() && !g_opt.shot_frame;
        if (interpolating != prep_on) {   // a frame it made earlier is not one to go back to
            prep_on = interpolating;
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready_newest.reset();
        }
        double display = display_clock.tick();
        std::shared_ptr<const R3DFrameState> latest;
        std::shared_ptr<Prepared> ready;
        if (interpolating) {
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready = ready_newest;
        } else {
            std::lock_guard<std::mutex> lock(g_r3d_mutex);
            latest = g_r3d_latest;
        }
        if (ready && built && ready->state->frame < built->frame) ready.reset();   // made before the switch
        if (ready) latest = ready->state;
        if (latest && latest != seen) {
            if (seen) next_period = std::clamp(latest->time - seen->time, 1.0 / 240, 0.1);
            // How long after its vblank the frame was ready to take, plus how
            // far ahead of now this image's moment is: from the preparation
            // thread, when it finished; from the game, when this loop saw it
            double late = ready ? ready->ready_at - latest->time + (display - display_clock.last) : display - latest->time;
            // It rises at once with 1.5 ms to spare and eases down 1.7 ms per
            // second, so that later frames by a little do not each step the
            // motion back
            late = std::clamp(late, 0.0, 0.05);
            lateness = late > lateness ? late + 0.0015 : std::max(lateness - 0.00003, late);
            seen = latest;
        }
        double shown_time = display - lateness - next_period - 0.0005;
        bool take = latest && latest != built && (!interpolating || !built || shown_time >= built->time);
        // The 2D layers of each vblank are kept in a few copies: while the 3D
        // frames are interpolated, those of the 3D frame drawn are shown (the
        // picture runs behind the game; the preparation thread composes them
        // with it), unless the game stopped drawing 3D; otherwise the newest
        uint64_t newest_2d;
        {
            std::lock_guard<std::mutex> lock(snaps_mutex);
            poll_snapshots();
            newest_2d = snaps[newest_snap].frame;
        }
        bool follow = interpolating && latest && newest_2d != ~0ull && newest_2d <= latest->vblank + 4;
        double scene_from = qpc_seconds(), scene_seconds = 0;   // 2D composition and 3D scene build here (CPU)
        const uint64_t shown_before = shown;
        // (shown = ~0: composed again here, as after a change of settings in the pause menu)
        uint64_t compose_up_to = !follow ? (newest_2d != shown ? newest_2d : ~0ull)
                                         : shown == ~0ull && built ? built->vblank : ~0ull;
        if (compose_up_to != ~0ull) take_snapshot(compose_up_to, own_layers);
        if (compose_up_to != ~0ull && own_layers.frame != ~0ull) {
            std::lock_guard<std::mutex> lock(prep_mutex);
            compose_2d(own_layers);
            if (!own_layers.sky_pictures.empty()) renderer.add_sky_pictures(own_layers.sky_pictures);
            if (!own_layers.overlays.empty()) renderer.add_overlays(own_layers.overlays);
            own_layers.sky_pictures.clear();
            own_layers.overlays.clear();
            apply_2d(own_layers);
            layers_prep.reset();
            scene_seconds += qpc_seconds() - scene_from;
        }
        if (take) {
            if (built) frame_period = std::clamp(latest->time - built->time, 1.0 / 240, 0.1);
            built_from = built ? built->vblank : ~0ull;
            scene_from = qpc_seconds();
            if (ready) {   // made ready by the preparation thread, with the 2D layers of its vblank
                give_pictures(ready->serial);
                built_prep = ready;
                scene = &ready->scene;
                // (a frame number going back a long way is the game restarted from a saved state)
                const Layers2D& l = ready->layers;
                if (follow && l.frame != ~0ull && (shown == ~0ull || l.frame > shown || shown - l.frame > 8)) {
                    apply_2d(l);
                    layers_prep = ready;
                }
            } else {
                std::lock_guard<std::mutex> lock(prep_mutex);
                builder.set_fov_widened(g_wide_applied);
                builder.set_view_wide(g_opt.max_aspect > 4.0f / 3.0f + 0.01f ? std::max(1.0f, g_wide_view.load()) : 1.0f);
                builder.build(*latest, own_scene);
                auto images = pack.take_new_images();
                if (!images.empty()) renderer.add_replacements(images);
                built_prep.reset();
                scene = &own_scene;
            }
            scene_seconds += qpc_seconds() - scene_from;
            g_select_aim = select_dot_on_screen(*scene);
            built = latest;
            renderer.set_scene(scene);
            // Debugging aid: LW_SPOTLOG=path writes the world viewport's spotlight every game frame
            static FILE* spot_log = getenv("LW_SPOTLOG") ? fopen(getenv("LW_SPOTLOG"), "w") : nullptr;
            if (spot_log) {
                int worlds = 0;
                for (const R3DViewport& v : scene->viewports)
                    if (v.world) {
                        worlds++;
                        fprintf(spot_log,
                                "%llu vp%d col %.0f%.0f%.0f c %.4f %.4f r %.4f %.4f start %.3f ext %.2f sun %.3f amb %.3f "
                                "fog %.5f/%.3f scroll %.3f far %.1f black %d night %.2f "
                                "light %.3f %.3f %.3f world %.3f %.3f %.3f\n",
                                (unsigned long long)built->frame, worlds, v.spot_color[0], v.spot_color[1], v.spot_color[2],
                                v.spot_center[0], v.spot_center[1], v.spot_radius[0], v.spot_radius[1], v.spot_start,
                                v.spot_extent, v.sun_intensity, v.ambient, v.fog_density, v.fog_ambient, v.scroll_fog,
                                v.scenery_far, int(renderer.back_black()), renderer.night_level(), v.sun[0], v.sun[1], v.sun[2],
                                v.world_axes[0] * v.sun[0] + v.world_axes[1] * v.sun[1] + v.world_axes[2] * v.sun[2],
                                v.world_axes[3] * v.sun[0] + v.world_axes[4] * v.sun[1] + v.world_axes[5] * v.sun[2],
                                v.world_axes[6] * v.sun[0] + v.world_axes[7] * v.sun[1] + v.world_axes[8] * v.sun[2]);
                    }
                if (!worlds) fprintf(spot_log, "%llu no world\n", (unsigned long long)built->frame);
                fflush(spot_log);
            }
            // Debugging aid: LW_FILM=first,count records that many game frames
            // as snapshots in out/film, to replay as a sequence
            static const char* film_rec = getenv("LW_FILM");
            static int film_saved = 0;
            if (film_rec && g_opt.replay.empty()) {
                unsigned long long first = 0;
                int count = 0, every = 1;   // optional third value: keep one frame in that many
                sscanf(film_rec, "%llu,%d,%d", &first, &count, &every);
                static unsigned long long film_last = 0;
                if (built->frame >= first && film_saved < count && (!film_saved || built->frame >= film_last + every)) {
                    film_last = built->frame;
                    CreateDirectoryA("out/film", nullptr);
                    char path[64];
                    snprintf(path, sizeof(path), "out/film/f_%06llu.bin", (unsigned long long)built->frame);
                    save_snapshot(path, *built, layers_shown->vram);
                    film_saved++;
                }
            }
        }
        int y0, y1, tex_dirty_rows = 0;
        if (g_texture_ram.take_dirty(tex_rows, y0, y1)) {
            renderer.upload_texture_rows(tex_rows, y0, y1);
            tex_dirty_rows = y1 - y0;
        }

        bool shot = g_opt.shot_frame && shown != ~0ull && shown >= g_opt.shot_frame;   // --shot N: screenshot and exit
        // A replayed scene is captured after a second on screen, once temporal
        // anti-aliasing (DLAA) has settled
        static uint64_t replay_presents = 0;
        if (shot && !g_opt.replay.empty() && replay_presents++ < 60) shot = false;
        // Debugging aid: LW_SHOTS=N[,FROM[,TO]] saves one game frame in N as out/shots/<frame>.bmp
        static int shots_every = 0;
        static unsigned long long shots_from = 0, shots_to = ~0ull;
        static bool shots_parsed = false;
        if (!shots_parsed) {
            shots_parsed = true;
            if (const char* e = getenv("LW_SHOTS")) sscanf(e, "%d,%llu,%llu", &shots_every, &shots_from, &shots_to);
        }
        static uint64_t last_shot = 0;
        if (shots_every > 0 && shown != ~0ull && shown >= shots_from && shown <= shots_to &&
            shown >= last_shot + shots_every) {
            last_shot = shown;
            CreateDirectoryA("out/shots", nullptr);
            char path[64];
            snprintf(path, sizeof(path), "out/shots/%06llu.bmp", (unsigned long long)shown);
            presenter.capture_next(path);
        }
        if (shot) {
            presenter.capture_next("out/capture.bmp");
            g_texture_ram.dump("out/texture_ram.bin");
            dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), "out/scene.txt");
            if (built && g_opt.replay.empty()) {
                CreateDirectoryA("out/snap", nullptr);
                save_snapshot("out/snap/shot.bin", *built, layers_shown->vram);
            }
        }
        // The game makes ~57 frames per second; in between, the 3D scene is
        // drawn part way from the previous frame to the one built, at shown_time
        // on the game's own evenly spaced timeline
        static const char* fixed_interp = getenv("LW_INTERP");   // debugging aid: fixed position, e.g. 0.5
        if ((interpolating || fixed_interp) && built) {
            double now = qpc_seconds();
            double t = std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0);
            if (fixed_interp) t = std::clamp(atof(fixed_interp), 0.0, 1.0);
            renderer.set_interpolation(float(t));
            // The back layers move with the 3D when their 2D frames are those
            // of the two 3D frames it goes between
            bool paired = shift_to == built->vblank && shift_from == built_from;
            renderer.set_back_shift(paired ? float(1 - t) : 0.0f);
            if (g_pacing_log) pacing_sample(now, display, built->time - frame_period * (1 - t), *built);
        }
        static int film_presents = 0;
        if (!film.empty() && ++film_presents == 3) {
            char path[64];
            snprintf(path, sizeof(path), "out/film_out/%04zu.bmp", film_pos);
            presenter.capture_next(path);
        }
        presenter.frame([&](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
            renderer.record(cmd, view, extent);
            ui.record(cmd, view, extent);
            if (toast_shown) toast_layer.record(cmd, view, extent);   // over the menus too: their backdrop is opaque
        });
        if (g_frame_log) {
            const VkPresenter::LastFrame& f = presenter.last_frame();
            double t = interpolating && built ? std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0) : 1;
            frame_log("P %llu %.6f %.6f %.6f %.4f %llu %llu %.6f %d %d %.3f %d %.3f %.3f %.3f %.3f %.3f %.6f\n",
                      (unsigned long long)f.present_id, loop_from, display, shown_time, t,
                      (unsigned long long)(built ? built->frame : 0), (unsigned long long)(latest ? latest->frame : 0),
                      latest ? latest->time : 0.0, int(take), int(shown != shown_before), scene_seconds * 1000,
                      tex_dirty_rows, f.fence * 1000, f.acquire * 1000, f.record * 1000, f.present * 1000,
                      (qpc_seconds() - loop_from) * 1000, lateness);
            if (f.present_id) presented_id = f.present_id;
        }
        if (!film.empty() && film_presents >= 4) {   // next snapshot of the sequence
            film_presents = 0;
            if (++film_pos >= film.size() || !load_snapshot(film[film_pos].c_str())) break;
        }
        if (shot) {
            printf("Capture saved to out/capture.bmp (frame %llu, %zu polygons, %zu of %zu vertices moving)\n",
                   (unsigned long long)shown, scene->polygons, scene->moving, scene->vertices.size());
            break;
        }
        presents++;
        scene_total += scene_seconds;
        ULONGLONG now = GetTickCount64();
        if (now - last_title >= 1000) {
            // Where the time goes, in ms per frame: the game thread (CPU, without
            // its waits), the preparation thread (per game frame), this thread's
            // scene work and command recording, and the time blocked on the GPU
            // and the display (vsync)
            VkPresenter::Timing t = presenter.take_timing();
            uint64_t game_us = g_game_busy_us.exchange(0), game_n = g_game_frames.exchange(0);
            double per = t.frames ? 1000.0 / t.frames : 0;   // seconds summed over the presents -> ms per present
            double game_ms = game_n ? double(game_us) / 1000.0 / double(game_n) : 0;
            double scene_ms = presents ? scene_total * 1000.0 / double(presents) : 0;
            uint64_t prep_total = prep_us.exchange(0), prep_n = prep_frames.exchange(0);
            double prep_ms = prep_n ? double(prep_total) / 1000.0 / double(prep_n) : 0;
            char title[256];
            snprintf(title, sizeof(title),
                     "Star Wars Trilogy Native by snicolast - %llu Hz display - frame %llu - %zu polygons | ms/frame: game %.1f, "
                     "prep %.1f, scene %.1f, render %.1f, GPU %.1f",
                     (unsigned long long)(presents * 1000 / (now - last_title)), (unsigned long long)(shown == ~0ull ? 0 : shown),
                     scene->polygons, game_ms, prep_ms, scene_ms, t.record * per, (t.gpu_wait + t.present) * per);
            SetWindowTextA(presenter.hwnd(), title);
            static ULONGLONG last_log = now;
            if (now - last_log >= 5000) {   // the same, in the log, every 5 s
                last_log = now;
                printf("[timing] frame %llu  %zu polygons  ms/frame: game %.2f  prep %.2f  scene %.2f  render %.2f  "
                       "GPU wait %.2f  present %.2f  (%llu Hz display)\n",
                       (unsigned long long)(shown == ~0ull ? 0 : shown), scene->polygons, game_ms, prep_ms, scene_ms, t.record * per,
                       t.gpu_wait * per, t.present * per, (unsigned long long)(presents * 1000 / (now - last_title)));
                fflush(stdout);
            }
            presents = 0;
            scene_total = 0;
            last_title = now;
        }
    }
    // Orderly exit: park the game thread, stop the audio, then the video
    stop_threads();
    g_quit = true;
    if (g_opt.replay.empty())
        for (int i = 0; i < 100 && !g_parked; i++) Sleep(10);   // a frame takes ~17 ms; 1 s is plenty
    g_audio.stop();
    stats_save();
    fflush(stdout);
    ui.shutdown();
    if (toast_shown) toast_layer.shutdown();
    toast.shutdown();
    frontend.shutdown();
    renderer.shutdown();
    presenter.shutdown();
    g_pads.shutdown();
    ExitProcess(0);
}
