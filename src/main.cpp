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
#include "systheme.h"
#include "termtheme.h"
#include "menu.h"
#include "spawn.h"
#include "ui.h"
#include "net.h"
#include "audio.h"
#include "display.h"
#include "panels.h"
#include "agents.h"
#include "bt.h"
#include "browsertheme.h"

// The compiler may emit calls to these even in freestanding code. Optimisation
// is disabled inside them so the loops are not turned back into self-calls.
#pragma optimize("", off)
extern "C" {
#pragma function(memset, memcpy, memcmp)
void* memset(void* d, int v, size_t n) { auto p = (unsigned char*)d; while (n--) *p++ = (unsigned char)v; return d; }
void* memcpy(void* d, const void* s, size_t n) { auto a = (unsigned char*)d; auto b = (const unsigned char*)s; while (n--) *a++ = *b++; return d; }
int memcmp(const void* a, const void* b, size_t n) { auto x = (const unsigned char*)a; auto y = (const unsigned char*)b; for (; n--; ++x, ++y) if (*x != *y) return *x - *y; return 0; }
int _fltused;
int __cdecl _purecall() { return 0; }
}
#pragma optimize("", on)

// ---------------------------------------------------------------- policies --
using Themes = EmbeddedThemes;
using Render = GdiRenderer;

struct Bus {  // what the bar, picker, menu and layout may ask of the shell
    static void openPicker(HMONITOR mon = nullptr);
    static int theme();
    static const Palette& palette();
    static void preview(int);
    static void commit(int);
    static void scheduleTrim();
    static void dpiChanged(int);
    static void monitorDpiChanged(HMONITOR, int);
    static void chord(int);
    static void optionsChanged();
    static void quit();
    static void restoreDefault();
    static void repaintBar();
    static void displayChanged();
    static void launchOn(HMONITOR);
    static void windowShown(HWND);
    static void openMenu(HMONITOR);
    static void openTerminal(HMONITOR);
    static void openPanel(int, HMONITOR, RECT);
};

using Layout = Dwindle<Bus>;
using Placement = SpawnPlacement<Layout>;
using Watcher = WindowWatcher<Bus, DwmStyle, StripCaption, Layout>;

// Bar workspace indicator fed by the tiler; hidden while tiling is off. Every monitor has its
// own bar, which shows and drives that monitor's workspaces.
struct TileWorkspaces {
    static bool visible() { return Options::tiling; }
    static int count(HMONITOR m) { return Layout::slots(Layout::monIndex(m)); }
    static int active(HMONITOR m) { return Layout::curWs(Layout::monIndex(m)); }
    static void focus(HMONITOR m, int i) { Layout::switchTo(i, Layout::monIndex(m)); }
};

// Status icon sources (bar.h GlyphW): each names its glyph and its optional click/scroll hooks.
struct NetIcon { static const wchar_t* glyph() { return Net::barGlyph(); } };
struct AudioIcon {
    static const wchar_t* glyph() { return Audio::barGlyph(); }
    static void scroll(int dir) { Audio::scroll(dir); }
    static void right() { Audio::toggleMute(); }
};
struct DisplayIcon { static const wchar_t* glyph() { return L"\xE7F4"; } };
struct BluetoothIcon {  // shown when the machine has a Bluetooth adapter; right click = radio on/off
    static bool visible() { return Bt::visible(); }
    static const wchar_t* glyph() { return Bt::barGlyph(); }
    static void right() { Bt::radioAsync(Bt::radio == 0 ? 1 : 0); }
};
struct AgentsIcon {  // Claude's mark from the Omarchy font; shown once Claude Code is on the machine
    static inline int seen = -1;
    static bool visible() { if (seen < 0) seen = Agents::installed(); return seen != 0; }
    static Font font() { return F_OMA; }
    static const wchar_t* glyph() { return L"\xE90E"; }
    static void right() { Agents::launch(nullptr); }
};

// Omarchy's bar: menu, workspaces | clock | keyboard, agents, bluetooth, network, audio, display, power.
using TheBar = Bar<Bus, Render,
                   LogoW, WorkspacesW<TileWorkspaces>, ClockW, KeyboardW,
                   GlyphW<AgentsIcon, P_AGENTS>, GlyphW<BluetoothIcon, P_BLUETOOTH>, GlyphW<NetIcon, P_NET>,
                   GlyphW<AudioIcon, P_AUDIO>, GlyphW<DisplayIcon, P_DISPLAY>, PowerW<Power>>;
