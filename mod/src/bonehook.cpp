// Motion aiming through the look bone (bonetest1, motion1): the on-foot camera takes its turn from
// bone slot 0 of the player's animation component, and the player's shots read the same look. The
// camera's read teaches us the bone's axis (Learn); the shot reads are turned along the right
// controller (AimShot). Also: hip-fire spread scaling, shots starting at the beam (ShotFix) and the
// cover peek switch. Blood Dragon addresses and how they were found: see bd4 below.
#include <windows.h>
#include <math.h>
#include <string.h>

#include "fc3vr.h"

namespace {

const DWORD kImageBase = 0x10000000;
BYTE *g_fc3;
void *volatile g_playerAnim;

volatile LONG g_calls;

// bonetest1 (30 Oct): turning bone slot 0 for the shot readers only (seen only while firing) moved
// the bullet impacts, not the gun. So motion aiming (motion1, on by default) points that bone along
// the right controller for those readers only: the camera, the gun model and everything else keep
// the game's own look.
volatile bool g_aim = true;

void Rot(const float q[4], const float v[3], float o[3]) {  // o = q v q* (x y z w)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty);
    o[1] = v[1] + w * ty + (z * tx - x * tz);
    o[2] = v[2] + w * tz + (x * ty - y * tx);
}

// Which of the bone's axes is the look forward, and whether the engine turns
// vectors by q or by its conjugate: learnt from the camera, whose forward we
// know (it is made from this bone, plus a little shake).
float g_axis[3];
bool g_conj, g_learnt;
float g_learnErr = 9;
LONG g_aimed, g_aimMiss;

void Learn(const float *q) {
    float r[3], u[3], f[3], cf[3];
    if (!EngineAimBasis(r, u, f, cf)) return;
    float l = sqrtf(cf[0] * cf[0] + cf[1] * cf[1] + cf[2] * cf[2]);
    if (l < 0.5f) return;
    for (int i = 0; i < 3; ++i) cf[i] /= l;
    static const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    float best = 9, qc[4] = {-q[0], -q[1], -q[2], q[3]};
    int bi = 0;
    bool bc = false;
    // shotwatch1: once learnt, the axis is kept while it still fits. Facing along a world axis with a level look,
    // q and its conjugate (and other axis pairs) fit equally well, and picking the best of 12 afresh each frame
    // could swap to a twin for a frame; a shot aimed in that frame is turned the wrong way round.
    if (g_learnt) {
        float o[3];
        Rot(g_conj ? qc : q, g_axis, o);
        float e = (o[0] - cf[0]) * (o[0] - cf[0]) + (o[1] - cf[1]) * (o[1] - cf[1]) + (o[2] - cf[2]) * (o[2] - cf[2]);
        if (e < 0.01f) {
            g_learnErr = e;
            return;
        }
    }
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 6; ++i) {
            float o[3];
            Rot(c ? qc : q, axes[i], o);
            float e = (o[0] - cf[0]) * (o[0] - cf[0]) + (o[1] - cf[1]) * (o[1] - cf[1]) + (o[2] - cf[2]) * (o[2] - cf[2]);
            if (e < best) best = e, bi = i, bc = c;
        }
    g_learnErr = best;
    if (best < 0.01f) {
        static int s_n;
        bool changed = g_learnt && (g_conj != bc || memcmp(g_axis, axes[bi], 12));
        if ((changed || !g_learnt) && s_n < 40)
            ++s_n, Log("bonehook: look axis %s: %s(%.0f %.0f %.0f), fit %.4f", changed ? "CHANGED" : "learnt", bc ? "conj " : "", axes[bi][0], axes[bi][1], axes[bi][2], best);
        memcpy(g_axis, axes[bi], 12), g_conj = bc, g_learnt = true;
    }
}

