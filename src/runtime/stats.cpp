// Records and achievements (stats.h), Star Wars Trilogy.
//
// The player's block of the game is at 0x221544: +4 the episode (bits 12-15:
// 1 Yavin, 2 Hoth, 3 Endor), +0x10 the score, +0x4C the enemies of the phase
// that have appeared and +0x4E those destroyed (the phase results' "enemy
// destroyed" ratio), +0x56 the shield and +0x58 its full value. Both counts
// start again at every phase. The mode byte 0x1BC85B is 0 in the attract, 1
// once a credit is taken, 4 at the mission select and 9 while playing; the top
// state 0x2210E8 runs each stage as a pair (init, run), see runtime.cpp.
// Each kind of stage works its phase results out in one of four functions
// (fn_3BEF8, fn_3D070, fn_3DEB4, fn_79DE0: the bonus for the ratio, the time
// and the shield into 0x23B4BC), entered once at the end of every phase.
//
// The game is read at every vblank on the game thread; what the window shows
// is a copy taken under a mutex. Debug runs (tests, --auto, the DEBUG jumps)
// count nothing.
#include "stats.h"
#include "inputs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "ppc_rt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

// Game RAM
constexpr uint32_t MODE = 0x1BC85B;           // 0 attract, 1 credit, 4 mission select, 6-9 playing (see swt_playing)
constexpr uint32_t SEQUENCE = 0x2210FC;       // the main loop's step: 2 game init, 3 the game (runtime.cpp)
constexpr uint32_t TOP_STATE = 0x2210E8;      // stage pairs 2/3 .. 12/13 (init, run)
constexpr uint32_t PLAYER = 0x221544;         // the player's block
constexpr uint32_t P_SCORE = PLAYER + 0x10, P_APPEARED = PLAYER + 0x4C, P_DESTROYED = PLAYER + 0x4E;
constexpr uint32_t P_SHIELD = PLAYER + 0x56, P_SHIELD_FULL = PLAYER + 0x58;
constexpr uint32_t FN_RESULTS[4] = {0x0003BEF8, 0x0003D070, 0x0003DEB4, 0x00079DE0};   // a phase's results
constexpr double FRAME_HZ = 57.524;
constexpr double MIN_STAGE_SECONDS = 20;    // a stage played through takes minutes

constexpr const char* STATS_FILE = "save/stats.ini";
constexpr const char* ACHIEVEMENTS_FILE = "save/achievements.ini";

int64_t frames_of(double seconds) { return int64_t(seconds * FRAME_HZ); }

const std::vector<AchievementInfo> TABLE = {
    {"first_kill", L"FIRST STRIKE", L"Destroy your first enemy", 1},
    {"ace", L"ACE PILOT", L"Destroy 100 enemies", 100},
    {"rogue_leader", L"ROGUE LEADER", L"Destroy 1,000 enemies", 1000},
    {"yavin", L"GREAT SHOT, KID", L"Clear the Battle of Yavin", 1},
    {"hoth", L"ECHO BASE EVACUATED", L"Clear the Battle of Hoth", 1},
    {"endor", L"SHIELD GENERATOR DOWN", L"Clear the Battle of Endor", 1},
    {"trilogy", L"THE TRILOGY", L"Clear Yavin, Hoth and Endor", 3},
    {"duel", L"JEDI KNIGHT", L"Win a lightsaber duel", 1},
    {"total_destruction", L"TOTAL DESTRUCTION", L"Destroy every enemy of a phase", 1},
    {"untouchable", L"UNTOUCHABLE", L"Clear a phase without losing any shield", 1},
    {"close_call", L"CLOSE CALL", L"Clear a phase with 10% of the shield or less", 1},
    {"phases", L"SEASONED PILOT", L"Clear 25 phases", 25},
    {"score_500k", L"HERO OF THE REBELLION", L"Score 500,000 points in one game", 500000},
    {"score_1m", L"LEGEND OF THE REBELLION", L"Score 1,000,000 points in one game", 1000000},
    {"veteran", L"VETERAN", L"Play for 5 hours", frames_of(5 * 3600)},
};

