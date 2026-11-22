// Bar panels (Omarchy's popups): one flyout window, immediate-mode contents.
//
// A panel is a type with static functions:
//   void open(HMONITOR)   panel shown (start workers, take a snapshot)
//   void close()          panel hidden (stop workers, free what open took)
//   bool tick()           once a second while shown; true = repaint
//   void ui(Ui&)          lay out, paint and handle input, top to bottom
// ui() runs for every paint and for every mouse event (on a 1x1 off-screen surface), so layout,
// drawing and hit testing can never disagree, and no widget tree is kept anywhere.
// Only one panel is open at a time; the flyout and everything a panel holds go away on close.
#pragma once
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "tiler.h"  // tileutil::forceForeground

struct Ui {
    GdiRenderer& r;
    const Palette& p;
    int dpi, x, w, y;   // content box (x, w) and layout cursor (y)
    int mx, my;         // mouse in client coordinates, -1 when outside
    bool click, press, held;
    int id = 0;
    bool wantClose = false;
    static inline int drag;  // id of the slider being dragged, 0 for none

    int s(int v) const { return sys::scale(v, dpi); }
    bool in(int X, int Y, int W, int H) const { return mx >= X && mx < X + W && my >= Y && my < Y + H; }
    bool hit(int X, int Y, int W, int H) const { return click && in(X, Y, W, H); }
    COLORREF line() const { return sys::mix(p.bg(), p.fg(), 16); }
    void gap(int v) { y += s(v); }

