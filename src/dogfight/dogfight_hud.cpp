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
    int text(int x, int y, const char* s, uint32_t color) {
        for (int pass = 0; pass < 2; pass++) {
            int at = x;
            for (const char* p = s; *p; p++, at += 6) {
                for (const DogfightGlyph& g : DOGFIGHT_GLYPHS) {
                    if (g.c != *p) continue;
                    for (int r = 0; r < 7; r++)
                        for (int c = 0; c < 5; c++) {
                            if (g.rows[r][c] != '#') continue;
                            if (pass) dot(at + c, y + r, color);
                            else box(at + c - 1, y + r - 1, at + c + 2, y + r + 2, 0xFF000000u);
                        }
                    break;
                }
            }
        }
        return x + 6 * int(strlen(s));
    }
};

// BGRA, opaque: the arcade HUD's green and yellow
constexpr uint32_t GREEN = 0xFF30E040u, YELLOW = 0xFFF0E020u, DIM = 0xFF104018u, EDGE = 0xFF000000u;

}  // namespace

void dogfight_draw_hud(uint32_t* front, int w, int h) {
    if (dogfight_cockpit_on()) return;   // the cockpit's screen shows them instead
    Canvas cv{front, w, h};
    // Bottom left: SPEED and its number, the throttle as a bar under it
    int x = 22, y = h - 44;
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
