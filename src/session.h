// Session actions shared by the menu's power buttons and the power panel.
#pragma once
#include "sys.h"

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
    static bool confirm(const wchar_t* q) {
        return MessageBoxW(nullptr, q, L"archwindows", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
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
            case LOGOUT:
                if (confirm(L"Sign out now? Unsaved work in open apps may be lost.")) ExitWindowsEx(EWX_LOGOFF, 0);
                break;
            case RESTART:
                if (confirm(L"Restart this computer now?") && privilege())
                    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
            case SHUTDOWN:
                if (confirm(L"Shut down this computer now?") && privilege())
                    ExitWindowsEx(EWX_POWEROFF | EWX_SHUTDOWN, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
        }
    }
};
