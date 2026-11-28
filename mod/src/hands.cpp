// VR hands for Blood Dragon (DrGonzaga, 12-13 Nov, thread "Add VR hands to Blood Dragon guns").
// The game's first-person arms, shown again and put on the motion-controlled gun, with the game's
// own animation kept (grip, finger signs, gun spins).
//
// Where: the player's bone table (120 bones of 0x90 bytes, the same layout as the bow's in
// bowstring.cpp), reached through the attachment component (comp+0xc0). Every animated bone is
// written in MODEL space (mode 2) each frame; the gun object sits exactly on its bone. The skin
// update's hook (BD 0x102d10e7) runs on the game thread just after gunattach has placed the gun.
// Right arm: 48 collar, 49 upper arm (77-79 twist), 50 forearm (75, 76 twist), 51 hand (fingers
// 52-73, 74 weapon bone). Left arm: 17, 18 (45-47), 19 (43, 44), 20 hand (21-42). Spine 14-16.
//
// Each frame, in the skin hook (Apply):
//   * a hand on the gun: forearm, hand and fingers * T, T = inverse(gun bone) * our gun pose *
//     inverse(player object), so they keep the game's animation against the gun;
//   * a hand that holds nothing (left hand until both grips hold the gun; the arrow hand with the
//     bow): fitted to its own controller (FreeT), lightly smoothed (Follow);
//   * forearms turned about the wrist towards a likely elbow (Elbow); the bow's drawing arm has its
//     own rule there;
//   * collar, upper arm, and the spine bones the sleeves hang on: shrunk to a point.
// The game's own pose is put back before the game next reads the bones (HandsRestore), because it
// poses held objects from them.
// The hands' draws are the body parts ticked under "Hide my body" (body.cpp), drawn only while this
// runs. Switch: [hands] On. Nothing is touched while it is off or no part is ticked.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fc3vr.h"

extern "C" {
BYTE *volatile g_handsComp;  // the player's graphic component: the skin stub (bowstring.cpp) stops for it too
}

namespace {
enum { kEntry = 0x90, kMaxBones = 256 };
bool g_on = true, g_left = true;
float g_gunNow[16], g_playerNow[16];
volatile DWORD g_frameTick, g_liveTick;
volatile LONG g_frameBone = -1;
// hands23 (DrGonzaga 13 Nov: "can you add hands to the C4 now"): with C4 in both hands (gunattach
// c4hands1) the right hand stays on the detonator like on any gun, and the left hand goes with the
// charge: g_c4Rel = inverse(charge as the game had it) * charge as we placed it, so the hand keeps
// the game's own hold on it. No charge in the hand (just thrown): the left hand is free.
float g_c4Rel[16];
volatile DWORD g_c4Tick;
volatile bool g_c4;
// hands24 (DrGonzaga 15 Nov: "Left hand holding the C4 doesn't line up very well, is there some way for
// me to adjust where holding and possibly even the pose"; "when I accidentally press grip left hand
// when holding the pistol, the left hand disappears. Perfect would be that the left hand when close
// to pistol, and when pressing grip, snaps to grip the pistol"):
// * g_c4Adj moves and turns the hand on the charge (in the left controller's axes, about the wrist).
// * With a pistol the game has no left hand on the gun, so the left hand stays on its controller.
//   Left grip pressed with the hand near the pistol: the hand is locked to the gun, where it was at
//   the press, or at a place saved from the menu (g_pgRel: the hand against the gun, in metres).
// * A finger pose can be captured from the menu (the left hand's fingers as the game has them at
//   that moment, against the hand bone) and used on the C4 (slot 0) or on the pistol (slot 1).
float g_c4Adj[6];              // [hands] C4Right, C4Up, C4Forward (m), C4Pitch, C4Yaw, C4Roll (deg)
bool g_pistolGrip = true;      // [hands] PistolGrip
float g_pistolReach = 0.22f;   // [hands] PistolGripReach (m, wrist to wrist)
bool g_pgHave;
float g_pgRel[16];             // [hands] PistolGripPlace
volatile bool g_lgrip, g_pistolLocked;
volatile LONG g_pgSaveReq, g_poseCapReq = -1, g_dirty;
bool g_signShow = true;   // sign1: [hands] GameFingerSigns
bool g_emptyArms = true;  // emptyarms1: [hands] GameArmsEmptyHanded
bool g_emptyTracked = true;   // emptyhands1: [hands] EmptyHandsTracked
volatile bool g_emptyFrame;   // emptyhands1: this frame has no weapon: both hands on their controllers
// emptyhands5 (DrGonzaga 20 Nov, final levels: "brief moments with no weapon in hand ... arms were stiff and connected to
// right controller only ... when 'stiff arms' appears I'd like them to map to each controller"): tracked empty hands
// anywhere in plain on-foot play, not just the opening section. While the game itself moves its empty arms well away
// from where they usually sit (patting out fire and the like), its own arms are shown for that animation.
bool g_emptyAnywhere = true;  // [hands] EmptyHandsAnywhere
bool g_emptyGuard = true;     // [hands] EmptyHandsGameAnims
float g_emptyAway = 0.15f;    // [hands] EmptyHandsGameAnimCm / 100
static volatile bool g_openSect;  // emptyhands4 (declared here for emptyhands5)
// killstar1 (DrGonzaga 20 Nov: the Killstar "seems to be misplaced from the hand ... and the arms don't line up"):
// the game holds it on bone 20 (the left hand) like the bow, so the arms took the bow's rule (left arm rigid to the
// gun, right hand free) while the star itself rides the right controller. Now: right hand on its controller with the
// game's own grip on the star (its left fingers, mirrored), left hand free on its controller.
bool g_starMirror = true;  // [hands] KillstarGrip
static bool StarName(const char *n) { return n && (strstr(n, "KillStar") || strstr(n, "Killstar") || strstr(n, "KILLSTAR")); }
float g_signAway = 0.03f; // sign1: [hands] GameFingerSignCm / 100
enum { kFing0 = 21, kFingN = 22 };  // the left hand's finger bones, 21..42
struct Pose {
    bool have, use, changed;
    float m[kFingN][16];
} g_pose[4];  // 0 C4, 1 pistol, 2 neutral, 3 zipline (zip1)
// neutral2 (DrGonzaga 15 Nov: "make it so that the game no longer has to re-learn the pistol pose"): his learnt neutral
// left hand (bd62, [hands.pose.neutral] B21..B42: rotation rows, then position) is built in, so the free left hand is
// neutral from the first frame of a fresh install; a captured pose in the ini still wins.
const float kNeutral[kFingN][12] = {
    {0.63775f, -0.76022f, 0.12383f, 0.66670f, 0.46433f, -0.58302f, 0.38573f, 0.45438f, 0.80296f, 0.02947f, -0.02115f, -0.01514f},
    {0.85494f, -0.50322f, -0.12584f, 0.34685f, 0.73499f, -0.58265f, 0.38569f, 0.45449f, 0.80292f, 0.06202f, -0.05972f, -0.00888f},
    {0.85495f, -0.50325f, -0.12572f, 0.34689f, 0.73490f, -0.58273f, 0.38565f, 0.45459f, 0.80288f, 0.09512f, -0.07921f, -0.01376f},
    {-0.51391f, 0.11663f, -0.84988f, -0.22332f, -0.97474f, 0.00128f, -0.82827f, 0.19046f, 0.52697f, 0.03508f, -0.31777f, -0.51331f},
    {0.98335f, -0.09090f, -0.15734f, -0.07437f, 0.58870f, -0.80492f, 0.16580f, 0.80322f, 0.57214f, 0.03058f, -0.00093f, -0.02393f},
    {1.00000f, -0.00009f, 0.00008f, 0.00009f, 1.00000f, -0.00009f, -0.00009f, 0.00009f, 1.00000f, 0.09882f, -0.03461f, 0.00360f},
    {1.00000f, -0.00017f, 0.00017f, 0.00017f, 1.00000f, -0.00017f, -0.00017f, 0.00017f, 1.00000f, 0.15023f, -0.03461f, 0.00361f},
    {1.00000f, -0.00026f, 0.00026f, 0.00026f, 1.00000f, -0.00026f, -0.00026f, 0.00026f, 1.00000f, 0.17483f, -0.03462f, 0.00361f},
    {-0.54416f, -0.04885f, -0.83756f, -0.13396f, -0.98043f, 0.14422f, -0.82822f, 0.19068f, 0.52697f, 0.03722f, -0.31725f, -0.51013f},
    {1.00000f, -0.00009f, 0.00008f, 0.00009f, 1.00000f, -0.00009f, -0.00009f, 0.00009f, 1.00000f, 0.09277f, -0.01099f, 0.00989f},
    {1.00000f, -0.00017f, 0.00017f, 0.00017f, 1.00000f, -0.00017f, -0.00017f, 0.00017f, 1.00000f, 0.15345f, -0.01099f, 0.00989f},
    {1.00000f, -0.00026f, 0.00026f, 0.00026f, 1.00000f, -0.00026f, -0.00026f, 0.00026f, 1.00000f, 0.18253f, -0.01100f, 0.00990f},
    {-0.54416f, -0.04885f, -0.83756f, -0.13396f, -0.98043f, 0.14422f, -0.82822f, 0.19068f, 0.52697f, 0.03719f, -0.31726f, -0.51018f},
    {0.99072f, 0.13534f, 0.01263f, -0.13486f, 0.99030f, -0.03321f, -0.01700f, 0.03120f, 0.99937f, 0.00487f, -0.00007f, -0.00006f},
    {1.00000f, -0.00017f, 0.00018f, 0.00017f, 1.00000f, -0.00016f, -0.00019f, 0.00016f, 1.00000f, 0.09543f, 0.00078f, 0.00493f},
    {1.00000f, -0.00026f, 0.00027f, 0.00026f, 1.00000f, -0.00024f, -0.00027f, 0.00025f, 1.00000f, 0.14907f, 0.00077f, 0.00494f},
    {1.00000f, -0.00034f, 0.00036f, 0.00034f, 1.00000f, -0.00033f, -0.00036f, 0.00033f, 1.00000f, 0.17763f, 0.00076f, 0.00495f},
    {-0.54416f, -0.04885f, -0.83756f, -0.13396f, -0.98043f, 0.14422f, -0.82822f, 0.19068f, 0.52697f, 0.03844f, -0.31714f, -0.50825f},
    {1.00000f, -0.00017f, 0.00018f, 0.00017f, 1.00000f, -0.00016f, -0.00019f, 0.00016f, 1.00000f, 0.09623f, 0.02313f, -0.00259f},
    {1.00000f, -0.00026f, 0.00027f, 0.00026f, 1.00000f, -0.00024f, -0.00027f, 0.00025f, 1.00000f, 0.13765f, 0.02313f, -0.00258f},
    {1.00000f, -0.00034f, 0.00036f, 0.00034f, 1.00000f, -0.00033f, -0.00036f, 0.00033f, 1.00000f, 0.15828f, 0.02312f, -0.00258f},
    {-0.54416f, -0.04885f, -0.83756f, -0.13396f, -0.98043f, 0.14422f, -0.82822f, 0.19068f, 0.52697f, 0.04064f, -0.31695f, -0.50486f},
};
const wchar_t *const kPoseSec[4] = {L"hands.pose.c4", L"hands.pose.pistol", L"hands.pose.neutral", L"hands.pose.zipline"};
// zip1 (DrGonzaga 15 Nov: "I set a left hand pose (closed grip ...) and a left hand position (raised up and to the left,
// as though I'm hanging) that lines up with the zipline, and we trigger it when ziplining"): while the game's input
// mode is "ziplining" (or the menu's preview is ticked) the left hand leaves its controller and sits at a place given
// from the head, in the body's axes (g_axes: head facing, levelled), with the captured fingers of slot 3.
bool g_zipOn = true, g_zipPreview, g_zipHook = true;  // [hands] ZipHand, ZipHookGame
float g_zip[6] = {-0.20f, 0.25f, 0.10f, 80, 0, 0};    // [hands] ZipRight, ZipUp, ZipForward (m), ZipPitch, ZipYaw, ZipRoll (deg)
// c4learn2: see the C4 branch
int g_c4LearnN, g_c4Bad;
bool g_c4Built;
// c4mirror1 (DrGonzaga 15 Nov, bd64: "C4 is still transparent, it seems to mostly render the inside of the model"): seeing
// the inside = the faces are drawn back to front = our matrix has the other handedness than the game's (a mirrored
// model, as left-hand things often are). The charge we place now keeps the game's handedness.
volatile bool g_c4Mirror;
// capture2 (DrGonzaga 15 Nov: "I can't get it to grip when in the debug menu - should we set a timer to capture it?")
volatile LONG g_poseCapSlot = -1;
volatile DWORD g_poseCapAt, g_capBuzzUntil;

// hands25 (DrGonzaga 15 Nov): "can we have it 'join' automatically when the left controller is close enough to
// cause smoothing" = g_pistolAuto: the left hand goes onto the pistol while the pistol is steadied (twohand.cpp,
// hands together), no grip needed. "With the left hand, when not holding a gun, currently it will pose dependent
// upon what gun is held ... add an option for a 'neutral' left hand pose" = g_neutral: the free left hand's
// fingers come from pose slot 2 whatever the gun; the hand is fitted to the controller from that pose too, so it
// sits the same with every gun. Slot 2 is captured from the menu, or learnt by itself the first time a pistol is
// out with the left hand free (the game holds that hand open and relaxed then).
// c4rigid1 (DrGonzaga 15 Nov, bd61: "C4 shake is still there"; earlier: "find the C4 model and attach it to a posed
// left hand, rather than have the 'games hand' and 'games C4' conflict"). bd61's log showed the game's charge pose
// was always read fresh, so that was not the cause. Now nothing of the game's moves either of them: the left hand
// is the free hand on its controller (no smoothing), and the charge is fixed to the same controller frame by
// g_c4K = how the game held the charge against the hand's finger frame, averaged over 30 frames once, then kept
// ([hands] C4Hold; "learn again" on the menu). Hand and charge are one rigid thing on the controller.
float g_c4K[16], g_c4Game[16];
bool g_c4KHave;
// c4turn1 (DrGonzaga 15 Nov, bd62: "the C4 object doesn't rotate when I move the sliders"): the charge's own fit on
// the hand, a shift and a turn in the left controller's axes about the charge ([hands] C4Obj...).
float g_c4Obj[6];
volatile LONG g_saveSoon;      // a learnt pose is written to the ini at once (not only when the menu saves)
// gripfwd1 (DrGonzaga 21 Nov: "The left grip grab point for the mini gun is quite caudal vs the front handle. Any way to
// adjust this grab point, for the mini gun only"): with both grips holding a gun the left hand goes where the game's
// animation has it on that gun. Now each gun can move it: a shift in the gun's own axes (right, up, forward) and a turn
// about the wrist, kept per gun in [gun.<name>] LeftHandRight/Up/Forward (m), LeftHandPitch/Yaw/Roll (deg). Only the
// Minigun has a built-in shift (forward, a guess at its front handle); every other gun stays as the game has it.
// The menu can also take the place from the left controller: hold the gun with both grips, hand where it should be,
// and the shift becomes "the game's wrist -> your wrist" in the gun's axes.
float g_gl[6];                 // the gun in hand's left hand shift and turn
static char g_glGun[48];       // which gun g_gl belongs to ("" = none loaded)
static volatile bool g_glTouched;  // changed from the menu or a capture: written to the ini at the next save
static volatile LONG g_glCapAt, g_glCapOn, g_glCapNow;  // capture timer / take it this frame
static volatile bool g_glOnGun;    // last frame: the left hand was on the gun (both grips)
static float g_glGame[3], g_glMine[3];  // last frame, cm in the gun's axes from the gun's point: the game's left wrist, the controller's
static DWORD g_glBuzzUntil;
static void GunLeftDefaults(const char *gun, float *o) {
    memset(o, 0, 6 * sizeof(float));
    (void)gun;  // bd117: no built-in shift (DrGonzaga: the game's hand was already on the right part of the minigun)
}
static const wchar_t *const kGlKey[6] = {L"LeftHandRight", L"LeftHandUp", L"LeftHandForward", L"LeftHandPitch", L"LeftHandYaw", L"LeftHandRoll"};
static void GunLeftLoad(const char *gun) {
    strncpy(g_glGun, gun ? gun : "", 47), g_glGun[47] = 0, g_glTouched = false;
    GunLeftDefaults(g_glGun, g_gl);
    if (!g_glGun[0]) return;
    wchar_t path[MAX_PATH], sec[64], b[32];
    IniPath(path);
    swprintf(sec, 64, L"gun.%hs", g_glGun);
    for (int i = 0; i < 6; ++i)
        if (GetPrivateProfileStringW(sec, kGlKey[i], L"", b, 32, path)) g_gl[i] = (float)_wtof(b);
    bool any = false;
    for (int i = 0; i < 6; ++i) any |= g_gl[i] != 0;
    if (any) Log("hands: %s: left hand on the gun moved %.1f cm right, %.1f up, %.1f forward, turned %.0f %.0f %.0f deg", g_glGun,
                 g_gl[0] * 100, g_gl[1] * 100, g_gl[2] * 100, g_gl[3], g_gl[4], g_gl[5]);
}
static void GunLeftSave(const wchar_t *path) {
    if (!g_glTouched || !g_glGun[0]) return;
    wchar_t sec[64], b[32];
    swprintf(sec, 64, L"gun.%hs", g_glGun);
    for (int i = 0; i < 6; ++i) swprintf(b, 32, i < 3 ? L"%.4f" : L"%.2f", g_gl[i]), WritePrivateProfileStringW(sec, kGlKey[i], b, path);
    g_glTouched = false;
}
volatile LONG g_c4Relearn;
bool g_pistolAuto = true;      // [hands] PistolAutoJoin
bool g_neutral = true;         // [hands] NeutralLeft
volatile bool g_frameOk;
// hands3 (DrGonzaga 13 Nov): a hand that holds nothing follows its own controller.
bool g_freeLeft = true;   // [hands] FreeLeft: left hand free until both grips hold the gun
bool g_chest = true;      // [hands] HideShoulders: spine bones go to the right elbow too
float g_free[6] = {0.0f, -0.02f, -0.10f, 0, 0, 0};
float g_freeBow[6] = {0.0f, -0.02f, -0.10f, 0, 0, 0};  // hands5: the arrow hand with the bow has its own fit ([hands] Arrow*)
float g_ctlF[2][9], g_ctlP[2][3], g_ctlScale = 1;
volatile bool g_ctlOk[2], g_twoHand;
volatile LONG g_freeL, g_freeR;
// hands4 (DrGonzaga 13 Nov: with the bow the right forearm bent back past the elbow; the left hand
// covers the scope): a free hand's forearm is turned about the wrist towards where the elbow would
// be (two-bone reach from a shoulder guessed under the head, elbow down and a little out), and the
// left hand is not drawn while zoomed.
float g_eye[3], g_axes[9];
volatile bool g_headOk, g_scoped;
bool g_scopeHide = true;  // [hands] HideLeftScoped
bool g_elbow = true;      // [hands] FreeElbow
// hands6 (DrGonzaga 13 Nov: free hands a little jittery; smooth the change from free hand to grip):
// each arm's hand pose (world turn, place relative to the game camera's place) is followed with a
// short time constant: g_smooth seconds while free, none on the gun (it must stay on the gun), plus
// an extra that fades over a quarter second after every change between free and gun. The elbow turn
// fades in and out the same way, and the head position the shoulder is guessed from is smoothed.
float g_smooth = 0.035f;     // [hands] Smoothing (s)
float g_blend = 0.25f;       // [hands] Blend (s)
float g_camRef[3];
struct ArmState {
    float rot[9], pos[3];    // followed pose
    int mode;                // last mode (0 = none yet / hidden)
    double since;            // seconds since the mode changed
    float elbow;             // 0..1 how much of the elbow turn is applied
};
ArmState g_arm[2];           // 0 right, 1 left
float g_eyeS[3];
bool g_eyeSOk;
// walkhands2 (DrGonzaga 27 Nov: "when you move physically and press forward in-game at the same time, I want them to feel
// the same as they do when you are doing either action independently"): the hands are followed (Smoothing, Blend) and
// the shoulder guess smoothed against a point that moves with the game body (g_camRef). Walking for real on its own,
// roomscale walks the body after the head, so the hands barely move against that point. With your own stick pushed,
// roomscale stands aside: the body runs on by the stick while your real steps carry head and hands away from it at
// walking speed, so the smoothing held the hands back (speed x 83 ms, plus 0.15 s on the shoulder) until you stopped.
// Now that point also takes the level head offset, slowly (0.3 s), while your own stick is pushed: it moves at the
// speed your real steps carry you, so the hands keep up, and quick head moves (lean, bob) still do not pull the hands.
// With the stick let go it holds still (a fixed offset changes nothing), so walking for real alone or the stick alone
// feel exactly as before.
float g_qL[3], g_qW[3];  // the offset in the body's level axes, and in the world
LONGLONG g_qT;
LONGLONG g_lastQpc;
// hands7 (DrGonzaga 13 Nov: after tuning arrows in the menu preview, the next nocked arrow sat 50 cm
// to the side): the attachment update reads a mode 2 bone's own matrix (BD 0x101a6b45), so while the
// animation does not write (menu preview, pause) the game "posed" the hand arrow from the bones WE
// had moved, and bowstring.cpp learnt that as the arrow's resting place on the bow. The bones are
// now put back as the game wrote them before the game reads them again (HandsRestore, from the
// attachment hook); the skin hook then moves them again for drawing.
float (*g_saved)[16];  // the table as the game wrote it, before our last Apply
BYTE *g_savedBones;
int g_savedN;
volatile LONG g_restored;
int g_wroteBone = -1;
volatile LONG g_bowWait;  // frames the bow was out with the hands off (hand arrow's rest not known yet)
bool g_elbowGun = true;   // hands11: [hands] GripElbow: hands on a gun / the bow get the elbow rule too
float g_elbowOut = 0.45f;  // hands11: [hands] ElbowOut
float g_wristBend = 55;  // hands15: [hands] WristBend (deg)
float g_bowEl[3];  // [hands] BowAxisHigh, BowAxisSide, BowAxisBack (m): moves the green line's far end
float g_wrote[16];  // what we last wrote to the anchor bone (so one frame is never moved twice)
volatile LONG g_applied, g_skippedTwice, g_notRigid;
BYTE *volatile g_bones;
volatile LONG g_n;
double g_qpf;

LONGLONG Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// 4x4, rows right / forward / up / position (the game's layout), row vectors: world = model * object.
void Mul(const float *a, const float *b, float *o) {
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, sizeof t);
}
// Inverse of a turn + move (rows unit length and square to each other). false = not such a matrix.
bool InvRigid(const float *m, float *o) {
    for (int r = 0; r < 3; ++r) {
        float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(l > 0.94f && l < 1.06f)) return false;
        int q = (r + 1) % 3;
        float d = m[r * 4] * m[q * 4] + m[r * 4 + 1] * m[q * 4 + 1] + m[r * 4 + 2] * m[q * 4 + 2];
        if (fabsf(d) > 0.04f) return false;
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o[r * 4 + c] = m[c * 4 + r];
    for (int c = 0; c < 3; ++c) o[12 + c] = -(m[12] * o[c] + m[13] * o[4 + c] + m[14] * o[8 + c]);
    o[3] = o[7] = o[11] = 0, o[15] = 1;
    return true;
}
bool Under(BYTE *bones, int n, int b, int top) {  // is bone b the bone top or below it?
    for (int hop = 0; b >= 0 && b < n && hop < 12; ++hop) {
        if (b == top) return true;
        b = *(int *)(bones + b * kEntry + 4);
    }
    return false;
}
void Shrink(float *m, const float *at) {
    for (int k = 0; k < 12; ++k) m[k] *= 0.001f;
    m[12] = at[0], m[13] = at[1], m[14] = at[2];
}

