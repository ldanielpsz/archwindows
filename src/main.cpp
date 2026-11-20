// archwindows - composition root.
//
// Policy-based design: each concern (themes, rendering, images, wallpaper, persistence,
// workspace model, bar widgets, window style/frame/layout, launcher) is a small type with a
// static interface, and the shell is just the instantiation below. Swapping GDI for Direct2D,
// or the dwindle layout for another one, is a one-line change.
//
// No C runtime: see the entry point at the bottom and build.cmd.
#include <stdarg.h>
#include "sys.h"
#include "theme.h"
#include "options.h"
#include "render_gdi.h"
#include "image_wic.h"
#include "platform.h"
#include "bar.h"
#include "picker.h"
#include "tiler.h"
#include "winstyle.h"
#include "menu.h"

// The compiler may emit calls to these even in freestanding code. Optimisation
// is disabled inside them so the loops are not turned back into self-calls.
#pragma optimize("", off)
extern "C" {
#pragma function(memset, memcpy)
void* memset(void* d, int v, size_t n) { auto p = (unsigned char*)d; while (n--) *p++ = (unsigned char)v; return d; }
void* memcpy(void* d, const void* s, size_t n) { auto a = (unsigned char*)d; auto b = (const unsigned char*)s; while (n--) *a++ = *b++; return d; }
int _fltused;
}
#pragma optimize("", on)

// ---------------------------------------------------------------- policies --
using Themes = EmbeddedThemes;
using Render = GdiRenderer;

struct Bus {  // what the bar, picker, menu and layout may ask of the shell
    static void openPicker();
    static int theme();
    static const Palette& palette();
    static void preview(int);
    static void commit(int);
    static void scheduleTrim();
    static void dpiChanged(int);
    static void chord(int);
    static void optionsChanged();
    static void quit();
    static void restoreDefault();
    static void repaintBar();
    static void displayChanged();
};

using Layout = Dwindle<Bus>;
using Watcher = WindowWatcher<Bus, DwmStyle, StripCaption, Layout>;

// Bar workspace indicator fed by the tiler; hidden while tiling is off.
struct TileWorkspaces {
    static bool visible() { return Options::tiling; }
    // The bar lives on the primary monitor, so it shows and drives that monitor's workspaces.
    static int count() { return Layout::slots(Layout::primaryMon()); }
    static int active() { return Layout::curWs(Layout::primaryMon()); }
    static void focus(int i) { Layout::switchTo(i, Layout::primaryMon()); }
};

using TheBar = Bar<Bus, Render,
                   LogoW, WorkspacesW<TileWorkspaces>, ClockW,
                   IconW<0xE701>,  // wifi
                   IconW<0xE702>,  // bluetooth
                   IconW<0xE767>,  // volume
                   BatteryW, PowerW>;
using ThePicker = Picker<Bus, Render, Themes, WicImage, SpiWallpaper, RegistryStore>;
using TheMenu = Menu<Bus, Render, ShellLaunch>;

// Command line --------------------------------------------------------------
static bool hasFlag(const wchar_t* name) {
    const int n = sys::wlen(name);
    for (const wchar_t* c = GetCommandLineW(); *c; ++c) {
        int i = 0;
        while (i < n && c[i] == name[i]) ++i;
        if (i == n) return true;
    }
    return false;
}

// Value of `--name=value` (value ends at a space or quote).
static bool flagValue(const wchar_t* name, wchar_t* out, int cap) {
    const int n = sys::wlen(name);
    for (const wchar_t* c = GetCommandLineW(); *c; ++c) {
        int i = 0;
        while (i < n && c[i] == name[i]) ++i;
        if (i != n) continue;
        const wchar_t* v = c + n;
        int k = 0;
        while (*v && *v != L' ' && *v != L'"' && k < cap - 1) out[k++] = *v++;
        out[k] = 0;
        return k > 0;
    }
    return false;
}

// `--log=<path>`: append timestamped lifecycle lines (start, hook, taskbar, quit) for diagnosis.
static wchar_t gLogPath[MAX_PATH];
static void logf(const wchar_t* fmt, ...) {
    if (!gLogPath[0]) return;
    wchar_t msg[256], line[320];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    const int n = wsprintfW(line, L"%02d:%02d:%02d.%03d pid=%lu tid=%lu %s\r\n", t.wHour, t.wMinute, t.wSecond,
                            t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), msg);
    HANDLE f = CreateFileW(gLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char u[960];
    const int b = WideCharToMultiByte(CP_UTF8, 0, line, n, u, sizeof(u), nullptr, nullptr);
    DWORD w;
    WriteFile(f, u, (DWORD)b, &w, nullptr);
    CloseHandle(f);
}

// ------------------------------------------------------------------- state --
static struct {
    Palette pal;
    int committed;
    int dpi;
    bool tilingOn;  // what the layout engine is currently doing
    wchar_t logoFace[64];
    TheBar bar;
} S;

