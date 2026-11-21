// Theme policy. A ThemePolicy exposes:
//   static int count();
//   static const wchar_t* name(int);
//   static int bgCount(int);
//   static Palette palette(int);
//   static int find(const wchar_t*);        // -1 when unknown
// EmbeddedThemes serves the imported palettes straight from .rdata:
// no parsing, no allocation, ~2 KB for 23 themes.
#pragma once
#include "sys.h"
#include "themes.gen.h"

using gen::Color;

struct Palette {
    COLORREF c[gen::kColorCount] = {};  // initialised so shell state stays constant-initialised (no CRT)
    bool light = false;
    COLORREF operator[](Color k) const { return c[k]; }

    COLORREF bg()  const { return c[gen::C_BACKGROUND]; }
    COLORREF fg()  const { return c[gen::C_FOREGROUND]; }
    COLORREF accent() const { return c[gen::C_ACCENT]; }
    // Secondary text: stays legible on every theme, light or dark.
    COLORREF dim() const { return sys::mix(fg(), bg(), 45); }
    // Row/pill fill used for hover and selection (foreground at 10-18%).
    COLORREF hover() const { return sys::mix(bg(), fg(), 10); }
    COLORREF selected() const { return sys::mix(bg(), fg(), 18); }
};

struct EmbeddedThemes {
    static constexpr int count() { return gen::kThemeCount; }
    static const wchar_t* name(int i) { return gen::kThemes[i].name; }
    static int bgCount(int i) { return gen::kThemes[i].bgCount; }
    static Palette palette(int i) {
        Palette p;
        for (int k = 0; k < gen::kColorCount; ++k) p.c[k] = gen::kThemes[i].colors[k];
        p.light = gen::kThemes[i].light;
        return p;
    }
    static int find(const wchar_t* n) {
        for (int i = 0; i < gen::kThemeCount; ++i)
            if (sys::weq(gen::kThemes[i].name, n)) return i;
        return -1;
    }
};