// hands3: where a free hand goes. The hand's own frame is taken from its bones, so no guess about
// the model's axes is needed: wrist = the hand bone, "along" = wrist to the middle finger's first
// bone, "thumb side" = from the ring finger's first bone to the index finger's. That frame is put
// on the controller: along = the controller's forward, thumb side = its up, wrist a little behind
// the grip. g_free shifts and turns it (given for the left hand, mirrored for the right).
// c4rigid1: the controller's hand frame in the world (what the hand's finger frame is put on), from the settings
bool CtlFrame(int which, float *W);
bool CtlFrameOf(const float *ctlF, const float *ctlP, const float *fit, float sgn, float *Wout);
// zip1: is the zipline hand wanted now, and its frame (a made-up controller: the body's right / forward / up at a
// place from the smoothed head, turned by the sliders)
bool ZipNow() { return g_left && g_zipOn && g_headOk && (g_zipPreview || EngineInputMode() == 0xff0b2e0e); }  // crc32("ziplining")
bool ZipFrame(float *W) {
    if (!g_headOk) return false;
    const float *R = g_axes, *U = g_axes + 3, *F = g_axes + 6;
    float sc = g_ctlScale > 0.01f ? g_ctlScale : 1, C[9], P[3], fit[6] = {0, 0, 0, g_zip[3], g_zip[4], g_zip[5]};
    for (int k = 0; k < 3; ++k) {
        C[k] = R[k], C[3 + k] = F[k], C[6 + k] = U[k];
        P[k] = (g_eyeSOk ? g_eyeS[k] + g_camRef[k] + g_qW[k] : g_eye[k]) + (R[k] * g_zip[0] + U[k] * g_zip[1] + F[k] * g_zip[2]) / sc;
    }
    return CtlFrameOf(C, P, fit, 1, W);
}
bool FreeT(BYTE *bones, int hand, int mid, int idx, int ring, int which, const float *ip, float *T, float *Gout = nullptr, const float *Wgiven = nullptr) {
    if (!Wgiven && !g_ctlOk[which]) return false;
    auto Pos = [&](int b) { return (const float *)(bones + b * kEntry + 0x10) + 12; };
    const float *w = Pos(hand), *pm = Pos(mid), *pi = Pos(idx), *pr = Pos(ring);
    float f[3], sd[3], nn[3];
    for (int k = 0; k < 3; ++k) f[k] = pm[k] - w[k], sd[k] = pi[k] - pr[k];
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 0.02f || fl > 0.3f) return false;
    for (int k = 0; k < 3; ++k) f[k] /= fl;
    float d = sd[0] * f[0] + sd[1] * f[1] + sd[2] * f[2];
    for (int k = 0; k < 3; ++k) sd[k] -= d * f[k];
    float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    if (sl < 0.004f) return false;
    for (int k = 0; k < 3; ++k) sd[k] /= sl;
    nn[0] = f[1] * sd[2] - f[2] * sd[1], nn[1] = f[2] * sd[0] - f[0] * sd[2], nn[2] = f[0] * sd[1] - f[1] * sd[0];
    float G[16] = {sd[0], sd[1], sd[2], 0, f[0], f[1], f[2], 0, nn[0], nn[1], nn[2], 0, w[0], w[1], w[2], 1}, iG[16];
    if (!InvRigid(G, iG)) return false;
    if (Gout) memcpy(Gout, G, 64);
    float W[16];
    if (Wgiven) memcpy(W, Wgiven, 64);  // zip1
    else if (!CtlFrame(which, W)) return false;
    Mul(iG, W, T);
    Mul(T, ip, T);
    return true;
}
bool CtlFrameOf(const float *ctlF, const float *ctlP, const float *fit, float sgn, float *Wout);
bool CtlFrame(int which, float *Wout) {
    if (!g_ctlOk[which]) return false;
    // hands5: the right hand is only free with the bow
    return CtlFrameOf(g_ctlF[which], g_ctlP[which], which == 1 ? g_freeBow : g_free, which == 0 ? 1.f : -1.f, Wout);
}
bool CtlFrameOf(const float *ctlF, const float *ctlP, const float *fit, float sgn, float *Wout) {
    // the controller's frame, turned by the settings (pitch about right, yaw about up, roll about forward)
    float p = fit[3] * 0.0174533f, y = fit[4] * 0.0174533f * sgn, r = fit[5] * 0.0174533f * sgn;
    float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(r), sr = sinf(r);
    float Ry[9] = {cy, sy, 0, -sy, cy, 0, 0, 0, 1}, Rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp}, Rr[9] = {cr, 0, -sr, 0, 1, 0, sr, 0, cr};
    auto M3 = [](const float *A, const float *B, float *C) {
        float t[9];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) t[a * 3 + b] = A[a * 3] * B[b] + A[a * 3 + 1] * B[3 + b] + A[a * 3 + 2] * B[6 + b];
        memcpy(C, t, sizeof t);
    };
    float Q[9], H[9];
    M3(Rp, Rr, Q), M3(Ry, Q, Q), M3(Q, ctlF, H);  // rows right, forward, up
    const float *R = H, *F = H + 3, *U = H + 6, *cp3 = ctlP;
    float sc = g_ctlScale > 0.01f ? g_ctlScale : 1;
    float W[16] = {U[0], U[1], U[2], 0, F[0], F[1], F[2], 0, 0, 0, 0, 0, 0, 0, 0, 1};
    // third row = forward x up-row, the same rule as nn = f x sd above
    W[8] = F[1] * U[2] - F[2] * U[1], W[9] = F[2] * U[0] - F[0] * U[2], W[10] = F[0] * U[1] - F[1] * U[0];
    for (int k = 0; k < 3; ++k) W[12 + k] = cp3[k] + (R[k] * fit[0] * sgn + U[k] * fit[1] + F[k] * fit[2]) / sc;
    memcpy(Wout, W, 64);
    return true;
}

