// mark1 (DrGonzaga 3 Nov): hide the yellow quest objective icon (with the
// off-screen arrow) and the square map-marker icon, each on its own switch.
// RE (Ghidra): both are Scaleform clips moved every frame by their indicator's
// update (vtable slot after the shared base slot 0x10047c30):
//  - CFCXObjectiveIndicator (vtable 0x12036ff8, clips "ObjectiveIndicator_%u",
//    "ObjectiveArrow" frame) update 0x10f72860 sets the clip position through
//    0x11642e80 (x double on the stack, y in xmm0) at call 0x10f72a96.
//  - CFCXPlayerMarkerIndicator (vtable 0x1204708c, clip "PlayerMarkerIndicator")
//    update 0x11a87c60 calls the clip SetPosition 0x10a22ad0(out, x, y) at
//    0x11a87d20.
// We point those two calls at stubs that, while the switch is on, move the clip
// far off the screen. Every frame the game sends the real position again, so
// turning the switch off brings the icon straight back. Cost: one compare per
// icon per frame. Map, minimap and other HUD parts are untouched.
#include <windows.h>

#include "fc3vr.h"
#include <math.h>
#include <string.h>

static const DWORD kImageBase = 0x10000000;

extern "C" {
volatile BYTE g_hideObjIcon = 1, g_hideMarkIcon = 1;
void *g_objSetPos, *g_plSetPos;
double g_iconFar = -20000.0;
void MarkPlStub();
volatile BYTE g_mapHead = 1;  // [hud] MinimapFollowsHead (headmap1, below)
const float *MapYawPick(const float *m);
}

// Player marker: [esp+4] = out, [esp+8] = x, [esp+0x10] = y (doubles).
__asm__(
    ".text\n"
    ".globl _MarkPlStub\n"
    "_MarkPlStub:\n"
    "  cmpb $0, _g_hideMarkIcon\n"
    "  je 2f\n"
    "  movl _g_iconFar, %eax\n"
    "  movl %eax, 8(%esp)\n"
    "  movl %eax, 16(%esp)\n"
    "  movl _g_iconFar+4, %eax\n"
    "  movl %eax, 12(%esp)\n"
    "  movl %eax, 20(%esp)\n"
    "2:\n"
    "  jmp *_g_plSetPos\n");

// bd20: Blood Dragon. Both icons set their clip's place through BD+10396665 (thiscall: out, x, y doubles on the
// stack, ret 0x14), so both use the player-marker layout: [esp+4] = out, [esp+8] = x, [esp+0x10] = y.
extern "C" void MarkObjStubBd();
__asm__(
    ".text\n"
    ".globl _MarkObjStubBd\n"
    "_MarkObjStubBd:\n"
    "  cmpb $0, _g_hideObjIcon\n"
    "  je 4f\n"
    "  movl _g_iconFar, %eax\n"
    "  movl %eax, 8(%esp)\n"
    "  movl %eax, 16(%esp)\n"
    "  movl _g_iconFar+4, %eax\n"
    "  movl %eax, 12(%esp)\n"
    "  movl %eax, 20(%esp)\n"
    "4:\n"
    "  jmp *_g_objSetPos\n");

static bool PatchCall(BYTE *fc3, DWORD call, DWORD target, void *stub, void **orig, const char *what) {
    BYTE *c = BdAt(fc3, call);
    BYTE *t = BdAt(fc3, target);
    if (IsBadReadPtr(c, 5) || c[0] != 0xe8 || c + 5 + *(int *)(c + 1) != t) {
        Log("markers: %s call doesn't match; switch off", what);
        return false;
    }
    *orig = t;  // (set first: the stub may run the moment the call is retargeted)
    return PatchCallTo(c, stub);
}

// headmap1 (DrGonzaga 10 Nov): the minimap turns with the head, not the aim.
// RE (FC3, Ghidra): the minimap's heading comes from the radar HUD update, which turns the player's
// world matrix (entity +0x20, rows = axes) into a quaternion, takes its yaw and from that sets the
// map texture's turn and the Flash clips' turn. On foot the player faces the game's look, which
// follows the right controller, so the map spun as you pointed. The hook (BdMapQuat, bd20) hands
// that call a copy of the matrix turned about the world up by (head yaw - game look yaw), so the
// map faces where the head looks. Game thread, once per frame, a few multiplies.

