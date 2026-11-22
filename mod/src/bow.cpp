// bow1p (DrGonzaga 10 Nov): physical bow in two-handed mode.
// With the bow out, the bow sits in the LEFT hand (its angle and position are the
// left controller's). Reach to the arrow spot (one of the holster spots: right /
// left hip, behind the right / left shoulder) and squeeze the right grip to take an
// arrow; bring that hand to the bow to nock it; pull back with the grip held to draw;
// let go of the grip to shoot.
//
// The draw is tied to the distance between the hands (DrGonzaga: "so it feels real"):
// the game's own bow (CWeaponFireArrowStrategy) keeps its prime amount, 0..1, at
// +0x61c and the time held at +0x618; its update (FUN_112c5f50, vtable slot at
// 0x11fc4b80) adds the frame time to the time held, sets the prime amount from it
// (time / prime time) and pushes that into the arms' draw animation. We set the time
// held from the hands' distance before each update, so the string, arms and arrow
// follow the right hand, and the shot's tension (speed, spread) is the draw you
// let go at (FUN_112c5850 reads +0x61c when the arrow leaves).
//
// While drawing, the bow (and aim, laser, shots) points along the line from the right
// hand to the left; the left controller keeps the bow's cant (roll). The arrow model
// is hidden until taken, in the right hand while carried, and on the string at the
// right hand once nocked.
//
// Cost: a few vector ops per pad read and per hand query; the game hook is one
// vtable slot with two float writes.
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <math.h>
#include <string.h>

#include "fc3vr.h"