// shotwatch1 (DrGonzaga 15 Nov: the Fazertron once shot away from the beam; not seen again, its per-shot log lines
// were removed in bd126): which thread's shot was really pointed along the hand (g_shotAimed). A shot that was not
// pointed along the hand does not get the game's own direction added to the beam as "spread" (that sum is the
// beam plus however far the game's look trails the hand).
volatile DWORD g_shotAimed;
bool AimShot(float *q) {
    float r[3], u[3], f[3], cf[3], m[16];
    g_shotAimed = 0;
    if (!g_learnt || !EngineAimBasis(r, u, f, cf) || !HeadtrackHand(1, m)) {
        ++g_aimMiss;
        return false;
    }
    // Controller forward (-z) in the game world, on the same axes the view uses.
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = -(r[i] * m[0 * 4 + 2] + u[i] * m[1 * 4 + 2] - f[i] * m[2 * 4 + 2]);
    if (BowArrowsAlongAim()) {  // quiver5: the bow's zeroing
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1e-4f) {
            for (int i = 0; i < 3; ++i) d[i] /= l;
            BowZeroWorld(d);
        }
    }
    // The bone's current forward in the world.
    float qq[4] = {q[0], q[1], q[2], q[3]};
    if (g_conj) qq[0] = -qq[0], qq[1] = -qq[1], qq[2] = -qq[2];
    float g[3];
    Rot(qq, g_axis, g);
    // Shortest turn from g to d, applied in the world: qq' = t qq.
    float t[4] = {g[1] * d[2] - g[2] * d[1], g[2] * d[0] - g[0] * d[2], g[0] * d[1] - g[1] * d[0],
                  1 + g[0] * d[0] + g[1] * d[1] + g[2] * d[2]};
    float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[3] * t[3]);
    if (tl < 1e-4f) return false;  // exactly backwards: leave it
    for (int i = 0; i < 4; ++i) t[i] /= tl;
    float n[4] = {t[3] * qq[0] + t[0] * qq[3] + t[1] * qq[2] - t[2] * qq[1],
                  t[3] * qq[1] - t[0] * qq[2] + t[1] * qq[3] + t[2] * qq[0],
                  t[3] * qq[2] + t[0] * qq[1] - t[1] * qq[0] + t[2] * qq[3],
                  t[3] * qq[3] - t[0] * qq[0] - t[1] * qq[1] - t[2] * qq[2]};
    if (g_conj) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    memcpy(q, n, 16);
    ++g_aimed;
    g_shotAimed = GetCurrentThreadId();
    return true;
}

// Hip-fire spread (spread1): the player's shot direction gets a random cone from the weapon's
// current spread (Blood Dragon: a call, kBdSpreadCall below). We scale that spread for the
// player's shots when aim-down-sights (left trigger) isn't held, so hip fire can be as tight as
// ADS without its slow walk. NPC shots go through the same code but are never marked as the
// player's, so they keep theirs.
volatile DWORD g_playerShot;
float g_hipSpread = 0.3f;
LONG g_spreadScaled;

extern "C" float __cdecl SpreadScale(float s) {
    if (g_playerShot != GetCurrentThreadId()) return s;
    g_playerShot = 0;
    VrPad p;
    bool ads = XrGetPad(&p) && p.lt > 0.5f;
    if (g_stockOn || *TwoHandOn()) return StockSpread(s, ads);  // stock1; twosmooth1: two-handed mode uses it too
    if (ads) return s;  // aiming down sights: the game's own
    ++g_spreadScaled;
    return s * g_hipSpread;
}

// shot1 (31 Oct): the player's shots start at the game's eye bone, not where the gun is drawn: the
// hand, the camera offset and 6DoF head movement all put the beam somewhere else, so hits landed
// off it by that much. The two shot functions (cdecl: shooter, ?, origin out, direction out, ...)
// are wrapped: for the player's shots on foot, the origin becomes the beam's start and the
// direction the beam's, keeping the spread the game added (its direction minus the controller's
// forward the look bone was given).
typedef void(__cdecl *ShotFn)(int, int, float *, float *, int, int);
ShotFn g_shotOrig, g_shotOrig2;
volatile DWORD g_shotMark;
volatile bool g_shotFromHand = true;
LONG g_shots, g_shotsMoved;
float g_shotMoveSum, g_shotMoveMax, g_shotTurnSum;

