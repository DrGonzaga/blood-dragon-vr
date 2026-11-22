// SMAA anti-aliasing for the headset (smaa1, 1 Nov). With [render] SMAA=1..4
// (Low, Medium, High, Ultra) each eye's image gets SMAA 1x (Jimenez et al.,
// MIT licence, third_party/smaa) as it goes to the headset, in place of the
// plain copy: edge detection, blend weights, neighbourhood blending. SMAA=0
// (the default) is the plain copy as before. States we change are put back,
// as this also runs between the two eyes.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

#include "fc3vr.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

int g_smaa = 0;  // 0 off, 1 low, 2 medium, 3 high, 4 ultra

namespace {

const char kShader[] =
#include "smaa_hlsl.inc"
    ;

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);

struct Cb {
    float rtm[4];
    float opt[4];
};

struct Shaders {
    ID3D11VertexShader *vs[3];
    ID3D11PixelShader *ps[3];
    bool tried, ok;
};
Shaders g_sh[5];  // per preset

ID3D11Device *g_dev;
ID3D11Buffer *g_cb;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_ds;
ID3D11BlendState *g_bs;
ID3D11SamplerState *g_linear, *g_point;
ID3D11ShaderResourceView *g_areaSrv, *g_searchSrv;
bool g_failed;
CompileFn g_compile;

// Edges and blend weights, at the image size.
ID3D11Texture2D *g_edges, *g_blend;
ID3D11ShaderResourceView *g_edgesSrv, *g_blendSrv;
ID3D11RenderTargetView *g_edgesRtv, *g_blendRtv;
UINT g_w, g_h;

// The game's image: read directly when it allows it, else copied first.
ID3D11Texture2D *g_srcTex;  // not held
ID3D11ShaderResourceView *g_srcSrv;
ID3D11Texture2D *g_copy;
ID3D11ShaderResourceView *g_copySrv;

struct Dst {
    ID3D11Texture2D *tex;  // not held (the runtime owns it)
    ID3D11RenderTargetView *rtv;
    bool linear;
};
Dst g_dst[24];
int g_dstN;
LONG g_count;

template <class T> void Rel(T *&p) {
    if (p) p->Release(), p = nullptr;
}

bool Setup(ID3D11Device *dev) {
    if (g_failed) return false;
    if (g_dev) return true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    g_compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!g_compile) {
        Log("smaa: no d3dcompiler; SMAA off");
        return g_failed = true, false;
    }
    D3D11_BUFFER_DESC bd = {sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_bs);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &g_point);

    ID3D11Texture2D *t = nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.MipLevels = 1, td.ArraySize = 1, td.SampleDesc = {1, 0}, td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Width = AREATEX_WIDTH, td.Height = AREATEX_HEIGHT, td.Format = DXGI_FORMAT_R8G8_UNORM;
    D3D11_SUBRESOURCE_DATA init = {areaTexBytes, AREATEX_PITCH, 0};
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &t))) dev->CreateShaderResourceView(t, nullptr, &g_areaSrv), t->Release();
    td.Width = SEARCHTEX_WIDTH, td.Height = SEARCHTEX_HEIGHT, td.Format = DXGI_FORMAT_R8_UNORM;
    init = {searchTexBytes, SEARCHTEX_PITCH, 0};
    t = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &t))) dev->CreateShaderResourceView(t, nullptr, &g_searchSrv), t->Release();
    if (!(g_cb && g_rs && g_ds && g_bs && g_linear && g_point && g_areaSrv && g_searchSrv)) {
        Log("smaa: couldn't create its GPU objects; SMAA off");
        return g_failed = true, false;
    }
    dev->AddRef();
    g_dev = dev;
    return true;
}

bool Compile(int preset) {
    Shaders &s = g_sh[preset];
    if (s.tried) return s.ok;
    s.tried = true;
    static const char *names[5] = {"", "SMAA_PRESET_LOW", "SMAA_PRESET_MEDIUM", "SMAA_PRESET_HIGH", "SMAA_PRESET_ULTRA"};
    D3D_SHADER_MACRO defs[2] = {{names[preset], "1"}, {nullptr, nullptr}};
    static const char *vs[3] = {"vsEdge", "vsBlend", "vsNb"}, *ps[3] = {"psEdge", "psBlend", "psNb"};
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 3; ++i) {
        ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
        const UINT fl = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        bool ok = SUCCEEDED(g_compile(kShader, sizeof kShader - 1, "smaa", defs, nullptr, vs[i], "vs_5_0", fl, 0, &vb, &err)) &&
                  SUCCEEDED(g_compile(kShader, sizeof kShader - 1, "smaa", defs, nullptr, ps[i], "ps_5_0", fl, 0, &pb, &err));
        if (ok) {
            g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &s.vs[i]);
            g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &s.ps[i]);
        } else {
            Log("smaa: shader %s failed: %s", ps[i], err ? (const char *)err->GetBufferPointer() : "?");
        }
        Rel(vb), Rel(pb), Rel(err);
        if (!ok || !s.vs[i] || !s.ps[i]) return false;
    }
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    Log("smaa: %s ready (compiled in %.0f ms)", names[preset], double(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
    return s.ok = true;
}

DXGI_FORMAT Unorm(DXGI_FORMAT f, bool *srgb) {
    *srgb = false;
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: *srgb = true; return f;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *srgb = true; return f;
    default: return f;
    }
}

