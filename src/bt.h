// Bluetooth panel (Omarchy's omarchy.bluetooth): radio switch, connected / paired / available
// devices; connect, disconnect, pair, forget.
//
// - Devices, pairing and forgetting: the Win32 Bluetooth API (bthprops.cpl, loaded on demand).
// - Radio on/off: Win32 has no call for it, so it goes through WinRT's Windows.Devices.Radios,
//   used through its plain COM (ABI) interfaces on a worker thread, no C++/WinRT or WRL.
// - Connect/disconnect: Windows connects keyboards, mice and the like by itself; for headphones
//   and speakers it uses the Bluetooth audio driver's one-shot reconnect/disconnect requests
//   (KSPROPSETID_BtAudio on the endpoint's KS filter), the same thing the Settings page does.
//   Other device types open Settings.
// - Available devices: a classic inquiry (about 5 s) repeated on a worker thread while the panel
//   is open. Bluetooth LE devices that only advertise are left to Settings ("Add device").
#pragma once
#include "sys.h"
#include <bluetoothapis.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.devices.radios.h>
#include "ui.h"
#include "audio.h"

struct BtApi {
    typedef HBLUETOOTH_RADIO_FIND(WINAPI* FirstRadio_t)(const BLUETOOTH_FIND_RADIO_PARAMS*, HANDLE*);
    typedef BOOL(WINAPI* RadioClose_t)(HBLUETOOTH_RADIO_FIND);
    typedef HBLUETOOTH_DEVICE_FIND(WINAPI* First_t)(const BLUETOOTH_DEVICE_SEARCH_PARAMS*, BLUETOOTH_DEVICE_INFO*);
    typedef BOOL(WINAPI* Next_t)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO*);
    typedef BOOL(WINAPI* Close_t)(HBLUETOOTH_DEVICE_FIND);
    typedef DWORD(WINAPI* Remove_t)(const BLUETOOTH_ADDRESS*);
    typedef DWORD(WINAPI* Auth_t)(HWND, HANDLE, BLUETOOTH_DEVICE_INFO*, PBLUETOOTH_OOB_DATA_INFO, AUTHENTICATION_REQUIREMENTS);
    static inline FirstRadio_t firstRadio;
    static inline RadioClose_t radioClose;
    static inline First_t first;
    static inline Next_t next;
    static inline Close_t close;
    static inline Remove_t remove;
    static inline Auth_t auth;
    static inline int state;  // 0 untried, 1 ok, -1 unavailable

    static bool ready() {
        if (state) return state > 0;
        state = -1;
        HMODULE m = LoadLibraryW(L"bthprops.cpl");
        if (!m) return false;
        firstRadio = (FirstRadio_t)GetProcAddress(m, "BluetoothFindFirstRadio");
        radioClose = (RadioClose_t)GetProcAddress(m, "BluetoothFindRadioClose");
        first = (First_t)GetProcAddress(m, "BluetoothFindFirstDevice");
        next = (Next_t)GetProcAddress(m, "BluetoothFindNextDevice");
        close = (Close_t)GetProcAddress(m, "BluetoothFindDeviceClose");
        remove = (Remove_t)GetProcAddress(m, "BluetoothRemoveDevice");
        auth = (Auth_t)GetProcAddress(m, "BluetoothAuthenticateDeviceEx");
        if (firstRadio && radioClose && first && next && close && remove && auth) state = 1;
        return state > 0;
    }
    static bool hasRadio() {
        if (!ready()) return false;
        BLUETOOTH_FIND_RADIO_PARAMS p{sizeof(p)};
        HANDLE r = nullptr;
        HBLUETOOTH_RADIO_FIND f = firstRadio(&p, &r);
        if (!f) return false;
        CloseHandle(r);
        radioClose(f);
        return true;
    }
};

