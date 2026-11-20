// Small OS-facing policies: asset lookup, wallpaper, persistence, memory trim.
#pragma once
#include "sys.h"

// Where imported assets (assets\themes\<name>\{p.jpg,b0.jpg..}) live.
// Looked up next to the exe, then one level up (so build\archwindows.exe works).
struct Assets {
    static inline wchar_t root[MAX_PATH];
    static inline bool ok;

    static void locate() {
        wchar_t exe[MAX_PATH];
        int n = (int)GetModuleFileNameW(nullptr, exe, MAX_PATH);
        while (n > 0 && exe[n - 1] != L'\\') --n;
        exe[n] = 0;
        static const wchar_t* const rel[] = {L"assets", L"..\\assets"};
        for (const wchar_t* r : rel) {
            wchar_t* e = sys::wcopy(root, exe);
            e = sys::wcopy(e, r);
            sys::wcopy(e, L"\\themes");
            if (GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES) {
                *(e) = 0;
                ok = true;
                return;
            }
        }
        root[0] = 0;
    }
    static void themeFile(wchar_t* out, const wchar_t* theme, const wchar_t* file) {
        wsprintfW(out, L"%s\\themes\\%s\\%s", root, theme, file);
    }
};

// Wallpaper policy: hands the file path to the shell and keeps nothing in our
// own memory. Needs JPEG/PNG/BMP (the importer emits JPEG).
struct SpiWallpaper {
    static bool set(const wchar_t* path) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"WallpaperStyle", 0, REG_SZ, (const BYTE*)L"10", 6);  // fill
            RegSetValueExW(k, L"TileWallpaper", 0, REG_SZ, (const BYTE*)L"0", 4);
            RegCloseKey(k);
        }
        return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)path,
                                     SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != 0;
    }
};

// Persistence policy: two registry values, no config file to parse.
struct RegistryStore {
    static constexpr const wchar_t* key = L"Software\\ArchWindows";

    static bool load(wchar_t* theme, int cap, int& bg) {
        DWORD sz = (DWORD)cap * sizeof(wchar_t), d = 0, dsz = sizeof(d);
        bool ok = RegGetValueW(HKEY_CURRENT_USER, key, L"Theme", RRF_RT_REG_SZ, nullptr, theme, &sz) == ERROR_SUCCESS;
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"Background", RRF_RT_REG_DWORD, nullptr, &d, &dsz) == ERROR_SUCCESS)
            bg = (int)d;
        return ok;
    }
    // Optional logo override (HKCU\Software\ArchWindows): LogoFont = installed font family,
    // LogoGlyph = Unicode codepoint (DWORD). Absent -> the default system power glyph.
    static bool loadLogo(wchar_t* face, int cap, unsigned& glyph) {
        DWORD sz = (DWORD)cap * sizeof(wchar_t), d = 0, dsz = sizeof(d);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"LogoGlyph", RRF_RT_REG_DWORD, nullptr, &d, &dsz) == ERROR_SUCCESS)
            glyph = d;
        return RegGetValueW(HKEY_CURRENT_USER, key, L"LogoFont", RRF_RT_REG_SZ, nullptr, face, &sz) == ERROR_SUCCESS
               && face[0];
    }
    static void save(const wchar_t* theme, int bg) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
        RegSetValueExW(k, L"Theme", 0, REG_SZ, (const BYTE*)theme, (DWORD)(sys::wlen(theme) + 1) * sizeof(wchar_t));
        DWORD d = (DWORD)bg;
        RegSetValueExW(k, L"Background", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
};

// Memory policy: ask Windows to drop our pages once we go quiet. RAM is the
// primary budget, so every burst of activity ends with this.
struct TrimMemory {
    static void now() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }
};
