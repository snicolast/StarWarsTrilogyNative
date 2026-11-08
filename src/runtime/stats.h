// Records and achievements (Star Wars Trilogy): what the player has done over
// all games, saved in save/stats.ini, and local achievements
// (save/achievements.ini) announced with a toast and listed in the RECORDS
// screen of the main menu.
//
// The game thread reads the player's block of the game at every vblank (its
// score, shield and the counts of the phase results screen: enemies that
// appeared and enemies destroyed) and is told when a phase's results are
// worked out (stats_enter); see stats.cpp. The window thread reads a copy of
// everything to draw.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Ctx;

// Stages by the game's top state pair (0x2210E8: 2/3 Yavin, 4/5 Hoth, 6/7
// Endor, 8/9 the second space stage, 10/11 and 12/13 the duels)
constexpr int STAT_STAGES = 6;
inline constexpr const wchar_t* STAT_STAGE_NAMES[STAT_STAGES] = {L"YAVIN", L"HOTH", L"ENDOR", L"SPACE STAGE 2",
                                                                 L"DUEL STAGE 1", L"DUEL STAGE 2"};

struct Stats {
    // Totals
    int64_t games = 0, play_frames = 0;
    int64_t kills = 0, appeared = 0;    // enemies destroyed, of those that appeared
    int64_t phases_cleared = 0;
    int best_score = 0;                 // in one game
    // Per stage, played through in arcade mode
    struct Stage {
        int clears = 0;
        int best_destroyed = -1;        // per mille of a phase's enemies, best phase; -1 none yet
        int best_frames = 0;            // whole stage; 0 none yet
    } stage[STAT_STAGES];
    int perfect_phases = 0;             // every enemy destroyed
    int untouched_phases = 0;           // cleared without losing any shield
    int close_calls = 0;                // cleared with 10% of the shield or less
};

// Achievement table (stats.cpp): progress is read from Stats
struct AchievementInfo {
    const char* key;          // in save/achievements.ini
    const wchar_t* name;
    const wchar_t* text;      // what it asks
    int64_t goal;             // progress needed (1 for yes / no ones)
};
const std::vector<AchievementInfo>& achievement_table();
int64_t achievement_progress(const Stats& s, int index);   // capped at the goal

// Window thread
void stats_init();                  // a normal launch: tracking on, files loaded (tests leave it off)
void stats_game_cheated();          // DEBUG jumps: the game on counts nothing more
struct StatsView {
    Stats stats;
    std::vector<int64_t> unlocked;  // per achievement: unix time, 0 locked
};
StatsView stats_view();
std::vector<int> stats_take_new_achievements();   // unlocked since the last call, for the toast
void stats_save();                  // at exit (it also saves by itself every 30 s of play)

// Game thread
void stats_frame();                 // every vblank
bool swt_playing();                 // a game being played (mode byte 6-9 and the game's sequence step)
void stats_enter(Ctx& c, uint32_t fn);   // entry of a hooked function
