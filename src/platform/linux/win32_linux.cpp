// What windows.h (src/platform/mac/windows.h) declares, on Linux. Files,
// threads and clocks are POSIX as on macOS; GDI's text is FreeType, the
// achievement's sound SDL3 and the news download libcurl.
#include "windows.h"

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
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

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

}  // namespace

// ---- Clocks, threads, the process ----
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    out->QuadPart = LONGLONG(now_ns());
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = 1000000000;
    return TRUE;
}

ULONGLONG GetTickCount64() { return now_ns() / 1000000; }

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
    return reinterpret_cast<void*>(uintptr_t(start.fn(start.arg)));
}
}  // namespace

HANDLE CreateThread(void*, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD*) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) pthread_attr_setstacksize(&attr, stack);
    pthread_t t{};
    int r = pthread_create(&t, &attr, thread_main, new ThreadStart{fn, arg});
    pthread_attr_destroy(&attr);
    return r == 0 ? reinterpret_cast<HANDLE>(t) : nullptr;
}

DWORD WaitForSingleObject(HANDLE thread, DWORD) {
    pthread_join(reinterpret_cast<pthread_t>(thread), nullptr);
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
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return "";
    path[n] = 0;
    return path;
}

// The game reads and writes relative to the current folder: the project's
// folder, where build_linux.sh leaves the program next to data/, res/ and
// third_party/. Started from anywhere else, it goes to its own folder.
void mac_enter_game_folder() {
    if (GetFileAttributesA("res/menu") != INVALID_FILE_ATTRIBUTES) return;
    std::string exe = mac_exe_path();
    if (!exe.empty() && chdir(exe.substr(0, exe.find_last_of('/')).c_str()) != 0)
        fprintf(stderr, "[files] could not enter %s\n", exe.c_str());
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
    std::replace(p.begin(), p.end(), '\\', '/');
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

// The main screen without the panels, in the units of the window's size
BOOL SystemParametersInfoA(UINT action, UINT, void* out, UINT) {
    SDL_Rect r{};
    if (action != SPI_GETWORKAREA || !SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) return FALSE;
    *static_cast<RECT*>(out) = {r.x, r.y, r.x + r.w, r.y + r.h};
    return TRUE;
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
    fprintf(stderr, "[%s] %s\n", title, text);
    if (!getenv("SWT_BACKGROUND")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, reinterpret_cast<SDL_Window*>(hwnd));
    return 1;
}

INT_PTR ShellExecuteW(HWND, const wchar_t*, const wchar_t* file, const wchar_t*, const wchar_t*, int) {
    std::string url = narrow(file);
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return 0;
    return SDL_OpenURL(url.c_str()) ? 33 : 0;   // above 32: it worked
}

// ---- Fonts: FreeType in place of GDI ----
namespace {

FT_Library g_ft = nullptr;

struct LoadedFont {
    FT_Face face;
    std::string family;   // letters and digits only, lower case
};
std::vector<LoadedFont> g_loaded_fonts;

std::string font_key(const std::string& name) {
    std::string out;
    for (char c : name)
        if (std::isalnum(uint8_t(c))) out += char(std::tolower(uint8_t(c)));
    return out;
}

// One face per size: FreeType keeps the size in the face
struct LinuxFont {
    FT_Face face = nullptr;
    LONG px = 0, ascent = 0, descent = 0;
};
struct LinuxBitmap {
    std::vector<uint32_t> px;
    int w, h;
};
struct LinuxDC {
    LinuxFont* font = nullptr;
    LinuxBitmap* bitmap = nullptr;
    COLORREF color = 0;
};
std::map<void*, int> g_gdi_kind;   // HGDIOBJ is either: 1 font, 2 bitmap

const FT_Int32 LOAD_FLAGS = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;

// Calls f(glyph slot, pen x) for each character, without kerning (as GDI's TextOut)
template <class F>
LONG for_each_glyph(LinuxFont* font, const wchar_t* text, int n, F&& f) {
    LONG pen = 0;
    for (int i = 0; i < n; i++) {
        if (FT_Load_Char(font->face, FT_ULong(text[i]), LOAD_FLAGS) != 0) continue;
        f(font->face->glyph, pen);
        pen += LONG((font->face->glyph->advance.x + 32) >> 6);
    }
    return pen;
}

}  // namespace

HANDLE AddFontMemResourceEx(void* data, DWORD size, void*, DWORD* count) {
    if (!g_ft && FT_Init_FreeType(&g_ft) != 0) return nullptr;
    FT_Face face = nullptr;   // the caller keeps `data` until RemoveFontMemResourceEx, as with GDI
    if (FT_New_Memory_Face(g_ft, static_cast<const FT_Byte*>(data), FT_Long(size), 0, &face) != 0) return nullptr;
    g_loaded_fonts.push_back({face, font_key(face->family_name ? face->family_name : "")});
    if (count) *count = 1;
    return face;
}

// Windows calls a face by more of its name ("Saira ExtraCondensed ExCond" for
// the family "Saira ExtraCondensed"), so the longest family it starts with
// wins; none of them: the first one loaded
HFONT CreateFontW(int height, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const wchar_t* face) {
    std::string want = font_key(narrow(face));
    const LoadedFont* best = nullptr;
    for (const LoadedFont& f : g_loaded_fonts)
        if (want.rfind(f.family, 0) == 0 && (!best || f.family.size() > best->family.size())) best = &f;
    if (!best && !g_loaded_fonts.empty()) best = &g_loaded_fonts.front();
    if (!best) return nullptr;
    auto* m = new LinuxFont;
    m->px = std::max(1, std::abs(height));   // negative: the character height, as the game asks
    FT_Face src = best->face;
    // A face of its own for this size, on the same bytes
    if (FT_New_Memory_Face(g_ft, src->stream->base, FT_Long(src->stream->size), 0, &m->face) != 0) {
        delete m;
        return nullptr;
    }
    FT_Set_Pixel_Sizes(m->face, 0, FT_UInt(m->px));
    // GDI's ascent and descent: the OS/2 table's Windows ones, else hhea's
    const TT_OS2* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(m->face, FT_SFNT_OS2));
    double em = double(m->face->units_per_EM ? m->face->units_per_EM : 1000);
    if (os2 && os2->version != 0xFFFF && (os2->usWinAscent || os2->usWinDescent)) {
        m->ascent = LONG(std::lround(os2->usWinAscent * m->px / em));
        m->descent = LONG(std::lround(os2->usWinDescent * m->px / em));
    } else {
        m->ascent = LONG(std::lround(m->face->ascender * m->px / em));
        m->descent = LONG(std::lround(-m->face->descender * m->px / em));
    }
    g_gdi_kind[m] = 1;
    return m;
}

