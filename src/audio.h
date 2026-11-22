// Audio panel (Omarchy's omarchy.audio): output and input volume, device pickers, mute switch.
//
// Core Audio (MMDevice + IAudioEndpointVolume) on the UI thread. The default device is changed
// through IPolicyConfig, the interface Windows' own sound settings use (undocumented, but stable
// since Windows 7). A volume callback repaints the bar when the volume changes elsewhere.
#pragma once
#include "sys.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include "ui.h"

// IPolicyConfig (Windows 7+): only SetDefaultEndpoint is called; the rest are vtable fillers.
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") DECLSPEC_NOVTABLE IPolicyConfigAw : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE f0() = 0;  // GetMixFormat
    virtual HRESULT STDMETHODCALLTYPE f1() = 0;  // GetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE f2() = 0;  // ResetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE f3() = 0;  // SetDeviceFormat
    virtual HRESULT STDMETHODCALLTYPE f4() = 0;  // GetProcessingPeriod
    virtual HRESULT STDMETHODCALLTYPE f5() = 0;  // SetProcessingPeriod
    virtual HRESULT STDMETHODCALLTYPE f6() = 0;  // GetShareMode
    virtual HRESULT STDMETHODCALLTYPE f7() = 0;  // SetShareMode
    virtual HRESULT STDMETHODCALLTYPE f8() = 0;  // GetPropertyValue
    virtual HRESULT STDMETHODCALLTYPE f9() = 0;  // SetPropertyValue
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR id, ERole role) = 0;
};

struct Audio {
    struct Dev { wchar_t id[128]; wchar_t name[64]; };
    static constexpr int kDevs = 8;
    static constexpr UINT WM_AUDIO = WM_APP + 41;  // posted to `notify` when the volume changes

    static inline IMMDeviceEnumerator* en;
    static inline IAudioEndpointVolume* vol[2];  // [0] output, [1] input
    static inline Dev devs[2][kDevs];
    static inline int nDev[2], def[2] = {-1, -1};
    static inline HWND notify;
    static inline bool comTried;

    struct Callback : IAudioEndpointVolumeCallback {
        STDMETHODIMP QueryInterface(REFIID r, void** p) override {
            if (r == __uuidof(IUnknown) || r == __uuidof(IAudioEndpointVolumeCallback)) { *p = this; return S_OK; }
            *p = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static object: no reference counting
        STDMETHODIMP_(ULONG) Release() override { return 1; }
        STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
            if (notify) PostMessageW(notify, WM_AUDIO, 0, 0);
            return S_OK;
        }
    };
    static Callback& cb() { static Callback c; return c; }

