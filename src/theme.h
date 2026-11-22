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
    // Derived colours follow Omarchy's shell (default/themed/shell.toml.tpl and its Qt code), so
    // the same theme looks the same here. Alpha-over-background becomes an opaque mix.
    //
    // Secondary text (panel subtitles, captions, inactive titles): Qt.darker(foreground, 1.4),
    // kept at 3:1 or better against the background.
    COLORREF dim() const {
        const COLORREF f = fg();
        const COLORREF d = RGB(GetRValue(f) * 10 / 14, GetGValue(f) * 10 / 14, GetBValue(f) * 10 / 14);
        return sys::ensure(d, bg(), 300, f);
    }
    // Unused workspace numbers: foreground at 50% opacity (Omarchy's Workspaces widget).
    COLORREF faint() const { return sys::mix(bg(), fg(), 50); }
    // Control states ([controls]): idle fill 4% / border 40%, hover fill 8% / border 25%,
    // selected fill 18%.
    COLORREF idleFill() const { return sys::mix(bg(), fg(), 4); }
    COLORREF idleBorder() const { return sys::mix(bg(), fg(), 40); }
    COLORREF hover() const { return sys::mix(bg(), fg(), 8); }
    COLORREF hoverBorder() const { return sys::mix(bg(), fg(), 25); }
    COLORREF selected() const { return sys::mix(bg(), fg(), 18); }
    // Menu ([menu]): border is the foreground, the selected row foreground at 8% with a 25%
    // outline and accent text.
    COLORREF menuBorder() const { return fg(); }
    COLORREF menuSelected() const { return hover(); }
    COLORREF menuSelectedBorder() const { return hoverBorder(); }
    // Accent used as text on a highlighted row: the theme accent, lifted toward the bright
    // foreground only where it would fall under 3:1 on that fill (it rarely does on 8%).
    COLORREF accentText() const { return sys::ensure(accent(), selected(), 300, c[gen::C_BRIGHT_FOREGROUND]); }
    // Window borders (Hyprland): active = accent, inactive = rgba(595959aa) over the background.
    COLORREF borderActive() const { return accent(); }
    COLORREF borderInactive() const { return sys::mix(bg(), RGB(0x59, 0x59, 0x59), 67); }
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
