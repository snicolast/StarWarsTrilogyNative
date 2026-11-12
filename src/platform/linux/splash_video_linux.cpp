// Linux: no video player without another library, so SplashVideo (splash_video.h)
// never opens and the menu shows the splash picture in its place, as on a
// Windows that cannot play the file.
#include "splash_video.h"

struct SplashVideo::Impl {};

bool SplashVideo::open(const std::wstring&) { return false; }
void SplashVideo::close() {}
void SplashVideo::play() {}
void SplashVideo::set_volume(float) {}
bool SplashVideo::failed() const { return false; }
bool SplashVideo::ended() const { return true; }
double SplashVideo::position() const { return 0; }
bool SplashVideo::size(int&, int&) const { return false; }
bool SplashVideo::frame(int, int, std::vector<uint32_t>&, bool) { return false; }