void ShotFix(float *origin, float *dir);
extern "C" void __cdecl ShotHook(int a, int b, float *origin, float *dir, int e, int f) {
    g_shotMark = 0;
    g_shotOrig(a, b, origin, dir, e, f);
    ShotFix(origin, dir);
}
extern "C" void __cdecl ShotHook2(int a, int b, float *origin, float *dir, int e, int f) {
    g_shotMark = 0;
    g_shotOrig2(a, b, origin, dir, e, f);
    ShotFix(origin, dir);
}
static float Deg(const float *a, const float *b) {
    float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-5f || lb < 1e-5f) return -1;
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    return acosf(c > 1 ? 1 : c < -1 ? -1 : c) * 57.2958f;
}
void ShotFix(float *origin, float *dir) {
    if (g_shotMark != GetCurrentThreadId()) return;
    g_shotMark = 0;
    const bool aimed = g_shotAimed == GetCurrentThreadId();
    g_shotAimed = 0;
    InterlockedIncrement(&g_shots);
    InputNoteShot();  // firelag1
    if (!EngineScopePictureActive()) GunAttachShot();  // kick1 (steadyonly1: auto rifle scope counts as scoped): hip-fire recoil (scoped shots kick the scope instead)
    float hand[3], beam[3], raw[3];
    if (IsBadWritePtr(origin, 12) || IsBadWritePtr(dir, 12)) return;
    if (!g_shotFromHand || !g_aim || !EngineOnFoot() || !LaserRay(hand, beam, raw)) return;  // left as the game aimed it
    const float game0[3] = {dir[0], dir[1], dir[2]};
    // scope3: while scoped (scope eye), shots leave from the scope view along its centre (DrGonzaga).
    {
        float sp[3], sd[3];
        if (EngineScopeRay(sp, sd)) {
            // scope4: exactly along the scope's centre. The game's own shot
            // direction is left out: zoomed, the game turns slower, so its look
            // lags the controller and that lag (not spread) was moving the shots.
            float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (l < 1e-4f) return;
            for (int i = 0; i < 3; ++i) origin[i] = sp[i], dir[i] = sd[i] * l;
            InterlockedIncrement(&g_shotsMoved);
            EngineScopeKick();  // recoil1
            return;
        }
    }
    float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (l < 1e-4f) return;
    float nd[3], nl = 0, d2 = 0, turn = 0;
    // shotwatch1: the game's direction is only "the hand plus spread" when the shot was pointed along the hand.
    // When it was not (or the two are far apart), the difference is how far the game's look is from the hand,
    // not spread: such a shot goes straight down the beam.
    const float gameToHand = Deg(game0, raw);
    const bool keepSpread = aimed && gameToHand <= 12.0f && !EngineTurret();
    const bool mounted = EngineTurret();
    for (int i = 0; i < 3; ++i) {
        nd[i] = beam[i] + (keepSpread || mounted ? dir[i] / l - raw[i] : 0);  // the beam plus the game's spread
        nl += nd[i] * nd[i];
    }
    nl = sqrtf(nl);
    for (int i = 0; i < 3; ++i) {
        nd[i] /= nl;
        turn += nd[i] * dir[i] / l;
        float d = hand[i] - origin[i];
        d2 += d * d;
        origin[i] = hand[i];
        dir[i] = nd[i] * l;
    }
    float moved = sqrtf(d2);
    {
        float bl = sqrtf(beam[0] * beam[0] + beam[1] * beam[1] + beam[2] * beam[2]);
        if (bl > 1e-4f && !EngineScopePictureActive()) {
            float bu[3] = {beam[0] / bl, beam[1] / bl, beam[2] / bl};
            GunAttachShotDir(bu, nd);  // aimdev1: the kick, beam and dot go where this bullet went
        }
    }
    InterlockedIncrement(&g_shotsMoved);
    g_shotMoveSum += moved, g_shotTurnSum += acosf(turn > 1 ? 1 : turn) * 57.2958f;
    if (moved > g_shotMoveMax) g_shotMoveMax = moved;
}

}  // namespace

