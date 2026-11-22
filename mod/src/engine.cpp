// Engine camera hook: put the head rotation into the game's render camera
// before the engine derives anything from it, so the sky, shadows, culling
// and reflections all see the same, head-turned camera.
//
// Found with the F11 camera hunt (28 Oct). In FC3_d3d11.dll (image base 0x10000000):
//   0x11503f60  camera UpdateMatrices(camera*), stdcall. Builds the world
//               matrix at +0xb0 from forward (+0x400), up (+0x40c) and
//               position (+0x1f0) (or from a matrix at +0xc when byte +8 is
//               set), then view, projection and the rest.
//   0x118ac710  builds the frame's render camera and calls UpdateMatrices
//               at 0x118ac9d5 (returns to 0x118ac9da).
//   0x115c1f01  the main render setup's call of 0x118ac710 (returns to
//               0x115c1f06); it then copies the camera to every viewport.
// The gameplay camera (aiming) is left alone.
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "fc3vr.h"

static const DWORD kImageBase = 0x10000000;
// The render camera's source (position +4, forward +0x64, up +0x70), noted on the way
// into the camera build for the aim work (BdBuildStub below).
extern "C" {
volatile DWORD g_srcCam;
volatile DWORD g_renderer;  // (set only by the Far Cry 3 hook; stays 0 in Blood Dragon)
}

static BYTE *g_mod;
static BYTE *g_tramp;

// Head rotation, double-buffered between the render thread (writer) and the
// game's camera code (reader).
struct Head {
    float m[16];  // column-vector rotation in eye space (x right, y up, -z forward)
    float off[3]; // head position since recentring, same space, metres
    float q[4], p[3];
};
static Head g_heads[2];
static volatile LONG g_headIdx = -1;

// What was rendered with which pose, so the headset shows each frame at the
// pose it was rendered for.
static struct {
    float fwd[3], pos[3];
    int eye;
    LONG seq;
    float q[4], p[3];
    float up[3], base[3], basis[9], posAxes[9];  // for drawing in this view (laser.cpp)
    float aimPos[3], aimFwd[3];  // turret1: the game's own camera before our changes (where the mounted gun aims)
    float oc[4];                 // oc1: this picture's off-centre zoom and shift (1, 0, 1, 0 = centred)
} g_ring[8];
static LONG g_eyeCount;
static volatile int g_forceEye = -1;  // render-twice stereo: the eye being drawn now, -1 = alternate
static volatile bool g_stereo = true;
static volatile bool g_6dof = true;
// How big the world looks: 1 = true size. Eye separation and head movement are
// divided by it. Numpad +/- change it by 5% and save it to fc3vr.ini [vr] WorldScale.
static float g_worldScale = 1.0f;

static void LoadWorldScale() {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    GetPrivateProfileStringW(L"vr", L"WorldScale", L"1.0", buf, 32, path);
    float v = (float)_wtof(buf);
    g_worldScale = v > 0.3f && v < 3.0f ? v : 1.0f;
    Log("engine: world scale %.2f", g_worldScale);
}

static void SaveWorldScale() {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.2f", g_worldScale);
    WritePrivateProfileStringW(L"vr", L"WorldScale", buf, path);
    Log("engine: world scale %.2f (saved)", g_worldScale);
}
static volatile LONG g_ringPos;
static volatile LONG g_ringDone;  // cb1 (review S7): the newest entry that is completely written; readers use this one
static volatile LONG g_calls, g_applied, g_flagged;
static DWORD g_lastApplied;

void EngineForceEye(int eye) { g_forceEye = eye; }

bool EngineLastEye(int *eye, float q[4], float p[3]) {
    LONG slot = g_ringDone & 7;
    if (!g_ringDone || g_ring[slot].eye < 0) return false;
    *eye = g_ring[slot].eye;
    memcpy(q, g_ring[slot].q, 16);
    memcpy(p, g_ring[slot].p, 12);
    return true;
}

// The view drawn last: eye position and axes, the camera before the head
// (position and level basis r, u, f), and the axes head offsets move along.
bool EngineLastView(EngineView *v) {
    LONG slot = g_ringDone & 7;
    if (!g_ringDone || GetTickCount() - g_lastApplied > 200) return false;
    memcpy(v->pos, g_ring[slot].pos, 12), memcpy(v->fwd, g_ring[slot].fwd, 12), memcpy(v->up, g_ring[slot].up, 12);
    memcpy(v->base, g_ring[slot].base, 12), memcpy(v->basis, g_ring[slot].basis, 36);
    memcpy(v->posAxes, g_ring[slot].posAxes, 36);
    v->scale = g_worldScale;
    v->sixdof = g_6dof;
    return true;
}

// turret1: where the game's camera was and looked (unlevelled) in the last view;
// on a mounted gun that is where the gun aims.
bool EngineTurretAim(float pos[3], float fwd[3]) {
    LONG slot = g_ringDone & 7;
    if (!g_ringDone || GetTickCount() - g_lastApplied > 200) return false;
    memcpy(pos, g_ring[slot].aimPos, 12), memcpy(fwd, g_ring[slot].aimFwd, 12);
    return true;
}

void EngineSetHead(const float m[16], const float off[3], const float q[4], const float p[3]) {
    LONG next = g_headIdx < 0 ? 0 : 1 - g_headIdx;
    memcpy(g_heads[next].m, m, 64);
    memcpy(g_heads[next].off, off, 12);
    memcpy(g_heads[next].q, q, 16);
    memcpy(g_heads[next].p, p, 12);
    InterlockedExchange(&g_headIdx, next);
}

// flat1: while a cutscene plays on the 2D screen the head isn't applied, but the
// engine hook still counts as live (so the old constant-buffer head patching stays off).
static volatile DWORD g_flatTick;
static volatile bool g_flatNow;
bool EngineFlat() { return g_flatNow; }
bool EngineActive() {
    DWORD t = GetTickCount();
    return g_tramp && (t - g_lastApplied < 1000 || (g_flatNow && t - g_flatTick < 1000));
}

// cyberdirect1: what the game's look is still short of the head (yaw, pitch), for the flat picture's camera
static volatile float g_cyErr[2];
static volatile DWORD g_cyErrT;
static void Cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void Normalize(float *v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// Camera field of view (found 28 Oct in UpdateMatrices): +0x228 horizontal
// FOV in radians, +0x18 width / height (1.778 at 16:9); P00 = cot(fov / 2), P11 = P00 * aspect.
static volatile bool g_vrFov = true;
// +0x22c is the "near" FOV the game draws the gun and arms with (set next to
// +0x228 by 0x118ac710 from the camera's fNearFovDegrees). Left at the game's
// flat-screen value it no longer matches the headset FOV, so the gun looks
// magnified and out of shape in VR. Matching it to the world FOV costs nothing.
static volatile bool g_gunFov = true;

// scope1 (31 Oct): scoped aiming. The game zooms by narrowing the camera's
// field of view below 40 degrees while left trigger is held with a scoped
// gun. With the scope eye on, the right eye's render becomes the scope view:
// along the gun (smoothed), at the game's zoom; the headset then shows the
// left image to both eyes and that view in a lens (scope.cpp).
static volatile bool g_scopeOn = true;
static float g_scopeSmooth = 0.4f;  // 0..1 (Aim page "Scope steadiness")
// lasers2 (DrGonzaga 10 Nov, two-handed: the gun showed inside the scope with it close to
// the face): the scope eye's camera sits this far ahead of the hand along the scope's
// line, past the gun's own model, instead of at the eye. 0 = at the eye (old).
static float g_scopeAhead = 0.9f;  // [aim] ScopeCameraAhead (m)
float *EngineScopeAhead() { return &g_scopeAhead; }
static float s_scopeHand[3];
static bool s_scopeHandOk;
static volatile float g_scopeFov;   // the game's zoomed fov, radians
static volatile DWORD g_scopeTick;  // last time the game asked for a zoomed fov
static bool s_scopeEyeNow;          // this camera is the scope view
// noscope1 (DrGonzaga 15 Nov: "an option to turn off scopes on a per-gun per-attachment basis (the bow and arrow scopes,
// the fazertron scopes ... they're not meant to scope)"): [sight.<gun>.<optic>] NoScope: the game's zoom is not a scope.
static volatile bool g_scopeGunOff;
bool *EngineScopeGunOff() { return (bool *)&g_scopeGunOff; }
static bool g_ladderTurn = true;  // ladder1: [vr] LadderTurn
static bool g_zipRecentre = true;  // ziprec1 (bd80): [vr] ZiplineRecentre
bool *EngineZipRecentre() { return &g_zipRecentre; }
static bool g_keepView = true;     // keepview2: [vr] KeepViewAfterPause; never after a vehicle (DrGonzaga wants the game's own facing there, vehrec2)
static DWORD g_vehSeenT;           // keepview2: last time AimMatrix saw a vehicle or mounted gun
static BYTE *volatile g_rideEnt;   // turride1: the vehicle object a mounted gun rides on (null = not used)
static volatile bool g_turScripted;  // turscript1: this mounted gun is a scripted ride (cinematic flag seen on since getting on)
static volatile bool g_turHeli;      // turheli1: ... and its vehicle object is a real vehicle (more than 1.5 m from the gun): the opening helicopter
// turdragon1 (DrGonzaga 20 Nov, final level: the armoured blood dragon's gun "a similar section" to the opening helicopter):
// the game never says "in a vehicle" on the dragon, so no vehicle object is found. A scripted ride (cinematic flag on)
// with no vehicle object at all after 1.5 s gets the helicopter's view rules (held above the gun, no shake). A scripted
// turret on its own base (bd98: base object 0.94 m away) has a vehicle object, so it is not caught. [vr] TurretDragonRules.
static volatile bool g_turVehSeen;   // a vehicle object (other than the gun) was found under this mounted gun
static volatile bool g_turDragon;    // this mounted gun is the dragon (scripted, no vehicle object)
static volatile bool g_turDragonFrame;  // ... and the gun's own object keeps its heading while you aim (so it turns with the dragon only)
static bool TurSpecial() { return g_turHeli || g_turDragon; }
bool *EngineKeepView() { return &g_keepView; }
static bool g_vehRecentre = true;  // vehrec1 (DrGonzaga 16 Nov): [vr] VehicleRecentre
bool *EngineVehRecentre() { return &g_vehRecentre; }
// vehface1 (DrGonzaga 18 Nov: "when I exit, I'm currently facing where the 2d game would put me ... it would feel better
// if I faced the same way as I was facing in the vehicle, i.e. just shift out"): the view's direction in the world is
// kept from the last frame of driving (before the game's own getting-out), and on foot the body is set so the view
// carries on looking that way; no recentre on getting out. [vr] VehicleExitKeepFacing (1).
static bool g_vehExitKeep = true;
bool *EngineVehExitKeep() { return &g_vehExitKeep; }
static volatile bool g_vehDriving;    // seated in a vehicle and the game has not started getting you out
static volatile float g_vehViewYaw;   // the view's heading while driving (radians, world)
static volatile DWORD g_vehViewT;     // when it was last taken
static DWORD g_vehKeepT;              // when the facing was kept on getting out (the game's own turn is not followed for a moment)
bool *EngineLadderTurn() { return &g_ladderTurn; }
static bool ScopedNow() { return !g_scopeGunOff && g_scopeTick && GetTickCount() - g_scopeTick < 150; }
bool EngineScoped() { return ScopedNow(); }
// scope9: only while you hold the aim (left trigger): cutscenes zoom too.
bool EngineScopeEyeActive() { return g_scopeOn && ScopedNow() && EngineOnFoot() && StereoTwice() && InputAimHeld() && !EngineTurret() && !CyberEyeRecent(); }  // turacc1 // cyoff1: no scope lens on a mounted gun
bool *EngineScopeOn() { return (bool *)&g_scopeOn; }
// pip1 (DrGonzaga 21 Nov: "picture-in-picture for the sniper scope"): with [aim] ScopeStereo the scope view is a THIRD
// render each frame (eye 2, drawn first by stereo.cpp), so both eyes keep their own picture and each gets the lens;
// off = the scope1 way (the right eye's render is the scope view and both eyes see the left picture).
static volatile bool g_scopePip;
bool *EngineScopePip() { return (bool *)&g_scopePip; }
// rifle1: a scope on the rifle (scope.cpp, per gun) always uses the scope picture.
static inline bool PipOn() { return g_scopePip || ScopeRifleOn(); }
static inline int ScopeEyeId() { return PipOn() ? 2 : 1; }
int EngineScopeEye() { return ScopeEyeId(); }
// The headset shows the left picture to both eyes (scope1 only).
bool EngineScopeMono() { return !PipOn() && EngineScopeEyeActive(); }
// stereo.cpp: draw the scope picture this frame (pip1).
// autoscope1 (DrGonzaga 21 Nov: "turn on the scope automatically when it's brought to eye level (so holding left trigger
// stabilizes)"): a rifle scope with RifleAuto shows its picture whenever the eyepiece is at the eye (scope.cpp), with
// no trigger. Without the trigger the game has no zoom: the scope uses the zoom learnt from the last trigger-held
// scoping (ScopeAutoFov) and no steadiness smoothing. Shots, recoil and the A / X zoom steps keep going by
// EngineScopeEyeActive (trigger held) only.
static bool AutoScopeNow() {
    return g_scopeOn && !g_scopeGunOff && ScopeRifleAutoNow() && EngineOnFoot() && StereoTwice() && !EngineTurret();
}
bool EngineScopePictureActive() { return EngineScopeEyeActive() || (AutoScopeNow() && !CyberEyeRecent()); }  // cyoff1
// steadyonly1: input.cpp keeps the left trigger from the game on an auto rifle scope gun (it only steadies).
bool EngineScopeSteadyOnly() { return g_scopeOn && !g_scopeGunOff && ScopeSteadyOnlyNow() && EngineOnFoot() && !EngineTurret(); }
bool EngineScopePipWanted() { return PipOn() && EngineScopePictureActive(); }
// narrow1 (DrGonzaga 21 Nov, optimisation 1: "draw only what the lens shows"): the scope camera's view is narrowed by the
// lens's extra zoom, so the game draws (and culls to) just the part the lens shows, at the picture's full resolution.
// scope.cpp samples with the zoom actually used (EngineScopeDrawnZoom), so a frame with a changed zoom still lines up.
static bool g_scopeNarrow = true;  // [aim] ScopeNarrow
bool *EngineScopeNarrow() { return &g_scopeNarrow; }
static volatile float g_scopeDrawnZoom = 1;
float EngineScopeDrawnZoom() { return g_scopeDrawnZoom; }
static float NarrowFov(float fov) {
    float z = *ScopeZoom();
    if (!g_scopeNarrow || !(z > 1.0f)) return g_scopeDrawnZoom = 1, fov;
    if (z > 12) z = 12;
    g_scopeDrawnZoom = z;
    return 2.0f * atanf(tanf(fov * 0.5f) / z);
}
float *EngineScopeSmoothing() { return &g_scopeSmooth; }
// crouchsteady1 (DrGonzaga 6 Nov): extra scope steadiness while crouched, and the
// pre-2 Nov plain average as an option to compare.
static volatile bool g_crouched;
static float g_crouchSteady = 1.5f;  // [aim] ScopeCrouchSteady, x steadiness
static bool g_classicSmooth;         // [aim] ScopeClassicSmoothing
// realcrouch1 (DrGonzaga 6-8 Nov): crouch for real to crouch in game. (Lying down was
// dropped by DrGonzaga and removed in nolie1.) The game's crouch is a toggle (pad B),
// so we press it when the real head goes below / comes back above the depth.
// While we crouched you, the view follows your real head (the game's own crouch
// drop is taken back out), so you don't drop twice.
static bool g_realCrouch = true;         // [gameplay] RealCrouch
static float g_crouchDepth = 0.30f;      // [gameplay] RealCrouchDepth, m below standing
static volatile float g_headUp;          // real head height since recentring (m)
static volatile float g_rawCamZ;         // move2: the game camera's own height (before our crouch lift)
static volatile DWORD g_rawCamTick;
static volatile DWORD g_headUpTick;
static volatile DWORD g_crouchPulse;     // press pad B until this tick
static volatile bool g_ourCrouch, g_viewComp;
static float g_standUp = 1.66f;          // game camera above the player when standing (learnt)
bool *EngineRealCrouch() { return &g_realCrouch; }
float *EngineRealCrouchDepth() { return &g_crouchDepth; }
bool EngineCrouchPress() { return GetTickCount() < g_crouchPulse; }
// crouch3 (DrGonzaga 13 Nov, Blood Dragon: hands and weapons far too low when crouching for real): the view is
// lifted by the game's crouch drop (g_viewComp below), but the gun, arrows and VR hands are placed from the
// game's own camera object, which still drops. The lift of the last frame, for gunattach.cpp to add.
static volatile float g_crouchLift;
static volatile DWORD g_crouchLiftTick;
float EngineCrouchLift() { return GetTickCount() - g_crouchLiftTick < 300 ? g_crouchLift : 0; }
float EngineHeadDrop() { return GetTickCount() - g_headUpTick < 500 ? -g_headUp : 0; }
bool EngineCrouched() { return g_crouched; }
float *EngineScopeCrouchSteady() { return &g_crouchSteady; }
bool *EngineScopeClassic() { return &g_classicSmooth; }
float EngineScopeFov() { return g_scopeFov; }
// scope3: where the scope view was rendered from (shots fire along it while scoped).
static float g_scopeRay[6];
static volatile DWORD g_scopeRayTick;
bool EngineScopeRay(float pos[3], float dir[3]) {
    if (!EngineScopePictureActive() || !g_scopeRayTick || GetTickCount() - g_scopeRayTick > 200) return false;  // steadyonly1: auto scope too
    memcpy(pos, g_scopeRay, 12), memcpy(dir, g_scopeRay + 3, 12);
    return true;
}

static float *nearFov0(BYTE *cam) { return (float *)(cam + 0x22c); }
// lod1 (DrGonzaga 1 Nov: heads and wheels appear closer than in the flat game).
// The renderer's LOD distance factor (0x11563d40) is
//   (tan(fov/2) * k * 0.8 + 0.2), k = 1/tan(37.5 deg) cached at 0x128b7864,
// and fov is the render camera's +0x228, which we widen to the headset's
// (98 vs 54 deg), so LOD and kill distances were roughly halved. k is
// rescaled so the factor matches the game's own fov again: g_lodFix 1 =
// flat-game detail distances, 0 = as before (cheaper). Graphics page slider.
float g_lodFix = 0.0f;  // [render] LodFix (lod2: off by default, it did not bring the heads back)
// bd68 kill1 (DrGonzaga 15 Nov: "at a distance, character models don't have heads. They can pop in quite late").
// Blood Dragon builds each view's detail numbers in BD+0x100d47a7 (out, settings, view):
//   zoom = sin(view fov / 2) / sin(reference fov / 2)   (BD+0x100d4718, fovs at +0x28 and +0x38 of the
//          view's camera record, table at 0x11efde0c), not below the settings' MinZoomFactor (+0x5c8)
//   out[1] = LodScale (+0x57c) * view[0x30] * zoom              detail steps
//   out[2] = KillLodScale (+0x580) * view[0x38] * view[0x34] * zoom   where a part stops being drawn
// (bd68 assumed the headset's fov upset this zoom; bd69's log showed it does not.)
float g_minSizeScale = 1;  // [render] TestMinSizeScale: SceneObjectMinSize (+0x5b8) and its depth pass twin (+0x5bc)
float g_killScale = 1, g_detailScale = 1, g_zoomScale = 1;  // [render] TestKillScale, TestDetailScale, TestZoomScale (bd71 kill2)
DWORD g_lodScaleTick;  // when a test slider last moved (menu.cpp)
static float g_lodGameFov, g_lodOurFov;
typedef float *(__cdecl *LodParamsFn)(float *out, BYTE *cfg, BYTE *view);
typedef float(__cdecl *LodZoomFn)(BYTE *camRec, BYTE *cfg);
static LodParamsFn g_lodParams;
static LodZoomFn g_lodZoom;
static float *__cdecl LodParamsHook(float *out, BYTE *cfg, BYTE *view) {
    float *r = g_lodParams(out, cfg, view);
    // bd71 kill2: bd69's log showed the camera record's fov is the game's own (74.8 deg against a reference of
    // 74.8, zoom 1.00), so this path is NOT upset by the headset's fov and the bd68 correction was wrong (it
    // pushed out[2] down, and DrGonzaga saw less foliage, not more heads). Now plain test multipliers, 1 = the game's.
    const float ks = g_killScale, ds = g_detailScale, zs = g_zoomScale, ms = g_minSizeScale;
    // The settings' smallest-part sizes: kept as the game set them, written back scaled (per settings object).
    static struct MinKeep { BYTE *cfg; float game[2], wrote[2]; } s_keep[4];
    static int s_keepN, s_keepLogs;
    float *mn = (float *)(cfg + 0x5b8);
    MinKeep *mk = nullptr;
    for (int i = 0; i < s_keepN; i++)
        if (s_keep[i].cfg == cfg) mk = &s_keep[i];
    if (!mk && s_keepN < 4) mk = &s_keep[s_keepN++], mk->cfg = cfg, mk->wrote[0] = mk->wrote[1] = -1;
    if (mk) {
        if (mn[0] != mk->wrote[0] || mn[1] != mk->wrote[1]) {  // new, or the game set them again (graphics settings changed)
            mk->game[0] = mn[0], mk->game[1] = mn[1];
            if (s_keepLogs < 12)
                ++s_keepLogs, Log("engine: kill2: settings %p: the game's smallest part size %.4f (depth pass %.4f), cluster %.4f, zoom %.2f, detail %.2f, kill %.2f",
                                  cfg, mn[0], mn[1], *(float *)(cfg + 0x5b0), out[0], out[1], out[2]);
        }
        mn[0] = mk->wrote[0] = mk->game[0] * ms, mn[1] = mk->wrote[1] = mk->game[1] * ms;
    }
    out[0] *= zs, out[1] *= ds, out[2] *= ks;
    return r;
}
static void LodParamsInstallBd() {
    static const DWORD kFn = 0x100d47a7, kZoom = 0x100d4718, kSites[2] = {0x100d7f14, 0x1118091b};
    BYTE *fn = g_mod + (kFn - kImageBase), *zf = g_mod + (kZoom - kImageBase);
    static const BYTE kHead[7] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45, 0x10}, kZHead[8] = {0x55, 0x8b, 0xec, 0x51, 0x56, 0x8b, 0x75, 0x08};
    bool ok = !memcmp(fn, kHead, sizeof kHead) && !memcmp(zf, kZHead, sizeof kZHead);
    for (DWORD a : kSites) {
        BYTE *c = g_mod + (a - kImageBase);
        ok = ok && c[0] == 0xe8 && c + 5 + *(int *)(c + 1) == fn;
    }
    if (!ok) {
        Log("engine: kill1: the view detail code doesn't match; distant heads fix off");
        return;
    }
    g_lodParams = (LodParamsFn)fn, g_lodZoom = (LodZoomFn)zf;
    for (DWORD a : kSites) PatchCallTo(g_mod + (a - kImageBase), (void *)LodParamsHook);
    Log("engine: kill1: view detail numbers hooked at BD+%08lx and BD+%08lx", kSites[0], kSites[1]);
}
static void LodFix(float gameFov, float ourFov) {
    // bd68 kill1: Blood Dragon's k is at 0x1184e2e4 (flag 0x1184e2e8), set up in BD+0x1014a440.
    const DWORD kK = g_bloodDragon ? 0x1184e2e4 : 0x128b7864, kInit = g_bloodDragon ? 0x1184e2e8 : 0x128b7868;
    static const float k0 = 1.0f / tanf(0.6545f);
    g_lodGameFov = gameFov, g_lodOurFov = ourFov;
    if (!g_mod) return;
    float *k = (float *)(g_mod + (kK - kImageBase));
    if (!(*(DWORD *)(g_mod + (kInit - kImageBase)) & 1)) return;  // the game sets k up first
    float r = ourFov > 0.1f ? tanf(gameFov * 0.5f) / tanf(ourFov * 0.5f) : 1.0f;
    float f = g_lodFix < 0 ? 0 : g_lodFix > 1 ? 1 : g_lodFix;
    float nk = k0 * (1.0f + (r - 1.0f) * f);
    // tidy1: logged only when the fix is on and changes (it repeated every zoom change).
    static float s_last;
    if (f > 0 && fabsf(nk - s_last) > 0.25f)
        s_last = nk, Log("engine: lod1: detail distance factor x%.2f (game fov %.1f, ours %.1f, fix %.0f%%)", k0 / nk,
                         gameFov * 57.2958f, ourFov * 57.2958f, f * 100);
    *k = nk;
}
static void ApplyFov(BYTE *cam) {
    float tx, ty;
    s_scopeEyeNow = false;
    if (!g_vrFov || !XrGetHeadsetFov(&tx, &ty)) {
        LodFix(1, 1);
        return;
    }
    float *fov = (float *)(cam + 0x228), *aspect = (float *)(cam + 0x18);
    if (*fov > 0.02f && *fov <= 0.7f && *aspect > 0.3f && *aspect < 2.0f && EngineOnFoot()) {
        // Zoomed (scope): note it; the scope eye keeps the game's zoom, the
        // other eye (or both, with the scope eye off) gets the headset's fov.
        static bool s_was;
        if (!ScopedNow() || !s_was) LogV("engine: scope: game zoom %.1f deg", *fov * 57.2958f);
        s_was = true;
        g_scopeFov = *fov, g_scopeTick = GetTickCount();
        if (g_scopeOn && !g_scopeGunOff && g_forceEye == ScopeEyeId() && StereoTwice() && InputAimHeld()) {
            *aspect = tx / ty;
            *fov = NarrowFov(*fov);  // narrow1
            s_scopeEyeNow = true;
            return;
        }
        if (g_forceEye == ScopeEyeId() && ScopeEyeId() == 2 && AutoScopeNow()) {  // fade1: trigger let go while the game zooms out
            *aspect = tx / ty;
            *fov = NarrowFov(ScopeAutoFov());
            s_scopeEyeNow = true;
            return;
        }
        *fov = 2 * atanf(tx);
        *aspect = tx / ty;
        if (g_gunFov && *nearFov0(cam) > 0.1f && *nearFov0(cam) < 3.0f) *nearFov0(cam) = *fov;
        return;
    }
    // Only touch a sane perspective camera (game FOV 40-120 degrees, aspect 0.3-2).
    if (!(*fov > 0.7f && *fov < 2.1f && *aspect > 0.3f && *aspect < 2.0f)) return;
    if (g_forceEye == ScopeEyeId() && ScopeEyeId() == 2 && AutoScopeNow()) {  // autoscope1: the scope picture with no game zoom
        *aspect = tx / ty;
        *fov = NarrowFov(ScopeAutoFov());
        s_scopeEyeNow = true;
        return;
    }
    LodFix(*fov, 2 * atanf(tx));
    static bool logged;
    float *nearFov = (float *)(cam + 0x22c);
    if (!logged) {
        logged = true;
        Log("engine: game fov %.1f deg gun fov %.1f deg aspect %.3f -> headset fov %.1f deg aspect %.3f",
            *fov * 57.2958f, *nearFov * 57.2958f, *aspect, 2 * atanf(tx) * 57.2958f, tx / ty);
    }
    *fov = 2 * atanf(tx);
    *aspect = tx / ty;
    if (g_gunFov && *nearFov > 0.1f && *nearFov < 3.0f) *nearFov = *fov;
}