// hands4: turn a free hand's forearm (bone fore and what hangs on it except the hand) about the wrist
// so the elbow lies where a real arm's would. side: +1 right arm, -1 left. All in model space.
void Elbow(BYTE *bones, int n, int fore, int hand, float upperLen, float side, const float *ip, float weight, bool bowArm = false) {
    if (!g_headOk || !g_elbow || weight < 0.01f) return;
    float sc = g_ctlScale > 0.01f ? g_ctlScale : 1;
    const float *R = g_axes, *U = g_axes + 3, *F = g_axes + 6;
    float sw[3], dw[3];  // shoulder and "elbow goes this way" in the world
    for (int k = 0; k < 3; ++k) {
        sw[k] = (g_eyeSOk ? g_eyeS[k] + g_camRef[k] + g_qW[k] : g_eye[k]) + (-U[k] * 0.20f + R[k] * 0.19f * side - F[k] * 0.08f) / sc;
        dw[k] = -U[k] + R[k] * g_elbowOut * side - F[k] * 0.25f;
    }
    float s[3], dn[3];
    for (int k = 0; k < 3; ++k) {
        s[k] = sw[0] * ip[k] + sw[1] * ip[4 + k] + sw[2] * ip[8 + k] + ip[12 + k];
        dn[k] = dw[0] * ip[k] + dw[1] * ip[4 + k] + dw[2] * ip[8 + k];
    }
    float *fm = (float *)(bones + fore * kEntry + 0x10), *hm = (float *)(bones + hand * kEntry + 0x10);
    float w[3] = {hm[12], hm[13], hm[14]}, cur[3], d[3];
    // (hands14: this line was lost in bd40's edit; bd40-bd44 turned the forearms by whatever was in memory)
    for (int k = 0; k < 3; ++k) cur[k] = fm[12 + k] - w[k], d[k] = w[k] - s[k];
    float bowX = 0;
    if (bowArm) {
        // hands8: the higher the arrow hand (from 30 cm under the shoulder to 5 cm under it), the more
        // the forearm takes the draw rule below instead of the shoulder guess.
        float um[3], h = 0;
        for (int k = 0; k < 3; ++k) um[k] = U[0] * ip[k] + U[1] * ip[4 + k] + U[2] * ip[8 + k];
        for (int k = 0; k < 3; ++k) h += (w[k] - s[k]) * um[k];
        float x = (h * sc + 0.30f) / 0.25f;
        x = x < 0 ? 0 : x > 1 ? 1 : x;
        bowX = x * x * (3 - 2 * x);
    }
    float b = sqrtf(cur[0] * cur[0] + cur[1] * cur[1] + cur[2] * cur[2]), a = upperLen;
    float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (b < 0.1f || a < 0.1f || L < 0.02f) return;
    for (int k = 0; k < 3; ++k) d[k] /= L, cur[k] /= b;
    if (L > a + b - 0.01f) L = a + b - 0.01f;  // further than the arm reaches: a straight arm towards the hand
    if (L < fabsf(a - b) + 0.01f) L = fabsf(a - b) + 0.01f;
    float x = (a * a - b * b + L * L) / (2 * L), h2 = a * a - x * x, h = h2 > 0 ? sqrtf(h2) : 0;
    float dd = dn[0] * d[0] + dn[1] * d[1] + dn[2] * d[2], bend[3];
    for (int k = 0; k < 3; ++k) bend[k] = dn[k] - dd * d[k];
    float bl = sqrtf(bend[0] * bend[0] + bend[1] * bend[1] + bend[2] * bend[2]);
    if (bl < 1e-3f) return;
    // the elbow, measured back from the wrist (the shoulder end is only a guess; the wrist is exact)
    float t[3];
    for (int k = 0; k < 3; ++k) t[k] = -d[k] * (L - x) + bend[k] / bl * h;
    float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (tl < 1e-3f) return;
    for (int k = 0; k < 3; ++k) t[k] /= tl;
    if (bowX > 0.001f) {
        // hands18 (DrGonzaga 13 Nov, after bd47 / bd48): the bow's drawing arm, his rule.
        // 1) Until the arrow is nocked the forearm lies along the "green line": from the wrist to a
        //    point set against the head (from the eyes 8 cm down, 45 cm out, 20 cm back along where the
        //    head faces; [hands] BowAxisHigh / Side / Back move it).
        // 2) From the nock on, that line is no longer taken from the head: the direction it had at the
        //    nock is kept and turns only with the controller's heading (its yaw about the world's up),
        //    so the wrist stays close to locked and the elbow slides back with the hand.
        // Eased (50 ms) so nocking and letting go do not snap. No wrist limit on this arm.
        const float *P = g_playerNow;
        float ew[3], e[3], gm[3], gl = 0, gw[3];
        for (int k = 0; k < 3; ++k)
            ew[k] = (g_eyeSOk ? g_eyeS[k] + g_camRef[k] + g_qW[k] : g_eye[k]) +
                    (U[k] * (-0.08f + g_bowEl[0]) + R[k] * (0.45f + g_bowEl[1]) * side - F[k] * (0.20f + g_bowEl[2])) / sc;
        for (int k = 0; k < 3; ++k) e[k] = ew[0] * ip[k] + ew[1] * ip[4 + k] + ew[2] * ip[8 + k] + ip[12 + k];
        for (int k = 0; k < 3; ++k) gm[k] = e[k] - w[k], gl += gm[k] * gm[k];
        gl = sqrtf(gl);
        static bool s_lock, s_ok;
        static float s_dir0[3], s_h0[3], s_tw[3];
        static int s_axis;
        static LONGLONG s_q;
        if (gl > 0.03f) {
            for (int k = 0; k < 3; ++k) gm[k] /= gl;
            for (int k = 0; k < 3; ++k) gw[k] = gm[0] * P[k] + gm[1] * P[4 + k] + gm[2] * P[8 + k];
            LONGLONG qn = Qpc();
            float dt = s_q && g_qpf > 0 ? (float)((qn - s_q) / g_qpf) : 1;
            s_q = qn;
            if (dt > 0.3f) s_ok = false, s_lock = false;
            auto Heading = [&](int axis, float *h) {  // a controller axis, levelled; gives its level length
                const float *a = g_ctlF[1] + 3 * axis;
                float dd = a[0] * U[0] + a[1] * U[1] + a[2] * U[2], l = 0;
                for (int k = 0; k < 3; ++k) h[k] = a[k] - dd * U[k], l += h[k] * h[k];
                l = sqrtf(l);
                if (l > 1e-4f)
                    for (int k = 0; k < 3; ++k) h[k] /= l;
                return l;
            };
            bool nocked = BowArrowMode() == 3 && g_ctlOk[1];
            if (!nocked) s_lock = false;
            else if (!s_lock) {
                float h0[3], h1[3], l0 = Heading(1, h0), l1 = Heading(0, h1);  // forward, or right when it points up / down
                s_axis = l0 >= l1 ? 1 : 0;
                memcpy(s_h0, s_axis ? h0 : h1, 12);
                memcpy(s_dir0, s_ok ? s_tw : gw, 12);
                s_lock = true;
            }
            float want[3];
            memcpy(want, gw, 12);
            if (s_lock) {
                float h[3];
                if (Heading(s_axis, h) > 0.2f) {
                    float cx[3] = {s_h0[1] * h[2] - s_h0[2] * h[1], s_h0[2] * h[0] - s_h0[0] * h[2], s_h0[0] * h[1] - s_h0[1] * h[0]};
                    float ang = atan2f(cx[0] * U[0] + cx[1] * U[1] + cx[2] * U[2], s_h0[0] * h[0] + s_h0[1] * h[1] + s_h0[2] * h[2]);
                    float cs2 = cosf(ang), sn2 = sinf(ang), kd = U[0] * s_dir0[0] + U[1] * s_dir0[1] + U[2] * s_dir0[2];
                    float kx[3] = {U[1] * s_dir0[2] - U[2] * s_dir0[1], U[2] * s_dir0[0] - U[0] * s_dir0[2], U[0] * s_dir0[1] - U[1] * s_dir0[0]};
                    for (int k = 0; k < 3; ++k) want[k] = s_dir0[k] * cs2 + kx[k] * sn2 + U[k] * kd * (1 - cs2);
                } else {
                    memcpy(want, s_ok ? s_tw : s_dir0, 12);
                }
            }
            float kk = s_ok ? 1 - expf(-dt / 0.05f) : 1, nl = 0;
            for (int k = 0; k < 3; ++k) s_tw[k] = s_ok ? s_tw[k] + (want[k] - s_tw[k]) * kk : want[k], nl += s_tw[k] * s_tw[k];
            nl = sqrtf(nl);
            if (nl > 1e-3f) {
                for (int k = 0; k < 3; ++k) s_tw[k] /= nl;
                s_ok = true;
                float tb[3], n2 = 0;
                for (int k = 0; k < 3; ++k) tb[k] = s_tw[0] * ip[k] + s_tw[1] * ip[4 + k] + s_tw[2] * ip[8 + k];
                for (int k = 0; k < 3; ++k) t[k] += (tb[k] - t[k]) * bowX, n2 += t[k] * t[k];
                n2 = sqrtf(n2);
                if (n2 < 1e-3f) return;
                for (int k = 0; k < 3; ++k) t[k] /= n2;
            } else {
                s_ok = false;
            }
        }
    }
    // the turn taking cur to t (Rodrigues)
    float ax[3] = {cur[1] * t[2] - cur[2] * t[1], cur[2] * t[0] - cur[0] * t[2], cur[0] * t[1] - cur[1] * t[0]};
    float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), cs = cur[0] * t[0] + cur[1] * t[1] + cur[2] * t[2];
    if (sn < 1e-4f) return;
    for (int k = 0; k < 3; ++k) ax[k] /= sn;
    {
        // hands15 (DrGonzaga 13 Nov: drawing the bow, "the wrist twists into a tiny area"): the hand keeps
        // the controller's turn, so a forearm turned far from it folds the wrist's skin shut. The turn
        // away from the straight-wrist pose is limited, softly ([hands] WristBend, degrees): small turns
        // pass unchanged, large ones level off at the limit. hands6: and only part of it while fading.
        float ang = atan2f(sn, cs), lim = (g_wristBend < 5 ? 5 : g_wristBend) * 0.0174533f, wt = weight < 0.999f ? weight : 1;
        float bent = lim * tanhf(ang / lim) * wt;
        // The bow's drawing arm has no limit while its own rule is in charge (its line is tied to the controller).
        ang = bowArm ? bent + (ang * wt - bent) * bowX : bent;
        sn = sinf(ang), cs = cosf(ang);
    }
    auto Rot = [&](float *q) {
        float kx[3] = {ax[1] * q[2] - ax[2] * q[1], ax[2] * q[0] - ax[0] * q[2], ax[0] * q[1] - ax[1] * q[0]}, kd = ax[0] * q[0] + ax[1] * q[1] + ax[2] * q[2];
        for (int k = 0; k < 3; ++k) q[k] = q[k] * cs + kx[k] * sn + ax[k] * kd * (1 - cs);
    };
    for (int i = 0; i < n; ++i) {
        if (*(int *)(bones + i * kEntry + 8) != 2 || !Under(bones, n, i, fore) || Under(bones, n, i, hand)) continue;
        float *m = (float *)(bones + i * kEntry + 0x10), rel[3] = {m[12] - w[0], m[13] - w[1], m[14] - w[2]};
        Rot(m), Rot(m + 4), Rot(m + 8), Rot(rel);
        for (int k = 0; k < 3; ++k) m[12 + k] = w[k] + rel[k];
    }
    if (bowArm && bowX > 0.001f && hand == 51 && n > 66) {
        // hands19 (DrGonzaga 13 Nov, bd49: every point in the right place, but "when I draw back the whole
        // forearm does a 180 and twists at the wrist"). The turn above is the shortest one from where
        // the game's animation has the forearm against the hand; the game's draw animation curls that
        // hand, and once the two directions are far apart the shortest turn comes round the other way:
        // the forearm arrives upside down. So its roll about its own length is now set outright: the
        // forearm's "thumb side" (the hand's, as the forearm carried it) is rolled onto the hand's real
        // thumb side (index knuckle minus ring knuckle, bones 57 / 66), whichever way the turn went.
        const float *pi = (const float *)(bones + 57 * kEntry + 0x10) + 12, *pr = (const float *)(bones + 66 * kEntry + 0x10) + 12;
        float hs[3] = {pi[0] - pr[0], pi[1] - pr[1], pi[2] - pr[2]}, hl = sqrtf(hs[0] * hs[0] + hs[1] * hs[1] + hs[2] * hs[2]);
        float c2[3] = {cur[0], cur[1], cur[2]};
        Rot(c2);  // the forearm's line as it now is
        if (hl > 1e-3f) {
            for (int k = 0; k < 3; ++k) hs[k] /= hl;
            float d0 = hs[0] * cur[0] + hs[1] * cur[1] + hs[2] * cur[2], d2 = hs[0] * c2[0] + hs[1] * c2[1] + hs[2] * c2[2];
            float v1[3], v2[3], l1 = 0, l2 = 0;
            for (int k = 0; k < 3; ++k) v1[k] = hs[k] - d0 * cur[k], v2[k] = hs[k] - d2 * c2[k], l1 += v1[k] * v1[k], l2 += v2[k] * v2[k];
            l1 = sqrtf(l1), l2 = sqrtf(l2);
            float fade = ((l1 < l2 ? l1 : l2) - 0.15f) / 0.25f;  // thumb side nearly along the forearm: no say
            fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
            if (fade > 0) {
                for (int k = 0; k < 3; ++k) v1[k] /= l1, v2[k] /= l2;
                Rot(v1);  // carried round with the forearm
                float cx[3] = {v1[1] * v2[2] - v1[2] * v2[1], v1[2] * v2[0] - v1[0] * v2[2], v1[0] * v2[1] - v1[1] * v2[0]};
                float phi = atan2f(cx[0] * c2[0] + cx[1] * c2[1] + cx[2] * c2[2], v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2]) * fade * bowX;
                float ts = sinf(phi), tc = cosf(phi);
                auto Roll = [&](float *q) {
                    float kx[3] = {c2[1] * q[2] - c2[2] * q[1], c2[2] * q[0] - c2[0] * q[2], c2[0] * q[1] - c2[1] * q[0]}, kd = c2[0] * q[0] + c2[1] * q[1] + c2[2] * q[2];
                    for (int k = 0; k < 3; ++k) q[k] = q[k] * tc + kx[k] * ts + c2[k] * kd * (1 - tc);
                };
                for (int i = 0; i < n; ++i) {
                    if (*(int *)(bones + i * kEntry + 8) != 2 || !Under(bones, n, i, fore) || Under(bones, n, i, hand)) continue;
                    float *m = (float *)(bones + i * kEntry + 0x10), rel[3] = {m[12] - w[0], m[13] - w[1], m[14] - w[2]};
                    Roll(m), Roll(m + 4), Roll(m + 8), Roll(rel);
                    for (int k = 0; k < 3; ++k) m[12 + k] = w[k] + rel[k];
                }
            }
        }
    }
}

// hands24: a turn (pitch about right, yaw about up, roll about forward, as FreeT) and a shift given in
// the left controller's axes, as a world transform about the point 'at' (row vectors).
// gripfwd1: the same with any axes C (rows right, forward, up, unit length); CtlAdjust = the left controller's.
void AxesAdjust(const float *C, const float *adj, const float *at, float *D);
void CtlAdjust(const float *adj, const float *at, float *D) { AxesAdjust(g_ctlF[0], adj, at, D); }
void AxesAdjust(const float *C, const float *adj, const float *at, float *D) {
    float p = adj[3] * 0.0174533f, y = adj[4] * 0.0174533f, r = adj[5] * 0.0174533f;
    float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(r), sr = sinf(r);
    float Ry[9] = {cy, sy, 0, -sy, cy, 0, 0, 0, 1}, Rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp}, Rr[9] = {cr, 0, -sr, 0, 1, 0, sr, 0, cr};
    auto M3 = [](const float *A, const float *B, float *O) {
        float t[9];
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) t[a * 3 + b] = A[a * 3] * B[b] + A[a * 3 + 1] * B[3 + b] + A[a * 3 + 2] * B[6 + b];
        memcpy(O, t, sizeof t);
    };
    float Q[9], Ct[9], R[9];
    M3(Rp, Rr, Q), M3(Ry, Q, Q);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) Ct[a * 3 + b] = C[b * 3 + a];
    M3(Ct, Q, R), M3(R, C, R);  // world turn: the controller's axes go to their turned selves
    float sc = g_ctlScale > 0.01f ? g_ctlScale : 1;
    memset(D, 0, 64), D[15] = 1;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) D[a * 4 + b] = R[a * 3 + b];
    for (int k = 0; k < 3; ++k)
        D[12 + k] = at[k] - (at[0] * R[k] + at[1] * R[3 + k] + at[2] * R[6 + k]) + (C[k] * adj[0] + C[6 + k] * adj[1] + C[3 + k] * adj[2]) / sc;
}

