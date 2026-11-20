// OGS-TS telemetry (telemetry.h). Game RAM, as stats.cpp reads it:
//
//   0x2210E8  top state: the stages run as pairs (init, run), 10-13 are the lightsaber duels
//   0x221136  the stage script's step (16 bits). A phase plays in one step (Yavin 11, 19, 27; Hoth and Endor 10, 18,
//             24; the second space stage 11, 21, 28: the table of the DEBUG menu, seen on long runs of all three
//             stages) and the steps between are the opening cinematic, the closing scene and the next phase's setup
//   0x221548  bits 12-15 the episode: 1 Yavin, 2 Hoth, 3 Endor
//   [the player's block, 0x221544]
//      +0x10  score (32 bits)
//      +0x56  shield, +0x58 its full value (signed 16 bits)
//   The craft's (camera's) transform, found on RAM recordings of Yavin, Hoth and Endor. An object is
//     +0x00  3x3 floats, row by row: the world -> view rotation. Its rows are the craft's right, up and forward in
//            the world, so the game's frame is OGS's own (left-handed, +x right, +y up, +z forward)
//     +0x24  3 floats: the position in the world (the next 3, 0x38 on, are -R * P, the view's translation)
//   and there are two of them: one at 0x2214A8 (Hoth and Endor fly with it, and Yavin's cutscenes use it) and, in
//   Yavin's play, the one that the pointer at 0x22B084 points to (0x226808), while the first stands still. When both
//   move, as in the Death Star's trench, they are the same. The sender follows the one that moved last.
//   The position jumps when the game moves on to the next section of the track, and each stage opens with a
//   cinematic where the camera moves by itself.
//
// A game being played is swt_playing() (stats.cpp). The units are taken to be metres and the game's frame
// 1/60 s, as in Sega Rally 2 (the speeds, 50 to 220 a second, are those of the craft in metres).
#include "telemetry.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../../third_party/ogs_telemetry/OgsTelemetryFrame.h"
#include "../../third_party/ogs_telemetry/OgsTelemetryWire.h"
#include "ppc_rt.h"
#include "stats.h"

