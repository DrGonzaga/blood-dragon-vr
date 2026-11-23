// Debug menu (menu1): an ImGui panel floating in front of you in the game
// world, drawn into each eye like the aim beam (laser.cpp), so it is in 3D.
// Open/close: click both thumbsticks together (or Insert on the keyboard);
// close with the X in its corner, the Close button or B too.
// Point at it with the right controller; right trigger clicks, right stick
// up/down scrolls, B closes. While it is open the game gets no controller input.
//
// Settings that should last are saved to fc3vr.ini ([aim], [vr] WorldScale).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <wincodec.h>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_impl_dx11.h"
#include "fc3vr.h"

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);
static CompileFn g_compile;

// For imgui_dx11.cpp.
extern "C" HRESULT WINAPI FC3VR_D3DCompile(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                                LPCSTR entry, LPCSTR target, UINT f1, UINT f2, ID3DBlob **code, ID3DBlob **err) {
    if (!g_compile) return E_FAIL;
    return g_compile(src, size, name, defs, inc, entry, target, f1, f2, code, err);
}

namespace {

const int kTexW = 1300, kTexH = 900;
void SynthFonts();  // synth1 (synth.inc)  // bow6p: bigger (text was cut off)
const float kPanelW = 1.04f, kPanelH = 0.72f, kDist = 0.9f;  // metres

const char kShader[] =
    "cbuffer C : register(b0) { float4 corner[4]; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.p = corner[id]; o.uv = float2(id & 1, id >> 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n";

bool g_failed, g_ready;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
ID3D11Texture2D *g_tex;
ID3D11RenderTargetView *g_texRtv;
ID3D11ShaderResourceView *g_texSrv;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
ID3D11RasterizerState *g_rs;
ID3D11DepthStencilState *g_ds;
ID3D11BlendState *g_bs;
ID3D11SamplerState *g_smp;
IDXGISwapChain *g_sc;
ID3D11RenderTargetView *g_rtv;
UINT g_w, g_h;

volatile bool g_open;
bool g_recentreOnClose = true;  // recentre1: closing the menu recentres the view (DrGonzaga)
volatile DWORD g_openedAt, g_lastShown;
// Panel in the recentred XR space (x right, y up, -z forward, metres): centre and axes.
float g_c[3], g_r[3], g_u[3], g_n[3];
bool g_placed;

// Controller state for the menu, written by the input thread.
CRITICAL_SECTION g_padLock;
VrPad g_pad;
LARGE_INTEGER g_lastUi;
float g_mouse[2] = {-1, -1};
bool g_hit;
// menu2: the picture panel beside the menu (Controls pages)
bool g_picOn = true;             // [menu] SidePicture
int g_picRight = 1;              // [menu] SidePictureRight (0 = left of the menu)
float g_picHeight = 0.72f;       // [menu] SidePictureHeight (m)
float g_picAngle = 25;           // [menu] SidePictureAngle (deg, turned towards you)

// Saved settings.
void IniPathW(wchar_t *p) { IniPath(p); }
// save1 (DrGonzaga 3 Nov, perf check): a slider used to write fc3vr.ini on every
// frame while it was dragged; each key rewrites the whole file, so the HUD tab
// (~34 keys a save) froze the game ~110 ms a frame. Now changes made in the
// menu are only noted; once nothing in the menu is held (slider let go), they
// are written in one go on a worker thread, so the game never waits on the file.
namespace {
struct PendKey {
    wchar_t sec[32], key[48];
    float v;
};
SRWLOCK g_saveLock = SRWLOCK_INIT;
PendKey g_pendKeys[96];
int g_npend;
void (*g_laterFn[12])();
int g_nlater;
volatile bool g_queueSaves;  // true while the menu is being built
volatile LONG g_saveBusy;
struct SaveJob {
    PendKey keys[96];
    int n;
    void (*fn[12])();
    int nfn;
};
void WriteKey(const wchar_t *sec, const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPathW(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(sec, key, buf, path);
}
DWORD WINAPI SaveWorker(void *p) {
    SaveJob *j = (SaveJob *)p;
    for (int i = 0; i < j->n; ++i) WriteKey(j->keys[i].sec, j->keys[i].key, j->keys[i].v);
    for (int i = 0; i < j->nfn; ++i) j->fn[i]();
    HeapFree(GetProcessHeap(), 0, j);
    InterlockedExchange(&g_saveBusy, 0);
    return 0;
}
// Run a module's own save (HudSave, ScopeSave...) once the menu is let go.
void Later(void (*fn)()) {
    AcquireSRWLockExclusive(&g_saveLock);
    bool have = false;
    for (int i = 0; i < g_nlater; ++i) have |= g_laterFn[i] == fn;
    bool full = !have && g_nlater >= 12;
    if (!have && !full) g_laterFn[g_nlater++] = fn;
    ReleaseSRWLockExclusive(&g_saveLock);
    if (full) fn();
}
// Write what's waiting, if a write isn't already going on.
void FlushSaves() {
    if (!g_npend && !g_nlater) return;
    if (InterlockedCompareExchange(&g_saveBusy, 1, 0) != 0) return;
    SaveJob *j = (SaveJob *)HeapAlloc(GetProcessHeap(), 0, sizeof(SaveJob));
    if (!j) return (void)InterlockedExchange(&g_saveBusy, 0);
    AcquireSRWLockExclusive(&g_saveLock);
    memcpy(j->keys, g_pendKeys, sizeof(PendKey) * g_npend), j->n = g_npend;
    memcpy(j->fn, g_laterFn, sizeof(g_laterFn[0]) * g_nlater), j->nfn = g_nlater;
    g_npend = g_nlater = 0;
    ReleaseSRWLockExclusive(&g_saveLock);
    HANDLE t = CreateThread(nullptr, 0, SaveWorker, j, 0, nullptr);
    if (t) CloseHandle(t);
    else SaveWorker(j);  // no thread: write here
}
}  // namespace

void SaveF(const wchar_t *sec, const wchar_t *key, float v) {
    if (g_queueSaves) {
        AcquireSRWLockExclusive(&g_saveLock);
        int i = 0;
        while (i < g_npend && (wcscmp(g_pendKeys[i].sec, sec) || wcscmp(g_pendKeys[i].key, key))) ++i;
        bool ok = i < g_npend || (g_npend < 96 && wcslen(sec) < 32 && wcslen(key) < 48);
        if (ok) {
            if (i == g_npend) wcscpy(g_pendKeys[i].sec, sec), wcscpy(g_pendKeys[i].key, key), ++g_npend;
            g_pendKeys[i].v = v;
        }
        ReleaseSRWLockExclusive(&g_saveLock);
        if (ok) return;
    }
    WriteKey(sec, key, v);
}
float LoadF(const wchar_t *sec, const wchar_t *key, float def) {
    {  // save1: a change still waiting to be written wins
        AcquireSRWLockShared(&g_saveLock);
        for (int i = 0; i < g_npend; ++i)
            if (!wcscmp(g_pendKeys[i].sec, sec) && !wcscmp(g_pendKeys[i].key, key)) {
                float v = g_pendKeys[i].v;
                ReleaseSRWLockShared(&g_saveLock);
                return v;
            }
        ReleaseSRWLockShared(&g_saveLock);
    }
    wchar_t path[MAX_PATH], buf[32], d[32];
    IniPathW(path);
    swprintf(d, 32, L"%.3f", def);
    GetPrivateProfileStringW(sec, key, d, buf, 32, path);
    return (float)_wtof(buf);
}

// bow3: a slider with < > buttons for fine steps; true when a change is finished (save then).
bool FineSlider(const char *label, float *v, float lo, float hi, const char *fmt, float step) {
    bool done = false;
    ImGui::PushID(label);
    // fine2 (DrGonzaga 8 Nov): << >> = the old step, < > = a tenth of it.
    float h = ImGui::GetFrameHeight();
    if (ImGui::Button("<<##dd", ImVec2(h * 1.3f, h))) *v = fmaxf(lo, *v - step), done = true;
    ImGui::SameLine(0, 4);
    if (ImGui::ArrowButton("##dn", ImGuiDir_Left)) *v = fmaxf(lo, *v - step * 0.1f), done = true;
    ImGui::SameLine(0, 4);
    if (ImGui::ArrowButton("##up", ImGuiDir_Right)) *v = fminf(hi, *v + step * 0.1f), done = true;
    ImGui::SameLine(0, 4);
    if (ImGui::Button(">>##uu", ImVec2(h * 1.3f, h))) *v = fminf(hi, *v + step), done = true;
    ImGui::SameLine();
    ImGui::PopID();
    ImGui::SliderFloat(label, v, lo, hi, fmt);
    return done | ImGui::IsItemDeactivatedAfterEdit();
}

void LoadSettings() {
    HolsterLoad();  // holster1
    EngineSetLookMode((int)LoadF(L"aim", L"LookFollows", 2));
    EngineSetLookRates(LoadF(L"aim", L"LookTurnRate", 0), LoadF(L"aim", L"LookTiltRate", 0));
    float *t = EngineSteerTuning();
    t[0] = LoadF(L"aim", L"FollowStrength", t[0]);
    t[1] = LoadF(L"aim", L"StickDeadZone", t[1]);
    if (t[0] < 1 || t[0] > 30) t[0] = 8;
    if (t[1] < 0 || t[1] > 0.6f) t[1] = 0.25f;
    LaserSetOn(LoadF(L"aim", L"Beam", 1) != 0);
    *LaserLength() = LoadF(L"aim", L"BeamLength", 40);
    *LaserStart() = LoadF(L"aim", L"BeamStart", 0.05f);
    *LaserFade() = LoadF(L"aim", L"BeamFadeIn", 0);
    *LaserWidth() = LoadF(L"aim", L"BeamWidth", 1);
    *InputSwingOn() = LoadF(L"aim", L"SwingMelee", 1) != 0;
    *InputCrouchOn() = LoadF(L"aim", L"StickCrouch", 1) != 0;
    *InputSwingSpeed() = LoadF(L"aim", L"SwingSpeed", 2.5f);
    *TwoHandOn() = LoadF(L"twohand", L"On", 1) != 0;  // twohand1
    *TwoHandReach() = LoadF(L"twohand", L"Reach", 0.15f);
    *TwoHandFar() = LoadF(L"twohand", L"Furthest", 0.80f);
    *TwoHandStockAim() = LoadF(L"twohand", L"StockAim", 1) != 0;  // stockaim1
    *TwoHandSmoothing() = LoadF(L"twohand", L"Smoothing", 0.3f);  // twosmooth1
    *TwoHandCupReach() = LoadF(L"twohand", L"HandsTogether", 0.15f);  // pistolgrip1
    *PistolSteadyOn() = LoadF(L"twohand", L"PistolSteady", 1) != 0;  // steady1
    *PistolSteadyBuzz() = LoadF(L"twohand", L"SteadyBuzz", 1) != 0;  // steadybuzz1
    *PistolSteadyHz() = LoadF(L"twohand", L"PistolSteadyHz", 1.0f);
    *PistolSwing() = LoadF(L"twohand", L"PistolSwing", 2.0f);
    *BowPhysicalOn() = LoadF(L"twohand", L"PhysicalBow", 1) != 0;  // bow1p
    *BowStringOn() = LoadF(L"twohand", L"BowString", 1) != 0;  // string10
    *BowAimAlongArrow() = true, *BowArrowsAlongAimOn() = true;  // aimfix1 / quiver4 (tidy1: always on)
    *BowZeroUp() = LoadF(L"twohand", L"BowZeroUp", 0), *BowZeroRight() = LoadF(L"twohand", L"BowZeroRight", 0);  // quiver5
    *BowSteerBoostPtr() = LoadF(L"twohand", L"BowSteerBoost", 2.5f);  // quiver7
    *BowNoSpread() = LoadF(L"twohand", L"BowNoSpread", 1) != 0;  // quiver9
    BowLoadArrowLine();
    *BowArrowSpot() = (int)LoadF(L"twohand", L"BowArrowSpot", 2);
    if (*BowArrowSpot() < 0 || *BowArrowSpot() > 3) *BowArrowSpot() = 2;
    *BowNockReach() = LoadF(L"twohand", L"BowNockReach", 0.15f);
    *BowFullDraw() = LoadF(L"twohand", L"BowFullDraw", 0.65f);
    *BowArrowAhead() = LoadF(L"twohand", L"BowArrowAhead", 0.0f);
    *BowArrowLength() = LoadF(L"twohand", L"ArrowTipAhead", 0.70f);  // bow7p (was BowArrowLength + BowArrowAhead)
    *BowDropAmount() = LoadF(L"twohand", L"BowDrop", 8.0f);
    *BowNockAhead() = LoadF(L"twohand", L"BowNockAhead", 0.0f);  // bow7p
    *BowNockHands() = LoadF(L"twohand", L"BowNockHands", 0.40f);  // bow12p
    *BowUnnockAngle() = LoadF(L"twohand", L"BowUnnockRoll", 55);  // unnock5 (was BowUnnockAngle: another measure)
    *BowUnnockCancel() = LoadF(L"twohand", L"BowUnnockCancel", 1) != 0;  // unnock2
    *BowDropIn() = LoadF(L"twohand", L"BowDropIn", 0.04f);
    *BowDropBack() = LoadF(L"twohand", L"BowDropBack", 0.08f);
    *BowReturnTime() = LoadF(L"twohand", L"BowReturnTime", 0.25f);
    *BowSteerStart() = LoadF(L"twohand", L"BowSteerStart", 0.06f);  // bowfeel1
    *BowSteerRange() = LoadF(L"twohand", L"BowSteerRange", 0.12f);
    *BowSteerWeight() = LoadF(L"twohand", L"BowSteerWeight", 0.7f);
    *BowKeepAngle() = LoadF(L"twohand", L"BowKeepAngle", 20);
    *BowNockTick() = LoadF(L"twohand", L"BowNockTick", 0.8f);
    *BowDipDown() = LoadF(L"twohand", L"BowDipDown", 0.03f);  // bow15p
    *BowDipForward() = LoadF(L"twohand", L"BowDipForward", 0.03f);
    *BowFirstArrowShift() = LoadF(L"twohand", L"FirstArrowShift", 0.65f);
    BowNockPoint()[0] = LoadF(L"twohand", L"NockPointRight", 0);
    BowNockPoint()[1] = LoadF(L"twohand", L"NockPointUp", 0);
    BowNockPoint()[2] = LoadF(L"twohand", L"NockPointForward", 0);
    {   // bow6p: bow in the left hand / arrow in the right hand poses
        static const wchar_t *const kB[6] = {L"BowHandRight", L"BowHandUp", L"BowHandForward", L"BowHandPitch", L"BowHandYaw", L"BowHandRoll"};
        static const wchar_t *const kA[6] = {L"ArrowGripRight", L"ArrowGripUp", L"ArrowGripForward", L"ArrowGripPitch", L"ArrowGripYaw", L"ArrowGripRoll"};
        *BowHandPoseSet() = LoadF(L"twohand", kB[0], 1e9f) < 1e8f;
        for (int i = 0; i < 6; ++i) {
            if (*BowHandPoseSet()) BowHandPose()[i] = LoadF(L"twohand", kB[i], 0);
            BowArrowPose()[i] = LoadF(L"twohand", kA[i], 0);
            static const wchar_t *const kF[6] = {L"FirstGripRight", L"FirstGripUp", L"FirstGripForward", L"FirstGripPitch", L"FirstGripYaw", L"FirstGripRoll"};
            BowArrowPoseFirst()[i] = LoadF(L"twohand", kF[i], BowArrowPose()[i]);  // arrowsets1: starts as the shared values were
        }
    }
    *LaserOpacity() = LoadF(L"aim", L"BeamOpacity", 0.75f);
    *LaserStopsAtWalls() = LoadF(L"aim", L"BeamStopsAtWalls", 1) != 0;  // wall1
    *LaserDotOn() = LoadF(L"aim", L"HitGlow", 1) != 0;  // dot1
    *LaserDotSize() = LoadF(L"aim", L"HitGlowSize", 0.015f);
    *LaserSightMoa() = LoadF(L"aim", L"SightDotMoa", 3.0f);  // sight1
    *LaserDotGlow() = LoadF(L"aim", L"HitGlowHalo", 0.6f);
    *LaserDotRange() = LoadF(L"aim", L"HitGlowRange", 150.0f);  // dot3
    LaserOffset()[0] = LoadF(L"aim", L"AimOffsetRight", 0);
    LaserOffset()[1] = LoadF(L"aim", L"AimOffsetUp", 0);
    BoneHookSetShots(LoadF(L"aim", L"ShotsFollowController", 1) != 0);
    *BoneHookHipSpread() = LoadF(L"aim", L"HipFireSpread", 0.3f);
    g_stockOn = LoadF(L"aim", L"GunstockAccuracy", 1) != 0;  // stock1
    g_gliderSteady = LoadF(L"vr", L"SteadyGlider", 1) != 0;  // glide1
    g_cutSteady = LoadF(L"vr", L"CutsceneSteady", 1) != 0;  // cut1
    g_cutFlat = LoadF(L"vr", L"CutsceneScreen", 1) != 0;  // flat1
    g_cutScreen[0] = LoadF(L"vr", L"CutsceneScreenDistance", 3.0f);
    g_cutScreen[1] = LoadF(L"vr", L"CutsceneScreenWidth", 2.8f);
    g_cutScreen[2] = LoadF(L"vr", L"CutsceneScreenHeight", 0.0f);
    g_cutWide = LoadF(L"vr", L"CutsceneScreen169", 1) != 0;  // movie1
    g_cutZoom = LoadF(L"vr", L"CutsceneScreenZoom", 1.0f);
    g_seqBySeq = LoadF(L"vr", L"CutsceneBySequence", 1) != 0;
    g_seqInter3D = LoadF(L"vr", L"CutscenePromptsIn3D", 1) != 0;  // seq1
    g_seqByMap = LoadF(L"vr", L"CutsceneByInputMode", 0) != 0;
    g_seqHold = LoadF(L"vr", L"CutscenePromptHold", 8.0f);
    g_turretSteady = LoadF(L"vr", L"TurretSteadyView", 1) != 0;  // turret1
    g_turretAnchor = LoadF(L"vr", L"TurretAnchor", 1) != 0 ? 1 : 0;  // turanchor1
    g_turretAbove = LoadF(L"vr", L"TurretSteadyAbove", 1) != 0 ? 1 : 0;  // turabove1
    g_turretLift = LoadF(L"vr", L"TurretAboveLift", 0.3f);  // turabove2
    g_turretRide = LoadF(L"vr", L"TurretRideVehicle", 1) != 0 ? 1 : 0;  // turride1
    g_turretDragon = LoadF(L"vr", L"TurretDragonRules", 1) != 0 ? 1 : 0;  // turdragon1
    g_turretLaser = LoadF(L"vr", L"TurretLaser", 1) != 0;
    g_turretMotion = LoadF(L"vr", L"TurretMotion", 1) != 0;  // bd13
    g_turretHeight = LoadF(L"vr", L"TurretHeight", 0);
    if (g_turretHeight < -0.5f || g_turretHeight > 0.5f) g_turretHeight = 0;
    *BoneHookShotFromHand() = LoadF(L"aim", L"ShotsFromBeam", 1) != 0;
    *BoneHookNoPeek() = LoadF(L"aim", L"NoCoverLift", 1) != 0;
    BoneHookPeekApply();
    *HeadtrackAutoRecenter() = LoadF(L"vr", L"AutoRecentre", 1) != 0;
    g_recentreOnClose = LoadF(L"vr", L"RecentreOnMenuClose", 1) != 0;
    g_soundHead = LoadF(L"vr", L"SoundFollowsHead", 1) != 0;
    g_skipLoot = LoadF(L"gameplay", L"SkipLootAnimation", 1) != 0;
    LootApply();
    g_outpostAnytime = LoadF(L"gameplay", L"OutpostResetAnytime", 1) != 0;
    OutpostApply();
    g_damageTaken = LoadF(L"gameplay", L"DamageTaken", 1);
    if (g_damageTaken < 0.25f || g_damageTaken > 5) g_damageTaken = 1;
    *InputRumbleStrengthPtr() = LoadF(L"input", L"RumbleStrength", 1);
    *InputRumbleRealPadPtr() = LoadF(L"input", L"RumbleRealPad", 0) != 0;
    *EngineRoomScale() = LoadF(L"vr", L"RoomScale", 1) != 0;
    *EngineRoomStart() = LoadF(L"vr", L"RoomScaleStart", 0.30f);
    *EngineSnapTurn() = LoadF(L"vr", L"SnapTurn", 1) != 0;  // snap1
    *EngineSnapAngle() = LoadF(L"vr", L"SnapTurnAngle", 45);
    *HeadtrackHandSmoothing() = LoadF(L"aim", L"ControllerSmoothing", 0);
    *EngineScopeOn() = LoadF(L"aim", L"ScopeEye", 1) != 0;
    *EngineScopePip() = LoadF(L"aim", L"ScopeStereo", 0) != 0;  // pip1
    *EngineScopeNarrow() = LoadF(L"aim", L"ScopeNarrow", 1) != 0;  // narrow1
    {
        int o = (int)LoadF(L"aim", L"ScopeOutside", 0);
        *VrsScopeOutside() = o < 0 ? 0 : o > 2 ? 2 : o;
    }
    *EngineScopeSmoothing() = LoadF(L"aim", L"ScopeSteadiness", 0.4f);
    *ScopeSize() = LoadF(L"aim", L"ScopeSize", 14);
    *ScopeZoom() = LoadF(L"aim", L"ScopeExtraZoom", 2);
    ScopeReticle()[0] = LoadF(L"aim", L"ScopeReticleRight", 0);
    ScopeReticle()[1] = LoadF(L"aim", L"ScopeReticleUp", 0);
    *ScopeReticleOpacity() = LoadF(L"aim", L"ScopeReticleOpacity", 0.9f);
    *ScopeMatchLaser() = LoadF(L"aim", L"ScopeReticleMatchLaser", 0) != 0;  // match1
    *ScopeReticleGlow() = LoadF(L"aim", L"ScopeReticleGlow", 0.3f);
    *ScopeDistortion() = LoadF(L"aim", L"ScopeDistortion", 0.3f);  // lens1
    *ScopeHideHud() = LoadF(L"aim", L"ScopeHideHud", 1) != 0;  // nohud1
    *InputScopeZoomButtons() = LoadF(L"aim", L"ScopeZoomButtons", 1) != 0;  // zoom1
    *EngineScopeCrouchSteady() = LoadF(L"aim", L"ScopeCrouchSteady", 1.5f);  // crouchsteady1
    *EngineScopeClassic() = LoadF(L"aim", L"ScopeClassicSmoothing", 0) != 0;
    *LaserOffWithoutGun() = LoadF(L"aim", L"BeamOffWithoutGun", 1) != 0;  // nogun1
    *GunKickFollowsShots() = LoadF(L"aim", L"KickFollowsShots", 1) != 0;  // aimdev1
    *LaserDotOnly() = LoadF(L"aim", L"DotOnly", 0) != 0;  // dotonly1
    *LaserBehindObjects() = LoadF(L"aim", L"BeamBehindObjects", 1) != 0;  // occl2
    *EngineLadderTurn() = LoadF(L"vr", L"LadderTurn", 1) != 0;  // ladder1
    *EngineZipRecentre() = LoadF(L"vr", L"ZiplineRecentre", 1) != 0;  // ziprec1
    *EngineVehRecentre() = LoadF(L"vr", L"VehicleRecentre", 1) != 0;  // vehrec1
    *EngineVehExitKeep() = LoadF(L"vr", L"VehicleExitKeepFacing", 1) != 0;  // vehface1
    *EngineKeepView() = LoadF(L"vr", L"KeepViewAfterPause", 1) != 0;   // keepview2 (new key, on)
    g_objHead = LoadF(L"hud", L"QuestIconByHead", 1) != 0;  // objhead1
    g_objScale = LoadF(L"hud", L"QuestIconScale", 4);
    if (g_objScale < 0.5f || g_objScale > 8) g_objScale = 4;
    *LaserTurretDotOnly() = LoadF(L"vr", L"TurretDotOnly", 1) != 0;  // lasers1
    *LaserTurretDotRange() = LoadF(L"vr", L"TurretDotRange", 250);
    *LaserThrowOn() = LoadF(L"aim", L"ThrowBeam", 1) != 0;
    *LaserThrowValue(0) = LoadF(L"aim", L"ThrowBeamLength", 0.35f);
    *LaserThrowValue(1) = LoadF(L"aim", L"ThrowBeamWidth", 5);
    *LaserThrowValue(2) = LoadF(L"aim", L"ThrowBeamOpacity", 0.8f);
    *LaserThrowColour() = (int)LoadF(L"aim", L"ThrowBeamColour", 0) & 3;  // lasers2
    *EngineScopeAhead() = LoadF(L"aim", L"ScopeCameraAhead", 0.9f);
    *InputTriggerSens() = LoadF(L"input", L"RightTriggerSensitivity", 1.5f);  // lasers3
    InputSlowSpeeds()[0] = LoadF(L"gameplay", L"AdsSlow", 1);  // ads1
    InputSlowSpeeds()[1] = LoadF(L"gameplay", L"CrouchSpeed", 1);
    *InputCrouchCap() = LoadF(L"gameplay", L"CrouchStickCap", 0.5f);  // adscrouch1
    *StockAdsDrop() = LoadF(L"gameplay", L"AdsHeightDrop", 0.15f);  // adsmove1
    *EngineRealCrouch() = LoadF(L"gameplay", L"RealCrouch", 1) != 0;  // realcrouch1
    *EngineRealCrouchDepth() = LoadF(L"gameplay", L"RealCrouchDepth", 0.30f);
    g_smaa = (int)LoadF(L"render", L"SMAA", 0);  // smaa1
    g_lodFix = LoadF(L"render", L"LodFix", 0);  // lod1
    // bd74 heads1 (DrGonzaga 15 Nov: "It was smallest part size for heads. Detail step scale seemed to improve image as
    // well. Can you please keep those sliders there, and reset the other two to default"). The two kept ones have
    // their own keys now and take over the bd71 test values once; the cut-off and zoom tests are gone (always 1).
    g_minSizeScale = LoadF(L"render", L"SmallPartSize", LoadF(L"render", L"TestMinSizeScale", 0.3f));
    g_detailScale = LoadF(L"render", L"DetailStepScale", LoadF(L"render", L"TestDetailScale", 1));
    // tests2 (DrGonzaga 15 Nov: "I'm getting more pop-in overall, especially of characters ... could we please return
    // the test sliders"): the cut-off and zoom tests are back, under their bd71 keys (default 1).
    g_killScale = LoadF(L"render", L"TestKillScale", 1), g_zoomScale = LoadF(L"render", L"TestZoomScale", 1);
    if (g_smaa < 0 || g_smaa > 4) g_smaa = 0;
    g_mirrorEvery = (int)LoadF(L"vr", L"DesktopMirrorEvery", 3);  // mirror2: every 3rd again (the crashes were the game's MT rendering)
    if (g_mirrorEvery < 1 || g_mirrorEvery > 45) g_mirrorEvery = 3;
    EngineSetOpt(kOptGunFov, LoadF(L"aim", L"GunMatchesHeadsetFov", 1) != 0);
    *EngineOffCentreOn() = LoadF(L"render", L"OffCentre", 1) != 0;  // oc3
    *StereoFastCheck() = LoadF(L"perf", L"FastBufferCheck", 1) != 0;
    *StereoWakeFast() = LoadF(L"perf", L"WakeFast", 1) != 0;  // wakefast1
    *StereoWakeKeep() = (int)LoadF(L"perf", L"WakeKeep", 5);  // fast1, made leak-free in wake3
    if (*StereoWakeKeep() < 0 || *StereoWakeKeep() > 6) *StereoWakeKeep() = 5;
    *VrsOn() = LoadF(L"render", L"Vrs", 0) != 0;  // vrs1
    *VrsInner() = LoadF(L"render", L"VrsFullDeg", 30);
    *VrsMid() = LoadF(L"render", L"VrsHalfDeg", 40);
    GfxApplyLive();
    HudLoad();
    g_hideObjIcon = LoadF(L"hud", L"HideQuestIcon", 1) != 0;  // mark1
    CyberEyeLoad();  // cybereye1
    g_hideMarkIcon = LoadF(L"hud", L"HideMapMarkerIcon", 1) != 0;
    g_mapHead = LoadF(L"hud", L"MinimapFollowsHead", 1) != 0;  // headmap1
    InputMapLoad();
    *VrLeftHanded() = LoadF(L"vr", L"LeftHanded", 0) != 0;  // lefty1
    Log("menu: %s", *VrLeftHanded() ? "LEFT-HANDED mode ([vr] LeftHanded=1): the controllers swap roles" : "right-handed ([vr] LeftHanded=0)");
    ComfortLoad();  // comfort1
    g_picOn = LoadF(L"menu", L"SidePicture", 1) != 0;  // menu2
    g_picRight = LoadF(L"menu", L"SidePictureRight", 1) != 0;
    g_picHeight = LoadF(L"menu", L"SidePictureHeight", 0.72f);
    if (g_picHeight < 0.3f || g_picHeight > 1.2f) g_picHeight = 0.72f;
    g_picAngle = LoadF(L"menu", L"SidePictureAngle", 25);
    // Enemy tag icons off unless chosen (DrGonzaga, 30 Oct): engine switch
    // g_disableTagIcon, read by the tag icon drawing (0x11abd480).
    wchar_t ini[MAX_PATH];
    IniPath(ini);
    int tag = GfxVarFind("g_disableTagIcon");
    if (tag >= 0 && GetPrivateProfileIntW(L"gfxlive", L"g_disableTagIcon", -1, ini) < 0) GfxVarSetBool(tag, true);
}

void Release() {
    if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
    g_sc = nullptr;
}

bool Setup(IDXGISwapChain *sc) {
    if (g_failed) return false;
    if (!g_ready) {
        ID3D11Device *dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev))) return g_failed = true, false;
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        g_compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr;
        if (!g_compile || FAILED(g_compile(kShader, sizeof kShader - 1, "menu", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, nullptr)) ||
            FAILED(g_compile(kShader, sizeof kShader - 1, "menu", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, nullptr))) {
            Log("menu: shader compile failed; menu off");
            dev->Release();
            return g_failed = true, false;
        }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
        vsb->Release(), psb->Release();
        D3D11_BUFFER_DESC bd = {64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        dev->CreateBuffer(&bd, nullptr, &g_cb);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &g_rs);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dev->CreateDepthStencilState(&dd, &g_ds);
        D3D11_BLEND_DESC bld = {};
        auto &b = bld.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D11_BLEND_SRC_ALPHA, b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, b.BlendOp = D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D11_BLEND_ONE, b.DestBlendAlpha = D3D11_BLEND_ZERO, b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bld, &g_bs);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &g_smp);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kTexW, td.Height = kTexH, td.MipLevels = 1, td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &g_tex);
        if (g_tex) dev->CreateRenderTargetView(g_tex, nullptr, &g_texRtv), dev->CreateShaderResourceView(g_tex, nullptr, &g_texSrv);
        dev->GetImmediateContext(&g_ctx);
        g_dev = dev;
        if (!g_ctx || !g_vs || !g_ps || !g_cb || !g_rs || !g_ds || !g_bs || !g_smp || !g_texRtv || !g_texSrv) {
            Log("menu: setup incomplete; menu off");
            return g_failed = true, false;
        }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2((float)kTexW, (float)kTexH);
        io.FontGlobalScale = 1.6f;
        io.MouseDrawCursor = true;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // synth1: the stick moves between items, A presses
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        SynthFonts();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(1.4f);
        if (!ImGui_ImplDX11_Init(dev, g_ctx)) {
            Log("menu: ImGui DX11 init failed; menu off");
            return g_failed = true, false;
        }
        g_ready = true;
        Log("menu: ready (both thumbsticks or Insert opens it)");
    }
    if (sc != g_sc) {
        Release();
        ID3D11Texture2D *bb = nullptr;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) return false;
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        g_w = td.Width, g_h = td.Height;
        HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
        if (FAILED(hr)) return false;
        g_sc = sc;
    }
    return true;
}

