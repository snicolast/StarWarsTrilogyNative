// macOS: AudioOut (audio_out.h) through SDL3, which follows the default output
// by itself (headphones plugged in, etc.). The stream's rate is the device's,
// so the resampling stays ours, in AudioStream.
#include "audio_out.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

namespace {

struct Playing {
    AudioStream* stream;
    AudioStream::Speakers mode;
    int channels;
    std::vector<float> buffer;
};

void SDLCALL fill(void* user, SDL_AudioStream* out, int additional, int) {
    auto* p = static_cast<Playing*>(user);
    size_t frames = size_t(additional) / (sizeof(float) * size_t(p->channels));
    if (!frames) return;
    p->buffer.resize(frames * size_t(p->channels));
    p->stream->pull(p->buffer.data(), frames, p->mode);
    SDL_PutAudioStreamData(out, p->buffer.data(), int(p->buffer.size() * sizeof(float)));
}

}  // namespace

bool AudioOut::start() {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio] SDL could not start the sound: %s\n", SDL_GetError());
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void AudioOut::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioOut::run() {
    bool warned = false;
    while (running_) {
        bool opened = play_default_device();
        if (!opened && !warned) {
            fprintf(stderr, "[audio] could not open the audio output; retrying in the background\n");
            warned = true;
        }
        for (int i = 0; i < (opened ? 1 : 10) && running_; i++) usleep(50000);
    }
}

// Plays until the output stops or the speakers want another channel count
bool AudioOut::play_default_device() {
    const auto mode = AudioStream::Speakers(speakers_.load());
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr)) return false;
    Playing playing{&stream_, mode, AudioStream::channels(mode), {}};
    const int device_channels = spec.channels;
    spec.format = SDL_AUDIO_F32;
    spec.channels = playing.channels;
    stream_.set_output_rate(spec.freq);
    SDL_AudioStream* out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, &playing);
    if (!out) return false;
    SDL_ResumeAudioStreamDevice(out);
    printf("[audio] output at %d Hz, %d channels (the device has %d)\n", spec.freq, spec.channels, device_channels);
    while (running_ && AudioStream::channels(AudioStream::Speakers(speakers_.load())) == playing.channels) {
        // Same channel count, another layout (stereo and headphones)
        SDL_LockAudioStream(out);
        playing.mode = AudioStream::Speakers(speakers_.load());
        SDL_UnlockAudioStream(out);
        usleep(50000);
    }
    SDL_DestroyAudioStream(out);
    return true;
}