// Panel order matches PanelId (bar.h).
using TheFlyout = Flyout<Bus, Net, Audio, Display, Power, Agents, Calendar, Bt>;
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
static constexpr int kMaxBars = 4;  // as many as the tiler handles
static constexpr UINT_PTR kTrimTimer = 2, kTaskbarTimer = 3, kStatusTimer = 4;

// constinit: there is no CRT to run dynamic initialisers, so this must be built at compile time.
static constinit struct {
    Palette pal;
    int committed = 0;
    int dpi = 96;  // shared by every bar (fonts are global): the primary monitor's
    bool tilingOn = false;  // what the layout engine is currently doing
    bool sysThemeOn = false;  // Windows' own colours currently follow the theme
    bool termThemeOn = false;  // terminal colours currently follow the theme
    bool browserThemeOn = false;  // Chrome/Brave colour currently follows the theme
    unsigned statusSig = 0;  // what the bar's status icons showed at the last check
    wchar_t logoFace[64] = {};
    HWND ctl = nullptr;  // hidden controller: hotkeys, timers, broadcasts, display changes
    TheBar bars[kMaxBars];  // one per monitor; a free entry has hwnd == nullptr
} S;

// Each bar window carries a pointer to its TheBar (set on WM_NCCREATE from CreateWindowEx's param).
static LRESULT CALLBACK barProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        TheBar* b = (TheBar*)((CREATESTRUCTW*)l)->lpCreateParams;
        b->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)b);
    }
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }  // e.g. taskkill without /f
    TheBar* b = (TheBar*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return b ? b->proc(h, m, w, l, S.pal) : DefWindowProcW(h, m, w, l);
}

void Bus::openPicker(HMONITOR mon) { ThePicker::open(S.dpi, mon); }
int Bus::theme() { return S.committed; }
const Palette& Bus::palette() { return S.pal; }
void Bus::preview(int i) {
    S.pal = Themes::palette(i);
    Bus::repaintBar();
    if (ThePicker::hwnd) InvalidateRect(ThePicker::hwnd, nullptr, FALSE);
    if (TheMenu::hwnd) InvalidateRect(TheMenu::hwnd, nullptr, FALSE);
    TheFlyout::repaint();
    Watcher::restyleAll();  // other apps' windows follow the theme live
}
void Bus::commit(int i) {
    S.committed = i;
    preview(i);
    if (Options::sysTheme) SysTheme::apply(S.pal);  // committed themes only, never previews
    if (Options::termTheme) TermTheme::apply(S.pal);
    if (Options::browserTheme) BrowserTheme::apply(Themes::name(S.committed), S.pal.light);
}
void Bus::scheduleTrim() { if (S.ctl) SetTimer(S.ctl, kTrimTimer, 1500, nullptr); }
void Bus::dpiChanged(int dpi) {
    S.dpi = dpi;
    Fonts::build(dpi, S.logoFace[0] ? S.logoFace : nullptr);
}
// Only the primary monitor's scale drives the shared fonts and bar height; bars on monitors with
// another scale keep the primary's size.
void Bus::monitorDpiChanged(HMONITOR mon, int dpi) {
    if (mon != MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY) || dpi == S.dpi) return;
    Bus::dpiChanged(dpi);
    for (TheBar& b : S.bars)
        if (b.hwnd) { b.setDpi(dpi); b.place(); InvalidateRect(b.hwnd, nullptr, FALSE); }
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
// Always, even with tiling off, so the tiler never works from a stale monitor list.
void Bus::displayChanged() { Layout::displayChanged(); }
void Bus::launchOn(HMONITOR m) { Placement::arm(m); }
void Bus::windowShown(HWND h) { Placement::claim(h); }
void Bus::openMenu(HMONITOR m) { TheFlyout::close(); TheMenu::open(TheMenu::MAIN, S.dpi, m); }
void Bus::openTerminal(HMONITOR m) { Placement::arm(m); DefaultApps<ShellLaunch>::terminal(); }
void Bus::openPanel(int which, HMONITOR m, RECT anchor) { TheFlyout::toggle(which, m, anchor, S.dpi); }