static float g_mapM[16];
static float g_mapDelta;     // last good turn (kept while the head looks straight up/down)
static DWORD g_mapDeltaTick;
const float *MapYawPick(const float *m) {
    if (!HeadtrackActive() || EngineFlat()) return m;  // flat1
    EngineView v;
    float gy, gf[2], gp;
    if (!EngineLastView(&v) || !EngineGameLook(&gy, gf, &gp)) return m;
    float h = v.fwd[0] * v.fwd[0] + v.fwd[1] * v.fwd[1], g = gf[0] * gf[0] + gf[1] * gf[1];
    DWORD now = GetTickCount();
    if (h > 0.04f && g > 0.04f) {
        g_mapDelta = atan2f(v.fwd[1], v.fwd[0]) - atan2f(gf[1], gf[0]);
        g_mapDeltaTick = now;
    } else if (now - g_mapDeltaTick > 1000) return m;
    float c = cosf(g_mapDelta), s = sinf(g_mapDelta);
    for (int r = 0; r < 4; ++r) {
        const float *a = m + r * 4;
        float *o = g_mapM + r * 4;
        if (r < 3) o[0] = c * a[0] - s * a[1], o[1] = s * a[0] + c * a[1];
        else o[0] = a[0], o[1] = a[1];
        o[2] = a[2], o[3] = a[3];
    }
    return g_mapM;
}

// bd20: Blood Dragon's radar update (BD+109efd43) gets the heading from BD+1029c8ad (thiscall: entity, out; ret 4),
// which turns the entity's matrix (+0x20) into a quaternion with BD+101a63ef (cdecl: out, matrix). The call at
// BD+109efe24 comes here instead.
static BYTE *g_bdMod;
static void *__attribute__((thiscall)) BdMapQuat(BYTE *ent, void *out) {
    const float *m = (const float *)(ent + 0x20);
    if (g_mapHead) m = MapYawPick(m);
    BdCdeclCall(BdAt(g_bdMod, 0x1a63ef), 2, out, m);
    return out;
}

// objhead1 (bd80; DrGonzaga 15 Nov: the objective marker "doesn't appear relative to your head, it appears relative
// to where you are pointing the controller", and is tiny). The quest indicator's update (BD+1082c00a) gets the
// icon's place from BD+1098c204 (thiscall, 12 arguments, ret 0x30; called at BD+1082c0f5): world position ->
// place on the screen through the game camera, which on foot follows the right controller. Arguments used here:
// 1 the world position (3 floats; the indicator's +0x2c is added to its height), 6 out: x, y (ints, in the
// Flash stage's units, 1280 x 720 unless the clip says otherwise), 7 out: the arrow's turn (radians, clockwise
// from up), 8 out: x, y as fractions of the screen. It returns 1 when the place is inside an ellipse a third of
// the screen out from the centre (plain icon), else 0 with the place on that ellipse (arrow).
// The HUD is a head-locked panel in VR, so after the game's own call the place is worked out again from the
// head: where the line from the head to the objective crosses that panel. Same ellipse rule, same outputs.
// Size: the clip's scale is set through BD+1039675b (thiscall clip: out, x scale, y scale as doubles, 1 = 100%;
// property 0x1b in BD+10eaef00), the clip at indicator +0x460 as the position call uses it.
bool g_objHead = true;
float g_objScale = 4;
static void *g_objProj;
static volatile float g_qu, g_qw;  // farquest1: where the quest icon was last put (HUD picture fractions)
static volatile float g_qD;        // questdepth1: how far its target is (m), 0 = off screen (an arrow at the edge)
static volatile DWORD g_qT;
bool MarkersQuestPlace(float *u, float *w, float *dist) {
    if (!g_qT || GetTickCount() - g_qT > 300) return false;
    *u = g_qu, *w = g_qw, *dist = g_qD;
    return true;
}
static BYTE __attribute__((thiscall)) BdObjProject(BYTE *self, const float *world, void *clip, DWORD a3, DWORD a4, DWORD mode, int *xy,
                                                   float *angle, float *norm, float *o9, DWORD margin, DWORD ox, DWORD oy) {
    BYTE r = (BYTE)BdThisCall(g_objProj, self, 12, world, clip, a3, a4, mode, xy, angle, norm, o9, margin, ox, oy);
    static float s_w = 1280, s_h = 720;
    static int s_logs;
    if (!xy || !norm || !angle || !world) return r;
    if (norm[0] > 0.1f && norm[1] > 0.1f && xy[0] > 0 && xy[1] > 0) {  // the stage size, from the game's own answer
        float w = xy[0] / norm[0], h = xy[1] / norm[1];
        if (w > 300 && w < 8000 && h > 200 && h < 5000) s_w = roundf(w / 8) * 8, s_h = roundf(h / 8) * 8;
    }
    DWORD now = GetTickCount();
    {
        static BYTE *s_clip;
        static float s_sc = 1;
        static DWORD s_t;
        BYTE *c = *(BYTE **)(self + 0x460);
        // set again four times a second in case the game sets the scale itself; a clip never scaled is left alone at x1
        const bool need = g_objScale != 1 ? c != s_clip || s_sc != g_objScale || now - s_t > 250 : c == s_clip && s_sc != 1;
        if (c && need) {
            double s = g_objScale;
            DWORD w[4];
            BYTE out[32];
            memcpy(w, &s, 8), memcpy(w + 2, &s, 8);
            BdThisCall(BdAt(g_bdMod, 0x39675b), c, 5, out, w[0], w[1], w[2], w[3]);
            s_clip = c, s_sc = g_objScale, s_t = now;
        }
    }
    EngineView v;
    if (!g_objHead || !HeadtrackActive() || EngineFlat() || CyberEyeOn() || !EngineLastView(&v)) return r;
    float d[3] = {world[0] - v.pos[0], world[1] - v.pos[1], world[2] + *(float *)(self + 0x2c) - v.pos[2]};
    float rt[3] = {v.fwd[1] * v.up[2] - v.fwd[2] * v.up[1], v.fwd[2] * v.up[0] - v.fwd[0] * v.up[2], v.fwd[0] * v.up[1] - v.fwd[1] * v.up[0]};
    float x = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2], y = d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2];
    float z = d[0] * v.fwd[0] + d[1] * v.fwd[1] + d[2] * v.fwd[2];
    float pd, pw, ph, pup;
    HudMainPanel(&pd, &pw, &ph, &pup);
    if (pw < 0.05f || ph < 0.05f) return r;
    const float R = 0.335f;  // the game's own ellipse: a third of the screen out from the centre
    float u = 0.5f, w = 0.5f, a = 3.14159265f;
    bool on = false;
    if (z > 1e-3f) {
        u = 0.5f + x / z * pd / pw, w = 0.5f - (y / z * pd - pup) / ph;
        float eu = (u - 0.5f) / R, ew = (w - 0.5f) / R;
        on = eu * eu + ew * ew <= 1;
        if (!on) a = atan2f(u - 0.5f, -(w - 0.5f));
    } else a = atan2f(x, y);  // behind: towards the side it is on
    if (!on) u = 0.5f + sinf(a) * R, w = 0.5f - cosf(a) * R;
    if (s_logs < 6 && ++s_logs)
        Log("markers: quest icon by the head: the game said %d,%d (%s), now %d,%d (%s), stage %.0f x %.0f, size x%.1f", xy[0], xy[1], r ? "in view" : "arrow",
            (int)(s_w * u), (int)(s_h * w), on ? "in view" : "arrow", s_w, s_h, g_objScale);
    xy[0] = (int)(s_w * u), xy[1] = (int)(s_h * w);
    norm[0] = u, norm[1] = w;
    g_qu = u, g_qw = w, g_qD = on && z > 0 ? sqrtf(x * x + y * y + z * z) : 0, g_qT = GetTickCount();  // farquest1 / questdepth1
    *angle = a;
    return on ? 1 : 0;
}

