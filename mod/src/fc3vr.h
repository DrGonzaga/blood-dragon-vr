#pragma once
#include <windows.h>

// bd1 (12 Nov, Blood Dragon port): which game this is, worked out once from the exe name in
// Fc3vrStart. cleanA (22 Nov): this build is Blood Dragon only; the Far Cry 3 hooks and their
// address tables were removed (the FC3 mod has its own source).
extern bool g_bloodDragon;
bool BdEngineOn();  // bd2: fc3vr.ini [bd] Engine (default 1): 0 = Blood Dragon runs with no engine hooks (flat screen)
int BdStage();     // bd2: fc3vr.ini [bd] Stage (default 9): 1 = camera only, 2 = + render-twice stereo and worker wake-ups


// Called once from DllMain. Must not do real work under the loader lock;
// it only decides whether this process is the game and starts a thread.
void Fc3vrStart(HMODULE self);
void IniPath(wchar_t *path);  // fc3vr.ini next to version.dll

// settings1: fc3vr.ini is kept in memory (settings.cpp). The three Windows
// calls the mod uses for it are pointed at the table here, so every module's
// reads are instant and its writes go to one background writer.
UINT IniGetInt(LPCWSTR sec, LPCWSTR key, INT def, LPCWSTR path);
DWORD IniGetStr(LPCWSTR sec, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR path);
BOOL IniSet(LPCWSTR sec, LPCWSTR key, LPCWSTR val, LPCWSTR path);
void IniFlush();  // write what is still waiting, now (game quitting)
#ifndef FC3VR_SETTINGS_IMPL
#define GetPrivateProfileIntW IniGetInt
#define GetPrivateProfileStringW IniGetStr
#define WritePrivateProfileStringW IniSet
#endif

// state1: the game's D3D pipeline state, saved on construction and put back by
// Restore() (or at the end of the scope). d3dstate.cpp.
#ifdef __d3d11_h__
struct D3DState {
    D3DState(ID3D11DeviceContext *ctx, UINT psTextures, UINT psSamplers, bool allShaders = false);
    ~D3DState() { Restore(); }
    void Restore();
    // state2: between these two (hook.cpp, around the overlays drawn into one eye's
    // image) the state is saved once, by the first overlay that draws, and put back
    // once at the end, instead of by each overlay. [debug] ShareState=0 turns it off.
    static void ShareBegin();
    static void ShareEnd();

  private:
    D3DState() : live(false) {}
    void Save();
    ID3D11DeviceContext *c;
    UINT nSrv, nSmp;
    bool all, live;
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView *dsv = nullptr;
    UINT nvp = 0;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState *rs = nullptr;
    ID3D11DepthStencilState *ds = nullptr;
    UINT ref = 0;
    ID3D11BlendState *bs = nullptr;
    float bf[4];
    UINT mask = 0;
    ID3D11InputLayout *il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY top;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    ID3D11GeometryShader *gs = nullptr;
    ID3D11HullShader *hs = nullptr;
    ID3D11DomainShader *dsh = nullptr;
    ID3D11ClassInstance *vsCi[256], *psCi[256], *gsCi[256], *hsCi[256], *dsCi[256];
    UINT nVsCi = 0, nPsCi = 0, nGsCi = 0, nHsCi = 0, nDsCi = 0;
    ID3D11Buffer *vcb = nullptr, *pcb = nullptr;
    ID3D11ShaderResourceView *srv[8] = {};
    ID3D11SamplerState *smp[4] = {};
};
#endif

// keys1: the mod's keyboard keys (keys.cpp, which lists them).
void KeysPoll();        // render thread, every Present
bool KeyHit(int vk);    // true once per press

// patch1: the one place that writes into game code and tables (patch.cpp).
bool PatchBytes(void *at, const void *bytes, size_t n);        // up to 8 bytes land in one step
bool PatchCallTo(BYTE *site, const void *target);              // retarget a call/jmp rel32 (caller has checked it)
bool PatchJmp(BYTE *fn, const void *hook, size_t n);           // "jmp hook" + NOPs over n bytes (5 to 8)

// ptr1: memory checks that touch nothing and never fault (memcheck.cpp). The
// three Windows calls the mod uses for this are pointed at it here.
BOOL MemBad(const void *p, size_t n, bool write);
extern bool g_shareState;  // [debug] ShareState
extern bool g_safePtr;  // [debug] SafePtr (0 = the old Windows calls)
#ifndef FC3VR_MEMCHECK_IMPL
#define IsBadReadPtr(p, n) MemBad((p), (n), false)
#define IsBadWritePtr(p, n) MemBad((p), (n), true)
#define IsBadCodePtr(p) MemBad((const void *)(p), 1, false)
#endif
void ResStart();  // render resolution from fc3vr.ini
bool ResForce(HWND w, UINT *width, UINT *height);  // swapchain size for the game's window

void Log(const char *fmt, ...);
void OnlineInstall(HMODULE game);  // online1
void ScanlinesInstall(HMODULE game);  // scan1
// bd9: call a Blood Dragon thiscall function: self in ecx, n stack arguments (the callee removes them).
// In assembly on purpose: bd8 crashed because the compiler merged two casts of one pointer to
// different calling conventions into one call (the stdcall one), leaving ecx as rubbish.
extern "C" unsigned long __cdecl BdThisCall(void *fn, void *self, int n, ...);
extern "C" unsigned long __cdecl BdCdeclCall(void *fn, int n, ...);
void LogFlush();
void InputNoteShot();  // firelag1: the player fired (bonehook ShotFix)  // quiet1: write buffered log lines now (before exiting)
// trim1: diagnostics every few seconds only when [debug] VerboseLog=1 in fc3vr.ini
extern bool g_verboseLog;
#define LogV(...) do { if (g_verboseLog) Log(__VA_ARGS__); } while (0)
void PrewarmStart();  // prewarm.cpp: game data into the Windows file cache

struct IDXGISwapChain;
// Called from the Present hook with the game's finished frame, before it is shown.
void XrOnPresent(IDXGISwapChain *sc);
// Called when the game is about to resize or destroy its swapchain, so the
// headset session lets go of the game's device first (fixes crash on exit).
void XrOnSwapChainGoing(IDXGISwapChain *sc, const char *why);
void XrQuitHookInstallBd(BYTE *bd);  // quitbd1

// Patch one vtable slot, keeping the module's own original (read from disk)
// as *orig so we never chain into another tool's hook.
bool HookSlot(void **vtable, int index, void *hook, void **orig, const char *what);

struct ID3D11Device;
// Phase 3 discovery: log where shaders keep the camera matrices.

struct ID3D11DeviceContext;
// Phase 3: head tracking via the viewport constant buffer.
void HeadtrackHookContext(ID3D11DeviceContext *ctx, const char *what);
void HeadtrackOnPresent();      // after XR submit: fetch the pose for the next frame
bool HeadtrackActive();         // tracking on and patching the game's camera
void HeadtrackBeforeSubmit();   // before XR submit: pick the pose the frame was rendered with
bool XrGetHeadOrientation(float q[4]);
void XrSetGameFov(float tanHalfX, float tanHalfY);
void XrSetImageFov(int eye, float l, float r, float u, float d);  // oc1: the view one eye's image is drawn with (tangents)
bool XrGetEyeTan(int eye, float t[4]);                            // oc1: the eye's real view: tan left, right, up, down
bool *EngineOffCentreOn();                                        // oc1 trial switch (VR page)
int EngineDrawEye();                    // vrs1: eye being drawn for the headset now, or -1
bool EngineEyeOffCentreNow(int eye);    // vrs1: that eye's picture is the off-centre one
// vrs.cpp (vrs1): variable rate shading
struct ID3D11RenderTargetView;
struct IDXGISwapChain;
struct ID3D11DeviceContext;
bool *VrsOn();
int *VrsScopeOutside();  // pip1: scope picture outside the lens: 0 full shading, 1 coarse (1 per 4x4), 2 skipped
float *VrsInner();
float *VrsMid();
const char *VrsState();
void VrsOnTargets(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtv, bool swapped);
void VrsOnPresent(IDXGISwapChain *sc);
void VrsContextReset();
void VrsOnSwapChainGoing();
void VrsLog();
void EngineEyeViewport(float *x, float *y, float *w, float *h);   // oc1: viewport for our overlays in the last view's image