    // Panel header: icon, bold title, spaced-caps subtitle; optional icon buttons and a switch at
    // the right. Returns 1 for the switch, 2 + k for button k, 0 otherwise.
    int hero(const wchar_t* glyph, Font gf, COLORREF gc, const wchar_t* title, const wchar_t* meta, int toggle = -1,
             const wchar_t* const* btn = nullptr, int nb = 0) {
        const int h = s(42), ix = s(36);
        r.textCentered(x, y, ix, h, glyph, gf, gc);
        int right = x + w, res = 0;
        if (toggle >= 0) {
            const int tw = s(32), th = s(18), tx = right - tw, ty = y + (h - th) / 2;
            r.round(tx, ty, tw, th, s(6), toggle ? sys::mix(p.bg(), p.fg(), 22) : sys::mix(p.bg(), p.fg(), 10),
                    CLR_NONE);
            const int k = th - s(6);
            r.round(toggle ? tx + tw - k - s(3) : tx + s(3), ty + s(3), k, k, s(3),
                    toggle ? p.fg() : p.dim(), CLR_NONE);
            if (hit(tx - s(4), ty - s(4), tw + s(8), th + s(8))) res = 1;
            right = tx - s(10);
        }
        for (int i = nb - 1; i >= 0; --i) {
            const int bw = s(28), bx = right - bw;
            const bool hv = in(bx, y + s(7), bw, h - s(14));
            if (hv) r.round(bx, y + s(7), bw, h - s(14), s(6), p.hover(), CLR_NONE);
            r.textCentered(bx, y, bw, h, btn[i], F_ICON, hv ? p.accentText() : p.fg());
            if (hit(bx, y + s(7), bw, h - s(14))) res = 2 + i;
            right = bx - s(4);
        }
        const int tx = x + ix + s(8);
        r.text(tx, y + s(2), s(22), title, F_BOLD, p.fg());
        if (meta) r.caption(tx, y + s(22), s(16), meta, p.dim(), s(1));
        y += h;
        return res;
    }
    void sep() { y += s(10); r.fill(x, y, w, 1, line()); y += s(11); }
    void section(const wchar_t* label, const wchar_t* right = nullptr) {
        const int h = s(16);
        r.caption(x, y, h, label, p.dim(), s(1));
        if (right) r.caption(x + w - r.measureCaption(right, s(1)), y, h, right, p.dim(), s(1));
        y += h + s(8);
    }
    // Two key/value pairs side by side (the network stats grid).
    void stat(const wchar_t* k1, const wchar_t* v1, const wchar_t* k2, const wchar_t* v2) {
        const int h = s(19), cw = (w - s(16)) / 2;
        const wchar_t* k[2] = {k1, k2};
        const wchar_t* v[2] = {v1, v2};
        for (int i = 0; i < 2; ++i) {
            if (!k[i]) continue;
            const int cx = x + i * (cw + s(16));
            r.text(cx, y, h, k[i], F_TEXT, p.dim());
            r.text(cx + cw - r.measure(v[i], F_TEXT), y, h, v[i], F_TEXT, p.fg());
        }
        y += h;
    }
    // Horizontal slider over [0, max]. ticks > 1 draws Omarchy's segmented track and snaps to the
    // segments. Returns the new value while it is dragged/clicked, else -1.
    int slider(int v, int max, int ticks = 0) {
        const int me = ++id, h = s(24), kn = s(12), tx = x + kn / 2, tw = w - kn, cy = y + h / 2;
        int res = -1;
        if (press && in(x, y, w, h)) drag = me;
        if (drag == me && (held || click)) {
            int nv = (int)((long long)(mx - tx) * max / (tw > 0 ? tw : 1));
            if (nv < 0) nv = 0;
            if (nv > max) nv = max;
            if (ticks > 1) { const int st = max / (ticks - 1); nv = (nv + st / 2) / st * st; }
            if (nv != v) { res = nv; v = nv; }
        }
        if (drag == me && !held) drag = 0;
        const int th = s(4), fx = tx + (int)((long long)tw * v / (max ? max : 1));
        r.round(tx, cy - th / 2, tw, th, th, sys::mix(p.bg(), p.fg(), 16), CLR_NONE);
        r.round(tx, cy - th / 2, fx - tx, th, th, p.fg(), CLR_NONE);
        for (int i = 1; ticks > 1 && i < ticks - 1; ++i)
            r.fill(tx + tw * i / (ticks - 1) - s(1), cy - th, s(2), th * 2, p.bg());
        r.ellipse(fx - kn / 2, cy - kn / 2, kn, p.fg());
        y += h;
        return res;
    }
    // A row of equal boxed choices (DNS provider, scale). Returns the index clicked or -1.
    int buttons(const wchar_t* const* labels, int n, int sel) {
        const int h = s(30), g = s(6), bw = (w - g * (n - 1)) / n;
        int res = -1;
        for (int i = 0; i < n; ++i) {
            const int bx = x + i * (bw + g);
            const bool on = i == sel, hv = in(bx, y, bw, h);
            r.round(bx, y, bw, h, s(4), on ? p.selected() : hv ? p.hover() : p.idleFill(), on ? p.selected() : hv ? p.hoverBorder() : p.idleBorder());
            r.textCentered(bx, y, bw, h, labels[i], F_TEXT, p.fg());
            if (hit(bx, y, bw, h)) res = i;
        }
        y += h;
        return res;
    }
    // A list row: glyph, label, optional second line, optional trailing glyph.
    // Returns 0, 1 for a click on the row, 2 for a click on the trailing glyph.
    int row(const wchar_t* glyph, const wchar_t* label, const wchar_t* sub = nullptr, bool selected = false,
            const wchar_t* trail = nullptr) {
        const int h = sub ? s(42) : s(32);
        const bool hv = in(x, y, w, h);
        if (selected || hv) r.fill(x, y, w, h, selected ? p.selected() : p.hover());
        int tx = x + s(10);
        if (glyph) { r.textCentered(tx, y, s(20), h, glyph, F_ICON, p.fg()); tx += s(32); }
        if (sub) {
            r.text(tx, y + s(4), s(18), label, F_TEXT, p.fg());
            r.text(tx, y + s(21), s(16), sub, F_SMALL, p.dim());
        } else r.text(tx, y, h, label, F_TEXT, p.fg());
        const bool onTrail = trail && in(x + w - s(36), y, s(36), h);
        if (trail) r.textCentered(x + w - s(30), y, s(20), h, trail, F_ICON, onTrail ? p.accentText() : p.dim());
        const int c = hit(x, y, w, h) ? (onTrail ? 2 : 1) : 0;
        y += h;
        return c;
    }
    // Boxed, word-wrapped message (errors, hints). Returns true when clicked.
    bool notice(const wchar_t* text, COLORREF border) {
        const int pd = s(10), th = r.wrap(x + pd, y + pd, w - 2 * pd, text, F_TEXT, p.dim(), false), h = th + 2 * pd;
        r.round(x, y, w, h, s(4), sys::mix(p.bg(), border, 8), sys::mix(p.bg(), border, 55));
        r.wrap(x + pd, y + pd, w - 2 * pd, text, F_TEXT, p.dim());
        const bool c = hit(x, y, w, h);
        y += h;
        return c;
    }
    // Labelled meter (tokens by day).
    void meter(const wchar_t* label, long long v, long long max, const wchar_t* value, bool strong) {
        const int h = s(24), lw = s(64), vw = s(56), tx = x + lw, tw = w - lw - vw, th = s(4), cy = y + h / 2;
        r.text(x, y, h, label, strong ? F_BOLD : F_TEXT, strong ? p.fg() : p.dim());
        r.round(tx, cy - th / 2, tw, th, th, sys::mix(p.bg(), p.fg(), 16), CLR_NONE);
        if (max > 0 && v > 0) r.round(tx, cy - th / 2, sys::imax(th, (int)(tw * v / max)), th, th, p.fg(), CLR_NONE);
        r.text(x + w - r.measure(value, F_TEXT), y, h, value, strong ? F_BOLD : F_TEXT,
               p.fg());
        y += h;
    }
    // A filled key/value strip (tokens by model).
    void strip(const wchar_t* k, const wchar_t* v) {
        const int h = s(26);
        r.fill(x, y, w, h, p.selected());
        r.text(x + s(8), y, h, k, F_TEXT, p.fg());
        r.text(x + w - s(8) - r.measure(v, F_TEXT), y, h, v, F_TEXT, p.dim());
        y += h + s(4);
    }
    // Tall glyph buttons with a caption (lock, sleep, restart, ...). Returns the index clicked or -1.
    int glyphButtons(const wchar_t* const* glyph, const wchar_t* const* label, int n) {
        const int h = s(58), g = s(6), bw = (w - g * (n - 1)) / n;
        int res = -1;
        for (int i = 0; i < n; ++i) {
            const int bx = x + i * (bw + g);
            const bool hv = in(bx, y, bw, h);
            r.round(bx, y, bw, h, s(4), hv ? p.hover() : p.idleFill(), hv ? p.hoverBorder() : p.idleBorder());
            r.textCentered(bx, y + s(6), bw, s(28), glyph[i], F_BIGICON, hv ? p.accentText() : p.fg());
            r.textCentered(bx, y + s(34), bw, s(16), label[i], F_SMALL, hv ? p.fg() : p.dim());
            if (hit(bx, y, bw, h)) res = i;
        }
        y += h;
        return res;
    }
};

