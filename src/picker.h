// Theme + background picker, as a native popup.
//
// Everything transient lives only while the popup is open: the window, the
// preview pixel buffer (VirtualAlloc) and WIC itself. Closing it returns the
// process to the idle footprint of the bare bar.
//
// Keys: Up/Down/PgUp/PgDn/Home/End theme (previews live on the bar),
//       Left/Right background, Enter apply, Esc cancel.
#pragma once
#include "sys.h"
#include "theme.h"
#include "render_gdi.h"
#include "platform.h"

template <class Bus, class R, class Themes, class Img, class Wall, class Store>
struct Picker {
    static inline HWND hwnd;
    static inline int sel, top, view, dpi = 96;
    static inline void* pix;
    static inline int pixW, pixH, boxW, boxH;
    static inline bool registered;

    struct Geo {
        int W, H, pad, listW, rowH, rx, rw, titleH, previewY, vis;
    };
    static Geo geo() {
        Geo g;
        g.W = sys::scale(760, dpi);
        g.H = sys::scale(450, dpi);
        g.pad = sys::scale(14, dpi);
        g.listW = sys::scale(210, dpi);
        g.rowH = sys::scale(26, dpi);
        g.rx = g.pad * 2 + g.listW;
        g.rw = g.W - g.rx - g.pad;
        g.titleH = sys::scale(30, dpi);
        g.previewY = g.pad + g.titleH;
        g.vis = (g.H - 2 * g.pad) / g.rowH;
        return g;
    }

    static bool isOpen() { return hwnd != nullptr; }