// Debug screenshots (F10): saves 4 frames to bin\fc3vr_shots\ and logs that frame's camera buffers.
void ShotsOnPresent(IDXGISwapChain *sc);

// Camera hunt (F11): find the engine's camera in memory and the code that writes it.   

// Last head pose fetched for rendering, and an override for the pose the next frame is submitted with.
void XrGetLastPose(float q[4], float p[3]);
void XrSetSubmitPose(const float q[4], const float p[3]);
// Stereo: the frame about to be submitted was rendered for this eye (0 left, 1 right) from head pose q, p.
void XrSetSubmitEye(int eye, const float q[4], const float p[3]);

// Engine camera hook: adds the head rotation to the game's render camera itself.
bool EngineInstall();                       // once, after the game is up
void EngineSetHead(const float m[16], const float off[3], const float q[4], const float p[3]);  // head rotation and offset (eye space) + raw pose
bool EngineActive();                        // hook installed and recently used
void EngineOnPresent();  // once per shown picture: hotkeys, submit pose

// The headset's field of view (tan of half-angles, symmetric) and eye separation in metres.
bool XrGetHeadsetFov(float *tanX, float *tanY);
bool XrSessionRunning();  // mirror1
extern int g_mirrorEvery;  // hook.cpp: desktop window shows 1 in N frames
float XrGetIpd();

// Touch controllers, as read each frame (sticks -1..1, triggers/grips 0..1, hand aim poses in the XR space).
struct VrPad {
    bool active;
    float lx, ly, rx, ry, lt, rt, lgrip, rgrip;
    bool a, b, x, y, menu, lclick, rclick;
    bool handValid[2];
    float handQ[2][4], handP[2][3];
};
bool XrGetPad(VrPad *out);
bool *VrLeftHanded();
void GunAttachAxesW(float out[9]);  // lefty2: the gun's right / forward / up (world), last HandFrame
bool *HandsMirrorArms();  // lefty2: [vr] LeftHandedMirrorArms  // lefty1 (xr.cpp): [vr] LeftHanded: the controllers swap roles (gun on the left)
// comfort1 (comfort.cpp): comfort vignette, darkens the edges of view while moving / turning
struct ComfortCfg { bool on; float strength, size, fadeIn, fadeOut; int mode; float colour[3]; bool preview; };
ComfortCfg *Comfort();
void ComfortLoad();
void ComfortSave();
void ComfortDraw(IDXGISwapChain *sc);
bool *InputSwingOn();      // swing1: swing the right hand to melee
float *InputSwingSpeed();  // m/s
bool *InputCrouchOn();  // crouch1
void InputStart();  // the game reads the Touch controllers as an Xbox pad

// True stereo work: view registry probe (logs once; Numpad 5 test).
void StereoOnPresent();

// Frame trace (Numpad 8): logs one frame's render targets, draws and camera builds.
void TraceHookContext(ID3D11DeviceContext *ctx);

