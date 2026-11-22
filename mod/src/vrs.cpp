// vrs1 (11 Nov, review N9, DrGonzaga's trial): variable rate shading.
//
// The middle of each eye's picture is shaded pixel by pixel as before; a ring
// further out is shaded once per 2 pixels, and the rest once per 2x2 pixels.
// Edges of objects stay at full sharpness (the picture is still drawn at full
// size); only the colour work inside surfaces gets coarser, where the lenses
// and the eye see least. The rings are measured in degrees from straight
// ahead, so they sit on the lens centre (which is off-centre in the picture
// since oc1).
//
// It is an NVIDIA driver feature (RTX cards), reached through nvapi.dll, which
// the driver installs. Only the game's own passes into full-size targets get
// it: never the back buffer (HUD, menus), never our overlays, never smaller
// targets (shadows, reflections, half-size effects), never the scope view.
// [render] Vrs=0 (default, the VR page checkbox) leaves every call out.
// pip1 (21 Nov): the scope picture (third render, scope.cpp) is only ever seen inside the lens, a circle round its
// middle; [aim] ScopeOutside 1 shades the rest once per 4x4 pixels, 2 not at all. Independent of [render] Vrs.
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <string.h>

#include "fc3vr.h"

namespace {

// ---- the few pieces of NVIDIA's nvapi.h (MIT licence) that are used ----
enum NvRate {
    kCull, kX16, kX8, kX4, kX2,
    kX1,      // per pixel
    k2x1,     // one shade per 2 pixels (2 wide)
    k1x2,     // one shade per 2 pixels (2 tall)
    k2x2,     // one shade per 4 pixels
    k4x2, k2x4, k4x4
};
struct NvViewportRate {
    bool enable;
    int table[16];
};
struct NvViewportsRate {
    UINT version, count;
    NvViewportRate *viewports;
};
struct NvViewDesc {
    UINT version;
    DXGI_FORMAT format;
    int dimension;  // 4 = 2D texture
    UINT mip, first, size;
};
#define NV_VER(T) (UINT)(sizeof(T) | (1 << 16))
typedef int(__cdecl *NvInitFn)();
typedef int(__cdecl *NvSetRatesFn)(IUnknown *ctx, NvViewportsRate *);
typedef int(__cdecl *NvCreateViewFn)(ID3D11Device *, ID3D11Resource *, NvViewDesc *, ID3D11View **);
typedef int(__cdecl *NvSetViewFn)(IUnknown *ctx, ID3D11View *);
NvSetRatesFn nvSetRates;
NvCreateViewFn nvCreateView;
NvSetViewFn nvSetView;

bool g_on;                         // [render] Vrs
float g_inner = 30, g_mid = 40;    // degrees from straight ahead: full detail, then half, then quarter
const char *g_state = "off";       // for the menu
bool g_bad, g_tried;

UINT g_w, g_h;                     // the eye picture's size (the game's back buffer)
ID3D11Resource *g_bb;              // not held: only compared

struct Pattern {
    ID3D11Texture2D *tex;
    ID3D11View *view;
    float key[6];                  // tan left, right, up, down, inner, mid it was made for
};
Pattern g_pat[2];                  // per eye
Pattern g_patScope;                // pip1: the scope picture's (key: radius in pixels, mode)
int g_scopeOut;                    // pip1: [aim] ScopeOutside
ID3D11View *g_applied;             // what the context has now (null = off)
bool g_unknown;                    // the context's state may have been reset
LONG g_switches;                   // on/off changes, for the log

template <class T> void Rel(T *&p) {
    if (p) p->Release(), p = nullptr;
}

bool Init() {
    if (g_tried) return !g_bad;
    g_tried = true, g_bad = true, g_state = "not available on this graphics driver";
    HMODULE m = LoadLibraryA("nvapi.dll");
    if (!m) return Log("vrs: nvapi.dll not found (error %lu); off", GetLastError()), false;
    typedef void *(__cdecl *QueryFn)(UINT id);
    QueryFn q = (QueryFn)GetProcAddress(m, "nvapi_QueryInterface");
    if (!q) return Log("vrs: nvapi.dll has no query function; off"), false;
    NvInitFn init = (NvInitFn)q(0x0150e828);
    int st = init ? init() : -1;
    nvSetRates = (NvSetRatesFn)q(0x34f7938f);
    nvCreateView = (NvCreateViewFn)q(0x99ca2dff);
    nvSetView = (NvSetViewFn)q(0x1b0c2f83);
    if (st || !nvSetRates || !nvCreateView || !nvSetView)
        return Log("vrs: driver interface missing (init %d, %p %p %p); off", st, (void *)nvSetRates, (void *)nvCreateView,
                   (void *)nvSetView), false;
    g_bad = false, g_state = "ready";
    Log("vrs: driver interface ready");
    return true;
}

void Fail(const char *what, int st) {
    g_bad = true, g_state = "switched itself off (see log)";
    Log("vrs: %s failed (status %d); off for this run", what, st);
}

// The rate picture for one eye: one byte per 16x16 pixel tile, an index into the table in Apply.
bool Build(int eye, const float t[4]) {
    Pattern &p = g_pat[eye];
    float key[6] = {t[0], t[1], t[2], t[3], g_inner, g_mid};
    if (p.view && !memcmp(key, p.key, sizeof key)) return true;
    ID3D11Device *dev = nullptr;
    TraceContext()->GetDevice(&dev);
    if (!dev) return false;
    Rel(p.view), Rel(p.tex);
    UINT tw = (g_w + 15) / 16, th = (g_h + 15) / 16;
    BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, tw * th);
    if (!px) return dev->Release(), false;
    float ti = tanf(g_inner * 0.0174533f), tm = tanf(g_mid * 0.0174533f);
    UINT n[3] = {};
    for (UINT j = 0; j < th; ++j)
        for (UINT i = 0; i < tw; ++i) {
            float u = (i + 0.5f) * 16 / g_w, v = (j + 0.5f) * 16 / g_h;
            float x = t[0] + (t[1] - t[0]) * u, y = t[2] - (t[2] - t[3]) * v;
            float r = sqrtf(x * x + y * y);
            BYTE k = r < ti ? 0 : r < tm ? (fabsf(x) > fabsf(y) ? 1 : 2) : 3;
            px[j * tw + i] = k, ++n[k == 0 ? 0 : k == 3 ? 2 : 1];
        }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = tw, td.Height = th, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA sd = {px, tw, 0};
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &p.tex);
    HeapFree(GetProcessHeap(), 0, px);
    if (FAILED(hr)) return dev->Release(), Fail("rate texture", (int)hr), false;
    NvViewDesc vd = {NV_VER(NvViewDesc), DXGI_FORMAT_R8_UINT, 4, 0, 0, 0};
    int st = nvCreateView(dev, p.tex, &vd, &p.view);
    dev->Release();
    if (st || !p.view) return Rel(p.tex), Fail("rate view", st), false;
    memcpy(p.key, key, sizeof key);
    float all = (float)(tw * th);
    Log("vrs: %s eye pattern %ux%u tiles for view %.1f/%.1f/%.1f/%.1f deg: full %.0f%%, half %.0f%%, quarter %.0f%% "
        "(shading work %.0f%% of before)",
        eye ? "right" : "left", tw, th, atanf(t[0]) * 57.2958f, atanf(t[1]) * 57.2958f, atanf(t[2]) * 57.2958f,
        atanf(t[3]) * 57.2958f, 100 * n[0] / all, 100 * n[1] / all, 100 * n[2] / all,
        100 * (n[0] + n[1] * 0.5f + n[2] * 0.25f) / all);
    return true;
}

