// Optional: terminal colours follow the shell's theme (Options > "Theme terminal colours"),
// the way Omarchy re-themes its terminal.
//
//   * Windows Terminal (packaged, Preview, unpackaged): a colour scheme named "archwindows" is
//     written into settings.json and profiles.defaults.colorScheme points at it. Terminal reloads
//     settings.json by itself, so open windows change at once. Profiles that set their own
//     colorScheme keep it.
//   * mintty (Git Bash): colour lines in %USERPROFILE%\.minttyrc (other lines kept) for new
//     windows; open windows change at their next prompt through a small ~/.bashrc block.
//   * Classic console: HKCU\Console ColorTable00-15 for new windows; open (non-elevated) consoles
//     are recoloured on the spot.
//
// settings.json is JSON with comments, edited in place: only the "archwindows" scheme object and
// the one defaults value are touched, the rest of the file is kept byte for byte. The originals
// (previous defaults.colorScheme, .minttyrc, console table) are backed up the first time, under
// HKCU\Software\ArchWindows\TermThemeBackup and .minttyrc.archwindows-backup, and put back when the
// option is turned off. Unlike SysTheme this is not undone on quit: like the wallpaper, it stays.
#pragma once
#include "sys.h"
#include "theme.h"

struct TermTheme {
    static constexpr const wchar_t* kBackup = L"Software\\ArchWindows\\TermThemeBackup";
    static constexpr const char* kName = "archwindows";

    // ------------------------------------------------------------- palette --
    struct Ansi { COLORREF bg, fg, cursor, sel, c[16]; };
    // Exactly Omarchy's terminal mapping (default/themed/alacritty.toml.tpl): black = background,
    // white = foreground, bright black = muted, bright white = bright_foreground, cursor =
    // bright_foreground, selection = selection (Omarchy's selection_background).
    static Ansi ansi(const Palette& p) {
        Ansi a;
        a.bg = p[gen::C_BACKGROUND];
        a.fg = p[gen::C_FOREGROUND];
        a.cursor = p[gen::C_BRIGHT_FOREGROUND];
        a.sel = p[gen::C_SELECTION];
        a.c[0] = p[gen::C_BACKGROUND];
        a.c[1] = p[gen::C_RED];
        a.c[2] = p[gen::C_GREEN];
        a.c[3] = p[gen::C_YELLOW];
        a.c[4] = p[gen::C_BLUE];
        a.c[5] = p[gen::C_MAGENTA];
        a.c[6] = p[gen::C_CYAN];
        a.c[7] = p[gen::C_FOREGROUND];
        a.c[8] = p[gen::C_MUTED];
        a.c[9] = p[gen::C_BRIGHT_RED];
        a.c[10] = p[gen::C_BRIGHT_GREEN];
        a.c[11] = p[gen::C_BRIGHT_YELLOW];
        a.c[12] = p[gen::C_BRIGHT_BLUE];
        a.c[13] = p[gen::C_BRIGHT_MAGENTA];
        a.c[14] = p[gen::C_BRIGHT_CYAN];
        a.c[15] = p[gen::C_BRIGHT_FOREGROUND];
        return a;
    }

    // ---------------------------------------------------------------- bytes --
    struct Buf { char* p; DWORD n; };
    static void* alloc(DWORD n) { return HeapAlloc(GetProcessHeap(), 0, n); }
    static void release(Buf& b) { if (b.p) HeapFree(GetProcessHeap(), 0, b.p); b.p = nullptr; b.n = 0; }

