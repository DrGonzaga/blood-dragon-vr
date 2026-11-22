// HUD in VR (hud1). The game draws its HUD (and its pause menu) straight
// into the eye image after the scene, at the screen edges, so in the headset
// it sits flat at infinity and mostly outside what you can see. Here the game's
// passes into the back buffer after the scene's own one are pointed at a
// texture of ours instead, and that texture is drawn into each eye as a panel
// floating in front of you (head-locked, in 3D at a set distance).
//
// "Pass" = each time the game binds the back buffer as its render target on
// its immediate context during one eye. The first (0) is the scene's final
// image; the HUD starts at the pass set by HudFirstPass (default 1). Debug
// menu: on/off, distance, size, height, opacity, and the starting pass.
// Saved in fc3vr.ini [hud].
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>

#include "fc3vr.h"

// crash2: a texture vtable, so the crash logger can recognise D3D textures on the stack.
static void *volatile g_texVt;
void *HudTextureVtable() { return g_texVt; }

namespace {

const char kShader[] =
    "cbuffer C : register(b0) { float4 corner[4]; float4 opacity; float4 uvr; float4 mask0; float4 mask1; float4 mflags; float4 pflags; float4 hbox; float4 kbox; float4 kflags; float4 qbox; };\n"
    // opacity.y: hidden areas, one bit per cell of a 3x3 grid (row-major from
    // top-left), the outer rows/columns 30% wide.
    // map1/ammo1: uvr = the part of the HUD texture this quad shows (x0 y0 x1 y1);
    // mask0/1 = parts of the HUD on panels of their own (minimap, ammo);
    // mflags x/y = leave mask 0/1 out of the main panel, z/w = outline it (lining up);
    // pflags.x 1 = this is a cut-out panel, pflags.y 1 = round.
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.p = corner[id]; o.uv = float2(id & 1, id >> 1); return o; }\n"
    "bool In(float2 uv, float4 m) { return m.z > m.x && all(uv >= m.xy) && all(uv <= m.zw); }\n"
    "bool Edge(float2 uv, float4 m) { float2 e = min(uv - m.xy, m.zw - uv); return min(e.x, e.y) < 0.004; }\n"
    // The HUD was drawn over transparent black, so its colour is already
    // multiplied by its coverage; if the game left alpha unwritten, take
    // coverage from the brightness.
    "float4 ps(V i) : SV_Target {\n"
    "  float2 uv = lerp(uvr.xy, uvr.zw, i.uv);\n"
    // hide1 (DrGonzaga 3 Nov, crouch icon): a box of the HUD image never shown, on any panel;
    // pflags.z = on, pflags.w = outline it (lining up).
    "  if (In(uv, hbox)) { if (pflags.w > 0.5 && Edge(uv, hbox)) return float4(0, 0.9, 1, 1); if (pflags.z > 0.5) return 0; }\n"
    "  if (pflags.x > 0.5) {\n"
    "    if (pflags.y > 0.5 && length(i.uv - 0.5) > 0.5) return 0;\n"
    "    if (kflags.z > 0.5 && kflags.x > 0.5 && In(uv, kbox)) return 0;\n"
    "  } else {\n"
    // farquest1: opacity.w 1 = the quest icon's patch (qbox) is left out of the main panel (drawn far away on its own)
    "    if (opacity.w > 0.5 && In(uv, qbox)) return 0;\n"
    "    int cx = uv.x < 0.3 ? 0 : uv.x < 0.7 ? 1 : 2, cy = uv.y < 0.3 ? 0 : uv.y < 0.7 ? 1 : 2;\n"
    "    if (((uint)opacity.y >> (cy * 3 + cx)) & 1) return 0;\n"
    // keep1 (DrGonzaga 10 Nov): kbox = a part of the minimap box that stays on the main
    // panel (game prompts like "press X to cut plant"); kflags x on, y outline, z this
    // quad is the minimap's own panel.
    "    if (kflags.y > 0.5 && In(uv, kbox) && Edge(uv, kbox)) return float4(0.2, 1, 0.3, 1);\n"
    "    if (In(uv, mask0) && !(kflags.x > 0.5 && In(uv, kbox))) { if (mflags.z > 0.5 && Edge(uv, mask0)) return float4(1, 0.85, 0, 1); if (mflags.x > 0.5) return 0; }\n"
    "    if (In(uv, mask1)) { if (mflags.w > 0.5 && Edge(uv, mask1)) return float4(1, 0.85, 0, 1); if (mflags.y > 0.5) return 0; }\n"
    "  }\n"
    "  float4 c = t.Sample(s, uv);\n"
    "  float a = max(c.a, max(c.r, max(c.g, c.b))); return float4(c.rgb, a) * opacity.x; }\n"
    // ammokeep1: counts (occlusion query) the pixels of a HUD part that are at least opacity.z covered.
    "float4 psc(V i) : SV_Target { float2 uv = lerp(uvr.xy, uvr.zw, i.uv); float4 c = t.Sample(s, uv);\n"
    "  float a = max(c.a, max(c.r, max(c.g, c.b))); clip(a - opacity.z); return 1; }\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);

struct Cb {
    float corner[4][4];
    float opacity[4];
    float uvr[4];
    float mask[2][4];
    float mflags[4];
    float pflags[4];
    float hbox[4];
    float kbox[4];    // keep1
    float kflags[4];
    float qbox[4];    // farquest1: the quest icon's patch of the HUD picture
};

// map1 / ammo1 (DrGonzaga 3 Nov): parts of the HUD cut out onto panels of their
// own, which turn to face you: 0 the minimap (with health), 1 ammo. Anchors:
// 0 head (moves with your view), 1 body (level, turns with you; look down to
// see it), 2 left wrist, 3 right wrist. Off while the game is paused (menus).
const float kDefRect[kHudPanels][4] = {{0.03f, 0.66f, 0.23f, 0.92f}, {0.76f, 0.80f, 0.97f, 0.95f}};
HudPanelCfg g_el[kHudPanels];
HudHideBox g_hbox = {false, {0.40f, 0.85f, 0.46f, 0.95f}};  // hide1: a guess; DrGonzaga lines it up
volatile DWORD g_hboxOutlineAt;
HudHideBox g_kbox = {false, {0.03f, 0.58f, 0.30f, 0.68f}};  // keep1: a guess; DrGonzaga lines it up
volatile DWORD g_kboxOutlineAt;
volatile DWORD g_outlineAt[kHudPanels];  // menu showing that panel's controls (refreshed each frame)
const wchar_t *kPfx[kHudPanels] = {L"Map", L"Ammo"};

// Settings (Debug menu).
volatile bool g_on = true;
volatile bool g_visible = true;  // shortcut (Buttons page): hides the panel, the HUD stays off the eye image
float g_dist = 1.6f, g_size = 1.4f, g_height = 0.0f, g_opacity = 0.9f;
volatile int g_first = 1;
int g_hide;  // areas hidden (bits, see the shader)
volatile bool g_mapShown = true;  // lasers1: [hud] MinimapShown, X on foot toggles it

bool g_failed;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11PixelShader *g_psCount;  // ammokeep1
ID3D11Buffer *g_cb;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_ds;
ID3D11BlendState *g_bs;
ID3D11SamplerState *g_smp;

// Per swapchain.
IDXGISwapChain *g_sc;
ID3D11Texture2D *g_bb;          // the back buffer (compared, not used)
ID3D11RenderTargetView *g_bbRtv;  // ours, for drawing the panel
ID3D11Texture2D *g_tex;         // the HUD
ID3D11RenderTargetView *g_texRtv;
ID3D11ShaderResourceView *g_texSrv;
UINT g_w, g_h;
DXGI_FORMAT g_rtvFormat;

