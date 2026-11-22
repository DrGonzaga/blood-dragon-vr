// state1 (11 Nov, review D2): one copy of "remember the game's D3D state,
// draw our own thing, put the state back".
//
// The beam, the HUD panel, the scope lens, the menu panel and the SMAA pass
// each carried their own 80 to 120 lines of this. They all saved the same
// things; the only differences were how many pixel-shader textures and
// samplers, and whether the geometry/hull/domain shaders too (SMAA only).
// The mod's hooks on the context are not touched by this: saving and putting
// back is done with the plain Get/Set calls, as before (never a device
// context state swap, which hid the game's constant buffer writes).
//
// state2: the overlays that go into one eye's image at Present (beam, holster
// marks, HUD, scope lens, menu) each set every piece of state they use, so the
// game's state only has to be saved before the first of them and put back
// after the last: once per eye instead of up to five times.
#include <windows.h>
#include <d3d11.h>
#include <new>

#include "fc3vr.h"

bool g_shareState = true;

namespace {
bool s_sharing;          // between ShareBegin and ShareEnd (render thread only)
D3DState *s_shared;      // the one saved copy, once an overlay has drawn
alignas(D3DState) char s_buf[sizeof(D3DState)];
}  // namespace

void D3DState::ShareBegin() { s_sharing = g_shareState; }

void D3DState::ShareEnd() {
    s_sharing = false;
    if (!s_shared) return;
    HeadtrackOwnDraw(true);  // (putting the game's targets back must not count as the game binding them: hud.cpp)
    s_shared->Restore();
    HeadtrackOwnDraw(false);
    s_shared = nullptr;
}

D3DState::D3DState(ID3D11DeviceContext *ctx, UINT srvs, UINT samplers, bool allShaders)
    : c(ctx), nSrv(srvs > 8 ? 8 : srvs), nSmp(samplers > 4 ? 4 : samplers), all(allShaders), live(true) {
    if (s_sharing && srvs <= 1 && samplers <= 1 && !allShaders) {
        if (!s_shared) {
            s_shared = new (s_buf) D3DState();
            s_shared->c = ctx, s_shared->nSrv = 1, s_shared->nSmp = 1, s_shared->all = false, s_shared->live = true;
            s_shared->Save();
        }
        live = false;  // this one does nothing; ShareEnd puts the state back
        return;
    }
    Save();
}

void D3DState::Save() {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&nvp, vp);
    c->RSGetState(&rs);
    c->OMGetDepthStencilState(&ds, &ref);
    c->OMGetBlendState(&bs, bf, &mask);
    c->IAGetInputLayout(&il);
    c->IAGetPrimitiveTopology(&top);
    nVsCi = nPsCi = 256;
    c->VSGetShader(&vs, vsCi, &nVsCi);
    c->PSGetShader(&ps, psCi, &nPsCi);
    if (all) {
        nGsCi = nHsCi = nDsCi = 256;
        c->GSGetShader(&gs, gsCi, &nGsCi);
        c->HSGetShader(&hs, hsCi, &nHsCi);
        c->DSGetShader(&dsh, dsCi, &nDsCi);
    }
    c->VSGetConstantBuffers(0, 1, &vcb);
    c->PSGetConstantBuffers(0, 1, &pcb);
    if (nSrv) c->PSGetShaderResources(0, nSrv, srv);
    if (nSmp) c->PSGetSamplers(0, nSmp, smp);
}

template <class T> static void Rel(T *&p) {
    if (p) p->Release(), p = nullptr;
}

void D3DState::Restore() {
    if (!live) return;
    live = false;
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
    c->RSSetViewports(nvp, vp);
    c->RSSetState(rs);
    c->OMSetDepthStencilState(ds, ref);
    c->OMSetBlendState(bs, bf, mask);
    c->IASetInputLayout(il);
    c->IASetPrimitiveTopology(top);
    c->VSSetShader(vs, vsCi, nVsCi);
    c->PSSetShader(ps, psCi, nPsCi);
    if (all) {
        c->GSSetShader(gs, gsCi, nGsCi);
        c->HSSetShader(hs, hsCi, nHsCi);
        c->DSSetShader(dsh, dsCi, nDsCi);
    }
    c->VSSetConstantBuffers(0, 1, &vcb);
    c->PSSetConstantBuffers(0, 1, &pcb);
    if (nSrv) c->PSSetShaderResources(0, nSrv, srv);
    if (nSmp) c->PSSetSamplers(0, nSmp, smp);
    for (auto *&x : rtv) Rel(x);
    Rel(dsv), Rel(rs), Rel(ds), Rel(bs), Rel(il), Rel(vs), Rel(ps), Rel(vcb), Rel(pcb);
    for (UINT i = 0; i < nVsCi; ++i) Rel(vsCi[i]);
    for (UINT i = 0; i < nPsCi; ++i) Rel(psCi[i]);
    if (all) {
        Rel(gs), Rel(hs), Rel(dsh);
        for (UINT i = 0; i < nGsCi; ++i) Rel(gsCi[i]);
        for (UINT i = 0; i < nHsCi; ++i) Rel(hsCi[i]);
        for (UINT i = 0; i < nDsCi; ++i) Rel(dsCi[i]);
    }
    for (auto *&x : srv) Rel(x);
    for (auto *&x : smp) Rel(x);
}