namespace {
using namespace ogs::telemetry;

#ifdef _WIN32
using Sock = SOCKET;
const Sock NO_SOCK = INVALID_SOCKET;
using SockLen = int;
void sock_close(Sock s) { closesocket(s); }
#else
using Sock = int;
const Sock NO_SOCK = -1;
using SockLen = socklen_t;
void sock_close(Sock s) { ::close(s); }
#endif

// Game RAM
constexpr uint32_t TOP_STATE = 0x2210E8, EPISODE = 0x221548, STAGE_STEP = 0x221136;
constexpr uint32_t PLAYER = 0x221544;
constexpr uint32_t P_SCORE = PLAYER + 0x10, P_SHIELD = PLAYER + 0x56, P_SHIELD_FULL = PLAYER + 0x58;

constexpr uint32_t FIXED_OBJECT = 0x2214A8, OBJECT_POINTER = 0x22B084;
constexpr uint32_t OBJ_POSITION = 0x24;   // from the object's start, which is its rotation
// The stages 2-9 are the flights; 10-13 are the lightsaber duels, whose object has not been looked at
constexpr uint32_t FIRST_DUEL = 10;

constexpr float G = 9.80665f;
constexpr float GAME_HZ = 60.0f;          // the game's frames a second in its own time
// The game does not always move the craft every frame: in Yavin's play it does every second one (the positions
// step, stand, step, stand), so the forces are worked out between changes, over the time they took. A pose that has
// not changed for this many frames is standing still (a cutscene held, a stage between phases)
constexpr int STILL_FRAMES = 6;
// Between two readings the velocity and the turning rate change by what the flying does (on recordings 99% of
// the velocity changes are under 200 m/s2 and of the turn-rate changes under 31 rad/s2); a change past these
// is a cut, or the craft starting at speed from a standstill, and the forces start again from rest
constexpr float MAX_ACCEL = 1000.0f;      // m/s2
constexpr float MAX_ALPHA = 300.0f;       // rad/s2
// A frame that moves the craft or turns it more than this is the game moving on to another section of the
// track, not flying: the forces start again from rest. (The craft goes 1 to 10 a frame, in Yavin's trench up to
// 10, and the jumps are a hundred or more; a turn is 0.03 rad)
constexpr float MAX_STEP = 40.0f;         // m
constexpr float MAX_TURN = 0.5f;          // rad
constexpr float STREAM_HZ = 57.524f;      // the Model 3 vblank (runtime.cpp FRAME_HZ): a frame is sent each
constexpr const char* GROUP = "239.255.17.17";
constexpr double DISCOVERY_EVERY = 2.0;   // s, for late joiners
constexpr double EVENT_CARRY = 0.1;       // s, a sent event rides along until it is this old (C-10)

// A hit is each drop of the shield. Shots come in streams, so the drops of the next few frames are one hit,
// sent once with all of them in its strength
constexpr int HOLD = 3;                   // frames a hit is measured before it is sent
constexpr float HIT_MIN = 0.25f;          // intensity of the smallest hit
constexpr float HIT_PER_SHIELD = 5.0f;    // added intensity per whole shield lost (a fifth of it is full)

// Channel ids, in the order they are declared
enum : uint16_t { C_PAUSED, C_SHIELD, C_SCORE };

struct Craft {
    uint16_t type;          // ObjectType: 2 aircraft, 5 humanoid
    const char* label;      // what it is
    const char* location;   // where
};
Craft craft_of(bool playing, uint32_t top, uint32_t episode) {
    if (!playing) return {2, "", ""};
    if (top >= 10 && top <= 13) return {5, "Lightsaber duel", "Duel"};
    switch (episode) {
        case 1: return {2, "X-wing", "Yavin"};
        case 2: return {2, "Snowspeeder", "Hoth"};
        case 3: return {2, "Speeder bike", "Endor"};
    }
    return {2, "", ""};
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Whether the craft is being flown: the stage's script is on the step a phase plays in. Before it is the opening
// cinematic (22 s in Yavin, 28 in Hoth) where the camera moves by itself, and between the phases the closing scene
// and the set-up of the next one: scenes to watch, not to feel
bool flown(uint32_t top, int step) {
    switch (top) {
        case 3: return step == 11 || step == 19 || step == 27;    // Yavin
        case 5:                                                   // Hoth
        case 7: return step == 10 || step == 18 || step == 24;    // Endor
        case 9: return step == 11 || step == 21 || step == 28;    // the second space stage
    }
    return false;
}

float read_float(uint32_t a) {
    uint32_t u = rd32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// The unit quaternion of a rotation matrix m (v' = m v), whose columns are the body's axes in the world
Quat quat_from_matrix(const float m[3][3]) {
    Quat q;
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s};
    }
    const float n = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    return {q.X / n, q.Y / n, q.Z / n, q.W / n};
}

// The position and attitude of the object at `at`, from the game's RAM; false when what is there is not a rotation
// and a position (a stage not started, memory that is not the object)
bool read_pose(uint32_t at, Vec3& position, Quat& attitude) {
    float r[3][3];   // the rows: the craft's right, up and forward in the world
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            r[i][j] = read_float(at + 4 * uint32_t(3 * i + j));
            if (!std::isfinite(r[i][j])) return false;
        }
    auto dot = [&](int a, int b) { return r[a][0] * r[b][0] + r[a][1] * r[b][1] + r[a][2] * r[b][2]; };
    for (int i = 0; i < 3; i++) {
        if (std::fabs(dot(i, i) - 1.0f) > 0.01f) return false;
        for (int j = i + 1; j < 3; j++)
            if (std::fabs(dot(i, j)) > 0.01f) return false;
    }
    const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                      r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.5f) return false;   // a mirror, not a rotation
    const Vec3 p{read_float(at + OBJ_POSITION), read_float(at + OBJ_POSITION + 4), read_float(at + OBJ_POSITION + 8)};
    if (!std::isfinite(p.X) || !std::isfinite(p.Y) || !std::isfinite(p.Z) || std::fabs(p.X) > 1e6f || std::fabs(p.Y) > 1e6f ||
        std::fabs(p.Z) > 1e6f)
        return false;
    float m[3][3];   // the columns are the rows of r
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = r[j][i];
    position = p;
    attitude = quat_from_matrix(m);
    return true;
}

struct Publisher {
    Sock sock = NO_SOCK;
    bool wsa = false;
    sockaddr_in to{};
    int port = 0;
    std::chrono::steady_clock::time_point t0;
    int64_t session_us = 0;
    double last_discovery = -1e9;
    uint32_t generation = 0;
    std::string label, location;   // the craft and the place: the object's identity, a change is a new generation
    uint16_t type = 2;
    bool announced = false;        // a discovery went out with this generation
    WireBuilder wire;