namespace {

// nopeek1 (31 Oct): firing next to cover, the game lifts the camera (and aim) over it: the
// first-person cover update (props PeekDetection, fPeekMoveDistance at +0x508; lift kept at +0x9c)
// only peeks when its +0x48 flag is set. Both checks of that flag are turned into "not set" jumps,
// so the lift just eases back to 0, as when not at cover. Aim page switch. bd4: Blood Dragon's two
// checks are at 0x10432bb0 and 0x10432bc3 (same test, other jump distances).
bool g_peekOk, g_peekOff = true;
void PeekApply() {
    if (!g_peekOk) return;
    static const BYTE o1[6] = {0x80, 0x7b, 0x48, 0x00, 0x74, 0x2f}, o2[6] = {0x80, 0x7b, 0x48, 0x00, 0x74, 0x1c};
    static const BYTE n1[6] = {0xeb, 0x33, 0x90, 0x90, 0x90, 0x90}, n2[6] = {0xeb, 0x20, 0x90, 0x90, 0x90, 0x90};
    PatchBytes(g_fc3 + (0x10432bb0 - kImageBase), g_peekOff ? n1 : o1, 6);
    PatchBytes(g_fc3 + (0x10432bc3 - kImageBase), g_peekOff ? n2 : o2, 6);
    Log("bonehook: game's lift over cover when firing %s", g_peekOff ? "OFF" : "on");
}

}  // namespace

bool *BoneHookNoPeek() { return &g_peekOff; }
void BoneHookPeekApply() { PeekApply(); }