float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Put the panel in front of where the head looks (level), a little low.
void Place() {
    float m[16], off[3];
    if (!EngineHeadPose(m, off)) return;
    float f[3] = {-m[2], 0, -m[10]};  // head forward (-z column), level
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (l < 1e-3f) f[0] = 0, f[2] = -1, l = 1;
    f[0] /= l, f[2] /= l;
    for (int i = 0; i < 3; ++i) g_c[i] = off[i] + f[i] * kDist;
    g_c[1] -= 0.10f;
    g_n[0] = -f[0], g_n[1] = 0, g_n[2] = -f[2];  // towards the viewer
    g_u[0] = 0, g_u[1] = 1, g_u[2] = 0;
    g_r[0] = -f[2], g_r[1] = 0, g_r[2] = f[0];  // right = forward x up
    g_placed = true;
}

// Where the right controller points on the panel, in texture pixels.
void Point() {
    float m[16], off[3];
    g_hit = false;
    if (!HeadtrackHandRaw(1, m, off)) return;  // bow6p: the real right hand (the bow turns HeadtrackHand(1))
    float d[3] = {-m[2], -m[6], -m[10]};
    float dn = Dot(d, g_n);
    if (dn > -1e-3f) return;  // pointing away
    float pc[3] = {g_c[0] - off[0], g_c[1] - off[1], g_c[2] - off[2]};
    float t = Dot(pc, g_n) / dn;
    if (t < 0) return;
    float h[3] = {off[0] + d[0] * t - g_c[0], off[1] + d[1] * t - g_c[1], off[2] + d[2] * t - g_c[2]};
    float x = Dot(h, g_r) / kPanelW + 0.5f, y = 0.5f - Dot(h, g_u) / kPanelH;
    if (x < -0.05f || x > 1.05f || y < -0.05f || y > 1.05f) return;
    g_mouse[0] = x * kTexW, g_mouse[1] = y * kTexH;
    g_hit = true;
}

bool Check(const char *label, bool v, void (*set)(bool), const wchar_t *iniKey = nullptr) {
    bool b = v;
    if (ImGui::Checkbox(label, &b)) {
        set(b);
        if (iniKey) SaveF(L"aim", iniKey, b ? 1.f : 0.f);
        return true;
    }
    return false;
}

// Live engine switches worth having up front: variable, label, and whether
// "on" in the menu means the variable is 0 (the engine's are mostly "disable" flags).
struct LiveItem {
    const char *var, *label;
    bool inverted;
};
const LiveItem kLive[] = {
    {"gfx_disablePost", "Post effects (bloom, colour grading, damage effects)", true},
    {"gfx_disableFog", "Fog", true},
    {"gfx_disablePostAtmosphericScattering", "Atmospheric haze", true},
};

// gameplay1 (DrGonzaga 6 Nov): time of day, damage and other game rules on their own tab.
void GameplayPage() {
    if (ImGui::SliderFloat("Damage I take", &g_damageTaken, 0.5f, 4.0f, "%.2fx"))
        g_damageTaken = roundf(g_damageTaken * 20) / 20, SaveF(L"gameplay", L"DamageTaken", g_damageTaken);
    ImGui::TextDisabled("1.00x = the game's normal. Enemies still take normal damage.");
    if (ImGui::Checkbox("Skip looting animations", &g_skipLoot))
        SaveF(L"gameplay", L"SkipLootAnimation", g_skipLoot ? 1.f : 0.f), LootApply();
    if (OutpostAvailable()) {
        if (ImGui::Checkbox("Reset outposts any time", &g_outpostAnytime))
            SaveF(L"gameplay", L"OutpostResetAnytime", g_outpostAnytime ? 1.f : 0.f), OutpostApply();
        ImGui::TextDisabled("Game menu > Options > Reset outposts works without finishing the story or freeing every outpost.");
    }
    ImGui::Separator();
    // ads1 / realcrouch1 / lie1 (DrGonzaga 8 Nov): movement.
    float *sl = InputSlowSpeeds();
    ImGui::Text("Real head %.2f m below standing.  %s", EngineHeadDrop(),
                EngineCrouched() ? "Crouched" : "Standing");
    if (ImGui::Checkbox("Crouch for real to crouch in game", EngineRealCrouch()))
        SaveF(L"gameplay", L"RealCrouch", *EngineRealCrouch() ? 1.f : 0.f);
    float cd = *EngineRealCrouchDepth() * 100;
    if (ImGui::SliderFloat("Crouch when my head is down", &cd, 15, 60, "%.0f cm")) *EngineRealCrouchDepth() = cd / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"gameplay", L"RealCrouchDepth", *EngineRealCrouchDepth());
    ImGui::TextDisabled("Measured from your height at the last recentre (F9 / after loading), so recentre standing.");
    bool ads = sl[0] > 0.5f;
    if (ImGui::Checkbox("Slower with the gun at my face (aiming down sights)", &ads))
        sl[0] = ads ? 1.f : 0.f, SaveF(L"gameplay", L"AdsSlow", sl[0]);
    if (StockGunName()[0]) {
        char lb[96];
        snprintf(lb, sizeof lb, "Aim-down-sights speed (%s)", StockGunName());
        ImGui::SliderFloat(lb, StockAdsSpeed(), 0.1f, 1.0f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) Later(StockSave);
        ImGui::SameLine();
        ImGui::TextDisabled(StockGunAtFace() ? "(at face now)" : "(per gun)");
    }
    float ad = *StockAdsDrop() * 100;  // adsmove1
    if (ImGui::SliderFloat("Gun counts as up when no lower than", &ad, 0, 100, ad >= 99 ? "any height" : "%.0f cm below my eyes"))
        *StockAdsDrop() = ad / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"gameplay", L"AdsHeightDrop", *StockAdsDrop());
    ImGui::SameLine();
    ImGui::TextDisabled("(now %.0f cm)", -StockAdsHeight() * 100);
    ImGui::TextDisabled("'At the face' = within the gun's 'accurate within' distance (Aim tab) and up at that height.");
    ImGui::SliderFloat("Crouched speed", &sl[1], 0.1f, 1.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"gameplay", L"CrouchSpeed", sl[1]);
    ImGui::SliderFloat("Crouched top speed at stick", InputCrouchCap(), 0.2f, 1.0f, "%.2f");  // adscrouch1
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"gameplay", L"CrouchStickCap", *InputCrouchCap());
    ImGui::TextDisabled("How far you push the stick before crouched walking is at full speed. Slowdowns start from there.");
}

