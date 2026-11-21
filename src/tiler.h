// Dwindle tiling layout (binary space partition, as in common tiling window managers).
//
// Multi-monitor: every monitor has its own workspaces 1-10 and shows one of them at a time. A
// "slot" (monitor * kWs + workspace) names one workspace of one monitor; trees, focus memory and
// fullscreen state are per slot. Windows belong to the monitor they are on when first seen.
//
// Each workspace is a binary tree: a leaf is a window, an inner node splits its box in two
// (side by side or stacked) at `ratio`. A new window splits the last-focused window's leaf,
// side-by-side when that box is wider than tall. Everything lives in fixed static arrays
// (about 17 KB of .bss, touched only when tiling is on): no heap, no STL.
//
// A LayoutPolicy provides:
//   track/untrack(HWND)  onHide / onMinimize   window lifecycle from WinEvents
//   isTiled(HWND)        so the frame policy knows whom to strip
//   relayout()           batched DeferWindowPos of the current workspace
//   focusDir / swapDir / toggleSplit / toggleFloat / fullscreen / resize
//   switchTo / moveToWorkspace / moveToMonitorDir / onMoveEnd / displayChanged / enable / disable
#pragma once
#include "sys.h"
#include "options.h"
#include <dwmapi.h>

namespace tileutil {
inline bool contains(const wchar_t* s, const wchar_t* sub) {
    if (!*sub) return true;
    for (; *s; ++s) {
        const wchar_t *a = s, *b = sub;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
// Give `h` the foreground even though we are not the foreground process. Windows only allows
// that for the process that received the last input, so tap Alt (marked as ours, so our own
// keyboard hook ignores it) and share the foreground thread's input queue for the call.
inline void forceForeground(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = VK_MENU;
        in[i].ki.dwFlags = i ? KEYEVENTF_KEYUP : 0;
        in[i].ki.dwExtraInfo = 0x41574B59;  // kKeyMagic (keys.h)
    }
    SendInput(2, in, sizeof(INPUT));
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId();
    const DWORD them = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attach = them && them != me;
    if (attach) AttachThreadInput(me, them, TRUE);
    SetForegroundWindow(h);
    BringWindowToTop(h);
    if (attach) AttachThreadInput(me, them, FALSE);
}
}  // namespace tileutil

template <class Bus>
struct Dwindle {
    static constexpr int kMaxWin = 128, kNodes = kMaxWin * 2, kWs = 10, kMaxMon = 4, kSlots = kMaxMon * kWs;
    static constexpr int kPerMon = 4;  // Options::limit: windows tiled per monitor when tiling is switched on
    enum : u8 { F_FLOAT = 1, F_HIDDEN = 2, F_MIN = 4 };
    static constexpr const wchar_t* kHidProp = L"awhid";

    struct Win { HWND h; RECT orig; u8 ws, flags; };  // ws: slot
    struct Mon { HMONITOR h; RECT mon, work; bool primary; };
    struct Node { short parent, a, b; HWND h; RECT r; u16 ratio; bool horiz, used; };

    static inline Win wins[kMaxWin];
    static inline int nWins;
    static inline Node nodes[kNodes];
    static inline short root[kSlots];
    static inline HWND last[kSlots], fs[kSlots];
    static inline u8 fsMode[kSlots];
    static inline Mon mons[kMaxMon];
    static inline int nMon, curW[kMaxMon];  // monitors (left to right) and the workspace each one shows
    static inline bool ready;
    static inline bool enabling;  // inside enable(): the per-monitor limit applies
    static inline wchar_t scope[48];  // dev option: only manage windows whose title contains this

    // ------------------------------------------------------------ geometry --
    static int dpi() { return (int)GetDpiForSystem(); }
    static int gapIn() { return Options::gaps ? sys::scale(5, dpi()) : 0; }
    static int gapOut() { return Options::gaps ? sys::scale(10, dpi()) : 0; }

    static int slotOf(int m, int w) { return m * kWs + w; }
    static int monOf(int slot) { return slot / kWs; }
    static bool visible(int slot) { return curW[slot / kWs] == slot % kWs; }
    static int curWs(int m) { return curW[m]; }
    static int primaryMon() {
        for (int i = 0; i < nMon; ++i) if (mons[i].primary) return i;
        return 0;
    }