    static void open(int curDpi) {
        if (hwnd) { SetForegroundWindow(hwnd); return; }
        dpi = curDpi;
        const Geo g = geo();
        if (!registered) {
            WNDCLASSW wc{};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AWPicker";
            RegisterClassW(&wc);
            registered = true;
        }
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int x = wa.left + (wa.right - wa.left - g.W) / 2;
        const int y = wa.top + (wa.bottom - wa.top - g.H) / 2;

        boxW = g.rw;
        boxH = g.rw * 9 / 16;
        pix = VirtualAlloc(nullptr, (SIZE_T)boxW * boxH * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Img::acquire();

        sel = Bus::theme();
        view = 0;
        ensureVisible();
        loadView();
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AWPicker", L"", WS_POPUP, x, y, g.W, g.H,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }

    static void close(bool apply) {
        if (!hwnd) return;
        HWND h = hwnd;
        hwnd = nullptr;  // makes WM_ACTIVATE during destroy a no-op
        if (apply) {
            const int bg = view > 0 ? view - 1 : 0;
            Bus::commit(sel);
            Store::save(Themes::name(sel), bg);
            if (Assets::ok && Themes::bgCount(sel) > 0) {
                wchar_t file[8], path[MAX_PATH];
                wsprintfW(file, L"b%d.jpg", bg);
                Assets::themeFile(path, Themes::name(sel), file);
                Wall::set(path);
            }
        } else {
            Bus::preview(Bus::theme());
        }
        DestroyWindow(h);
        if (pix) { VirtualFree(pix, 0, MEM_RELEASE); pix = nullptr; }
        Img::release();
        Bus::scheduleTrim();
    }

    // -- model ------------------------------------------------------------
    static void ensureVisible() {
        const int vis = geo().vis;
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        if (top < 0) top = 0;
    }
    static int viewCount() { return 1 + Themes::bgCount(sel); }

    static void loadView() {
        pixW = pixH = 0;
        if (!Assets::ok || !pix) return;
        wchar_t file[8], path[MAX_PATH];
        if (view == 0) sys::wcopy(file, L"p.jpg");
        else wsprintfW(file, L"b%d.jpg", view - 1);
        Assets::themeFile(path, Themes::name(sel), file);
        Img::decode(path, boxW, boxH, pix, pixW, pixH);
    }

    static void select(int i) {
        if (i < 0) i = 0;
        if (i >= Themes::count()) i = Themes::count() - 1;
        if (i == sel) return;
        sel = i;
        view = 0;
        ensureVisible();
        Bus::preview(sel);  // recolours the bar and (via g.pal) this popup
        loadView();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    static void setView(int v) {
        const int n = viewCount();
        v = (v % n + n) % n;
        if (v == view) return;
        view = v;
        loadView();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // -- painting ---------------------------------------------------------
    static void paint(HDC dc, int cw, int ch, const Palette& p) {
        const Geo g = geo();
        R r(dc, cw, ch);
        r.fill(0, 0, cw, ch, p.bg());

        // Theme list.
        const int last = sys::imin(Themes::count(), top + g.vis);
        for (int i = top; i < last; ++i) {
            const int y = g.pad + (i - top) * g.rowH;
            const bool on = i == sel;
            if (on) r.fill(g.pad, y, g.listW, g.rowH, p.selected());
            const Palette tp = Themes::palette(i);
            const int d = sys::scale(10, dpi), dy = y + (g.rowH - d) / 2;
            int dx = g.pad + sys::scale(8, dpi);
            r.fill(dx, dy, d, d, tp.bg());
            r.frame(dx, dy, d, d, 1, p.dim());
            r.fill(dx += d + 3, dy, d, d, tp.accent());
            r.fill(dx += d + 3, dy, d, d, tp.fg());
            r.text(dx + d + sys::scale(10, dpi), y, g.rowH, Themes::name(i), F_TEXT, on ? p.accent() : p.fg());
        }

        // Header.
        r.text(g.rx, g.pad, g.titleH, Themes::name(sel), F_TITLE, p.accent());
        const wchar_t* mode = p.light ? L"light" : L"dark";
        r.text(g.rx + g.rw - r.measure(mode, F_SMALL), g.pad, g.titleH, mode, F_SMALL, p.dim());

        // Preview box (letterboxed, centred).
        const int by = g.previewY;
        r.fill(g.rx, by, boxW, boxH, p[gen::C_DARKER_BACKGROUND]);
        if (pixW) r.image(g.rx + (boxW - pixW) / 2, by + (boxH - pixH) / 2, pixW, pixH, pix);
        else r.textCentered(g.rx, by, boxW, boxH, L"no preview", F_TEXT, p.dim());
        r.frame(g.rx, by, boxW, boxH, 1, sys::mix(p.bg(), p.fg(), 20));

        // Palette strip: red..brown, then the bright accents.
        int y = by + boxH + sys::scale(12, dpi);
        const int sq = sys::scale(18, dpi), sg = sys::scale(5, dpi);
        static const Color strip[] = {gen::C_RED, gen::C_ORANGE, gen::C_YELLOW, gen::C_GREEN, gen::C_CYAN,
                                      gen::C_BLUE, gen::C_MAGENTA, gen::C_BROWN, gen::C_BRIGHT_RED,
                                      gen::C_BRIGHT_YELLOW, gen::C_BRIGHT_GREEN, gen::C_BRIGHT_CYAN,
                                      gen::C_BRIGHT_BLUE, gen::C_BRIGHT_MAGENTA};
        for (int i = 0; i < 14; ++i) r.fill(g.rx + i * (sq + sg), y, sq, sq, p[strip[i]]);

        // Background selector.
        y += sq + sys::scale(12, dpi);
        wchar_t line[48];
        const int n = viewCount();
        if (view == 0) wsprintfW(line, L"preview   (%d backgrounds)", n - 1);
        else wsprintfW(line, L"background %d/%d", view, n - 1);
        r.text(g.rx, y, g.rowH, line, F_TEXT, p.fg());
        const int dotR = sys::scale(7, dpi);
        int dx = g.rx + g.rw - n * (dotR + sys::scale(6, dpi));
        for (int i = 0; i < n; ++i, dx += dotR + sys::scale(6, dpi))
            r.fill(dx, y + (g.rowH - dotR) / 2, dotR, dotR, i == view ? p.accent() : sys::mix(p.bg(), p.fg(), 28));

        // Footer.
        const wchar_t* hint = L"\x2191\x2193 theme   \x2190\x2192 background   Enter apply   Esc cancel";
        r.text(g.rx, g.H - g.pad - g.rowH, g.rowH, hint, F_SMALL, p.dim());

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
            case WM_KEYDOWN: {
                const int vis = geo().vis;
                switch (wp) {
                    case VK_UP: select(sel - 1); break;
                    case VK_DOWN: select(sel + 1); break;
                    case VK_PRIOR: select(sel - vis); break;
                    case VK_NEXT: select(sel + vis); break;
                    case VK_HOME: select(0); break;
                    case VK_END: select(Themes::count() - 1); break;
                    case VK_LEFT: setView(view - 1); break;
                    case VK_RIGHT: setView(view + 1); break;
                    case VK_RETURN: close(true); break;
                    case VK_ESCAPE: close(false); break;
                }
                return 0;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
                const Geo g = geo();
                const int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                if (x >= g.pad && x < g.pad + g.listW && y >= g.pad) {
                    const int i = top + (y - g.pad) / g.rowH;
                    if (i < Themes::count()) {
                        select(i);
                        if (msg == WM_LBUTTONDBLCLK) close(true);
                    }
                } else if (x >= g.rx && y >= g.previewY && y < g.previewY + boxH) {
                    setView(view + 1);  // click the preview to flip through backgrounds
                }
                return 0;
            }
            case WM_MOUSEWHEEL:
                select(sel + ((short)HIWORD(wp) > 0 ? -1 : 1));
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE && hwnd) close(false);
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};
