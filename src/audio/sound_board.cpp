#include "sound_board.h"
#include "engine_sound.h"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio_out.h"
#include "audio_stream.h"

extern "C" {
#include "m68k.h"
}

namespace {
// The SCSP holds its interrupt line until the program acknowledges it, so
// taking the interrupt leaves the level set (Musashi's default ack, enabled
// for the DSB2, would clear it)
int scsp_int_ack(int) { return M68K_INT_ACK_AUTOVECTOR; }

SoundBoard* g_board = nullptr;

constexpr uint32_t kRamSize = 0x100000;   // 1 MB per SCSP
}  // namespace

extern Dsb2* g_dsb_bus;   // set while the DSB2's 68000 runs (dsb2.cpp)

// Musashi bus callbacks: the sound board's 68000, or the DSB2's while it runs
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) {
    return g_dsb_bus ? g_dsb_bus->read8(a & 0xFFFFFF) : g_board->read8(a & 0xFFFFFF);
}
unsigned int m68k_read_memory_16(unsigned int a) {
    return g_dsb_bus ? g_dsb_bus->read16(a & 0xFFFFFF) : g_board->read16(a & 0xFFFFFF);
}
unsigned int m68k_read_memory_32(unsigned int a) {
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (g_dsb_bus) g_dsb_bus->write8(a & 0xFFFFFF, uint8_t(v));
    else g_board->write8(a & 0xFFFFFF, uint8_t(v));
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    if (g_dsb_bus) g_dsb_bus->write16(a & 0xFFFFFF, uint16_t(v));
    else g_board->write16(a & 0xFFFFFF, uint16_t(v));
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}
}

bool SoundBoard::init(std::vector<uint8_t> program, std::vector<uint8_t> samples, AudioOut* out) {
    if (program.empty()) return false;
    program_ = std::move(program);
    samples_ = std::move(samples);
    out_ = out;
    const bool dbg = getenv("LW_SNDDBG") != nullptr;
    for (int i = 0; i < 2; i++) {
        ram_[i].assign(kRamSize, 0);
        scsp_[i].init(ram_[i].data(), kRamSize, i == 0);
    }
    if (dbg) fprintf(stderr, "[snd] scsp ok\n");
    // Reset: the first 16 bytes of the program ROM (SSP, PC) are copied to RAM
    memcpy(ram_[0].data(), program_.data(), 16);
    g_board = this;
    m68k_init();
    m68k_set_int_ack_callback(scsp_int_ack);
    if (dbg) fprintf(stderr, "[snd] m68k_init ok\n");
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    if (dbg) fprintf(stderr, "[snd] reset ok\n");
    ready_ = true;
    return true;
}

// Layout: both RAMs, both SCSPs, the 68000 context, ctrl_. The SCSPs keep
// their own RAM pointers; the 68000 context carries pointers into this
// program (cycle tables, callbacks), set again after it is put back.
void SoundBoard::save_state(std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready_) return;
    size_t ctx = m68k_context_size();
    out.resize(2 * kRamSize + 2 * sizeof(Scsp) + ctx + 1);
    uint8_t* p = out.data();
    for (int i = 0; i < 2; i++, p += kRamSize) memcpy(p, ram_[i].data(), kRamSize);
    for (int i = 0; i < 2; i++, p += sizeof(Scsp)) memcpy(p, &scsp_[i], sizeof(Scsp));
    m68k_get_context(p);
    p[ctx] = ctrl_;
}

bool SoundBoard::load_state(const std::vector<uint8_t>& in) {
    size_t ctx = m68k_context_size();
    if (!ready_ || in.size() != 2 * kRamSize + 2 * sizeof(Scsp) + ctx + 1) return false;
    const uint8_t* p = in.data();
    for (int i = 0; i < 2; i++, p += kRamSize) memcpy(ram_[i].data(), p, kRamSize);
    for (int i = 0; i < 2; i++, p += sizeof(Scsp)) {
        Scsp chip;
        memcpy(static_cast<void*>(&chip), p, sizeof(Scsp));
        scsp_[i].load_state(chip);
    }
    std::vector<uint8_t> cpu(p, p + ctx);
    m68k_set_context(cpu.data());
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);   // the cycle tables of this run
    m68k_set_int_ack_callback(scsp_int_ack);
    m68k_set_bkpt_ack_callback(nullptr);
    m68k_set_reset_instr_callback(nullptr);
    m68k_set_pc_changed_callback(nullptr);
    m68k_set_tas_instr_callback(nullptr);
    m68k_set_illg_instr_callback(nullptr);
    m68k_set_trap_instr_callback(nullptr);
    m68k_set_fc_callback(nullptr);
    m68k_set_instr_hook_callback(nullptr);
    ctrl_ = p[ctx];
    sample_acc_ = 0;
    return true;
}

uint8_t SoundBoard::read8(uint32_t a) {
    switch ((a >> 20) & 0xF) {
    case 0x0: return ram_[0][a & 0xFFFFF];
    case 0x1: {
        uint8_t v = scsp_[0].read8(a & 0xFFFF);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));   // reading MIDI-in may drop the interrupt
        return v;
    }
    case 0x2: return ram_[1][a & 0xFFFFF];
    case 0x3: return scsp_[1].read8(a & 0xFFFF);
    case 0x6: return program_[a & (program_.size() - 1)];
    default:
        if (a >= 0x800000 && !samples_.empty()) return samples_[(a & 0x7FFFFF) % samples_.size()];
        return 0;
    }
}