// Render-twice stereo: force the eye the engine camera is turned for (-1 = alternate),
// read back the eye and pose of the camera last built, and keep the left image.
void EngineForceEye(int eye);
bool EngineLastEye(int *eye, float q[4], float p[3]);
bool XrHoldLeftEye(const float q[4], const float p[3]);
bool StereoLeftPresent();  // render-twice: this Present is the left eye; kept for the headset, not shown
void StereoNoteXrTime(double ms);  // time spent in the headset submit this Present
void StereoBeforeXr();  // just before the headset submit: ends the right eye's GPU timing
bool EngineGameLook(float *yaw, float fwd[2], float *pitch);  // the game camera's own yaw and forward x/y, if fresh   
void BoneHookInstallBd(unsigned char *fc3);  // bd4
void BoneHookLog();
bool HeadtrackHand(int hand, float m[16], float off[3] = nullptr);  // hand aim rotation (+ offset from the recentred head), recentred frame
bool HeadtrackHandRaw(int hand, float m[16], float off[3]);  // twohand1: without the two-handed turn
// twohand1 (twohand.cpp): left grip on the front of the gun = aim along the line between the hands
bool TwoHandInput(const struct VrPad &p);  // input thread; true = the left grip press is ours
void TwoHandApply(float m[16], const float offR[3]);
bool TwoHandHeld();  // both grips: two-handed, or hands together (pistolgrip1)
float *TwoHandCupReach();  // pistolgrip1: m, 0 = off
// steady1: pistols steadied with the hands together (no grip), aim unchanged
bool TwoHandSteady();             // hands together with a pistol right now
bool *PistolSteadyBuzz();         // steadybuzz1
float TwoHandSteadyBuzz();
bool *HandsPistolAuto();          // hands25
bool *HandsNeutral();
bool *HandsGameSigns();      // sign1: [hands] GameFingerSigns
bool *HandsGameArmsEmpty();  // emptyarms1: [hands] GameArmsEmptyHanded
bool HandsGameArmsNow();     // emptyarms1: the game's own arms are being shown now (no weapon in the hand)
float *CyberEyeSmoothPtr();       // cybermenu1
float *CyberEyeWidthPtr();
float *CyberEyeDistPtr();
float *CyberEyeScanPtr();
void CyberEyeSave();
bool *CyberEyeLevel();            // cyberlevel1
bool *HolsterHeartAdjust();       // heartadj1
bool HolsterHeartAdjHold();
bool HandsC4Charge(float *out);   // c4rigid1
void HandsC4Relearn();
float *HandsC4Obj();              // c4turn1
bool HandsC4Learnt();
float *ScopeCorner();             // square2
float *ScopeCrt();
float *ScopeFringe();
void PistolSteadyFilter(float m[16]);  // headtrack.cpp, after TwoHandSmooth
bool *PistolSteadyOn();
float *PistolSteadyHz();
float *PistolSwing();
int *PistolGunOwn();              // gun in hand: -1 by name, 0 no, 1 yes
bool PistolGunByName(const char *n);
bool *TwoHandOn();
bool *TwoHandStockAim();             // stockaim1: while two-handed, the right controller aims (gunstock)
void TwoHandSmooth(float m[16]);     // twosmooth1: headtrack.cpp, after BowApply / TwoHandApply
float *TwoHandSmoothing();           // global, 0..1
const char *TwoHandSmoothGun();      // weapon the per-gun value belongs to
float *TwoHandSmoothGunValue();      // -1 = use the global value
void TwoHandSmoothSave();
// bow1p (bow.cpp): physical bow in two-handed mode
bool BowInput(const struct VrPad &p, bool *rt);  // input thread; true = bow owns the right trigger / grip
void BowApply(float m[16], float off[3]);         // HeadtrackHand(1): the bow hand (left), turned to the arrow line
bool BowActive();
int BowState();  // -1 off, 0 no arrow, 1 arrow in hand, 2 nocked
float BowDraw();
int BowArrowMode();  // gunattach: 0 game, 1 hidden, 2 right hand, 3 on the string at the right hand
float BowBuzz();
float BowBuzzLeft();
float *BowNockTick();
void BowInstallBd(BYTE *bd);        // bowbd1
void BowStringInstallBd(BYTE *bd);  // bowbd1
// hands1 (hands.cpp): probe of the player's arm bones, Blood Dragon
extern "C" {
extern BYTE *volatile g_handsComp;
}
void HandsInstallBd();
void HandsSkin(BYTE *comp);  // from the skin stub
bool *HandsEmptyTracked();                       // emptyhands1: [hands] EmptyHandsTracked
bool *HandsEmptyAnywhere();                      // emptyhands5: [hands] EmptyHandsAnywhere
bool *HandsEmptyGameAnims();                     // emptyhands5: [hands] EmptyHandsGameAnims
float *HandsEmptyGameAnimCm();                   // emptyhands5: [hands] EmptyHandsGameAnimCm (metres in memory)
bool *HandsKillstarGrip();                       // killstar1: [hands] KillstarGrip
bool HandsEmptyWanted();                         // emptyhands1
void HandsGunSeen(const char *name);             // emptyhands4: gunattach, a new gun in the hand
void HandsFrameEmpty(BYTE *attComp, BYTE *playerEnt);  // emptyhands1
void HandsFrame(BYTE *attComp, BYTE *playerEnt, int gunBone, const float *ourGun);  // gunattach
void HandsC4(const float *gameCharge, const float *ourCharge);  // hands23: C4 in both hands, the charge as the game had it and as we placed it
float *HandsC4Adj();           // hands24: 6: right, up, forward (m), pitch, yaw, roll (deg)
bool *HandsPistolGrip();       // hands24: left grip near a pistol locks the left hand to it
float *HandsPistolReach();
bool HandsPistolLocked();
int HandsPistolPlace(int op);  // 0 query, 1 save where the hand is now, 2 forget
int HandsPose(int slot, int op);  // slot 0 C4, 1 pistol; op 0 query, 1 capture now, 2 forget
bool *HandsPoseUse(int slot);
bool HandsC4RelearnDue();   // c4learn2
void HandsC4Mirror(bool m); // c4mirror1
void HandsPoseIn(int slot, float seconds);  // capture2
float HandsPoseLeft(int slot);
float HandsBuzz();
// gripfwd1: the left hand's place on the gun in hand (both grips), per gun ([gun.<name>] LeftHand*)
float *HandsGunLeft();             // 6: right, up, forward (m, the gun's axes), pitch, yaw, roll (deg)
const char *HandsGunLeftGun();     // the gun they belong to
void HandsGunLeftTouched();        // changed from the menu: save it
void HandsGunLeftReset();          // back to the gun's default
void HandsGunLeftIn(float seconds);  // take the place from the left controller in so many seconds (0 = cancel)
float HandsGunLeftLeft();
bool HandsGunLeftOnGun();
const float *HandsGunLeftSeen(int mine);  // cm right, up, forward of the gun's point: 0 the game's wrist, 1 the controller's
bool *StereoWakeFast();  // wakefast1
bool *LaserBeamBehind();    // occl4
bool *LaserDotBehind();
bool *EngineScopeGunOff();  // noscope1
bool *EngineLadderTurn();   // ladder1
bool *HandsZipOn();         // zip1: the zipline left hand
bool *HandsZipPreview();
bool *HandsZipHookGame();
float *HandsZip();
bool HandsZipNow();
DWORD EngineInputBase();   // emptyhands2: the oldest pushed input mode
DWORD EngineInputMode();
bool HandsTakeDirty();         // menu: something was saved from the game thread, write the ini
void HandsControllers(const float *FL, const float *pL, const float *FR, const float *pR, float scale, bool twoHand);  // hands3
float *HandsSmooth();  // hands6: [hands] Smoothing (s)
float *HandsBlend();   // hands6: [hands] Blend (s)
void HandsHead(const float *eye, const float *axes, bool scoped, const float *cam, const float *fwd);  // hands4 / hands13: view position, level axes, zoomed, camera, view forward
float *HandsWristBend();  // hands15
float *HandsFreeBow();  // hands5: the same six values for the arrow hand with the bow
float *HandsFree();  // hands3: free hand placement: right, up, forward (m), pitch, yaw, roll (deg), written for the LEFT hand
float *HandsElbowOut();
bool *HandsElbowGun();  // hands11
float *HandsBowElbow();  // 3 floats, [hands] BowAxisHigh / Side / Back
bool BowStringHandRestKnown();  // hands10: the hand arrow's rest on the bow is learnt or loaded
void HandsRestore();  // hands7: gunattach, before the game reads the player's bones
bool HandsLive();    // hands2: the arms were moved to the gun in the last 120 ms
bool *HandsOn();     // [hands] On
bool *HandsLeft();   // [hands] LeftHand
bool *HandsFreeLeft();  // hands3: [hands] FreeLeft
bool *HandsScopeHide();  // hands4: [hands] HideLeftScoped
bool *HandsElbow();      // hands4: [hands] FreeElbow
bool *HandsChest();     // hands3: [hands] HideShoulders
void HandsSave();
bool BodyAnyShown(); // hands2: at least one saved body part is ticked to be shown
BYTE *BowStringCompBd(BYTE *ent);
bool *BodyShow(int i);  // hands1: draw i of the saved body parts is shown again
void BodyShowChanged();
// string10 (bowstring.cpp): the bow's string and nocked arrow follow the right hand
bool *BowStringOn();           // [twohand] BowString
const char *BowStringStatus();
void BowStringRelearn();       // drop the record; the next full draw is recorded again
bool BowStringLearning();      // the next full draw runs on the game's own clock and is recorded
bool BowStringDriving();
void BowStringGame(BYTE *strategy, float amount);       // bow.cpp, after the game's bow update
bool BowStringGeo(float *k, float *pull);               // input thread: draw 0..1 and pull (m) from the string's path
void BowStringFrame(BYTE *bowEnt, const float *M, const float *rightHand, const float *RF, const float *leftHand, float scale);  // gunattach
bool BowStringArrow(int kind, float loc[9], float off[3]);  // gunattach: true = arrow goes on our string
DWORD BowClockMs();
DWORD BowNockMs();
bool BowRtHeld();
bool *BowPhysicalOn();
float *BowSteerStart();  // bowfeel1
float *BowSteerRange();
float *BowSteerWeight();
float *BowKeepAngle();
bool BowClassic();  // bowtrig1: the bow is out without the motion controls (right hand, trigger)
bool BowOut();  // the bow is in hand and the physical bow is on (two-handed holding stays off it)
int HolsterLeftSpot();          // bow1p: weapon spot the left hand is on (-1 none)
void HolsterTakeSpot(int spot);  // bow1p: switch to that spot's weapon
int *BowArrowSpot();
int BowQuiverSpot();    // spot1: the bow's own spot 0-3 (arrows only there)
int HolsterBowSpot();   // spot1: the bow's spot from the game's weapon slots (-1 not known)
float *BowNockReach();
float *BowFullDraw();
float *BowArrowAhead();
float *BowArrowLength();
float *BowDropAmount();  // bow4p: bow pitch-down after the shot (degrees)
float BowDropNow();
bool *BowPreview();       // bow6p: bow left / arrow right while the debug menu is open
float *BowHandPose();     // bow6p: bow in the left hand: right, up, forward (m), pitch, yaw, roll (deg)
float *BowArrowPoseFirst();  // arrowsets1: the same for the bow's own first arrow
int GunAttachArrowKind();    // arrowsets1: the arrow carried in the hand now: 0 none, 1 the first arrow, 2 a later one
float *BowArrowPose();    // bow6p: arrow in the right hand, same order
bool *BowHandPoseSet();
float *BowNockAhead();  // bow7p: nocked arrow moved along itself (m)
bool *BowPreviewNocked();  // bow7p
float *BowNockHands();     // bow12p
float *BowUnnockAngle();   // unnock1: deg, 0 = never
bool BowCancelTap();       // unnock9: input.cpp holds the game's reload (pad X) to cancel a draw
bool *BowUnnockCancel();   // unnock2: un-nocking may call off a draw the game has started
float *BowDropIn();
float *BowDropBack();
float *BowReturnTime();
void BowPlayRelease();
void BowSetLine(const float p[3], const float a[3], bool hand);  // bow14p; quiver4: hand = an arrow taken by hand (not the bow's own)
bool BowArrowsAlongAim();
float *BowZeroUp();       // quiver5: bow zeroing (deg): arrows up
float *BowZeroRight();    // quiver5: arrows right
void BowZeroWorld(float d[3]);  // quiver5: turn a game-world aim direction by the zeroing
void BowZeroXr(float m[16]);
float BowSteerBoost();
bool *BowNoSpread();
float *BowSteerBoostPtr();
bool *BowArrowsAlongAimOn();
bool *BowAimAlongArrow();                // aimfix1: bow aim follows the arrow on the bow
void BowLoadArrowLine();               // aimfix1: menu load
bool BowAimFix(float C[9]);              // aimfix1: the local turn aim = bow frame * C (gunattach takes it off)
float BowDipCurve();  // bow15p
float *BowDipDown();
float *BowDipForward();
float *BowFirstArrowShift();
float *BowNockPoint();        // bow7p: how far the hand has drawn since the nock (m)      // gunattach: current drop (degrees)  // bow4p: arrow model length (its origin is the tip)
float *TwoHandReach();
float *TwoHandGunReachSetting();  // grabreach1: per gun reach to the side (m), -1 = the gun's default
float TwoHandGunReachDefault();
float TwoHandGunReach();
float *TwoHandFar();
struct EngineView {
    float pos[3], fwd[3], up[3], base[3], basis[9], posAxes[9], scale;
    bool sixdof;
};
bool EngineLastView(EngineView *v);  // the view drawn last (laser.cpp)
// scope1: scoped aiming (engine.cpp) and the scope lens (scope.cpp)
bool EngineScoped();          // the game is zoomed (scoped gun aimed)
bool EngineScopeEyeActive();
bool InputWheelHeld();  // weapon wheel (left grip) held
bool InputAimHeld();  // scope9: left trigger (the game's aim) held  // the right eye is the scope view now
bool *EngineScopeOn();
// pip1: scope picture as a third render ([aim] ScopeStereo); the scope view's eye number (2 with it, 1 without);
// whether the headset shows the left picture to both eyes now; whether stereo.cpp should draw the scope picture.
bool *EngineScopePip();
int EngineScopeEye();
bool EngineScopeMono();
bool EngineScopePipWanted();
bool EngineScopePictureActive();  // autoscope1: the scope picture is wanted (trigger held, or a rifle scope at the eye)
bool *EngineScopeNarrow();        // narrow1: [aim] ScopeNarrow
float EngineScopeDrawnZoom();     // narrow1: the lens zoom the scope picture was narrowed by (1 = not)
float *EngineScopeSmoothing();
float EngineScopeFov();
bool EngineScopeRay(float pos[3], float dir[3]);  // scope3: scope view origin and centre
float *ScopeReticle();
float *ScopeReticleOpacity();
bool *ScopeMatchLaser();  // match1
int *ScopeStyle();          // scope7: reticle style
bool *ScopeSquare();  // square1: per gun, a square lens turned with the gun
float *ScopeScan();   // square1: scan lines in the lens, 0-1
float *ScopeTint();   // square1: orange tint in the lens, 0-1
void ScopeSave();           // scope7: save this gun's scope settings
bool *ScopeHideHud();       // nohud1: game HUD left out of the scope image
float *ScopeDistortion();   // lens1: slight lens distortion 0-1 (all guns)
void ScopeZoomStep(int dir);  // zoom1: A / B while scoped, +1 in, -1 out
bool *InputScopeZoomButtons();  // zoom1
void MenuSaveLater(void (*fn)());  // zoom1: run a module's save on the menu's save worker
bool EngineCrouched();            // crouchsteady1: on foot, crouched (game camera low)
float *EngineScopeCrouchSteady(); // crouchsteady1: x steadiness while crouched
bool *EngineScopeClassic();       // crouchsteady1: pre-2 Nov plain average
bool *LaserOffWithoutGun();  // nogun1
bool *LaserDotOnly();        // dotonly1
bool *LaserBehindObjects();  // occl2 (from the Far Cry 3 mod): beam and dot go behind things in front of them
void LaserNoteGameDepth(struct ID3D11DepthStencilView *dsv);
void LaserEyeDone();
bool EngineDepthTerms(float *a, float *b);
bool *LaserTurretDotOnly();  // lasers1: mounted guns show just the dot ([vr] TurretDotOnly)
float *LaserTurretDotRange(); // lasers1: how far the mounted gun's dot reaches (m)
bool *LaserThrowOn();         // lasers1: short fat stub while throwing a grenade / molotov
float *LaserThrowValue(int i);  // 0 length (m), 1 width (x), 2 opacity
bool InputThrowAiming();      // lasers1: a grenade / molotov throw is being held now
void EngineAfterLoadRecentre();  // loadface1: headtrack, the recentre after a load was just made
bool InputStickUpHeld();  // dice1: the right stick held up (the D20 dice throw)
int *InputXMode();            // lasers1: X on foot with a gun: 0 grenade (old), 1 minimap on/off, 2 game map (map1x)
bool *InputMenuMinimap();  // wrist1m: [buttons] MenuMinimap (Menu = the game's map, left trigger / grip + Menu = pause)
bool *InputLeftGripWheel();   // map1x: left grip opens the weapon wheel on foot
bool *InputMoveByLeftHand();  // lasers1: the left stick walks where the left controller points
bool *HudMinimapShown();      // lasers1: minimap panel shown (X toggles it)
void HudMinimapToggle();
int *LaserThrowColour();      // lasers2: throw stub colour 0 red 1 green 2 blue 3 orange
bool *LaserGunOff();
bool *LaserShowBeam();         // sight5: laser beam on, per gun and optic ([sight.x] ShowBeam)
bool *LaserShowDot();          // sight5: red dot where it lands, per gun and optic ([sight.x] ShowHitDot)
bool *LaserSightOn();          // sight1: dot only through the sight window, per gun ([gun.x] SightDot)
int *LaserSightShape();        // sight1: 0 circle, 1 rectangle
float *LaserSightValue(int i); // sight1: 0 right, 1 up, 2 forward, 3 width, 4 height (m), 5 edge fade start
int *LaserSightReticle();      // sight3: 0 dot, 1 holo ring, 2 chevron, 3 cross, 4 triangle
float *LaserSightMoa();        // sight1: [aim] SightDotMoa
bool *LaserSightOutline();     // sight1: show the window edge (not saved)
float *LaserDotBright();      // lasers3: dot brightness, per gun ([gun.x] DotBrightness)
float *InputTriggerSens();    // lasers3: right trigger sensitivity ([input] RightTriggerSensitivity)
float *EngineScopeAhead();    // lasers2: scope camera this far ahead of the hand (m), [aim] ScopeCameraAhead          // lasers2: no laser for the gun in hand (per gun)
bool HolsterThrowing();       // lasers1: a chest grenade / molotov is held (grip down)
const wchar_t *ScopeGun();  // scope7: the gun the scope settings are for  // scope3: reticle offset right, up (lens radii)
bool ScopeIsScopeImage();
float ScopeUsedRadiusPx(UINT h);  // pip1: radius of the scope image's part the lens shows, pixels
// rifle1: the scope sits on the rifle (per gun, [scope.<gun>] RifleScope): its picture is seen through the
// eyepiece glass on the gun, with an eye box. Values: 0 right, 1 up, 2 forward (m, controller frame), 3 glass
// diameter (m), 4 eye box radius (m), 5 eye distance (m), 6 eye distance range (m).
bool ScopeRifleOn();
bool *ScopeRifle();
float *ScopeRifleValue(int i);
bool *ScopeRifleShowGlass();
bool *ScopeRifleAuto();     // autoscope1: [scope.<gun>] RifleAuto
bool ScopeRifleAutoNow();   // autoscope1: a rifle scope with RifleAuto is at the eye now
float *ScopeAutoFovRef();   // steadyonly1
bool *ScopeRifleSteadyOnly();
bool ScopeSteadyOnlyNow();
bool EngineScopeSteadyOnly();
float ScopeRifleDefault(int i);      // autoscope2: default of ScopeRifleValue(i)
bool ScopeRifleAtEye(float now[3]);  // autoscope2: at the eye now; now = cm behind the glass, cm off axis, deg off the barrel
float ScopeAutoFov();       // autoscope1: the game zoom (radians) learnt for this gun
void ScopeCapture(IDXGISwapChain *sc);
void ScopeDraw(IDXGISwapChain *sc);
float *ScopeSize();
float *ScopeZoom();
void ScopeOnSwapChainGoing();
void LaserDraw(IDXGISwapChain *sc);
float *LaserLength();   // beam reach (m)
float *LaserStart();    // beam start, m from the hand
float *LaserFade();     // beam fade-in length, m
float *LaserWidth();    // beam width scale
float *LaserOpacity();  // beam opacity 0..1
float *LaserOffset();   // beam aim offset, degrees right, up
void LaserOnSwapChainGoing();
long LaserDrawn();
bool BoneHookAiming();
float *BoneHookHipSpread();  // hip-fire spread, fraction of the game's (spread1)
bool *BoneHookNoPeek();  // nopeek1: no lift over cover when firing
void BoneHookPeekApply();
bool *BoneHookShotFromHand();  // shot1: player shots start at the beam and follow it
DWORD StereoRenderThread();
void StereoCrashState(char *out, int n);  // crash4
void StereoGpuMark(const char *name);  // gpu3: timestamp a pass boundary (render thread)
bool StereoGpuProfiling();
int *StereoWakeKeep();    // fast1/wake3: wake a sleeping worker only while fewer than this are awake (0 = off)
bool *StereoFastCheck();  // fast1: quick stand-in for the game's IsBadWritePtr over locked buffers
bool PatchImport(HMODULE mod, const char *dll, const char *name, void *hook, void **orig);  // res.cpp
void StereoGpuProfile();  // 10 s GPU-by-pass profile into the log
DWORD EngineGameThread();  // shots follow the right controller  // laser.cpp: aim beam from the right hand, before each eye's Present
bool EngineAimBasis(float r[3], float u[3], float f[3], float camFwd[3]);  // level on-foot camera basis + its forward  // every 10 s: who reads the player's bones  // Numpad 1: name the code that reads the right stick's axis values  // Numpad 2: find the player's look direction in memory
bool EngineOnFoot();  // the on-foot game camera ran in the last 200 ms
bool EngineInVehicle();
bool *EngineRoomScale();  // room1
void EngineUserMoved();   // your own left stick moved (room1)
bool EngineRoomStick(SHORT *x, SHORT *y);  // room1: the left stick that walks the body to the head
void HeadtrackShiftAnchor(float x, float z);
float *HeadtrackHandSmoothing();
float *EngineRoomStart();  // room2: how far the head gets from the body before it walks (m)
// body1 (body.cpp): hide the player's own body draws
void BodyHookContext(ID3D11DeviceContext *ctx);
void BodyPresent(bool in);
void BodyTick();
const char *BodyStatus(int *left);
int BodyPhase();
void BodyStartHunt();
void BodyAnswer(bool gone);
void BodyCancel();
bool *BodyHideOn();
void BodyHideChanged();
int BodySaved();
void BodyClear();
bool EngineAimHeadLive();  // Numpad 1: on foot, the game is steered to look where the head looks
void EngineSteerStick(SHORT *x, SHORT *y);  // lookaim1: the right stick that steers the game look
void EngineSetRightX(SHORT x);  // the pad's right stick x, which turns the body while aiming with the head   
bool EngineHorizonLock();  // Numpad 7: game camera kept level, right stick up/down dropped
const char *TraceLastTargets();  // gpu3: the last render targets set (name)
ID3D11DeviceContext *TraceContext();  // the game's immediate context
struct ID3D11DepthStencilView;