void GraphicsPage() {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "Render resolution");
    {
        static UINT cw, ch, rw, rh;
        static float want;
        if (!cw) ResGetSize(&cw, &ch);
        if (!rw && !XrRecommendedSize(&rw, &rh)) rw = 0;
        if (rw && cw) {
            if (want == 0) want = roundf((float)cw / rw * 20) / 20;
            ImGui::SetNextItemWidth(380);
            if (ImGui::SliderFloat("Scale", &want, 0.5f, 2.0f, "%.2f")) want = roundf(want * 20) / 20;
            UINT ww = (UINT)(rw * want + 8) / 16 * 16, wh = (UINT)(rh * want + 8) / 16 * 16;
            ImGui::SameLine();
            ImGui::Text("= %u x %u", ww, wh);
            ImGui::Text("Now %u x %u (%.2f).  1.00 = the headset's recommended %u x %u.", cw, ch, (float)cw / rw, rw, rh);
            ImGui::TextDisabled("Higher = sharper but more GPU time. VD's own quality setting changes what 1.00 is.");
            if (ww != cw || wh != ch) {
                if (ImGui::Button("Apply now (experimental: headset blinks)")) ResSetSize(ww, wh, true), cw = ww, ch = wh;
                ImGui::SameLine();
                if (ImGui::Button("Save for next launch")) ResSetSize(ww, wh, false);
            }
        } else {
            ImGui::TextDisabled("(waiting for the headset's recommended size)");
        }
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "Live (changes at once)");
    if (GfxBloomAvailable()) {
        bool b = GfxBloomOn();
        if (ImGui::Checkbox("Bloom (glow around bright light)", &b)) GfxSetBloom(b);
    }
    for (const auto &it : kLive) {
        int i = GfxVarFind(it.var);
        if (i < 0 || GfxVarType(i) != 1) continue;
        bool v = GfxVarBool(i) != it.inverted;
        if (ImGui::Checkbox(it.label, &v)) GfxVarSetBool(i, v != it.inverted);
    }
    int ex = GfxVarFind("gfx_overrideExposure");
    if (ex >= 0 && GfxVarType(ex) == 2) {
        float e = GfxVarFloat(ex);
        bool on = e > 0;
        if (ImGui::Checkbox("Fixed exposure (brightness)", &on)) GfxVarSetFloat(ex, on ? 1.0f : -1.0f);
        if (on) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            if (ImGui::SliderFloat("##exp", &e, 0.1f, 4.0f, "%.2f")) GfxVarSetFloat(ex, e);
        }
    }
    if (ImGui::Button("Reset live settings to the game's")) GfxResetLive();
    ImGui::TextDisabled("(takes effect on the next launch)");
    ImGui::Separator();
    {
        // lod1: detail distance (heads, wheels, small parts) back to the flat game's.
        int pct = (int)(g_lodFix * 100 + 0.5f);
        ImGui::SetNextItemWidth(380);
        if (ImGui::SliderInt("Detail distance fix", &pct, 0, 100, "%d%%")) g_lodFix = pct / 100.f, SaveF(L"render", L"LodFix", g_lodFix);
        ImGui::TextDisabled("100%%: models switch to their detailed versions as far away as in the flat game. Lower is cheaper.");
        if (g_bloodDragon) {
            // bd74 heads1: the two that mattered in the bd71 test, kept as settings.
            ImGui::SetNextItemWidth(380);
            if (ImGui::SliderFloat("Smallest part drawn", &g_minSizeScale, 0.0f, 2.0f, "x%.2f")) g_lodScaleTick = GetTickCount(), SaveF(L"render", L"SmallPartSize", g_minSizeScale);
            ImGui::TextDisabled("How small on screen a part may get before the game stops drawing it (distant heads, wheels).\n"
                                "x1.00 is the flat game's own limit, which hides heads early in a headset. x0.00 draws everything.");
            ImGui::SetNextItemWidth(380);
            if (ImGui::SliderFloat("Detail step scale", &g_detailScale, 0.1f, 10.0f, "x%.2f", ImGuiSliderFlags_Logarithmic)) g_lodScaleTick = GetTickCount(), SaveF(L"render", L"DetailStepScale", g_detailScale);
            ImGui::TextDisabled("Detail steps (the game's LodScale). x1.00 is the game's own. Which way is further is not proven yet.");
            // tests2: the other two bd71 test sliders back, for trying
            ImGui::SetNextItemWidth(380);
            if (ImGui::SliderFloat("Cut-off distance scale", &g_killScale, 0.1f, 10.0f, "x%.2f", ImGuiSliderFlags_Logarithmic)) g_lodScaleTick = GetTickCount(), SaveF(L"render", L"TestKillScale", g_killScale);
            ImGui::TextDisabled("Where whole models stop being drawn (the game's KillLodScale). x1.00 is the game's own; in bd68 lower showed less.");
            ImGui::SetNextItemWidth(380);
            if (ImGui::SliderFloat("Zoom scale", &g_zoomScale, 0.1f, 10.0f, "x%.2f", ImGuiSliderFlags_Logarithmic)) g_lodScaleTick = GetTickCount(), SaveF(L"render", L"TestZoomScale", g_zoomScale);
            ImGui::TextDisabled("The game's zoom number for distances. x1.00 is the game's own.");
            if (ImGui::Button("All four back to the game's own (x1.00)"))
                g_minSizeScale = g_detailScale = g_killScale = g_zoomScale = 1, g_lodScaleTick = GetTickCount(),
                SaveF(L"render", L"SmallPartSize", 1), SaveF(L"render", L"DetailStepScale", 1), SaveF(L"render", L"TestKillScale", 1), SaveF(L"render", L"TestZoomScale", 1);
        }
    }
    {
        // smaa1: SMAA on each eye's image, live.
        static const char *q[] = {"Off", "Low", "Medium", "High", "Ultra"};
        ImGui::SetNextItemWidth(380);
        if (ImGui::Combo("Anti-aliasing (SMAA, now)", &g_smaa, q, 5)) SaveF(L"render", L"SMAA", (float)g_smaa);
        ImGui::TextDisabled("Smooths jagged edges for about 0.2-0.4 ms per eye. Off = exactly as before.");
        ImGui::TextDisabled("Closest to SMAA 2x: this plus MSAA 2x below.");
    }
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.5f, 1), "Next launch (the game reads these at start)");
    for (int i = 0; i < kProfileCount; ++i) {
        const auto &s = kProfile[i];
        char cur[16];
        GfxProfileChosen(i, cur, 16);
        int sel = -1;
        for (int k = 0; k < s.count; ++k)
            if (!strcmp(s.values[k], cur)) sel = k;
        ImGui::SetNextItemWidth(380);
        if (ImGui::BeginCombo(s.label, sel >= 0 ? s.names[sel] : cur)) {
            for (int k = 0; k < s.count; ++k)
                if (ImGui::Selectable(s.names[k], k == sel)) GfxProfileChoose(i, s.values[k]);
            ImGui::EndCombo();
        }
        if (strcmp(cur, GfxProfileNow(i))) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "(restart)");
        }
    }
    ImGui::TextDisabled("MSAA costs a lot of GPU time at headset resolution.");
    ImGui::Separator();
    static bool showAll;
    ImGui::Checkbox("Show every engine variable (advanced: some may crash the game)", &showAll);
    if (showAll) {
        ImGui::TextDisabled("Point and drag numbers left/right to change them.");
        ImGui::BeginChild("vars", ImVec2(0, 300), true);
        for (int i = 0; i < GfxVarCount(); ++i) {
            const char *n = GfxVarName(i);
            ImGui::PushID(i);
            if (GfxVarType(i) == 1) {
                bool v = GfxVarBool(i);
                if (ImGui::Checkbox(n, &v)) GfxVarSetBool(i, v);
            } else {
                float v = GfxVarFloat(i);
                ImGui::SetNextItemWidth(200);
                if (ImGui::DragFloat(n, &v, 0.01f, 0, 0, "%.3f")) GfxVarSetFloat(i, v);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

// menu2 (DrGonzaga 3 Nov): pages reordered by use. Settings that are used sit at the top of
// each page; ones never changed from the start (none of their keys in fc3vr.ini on 3 Nov)
// moved into a closed "More ..." section at the bottom of their page. Aim split into
// "Aim" (look, shooting, melee) and "Beam & scope".
bool More(const char *label) {
    ImGui::Spacing();
    return ImGui::CollapsingHeader(label);
}

void VrPage(const float *t) {
    if (ImGui::Button("Recentre view (F9)")) HeadtrackRequestRecenter();
    float ws = EngineWorldScale();
    if (ImGui::SliderFloat("World scale", &ws, 0.5f, 2.0f, "%.2f")) EngineSetWorldScale(ws);
    ImGui::TextDisabled("Bigger number = smaller world");
    // snap1 (DrGonzaga 11 Nov): snap turn on the right stick, with its angle.
    if (ImGui::Checkbox("Ladders: turn me to face the ladder when I get on with my back to it (climbing down)", EngineLadderTurn()))  // ladder1
        SaveF(L"vr", L"LadderTurn", *EngineLadderTurn() ? 1.f : 0.f);
    if (g_bloodDragon && ImGui::Checkbox("Ziplines: recentre the view when I come off the line", EngineZipRecentre()))  // ziprec1
        SaveF(L"vr", L"ZiplineRecentre", *EngineZipRecentre() ? 1.f : 0.f);
    if (ImGui::Checkbox("Vehicles: face the front when I get in (and the game's way when I get out, if the next box is off)", EngineVehRecentre()))  // vehrec1
        SaveF(L"vr", L"VehicleRecentre", *EngineVehRecentre() ? 1.f : 0.f);
    if (ImGui::Checkbox("Vehicles: keep facing the way I looked when I get out (just shift out)", EngineVehExitKeep()))  // vehface1
        SaveF(L"vr", L"VehicleExitKeepFacing", *EngineVehExitKeep() ? 1.f : 0.f);
    if (ImGui::Checkbox("After a pause, menu, shop or terminal: keep looking where I was, no turn (not after vehicles)", EngineKeepView()))  // keepview2
        SaveF(L"vr", L"KeepViewAfterPause", *EngineKeepView() ? 1.f : 0.f);
    if (ImGui::Checkbox("Snap turn (right stick left/right)", EngineSnapTurn()))
        SaveF(L"vr", L"SnapTurn", *EngineSnapTurn() ? 1.f : 0.f);
    if (*EngineSnapTurn()) {
        static const char *names[] = {"15 deg", "22.5 deg", "30 deg", "45 deg", "60 deg", "90 deg"};
        static const float val[] = {15, 22.5f, 30, 45, 60, 90};
        int cur = 3;
        for (int i = 0; i < 6; ++i)
            if (fabsf(val[i] - *EngineSnapAngle()) < 0.1f) cur = i;
        if (ImGui::Combo("Snap angle", &cur, names, 6)) *EngineSnapAngle() = val[cur], SaveF(L"vr", L"SnapTurnAngle", val[cur]);
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(off = smooth turn)");
    }
    if (ImGui::Checkbox("Roomscale: walking in the room moves you in the game", EngineRoomScale()))
        SaveF(L"vr", L"RoomScale", *EngineRoomScale() ? 1.f : 0.f);
    {
        float cm = *EngineRoomStart() * 100;
        if (ImGui::SliderFloat("Body follows after", &cm, 15, 60, "%.0f cm")) {
            *EngineRoomStart() = cm / 100;
            SaveF(L"vr", L"RoomScaleStart", *EngineRoomStart());
        }
    }
    {
        static const char *mm[] = {"Every frame", "Every 3rd frame", "Every 10th frame", "Every 45th frame"};
        static const int ev[] = {1, 3, 10, 45};
        int cur = 0;
        for (int i = 0; i < 4; ++i)
            if (ev[i] == g_mirrorEvery) cur = i;
        if (ImGui::Combo("Desktop window shows", &cur, mm, 4))
            g_mirrorEvery = ev[cur], SaveF(L"vr", L"DesktopMirrorEvery", (float)g_mirrorEvery);
        ImGui::TextDisabled("Fewer = more GPU time for the headset (the headset always gets every frame)");
    }
    ImGui::Separator();
    // flat1 (DrGonzaga 11 Nov): cutscenes on a flat 2D screen, as in the normal game
    if (ImGui::Checkbox("Cutscenes on a 2D screen (as in the normal game)", &g_cutFlat))
        SaveF(L"vr", L"CutsceneScreen", g_cutFlat ? 1.f : 0.f);
    if (g_cutFlat) {
        ImGui::SliderFloat("Cutscene screen distance", &g_cutScreen[0], 1.0f, 10.0f, "%.1f m");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"CutsceneScreenDistance", g_cutScreen[0]);
        ImGui::SliderFloat("Cutscene screen size (width)", &g_cutScreen[1], 0.5f, 12.0f, "%.1f m");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"CutsceneScreenWidth", g_cutScreen[1]);
        ImGui::SliderFloat("Cutscene screen height", &g_cutScreen[2], -1.5f, 1.5f, "%+.2f m");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"CutsceneScreenHeight", g_cutScreen[2]);
        // movie1 (DrGonzaga 12 Nov): a 16:9 movie screen
        if (ImGui::Checkbox("16:9 movie screen (off = the whole picture, taller)", &g_cutWide))
            SaveF(L"vr", L"CutsceneScreen169", g_cutWide ? 1.f : 0.f);
        if (g_cutWide) {
            ImGui::SliderFloat("Movie picture zoom", &g_cutZoom, 0.5f, 2.5f, "%.2fx");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"CutsceneScreenZoom", g_cutZoom);
        }
        ImGui::TextDisabled("Off = cutscenes in VR, turned by your head only. Open this menu in a cutscene to see the screen.");
    }
    // seq1 (DrGonzaga 11 Nov): scenes that ask for input stay in VR
    if (ImGui::Checkbox("Cutscenes with button prompts stay in 3D", &g_seqInter3D))
        SaveF(L"vr", L"CutscenePromptsIn3D", g_seqInter3D ? 1.f : 0.f);
    if (g_seqInter3D) {
        ImGui::SliderFloat("Stay in 3D after the last prompt", &g_seqHold, 1.0f, 30.0f, "%.0f s");
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"CutscenePromptHold", g_seqHold);
    }
    // movie1: the game's own "a cutscene sequence is playing" (scripted scenes through your own eyes too)
    if (ImGui::Checkbox("Scripted scenes without control go on the screen too", &g_seqBySeq))
        SaveF(L"vr", L"CutsceneBySequence", g_seqBySeq ? 1.f : 0.f);
    if (ImGui::Checkbox("TEST: first-person scripted scenes (no control) also on the 2D screen", &g_seqByMap))
        SaveF(L"vr", L"CutsceneByInputMode", g_seqByMap ? 1.f : 0.f);
    // mark1 (DrGonzaga 11 Nov): tell Claude what was and wasn't a cutscene (marked lines in fc3vr.log)
    {
        char nb[40];
        ImGui::Text("Cutscene detection: %s   (game input mode: %s)",
                    EngineCutscene() ? (EngineCutsceneIn3D() ? "IN A CUTSCENE with prompts (3D)" : "IN A CUTSCENE now") : "not in a cutscene now",
                    EngineInputModeName(nb, sizeof nb));
    }
    if (ImGui::Button("That WAS a cutscene")) EngineCutsceneMark(true);
    ImGui::SameLine();
    if (ImGui::Button(EngineCutscene() ? "This is NOT a cutscene (back to VR)" : "That was NOT a cutscene")) EngineCutsceneMark(false);
    ImGui::TextDisabled("Press just after it happens. Marked so far: %ld was, %ld wasn't", EngineCutsceneMarks(true), EngineCutsceneMarks(false));
    ImGui::Separator();
    ImGui::Text("Frame time (ms)   CPU left %.1f  right %.1f   GPU left %.1f  right %.1f", t[0], t[1], t[3], t[4]);
    ImGui::Text("Headset wait %.1f ms   (90 Hz budget 11.1 ms)", t[2]);
    if (g_bloodDragon && More("Cyber-eye (left grip held at the head)")) {  // cybermenu1
        bool cs = false;
        ImGui::SliderFloat("Cyber-eye smoothing (how slowly the view follows the head)", CyberEyeSmoothPtr(), 0, 1.5f, "%.2f s");
        cs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Cyber-eye screen size (width)", CyberEyeWidthPtr(), 0.3f, 3, "%.2f m");
        cs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Cyber-eye screen distance", CyberEyeDistPtr(), 0.3f, 5, "%.2f m");
        cs |= ImGui::IsItemDeactivatedAfterEdit();
        float sl = *CyberEyeScanPtr() * 100;
        if (ImGui::SliderFloat("Cyber-eye scan lines", &sl, 0, 100, "%.0f%%")) *CyberEyeScanPtr() = sl / 100;
        cs |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::Checkbox("Keep the horizon level when I tilt my head (the screen turns against the tilt)", CyberEyeLevel())) cs = true;
        if (ImGui::Checkbox("Cyber-eye on only while I hold the left grip at my head (off = hold once for on, again for off)", CyberEyeHoldKeep())) cs = true;  // cyhold1
        ImGui::TextDisabled("Size as seen = width against distance: 0.90 m at 1.00 m fills about 48 degrees.");
        if (cs) CyberEyeSave();
    }
    if (More("More VR and gameplay options")) {
        if (ImGui::Checkbox("Recentre when I close this menu", &g_recentreOnClose))
            SaveF(L"vr", L"RecentreOnMenuClose", g_recentreOnClose ? 1.f : 0.f);
        if (ImGui::Checkbox("Recentre automatically after loading", HeadtrackAutoRecenter()))
            SaveF(L"vr", L"AutoRecentre", *HeadtrackAutoRecenter() ? 1.f : 0.f);
        if (ImGui::Checkbox("Sound follows my head", &g_soundHead))
            SaveF(L"vr", L"SoundFollowsHead", g_soundHead ? 1.f : 0.f);
        if (ImGui::Checkbox("Steady hang glider view (smooths the game's sway)", &g_gliderSteady))
            SaveF(L"vr", L"SteadyGlider", g_gliderSteady ? 1.f : 0.f);
        if (ImGui::Checkbox("Cutscene handling (gun and laser rest during cutscenes)", &g_cutSteady))  // cut1
            SaveF(L"vr", L"CutsceneSteady", g_cutSteady ? 1.f : 0.f);
        // oc3: saved
        if (ImGui::Checkbox("Off-centre eye view (draw only what each eye can show: sharper)", EngineOffCentreOn()))
            SaveF(L"render", L"OffCentre", *EngineOffCentreOn() ? 1.f : 0.f);
        // vrs1 (trial)
        if (ImGui::Checkbox("Coarser shading toward the edges (variable rate shading: faster)", VrsOn()))
            SaveF(L"render", L"Vrs", *VrsOn() ? 1.f : 0.f);
        if (*VrsOn()) {
            ImGui::SameLine(), ImGui::TextDisabled("%s", VrsState());
            float *vi = VrsInner(), *vm = VrsMid();
            ImGui::SliderFloat("Full detail out to", vi, 10, 60, "%.0f deg from straight ahead");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"render", L"VrsFullDeg", *vi);
            if (*vm < *vi) *vm = *vi;
            ImGui::SliderFloat("Half detail out to (quarter beyond)", vm, 10, 65, "%.0f deg");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"render", L"VrsHalfDeg", *vm);
            if (*vm < *vi) *vm = *vi;
        }
        // turret1
        if (ImGui::Checkbox("Steady view on mounted guns (stays put while the gun swings)", &g_turretSteady))
            SaveF(L"vr", L"TurretSteadyView", g_turretSteady ? 1.f : 0.f);
        if (g_turretSteady) {  // turanchor1
            ImGui::Indent();
            if (ImGui::RadioButton("Stay with the gun's mount (moving vehicles)", g_turretAnchor == 1)) g_turretAnchor = 1, SaveF(L"vr", L"TurretAnchor", 1.f);
            ImGui::SameLine();
            if (ImGui::RadioButton("At the player's place (older)", g_turretAnchor == 0)) g_turretAnchor = 0, SaveF(L"vr", L"TurretAnchor", 0.f);
            if (g_turretAnchor == 1) {  // turabove1
                bool ab = g_turretAbove != 0;
                if (ImGui::Checkbox("Held a set height above the gun, no camera shake (helicopter gun)", &ab))
                    g_turretAbove = ab ? 1 : 0, SaveF(L"vr", L"TurretSteadyAbove", ab ? 1.f : 0.f);
                if (g_turretAbove) {  // turride1
                    bool rv = g_turretRide != 0;
                    if (ImGui::Checkbox("Move and turn with the vehicle the gun is on", &rv))
                        g_turretRide = rv ? 1 : 0, SaveF(L"vr", L"TurretRideVehicle", rv ? 1.f : 0.f);
                    bool dr = g_turretDragon != 0;  // turdragon1
                    if (ImGui::Checkbox("Same rules on the armoured dragon (final level)", &dr))
                        g_turretDragon = dr ? 1 : 0, SaveF(L"vr", L"TurretDragonRules", dr ? 1.f : 0.f);
                }
                if (g_turretAbove) {  // turabove2
                    float cm = g_turretLift * 100;
                    if (ImGui::SliderFloat("Extra height above the gun##turlift", &cm, -30, 100, "%+.0f cm")) g_turretLift = cm / 100;
                    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"TurretAboveLift", g_turretLift);
                }
            }
            ImGui::Unindent();
        }
        if (ImGui::Checkbox("Mounted guns follow the right controller", &g_turretMotion))  // bd13
            SaveF(L"vr", L"TurretMotion", g_turretMotion ? 1.f : 0.f);
        if (ImGui::Checkbox("Aiming laser on mounted guns", &g_turretLaser))
            SaveF(L"vr", L"TurretLaser", g_turretLaser ? 1.f : 0.f);
        if (g_turretLaser) {  // lasers1
            ImGui::SameLine();
            if (ImGui::Checkbox("Dot only (no beam)##tur", LaserTurretDotOnly()))
                SaveF(L"vr", L"TurretDotOnly", *LaserTurretDotOnly() ? 1.f : 0.f);
            ImGui::SliderFloat("Mounted gun dot reaches", LaserTurretDotRange(), 50, 400, "%.0f m");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"TurretDotRange", *LaserTurretDotRange());
        }
        float th = g_turretHeight * 100;
        if (FineSlider("Mounted gun eye height", &th, -50, 50, "%+.0f cm", 10)) SaveF(L"vr", L"TurretHeight", th / 100);
        g_turretHeight = th / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"vr", L"TurretHeight", g_turretHeight);
    }
    // lefty3 (DrGonzaga 22 Nov: "Can we just have it as experimental in the debug menu, under VR options, with an
    // explanation as to how to manually move guns over?"): off the VR menu, here with its how-to.
    if (More("Left-handed mode (EXPERIMENTAL)")) {
        bool lh = *VrLeftHanded();
        if (ImGui::Checkbox("Left-handed: the gun is in my LEFT hand", &lh)) {
            *VrLeftHanded() = lh, SaveF(L"vr", L"LeftHanded", lh ? 1.f : 0.f);
            Log("menu: %s mode (debug menu)", lh ? "left-handed" : "right-handed");
        }
        if (lh && ImGui::Checkbox("Mirror the game's arms (its left hand holds the gun; not the bow, Killstar or zipline)", HandsMirrorArms()))
            SaveF(L"vr", L"LeftHandedMirrorArms", *HandsMirrorArms() ? 1.f : 0.f);
        ImGui::TextWrapped(
            "Experimental. The controllers swap roles: gun, trigger, aim, holster grabs and melee on the LEFT controller, "
            "walking and the two-handed grip on the RIGHT; the sticks, A/X and B/Y swap. The Menu button stays on the left.");
        ImGui::TextWrapped(
            "Each gun keeps a separate LEFT-hand position and angle; your right-hand settings are never changed. "
            "The first time you pick up a gun left-handed it starts as a mirror image of its right-hand setting "
            "(Right, Yaw and Roll flip sign) and is saved.");
        ImGui::TextWrapped(
            "To move a gun over by hand: switch this on, hold the gun in your left hand, open Guns > Gun & body and use "
            "the Right / Up / Forward and Pitch / Yaw / Roll sliders. They now change only the left-hand values and save on "
            "release. Do each gun once. The button below starts the current gun again from its right-hand setting.");
        ImGui::TextWrapped(
            "In fc3vr.ini each gun's section [gun.NAME] has Right, Up, Forward, Pitch, Yaw, Roll (right hand) and "
            "LeftRight, LeftUp, LeftForward, LeftPitch, LeftYaw, LeftRoll (left hand). You can copy numbers across there "
            "with the game closed: Right, Yaw and Roll need their sign flipped. Delete the Left keys to start a gun again from the mirror.");
        if (lh && GunAttachName()[0]) {
            if (ImGui::Button("Reset this gun's left hand from its right hand (mirrored)")) GunAttachLeftFromRight(), Later(GunAttachSave);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", GunAttachName());
        }
    }
}

void AimPage() {
    static const char *modes[] = {"Right stick only (flat game)", "Head", "Right controller"};
    int mode = EngineLookMode();
    ImGui::Text("Gun, pickups and headlamp follow:");
    for (int i = 2; i >= 0; --i)
        if (ImGui::RadioButton(modes[i], mode == i)) EngineSetLookMode(i), SaveF(L"aim", L"LookFollows", (float)i);
    float *tu = EngineSteerTuning();
    if (ImGui::SliderFloat("Follow strength", &tu[0], 2, 20, "%.1f")) SaveF(L"aim", L"FollowStrength", tu[0]);
    ImGui::TextDisabled("Higher = gun catches up faster, too high wobbles");
    ImGui::Text("Gun off target: %.1f degrees on average (last 10 s)", EngineSteerError());
    if (ImGui::SliderFloat("Controller smoothing", HeadtrackHandSmoothing(), 0, 1, "%.2f"))
        SaveF(L"aim", L"ControllerSmoothing", *HeadtrackHandSmoothing());
    ImGui::SameLine();
    ImGui::TextDisabled("(0 = off; steadier aim, a little lag)");
    ImGui::Separator();
    Check("Shots follow the controller exactly", BoneHookShotsOn(), BoneHookSetShots, L"ShotsFollowController");
    // twosmooth1 (DrGonzaga 10 Nov): two-handed mode replaces the head-distance accuracy with the grips.
    bool twoSpread = *TwoHandOn();
    if (twoSpread) {
        ImGui::TextWrapped("Two-handed mode is on, so head-distance accuracy is off: right grip only = this gun's "
                           "worst spread, both grips (or the bow held) = its best. Turn two-handed holding off to go back.");
    } else if (ImGui::Checkbox("Gunstock accuracy (spread grows as the controller moves away from your head)", &g_stockOn))
        SaveF(L"aim", L"GunstockAccuracy", g_stockOn ? 1.f : 0.f);
    if (g_stockOn || twoSpread) {
        StockGun *g = StockSettings();
        if (StockGunName()[0])
            ImGui::Text("Gunstock settings for %s (saved per gun):", StockGunName());
        else
            ImGui::Text("Gunstock settings (per gun, once a gun is in hand):");
        bool sv = false;
        ImGui::SetNextItemWidth(380);
        sv |= ImGui::SliderFloat("Accurate within", &g->nearM, 0.05f, 0.8f, "%.2f m");
        ImGui::SetNextItemWidth(380);
        sv |= ImGui::SliderFloat("Worst at", &g->farM, 0.1f, 1.0f, "%.2f m");
        ImGui::SetNextItemWidth(380);
        sv |= ImGui::SliderFloat("Spread at best", &g->best, 0, 5, "%.2f deg");
        ImGui::SetNextItemWidth(380);
        sv |= ImGui::SliderFloat("Spread at worst", &g->worst, 0, 15, "%.2f deg");
        if (g->farM < g->nearM + 0.02f) g->farM = g->nearM + 0.02f;
        if (sv) Later(StockSave);
        float d, sp, gm;
        StockLast(&d, &sp, &gm);
        ImGui::Text("Controller is %.2f m from your head now.", StockLiveDistance());
        if (d >= 0) ImGui::TextDisabled("Last shot: %.2f m, spread %.2f deg (the game's own was %.2f)", d, sp, gm);
    } else {
        if (ImGui::SliderFloat("Hip-fire spread", BoneHookHipSpread(), 0, 1, "%.2f")) SaveF(L"aim", L"HipFireSpread", *BoneHookHipSpread());
        ImGui::TextDisabled("1 = the game's own, lower = tighter when not aiming down sights (left trigger)");
    }
    {
        // kick1: hip-fire recoil per gun (gun model and beam; shots unchanged). Saved with the gun's beam settings.
        if (GunAttachName()[0]) ImGui::Text("Hip recoil for %s (saved per gun):", GunAttachName());
        else ImGui::Text("Hip recoil (per gun, once a gun is in your hand):");
        bool bs = false;
        float *k = GunAttachKick();
        ImGui::TextDisabled(*TwoHandOn() ? "Two-handed mode: 'gun close' with both grips, 'arm out' with the right grip only."
                                         : "Grows as the gun moves out, between the gunstock distances above.");
        if (ImGui::Checkbox("Recoil, beam and dot follow where each bullet went", GunKickFollowsShots()))  // aimdev1
            SaveF(L"aim", L"KickFollowsShots", *GunKickFollowsShots() ? 1.f : 0.f);
        ImGui::TextDisabled("On: the gun (with its beam and dot) kicks towards each bullet's real path, then settles.");
        ImGui::SliderFloat("Hip recoil up, gun close", &k[3], 0, 12, "%.1f deg");
        bs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Hip recoil up, arm out", &k[0], 0, 12, "%.1f deg");
        bs |= ImGui::IsItemDeactivatedAfterEdit();
        float ccm = k[4] * 100;
        if (ImGui::SliderFloat("Hip recoil back, gun close", &ccm, 0, 10, "%.1f cm")) k[4] = ccm / 100;
        bs |= ImGui::IsItemDeactivatedAfterEdit();
        float bcm = k[1] * 100;
        if (ImGui::SliderFloat("Hip recoil back, arm out", &bcm, 0, 10, "%.1f cm")) k[1] = bcm / 100;
        bs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Hip recoil direction", &k[2], -180, 180, "%.0f deg");
        bs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::TextDisabled("(direction: 0 = straight up from your hand, + turns it clockwise; all 0 = off;\n with 'follow' on, used only when a bullet goes straight down the beam)");
        if (bs) Later(GunAttachSaveBeam);
    }
    {  // lasers1 (DrGonzaga 10 Nov): grenade / molotov throw: a short fat stub for the angle, no dot
        ImGui::Separator();
        if (ImGui::Checkbox("Throwing laser (grenade / molotov): short stub, no dot", LaserThrowOn()))
            SaveF(L"aim", L"ThrowBeam", *LaserThrowOn() ? 1.f : 0.f);
        if (*LaserThrowOn()) {
            float cm = *LaserThrowValue(0) * 100;
            if (ImGui::SliderFloat("Throwing laser length", &cm, 10, 150, "%.0f cm")) *LaserThrowValue(0) = cm / 100;
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"aim", L"ThrowBeamLength", *LaserThrowValue(0));
            ImGui::SliderFloat("Throwing laser width", LaserThrowValue(1), 1, 12, "%.1fx");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"aim", L"ThrowBeamWidth", *LaserThrowValue(1));
            ImGui::SliderFloat("Throwing laser opacity", LaserThrowValue(2), 0.1f, 1, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"aim", L"ThrowBeamOpacity", *LaserThrowValue(2));
            static const char *tc[4] = {"Red##tc", "Green##tc", "Blue##tc", "Orange##tc"};  // lasers2
            for (int i = 0; i < 4; ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(tc[i], *LaserThrowColour() == i)) *LaserThrowColour() = i, SaveF(L"aim", L"ThrowBeamColour", (float)i);
            }
            ImGui::TextDisabled("Shows while a chest grenade / molotov is held (or X is held, in grenade mode).");
        }
    }
    if (More("More aim options")) {
        if (ImGui::SliderFloat("Stick dead zone", &tu[1], 0, 0.5f, "%.2f")) SaveF(L"aim", L"StickDeadZone", tu[1]);
        ImGui::TextDisabled("Raise if the gun stops short of the target, lower if it jitters");
        if (ImGui::Checkbox("Bullets fly along the beam (from the gun)", BoneHookShotFromHand()))
            SaveF(L"aim", L"ShotsFromBeam", *BoneHookShotFromHand() ? 1.f : 0.f);
        if (ImGui::Checkbox("No lift over cover when firing", BoneHookNoPeek())) {
            SaveF(L"aim", L"NoCoverLift", *BoneHookNoPeek() ? 1.f : 0.f);
            BoneHookPeekApply();
        }
        if (ImGui::Checkbox("Pull right stick down to crouch / stand", InputCrouchOn())) SaveF(L"aim", L"StickCrouch", *InputCrouchOn() ? 1.f : 0.f);
        bool gf = EngineGetOpt(kOptGunFov);
        if (ImGui::Checkbox("Gun drawn at headset field of view", &gf))
            EngineSetOpt(kOptGunFov, gf), SaveF(L"aim", L"GunMatchesHeadsetFov", gf ? 1.f : 0.f);
        ImGui::TextDisabled("Off = the game's own gun field of view (gun looks stretched and too big in VR)");
    }
}