static bool g_on = true;           // [twohand] PhysicalBow
static int g_arrowSpot = 2;        // [twohand] BowArrowSpot: 0 right hip, 1 left hip, 2 behind right shoulder, 3 behind left
static float g_nockReach = 0.15f;  // m: right hand this close to the bow (left hand) nocks
static float g_fullDraw = 0.65f;   // m: hands this far apart = full draw
static float g_arrowAhead = 0.0f;  // m: arrow model moved along its length from the right hand
bool *BowPhysicalOn() { return &g_on; }
int *BowArrowSpot() { return &g_arrowSpot; }
// spot1 (DrGonzaga 11 Nov): arrows only from the bow's own spot: the game's slots say where (holster.cpp),
// else the spot it was last taken from
int BowQuiverSpot() {
    int s = HolsterBowSpot();
    if (s >= 0 && s < 4) g_arrowSpot = s;
    return g_arrowSpot;
}
float *BowNockReach() { return &g_nockReach; }
float *BowFullDraw() { return &g_fullDraw; }
float *BowArrowAhead() { return &g_arrowAhead; }
// bow4p (DrGonzaga 10 Nov: the arrow was held by its tip): the model's origin is its tip, so it
// sits this far ahead of the hand along its length; the hand holds the back end.
static float g_arrowLen = 0.72f;  // [twohand] BowArrowLength
float *BowArrowLength() { return &g_arrowLen; }
// bow4p (DrGonzaga 10 Nov): after the shot the bow's front pitches down (like a real release)
// then softly but quickly comes back to the left controller. Model only (aim and view unchanged).
static float g_drop = 8.0f;  // degrees at the peak, [twohand] BowDrop
float *BowDropAmount() { return &g_drop; }
static volatile DWORD g_shotT;
// quiver4 (DrGonzaga 11 Nov: arrows land high and right of the dot, variably; the guns are fine):
// arrows don't go through the shot code the mod aims for bullets (no shot-hook call at the loose),
// so while an arrow is nocked / drawn and just after the loose, every reader of the game's look
// bone (except the camera) gets the bow's aim, as the bullet readers do (bonehook.cpp AimShot).
static bool g_arrowsAlongAim = true;
bool *BowArrowsAlongAimOn() { return &g_arrowsAlongAim; }
// quiver5 (DrGonzaga 11 Nov: still high and right, but consistent): sight zeroing for the bow. The
// game's own aim (its look steering and the look bone) is turned by these, the dot and laser stay:
// negative up moves the arrows down, negative right moves them left. Degrees.
static float g_zeroUp, g_zeroRight;
float *BowZeroUp() { return &g_zeroUp; }
float *BowZeroRight() { return &g_zeroRight; }
static void ZeroTurn(float d[3], const float up[3]) {  // d (unit) turned up / right by the zeroing
    float R[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
    float rl = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
    if (rl < 1e-4f) return;
    for (int k = 0; k < 3; ++k) R[k] /= rl;
    float U[3] = {R[1] * d[2] - R[2] * d[1], R[2] * d[0] - R[0] * d[2], R[0] * d[1] - R[1] * d[0]};
    float a = tanf(g_zeroRight * 0.0174533f), b = tanf(g_zeroUp * 0.0174533f), n = 0;
    for (int k = 0; k < 3; ++k) d[k] += a * R[k] + b * U[k], n += d[k] * d[k];
    n = sqrtf(n);
    for (int k = 0; k < 3; ++k) d[k] /= n;
}
// quiver9: no random arrow spread at full draw ([twohand] BowNoSpread, Bow page)
static bool g_noSpread = true;
bool *BowNoSpread() { return &g_noSpread; }
// quiver7: look steering gain multiplier while an arrow is drawn ([twohand] BowSteerBoost, Bow page)
static float g_steerBoost = 2.5f;
float BowSteerBoost() { return g_steerBoost; }
float *BowSteerBoostPtr() { return &g_steerBoost; }
bool BowZeroActive() { return BowActive() && (g_zeroUp != 0 || g_zeroRight != 0); }
void BowZeroWorld(float d[3]) {  // game world, z up
    static const float up[3] = {0, 0, 1};
    if (BowZeroActive()) ZeroTurn(d, up);
}
void BowZeroXr(float m[16]) {  // headset space (y up, forward -z): the whole frame turned
    if (!BowZeroActive()) return;
    static const float up[3] = {0, 1, 0};
    float f[3] = {-m[2], -m[6], -m[10]}, g[3] = {f[0], f[1], f[2]};
    ZeroTurn(g, up);
    float k[3] = {f[1] * g[2] - f[2] * g[1], f[2] * g[0] - f[0] * g[2], f[0] * g[1] - f[1] * g[0]};
    float s = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = f[0] * g[0] + f[1] * g[1] + f[2] * g[2];
    if (s < 1e-6f) return;
    float x = k[0] / s, y = k[1] / s, z = k[2] / s, t = 1 - c;
    float Q[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                  t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                  t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    float r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = Q[i * 3] * m[j] + Q[i * 3 + 1] * m[4 + j] + Q[i * 3 + 2] * m[8 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = r[i * 3 + j];
}
// bow12p (DrGonzaga 11 Nov: "work on that animation and return to left pose"): times on the page.
static float g_dropIn = 0.04f, g_dropBack = 0.08f, g_returnT = 0.25f;  // s: snap down, spring back (time constant), back to the left hand
float *BowDropIn() { return &g_dropIn; }
float *BowDropBack() { return &g_dropBack; }
float *BowReturnTime() { return &g_returnT; }
// 0..1 shape of the dip over time (bow12p times)
float BowDipCurve() {
    DWORD t0 = g_shotT;
    if (!t0) return 0;
    float t = (GetTickCount() - t0) * 0.001f, in = g_dropIn > 0.005f ? g_dropIn : 0.005f, bk = g_dropBack > 0.01f ? g_dropBack : 0.01f;
    if (t > in + 6 * bk) return 0;
    // snaps down, then eases back (exponential: ~95% back after 3 time constants)
    if (t < in) return sinf(1.5708f * t / in);
    return expf(-(t - in) / bk);
}
float BowDropNow() { return g_drop > 0 ? g_drop * BowDipCurve() : 0; }
// bow15p (DrGonzaga 11 Nov: "drop it down/forward just a little as it fires"): the bow also moves (m)
static float g_dipDown = 0.03f, g_dipFwd = 0.03f;
float *BowDipDown() { return &g_dipDown; }
float *BowDipForward() { return &g_dipFwd; }
// bow15p (DrGonzaga 11 Nov: the first arrow was held by its point): the bow's own first arrow model
// sits this far further along its shaft than the later (hand) arrows, m
static float g_firstShift = 0.65f;
float *BowFirstArrowShift() { return &g_firstShift; }
// bow15p (DrGonzaga 11 Nov: "can I adjust the grip point?"): where the string / arrow's back end sits
// on the right controller (right, up, forward, m); the bow turns its arrow line through that point
static float g_nockPt[3];
float *BowNockPoint() { return g_nockPt; }
void BowPlayRelease() { g_shotT = GetTickCount() | 1; }  // menu: show the dip

enum { kEmpty, kCarry, kNocked };
static volatile LONG g_state = kEmpty;
static volatile bool g_active;      // bow out, two-handed bow controls in charge
static volatile bool g_rt;          // we hold the game's fire (drawing)
// unnock9: the game's draw is being called off without a shot, the game's own way (DrGonzaga: in the
// flat game X cancels a drawn arrow): with the game's fire still held, its reload (pad X) is tapped;
// the game runs its abort (the draw runs down, the pull sound stops, bow strategy +0x620 "aborted"
// goes on), and only then is the fire let go. History: forcing the update's abort branch (unnock2-5)
// still loosed an arrow; the no-shot byte +0x648 (unnock6-8) loosed nothing but left the game in its
// draw (pull sound on, no weapon swap until an arrow was shot). +0x648 is now only the fallback.
static volatile bool g_cancel;
static volatile DWORD g_cancelMs;
// unnock10: pad X is the game's "[Reload] Normal arrow" (Blood Dragon has one arrow type, so the FC3
// same-arrows-again cancel for fire / explosive arrows is gone).
bool BowCancelTap() { return g_cancel && BowClockMs() - g_cancelMs < 150; }  // input.cpp: hold pad X
static bool g_unCancel = true;  // [twohand] BowUnnockCancel
bool *BowUnnockCancel() { return &g_unCancel; }
static volatile float g_draw;       // 0..1 from the hands' distance
static volatile float g_aimW;       // 0..1 how much the aim follows the hand-to-hand line
static volatile LONG g_buzzReq;     // 1 short, 2 release thump
static const float kBrace = 0.12f;  // m: hands' distance where the draw starts (string at rest)
static volatile float g_relW;       // aim turn at the moment of the shot, fading out
static volatile DWORD g_relT;
// string10: a millisecond clock finer than GetTickCount (the string's ease-in and snap back), and
// when the arrow was nocked / the game's fire was pressed, on it.
static volatile DWORD g_nockMs, g_rtMs;
DWORD BowClockMs() {
    static LARGE_INTEGER s_f;
    LARGE_INTEGER c;
    if (!s_f.QuadPart) QueryPerformanceFrequency(&s_f);
    QueryPerformanceCounter(&c);
    return (DWORD)(c.QuadPart * 1000 / s_f.QuadPart);
}
DWORD BowNockMs() { return g_nockMs; }
bool BowRtHeld() { return g_rt; }
static const char *const kSpotNames[4] = {"right hip", "left hip", "right shoulder", "left shoulder"};

static bool IsBowName(const char *n) { return strstr(n, "BOW") || strstr(n, "Bow") || strstr(n, "bow"); }
bool BowOut() { return g_on && *TwoHandOn() && IsBowName(GunAttachName()); }
// bowtrig1 (DrGonzaga 12 Nov: a toggle for the bow as it was before the motion controls, in the right
// hand with the trigger, faster in a bow challenge): the bow is out and the motion controls are not
// in charge of it. Right stick up still picks the arrow type then (holster.cpp, input.cpp).
bool BowClassic() { return !g_active && IsBowName(GunAttachName()) && GetTickCount() - GunAttachHandTick() < 300; }

// bowfeel1 (DrGonzaga 12 Nov: faster, calmer shooting): the bow stays the left hand's. The arrow hand
// only starts to steer once it has pulled the string back a little (so the nock itself moves
// nothing), it steers with less than full weight, and after the shot the bow keeps the turn it had
// against the left controller for as long as that hand keeps aiming at roughly the same place, so
// the next arrow starts from the same aim instead of swinging back and out again.
static float g_steerStart = 0.06f;  // m of pull before the arrow hand steers at all   [twohand] BowSteerStart
static float g_steerRange = 0.12f;  // m of pull over which its say grows to the full   [twohand] BowSteerRange
static float g_steerMax = 0.7f;     // 0..1: how far the bow turns onto the hand line   [twohand] BowSteerWeight
static float g_keepAngle = 20.0f;   // deg the bow hand may turn after a shot and keep the aim (0 = always return) [twohand] BowKeepAngle
float *BowSteerStart() { return &g_steerStart; }
float *BowSteerRange() { return &g_steerRange; }
float *BowSteerWeight() { return &g_steerMax; }
float *BowKeepAngle() { return &g_keepAngle; }
static float g_nockTick = 0.8f;  // 0..1 strength of the tick in both hands when the arrow snaps on (0 = none) [twohand] BowNockTick
float *BowNockTick() { return &g_nockTick; }
static volatile DWORD g_tickUntil;  // bowfeel2: the nock tick also runs in the bow hand until this tick
// The turn kept after a shot: a rotation in the left controller's own frame (L' = L * D), how much
// of it is still in force, and where that hand pointed when the arrow went. Written by the input
// thread (BowInput), read wherever the hand is asked for (BowApply).
static SRWLOCK g_holdLock = SRWLOCK_INIT;
static float g_holdD[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, g_holdFwd[3];
static volatile float g_holdW;
static DWORD g_holdFadeT;  // 0 = held in full; else the tick the fade began
static void Mul3(const float *A, const float *B, float *C) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) C[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
}
static void MulT3(const float *A, const float *B, float *C) {  // A^T * B
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) C[i * 3 + j] = A[i] * B[j] + A[3 + i] * B[3 + j] + A[6 + i] * B[6 + j];
}
// R (a rotation) taken the fraction t of its angle
static void Part3(const float *R, float t, float *O) {
    static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float c = (R[0] + R[4] + R[8] - 1) * 0.5f;
    float k[3] = {R[7] - R[5], R[2] - R[6], R[3] - R[1]};
    float s = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]) * 0.5f;
    if (t <= 0.0005f || s < 1e-6f) {
        memcpy(O, t > 0.9995f ? R : I, 36);
        return;
    }
    if (t > 0.9995f) {
        memcpy(O, R, 36);
        return;
    }
    float a = atan2f(s, c) * t, x = k[0] / (2 * s), y = k[1] / (2 * s), z = k[2] / (2 * s);
    float cs = cosf(a), sn = sinf(a), u = 1 - cs;
    float Q[9] = {u * x * x + cs,     u * x * y - sn * z, u * x * z + sn * y,
                  u * x * y + sn * z, u * y * y + cs,     u * y * z - sn * x,
                  u * x * z - sn * y, u * y * z + sn * x, u * z * z + cs};
    memcpy(O, Q, 36);
}
static void BowTurn(const float lm[16], const float offL[3], const float rm[16], const float offR[3], bool nocked, float w, float D[9]);