// Horizon lock (Numpad 7, on by default): the game camera is made level
// before the head is applied, so only the headset tilts the view. The right
// stick's up/down is dropped too (input.cpp), so the game's aim stays level.
static volatile bool g_horizon = true;
bool EngineHorizonLock() { return g_horizon; }

// Aim with the head (Numpad 1, on by default; a stand-in until motion
// controls). Turning the game camera doesn't move the gun (aim1/aim2), so we
// steer the game like a player would, with mouse movement (mouseaim.cpp),
// until the game's on-foot look points where the head looks. The on-foot
// camera (0x11416bc0) hands us its look matrix at 0x11417518 (via
// 0x10089f40, rows right +0, forward +4, up +8), which tells us how far off
// the game is. The view doesn't wait for the game: on foot, the render camera
// is our own level body direction (turned by the right stick here, not by
// the game) plus the head, so the slight lag of the gun never moves the view.
// lookaim1: steer the game's look with the right stick instead (the game does
// read the pad). 0 off, 1 head, 2 right controller (default). Numpad 2 cycles.
static volatile int g_lookMode = 2;
static volatile float g_stick[2];     // right stick we feed the game while steering (-1..1)
static float g_steerTune[2] = {8.0f, 0.25f};  // gain (1/s: how hard the look is pulled to the target), stick dead zone we jump over (menu: Aim)
#define g_steerGain g_steerTune[0]
#define g_steerDz g_steerTune[1]
static volatile float g_body[2];      // level body forward (x, y) while aiming with the head
static volatile bool g_bodyLive;      // ApplyHead uses g_body
static volatile DWORD g_aimLast;      // tick of the last on-foot camera update
static volatile LONG g_rightX;        // right stick x as the pad gave it (input.cpp)
static volatile LONG g_rightY;        // snap1: right stick y as the pad gave it
static bool g_snapOn = true;          // snap1: snap turn instead of smooth turn ([vr] SnapTurn)
static float g_snapDeg = 45;          // snap1: degrees per snap ([vr] SnapTurnAngle)
bool *EngineSnapTurn() { return &g_snapOn; }
float *EngineSnapAngle() { return &g_snapDeg; }
static void SaveLookRate(const wchar_t *key, float v) {  // look3
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"aim", key, buf, path);
}
static struct {
    int phase;                        // 0 off, 1 measure yaw, 2 measure pitch, 3 steering, -1 gave up
    int frames;
    float y0, p0;
    float sYaw, sPitch;               // rad/s for a full stick (sign: which way it turns)
    DWORD retryAt;                    // look2: a failed learn is tried again from then
    float t0, prevErr;
    bool calibrated;
    int bigErr;
    LARGE_INTEGER last;
    float errSum, errMax;
    int errN;
} g_steer;

static void TurnByHead(const float *r, const float *u, const float *f, const float *m, float *nr, float *nu, float *nf) {
    for (int i = 0; i < 3; ++i) {
        nf[i] = -(r[i] * m[0 * 4 + 2] + u[i] * m[1 * 4 + 2] - f[i] * m[2 * 4 + 2]);
        nu[i] = r[i] * m[0 * 4 + 1] + u[i] * m[1 * 4 + 1] - f[i] * m[2 * 4 + 1];
        nr[i] = r[i] * m[0 * 4 + 0] + u[i] * m[1 * 4 + 0] - f[i] * m[2 * 4 + 0];
    }
}

static void LevelBasis(float *f, float *u, float *r);

// The game's own look (for the look-angle scan, aimscan.cpp).
static volatile float g_gameYaw, g_gameFwd[2], g_gamePitch;
static volatile DWORD g_gameLookTick;
static float g_aimBasis[2][9];  // level game camera basis (r, u, f) from the on-foot camera, double-buffered
static volatile LONG g_aimBasisIdx = -1;
bool EngineAimBasis(float r[3], float u[3], float f[3], float camFwd[3]) {
    LONG i = g_aimBasisIdx;
    if (i < 0 || GetTickCount() - g_gameLookTick > 300) return false;
    memcpy(r, g_aimBasis[i], 12), memcpy(u, g_aimBasis[i] + 3, 12), memcpy(f, g_aimBasis[i] + 6, 12);
    if (EngineAimHeadLive()) {  // the view turns with our body direction then, so the hand is relative to it
        f[0] = g_body[0], f[1] = g_body[1], f[2] = 0, u[0] = u[1] = 0, u[2] = 1;
        Cross(f, u, r);
    }
    camFwd[0] = g_gameFwd[0], camFwd[1] = g_gameFwd[1], camFwd[2] = sinf(g_gamePitch);
    return true;
}
bool EngineGameLook(float *yaw, float fwd[2], float *pitch) {
    if (GetTickCount() - g_gameLookTick > 500) return false;
    *yaw = g_gameYaw, fwd[0] = g_gameFwd[0], fwd[1] = g_gameFwd[1], *pitch = g_gamePitch;
    return true;
}

bool EngineOnFoot() { return GetTickCount() - g_aimLast < 200; }
// loadface1 (DrGonzaga 17 Nov, bd102 run: "already facing the wrong way" at the start): after a load the view is lined up
// with where the game looks (keepview1, using the head's turn), and about a second later the automatic recentre after
// loading sets the head's turn to nothing, which swung the view by however far the head had been turned (125 deg in
// that run). So the view is lined up with the game's look again just after that recentre.
static volatile DWORD g_reseedT;
void EngineAfterLoadRecentre() { g_reseedT = GetTickCount() | 1; }
bool EngineAimHeadLive() { return g_bodyLive && GetTickCount() - g_aimLast < 200; }
void EngineSteerStick(SHORT *x, SHORT *y) {
    *x = (SHORT)(g_stick[0] * 32767), *y = (SHORT)(g_stick[1] * 32767);
}
void EngineSetRightX(SHORT x) { g_rightX = x; }
void EngineSetRightY(SHORT y) { g_rightY = y; }
void EngineRightStick(float *x, float *y) { *x = g_rightX / 32767.f, *y = g_rightY / 32767.f; }  // point1

static float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void SetBody(float x, float y) {
    float l = sqrtf(x * x + y * y);
    if (l < 1e-4f) return;
    g_body[0] = x / l, g_body[1] = y / l;
}

static volatile DWORD g_gameThread;
DWORD EngineGameThread() { return g_gameThread; }

// Vehicles (veh3/veh4, 2 Nov; cleaned up in veh5): in a vehicle the view is
// left to the game's camera, which turns with the vehicle, and the right stick
// goes to the game. In a car the game keeps using its on-foot camera, so the
// signals are the player link's seat bone (-1 on foot, the seat in a vehicle;
// it stays set after getting out) and the camera's height above the player
// (~0.75 m seated, 0.05-0.35 m on the hang glider, 1.58-1.67 m on foot).
// 250 ms settle each way. Level horizon and head position stay on.
static volatile bool g_vehicle;
static volatile bool g_turSteer;  // bd13: mounted gun steered to the controller now
bool EngineInVehicle() { return g_vehicle || EngineTurret(); }
float g_turretEye;  // turret1: eye height above the player on a mounted gun (0 = not measured yet)
static const bool g_car[4] = {true, true, false, true};  // detect, view follows the game camera, 3DoF, level horizon

// cut1 (DrGonzaga 9 Nov): cutscenes. The game keeps its own "cinematic playing"
// flag (FC3_d3d11.dll, Ghidra): a byte at +0x70 of the struct the global at
// 0x12829bc0 points to. A cinematic sequence's start (0x10c6b580, "Started")
// and the cinematic camera service's start (0x111b1d50) set it to 1; the
// sequence's stop (0x10c6b0c0), the camera service's end (0x111b26a0,
// 0x111b2ed0) and level loads (0x10cfe3d0) clear it. Read only.
// While it is set: the view's turn is the headset's alone on a fixed level
// heading (the game's camera only moves you: no game pitch, roll, sway or
// pans), the vehicle test is paused, and the on-foot look, laser and gun in
// hand rest. At the end the vehicle state is reset to on foot and found afresh
// (9 Nov log: after a cutscene the view stayed in "vehicle" mode for 4.5
// minutes, so head bob came back and the gun never returned to the hand).
static BYTE **g_cinePtr;               // the game's pointer to the struct with the flag
static volatile bool g_cutscene;
static volatile LONG g_cutGen;         // +1 at each start (the view takes a new heading)
static volatile LONG g_cutEnded;       // set at the end, taken by AimMatrix (game thread)
bool g_cutSteady = true;               // [vr] CutsceneSteady (VR page)
// flat1 (DrGonzaga 11 Nov): cutscenes on a flat 2D screen in front of you, played
// exactly as in the normal game (the game's own camera, no head turn, one picture).
bool g_cutFlat = true;                 // [vr] CutsceneScreen
float g_cutScreen[3] = {3.0f, 2.8f, 0.0f};  // [vr] CutsceneScreenDistance, Width, Height (metres)
bool g_cutWide = true;                 // [vr] CutsceneScreen169 (movie1): the screen is 16:9 (the picture's middle band)
float g_cutZoom = 1.0f;                // [vr] CutsceneScreenZoom: over 1 = closer picture
bool EngineCutscene() { return g_cutscene; }
// cut3 (DrGonzaga 9 Nov, save1fix log): the flag also stays set for half a minute
// of normal play after a door/area load or a respawn (a sequence that doesn't
// take the camera), and the gun stayed off the hand. So cutscene mode also
// needs the picture to come from another camera: the render camera's source
// (g_srcCam) isn't the on-foot camera (AimMatrix's camera + 4, veh1 2 Nov;
// vehicles use it too), or the on-foot camera stopped. 300 ms settle each way.
static volatile DWORD g_footCam;       // the on-foot camera (0x11416bc0's ebx), from AimMatrix
// mark1 (DrGonzaga 11 Nov): "that was / wasn't a cutscene" from the menu. The last
// few things the detector saw go into the log with the mark, so each call can be
// checked later. "Wasn't" during a cutscene also ends it now (back to VR) and keeps
// it off until the game's flag goes off or the on-foot camera is back.
static struct CutEv { DWORD t; BYTE kind; } g_cutEv[12];  // render thread only
static int g_cutEvN;
static volatile bool g_cutVeto;
static bool g_cutFlagNow, g_cutGameCamNow;
static const char *const kCutEvName[] = {"flag on, picture from another camera", "flag on, picture from the on-foot camera",
                                          "flag off", "picture moved to another camera (flag on)",
                                          "picture back to the on-foot camera (flag on)", "cutscene started", "cutscene ended",
                                          "you said: not a cutscene (back to VR)", "input mode: asks for input",
                                          "input mode: no longer asks for input", "input mode: cinematic",
                                          "input mode: no longer cinematic", "cutscene moved to VR (prompts)",
                                          "cutscene back on the 2D screen", "the game's cutscene sequence started",
                                          "the game's cutscene sequence ended"};
