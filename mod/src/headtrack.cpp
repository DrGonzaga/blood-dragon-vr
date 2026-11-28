// Head pose: the headset's rotation and position relative to where the player
// recentred, handed to the engine camera hook (engine.cpp), plus the hand poses.
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fc3vr.h"

// ---- Small matrix helpers (column-vector convention: x' = M * x) ----

struct Mat {
    float m[4][4];
};

// Rotation matrix from a unit quaternion (x, y, z, w).
static Mat FromQuat(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat r = {};
    r.m[0][0] = 1 - 2 * (y * y + z * z);
    r.m[0][1] = 2 * (x * y - z * w);
    r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);
    r.m[1][1] = 1 - 2 * (x * x + z * z);
    r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);
    r.m[2][1] = 2 * (y * z + x * w);
    r.m[2][2] = 1 - 2 * (x * x + y * y);
    r.m[3][3] = 1;
    return r;
}

static void QuatMul(const float a[4], const float b[4], float out[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x, out[1] = y, out[2] = z, out[3] = w;
}

// ---- Head pose ----

static bool g_enabled = true;
static bool g_haveRef;
static float g_refInv[4] = {0, 0, 0, 1};  // inverse of the yaw we recentred on
static float g_refPos[3];                  // head position we recentred on
static volatile LONG g_recGen;              // flat1: +1 at each recentre
static bool g_wantPosRef;
static Mat g_head;                        // this frame's head rotation, game eye space
static bool g_headValid;

static void Recenter(const float q[4]) {
    // Keep only yaw, so recentring never tilts the horizon.
    float fx = -2 * (q[0] * q[2] + q[1] * q[3]);  // forward = q * (0,0,-1)
    float fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
    float yaw = atan2f(-fx, -fz);
    g_refInv[0] = 0, g_refInv[1] = -sinf(yaw / 2), g_refInv[2] = 0, g_refInv[3] = cosf(yaw / 2);
    g_haveRef = true;
    InterlockedIncrement(&g_recGen);  // flat1
    g_wantPosRef = true;
    Log("head: recentred (yaw %.1f deg)", yaw * 57.2958f);
}

// flat1: the recentred frame (where the menu panel lives) in headset space:
// headset = pos + (turned by yaw about up) recentred.
void HeadtrackRefPose(float *yaw, float pos[3]) {
    *yaw = -2.0f * atan2f(g_refInv[1], g_refInv[3]);
    memcpy(pos, g_refPos, 12);
}
LONG HeadtrackRecentreGen() { return g_recGen; }

static volatile bool g_recenterReq;
void HeadtrackRequestRecenter() { g_recenterReq = true; }

// autocentre1: recentre 2 s after the game camera comes back from a long break
// (loading screen, main menu): no engine camera for 3+ s, then 2 s of play.
static bool g_autoRecenter = true;
bool *HeadtrackAutoRecenter() { return &g_autoRecenter; }
static bool AutoRecenterDue() {
    static DWORD s_outSince, s_playSince;
    DWORD now = GetTickCount();
    if (!EngineActive()) {
        if (!s_outSince) s_outSince = now ? now : 1;
        s_playSince = 0;
        return false;
    }
    if (!s_outSince) return false;
    if (now - s_outSince < 3000 && !s_playSince) {  // a short break: not a load
        s_outSince = 0;
        return false;
    }
    if (!s_playSince) s_playSince = now ? now : 1;
    if (now - s_playSince < 2000) return false;
    s_outSince = s_playSince = 0;
    if (!g_autoRecenter) return false;
    Log("head: auto recentre after loading");
    EngineAfterLoadRecentre();  // loadface1
    return true;
}

static bool KeyPressed(int vk) {
    return KeyHit(vk);  // keys1
}


// room1: move the recentred head position by (x, z) metres in the recentred
// frame (x right, z back), as the player's body walks after the head: the
// view and hands stay where they are in the world while the offset shrinks.
void HeadtrackShiftAnchor(float x, float z) {
    float y = g_refInv[1], w = g_refInv[3];
    float c = 1 - 2 * y * y, sn = 2 * y * w;
    g_refPos[0] += c * x - sn * z;
    g_refPos[2] += sn * x + c * z;
}

// smooth1: controller smoothing (Aim page). Each new controller pose is
// blended into a smoothed one with a time constant of up to 80 ms (rotation
// by normalised blend, position likewise), once per new pose whoever asks.
static float g_handSmooth;
float *HeadtrackHandSmoothing() { return &g_handSmooth; }
static void SmoothHand(int hand, float q[4], float pos[3]) {
    static SRWLOCK s_lock = SRWLOCK_INIT;
    static double s_q[2][4], s_p[2][3];
    static float s_outQ[2][4], s_outP[2][3];
    static LARGE_INTEGER s_t[2];
    static bool s_have[2];
    float k = g_handSmooth;
    if (k <= 0.001f) return;
    AcquireSRWLockExclusive(&s_lock);
    // still3 (DrGonzaga 2 Nov: gun sticks when held very still, and only with
    // smoothing on): the gun, laser, aim and spread code all ask for the hand
    // within microseconds of each other, so each call moved the smoothed pose by
    // a tiny fraction. Steps that small were lost to float rounding (and the
    // quaternion renormalising), so it froze until the hand had turned well away:
    // a dead zone. Now the blend steps at most every 2 ms (calls in between get
    // the same result) and the state is kept in doubles.
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
    double dt = s_have[hand] ? (double)(now.QuadPart - s_t[hand].QuadPart) / fq.QuadPart : 1;
    if (!s_have[hand] || dt > 0.25) {
        for (int i = 0; i < 4; ++i) s_q[hand][i] = q[i];
        for (int i = 0; i < 3; ++i) s_p[hand][i] = pos[i];
        s_have[hand] = true, s_t[hand] = now;
    } else if (dt >= 0.002) {
        s_t[hand] = now;
        double a = 1 - exp(-dt / (k * 0.08));
        double d = s_q[hand][0] * q[0] + s_q[hand][1] * q[1] + s_q[hand][2] * q[2] + s_q[hand][3] * q[3];
        double sg = d < 0 ? -1 : 1, l = 0;
        for (int i = 0; i < 4; ++i) s_q[hand][i] += (sg * q[i] - s_q[hand][i]) * a, l += s_q[hand][i] * s_q[hand][i];
        l = sqrt(l);
        for (int i = 0; i < 4; ++i) s_q[hand][i] /= l;
        for (int i = 0; i < 3; ++i) s_p[hand][i] += (pos[i] - s_p[hand][i]) * a;
    } else {
        memcpy(q, s_outQ[hand], 16), memcpy(pos, s_outP[hand], 12);
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    for (int i = 0; i < 4; ++i) q[i] = s_outQ[hand][i] = (float)s_q[hand][i];
    for (int i = 0; i < 3; ++i) pos[i] = s_outP[hand][i] = (float)s_p[hand][i];
    ReleaseSRWLockExclusive(&s_lock);
}

// A hand's aim rotation in the same recentred frame as the head (for motion aiming).
bool HeadtrackHandRaw(int hand, float m[16], float off[3]) {
    VrPad p;
    if (!g_haveRef || !XrGetPad(&p) || !p.handValid[hand]) return false;
    SmoothHand(hand, p.handQ[hand], p.handP[hand]);
    float rel[4];
    QuatMul(g_refInv, p.handQ[hand], rel);
    Mat r = FromQuat(rel);
    memcpy(m, &r.m[0][0], 64);
    if (off) {  // like the head's offset: from the recentred head position, turned by the recentre yaw
        float d[3] = {p.handP[hand][0] - g_refPos[0], p.handP[hand][1] - g_refPos[1], p.handP[hand][2] - g_refPos[2]};
        float y = g_refInv[1], w = g_refInv[3];
        float c = 1 - 2 * y * y, sn = 2 * y * w;
        off[0] = c * d[0] + sn * d[2];
        off[1] = d[1];
        off[2] = -sn * d[0] + c * d[2];
    }
    return true;
}
// mount1 (DrGonzaga 24 Nov: "a 'rotate gun' option for gunstock ... aim, laser, reticle, and where the sight is positioned
// on the gun, are all dependent on the current alignment"): a stock mount angle. Everything aimed (the game's look, the
// shots, beam, dot, sight window, rifle glass and eye check, scope camera, kick, the gun model) is built from
// HeadtrackHand(1), so one fixed turn here moves them all together; each gun's own place and angle stay as tuned on top.
// The turn is about the cheek point (where the eye sits on the stock, in the controller's own axes: x right, y up,
// z back), so the sight line still runs through the eye and every eyepiece stays at it. Default cheek point: from the
// first rifle-scoped gun in the ini (its eyepiece minus the eye distance); "Learn my cheek point" in the VR menu sets
// it by hand (6 s countdown). Both angles 0 = nothing here runs. [aim] StockMountPitch / StockMountYaw (degrees, front
// up / front right), StockCheekRight / StockCheekUp / StockCheekBack (m, only once learnt).
static float g_mount[2];
static float g_cheekLearnt[3], g_cheekRifle[3];
static bool g_haveLearnt, g_haveRifle, g_mountLoaded;
static char g_cheekGun[64];
static volatile DWORD g_cheekCapAt, g_cheekBuzzUntil;
static volatile LONG g_cheekCapOn;
static char g_cheekMsg[160];
static void MountLoad() {
    if (g_mountLoaded) return;
    g_mountLoaded = true;
    wchar_t path[MAX_PATH], buf[64];
    IniPath(path);
    static const wchar_t *const kA[2] = {L"StockMountPitch", L"StockMountYaw"};
    for (int i = 0; i < 2; ++i)
        if (GetPrivateProfileStringW(L"aim", kA[i], L"", buf, 64, path)) g_mount[i] = (float)_wtof(buf);
    static const wchar_t *const kC[3] = {L"StockCheekRight", L"StockCheekUp", L"StockCheekBack"};
    int got = 0;
    for (int i = 0; i < 3; ++i)
        if (GetPrivateProfileStringW(L"aim", kC[i], L"", buf, 64, path)) g_cheekLearnt[i] = (float)_wtof(buf), ++got;
    g_haveLearnt = got == 3;
    // The default: the first [scope.<gun>] with RifleScope=1 (sections in file order).
    static wchar_t names[32768];
    DWORD n = GetPrivateProfileSectionNamesW(names, 32768, path);
    for (wchar_t *q = names; n && *q && !g_haveRifle; q += wcslen(q) + 1) {
        if (_wcsnicmp(q, L"scope.", 6) || !GetPrivateProfileIntW(q, L"RifleScope", 0, path)) continue;
        float v[4] = {0, 0.075f, -0.03f, 0.07f};  // scope.cpp's defaults
        static const wchar_t *const kR[4] = {L"EyeRight", L"EyeUp", L"EyeForward", L"EyeDistance"};
        for (int i = 0; i < 4; ++i)
            if (GetPrivateProfileStringW(q, kR[i], L"", buf, 64, path)) v[i] = (float)_wtof(buf);
        g_cheekRifle[0] = v[0], g_cheekRifle[1] = v[1], g_cheekRifle[2] = -(v[2] - v[3]);  // eye = glass - forward * distance
        snprintf(g_cheekGun, sizeof g_cheekGun, "%ls", q + 6);
        g_haveRifle = true;
    }
    Log("stock mount: turn %.2f deg front up, %.2f deg front right; cheek point %s (%.3f right, %.3f up, %.3f back m)%s%s",
        g_mount[0], g_mount[1], g_haveLearnt ? "learnt" : g_haveRifle ? "from the rifle scope of" : "none, turning about the controller",
        g_haveLearnt ? g_cheekLearnt[0] : g_cheekRifle[0], g_haveLearnt ? g_cheekLearnt[1] : g_cheekRifle[1],
        g_haveLearnt ? g_cheekLearnt[2] : g_cheekRifle[2], !g_haveLearnt && g_haveRifle ? " " : "", !g_haveLearnt && g_haveRifle ? g_cheekGun : "");
}
void MountSave() {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.2f", g_mount[0]), WritePrivateProfileStringW(L"aim", L"StockMountPitch", buf, path);
    swprintf(buf, 32, L"%.2f", g_mount[1]), WritePrivateProfileStringW(L"aim", L"StockMountYaw", buf, path);
    static const wchar_t *const kC[3] = {L"StockCheekRight", L"StockCheekUp", L"StockCheekBack"};
    for (int i = 0; i < 3; ++i) {
        if (g_haveLearnt) swprintf(buf, 32, L"%.4f", g_cheekLearnt[i]);
        WritePrivateProfileStringW(L"aim", kC[i], g_haveLearnt ? buf : nullptr, path);
    }
    Log("stock mount: saved %.2f deg front up, %.2f deg front right, cheek point %s", g_mount[0], g_mount[1], g_haveLearnt ? "learnt" : "default");
}
float *MountAngle(int i) { MountLoad(); return &g_mount[i ? 1 : 0]; }
// 0 = turning about the controller, 1 = cheek point from a rifle scope, 2 = learnt
int MountCheekKind(float out[3], const char **gun) {
    MountLoad();
    if (gun) *gun = g_cheekGun;
    if (g_haveLearnt) {
        if (out) memcpy(out, g_cheekLearnt, 12);
        return 2;
    }
    if (g_haveRifle) {
        float s = 1;
        EngineView v;
        if (EngineLastView(&v) && v.scale > 0.1f) s = v.scale;  // the eyepiece values are game-world units
        if (out) for (int i = 0; i < 3; ++i) out[i] = g_cheekRifle[i] * s;
        return 1;
    }
    if (out) out[0] = out[1] = out[2] = 0;
    return 0;
}
void MountCheekDefault() {
    MountLoad();
    g_haveLearnt = false;
    MountSave();
    snprintf(g_cheekMsg, sizeof g_cheekMsg, "Back to the default cheek point.");
}
void MountCheekLearnIn(float s) {
    g_cheekCapAt = GetTickCount() + (DWORD)(s * 1000), g_cheekCapOn = 1;
    g_cheekMsg[0] = 0;
}
float MountCheekLearnLeft() { return g_cheekCapOn ? (float)(LONG)(g_cheekCapAt - GetTickCount()) / 1000.0f : 0; }
const char *MountCheekMsg() { return g_cheekMsg; }
float MountBuzz() {  // both hands: a tick each second of the countdown, a long one at the capture
    DWORD t = GetTickCount();
    if ((LONG)(g_cheekBuzzUntil - t) > 0) return 0.8f;
    LONG d = (LONG)(g_cheekCapAt - t);
    return g_cheekCapOn && d > 0 && d % 1000 > 920 ? 0.4f : 0;
}
// Once a frame (HeadtrackOnPresent): the countdown. The eye's place in the real controller's axes.
static void MountCheekTick() {
    if (!g_cheekCapOn || (LONG)(g_cheekCapAt - GetTickCount()) > 0) return;
    g_cheekCapOn = 0, g_cheekBuzzUntil = GetTickCount() + 400;
    float m[16], ho[3], hm[16], hp[3];
    if (!HeadtrackHandRaw(1, m, ho) || !EngineHeadPose(hm, hp)) {
        snprintf(g_cheekMsg, sizeof g_cheekMsg, "Not learnt: the controller or headset wasn't tracked.");
        Log("stock mount: cheek point not learnt (no controller or head pose)");
        return;
    }
    float d[3] = {hp[0] - ho[0], hp[1] - ho[1], hp[2] - ho[2]}, k[3];
    for (int j = 0; j < 3; ++j) k[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];  // M^T d: the controller's own axes
    float len = sqrtf(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    if (len > 0.8f || k[2] < 0) {  // the eye must be behind the controller, within 80 cm
        snprintf(g_cheekMsg, sizeof g_cheekMsg, "Not learnt: your eye was %.0f cm %s the controller. Hold your normal aim and try again.", len * 100,
                 k[2] < 0 ? "in front of" : "from");
        Log("stock mount: cheek point not learnt: %.3f right, %.3f up, %.3f back m (%.0f cm away)", k[0], k[1], k[2], len * 100);
        return;
    }
    memcpy(g_cheekLearnt, k, 12), g_haveLearnt = true;
    MountSave();
    snprintf(g_cheekMsg, sizeof g_cheekMsg, "Learnt: %.0f cm behind, %.0f cm above, %.0f cm right of the controller.", k[2] * 100, k[1] * 100, k[0] * 100);
    Log("stock mount: cheek point learnt: %.3f right, %.3f up, %.3f back m", k[0], k[1], k[2]);
}
static bool MountApplies() {
    if (!g_mountLoaded) MountLoad();
    if (g_mount[0] == 0 && g_mount[1] == 0) return false;
    // gunstock aiming only, on foot, not with the bow (left hand), C4 (left hand) or a mounted gun
    return *TwoHandStockAim() && !BowActive() && EngineOnFoot() && !EngineInVehicle() && !EngineTurret();
}
// m: the hand's rotation (columns = its axes), o: its place; turned by the mount angle about the cheek point.
static void MountTurn(float m[16], float o[3]) {
    const float p = g_mount[0] * 0.0174533f, y = -g_mount[1] * 0.0174533f;  // +yaw = front to the right
    const float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y);
    // R = Ry * Rx in the controller's axes: pitch about its right (front up), then yaw about its up
    const float R[9] = {cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
    float k[3];
    MountCheekKind(k, nullptr);
    float dk[3];
    for (int i = 0; i < 3; ++i) dk[i] = k[i] - (R[i * 3] * k[0] + R[i * 3 + 1] * k[1] + R[i * 3 + 2] * k[2]);
    for (int i = 0; i < 3; ++i) o[i] += m[i * 4] * dk[0] + m[i * 4 + 1] * dk[1] + m[i * 4 + 2] * dk[2];
    float n[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) n[i * 3 + j] = m[i * 4] * R[j] + m[i * 4 + 1] * R[3 + j] + m[i * 4 + 2] * R[6 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = n[i * 3 + j];
}
// twohand1: the right hand's aim turns to the line between the hands while both hold the gun.
static bool HandAim(int hand, float m[16], float off[3], bool mount) {
    float o[3];
    // c4hands1: with C4 out the left hand (the charge, thrown with the left trigger) aims
    if (hand == 1 && GunAttachC4Split()) return HeadtrackHandRaw(0, m, off ? off : o);
    if (!HeadtrackHandRaw(hand, m, o)) return false;
    if (hand == 1) {
        if (BowActive()) BowApply(m, o);  // bow1p: the bow is in the left hand
        else TwoHandApply(m, o);
        TwoHandSmooth(m);  // twosmooth1: both grips holding (gun or bow)
        PistolSteadyFilter(m);  // steady1: pistol with the hands together
        if (mount && MountApplies()) MountTurn(m, o);  // mount1
    }
    if (off) memcpy(off, o, 12);
    return true;
}
bool HeadtrackHand(int hand, float m[16], float off[3]) { return HandAim(hand, m, off, true); }
// mount1: the same without the stock mount angle (gunstock distances measure the real controller)
bool HeadtrackHandUnturned(int hand, float m[16], float off[3]) { return HandAim(hand, m, off, false); }

void HeadtrackOnPresent() {
    if (KeyPressed(VK_F8)) {
        g_enabled = !g_enabled;
        Log("head: tracking %s (F8)", g_enabled ? "ON" : "OFF");
    }
    static volatile bool &req = g_recenterReq;
    bool recenter = KeyPressed(VK_F9) || req;
    if (AutoRecenterDue()) recenter = true;
    req = false;

    float q[4];
    g_headValid = false;
    if (!XrGetHeadOrientation(q)) return;
    if (!g_haveRef || recenter) Recenter(q);

    float rel[4];
    QuatMul(g_refInv, q, rel);
    // Test 3: the game's eye space matches OpenXR's (x right, y up, looking down -z), so no mirroring.
    g_head = FromQuat(rel);
    g_headValid = true;

    static bool engineTried;
    if (!engineTried) engineTried = true, EngineInstall();
    MountCheekTick();  // mount1
    float pq[4], pp[3];
    XrGetLastPose(pq, pp);
    Mat h = g_head;
    if (!g_enabled) {
        static const float id[4] = {0, 0, 0, 1};
        h = FromQuat(id);
    }
    // Head position relative to where we recentred, in the recentred (eye-space) frame.
    if (g_wantPosRef) memcpy(g_refPos, pp, 12), g_wantPosRef = false;
    float d[3] = {pp[0] - g_refPos[0], pp[1] - g_refPos[1], pp[2] - g_refPos[2]}, off[3] = {0, 0, 0};
    if (g_enabled) {
        // Rotate by the yaw-only g_refInv: v' = v + 2w(u x v) + 2u x (u x v), u = (0, y, 0).
        float y = g_refInv[1], w = g_refInv[3];
        float c = 1 - 2 * y * y, sn = 2 * y * w;
        off[0] = c * d[0] + sn * d[2];
        off[1] = d[1];
        off[2] = -sn * d[0] + c * d[2];
    }
    // walkhands4 (DrGonzaga 28 Nov, bd172: hands fine, "but hud moved forward"; bd172 log: head up to 4.1 m from the body):
    // with your own stick pushed roomscale stands aside, so real steps took the head metres from the game body. The view
    // stops at 1 m, but the hands, HUD panels, holsters and menus all place from the full offset and ran on ahead. Now the
    // anchor is moved along (as roomscale does) so the level offset never passes 1 m: everything stays with the view.
    if (g_enabled && EngineOnFoot() && !EngineInVehicle()) {
        float sx = off[0] > 1 ? off[0] - 1 : off[0] < -1 ? off[0] + 1 : 0;
        float sz = off[2] > 1 ? off[2] - 1 : off[2] < -1 ? off[2] + 1 : 0;
        static DWORD s_logT;
        static int s_logs;
        if (sx != 0 || sz != 0) {
            if (GetTickCount() - s_logT > 5000 && s_logs < 30)
                ++s_logs, Log("head: %.2f m right, %.2f m back of the body: the room anchor moves along (kept within 1 m)", off[0], off[2]);
            s_logT = GetTickCount();
            HeadtrackShiftAnchor(sx, sz);
            off[0] -= sx, off[2] -= sz;
        }
    }
    EngineSetHead(&h.m[0][0], off, pq, pp);

    // quiet1: the 10 s "head:" counts line is gone (always 0 since the engine camera hook).
}

// cbgone1 (11 Nov, review F2): the constant-buffer head tracking that lived here
// (Map / Unmap / UpdateSubresource hooks that rebuilt the camera matrices in the
// shader buffers) is deleted. The engine camera hook (engine.cpp) turns the camera
// itself and supplies the pose and field of view for the headset since cb1.

// ---- Context hooks ----

// d3d11 builds each context's vtable on the heap, so every context we hook
// can have its own originals. Look them up by vtable.
static struct Hooked {
    void **vtable;
    void(STDMETHODCALLTYPE *exec)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
} g_hooked[8];
static LONG g_hookedCount;

static Hooked *Originals(ID3D11DeviceContext *ctx) {
    void **vt = *(void ***)ctx;
    for (LONG i = 0; i < g_hookedCount; ++i)
        if (g_hooked[i].vtable == vt) return &g_hooked[i];
    return &g_hooked[0];
}

bool HeadtrackActive() { return g_enabled && EngineActive(); }

void HeadtrackBeforeSubmit() { EngineOnPresent(); }

// Our own drawing (laser, menu) on this thread: its buffers aren't the game's camera.
static thread_local bool tl_own;
void HeadtrackOwnDraw(bool on) { tl_own = on; }
bool HeadtrackOwn() { return tl_own; }

static void STDMETHODCALLTYPE HookExec(ID3D11DeviceContext *ctx, ID3D11CommandList *list, BOOL restore) {
    bool prof = ctx == TraceContext() && StereoGpuProfiling();
    char name[56];
    if (prof) snprintf(name, sizeof name, "list in %s", TraceLastTargets()), StereoGpuMark(name);
    Originals(ctx)->exec(ctx, list, restore);
    if (ctx == TraceContext()) VrsContextReset();  // vrs1
    if (prof) snprintf(name, sizeof name, "after list in %s", TraceLastTargets()), StereoGpuMark(name);
}

void HeadtrackHookContext(ID3D11DeviceContext *ctx, const char *what) {
    void **vt = *(void ***)ctx;
    for (LONG i = 0; i < g_hookedCount; ++i)
        if (g_hooked[i].vtable == vt) return;
    if (g_hookedCount >= 8) return;
    Hooked &h = g_hooked[g_hookedCount];
    h.vtable = vt;
    bool ok = HookSlot(vt, 58, (void *)HookExec, (void **)&h.exec, "ExecuteCommandList");  // GPU profile marks only
    if (ok) InterlockedIncrement(&g_hookedCount);
    Log("head: %s context %p vtable %p, command-list hook %s. F8 toggles head tracking, F9 recentres.", what, (void *)ctx,
        (void *)vt, ok ? "installed" : "FAILED");
}
