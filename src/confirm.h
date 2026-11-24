// Themed yes/no prompt (sign out, restart, shut down, forget a Bluetooth device), drawn like the
// Win+Space menu instead of Windows' MessageBox, which ignores the theme.
//
// Not modal: ask() shows the prompt and returns at once; the action runs only if it is confirmed.
// The callers sit inside a panel's or the menu's input handling, which closes its own window on
// the click, so a nested message loop there would run under a window being destroyed.
//
// Keys: Enter answers with the focused button (the action, at first), Esc cancels, Tab / arrows
// move between the buttons. Clicking elsewhere cancels, as it closes the menu.
#pragma once
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "tiler.h"  // tileutil::forceForeground

struct Confirm {
    using Fn = void (*)(void*);
    // Set by the shell: the live palette, and the scale the shared fonts are built for.
    static inline const Palette& (*palette)() = nullptr;
    static inline int dpi = 96;

    static inline HWND hwnd;
    static inline bool registered, tracking;
    static inline int focus, hot = -1;  // button: 0 the action, 1 cancel
    static inline DWORD openedAt;
    static inline wchar_t glyph[4], title[48], text[320], yes[32];
    static inline Fn fn;
    static inline void* arg;

    static void copy(wchar_t* d, const wchar_t* s, int n) {
        int i = 0;
        for (; s && s[i] && i < n - 1; ++i) d[i] = s[i];
        d[i] = 0;
    }
    static int s(int v) { return sys::scale(v, dpi); }
    static int width() { return s(420); }
    static int pad() { return s(18); }
    static int btnH() { return s(32); }
    static int btnW(GdiRenderer& r, const wchar_t* t) { return sys::imax(s(96), r.measure(t, F_TEXT) + s(32)); }

    // The two buttons, right-aligned at the bottom: the action, then cancel at the far right.
    static void buttons(GdiRenderer& r, int ch, RECT* b) {
        const int pd = pad(), h = btnH(), y = ch - pd - h;
        const int wc = btnW(r, L"Cancel"), wy = btnW(r, yes);
        b[1] = {width() - pd - wc, y, width() - pd, y + h};
        b[0] = {b[1].left - s(8) - wy, y, b[1].left - s(8), y + h};
    }
    static int textTop() { return pad() + s(42) + s(6); }
    static int height(GdiRenderer& r) {
        const int pd = pad(), th = r.wrap(pd, 0, width() - 2 * pd, text, F_TEXT, 0, false);
        return textTop() + th + s(20) + btnH() + pd;
    }

    static void paint(GdiRenderer& r, int cw, int ch) {
        const Palette& p = palette();
        const int pd = pad();
        r.fill(0, 0, cw, ch, p.bg());
        // Header: glyph and bold title, as the bar panels' hero row.
        const int hh = s(42);
        r.textCentered(pd, pd, s(36), hh, glyph, F_BIGICON, p.accentText());
        r.text(pd + s(44), pd, hh, title, F_BOLD, p.fg());
        r.wrap(pd, textTop(), cw - 2 * pd, text, F_TEXT, p.dim());
        RECT b[2];
        buttons(r, ch, b);
        const wchar_t* lab[2] = {yes, L"Cancel"};
        for (int i = 0; i < 2; ++i) {
            const RECT& k = b[i];
            const bool on = i == focus, hv = i == hot;
            // Focused: the menu's selected row (8% fill, 25% outline, accent text); hover lifts idle.
            r.round(k.left, k.top, k.right - k.left, k.bottom - k.top, s(4),
                    on ? p.menuSelected() : hv ? p.hover() : p.idleFill(),
                    on ? p.accent() : hv ? p.hoverBorder() : p.idleBorder());
            r.textCentered(k.left, k.top, k.right - k.left, k.bottom - k.top, lab[i], F_TEXT,
                           on ? p.accentText() : p.fg());
        }
        r.frame(0, 0, cw, ch, s(2), p.menuBorder());
    }

    static int hitButton(int x, int y) {
        HDC mem = CreateCompatibleDC(nullptr);
        RECT rc, b[2];
        GetClientRect(hwnd, &rc);
        { GdiRenderer r(mem, 1, 1); buttons(r, rc.bottom, b); }
        DeleteDC(mem);
        const POINT pt{x, y};
        for (int i = 0; i < 2; ++i) if (PtInRect(&b[i], pt)) return i;
        return -1;
    }

    // Show the prompt on the monitor under the cursor; `run(a)` once it is confirmed.
    static void ask(const wchar_t* g, const wchar_t* t, const wchar_t* body, const wchar_t* action, Fn run, void* a) {
        close();
        if (!palette) return;
        copy(glyph, g, 4); copy(title, t, 48); copy(text, body, 320); copy(yes, action, 32);
        fn = run;
        arg = a;
        focus = 0;
        hot = -1;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AWConfirm";
            RegisterClassW(&wc);
            registered = true;
        }
        HDC mem = CreateCompatibleDC(nullptr);
        int h;
        { GdiRenderer r(mem, 1, 1); h = height(r); }
        DeleteDC(mem);
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi))
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
        const RECT& wa = mi.rcWork;
        const int w = width();
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AWConfirm", title, WS_POPUP,
                               wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 3, w, h,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return;
        openedAt = GetTickCount();
        ShowWindow(hwnd, SW_SHOW);
        tileutil::forceForeground(hwnd);
    }

    static void close() {
        if (!hwnd) return;
        HWND h = hwnd;
        hwnd = nullptr;
        DestroyWindow(h);
    }
    static void answer(bool ok) {
        const Fn f = fn;
        void* a = arg;
        fn = nullptr;
        close();
        if (ok && f) f(a);
    }

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                RECT rc;
                GetClientRect(h, &rc);
                { GdiRenderer r(dc, rc.right, rc.bottom); paint(r, rc.right, rc.bottom); }
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_MOUSEMOVE: {
                if (!tracking) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); tracking = true; }
                const int k = hitButton((short)LOWORD(lp), (short)HIWORD(lp));
                if (k != hot) { hot = k; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking = false;
                if (hot >= 0) { hot = -1; InvalidateRect(h, nullptr, FALSE); }
                return 0;
            case WM_LBUTTONUP: {
                const int k = hitButton((short)LOWORD(lp), (short)HIWORD(lp));
                if (k >= 0) answer(k == 0);
                return 0;
            }
            case WM_KEYDOWN:
                switch (wp) {
                    case VK_ESCAPE: answer(false); break;
                    case VK_RETURN: case VK_SPACE: answer(focus == 0); break;
                    case VK_TAB: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                        focus ^= 1;
                        InvalidateRect(h, nullptr, FALSE);
                        break;
                }
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && hwnd) {
                    if (GetTickCount() - openedAt < 500) tileutil::forceForeground(hwnd);
                    else answer(false);
                }
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};