static void CutNote(int kind) {
    CutEv &e = g_cutEv[g_cutEvN++ % 12];
    e.t = GetTickCount(), e.kind = (BYTE)kind;
}
static volatile LONG g_cutMarks[2];
LONG EngineCutsceneMarks(bool was) { return g_cutMarks[was ? 1 : 0]; }
void EngineCutsceneMark(bool was) {
    InterlockedIncrement(&g_cutMarks[was ? 1 : 0]);
    char hist[1200] = "";
    DWORD t = GetTickCount();
    int n = g_cutEvN < 12 ? g_cutEvN : 12;
    for (int i = n; i >= 1; --i) {
        const CutEv &e = g_cutEv[(g_cutEvN - i) % 12];
        if (t - e.t > 300000) continue;  // the last 5 minutes
        char one[100];
        snprintf(one, sizeof one, "%s%.1f s ago %s", hist[0] ? "; " : "", (t - e.t) / 1000.0f, kCutEvName[e.kind]);
        strncat(hist, one, sizeof hist - strlen(hist) - 1);
    }
    char nb[40];
    Log("cutscene-mark: YOU SAID %s. Now: %s, flag %s, picture from %s, input mode %s. Seen: %s", was ? "THAT WAS A CUTSCENE" : "THAT WAS NOT A CUTSCENE",
        g_cutscene ? (g_flatNow ? "on the 2D screen" : "cutscene mode") : "normal VR", g_cutFlagNow ? "on" : "off",
        g_cutGameCamNow ? "the on-foot camera" : "another camera", EngineInputModeName(nb, sizeof nb), hist[0] ? hist : "nothing in the last 5 minutes");
    if (!was && g_cutscene) g_cutVeto = true, CutNote(7);
}

// seq1 (DrGonzaga 11 Nov: "if it's a sequence that needs player input it's 3D; if
// it's just a video rendered in-game, it's on the screen until player control is
// given back"). The game keeps a list of input modes ("action maps") for the
// player and says which one is in force (FC3_d3d11.dll, Ghidra):
// - global 0x12829474 -> player list: +8 array, +0xc count; entry 0 is a small
//   wrapper (vtable 0x11f48574) whose +4 points at the player's controller;
//   controller + 0x13c is the input-mode object (the wrapper's vtable slot 2,
//   0x116fe9a0, returns exactly that).
// - input-mode object: +0x30 the mode in force (0 = none), +0x34 its level,
//   +0x14 / +0x18 the list of pushed modes (12 bytes each: mode, level, flag).
// - a mode: +0x48 and +0x4c hold the CRC32 of its name (0x117019f0 and
//   0x11701260 compare them with 0x11798fc0(name), a plain CRC32).
// Names found in the code: "cinematic" (taken off again when a cinematic ends,
// 0x1117ae70), "narrative_interact" and "narrative_interact_qte" (scenes with
// button prompts; the QTE widget 0x10dcbf60 sets the second), and the minigame
// and "exotic gameplay" modes. Read only, on the render thread, a few guarded
// reads a frame; every change is logged with the pushed list so that modes the
// game's data names (not in the code) show up as numbers to be named later.
static DWORD kPlayersGlobal = 0x12829474, kPlayerWrapVt = 0x11f48574;  // bd7: Blood Dragon sets its own (SeqInstallBd)
bool g_seqInter3D = true;   // [vr] CutscenePromptsIn3D: a cutscene with button prompts is shown in VR, not on the screen
bool g_seqByMap = false;    // [vr] CutsceneByInputMode (TEST): "cinematic" input mode alone (on-foot camera) also counts
float g_seqHold = 8.0f;     // [vr] CutscenePromptHold: seconds a cutscene stays in VR after its last prompt
static bool g_seqOk;
static volatile bool g_cutInter;       // this cutscene is shown in VR because it asks for input
static volatile DWORD g_seqBase;       // emptyhands2: the oldest pushed input mode
static volatile DWORD g_seqHash;       // the input mode in force (name CRC), for the menu and the marks
static volatile bool g_seqPrompt;      // qte6: the input mode is narrative_interact / narrative_interact_qte (a scene asking for a button)
bool EngineScenePrompt() { return g_seqPrompt; }
struct SeqMode { DWORD hash; const char *name; BYTE kind; };  // kind 1 = cinematic, 2 = asks for input, 3 = other
static const SeqMode kSeqModes[] = {
    {0x1eb7b814, "cinematic", 1},
    {0x8da3fcea, "narrative_interact", 2},
    {0x82add0f5, "narrative_interact_qte", 2},
    {0xdaa560d2, "beerpong_minigame", 2},
    {0x795b1741, "poker_exotic_gameplay", 2},
    {0x3e492976, "shootinggallery_minigame", 2},
    {0xdc7b0cac, "mountedshootinggallery_minigame", 2},
    {0xe2f4f17b, "snipingchallenge_minigame", 2},
    {0x5725d3cc, "cenote_exotic_gameplay", 2},
    {0x14e0bbb1, "dog_attack_exotic_gameplay", 2},
    {0xa62a4106, "vehicleChallenge", 2},
    {0x9710e80a, "buddy_boost_input", 2},
    {0x876daa56, "intoxicated", 3},
    {0x46b9f389, "intoxicated_nomove", 3},
    {0x9b6f44e7, "observer", 3},
};
static const SeqMode *SeqFind(DWORD h, DWORD h2) {
    for (const SeqMode &m : kSeqModes)
        if (m.hash == h || m.hash == h2) return &m;
    return nullptr;
}
static const char *SeqName(DWORD h, DWORD h2, char *buf, size_t cap) {
    const SeqMode *m = SeqFind(h, h2);
    if (m) return m->name;
    if (!h && !h2) return "none";
    if (h == h2) snprintf(buf, cap, "mode %08lx", h);
    else snprintf(buf, cap, "mode %08lx/%08lx", h, h2);
    return buf;
}
const char *EngineInputModeName(char *buf, size_t cap) {
    DWORD h = g_seqHash;
    return g_seqOk ? SeqName(h, h, buf, cap) : "not available";
}
bool EngineCutsceneIn3D() { return g_cutInter; }
// The input mode in force: false = can't tell right now.
static bool SeqRead(DWORD *h, DWORD *h2, BYTE **inp) {
    BYTE *pm = *(BYTE **)(g_mod + (kPlayersGlobal - kImageBase));
    if (!pm || IsBadReadPtr(pm + 8, 8)) return false;
    BYTE **arr = *(BYTE ***)(pm + 8);
    DWORD n = *(DWORD *)(pm + 0xc);
    if (!arr || !n || n > 16 || IsBadReadPtr(arr, 4)) return false;
    BYTE *el = arr[0];
    if (!el || IsBadReadPtr(el, 8) || *(DWORD *)el != (DWORD)g_mod + (kPlayerWrapVt - kImageBase)) return false;
    BYTE *ctl = *(BYTE **)(el + 4);
    if (!ctl || IsBadReadPtr(ctl + 0x13c, 0x3c)) return false;
    BYTE *in = ctl + 0x13c, *cur = *(BYTE **)(in + 0x30);
    *h = *h2 = 0, *inp = in;
    if (cur) {
        if (IsBadReadPtr(cur + 0x48, 8)) return false;
        *h2 = *(DWORD *)(cur + 0x48), *h = *(DWORD *)(cur + 0x4c);
    }
    return true;
}
// The pushed modes, oldest first, for the log (the game thread may be changing the
// list: every read is checked, and a torn line in the log does no harm).
static void SeqList(BYTE *in, char *out, size_t cap) {
    out[0] = 0;
    BYTE *a = *(BYTE **)(in + 0x14);
    DWORD n = *(DWORD *)(in + 0x18);
    if (!a || !n || n > 24 || IsBadReadPtr(a, n * 12)) return;
    for (DWORD i = 0; i < n; ++i) {
        BYTE *m = *(BYTE **)(a + i * 12);
        int lvl = *(int *)(a + i * 12 + 4);
        if (!m || IsBadReadPtr(m + 0x48, 8)) continue;
        char nb[40], one[80];
        snprintf(one, sizeof one, "%s%s@%d", out[0] ? ", " : "", SeqName(*(DWORD *)(m + 0x4c), *(DWORD *)(m + 0x48), nb, sizeof nb), lvl);
        strncat(out, one, cap - strlen(out) - 1);
    }
}

// movie1 (DrGonzaga 12 Nov: non-interactive scripted scenes still play in VR and look
// janky; he wants them on a 16:9 movie screen). The game itself knows when a
// cutscene is playing: 0x10c73aa0 (10 callers: it picks the "cinematic" input mode,
// blocks the weapon wheel and so on) walks its list of sequences (global
// 0x1282af4c -> +0x24 array, +0x28 count) for one that is playing (+0x2c == 1), is
// a CCinematicSequence (vtable 0x12091088) and has the designers' "bIsCutscene"
// box ticked (+0xaa). The same walk here, by reading only (no game calls), 20 times
// a second. Unlike the cinematic flag it is not set by doors and area loads
// (their sequences don't have the box ticked), and unlike the camera test it also
// covers scenes played through the player's own camera.
static DWORD kSeqMgrGlobal = 0x1282af4c, kSeqVt = 0x12091088;  // bd35: Blood Dragon sets its own (CutsceneInstallBd)
bool g_seqBySeq = true;     // [vr] CutsceneBySequence: a playing cutscene sequence counts as a cutscene
static bool g_seqMgrOk;
static bool SeqCutscenePlaying(int *playing) {
    *playing = 0;
    BYTE *m = *(BYTE **)(g_mod + (kSeqMgrGlobal - kImageBase));
    if (!m || IsBadReadPtr(m + 0x24, 8)) return false;
    BYTE **arr = *(BYTE ***)(m + 0x24);
    DWORD n = *(DWORD *)(m + 0x28);
    if (!arr || !n || n > 4096 || IsBadReadPtr(arr, n * 4)) return false;
    const DWORD vt = (DWORD)g_mod + (kSeqVt - kImageBase);
    bool cut = false;
    for (DWORD i = 0; i < n; ++i) {
        BYTE *q = arr[i];
        if (!q || IsBadReadPtr(q, 0xac) || *(int *)(q + 0x2c) != 1) continue;
        ++*playing;
        if (*(DWORD *)q == vt && q[0xaa]) cut = true;
    }
    return cut;
}

static void CutsceneTick(DWORD src) {
    if (!g_cinePtr) return;
    static BYTE *s_ok;
    static DWORD s_chgT, s_startT;
    static bool s_flag, s_logFlag;
    BYTE *s = *g_cinePtr;
    bool flag = false;
    if (s) {
        if (s != s_ok) s_ok = IsBadReadPtr(s + 0x70, 1) ? nullptr : s;
        flag = s_ok == s && s[0x70] != 0;
    }
    DWORD t = GetTickCount(), foot = g_footCam;
    bool gameCam = foot && src == foot + 4 && t - g_gameLookTick < 300;
    if (g_bloodDragon && foot) {  // bd7: once, so the first test shows whether the two cameras line up as in FC3
        static bool s_once;
        if (!s_once) s_once = true, Log("bd: on-foot camera %08lx, picture's source camera %08lx (expected: on-foot + 4)", foot, src);
    }
    if (flag != s_logFlag) {
        s_logFlag = flag;
        Log("cutscene: the game's cinematic flag %s (picture from %s)", flag ? "on" : "off",
            gameCam ? "the on-foot camera" : "another camera");
        CutNote(flag ? (gameCam ? 1 : 0) : 2);
    } else if (flag && gameCam != g_cutGameCamNow) {
        CutNote(gameCam ? 4 : 3);  // mark1: the picture's camera changed while the flag stayed on
    }
    g_cutFlagNow = flag, g_cutGameCamNow = gameCam;
    // seq1: the game's input mode for the player
    static DWORD s_h = 0xffffffff, s_h2, s_interT, s_cineT;
    static int s_lines;
    // seqrate1 (bd127): the read (half a dozen pointer checks) at most every 50 ms, not on every camera build
    // (2-3 a frame); between reads the last values stand.
    static DWORD s_readT;
    static int s_kind;
    DWORD h = s_h != 0xffffffff ? s_h : 0, h2 = s_h2;
    BYTE *in = nullptr;
    int kind = s_kind;
    if (g_seqOk && t - s_readT >= 50 && (s_readT = t, SeqRead(&h, &h2, &in))) {
        const SeqMode *m = SeqFind(h, h2);
        kind = s_kind = m ? m->kind : 0;
        g_seqHash = h ? h : h2;
        {   // emptyhands2: the oldest pushed mode (e2e5f03d only in the opening on-foot section, 17 Nov logs)
            DWORD b = 0;
            BYTE *a = *(BYTE **)(in + 0x14);
            DWORD n = *(DWORD *)(in + 0x18);
            if (a && n && n <= 24 && !IsBadReadPtr(a, 12)) {
                BYTE *m0 = *(BYTE **)a;
                if (m0 && !IsBadReadPtr(m0 + 0x48, 8)) b = *(DWORD *)(m0 + 0x4c) ? *(DWORD *)(m0 + 0x4c) : *(DWORD *)(m0 + 0x48);
            }
            g_seqBase = b;
        }
        g_seqPrompt = SeqFind(h, h2) && (SeqFind(h, h2)->hash == 0x8da3fcea || SeqFind(h, h2)->hash == 0x82add0f5);  // qte6
        if (h != s_h || h2 != s_h2) {
            int was = s_h == 0xffffffff ? 0 : (SeqFind(s_h, s_h2) ? SeqFind(s_h, s_h2)->kind : 0);
            s_h = h, s_h2 = h2;
            if (kind == 2 || was == 2) CutNote(kind == 2 ? 8 : 9);
            else if (kind == 1 || was == 1) CutNote(kind == 1 ? 10 : 11);
            if (s_lines < 1500) {
                char nb[40], list[600];
                SeqList(in, list, sizeof list);
                ++s_lines;
                Log("seq: input mode now %s (pushed: %s); cinematic flag %s, picture from %s%s", SeqName(h, h2, nb, sizeof nb),
                    list[0] ? list : "nothing", flag ? "on" : "off", gameCam ? "the on-foot camera" : "another camera",
                    s_lines == 1500 ? " (last line of this kind this session)" : "");
            }
        }
    }
    bool camCut = flag && !gameCam;
    // The "cinematic" input mode while the picture still comes from the on-foot camera
    // (a first-person scripted scene): counted only with the test box ticked; always
    // timed in the log so that doors and the like can be told from real scenes.
    bool cineFoot = kind == 1 && !camCut;
    if (cineFoot) {
        if (!s_cineT) s_cineT = t ? t : 1;
    } else if (s_cineT) {
        if (t - s_cineT >= 300)
            Log("seq: cinematic input mode with the on-foot camera for %.1f s (%s)", (t - s_cineT) / 1000.0f,
                g_seqByMap ? "counted as a cutscene: test box ticked" : "not counted; it would be with the test box ticked");
        s_cineT = 0;
    }
    // movie1: the game's own "a cutscene sequence is playing"
    static DWORD s_seqT, s_seqSince;
    static bool s_seqCut;
    if (g_seqMgrOk && t - s_seqT >= 50) {
        s_seqT = t;
        int playing = 0;
        bool c = SeqCutscenePlaying(&playing);
        if (c != s_seqCut) {
            static int s_n;
            char nb[40];
            s_seqCut = c;
            CutNote(c ? 14 : 15);
            if (s_n < 1500) {
                ++s_n;
                if (c) Log("seq: the game says a cutscene sequence is playing (%d sequences playing; picture from %s, cinematic flag %s, input mode %s)",
                           playing, gameCam ? "the on-foot camera" : "another camera", flag ? "on" : "off", SeqName(h, h2, nb, sizeof nb));
                else Log("seq: the game's cutscene sequence is over after %.1f s (%d other sequences playing)", (t - s_seqSince) / 1000.0f, playing);
            }
            s_seqSince = t;
        }
    }
    // A scene played through your own eyes that asks for a button (the bow handed to you,
    // a struggle) stays as it was before movie1: ordinary VR with your hands, until
    // g_seqHold seconds after the prompt. (With another camera it is the steady VR view
    // of seq1, below.)
    static DWORD s_promptT;
    if (kind == 2) s_promptT = t ? t : 1;
    else if (s_promptT && t - s_promptT >= (DWORD)(g_seqHold * 1000.0f)) s_promptT = 0;
    bool seqCut = g_seqBySeq && s_seqCut && !(g_seqInter3D && s_promptT && !camCut);
    bool rawCut = camCut || seqCut || (g_seqByMap && cineFoot);
    if (g_cutVeto && !rawCut) g_cutVeto = false;  // mark1: the "not a cutscene" lasts until this one is over
    bool want = rawCut && g_cutSteady && !g_cutVeto;
    if (want != s_flag) s_flag = want, s_chgT = t;
    if (want != g_cutscene && t - s_chgT >= (want ? 300u : 100u)) {  // flat1: back to play after 100 ms
        if (want) {
            InterlockedIncrement(&g_cutGen);
            g_cutscene = true, s_startT = t, s_interT = 0;
            CutNote(5);
            Log("cutscene: started (%s): %s%s", camCut ? "cinematic flag + cinematic camera" : seqCut ? (gameCam ? "the game's cutscene sequence, on-foot camera" : "the game's cutscene sequence") : "cinematic input mode, on-foot camera",
                g_cutFlat ? "shown on the 2D screen" : "the headset turns the view, the game's camera only moves it",
                g_vehicle ? "; vehicle mode paused" : "");
        } else {
            g_cutscene = false;
            InterlockedExchange(&g_cutEnded, 1);
            CutNote(6);
            Log("cutscene: ended after %.1f s (%s)", (t - s_startT) / 1000.0f,
                rawCut ? (g_cutSteady ? "you said it wasn't one" : "switched off") : flag ? "the on-foot camera is back" : "flag off");
        }
    }
    // seq1: a cutscene that asks for input (button prompts) is shown in VR, and stays
    // there for g_seqHold seconds after its last prompt (prompts come in runs).
    if (!g_cutscene) s_interT = 0;
    else if (kind == 2) s_interT = t ? t : 1;
    bool in3d = g_seqInter3D && g_cutscene && s_interT && t - s_interT < (DWORD)(g_seqHold * 1000.0f);
    if (in3d != g_cutInter) {
        if (in3d) InterlockedIncrement(&g_cutGen);  // the VR view takes its heading from the camera now
        g_cutInter = in3d;
        if (g_cutscene) {
            char nb[40];
            CutNote(in3d ? 12 : 13);
            Log("cutscene: %s", in3d ? "it asks for input: shown in VR, not on the 2D screen" : "no prompt for a while: back on the 2D screen");
            LogV("cutscene: input mode %s", SeqName(h, h2, nb, sizeof nb));
        }
    }
}
// bd7: Blood Dragon. The cinematic start is 0x1120c274: mov ecx,[0x11846744]; push 1; call 0x1005dfff,
// and that setter writes the same flag (mov [ecx+0x70],al; then clears +0x78). The player list is
// 0x11853560 (+8 array, +0xc count, as FC3) and the wrapper's vtable is 0x11530bd8, whose slot 2
// (0x102a9478) is byte for byte FC3's getter of the input-mode object.
static void CutsceneInstallBd() {
    BYTE *f = g_mod + 0x120c274, *set = g_mod + 0x5dfff;
    BYTE **gl = (BYTE **)(g_mod + 0x1846744);
    bool ok = !IsBadReadPtr(f, 0x18) && f[9] == 0x8b && f[10] == 0x0d && *(DWORD *)(f + 11) == (DWORD)gl && f[15] == 0x6a &&
              f[16] == 1 && f[17] == 0xe8 && f + 22 + *(int *)(f + 18) == set && !IsBadReadPtr(set, 12) &&
              !memcmp(set + 3, "\x8a\x45\x08\x0f\x57\xc0\x88\x41\x70", 9);
    if (ok) g_cinePtr = gl;
    Log(ok ? "cutscene: watching the game's cinematic flag (BD+11846744 +0x70)"
           : "cutscene: Blood Dragon's cinematic flag not found; cutscene handling off");
    kPlayersGlobal = 0x11853560, kPlayerWrapVt = 0x11530bd8;
    DWORD *vt = (DWORD *)(g_mod + (kPlayerWrapVt - kImageBase));
    BYTE *fn = IsBadReadPtr(vt, 12) ? nullptr : (BYTE *)vt[2];
    g_seqOk = fn && fn == g_mod + 0x2a9478 && !IsBadReadPtr(fn, 9) && !memcmp(fn, "\x8b\x41\x04\x05\x3c\x01\x00\x00\xc3", 9);
    Log("seq: the game's input mode list %s", g_seqOk ? "found" : "NOT found: cutscenes told by the flag and camera only");
    {
        // bd35 (movie1 for Blood Dragon): its "is a cutscene sequence playing" is BD+111fdf58 (thiscall, ecx = *(BD+11f02bf0)):
        // the same walk (+0x24 array, +0x28 count, +0x2c == 1 playing, byte +0xaa bIsCutscene), cast by BD+106ba1bb.
        // CCinematicSequence's vtable is BD+116ee088 (slots 30 / 32 = start BD+1120c274 / stop BD+1120c355).
        BYTE *w = g_mod + 0x11fdf58, *c = g_mod + 0x833d69;
        DWORD *svt = (DWORD *)(g_mod + 0x16ee088);
        g_seqMgrOk = !IsBadReadPtr(w, 0x30) && !memcmp(w, "\x8b\x41\x28\x56\x8b\x71\x24", 7) && !memcmp(w + 0x13, "\x83\x78\x2c\x01", 4) &&
                     !memcmp(w + 0x24, "\x80\xb8\xaa\x00\x00\x00\x00", 7) && !IsBadReadPtr(c, 6) && c[0] == 0x8b && c[1] == 0x0d &&
                     *(DWORD *)(c + 2) == (DWORD)g_mod + 0x1f02bf0 && !IsBadReadPtr(svt, 33 * 4) && svt[30] == (DWORD)g_mod + 0x120c274 &&
                     svt[32] == (DWORD)g_mod + 0x120c355;
        if (g_seqMgrOk) kSeqMgrGlobal = 0x11f02bf0, kSeqVt = 0x116ee088;
        Log("seq: the game's list of playing cutscene sequences %s", g_seqMgrOk ? "found: scripted scenes without control go on the movie screen"
                                                                                : "NOT found: cutscenes found by camera only");
    }
}
// The view during a cutscene: a level heading that only changes at a camera cut
// (the camera jumps over 1.5 m) or when the game's camera has faced over 70
// degrees away from it for 1.5 s (a long turn: it snaps, no slow swing).
static void CutsceneView(float *f, float *u, const float *pos) {
    static LONG s_gen = -1;
    static float s_yaw, s_pos[3];
    static DWORD s_awayT;
    float h = f[0] * f[0] + f[1] * f[1];
    float gy = h > 1e-4f ? atan2f(f[1], f[0]) : s_yaw;
    DWORD t = GetTickCount();
    LONG gen = g_cutGen;
    float d[3] = {pos[0] - s_pos[0], pos[1] - s_pos[1], pos[2] - s_pos[2]};
    if (gen != s_gen) {
        s_gen = gen, s_yaw = gy, s_awayT = 0;
    } else if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1.5f * 1.5f) {
        LogV("cutscene: camera cut (%.1f m): new heading %.0f deg", sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), gy * 57.3f);
        s_yaw = gy, s_awayT = 0;
    } else {
        float a = gy - s_yaw;
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        if (fabsf(a) < 1.22f) s_awayT = 0;
        else if (!s_awayT) s_awayT = t ? t : 1;
        else if (t - s_awayT >= 1500) {
            LogV("cutscene: the camera turned %.0f deg and stayed: new heading", a * 57.3f);
            s_yaw = gy, s_awayT = 0;
        }
    }
    memcpy(s_pos, pos, 12);
    f[0] = cosf(s_yaw), f[1] = sinf(s_yaw), f[2] = 0;
    u[0] = 0, u[1] = 0, u[2] = 1;
}

