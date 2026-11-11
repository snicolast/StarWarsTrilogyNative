// PC front end: main menu (Play, Options, Quit) shown at launch instead of
// the arcade attract loop, and the pause menu Esc opens during the game.
//
// Drawn on the CPU into a full-window BGRA picture (premultiplied alpha) with
// the OFL fonts in third_party/fonts and the game's yellow-to-orange letter
// colours; video/ui_overlay.h puts it on screen. Driven by the keyboard
// (arrows, Enter, Esc), by the mouse/gun (point and click, wheel) and by the
// gamepads (app.cpp turns what they send into keys).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../runtime/stats.h"
#include "news.h"
#include "splash_video.h"

struct FrontendSettings {
    bool fullscreen = false;
    int aspect = 1;    // widest 3D view: 0 4:3 (original), 1 16:9, 2 21:9, 3 32:9, 4 48:9 (MAX_ASPECT in settings_bridge.cpp)
    int volume = 5;    // 0..10 (output gain): the sound effects and voices
    int music = 5;     // 0..10: the music board's gain
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 6;  // dB on the rear pair: 0, 3, 6, 9, 12
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
#ifdef __APPLE__
    int aa = 1;        // 0 MSAA 4x, 1 MSAA 8x, 2 off (takes effect on the next launch)
#else
    int aa = 0;        // 0 MSAA, 1 DLAA, 2 off (takes effect on the next launch)
#endif
    int supersample = 100;   // the 3D drawn at this % of the window and scaled down (macOS menu: 100, 150, 200)
    bool haze = true;  // distance haze
    int bloom = 2;     // glow around bright things: 0 off, 1 low, 2 medium, 3 high
    bool ao = true;    // ambient occlusion
    bool sky = true;   // procedural sky instead of the original picture
    bool sun = true;   // a sun in the procedural sky
    bool shadows = true;   // sun shadows
    int ray_tracing = 2;   // traced through the world (GPUs with ray tracing): 0 off, 1 sun shadows, 2 also ambient occlusion
    bool shafts = true;    // light shafts from the sun
    bool soft = true;      // soft particles
    bool explosions = true;    // EXPLOSIONS: ENHANCED (new fireballs, sparks and debris; Yavin)
    bool hdr = true;       // HDR lighting
    bool relief = true;    // surface detail from the textures
    bool sharpen = true;   // contrast adaptive sharpening
    bool hd_text = true;
    bool crosshair = true;
    bool smooth = true;     // the 3D drawn between game frames at the monitor's refresh rate
    bool second_mouse = true;   // a second mouse is player 2's gun
    int saturation = 80;    // colour saturation in % (100 = original; 80 by default)
    int gamma = 100;        // gamma x 100 (100 = original, higher = brighter)
    int pad_speed = 5;      // gamepad sight speed with the sticks, 1..10
    bool gyro = false;      // the gamepad's gyroscope moves the sight (CONTROLS > GYRO AIM turns it on)
    int gyro_speed = 5;     // 1..10
    int vibration = 2;      // gamepad vibration: 0 off, 1 low, 2 medium, 3 high
    int aim_assist = 1;     // gamepad aim assist: 0 off, 1 low, 2 medium, 3 high
    bool pad_lever = false; // the gamepad's sticks are the flight lever (back to the centre when let go)
    int pad_invert = 0;     // the sticks' axes turned round: 0 off, 1 x, 2 y, 3 both
    int duel_invert = 0;    // the aim turned round in the lightsaber duels only (mouse, light gun, sticks): 0 off, 1 x, 2 y, 3 both
    bool splash = true;     // the logo video at launch (splash_screens = 0: straight to the menu)
    bool menu_music = true; // the game's Main Title in the menus
    int sight_size = 80;    // the game's sight drawn at this size, % (Star Wars Trilogy)
    bool pointer_sight = true; // the sight goes where the mouse / light gun points (false: the mouse is the lever)
    std::string lost_world_path;   // MORE RECOMPS: where The Lost World is (its folder or LostWorld.exe; "" = look for it)
    std::string sega_rally_2_path; // MORE RECOMPS: where Sega Rally 2 is (its folder or SegaRally2.exe; "" = look for it)
    bool check_updates = true;     // at launch, news.txt on GitHub: a window with what is new and UPDATE NOW
    int news_seen = 0;             // the newest news.txt entry the player closed with LATER

    // save/settings.ini; missing keys keep their current value
    void load(const char* path);
    // QUALITY: the graphics options that cost the most, set together.
    // quality() is the level they match, CUSTOM if none
    enum Quality { LOW, MEDIUM, HIGH, ULTRA, CUSTOM };
    void set_quality(int q);
    int quality() const;
    void save(const char* path) const;
};

class Frontend {
public:
    enum class Screen { None, Splash, Main, Play, Options, Pause, Records, Debug, Extra, More };
    enum class Action { None, StartArcade, Resume, Quit, ToggleFullscreen, SettingsChanged,
                        QuitToMenu, StartDebug };

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

