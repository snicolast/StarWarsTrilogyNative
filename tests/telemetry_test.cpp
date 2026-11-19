// OGS-TS telemetry (telemetry.cpp): the real sender run over a fake game RAM, its datagrams caught by the test
// seam. Checks the envelope and the 1,400-byte budget, that a hit on the shield sends an event while a steady
// shield, one that fills up, and a new stage do not, and the craft's pose (read back with a small Cap'n Proto
// reader: the vendored writer only writes): its attitude and position, and forces that stay calm through a
// cruise and the jump to the next section of the track.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../third_party/ogs_telemetry/OgsTelemetryFrame.h"
#include "ppc_rt.h"
#include "stats.h"
#include "telemetry.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// ---- the game's RAM, as telemetry.cpp reads it ------------------------------------------------------

static std::vector<uint8_t> g_memory(RAM_SIZE);
uint8_t* g_ram = g_memory.data();
uint8_t rt_rd8_slow(uint32_t) { return 0; }
uint16_t rt_rd16_slow(uint32_t) { return 0; }
uint32_t rt_rd32_slow(uint32_t) { return 0; }
void rt_wr8_slow(uint32_t, uint8_t) {}
void rt_wr16_slow(uint32_t, uint16_t) {}
void rt_wr32_slow(uint32_t, uint32_t) {}

constexpr uint32_t MODE = 0x1BC85B, SEQUENCE = 0x2210FC, TOP_STATE = 0x2210E8, EPISODE = 0x221548, STAGE_STEP = 0x221136;
constexpr uint32_t PLAYER = 0x221544, P_SCORE = PLAYER + 0x10, P_SHIELD = PLAYER + 0x56, P_SHIELD_FULL = PLAYER + 0x58;

// stats.cpp's own, which the test does not link (it reads the same two bytes: a game being played)
bool swt_playing() { return rd8(MODE) >= 6 && rd8(MODE) <= 9 && rd32(SEQUENCE) == 3; }

constexpr uint32_t MATRIX = 0x2214A8, POSITION = 0x2214CC;
constexpr uint32_t OBJECT_POINTER = 0x22B084, POINTED = 0x226808;   // Yavin's play: the object the pointer names

static void put_f(uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    wr32(a, u);
}

// The craft's transform: heading h (0 is +z, positive toward +x) at (x, y, z). The matrix is the world -> view
// rotation, whose rows are the craft's right, up and forward in the world
static void put_object(uint32_t at, float h, float x, float y, float z) {
    const float r[9] = {std::cos(h), 0, -std::sin(h), 0, 1, 0, std::sin(h), 0, std::cos(h)};
    for (int i = 0; i < 9; i++) put_f(at + 4 * uint32_t(i), r[i]);
    put_f(at + 0x24, x), put_f(at + 0x28, y), put_f(at + 0x2C, z);
}
static void put_pose(float h, float x, float y, float z) { put_object(MATRIX, h, x, y, z); }

// A game in Yavin's first stage with this much shield of 1000
// (the stage script is on the step a phase plays in, unless `step` says another)
static void set_game(int shield, uint32_t top = 3, uint32_t episode = 1, int step = -1) {
    wr8(MODE, 9), wr32(SEQUENCE, 3), wr32(TOP_STATE, top), wr32(EPISODE, episode << 12);
    wr16(STAGE_STEP, uint16_t(step >= 0 ? step : (top == 3 || top == 9) ? 11 : 10));
    wr16(P_SHIELD, uint16_t(int16_t(shield))), wr16(P_SHIELD_FULL, 1000), wr32(P_SCORE, 12345);
}
static void set_menus() { wr8(MODE, 4), wr32(SEQUENCE, 2); }

// ---- the datagrams ----------------------------------------------------------------------------------

struct Datagram {
    uint8_t type;
    std::vector<uint8_t> data;
};
static std::vector<Datagram> g_sent;
static void sink(const uint8_t* d, size_t n) { g_sent.push_back({n > 4 ? d[4] : uint8_t(0xFF), std::vector<uint8_t>(d, d + n)}); }