// glide1 (DrGonzaga 2 Nov): on the hang glider the game sways its camera, so the
// view wobbled. While hanging from the glider, the view's turn and the
// camera's place relative to the player are smoothed (0.35 s); the player's own
// movement is not, so the view doesn't lag behind the glider. VR page switch.
static volatile bool g_glider;
static volatile LONG g_kicks;  // recoil1: shots since the last scope frame
static volatile DWORD g_kickT;
void EngineScopeKick() {
    DWORD t = GetTickCount();
    if (t - g_kickT < 40) return;  // a shotgun's pellets are one shot
    g_kickT = t, InterlockedExchange(&g_kicks, 1);
}
bool g_gliderSteady = true;  // [vr] SteadyGlider
static void GliderSteady(float *f, float *pos) {
    static float s_f[3], s_off[3];
    static LARGE_INTEGER s_last;
    static bool s_have;
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
    BYTE *pl = GunAttachPlayer();
    if (!g_glider || !g_gliderSteady || !pl || IsBadReadPtr(pl + 0x50, 12)) {
        s_have = false;
        return;
    }
    const float *pp = (const float *)(pl + 0x50);
    float off[3] = {pos[0] - pp[0], pos[1] - pp[1], pos[2] - pp[2]};
    float dt = s_have ? (float)(now.QuadPart - s_last.QuadPart) / fq.QuadPart : 1.0f;
    s_last = now;
    float a = !s_have || dt > 0.3f ? 1.0f : 1.0f - expf(-dt / 0.35f);
    s_have = true;
    for (int i = 0; i < 3; ++i) s_f[i] += (f[i] - s_f[i]) * a, s_off[i] += (off[i] - s_off[i]) * a;
    float l = sqrtf(s_f[0] * s_f[0] + s_f[1] * s_f[1] + s_f[2] * s_f[2]);
    if (l < 1e-3f) return;
    for (int i = 0; i < 3; ++i) f[i] = s_f[i] / l, pos[i] = pp[i] + s_off[i];
}