    FrontendSettings settings;
    int aa_at_launch = 0;   // anti-aliasing the renderer was started with
    bool rt_available = false;   // the GPU can trace rays (RAY TRACING option)
    std::wstring gamepads;  // the pads connected, for the CONTROLS tab ("" = none)
    bool gyro_pad = false;  // one of them has a gyroscope
    bool pad_prompts = false;   // a gamepad drives the menu: its buttons in the key help and hints, not the keyboard's

    // At launch: news.txt from GitHub in the background (when the settings
    // allow it). An entry newer than the news.txt here and than the last one
    // closed with LATER opens a window over the main menu listing every entry
    // this version lacks (see news.h)
    void check_for_news();

    // DEBUG (Star Wars Trilogy): straight to a phase of a stage. StartDebug
    // reads debug_choice = stage * 4 + phase (runtime.cpp, debug jumps)
    int debug_choice = -1;
    bool starting = false;    // PLAY pressed: the game still gets to its mission select (a notice instead of the hint)

    // RECORDS screen (runtime/stats.h): the window keeps this copy up to date while it is open
    StatsView records;

    // Redraws into `out` when something changed (or the size did); true if it did
    bool draw(int w, int h, std::vector<uint32_t>& out);
    void invalidate() { dirty_ = true; }

private:
    enum Id {
        PLAY, OPTIONS, QUIT, ARCADE, BACK, RESUME, SETTINGS, QUIT_TO_MENU, TABS,
        RECORDS, DEBUG, DEBUG_STAGE, DEBUG_PHASE, MORE_GAMES, LOST_WORLD, SEGA_RALLY_2,
        OPT_QUALITY, OPT_DISPLAY, OPT_VOLUME, OPT_MUSIC, OPT_SPEAKERS, OPT_REAR, OPT_SAMPLES, OPT_AA, OPT_HAZE, OPT_BLOOM, OPT_AO, OPT_SKY, OPT_SUN, OPT_SHADOWS, OPT_SHAFTS, OPT_SOFT, OPT_HDR, OPT_RELIEF, OPT_SHARPEN, OPT_TEXT, OPT_CROSS, OPT_MICE,
        OPT_SAT, OPT_GAMMA, OPT_MOTION, OPT_PAD_SPEED, OPT_GYRO, OPT_GYRO_SPEED, OPT_VIBRATION, OPT_ASSIST, OPT_RT, OPT_MENU_MUSIC, EXTRA, DOGFIGHT, OPT_POINTER, OPT_SIGHT, OPT_PAD_AIM, OPT_PAD_INVERT, OPT_DUEL_INVERT, OPT_UPDATES, OPT_SCALE,
        OPT_ASPECT, OPT_EXPLOSIONS,
    };
    struct Item {
        Id id;
        std::wstring label, value, hint;
        bool enabled = true;
        int index = -1;   // DEBUG_STAGE / DEBUG_PHASE: which one
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
    enum Family { TITLE, ITEM, MONO, JURASSIC };
    // Titles and menu entries: the Jurassic Park font when it is installed
    Family display() const { return jurassic_ ? JURASSIC : TITLE; }

    std::vector<Item> items() const;
    void activate(const Item& it, int dir);
    void back();
    int index_of(Id id) const;   // the entry with that id in the current screen (0 if none)
    void push(Action a) { actions_.push_back(a); }
    void select(int index);

