// Freestanding helpers. The program links without the C runtime, so nothing
// here may call into it. Everything is header-only and inlines away.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int  u32;

namespace sys {


inline int wlen(const wchar_t* s) { int n = 0; while (s[n]) ++n; return n; }

inline bool weq(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// Copies s to d, returns a pointer at d's new terminator (so calls chain).
inline wchar_t* wcopy(wchar_t* d, const wchar_t* s) {
    while ((*d = *s++)) ++d;
    return d;
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

// Linear blend, pctB percent of b.
inline COLORREF mix(COLORREF a, COLORREF b, int pctB) {
    int r = GetRValue(a) + (GetRValue(b) - (int)GetRValue(a)) * pctB / 100;
    int g = GetGValue(a) + (GetGValue(b) - (int)GetGValue(a)) * pctB / 100;
    int bl = GetBValue(a) + (GetBValue(b) - (int)GetBValue(a)) * pctB / 100;
    return RGB(r, g, bl);
}

// DPI scaling of a 96-dpi design unit.
inline int scale(int v, int dpi) { return (v * dpi + 48) / 96; }

} // namespace sys
