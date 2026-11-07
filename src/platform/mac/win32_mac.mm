// What windows.h declares on macOS.
#include "windows.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include <SDL3/SDL.h>

#include <dirent.h>
#include <fnmatch.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "platform_mac.h"

namespace {

std::string narrow(const wchar_t* w) {
    std::string out;
    for (; *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c == '\\') c = '/';   // the paths the Windows code builds
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 63));
        else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 63)), out += char(0x80 | (c & 63));
        else out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 63)), out += char(0x80 | ((c >> 6) & 63)), out += char(0x80 | (c & 63));
    }
    return out;
}

DWORD copy_wide(const char* s, wchar_t* out, DWORD size) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, int(strlen(s)), nullptr, 0);
    if (DWORD(n) >= size) return 0;
    MultiByteToWideChar(CP_UTF8, 0, s, int(strlen(s)), out, n);
    out[n] = 0;
    return DWORD(n);
}

CFStringRef cf_string(const wchar_t* s, int n) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s), CFIndex(n) * CFIndex(sizeof(wchar_t)),
                                   kCFStringEncodingUTF32LE, false);
}

}  // namespace

// ---- Clocks, threads, the process ----
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    out->QuadPart = LONGLONG(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = 1000000000;
    return TRUE;
}

ULONGLONG GetTickCount64() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

void Sleep(DWORD ms) {
    if (ms == INFINITE) for (;;) pause();
    usleep(useconds_t(ms) * 1000);
}

namespace {
struct ThreadStart {
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
};

void* thread_main(void* p) {
    ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // the performance cores: it runs in real time
    return reinterpret_cast<void*>(uintptr_t(start.fn(start.arg)));
}
}  // namespace

HANDLE CreateThread(void*, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD*) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) pthread_attr_setstacksize(&attr, stack);
    pthread_t t = nullptr;
    int r = pthread_create(&t, &attr, thread_main, new ThreadStart{fn, arg});
    pthread_attr_destroy(&attr);
    return r == 0 ? HANDLE(t) : nullptr;
}

DWORD WaitForSingleObject(HANDLE thread, DWORD) {
    pthread_join(pthread_t(thread), nullptr);
    return 0;
}

void ExitProcess(UINT code) {
    fflush(nullptr);
    _exit(int(code));
}

// ---- Files ----
BOOL CreateDirectoryA(const char* path, void*) { return mkdir(path, 0755) == 0; }

DWORD GetFileAttributesA(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

DWORD GetFileAttributesW(const wchar_t* path) { return GetFileAttributesA(narrow(path).c_str()); }

DWORD GetFullPathNameA(const char* path, DWORD size, char* out, char** file) {
    std::string s = path;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s[0] != '/') {
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        s = std::string(cwd) + "/" + s;
    }
    if (s.size() >= size) return DWORD(s.size() + 1);
    memcpy(out, s.c_str(), s.size() + 1);
    if (file) *file = out + s.find_last_of('/') + 1;
    return DWORD(s.size());
}

DWORD GetFullPathNameW(const wchar_t* path, DWORD size, wchar_t* out, wchar_t**) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(narrow(path).c_str(), MAX_PATH, full, nullptr);
    return n && n < MAX_PATH ? copy_wide(full, out, size) : 0;
}

DWORD GetModuleFileNameA(void*, char* out, DWORD size) {
    std::string s = mac_exe_path();
    if (s.size() >= size) return 0;
    memcpy(out, s.c_str(), s.size() + 1);
    return DWORD(s.size());
}

DWORD GetModuleFileNameW(void*, wchar_t* out, DWORD size) { return copy_wide(mac_exe_path().c_str(), out, size); }

std::string mac_exe_path() {
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return "";
    return realpath(path, real) ? real : path;
}

