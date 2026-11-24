// Network panel (Omarchy's omarchy.network): Wi-Fi status, live link stats, DNS provider, and
// the known / other networks list.
//
// Wi-Fi goes through the native WLAN API (wlanapi.dll, loaded on demand so machines without the
// WLAN service still start); addresses and byte counters come from IP Helper; the ping runs on a
// worker thread that lives only while the panel is open. Switching DNS provider needs admin
// rights, so it runs one elevated PowerShell command (Windows shows its UAC prompt).
#pragma once
#include "sys.h"
#include "launch.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <wlanapi.h>
#include <shellapi.h>
#include "ui.h"

struct Wlan {  // the few WLAN entry points we use, resolved at run time
    typedef DWORD(WINAPI* Open_t)(DWORD, PVOID, PDWORD, PHANDLE);
    typedef DWORD(WINAPI* Enum_t)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
    typedef DWORD(WINAPI* List_t)(HANDLE, const GUID*, DWORD, PVOID, PWLAN_AVAILABLE_NETWORK_LIST*);
    typedef DWORD(WINAPI* Query_t)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
    typedef DWORD(WINAPI* Set_t)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, const PVOID, PVOID);
    typedef DWORD(WINAPI* Connect_t)(HANDLE, const GUID*, const PWLAN_CONNECTION_PARAMETERS, PVOID);
    typedef DWORD(WINAPI* Scan_t)(HANDLE, const GUID*, const PDOT11_SSID, const PWLAN_RAW_DATA, PVOID);
    typedef VOID(WINAPI* Free_t)(PVOID);
    static inline Open_t open;
    static inline Enum_t enumIf;
    static inline List_t list;
    static inline Query_t query;
    static inline Set_t set;
    static inline Connect_t connect;
    static inline Scan_t scan;
    static inline Free_t freeMem;
    static inline HANDLE h;
    static inline bool tried;

    static bool ready() {
        if (h) return true;
        if (tried) return false;
        tried = true;
        HMODULE m = LoadLibraryW(L"wlanapi.dll");
        if (!m) return false;
        open = (Open_t)GetProcAddress(m, "WlanOpenHandle");
        enumIf = (Enum_t)GetProcAddress(m, "WlanEnumInterfaces");
        list = (List_t)GetProcAddress(m, "WlanGetAvailableNetworkList");
        query = (Query_t)GetProcAddress(m, "WlanQueryInterface");
        set = (Set_t)GetProcAddress(m, "WlanSetInterface");
        connect = (Connect_t)GetProcAddress(m, "WlanConnect");
        scan = (Scan_t)GetProcAddress(m, "WlanScan");
        freeMem = (Free_t)GetProcAddress(m, "WlanFreeMemory");
        if (!open || !enumIf || !list || !query || !set || !connect || !scan || !freeMem) return false;
        DWORD ver = 0;
        return open(2, nullptr, &ver, &h) == ERROR_SUCCESS;
    }
    // The first wireless interface, if any.
    static bool iface(GUID& g) {
        if (!ready()) return false;
        PWLAN_INTERFACE_INFO_LIST l = nullptr;
        if (enumIf(h, nullptr, &l) != ERROR_SUCCESS || !l) return false;
        const bool ok = l->dwNumberOfItems > 0;
        if (ok) g = l->InterfaceInfo[0].InterfaceGuid;
        freeMem(l);
        return ok;
    }
};

struct Net {
    struct Row { wchar_t ssid[33]; wchar_t profile[64]; int signal; bool known, secure, connected; };
    static constexpr int kRows = 24, kPings = 20;

    // ---- state (refreshed while the panel is open; the bar reads the cheap subset) ----
    static inline GUID ifGuid;
    static inline bool hasWifi, radioOn, connected, wired;
    static inline wchar_t ssid[33], ip[48], gateway[48], alias[128];
    static inline int signal, dns;  // dns: 0 DHCP, 1 Cloudflare, 2 Google, 3 custom
    static inline NET_LUID luid;
    static inline unsigned long long inOct, outOct, inRate, outRate;
    static inline DWORD lastStat;
    static inline Row rows[kRows];
    static inline int nRows;
    static inline DWORD lastList, lastBar;
    static inline int barGlyphCache;
    static inline HMONITOR monitor;

