// macOS: the compiler intrinsics and CRT calls of MSVC the code uses. Included
// in every C++ file on the command line (CMakeLists.txt).
#pragma once
#ifdef __cplusplus
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <ctime>

static inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
static inline unsigned char _BitScanReverse(unsigned long* i, uint32_t v) {
    if (!v) return 0;
    *i = 31 - __builtin_clz(v);
    return 1;
}
static inline int __popcnt(uint32_t v) { return __builtin_popcount(v); }
static inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 1; }
static inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
extern "C" char** environ;
#define _environ environ
#endif
