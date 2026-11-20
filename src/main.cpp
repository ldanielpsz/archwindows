// archwindows - composition root.
//
// Policy-based design: each concern (themes, rendering, images, wallpaper,
// persistence, workspace model, bar widgets) is a small type with a static
// interface, and the shell is just the instantiation below. Swapping GDI for
// Direct2D, or the stub workspace model for the tiler, is a one-line change.
//
// No C runtime: see the entry point at the bottom and build.cmd.
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "image_wic.h"
#include "platform.h"
#include "bar.h"
#include "picker.h"

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

struct Bus {  // what the bar and picker may ask of the shell
    static void openPicker();
    static int theme();
    static const Palette& palette();
    static void preview(int);
    static void commit(int);
    static void scheduleTrim();
    static void dpiChanged(int);
};

using TheBar = Bar<Bus, Render,
                   LogoW, WorkspacesW<StubWorkspaces>, ClockW,
                   IconW<0xE701>,  // wifi
                   IconW<0xE702>,  // bluetooth
                   IconW<0xE767>,  // volume
                   BatteryW, PowerW>;
using ThePicker = Picker<Bus, Render, Themes, WicImage, SpiWallpaper, RegistryStore>;

// ------------------------------------------------------------------- state --
static struct {
    Palette pal;
    int committed;
    int dpi;
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
}
void Bus::commit(int i) { S.committed = i; preview(i); }
void Bus::scheduleTrim() { SetTimer(S.bar.hwnd, TheBar::kTrimTimer, 1500, nullptr); }
void Bus::dpiChanged(int dpi) {
    S.dpi = dpi;
    Fonts::build(dpi, S.logoFace[0] ? S.logoFace : nullptr);
}

// Trim runs from a timer so it fires once the bar has been idle.
static LRESULT CALLBACK barProcWithTrim(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == TheBar::kTrimTimer) {
        KillTimer(h, TheBar::kTrimTimer);
        TrimMemory::now();
        return 0;
    }
    return barProc(h, m, w, l);
}

// ------------------------------------------------------------- entry point --
extern "C" void start() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\ArchWindowsBar");
    if (GetLastError() == ERROR_ALREADY_EXISTS) ExitProcess(0);

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

    // `--picker` opens the theme picker right away (handy for testing and, later, hotkey launches).
    for (const wchar_t* c = GetCommandLineW(); *c; ++c)
        if (c[0] == L'-' && c[1] == L'-' && c[2] == L'p' && c[3] == L'i') { Bus::openPicker(); break; }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(once);
    ExitProcess(0);
}
