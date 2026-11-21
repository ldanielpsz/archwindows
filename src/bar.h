// The bar and its widgets.
//
// Bar<Bus, R, Widgets...> derives from every widget, so each widget is a plain
// value inside the bar: no vtables, no heap, no registry of callbacks. The
// bar walks the pack with fold expressions, which the compiler unrolls.
//
// A BarWidget provides (checked by the concept below):
//   static constexpr Slot slot;
//   int    width(R&, const Metrics&);             // 0 hides the widget
//   void   paint(R&, x, y, w, h, const Palette&, bool hover, const Metrics&);
//   Action click(int button, int relX, int width, const Metrics&);  // button: 1 left 2 right 3 middle
//   (optional) Action scroll(int dir, const Metrics&);
//
// There is one Bar per monitor (Metrics::mon); each reserves the top strip of its own monitor.
// Shell-wide work (hotkeys, timers, display changes) lives in the shell's hidden controller
// window, not here, so bars can come and go with monitors.
#pragma once
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "keys.h"
#include <shellapi.h>

enum class Slot : u8 { Left, Center, Right };
enum Action : u8 { A_NONE, A_REPAINT, A_PICKER, A_QUIT };

struct Metrics {
    int dpi, h, pad, gap;
    HMONITOR mon;  // the monitor this bar sits on
    int s(int v) const { return sys::scale(v, dpi); }
};

// ---------------------------------------------------------------- widgets --

// Workspace model: per monitor, since every monitor shows its own workspace.
// This stand-in is a fixed set of five; the shell feeds the real one from the tiler.
struct StubWorkspaces {
    static inline int act;
    static constexpr bool visible() { return true; }
    static constexpr int count(HMONITOR) { return 5; }
    static int active(HMONITOR) { return act; }
    static void focus(HMONITOR, int i) { act = i; }
};

struct LogoW {
    static constexpr Slot slot = Slot::Left;
    template <class R> int width(R&, const Metrics& m) { return m.h; }
    template <class R>
    void paint(R& r, int x, int y, int w, int h, const Palette& p, bool hover, const Metrics&) {
        if (hover) r.fill(x, y, w, h, p.hover());
        r.textCentered(x, y, w, h, Fonts::logoGlyph, F_LOGO, p.accent());
    }
    Action click(int b, int, int, const Metrics&) { return b == 1 ? A_PICKER : A_NONE; }
};

template <class Model>
struct WorkspacesW {
    static constexpr Slot slot = Slot::Left;
    static int cell(const Metrics& m) { return m.s(22); }
    template <class R> int width(R&, const Metrics& m) { return Model::visible() ? Model::count(m.mon) * cell(m) : 0; }
    template <class R>
    void paint(R& r, int x, int y, int, int h, const Palette& p, bool, const Metrics& m) {
        const int c = cell(m);
        const int n_ = Model::count(m.mon), act = Model::active(m.mon);
        for (int i = 0; i < n_; ++i) {
            wchar_t n[4];
            wsprintfW(n, L"%d", i + 1);
            const bool on = i == act;
            r.textCentered(x + i * c, y, c, h, n, F_TEXT, on ? p[gen::C_BRIGHT_FOREGROUND] : p.dim());
            if (on) r.fill(x + i * c + m.s(4), y + h - m.s(3), c - m.s(8), m.s(2), p.accent());
        }
    }
    Action click(int b, int relX, int, const Metrics& m) {
        if (b != 1) return A_NONE;
        int i = relX / cell(m);
        if (i < 0 || i >= Model::count(m.mon)) return A_NONE;
        Model::focus(m.mon, i);
        return A_REPAINT;
    }
    Action scroll(int dir, const Metrics& m) {
        int n = Model::active(m.mon) + dir;
        if (n < 0 || n >= Model::count(m.mon)) return A_NONE;
        Model::focus(m.mon, n);
        return A_REPAINT;
    }
};

struct ClockW {
    static constexpr Slot slot = Slot::Center;
    int fmt = 0;
    static constexpr int kFormats = 3;

