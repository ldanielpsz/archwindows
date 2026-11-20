// Image policy backed by Windows Imaging Component (part of the OS, so no
// bundled decoder). ole32/WIC are loaded only while the picker is open and
// unloaded again afterwards, so the idle bar never pays for them.
//
// An ImagePolicy provides:
//   static bool acquire();   static void release();
//   static bool decode(path, maxW, maxH, out, outW, outH)   // BGRX, aspect-fit
#pragma once
#include "sys.h"
#include <initguid.h>
#include <wincodec.h>

struct WicImage {
    typedef HRESULT(WINAPI* CoInit_t)(LPVOID, DWORD);
    typedef void(WINAPI* CoUninit_t)();
    typedef HRESULT(WINAPI* CoCreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    typedef void(WINAPI* CoFree_t)();

    static inline HMODULE ole;
    static inline CoUninit_t coUninit;
    static inline CoCreate_t coCreate;
    static inline CoFree_t coFree;
    static inline IWICImagingFactory* factory;

    static bool acquire() {
        if (factory) return true;
        ole = LoadLibraryW(L"ole32.dll");
        if (!ole) return false;
        auto coInit = (CoInit_t)GetProcAddress(ole, "CoInitializeEx");
        coUninit = (CoUninit_t)GetProcAddress(ole, "CoUninitialize");
        coCreate = (CoCreate_t)GetProcAddress(ole, "CoCreateInstance");
        coFree = (CoFree_t)GetProcAddress(ole, "CoFreeUnusedLibraries");
        if (!coInit || !coUninit || !coCreate) { release(); return false; }
        coInit(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(coCreate(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(IWICImagingFactory), (void**)&factory))) {
            factory = nullptr;
            coUninit();
            FreeLibrary(ole);
            ole = nullptr;
            return false;
        }
        return true;
    }

    static void release() {
        if (factory) { factory->Release(); factory = nullptr; }
        if (ole) {
            if (coFree) coFree();
            if (coUninit) coUninit();
            FreeLibrary(ole);
            ole = nullptr;
        }
    }

    template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

    static bool decode(const wchar_t* path, int maxW, int maxH, void* out, int& outW, int& outH) {
        outW = outH = 0;
        if (!factory) return false;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICBitmapScaler* sc = nullptr;
        IWICFormatConverter* cv = nullptr;
        bool ok = false;
        UINT sw = 0, sh = 0;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                          WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&sw, &sh)) && sw && sh) {
            // Aspect-fit into maxW x maxH (never upscale past the box).
            int rw, rh;
            if ((long long)sw * maxH > (long long)sh * maxW) { rw = maxW; rh = (int)((long long)sh * maxW / sw); }
            else { rh = maxH; rw = (int)((long long)sw * maxH / sh); }
            if (rw < 1) rw = 1;
            if (rh < 1) rh = 1;
            if (SUCCEEDED(factory->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(fr, rw, rh, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(factory->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(cv->CopyPixels(nullptr, rw * 4, rw * 4 * rh, (BYTE*)out))) {
                outW = rw;
                outH = rh;
                ok = true;
            }
        }
        rel(cv); rel(sc); rel(fr); rel(dec);
        return ok;
    }
};
