// Super-key chords (a tiling-WM style key map) via a minimal low-level keyboard hook.
//
// RegisterHotKey cannot claim Win+<key> combinations that Windows already owns (Win+Space,
// Win+Q/W/F/T/M, Win+1..9, Win+arrows, Win+Tab ...), so this is the one place a WH_KEYBOARD_LL
// hook is justified. It is kept as cheap as possible: no allocation, no calls except a
// PostMessage when a chord matches, and it only swallows keys while Win is held and the chord
// is in the table. Tiling chords are only swallowed while tiling is on, so with tiling off
// Windows keeps all of its own Win-key shortcuts; only Win+Space (the menu), the launchers
// (Win+Enter, Win+Shift+B) and the theme picker (Win+Ctrl+T), which Windows leaves unbound,
// are always ours.
//
// Win released alone (or after a swallowed chord) would open Start, so one dummy keystroke
// (VK 0xE8, unassigned) is injected before Win-up, the same trick AutoHotkey uses. This makes
// the Start menu unreachable from the Win key regardless of the tiling option.
#pragma once
#include "sys.h"
#include "options.h"

enum Act : u8 {
    K_NONE, K_MENU, K_CLOSE, K_SPLIT, K_FLOAT, K_FULL, K_FULLW, K_MIN,
    K_FOCUS, K_SWAP, K_WS, K_MOVE, K_WSREL, K_RESIZE, K_MONMOVE, K_TERMINAL, K_BROWSER, K_PICKER, K_POP,
};

struct KeyChord { u8 vk; u8 mods; u8 act; signed char arg; bool tilingOnly; };

enum : u8 { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };

// Tiling-WM style keys (Super = Win key). Resize: Super+-/= width, +Shift height.
inline constexpr KeyChord kChords[] = {
    {VK_SPACE, 0, K_MENU, 0, false},
    // Launchers: always on (Windows binds neither), and the app opens on the focused monitor.
    {VK_RETURN, 0, K_TERMINAL, 0, false},  {'B', M_SHIFT, K_BROWSER, 0, false},
    {'T', M_CTRL, K_PICKER, 0, false},     // theme picker; always on (Windows leaves Win+Ctrl+T unbound)
    {'Q', 0, K_CLOSE, 0, true},        {'W', 0, K_CLOSE, 0, true},
    {'J', 0, K_SPLIT, 0, true},        {'T', 0, K_FLOAT, 0, true},
    {'F', 0, K_FULL, 2, true},         {'F', M_ALT, K_FULLW, 1, true},
    {'M', 0, K_MIN, 0, true},         {'O', 0, K_POP, 0, true},   // Omarchy: pop window out (float & pin)
    {VK_LEFT, 0, K_FOCUS, 0, true},    {VK_RIGHT, 0, K_FOCUS, 1, true},
    {VK_UP, 0, K_FOCUS, 2, true},      {VK_DOWN, 0, K_FOCUS, 3, true},
    {VK_LEFT, M_SHIFT, K_SWAP, 0, true},  {VK_RIGHT, M_SHIFT, K_SWAP, 1, true},
    {VK_UP, M_SHIFT, K_SWAP, 2, true},    {VK_DOWN, M_SHIFT, K_SWAP, 3, true},
    {'1', 0, K_WS, 0, true}, {'2', 0, K_WS, 1, true}, {'3', 0, K_WS, 2, true}, {'4', 0, K_WS, 3, true},
    {'5', 0, K_WS, 4, true}, {'6', 0, K_WS, 5, true}, {'7', 0, K_WS, 6, true}, {'8', 0, K_WS, 7, true},
    {'9', 0, K_WS, 8, true}, {'0', 0, K_WS, 9, true},
    {'1', M_SHIFT, K_MOVE, 0, true}, {'2', M_SHIFT, K_MOVE, 1, true}, {'3', M_SHIFT, K_MOVE, 2, true},
    {'4', M_SHIFT, K_MOVE, 3, true}, {'5', M_SHIFT, K_MOVE, 4, true}, {'6', M_SHIFT, K_MOVE, 5, true},
    {'7', M_SHIFT, K_MOVE, 6, true}, {'8', M_SHIFT, K_MOVE, 7, true}, {'9', M_SHIFT, K_MOVE, 8, true},
    {'0', M_SHIFT, K_MOVE, 9, true},
    {VK_TAB, 0, K_WSREL, 1, true},     {VK_TAB, M_SHIFT, K_WSREL, -1, true},
    {VK_OEM_MINUS, 0, K_RESIZE, 0, true},        {VK_OEM_PLUS, 0, K_RESIZE, 1, true},
    {VK_OEM_MINUS, M_SHIFT, K_RESIZE, 2, true},  {VK_OEM_PLUS, M_SHIFT, K_RESIZE, 3, true},
    {VK_LEFT, M_ALT, K_MONMOVE, 0, true},        {VK_RIGHT, M_ALT, K_MONMOVE, 1, true},
    {VK_UP, M_ALT, K_MONMOVE, 2, true},          {VK_DOWN, M_ALT, K_MONMOVE, 3, true},
};
inline constexpr int kChordCount = (int)(sizeof(kChords) / sizeof(kChords[0]));

