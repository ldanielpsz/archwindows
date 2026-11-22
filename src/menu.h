// The Win+Space menu, laid out like Omarchy's: a "Go" list of categories, each a titled submenu.
//
// Apps page: every Start Menu shortcut (.lnk/.url/.appref-ms) and every packaged (Store) app, sorted
// and de-duplicated. They are collected lazily by a background thread started when the menu opens,
// so drawing never waits for the disk or COM; the rows are merged in when the thread is done.
// Options page: checkboxes for the toggles in options.h; changes apply immediately.
// Everything transient (window, item list, the app scan) lives in one VirtualAlloc'd arena
// that is freed when the menu closes, so the idle process pays nothing for it.
//
// A LaunchPolicy provides: static bool run(const wchar_t* path).
#pragma once
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "options.h"
#include "tiler.h"  // tileutil::forceForeground, contains
#include "winstyle.h"  // winutil::isOwn, isShellWindow
#include "session.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>

struct ShellLaunch {
    static bool run(const wchar_t* path, const wchar_t* params = nullptr) {
        return (INT_PTR)ShellExecuteW(nullptr, L"open", path, params, nullptr, SW_SHOWNORMAL) > 32;
    }
};

// The user's default terminal and browser, for the Super+Enter / Super+Shift+B launchers.
template <class Launch>
struct DefaultApps {
    // Windows Terminal with its default profile (wt.exe is its app alias, on PATH); the classic
    // console if Terminal is not installed.
    static bool terminal() { return Launch::run(L"wt.exe") || Launch::run(L"cmd.exe"); }

    // The program registered for https links, started with no page, which opens a new window in
    // every common browser. If only a handler without an executable is registered (some packaged
    // browsers), open the browser's start page through the protocol instead.
    static bool browser() {
        wchar_t exe[MAX_PATH];
        DWORD n = MAX_PATH;
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, L"https", L"open", exe, &n)) &&
            GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && Launch::run(exe))
            return true;
        return Launch::run(L"https://");
    }
};

template <class Bus, class R, class Launch>
struct Menu {
    static constexpr int kMaxItems = 1536, kRows = 11;
    static constexpr size_t kArena = 1 << 20;
    // Omarchy-style menu tree: "Go" (MAIN) lists the categories, each opens a titled submenu.
    enum Page : u8 { MAIN, APPS, LEARN, KEYS, TRIGGER, CAPTURE, TOGGLE, STYLE, SETUP, INSTALL, REMOVE, UPDATE,
                     SYSTEM, kPages };
    enum : int { ID_NOOP = 1, ID_THEME, ID_QUIT, ID_RESTORE, ID_SESSION0 = 10, ID_PAGE0 = 20,
                 ID_OPT0 = 100, ID_APP = 1000 };  // ID_APP and above: launch arg (+ params)
    // folder: Start Menu folder an app lives in ("Inno Setup 6", "Windows Tools"), shown in its path.
    struct Item { const wchar_t* label; const wchar_t* arg; int id; const wchar_t* icon; const wchar_t* params; u8 page; const wchar_t* folder; };
    static constexpr const wchar_t* kTitle[kPages] = {L"Go", L"Apps", L"Learn", L"Keybindings", L"Trigger", L"Capture",
                                                      L"Toggle", L"Style", L"Setup", L"Install", L"Remove", L"Update",
                                                      L"System"};
    static Page parent(Page p) { return p == KEYS ? LEARN : (p == CAPTURE || p == TOGGLE) ? TRIGGER : MAIN; }
    // Every page takes typed text, and a query searches ALL pages (not just the current one).
    // Pages with checkbox rows also toggle on Space while the query is empty.
    static bool hasToggles() { return page == SETUP || page == TOGGLE; }

    static inline HWND hwnd;
    static inline bool registered;
    static inline Page page, building;
    static inline wchar_t query[40];
    static inline int qlen, sel, top, dpi = 96;
    static inline char* arena;
    static inline size_t used, base;
    static inline Item* items;
    static inline int* view;
    static inline HBITMAP* icons;  // per item: the app's own (colour) icon, loaded when first shown
    static inline u8* iconTried;
    static inline int nItems, nView;
    static inline DWORD openedAt;

