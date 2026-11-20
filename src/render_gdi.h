// Render policy: plain GDI. Chosen over Direct2D/DirectWrite because it keeps
// the process at a few MB private working set and loads no extra DLLs.
// A RenderPolicy provides:
//   R(HDC target, int w, int h)   double-buffered frame, presented in ~R()
//   fill / frame / text / textCentered / measure / image
// A Direct2D policy can replace this without touching bar or picker code.
#pragma once
#include "sys.h"

enum Font : u8 { F_TEXT, F_SMALL, F_TITLE, F_ICON, F_LOGO, F_COUNT };

struct Fonts {
    static inline HFONT f[F_COUNT];
    static inline int h[F_COUNT];
    // The bar's logo is one glyph of one font. Default: the system's power glyph, drawn
    // from the icon font Windows already ships (nothing is bundled). Users may point it
    // at any *installed* font family + codepoint; see RegistryStore::loadLogo.
    static constexpr unsigned kDefaultLogo = 0xE7E8;
    static inline wchar_t logoGlyph[3] = {(wchar_t)kDefaultLogo, 0, 0};

    static void setLogoGlyph(unsigned cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kDefaultLogo;
        if (cp < 0x10000) { logoGlyph[0] = (wchar_t)cp; logoGlyph[1] = 0; }
        else {
            cp -= 0x10000;
            logoGlyph[0] = (wchar_t)(0xD800 + (cp >> 10));
            logoGlyph[1] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            logoGlyph[2] = 0;
        }
    }

    // `missing` (optional) reports that GDI had to substitute `face`.
    static HFONT make(HDC dc, const wchar_t* face, const wchar_t* fallback, int px, int weight,
                      bool* missing = nullptr) {
        HFONT hf = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        // GDI silently substitutes a missing face; detect that and use the fallback instead.
        HGDIOBJ old = SelectObject(dc, hf);
        wchar_t got[LF_FACESIZE];
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        const bool miss = !sys::weq(got, face);
        if (missing) *missing = miss;
        if (miss) {
            DeleteObject(hf);
            hf = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             fallback);
        }
        return hf;
    }

    // logoFace: user's installed font family for the logo, or null for the default.
    static void build(int dpi, const wchar_t* logoFace) {
        HDC dc = GetDC(nullptr);
        for (int i = 0; i < F_COUNT; ++i) if (f[i]) DeleteObject(f[i]);
        // Base size 12px at 96 dpi; the rest of the scale hangs off it.
        f[F_TEXT]  = make(dc, L"Consolas", L"Courier New", sys::scale(12, dpi), FW_NORMAL);
        f[F_SMALL] = make(dc, L"Consolas", L"Courier New", sys::scale(11, dpi), FW_NORMAL);
        f[F_TITLE] = make(dc, L"Consolas", L"Courier New", sys::scale(16, dpi), FW_BOLD);
        f[F_ICON]  = make(dc, L"Segoe Fluent Icons", L"Segoe MDL2 Assets", sys::scale(14, dpi), FW_NORMAL);
        bool miss = false;
        f[F_LOGO] = logoFace ? make(dc, logoFace, L"Segoe MDL2 Assets", sys::scale(17, dpi), FW_NORMAL, &miss)
                             : nullptr;
        if (!logoFace || miss) {  // no custom font, or it is not installed: use the default glyph
            if (f[F_LOGO]) DeleteObject(f[F_LOGO]);
            f[F_LOGO] = make(dc, L"Segoe Fluent Icons", L"Segoe MDL2 Assets", sys::scale(17, dpi), FW_NORMAL);
            setLogoGlyph(kDefaultLogo);
        }
        for (int i = 0; i < F_COUNT; ++i) {
            HGDIOBJ old = SelectObject(dc, f[i]);
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            h[i] = tm.tmHeight;
            SelectObject(dc, old);
        }
        ReleaseDC(nullptr, dc);
    }
};

struct GdiRenderer {
    HDC dst, dc;
    HBITMAP bmp, old;
    int w, h;

    GdiRenderer(HDC target, int w_, int h_) : dst(target), w(w_), h(h_) {
        dc = CreateCompatibleDC(target);
        bmp = CreateCompatibleBitmap(target, w, h);
        old = (HBITMAP)SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
    }
    ~GdiRenderer() {
        BitBlt(dst, 0, 0, w, h, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    GdiRenderer(const GdiRenderer&) = delete;
    GdiRenderer& operator=(const GdiRenderer&) = delete;

    void fill(int x, int y, int cw, int ch, COLORREF c) {
        RECT r{x, y, x + cw, y + ch};
        SetBkColor(dc, c);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    }
    void frame(int x, int y, int cw, int ch, int t, COLORREF c) {
        fill(x, y, cw, t, c);
        fill(x, y + ch - t, cw, t, c);
        fill(x, y + t, t, ch - 2 * t, c);
        fill(x + cw - t, y + t, t, ch - 2 * t, c);
    }
    int measure(const wchar_t* s, Font ft, int n = -1) {
        if (n < 0) n = sys::wlen(s);
        SelectObject(dc, Fonts::f[ft]);
        SIZE z;
        GetTextExtentPoint32W(dc, s, n, &z);
        return z.cx;
    }
    // Left-aligned at x, vertically centred in [y, y+ch).
    void text(int x, int y, int ch, const wchar_t* s, Font ft, COLORREF c, int n = -1) {
        if (n < 0) n = sys::wlen(s);
        SelectObject(dc, Fonts::f[ft]);
        SetTextColor(dc, c);
        ExtTextOutW(dc, x, y + (ch - Fonts::h[ft]) / 2, 0, nullptr, s, n, nullptr);
    }
    void textCentered(int x, int y, int cw, int ch, const wchar_t* s, Font ft, COLORREF c, int n = -1) {
        if (n < 0) n = sys::wlen(s);
        text(x + (cw - measure(s, ft, n)) / 2, y, ch, s, ft, c, n);
    }
    // Top-down 32bpp BGRX pixels.
    void image(int x, int y, int iw, int ih, const void* px) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = iw;
        bi.bmiHeader.biHeight = -ih;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, x, y, iw, ih, 0, 0, 0, ih, px, &bi, DIB_RGB_COLORS);
    }
};