// A view that reads the game's image as stored (its pixels are sRGB-encoded already,
// which is what SMAA's luma edge detection wants).
ID3D11ShaderResourceView *SourceView(ID3D11DeviceContext *ctx, ID3D11Texture2D *src, const D3D11_TEXTURE2D_DESC &sd) {
    bool srgb;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    vd.Format = Unorm(sd.Format, &srgb);
    if (srgb) vd.Format = sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    if ((sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) && sd.SampleDesc.Count == 1) {
        if (src == g_srcTex && g_srcSrv) return g_srcSrv;
        Rel(g_srcSrv);
        g_srcTex = src;
        if (FAILED(g_dev->CreateShaderResourceView(src, &vd, &g_srcSrv)) &&
            FAILED(g_dev->CreateShaderResourceView(src, nullptr, &g_srcSrv)))
            g_srcSrv = nullptr, g_srcTex = nullptr;
        if (g_srcSrv) Log("smaa: reading the game's image directly (%ux%u format %d)", sd.Width, sd.Height, (int)sd.Format);
        if (g_srcSrv) return g_srcSrv;
    }
    D3D11_TEXTURE2D_DESC cd = {};
    if (g_copy) g_copy->GetDesc(&cd);
    if (!g_copy || cd.Width != sd.Width || cd.Height != sd.Height || cd.Format != sd.Format) {
        Rel(g_copySrv), Rel(g_copy);
        cd = sd;
        cd.MipLevels = 1, cd.ArraySize = 1, cd.SampleDesc = {1, 0}, cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE, cd.CPUAccessFlags = 0, cd.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_copy)) ||
            (FAILED(g_dev->CreateShaderResourceView(g_copy, &vd, &g_copySrv)) &&
             FAILED(g_dev->CreateShaderResourceView(g_copy, nullptr, &g_copySrv)))) {
            Rel(g_copy), g_copySrv = nullptr;
            return nullptr;
        }
        Log("smaa: copying the game's image first (it can't be read directly)");
    }
    if (sd.SampleDesc.Count > 1) ctx->ResolveSubresource(g_copy, 0, src, 0, sd.Format);
    else ctx->CopyResource(g_copy, src);
    return g_copySrv;
}

Dst *Target(ID3D11Texture2D *dst, const D3D11_TEXTURE2D_DESC &dd) {
    for (int i = 0; i < g_dstN; ++i)
        if (g_dst[i].tex == dst) return &g_dst[i];
    if (g_dstN == 24) {
        for (int i = 0; i < g_dstN; ++i) Rel(g_dst[i].rtv);
        g_dstN = 0;
    }
    Dst &d = g_dst[g_dstN];
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = Unorm(dd.Format, &d.linear);
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d.tex = dst, d.rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(dst, &rv, &d.rtv))) return nullptr;
    ++g_dstN;
    Log("smaa: headset image %ux%u format %d (%s)", dd.Width, dd.Height, (int)dd.Format,
        d.linear ? "sRGB target, written linear" : "written as is");
    return &d;
}

bool Work(UINT w, UINT h) {
    if (g_edges && g_w == w && g_h == h) return true;
    Rel(g_edgesSrv), Rel(g_edgesRtv), Rel(g_edges), Rel(g_blendSrv), Rel(g_blendRtv), Rel(g_blend);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1, td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_edges)) &&
              SUCCEEDED(g_dev->CreateShaderResourceView(g_edges, nullptr, &g_edgesSrv)) &&
              SUCCEEDED(g_dev->CreateRenderTargetView(g_edges, nullptr, &g_edgesRtv));
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_blend)) &&
         SUCCEEDED(g_dev->CreateShaderResourceView(g_blend, nullptr, &g_blendSrv)) &&
         SUCCEEDED(g_dev->CreateRenderTargetView(g_blend, nullptr, &g_blendRtv));
    if (!ok) {
        Rel(g_edgesSrv), Rel(g_edgesRtv), Rel(g_edges), Rel(g_blendSrv), Rel(g_blendRtv), Rel(g_blend);
        return false;
    }
    g_w = w, g_h = h;
    return true;
}