// hands6: follow the wanted hand pose. hb = the hand bone as the game wrote it (model space), Tt = the
// transform this arm should get, P / ip = the player object's matrix and its inverse. Gives the
// transform to use instead (Tt itself when nothing is to be smoothed).
void Follow(ArmState &a, int mode, const float *hb, const float *Tt, const float *P, const float *ip, float dt, float *out) {
    memcpy(out, Tt, 64);
    float ih[16], W[16];
    if (!InvRigid(hb, ih)) {
        a.mode = 0;
        return;
    }
    Mul(hb, Tt, W), Mul(W, P, W);  // the wanted hand, world
    float rot[9], pos[3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rot[r * 3 + c] = W[r * 4 + c];
    for (int k = 0; k < 3; ++k) pos[k] = W[12 + k] - g_camRef[k] - g_qW[k];  // walkhands2
    bool snap = a.mode == 0;
    if (!snap) {
        float d2 = 0, dot = 0;
        for (int k = 0; k < 3; ++k) d2 += (pos[k] - a.pos[k]) * (pos[k] - a.pos[k]);
        for (int k = 0; k < 9; ++k) dot += rot[k] * a.rot[k];
        if (a.mode == mode && (d2 > 0.35f * 0.35f || dot < 1.5f)) snap = true;  // a snap turn, a load: no swoop
    }
    if (a.mode != mode) a.since = snap ? 10 : 0;
    a.since += dt;
    a.mode = mode;
    float u = g_blend > 0.01f ? (float)(a.since / g_blend) : 1;
    float tau = (mode == 2 ? g_smooth : 0) + (u < 1 ? 0.07f * (1 - u) * (1 - u) : 0);
    if (snap || tau < 0.002f) {
        memcpy(a.rot, rot, sizeof rot), memcpy(a.pos, pos, sizeof pos);
        return;  // exactly the wanted pose
    }
    float k = 1 - expf(-dt / tau);
    for (int i = 0; i < 9; ++i) a.rot[i] += (rot[i] - a.rot[i]) * k;
    for (int i = 0; i < 3; ++i) a.pos[i] += (pos[i] - a.pos[i]) * k;
    // rows square and unit again
    float *x = a.rot, *y = a.rot + 3, *z = a.rot + 6;
    float lx = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (lx < 1e-4f) {
        memcpy(a.rot, rot, sizeof rot);
    } else {
        for (int i = 0; i < 3; ++i) x[i] /= lx;
        float d = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
        for (int i = 0; i < 3; ++i) y[i] -= d * x[i];
        float ly = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
        if (ly < 1e-4f) {
            memcpy(a.rot, rot, sizeof rot);
        } else {
            for (int i = 0; i < 3; ++i) y[i] /= ly;
            float c[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
            float sg = c[0] * z[0] + c[1] * z[1] + c[2] * z[2] < 0 ? -1.f : 1.f;
            for (int i = 0; i < 3; ++i) z[i] = c[i] * sg;
        }
    }
    float S[16] = {a.rot[0], a.rot[1], a.rot[2], 0, a.rot[3], a.rot[4], a.rot[5], 0, a.rot[6], a.rot[7], a.rot[8], 0,
                   a.pos[0] + g_camRef[0] + g_qW[0], a.pos[1] + g_camRef[1] + g_qW[1], a.pos[2] + g_camRef[2] + g_qW[2], 1};  // walkhands2
    Mul(ih, S, out);
    Mul(out, ip, out);
}

// Game thread, just before the player's model is resolved for drawing.
// neutralright1: the right hand's bones mirror the left's (finger bone b + 31, hand 51 for 20). Mirrored bones relate as
// F_R = B F_L X and H_R = A H_L X (X the body's mirror, A / B each bone's own axis flips, diagonal +-1), so a finger
// against its hand is R = B L A. A and B are read from the game's own skeleton, not from a pose: A from where the finger
// roots sit in each hand, B from where each bone's child sits in it (bone lengths, the same whatever the fingers do).
static float g_nrA[3], g_nrB[kFingN][3];
static BYTE *g_nrBones;
static bool g_nrOk;
static float Det3(const float *m) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
static void NeutralRight(BYTE *bones, int n, const float (*src)[16] = nullptr) {  // killstar1: src = other fingers (left hand's frame)
    if ((!src && !g_pose[2].have) || n < 75) return;
    auto M = [&](int b) { return (const float *)(bones + b * kEntry + 0x10); };
    auto Par = [&](int b) { return *(int *)(bones + b * kEntry + 4); };
    if (bones != g_nrBones) {  // work the axis flips out once per skeleton
        g_nrBones = bones, g_nrOk = false;
        for (int i = 0; i < kFingN; ++i) {  // the right hand is laid out as the left, 31 bones on
            int b = kFing0 + i, p = Par(b), want = p == 20 ? 51 : p + 31;
            if (p != 20 && (p < kFing0 || p >= b)) {  // (a bone's parent must come before it)
                Log("hands: right hand neutral: finger bone %d hangs on %d, not handled", b, p);
                return;
            }
            if (Par(b + 31) != want) {
                Log("hands: right hand neutral: the right hand's bones are not laid out as the left (bone %d)", b + 31);
                return;
            }
        }
        // the sign set (product = wantDet) that agrees best with the per-axis scores
        auto Pick = [](float *sgn, const float *sc, float wantDet) {
            float best = -1e9f;
            for (int m = 0; m < 8; ++m) {
                float c[3] = {m & 1 ? -1.f : 1.f, m & 2 ? -1.f : 1.f, m & 4 ? -1.f : 1.f};
                if (c[0] * c[1] * c[2] * wantDet < 0) continue;
                float v = c[0] * sc[0] + c[1] * sc[1] + c[2] * sc[2];
                if (v > best) best = v, memcpy(sgn, c, 12);
            }
        };
        auto Dir = [](const float *a, const float *b, float *out, float w) {  // per-axis share of the agreement of two places
            float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (la < 1e-5f || lb < 1e-5f) return;
            for (int k = 0; k < 3; ++k) out[k] += w * a[k] * b[k] / (la * lb);
        };
        auto Sg = [](float d) { return d < 0 ? -1.f : 1.f; };
        float ihl[16], ihr[16];
        if (!InvRigid(M(20), ihl) || !InvRigid(M(51), ihr)) return void(g_nrBones = nullptr);  // try again next frame
        float sc[3] = {0, 0, 0};
        for (int i = 0; i < kFingN; ++i) {  // A: where the finger roots sit in each hand
            int b = kFing0 + i;
            if (Par(b) != 20) continue;
            float l[16], r[16];
            Mul(M(b), ihl, l), Mul(M(b + 31), ihr, r);
            Dir(l + 12, r + 12, sc, 1);
        }
        Pick(g_nrA, sc, -Sg(Det3(M(51))) * Sg(Det3(M(20))));  // the body's mirror flips one way
        for (int i = 0; i < kFingN; ++i) {
            // B: each bone against the one it hangs on (a finger joint's hinge axis is the same whatever the curl),
            // and where its children sit in it (the bone's length)
            int b = kFing0 + i, p = Par(b), pr = p == 20 ? 51 : p + 31;
            const float *bp = p == 20 ? g_nrA : g_nrB[p - kFing0];
            float ipl[16], ipr[16], ib[16], ibr[16], l[16], r[16], s2[3] = {0, 0, 0};
            if (!InvRigid(M(p), ipl) || !InvRigid(M(pr), ipr) || !InvRigid(M(b), ib) || !InvRigid(M(b + 31), ibr)) return void(g_nrBones = nullptr);
            Mul(M(b), ipl, l), Mul(M(b + 31), ipr, r);
            for (int k = 0; k < 3; ++k) s2[k] = (l[4 * k] * bp[0] * r[4 * k] + l[4 * k + 1] * bp[1] * r[4 * k + 1] + l[4 * k + 2] * bp[2] * r[4 * k + 2]);
            for (int c = kFing0; c < kFing0 + kFingN; ++c) {
                if (Par(c) != b) continue;
                Mul(M(c), ib, l), Mul(M(c + 31), ibr, r);
                Dir(l + 12, r + 12, s2, 4);
            }
            Pick(g_nrB[i], s2, -Sg(Det3(M(b + 31))) * Sg(Det3(M(b))));
        }
        g_nrOk = true;
        Log("hands: right hand neutral: the left's open fingers mirrored (hand axes %+.0f %+.0f %+.0f, first finger bone %+.0f %+.0f %+.0f)",
            g_nrA[0], g_nrA[1], g_nrA[2], g_nrB[0][0], g_nrB[0][1], g_nrB[0][2]);
    }
    if (!g_nrOk) return;
    const float *hr = M(51);
    for (int i = 0; i < kFingN; ++i) {
        int b = kFing0 + 31 + i;
        if (*(int *)(bones + b * kEntry + 8) != 2 || !Under(bones, n, b, 51)) continue;
        float L[16], R[16];
        memcpy(L, src ? src[i] : g_pose[2].m[i], 64);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) R[r * 4 + c] = (r < 3 ? g_nrB[i][r] : 1.f) * L[r * 4 + c] * (c < 3 ? g_nrA[c] : 1.f);
        Mul(R, hr, (float *)(bones + b * kEntry + 0x10));
    }
}

// emptyhands5: is the game moving the empty arms itself (patting out fire ...)? Each hand's place in the head bone's
// frame (83) against a slow average of where it usually sits there: further than [hands] EmptyHandsGameAnimCm for 3
// frames = the game's animation (its own arms shown); back within half that for 0.4 s = tracked again.
volatile LONG g_emptyAnimN;
static bool EmptyGameAnim(BYTE *bones, int n) {
    static float s_base[2][3];
    static bool s_have, s_anim;
    static int s_away;
    static DWORD s_t, s_calmT, s_animT;
    static float s_max;
    static int s_logs;
    static float s_refDev;  // settle1
    static DWORD s_steadyT;
    DWORD now = GetTickCount();
    if (n <= 83) return false;
    // settle1 (DrGonzaga 24 Nov, bd149: hands frozen after the second cutscene until the map was opened): a new stretch
    // (after a cutscene, the map, a weapon) used to take the very first frame as the usual place, while the game's
    // arms were still in their end-of-scene pose; they then sat 30 cm "away" from it for good. Now it waits 0.5 s
    // (hands tracked meanwhile) before learning the usual place, like a change of stance.
    const bool fresh = now - s_t > 500;
    if (fresh) s_have = false, s_anim = false, s_away = 0, s_max = 0;  // a new stretch with no weapon
    s_t = now;
    // crouchhands1 (DrGonzaga 24 Nov, bd147: "hands are frozen when I'm crouched"): crouching moves the game's empty arms
    // ~38 cm from their standing place, which read as one of its animations for as long as he stayed down. A change of
    // stance now starts the usual place again (learnt afresh 0.5 s later, hands tracked meanwhile).
    static bool s_cr;
    static DWORD s_crT;
    const bool cr = EngineCrouched();
    if (cr != s_cr || fresh) s_cr = cr, s_crT = now ? now : 1;
    if (s_crT && now - s_crT < 500) {
        s_have = false, s_anim = false, s_away = 0;
        return false;
    }
    float ih[16];
    if (!InvRigid((const float *)(bones + 83 * kEntry + 0x10), ih)) return s_anim;
    float cur[2][3], dev = 0;
    const int hb[2] = {51, 20};
    for (int h = 0; h < 2; ++h) {
        float l[16];
        Mul((const float *)(bones + hb[h] * kEntry + 0x10), ih, l);
        for (int k = 0; k < 3; ++k) cur[h][k] = l[12 + k];
        if (s_have) {
            float dx = cur[h][0] - s_base[h][0], dy = cur[h][1] - s_base[h][1], dz = cur[h][2] - s_base[h][2];
            dev = fmaxf(dev, sqrtf(dx * dx + dy * dy + dz * dz));
        }
    }
    if (!s_have) {
        memcpy(s_base, cur, sizeof s_base), s_have = true;
        return s_anim;
    }
    if (dev > s_max) s_max = dev;
    if (!s_anim) {
        s_away = dev > g_emptyAway ? s_away + 1 : 0;
        if (s_away >= 3) {
            s_anim = true, s_calmT = 0, s_animT = now, s_refDev = dev, s_steadyT = now;
            if (s_logs < 40) ++s_logs, Log("hands: no weapon: the game moved its own arms %.0f cm from where they usually sit: its own arms shown", dev * 100);
        } else if (dev < g_emptyAway * 0.5f)  // the usual place follows slowly (about 2 s), never during an animation
            for (int h = 0; h < 2; ++h)
                for (int k = 0; k < 3; ++k) s_base[h][k] += (cur[h][k] - s_base[h][k]) * 0.008f;
    } else if (s_anim && (fabsf(dev - s_refDev) > 0.03f ? (s_refDev = dev, s_steadyT = now, false) : now - s_steadyT > 2000)) {
        // settle1: "away" but not moving for 2 s is not one of the game's animations (those move: patting out flames):
        // the arms have a new usual place. Learnt again; hands on the controllers.
        s_anim = false, s_away = 0, s_have = false;
        if (s_logs < 40) ++s_logs, Log("hands: no weapon: the game's arms stayed %.0f cm away without moving for 2 s: a new usual place, hands on the controllers again", dev * 100);
    } else if (dev < g_emptyAway * 0.5f) {
        if (!s_calmT) s_calmT = now;
        if (now - s_calmT > 400) {
            s_anim = false, s_away = 0;
            if (s_logs < 40) ++s_logs, Log("hands: no weapon: the game's arm animation is over after %.1f s: hands on the controllers again", (now - s_animT) * 0.001f);
        }
    } else s_calmT = 0;
    static DWORD s_rep;
    if (now - s_rep > 10000 && s_logs < 40) {  // bd126: detailed log only
        s_rep = now, ++s_logs;
        LogV("hands: no weapon: the game's arms were at most %.0f cm from their usual place in the last stretch (limit %.0f)", s_max * 100, g_emptyAway * 100);
        s_max = 0;
    }
    return s_anim;
}

bool g_coreWrote;  // lefty2: ApplyCore moved the arms this call
void ApplyCore(BYTE *comp, BYTE *bones, int n) {
    if (!g_on || CyberEyeOn() /* cybereye1: the game's own arms on the flat screen */ || !g_frameOk || !BodyAnyShown() || GetTickCount() - g_frameTick > 60) return;
    int gb = g_frameBone;
    if (n != 120 || (gb != 51 && gb != 74 && gb != 20)) return;
    // hands10 (bd38-bd40: a nocked later arrow followed the right controller, and bd40's on / off
    // switching flashed): until the mod knows where the game rests a hand arrow on the bow it must
    // read that from the game's own arm pose. So with the bow out the arms stay the game's (hidden)
    // until that is learnt; it is saved ([bowstring] HandArrowRest), so this happens once.
    const bool star = gb == 20 && !ZipNow() && StarName(GunAttachName());  // killstar1
    if (gb == 20 && !star && !ZipNow() && !BowStringHandRestKnown()) {
        InterlockedIncrement(&g_bowWait);
        return;
    }
    auto Par = [&](int b) { return *(int *)(bones + b * kEntry + 4); };
    if (Par(51) != 50 || Par(50) != 49 || Par(49) != 48 || Par(20) != 19 || Par(19) != 18 || Par(18) != 17 || Par(74) != 51 ||
        Par(30) != 20 || Par(26) != 20 || Par(35) != 34 || Par(34) != 20 || Par(61) != 51 || Par(57) != 51 || Par(66) != 65 || Par(65) != 51) return;
    float *anchor = (float *)(bones + gb * kEntry + 0x10);
    if (!memcmp(anchor, g_wrote, 64)) {  // the animation did not write this frame: already ours
        InterlockedIncrement(&g_skippedTwice);
        g_liveTick = GetTickCount();
        return;
    }
    float ia[16], ip[16], T[16], TL[16], TR[16];
    if (!InvRigid(anchor, ia) || !InvRigid(g_playerNow, ip)) {
        InterlockedIncrement(&g_notRigid);
        return;
    }
    if (!g_saved) g_saved = (float (*)[16])VirtualAlloc(nullptr, kMaxBones * 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_saved) {  // hands7: the game's own pose, to put back before the game reads the bones again
        for (int i = 0; i < n; ++i) memcpy(g_saved[i], bones + i * kEntry + 0x10, 64);
        g_savedBones = bones, g_savedN = n;
    }
    {  // hands24: capture the left hand's fingers as the game has them now
        if (g_poseCapSlot >= 0 && (LONG)(GetTickCount() - g_poseCapAt) >= 0)  // capture2: the timer ran out
            InterlockedExchange(&g_poseCapReq, InterlockedExchange(&g_poseCapSlot, -1)), g_capBuzzUntil = GetTickCount() + 400;
        LONG slot = InterlockedExchange(&g_poseCapReq, -1);
        float ih[16];
        if (slot >= 0 && slot < 4 && n > kFing0 + kFingN && InvRigid((const float *)(bones + 20 * kEntry + 0x10), ih)) {
            for (int i = 0; i < kFingN; ++i) Mul((const float *)(bones + (kFing0 + i) * kEntry + 0x10), ih, g_pose[slot].m[i]);
            g_pose[slot].have = g_pose[slot].use = g_pose[slot].changed = true;
            InterlockedExchange(&g_dirty, 1);
            Log("hands: left hand finger pose captured for %s", slot == 3 ? "the zipline" : slot == 2 ? "the free left hand (neutral)" : slot ? "the pistol grip" : "the C4");
        }
    }
    Mul(ia, g_gunNow, T);
    Mul(T, ip, T);
    const bool zip = ZipNow();  // zip1
    bool bow = gb == 20 && !zip && !star;  // killstar1: the Killstar also hangs on bone 20
    {
        // hands20 (DrGonzaga 13 Nov, bd50: "the forearm rotates as soon as tension is applied to the bow,
        // after nocking. Before nocking, if I simulate the movement, it looks / feels good - no
        // twisting"). The only thing tension changes is the game's own draw animation on the right arm:
        // it poses that hand and forearm differently, and the free hand is fitted to the controller
        // from that pose. So while an arrow is nocked the right arm keeps the pose it had while the
        // arrow was carried: remembered every frame before the nock, written over the game's here
        // (before anything is measured from it). The game's own pose is still what gets put back.
        static float (*s_snap)[16];
        static bool s_snapOk;
        if (!s_snap) s_snap = (float (*)[16])VirtualAlloc(nullptr, kMaxBones * 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        // hands21 (DrGonzaga 13 Nov, bd51: nock and pull great; "un-nocking while holding the arrow, the
        // forearm goes through a quick twist animation again then goes back to normal"): the game also
        // plays an animation when the draw ends. The remembered pose stays on after the nock is over
        // until the game's own right arm is back near it (forearm against the hand within about 15
        // degrees, for 12 frames running) or 2 s have passed; nothing is remembered meanwhile.
        static bool s_hold;
        static DWORD s_holdT;
        static int s_calm;
        if (!bow || !s_snap) s_snapOk = false, s_hold = false;
        else {
            bool nocked = BowArrowMode() == 3;
            DWORD nowT = GetTickCount();
            if (nocked && s_snapOk) s_hold = true, s_holdT = nowT, s_calm = 0;
            else if (s_hold) {
                auto Local = [&](const float *hm2, const float *fm2, float *o) {  // forearm's place as the hand sees it
                    float v[3] = {fm2[12] - hm2[12], fm2[13] - hm2[13], fm2[14] - hm2[14]}, l = 0;
                    for (int r = 0; r < 3; ++r) o[r] = v[0] * hm2[4 * r] + v[1] * hm2[4 * r + 1] + v[2] * hm2[4 * r + 2], l += o[r] * o[r];
                    l = sqrtf(l);
                    if (l > 1e-4f)
                        for (int r = 0; r < 3; ++r) o[r] /= l;
                };
                float a[3], b2[3];
                Local((const float *)(bones + 51 * kEntry + 0x10), (const float *)(bones + 50 * kEntry + 0x10), a);
                Local(s_snap[51], s_snap[50], b2);
                s_calm = a[0] * b2[0] + a[1] * b2[1] + a[2] * b2[2] > 0.966f ? s_calm + 1 : 0;
                if (s_calm >= 12 || nowT - s_holdT > 2000) s_hold = false;
            }
            bool keep = s_hold && s_snapOk;
            for (int i = 0; i < n; ++i) {
                if (*(int *)(bones + i * kEntry + 8) != 2 || !Under(bones, n, i, 48)) continue;
                float *m = (float *)(bones + i * kEntry + 0x10);
                if (keep) memcpy(m, s_snap[i], 64);
                else if (!nocked) memcpy(s_snap[i], m, 64);
            }
            if (!keep && !nocked) s_snapOk = true;
        }
    }
    auto Len = [&](int a, int b) {
        const float *p = (const float *)(bones + a * kEntry + 0x10) + 12, *q = (const float *)(bones + b * kEntry + 0x10) + 12;
        return sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    };
    float upR = Len(49, 50), upL = Len(18, 19);
    // 1 = with the gun, 2 = free on its own controller, 0 = not shown
    int modeR = 1, modeL = g_left ? 1 : 0;
    bool c4L = false;  // hands23: the left hand is on the C4 charge (its own transform, TL); hands24: or locked to a pistol
    int poseSlot = -1;  // hands24: which captured finger pose applies (0 C4, 1 pistol)
    g_pistolLocked = false;
    // sign1 (DrGonzaga 16 Nov: "the occasional 'devil horn' animation" is gone): the neutral left hand (hands25)
    // replaced the game's own fingers every frame, so the game's finger signs never showed. The game's left
    // fingers are now watched against their usual shape (a slow average, per weapon): while they are well
    // away from it (a sign, a reload) the game's own fingers stay on the hand instead of the neutral pose.
    bool gameSign = false;
    if (n > kFing0 + kFingN) {
        static float s_base[kFingN][3];
        static bool s_have;
        static int s_away, s_back;
        static bool s_sign;
        static char s_gun[48];
        static DWORD s_t;
        const char *gn = GunAttachName();
        DWORD nowT = GetTickCount();
        if (strncmp(s_gun, gn, 47) || nowT - s_t > 500) s_have = false, s_sign = false, s_away = s_back = 0, strncpy(s_gun, gn, 47);
        s_t = nowT;
        float ih[16];
        if (InvRigid((const float *)(bones + 20 * kEntry + 0x10), ih)) {
            float cur[kFingN][3], dev = 0;
            for (int i = 0; i < kFingN; ++i) {
                float l[16];
                Mul((const float *)(bones + (kFing0 + i) * kEntry + 0x10), ih, l);
                cur[i][0] = l[12], cur[i][1] = l[13], cur[i][2] = l[14];
                if (s_have) {
                    float dx = cur[i][0] - s_base[i][0], dy = cur[i][1] - s_base[i][1], dz = cur[i][2] - s_base[i][2];
                    float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (d > dev) dev = d;
                }
            }
            if (!s_have) memcpy(s_base, cur, sizeof s_base), s_have = true;
            if (!s_sign) {
                s_away = dev > g_signAway ? s_away + 1 : 0;
                static int s_n;
                if (s_away >= 3 && !s_sign && (s_sign = true, s_back = 0, s_n++ < 30)) Log("hands: the game's left fingers moved %.1f cm from their usual shape: shown as the game has them", dev * 100);
            } else {
                s_back = dev < g_signAway * 0.5f ? s_back + 1 : 0;
                if (s_back >= 10) s_sign = false, s_away = 0;
            }
            if (!s_sign && dev < g_signAway * 0.5f)  // the usual shape follows slowly (about 3 s), never during a sign
                for (int i = 0; i < kFingN; ++i)
                    for (int k = 0; k < 3; ++k) s_base[i][k] += (cur[i][k] - s_base[i][k]) * 0.005f;
        }
        gameSign = s_sign && g_signShow;
    }
    auto Neutral = [&]() {  // hands25: the free left hand's fingers, before the hand is fitted to the controller
        if (!g_neutral || !g_pose[2].have || n <= kFing0 + kFingN || gameSign) return;
        const float *hb2 = (const float *)(bones + 20 * kEntry + 0x10);
        for (int i = 0; i < kFingN; ++i)
            if (*(int *)(bones + (kFing0 + i) * kEntry + 8) == 2 && Under(bones, n, kFing0 + i, 20))
                Mul(g_pose[2].m[i], hb2, (float *)(bones + (kFing0 + i) * kEntry + 0x10));
    };
    if (g_emptyFrame && !bow && !zip && !g_openSect && g_emptyGuard && EmptyGameAnim(bones, n)) {
        InterlockedIncrement(&g_emptyAnimN);
        return;  // emptyhands5: the game is animating the empty arms: its own arms show (HandsGameArmsNow)
    }
    if (g_emptyFrame && !bow && !zip) {
        // emptyhands1 (DrGonzaga 17 Nov: "your hands are empty for that first section on foot- can we add motion controls
        // to the hands"): no weapon: both hands on their controllers, the game's own right fingers, the neutral left.
        // neutralright1 (DrGonzaga 17 Nov, bd99: "make the right hand pose the same as the left, for the hands at neutral?
        // It's clenched shut"): the neutral left fingers, mirrored, on the right hand (before it is fitted).
        if (g_neutral) NeutralRight(bones, n);
        modeR = FreeT(bones, 51, 61, 57, 66, 1, ip, TR) ? 2 : 0;
        if (g_left) Neutral();
        modeL = g_left && FreeT(bones, 20, 30, 26, 35, 0, ip, TL) ? 2 : 0;
        poseSlot = modeL == 2 && g_neutral ? 2 : -1;
        static DWORD s_eT;
        if (GetTickCount() - s_eT > 5000) LogV("hands: no weapon: hands on the controllers (right %d, left %d)", modeR, modeL);
        s_eT = GetTickCount();
    } else if (star) {
        // killstar1: the game's left fingers hold the star; that grip goes on the right hand (read before Neutral()
        // changes the left fingers), then both hands go on their own controllers.
        static float s_grip[kFingN][16];
        float ih[16];
        bool gripOk = g_starMirror && n > kFing0 + kFingN + 31 && InvRigid((const float *)(bones + 20 * kEntry + 0x10), ih);
        if (gripOk) {
            for (int i = 0; i < kFingN; ++i) Mul((const float *)(bones + (kFing0 + i) * kEntry + 0x10), ih, s_grip[i]);
            NeutralRight(bones, n, s_grip);
        }
        modeR = FreeT(bones, 51, 61, 57, 66, 1, ip, TR) ? 2 : 0;
        if (g_left) Neutral();
        modeL = g_left && FreeT(bones, 20, 30, 26, 35, 0, ip, TL) ? 2 : 0;
        poseSlot = modeL == 2 && g_neutral ? 2 : -1;
        static DWORD s_sT;
        if (GetTickCount() - s_sT > 10000) Log("hands: Killstar: in the right hand (%s), left hand %s", gripOk ? "the game's grip mirrored" : "the game's right fingers", modeL ? "free on its controller" : "not shown");
        s_sT = GetTickCount();
    } else if (bow) {
        modeL = 1;
        modeR = FreeT(bones, 51, 61, 57, 66, 1, ip, TR) ? 2 : 0;
    } else if (zip) {  // zip1: the left hand hangs from the line, at its place from the head
        float Wz[16];
        const bool own = g_pose[3].have && g_pose[3].use && n > kFing0 + kFingN;
        if (own) {
            const float *hb2 = (const float *)(bones + 20 * kEntry + 0x10);
            for (int i = 0; i < kFingN; ++i)
                if (*(int *)(bones + (kFing0 + i) * kEntry + 8) == 2 && Under(bones, n, kFing0 + i, 20))
                    Mul(g_pose[3].m[i], hb2, (float *)(bones + (kFing0 + i) * kEntry + 0x10));
        } else Neutral();
        if (ZipFrame(Wz) && FreeT(bones, 20, 30, 26, 35, 0, ip, TL, nullptr, Wz)) c4L = true, poseSlot = own ? 3 : g_neutral ? 2 : -1;
        else modeL = 0;
        if (gb == 20) modeR = FreeT(bones, 51, 61, 57, 66, 1, ip, TR) ? 2 : 0;  // the hook is "the gun" for a moment: the right hand stays free
        static DWORD s_zipT;
        if (GetTickCount() - s_zipT > 3000)
            Log("hands: zipline hand shown (%s; fingers: %s; hand %s)", g_zipPreview ? "menu preview" : "ziplining", own ? "captured" : "neutral", modeL ? "placed" : "could not be placed");
        s_zipT = GetTickCount();
    } else if (g_c4) {
        bool charge = g_left && GetTickCount() - g_c4Tick < 60;
        float Gm[16];
        if (charge && g_pose[0].have && g_pose[0].use && n > kFing0 + kFingN) {  // c4rigid1: the captured fingers first, the hand is fitted from them
            const float *hb2 = (const float *)(bones + 20 * kEntry + 0x10);
            for (int i = 0; i < kFingN; ++i)
                if (*(int *)(bones + (kFing0 + i) * kEntry + 8) == 2 && Under(bones, n, kFing0 + i, 20))
                    Mul(g_pose[0].m[i], hb2, (float *)(bones + (kFing0 + i) * kEntry + 0x10));
        }
        if (charge && FreeT(bones, 20, 30, 26, 35, 0, ip, TL, Gm)) {
            // c4learn2 (DrGonzaga 15 Nov, bd63: "the default C4 was really far away"): bd62's log: learnt as the C4 came
            // out, the hold was 5 / 7 / 2 cm; learnt again with the charge already riding on the hand it was 31 / 50 /
            // 58 cm, i.e. once we place the charge the game's "own" pose is no longer its own. So the hold is only
            // learnt as the C4 comes out, gunattach leaves the charge alone meanwhile, and a hold further than 25 cm
            // from the hand is refused.
            static float s_sum[16];
            if (!g_c4KHave) {  // the game's own hold: charge against the hand's finger frame, averaged
                float Gw[16], iGw[16], K[16];
                Mul(Gm, g_playerNow, Gw);
                if (g_c4LearnN < 0) ++g_c4LearnN;
                else if (InvRigid(Gw, iGw)) {
                    Mul(g_c4Game, iGw, K);
                    if (!g_c4LearnN) memset(s_sum, 0, sizeof s_sum);
                    for (int k = 0; k < 16; ++k) s_sum[k] += K[k];
                    if (++g_c4LearnN >= 30) {
                        float *x = s_sum, *y = s_sum + 4, *z = s_sum + 8;
                        float lx = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                        for (int k = 0; k < 3; ++k) x[k] /= lx > 1e-6f ? lx : 1;
                        float dd = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
                        for (int k = 0; k < 3; ++k) y[k] -= dd * x[k];
                        float ly = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                        for (int k = 0; k < 3; ++k) y[k] /= ly > 1e-6f ? ly : 1;
                        z[0] = x[1] * y[2] - x[2] * y[1], z[1] = x[2] * y[0] - x[0] * y[2], z[2] = x[0] * y[1] - x[1] * y[0];
                        for (int k = 0; k < 3; ++k) s_sum[12 + k] /= g_c4LearnN;
                        s_sum[3] = s_sum[7] = s_sum[11] = 0, s_sum[15] = 1;
                        float sc2 = g_ctlScale > 0.01f ? g_ctlScale : 1;
                        float farM = sqrtf(s_sum[12] * s_sum[12] + s_sum[13] * s_sum[13] + s_sum[14] * s_sum[14]) * sc2;
                        g_c4LearnN = -6;
                        if (farM < 0.25f && lx > 1e-3f && ly > 1e-3f) {
                            memcpy(g_c4K, s_sum, 64), g_c4KHave = true, g_c4Built = false, g_c4Bad = 0;
                            InterlockedExchange(&g_dirty, 1), InterlockedExchange(&g_saveSoon, 1);
                            Log("hands: C4 hold learnt from the game (charge %.1f %.1f %.1f cm from the hand's frame); the charge now rides on the posed hand",
                                g_c4K[12] * 100, g_c4K[13] * 100, g_c4K[14] * 100);
                        } else if (++g_c4Bad >= 3) {
                            static const float kHold[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.052f, 0.069f, 0.017f, 1};
                            memcpy(g_c4K, kHold, 64), g_c4KHave = g_c4Built = true, g_c4Bad = 0;
                            for (int k = 0; k < 3; ++k) g_c4K[12 + k] /= sc2;
                            Log("hands: C4 hold NOT learnt (the game's charge was %.0f cm from the hand, three tries); a built-in place is used this session, turn it with the charge sliders",
                                farM * 100);
                        } else Log("hands: C4 hold refused (the game's charge was %.1f %.1f %.1f cm from the hand's frame); trying again", s_sum[12] * 100, s_sum[13] * 100, s_sum[14] * 100);
                    }
                }
            }
            bool adj = false;
            for (int k = 0; k < 6; ++k) adj |= g_c4Adj[k] != 0;
            if (adj) {  // hands24: DrGonzaga's fit of the hand on the charge
                float hw[16], D[16];
                Mul((const float *)(bones + 20 * kEntry + 0x10), TL, hw), Mul(hw, g_playerNow, hw);
                CtlAdjust(g_c4Adj, hw + 12, D);
                Mul(TL, g_playerNow, TL), Mul(TL, D, TL), Mul(TL, ip, TL);
            }
            poseSlot = 0;
        } else if (charge) {
            charge = false, modeL = 0;
        } else {
            if (g_left) Neutral();
            modeL = g_left && FreeT(bones, 20, 30, 26, 35, 0, ip, TL) ? 2 : 0;
        }
        c4L = charge;
    } else if (g_left && (g_pistolGrip || g_pistolAuto) && (*PistolGunOwn() >= 0 ? *PistolGunOwn() != 0 : PistolGunByName(GunAttachName()))) {
        // hands24: a pistol. Free on its controller; left grip near the pistol locks it to the gun.
        static bool s_lock;
        static float s_rel[16];
        const float *hb = (const float *)(bones + 20 * kEntry + 0x10);
        float iG[16], ib[16], Hw[16], sc = g_ctlScale > 0.01f ? g_ctlScale : 1;
        static int s_open;
        static bool s_auto;
        if (g_neutral && !g_pose[2].have && !s_lock && n > kFing0 + kFingN && InvRigid(hb, ib)) {  // hands25: learn the open hand
            if (++s_open >= 90) {
                for (int i = 0; i < kFingN; ++i) Mul((const float *)(bones + (kFing0 + i) * kEntry + 0x10), ib, g_pose[2].m[i]);
                g_pose[2].have = g_pose[2].use = g_pose[2].changed = true, s_open = 0;
                InterlockedExchange(&g_dirty, 1), InterlockedExchange(&g_saveSoon, 1);
                Log("hands: neutral left hand learnt from the pistol's open left hand (kept for every gun; capture your own on the hands page)");
            }
        } else s_open = 0;
        Neutral();
        bool freeOk = FreeT(bones, 20, 30, 26, 35, 0, ip, TL), gOk = InvRigid(g_gunNow, iG) && InvRigid(hb, ib);
        if (freeOk) Mul(hb, TL, Hw), Mul(Hw, g_playerNow, Hw);  // the free hand, world
        bool autoJoin = g_pistolAuto && TwoHandSteady();  // hands25: steadied = the left hand joins by itself
        bool gripJoin = g_pistolGrip && g_lgrip;
        if ((!gripJoin && !autoJoin) || !gOk) s_lock = false;
        else if (!s_lock && freeOk) {
            float d[3] = {Hw[12] - g_gunNow[12], Hw[13] - g_gunNow[13], Hw[14] - g_gunNow[14]};
            if (autoJoin || sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * sc < g_pistolReach) {
                if (autoJoin != s_auto || !s_lock) LogV("hands: left hand on the pistol (%s)", autoJoin ? "by itself, steadied" : "left grip");
                s_auto = autoJoin;
                s_lock = true;
                if (g_pgHave) {
                    memcpy(s_rel, g_pgRel, 64);
                    for (int k = 0; k < 3; ++k) s_rel[12 + k] /= sc;
                } else Mul(Hw, iG, s_rel);
            }
        }
        if (InterlockedExchange(&g_pgSaveReq, 0) && gOk && (s_lock || freeOk)) {  // menu: this is the place
            if (s_lock) memcpy(g_pgRel, s_rel, 64);
            else Mul(Hw, iG, g_pgRel);
            for (int k = 0; k < 3; ++k) g_pgRel[12 + k] *= sc;
            g_pgHave = true;
            InterlockedExchange(&g_dirty, 1);
            Log("hands: the left hand's place on the pistol saved (%.1f %.1f %.1f cm from the gun's own point)", g_pgRel[12] * 100, g_pgRel[13] * 100, g_pgRel[14] * 100);
        }
        if (s_lock) Mul(ib, s_rel, TL), Mul(TL, g_gunNow, TL), Mul(TL, ip, TL), c4L = true, poseSlot = 1;
        else if (freeOk) modeL = 2, poseSlot = g_neutral ? 2 : -1;
        g_pistolLocked = s_lock;
    } else if (g_left && g_freeLeft && !g_twoHand && (Neutral(), FreeT(bones, 20, 30, 26, 35, 0, ip, TL))) {
        modeL = 2, poseSlot = g_neutral ? 2 : -1;
    }
    {  // gripfwd1: the left hand's place on a gun held with both grips, per gun
        const char *gn = GunAttachName();
        if (strncmp(gn ? gn : "", g_glGun, 47)) GunLeftLoad(gn);
        if (g_glCapOn && (LONG)(GetTickCount() - g_glCapAt) >= 0) InterlockedExchange(&g_glCapOn, 0), InterlockedExchange(&g_glCapNow, 1);
        bool take = InterlockedExchange(&g_glCapNow, 0) != 0;
        const bool onGun = modeL == 1 && !c4L && !bow && !star && !zip && !g_c4 && !g_emptyFrame;
        g_glOnGun = onGun;
        float C[9];
        bool axOk = true;
        for (int r = 0; r < 3; ++r) {
            float l = sqrtf(g_gunNow[r * 4] * g_gunNow[r * 4] + g_gunNow[r * 4 + 1] * g_gunNow[r * 4 + 1] + g_gunNow[r * 4 + 2] * g_gunNow[r * 4 + 2]);
            if (l < 1e-6f) axOk = false;
            for (int k = 0; k < 3; ++k) C[r * 3 + k] = axOk ? g_gunNow[r * 4 + k] / l : 0;
        }
        if (onGun && axOk) {
            const float *hb = (const float *)(bones + 20 * kEntry + 0x10);
            float hw[16], TLf[16], fw[16], sc = g_ctlScale > 0.01f ? g_ctlScale : 1;
            Mul(hb, T, hw), Mul(hw, g_playerNow, hw);  // the game's left wrist on our gun, world
            bool freeOk = FreeT(bones, 20, 30, 26, 35, 0, ip, TLf);
            if (freeOk) Mul(hb, TLf, fw), Mul(fw, g_playerNow, fw);  // where the left controller would put it
            float dg[3] = {hw[12] - g_gunNow[12], hw[13] - g_gunNow[13], hw[14] - g_gunNow[14]};
            // the gun model's own axes are not known for sure: on a long gun the left hand is in front of the gun's point
            // (the right hand), so if its forward points the other way, turn the axes half round about up
            if (dg[0] * C[3] + dg[1] * C[4] + dg[2] * C[5] < 0)
                for (int k = 0; k < 6; ++k) C[k] = -C[k];
            static const int kAx[3] = {0, 2, 1};  // right, up, forward -> rows of C
            for (int i = 0; i < 3; ++i) {
                const float *a = C + kAx[i] * 3;
                g_glGame[i] = (dg[0] * a[0] + dg[1] * a[1] + dg[2] * a[2]) * sc * 100;
                if (freeOk) g_glMine[i] = ((fw[12] - g_gunNow[12]) * a[0] + (fw[13] - g_gunNow[13]) * a[1] + (fw[14] - g_gunNow[14]) * a[2]) * sc * 100;
            }
            static char s_logGun[48];
            if (strncmp(s_logGun, g_glGun, 47)) {
                memcpy(s_logGun, g_glGun, 48);
                Log("hands: %s held with both hands: the game's left wrist is %.1f cm right, %.1f up, %.1f forward of the gun's point; your left controller's wrist %.1f, %.1f, %.1f",
                    g_glGun, g_glGame[0], g_glGame[1], g_glGame[2], g_glMine[0], g_glMine[1], g_glMine[2]);
            }
            if (take) {
                if (freeOk) {
                    for (int i = 0; i < 3; ++i) g_gl[i] = (g_glMine[i] - g_glGame[i]) / 100;
                    g_glTouched = true, g_glBuzzUntil = GetTickCount() + 400;
                    InterlockedExchange(&g_dirty, 1), InterlockedExchange(&g_saveSoon, 1);
                    Log("hands: %s: left hand's place taken from the controller: %.1f cm right, %.1f up, %.1f forward of the game's", g_glGun, g_gl[0] * 100, g_gl[1] * 100, g_gl[2] * 100);
                } else Log("hands: %s: left hand's place NOT taken (the left controller was not tracked)", g_glGun);
            }
            bool adj = false;
            for (int k = 0; k < 6; ++k) adj |= g_gl[k] != 0;
            if (adj) {
                float D[16], W[16];
                AxesAdjust(C, g_gl, hw + 12, D);
                Mul(T, g_playerNow, W), Mul(W, D, W), Mul(W, ip, TL);
                c4L = true;  // Follow uses TL for the left arm
            }
        } else if (take) Log("hands: left hand's place NOT taken: the left hand was not holding a gun with both grips at that moment");
    }
    if (!bow && g_scopeHide && g_scoped) modeL = 0;  // hands4: zoomed, the left hand would cover the scope
    if (modeL == 2) InterlockedIncrement(&g_freeL);
    if (modeR == 2) InterlockedIncrement(&g_freeR);
    // hands6: time step, the smoothed head, and each arm's followed pose
    LONGLONG qn = Qpc();
    float dt = g_lastQpc && g_qpf > 0 ? (float)((qn - g_lastQpc) / g_qpf) : 0.011f;
    g_lastQpc = qn;
    if (dt > 0.25f) g_arm[0].mode = g_arm[1].mode = 0, g_eyeSOk = false;  // back after a break: no swoop from the old place
    dt = dt < 0.001f ? 0.001f : dt > 0.05f ? 0.05f : dt;
    {
        float e[3] = {g_eye[0] - g_camRef[0] - g_qW[0], g_eye[1] - g_camRef[1] - g_qW[1], g_eye[2] - g_camRef[2] - g_qW[2]}, d2 = 0;  // walkhands2
        for (int k = 0; k < 3; ++k) d2 += (e[k] - g_eyeS[k]) * (e[k] - g_eyeS[k]);
        float k = !g_eyeSOk || d2 > 0.25f ? 1 : 1 - expf(-dt / 0.15f);
        for (int i = 0; i < 3; ++i) g_eyeS[i] += (e[i] - g_eyeS[i]) * k;
        g_eyeSOk = g_headOk;
    }
    float AR[16], AL[16];
    if (modeR) Follow(g_arm[0], modeR, (const float *)(bones + 51 * kEntry + 0x10), modeR == 2 ? TR : T, g_playerNow, ip, dt, AR);
    else g_arm[0].mode = 0;
    if (modeL) Follow(g_arm[1], modeL, (const float *)(bones + 20 * kEntry + 0x10), modeL == 2 || c4L ? TL : T, g_playerNow, ip, dt, AL);
    else g_arm[1].mode = 0;
    for (int a = 0; a < 2; ++a) {
        int md = a ? modeL : modeR;
        float want = md == 2 || (md == 1 && g_elbowGun) ? 1.f : 0.f, k = g_blend > 0.01f ? dt / (g_blend * 0.6f) : 1;
        if (!md) g_arm[a].elbow = 0;
        else if (g_arm[a].elbow < want) g_arm[a].elbow = fminf(want, g_arm[a].elbow + k);
        else g_arm[a].elbow = fmaxf(want, g_arm[a].elbow - k);
    }
    if (poseSlot >= 0 && modeL && g_pose[poseSlot].have && g_pose[poseSlot].use && n > kFing0 + kFingN) {  // hands24
        const float *hb = (const float *)(bones + 20 * kEntry + 0x10);
        for (int i = 0; i < kFingN; ++i)
            if (*(int *)(bones + (kFing0 + i) * kEntry + 8) == 2 && Under(bones, n, kFing0 + i, 20))
                Mul(g_pose[poseSlot].m[i], hb, (float *)(bones + (kFing0 + i) * kEntry + 0x10));
    }
    for (int i = 0; i < n; ++i) {
        if (*(int *)(bones + i * kEntry + 8) != 2) continue;
        bool right = Under(bones, n, i, 50), left = Under(bones, n, i, 19);
        if (!right && !left) continue;
        float *m = (float *)(bones + i * kEntry + 0x10);
        int md = right ? modeR : modeL;
        if (md) Mul(m, right ? AR : AL, m);
    }
    if (modeR) Elbow(bones, n, 50, 51, upR, 1, ip, g_arm[0].elbow, bow);
    if (modeL) Elbow(bones, n, 19, 20, upL, -1, ip, g_arm[1].elbow);
    // What is not shown goes to one point each: an arm's collar, upper arm and twist bones to its
    // own elbow; a whole arm that is off, and the spine (the sleeves' top edge hangs on it), to the
    // elbow of the arm that is always there.
    float elR[3], elL[3], keep[3];
    memcpy(elR, (const float *)(bones + 50 * kEntry + 0x10) + 12, 12), memcpy(elL, (const float *)(bones + 19 * kEntry + 0x10) + 12, 12);
    memcpy(keep, modeR ? elR : elL, 12);
    for (int i = 0; i < n; ++i) {
        if (*(int *)(bones + i * kEntry + 8) != 2) continue;
        float *m = (float *)(bones + i * kEntry + 0x10);
        bool rightLow = Under(bones, n, i, 50), leftLow = Under(bones, n, i, 19);
        bool rightUp = i == 48 || i == 49 || (Par(i) == 49 && i != 50), leftUp = i == 17 || i == 18 || (Par(i) == 18 && i != 19);
        if (rightLow || rightUp) {
            if (!modeR) Shrink(m, keep);
            else if (rightUp) Shrink(m, elR);
        } else if (leftLow || leftUp) {
            if (!modeL) Shrink(m, keep);
            else if (leftUp) Shrink(m, elL);
        } else if (g_chest && (i == 14 || i == 15 || i == 16)) {
            Shrink(m, keep);
        }
    }
    memcpy(g_wrote, anchor, 64), g_wroteBone = gb;
    *(BYTE *)(comp + 0x8c) |= 3;  // needs resolving (as the game's own bone writers set it)
    g_liveTick = GetTickCount();
    InterlockedIncrement(&g_applied);
    g_coreWrote = true;
}

// lefty2 (DrGonzaga 22 Nov, bd122 left-handed test: "it switched arms around - I was hoping just to mirror the
// poses"). In left-handed mode the gun is on the left controller; bd122 put the model's RIGHT arm on it (crossed
// over) and the model's left arm on the right controller. Now the whole arm solve is done in a mirror world and
// the result mirrored back, so the model's LEFT hand holds the gun exactly as the right hand would hold the
// mirrored gun (same grip, fingers mirrored), and the model's right hand is the free hand:
//   1. mirror plane: through the eye, across the body's right axis (the head data is then unchanged by it);
//   2. the gun pose and both controller frames are mirrored into that world (each turned about its own sideways
//      axis so it stays a proper turn: a left controller becomes a right one), and the normal right-handed solve
//      runs on them;
//   3. every arm bone is swapped with its twin on the other arm and mirrored back: B' = C * twin * (P * Mirror * P^-1),
//      where C flips the bone axes the skeleton's two sides differ in. C is worked out once from the skeleton
//      itself (each bone's offset from its parent, left against right) and logged; if the two sides don't pair
//      up as mirror images, nothing is mirrored (bd122's behaviour) and the log says so.
// [vr] LeftHandedMirrorArms (1) switches it off.
bool g_mirArms = true;
int g_mirState;     // 0 not worked out, 1 ready, -1 not possible
float g_mirC[3];    // C's row signs
int g_twin[kMaxBones];  // the other arm's bone (-1 none)

int TwinOf(int i) {
    if (i >= 48 && i <= 73) return i - 31;
    if (i >= 75 && i <= 79) return i - 32;
    if (i >= 17 && i <= 42) return i + 31;
    if (i >= 43 && i <= 47) return i + 32;
    return -1;
}
void LocalOff(BYTE *bones, int c, int p, float *o) {  // bone c's place in bone p's frame
    const float *mc = (const float *)(bones + c * kEntry + 0x10), *mp = (const float *)(bones + p * kEntry + 0x10);
    float d[3] = {mc[12] - mp[12], mc[13] - mp[13], mc[14] - mp[14]};
    for (int k = 0; k < 3; ++k) {
        const float *r = mp + 4 * k;
        float l2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        o[k] = l2 > 1e-8f ? (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / l2 : 0;
    }
}
bool MirrorReady(BYTE *bones, int n) {
    if (g_mirState) return g_mirState > 0;
    auto Par = [&](int b) { return *(int *)(bones + b * kEntry + 4); };
    int pairs = 0, bad = 0, votes[3][2] = {};
    for (int i = 0; i < n; ++i) g_twin[i] = -1;
    for (int i = 48; i <= 79; ++i) {
        if (i == 74) continue;
        int j = TwinOf(i);
        if (j < 0 || j >= n) continue;
        if (i == 48 ? Par(17) != Par(48) : TwinOf(Par(i)) != Par(j)) { ++bad; continue; }
        g_twin[i] = j, g_twin[j] = i, ++pairs;
        if (i == 48) continue;
        float a[3], b[3];
        LocalOff(bones, i, Par(i), a), LocalOff(bones, j, Par(j), b);
        float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (la < 0.003f || fabsf(la - lb) > 0.15f * la + 0.003f) continue;
        for (int k = 0; k < 3; ++k)
            if (fabsf(a[k]) > 0.3f * la && fabsf(b[k]) > 0.3f * lb) ++votes[k][a[k] * b[k] > 0 ? 0 : 1];
    }
    int known = 0, unknown = -1;
    for (int k = 0; k < 3; ++k) {
        int same = votes[k][0], flip = votes[k][1], tot = same + flip;
        if (tot >= 3 && (same >= 0.85f * tot || flip >= 0.85f * tot)) g_mirC[k] = same > flip ? 1.f : -1.f, ++known;
        else g_mirC[k] = 0, unknown = k;
    }
    if (known == 2) {  // the third follows: C must flip an odd number of axes (a mirror)
        float p = 1;
        for (int k = 0; k < 3; ++k)
            if (k != unknown) p *= g_mirC[k];
        g_mirC[unknown] = -p;
        ++known;
    }
    bool ok = pairs >= 30 && bad <= 2 && known == 3 && g_mirC[0] * g_mirC[1] * g_mirC[2] < 0;
    g_mirState = ok ? 1 : -1;
    Log("hands: left-handed arms %s: %d bone pairs (%d did not pair), axis flips %+.0f %+.0f %+.0f (votes x %d/%d, y %d/%d, z %d/%d)",
        ok ? "MIRRORED (lefty2)" : "can't be mirrored, the arms stay as in bd122", pairs, bad, g_mirC[0], g_mirC[1], g_mirC[2],
        votes[0][0], votes[0][1], votes[1][0], votes[1][1], votes[2][0], votes[2][1]);
    return ok;
}
// The mirror (4x4, row vectors) through point e across unit normal nrm.
void MirrorMat(const float *e, const float *nrm, float *M) {
    float d = e[0] * nrm[0] + e[1] * nrm[1] + e[2] * nrm[2];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) M[r * 4 + c] = (r == c ? 1.f : 0.f) - 2 * nrm[r] * nrm[c];
        M[r * 4 + 3] = 0, M[12 + r] = 2 * d * nrm[r];
    }
    M[15] = 1;
}
void MirVec(const float *v, const float *nrm, float *o) {
    float d = v[0] * nrm[0] + v[1] * nrm[1] + v[2] * nrm[2];
    for (int k = 0; k < 3; ++k) o[k] = v[k] - 2 * d * nrm[k];
}
void MirPt(const float *p, const float *e, const float *nrm, float *o) {
    float d = (p[0] - e[0]) * nrm[0] + (p[1] - e[1]) * nrm[1] + (p[2] - e[2]) * nrm[2];
    for (int k = 0; k < 3; ++k) o[k] = p[k] - 2 * d * nrm[k];
}

void Apply(BYTE *comp, BYTE *bones, int n) {
    float nrm[3] = {g_axes[0], g_axes[1], g_axes[2]}, e[3] = {g_eye[0], g_eye[1], g_eye[2]};
    float nl = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    bool mir = *VrLeftHanded() && g_mirArms && g_headOk && n == 120 && nl > 0.5f && g_frameBone != 20 && MirrorReady(bones, n);
    if (!mir) {
        g_coreWrote = false;
        ApplyCore(comp, bones, n);
        return;
    }
    for (int k = 0; k < 3; ++k) nrm[k] /= nl;
    // 2. the inputs into the mirror world
    float gun[16], ctlF[2][9], ctlP[2][3];
    memcpy(gun, g_gunNow, 64), memcpy(ctlF, g_ctlF, sizeof ctlF), memcpy(ctlP, g_ctlP, sizeof ctlP);
    {
        // the gun turned about its own sideways axis (the gun frame's right, from gunattach) and mirrored
        float ax[9], lat[3], dl[3], l = 0;
        GunAttachAxesW(ax);
        memcpy(lat, ax, 12);
        for (int k = 0; k < 3; ++k) {
            const float *r = gun + 4 * k;
            dl[k] = lat[0] * r[0] + lat[1] * r[1] + lat[2] * r[2], l += dl[k] * dl[k];
        }
        l = sqrtf(l);
        if (l < 0.5f) dl[0] = 1, dl[1] = dl[2] = 0, l = 1;
        for (int k = 0; k < 3; ++k) dl[k] /= l;
        float G[16];
        memcpy(G, gun, 64);
        for (int a = 0; a < 3; ++a) {  // local reflection across dl: row_a' = sum_b (I - 2 dl dl^T)[a][b] row_b
            float row[3] = {0, 0, 0};
            for (int b = 0; b < 3; ++b) {
                float c = (a == b ? 1.f : 0.f) - 2 * dl[a] * dl[b];
                for (int k = 0; k < 3; ++k) row[k] += c * gun[4 * b + k];
            }
            MirVec(row, nrm, G + 4 * a);
        }
        MirPt(gun + 12, e, nrm, G + 12);
        memcpy(g_gunNow, G, 64);
    }
    for (int w = 0; w < 2; ++w) {  // controllers: rows right, forward, up; the right row turned back
        float t[3];
        MirVec(ctlF[w], nrm, t);
        for (int k = 0; k < 3; ++k) g_ctlF[w][k] = -t[k];
        MirVec(ctlF[w] + 3, nrm, g_ctlF[w] + 3), MirVec(ctlF[w] + 6, nrm, g_ctlF[w] + 6);
        MirPt(ctlP[w], e, nrm, g_ctlP[w]);
    }
    g_coreWrote = false;
    ApplyCore(comp, bones, n);
    memcpy(g_gunNow, gun, 64), memcpy(g_ctlF, ctlF, sizeof ctlF), memcpy(g_ctlP, ctlP, sizeof ctlP);
    if (!g_coreWrote) return;
    // 3. swap each arm bone with its twin, mirrored back into the real world (model space)
    float ip[16], M[16], X[16];
    if (!InvRigid(g_playerNow, ip)) return;
    MirrorMat(e, nrm, M);
    Mul(g_playerNow, M, X), Mul(X, ip, X);
    static float s_old[kMaxBones][16];
    for (int i = 0; i < n; ++i) memcpy(s_old[i], bones + i * kEntry + 0x10, 64);
    for (int i = 0; i < n; ++i) {
        int j = g_twin[i];
        if (j < 0 || *(int *)(bones + i * kEntry + 8) != 2 || *(int *)(bones + j * kEntry + 8) != 2) continue;
        float B[16];
        memcpy(B, s_old[j], 64);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) B[r * 4 + c] *= g_mirC[r];
        Mul(B, X, (float *)(bones + i * kEntry + 0x10));
    }
    for (int i = 14; i <= 16; ++i)  // spine bones the sleeves hang on (shrunk to a point): just moved to the mirrored place
        if (g_chest && *(int *)(bones + i * kEntry + 8) == 2) {
            float *m = (float *)(bones + i * kEntry + 0x10), p[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, m[12], m[13], m[14], 1};
            Mul(p, X, p);
            m[12] = p[12], m[13] = p[13], m[14] = p[14];
        }
    memcpy(g_wrote, bones + g_wroteBone * kEntry + 0x10, 64);  // the anchor as it is now
}
bool *HandsMirrorArmsPtr() { return &g_mirArms; }
}  // namespace