// Human-readable sizes and rates without the CRT.
namespace fmt {
// 1 decimal: 525.9 MB, 1.1 KB
inline int bytes(wchar_t* o, unsigned long long v, const wchar_t* suffix = L"") {
    static const wchar_t* const u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int k = 0;
    unsigned long long whole = v, tenth = 0;
    while (whole >= 1024 && k < 4) { tenth = (whole % 1024) * 10 / 1024; whole /= 1024; ++k; }
    return k ? wsprintfW(o, L"%u.%u %s%s", (unsigned)whole, (unsigned)tenth, u[k], suffix)
             : wsprintfW(o, L"%u %s%s", (unsigned)whole, u[k], suffix);
}
// 33.0M, 812.4K, 950
inline int count(wchar_t* o, unsigned long long v) {
    if (v >= 1000000000ull) return wsprintfW(o, L"%u.%uB", (unsigned)(v / 1000000000ull), (unsigned)(v / 100000000ull % 10));
    if (v >= 1000000ull) return wsprintfW(o, L"%u.%uM", (unsigned)(v / 1000000ull), (unsigned)(v / 100000ull % 10));
    if (v >= 1000ull) return wsprintfW(o, L"%u.%uK", (unsigned)(v / 1000ull), (unsigned)(v / 100ull % 10));
    return wsprintfW(o, L"%u", (unsigned)v);
}
inline void upper(wchar_t* s) { for (; *s; ++s) if (*s >= L'a' && *s <= L'z') *s -= 32; }
}  // namespace fmt

// The flyout window: placed under the bar widget that opened it, on that bar's monitor.
template <class Bus, class... Ps>
struct Flyout {
    static constexpr int kTick = 1;
    static inline HWND hwnd;
    static inline bool registered;
    static inline int cur = -1, dpi = 96, mx = -1, my = -1, height;
    static inline bool held, tracking;
    static inline HMONITOR mon;
    static inline RECT anchor;
    static inline DWORD openedAt;

    static int width() { return sys::scale(370, dpi); }
    static int pad() { return sys::scale(14, dpi); }

    static void callOpen() { int i = 0; ((i++ == cur ? Ps::open(mon) : void()), ...); }
    static void callClose() { int i = 0; ((i++ == cur ? Ps::close() : void()), ...); }
    static bool callTick() { int i = 0; bool r = false; ((i++ == cur ? (void)(r = Ps::tick()) : void()), ...); return r; }
    static void callUi(Ui& u) { int i = 0; ((i++ == cur ? Ps::ui(u) : void()), ...); }

