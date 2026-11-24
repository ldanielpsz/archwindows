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
//   focusDir / swapDir / toggleSplit / toggleFloat / pop / fullscreen / resize
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
// that right after Alt was pressed, so press Alt (marked as ours, so our own keyboard hook
// ignores it). A bare Alt tap reached the focused app as a menu key: it entered menu / KeyTips
// mode and the next letters typed (Shift+N, Shift+S ...) ran its menu commands. So the
// unassigned key 0xE8 goes between Alt down and up, as for the Win key (keys.h): Alt then
// counts as a shortcut, not a tap. The foreground thread's input queue is shared only if that
// was not enough: attached queues share key state, and a modifier held at that moment (Win,
// Shift during a chord) could stay down in the other app after it was released.
inline void forceForeground(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    static const WORD vk[4] = {VK_MENU, 0xE8, 0xE8, VK_MENU};
    INPUT in[4] = {};
    for (int i = 0; i < 4; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk[i];
        in[i].ki.dwFlags = (i == 2 || i == 3) ? KEYEVENTF_KEYUP : 0;
        in[i].ki.dwExtraInfo = 0x41574B59;  // kKeyMagic (keys.h)
    }
    SendInput(4, in, sizeof(INPUT));
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) { BringWindowToTop(h); return; }
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
    enum : u8 { F_FLOAT = 1, F_HIDDEN = 2, F_MIN = 4, F_PIN = 8, F_SETTLE = 16 };
    // F_PIN: popped out (Super+O), always with F_FLOAT. F_SETTLE: just sent to another monitor, see settle().
    static constexpr const wchar_t* kHidProp = L"awhid";
    static constexpr const wchar_t* kTopProp = L"awtop";  // we made it topmost (fullscreen / popped out)
    static constexpr const wchar_t* kFsProp = L"awfs";    // frame bits we took off for Super+F (see bare())

    struct Win { HWND h; RECT orig, pre; u8 ws, flags; };  // ws: slot; pre: floating rect before fullscreen
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
    static inline UINT_PTR settleId;  // thread timer of the settle passes, 0 when idle
    static inline int settleLeft;
    static inline bool settling;
    static inline void (*trace)(const wchar_t*, ...);  // the shell's log (--log / LogPath), else null

    // Log every tracked window and the workspace state (diagnosis only; nothing when not logging).
    static void dump(const wchar_t* why, HWND about = nullptr) {
        if (!trace) return;
        trace(L"tiler %s %p: mons=%d cur=%d,%d,%d", why, (void*)about, nMon, curW[0], nMon > 1 ? curW[1] : -1,
              nMon > 2 ? curW[2] : -1);
        for (int s = 0; s < kSlots; ++s)
            if (fs[s]) trace(L"  fs slot %d = %p mode %d", s, (void*)fs[s], (int)fsMode[s]);
        for (int i = 0; i < nWins; ++i) {
            RECT r{};
            GetWindowRect(wins[i].h, &r);
            wchar_t t[40] = {};
            GetWindowTextW(wins[i].h, t, 40);
            const short l = leafOf(wins[i].h);
            trace(L"  %p ws=%d flags=%x leaf=%d vis=%d rect=%d,%d,%d,%d box=%d,%d,%d,%d '%s'", (void*)wins[i].h,
                  (int)wins[i].ws, (int)wins[i].flags, (int)l, (int)IsWindowVisible(wins[i].h), r.left, r.top, r.right,
                  r.bottom, l >= 0 ? nodes[l].r.left : 0, l >= 0 ? nodes[l].r.top : 0, l >= 0 ? nodes[l].r.right : 0,
                  l >= 0 ? nodes[l].r.bottom : 0, t);
        }
    }

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
    static bool shellClass(HWND h) {
        wchar_t c[64];
        GetClassNameW(h, c, 64);
        static const wchar_t* const skip[] = {L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
                                              L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow",
                                              L"Shell_", L"ForegroundStaging", L"Progman", L"WorkerW"};
        for (const wchar_t* s : skip) {
            const wchar_t* a = c;
            const wchar_t* b = s;
            while (*b && *a == *b) { ++a; ++b; }
            if (!*b) return true;
        }
        return false;
    }
    // Asked for by name (Super+O / Super+T on a window we do not manage): looser than tileable().
    // Windows opened by another program are often owned by it, topmost, fixed-size, untitled when
    // shown, or were cloaked when they appeared, and so were never picked up automatically.
    static bool adoptable(HWND h) {
        if (!h || !IsWindow(h) || !IsWindowVisible(h) || GetAncestor(h, GA_ROOT) != h) return false;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;  // palettes, overlays
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return false;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        return !cloaked && !shellClass(h);
    }

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
        if (shellClass(h)) return false;
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
        if (leafOf(h) >= 0) return;  // already tiled: a second leaf would cut the tree
        if (visible(ws) && root[ws] >= 0) computeLayout();  // boxes must be current to pick the split direction
        // The leaf to split: the workspace's last focused window, picked BEFORE the new leaf holds
        // `h`. A window going back into the tiles is usually that last focused window itself
        // (Super+O / Super+T back, a restore), and looked up afterwards it found its own new
        // leaf: the split got that leaf twice and the real neighbour fell out of the tree, left
        // at its old box (one window full screen, the other tiled over half of it).
        short t = (last[ws] && last[ws] != h) ? leafOf(last[ws]) : -1;
        if (t >= 0 && wins[idx(last[ws])].ws != ws) t = -1;
        short leaf = allocNode();
        if (leaf < 0) return;
        nodes[leaf].h = h;
        if (root[ws] < 0) { root[ws] = leaf; return; }
        if (t < 0) t = anyLeaf(root[ws]);
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
            // Only windows that are not already in place are touched: every relayout used to
            // re-send every window of every monitor, and apps (Terminal, Chrome, Electron) redraw
            // or re-lay out on that, so Super+F on one screen flickered the others. The fullscreen
            // window is skipped too: it was squeezed back into its tile and blown up again each time.
            HDWP d = BeginDeferWindowPos(n);
            for (int i = 0; i < n && d; ++i) {
                HWND h = list[i];
                if (!IsWindow(h) || h == fs[s]) continue;
                markCross(h, m);
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                const RECT& r = nodes[leafOf(h)].r;
                const RECT t = outer(h, {r.left + gi, r.top + gi, r.right - gi, r.bottom - gi});
                RECT cur{};
                if (GetWindowRect(h, &cur) && EqualRect(&cur, &t)) continue;
                d = DeferWindowPos(d, h, nullptr, t.left, t.top, t.right - t.left, t.bottom - t.top,
                                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            if (d) EndDeferWindowPos(d);
            // A fullscreen window covers its workspace: mode 2 the whole monitor, over the bar
            // (topmost, as Omarchy's `fullscreen 0`), mode 1 the work area inside the outer gaps.
            if (fs[s] && IsWindow(fs[s])) {
                const RECT& a = mons[m].work;
                RECT f = mons[m].mon;
                if (fsMode[s] == 1) f = {a.left + go, a.top + go, a.right - go, a.bottom - go};
                markCross(fs[s], m);
                if (IsZoomed(fs[s])) ShowWindow(fs[s], SW_RESTORE);
                if (fsMode[s] == 2) bare(fs[s], true);  // no frame: its rect is then exactly the monitor
                const RECT t = fsMode[s] == 2 ? f : outer(fs[s], f);
                RECT cur{};
                const bool placed = GetWindowRect(fs[s], &cur) && EqualRect(&cur, &t);
                const bool top = (GetWindowLongW(fs[s], GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                if (fsMode[s] == 2) markTop(fs[s], true);
                // Only touched when it is not in place yet: mode 1 used to be raised on every
                // relayout, so a change on another monitor re-sent it and buried floating windows.
                if (!placed || (fsMode[s] == 2 && !top))
                    SetWindowPos(fs[s], fsMode[s] == 2 ? HWND_TOPMOST : HWND_TOP, t.left, t.top, t.right - t.left,
                                 t.bottom - t.top, SWP_NOACTIVATE | (placed ? SWP_NOMOVE | SWP_NOSIZE : 0));
            }
        }
        armSettle();
        if (!settling) dump(L"relayout");
        for (int i = 0; i < nWins; ++i)
            if (!(wins[i].flags & (F_HIDDEN | F_MIN)) && (!IsWindow(wins[i].h) || !IsWindowVisible(wins[i].h))) {
                armReap();
                break;
            }
        Bus::repaintBar();
    }

    // Ghost tiles: a window still holding a tile although it is gone or hidden by its app, so the
    // tile showed only wallpaper (WhatsApp closed to the tray kept its tile). Normally the hide or
    // destroy event untracks it; when that was missed, the next relayout notices and, a moment
    // later (a window the shell just showed is only visible once its app has run the show), drops
    // whatever is still gone.
    static inline UINT_PTR reapId;
    static void armReap() { if (!reapId) reapId = SetTimer(nullptr, 0, 1000, onReap); }
    static void CALLBACK onReap(HWND, UINT, UINT_PTR, DWORD) {
        KillTimer(nullptr, reapId);
        reapId = 0;
        if (!Options::tiling || !ready) return;
        bool any = false;
        for (int i = nWins - 1; i >= 0; --i)  // drop() moves the last entry into slot i
            if (!(wins[i].flags & (F_HIDDEN | F_MIN)) && (!IsWindow(wins[i].h) || !IsWindowVisible(wins[i].h))) {
                drop(i);
                any = true;
            }
        if (any) relayout();
    }

    // ------------------------------------------------- monitor DPI changes --
    // A window sent to a monitor with another scale (100% <-> 125%) gets WM_DPICHANGED and, being
    // per-monitor aware, rescales itself by the DPI ratio, in its own thread, after we placed it:
    // it overflows its tile (to a larger scale) or fills only part of it (to a smaller one). Its
    // invisible DWM borders change with the DPI too, so outer() was off. So a window that crosses
    // monitors is flagged and, once it has rescaled, placed again (same monitor now, so no second
    // DPI change), a few times over ~0.6 s for apps that rescale late. A window that will not
    // shrink to its tile (minimum size) is at least kept inside its monitor's work area.
    static void markCross(HWND h, int m) {
        const int i = idx(h);
        if (i >= 0 && MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != mons[m].h) wins[i].flags |= F_SETTLE;
    }
    static void armSettle() {
        if (settling) return;  // the settle pass itself must not extend the run forever
        bool any = false;
        for (int i = 0; i < nWins && !any; ++i) any = (wins[i].flags & F_SETTLE) != 0;
        if (!any) return;
        settleLeft = 4;
        settleId = SetTimer(nullptr, settleId, 150, onSettle);
    }
    static void stopSettle() {
        if (settleId) KillTimer(nullptr, settleId);
        settleId = 0;
        settleLeft = 0;
        for (int i = 0; i < nWins; ++i) wins[i].flags &= (u8)~F_SETTLE;
    }
    static void CALLBACK onSettle(HWND, UINT, UINT_PTR, DWORD) {
        if (!Options::tiling || !ready) { stopSettle(); return; }
        settling = true;
        relayout();
        for (int i = 0; i < nWins; ++i)
            if ((wins[i].flags & F_SETTLE) && !(wins[i].flags & (F_HIDDEN | F_MIN)) && fs[wins[i].ws] != wins[i].h) {
                // A popped-out window is centred again once it has rescaled (Omarchy's pop.lua).
                if (wins[i].flags & F_PIN) centreOn(wins[i].h, monOf(wins[i].ws));
                fitInto(wins[i].h, monOf(wins[i].ws));
            }
        settling = false;
        if (--settleLeft <= 0) stopSettle();
    }
    // Centre `h` on monitor m's work area, keeping its size.
    static void centreOn(HWND h, int m) {
        if (!IsWindow(h) || m < 0 || m >= nMon) return;
        RECT r{};
        GetWindowRect(h, &r);
        const RECT& a = mons[m].work;
        const int w = r.right - r.left, hh = r.bottom - r.top;
        SetWindowPos(h, nullptr, (a.left + a.right - w) / 2, (a.top + a.bottom - hh) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    // Keep the visible frame of `h` inside monitor m's work area (inside the outer gaps): shift
    // it back on screen, and shrink it if it is larger than the whole area.
    static void fitInto(HWND h, int m) {
        if (!IsWindow(h) || m < 0 || m >= nMon) return;
        RECT fb{};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb)))) GetWindowRect(h, &fb);
        const int go = gapOut();
        const RECT a{mons[m].work.left + go, mons[m].work.top + go, mons[m].work.right - go, mons[m].work.bottom - go};
        if (fb.left >= a.left && fb.top >= a.top && fb.right <= a.right && fb.bottom <= a.bottom) return;
        int w = fb.right - fb.left, hh = fb.bottom - fb.top;
        if (w > a.right - a.left) w = a.right - a.left;
        if (hh > a.bottom - a.top) hh = a.bottom - a.top;
        int x = fb.left, y = fb.top;
        if (x + w > a.right) x = a.right - w;
        if (y + hh > a.bottom) y = a.bottom - hh;
        if (x < a.left) x = a.left;
        if (y < a.top) y = a.top;
        const RECT t = outer(h, {x, y, x + w, y + hh});
        SetWindowPos(h, nullptr, t.left, t.top, t.right - t.left, t.bottom - t.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    // Topmost on/off without moving or activating. Only fullscreen (mode 2) and popped-out
    // windows are made topmost, and they are always put back.
    // The window carries kTopProp while we keep it topmost, so a crash or a forced stop can be
    // undone by recoverHidden() on the next start or --reset.
    static void markTop(HWND h, bool on) { if (on) SetPropW(h, kTopProp, (HANDLE)1); else RemovePropW(h, kTopProp); }
    static void setTopmost(HWND h, bool on) {
        if (!h || !IsWindow(h)) return;
        markTop(h, on);
        SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    static bool pinned(HWND h) { const int i = idx(h); return i >= 0 && (wins[i].flags & F_PIN); }

    // Super+F takes the window's frame off (WS_THICKFRAME, and WS_CAPTION where it has one), as
    // Hyprland gives a fullscreen window exactly its monitor. With its frame the window rect
    // includes the invisible resize borders (6-8 px left, right and bottom, by DPI), so sized over
    // the whole monitor it reached that far onto the neighbouring monitors: a topmost strip there
    // that drew over their windows and took their clicks. It is the standard Windows fullscreen
    // (Chrome's F11, Terminal's own fullscreen do the same). A window region clipped it before,
    // but regions do not mix with DirectComposition windows (Chrome, Terminal, WinUI): their
    // content could go blank. The removed bits are kept in kFsProp (bit 0 marks it set), so
    // leaving fullscreen, turning tiling off or recoverHidden() after a crash puts them back.
    static void bare(HWND h, bool on) {
        if (!h || !IsWindow(h)) return;
        const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        if (on) {
            if (GetPropW(h, kFsProp)) return;
            const LONG_PTR bits = st & (WS_CAPTION | WS_THICKFRAME);
            SetPropW(h, kFsProp, (HANDLE)(bits | 1));
            SetWindowLongPtrW(h, GWL_STYLE, st & ~bits);
        } else {
            const LONG_PTR v = (LONG_PTR)GetPropW(h, kFsProp);
            if (!v) return;
            RemovePropW(h, kFsProp);
            SetWindowLongPtrW(h, GWL_STYLE, st | (v & ~(LONG_PTR)1));
            const int i = idx(h);
            if (i >= 0) wins[i].flags |= F_SETTLE;  // its frame offsets change: placed again once settled
        }
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    // Leave fullscreen on slot `s` (the caller relayouts).
    static void clearFs(int s) {
        const HWND h = fs[s];
        if (!h) return;
        if (fsMode[s] == 2 && !pinned(h)) setTopmost(h, false);
        bare(h, false);
        // A floating window goes back to where it floated, as in Hyprland. The layout never
        // places floating windows, so it used to stay monitor-sized after fullscreen ended (and
        // no longer on top), with the tiles laid out underneath it.
        const int i = idx(h);
        if (i >= 0 && (wins[i].flags & F_FLOAT) && IsWindow(h)) {
            const RECT& p = wins[i].pre;
            if (p.right > p.left && p.bottom > p.top)
                SetWindowPos(h, nullptr, p.left, p.top, p.right - p.left, p.bottom - p.top,
                             SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        fs[s] = nullptr;
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
        w.pre = {};
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
        if (fs[ws] == h) clearFs(ws);
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

    // An app showed a window the shell had hidden with another workspace (its tray icon clicked,
    // a notification opened): it comes to the workspace showing on its monitor, so it is not
    // left showing over the wrong one (and lost there when the app hides it again). The shell's
    // own shows remove kHidProp first, so they never get here.
    static void onShow(HWND h) {
        const int i = ready && Options::tiling ? idx(h) : -1;
        if (i < 0 || !(wins[i].flags & F_HIDDEN) || !GetPropW(h, kHidProp)) return;
        const int m = monOf(wins[i].ws);
        assign(i, slotOf(m, curW[m]));
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
        if (i < 0) return;
        const int s = wins[i].ws;
        last[s] = h;
        if (fs[s] && fs[s] != h) dump(L"focus-under-fullscreen", h);
        // Another window of a fullscreen workspace took focus (Super+arrows, a new window). As
        // Omarchy's `on_focus_under_fullscreen = 1`, a tiled one takes the fullscreen over, in the
        // same mode, so it is never focused behind the fullscreen window; a floating one ends
        // fullscreen. Popped-out windows float above it, so they leave it as it is. Only this
        // workspace is affected: fullscreen windows on other monitors stay.
        if (fs[s] && fs[s] != h && !(wins[i].flags & F_PIN)) {
            const u8 mode = fsMode[s];
            clearFs(s);
            if (!(wins[i].flags & (F_FLOAT | F_MIN | F_HIDDEN))) { fs[s] = h; fsMode[s] = mode; }
            relayout();
        }
        if (!(wins[i].flags & F_FLOAT) && !fs[s]) raiseFloats(s);
    }

    // Floating windows stay above the tiles of their workspace, as in Hyprland, where they are
    // drawn over the tiled layer. Windows raises whatever is clicked, so when a tile takes focus
    // the workspace's floating windows are put back over it, in their own order, without
    // activating them. Not topmost: that is global, and would cover the other monitors' windows.
    static inline HWND zBuf[kMaxWin];
    static inline int zN, zSlot;
    static BOOL CALLBACK enumFloats(HWND h, LPARAM) {  // EnumWindows goes front to back
        const int i = idx(h);
        if (i >= 0 && wins[i].ws == zSlot && (wins[i].flags & (F_FLOAT | F_PIN | F_MIN | F_HIDDEN)) == F_FLOAT &&
            zN < kMaxWin)
            zBuf[zN++] = h;
        return TRUE;
    }
    static void raiseFloats(int s) {
        zN = 0;
        zSlot = s;
        EnumWindows(enumFloats, 0);
        for (int k = zN - 1; k >= 0; --k)  // back to front, so the frontmost ends on top again
            SetWindowPos(zBuf[k], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Super+T floats a tile as Hyprland does: at its last floating size (the size it had before
    // it was tiled, or when it was last floating), centred on its tile, so it stays where it was
    // (its old position could be on another monitor or workspace by now); 10 px smaller all round
    // if that is about the tile's size, so it visibly comes off the layout; on top of the tiles.
    static void floatOut(HWND h, const Win& w) {
        RECT cur{};
        GetWindowRect(h, &cur);
        const int tw = cur.right - cur.left, th = cur.bottom - cur.top;
        int cw = w.orig.right - w.orig.left, ch = w.orig.bottom - w.orig.top;
        if (cw <= 0 || ch <= 0) { cw = tw; ch = th; }
        const UINT wd = GetDpiForWindow(h);
        const int d = sys::scale(10, wd ? (int)wd : dpi());
        if (cw - tw < d && tw - cw < d && ch - th < d && th - ch < d) { cw -= 2 * d; ch -= 2 * d; }
        if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
        SetWindowPos(h, HWND_TOP, (cur.left + cur.right - cw) / 2, (cur.top + cur.bottom - ch) / 2, cw, ch,
                     SWP_NOACTIVATE);
        fitInto(h, monOf(w.ws));
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
        for (int s = 0; s < kSlots; ++s) if (fs[s]) clearFs(s);
        for (int i = 0; i < nWins; ++i) {
            HWND h = wins[i].h;
            if (!IsWindow(h)) continue;
            if (wins[i].flags & F_PIN) setTopmost(h, false);
            if (wins[i].flags & F_HIDDEN) showWin(h);
            const RECT& o = wins[i].orig;
            SetWindowPos(h, nullptr, o.left, o.top, o.right - o.left, o.bottom - o.top,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        stopSettle();
        nWins = 0;
        for (int i = 0; i < kNodes; ++i) nodes[i].used = false;
        for (int i = 0; i < kSlots; ++i) { root[i] = -1; last[i] = fs[i] = nullptr; }
        for (int i = 0; i < kMaxMon; ++i) curW[i] = 0;
        Bus::repaintBar();
    }

    // Show anything a previous (crashed) instance left hidden. Safe to call at any time.
    static BOOL CALLBACK enumUnhide(HWND h, LPARAM) {
        if (GetPropW(h, kHidProp)) showWin(h);
        if (GetPropW(h, kTopProp)) setTopmost(h, false);  // left topmost by Super+F / Super+O
        bare(h, false);                                    // left frameless by Super+F
        if (GetPropW(h, L"awrgn")) { RemovePropW(h, L"awrgn"); SetWindowRgn(h, nullptr, TRUE); }  // 0.3.1 dev builds
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
        if (!Options::tiling || !ready || !h) return;
        dump(L"float", h);
        if (idx(h) < 0) { if (tileable(h)) track(h); else adopt(h); return; }  // unmanaged: into the tiles
        const int i = idx(h);
        Win& w = wins[i];
        if (w.flags & F_PIN) { pop(h); return; }  // Super+T on a popped-out window tiles it again
        if (w.flags & F_FLOAT) {
            w.flags &= (u8)~F_FLOAT;
            GetWindowRect(h, &w.orig);
            insert(w.ws, h);
        } else {
            if (fs[w.ws] == h) clearFs(w.ws);  // it was left placed fullscreen, out of the tiles
            w.flags |= F_FLOAT;
            remove(w.ws, h);
            if (!(w.flags & F_MIN)) floatOut(h, w);
        }
        relayout();
    }

    // Bring a window we do not manage into the tiles of its monitor (see adoptable()).
    static bool adopt(HWND h) {
        if (!Options::tiling || !ready || idx(h) >= 0) return idx(h) >= 0;
        if (nWins >= kMaxWin || !adoptable(h)) return false;
        if (IsIconic(h) || IsZoomed(h)) ShowWindow(h, SW_RESTORE);
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) setTopmost(h, false);
        addWin(h);
        relayout();
        focus(h);
        return true;
    }

    // Super+O, after Omarchy's omarchy-hyprland-window-pop. A tiled window pops out of the layout
    // as a floating 1300x900 window centred on its monitor, pinned (on top, and it follows
    // workspace switches) and with its title bar back so it can be dragged. A window outside the
    // tiles (popped out, floating, or never picked up, e.g. opened by another program) goes into them.
    static void pop(HWND h) {
        if (!Options::tiling || !ready || !h) return;
        dump(L"pop", h);
        if (idx(h) < 0) { if (tileable(h)) { track(h); focus(h); } else adopt(h); return; }
        const int i = idx(h);
        Win& w = wins[i];
        if (w.flags & (F_PIN | F_FLOAT)) {
            if (w.flags & F_PIN) setTopmost(h, false);
            else GetWindowRect(h, &w.orig);  // where Super+T floats it back to
            w.flags &= (u8)~(F_PIN | F_FLOAT);
            if (!(w.flags & F_MIN)) insert(w.ws, h);
        } else {
            if (fs[w.ws] == h) clearFs(w.ws);
            if (!(w.flags & F_FLOAT)) remove(w.ws, h);
            w.flags |= F_FLOAT | F_PIN;
            if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
            const RECT& a = mons[monOf(w.ws)].work;
            const UINT wd = GetDpiForWindow(h);
            const int d = wd ? (int)wd : dpi(), go = gapOut();
            int cw = sys::scale(1300, d), ch = sys::scale(900, d);
            if (cw > a.right - a.left - 2 * go) cw = a.right - a.left - 2 * go;
            if (ch > a.bottom - a.top - 2 * go) ch = a.bottom - a.top - 2 * go;
            const int x = (a.left + a.right - cw) / 2, y = (a.top + a.bottom - ch) / 2;
            const RECT t = outer(h, {x, y, x + cw, y + ch});
            markTop(h, true);
            SetWindowPos(h, HWND_TOPMOST, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE);
        }
        relayout();
        focus(h);
    }

    // 1: full width (keeps gaps), 2: whole monitor, over the bar
    static void fullscreen(HWND h, int mode) {
        dump(mode == 2 ? L"fullscreen" : L"fullwidth", h);
        const int i = ready ? idx(h) : -1;
        if (i < 0 || (wins[i].flags & F_PIN)) return;
        const int s = wins[i].ws;
        if (!visible(s)) return;
        const bool off = fs[s] == h && fsMode[s] == mode;
        const bool was = fs[s] == h;  // switching modes: keep the rect from before the first one
        clearFs(s);
        if (!off) {
            if (!was) GetWindowRect(h, &wins[i].pre);
            fs[s] = h;
            fsMode[s] = (u8)mode;
        }
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
        if (fs[w.ws] == h) clearFs(w.ws);
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
        for (int i = 0; i < nWins; ++i)  // popped-out windows stay on screen and come along
            if (wins[i].ws == from && (wins[i].flags & F_PIN)) wins[i].ws = (u8)to;
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
            w.flags |= F_SETTLE;  // it rescales for the new monitor's DPI and may no longer fit
        }
        assign(i, slotOf(m, curW[m]));
        if (w.flags & F_PIN) centreOn(h, m);  // popped out: centred on the new monitor (pop.lua)
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
        if (m != monOf(wins[i].ws)) {
            if (wins[i].flags & F_FLOAT) wins[i].flags |= F_SETTLE;
            assign(i, slotOf(m, curW[m]));
            if (wins[i].flags & F_PIN) centreOn(h, m);  // popped out: centred where it lands (pop.lua)
        }
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
            const int n = slotOf(p, (w.flags & F_PIN) ? curW[p] : w.ws % kWs);
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
        // Scales may have changed too (a monitor set to 125%, docking): every window rescales
        // itself, so settle them all once they have.
        for (int i = 0; i < nWins; ++i) wins[i].flags |= F_SETTLE;
        relayout();
    }

    // For the bar: is a Super+F (whole monitor) fullscreen window showing on monitor `h`?
    static bool fullscreenOn(HMONITOR h) {
        if (!ready || !Options::tiling || !nMon) return false;
        const int s = slotOf(monIndex(h), curW[monIndex(h)]);
        const HWND f = fs[s];
        return f && fsMode[s] == 2 && IsWindow(f) && IsWindowVisible(f) && !IsIconic(f);
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
