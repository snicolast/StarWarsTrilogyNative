// The emulator: Model 3 memory, CROM and banks, hardware registers, vblank
// and decrementer, the game thread, game states and a diagnostic report. The
// window, the renderer and the menus are in app.cpp; runtime.h is the seam.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <atomic>
#include <mutex>

#include "eeprom.h"
#include "inputs.h"
#include "fns.h"
#include "../dogfight/dogfight.h"
#include "runtime.h"
#include "drive_board.h"
#include "jtag.h"
#include "scsi53c810.h"
#include "../video/tilegen.h"
#include "../audio/audio_out.h"
#include "../audio/sound_board.h"
#include "../video/r3d_state.h"
#include "stats.h"

Ctx g_ctx;
uint8_t* g_ram;
uint8_t g_trace_bits[512];
std::map<uint32_t, uint64_t> g_trace_hits;   // function -> hit count
uint32_t g_recent_fn[64];
uint32_t g_recent_idx = 0;

void rt_print_recent() {
    fprintf(stderr, "  last functions:");
    for (int i = 24; i >= 1; i--) fprintf(stderr, " %X", g_recent_fn[(g_recent_idx - i) & 63]);
    fprintf(stderr, "\n");
}

// --diff FN@FRAME: on the first call of FN at or after FRAME, saves the CPU
// state and RAM before and after running it (out/diff_before.bin,
// out/diff_after.bin) plus every device read (out/diff_mmio.bin), then exits.
// tools/diff_unicorn.py replays the call on an independent PowerPC emulator.
uint32_t g_diff_fn = 0xFFFFFFFF;
uint64_t g_diff_frame = 0;
bool g_diff_active = false;
std::vector<uint32_t> g_diff_mmio;   // (address, size, value) triples
void diff_run(Ctx& c, uint32_t fn);

void rt_trace(Ctx& c, uint32_t fn) {
    stats_enter(c, fn);
    if (fn == g_diff_fn && !g_diff_active) {
        diff_run(c, fn);
        return;
    }
    auto it = g_trace_hits.find(fn);
    if (it == g_trace_hits.end()) return;   // filter collision
    if (++it->second > 3) return;            // full detail only for the first 3
    printf("[trace] fn_%08X lr=%08X\n", fn, c.lr);
    for (int i = 0; i < 32; i++) printf("  r%-2d=%08X%s", i, c.r[i], i % 4 == 3 ? "\n" : "");
    for (int i = 3; i <= 10; i++) {
        uint32_t p = c.r[i];
        if (p < RAM_SIZE - 32 || p >= 0xFF000000) {
            printf("  [r%d]:", i);
            for (int k = 0; k < 8; k++) printf(" %08X", rd32(p + 4 * k));
            printf("\n");
        }
    }
}

// ---- Shared with the window side (app.cpp): see runtime.h ----
std::vector<uint8_t> g_crom, g_banked, g_vrom;
std::vector<uint8_t> g_drive_rom;
std::string g_data_dir = "data";
int g_frame_limit = INT32_MAX;   // --frames; with --headless, 600 by default
bool g_collect_stats = false;    // keep access statistics and the full texture stream for report()
Eeprom93C46 g_eeprom;
SoundBoard g_sound;
AudioOut g_audio;
AutoInput g_auto;
std::shared_ptr<const R3DFrameState> g_r3d_latest;                  // last closed 3D frame
std::condition_variable g_r3d_cv;                                   // notified when it changes
std::mutex g_r3d_mutex;
R3DTextureRam g_texture_ram;
std::mutex g_video_mutex;
std::atomic<float> g_wide_view{1.0f}, g_wide_applied{1.0f};   // widescreen culling (runtime.h)
std::vector<uint8_t> g_video_vram(TILEGEN_SIZE);
uint64_t g_video_frame = 0;
bool g_realtime = false;
LARGE_INTEGER g_qpf, g_start_time;
// CPU time of the game thread per emulated frame, without the waits (frame
// pacing, menus): summed in microseconds, read by the window thread once a second
std::atomic<uint64_t> g_game_busy_us{0};
std::atomic<uint64_t> g_game_frames{0};
// The WARNING notice and the copyright line shown at power-on are run through
// at full speed with nothing on screen: the game appears at the SEGA logo.
std::atomic<bool> g_boot_skip{false};
std::atomic<int> g_arcade_entry{0};
std::atomic<bool> g_skip_attract{false}, g_game_over_menu{false}, g_in_attract{false};
std::atomic<int> g_debug_jump{-1};
// Menus: the game stands still (and so does its sound) while set. The boot
// screens still run through first, so Play starts at the SEGA logo.
std::atomic<bool> g_hold{false};
// Shutdown: the window thread sets g_quit; the game thread parks itself at
// its next frame event (its native stack cannot be unwound) and reports it
// in g_parked, so the audio can be stopped with nobody pushing samples.
std::atomic<bool> g_quit{false}, g_parked{false};

double qpc_seconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(freq.QuadPart);
}