// DrGonzaga 10 Nov: left grip at the bow's spot takes the bow (held while the grip is held);
// the left grip next to the right hand also takes a bow that is already out. Other spots:
// that spot's weapon comes out as usual (right hand). Returns true while the left hand holds the bow.
static bool LeftHold(const VrPad &p, bool ok) {
    static bool s_grip, s_hold, s_pending;
    static int s_spot = -1;
    static DWORD s_t;
    bool grip = p.lgrip > (s_grip ? 0.35f : 0.5f);
    bool press = grip && !s_grip;
    s_grip = grip;
    DWORD now = GetTickCount();
    bool bow = IsBowName(GunAttachName()) && now - GunAttachHandTick() < 300;
    // bowstay1 (DrGonzaga 11 Nov): the bow stays in the left hand after the grip is let go, until
    // another weapon comes out (menus, vehicles and cutscenes only pause it).
    if (!ok) return false;
    static DWORD s_gone;
    if (s_hold && !bow) {
        if (!s_gone) s_gone = now;
        if (now - s_gone <= 600) return false;  // (a moment without it: a reload, a hiccup)
        LogV("bow: bow put away; left hand free");
        s_hold = false, s_gone = 0;
    } else {
        s_gone = 0;
    }
    // bowfirst1 (DrGonzaga 11 Nov: a bow already selected, e.g. at a game start or from the weapon wheel,
    // showed an arrow nocked in the right controller): a bow that comes out any way goes straight
    // into the left hand, empty; arrows come from the last arrow spot as usual.
    if (bow && !s_hold && !s_pending) {
        s_hold = true;
        Log("bow: bow out; in the left hand, empty");
        return true;
    }
    if (!grip && !s_hold && !s_pending) return false;
    if (press) {
        int spot = HolsterLeftSpot();
        // rightonly1 (DrGonzaga 13 Nov: the left hand took the left shoulder weapon by accident): the left
        // grip takes the bow from the bow's own spot and nothing else; every other weapon is the right grip's.
        // bowspot2 (DrGonzaga 15 Nov: "can't select the bow using my left grip at right shoulder"): Blood Dragon never
        // learnt the bow's spot (the slot reader is Far Cry 3's), so every spot counted as "not the bow's". Not
        // known = the spot the arrows come from ([bow] ArrowSpot, the right shoulder unless changed).
        int bs = HolsterBowSpot();
        if (bs < 0) bs = g_arrowSpot;
        if (spot >= 0 && spot != bs) {
            Log("bow: left grip at the %s: not the bow's spot (%s), nothing taken", spot < 4 ? kSpotNames[spot] : "chest", kSpotNames[bs & 3]);
            spot = -1;
        }
        if (spot >= 0) {
            if (bow && s_hold && spot != BowQuiverSpot()) {  // spot1: the bow's own spot, not the last left grip's  // bowstay1: the bow stays, so the left grip at another spot swaps weapon
                s_hold = false, s_pending = false, HolsterTakeSpot(spot);
                LogV("bow: left hand at spot %d: that weapon instead of the bow", spot);
                return false;
            }
            s_spot = spot;
            if (bow) s_hold = true;
            else {
                s_pending = true, s_t = now, HolsterTakeSpot(spot);
                LogV("bow: left hand at spot %d: taking that weapon", spot);
            }
        } else if (bow) {  // next to the right hand (the bow already out)
            float lm[16], rm[16], oL[3], oR[3];
            if (HeadtrackHandRaw(0, lm, oL) && HeadtrackHandRaw(1, rm, oR)) {
                float d[3] = {oL[0] - oR[0], oL[1] - oR[1], oL[2] - oR[2]};
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.25f * 0.25f) s_hold = true;  // bow2p: quiver unchanged
            }
        }
        if (s_hold) {
            // bow2p (DrGonzaga 10 Nov: no arrow came): taking the bow next to the right hand
            // reset the arrow spot to the default (behind the right shoulder) while he reached
            // for the spot he took the bow from. The spot the bow was last taken from stays the arrow spot.
            if (spot >= 0) g_arrowSpot = spot;
            Log("bow: bow in the left hand (arrows from the %s)", kSpotNames[g_arrowSpot]);
        }
    }
    if (s_pending && bow) {
        s_pending = false, s_hold = true, g_arrowSpot = s_spot;
        Log("bow: bow taken into the left hand from the %s (arrows from there too)", kSpotNames[s_spot]);
    }
    if (s_pending && now - s_t > 2000) s_pending = false;  // that spot held something else
    return s_hold && bow;
}

// bow6p (DrGonzaga 10 Nov): "Bow & arrow" page. Preview: while the debug menu is open, the bow sits
// in the left hand and an arrow in the right, to line them up with the sliders below.
static bool g_preview;
bool *BowPreview() { return &g_preview; }
static bool g_previewNock;  // bow7p: preview the arrow on the bow instead of in the hand
bool *BowPreviewNocked() { return &g_previewNock; }
static float g_bowPose[6] = {0, 0, 0, 0, 0, 0};    // bow in the left hand: right, up, forward (m), pitch, yaw, roll (deg)
static float g_arrowPose[6] = {0, 0, 0, 0, 0, 0};  // arrow in the right hand, same order (forward is along the arrow: the length sliders)
static bool g_bowPoseSet;  // [bow] pose saved (else seeded from the bow's one-handed settings once)
float *BowHandPose() { return g_bowPose; }
float *BowArrowPose() { return g_arrowPose; }
// arrowsets1 (DrGonzaga 13 Nov): the bow's own first arrow is a different model from the later ones,
// so it gets its own place in the hand ([twohand] FirstGrip*); g_arrowPose is the later arrows'.
static float g_arrowPoseFirst[6];
float *BowArrowPoseFirst() { return g_arrowPoseFirst; }
bool *BowHandPoseSet() { return &g_bowPoseSet; }
static bool g_aimArrow = true;  // aimfix1 (see AimAlongArrow)
static void SaveArrowLine();
static bool BowLineFresh();
static bool ArrowLineChanged();
static float g_nockHands = 0.40f;  // bow12p (DrGonzaga 11 Nov: stickier nocking): right hand this close to the bow hand nocks
float *BowNockHands() { return &g_nockHands; }
static float g_nockAhead;  // bow7p: m, the nocked arrow moved along itself from the game's rest place
float *BowNockAhead() { return &g_nockAhead; }
static float g_nockDist = kBrace, g_pull;  // bow7p: hands' distance at the nock; how far the hand has pulled since