void BeamScopePage() {
    Check("Aim beam", LaserOn(), LaserSetOn, L"Beam");
    ImGui::SameLine();
    if (ImGui::Checkbox("Stops at walls", LaserStopsAtWalls()))
        SaveF(L"aim", L"BeamStopsAtWalls", *LaserStopsAtWalls() ? 1.f : 0.f);
    if (ImGui::Checkbox("Beam and dot can go behind things in front of them (all guns; each gun's own ticks are below)", LaserBehindObjects()))  // occl2
        SaveF(L"aim", L"BeamBehindObjects", *LaserBehindObjects() ? 1.f : 0.f);
    if (ImGui::Checkbox("Beam off when there's no gun in my hand", LaserOffWithoutGun()))
        SaveF(L"aim", L"BeamOffWithoutGun", *LaserOffWithoutGun() ? 1.f : 0.f);
    // sight5 (DrGonzaga 10 Nov): menu reorganised. This gun and optic first: what shows (laser beam,
    // red dot where it lands, reticle through the sight, any mix) and the colour; then one fold-out
    // section for each. Everything there is saved per gun (beam, dot) or per gun and optic (what shows, reticle).
    ImGui::Separator();
    if (GunAttachName()[0]) ImGui::Text("This gun: %s   optic: %s", GunAttachName(), GunAttachOptic());
    else ImGui::Text("This gun: (none yet; settings appear once a gun is in your hand)");
    {
        bool bs = false;
        if (ImGui::Checkbox("Laser beam", LaserShowBeam())) bs = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Red dot where it lands", LaserShowDot())) bs = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Reticle through the sight", LaserSightOn())) bs = true;
        if (*LaserBehindObjects()) {  // occl4: per gun and optic
            if (ImGui::Checkbox("Beam goes behind things", LaserBeamBehind())) bs = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Dot goes behind things (grass hides it)", LaserDotBehind())) bs = true;
        }
        if (ImGui::Checkbox("No scope for this gun with this attachment (the game's zoom shows no lens)", EngineScopeGunOff())) bs = true;  // noscope1
        static const char *cols[4] = {"Red", "Green", "Blue", "Orange"};
        ImGui::Text("Colour:");
        for (int i = 0; i < 4; ++i) {
            ImGui::SameLine();
            char id[24];
            snprintf(id, sizeof id, "%s##bc", cols[i]);
            if (ImGui::RadioButton(id, *LaserColour() == i)) *LaserColour() = i, bs = true;
        }
        if (ImGui::CollapsingHeader("Laser beam settings (this gun)")) {
            ImGui::SliderFloat("Beam length", LaserLength(), 0.5f, 60, "%.1f m");
            bs |= ImGui::IsItemDeactivatedAfterEdit();
            float cm = *LaserStart() * 100;
            if (ImGui::SliderFloat("Beam starts", &cm, 0, 150, "%.0f cm from the hand")) *LaserStart() = cm / 100;  // wall1: max 100 -> 150
            bs |= ImGui::IsItemDeactivatedAfterEdit();
            float fcm = *LaserFade() * 100;
            if (ImGui::SliderFloat("Beam fades in over", &fcm, 0, 50, "%.0f cm")) *LaserFade() = fcm / 100;
            bs |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("Beam width", LaserWidth(), 0.2f, 2.0f, "%.2fx");
            bs |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("Beam opacity", LaserOpacity(), 0.05f, 1, "%.2f");
            bs |= ImGui::IsItemDeactivatedAfterEdit();
        }
        if (ImGui::CollapsingHeader("Red dot settings")) {
            bs |= FineSlider("Dot brightness (this gun)", LaserDotBright(), 0.05f, 2, "%.3f", 0.1f);  // lasers3 (sight3: up to 2x)
            float cm = *LaserDotSize() * 100;
            if (FineSlider("Dot size (all guns)", &cm, 0.3f, 6.0f, "%.2f cm", 0.5f)) {
                *LaserDotSize() = cm / 100;
                SaveF(L"aim", L"HitGlowSize", *LaserDotSize());
            } else *LaserDotSize() = cm / 100;
            if (FineSlider("Dot softness (all guns)", LaserDotGlow(), 0.0f, 2.0f, "%.3f", 0.1f))  // soft1: up to 2
                SaveF(L"aim", L"HitGlowHalo", *LaserDotGlow());
            if (FineSlider("Dot reaches (all guns)", LaserDotRange(), 5.0f, 300.0f, "%.0f m", 10))
                SaveF(L"aim", L"HitGlowRange", *LaserDotRange());
        }
        if (ImGui::CollapsingHeader("Reticle settings (this gun and optic)")) {
            if (!*LaserSightOn()) ImGui::TextDisabled("(tick 'Reticle through the sight' above to see it)");
            ImGui::Checkbox("Show window outline", LaserSightOutline());
            if (ImGui::RadioButton("Round window", *LaserSightShape() == 0)) *LaserSightShape() = 0, bs = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rectangular window", *LaserSightShape() == 1)) *LaserSightShape() = 1, bs = true;
            // sight4 (DrGonzaga 10 Nov): every sight setting with << >> (coarse) and < > (fine) steps.
            static const char *lab[5] = {"Sight window right", "Sight window up", "Sight window forward",
                                         "Sight window width", "Sight window height"};
            static const float lo[5] = {-15, -10, -30, 0.5f, 0.5f}, hi[5] = {15, 20, 60, 12, 12};
            for (int i = 0; i < 5; ++i) {
                if (i == 4 && *LaserSightShape() == 0) continue;
                float cm = *LaserSightValue(i) * 100;
                bool d = FineSlider(lab[i], &cm, lo[i], hi[i], "%.2f cm", i < 3 ? 0.5f : 0.2f);
                *LaserSightValue(i) = cm / 100;
                bs |= d;
            }
            bs |= FineSlider("Sight window turn", LaserSightValue(6), -90, 90, "%.2f deg", 1);  // sight3
            bs |= FineSlider("Sight edge fade starts at", LaserSightValue(5), 0, 0.99f, "%.3f of the edge", 0.05f);
            // sight3 (DrGonzaga 10 Nov): reticles, each with its own brightness / softness / thickness.
            static const char *rets[10] = {"Dot", "Holo ring", "Chevron", "Cross", "Triangle", "Bow pins",
                                           "Target lock", "Tri-prong", "Hex", "Cyber ring"};  // future1: the last four
            for (int i = 0; i < 10; ++i) {
                if (i && i != 6) ImGui::SameLine();
                char id[24];
                snprintf(id, sizeof id, "%s##ret", rets[i]);
                if (ImGui::RadioButton(id, *LaserSightReticle() == i)) *LaserSightReticle() = i, bs = true;
            }
            bs |= FineSlider("Reticle brightness", LaserSightValue(7), 0.05f, 2, "%.3f", 0.1f);
            bs |= FineSlider("Reticle softness", LaserSightValue(8), 0, 2, "%.3f", 0.1f);
            bs |= FineSlider("Dot size / line thickness", LaserSightValue(9), 0.5f, 12, "%.2f MOA", 0.5f);
            if (*LaserSightReticle()) {
                bs |= FineSlider("Reticle size", LaserSightValue(10), 0.25f, 3, "%.3fx", 0.1f);
                // sight4: the reticle's own fine turn (guns are turned a little to line up with the bullets)
                bs |= FineSlider("Reticle turn", LaserSightValue(11), -20, 20, "%.2f deg", 0.5f);
            }
            ImGui::TextDisabled("Tick the outline, then line the cyan window up with the sight's glass.");
        }
        if (bs) Later(GunAttachSaveBeam);
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Scope lens when aiming a scoped gun (left trigger)", EngineScopeOn()))
        SaveF(L"aim", L"ScopeEye", *EngineScopeOn() ? 1.f : 0.f);
    // pip1 (DrGonzaga 21 Nov: picture-in-picture scope)
    if (ImGui::Checkbox("Picture-in-picture scope: the world stays 3D while scoped (an extra picture each frame)", EngineScopePip()))
        SaveF(L"aim", L"ScopeStereo", *EngineScopePip() ? 1.f : 0.f), Log("scope: picture-in-picture %s (menu)", *EngineScopePip() ? "on" : "off");
    if (ImGui::Checkbox("Show the gun while zoomed (the game hides scoped guns)", GunAttachScopeShowGun()))  // scopegun2
        SaveF(L"aim", L"ScopeShowGun", *GunAttachScopeShowGun() ? 1.f : 0.f);
    if (ImGui::Checkbox("Draw only what the lens shows (faster, sharper)", EngineScopeNarrow()))  // narrow1
        SaveF(L"aim", L"ScopeNarrow", *EngineScopeNarrow() ? 1.f : 0.f), Log("scope: draw only what the lens shows %s (menu)", *EngineScopeNarrow() ? "on" : "off");
    if (*EngineScopePip()) {
        static const char *outs[] = {"Full", "Coarse (1 per 4x4 pixels)", "Skipped"};
        ImGui::TextUnformatted("Scope picture outside the lens (NVIDIA RTX only):");
        for (int i = 0; i < 3; ++i) {
            ImGui::SameLine();
            if (ImGui::RadioButton(outs[i], *VrsScopeOutside() == i))
                *VrsScopeOutside() = i, SaveF(L"aim", L"ScopeOutside", (float)i), Log("scope: picture outside the lens: %s (menu)", outs[i]);
        }
        float st[2];
        StereoScopeTimes(st);
        ImGui::TextDisabled("Scope picture cost (last scoped): CPU %.2f ms, GPU %.2f ms. Frame budget at 90 Hz is 11.1 ms.", st[0], st[1]);
    } else
        ImGui::TextDisabled("The world shows flat while scoped; the lens is the game's own zoom, along the gun.");
    if (ScopeGun()[0])
        ImGui::Text("Scope settings for %ls (saved per gun):", ScopeGun());
    else
        ImGui::Text("Scope settings (per gun, once a gun has been scoped):");
    bool ss = false;
    {
        // ret3 (DrGonzaga 6 Nov): four more of our own, on a second row.
        static const char *styles[] = {"MRAD (long range)", "Blue dot", "Green prism", "Black duplex",
                                       "Amber chevron", "Green ring", "Ruby fine cross", "Cyan mil-dot",
                                       "Neon brackets", "Cyber hex", "Tri-prong", "Scanner"};  // future1: the last four
        int *st = ScopeStyle();
        for (int i = 0; i < 12; ++i) {
            if (i & 3) ImGui::SameLine();
            if (ImGui::RadioButton(styles[i], *st == i)) *st = i, ss = true;
        }
    }
    if (ScopeGun()[0]) {  // rifle1 (DrGonzaga 21 Nov): the scope on the rifle, looked into through its eyepiece
        ss |= ImGui::Checkbox("Scope on the rifle for this gun: look into the eyepiece, with an eye box (uses the picture-in-picture scope)", ScopeRifle());
        if (*ScopeRifle()) {
            ss |= ImGui::Checkbox("Scope comes on when the rifle is at your eye (the left trigger then steadies it)", ScopeRifleAuto());  // autoscope1
            if (*ScopeRifleAuto()) {
                ss |= ImGui::Checkbox("Left trigger only steadies the scope (the game doesn't zoom; A / X still zoom the lens)", ScopeRifleSteadyOnly());  // steadyonly1
                if (*ScopeRifleSteadyOnly()) {
                    float deg = *ScopeAutoFovRef() * 57.29578f;
                    if (FineSlider("Scope view width (smaller = more zoom; the lens zoom multiplies it)", &deg, 1.0f, 40.0f, "%.1f deg", 0.5f))
                        *ScopeAutoFovRef() = deg * 0.0174533f, ss = true;
                } else
                    ImGui::TextDisabled("Zoom without the trigger: %.1f deg (learnt each time you scope with the trigger)", ScopeAutoFov() * 57.29578f);
                // autoscope2 (DrGonzaga 21 Nov): when it turns on, per gun.
                ImGui::TextDisabled("When it comes on (it goes off a little further out, so it does not flicker). With a gunstock, raise the reach:");
                ss |= FineSlider("Turn-on reach past the eye distance leeway", ScopeRifleValue(8), 0.0f, 0.80f, "%.3f m", 0.005f);
                ss |= FineSlider("Turn-on distance off the scope's middle", ScopeRifleValue(9), 0.01f, 0.40f, "%.3f m", 0.005f);
                ss |= FineSlider("Turn-on angle between your view and the barrel", ScopeRifleValue(10), 5.0f, 89.0f, "%.0f deg", 1.0f);
                if (ImGui::Button("Turn-on points back to default")) {
                    for (int i = 8; i < 11; ++i) *ScopeRifleValue(i) = ScopeRifleDefault(i);
                    ss = true;
                }
                float now[3];
                const bool at = ScopeRifleAtEye(now);
                const float reachTo = (*ScopeRifleValue(5) + *ScopeRifleValue(6) + *ScopeRifleValue(8)) * 100;
                ImGui::TextDisabled("Now: eye %.1f cm behind the glass (on up to %.1f), %.1f cm off the middle (on under %.1f), %.0f deg off the barrel (on under %.0f): %s",
                                    now[0], reachTo, now[1], *ScopeRifleValue(9) * 100, now[2], *ScopeRifleValue(10), at ? "ON" : "off");
            }
            ImGui::Checkbox("Show the eyepiece glass without aiming (to line it up; not saved)", ScopeRifleShowGlass());
            ss |= FineSlider("Eyepiece right", ScopeRifleValue(0), -0.15f, 0.15f, "%.3f m", 0.002f);
            ss |= FineSlider("Eyepiece up", ScopeRifleValue(1), -0.10f, 0.25f, "%.3f m", 0.002f);
            ss |= FineSlider("Eyepiece forward", ScopeRifleValue(2), -0.40f, 0.40f, "%.3f m", 0.002f);
            ss |= FineSlider("Eyepiece glass size", ScopeRifleValue(3), 0.01f, 0.08f, "%.3f m", 0.001f);
            ss |= FineSlider("Eye box (how far off the middle before the shadow)", ScopeRifleValue(4), 0.003f, 0.10f, "%.3f m", 0.001f);
            ss |= FineSlider("Best eye distance behind the glass", ScopeRifleValue(5), 0.02f, 0.60f, "%.3f m", 0.002f);
            ss |= FineSlider("Eye distance leeway (either way)", ScopeRifleValue(6), 0.01f, 0.40f, "%.3f m", 0.002f);
            ss |= FineSlider("Picture size in the glass (1 = fills it at the best eye distance)", ScopeRifleValue(7), 0.3f, 3.0f, "%.2fx", 0.05f);
            ss |= FineSlider("Scope picture fade in (0 = at once)", ScopeRifleValue(11), 0.0f, 1.0f, "%.2f s", 0.01f);  // fade1
            ss |= FineSlider("Scope picture fade out (0 = at once)", ScopeRifleValue(12), 0.0f, 1.0f, "%.2f s", 0.01f);
            ImGui::TextDisabled("Hold the aim and look into the glass. The scope effects below (square, corners, CRT, distortion) fit the glass.");
        }
    }
    if (ScopeGun()[0]) {  // square1 (DrGonzaga 15 Nov): per gun
        ss |= ImGui::Checkbox(*ScopeRifle() ? "Square scope for this gun (square glass on the rifle; \"Rounded corners\" below)" : "Square scope for this gun (turns with the gun; \"Scope reticle turn\" squares it up)", ScopeSquare());
        float sc = *ScopeScan() * 100, ti = *ScopeTint() * 100;
        if (ImGui::SliderFloat("Scan lines in the scope (this gun)", &sc, 0, 100, "%.0f%%")) *ScopeScan() = sc / 100;
        ss |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::SliderFloat("Orange tint in the scope (this gun)", &ti, 0, 100, "%.0f%%")) *ScopeTint() = ti / 100;
        ss |= ImGui::IsItemDeactivatedAfterEdit();
        if (*ScopeSquare()) {  // square2
            float co = *ScopeCorner() * 100, cr = *ScopeCrt() * 100, fr = *ScopeFringe() * 100;
            if (ImGui::SliderFloat("Rounded corners (square scope)", &co, 0, 50, "%.0f%%")) *ScopeCorner() = co / 100;
            ss |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::SliderFloat("Old TV bulge (square scope; 0 = off)", &cr, 0, 100, "%.0f%%")) *ScopeCrt() = cr / 100;
            ss |= ImGui::IsItemDeactivatedAfterEdit();
            if (*ScopeCrt() > 0.001f) {
                if (ImGui::SliderFloat("Colour fringe towards the edges (with the TV bulge)", &fr, 0, 100, "%.0f%%")) *ScopeFringe() = fr / 100;
                ss |= ImGui::IsItemDeactivatedAfterEdit();
            }
        }
    }
    if (ImGui::Checkbox("Scope reticles in each gun's laser colour (all guns; black styles stay black)", ScopeMatchLaser()))
        SaveF(L"aim", L"ScopeReticleMatchLaser", *ScopeMatchLaser() ? 1.f : 0.f);  // match1
    ss |= ImGui::SliderFloat(*ScopeRifle() ? "Scope lens size (floating lens only; not used by the scope on the rifle)" : "Scope lens size", ScopeSize(), 5, 30, "%.0f deg");
    ss |= ImGui::SliderFloat("Scope extra zoom", ScopeZoom(), 1, 6, "%.1fx");  // scope10: max 4 -> 6
    if (ImGui::Checkbox("A zooms in, X zooms out while scoped (1, 1.5, 2, 3, 4, 6x)", InputScopeZoomButtons()))
        SaveF(L"aim", L"ScopeZoomButtons", *InputScopeZoomButtons() ? 1.f : 0.f);
    if (ImGui::SliderFloat("Scope steadiness", EngineScopeSmoothing(), 0, 5, "%.2f"))
        ss = true, SaveF(L"aim", L"ScopeSteadiness", *EngineScopeSmoothing());  // also the start for new guns
    ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Scope sticks when held still? Hold the right grip (keeps the controller tracked).");
    ImGui::SliderFloat("Extra steadiness when crouched", EngineScopeCrouchSteady(), 1, 4, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"aim", L"ScopeCrouchSteady", *EngineScopeCrouchSteady());
    ImGui::SameLine();
    ImGui::TextDisabled(EngineCrouched() ? "(crouched now)" : "(1x = same as standing)");
    if (ImGui::Checkbox("Classic scope smoothing (plain average, before 2 Nov; lags on moves)", EngineScopeClassic()))
        SaveF(L"aim", L"ScopeClassicSmoothing", *EngineScopeClassic() ? 1.f : 0.f);
    ss |= ImGui::SliderFloat("Scope recoil", ScopeRecoil(), 0, 6, "%.2f deg");
    ImGui::SameLine();
    ImGui::TextDisabled("(kick per shot; 0 = off)");
    ss |= ImGui::SliderFloat("Scope reticle right", &ScopeReticle()[0], -0.3f, 0.3f, "%.3f");
    ss |= ImGui::SliderFloat("Scope reticle up", &ScopeReticle()[1], -0.3f, 0.3f, "%.3f");
    ss |= ImGui::SliderFloat("Scope reticle opacity", ScopeReticleOpacity(), 0.1f, 1, "%.2f");
    if (ImGui::SliderFloat("Reticle glow (all guns)", ScopeReticleGlow(), 0, 1, "%.2f"))
        SaveF(L"aim", L"ScopeReticleGlow", *ScopeReticleGlow());
    ss |= FineSlider(*ScopeRifle() ? "Scope turn (rifle glass, square and reticle together)" : "Scope reticle turn", ScopeReticleTurn(), -180, 180, "%.1f deg", 0.5f);
    ImGui::SliderFloat("Lens distortion (all guns)", ScopeDistortion(), 0, 2, "%.2f");  // lens2: stronger, range to 2
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"aim", L"ScopeDistortion", *ScopeDistortion());
    if (ImGui::Checkbox("Hide the game's HUD inside the scope", ScopeHideHud()))  // nohud1
        SaveF(L"aim", L"ScopeHideHud", *ScopeHideHud() ? 1.f : 0.f);
    ImGui::SameLine();
    ImGui::TextDisabled("(level it at your normal hold)");
    if (ss) Later(ScopeSave);
    if (More("More beam options")) {
        if (ImGui::SliderFloat("Beam offset right", &LaserOffset()[0], -5, 5, "%.2f deg")) SaveF(L"aim", L"AimOffsetRight", LaserOffset()[0]);
        if (ImGui::SliderFloat("Beam offset up", &LaserOffset()[1], -5, 5, "%.2f deg")) SaveF(L"aim", L"AimOffsetUp", LaserOffset()[1]);
        ImGui::TextDisabled("Move the beam onto where the bullets actually land");
    }
}

void HudPage() {
    bool on = HudOn();
    if (ImGui::Checkbox("HUD on a floating panel", &on)) HudSetOn(on), Later(HudSave);
    ImGui::SameLine();
    bool shown = HudShown();
    if (ImGui::Checkbox("Show HUD", &shown)) HudSetShown(shown);
    ImGui::TextDisabled("Off = the game's own flat HUD at the edges of the view");
    bool ch = false;
    ch |= ImGui::SliderFloat("Distance", HudDistance(), 0.3f, 5, "%.2f m");
    ch |= ImGui::SliderFloat("Size (width)", HudSize(), 0.2f, 4, "%.2f m");
    ch |= ImGui::SliderFloat("Height", HudHeight(), -1, 1, "%.2f m");
    ch |= ImGui::SliderFloat("Opacity", HudOpacity(), 0.05f, 1, "%.2f");
    ch |= ImGui::Checkbox("Same HUD picture in both eyes (the left eye's; stops icons sitting differently in each eye)", HudOneForBothEyes());  // onehud1
    ch |= ImGui::Checkbox("Quest icon at its target's distance (lines up in both eyes; the rest of the HUD stays put)", HudMainFar());  // farquest1
    if (*HudMainFar()) {
        ch |= ImGui::SliderFloat("Quest icon patch width", HudQuestPatch(), 0.01f, 0.3f, "%.2f");
        ch |= ImGui::SliderFloat("Quest icon patch height", HudQuestPatch() + 1, 0.01f, 0.4f, "%.2f");
    }
    ch |= ImGui::Checkbox("Story videos on a 16:9 screen", &g_movieOn);  // movie1
    if (g_movieOn) {
        ch |= ImGui::Checkbox("The opening story comic too", &g_movieComic);  // movie3
        ImGui::SameLine();
        ch |= ImGui::Checkbox("black around it", HudMovieBlack());
        ch |= ImGui::SliderFloat("Video screen width", HudMovieSize(0), 0.5f, 8, "%.2f m");
        ch |= ImGui::SliderFloat("Video screen distance", HudMovieSize(1), 0.5f, 10, "%.2f m");
    }
    ImGui::TextDisabled("Bigger size or nearer = HUD fills more of your view");
    ImGui::Separator();
    for (int e = 0; e < kHudPanels; ++e) {  // map1 / ammo1 (DrGonzaga 3 Nov)
        static const char *heads[kHudPanels] = {"Minimap (with health) on its own panel", "Ammo on its own panel"};
        ImGui::PushID(e);
        if (!ImGui::CollapsingHeader(heads[e])) {
            HudPanelOutline(e, false);
            ImGui::PopID();
            continue;
        }
        HudPanelOutline(e, true);
        HudPanelCfg *p = HudPanel(e);
        ch |= ImGui::Checkbox("On its own panel", &p->own);
        static const char *anchors[5] = {"Follows my view", "Low in front (look down)", "On my left wrist", "On my right wrist", "With the chest spheres"};
        int a = p->anchor;
        if (ImGui::Combo("Where", &a, anchors, 5)) HudPanelDefaults(e, a), ch = true;
        bool wrist = a == 2 || a == 3, chest = a == 4;
        ch |= ImGui::SliderFloat("Right / left", &p->off[0], -0.6f, 0.6f, "%.2f m");
        ch |= ImGui::SliderFloat("Up / down", &p->off[1], -0.8f, 0.6f, "%.2f m");
        ch |= ImGui::SliderFloat(wrist ? "Towards elbow / hand" : chest ? "Ahead of the spheres" : "Distance", &p->off[2],
                                 wrist || chest ? -0.3f : 0.0f, wrist ? 0.3f : chest ? 0.6f : 1.5f, "%.2f m");
        if (chest) ImGui::TextDisabled("Turns with the chest spheres (holsters on); otherwise as 'Low in front'.");
        if (wrist) {  // wrist1 (DrGonzaga 12 Nov): fixed to the controller like a watch face
            ch |= ImGui::Checkbox("Fixed to the controller (turn your hand to read it)", &p->fixed);
            if (p->fixed) {
                ch |= ImGui::SliderFloat("Tilt (0 = faces back, 90 = faces up)", &p->tilt, -90, 90, "%.0f deg");
                ch |= ImGui::SliderFloat("Turn left / right", &p->turn, -90, 90, "%.0f deg");
                ch |= ImGui::Checkbox("Only show when I look at it", &p->glance);
                if (p->glance) {  // glance2 (DrGonzaga 20 Nov): wrist turn and look on their own sliders
                    ch |= ImGui::SliderFloat("Wrist turn needed (higher = easier)", &p->turnDeg, 10, 89, "within %.0f deg");
                    ch |= ImGui::SliderFloat("Look needed (higher = easier)", &p->glanceDeg, 10, 80, "within %.0f deg");
                }
            } else ImGui::TextDisabled("Off = turns to face you wherever your hand is.");
        }
        ch |= ImGui::SliderFloat("Size", &p->size, 0.05f, 1.0f, "%.2f m");
        ch |= ImGui::SliderFloat("Opacity", &p->opacity, 0.05f, 1, "%.2f");
        ch |= ImGui::Checkbox("Round", &p->round);
        ImGui::TextWrapped("Line up the yellow box on the main panel around it:");
        ch |= ImGui::SliderFloat("Box left edge", &p->rect[0], 0, 1, "%.3f");
        ch |= ImGui::SliderFloat("Box right edge", &p->rect[2], 0, 1, "%.3f");
        ch |= ImGui::SliderFloat("Box top edge", &p->rect[1], 0, 1, "%.3f");
        ch |= ImGui::SliderFloat("Box bottom edge", &p->rect[3], 0, 1, "%.3f");
        if (ImGui::Button("Reset this panel")) HudPanelReset(e), ch = true;
        ImGui::TextDisabled("Back on the main panel while paused; hidden while scoped.");
        if (e == 1 && g_bloodDragon) {  // ammo2 (DrGonzaga 15 Nov): there on a glance without pressing reload
            ImGui::Separator();
            ch |= ImGui::Checkbox("Keep the game's own ammo picture for the glance (no need to press reload to see it)", HudAmmoKeep());  // ammokeep1
            {
                static const char *const kSt[4] = {"off", "waiting for the game to show its ammo once (fire or reload)", "the game's own picture is showing", "the kept copy is showing"};
                ImGui::TextDisabled("Now: %s", kSt[HudAmmoKeepState() & 3]);
            }
            ch |= ImGui::Checkbox("Numbers drawn by the mod instead of the game's picture", AmmoOwn());
            if (*AmmoOwn()) {
                ch |= ImGui::Checkbox("Spare rounds under the rounds in the gun", AmmoSpare());
                ch |= ImGui::SliderFloat("Number size", AmmoSize(), 0.3f, 3.0f, "x%.2f");
                if (AmmoClipNow() >= 0) ImGui::TextDisabled("Read from the game now: %d in the gun, %d spare.", AmmoClipNow(), AmmoSpareNow());
                else ImGui::TextDisabled("No gun with rounds in hand: the game's own ammo picture shows.");
                ImGui::TextDisabled("They stand where this panel is and follow its glance settings.");
            }
        }
        ImGui::PopID();
    }
    {  // keep1 (DrGonzaga 10 Nov): leave game prompts in the minimap box on the main HUD
        ImGui::PushID("keepbox");
        if (ImGui::CollapsingHeader("Keep a part of the minimap box on the HUD (e.g. prompts)")) {
            HudKeepBoxOutline(true);
            HudHideBox *kb = HudKeepBoxCfg();
            ch |= ImGui::Checkbox("Keep what's inside the box on the main HUD", &kb->on);
            ImGui::TextWrapped("Line up the green box around the text (it stays off the minimap panel):");
            ch |= ImGui::SliderFloat("Box left edge", &kb->rect[0], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box right edge", &kb->rect[2], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box top edge", &kb->rect[1], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box bottom edge", &kb->rect[3], 0, 1, "%.3f");
        } else
            HudKeepBoxOutline(false);
        ImGui::PopID();
    }
    {  // hide1 (DrGonzaga 3 Nov): hide one small part, e.g. the crouch icon
        ImGui::PushID("hidebox");
        if (ImGui::CollapsingHeader("Hide one small part (e.g. the crouch icon)")) {
            HudHideBoxOutline(true);
            HudHideBox *hb = HudHideBoxCfg();
            ch |= ImGui::Checkbox("Hide what's inside the box", &hb->on);
            ImGui::TextWrapped("Line up the cyan box around it (works on the minimap / ammo panels too):");
            ch |= ImGui::SliderFloat("Box left edge", &hb->rect[0], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box right edge", &hb->rect[2], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box top edge", &hb->rect[1], 0, 1, "%.3f");
            ch |= ImGui::SliderFloat("Box bottom edge", &hb->rect[3], 0, 1, "%.3f");
        } else
            HudHideBoxOutline(false);
        ImGui::PopID();
    }
    {  // headmap1 (DrGonzaga 10 Nov)
        bool h = g_mapHead != 0;
        if (ImGui::Checkbox("Minimap turns with my head (not my aim)", &h)) g_mapHead = h, SaveF(L"hud", L"MinimapFollowsHead", h ? 1.f : 0.f);
        if (ImGui::Checkbox("Show the minimap (X switches it on foot)", HudMinimapShown())) ch = true;  // lasers1
    }
    ImGui::Separator();
    ImGui::Text("Icons in the world:");
    int tag = GfxVarFind("g_disableTagIcon");
    if (tag >= 0) {
        bool icons = !GfxVarBool(tag);
        if (ImGui::Checkbox("Enemy tags", &icons)) GfxVarSetBool(tag, !icons);
        ImGui::SameLine();
    }
    {  // mark1 (DrGonzaga 3 Nov)
        bool q = !g_hideObjIcon, m = !g_hideMarkIcon;
        if (ImGui::Checkbox("Quest (yellow, with arrow)", &q)) g_hideObjIcon = !q, SaveF(L"hud", L"HideQuestIcon", q ? 0.f : 1.f);
        ImGui::SameLine();
        if (ImGui::Checkbox("Map marker (square)", &m)) g_hideMarkIcon = !m, SaveF(L"hud", L"HideMapMarkerIcon", m ? 0.f : 1.f);
        ImGui::TextDisabled("Untick to hide. The world map and minimap still show them.");
        if (g_bloodDragon) {  // objhead1 (bd80)
            if (ImGui::Checkbox("Quest icon placed by where my head looks (not the controller)", &g_objHead)) SaveF(L"hud", L"QuestIconByHead", g_objHead ? 1.f : 0.f);
            if (ImGui::SliderFloat("Quest icon size", &g_objScale, 0.5f, 8.0f, "x%.1f")) SaveF(L"hud", L"QuestIconScale", g_objScale);
        }
        if (ImGui::Checkbox(g_bloodDragon ? "Hide chosen button prompts (zoom, hold breath)" : "Hide the arrow-switch prompts (and their strip)", PromptsHideOn()))  // prompts1
            SaveF(L"hud", L"HidePrompts", *PromptsHideOn() ? 1.f : 0.f), PromptsChanged();
        if (g_bloodDragon && ImGui::Checkbox("Prompts use the VR texts (bin\\bdvr-prompts.txt)", PromptsVrTextOn()))  // vrtext1
            SaveF(L"hud", L"VrPrompts", *PromptsVrTextOn() ? 1.f : 0.f), PromptsChanged();
        ImGui::TextDisabled("Other prompts (loot etc.) stay. More texts: fc3vr.ini [hud] HidePromptText=a;b");
    }
    if (More("More HUD options")) {
        ImGui::Text("Hide parts of the HUD (by where they are on the panel):");
        static const char *areas[9] = {"Top left", "Top middle", "Top right", "Left middle", "Centre (crosshair)",
                                       "Right middle", "Bottom left", "Bottom middle", "Bottom right"};
        int *hide = HudHidden();
        for (int i = 0; i < 9; ++i) {
            bool b = (*hide >> i) & 1;
            char id[40];
            snprintf(id, sizeof id, "%s##hide%d", areas[i], i);
            if (ImGui::Checkbox(id, &b)) *hide = b ? (*hide | (1 << i)) : (*hide & ~(1 << i)), ch = true;
            if (i % 3 != 2) ImGui::SameLine(i % 3 == 0 ? 240.f : 520.f);
        }
        ch |= ImGui::SliderInt("HUD starts at pass", HudFirstPass(), 1, 6);
        ImGui::TextDisabled("Leave at 1. Only change if the panel shows the world or the HUD is missing.");
        if (ImGui::Button("Reset HUD")) {
            *HudDistance() = 1.6f, *HudSize() = 1.4f, *HudHeight() = 0, *HudOpacity() = 0.9f, *HudFirstPass() = 1, *HudHidden() = 0;
            ch = true;
        }
    }
    if (ch) Later(HudSave);
}

void ButtonsPage() {
    ImGui::TextWrapped("What each Quest controller input sends to the game (as an Xbox pad button). "
                       "Example: to press B for the game's X, set B to X. Saved straight away.");
    ImGui::TextDisabled("Both stick clicks always open this menu; B always closes it.");
    if (ImGui::Checkbox("On foot: right grip just holds the gun, X does the grip's job", InputGripHoldPtr()))
        Later(InputMapSave);
    ImGui::TextDisabled("Hold the grip when keeping the gun still: the Quest can stop tracking an untouched, still controller.");
    {  // lasers1 (DrGonzaga 10 Nov)
        ImGui::Text("X on foot with a gun in hand:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Game map", *InputXMode() == 2)) *InputXMode() = 2, Later(InputMapSave);  // map1x
        ImGui::SameLine();
        if (ImGui::RadioButton("Minimap on / off", *InputXMode() == 1)) *InputXMode() = 1, Later(InputMapSave);
        ImGui::SameLine();
        if (ImGui::RadioButton("Throw (tap) / cook (hold)", *InputXMode() == 0)) *InputXMode() = 0, Later(InputMapSave);
        ImGui::TextDisabled("In cutscenes, button-mash scenes, vehicles and with no gun, X goes to the game as mapped below.");
        // wrist1m (DrGonzaga 13 Nov)
        if (ImGui::Checkbox("Menu button opens the full map; hold the left trigger (or left grip) and tap Menu for the pause menu", InputMenuMinimap())) Later(InputMapSave);
        ImGui::TextDisabled("While the game is paused, Menu goes to the game as mapped below. Minimap on / off is on the HUD page.");
        if (ImGui::Checkbox("Left grip opens the weapon wheel on foot", InputLeftGripWheel())) Later(InputMapSave);  // map1x
        ImGui::TextDisabled("Off = left grip only holds the gun with both hands (weapons from the holsters).");
        if (ImGui::Checkbox("In the game's menus and map, B is B and X is X (matches the pictures)", InputMenuNaturalBX())) Later(InputMapSave);  // menuxb1
        ImGui::Text("Walk where:");  // walkdir1
        ImGui::SameLine();
        if (ImGui::RadioButton("the gun aims", InputWalkDir(), 0)) Later(InputMapSave);
        ImGui::SameLine();
        if (ImGui::RadioButton("my left controller points", InputWalkDir(), 1)) Later(InputMapSave);
        ImGui::SameLine();
        if (ImGui::RadioButton("my head looks", InputWalkDir(), 2)) Later(InputMapSave);
    }
    ImGui::Separator();
    for (int i = 0; i < InputMapCount(); ++i) {
        int *m = InputMap(i);
        ImGui::SetNextItemWidth(360);
        char id[48];
        snprintf(id, sizeof id, "%s##map%d", InputMapName(i), i);
        if (ImGui::BeginCombo(id, InputOutputName(*m))) {
            for (int o = 0; o < InputOutputCount(); ++o)
                if (ImGui::Selectable(InputOutputName(o), *m == o)) *m = o, Later(InputMapSave);
            ImGui::EndCombo();
        }
    }
    if (ImGui::Button("Reset buttons")) InputMapDefaults(), Later(InputMapSave);
    ImGui::SliderFloat("Right trigger sensitivity", InputTriggerSens(), 1, 3, "%.2fx");  // lasers3
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"input", L"RightTriggerSensitivity", *InputTriggerSens());
    ImGui::TextDisabled("Higher = the gun fires with a lighter pull (1 = as the controller reports it)");
    if (More("Rumble")) {
        if (ImGui::SliderFloat("Rumble strength", InputRumbleStrengthPtr(), 0.f, 1.f, "%.2f"))
            SaveF(L"input", L"RumbleStrength", *InputRumbleStrengthPtr());
        if (ImGui::Checkbox("Rumble a real Xbox pad too", InputRumbleRealPadPtr()))
            SaveF(L"input", L"RumbleRealPad", *InputRumbleRealPadPtr() ? 1.f : 0.f);
    }
}