// Windows.Devices.Radios through its ABI interfaces. Every call blocks, so callers run on a worker.
struct BtRadio {
    typedef HRESULT(WINAPI* RoInit_t)(RO_INIT_TYPE);
    typedef void(WINAPI* RoUninit_t)();
    typedef HRESULT(WINAPI* Factory_t)(HSTRING, REFIID, void**);
    typedef HRESULT(WINAPI* StrRef_t)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);

    template <class Op> static bool wait(Op* op) {
        IAsyncInfo* ai = nullptr;
        if (FAILED(op->QueryInterface(__uuidof(IAsyncInfo), (void**)&ai))) return false;
        AsyncStatus st = AsyncStatus::Started;
        for (int i = 0; i < 400 && SUCCEEDED(ai->get_Status(&st)) && st == AsyncStatus::Started; ++i) Sleep(10);
        ai->Release();
        return st == AsyncStatus::Completed;
    }

    // Reads (set < 0) or sets the Bluetooth radio. Returns -1 unknown/none, 0 off, 1 on.
    static int run(int set) {
        using namespace ABI::Windows::Devices::Radios;
        HMODULE cb = LoadLibraryW(L"combase.dll");
        auto roInit = cb ? (RoInit_t)GetProcAddress(cb, "RoInitialize") : nullptr;
        auto roUninit = cb ? (RoUninit_t)GetProcAddress(cb, "RoUninitialize") : nullptr;
        auto factory = cb ? (Factory_t)GetProcAddress(cb, "RoGetActivationFactory") : nullptr;
        auto strRef = cb ? (StrRef_t)GetProcAddress(cb, "WindowsCreateStringReference") : nullptr;
        if (!roInit || !roUninit || !factory || !strRef) return -1;
        const HRESULT init = roInit(RO_INIT_MULTITHREADED);
        int result = -1;
        static const wchar_t kClass[] = L"Windows.Devices.Radios.Radio";
        HSTRING_HEADER hh;
        HSTRING cls = nullptr;
        IRadioStatics* st = nullptr;
        if (SUCCEEDED(strRef(kClass, (UINT32)(sizeof(kClass) / 2 - 1), &hh, &cls)) &&
            SUCCEEDED(factory(cls, __uuidof(IRadioStatics), (void**)&st))) {
            __FIAsyncOperation_1___FIVectorView_1_Windows__CDevices__CRadios__CRadio* op = nullptr;
            if (SUCCEEDED(st->GetRadiosAsync(&op)) && wait(op)) {
                __FIVectorView_1_Windows__CDevices__CRadios__CRadio* list = nullptr;
                if (SUCCEEDED(op->GetResults(&list)) && list) {
                    unsigned n = 0;
                    list->get_Size(&n);
                    for (unsigned i = 0; i < n && result < 0; ++i) {
                        IRadio* r = nullptr;
                        if (FAILED(list->GetAt(i, &r)) || !r) continue;
                        RadioKind k;
                        if (SUCCEEDED(r->get_Kind(&k)) && k == RadioKind_Bluetooth) {
                            if (set >= 0) {
                                __FIAsyncOperation_1_Windows__CDevices__CRadios__CRadioAccessStatus* so = nullptr;
                                if (SUCCEEDED(r->SetStateAsync(set ? RadioState_On : RadioState_Off, &so)) && so) {
                                    wait(so);
                                    so->Release();
                                }
                            }
                            RadioState s;
                            if (SUCCEEDED(r->get_State(&s))) result = s == RadioState_On ? 1 : 0;
                        }
                        r->Release();
                    }
                    list->Release();
                }
            }
            if (op) op->Release();
            st->Release();
        }
        if (SUCCEEDED(init)) roUninit();
        return result;
    }
};

struct Bt {
    struct Dev { BLUETOOTH_DEVICE_INFO info; };
    static constexpr int kDevs = 16;

    static inline Dev paired[kDevs], avail[kDevs], scanBuf[kDevs];
    static inline int nPaired, nAvail;
    static inline volatile LONG nScan, scanVer, shownScanVer;
    static inline volatile LONG radio = -1, radioVer, shownRadioVer, gen, busy;
    static inline CRITICAL_SECTION cs;
    static inline bool csReady, hasRadio;
    static inline DWORD lastList;
    static inline wchar_t status[64];  // last action's note ("Connecting…"), cleared on the next refresh
    static inline DWORD statusAt;

    static void lock() { if (!csReady) { InitializeCriticalSection(&cs); csReady = true; } EnterCriticalSection(&cs); }
    static void unlock() { LeaveCriticalSection(&cs); }