    // ---- ping worker ----
    static inline volatile LONG pingGen, rtt = -1;
    static inline volatile LONG pingRing[kPings];  // 0 empty, 1 ok, 2 lost
    static inline volatile LONG pingPos;

    static DWORD WINAPI pinger(LPVOID arg) {
        const LONG g = (LONG)(INT_PTR)arg;
        HANDLE ic = IcmpCreateFile();
        if (ic == INVALID_HANDLE_VALUE) return 0;
        char data[8] = "awping";
        alignas(8) char reply[sizeof(ICMP_ECHO_REPLY) + 64];
        const IPAddr target = 0x01010101;  // 1.1.1.1
        while (pingGen == g) {
            const DWORD t0 = GetTickCount();
            const DWORD n = IcmpSendEcho(ic, target, data, sizeof(data), nullptr, reply, sizeof(reply), 1000);
            const auto* rp = (const ICMP_ECHO_REPLY*)reply;
            const bool ok = n > 0 && rp->Status == IP_SUCCESS;
            if (ok) rtt = (LONG)rp->RoundTripTime;
            pingRing[pingPos % kPings] = ok ? 1 : 2;
            InterlockedIncrement(&pingPos);
            const DWORD spent = GetTickCount() - t0;
            if (spent < 1000) Sleep(1000 - spent);
        }
        IcmpCloseHandle(ic);
        return 0;
    }
    static int lossPct() {
        int sent = 0, lost = 0;
        for (int i = 0; i < kPings; ++i) { sent += pingRing[i] != 0; lost += pingRing[i] == 2; }
        return sent ? lost * 100 / sent : 0;
    }

    // ---- adapters ----
    static void addrText(const SOCKADDR* sa, wchar_t* o) {
        o[0] = 0;
        if (sa && sa->sa_family == AF_INET) {
            const u8* b = (const u8*)&((const sockaddr_in*)sa)->sin_addr;
            wsprintfW(o, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
    }
    static unsigned ipv4(const SOCKADDR* sa) {
        return sa && sa->sa_family == AF_INET ? ((const sockaddr_in*)sa)->sin_addr.S_un.S_addr : 0;
    }
    static void guidText(const GUID& g, wchar_t* o) {
        wsprintfW(o, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3, g.Data4[0],
                  g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }
    static bool sameNameA(const char* a, const wchar_t* w) {
        for (; *a && *w; ++a, ++w) {
            wchar_t x = (wchar_t)*a, y = *w;
            if (x >= L'a' && x <= L'z') x -= 32;
            if (y >= L'a' && y <= L'z') y -= 32;
            if (x != y) return false;
        }
        return !*a && !*w;
    }

    // The Wi-Fi adapter when there is one, otherwise the first adapter that is up with a gateway.
    static void adapters() {
        ULONG sz = 24 * 1024;
        auto* buf = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, sz);
        if (!buf) return;
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                 nullptr, buf, &sz) != NO_ERROR) {
            HeapFree(GetProcessHeap(), 0, buf);
            return;
        }
        wchar_t want[40];
        guidText(ifGuid, want);
        IP_ADAPTER_ADDRESSES* pick = nullptr;
        for (auto* a = buf; a; a = a->Next)
            if (hasWifi && sameNameA(a->AdapterName, want)) { pick = a; break; }
        if (!pick || pick->OperStatus != IfOperStatusUp || !pick->FirstGatewayAddress)
            for (auto* a = buf; a; a = a->Next)
                if (a->OperStatus == IfOperStatusUp && a->FirstGatewayAddress && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
                    a->IfType != IF_TYPE_TUNNEL) { pick = a; break; }
        ip[0] = gateway[0] = alias[0] = 0;
        wired = false;
        if (pick) {
            wired = pick->IfType != IF_TYPE_IEEE80211;
            luid = pick->Luid;
            addrText(pick->FirstUnicastAddress ? pick->FirstUnicastAddress->Address.lpSockaddr : nullptr, ip);
            addrText(pick->FirstGatewayAddress ? pick->FirstGatewayAddress->Address.lpSockaddr : nullptr, gateway);
            lstrcpynW(alias, pick->FriendlyName, 128);
            dns = 3;
            unsigned d0 = pick->FirstDnsServerAddress ? ipv4(pick->FirstDnsServerAddress->Address.lpSockaddr) : 0;
            const u8* b = (const u8*)&d0;
            if ((b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) || (b[0] == 1 && b[1] == 0 && b[2] == 0 && b[3] == 1)) dns = 1;
            else if (b[0] == 8 && b[1] == 8 && (b[2] == 8 || b[2] == 4) && (b[3] == 8 || b[3] == 4)) dns = 2;
            else {  // no static servers in the interface's settings: they came from DHCP
                wchar_t key[160], ns[256];
                wchar_t g[64];
                int i = 0;
                for (const char* c = pick->AdapterName; *c && i < 63; ++c) g[i++] = (wchar_t)*c;
                g[i] = 0;
                wsprintfW(key, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\%s", g);
                DWORD nsz = sizeof(ns);
                ns[0] = 0;
                RegGetValueW(HKEY_LOCAL_MACHINE, key, L"NameServer", RRF_RT_REG_SZ, nullptr, ns, &nsz);
                if (!ns[0]) dns = 0;
            }
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }

    static void counters() {
        if (!ip[0]) { inRate = outRate = 0; return; }
        MIB_IF_ROW2 r{};
        r.InterfaceLuid = luid;
        if (GetIfEntry2(&r) != NO_ERROR) return;
        const DWORD now = GetTickCount();
        if (lastStat && now > lastStat && inOct) {
            const unsigned long long dt = now - lastStat;
            inRate = (r.InOctets - inOct) * 1000 / dt;
            outRate = (r.OutOctets - outOct) * 1000 / dt;
        }
        inOct = r.InOctets;
        outOct = r.OutOctets;
        lastStat = now;
    }

    static void ssidText(const DOT11_SSID& s, wchar_t* o) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, o, 32);
        o[n > 0 ? n : 0] = 0;
    }

