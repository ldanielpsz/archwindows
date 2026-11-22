// Smaller bar panels: power (Omarchy's omarchy.power) and the clock's calendar (omarchy.clock).
#pragma once
#include "sys.h"
#include "ui.h"
#include "session.h"

// Battery state, Windows power mode (the Settings > Power "Power mode" overlay) and session buttons.
struct Power {
    typedef DWORD(WINAPI* GetOverlay_t)(GUID*);
    typedef DWORD(WINAPI* SetOverlay_t)(GUID);
    static inline GetOverlay_t getOverlay;
    static inline SetOverlay_t setOverlay;
    static inline bool tried;
    static constexpr GUID kModes[3] = {
        {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}},  // best power efficiency
        {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}},                                              // balanced
        {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}},  // best performance
    };
    static inline int mode = -1;

    static void load() {
        if (tried) return;
        tried = true;
        HMODULE m = LoadLibraryW(L"powrprof.dll");
        if (!m) return;
        getOverlay = (GetOverlay_t)GetProcAddress(m, "PowerGetEffectiveOverlayScheme");
        setOverlay = (SetOverlay_t)GetProcAddress(m, "PowerSetActiveOverlayScheme");
    }
    static void readMode() {
        load();
        mode = -1;
        GUID g{};
        if (!getOverlay || getOverlay(&g) != ERROR_SUCCESS) return;
        mode = 1;
        for (int i = 0; i < 3; ++i) if (IsEqualGUID(g, kModes[i])) mode = i;
    }

    // ---- bar ----
    static int battery(bool* charging = nullptr) {
        SYSTEM_POWER_STATUS s;
        if (!GetSystemPowerStatus(&s) || (s.BatteryFlag & 128) || s.BatteryLifePercent > 100) return -1;
        if (charging) *charging = s.ACLineStatus == 1;
        return s.BatteryLifePercent;
    }
    static const wchar_t* glyph() {
        bool ch = false;
        const int b = battery(&ch);
        static wchar_t g[2];
        if (b < 0) return L"\xE7E8";  // no battery: the power button
        const int step = b >= 95 ? 10 : b / 10;
        g[0] = ch ? (wchar_t)(0xEBAB + step) : step == 10 ? (wchar_t)0xE83F : (wchar_t)(0xE850 + step);
        g[1] = 0;
        return g;
    }

    // ---- panel ----
    static void open(HMONITOR) { readMode(); }
    static void close() {}
    static bool tick() { return true; }

    static void ui(Ui& u) {
        SYSTEM_POWER_STATUS s{};
        GetSystemPowerStatus(&s);
        const int b = battery();
        wchar_t title[32], meta[64];
        if (b >= 0) {
            wsprintfW(title, L"Battery %d%%", b);
            if (s.ACLineStatus == 1) sys::wcopy(meta, b >= 100 ? L"FULLY CHARGED" : L"CHARGING");
            else if (s.BatteryLifeTime != (DWORD)-1)
                wsprintfW(meta, L"%luH %02luM REMAINING", s.BatteryLifeTime / 3600, s.BatteryLifeTime / 60 % 60);
            else sys::wcopy(meta, L"ON BATTERY");
        } else {
            sys::wcopy(title, L"Power");
            sys::wcopy(meta, L"PLUGGED IN");
        }
        u.hero(glyph(), F_HERO, b >= 0 && b <= 15 && s.ACLineStatus != 1 ? u.p[gen::C_RED] : u.p.fg(), title, meta);
        if (mode >= 0) {
            u.sep();
            u.section(L"POWER MODE");
            static const wchar_t* const ml[] = {L"Efficiency", L"Balanced", L"Performance"};
            const int k = u.buttons(ml, 3, mode);
            if (k >= 0 && setOverlay && setOverlay(kModes[k]) == ERROR_SUCCESS) mode = k;
        }
        u.sep();
        u.section(L"SESSION");
        const int a = u.glyphButtons(Session::glyph, Session::label, Session::kCount);
        if (a >= 0) { u.wantClose = true; Session::run(a); }
    }
};

