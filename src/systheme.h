// Optional: let the shell's theme drive Windows' own colours (Options > "Theme drives Windows colours").
//
//   * Dark/light: AppsUseLightTheme and SystemUsesLightTheme follow the theme's `light` flag, so
//     Explorer, Settings, Terminal, Chrome/Edge (set to follow the device) and the taskbar flyouts
//     switch with it. Windows is told with the same WM_SETTINGCHANGE("ImmersiveColorSet") broadcast
//     the Settings app sends.
//   * Accent: the theme's accent goes into the DWM/Explorer accent values (undocumented but what
//     Settings itself writes).
//
// This changes user-wide settings, so the original values are backed up under
// HKCU\Software\ArchWindows\SysThemeBackup the first time, and put back when the option is turned
// off, the shell quits, or `--reset` / the next start finds a backup left by a crash.
// Only committed themes are applied (not picker previews), and the broadcast runs on a short-lived
// thread so a slow or hung app can never stall the shell.
#pragma once
#include "sys.h"
#include "theme.h"

struct SysTheme {
    struct Val { const wchar_t* sub; const wchar_t* name; };
    static constexpr const wchar_t* kPers = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr const wchar_t* kDwm = L"Software\\Microsoft\\Windows\\DWM";
    static constexpr const wchar_t* kAccent = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";
    static constexpr const wchar_t* kBackup = L"Software\\ArchWindows\\SysThemeBackup";
    static constexpr int kN = 6;
    static constexpr Val kVals[kN] = {
        {kPers, L"AppsUseLightTheme"}, {kPers, L"SystemUsesLightTheme"},
        {kDwm, L"AccentColor"},        {kAccent, L"AccentColorMenu"},
        {kAccent, L"StartColorMenu"},  {kAccent, L"AccentPalette"},
    };
    static inline bool active;

    // ------------------------------------------------------------- registry --
    static bool get(const Val& v, BYTE* buf, DWORD& sz, DWORD& type) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, v.sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
        sz = 64;
        const bool ok = RegQueryValueExW(k, v.name, nullptr, &type, buf, &sz) == ERROR_SUCCESS;
        RegCloseKey(k);
        return ok;
    }
    static void put(const Val& v, DWORD type, const void* data, DWORD sz) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, v.sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
        RegSetValueExW(k, v.name, 0, type, (const BYTE*)data, sz);
        RegCloseKey(k);
    }
    static void del(const Val& v) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, v.sub, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
        RegDeleteValueW(k, v.name);
        RegCloseKey(k);
    }
    static bool hasBackup() {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackup, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
        RegCloseKey(k);
        return true;
    }

    // Per value i: "a<i>" = 1 when it did not exist, else "t<i>" (type) and "v<i>" (data).
    static void backupOnce() {
        if (hasBackup()) return;  // keep the true originals across crashes and restarts
        HKEY b;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kBackup, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &b, nullptr) != ERROR_SUCCESS) return;
        for (int i = 0; i < kN; ++i) {
            BYTE buf[64];
            DWORD sz = 0, type = 0;
            wchar_t n[8];
            if (get(kVals[i], buf, sz, type)) {
                wsprintfW(n, L"t%d", i);
                RegSetValueExW(b, n, 0, REG_DWORD, (const BYTE*)&type, sizeof(type));
                wsprintfW(n, L"v%d", i);
                RegSetValueExW(b, n, 0, REG_BINARY, buf, sz);
            } else {
                wsprintfW(n, L"a%d", i);
                const DWORD one = 1;
                RegSetValueExW(b, n, 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            }
        }
        RegCloseKey(b);
    }

    // ------------------------------------------------------------ broadcast --
    static void broadcast() {
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                            SMTO_ABORTIFHUNG, 100, &r);
    }
    static DWORD WINAPI broadcastThread(LPVOID) { broadcast(); return 0; }
    static void broadcastAsync() {
        HANDLE t = CreateThread(nullptr, 0, broadcastThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else broadcast();
    }

    // -------------------------------------------------------------- apply --
    static bool setIfChanged(const Val& v, DWORD type, const void* data, DWORD sz) {
        BYTE cur[64];
        DWORD csz = 0, ctype = 0;
        if (get(v, cur, csz, ctype) && ctype == type && csz == sz) {
            bool same = true;
            for (DWORD i = 0; i < sz; ++i) same &= cur[i] == ((const BYTE*)data)[i];
            if (same) return false;
        }
        put(v, type, data, sz);
        return true;
    }

    static DWORD abgr(COLORREF c) { return 0xFF000000u | ((DWORD)GetBValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetRValue(c); }

    static void apply(const Palette& p) {
        backupOnce();
        active = true;
        bool changed = false;
        const DWORD light = p.light ? 1 : 0;
        changed |= setIfChanged(kVals[0], REG_DWORD, &light, sizeof(light));
        changed |= setIfChanged(kVals[1], REG_DWORD, &light, sizeof(light));
        const DWORD acc = abgr(p.accent());
        changed |= setIfChanged(kVals[2], REG_DWORD, &acc, sizeof(acc));
        changed |= setIfChanged(kVals[3], REG_DWORD, &acc, sizeof(acc));
        changed |= setIfChanged(kVals[4], REG_DWORD, &acc, sizeof(acc));
        // Eight swatches (R,G,B,0), light to dark, around the accent, as Settings lays them out.
        static constexpr int kMix[8] = {-60, -40, -20, 0, 20, 40, 60, 80};  // <0 toward white, >0 toward black
        BYTE pal[32];
        for (int i = 0; i < 8; ++i) {
            const COLORREF c = kMix[i] < 0 ? sys::mix(p.accent(), RGB(255, 255, 255), -kMix[i])
                                           : sys::mix(p.accent(), RGB(0, 0, 0), kMix[i]);
            pal[i * 4] = GetRValue(c);
            pal[i * 4 + 1] = GetGValue(c);
            pal[i * 4 + 2] = GetBValue(c);
            pal[i * 4 + 3] = 0;
        }
        changed |= setIfChanged(kVals[5], REG_BINARY, pal, sizeof(pal));
        if (changed) broadcastAsync();
    }

    // Put the original values back (no-op when there is no backup). Synchronous: used on the way out.
    static void restore() {
        HKEY b;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackup, 0, KEY_QUERY_VALUE, &b) != ERROR_SUCCESS) { active = false; return; }
        for (int i = 0; i < kN; ++i) {
            wchar_t n[8];
            BYTE buf[64];
            DWORD sz = sizeof(buf), type = 0, ty = sizeof(DWORD), tsz = sizeof(DWORD);
            wsprintfW(n, L"t%d", i);
            const bool had = RegQueryValueExW(b, n, nullptr, nullptr, (BYTE*)&type, &tsz) == ERROR_SUCCESS;
            (void)ty;
            wsprintfW(n, L"v%d", i);
            if (had && RegQueryValueExW(b, n, nullptr, nullptr, buf, &sz) == ERROR_SUCCESS) put(kVals[i], type, buf, sz);
            else del(kVals[i]);
        }
        RegCloseKey(b);
        RegDeleteKeyW(HKEY_CURRENT_USER, kBackup);
        active = false;
        broadcast();
    }
};