    // ------------------------------------------------------------- storage --
    static void* take(size_t n) {
        n = (n + 7) & ~(size_t)7;
        if (!arena || used + n > kArena) return nullptr;
        void* p = arena + used;
        used += n;
        return p;
    }
    static const wchar_t* dup(const wchar_t* s, int n) {
        wchar_t* d = (wchar_t*)take((size_t)(n + 1) * sizeof(wchar_t));
        if (!d) return nullptr;
        for (int i = 0; i < n; ++i) d[i] = s[i];
        d[n] = 0;
        return d;
    }
    static void add(const wchar_t* label, const wchar_t* arg, int id, const wchar_t* icon = nullptr,
                    const wchar_t* params = nullptr) {
        if (nItems >= kMaxItems || !label) return;
        items[nItems++] = {label, arg, id, icon, params, (u8)building, nullptr};
    }
    static void go(const wchar_t* icon, const wchar_t* label, Page p) { add(label, nullptr, ID_PAGE0 + (int)p, icon); }
    static void run(const wchar_t* icon, const wchar_t* label, const wchar_t* file, const wchar_t* params = nullptr) {
        add(label, file, ID_APP, icon, params);
    }
    static void opt(const wchar_t* label, int o) { add(label, nullptr, ID_OPT0 + o); }
    static void act(const wchar_t* icon, const wchar_t* label, int id) { add(label, nullptr, id, icon); }

    // ------------------------------------------------------------ app scan --
    // Runs on a worker thread into its own VirtualAlloc'd block (header + items + string pool), so it
    // shares nothing with the UI arena. The UI thread adopts the finished block and frees it on close.
    static constexpr int kMaxApps = 1024;
    static constexpr size_t kScanBytes = 1 << 20;
    static constexpr UINT WM_APPS = WM_APP + 20;
    struct Scan { size_t used; int n; Item items[kMaxApps]; };

    static inline Scan* volatile ready;  // finished scan waiting for the UI thread
    static inline Scan* mine;            // scan whose strings the item list points into
    static inline volatile LONG gen;     // bumped on open and close: a stale worker discards its result
    static inline bool scanning;

    static const wchar_t* sdup(Scan* s, const wchar_t* t, int n) {
        const size_t bytes = ((size_t)(n + 1) * sizeof(wchar_t) + 7) & ~(size_t)7;
        if (s->used + bytes > kScanBytes) return nullptr;
        wchar_t* d = (wchar_t*)((char*)s + s->used);
        s->used += bytes;
        for (int i = 0; i < n; ++i) d[i] = t[i];
        d[n] = 0;
        return d;
    }
    static void sadd(Scan* s, const wchar_t* label, const wchar_t* arg, const wchar_t* folder = nullptr) {
        if (s->n >= kMaxApps || !label || !arg) return;
        s->items[s->n++] = {label, arg, ID_APP, nullptr, nullptr, (u8)APPS, folder};
    }

    static bool endsWithI(const wchar_t* name, int n, const wchar_t* ext) {
        const int e = sys::wlen(ext);
        if (n <= e) return false;
        for (int i = 0; i < e; ++i) {
            wchar_t a = name[n - e + i], b = ext[i];
            if (a >= L'A' && a <= L'Z') a += 32;
            if (a != b) return false;
        }
        return true;
    }
    static bool startsWithI(const wchar_t* s, const wchar_t* prefix) {
        for (; *prefix; ++s, ++prefix) {
            wchar_t a = *s;
            if (a >= L'A' && a <= L'Z') a += 32;
            if (a != *prefix) return false;
        }
        return true;
    }