    // The two objects the pose can be in, and how many frames each has stood still (bit for bit). The one that
    // moved last is the craft's: the other is the camera of a cutscene, or a craft that is not flown
    struct Source {
        uint32_t prev[12] = {};
        int still = 1 << 20;
    } sources[2];
    // The craft, frame to frame: its forces come from how the position and attitude change
    BodyDynamicsDeriver deriver;
    Vec3 pos{};
    Quat quat{0, 0, 0, 1};
    bool have_pose = false;
    int picked_still = 0;         // frames the pose just picked has stood still
    int since_change = 0;         // frames since the last new reading
    Vec3 prev_v{};                // the velocity and the turning rate between the last two readings
    float prev_rate = 0;
    bool have_motion = false;

    // The shield, frame to frame
    bool have_prev = false;
    int prev_shield = 0;
    uint32_t prev_top = 0;
    float pend_drop = 0;           // shield lost in a hit being measured (0 = none), as a part of the full shield
    int pend_age = 0;
    uint32_t next_event = 1;
    std::vector<std::pair<double, EventInput>> recent;

    bool ok() const { return sock != NO_SOCK; }

    bool open(int p) {
        if (ok() && port == p) return true;
        close();
#ifdef _WIN32
        if (!wsa) {
            WSADATA d;
            if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
            wsa = true;
        }
#endif
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!ok()) return false;
#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        DWORD ttl = 1, loop = 1;   // the local subnet, and receivers on this PC too
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
        unsigned char ttl = 1, loop = 1;   // BSD and macOS take a byte for these two (Linux takes a byte or an int)
#endif
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof loop);
        to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(uint16_t(p));
        inet_pton(AF_INET, GROUP, &to.sin_addr);
        route_by_default();
        port = p;
        session_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        t0 = std::chrono::steady_clock::now();
        last_discovery = -1e9;
        announced = false;
        have_prev = false;
        have_pose = false;
        deriver = BodyDynamicsDeriver{};
        sources[0] = sources[1] = Source{};
        pend_drop = 0;
        recent.clear();
        printf("[telemetry] OGS-TS to %s:%d\n", GROUP, p);
        return true;
    }
    // Name the default route's interface for the group instead of leaving it to the OS, which moves a group to
    // loopback once anything here has sent to it there (implementation-udp.md, network interfaces). Connecting a
    // UDP socket only looks up the route; 192.0.2.1 is reserved for documentation (RFC 5737)
    void route_by_default() {
        Sock probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe == NO_SOCK) return;
        sockaddr_in remote{}, local{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);
        SockLen len = sizeof local;
        if (connect(probe, reinterpret_cast<const sockaddr*>(&remote), sizeof remote) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&local), &len) == 0 && local.sin_addr.s_addr != htonl(INADDR_ANY) &&
            (ntohl(local.sin_addr.s_addr) >> 24) != 127)
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local.sin_addr), sizeof local.sin_addr);
        sock_close(probe);
    }
    void close() {
        if (ok()) sock_close(sock);
        sock = NO_SOCK;
        port = 0;
    }
    void send(const std::vector<uint8_t>& d) {
        if (g_sink) {
            g_sink(d.data(), d.size());
            return;
        }
        if (ok() && !d.empty())
            sendto(sock, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }

    void set_identity(const Craft& c) {
        if (c.label == label && c.location == location && c.type == type) return;
        label = c.label, location = c.location, type = c.type;
        if (announced) generation++;   // the craft is part of the descriptor set
        last_discovery = -1e9;
    }

    void send_discovery() {
        ObjectDecl obj;
        obj.Name = "player";
        obj.Type = type;
        obj.Location = location;
        obj.TypeLabel = label;
        obj.Primary = true;
        using T = schema::ChannelType;
        auto channel = [&](const char* name, const char* unit, T channel_type, const char* text) {
            ChannelDecl c;
            c.Name = name, c.Unit = unit, c.Type = channel_type, c.Description = text;
            obj.Channels.push_back(c);
        };
        channel("session.paused", "", T::Boolean, "Not in a game (attract, menus, mission select): a rig settles (C-11)");
        channel("player.shield", "", T::Number, "Shield left, 0..1");
        channel("game.score", "", T::Number, "Score of the game");
        obj.Channels[C_SHIELD].HasRange = true;
        obj.Channels[C_SHIELD].Min = 0, obj.Channels[C_SHIELD].Max = 1;
        Environment env{};
        env.Gravity = G;
        env.Medium = 1;   // air
        Instance instance;
        instance.MotionRateHz = STREAM_HZ;
        char host[256] = {};
        if (gethostname(host, sizeof host - 1) == 0) instance.Host = host;
        for (const auto& page : PaginateDiscovery(1, session_us, "Star Wars Trilogy", env, {obj}, generation, kDatagramBudget, instance)) send(page);
        announced = true;
    }

    // The pose of the object that moved last of the two; false when neither holds a pose. Called every frame of a
    // flight, so that how long each stood still is counted
    bool pick_pose(Vec3& position, Quat& attitude) {
        const uint32_t at[2] = {FIXED_OBJECT, rd32(OBJECT_POINTER)};
        Vec3 pos_of[2];
        Quat quat_of[2];
        bool valid[2] = {false, false};
        for (int i = 0; i < 2; i++) {
            if (at[i] < 0x100000 || at[i] >= RAM_SIZE - 0x100 || (at[i] & 3) != 0) continue;
            valid[i] = read_pose(at[i], pos_of[i], quat_of[i]);
            if (!valid[i]) continue;
            uint32_t words[12];
            for (int k = 0; k < 9; k++) words[k] = rd32(at[i] + 4 * uint32_t(k));
            for (int k = 0; k < 3; k++) words[9 + k] = rd32(at[i] + OBJ_POSITION + 4 * uint32_t(k));
            const bool moved = memcmp(words, sources[i].prev, sizeof words) != 0;
            sources[i].still = moved ? 0 : sources[i].still + 1;
            memcpy(sources[i].prev, words, sizeof words);
        }
        // Both moving, even out of step with each other, is a tie (the pointed-to one: it is the craft where the
        // two differ); one that has stood still for a while is not
        auto age = [&](int i) { return sources[i].still < STILL_FRAMES ? 0 : sources[i].still; };
        int pick = -1;
        for (int i = 1; i >= 0; i--)
            if (valid[i] && (pick < 0 || age(i) < age(pick))) pick = i;
        if (pick < 0) return false;
        position = pos_of[pick], attitude = quat_of[pick];
        picked_still = sources[pick].still;
        return true;
    }

    // A new reading of the craft's pose: the forces follow from the change since the last one, unless the game has
    // moved on to another section of the track (a jump of the position or a cut of the attitude), which starts
    // them again from rest instead of being read as a huge acceleration
    void update_pose(const Vec3& position, const Quat& attitude) {
        if (!have_pose) {
            deriver.Reset(position, attitude);
            pos = position, quat = attitude, have_pose = true, since_change = 0, have_motion = false;
            return;
        }
        since_change++;
        if (picked_still == 0) {   // the game moved the craft: a new reading, since_change frames after the last
            const float dt = float(since_change) / GAME_HZ;
            const float dx = position.X - pos.X, dy = position.Y - pos.Y, dz = position.Z - pos.Z;
            const float dot = std::fabs(attitude.X * quat.X + attitude.Y * quat.Y + attitude.Z * quat.Z + attitude.W * quat.W);
            const float angle = 2.0f * std::acos(dot > 1.0f ? 1.0f : dot);
            const Vec3 v{dx / dt, dy / dt, dz / dt};
            const float rate = angle / dt;
            bool jump = std::sqrt(dx * dx + dy * dy + dz * dz) > MAX_STEP || angle > MAX_TURN || since_change > STILL_FRAMES;
            if (!jump && have_motion) {
                const float dvx = v.X - prev_v.X, dvy = v.Y - prev_v.Y, dvz = v.Z - prev_v.Z;
                jump = std::sqrt(dvx * dvx + dvy * dvy + dvz * dvz) / dt > MAX_ACCEL || std::fabs(rate - prev_rate) / dt > MAX_ALPHA;
            }
            if (jump) {
                deriver.Reset(position, attitude);
                have_motion = false;
            } else {
                deriver.Sample(dt, position, attitude);
                prev_v = v, prev_rate = rate, have_motion = true;
            }
            pos = position, quat = attitude, since_change = 0;
        } else if (picked_still >= STILL_FRAMES) {
            deriver.Reset(pos, quat);   // standing still: gravity's reaction and no turning
            have_motion = false;
        }
        // (a frame between two changes keeps what the last reading gave)
    }
    // No pose to read (outside a flight): the last one stays, with nothing measured (C-11)
    void lose_pose() {
        deriver = BodyDynamicsDeriver{};
        have_pose = false;
        since_change = 0;
        have_motion = false;
    }

    // Watches the shield: each drop starts a hit, measured for HOLD frames and sent once
    void detect_hit(double t, bool playing, uint32_t top, int shield, int full) {
        // Nothing is carried across a stage change or out of a game (the shield is set again there), and
        // a rising shield is no hit
        if (!playing || !have_prev || top != prev_top) {
            have_prev = playing;
            prev_shield = shield, prev_top = top;
            if (!playing) pend_drop = 0, pend_age = 0, recent.clear();
            return;
        }
        const int lost = prev_shield - shield;
        prev_shield = shield;
        if (lost > 0 && lost <= full) pend_drop += float(lost) / float(full);
        if (pend_drop > 0 && ++pend_age >= HOLD) {
            EventInput e;
            e.Id = next_event++;
            e.Name = "body.impact";
            e.Intensity = clampf(HIT_MIN + HIT_PER_SHIELD * pend_drop, HIT_MIN, 1.0f);
            recent.push_back({t, e});   // (no direction: shots come from anywhere)
            if (log) printf("[telemetry] body.impact %u intensity %.2f (shield lost %.3f)\n", e.Id, e.Intensity, pend_drop);
            pend_drop = 0, pend_age = 0;
        }
    }

    void send_events(double t) {
        while (!recent.empty() && t - recent.front().first > EVENT_CARRY) recent.erase(recent.begin());
        if (recent.empty()) return;
        std::vector<EventInput> events;
        for (const auto& e : recent) events.push_back(e.second);
        for (const auto& run : PlanEventFrames(events, t)) {
            BuildEventFrame(wire, t, std::vector<EventInput>(events.begin() + run.first, events.begin() + run.first + run.second));
            send(wire.Wrap(FrameType::Event));
        }
    }

    bool log = getenv("SWT_TELEMETRY_LOG") != nullptr;
    static void (*g_sink)(const uint8_t*, size_t);
};

