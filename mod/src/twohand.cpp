// twohand1 (DrGonzaga 10 Nov): two-handed weapon holding.
// Put the left hand on the front of the gun and squeeze the left grip: while it is
// held, the right hand still sets where the gun is and its roll, but the gun points
// along the line from the right hand to the left (yaw and pitch).
//
// At the grab we note where the left hand sits in the right controller's own frame
// (as a direction), so the gun doesn't jump when you take hold; after that the gun
// turns with the line between the hands. Letting go blends back to one hand over
// 120 ms. That grip press is ours (the game's weapon wheel doesn't open).
//
// Cost: one extra controller read and a few multiplies per hand query.
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

#include "fc3vr.h"

static bool g_on = true;
static float g_reach = 0.15f;  // m: how far the left hand may be from the barrel's line
static float g_far = 0.80f;    // m: furthest in front of the right hand
bool *TwoHandOn() { return &g_on; }
// stockaim1 (DrGonzaga 11 Nov: two-handed drifts, one-handed is rock solid; he uses a gunstock).
// The line between the hands comes from the two controllers' POSITIONS, which wander by
// several mm to a cm (more when the controllers are close together under the headset);
// at a 21 cm grip that is 1-3 deg. One-handed aim uses the right controller's ROTATION
// (gyro), which doesn't wander. On a stock the hands are fixed to each other anyway, so
// here the right controller keeps aiming; the left grip still counts as two-handed
// (best spread, gun-close kick, two-handed smoothing, no weapon wheel).
static bool g_stockAim = true;
bool *TwoHandStockAim() { return &g_stockAim; }
float *TwoHandReach() { return &g_reach; }
float *TwoHandFar() { return &g_far; }

static SRWLOCK g_lock = SRWLOCK_INIT;
static bool g_held;
static float g_L[3];          // right-hand-to-left-hand direction at the grab, in the right controller's frame
static float g_lastN[3], g_lastA;  // last turn applied (axis, angle), for the blend after letting go
static DWORD g_relT;
static bool g_blend;

// pistolgrip1 (DrGonzaga 11 Nov): left grip with the hands close together (cupping a
// pistol): no change to where the gun points, but it counts as both grips held for the
// two-handed smoothing, spread and recoil. 0 = off.
static float g_cup = 0.15f;  // m between the controllers
static volatile bool g_cupHeld;
float *TwoHandCupReach() { return &g_cup; }
bool TwoHandHeld() { return g_held || g_cupHeld; }

// steady1 (DrGonzaga 12 Nov): pistols only. With the left controller brought close to the
// right one (no grip needed) the pistol is steadied: the right controller's own turn goes
// through an adaptive "One Euro" filter (strong when nearly still, fading as you swing, so
// no lag on fast turns). Where the gun points is not changed (the line between the hands
// is never used). Also counts as two-handed for spread and kick. The left grip stays free.
// Pistol = by gun name, or [twohand.<gun>] PistolSteady=1/0 (tick box on the page).
static bool g_stOn = true;           // [twohand] PistolSteady
static float g_stMinHz = 1.0f;       // [twohand] PistolSteadyHz: filter cut-off when still (lower = steadier)
static float g_stBeta = 2.0f;        // [twohand] PistolSwing: how fast the filter opens up as you swing
static volatile bool g_near;         // hands together with a pistol out (input thread)
static volatile int g_pistolGun;     // 1 = the gun in hand is steadied as a pistol
static int g_pistolOwn = -1;         // [twohand.<gun>] PistolSteady: -1 by name, 0 no, 1 yes
bool *PistolSteadyOn() { return &g_stOn; }
float *PistolSteadyHz() { return &g_stMinHz; }
float *PistolSwing() { return &g_stBeta; }
bool TwoHandSteady() { return g_near; }
// steadybuzz1 (DrGonzaga 15 Nov: "would be good to provide feedback to player when smoothing is active"): a short
// tick in both controllers the moment the pistol becomes steadied ([twohand] SteadyBuzz), and the left hand goes
// onto the pistol (hands.cpp hands25).
static bool g_stBuzz = true;
static volatile DWORD g_stBuzzUntil;
bool *PistolSteadyBuzz() { return &g_stBuzz; }
float TwoHandSteadyBuzz() { return (LONG)(g_stBuzzUntil - GetTickCount()) > 0 ? 0.5f : 0.0f; }
int *PistolGunOwn() { return &g_pistolOwn; }
static bool PistolName(const char *n) {
    static const char *const ks[] = {"1911", "6P9", "D50", "Desert", "Magnum", "P226", "Pistol", "PISTOL", "FLARE", "Flare", "Shark", "Auto9",
                                      "AUTO9"}  /* bd54: Blood Dragon's pistol is DLC_BD_AUTO9 */;
    for (const char *k : ks)
        if (strstr(n, k)) return true;
    return false;
}
bool PistolGunByName(const char *n) { return n && PistolName(n); }

