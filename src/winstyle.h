// Window styling and lifecycle for other applications' windows.
//
// Three policies, all reversible, composed by WindowWatcher:
//   * Style  (DwmStyle):     recolours the frame DWM already draws - border, caption, caption
//                            text, square corners, dark/light frame. Costs nothing.
//   * Frame  (StripCaption): removes a window's title bar (WS_CAPTION) and puts it back.
//   * Layout (Dwindle, see tiler.h): tiles windows while Options::tiling is on.
//
// A StylePolicy provides:  styleable(HWND)  apply(HWND, Palette, active)  reset(HWND)
// A FramePolicy provides:  hide(HWND)  restore(HWND)
//
// Frame state lives in a window property on the target itself (no list in our memory, and
// `--reset` can still find stripped windows after a crash).
//
// WindowWatcher is driven by WinEvent hooks: no polling.
#pragma once
#include "sys.h"
#include "theme.h"
#include "options.h"
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_BORDER_COLOR 34
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

namespace winutil {

inline bool startsWith(const wchar_t* s, const wchar_t* prefix) {
    while (*prefix) if (*s++ != *prefix++) return false;
    return true;
}

inline bool isShellWindow(HWND h) {
    wchar_t c[48];
    GetClassNameW(h, c, 48);
    return sys::weq(c, L"Progman") || sys::weq(c, L"WorkerW") || sys::weq(c, L"Shell_TrayWnd") ||
           sys::weq(c, L"Shell_SecondaryTrayWnd");
}

inline bool isOwn(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace winutil

// ------------------------------------------------------------------ frame --

struct StripCaption {
    static constexpr const wchar_t* prop = L"awframe";

    static bool stripped(HWND h) { return GetPropW(h, prop) != nullptr; }

    // Apps that draw their own title bar (or host their UI in it) break when WS_CAPTION
    // goes, so they are left alone. Extend this list as you find more.
    static bool excludedClass(HWND h) {
        wchar_t c[64];
        GetClassNameW(h, c, 64);
        static const wchar_t* const skip[] = {
            L"Chrome_WidgetWin",         // Chrome, Edge, VS Code, Electron, Teams
            L"ApplicationFrameWindow",   // UWP / packaged apps
            L"Windows.UI.",              // WinUI hosts
            // L"CabinetWClass",         // File Explorer: experiment, title bar now stripped like other windows
            L"MozillaWindowClass",       // Firefox
            L"OpusApp", L"XLMAIN", L"PPTFrameClass", L"rctrl_renwnd32",  // Office
        };
        for (const wchar_t* s : skip) if (winutil::startsWith(c, s)) return true;
        return false;
    }

    static bool eligible(HWND h) {
        const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        if ((st & WS_CAPTION) != WS_CAPTION || !(st & WS_THICKFRAME)) return false;  // needs a resize frame
        if (GetWindow(h, GW_OWNER)) return false;                                     // dialogs keep theirs
        return !excludedClass(h);
    }

    static void refresh(HWND h) {
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
    }

    static void hide(HWND h) {
        if (stripped(h) || !eligible(h)) return;
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) & ~(LONG_PTR)WS_CAPTION);
        SetPropW(h, prop, (HANDLE)1);
        refresh(h);
    }

    static void restore(HWND h) {
        if (!stripped(h)) return;
        SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | WS_CAPTION);
        RemovePropW(h, prop);
        refresh(h);
    }
};

// ----------------------------------------------------------------- colours --

struct DwmStyle {
    // Framed top-level windows that are not ours. A stripped window has no WS_CAPTION any
    // more, so carrying our frame property counts as framed too.
    static bool styleable(HWND h) {
        if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;
        if (GetAncestor(h, GA_ROOT) != h) return false;                       // top-level only
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;   // palettes, our bar
        if ((GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) != WS_CAPTION && !StripCaption::stripped(h)) return false;
        if (winutil::isOwn(h) || winutil::isShellWindow(h)) return false;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        return !cloaked;
    }

