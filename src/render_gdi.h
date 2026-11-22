// Render policy: plain GDI. Chosen over Direct2D/DirectWrite because it keeps
// the process at a few MB private working set and loads no extra DLLs.
// A RenderPolicy provides:
//   R(HDC target, int w, int h)   double-buffered frame, presented in ~R()
//   fill / frame / text / textCentered / measure / image
// A Direct2D policy can replace this without touching bar or picker code.
#pragma once
#include "sys.h"

#include "omarchyfont.gen.h"

// F_TEXT..F_LOGO: bar/picker. F_BOLD (panel titles), F_CAPTION (spaced caps), F_HERO (panel icon),
// F_OMA/F_OMAHERO: the Omarchy mark font at bar/panel size, F_ROW (menu rows), F_BIGICON (buttons).
enum Font : u8 { F_TEXT, F_SMALL, F_TITLE, F_ICON, F_LOGO, F_BOLD, F_CAPTION, F_HERO, F_OMA, F_OMAHERO, F_ROW,
                 F_BIGICON, F_COUNT };

struct Fonts {
    static inline HFONT f[F_COUNT];
    static inline int h[F_COUNT];
    static inline wchar_t face[LF_FACESIZE];  // the text family actually in use
    static inline bool omarchyLoaded;
    // The bar's logo is one glyph of one font. Default: the Omarchy mark (U+E900) from the
    // omarchy.ttf the binary embeds and loads privately (nothing is installed). Users may point
    // it at any *installed* font family + codepoint; see RegistryStore::loadLogo.
    static constexpr unsigned kDefaultLogo = 0xE900;
    static constexpr const wchar_t* kOmarchy = L"omarchy";