// This eye.
volatile int g_pass;      // back buffer binds so far
volatile bool g_captured;  // some went to our texture
ID3D11RenderTargetView *g_lastBbRtv;  // the game's view of the back buffer last seen (not held)

// Stats (logged every 10 s and shown in the menu).
LONG g_eyes, g_eyesCaptured, g_passSum, g_shown;
volatile LONG g_lastPasses;
DWORD g_statAt;

// ammokeep1 (bd80; DrGonzaga 15 Nov, on bd78's mod-drawn ammo numbers: "I don't like the new graphics, is there any
// way to get the games image but behave as I said (appears upon look, and that's it)"). The game only draws its
// ammo for a few seconds after a shot, a reload or a weapon change. So a copy of that part of the HUD picture is
// kept from the last time the game showed it in full, and the ammo panel shows the copy while the game's own is
// away. "Showing" is measured, not guessed: two occlusion queries count the part's pixels that are at least 25%
// and at least 85% covered; enough of the first = the game is drawing it now (the panel shows it live), and
// enough of the second among them = fully faded in, so the copy is taken (never a half-faded one).
bool g_ammoKeep = true;  // [hud] AmmoKeepPicture
// onehud1 (bd81; DrGonzaga 15 Nov: "the objectives appear in different parts of each eye. Is there any way to just
// choose an eye and have that one in both eyes"). The game draws its HUD once for each eye's picture, and
// between the two its own update moves the icons that follow the world (the quest icon moves whenever the head
// turns), so the two eyes got pictures a moment apart. Now the left eye's HUD picture is kept and shown in the
// right eye too; the game's HUD passes for the right eye draw nowhere (no target), which also saves their cost.
bool g_oneHud = true;  // [hud] SameHudBothEyes
// farhud1 (DrGonzaga 16 Nov, after bd84: "HUD mission icons still different per eye"): one picture in both eyes was not
// enough. The quest icon is placed on the main panel in the direction of its target, but the panel sat at its own
// distance (a metre or two) with the usual left/right eye offset, so looking at a target 50 m away the icon split
// into two, one per eye. farquest1 (DrGonzaga 16 Nov: other HUD parts such as the dragon's health and "press X to
// loot" should stay at the HUD's distance): only a patch round the quest icon (markers.cpp tells where it is) is
// cut out of the main panel and drawn on its own with no eye offset ("far away", like the stars), so the icon
// lines up with its target in both eyes; everything else stays where it was.
bool g_mainFar = true;  // [hud] QuestIconFar
float g_qHalf[2] = {0.07f, 0.12f};  // [hud] QuestPatchWidth / QuestPatchHeight: half the patch, in HUD picture fractions
float g_movieW = 2.4f, g_movieD = 2.5f;  // movie1: [hud] MovieWidth, MovieDistance (metres)
bool g_movieBlack = true;                // movie1: [hud] MovieBlack
bool g_leftKept;       // the HUD texture still holds this frame's left-eye picture, for the right eye
ID3D11Texture2D *g_ammoTex, *g_ammoScratch;
ID3D11ShaderResourceView *g_ammoSrv;
ID3D11RenderTargetView *g_ammoScratchRtv;
ID3D11Query *g_ammoQ[2];  // 0: 25% covered, 1: 85% covered
bool g_ammoQBusy, g_ammoHave, g_ammoLive, g_ammoFailed;
UINT g_ammoBox[4];  // left, top, right, bottom in HUD pixels
DWORD g_ammoFullT;  // when the copy was last taken
void AmmoKeepRelease() {
    if (g_ammoSrv) g_ammoSrv->Release(), g_ammoSrv = nullptr;
    if (g_ammoTex) g_ammoTex->Release(), g_ammoTex = nullptr;
    if (g_ammoScratchRtv) g_ammoScratchRtv->Release(), g_ammoScratchRtv = nullptr;
    if (g_ammoScratch) g_ammoScratch->Release(), g_ammoScratch = nullptr;
    for (auto &q : g_ammoQ)
        if (q) q->Release(), q = nullptr;
    g_ammoQBusy = g_ammoHave = g_ammoLive = false;
}
// The copy's texture for the ammo box as it is set now (made again when the box or the HUD picture changes).
bool AmmoKeepSetup(const float *rect) {
    if (g_ammoFailed || !g_psCount || !g_tex) return false;
    UINT b[4] = {(UINT)(rect[0] * g_w), (UINT)(rect[1] * g_h), (UINT)(rect[2] * g_w + 0.5f), (UINT)(rect[3] * g_h + 0.5f)};
    if (b[2] > g_w) b[2] = g_w;
    if (b[3] > g_h) b[3] = g_h;
    if (b[2] < b[0] + 8 || b[3] < b[1] + 8) return false;
    if (g_ammoTex && !memcmp(b, g_ammoBox, sizeof b)) return true;
    AmmoKeepRelease();
    memcpy(g_ammoBox, b, sizeof b);
    D3D11_TEXTURE2D_DESC td;
    g_tex->GetDesc(&td);
    td.Width = b[2] - b[0], td.Height = b[3] - b[1], td.MipLevels = 1, td.ArraySize = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = g_rtvFormat, rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0};
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_ammoTex)) || FAILED(g_dev->CreateShaderResourceView(g_ammoTex, nullptr, &g_ammoSrv)) ||
        FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_ammoScratch)) || FAILED(g_dev->CreateRenderTargetView(g_ammoScratch, &rd, &g_ammoScratchRtv)) ||
        FAILED(g_dev->CreateQuery(&qd, &g_ammoQ[0])) || FAILED(g_dev->CreateQuery(&qd, &g_ammoQ[1]))) {
        Log("hud: couldn't make the kept ammo picture (%ux%u); the ammo panel works as before", td.Width, td.Height);
        AmmoKeepRelease();
        g_ammoFailed = true;
        return false;
    }
    Log("hud: ammo picture kept between showings: %ux%u of the HUD at %u,%u", td.Width, td.Height, b[0], b[1]);
    return true;
}
// Once per eye, the HUD picture bound and our pipeline set: read the last count, take the copy, count again.
// Leaves the scratch target and the counting shader bound; the caller puts its own back.
void AmmoKeepProbe(ID3D11DeviceContext *c, const Cb &cb, const float *rect) {
    static int s_logs;
    if (g_ammoQBusy) {
        UINT64 lo = 0, hi = 0;
        if (c->GetData(g_ammoQ[0], &lo, sizeof lo, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            c->GetData(g_ammoQ[1], &hi, sizeof hi, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;  // not ready yet: next eye
        g_ammoQBusy = false;
        const UINT64 area = (UINT64)(g_ammoBox[2] - g_ammoBox[0]) * (g_ammoBox[3] - g_ammoBox[1]);
        const UINT64 need = area / 1500 > 24 ? area / 1500 : 24;
        const bool live = lo >= need, full = live && hi * 100 >= lo * 30;
        if (live != g_ammoLive && s_logs < 40)
            ++s_logs, Log("hud: the game's ammo picture %s (%lu pixels drawn, %lu of them solid; needs %lu)%s", live ? "is showing" : "has gone", (unsigned long)lo,
                          (unsigned long)hi, (unsigned long)need, live ? "" : g_ammoHave ? "; the kept copy shows on the panel" : "; no copy kept yet");
        g_ammoLive = live;
        if (full) {
            D3D11_BOX bx = {g_ammoBox[0], g_ammoBox[1], 0, g_ammoBox[2], g_ammoBox[3], 1};
            c->CopySubresourceRegion(g_ammoTex, 0, 0, 0, 0, g_tex, 0, &bx);
            if (!g_ammoHave) Log("hud: first copy of the game's ammo picture kept (%lu pixels drawn, %lu solid)", (unsigned long)lo, (unsigned long)hi);
            g_ammoHave = true, g_ammoFullT = GetTickCount();
        }
    }
    Cb q = {};
    static const float kFull[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
    for (int k = 0; k < 4; ++k) q.corner[k][0] = kFull[k][0], q.corner[k][1] = kFull[k][1], q.corner[k][3] = 1;
    memcpy(q.uvr, rect, 16);
    D3D11_VIEWPORT vp = {0, 0, (float)(g_ammoBox[2] - g_ammoBox[0]), (float)(g_ammoBox[3] - g_ammoBox[1]), 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &g_ammoScratchRtv, nullptr);
    c->PSSetShader(g_psCount, nullptr, 0);
    for (int i = 0; i < 2; ++i) {
        q.opacity[2] = i ? 0.85f : 0.25f;
        c->UpdateSubresource(g_cb, 0, nullptr, &q, 0, 0);
        c->Begin(g_ammoQ[i]);
        c->Draw(4, 0);
        c->End(g_ammoQ[i]);
    }
    g_ammoQBusy = true;
    (void)cb;
}

void ReleaseChain() {
    AmmoKeepRelease();
    if (g_texSrv) g_texSrv->Release(), g_texSrv = nullptr;
    if (g_texRtv) g_texRtv->Release(), g_texRtv = nullptr;
    if (g_tex) g_tex->Release(), g_tex = nullptr;
    if (g_bbRtv) g_bbRtv->Release(), g_bbRtv = nullptr;
    if (g_bb) g_bb->Release(), g_bb = nullptr;
    g_sc = nullptr;
    g_lastBbRtv = nullptr;
}

bool SetupDevice(IDXGISwapChain *sc) {
    if (g_failed) return false;
    if (g_dev) return true;
    ID3D11Device *dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return g_failed = true, false;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (!compile || FAILED(compile(kShader, sizeof kShader - 1, "hud", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, sizeof kShader - 1, "hud", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        Log("hud: shader compile failed; HUD stays where the game draws it");
        if (err) err->Release();
        dev->Release();
        return g_failed = true, false;
    }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(), psb->Release();
    {  // ammokeep1 (without it the ammo panel just works as before)
        ID3DBlob *cb2 = nullptr, *e2 = nullptr;
        if (SUCCEEDED(compile(kShader, sizeof kShader - 1, "hud", nullptr, nullptr, "psc", "ps_4_0", 0, 0, &cb2, &e2)) && cb2)
            dev->CreatePixelShader(cb2->GetBufferPointer(), cb2->GetBufferSize(), nullptr, &g_psCount);
        else Log("hud: the counting shader did not compile; the ammo picture is not kept");
        if (cb2) cb2->Release();
        if (e2) e2->Release();
    }
    D3D11_BUFFER_DESC bd = {sizeof(Cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dev->CreateDepthStencilState(&dd, &g_ds);
    D3D11_BLEND_DESC bld = {};
    auto &b = bld.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_ONE, b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, b.BlendOp = D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO, b.DestBlendAlpha = D3D11_BLEND_ONE, b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &g_bs);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;  // transparent outside
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_smp);
    dev->GetImmediateContext(&g_ctx);  // kept
    g_dev = dev;
    if (!g_ctx || !g_vs || !g_ps || !g_cb || !g_rs || !g_ds || !g_bs || !g_smp) {
        Log("hud: setup incomplete; HUD stays where the game draws it");
        return g_failed = true, false;
    }
    Log("hud: ready");
    return true;
}

// The HUD texture: like the back buffer, viewed the way the game views it.
bool SetupChain(IDXGISwapChain *sc, ID3D11RenderTargetView *gameRtv) {
    if (sc == g_sc && g_tex) return true;
    ReleaseChain();
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&g_bb))) return false;
    D3D11_TEXTURE2D_DESC td;
    g_bb->GetDesc(&td);
    g_w = td.Width, g_h = td.Height;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    gameRtv->GetDesc(&rd);
    g_rtvFormat = rd.Format;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0, td.CPUAccessFlags = 0, td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tex)) || FAILED(g_dev->CreateRenderTargetView(g_tex, &rd, &g_texRtv)) ||
        FAILED(g_dev->CreateShaderResourceView(g_tex, nullptr, &g_texSrv)) ||
        FAILED(g_dev->CreateRenderTargetView(g_bb, nullptr, &g_bbRtv))) {
        Log("hud: couldn't make the HUD texture (%ux%u format %d)", td.Width, td.Height, (int)td.Format);
        ReleaseChain();
        g_failed = true;
        return false;
    }
    g_texVt = *(void **)g_tex;
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_texRtv, clear);
    g_sc = sc;
    Log("hud: HUD texture %ux%u format %d", td.Width, td.Height, (int)g_rtvFormat);
    return true;
}