void HandsC4(const float *gameCharge, const float *ourCharge) {
    float g[16], o[16], ig[16];
    memcpy(g, gameCharge, 64), memcpy(o, ourCharge, 64);
    g[3] = g[7] = g[11] = o[3] = o[7] = o[11] = 0, g[15] = o[15] = 1;
    if (!InvRigid(g, ig)) return;
    Mul(ig, o, g_c4Rel);
    memcpy(g_c4Game, g, 64);
    DWORD now = GetTickCount();
    if (now - g_c4Tick > 500) {  // c4learn2: the C4 has just come out
        if (InterlockedExchange(&g_c4Relearn, 0)) g_c4KHave = g_c4Built = false;
        g_c4LearnN = -6, g_c4Bad = 0;
    }
    g_c4Tick = now;
}
// c4rigid1: gunattach, after HandsControllers: where the charge goes this frame (false = hold not learnt yet)
bool HandsC4Charge(float *out) {
    float W[16];
    if (!g_on || !g_left || !g_c4KHave || !CtlFrame(0, W)) return false;
    Mul(g_c4K, W, out);
    bool adj = false;
    for (int k = 0; k < 6; ++k) adj |= g_c4Obj[k] != 0;
    if (adj) {  // c4turn1
        float D[16];
        CtlAdjust(g_c4Obj, out + 12, D);
        Mul(out, D, out);
    }
    {  // c4mirror1
        const float *A = out;
        float cx[3] = {A[5] * A[10] - A[6] * A[9], A[6] * A[8] - A[4] * A[10], A[4] * A[9] - A[5] * A[8]};
        if ((A[0] * cx[0] + A[1] * cx[1] + A[2] * cx[2] < 0) != (bool)g_c4Mirror)
            for (int k = 0; k < 3; ++k) out[8 + k] = -out[8 + k];
    }
    return true;
}
// gripfwd1
float *HandsGunLeft() { return g_gl; }
const char *HandsGunLeftGun() { return g_glGun; }
void HandsGunLeftTouched() { g_glTouched = true; }
void HandsGunLeftReset() { GunLeftDefaults(g_glGun, g_gl), g_glTouched = true; }
void HandsGunLeftIn(float seconds) {
    if (seconds <= 0) InterlockedExchange(&g_glCapOn, 0);
    else g_glCapAt = GetTickCount() + (DWORD)(seconds * 1000), InterlockedExchange(&g_glCapOn, 1);
}
float HandsGunLeftLeft() {
    LONG d = (LONG)(g_glCapAt - GetTickCount());
    return g_glCapOn && d > 0 ? d / 1000.f : 0;
}
bool HandsGunLeftOnGun() { return g_glOnGun; }
const float *HandsGunLeftSeen(int mine) { return mine ? g_glMine : g_glGame; }
void HandsC4Relearn() { InterlockedExchange(&g_c4Relearn, 1); }
void HandsC4Mirror(bool m) { g_c4Mirror = m; }
// capture2: capture slot's fingers in so many seconds (0 = cancel); how long is left (0 = none running for the slot)
void HandsPoseIn(int slot, float seconds) {
    if (seconds <= 0) InterlockedExchange(&g_poseCapSlot, -1);
    else g_poseCapAt = GetTickCount() + (DWORD)(seconds * 1000), InterlockedExchange(&g_poseCapSlot, slot);
}
float HandsPoseLeft(int slot) {
    LONG d = (LONG)(g_poseCapAt - GetTickCount());
    return g_poseCapSlot == slot && d > 0 ? d / 1000.f : 0;
}
float HandsBuzz() {  // both hands: a tick each second of the countdown, a long one at the capture
    DWORD t = GetTickCount();
    if ((LONG)(g_capBuzzUntil - t) > 0 || (LONG)(g_glBuzzUntil - t) > 0) return 0.8f;
    LONG dg = (LONG)(g_glCapAt - t);  // gripfwd1: the left hand capture's countdown
    if (g_glCapOn && dg > 0 && dg % 1000 > 920) return 0.4f;
    LONG d = (LONG)(g_poseCapAt - t);
    return g_poseCapSlot >= 0 && d > 0 && d % 1000 > 920 ? 0.4f : 0;
}
bool HandsC4RelearnDue() { return g_c4Relearn != 0; }
// zip1
bool *HandsZipOn() { return &g_zipOn; }
bool *HandsZipPreview() { return &g_zipPreview; }
bool *HandsZipHookGame() { return &g_zipHook; }
float *HandsZip() { return g_zip; }
bool HandsZipNow() { return g_on && ZipNow(); }
bool HandsC4Learnt() { return g_c4KHave; }