// The game reads and writes relative to the current folder. The application
// carries what it reads and writes in Application Support, where links lead to
// the rest: it can then live anywhere, and macOS has no reason to ask for the
// Desktop or Documents folder. The bare program plays from its own folder.
void mac_enter_game_folder() {
    std::string exe = mac_exe_path();
    size_t app = exe.rfind(".app/Contents/MacOS/");
    if (app == std::string::npos) {
        if (GetFileAttributesA("res/menu") != INVALID_FILE_ATTRIBUTES) return;
        chdir(exe.substr(0, exe.find_last_of('/')).c_str());
        return;
    }
    std::string inside = exe.substr(0, app) + ".app/Contents/Resources/";
    const char* home = getenv("HOME");
    std::string folder = std::string(home ? home : "") + "/Library/Application Support/Star Wars Trilogy Native";
    mkdir(folder.c_str(), 0755);
    if (chdir(folder.c_str()) != 0) return;
    auto link_to = [](const std::string& target, const std::string& name) {   // again each time: the application may have moved
        unlink(name.c_str());
        symlink(target.c_str(), name.c_str());
    };
    for (const char* name : {"res", "third_party", "news.txt"}) link_to(inside + name, name);
    // data/ is a real folder: the menu music is decoded into it at the first launch
    mkdir("data", 0755);
    if (DIR* d = opendir((inside + "data").c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') link_to(inside + "data/" + e->d_name, std::string("data/") + e->d_name);
        closedir(d);
    }
}

namespace {
struct FindState {
    std::vector<std::string> names;
    size_t next = 0;
};

BOOL find_next(FindState* f, WIN32_FIND_DATAA* out) {
    if (f->next >= f->names.size()) return FALSE;
    snprintf(out->cFileName, sizeof(out->cFileName), "%s", f->names[f->next++].c_str());
    return TRUE;
}
}  // namespace

HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out) {
    std::string p = pattern;
    size_t slash = p.find_last_of('/');
    std::string dir = slash == std::string::npos ? "." : p.substr(0, slash), mask = slash == std::string::npos ? p : p.substr(slash + 1);
    DIR* d = opendir(dir.c_str());
    if (!d) return INVALID_HANDLE_VALUE;
    auto* f = new FindState;
    while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && fnmatch(mask.c_str(), e->d_name, FNM_CASEFOLD) == 0)
            f->names.push_back(e->d_name);
    closedir(d);
    std::sort(f->names.begin(), f->names.end());
    if (!find_next(f, out)) {
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA* out) { return find_next(static_cast<FindState*>(find), out); }

BOOL FindClose(HANDLE find) {
    delete static_cast<FindState*>(find);
    return TRUE;
}

int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* out, int size) {
    if (n < 0) n = int(strlen(s)) + 1;
    int written = 0;
    for (int i = 0; i < n;) {
        uint8_t b = uint8_t(s[i]);
        int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
        uint32_t c = extra == 3 ? b & 7u : extra == 2 ? b & 15u : extra == 1 ? b & 31u : b;
        for (int k = 1; k <= extra && i + k < n; k++) c = (c << 6) | (uint8_t(s[i + k]) & 63u);
        i += extra + 1;
        if (out && written < size) out[written] = wchar_t(c);
        written++;
    }
    return written;
}

// ---- The window ----
HWND GetForegroundWindow() {
    SDL_Window* w = mac_window();
    return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) ? reinterpret_cast<HWND>(w) : nullptr;
}

BOOL GetClientRect(HWND hwnd, RECT* out) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(reinterpret_cast<SDL_Window*>(hwnd), &w, &h);
    *out = {0, 0, w, h};
    return TRUE;
}

BOOL GetCursorPos(POINT* out) {
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    float k = mac_window() ? SDL_GetWindowPixelDensity(mac_window()) : 1.0f;
    *out = {LONG(x * k), LONG(y * k)};
    return TRUE;
}

short GetAsyncKeyState(int vk) {
    bool down = false;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    switch (vk) {
    case VK_LBUTTON: down = buttons & SDL_BUTTON_LMASK; break;
    case VK_RBUTTON: down = buttons & SDL_BUTTON_RMASK; break;
    case VK_MBUTTON: down = buttons & SDL_BUTTON_MMASK; break;
    case VK_SHIFT: down = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]; break;
    case VK_CONTROL: down = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]; break;
    case VK_MENU: down = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT]; break;
    default: {
        SDL_Scancode sc = mac_scancode(vk);
        down = sc != SDL_SCANCODE_UNKNOWN && keys[sc];
    }
    }
    return down ? short(0x8000) : short(0);
}