void GunBodyPage() {
    if (GunAttachName()[0])
        ImGui::Text("Gun position in the %s hand (%s, saved automatically per weapon)", GunAttachLeftValues() ? "LEFT" : "right", GunAttachName());
    else
        ImGui::Text("Gun position in the hand (no gun learnt yet)");
    float *o = GunAttachOffset();
    // bow3 (DrGonzaga 2 Nov): wider ranges (the bow at hip fire needed more), with
    // arrow buttons for fine steps. Saved values are untouched. range2 (DrGonzaga 3 Nov, grenade launcher): angles +-180, position +-0.8 m.
    bool gs = false;
    gs |= FineSlider("Right##g", &o[0], -0.80f, 0.80f, "%.4f m", 0.005f);
    gs |= FineSlider("Up##g", &o[1], -0.80f, 0.80f, "%.4f m", 0.005f);
    gs |= FineSlider("Forward##g", &o[2], -0.80f, 0.80f, "%.4f m", 0.005f);
    ImGui::Text("Gun angle in the hand");
    float *ang = GunAttachAngle();
    gs |= FineSlider("Pitch", &ang[0], -180, 180, "%.2f deg", 1);
    gs |= FineSlider("Yaw", &ang[1], -180, 180, "%.2f deg", 1);
    gs |= FineSlider("Roll", &ang[2], -180, 180, "%.2f deg", 1);
    float *c = GunAttachCamera();
    if (ImGui::Button("Re-learn gun")) GunAttachRelearn();
    ImGui::SameLine();
    ImGui::TextDisabled("(after an upgrade, or if the gun looks wrong)");
    if (ImGui::Button("Reset this gun")) {
        o[0] = o[1] = o[2] = 0;
        ang[0] = ang[1] = ang[2] = 0;
        c[0] = c[1] = c[2] = 0;
        Later(GunAttachSave);
    }
    if (More("Camera away from the body (floating gun)")) {
        ImGui::SliderFloat("Right##c", &c[0], -0.6f, 0.6f, "%.2f m");
        gs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Up##c", &c[1], -0.6f, 0.6f, "%.2f m");
        gs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Forward##c", &c[2], -0.6f, 0.8f, "%.2f m");
        gs |= ImGui::IsItemDeactivatedAfterEdit();
    }
    // c4hands1 (DrGonzaga 12 Nov): C4 charge in the left hand, detonator in the right, aimed from the left
    gs |= ImGui::Checkbox("C4: charge in my left hand, detonator in my right, aim the throw with my left hand", GunAttachC4SplitOpt());
    // c4turn1: once Blood Dragon's hold is learnt the charge has its own sliders on the hands section below
    if (*GunAttachC4SplitOpt() && !(g_bloodDragon && HandsC4Learnt()) && More("C4 charge position in the left hand")) {
        float *c4 = GunAttachC4Offset();
        gs |= FineSlider("Right##c4", &c4[0], -0.40f, 0.40f, "%.3f m", 0.005f);
        gs |= FineSlider("Up##c4", &c4[1], -0.40f, 0.40f, "%.3f m", 0.005f);
        gs |= FineSlider("Forward##c4", &c4[2], -0.40f, 0.40f, "%.3f m", 0.005f);
    }
    if (gs) Later(GunAttachSave);  // gunid1: saved on release, no button needed
    // holster (DrGonzaga 10 Nov): weapons on the body, one right grip press to swap.
    ImGui::Separator();
    bool hs = false;
    hs |= ImGui::Checkbox("Weapon holsters on the body (right grip at a spot swaps to its weapon)", HolsterOn());
    if (*HolsterOn()) {
        hs |= ImGui::Checkbox("Buzz when my hand reaches a spot", HolsterBuzzOn());
        hs |= ImGui::Checkbox("See-through spheres on the hips", HolsterSpheres());
        // wrist1m (DrGonzaga 13 Nov): throwables menu on the left wrist
        hs |= ImGui::Checkbox("Throwables menu on my left wrist (look at it; right hand on an icon + right grip)", HolsterWristOpt(0));
        if (*HolsterWristOpt(0)) {
            ImGui::Indent();
            ImGui::TextDisabled("Grenade / molotov: the type X throws (tap throws, hold cooks). C4 / mines: out into your hands.");
            if (More("Wrist menu settings")) {
                hs |= ImGui::Checkbox("Icons in a row (off = two by two)", HolsterWristOpt(1));
                hs |= ImGui::Checkbox("Show how many grenades / molotovs are left", HolsterWristOpt(2));
                if (*HolsterWristOpt(2)) {  // numstyle1
                    static const char *kSt[5] = {"Plain##wst", "Digital##wst", "Neon tube##wst", "Dot matrix##wst", "Green CRT##wst"};
                    ImGui::TextUnformatted("Number style:");
                    for (int i = 0; i < 5; ++i) {
                        ImGui::SameLine();
                        if (ImGui::RadioButton(kSt[i], *HolsterWristCountStyle() == i)) *HolsterWristCountStyle() = i, hs = true;
                    }
                    // numcol1 (DrGonzaga 17 Nov: "option for colour"): presets plus a picker
                    static const char *kCn[6] = {"Aqua##wcol", "Pink##wcol", "Amber##wcol", "Green##wcol", "Red##wcol", "White##wcol"};
                    static const float kCv[6][3] = {{0.1f, 1.0f, 0.82f}, {1.0f, 0.2f, 0.75f}, {1.0f, 0.68f, 0.18f}, {0.35f, 1.0f, 0.4f}, {1.0f, 0.15f, 0.12f}, {0.95f, 0.95f, 0.95f}};
                    float *col = HolsterWristCountColour();
                    ImGui::TextUnformatted("Number colour:");
                    for (int i = 0; i < 6; ++i) {
                        ImGui::SameLine();
                        if (ImGui::Button(kCn[i])) col[0] = kCv[i][0], col[1] = kCv[i][1], col[2] = kCv[i][2], hs = true;
                    }
                    ImGui::ColorEdit3("Own colour##wcol", col, ImGuiColorEditFlags_NoInputs);
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                    // numcol2: the panel behind and the unlit segments
                    static const char *kBn[5] = {"Black##wbg", "Dark grey##wbg", "Navy##wbg", "Purple##wbg", "Dark teal##wbg"};
                    static const float kBv[5][3] = {{0, 0, 0}, {0.1f, 0.1f, 0.11f}, {0.02f, 0.04f, 0.16f}, {0.14f, 0.02f, 0.18f}, {0.0f, 0.1f, 0.1f}};
                    float *bg = HolsterWristCountBg();
                    ImGui::TextUnformatted("Background:");
                    for (int i = 0; i < 5; ++i) {
                        ImGui::SameLine();
                        if (ImGui::Button(kBn[i])) bg[0] = kBv[i][0], bg[1] = kBv[i][1], bg[2] = kBv[i][2], hs = true;
                    }
                    ImGui::ColorEdit3("Own background colour##wbg", bg, ImGuiColorEditFlags_NoInputs);
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Background opacity (0 = none)##wbg", &bg[3], 0, 1, "%.2f");
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                    static const char *kUn[4] = {"Dark grey##wun", "Dim number colour##wun", "Same as background (hidden)##wun", "Light grey##wun"};
                    float *un = HolsterWristCountOff();
                    ImGui::TextUnformatted("Unlit segments:");
                    for (int i = 0; i < 4; ++i) {
                        ImGui::SameLine();
                        if (ImGui::Button(kUn[i])) {
                            for (int k = 0; k < 3; ++k)
                                un[k] = i == 0 ? (k == 2 ? 0.16f : 0.15f) : i == 1 ? col[k] * 0.22f : i == 2 ? bg[k] : 0.4f;
                            hs = true;
                        }
                    }
                    ImGui::ColorEdit3("Own unlit colour##wun", un, ImGuiColorEditFlags_NoInputs);
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                }
                if (*HolsterWristOpt(2)) ImGui::SliderFloat("Size of the numbers##wcount", HolsterWristCountSize(), 0.15f, 1.5f, "%.2f x"), hs |= ImGui::IsItemDeactivatedAfterEdit();  // smallnum1
                if (g_bloodDragon) {  // heart1
                    hs |= ImGui::Checkbox("Cyber heart on the wrist menu (right grip takes it; let go to throw, or let go on the menu to put it back)", HolsterHeartOn());
                    hs |= ImGui::Checkbox("The game's own heart in my hand (off = just the icon in my hand)", HolsterHeartModel());  // heart2
                    ImGui::Checkbox("Hold a cyber heart in my hand now, to line it up (put back when unticked or the menu closes)", HolsterHeartAdjust());  // heartadj1
                    if (*HolsterHeartAdjust())
                        ImGui::TextDisabled("Now holding: %s. Its position and angle are the \"Gun position in the hand\" sliders on this page.", GunAttachName());
                    static const char *const kHb[4] = {"D-pad left", "D-pad right", "D-pad up", "D-pad down"};
                    ImGui::TextUnformatted("With the icon: the game's cyber heart button:");
                    for (int i = 0; i < 4; ++i) {
                        ImGui::SameLine();
                        if (ImGui::RadioButton(kHb[i], *HolsterHeartBtn() == i)) *HolsterHeartBtn() = i, hs = true;
                    }
                }
                if (g_bloodDragon) {  // slowmo1 (DrGonzaga 15 Nov): slow motion clock
                    hs |= ImGui::Checkbox("Slow motion clock (grip on it: on / off; the controllers buzz as it starts and ends)", SlowmoOn());
                    if (*SlowmoOn()) {
                        ImGui::Indent();
                        // slowbtn1 (DrGonzaga 15 Nov): floating by the minimap, left grip
                        if (ImGui::RadioButton("Floating by the minimap (left grip)##slowmo", *SlowmoPlace() == 0)) *SlowmoPlace() = 0, hs = true;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("On the wrist menu (right grip)##slowmo", *SlowmoPlace() == 1)) *SlowmoPlace() = 1, hs = true;
                        if (*SlowmoPlace() == 0) {
                            static const char *const kB[5] = {"Right of the minimap##slowbtn", "Above it##slowbtn", "Towards me##slowbtn", "Clock size##slowbtn", "How close my hand must be##slowbtn"};
                            for (int i = 0; i < 5; ++i) {
                                float cm = *SlowmoButton(i) * 100;
                                if (ImGui::SliderFloat(kB[i], &cm, i < 3 ? -40.f : i == 3 ? 1.f : 4.f, i < 3 ? 40.f : i == 3 ? 10.f : 30.f, "%.1f cm")) *SlowmoButton(i) = cm / 100;
                                hs |= ImGui::IsItemDeactivatedAfterEdit();
                            }
                        }
                        {  // slowmo3 (DrGonzaga 15 Nov: "25 was too low, 50 was too fast"): a slider, 5 to 100 %
                            float pc = *SlowmoSpeed() * 100;
                            if (ImGui::SliderFloat("Game speed while on##slowmo", &pc, 5, 100, "%.0f%%")) *SlowmoSpeed() = pc / 100;
                            hs |= ImGui::IsItemDeactivatedAfterEdit();
                        }
                        ImGui::SliderFloat("Time active (0 = until I touch the clock again)##slowmo", SlowmoSeconds(), 0, 60, "%.0f s");
                        hs |= ImGui::IsItemDeactivatedAfterEdit();
                        hs |= ImGui::Checkbox("Show the seconds left under the clock##slowmo", SlowmoShowSeconds());  // slowglow1
                        ImGui::SliderFloat("Ease in / out##slowmo", SlowmoRamp(), 0, 1, "%.2f s");
                        hs |= ImGui::IsItemDeactivatedAfterEdit();
                        if (SlowmoFound() < 0) ImGui::TextDisabled("Not available: the game's time scale was not found (see the log).");
                        else if (SlowmoActive()) ImGui::TextDisabled("On now: game at %.0f%% speed", SlowmoNow() * 100);
                        if (ImGui::TreeNode("How slow motion looks, sounds and feels (try them out)")) {  // slowfx1 (DrGonzaga 16 Nov)
                            SlowFxCfg *f = SlowFx();
                            auto sl = [&](const char *label, float *v, float lo, float hi, const char *fmt) {
                                ImGui::SliderFloat(label, v, lo, hi, fmt);
                                hs |= ImGui::IsItemDeactivatedAfterEdit();
                            };
                            auto pc = [&](const char *label, float *v) {
                                float p = *v * 100;
                                if (ImGui::SliderFloat(label, &p, 0, 100, "%.0f%%")) *v = p / 100;
                                hs |= ImGui::IsItemDeactivatedAfterEdit();
                            };
                            ImGui::Checkbox("Preview now, without slow motion (not saved)##slowfx", &f->preview);
                            ImGui::Separator();
                            hs |= ImGui::Checkbox("Colour push##slowfx", &f->push);
                            if (f->push) {
                                pc("Push strength##slowfx", &f->pushStrength);
                                ImGui::ColorEdit3("Push colour##slowfx", f->pushCol, ImGuiColorEditFlags_NoInputs);
                                hs |= ImGui::IsItemDeactivatedAfterEdit();
                            }
                            hs |= ImGui::Checkbox("Edge vignette##slowfx", &f->vig);
                            if (f->vig) {
                                pc("Vignette strength##slowfx", &f->vigStrength);
                                pc("How far in it reaches##slowfx", &f->vigStart);
                                ImGui::SameLine();
                                ImGui::TextDisabled("(low = further in)");
                                pc("Throb with the heartbeat##slowfx", &f->vigThrob);
                                ImGui::ColorEdit3("Vignette colour##slowfx", f->vigCol, ImGuiColorEditFlags_NoInputs);
                                hs |= ImGui::IsItemDeactivatedAfterEdit();
                            }
                            hs |= ImGui::Checkbox("Scan lines##slowfx", &f->scan);
                            if (f->scan) {
                                pc("Scan line strength##slowfx", &f->scanStrength);
                                sl("Lines down the picture##slowfx", &f->scanLines, 60, 1000, "%.0f");
                            }
                            hs |= ImGui::Checkbox("Heartbeat on the controllers##slowfx", &f->heart);
                            if (f->heart) {
                                pc("Heartbeat strength##slowfx", &f->heartStrength);
                                sl("Beats a minute##slowfx", &f->heartBpm, 30, 180, "%.0f");
                            }
                            hs |= ImGui::Checkbox("Whoosh sound at the start and end##slowfx", &f->sound);
                            if (f->sound) {
                                pc("Whoosh volume##slowfx", &f->soundVol);
                                if (ImGui::Button("Play start##slowfx")) SlowFxTestSound(1);
                                ImGui::SameLine();
                                if (ImGui::Button("Play end##slowfx")) SlowFxTestSound(2);
                            }
                            hs |= ImGui::Checkbox("Slow the game's own sounds with it##slowsnd", SlowSndOn());  // slowsnd1 (DrGonzaga 16 Nov)
                            if (*SlowSndOn()) {
                                if (ImGui::RadioButton("Effects only (guns, impacts, steps; music and speech stay)##slowsnd", *SlowSndMode() == 0)) *SlowSndMode() = 0, hs = true;
                                if (ImGui::RadioButton("Everything##slowsnd", *SlowSndMode() == 1)) *SlowSndMode() = 1, hs = true;
                                pc("How much of the slowdown the sounds take##slowsnd", SlowSndAmount());
                                if (SlowSndState() < 2) ImGui::TextDisabled(SlowSndState() ? "Waiting for the game's first sound." : "Not available: the game's sound engine was not hooked (see the log).");
                                else ImGui::TextDisabled("%d of the game's sound voices in view.", SlowSndVoices());
                            }
                            hs |= ImGui::Checkbox("Clock glows while slow motion is on##slowfx", &f->glow);  // slowglow1
                            if (f->glow) sl("Glow strength##slowfx", &f->glowStrength, 0, 1, "%.2f");
                            hs |= ImGui::Checkbox("Countdown ring round the clock (drains with the time left)##slowfx", &f->ring);
                            sl("Fade in##slowfx", &f->fadeIn, 0, 3, "%.2f s");
                            sl("Fade out##slowfx", &f->fadeOut, 0, 3, "%.2f s");
                            ImGui::TextDisabled("The picture cues are left out while scoped and in the cyber-eye.");
                            ImGui::TreePop();
                        }
                        ImGui::Unindent();
                    }
                }
                auto wcm = [&](const char *label, int i, float lo, float hi, const char *fmt = "%.1f cm") {
                    float cm = *HolsterWristValue(i) * 100;
                    if (ImGui::SliderFloat(label, &cm, lo, hi, fmt)) *HolsterWristValue(i) = cm / 100;
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                };
                auto wf = [&](const char *label, int i, float lo, float hi, const char *fmt) {
                    ImGui::SliderFloat(label, HolsterWristValue(i), lo, hi, fmt);
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                };
                wcm("Icon size##w", 6, 1, 6);
                wcm("Spacing##w", 7, 2, 15);
                wf("Opacity##w", 8, 0.05f, 1.0f, "%.2f");
                wcm("Right of the controller##w", 0, -30, 30);
                wcm("Above the controller##w", 1, -40, 30);
                wcm("Behind the controller##w", 2, -30, 30);
                wf("Tilt##w", 3, -90, 90, "%.0f deg");
                wf("Turn##w", 4, -180, 180, "%.0f deg");
                wf("Shows within this angle of your gaze##w", 5, 10, 80, "%.0f deg");
                wf("Palm up: shows only with the palm within this angle of straight up (180 = any way)##w", 9, 15, 180, "%.0f deg");  // palmup1
                ImGui::TextDisabled("It stays visible while this menu is open, so you can place it.");
            }
            ImGui::Unindent();
        }
        hs |= ImGui::Checkbox("Throwables on the chest (grip one to take it)", HolsterIcons());
        // nade1 (DrGonzaga 11 Nov): hold, arm, throw
        if (*HolsterIcons()) {
            ImGui::Indent();
            hs |= ImGui::Checkbox("Grenade in my hand unarmed until I arm it (fuse starts on arming; a holster weapon puts it away)", HolsterNadeOpt(0));
            if (*HolsterNadeOpt(0)) {
                hs |= ImGui::Checkbox("Right trigger arms it", HolsterNadeOpt(1));
                hs |= ImGui::Checkbox("Pull the pin with my teeth (hand to mouth arms it)", HolsterNadeOpt(2));
                if (*HolsterNadeOpt(2)) {
                    float cm = *HolsterNadeValue(0) * 100;
                    if (ImGui::SliderFloat("Mouth reach", &cm, 4, 25, "%.0f cm")) *HolsterNadeValue(0) = cm / 100;
                    hs |= ImGui::IsItemDeactivatedAfterEdit();
                }
            }
            hs |= ImGui::Checkbox("Physical throw (let go mid-swing: it flies along the swing)", HolsterNadeOpt(3));
            if (*HolsterNadeOpt(3)) {
                ImGui::SliderFloat("Swing speed that counts as a throw", HolsterNadeValue(1), 0.5f, 5, "%.1f m/s");
                hs |= ImGui::IsItemDeactivatedAfterEdit();
                float ms = *HolsterNadeValue(2) * 1000;
                if (ImGui::SliderFloat("Time to turn the aim before it leaves", &ms, 0, 500, "%.0f ms")) *HolsterNadeValue(2) = ms / 1000;
                hs |= ImGui::IsItemDeactivatedAfterEdit();
            }
            static const char *gs[] = {"no grenade in hand", "grenade in hand (not armed)", "ARMED", "in hand, grip let go (not armed)"};
            int g = HolsterGrenadeState();
            ImGui::TextDisabled("Grenade now: %s", gs[g < 0 || g > 3 ? 0 : g]);
            ImGui::Unindent();
        }
        float nearD;
        int sp = HolsterSpot(&nearD);
        static const char *names[] = {"right hip", "left hip", "right shoulder", "left shoulder",
                                      "chest: grenade", "chest: C4", "chest: molotov", "chest: mine"};
        if (sp >= 0) ImGui::Text("Right hand now: at the %s", sp < 8 ? names[sp] : HolsterSpotName(sp));  // slowmo1: the wrist spots (11+) ran past names[]
        else ImGui::TextDisabled("Right hand now: %.0f cm from the nearest hip spot", nearD * 100);
        if (HolsterLast()[0]) ImGui::TextDisabled("Last: %s", HolsterLast());
        static const char *slots[] = {"Weapon slot 1", "Weapon slot 2", "Weapon slot 3", "Weapon slot 4"};
        for (int i = 0; i < 4; ++i) {
            char id[48];
            snprintf(id, sizeof id, "%s gives##hs%d", names[i], i);
            ImGui::SetNextItemWidth(220);
            hs |= ImGui::Combo(id, HolsterSlot(i), slots, 4);
        }
        // holster4 (DrGonzaga 10 Nov): sphere look and the main heights out in the open.
        auto cmSlider = [&](const char *lb, int i, float lo, float hi) {
            float cm = *HolsterValue(i) * 100;
            if (ImGui::SliderFloat(lb, &cm, lo, hi, "%.1f cm")) *HolsterValue(i) = cm / 100;
            hs |= ImGui::IsItemDeactivatedAfterEdit();
        };
        ImGui::SliderFloat("Sphere opacity", HolsterValue(8), 0.05f, 1.0f, "%.2f");
        hs |= ImGui::IsItemDeactivatedAfterEdit();
        cmSlider("Hip sphere size", 9, 1, 15);
        cmSlider("Hip grab area: extra reach out to the side", 11, 0, 25);  // belt1
        cmSlider("Chest sphere size", 10, 1.5f, 6);
        cmSlider("Hips below my eyes", 0, 30, 110);
        cmSlider("Chest spheres below my eyes", 6, 15, 70);
        if (More("More holster spot positions")) {
            cmSlider("Hips out to the side", 1, 5, 50);
            cmSlider("Hips forward", 2, -30, 40);
            cmSlider("Hip grab size (radius)", 3, 6, 35);
            cmSlider("Shoulder zone starts behind my eyes", 4, -10, 30);
            cmSlider("Shoulder zone reaches down to", 5, 10, 90);
            cmSlider("Chest spheres forward", 7, 0, 40);
        }
    }
    // hands1 (DrGonzaga 12 Nov): small see-through spheres for the hands, gone while a hand holds something.
    hs |= ImGui::Checkbox("Neon style: wire globes, rings and outline icons with clear centres", HolsterNeon());  // neon1
    hs |= ImGui::Checkbox("See-through spheres for my hands (hidden while a hand holds a weapon)", HolsterHands());
    if (*HolsterHands()) {
        float cm = *HolsterHandValue(0) * 100;
        if (ImGui::SliderFloat("Hand sphere size", &cm, 1, 8, "%.1f cm")) *HolsterHandValue(0) = cm / 100;
        hs |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Hand sphere opacity", HolsterHandValue(1), 0.05f, 1.0f, "%.2f");
        hs |= ImGui::IsItemDeactivatedAfterEdit();
    }
    if (hs) Later(HolsterSave);
    BodyTick();
    if (More("Hide my body (experimental)")) {
        if (ImGui::Checkbox("Hide the body parts found", BodyHideOn())) BodyHideChanged();
        ImGui::SameLine();
        ImGui::TextDisabled("(%d found)", BodySaved());
        int left;
        const char *st = BodyStatus(&left);
        int ph = BodyPhase();
        if (ph == 0) {
            if (ImGui::Button("Find a body part")) BodyStartHunt();
            ImGui::SameLine();
            ImGui::TextDisabled("look at one part of your body (arm, chest...), then press");
            if (BodySaved() && ImGui::Button("Forget found parts")) BodyClear();
        } else {
            ImGui::Text("%s", st);
            if (ph >= 2) {
                ImGui::Text("%d candidates left", left);
                if (ImGui::Button("Gone")) BodyAnswer(true);
                ImGui::SameLine();
                if (ImGui::Button("Still there")) BodyAnswer(false);
                ImGui::SameLine();
            }
            if (ImGui::Button("Stop")) BodyCancel();
        }
        if (g_bloodDragon && BodySaved() && ph == 0) {  // hands1 / hands2
            ImGui::Separator();
            ImGui::TextWrapped("VR hands (test): tick the parts that are your hands and arms. Ticked parts are drawn on the gun, and only while a gun is in your hand.");
            for (int i = 0; i < BodySaved(); ++i) {
                char lb[24];
                snprintf(lb, sizeof lb, "Part %d", i + 1);
                if (i % 4) ImGui::SameLine();
                if (ImGui::Checkbox(lb, BodyShow(i))) BodyShowChanged();
            }
            bool ch = ImGui::Checkbox("Hands on the gun", HandsOn());
            ImGui::SameLine();
            ImGui::TextDisabled(!*HandsOn() ? "(off: ticked parts show where the game puts them)" : HandsLive() ? "(on the gun now)" : "(waiting for a gun in your hand)");
            ch |= ImGui::Checkbox("Left hand too", HandsLeft());
            if (*HandsLeft()) {  // hands3
                ch |= ImGui::Checkbox("Left hand follows my controller until both grips hold the gun", HandsFreeLeft());
            }
            for (int set = 0; set < 2; ++set) {  // hands5: the guns' free left hand, and the bow's arrow hand, each with its own fit
                if (set == 0 && !*HandsLeft()) continue;
                ImGui::PushID(set);
                ImGui::TextDisabled(set ? "Arrow hand with the bow (right hand):" : "Free left hand with guns:");
                float *fr = set ? HandsFreeBow() : HandsFree(), cm[3] = {fr[0] * 100, fr[1] * 100, fr[2] * 100};
                static const char *const kN[3] = {"Sideways", "Up", "Forward"};
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::SliderFloat(kN[i], &cm[i], -20, 20, "%.1f cm")) fr[i] = cm[i] / 100;
                    ch |= ImGui::IsItemDeactivatedAfterEdit();
                }
                ImGui::SliderFloat("Tilt up/down", &fr[3], -90, 90, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Turn in/out", &fr[4], -90, 90, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Roll", &fr[5], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::PopID();
            }
            {   // hands6
                float ms = *HandsSmooth() * 1000, bl = *HandsBlend() * 1000;
                if (ImGui::SliderFloat("Free hand smoothing", &ms, 0, 150, "%.0f ms")) *HandsSmooth() = ms / 1000;
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::SliderFloat("Blend between free hand and grip", &bl, 0, 600, "%.0f ms")) *HandsBlend() = bl / 1000;
                ch |= ImGui::IsItemDeactivatedAfterEdit();
            }
            ch |= ImGui::Checkbox("Hide the left hand while zoomed in a scope", HandsScopeHide());  // hands4
            ch |= ImGui::Checkbox("Free hand: forearm points to a natural elbow", HandsElbow());
            ch |= ImGui::Checkbox("Forearms point to a natural elbow on guns and the bow hand too", HandsElbowGun());  // hands11
            ImGui::SliderFloat("Elbows: how far out to the side", HandsElbowOut(), -0.5f, 2, "%.2f");
            ch |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("Wrist bend limit (lower = straighter wrists, elbows follow the hand more)", HandsWristBend(), 10, 120, "%.0f deg");  // hands15
            ch |= ImGui::IsItemDeactivatedAfterEdit();
            {   // hands12: the drawing elbow's target point, offsets in cm from "cheek level, behind the shoulder"
                static const char *const kL[3] = {"Bow: green line's far end higher / lower", "Bow: green line's far end further out / in", "Bow: green line's far end further back / forward"};
                for (int i = 0; i < 3; ++i) {
                    float cm = HandsBowElbow()[i] * 100;
                    if (ImGui::SliderFloat(kL[i], &cm, -60, 60, "%.0f cm")) HandsBowElbow()[i] = cm / 100;
                    ch |= ImGui::IsItemDeactivatedAfterEdit();
                }
            }
            ch |= ImGui::Checkbox("Hide the shoulder leftovers", HandsChest());
            if (*HandsLeft() && More("C4: left hand on the charge")) {  // hands24
                ImGui::PushID("c4hand");
                ImGui::TextDisabled("Take the C4 out and move these until the hand sits right. The directions are your left controller's.");
                float *c4 = HandsC4Adj(), cm[3] = {c4[0] * 100, c4[1] * 100, c4[2] * 100};
                static const char *const kN[3] = {"Sideways", "Up", "Forward"};
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::SliderFloat(kN[i], &cm[i], -100, 100, "%.1f cm")) c4[i] = cm[i] / 100;
                    ch |= ImGui::IsItemDeactivatedAfterEdit();
                }
                ImGui::SliderFloat("Tilt up/down", &c4[3], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Turn in/out", &c4[4], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Roll", &c4[5], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::Button("Back to zero")) memset(c4, 0, 6 * sizeof(float)), ch = true;
                ImGui::TextUnformatted("The charge itself, on the hand:");  // c4turn1
                {
                    float *co = HandsC4Obj(), cmo[3] = {co[0] * 100, co[1] * 100, co[2] * 100};
                    static const char *const kL[6] = {"Charge right", "Charge up", "Charge forward", "Charge tilt up/down", "Charge turn in/out", "Charge roll"};
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::SliderFloat(kL[i], &cmo[i], -100, 100, "%.1f cm")) co[i] = cmo[i] / 100;
                        ch |= ImGui::IsItemDeactivatedAfterEdit();
                    }
                    for (int i = 3; i < 6; ++i) {
                        ImGui::SliderFloat(kL[i], &co[i], -180, 180, "%.0f deg");
                        ch |= ImGui::IsItemDeactivatedAfterEdit();
                    }
                    if (ImGui::Button("Charge back to zero")) memset(co, 0, 6 * sizeof(float)), ch = true;
                    ImGui::TextDisabled("Hold Ctrl and click a slider to type a number.");
                }
                ImGui::TextDisabled("Charge on the hand: %s", HandsC4RelearnDue() ? "will be learnt again the next time the C4 comes out" : HandsC4Learnt() ? "fixed to the hand (learnt from the game's own hold)" : "not learnt yet: take the C4 out");  // c4rigid1, c4learn2
                if (ImGui::Button("Learn the charge's place on the hand again (then put the C4 away and take it out)")) HandsC4Relearn();
                ImGui::TextDisabled("Fingers: %s", HandsPose(0, 0) ? "a captured pose is saved" : "the game's own C4 pose");
                if (ImGui::Button("Capture the left hand's fingers as they are now, for the C4")) HandsPose(0, 1);
                if (HandsPoseLeft(0) > 0) {
                    ImGui::Text("Capturing in %.0f s: close this menu and hold the grip you want.", HandsPoseLeft(0) + 0.5f);
                    if (ImGui::Button("Cancel the timer")) HandsPoseIn(0, 0);
                } else if (ImGui::Button("Capture in 6 seconds (close the menu and hold the grip; both controllers buzz when it is taken)")) HandsPoseIn(0, 6);
                if (HandsPose(0, 0)) {
                    ch |= ImGui::Checkbox("Use the captured fingers on the C4", HandsPoseUse(0));
                    ImGui::SameLine();
                    if (ImGui::Button("Forget them")) HandsPose(0, 2);
                }
                ImGui::TextDisabled("To capture: hold any weapon whose left hand looks right (open this menu with it out), then press the button.");
                ImGui::PopID();
            }
            if (*HandsLeft() && More("Pistol: left hand on the pistol")) {  // hands24
                ImGui::PushID("pistolhand");
                ch |= ImGui::Checkbox("Hands together (pistol steadied): the left hand goes onto the pistol by itself", HandsPistolAuto());  // hands25
                ch |= ImGui::Checkbox("Left grip with my left hand near the pistol: the hand holds the pistol", HandsPistolGrip());
                ImGui::TextDisabled("Now: %s", HandsPistolLocked() ? "left hand locked to the pistol" : "left hand free");
                float rc = *HandsPistolReach() * 100;
                if (ImGui::SliderFloat("How near (wrist to wrist)", &rc, 8, 45, "%.0f cm")) *HandsPistolReach() = rc / 100;
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TextDisabled("Place: %s", HandsPistolPlace(0) ? "a saved place (the hand snaps there)" : "wherever the hand is when you press the grip");
                if (ImGui::Button("Save where my left hand is now as its place on the pistol")) HandsPistolPlace(1);
                if (HandsPistolPlace(0)) {
                    ImGui::SameLine();
                    if (ImGui::Button("Forget the place")) HandsPistolPlace(2);
                }
                ImGui::TextDisabled("Fingers: %s", HandsPose(1, 0) ? "a captured pose is saved" : "the game's own (open hand)");
                if (ImGui::Button("Capture the left hand's fingers as they are now, for the pistol")) HandsPose(1, 1);
                if (HandsPoseLeft(1) > 0) {
                    ImGui::Text("Capturing in %.0f s: close this menu and hold the grip you want.", HandsPoseLeft(1) + 0.5f);
                    if (ImGui::Button("Cancel the timer")) HandsPoseIn(1, 0);
                } else if (ImGui::Button("Capture in 6 seconds (close the menu and hold the grip; both controllers buzz when it is taken)")) HandsPoseIn(1, 6);
                if (HandsPose(1, 0)) {
                    ch |= ImGui::Checkbox("Use the captured fingers on the pistol", HandsPoseUse(1));
                    ImGui::SameLine();
                    if (ImGui::Button("Forget them")) HandsPose(1, 2);
                }
                ImGui::PopID();
            }
            if (*HandsLeft() && More("Two-handed guns: where the left hand holds this gun")) {  // gripfwd1
                ImGui::PushID("gunleft");
                const char *g = HandsGunLeftGun();
                if (!g[0]) ImGui::TextDisabled("Take a gun out first: each gun keeps its own place.");
                else {
                    ImGui::Text("Gun: %s", g);
                    ImGui::TextDisabled("Hold the gun with both grips and move these until the left hand sits on the front handle.");
                    ImGui::TextDisabled("Directions are the gun's own: forward = towards the muzzle.");
                    float *gl = HandsGunLeft(), cm[3] = {gl[0] * 100, gl[1] * 100, gl[2] * 100};
                    static const char *const kN[3] = {"Left hand right / left", "Left hand up / down", "Left hand forward / back"};
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::SliderFloat(kN[i], &cm[i], -60, 60, "%.1f cm")) gl[i] = cm[i] / 100, HandsGunLeftTouched();
                        ch |= ImGui::IsItemDeactivatedAfterEdit();
                    }
                    static const char *const kT[3] = {"Left hand tilt up/down", "Left hand turn in/out", "Left hand roll"};
                    for (int i = 3; i < 6; ++i) {
                        if (ImGui::SliderFloat(kT[i - 3], &gl[i], -180, 180, "%.0f deg")) HandsGunLeftTouched();
                        ch |= ImGui::IsItemDeactivatedAfterEdit();
                    }
                    if (ImGui::Button("Back to this gun's default")) HandsGunLeftReset(), ch = true;
                    ImGui::TextDisabled("Hold Ctrl and click a slider to type a number.");
                    ImGui::TextDisabled("Now: %s", HandsGunLeftOnGun() ? "left hand on the gun" : "left hand not on the gun (hold both grips)");
                    if (HandsGunLeftOnGun()) {
                        const float *a = HandsGunLeftSeen(0), *b = HandsGunLeftSeen(1);
                        ImGui::TextDisabled("Game's hand: %.0f right, %.0f up, %.0f forward cm. Your left controller: %.0f, %.0f, %.0f cm.", a[0], a[1], a[2], b[0], b[1], b[2]);
                    }
                    if (HandsGunLeftLeft() > 0) {
                        ImGui::Text("Taking it in %.0f s: close this menu, hold both grips with your left hand on the front handle.", HandsGunLeftLeft() + 0.5f);
                        if (ImGui::Button("Cancel the timer")) HandsGunLeftIn(0);
                    } else if (ImGui::Button("Take the place from my left hand in 6 seconds (close the menu, hold both grips; both controllers buzz)")) HandsGunLeftIn(6);
                }
                ImGui::PopID();
            }
            if (*HandsLeft() && More("Zipline: left hand on the line")) {  // zip1
                ImGui::PushID("ziphand");
                ch |= ImGui::Checkbox("On a zipline my left hand hangs from the line (placed from my head, not the controller)", HandsZipOn());
                ImGui::Checkbox("Show the zipline hand now, to set it up (not saved; untick when done)", HandsZipPreview());
                ImGui::TextDisabled("Now: %s", HandsZipNow() ? "zipline hand shown" : "not shown");
                ImGui::TextDisabled("Where the hand is, from your head. Sideways is to your right (minus = left).");
                float *z = HandsZip(), zc[3] = {z[0] * 100, z[1] * 100, z[2] * 100};
                static const char *const kZ[3] = {"Hand sideways", "Hand up", "Hand forward"};
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::SliderFloat(kZ[i], &zc[i], -100, 100, "%.1f cm")) z[i] = zc[i] / 100;
                    ch |= ImGui::IsItemDeactivatedAfterEdit();
                }
                ImGui::SliderFloat("Hand tilt up/down", &z[3], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Hand turn in/out", &z[4], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SliderFloat("Hand roll", &z[5], -180, 180, "%.0f deg");
                ch |= ImGui::IsItemDeactivatedAfterEdit();
                if (ImGui::Button("Back to the built-in place")) z[0] = -0.20f, z[1] = 0.25f, z[2] = 0.10f, z[3] = 80, z[4] = z[5] = 0, ch = true;
                ImGui::TextDisabled("Fingers: %s", HandsPose(3, 0) ? "a captured pose is saved" : "none captured (the neutral hand is used)");
                if (ImGui::Button("Capture the left hand's fingers as they are now, for the zipline")) HandsPose(3, 1);
                if (HandsPoseLeft(3) > 0) {
                    ImGui::Text("Capturing in %.0f s: close this menu and hold the grip you want.", HandsPoseLeft(3) + 0.5f);
                    if (ImGui::Button("Cancel the timer")) HandsPoseIn(3, 0);
                } else if (ImGui::Button("Capture in 6 seconds (close the menu and hold the grip; both controllers buzz when it is taken)")) HandsPoseIn(3, 6);
                if (HandsPose(3, 0)) {
                    ch |= ImGui::Checkbox("Use the captured fingers on the zipline", HandsPoseUse(3));
                    ImGui::SameLine();
                    if (ImGui::Button("Forget them")) HandsPose(3, 2);
                }
                ImGui::TextDisabled("To capture: untick the two boxes above, hold a weapon whose left hand has a closed grip, press the button, then tick them again.");
                ch |= ImGui::Checkbox("Leave the zipline's hook where the game puts it (on the line), not on my controller", HandsZipHookGame());
                ImGui::PopID();
            }
            if (*HandsLeft() && More("Free left hand: neutral pose")) {  // hands25
                ImGui::PushID("neutralhand");
                ch |= ImGui::Checkbox("Neutral left hand when it is not on a gun (same fingers whatever gun is out)", HandsNeutral());
                ch |= ImGui::Checkbox("Let the game's finger signs through (devil horns etc.)", HandsGameSigns());  // sign1
                ch |= ImGui::Checkbox("Show the game's own arms when no weapon is in the hand (patting out fire, ladders)", HandsGameArmsEmpty());  // emptyarms1
                ch |= ImGui::Checkbox("Opening section (no weapon yet): my hands follow the controllers", HandsEmptyTracked());  // emptyhands1
                if (*HandsEmptyTracked()) {  // emptyhands5
                    ch |= ImGui::Checkbox("...and whenever no weapon is in the hand later in the game", HandsEmptyAnywhere());
                    if (*HandsEmptyAnywhere()) {
                        ch |= ImGui::Checkbox("Show the game's own arm animations then (patting out fire)", HandsEmptyGameAnims());
                        if (*HandsEmptyGameAnims()) {
                            float cm = *HandsEmptyGameAnimCm() * 100;
                            if (ImGui::SliderFloat("Arm movement that counts as an animation (cm)", &cm, 3, 60, "%.0f")) *HandsEmptyGameAnimCm() = cm * 0.01f, ch = true;
                        }
                    }
                }
                ch |= ImGui::Checkbox("Killstar: right hand takes the game's grip on the star", HandsKillstarGrip());  // killstar1
                ImGui::TextDisabled("Pose: %s", HandsPose(2, 0) ? "saved" : "not learnt yet: take a pistol out for a second with the left hand free");
                if (ImGui::Button("Capture the left hand's fingers as they are now, as the neutral pose")) HandsPose(2, 1);
                if (HandsPoseLeft(2) > 0) {
                    ImGui::Text("Capturing in %.0f s: close this menu and hold the grip you want.", HandsPoseLeft(2) + 0.5f);
                    if (ImGui::Button("Cancel the timer")) HandsPoseIn(2, 0);
                } else if (ImGui::Button("Capture in 6 seconds (close the menu and hold the grip; both controllers buzz when it is taken)")) HandsPoseIn(2, 6);
                if (HandsPose(2, 0)) {
                    ImGui::SameLine();
                    if (ImGui::Button("Forget it (learn again from the pistol)")) HandsPose(2, 2);
                }
                ImGui::TextDisabled("To capture your own: switch this off, hold a weapon whose left hand looks right, open this menu, press the button, switch it on again.");
                ImGui::PopID();
            }
            if (HandsTakeDirty()) ch = true;
            if (ch) Later(HandsSave);
        }
    }
}