int64_t raw_progress(const Stats& s, const std::string& k) {
    if (k == "first_kill" || k == "ace" || k == "rogue_leader") return s.kills;
    if (k == "yavin") return s.stage[0].clears;
    if (k == "hoth") return s.stage[1].clears;
    if (k == "endor") return s.stage[2].clears;
    if (k == "trilogy") return (s.stage[0].clears > 0) + (s.stage[1].clears > 0) + (s.stage[2].clears > 0);
    if (k == "duel") return s.stage[4].clears + s.stage[5].clears;
    if (k == "total_destruction") return s.perfect_phases;
    if (k == "untouchable") return s.untouched_phases;
    if (k == "close_call") return s.close_calls;
    if (k == "phases") return s.phases_cleared;
    if (k == "score_500k" || k == "score_1m") return s.best_score;
    if (k == "veteran") return s.play_frames;
    return 0;
}

// ---- Files: "key = value" lines ----

struct Ini {
    std::vector<std::pair<std::string, int64_t>> rows;
    void put(const std::string& k, int64_t v) { rows.push_back({k, v}); }
    bool save(const char* path) const {
        std::string tmp = std::string(path) + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return false;
        for (auto& [k, v] : rows) fprintf(f, "%s = %lld\n", k.c_str(), (long long)v);
        bool ok = fclose(f) == 0;
#ifdef _WIN32
        return ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
#else
        return ok && rename(tmp.c_str(), path) == 0;
#endif
    }
    static Ini load(const char* path) {
        Ini ini;
        if (FILE* f = fopen(path, "r")) {
            char line[256], key[128];
            long long v;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, " %127[^= ] = %lld", key, &v) == 2) ini.put(key, v);
            fclose(f);
        }
        return ini;
    }
};

// Every field of Stats under its name in save/stats.ini
template <class F>
void each_field(Stats& s, F&& f) {
    f("games", s.games), f("play_frames", s.play_frames), f("kills", s.kills), f("appeared", s.appeared);
    f("phases_cleared", s.phases_cleared), f("best_score", s.best_score);
    static const char* const KEYS[STAT_STAGES] = {"yavin", "hoth", "endor", "space2", "duel1", "duel2"};
    for (int i = 0; i < STAT_STAGES; i++) {
        std::string p = std::string(KEYS[i]) + "_";
        Stats::Stage& st = s.stage[i];
        f(p + "clears", st.clears), f(p + "best_destroyed", st.best_destroyed), f(p + "best_frames", st.best_frames);
    }
    f("perfect_phases", s.perfect_phases), f("untouched_phases", s.untouched_phases), f("close_calls", s.close_calls);
}

// ---- Tracking ----

// ---- save/achievements.json: the table with the progress and unlock times ----
// Written for the launcher (NicoLauncher), which shows them on the game's page
// without knowing this game's table; the format is in its docs/manifest.md.
// Nothing reads it back: save/achievements.ini stays the game's own record.

void json_string(std::string& out, const wchar_t* w) {
    out += '"';
    for (; *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", unsigned(c));
            out += b;
        } else if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | ((c >> 12) & 0x0F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    out += '"';
}

void write_achievements_json(const Stats& s, const std::vector<int64_t>& unlocked) {
    std::string out = "{\n  \"schema\": 1,\n  \"achievements\": [\n";
    for (size_t i = 0; i < TABLE.size(); i++) {
        const AchievementInfo& a = TABLE[i];
        int64_t p = std::clamp<int64_t>(raw_progress(s, a.key), 0, a.goal);
        int64_t when = i < unlocked.size() ? unlocked[i] : 0;
        if (when) p = a.goal;
        out += "    {\"id\": \"" + std::string(a.key) + "\", \"name\": ";
        json_string(out, a.name);
        out += ", \"description\": ";
        json_string(out, a.text);
        out += ", \"goal\": " + std::to_string(a.goal) + ", \"progress\": " + std::to_string(p);
        if (std::string(a.key) == "veteran") {   // play time, counted in frames
            int minutes = int(double(p) / FRAME_HZ / 60);
            char b[48];
            snprintf(b, sizeof b, "%dh %02dm / %dh", minutes / 60, minutes % 60, int(double(a.goal) / FRAME_HZ / 3600 + 0.5));
            out += std::string(", \"progress_text\": \"") + b + "\"";
        }
        out += ", \"unlocked\": " + std::to_string(when) + "}";
        out += i + 1 < TABLE.size() ? ",\n" : "\n";
    }
    out += "  ]\n}\n";
    const char* path = "save/achievements.json";
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp.c_str(), path) == 0;
#endif
    if (!ok) printf("[achievements] could not save %s\n", path);
}

