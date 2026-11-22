// ammo2 (DrGonzaga 15 Nov, Blood Dragon: "I've got the ammo counter set so I can tilt my gun and glance at
// it, but I need to press B to bring it up. B also reloads the gun. I was hoping to have it just appear
// when I glanced at it").
//
// The ammo panel is a piece cut out of the game's own HUD picture, and the game only draws its ammo there
// for a few seconds after a shot, a reload or a weapon change. So the numbers are read from the game and
// drawn by the mod instead, at the ammo panel's place, whenever that panel would show (its glance, pause,
// scope and weapon wheel rules are unchanged):
// - rounds in the gun: as the script call GetCurrentWeaponCurrentClipAmmo (BD+10bc2185 -> BD+10be8e88):
//   inventory = *(CPawn + 0x24) + 0xbd4, gun entity = *(*(inventory + 0xb4) + 0xc), its weapon component
//   W = BD+104508db (thiscall, entity), rounds = *(*(W + 0x48) + 0xcc).
// - spare rounds: the gun's properties P = *(*(W + 0x48) + 0x64); P + 0x18c = it draws on carried ammo,
//   P + 0x184 = that ammo's id (BD+1051bed8 reads both). The carried item = BD+1051d017 (inventory, &id),
//   its count *(*(item + 0x3c) + 0x60), as the throwables are counted in holster.cpp. Read from the code,
//   not yet checked in play: the log line "ammo: ..." at each weapon change shows what was read.
// With no gun that has rounds (fists, a throwable, the bow not found) nothing is read and the game's own
// cut-out shows as before.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
bool g_own = false;   // [hud] AmmoOwnNumbers (bd80: off by default; the game's own picture is kept instead, hud.cpp ammokeep1)
bool g_spare = true;  // [hud] AmmoOwnSpare
float g_size = 1;     // [hud] AmmoOwnSize
volatile LONG g_clip = -1, g_res = -1;
volatile DWORD g_readT;

float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32], d[32];
    IniPath(path);
    swprintf(d, 32, L"%.3f", def);
    GetPrivateProfileStringW(L"hud", key, d, buf, 32, path);
    return (float)_wtof(buf);
}
void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"hud", key, buf, path);
}
bool Bad(const void *p, size_t n) { return !p || IsBadReadPtr(p, n); }
}  // namespace

void AmmoLoad() {
    g_own = LoadF(L"AmmoOwnNumbers", 0) != 0;
    g_spare = LoadF(L"AmmoOwnSpare", 1) != 0;
    float s = LoadF(L"AmmoOwnSize", 1);
    g_size = s < 0.3f ? 0.3f : s > 3.f ? 3.f : s;
}
void AmmoSave() {
    SaveF(L"AmmoOwnNumbers", g_own ? 1.f : 0.f);
    SaveF(L"AmmoOwnSpare", g_spare ? 1.f : 0.f);
    SaveF(L"AmmoOwnSize", g_size);
}
bool *AmmoOwn() { return &g_own; }
bool *AmmoSpare() { return &g_spare; }
float *AmmoSize() { return &g_size; }
int AmmoClipNow() { return GetTickCount() - g_readT < 500 ? (int)g_clip : -1; }
int AmmoSpareNow() { return GetTickCount() - g_readT < 500 ? (int)g_res : -1; }

// hud.cpp: the mod's numbers stand in for the game's ammo cut-out right now.
bool AmmoShowing() { return g_bloodDragon && g_own && AmmoClipNow() >= 0; }

