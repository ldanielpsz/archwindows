// User-facing toggles (the Options menu). Persisted as one registry DWORD whose bits are
// the *non-default* state, so a fresh install (value 0) gets exactly the defaults below.
#pragma once
#include "sys.h"

struct Options {
    enum Id : int { O_TILING, O_HIDE_TITLEBARS, O_THEME_WINDOWS, O_GAPS, O_STARTUP, O_LIMIT, O_SYSTHEME, O_COUNT };

    static inline bool tiling = false;          // dwindle tiling (off: windows roam free)
    static inline bool hideTitlebars = false;   // strip title bars even when not tiling
    static inline bool themeWindows = true;     // recolour other windows' frames to the theme
    static inline bool gaps = true;             // gaps between tiled windows
    static inline bool limit = true;            // turning tiling on tiles at most kPerMon windows per monitor
    static inline bool sysTheme = false;        // theme drives Windows' dark/light mode and accent (systheme.h)
    static inline bool startup = false;         // launch at sign-in (HKCU Run key)

    static bool get(int i) {
        switch (i) {
            case O_TILING: return tiling;
            case O_HIDE_TITLEBARS: return hideTitlebars;
            case O_THEME_WINDOWS: return themeWindows;
            case O_STARTUP: return startup;
            case O_LIMIT: return limit;
            case O_SYSTHEME: return sysTheme;
            default: return gaps;
        }
    }
    static void set(int i, bool v) {
        switch (i) {
            case O_TILING: tiling = v; break;
            case O_HIDE_TITLEBARS: hideTitlebars = v; break;
            case O_THEME_WINDOWS: themeWindows = v; break;
            case O_STARTUP: startup = v; break;
            case O_LIMIT: limit = v; break;
            case O_SYSTHEME: sysTheme = v; break;
            default: gaps = v; break;
        }
    }
    static unsigned pack() {
        return (tiling ? 1u : 0) | (hideTitlebars ? 2u : 0) | (themeWindows ? 0 : 4u) | (gaps ? 0 : 8u) | (startup ? 16u : 0) | (limit ? 0 : 32u) | (sysTheme ? 64u : 0);
    }
    static void unpack(unsigned v) {
        tiling = v & 1;
        hideTitlebars = v & 2;
        themeWindows = !(v & 4);
        gaps = !(v & 8);
        startup = v & 16;
        limit = !(v & 32);
        sysTheme = v & 64;
    }
};