// ---- bd4: Blood Dragon (found 12 Nov by matching both DLLs; each site read in both) ----
// The look bone function is 0x1052c416: thiscall (ecx = animation component, slot, quaternion
// out), ret 8; 44 callers (FC3: 45). The on-foot camera reaches it through the small function
// 0x10436bd6, whose call is at 0x10436c03.
// The shot code does NOT call it: Blood Dragon's two shot functions read the component's
// cached look (quaternion at +0x150 through 0x1052bc26, thiscall (component, out) returning
// out; position at +0x160 through 0x1052c45b). So the two reads are redirected instead:
//   0x1061c6ed in 0x1061c5fa (FC3's 0x119f1ee0, reader 0x119f20d2)
//   0x1061bb85 in 0x1061b9f4 (FC3's 0x119f0f10, reader 0x119f1129)
// Both shot functions are cdecl with the same argument order as FC3's (thing, shooter,
// origin out, direction out, ...). The hip-fire spread is a call here, not an inline read:
// 0x1061c223 calls 0x10510eff (thiscall, weapon in ecx, spread returned in st0).
// Cover peek: the same two checks of +0x48, at 0x10432bb0 and 0x10432bc3 in 0x10432a98.
namespace {
const DWORD kBdBoneFn = 0x1052c416, kBdCamCall = 0x10436c03, kBdQuatGet = 0x1052bc26;
const DWORD kBdFire1 = 0x1061c6ed, kBdFire2 = 0x1061bb85, kBdShot1 = 0x1061c5fa, kBdShot2 = 0x1061b9f4;
const DWORD kBdSpreadCall = 0x1061c223, kBdSpreadFn = 0x10510eff, kBdPeek1 = 0x10432bb0, kBdPeek2 = 0x10432bc3;
const BYTE kBdPrologue[12] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
const BYTE kBdPeekOrig1[6] = {0x80, 0x7b, 0x48, 0x00, 0x74, 0x2f}, kBdPeekOrig2[6] = {0x80, 0x7b, 0x48, 0x00, 0x74, 0x1c};
const BYTE kBdPeekNew1[6] = {0xeb, 0x33, 0x90, 0x90, 0x90, 0x90}, kBdPeekNew2[6] = {0xeb, 0x20, 0x90, 0x90, 0x90, 0x90};
typedef void(__attribute__((thiscall)) *BdBoneFn)(void *comp, DWORD slot, float *q);
typedef float *(__attribute__((thiscall)) *BdQuatFn)(void *comp, float *out);
typedef float(__attribute__((thiscall)) *BdSpreadFn)(void *weapon);
BdBoneFn g_bdBone;
BdQuatFn g_bdQuat;
BdSpreadFn g_bdSpread;

// The camera's read of the look bone: note the player's component and learn the bone's axis.
void __attribute__((thiscall)) BdCamBone(void *comp, DWORD slot, float *q) {
    g_playerAnim = comp;
    g_bdBone(comp, slot, q);
    if (g_verboseLog) InterlockedIncrement(&g_calls);
    if (slot == 0 && q) Learn(q);
}
float *BdFire(void *comp, float *out, bool second) {
    float *q = g_bdQuat(comp, out);
    if (!q || comp != g_playerAnim) return q;
    g_shotMark = GetCurrentThreadId();                // shot1: this shot is the player's
    if (second) g_playerShot = GetCurrentThreadId();  // the spread below is for the player's shot
    g_shotAimed = 0;
    if (g_aim && !EngineTurret()) AimShot(q);         // q is the caller's own copy, not the cached look
    return q;
}
float *__attribute__((thiscall)) BdFireQuat1(void *comp, float *out) { return BdFire(comp, out, false); }
float *__attribute__((thiscall)) BdFireQuat2(void *comp, float *out) { return BdFire(comp, out, true); }
// turacc1 (DrGonzaga 12 Nov: the mounted gun sprayed unless the left trigger was held): with the mounted
// gun following the controller, our own shots from it fly with no spread. "Ours" = marked as the
// player's shot, or the weapon's object is the mounted gun we are on (or sits within 2.5 m of it).
float __attribute__((thiscall)) BdSpreadHook(void *weapon) {
    float s = g_bdSpread(weapon);
    BYTE *tur = g_turretMotion ? TurretGunEnt() : nullptr;
    if (tur) {
        bool mine = g_playerShot == GetCurrentThreadId();
        BYTE *w = (BYTE *)weapon, *a = w && !IsBadReadPtr(w + 4, 4) ? *(BYTE **)(w + 4) : nullptr;
        BYTE *ent = a && !IsBadReadPtr(a + 0xc, 4) ? *(BYTE **)(a + 0xc) : nullptr;
        float d = -1;
        if (ent && !IsBadReadPtr(ent + 0x50, 12) && !IsBadReadPtr(tur + 0x50, 12)) {
            const float *p = (const float *)(ent + 0x50), *q = (const float *)(tur + 0x50);
            d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
        }
        bool ours = mine || ent == tur || (d >= 0 && d < 2.5f);
        static int s_n;
        if (s_n < 12) ++s_n, Log("turret: shot spread %.2f deg, weapon object %p (mounted gun %p, %.2f m apart), %s: %s", s, ent, tur, d,
                                 mine ? "marked as the player's" : "not marked", ours ? "no spread" : "left alone");
        if (ours) {
            if (mine) g_playerShot = 0;
            return 0;
        }
    }
    return SpreadScale(s);
}

bool BdCallIs(BYTE *fc3, DWORD site, DWORD target) {
    BYTE *c = fc3 + (site - kImageBase);
    return c[0] == 0xe8 && c + 5 + *(int *)(c + 1) == fc3 + (target - kImageBase);
}
ShotFn BdWrap(BYTE *fc3, DWORD at) {
    BYTE *fn = fc3 + (at - kImageBase);
    if (memcmp(fn, kBdPrologue, sizeof kBdPrologue)) return nullptr;
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, fn, 5);  // push ebx; mov ebx, esp; push ecx; push ecx
    t[5] = 0xe9;
    *(int *)(t + 6) = (int)(fn + 5 - (t + 10));
    return (ShotFn)(void *)t;
}
}  // namespace