struct Tracker {
    bool on = false;
    std::mutex mutex;   // everything below: the window reads copies
    Stats stats;
    std::vector<int64_t> unlocked;
    std::vector<int> fresh;   // unlocked, not yet announced

    // Game thread
    uint64_t frame = 0;
    bool rebase = true;          // read the game again without counting the jump (start, game state loaded)
    bool in_game = false, cheated = false;
    int mode = 0;
    uint32_t state = 0;
    int destroyed = 0, appeared = 0, shield = 0;
    // The stage on (its run state) and the phase on
    int stage = -1;
    uint64_t stage_from = 0;
    int stage_phases = 0;        // phases cleared in it
    bool phase_hurt = false;
    uint64_t saved_at = 0;
    bool dirty = false;

    bool counting() const { return in_game && !cheated; }

    void load() {
        Ini ini = Ini::load(STATS_FILE);
        each_field(stats, [&](const std::string& k, auto& field) {
            for (auto& [key, v] : ini.rows)
                if (key == k) field = decltype(+field)(v);
        });
        unlocked.assign(TABLE.size(), 0);
        Ini a = Ini::load(ACHIEVEMENTS_FILE);
        for (size_t i = 0; i < TABLE.size(); i++)
            for (auto& [key, v] : a.rows)
                if (key == TABLE[i].key) unlocked[i] = v;
    }

    void save() {
        Ini ini;
        each_field(stats, [&](const std::string& k, auto& field) { ini.put(k, int64_t(field)); });
        Ini a;
        for (size_t i = 0; i < TABLE.size(); i++)
            if (unlocked[i]) a.put(TABLE[i].key, unlocked[i]);
        if (!ini.save(STATS_FILE) || !a.save(ACHIEVEMENTS_FILE)) printf("[achievements] could not save save/stats.ini\n");
        write_achievements_json(stats, unlocked);
        saved_at = frame;
        dirty = false;
    }

    // With the mutex held
    void check_achievements() {
        for (size_t i = 0; i < TABLE.size(); i++)
            if (!unlocked[i] && raw_progress(stats, TABLE[i].key) >= TABLE[i].goal) {
                unlocked[i] = int64_t(time(nullptr));
                fresh.push_back(int(i));
                dirty = true;
                printf("[achievements] unlocked: %ls\n", TABLE[i].name);
            }
    }

    static int stage_of(uint32_t st) { return st >= 3 && st <= 13 && (st & 1) ? int(st - 3) / 2 : -1; }

    // The end of a phase (its results being worked out), on the game thread
    void phase_end(int arg) {
        std::lock_guard<std::mutex> lock(mutex);
        int sh = int16_t(rd16(P_SHIELD)), full = std::max(1, int(int16_t(rd16(P_SHIELD_FULL))));
        int ap = int16_t(rd16(P_APPEARED)), de = int16_t(rd16(P_DESTROYED));
        printf("[achievements] phase results (%d): destroyed %d of %d, shield %d/%d%s\n", arg, de, ap, sh, full,
               counting() ? "" : " (not counted)");
        if (counting() && sh > 0 && stage >= 0) {
            stats.phases_cleared++;
            stage_phases++;
            if (ap > 0) {
                int pm = int(std::min<int64_t>(1000, int64_t(de) * 1000 / ap));
                stats.stage[stage].best_destroyed = std::max(stats.stage[stage].best_destroyed, pm);
                if (de >= ap) stats.perfect_phases++;
            }
            if (!phase_hurt) stats.untouched_phases++;
            if (sh * 10 <= full) stats.close_calls++;
            dirty = true;
            check_achievements();
        }
        phase_hurt = false;
    }

    void stage_end(bool cleared) {
        if (stage < 0) return;
        int frames = int(frame - stage_from);
        if (cleared && counting() && stage_phases > 0 && frames >= frames_of(MIN_STAGE_SECONDS)) {
            Stats::Stage& st = stats.stage[stage];
            st.clears++;
            if (st.best_frames == 0 || frames < st.best_frames) st.best_frames = frames;
            dirty = true;
            printf("[achievements] %ls cleared in %.1f s (%d phases)\n", STAT_STAGE_NAMES[stage], frames / FRAME_HZ,
                   stage_phases);
        }
        stage = -1;
    }