    // Windows that draw their own title area over the frame (Explorer, Terminal, Chromium, Snipping
    // Tool and other XAML/WinUI apps). There the DWM caption colour only shows through where the
    // app's surface is translucent, e.g. the title row of an unfocused window, which looked like a
    // stray tinted strip. They keep their own title area and get only the border colour and
    // light/dark. Detected by what they all do rather than by class name: their client area starts
    // at the top of the frame, where a classic window has its caption above the client area.
    static bool ownTitleArea(HWND h) {
        // A window whose caption we stripped (tiling) also reports a client area at the top; that is
        // fine: with no caption, the caption colour has nothing to paint anyway.
        RECT fr;
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) GetWindowRect(h, &fr);
        POINT top{0, 0};
        ClientToScreen(h, &top);
        const int caption = GetSystemMetricsForDpi(SM_CYCAPTION, GetDpiForWindow(h));
        return top.y - fr.top < caption / 2;
    }

    static void apply(HWND h, const Palette& p, bool active) {
        const BOOL dark = !p.light;
        const COLORREF border = active ? p.borderActive() : p.borderInactive();
        const bool own = ownTitleArea(h);
        const COLORREF caption = own ? DWMWA_COLOR_DEFAULT : p.bg();
        const COLORREF text = own ? DWMWA_COLOR_DEFAULT : active ? p.fg() : p.dim();
        const int corner = 1;  // DWMWCP_DONOTROUND: tiling look
        DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
        DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR, &text, sizeof(text));
    }

    static void reset(HWND h) {
        const COLORREF def = DWMWA_COLOR_DEFAULT;
        const int corner = 0;  // DWMWCP_DEFAULT
        // Frame darkness goes back to the user's system-wide app setting.
        DWORD light = 1, sz = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz);
        const BOOL dark = light == 0;
        DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &def, sizeof(def));
        DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &def, sizeof(def));
        DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR, &def, sizeof(def));
    }
};

// ---------------------------------------------------------------- watcher --

template <class Bus, class Style, class Frame, class Layout>
struct WindowWatcher {
    static inline HWINEVENTHOOK hookFg, hookObj, hookMin, hookMove;
    static inline HWND lastActive;

    static HWND root(HWND h) { return h ? GetAncestor(h, GA_ROOT) : nullptr; }

    // Bring one window in line with the current options.
    static void handle(HWND h, bool active) {
        if (Options::tiling) Layout::track(h);
        if (!Style::styleable(h)) return;
        const bool wantStrip = Options::tiling ? Layout::isTiled(h) : Options::hideTitlebars;
        if (wantStrip) Frame::hide(h); else Frame::restore(h);
        if (Options::themeWindows) Style::apply(h, Bus::palette(), active);
        else Style::reset(h);
    }

    static void CALLBACK onEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG idObject, LONG idChild, DWORD, DWORD) {
        if (!h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
        switch (ev) {
            case EVENT_SYSTEM_FOREGROUND: {
                HWND prev = lastActive;
                lastActive = root(h);
                Layout::noteFocus(lastActive);
                if (prev && prev != lastActive && Style::styleable(prev) && Options::themeWindows)
                    Style::apply(prev, Bus::palette(), false);
                handle(lastActive, true);
                break;
            }
            case EVENT_OBJECT_SHOW:
                Bus::windowShown(h);  // a just-launched app may be moved to the menu's monitor first
                handle(h, h == GetForegroundWindow());
                break;
            case EVENT_OBJECT_DESTROY: Layout::untrack(h); break;
            case EVENT_OBJECT_HIDE: Layout::onHide(h); break;
            case EVENT_SYSTEM_MINIMIZESTART: Layout::onMinimize(h, true); break;
            case EVENT_SYSTEM_MINIMIZEEND: Layout::onMinimize(h, false); break;
            case EVENT_SYSTEM_MOVESIZEEND: Layout::onMoveEnd(h); break;
        }
    }

    static BOOL CALLBACK enumProc(HWND h, LPARAM mode) {
        if (mode == 1) {  // full reset
            Frame::restore(h);
            if (IsWindowVisible(h)) Style::reset(h);
        } else {
            handle(h, h == lastActive);
        }
        return TRUE;
    }

    // Re-apply options/theme to every window.
    static void restyleAll() {
        if (!hookFg) return;
        EnumWindows(enumProc, 0);
        Layout::relayout();
    }
    static void resetAll() { EnumWindows(enumProc, 1); }

    // Target of a window command: the focused top-level window, unless it is ours or the shell.
    static HWND target() {
        HWND h = root(GetForegroundWindow());
        if (!h || winutil::isOwn(h) || winutil::isShellWindow(h)) return nullptr;
        return h;
    }

    static void start() {
        lastActive = root(GetForegroundWindow());
        hookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onEvent, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        hookObj = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, onEvent, 0, 0,  // destroy, show, hide
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        hookMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, onEvent, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        hookMove = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, onEvent, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        Layout::recoverHidden();  // anything a crashed instance left hidden
        if (Options::tiling) Layout::enable();
        restyleAll();
    }

    static void stop() {
        if (hookFg) UnhookWinEvent(hookFg);
        if (hookObj) UnhookWinEvent(hookObj);
        if (hookMin) UnhookWinEvent(hookMin);
        if (hookMove) UnhookWinEvent(hookMove);
        hookFg = hookObj = hookMin = hookMove = nullptr;
        Layout::disable();  // windows back where they were, nothing left hidden
        resetAll();         // and with their own title bars and colours
    }
};
