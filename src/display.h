// Display panel (Omarchy's omarchy.monitor): Windows text size, per-display scale, display list.
//
// Scale uses the same steps Windows Settings offers (100% .. 500%, capped at the largest step the
// display allows). It is read and set through DisplayConfigGet/SetDeviceInfo with the
// source-DPI-scale request types Settings itself uses (-3 get, -4 set, relative to the display's
// recommended step). Text size is Windows' "Make text bigger" (Accessibility\TextScaleFactor,
// 100..225%); apps pick it up as they redraw or restart, as with the Settings page.
#pragma once
#include "sys.h"
#include "ui.h"

struct Display {
    static constexpr int kSteps[] = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};
    static constexpr int kNSteps = sizeof(kSteps) / sizeof(kSteps[0]);
    static constexpr int kMax = 6;

    struct Mon {
        wchar_t name[64], gdi[32];
        LUID adapter;
        UINT32 source;
        HMONITOR h;
        int rec, cur, maxIdx;  // indices into kSteps
        int w, hgt, hz;
        bool internal;
    };
    static inline Mon mons[kMax];
    static inline int n, sel;
    static inline HMONITOR focused;
    static inline int textScale = 100;

    struct ScaleGet { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 minRel, curRel, maxRel; };
    struct ScaleSet { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 rel; };

    struct Find { const wchar_t* gdi; HMONITOR h; };
    static BOOL CALLBACK findMon(HMONITOR h, HDC, LPRECT, LPARAM l) {
        Find* f = (Find*)l;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(h, &mi) && sys::weq(mi.szDevice, f->gdi)) { f->h = h; return FALSE; }
        return TRUE;
    }

    static void refresh() {
        n = 0;
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
        auto* paths = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(GetProcessHeap(), 0, np * sizeof(DISPLAYCONFIG_PATH_INFO));
        auto* modes = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(GetProcessHeap(), 0, nm * sizeof(DISPLAYCONFIG_MODE_INFO));
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np && n < kMax; ++i) {
                const DISPLAYCONFIG_PATH_INFO& p = paths[i];
                Mon& m = mons[n];
                m = Mon{};
                m.adapter = p.sourceInfo.adapterId;
                m.source = p.sourceInfo.id;
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
                sn.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(sn), p.sourceInfo.adapterId, p.sourceInfo.id};
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
                lstrcpynW(m.gdi, sn.viewGdiDeviceName, 32);
                bool dup = false;  // a cloned (mirrored) source shows up once per target
                for (int k = 0; k < n; ++k) dup |= sys::weq(mons[k].gdi, m.gdi);
                if (dup) continue;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
                tn.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tn), p.targetInfo.adapterId, p.targetInfo.id};
                if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
                    const auto tech = tn.outputTechnology;
                    m.internal = tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                                 tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                                 tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
                    lstrcpynW(m.name, tn.monitorFriendlyDeviceName, 64);
                }
                if (!m.name[0]) sys::wcopy(m.name, m.internal ? L"Built-in Display" : L"Display");
                if (p.sourceInfo.modeInfoIdx < nm && modes[p.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    m.w = (int)modes[p.sourceInfo.modeInfoIdx].sourceMode.width;
                    m.hgt = (int)modes[p.sourceInfo.modeInfoIdx].sourceMode.height;
                }
                if (p.targetInfo.refreshRate.Denominator)
                    m.hz = (int)((p.targetInfo.refreshRate.Numerator + p.targetInfo.refreshRate.Denominator / 2) /
                                 p.targetInfo.refreshRate.Denominator);
                ScaleGet g{};
                g.h = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)-3, sizeof(g), m.adapter, m.source};
                if (DisplayConfigGetDeviceInfo(&g.h) == ERROR_SUCCESS) {
                    m.rec = g.minRel < 0 ? -g.minRel : 0;
                    m.cur = m.rec + g.curRel;
                    m.maxIdx = m.rec + g.maxRel;
                } else {  // fall back to the monitor's effective DPI
                    m.rec = m.cur = 0;
                    m.maxIdx = 0;
                }
                if (m.maxIdx >= kNSteps) m.maxIdx = kNSteps - 1;
                if (m.cur < 0) m.cur = 0;
                if (m.cur > m.maxIdx) m.cur = m.maxIdx;
                Find f{m.gdi, nullptr};
                EnumDisplayMonitors(nullptr, nullptr, findMon, (LPARAM)&f);
                m.h = f.h;
                ++n;
            }
        }
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        if (modes) HeapFree(GetProcessHeap(), 0, modes);
        DWORD d = 100, sz = sizeof(d);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                         nullptr, &d, &sz) != ERROR_SUCCESS) d = 100;
        textScale = (int)d;
    }

    static void setScale(int idx) {
        Mon& m = mons[sel];
        if (idx < 0 || idx > m.maxIdx || idx == m.cur) return;
        ScaleSet s{};
        s.h = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)-4, sizeof(s), m.adapter, m.source};
        s.rel = idx - m.rec;
        if (DisplayConfigSetDeviceInfo(&s.h) == ERROR_SUCCESS) m.cur = idx;
    }
    static void setText(int pct) {
        if (pct == textScale) return;
        textScale = pct;
        DWORD d = (DWORD)pct;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", REG_DWORD, &d, sizeof(d));
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Accessibility", SMTO_ABORTIFHUNG, 200, &r);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETNONCLIENTMETRICS, (LPARAM)L"WindowMetrics",
                            SMTO_ABORTIFHUNG, 200, &r);
    }

    static void open(HMONITOR m) {
        focused = m;
        refresh();
        sel = 0;
        for (int i = 0; i < n; ++i) if (mons[i].h == m) sel = i;
    }
    static void close() {}
    static bool tick() { return false; }

    static void ui(Ui& u) {
        const Mon* m = n ? &mons[sel] : nullptr;
        wchar_t meta[48];
        if (m && m->w) wsprintfW(meta, L"%dx%d \x00B7 %d HZ", m->w, m->hgt, m->hz);
        else sys::wcopy(meta, L"FIXED BRIGHTNESS");
        u.hero(L"\xE7F4", F_HERO, u.p.fg(), L"Display", meta);
        u.sep();
        wchar_t pc[16];
        wsprintfW(pc, L"%d%%", textScale);
        u.section(L"TEXT SIZE", pc);
        // 100..225 in the six 25% steps Settings' slider snaps to
        const int v = u.slider((textScale - 100) * 1000 / 125, 1000, 6);
        if (v >= 0) setText(100 + v * 125 / 1000);
        if (!m) return;
        u.sep();
        wchar_t nm[64];
        lstrcpynW(nm, m->name, 64);
        fmt::upper(nm);
        u.section(L"SCALE", nm);
        const wchar_t* labels[kNSteps];
        wchar_t txt[kNSteps][8];
        int cnt = 0, first = 0;
        const int show = 6;  // at most six choices, a window around the current step
        if (m->maxIdx + 1 > show) { first = m->cur - 2; if (first < 0) first = 0; if (first > m->maxIdx + 1 - show) first = m->maxIdx + 1 - show; }
        for (int i = first; i <= m->maxIdx && cnt < show; ++i) {
            const int s = kSteps[i];
            if (s % 100 == 0) wsprintfW(txt[cnt], L"%dx", s / 100);
            else if (s % 10 == 0) wsprintfW(txt[cnt], L"%d.%dx", s / 100, s / 10 % 10);
            else wsprintfW(txt[cnt], L"%d.%02dx", s / 100, s % 100);
            labels[cnt] = txt[cnt];
            ++cnt;
        }
        const int k = u.buttons(labels, cnt, m->cur - first);
        if (k >= 0) setScale(first + k);
        u.sep();
        u.section(L"DISPLAYS");
        for (int i = 0; i < n; ++i) {
            wchar_t lab[96];
            if (mons[i].h == focused) wsprintfW(lab, L"%s \x00B7 focused", mons[i].name);
            else lstrcpynW(lab, mons[i].name, 96);
            if (u.row(mons[i].internal ? L"\xE7F8" : L"\xE7F4", lab, nullptr, i == sel, L"\xE73E")) sel = i;
        }
    }
};
