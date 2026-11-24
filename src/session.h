// Session actions shared by the menu's power buttons and the power panel.
#pragma once
#include "sys.h"
#include "confirm.h"

struct Session {
    enum Act : int { LOCK, SLEEP, LOGOUT, RESTART, SHUTDOWN, kCount };
    static constexpr const wchar_t* glyph[kCount] = {L"\xE72E", L"\xE708", L"\xE748", L"\xE777", L"\xE7E8"};
    static constexpr const wchar_t* label[kCount] = {L"Lock", L"Sleep", L"Sign out", L"Restart", L"Shut down"};

    static bool privilege() {
        HANDLE t;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        const bool ok = AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
        CloseHandle(t);
        return ok;
    }
    // Themed prompt (confirm.h); the action runs only once it is confirmed.
    static void confirm(int a, const wchar_t* q) { Confirm::ask(glyph[a], label[a], q, label[a], end, (void*)(LONG_PTR)a); }
    static void end(void* p) {
        switch ((int)(LONG_PTR)p) {
            case LOGOUT: ExitWindowsEx(EWX_LOGOFF, 0); break;
            case RESTART:
                if (privilege()) ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
            case SHUTDOWN:
                if (privilege())
                    ExitWindowsEx(EWX_POWEROFF | EWX_SHUTDOWN, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
        }
    }
    // Restart, shut down and sign out ask first (they end every app); lock and sleep do not.
    static void run(int a) {
        switch (a) {
            case LOCK: LockWorkStation(); break;
            case SLEEP: {
                typedef BOOLEAN(WINAPI * Suspend)(BOOLEAN, BOOLEAN, BOOLEAN);
                HMODULE pp = LoadLibraryW(L"powrprof.dll");
                auto f = pp ? (Suspend)GetProcAddress(pp, "SetSuspendState") : nullptr;
                privilege();
                if (f) f(FALSE, FALSE, FALSE);
                break;
            }
            case LOGOUT: confirm(a, L"Sign out now? Unsaved work in open apps may be lost."); break;
            case RESTART: confirm(a, L"Restart this computer now? Unsaved work in open apps may be lost."); break;
            case SHUTDOWN: confirm(a, L"Shut down this computer now? Unsaved work in open apps may be lost."); break;
        }
    }
};