// Month grid with ISO week numbers; arrows step months.
struct Calendar {
    static inline int year, month, offset;  // offset: months from today's month

    static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
    static int days(int y, int m) {
        static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        return m == 2 && leap(y) ? 29 : d[m - 1];
    }
    static int dow(int y, int m, int d) {  // 0 = Monday (Sakamoto)
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        if (m < 3) y -= 1;
        const int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;  // 0 = Sunday
        return (w + 6) % 7;
    }
    static int yday(int y, int m, int d) { int n = d; for (int i = 1; i < m; ++i) n += days(y, i); return n; }
    static int weeksIn(int y) {  // 53 when the year starts on a Thursday, or on a Wednesday in a leap year
        const int j = dow(y, 1, 1);
        return j == 3 || (j == 2 && leap(y)) ? 53 : 52;
    }
    static int isoWeek(int y, int m, int d) {
        const int w = (yday(y, m, d) - (dow(y, m, d) + 1) + 10) / 7;
        if (w < 1) return weeksIn(y - 1);
        return w > weeksIn(y) ? 1 : w;
    }

    static void open(HMONITOR) { offset = 0; }
    static void close() {}
    static bool tick() { return false; }

    static void ui(Ui& u) {
        static const wchar_t* const dn[] = {L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday"};
        static const wchar_t* const mn[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                            L"August", L"September", L"October", L"November", L"December"};
        SYSTEMTIME t;
        GetLocalTime(&t);
        int y = t.wYear, m = t.wMonth + offset;
        while (m < 1) { m += 12; --y; }
        while (m > 12) { m -= 12; ++y; }
        wchar_t meta[64];
        wsprintfW(meta, L"%d %s %d \x00B7 W%d", t.wDay, mn[t.wMonth - 1], t.wYear, isoWeek(t.wYear, t.wMonth, t.wDay));
        fmt::upper(meta);
        static const wchar_t* const btn[] = {L"\xE76B", L"\xE76C"};
        const int h = u.hero(L"\xE787", F_HERO, u.p.fg(), dn[dow(t.wYear, t.wMonth, t.wDay)], meta, -1, btn, 2);
        if (h == 2) --offset;
        else if (h == 3) ++offset;
        u.sep();
        wchar_t head[32];
        wsprintfW(head, L"%s %d", mn[m - 1], y);
        fmt::upper(head);
        u.section(head);
        const int cw = u.w / 8, ch = u.s(26);
        static const wchar_t* const wd[] = {L"Wk", L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa", L"Su"};
        for (int i = 0; i < 8; ++i) u.r.textCentered(u.x + i * cw, u.y, cw, ch, wd[i], F_SMALL, u.p.dim());
        u.y += ch;
        const int first = dow(y, m, 1), nd = days(y, m);
        int day = 1 - first;
        while (day <= nd) {
            wchar_t wk[4];
            const int ref = day < 1 ? 1 : day;
            wsprintfW(wk, L"%d", isoWeek(y, m, ref));
            u.r.textCentered(u.x, u.y, cw, ch, wk, F_SMALL, sys::mix(u.p.bg(), u.p.dim(), 70));
            for (int c = 0; c < 7; ++c, ++day) {
                if (day < 1 || day > nd) continue;
                wchar_t d[4];
                wsprintfW(d, L"%d", day);
                const bool today = y == t.wYear && m == t.wMonth && day == t.wDay;
                const int cx = u.x + (c + 1) * cw;
                if (today) u.r.round(cx + u.s(3), u.y + u.s(2), cw - u.s(6), ch - u.s(4), u.s(4), u.p.selected(), u.p.dim());
                u.r.textCentered(cx, u.y, cw, ch, d, today ? F_BOLD : F_TEXT,
                                 today ? u.p[gen::C_BRIGHT_FOREGROUND] : c >= 5 ? u.p.dim() : u.p.fg());
            }
            u.y += ch;
        }
    }
};
