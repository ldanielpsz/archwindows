// Freestanding helpers. The program links without the C runtime, so nothing
// here may call into it. Everything is header-only and inlines away.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef CLR_NONE
#define CLR_NONE 0xFFFFFFFFL  // "no colour" (commctrl.h), used by the renderer's outline/fill switches
#endif

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

// sRGB channel -> linear light, x10000 (WCAG relative luminance without pow()).
inline constexpr unsigned short kLin[256] = {
    0,3,6,9,12,15,18,21,24,27,30,33,37,40,44,48,
    52,56,60,65,70,75,80,86,91,97,103,110,116,123,130,137,
    144,152,160,168,176,185,194,203,212,222,232,242,252,262,273,284,
    296,307,319,331,343,356,369,382,395,409,423,437,452,467,482,497,
    513,529,545,561,578,595,612,630,648,666,685,704,723,742,762,782,
    802,823,844,865,887,908,931,953,976,999,1022,1046,1070,1095,1119,1144,
    1170,1195,1221,1248,1274,1301,1329,1356,1384,1413,1441,1470,1500,1529,1559,1590,
    1620,1651,1683,1714,1746,1779,1812,1845,1878,1912,1946,1981,2016,2051,2086,2122,
    2159,2195,2232,2270,2307,2346,2384,2423,2462,2502,2542,2582,2623,2664,2705,2747,
    2789,2831,2874,2918,2961,3005,3050,3095,3140,3185,3231,3278,3325,3372,3419,3467,
    3515,3564,3613,3663,3712,3763,3813,3864,3916,3968,4020,4072,4125,4179,4233,4287,
    4342,4397,4452,4508,4564,4621,4678,4735,4793,4851,4910,4969,5029,5089,5149,5210,
    5271,5333,5395,5457,5520,5583,5647,5711,5776,5841,5906,5972,6038,6105,6172,6240,
    6308,6376,6445,6514,6584,6654,6724,6795,6867,6939,7011,7084,7157,7231,7305,7379,
    7454,7529,7605,7682,7758,7835,7913,7991,8070,8148,8228,8308,8388,8469,8550,8632,
    8714,8796,8879,8963,9047,9131,9216,9301,9387,9473,9560,9647,9734,9823,9911,10000};
inline int luminance(COLORREF c) {  // 0..10000
    return (2126 * kLin[GetRValue(c)] + 7152 * kLin[GetGValue(c)] + 722 * kLin[GetBValue(c)]) / 10000;
}
// WCAG contrast ratio x100 (e.g. 450 = 4.5:1).
inline int contrast(COLORREF a, COLORREF b) {
    int la = luminance(a), lb = luminance(b);
    if (la < lb) { const int t = la; la = lb; lb = t; }
    return (la + 500) * 100 / (lb + 500);
}
// c, moved toward `toward` in 5% steps only as far as needed to reach `minX100` against bg.
// Colours that already pass come back unchanged, so a theme's own hues survive.
inline COLORREF ensure(COLORREF c, COLORREF bg, int minX100, COLORREF toward) {
    COLORREF r = c;
    for (int pct = 5; contrast(r, bg) < minX100 && pct <= 100; pct += 5) r = mix(c, toward, pct);
    return r;
}

// DPI scaling of a 96-dpi design unit.
inline int scale(int v, int dpi) { return (v * dpi + 48) / 96; }

} // namespace sys