static DWORD WINAPI SaveSoonThread(void *) {
    HandsSave();
    return 0;
}
void HandsControllers(const float *FL, const float *pL, const float *FR, const float *pR, float scale, bool twoHand) {
    if (InterlockedExchange(&g_saveSoon, 0)) QueueUserWorkItem(SaveSoonThread, nullptr, 0);
    if (FL) memcpy(g_ctlF[0], FL, 36), memcpy(g_ctlP[0], pL, 12);
    if (FR) memcpy(g_ctlF[1], FR, 36), memcpy(g_ctlP[1], pR, 12);
    g_ctlOk[0] = FL != nullptr, g_ctlOk[1] = FR != nullptr;
    g_ctlScale = scale, g_twoHand = twoHand;
}
float *HandsSmooth() { return &g_smooth; }
float *HandsBlend() { return &g_blend; }
// hands13 (DrGonzaga 13 Nov: "both elbows seem skewed to the right", sliders did little): the axes given
// here are the play space's, so shoulders and elbows were laid out along the room, not along where he
// faces. The body frame is now the head's facing, levelled: up as given, forward = the view's forward
// flattened, right from those two (the same handedness as the axes given). Eased, so a glance does
// not swing the elbows at once.
void HandsHead(const float *eye, const float *axes, bool scoped, const float *cam, const float *fwd, const float *head) {
    memcpy(g_eye, eye, 12), memcpy(g_camRef, cam, 12);
    {   // walkhands2: the followed point's offset from the body (see g_qL)
        const float *Ra = axes, *Ua = axes + 3, *Fa = axes + 6;
        LONGLONG qn = Qpc();
        float dt = g_qT && g_qpf > 0 ? (float)((qn - g_qT) / g_qpf) : 0;
        g_qT = qn;
        if (!head) g_qL[0] = g_qL[2] = 0;
        else {
            float d[3] = {head[0] - cam[0], head[1] - cam[1], head[2] - cam[2]};
            float h[3] = {d[0] * Ra[0] + d[1] * Ra[1] + d[2] * Ra[2], 0, d[0] * Fa[0] + d[1] * Fa[1] + d[2] * Fa[2]};  // level only
            if (EngineUserStickRecent() && dt > 0) {
                float dx = h[0] - g_qL[0], dz = h[2] - g_qL[2];
                float k = dt > 0.25f || dx * dx + dz * dz > 2.0f ? 1 : 1 - expf(-dt / 0.3f);
                g_qL[0] += dx * k, g_qL[2] += dz * k;
            } else {
                // stick let go: never further out than the head now, so as roomscale brings the body back under the head
                // the offset goes with it and ends about 0 (turning then feels as before too)
                float hl = sqrtf(h[0] * h[0] + h[2] * h[2]), ql = sqrtf(g_qL[0] * g_qL[0] + g_qL[2] * g_qL[2]);
                if (ql > hl && ql > 1e-4f) g_qL[0] *= hl / ql, g_qL[2] *= hl / ql;
            }
        }
        for (int k = 0; k < 3; ++k) g_qW[k] = Ra[k] * g_qL[0] + Ua[k] * g_qL[1] + Fa[k] * g_qL[2];
    }
    const float *R0 = axes, *U = axes + 3, *F0 = axes + 6;
    float f[3], d = fwd ? fwd[0] * U[0] + fwd[1] * U[1] + fwd[2] * U[2] : 0, l = 0;
    for (int k = 0; k < 3; ++k) f[k] = fwd ? fwd[k] - d * U[k] : F0[k], l += f[k] * f[k];
    l = sqrtf(l);
    if (l < 0.25f) {  // looking straight up or down: keep the last facing
        if (!g_headOk) memcpy(g_axes, axes, 36);
    } else {
        for (int k = 0; k < 3; ++k) f[k] /= l;
        if (g_headOk) {
            float *pf = g_axes + 6, n2 = 0;
            for (int k = 0; k < 3; ++k) f[k] = pf[k] + (f[k] - pf[k]) * 0.06f, n2 += f[k] * f[k];
            n2 = sqrtf(n2);
            if (n2 > 1e-3f)
                for (int k = 0; k < 3; ++k) f[k] /= n2;
        }
        float c[3] = {f[1] * U[2] - f[2] * U[1], f[2] * U[0] - f[0] * U[2], f[0] * U[1] - f[1] * U[0]};
        float c0[3] = {F0[1] * U[2] - F0[2] * U[1], F0[2] * U[0] - F0[0] * U[2], F0[0] * U[1] - F0[1] * U[0]};
        float sg = c0[0] * R0[0] + c0[1] * R0[1] + c0[2] * R0[2] < 0 ? -1.f : 1.f;
        for (int k = 0; k < 3; ++k) g_axes[k] = c[k] * sg, g_axes[3 + k] = U[k], g_axes[6 + k] = f[k];
    }
    g_headOk = true, g_scoped = scoped;
}
float *HandsWristBend() { return &g_wristBend; }
// hands24
void HandsLeftGrip(bool held) { g_lgrip = held; }
float *HandsC4Adj() { return g_c4Adj; }
float *HandsC4Obj() { return g_c4Obj; }
bool *HandsPistolGrip() { return &g_pistolGrip; }
float *HandsPistolReach() { return &g_pistolReach; }
bool HandsPistolLocked() { return g_pistolLocked; }
int HandsPistolPlace(int op) {  // 0 = is one saved, 1 = save where the hand is now, 2 = forget it
    if (op == 1) InterlockedExchange(&g_pgSaveReq, 1);
    if (op == 2) g_pgHave = false, InterlockedExchange(&g_dirty, 1);
    return g_pgHave;
}
int HandsPose(int slot, int op) {  // 0 = is one captured, 1 = capture now, 2 = forget it
    if (slot < 0 || slot > 3) return 0;
    if (op == 1) InterlockedExchange(&g_poseCapReq, slot);
    if (op == 2) g_pose[slot].have = false, g_pose[slot].changed = true, InterlockedExchange(&g_dirty, 1);
    if (op == 2 && slot == 2) {  // neutral2: back to the built-in one
        for (int i = 0; i < kFingN; ++i) {
            const float *v = kNeutral[i];
            float o[16] = {v[0], v[1], v[2], 0, v[3], v[4], v[5], 0, v[6], v[7], v[8], 0, v[9], v[10], v[11], 1};
            memcpy(g_pose[2].m[i], o, 64);
        }
        g_pose[2].have = true;
    }
    if (op && slot == 0) InterlockedExchange(&g_c4Relearn, 1);  // c4rigid1: the hold is measured against the fingers
    return g_pose[slot].have;
}
bool *HandsPoseUse(int slot) { return &g_pose[slot < 0 || slot > 3 ? 0 : slot].use; }
bool *HandsPistolAuto() { return &g_pistolAuto; }
bool *HandsNeutral() { return &g_neutral; }
bool *HandsGameSigns() { return &g_signShow; }    // sign1
bool *HandsGameArmsEmpty() { return &g_emptyArms; }  // emptyarms1
bool *HandsEmptyTracked() { return &g_emptyTracked; }  // emptyhands1
bool *HandsEmptyAnywhere() { return &g_emptyAnywhere; }  // emptyhands5
bool *HandsEmptyGameAnims() { return &g_emptyGuard; }
float *HandsEmptyGameAnimCm() { return &g_emptyAway; }
bool *HandsKillstarGrip() { return &g_starMirror; }  // killstar1
// emptyarms1 (DrGonzaga 16 Nov: "when arms are on fire, the animation to 'pat the fire out' is gone"): with VR hands on,
// the ticked arm parts were drawn only while the arms were on a gun, so anything the game does with no weapon in the
// hand (patting out flames, ladders, swimming, takedowns) was invisible. Now with no weapon seen in the hand for
// 0.4 s, on foot, the arms are drawn where the game puts them. body.cpp asks this for every arm draw.
bool HandsGameArmsNow() {
    bool now = g_on && g_emptyArms && !HandsLive() && GetTickCount() - GunAttachHandTick() > 400 && EngineOnFoot() && !EngineInVehicle() &&
               !EngineCutscene() && !CyberEyeOn() && !GamePaused();
    static bool s_was;
    static int s_n;
    static DWORD s_t;
    if (now != s_was) {
        s_was = now;
        DWORD t = GetTickCount();
        if (now) s_t = t;
        else if (s_n < 40 && t - s_t > 300) ++s_n, Log("hands: the game's own arms were shown for %.1f s (no weapon in the hand)", (t - s_t) * 0.001f);
    }
    return now;
}
// emptyhands1: gunattach.cpp, game thread, a frame with no weapon in the player's hand (on foot, normal play).
// emptyhands4 (DrGonzaga 17 Nov, bd98: the hands stopped at "an early tutorial section that asks you to use your
// cyber-eye ... once a weapon is in hand, we can switch to normal"): the opening section's mode (e2e5f03d) leaves the
// bottom of the game's list just before the cyber-eye tutorial. So it now only starts the section, which then lasts
// until the first real gun (a DLC_BD_ name) comes into the hand; the knife of a takedown does not end it.
void HandsGunSeen(const char *name) {
    if (g_openSect && name && !strncmp(name, "DLC_BD_", 7))
        g_openSect = false, Log("hands: first gun in the hand (%s): the opening section's tracked empty hands end here", name);
}
bool HandsEmptyWanted() {
    if (!g_on || !g_emptyTracked || !g_bloodDragon) return false;
    DWORD im = EngineInputMode();  // plain play only: not ladders, takedowns, scenes
    // emptyhands2 (DrGonzaga 17 Nov: "the no hands pose is only at the start ... make it for that section only"): only
    // in the opening section; elsewhere the game's own arms.
    if (!g_openSect && EngineInputBase() == 0xe2e5f03d)
        g_openSect = true, Log("hands: the opening on-foot section: empty hands follow the controllers until the first gun");
    if (!g_openSect && !g_emptyAnywhere) return false;  // emptyhands5: anywhere, unless switched back to the opening only
    // emptyhand6 (DrGonzaga 24 Nov, bd146 log 12:37: "I couldn't control my hands - can I control my hands for that section"):
    // c5455f40 is the story mode with the weapons put away (walking about, no gun to take); hands tracked there too.
    return (im == 0xe35e00df || im == 0xe2e5f03d || im == 0xc5455f40) && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() && !GamePaused();
}
void HandsFrameEmpty(BYTE *attComp, BYTE *playerEnt) {
    if (IsBadReadPtr(playerEnt + 0x20, 64)) return;
    HandsFrame(attComp, playerEnt, 51, (const float *)(playerEnt + 0x20));
    g_emptyFrame = true;
}
bool HandsTakeDirty() { return InterlockedExchange(&g_dirty, 0) != 0; }
bool *HandsScopeHide() { return &g_scopeHide; }
bool *HandsElbow() { return &g_elbow; }
float *HandsFree() { return g_free; }
float *HandsFreeBow() { return g_freeBow; }
bool *HandsFreeLeft() { return &g_freeLeft; }
bool *HandsChest() { return &g_chest; }