namespace {

constexpr uint64_t CPU_HZ = 166666667;      // PowerPC 603ev at 166 MHz (Step 2.1). TODO: DEC and TB still tick every 4 cycles, as tuned for Lost World
constexpr double FRAME_HZ = 57.524;         // Model 3 refresh rate
constexpr uint64_t FRAME_CYCLES = uint64_t(CPU_HZ / FRAME_HZ);
constexpr uint64_t SAMPLE_CYCLES = 200000;  // sampling profiler
constexpr uint32_t MSR_EE = 0x8000, MSR_IP = 0x40;

uint64_t g_frame = 0, g_next_sample = SAMPLE_CYCLES;
// Frame timeline (424 lines: 40 of VBlank, then 384 active; spec section 8)
constexpr uint64_t LINE_CYCLES = FRAME_CYCLES / 424;
uint64_t g_frame_start = 0, g_next_phase = 0;
int g_phase = 0;              // 0 = VBlank start, 1 = end of VBlank, 2 = ping-pong flip, 3 = last line
uint8_t g_midi_ctrl = 0;      // sound board MIDI control port
int g_midi_pulses = 0;        // MIDI interrupts left in this VBlank
uint64_t g_midi_next = 0;
uint32_t flip_line();

// Interrupt controller (0xF0100014 mask, 0xF0100018 status)
uint8_t g_irq_state = 0, g_irq_enable = 0;   // level-triggered, acknowledged by writing 0xF0100018
bool g_in_irq = false;
uint64_t g_irq_count[8] = {};

// Decrementer
uint32_t g_dec_value = 0xFFFFFFFF;
uint64_t g_dec_base = 0;
bool g_dec_armed = false;
uint64_t g_dec_irqs = 0;

[[noreturn]] void exit_from_game_thread(int code);
uint8_t g_adc_channel = 0;
Ncr53c810 g_scsi;
DriveBoard g_drive;   // force feedback board of the joystick (its own Z80)
Jtag g_jtag;   // Real3D ASIC IDs, checked at boot on Step 2.1

// Time base (TB): the game resets it to 0 and measures delays with it
uint64_t g_tb_base = 0;

// Real3D: a status bit (0x84000000) toggles every frame;
// the game uses it to know the chip has finished drawing.
uint32_t g_real3d_status = 0xFFFFFFFD;   // CPU view: bit 1 = ping-pong

// PCI configuration space. Step 2.1 has an MPC106 bridge: its address register
// is at 0xFEC00000 and its data register at 0xFEE00000; io_addr() folds them
// onto the MPC105 addresses Lost World used (0xF0800CF8 address, 0xF0C00CFC data)
std::map<uint32_t, uint32_t> g_pci_cfg;

uint32_t le32_at(const uint8_t* p) { return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

uint32_t pci_read(uint32_t cfg) {
    uint32_t dev = (cfg >> 11) & 0x1F, reg = cfg & 0xFC;
    if (reg == 0) {
        if (dev == 0x00) return 0x00021057;   // the bridge itself: Motorola MPC106
        if (dev == 0x0D) return 0x178611DB;   // Real3D Pro-1000, Step 2.x (Step 1.x: 0x16C311DB)
        return 0xFFFFFFFF;                    // no device (no 53C810 on Step 2 boards)
    }
    auto it = g_pci_cfg.find(cfg & 0xFFFFFFFC);
    return it == g_pci_cfg.end() ? 0 : it->second;
}

// Device memory: 64 KB pages allocated on demand
std::unordered_map<uint32_t, std::vector<uint8_t>> g_dev;
std::vector<uint8_t> g_texture_fifo;   // the part not yet uploaded (the whole stream with g_collect_stats)
std::unordered_set<uint32_t> g_r3d_dirty;                          // pages touched since the last frame
size_t g_fifo_consumed = 0;                                         // bytes of the FIFO already uploaded
uint64_t g_r3d_frames = 0;
void r3d_flush();
void texture_port_word(uint32_t w);

struct Stat { uint64_t reads = 0, writes = 0; uint32_t last = 0; };
std::unordered_map<uint32_t, Stat> g_stats;  // per address (control) or per page (everything else)
std::map<uint32_t, uint64_t> g_missing;      // called functions that don't exist
std::map<uint32_t, uint64_t> g_bad;          // untranslated instructions
std::unordered_map<uint32_t, uint64_t> g_samples;

// Control registers by exact address; memories (Real3D, backup RAM) by page
bool is_page(uint32_t a) {
    if (a >= 0xC1000000 && a < 0xC1000100) return false;   // 53C810 registers
    return !(a >= 0xF0000000 && a < 0xF1000000 && !(a >= 0xF00C0000 && a < 0xF0100000));
}
uint32_t stat_key(uint32_t a) { return is_page(a) ? (a & ~0xFFFu) : a; }

uint8_t* dev_ptr(uint32_t a) {
    static uint32_t last_key = 0xFFFFFFFF;
    static uint8_t* last_page = nullptr;
    if ((a >> 16) != last_key) {
        auto& page = g_dev[a >> 16];
        if (page.empty()) page.assign(0x10000, 0);
        last_key = a >> 16;
        last_page = page.data();
    }
    return last_page + (a & 0xFFFF);
}

const uint8_t* rom_ptr(uint32_t a) {
    // Step 2.1: the fixed 8 MB CROM fills 0xFF800000-0xFFFFFFFF; below it, the
    // 0xFF000000 window shows one 8 MB bank of the banked CROM.
    // The bank is the inverted low nibble of system register 0x08 (written
    // at 0xFE100008, folded onto 0xF0100008 by io_addr)
    if (a >= 0xFF800000) return &g_crom[(a - 0xFF800000) % g_crom.size()];
    if (a >= 0xFF000000 && !g_banked.empty()) {
        uint32_t bank = (~*dev_ptr(0xF0100008)) & 0xF;
        size_t off = size_t(bank) * 0x800000 + (a - 0xFF000000);
        return &g_banked[off % g_banked.size()];
    }
    return nullptr;
}

// Step 2.1 boards decode the system devices (inputs, MIDI, backup RAM, system
// registers, RTC, security) at 0xFExxxxxx as a mirror of 0xF0xxxxxx; the MPC106
// configuration registers sit inside that mirror. Everything below is written
// against the 0xF0 addresses, so device accesses go through here first.
uint32_t io_addr(uint32_t a) {
    if ((a >> 24) != 0xFE) return a;
    if (a >= 0xFEC00000 && a < 0xFEE00000) return 0xF0800CF8 + (a & 3);   // MPC106 CONFIG_ADDR
    if (a >= 0xFEE00000 && a < 0xFEF00000) return 0xF0C00CFC + (a & 3);   // MPC106 CONFIG_DATA
    return 0xF0000000 | (a & 0x00FFFFFF);
}

// Real3D DMA device (Step 2.x, 0xC2000000). The game writes its 32-bit
// registers byte-reversed (little-endian device), so le32_at() reads them:
// 0x00 source, 0x04 destination, 0x08 length in words (writing it runs the
// copy), 0x0C status (bit 0 IRQ pending), 0x0D IRQ acknowledge, 0x0E config
// (bit 0x80 byte-reverses each word, bit 0x01 raises the IRQ when done),
// 0x10 command, 0x14 command result.
bool g_dma_irq = false;   // non-maskable in the board's IRQ controller

static void r3d_dma_copy() {
    uint32_t src = le32_at(dev_ptr(0xC2000000)), dst = le32_at(dev_ptr(0xC2000004));
    uint32_t words = le32_at(dev_ptr(0xC2000008));
    uint8_t config = *dev_ptr(0xC200000E);
    rt_dma_copy(dst, src, words * 4, config & 0x80);
    if (config & 0x01) {
        *dev_ptr(0xC200000C) |= 0x01;
        g_dma_irq = true;
        g_ctx.next_event = g_ctx.cycles;
    }
}

static void r3d_dma_command(uint32_t cmd) {
    uint32_t result = 0;
    if (cmd & 0x20000000) result = 0x16C311DB;   // ID command: the games expect the Step 1.x ID here
    // Real3D register read. g_real3d_status is the CPU's view through lwz at
    // 0x84000000, i.e. byte-swapped; the register itself is the swap of it
    // (the game reads the result with lwbrx and waits for ping-pong bit 0x02000000)
    else if (cmd & 0x80000000) result = (cmd & 0x3F) == 0 ? bswap32(g_real3d_status) : 0;
    uint8_t* p = dev_ptr(0xC2000014);
    for (int i = 0; i < 4; i++) p[i] = uint8_t(result >> (8 * i));
}

// RTC-72421 at 0xF0140000: one digit per register, register n in the high
// byte of word n (0 = seconds, 1 = tens of seconds ... 12 = day of the week);
// the next byte reads 0x03 (battery voltage test). The clock runs on game
// time (host time at start + cycles), so the boot code's CPU speed
// measurement (TB ticks over one RTC second, fn_000F15E4) stays consistent
// at any emulation speed.
static uint8_t rtc_read(uint32_t a) {
    if ((a & 3) == 1) return 0x03;
    if (a & 3) return 0;
    static const time_t start = time(nullptr);
    time_t now = start + time_t(g_ctx.cycles / CPU_HZ);
    struct tm t;
    localtime_s(&t, &now);
    switch ((a >> 2) & 0xF) {
    case 0: return uint8_t(t.tm_sec % 10);
    case 1: return uint8_t(t.tm_sec / 10);
    case 2: return uint8_t(t.tm_min % 10);
    case 3: return uint8_t(t.tm_min / 10);
    case 4: return uint8_t(t.tm_hour % 10);
    case 5: return uint8_t(t.tm_hour / 10);
    case 6: return uint8_t(t.tm_mday % 10);
    case 7: return uint8_t(t.tm_mday / 10);
    case 8: return uint8_t((t.tm_mon + 1) % 10);
    case 9: return uint8_t((t.tm_mon + 1) / 10);
    case 10: return uint8_t(t.tm_year % 10);
    case 11: return uint8_t((t.tm_year % 100) / 10);
    case 12: return uint8_t(t.tm_wday);
    default: return 0;
    }
}

// Security board (315-5881 crypto chip) at 0xF01A0000. The game reads 112
// bytes through the data register (0x1C, two bytes per word in its high half,
// low byte first) and compares them with its copyright text at RAM 0x1B1E80
// ("===== STAR WARS =====..."); a mismatch prints "not correct" and a screen
// of numbers (fn_000A3600). The chip decrypts data stored on the board; here
// it hands back the text the game expects. The first read after the address
// registers (0x10/0x14) are written returns 0xFFFF.
static bool g_security_first = true;
static uint32_t g_security_pos = 0;
static uint8_t security_read(uint32_t a) {
    static uint8_t word[4] = {0xFF, 0xFF, 0, 0};
    if (a == 0xF01A001C) {
        if (g_security_first) {
            g_security_first = false;
            word[0] = word[1] = 0xFF;
        } else {
            uint32_t p = 0x1B1E80 + 2 * (g_security_pos++ % 0x38);
            word[1] = rd8(p);       // byte stored first (bits 16-23)
            word[0] = rd8(p + 1);   // then bits 24-31
        }
    }
    if (a >= 0xF01A001C && a < 0xF01A0020) return word[a - 0xF01A001C];
    if (a < 0xF01A0004) return 0;   // status: ready
    return 0xFF;
}

uint8_t dev_read8(uint32_t a) {
    a = io_addr(a);
    if ((a >> 8) == 0xF01A00) return security_read(a);
    if (a == 0xF0100018) return g_irq_state;
    if (a == 0xF0100014) return g_irq_enable;
    if (a == 0xF010001C) return 0xFF;
    if (a == 0xF0100010) return g_jtag.read();
    if (a == 0xF0080000) return 0x00;   // MIDI data port
    if (a == 0xF0080004) return 0x83;   // MIDI control port
    if ((a >> 16) == 0xF014) return rtc_read(a);
    if (a == 0xF0040004) {
        bool bank1 = *dev_ptr(0xF0040000) & 0x01;   // bank selected by writing 0xF0040000
        uint8_t v = g_inputs.system_port(bank1);
        return bank1 ? uint8_t((v & 0xDF) | (g_eeprom.read_do() << 5)) : v;
    }
    // Player buttons: active low, 0xFF = nothing pressed
    if (a == 0xF0040008) return g_inputs.player_port(0);
    if (a == 0xF004000C) return g_drive.reply();   // force feedback board reply (Lost World: player 2 buttons)
    if (a == 0xF0040018) return 0x7F;
    if (a == 0xF0040034) return 0x00;   // serial FIFO flags (no light gun board)
    if (a == 0xF004003C) {              // ADC: gun position, channel auto-increments
        uint8_t v = g_inputs.adc(g_adc_channel & 7);
        g_adc_channel++;
        return v;
    }
    if (a > 0xF0040000 && a < 0xF0040040 && (a & 3) == 0) return 0xFF;   // unused inputs read as 1s (0x00 reads back the bank)
    if (a >= 0x84000000 && a < 0x84000004) return uint8_t(g_real3d_status >> (24 - 8 * (a & 3)));
    if (a >= 0xF0C00CFC && a < 0xF0C00D00) {
        uint32_t v = pci_read(le32_at(dev_ptr(0xF0800CF8)));
        return uint8_t(v >> (8 * (a - 0xF0C00CFC)));
    }
    if (a >= 0xC1000000 && a < 0xC1000100) {
        uint8_t v;
        if (g_scsi.read8(a & 0xFF, v)) return v;
    }
    return *dev_ptr(a);
}

// Debugging aid: SWT_MIDILOG=path writes the bytes the game sends to the sound
// board, a line for each frame that has any: the frame, the top state and
// its step, the triggers held (1 the first, 2 the second), then the bytes.
// (To find which commands play which sound: the DOGFIGHT mode sends them itself)
static void midi_log(uint8_t v) {
    static FILE* f = getenv("SWT_MIDILOG") ? fopen(getenv("SWT_MIDILOG"), "w") : nullptr;
    static uint64_t frame = ~0ull;
    if (!f) return;
    if (frame != g_frame) {
        frame = g_frame;
        fprintf(f, "\n%llu %u %d %d:", (unsigned long long)g_frame, (unsigned)rd32(0x2210E8), int(int16_t(rd16(0x221136))),
                int(g_inputs.p[0].trigger) | int(g_inputs.p[0].offscreen) << 1);
    }
    fprintf(f, " %02X", v);
    fflush(f);
}

void dev_write8(uint32_t a, uint8_t v) {
    a = io_addr(a);
    if (a >= 0xF01A0010 && a < 0xF01A0018) { g_security_first = true; g_security_pos = 0; }
    if ((a >> 8) == 0xC20000) {   // Real3D DMA registers
        if (a == 0xC200000D) {   // IRQ acknowledge
            if (v & 1) { *dev_ptr(0xC200000C) &= ~1; g_dma_irq = false; }
            return;
        }
        *dev_ptr(a) = v;
        if (a == 0xC200000B) r3d_dma_copy();
        if (a == 0xC2000013) r3d_dma_command(le32_at(dev_ptr(0xC2000010)));
        if (a == 0xC2000017) for (int i = 0; i < 4; i++) dev_ptr(0xC2000014)[i] = 0xFF;
        return;
    }
    if (a == 0xF0100014) { g_irq_enable = v; g_ctx.next_event = g_ctx.cycles; }
    if (a == 0xF0100018) g_irq_state &= ~v;   // acknowledge
    if (a == 0xF010000C) g_jtag.write(v);
    if (a == 0xF0040010) g_drive.command(v);   // command to the force feedback board
    if (a == 0xF1180010) g_irq_state &= ~v;   // tilegen IRQ acknowledge register (LE low byte)
    if (a == 0xF0080000) {   // MIDI byte to the sound board
        g_irq_state &= ~0x40;
        midi_log(v);
        g_sound.midi_write(v);
    }
    if (a == 0xF0080004) {
        g_midi_ctrl = v;
        if (!(v & 0x20)) g_irq_state &= ~0x40;
    }
    if (a == 0xF0040000) g_eeprom.write_pins(v);
    if (a == 0xF004003C) g_adc_channel = v & 7;
    *dev_ptr(a) = v;
    if (a == 0xF0C00CFF)
        g_pci_cfg[le32_at(dev_ptr(0xF0800CF8)) & 0xFFFFFFFC] = le32_at(dev_ptr(0xF0C00CFC));
    if (a >= 0xC1000000 && a < 0xC1000100) g_scsi.wrote8(a & 0xFF, v, dev_ptr(0xC1000000));
    // 0x94000000 is a texture FIFO: the whole stream is stored in order
    if ((a >> 24) == 0x94) g_texture_fifo.push_back(v);
    // 0x90000000: texture port, three words (source, header, trigger) per upload
    if ((a >> 24) == 0x90 && (a & 3) == 3) texture_port_word(le32_at(dev_ptr(a & ~3u)));
    uint32_t region = a >> 24;
    if (region == 0x8C || region == 0x8E || region == 0x98) g_r3d_dirty.insert(a >> 16);
    // Writing to the command port (0x88000000) closes the 3D frame
    if (a == 0x88000003) r3d_flush();
}

void texture_port_word(uint32_t w) {
    static uint32_t words[2];
    static int count = 0;
    if (count < 2) {
        words[count++] = w;
        return;
    }
    count = 0;   // third write triggers the upload; its data is ignored
    uint32_t src = words[0] & 0xFFFFFF, header = words[1];
    if (src >= 0x100000) {
        if (g_vrom.empty()) return;
        size_t words_total = g_vrom.size() / 4;
        size_t start = src % words_total;
        const uint16_t* p = reinterpret_cast<const uint16_t*>(g_vrom.data() + start * 4);
        g_texture_ram.upload_direct(header, p, (words_total - start) * 2);
    } else {
        // Polygon RAM source: copy the (at most 1 MB) rest of it
        std::vector<uint16_t> buf((0x100000 - src) * 2);
        for (size_t i = 0; i < buf.size() / 2; i++) {
            uint32_t v = le32_at(dev_ptr(0x98000000 + uint32_t(src + i) * 4));
            buf[2 * i] = uint16_t(v);
            buf[2 * i + 1] = uint16_t(v >> 16);
        }
        g_texture_ram.upload_direct(header, buf.data(), buf.size());
    }
}

// When the game writes the command port the Real3D memory is published as an
// immutable frame (unchanged pages are shared) and the texture FIFO collected
// since the previous flush is applied to texture RAM.

void r3d_flush() {
    auto state = std::make_shared<R3DFrameState>();
    state->flushed = state->time = qpc_seconds();
    state->vblank = g_frame;
    // At real speed the frame is dated by its vblank: the game finishes its
    // frames a few ms after the vblank, by a varying amount, and that jitter
    // would make the interpolated motion uneven
    if (g_realtime && !g_boot_skip) {
        double vblank = double(g_start_time.QuadPart) / double(g_qpf.QuadPart) + double(g_frame) / FRAME_HZ;
        if (state->flushed >= vblank && state->flushed < vblank + 0.05) state->time = vblank;
    }
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        if (g_r3d_latest) state->pages = g_r3d_latest->pages;
    }
    double t_pages = qpc_seconds();
    size_t dirty_pages = g_r3d_dirty.size();
    for (uint32_t key : g_r3d_dirty)
        state->pages[key] = std::make_shared<const R3DPage>(*reinterpret_cast<const R3DPage*>(g_dev[key].data()));
    g_r3d_dirty.clear();
    state->frame = ++g_r3d_frames;

    double t_tex = qpc_seconds();
    size_t words = (g_texture_fifo.size() - g_fifo_consumed) / 4;
    if (words) {
        std::vector<uint32_t> fifo(words);
        memcpy(fifo.data(), g_texture_fifo.data() + g_fifo_consumed, words * 4);
        g_fifo_consumed += words * 4;
        g_texture_ram.upload_fifo(fifo);
        // The whole stream is only needed by dump_video_memory (report());
        // otherwise the uploaded part is dropped so the vector does not grow all session
        if (!g_collect_stats) {
            g_texture_fifo.erase(g_texture_fifo.begin(), g_texture_fifo.begin() + ptrdiff_t(g_fifo_consumed));
            g_fifo_consumed = 0;
        }
    }
    double t_end = qpc_seconds();
    frame_log("F %llu %.3f %.3f %zu %zu\n", (unsigned long long)g_frame, (t_tex - t_pages) * 1000,
              (t_end - t_tex) * 1000, dirty_pages, words);
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        g_r3d_latest = std::move(state);
    }
    g_r3d_cv.notify_all();
}