static LRESULT CALLBACK barProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    S.bar.hwnd = h;
    return S.bar.proc(h, m, w, l, S.pal);
}

void Bus::openPicker() { ThePicker::open(S.dpi); }
int Bus::theme() { return S.committed; }
const Palette& Bus::palette() { return S.pal; }
void Bus::preview(int i) {
    S.pal = Themes::palette(i);
    InvalidateRect(S.bar.hwnd, nullptr, FALSE);
    if (ThePicker::hwnd) InvalidateRect(ThePicker::hwnd, nullptr, FALSE);
    if (TheMenu::hwnd) InvalidateRect(TheMenu::hwnd, nullptr, FALSE);
    Watcher::restyleAll();  // other apps' windows follow the theme live
}
void Bus::commit(int i) { S.committed = i; preview(i); }
void Bus::scheduleTrim() { SetTimer(S.bar.hwnd, TheBar::kTrimTimer, 1500, nullptr); }
void Bus::dpiChanged(int dpi) {
    S.dpi = dpi;
    Fonts::build(dpi, S.logoFace[0] ? S.logoFace : nullptr);
}
static bool gReloadExplorer;
void Bus::quit() { PostQuitMessage(0); }
void Bus::restoreDefault() { gReloadExplorer = true; PostQuitMessage(0); }

// Add/remove this exe in HKCU\...\Run so it starts at sign-in.
static void applyStartup(bool on) {
    static constexpr const wchar_t* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (on) {
        wchar_t path[MAX_PATH + 2];
        path[0] = L'"';
        const DWORD n = GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
        if (!n) return;
        path[n + 1] = L'"';
        path[n + 2] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, run, L"ArchWindows", REG_SZ, path, (DWORD)(n + 3) * sizeof(wchar_t));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, run, L"ArchWindows");
    }
}
void Bus::displayChanged() { if (Options::tiling) Layout::displayChanged(); }
void Bus::repaintBar() { if (S.bar.hwnd) InvalidateRect(S.bar.hwnd, nullptr, FALSE); }

void Bus::optionsChanged() {
    RegistryStore::saveOptions(Options::pack());
    applyStartup(Options::startup);
    if (Options::tiling != S.tilingOn) {
        S.tilingOn = Options::tiling;
        if (S.tilingOn) Layout::enable(); else Layout::disable();
    }
    Watcher::restyleAll();  // title bars, colours, and (via relayout) gaps
    Bus::repaintBar();
}

// A Super-key chord from the keyboard hook (keys.h). Window commands act on the focused window.
void Bus::chord(int i) {
    const KeyChord& c = kChords[i];
    if (c.act == K_MENU) { TheMenu::open(TheMenu::MAIN, S.dpi); return; }
    HWND h = Watcher::target();
    const int step = sys::scale(100, S.dpi);
    switch (c.act) {
        case K_CLOSE: if (h) PostMessageW(h, WM_CLOSE, 0, 0); break;
        case K_MIN: if (h) ShowWindow(h, SW_MINIMIZE); break;
        case K_SPLIT: Layout::toggleSplit(h); break;
        case K_FLOAT: Layout::toggleFloat(h); Watcher::handle(h, true); break;
        case K_FULL: case K_FULLW: Layout::fullscreen(h, c.arg); break;
        case K_FOCUS: Layout::focusDir(h, c.arg); break;
        case K_SWAP: Layout::swapDir(h, c.arg); break;
        case K_WS: Layout::switchTo(c.arg); break;
        case K_MOVE: Layout::moveToWorkspace(h, c.arg); break;
        case K_WSREL: Layout::switchRel(c.arg); break;
        case K_MONMOVE: Layout::moveToMonitorDir(h, c.arg); break;
        case K_RESIZE: Layout::resize(h, c.arg < 2 ? 0 : 1, (c.arg & 1) ? step : -step); break;
    }
}

// The Windows taskbar (and with it the Start button) is hidden while we run. Explorer can
// re-show it or recreate it (crash/restart -> "TaskbarCreated"), so it is re-hidden on that
// broadcast and on the bar's minute clock tick. Restored on exit.
static void setTaskbars(bool show) {
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        logf(L"taskbar: primary %p show=%d", (void*)t, (int)show);
        ShowWindow(t, show ? SW_SHOWNA : SW_HIDE);
        logf(L"taskbar: primary done");
    }
    // Collect first, then change: showing/hiding reorders the Z-order, which makes a
    // FindWindowEx walk revisit windows forever (it hung shutdown with 2+ monitors).
    HWND sec[16];
    int n = 0;
    for (HWND t = nullptr; n < 16 && (t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr));) {
        bool seen = false;
        for (int i = 0; i < n; ++i) seen |= sec[i] == t;
        if (seen) break;
        sec[n++] = t;
    }
    for (int i = 0; i < n; ++i) {
        logf(L"taskbar: secondary %p show=%d", (void*)sec[i], (int)show);
        ShowWindow(sec[i], show ? SW_SHOWNA : SW_HIDE);
    }
}
static UINT gTaskbarCreated;

