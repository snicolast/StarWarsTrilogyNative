// Player controls. Written by the window (keyboard/mouse), the pads or by
// --auto, and read by the emulated hardware at 0xF0040000. Star Wars Trilogy
// has one analog flight lever: p[0] is the player; p[1] only has the second
// coin slot and start button the I/O board still reads.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// gun_x/gun_y hold the lever's ADC values (0x80 = centred), trigger is the
// lever's trigger and offscreen its second (thumb) button
struct PlayerInputs {
    std::atomic<bool> coin{false}, start{false}, trigger{false}, offscreen{false}, event{false};
    std::atomic<int> gun_x{128}, gun_y{128};   // raw ADC values, 0..255
};

struct Inputs {
    PlayerInputs p[2];
    std::atomic<bool> service{false}, test{false};

    // F0040004, active low. Bank 0: bit0 coin 1, bit1 coin 2, bit2 test,
    // bit3 service, bit4 start 1, bit5 start 2 (fn_001180A8 packs bit 4 with
    // player 1's port and bit 5 with player 2's). Bank 1: bit5 = EEPROM (set by runtime)
    uint8_t system_port(bool bank1) const {
        uint8_t v = 0xFF;
        if (!bank1) {
            if (p[0].coin) v &= ~0x01;
            if (p[1].coin) v &= ~0x02;
            if (test) v &= ~0x04;
            if (service) v &= ~0x08;
            if (p[0].start) v &= ~0x10;
            if (p[1].start) v &= ~0x20;
        }
        return v;
    }
    // F0040008: bit5 the lever's trigger, bit4 its second trigger, bit0 the
    // event button (the second player's port, F004000C, is not read here)
    uint8_t player_port() const {
        uint8_t v = 0xFF;
        if (p[0].trigger) v &= ~0x20;
        if (p[0].offscreen) v &= ~0x10;
        if (p[0].event) v &= ~0x01;
        return v;
    }
    // ADC channels: 0 = lever Y, 1 = lever X (raw values; the window maps the mouse)
    uint8_t adc(int channel) const {
        return channel == 0 ? uint8_t(p[0].gun_y) : channel == 1 ? uint8_t(p[0].gun_x) : 0x80;
    }
};

extern Inputs g_inputs;

// Aim of a player as a fraction of the screen (outside 0..1 = off the
// screen): sets the lever's ADC values (player 1's; a second pad's aim is
// kept but read by nothing). The mouse and the pads all aim through here.
void set_player_aim(int player, float x, float y);
// Star Wars Trilogy: the shakes of the lever's force feedback since the
// pads last took them (0..1; runtime.cpp, from the drive board's motors)
extern std::atomic<float> g_ffb_shake;
extern std::atomic<float> g_sight_scale;   // Star Wars Trilogy: the game's sight drawn at this size (SIGHT SIZE)
extern std::atomic<bool> g_swt_duel;   // Star Wars Trilogy: a lightsaber duel (top state 10..13): the lever turned round
extern std::atomic<int> g_duel_invert;   // OPTIONS > CONTROLS > INVERT IN DUELS: 0 off, 1 x, 2 y, 3 both (the duels only)
extern std::atomic<int> g_swt_top_state;   // Star Wars Trilogy: the top state (0x2210E8) while playing, -1 otherwise
extern std::atomic<int> g_swt_phase_results;   // Star Wars Trilogy: how many phase results screens have begun (stats.cpp)
struct PlayerAim { float x, y; bool inside; };
PlayerAim player_aim(int player);
// Star Wars Trilogy: how far the lever's exact position (axis 0 = x, 1 = y)
// is from the 0..255 step the game reads (-0.5..0.5; 0 at the ends or when
// something else set the lever)
float lever_fraction(int axis);
double sight_reach(int axis);   // how much farther the sight goes at full tilt, drawn smaller (aim_hooks.cpp)
extern std::atomic<bool> g_fixed_aim;    // --aim: the ADC values stay fixed

// Which player a pad controls (0 or 1), given the connected pads as a bit
// mask: the first connected pad shares player 1 with the mouse, whenever it
// was plugged in, and the others are player 2's
int pad_player(int pad_index, uint32_t connected);

// A pad's coin / start, held for 0.1 s: driven by the clock, so whoever polls
// the controls (the window at the monitor's rate) calls join_update() each time
void join_update();
void press_coin();    // player 1's coin / start for 0.1 s (pads)
void press_start();

// --auto "coin@1500,start@1600": scheduled presses (6 frames each); a "2"
// suffix is player 2's (coin2, start2, trigger2, reload2) and so is aim2:X/Y
struct AutoInput {
    struct Event { std::string name; uint64_t frame; };
    std::vector<Event> events;
    void parse(const std::string& s);
    void apply(uint64_t frame);
};