// The status icons (network, volume, battery, keyboard layout) change without any event the bars
// see, so the controller checks them every few seconds and repaints only when one changed.
static unsigned statusSignature() {
    unsigned h = 2166136261u;
    auto mixIn = [&](unsigned v) { h = (h ^ v) * 16777619u; };
    for (const wchar_t* g = Net::barGlyph(); *g; ++g) mixIn(*g);
    for (const wchar_t* g = Audio::barGlyph(); *g; ++g) mixIn(*g);
    bool ch = false;
    mixIn((unsigned)Power::battery(&ch) * 2 + ch);
    mixIn((unsigned)(ULONG_PTR)KeyboardW::current());
    return h;
}
void Bus::repaintBar() {
    for (TheBar& b : S.bars) if (b.hwnd) InvalidateRect(b.hwnd, nullptr, FALSE);
}

void Bus::optionsChanged() {
    RegistryStore::saveOptions(Options::pack());
    applyStartup(Options::startup);
    if (Options::tiling != S.tilingOn) {
        S.tilingOn = Options::tiling;
        if (S.tilingOn) Layout::enable(); else Layout::disable();
    }
    if (Options::sysTheme != S.sysThemeOn) {
        S.sysThemeOn = Options::sysTheme;
        if (S.sysThemeOn) SysTheme::apply(S.pal); else SysTheme::restore();
    }
    if (Options::termTheme != S.termThemeOn) {
        S.termThemeOn = Options::termTheme;
        if (S.termThemeOn) TermTheme::apply(S.pal); else TermTheme::restore();
    }
    if (Options::browserTheme != S.browserThemeOn) {
        S.browserThemeOn = Options::browserTheme;
        if (S.browserThemeOn) BrowserTheme::apply(Themes::name(S.committed), S.pal.light); else BrowserTheme::restore();
    }
    Watcher::restyleAll();  // title bars, colours, and (via relayout) gaps
    Bus::repaintBar();
}

// A Super-key chord from the keyboard hook (keys.h). Window commands act on the focused window.
void Bus::chord(int i) {
    const KeyChord& c = kChords[i];
    if (c.act == K_MENU) { TheFlyout::close(); TheMenu::open(TheMenu::MAIN, S.dpi); return; }
    if (c.act == K_PICKER) { Bus::openPicker(TheMenu::focusedMonitor()); return; }
    if (c.act == K_TERMINAL || c.act == K_BROWSER) {
        Placement::arm(TheMenu::focusedMonitor());  // opens on the screen you are working on
        if (c.act == K_TERMINAL) DefaultApps<ShellLaunch>::terminal(); else DefaultApps<ShellLaunch>::browser();
        return;
    }
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

// A hidden taskbar still reserves its strip of the work area, which left an empty band under
// maximised and tiled windows. Switching the taskbar to auto-hide releases the strip (it is the
// "Automatically hide the taskbar" setting, the one way Windows offers). The user's own state is
// kept under HKCU\Software\ArchWindows\TaskbarState while we run, so a crash or --reset can put
// it back, and it is restored on exit.
static constexpr const wchar_t* kTaskbarState = L"TaskbarState";
static void taskbarAutoHide(bool on) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD saved = 0, sz = sizeof(saved);
    const bool have = RegGetValueW(HKEY_CURRENT_USER, RegistryStore::key, kTaskbarState, RRF_RT_REG_DWORD, nullptr,
                                   &saved, &sz) == ERROR_SUCCESS;
    if (on) {
        const UINT cur = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
        if (cur & ABS_AUTOHIDE) return;  // already auto-hiding (the user's choice, or ours)
        if (!have) {                     // remember the true original only
            const DWORD v = cur;
            RegSetKeyValueW(HKEY_CURRENT_USER, RegistryStore::key, kTaskbarState, REG_DWORD, &v, sizeof(v));
        }
        abd.lParam = (LPARAM)(cur | ABS_AUTOHIDE);
        SHAppBarMessage(ABM_SETSTATE, &abd);
        logf(L"taskbar: auto-hide on (was 0x%x)", cur);
    } else if (have) {
        abd.lParam = (LPARAM)saved;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RegistryStore::key, kTaskbarState);
        logf(L"taskbar: state restored (0x%lx)", saved);
    }
}