    static void listPaired() {
        nPaired = 0;
        if (!BtApi::ready()) return;
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof(sp)};
        sp.fReturnAuthenticated = TRUE;
        sp.fReturnRemembered = TRUE;
        sp.fReturnConnected = TRUE;
        BLUETOOTH_DEVICE_INFO di{sizeof(di)};
        HBLUETOOTH_DEVICE_FIND f = BtApi::first(&sp, &di);
        if (!f) return;
        do {
            if ((di.fAuthenticated || di.fRemembered) && nPaired < kDevs) paired[nPaired++].info = di;
            di.dwSize = sizeof(di);
        } while (BtApi::next(f, &di));
        BtApi::close(f);
        for (int i = 1; i < nPaired; ++i) {  // connected first, then by name
            Dev t = paired[i];
            int j = i;
            auto before = [](const Dev& a, const Dev& b) {
                if (a.info.fConnected != b.info.fConnected) return a.info.fConnected != 0;
                return CompareStringOrdinal(a.info.szName, -1, b.info.szName, -1, TRUE) == CSTR_LESS_THAN;
            };
            for (; j > 0 && before(t, paired[j - 1]); --j) paired[j] = paired[j - 1];
            paired[j] = t;
        }
    }

    // ---- workers ----
    static DWORD WINAPI scanner(LPVOID arg) {
        const LONG g = (LONG)(INT_PTR)arg;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        while (gen == g) {
            if (radio == 0 || !BtApi::ready()) { Sleep(500); continue; }
            BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof(sp)};
            sp.fReturnUnknown = TRUE;
            sp.fIssueInquiry = TRUE;
            sp.cTimeoutMultiplier = 4;  // 4 x 1.28 s
            Dev found[kDevs];
            int n = 0;
            BLUETOOTH_DEVICE_INFO di{sizeof(di)};
            HBLUETOOTH_DEVICE_FIND f = BtApi::first(&sp, &di);
            if (f) {
                do {
                    if (!di.fAuthenticated && !di.fRemembered && n < kDevs) found[n++].info = di;
                    di.dwSize = sizeof(di);
                } while (gen == g && BtApi::next(f, &di));
                BtApi::close(f);
            } else Sleep(1000);
            if (gen != g) break;
            lock();
            for (int i = 0; i < n; ++i) scanBuf[i] = found[i];
            nScan = n;
            unlock();
            InterlockedIncrement(&scanVer);
        }
        return 0;
    }
    static DWORD WINAPI radioWorker(LPVOID arg) {
        const int set = (int)(INT_PTR)arg;
        const int r = BtRadio::run(set);
        InterlockedExchange(&radio, r);
        InterlockedIncrement(&radioVer);
        InterlockedExchange(&busy, 0);
        return 0;
    }
    static void radioAsync(int set) {
        if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;
        HANDLE t = CreateThread(nullptr, 0, radioWorker, (LPVOID)(INT_PTR)set, 0, nullptr);
        if (t) CloseHandle(t); else busy = 0;
    }
    static DWORD WINAPI pairWorker(LPVOID arg) {
        BLUETOOTH_DEVICE_INFO di = *(BLUETOOTH_DEVICE_INFO*)arg;
        HeapFree(GetProcessHeap(), 0, arg);
        BtApi::auth(nullptr, nullptr, &di, nullptr, MITMProtectionNotRequiredGeneralBonding);  // Windows shows its own prompt if one is needed
        return 0;
    }

    // ---- audio connect/disconnect through the BT audio driver ----
    static bool containsI(const wchar_t* s, const wchar_t* sub) {
        const int n = sys::wlen(sub);
        for (; *s; ++s) {
            int i = 0;
            for (; i < n && s[i]; ++i) {
                wchar_t a = s[i], b = sub[i];
                if (a >= L'a' && a <= L'z') a -= 32;
                if (b >= L'a' && b <= L'z') b -= 32;
                if (a != b) break;
            }
            if (i == n) return true;
        }
        return false;
    }
    static void macText(const BLUETOOTH_ADDRESS& a, wchar_t* o) {
        static const wchar_t hx[] = L"0123456789ABCDEF";
        for (int i = 0; i < 6; ++i) {
            const u8 b = a.rgBytes[5 - i];
            o[i * 2] = hx[b >> 4];
            o[i * 2 + 1] = hx[b & 15];
        }
        o[12] = 0;
    }
    // Sends the one-shot reconnect (or disconnect) to every audio endpoint whose KS filter belongs
    // to this device. Returns false when the device has no audio endpoint.
    static bool audioCmd(const BLUETOOTH_ADDRESS& addr, bool connect) {
        if (!Audio::ready()) return false;
        static const GUID kBtAudio = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
        wchar_t mac[13];
        macText(addr, mac);
        bool found = false;
        for (int flow = 0; flow < 2; ++flow) {
            IMMDeviceCollection* c = nullptr;
            if (FAILED(Audio::en->EnumAudioEndpoints(flow ? eCapture : eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &c))) continue;
            UINT n = 0;
            c->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* ep = nullptr;
                if (FAILED(c->Item(i, &ep))) continue;
                IDeviceTopology* topo = nullptr;
                IConnector* con = nullptr;
                IConnector* other = nullptr;
                IPart* part = nullptr;
                IDeviceTopology* ft = nullptr;
                LPWSTR fid = nullptr;
                if (SUCCEEDED(ep->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, (void**)&topo)) &&
                    SUCCEEDED(topo->GetConnector(0, &con)) && SUCCEEDED(con->GetConnectedTo(&other)) &&
                    SUCCEEDED(other->QueryInterface(__uuidof(IPart), (void**)&part)) &&
                    SUCCEEDED(part->GetTopologyObject(&ft)) && SUCCEEDED(ft->GetDeviceId(&fid)) && containsI(fid, mac)) {
                    IMMDevice* filter = nullptr;
                    IKsControl* ks = nullptr;
                    if (SUCCEEDED(Audio::en->GetDevice(fid, &filter)) &&
                        SUCCEEDED(filter->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr, (void**)&ks))) {
                        KSPROPERTY p{};
                        p.Set = kBtAudio;
                        p.Id = connect ? 0 : 1;  // KSPROPERTY_ONESHOT_RECONNECT / _DISCONNECT
                        p.Flags = KSPROPERTY_TYPE_GET;
                        ULONG ret = 0;
                        ks->KsProperty(&p, sizeof(p), nullptr, 0, &ret);
                        found = true;
                        ks->Release();
                    }
                    if (filter) filter->Release();
                }
                if (fid) CoTaskMemFree(fid);
                if (ft) ft->Release();
                if (part) part->Release();
                if (other) other->Release();
                if (con) con->Release();
                if (topo) topo->Release();
                ep->Release();
            }
            c->Release();
        }
        return found;
    }

    static const wchar_t* devGlyph(const BLUETOOTH_DEVICE_INFO& d) {
        const ULONG major = (d.ulClassofDevice >> 8) & 0x1F, minor = (d.ulClassofDevice >> 2) & 0x3F;
        switch (major) {
            case 1: return L"\xE7F8";  // computer
            case 2: return L"\xE8EA";  // phone
            case 4: return L"\xE7F6";  // audio / video
            case 5: return (minor & 0x10) ? L"\xE765" : (minor & 0x20) ? L"\xE962" : L"\xE7FC";  // keyboard / mouse / gamepad
            default: return L"\xE702";
        }
    }
    static void note(const wchar_t* fmt, const wchar_t* name) {
        wsprintfW(status, fmt, name);
        statusAt = GetTickCount();
    }

    static void toggleConnect(const BLUETOOTH_DEVICE_INFO& d) {
        if (audioCmd(d.Address, !d.fConnected)) note(d.fConnected ? L"Disconnecting %s\x2026" : L"Connecting %s\x2026", d.szName);
        else ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
    }
    static void forget(const BLUETOOTH_DEVICE_INFO& d) {
        wchar_t q[320];
        wsprintfW(q, L"Forget %s? You will need to pair it again to use it.", d.szName);
        if (MessageBoxW(nullptr, q, L"archwindows", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES)
            BtApi::remove(&d.Address);
        listPaired();
    }
    static void pair(const BLUETOOTH_DEVICE_INFO& d) {
        auto* copy = (BLUETOOTH_DEVICE_INFO*)HeapAlloc(GetProcessHeap(), 0, sizeof(d));
        if (!copy) return;
        *copy = d;
        HANDLE t = CreateThread(nullptr, 0, pairWorker, copy, 0, nullptr);
        if (t) { CloseHandle(t); note(L"Pairing with %s\x2026", d.szName); } else HeapFree(GetProcessHeap(), 0, copy);
    }

    // ---- bar ----
    static const wchar_t* barGlyph() { return L"\xE702"; }
    static bool visible() {
        static int seen = -1;
        if (seen < 0) seen = BtApi::hasRadio() ? 1 : 0;
        return seen != 0;
    }

    // ---- panel ----
    static void open(HMONITOR) {
        hasRadio = BtApi::hasRadio();
        status[0] = 0;
        nAvail = 0;
        listPaired();
        lastList = GetTickCount();
        radioAsync(-1);
        HANDLE t = CreateThread(nullptr, 0, scanner, (LPVOID)(INT_PTR)InterlockedIncrement(&gen), 0, nullptr);
        if (t) CloseHandle(t);
    }
    static void close() { InterlockedIncrement(&gen); }
    static bool tick() {
        if (GetTickCount() - lastList > 2000) { listPaired(); lastList = GetTickCount(); }
        if (scanVer != shownScanVer) {
            shownScanVer = scanVer;
            lock();
            nAvail = 0;
            for (int i = 0; i < nScan; ++i) {  // a device paired meanwhile now lives in the paired list
                bool known = false;
                for (int k = 0; k < nPaired; ++k) known |= paired[k].info.Address.ullLong == scanBuf[i].info.Address.ullLong;
                if (!known) avail[nAvail++] = scanBuf[i];
            }
            unlock();
        }
        if (status[0] && GetTickCount() - statusAt > 6000) status[0] = 0;
        return true;
    }

    static void ui(Ui& u) {
        static const wchar_t* const phrases[] = {L"Untangling wires", L"Streaming vikings", L"Pairing mysteries",
                                                 L"Herding headsets", L"Taming radios", L"Summoning speakers",
                                                 L"Wrangling codecs", L"Polishing packets"};
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t meta[40];
        sys::wcopy(meta, !hasRadio ? L"No adapter" : radio == 0 ? L"Turned off" : phrases[t.wMinute % 8]);
        fmt::upper(meta);
        static const wchar_t* const btn[] = {L"\xE710"};  // add device (Settings)
        const int h = u.hero(L"\xE702", F_HERO, u.p.fg(), L"Bluetooth", meta, hasRadio && radio >= 0 ? (int)radio : -1,
                             btn, hasRadio ? 1 : 0);
        if (h == 1) radioAsync(radio ? 0 : 1);
        else if (h == 2) { ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL); u.wantClose = true; }
        if (!hasRadio) {
            u.gap(10);
            u.notice(L"No Bluetooth adapter was found on this computer.", u.p.accent());
            return;
        }
        if (radio == 0) {
            u.sep();
            u.row(nullptr, L"Turn Bluetooth on to scan");
            return;
        }
        if (status[0]) { u.gap(10); u.notice(status, u.p.accent()); }
        int conn = 0;
        for (int i = 0; i < nPaired; ++i) conn += paired[i].info.fConnected != 0;
        if (conn) {
            u.sep();
            u.section(L"CONNECTED");
            for (int i = 0; i < nPaired; ++i) {
                const BLUETOOTH_DEVICE_INFO& d = paired[i].info;
                if (!d.fConnected) continue;
                const int c = u.row(devGlyph(d), d.szName, L"Connected \x00B7 click to disconnect", true, L"\xE711");
                if (c == 1) toggleConnect(d);
                else if (c == 2) forget(d);
            }
        }
        if (nPaired > conn) {
            u.sep();
            u.section(L"PAIRED");
            for (int i = 0; i < nPaired; ++i) {
                const BLUETOOTH_DEVICE_INFO& d = paired[i].info;
                if (d.fConnected) continue;
                const int c = u.row(devGlyph(d), d.szName, nullptr, false, L"\xE711");
                if (c == 1) toggleConnect(d);
                else if (c == 2) forget(d);
            }
        }
        u.sep();
        u.section(L"AVAILABLE", nAvail ? nullptr : L"SCANNING");
        if (!nAvail) u.row(nullptr, L"Scanning for devices\x2026");
        for (int i = 0; i < nAvail; ++i) {
            const BLUETOOTH_DEVICE_INFO& d = avail[i].info;
            wchar_t mac[13];
            if (!d.szName[0]) macText(d.Address, mac);
            if (u.row(devGlyph(d), d.szName[0] ? d.szName : mac, L"Click to pair")) pair(d);
        }
    }
};