// twohand1 (DrGonzaga 10 Nov): "Two handed mode" page: two-handed holding, and swing melee (moved from Aim).
void TwoHandPage() {
    ImGui::TextDisabled("Now: %s", TwoHandHeld() ? "holding the gun with both hands" : TwoHandSteady() ? "pistol steadied (hands together)" : "one hand");
    if (ImGui::Checkbox("Two-handed holding (left grip on the front of the gun)", TwoHandOn()))
        SaveF(L"twohand", L"On", *TwoHandOn() ? 1.f : 0.f);
    ImGui::TextDisabled("Put your left hand in front of your right, along the gun, and squeeze the left grip.\n"
                        "Right hand: where the gun is and its roll. The line between your hands: where it points.\n"
                        "That grip press doesn't open the weapon wheel; grip anywhere else still does.");
    float cm = *TwoHandReach() * 100;
    if (ImGui::SliderFloat("Grab reach (to the side of the barrel)", &cm, 5, 30, "%.0f cm")) *TwoHandReach() = cm / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"twohand", L"Reach", *TwoHandReach());
    if (TwoHandSmoothGun()[0]) {  // grabreach1
        float *gr = TwoHandGunReachSetting(), gcm = TwoHandGunReach() * 100;
        char lb[96];
        snprintf(lb, sizeof lb, "Grab reach for %s%s", TwoHandSmoothGun(), *gr >= 0 ? "" : " (default)");
        if (ImGui::SliderFloat(lb, &gcm, 5, 60, "%.0f cm")) *gr = gcm / 100;
        if (ImGui::IsItemDeactivatedAfterEdit()) Later(TwoHandSmoothSave);
        if (*gr >= 0) {
            ImGui::SameLine();
            if (ImGui::Button("Default##grabreach")) *gr = -1, Later(TwoHandSmoothSave);
        }
    }
    float furthest = *TwoHandFar() * 100;
    if (ImGui::SliderFloat("Furthest grab in front of the right hand", &furthest, 30, 100, "%.0f cm")) *TwoHandFar() = furthest / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"twohand", L"Furthest", *TwoHandFar());
    if (ImGui::Checkbox("Gunstock: right controller aims (steadier)", TwoHandStockAim()))  // stockaim1
        SaveF(L"twohand", L"StockAim", *TwoHandStockAim() ? 1.f : 0.f);
    ImGui::TextDisabled("On: the gun points where the right controller points, as one-handed; the left grip still\n"
                        "counts as two-handed (best spread, kick, smoothing). Off: the line between your hands aims\n"
                        "(for no stock; it can wander a little, since controller positions are less steady than their turn).");
    ImGui::Separator();
    float cup = *TwoHandCupReach() * 100;  // pistolgrip1
    if (ImGui::SliderFloat("Hands together (pistol grip) within", &cup, 0, 25, cup < 0.5f ? "off" : "%.0f cm")) *TwoHandCupReach() = cup / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"twohand", L"HandsTogether", *TwoHandCupReach());
    ImGui::TextDisabled("Left grip with the controllers this close: the gun points as before (right hand),\n"
                        "but it counts as two hands: smoothing below, best spread, close recoil.");
    ImGui::Separator();
    // steady1 (DrGonzaga 12 Nov): pistols steady with the hands together, no grip
    if (ImGui::Checkbox("Steady pistols with the hands together (no grip)", PistolSteadyOn()))
        SaveF(L"twohand", L"PistolSteady", *PistolSteadyOn() ? 1.f : 0.f);
    if (*PistolSteadyOn()) {
        if (ImGui::Checkbox("A tick in both controllers when the pistol becomes steadied", PistolSteadyBuzz()))  // steadybuzz1
            SaveF(L"twohand", L"SteadyBuzz", *PistolSteadyBuzz() ? 1.f : 0.f);
        if (ImGui::SliderFloat("Pistol steadiness (lower = steadier)", PistolSteadyHz(), 0.2f, 5, "%.1f Hz"))
            ;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"twohand", L"PistolSteadyHz", *PistolSteadyHz());
        if (ImGui::SliderFloat("Pistol swing response (higher = less lag)", PistolSwing(), 0, 10, "%.1f"))
            ;
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"twohand", L"PistolSwing", *PistolSwing());
        if (TwoHandSmoothGun()[0]) {
            int *own = PistolGunOwn();
            bool is = *own >= 0 ? *own != 0 : PistolGunByName(TwoHandSmoothGun());
            char lb[96];
            snprintf(lb, sizeof lb, "%s counts as a pistol%s", TwoHandSmoothGun(), *own >= 0 ? "" : " (by name)");
            if (ImGui::Checkbox(lb, &is)) *own = is ? 1 : 0, Later(TwoHandSmoothSave);
        }
        ImGui::TextDisabled("Bring the left controller within the 'hands together' distance above: the pistol's\n"
                            "jitter is filtered (strong when still, less as you swing); where it points doesn't change.\n"
                            "Also best spread and close recoil. The left grip stays free (weapon wheel, holsters).");
    }
    ImGui::Separator();
    // twosmooth1: extra smoothing while both grips hold (two-handed gun, or the bow while drawing)
    if (ImGui::SliderFloat("Two-handed smoothing (all weapons)", TwoHandSmoothing(), 0, 1, "%.2f"))
        SaveF(L"twohand", L"Smoothing", *TwoHandSmoothing());
    ImGui::TextDisabled("While both grips hold the weapon (also the bow while drawing). 0 = off; higher = steadier, a little lag.");
    if (TwoHandSmoothGun()[0]) {
        float *gv = TwoHandSmoothGunValue();
        bool own = *gv >= 0;
        char lb[96];
        snprintf(lb, sizeof lb, "Own setting for %s", TwoHandSmoothGun());
        if (ImGui::Checkbox(lb, &own)) *gv = own ? *TwoHandSmoothing() : -1, Later(TwoHandSmoothSave);
        if (own) {
            ImGui::SliderFloat("Two-handed smoothing, this weapon", gv, 0, 1, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) Later(TwoHandSmoothSave);
        }
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Swing right hand to melee", InputSwingOn())) SaveF(L"aim", L"SwingMelee", *InputSwingOn() ? 1.f : 0.f);
    if (ImGui::SliderFloat("Swing speed needed", InputSwingSpeed(), 1.0f, 5.0f, "%.1f m/s")) SaveF(L"aim", L"SwingSpeed", *InputSwingSpeed());
    ImGui::TextDisabled("Lower = easier to trigger; raise it if aiming sets it off");
}