    // One pass over the panel's ui(); returns the content height. Mouse events run it on a 1x1
    // surface so the panel can react, then the window is repainted.
    static int pass(GdiRenderer& r, bool click, bool press, bool* close) {
        const int pd = pad();
        Ui u{r, Bus::palette(), dpi, pd, width() - 2 * pd, pd, mx, my, click, press, held};
        callUi(u);
        if (close) *close = u.wantClose;
        return u.y + pd;
    }
    static void event(bool click, bool press) {
        HDC mem = CreateCompatibleDC(nullptr);
        bool cl = false;
        { GdiRenderer r(mem, 1, 1); pass(r, click, press, &cl); }
        DeleteDC(mem);
        if (cl) { close(); return; }
        relayout();
    }
    static void relayout() {
        if (!hwnd) return;
        HDC mem = CreateCompatibleDC(nullptr);
        int h;
        { GdiRenderer r(mem, 1, 1); const int sx = mx; mx = my = -1; h = pass(r, false, false, nullptr); mx = sx; }
        DeleteDC(mem);
        if (h != height) { height = h; place(); }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    static void place() {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
        const RECT wa = mi.rcWork;
        const int w = width(), g = sys::scale(6, dpi);
        int x = (anchor.left + anchor.right) / 2 - w / 2;  // centred under the widget...
        if (anchor.right - wa.left > (wa.right - wa.left) * 2 / 3) x = anchor.right - w;  // ...right-aligned on the right
        if (x > wa.right - g - w) x = wa.right - g - w;
        if (x < wa.left + g) x = wa.left + g;
        int h = height, maxH = wa.bottom - wa.top - 2 * g;
        if (h > maxH) h = maxH;
        SetWindowPos(hwnd, HWND_TOPMOST, x, wa.top + g, w, h, SWP_NOACTIVATE);
    }

    static bool isOpen(int which) { return hwnd && cur == which; }

    // Clicking the widget of the open panel closes it; another widget switches panels.
    static void toggle(int which, HMONITOR m, RECT a, int curDpi) {
        if (hwnd && cur == which) { close(); return; }
        close();
        dpi = curDpi;
        mon = m;
        anchor = a;
        cur = which;
        mx = my = -1;
        held = false;
        Ui::drag = 0;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AWPanel";
            RegisterClassW(&wc);
            registered = true;
        }
        callOpen();
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AWPanel", L"", WS_POPUP, 0, 0, width(), 10, nullptr,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) { callClose(); cur = -1; return; }
        height = 0;
        relayout();
        openedAt = GetTickCount();
        ShowWindow(hwnd, SW_SHOW);
        tileutil::forceForeground(hwnd);
        SetTimer(hwnd, kTick, 1000, nullptr);
    }
    static void close() {
        if (!hwnd) return;
        HWND h = hwnd;
        hwnd = nullptr;
        callClose();
        cur = -1;
        DestroyWindow(h);
        Bus::scheduleTrim();
    }
    static void repaint() { if (hwnd) relayout(); }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT rc;
                GetClientRect(h, &rc);
                {
                    GdiRenderer r(dc, rc.right, rc.bottom);
                    const Palette& p = Bus::palette();
                    r.fill(0, 0, rc.right, rc.bottom, p.bg());
                    pass(r, false, false, nullptr);
                    r.frame(0, 0, rc.right, rc.bottom, sys::scale(2, dpi), p.accent());
                }
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
                if (wp == kTick && hwnd && callTick()) relayout();
                return 0;
            case WM_MOUSEMOVE:
                if (!tracking) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); tracking = true; }
                mx = (short)LOWORD(lp);
                my = (short)HIWORD(lp);
                held = (wp & MK_LBUTTON) != 0;
                if (held && Ui::drag) event(false, false); else InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_MOUSELEAVE:
                tracking = false;
                if (!held) { mx = my = -1; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONDOWN:
                SetCapture(h);
                mx = (short)LOWORD(lp);
                my = (short)HIWORD(lp);
                held = true;
                event(false, true);
                return 0;
            case WM_LBUTTONUP:
                ReleaseCapture();
                mx = (short)LOWORD(lp);
                my = (short)HIWORD(lp);
                held = false;
                event(true, false);
                Ui::drag = 0;
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) close();
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && hwnd) {
                    if (GetTickCount() - openedAt < 500) tileutil::forceForeground(hwnd);
                    else close();
                }
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};