// ---- Debug menu (menu.cpp) and the settings it changes ----
void MenuDraw(IDXGISwapChain *sc);  // into this eye's image, before its Present
void MenuOnSwapChainGoing();
bool MenuOpen();                     // input.cpp: the controllers drive the menu, not the game
void MenuInput(const struct VrPad &p);
enum { kOptStereo, kOpt6dof, kOptVrFov, kOptHorizon, kOptCamera, kOptGunFov };
float EngineWorldScale();
void EngineSetWorldScale(float v);
bool EngineGetOpt(int which);
void EngineSetOpt(int which, bool on);
int EngineLookMode();
void EngineSetLookMode(int m);
float *EngineSteerTuning();
bool *EngineSnapTurn();      // snap1: snap turn on the right stick ([vr] SnapTurn)
float *EngineSnapAngle();    // snap1: degrees per snap ([vr] SnapTurnAngle)
void EngineSetRightY(SHORT y);
void EngineSetLookRates(float yaw, float pitch);  // look3
float EngineSteerError();
void StereoFrameTimes(float out[5]);
void StereoScopeTimes(float out[2]);  // pip1: scope picture CPU, GPU (ms, smoothed)
bool StereoScopePassNow();  // pip1: the scope picture is being drawn now
bool StereoTwice();
void StereoSetTwice(bool on);
bool LaserOn();
bool LaserRay(float hand[3], float dir[3], float raw[3]);  // shot1: the beam in the game world (raw: no offset)
bool LaserRay(EngineView &v, float hand[3], float dir[3], float raw[3]);
void LaserSetOn(bool on);
bool BoneHookShotsOn();
void BoneHookSetShots(bool on);
void HeadtrackRequestRecenter();
bool *EngineZipRecentre();  // ziprec1
bool *EngineKeepView();     // keepview1: [vr] KeepViewOnReturn
bool *EngineVehRecentre();  // vehrec1: [vr] VehicleRecentre
bool *EngineVehExitKeep();  // vehface1: [vr] VehicleExitKeepFacing
bool *HeadtrackAutoRecenter();
void HeadtrackOwnDraw(bool on);  // our own drawing on this thread: context hooks leave it alone
bool HeadtrackOwn();
struct ID3D11RenderTargetView;
// hud1 (hud.cpp): the game's HUD and pause menu on a panel in front of you.
ID3D11RenderTargetView *const *HudTargets(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                          ID3D11RenderTargetView **tmp);
void HudDraw(IDXGISwapChain *sc);
void HudOnSwapChainGoing();
void HudLoad();
void HudSave();
bool HudOn();
void HudSetOn(bool on);
bool HudShown();
void HudSetShown(bool on);  // shortcut
float *HudDistance();
float *HudSize();
float *HudHeight();
float *HudOpacity();
int *HudFirstPass();
int *HudHidden();  // 3x3 grid of HUD areas, bit per cell
// map1 / ammo1: parts of the HUD on panels of their own that face you
// (0 minimap with health, 1 ammo). Anchor 0 head, 1 body, 2 left wrist, 3 right wrist.
enum { kHudPanels = 2 };
struct HudPanelCfg {
    bool own;
    float rect[4];  // x0 y0 x1 y1 of the HUD image
    int anchor;
    float size;     // width, metres
    float off[3];   // right, up, ahead (wrist: right, up, back)
    float opacity;
    bool round;
    // wrist1 (DrGonzaga 12 Nov): on a wrist, fixed to the controller like a watch face
    // instead of turning to face you; tilt/turn in degrees from facing straight back
    // (tilt + = towards the controller's top, turn + = towards its right side).
    bool fixed;
    float tilt, turn;
    bool glance;      // only shown while you look at it and it faces you (fades in/out)
    float glanceDeg;  // how directly you look at it (degrees off)
    float turnDeg;    // glance2 (DrGonzaga 20 Nov): how far the wrist must turn the panel to you (degrees off)
};
HudPanelCfg *HudPanel(int e);
float HudPanelFade(int e);               // ammo2: glance fade of an own panel, 0 when not drawn
bool HudPanelPlace(int e, float c[3]);   // slowbtn1: where it sits now, drawn or not
bool HudPanelCentre(int e, float c[3]);  // ammo2: its centre (recentred XR frame)
// ammo2 (ammo.cpp): rounds in the gun and spare rounds as the mod's own numbers at the ammo panel's place
void AmmoLoad();
void AmmoSave();
void AmmoGameTick(unsigned char *pawn);
void AmmoDraw(IDXGISwapChain *sc);
bool AmmoShowing();
int StereoEyeNow();       // onehud1 (stereo.cpp)
bool *HudOneForBothEyes();  // onehud1 (hud.cpp): [hud] SameHudBothEyes
bool *HudMainFar();          // farquest1 (hud.cpp): [hud] QuestIconFar, the quest icon's patch drawn with no eye offset
float *HudQuestPatch();      // farquest1: half width, half height of the patch
bool MarkersQuestPlace(float *u, float *w, float *dist);  // farquest1 (markers.cpp): where the quest icon was put on the HUD picture just now
bool *HudMovieBlack();       // movie1 (hud.cpp)
float *HudMovieSize(int i);  // movie1: 0 width, 1 distance (m)
bool *HudAmmoKeep();     // ammokeep1 (hud.cpp): [hud] AmmoKeepPicture
int HudAmmoKeepState();
bool *AmmoOwn();
bool *AmmoSpare();
float *AmmoSize();
int AmmoClipNow();
int AmmoSpareNow();
void HudPanelOutline(int e, bool on);  // yellow box round that part while lining it up
void HudPanelDefaults(int e, int anchor);
void HudPanelReset(int e);
struct HudHideBox {  // hide1: a part of the HUD image never shown (crouch icon)
    bool on;
    float rect[4];
};
HudHideBox *HudHideBoxCfg();
void HudHideBoxOutline(bool on);  // cyan box round it while lining it up
HudHideBox *HudKeepBoxCfg();       // keep1: part of the minimap box kept on the main panel
void HudKeepBoxOutline(bool on);   // green box round it while lining it up
bool GamePaused();  // ammo1 (markers.cpp): the game's own IsGamePaused
// Button mapping (input.cpp): each Touch input sends a chosen Xbox button.
int InputMapCount();
const char *InputMapName(int i);
int *InputMap(int i);
int InputOutputCount();
const char *InputOutputName(int o);
void InputMapDefaults();
void InputMapLoad();
void InputMapSave();
bool *InputGripHoldPtr();  // grip1
bool EngineHeadPose(float m[16], float off[3]);  // head rotation and position (recentred XR space)