uint16_t SoundBoard::read16(uint32_t a) {
    switch ((a >> 20) & 0xF) {
    case 0x1: {
        uint16_t v = scsp_[0].read16(a & 0xFFFF);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        return v;
    }
    case 0x3: return scsp_[1].read16(a & 0xFFFF);
    default: return uint16_t(read8(a) << 8 | read8(a + 1));
    }
}

void SoundBoard::write8(uint32_t a, uint8_t v) {
    switch ((a >> 20) & 0xF) {
    case 0x0: ram_[0][a & 0xFFFFF] = v; break;
    case 0x1:
        scsp_[0].write8(a & 0xFFFF, v);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));   // e.g. SCIRE acknowledges
        break;
    case 0x2: ram_[1][a & 0xFFFFF] = v; break;
    case 0x3: scsp_[1].write8(a & 0xFFFF, v); break;
    case 0x4:
        if (a == 0x400001) ctrl_ = v;   // sample ROM bank (no effect with 8 MB of samples)
        break;
    default: break;
    }
}

void SoundBoard::write16(uint32_t a, uint16_t v) {
    switch ((a >> 20) & 0xF) {
    case 0x1:
        scsp_[0].write16(a & 0xFFFF, v);
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        break;
    case 0x3: scsp_[1].write16(a & 0xFFFF, v); break;
    default:
        write8(a, uint8_t(v >> 8));
        write8(a + 1, uint8_t(v));
    }
}

void SoundBoard::run_frame(double frame_hz) {
    if (!ready_) return;
    Scsp::explosion_frame();   // EXPLOSIONS: ENHANCED: where on the screen the new ones are
    sample_acc_ += 44100.0 / frame_hz;
    int samples = int(sample_acc_);
    sample_acc_ -= samples;
    // Four channels as on the cabinet: the master SCSP drives the front
    // speakers and the slave the rear ones. Full scale = 1.
    buffer_.resize(size_t(samples) * AudioStream::kInChannels);
    const float scale = gain_ / 32768.0f;
    auto t_scsp = std::chrono::steady_clock::now();
    for (int i = 0; i < samples; i++) {
        float* f = &buffer_[size_t(i) * AudioStream::kInChannels];
        scsp_[0].render(f[0], f[1]);
        scsp_[1].render(f[2], f[3]);
        for (int c = 0; c < AudioStream::kInChannels; c++) f[c] *= c < 2 ? scale : scale * rear_gain_;
        scsp_[0].tick_timers();
        m68k_set_irq(unsigned(scsp_[0].irq_level()));
        int budget = 256 - overshoot_;
        overshoot_ = budget > 0 ? m68k_execute(budget) - budget : -budget;
    }
    auto t_dsb = std::chrono::steady_clock::now();
    // The DSB2's music, on the front pair (it gets the same commands)
    if (dsb_.ready()) dsb_.run(samples, buffer_.data(), AudioStream::kInChannels, music_gain_ / 32768.0f);
    // DOGFIGHT: the player's engines (silent, and no work, outside it)
    engine_sound_mix(buffer_.data(), samples, AudioStream::kInChannels, gain_ * 0.08f);
    auto t_end = std::chrono::steady_clock::now();
    last_scsp_ms_ = std::chrono::duration<double, std::milli>(t_dsb - t_scsp).count();
    last_dsb_ms_ = std::chrono::duration<double, std::milli>(t_end - t_dsb).count();
    if (wav_) {
        std::vector<int16_t> pcm(size_t(samples) * 2);
        for (size_t i = 0; i < size_t(samples); i++) {   // the stereo mix, as the output plays it
            const float* f = &buffer_[i * AudioStream::kInChannels];
            float st[2] = {(f[0] + f[2]) * 0.5f, (f[1] + f[3]) * 0.5f};
            wav_limiter_.process(st, 2);
            for (int c = 0; c < 2; c++) pcm[i * 2 + c] = int16_t(std::lround(soft_limit(st[c]) * 32767.0f));
        }
        fwrite(pcm.data(), 4, size_t(samples), wav_);
        wav_frames_ += uint32_t(samples);
    }
    if (out_) {
        if (muted_) {
            was_muted_ = true;
            return;
        }
        if (was_muted_) {   // back from silence: no click
            was_muted_ = false;
            fade_ = 882;
        }
        for (int i = 0; i < samples && fade_ > 0; i++, fade_--) {
            float k = 1.0f - float(fade_) / 882.0f;
            float* f = &buffer_[size_t(i) * AudioStream::kInChannels];
            for (int c = 0; c < AudioStream::kInChannels; c++) f[c] *= k;
        }
        out_->push(buffer_.data(), size_t(samples));   // the output keeps the latency bounded
        frames_out_ += uint64_t(samples);
    }
}

void SoundBoard::dump_wav(const char* path) {
    wav_ = fopen(path, "wb");
    if (!wav_) return;
    wav_limiter_.set_rate(44100);
    uint8_t header[44] = {};
    fwrite(header, 1, 44, wav_);   // filled in by close_wav
}

void SoundBoard::close_wav() {
    if (!wav_) return;
    uint32_t data = wav_frames_ * 4;
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, wav_); };
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, wav_); };
    fseek(wav_, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav_);
    put32(36 + data);
    fwrite("WAVEfmt ", 1, 8, wav_);
    put32(16);
    put16(1);
    put16(2);
    put32(44100);
    put32(44100 * 4);
    put16(4);
    put16(16);
    fwrite("data", 1, 4, wav_);
    put32(data);
    fclose(wav_);
    wav_ = nullptr;
}