    // rel: the folder path below Start Menu\Programs ("" at the top), joined with " \x203A ".
    static void scanDir(Scan* s, const wchar_t* dir, int depth, const wchar_t* rel = L"") {
        wchar_t pat[MAX_PATH];
        wsprintfW(pat, L"%s\\*", dir);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.') continue;
            wchar_t full[MAX_PATH];
            wsprintfW(full, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth < 6) {
                    wchar_t sub[160];
                    if (rel[0]) wsprintfW(sub, L"%.70s \x203A %.70s", rel, fd.cFileName);
                    else lstrcpynW(sub, fd.cFileName, 160);
                    const wchar_t* keep = sdup(s, sub, sys::wlen(sub));
                    scanDir(s, full, depth + 1, keep ? keep : L"");
                }
                continue;
            }
            const int n = sys::wlen(fd.cFileName);
            int cut = 0;
            if (endsWithI(fd.cFileName, n, L".lnk") || endsWithI(fd.cFileName, n, L".url")) cut = 4;
            else if (endsWithI(fd.cFileName, n, L".appref-ms")) cut = 10;
            if (!cut) continue;
            if (startsWithI(fd.cFileName, L"uninstall") || startsWithI(fd.cFileName, L"unins0")) continue;  // uninstallers, not apps named "... Uninstall ..."
            sadd(s, sdup(s, fd.cFileName, n - cut), sdup(s, full, sys::wlen(full)), rel[0] ? rel : nullptr);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    // Packaged (Store) apps: enumerate the "All apps" shell folder. Only entries whose AppUserModelID
    // has the Package!App form are taken; desktop apps come from their shortcuts above.
    static void scanPackaged(Scan* s) {
        static const GUID kAppsFolder = {0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
        static const GUID kEnumItems = {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHGetKnownFolderItem(kAppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) {
            IEnumShellItems* en = nullptr;
            if (SUCCEEDED(folder->BindToHandler(nullptr, kEnumItems, IID_PPV_ARGS(&en)))) {
                IShellItem* it = nullptr;
                ULONG got = 0;
                while (en->Next(1, &it, &got) == S_OK && got == 1) {
                    LPWSTR id = nullptr, name = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && id &&
                        SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                        bool bang = false, slash = false;
                        for (const wchar_t* c = id; *c; ++c) { bang |= *c == L'!'; slash |= *c == L'\\'; }
                        const int ln = sys::wlen(name), li = sys::wlen(id);
                        if (bang && !slash && ln && li < 220) {
                            wchar_t arg[260];
                            sys::wcopy(sys::wcopy(arg, L"shell:AppsFolder\\"), id);
                            sadd(s, sdup(s, name, ln), sdup(s, arg, sys::wlen(arg)));
                        }
                    }
                    if (id) CoTaskMemFree(id);
                    if (name) CoTaskMemFree(name);
                    it->Release();
                }
                en->Release();
            }
            folder->Release();
        }
        CoUninitialize();
    }

    static int cmp(const Item& a, const Item& b) { return CompareStringOrdinal(a.label, -1, b.label, -1, TRUE) - 2; }

    static void sortApps(Scan* s) {  // shell sort: no recursion, no CRT
        Item* it = s->items;
        const int n = s->n;
        for (int gap = n / 2; gap > 0; gap /= 2)
            for (int i = gap; i < n; ++i) {
                Item t = it[i];
                int j = i;
                for (; j >= gap && cmp(it[j - gap], t) > 0; j -= gap) it[j] = it[j - gap];
                it[j] = t;
            }
        int w = 0;  // drop exact duplicates (same app in user and system menus, or as shortcut and Store entry)
        for (int i = 0; i < n; ++i)
            if (w == 0 || cmp(it[w - 1], it[i]) != 0) it[w++] = it[i];
        s->n = w;
    }

    static void freeScan(Scan* s) { if (s) VirtualFree(s, 0, MEM_RELEASE); }

    static DWORD WINAPI scanThread(LPVOID arg) {
        const LONG g = (LONG)(INT_PTR)arg;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Scan* s = (Scan*)VirtualAlloc(nullptr, kScanBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s) return 0;
        s->used = (sizeof(Scan) + 7) & ~(size_t)7;
        wchar_t dir[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH)) {
            sys::wcopy(dir + sys::wlen(dir), L"\\Microsoft\\Windows\\Start Menu\\Programs");
            scanDir(s, dir, 0);
        }
        if (GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) {
            sys::wcopy(dir + sys::wlen(dir), L"\\Microsoft\\Windows\\Start Menu\\Programs");
            scanDir(s, dir, 0);
        }
        if (gen == g) scanPackaged(s);
        sortApps(s);
        if (gen != g) { freeScan(s); return 0; }  // menu closed meanwhile
        InterlockedExchangePointer((PVOID volatile*)&ready, s);
        if (gen != g) {  // closed between the check and the hand-off: whoever swaps it out frees it
            freeScan((Scan*)InterlockedExchangePointer((PVOID volatile*)&ready, nullptr));
            return 0;
        }
        if (HWND w = hwnd) PostMessageW(w, WM_APPS, 0, 0);
        return 0;
    }

    static void startScan() {
        scanning = true;
        HANDLE t = CreateThread(nullptr, 0, scanThread, (LPVOID)(INT_PTR)gen, 0, nullptr);
        if (t) CloseHandle(t); else scanning = false;
    }