    static void wifiState() {
        hasWifi = Wlan::iface(ifGuid);
        connected = false;
        ssid[0] = 0;
        signal = 0;
        radioOn = hasWifi;
        if (!hasWifi) return;
        DWORD sz = 0;
        PVOID data = nullptr;
        WLAN_OPCODE_VALUE_TYPE vt;
        if (Wlan::query(Wlan::h, &ifGuid, wlan_intf_opcode_radio_state, nullptr, &sz, &data, &vt) == ERROR_SUCCESS && data) {
            auto* rs = (PWLAN_RADIO_STATE)data;
            radioOn = false;
            for (DWORD i = 0; i < rs->dwNumberOfPhys; ++i)
                radioOn |= rs->PhyRadioState[i].dot11SoftwareRadioState == dot11_radio_state_on &&
                           rs->PhyRadioState[i].dot11HardwareRadioState == dot11_radio_state_on;
            Wlan::freeMem(data);
        }
        data = nullptr;
        if (Wlan::query(Wlan::h, &ifGuid, wlan_intf_opcode_current_connection, nullptr, &sz, &data, &vt) == ERROR_SUCCESS && data) {
            auto* c = (PWLAN_CONNECTION_ATTRIBUTES)data;
            if (c->isState == wlan_interface_state_connected) {
                connected = true;
                ssidText(c->wlanAssociationAttributes.dot11Ssid, ssid);
                signal = (int)c->wlanAssociationAttributes.wlanSignalQuality;
            }
            Wlan::freeMem(data);
        }
    }