void (*Publisher::g_sink)(const uint8_t*, size_t) = nullptr;   // tests: where the datagrams go instead of the network

Publisher g_pub;

}  // namespace

void telemetry_test_begin(void (*sink)(const uint8_t*, size_t)) {
    g_pub.close();
    g_pub = Publisher{};
    Publisher::g_sink = sink;
}

void telemetry_frame(bool on, int port) {
    Publisher& p = g_pub;
    if (!on) {
        if (p.ok()) p.close();
        return;
    }
    if (!p.open(port > 0 && port < 65536 ? port : 40123)) return;

    const double t = p.now();
    const bool playing = swt_playing();
    const uint32_t top = rd32(TOP_STATE);
    const int shield = int16_t(rd16(P_SHIELD));
    const int full = int16_t(rd16(P_SHIELD_FULL)) > 0 ? int16_t(rd16(P_SHIELD_FULL)) : 1;
    if (playing) p.set_identity(craft_of(true, top, (rd32(EPISODE) >> 12) & 0xF));
    if (t - p.last_discovery >= DISCOVERY_EVERY) {
        p.send_discovery();
        p.last_discovery = t;
    }
    p.detect_hit(t, playing, top, shield, full);

    // The craft: the pose and its forces in a flight, the last pose and nothing measured otherwise
    MotionObjectInput o;
    o.Name = "player";
    Vec3 position{};
    Quat attitude{};
    const int step = int16_t(rd16(STAGE_STEP));
    const bool flying = playing && top < FIRST_DUEL && flown(top, step) && p.pick_pose(position, attitude);
    if (flying) p.update_pose(position, attitude);
    else p.lose_pose();
    o.Position = p.pos;
    o.Orientation = p.quat;
    if (flying) {
        o.HasBodyDynamics = true;
        o.SpecificForce = p.deriver.SpecificForce;
        o.AngularVelocity = p.deriver.AngularVelocity;
    }
    auto number = [&](uint16_t id, double v) {
        ChannelValueInput c;
        c.Id = id, c.Number = v;
        o.Channels.push_back(c);
    };
    ChannelValueInput paused;
    paused.Id = C_PAUSED, paused.IsBool = true, paused.Boolean = !playing;
    o.Channels.push_back(paused);
    number(C_SHIELD, playing ? clampf(float(shield) / float(full), 0.0f, 1.0f) : 0.0f);
    number(C_SCORE, playing ? double(rd32(P_SCORE)) : 0.0);

    p.send_events(t);   // before the frame, so a receiver has the hit as the game shows it
    BuildMotion(p.wire, t, {o}, p.generation);
    p.send(p.wire.Wrap(FrameType::Motion));
}
