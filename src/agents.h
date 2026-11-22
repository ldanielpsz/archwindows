// Agents panel (Omarchy's omarchy.agents), for Claude Code.
//
// Like Omarchy's collector, it reads Claude Code's own transcripts (~/.claude/projects/**/*.jsonl,
// or $CLAUDE_CONFIG_DIR) on this machine: every assistant reply carries the model that answered and
// its token usage. Tokens = input + output + cache writes + cache reads; a reply streamed over
// several lines counts once (the last line, which has the final counts). Only files touched in
// the last week are read, memory-mapped, on a worker thread; nothing is sent anywhere and no
// credential is read beyond whether a sign-in exists and its plan name.
#pragma once
#include "sys.h"
#include "ui.h"

struct Agents {
    struct Model { char name[40]; unsigned long long tok; };
    struct Result {
        unsigned long long day[7];  // [6] = today, [0] = six days ago
        Model model[8];
        int nModel, files;
        unsigned long long replies;
    };
    static inline Result res, work;
    static inline volatile LONG version, busy;
    static inline DWORD scannedAt;
    static inline int shownVersion;
    static inline wchar_t plan[24];
    static inline bool signedIn, haveDir;

    static bool dir(wchar_t* o) {
        if (GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", o, MAX_PATH) && o[0]) return true;
        if (!GetEnvironmentVariableW(L"USERPROFILE", o, MAX_PATH)) return false;
        sys::wcopy(o + sys::wlen(o), L"\\.claude");
        return true;
    }
    static bool installed() {
        wchar_t d[MAX_PATH];
        return dir(d) && GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES;
    }

    // ---- tiny byte-string helpers (no CRT) ----
    static const char* find(const char* b, const char* e, const char* pat) {
        const int n = lstrlenA(pat);
        for (const char* p = b; p + n <= e; ++p) {
            if (*p != *pat) continue;
            int i = 1;
            while (i < n && p[i] == pat[i]) ++i;
            if (i == n) return p;
        }
        return nullptr;
    }
    static const char* findLast(const char* b, const char* e, const char* pat) {
        const int n = lstrlenA(pat);
        for (const char* p = e - n; p >= b; --p) {
            if (*p != *pat) continue;
            int i = 1;
            while (i < n && p[i] == pat[i]) ++i;
            if (i == n) return p;
        }
        return nullptr;
    }
    static unsigned long long num(const char* b, const char* e, const char* key) {
        const char* p = find(b, e, key);
        if (!p) return 0;
        p += lstrlenA(key);
        unsigned long long v = 0;
        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        return v;
    }
    static int two(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
    static long long localDay(const FILETIME& utc) {
        FILETIME l;
        FileTimeToLocalFileTime(&utc, &l);
        return (long long)((((unsigned long long)l.dwHighDateTime << 32) | l.dwLowDateTime) / 864000000000ull);
    }

    static void addModel(const char* m, int n, unsigned long long tok) {
        for (int i = 0; i < work.nModel; ++i)
            if (lstrlenA(work.model[i].name) == n &&
                CompareStringA(LOCALE_INVARIANT, 0, work.model[i].name, n, m, n) == CSTR_EQUAL) {
                work.model[i].tok += tok;
                return;
            }
        if (work.nModel >= 8 || n >= 40) return;
        Model& o = work.model[work.nModel++];
        for (int i = 0; i < n; ++i) o.name[i] = m[i];
        o.name[n] = 0;
        o.tok = tok;
    }

    static void scanFile(const wchar_t* path, long long today) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(f, &sz);
        HANDLE m = sz.QuadPart ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        const char* base = m ? (const char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (base) {
            ++work.files;
            const char* end = base + sz.QuadPart;
            char lastId[64] = {};
            int lastDay = -1, lastModelN = 0;
            const char* lastModel = nullptr;
            unsigned long long lastTok = 0;
            for (const char* ln = base; ln < end;) {
                const char* le = ln;
                while (le < end && *le != '\n') ++le;
                const char* next = le < end ? le + 1 : end;
                if (find(ln, le, "\"type\":\"assistant\"")) {
                    const char* ts = findLast(ln, le, "\"timestamp\":\"");
                    const char* us = findLast(ln, le, "\"usage\":{");
                    const char* mo = find(ln, le, "\"model\":\"");
                    if (ts && us && mo && ts + 32 < le) {
                        ts += 13;
                        SYSTEMTIME st{};
                        st.wYear = (WORD)(two(ts) * 100 + two(ts + 2));
                        st.wMonth = (WORD)two(ts + 5);
                        st.wDay = (WORD)two(ts + 8);
                        st.wHour = (WORD)two(ts + 11);
                        st.wMinute = (WORD)two(ts + 14);
                        st.wSecond = (WORD)two(ts + 17);
                        FILETIME ft;
                        const long long d = SystemTimeToFileTime(&st, &ft) ? today - localDay(ft) : 99;
                        const unsigned long long tok = num(us, le, "\"input_tokens\":") + num(us, le, "\"output_tokens\":") +
                                                       num(us, le, "\"cache_creation_input_tokens\":") +
                                                       num(us, le, "\"cache_read_input_tokens\":");
                        mo += 9;
                        int mn = 0;
                        while (mo + mn < le && mo[mn] != '"' && mn < 39) ++mn;
                        char id[64] = {};
                        if (const char* ip = find(ln, le, "\"id\":\"msg_")) {
                            ip += 6;
                            for (int i = 0; i < 63 && ip + i < le && ip[i] != '"'; ++i) id[i] = ip[i];
                        }
                        // the same reply streamed over several lines: replace the earlier count
                        if (id[0] && lstrcmpA(id, lastId) == 0 && lastModel) {
                            if (lastDay >= 0) work.day[6 - lastDay] -= lastTok;
                            addModel(lastModel, lastModelN, 0ull - lastTok);
                            --work.replies;
                        }
                        lastTok = 0;
                        lastModel = nullptr;
                        if (tok && d >= 0 && d < 7 && *mo != '<') {  // "<synthetic>": Claude Code's own placeholder
                            work.day[6 - d] += tok;
                            addModel(mo, mn, tok);
                            ++work.replies;
                            lastTok = tok;
                            lastModel = mo;
                            lastModelN = mn;
                            lastDay = (int)d;
                        }
                        lstrcpynA(lastId, id, 64);
                    }
                }
                ln = next;
            }
            UnmapViewOfFile(base);
        }
        if (m) CloseHandle(m);
        CloseHandle(f);
    }

    static void scanDir(const wchar_t* d, int depth, long long today) {
        wchar_t pat[MAX_PATH];
        wsprintfW(pat, L"%s\\*", d);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.') continue;
            wchar_t full[MAX_PATH];
            if (sys::wlen(d) + sys::wlen(fd.cFileName) + 2 >= MAX_PATH) continue;
            wsprintfW(full, L"%s\\%s", d, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) scanDir(full, depth + 1, today); continue; }
            const int n = sys::wlen(fd.cFileName);
            if (n < 6 || !sys::weq(fd.cFileName + n - 6, L".jsonl")) continue;
            if (today - localDay(fd.ftLastWriteTime) > 7) continue;  // nothing from this week can be in it
            scanFile(full, today);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    static DWORD WINAPI worker(LPVOID) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        work = Result{};
        wchar_t d[MAX_PATH];
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        if (dir(d)) {
            sys::wcopy(d + sys::wlen(d), L"\\projects");
            scanDir(d, 0, localDay(now));
        }
        for (int i = 1; i < work.nModel; ++i) {  // biggest first
            Model t = work.model[i];
            int j = i;
            for (; j > 0 && work.model[j - 1].tok < t.tok; --j) work.model[j] = work.model[j - 1];
            work.model[j] = t;
        }
        res = work;
        InterlockedIncrement(&version);
        InterlockedExchange(&busy, 0);
        return 0;
    }
    static void rescan() {
        if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;
        scannedAt = GetTickCount();
        HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else busy = 0;
    }

    static void readAccount() {
        signedIn = false;
        plan[0] = 0;
        wchar_t p[MAX_PATH];
        if (!dir(p)) return;
        haveDir = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
        sys::wcopy(p + sys::wlen(p), L"\\.credentials.json");
        HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        char b[4096];
        DWORD n = 0;
        ReadFile(f, b, sizeof(b) - 1, &n, nullptr);
        CloseHandle(f);
        signedIn = find(b, b + n, "\"claudeAiOauth\"") != nullptr;
        if (const char* s = find(b, b + n, "\"subscriptionType\":\"")) {  // the plan name only
            s += 20;
            int i = 0;
            while (s + i < b + n && s[i] != '"' && i < 20) { plan[i] = (wchar_t)s[i]; ++i; }
            plan[i] = 0;
            if (plan[0] >= L'a' && plan[0] <= L'z') plan[0] -= 32;
        }
    }

    // "claude-opus-5-5" -> "Opus 5.5", "claude-sonnet-4-5-20250929" -> "Sonnet 4.5"
    static void pretty(const char* m, wchar_t* o) {
        const char* p = m;
        if (CompareStringA(LOCALE_INVARIANT, 0, p, 7, "claude-", 7) == CSTR_EQUAL) p += 7;
        int k = 0;
        bool word = true, firstNum = true;
        while (*p && k < 36) {
            const char* s = p;
            while (*p && *p != '-') ++p;
            const int len = (int)(p - s);
            const bool digits = len && s[0] >= '0' && s[0] <= '9';
            if (digits && len >= 8) break;  // release date
            if (k) o[k++] = digits && !firstNum ? L'.' : L' ';
            for (int i = 0; i < len && k < 36; ++i) {
                wchar_t c = (wchar_t)s[i];
                if (word && i == 0 && c >= L'a' && c <= L'z') c -= 32;
                o[k++] = c;
            }
            if (digits) firstNum = false;
            if (*p == '-') ++p;
        }
        o[k] = 0;
    }

    static void launch(const wchar_t* args) {
        wchar_t cmd[96];
        wsprintfW(cmd, L"claude%s%s", args ? L" " : L"", args ? args : L"");
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"wt.exe", cmd, nullptr, SW_SHOWNORMAL) > 32) return;
        wchar_t k[112];
        wsprintfW(k, L"/k %s", cmd);
        ShellExecuteW(nullptr, L"open", L"cmd.exe", k, nullptr, SW_SHOWNORMAL);
    }

    // ---- panel ----
    static void open(HMONITOR) {
        readAccount();
        if (!scannedAt || GetTickCount() - scannedAt > 60000) rescan();
    }
    static void close() {}
    static bool tick() {
        if (version != shownVersion) { shownVersion = version; return true; }
        return false;
    }

    static void ui(Ui& u) {
        static const COLORREF kClaude = RGB(0xD9, 0x77, 0x57);
        wchar_t meta[48];
        if (!haveDir) sys::wcopy(meta, L"NOT INSTALLED");
        else if (!signedIn) sys::wcopy(meta, L"NOT SIGNED IN");
        else if (plan[0]) { wsprintfW(meta, L"%s PLAN", plan); fmt::upper(meta); }
        else sys::wcopy(meta, L"SIGNED IN");
        static const wchar_t* const btn[] = {L"\xE72C", L"\xE756"};
        const int h = u.hero(L"\xE90E", F_OMAHERO, kClaude, L"Claude Code", meta, -1, btn, 2);
        if (h == 2) rescan();
        else if (h == 3) { launch(nullptr); u.wantClose = true; }
        if (haveDir && !signedIn) {
            u.gap(10);
            if (u.notice(L"Claude Code is not signed in on this machine. Click here, or run `claude auth login`, to sign in.",
                         u.p[gen::C_RED])) { launch(L"auth login"); u.wantClose = true; }
        } else if (!haveDir) {
            u.gap(10);
            if (u.notice(L"Claude Code is not installed. Click to open its install page.", u.p.accent())) {
                ShellExecuteW(nullptr, L"open", L"https://docs.anthropic.com/en/docs/claude-code/setup", nullptr, nullptr, SW_SHOWNORMAL);
                u.wantClose = true;
            }
        }
        u.sep();
        u.section(L"TOKENS BY DAY", busy ? L"SCANNING" : nullptr);
        static const wchar_t* const dn[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        SYSTEMTIME t;
        GetLocalTime(&t);
        unsigned long long mx = 0, week = 0;
        for (int i = 0; i < 7; ++i) { if (res.day[i] > mx) mx = res.day[i]; week += res.day[i]; }
        for (int i = 0; i < 7; ++i) {
            wchar_t v[16];
            fmt::count(v, res.day[i]);
            u.meter(i == 6 ? L"Today" : dn[(t.wDayOfWeek + 1 + i) % 7], (long long)res.day[i], (long long)mx, v, i == 6);
        }
        u.sep();
        wchar_t wk[24], wkl[32];
        fmt::count(wk, week);
        wsprintfW(wkl, L"%s THIS WEEK", wk);
        u.section(L"TOKENS BY MODEL", wkl);
        if (!res.nModel) u.row(nullptr, busy ? L"Reading transcripts\x2026" : L"No usage this week");
        for (int i = 0; i < res.nModel; ++i) {
            wchar_t n[40], v[16];
            pretty(res.model[i].name, n);
            fmt::count(v, res.model[i].tok);
            u.strip(n, v);
        }
    }
};