    static void loadOmarchy() {
        if (omarchyLoaded) return;
        DWORD n = 0;
        omarchyLoaded = AddFontMemResourceEx((void*)kOmarchyTtf, sizeof(kOmarchyTtf), nullptr, &n) != nullptr;
    }
    // Omarchy's monospace (JetBrains Mono) when installed, else Windows' own monospace faces.
    static void pickFace(HDC dc) {
        static const wchar_t* const order[] = {L"JetBrainsMono Nerd Font", L"JetBrainsMono NF", L"JetBrains Mono",
                                               L"Cascadia Mono", L"Consolas"};
        for (const wchar_t* fc : order) {
            bool miss = true;
            HFONT t = make(dc, fc, L"Consolas", 12, FW_NORMAL, &miss);
            DeleteObject(t);
            if (!miss) { sys::wcopy(face, fc); return; }
        }
        sys::wcopy(face, L"Courier New");
    }
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
    static HFONT make(HDC dc, const wchar_t* want, const wchar_t* fallback, int px, int weight,
                      bool* missing = nullptr) {
        HFONT hf = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, want);
        // GDI silently substitutes a missing face; detect that and use the fallback instead.
        HGDIOBJ old = SelectObject(dc, hf);
        wchar_t got[LF_FACESIZE];
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        const bool miss = !sys::weq(got, want);
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
        loadOmarchy();
        if (!face[0]) pickFace(dc);
        for (int i = 0; i < F_COUNT; ++i) if (f[i]) DeleteObject(f[i]);
        // Base size 12px at 96 dpi; the rest of the scale hangs off it.
        const wchar_t* ic = L"Segoe Fluent Icons";
        f[F_TEXT]    = make(dc, face, L"Consolas", sys::scale(12, dpi), FW_NORMAL);
        f[F_SMALL]   = make(dc, face, L"Consolas", sys::scale(11, dpi), FW_NORMAL);
        f[F_TITLE]   = make(dc, face, L"Consolas", sys::scale(16, dpi), FW_BOLD);
        f[F_ICON]    = make(dc, ic, L"Segoe MDL2 Assets", sys::scale(14, dpi), FW_NORMAL);
        f[F_BOLD]    = make(dc, face, L"Consolas", sys::scale(14, dpi), FW_BOLD);
        f[F_CAPTION] = make(dc, face, L"Consolas", sys::scale(10, dpi), FW_BOLD);
        f[F_HERO]    = make(dc, ic, L"Segoe MDL2 Assets", sys::scale(24, dpi), FW_NORMAL);
        f[F_OMA]     = make(dc, kOmarchy, ic, sys::scale(15, dpi), FW_NORMAL);
        f[F_OMAHERO] = make(dc, kOmarchy, ic, sys::scale(26, dpi), FW_NORMAL);
        f[F_ROW]     = make(dc, face, L"Consolas", sys::scale(14, dpi), FW_NORMAL);
        f[F_BIGICON] = make(dc, ic, L"Segoe MDL2 Assets", sys::scale(18, dpi), FW_NORMAL);
        bool miss = false;
        f[F_LOGO] = logoFace ? make(dc, logoFace, ic, sys::scale(17, dpi), FW_NORMAL, &miss) : nullptr;
        if (!logoFace || miss) {  // no custom font, or it is not installed: the embedded Omarchy mark
            if (f[F_LOGO]) DeleteObject(f[F_LOGO]);
            bool noOma = !omarchyLoaded;
            f[F_LOGO] = noOma ? nullptr : make(dc, kOmarchy, ic, sys::scale(16, dpi), FW_NORMAL, &noOma);
            if (noOma) {  // embedded font failed to load: the system power glyph
                if (f[F_LOGO]) DeleteObject(f[F_LOGO]);
                f[F_LOGO] = make(dc, ic, L"Segoe MDL2 Assets", sys::scale(17, dpi), FW_NORMAL);
                setLogoGlyph(0xE7E8);
            } else setLogoGlyph(kDefaultLogo);
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
    // Like text(), cut off at x + maxW.
    void textClipped(int x, int y, int ch, int maxW, const wchar_t* s, Font ft, COLORREF c) {
        SelectObject(dc, Fonts::f[ft]);
        SetTextColor(dc, c);
        RECT r{x, y, x + maxW, y + ch};
        ExtTextOutW(dc, x, y + (ch - Fonts::h[ft]) / 2, ETO_CLIPPED, &r, s, sys::wlen(s), nullptr);
    }
    void textCentered(int x, int y, int cw, int ch, const wchar_t* s, Font ft, COLORREF c, int n = -1) {
        if (n < 0) n = sys::wlen(s);
        text(x + (cw - measure(s, ft, n)) / 2, y, ch, s, ft, c, n);
    }
    // Spaced capitals (Omarchy's section captions).
    void caption(int x, int y, int ch, const wchar_t* s, COLORREF c, int spacing) {
        SetTextCharacterExtra(dc, spacing);
        text(x, y, ch, s, F_CAPTION, c);
        SetTextCharacterExtra(dc, 0);
    }
    int measureCaption(const wchar_t* s, int spacing) {
        return measure(s, F_CAPTION) + spacing * sys::wlen(s);
    }
    // Rounded rectangle, filled and/or outlined (CLR_NONE skips either).
    void round(int x, int y, int cw, int ch, int rad, COLORREF fillC, COLORREF lineC) {
        HBRUSH b = fillC == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fillC);
        HPEN p = lineC == CLR_NONE ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, 1, lineC);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, x, y, x + cw + (lineC == CLR_NONE), y + ch + (lineC == CLR_NONE), rad, rad);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        if (fillC != CLR_NONE) DeleteObject(b);
        if (lineC != CLR_NONE) DeleteObject(p);
    }
    void ellipse(int x, int y, int d, COLORREF c) { round(x, y, d, d, d, c, CLR_NONE); }
    // Word-wrapped text in a box; returns the height used (measure only when draw is false).
    int wrap(int x, int y, int cw, const wchar_t* s, Font ft, COLORREF c, bool draw = true) {
        SelectObject(dc, Fonts::f[ft]);
        SetTextColor(dc, c);
        RECT r{x, y, x + cw, y + 10000};
        DrawTextW(dc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        if (draw) DrawTextW(dc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        return r.bottom - r.top;
    }
    // A 32bpp premultiplied-alpha bitmap (shell icons), scaled into the box.
    void alpha(int x, int y, int cw, int ch, HBITMAP bm) {
        BITMAP b{};
        if (!bm || !GetObjectW(bm, sizeof(b), &b)) return;
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ o = SelectObject(m, bm);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        SetStretchBltMode(dc, HALFTONE);
        GdiAlphaBlend(dc, x, y, cw, ch, m, 0, 0, b.bmWidth, b.bmHeight, bf);
        SelectObject(m, o);
        DeleteDC(m);
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
