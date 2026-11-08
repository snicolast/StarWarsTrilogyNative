// macOS: the part of the Win32 API this game calls, so its code stays as it is
// on Windows. Files, threads and clocks are POSIX, the window is SDL3
// (vk_window_mac.cpp) and GDI's text is Core Text (win32_mac.mm).
//
// There is one window and no desktop: screen coordinates are the window's own
// pixels, so ScreenToClient and ClientToScreen change nothing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

// ---- Types ----
typedef uint8_t BYTE;
typedef uint16_t WORD, USHORT;
typedef uint32_t DWORD, UINT, ULONG, COLORREF;
typedef int32_t LONG, HRESULT;   // LONG is 32 bits on Windows, `long` here is 64
typedef bool BOOL;               // as Objective-C's on arm64, for the .mm files that see both
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef intptr_t INT_PTR, LONG_PTR, LPARAM, LRESULT;
typedef uintptr_t UINT_PTR, WPARAM;
typedef void* HANDLE;
typedef void* LPVOID;
typedef void* HGDIOBJ;
typedef void* HFONT;
typedef void* HBITMAP;
typedef void* HDC;
typedef struct HWND__* HWND;
typedef struct HRAWINPUT__* HRAWINPUT;
typedef const wchar_t* LPCWSTR;

#define WINAPI
#define TRUE 1
#define FALSE 0
#define MAX_PATH 1024
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(intptr_t(-1)))
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define SUCCEEDED(hr) (HRESULT(hr) >= 0)
#define FAILED(hr) (HRESULT(hr) < 0)
#define S_OK HRESULT(0)
#define E_FAIL HRESULT(0x80004005)
#define CP_UTF8 65001u

union LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
};
struct POINT { LONG x, y; };
struct SIZE { LONG cx, cy; };
struct RECT { LONG left, top, right, bottom; };
struct WIN32_FIND_DATAA { char cFileName[MAX_PATH]; };

// ---- Window messages and keys (the same numbers as Windows) ----
#define WM_KEYDOWN 0x0100u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_RBUTTONDOWN 0x0204u
#define WM_MOUSEWHEEL 0x020Au
#define LOWORD(v) (WORD(uintptr_t(v) & 0xFFFF))
#define HIWORD(v) (WORD((uintptr_t(v) >> 16) & 0xFFFF))
#define MAKELPARAM(l, h) (LPARAM(uint32_t(WORD(l)) | (uint32_t(WORD(h)) << 16)))
#define GET_X_LPARAM(lp) (int(short(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (int(short(HIWORD(lp))))
#define GET_WHEEL_DELTA_WPARAM(wp) (short(HIWORD(wp)))

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F9 0x78
#define VK_F11 0x7A
#define VK_F12 0x7B

#define MB_OK 0u
#define MB_ICONERROR 0x10u
#define SW_SHOWNORMAL 1

// ---- Clocks, threads, the process ----
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
ULONGLONG GetTickCount64();
void Sleep(DWORD ms);
inline UINT timeBeginPeriod(UINT) { return 0; }
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
HANDLE CreateThread(void* attr, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD* id);
DWORD WaitForSingleObject(HANDLE thread, DWORD ms);
[[noreturn]] void ExitProcess(UINT code);

// ---- Files ----
BOOL CreateDirectoryA(const char* path, void* attr);
#define CreateDirectory CreateDirectoryA
DWORD GetFileAttributesA(const char* path);
DWORD GetFileAttributesW(const wchar_t* path);
DWORD GetFullPathNameA(const char* path, DWORD size, char* out, char** file);
DWORD GetFullPathNameW(const wchar_t* path, DWORD size, wchar_t* out, wchar_t** file);
DWORD GetModuleFileNameA(void* module, char* out, DWORD size);
DWORD GetModuleFileNameW(void* module, wchar_t* out, DWORD size);
HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out);
BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA* out);
BOOL FindClose(HANDLE find);
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int n, wchar_t* out, int size);

// ---- The window ----
HWND GetForegroundWindow();
BOOL GetClientRect(HWND hwnd, RECT* out);
#define SPI_GETWORKAREA 48
BOOL SystemParametersInfoA(UINT action, UINT param, void* out, UINT flags);
BOOL GetCursorPos(POINT* out);
inline BOOL ScreenToClient(HWND, POINT*) { return TRUE; }
inline BOOL ClientToScreen(HWND, POINT*) { return TRUE; }
short GetAsyncKeyState(int vk);
inline short GetKeyState(int vk) { return GetAsyncKeyState(vk); }
BOOL SetWindowTextA(HWND hwnd, const char* text);
int MessageBoxA(HWND hwnd, const char* text, const char* title, UINT type);
inline BOOL PtInRect(const RECT* r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
// Web addresses go to the default browser; anything else (.exe, .bat) fails with 0
INT_PTR ShellExecuteW(HWND hwnd, const wchar_t* verb, const wchar_t* file, const wchar_t* args, const wchar_t* dir, int show);

// The compositor's vblank: not known here, DisplayClock (app.cpp) keeps its own
struct DWM_TIMING_INFO {
    UINT cbSize;
    ULONGLONG qpcRefreshPeriod, qpcVBlank;
};
inline HRESULT DwmGetCompositionTimingInfo(HWND, DWM_TIMING_INFO*) { return E_FAIL; }

// ---- GDI: text into a 32-bit top-down bitmap ----
#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1u
#define OUT_TT_ONLY_PRECIS 7u
#define CLIP_DEFAULT_PRECIS 0u
#define ANTIALIASED_QUALITY 4u
#define DEFAULT_PITCH 0u
#define DIB_RGB_COLORS 0u
#define TRANSPARENT 1
#define RGB(r, g, b) (COLORREF(BYTE(r)) | (COLORREF(BYTE(g)) << 8) | (COLORREF(BYTE(b)) << 16))

struct TEXTMETRICW { LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth; };
#pragma pack(push, 2)
struct BITMAPFILEHEADER { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; };
#pragma pack(pop)
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; };

HANDLE AddFontMemResourceEx(void* data, DWORD size, void* reserved, DWORD* count);
inline BOOL RemoveFontMemResourceEx(HANDLE) { return TRUE; }
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality, DWORD pitch,
                  const wchar_t* face);
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object);
BOOL DeleteObject(HGDIOBJ object);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset);
BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* text, int n, SIZE* out);
BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out);
inline int SetBkMode(HDC, int) { return TRANSPARENT; }
COLORREF SetTextColor(HDC dc, COLORREF color);
BOOL TextOutW(HDC dc, int x, int y, const wchar_t* text, int n);
inline BOOL GdiFlush() { return TRUE; }

// ---- A sound from memory (the achievement's), played apart from the game's ----
#define SND_ASYNC 0x1u
#define SND_NODEFAULT 0x2u
#define SND_MEMORY 0x4u
BOOL PlaySoundW(const wchar_t* sound, void* module, DWORD flags);