    // UI thread: adopt a finished scan and append its rows.
    static void adoptScan() {
        Scan* s = (Scan*)InterlockedExchangePointer((PVOID volatile*)&ready, nullptr);
        if (!s) return;
        scanning = false;
        if (!items) { freeScan(s); return; }
        freeScan(mine);
        mine = s;
        for (int i = 0; i < s->n && nItems < kMaxItems; ++i) items[nItems++] = s->items[i];
        const int keepSel = sel, keepTop = top;
        refilter();
        if (keepSel < nView) { sel = keepSel; top = keepTop; }
    }

    static constexpr const wchar_t* kPs = L"powershell.exe";

    static void buildPage(Page p) {
        building = p;
        switch (p) {
            case MAIN:
                go(L"\xE71D", L"Apps", APPS);
                go(L"\xE897", L"Learn", LEARN);
                go(L"\xE945", L"Trigger", TRIGGER);
                go(L"\xE790", L"Style", STYLE);
                go(L"\xE713", L"Setup", SETUP);
                go(L"\xE896", L"Install", INSTALL);
                go(L"\xE74D", L"Remove", REMOVE);
                go(L"\xE72C", L"Update", UPDATE);
                run(L"\xE946", L"About", L"ms-settings:about");
                go(L"\xE7E8", L"System", SYSTEM);
                break;
            case APPS: break;  // filled in by the background scan (adoptScan)
            case LEARN:
                go(L"\xE765", L"Keybindings", KEYS);
                run(L"\xE774", L"Omarchy", L"https://learn.omacom.io/2/the-omarchy-manual");
                run(L"\xE774", L"Hyprland", L"https://wiki.hypr.land/");
                run(L"\xE774", L"Arch", L"https://wiki.archlinux.org/title/Main_page");
                run(L"\xE774", L"Neovim", L"https://www.lazyvim.org/keymaps");
                run(L"\xE774", L"Bash", L"https://devhints.io/bash");
                break;
            case KEYS: {  // tiling chords only act while Tiling is on (Setup)
                static constexpr const wchar_t* k[] = {
                    L"Super + Space  \x2014  this menu",
                    L"Super + Enter  \x2014  terminal (Windows Terminal, default profile)",
                    L"Super + Shift + B  \x2014  browser (your default)",
                    L"Super + Ctrl + T  \x2014  theme picker",
                    L"Super + Q / W  \x2014  close window",
                    L"Super + J  \x2014  toggle split",
                    L"Super + T  \x2014  toggle floating",
                    L"Super + F  \x2014  fullscreen",
                    L"Super + Alt + F  \x2014  full width",
                    L"Super + M  \x2014  minimize",
                    L"Super + arrows  \x2014  move focus",
                    L"Super + Shift + arrows  \x2014  swap windows",
                    L"Super + 1 .. 0  \x2014  workspace",
                    L"Super + Shift + 1 .. 0  \x2014  move to workspace",
                    L"Super + Tab / Shift + Tab  \x2014  next / previous workspace",
                    L"Super + - / =  \x2014  resize width (Shift: height)"};
                for (const wchar_t* t : k) add(t, nullptr, ID_NOOP);
                break;
            }
            case TRIGGER:
                go(L"\xE722", L"Capture", CAPTURE);
                go(L"\xE7E7", L"Toggle", TOGGLE);
                break;
            case CAPTURE:
                run(L"\xE722", L"Screenshot", L"ms-screenclip:");
                run(L"\xE714", L"Screen recording", L"snippingtool.exe");
                break;
            case TOGGLE:
                run(L"\xE706", L"Night light", L"ms-settings:nightlight");
                opt(L"Tiling (dwindle layout, Super key chords)", Options::O_TILING);
                opt(L"Gaps between windows", Options::O_GAPS);
                break;
            case STYLE:
                act(L"\xE790", L"Theme", ID_THEME);
                run(L"\xE8D2", L"Font", L"ms-settings:fonts");
                act(L"\xE8B9", L"Background", ID_THEME);
                run(L"\xE7F4", L"Screensaver", L"rundll32.exe", L"shell32.dll,Control_RunDLL desk.cpl,,1");
                break;
            case SETUP:  // the shell's own options
                opt(L"Tiling (dwindle layout, Super key chords)", Options::O_TILING);
                opt(L"Hide title bars", Options::O_HIDE_TITLEBARS);
                opt(L"Theme window colours", Options::O_THEME_WINDOWS);
                opt(L"Gaps between windows", Options::O_GAPS);
                opt(L"Limit to 4 tiled windows per monitor when tiling starts", Options::O_LIMIT);
                opt(L"Workspaces switch on all monitors together", Options::O_SYNCWS);
                opt(L"Theme drives Windows dark/light and accent", Options::O_SYSTHEME);
                opt(L"Theme drives terminal colours", Options::O_TERMTHEME);
                opt(L"Theme drives Chrome / Brave colour", Options::O_BROWSERTHEME);
                opt(L"Run at startup", Options::O_STARTUP);
                break;
            case INSTALL:
                run(L"\xE896", L"Package", kPs,
                    L"-NoExit -Command \"$n = Read-Host 'Search packages'; winget search $n; "
                    L"$id = Read-Host 'Package id to install (empty = cancel)'; if ($id) { winget install --id $id }\"");
                run(L"\xE719", L"Store", L"ms-windows-store:");
                run(L"\xE713", L"Windows feature", L"optionalfeatures.exe");
                break;
            case REMOVE:
                run(L"\xE74D", L"Package", kPs,
                    L"-NoExit -Command \"winget list; $id = Read-Host 'Package id to remove (empty = cancel)'; "
                    L"if ($id) { winget uninstall --id $id }\"");
                run(L"\xE71D", L"Installed apps", L"ms-settings:appsfeatures");
                run(L"\xE713", L"Windows feature", L"optionalfeatures.exe");
                break;
            case UPDATE:
                run(L"\xE777", L"Windows", L"ms-settings:windowsupdate");
                run(L"\xE896", L"Packages", kPs, L"-NoExit -Command \"winget upgrade --all\"");
                run(L"\xE719", L"Store apps", L"ms-windows-store://downloadsandupdates");
                run(L"\xE121", L"Timezone", L"ms-settings:dateandtime");
                break;
            case SYSTEM:
                for (int k = 0; k < Session::kCount; ++k) act(Session::glyph[k], Session::label[k], ID_SESSION0 + k);
                act(L"\xE711", L"Quit shell", ID_QUIT);
                act(L"\xE72C", L"Close and reload Windows default shell", ID_RESTORE);
                break;
            default: break;
        }
    }