// Checked on every bind (hud2): a remembered view pointer could be reused by
// the game for another texture after it frees the old view.
bool IsBackBuffer(ID3D11RenderTargetView *v) {
    if (!v || !g_bb) return false;
    ID3D11Resource *r = nullptr;
    v->GetResource(&r);
    bool bb = r == g_bb;
    if (r) r->Release();
    if (bb) g_lastBbRtv = v;
    return bb;
}

void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"hud", key, buf, path);
}
float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32], d[32];
    IniPath(path);
    swprintf(d, 32, L"%.3f", def);
    GetPrivateProfileStringW(L"hud", key, d, buf, 32, path);
    return (float)_wtof(buf);
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

static void Key(wchar_t *out, int e, const wchar_t *k) { swprintf(out, 40, L"%ls%ls", kPfx[e], k); }
static void HudPanelLoad(int e) {
    HudPanelCfg &c = g_el[e];
    wchar_t k[40];
    memcpy(c.rect, kDefRect[e], 16);
    HudPanelDefaults(e, 1);
    Key(k, e, L"OwnPanel"), c.own = LoadF(k, 0) != 0;
    static const wchar_t *rk[4] = {L"Left", L"Top", L"Right", L"Bottom"};
    for (int i = 0; i < 4; ++i) Key(k, e, rk[i]), c.rect[i] = Clamp(LoadF(k, c.rect[i]), 0, 1);
    Key(k, e, L"Anchor"), c.anchor = (int)Clamp(LoadF(k, (float)c.anchor), 0, 4);
    HudPanelDefaults(e, c.anchor);
    Key(k, e, L"Size"), c.size = Clamp(LoadF(k, c.size), 0.05f, 1.5f);
    static const wchar_t *ok[3] = {L"PosRight", L"PosUp", L"PosAhead"};
    for (int i = 0; i < 3; ++i) Key(k, e, ok[i]), c.off[i] = Clamp(LoadF(k, c.off[i]), -2, 2);
    Key(k, e, L"Opacity"), c.opacity = Clamp(LoadF(k, c.opacity), 0.05f, 1);
    Key(k, e, L"Round"), c.round = LoadF(k, 0) != 0;
    // wrist1: default on for the ammo panel only (DrGonzaga asked for it there)
    Key(k, e, L"Fixed"), c.fixed = LoadF(k, e == 1 ? 1.f : 0.f) != 0;
    Key(k, e, L"Tilt"), c.tilt = Clamp(LoadF(k, 45), -90, 90);
    Key(k, e, L"Turn"), c.turn = Clamp(LoadF(k, 0), -90, 90);
    Key(k, e, L"Glance"), c.glance = LoadF(k, 1) != 0;
    Key(k, e, L"GlanceAngle"), c.glanceDeg = Clamp(LoadF(k, 40), 10, 80);
    // glance2 (DrGonzaga 20 Nov: "I need to really tilt the guns to show it"): the wrist turn has its own angle now,
    // easier by default; GlanceAngle is only how directly you look.
    Key(k, e, L"GlanceTurnAngle"), c.turnDeg = Clamp(LoadF(k, 75), 10, 89);
}
static void HudPanelSave(int e) {
    const HudPanelCfg &c = g_el[e];
    wchar_t k[40];
    Key(k, e, L"OwnPanel"), SaveF(k, c.own ? 1.f : 0.f);
    static const wchar_t *rk[4] = {L"Left", L"Top", L"Right", L"Bottom"};
    for (int i = 0; i < 4; ++i) Key(k, e, rk[i]), SaveF(k, c.rect[i]);
    Key(k, e, L"Anchor"), SaveF(k, (float)c.anchor);
    Key(k, e, L"Size"), SaveF(k, c.size);
    static const wchar_t *ok[3] = {L"PosRight", L"PosUp", L"PosAhead"};
    for (int i = 0; i < 3; ++i) Key(k, e, ok[i]), SaveF(k, c.off[i]);
    Key(k, e, L"Opacity"), SaveF(k, c.opacity);
    Key(k, e, L"Round"), SaveF(k, c.round ? 1.f : 0.f);
    Key(k, e, L"Fixed"), SaveF(k, c.fixed ? 1.f : 0.f);  // wrist1
    Key(k, e, L"Tilt"), SaveF(k, c.tilt);
    Key(k, e, L"Turn"), SaveF(k, c.turn);
    Key(k, e, L"Glance"), SaveF(k, c.glance ? 1.f : 0.f);
    Key(k, e, L"GlanceAngle"), SaveF(k, c.glanceDeg);
    Key(k, e, L"GlanceTurnAngle"), SaveF(k, c.turnDeg);  // glance2
}

