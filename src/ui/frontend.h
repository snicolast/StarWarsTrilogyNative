// PC front end: main menu (Play, Options, Records, Quit) shown at launch
// instead of the arcade attract loop, and the pause menu Esc opens during the game.
//
// Drawn on the CPU into a full-window BGRA picture (premultiplied alpha) with
// the OFL fonts in third_party/fonts; the settings page and the records in
// Sega Rally 2's layout, in lightsaber blue. video/ui_overlay.h puts it on
// screen. Driven by the keyboard (arrows, Enter, Esc), by the mouse/gun (point
// and click, wheel) and by the gamepads (app.cpp turns what they send into keys).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../runtime/stats.h"
#include "frontend_settings.h"
#include "splash_video.h"

class Frontend {
public:
    enum class Screen { None, Splash, Main, Play, Options, Pause, Records, Debug };
    // DisplayChanged: MODE or RESOLUTION, applied to the window at once.
    // Restart: RESTART NOW (app.cpp saves, closes and starts the game again)
    enum class Action { None, StartArcade, Resume, Quit, DisplayChanged, SettingsChanged,
                        QuitToMenu, StartDebug, Restart, CancelPlay };

    bool init(const std::string& font_dir);
    void shutdown();

    void open(Screen s);
    Screen screen() const { return screen_; }
    bool active() const { return screen_ != Screen::None; }
    // At launch: the logo video with its sound (third_party/menu/ or res/menu/
    // splash_snicolast.mp4; if Windows cannot play it, splash_snicolast.png),
    // skipped with Enter / Esc / a click / START, then the main menu fades in
    void open_splash();
    bool splash() const { return screen_ == Screen::Splash; }
    // The menu is over the frozen game (pause and its settings)
    bool over_game() const { return screen_ == Screen::Pause || (screen_ == Screen::Options && options_from_pause_); }

    // Window input; true when the menu used it
    bool key(int vk);
    void mouse_move(int x, int y);
    void click(int x, int y);
    void wheel(int x, int y, int delta);
    // Actions picked since the last call, oldest first
    std::vector<Action> take_actions();

    FrontendSettings settings;   // the experience running (EXPERIENCE: settings.experience, the choice for the next start)
    // The other experience's settings (PC EDITION's while ARCADE ORIGINAL runs, and
    // the other way round): EXPERIENCE's ASPECT and saving both (app.cpp)
    FrontendSettings other;
    bool arcade = false;    // ARCADE ORIGINAL is running: no main menu, a pause of its own
    int aa_at_launch = 0;   // anti-aliasing the renderer was started with
    bool rt_available = false;   // the GPU can trace rays (RAY TRACING option)
    bool dlss_available = true;  // the GPU can run DLAA and DLSS (an NVIDIA RTX): otherwise ANTI-ALIASING offers FXAA instead
    std::wstring gamepads;  // the pads connected, for OPTIONS > CONTROLS > CONNECTED ("" = none)
    bool gyro_pad = false;  // one of them has a gyroscope
    bool pad_prompts = false;   // a gamepad drives the menu: its buttons in the key help and hints, not the keyboard's
    std::vector<std::pair<int, int>> resolutions;   // the monitor's (app.cpp, VkPresenter::display_modes), smallest first
    int client_w = 0, client_h = 0, desktop_w = 0, desktop_h = 0;   // the window's inside, the desktop (app.cpp)
    int refresh_hz = 60;                            // the monitor's (MAX FPS MONITOR)

    // DEBUG (Star Wars Trilogy): straight to a phase of a stage. StartDebug
    // reads debug_choice = stage * 4 + phase (runtime.cpp, debug jumps)
    int debug_choice = -1;
    // PLAY (or DEBUG's phase) pressed while the game still gets to its mission
    // select: LOADING in PLAY's place, nothing to choose; Esc (the pad's B or
    // BACK, the right button) gives it up (Action::CancelPlay, app.cpp)
    bool starting = false;

    // RECORDS screen (runtime/stats.h): the window keeps this copy up to date while it is open
    StatsView records;

