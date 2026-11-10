// DOGFIGHT's HUD, drawn into the game's own 2D front layer (496 x 384, as the
// arcade's score and shield are), so it is scaled and filtered with the rest
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dogfight.h"
#include "dogfight_font.h"

namespace {

struct Canvas {
    uint32_t* px;
    int w, h;
    void dot(int x, int y, uint32_t color) {
        if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = color;
    }
    void box(int x0, int y0, int x1, int y1, uint32_t color) {   // filled, x1 and y1 not included
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) dot(x, y, color);
    }
    void frame(int x0, int y0, int x1, int y1, uint32_t color) {
        box(x0, y0, x1, y0 + 1, color), box(x0, y1 - 1, x1, y1, color);
        box(x0, y0, x0 + 1, y1, color), box(x1 - 1, y0, x1, y1, color);
    }
    // Letters with a dark edge, so they read over bright ships; returns the x after the text
    int text(int x, int y, const char* s, uint32_t color, int size = 1) {
        for (int pass = 0; pass < 2; pass++) {
            int at = x;
            for (const char* p = s; *p; p++, at += 6 * size) {
                for (const DogfightGlyph& g : DOGFIGHT_GLYPHS) {
                    if (g.c != *p) continue;
                    for (int r = 0; r < 7; r++)
                        for (int c = 0; c < 5; c++) {
                            if (g.rows[r][c] != '#') continue;
                            if (pass) box(at + c * size, y + r * size, at + (c + 1) * size, y + (r + 1) * size, color);
                            else box(at + c * size - 1, y + r * size - 1, at + (c + 1) * size + 1, y + (r + 1) * size + 1, 0xFF000000u);
                        }
                    break;
                }
            }
        }
        return x + 6 * size * int(strlen(s));
    }
};

// BGRA, opaque: the arcade HUD's green and yellow
constexpr uint32_t GREEN = 0xFF30E040u, YELLOW = 0xFFF0E020u, DIM = 0xFF104018u, EDGE = 0xFF000000u;

}  // namespace

void dogfight_draw_hud(uint32_t* front, int w, int h) {
    Canvas cv{front, w, h};
    // How the match went, across the middle (over the cockpit too), with the
    // player's kills and the time it took
    if (const int result = dogfight_result()) {
        const char* words = result == DOGFIGHT_VICTORY ? "VICTORY" : "DEFEATED";
        cv.text((w - 6 * 4 * int(strlen(words))) / 2, h / 2 - 60, words, YELLOW, 4);
        char line[48];
        const int took = int(dogfight_took());
        snprintf(line, sizeof(line), "KILLS %d   TIME %d:%02d", dogfight_kills(), took / 60, took % 60);
        cv.text((w - 6 * int(strlen(line))) / 2, h / 2 - 22, line, GREEN);
    }
    if (dogfight_cockpit_on()) return;   // the cockpit's screen shows the rest instead
    // Bottom left: the fighters left on each side, the shield, SPEED and its
    // number, the throttle as a bar under it
    int x = 22, y = h - 68;
    {
        char left[16];
        snprintf(left, sizeof(left), "%d", dogfight_left(false));
        int after = cv.text(cv.text(x, y, "X-WING", GREEN) + 6, y, left, YELLOW);
        snprintf(left, sizeof(left), "%d", dogfight_left(true));
        cv.text(cv.text(after + 12, y, "TIE", GREEN) + 6, y, left, YELLOW);
        if (const float theirs = dogfight_target_shield(); theirs >= 0) {   // the locked enemy's shield
            snprintf(left, sizeof(left), "%03d", int(theirs * 100 + 0.5f));
            cv.text(cv.text(x + 132, y, "TARGET", GREEN) + 6, y, left, YELLOW);
        }
    }
    y += 12;
    {
        char shield[24];
        snprintf(shield, sizeof(shield), "%03d", int(dogfight_shield() * 100 + 0.5f));
        cv.text(cv.text(x, y, "SHIELD", GREEN) + 6, y, shield, YELLOW);
        if (dogfight_warning()) cv.text(x + 90, y, "WARNING", YELLOW);
    }
    y += 12;
    int after = cv.text(x, y, "SPEED", GREEN);
    char number[16];
    snprintf(number, sizeof(number), "%03d", std::clamp(int(dogfight_speed() + 0.5f), 0, 999));
    cv.text(after + 6, y, number, YELLOW);
    y += 12;
    after = cv.text(x, y, "THROTTLE", GREEN);
    const int bar_x = after + 6, bar_w = 60;
    cv.box(bar_x - 1, y - 1, bar_x + bar_w + 1, y + 8, EDGE);
    cv.box(bar_x, y, bar_x + bar_w, y + 7, DIM);
    cv.box(bar_x, y, bar_x + int(float(bar_w) * dogfight_speed_part() + 0.5f), y + 7, YELLOW);
    const int mark = bar_x + int(float(bar_w - 2) * std::clamp(dogfight_throttle(), 0.0f, 1.0f) + 0.5f);
    cv.box(mark, y - 1, mark + 2, y + 8, GREEN);   // where the throttle asks the speed to be
    cv.frame(bar_x - 1, y - 1, bar_x + bar_w + 1, y + 8, GREEN);
}
