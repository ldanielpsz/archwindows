// Open launched apps on the screen the menu was opened on, the way tiling window managers open
// new windows on the focused monitor.
//
// The menu calls arm(monitor) right before it launches something. The first new app window shown
// within kWindowMs is claimed and moved onto that monitor (centred in its work area, size kept,
// maximised state kept). This happens before the layout sees the window, so with tiling on it is
// tiled on that monitor too. Windows the layout already tracks (e.g. ones a workspace switch
// reveals) are never claimed, and one launch claims at most one window.
//
// A PlacementPolicy provides:  arm(HMONITOR)  claim(HWND)
#pragma once
#include "sys.h"
#include "winstyle.h"  // winutil::isOwn, isShellWindow

template <class Layout>
struct SpawnPlacement {
    static constexpr DWORD kWindowMs = 10000;  // slow cold starts (Office, IDEs) still land
    static inline HMONITOR mon;
    static inline DWORD armedAt;

    static void arm(HMONITOR m) { mon = m; armedAt = GetTickCount(); }

    // A top-level, framed app window: not a dialog, splash screen, tool palette, ours or the shell's.
    // Cloaked windows are allowed: packaged (Store) apps show their frame cloaked first.
    static bool appWindow(HWND h) {
        if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
        if (GetAncestor(h, GA_ROOT) != h || GetWindow(h, GW_OWNER)) return false;
        if (GetWindowLongW(h, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
        const LONG st = GetWindowLongW(h, GWL_STYLE);
        if ((st & WS_CAPTION) != WS_CAPTION && !(st & WS_THICKFRAME)) return false;
        return !winutil::isOwn(h) && !winutil::isShellWindow(h);
    }

    static void claim(HWND h) {
        if (!mon) return;
        if (GetTickCount() - armedAt > kWindowMs) { mon = nullptr; return; }
        if (!appWindow(h) || Layout::known(h)) return;
        const HMONITOR target = mon;
        mon = nullptr;
        moveTo(h, target);
    }

    static void moveTo(HWND h, HMONITOR target) {
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == target) return;
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(target, &mi)) return;
        const bool max = IsZoomed(h) != FALSE;
        if (max) ShowWindow(h, SW_RESTORE);
        RECT r;
        GetWindowRect(h, &r);
        const RECT& a = mi.rcWork;
        const int aw = a.right - a.left, ah = a.bottom - a.top;
        int w = r.right - r.left, ht = r.bottom - r.top;
        if (w > aw) w = aw;
        if (ht > ah) ht = ah;
        // Synchronous, so the layout reads the new monitor straight after. If the target has a
        // different DPI, the app rescales itself on WM_DPICHANGED.
        SetWindowPos(h, nullptr, a.left + (aw - w) / 2, a.top + (ah - ht) / 2, w, ht,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        if (max) ShowWindow(h, SW_MAXIMIZE);
    }
};