static void Rotate(float m[16], const float n[3], float a) {
    float c = cosf(a), s = sinf(a), t = 1 - c, x = n[0], y = n[1], z = n[2];
    float Q[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                  t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                  t * x * z - s * y, t * y * z + s * x, t * z * z + c};
    float r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = Q[i * 3] * m[j] + Q[i * 3 + 1] * m[4 + j] + Q[i * 3 + 2] * m[8 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = r[i * 3 + j];
}

// headtrack.cpp HeadtrackHand(1): turn the right hand's rotation m (offset offR) for two hands.
void TwoHandApply(float m[16], const float offR[3]) {
    AcquireSRWLockShared(&g_lock);
    bool held = g_held, blend = g_blend;
    float L[3] = {g_L[0], g_L[1], g_L[2]}, n[3] = {g_lastN[0], g_lastN[1], g_lastN[2]}, a = g_lastA;
    DWORD rel = g_relT;
    ReleaseSRWLockShared(&g_lock);
    if (held && g_stockAim) return;  // stockaim1: the right controller aims
    if (held) {
        float lm[16], offL[3];
        if (!HeadtrackHandRaw(0, lm, offL)) return;
        float d[3] = {offL[0] - offR[0], offL[1] - offR[1], offL[2] - offR[2]};
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 0.06f) return;  // hands together: no clear line
        float b[3] = {d[0] / len, d[1] / len, d[2] / len}, v[3];
        for (int i = 0; i < 3; ++i) v[i] = m[i * 4] * L[0] + m[i * 4 + 1] * L[1] + m[i * 4 + 2] * L[2];
        float k[3] = {v[1] * b[2] - v[2] * b[1], v[2] * b[0] - v[0] * b[2], v[0] * b[1] - v[1] * b[0]};
        float s = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = v[0] * b[0] + v[1] * b[1] + v[2] * b[2];
        if (s < 1e-6f) return;
        float nn[3] = {k[0] / s, k[1] / s, k[2] / s}, ang = atan2f(s, c);
        Rotate(m, nn, ang);
        AcquireSRWLockExclusive(&g_lock);
        if (g_held) memcpy(g_lastN, nn, 12), g_lastA = ang;
        ReleaseSRWLockExclusive(&g_lock);
    } else if (blend) {
        DWORD dt = GetTickCount() - rel;
        if (dt >= 120) {
            // safe1 (review S13): under the lock, and only for the letting-go this blend belongs to
            AcquireSRWLockExclusive(&g_lock);
            if (g_relT == rel) g_blend = false;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        Rotate(m, n, a * (1 - dt / 120.f));
    }
}

float TwoHandGunReach();  // grabreach1
// Input thread (input.cpp), each pad read. True = the left grip is ours (don't send LB).
bool TwoHandInput(const VrPad &p) {
    static bool s_grip, s_own;
    bool grip = p.lgrip > (s_grip ? 0.35f : 0.5f);  // 0.5 = when the game would get LB
    bool press = grip && !s_grip;
    s_grip = grip;
    bool ok = g_on && p.handValid[0] && p.handValid[1] && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() &&
              !MenuOpen() && !GamePaused() && GunAttachName()[0] && !BowOut();
    if (press && ok) {
        float R[16], lm[16], offR[3], offL[3];
        if (HeadtrackHandRaw(1, R, offR) && HeadtrackHandRaw(0, lm, offL)) {
            float d[3] = {offL[0] - offR[0], offL[1] - offR[1], offL[2] - offR[2]};
            float f[3] = {-R[2], -R[6], -R[10]};  // controller forward (-z)
            // grabreach1 (DrGonzaga 21 Nov, minigun: "I'd like to be able to press left grip further forward than usual, and
            // have it register as a grip on the gun"): bd117 turned the grab line by the gun's angle sliders, which made
            // the minigun's misses further off (18-23 cm to the side, was 12-14), so the line is the controller's own
            // forward again and each gun can have its own reach to the side ([twohand.<gun>] GrabReach; the Minigun 30 cm).
            const float reach = TwoHandGunReach();
            float along = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
            float lat[3] = {d[0] - along * f[0], d[1] - along * f[1], d[2] - along * f[2]};
            float side = sqrtf(lat[0] * lat[0] + lat[1] * lat[1] + lat[2] * lat[2]);
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (along > 0.08f && along < g_far && side < reach && len > 0.06f) {
                AcquireSRWLockExclusive(&g_lock);
                for (int i = 0; i < 3; ++i)  // R^T * direction: into the controller's frame
                    g_L[i] = (R[i] * d[0] + R[4 + i] * d[1] + R[8 + i] * d[2]) / len;
                g_held = true, g_blend = false, g_lastA = 0;
                ReleaseSRWLockExclusive(&g_lock);
                s_own = true;
                Log("twohand: holding with both hands (%.0f cm in front, %.0f cm to the side)", along * 100, side * 100);
            } else if (len < g_cup && !(g_stOn && g_pistolGun)) {  // steady1: pistols steady without the grip
                g_cupHeld = true, s_own = true;
                Log("twohand: hands together (%.0f cm apart): steadied, aim unchanged", len * 100);
            } else Log("twohand: left grip but no hold: left hand %.0f cm along the gun, %.0f cm to the side of it (needs 8 to %.0f along, under %.0f to the side)",
                       along * 100, side * 100, g_far * 100, reach * 100);  // grabline1
        }
    }
    if (g_held && (!grip || !ok)) {
        AcquireSRWLockExclusive(&g_lock);
        g_held = false, g_blend = true, g_relT = GetTickCount();
        ReleaseSRWLockExclusive(&g_lock);
        LogV("twohand: let go");
    }
    if (g_cupHeld && (!grip || !ok)) {
        g_cupHeld = false;
        LogV("twohand: hands apart");
    }
    // steady1: hands together with a pistol, no grip needed (15 cm on, 19 cm off)
    bool nearNow = false;
    if (g_stOn && g_pistolGun && ok && !g_held && g_cup > 0.005f) {
        float R[16], lm[16], offR[3], offL[3];
        if (HeadtrackHandRaw(1, R, offR) && HeadtrackHandRaw(0, lm, offL)) {
            float d[3] = {offL[0] - offR[0], offL[1] - offR[1], offL[2] - offR[2]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            nearNow = len < (g_near ? g_cup + 0.04f : g_cup);
        }
    }
    if (nearNow != g_near) {
        if (nearNow && g_stBuzz) g_stBuzzUntil = GetTickCount() + 70;  // steadybuzz1: a tick in both hands as steadying starts
        g_near = nearNow;
        LogV("twohand: pistol %s", nearNow ? "steadied (hands together)" : "one hand");
    }
    if (!grip) s_own = false;
    return s_own;
}

// ---- twosmooth1 (DrGonzaga 10 Nov): extra smoothing while both grips hold the weapon ----
// Two-handed holding on a gun, or the bow held in the left grip while the right grip
// holds the string. Global strength (Two handed mode page), optionally per weapon
// ([twohand.<gun>] Smoothing). Rotation only, so the gun stays in your hand. It eases
// in and out over 150 ms, so taking hold or letting go doesn't jump.
static float g_smooth = 0.3f;  // 0..1: time constant up to 150 ms
static char g_smGun[48];
static float g_smGunVal = -1;  // -1 = this gun uses the global value
static volatile float g_grabGun = -1;  // grabreach1: this gun's reach to the side (m), -1 = the gun's default
float *TwoHandSmoothing() { return &g_smooth; }
const char *TwoHandSmoothGun() { return g_smGun; }
float *TwoHandSmoothGunValue() { return &g_smGunVal; }
static void SmoothGunIni(bool save) {
    if (!g_smGun[0]) return;
    wchar_t path[MAX_PATH], sec[80], buf[32];
    IniPath(path);
    swprintf(sec, 80, L"twohand.%hs", g_smGun);
    if (save) {
        if (g_pistolOwn < 0) WritePrivateProfileStringW(sec, L"PistolSteady", nullptr, path);  // steady1
        else WritePrivateProfileStringW(sec, L"PistolSteady", g_pistolOwn ? L"1" : L"0", path);
        g_pistolGun = g_pistolOwn >= 0 ? g_pistolOwn : PistolName(g_smGun);
        if (g_grabGun < 0) WritePrivateProfileStringW(sec, L"GrabReach", nullptr, path);  // grabreach1
        else swprintf(buf, 32, L"%.3f", g_grabGun), WritePrivateProfileStringW(sec, L"GrabReach", buf, path);
        if (g_smGunVal < 0) WritePrivateProfileStringW(sec, L"Smoothing", nullptr, path);
        else swprintf(buf, 32, L"%.3f", g_smGunVal), WritePrivateProfileStringW(sec, L"Smoothing", buf, path);
        return;
    }
    g_smGunVal = GetPrivateProfileStringW(sec, L"Smoothing", L"", buf, 32, path) ? (float)_wtof(buf) : -1;
    g_grabGun = GetPrivateProfileStringW(sec, L"GrabReach", L"", buf, 32, path) ? (float)_wtof(buf) : -1;  // grabreach1
    g_pistolOwn = GetPrivateProfileStringW(sec, L"PistolSteady", L"", buf, 32, path) ? (_wtoi(buf) != 0) : -1;  // steady1
    g_pistolGun = g_pistolOwn >= 0 ? g_pistolOwn : PistolName(g_smGun);
    if (g_pistolGun) Log("twohand: %s is steadied as a pistol with the hands together", g_smGun);
}
void TwoHandSmoothSave() { SmoothGunIni(true); }
// grabreach1: the gun in hand's grab reach to the side; -1 in the setting = its default (Minigun 30 cm, others the global)
float *TwoHandGunReachSetting() { return (float *)&g_grabGun; }
float TwoHandGunReachDefault() { return strstr(g_smGun, "Minigun") ? 0.30f : g_reach; }
float TwoHandGunReach() { return g_grabGun >= 0 ? g_grabGun : TwoHandGunReachDefault(); }

static void MatToQuat(const float m[16], double q[4]) {
    double t = m[0] + m[5] + m[10];
    if (t > 0) {
        double s = sqrt(t + 1) * 2;
        q[3] = s / 4, q[0] = (m[9] - m[6]) / s, q[1] = (m[2] - m[8]) / s, q[2] = (m[4] - m[1]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        double s = sqrt(1 + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[9] - m[6]) / s, q[0] = s / 4, q[1] = (m[1] + m[4]) / s, q[2] = (m[2] + m[8]) / s;
    } else if (m[5] > m[10]) {
        double s = sqrt(1 + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[2] - m[8]) / s, q[0] = (m[1] + m[4]) / s, q[1] = s / 4, q[2] = (m[6] + m[9]) / s;
    } else {
        double s = sqrt(1 + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[4] - m[1]) / s, q[0] = (m[2] + m[8]) / s, q[1] = (m[6] + m[9]) / s, q[2] = s / 4;
    }
}
static void QuatToMat(const double q[4], float m[16]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (float)(1 - 2 * (y * y + z * z)), m[1] = (float)(2 * (x * y - z * w)), m[2] = (float)(2 * (x * z + y * w));
    m[4] = (float)(2 * (x * y + z * w)), m[5] = (float)(1 - 2 * (x * x + z * z)), m[6] = (float)(2 * (y * z - x * w));
    m[8] = (float)(2 * (x * z - y * w)), m[9] = (float)(2 * (y * z + x * w)), m[10] = (float)(1 - 2 * (x * x + y * y));
}

// headtrack.cpp HeadtrackHand(1), after BowApply / TwoHandApply.
// bowsteady1 (DrGonzaga 24 Nov: "add bow smoothing when holding left trigger"): with the bow in hand, the left trigger
// held smooths the aim more ([bow] SteadySmoothing, 0..1 like the two-handed smoothing; 0 = off).
static float g_bowSteady = 0.8f;
float *BowSteadySmoothing() { return &g_bowSteady; }
void TwoHandSmooth(float m[16]) {
    static SRWLOCK s_lock = SRWLOCK_INIT;
    static double s_q[4], s_out[4];
    static LARGE_INTEGER s_t;
    static bool s_have, s_was;
    static DWORD s_edge;  // when both-grips holding last started or ended
    bool both = TwoHandHeld(), bowLt = false;
    if (BowActive()) {
        VrPad p;
        if (XrGetPad(&p)) {
            if (!both) both = p.rgrip > 0.5f;
            bowLt = p.lt > 0.5f && g_bowSteady > 0.005f;  // bowsteady1
            if (bowLt) both = true;
        }
    }
    AcquireSRWLockExclusive(&s_lock);
    const char *n = GunAttachName();
    if (n && strcmp(n, g_smGun)) {
        snprintf(g_smGun, sizeof g_smGun, "%s", n);
        if (n[0]) SmoothGunIni(false);
        else g_pistolGun = 0, g_pistolOwn = -1;  // steady1: no gun in hand
    }
    float k = g_smGunVal >= 0 ? g_smGunVal : g_smooth;
    static bool s_bowLtWas;
    static DWORD s_ltEdge;
    if (bowLt != s_bowLtWas) s_bowLtWas = bowLt, s_ltEdge = GetTickCount();
    if (bowLt || GetTickCount() - s_ltEdge < 150) {  // bowsteady1: eases between the two strengths over 150 ms
        float r = (GetTickCount() - s_ltEdge) / 150.f;
        r = r > 1 ? 1 : r;
        float hi = g_bowSteady > k ? g_bowSteady : k;
        k = bowLt ? k + (hi - k) * r : hi + (k - hi) * r;
    }
    DWORD now = GetTickCount();
    if (both != s_was) s_was = both, s_edge = now;
    float ramp = (now - s_edge) / 150.f;
    if (ramp > 1) ramp = 1;
    k *= both ? ramp : 1 - ramp;  // eases in after taking hold, out after letting go
    if (k <= 0.005f) {
        s_have = false;
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    double q[4];
    MatToQuat(m, q);
    LARGE_INTEGER t, fq;
    QueryPerformanceCounter(&t), QueryPerformanceFrequency(&fq);
    double dt = s_have ? (double)(t.QuadPart - s_t.QuadPart) / fq.QuadPart : 1;
    if (!s_have || dt > 0.25) {
        memcpy(s_q, q, sizeof q), s_have = true, s_t = t;
    } else if (dt >= 0.002) {  // as SmoothHand: step at most every 2 ms, so near-simultaneous calls agree
        s_t = t;
        double a = 1 - exp(-dt / (k * 0.15)), d = 0, l = 0;
        for (int i = 0; i < 4; ++i) d += s_q[i] * q[i];
        double sg = d < 0 ? -1 : 1;
        for (int i = 0; i < 4; ++i) s_q[i] += (sg * q[i] - s_q[i]) * a, l += s_q[i] * s_q[i];
        l = sqrt(l);
        for (int i = 0; i < 4; ++i) s_q[i] /= l;
    } else {
        QuatToMat(s_out, m);
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    memcpy(s_out, s_q, sizeof s_q);
    QuatToMat(s_out, m);
    ReleaseSRWLockExclusive(&s_lock);
}

// ---- steady1: adaptive (One Euro) filter on the pistol's turn while the hands are together ----
// headtrack.cpp HeadtrackHand(1), after TwoHandSmooth. Runs whenever a pistol is out so it
// is warm, and blends in/out over 150 ms when the hands come together or part.
// Cut-off = PistolSteadyHz + PistolSwing x turn speed (rad/s), the turn speed itself
// filtered at 1 Hz (Casiez et al. 2012). Rotation only: the gun stays in the hand.
void PistolSteadyFilter(float m[16]) {
    static SRWLOCK s_lock = SRWLOCK_INIT;
    static double s_q[4], s_raw[4], s_spd, s_out[4];
    static LARGE_INTEGER s_t;
    static bool s_have, s_was;
    static DWORD s_edge;
    if (!g_stOn || !g_pistolGun) {
        s_have = false;
        return;
    }
    bool on = g_near;
    AcquireSRWLockExclusive(&s_lock);
    DWORD now = GetTickCount();
    if (on != s_was) s_was = on, s_edge = now;
    float w = (now - s_edge) / 150.f;
    if (w > 1) w = 1;
    if (!on) w = 1 - w;
    double q[4];
    MatToQuat(m, q);
    LARGE_INTEGER t, fq;
    QueryPerformanceCounter(&t), QueryPerformanceFrequency(&fq);
    double dt = s_have ? (double)(t.QuadPart - s_t.QuadPart) / fq.QuadPart : 1;
    if (!s_have || dt > 0.25) {
        memcpy(s_q, q, sizeof q), memcpy(s_raw, q, sizeof q), s_spd = 0, s_have = true, s_t = t;
    } else if (dt >= 0.002) {  // as SmoothHand: step at most every 2 ms
        s_t = t;
        double d = 0;
        for (int i = 0; i < 4; ++i) d += s_raw[i] * q[i];
        if (d < 0) d = -d;
        if (d > 1) d = 1;
        double spd = 2 * acos(d) / dt;  // rad/s of the raw controller
        memcpy(s_raw, q, sizeof q);
        const double kPi = 3.14159265358979;
        double aD = 1 / (1 + 1 / (2 * kPi * 1.0 * dt));
        s_spd += (spd - s_spd) * aD;
        double fc = g_stMinHz + g_stBeta * s_spd;
        double a = 1 / (1 + 1 / (2 * kPi * fc * dt)), dd = 0, l = 0;
        for (int i = 0; i < 4; ++i) dd += s_q[i] * q[i];
        double sg = dd < 0 ? -1 : 1;
        for (int i = 0; i < 4; ++i) s_q[i] += (sg * q[i] - s_q[i]) * a, l += s_q[i] * s_q[i];
        l = sqrt(l);
        for (int i = 0; i < 4; ++i) s_q[i] /= l;
    } else {
        if (w > 0.001f) QuatToMat(s_out, m);
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    // blend raw -> filtered by w (eases in and out)
    double dd = 0, l = 0;
    for (int i = 0; i < 4; ++i) dd += s_q[i] * q[i];
    double sg = dd < 0 ? -1 : 1;
    for (int i = 0; i < 4; ++i) s_out[i] = q[i] + (sg * s_q[i] - q[i]) * w, l += s_out[i] * s_out[i];
    l = sqrt(l);
    for (int i = 0; i < 4; ++i) s_out[i] /= l;
    if (w > 0.001f) QuatToMat(s_out, m);
    ReleaseSRWLockExclusive(&s_lock);
}