extern "C" void __cdecl AimMatrix(float *mx, BYTE *cam, BYTE *owner) {
    g_gameThread = GetCurrentThreadId();
    LaserGameTick();  // ray1: beam wall stop from the game's collision (game thread)
    g_gameYaw = atan2f(mx[1], mx[0]), g_gameFwd[0] = mx[4], g_gameFwd[1] = mx[5];
    g_gamePitch = asinf(mx[6] > 1 ? 1 : mx[6] < -1 ? -1 : mx[6]);
    {
        float f[3] = {mx[4], mx[5], mx[6]}, u[3] = {mx[8], mx[9], mx[10]}, r[3];
        LevelBasis(f, u, r);
        LONG n = g_aimBasisIdx < 0 ? 0 : 1 - g_aimBasisIdx;
        memcpy(g_aimBasis[n], r, 12), memcpy(g_aimBasis[n] + 3, u, 12), memcpy(g_aimBasis[n] + 6, f, 12);
        InterlockedExchange(&g_aimBasisIdx, n);
    }
    g_gameLookTick = GetTickCount();
    g_footCam = (DWORD)cam;  // cut3
    if (g_cutscene) {
        // cut1: no vehicle test, look steering, laser or gun in hand during a cutscene
        // (g_aimLast isn't updated, so the mod counts as not on foot).
        g_bodyLive = false, g_glider = false, g_stick[0] = g_stick[1] = 0;
        return;
    }
    bool cutEnded = InterlockedExchange(&g_cutEnded, 0) != 0;
    if (cutEnded) {
        Log("cutscene: back to play: on foot until the game says otherwise%s (player %p)", g_vehicle ? " (was in vehicle mode)" : "",
            GunAttachPlayer());
        g_vehicle = false, g_glider = false;
        VehGameForget();
    }
    {
        BYTE *pl = GunAttachPlayer(), *link = pl ? *(BYTE **)(pl + 0x68) : nullptr;
        int bone = link && !IsBadReadPtr(link + 0x20, 4) ? *(int *)(link + 0x20) : -99;
        float camUp = 9;
        if (pl && g_rawCamTick && GetTickCount() - g_rawCamTick < 500) camUp = g_rawCamZ - *(float *)(pl + 0x58);  // move2: raw (base now includes the crouch lift)
        // vehgame2 (DrGonzaga 9 Nov): in a vehicle = the game's own record says so
        // (vehgame.cpp). The old seat bone + camera height test and its patches (crouch
        // after driving, healing, swimming, stale seat after getting out) are gone.
        // Kept: a weapon in hand for 0.5 s means on foot (passenger seats you shoot from;
        // the hands blip while driving, so it has to be held), and the hang glider
        // (hanging from it, camera < 0.6 m above the player) in case the game doesn't
        // count it as a vehicle.
        DWORD t = GetTickCount();
        bool empty = t - GunAttachHandTick() > 300;
        static DWORD s_heldSince;
        if (empty) s_heldSince = 0;
        else if (!s_heldSince) s_heldSince = t ? t : 1;
        bool held = s_heldSince && t - s_heldSince >= 500;
        // Swimming (link bone 50 then 49, camera ~1 m up) looks like a crouch to the
        // crouch test below, so that test leaves it out.
        bool swim = bone == 49 || bone == 50;
        int gv = VehGameInVehicle(pl);  // -1 = can't tell (loading, no player yet): stay as we are
        HolsterGameTick(gv >= 0 ? VehGamePawn() : nullptr);  // holster1: grip draws / puts away, on the game thread
        // cut2 (DrGonzaga 9 Nov): the hang glider backup guess (low camera, empty hands) is gone:
        // it fired on a cutscene camera and held vehicle mode. The game's own record decides.
        // cut1: "can't tell" used to keep the last state for good, so vehicle mode could
        // stick (no player found after a cutscene). Now it lasts 2 s at most.
        static DWORD s_unknownT;
        if (gv >= 0) s_unknownT = 0;
        else if (!s_unknownT) s_unknownT = t ? t : 1;
        bool lost = gv < 0 && s_unknownT && t - s_unknownT >= 2000;
        bool seated = gv < 0 ? (lost ? false : g_vehicle) : gv == 1 && !held;
        // shopveh1 (DrGonzaga 16 Nov: the ammo vending machine and the contracts terminal turned him round, as the
        // ladder fix does). Using one, the game puts the player in a "seat" (seat bone 3, camera 0.82 m up) and says
        // "in a vehicle" while its "cinematic" input mode plays the walk-up, then mode 5c7de9b9 (the shop screen);
        // leaving vehicle mode reseeds the view from the game's look, which faced elsewhere. Cars go through the same
        // "cinematic" mode first and then "driving" (11a7b343). So while the input mode is "cinematic" the vehicle
        // state is left as it is, and in the shop screen it is "on foot".
        {
            const DWORD im = g_seqHash;
            if (im == 0x1eb7b814) seated = g_vehicle;  // crc32("cinematic"): wait for what follows
            else if (im == 0x5c7de9b9) seated = false;  // the shop / terminal screen
        }
        static bool s_raw;
        static DWORD s_since;
        if (cutEnded) s_raw = false, s_since = t, s_unknownT = 0;
        if (seated != s_raw) s_raw = seated, s_since = t;
        g_vehDriving = s_raw && g_vehicle && g_seqHash != 0x1eb7b814;  // vehface1: not once "cinematic" (getting out) or on foot
        if (seated != g_vehicle && t - s_since >= 250) {
            g_vehicle = seated;
            // vehrec1 (DrGonzaga 16 Nov: getting into a vehicle kept the old "forward", so he had to turn round
            // physically to line up with it): on getting in, the view is recentred, so wherever you are facing
            // becomes the front of the vehicle (in a vehicle the view follows the game camera, which looks ahead).
            // vehrec2 (DrGonzaga 16 Nov: "you get out of a vehicle - you are facing the game's default"): getting out
            // recentres too; the body then takes the game's look (stale look below), so you face where the game puts you.
            // vehface1: not on getting out while VehicleExitKeepFacing is on (the facing is kept instead, AimMatrix)
            if (g_vehRecentre && (seated || !g_vehExitKeep)) HeadtrackRequestRecenter(), Log("engine: %s", seated ? "into a vehicle: view recentred to face its front" : "out of a vehicle: view recentred to face where the game faces you");
            Log("engine: %s (%s; seat bone %d, camera %.2f m above the player, input mode %08lx)",
                seated ? "in a vehicle: view turns with it" : "out of the vehicle: view on foot",
                gv == 1 ? (held ? "the game says in a vehicle, gun in hand" : "the game says in a vehicle")
                : gv < 0 ? (pl ? "the game's vehicle record can't be read for 2 s" : "no player found for 2 s") : "the game says on foot",
                bone, camUp, (unsigned long)g_seqHash);
        }
        // turret1 (DrGonzaga 9 Nov): on a mounted gun (turret.cpp) the view is handled like a
        // vehicle (it turns with the gun, the game gets the right stick) whatever the seat
        // test above says: it flipped in and out on the boat turret (healing, pauses).
        bool tur = EngineTurret();
        {
            static bool s_tur;
            static float s_hSum;
            static int s_hN;
            static DWORD s_turT;
            if (tur != s_tur) {
                s_tur = tur;
                if (tur) {
                    s_hSum = 0, s_hN = 0, s_turT = t, g_turretEye = 0;
                    float gp[3], rel[3] = {0, 0, 0};
                    bool have = pl && TurretGunPos(gp);
                    if (have) for (int i = 0; i < 3; ++i) rel[i] = gp[i] - ((const float *)(pl + 0x50))[i];
                    Log("turret: on a mounted gun: %s (", g_turretMotion ? "it follows the right controller" : "the right stick aims it");
                    Log("turret: (camera %.2f m above the player, seat bone %d, gun %s %.2f %.2f %.2f m from the player)",
                        camUp, bone, have ? "at" : "not found,", rel[0], rel[1], rel[2]);
                } else {
                    Log("turret: off the mounted gun (eye height was %.2f m; %ld updates from other gunners)", g_turretEye, TurretOtherUsers());
                }
            }
            // The eye height: the game camera's height above the player over the first 0.6 s.
            if (tur && t - s_turT < 600 && camUp > 0.5f && camUp < 2.2f) {
                s_hSum += camUp, ++s_hN;
                float h = s_hSum / s_hN;
                g_turretEye = h < 0.8f ? 0.8f : h > 1.9f ? 1.9f : h;
            }
        }
        bool veh = g_vehicle || tur;
        if (veh) g_vehSeenT = t;  // keepview2
        // crouchsteady1 (DrGonzaga 6 Nov): crouched = on foot with the game camera
        // ~0.95-1.05 m above the player (standing ~1.66). Hysteresis, 200 ms debounce.
        {
            static DWORD s_cs;
            bool c = !veh && !swim && pl && camUp > 0.6f && camUp < (g_crouched ? 1.40f : 1.30f);
            if (c == g_crouched) s_cs = t;
            else if (t - s_cs >= 200) g_crouched = c, s_cs = t;
            if (!veh && !swim && pl && !g_crouched && camUp > 1.5f && camUp < 1.9f) g_standUp += (camUp - g_standUp) * 0.02f;
        }
        {   // realcrouch1
            static bool s_low;
            static DWORD s_pressT;
            bool live = !veh && !swim && pl && t - g_headUpTick < 300 && !MenuOpen();
            float drop = -g_headUp;
            s_low = live && g_realCrouch && (s_low ? drop > g_crouchDepth - 0.06f : drop > g_crouchDepth);
            if (!g_crouched && !s_low) g_ourCrouch = false;
            if (live && g_realCrouch && t - s_pressT > 1200) {
                if (s_low && !g_crouched) g_crouchPulse = t + 120, s_pressT = t, g_ourCrouch = g_viewComp = true, Log("engine: real crouch: head %.2f m down, crouching", drop);
                else if (!s_low && g_crouched && g_ourCrouch) g_crouchPulse = t + 120, s_pressT = t, g_ourCrouch = false, Log("engine: real crouch: head back up, standing");
            }
            if (g_ourCrouch && g_crouched) g_viewComp = true;
            if (veh || swim || (!g_ourCrouch && camUp > g_standUp - 0.05f) || (!g_ourCrouch && !g_crouched && t - s_pressT > 1500)) g_viewComp = false;
        }
        g_glider = g_vehicle && !tur && camUp < 0.5f;  // glide1: hanging (glider ~0.05-0.35 m), not seated (~0.75 m)
        // bd13 (DrGonzaga 12 Nov): on a mounted gun with [vr] TurretMotion, carry on as on foot: the view
        // holds our body direction and the game's look (= the gun) is steered to the right controller.
        g_turSteer = tur && g_turretMotion;
        {   // turride1: on a mounted gun on a vehicle (the opening helicopter), the vehicle object, if it is not the gun
            // itself and is a sensible distance from it; the body turns with its heading
            static float s_prevYaw;
            static bool s_prevOk;
            static BYTE *s_logged;
            // turscript1 (DrGonzaga 17 Nov, bd96: on a turret reached on foot "I appeared about 2 meters away from the turret.
            // ... all of the turrets EXCEPT the helicopter scripted one were really good before"): the bd85-bd96 turret
            // changes (anchor, held above the gun, riding the vehicle) were all made for the opening helicopter. They now
            // apply only to a scripted ride: the game's cinematic flag was on at some point since getting on (it is on for
            // the whole helicopter ride and off on a turret you walk up to). Other turrets are as before bd85.
            // (g_turScripted is set in ApplyHead's turret block, which runs every frame)
            BYTE *V0 = tur && g_turScripted ? VehGameVehicleEnt() : nullptr;
            if (V0 && V0 != TurretGunEnt()) g_turVehSeen = true;  // turdragon1: not the dragon
            BYTE *V = g_turretRide && g_turretAbove && g_turretSteady ? V0 : nullptr;
            BYTE *G = V ? TurretGunEnt() : nullptr;
            float gp[3];
            if (V && G && V != G && TurretGunPos(gp)) {
                const float *M = (const float *)(V + 0x20);
                float dx = M[12] - gp[0], dy = M[13] - gp[1], dz = M[14] - gp[2], dd = sqrtf(dx * dx + dy * dy + dz * dz);
                // turheli1 (DrGonzaga 17 Nov, bd98: a scripted turret reached on foot was offset again): that turret's own
                // base is the "vehicle", 0.94 m from the gun; the opening helicopter's object is 2.57 m from it. Only a real
                // vehicle (over 1.5 m away) counts, and only then do the helicopter's view rules apply (g_turHeli).
                bool ok = dd > 1.5f && dd < 30.0f && fabsf(M[4]) + fabsf(M[5]) > 0.05f;
                if (ok) g_turHeli = true;
                if (s_logged != V) s_logged = V, Log("turret: the vehicle under the gun: object %p, %.2f m from the gun, heading %.0f deg (%s)", V, dd,
                                                    atan2f(M[5], M[4]) * 57.2958f, ok ? "the view rides it" : "not used");
                if (!ok) V = nullptr;
            } else V = nullptr;
            // turdragon1: on the dragon, the gun's own object stands in for the vehicle, once it has shown that it keeps
            // its heading while you aim (turned by the dragon only); only its heading is used (no dip or roll)
            if (!V && tur && g_turDragon && g_turDragonFrame && g_turretRide && g_turretAbove && g_turretSteady) {
                BYTE *GD = TurretGunEnt();
                if (GD && !IsBadReadPtr(GD + 0x20, 64)) {
                    const float *M = (const float *)(GD + 0x20);
                    if (fabsf(M[4]) + fabsf(M[5]) > 0.05f) V = GD;
                }
            }
            if (V) {
                const float *M = (const float *)(V + 0x20);
                float yaw = atan2f(M[5], M[4]);
                if (s_prevOk && g_bodyLive) {
                    float d = Wrap(yaw - s_prevYaw), c = cosf(d), sn = sinf(d), bx = g_body[0], by = g_body[1];
                    if (fabsf(d) < 0.5f) SetBody(bx * c - by * sn, bx * sn + by * c);
                }
                s_prevYaw = yaw, s_prevOk = true;
            } else s_prevOk = false;
            g_rideEnt = V;
        }
        if (veh && g_car[1] && !g_turSteer) {
            // Not on foot: g_aimLast left alone, so getting out reseeds the body from the game's look.
            g_bodyLive = false, g_stick[0] = g_stick[1] = 0;
            return;
        }
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    auto &s = g_steer;
    float dt = s.last.QuadPart ? (float)(now.QuadPart - s.last.QuadPart) / freq.QuadPart : 0;
    s.last = now;
    if (dt > 0.1f) dt = 0.1f;
    DWORD t = GetTickCount();
    bool stale = t - g_aimLast > 300;  // back from a vehicle, menu or load
    g_aimLast = t;
    LONG idx = g_headIdx;
    if (s.phase == -2 && t >= s.retryAt) s.phase = 0;  // look2: try learning again
    // cybereye1: on the flat screen the head isn't applied to the picture, but the look still follows it
    static DWORD s_cyT;
    if (CyberEyeOn()) s_cyT = t;
    if (!g_lookMode || s.phase < 0 || idx < 0 || (t - g_lastApplied > 1000 && (!s_cyT || t - s_cyT > 1000))) {
        g_bodyLive = false;
        if (s.phase > 0) s.phase = 0;
        g_stick[0] = g_stick[1] = 0;
        return;
    }
    const float gf[3] = {mx[4], mx[5], mx[6]};
    float gy = atan2f(gf[1], gf[0]);
    float gp = asinf(gf[2] > 1 ? 1 : gf[2] < -1 ? -1 : gf[2]);
    // keepbody1 (DrGonzaga 17 Nov: pressing A to skip a tutorial moved the view "approx. 5 or 10 degrees to the left"):
    // after a pause the body was set again from the game's look and turned by the head, which shifted the view by
    // however far the game's look was from the view. If the game has not turned you meanwhile (its look within 15 deg
    // of where it was), nothing is touched now: you carry on exactly as before the pause.
    static float s_lastGy;
    static bool s_lastGyOk;
    const float lookMoved = s_lastGyOk ? Wrap(gy - s_lastGy) : 9.f;
    const bool keepBody = stale && s.phase != 0 && g_keepView && t - g_vehSeenT > 1500 && fabsf(lookMoved) < 0.26f;
    s_lastGy = gy, s_lastGyOk = true;
    if (keepBody) {
        static int s_kn;
        if (s_kn++ < 40) Log("engine: back on foot: kept your facing (the game's look moved %.0f deg)", lookMoved * 57.2958f);
        s.bigErr = 0;
    } else if (s.phase == 0 || stale) {
        // Start from where the game looks, so the view doesn't jump.
        SetBody(gf[0], gf[1]);
        // keepview1 (DrGonzaga 16 Nov: after a vehicle, the ammo vending machine or a contracts terminal the view came
        // back turned): the body took the game's look, but the picture is the body turned by the head, so a head
        // turned away from where it was last recentred turned the view by that much. Now the body is set so that
        // where the head looks now is where the game looks.
        // keepview2 (DrGonzaga 17 Nov: "now when I'm on foot and I pause, I turn 180 degrees"): bd89 had switched this
        // off for vehicles; now it is on again for a pause, menu, shop, terminal or load, and left out only just after
        // a vehicle or mounted gun (there the game's own facing is wanted).
        const bool fromVeh = stale && g_vehExitKeep && g_vehViewT && t - g_vehViewT < 8000 && t - g_vehSeenT < 1500;  // vehface1
        if (fromVeh) {
            // vehface1: the view carries on looking the way it looked while driving (head turn included)
            float f0[3] = {g_body[0], g_body[1], 0}, u0[3] = {0, 0, 1}, r0[3], hr[3], hu[3], hf[3];
            Cross(f0, u0, r0);
            TurnByHead(r0, u0, f0, g_heads[idx].m, hr, hu, hf);
            if (hf[0] * hf[0] + hf[1] * hf[1] > 1e-4f) {
                const float want = g_vehViewYaw;
                float d = -Wrap(atan2f(hf[1], hf[0]) - want), c = cosf(d), sn = sinf(d), bx = g_body[0], by = g_body[1];
                SetBody(bx * c - by * sn, bx * sn + by * c);
                g_vehKeepT = t | 1;
                Log("engine: out of a vehicle: kept your facing from driving (%.0f deg from where the game faces you, taken %lu ms ago)",
                    Wrap(want - gy) * 57.2958f, (unsigned long)(t - g_vehViewT));
            }
            g_vehViewT = 0;
        } else if (stale && g_keepView && t - g_vehSeenT > 1500) {
            float f0[3] = {g_body[0], g_body[1], 0}, u0[3] = {0, 0, 1}, r0[3], hr[3], hu[3], hf[3];
            Cross(f0, u0, r0);
            TurnByHead(r0, u0, f0, g_heads[idx].m, hr, hu, hf);
            if (hf[0] * hf[0] + hf[1] * hf[1] > 1e-4f) {
                float d = -Wrap(atan2f(hf[1], hf[0]) - gy), c = cosf(d), sn = sinf(d), bx = g_body[0], by = g_body[1];
                SetBody(bx * c - by * sn, bx * sn + by * c);
                Log("engine: back on foot: the view picks up where the game looks (head was %.0f deg off the body)", -d * 57.2958f);
            }
        }
        s.bigErr = 0;
        if (s.calibrated) {
            s.phase = 3;
        } else {
            // Learn how fast the game turns for a full stick: right for a moment, then up.
            s.phase = 1, s.frames = 0, s.y0 = gy, s.t0 = 0;
            g_stick[0] = 1, g_stick[1] = 0;
        }
    }
    if (g_reseedT && t - g_reseedT > 80) {  // loadface1: the recentre has reached the head by now
        const bool fresh = t - g_reseedT < 3000;  // (not on foot then: left alone)
        g_reseedT = 0;
        float f0[3] = {g_body[0], g_body[1], 0}, u0[3] = {0, 0, 1}, r0[3], hr[3], hu[3], hf[3];
        Cross(f0, u0, r0);
        TurnByHead(r0, u0, f0, g_heads[idx].m, hr, hu, hf);
        if (fresh && s.phase != 0 && hf[0] * hf[0] + hf[1] * hf[1] > 1e-4f) {
            float d = -Wrap(atan2f(hf[1], hf[0]) - gy), c = cosf(d), sn = sinf(d), bx = g_body[0], by = g_body[1];
            SetBody(bx * c - by * sn, bx * sn + by * c);
            s.bigErr = 0;
            Log("engine: after the recentre on loading: the view lined up with where the game looks again (it was %.0f deg off)", -d * 57.2958f);
        }
    }
    g_bodyLive = true;

    // Turn the body with the right stick (the game gets our stick instead).
    float x = g_rightX / 32767.f;
    float ax = fabsf(x);
    if (g_snapOn) {
        // snap1 (DrGonzaga 11 Nov): a hard push left/right turns the body by a fixed angle
        // once; the stick must come back towards the centre before the next snap.
        // Only a mostly sideways push counts, so stick up (arrow type) / down (crouch) never snap.
        static bool s_snapArmed = true;
        if (ax < 0.35f) s_snapArmed = true;
        else if (s_snapArmed && ax > 0.75f && ax > fabsf(g_rightY / 32767.f)) {
            s_snapArmed = false;
            float turn = -(x > 0 ? 1 : -1) * g_snapDeg * 0.0174533f;  // right = clockwise
            float c = cosf(turn), sn = sinf(turn), bx = g_body[0], by = g_body[1];
            SetBody(bx * c - by * sn, bx * sn + by * c);
            LogV("lookaim: snap turn %s %.0f deg", x > 0 ? "right" : "left", g_snapDeg);
        }
    } else if (ax > 0.15f) {
        float turn = -(x > 0 ? 1 : -1) * (ax - 0.15f) / 0.85f * 2.6f * dt;  // up to 2.6 rad/s, right = clockwise
        float c = cosf(turn), sn = sinf(turn), bx = g_body[0], by = g_body[1];
        SetBody(bx * c - by * sn, bx * sn + by * c);
    }

    if (s.phase == 1 || s.phase == 2) {
        s.t0 += dt;
        if (++s.frames < 24) return;
        if (s.phase == 1) {
            s.sYaw = Wrap(gy - s.y0) / s.t0;  // rad/s for a full stick right
            if (fabsf(s.sYaw) < 0.05f) {
                // look2 (2 Nov): this happened in an opening cutscene (the game ignores the
                // stick there) and left look steering off for the whole session, so the
                // view followed the game's camera sway. Now it tries again in 3 s.
                Log("lookaim: the game didn't turn for the stick (%.3f rad/s); trying again in 3 s", s.sYaw);
                s.phase = -2, s.retryAt = t + 3000, g_bodyLive = false, g_stick[0] = g_stick[1] = 0;
                return;
            }
            s.phase = 2, s.frames = 0, s.p0 = gp, s.t0 = 0;
            g_stick[0] = 0, g_stick[1] = 1;
            return;
        }
        s.sPitch = (gp - s.p0) / s.t0;
        if (fabsf(s.sPitch) < 0.05f) {
            Log("lookaim: the game didn't tilt for the stick; trying again in 3 s");
            s.phase = -2, s.retryAt = t + 3000, g_bodyLive = false, g_stick[0] = g_stick[1] = 0;
            return;
        }
        Log("lookaim: a full stick turns the game %.2f rad/s and tilts it %.2f rad/s (saved)", s.sYaw, s.sPitch);
        s.calibrated = true;
        SaveLookRate(L"LookTurnRate", s.sYaw), SaveLookRate(L"LookTiltRate", s.sPitch);  // look3
        s.phase = 3;
    }

    {
        // ladder1 (DrGonzaga 15 Nov: "when I climb down, I'm facing out. Any way to have the camera spin 180"): the
        // game turns its own look to the ladder as you get on, but the picture is the body direction plus the head,
        // so getting on from the top left you with your back to it. For the first half second on a ladder the game's
        // look is left alone; then, if the head faces more than 100 degrees away from it, the body is turned so
        // the head faces where the game looks.
        {  // ziprec1 (DrGonzaga 15 Nov: "can we do a recentre after zipline (had some confusion as to where body was after)")
            static bool s_zip;
            const bool zip = g_seqHash == 0xff0b2e0e;  // crc32("ziplining")
            if (s_zip && !zip && g_zipRecentre) HeadtrackRequestRecenter(), Log("zipline: off the line; view recentred");
            s_zip = zip;
        }
        static bool s_lad, s_done;
        static DWORD s_ladT;
        static float s_gy0;
        const bool lad = g_ladderTurn && g_seqHash == 0x37bdbc25;  // crc32("ladder")
        if (lad && !s_lad) s_ladT = t, s_done = false, s_gy0 = gy;
        s_lad = lad;
        if (lad && !s_done) {
            if (t - s_ladT < 500) {
                g_stick[0] = g_stick[1] = 0;
                return;
            }
            s_done = true;
            float f0[3] = {g_body[0], g_body[1], 0}, u0[3] = {0, 0, 1}, r0[3], hr[3], hu[3], hf[3];
            Cross(f0, u0, r0);
            TurnByHead(r0, u0, f0, g_heads[idx].m, hr, hu, hf);
            float hy = atan2f(hf[1], hf[0]), d = Wrap(gy - hy);
            const bool turn = fabsf(d) > 1.745f;
            if (turn) {
                float c = cosf(d), sn = sinf(d), bx = g_body[0], by = g_body[1];
                SetBody(bx * c - by * sn, bx * sn + by * c);
            }
            Log("ladder: on a ladder: the game's look turned %.0f deg in the first half second; the head faced %.0f deg from it: %s", Wrap(gy - s_gy0) * 57.2958f,
                d * 57.2958f, turn ? "view turned to face the ladder" : "left as it is");
        }
    }
    // Where we want the game to look, in the world, from the body direction:
    // along the right controller (mode 2) or the head (mode 1).
    float m[16];
    const bool cy = CyberEyeOn();  // cybereye1: the look follows the head (smoothed below)
    if (cy) {
        memcpy(m, g_heads[idx].m, 64);
    } else if (g_lookMode == 2) {
        // holster2 (DrGonzaga 10 Nov): reaching to a holster spot (behind the head, the hips)
        // swung the look, and with it the view: the look holds where it was meanwhile.
        static float s_aimM[16];
        static bool s_haveAim;
        if (!HeadtrackHand(1, m)) memcpy(m, g_heads[idx].m, 64);
        else if (BowActive()) BowZeroXr(m);  // quiver5: the bow's zeroing turns the game's look
        if (HolsterAimPaused() && s_haveAim) memcpy(m, s_aimM, 64);
        else memcpy(s_aimM, m, 64), s_haveAim = true;
        float tm[16];
        if (HolsterThrowAim(tm)) memcpy(m, tm, 64);  // nade1: physical throw, the look along the swing
    } else {
        memcpy(m, g_heads[idx].m, 64);
    }
    float f[3] = {g_body[0], g_body[1], 0}, u[3] = {0, 0, 1}, r[3], nr[3], nu[3], nf[3];
    Cross(f, u, r);
    TurnByHead(r, u, f, m, nr, nu, nf);
    float dy = atan2f(nf[1], nf[0]);
    float dp = asinf(nf[2] > 1 ? 1 : nf[2] < -1 ? -1 : nf[2]);
    if (cy) {
        // cybereye1 (DrGonzaga: "control camera with head, even when zoomed, but very smoothed")
        static float s_y, s_p;
        static DWORD s_last;
        float tau = CyberEyeSmoothing();
        if (t - s_last > 300 || tau < 0.01f) s_y = dy, s_p = dp;
        else {
            float k = 1 - expf(-dt / tau);
            s_y = Wrap(s_y + Wrap(dy - s_y) * k), s_p += (dp - s_p) * k;
        }
        s_last = t, dy = s_y, dp = s_p;
    }
    float ey = Wrap(dy - gy), ep = dp - gp;
    g_cyErr[0] = cy ? ey : 0, g_cyErr[1] = cy ? ep : 0, g_cyErrT = cy ? t : 0;  // cyberdirect1
    // The game is turning the player itself (a takedown, a climb) and won't
    // follow the stick: after a while, go with it.
    const bool keptExit = g_vehKeepT && t - g_vehKeepT < 3000;  // vehface1: the game may still be playing its getting-out
    if (!g_turSteer && !keptExit && fabsf(ey) > 1.0f && fabsf(ey) >= s.prevErr - 0.002f) {  // (a mounted gun at its stop: the view stays)
        if (++s.bigErr > 90) {
            float c = cosf(-ey), sn = sinf(-ey), bx = g_body[0], by = g_body[1];
            SetBody(bx * c - by * sn, bx * sn + by * c);
            s.bigErr = 0;
            // veh6: if this repeats while driving, the vehicle test missed; say why.
            Log("lookaim: the game turned the player %.0f degrees itself; following it", -ey * 57.3f);
        }
    } else {
        s.bigErr = 0;
    }
    s.prevErr = fabsf(ey);
    // Turn rate wanted: close the gap in about 1/8 s; then the stick that gives it.
    // The game ignores small deflections (dead zone), so we start above it.
    // quiver7 (DrGonzaga 11 Nov: arrows all over the place): the arrow leaves along the game's look,
    // which trails the bow by 0-4 deg at the loose; while an arrow is drawn it is pulled in harder.
    // (quiver6 turned the look matrix itself instead: the game's look then drifted 9-60 deg.)
    const float gain = g_steerGain * (cy ? 2.0f : BowArrowsAlongAim() || HolsterThrowAimActive() ? BowSteerBoost() : 1.0f), dz = g_steerDz;  // cybereye1: zoomed, the game turns slower for the same stick  // nade1
    auto Stick = [&](float err, float full) -> float {
        if (!full || fabsf(err) < (cy ? 0.001f : 0.004f)) return 0;
        float v = gain * err / full;
        float a = fabsf(v);
        a = dz + (1 - dz) * (a > 1 ? 1 : a);
        return v > 0 ? a : -a;
    };
    g_stick[0] = Stick(ey, s.sYaw);
    g_stick[1] = Stick(ep, s.sPitch);
    float e = sqrtf(ey * ey + ep * ep);
    s.errSum += e, s.errN++;
    if (e > s.errMax) s.errMax = e;
}

// Camera basis as the engine builds it: right = forward x up; level first
// when the horizon lock is on.
static void LevelBasis(float *f, float *u, float *r) {
    Normalize(f);
    if (g_horizon && (!g_vehicle || g_car[3]) && f[0] * f[0] + f[1] * f[1] > 1e-4f) {
        f[2] = 0, Normalize(f);  // world up is +z
        u[0] = 0, u[1] = 0, u[2] = 1;
    }
    Cross(f, u, r);
    Normalize(r);
    Cross(r, f, u);  // up made exactly perpendicular
}

// room1 (roomscale): walking in the room should move the player, not just
// the view (6DoF alone leaves the body behind, up to 1 m). When the head is
// more than g_roomStart from the body, the left stick walks the body towards
// it (the game moves relative to its own look), and every bit the body moves
// meanwhile is taken off the head offset (HeadtrackShiftAnchor), so the view
// stays still in the world while the body catches up. Your own left stick
// always wins. VR page switch.
static bool g_room = true;
static float g_roomStart = 0.30f, g_roomStop = 0.06f;  // room2: start 30 cm (DrGonzaga), slider
static volatile float g_roomStick[2];
static volatile bool g_roomWalk;
static volatile DWORD g_userStickTick;
bool *EngineRoomScale() { return &g_room; }
float *EngineRoomStart() { return &g_roomStart; }
void EngineUserMoved() { g_userStickTick = GetTickCount(); }
bool EngineRoomStick(SHORT *x, SHORT *y) {
    if (!g_roomWalk) return false;
    *x = (SHORT)(g_roomStick[0] * 32767), *y = (SHORT)(g_roomStick[1] * 32767);
    return true;
}
static void RoomScale(const Head &h, const float *base, const float *fr, const float *ff) {
    static float s_last[3];
    static bool s_have, s_walking;
    static LONG s_steps;
    DWORD t = GetTickCount();
    bool ok = g_room && EngineOnFoot() && !g_vehicle && t - g_userStickTick > 300 && t - g_gameLookTick < 300;
    if (ok && s_have && s_walking) {
        // What the body moved since last frame, on the level axes, taken off the head offset.
        float d[3] = {base[0] - s_last[0], base[1] - s_last[1], 0};
        float dx = d[0] * fr[0] + d[1] * fr[1], dz = -(d[0] * ff[0] + d[1] * ff[1]);
        float l = sqrtf(dx * dx + dz * dz);
        if (l < 0.5f) HeadtrackShiftAnchor(dx * g_worldScale, dz * g_worldScale);  // (a jump means a teleport / load)
    }
    memcpy(s_last, base, 12), s_have = true;
    // Head offset from the body, level, in game metres and in the world.
    float ox = h.off[0] / g_worldScale, oz = h.off[2] / g_worldScale;
    float e = sqrtf(ox * ox + oz * oz);
    if (!ok) s_walking = false;
    else if (!s_walking && e > g_roomStart) s_walking = true, ++s_steps;
    else if (s_walking && e < g_roomStop) s_walking = false;
    if (s_walking) {
        float w[2] = {fr[0] * ox - ff[0] * oz, fr[1] * ox - ff[1] * oz};  // towards the head, world x y
        float lf[2] = {g_gameFwd[0], g_gameFwd[1]};
        float ll = sqrtf(lf[0] * lf[0] + lf[1] * lf[1]);
        if (ll < 1e-3f) {
            s_walking = false;
        } else {
            lf[0] /= ll, lf[1] /= ll;
            float sy = (w[0] * lf[0] + w[1] * lf[1]) / e, sx = (w[0] * lf[1] - w[1] * lf[0]) / e;  // right = (fy, -fx)
            float m = 0.35f + e * 1.5f;
            if (m > 1) m = 1;
            g_roomStick[0] = sx * m, g_roomStick[1] = sy * m;
        }
    }
    g_roomWalk = s_walking;
    static DWORD s_log;
    if (t - s_log > 10000) {
        s_log = t;
        LogV("engine: roomscale %s: %ld walks in 10 s, head %.2f m from the body now", g_room ? "on" : "off", s_steps, e);
        s_steps = 0;
    }
}

// sound1 (1 Nov): the game's sound listener follows the game camera, which
// on foot is steered by the right controller (lookaim), so sounds were placed
// around the gun, not the head. Found with a one-off read-breakpoint run (listen2, removed):
// 0x11662b70 (called once a game frame from 0x116ff970 at 0x116ffa3d, struct
// in edi) copies the camera's position (+8 -> +0x18), right (+0x80 -> +0x30),
// forward (+0x68 -> +0x3c) and up (+0x74 -> +0x48), plus the player's
// velocity (+0x24). After it runs we put in the head's view as last rendered.
bool g_soundHead = true;  // [vr] SoundFollowsHead (VR page)
static struct Lis { float f[3], u[3], d[3]; DWORD tick; } g_lis[2];
static volatile LONG g_lisIdx = -1;
static void PublishListener(const float *f, const float *u, const float *pos, const float *base) {
    LONG n = g_lisIdx < 0 ? 0 : 1 - g_lisIdx;
    memcpy(g_lis[n].f, f, 12), memcpy(g_lis[n].u, u, 12);
    for (int i = 0; i < 3; ++i) g_lis[n].d[i] = pos[i] - base[i];  // head position (6DoF/roomscale), no eye offset
    g_lis[n].tick = GetTickCount();
    InterlockedExchange(&g_lisIdx, n);
}
extern "C" void __cdecl ListenerFix(BYTE *l) {
    LONG i = g_lisIdx;
    if (!g_soundHead || i < 0 || !l || GetTickCount() - g_lis[i].tick > 200) return;
    Lis v = g_lis[i];
    float *pos = (float *)(l + 0x18), *r = (float *)(l + 0x30), *f = (float *)(l + 0x3c), *u = (float *)(l + 0x48);
    // Keep the game's handedness: is its right = forward x up, or up x forward?
    float c[3];
    Cross(f, u, c);
    float sgn = c[0] * r[0] + c[1] * r[1] + c[2] * r[2] < 0 ? -1.f : 1.f;
    float nr[3];
    Cross(v.f, v.u, nr);
    Normalize(nr);
    for (int k = 0; k < 3; ++k) f[k] = v.f[k], u[k] = v.u[k], r[k] = nr[k] * sgn, pos[k] += v.d[k];
}

// soundbd1 (bd80; DrGonzaga 15 Nov, Blood Dragon: "the directional audio seems to lag behind head position"): the
// listener hook was never ported, so in Blood Dragon sound followed the game camera (the right controller's
// look). The same copy is BD+10366757 (thiscall, the listener struct in ecx, same fields), called once, at
// BD+102a9a4d.
static void *g_lisBd;
static void __attribute__((thiscall)) BdListenerHook(BYTE *l) {
    BdThisCall(g_lisBd, l, 0);
    if (l) ListenerFix(l);
}
static void ListenerInstallBd(BYTE *mod) {
    BYTE *site = mod + (0x102a9a4d - kImageBase), *fn = mod + (0x10366757 - kImageBase);
    static const BYTE kHead[8] = {0x55, 0x8b, 0xec, 0x0f, 0x57, 0xc0, 0x83, 0xec};
    if (site[0] != 0xe8 || site + 5 + *(int *)(site + 1) != fn || memcmp(fn, kHead, sizeof kHead)) {
        Log("engine: Blood Dragon sound listener call doesn't match; sound stays with the game camera");
        return;
    }
    g_lisBd = fn;
    if (!PatchCallTo(site, (void *)BdListenerHook)) return;
    Log("engine: sound listener hooked at BD+102a9a4d (follows the head: %d)", (int)g_soundHead);
}

// oc1 (11 Nov; oc3: VR page "Off-centre eye view", [render] OffCentre, on): each eye of
// the headset sees further outwards than towards the nose and further down than up
// (Quest 3 through Virtual Desktop: 52.5 / 38.5 / 39.3 / 50.5 degrees), but the game
// draws a centred picture, so a third of it is never shown. The camera has a matrix
// at +0x3c0 that UpdateMatrices multiplies onto the projection (proj = proj * M, row
// vectors: FC3+115043a5), normally the identity. Writing a zoom and shift there aims
// the picture at exactly what the eye shows; view, culling and field of view stay
// those of the centred camera (a superset). The overlays follow through
// EngineEyeViewport, the headset is told the real view (XrSetImageFov).
static bool g_ocOn = true, g_ocBad, g_ocWrote;  // [render] OffCentre (oc3: saved, on)
static float g_ocCur[4] = {1, 0, 1, 0};  // the camera being built: zoom x, shift x, zoom y, shift y
bool *EngineOffCentreOn() { return &g_ocOn; }
static const float kOcId[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// One of ours: the identity but for a zoom (1 to 2) and a shift in x and y.
static bool OcOurs(const float *m) {
    if (!g_ocWrote || !(m[0] > 1.0f && m[0] < 2.0f && m[5] > 1.0f && m[5] < 2.0f && fabsf(m[12]) < 1 && fabsf(m[13]) < 1)) return false;
    float t[16];
    memcpy(t, m, 64);
    t[0] = t[5] = 1, t[12] = t[13] = 0;
    return !memcmp(t, kOcId, 64);
}
static void OcPut(float *m, float sx, float ox, float sy, float oy) {
    memcpy(m, kOcId, 64);
    m[0] = sx, m[12] = ox, m[5] = sy, m[13] = oy;
}
// oc2: what the main camera of the eye being drawn was given, for the cameras the game derives from it.
static DWORD g_ocThread;
static float g_ocFov, g_ocAspect, g_ocRight[3];
static volatile DWORD g_ocTick[2];  // vrs1: when each eye's camera last got the zoom and shift
bool EngineEyeOffCentreNow(int eye) { return !g_ocBad && g_ocTick[eye] && GetTickCount() - g_ocTick[eye] < 100; }
// vrs1: the eye whose picture the game is drawing now for the headset, or -1 (2D screen, scope, not playing).
int EngineDrawEye() {
    int eye = g_forceEye;
    return (eye == 0 || eye == 1) && EngineActive() && !g_flatNow && !ScopedNow() ? eye : -1;
}
static void OffCentre(BYTE *cam, bool want) {
    const float *kId = kOcId;
    float *m = (float *)(cam + 0x3c0), eyeT[4], hx, hy;
    g_ocCur[0] = 1, g_ocCur[1] = 0, g_ocCur[2] = 1, g_ocCur[3] = 0;
    if (g_ocBad) return;
    bool ours = OcOurs(m);
    if (!ours && memcmp(m, kId, 64)) {
        if (!want && !g_ocWrote) return;  // not ours and nothing asked: leave it
        g_ocBad = true;
        Log("engine: off-centre: the camera's extra projection matrix is in use by the game (%.3f %.3f %.3f %.3f ...); trial off",
            m[0], m[5], m[12], m[13]);
        return;
    }
    int eye = g_forceEye;
    float fov = *(float *)(cam + 0x228), aspect = *(float *)(cam + 0x18);
    float tx = tanf(fov * 0.5f), ty = aspect > 0.1f ? tx / aspect : 0;
    want = want && (eye == 0 || eye == 1) && !ScopedNow() && XrGetHeadsetFov(&hx, &hy) && XrGetEyeTan(eye, eyeT) &&
           fabsf(tx - hx) < 0.002f && fabsf(ty - hy) < 0.002f && eyeT[1] - eyeT[0] > 0.5f && eyeT[2] - eyeT[3] > 0.5f;
    if (!want) {
        if (ours) memcpy(m, kId, 64);
        return;
    }
    float l = eyeT[0], r = eyeT[1], u = eyeT[2], d = eyeT[3];
    g_ocCur[0] = 2 * tx / (r - l), g_ocCur[1] = -(r + l) / (r - l);
    g_ocCur[2] = 2 * ty / (u - d), g_ocCur[3] = -(u + d) / (u - d);
    g_ocWrote = true;
    g_ocTick[eye] = GetTickCount();  // vrs1
    OcPut(m, g_ocCur[0], g_ocCur[1], g_ocCur[2], g_ocCur[3]);
    g_ocThread = GetCurrentThreadId(), g_ocFov = fov, g_ocAspect = aspect;
    static bool s_logged[2];
    if (!s_logged[eye])
        s_logged[eye] = true, Log("engine: off-centre: %s eye zoom %.3f x %.3f, shift %.3f %.3f", eye ? "right" : "left",
                                  g_ocCur[0], g_ocCur[2], g_ocCur[1], g_ocCur[3]);
}

// oc2: a camera the game makes from the main one while that eye is drawn (the water
// reflection's mirrored camera, ...) gets the same zoom and shift; a mirrored camera
// (its right points the other way) gets the sideways shift the other way round.
// oc2's census (11 Nov) confirmed it: per eye the game runs about a dozen copies of the
// main camera through here (they arrive with our matrix already copied), three of them
// mirrored (up z -0.97, right -1.00: from FC3+1157cf02 and FC3+118a6280); the shadow and
// cube cameras (lens 1, 90 degrees) are left alone.
static void OcDerived(BYTE *cam) {
    float *m = (float *)(cam + 0x3c0);
    const float *f = (const float *)(cam + 0x400), *u = (const float *)(cam + 0x40c);
    bool ident = !memcmp(m, kOcId, 64), ours = OcOurs(m);
    bool live = g_ocCur[0] != 1 && GetCurrentThreadId() == g_ocThread;
    float fov = *(float *)(cam + 0x228), aspect = *(float *)(cam + 0x18);
    bool like = live && !cam[8] && *(int *)(cam + 0x20) == 0 && fabsf(fov - g_ocFov) < 1e-4f && fabsf(aspect - g_ocAspect) < 1e-4f;
    float rt[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    float d = rt[0] * g_ocRight[0] + rt[1] * g_ocRight[1] + rt[2] * g_ocRight[2];
    if (g_ocBad || (!ident && !ours)) return;
    if (!like || fabsf(d) < 0.5f) {
        if (ours) memcpy(m, kOcId, 64);  // left over from a frame with the test on
        return;
    }
    OcPut(m, g_ocCur[0], d < 0 ? -g_ocCur[1] : g_ocCur[1], g_ocCur[2], g_ocCur[3]);
}

// oc2: the game works out what is in view (its culling planes) from ONE corner of the
// projection and mirrors it, i.e. it assumes a centred picture (FC3+11502020). With our
// zoom and shift that cut scenery out at the right eye's outer edge. So: run the game's
// function with the zoom and shift, keep the four projection matrices it made, run it
// again centred (culling planes of the full centred view), and put the matrices back.
// bd2: the game's own UpdateMatrices: thiscall (camera in ecx) in Blood Dragon.
typedef int(__attribute__((thiscall)) *UpdateFnBd)(BYTE *cam);
static inline int OrigUpdate(BYTE *cam) { return (int)BdThisCall(g_tramp, cam, 0); }
static int OcUpdate(BYTE *cam) {
    float *m = (float *)(cam + 0x3c0);
    if (g_ocBad || !OcOurs(m)) return OrigUpdate(cam);
    float keep[16];
    BYTE mats[0x100];  // +0xf0 projection, +0x130 its inverse, +0x170 view * projection, +0x1b0 its inverse
    memcpy(keep, m, 64);
    OrigUpdate(cam);
    memcpy(mats, cam + 0xf0, sizeof mats);
    memcpy(m, kOcId, 64);
    int r = OrigUpdate(cam);
    memcpy(cam + 0xf0, mats, sizeof mats);
    memcpy(m, keep, 64);
    return r;
}
// The viewport that makes a centred picture's coordinates land right in the last view's image.
void EngineEyeViewport(float *x, float *y, float *w, float *h) {
    LONG seq = g_ringDone;
    if (!seq) return;
    const float *o = g_ring[seq & 7].oc;
    if (o[0] == 1 && o[1] == 0 && o[2] == 1 && o[3] == 0) return;
    float W = *w, H = *h;
    *x = W * 0.5f * (o[1] + 1 - o[0]), *w = W * o[0];
    *y = H * 0.5f * (1 - o[3] - o[2]), *h = H * o[2];
}

static void ApplyHead(BYTE *cam) {
    ApplyFov(cam);
    OffCentre(cam, g_ocOn && !s_scopeEyeNow);
    LONG idx = g_headIdx;
    if (idx < 0) return;
    Head h = g_heads[idx];
    float *fwd = (float *)(cam + 0x400), *up = (float *)(cam + 0x40c);
    float aimPos[3], aimFwd[3] = {fwd[0], fwd[1], fwd[2]};  // turret1: the game's own camera
    const float aimUp[3] = {up[0], up[1], up[2]};             // turanchor1: its up too (fwd/up are ours further down)
    memcpy(aimPos, cam + 0x1f0, 12);
    Normalize(aimFwd);
    // Camera basis as the engine builds it: right = forward x up.
    float f[3] = {fwd[0], fwd[1], fwd[2]}, u[3] = {up[0], up[1], up[2]}, r[3];
    Normalize(f);
    GliderSteady(f, (float *)(cam + 0x1f0));  // glide1
    // tutpause1 (DrGonzaga 17 Nov, bd96: "When pressing A to acknowledge tutorial messages, the view is still moving"):
    // a tutorial message pauses the game, and while paused the on-foot camera stops, so the view fell back to the
    // game's look, which already has the head's turn in it (the head aims): the head's turn counted twice until A.
    // Now a pause that starts on foot keeps the body direction, so nothing moves in or out of the message.
    static bool s_footPause;
    const bool footNow = g_bodyLive && GetTickCount() - g_aimLast < 200;
    if (footNow) s_footPause = true;
    else if (!g_bodyLive || g_cutscene || !GamePaused()) s_footPause = false;
    if (footNow || s_footPause) {
        // On foot, aiming with the head: the view turns with our body direction, not the game's look.
        f[0] = g_body[0], f[1] = g_body[1], f[2] = 0;
        u[0] = 0, u[1] = 0, u[2] = 1;
    }
    if (g_cutscene && g_cutSteady) CutsceneView(f, u, (const float *)(cam + 0x1f0));  // cut1
    LevelBasis(f, u, r);
    // Eye axes in the world: x = r, y = u, z = -f. Turn the eye by H: new axis = B * H column.
    const float *m = h.m;  // m[row * 4 + col]
    float nf[3], nu[3];
    for (int i = 0; i < 3; ++i) {
        nf[i] = -(r[i] * m[0 * 4 + 2] + u[i] * m[1 * 4 + 2] - f[i] * m[2 * 4 + 2]);
        nu[i] = r[i] * m[0 * 4 + 1] + u[i] * m[1 * 4 + 1] - f[i] * m[2 * 4 + 1];
    }
    float hf[3], hu[3];  // sound1: the head's view for the sound listener (before the scope override)
    memcpy(hf, nf, 12), memcpy(hu, nu, 12);
    if (g_vehicle && g_vehDriving && !EngineTurret() && !g_cutscene && hf[0] * hf[0] + hf[1] * hf[1] > 1e-4f)
        g_vehViewYaw = atan2f(hf[1], hf[0]), g_vehViewT = GetTickCount() | 1;  // vehface1
    if (s_scopeEyeNow) {
        // scope1: the scope view looks along the gun (the beam), smoothed while scoped.
        static float s_dir[3];
        static LARGE_INTEGER s_last;
        LARGE_INTEGER now, fq;
        QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
        float hand[3], dir[3], raw[3];
        static float s_prev[3], s_speed, s_rec, s_recV;  // smooth2: the gun's turn speed (deg/s), itself smoothed
        static int s_frames, s_lost;
        static DWORD s_logT;
        ++s_frames;
        s_scopeHandOk = false;
        if (LaserRay(hand, dir, raw)) {
            memcpy(s_scopeHand, hand, 12), s_scopeHandOk = true;
            float dt = s_last.QuadPart ? float(now.QuadPart - s_last.QuadPart) / fq.QuadPart : 1.0f;
            // smooth2 (DrGonzaga 2 Nov: "stuck", wanted an average): adaptive. When the gun
            // is nearly still (shake) the full steadiness applies; the faster it turns,
            // the less smoothing, so a deliberate move follows straight away (no lag/drift).
            if (dt > 0.3f) s_speed = 0;
            else if (dt > 1e-4f) {
                float d = dir[0] * s_prev[0] + dir[1] * s_prev[1] + dir[2] * s_prev[2];
                float sp = acosf(d > 1 ? 1 : d < -1 ? -1 : d) * 57.2958f / dt;
                s_speed += (sp - s_speed) * (1.0f - expf(-dt / 0.05f));
            }
            memcpy(s_prev, dir, 12);
            float st = g_scopeSmooth * (g_crouched ? g_crouchSteady : 1.0f);
            if (!InputAimHeld()) st = 0;  // autoscope1: the left trigger steadies; without it the scope follows the gun
            float tau = g_classicSmooth ? st * 0.25f : st * 0.25f / (1.0f + s_speed / 8.0f);
            float a = dt > 0.3f || tau < 1e-3f ? 1.0f : 1.0f - expf(-dt / tau);
            for (int i = 0; i < 3; ++i) s_dir[i] += (dir[i] - s_dir[i]) * a;
            Normalize(s_dir);
            memcpy(nf, s_dir, 12);
            const float wz[3] = {0, 0, 1};
            float rr[3];
            Cross(nf, wz, rr);
            if (rr[0] * rr[0] + rr[1] * rr[1] + rr[2] * rr[2] > 1e-6f) {
                Normalize(rr);
                Cross(rr, nf, nu);
            }
            // recoil1 (DrGonzaga 2 Nov): each shot kicks the view up, then it settles
            // back (critically damped spring: peak ~40 ms, back in ~0.2 s).
            {
                const float w = 25.0f;
                LONG k = InterlockedExchange(&g_kicks, 0);
                if (dt > 0.3f) s_rec = s_recV = 0;
                if (k) s_recV += *ScopeRecoil() * 0.0174533f * w * 2.71828f;
                float h = dt > 0.05f ? 0.05f : dt;
                s_recV += (-w * w * s_rec - 2 * w * s_recV) * h, s_rec += s_recV * h;
                if (fabsf(s_rec) > 1e-5f) {
                    // recoil2: kick towards the reticle's up (the gun's up, with the
                    // per-gun reticle turn), so a canted gun kicks along its cant.
                    float d[3];
                    memcpy(d, nu, 12);
                    float gm[16];
                    if (GunAttachPose(gm)) {
                        const float *gu = gm + 8;
                        float k = gu[0] * nf[0] + gu[1] * nf[1] + gu[2] * nf[2], uu[3], l = 0;
                        for (int i = 0; i < 3; ++i) uu[i] = gu[i] - nf[i] * k, l += uu[i] * uu[i];
                        if (l > 1e-4f) {
                            l = sqrtf(l);
                            float rt[3], a = *ScopeReticleTurn() * 0.0174533f, ca = cosf(a), sa = sinf(a);
                            for (int i = 0; i < 3; ++i) uu[i] /= l;
                            Cross(nf, uu, rt);  // right = forward x up
                            for (int i = 0; i < 3; ++i) d[i] = uu[i] * ca + rt[i] * sa;  // turned clockwise
                        }
                    }
                    // Turn the view by s_rec in the plane of nf and d (nu turns with it).
                    float c = cosf(s_rec), sn = sinf(s_rec), ax[3], f2[3], u2[3];
                    Cross(nf, d, ax);
                    Normalize(ax);
                    auto rot = [&](const float *v, float *o) {
                        float kv[3], kd = ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2];
                        Cross(ax, v, kv);
                        for (int i = 0; i < 3; ++i) o[i] = v[i] * c + kv[i] * sn + ax[i] * kd * (1 - c);
                    };
                    rot(nf, f2), rot(nu, u2);
                    memcpy(nf, f2, 12), memcpy(nu, u2, 12);
                }
            }
        } else ++s_lost;
        if (GetTickCount() - s_logT > 10000) {  // how often the controller ray was missing while scoped
            if (s_lost) Log("engine: scope: controller ray missing in %d of %d scoped frames", s_lost, s_frames);
            s_logT = GetTickCount(), s_frames = s_lost = 0;
        }
        s_last = now;
    }
    memcpy(fwd, nf, 12);
    memcpy(up, nu, 12);

    // Head position (6DoF), on the level game axes: world up is +z.
    float *pos = (float *)(cam + 0x1f0);
    g_rawCamZ = pos[2], g_rawCamTick = GetTickCount();  // move2
    g_headUp = h.off[1], g_headUpTick = GetTickCount();  // realcrouch1: real metres
    if (g_viewComp && g_6dof) {
        // realcrouch1: take the game's crouch drop back out; the 6DoF offset below adds
        // the real head's drop, so the view sits at the real eye height.
        BYTE *pl = GunAttachPlayer();
        if (pl && !IsBadReadPtr(pl + 0x50, 12)) {
            float up = pos[2] - *(float *)(pl + 0x58), comp = g_standUp - up;
            if (comp > 0 && comp < 1.0f) pos[2] += comp, g_crouchLift = comp, g_crouchLiftTick = GetTickCount();
        }
    }
    // turret1 (DrGonzaga 9 Nov: on the boat turret the camera skipped metres and its height
    // was unreliable): the game's mounted gun camera swings with the gun. Steady view: at
    // the player's own place, at the eye height measured when getting on (VR page).
    // turanchor1 (DrGonzaga 16 Nov: on the opening helicopter's turret "the turret moves away as the helicopter moves,
    // probably by a few meters"): the player's own place (pl+0x50) does not keep up with a fast vehicle, so the view
    // fell behind the gun. The game's own turret camera is always where the picture is (it is what the flat game
    // shows) but it swings round the gun as you aim. So now: the gun's pivot is worked out from the game's camera
    // each frame (camera minus its offset from the gun, turned with the camera), and the view stays where you
    // were when you got on (level with the game's camera then, at the measured eye height), measured from that pivot. [vr] TurretAnchor: 1 this (default), 0 the player's place.
    {
        static bool s_on, s_have;
        static DWORD s_t0;
        static float s_L[3], s_E[3];
        const bool tur = g_turretSteady && EngineTurret();
        BYTE *pl = GunAttachPlayer();
        const bool plOk = pl && !IsBadReadPtr(pl + 0x50, 12);
        float gf[3] = {aimFwd[0], aimFwd[1], aimFwd[2]}, gu[3] = {aimUp[0], aimUp[1], aimUp[2]}, gr[3];
        Normalize(gf);
        Cross(gf, gu, gr), Normalize(gr), Cross(gr, gf, gu);  // the game camera's own axes: right, up, forward
        if (tur != s_on) s_on = tur, s_have = false, s_t0 = GetTickCount(), g_turScripted = false, g_turHeli = false, g_turVehSeen = false,
                         g_turDragon = false, g_turDragonFrame = false;
        if (tur && g_cutFlagNow && !g_turScripted)  // turscript1
            g_turScripted = true, Log("turret: a scripted ride (the game's cinematic flag is on): looking for the vehicle under it");
        if (tur && g_turretDragon && g_turScripted && !g_turHeli && !g_turVehSeen && !g_turDragon && GetTickCount() - s_t0 > 1500) {  // turdragon1
            g_turDragon = true;
            Log("turret: a scripted ride with no vehicle under the gun (the armoured dragon): the helicopter's view rules apply (held above the gun, no shake)");
        }
        if (tur && g_turDragon && !g_turDragonFrame) {  // turdragon1: does the gun's own object keep its heading while you aim?
            static float s_rel0, s_lo, s_hi;
            static bool s_relOk;
            static BYTE *s_relG;
            BYTE *G = TurretGunEnt();
            if (G != s_relG) s_relG = G, s_relOk = false;
            if (G && !IsBadReadPtr(G + 0x20, 64) && gf[0] * gf[0] + gf[1] * gf[1] > 0.04f) {
                const float *M = (const float *)(G + 0x20);
                if (fabsf(M[4]) + fabsf(M[5]) > 0.05f) {
                    float ey = atan2f(M[5], M[4]), rel = Wrap(atan2f(gf[1], gf[0]) - ey);
                    if (!s_relOk) s_rel0 = rel, s_lo = s_hi = 0, s_relOk = true;
                    float d = Wrap(rel - s_rel0);
                    if (d < s_lo) s_lo = d;
                    if (d > s_hi) s_hi = d;
                    if (s_hi - s_lo > 0.35f) {
                        g_turDragonFrame = true;
                        Log("turret: the dragon's gun object keeps its own heading while you aim (aim swung %.0f deg against it): the view now turns with it",
                            (s_hi - s_lo) * 57.2958f);
                    }
                }
            }
        }
        float gp[3];
        if (tur && !s_have && g_turretEye > 0 && plOk && GetTickCount() - s_t0 >= 600 && TurretGunPos(gp)) {
            const float *pp = (const float *)(pl + 0x50);
            float d[3] = {aimPos[0] - gp[0], aimPos[1] - gp[1], aimPos[2] - gp[2]};
            s_L[0] = d[0] * gr[0] + d[1] * gr[1] + d[2] * gr[2];
            s_L[1] = d[0] * gu[0] + d[1] * gu[1] + d[2] * gu[2];
            s_L[2] = d[0] * gf[0] + d[1] * gf[1] + d[2] * gf[2];
            // where the view stays, from the pivot: level, where the game's camera is now (not the player's own place,
            // which already lags if the vehicle is moving); height as measured when getting on
            s_E[0] = aimPos[0] - gp[0], s_E[1] = aimPos[1] - gp[1], s_E[2] = pp[2] + g_turretEye - gp[2];
            s_have = true;
            Log("turret: anchored to the gun: game camera %.2f right %.2f up %.2f ahead of the gun, you stand %.2f %.2f %.2f m from it (%s)",
                s_L[0], s_L[1], s_L[2], s_E[0], s_E[1], s_E[2], g_turretAnchor ? "the view moves with the gun's mount" : "not used: TurretAnchor=0");
        }
        if (tur && g_turretEye > 0 && plOk) {
            const float *pp = (const float *)(pl + 0x50);
            float old[3] = {pp[0], pp[1], pp[2] + g_turretEye + g_turretHeight}, nw[3];
            bool use = false;
            if (s_have) {
                for (int i = 0; i < 3; ++i) nw[i] = aimPos[i] - (gr[i] * s_L[0] + gu[i] * s_L[1] + gf[i] * s_L[2]) + s_E[i];
                nw[2] += g_turretHeight;
                // turabove1 (DrGonzaga 17 Nov, opening helicopter: the minigun's camera shake shakes the gun and the
                // helicopter, and you shift as it banks; "camera fixed above the minigun by a set distance, no camera
                // shake"): the view is held a set height above the gun itself (its own place, which has no shake), and
                // where it sits round the gun only follows the game's camera slowly (2 s), so the shake is left out.
                static float s_off[2], s_rideL[3];
                static bool s_offOk, s_rideOk;
                static DWORD s_offT;
                float gq0[3];
                if (!s_have || !g_turretAbove || !TurSpecial()) s_offOk = false, s_rideOk = false;
                else if (TurretGunPos(gq0)) {
                    DWORD t = GetTickCount();
                    float tx = nw[0] - gq0[0], ty = nw[1] - gq0[1];
                    if (!s_offOk || t - s_offT > 500) s_off[0] = tx, s_off[1] = ty, s_offOk = true;
                    else {
                        float a = 1 - expf(-(float)(t - s_offT) * 0.001f / 2.0f);  // turabove2: was an unsigned minus (bd93 NaN)
                        s_off[0] += (tx - s_off[0]) * a, s_off[1] += (ty - s_off[1]) * a;
                    }
                    s_offT = t;
                    nw[0] = gq0[0] + s_off[0], nw[1] = gq0[1] + s_off[1], nw[2] = gq0[2] + s_E[2] + g_turretHeight + g_turretLift;  // turabove2: higher
                    // turride1 (DrGonzaga 17 Nov: "Can the player turn/move with the helicopter? How does the 2D game keep
                    // you in place"): the flat game's camera rides the gun's seat, so it dips, banks and turns with the
                    // helicopter. The view is now fixed in the helicopter's own frame (the game's vehicle object), at
                    // the place worked out above when you got on; AimMatrix turns the body with its heading.
                    BYTE *V = g_rideEnt;
                    if (!V) s_rideOk = false;
                    else {
                        float Mf[16];
                        const float *M = (const float *)(V + 0x20);
                        if (g_turDragon) {  // turdragon1: the gun object's heading only, level (no dip, roll or aim tilt)
                            float hy = atan2f(M[5], M[4]), c = cosf(hy), sn = sinf(hy);
                            const float Y[16] = {sn, -c, 0, 0, c, sn, 0, 0, 0, 0, 1, 0, M[12], M[13], M[14], 1};
                            memcpy(Mf, Y, sizeof Mf), M = Mf;
                        }
                        if (!s_rideOk) {
                            float d[3] = {nw[0] - M[12], nw[1] - M[13], nw[2] - M[14]};
                            for (int j = 0; j < 3; ++j) s_rideL[j] = d[0] * M[4 * j] + d[1] * M[4 * j + 1] + d[2] * M[4 * j + 2];
                            s_rideOk = true;
                            Log("turret: riding the vehicle: view fixed %.2f %.2f %.2f m in its frame", s_rideL[0], s_rideL[1], s_rideL[2]);
                        } else
                            for (int i = 0; i < 3; ++i) nw[i] = M[12 + i] + M[i] * s_rideL[0] + M[4 + i] * s_rideL[1] + M[8 + i] * s_rideL[2];
                    }
                    if (!(fabsf(nw[0]) < 1e6f && fabsf(nw[1]) < 1e6f && fabsf(nw[2]) < 1e6f)) s_offOk = false, memcpy(nw, pos, 12);
                }
                float dx = nw[0] - old[0], dy = nw[1] - old[1], dz = nw[2] - old[2], dd = sqrtf(dx * dx + dy * dy + dz * dz);
                use = g_turretAnchor && TurSpecial() && dd < 15.0f;  // turdragon1: the dragon too  // further than that is a load or a jump, not lag (turheli1: the opening helicopter only)
                // tidy2 (DrGonzaga 22 Nov): the 2 s "turret: moving" probe line removed (turret and dragon views done)
            }
            memcpy(pos, use ? nw : old, 12);
        }
    }
    // move2 (DrGonzaga 8 Nov: gun and map dropped to the floor when crouching for real):
    // base (where the gun, beam and wrist panels are placed from) now includes the lift.
    float base[3] = {pos[0], pos[1], pos[2]};
    const float wu[3] = {0, 0, 1};
    float fr[3], ff[3];
    Cross(f, wu, fr);  // right = forward x up
    if (fr[0] * fr[0] + fr[1] * fr[1] < 1e-4f) memcpy(fr, r, 12);  // looking straight up/down
    Normalize(fr);
    Cross(wu, fr, ff);
    if (g_6dof) RoomScale(h, base, fr, ff);
    if (g_6dof && !(g_vehicle && g_car[2])) {  // car6: 3DoF in vehicles (DrGonzaga 31 Oct)
        float o[3];
        for (int i = 0; i < 3; ++i) {
            o[i] = h.off[i] < -1 ? -1 : h.off[i] > 1 ? 1 : h.off[i];  // 1 m at most
            o[i] /= g_worldScale;
        }
        for (int i = 0; i < 3; ++i) pos[i] += fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2];
    }

    PublishListener(hf, hu, pos, base);

    // Alternate-eye stereo: move the camera half the IPD left or right (game units are metres).
    int eye = -1;
    if (g_forceEye >= 0) eye = g_forceEye;
    else if (g_stereo) eye = g_eyeCount++ & 1;
    if (eye >= 0) {
        float nr[3];
        Cross(nf, nu, nr);
        Normalize(nr);
        float s = (eye ? 0.5f : -0.5f) * XrGetIpd() / g_worldScale;
        for (int i = 0; i < 3; ++i) pos[i] += nr[i] * s;
    }

    if (s_scopeEyeNow && s_scopeHandOk && g_scopeAhead > 0.01f)  // lasers2: the scope view starts past the gun
        for (int i = 0; i < 3; ++i) pos[i] = s_scopeHand[i] + nf[i] * g_scopeAhead / g_worldScale;
    if (s_scopeEyeNow) memcpy(g_scopeRay, pos, 12), memcpy(g_scopeRay + 3, nf, 12), g_scopeRayTick = GetTickCount();
    LONG seq = InterlockedIncrement(&g_ringPos);
    LONG slot = seq & 7;
    g_ring[slot].seq = seq;
    g_ring[slot].eye = eye;
    memcpy(g_ring[slot].pos, pos, 12);
    memcpy(g_ring[slot].fwd, nf, 12);
    memcpy(g_ring[slot].up, nu, 12);
    memcpy(g_ring[slot].base, base, 12);
    memcpy(g_ring[slot].basis, r, 12), memcpy(g_ring[slot].basis + 3, u, 12), memcpy(g_ring[slot].basis + 6, f, 12);
    memcpy(g_ring[slot].posAxes, fr, 12), memcpy(g_ring[slot].posAxes + 3, wu, 12), memcpy(g_ring[slot].posAxes + 6, ff, 12);
    memcpy(g_ring[slot].aimPos, aimPos, 12), memcpy(g_ring[slot].aimFwd, aimFwd, 12);
    memcpy(g_ring[slot].q, h.q, 16);
    memcpy(g_ring[slot].p, h.p, 12);
    memcpy(g_ring[slot].oc, g_ocCur, 16);
    g_ringDone = seq;
    InterlockedIncrement(&g_applied);
    g_lastApplied = GetTickCount();
}

static volatile bool g_enabled = true;

// flat1: the cutscene's own camera, as the flat game draws it: no head turn, no
// headset field of view, no eye offset; the picture's shape is the eye image's.
// cyberdirect1 (DrGonzaga 15 Nov: the cyber-eye "starts as though you're looking at the sky, then it pulls down ...
// It also lags a bit, even though smoothing is zero"): the game's look is steered to the head with the stick, which
// always trails. The picture's own camera is now turned the rest of the way (what the look is still short of the
// head, AimMatrix above), so the picture is where the head points from the first frame; the game's look catches up
// underneath (it is what the game tags and zooms with).
static void FlatCamera(BYTE *cam) {
    s_scopeEyeNow = false;
    g_flatTick = GetTickCount();
    if (CyberEyeOn() && g_cyErrT && g_flatTick - g_cyErrT < 200) {
        float *fwd = (float *)(cam + 0x400), *up = (float *)(cam + 0x40c);
        float f[3] = {fwd[0], fwd[1], fwd[2]};
        Normalize(f);
        float y = atan2f(f[1], f[0]) + g_cyErr[0], p = asinf(f[2] > 1 ? 1 : f[2] < -1 ? -1 : f[2]) + g_cyErr[1];
        p = p > 1.5f ? 1.5f : p < -1.5f ? -1.5f : p;
        float nf[3] = {cosf(p) * cosf(y), cosf(p) * sinf(y), sinf(p)}, z[3] = {0, 0, 1}, r[3], nu[3];
        Cross(nf, z, r), Normalize(r), Cross(r, nf, nu);
        if (isfinite(nf[0] + nf[1] + nf[2] + nu[0] + nu[1] + nu[2])) memcpy(fwd, nf, 12), memcpy(up, nu, 12);
    }
    if (CyberEyeOn()) {
        // cybereye1: the game's own field of view is logged as it changes, to learn what the binoculars ask for.
        static float s_fov;
        static DWORD s_t;
        float fv = *(float *)(cam + 0x228);
        if (fabsf(fv - s_fov) > 0.03f && g_flatTick - s_t > 400) s_fov = fv, s_t = g_flatTick, Log("cybereye: the game's view is %.1f deg wide", fv * 57.29578f);
    }
    float *fov = (float *)(cam + 0x228), *aspect = (float *)(cam + 0x18);
    UINT w = 0, h = 0;
    if (XrImageSize(&w, &h) && h && *fov > 0.02f && *fov < 3.0f && *aspect > 0.3f && *aspect < 3.0f) {
        float a = (float)w / (float)h;
        // movie1: a 16:9 movie screen. xr.cpp shows the middle 16:9 band of the picture.
        // The game fits its field of view to the picture's shape (taller picture = less
        // to the sides), so the view is widened until that band holds what a 16:9
        // monitor would show; "zoom" trims it by eye.
        if (g_cutWide && a < 16.0f / 9.0f) {
            float z = g_cutZoom < 0.4f ? 0.4f : g_cutZoom > 3.0f ? 3.0f : g_cutZoom;
            float tx = tanf(*fov * 0.5f) * (16.0f / 9.0f) / a / z;
            if (tx > 0.01f && tx < 5.0f) *fov = 2.0f * atanf(tx);
        }
        *aspect = a;
    }
    LodFix(1, 1);
}

// occl1: how the main camera's projection (cam+0xf0, just made) turns distance ahead
// into a depth-buffer value: depth = a + b / distance. Row vectors (clip = v * P): the
// depth column is m[2], m[6], m[10], m[14] and w is m[11] * z; the other layout is
// handled too. Logged once (and again if it changes) so a wrong guess shows in the log.
static float g_depthA, g_depthB;
static bool g_depthOk;
static void DepthTerms(const BYTE *cam) {
    float m[16];
    memcpy(m, cam + 0xf0, 64);
    float a, b;
    bool ok = true;
    if (fabsf(fabsf(m[11]) - 1) < 1e-3f && fabsf(m[15]) < 1e-3f) a = m[10] / m[11], b = m[14];
    else if (fabsf(fabsf(m[14]) - 1) < 1e-3f && fabsf(m[15]) < 1e-3f) a = m[10] / m[14], b = m[11];
    else a = b = 0, ok = false;
    ok = ok && a == a && b == b && fabsf(a) < 4 && fabsf(b) < 100 && b != 0;
    if (ok != g_depthOk || fabsf(a - g_depthA) > 1e-4f || fabsf(b - g_depthB) > 1e-4f) {
        static int s_logs;
        if (s_logs < 12)
            ++s_logs, Log("engine: depth terms %s: depth = %.6f + %.6f / distance; projection rows [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]",
                          ok ? "read" : "NOT understood (beam stays on top)", a, b, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10],
                          m[11], m[12], m[13], m[14], m[15]);
    }
    g_depthA = a, g_depthB = b, g_depthOk = ok;
}
DWORD EngineInputBase() { return g_seqBase; }  // emptyhands2
DWORD EngineInputMode() { return g_seqHash; }  // zip1: the game's input mode for the player (crc32 of its name)
bool EngineDepthTerms(float *a, float *b) { *a = g_depthA, *b = g_depthB; return g_depthOk; }