// (bd30 flashed: the stamp was "tick | 1", a millisecond in the future half the time, which an
// unsigned "now - stamp" read as 49 days.)
// gunattach.cpp, game thread, before the game's attachment update reads the player's bones: if they
// still hold our pose (the animation has not written since), give the game's own back.
void HandsRestore() {
    BYTE *bones = g_bones;
    int n = g_n, gb = g_wroteBone;
    if (!g_saved || !bones || bones != g_savedBones || n != g_savedN || gb < 0 || gb >= n) return;
    if (IsBadWritePtr(bones, n * kEntry)) return;
    if (memcmp(bones + gb * kEntry + 0x10, g_wrote, 64)) return;  // the animation wrote: nothing of ours left
    for (int i = 0; i < n; ++i)
        if (*(int *)(bones + i * kEntry + 8) == 2) memcpy(bones + i * kEntry + 0x10, g_saved[i], 64);
    InterlockedIncrement(&g_restored);
}

float *HandsBowElbow() { return g_bowEl; }  // 3: high, side, back
float *HandsElbowOut() { return &g_elbowOut; }
bool *HandsElbowGun() { return &g_elbowGun; }

bool HandsLive() { return g_on && g_liveTick && (LONG)(GetTickCount() - g_liveTick) < 120; }
bool *HandsOn() { return &g_on; }
bool *HandsLeft() { return &g_left; }
void HandsSave() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    WritePrivateProfileStringW(L"hands", L"On", g_on ? L"1" : L"0", path);
    GunLeftSave(path);  // gripfwd1
    WritePrivateProfileStringW(L"hands", L"LeftHand", g_left ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"hands", L"FreeLeft", g_freeLeft ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"hands", L"HideShoulders", g_chest ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"hands", L"HideLeftScoped", g_scopeHide ? L"1" : L"0", path);
    {
        wchar_t b[32];
        swprintf(b, 32, L"%.3f", g_smooth);
        WritePrivateProfileStringW(L"hands", L"Smoothing", b, path);
        swprintf(b, 32, L"%.3f", g_blend);
        WritePrivateProfileStringW(L"hands", L"Blend", b, path);
    }
    WritePrivateProfileStringW(L"hands", L"FreeElbow", g_elbow ? L"1" : L"0", path);
    {
        wchar_t b[32];
        swprintf(b, 32, L"%.1f", g_wristBend);
        WritePrivateProfileStringW(L"hands", L"WristBend", b, path);
    }
    {
        wchar_t b[32];
        static const wchar_t *const kE[3] = {L"BowAxisHigh", L"BowAxisSide", L"BowAxisBack"};
        for (int i = 0; i < 3; ++i) swprintf(b, 32, L"%.3f", g_bowEl[i]), WritePrivateProfileStringW(L"hands", kE[i], b, path);
        swprintf(b, 32, L"%.3f", g_elbowOut);
        WritePrivateProfileStringW(L"hands", L"ElbowOut", b, path);
        WritePrivateProfileStringW(L"hands", L"GripElbow", g_elbowGun ? L"1" : L"0", path);
    }
    {  // hands24
        static const wchar_t *const kC[6] = {L"C4Right", L"C4Up", L"C4Forward", L"C4Pitch", L"C4Yaw", L"C4Roll"};
        wchar_t b[320];
        for (int i = 0; i < 6; ++i) swprintf(b, 32, L"%.3f", g_c4Adj[i]), WritePrivateProfileStringW(L"hands", kC[i], b, path);
        WritePrivateProfileStringW(L"hands", L"PistolGrip", g_pistolGrip ? L"1" : L"0", path);
        WritePrivateProfileStringW(L"hands", L"PistolAutoJoin", g_pistolAuto ? L"1" : L"0", path);
        WritePrivateProfileStringW(L"hands", L"NeutralLeft", g_neutral ? L"1" : L"0", path);
        WritePrivateProfileStringW(L"hands", L"GameFingerSigns", g_signShow ? L"1" : L"0", path);  // sign1
        WritePrivateProfileStringW(L"hands", L"GameArmsEmptyHanded", g_emptyArms ? L"1" : L"0", path);  // emptyarms1
        WritePrivateProfileStringW(L"hands", L"EmptyHandsTracked", g_emptyTracked ? L"1" : L"0", path);  // emptyhands1
        WritePrivateProfileStringW(L"hands", L"EmptyHandsAnywhere", g_emptyAnywhere ? L"1" : L"0", path);  // emptyhands5
        WritePrivateProfileStringW(L"hands", L"EmptyHandsGameAnims", g_emptyGuard ? L"1" : L"0", path);
        swprintf(b, 32, L"%.1f", g_emptyAway * 100), WritePrivateProfileStringW(L"hands", L"EmptyHandsGameAnimCm", b, path);
        WritePrivateProfileStringW(L"hands", L"KillstarGrip", g_starMirror ? L"1" : L"0", path);  // killstar1
        swprintf(b, 32, L"%.3f", g_pistolReach), WritePrivateProfileStringW(L"hands", L"PistolGripReach", b, path);
        auto Twelve = [&](const float *m) {
            swprintf(b, 320, L"%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
        };
        static const wchar_t *const kO[6] = {L"C4ObjRight", L"C4ObjUp", L"C4ObjForward", L"C4ObjPitch", L"C4ObjYaw", L"C4ObjRoll"};
        for (int i = 0; i < 6; ++i) swprintf(b, 32, L"%.3f", g_c4Obj[i]), WritePrivateProfileStringW(L"hands", kO[i], b, path);
        {  // zip1
            static const wchar_t *const kZ[6] = {L"ZipRight", L"ZipUp", L"ZipForward", L"ZipPitch", L"ZipYaw", L"ZipRoll"};
            for (int i = 0; i < 6; ++i) swprintf(b, 32, L"%.3f", g_zip[i]), WritePrivateProfileStringW(L"hands", kZ[i], b, path);
            WritePrivateProfileStringW(L"hands", L"ZipHand", g_zipOn ? L"1" : L"0", path);
            WritePrivateProfileStringW(L"hands", L"ZipHookGame", g_zipHook ? L"1" : L"0", path);
        }
        if (g_c4KHave && !g_c4Built) Twelve(g_c4K), WritePrivateProfileStringW(L"hands", L"C4Hold", b, path);
        else WritePrivateProfileStringW(L"hands", L"C4Hold", nullptr, path);
        if (g_pgHave) Twelve(g_pgRel), WritePrivateProfileStringW(L"hands", L"PistolGripPlace", b, path);
        else WritePrivateProfileStringW(L"hands", L"PistolGripPlace", nullptr, path);
        for (int sl = 0; sl < 4; ++sl) {
            Pose &ps = g_pose[sl];
            WritePrivateProfileStringW(kPoseSec[sl], L"Use", ps.use ? L"1" : L"0", path);
            if (!ps.changed) continue;
            ps.changed = false;
            for (int i = 0; i < kFingN; ++i) {
                wchar_t key[8];
                swprintf(key, 8, L"B%d", kFing0 + i);
                if (ps.have) Twelve(ps.m[i]);
                WritePrivateProfileStringW(kPoseSec[sl], key, ps.have ? b : nullptr, path);
            }
        }
    }
    static const wchar_t *const kF[6] = {L"FreeRight", L"FreeUp", L"FreeForward", L"FreePitch", L"FreeYaw", L"FreeRoll"};
    for (int i = 0; i < 6; ++i) {
        wchar_t b[32];
        swprintf(b, 32, L"%.3f", g_free[i]);
        WritePrivateProfileStringW(L"hands", kF[i], b, path);
        static const wchar_t *const kA[6] = {L"ArrowRight", L"ArrowUp", L"ArrowForward", L"ArrowPitch", L"ArrowYaw", L"ArrowRoll"};
        swprintf(b, 32, L"%.3f", g_freeBow[i]);
        WritePrivateProfileStringW(L"hands", kA[i], b, path);
    }
    Log("hands: VR hands %s, left hand %s", g_on ? "on" : "off", g_left ? "shown" : "hidden");
}