// bow7p (DrGonzaga 10 Nov: nocking unreliable): the shaft as the arrow is drawn in the hand (controller
// forward turned by the page's pitch / yaw / roll, the same turn gunattach uses), or simply the right
// hand (the arrow's back end) brought close to the bow hand, as when setting an arrow on the string.
static void ShaftDir(const float rm[16], float f[3]) {
    float R[3] = {rm[0], rm[4], rm[8]}, Fw[3] = {-rm[2], -rm[6], -rm[10]}, U[3] = {rm[1], rm[5], rm[9]};
    const float *ap = g_arrowPose;
    float p = ap[3] * 0.0174533f, y = ap[4] * 0.0174533f, rl = ap[5] * 0.0174533f;
    // row 1 (forward) of yaw * pitch * roll in (right, forward, up), as gunattach TurnFrame
    float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(rl), sr = sinf(rl);
    float Ry[9] = {cy, sy, 0, -sy, cy, 0, 0, 0, 1};
    float Rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp};
    float Rr[9] = {cr, 0, -sr, 0, 1, 0, sr, 0, cr};
    float T[9], Q[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) T[i * 3 + j] = Rp[i * 3] * Rr[j] + Rp[i * 3 + 1] * Rr[3 + j] + Rp[i * 3 + 2] * Rr[6 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Q[i * 3 + j] = Ry[i * 3] * T[j] + Ry[i * 3 + 1] * T[3 + j] + Ry[i * 3 + 2] * T[6 + j];
    for (int k = 0; k < 3; ++k) f[k] = Q[3] * R[k] + Q[4] * Fw[k] + Q[5] * U[k];
}
static bool ShaftNearBow(const float rm[16], const float offR[3], const float offL[3]) {
    float d[3] = {offL[0] - offR[0], offL[1] - offR[1], offL[2] - offR[2]};
    float r2 = g_nockHands;  // bow12p: own setting (was nock reach + 10 cm)
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < r2 * r2) return true;
    float f[3];
    ShaftDir(rm, f);
    float t = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    const float len = 0.75f;
    if (t < 0.05f) t = 0.05f;
    if (t > len) t = len;
    float e[3] = {d[0] - t * f[0], d[1] - t * f[1], d[2] - t * f[2]};
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < g_nockReach * g_nockReach;
}

// unnock1 (DrGonzaga 11 Nov: an option to un-nock): degrees between the arrow as the right controller
// holds it and the arrow's line on the bow as the left controller holds it. Nocked, the two run the
// same way; turning the arrow hand away past the angle, with the string at rest, takes the arrow off
// again (still in the hand; let go of the grip to put it back). It nocks again inside 2/3 of the angle.
// unnock5 (DrGonzaga's recorded motion, 11 Nov 22:20Z): what he does to take an arrow off is ROLL the
// arrow hand about a quarter turn (68-128 deg against the bow hand; a normal nock and draw stay inside
// about 15, up to 47 in passing). So the measure is the roll between the two controllers, not the angle
// between where they point (2-41 deg in normal use, 20-40 in the take-off pose: no gap).
static float g_unAngle = 55;  // deg of roll, 0 = never
float *BowUnnockAngle() { return &g_unAngle; }
static float HandsAngle(const float lm[16], const float rm[16]);
// An arrow just taken off the string, hand still at the bow: back on after a second, once held
// lined up (inside half the un-nock angle) for 0.4 s.
static bool RenockHeld(float ang, DWORD now, DWORD unMs, DWORD *lined) {
    if (ang >= g_unAngle * 0.5f || now - unMs < 600) {  // unnock7 (DrGonzaga: re-nocking a removed arrow was not sticky): was 1.5 s + 0.5 s
        *lined = 0;
        return false;
    }
    if (!*lined) *lined = now ? now : 1;
    return now - *lined > 250;
}

bool BowActive() { return g_active; }
bool BowArrowsAlongAim() {
    if (!g_active || !g_arrowsAlongAim) return false;
    DWORD lt = g_shotT;
    return g_state == kNocked || (lt && GetTickCount() - lt < 400);
}
int BowState() { return g_active ? (int)g_state : -1; }
float BowDraw() { return g_draw; }