    static void networks() {
        nRows = 0;
        if (!hasWifi || !radioOn) return;
        PWLAN_AVAILABLE_NETWORK_LIST l = nullptr;
        if (Wlan::list(Wlan::h, &ifGuid, 0, nullptr, &l) != ERROR_SUCCESS || !l) return;
        for (DWORD i = 0; i < l->dwNumberOfItems; ++i) {
            const WLAN_AVAILABLE_NETWORK& n = l->Network[i];
            if (!n.dot11Ssid.uSSIDLength) continue;  // hidden network
            Row t{};
            ssidText(n.dot11Ssid, t.ssid);
            lstrcpynW(t.profile, n.strProfileName, 64);
            t.signal = (int)n.wlanSignalQuality;
            t.known = (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
            t.connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
            t.secure = n.bSecurityEnabled != 0;
            int dup = -1;  // one row per SSID: the profile entry beats the bare one, connected beats both
            for (int k = 0; k < nRows; ++k) if (sys::weq(rows[k].ssid, t.ssid)) { dup = k; break; }
            if (dup >= 0) {
                Row& o = rows[dup];
                if (t.connected || (t.known && !o.connected && !o.known)) o = t;
                continue;
            }
            if (nRows < kRows) rows[nRows++] = t;
        }
        Wlan::freeMem(l);
        for (int i = 1; i < nRows; ++i) {  // known first, then by signal
            Row t = rows[i];
            int j = i;
            auto rank = [](const Row& r) { return (r.connected ? 300 : 0) + (r.known ? 150 : 0) + r.signal; };
            for (; j > 0 && rank(rows[j - 1]) < rank(t); --j) rows[j] = rows[j - 1];
            rows[j] = t;
        }
    }

    static const wchar_t* signalGlyph(int q) {
        return q >= 75 ? L"\xE701" : q >= 50 ? L"\xE874" : q >= 25 ? L"\xE873" : L"\xE872";
    }

    // ---- bar ----
    static const wchar_t* barGlyph() {
        const DWORD now = GetTickCount();
        if (!lastBar || now - lastBar > 4000) {
            lastBar = now;
            wifiState();
            if (!connected) { if (!ip[0] || hasWifi) adapters(); }
            barGlyphCache = connected ? (signal >= 75 ? 0 : signal >= 50 ? 1 : signal >= 25 ? 2 : 3)
                            : (wired && ip[0]) ? 4 : hasWifi && !radioOn ? 6 : 5;
        }
        static const wchar_t* const g[] = {L"\xE701", L"\xE874", L"\xE873", L"\xE872", L"\xE839", L"\xEB5E", L"\xEB5E"};
        return g[barGlyphCache];
    }
    static void invalidateBar() { lastBar = 0; }

    // ---- panel ----
    static void open(HMONITOR m) {
        monitor = m;
        wifiState();
        adapters();
        lastStat = 0;
        inOct = outOct = inRate = outRate = 0;
        counters();
        if (hasWifi && radioOn) Wlan::scan(Wlan::h, &ifGuid, nullptr, nullptr, nullptr);
        networks();
        lastList = GetTickCount();
        rtt = -1;
        pingPos = 0;
        for (int i = 0; i < kPings; ++i) pingRing[i] = 0;
        HANDLE t = CreateThread(nullptr, 0, pinger, (LPVOID)(INT_PTR)InterlockedIncrement(&pingGen), 0, nullptr);
        if (t) CloseHandle(t);
    }
    static void close() { InterlockedIncrement(&pingGen); invalidateBar(); }
    static bool tick() {
        counters();
        if (GetTickCount() - lastList > 4000) { wifiState(); adapters(); networks(); lastList = GetTickCount(); }
        return true;
    }

    static void setRadio(bool on) {
        if (!hasWifi) return;
        WLAN_PHY_RADIO_STATE st{};
        st.dwPhyIndex = 0;
        st.dot11SoftwareRadioState = on ? dot11_radio_state_on : dot11_radio_state_off;
        if (Wlan::set(Wlan::h, &ifGuid, wlan_intf_opcode_radio_state, sizeof(st), &st, nullptr) != ERROR_SUCCESS)
            launch::run(L"ms-settings:network-wifi", nullptr);
    }
    static void join(const Row& r) {
        if (r.connected) return;
        if (!r.known) {  // needs a password: Windows' own network list asks for it
            launch::run(L"ms-availablenetworks:", nullptr);
            return;
        }
        WLAN_CONNECTION_PARAMETERS cp{};
        cp.wlanConnectionMode = wlan_connection_mode_profile;
        cp.strProfile = r.profile;
        cp.dot11BssType = dot11_BSS_type_any;
        Wlan::connect(Wlan::h, &ifGuid, &cp, nullptr);
    }
    static void setDns(int which) {
        if (which == 3 || !alias[0]) {
            launch::run(wired ? L"ms-settings:network-ethernet" : L"ms-settings:network-wifi");
            return;
        }
        wchar_t a[260], *o = a;  // alias inside single quotes: double any quote it contains
        for (const wchar_t* c = alias; *c && o < a + 250; ++c) { if (*c == L'\'') *o++ = L'\''; *o++ = *c; }
        *o = 0;
        wchar_t cmd[600];
        if (which == 0) wsprintfW(cmd, L"-NoProfile -WindowStyle Hidden -Command \"Set-DnsClientServerAddress -InterfaceAlias '%s' -ResetServerAddresses\"", a);
        else wsprintfW(cmd, L"-NoProfile -WindowStyle Hidden -Command \"Set-DnsClientServerAddress -InterfaceAlias '%s' -ServerAddresses %s\"", a,
                       which == 1 ? L"1.1.1.1,1.0.0.1" : L"8.8.8.8,8.8.4.4");
        SHELLEXECUTEINFOW se{sizeof(se)};
        se.lpVerb = L"runas";
        se.lpFile = L"powershell.exe";
        se.lpParameters = cmd;
        se.nShow = SW_HIDE;
        ShellExecuteExW(&se);
    }

    static void ui(Ui& u) {
        static const wchar_t* const phrases[] = {L"Wiring bits", L"Handling packets", L"Sorting frames", L"Hauling bytes",
                                                 L"Routing crumbs", L"Counting collisions", L"Bending light"};
        SYSTEMTIME t;
        GetLocalTime(&t);
        const wchar_t* title = connected ? ssid : (wired && ip[0]) ? L"Ethernet" : hasWifi && !radioOn ? L"Wi-Fi off"
                               : hasWifi ? L"Not connected" : L"Offline";
        const bool online = connected || (wired && ip[0]);
        wchar_t meta[40];
        sys::wcopy(meta, online ? phrases[t.wMinute % 7] : hasWifi && !radioOn ? L"Radio off" : L"No link");
        fmt::upper(meta);
        const wchar_t* glyph = connected ? signalGlyph(signal) : (wired && ip[0]) ? L"\xE839" : L"\xEB5E";
        static const wchar_t* const btn[] = {L"\xEC4A"};
        const int h = u.hero(glyph, F_HERO, u.p.fg(), title, meta, hasWifi ? (radioOn ? 1 : 0) : -1, btn, 1);
        if (h == 1) { setRadio(!radioOn); Sleep(150); wifiState(); networks(); invalidateBar(); }
        else if (h == 2) { launch::run(L"https://speed.cloudflare.com/"); u.wantClose = true; }
        u.gap(10);

        if (online) {
            wchar_t a[24], b[24], c[24], d[24];
            if (rtt >= 0) wsprintfW(a, L"%ld ms", rtt); else sys::wcopy(a, L"\x2014");
            wsprintfW(b, L"%d%%", lossPct());
            u.stat(L"Ping", a, L"Packet Loss", b);
            fmt::bytes(c, inRate, L"/s");
            fmt::bytes(d, outRate, L"/s");
            u.stat(L"Receiving", c, L"Sending", d);
            fmt::bytes(c, inOct);
            fmt::bytes(d, outOct);
            u.stat(L"Downloaded", c, L"Uploaded", d);
            u.stat(L"IP Address", ip, L"Gateway", gateway);
            u.sep();
            u.section(L"DNS PROVIDER");
            static const wchar_t* const dl[] = {L"DHCP", L"Cloudflare", L"Google", L"Custom"};
            const int k = u.buttons(dl, 4, dns);
            if (k >= 0) { setDns(k); if (k != 3) dns = k; else u.wantClose = true; }
        }
        if (!hasWifi || !radioOn) return;
        bool anyKnown = false, anyOther = false;
        for (int i = 0; i < nRows; ++i) { anyKnown |= rows[i].known; anyOther |= !rows[i].known; }
        const int y0 = u.y;
        if (anyKnown) {
            u.sep();
            u.section(L"KNOWN NETWORKS");
            for (int i = 0; i < nRows; ++i)
                if (rows[i].known && u.row(signalGlyph(rows[i].signal), rows[i].ssid, rows[i].connected ? L"Connected" : nullptr,
                                           rows[i].connected, rows[i].secure ? L"\xE72E" : nullptr))
                    join(rows[i]);
        }
        if (anyOther) {
            if (u.y > y0 || online) u.sep();
            u.section(L"OTHER NETWORKS");
            int shown = 0;
            for (int i = 0; i < nRows && shown < 8; ++i)
                if (!rows[i].known && ++shown &&
                    u.row(signalGlyph(rows[i].signal), rows[i].ssid, nullptr, false, rows[i].secure ? L"\xE72E" : nullptr)) {
                    join(rows[i]);
                    u.wantClose = true;
                }
        }
        if (!nRows) { u.sep(); u.row(nullptr, L"Scanning\x2026"); }
    }
};