inline constexpr UINT WM_CHORD = WM_APP + 1;
inline constexpr ULONG_PTR kKeyMagic = 0x41574B59;  // marks our own injected keystrokes

struct LowLevelKeys {
    static inline HHOOK hook;
    static inline HWND target;
    static inline bool masked;
    static inline u8 mods;
    // Win keys the hook saw go down and not up yet (bit 0 left, bit 1 right). A chord needs this
    // AND Windows' own key state: either alone can go stale. Win+L (or Ctrl+Alt+Del, a UAC prompt)
    // releases Win on the secure desktop, where neither the hook nor the key state sees it, so
    // Win stayed "held" and plain typing ran chords: Shift+1 ("!") moved the window, Q closed it.
    // Switching desktops forgets it for the same reason.
    static inline u8 win;
    static inline HWINEVENTHOOK deskHook;
    static void CALLBACK onDesktop(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { win = 0; masked = false; }

    static int lookup(u8 vk) {
        for (int i = 0; i < kChordCount; ++i)
            if (kChords[i].vk == vk && kChords[i].mods == mods && (!kChords[i].tilingOnly || Options::tiling))
                return i;
        return -1;
    }

    static void maskStart() {
        INPUT in[2] = {};
        for (int i = 0; i < 2; ++i) {
            in[i].type = INPUT_KEYBOARD;
            in[i].ki.wVk = 0xE8;
            in[i].ki.dwFlags = i ? KEYEVENTF_KEYUP : 0;
            in[i].ki.dwExtraInfo = kKeyMagic;
        }
        SendInput(2, in, sizeof(INPUT));
    }

    // Modifier state at this instant. Reading the live keyboard state is more robust than
    // tracking key events (a modifier already held when the hook started, or injected input).
    static bool held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
    static u8 currentMods() {
        return (u8)((held(VK_SHIFT) ? M_SHIFT : 0) | (held(VK_CONTROL) ? M_CTRL : 0) | (held(VK_MENU) ? M_ALT : 0));
    }

    static LRESULT CALLBACK proc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION) {
            const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
            if (k->dwExtraInfo != kKeyMagic) {
                const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
                const DWORD vk = k->vkCode;
                if (vk == VK_LWIN || vk == VK_RWIN) {
                    const u8 bit = vk == VK_LWIN ? 1 : 2;
                    if (down) win |= bit; else win &= (u8)~bit;
                    // Win released: if no chord masked it yet, a bare tap would open Start. Inject
                    // the dummy key now, while Win is still logically down, so Windows sees a
                    // shortcut instead. Start never opens; other Win+<key> combos are untouched.
                    if (!down) {
                        if (!masked) maskStart();
                        masked = false;
                    }
                } else if (vk < 256 && vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_LCONTROL &&
                           vk != VK_RCONTROL && vk != VK_LMENU && vk != VK_RMENU && vk != VK_SHIFT &&
                           vk != VK_CONTROL && vk != VK_MENU && win && (held(VK_LWIN) || held(VK_RWIN))) {
                    mods = currentMods();
                    const int i = lookup((u8)vk);
                    if (i >= 0) {
                        if (down) {
                            if (!masked) { masked = true; maskStart(); }
                            PostMessageW(target, WM_CHORD, (WPARAM)i, 0);
                        }
                        return 1;  // swallow both the key-down and the key-up
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    static void start(HWND bar) {
        target = bar;
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, proc, GetModuleHandleW(nullptr), 0);
        deskHook = SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, nullptr, onDesktop, 0, 0,
                                   WINEVENT_OUTOFCONTEXT);
    }
    static void stop() {
        if (hook) UnhookWindowsHookEx(hook);
        if (deskHook) UnhookWinEvent(deskHook);
        hook = nullptr;
        deskHook = nullptr;
    }
};