// Input thread (input.cpp), each pad read, before the button mix. Returns true while
// the bow controls own the right trigger and grip; *rt = the game's fire held.
bool BowInput(const VrPad &p, bool *rt) {
    static bool s_grip;
    // bow6p: preview while the debug menu is open (bow out): bow left, arrow right, no shooting.
    static bool s_prev;
    bool prev = g_preview && MenuOpen() && p.handValid[0] && p.handValid[1] && IsBowName(GunAttachName()) &&
                GetTickCount() - GunAttachHandTick() < 300;
    if (prev) {
        if (!s_prev) Log("bow: preview in the menu (bow left hand, arrow right hand)");
        s_prev = true, g_active = true, g_state = g_previewNock ? kNocked : kCarry, g_pull = 0, g_rt = false, g_draw = 0, g_aimW = 0, g_relW = 0;
        *rt = false;
        return true;
    }
    if (s_prev) s_prev = false, g_active = false, g_state = kEmpty;
    bool ok = g_on && *TwoHandOn() && p.handValid[0] && p.handValid[1] && EngineOnFoot() && !EngineInVehicle() &&
              !EngineCutscene() && !MenuOpen() && !GamePaused();
    // qte6 (DrGonzaga 12 Nov: stuck in a scripted scene that hands him the bow and asks for the right
    // trigger; the physical bow owns that trigger): while the game asks for a button in a scene the
    // physical bow steps back and the pad's own trigger reaches the game.
    if (EngineScenePrompt()) ok = false;
    bool active = LeftHold(p, ok);
    bool grip = p.rgrip > (s_grip ? 0.35f : 0.6f);
    bool press = grip && !s_grip, release = !grip && s_grip;
    s_grip = grip;
    if (!active) {
        if (g_active) {
            g_active = false, g_state = kEmpty, g_draw = 0, g_aimW = 0, g_holdW = 0;
            if (g_rt) Log("bow: controls off while drawn (shot goes)");
            g_rt = false;
        }
        *rt = false;
        return false;
    }
    if (!g_active) g_active = true, g_state = kEmpty, Log("bow: two-handed bow controls on");
    float lm[16], rm[16], offL[3], offR[3];
    if (!HeadtrackHandRaw(0, lm, offL) || !HeadtrackHandRaw(1, rm, offR)) {
        *rt = g_rt;
        return true;
    }
    float d[3] = {offL[0] - offR[0], offL[1] - offR[1], offL[2] - offR[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    LONG st = g_state;
    static bool s_un;  // unnock1: the carried arrow came off the string (nocks again only lined up)
    static int s_angLog;
    static float s_unAt = 55;  // this nock's un-nock roll (the setting)
    static float s_nockAng;    // seat1: the roll this arrow was nocked at
    // unnock3 (DrGonzaga 11 Nov: "the nock is overriding it"; log: un-nocked at 54 deg, nocked again
    // 0.17 s later at 35): an arrow taken off the string goes back on only after the hand has left
    // the bow and come back, or after a second held lined up (inside half the angle)
    static bool s_away;
    static DWORD s_unMs, s_linedMs;
    float ang = g_unAngle > 0 && st != kEmpty ? HandsAngle(lm, rm) : 0;
    if (st == kEmpty) {
        s_un = false;
        // The arrow spot (holsters on) or, with holsters off, anywhere away from the bow.
        int spot = HolsterSpot(nullptr);
        // spot1 (DrGonzaga 11 Nov: arrows came from every hip / shoulder spot): only the bow's own spot
        // (the other spots swap weapon, holster.cpp); replaces bow13p's any spot
        int qs = BowQuiverSpot();
        bool at = *HolsterOn() ? spot == qs : dist > g_nockReach * 2;
        if (press && at) {
            g_state = kCarry, InterlockedExchange(&g_buzzReq, 1);
            LogV("bow: arrow taken from the %s", spot >= 0 && spot <= 3 ? kSpotNames[spot] : "side");
        } else if (press) {
            LogV("bow: right grip with no arrow there (hand at %s, arrows at the %s)", spot >= 0 && spot < 4 ? kSpotNames[spot] : spot >= 4 ? "the chest" : "no spot", kSpotNames[qs]);
        }
    } else if (st == kCarry) {
        if (!grip) {
            g_state = kEmpty;
            LogV("bow: arrow let go before nocking");
        } else if (!ShaftNearBow(rm, offR, offL) || g_cancel) {
            if (!g_cancel) s_away = true;
            s_linedMs = 0;
        } else if (s_un && g_unAngle > 0 && !s_away && !RenockHeld(ang, BowClockMs(), s_unMs, &s_linedMs)) {
        } else {
            // seat1 (DrGonzaga 12 Nov, Blood Dragon: un-nocking was inconsistent; log: arrows nocked with the
            // hand already rolled 39-59 deg took 4-7 s to come off, the others about 1 s): an arrow nocked
            // with the hand rolled does not have to be levelled first; it comes off 25 deg past that roll.
            s_nockAng = ang;
            // unnock11 (DrGonzaga 13 Nov: after a few nocks and un-nocks of one arrow it would not come off; log:
            // each re-nock was with the hand still rolled, so the mark climbed 55, 66, 76, 86 ... 136 deg):
            // the mark stops at 80 deg (or the set angle if higher). A nock rolled past that has to be
            // levelled first (below) and then rolled again.
            float cap = g_unAngle > 80 ? g_unAngle : 80;
            s_unAt = ang + 25 > g_unAngle ? ang + 25 : g_unAngle;
            if (s_unAt > cap) s_unAt = cap;
            if (s_angLog < 30) ++s_angLog, Log("bow: nocked with the arrow hand rolled %.0f deg against the bow hand (un-nocks past %.0f)", ang, s_unAt);
            s_un = false;
            g_state = kNocked, InterlockedExchange(&g_buzzReq, 3);  // bowfeel2: its own tick
            // bow7p: the draw (and the arrow sliding back) counts from where the hands were at the nock
            float nd = dist < kBrace ? kBrace : dist;
            if (nd > g_fullDraw - 0.15f) nd = g_fullDraw - 0.15f > kBrace ? g_fullDraw - 0.15f : kBrace;
            g_nockDist = nd, g_pull = 0;
            g_nockMs = BowClockMs();  // string10: the string eases to the hand from here
            LogV("bow: nocked (hands %.0f cm apart)", dist * 100);
        }
    } else {  // nocked
        float span = g_fullDraw - g_nockDist > 0.1f ? g_fullDraw - g_nockDist : 0.1f;
        float k = (dist - g_nockDist) / span;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        g_pull = k * span;  // bow7p: the arrow slides back 1:1 with the hand (up to full draw)
        // string10: with the string following the hand, the draw is how far the hand has taken the
        // string along its own path (measured on the bow by the game thread), not hands apart since the nock
        float gk, gp;
        if (BowStringGeo(&gk, &gp)) k = gk, g_pull = gp;
        g_draw = k;
        // bowfeel1: the arrow hand steers only once it has pulled back from where it nocked (was: hands
        // 10-25 cm apart, which at a 30-40 cm nock was already full weight: the bow jumped at the nock)
        float w = (dist - g_nockDist - g_steerStart) / (g_steerRange > 0.01f ? g_steerRange : 0.01f);
        w = w < 0 ? 0 : w > 1 ? 1 : w;
        g_aimW = w * w * (3 - 2 * w) * (g_steerMax < 0 ? 0 : g_steerMax > 1 ? 1 : g_steerMax);
        // unnock1: the game's own draw starts a little later (15% of the pull, was 6%), so there is
        // a relaxed stretch in which the arrow can still come off without a shot
        // unnock2 (DrGonzaga 11 Nov: it never un-nocked; the hand nocks 2-3 cm behind the string, so the
        // game's draw had always started): "at rest" is now the first quarter of the pull, and a draw
        // the game has already started is called off through the game's own abort
        bool canCancel = g_unCancel;
        static DWORD s_rollMs;  // the roll has to be held for 150 ms (a flick in passing does not count)
        DWORD nowMs = BowClockMs();
        static DWORD s_seatMs;  // ... and counts only once the hand has sat level on this nock
        if (s_seatMs != g_nockMs && (ang < s_unAt * 0.5f || (s_nockAng >= g_unAngle * 0.5f && s_nockAng + 25 <= s_unAt))) s_seatMs = g_nockMs;
        if (!(g_unAngle > 0 && ang > s_unAt)) s_rollMs = nowMs;
        if (grip && g_unAngle > 0 && ang > s_unAt && s_seatMs == g_nockMs && nowMs - s_rollMs > 150 && nowMs - g_nockMs > 250 && (g_rt ? canCancel && k < 0.25f : true)) {
            if (g_rt) g_cancelMs = BowClockMs(), g_cancel = true;
            g_state = kCarry, s_un = true, s_away = false, s_linedMs = 0, s_unMs = BowClockMs(), g_draw = 0, g_pull = 0, release = false;
            InterlockedExchange(&g_buzzReq, 1);
            if (s_angLog < 30) ++s_angLog, Log("bow: un-nocked (arrow hand rolled %.0f deg, draw %.2f)", ang, k);
        } else if (!g_rt && grip && k > (g_unAngle > 0 && !canCancel ? 0.15f : 0.06f)) {
            g_rtMs = BowClockMs();
            g_rt = true;
            LogV("bow: drawing");
        }
        if (release) {
            if (BowLineFresh() && ArrowLineChanged()) MenuSaveLater(SaveArrowLine);  // only when it moved (DrGonzaga 11 Nov)
            if (g_rt) InterlockedExchange(&g_buzzReq, 2);
            g_relW = g_aimW, g_relT = GetTickCount();
            {   // bowfeel1: the turn the bow has now stays (in the left hand's frame) while that hand keeps its aim
                float D[9];
                BowTurn(lm, offL, rm, offR, true, g_aimW, D);
                AcquireSRWLockExclusive(&g_holdLock);
                memcpy(g_holdD, D, 36);
                g_holdFwd[0] = -lm[2], g_holdFwd[1] = -lm[6], g_holdFwd[2] = -lm[10];
                g_holdFadeT = g_keepAngle > 0 ? 0 : (g_relT ? g_relT : 1);
                g_holdW = 1;
                ReleaseSRWLockExclusive(&g_holdLock);
            }
            if (g_rt) g_shotT = g_relT ? g_relT : 1;  // bow4p: the drop after the shot
            g_rt = false, g_state = kEmpty, g_draw = 0, g_aimW = 0;
        }
    }
    if (g_state != kNocked) g_aimW = 0, g_draw = 0;
    if (g_holdW > 0) {  // bowfeel1: the kept turn lets go once the bow hand aims somewhere else
        DWORD now = GetTickCount();
        AcquireSRWLockExclusive(&g_holdLock);
        if (!g_holdFadeT) {
            float c = -lm[2] * g_holdFwd[0] - lm[6] * g_holdFwd[1] - lm[10] * g_holdFwd[2];
            if (c < cosf(g_keepAngle * 0.0174533f)) g_holdFadeT = now ? now : 1;
        }
        if (g_holdFadeT) {
            float rt2 = g_returnT > 0.02f ? g_returnT : 0.02f, x = (now - g_holdFadeT) * 0.001f / rt2;
            g_holdW = x < 1 ? 1 - x * x * (3 - 2 * x) : 0;
        }
        ReleaseSRWLockExclusive(&g_holdLock);
    }
    *rt = g_rt;
    return true;
}

// headtrack.cpp HeadtrackHand(1): with the bow controls on, the "gun hand" is the
// left hand, turned onto the right-to-left line while drawing. offR is replaced.
// bow14p: the nocked arrow's line in the bow hand's own frame (columns of HeadtrackHand(1)'s matrix:
// x right, y up, z back), tracking metres: a point on it nearest the hand, and its direction to the tip.
static volatile float g_lineP[3], g_lineA[3];
static volatile DWORD g_lineT;
static bool g_handLine;  // quiver4: a line from a hand arrow is known (this session or saved)
void BowSetLine(const float p[3], const float a[3], bool hand) {
    // quiver4 (DrGonzaga 11 Nov: the first shot is still lined up oddly, then it feels natural): the
    // first arrow is the bow's own (its pose differs from the arrows taken by hand later), so once
    // a hand arrow's line is known (also saved), the bow lines up the same way for every arrow.
    if (!hand && g_handLine) {
        g_lineT = GetTickCount();
        return;
    }
    if (hand) g_handLine = true;
    for (int k = 0; k < 3; ++k) g_lineP[k] = p[k], g_lineA[k] = a[k];
    g_lineT = GetTickCount();
}
// aimfix1 (DrGonzaga 11 Nov: with the bow, the red dot sat ~35 deg above where the left controller
// felt pointed, while every gun aimed fine): the bow's aim follows the arrow on the bow. The arrow
// line measured from the game's nocked pose (bow-hand frame, kept after the shot) gives a turn C
// (minimal, local) from the hand's forward onto the arrow; the aim = bow frame * C, so the dot,
// laser, sight and the shot run along the arrow. gunattach takes C back off (BowAimFix) to place the
// bow model in the bow frame itself, so the model and the measurement never feed back.
static float g_aimC[9];
static bool g_aimCOk;
static SRWLOCK g_aimLock = SRWLOCK_INIT;
bool *BowAimAlongArrow() { return &g_aimArrow; }
bool BowAimFix(float C[9]) {
    if (!g_active || !g_aimArrow) return false;
    AcquireSRWLockShared(&g_aimLock);
    bool ok = g_aimCOk;
    if (ok) memcpy(C, g_aimC, 36);
    ReleaseSRWLockShared(&g_aimLock);
    return ok;
}
// The last arrow direction is kept in fc3vr.ini, so the aim is right before the first nock too.
static bool BowLineFresh() { return GetTickCount() - g_lineT < 3000; }
static float HandsAngle(const float lm[16], const float rm[16]) {
    // the arrow hand's up axis, seen in the bow hand's own right/up plane
    float x = rm[1] * lm[0] + rm[5] * lm[4] + rm[9] * lm[8], y = rm[1] * lm[1] + rm[5] * lm[5] + rm[9] * lm[9];
    return fabsf(atan2f(x, y)) * 57.29578f;
}
static float g_savedLine[6];  // the line last saved (or loaded): point, direction
static bool ArrowLineChanged() {  // more than ~0.5 deg or 5 mm from the saved one
    if (!g_handLine) return false;
    float d = g_savedLine[3] * g_lineA[0] + g_savedLine[4] * g_lineA[1] + g_savedLine[5] * g_lineA[2], e = 0;
    for (int k = 0; k < 3; ++k) e += (g_savedLine[k] - g_lineP[k]) * (g_savedLine[k] - g_lineP[k]);
    if (d > 0.99996f && e < 0.005f * 0.005f) return false;
    for (int k = 0; k < 3; ++k) g_savedLine[k] = g_lineP[k], g_savedLine[3 + k] = g_lineA[k];
    return true;
}
static void SaveArrowLine() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    wchar_t b2[160];
    swprintf(b2, 160, L"%.4f %.4f %.4f %.4f %.4f %.4f", (double)g_savedLine[0], (double)g_savedLine[1], (double)g_savedLine[2],
             (double)g_savedLine[3], (double)g_savedLine[4], (double)g_savedLine[5]);
    WritePrivateProfileStringW(L"twohand", L"BowArrowLine", b2, path);
}
void BowLoadArrowLine() {
    wchar_t path[MAX_PATH], buf[160];
    IniPath(path);
    float v[6];
    if (!GetPrivateProfileStringW(L"twohand", L"BowArrowLine", L"", buf, 160, path) || g_lineT) return;
    int n = swscanf(buf, L"%f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5);
    if (n == 6 && -v[5] > 0.34f) {  // quiver4: point and direction of a hand arrow's line
        for (int k = 0; k < 3; ++k) g_lineP[k] = v[k], g_lineA[k] = v[3 + k];
        for (int k = 0; k < 6; ++k) g_savedLine[k] = v[k];
        g_handLine = true;
        g_lineT = 1;  // known; BowApply takes it once a nocked arrow refreshes the time
    } else if (n == 3 && -v[2] > 0.34f) {  // aimfix1's (direction only)
        for (int k = 0; k < 3; ++k) g_lineA[k] = v[k];
        g_lineT = 1;
    }
}
static void AimAlongArrow(float lm[16]) {
    bool ok = false;
    float C[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (g_aimArrow && g_lineT) {
        float a[3] = {g_lineA[0], g_lineA[1], g_lineA[2]};
        float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (l > 1e-4f) {
            for (int k = 0; k < 3; ++k) a[k] /= l;
            float c = -a[2];  // forward = -z
            if (c > 0.34f) {  // within 70 deg: a real measurement
                float k[3] = {a[1], -a[0], 0};  // (0,0,-1) x a
                float s = sqrtf(k[0] * k[0] + k[1] * k[1]);
                if (s > 1e-6f) {
                    float x = k[0] / s, y = k[1] / s, z = 0, t = 1 - c;
                    float R[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                                  t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                                  t * x * z - s * y, t * y * z + s * x, t * z * z + c};
                    memcpy(C, R, 36);
                }
                ok = true;
            }
        }
    }
    if (ok) {
        float r[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = lm[i * 4] * C[j] + lm[i * 4 + 1] * C[3 + j] + lm[i * 4 + 2] * C[6 + j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) lm[i * 4 + j] = r[i * 3 + j];
    }
    AcquireSRWLockExclusive(&g_aimLock);
    memcpy(g_aimC, C, 36), g_aimCOk = ok;
    ReleaseSRWLockExclusive(&g_aimLock);
}

// bowfeel1: the bow's turn against the raw left controller, as a rotation in that hand's frame:
// the turn kept from the last shot, blended (by w) into the full turn that lays the arrow's line
// through the arrow hand.
static void BowTurn(const float lmRaw[16], const float offL[3], const float rm[16], const float offR[3], bool nocked, float w, float D[9]) {
    float L[9], H[9], hw;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) L[i * 3 + j] = lmRaw[i * 4 + j];
    AcquireSRWLockShared(&g_holdLock);
    memcpy(H, g_holdD, 36), hw = g_holdW;
    ReleaseSRWLockShared(&g_holdLock);
    Part3(H, hw, D);
    if (!nocked || w <= 0.001f) return;
    float lm[16];
    memcpy(lm, lmRaw, 64);
    {
        float pG[3] = {0, 0, 0}, aG[3] = {0, 0, -1};
        if (GetTickCount() - g_lineT < 3000) memcpy(pG, (const void *)g_lineP, 12), memcpy(aG, (const void *)g_lineA, 12);
        float nk[3];  // bow15p: the nock point on the right controller
        for (int k = 0; k < 3; ++k)
            nk[k] = offR[k] + rm[k * 4] * g_nockPt[0] + rm[k * 4 + 1] * g_nockPt[1] - rm[k * 4 + 2] * g_nockPt[2];
        int iters = 3;
        for (int it = 0; it < iters; ++it) {
            float X[3] = {lm[0], lm[4], lm[8]}, Y[3] = {lm[1], lm[5], lm[9]}, Z[3] = {lm[2], lm[6], lm[10]};
            float pW[3], v[3], d[3];
            for (int k = 0; k < 3; ++k)
                pW[k] = X[k] * pG[0] + Y[k] * pG[1] + Z[k] * pG[2], v[k] = X[k] * aG[0] + Y[k] * aG[1] + Z[k] * aG[2],
                d[k] = offL[k] + pW[k] - nk[k];
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 0.06f) break;
            float b[3] = {d[0] / len, d[1] / len, d[2] / len};
            float k[3] = {v[1] * b[2] - v[2] * b[1], v[2] * b[0] - v[0] * b[2], v[0] * b[1] - v[1] * b[0]};
            float s = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = v[0] * b[0] + v[1] * b[1] + v[2] * b[2];
            if (s < 1e-6f) break;
            float n[3] = {k[0] / s, k[1] / s, k[2] / s}, a = atan2f(s, c);
            float cs = cosf(a), sn = sinf(a), t = 1 - cs, x = n[0], y = n[1], z = n[2];
            float Q[9] = {t * x * x + cs,     t * x * y - sn * z, t * x * z + sn * y,
                          t * x * y + sn * z, t * y * y + cs,     t * y * z - sn * x,
                          t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs};
            float r[9];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) r[i * 3 + j] = Q[i * 3] * lm[j] + Q[i * 3 + 1] * lm[4 + j] + Q[i * 3 + 2] * lm[8 + j];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) lm[i * 4 + j] = r[i * 3 + j];
        }
    }
    float Lf[9], Df[9], Rel[9], Rp[9], Out[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Lf[i * 3 + j] = lm[i * 4 + j];
    MulT3(L, Lf, Df);   // the full turn, in the hand's frame
    MulT3(D, Df, Rel);  // from the kept turn to the full one
    Part3(Rel, w, Rp);
    Mul3(D, Rp, Out);
    memcpy(D, Out, 36);
}

void BowApply(float m[16], float off[3]) {
    if (!g_active) return;
    float lm[16], offL[3], rm[16], offR[3];
    if (!HeadtrackHandRaw(0, lm, offL)) return;
    float w = g_aimW;
    bool nocked = g_state == kNocked && w > 0.001f && HeadtrackHandRaw(1, rm, offR);
    if (nocked || g_holdW > 0.001f) {
        float D[9], L[9], O[9];
        BowTurn(lm, offL, rm, offR, nocked, w, D);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) L[i * 3 + j] = lm[i * 4 + j];
        Mul3(L, D, O);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) lm[i * 4 + j] = O[i * 3 + j];
    }
    AimAlongArrow(lm);  // aimfix1
    memcpy(m, lm, 64);
    if (off) memcpy(off, offL, 12);
}