    void frame_tick() {
        frame++;
        int md = rd8(MODE);
        uint32_t st = rd32(TOP_STATE);
        bool playing = swt_playing();
        g_swt_duel = playing && st >= 10 && st <= 13 && !getenv("SWT_DUEL_ORIGINAL");
        g_swt_top_state = playing ? int(st) : -1;
        int de = int16_t(rd16(P_DESTROYED)), ap = int16_t(rd16(P_APPEARED)), sh = int16_t(rd16(P_SHIELD));
        int score = int(rd32(P_SCORE));
        std::lock_guard<std::mutex> lock(mutex);
        if (rebase) {   // nothing counted across the jump
            rebase = false;
            mode = md, state = st, destroyed = de, appeared = ap, shield = sh;
            return;
        }
        if (md != mode) printf("[achievements] mode %d -> %d (state %u)\n", mode, md, st);
        {   // the stage script's step (0x221136), for the DEBUG menu's phase table
            static int step_was = -1;
            int step = int16_t(rd16(0x221136));
            if (playing && step != step_was)
                printf("[stage] state %u step %d (shield %d, destroyed %d of %d)\n", st, step, sh, de, ap);
            step_was = step;
        }
        if (mode == 0 && md != 0) {   // a credit taken: a new game
            in_game = true;
            cheated = false;
            stats.games++;
            dirty = true;
        }
        if (st != state) {
            printf("[achievements] state %u -> %u (mode %d, shield %d)\n", state, st, md, shield);
            if (stage_of(state) >= 0) stage_end(shield > 0 && md != 0);
            if (stage_of(st) >= 0) {
                stage = stage_of(st);
                stage_from = frame;
                stage_phases = 0;
                phase_hurt = false;
            }
        }
        if (playing && counting()) {
            stats.play_frames++;
            // The counts only go up during a phase; a fall is the next phase starting from zero
            if (de > destroyed && de - destroyed < 20) stats.kills += de - destroyed;
            if (ap > appeared && ap - appeared < 20) stats.appeared += ap - appeared;
            if (sh < shield) phase_hurt = true;
            stats.best_score = std::max(stats.best_score, score);
            dirty = true;
        }
        if (in_game && md == 0) {   // back in the attract: the game is over
            stage_end(false);
            in_game = cheated = false;
        }
        mode = md, state = st, destroyed = de, appeared = ap, shield = sh;
        check_achievements();
        if (dirty && frame - saved_at > uint64_t(frames_of(30))) save();
    }
};

Tracker g;

}  // namespace

const std::vector<AchievementInfo>& achievement_table() { return TABLE; }

int64_t achievement_progress(const Stats& s, int index) {
    const AchievementInfo& a = TABLE[size_t(index)];
    return std::clamp<int64_t>(raw_progress(s, a.key), 0, a.goal);
}

void stats_init() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.load();
    write_achievements_json(g.stats, g.unlocked);   // for the launcher, from the first launch of a new build
    g.on = true;
    g.rebase = true;
    for (uint32_t fn : FN_RESULTS) g_trace_bits[(fn >> 5) & 511] |= uint8_t(1 << ((fn >> 2) & 7));
}

void stats_save() {
    if (!g.on) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.save();
}

StatsView stats_view() {
    std::lock_guard<std::mutex> lock(g.mutex);
    StatsView v{g.stats, g.unlocked};
    if (v.unlocked.size() != TABLE.size()) v.unlocked.assign(TABLE.size(), 0);
    return v;
}

std::vector<int> stats_take_new_achievements() {
    std::lock_guard<std::mutex> lock(g.mutex);
    std::vector<int> r;
    r.swap(g.fresh);
    return r;
}

void stats_frame() {
    if (g.on) g.frame_tick();
}

void stats_enter(Ctx& c, uint32_t fn) {
    if (!g.on) return;
    for (uint32_t f : FN_RESULTS)
        if (fn == f) g.phase_end(int(c.r[3]));
}

void stats_game_cheated() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.cheated = true;
}

// A game being played. The mode byte is 9 in most games, but some run with 6
// (issue #24: an F9 snapshot of Boba Fett's duel at mode 6, top state 11),
// and the mode byte left from a game stays 6-9 through the attract's title
// and rankings, so the main loop's sequence must be at the game (3) too
bool swt_playing() {
    int md = rd8(MODE);
    return md >= 6 && md <= 9 && rd32(SEQUENCE) == 3;
}