// gfx.cpp: graphics settings for the menu.
struct GfxProfileSetting {
    const char *key, *label;
    const char *values[8], *names[8];
    int count;
};
extern const GfxProfileSetting kProfile[];
extern const int kProfileCount;
void GfxStartup();
const char *GfxProfileNow(int i);
void GfxProfileChosen(int i, char *out, int len);
void GfxProfileChoose(int i, const char *val);
int GfxVarCount();
const char *GfxVarName(int i);
int GfxVarType(int i);
int GfxVarFind(const char *name);
bool GfxVarBool(int i);
float GfxVarFloat(int i);
void GfxVarSetBool(int i, bool on);
void GfxVarSetFloat(int i, float v);
void GfxApplyLive();
void GfxResetLive();
bool GfxBloomAvailable();
bool GfxBloomOn();
void GfxSetBloom(bool on);
void ResGetSize(UINT *w, UINT *h);
void ResSetSize(UINT w, UINT h, bool now);
// smaa1: SMAA 1x on each eye's image (smaa.cpp); 0 off, 1 low .. 4 ultra
extern int g_smaa;
struct ID3D11Texture2D;
void SmaaCopy(ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst);
void SmaaOnSwapChainGoing();
bool XrRecommendedSize(UINT *w, UINT *h);  // the runtime's recommended size per eye   
void GunAttachInstallBd(BYTE *bd);  // bd5
bool *GunAttachScopeShowGun();       // scopegun2: [aim] ScopeShowGun
BYTE *GunAttachPlayer();  // car4: the player entity, or null before the gun parent is found
float *GunAttachOffset();
float *GunAttachAngle();
void GunAttachLeftFromRight();  // lefty3
bool GunAttachLeftValues();
void GunAttachRelearn();  // learn the gun's turn again from the game's own pose   // pitch, yaw, roll (degrees)
float *GunAttachCamera();  // camera offset right, up, forward (m)
void GunAttachLoad();
void GunAttachSave();  // also the current weapon's [gun.<name>] (gunid2)
const char *GunAttachOptic();  // sight2: the fitted optic's name, "iron" if none
const char *GunAttachName();  // gunid2: current weapon ("AK47"), empty before the first
bool GunAttachNoAmmo();       // noammo1: the Killstar (runs on health): no ammo panel
bool GunAttachNoLaser();      // nolaser1: syringe / other props / cyber heart in the hand: no beam, dot or throw stub
DWORD GunAttachHandTick();     // car9: last tick a weapon was in the player's hand (bone 51 / 74)  
bool GunAttachC4Split();       // c4hands1: C4 out: charge on the left controller, aim from the left hand
bool *GunAttachC4SplitOpt();   // [gun] C4BothHands
float *GunAttachC4Offset();    // [gun] C4Charge*: charge from the left controller (right, up, forward m)
extern bool g_soundHead;

