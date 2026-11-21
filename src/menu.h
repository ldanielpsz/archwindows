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
#include <shobjidl.h>
#include <shlobj.h>

struct ShellLaunch {
    static bool run(const wchar_t* path, const wchar_t* params = nullptr) {
        return (INT_PTR)ShellExecuteW(nullptr, L"open", path, params, nullptr, SW_SHOWNORMAL) > 32;
    }
};

template <class Bus, class R, class Launch>
struct Menu {
    static constexpr int kMaxItems = 1536, kRows = 9;
    static constexpr size_t kArena = 1 << 20;
    // Omarchy-style menu tree: "Go" (MAIN) lists the categories, each opens a titled submenu.
    enum Page : u8 { MAIN, APPS, LEARN, KEYS, TRIGGER, CAPTURE, TOGGLE, STYLE, SETUP, INSTALL, REMOVE, UPDATE,
                     SYSTEM, kPages };
    enum : int { ID_NOOP = 1, ID_THEME, ID_QUIT, ID_RESTORE, ID_POWEROFF, ID_RESTART, ID_PAGE0 = 20,
                 ID_OPT0 = 100, ID_APP = 1000 };  // ID_APP and above: launch arg (+ params)
    struct Item { const wchar_t* label; const wchar_t* arg; int id; const wchar_t* icon; const wchar_t* params; u8 page; };
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
        items[nItems++] = {label, arg, id, icon, params, (u8)building};
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
    static void sadd(Scan* s, const wchar_t* label, const wchar_t* arg) {
        if (s->n >= kMaxApps || !label || !arg) return;
        s->items[s->n++] = {label, arg, ID_APP, nullptr, nullptr, (u8)APPS};
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

    static void scanDir(Scan* s, const wchar_t* dir, int depth) {
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
                if (depth < 6) scanDir(s, full, depth + 1);
                continue;
            }
            const int n = sys::wlen(fd.cFileName);
            int cut = 0;
            if (endsWithI(fd.cFileName, n, L".lnk") || endsWithI(fd.cFileName, n, L".url")) cut = 4;
            else if (endsWithI(fd.cFileName, n, L".appref-ms")) cut = 10;
            if (!cut) continue;
            if (startsWithI(fd.cFileName, L"uninstall") || startsWithI(fd.cFileName, L"unins0")) continue;  // uninstallers, not apps named "... Uninstall ..."
            sadd(s, sdup(s, fd.cFileName, n - cut), sdup(s, full, sys::wlen(full)));
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
                go(L"\xE777", L"Update", UPDATE);
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
                opt(L"Theme drives Windows dark/light and accent", Options::O_SYSTHEME);
                opt(L"Theme drives terminal colours", Options::O_TERMTHEME);
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
                act(L"\xE7E8", L"Power off", ID_POWEROFF);
                act(L"\xE777", L"Restart", ID_RESTART);
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
    static void refilter() {
        nView = 0;
        for (int i = 0; i < nItems; ++i) {
            if (!qlen) { if (items[i].page == page) view[nView++] = i; continue; }
            if (!matches(items[i].label)) continue;
            bool dup = false;  // the same setting can live on two pages (e.g. Tiling): list it once
            for (int v = 0; v < nView && !dup; ++v)
                dup = items[view[v]].id == items[i].id && items[view[v]].id != ID_APP && items[view[v]].id != ID_NOOP;
            if (!dup) view[nView++] = i;
        }
        sel = top = 0;
    }

    static void showPage(Page p) {
        page = p;
        qlen = 0;
        query[0] = 0;
        refilter();
    }

    // ------------------------------------------------------------ lifecycle --
    static int width() { return sys::scale(560, dpi); }
    static int rowH() { return sys::scale(30, dpi); }
    static int inputH() { return sys::scale(44, dpi); }
    static int pad() { return sys::scale(12, dpi); }
    static int height() { return pad() * 2 + inputH() + kRows * rowH() + sys::scale(30, dpi); }

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

    static void open(Page p, int curDpi) {
        if (hwnd) { SetForegroundWindow(hwnd); return; }
        dpi = curDpi;
        mon = focusedMonitor();  // before our window takes the focus
        InterlockedIncrement(&gen);
        used = 0;
        arena = (char*)VirtualAlloc(nullptr, kArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!arena) return;
        items = (Item*)take(sizeof(Item) * kMaxItems);
        view = (int*)take(sizeof(int) * kMaxItems);
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
        if (id == ID_THEME) Bus::openPicker();
        else if (id == ID_QUIT) Bus::quit();
        else if (id == ID_RESTORE) Bus::restoreDefault();
        else if (id == ID_POWEROFF || id == ID_RESTART) {
            const bool off = id == ID_POWEROFF;
            if (MessageBoxW(nullptr, off ? L"Power off this computer now?" : L"Restart this computer now?",
                            L"archwindows", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
                Launch::run(L"shutdown.exe", off ? L"/s /t 0" : L"/r /t 0");
        }
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
    static void paint(HDC dc, int cw, int ch, const Palette& p) {
        R r(dc, cw, ch);
        r.fill(0, 0, cw, ch, p.bg());
        const int pd = pad(), rh = rowH(), ih = inputH();

        // Input row: prompt, query, caret.
        r.text(pd, pd, ih, kTitle[page], F_TITLE, p.accent());
        const int px = pd + r.measure(kTitle[page], F_TITLE) + sys::scale(14, dpi);
        {
            r.text(px, pd, ih, query, F_TEXT, p.fg(), qlen);
            const int cx = px + r.measure(query, F_TEXT, qlen);
            r.fill(cx + 1, pd + (ih - sys::scale(16, dpi)) / 2, sys::scale(2, dpi), sys::scale(16, dpi), p.accent());
            if (!qlen) r.text(px + sys::scale(8, dpi), pd, ih, L"type to search", F_TEXT, p.dim());
        }
        r.fill(pd, pd + ih, cw - 2 * pd, 1, sys::mix(p.bg(), p.fg(), 20));

        // Rows.
        const int y0 = pd + ih + sys::scale(4, dpi);
        const int last = sys::imin(nView, top + kRows);
        for (int v = top; v < last; ++v) {
            const Item& it = items[view[v]];
            const int y = y0 + (v - top) * rh;
            const bool on = v == sel;
            if (on) r.fill(pd, y, cw - 2 * pd, rh, p.selected());
            int tx = pd + sys::scale(10, dpi);
            if (it.id >= ID_OPT0 && it.id < ID_OPT0 + (int)Options::O_COUNT) {
                const bool c = Options::get(it.id - ID_OPT0);
                r.textCentered(tx, y, sys::scale(18, dpi), rh, c ? L"\xE73A" : L"\xE739", F_ICON, c ? p.accent() : p.dim());
                tx += sys::scale(28, dpi);
            } else if (it.icon) {
                r.textCentered(tx, y, sys::scale(18, dpi), rh, it.icon, F_ICON, on ? p.accent() : p.dim());
                tx += sys::scale(28, dpi);
            }
            r.text(tx, y, rh, it.label, F_TEXT, on ? p.accent() : p.fg());
            if (qlen && it.page != MAIN) {  // where this result lives
                const wchar_t* from = kTitle[it.page];
                r.text(cw - pd - sys::scale(10, dpi) - r.measure(from, F_SMALL), y, rh, from, F_SMALL, p.dim());
            }
        }
        if (!nView)
            r.text(pd + sys::scale(10, dpi), y0, rh, scanning && (qlen || page == APPS) ? L"loading apps..." : L"no matches",
                   F_TEXT, p.dim());

        const wchar_t* hint = qlen ? L"Enter select   Esc back" : page == MAIN ? L"Enter open   Esc close" : hasToggles() ? L"Enter/Space toggle   Esc back" : L"Enter select   Esc back";
        r.text(cw - pd - r.measure(hint, F_SMALL), ch - sys::scale(24, dpi), sys::scale(20, dpi), hint, F_SMALL, p.dim());
        r.frame(0, 0, cw, ch, sys::scale(2, dpi), p.accent());
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
                const int row = ((short)HIWORD(lp) - (pad() + inputH() + sys::scale(4, dpi))) / rowH();
                if ((short)HIWORD(lp) >= pad() + inputH() && row >= 0 && top + row < nView) {
                    sel = top + row;
                    activate();
                }
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