// bow6p (DrGonzaga 10 Nov): "Bow & arrow" page: everything for the physical bow together, with a
// preview (bow in the left hand, arrow in the right while this menu is open) to line them up.
void BowPage() {
    static const char *const kSt[] = {"no arrow", "arrow in hand", "nocked"};
    int st = BowState();
    ImGui::TextDisabled("Bow now: %s", st < 0 ? "not in use" : st == 2 ? (BowDraw() > 0.06f ? "drawing" : "nocked") : kSt[st]);
    if (ImGui::Checkbox("Motion controlled bow (needs two-handed holding on, Two handed mode page)", BowPhysicalOn()))
        SaveF(L"twohand", L"PhysicalBow", *BowPhysicalOn() ? 1.f : 0.f);
    ImGui::TextDisabled("Off = the bow as before: in the right hand, right trigger draws and shoots (faster).\n"
                        "Right stick up picks the arrow type either way.");
    ImGui::TextDisabled("Left grip at the bow's spot takes the bow into the left hand (hold the grip). Right grip at the\n"
                        "same spot takes an arrow; bring it to the bow to nock it; pull back with the grip held (the draw\n"
                        "follows your hands); let go of the right grip to shoot.");
    // string10: the string and the nocked arrow sit at the right hand
    if (ImGui::Checkbox("String and arrow follow the right hand (1:1)", BowStringOn())) SaveF(L"twohand", L"BowString", *BowStringOn() ? 1.f : 0.f);
    ImGui::TextDisabled("String: %s", BowStringStatus());
    if (ImGui::Button("Learn the string again")) BowStringRelearn();
    ImGui::SameLine();
    ImGui::TextDisabled("(learnt once from the game's own draw: nock, draw and hold 2 s)");
    // quiver5: sight zeroing (DrGonzaga 11 Nov: arrows land high and right of the dot)
    if (FineSlider("Arrows hit higher (+) / lower (-)", BowZeroUp(), -10, 10, "%.1f deg", 0.5f)) SaveF(L"twohand", L"BowZeroUp", *BowZeroUp());
    if (FineSlider("Arrows hit right (+) / left (-)", BowZeroRight(), -10, 10, "%.1f deg", 0.5f)) SaveF(L"twohand", L"BowZeroRight", *BowZeroRight());
    ImGui::TextDisabled("Zeroing, like a real sight: if arrows land high and right of the dot, move both sliders below zero.");
    // quiver7: the game's aim is pulled onto the bow harder while an arrow is drawn
    if (FineSlider("Game aim catch-up while drawn", BowSteerBoostPtr(), 1, 4, "x%.1f", 0.5f)) SaveF(L"twohand", L"BowSteerBoost", *BowSteerBoostPtr());
    ImGui::TextDisabled("How hard the game's own aim (which the arrow flies along) is pulled onto the bow. Higher = less lag, but it may wobble.");
    if (ImGui::Checkbox("No random arrow spread at full draw (the game adds a little by default)", BowNoSpread()))
        SaveF(L"twohand", L"BowNoSpread", *BowNoSpread() ? 1.f : 0.f);
    ImGui::Separator();
    {   // bowfeel1 (DrGonzaga 12 Nov)
        ImGui::Text("Handling");
        float ss = *BowSteerStart() * 100, sr = *BowSteerRange() * 100, sw = *BowSteerWeight() * 100;
        if (FineSlider("Arrow hand starts to steer after pulling", &ss, 0, 30, "%.0f cm", 1)) SaveF(L"twohand", L"BowSteerStart", ss / 100);
        *BowSteerStart() = ss / 100;
        if (FineSlider("...and has its full say after a further", &sr, 1, 40, "%.0f cm", 1)) SaveF(L"twohand", L"BowSteerRange", sr / 100);
        *BowSteerRange() = sr / 100;
        if (FineSlider("How much the arrow hand steers the bow", &sw, 0, 100, "%.0f%%", 5)) SaveF(L"twohand", L"BowSteerWeight", sw / 100);
        *BowSteerWeight() = sw / 100;
        if (FineSlider("After a shot the aim is kept while the bow hand stays within", BowKeepAngle(), 0, 60, "%.0f deg", 5))
            SaveF(L"twohand", L"BowKeepAngle", *BowKeepAngle());
        if (FineSlider("Tick in both hands when the arrow snaps on (0 = none)", BowNockTick(), 0, 1, "%.2f", 0.1f))
            SaveF(L"twohand", L"BowNockTick", *BowNockTick());
        ImGui::TextDisabled("The bow is the left hand's. 100%% = the arrow's line runs through the arrow hand at full pull (as before);\n"
                            "less = the left hand decides more. 0 deg = the bow always goes back to the left controller after a shot.");
    }
    ImGui::Separator();
    ImGui::Checkbox("Preview while this menu is open: bow in the left hand, arrow in the right", BowPreview());
    if (*BowPreview()) {
        ImGui::SameLine();
        ImGui::Checkbox("Show it nocked", BowPreviewNocked());
        bool ok = BowActive();
        ImGui::TextDisabled(ok ? "Previewing. Point and click with the right controller as usual; the arrow moves with it."
                               : "Take the bow out first (it shows once the bow is in your hands).");
    }
    static const char *const kNames[6] = {"Right", "Up", "Forward", "Pitch", "Yaw", "Roll"};
    static const wchar_t *const kB[6] = {L"BowHandRight", L"BowHandUp", L"BowHandForward", L"BowHandPitch", L"BowHandYaw", L"BowHandRoll"};
    static const wchar_t *const kA[6] = {L"ArrowGripRight", L"ArrowGripUp", L"ArrowGripForward", L"ArrowGripPitch", L"ArrowGripYaw", L"ArrowGripRoll"};
    ImGui::Text("Bow in the left hand");
    {
        float *bp = BowHandPose();
        bool ch = false;
        char lab[32];
        for (int i = 0; i < 6; ++i) {
            snprintf(lab, sizeof lab, "%s##bh%d", kNames[i], i);
            ch |= i < 3 ? FineSlider(lab, &bp[i], -0.8f, 0.8f, "%.4f m", 0.005f) : FineSlider(lab, &bp[i], -180, 180, "%.2f deg", 1);
        }
        if (ch) {
            *BowHandPoseSet() = true;
            for (int i = 0; i < 6; ++i) SaveF(L"twohand", kB[i], bp[i]);
        }
    }
    ImGui::Text("Arrow in the right hand (before it is nocked; points out the front of the controller at zero)");
    {
        // arrowsets1 (DrGonzaga 13 Nov): the first arrow (the bow's own, until the first shot after loading) and the
        // later ones are different models; each has its own place in the hand. Pick which one the sliders move.
        static int s_set;  // 0 later arrows, 1 the first arrow
        static const wchar_t *const kF[6] = {L"FirstGripRight", L"FirstGripUp", L"FirstGripForward", L"FirstGripPitch", L"FirstGripYaw", L"FirstGripRoll"};
        ImGui::TextUnformatted("Adjust:");
        ImGui::SameLine();
        ImGui::RadioButton("Later arrows", &s_set, 0);
        ImGui::SameLine();
        ImGui::RadioButton("First arrow", &s_set, 1);
        int kind = GunAttachArrowKind();
        ImGui::SameLine();
        ImGui::TextDisabled(kind == 1 ? "(in your hand now: the FIRST arrow)" : kind == 2 ? "(in your hand now: a LATER arrow)" : "(no arrow in your hand now)");
        if (kind && (kind == 1) != (s_set == 1)) ImGui::TextDisabled("You are adjusting the other kind: nothing will move until that arrow is in your hand.");
        float *ap = s_set ? BowArrowPoseFirst() : BowArrowPose();
        bool ch = false;
        char lab[32];
        for (int i = 0; i < 6; ++i) {
            if (i == 2) continue;  // forward = slide along, below
            snprintf(lab, sizeof lab, "%s##ah%d", kNames[i], i);
            ch |= i < 3 ? FineSlider(lab, &ap[i], -0.5f, 0.5f, "%.4f m", 0.005f) : FineSlider(lab, &ap[i], -180, 180, "%.2f deg", 1);
        }
        if (ch)
            for (int i = 0; i < 6; ++i) SaveF(L"twohand", s_set ? kF[i] : kA[i], ap[i]);
        // along the shaft: the later arrows' value is the base, the first arrow's is that plus its own shift;
        // moving the later arrows leaves the first arrow where it is
        if (!s_set) {
            float was = *BowArrowLength(), al = was * 100;  // bow7p: one slider for where the hand grips the shaft
            if (FineSlider("Grip along the shaft (tip this far ahead of the hand)", &al, -20, 110, "%.1f cm", 1)) {
                *BowArrowLength() = al / 100;
                *BowFirstArrowShift() -= al / 100 - was;
                SaveF(L"twohand", L"ArrowTipAhead", al / 100);
                SaveF(L"twohand", L"FirstArrowShift", *BowFirstArrowShift());
            }
        } else {
            float tot = (*BowArrowLength() + *BowFirstArrowShift()) * 100;
            if (FineSlider("Grip along the shaft (tip this far ahead of the hand)##first", &tot, -100, 200, "%.1f cm", 1)) {
                *BowFirstArrowShift() = tot / 100 - *BowArrowLength();
                SaveF(L"twohand", L"FirstArrowShift", *BowFirstArrowShift());
            }
        }
    }
    ImGui::TextDisabled("Nocked, the arrow sits on the bow as the game animates it: string and arrow draw back\n"
                        "together as far as your hands pull (see Full draw below).");
    {   // bow15p
        ImGui::Text("Nock point on the right controller (the bow turns its arrow line through it)");
        static const char *const kN[3] = {"Right##np", "Up##np", "Forward##np"};
        static const wchar_t *const kK[3] = {L"NockPointRight", L"NockPointUp", L"NockPointForward"};
        for (int i = 0; i < 3; ++i) {
            float c = BowNockPoint()[i] * 100;
            if (FineSlider(kN[i], &c, -20, 20, "%.1f cm", 1)) SaveF(L"twohand", kK[i], c / 100);
            BowNockPoint()[i] = c / 100;
        }
    }
    ImGui::Separator();
    ImGui::Text("Shooting");
    float nr = *BowNockReach() * 100;
    if (FineSlider("Nock reach (arrow shaft to the bow)", &nr, 5, 50, "%.1f cm", 1)) SaveF(L"twohand", L"BowNockReach", nr / 100);
    *BowNockReach() = nr / 100;
    float nh = *BowNockHands() * 100;  // bow12p
    if (FineSlider("Also nocks with the hands this close", &nh, 10, 70, "%.1f cm", 1)) SaveF(L"twohand", L"BowNockHands", nh / 100);
    *BowNockHands() = nh / 100;
    // unnock1
    if (FineSlider("Un-nock when the arrow hand rolls by", BowUnnockAngle(), 0, 120, "%.0f deg", 1)) SaveF(L"twohand", L"BowUnnockRoll", *BowUnnockAngle());
    ImGui::TextDisabled("Only with the string at rest (first quarter of the pull). The arrow stays in the hand; let go of the grip to put it back, or take the hand away from the bow and bring it back to nock again. 0 = never.");
    float fd = *BowFullDraw() * 100;
    if (FineSlider("Full draw (hands this far apart)", &fd, 35, 90, "%.1f cm", 1)) SaveF(L"twohand", L"BowFullDraw", fd / 100);
    *BowFullDraw() = fd / 100;
    ImGui::Text("After the shot");
    if (FineSlider("Bow dips by", BowDropAmount(), 0, 40, "%.1f deg", 1)) SaveF(L"twohand", L"BowDrop", *BowDropAmount());
    float di = *BowDropIn() * 1000, db = *BowDropBack() * 1000, rt = *BowReturnTime() * 1000;  // bow12p
    if (FineSlider("Dip snaps down in", &di, 5, 300, "%.0f ms", 10)) SaveF(L"twohand", L"BowDropIn", di / 1000);
    *BowDropIn() = di / 1000;
    if (FineSlider("Dip springs back (time constant)", &db, 10, 500, "%.0f ms", 10)) SaveF(L"twohand", L"BowDropBack", db / 1000);
    *BowDropBack() = db / 1000;
    if (FineSlider("Bow returns to the left hand in", &rt, 20, 1500, "%.0f ms", 50)) SaveF(L"twohand", L"BowReturnTime", rt / 1000);
    *BowReturnTime() = rt / 1000;
    {   // bow15p
        float dd = *BowDipDown() * 100, df = *BowDipForward() * 100;
        if (FineSlider("Bow drops down by", &dd, 0, 20, "%.1f cm", 1)) SaveF(L"twohand", L"BowDipDown", dd / 100);
        *BowDipDown() = dd / 100;
        if (FineSlider("Bow moves forward by", &df, -10, 20, "%.1f cm", 1)) SaveF(L"twohand", L"BowDipForward", df / 100);
        *BowDipForward() = df / 100;
    }
    if (ImGui::Button("Play the dip")) BowPlayRelease();
    ImGui::SameLine();
    ImGui::TextDisabled("(with Preview on, shows the dip on the bow in your hand)");
    ImGui::TextDisabled("Arrows come from any hip or shoulder spot while the bow is in your left hand.\n"
                        "Left grip next to the right hand also takes a bow that is already out.");
}

void DebugPage() {
    ImGui::TextDisabled("Now: %s", EngineCutscene() ? "in a cutscene" : EngineTurret() ? "on a mounted gun" : EngineInVehicle() ? "in a vehicle (seated)" : "on foot");
    ImGui::Separator();
    ImGui::TextWrapped("Tools for working on the mod. Results go to fc3vr.log.");
    if (ImGui::Button("Profile GPU (10 s)")) StereoGpuProfile();
    ImGui::SameLine();
    ImGui::TextDisabled("close the menu and play normally");
    ImGui::Separator();
    // fast1; wake3: sleeping workers are now woken once a clock tick to empty their inboxes (no leak)
    if (ImGui::Checkbox("Wake a sleeping worker anyway while the others are busy (cures the slow picture on the first shot)", StereoWakeFast()))  // wakefast1
        SaveF(L"perf", L"WakeFast", *StereoWakeFast() ? 1.f : 0.f);
    ImGui::SliderInt("Workers kept awake before waking more", StereoWakeKeep(), 0, 6, *StereoWakeKeep() ? "%d" : "0 = off");
    if (ImGui::IsItemDeactivatedAfterEdit())
        SaveF(L"perf", L"WakeKeep", (float)*StereoWakeKeep()), Log("perf: workers kept awake now %d (slider)", *StereoWakeKeep());
    if (ImGui::Checkbox("Quick buffer check (skips the game's slow one)", StereoFastCheck()))
        SaveF(L"perf", L"FastBufferCheck", *StereoFastCheck() ? 1.f : 0.f);
    {
        float ft[5];
        StereoFrameTimes(ft);
        ImGui::Text("CPU per eye now (ms): left %.2f  right %.2f   headset wait %.1f", ft[0], ft[1], ft[2]);
    }
    ImGui::Separator();
    if (More("VR switches (leave on; for testing)")) {
        bool tw = StereoTwice();
        if (ImGui::Checkbox("True stereo (each eye rendered)", &tw)) StereoSetTwice(tw);
        static const char *names[] = {"Stereo (eye separation)", "6DoF head movement", "Headset field of view",
                                      "Level horizon", "Head tracking camera"};
        for (int i = 0; i < 5; ++i) {
            bool b = EngineGetOpt(i);
            if (ImGui::Checkbox(names[i], &b)) EngineSetOpt(i, b);
        }
    }
    ImGui::TextDisabled("The keyboard debug keys (F4-F8, F10, Numpad) are off unless fc3vr.ini [debug] DebugKeys=1. F9 recentre and Insert always work.");
    ImGui::Separator();
    if (ImGui::Checkbox("Detailed log (timings and states every 10 s)", &g_verboseLog))
        SaveF(L"debug", L"VerboseLog", g_verboseLog ? 1.f : 0.f);
    ImGui::TextDisabled("Leave off for play; turn on when something needs looking into.");
}

// ---------------------------------------------------------------------------------------------------------------
// menu2 (DrGonzaga 20 Nov: "an in-game menu, especially with controller instructions ... and a big controls picture to
// the side"): the menu is in sections (Controls, Options, Guns, Gameplay, Debug). Controls explains every control,
// one situation a page, written from the live settings (button mapping, what X does, grips, wrist menu ...), with a
// picture for that page on a second panel beside the menu. Pictures are plain files (bin\bdvr-pictures\*.png, any
// size, shape kept) so DrGonzaga's own diagrams can be dropped in without a new build; "Reload pictures" re-reads them.

enum { kPicGuns, kPicBody, kPicBow, kPicThrow, kPicMove, kPicVehicles, kPicMenus, kPicControllers, kPicControllersL, kPicBodyL, kPicTutorial, kPicCount };
const char *const kPicFile[kPicCount] = {"controls-guns.png",   "controls-body.png",     "controls-bow.png",
                                         "controls-throw.png",  "controls-moving.png",   "controls-vehicles.png",
                                         "controls-menus.png",  "controls-controllers.png",  // synth1: DrGonzaga's two diagrams (and left-handed copies)
                                         "controls-controllers-left.png", "controls-body-left.png",
                                         "controls-tutorial.png"};  // tut1: DrGonzaga's tutorial tips screen
struct PicData {
    BYTE *px;  // RGBA8, from the worker thread
    int w, h;
    bool found;
};
PicData g_picData[kPicCount];
ID3D11ShaderResourceView *g_picSrv[kPicCount];
int g_picW[kPicCount], g_picH[kPicCount];
bool g_picFound[kPicCount];
volatile LONG g_picState;  // 0 idle, 1 the worker is reading, 2 read: the render thread makes the textures
volatile bool g_picWant = true;  // read (again) when the menu next draws
int g_picShow = -1;              // the picture beside the menu this frame (-1 none)

void PicDir(wchar_t *dir) {  // bin\bdvr-pictures\ (next to fc3vr.ini)
    IniPath(dir);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (s) s[1] = 0;
    else dir[0] = 0;
    wcscat(dir, L"bdvr-pictures\\");
}

DWORD WINAPI PicWorker(void *) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void **)&f);
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    PicDir(dir);
    for (int i = 0; i < kPicCount; ++i) {
        PicData &d = g_picData[i];
        d.px = nullptr, d.w = d.h = 0, d.found = false;
        swprintf(path, MAX_PATH, L"%ls%hs", dir, kPicFile[i]);
        if (!f || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        d.found = true;
        IWICBitmapDecoder *dec = nullptr;
        IWICBitmapFrameDecode *fr = nullptr;
        IWICFormatConverter *cv = nullptr;
        UINT w = 0, h = 0;
        if (SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(cv->GetSize(&w, &h)) && w >= 16 && h >= 16 && w <= 8192 && h <= 8192) {
            BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
            if (px && SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, px))) d.px = px, d.w = (int)w, d.h = (int)h;
            else if (px) HeapFree(GetProcessHeap(), 0, px);
        }
        if (cv) cv->Release();
        if (fr) fr->Release();
        if (dec) dec->Release();
    }
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    InterlockedExchange(&g_picState, 2);
    return 0;
}

// Render thread (MenuDraw): start a read when wanted, make the textures when it is done.
void PicUpdate() {
    if (g_picWant && g_picState == 0) {
        g_picWant = false;
        InterlockedExchange(&g_picState, 1);
        HANDLE t = CreateThread(nullptr, 0, PicWorker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else PicWorker(nullptr);
    }
    if (g_picState != 2) return;
    char list[400];
    int len = 0;
    list[0] = 0;
    for (int i = 0; i < kPicCount; ++i) {
        PicData &d = g_picData[i];
        if (g_picSrv[i]) g_picSrv[i]->Release(), g_picSrv[i] = nullptr;
        g_picW[i] = g_picH[i] = 0, g_picFound[i] = d.found;
        if (d.px) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = d.w, td.Height = d.h, td.MipLevels = 0, td.ArraySize = 1;  // full mip chain: no shimmer when small
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D *tex = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex))) {
                g_ctx->UpdateSubresource(tex, 0, nullptr, d.px, d.w * 4, 0);
                if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, nullptr, &g_picSrv[i])))
                    g_ctx->GenerateMips(g_picSrv[i]), g_picW[i] = d.w, g_picH[i] = d.h;
                tex->Release();
            }
            HeapFree(GetProcessHeap(), 0, d.px), d.px = nullptr;
        }
        len += snprintf(list + len, sizeof list - len, " %s %s;", kPicFile[i],
                        g_picSrv[i] ? "ok" : d.found ? "NOT READABLE" : "missing");
        if (len >= (int)sizeof list) len = sizeof list - 1;
    }
    Log("menu: pictures (bin\\bdvr-pictures):%s", list);
    InterlockedExchange(&g_picState, 0);
}

