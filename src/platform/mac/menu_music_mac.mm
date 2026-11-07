// macOS: MenuMusic (menu_music.h) with AVAudioPlayer in place of MCI.
#include "menu_music.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>

namespace {
AVAudioPlayer* g_player = nil;
}

bool MenuMusic::open() {
    for (const char* path : {"third_party/menu/music.mp3", "third_party/menu/music.wav", "data/menu_music.wav"}) {
        if (access(path, R_OK) != 0) continue;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        g_player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:nil];
        if (g_player) {
            g_player.numberOfLoops = -1;
            printf("[menu] music %s\n", path);
            open_ = true;
            return true;
        }
        fprintf(stderr, "[menu] could not open the music %s\n", path);
    }
    return false;
}

void MenuMusic::close() {
    [g_player stop];
    g_player = nil;
    open_ = playing_ = false;
}

void MenuMusic::update(bool want, int volume, double dt) {
    if (!open_) return;
    // A second to come in, half a second to go when the game starts
    level_ = want ? std::min(1.0f, level_ + float(dt) / 1.0f) : std::max(0.0f, level_ - float(dt) / 0.5f);
    if (want && !playing_) {
        sent_ = -1;
        playing_ = [g_player play];
    }
    if (!playing_) return;
    if (level_ <= 0) {   // faded out: it waits where it was
        [g_player pause];
        playing_ = false;
        return;
    }
    // The same curve as on Windows: the game's volume 5 plays it at 0.71
    float g = std::sqrt(std::clamp(float(volume) / 10.0f, 0.0f, 1.0f));
    int v = int(std::clamp(level_ * g, 0.0f, 1.0f) * 1000.0f + 0.5f);
    if (v != sent_) {
        g_player.volume = float(v) / 1000.0f;
        sent_ = v;
    }
}