// pip1: full rate inside the circle the lens shows (plus a margin), index 4 (1 per 4x4) or 5 (none) outside.
bool BuildScope(float rad, int mode) {
    Pattern &p = g_patScope;
    float key[6] = {rad, (float)mode, (float)g_w, (float)g_h, 0, 0};
    if (p.view && !memcmp(key, p.key, sizeof key)) return true;
    ID3D11Device *dev = nullptr;
    TraceContext()->GetDevice(&dev);
    if (!dev) return false;
    Rel(p.view), Rel(p.tex);
    UINT tw = (g_w + 15) / 16, th = (g_h + 15) / 16, inside = 0;
    BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, tw * th);
    if (!px) return dev->Release(), false;
    const float cx = g_w * 0.5f, cy = g_h * 0.5f, r = rad + 32;  // margin: the tile's corner, filtering, blur
    for (UINT j = 0; j < th; ++j)
        for (UINT i = 0; i < tw; ++i) {
            // nearest point of the tile to the middle
            float x0 = i * 16.f, x1 = x0 + 16, y0 = j * 16.f, y1 = y0 + 16;
            float dx = cx < x0 ? x0 - cx : cx > x1 ? cx - x1 : 0, dy = cy < y0 ? y0 - cy : cy > y1 ? cy - y1 : 0;
            bool in = dx * dx + dy * dy <= r * r;
            px[j * tw + i] = in ? 0 : mode >= 2 ? 5 : 4, inside += in;
        }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = tw, td.Height = th, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA sd = {px, tw, 0};
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &p.tex);
    HeapFree(GetProcessHeap(), 0, px);
    if (FAILED(hr)) return dev->Release(), Fail("scope rate texture", (int)hr), false;
    NvViewDesc vd = {NV_VER(NvViewDesc), DXGI_FORMAT_R8_UINT, 4, 0, 0, 0};
    int st = nvCreateView(dev, p.tex, &vd, &p.view);
    dev->Release();
    if (st || !p.view) return Rel(p.tex), Fail("scope rate view", st), false;
    memcpy(p.key, key, sizeof key);
    Log("vrs: scope picture pattern: full shading inside %.0f px of the middle (%.0f%% of the picture), %s outside",
        r, 100.0f * inside / (tw * th), mode >= 2 ? "none" : "1 per 4x4 pixels");
    return true;
}