// The Windows taskbar (and with it the Start button) is hidden while we run. Explorer can
// re-show it or recreate it (crash/restart -> "TaskbarCreated"), so it is re-hidden on that
// broadcast and on the bar's minute clock tick. Restored on exit.
static void setTaskbars(bool show) {
    if (!show) taskbarAutoHide(true);
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
    if (show) taskbarAutoHide(false);
}
static UINT gTaskbarCreated;

// ------------------------------------------------------------------ bars --
struct MonList { HMONITOR h[kMaxBars]; int n; };
static BOOL CALLBACK collectMon(HMONITOR h, HDC, LPRECT, LPARAM l) {
    MonList* ml = (MonList*)l;
    if (ml->n < kMaxBars) ml->h[ml->n++] = h;
    return ml->n < kMaxBars;
}

static void logMonitors(const wchar_t* why) {
    MonList ml{};
    EnumDisplayMonitors(nullptr, nullptr, collectMon, (LPARAM)&ml);
    logf(L"monitors (%s): %d", why, ml.n);
    for (int i = 0; i < ml.n; ++i) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(ml.h[i], &mi);
        logf(L"  %p %d,%d %dx%d work %d,%d %dx%d%s", (void*)ml.h[i], mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcWork.left,
             mi.rcWork.top, mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top,
             (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" primary" : L"");
    }
}

static void createBar(TheBar& b, HMONITOR mon) {
    b.setDpi(S.dpi);
    b.m.mon = mon;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return;
    // Created on its own monitor, so it never gets a WM_DPICHANGED for moving there.
    CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"AWBar", L"archwindows", WS_POPUP,
                    mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, b.m.h, nullptr,
                    nullptr, GetModuleHandleW(nullptr), &b);
    if (!b.hwnd) { logf(L"bar: CreateWindow failed: %lu", GetLastError()); return; }
    b.place();
    b.armClock();
}

static void destroyBar(TheBar& b) {
    b.unreserve();
    DestroyWindow(b.hwnd);
    b = TheBar{};
}

// One bar per monitor: drop bars whose monitor is gone (HMONITORs change with the topology),
// re-place the rest, add bars for new monitors.
static void syncBars() {
    MonList ml{};
    EnumDisplayMonitors(nullptr, nullptr, collectMon, (LPARAM)&ml);
    for (TheBar& b : S.bars) {
        if (!b.hwnd) continue;
        bool keep = false;
        for (int j = 0; j < ml.n; ++j) keep |= ml.h[j] == b.m.mon;
        if (keep) b.place(); else destroyBar(b);
    }
    for (int j = 0; j < ml.n; ++j) {
        bool has = false;
        for (TheBar& b : S.bars) has |= b.hwnd && b.m.mon == ml.h[j];
        if (has) continue;
        for (TheBar& b : S.bars)
            if (!b.hwnd) { createBar(b, ml.h[j]); break; }
    }
    int n = 0;
    for (TheBar& b : S.bars) n += b.hwnd != nullptr;
    logf(L"bars: %d", n);
}