// bd2: the body shared by both games. fromBuild: called by the function that builds a
// frame's render camera; mainCall: that build is the main render setup's own.
static int UpdateMatricesCore(BYTE *cam, DWORD src, bool fromBuild, bool mainCall) {
    if (g_verboseLog) InterlockedIncrement(&g_calls);  // review1: counted only for the detailed log
    bool mainCam = false;
    if (fromBuild) CutsceneTick(src);  // cut1, cut3
    if (fromBuild && g_enabled) {
        if (mainCall) {
            mainCam = true;
            // flat1: 2D screen on or off is decided once per frame (the left eye's
            // build, or the only one), so both eyes of a frame agree.
            if (g_forceEye != 1 && g_forceEye != 2) {  // pip1: nor the scope picture (eye 2)
                const bool cyb = CyberEyeOn() && !g_cutscene;  // cybereye1: the binoculars on the flat screen
                bool fl = (g_cutscene && g_cutFlat && g_cutSteady && !g_cutInter) || cyb;  // seq1: not while it asks for input
                if (fl != g_flatNow) {
                    if (fl) g_flatTick = GetTickCount();
                    g_flatNow = fl;
                    Log("%s: 2D screen %s", cyb ? "cybereye" : "cutscene", fl ? "on (the game's own camera, one picture a frame)" : "off: back to VR");
                    // seq1: for a 16:9 screen shape later: what the game's camera asks for here
                    if (fl) Log("cutscene: the game's camera here: field of view %.1f deg, shape %.3f", *(float *)(cam + 0x228) * 57.29578f, *(float *)(cam + 0x18));
                }
            }
            if (g_flatNow)
                FlatCamera(cam), OffCentre(cam, false);
            else if (cam[8])
                InterlockedIncrement(&g_flagged), OffCentre(cam, false);  // camera built from a matrix; not handled yet
            else
                ApplyHead(cam);
            // cb1 (review F1): the field of view this picture is drawn with, straight from
            // the camera. Not the scope eye's zoom (scope2).
            if (!cam[8] && !s_scopeEyeNow) {
                float fov = *(float *)(cam + 0x228), aspect = *(float *)(cam + 0x18);
                if (fov > 0.02f && fov < 3.0f && aspect > 0.3f && aspect < 3.0f) {
                    float tx = tanf(fov * 0.5f), ty = tx / aspect, t[4];
                    int eye = g_forceEye;
                    XrSetGameFov(tx, ty);
                    if ((eye == 0 || eye == 1) && g_ocCur[0] != 1 && XrGetEyeTan(eye, t)) XrSetImageFov(eye, t[0], t[1], t[2], t[3]);  // oc1
                    else XrSetImageFov(eye, -tx, tx, ty, -ty);
                }
            }
            if (g_ocCur[0] != 1) {  // oc2: the main camera's right, after the head turn
                const float *f = (const float *)(cam + 0x400), *u = (const float *)(cam + 0x40c);
                g_ocRight[0] = f[1] * u[2] - f[2] * u[1], g_ocRight[1] = f[2] * u[0] - f[0] * u[2], g_ocRight[2] = f[0] * u[1] - f[1] * u[0];
            }
        }
    }
    if (!mainCam && g_ocWrote) OcDerived(cam);
    if (!mainCam || !*LaserBehindObjects() || g_flatNow) return OcUpdate(cam);
    int r = OcUpdate(cam);
    DepthTerms(cam);  // occl2
    return r;
}