    // Redraws into `out` when something changed (or the size did); true if it did
    bool draw(int w, int h, std::vector<uint32_t>& out);
    void invalidate() { dirty_ = true; }

private:
    enum Id {
        PLAY, OPTIONS, QUIT, ARCADE, BACK, RESUME, SETTINGS, QUIT_TO_MENU, TABS,
        RECORDS, REC_ROW, DEBUG, DEBUG_STAGE, DEBUG_PHASE, DOGFIGHT,
        PAUSE_QUIT, PAUSE_RESTORE, PC_EDITION,   // ARCADE ORIGINAL's pause: QUIT GAME, RESTORE ORIGINAL, BACK TO PC EDITION
        // the settings' rows: OPT_DISPLAY first (key() and wheel(): an option row is one from it on)
        OPT_DISPLAY, OPT_VOLUME, OPT_MUSIC, OPT_SPEAKERS, OPT_REAR, OPT_SAMPLES, OPT_AA, OPT_HAZE, OPT_BLOOM, OPT_AO, OPT_SUN, OPT_SHADOWS, OPT_HDR, OPT_SHARPEN,
        OPT_SAT, OPT_GAMMA, OPT_MOTION, OPT_PAD_SPEED, OPT_GYRO, OPT_GYRO_SPEED, OPT_VIBRATION, OPT_ASSIST, OPT_RT, OPT_MENU_MUSIC, OPT_POINTER, OPT_SIGHT, OPT_PAD_AIM, OPT_PAD_INVERT, OPT_DUEL_INVERT, OPT_SCALE,
        OPT_ASPECT, OPT_EXPLOSIONS, OPT_CONNECTED, OPT_RESOLUTION, OPT_VSYNC, OPT_MAXFPS, OPT_BGFPS, OPT_TRIGGERS,
        OPT_EXPERIENCE, OPT_ARCADE_ASPECT, OPT_TELEMETRY, OPT_TELEMETRY_FORMAT,
    };
    struct Item {
        Id id;
        std::wstring label, value, hint;
        bool enabled = true;
        int index = -1;   // DEBUG_STAGE / DEBUG_PHASE: which one; REC_ROW: its group (RECORD_GROUPS)
    };
    struct Mask {   // text coverage (0..255) with `pad` pixels around the text box
        int w = 0, h = 0, pad = 0, text_w = 0, text_h = 0;
        int grad0 = 0, grad1 = 0;   // rows of the colour gradient (top of the capitals to the baseline)
        std::vector<uint8_t> a, e;  // letters, letters grown by the outline width
    };
    struct Style {
        uint32_t top, bottom;   // gradient (0xRRGGBB)
        uint32_t edge = 0;      // outline colour
        int edge_px = 0;
        float alpha = 1;
        int shadow = 0;         // drop shadow offset (pixels), black
    };
    // ROW, ROW_MEDIUM and BODY: the settings page's rows, group headings and descriptions (as Sega Rally 2's)
    enum Family { TITLE, ITEM, MONO, ROW, ROW_MEDIUM, BODY };

    std::vector<Item> items() const;
    void activate(const Item& it, int dir);
    void back();
    int index_of(Id id) const;   // the entry with that id in the current screen (0 if none)
    void push(Action a) { actions_.push_back(a); }
    void select(int index);