void HudLoad() {
    g_on = LoadF(L"On", 1) != 0;
    g_dist = Clamp(LoadF(L"Distance", g_dist), 0.3f, 5);
    g_size = Clamp(LoadF(L"Size", g_size), 0.2f, 4);
    g_height = Clamp(LoadF(L"Height", g_height), -1, 1);
    g_opacity = Clamp(LoadF(L"Opacity", g_opacity), 0.05f, 1);
    g_first = (int)Clamp(LoadF(L"FirstPass", 1), 1, 6);
    g_hide = (int)LoadF(L"Hide", 0) & 0x1ff;
    g_mapShown = LoadF(L"MinimapShown", 1) != 0;  // lasers1
    for (int e = 0; e < kHudPanels; ++e) HudPanelLoad(e);  // map1 / ammo1
    AmmoLoad();  // ammo2
    g_ammoKeep = LoadF(L"AmmoKeepPicture", 1) != 0;  // ammokeep1
    g_oneHud = LoadF(L"SameHudBothEyes", 1) != 0;  // onehud1
    g_mainFar = LoadF(L"QuestIconFar", 1) != 0;     // farquest1
    g_qHalf[0] = Clamp(LoadF(L"QuestPatchWidth", 0.07f), 0.01f, 0.3f), g_qHalf[1] = Clamp(LoadF(L"QuestPatchHeight", 0.12f), 0.01f, 0.4f);
    g_movieOn = LoadF(L"MovieScreen", 1) != 0;       // movie1
    g_movieComic = LoadF(L"MovieForComic", 1) != 0;  // movie3
    g_movieBlack = LoadF(L"MovieBlack", 1) != 0;
    g_movieW = Clamp(LoadF(L"MovieWidth", 2.4f), 0.5f, 8.0f);
    g_movieD = Clamp(LoadF(L"MovieDistance", 2.5f), 0.5f, 10.0f);
    g_hbox.on = LoadF(L"HideBoxOn", 0) != 0;  // hide1
    static const wchar_t *hk[4] = {L"HideBoxLeft", L"HideBoxTop", L"HideBoxRight", L"HideBoxBottom"};
    for (int i = 0; i < 4; ++i) g_hbox.rect[i] = Clamp(LoadF(hk[i], g_hbox.rect[i]), 0, 1);
    g_kbox.on = LoadF(L"MapKeepBoxOn", 0) != 0;  // keep1
    static const wchar_t *kk[4] = {L"MapKeepBoxLeft", L"MapKeepBoxTop", L"MapKeepBoxRight", L"MapKeepBoxBottom"};
    for (int i = 0; i < 4; ++i) g_kbox.rect[i] = Clamp(LoadF(kk[i], g_kbox.rect[i]), 0, 1);
}

void HudSave() {
    SaveF(L"On", g_on ? 1.f : 0.f);
    SaveF(L"Distance", g_dist);
    SaveF(L"Size", g_size);
    SaveF(L"Height", g_height);
    SaveF(L"Opacity", g_opacity);
    SaveF(L"FirstPass", (float)g_first);
    SaveF(L"Hide", (float)g_hide);
    SaveF(L"MinimapShown", g_mapShown ? 1.f : 0.f);  // lasers1
    for (int e = 0; e < kHudPanels; ++e) HudPanelSave(e);  // map1 / ammo1
    AmmoSave();  // ammo2
    SaveF(L"AmmoKeepPicture", g_ammoKeep ? 1.f : 0.f);  // ammokeep1
    SaveF(L"SameHudBothEyes", g_oneHud ? 1.f : 0.f);  // onehud1
    SaveF(L"QuestIconFar", g_mainFar ? 1.f : 0.f);     // farquest1
    SaveF(L"QuestPatchWidth", g_qHalf[0]), SaveF(L"QuestPatchHeight", g_qHalf[1]);
    SaveF(L"MovieScreen", g_movieOn ? 1.f : 0.f);       // movie1
    SaveF(L"MovieForComic", g_movieComic ? 1.f : 0.f);  // movie3
    SaveF(L"MovieBlack", g_movieBlack ? 1.f : 0.f);
    SaveF(L"MovieWidth", g_movieW), SaveF(L"MovieDistance", g_movieD);
    SaveF(L"HideBoxOn", g_hbox.on ? 1.f : 0.f);  // hide1
    static const wchar_t *hk[4] = {L"HideBoxLeft", L"HideBoxTop", L"HideBoxRight", L"HideBoxBottom"};
    for (int i = 0; i < 4; ++i) SaveF(hk[i], g_hbox.rect[i]);
    SaveF(L"MapKeepBoxOn", g_kbox.on ? 1.f : 0.f);  // keep1
    static const wchar_t *kk[4] = {L"MapKeepBoxLeft", L"MapKeepBoxTop", L"MapKeepBoxRight", L"MapKeepBoxBottom"};
    for (int i = 0; i < 4; ++i) SaveF(kk[i], g_kbox.rect[i]);
}
HudHideBox *HudHideBoxCfg() { return &g_hbox; }
void HudHideBoxOutline(bool on) { g_hboxOutlineAt = on ? GetTickCount() : 0; }
HudHideBox *HudKeepBoxCfg() { return &g_kbox; }
void HudKeepBoxOutline(bool on) { g_kboxOutlineAt = on ? GetTickCount() : 0; }

