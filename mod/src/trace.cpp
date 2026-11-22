// Render-target hooks on the game's immediate context: the HUD capture (hud.cpp
// HudTargets) and the GPU profile's pass names. (review2: the Numpad 8 one-frame
// trace that this file was written for was never switched on any more; removed.)
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#include "fc3vr.h"

typedef void(STDMETHODCALLTYPE *OMSetRTFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                           ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *OMSetRTUAVFn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                              ID3D11DepthStencilView *, UINT, UINT,
                                              ID3D11UnorderedAccessView *const *, const UINT *);

static void **g_vt;
static ID3D11DeviceContext *g_ctx;
static OMSetRTFn o_setRT;
static OMSetRTUAVFn o_setRTUAV;

static char g_lastTargets[40];
// gpu3: name a pass by what it draws into (first target's size and format, or the depth buffer's).
static void GpuMarkTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                           ID3D11DepthStencilView *dsv) {
    if (c != TraceContext() || !StereoGpuProfiling()) return;
    ID3D11View *v = n && rtv && rtv[0] ? (ID3D11View *)rtv[0] : (ID3D11View *)dsv;
    char name[56] = "targets: none";
    if (v) {
        ID3D11Resource *r = nullptr;
        v->GetResource(&r);
        ID3D11Texture2D *t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            snprintf(name, sizeof name, "%s %ux%u f%u x%u%s", v == (ID3D11View *)dsv ? "depth" : "rt", d.Width, d.Height,
                     d.Format, n, n && dsv ? "+d" : "");
            t->Release();
        }
        if (r) r->Release();
    }
    lstrcpynA(g_lastTargets, name, sizeof g_lastTargets);
    StereoGpuMark(name);
}
const char *TraceLastTargets() { return g_lastTargets; }

static void STDMETHODCALLTYPE HSetRT(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                     ID3D11DepthStencilView *dsv) {
    GpuMarkTargets(c, n, rtv, dsv);
    ID3D11RenderTargetView *tmp[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView *const *asked = rtv;
    if (c == g_ctx) rtv = HudTargets(n, rtv, dsv, tmp);  // hud1: the HUD goes to its own texture
    if (c == g_ctx && dsv) LaserNoteGameDepth(dsv);  // occl2
    o_setRT(c, n, rtv, dsv);
    if (c == g_ctx) VrsOnTargets(c, n, rtv, rtv != asked);  // vrs1
}
static void STDMETHODCALLTYPE HSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                        ID3D11DepthStencilView *dsv, UINT us, UINT un,
                                        ID3D11UnorderedAccessView *const *uav, const UINT *counts) {
    ID3D11RenderTargetView *tmp[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView *const *asked = rtv;
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        GpuMarkTargets(c, n, rtv, dsv);
        if (c == g_ctx) rtv = HudTargets(n, rtv, dsv, tmp);
        if (c == g_ctx && dsv) LaserNoteGameDepth(dsv);  // occl2
    }
    o_setRTUAV(c, n, rtv, dsv, us, un, uav, counts);
    if (c == g_ctx && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) VrsOnTargets(c, n, rtv, rtv != asked);  // vrs1
}

ID3D11DeviceContext *TraceContext() { return g_ctx; }


void TraceHookContext(ID3D11DeviceContext *ctx) {
    void **vt = *(void ***)ctx;
    if (g_vt) return;
    g_ctx = ctx;  // the game's immediate context lives as long as the game
    g_vt = vt;
    bool ok = HookSlot(vt, 33, (void *)HSetRT, (void **)&o_setRT, "OMSetRenderTargets") &&
              HookSlot(vt, 34, (void *)HSetRTUAV, (void **)&o_setRTUAV, "OMSetRenderTargetsAndUAVs");
    Log("trace: render-target hooks %s", ok ? "installed" : "FAILED");
}