static void begin() {
    telemetry_test_begin(sink);
    g_sent.clear();
    set_menus();
}
static void frames(int n) {
    for (int i = 0; i < n; i++) telemetry_frame(true, 40123);
}

// ---- a motion frame's pose and forces -----------------------------------------------------------------

static uint64_t word(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
static int32_t offset(uint64_t w) { return int32_t(uint32_t(w) & ~3u) / 4; }   // a pointer's signed word offset

struct Struct {   // a struct in a message: its data bytes and its pointers
    const uint8_t* data = nullptr;
    size_t data_bytes = 0;
    const uint8_t* ptrs = nullptr;
    bool ok = false;
    float f32(int at) const {   // a float at a bit offset (zero where trailing zero words are not sent)
        uint32_t u = 0;
        if (size_t(at / 8 + 4) <= data_bytes) memcpy(&u, data + at / 8, 4);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
};
// The struct a pointer points to
static Struct struct_at(const uint8_t* ptr) {
    Struct s;
    const uint64_t w = word(ptr);
    if (w == 0 || (w & 3) != 0) return s;
    s.data = ptr + 8 + 8 * offset(w);
    s.data_bytes = 8 * size_t(uint16_t(w >> 32));
    s.ptrs = s.data + s.data_bytes;
    s.ok = true;
    return s;
}

struct Motion {
    bool ok = false;
    float pos[3] = {}, quat[4] = {}, force[3] = {}, rate[3] = {};
    int dynamics = 0;   // 0 none, 2 bodyDynamics
};
// The first object of a motion frame: root -> objects list -> its position, orientation and dynamics
static Motion read_motion(const Datagram& d) {
    Motion m;
    if (d.type != 1 || d.data.size() < 24) return m;
    const uint8_t* seg = d.data.data() + 16;
    const uint64_t w = word(seg);                                  // the root pointer: the MotionFrame
    const uint8_t* frame = seg + 8 + 8 * offset(w);
    const uint8_t* objects = frame + 8 * uint16_t(w >> 32);        // its first pointer, after the data words
    const uint64_t lw = word(objects);                             // a composite list of MotionObject
    if ((lw & 3) != 1 || ((lw >> 32) & 7) != 7) return m;
    const uint8_t* tag = objects + 8 + 8 * offset(lw);
    const uint64_t t = word(tag);
    const uint8_t* first = tag + 8;                                // the first object
    const uint8_t* data = first;
    const uint8_t* ptrs = first + 8 * uint16_t(t >> 32);
    m.dynamics = uint16_t(t >> 32) ? int(*reinterpret_cast<const uint16_t*>(data)) : 0;   // the union's discriminant (no data words sent: none)
    const Struct p = struct_at(ptrs + 8 * 1), q = struct_at(ptrs + 8 * 2);
    for (int i = 0; i < 3; i++) m.pos[i] = p.ok ? p.f32(32 * i) : 0.0f;
    for (int i = 0; i < 4; i++) m.quat[i] = q.ok ? q.f32(32 * i) : 0.0f;
    if (m.dynamics == 2) {
        const Struct body = struct_at(ptrs + 8 * 3);               // bodyDynamics: specific force, angular velocity
        const Struct f = body.ok ? struct_at(body.ptrs) : Struct{};
        for (int i = 0; i < 3; i++) m.force[i] = f.ok ? f.f32(32 * i) : 0.0f;
        const Struct r = body.ok ? struct_at(body.ptrs + 8) : Struct{};
        for (int i = 0; i < 3; i++) m.rate[i] = r.ok ? r.f32(32 * i) : 0.0f;
    }
    m.ok = true;
    return m;
}
static Motion last_motion() {
    for (size_t i = g_sent.size(); i-- > 0;)
        if (g_sent[i].type == 1) return read_motion(g_sent[i]);
    return Motion{};
}
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static int count(uint8_t type) {
    int n = 0;
    for (const Datagram& d : g_sent) n += d.type == type;
    return n;
}

int main() {
    {   // the envelope and the size budget, a discovery before the motion
        begin();
        set_game(1000);
        frames(3);
        CHECK(!g_sent.empty() && g_sent[0].type == 0);
        CHECK(count(1) == 3);
        for (const Datagram& d : g_sent) {
            CHECK(d.type == 0 || d.type == 1 || d.type == 2);
            CHECK(d.data.size() >= 16 && d.data.size() <= 1400);
            CHECK(memcmp(d.data.data(), "OMT1", 4) == 0 && d.data[5] == 0 && d.data[6] == 0 && d.data[7] == 0);
        }
    }
    {   // the menus: the stream goes on, nothing happens
        begin();
        frames(20);
        CHECK(count(1) == 20 && count(2) == 0);
    }
    {   // a hit on the shield sends an event (re-sent for a moment); steady play sends none
        begin();
        set_game(1000);
        frames(30);
        CHECK(count(2) == 0);
        set_game(900);
        frames(10);
        CHECK(count(2) >= 1);
        for (const Datagram& d : g_sent) CHECK(d.data.size() <= 1400);
    }
    {   // the shield filling up (a new life, a power-up) is not a hit
        begin();
        set_game(500);
        frames(20);
        set_game(1000);
        frames(20);
        CHECK(count(2) == 0);
    }
    {   // a new stage sets the shield again: nothing carried across
        begin();
        set_game(1000, 3);
        frames(20);
        set_game(400, 5, 2);
        frames(20);
        CHECK(count(2) == 0);
    }
    {   // shots in a stream are one hit while they go on (an event rides along 100 ms, then it is over)
        begin();
        set_game(1000);
        frames(10);
        for (int i = 1; i <= 8; i++) {
            set_game(1000 - 20 * i);
            frames(1);
        }
        frames(10);
        CHECK(count(2) >= 1);
    }
    {   // leaving the game drops a hit still being measured
        begin();
        set_game(1000);
        frames(10);
        set_game(900);
        frames(1);
        set_menus();
        frames(10);
        CHECK(count(2) == 0);
    }
    {   // the pose: the attitude turns +z to the craft's forward, +x to its right; the position goes as it is
        for (float heading : {0.0f, 0.8f, -1.2f, 3.0f}) {
            begin();
            set_game(1000);
            put_pose(heading, 12.0f, 3.0f, -7.0f);
            frames(4);
            const Motion m = last_motion();
            CHECK(m.ok && m.dynamics == 2);
            CHECK(near(m.pos[0], 12.0f, 1e-3f) && near(m.pos[1], 3.0f, 1e-3f) && near(m.pos[2], -7.0f, 1e-3f));
            const ogs::telemetry::Quat q{m.quat[0], m.quat[1], m.quat[2], m.quat[3]};
            CHECK(near(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W, 1.0f, 1e-3f));
            const ogs::telemetry::Vec3 forward = ogs::telemetry::Rotate(q, {0, 0, 1});
            const ogs::telemetry::Vec3 right = ogs::telemetry::Rotate(q, {1, 0, 0});
            const ogs::telemetry::Vec3 up = ogs::telemetry::Rotate(q, {0, 1, 0});
            CHECK(near(forward.X, std::sin(heading), 2e-3f) && near(forward.Y, 0.0f, 2e-3f) && near(forward.Z, std::cos(heading), 2e-3f));
            CHECK(near(right.X, std::cos(heading), 2e-3f) && near(right.Z, -std::sin(heading), 2e-3f));
            CHECK(near(up.Y, 1.0f, 2e-3f));
        }
    }
    {   // no motion where there is no craft to read: the menus, the duels, memory that is not a rotation
        begin();
        put_pose(0.5f, 1.0f, 2.0f, 3.0f);
        frames(3);
        CHECK(last_motion().ok && last_motion().dynamics == 0);       // the menus
        set_game(1000, 11);
        frames(3);
        CHECK(last_motion().dynamics == 0);                           // a duel
        set_game(1000, 3);
        put_f(MATRIX, 3.0f);                                          // a row that is not a unit vector
        frames(3);
        CHECK(last_motion().dynamics == 0);
        put_pose(0.5f, 1.0f, 2.0f, 3.0f);
        put_f(MATRIX + 4 * 4, 2.0f);                                  // a row at right angles to the others but twice as long
        frames(3);
        CHECK(last_motion().dynamics == 0);
        put_pose(0.5f, 1.0f, 2.0f, 3.0f);
        put_f(POSITION, 1e30f);                                       // a position out of any stage
        frames(3);
        CHECK(last_motion().dynamics == 0);
        put_pose(0.5f, 1.0f, 2.0f, 3.0f);
        frames(3);
        CHECK(last_motion().dynamics == 2);                           // and back in a flight
    }
    {   // two objects can hold the pose: the fixed one (Hoth, Endor, Yavin's cutscenes) and the one the pointer names
        // (Yavin's play, while the fixed one stands still). The one that moved last is followed, the pointed-to one
        // when both move
        begin();
        set_game(1000);
        put_pose(0.2f, 5.0f, 5.0f, 5.0f);                            // the fixed object, standing still
        wr32(OBJECT_POINTER, POINTED);
        for (int i = 0; i < 12; i++) {
            put_object(POINTED, 0.4f, 100.0f, 2.0f, 300.0f + float(i));
            frames(1);
        }
        Motion m = last_motion();
        CHECK(m.dynamics == 2 && near(m.pos[0], 100.0f, 1e-2f) && near(m.pos[2], 311.0f, 1e-2f));   // the pointed-to one

        for (int i = 0; i < 12; i++) {                                // now the fixed one moves and the pointed-to one stops
            put_pose(0.2f, 5.0f, 5.0f, 5.0f + float(i));
            frames(1);
        }
        m = last_motion();
        CHECK(near(m.pos[0], 5.0f, 1e-2f) && near(m.pos[2], 16.0f, 1e-2f));                        // the fixed one

        for (int i = 0; i < 6; i++) {                                 // both move: the pointed-to one
            put_pose(0.2f, 5.0f, 5.0f, 100.0f + float(i));
            put_object(POINTED, 0.4f, 100.0f, 2.0f, 500.0f + float(i));
            frames(1);
        }
        m = last_motion();
        CHECK(near(m.pos[0], 100.0f, 1e-2f) && near(m.pos[2], 505.0f, 1e-2f));

        wr32(OBJECT_POINTER, 0x7FFFFF0);                              // a pointer into nothing: the fixed one
        for (int i = 0; i < 6; i++) {
            put_pose(0.2f, 5.0f, 5.0f, 200.0f + float(i));
            frames(1);
        }
        m = last_motion();
        CHECK(m.dynamics == 2 && near(m.pos[0], 5.0f, 1e-2f) && near(m.pos[2], 205.0f, 1e-2f));
        wr32(OBJECT_POINTER, 0);
    }
    {   // the cinematics send no motion: the script is on the step a phase plays in, or it is not
        struct Stage { uint32_t top; int plays[3]; };
        const Stage stages[] = {{3, {11, 19, 27}}, {5, {10, 18, 24}}, {7, {10, 18, 24}}, {9, {11, 21, 28}}};
        for (const Stage& st : stages)
            for (int step = 0; step <= 40; step++) {
                bool plays = false;
                for (int p : st.plays) plays = plays || p == step;
                begin();
                set_game(1000, st.top, 1, step);
                put_pose(0.2f, 1.0f, 2.0f, 3.0f);
                frames(3);
                const Motion m = last_motion();
                CHECK(m.ok && (m.dynamics == 2) == plays);
            }
        begin();                                                      // a game going from the cinematic into play
        set_game(1000, 3, 1, 5);
        put_pose(0.2f, 1.0f, 2.0f, 3.0f);
        frames(3);
        CHECK(last_motion().dynamics == 0);
        set_game(1000, 3, 1, 11);
        frames(3);
        CHECK(last_motion().dynamics == 2);
    }
    {   // forces: a cruise reads as gravity's reaction and nothing else; a jump to the next section of the track
        // (a hundred metres in a frame) starts them again from rest instead of being an acceleration
        begin();
        set_game(1000);
        float z = 0.0f, worst = 0.0f;
        bool jumped = false;
        for (int i = 0; i < 90; i++) {
            z += 1.0f;                                                // 60 m/s
            if (i == 45) z += 150.0f, jumped = true;
            put_pose(0.0f, 0.0f, 2.0f, z);
            frames(1);
            const Motion m = last_motion();
            if (jumped) worst = std::fmax(worst, std::sqrt(m.force[0] * m.force[0] + m.force[1] * m.force[1] + m.force[2] * m.force[2]));
        }
        const Motion m = last_motion();
        CHECK(near(m.force[1], 9.80665f, 0.5f) && near(m.force[0], 0.0f, 0.5f) && near(m.force[2], 0.0f, 0.5f));
        CHECK(worst < 40.0f);                                         // gravity's 10, a little more while it settles
    }
    {   // the game moves the craft every second frame in some phases (the position steps, stands, steps): the forces
        // are worked out between the changes, not frame by frame, which would read as an acceleration swinging
        // between full speed and none. And a pose that stops is at rest
        begin();
        set_game(1000);
        float z = 0.0f, worst = 0.0f;
        for (int i = 0; i < 180; i++) {
            if (i % 2 == 0) z += 2.0f;                                // 60 m/s, in steps of 2 m every other frame
            put_pose(0.0f, 0.0f, 2.0f, z);
            frames(1);
            if (i > 60) {
                const Motion m = last_motion();
                worst = std::fmax(worst, std::sqrt(m.force[0] * m.force[0] + m.force[1] * m.force[1] + m.force[2] * m.force[2]));
            }
        }
        Motion m = last_motion();
        CHECK(near(m.force[1], 9.80665f, 0.5f) && near(m.force[0], 0.0f, 0.5f) && near(m.force[2], 0.0f, 0.5f));
        CHECK(worst < 15.0f);                                         // gravity's 10, with a little noise from the steps
        for (int i = 0; i < 30; i++) frames(1);                       // the pose stands still
        m = last_motion();
        CHECK(near(m.force[1], 9.80665f, 0.5f) && near(m.force[0], 0.0f, 0.5f) && near(m.force[2], 0.0f, 0.5f));
        CHECK(m.dynamics == 2);
    }
    {   // the craft starting at speed from a standstill, and a cut the jump limits let through (22 m with a turn of
        // 0.24 rad in one frame): neither is flying, so neither becomes a force or a turning rate of that size
        begin();
        set_game(1000);
        float worst_force = 0.0f, worst_rate = 0.0f, z = 0.0f;
        auto watch = [&] {
            const Motion m = last_motion();
            worst_force = std::fmax(worst_force, std::sqrt(m.force[0] * m.force[0] + m.force[1] * m.force[1] + m.force[2] * m.force[2]));
            worst_rate = std::fmax(worst_rate, std::sqrt(m.rate[0] * m.rate[0] + m.rate[1] * m.rate[1] + m.rate[2] * m.rate[2]));
        };
        for (int i = 0; i < 20; i++) {                                // standing
            put_pose(0.0f, 0.0f, 2.0f, 0.0f);
            frames(1);
        }
        for (int i = 0; i < 60; i++) {                                // then at once 120 m/s
            z += 2.0f;
            put_pose(0.0f, 0.0f, 2.0f, z);
            frames(1);
            watch();
        }
        CHECK(worst_force < 40.0f);
        worst_force = 0.0f;
        for (int i = 0; i < 40; i++) {                                // a cut in a cruise
            z += 1.0f + (i == 20 ? 22.0f : 0.0f);
            put_pose(i < 20 ? 0.0f : 0.24f, 0.0f, 2.0f, z);
            frames(1);
            watch();
        }
        CHECK(worst_force < 40.0f);
        CHECK(worst_rate < 3.0f);
        worst_rate = 0.0f;
        for (int i = 0; i < 40; i++) {                                // a cut of the attitude alone, the position carrying on
            z += 1.0f;
            put_pose(i < 20 ? 0.24f : 0.54f, 0.0f, 2.0f, z);
            frames(1);
            watch();
        }
        CHECK(worst_rate < 3.0f);
    }
    {   // turned off: nothing more is sent
        begin();
        set_game(1000);
        frames(2);
        size_t sent = g_sent.size();
        telemetry_frame(false, 40123);
        telemetry_frame(false, 40123);
        CHECK(g_sent.size() == sent);
    }
    telemetry_test_begin(nullptr);
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    printf("telemetry: all correct\n");
    return 0;
}
