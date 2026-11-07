// macOS: SplashVideo (splash_video.h) with AVFoundation in place of Media
// Foundation. The sound goes to the default device by itself, the pictures come
// back as BGRA from an AVPlayerItemVideoOutput.
#include "splash_video.h"

#import <AVFoundation/AVFoundation.h>
#import <Accelerate/Accelerate.h>
#import <CoreVideo/CoreVideo.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>

struct SplashVideo::Impl {
    AVPlayer* player = nil;
    AVPlayerItem* item = nil;
    AVPlayerItemVideoOutput* output = nil;
    CVPixelBufferRef last = nullptr;   // the picture on screen, for `always`
    bool played = false;
};

bool SplashVideo::open(const std::wstring& path) {
    close();
    std::string p(path.begin(), path.end());   // the project's paths are ASCII
    if (access(p.c_str(), R_OK) != 0) return false;
    impl_ = new Impl;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
    impl_->item = [AVPlayerItem playerItemWithURL:url];
    NSDictionary* format = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
    impl_->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:format];
    [impl_->item addOutput:impl_->output];
    impl_->player = [AVPlayer playerWithPlayerItem:impl_->item];
    return true;
}

void SplashVideo::close() {
    if (!impl_) return;
    [impl_->player pause];
    if (impl_->last) CVPixelBufferRelease(impl_->last);
    delete impl_;
    impl_ = nullptr;
}

void SplashVideo::play() {
    if (!impl_) return;
    impl_->played = true;
    [impl_->player play];
}

void SplashVideo::set_volume(float v) {
    if (impl_) impl_->player.volume = std::clamp(v, 0.0f, 1.0f);
}

bool SplashVideo::failed() const {
    bool f = impl_ && impl_->item.status == AVPlayerItemStatusFailed;
    if (f) fprintf(stderr, "[menu] splash video: could not play it: %s\n", impl_->item.error.localizedDescription.UTF8String);
    return f;
}

bool SplashVideo::ended() const {
    if (!impl_ || !impl_->played || impl_->item.status != AVPlayerItemStatusReadyToPlay) return false;
    return CMTimeCompare(impl_->item.currentTime, impl_->item.duration) >= 0;
}

double SplashVideo::position() const {
    if (!impl_) return 0;
    double t = CMTimeGetSeconds(impl_->item.currentTime);
    return t == t ? t : 0;
}

bool SplashVideo::size(int& w, int& h) const {
    if (!impl_ || impl_->item.status != AVPlayerItemStatusReadyToPlay) return false;
    CGSize s = impl_->item.presentationSize;
    if (s.width <= 0 || s.height <= 0) return false;
    w = int(s.width), h = int(s.height);
    return true;
}

bool SplashVideo::frame(int w, int h, std::vector<uint32_t>& px, bool always) {
    if (!impl_ || w <= 0 || h <= 0) return false;
    Impl& m = *impl_;
    CMTime now = m.item.currentTime;
    bool fresh = [m.output hasNewPixelBufferForItemTime:now];
    if (fresh) {
        CVPixelBufferRef next = [m.output copyPixelBufferForItemTime:now itemTimeForDisplay:nullptr];
        if (next) {
            if (m.last) CVPixelBufferRelease(m.last);
            m.last = next;
        } else {
            fresh = false;
        }
    }
    if (!m.last || (!fresh && !always)) return false;
    px.resize(size_t(w) * h);
    CVPixelBufferLockBaseAddress(m.last, kCVPixelBufferLock_ReadOnly);
    vImage_Buffer src{CVPixelBufferGetBaseAddress(m.last), CVPixelBufferGetHeight(m.last), CVPixelBufferGetWidth(m.last),
                      CVPixelBufferGetBytesPerRow(m.last)};
    vImage_Buffer dst{px.data(), vImagePixelCount(h), vImagePixelCount(w), size_t(w) * 4};
    vImage_Error err = vImageScale_ARGB8888(&src, &dst, nullptr, kvImageHighQualityResampling);
    CVPixelBufferUnlockBaseAddress(m.last, kCVPixelBufferLock_ReadOnly);
    if (err != kvImageNoError) return false;
    for (uint32_t& p : px) p |= 0xFF000000u;   // opaque
    return true;
}