void MarkersInstallBd(BYTE *bd) {
    g_bdMod = bd;
    const DWORD b0 = (DWORD)bd - kImageBase;  // PatchCall takes addresses as the listing shows them
    BYTE *fc3 = (BYTE *)(b0 + kImageBase);
    bool a = PatchCall(fc3, 0x1082c235, 0x10396665, (void *)MarkObjStubBd, &g_objSetPos, "quest icon");
    bool b = PatchCall(fc3, 0x109c2e0d, 0x10396665, (void *)MarkPlStub, &g_plSetPos, "map marker icon");
    void *dummy;
    static const BYTE kQ[6] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83};
    bool c = !memcmp(BdAt(bd, 0x1a63ef), kQ, 6) && PatchCall(fc3, 0x109efe24, 0x1029c8ad, (void *)BdMapQuat, &dummy, "minimap heading");
    Log("markers: minimap heading %s (%s)", c ? "hook in" : "NOT hooked", g_mapHead ? "follows head" : "game's");
    Log("markers: quest icon %s (%s), map marker icon %s (%s)", a ? "hook in" : "NOT hooked",
        g_hideObjIcon ? "hidden" : "shown", b ? "hook in" : "NOT hooked", g_hideMarkIcon ? "hidden" : "shown");
    {  // objhead1
        static const BYTE kProj[8] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0}, kScale[8] = {0x55, 0x8b, 0xec, 0xf2, 0x0f, 0x10, 0x45, 0x14};
        BYTE *sc = BdAt(bd, 0x39675b);
        bool ok = !memcmp(BdAt(bd, 0x98c204), kProj, 8) && !memcmp(sc, kScale, 8) && sc[0x21] == 0xe8 && sc + 0x26 + *(int *)(sc + 0x22) == BdAt(bd, 0xeaef00) &&
                  PatchCall(fc3, 0x1082c0f5, 0x1098c204, (void *)BdObjProject, &g_objProj, "quest icon place");
        Log("markers: quest icon by the head %s (%s, size x%.1f)", ok ? "hook in" : "NOT hooked", g_objHead ? "on" : "off", g_objScale);
    }
}

// ammo1: the game's script call IsGamePaused reads the byte at +0x58 of a global object (pause
// menu, map, inventory...). bd20: that object is at BD+11846744 (read by BD+1005df44).
bool GamePaused() {
    static BYTE *s_bd;
    if (!s_bd) s_bd = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    BYTE *o = s_bd ? *(BYTE **)(BdAt(s_bd, 0x1846744)) : nullptr;
    return o && o[0x58] != 0;
}