// rumble1 (input.cpp): the game's pad rumble, 0..1 per motor
void InputRumble(float *heavy, float *light);
float InputRumbleStrength();
float *InputRumbleStrengthPtr();
bool *InputRumbleRealPadPtr();
// loot.cpp
extern bool g_skipLoot;
// tod1: time of day (tod.cpp)
void LootInstallBd(BYTE *bd);
void MarkersInstallBd(BYTE *bd);
// dmg1: damage the player takes (damage.cpp)
extern float g_damageTaken;
void DamageInstallBd(BYTE *bd);  // dmgbd1: Blood Dragon player damage hook
// mark1: hide the quest objective icon / the map-marker icon (live switches).
extern "C" volatile BYTE g_hideObjIcon, g_hideMarkIcon;
extern bool g_objHead;    // objhead1 (markers.cpp): [hud] QuestIconByHead
extern float g_objScale;  // [hud] QuestIconScale
void HudMainPanel(float *dist, float *w, float *h, float *height);  // objhead1: the head-locked HUD panel (metres)
extern "C" volatile BYTE g_mapHead;  // headmap1: minimap turns with the head
void LootApply();
extern bool g_outpostAnytime;  // outpost.cpp
void OutpostInstallBd(BYTE *bd);  // outpost2: Blood Dragon
void OutpostApply();
bool OutpostAvailable();

extern float g_lodFix;  // lod1, engine.cpp
extern float g_killScale, g_detailScale, g_zoomScale, g_minSizeScale;  // bd71 kill2, engine.cpp
extern DWORD g_lodScaleTick;