// Dumps the Real3D and 2D layer memories for the future renderer
void dump_video_memory() {
    CreateDirectoryA("out", nullptr);   // CreateDirectory does not create the parent
    CreateDirectoryA("out/real3d", nullptr);
    struct Region { uint32_t base, size; const char* name; };
    const Region regions[] = {
        {0x8C000000, 0x01000000, "culling_lo"}, {0x8E000000, 0x01000000, "culling_hi"},
        {0x98000000, 0x01000000, "polygon"},    {0xF1000000, 0x00200000, "tilegen"},
    };
    for (auto& r : regions) {
        // The 3D memories are taken from the last closed frame (consistent state)
        std::unordered_map<uint32_t, const uint8_t*> pages;
        if ((r.base >> 24) != 0xF1 && g_r3d_latest)
            for (auto& [key, page] : g_r3d_latest->pages) pages[key] = page->data();
        else
            for (auto& [key, page] : g_dev) pages[key] = page.data();
        uint32_t top = 0;
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + r.size) top = std::max(top, (key << 16) + 0x10000 - r.base);
        if (!top) continue;
        std::vector<uint8_t> buf(top, 0);
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + top) memcpy(&buf[(key << 16) - r.base], page, 0x10000);
        std::string path = std::string("out/real3d/") + r.name + ".bin";
        if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
        else fprintf(stderr, "[warning] could not write %s\n", path.c_str());
        printf("  %-12s %08X  %u KB\n", r.name, r.base, top / 1024);
    }
    if (FILE* f = fopen("out/real3d/texture_fifo.bin", "wb")) {
        fwrite(g_texture_fifo.data(), 1, g_texture_fifo.size(), f);
        fclose(f);
    }
    printf("  texture_fifo %08X  %zu KB\n", 0x94000000u, g_texture_fifo.size() / 1024);
}

// SWT_TILEGEN_DUMP=first,last,step: the tilegen memory of those frames to
// out/real3d/tilegen_<frame>.bin (to follow the 2D layers during the boot)
constexpr uint32_t TILEGEN_DUMP_SIZE = 0x190000;
static void tilegen_dump_frame(uint64_t frame) {
    static int first = -1, last = 0, step = 1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("SWT_TILEGEN_DUMP")) sscanf(e, "%d,%d,%d", &first, &last, &step);
        if (step < 1) step = 1;
    }
    if (first < 0 || frame < uint64_t(first) || frame > uint64_t(last) || (frame - first) % step) return;
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA("out/real3d", nullptr);
    std::vector<uint8_t> buf(TILEGEN_DUMP_SIZE, 0);
    for (auto& [key, page] : g_dev)
        if ((key << 16) >= 0xF1000000u && (key << 16) < 0xF1000000u + TILEGEN_DUMP_SIZE)
            memcpy(&buf[(key << 16) - 0xF1000000u], page.data(), 0x10000);
    char path[64];
    snprintf(path, sizeof path, "out/real3d/tilegen_%llu.bin", (unsigned long long)frame);
    if (FILE* f = fopen(path, "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
}

void note(uint32_t a, bool write, uint32_t v) {
    if (!g_collect_stats) return;   // the report only comes with --frames / --headless
    auto& s = g_stats[stat_key(a)];
    (write ? s.writes : s.reads)++;
    s.last = v;
}

uint32_t find_function(uint32_t pc) {
    int lo = 0, hi = g_fn_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr <= pc) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best >= 0 ? g_fn_table[best].addr : 0;
}