HDC CreateCompatibleDC(HDC) { return new LinuxDC; }

BOOL DeleteDC(HDC dc) {
    delete static_cast<LinuxDC*>(dc);
    return TRUE;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) {
    auto* d = static_cast<LinuxDC*>(dc);
    if (g_gdi_kind[object] == 1) {
        HGDIOBJ old = d->font;
        d->font = static_cast<LinuxFont*>(object);
        return old;
    }
    HGDIOBJ old = d->bitmap;
    d->bitmap = static_cast<LinuxBitmap*>(object);
    return old;
}

BOOL DeleteObject(HGDIOBJ object) {
    if (!object) return FALSE;
    int kind = g_gdi_kind[object];
    g_gdi_kind.erase(object);
    if (kind == 1) {
        FT_Done_Face(static_cast<LinuxFont*>(object)->face);
        delete static_cast<LinuxFont*>(object);
    } else if (kind == 2) {
        delete static_cast<LinuxBitmap*>(object);
    }
    return TRUE;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* info, UINT, void** bits, HANDLE, DWORD) {
    int w = info->bmiHeader.biWidth, h = std::abs(info->bmiHeader.biHeight);
    auto* b = new LinuxBitmap{std::vector<uint32_t>(size_t(w) * h), w, h};
    g_gdi_kind[b] = 2;
    *bits = b->px.data();
    return b;
}

BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* text, int n, SIZE* out) {
    auto* d = static_cast<LinuxDC*>(dc);
    if (!d->font) return FALSE;
    LONG w = for_each_glyph(d->font, text, n, [](FT_GlyphSlot, LONG) {});
    *out = {w, d->font->ascent + d->font->descent};
    return TRUE;
}

BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out) {
    auto* d = static_cast<LinuxDC*>(dc);
    *out = {};
    if (!d->font) return FALSE;
    out->tmAscent = d->font->ascent;
    out->tmDescent = d->font->descent;
    out->tmHeight = out->tmAscent + out->tmDescent;
    out->tmInternalLeading = std::max<LONG>(0, out->tmHeight - d->font->px);
    return TRUE;
}

COLORREF SetTextColor(HDC dc, COLORREF color) {
    auto* d = static_cast<LinuxDC*>(dc);
    std::swap(d->color, color);
    return color;
}

// Antialiased, over what is there: BGRA, premultiplied, the top of the letters `y`
BOOL TextOutW(HDC dc, int x, int y, const wchar_t* text, int n) {
    auto* d = static_cast<LinuxDC*>(dc);
    if (!d->bitmap || !d->font) return FALSE;
    LinuxBitmap& b = *d->bitmap;
    const uint32_t r = d->color & 255, g = (d->color >> 8) & 255, bl = (d->color >> 16) & 255;
    int baseline = y + d->font->ascent;
    for_each_glyph(d->font, text, n, [&](FT_GlyphSlot slot, LONG pen) {
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return;
        const FT_Bitmap& bm = slot->bitmap;
        int left = x + int(pen) + slot->bitmap_left, top = baseline - slot->bitmap_top;
        for (unsigned row = 0; row < bm.rows; row++) {
            int py = top + int(row);
            if (py < 0 || py >= b.h) continue;
            const uint8_t* src = bm.buffer + ptrdiff_t(row) * bm.pitch;
            for (unsigned col = 0; col < bm.width; col++) {
                int px = left + int(col);
                uint32_t a = src[col];
                if (px < 0 || px >= b.w || !a) continue;
                uint32_t& p = b.px[size_t(py) * b.w + px];
                auto over = [a](uint32_t c, uint32_t under) { return (c * a + under * (255 - a) + 127) / 255; };
                p = over(bl, p & 255) | over(g, (p >> 8) & 255) << 8 | over(r, (p >> 16) & 255) << 16 | over(255, p >> 24) << 24;
            }
        }
    });
    return TRUE;
}

// ---- A sound from memory (a whole RIFF WAV file), on its own SDL stream ----
BOOL PlaySoundW(const wchar_t* sound, void*, DWORD) {
    static SDL_AudioStream* stream = nullptr;
    if (stream) SDL_DestroyAudioStream(stream);
    stream = nullptr;
    if (!sound) return TRUE;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return FALSE;
    const auto* wav = reinterpret_cast<const uint8_t*>(sound);
    uint32_t riff_size;
    memcpy(&riff_size, wav + 4, 4);   // 8 bytes of header say how long
    SDL_AudioSpec spec{};
    Uint8* pcm = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav, size_t(riff_size) + 8), true, &spec, &pcm, &len)) return FALSE;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    bool ok = stream && SDL_PutAudioStreamData(stream, pcm, int(len)) && SDL_FlushAudioStream(stream) &&
              SDL_ResumeAudioStreamDevice(stream);
    SDL_free(pcm);
    return ok;
}

// ---- Web ----
namespace {
size_t keep(char* data, size_t size, size_t n, void* user) {
    auto* body = static_cast<std::string*>(user);
    if (body->size() + size * n > 256 * 1024) return 0;   // too long: fails the transfer
    body->append(data, size * n);
    return size * n;
}
}  // namespace

bool mac_https_get(const wchar_t* host, const wchar_t* path, std::string& body) {
    body.clear();
    static bool started = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    CURL* c = started ? curl_easy_init() : nullptr;
    if (!c) return false;
    std::string url = "https://" + narrow(host) + narrow(path);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "StarWarsTrilogyNative");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, keep);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    long status = 0;
    bool ok = curl_easy_perform(c) == CURLE_OK && curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
              status == 200;
    curl_easy_cleanup(c);
    if (!ok) body.clear();
    return ok;
}