// Game thread, once a frame, with the player's CPawn component (holster.cpp HolsterGameTick).
void AmmoGameTick(BYTE *pawn) {
    if (!g_bloodDragon || !g_own) return;
    static BYTE *s_mod;
    static BYTE *s_lastW;
    static DWORD s_resT;
    static int s_logs;
    if (!s_mod) s_mod = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    LONG clip = -1, res = -1;
    DWORD now = GetTickCount();
    BYTE *W = nullptr, *inv = nullptr, *P = nullptr;
    do {
        if (!s_mod || Bad(pawn + 0x24, 4) || !pawn) break;
        BYTE *owner = *(BYTE **)(pawn + 0x24);
        if (Bad(owner + 0xbd4, 0x100)) break;
        inv = owner + 0xbd4;
        BYTE *h = *(BYTE **)(inv + 0xb4);
        if (Bad(h + 0xc, 4)) break;
        BYTE *ent = *(BYTE **)(h + 0xc);
        if (Bad(ent, 0x40)) break;
        W = (BYTE *)BdThisCall(s_mod + 0x4508db, ent, 0);
        if (Bad(W + 0x48, 4)) { W = nullptr; break; }
        BYTE *c = *(BYTE **)(W + 0x48);
        if (Bad(c + 0x64, 0x70)) break;
        int v = *(int *)(c + 0xcc);
        if (v < 0 || v > 9999) break;
        clip = v;
        P = *(BYTE **)(c + 0x64);
        if (Bad(P + 0x184, 12)) P = nullptr;
    } while (0);
    if (clip >= 0 && P && *(BYTE *)(P + 0x18c)) {
        res = g_res;
        if (W != s_lastW || now - s_resT > 200) {  // the walk over the carried items, five times a second
            s_resT = now;
            res = -1;
            BYTE *item = (BYTE *)BdThisCall(s_mod + 0x51d017, inv, 1, P + 0x184);
            if (!Bad(item, 0x40)) {
                BYTE *x = *(BYTE **)(item + 0x3c);
                if (!Bad(x + 0x60, 4)) {
                    int v = *(int *)(x + 0x60);
                    if (v >= 0 && v <= 9999) res = v;
                }
            }
        }
    }
    if (W != s_lastW) {
        s_lastW = W;
        if (s_logs < 60) {
            ++s_logs;
            if (clip >= 0)
                Log("ammo: %s: %ld in the gun, %ld spare (ammo id %08lx, %s)", StockGunName()[0] ? StockGunName() : "weapon", clip, res,
                    P ? (unsigned long)*(DWORD *)(P + 0x184) : 0ul, P && *(BYTE *)(P + 0x18c) ? "draws on carried ammo" : "no carried ammo");
            else Log("ammo: nothing with rounds in hand; the game's own ammo picture shows");
        }
    }
    InterlockedExchange(&g_res, res);
    InterlockedExchange(&g_clip, clip);
    g_readT = now;
}

// Render thread, each eye, after the wrist marks (hook.cpp).
void AmmoDraw(IDXGISwapChain *sc) {
    if (!AmmoShowing() || CyberEyeOn()) return;
    float fade = HudPanelFade(1), c[3], hm[16], head[3];
    if (fade <= 0.001f || !HudPanelCentre(1, c) || !EngineHeadPose(hm, head)) return;
    EngineView v;
    if (!EngineLastView(&v) || !v.sixdof) return;
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    auto world = [&](const float *p, float *w) {
        float o[3] = {p[0] / v.scale, p[1] / v.scale, p[2] / v.scale};
        for (int i = 0; i < 3; ++i) w[i] = v.base[i] + fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2];
    };
    const float U[3] = {hm[1], hm[5], hm[9]};  // the head's up: the spare count sits under the rounds as you see them
    const HudPanelCfg *g = HudPanel(1);
    float rc = g->size * 0.22f * g_size;  // half the width of the mark; its digits are that tall
    LaserMark m[2];
    int n = 0;
    int clip = AmmoClipNow(), res = g_spare ? AmmoSpareNow() : -1;
    auto number = [&](int val, float up, float r, const float *rgb, float op) {  // one mark, up to three digits (ammo2 in laser.cpp)
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = c[k] + U[k] * up;
        LaserMark &k = m[n++];
        world(p, k.pos);
        k.radius = r / v.scale, k.icon = 8, k.val = (float)(val > 999 ? 999 : val);
        k.rgba[0] = rgb[0], k.rgba[1] = rgb[1], k.rgba[2] = rgb[2], k.rgba[3] = op;
    };
    const float cyan[3] = {0.35f, 1.0f, 1.0f}, red[3] = {1.0f, 0.3f, 0.25f}, amber[3] = {1.0f, 0.8f, 0.4f};
    number(clip, res >= 0 ? rc * 0.5f : 0, rc, clip == 0 ? red : cyan, fade);
    if (res >= 0) number(res, -rc * 0.85f, rc * 0.6f, amber, fade * 0.85f);
    LaserDrawMarks(sc, m, n);
}