HudPanelCfg *HudPanel(int e) { return &g_el[e < 0 ? 0 : e >= kHudPanels ? kHudPanels - 1 : e]; }
void HudPanelOutline(int e, bool on) { g_outlineAt[e] = on ? GetTickCount() : 0; }
void HudPanelDefaults(int e, int anchor) {
    // Starting spots per anchor: right, up, ahead (wrist: right, up, back), size.
    static const float k[kHudPanels][5][4] = {  // anchor 4: from the chest spheres' centre
        {{-0.22f, -0.20f, 0.60f, 0.20f}, {-0.22f, -0.38f, 0.55f, 0.22f}, {0.00f, 0.07f, 0.06f, 0.14f}, {0.00f, 0.07f, 0.06f, 0.14f}, {0.00f, 0.13f, 0.02f, 0.18f}},
        {{0.24f, -0.22f, 0.60f, 0.16f}, {0.22f, -0.38f, 0.55f, 0.16f}, {0.00f, 0.05f, 0.10f, 0.10f}, {0.00f, 0.05f, 0.10f, 0.10f}, {0.24f, 0.06f, 0.02f, 0.14f}}};
    anchor = anchor < 0 ? 0 : anchor > 4 ? 4 : anchor;
    HudPanelCfg &c = g_el[e];
    c.anchor = anchor;
    c.off[0] = k[e][anchor][0], c.off[1] = k[e][anchor][1], c.off[2] = k[e][anchor][2], c.size = k[e][anchor][3];
}
void HudPanelReset(int e) {
    HudPanelCfg &c = g_el[e];
    memcpy(c.rect, kDefRect[e], 16);
    c.opacity = 1, c.round = false;
    c.fixed = e == 1, c.tilt = 45, c.turn = 0, c.glance = true, c.glanceDeg = 40, c.turnDeg = 75;  // wrist1, glance2
    HudPanelDefaults(e, 1);
}

bool HudOn() { return g_on; }
void HudSetOn(bool on) { g_on = on; }
bool HudShown() { return g_visible; }
void HudSetShown(bool on) {
    g_visible = on;
    Log("hud: %s (shortcut)", on ? "shown" : "hidden");
}
float *HudDistance() { return &g_dist; }
float *HudSize() { return &g_size; }
float *HudHeight() { return &g_height; }
float *HudOpacity() { return &g_opacity; }
int *HudFirstPass() { return (int *)&g_first; }
int *HudHidden() { return &g_hide; }
bool *HudMinimapShown() { return (bool *)&g_mapShown; }
static void SaveMapShown() { SaveF(L"MinimapShown", g_mapShown ? 1.f : 0.f); }
void HudMinimapToggle() {  // input thread: the ini write goes to the menu's save worker
    g_mapShown = !g_mapShown;
    Log("hud: minimap %s (X)", g_mapShown ? "shown" : "hidden");
    MenuSaveLater(SaveMapShown);
}

void HudOnSwapChainGoing() { ReleaseChain(); }

// trace.cpp, OMSetRenderTargets(AndUAVs) on the game's immediate context:
// returns the targets to bind (tmp holds a changed copy).
// hud2: each new kind of back-buffer bind (pass number, target count, depth
// buffer size) is logged once, so a crash can be matched to what came just before.
static void NoteKind(int pass, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    static DWORD seen[32];
    static int nseen;
    UINT dw = 0, dh = 0, used = 0;
    for (UINT i = 0; i < n; ++i)
        if (rtv[i]) ++used;
    if (dsv) {
        ID3D11Resource *r = nullptr;
        dsv->GetResource(&r);
        ID3D11Texture2D *t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t))) {
            D3D11_TEXTURE2D_DESC d;
            t->GetDesc(&d);
            dw = d.Width, dh = d.Height;
            t->Release();
        }
        if (r) r->Release();
    }
    DWORD key = (DWORD)(pass & 0xf) | (n << 4) | (used << 8) | ((dw & 0xfff) << 12) | ((dh & 0xff) << 24);
    for (int i = 0; i < nseen; ++i)
        if (seen[i] == key) return;
    if (nseen < 32) seen[nseen++] = key;
    Log("hud: new back-buffer pass kind: pass %d, %u targets (%u used), depth %ux%u%s", pass, n, used, dw, dh,
        pass >= g_first && n == 1 ? " -> HUD panel" : pass >= g_first ? " -> left in the image (not a plain HUD pass)" : "");
}

ID3D11RenderTargetView *const *HudTargets(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                          ID3D11RenderTargetView **tmp) {
    if (!g_on || !g_dev || !g_bb || HeadtrackOwn() || !n || n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT || !rtv ||
        !rtv[0] || !IsBackBuffer(rtv[0]))
        return rtv;
    int pass = g_pass++;
    if (g_verboseLog) NoteKind(pass, n, rtv, dsv);  // review1: diagnostic, detailed log only
    if (n != 1) return rtv;  // only the plain HUD passes seen working (one target)
    if (pass < g_first || !HeadtrackActive() || EngineFlat() || !g_texRtv) return rtv;  // flat1: the 2D screen keeps the game's own HUD (subtitles)
    if (ScopeIsScopeImage()) {
        // scope1 kept the game's HUD in the scope view (for its reticle); nohud1: our
        // reticles replaced it, so by default its passes draw nowhere (no target bound).
        if (!*ScopeHideHud()) return rtv;
        for (UINT i = 0; i < n; ++i) tmp[i] = nullptr;
        return tmp;
    }
    if (g_leftKept) {
        if (g_oneHud && StereoEyeNow() == 1) {  // onehud1: the right eye shows the left eye's picture
            for (UINT i = 0; i < n; ++i) tmp[i] = nullptr;
            g_captured = true;
            return tmp;
        }
        const float clear[4] = {0, 0, 0, 0};  // a kept picture nobody used (the right eye was not drawn): start clean
        g_ctx->ClearRenderTargetView(g_texRtv, clear);
        g_leftKept = false;
    }
    for (UINT i = 0; i < n; ++i) tmp[i] = rtv[i];
    tmp[0] = g_texRtv;
    g_captured = true;
    return tmp;
}

