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
// twohand1: the right hand's aim turns to the line between the hands while both hold the gun.
bool HeadtrackHand(int hand, float m[16], float off[3]) {
    float o[3];
    // c4hands1: with C4 out the left hand (the charge, thrown with the left trigger) aims
    if (hand == 1 && GunAttachC4Split()) return HeadtrackHandRaw(0, m, off ? off : o);
    if (!HeadtrackHandRaw(hand, m, o)) return false;
    if (hand == 1) {
        if (BowActive()) BowApply(m, o);  // bow1p: the bow is in the left hand
        else TwoHandApply(m, o);
        TwoHandSmooth(m);  // twosmooth1: both grips holding (gun or bow)
        PistolSteadyFilter(m);  // steady1: pistol with the hands together
    }
    if (off) memcpy(off, o, 12);
    return true;
}

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