    // ------------------------------------------------------------ monitors --
    static BOOL CALLBACK enumMon(HMONITOR h, HDC, LPRECT, LPARAM) {
        if (nMon >= kMaxMon) return FALSE;
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(h, &mi)) return TRUE;
        mons[nMon++] = Mon{h, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0};
        return TRUE;
    }
    // Left to right (then top to bottom), so monitor numbers follow the physical arrangement.
    static void enumMonitors() {
        nMon = 0;
        EnumDisplayMonitors(nullptr, nullptr, enumMon, 0);
        if (!nMon) {
            MONITORINFO mi{sizeof(mi)};
            HMONITOR h = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            GetMonitorInfoW(h, &mi);
            mons[nMon++] = Mon{h, mi.rcMonitor, mi.rcWork, true};
        }
        for (int i = 1; i < nMon; ++i)
            for (int j = i; j > 0; --j) {
                const RECT &a = mons[j - 1].mon, &b = mons[j].mon;
                if (a.left < b.left || (a.left == b.left && a.top <= b.top)) break;
                const Mon t = mons[j];
                mons[j] = mons[j - 1];
                mons[j - 1] = t;
            }
    }
    static void refreshWork() {
        for (int i = 0; i < nMon; ++i) {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(mons[i].h, &mi)) { mons[i].mon = mi.rcMonitor; mons[i].work = mi.rcWork; }
        }
    }
    // A handle we do not know means the list is stale: Windows hands out new HMONITORs when the
    // display topology changes. Match by area (same screen, new handle) before giving up, and then
    // fall back to the primary monitor rather than whichever one happens to sort first.
    static int monIndex(HMONITOR h) {
        for (int i = 0; i < nMon; ++i) if (mons[i].h == h) return i;
        MONITORINFO mi{sizeof(mi)};
        if (h && GetMonitorInfoW(h, &mi)) {
            int best = -1;
            long long bestArea = 0;
            for (int i = 0; i < nMon; ++i) {
                RECT x;
                if (!IntersectRect(&x, &mons[i].mon, &mi.rcMonitor)) continue;
                const long long a = (long long)(x.right - x.left) * (x.bottom - x.top);
                if (a > bestArea) { best = i; bestArea = a; }
            }
            if (best >= 0) {
                if (EqualRect(&mons[best].mon, &mi.rcMonitor)) { mons[best].h = h; mons[best].work = mi.rcWork; }
                return best;
            }
        }
        return primaryMon();
    }
    static int monOfWindow(HWND h) { return monIndex(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)); }
    // The monitor that window commands and workspace switches act on: the focused window's,
    // else the one under the cursor.
    static int activeMon() {
        HWND fg = GetForegroundWindow();
        fg = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
        const int i = fg ? idx(fg) : -1;
        if (i >= 0) return monOf(wins[i].ws);
        POINT p{};
        GetCursorPos(&p);
        return monIndex(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST));
    }
    // Nearest monitor in a direction (0 left, 1 right, 2 up, 3 down), or -1.
    static int monInDir(int m, int dir) {
        const RECT& a = mons[m].mon;
        const int cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
        int best = -1;
        long long bestScore = 0;
        for (int i = 0; i < nMon; ++i) {
            if (i == m) continue;
            const RECT& r = mons[i].mon;
            const int dx = (r.left + r.right) / 2 - cx, dy = (r.top + r.bottom) / 2 - cy;
            const bool ok = dir == 0 ? dx < 0 : dir == 1 ? dx > 0 : dir == 2 ? dy < 0 : dy > 0;
            if (!ok) continue;
            const long long along = dir < 2 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            const long long across = dir < 2 ? (dy < 0 ? -dy : dy) : (dx < 0 ? -dx : dx);
            const long long score = along + 3 * across;
            if (best < 0 || score < bestScore) { best = i; bestScore = score; }
        }
        return best;
    }

    // The rect to hand SetWindowPos so the *visible* frame lands on `c` (DWM adds invisible borders).
    static RECT outer(HWND h, const RECT& c) {
        RECT wr{}, fb{};
        GetWindowRect(h, &wr);
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb)))) fb = wr;
        return {c.left - (fb.left - wr.left), c.top - (fb.top - wr.top),
                c.right + (wr.right - fb.right), c.bottom + (wr.bottom - fb.bottom)};
    }

    // ------------------------------------------------------------- lookups --
    static int idx(HWND h) {
        for (int i = 0; i < nWins; ++i) if (wins[i].h == h) return i;
        return -1;
    }
    static short leafOf(HWND h) {
        if (!h) return -1;
        for (short i = 0; i < kNodes; ++i) if (nodes[i].used && nodes[i].a < 0 && nodes[i].h == h) return i;
        return -1;
    }
    static short allocNode() {
        for (short i = 0; i < kNodes; ++i)
            if (!nodes[i].used) {
                nodes[i] = Node{};
                nodes[i].used = true;
                nodes[i].parent = nodes[i].a = nodes[i].b = -1;
                nodes[i].ratio = 500;
                return i;
            }
        return -1;
    }
    static short anyLeaf(short n) { while (nodes[n].a >= 0) n = nodes[n].b; return n; }

    static void init() {
        if (ready) return;
        for (int i = 0; i < kSlots; ++i) root[i] = -1;
        enumMonitors();
        ready = true;
    }

    // -------------------------------------------------------- eligibility --
    static bool tileable(HWND h) {
        if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;
        if (GetAncestor(h, GA_ROOT) != h || GetWindow(h, GW_OWNER)) return false;
        const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST)) return false;
        const LONG st = GetWindowLongW(h, GWL_STYLE);
        if (!(st & WS_THICKFRAME)) return false;
        if ((st & WS_CAPTION) != WS_CAPTION && !GetPropW(h, L"awframe")) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return false;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return false;
        wchar_t c[64];
        GetClassNameW(h, c, 64);
        static const wchar_t* const skip[] = {L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
                                              L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow",
                                              L"Shell_", L"ForegroundStaging", L"Progman", L"WorkerW"};
        for (const wchar_t* s : skip) {
            const wchar_t* a = c;
            const wchar_t* b = s;
            while (*b && *a == *b) { ++a; ++b; }
            if (!*b) return false;
        }
        wchar_t title[96];
        if (!GetWindowTextW(h, title, 96)) return false;
        return !scope[0] || tileutil::contains(title, scope);
    }

    static bool known(HWND h) { return ready && idx(h) >= 0; }  // tracked, including hidden on another workspace

    static bool isTiled(HWND h) {
        const int i = idx(h);
        return i >= 0 && !(wins[i].flags & (F_FLOAT | F_MIN));
    }

    // ---------------------------------------------------------------- tree --
    static void insert(int ws, HWND h) {
        short leaf = allocNode();
        if (leaf < 0) return;
        nodes[leaf].h = h;
        if (root[ws] < 0) { root[ws] = leaf; return; }
        if (visible(ws)) computeLayout();  // boxes must be current to pick the split direction
        short t = leafOf(last[ws]);
        if (t < 0 || wins[idx(last[ws])].ws != ws) t = anyLeaf(root[ws]);
        short p = allocNode();
        if (p < 0) { nodes[leaf].used = false; return; }
        const short gp = nodes[t].parent;
        nodes[p].parent = gp;
        if (gp < 0) root[ws] = p;
        else if (nodes[gp].a == t) nodes[gp].a = p;
        else nodes[gp].b = p;
        nodes[p].a = t;
        nodes[p].b = leaf;
        nodes[t].parent = p;
        nodes[leaf].parent = p;
        RECT r = nodes[t].r;
        int w = r.right - r.left, hh = r.bottom - r.top;
        if (w <= 0) { r = mons[monOf(ws)].work; w = r.right - r.left; hh = r.bottom - r.top; }
        nodes[p].r = r;
        nodes[p].horiz = w >= hh;  // wide box: side by side; tall box: stacked
    }

    static void remove(int ws, HWND h) {
        const short l = leafOf(h);
        if (l < 0) return;
        const short p = nodes[l].parent;
        if (p < 0) { root[ws] = -1; nodes[l].used = false; return; }
        const short s = nodes[p].a == l ? nodes[p].b : nodes[p].a;
        const short gp = nodes[p].parent;
        nodes[s].parent = gp;
        if (gp < 0) root[ws] = s;
        else if (nodes[gp].a == p) nodes[gp].a = s;
        else nodes[gp].b = s;
        nodes[l].used = nodes[p].used = false;
    }

    static void place(short n, RECT r) {
        Node& N = nodes[n];
        N.r = r;
        if (N.a < 0) return;
        if (N.horiz) {
            const int w = (r.right - r.left) * N.ratio / 1000;
            place(N.a, {r.left, r.top, r.left + w, r.bottom});
            place(N.b, {r.left + w, r.top, r.right, r.bottom});
        } else {
            const int h = (r.bottom - r.top) * N.ratio / 1000;
            place(N.a, {r.left, r.top, r.right, r.top + h});
            place(N.b, {r.left, r.top + h, r.right, r.bottom});
        }
    }

    static int collect(short n, HWND* out, int k) {
        if (nodes[n].a < 0) { out[k] = nodes[n].h; return k + 1; }
        return collect(nodes[n].b, out, collect(nodes[n].a, out, k));
    }

    // Recompute every box of the visible workspaces (no window is touched).
    static void computeLayout() {
        const int go = gapOut(), gi = gapIn();
        for (int m = 0; m < nMon; ++m) {
            const int s = slotOf(m, curW[m]);
            if (root[s] < 0) continue;
            const RECT& a = mons[m].work;
            place(root[s], {a.left + go - gi, a.top + go - gi, a.right - go + gi, a.bottom - go + gi});
        }
    }

    static void relayout() {
        if (!Options::tiling || !ready) { Bus::repaintBar(); return; }
        refreshWork();
        computeLayout();
        const int go = gapOut(), gi = gapIn();
        for (int m = 0; m < nMon; ++m) {
            const int s = slotOf(m, curW[m]);
            if (root[s] < 0) continue;
            HWND list[kMaxWin];
            const int n = collect(root[s], list, 0);
            HDWP d = BeginDeferWindowPos(n);
            for (int i = 0; i < n && d; ++i) {
                HWND h = list[i];
                if (!IsWindow(h)) continue;
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                const RECT& r = nodes[leafOf(h)].r;
                const RECT t = outer(h, {r.left + gi, r.top + gi, r.right - gi, r.bottom - gi});
                d = DeferWindowPos(d, h, nullptr, t.left, t.top, t.right - t.left, t.bottom - t.top,
                                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            if (d) EndDeferWindowPos(d);
            // A fullscreen/maximised window covers the layout of its workspace.
            if (fs[s] && IsWindow(fs[s])) {
                const RECT& a = mons[m].work;
                RECT f = a;
                if (fsMode[s] == 1) f = {a.left + go, a.top + go, a.right - go, a.bottom - go};
                const RECT t = outer(fs[s], f);
                SetWindowPos(fs[s], HWND_TOP, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE);
            }
        }
        Bus::repaintBar();
    }

    // --------------------------------------------------- window lifecycle --
    static int tiledOn(int slot) {
        int n = 0;
        for (int i = 0; i < nWins; ++i) if (wins[i].ws == slot && !(wins[i].flags & (F_FLOAT | F_MIN))) ++n;
        return n;
    }

    static void addWin(HWND h) {
        Win& w = wins[nWins++];
        w.h = h;
        GetWindowRect(h, &w.orig);
        int m = monOfWindow(h);
        w.ws = (u8)slotOf(m, curW[m]);
        w.flags = IsIconic(h) ? F_MIN : 0;
        if (enabling && Options::limit && !(w.flags & F_MIN) && tiledOn(w.ws) > kPerMon) {  // counts w itself
            // Monitor full: try the others (primary first), else leave the window floating where it is.
            int to = -1;
            for (int k = 0; k < nMon && to < 0; ++k) {
                const int c = k == 0 ? primaryMon() : (k - 1 < primaryMon() ? k - 1 : k);
                if (tiledOn(slotOf(c, curW[c])) < kPerMon) to = c;
            }
            if (to >= 0) w.ws = (u8)slotOf(to, curW[to]);
            else w.flags |= F_FLOAT;
        }
        if (w.flags & F_FLOAT) return;
        if (!(w.flags & F_MIN)) insert(w.ws, h);
    }

    static void track(HWND h) {
        if (!Options::tiling || !ready || idx(h) >= 0 || nWins >= kMaxWin || !tileable(h)) return;
        addWin(h);
        relayout();
    }

    static void drop(int i) {
        const HWND h = wins[i].h;
        const int ws = wins[i].ws;
        remove(ws, h);
        if (last[ws] == h) last[ws] = nullptr;
        if (fs[ws] == h) fs[ws] = nullptr;
        wins[i] = wins[--nWins];
    }

    static void untrack(HWND h) {
        const int i = ready ? idx(h) : -1;
        if (i < 0) return;
        const bool onCur = visible(wins[i].ws);
        drop(i);
        if (onCur) relayout();
    }

    static void onHide(HWND h) {
        const int i = ready ? idx(h) : -1;
        if (i >= 0 && !(wins[i].flags & F_HIDDEN)) untrack(h);  // the app hid itself
    }

    static void onMinimize(HWND h, bool start) {
        const int i = ready ? idx(h) : -1;
        if (i < 0) return;
        Win& w = wins[i];
        if (start) { w.flags |= F_MIN; remove(w.ws, h); }
        else { w.flags &= (u8)~F_MIN; if (!(w.flags & F_FLOAT)) insert(w.ws, h); }
        if (visible(w.ws)) relayout();
    }

    static void noteFocus(HWND h) {
        const int i = ready ? idx(h) : -1;
        if (i >= 0) last[wins[i].ws] = h;
    }

    static BOOL CALLBACK enumTrack(HWND h, LPARAM) {
        if (nWins < kMaxWin && idx(h) < 0 && tileable(h)) addWin(h);
        return TRUE;
    }

    static void enable() {
        init();
        enumMonitors();   // monitors may have come and gone while tiling was off (nothing to remap: no windows)
        enabling = true;  // EnumWindows runs front to back, so the topmost windows get the tiles
        EnumWindows(enumTrack, 0);
        enabling = false;
        relayout();
    }

    static void showWin(HWND h) {
        RemovePropW(h, kHidProp);
        ShowWindowAsync(h, SW_SHOWNA);
    }

    // Put every window back where it was and forget them.
    static void disable() {
        if (!ready) return;
        for (int i = 0; i < nWins; ++i) {
            HWND h = wins[i].h;
            if (!IsWindow(h)) continue;
            if (wins[i].flags & F_HIDDEN) showWin(h);
            const RECT& o = wins[i].orig;
            SetWindowPos(h, nullptr, o.left, o.top, o.right - o.left, o.bottom - o.top,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        nWins = 0;
        for (int i = 0; i < kNodes; ++i) nodes[i].used = false;
        for (int i = 0; i < kSlots; ++i) { root[i] = -1; last[i] = fs[i] = nullptr; }
        for (int i = 0; i < kMaxMon; ++i) curW[i] = 0;
        Bus::repaintBar();
    }

    // Show anything a previous (crashed) instance left hidden. Safe to call at any time.
    static BOOL CALLBACK enumUnhide(HWND h, LPARAM) {
        if (GetPropW(h, kHidProp)) showWin(h);
        return TRUE;
    }
    static void recoverHidden() { EnumWindows(enumUnhide, 0); }

    // ------------------------------------------------------------ commands --
    static void focus(HWND h) { if (h && IsWindow(h)) tileutil::forceForeground(h); }

    // Nearest visible window in a direction, across monitors (their boxes share one coordinate
    // space). `sameWs` restricts the search to the window's own workspace.
    static HWND neighbor(HWND h, int dir, bool sameWs = false) {  // 0 left, 1 right, 2 up, 3 down
        const short me = leafOf(h);
        if (me < 0) return nullptr;
        const int myWs = wins[idx(h)].ws;
        const RECT& m = nodes[me].r;
        const int cx = (m.left + m.right) / 2, cy = (m.top + m.bottom) / 2;
        HWND best = nullptr;
        long long bestScore = 0;
        for (short i = 0; i < kNodes; ++i) {
            if (!nodes[i].used || nodes[i].a >= 0 || i == me) continue;
            const int wi = idx(nodes[i].h);
            if (wi < 0 || !visible(wins[wi].ws) || (sameWs && wins[wi].ws != myWs)) continue;
            const RECT& r = nodes[i].r;
            const int dx = (r.left + r.right) / 2 - cx, dy = (r.top + r.bottom) / 2 - cy;
            const bool ok = dir == 0 ? dx < 0 : dir == 1 ? dx > 0 : dir == 2 ? dy < 0 : dy > 0;
            if (!ok) continue;
            const long long along = dir < 2 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            const long long across = dir < 2 ? (dy < 0 ? -dy : dy) : (dx < 0 ? -dx : dx);
            const long long score = along + 3 * across;  // prefer windows lined up with us
            if (!best || score < bestScore) { best = nodes[i].h; bestScore = score; }
        }
        return best;
    }

    static void focusDir(HWND h, int dir) { focus(neighbor(h, dir)); }

    // Swap with the neighbour on this workspace; at the edge, hop to the next monitor instead.
    static void swapDir(HWND h, int dir) {
        HWND o = neighbor(h, dir, true);
        if (!o) { moveToMonitorDir(h, dir); return; }
        const short a = leafOf(h), b = leafOf(o);
        if (a < 0 || b < 0) return;
        nodes[a].h = o;
        nodes[b].h = h;
        relayout();
    }

    static void toggleSplit(HWND h) {
        const short l = leafOf(h);
        if (l < 0 || nodes[l].parent < 0) return;
        nodes[nodes[l].parent].horiz = !nodes[nodes[l].parent].horiz;
        relayout();
    }

    static void toggleFloat(HWND h) {
        if (!Options::tiling || !ready) return;
        if (idx(h) < 0) track(h);
        const int i = idx(h);
        if (i < 0) return;
        Win& w = wins[i];
        if (w.flags & F_FLOAT) {
            w.flags &= (u8)~F_FLOAT;
            GetWindowRect(h, &w.orig);
            insert(w.ws, h);
        } else {
            w.flags |= F_FLOAT;
            remove(w.ws, h);
            const RECT& o = w.orig;
            SetWindowPos(h, HWND_TOP, o.left, o.top, o.right - o.left, o.bottom - o.top, SWP_NOACTIVATE);
        }
        relayout();
    }

    static void fullscreen(HWND h, int mode) {  // 1: full width (keeps gaps), 2: full work area
        const int i = ready ? idx(h) : -1;
        if (i < 0) return;
        const int s = wins[i].ws;
        if (!visible(s)) return;
        if (fs[s] == h && fsMode[s] == mode) fs[s] = nullptr;
        else { fs[s] = h; fsMode[s] = (u8)mode; }
        relayout();
    }

    static void resize(HWND h, int axis, int deltaPx) {  // axis 0: width, 1: height
        short n = leafOf(h);
        while (n >= 0 && nodes[n].parent >= 0) {
            const short p = nodes[n].parent;
            if (nodes[p].horiz == (axis == 0)) {
                const RECT& r = nodes[p].r;
                const int span = axis == 0 ? r.right - r.left : r.bottom - r.top;
                if (span <= 0) return;
                const int d = deltaPx * 1000 / span;
                int ratio = nodes[p].ratio + (nodes[p].a == n ? d : -d);
                if (ratio < 100) ratio = 100;
                if (ratio > 900) ratio = 900;
                nodes[p].ratio = (u16)ratio;
                relayout();
                return;
            }
            n = p;
        }
    }

    // ---------------------------------------------------------- workspaces --
    // Put window `i` on slot `n`: hidden if that workspace is not showing, tiled into it if it
    // is a tiled window.
    static void assign(int i, int n) {
        Win& w = wins[i];
        HWND h = w.h;
        remove(w.ws, h);
        if (last[w.ws] == h) last[w.ws] = nullptr;
        if (fs[w.ws] == h) fs[w.ws] = nullptr;
        w.ws = (u8)n;
        if (!visible(n) && !(w.flags & F_HIDDEN)) {
            w.flags |= F_HIDDEN;
            SetPropW(h, kHidProp, (HANDLE)1);
            ShowWindowAsync(h, SW_HIDE);
        } else if (visible(n) && (w.flags & F_HIDDEN)) {
            w.flags &= (u8)~F_HIDDEN;
            showWin(h);
        }
        if (!(w.flags & (F_FLOAT | F_MIN))) insert(n, h);
        relayout();
    }

    // Make monitor `m` show its workspace `n` (hide the old one's windows, show the new one's).
    static bool showWs(int m, int n) {
        if (m < 0 || m >= nMon || n == curW[m]) return false;
        const int from = slotOf(m, curW[m]), to = slotOf(m, n);
        for (int i = 0; i < nWins; ++i)
            if (wins[i].ws == from && !(wins[i].flags & (F_MIN | F_HIDDEN))) {
                wins[i].flags |= F_HIDDEN;
                SetPropW(wins[i].h, kHidProp, (HANDLE)1);
                ShowWindowAsync(wins[i].h, SW_HIDE);
            }
        curW[m] = n;
        for (int i = 0; i < nWins; ++i)
            if (wins[i].ws == to && (wins[i].flags & F_HIDDEN)) {
                wins[i].flags &= (u8)~F_HIDDEN;
                showWin(wins[i].h);
            }
        return true;
    }

    // Show workspace `n`, asked from monitor `m` (default: the active one). With Options::syncWs
    // every monitor switches to its own workspace `n` together, so a workspace spans all screens.
    static void switchTo(int n, int m = -1) {
        if (!Options::tiling || !ready || n < 0 || n >= kWs) return;
        if (m < 0) m = activeMon();
        if (m >= nMon) return;
        bool changed = false;
        if (Options::syncWs) for (int k = 0; k < nMon; ++k) changed |= showWs(k, n);
        else changed = showWs(m, n);
        if (!changed) return;
        relayout();
        const int to = slotOf(m, n);
        if (last[to] && idx(last[to]) >= 0) focus(last[to]);
    }

    static void switchRel(int d) {
        const int m = activeMon();
        switchTo((curW[m] + d + kWs) % kWs, m);
    }

    // Move to workspace `n` of the window's own monitor.
    static void moveToWorkspace(HWND h, int n) {
        const int i = ready ? idx(h) : -1;
        if (i < 0 || n < 0 || n >= kWs) return;
        const int s = slotOf(monOf(wins[i].ws), n);
        if (s != wins[i].ws) assign(i, s);
    }

    // Move to whatever workspace monitor `m` is showing.
    static void moveToMonitor(HWND h, int m) {
        const int i = ready ? idx(h) : -1;
        if (i < 0 || m < 0 || m >= nMon || m == monOf(wins[i].ws)) return;
        Win& w = wins[i];
        if (w.flags & F_FLOAT) {  // floating windows keep their place relative to the work area
            const RECT &a = mons[monOf(w.ws)].work, &b = mons[m].work;
            RECT r;
            GetWindowRect(h, &r);
            SetWindowPos(h, nullptr, r.left + b.left - a.left, r.top + b.top - a.top, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        }
        assign(i, slotOf(m, curW[m]));
    }

    static void moveToMonitorDir(HWND h, int dir) {
        const int i = ready ? idx(h) : -1;
        if (i < 0) return;
        const int m = monInDir(monOf(wins[i].ws), dir);
        if (m >= 0) { moveToMonitor(h, m); focus(h); }
    }

    // A move/resize by the user finished: a window dropped on another monitor joins it, a tiled
    // one dropped elsewhere on its own monitor snaps back into its box.
    static void onMoveEnd(HWND h) {
        const int i = ready && Options::tiling ? idx(h) : -1;
        if (i < 0 || (wins[i].flags & (F_MIN | F_HIDDEN))) return;
        const int m = monOfWindow(h);
        if (m != monOf(wins[i].ws)) assign(i, slotOf(m, curW[m]));
        else if (!(wins[i].flags & F_FLOAT)) relayout();
    }

    // Monitors came, went or were rearranged. Re-read them and carry every monitor's workspaces
    // (trees, focus memory, fullscreen, shown workspace) over to its new index: matched by handle,
    // else by identical area. Windows of a monitor that is gone move to the same workspace number
    // on the primary monitor.
    static void displayChanged() {
        if (!ready) return;
        Mon old[kMaxMon];
        const int oldN = nMon;
        for (int i = 0; i < oldN; ++i) old[i] = mons[i];
        enumMonitors();

        int map[kMaxMon];
        bool taken[kMaxMon] = {};
        for (int i = 0; i < oldN; ++i) {
            map[i] = -1;
            for (int j = 0; j < nMon && map[i] < 0; ++j)
                if (!taken[j] && mons[j].h == old[i].h) map[i] = j;
            if (map[i] >= 0) taken[map[i]] = true;
        }
        for (int i = 0; i < oldN; ++i) {
            for (int j = 0; j < nMon && map[i] < 0; ++j)
                if (!taken[j] && EqualRect(&mons[j].mon, &old[i].mon)) map[i] = j;
            if (map[i] >= 0) taken[map[i]] = true;
        }

        // Windows of vanished monitors leave their (soon dropped) trees while the old slots are live.
        bool orphan[kMaxWin] = {};
        for (int i = 0; i < nWins; ++i) {
            const int om = monOf(wins[i].ws);
            if (om < oldN && map[om] >= 0) continue;
            remove(wins[i].ws, wins[i].h);
            orphan[i] = true;
        }

        short nr[kSlots];
        HWND nl[kSlots], nf[kSlots];
        u8 nfm[kSlots];
        int nc[kMaxMon] = {};
        for (int s = 0; s < kSlots; ++s) { nr[s] = -1; nl[s] = nf[s] = nullptr; nfm[s] = 0; }
        for (int i = 0; i < oldN; ++i) {
            if (map[i] < 0) continue;
            nc[map[i]] = curW[i];
            for (int w = 0; w < kWs; ++w) {
                const int a = slotOf(i, w), b = slotOf(map[i], w);
                nr[b] = root[a]; nl[b] = last[a]; nf[b] = fs[a]; nfm[b] = fsMode[a];
            }
        }
        for (int s = 0; s < kSlots; ++s) { root[s] = nr[s]; last[s] = nl[s]; fs[s] = nf[s]; fsMode[s] = nfm[s]; }
        // A monitor that just appeared joins the workspace the others show.
        if (Options::syncWs && oldN > 0) {
            int shown = curW[0];
            for (int i = 0; i < oldN; ++i) if (map[i] >= 0) { shown = curW[i]; break; }
            for (int m = 0; m < nMon; ++m) if (!taken[m]) nc[m] = shown;
        }
        for (int m = 0; m < kMaxMon; ++m) curW[m] = nc[m];
        for (int i = 0; i < nWins; ++i)
            if (!orphan[i]) wins[i].ws = (u8)slotOf(map[monOf(wins[i].ws)], wins[i].ws % kWs);

        const int p = primaryMon();
        for (int i = 0; i < nWins; ++i) {
            if (!orphan[i]) continue;
            Win& w = wins[i];
            const int n = slotOf(p, w.ws % kWs);
            w.ws = (u8)n;
            if (!visible(n) && !(w.flags & F_HIDDEN)) {
                w.flags |= F_HIDDEN;
                SetPropW(w.h, kHidProp, (HANDLE)1);
                ShowWindowAsync(w.h, SW_HIDE);
            } else if (visible(n) && (w.flags & F_HIDDEN)) {
                w.flags &= (u8)~F_HIDDEN;
                showWin(w.h);
            }
            if (!(w.flags & (F_FLOAT | F_MIN))) insert(n, w.h);
        }
        relayout();
    }

    // For the bar: number of workspace slots to show on monitor `m` (at least 5, up to the highest in use).
    static int slots(int m) {
        int hi = 4;
        for (int i = 0; i < nWins; ++i)
            if (monOf(wins[i].ws) == m && wins[i].ws % kWs > hi) hi = wins[i].ws % kWs;
        if (curW[m] > hi) hi = curW[m];
        return hi + 1;
    }
};