static float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void Norm3(float *a) {
    float l = sqrtf(Dot3(a, a));
    if (l > 1e-6f) a[0] /= l, a[1] /= l, a[2] /= l;
}
static void Cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// map1: the minimap panel's corners in this eye (clip positions, w = depth),
// placed in the recentred XR frame (metres; x right, y up, z back) and turned
// to face the head, then taken into the eye the way the menu panel is.
// wrist1: *seen = 1 while a wrist panel fixed to the controller faces you and you look at it
// (glance on; 'shown' widens the angle by 10 deg so it does not flicker at the edge), else 0.
static float g_panelC[kHudPanels][3], g_panelFade[kHudPanels];  // ammo2: where each own panel is (XR frame) and how far faded in
static bool g_panelCOk[kHudPanels];
static DWORD g_panelT;  // when the HUD was last drawn (stale after 300 ms: HUD off)
static bool PanelCorners(const HudPanelCfg &g, float out[4][4], float *seen, bool shown, float *cOut = nullptr) {
    float hm[16], head[3], c[3], up0[3];
    if (seen) *seen = 1;
    if (!EngineHeadPose(hm, head)) return false;
    const float *o = g.off;
    bool fixed = false;
    float r[3], u[3];
    if (g.anchor == 2 || g.anchor == 3) {  // wrist (2 left, 3 right): offset along the hand's axes (x right, y up, z back)
        float m[16], hp[3];
        if (!HeadtrackHandRaw(g.anchor == 3 ? 1 : 0, m, hp)) return false;  // bow1p: the real hand (not the aim turn)
        for (int i = 0; i < 3; ++i) c[i] = hp[i] + m[i * 4 + 0] * o[0] + m[i * 4 + 1] * o[1] + m[i * 4 + 2] * o[2];
        up0[0] = 0, up0[1] = 1, up0[2] = 0;
        if (g.fixed) {
            // wrist1: the panel's face (n, towards whoever reads it) starts as the hand's back
            // axis, turned about the hand's up by 'turn', then tipped towards its top by 'tilt'.
            const float d2r = 0.01745329f;
            float ca = cosf(g.turn * d2r), sa = sinf(g.turn * d2r), ct = cosf(g.tilt * d2r), st = sinf(g.tilt * d2r);
            float n0[3], n[3];
            for (int i = 0; i < 3; ++i) {
                float R = m[i * 4 + 0], U = m[i * 4 + 1], B = m[i * 4 + 2];
                r[i] = R * ca - B * sa, n0[i] = B * ca + R * sa;
                n[i] = n0[i] * ct + U * st, u[i] = U * ct - n0[i] * st;
            }
            fixed = true;
            if (seen && g.glance) {
                float th[3] = {head[0] - c[0], head[1] - c[1], head[2] - c[2]};
                Norm3(th);
                float lim = cosf((g.glanceDeg + (shown ? 10.f : 0.f)) * d2r);
                float tdeg = g.turnDeg + (shown ? 10.f : 0.f);  // glance2: the wrist turn's own angle
                float limTurn = cosf((tdeg > 95 ? 95 : tdeg) * d2r);
                float facing = Dot3(n, th);                               // the panel turned to you
                float gaze = -(-hm[2] * th[0] - hm[6] * th[1] - hm[10] * th[2]);  // you looking at it
                *seen = facing > limTurn && gaze > lim ? 1.f : 0.f;
            }
        }
    } else if (g.anchor == 0) {  // head: right / up / ahead in the head's own axes
        for (int i = 0; i < 3; ++i) c[i] = head[i] + hm[i * 4 + 0] * o[0] + hm[i * 4 + 1] * o[1] - hm[i * 4 + 2] * o[2];
        up0[0] = hm[1], up0[1] = hm[5], up0[2] = hm[9];
    } else if (float cc[3], cf[3], cr[3]; g.anchor == 4 && HolsterChestFrame(cc, cf, cr)) {
        // with the chest spheres: their centre and body facing (40 deg dead band), so both move together
        for (int i = 0; i < 3; ++i) c[i] = cc[i] + cr[i] * o[0] + cf[i] * o[2];
        c[1] += o[1];
        up0[0] = 0, up0[1] = 1, up0[2] = 0;
    } else {  // body: level, turned with where the head faces (also anchor 4 while the holsters are off)
        float f[3] = {-hm[2], 0, -hm[10]};
        float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) f[0] = 0, f[2] = -1, l = 1;
        f[0] /= l, f[2] /= l;
        float r[3] = {-f[2], 0, f[0]};
        for (int i = 0; i < 3; ++i) c[i] = head[i] + r[i] * o[0] + f[i] * o[2];
        c[1] += o[1];
        up0[0] = 0, up0[1] = 1, up0[2] = 0;
    }
    if (!fixed) {  // Face the head: f from the head to the panel; r = f x up; u = r x f.
        float f[3] = {c[0] - head[0], c[1] - head[1], c[2] - head[2]};
        Norm3(f);
        Cross3(f, up0, r);
        if (Dot3(r, r) < 0.03f) {  // near1: (almost) straight below or above the head: its top edge points where the head faces
            float hf[3] = {-hm[2], 0, -hm[10]};
            if (Dot3(hf, hf) < 1e-4f) hf[0] = 0, hf[2] = -1;
            Norm3(hf);
            up0[0] = hf[0], up0[1] = 0, up0[2] = hf[2], Cross3(f, up0, r);
        }
        Norm3(r);
        Cross3(r, f, u);
    }
    if (cOut) memcpy(cOut, c, 12);
    float w = g.size;
    float h = w * ((g.rect[3] - g.rect[1]) * g_h) / ((g.rect[2] - g.rect[0]) * g_w + 1e-6f);
    if (h > w * 3) h = w * 3;
    EngineView v;
    float tx, ty;
    if (!EngineLastView(&v) || !XrGetHeadsetFov(&tx, &ty)) return false;
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    float nr[3];
    Cross3(v.fwd, v.up, nr);
    for (int k = 0; k < 4; ++k) {  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        float sx = (k & 1) ? 0.5f : -0.5f, sy = (k & 2) ? -0.5f : 0.5f, q[3], p[3];
        for (int i = 0; i < 3; ++i) q[i] = (c[i] + r[i] * sx * w + u[i] * sy * h) / v.scale;
        for (int i = 0; i < 3; ++i) p[i] = v.base[i] + fr[i] * q[0] + wu[i] * q[1] - ff[i] * q[2] - v.pos[i];
        float z = Dot3(p, v.fwd);
        if (z < 0.05f) return false;
        out[k][0] = Dot3(p, nr) / tx, out[k][1] = Dot3(p, v.up) / ty, out[k][2] = 0, out[k][3] = z;
    }
    return true;
}