// stock1: gunstock accuracy (stock.cpp)
struct StockGun { float nearM, farM, best, worst; };
extern bool g_stockOn;
float StockSpread(float game, bool ads);
StockGun *StockSettings();
const char *StockGunName();
void StockSave();
float StockLiveDistance();
void StockLast(float *dist, float *spread, float *game);
bool EngineInVehicle();
bool GunAttachPose(float m[16]);  // scope12
// kick1 / laser2 (2 Nov): hip-fire recoil and per-gun beam settings
float *GunAttachKick();     // hip recoil of the current gun: up, back arm out (deg, m), direction (deg), up, back close
float *StockAdsSpeed();     // ads1: per gun, x walking speed with the gun at the face
void StockTick();           // ads1: render thread, once a frame
bool StockGunAtFace();      // ads1
float *StockAdsDrop();      // adsmove1: m below the eye the controller may be and still count as up
float StockAdsHeight();     // adsmove1: controller height minus head height now (m)
void PromptsInstallBd(BYTE *bd);  // prompts3
bool *PromptsHideOn();
void PromptsChanged();
bool *EngineRealCrouch();   // realcrouch1
float *EngineRealCrouchDepth();
// cybereye1 (cybereye.cpp): left hand at the head, grip held = the game's binoculars on a flat screen
void CyberEyeLoad();
bool CyberEyeRecent();  // cyoff1
bool CyberEyeOn();
bool *CyberEyeHoldKeep();  // cyhold1: [cybereye] HoldToKeep
bool CyberEyeInput(VrPad &p);
bool CyberEyeDpadUp();
float CyberEyeBuzz();
float CyberEyeSmoothing();
float CyberEyeScanlines();
void CyberEyeScreen(float *w, float *d);
bool EngineCrouchPress();   // realcrouch1: input presses pad B
float EngineCrouchLift();   // crouch3: how far the view is lifted above the game's crouched camera now (m)
float EngineHeadDrop();     // m below the recentred height
float *InputCrouchCap();     // adscrouch1
float *InputSlowSpeeds();   // ads1: [0] on/off for ADS, [1] crouched
float StockKickT();         // kick2: 0 gun close .. 1 arm out (gunstock distances)
void GunAttachShot();       // a player shot, not scoped (bonehook)
void GunAttachShotDir(const float beam[3], const float shot[3]);  // aimdev1: each bullet's real direction (unit, world)
bool *GunKickFollowsShots();
bool GunKickNow(const float F[9], float R[9], float bk[3]);  // the kick now from controller frame F
void GunKickRotate(const float R[9], const float v[3], float o[3]);
void GunAttachSaveBeam();   // current gun's beam + hip recoil to [gun.<name>]
int *LaserColour();         // 0 red, 1 green, 2 blue, 3 orange
bool *LaserDotOn();  // dot1: glow where the beam hits
float *LaserDotSize();
float *LaserDotGlow();
float *LaserDotRange();  // dot3
bool *LaserStopsAtWalls();  // beam cut at the first surface (game collision ray, ray1)
void LaserGameTick();       // ray1 (engine.cpp, game thread): collision ray for walls
float *ScopeReticleTurn();  // scope12b
float *ScopeRecoil();       // recoil1: degrees per shot
float *ScopeReticleGlow();  // glow1
void EngineScopeKick();     // recoil1: a shot while scoped
extern bool g_gliderSteady;  // glide1, engine.cpp
extern bool g_cutSteady;     // cut1, engine.cpp: cutscenes turn with the headset only
bool EngineCutscene();       // cut1: the game's cinematic flag is set
// flat1 (engine.cpp): cutscenes on a flat 2D screen
extern bool g_cutFlat;        // [vr] CutsceneScreen
extern float g_cutScreen[3];  // distance, width, height (metres)
extern bool g_cutWide;        // movie1: [vr] CutsceneScreen169, a 16:9 movie screen
extern float g_cutZoom;       // movie1: [vr] CutsceneScreenZoom
extern bool g_seqBySeq;       // movie1: [vr] CutsceneBySequence, the game's own playing cutscene sequence counts
bool EngineFlat();            // this frame is a cutscene shown on the 2D screen
void EngineCutsceneMark(bool was);  // mark1: DrGonzaga's 'was / wasn't a cutscene' (logged with recent history)
LONG EngineCutsceneMarks(bool was);
// seq1 (engine.cpp): the game's input mode tells button-prompt scenes from plain cutscenes
extern bool g_seqInter3D;     // [vr] CutscenePromptsIn3D
extern bool g_seqByMap;       // [vr] CutsceneByInputMode (test)
extern float g_seqHold;       // [vr] CutscenePromptHold (seconds)
const char *EngineInputModeName(char *buf, size_t cap);  // the input mode in force, for the menu
bool EngineScenePrompt();  // qte6: a scripted scene is asking for a button press (the game's narrative input modes)
bool EngineCutsceneIn3D();    // a cutscene shown in VR because it asks for input
bool XrImageSize(UINT *w, UINT *h);  // xr.cpp: the eye image size
void XrSetMenuLayer(ID3D11Texture2D *tex, const float c[3], float yaw, float w, float h);  // xr.cpp: menu on the 2D screen
void HeadtrackRefPose(float *yaw, float pos[3]);  // headtrack.cpp: the recentred frame in headset space
LONG HeadtrackRecentreGen();                     // +1 at each recentre
// turret1 (turret.cpp): mounted guns
void TurretInstallBd(BYTE *bd);
extern bool g_turretMotion;
BYTE *TurretGunEnt();               // turacc1: the mounted gun we are on (its object), or null
bool EngineTurret();                 // on a mounted gun now (we're its gunner)
bool TurretGunPos(float p[3]);       // the mounted gun entity's position
LONG TurretOtherUsers();
bool EngineTurretAim(float pos[3], float fwd[3]);  // engine.cpp: the game camera's own place and look
extern bool g_turretSteady, g_turretLaser;
extern int g_turretRide;    // turride1: [vr] TurretRideVehicle
extern int g_turretDragon;  // turdragon1: [vr] TurretDragonRules
extern float g_turretLift;  // turabove2: [vr] TurretAboveLift
extern int g_turretAbove;   // turabove1: [vr] TurretSteadyAbove
extern int g_turretAnchor;  // turanchor1: [vr] TurretAnchor
void MovieInstallBd(BYTE *bd);  // movie1 (movie.cpp): Bink video calls watched
bool MoviePlaying();            // movie1: a story video (not a looping menu background) is playing now
extern bool g_movieComic;       // movie3: [hud] MovieForComic
extern bool g_movieOn;          // movie1: [hud] MovieScreen
extern float g_turretHeight, g_turretEye;
int VehGameInVehicle(BYTE *playerEnt);  // vehgame1: 1 in a vehicle, 0 not, -1 unknown
void VehGameForget();                      // cut1: look the player up again
void *HudTextureVtable();  // crash2: a D3D11 texture vtable (crash logger)

// holster1 (holster.cpp): weapons on body spots, drawn with the right grip
struct VrPad;
void HolsterInput(const VrPad &p);   // input thread, each pad read
bool HolsterActive();                // on foot and playing: the right grip is the holsters'
void HolsterGameTick(BYTE *pawn);    // game thread, once a frame (player's CPawn or null)
float HolsterBuzz();                 // xr.cpp haptics, right hand
BYTE *VehGameVehicleEnt();           // turride1: the vehicle object the player is in, or null
BYTE *VehGamePawn();                 // vehgame.cpp: player's CPawn (game thread)
bool *HolsterOn();
int *HolsterSlot(int spot);  // game slot 0-3 for spot 0 right hip, 1 left hip, 2 right shoulder, 3 left shoulder
bool HolsterAimPaused();     // engine.cpp: hand on a spot, look steering holds still
bool *HolsterBuzzOn();
float *HolsterValue(int i);  // 0 hip drop, 1 hip side, 2 hip forward, 3 hip radius, 4 shoulder zone behind, 5 shoulder zone lowest, 6 chest drop, 7 chest forward (m), 8 opacity, 9 hip sphere, 10 chest sphere, 11 hip grab extra reach to the side (belt1)
void HolsterLoad();
void HolsterSave();
int HolsterSpot(float *nearest);  // 0-3 hips/shoulders, 4-7 chest (8-10 were FC3's quiver), 11-16 wrist menu
const char *HolsterLast();
struct LaserMark { float pos[3]; float radius; float rgba[4]; int icon; float val;
    float bg[4] = {0, 0, 0, 0}, off[4] = {0, 0, 0, 0}; };  // numcol2: numbers' panel rgba and unlit segment rgb, used when off[3] = 1  // wrist1m: icon 8 = the number val (0-99; ammo2: to 999)