// ------------------------------------------------------------ controller --
// Never shown. Top-level, so it still gets broadcasts (TaskbarCreated, WM_DISPLAYCHANGE).
static LRESULT CALLBACK ctlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (gTaskbarCreated && m == gTaskbarCreated) {
        // Explorer restarted: the taskbar is back and our app bar reservations are gone.
        setTaskbars(false);
        for (TheBar& b : S.bars) if (b.hwnd) { b.reserved = false; b.place(); }
        return 0;
    }
    switch (m) {
        case WM_TIMER:
            if (w == kTrimTimer) { KillTimer(h, kTrimTimer); TrimMemory::now(); }
            else if (w == kTaskbarTimer) setTaskbars(false);  // Explorer can re-show it
            else if (w == kStatusTimer) {
                const unsigned sig = statusSignature();
                if (sig != S.statusSig) { S.statusSig = sig; Bus::repaintBar(); }
            }
            return 0;
        case Audio::WM_AUDIO:  // volume changed (keys, other apps, the panel)
            Bus::repaintBar();
            if (TheFlyout::isOpen(P_AUDIO)) TheFlyout::repaint();
            return 0;
        case WM_CHORD: Bus::chord((int)w); return 0;
        case WM_SETTINGCHANGE:  // the work area grew or shrank (e.g. the taskbar's strip released)
            if (w == SPI_SETWORKAREA) {
                logf(L"work area changed: relayout");
                if (S.tilingOn) Layout::relayout();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            logMonitors(L"display change");
            syncBars();
            Bus::displayChanged();
            return 0;
        case WM_CLOSE: logf(L"shell: WM_CLOSE received -> quitting"); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------- entry point --
extern "C" void start() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // `--reset`: give every window its title bar, position visibility and colours back, then exit
    // (use after a crash or force-kill).
    if (hasFlag(L"--reset")) {
        Layout::recoverHidden();
        SysTheme::restore();
        BrowserTheme::restore();
        taskbarAutoHide(false);
        Watcher::resetAll();
        ExitProcess(0);
    }

    flagValue(L"--log=", gLogPath, MAX_PATH);
    logf(L"start: archwindows launching");
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\ArchWindowsBar");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { logf(L"start: another instance owns the mutex, exiting"); ExitProcess(0); }

    HINSTANCE hi = GetModuleHandleW(nullptr);
    Assets::locate();
    Assets::loadFonts();  // before the fonts are built

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
    S.sysThemeOn = Options::sysTheme;
    S.termThemeOn = Options::termTheme;
    if (S.termThemeOn) TermTheme::apply(S.pal);  // e.g. first run after install, or the option set elsewhere
    S.browserThemeOn = Options::browserTheme;
    if (S.browserThemeOn) BrowserTheme::apply(Themes::name(S.committed), S.pal.light);
    if (S.sysThemeOn) SysTheme::apply(S.pal);
    else SysTheme::restore();  // a backup left by a crash

    unsigned glyph = Fonts::kDefaultLogo;
    RegistryStore::loadLogo(S.logoFace, 64, glyph);
    Fonts::setLogoGlyph(glyph);

    S.dpi = (int)GetDpiForSystem();
    Bus::dpiChanged(S.dpi);
    logf(L"font: %s", Fonts::face);

    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = ctlProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"AWShell";
    RegisterClassW(&wc);
    wc.lpfnWndProc = barProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AWBar";
    RegisterClassW(&wc);

    S.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, L"AWShell", L"archwindows", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi,
                            nullptr);
    if (!S.ctl) { wchar_t e[32]; wsprintfW(e, L"CreateWindow failed: %lu", GetLastError()); MessageBoxW(nullptr, e, L"archwindows", MB_OK); ExitProcess(1); }
    SetTimer(S.ctl, kTaskbarTimer, 60000, nullptr);
    SetTimer(S.ctl, kStatusTimer, 3000, nullptr);
    Audio::notify = S.ctl;

    logMonitors(L"start");
    syncBars();
    setTaskbars(false);
    logf(L"start: bars created, taskbar hidden");
    LowLevelKeys::start(S.ctl);
    logf(L"start: keyboard hook %s", LowLevelKeys::hook ? L"installed" : L"FAILED");  // Win+Space always; the tiling chords only while tiling is on
    if (hasFlag(L"--picker")) Bus::openPicker();
    if (hasFlag(L"--menu")) Bus::chord(0);
    // dev: --panel=N opens bar panel N (PanelId) under the primary bar's right edge
    wchar_t pv[4];
    if (flagValue(L"--panel=", pv, 4) && S.bars[0].hwnd) {
        RECT wr;
        GetWindowRect(S.bars[0].hwnd, &wr);
        Bus::openPanel(pv[0] - L'0', S.bars[0].m.mon, RECT{wr.right - 80, wr.top, wr.right - 40, wr.bottom});
    }
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
    if (SysTheme::active) SysTheme::restore();  // before Watcher::stop, whose frame reset reads the system mode
    if (!noStyle) Watcher::stop();
    logf(L"stop: window styles restored");
    for (TheBar& b : S.bars) if (b.hwnd) b.unreserve();
    logf(L"stop: appbar reservations released, exiting 0");
    CloseHandle(once);
    ExitProcess(0);
}