void Apply(ID3D11DeviceContext *ctx, ID3D11View *view) {
    if (view == g_applied && !g_unknown) return;
    if (!view && !g_applied && !g_unknown) return;
    g_unknown = false;
    NvViewportRate vr = {};
    vr.enable = true;
    for (int &r : vr.table) r = kX1;
    vr.table[1] = k2x1, vr.table[2] = k1x2, vr.table[3] = k2x2;
    vr.table[4] = k4x4, vr.table[5] = kCull;  // pip1: the scope picture outside the lens
    NvViewportsRate d = {NV_VER(NvViewportsRate), view ? 1u : 0u, &vr};
    int st = nvSetView(ctx, view);
    if (!st) st = nvSetRates(ctx, &d);
    g_applied = view;
    ++g_switches;
    if (st) {
        Fail("setting the rates", st);
        d.count = 0;
        nvSetView(ctx, nullptr), nvSetRates(ctx, &d);
        g_applied = nullptr;
    }
}

// The view for the targets just bound, or null for "no coarse shading here".
bool ScopeWanted() { return g_scopeOut > 0 && StereoScopePassNow(); }

ID3D11View *Pick(ID3D11RenderTargetView *first, bool swapped) {
    if (ScopeWanted()) {  // pip1: every full-size target of the scope picture, the back buffer too (nothing of ours is drawn into it)
        if (g_bad || !first || HeadtrackOwn() || !g_w || !ScopeIsScopeImage()) return nullptr;
        ID3D11Resource *r = nullptr;
        first->GetResource(&r);
        if (!r) return nullptr;
        bool ok = r == g_bb;
        if (!ok) {
            ID3D11Texture2D *t = nullptr;
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) {
                D3D11_TEXTURE2D_DESC d;
                t->GetDesc(&d);
                ok = d.Width == g_w && d.Height == g_h;
                t->Release();
            }
        }
        r->Release();
        if (!ok || !Init() || !BuildScope(ScopeUsedRadiusPx(g_h), g_scopeOut)) return nullptr;
        return g_patScope.view;
    }
    if (!g_on || g_bad || swapped || !first || HeadtrackOwn() || !g_w) return nullptr;
    int eye = EngineDrawEye();
    if (eye < 0) return nullptr;
    ID3D11Resource *r = nullptr;
    first->GetResource(&r);
    if (!r) return nullptr;
    bool ok = false;
    if (r != g_bb) {
        ID3D11Texture2D *t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            ok = d.Width == g_w && d.Height == g_h;
            t->Release();
        }
    }
    r->Release();
    if (!ok) return nullptr;
    float t[4], hx, hy;
    if (!(EngineEyeOffCentreNow(eye) && XrGetEyeTan(eye, t))) {
        if (!XrGetHeadsetFov(&hx, &hy)) return nullptr;
        t[0] = -hx, t[1] = hx, t[2] = hy, t[3] = -hy;
    }
    if (!Init() || !Build(eye, t)) return nullptr;
    return g_pat[eye].view;
}

}  // namespace

bool *VrsOn() { return &g_on; }
int *VrsScopeOutside() { return &g_scopeOut; }
float *VrsInner() { return &g_inner; }
float *VrsMid() { return &g_mid; }
const char *VrsState() { return !g_on ? "off" : g_bad ? g_state : g_tried ? "working" : "starts with the next picture"; }

// trace.cpp: the game's immediate context has just been given these targets
// (swapped = the mod put its own HUD texture in their place).
void VrsOnTargets(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtv, bool swapped) {
    const bool scope = ScopeWanted();  // pip1
    if (!g_on && !scope && !g_applied && !g_unknown) return;  // the usual case with the option off: nothing at all
    if (g_bad && !g_applied) return;
    if (!nvSetView) {
        if (!(g_on || scope) || !Init()) return;
    }
    Apply(ctx, Pick(n && rtv ? rtv[0] : nullptr, swapped));
}

// Present: everything from here to the next eye is ours (overlays, the copy to the headset).
void VrsOnPresent(IDXGISwapChain *sc) {
    ID3D11DeviceContext *ctx = TraceContext();
    if (!ctx) return;
    if (g_applied || g_unknown) {
        if (nvSetView) Apply(ctx, nullptr);
        else g_unknown = false;
    }
    if ((!g_on && !g_scopeOut) || g_w) return;
    ID3D11Texture2D *bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    g_w = d.Width, g_h = d.Height, g_bb = bb;
    bb->Release();
}

// A command list was run on the context: its state (ours included) may be back to defaults.
void VrsContextReset() {
    if (g_applied) g_unknown = true;
}

void VrsOnSwapChainGoing() {
    ID3D11DeviceContext *ctx = TraceContext();
    if (ctx && g_applied && nvSetView) Apply(ctx, nullptr);
    for (Pattern &p : g_pat) Rel(p.view), Rel(p.tex);
    Rel(g_patScope.view), Rel(g_patScope.tex);
    g_w = g_h = 0, g_bb = nullptr;
}

// Every 10 s with the option on (hook.cpp's frames line): how often it switched.
void VrsLog() {
    if (!g_on) return;
    LONG n = InterlockedExchange(&g_switches, 0);
    LogV("vrs: %ld on/off switches in the last 10 s, state %s", n, VrsState());
}