float *HolsterWristCountColour();  // numcol1: rgb 0-1
float *HolsterWristCountBg();      // numcol2: panel rgb + opacity
float *HolsterWristCountOff();     // numcol2: unlit segments rgb
int *HolsterWristCountStyle();    // numstyle1: 0 plain, 1 digital, 2 neon tube, 3 dot matrix, 4 green CRT
float *HolsterWristCountSize();   // smallnum1: the numbers' size (1 = as before)
bool *SlowmoShowSeconds();        // slowglow1: seconds left shown under the clock
bool *HolsterWristOpt(int i);      // wrist1m: 0 wrist menu on, 1 icons in a row, 2 counts shown
float *HolsterWristValue(int i);   // 0-2 right / up / back (m), 3 tilt, 4 turn, 5 glance angle (deg), 6 icon size, 7 spacing (m), 8 opacity, 9 palm-up angle (palmup1)  // holster2: icon 0 sphere, 1 grenade, 2 C4, 3 molotov, 4 mine
void LaserDrawMarks(IDXGISwapChain *sc, const LaserMark *m, int n);
void HolsterNeonMarks(LaserMark *m, int n);  // slowring1: the neon look (rings, outline icons) when [holster] neon is on
void HolsterDraw(IDXGISwapChain *sc);  // holster2: per eye, after the beam
bool HolsterThrowHeld();               // holster2: input.cpp presses the game's throw while true
// nade1 (DrGonzaga 11 Nov): grip = grenade in the hand, right trigger / teeth arms, let go throws
bool HolsterTriggerOwned(float rt);    // input.cpp: right trigger kept from the game (arming, not firing)
bool HolsterSwingBlocked();            // input.cpp: no swing melee while holding / just after a throw
WORD HolsterHeartButton();             // heart1: input.cpp: the cyber heart throw button while pressed (0 none)
bool *HolsterHeartOn();                // [holster] WristHeart
const char *HolsterSpotName(int s);    // slowmo1: holster.cpp spot names (0-16)
// slowmo1 (slowmo.cpp): slow motion from the wrist menu's clock, via the game's debug global time scale
void SlowmoLoad();
void SlowmoSave();
void SlowmoTick();          // input thread, each pad read (holster.cpp)
void SlowmoToggle();        // holster.cpp: right grip on the clock
// slowbtn1: the clock as a floating button by the minimap (left grip), buzzes at start and end
bool SlowmoOnWrist();       // the clock is on the wrist menu ([slowmo] Place=1)
bool SlowmoByMap();         // the clock floats by the minimap ([slowmo] Place=0)
int *SlowmoPlace();
float *SlowmoButton(int i); // 0-2 right / up / ahead of the minimap (m), 3 size (m), 4 reach (m)
float SlowmoBuzz();         // xr.cpp haptics
bool SlowmoButtonPos(float p[3]);  // the floating clock's centre (recentred XR frame)
// slowfx1 (slowfx.cpp): cues while slow motion is on, each one switchable and adjustable from the menu
struct SlowFxCfg {
    bool push; float pushStrength, pushCol[3];
    bool vig; float vigStrength, vigStart, vigCol[3], vigThrob;
    bool scan; float scanStrength, scanLines;
    bool heart; float heartStrength, heartBpm;
    bool sound; float soundVol;
    bool ring;
    bool glow; float glowStrength;  // slowglow1 (DrGonzaga 16 Nov): the clock glows while slow motion is on
    float fadeIn, fadeOut;
    bool preview;  // not saved: the look and the heartbeat without slow motion, for setting them up
};
SlowFxCfg *SlowFx();
void SlowFxLoad();
void SlowFxSave();
void SlowFxEvent(int kind);      // 1 slow motion started, 2 ended
void SlowFxTestSound(int kind);
float SlowFxBuzz();              // xr.cpp haptics
void SlowFxDraw(IDXGISwapChain *sc);
struct LaserFx { float gain[3]; float vig[4]; float vigStart, scan, scanLines; };
void LaserDrawFx(IDXGISwapChain *sc, const LaserFx &fx);  // laser.cpp
// slowsnd1 (slowsnd.cpp): the game's sounds (XAudio2 voices) slowed with slow motion
void SlowSndStart();             // hook.cpp, as the mod starts
void SlowSndLoad();
void SlowSndSave();
void SlowSndTick(float speed);   // slowmo.cpp, input thread
bool *SlowSndOn();
int *SlowSndMode();              // 0 effects only, 1 everything
float *SlowSndAmount();
int SlowSndState();              // 0 not hooked, 1 waiting for the game's first voice, 2 working
int SlowSndVoices();
void LaserDrawRing(IDXGISwapChain *sc, const float *pos, float radius, float frac, const float *rgba);
void LaserDrawGlow(IDXGISwapChain *sc, const float *pos, float radius, const float *rgba);  // slowglow1: a soft round glow
void SlowmoInput(struct VrPad &p);  // input thread, before the other left grip uses
void SlowmoDraw(IDXGISwapChain *sc);
bool SlowmoShown();         // the clock is on the wrist menu (Blood Dragon, on, the address checked out)
bool SlowmoActive();        // on now
float SlowmoLeft();         // seconds left while on and timed (-1 otherwise)
float SlowmoNow();          // the game speed written last (1 = normal)
int SlowmoFound();          // 0 not looked yet, 1 found, -1 not found
bool *SlowmoOn();           // [slowmo] On
float *SlowmoSpeed();       // [slowmo] Speed (0.25 / 0.5)
float *SlowmoSeconds();     // [slowmo] Seconds (0 = until touched again)
float *SlowmoRamp();        // [slowmo] Ramp (real seconds)
bool *HolsterHeartModel();             // heart2: [holster] HeartModel: the game's own heart in the hand
int *HolsterHeartBtn();                // [holster] HeartButton: 0 D-pad left, 1 right, 2 up, 3 down
void HandsLeftGrip(bool held);         // hands24: input.cpp: the left grip as pressed
bool HolsterThrowAim(float m[16]);     // engine.cpp: physical throw, look along the swing (XR space)
bool HolsterThrowAimActive();
int HolsterGrenadeState();             // 0 none, 1 in the hand unarmed, 2 armed, 3 unarmed, grip let go (nade7)
bool *HolsterNadeOpt(int i);           // 0 hold to arm, 1 trigger arms, 2 teeth arm, 3 physical throw
float *HolsterNadeValue(int i);        // 0 mouth reach (m), 1 throw speed (m/s), 2 look turn time (s)
bool *HolsterSpheres();
bool *HolsterIcons();
bool *HolsterNeon();              // neon1: neon rings and outline icons for the grab spots
bool *HolsterHands();             // hands1: see-through spheres at the hands (gone while a hand holds something)
float *HolsterHandValue(int i);   // 0 radius (m), 1 opacity
bool HolsterChestFrame(float c[3], float f[3], float r[3]);  // holster4: chest row centre + body facing (XR space)