// Trim runs from a timer so it fires once the bar has been idle.
static LRESULT CALLBACK barProcWithTrim(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gTaskbarCreated || (m == WM_TIMER && w == TheBar::kClockTimer)) setTaskbars(false);
    if (m == WM_TIMER && w == TheBar::kTrimTimer) {
        KillTimer(h, TheBar::kTrimTimer);
        TrimMemory::now();
        return 0;
    }
    if (m == WM_CLOSE) { logf(L"bar: WM_CLOSE received -> quitting"); PostQuitMessage(0); return 0; }
    return barProc(h, m, w, l);
}

// ------------------------------------------------------------- entry point --
extern "C" void start() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // `--reset`: give every window its title bar, position visibility and colours back, then exit
    // (use after a crash or force-kill).
    if (hasFlag(L"--reset")) {
        Layout::recoverHidden();
        Watcher::resetAll();
        ExitProcess(0);
    }

    flagValue(L"--log=", gLogPath, MAX_PATH);
    logf(L"start: archwindows launching");
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\ArchWindowsBar");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { logf(L"start: another instance owns the mutex, exiting"); ExitProcess(0); }

    HINSTANCE hi = GetModuleHandleW(nullptr);
    Assets::locate();

    // Restore the last applied theme (the bar itself needs no assets).
    wchar_t saved[64] = {};
    int bg = 0;
    int idx = RegistryStore::load(saved, 64, bg) ? Themes::find(saved) : -1;
    if (idx < 0) idx = Themes::find(L"tokyo-night");
    if (idx < 0) idx = 0;
    S.committed = idx;
    S.pal = Themes::palette(idx);

    // Options (defaults: tiling off, windows roam free). `--tiling` forces it on for this run.
    unsigned bits = 0;
    if (RegistryStore::loadOptions(bits)) Options::unpack(bits);
    const bool noStyle = hasFlag(L"--no-style");  // bar + menu only; leave other windows alone
    if (hasFlag(L"--tiling")) Options::tiling = true;
    if (noStyle) Options::tiling = false;
    flagValue(L"--scope=", Layout::scope, 48);  // dev: only manage windows whose title contains this
    S.tilingOn = Options::tiling;

    unsigned glyph = Fonts::kDefaultLogo;
    RegistryStore::loadLogo(S.logoFace, 64, glyph);
    Fonts::setLogoGlyph(glyph);

    S.dpi = (int)GetDpiForSystem();
    S.bar.setDpi(S.dpi);
    Bus::dpiChanged(S.dpi);

    WNDCLASSW wc{};
    wc.lpfnWndProc = barProcWithTrim;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AWBar";
    RegisterClassW(&wc);

    S.bar.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"AWBar", L"archwindows",
                                 WS_POPUP, 0, 0, 100, S.bar.m.h, nullptr, nullptr, hi, nullptr);
    if (!S.bar.hwnd) { wchar_t e[32]; wsprintfW(e, L"CreateWindow failed: %lu", GetLastError()); MessageBoxW(nullptr, e, L"archwindows", MB_OK); ExitProcess(1); }
    S.bar.place();
    S.bar.armClock();

    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    setTaskbars(false);
    logf(L"start: bar window %p created, taskbar hidden", (void*)S.bar.hwnd);
    LowLevelKeys::start(S.bar.hwnd);
    logf(L"start: keyboard hook %s", LowLevelKeys::hook ? L"installed" : L"FAILED");  // Win+Space always; the tiling chords only while tiling is on
    if (hasFlag(L"--picker")) Bus::openPicker();
    if (hasFlag(L"--menu")) Bus::chord(0);
    if (!noStyle) Watcher::start();
    logf(L"start: ready, entering message loop (tiling=%d)", (int)Options::tiling);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logf(L"stop: message loop ended (reloadExplorer=%d)", (int)gReloadExplorer);
    LowLevelKeys::stop();
    logf(L"stop: keyboard hook removed");
    setTaskbars(true);
    logf(L"stop: taskbar restored");
    if (gReloadExplorer) {
        // Restart Explorer so the desktop, work area and taskbar come back clean. Explorer is
        // normally respawned by Windows; start it ourselves only if it did not come back.
        wchar_t cmd[] = L"cmd.exe /c taskkill /f /im explorer.exe >nul & timeout /t 2 /nobreak >nul & "
                        L"(tasklist /fi \"imagename eq explorer.exe\" | find /i \"explorer.exe\" >nul || start explorer.exe)";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (!noStyle) Watcher::stop();
    logf(L"stop: window styles restored");
    S.bar.unreserve();
    logf(L"stop: appbar reservation released, exiting 0");
    CloseHandle(once);
    ExitProcess(0);
}