    HFONT font(Family f, int px);
    const Mask& mask(Family f, int px, const std::wstring& text, int edge_px);
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, const Style& st);
    int measure(Family f, int px, const std::wstring& s);
    void fill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);
    void band(int cx, int y0, int y1, int half_w, uint32_t rgb, float alpha);
    void triangle(int x, int y, int size, int dir, uint32_t rgb, float alpha);
    void blend(int x, int y, uint32_t rgb, float a);
    void background();
    void saber(int cx, int y, int half_w);   // a glowing lightsaber-blue line
    void hgrad(int x0, int y0, int x1, int y1, uint32_t rgb, float a0, float a1);
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
    bool has_backdrop() const { return backdrop_.w > 0; }
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
    // Main, Play, Boss, Survival and Debug on the backdrop: a column at the right under the logo
    void draw_side_menu(const std::vector<Item>& list);
    // MORE RECOMPS: snicolast's other ports (The Lost World, Sega Rally 2),
    // in the order of RECOMPS in frontend.cpp. The selected one's art
    // (res/menu/more/) on the left of the backdrop; ENTER plays it when it is
    // on this PC, else opens its page
    static constexpr int RECOMP_COUNT = 2;
    Picture more_backdrop_[RECOMP_COUNT], more_logo_[RECOMP_COUNT];
    std::vector<uint32_t> bg_more_[RECOMP_COUNT];   // their backdrops of MORE RECOMPS for the current size
    std::wstring more_exe_[RECOMP_COUNT];           // its .exe found on this PC ("" = not found)
    int more_art_ = 0;                              // whose art MORE RECOMPS shows (the selected one, or the last)
    static int recomp_of(Id id);                    // its index, -1 for an entry that is not one
    int selected_recomp() const;                    // MORE RECOMPS: the selected one, -1 on BACK
    std::string& recomp_path(int r);                // its settings key (folder or .exe)
    void find_recomp(int r);
    void open_recomp_page(int r);
    void play_recomp(int r);
    RECT more_logo_rect(int r) const;               // where its logo goes on the backdrop
    void shade_backdrop();            // darker at the right and the bottom, the logo: on px_
    // UPDATE AVAILABLE: the window over the main menu with the entries of
    // news.txt this version lacks. UPDATE NOW starts update.bat (it waits for the game to close,
    // updates, rebuilds and starts it again) and closes the game; LATER (Esc)
    // keeps it closed until a newer entry
    struct NewsDownload {                  // shared with the thread that downloads it (left to finish on its own)
        std::mutex mutex;
        std::string text;                  // news.txt from GitHub
        std::atomic<int> state{0};         // 0 downloading, 1 there, 2 failed, 3 taken (take_news)
    };
    std::shared_ptr<NewsDownload> news_download_;
    std::vector<NewsEntry> news_;          // the entries shown
    int news_latest_ = 0;                  // the newest one on GitHub
    bool news_open_ = false;
    int news_button_ = 0;                  // 0 UPDATE NOW, 1 LATER
    int news_hot_ = -1;                    // the button under the pointer
    RECT news_buttons_[2]{};
    void take_news();                      // the download, once it is there
    void draw_news();
    bool news_key(int vk);
    void news_choose(int button);
    void start_update();

    RECT coffee_rect_{};   // the donation link (main menu)
    bool coffee_hot_ = false, coffee_focus_ = false;
    void open_coffee();

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
    struct ValueArea { int left, middle; };   // option rows: value column (left < 0: none)
    std::vector<ValueArea> values_;
    bool wrap_ = false;
    // Settings tabs (tab_of in frontend.cpp says where each option goes)
    enum { DISPLAY_TAB, LIGHTING_TAB, AUDIO_TAB, CONTROLS_TAB, GAME_TAB };   // LIGHTING_TAB is shown as GRAPHICS
    static constexpr int TAB_COUNT = 4;   // (GAME_TAB: not shown in Star Wars)
    static const wchar_t* const TAB_NAMES[TAB_COUNT];
    static int tab_of(Id id);
    struct OptionTab { Id id; int tab; };
    static const OptionTab OPTION_TABS[];   // frontend.cpp: where each option goes
    void set_tab(int t);
    int tab_ = 0;   // open tab of the settings, or of the records
    // The tab row of the screen on: settings tabs, or ACHIEVEMENTS / STATS
    static constexpr int RECORD_TAB_COUNT = 2;
    static const wchar_t* const RECORD_TAB_NAMES[RECORD_TAB_COUNT];
    int tab_count() const { return screen_ == Screen::Records ? RECORD_TAB_COUNT : TAB_COUNT; }
    const wchar_t* tab_name(int t) const { return screen_ == Screen::Records ? RECORD_TAB_NAMES[t] : TAB_NAMES[t]; }
    // Records: the rows of the open tab and the first one shown
    struct RecordRow {
        std::wstring label, detail, value;
        bool lit = true;   // an achievement: unlocked
    };
    std::vector<RecordRow> record_rows() const;
    static constexpr int VISIBLE_RECORDS = 7;
    int record_scroll_ = 0;
    void scroll_records(int by);
    void draw_records(int x0, int x1, int y0, int row_h);
    // A settings tab shows this many options at most; the rest scroll
    static constexpr int VISIBLE_OPTIONS = 6;
    int scroll_ = 0;           // first option shown
    RECT more_up_{}, more_down_{};   // clickable "more" arrows of a scrolled tab
    std::vector<RECT> tab_rects_;   // a step past the end starts again (clicks on the label)
    int mouse_x_ = -1, mouse_y_ = -1;

    std::vector<HANDLE> font_handles_;
    std::vector<std::vector<char>> font_data_;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::wstring, Mask> masks_;
    HDC dc_ = nullptr;
    bool jurassic_ = false;   // third_party/fonts/Jurassic_Park.ttf loaded

    int w_ = 0, h_ = 0;
    float u_ = 1;   // pixels per unit of a 1920x1080 layout
    uint32_t* px_ = nullptr;
    std::vector<uint32_t> bg_;   // main menu backdrop for the current size
    std::vector<uint32_t> bg_stone_;   // with a poster: its stone only, without the logo (options)
    // Optional poster (third_party/menu/cover.png, not in the repository):
    // portrait picture with the logo at the top and plain dark stone below.
    // It becomes the backdrop and the title; its stone continues at the sides.
    std::vector<uint32_t> poster_;
    int poster_w_ = 0, poster_h_ = 0;
    bool load_poster(const std::wstring& path);
    static bool load_image(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h);
    bool has_poster() const { return poster_w_ > 0; }
    void poster_background(bool stone_only, std::vector<uint32_t>& dst);
    void poster_layout(float& scale, float& y0) const;
};