// Ping-pong flip line: tilegen register 0x08 (device word = LE bytes)
uint32_t flip_line() {
    uint32_t v = le32_at(dev_ptr(0xF1180008));
    return v ? v : 238;
}

void schedule(Ctx& c) {
    if (g_diff_active) { c.next_event = ~0ull; return; }
    uint64_t n = std::min(g_next_phase, g_next_sample);
    if (g_dec_armed) n = std::min(n, g_dec_base + uint64_t(g_dec_value) * 4);
    if (g_midi_pulses > 0) n = std::min(n, std::max(g_midi_next, c.cycles + 1));
    if (((g_irq_state & g_irq_enable) || g_dma_irq) && (c.msr & MSR_EE)) n = c.cycles;
    c.next_event = n;
}

void exception(Ctx& c, uint32_t vector) {
    g_in_irq = true;
    c.srr0 = c.pc_hint;
    c.srr1 = c.msr;
    c.msr &= ~(MSR_EE | 0x4000u /*PR*/ | 0x30u /*IR/DR*/);
    rt_call(c, ((c.srr1 & MSR_IP) ? 0xFFF00000u : 0u) + vector);
    c.msr = c.srr1;   // rfi
    g_in_irq = false;
}

void report() {
    Ctx& c = g_ctx;
    printf("\n================ REPORT ================\n");
    printf("Frames: %llu  cycles: %llu  MSR: %08X\n", (unsigned long long)g_frame,
           (unsigned long long)c.cycles, c.msr);
    printf("Interrupts: mask=%02X state=%02X  delivered per bit:", g_irq_enable, g_irq_state);
    for (int i = 0; i < 8; i++) printf(" %llu", (unsigned long long)g_irq_count[i]);
    printf("  decrementer: %llu\n", (unsigned long long)g_dec_irqs);

    printf("\nWhere the time goes (samples per function):\n");
    std::map<uint32_t, uint64_t> per_fn;
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& [pc, n] : g_samples) { per_fn[find_function(pc)] += n; hot.push_back({n, pc}); }
    std::vector<std::pair<uint64_t, uint32_t>> fns;
    for (auto& [f, n] : per_fn) fns.push_back({n, f});
    std::sort(fns.rbegin(), fns.rend());
    std::sort(hot.rbegin(), hot.rend());
    for (size_t i = 0; i < fns.size() && i < 12; i++)
        printf("  fn_%08X  %llu\n", fns[i].second, (unsigned long long)fns[i].first);
    printf("Hottest single spots:\n");
    for (size_t i = 0; i < hot.size() && i < 8; i++)
        printf("  %08X  %llu\n", hot[i].second, (unsigned long long)hot[i].first);

    printf("\nSound: %u notes restarted on stopped voices (they used to be lost)\n", unsigned(Scsp::restarted()));
    printf("\nHardware accesses (register or memory range, reads, writes, last value):\n");
    std::map<uint32_t, Stat> stats(g_stats.begin(), g_stats.end());
    for (auto it = stats.begin(); it != stats.end();) {
        // Consecutive 4 KB pages are merged into a single range
        uint32_t first = it->first, last = first;
        Stat s = it->second;
        auto next = std::next(it);
        bool page = is_page(first);
        while (page && next != stats.end() && next->first == last + 0x1000) {
            last = next->first;
            s.reads += next->second.reads;
            s.writes += next->second.writes;
            s.last = next->second.last;
            ++next;
        }
        if (page)
            printf("  %08X-%08X  R=%-9llu W=%-9llu last=%08X\n", first, last + 0xFFF,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        else
            printf("  %08X           R=%-9llu W=%-9llu last=%08X\n", first,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        it = next;
    }

    g_scsi.report();
    g_sound.close_wav();
    if (!g_trace_hits.empty()) {
        printf("\nTraced functions (times called):\n");
        for (auto& [fn, n] : g_trace_hits) printf("  fn_%08X  %llu\n", fn, (unsigned long long)n);
    }
    if (FILE* f = fopen("out/ram.bin", "wb")) {   // for analysis with the tools
        fwrite(g_ram, 1, RAM_SIZE, f);
        fclose(f);
    }
    printf("\nVideo memories dumped to out/real3d/:\n");
    dump_video_memory();

    if (!g_missing.empty()) {
        printf("\nCalls to functions not recompiled:\n");
        for (auto& [a, n] : g_missing) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    if (!g_bad.empty()) {
        printf("\nInvalid instructions executed:\n");
        for (auto& [a, n] : g_bad) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    fflush(stdout);
}

}  // namespace

bool load_rom(const std::string& name, std::vector<uint8_t>& out, bool required) {
    std::string path = g_data_dir + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (required) fprintf(stderr, "Cannot find %s (run tools/romtool.py first)\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    out.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

DWORD WINAPI game_thread(LPVOID) {
    Ctx& c = g_ctx;
    c.msr = MSR_IP;
    c.next_event = 0;
    // PVR: PowerPC 603ev ("Goldeneye", 2.1). The boot code wants a version
    // above 0x00070000 (error 0x10 on the LEDs otherwise)
    c.spr[287] = 0x00071202;
    g_drive.init(g_drive_rom);
    if (g_drive_rom.empty()) printf("No force feedback board program (epr-21119): the game will stop at FAILED FEEDBACK LEVER\n");
    // Test switches left set in the console (set SWT_...=1) change the game
    // in ways that look like bugs later on: listed, so a log shows them
    if (char** env = _environ)
        for (; *env; env++)
            if (strncmp(*env, "SWT_", 4) == 0 || strncmp(*env, "LW_", 3) == 0) printf("[env] %s\n", *env);
    printf("Running the boot code from 0xFFF00100...\n");
    fn_FFF00100(c);
    printf("Boot done: jumping to %08X (MSR=%08X)\n", c.lr, c.msr);
    rt_call(c, c.lr);
    printf("The main program returned (unexpected)\n");
    report();
    return 0;
}

// ---- Slow memory: CROM, banks and devices ----
uint8_t rt_rd8_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return *p;
    uint8_t v = dev_read8(a);
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 1u, v});
    return v;
}
uint16_t rt_rd16_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint16_t(p[0] << 8 | p[1]);
    uint16_t v = uint16_t(dev_read8(a) << 8 | dev_read8(a + 1));
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 2u, v});
    return v;
}
uint32_t rt_rd32_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
    uint32_t v = uint32_t(dev_read8(a)) << 24 | dev_read8(a + 1) << 16 | dev_read8(a + 2) << 8 | dev_read8(a + 3);
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 4u, v});
    return v;
}
// Stores outside RAM and outside any device usually mean a bad pointer:
// report the first ones with the recent call history
static void check_wild_write(uint32_t a, uint32_t v) {
    static int reported = 0;
    if (a >= RAM_SIZE && a < 0x80000000u && reported < 8) {
        reported++;
        fprintf(stderr, "[warning] write outside RAM %08X = %08X (pc ~%08X, frame %llu)\n", a, v, g_ctx.pc_hint,
                (unsigned long long)g_frame);
        rt_print_recent();
    }
}

// SWT_WATCH=lo,hi: the first 300 CPU writes in [lo, hi) with the code that made them
// (pc of the function, its caller and three more frames of the stack's back chain)
static void watch_write(uint32_t a, uint32_t v, int bytes) {
    static uint32_t lo = 1, hi = 0;
    static int left = 300;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("SWT_WATCH")) sscanf(e, "%x,%x", &lo, &hi);
    }
    if (a < lo || a >= hi || left <= 0) return;
    left--;
    printf("[watch] frame %llu %08X = %0*X  pc~%08X lr=%08X", (unsigned long long)g_frame, a, bytes * 2, v,
           g_ctx.pc_hint, g_ctx.lr);
    uint32_t sp = g_ctx.r[1];
    for (int i = 0; i < 4 && sp && sp < RAM_SIZE - 8; i++) {
        sp = rd32(sp);
        if (!sp || sp >= RAM_SIZE - 8) break;
        printf(" <%08X", rd32(sp + 4));
    }
    printf("\n");
}

void rt_wr8_slow(uint32_t a, uint8_t v) { watch_write(a, v, 1); check_wild_write(a, v); note(a, true, v); dev_write8(a, v); }
void rt_wr16_slow(uint32_t a, uint16_t v) {
    watch_write(a, v, 2);
    check_wild_write(a, v);
    note(a, true, v);
    dev_write8(a, uint8_t(v >> 8));
    dev_write8(a + 1, uint8_t(v));
}
void rt_wr32_slow(uint32_t a, uint32_t v) {
    watch_write(a, v, 4);
    check_wild_write(a, v);
    note(a, true, v);
    for (int i = 0; i < 4; i++) dev_write8(a + i, uint8_t(v >> (24 - 8 * i)));
}

// Access statistics for a run of bytes, as the byte-by-byte path would count them
static void note_range(uint32_t a, uint32_t n, bool write, uint8_t last) {
    if (!g_collect_stats) return;
    while (n) {
        uint32_t run = std::min(n, 0x1000 - (a & 0xFFF));
        auto& s = g_stats[stat_key(a)];
        (write ? s.writes : s.reads) += run;
        s.last = last;
        a += run;
        n -= run;
    }
}