BOOL SetWindowTextA(HWND hwnd, const char* text) { return SDL_SetWindowTitle(reinterpret_cast<SDL_Window*>(hwnd), text); }

int MessageBoxA(HWND hwnd, const char* text, const char* title, UINT) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, reinterpret_cast<SDL_Window*>(hwnd));
    return 1;
}

INT_PTR ShellExecuteW(HWND, const wchar_t*, const wchar_t* file, const wchar_t*, const wchar_t*, int) {
    std::string url = narrow(file);
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return 0;
    return SDL_OpenURL(url.c_str()) ? 33 : 0;   // above 32: it worked
}

// ---- Fonts: Core Text in place of GDI ----
namespace {

struct LoadedFont {
    CGFontRef font;
    std::string family;   // letters and digits only, lower case
};
std::vector<LoadedFont> g_loaded_fonts;

std::string font_key(const std::string& name) {
    std::string out;
    for (char c : name)
        if (std::isalnum(uint8_t(c))) out += char(std::tolower(uint8_t(c)));
    return out;
}

struct MacFont {
    CTFontRef font;
    LONG px;
};
struct MacBitmap {
    std::vector<uint32_t> px;
    int w, h;
};
struct MacDC {
    MacFont* font = nullptr;
    MacBitmap* bitmap = nullptr;
    COLORREF color = 0;
};
// HGDIOBJ is either: told apart by where it came from
std::map<void*, int> g_gdi_kind;   // 1 font, 2 bitmap

CTLineRef make_line(MacFont* font, const wchar_t* text, int n) {
    CFStringRef s = cf_string(text, n);
    int none = 0;
    CFNumberRef no_ligatures = CFNumberCreate(nullptr, kCFNumberIntType, &none);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName, kCTLigatureAttributeName};
    const void* values[] = {font->font, kCFBooleanTrue, no_ligatures};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, s, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(no_ligatures);
    CFRelease(s);
    return line;
}

}  // namespace

HANDLE AddFontMemResourceEx(void* data, DWORD size, void*, DWORD* count) {
    CFDataRef bytes = CFDataCreate(nullptr, static_cast<const UInt8*>(data), CFIndex(size));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
    CGFontRef font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    CFRelease(bytes);
    if (!font) return nullptr;
    CTFontRef ct = CTFontCreateWithGraphicsFont(font, 16, nullptr, nullptr);
    CFStringRef family = CTFontCopyFamilyName(ct);
    char name[256] = {};
    CFStringGetCString(family, name, sizeof(name), kCFStringEncodingUTF8);
    CFRelease(family);
    CFRelease(ct);
    g_loaded_fonts.push_back({font, font_key(name)});
    if (count) *count = 1;
    return font;
}

// Windows calls a face by more of its name ("Saira ExtraCondensed ExCond" for
// the family "Saira ExtraCondensed"), so the longest family it starts with wins
HFONT CreateFontW(int height, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const wchar_t* face) {
    std::string want = font_key(narrow(face));
    const LoadedFont* best = nullptr;
    for (const LoadedFont& f : g_loaded_fonts)
        if (want.rfind(f.family, 0) == 0 && (!best || f.family.size() > best->family.size())) best = &f;
    LONG px = std::abs(height);
    auto* m = new MacFont{nullptr, px};
    if (best) m->font = CTFontCreateWithGraphicsFont(best->font, CGFloat(px), nullptr, nullptr);
    else m->font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, CGFloat(px), nullptr);
    g_gdi_kind[m] = 1;
    return m;
}

HDC CreateCompatibleDC(HDC) { return new MacDC; }

BOOL DeleteDC(HDC dc) {
    delete static_cast<MacDC*>(dc);
    return TRUE;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) {
    auto* d = static_cast<MacDC*>(dc);
    if (g_gdi_kind[object] == 1) {
        HGDIOBJ old = d->font;
        d->font = static_cast<MacFont*>(object);
        return old;
    }
    HGDIOBJ old = d->bitmap;
    d->bitmap = static_cast<MacBitmap*>(object);
    return old;
}