// gunattach.cpp, for the arrow model: 0 the game's place on the bow, 1 hidden,
// 2 in the right hand, 3 on the string at the right hand (bow's turn).
int BowArrowMode() {
    if (!g_active) return 0;
    LONG st = g_state;
    // bow3p (DrGonzaga 10 Nov: the nocked arrow sat too high and off the string): once nocked,
    // the game's own arrow pose on the bow (rest + string, pulled back by the draw we drive).
    // bow7p (DrGonzaga 10 Nov): nocked = glued to the bow in the game's rest pose, slid back along
    // itself by how far the hand has pulled.
    return st == kEmpty ? 1 : st == kCarry ? 2 : 3;
}

// xr.cpp: right hand buzz (0..1): taking / nocking, the string's tension, the release.
float BowBuzz() {
    static DWORD s_until;
    static float s_amp;
    DWORD t = GetTickCount();
    LONG req = InterlockedExchange(&g_buzzReq, 0);
    if (req == 1) s_until = t + 40, s_amp = 0.5f;
    if (req == 2) s_until = t + 70, s_amp = 0.9f;
    if (req == 3) s_until = t + 30, s_amp = g_nockTick, g_tickUntil = t + 30;
    if ((LONG)(s_until - t) > 0) return s_amp;
    return g_rt ? 0.04f + 0.16f * g_draw : 0;
}