// Block copy for the 53C810 Memory Move and the Real3D DMA. Plain memories
// (RAM, ROM, the Real3D pages and the texture FIFO) are copied in runs that
// stay inside one 64 KB page, with the same side effects as the byte path
// (dirty pages, FIFO stream, statistics); registers and ports go through
// rd/wr so their side effects stay exact. `swap` reverses the bytes of each
// 32-bit word (Real3D DMA config bit 0x80), word by word in address order
// as the per-word copy did, so overlapping RAM copies give the same result.
void rt_dma_copy(uint32_t dst, uint32_t src, uint32_t n, bool swap) {
    if (swap) n &= ~3u;
    while (n) {
        uint32_t run = std::min(n, 0x10000 - (src & 0xFFFF));
        run = std::min(run, 0x10000 - (dst & 0xFFFF));
        if (swap) run &= ~3u;   // a word that straddles a page goes the slow way
        const uint8_t* sp = nullptr;
        if (src < RAM_SIZE) sp = g_ram + src;
        else if (src >= 0xFF000000) sp = rom_ptr(src);   // contiguous within a 64 KB page
        uint8_t* dp = nullptr;
        uint32_t region = dst >> 24;
        bool r3d = region == 0x8C || region == 0x8E || region == 0x98, fifo = region == 0x94;
        if (dst < RAM_SIZE) dp = g_ram + dst;
        else if (r3d || fifo) dp = dev_ptr(dst);
        if (!sp || !dp || !run) {   // a device: as the chip would, one byte (or word) at a time
            if (swap) {
                wr32(dst, bswap32(rd32(src)));
                dst += 4, src += 4, n -= 4;
            } else {
                wr8(dst, rd8(src));
                dst++, src++, n--;
            }
            continue;
        }
        if (swap) {
            for (uint32_t i = 0; i < run; i += 4) {
                uint8_t b0 = sp[i], b1 = sp[i + 1], b2 = sp[i + 2], b3 = sp[i + 3];
                dp[i] = b3, dp[i + 1] = b2, dp[i + 2] = b1, dp[i + 3] = b0;
            }
        } else {
            memmove(dp, sp, run);   // RAM to RAM may overlap
        }
        if (dst >= RAM_SIZE) {
            note_range(dst, run, true, dp[run - 1]);
            if (r3d) g_r3d_dirty.insert(dst >> 16);
            if (fifo) g_texture_fifo.insert(g_texture_fifo.end(), dp, dp + run);
        }
        dst += run, src += run, n -= run;
    }
}

// ---- Differential testing (--diff) ----
static void diff_save(const char* path, const Ctx& c, uint32_t fn) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[8] = {0x4C574446 /*LWDF*/, fn, c.cr, c.xer, c.lr, c.ctr, c.msr, c.fpscr};
    fwrite(hdr, 4, 8, f);
    fwrite(c.r, 4, 32, f);
    for (int i = 0; i < 32; i++) fwrite(&c.f[i].u, 8, 1, f);
    uint8_t bank = *dev_ptr(0xF0100008);
    fwrite(&bank, 1, 1, f);
    fwrite(g_ram, 1, RAM_SIZE, f);
    fclose(f);
}

void diff_run(Ctx& c, uint32_t fn) {
    if (g_frame < g_diff_frame) return;
    g_diff_active = true;
    c.next_event = ~0ull;
    diff_save("out/diff_before.bin", c, fn);
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {   // run the function once, with events held off
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr == fn) { g_fn_table[mid].fn(c); break; }
        if (g_fn_table[mid].addr < fn) lo = mid + 1; else hi = mid - 1;
    }
    diff_save("out/diff_after.bin", c, fn);
    if (FILE* f = fopen("out/diff_mmio.bin", "wb")) {
        fwrite(g_diff_mmio.data(), 4, g_diff_mmio.size(), f);
        fclose(f);
    }
    printf("diff: fn_%08X at frame %llu saved (%zu device reads)\n", fn,
           (unsigned long long)g_frame, g_diff_mmio.size() / 3);
    exit_from_game_thread(0);
}

// ---- Indirect calls ----
void rt_call(Ctx& c, uint32_t addr) {
    if (addr == 0 && g_frame > 0) {   // a null function pointer restarts the game on real hardware
        static int reported = 0;
        if (reported++ < 3) {
            fprintf(stderr, "[warning] call to address 0 (frame %llu, lr=%08X, r3=%08X)\n",
                    (unsigned long long)g_frame, c.lr, c.r[3]);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", rd32(c.r[3] + 4 * i));
            fprintf(stderr, "\n");
            rt_print_recent();
        }
    }
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr == addr) { g_fn_table[mid].fn(c); return; }
        if (g_fn_table[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    if (g_missing[addr]++ == 0) {
        fprintf(stderr, "[warning] call to %08X, not recompiled (from ~%08X)\n", addr, c.pc_hint);
        // recomp.py will include it in the next build
        if (FILE* f = fopen("out/missing.txt", "a")) {
            fprintf(f, "%08X  # called from ~%08X\n", addr, c.pc_hint);
            fclose(f);
        }
    }
}

void rt_bad(Ctx&, uint32_t addr) {
    if (g_bad[addr]++ == 0) fprintf(stderr, "[warning] invalid instruction at %08X\n", addr);
}

void rt_sc(Ctx& c, uint32_t addr) {
    c.pc_hint = addr;
    exception(c, 0xC00);
}

// ---- Events: frames, vblank, decrementer, sampling ----
// ---- Video: copy of the 2D layer on every vblank for the window thread ----
namespace {

// Star Wars Trilogy culls what is outside its own 4:3 field of view, so a
// widescreen window showed empty sides. Its viewport records (32 of 0x160
// bytes at 0x1E3940; the Real3D viewport node is at +0x40) keep the frustum
// the game culls with: +0x130..+0x13C the left/right plane normals (-sin, cos,
// sin, cos of the half angle), +0x150/+0x154 the left/right tangents (-t, t),
// from which it also builds the node's projection. For full-width viewports the
// horizontal half angle is widened here so the game keeps what the wider view
// shows (and the node too); the scene builder divides the widening back out
// of the projection. Up to 6:1 (WIDESCREEN 48:9, three monitors) = 4.5 times
// 4:3. SWT_NO_WIDE_CULL turns it off
static void widen_fov() {
    static const bool off = getenv("SWT_NO_WIDE_CULL") != nullptr;
    // In the lightsaber duels (top state 10..13) only the world's viewports
    // (priority 0) are widened: the player's arms and sabre (priority 1),
    // widened, were dropped by the game now and then (they flickered)
    uint32_t top_state = rd32(0x2210E8);
    static const bool duel_wide = getenv("SWT_DUEL_WIDE_CULL") != nullptr;
    bool duel = swt_playing() && top_state >= 10 && top_state <= 13 && !duel_wide;
    float want = off ? 1.0f : std::clamp(g_wide_view.load(), 1.0f, 4.5f);
    struct Slot { float base = 0, wrote = 0, factor = 1; };
    static Slot slots[32];
    auto rdf = [](uint32_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; };
    auto wrf = [](uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); };
    for (int n = 0; n < 32; n++) {
        uint32_t s = 0x1E3940 + uint32_t(n) * 0x160;
        uint32_t size = rd32(s + 0x40 + 0x14 * 4), pos = rd32(s + 0x40 + 0x1A * 4);
        bool full = (size & 0xFFFF) >= 495 * 4 && (pos & 0xFFFF) < 8;
        if (duel && ((rd32(s + 0x40) >> 3) & 3) != 0) full = false;
        float t = rdf(s + 0x154);
        Slot& sl = slots[n];
        if (t != sl.wrote || t <= 0) sl = {t, 0, 1};   // set (again) by the game
        float f = full && sl.base > 0 && sl.base < 10 ? want : 1.0f;
        if (f == 1 && sl.factor == 1) {   // as the game set it
            sl.wrote = t;
            continue;
        }
        if (f == sl.factor || sl.base <= 0 || sl.base >= 10) continue;
        float tw = sl.base * f, a = std::atan(tw), sn = std::sin(a), cs = std::cos(a);
        wrf(s + 0x130, -sn), wrf(s + 0x134, cs), wrf(s + 0x138, sn), wrf(s + 0x13C, cs);
        wrf(s + 0x150, -tw), wrf(s + 0x154, tw);
        // (not the viewport node copy at +0x40: the Real3D nodes the scene is
        // drawn from keep the 4:3 projection, the renderer widens the view)
        sl.wrote = tw, sl.factor = f;
    }
    g_wide_applied = want;
}


void video_snapshot() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    for (uint32_t off = 0; off < TILEGEN_SIZE; off += 0x10000) {
        auto it = g_dev.find((TILEGEN_BASE + off) >> 16);
        if (it != g_dev.end()) memcpy(&g_video_vram[off], it->second.data(), std::min<uint32_t>(0x10000, TILEGEN_SIZE - off));
    }
    g_video_frame = g_frame;
}

double g_wait_seconds = 0;   // time spent waiting since the last frame event (game thread only)

// Keeps the game at the original speed (vblank at FRAME_HZ)
void pace_frame() {
    double wait_from = qpc_seconds();
    struct Done {   // whichever way pace_frame returns, the wait is counted
        double from;
        ~Done() { g_wait_seconds += qpc_seconds() - from; }
    } done{wait_from};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double target = double(g_frame) / FRAME_HZ;
    double elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    if (elapsed > target + 0.25) {   // far behind (e.g. window being dragged): don't try to catch up
        g_start_time.QuadPart = now.QuadPart - LONGLONG(target * double(g_qpf.QuadPart));
        return;
    }
    while (elapsed < target) {
        if (target - elapsed > 0.002) Sleep(1);
        QueryPerformanceCounter(&now);
        elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    }
}