BOOL DeleteObject(HGDIOBJ object) {
    if (!object) return FALSE;
    int kind = g_gdi_kind[object];
    g_gdi_kind.erase(object);
    if (kind == 1) {
        CFRelease(static_cast<MacFont*>(object)->font);
        delete static_cast<MacFont*>(object);
    } else if (kind == 2) {
        delete static_cast<MacBitmap*>(object);
    }
    return TRUE;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* info, UINT, void** bits, HANDLE, DWORD) {
    int w = info->bmiHeader.biWidth, h = std::abs(info->bmiHeader.biHeight);
    auto* b = new MacBitmap{std::vector<uint32_t>(size_t(w) * h), w, h};
    g_gdi_kind[b] = 2;
    *bits = b->px.data();
    return b;
}

BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* text, int n, SIZE* out) {
    auto* d = static_cast<MacDC*>(dc);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    CTLineRef line = make_line(d->font, text, n);
    double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    *out = {LONG(std::ceil(w)), tm.tmHeight};
    return TRUE;
}

BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out) {
    auto* d = static_cast<MacDC*>(dc);
    *out = {};
    out->tmAscent = LONG(std::lround(CTFontGetAscent(d->font->font)));
    out->tmDescent = LONG(std::lround(CTFontGetDescent(d->font->font)));
    out->tmHeight = out->tmAscent + out->tmDescent;
    out->tmInternalLeading = std::max<LONG>(0, out->tmHeight - d->font->px);
    return TRUE;
}

COLORREF SetTextColor(HDC dc, COLORREF color) {
    auto* d = static_cast<MacDC*>(dc);
    std::swap(d->color, color);
    return color;
}

BOOL TextOutW(HDC dc, int x, int y, const wchar_t* text, int n) {
    auto* d = static_cast<MacDC*>(dc);
    if (!d->bitmap || !d->font) return FALSE;
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(d->bitmap->px.data(), size_t(d->bitmap->w), size_t(d->bitmap->h), 8,
                                           size_t(d->bitmap->w) * 4, rgb,
                                           kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);   // BGRA
    CGColorSpaceRelease(rgb);
    if (!c) return FALSE;
    CGContextSetShouldSmoothFonts(c, false);
    CGContextSetRGBFillColor(c, (d->color & 255) / 255.0, ((d->color >> 8) & 255) / 255.0, ((d->color >> 16) & 255) / 255.0, 1);
    CGContextSetTextPosition(c, x, CGFloat(d->bitmap->h - y - tm.tmAscent));   // up from the bottom, to the baseline
    CTLineRef line = make_line(d->font, text, n);
    CTLineDraw(line, c);
    CFRelease(line);
    CGContextRelease(c);
    return TRUE;
}

// ---- A sound from memory ----
BOOL PlaySoundW(const wchar_t* sound, void*, DWORD) {
    @autoreleasepool {
        static AVAudioPlayer* player = nil;
        [player stop];
        player = nil;
        if (!sound) return TRUE;
        // SND_MEMORY: the sound is a whole RIFF file: 8 bytes of header say how long
        const auto* wav = reinterpret_cast<const uint8_t*>(sound);
        uint32_t riff_size;
        memcpy(&riff_size, wav + 4, 4);
        NSData* data = [NSData dataWithBytes:wav length:NSUInteger(riff_size) + 8];
        player = [[AVAudioPlayer alloc] initWithData:data error:nil];
        return [player play];
    }
}

// ---- Web ----
bool mac_https_get(const wchar_t* host, const wchar_t* path, std::string& body) {
    body.clear();
    @autoreleasepool {
        NSString* address = [NSString stringWithFormat:@"https://%s%s", narrow(host).c_str(), narrow(path).c_str()];
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 4;
        config.timeoutIntervalForResource = 8;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block bool ok = false;
        __block std::string received;
        NSURLSessionDataTask* task = [session dataTaskWithURL:[NSURL URLWithString:address]
                                            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)response).statusCode : 0;
            if (!error && status == 200 && data.length <= 256 * 1024) {
                received.assign(static_cast<const char*>(data.bytes), data.length);
                ok = true;
            }
            dispatch_semaphore_signal(done);
        }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];
        body = received;
        return ok;
    }
}