    // Every page's rows live in one list (tagged with their page) so a query can search them all.
    static void buildAll() {
        nItems = 0;
        for (int p = 0; p < (int)kPages; ++p) buildPage((Page)p);
    }

    // ----------------------------------------------------------- filtering --
    static bool matches(const wchar_t* s) {
        if (!qlen) return true;
        for (; *s; ++s) {
            int i = 0;
            while (i < qlen && s[i]) {
                wchar_t a = s[i], b = query[i];
                if (a >= L'A' && a <= L'Z') a += 32;
                if (b >= L'A' && b <= L'Z') b += 32;
                if (a != b) break;
                ++i;
            }
            if (i == qlen) return true;
        }
        return false;
    }
    static bool prefix(const wchar_t* s) {
        for (int i = 0; i < qlen; ++i) {
            wchar_t a = s[i], b = query[i];
            if (!a) return false;
            if (a >= L'A' && a <= L'Z') a += 32;
            if (b >= L'A' && b <= L'Z') b += 32;
            if (a != b) return false;
        }
        return true;
    }
    // A query lists matches from every page; labels that start with it come first (the best match
    // sits on top, as in Omarchy's menu).
    static void refilter() {
        nView = 0;
        for (int pass = 0; pass < (qlen ? 2 : 1); ++pass)
            for (int i = 0; i < nItems; ++i) {
                if (!qlen) { if (items[i].page == page) view[nView++] = i; continue; }
                if (!matches(items[i].label) || prefix(items[i].label) != (pass == 0)) continue;
                bool dup = false;  // the same setting can live on two pages (e.g. Tiling): list it once
                for (int v = 0; v < nView && !dup; ++v)
                    dup = items[view[v]].id == items[i].id && items[view[v]].id != ID_APP && items[view[v]].id != ID_NOOP;
                if (!dup) view[nView++] = i;
            }
        sel = top = 0;
        fit();
    }

    static void showPage(Page p) {
        page = p;
        qlen = 0;
        query[0] = 0;
        refilter();
    }