ULONGLONG g_boot_ms = GetTickCount64();

// Exit from the game thread itself: nothing else pushes audio, so it is
// stopped here before the process goes away
[[noreturn]] void exit_from_game_thread(int code) {
    fflush(stdout);
    g_audio.stop();
    ExitProcess(code);
}

// The SEGA logo is the first boot screen that fills the screen (grey backdrop);
// the ones before it are a few lines of text on black
bool boot_logo_up() {
    static std::vector<uint32_t> back(BACK_W * SCREEN_H), front(SCREEN_W * SCREEN_H), raw(SCREEN_W * SCREEN_H);
    TileGen::compose(g_video_vram.data(), {.back = back.data(), .front = front.data(), .front_raw = raw.data()});
    int lit = 0;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t b = back[y * BACK_W + BACK_MARGIN + x], f = raw[y * SCREEN_W + x];
            lit += ((f >> 24) && (f & 0xFFFFFF)) || ((b >> 24) && (b & 0xFFFFFF));
        }
    return lit * 4 > SCREEN_W * SCREEN_H;
}
}  // namespace

bool g_fast_idle = false;

// PLAY from the menu (app.cpp): the game, held at its SEGA logo, runs on
// unseen, with a coin and START every second (the logo takes neither), until
// its mode byte (0x1BC85B: 0 attract, 1 credit taken, 4 mission select,
// 9 playing) reaches the mission select; true once the mission select is up
//
// QUIT TO MAIN MENU (g_arcade_entry 3; Star Wars Trilogy has no state point to
// put a home state back from): the game is over at once, its shield
// (0x22159A) kept at 0 until it is back in the attract, and on to the mission
// select again, all behind the menu
// DEBUG (menu): from the mission select on to a phase of a stage, unseen.
// The game's top state (0x2210E8) runs a stage as a pair (init, run):
// 2/3 Yavin, 4/5 Hoth, 6/7 Endor, 8/9 a second space stage, 10/11 and 12/13
// the stages of area 0x341. A stage's run state steps through a script whose
// step is 0x221136 (each step a function of a table, e.g. Hoth's at
// 0x14631C): a phase plays in one step (Hoth: 10 snow, 18 tunnels, 24
// hangar; the space stages 11, 19 / 21, 27 / 28), and the steps after it end
// the phase (its closing scene, the next phase's textures and models, the
// fade). The stage the mission select starts is the one asked for (its
// entry is set), and once in a phase's play the script is moved on to the
// step after it, phase by phase, so each one ends and the next is set up as
// in a game played through
namespace {
struct DebugStageSteps {
    uint32_t init;
    int play[3];   // the step each phase plays in
};
const DebugStageSteps DEBUG_STEPS[6] = {
    {2, {11, 19, 27}},   // Yavin
    {4, {10, 18, 24}},   // Hoth
    {6, {10, 18, 24}},   // Endor
    {8, {11, 21, 28}},   // space stage 2
    {10, {0, 0, 0}},
    {12, {0, 0, 0}},
};
}  // namespace

static bool debug_jump_frame(uint64_t from) {
    stats_game_cheated();   // a game jumped through: no records
    static int jumps = 0;
    static uint64_t wait_at = 0;
    int choice = g_debug_jump;
    const DebugStageSteps& d = DEBUG_STEPS[std::clamp(choice / 4, 0, 5)];
    int phase = std::clamp(choice % 4, 0, 2);
    PlayerInputs& p = g_inputs.p[0];
    uint32_t state = rd32(0x2210E8);
    int step = int16_t(rd16(0x221136));
    auto finish = [&](const char* why) {
        printf("[debug] %s: state %u step %d after %llu frames\n", why, state, step, (unsigned long long)(g_frame - from));
        jumps = 0, wait_at = 0;
        g_debug_jump = -1;
        return true;
    };
    if (g_frame - from > 30000) return finish("gave up");
    // The mission select's entry (0x23B53C: 0 Hoth, 1 Yavin, 2 Endor) picks
    // the stage state and the episode (0x221548 bits 12-15: 1 Yavin, 2 Hoth,
    // 3 Endor), and the episode the textures: the select itself is set to the
    // stage asked for, so it starts with its own textures
    static const int SELECT_ENTRY[6] = {1, 0, 2, -1, -1, -1};
    int entry = SELECT_ENTRY[std::clamp(choice / 4, 0, 5)];
    if (state < 2 || state > 13) {   // the mission select: the entry, then the trigger
        if (entry >= 0) wr32(0x23B53C, uint32_t(entry));
        p.trigger = (g_frame - from) % 60 < 6;
        return false;
    }
    if (!(state & 1)) {   // a stage about to start: the one asked for (the later stages: no entry of their own)
        if (state != d.init) {
            printf("[debug] stage state %u -> %u\n", state, d.init);
            wr32(0x2210E8, d.init);
        }
        return false;
    }
    if (state != d.init + 1) return false;
    // The step alone moves the script on but not the scenery: the phase is
    // also counted in these bytes (F9 RAM of Yavin's three phases played: one
    // more, or two more, each phase). Kept at the phase's value from the jump
    // until the phase plays
    struct PhaseByte { uint32_t addr; int per_phase; };
    static const PhaseByte PHASE_BYTES[] = {{0x221269, 1}, {0x221275, 1}, {0x221547, 1}, {0x221549, 1}, {0x22154A, 1},
                                            {0x221555, 2}, {0x23B4D9, 2}};
    constexpr int PHASE_BYTE_COUNT = int(sizeof(PHASE_BYTES) / sizeof(PHASE_BYTES[0]));
    static uint8_t base[PHASE_BYTE_COUNT];
    if (jumps > 0)
        for (int i = 0; i < PHASE_BYTE_COUNT; i++)
            wr8(PHASE_BYTES[i].addr, uint8_t(base[i] + jumps * PHASE_BYTES[i].per_phase));
    if (jumps < phase) {
        if (step == d.play[jumps]) {
            if (!wait_at) wait_at = g_frame;
            if (g_frame - wait_at >= 30) {
                printf("[debug] phase %d playing (step %d): on to step %d\n", jumps + 1, step, step + 1);
                if (jumps == 0)
                    for (int i = 0; i < PHASE_BYTE_COUNT; i++) base[i] = rd8(PHASE_BYTES[i].addr);
                wr16(0x221136, uint16_t(step + 1));
                jumps++;
                wait_at = 0;
            }
        }
        return false;
    }
    if (step < d.play[phase]) return false;
    return finish("in the phase");
}

// The game's main loop (0x29C0) runs a sequence, its step at 0x2210FC
// (table 0x118C40): 0 attract init (0x16F4), 1 the attract (0x179C: title,
// rankings, demo; its own states at 0x2210E0), 2 game init (0x24DC: top state
// 0x2210E8 to 0), 3 the game (0x256C: the top states, the mission select's
// credit state 1 included). The last game over state (50, 0x25D4) only steps
// the sequence on, to 4, an empty entry that wraps it to 0: back in the
// attract. The mode byte is not a sure sign of it: it can stay at 9 through
// the attract's title and rankings (it goes to 0 only once the attract's demo
// game starts), so a game over waited on by the mode byte alone left the
// title on screen. In the attract: the mode byte at 0, or the sequence out
// of its game steps (2, 3) with the mode byte left from a game (6-9; 1-4 is
// a credit being taken)
static bool in_attract() {
    uint8_t mode = rd8(0x1BC85B);
    uint32_t seq = rd32(0x2210FC);
    return mode == 0 || (seq != 2 && seq != 3 && mode >= 6);
}

// DOGFIGHT (src/dogfight): its frames, published as the game's are. The set
// is the game's last frame (its Real3D memory: viewports, textures' places,
// the star field); the scene is the simulation's
static void dogfight_publish(double time) {
    auto state = std::make_shared<R3DFrameState>();
    state->time = state->flushed = time;
    state->vblank = g_frame;
    state->native = dogfight_scene();
    state->frame = ++g_r3d_frames;
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        if (g_r3d_latest) state->pages = g_r3d_latest->pages;
        g_r3d_latest = state;
    }
    g_r3d_cv.notify_all();
}

void dogfight_begin() {
    dogfight_reset();
    g_dogfight = 2;
    dogfight_publish(qpc_seconds());
    printf("[dogfight] flying (frame %llu, %d ships)\n", (unsigned long long)g_frame, dogfight_ships());
    fflush(stdout);
}

