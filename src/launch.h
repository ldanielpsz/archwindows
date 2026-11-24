// Starting programs, documents and URLs for the user.
//
// The installed shell can be built with uiAccess="true" (build.cmd ... uiaccess, signed by
// tools/uiaccess.ps1) so it can tile and drive elevated windows. Windows passes that UIAccess
// right on to every child process, so anything we started ourselves (Terminal, the browser, a
// Start-menu shortcut) could then also send input to elevated windows. When our token has
// UIAccess we therefore ask Explorer to start it (IShellDispatch2::ShellExecute on the desktop's
// shell view): the program becomes Explorer's child with an ordinary token. Without UIAccess, or
// if Explorer cannot be reached, it is a plain ShellExecute.
#pragma once
#include "sys.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>

namespace launch {

inline bool uiAccess() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        HANDLE t = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
            DWORD v = 0, n = 0;
            if (GetTokenInformation(t, TokenUIAccess, &v, sizeof(v), &n) && v) cached = 1;
            CloseHandle(t);
        }
    }
    return cached == 1;
}

// Explorer's ShellExecute reports nothing back, so check up front that a bare program name will
// resolve (PATH, App Paths), as ShellExecute would; callers fall back to another program on false.
inline bool resolvable(const wchar_t* f) {
    bool sep = false;
    for (const wchar_t* p = f; *p; ++p) {
        if (*p == L':' && p - f > 1) return true;  // URL or protocol (https:, ms-settings:)
        if (*p == L'\\' || *p == L'/') sep = true;
    }
    if (sep) return GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, f, nullptr, MAX_PATH, buf, nullptr)) return true;
    wchar_t key[160];
    wsprintfW(key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", f);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS ||
           RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

template <class T> inline void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The desktop's IShellDispatch2 (Raymond Chen's "launch through Explorer" route).
inline bool viaExplorer(const wchar_t* file, const wchar_t* params, int show) {
    IShellWindows* sw = nullptr;
    IDispatch* disp = nullptr;
    IServiceProvider* sp = nullptr;
    IShellBrowser* sb = nullptr;
    IShellView* sv = nullptr;
    IDispatch* bg = nullptr;
    IShellFolderViewDual* fvd = nullptr;
    IDispatch* app = nullptr;
    IShellDispatch2* sd = nullptr;
    bool ok = false;
    VARIANT none{};  // VT_EMPTY
    long hw = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sw))) &&
        sw->FindWindowSW(&none, &none, SWC_DESKTOP, &hw, SWFO_NEEDDISPATCH, &disp) == S_OK && disp &&
        SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) &&
        SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) &&
        SUCCEEDED(sb->QueryActiveShellView(&sv)) &&
        SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg))) &&
        SUCCEEDED(bg->QueryInterface(IID_PPV_ARGS(&fvd))) &&
        SUCCEEDED(fvd->get_Application(&app)) && app &&
        SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&sd)))) {
        BSTR f = SysAllocString(file);
        VARIANT a{}, dir{}, op{}, sh{};
        if (params && *params) { a.vt = VT_BSTR; a.bstrVal = SysAllocString(params); }
        op.vt = VT_BSTR;
        op.bstrVal = SysAllocString(L"open");
        sh.vt = VT_I4;
        sh.lVal = show;
        ok = f && SUCCEEDED(sd->ShellExecute(f, a, dir, op, sh));
        SysFreeString(f);
        if (a.vt == VT_BSTR) SysFreeString(a.bstrVal);
        SysFreeString(op.bstrVal);
    }
    rel(sd); rel(app); rel(fvd); rel(bg); rel(sv); rel(sb); rel(sp); rel(disp); rel(sw);
    return ok;
}

// When the user last started something through run(), 0 if nothing is pending. Started through
// Explorer, a program is not allowed to take the foreground itself (Explorer is not the foreground
// app), so its window would open behind the focused one; spawn.h focuses the first app window
// shown soon after this instead, as a keyboard-first shell must.
inline DWORD launchedAt;

// Open `file` (program, document, folder or URL) with optional parameters. `focus`: its first
// window gets the keyboard focus (false for background starts that open no window).
inline bool run(const wchar_t* file, const wchar_t* params = nullptr, int show = SW_SHOWNORMAL, bool focus = true) {
    if (focus) launchedAt = GetTickCount() | 1;
    if (uiAccess()) {
        if (!resolvable(file)) return false;
        if (viaExplorer(file, params, show)) return true;
    }
    return (INT_PTR)ShellExecuteW(nullptr, L"open", file, params, nullptr, show) > 32;
}

}  // namespace launch