// From the skin stub (bowstring.cpp): the player's model is about to be resolved for drawing.
void HandsSkin(BYTE *comp) {
    if (!comp || comp != g_handsComp) return;
    BYTE *b = *(BYTE **)(comp + 0x80);
    int c = *(int *)(comp + 0x84);
    if (!b || b != g_bones || c != g_n) return;
    Apply(comp, b, c);
    static DWORD s_log;
    DWORD t = GetTickCount();
    if (t - s_log < 10000) return;
    s_log = t;
    LONG a = InterlockedExchange(&g_applied, 0), k = InterlockedExchange(&g_skippedTwice, 0), nr = InterlockedExchange(&g_notRigid, 0);
    LONG bw = InterlockedExchange(&g_bowWait, 0);
    if (g_verboseLog)  // tidy1 (bd79): detailed log only
        Log("hands: 10 s: arms moved %ld times (left hand free %ld, right hand free %ld), left alone (same pose as last frame) %ld, game pose put back %ld times, bow without hands (arrow rest not learnt yet) %ld frames, odd matrix %ld; gun bone %ld, both grips %d, controllers L%d R%d",
            a, InterlockedExchange(&g_freeL, 0), InterlockedExchange(&g_freeR, 0), k, InterlockedExchange(&g_restored, 0), bw, nr, g_frameBone, (int)g_twoHand, (int)g_ctlOk[0], (int)g_ctlOk[1]);
}

// gunattach.cpp, game thread, each frame the gun is placed: the player's attachment component and
// object, the gun's bone and the gun's pose as we placed it. Also finds the player's bone table.
void HandsFrame(BYTE *attComp, BYTE *playerEnt, int gunBone, const float *ourGun) {
    if (!g_bloodDragon || IsBadReadPtr(playerEnt + 0x20, 64)) return;
    memcpy(g_gunNow, ourGun, 64), memcpy(g_playerNow, playerEnt + 0x20, 64);
    g_emptyFrame = false;
    g_gunNow[3] = g_gunNow[7] = g_gunNow[11] = 0, g_gunNow[15] = 1;
    g_frameBone = gunBone, g_frameTick = GetTickCount();
    g_c4 = GunAttachC4Split(), g_frameOk = true;

    static BYTE *s_att, *s_ent;
    static DWORD s_chk;
    DWORD tick = GetTickCount();
    bool fresh = attComp != s_att || playerEnt != s_ent;
    if (!fresh && tick - s_chk <= 1000) return;
    s_chk = tick, s_att = attComp, s_ent = playerEnt;
    // The bone container the attachment update reads (comp+0xc0) sits at graphic component +0x60.
    BYTE *skel = IsBadReadPtr(attComp + 0xc0, 4) ? nullptr : *(BYTE **)(attComp + 0xc0);
    BYTE *comp = nullptr, *bones = nullptr;
    int n = 0;
    if (skel && !IsBadReadPtr(skel - 0x60, 0x230)) {
        BYTE *c = skel - 0x60, *c4 = *(BYTE **)(c + 4);
        if (c4 && !IsBadReadPtr(c4 + 0xc, 4) && *(BYTE **)(c4 + 0xc) == playerEnt) comp = c;
        bones = *(BYTE **)(skel + 0x20), n = *(int *)(skel + 0x24);
    }
    if (!comp) {  // else the object's graphic component by the game's own lookup, if it owns that table
        BYTE *via = BowStringCompBd(playerEnt);
        if (via && !IsBadReadPtr(via, 0x230) && *(BYTE **)(via + 0x80) == bones) comp = via;
    }
    if (comp && bones && n >= 2 && n <= kMaxBones && !IsBadReadPtr(bones, n * kEntry)) {
        g_bones = bones, g_n = n;
        g_handsComp = comp;
    } else {
        g_handsComp = nullptr;
        if (fresh) Log("hands: no usable bone table for the player (model %p, bones %p, count %d)", comp, bones, n);
    }
}

bool *HandsMirrorArms() { return HandsMirrorArmsPtr(); }
void HandsInstallBd() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    g_on = GetPrivateProfileIntW(L"hands", L"On", 1, path) != 0;
    g_left = GetPrivateProfileIntW(L"hands", L"LeftHand", 1, path) != 0;
    g_mirArms = GetPrivateProfileIntW(L"vr", L"LeftHandedMirrorArms", 1, path) != 0;  // lefty2
    g_freeLeft = GetPrivateProfileIntW(L"hands", L"FreeLeft", 1, path) != 0;
    g_chest = GetPrivateProfileIntW(L"hands", L"HideShoulders", 1, path) != 0;
    g_scopeHide = GetPrivateProfileIntW(L"hands", L"HideLeftScoped", 1, path) != 0;
    {
        wchar_t b[32];
        if (GetPrivateProfileStringW(L"hands", L"Smoothing", L"", b, 32, path)) g_smooth = (float)_wtof(b);
        if (GetPrivateProfileStringW(L"hands", L"Blend", L"", b, 32, path)) g_blend = (float)_wtof(b);
        g_smooth = g_smooth < 0 ? 0 : g_smooth > 0.3f ? 0.3f : g_smooth;
        g_blend = g_blend < 0 ? 0 : g_blend > 1 ? 1 : g_blend;
    }
    g_elbow = GetPrivateProfileIntW(L"hands", L"FreeElbow", 1, path) != 0;
    {
        wchar_t b[32];
        if (GetPrivateProfileStringW(L"hands", L"WristBend", L"", b, 32, path)) g_wristBend = (float)_wtof(b);
    }
    {
        wchar_t b[32];
        static const wchar_t *const kE[3] = {L"BowAxisHigh", L"BowAxisSide", L"BowAxisBack"};
        for (int i = 0; i < 3; ++i)
            if (GetPrivateProfileStringW(L"hands", kE[i], L"", b, 32, path)) g_bowEl[i] = (float)_wtof(b);
        if (GetPrivateProfileStringW(L"hands", L"ElbowOut", L"", b, 32, path)) g_elbowOut = (float)_wtof(b);
        g_elbowGun = GetPrivateProfileIntW(L"hands", L"GripElbow", 1, path) != 0;
    }
    static const wchar_t *const kF[6] = {L"FreeRight", L"FreeUp", L"FreeForward", L"FreePitch", L"FreeYaw", L"FreeRoll"};
    for (int i = 0; i < 6; ++i) {
        wchar_t b[32];
        if (GetPrivateProfileStringW(L"hands", kF[i], L"", b, 32, path)) g_free[i] = (float)_wtof(b);
        static const wchar_t *const kA[6] = {L"ArrowRight", L"ArrowUp", L"ArrowForward", L"ArrowPitch", L"ArrowYaw", L"ArrowRoll"};
        if (GetPrivateProfileStringW(L"hands", kA[i], L"", b, 32, path)) g_freeBow[i] = (float)_wtof(b);
        else g_freeBow[i] = g_free[i];  // first time: as the free hand was
    }
    {  // hands24
        static const wchar_t *const kC[6] = {L"C4Right", L"C4Up", L"C4Forward", L"C4Pitch", L"C4Yaw", L"C4Roll"};
        wchar_t b[320];
        for (int i = 0; i < 6; ++i)
            if (GetPrivateProfileStringW(L"hands", kC[i], L"", b, 32, path)) g_c4Adj[i] = (float)_wtof(b);
        g_pistolGrip = GetPrivateProfileIntW(L"hands", L"PistolGrip", 1, path) != 0;
        g_pistolAuto = GetPrivateProfileIntW(L"hands", L"PistolAutoJoin", 1, path) != 0;
        g_neutral = GetPrivateProfileIntW(L"hands", L"NeutralLeft", 1, path) != 0;
        g_signShow = GetPrivateProfileIntW(L"hands", L"GameFingerSigns", 1, path) != 0;  // sign1
        g_emptyArms = GetPrivateProfileIntW(L"hands", L"GameArmsEmptyHanded", 1, path) != 0;  // emptyarms1
        g_emptyTracked = GetPrivateProfileIntW(L"hands", L"EmptyHandsTracked", 1, path) != 0;  // emptyhands1
        g_emptyAnywhere = GetPrivateProfileIntW(L"hands", L"EmptyHandsAnywhere", 1, path) != 0;  // emptyhands5
        g_emptyGuard = GetPrivateProfileIntW(L"hands", L"EmptyHandsGameAnims", 1, path) != 0;
        if (GetPrivateProfileStringW(L"hands", L"EmptyHandsGameAnimCm", L"", b, 32, path)) g_emptyAway = fminf(fmaxf((float)_wtof(b), 3.f), 60.f) * 0.01f;
        g_starMirror = GetPrivateProfileIntW(L"hands", L"KillstarGrip", 1, path) != 0;  // killstar1
        if (GetPrivateProfileStringW(L"hands", L"PistolGripReach", L"", b, 32, path)) g_pistolReach = (float)_wtof(b);
        auto Twelve = [&](float *m) {
            float v[12];
            if (swscanf(b, L"%f %f %f %f %f %f %f %f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7, v + 8, v + 9, v + 10, v + 11) != 12) return false;
            float o[16] = {v[0], v[1], v[2], 0, v[3], v[4], v[5], 0, v[6], v[7], v[8], 0, v[9], v[10], v[11], 1};
            memcpy(m, o, 64);
            return true;
        };
        g_pgHave = GetPrivateProfileStringW(L"hands", L"PistolGripPlace", L"", b, 320, path) && Twelve(g_pgRel);
        g_c4KHave = GetPrivateProfileStringW(L"hands", L"C4Hold", L"", b, 320, path) && Twelve(g_c4K);
        static const wchar_t *const kO[6] = {L"C4ObjRight", L"C4ObjUp", L"C4ObjForward", L"C4ObjPitch", L"C4ObjYaw", L"C4ObjRoll"};
        for (int i = 0; i < 6; ++i)
            if (GetPrivateProfileStringW(L"hands", kO[i], L"", b, 32, path)) g_c4Obj[i] = (float)_wtof(b);
        if (g_c4KHave && sqrtf(g_c4K[12] * g_c4K[12] + g_c4K[13] * g_c4K[13] + g_c4K[14] * g_c4K[14]) > 0.25f) {  // c4learn2
            Log("hands: the saved C4 hold was %.0f %.0f %.0f cm from the hand (a bad learn in bd62): dropped with the charge sliders; it is learnt again when the C4 next comes out",
                g_c4K[12] * 100, g_c4K[13] * 100, g_c4K[14] * 100);
            g_c4KHave = false, memset(g_c4Obj, 0, sizeof g_c4Obj);
            InterlockedExchange(&g_dirty, 1), InterlockedExchange(&g_saveSoon, 1);
        }
        if (!g_c4KHave) {
            // c4hold3 (bd64 log: left alone, the game's charge is 25 to 29 cm from its own hand's frame while the C4
            // comes out, so there is no clean hold to learn): the built-in place from the start, fitted by the sliders.
            static const float kHold[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.052f, 0.069f, 0.017f, 1};
            memcpy(g_c4K, kHold, 64), g_c4KHave = true, g_c4Built = false;
        }
        {  // zip1
            static const wchar_t *const kZ[6] = {L"ZipRight", L"ZipUp", L"ZipForward", L"ZipPitch", L"ZipYaw", L"ZipRoll"};
            for (int i = 0; i < 6; ++i)
                if (GetPrivateProfileStringW(L"hands", kZ[i], L"", b, 32, path)) g_zip[i] = (float)_wtof(b);
            g_zipOn = GetPrivateProfileIntW(L"hands", L"ZipHand", 1, path) != 0;
            g_zipHook = GetPrivateProfileIntW(L"hands", L"ZipHookGame", 1, path) != 0;
        }
        for (int sl = 0; sl < 4; ++sl) {
            Pose &ps = g_pose[sl];
            ps.use = GetPrivateProfileIntW(kPoseSec[sl], L"Use", 1, path) != 0;
            ps.have = true;
            for (int i = 0; i < kFingN && ps.have; ++i) {
                wchar_t key[8];
                swprintf(key, 8, L"B%d", kFing0 + i);
                ps.have = GetPrivateProfileStringW(kPoseSec[sl], key, L"", b, 320, path) && Twelve(ps.m[i]);
            }
            if (sl == 2 && !ps.have) {  // neutral2: the built-in neutral hand
                for (int i = 0; i < kFingN; ++i) {
                    const float *v = kNeutral[i];
                    float o[16] = {v[0], v[1], v[2], 0, v[3], v[4], v[5], 0, v[6], v[7], v[8], 0, v[9], v[10], v[11], 1};
                    memcpy(ps.m[i], o, 64);
                }
                ps.have = true;
            }
        }
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    Log("hands: VR hands %s ([hands] On), left hand %s", g_on ? "on" : "off", g_left ? "shown" : "hidden");
}