    int format(wchar_t* o) {
        static const wchar_t* const dn[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        static const wchar_t* const mn[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                            L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        SYSTEMTIME t;
        GetLocalTime(&t);
        switch (fmt) {
            case 0: return wsprintfW(o, L"%02d:%02d", t.wHour, t.wMinute);
            case 1: return wsprintfW(o, L"%s %d %s  %02d:%02d", dn[t.wDayOfWeek], t.wDay, mn[t.wMonth - 1], t.wHour, t.wMinute);
            default: return wsprintfW(o, L"%04d-%02d-%02d  %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        }
    }
    template <class R> int width(R& r, const Metrics& m) {
        wchar_t b[40];
        int n = format(b);
        return r.measure(b, F_TEXT, n) + m.s(16);
    }
    template <class R>
    void paint(R& r, int x, int y, int w, int h, const Palette& p, bool hover, const Metrics&) {
        wchar_t b[40];
        int n = format(b);
        if (hover) r.fill(x, y, w, h, p.hover());
        r.textCentered(x, y, w, h, b, F_TEXT, p[gen::C_BRIGHT_FOREGROUND], n);
    }
    Action click(int b, int, int, const Metrics&) {
        if (b == 2 || b == 1) { fmt = (fmt + 1) % kFormats; return A_REPAINT; }
        return A_NONE;
    }
};

// Placeholder status glyph. Real behaviour (network, bluetooth, audio panels)
// arrives with the Windows-specific session.
template <wchar_t Glyph>
struct IconW {
    static constexpr Slot slot = Slot::Right;
    template <class R> int width(R&, const Metrics& m) { return m.s(24); }
    template <class R>
    void paint(R& r, int x, int y, int w, int h, const Palette& p, bool hover, const Metrics&) {
        if (hover) r.fill(x, y, w, h, p.hover());
        wchar_t g[2] = {Glyph, 0};
        r.textCentered(x, y, w, h, g, F_ICON, p.fg());
    }
    Action click(int, int, int, const Metrics&) { return A_NONE; }
};

struct BatteryW {
    static constexpr Slot slot = Slot::Right;
    int pct = -1;

    void refresh() {
        SYSTEM_POWER_STATUS s;
        pct = (GetSystemPowerStatus(&s) && !(s.BatteryFlag & 128) && s.BatteryLifePercent <= 100)
                  ? s.BatteryLifePercent : -1;
    }
    template <class R> int width(R& r, const Metrics& m) {
        refresh();
        if (pct < 0) return 0;  // no battery: hide
        wchar_t b[8];
        return m.s(24) + r.measure(b, F_SMALL, wsprintfW(b, L"%d%%", pct)) + m.s(4);
    }
    template <class R>
    void paint(R& r, int x, int y, int w, int h, const Palette& p, bool hover, const Metrics& m) {
        if (hover) r.fill(x, y, w, h, p.hover());
        r.textCentered(x, y, m.s(24), h, L"\xE83F", F_ICON, pct <= 15 ? p[gen::C_RED] : p.fg());
        wchar_t b[8];
        int n = wsprintfW(b, L"%d%%", pct);
        r.text(x + m.s(24), y, h, b, F_SMALL, p.fg(), n);
    }
    Action click(int, int, int, const Metrics&) { return A_NONE; }
};

// Right-click quits (UI-only build has no other way to stop the shell).
struct PowerW {
    static constexpr Slot slot = Slot::Right;
    template <class R> int width(R&, const Metrics& m) { return m.s(26); }
    template <class R>
    void paint(R& r, int x, int y, int w, int h, const Palette& p, bool hover, const Metrics&) {
        if (hover) r.fill(x, y, w, h, p.hover());
        r.textCentered(x, y, w, h, L"\xE7E8", F_ICON, p.fg());
    }
    Action click(int b, int, int, const Metrics&) { return b == 2 ? A_QUIT : A_NONE; }
};

// -------------------------------------------------------------------- bar --

template <class W, class R>
concept BarWidget = requires(W w, R& r, const Metrics& m, const Palette& p) {
    W::slot;
    w.width(r, m);
    w.paint(r, 0, 0, 0, 0, p, false, m);
    w.click(1, 0, 0, m);
};

template <class Bus, class R, class... Ws>
struct Bar : Ws... {
    static_assert((BarWidget<Ws, R> && ...), "widget does not satisfy BarWidget");
    static constexpr int N = sizeof...(Ws);
    static constexpr int kClockTimer = 1;

    HWND hwnd = nullptr;
    Metrics m{96, 26, 10, 4, nullptr};
    int hover = -1;
    bool tracking = false;
    int px[N] = {}, pw[N] = {};

    // Layout: left group packs from the left edge, right group from the right,
    // center group is centred. Widths are cached for hit testing.
    void layout(R& r, int totalW) {
        int sum[3] = {0, 0, 0}, cnt[3] = {0, 0, 0}, i = 0;
        ((pw[i] = static_cast<Ws&>(*this).width(r, m),
          pw[i] ? (sum[(int)Ws::slot] += pw[i], ++cnt[(int)Ws::slot]) : 0, ++i), ...);
        auto span = [&](int s) { return sum[s] + (cnt[s] > 1 ? (cnt[s] - 1) * m.gap : 0); };
        int start[3] = {m.pad, (totalW - span(1)) / 2, totalW - m.pad - span(2)};
        i = 0;
        ((px[i] = pw[i] ? start[(int)Ws::slot] : 0,
          start[(int)Ws::slot] += pw[i] ? pw[i] + m.gap : 0, ++i), ...);
    }

    void paint(HDC dc, int w, int h, const Palette& p) {
        R r(dc, w, h);
        r.fill(0, 0, w, h, p.bg());
        layout(r, w);
        int i = 0;
        ((pw[i] ? static_cast<Ws&>(*this).paint(r, px[i], 0, pw[i], h, p, hover == i, m) : void(), ++i), ...);
        r.fill(0, h - 1, w, 1, sys::mix(p.bg(), p.fg(), 12));
    }

    int hit(int x) const {
        for (int i = 0; i < N; ++i)
            if (pw[i] && x >= px[i] && x < px[i] + pw[i]) return i;
        return -1;
    }

    // Dispatch to widget index `idx` (runtime index over a compile-time pack).
    Action click(int idx, int button, int x) {
        int i = 0;
        Action a = A_NONE;
        ((i++ == idx ? (a = static_cast<Ws&>(*this).click(button, x - px[idx], pw[idx], m)) : A_NONE), ...);
        return a;
    }
    Action scroll(int idx, int dir) {
        int i = 0;
        Action a = A_NONE;
        ((i++ == idx ? (a = scrollOne(static_cast<Ws&>(*this), dir, m)) : A_NONE), ...);
        return a;
    }
    template <class W> static Action scrollOne(W& w, int dir, const Metrics& m) {
        if constexpr (requires { w.scroll(dir, m); }) return w.scroll(dir, m);
        else return A_NONE;
    }

    void act(Action a) {
        if (a == A_REPAINT) InvalidateRect(hwnd, nullptr, FALSE);
        else if (a == A_PICKER) Bus::openPicker();
        else if (a == A_QUIT) PostQuitMessage(0);
    }

    void armClock() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        SetTimer(hwnd, kClockTimer, 60000 - t.wSecond * 1000 - t.wMilliseconds + 20, nullptr);
    }

    // Reserve the strip at the top of this bar's monitor as an app bar: maximised windows,
    // snapping and the tiler's work area all start below it. Windows removes the reservation
    // by itself if this process dies.
    bool reserved = false;
    void place() {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(m.mon, &mi)) return;  // monitor gone; the shell rebuilds the bars
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = hwnd;
        abd.uCallbackMessage = WM_APP;
        if (!reserved) { SHAppBarMessage(ABM_NEW, &abd); reserved = true; }
        abd.uEdge = ABE_TOP;
        abd.rc = mi.rcMonitor;
        abd.rc.bottom = abd.rc.top + m.h;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + m.h;
        SHAppBarMessage(ABM_SETPOS, &abd);
        SetWindowPos(hwnd, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, m.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    void unreserve() {
        if (!reserved) return;
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = hwnd;
        SHAppBarMessage(ABM_REMOVE, &abd);
        reserved = false;
    }

    void setDpi(int dpi) {
        m.dpi = dpi;
        m.h = sys::scale(26, dpi);
        m.pad = sys::scale(10, dpi);
        m.gap = sys::scale(4, dpi);
    }

    LRESULT proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, const Palette& pal) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT rc;
                GetClientRect(h, &rc);
                paint(dc, rc.right, rc.bottom, pal);
                EndPaint(h, &ps);
                Bus::scheduleTrim();
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_MOUSEMOVE: {
                if (!tracking) {
                    TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                    TrackMouseEvent(&t);
                    tracking = true;
                }
                int i = hit((short)LOWORD(lp));
                if (i != hover) { hover = i; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking = false;
                if (hover != -1) { hover = -1; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
                int i = hit((short)LOWORD(lp));
                if (i >= 0) act(click(i, msg == WM_LBUTTONUP ? 1 : msg == WM_RBUTTONUP ? 2 : 3, (short)LOWORD(lp)));
                return 0;
            }
            case WM_MOUSEWHEEL: {
                POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
                ScreenToClient(h, &pt);
                int i = hit(pt.x);
                if (i >= 0) act(scroll(i, (short)HIWORD(wp) > 0 ? -1 : 1));
                return 0;
            }
            case WM_TIMER:
                if (wp == kClockTimer) { KillTimer(h, kClockTimer); armClock(); InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_APP:  // app bar notifications
                if (wp == ABN_POSCHANGED) place();
                else if (wp == ABN_FULLSCREENAPP)
                    SetWindowPos(h, lp ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            // Fonts are shared by every bar, so the shell decides (only the primary monitor's
            // scale changes them). Never take the suggested rect: place() sizes the bar.
            case WM_DPICHANGED: Bus::monitorDpiChanged(m.mon, HIWORD(wp)); return 0;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};