// Where the side picture goes (recentred XR space): centre, right axis and size. It is hinged at the menu's
// side edge (3 cm gap) and turned towards you by the angle, as tall as set, as wide as its shape says.
bool PicQuad(int i, float c[3], float r[3], float *w, float *h) {
    if (!g_picOn || i < 0 || i >= kPicCount || !g_picSrv[i] || !g_picH[i]) return false;
    float H = g_picHeight, W = H * g_picW[i] / g_picH[i];
    if (W > 1.5f) W = 1.5f, H = W * g_picH[i] / g_picW[i];
    float a = g_picAngle * 0.01745329f, s = g_picRight ? 1.f : -1.f;
    for (int k = 0; k < 3; ++k) {
        r[k] = cosf(a) * g_r[k] + s * sinf(a) * g_n[k];
        float edge = g_c[k] + s * g_r[k] * (kPanelW * 0.5f + 0.03f);
        c[k] = edge + s * r[k] * W * 0.5f;
    }
    *w = W, *h = H;
    return true;
}

// ---- the Controls pages ----

// Touch inputs and game (Xbox pad) outputs, in input.cpp's order.
enum { cInA, cInB, cInX, cInY, cInLGrip, cInRGrip, cInLClick, cInRClick, cInMenu, cInGripMenu, cInRGripMenu, cInLT, cInRT,
       cInStickUp, cInXY, cInAB, cInCount };
enum { cOutNone, cOutA, cOutB, cOutX, cOutY, cOutLB, cOutRB, cOutLT, cOutRT, cOutLS, cOutRS, cOutStart, cOutBack, cOutUp,
       cOutDown, cOutLeft, cOutRight, cOutHud, cOutCount };
// What each pad output does in Blood Dragon on foot.
const char *const kGameDoes[cOutCount] = {"nothing",
                                          "jump",
                                          "crouch / stand (the game's back in its menus)",
                                          "reload; hold to pick up and use things",
                                          "switch to your last weapon",
                                          "the game's weapon wheel",
                                          "the game's throw",
                                          "aim down sights",
                                          "fire",
                                          "sprint",
                                          "melee",
                                          "the game's pause menu",
                                          "the game's map",
                                          "cyber-eye (the game's binoculars)",
                                          "the game's D-pad down",
                                          "throw a cyber heart (the game's D-pad left)",
                                          "throw a D20 to distract enemies",
                                          "nothing (the HUD is switched on the HUD page)"};
const char *const kTouchName[cInCount] = {"A", "B", "X", "Y", "Left grip", "Right grip", "Left stick click", "Right stick click",
                                          "Menu", "Left grip + Menu", "Right grip + Menu", "Left trigger", "Right trigger",
                                          "Right stick up hard", "X + Y", "A + B"};
int Mapped(int in) { return *InputMap(in); }
// The first Touch input that sends pad output 'out' ("" if none). Single buttons before the combos.
const char *Sends(int out) {
    static const int order[] = {cInA, cInB, cInX, cInY, cInLT, cInRT, cInLClick, cInRClick, cInStickUp, cInMenu,
                                cInLGrip, cInRGrip, cInXY, cInAB, cInGripMenu, cInRGripMenu};
    for (int in : order)
        if (Mapped(in) == out) return kTouchName[in];
    return "";
}

const ImVec4 kKeyCol(1.0f, 0.82f, 0.25f, 1), kHeadCol(0.35f, 0.9f, 1.0f, 1), kNoteCol(0.65f, 0.65f, 0.7f, 1);
// One control: the button(s) in yellow, then what it does, wrapped.
void Ctl(const char *keys, const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::Bullet();
    ImGui::TextColored(kKeyCol, "%s", keys);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", buf);
}
void Head(const char *s) {
    ImGui::Spacing();
    ImGui::TextColored(kHeadCol, "%s", s);
}
void Note(const char *s) {
    ImGui::PushStyleColor(ImGuiCol_Text, kNoteCol);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}
// "B" or, when nothing sends it, a pointer to the Buttons page.
const char *KeyFor(int out) {
    const char *s = Sends(out);
    return *s ? s : "(not on a button)";
}

void CtlGuns() {
    Head("Shooting");
    Ctl("Right trigger", "fire. Bullets go where the right controller points; your head never aims.");
    Ctl("Left trigger", "aim down sights, or just bring the gun up to your eye.");
    Ctl(KeyFor(cOutX), "reload. Hold it to pick things up and use things.");
    Ctl(KeyFor(cOutY), "switch to your last weapon.");
    Ctl("Swing", "swing your right hand fast for melee and takedowns (also %s).", KeyFor(cOutRS));
    if (*InputGripHoldPtr())
        Ctl("Right grip", "hold it while you keep the gun still: a Quest controller nobody touches can stop tracking. "
                          "On foot it does nothing else, except at a weapon spot (Body page).");
    else Ctl("Right grip", "%s.", kGameDoes[Mapped(cInRGrip)]);
    Head("The X button on foot, gun in hand");
    if (*InputXMode() == 0) Ctl("X", "tap to throw the grenade or molotov picked on your left wrist; hold to cook it, let go to throw.");
    else if (*InputXMode() == 1) Ctl("X", "minimap on / off.");
    else Ctl("X", "the game's map.");
    Note("In cutscenes, button-mash scenes, vehicles and with no gun, X goes to the game as set on the Buttons page.");
    Head("Two hands");
    Ctl("Left grip", "rifles, shotguns, SMGs: put your left hand on the front of the gun and hold the left grip. "
                     "Steadier, less kick and spread. Let go for one hand.");
    Ctl("Hands together", "the pistol: bring the left controller to the right one (about 15 cm). No button needed.");
    Head("Scopes");
    Ctl("Gun to eye", "look through the scope by bringing the gun up to your eye.");
    if (*InputScopeZoomButtons()) Ctl("A / X", "while looking through a scope: A zooms in a step, X zooms out a step.");
    Head("Ammo");
    {
        HudPanelCfg *a = HudPanel(1);
        if (a && a->own && (a->anchor == 2 || a->anchor == 3) && a->glance)
            Ctl("Wrist glance", "turn your %s wrist towards you and look at it, like a watch.", a->anchor == 3 ? "right" : "left");
        else Ctl("Ammo", "shown on its HUD panel (HUD page).");
    }
}

void CtlBody() {
    Head("Weapons on your body");
    Ctl("Right grip", "put your right hand on a hip or behind a shoulder (it buzzes when you are there) and squeeze once: "
                      "that spot's weapon comes out. Which weapon is where: Guns > Gun & body.");
    Note("Your view holds still while your hand is on a spot, so reaching behind your head doesn't swing the picture.");
    Head("Left wrist menu");
    if (*HolsterWristOpt(0)) {
        Ctl("Look", "turn your left wrist to your face: the throwables (grenade, molotov, C4, mine, cyber heart) appear with how many you have.");
        Ctl("Right grip", "with your right hand on an icon: grenade / molotov becomes what X throws; C4 and mines come out into your hands.");
        Ctl("Cyber heart", "grip its icon, swing and let go to throw it. Let go back on the wrist to put it back.");
    } else Note("The wrist menu is off (Guns > Gun & body).");
    Head("Beside your head (left hand)");
    Ctl("Left grip tap", "objective marker for 10 seconds.");
    if (*CyberEyeHoldKeep()) Ctl("Left grip hold", "cyber-eye while you hold it (after about half a second); let go to come back.");
    else Ctl("Left grip hold", "about half a second: cyber-eye on; do it again for off.");
    Ctl("Cyber-eye", "right trigger zooms in, left trigger zooms out. Weapon spots, wrist menu and swing melee are off meanwhile.");
    Head("Slow motion");
    if (SlowmoOnWrist()) Ctl("Right grip", "on the clock on your left wrist menu: slow motion on / off.");
    else Ctl("Left grip", "put your left hand on the floating clock beside the minimap and squeeze: slow motion on / off.");
    Note("Both controllers buzz three times when it starts and twice when it ends.");
}

void CtlBow() {
    Head("Shooting an arrow");
    Ctl("Left grip", "at the bow's spot (right shoulder): take the bow. It goes into your left hand and stays there.");
    Ctl("Right grip", "at the same spot: take an arrow.");
    Ctl("Nock", "bring the arrow to the bow; both controllers tick when it snaps on.");
    Ctl("Draw", "keep the right grip held and pull back. How far you pull is how hard it shoots.");
    Ctl("Loose", "let go of the right grip.");
    Head("Un-nocking");
    Ctl("Roll", "string at rest: roll your arrow hand a quarter turn to take the arrow off. Let go to put it away.");
    Ctl("Right grip", "at another weapon's spot: that weapon (the bow goes away).");
    Note("No swing melee while the bow is out, so reaching for arrows can't set it off.");
}

void CtlThrow() {
    Head("Grenades and molotovs");
    Ctl("Wrist", "pick the grenade or molotov on your left wrist with the right grip (its icon stays larger).");
    if (*InputXMode() == 0) Ctl("X", "tap to throw where your right hand points; hold to cook, let go to throw.");
    else Ctl("X", "X is set to the %s, so the quick throw is off (Options > Buttons).", *InputXMode() == 1 ? "minimap" : "map");
    Note("X throws only on foot with a weapon in hand, and not just after you close the map or pause menu.");
    Head("C4");
    Ctl("Wrist", "pick C4: the charge comes into your left hand, the detonator into your right.");
    Ctl("Left trigger", "throw a charge where your left hand points.");
    Ctl("Right trigger", "set the charges off.");
    Head("Mines");
    Ctl("Wrist", "pick the mine: it comes out in your right hand.");
    Ctl("Right trigger", "place it.");
    Head("Cyber heart and D20");
    Ctl("Cyber heart", "grip its wrist icon, swing and let go to throw (also %s).", KeyFor(cOutLeft));
    Ctl(KeyFor(cOutRight), "throw a D20 to distract enemies.");
}

void CtlMove() {
    Head("Walking");
    Ctl("Left stick", "walk, %s.", *InputWalkDir() == 1   ? "where your left hand points (look and reach anywhere without turning your walk)"
                                   : *InputWalkDir() == 2 ? "where your head looks"
                                                          : "where the gun (right controller) aims");
    Ctl(KeyFor(cOutLS), "sprint.");
    Ctl(KeyFor(cOutA), "jump.");
    Head("Turning");
    if (*EngineSnapTurn()) Ctl("Right stick", "left / right: snap turn %.0f degrees. Or just turn round in the room.", *EngineSnapAngle());
    else Ctl("Right stick", "left / right: smooth turn. Or just turn round in the room.");
    Head("Crouching");
    if (*EngineRealCrouch())
        Ctl("Real crouch", "crouch down for real (about %.0f cm) and you crouch in the game; stand up to stand.", *EngineRealCrouchDepth() * 100);
    Ctl("Right stick down", "pull down hard: crouch, again: stand (%s also stands you up).", KeyFor(cOutA));
    Head("Ladders and ziplines");
    Ctl("Ladders", "climb with the left stick. Climbing down, the view turns you to face the ladder.");
    Ctl("Ziplines", "use the game's prompt. Your left hand holds the line by itself.");
    Head("View");
    Ctl("Recentre", "closing this menu recentres your view (F9 on the keyboard too).");
}

void CtlVehicles() {
    Head("Driving");
    Ctl("Controllers", "in a vehicle they work like the game's own Xbox pad: right trigger to go, left trigger to brake / reverse, "
                       "left stick to steer. The game's prompts show the rest.");
    if (*EngineVehExitKeep()) Note("Getting out, you keep facing the way you looked in the vehicle.");
    Head("Mounted guns and the helicopter turret");
    Ctl("Right controller", "aim the gun; right trigger fires.");
    Note("On the helicopter turret your view sits above the gun and rides with the helicopter.");
    Head("Cutscenes");
    Note("Cutscenes play on a big screen in front of you. Scenes that ask for a button press stay in VR, and the buttons go "
         "straight to the game. Your controllers rumble when the game rumbles.");
}

void CtlMenus() {
    Head("This menu");
    Ctl("Both stick clicks", "open or close it (Insert on the keyboard).");
    Ctl("Right controller", "point; right trigger clicks; right stick up / down scrolls.");
    Ctl("B", "close it. Closing it also recentres your view.");
    Head("Map and pause");
    if (*InputMenuMinimap()) {
        Ctl("Menu", "tap: the game's map.");
        Ctl("Left trigger + Menu", "hold the left trigger (or left grip) and tap Menu: the game's pause menu.");
        Ctl("Menu", "while paused or on the map: closes it. Use short taps; Virtual Desktop keeps long presses for itself.");
    } else Ctl("Menu", "%s.", kGameDoes[Mapped(cInMenu)]);
    Head("Every button as you have it set (Options > Buttons)");
    if (ImGui::BeginTable("btns", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Touch", ImGuiTableColumnFlags_WidthFixed, 330);
        ImGui::TableSetupColumn("In the game");
        for (int i = 0; i < cInCount; ++i) {
            int o = Mapped(i);
            if (o < 0 || o >= cOutCount) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kKeyCol, "%s", kTouchName[i]);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", kGameDoes[o]);
        }
        ImGui::EndTable();
    }
    Note("These are what each button sends to the game when nothing above takes it first (scopes, bow, wrist menu ...).");
}

// The picture panel's own settings, under each Controls page.
void PicSettings(int page) {
    ImGui::Separator();
    char lbl[96];
    snprintf(lbl, sizeof lbl, "Picture: %s", kPicFile[page]);
    if (!ImGui::CollapsingHeader(lbl)) return;
    if (g_picSrv[page]) ImGui::Text("Loaded, %d x %d.", g_picW[page], g_picH[page]);
    else if (g_picState) ImGui::Text("Reading...");
    else if (g_picFound[page]) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Found but could not be read (PNG or JPG, at most 8192 px).");
    else ImGui::Text("Not there yet.");
    Note("Pictures live in the game's bin\\bdvr-pictures folder. Any size works and keeps its shape; "
         "1600 x 1200 suits the panel best. Replace a file, then press Reload.");
    if (ImGui::Button("Reload pictures")) g_picWant = true;
    if (ImGui::Checkbox("Show the picture beside the menu", &g_picOn)) SaveF(L"menu", L"SidePicture", g_picOn ? 1.f : 0.f);
    if (ImGui::RadioButton("Right of the menu", g_picRight == 1)) g_picRight = 1, SaveF(L"menu", L"SidePictureRight", 1);
    ImGui::SameLine();
    if (ImGui::RadioButton("Left of the menu", g_picRight == 0)) g_picRight = 0, SaveF(L"menu", L"SidePictureRight", 0);
    float cm = g_picHeight * 100;
    if (ImGui::SliderFloat("Picture height", &cm, 30, 120, "%.0f cm")) g_picHeight = cm / 100;
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"menu", L"SidePictureHeight", g_picHeight);
    ImGui::SliderFloat("Turned towards me", &g_picAngle, 0, 60, "%.0f deg");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveF(L"menu", L"SidePictureAngle", g_picAngle);
}

void ControlsSection() {
    struct Page {
        const char *name;
        void (*fn)();
        int pic;
    };
    static const Page pages[] = {{"Guns", CtlGuns, kPicGuns},          {"Body", CtlBody, kPicBody},
                                 {"Bow", CtlBow, kPicBow},             {"Throwing & C4", CtlThrow, kPicThrow},
                                 {"Moving", CtlMove, kPicMove},        {"Vehicles", CtlVehicles, kPicVehicles},
                                 {"Menus & buttons", CtlMenus, kPicMenus}};
    if (ImGui::BeginTabBar("controls")) {
        for (const Page &pg : pages)
            if (ImGui::BeginTabItem(pg.name)) {
                g_picShow = pg.pic;
                pg.fn();
                PicSettings(pg.pic);
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
}
// ---------------------------------------------------------------------------------------------------------------

#include "synth.inc"

void DebugUi() {
    float t[5];
    StereoFrameTimes(t);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)kTexW, (float)kTexH));
    bool keep = true;
    ImGui::Begin(g_bloodDragon ? "Blood Dragon VR###menu" : "Far Cry 3 VR###menu", &keep, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    if (!keep) g_open = false;  // the X in the corner
    g_picShow = -1;
    struct Tab {
        const char *name;
        void (*page)();
    };
    // menu2: sections, each with its pages (the old tabs, unchanged inside).
    static const Tab guns[] = {{"Aim", AimPage}, {"Beam & scope", BeamScopePage}, {"Gun & body", GunBodyPage},
                               {"Two handed mode", TwoHandPage}, {"Bow & arrow", BowPage}};
    static const Tab opts[] = {{"HUD", HudPage}, {"Graphics", GraphicsPage}, {"Buttons", ButtonsPage}};
    if (ImGui::BeginTabBar("sections")) {
        if (ImGui::BeginTabItem("Controls")) ControlsSection(), ImGui::EndTabItem();
        if (ImGui::BeginTabItem("Options")) {
            if (ImGui::BeginTabBar("opts")) {
                if (ImGui::BeginTabItem("VR")) VrPage(t), ImGui::EndTabItem();
                for (const Tab &tb : opts)
                    if (ImGui::BeginTabItem(tb.name)) tb.page(), ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Guns")) {
            if (ImGui::BeginTabBar("guns")) {
                for (const Tab &tb : guns)
                    if (ImGui::BeginTabItem(tb.name)) tb.page(), ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gameplay")) GameplayPage(), ImGui::EndTabItem();
        if (ImGui::BeginTabItem("Debug")) DebugPage(), ImGui::EndTabItem();
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close (B)")) g_open = false;
    ImGui::SameLine();
    if (ImGui::Button("<< Back to the VR menu")) g_synth = true;  // synth1
    ImGui::End();
}

void Ui() {
    if (g_synth) SynthUi();
    else DebugUi();
}

// Draw the menu into its texture, once per frame.
void RenderUi() {
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    double dt = g_lastUi.QuadPart ? double(now.QuadPart - g_lastUi.QuadPart) / fq.QuadPart : 1 / 90.0;
    if (dt < 0.006) return;  // the other eye of this frame
    g_lastUi = now;

    VrPad p = {};
    EnterCriticalSection(&g_padLock);
    p = g_pad;
    LeaveCriticalSection(&g_padLock);
    Point();
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = (float)(dt > 0.1 ? 0.1 : dt);
    io.AddMousePosEvent(g_hit ? g_mouse[0] : -FLT_MAX, g_hit ? g_mouse[1] : -FLT_MAX);
    io.AddMouseButtonEvent(0, p.rt > 0.5f);
    g_synthScroll = 0;
    if (fabsf(p.ry) > 0.3f) {
        if (g_synth) g_synthScroll = p.ry * (float)dt * 900;  // synth1: the page scrolls wherever the pointer is
        else io.AddMouseWheelEvent(0, p.ry * (float)dt * 12);
    }
    // synth1: controller navigation (walking stick = move between items, A = press)
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, p.ly > 0.5f, p.ly > 0 ? p.ly : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, p.ly < -0.5f, p.ly < 0 ? -p.ly : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, p.lx < -0.5f, p.lx < 0 ? -p.lx : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, p.lx > 0.5f, p.lx > 0 ? p.lx : 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, p.a);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    g_queueSaves = true;  // save1
    Ui();
    g_queueSaves = false;
    if (!ImGui::IsAnyItemActive()) FlushSaves();  // nothing held: write the changes
    ImGui::Render();
    ID3D11RenderTargetView *oRtv = nullptr;
    ID3D11DepthStencilView *oDsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &oRtv, &oDsv);
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_texRtv, clear);
    g_ctx->OMSetRenderTargets(1, &g_texRtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());  // saves and restores the rest of the state itself
    g_ctx->OMSetRenderTargets(1, &oRtv, oDsv);
    if (oRtv) oRtv->Release();
    if (oDsv) oDsv->Release();
}

// The panel's corners in this eye, as clip positions (w = depth, so the texture isn't skewed).
bool Corners(float out[4][4], const float *pc, const float *pr, const float *pu, float pw, float ph) {
    EngineView v;
    float tx, ty;
    if (!EngineLastView(&v) || !XrGetHeadsetFov(&tx, &ty)) return false;
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    float nr[3] = {v.fwd[1] * v.up[2] - v.fwd[2] * v.up[1], v.fwd[2] * v.up[0] - v.fwd[0] * v.up[2],
                   v.fwd[0] * v.up[1] - v.fwd[1] * v.up[0]};
    for (int k = 0; k < 4; ++k) {  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right (strip order)
        float sx = (k & 1) ? 0.5f : -0.5f, sy = (k & 2) ? -0.5f : 0.5f;
        float o[3];
        for (int i = 0; i < 3; ++i) o[i] = (pc[i] + pr[i] * sx * pw + pu[i] * sy * ph) / v.scale;
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = v.base[i] + fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2] - v.pos[i];
        float z = Dot(p, v.fwd);
        if (z < 0.05f) return false;
        out[k][0] = Dot(p, nr) / tx, out[k][1] = Dot(p, v.up) / ty, out[k][2] = 0, out[k][3] = z;
    }
    return true;
}

void Composite() {
    // look1 (DrGonzaga 22 Nov: the menu closed when he turned to the big controls picture): the menu counts as
    // shown whenever this is a 3D frame, wherever you look; each panel is drawn only while it is in front of you.
    EngineView lv;
    if (!EngineLastView(&lv)) return;
    g_lastShown = GetTickCount();
    float cb[4][4], cbp[4][4], pc[3], pr[3], pw, ph;
    const bool menuIn = Corners(cb, g_c, g_r, g_u, kPanelW, kPanelH);
    const bool picIn = PicQuad(g_picShow, pc, pr, &pw, &ph) && Corners(cbp, pc, pr, g_u, pw, ph);
    if (!menuIn && !picIn) return;
    ID3D11DeviceContext *c = g_ctx;
    D3DState game(c, 1, 1);  // state1: the game's pipeline state, put back by Restore() below

    if (menuIn) c->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_w, (float)g_h, 0, 1};
    EngineEyeViewport(&vp.TopLeftX, &vp.TopLeftY, &vp.Width, &vp.Height);  // oc1
    c->RSSetViewports(1, &vp);
    c->RSSetState(g_rs);
    c->OMSetDepthStencilState(g_ds, 0);
    float bf[4] = {0, 0, 0, 0};
    c->OMSetBlendState(g_bs, bf, 0xffffffff);
    c->OMSetRenderTargets(1, &g_rtv, nullptr);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(g_vs, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &g_cb);
    c->PSSetShader(g_ps, nullptr, 0);
    c->PSSetShaderResources(0, 1, &g_texSrv);
    c->PSSetSamplers(0, 1, &g_smp);
    if (menuIn) c->Draw(4, 0);
    // menu2: the Controls page's picture on its own panel beside the menu
    if (picIn) {
        c->UpdateSubresource(g_cb, 0, nullptr, cbp, 0, 0);
        c->PSSetShaderResources(0, 1, &g_picSrv[g_picShow]);
        c->Draw(4, 0);
    }

    game.Restore();
}

// flat1: while a cutscene is on the 2D screen the eye image is that screen, so the
// panel goes to the headset as a layer of its own, at the same place.
void FlatPanel() {
    float yaw, rp[3];
    HeadtrackRefPose(&yaw, rp);
    float c = cosf(yaw), s = sinf(yaw), p[3];
    p[0] = rp[0] + c * g_c[0] + s * g_c[2];
    p[1] = rp[1] + g_c[1];
    p[2] = rp[2] - s * g_c[0] + c * g_c[2];
    XrSetMenuLayer(g_tex, p, yaw + atan2f(g_n[0], g_n[2]), kPanelW, kPanelH);
    g_lastShown = GetTickCount();
}

volatile LONG g_init;
void Init() {
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_padLock);
        LoadSettings();
        g_init = 2;
    }
    while (g_init != 2) Sleep(0);
}

}  // namespace

void MenuSaveLater(void (*fn)()) { Later(fn); }  // zoom1: other modules' saves, written off their thread


bool MenuOpen() { return g_open; }

void MenuOnSwapChainGoing() { Release(); }

// Input thread (the game's XInput read): toggle, and keep the latest pad for the menu.
void MenuInput(const VrPad &p) {
    Init();
    static bool prevBoth, prevB;
    bool both = p.lclick && p.rclick;
    bool toggle = (both && !prevBoth) || KeyHit(VK_INSERT);
    prevBoth = both;
    if (g_open && p.b && !prevB) g_open = false;
    prevB = p.b;
    if (toggle) {
        g_open = !g_open;
        if (g_open) g_placed = false, g_openedAt = GetTickCount(), g_synth = true;  // synth1: opens on the VR menu
        Log("menu: %s", g_open ? "open" : "closed");
    }
    // Where it can't be shown (the game's own menus, loading), close it, so it
    // never takes the controllers while you can't see it (menu1 did).
    static bool s_wasOpen;
    bool autoClosed = false;
    if (g_open && GetTickCount() - g_openedAt > 1000 && GetTickCount() - g_lastShown > 1000) {
        g_open = false, autoClosed = true;
        Log("menu: closed (it can't be shown here)");
    }
    // recentre1: you closing the menu (B, Close, both sticks, the X) recentres the view, like F9.
    if (s_wasOpen && !g_open && !autoClosed && g_recentreOnClose) {
        HeadtrackRequestRecenter();
        Log("menu: closed by you; view recentred");
    }
    s_wasOpen = g_open;
    EnterCriticalSection(&g_padLock);
    g_pad = p;
    LeaveCriticalSection(&g_padLock);
}

void MenuDraw(IDXGISwapChain *sc) {
    Init();
    if (!g_open) return FlushSaves();  // save1: closed mid-drag, write what's left
    if (!Setup(sc)) return;
    if (!g_placed) Place();
    if (!g_placed) return;
    PicUpdate();  // menu2
    HeadtrackOwnDraw(true);
    RenderUi();
    if (EngineFlat()) FlatPanel();  // flat1
    else Composite();
    HeadtrackOwnDraw(false);
}