// Present (hook.cpp), for each eye: draw the HUD panel into this eye's image,
// then get ready for the next eye.
void HudDraw(IDXGISwapChain *sc) {
    int passes = g_pass;
    bool captured = g_captured;
    g_pass = 0, g_captured = false;
    g_lastPasses = passes;
    ++g_eyes, g_passSum += passes;
    if (captured) ++g_eyesCaptured;
    DWORD now = GetTickCount();
    if (now - g_statAt > 10000) {
        if (g_statAt)
            LogV("hud: last 10 s: %ld eye images, %.1f back-buffer passes each, HUD captured in %ld, panel drawn %ld "
                "(on %d, from pass %d)",
                g_eyes, g_eyes ? double(g_passSum) / g_eyes : 0.0, g_eyesCaptured, g_shown, (int)g_on, (int)g_first);
        g_statAt = now, g_eyes = g_eyesCaptured = g_passSum = g_shown = 0;
    }
    if (!g_on || !SetupDevice(sc)) return;
    if (!g_tex) {
        // The texture is made the first time we see the game's view of the
        // back buffer (it gives the format the HUD is drawn in).
        if (!g_lastBbRtv) {
            ID3D11Texture2D *bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) {
                if (g_bb) g_bb->Release();
                g_bb = bb;  // so IsBackBuffer can spot the game's view of it
            }
            return;
        }
        if (!SetupChain(sc, g_lastBbRtv)) return;
        return;
    }
    if (sc != g_sc) {
        ReleaseChain();
        return;
    }
    if (!captured) return;
    if (!g_visible) {
        const float clear[4] = {0, 0, 0, 0};
        g_ctx->ClearRenderTargetView(g_texRtv, clear);
        g_leftKept = false;
        return;
    }

    // Panel corners in this eye's view (metres; x right, y up, z ahead),
    // head-locked: centred ahead of the point between the eyes.
    float tx, ty;
    if (!XrGetHeadsetFov(&tx, &ty)) return;
    int eye = -1;
    float q[4], p[3];
    if (!EngineLastEye(&eye, q, p)) eye = -1;
    float eyeX = eye == 0 ? -XrGetIpd() * 0.5f : eye == 1 ? XrGetIpd() * 0.5f : 0.0f;
    // movie1 (movie.cpp): a story video fills the HUD picture, stretched to the eye picture's nearly square shape.
    // While one plays: the picture goes on a 16:9 screen ([hud] MovieWidth at MovieDistance, at its own distance in
    // both eyes), with black around it instead of the game's scene, and none of the HUD's cut-outs.
    const bool movie = MoviePlaying();
    float qu = 0, qw = 0, qd = 0;
    const bool qfar = g_mainFar && !movie && !EngineScoped() && MarkersQuestPlace(&qu, &qw, &qd);  // farquest1
    float w = movie ? g_movieW : g_size, h = movie ? g_movieW * 9.0f / 16.0f : g_size * g_h / g_w;
    const float hy = movie ? 0.0f : g_height, hz = movie ? g_movieD : g_dist;
    Cb cb = {};
    for (int k = 0; k < 4; ++k) {  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        float x = ((k & 1) ? 0.5f : -0.5f) * w - eyeX, y = ((k & 2) ? -0.5f : 0.5f) * h + hy, z = hz;
        cb.corner[k][0] = x / tx, cb.corner[k][1] = y / ty, cb.corner[k][2] = 0, cb.corner[k][3] = z;
    }
    {   // moviefix1 (DrGonzaga 17 Nov: the opening story screen "moves with the head (unlike other cutscenes)"): the movie
        // screen is placed once in the room (ahead of where you look when it starts, level) and stays there
        static bool s_was;
        static DWORD s_lastT;
        static float s_c[3], s_r[3];
        DWORD nowT = GetTickCount();
        bool fresh = movie && (!s_was || nowT - s_lastT > 500);
        s_was = movie, s_lastT = nowT;
        float hm[16], head[3];
        EngineView v;
        if (movie && EngineHeadPose(hm, head) && EngineLastView(&v)) {
            if (fresh) {
                float f[3] = {-hm[2], 0, -hm[10]}, l = sqrtf(f[0] * f[0] + f[2] * f[2]);
                if (l < 1e-3f) f[0] = 0, f[2] = -1, l = 1;
                f[0] /= l, f[2] /= l;
                for (int i = 0; i < 3; ++i) s_c[i] = head[i] + f[i] * hz;
                s_r[0] = -f[2], s_r[1] = 0, s_r[2] = f[0];
            }
            const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
            float nr[3];
            Cross3(v.fwd, v.up, nr);
            float out[4][4];
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
                float sx = (k & 1) ? 0.5f : -0.5f, sy = (k & 2) ? -0.5f : 0.5f, q[3], p[3];
                for (int i = 0; i < 3; ++i) q[i] = (s_c[i] + s_r[i] * sx * w + (i == 1 ? sy * h : 0)) / v.scale;
                for (int i = 0; i < 3; ++i) p[i] = v.base[i] + fr[i] * q[0] + wu[i] * q[1] - ff[i] * q[2] - v.pos[i];
                float z = Dot3(p, v.fwd);
                if (z < 0.05f) ok = false;
                else out[k][0] = Dot3(p, nr) / tx, out[k][1] = Dot3(p, v.up) / ty, out[k][2] = 0, out[k][3] = z;
            }
            if (ok) memcpy(cb.corner, out, sizeof out);
            else {  // looking away from it: nothing drawn but black
                for (int k = 0; k < 4; ++k) cb.corner[k][0] = cb.corner[k][1] = 0, cb.corner[k][3] = 1;
            }
        }
    }
    cb.opacity[0] = EngineScoped() || movie ? 1.0f : g_opacity;  // scope1: full while scoped (DrGonzaga); movie1: full
    cb.uvr[0] = 0, cb.uvr[1] = 0, cb.uvr[2] = 1, cb.uvr[3] = 1;
    // ammo1: cut-outs off while the game is paused (pause menu, map, inventory).
    // belt1 (DrGonzaga 11 Nov: cut-outs stayed in cutscenes, pause menus, sometimes the map): the same in a
    // cutscene shown in VR, and now everything that takes pieces out of the HUD picture goes together: the
    // own panels, the X-hidden minimap, the hidden box (it used to stay, a hole in menus and the map),
    // the kept box and the hidden grid areas. "paused" below = any of these, the whole HUD on its panel.
    bool paused = GamePaused() || EngineCutscene() || movie;  // movie1: no cut-outs during a story video
    static bool s_paused;
    if (paused != s_paused) s_paused = paused, Log("hud: game %s (HUD cut-outs %s)", paused ? "paused or in a cutscene" : "running", paused ? "off" : "on");
    cb.opacity[1] = paused ? 0.0f : (float)g_hide;
    bool scoped = EngineScoped();  // the scope keeps the plain HUD (and the weapon wheel, DrGonzaga 3 Nov)
    bool own[kHudPanels];
    DWORD now2 = GetTickCount();
    for (int e = 0; e < kHudPanels; ++e) {
        const HudPanelCfg &g = g_el[e];
        bool valid = g.rect[2] > g.rect[0] + 0.01f && g.rect[3] > g.rect[1] + 0.01f;
        // scope2 (DrGonzaga 3 Nov): while scoped the minimap and ammo are left out altogether
        // (cut from the main panel, own panels not drawn).
        bool wheel = InputWheelHeld();
        // lasers1 (DrGonzaga 10 Nov): X hides / brings back the minimap (with health), cut from the main panel too.
        bool hideMap = e == 0 && !g_mapShown && valid && !paused && !wheel;
        bool cut = (valid && g.own && !paused && !wheel) || hideMap;  // wheel2: whole HUD while the weapon wheel is up
        own[e] = cut && !scoped && !hideMap;
        bool outline = valid && g_outlineAt[e] && now2 - g_outlineAt[e] < 300;
        if (cut || outline) memcpy(cb.mask[e], g.rect, 16);
        cb.mflags[e] = cut ? 1.f : 0.f;
        cb.mflags[2 + e] = outline ? 1.f : 0.f;
    }
    {  // hide1: the hidden box, on the main panel and the cut-outs alike
        bool valid = g_hbox.rect[2] > g_hbox.rect[0] + 0.002f && g_hbox.rect[3] > g_hbox.rect[1] + 0.002f;
        bool outline = valid && g_hboxOutlineAt && now2 - g_hboxOutlineAt < 300;
        if (valid && (g_hbox.on || outline)) memcpy(cb.hbox, g_hbox.rect, 16);
        cb.pflags[2] = valid && g_hbox.on && !paused ? 1.f : 0.f;
        cb.pflags[3] = outline ? 1.f : 0.f;
    }
    {  // keep1: part of the minimap box left on the main panel
        bool valid = g_kbox.rect[2] > g_kbox.rect[0] + 0.002f && g_kbox.rect[3] > g_kbox.rect[1] + 0.002f;
        bool outline = valid && g_kboxOutlineAt && now2 - g_kboxOutlineAt < 300;
        if (valid && (g_kbox.on || outline)) memcpy(cb.kbox, g_kbox.rect, 16);
        cb.kflags[0] = valid && g_kbox.on && !paused ? 1.f : 0.f;
        cb.kflags[1] = outline ? 1.f : 0.f;
    }

    ID3D11DeviceContext *c = g_ctx;
    HeadtrackOwnDraw(true);
    D3DState game(c, 1, 1);  // state1: the game's pipeline state, put back by Restore() below

    if (qfar) {  // farquest1: the patch round the quest icon is left out here and drawn far away below
        cb.qbox[0] = qu - g_qHalf[0], cb.qbox[1] = qw - g_qHalf[1], cb.qbox[2] = qu + g_qHalf[0], cb.qbox[3] = qw + g_qHalf[1];
        cb.opacity[3] = 1;
    }
    c->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    EngineEyeViewport(&vp.TopLeftX, &vp.TopLeftY, &vp.Width, &vp.Height);  // oc1
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_rs);
    c->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    c->OMSetBlendState(g_bs, bf, 0xffffffff);
    c->OMSetRenderTargets(1, &g_bbRtv, nullptr);
    if (movie && g_movieBlack) {  // movie1: black round the screen, not the game's scene
        const float black[4] = {0, 0, 0, 1};
        c->ClearRenderTargetView(g_bbRtv, black);
    }
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g_vs, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &g_cb);
    c->PSSetShader(g_ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_cb);
    c->PSSetShaderResources(0, 1, &g_texSrv);
    c->PSSetSamplers(0, 1, &g_smp);
    c->Draw(4, 0);
    if (qfar) {  // farquest1: the quest icon's patch, the same size and place on the panel, with no eye offset
        Cb q = cb;
        q.opacity[3] = 0;
        memcpy(q.uvr, cb.qbox, 16);
        // questdepth1 (DrGonzaga 16 Nov: "perfect would be the quest icon being the same distance as the objective"):
        // the patch is pushed out to its target's distance (same size in view: everything scales with it) with the
        // usual eye offset, so both eyes see it at the objective's depth. Beyond 300 m, or an off-screen arrow at the
        // edge: no eye offset (as far as the stars).
        const float d = qd > hz && qd < 300 ? qd : 0, sc = d > 0 ? d / hz : 1, ex = d > 0 ? eyeX : 0;
        for (int k = 0; k < 4; ++k) {
            float uu = (k & 1) ? cb.qbox[2] : cb.qbox[0], vv = (k & 2) ? cb.qbox[3] : cb.qbox[1];
            float x = (uu - 0.5f) * w * sc - ex, y = ((0.5f - vv) * h + hy) * sc;
            q.corner[k][0] = x / tx, q.corner[k][1] = y / ty, q.corner[k][2] = 0, q.corner[k][3] = hz * sc;
        }
        c->UpdateSubresource(g_cb, 0, nullptr, &q, 0, 0);
        c->Draw(4, 0);
        c->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    }
    // ammokeep1: the ammo part of this eye's HUD picture counted and, when in full, copied.
    bool ammoKept = false;
    if (g_ammoKeep && g_bloodDragon && own[1] && !AmmoShowing() && !GunAttachNoAmmo() /* noammo1: don't keep the grey square */ && AmmoKeepSetup(g_el[1].rect)) {
        AmmoKeepProbe(c, cb, g_el[1].rect);
        c->RSSetViewports(1, &vp);
        c->OMSetRenderTargets(1, &g_bbRtv, nullptr);
        c->PSSetShader(g_ps, nullptr, 0);
        // the copy while the game's own is away or fading (it is the same picture, taken a few frames ago, at full strength);
        // the game's own if it shows without ever counting as full (then the copy would be an old one)
        ammoKept = g_ammoHave && (!g_ammoLive || GetTickCount() - g_ammoFullT < 400);
    }
    // map1 / ammo1: the cut-outs on panels of their own.
    g_panelT = GetTickCount();
    for (int e = 0; e < kHudPanels; ++e) {
        if (!own[e] || (e == 1 && GunAttachNoAmmo())) {  // noammo1: no ammo panel for the Killstar
            g_panelFade[e] = 0, g_panelCOk[e] = false;
            continue;
        }
        Cb m = cb;
        // wrist1: glance fade, by time (this runs once per eye), ~0.15 s each way
        static float s_fade[kHudPanels];
        static LARGE_INTEGER s_at[kHudPanels], s_qf;
        float seen = 1;
        bool ok = PanelCorners(g_el[e], m.corner, &seen, s_fade[e] > 0.5f, g_panelC[e]);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!s_qf.QuadPart) QueryPerformanceFrequency(&s_qf);
        float dt = s_at[e].QuadPart ? float(t.QuadPart - s_at[e].QuadPart) / float(s_qf.QuadPart) : 0.f;
        s_at[e] = t;
        if (dt > 0.1f) dt = 0.1f;
        float step = dt / 0.15f;
        if (!ok) seen = 0;
        s_fade[e] = seen > s_fade[e] ? fminf(seen, s_fade[e] + step) : fmaxf(seen, s_fade[e] - step);
        g_panelFade[e] = ok ? s_fade[e] : 0, g_panelCOk[e] = ok;
        if (!ok || s_fade[e] <= 0.001f) continue;
        if (e == 1 && AmmoShowing()) continue;  // ammo2: the mod's own numbers stand there instead (ammo.cpp)
        memcpy(m.uvr, g_el[e].rect, 16);
        m.opacity[0] = g_el[e].opacity * s_fade[e];
        m.pflags[0] = 1, m.pflags[1] = g_el[e].round ? 1.f : 0.f;
        m.kflags[2] = e == 0 ? 1.f : 0.f;  // keep1
        if (e == 1 && ammoKept) {  // ammokeep1: the kept copy, the whole of its own small texture
            const float *r = g_el[1].rect;
            const float rw = r[2] - r[0], rh = r[3] - r[1];
            m.uvr[0] = 0, m.uvr[1] = 0, m.uvr[2] = 1, m.uvr[3] = 1;
            m.hbox[0] = (cb.hbox[0] - r[0]) / rw, m.hbox[1] = (cb.hbox[1] - r[1]) / rh;  // the hidden box, in the copy's own frame
            m.hbox[2] = (cb.hbox[2] - r[0]) / rw, m.hbox[3] = (cb.hbox[3] - r[1]) / rh;
            c->PSSetShaderResources(0, 1, &g_ammoSrv);
            c->UpdateSubresource(g_cb, 0, nullptr, &m, 0, 0);
            c->Draw(4, 0);
            c->PSSetShaderResources(0, 1, &g_texSrv);
            continue;
        }
        c->UpdateSubresource(g_cb, 0, nullptr, &m, 0, 0);
        c->Draw(4, 0);
    }
    ID3D11ShaderResourceView *none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    // Ready for the next eye's HUD. onehud1: the left eye's picture stays for the right eye.
    if (g_oneHud && StereoEyeNow() == 0) g_leftKept = true;
    else {
        const float clear[4] = {0, 0, 0, 0};
        c->ClearRenderTargetView(g_texRtv, clear);
        g_leftKept = false;
    }

    game.Restore();
    HeadtrackOwnDraw(false);
    ++g_shown;
}