// ---- bd2: Blood Dragon (FC3_d3d11.dll built 17 Oct 2019, 28,693,672 bytes) ----
// Same engine and the same camera layout, compiled differently: other addresses, and
// thiscall where the Far Cry 3 build has stdcall. Found 12 Nov by matching both DLLs.
//   0x110b1538  UpdateMatrices, thiscall (camera in ecx), plain ret. 34 callers, as in FC3.
//   0x100ee4b2  builds a render camera: cdecl (source, camera, size, aspect, matrix); the
//               source has position +4, forward +0x64, up +0x70 like FC3's edi. It ENDS
//               with "jmp UpdateMatrices", so UpdateMatrices returns to ITS caller.
//               Six callers; the main render setup's is the small wrapper 0x100d469b
//               (one caller: the frame render 0x100db864, at 0x100dbc39), whose call
//               returns to 0x100d4713.
static const DWORD kBdUpdateMatrices = 0x110b1538, kBdBuildCamera = 0x100ee4b2, kBdMainRet = 0x100d4713;
static const DWORD kBdBuildCalls[6] = {0x100d470e, 0x102aaa8d, 0x108c39f1, 0x10a5bbb1, 0x10a5c022, 0x10cacde3};
static const BYTE kBdPrologue[12] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
static const BYTE kBdBuildPrologue[7] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08};
static DWORD g_bdBuildRet[6], g_bdMainRet;
extern "C" {
void *g_bdBuildTramp;
void BdBuildStub();
}
// At the build function's entry: note its source camera (first argument), then carry on.
// eax is free here (cdecl entry; the function clears it before use).
asm(".globl _BdBuildStub\n"
    "_BdBuildStub:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %eax, _g_srcCam\n"
    "    jmp *_g_bdBuildTramp\n");