bool Run(ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const D3D11_TEXTURE2D_DESC &sd,
         const D3D11_TEXTURE2D_DESC &dd, int preset) {
    ID3D11ShaderResourceView *in = SourceView(ctx, src, sd);
    Dst *out = Target(dst, dd);
    if (!in || !out || !Work(dd.Width, dd.Height)) return false;
    Shaders &s = g_sh[preset];

    Cb cb = {};
    cb.rtm[0] = 1.0f / dd.Width, cb.rtm[1] = 1.0f / dd.Height, cb.rtm[2] = (float)dd.Width, cb.rtm[3] = (float)dd.Height;

    HeadtrackOwnDraw(true);
    D3DState game(ctx, 5, 2, true);  // state1: the game's pipeline state, put back by Restore() below

    ID3D11ShaderResourceView *none[5] = {};
    ctx->PSSetShaderResources(0, 5, none);
    ctx->RSSetState(g_rs);
    ctx->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    cb.opt[1] = out->linear ? 1.0f : 0.0f;
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ID3D11SamplerState *smp[2] = {g_linear, g_point};
    ctx->PSSetSamplers(0, 2, smp);
    D3D11_VIEWPORT vp = {0, 0, (float)dd.Width, (float)dd.Height, 0, 1};
    ctx->RSSetViewports(1, &vp);
    const float clear[4] = {0, 0, 0, 0};

    // 1 edges (from the game's image)
    ctx->OMSetRenderTargets(1, &g_edgesRtv, nullptr);
    ctx->ClearRenderTargetView(g_edgesRtv, clear);
    ctx->PSSetShaderResources(0, 1, &in);
    ctx->VSSetShader(s.vs[0], nullptr, 0);
    ctx->PSSetShader(s.ps[0], nullptr, 0);
    ctx->Draw(3, 0);
    // 2 blend weights (from the edges and SMAA's lookup images)
    ctx->OMSetRenderTargets(1, &g_blendRtv, nullptr);
    ctx->ClearRenderTargetView(g_blendRtv, clear);
    ID3D11ShaderResourceView *bw[3] = {g_edgesSrv, g_areaSrv, g_searchSrv};
    ctx->PSSetShaderResources(1, 3, bw);
    ctx->VSSetShader(s.vs[1], nullptr, 0);
    ctx->PSSetShader(s.ps[1], nullptr, 0);
    ctx->Draw(3, 0);
    // 3 blend the game's image into the headset image
    ctx->PSSetShaderResources(1, 1, none);
    ctx->OMSetRenderTargets(1, &out->rtv, nullptr);
    ctx->PSSetShaderResources(4, 1, &g_blendSrv);
    ctx->VSSetShader(s.vs[2], nullptr, 0);
    ctx->PSSetShader(s.ps[2], nullptr, 0);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 5, none);

    game.Restore();
    HeadtrackOwnDraw(false);
    if (++g_count == 1) Log("smaa: first image anti-aliased (%ux%u)", dd.Width, dd.Height);
    return true;
}

}  // namespace

// The game's image to a headset image: SMAA when chosen, else the plain copy
// (also whenever SMAA can't run).
void SmaaCopy(ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst) {
    int preset = g_smaa;
    if (preset >= 1 && preset <= 4 && !g_failed) {
        D3D11_TEXTURE2D_DESC sd, dd;
        src->GetDesc(&sd);
        dst->GetDesc(&dd);
        if (sd.Width == dd.Width && sd.Height == dd.Height) {
            ID3D11Device *dev = nullptr;
            ctx->GetDevice(&dev);
            bool ok = Setup(dev) && Compile(preset) && Run(ctx, src, dst, sd, dd, preset);
            dev->Release();
            if (ok) return;
            static bool told;
            if (!told) told = true, Log("smaa: couldn't run; plain copy instead");
        }
    }
    ctx->CopyResource(dst, src);
}

// The headset session or the game's image is going: let go of them.
void SmaaOnSwapChainGoing() {
    for (int i = 0; i < g_dstN; ++i) Rel(g_dst[i].rtv);
    g_dstN = 0;
    Rel(g_srcSrv);
    g_srcTex = nullptr;
}