// Its sounds are the game's own: the commands the game sends to the sound
// board for each, as seen in a game of Yavin (SWT_MIDILOG): A0, a sound or
// a voice, and how. The player's cannons are A0 23 with 01 and 00 in turn
// (twice each: the four cannons, side by side); what the enemy fighters do
// comes on voices 12 to 22: 0A is a TIE fighter's shot, 02 and 03 are two
// explosions (02 the louder: a kill; 03: a hit on a fighter that takes more)
// and 08 is a shot striking something it does not harm; all heard by ear. 09, 0B and 0E gave no sound alone.
// SWT_DOGFIGHT_SOUND=laser,hit,kill gives others, each as hex bytes ("" none): to try them out
static void dogfight_sounds(int which) {
    static std::vector<uint8_t> bytes[3];
    static const bool given = [] {
        const char* e = getenv("SWT_DOGFIGHT_SOUND");
        for (int k = 0; e && k < 3; k++) {
            unsigned v;
            int n;
            while (sscanf(e, " %2x%n", &v, &n) == 1) bytes[k].push_back(uint8_t(v)), e += n;
            if (*e == ',') e++;
        }
        return e != nullptr;
    }();
    if (given) {
        for (int k = 0; k < 3; k++)
            if (which >> k & 1)
                for (uint8_t b : bytes[k]) g_sound.midi_write(b);
        return;
    }
    static unsigned shots = 0, kills = 0, hits = 0;
    if (which & DOGFIGHT_SOUND_LASER) {
        const uint8_t m[3] = {0xA0, 0x23, uint8_t(shots++ >> 1 & 1 ? 0x00 : 0x01)};
        for (uint8_t b : m) g_sound.midi_write(b);
    }
    if (which & DOGFIGHT_SOUND_KILL) {
        const uint8_t m[3] = {0xA0, uint8_t(0x14 + (kills & 3)), 0x02};
        kills++;
        for (uint8_t b : m) g_sound.midi_write(b);
    } else if (which & (DOGFIGHT_SOUND_HIT | DOGFIGHT_SOUND_DAMAGE)) {
        const uint8_t m[3] = {0xA0, uint8_t(0x18 + (hits++ & 3)), uint8_t(which & DOGFIGHT_SOUND_DAMAGE ? 0x03 : 0x08)};
        for (uint8_t b : m) g_sound.midi_write(b);
    }
}

void dogfight_run() {
    const double tick = 1.0 / FRAME_HZ;
    double next = qpc_seconds();
    while (g_dogfight == 2 && !g_quit) {
        if (g_hold) {   // the pause menu
            Sleep(4);
            next = qpc_seconds();
            continue;
        }
        double now = qpc_seconds();
        if (now < next) {
            if (next - now > 0.002) Sleep(1);
            continue;
        }
        if (now - next > 0.25) next = now;   // far behind (the window dragged): no catching up
        dogfight_step(float(tick));
        dogfight_sounds(dogfight_take_sounds());
        g_frame++;          // the vblank the 2D layers go by
        video_snapshot();   // (blanked while flying: app.cpp, compose_2d)
        dogfight_publish(next);
        next += tick;
        g_sound.run_frame(FRAME_HZ);   // the stage's music plays on
    }
    printf("[dogfight] left (frame %llu)\n", (unsigned long long)g_frame);
    fflush(stdout);
}

static bool arcade_entry_frame() {
    static uint64_t from = 0, select_at = 0, settle = 0;
    PlayerInputs& p = g_inputs.p[0];
    p.trigger = p.offscreen = p.event = false;
    uint8_t mode = rd8(0x1BC85B);
    bool attract = in_attract();
    if (g_arcade_entry == 3) {
        from = g_frame, select_at = 0;
        g_arcade_entry = mode == 4 ? 2 : 4;   // at the mission select already: nothing to leave
    }
    if (g_arcade_entry == 4) {
        p.coin = p.start = false;
        if (!attract && g_frame - from < 12000) {
            if (swt_playing()) {   // the shield (0x22159A, player struct; 0x1BC974 a copy)
                wr16(0x22159A, 0);
                wr32(0x1BC974, 0);
            }
            return false;
        }
        printf("[play] back in the attract after %llu frames (mode %u, sequence %u)\n", (unsigned long long)(g_frame - from),
               unsigned(mode), unsigned(rd32(0x2210FC)));
        g_arcade_entry = 1;
        settle = 180;   // the attract clears the game over's screens (CONTINUE?) first
    }
    if (g_arcade_entry == 1) {
        g_arcade_entry = 2;
        from = g_frame, select_at = 0;
    }
    if (attract && g_frame - from < settle) {
        p.coin = p.start = false;
        return false;
    }
    if (attract && g_frame - from < 6000 + settle) {
        uint64_t k = (g_frame - from - settle) % 60;
        p.coin = k < 6;
        p.start = k >= 30 && k < 36;
        return false;
    }
    p.coin = p.start = false;
    // 1-3: the credit taken, the SEGA logo or the game's start screens still
    // up. Done only at the mission select (4) or past it (a DEBUG jump); the
    // menu is never left over anything else, short of giving up
    if (mode != 4 && !((g_debug_jump >= 0 || g_dogfight == 1) && select_at) && g_frame - from < 9000) return false;
    if (!select_at) {
        select_at = g_frame;
        printf("[play] mission select after %llu frames (mode %u)%s\n", (unsigned long long)(g_frame - from), unsigned(mode),
               mode == 4 ? "" : ": not reached, given up");
    }
    if (g_frame - select_at < 20) return false;   // its first frames drawn
    if (g_debug_jump >= 0 && !debug_jump_frame(select_at)) return false;
    // DOGFIGHT: the stage it flies over is Yavin's first phase, a second and
    // a half into its play (everything of the phase loaded and drawn once)
    if (g_dogfight == 1) {
        static uint64_t playing_at = 0;
        bool playing = rd32(0x2210E8) == 3 && int16_t(rd16(0x221136)) == 11;
        if (!playing && g_frame - select_at < 6000) {
            playing_at = 0;
            return false;
        }
        if (!playing_at) playing_at = g_frame;
        if (playing && g_frame - playing_at < 90) return false;
        playing_at = 0;
        if (playing) dogfight_begin();
        else {
            printf("[dogfight] the stage was not reached: given up\n");
            g_dogfight = 0;
        }
    }
    g_arcade_entry = 0;
    settle = 0;
    return true;
}

// The attract never shows with the menu on: a game that ends (no credit at
// the CONTINUE? count, the game over, the ending and its name entry) goes
// back to the attract (its sequence out of the game steps). On that frame,
// before its picture is taken, the run on to the mission select starts behind the main menu,
// unseen and muted, as QUIT TO MAIN MENU does (arcade_entry_frame from entry
// 3), so the attract's title, rankings and demo never get on screen
static void attract_guard_frame() {
    static bool was = true;
    static uint32_t seq_was = 0;
    uint32_t seq = rd32(0x2210FC);
    if (seq != seq_was) printf("[play] sequence %u -> %u (mode %u, frame %llu)\n", unsigned(seq_was), unsigned(seq),
                               unsigned(rd8(0x1BC85B)), (unsigned long long)g_frame);
    seq_was = seq;
    // The game is over only once its sequence leaves the game steps (2, 3):
    // the last game over state steps it on to 4, which wraps to 0. The mode
    // byte is left out here: a game goes on over several missions (one
    // cleared, back to the mission select for the next, the duels after the
    // second and third, then the last stage), and a mode byte at 0 in
    // between was taken for a game over, so a cleared mission ended the game
    // at the main menu (issue #6)
    bool attract = seq != 2 && seq != 3;
    bool ended = !was && attract;
    was = attract;
    if (!ended || !g_skip_attract || g_arcade_entry || g_boot_skip) return;
    printf("[play] the game is over: back to the menu, on to the mission select unseen (frame %llu)\n",
           (unsigned long long)g_frame);
    g_boot_skip = g_fast_idle = true;
    g_sound.set_dsp_enabled(false);
    g_sound.set_muted(true);
    g_arcade_entry = 3;
    g_game_over_menu = true;
}

// The lever's force feedback as gamepad rumble: the drive board drives two
// motors (0x7F = no force). Their slow part is the spring that holds the
// lever to the middle (it follows the lever); the quick changes on top are
// the game's jolts and shakes (hits, crashes, the sabres clashing), kept for
// the pads (app.cpp). SWT_FFB_LOG prints the motors as they change
static void ffb_frame() {
    static const bool log = getenv("SWT_FFB_LOG") != nullptr;
    static float slow[2] = {0, 0};
    static int last[2] = {-1, -1};
    int m[2] = {g_drive.motor_x(), g_drive.motor_y()};
    float shake = 0;
    for (int a = 0; a < 2; a++) {
        float f = float(m[a] - 0x7F);
        slow[a] += 0.2f * (f - slow[a]);
        shake = std::max(shake, std::fabs(f - slow[a]) / 128.0f);
    }
    if (log && (m[0] != last[0] || m[1] != last[1]))
        printf("[ffb] frame %llu  x %02X y %02X  shake %.2f  lever %d %d\n", (unsigned long long)g_frame, m[0], m[1], shake,
               int(g_inputs.p[0].gun_x), int(g_inputs.p[0].gun_y));
    last[0] = m[0], last[1] = m[1];
    float was = g_ffb_shake.load();
    if (shake > was) g_ffb_shake = shake;
}