    // ------------------------------------------------------------ lifecycle --
    // Geometry. Search results and the Apps page are two lines (label + where it lives, Omarchy's
    // path detail); other pages are one line.
    static bool twoLine() { return qlen || page == APPS; }
    static int width() { return sys::scale(380, dpi); }
    static int rowH() { return sys::scale(twoLine() ? 48 : 40, dpi); }
    static int inputH() { return sys::scale(46, dpi); }
    static int pad() { return sys::scale(14, dpi); }
    static int sepH() { return qlen && nView > 1 ? sys::scale(13, dpi) : 0; }  // under the best match
    static int listTop() { return pad() + inputH() + sys::scale(6, dpi); }
    static int rowY(int v) { return listTop() + (v - top) * rowH() + (v > 0 && top == 0 ? sepH() : 0); }
    static int visibleRows() { return sys::imax(1, sys::imin(nView, kRows)); }
    static int height() { return listTop() + visibleRows() * rowH() + (top == 0 ? sepH() : 0) + pad(); }
    static void fit() {  // the window follows its content, like Omarchy's
        if (!hwnd) return;
        RECT rc;
        GetWindowRect(hwnd, &rc);
        if (rc.bottom - rc.top != height())
            SetWindowPos(hwnd, nullptr, 0, 0, width(), height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Where a row lives, as Omarchy shows it: its parents joined with " > " ("Setup", "Trigger > Capture",
    // "Apps > Inno Setup 6"); empty for rows on the top-level "Go" page.
    static void crumb(const Item& it, wchar_t* o) {
        const Page p = (Page)it.page;
        o[0] = 0;
        if (p == MAIN) return;
        if (parent(p) != MAIN) wsprintfW(o, L"%s \x203A %s", kTitle[parent(p)], kTitle[p]);
        else sys::wcopy(o, kTitle[p]);
        if (it.folder) {
            const int n = sys::wlen(o);
            wsprintfW(o + n, L" \x203A %.80s", it.folder);
        }
    }
    // The app's own icon (colour, from its shortcut or package), loaded the first time its row shows.
    static HBITMAP appIcon(int i) {
        if (!icons || iconTried[i]) return icons ? icons[i] : nullptr;
        iconTried[i] = 1;
        const Item& it = items[i];
        if (it.id != ID_APP || !it.arg || it.page != APPS) return nullptr;  // apps only; other rows keep glyphs
        IShellItemImageFactory* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(it.arg, nullptr, IID_PPV_ARGS(&f)))) {
            const int px = sys::scale(32, dpi);
            HBITMAP b = nullptr;
            if (SUCCEEDED(f->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &b))) icons[i] = b;
            f->Release();
        }
        return icons[i];
    }

    // The screen the user is working on: the focused window's monitor; with nothing focused (or
    // the desktop / our own windows focused), the monitor under the cursor.
    static inline HMONITOR mon;
    static HMONITOR focusedMonitor() {
        HWND fg = GetForegroundWindow();
        fg = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
        if (fg && IsWindowVisible(fg) && !IsIconic(fg) && !winutil::isOwn(fg) && !winutil::isShellWindow(fg))
            return MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        POINT pt{};
        GetCursorPos(&pt);
        return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }

    // at: the monitor to open on (a bar's logo); null = the screen the user is working on.
    static void open(Page p, int curDpi, HMONITOR at = nullptr) {
        if (hwnd) { SetForegroundWindow(hwnd); return; }
        dpi = curDpi;
        mon = at ? at : focusedMonitor();  // before our window takes the focus
        InterlockedIncrement(&gen);
        used = 0;
        arena = (char*)VirtualAlloc(nullptr, kArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!arena) return;
        items = (Item*)take(sizeof(Item) * kMaxItems);
        view = (int*)take(sizeof(int) * kMaxItems);
        icons = (HBITMAP*)take(sizeof(HBITMAP) * kMaxItems);  // the arena is zeroed by VirtualAlloc
        iconTried = (u8*)take(kMaxItems);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // shell icons; the UI thread keeps COM
        buildAll();
        showPage(p);
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AWMenu";
            RegisterClassW(&wc);
            registered = true;
        }
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
        const RECT wa = mi.rcWork;
        const int w = width(), h = height();
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AWMenu", L"", WS_POPUP,
                               wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top) / 6, w, h,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        openedAt = GetTickCount();
        ShowWindow(hwnd, SW_SHOW);
        tileutil::forceForeground(hwnd);
        startScan();  // after the window is up: the first paint never waits for it
    }

    static void close() {
        if (!hwnd) return;
        HWND h = hwnd;
        hwnd = nullptr;
        DestroyWindow(h);
        InterlockedIncrement(&gen);  // a scan still running discards its result
        freeScan((Scan*)InterlockedExchangePointer((PVOID volatile*)&ready, nullptr));
        freeScan(mine);
        mine = nullptr;
        scanning = false;
        for (int i = 0; icons && i < kMaxItems; ++i) if (icons[i]) DeleteObject(icons[i]);
        icons = nullptr;
        iconTried = nullptr;
        if (arena) { VirtualFree(arena, 0, MEM_RELEASE); arena = nullptr; }
        items = nullptr;
        view = nullptr;
        nItems = nView = 0;
        Bus::scheduleTrim();
    }

    static void activate() {
        if (sel < 0 || sel >= nView) return;
        const Item it = items[view[sel]];
        if (it.id >= ID_OPT0 && it.id < ID_OPT0 + (int)Options::O_COUNT) {
            const int o = it.id - ID_OPT0;
            Options::set(o, !Options::get(o));
            Bus::optionsChanged();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (it.id == ID_NOOP) return;
        if (it.id >= ID_PAGE0 && it.id < ID_PAGE0 + (int)kPages) {
            showPage((Page)(it.id - ID_PAGE0));
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        wchar_t path[MAX_PATH], params[512];
        path[0] = params[0] = 0;
        if (it.arg) sys::wcopy(path, it.arg);  // arena dies with the window
        if (it.params) sys::wcopy(params, it.params);
        const int id = it.id;
        const HMONITOR target = mon;
        close();
        if (id >= ID_APP) Bus::launchOn(target);  // the app's first window opens on the menu's screen
        if (id == ID_THEME) Bus::openPicker(target);
        else if (id == ID_QUIT) Bus::quit();
        else if (id == ID_RESTORE) Bus::restoreDefault();
        else if (id >= ID_SESSION0 && id < (int)ID_SESSION0 + (int)Session::kCount) Session::run(id - ID_SESSION0);
        else if (id >= ID_APP) Launch::run(path, params[0] ? params : nullptr);
    }

    static void move(int d) {
        if (!nView) return;
        sel += d;
        if (sel < 0) sel = 0;
        if (sel >= nView) sel = nView - 1;
        if (sel < top) top = sel;
        if (sel >= top + kRows) top = sel - kRows + 1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // --------------------------------------------------------------- paint --
    // Omarchy's menu: the prompt ("Go\u2026", "Capture\u2026") is the input line, rows carry a glyph (or
    // the app's own colour icon) and, in search results and Apps, the path they live under; submenus
    // end in a chevron. Lock / sleep / sign out / restart / shut down are rows under System.

    static void paint(HDC dc, int cw, int ch, const Palette& p) {
        R r(dc, cw, ch);
        r.fill(0, 0, cw, ch, p.bg());
        const int pd = pad(), rh = rowH(), ih = inputH(), s4 = sys::scale(4, dpi);
        const COLORREF bright = p.fg();  // Omarchy menu text is the foreground

        // Input line: the page's prompt until something is typed.
        const int ix = pd + sys::scale(4, dpi);
        if (qlen) {
            r.text(ix, pd, ih, query, F_ROW, bright, qlen);
            const int cx = ix + r.measure(query, F_ROW, qlen);
            r.fill(cx + 1, pd + (ih - sys::scale(18, dpi)) / 2, sys::scale(2, dpi), sys::scale(18, dpi), p.accent());
        } else {
            wchar_t prompt[32];
            wsprintfW(prompt, L"%s\x2026", kTitle[page]);
            r.text(ix, pd, ih, prompt, F_ROW, p.dim());
        }

        // Rows.
        const int last = sys::imin(nView, top + kRows);
        for (int v = top; v < last; ++v) {
            const int i = view[v];
            const Item& it = items[i];
            const int y = rowY(v);
            const bool on = v == sel;
            if (on) r.round(pd, y, cw - 2 * pd, rh, sys::scale(6, dpi), p.menuSelected(), p.menuSelectedBorder());
            int tx = pd + sys::scale(14, dpi);
            const int isz = sys::scale(20, dpi);
            wchar_t from[200];
            from[0] = 0;
            if (twoLine()) crumb(it, from);
            const bool sub = from[0] != 0;
            const int iy = sub ? y + sys::scale(6, dpi) : y + (rh - isz) / 2;
            if (it.id >= ID_OPT0 && it.id < ID_OPT0 + (int)Options::O_COUNT) {
                const bool c = Options::get(it.id - ID_OPT0);
                r.textCentered(tx, iy, isz, isz, c ? L"\xE73A" : L"\xE739", F_ICON, c ? p.accent() : p.dim());
                tx += sys::scale(32, dpi);
            } else if (HBITMAP b = appIcon(i)) {
                r.alpha(tx, iy, isz, isz, b);
                tx += sys::scale(32, dpi);
            } else if (it.icon || it.id == ID_APP) {
                r.textCentered(tx, iy, isz, isz, it.icon ? it.icon : L"\xE7C3", F_ICON, on ? p.accentText() : p.fg());
                tx += sys::scale(32, dpi);
            }
            const COLORREF lc = on ? p.accentText() : bright;
            if (sub) {  // two lines: label, then where it lives
                r.textClipped(tx, y + s4, sys::scale(24, dpi), cw - pd - sys::scale(36, dpi) - tx, it.label, F_ROW, lc);
                r.textClipped(tx, y + sys::scale(27, dpi), sys::scale(16, dpi), cw - pd - sys::scale(12, dpi) - tx, from, F_SMALL, p.dim());
            } else r.textClipped(tx, y, rh, cw - pd - sys::scale(36, dpi) - tx, it.label, F_ROW, lc);
            if (it.id >= ID_PAGE0 && it.id < ID_PAGE0 + (int)kPages)
                r.textCentered(cw - pd - sys::scale(30, dpi), y, sys::scale(20, dpi), rh, L"\xE76C", F_ICON, p.dim());
            if (v == 0 && top == 0 && sepH())
                r.fill(pd, y + rh + sepH() / 2, cw - 2 * pd, 1, sys::mix(p.bg(), p.fg(), 16));
        }
        if (!nView)
            r.text(pd + sys::scale(14, dpi), listTop(), rh,
                   scanning && (qlen || page == APPS) ? L"Loading apps\x2026" : L"No matches", F_ROW, p.dim());

        r.frame(0, 0, cw, ch, sys::scale(2, dpi), p.menuBorder());
    }

    static int rowAt(int my) {
        for (int v = top; v < sys::imin(nView, top + kRows); ++v)
            if (my >= rowY(v) && my < rowY(v) + rowH()) return v;
        return -1;
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT rc;
                GetClientRect(h, &rc);
                paint(dc, rc.right, rc.bottom, Bus::palette());
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_APPS: adoptScan(); InvalidateRect(h, nullptr, FALSE); return 0;
            case WM_KEYDOWN:
                switch (wp) {
                    case VK_UP: move(-1); break;
                    case VK_DOWN: move(1); break;
                    case VK_PRIOR: move(-kRows); break;
                    case VK_NEXT: move(kRows); break;
                    case VK_RETURN: activate(); break;
                    case VK_ESCAPE:
                        if (page != MAIN) { showPage(parent(page)); InvalidateRect(h, nullptr, FALSE); }
                        else close();
                        break;
                }
                return 0;
            case WM_CHAR:
                if (wp == L' ' && !qlen && hasToggles()) { activate(); return 0; }
                if (wp == 8) {
                    if (qlen) query[--qlen] = 0;
                    else if (page != MAIN) { showPage(parent(page)); InvalidateRect(h, nullptr, FALSE); return 0; }
                }
                else if (wp >= 32 && qlen < 38) { query[qlen++] = (wchar_t)wp; query[qlen] = 0; }
                else return 0;
                refilter();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_LBUTTONDOWN: {
                const int v = rowAt((short)HIWORD(lp));
                if (v >= 0) { sel = v; activate(); }
                return 0;
            }
            case WM_MOUSEMOVE: {
                const int v = rowAt((short)HIWORD(lp));
                if (v >= 0 && v != sel) { sel = v; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            }
            case WM_MOUSEWHEEL: move((short)HIWORD(wp) > 0 ? -1 : 1); return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && hwnd) {
                    // Focus is still settling right after opening: take it back instead of closing.
                    if (GetTickCount() - openedAt < 500) tileutil::forceForeground(hwnd);
                    else close();
                }
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};