    static bool ready() {
        if (en) return true;
        if (comTried) return false;
        comTried = true;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the UI thread keeps COM for the process lifetime
        return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)));
    }

    static void friendly(IMMDevice* d, wchar_t* o, int cap) {
        static const PROPERTYKEY kName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        o[0] = 0;
        IPropertyStore* ps = nullptr;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kName, &v)) && v.vt == VT_LPWSTR) lstrcpynW(o, v.pwszVal, cap);
        PropVariantClear(&v);
        ps->Release();
    }

    // Re-read devices and defaults; re-bind the volume interfaces to the current defaults.
    static void refresh() {
        if (!ready()) return;
        for (int k = 0; k < 2; ++k) {
            const EDataFlow flow = k ? eCapture : eRender;
            nDev[k] = 0;
            def[k] = -1;
            wchar_t defId[128] = {};
            IMMDevice* d = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id))) { lstrcpynW(defId, id, 128); CoTaskMemFree(id); }
                if (vol[k]) { if (!k) vol[k]->UnregisterControlChangeNotify(&cb()); vol[k]->Release(); vol[k] = nullptr; }
                if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol[k])) && !k)
                    vol[k]->RegisterControlChangeNotify(&cb());
                d->Release();
            }
            IMMDeviceCollection* c = nullptr;
            if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c))) continue;
            UINT n = 0;
            c->GetCount(&n);
            for (UINT i = 0; i < n && nDev[k] < kDevs; ++i) {
                IMMDevice* e = nullptr;
                if (FAILED(c->Item(i, &e))) continue;
                Dev& o = devs[k][nDev[k]];
                LPWSTR id = nullptr;
                if (SUCCEEDED(e->GetId(&id))) {
                    lstrcpynW(o.id, id, 128);
                    CoTaskMemFree(id);
                    friendly(e, o.name, 64);
                    if (sys::weq(o.id, defId)) def[k] = nDev[k];
                    ++nDev[k];
                }
                e->Release();
            }
            c->Release();
        }
    }

    static int level(int k) {  // 0..100, -1 when there is no device
        if (!vol[k]) refresh();
        float f = 0;
        return vol[k] && SUCCEEDED(vol[k]->GetMasterVolumeLevelScalar(&f)) ? (int)(f * 100 + 0.5f) : -1;
    }
    static bool muted(int k) {
        BOOL m = FALSE;
        return vol[k] && SUCCEEDED(vol[k]->GetMute(&m)) && m;
    }
    static void setLevel(int k, int pct) {
        if (!vol[k]) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        vol[k]->SetMasterVolumeLevelScalar(pct / 100.0f, nullptr);
        if (pct > 0 && muted(k)) vol[k]->SetMute(FALSE, nullptr);
    }
    static void setMute(int k, bool m) { if (vol[k]) vol[k]->SetMute(m, nullptr); }
    static void setDefault(int k, int i) {
        IPolicyConfigAw* pc = nullptr;
        static const CLSID kPolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
        if (FAILED(CoCreateInstance(kPolicyConfig, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfigAw), (void**)&pc))) return;
        pc->SetDefaultEndpoint(devs[k][i].id, eConsole);
        pc->SetDefaultEndpoint(devs[k][i].id, eMultimedia);
        pc->SetDefaultEndpoint(devs[k][i].id, eCommunications);
        pc->Release();
        refresh();
    }

    // ---- bar ----
    static const wchar_t* barGlyph() {
        const int l = level(0);
        if (l < 0 || muted(0)) return L"\xE74F";
        return l == 0 ? L"\xE992" : l < 34 ? L"\xE993" : l < 67 ? L"\xE994" : L"\xE995";
    }
    static void scroll(int dir) { const int l = level(0); if (l >= 0) setLevel(0, l - dir * 5); }
    static void toggleMute() { if (level(0) >= 0) setMute(0, !muted(0)); }

    // ---- panel ----
    static void open(HMONITOR) { refresh(); }
    static void close() {}
    static bool tick() { return true; }  // levels can change from the keyboard or other apps

    static void ui(Ui& u) {
        const int out = level(0), in = level(1);
        const bool mOut = muted(0);
        const wchar_t* phrase = out < 0 ? L"No output" : mOut ? L"Muted" : out == 0 ? L"Silenced" : out >= 100 ? L"Concert hall"
                                : out >= 85 ? L"Party mode" : out >= 70 ? L"Cranked up" : out >= 50 ? L"Steady groove"
                                : out >= 30 ? L"Easy listening" : out >= 15 ? L"Murmur" : L"Whisper";
        wchar_t meta[32];
        sys::wcopy(meta, phrase);
        fmt::upper(meta);
        if (u.hero(barGlyph(), F_HERO, u.p.fg(), L"Audio", meta, out < 0 ? -1 : (mOut ? 0 : 1)) == 1) setMute(0, !mOut);
        wchar_t pc[8];
        for (int k = 0; k < 2; ++k) {
            const int l = k ? in : out;
            u.sep();
            if (l >= 0) wsprintfW(pc, L"%d%%", (k ? muted(1) : mOut) ? 0 : l); else sys::wcopy(pc, L"");
            u.section(k ? L"INPUT" : L"OUTPUT", pc);
            if (l >= 0) {
                const int v = u.slider((k ? muted(1) : mOut) ? 0 : l, 100);
                if (v >= 0) setLevel(k, v);
                u.gap(8);
            }
            if (!nDev[k]) { u.row(nullptr, k ? L"No input devices" : L"No output devices"); continue; }
            for (int i = 0; i < nDev[k]; ++i)
                if (u.row(k ? L"\xE1D6" : L"\xE767", devs[k][i].name, nullptr, i == def[k]) && i != def[k])
                    setDefault(k, i);
        }
    }
};