void rt_event(Ctx& c) {
    if (c.cycles >= g_next_sample) {
        g_samples[c.pc_hint]++;
        g_next_sample = c.cycles + SAMPLE_CYCLES;
    }
    while (c.cycles >= g_next_phase) {
        switch (g_phase) {
        case 0: {   // start of VBlank: new frame
            g_drive.set_stick(uint8_t(g_inputs.p[0].gun_x), uint8_t(g_inputs.p[0].gun_y));
            g_drive.run_frame();
            ffb_frame();
            widen_fov();
            g_frame++;
            g_frame_start = g_next_phase;
            tilegen_dump_frame(g_frame);
            {   // CPU time of the frame just finished, without the waits
                static double last_frame_time = 0;
                double now_s = qpc_seconds();
                if (last_frame_time > 0) {
                    g_game_busy_us += uint64_t(std::max(now_s - last_frame_time - g_wait_seconds, 0.0) * 1e6);
                    g_game_frames++;
                }
                last_frame_time = now_s;
                g_wait_seconds = 0;
            }
            if (g_frame % 60 == 0) {
                static ULONGLONG last_ms = GetTickCount64();
                ULONGLONG now = GetTickCount64();
                double fps = now > last_ms ? 60000.0 / double(now - last_ms) : 0.0;
                last_ms = now;
                printf("  frame %llu  (pc ~%08X)  %.0f fps\n", (unsigned long long)g_frame, c.pc_hint, fps);
                fflush(stdout);
            }
            g_auto.apply(g_frame);
            attract_guard_frame();   // before the frame's picture is taken
            g_in_attract = in_attract();
            stats_frame();
            video_snapshot();
            bool boot_done = g_arcade_entry ? arcade_entry_frame() : (g_frame % 4 == 0 && boot_logo_up()) || g_frame > 3000;
            if (g_boot_skip && boot_done) {
                g_boot_skip = g_fast_idle = false;
                g_sound.set_dsp_enabled(true);
                g_sound.set_muted(false);   // fades in from silence
                printf("[boot] start screens skipped in %.1f s (frame %llu)\n",
                       double(GetTickCount64() - g_boot_ms) / 1000.0, (unsigned long long)g_frame);
                LARGE_INTEGER now;   // real-time pacing starts from here
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_hold && !g_boot_skip) {
                double hold_from = qpc_seconds();
                while (g_hold && !g_quit && !g_boot_skip) Sleep(4);   // a skip run (PLAY, QUIT) goes on behind the menu
                g_wait_seconds += qpc_seconds() - hold_from;
                LARGE_INTEGER now;   // carry on at real speed from here, without catching up
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_dogfight == 2 && !g_boot_skip) {   // DOGFIGHT: the game stands still while it is flown
                dogfight_run();
                LARGE_INTEGER now;   // carry on at real speed from here, without catching up
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_realtime && !g_boot_skip) pace_frame();
            frame_log("G %llu %.6f\n", (unsigned long long)g_frame, qpc_seconds());
            if (g_frame >= uint64_t(g_frame_limit)) {
                report();
                exit_from_game_thread(0);
            }
            if (g_quit) {   // the window is closing: stand still until the process ends
                g_parked = true;
                for (;;) Sleep(INFINITE);
            }
            {
                double t_sound = qpc_seconds();
                g_sound.run_frame(FRAME_HZ);
                double scsp_ms, dsb_ms;
                uint32_t midi;
                g_sound.frame_times(scsp_ms, dsb_ms, midi);
                frame_log("A %llu %.3f %.3f %.3f %u\n", (unsigned long long)g_frame, (qpc_seconds() - t_sound) * 1000, scsp_ms, dsb_ms, midi);
            }
            g_midi_pulses = (g_midi_ctrl & 0x20) ? 128 : 0;
            g_phase = 1;
            g_next_phase = g_frame_start + 40 * LINE_CYCLES;
            break;
        }
        case 1:   // end of VBlank
            g_irq_state |= 0x0D;
            g_midi_pulses = 0;
            g_phase = 2;
            g_next_phase = g_frame_start + (40 + std::min<uint32_t>(383, flip_line())) * LINE_CYCLES;
            break;
        case 2:   // Real3D ping-pong flip (line set in tilegen register 0x08)
            g_real3d_status ^= 0x00000002;   // the game reads it with lwz+stwbrx: bit 0x02000000 after the swap
            g_phase = 3;
            g_next_phase = g_frame_start + (40 + 383) * LINE_CYCLES;
            break;
        case 3:   // start of the last active line: VBlank interrupt
            g_irq_state |= 0x02;
            g_phase = 0;
            g_next_phase = g_frame_start + FRAME_CYCLES;
            break;
        }
    }
    // MIDI interrupts while the sound board wants commands (during VBlank)
    if (g_midi_pulses > 0 && (g_midi_ctrl & 0x20) && (g_irq_enable & 0x40) && !(g_irq_state & 0x40) &&
        c.cycles >= g_midi_next) {
        g_irq_state |= 0x40;
        g_midi_pulses--;
        g_midi_next = c.cycles + 1000;
    }
    if (!g_in_irq && (c.msr & MSR_EE)) {
        if (g_dec_armed && c.cycles >= g_dec_base + uint64_t(g_dec_value) * 4) {
            g_dec_armed = false;
            g_dec_irqs++;
            exception(c, 0x900);
        }
        uint8_t pend = g_irq_state & g_irq_enable;
        if (pend || g_dma_irq) {
            for (int i = 0; i < 8; i++) if (pend & (1 << i)) g_irq_count[i]++;
            exception(c, 0x500);
        }
    }
    schedule(c);
}

uint32_t rt_mfspr(Ctx& c, uint32_t spr) {
    switch (spr) {
    case 22: return g_dec_value - uint32_t((c.cycles - g_dec_base) / 4);
    case 268: return uint32_t((c.cycles - g_tb_base) / 4);
    case 269: return uint32_t(((c.cycles - g_tb_base) / 4) >> 32);
    default: return c.spr[spr];
    }
}

void rt_mtspr(Ctx& c, uint32_t spr, uint32_t v) {
    if (spr == 22) {
        g_dec_value = v;
        g_dec_base = c.cycles;
        g_dec_armed = !(v & 0x80000000u);
        schedule(c);
        return;
    }
    if (spr == 284) {   // TBL: only used to reset it to 0
        g_tb_base = c.cycles - uint64_t(v) * 4;
        return;
    }
    c.spr[spr] = v;
}

void rt_mtmsr(Ctx& c, uint32_t v) {
    c.msr = v;
    schedule(c);
}

void rt_lswi(Ctx& c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, i = 0;
    while (nb > 0) {
        if (i == 0) { r = (r + 1) & 31; c.r[r] = 0; }
        c.r[r] |= uint32_t(rd8(ea++)) << (24 - i);
        i = (i + 8) & 31;
        nb--;
    }
}

void rt_stswi(Ctx& c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, i = 0;
    while (nb > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea++, uint8_t(c.r[r] >> (24 - i)));
        i = (i + 8) & 31;
        nb--;
    }
}

// The game turns the lever's x into the sight's yaw (fn_0007C894: x * 28,
// 0x10000 = 360 degrees, about +-19.5 degrees at full tilt: the 4:3 screen).
// With the view widened for widescreen the yaw is widened too, keeping the
// sight at the same fraction of the (wider) screen: tan(yaw) * the factor.
// SWT_NARROW_AIM keeps the original travel. The lever's 256 steps are also
// filled in (each one was a visible jump of the sight with a pad or mouse):
// the game's step (its table maps the lever about 1:1, inverted or not) moves
// on by the fraction it did not see. SWT_LEVER_STEPS keeps the steps
static double sight_reach(int axis) {
    const double half = axis ? 0.125 : 0.097;   // the sight's half size over the screen's (F9 of Yavin)
    double k = std::clamp(double(g_sight_scale.load()), 0.3, 1.0);
    return (1.0 - k * half) / (1.0 - half);
}

uint32_t rt_aim_angle(int axis, uint32_t angle) {
    static const bool narrow = getenv("SWT_NARROW_AIM") != nullptr, steps = getenv("SWT_LEVER_STEPS") != nullptr;
    const int mul = axis ? 21 : 28;
    double a = double(int32_t(angle));
    if (!steps) {
        int v = int32_t(angle) / mul;   // the game's signed step, -128..127
        int raw = axis ? g_inputs.p[0].gun_y : g_inputs.p[0].gun_x;
        static double k[2] = {-1.0, -1.0};   // the lever's direction (inverted by default), as last seen
        if (v != 0 && raw != 128) k[axis] = ((v > 0) == (raw > 128)) ? 1.0 : -1.0;
        a += k[axis] * lever_fraction(axis) * mul;
    }
    float w = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    // The full tilt puts the sight's edge on the screen's (its half size is
    // about a tenth of the half width, an eighth of the half height): drawn
    // smaller (SIGHT SIZE), it goes that much farther so it still gets there
    double reach = sight_reach(axis);
    if (narrow) w = 1.0f;
    if (w > 1.001f || reach > 1.0001) {
        constexpr double UNIT = 6.283185307179586 / 65536.0;
        a = std::atan(std::tan(a * UNIT) * (axis == 0 ? w : 1.0f) * reach) / UNIT;
    }
    return uint32_t(int32_t(std::lround(a)));
}

// On the ground (Hoth, Endor) the sight is a position in front of the camera:
// fn_00053F78 turns the lever (negated x, y; 0x2260E0) into x * 0.647 and
// y * 0.490 (constants at 0x1B6CF8). x is widened like the view, so the
// sight reaches the sides of a widescreen screen (SWT_NARROW_AIM keeps it),
// and both get the lever's precision between steps (SWT_LEVER_STEPS keeps the steps)
double rt_ground_aim(int axis, double v) {
    static const bool narrow = getenv("SWT_NARROW_AIM") != nullptr, steps = getenv("SWT_LEVER_STEPS") != nullptr;
    double mul = f2d(rd32(axis ? 0x1B6CFC : 0x1B6CF8));
    if (!steps && mul != 0) {
        int step = int(std::lround(v / mul));   // the game's signed step
        int raw = axis ? g_inputs.p[0].gun_y : g_inputs.p[0].gun_x;
        static double k[2] = {-1.0, 1.0};   // the step's direction against the lever's, as last seen
        if (step != 0 && raw != 128) k[axis] = ((step > 0) == (raw > 128)) ? 1.0 : -1.0;
        v += k[axis] * lever_fraction(axis) * mul;
    }
    float w = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    if (axis == 0 && !narrow) v *= w;
    return v * sight_reach(axis);
}