// bowfeel2: left (bow) hand buzz: the tick of the arrow snapping onto the bow.
float BowBuzzLeft() { return (LONG)(g_tickUntil - GetTickCount()) > 0 ? g_nockTick : 0; }


// ---- the game's bow: set the time held from the draw before each update ----
namespace {
const DWORD kImageBase = 0x10000000;
typedef void(__attribute__((thiscall)) * UpdateFn)(BYTE *self, float dt);
UpdateFn g_origUpdate;
float g_prime = 0;  // s: the game's prime time, read back from its own (time, amount) pairs

int g_so, g_po;  // bowbd1: Blood Dragon keeps the same fields 0x30 (bow) and 0x38 (its properties) further on
void __attribute__((thiscall)) BowUpdate(BYTE *self, float dt) {
    bool drive = g_active && g_rt && !g_cancel && !IsBadReadPtr(self + g_so + 0x610, 0x14);
    // unnock9 fallback only: the game did not abort within 1.2 s -> no-shot byte on for 0.6 s
    static DWORD s_cancelFall;
    bool cw = g_cancel && !IsBadWritePtr(self + g_so + 0x648, 1);
    if (cw && s_cancelFall == g_cancelMs) self[g_so + 0x648] = 1;
    if (drive && g_prime <= 0) {  // first draw: the bow's own prime time (+0x72c of its properties)
        BYTE *props = *(BYTE **)(self + g_so + 0x610);
        if (props && !IsBadReadPtr(props + g_po + 0x72c, 4)) {
            float p = *(float *)(props + g_po + 0x72c);
            if (p > 0.05f && p < 10) g_prime = p;
        }
    }
    if (drive && g_prime > 0.05f) {
        float t = g_draw * g_prime - dt;  // the game adds dt
        // string10: while the string is being learnt, the game's draw runs on its own clock (as with
        // the plain trigger), so its whole draw animation plays through once and can be recorded
        if (BowStringLearning()) {
            float el = (float)(BowClockMs() - g_rtMs) * 0.001f;
            t = (el < 0 ? 0 : el < g_prime ? el : g_prime) - dt;
        }
        *(float *)(self + g_so + 0x618) = t < -dt ? -dt : t;
        // bow5p (DrGonzaga 10 Nov: the draw didn't follow the hand): the game only keeps updating
        // the draw while +0x614 ("priming") is set, and clears it after the first update when
        // not aiming down the sight (0x112c62ff); past the minimum draw it then froze. Keep it set.
        *(BYTE *)(self + g_so + 0x614) = 1;
    }
    // quiver9 (DrGonzaga 11 Nov: "any other ways to improve accuracy?"): the game turns each arrow
    // by a random deviation angle times (multiplier - 1) * tension + 1, where the multiplier is the
    // bow's fHipFireFullTensionDeviationMultiplier (+0x724) or fIronSightFullTensionDeviationMultiplier
    // (+0x728), set in 0x112c5850. With both 0, a fully drawn arrow flies with no random spread.
    if (!IsBadReadPtr(self + g_so + 0x610, 4)) {
        static BYTE *s_props;
        static float s_orig[2];
        BYTE *props = *(BYTE **)(self + g_so + 0x610);
        if (props && props != s_props && !IsBadWritePtr(props + g_po + 0x724, 8)) {
            // safe1 (review S2): this used to write the old values back into the PREVIOUS bow's
            // properties here, with no check. After a level load that object may be freed or
            // reused, so that was a write into someone else's memory. The previous object is
            // now simply left alone. If this one still holds our two zeros (the same bow coming
            // back), the values first read from it are kept instead of taking 0 as "the game's".
            float cur[2];
            memcpy(cur, props + g_po + 0x724, 8);
            static BYTE *s_seen[4];
            static float s_seenOrig[4][2];
            static int s_seenNext;
            int known = -1;
            for (int i = 0; i < 4; ++i)
                if (s_seen[i] == props) known = i;
            if (known >= 0 && cur[0] == 0 && cur[1] == 0) {
                memcpy(s_orig, s_seenOrig[known], 8);
            } else {
                memcpy(s_orig, cur, 8);
                int slot = known >= 0 ? known : s_seenNext++ & 3;
                s_seen[slot] = props, memcpy(s_seenOrig[slot], cur, 8);
            }
            s_props = props;
            // tidy2 (12 Nov): with two bows alive (the story fight's own bow beside yours) this ran for
            // each in turn every update and wrote 180 log lines a second; logged once per bow now.
            if (known < 0) Log("bow: the game's arrow deviation at full draw: hip x%.2f, sight x%.2f", s_orig[0], s_orig[1]);
        }
        if (props && props == s_props) {
            float want[2] = {0, 0};
            memcpy(props + g_po + 0x724, g_noSpread ? want : s_orig, 8);
        }
    }
    g_origUpdate(self, dt);
    if (!IsBadReadPtr(self + g_so + 0x618, 8)) {
        float held = *(float *)(self + g_so + 0x618), amt = *(float *)(self + g_so + 0x61c);
        BowStringGame(self, amt);  // string10
        if (g_cancel) {
            DWORD el = BowClockMs() - g_cancelMs;
            bool fall = s_cancelFall == g_cancelMs;
            bool aborted = cw && self[g_so + 0x620] && el > 60;
            if (!fall && (aborted || !cw || !g_active)) {
                Log("bow: game draw cancelled by the game's own abort (%lu ms, amount %.2f)", el, amt);
                g_rt = false, g_cancel = false;
            } else if (!fall && el > 1200) {
                Log("bow: the game did not abort the draw in 1.2 s (amount %.2f, flags %d %d); letting go with the no-shot flag", amt, self[g_so + 0x620], self[g_so + 0x621]);
                s_cancelFall = g_cancelMs, self[g_so + 0x648] = 1, g_rt = false;
            } else if (fall && el > 1800) {
                self[g_so + 0x648] = 0, g_cancel = false;
            }
        }
        if (held > 0.02f && amt > 0.02f && amt < 0.98f) {
            float p = held / amt;
            if (p > 0.05f && p < 10) {
                if (fabsf(p - g_prime) > 0.01f) LogV("bow: game prime time %.2f s", p);
                g_prime = p;
            }
        }
    }
    static bool s_logged;
    if (drive && !s_logged) s_logged = true, Log("bow: game draw driven by the hands (prime time %.2f s)", g_prime);
}
}  // namespace

// bowbd1: Blood Dragon. The update is 0x10590778 (vtable slot 0x1158d960), the same code as FC3's
// 0x112c5f50 with every field 0x30 further on (time held +0x648, amount +0x64c, priming +0x644,
// properties +0x640) and the properties 0x38 further on (deviation multipliers +0x75c / +0x760,
// fPrimeTime +0x764: from the property registrations at 0x104471db, 0x10447211, 0x10447103).
void BowInstallBd(BYTE *bd) {
    void **slot = (void **)(bd + 0x158d960);
    BYTE *want = bd + 0x590778;
    static const BYTE kPro[8] = {0x55, 0x8b, 0xec, 0xf3, 0x0f, 0x10, 0x45, 0x08};
    if (IsBadReadPtr(slot, 4) || *slot != (void *)want || memcmp(want, kPro, 8)) {
        Log("bow: Blood Dragon's bow update not where expected; draw follows the game's own timing");
        return;
    }
    g_so = 0x30, g_po = 0x38;
    g_origUpdate = (UpdateFn)(void *)want;
    void *hk = (void *)BowUpdate;
    if (!PatchBytes(slot, &hk, 4)) return;
    Log("bow: Blood Dragon bow update hooked (draw follows the hands)");
}
