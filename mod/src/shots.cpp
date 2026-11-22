// Debug screenshots: F10 saves the next 4 frames as PNGs in bin\fc3vr_shots\,
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <stdio.h>

#include "fc3vr.h"

static const int kBurst = 4;
static int g_left;          // frames still to save in this burst
static int g_burst;         // burst number, for file names

static ID3D11Texture2D *g_staging;
static D3D11_TEXTURE2D_DESC g_stagingDesc;

static void ShotsDir(wchar_t *out) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ShotsDir, &self);
    GetModuleFileNameW(self, out, MAX_PATH);
    wchar_t *slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, L"fc3vr_shots");
}

static bool SavePng(const wchar_t *path, const BYTE *bgra, UINT w, UINT h, UINT pitch) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // may already be initialised; either is fine
    IWICImagingFactory *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                   (void **)&factory)) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(frame->WritePixels(h, pitch, pitch * h, (BYTE *)bgra)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

static void SaveBackBuffer(IDXGISwapChain *sc, int index) {
    ID3D11Texture2D *bb = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void **)&bb))) return;
    bb->GetDevice(&dev);
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
                                    d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
        Log("shots: can't save back buffer (format %d, %u samples)", d.Format, d.SampleDesc.Count);
    } else {
        if (g_staging && (g_stagingDesc.Width != d.Width || g_stagingDesc.Height != d.Height ||
                          g_stagingDesc.Format != d.Format)) {
            g_staging->Release();
            g_staging = nullptr;
        }
        if (!g_staging) {
            g_stagingDesc = d;
            g_stagingDesc.BindFlags = 0;
            g_stagingDesc.MiscFlags = 0;
            g_stagingDesc.Usage = D3D11_USAGE_STAGING;
            g_stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateTexture2D(&g_stagingDesc, nullptr, &g_staging);
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (g_staging) {
            ctx->CopyResource(g_staging, bb);
            if (SUCCEEDED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) {
                UINT pitch = d.Width * 4;
                BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, pitch * d.Height);
                bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                for (UINT y = 0; y < d.Height; ++y) {
                    const BYTE *src = (const BYTE *)m.pData + y * m.RowPitch;
                    BYTE *dst = px + y * pitch;
                    for (UINT x = 0; x < d.Width; ++x, src += 4, dst += 4) {
                        dst[0] = rgba ? src[2] : src[0];
                        dst[1] = src[1];
                        dst[2] = rgba ? src[0] : src[2];
                        dst[3] = 255;
                    }
                }
                ctx->Unmap(g_staging, 0);
                wchar_t path[MAX_PATH], file[64];
                ShotsDir(path);
                CreateDirectoryW(path, nullptr);
                swprintf(file, 64, L"\\shot%03d_%d.png", g_burst, index);
                wcscat(path, file);
                bool ok = SavePng(path, px, d.Width, d.Height, pitch);
                HeapFree(GetProcessHeap(), 0, px);
                Log("shots: %s shot%03d_%d.png (%ux%u)", ok ? "saved" : "FAILED to save", g_burst, index, d.Width,
                    d.Height);
            }
        }
    }
    ctx->Release();
    dev->Release();
    bb->Release();
}

void ShotsOnPresent(IDXGISwapChain *sc) {
    bool pressed = KeyHit(VK_F10);

    if (g_left > 0) {
        SaveBackBuffer(sc, kBurst - g_left);
        --g_left;
    } else if (pressed) {
        ++g_burst;
        g_left = kBurst;
        Log("shots: F10 burst %d", g_burst);
    }
}