void BoneHookInstallBd(BYTE *fc3) {
    g_fc3 = fc3;
    static const BYTE kQuatGet[9] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x56, 0x57, 0x8d};
    if (memcmp(fc3 + (kBdBoneFn - kImageBase), kBdPrologue, sizeof kBdPrologue) || !BdCallIs(fc3, kBdCamCall, kBdBoneFn) ||
        memcmp(fc3 + (kBdQuatGet - kImageBase), kQuatGet, sizeof kQuatGet) || !BdCallIs(fc3, kBdFire1, kBdQuatGet) ||
        !BdCallIs(fc3, kBdFire2, kBdQuatGet)) {
        Log("bonehook: Blood Dragon look bone code doesn't match; motion aiming off");
        return;
    }
    g_bdBone = (BdBoneFn)(void *)(fc3 + (kBdBoneFn - kImageBase));
    g_bdQuat = (BdQuatFn)(void *)(fc3 + (kBdQuatGet - kImageBase));
    PatchCallTo(fc3 + (kBdCamCall - kImageBase), (void *)BdCamBone);
    PatchCallTo(fc3 + (kBdFire1 - kImageBase), (void *)BdFireQuat1);
    PatchCallTo(fc3 + (kBdFire2 - kImageBase), (void *)BdFireQuat2);
    Log("bonehook: look bone hooked (BD+%08lx camera call, shot reads BD+%08lx and BD+%08lx)", kBdCamCall, kBdFire1, kBdFire2);
    if (BdCallIs(fc3, kBdSpreadCall, kBdSpreadFn)) {
        g_bdSpread = (BdSpreadFn)(void *)(fc3 + (kBdSpreadFn - kImageBase));
        PatchCallTo(fc3 + (kBdSpreadCall - kImageBase), (void *)BdSpreadHook);
        Log("bonehook: hip-fire spread hooked (BD+%08lx), %.0f%% of the game's", kBdSpreadCall, g_hipSpread * 100);
    } else {
        Log("bonehook: Blood Dragon hip-fire spread code doesn't match; off");
    }
    g_shotOrig = BdWrap(fc3, kBdShot1);
    g_shotOrig2 = BdWrap(fc3, kBdShot2);
    if (g_shotOrig && g_shotOrig2) {
        PatchJmp(fc3 + (kBdShot2 - kImageBase), (void *)ShotHook2, 5);
        PatchJmp(fc3 + (kBdShot1 - kImageBase), (void *)ShotHook, 5);
        Log("bonehook: shots start at the beam (BD+%08lx, BD+%08lx)", kBdShot1, kBdShot2);
    } else {
        Log("bonehook: Blood Dragon shot code doesn't match; shots stay at the eye");
    }
    BYTE *a = fc3 + (kBdPeek1 - kImageBase), *b = fc3 + (kBdPeek2 - kImageBase);
    if (memcmp(a, kBdPeekOrig1, 6) || memcmp(b, kBdPeekOrig2, 6)) {
        Log("bonehook: Blood Dragon cover peek code doesn't match; left as the game has it");
    } else {
        g_peekOk = true;
        PeekApply();
    }
}

bool BoneHookAiming() { return g_aim && g_learnt; }

void BoneHookLog() {
    LONG calls = InterlockedExchange(&g_calls, 0);
    if (!g_playerAnim) return;
    Log("bonehook: player animation %p, %ld bone reads in the last 10 s; look axis %s(%.0f %.0f %.0f) err %.4f; shots aimed %ld, not %ld",
        g_playerAnim, calls, g_conj ? "conj " : "", g_axis[0], g_axis[1], g_axis[2], g_learnErr, g_aimed, g_aimMiss);
    g_aimed = g_aimMiss = 0;
    if (g_spreadScaled) Log("bonehook: %ld hip-fire shots tightened to %.0f%% spread", InterlockedExchange(&g_spreadScaled, 0), g_hipSpread * 100);
    Log("bonehook: aim beam drawn %ld times", LaserDrawn());
    if (g_shots) {
        LONG m = g_shotsMoved;
        Log("bonehook: %ld player shots, %ld moved to the beam: start moved %.2f m on average (max %.2f), turned %.2f deg on average",
            g_shots, m, m ? g_shotMoveSum / m : 0.f, g_shotMoveMax, m ? g_shotTurnSum / m : 0.f);
        g_shots = g_shotsMoved = 0, g_shotMoveSum = g_shotMoveMax = g_shotTurnSum = 0;
    }
}
bool BoneHookShotsOn() { return g_aim; }
bool *BoneHookShotFromHand() { return (bool *)&g_shotFromHand; }
float *BoneHookHipSpread() { return &g_hipSpread; }
void BoneHookSetShots(bool on) { g_aim = on; }
