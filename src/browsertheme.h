// Browser colours follow the theme the way Omarchy does it (bin/omarchy-theme-set-browser): two
// Chromium policies, BrowserThemeColor = the theme's browser colour (Omarchy's chromium.theme;
// otherwise a neutral seed, Omarchy's #1c2027 for dark themes or a light grey for light ones,
// since Chrome derives light/dark from the seed) and BrowserColorScheme = "device". Chrome then
// builds a calm palette from the seed instead of tinting everything with the theme background.
// Written per user (HKCU) for Google Chrome, Brave and Edge. Running browsers are told to reload
// their policies with `--refresh-platform-policy --no-startup-window`, as Omarchy does; a
// browser that is not running picks the colour up when it starts.
// Only values this shell wrote are removed again (marker under HKCU\Software\ArchWindows).
#pragma once
#include "sys.h"
#include <tlhelp32.h>

struct BrowserTheme {
    struct Browser { const wchar_t* key; const wchar_t* exe; };
    static constexpr Browser kBrowsers[] = {{L"Software\\Policies\\Google\\Chrome", L"chrome.exe"},
                                            {L"Software\\Policies\\BraveSoftware\\Brave", L"brave.exe"},
                                            {L"Software\\Policies\\Microsoft\\Edge", L"msedge.exe"}};
    static constexpr const wchar_t* kMarker = L"Software\\ArchWindows";

    // Omarchy's themes/<name>/chromium.theme values; other themes get a neutral seed.
    static const wchar_t* color(const wchar_t* theme, bool light) {
        static const wchar_t* const map[][2] = {{L"flexoki-light", L"#f2f0e5"}, {L"last-horizon", L"#0c0b0c"},
                                                {L"lumon", L"#0e1f29"},         {L"retro-82", L"#00172e"},
                                                {L"rose-pine", L"#d2c4db"}};
        for (auto& m : map) if (sys::weq(m[0], theme)) return m[1];
        return light ? L"#eeeeee" : L"#1c2027";
    }

    static void apply(const wchar_t* theme, bool light) {
        const wchar_t* hex = color(theme, light);
        for (const Browser& b : kBrowsers) {
            RegSetKeyValueW(HKEY_CURRENT_USER, b.key, L"BrowserThemeColor", REG_SZ, hex, 8 * sizeof(wchar_t));
            RegSetKeyValueW(HKEY_CURRENT_USER, b.key, L"BrowserColorScheme", REG_SZ, L"device", 7 * sizeof(wchar_t));
        }
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, kMarker, L"BrowserThemeSet", REG_DWORD, &one, sizeof(one));
        refreshRunning();
    }
    static void restore() {
        DWORD d = 0, sz = sizeof(d);
        if (RegGetValueW(HKEY_CURRENT_USER, kMarker, L"BrowserThemeSet", RRF_RT_REG_DWORD, nullptr, &d, &sz) != ERROR_SUCCESS || !d)
            return;
        for (const Browser& b : kBrowsers) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, b.key, L"BrowserThemeColor");
            RegDeleteKeyValueW(HKEY_CURRENT_USER, b.key, L"BrowserColorScheme");
        }
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kMarker, L"BrowserThemeSet");
        refreshRunning();
    }

    // Tell each running browser to re-read its policies (a second launch with this switch hands
    // the request to the running instance and exits; no window opens).
    static void refreshRunning() {
        bool running[3] = {};
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            for (int i = 0; i < 3; ++i)
                if (CompareStringOrdinal(pe.szExeFile, -1, kBrowsers[i].exe, -1, TRUE) == CSTR_EQUAL) running[i] = true;
        CloseHandle(snap);
        for (int i = 0; i < 3; ++i) {
            if (!running[i]) continue;
            wchar_t key[160], exe[MAX_PATH];
            wsprintfW(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", kBrowsers[i].exe);
            DWORD sz = sizeof(exe);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, nullptr, exe, &sz) != ERROR_SUCCESS) {
                sz = sizeof(exe);
                if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, exe, &sz) != ERROR_SUCCESS) continue;
            }
            wchar_t cmd[MAX_PATH + 64];
            wsprintfW(cmd, L"\"%s\" --refresh-platform-policy --no-startup-window", exe);
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
};