    HFONT font(Family f, int px);
    const Mask& mask(Family f, int px, const std::wstring& text, int edge_px);
    // The rows a capital's ink covers, from the y text() is given (as it draws
    // that family at that size): to centre entries on their row whatever the font
    void cap_rows(Family f, int px, int& top, int& bottom);
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st);
    int measure(Family f, int px, const std::wstring& s);
    void fill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);
    void triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha);
    void blend(int x, int y, uint32_t rgb, float a);
    void saber(int cx, int y, int half_w);   // a glowing lightsaber-blue line
    void hgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1);
    // The settings page's band behind the open section and the chosen row: saber
    // blue fading from a0 at the left to a1 at the right, a bright blade before it
    void page_band(int x0, int y0, int x1, int y1, float a0, float a1, float alpha);
    void vgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1);
    void frame(int x0, int y0, int x1, int y1, int t, uint32_t rgb, float alpha);
    void pill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);
    std::vector<std::wstring> wrap(Family f, int px, const std::wstring& s, int max_w);

    // Pictures of the menus (res/menu/, or your own of the same name in
    // third_party/menu/): the backdrop, the title logo over it, the credit
    struct Picture {
        std::vector<uint32_t> px;   // BGRA, straight alpha
        int w = 0, h = 0;
    };
    Picture backdrop_, logo_, credit_;
    void picture(const Picture& p, int x0, int y0, int x1, int y1, float anchor_x = 0.5f);   // covering the box, opaque
    void sprite(const Picture& p, int x0, int y0, int w, int h);      // with its transparency
    // The gamepad's buttons in the key help (res/pad/<button>.png, CC0); w = 0:
    // none, a drawn disc or pill instead
    std::map<std::wstring, Picture> pad_icons_;
    const Picture& pad_icon(const std::wstring& button);
    // One entry of the key help at (x, y), h high: the gamepad's button when
    // pad_prompts (else the key, in a keycap), then what it does. x moves past
    // it. prompt_width: how far it moves x
    void prompt(int& x, int y, int h, const wchar_t* pad_button, const wchar_t* key, const wchar_t* what);
    int prompt_width(int h, const wchar_t* pad_button, const wchar_t* key, const wchar_t* what);
    void backdrop_background();
    // Main, Play, Debug and the pause: a column at the right under the logo
    void draw_side_menu(const std::vector<Item>& list);
    void shade_backdrop();            // darker at the right and the bottom, the logo: on px_
    // OPTIONS, SETTINGS (over the paused game) and RECORDS, as Sega Rally 2's page:
    // the sections in a column at the left, the open one's rows in their groups,
    // a panel at the right with what the chosen row is and does, the keys at the bottom
    void draw_options_page(const std::vector<Item>& list, bool over_game);
    // The settings page's bands glide from one row (or section) to the next
    struct Glide {
        float from = 0, to = 0, from_h = 0, to_h = 0;
        double t0 = -1;
        long key = -1;
    };
    Glide glide_tab_, glide_row_;
    bool gliding_ = false;            // one of them is moving (this picture)
    void glide(Glide& g, long key, int y, int h, int& gy, int& gh);
    // RESTART THE GAME?: leaving the settings with something changed that only
    // applies on the next start (ANTI-ALIASING): RESTART NOW, or LATER (back as usual)
    bool restart_open_ = false;
    int restart_button_ = 0;   // 0 RESTART NOW, 1 LATER
    RECT restart_buttons_[2]{};
    std::wstring restart_list() const;   // what waits for a restart, "" nothing
    void restart_choose(int button);
    void draw_restart();
    // BACK TO PC EDITION (ARCADE ORIGINAL's pause): the same window, with this
    // note instead of the list (saved already; LATER: the pause again)
    std::wstring restart_note_;
    // RESTORE ORIGINAL (ARCADE ORIGINAL's pause): the same window asks RESTORE / CANCEL
    // first; RESTORE puts arcade_base() back, then RESTART THE GAME? if one of them needs it
    bool restore_ask_ = false;
    // ARCADE ORIGINAL's settings: the running ones while it runs, else the other experience's
    FrontendSettings& arcade_side() { return arcade ? settings : other; }
    const FrontendSettings& arcade_side() const { return arcade ? settings : other; }
    // A setting's list of values, over the description panel: Enter (A) or a click on
    // the row away from its arrows opens it. Each entry holds the settings as they
    // would be with that value, made by stepping the row with activate() from its
    // first value; picking one applies them. RESOLUTION: the monitor's modes, largest first
    struct DropEntry {
        std::wstring label;
        FrontendSettings s, other;   // settings and other with this value
    };
    bool drop_open_ = false;
    Id drop_id_ = OPT_DISPLAY;            // the row the list belongs to
    std::vector<DropEntry> drop_;
    Action drop_action_ = Action::SettingsChanged;   // what picking one pushes (as activate does)
    int drop_sel_ = 0, drop_top_ = 0, drop_in_use_ = -1;   // the highlighted one, the first shown, the value now
    std::vector<RECT> drop_rects_;
    RECT panel_rect_{};                   // the settings page's description panel (draw_options_page)
    // The entry of `resolutions` RESOLUTION stands on: the chosen mode (never
    // the window's measured inside), else the nearest to what is in use
    int resolution_index() const;
    void first_value(Id id);   // the row's setting at the first of its values (the list's order)
    bool open_drop(Id id);     // false: not a row with a list (fewer than two values)
    void drop_pick(int i);
    bool drop_key(int vk);
    void draw_drop();

    // The splash screens: their pictures, which one is on and since when
    std::vector<Picture> splash_;
    int splash_on_ = 0;
    double splash_from_ = 0;
    double skip_from_ = -1;     // hurried out at this time (-1: not)
    float skip_level_ = 1;      // from this brightness
    bool skip_next_ = false;    // pressed again while hurrying: the next one goes too
    std::vector<uint32_t> splash_px_;
    int splash_px_on_ = -1, splash_px_w_ = 0, splash_px_h_ = 0;
    void skip_splash();
    bool draw_splash(std::vector<uint32_t>& out);   // false: the last one ended
    // The splash video, when there is one Windows can open: it replaces the pictures
    SplashVideo video_;
    bool video_on_ = false;
    std::vector<uint32_t> video_px_;
    int video_pw_ = 0, video_ph_ = 0;
    double video_end_ = -1;
    float video_level_ = 0;
    bool draw_splash_video(std::vector<uint32_t>& out);
    double menu_in_from_ = -1;   // after the splash: the main menu fading in from black since then

    Screen screen_ = Screen::None;
    bool options_from_pause_ = false;
    int debug_stage_ = -1;   // DEBUG: the stage whose phases are listed (-1: the list of stages)
    int selected_ = 0, hovered_ = -1;
    bool dirty_ = true;
    std::vector<Action> actions_;
    std::vector<RECT> hit_;   // item rectangles of the last draw
    // option rows: from `left` the left arrow, from `value` the value, from `right` the right arrow (left < 0: none)
    struct ValueArea { int left, value, right; };
    std::vector<ValueArea> values_;
    bool wrap_ = false;
    // The settings' sections (OPTION_TABS in frontend.cpp says where each option goes)
    enum { EXPERIENCE_TAB, DISPLAY_TAB, GRAPHICS_TAB, AUDIO_TAB, CONTROLS_TAB };
    static constexpr int TAB_COUNT = 5;
    static const wchar_t* const TAB_NAMES[TAB_COUNT];
    struct OptionTab {
        Id id;
        int tab;
        const wchar_t* group = nullptr;   // a heading over this row: the group it starts
    };
    static const OptionTab OPTION_TABS[];   // frontend.cpp: where each option goes
    static const wchar_t* group_of(Id id);   // the heading over that option's row, if it starts a group
    void set_tab(int t);
    int tab_ = 0;   // open section of the settings, or of the records
    // The sections of the page on: the settings', or ACHIEVEMENTS / STATS
    static constexpr int RECORD_TAB_COUNT = 2;
    static const wchar_t* const RECORD_TAB_NAMES[RECORD_TAB_COUNT];
    int tab_count() const { return screen_ == Screen::Records ? RECORD_TAB_COUNT : TAB_COUNT; }
    // Records: the rows of the open section (REC_ROW entries of items())
    struct RecordRow {
        std::wstring label, detail, value;
        bool lit = true;   // an achievement: unlocked
        int group = 0;     // RECORD_GROUPS: UNLOCKED, LOCKED, TOTALS, BEST STAGES
    };
    std::vector<RecordRow> record_rows() const;
    int scroll_ = 0;           // first row shown
    bool options_scroll_ = false;   // the open section's rows do not all fit (the last draw)
    RECT more_up_{}, more_down_{};   // clickable "more" arrows of a scrolled section
    std::vector<RECT> tab_rects_;   // the sections in the column at the left (clicks)
    int mouse_y_ = -1;   // the pointer, for the section under it

    std::vector<HANDLE> font_handles_;
    std::vector<std::vector<char>> font_data_;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::wstring, Mask> masks_;
    std::map<std::pair<int, int>, std::pair<int, int>> caps_;   // cap_rows by (family, px)
    HDC dc_ = nullptr;

    int w_ = 0, h_ = 0;
    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    uint32_t* px_ = nullptr;
    std::vector<uint32_t> bg_;   // main menu backdrop for the current size
    std::vector<uint32_t> bg_blur_;   // the same blurred and darker: behind the settings page and the records
    static bool load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h);
};