// slowbtn1 (slowmo.cpp): where an own panel sits now (recentred XR frame), whether or not it is being drawn.
bool HudPanelPlace(int e, float c[3]) {
    if (e < 0 || e >= kHudPanels) return false;
    float o[4][4];
    c[0] = 1e9f;
    PanelCorners(g_el[e], o, nullptr, false, c);
    return c[0] < 1e8f;
}
bool *HudOneForBothEyes() { return &g_oneHud; }  // onehud1
bool *HudMainFar() { return &g_mainFar; }        // farquest1
float *HudQuestPatch() { return g_qHalf; }
bool *HudMovieBlack() { return &g_movieBlack; }  // movie1
float *HudMovieSize(int i) { return i ? &g_movieD : &g_movieW; }
bool *HudAmmoKeep() { return &g_ammoKeep; }  // ammokeep1
// 0 off / not possible, 1 waiting for the game to show its ammo once, 2 the game's own is showing, 3 the kept copy is showing
int HudAmmoKeepState() { return !g_ammoKeep || g_ammoFailed || !g_ammoTex ? 0 : g_ammoLive ? 2 : g_ammoHave ? 3 : 1; }
// objhead1 (markers.cpp): the main, head-locked HUD panel: how far ahead, its width and height, how far above centre.
void HudMainPanel(float *dist, float *w, float *h, float *height) {
    *dist = g_dist, *w = g_size, *h = g_size * (g_w ? (float)g_h / (float)g_w : 0.5625f), *height = g_height;
}

// ammo2 (ammo.cpp): an own panel's centre in the recentred XR frame and its glance fade (0 when it is not drawn).
float HudPanelFade(int e) { return e >= 0 && e < kHudPanels && GetTickCount() - g_panelT < 300 ? g_panelFade[e] : 0; }
bool HudPanelCentre(int e, float c[3]) {
    if (e < 0 || e >= kHudPanels || !g_panelCOk[e]) return false;
    memcpy(c, g_panelC[e], 12);
    return true;
}