static int __attribute__((thiscall)) HookUpdateMatricesBd(BYTE *cam) {
    DWORD src = g_srcCam;
    DWORD ret = (DWORD)__builtin_return_address(0);
    bool fromBuild = false;
    for (int i = 0; i < 6; ++i) fromBuild = fromBuild || ret == g_bdBuildRet[i];
    return UpdateMatricesCore(cam, src, fromBuild, ret == g_bdMainRet);
}

// bd3: the on-foot look. In Far Cry 3 the on-foot camera (0x11416bc0) sets its look inline
// and we hook its Euler-to-matrix call. In Blood Dragon that inline piece is a function of
// its own, 0x110bf832: thiscall (ecx = game camera + 4, one argument: the angles), which
// copies the angles to +0x88, builds the matrix and stores right at +0x7c, forward at +0x64
// and up at +0x70 (FC3: +0x80, +0x68, +0x74 from the camera itself, the same fields).
// It has 36 callers; the on-foot camera 0x10437ddb calls it at 0x104384a7, and only that
// call is redirected.
static const DWORD kBdAimCall = 0x104384a7, kBdSetLook = 0x110bf832;
typedef void(__attribute__((thiscall)) *BdSetLookFn)(BYTE *sub, float *angles);
static BdSetLookFn g_bdSetLook;
extern "C" void __cdecl AimMatrix(float *mx, BYTE *cam, BYTE *owner);
static void __attribute__((thiscall)) BdAimHook(BYTE *sub, float *angles) {
    g_bdSetLook(sub, angles);
    float mx[16] = {0};
    memcpy(mx, sub + 0x7c, 12), memcpy(mx + 4, sub + 0x64, 12), memcpy(mx + 8, sub + 0x70, 12);
    AimMatrix(mx, sub - 4, nullptr);
}

static bool EngineInstallBd() {
    BYTE *target = g_mod + (kBdUpdateMatrices - kImageBase), *build = g_mod + (kBdBuildCamera - kImageBase);
    if (memcmp(target, kBdPrologue, sizeof kBdPrologue) || memcmp(build, kBdBuildPrologue, sizeof kBdBuildPrologue)) {
        Log("engine: Blood Dragon camera code doesn't match this mod's game version; engine hook disabled");
        return false;
    }
    // Every expected caller must be a call to the build function.
    for (int i = 0; i < 6; ++i) {
        BYTE *c = g_mod + (kBdBuildCalls[i] - kImageBase);
        if (c[0] != 0xe8 || c + 5 + *(int *)(c + 1) != build) {
            Log("engine: Blood Dragon camera build call %d not found; engine hook disabled", i);
            return false;
        }
        g_bdBuildRet[i] = (DWORD)(c + 5);
    }
    g_bdMainRet = (DWORD)g_mod + (kBdMainRet - kImageBase);
    BYTE *tail = g_mod + (0x100ee5b8 - kImageBase);
    if (g_bdMainRet != g_bdBuildRet[0] || tail[0] != 0xe9 || tail + 5 + *(int *)(tail + 1) != target) {
        Log("engine: Blood Dragon camera build doesn't end as expected; engine hook disabled");
        return false;
    }
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    // UpdateMatrices: the first 5 bytes (push ebx; mov ebx, esp; push ecx; push ecx), then back.
    memcpy(t, target, 5);
    t[5] = 0xe9;
    *(DWORD *)(t + 6) = (DWORD)(target + 5) - (DWORD)(t + 10);
    // The build function: its first 7 bytes (push ebp; mov ebp, esp; push esi; mov esi, [ebp+8]).
    BYTE *bt = t + 32;
    memcpy(bt, build, 7);
    bt[7] = 0xe9;
    *(DWORD *)(bt + 8) = (DWORD)(build + 7) - (DWORD)(bt + 12);
    g_bdBuildTramp = bt;
    g_tramp = t;
    BYTE patch[8];
    memcpy(patch, build, 8);
    patch[0] = 0xe9;
    *(DWORD *)(patch + 1) = (DWORD)(void *)BdBuildStub - (DWORD)(build + 5);
    patch[5] = 0x90, patch[6] = 0x90;
    if (!PatchBytes(build, patch, 8)) {
        g_tramp = nullptr;
        return false;
    }
    memcpy(patch, target, 8);
    patch[0] = 0xe9;
    *(DWORD *)(patch + 1) = (DWORD)(void *)HookUpdateMatricesBd - (DWORD)(target + 5);
    if (!PatchBytes(target, patch, 8)) {
        g_tramp = nullptr;
        return false;
    }
    Log("engine: Blood Dragon camera hook installed at %p (BD+%08lx), camera build at BD+%08lx", target, kBdUpdateMatrices, kBdBuildCamera);
    LoadWorldScale();
    if (BdStage() >= 3) {
        BYTE *ac = g_mod + (kBdAimCall - kImageBase), *sl = g_mod + (kBdSetLook - kImageBase);
        static const BYTE kSetLook[12] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
        if (ac[0] == 0xe8 && ac + 5 + *(int *)(ac + 1) == sl && !memcmp(sl, kSetLook, sizeof kSetLook) &&
            (g_bdSetLook = (BdSetLookFn)sl, PatchCallTo(ac, (void *)BdAimHook)))
            Log("engine: on-foot look hooked at BD+%08lx", kBdAimCall);
        else
            Log("engine: Blood Dragon on-foot look call doesn't match; look steering off");
    }
    if (BdStage() >= 4 && g_bdSetLook) BoneHookInstallBd(g_mod);  // bd4: motion aiming, shots from the beam, spread, no cover peek
    if (BdStage() >= 5 && g_bdSetLook) GunAttachInstallBd(g_mod);  // bd5: gun in the hand
    if (BdStage() >= 7) LodParamsInstallBd();  // bd68 kill1
    if (BdStage() >= 7) TurretInstallBd(g_mod);  // bd13
    if (BdStage() >= 7) LootInstallBd(g_mod);  // bd19
    if (BdStage() >= 7) MarkersInstallBd(g_mod);  // bd20
    if (BdStage() >= 7) MovieInstallBd(g_mod);    // movie1
    if (BdStage() >= 7) OutpostInstallBd(g_mod);  // outpost2
    if (BdStage() >= 7) DamageInstallBd(g_mod);   // dmgbd1
    if (BdStage() >= 7) ListenerInstallBd(g_mod);  // soundbd1
    if (BdStage() >= 7) BowInstallBd(g_mod), BowStringInstallBd(g_mod);  // bd22
    if (BdStage() >= 7) HandsInstallBd();  // hands1
    if (BdStage() >= 7) XrQuitHookInstallBd(g_mod);  // bd23
    if (BdStage() >= 7) PromptsInstallBd(g_mod);  // bd33 prompts3
    if (BdStage() >= 7) CutsceneInstallBd();  // bd7: cutscenes and scenes with button prompts
    return true;
}

bool EngineInstall() {
    if (g_tramp) return true;
    if (g_bloodDragon && !BdEngineOn()) {
        Log("engine: Blood Dragon: engine hooks switched off in fc3vr.ini ([bd] Engine=0): flat screen only");
        return false;
    }
    g_mod = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    if (!g_mod) {
        Log("engine: FC3_d3d11.dll not loaded");
        return false;
    }
    if (!g_bloodDragon) {
        Log("engine: this build is for Blood Dragon only; engine hooks off");
        return false;
    }
    return EngineInstallBd();
}

void EngineOnPresent() {
    // Numpad 1: turn the player's look bone for all but the camera (bonehook.cpp), to
    // see if the gun aims from it. Aiming with the head by mouse is off (headaim2: no mouse reads).
    static DWORD lastAimLog;
    if (GetTickCount() - lastAimLog > 10000) {
        lastAimLog = GetTickCount();
        if (g_verboseLog) BoneHookLog();
        auto &st = g_steer;
        if (g_lookMode)
            LogV("lookaim: mode %d phase %d, %d updates; game look off target by %.1f deg avg, %.1f max (last 10 s)",
                g_lookMode, st.phase, st.errN, st.errN ? st.errSum / st.errN * 57.3f : 0.f, st.errMax * 57.3f);
        st.errSum = st.errMax = 0, st.errN = 0;
    }
    if (KeyHit(VK_F7)) {
        g_enabled = !g_enabled;
        Log("engine: camera hook %s (F7)", g_enabled ? "ON" : "OFF");
    }
    if (KeyHit(VK_F6)) {
        g_vrFov = !g_vrFov;
        Log("engine: headset field of view %s (F6)", g_vrFov ? "ON" : "OFF");
    }
    if (KeyHit(VK_F5)) {
        g_stereo = !g_stereo;
        Log("engine: stereo %s (F5)", g_stereo ? "ON" : "OFF");
    }
    if (KeyHit(VK_F4)) {
        g_6dof = !g_6dof;
        Log("engine: head position (6DoF) %s (F4)", g_6dof ? "ON" : "OFF");
    }
    if (KeyHit(VK_ADD) && g_worldScale < 3) g_worldScale *= 1.05f, SaveWorldScale();
    if (KeyHit(VK_SUBTRACT) && g_worldScale > 0.3f) g_worldScale /= 1.05f, SaveWorldScale();

    // Submit the frame with the pose its camera was turned by. cb1 (review F1): the
    // picture being shown is the one the camera hook set up last (the same rule the
    // left eye, HUD and laser use); flat1: nothing to do on the 2D screen.
    if (EngineActive() && !g_flatNow) {
        static LONG s_shown;
        LONG seq = g_ringDone;
        if (seq && seq != s_shown) {
            s_shown = seq;
            auto &r = g_ring[seq & 7];
            if (r.eye >= 0) XrSetSubmitEye(r.eye, r.q, r.p);
            else XrSetSubmitPose(r.q, r.p);
        }
    }

    static DWORD lastLog;
    DWORD t = GetTickCount();
    if (t - lastLog > 10000) {
        lastLog = t;
        LogV("engine: UpdateMatrices calls %ld, head applied %ld, matrix-built cameras skipped %ld (last 10 s)", g_calls,
            g_applied, g_flagged);
        g_calls = g_applied = g_flagged = 0;
    }
}

// ---- Menu (menu.cpp) ----
float EngineWorldScale() { return g_worldScale; }
void EngineSetWorldScale(float v) {
    if (v < 0.3f) v = 0.3f;
    if (v > 3.0f) v = 3.0f;
    g_worldScale = v;
    SaveWorldScale();
}
bool EngineGetOpt(int which) {
    switch (which) {
    case kOptStereo: return g_stereo;
    case kOpt6dof: return g_6dof;
    case kOptVrFov: return g_vrFov;
    case kOptHorizon: return g_horizon;
    case kOptCamera: return g_enabled;
    case kOptGunFov: return g_gunFov;
    }
    return false;
}
void EngineSetOpt(int which, bool on) {
    switch (which) {
    case kOptStereo: g_stereo = on; break;
    case kOpt6dof: g_6dof = on; break;
    case kOptVrFov: g_vrFov = on; break;
    case kOptHorizon: g_horizon = on; break;
    case kOptCamera: g_enabled = on; break;
    case kOptGunFov: g_gunFov = on; break;
    }
}
int EngineLookMode() { return g_lookMode; }
void EngineSetLookMode(int m) { g_lookMode = m < 0 || m > 2 ? 2 : m; }
// look3 (DrGonzaga 2 Nov: neater than retrying): the learnt turn rates are kept in
// fc3vr.ini, so after the first time no learning (and no cutscene trouble) at all.
void EngineSetLookRates(float yaw, float pitch) {
    if (fabsf(yaw) < 0.05f || fabsf(pitch) < 0.05f) return;
    g_steer.sYaw = yaw, g_steer.sPitch = pitch, g_steer.calibrated = true;
    Log("lookaim: turn rates from fc3vr.ini (%.2f, %.2f rad/s); no learning needed", yaw, pitch);
}
float *EngineSteerTuning() { return g_steerTune; }  // [0] gain, [1] dead zone
float EngineSteerError() { return g_steer.errN ? g_steer.errSum / g_steer.errN * 57.3f : 0.f; }
bool EngineHeadPose(float m[16], float off[3]) {
    LONG idx = g_headIdx;
    if (idx < 0) return false;
    memcpy(m, g_heads[idx].m, 64), memcpy(off, g_heads[idx].off, 12);
    return true;
}