    static bool readFile(const wchar_t* path, Buf& out) {
        out = {nullptr, 0};
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        const DWORD n = GetFileSize(f, nullptr);
        bool ok = false;
        if (n != INVALID_FILE_SIZE && n < (8u << 20) && (out.p = (char*)alloc(n + 1))) {
            DWORD got = 0;
            ok = ReadFile(f, out.p, n, &got, nullptr) && got == n;
            out.n = n;
            if (!ok) release(out);
        }
        CloseHandle(f);
        return ok;
    }
    // Write beside, then swap in, so a reader never sees half a file.
    static bool writeFile(const wchar_t* path, const char* data, DWORD n) {
        wchar_t tmp[MAX_PATH + 16];
        sys::wcopy(sys::wcopy(tmp, path), L".aw-tmp");
        HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0;
        const bool ok = WriteFile(f, data, n, &w, nullptr) && w == n;
        CloseHandle(f);
        if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return false; }
        return true;
    }
    // b with [at, at+del) replaced by ins.
    static bool splice(Buf& b, DWORD at, DWORD del, const char* ins, DWORD insN) {
        const DWORD n = b.n - del + insN;
        char* p = (char*)alloc(n + 1);
        if (!p) return false;
        DWORD k = 0;
        for (DWORD i = 0; i < at; ++i) p[k++] = b.p[i];
        for (DWORD i = 0; i < insN; ++i) p[k++] = ins[i];
        for (DWORD i = at + del; i < b.n; ++i) p[k++] = b.p[i];
        release(b);
        b = {p, n};
        return true;
    }
    static DWORD slen(const char* s) { DWORD n = 0; while (s[n]) ++n; return n; }

    static bool envPath(const wchar_t* var, const wchar_t* tail, wchar_t* out) {
        const DWORD n = GetEnvironmentVariableW(var, out, MAX_PATH);
        if (!n || n >= MAX_PATH - (DWORD)sys::wlen(tail)) return false;
        sys::wcopy(out + n, tail);
        return true;
    }

    // ---------------------------------------------------------------- JSONC --
    // Minimal scanner over UTF-8 JSON-with-comments. Positions are byte offsets; -1 = not found.
    struct Json {
        const char* s;
        int n;
        int ws(int i) const {
            for (;;) {
                while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
                if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') { while (i < n && s[i] != '\n') ++i; continue; }
                if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                    i += 2;
                    continue;
                }
                return i;
            }
        }
        int str(int i) const {  // s[i] == '"': index past the closing quote
            for (++i; i < n; ++i) {
                if (s[i] == '\\') ++i;
                else if (s[i] == '"') return i + 1;
            }
            return n;
        }
        int value(int i) const {  // index past the value starting at i
            if (i >= n) return n;
            if (s[i] == '"') return str(i);
            if (s[i] == '{' || s[i] == '[') {
                int depth = 0;
                while (i < n) {
                    const int j = ws(i);
                    if (j != i) { i = j; continue; }
                    const char c = s[i];
                    if (c == '"') { i = str(i); continue; }
                    if (c == '{' || c == '[') ++depth;
                    else if ((c == '}' || c == ']') && --depth == 0) return i + 1;
                    ++i;
                }
                return n;
            }
            while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t') ++i;
            return i;
        }
        bool strIs(int i, const char* lit) const {  // the string at i equals lit (no escapes in lit)
            if (i < 0 || i >= n || s[i] != '"') return false;
            int k = 0;
            for (++i; lit[k]; ++i, ++k) if (i >= n || s[i] != lit[k]) return false;
            return i < n && s[i] == '"';
        }
        // Member `key` of the object opening at `obj`: value start, plus the key's start.
        int find(int obj, const char* key, int* keyAt = nullptr) const {
            if (obj < 0 || obj >= n || s[obj] != '{') return -1;
            int i = obj + 1;
            for (;;) {
                i = ws(i);
                if (i >= n || s[i] != '"') return -1;
                const int k = i;
                const bool hit = strIs(i, key);
                i = ws(str(i));
                if (i >= n || s[i] != ':') return -1;
                i = ws(i + 1);
                if (hit) { if (keyAt) *keyAt = k; return i; }
                i = ws(value(i));
                if (i < n && s[i] == ',') ++i;
            }
        }
        bool empty(int open) const { const int i = ws(open + 1); return i < n && (s[i] == '}' || s[i] == ']'); }
        int root() const {
            int i = (n >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) ? 3 : 0;
            i = ws(i);
            return i < n && s[i] == '{' ? i : -1;
        }
    };

    // ------------------------------------------------------ Windows Terminal --
    static int hex(char* o, COLORREF c) { return wsprintfA(o, "\"#%02X%02X%02X\"", GetRValue(c), GetGValue(c), GetBValue(c)); }

    static DWORD schemeJson(const Ansi& a, char* o) {
        static const char* const names[16] = {"black", "red", "green", "yellow", "blue", "purple", "cyan", "white",
                                              "brightBlack", "brightRed", "brightGreen", "brightYellow",
                                              "brightBlue", "brightPurple", "brightCyan", "brightWhite"};
        char* p = o;
        p += wsprintfA(p, "{\r\n            \"name\": \"%s\",\r\n", kName);
        p += wsprintfA(p, "            \"background\": "); p += hex(p, a.bg);
        p += wsprintfA(p, ",\r\n            \"foreground\": "); p += hex(p, a.fg);
        p += wsprintfA(p, ",\r\n            \"cursorColor\": "); p += hex(p, a.cursor);
        p += wsprintfA(p, ",\r\n            \"selectionBackground\": "); p += hex(p, a.sel);
        for (int i = 0; i < 16; ++i) { p += wsprintfA(p, ",\r\n            \"%s\": ", names[i]); p += hex(p, a.c[i]); }
        p += wsprintfA(p, "\r\n        }");
        return (DWORD)(p - o);
    }

    static bool putScheme(Buf& b, const Ansi& a) {
        char sc[1400];
        const DWORD scN = schemeJson(a, sc);
        Json j{b.p, (int)b.n};
        const int root = j.root();
        if (root < 0) return false;
        const int arr = j.find(root, "schemes");
        if (arr < 0) {  // no schemes at all: add the array
            char ins[1500];
            const DWORD n = (DWORD)wsprintfA(ins, "\r\n    \"schemes\": [%s]%s", sc, j.empty(root) ? "" : ",");
            return splice(b, root + 1, 0, ins, n);
        }
        if (b.p[arr] != '[') return false;
        for (int i = arr + 1;;) {  // replace ours if it is there
            i = j.ws(i);
            if (i >= j.n || b.p[i] == ']') break;
            const int end = j.value(i);
            if (b.p[i] == '{' && j.strIs(j.find(i, "name"), kName)) return splice(b, i, end - i, sc, scN);
            i = j.ws(end);
            if (i < j.n && b.p[i] == ',') ++i;
        }
        char ins[1500];
        const DWORD n = (DWORD)wsprintfA(ins, "\r\n        %s%s", sc, j.empty(arr) ? "" : ",");
        return splice(b, arr + 1, 0, ins, n);
    }

    // Point profiles.defaults.colorScheme at ours; the value it replaces goes to `prev` (raw JSON
    // text, or `absent`). Returns false when there is nothing to change or the file is unusual.
    static bool pointDefaults(Buf& b, char* prev, DWORD& prevN, bool& absent) {
        Json j{b.p, (int)b.n};
        const int root = j.root();
        const int prof = j.find(root, "profiles");
        if (prof < 0 || b.p[prof] != '{') return false;  // legacy array form: leave it alone
        const int def = j.find(prof, "defaults");
        static const char kVal[] = "\"archwindows\"";
        if (def < 0) {
            char ins[96];
            const DWORD n = (DWORD)wsprintfA(ins, "\r\n        \"defaults\": { \"colorScheme\": %s }%s", kVal, j.empty(prof) ? "" : ",");
            absent = true;
            prevN = 0;
            return splice(b, prof + 1, 0, ins, n);
        }
        if (b.p[def] != '{') return false;
        const int cs = j.find(def, "colorScheme");
        if (cs < 0) {
            char ins[96];
            const DWORD n = (DWORD)wsprintfA(ins, "\r\n            \"colorScheme\": %s%s", kVal, j.empty(def) ? "" : ",");
            absent = true;
            prevN = 0;
            return splice(b, def + 1, 0, ins, n);
        }
        if (j.strIs(cs, kName)) return false;  // already ours
        const int end = j.value(cs);
        absent = false;
        prevN = (DWORD)(end - cs) < 512 ? (DWORD)(end - cs) : 0;
        for (DWORD i = 0; i < prevN; ++i) prev[i] = b.p[cs + i];
        return splice(b, cs, end - cs, kVal, sizeof(kVal) - 1);
    }

    // Undo pointDefaults: put `prev` back, or remove the member we added.
    static bool restoreDefaults(Buf& b, const char* prev, DWORD prevN, bool absent) {
        Json j{b.p, (int)b.n};
        const int def = j.find(j.find(j.root(), "profiles"), "defaults");
        int keyAt = -1;
        const int cs = j.find(def, "colorScheme", &keyAt);
        if (cs < 0 || !j.strIs(cs, kName)) return false;  // user changed it since: keep theirs
        const int end = j.value(cs);
        if (!absent) return prevN && splice(b, cs, end - cs, prev, prevN);
        int a = keyAt, e = j.ws(end);
        if (e < j.n && b.p[e] == ',') ++e;  // "key": v,  -> drop through the comma
        else {                               // last member: drop the comma before it instead
            int k = a - 1;
            while (k > def && (b.p[k] == ' ' || b.p[k] == '\t' || b.p[k] == '\r' || b.p[k] == '\n')) --k;
            if (b.p[k] == ',') a = k;
            e = end;
        }
        return splice(b, a, e - a, "", 0);
    }

    // Terminal's window chrome (tab row, title bar, light/dark) is a separate "theme", not part of the
    // colour scheme, so a light palette otherwise keeps Terminal's default dark tab row. Ours paints
    // the tab row in the palette and follows its lightness.
    static DWORD themeJson(const Palette& p, char* o) {
        char row[16], tab[16];
        hex(row, sys::mix(p.bg(), p.fg(), 8));
        hex(tab, sys::mix(p.bg(), p.fg(), 4));
        return (DWORD)wsprintfA(o,
            "{\r\n            \"name\": \"%s\",\r\n"
            "            \"window\": { \"applicationTheme\": \"%s\", \"useMica\": false },\r\n"
            "            \"tabRow\": { \"background\": %s, \"unfocusedBackground\": %s },\r\n"
            "            \"tab\": { \"background\": \"terminalBackground\", \"unfocusedBackground\": %s, \"showCloseButton\": \"hover\" }\r\n"
            "        }", kName, p.light ? "light" : "dark", row, row, tab);
    }
    static bool putTheme(Buf& b, const Palette& p) {
        char th[700];
        const DWORD thN = themeJson(p, th);
        Json j{b.p, (int)b.n};
        const int root = j.root();
        if (root < 0) return false;
        const int arr = j.find(root, "themes");
        char ins[800];
        if (arr < 0) {
            const DWORD n = (DWORD)wsprintfA(ins, "\r\n    \"themes\": [%s]%s", th, j.empty(root) ? "" : ",");
            return splice(b, root + 1, 0, ins, n);
        }
        if (b.p[arr] != '[') return false;
        for (int i = arr + 1;;) {
            i = j.ws(i);
            if (i >= j.n || b.p[i] == ']') break;
            const int end = j.value(i);
            if (b.p[i] == '{' && j.strIs(j.find(i, "name"), kName)) return splice(b, i, end - i, th, thN);
            i = j.ws(end);
            if (i < j.n && b.p[i] == ',') ++i;
        }
        const DWORD n = (DWORD)wsprintfA(ins, "\r\n        %s%s", th, j.empty(arr) ? "" : ",");
        return splice(b, arr + 1, 0, ins, n);
    }
    // Root "theme": "archwindows"; the value it replaces goes to `prev` (or `absent`).
    static bool pointTheme(Buf& b, char* prev, DWORD& prevN, bool& absent) {
        Json j{b.p, (int)b.n};
        const int root = j.root();
        if (root < 0) return false;
        static const char kVal[] = "\"archwindows\"";
        const int t = j.find(root, "theme");
        if (t < 0) {
            char ins[64];
            const DWORD n = (DWORD)wsprintfA(ins, "\r\n    \"theme\": %s%s", kVal, j.empty(root) ? "" : ",");
            absent = true;
            prevN = 0;
            return splice(b, root + 1, 0, ins, n);
        }
        if (j.strIs(t, kName)) return false;
        const int end = j.value(t);
        absent = false;
        prevN = (DWORD)(end - t) < 512 ? (DWORD)(end - t) : 0;
        for (DWORD i = 0; i < prevN; ++i) prev[i] = b.p[t + i];
        return splice(b, t, end - t, kVal, sizeof(kVal) - 1);
    }
    static bool restoreTheme(Buf& b, const char* prev, DWORD prevN, bool absent) {
        Json j{b.p, (int)b.n};
        const int root = j.root();
        int keyAt = -1;
        const int t = j.find(root, "theme", &keyAt);
        if (t < 0 || !j.strIs(t, kName)) return false;  // user changed it since: keep theirs
        const int end = j.value(t);
        if (!absent) return prevN && splice(b, t, end - t, prev, prevN);
        int a = keyAt, e = j.ws(end);
        if (e < j.n && b.p[e] == ',') ++e;
        else {
            int k = a - 1;
            while (k > root && (b.p[k] == ' ' || b.p[k] == '\t' || b.p[k] == '\r' || b.p[k] == '\n')) --k;
            if (b.p[k] == ',') a = k;
            e = end;
        }
        return splice(b, a, e - a, "", 0);
    }

    static int wtPaths(wchar_t (*out)[MAX_PATH]) {
        static const wchar_t* const tails[] = {
            L"\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json",
            L"\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json",
            L"\\Microsoft\\Windows Terminal\\settings.json"};
        int n = 0;
        for (const wchar_t* t : tails)
            if (envPath(L"LOCALAPPDATA", t, out[n]) && GetFileAttributesW(out[n]) != INVALID_FILE_ATTRIBUTES) ++n;
        return n;
    }

    // ------------------------------------------------------------- registry --
    static HKEY backupKey(bool create) {
        HKEY k = nullptr;
        if (create) RegCreateKeyExW(HKEY_CURRENT_USER, kBackup, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
        else RegOpenKeyExW(HKEY_CURRENT_USER, kBackup, 0, KEY_ALL_ACCESS, &k);
        return k;
    }
    static bool has(HKEY k, const wchar_t* name) { return RegQueryValueExW(k, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; }

    // ------------------------------------------------------------------ apply --
    static void applyTerminal(const Ansi& a, const Palette& pal, HKEY bk) {
        wchar_t paths[3][MAX_PATH];
        const int np = wtPaths(paths);
        for (int i = 0; i < np; ++i) {
            Buf b;
            if (!readFile(paths[i], b)) continue;
            bool changed = putScheme(b, a);
            changed |= putTheme(b, pal);
            {
                char prev[512];
                DWORD prevN = 0;
                bool absent = false;
                if (pointTheme(b, prev, prevN, absent)) {
                    changed = true;
                    wchar_t vn[16];
                    wsprintfW(vn, L"wtt%d", i);
                    if (!has(bk, vn)) {
                        if (absent) { const DWORD one = 1; RegSetValueExW(bk, vn, 0, REG_DWORD, (const BYTE*)&one, sizeof(one)); }
                        else RegSetValueExW(bk, vn, 0, REG_BINARY, (const BYTE*)prev, prevN);
                    }
                }
            }
            char prev[512];
            DWORD prevN = 0;
            bool absent = false;
            if (pointDefaults(b, prev, prevN, absent)) {
                changed = true;
                wchar_t vn[16];
                wsprintfW(vn, L"wt%d", i);
                if (!has(bk, vn)) {  // the true original only
                    if (absent) { const DWORD one = 1; RegSetValueExW(bk, vn, 0, REG_DWORD, (const BYTE*)&one, sizeof(one)); }
                    else RegSetValueExW(bk, vn, 0, REG_BINARY, (const BYTE*)prev, prevN);
                }
            }
            if (changed) writeFile(paths[i], b.p, b.n);
            release(b);
        }
    }

    static constexpr const char* kMinttyKeys[19] = {
        "ForegroundColour", "BackgroundColour", "CursorColour", "Black", "Red", "Green", "Yellow", "Blue",
        "Magenta", "Cyan", "White", "BoldBlack", "BoldRed", "BoldGreen", "BoldYellow", "BoldBlue",
        "BoldMagenta", "BoldCyan", "BoldWhite"};
    static bool ieqKey(const char* line, DWORD len, const char* key) {  // line starts with `key=` (case-insensitive)
        DWORD i = 0;
        for (; key[i]; ++i) {
            if (i >= len) return false;
            char a = line[i], b = key[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return false;
        }
        return i < len && line[i] == '=';
    }

    static void applyMintty(const Ansi& a, HKEY bk) {
        wchar_t rc[MAX_PATH], bak[MAX_PATH + 24];
        if (!envPath(L"USERPROFILE", L"\\.minttyrc", rc)) return;
        sys::wcopy(sys::wcopy(bak, rc), L".archwindows-backup");
        Buf old{nullptr, 0};
        const bool exists = readFile(rc, old);
        if (!has(bk, L"mintty")) {  // first time: remember whether there was a file, and keep a copy
            const DWORD v = exists ? 1 : 0;
            if (exists) CopyFileW(rc, bak, FALSE);
            RegSetValueExW(bk, L"mintty", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        }
        char* out = (char*)alloc(old.n + 1024);
        if (!out) { release(old); return; }
        DWORD k = 0;
        for (DWORD i = 0; i < old.n;) {  // keep every line that is not one of our keys
            DWORD e = i;
            while (e < old.n && old.p[e] != '\n') ++e;
            bool ours = false;
            for (const char* key : kMinttyKeys) ours |= ieqKey(old.p + i, e - i, key);
            if (!ours) { for (DWORD x = i; x < e; ++x) out[k++] = old.p[x]; out[k++] = '\n'; }
            i = e + 1;
        }
        const COLORREF v[19] = {a.fg, a.bg, a.cursor, a.c[0], a.c[1], a.c[2], a.c[3], a.c[4], a.c[5], a.c[6], a.c[7],
                                a.c[8], a.c[9], a.c[10], a.c[11], a.c[12], a.c[13], a.c[14], a.c[15]};
        for (int i = 0; i < 19; ++i)
            k += (DWORD)wsprintfA(out + k, "%s=%d,%d,%d\n", kMinttyKeys[i], GetRValue(v[i]), GetGValue(v[i]), GetBValue(v[i]));
        writeFile(rc, out, k);
        HeapFree(GetProcessHeap(), 0, out);
        release(old);
    }

    static void applyConsole(const Ansi& a, HKEY bk) {
        HKEY c;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Console", 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &c, nullptr) != ERROR_SUCCESS) return;
        if (!has(bk, L"console")) {  // 16 values + a mask of which ones existed
            DWORD t[17] = {};
            for (int i = 0; i < 16; ++i) {
                wchar_t n[16];
                wsprintfW(n, L"ColorTable%02d", i);
                DWORD sz = sizeof(DWORD);
                if (RegQueryValueExW(c, n, nullptr, nullptr, (BYTE*)&t[i], &sz) == ERROR_SUCCESS) t[16] |= 1u << i;
            }
            RegSetValueExW(bk, L"console", 0, REG_BINARY, (const BYTE*)t, sizeof(t));
        }
        // The console has no separate background/foreground: it paints with table entries chosen by
        // ScreenColors, 0x07 by default (entry 7 on entry 0). So entry 0 is the theme's background
        // and entry 7 its text, also for light themes where ANSI black/white are the other way round.
        for (int i = 0; i < 16; ++i) {
            wchar_t n[16];
            wsprintfW(n, L"ColorTable%02d", i);
            const DWORD v = i == 0 ? a.bg : i == 7 ? a.fg : a.c[i];  // COLORREF is the console's 0x00BBGGRR
            RegSetValueExW(c, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        }
        RegCloseKey(c);
    }

    // ------------------------------------------------- live: open consoles --
    // Recolour every open classic console now, the way its own Properties dialog does: attach to
    // it for a moment and set its colour table. The entries its current text and background use
    // become the theme's text and background (PowerShell 5.1's console, for one, paints with
    // entries 5 and 6, not 7 and 0). Elevated consoles refuse the attach and are left alone.
    static BOOL WINAPI ignoreCtrl(DWORD) { return TRUE; }  // a Ctrl+C there while attached must not end us

    static void recolorConsole(DWORD pid, const Ansi& a) {
        if (!pid || !AttachConsole(pid)) return;
        HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            CONSOLE_SCREEN_BUFFER_INFOEX x{};
            x.cbSize = sizeof(x);
            if (GetConsoleScreenBufferInfoEx(out, &x)) {
                for (int i = 0; i < 16; ++i) x.ColorTable[i] = a.c[i];
                x.ColorTable[(x.wAttributes >> 4) & 15] = a.bg;
                x.ColorTable[x.wAttributes & 15] = a.fg;
                ++x.srWindow.Right;  // Get reports the window inclusive, Set takes it exclusive:
                ++x.srWindow.Bottom; // without this every call shrinks the window by a cell
                SetConsoleScreenBufferInfoEx(out, &x);
            }
            CloseHandle(out);
        }
        FreeConsole();
    }
    static BOOL CALLBACK enumConsole(HWND h, LPARAM l) {
        wchar_t c[32];
        GetClassNameW(h, c, 32);
        if (!sys::weq(c, L"ConsoleWindowClass")) return TRUE;  // also skips ConPTY (Terminal-hosted) consoles
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);  // for a console window: a process attached to it
        recolorConsole(pid, *(const Ansi*)l);
        return TRUE;
    }
    static void applyLiveConsoles(const Ansi& a) {
        SetConsoleCtrlHandler(ignoreCtrl, TRUE);
        EnumWindows(enumConsole, (LPARAM)&a);
    }

    // ------------------------------------------------------ live: Git Bash --
    // mintty only reads .minttyrc when a window opens, and an open window can only be recoloured
    // from inside its session. So ~/.archwindows-term holds the theme as mintty escape codes
    // (line 1: a stamp, line 2: the codes), and a marked block in ~/.bashrc prints them at the next
    // prompt whenever the stamp changed. Bash built-ins only (no fork per prompt), and mintty only
    // (TERM_PROGRAM), so Git Bash inside Windows Terminal keeps Terminal's scheme.
    static constexpr const char* kBegin = "# >>> archwindows terminal colours";
    static constexpr const char* kEnd = "# <<< archwindows terminal colours <<<\n";
    static constexpr const char* kBlock =
        "# >>> archwindows terminal colours (managed by archwindows; removed when the option is turned off) >>>\n"
        "__aw_term() {\n"
        "  local f=\"$HOME/.archwindows-term\" v s\n"
        "  [ \"$TERM_PROGRAM\" = mintty ] && [ -r \"$f\" ] || return 0\n"
        "  { IFS= read -r v; IFS= read -r s; } < \"$f\"\n"
        "  [ \"$v\" = \"$__aw_v\" ] && return 0\n"
        "  __aw_v=$v; printf '%s' \"$s\"\n"
        "}\n"
        "case \";$PROMPT_COMMAND;\" in *\";__aw_term;\"*) ;; *) PROMPT_COMMAND=\"__aw_term${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;; esac\n"
        "# <<< archwindows terminal colours <<<\n";
    // What Git for Windows itself writes when it finds a .bashrc but no profile (and warns in red).
    static constexpr const char* kGitProfile =
        "# generated by Git for Windows\ntest -f ~/.profile && . ~/.profile\ntest -f ~/.bashrc && . ~/.bashrc\n";

    static int findBytes(const Buf& b, const char* lit) {
        const DWORD n = slen(lit);
        for (DWORD i = 0; i + n <= b.n; ++i) {
            DWORD k = 0;
            while (k < n && b.p[i + k] == lit[k]) ++k;
            if (k == n) return (int)i;
        }
        return -1;
    }
    static bool home(const wchar_t* tail, wchar_t* out) { return envPath(L"USERPROFILE", tail, out); }
    static bool exists(const wchar_t* tail) {
        wchar_t p[MAX_PATH];
        return home(tail, p) && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    }

    static void applyBash(const Ansi& a, HKEY bk) {
        wchar_t f[MAX_PATH];
        if (!home(L"\\.archwindows-term", f)) return;
        char seq[1024];
        char* p = seq + wsprintfA(seq, "%lu\n", GetTickCount());
        auto osc = [&](const char* pre, COLORREF c) {
            p += wsprintfA(p, "\x1b]%s;rgb:%02x/%02x/%02x\x07", pre, GetRValue(c), GetGValue(c), GetBValue(c));
        };
        for (int i = 0; i < 16; ++i) { char pre[8]; wsprintfA(pre, "4;%d", i); osc(pre, a.c[i]); }
        osc("10", a.fg);
        osc("11", a.bg);
        osc("12", a.cursor);
        *p++ = '\n';
        writeFile(f, seq, (DWORD)(p - seq));

        wchar_t rc[MAX_PATH];
        if (!home(L"\\.bashrc", rc)) return;
        Buf b{nullptr, 0};
        readFile(rc, b);
        if (findBytes(b, kBegin) < 0) {
            const DWORD bn = slen(kBlock);
            const bool nl = b.n && b.p[b.n - 1] != '\n';
            char* out = (char*)alloc(b.n + bn + 2);
            if (out) {
                DWORD k = 0;
                for (DWORD i = 0; i < b.n; ++i) out[k++] = b.p[i];
                if (nl) out[k++] = '\n';
                for (DWORD i = 0; i < bn; ++i) out[k++] = kBlock[i];
                writeFile(rc, out, k);
                HeapFree(GetProcessHeap(), 0, out);
            }
        }
        release(b);
        if (!exists(L"\\.bash_profile") && !exists(L"\\.bash_login") && !exists(L"\\.profile")) {
            wchar_t bp[MAX_PATH];
            if (home(L"\\.bash_profile", bp) && writeFile(bp, kGitProfile, slen(kGitProfile))) {
                const DWORD one = 1;
                RegSetValueExW(bk, L"bashProfile", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
            }
        }
    }

    static void restoreBash(HKEY bk) {
        wchar_t f[MAX_PATH];
        if (home(L"\\.archwindows-term", f)) DeleteFileW(f);
        wchar_t rc[MAX_PATH];
        Buf b;
        if (home(L"\\.bashrc", rc) && readFile(rc, b)) {
            const int s = findBytes(b, kBegin), e = findBytes(b, kEnd);
            if (s >= 0 && e > s && splice(b, (DWORD)s, (DWORD)(e - s) + slen(kEnd), "", 0)) {
                bool blank = true;
                for (DWORD i = 0; i < b.n; ++i) blank &= b.p[i] == ' ' || b.p[i] == '\n' || b.p[i] == '\r' || b.p[i] == '\t';
                if (blank) DeleteFileW(rc); else writeFile(rc, b.p, b.n);
            }
            release(b);
        }
        wchar_t bp[MAX_PATH];
        if (has(bk, L"bashProfile") && home(L"\\.bash_profile", bp) && readFile(bp, b)) {
            const DWORD n = slen(kGitProfile);
            bool same = b.n == n;
            for (DWORD i = 0; same && i < n; ++i) same = b.p[i] == kGitProfile[i];
            release(b);
            if (same && !exists(L"\\.bashrc")) DeleteFileW(bp);  // only ours, and nothing left for it to load
        }
    }

    static void apply(const Palette& p) {
        HKEY bk = backupKey(true);
        if (!bk) return;
        const Ansi a = ansi(p);
        applyTerminal(a, p, bk);
        applyMintty(a, bk);
        applyBash(a, bk);
        applyConsole(a, bk);
        applyLiveConsoles(a);
        RegCloseKey(bk);
    }

    // ---------------------------------------------------------------- restore --
    static void restore() {
        HKEY bk = backupKey(false);
        if (!bk) return;
        wchar_t paths[3][MAX_PATH];
        const int np = wtPaths(paths);
        for (int i = 0; i < np; ++i) {
            wchar_t vn[16];
            wsprintfW(vn, L"wt%d", i);
            char prev[512];
            DWORD type = 0, sz = sizeof(prev);
            const bool hasScheme = RegQueryValueExW(bk, vn, nullptr, &type, (BYTE*)prev, &sz) == ERROR_SUCCESS;
            char prevT[512];
            DWORD typeT = 0, szT = sizeof(prevT);
            wchar_t vt[16];
            wsprintfW(vt, L"wtt%d", i);
            const bool hasTheme = RegQueryValueExW(bk, vt, nullptr, &typeT, (BYTE*)prevT, &szT) == ERROR_SUCCESS;
            if (!hasScheme && !hasTheme) continue;
            Buf b;
            if (!readFile(paths[i], b)) continue;
            bool changed = hasScheme && restoreDefaults(b, prev, type == REG_BINARY ? sz : 0, type == REG_DWORD);
            changed |= hasTheme && restoreTheme(b, prevT, typeT == REG_BINARY ? szT : 0, typeT == REG_DWORD);
            if (changed) writeFile(paths[i], b.p, b.n);
            release(b);
        }
        restoreBash(bk);
        DWORD had = 0, sz = sizeof(had);
        if (RegQueryValueExW(bk, L"mintty", nullptr, nullptr, (BYTE*)&had, &sz) == ERROR_SUCCESS) {
            wchar_t rc[MAX_PATH], bak[MAX_PATH + 24];
            if (envPath(L"USERPROFILE", L"\\.minttyrc", rc)) {
                sys::wcopy(sys::wcopy(bak, rc), L".archwindows-backup");
                if (had) MoveFileExW(bak, rc, MOVEFILE_REPLACE_EXISTING);
                else DeleteFileW(rc);
            }
        }
        DWORD t[17];
        sz = sizeof(t);
        HKEY c;
        if (RegQueryValueExW(bk, L"console", nullptr, nullptr, (BYTE*)t, &sz) == ERROR_SUCCESS && sz == sizeof(t) &&
            RegOpenKeyExW(HKEY_CURRENT_USER, L"Console", 0, KEY_SET_VALUE, &c) == ERROR_SUCCESS) {
            for (int i = 0; i < 16; ++i) {
                wchar_t n[16];
                wsprintfW(n, L"ColorTable%02d", i);
                if (t[16] & (1u << i)) RegSetValueExW(c, n, 0, REG_DWORD, (const BYTE*)&t[i], sizeof(DWORD));
                else RegDeleteValueW(c, n);
            }
            RegCloseKey(c);
        }
        RegCloseKey(bk);
        RegDeleteKeyW(HKEY_CURRENT_USER, kBackup);
    }
};
