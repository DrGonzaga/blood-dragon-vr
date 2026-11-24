// gunattach1/2: move the first-person gun by moving what it hangs from.
// gunhunt2 (30 Oct): the gun's parts are separate game objects; each frame
// FC3+116d0560 (an object's "update my attached children" step) copies the
// parent object's world matrix (object+0x20, 4x4 rows, position +0x50) into
// each child's link, multiplies in the child's bone, and sets the child's
// world matrix. The gun's parent sits exactly at the game camera (the
// first-person arms). So: when the parent is at the camera, shift its position
// for the length of that call only, and every attached part (gun, sights,
// magazine) follows; the parent itself, and the game, are left as they were.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc3vr.h"

static void HiResLogTick();  // scopegun2

namespace {
volatile DWORD g_handTick;  // last time a weapon was seen in the player's hand (car9)

const DWORD kImageBase = 0x10000000;

typedef DWORD(__attribute__((thiscall)) * AttachFn)(BYTE *comp, DWORD a, DWORD b);
AttachFn g_orig;

volatile bool g_on, g_hand = true;
// gunattach4 settings (Debug page, saved to fc3vr.ini [gun]): the gun's
// offset from the hand and its angle there, both in the hand's own axes, and
// how far to move the camera (and gun) away from the body.
float g_off[3];  // right, up, forward (m): gun origin (the hand bone) from the controller
float g_ang[3];                            // pitch, yaw, roll (degrees)
bool g_roll = true;  // roll1: gun's attachments follow our pose
// c4hands1 (DrGonzaga 12 Nov): with C4 out the game holds the charge in the left hand (bone 20)
// and the detonator in the right (bone 51); both followed the right controller. Now the charge
// follows the left controller (offset from it, right / up / forward), the detonator the right,
// and the aim (game look, so the throw) comes from the left hand. [gun] C4BothHands.
bool g_c4Split = true;
float g_c4Off[3] = {-0.05f, -0.09f, -0.09f};
float g_cam[3];                            // camera right, up, forward (m)
BYTE *volatile g_gunEnt;  // roll1: the gun we place, and the pose we gave it
float g_gunPose[16];
float g_gunDrawn[16];
// kick2: the pose as drawn (with the hip recoil), for the gun's own attachments
volatile LONG g_gunKids, g_gunKidsFixed;
float g_restDev, g_restDevMax;  // melee1
volatile float g_gameGunDev;  // chargeglass2
volatile DWORD g_gameGunDevT;
volatile bool g_singleShot;  // chargeglass1/3: the scoped rifle (DLC_BD_VIPER) without its RELOADKIT is bolt action
volatile LONG g_moved, g_placed, g_shifted, g_fresh, g_noCam, g_noGun, g_noHand, g_rotKept, g_rotRewritten;
volatile bool g_relearn;
float g_camGap;  // largest camera object vs rendered camera distance (log)
BYTE *g_seen[8];
int g_nseen;

float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// gunid2: the gun's turn as learnt from the game differs a little each
// session (it is averaged from an animated pose), and each gun model sits
// differently. The weapon's name ("weapons:FC3/AK47" at *(ent+0x10)+0xc,
// found by gunid1) keys a saved turn in fc3vr.ini [gunturn] and its own
// position and angle settings in [gun.<name>].
char g_name[48];     // current gun ("AK47"), empty before the first gun
char g_turned[48];   // the gun whose settings are loaded
void TypeIni(bool save, const char *name);
bool g_loadedLefty;  // lefty3: which hand's values g_off / g_ang hold
char g_optic[48] = "iron";  // sight2: the fitted optic
void SightIni(bool save);       // sight2
void ScanOptic(BYTE *comp);

// belt1: the game's first-person props (names "FC3_Props:FPP/Syringe", "FC3_Props:FirstPerson/Meat", ...).
// nolaser1 (DrGonzaga 16 Nov: red laser while injecting and while holding the cyber heart): Blood Dragon's props are
// "Blood_Props:FirstPerson/Syringe_Liquid", "Blood_Props:FirstPerson/Cyborg_BloodPump", so any "..._Props:" counts;
// the cyber heart (gun "Lurer") never has a laser either. Saved per-gun beam ticks are overruled for these.
bool IsProp(const char *name) {
    return strstr(name, "_Props:") || !strcmp(name, "Lurer") || !strcmp(name, "gadgets:FC3/Lurer");
}

void GunName(BYTE *ent, char *out) {
    strcpy(out, "unknown");
    if (IsBadReadPtr(ent + 0x10, 4)) return;
    const char *p = *(const char **)(ent + 0x10);
    if ((DWORD)p < 0x10000 || IsBadReadPtr(p + 0xc, 48)) return;
    p += 0xc;
    if (!strncmp(p, "weapons:", 8)) p += 8;
    if (!strncmp(p, "FC3/", 4)) p += 4;
    int n = 0;
    for (; n < 40 && p[n] > 0x20 && p[n] < 0x7f && p[n] != '=' && p[n] != ']' && p[n] != ';'; ++n) out[n] = p[n];
    if (n) out[n] = 0;
    // gunname1 (DrGonzaga 17 Nov: guns "off in really unusual directions" on a new start, fine on a later save): a new
    // game's guns are named with their mod variant ("DLC_BD_AUTO9.DLC_BD_AUTO9_M010"); those names had no settings
    // of their own (a new one copied whatever gun was out before, e.g. the Viper took the Terminus's -73 deg). The
    // variant now shares the base gun's settings.
    if (g_bloodDragon && n)
        if (char *dot = strchr(out, '.')) *dot = 0;
}

// bow1 (1 Nov): with the bow out, the arrow is a second weapon object on a
// hand bone. It was taken for the gun (so the hand moved the arrow and the
// bow stayed at the game's pose). Arrows are never the gun now; they follow
// the bow instead. Names are cached per object (a few objects at a time).
// qte5 (FC3, DrGonzaga 12 Nov: a gun taken off an enemy stayed at the head; the player had 18 attached
// objects and every search here stopped at 16). 48 is only a guard against a bad count.
const int kMaxKids = 48;
bool IsArrow(BYTE *ent) {
    static BYTE *s_ent[8];
    static DWORD s_data[8];  // the object's data pointer too: freed objects' addresses get reused
    static bool s_arrow[8];
    static int s_next;
    DWORD data = *(DWORD *)(ent + 0x10);
    for (int i = 0; i < 8; ++i)
        if (s_ent[i] == ent && s_data[i] == data) return s_arrow[i];
    char nm[48];
    GunName(ent, nm);
    bool a = strstr(nm, "ARROW") || strstr(nm, "Arrow");
    s_ent[s_next] = ent, s_data[s_next] = data, s_arrow[s_next] = a, s_next = (s_next + 1) & 7;
    return a;
}

bool LoadTurn(const char *name, float *rel) {
    wchar_t path[MAX_PATH], key[48], buf[256];
    IniPath(path);
    swprintf(key, 48, L"%hs", name);
    if (!GetPrivateProfileStringW(L"gunturn", key, L"", buf, 256, path)) return false;
    float t[9];
    if (swscanf(buf, L"%f %f %f %f %f %f %f %f %f", t, t + 1, t + 2, t + 3, t + 4, t + 5, t + 6, t + 7, t + 8) != 9) return false;
    memcpy(rel, t, 36);
    return true;
}
void SaveTurn(const char *name, const float *rel) {
    wchar_t path[MAX_PATH], key[48], buf[256];
    IniPath(path);
    swprintf(key, 48, L"%hs", name);
    swprintf(buf, 256, L"%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", rel[0], rel[1], rel[2], rel[3], rel[4], rel[5],
             rel[6], rel[7], rel[8]);
    WritePrivateProfileStringW(L"gunturn", key, buf, path);
}


// gunattach2: no parent sat at the camera (gunattach1 logged none), so find
// the gun's parent by its children instead: a parent within 3 m of the camera
// with an attached object 0.2-1 m in front of it. Remembered once found.
BYTE *g_gunParent[4];
int g_gunBone = 51;  // the hand bone the gun hangs from (51 on 30 Oct; muzzle3: start there, the region rule missed it with no camera offset)
int g_camBone = -1;  // the head bone the camera hangs from
volatile LONG g_calls, g_near;
float g_nearest = 1e9f;

// veh7 (2 Nov): after a reload the player is a new object, found into the next
// slot, but the vehicle test kept reading slot 0 (the old, stale player: seat
// bone unreadable, "camera 132 m above"). g_curPlayer = the one seen near the
// camera most recently; slots are reused in turn once all four are taken.
BYTE *g_curPlayer;
int g_nextSlot;

bool IsGunParent(BYTE *comp, const EngineView &v) {
    BYTE *ent = *(BYTE **)(*(BYTE **)(comp + 4) + 0xc);
    const float *p = (const float *)(ent + 0x50);
    float rel[3] = {p[0] - v.base[0], p[1] - v.base[1], p[2] - v.base[2]};
    float dd = Dot(rel, rel);
    for (int i = 0; i < 4; ++i)
        if (g_gunParent[i] == comp) {
            if (dd < 3.0f * 3.0f) g_curPlayer = comp;
            return true;
        }
    if (dd < g_nearest) g_nearest = dd;
    if (dd > 3.0f * 3.0f) return false;
    InterlockedIncrement(&g_near);
    int n = *(int *)(comp + 0x30);
    BYTE **kids = *(BYTE ***)(comp + 0x2c);
    for (int i = 0; i < n && i < kMaxKids; ++i) {
        BYTE *ce = *(BYTE **)(kids[i] + 0xc);
        const float *q = (const float *)(ce + 0x50);
        float cr[3] = {q[0] - v.base[0], q[1] - v.base[1], q[2] - v.base[2]};
        BYTE *cl = *(BYTE **)(ce + 0x68);
        if (cl && *(int *)(cl + 0x20) >= 0 && Dot(cr, cr) < 0.05f * 0.05f) {
            // gunattach8: the parent holding the camera is the player: take it
            // even when the gun isn't where we expect (holstered, swapping),
            // so the camera offset works from the start.
            g_camBone = *(int *)(cl + 0x20);
            if (n >= 3 && EngineOnFoot()) {
                int k = 0;
                while (k < 4 && g_gunParent[k]) ++k;
                if (k == 4) k = g_nextSlot, g_nextSlot = (g_nextSlot + 1) & 3;
                g_gunParent[k] = comp, g_curPlayer = comp;
                return true;
            }
        }
    }
    // gpu3c: only the player (found by its camera child above) counts as the
    // gun's parent; the gun object has attachments of its own and was taken too.
    return false;
}

// Rows right, forward, up (the game's layout) of the right controller in
// the world, with the menu's angle added, plus the hand's world position given
// the camera's. false = no controller.
float g_lineW[3];  // bow14p: the nocked arrow's world direction, last measured
DWORD g_lineWT;
float g_handW[3];  // bow14p: the hand's world place, last HandFrame
float g_gunAxesW[9];  // lefty2
float g_F[9];  // kick1: the controller's own frame (rows right, forward, up), last HandFrame
// bow6p: the bow in the left hand has its own pose ([bow] BowPose), seeded once from the bow's own settings.
static const float *PoseOffRaw() {
    if (!BowActive()) return g_off;
    float *bp = BowHandPose();
    if (!*BowHandPoseSet()) memcpy(bp, g_off, 12), memcpy(bp + 3, g_ang, 12), *BowHandPoseSet() = true;
    return bp;
}
static const float *PoseAngRaw() { return BowActive() ? PoseOffRaw() + 3 : g_ang; }
// lefty1 / lefty3: in left-handed mode the gun sits on the left controller. lefty3 (DrGonzaga 22 Nov: "The left
// handed mode is going to take a lot of tuning. Can we work out how to save weapon position, rotation etc if
// in the left hand?"): each gun keeps its own left-hand place and angle ([gun.<name>] LeftRight .. LeftRoll,
// see TypeIni), so g_off / g_ang already hold the left-hand values and the debug sliders tune them directly.
// Only the bow's own pose (no left-hand copy yet) is still mirrored here.
static float g_mirOff[3], g_mirAng[3];
static const float *PoseOff() {
    const float *o = PoseOffRaw();
    if (!*VrLeftHanded() || !BowActive()) return o;
    g_mirOff[0] = -o[0], g_mirOff[1] = o[1], g_mirOff[2] = o[2];
    return g_mirOff;
}
static const float *PoseAng() {
    const float *a = PoseAngRaw();
    if (!*VrLeftHanded() || !BowActive()) return a;
    g_mirAng[0] = a[0], g_mirAng[1] = -a[1], g_mirAng[2] = -a[2];
    return g_mirAng;
}
bool HandFrame(const EngineView &v, const float cam[3], float H[9], float hand[3]) {
    float m[16], off[3];
    if (!HeadtrackHand(1, m, off)) return false;
    float C[9];
    if (BowActive() && BowAimFix(C)) {  // aimfix1: the aim runs along the arrow; the bow model sits in the bow frame
        float r[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = m[i * 4] * C[j * 3] + m[i * 4 + 1] * C[j * 3 + 1] + m[i * 4 + 2] * C[j * 3 + 2];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) m[i * 4 + j] = r[i * 3 + j];
    }
    const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
    auto W = [&](float x, float y, float z, float *o) {
        for (int i = 0; i < 3; ++i) o[i] = r[i] * x + u[i] * y - f[i] * z;
    };
    float F[9];
    W(m[0], m[4], m[8], F);          // right = controller +x
    W(-m[2], -m[6], -m[10], F + 3);  // forward = controller -z
    W(m[1], m[5], m[9], F + 6);      // up = controller +y
    // Angle: pitch about right, yaw about up, roll about forward (local axes).
    const float *ang = PoseAng();
    float p = ang[0] * 0.0174533f, y = ang[1] * 0.0174533f, rl = ang[2] * 0.0174533f;
    float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(rl), sr = sinf(rl);
    // Local rotation Q = yaw * pitch * roll in (right, forward, up) coordinates.
    float Ry[9] = {cy, sy, 0, -sy, cy, 0, 0, 0, 1};
    float Rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp};
    float Rr[9] = {cr, 0, -sr, 0, 1, 0, sr, 0, cr};
    float T[9], Q[9];
    auto Mul = [](const float *A, const float *B, float *C) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) C[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    };
    Mul(Rp, Rr, T), Mul(Ry, T, Q), Mul(Q, F, H);
    memcpy(g_F, F, sizeof F);
    memcpy(g_gunAxesW, H, sizeof g_gunAxesW);  // lefty2: the gun's right / forward / up in the world
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    for (int i = 0; i < 3; ++i)
        hand[i] = cam[i] + (v.sixdof ? (fr[i] * off[0] + wu[i] * off[1] - ff[i] * off[2]) / v.scale : 0);
    memcpy(g_handW, hand, 12);
    return true;
}

// bow1p: the real right controller's world frame (rows right, forward, up) and position,
// for the arrow carried in the right hand while the bow is in the left.
bool HandWorldRaw(int which, const EngineView &v, const float cam[3], float F[9], float hand[3]);
bool RightHandWorld(const EngineView &v, const float cam[3], float F[9], float hand[3]) { return HandWorldRaw(1, v, cam, F, hand); }
// hands3: either real controller (0 left, 1 right), world frame and position.
bool HandWorldRaw(int which, const EngineView &v, const float cam[3], float F[9], float hand[3]) {
    float m[16], off[3];
    if (!HeadtrackHandRaw(which, m, off)) return false;
    const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
    auto W = [&](float x, float y, float z, float *o) {
        for (int i = 0; i < 3; ++i) o[i] = r[i] * x + u[i] * y - f[i] * z;
    };
    W(m[0], m[4], m[8], F);
    W(-m[2], -m[6], -m[10], F + 3);
    W(m[1], m[5], m[9], F + 6);
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    for (int i = 0; i < 3; ++i)
        hand[i] = cam[i] + (v.sixdof ? (fr[i] * off[0] + wu[i] * off[1] - ff[i] * off[2]) / v.scale : 0);
    return true;
}

// bow1p: the arrow model with the physical bow. No arrow taken: hidden (far below).
// Carried: in the right hand, turned with it as it sits on the bow. Nocked: the bow's
// turn, back end at the right hand. B = the bow's pose as we placed it, rel = its learnt turn.
float g_bowCam[3], g_bowRel[9];  // bow2p: the camera and bow turn from the last player update
bool g_bowCamOk;
// Local turn from (pitch, yaw, roll) degrees applied to a frame F (rows right, forward, up), as HandFrame.
static void TurnFrame(const float ang[3], const float F[9], float out[9]) {
    float p = ang[0] * 0.0174533f, y = ang[1] * 0.0174533f, rl = ang[2] * 0.0174533f;
    float cp = cosf(p), sp = sinf(p), cy = cosf(y), sy = sinf(y), cr = cosf(rl), sr = sinf(rl);
    float Ry[9] = {cy, sy, 0, -sy, cy, 0, 0, 0, 1};
    float Rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp};
    float Rr[9] = {cr, 0, -sr, 0, 1, 0, sr, 0, cr};
    auto Mul = [](const float *A, const float *Bm, float *C) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) C[i * 3 + j] = A[i * 3] * Bm[j] + A[i * 3 + 1] * Bm[3 + j] + A[i * 3 + 2] * Bm[6 + j];
    };
    float T[9], Q[9];
    Mul(Rp, Rr, T), Mul(Ry, T, Q), Mul(Q, F, out);
}

// bow14p: the nocked arrow's line (game's pose) in the bow hand's frame, for BowApply to turn the
// bow so that line runs through the right hand. row = the arrow model's shaft row (towards the tip).
void BowLineMeasure(const float *A, int row, float scale, bool hand) {
    float a[3] = {A[4 * row], A[4 * row + 1], A[4 * row + 2]};
    float al = sqrtf(Dot(a, a));
    if (al < 1e-4f) return;
    for (int k = 0; k < 3; ++k) a[k] /= al;
    float d[3] = {A[12] - g_handW[0], A[13] - g_handW[1], A[14] - g_handW[2]}, t = Dot(d, a), p[3];
    for (int k = 0; k < 3; ++k) p[k] = d[k] - t * a[k];
    if (Dot(p, p) > 0.4f * 0.4f) return;  // not near the hand: not this bow
    const float *R = g_F, *F = g_F + 3, *U = g_F + 6;  // world rows right, forward (-z), up (+y)
    float pl[3] = {Dot(p, R) * scale, Dot(p, U) * scale, -Dot(p, F) * scale}, alc[3] = {Dot(a, R), Dot(a, U), -Dot(a, F)};
    BowSetLine(pl, alc, hand);
    memcpy(g_lineW, a, 12), g_lineWT = GetTickCount();
}

// The arrow model with the physical bow. 1: no arrow taken: hidden (far below). 2: carried in the
// right hand. 3: nocked: glued to the bow in the game's rest pose, slid back by the hand's pull.
// bow7p (DrGonzaga 10 Nov: couldn't get the arrow into the hand; nocked arrow 90 deg off): the arrow
// model's shaft is its own +Y axis, origin at the tip (ArrowRestOnBow: its Y lies along the bow's
// X, tip 0.63 m ahead). bow4p-6p guessed the shaft as the row nearest the bow's Y, the wrong axis,
// so "slide along" moved it sideways and the nocked arrow sat across the bow.
static volatile int g_arrowKind;  // arrowsets1
static volatile DWORD g_arrowKindTick;
static bool g_handGlued;  // hands10: the hand arrow sits on our string at its learnt rest (not at the game's pose)
static bool g_placingHand;  // bow15p: PlaceArrow on behalf of the hand arrow (HandArrow)
void PlaceArrow(BYTE *arrowEnt, const float *B, const EngineView &v, const float cam[3], const float rel[9]) {
    (void)rel;
    int am = BowArrowMode();
    float *A = (float *)(arrowEnt + 0x20), F[9], rh[3];
    if (am == 1) {
        for (int i = 0; i < 3; ++i) A[12 + i] = B[12 + i] - 500 * v.basis[3 + i];
        return;
    }
    if (am < 2) return;
    float sc[3];  // the model's row lengths (scale), kept
    for (int i = 0; i < 3; ++i) sc[i] = sqrtf(Dot(A + 4 * i, A + 4 * i)), sc[i] = sc[i] > 1e-4f ? sc[i] : 1;
    if (am == 2) {
        if (!RightHandWorld(v, cam, F, rh)) return;
        const float *ap = g_placingHand ? BowArrowPose() : BowArrowPoseFirst();  // right, up, (forward unused), pitch, yaw, roll (arrowsets1)
        g_arrowKind = g_placingHand ? 2 : 1, g_arrowKindTick = GetTickCount();
        float R[9];  // controller frame turned by the page's angles: rows right, forward (= shaft), up
        TurnFrame(ap + 3, F, R);
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) A[4 * i + k] = R[i * 3 + k] * sc[i];
        float along = *BowArrowLength();  // bow7p: the tip this far ahead of the hand (Grip along the shaft)
        if (!g_placingHand) along += *BowFirstArrowShift();  // bow15p: the bow's own (first) arrow model
        for (int k = 0; k < 3; ++k) A[12 + k] = rh[k] + ap[0] * F[k] + ap[1] * F[6 + k] + along * R[3 + k];
        return;
    }
    // bow13p (DrGonzaga 11 Nov: "a different approach overall?"; with the plain trigger the game's own
    // animation keeps string and arrow together): nocked, the arrow keeps the game's own pose on the
    // bow (the draw amount, driven by the hands, pulls string and arrow back together).
    if (am == 3) return;
}

// string10 (DrGonzaga 11 Nov: "nice smooth notch"): with the string following the hand, the arrow
// glides from where it was carried onto the string over 150 ms instead of jumping there.
static void NockBlend(BYTE *ent) {
    static BYTE *s_ent;
    static float s_m[16];
    float *A = (float *)(ent + 0x20);
    int am = BowArrowMode();
    if (am == 2) {
        s_ent = ent, memcpy(s_m, A, 64);
        return;
    }
    if (am != 3 || ent != s_ent || !BowStringDriving()) return;
    float x = (float)(BowClockMs() - BowNockMs()) / 150.0f;
    if (x < 0 || x >= 1) return;
    float w = x * x * (3 - 2 * x);
    for (int i = 0; i < 3; ++i) {
        float len = sqrtf(Dot(A + 4 * i, A + 4 * i)), r[3];
        for (int k = 0; k < 3; ++k) r[k] = s_m[4 * i + k] + (A[4 * i + k] - s_m[4 * i + k]) * w;
        float rl = sqrtf(Dot(r, r));
        if (rl > 1e-5f)
            for (int k = 0; k < 3; ++k) A[4 * i + k] = r[k] * len / rl;
        A[12 + i] = s_m[12 + i] + (A[12 + i] - s_m[12 + i]) * w;
    }
}

// bow11p: the hand arrow's model frame in the bow's own arrow's frame (turn 9, origin 3), learnt from
// the game's nocked pose (both in the game bow's axes); then placed as the bow's arrow would be.
void HandArrow(BYTE *arrowEnt, const float loc[9], const float off[3], const float *M, const EngineView &v,
               const float cam[3], const float rel[9]) {
    (void)loc, (void)off;
    float *A = (float *)(arrowEnt + 0x20);
    int am = BowArrowMode();
    // bow13p: nocked: the game's own pose relative to its bow, already moved onto our bow by the
    // caller (string and arrow drawn together by the game's animation).
    if (am == 3) {
        // hands10 (bd40: the bow pointed down, [twohand] BowArrowLine had been re-measured from an arrow
        // posed by the VR hands' bones): with the VR hands on, only an arrow at its learnt rest counts.
        if (g_handGlued || !g_bloodDragon || !*HandsOn())
            BowLineMeasure(A, 0, v.scale, true);  // bow14p: the hand arrow's shaft is its X row
        return;
    }
    if (am == 1) {
        for (int i = 0; i < 3; ++i) A[12 + i] = M[12 + i] - 500 * v.basis[3 + i];
        return;
    }
    // carried: as the bow's own arrow would be carried, through the hand arrow's fixed model turn
    // (bow11p logs, two sessions: its rows are the bow arrow's (Y, -Z, -X): the shaft is its X axis).
    // The learnt origin was unreliable (0.93 / 0.47 m: the game's hand was mid-draw), so none.
    alignas(16) BYTE fake[0x60] = {};
    float *T = (float *)(fake + 0x20);
    T[0] = T[5] = T[10] = T[15] = 1;
    g_placingHand = true;
    PlaceArrow(fake, M, v, cam, rel);
    g_placingHand = false;
    float sc[3];
    for (int i = 0; i < 3; ++i) sc[i] = sqrtf(Dot(A + 4 * i, A + 4 * i)), sc[i] = sc[i] > 1e-4f ? sc[i] : 1;
    static const float P[9] = {0, 1, 0, 0, 0, -1, -1, 0, 0};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) A[4 * i + k] = (P[i * 3] * T[k] + P[i * 3 + 1] * T[4 + k] + P[i * 3 + 2] * T[8 + k]) * sc[i];
    for (int k = 0; k < 3; ++k) A[12 + k] = T[12 + k];
}

void Note(BYTE *comp, BYTE *ent, const EngineView &v) {
    for (int i = 0; i < g_nseen; ++i)
        if (g_seen[i] == comp) return;
    if (g_nseen >= 8) return;
    g_seen[g_nseen++] = comp;
    const float *p = (const float *)(ent + 0x50);
    float rel[3] = {p[0] - v.base[0], p[1] - v.base[1], p[2] - v.base[2]};
    int n = *(int *)(comp + 0x30);
    Log("gunattach: gun parent %p (object %p, class FC3+%08lx) at cam(r %.3f u %.3f f %.3f) with %d attached:", comp,
        ent, *(DWORD *)ent - (DWORD)GetModuleHandleA("FC3_d3d11.dll") + kImageBase, Dot(rel, v.basis),
        Dot(rel, v.basis + 3), Dot(rel, v.basis + 6), n);
    BYTE **kids = *(BYTE ***)(comp + 0x2c);
    for (int i = 0; i < n && i < kMaxKids; ++i) {
        BYTE *ce = *(BYTE **)(kids[i] + 0xc);
        const float *q = (const float *)(ce + 0x50);
        float cr[3] = {q[0] - v.base[0], q[1] - v.base[1], q[2] - v.base[2]};
        BYTE *link = *(BYTE **)(ce + 0x68);
        Log("gunattach:   %p object %p at cam(r %.3f u %.3f f %.3f), bone %d", kids[i], ce, Dot(cr, v.basis),
            Dot(cr, v.basis + 3), Dot(cr, v.basis + 6), link ? *(int *)(link + 0x20) : -99);
    }
}

// muzzle2: when the game places an attached object it then calls the object's "moved" (bd5: a plain
// thiscall(object, 0)) to tell the object's parts it moved; the muzzle flash takes its place from
// that. Do the same after we place the gun.
void *g_movedFn;
volatile LONG g_movedCalls;
void Moved(BYTE *ent) {
    if (!g_movedFn) return;
    BdThisCall(g_movedFn, ent, 1, 0);
    InterlockedIncrement(&g_movedCalls);
}

DWORD __attribute__((fastcall)) Hook(BYTE *comp, void *, DWORD a, DWORD b) {
    // review1: objects with nothing attached (most calls) go straight to the game; counters and
    // the 10 s summary only for the detailed log.
    const bool verbose = g_verboseLog;
    HiResLogTick();  // scopegun2
    if (!verbose && *(int *)(comp + 0x30) == 0) return g_orig(comp, a, b);
    if (verbose) InterlockedIncrement(&g_calls);
    static DWORD lastLog;
    DWORD now = GetTickCount();
    if (verbose && now - lastLog > 10000) {
        lastLog = now;
        LogV("gunattach: %ld attachment updates in 10 s, %ld parents within 3 m, nearest %.2f m, on foot %d, on %d, camera moved %ld, placed %ld (%ld learnt), camera gap %.3f m; not placed: no camera %ld, no gun %ld, no hand %ld; our turn kept %ld, rewritten by the game %ld; gun's own attachments updated %ld (%ld from our pose), moved told %ld",
            InterlockedExchange(&g_calls, 0), InterlockedExchange(&g_near, 0), sqrtf(g_nearest), EngineOnFoot(), (int)g_on, InterlockedExchange(&g_shifted, 0),
            InterlockedExchange(&g_placed, 0), InterlockedExchange(&g_fresh, 0), g_camGap,
            InterlockedExchange(&g_noCam, 0), InterlockedExchange(&g_noGun, 0), InterlockedExchange(&g_noHand, 0),
            InterlockedExchange(&g_rotKept, 0), InterlockedExchange(&g_rotRewritten, 0),
            InterlockedExchange(&g_gunKids, 0), InterlockedExchange(&g_gunKidsFixed, 0), InterlockedExchange(&g_movedCalls, 0));
        LogV("gunattach: game's own gun angle at rest vs learnt: now %.3f, max %.3f (0 = same; >0.05 = out of line)", g_restDev,
            g_restDevMax);
        g_restDevMax = 0;
        g_camGap = 0;
        g_nearest = 1e9f;
    }
    if (*(int *)(comp + 0x30) == 0) return g_orig(comp, a, b);
    // roll1: the gun has attachments of its own (muzzle flash, smoke, maybe
    // the visible model). The game updates them from its own gun pose, so
    // give the gun our pose (with the controller's roll) for that update.
    if (g_gunEnt && g_on && *(BYTE **)(*(BYTE **)(comp + 4) + 0xc) == g_gunEnt) {
        InterlockedIncrement(&g_gunKids);
        static DWORD s_optTick;  // sight2: which optic is fitted
        if (now - s_optTick > 500) s_optTick = now, ScanOptic(comp);
        if (g_roll && EngineOnFoot()) {
            memcpy(g_gunEnt + 0x20, g_gunDrawn, 64);  // kick2: with the recoil, so the silencer etc. kick too
            InterlockedIncrement(&g_gunKidsFixed);
        }
        DWORD rr = g_orig(comp, a, b);
        // bow2p (DrGonzaga 10 Nov: arrow still nocked on the bow, nothing in the right hand): the
        // arrow is one of the bow's own attachments, placed here from our bow pose; move it.
        EngineView bv;
        if (g_bowCamOk && BowArrowMode() && EngineLastView(&bv)) {
            int kn = *(int *)(comp + 0x30);
            BYTE **kk = *(BYTE ***)(comp + 0x2c);
            for (int i = 0; i < kn && i < kMaxKids; ++i) {
                BYTE *ce = kk[i] ? *(BYTE **)(kk[i] + 0xc) : nullptr;
                if (!ce || !IsArrow(ce)) continue;
                // string10: the bow's own arrow as the game has just posed it, in the bow's axes;
                // nocked with the string following the hand, it sits on our string instead
                const float *Bm = (const float *)(g_gunEnt + 0x20);
                float *Am = (float *)(ce + 0x20), sl[9], so[3], sd[3];
                for (int r = 0; r < 3; ++r) {
                    for (int j = 0; j < 3; ++j) sl[r * 3 + j] = Dot(Am + 4 * r, Bm + 4 * j);
                    sd[r] = Am[12 + r] - Bm[12 + r];
                }
                for (int j = 0; j < 3; ++j) so[j] = Dot(sd, Bm + 4 * j);
                bool glue = BowStringArrow(0, sl, so);
                PlaceArrow(ce, (const float *)(g_gunEnt + 0x20), bv, g_bowCam, g_bowRel);
                if (glue)
                    for (int r = 0; r < 3; ++r) {
                        for (int k = 0; k < 3; ++k) Am[4 * r + k] = sl[r * 3] * Bm[k] + sl[r * 3 + 1] * Bm[4 + k] + sl[r * 3 + 2] * Bm[8 + k];
                        Am[12 + r] = Bm[12 + r] + so[0] * Bm[r] + so[1] * Bm[4 + r] + so[2] * Bm[8 + r];
                    }
                if (BowArrowMode() == 3) BowLineMeasure((const float *)(ce + 0x20), 1, bv.scale, false);  // bow14p
                NockBlend(ce);  // string10 (after the line is measured from its final place)
                if (g_roll) Moved(ce);
            }
        }
        return rr;
    }
    EngineView v;
    if (!EngineLastView(&v)) return g_orig(comp, a, b);
    if (!IsGunParent(comp, v)) {
        return g_orig(comp, a, b);
    }
    BYTE *ent = *(BYTE **)(*(BYTE **)(comp + 4) + 0xc);
    if (g_bloodDragon) HandsRestore();  // hands7: the game's own arm pose back before it places what the hands hold
    if (g_nseen < 8) Note(comp, ent, v);
    {
        // car9: is a weapon in the player's hand (bone 51 / 74)? The engine uses
        // it to tell a car seat (no weapon) from on foot.
        int n = *(int *)(comp + 0x30);
        BYTE **kids = *(BYTE ***)(comp + 0x2c);
        for (int i = 0; i < n && i < kMaxKids; ++i) {
            BYTE *ce = *(BYTE **)(kids[i] + 0xc);
            BYTE *link = ce ? *(BYTE **)(ce + 0x68) : nullptr;
            int bone = link ? *(int *)(link + 0x20) : -1;
            if (bone == 51 || bone == 74 || bone == 20) {
                g_handTick = GetTickCount();
                break;
            }
        }
    }
    if (!g_on || !EngineOnFoot()) return g_orig(comp, a, b);
    // gunattach2 moved the whole player, and with it the camera (head bone
    // 83) and the gun but not the body. gunattach4 keeps that as the camera
    // offset (a floating gun), then places the gun itself (bone 51 on 30 Oct)
    // at the controller: its natural place and angle relative to the camera
    // become relative to the hand, plus the menu's offset and angle.
    float *pp = (float *)(ent + 0x50), keep[3] = {pp[0], pp[1], pp[2]};
    for (int i = 0; i < 3; ++i) pp[i] += v.basis[i] * g_cam[0] + v.basis[3 + i] * g_cam[1] + v.basis[6 + i] * g_cam[2];
    InterlockedIncrement(&g_shifted);
    DWORD r = g_orig(comp, a, b);
    pp[0] = keep[0], pp[1] = keep[1], pp[2] = keep[2];
    int n = *(int *)(comp + 0x30);
    BYTE **kids = *(BYTE ***)(comp + 0x2c);
    BYTE *camEnt = nullptr, *gunEnt = nullptr;
    // gunattach7: find the camera object (the child nearest the rendered
    // camera) and the gun (the child on the gun's bone, or else the child in
    // the gun's usual place in front of the camera) on every update, so a
    // weapon change or a moved camera can't leave them unfound.
    float best = 0.5f * 0.5f;
    for (int i = 0; i < n && i < kMaxKids; ++i) {
        BYTE *ce = *(BYTE **)(kids[i] + 0xc);
        BYTE *link = *(BYTE **)(ce + 0x68);
        if (!link || *(int *)(link + 0x20) < 0) continue;
        if (*(int *)(link + 0x20) == g_camBone) {
            camEnt = ce;
            break;
        }
        const float *q = (const float *)(ce + 0x50);
        float d[3] = {q[0] - v.base[0], q[1] - v.base[1], q[2] - v.base[2]};
        if (Dot(d, d) < best) best = Dot(d, d), camEnt = ce;
    }
    if (camEnt) {
        const float *c = (const float *)(camEnt + 0x50);
        // gunpick2: pass 0 the hands (bones 51 / 74, the last one first);
        // pass 1 the region rule, but never a body piece (bones 1, 2, 8:
        // grenade and looting animations swing those in front of the camera)
        // or the holster (bone 16: with a second weapon slot the holstered
        // gun hangs there, and gunpick1 kept taking it once it had been picked,
        // leaving the real gun at the head).
        // c4hands1 (as FC3, for this pick): after the bow (bone 20) the C4 charge (also bone 20) could be
        // taken for the gun; with the detonator ("IED...") in the right hand (51) that one is the gun.
        bool c4pair = false;
        for (int i = 0; i < n && i < kMaxKids && !c4pair; ++i) {
            BYTE *ce = *(BYTE **)(kids[i] + 0xc);
            BYTE *link = *(BYTE **)(ce + 0x68);
            if (ce == camEnt || !link || *(int *)(link + 0x20) != 51 || IsArrow(ce)) continue;
            char nm[48];
            GunName(ce, nm);
            c4pair = !strncmp(nm, "IED", 3);
        }
        for (int pass = 0; pass < 3 && !gunEnt; ++pass)
            for (int i = 0; i < n && i < kMaxKids && !gunEnt; ++i) {
                BYTE *ce = *(BYTE **)(kids[i] + 0xc);
                BYTE *link = *(BYTE **)(ce + 0x68);
                if (ce == camEnt || !link || *(int *)(link + 0x20) < 0) continue;
                int bone = *(int *)(link + 0x20);
                if (c4pair && bone == 20) continue;
                if (pass < 2) {
                    bool hand = bone == 51 || bone == 74 || bone == 20;
                    // bow2: the bow hangs on bone 20 (the left hand); learning was hand-only (51 / 74) so it never learnt
                    if (hand && IsArrow(ce)) continue;  // bow1
                    if (hand && (pass == 1 || bone == g_gunBone)) {
                        gunEnt = ce;
                        if (g_gunBone != bone) LogV("gunattach: gun now on bone %d", bone);
                        g_gunBone = bone;
                    }
                    continue;
                }
                if (bone <= 8 || bone == 16 || IsArrow(ce)) continue;
                const float *q = (const float *)(ce + 0x50);
                float d[3] = {q[0] - c[0], q[1] - c[1], q[2] - c[2]};
                float f = Dot(d, v.basis + 6), r = Dot(d, v.basis), u = Dot(d, v.basis + 3);
                if (f > 0.1f && f < 1.0f && fabsf(r) < 0.5f && u > -0.5f && u < 0.2f) {
                    gunEnt = ce;
                    if (g_gunBone != bone) LogV("gunattach: gun now on bone %d", bone);
                    g_gunBone = bone;
                }
            }
    }
    if (!camEnt) InterlockedIncrement(&g_noCam);
    else if (!gunEnt) InterlockedIncrement(&g_noGun);
    // emptyhands1: no weapon in the hand for 0.4 s: the hands still follow the controllers.
    // emptyhands3 (DrGonzaga 17 Nov, bd96: "Arms took a while to appear and both arms were linked to right controller
    // only"): the controllers and head were only passed to the hands when a gun was placed, so with no gun the hands
    // went to wherever the controllers were the last time a gun was out. Now they are passed here too.
    if (!gunEnt && camEnt && g_bloodDragon && GetTickCount() - g_handTick > 400 && HandsEmptyWanted()) {
        const float *Gc = (const float *)(camEnt + 0x20);
        float camE[3] = {Gc[12], Gc[13], Gc[14] + EngineCrouchLift()}, FL[9], pL[3], FR[9], pR[3];
        bool okL = HandWorldRaw(0, v, camE, FL, pL), okR = HandWorldRaw(1, v, camE, FR, pR);
        HandsControllers(okL ? FL : nullptr, pL, okR ? FR : nullptr, pR, v.scale, false);
        HandsHead(v.pos, v.posAxes, EngineScoped(), camE, v.fwd);
        HandsFrameEmpty(comp, ent);
        static int s_en;
        if (s_en < 6 && okL != okR) ++s_en, Log("hands: no weapon: only the %s controller is tracked", okL ? "left" : "right");
    }
    if (!gunEnt || !camEnt || !g_hand) return r;
    float *G = (float *)(camEnt + 0x20);  // camera rows right, forward, up; position
    // crouch3: with a real crouch the view is lifted by the game's own crouch drop; the hands, gun and arrows
    // are placed from here, so they get the same lift (they sat that far too low).
    float camLift[3] = {G[12], G[13], G[14] + EngineCrouchLift()};
    const float *cam = camLift;
    float *M = (float *)(gunEnt + 0x20), *gp = M + 12;
    float H[9], hand[3];
    if (!HandFrame(v, cam, H, hand)) {
        InterlockedIncrement(&g_noHand);
        return r;
    }
    // c4hands1: HandFrame followed HeadtrackHand(1), which is the left hand with C4 out: keep
    // that for the charge and put the detonator (the gun) on the right controller.
    float HL[9], handL[3];
    bool c4 = GunAttachC4Split();
    if (c4) {
        float F[9], rh[3];
        if (RightHandWorld(v, cam, F, rh)) {
            memcpy(HL, H, 36), memcpy(handL, hand, 12);
            TurnFrame(PoseAng(), F, H), memcpy(hand, rh, 12), memcpy(g_F, F, 36);
        } else {
            c4 = false;
        }
    }
    // gunattach6: lock the gun to the controller. Its turn relative to the
    // game's look (Rel = M G^T) is learnt once, from a pose the game wrote
    // (new gun object, or after "Re-learn"), then the gun is placed purely from
    // the hand: rotation Rel * hand, position hand + offset in the hand's axes.
    // Nothing is read back from frames we placed (gunattach4/5 fed back).
    static BYTE *s_ent;
    static float s_rel[9], s_wrote[16], s_prev[9];
    static bool s_learning;
    static int s_stable;
    static DWORD s_since;
    static int s_bone;
    static bool s_manual;
    if (gunEnt != s_ent || g_relearn || g_gunBone != s_bone) {
        // gunid2: also when the same gun object moves bone (at load it hangs
        // on bone 16 first, then goes to the hand: learning there was off).
        bool manual = g_relearn;
        s_ent = gunEnt, g_relearn = false, s_bone = g_gunBone;
        GunName(gunEnt, g_name);
        if (g_bloodDragon) HandsGunSeen(g_name);  // emptyhands4
        if (strcmp(g_name, g_turned) || g_loadedLefty != *VrLeftHanded()) {  // lefty3: reload on a hand switch too
            strcpy(g_turned, g_name);
            TypeIni(false, g_name);
            strcpy(g_optic, "iron");  // sight2: until the gun's own parts are looked at
            SightIni(false);
            // chargeglass3: the scoped rifle is DLC_BD_VIPER; un-upgraded it is bolt action (no RELOADKIT). Set here on every
            // gun change so it can't carry over from another gun; ScanOptic refines it once the gun's parts are read.
            g_singleShot = !strncmp(g_turned, "DLC_BD_VIPER", 12);
        }
        // A saved turn for this weapon is used straight away (same every
        // session); "Re-learn gun" learns it again and saves the new one.
        if (!manual && LoadTurn(g_name, s_rel)) {
            if (s_learning || !s_manual) LogV("gunattach: gun %s (%p, bone %d): saved turn", g_name, gunEnt, g_gunBone);
            s_learning = false;
        } else {
            // relearn1: leave the gun to the game until its pose relative to the
            // camera holds still (the draw / swap animation has finished), then learn.
            s_learning = true, s_stable = 0, s_since = GetTickCount(), s_manual = manual;
        }
    }
    if (s_learning) {
        if (!memcmp(M, s_wrote, 64)) return r;  // still our own pose: wait for the game's
        float rel[9], diff = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                rel[i * 3 + j] = Dot(M + 4 * i, G + 4 * j);
                diff = fmaxf(diff, fabsf(rel[i * 3 + j] - s_prev[i * 3 + j]));
            }
        memcpy(s_prev, rel, sizeof rel);
        // learn2: walking bobs the gun, so "steady" (relearn1) rarely came and
        // the 5 s timeout learnt a pose mid-animation (gun out of line). Now:
        // average the pose over a calm stretch (small changes only, bobbing
        // allowed, swaps / throws not), and never learn from a timeout. After
        // 8 s the calm limit is loosened.
        static float s_sum[9];
        DWORD age = GetTickCount() - s_since;
        float calm = age > 8000 ? 0.06f : 0.03f;
        if (diff < calm) {
            if (!s_stable) memset(s_sum, 0, sizeof s_sum);  // a fresh calm stretch
            for (int k = 0; k < 9; ++k) s_sum[k] += rel[k];
            ++s_stable;
        } else {
            memset(s_sum, 0, sizeof s_sum), s_stable = 0;
        }
        // gunid2: only learn a new turn with the gun in the hand (bone 51 / 74)
        if (s_stable >= 60 && age > 600 && (g_gunBone == 51 || g_gunBone == 74 || g_gunBone == 20)) {
            // average, then make the rows unit length and square again
            float *a = s_sum, *b = s_sum + 3, *c = s_sum + 6;
            float la = sqrtf(Dot(a, a));
            for (int k = 0; k < 3; ++k) a[k] /= la;
            float d = Dot(b, a);
            for (int k = 0; k < 3; ++k) b[k] -= d * a[k];
            float lb = sqrtf(Dot(b, b));
            for (int k = 0; k < 3; ++k) b[k] /= lb;
            float cx[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            if (Dot(cx, c) < 0) for (int k = 0; k < 3; ++k) cx[k] = -cx[k];
            memcpy(c, cx, 12);
            memcpy(s_rel, s_sum, sizeof s_rel);
            memset(s_sum, 0, sizeof s_sum);
            s_learning = false;
            SaveTurn(g_name, s_rel);
            InterlockedIncrement(&g_fresh);
            Log("gunattach: learnt gun %s %p turn after %lu ms (calm average): %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f",
                g_name, gunEnt, age, s_rel[0], s_rel[1], s_rel[2], s_rel[3], s_rel[4], s_rel[5], s_rel[6], s_rel[7], s_rel[8]);
        }
        return r;
    }
    // melee1 check: the game's own turn for the gun (before we overwrite it)
    // against the learnt one, whenever the game holds it still. If the gun
    // drifts out of line after melee, this shows whether the game's rest pose
    // itself moved (then re-learning on settle is the fix).
    if (g_verboseLog) {  // review1: both checks below only feed the detailed log
        static float s_last[9];
        static int s_still;
        float rel[9], ch = 0, dev = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                int k = i * 3 + j;
                rel[k] = Dot(M + 4 * i, G + 4 * j);
                ch = fmaxf(ch, fabsf(rel[k] - s_last[k]));
                dev = fmaxf(dev, fabsf(rel[k] - s_rel[k]));
            }
        memcpy(s_last, rel, sizeof rel);
        s_still = ch < 0.01f ? s_still + 1 : 0;
        if (s_still >= 40) {
            g_restDev = dev;
            if (dev > g_restDevMax) g_restDevMax = dev;
        }
    }
    // chargeglass2 (DrGonzaga 24 Nov, bd157: "it worked, then I reloaded, moved a bit, then it stopped working"): how far the
    // game's own gun turn (its animation: charging, reloading) is from the learnt rest turn, every frame the game wrote it.
    if (memcmp(M, s_wrote, 64)) {
        float dev = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) dev = fmaxf(dev, fabsf(Dot(M + 4 * i, G + 4 * j) - s_rel[i * 3 + j]));
        g_gameGunDev = dev, g_gameGunDevT = GetTickCount();
    }
    // roll1 check: is the turn we wrote last time still there (game draws our
    // turn), or did the game write its own (then roll and muzzle come from it)?
    if (g_verboseLog) {
        float e = 0;
        for (int i = 0; i < 12; ++i)
            if ((i & 3) != 3) e += fabsf(M[i] - s_wrote[i]);
        InterlockedIncrement(e < 1e-3f ? &g_rotKept : &g_rotRewritten);
    }
    // bow1: the arrow (if any) keeps its place on the bow as the game animates it.
    float gameGun[16];
    memcpy(gameGun, M, 64);
    // rocket1 (DrGonzaga 3 Nov: the reloaded rocket floated off the RPG): anything
    // else in the game's hands (the arrow, a fresh rocket) keeps its place on the
    // gun as the game animates it, moved over to our gun like the bow1 arrow.
    BYTE *extra[4];
    int nx = 0;
    for (int i = 0; i < n && i < kMaxKids && nx < 4; ++i) {
        BYTE *ce = *(BYTE **)(kids[i] + 0xc);
        BYTE *link = *(BYTE **)(ce + 0x68);
        if (ce == gunEnt || ce == camEnt || !link) continue;
        int bone = *(int *)(link + 0x20);
        // bow8p (DrGonzaga 10 Nov: arrow sometimes 50 cm in front of the bow, turned): an arrow on
        // any other bone (the game's reload hand?) was left at the game's pose; place it too.
        bool arrowAny = BowArrowMode() && IsArrow(ce);
        // shell1 (DrGonzaga 12 Nov, Blood Dragon: the shotgun's shell floated apart from the shotgun): the shell is
        // on some other bone (the loading hand). Anything the game holds within 28 cm of its own gun pose, and not
        // on a body / holster bone (1, 2, 8, 16, 83), goes with the gun too.
        bool nearGun = false;
        if (g_bloodDragon && bone != 51 && bone != 74 && bone != 20 && bone != 1 && bone != 2 && bone != 8 && bone != 16 && bone != 83 &&
            !IsBadReadPtr(ce + 0x20, 64)) {
            const float *A = (const float *)(ce + 0x20);
            float dx = A[12] - M[12], dy = A[13] - M[13], dz = A[14] - M[14], d2 = dx * dx + dy * dy + dz * dz;
            nearGun = d2 < 0.28f * 0.28f;
            static BYTE *s_seen[24];
            static int s_ns;
            if (nearGun && s_ns < 24) {
                bool seen = false;
                for (int k = 0; k < s_ns; ++k) seen |= s_seen[k] == ce;
                if (!seen) s_seen[s_ns++] = ce, Log("gunattach: object %p on bone %d is %.2f m from the game's gun: it goes with the gun (%s)", ce, bone, sqrtf(d2), g_name);
            }
        }
        if (bone == 51 || bone == 74 || bone == 20 || arrowAny || nearGun) extra[nx++] = ce;
    }
    const float *po = PoseOff();
    float P[3] = {po[0], po[2], po[1]};  // right, forward, up
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k)
            M[4 * i + k] = s_rel[i * 3] * H[k] + s_rel[i * 3 + 1] * H[3 + k] + s_rel[i * 3 + 2] * H[6 + k];
        gp[i] = hand[i] + P[0] * H[i] + P[1] * H[3 + i] + P[2] * H[6 + i];
    }
    {
        float d[3] = {cam[0] - v.base[0], cam[1] - v.base[1], cam[2] - v.base[2]};
        float dd = sqrtf(Dot(d, d));
        if (dd > g_camGap) g_camGap = dd;
    }
    memcpy(g_gunPose, M, 64), g_gunEnt = gunEnt;  // kick1: without the hip recoil (scope uses it)
    {
        // kick1: hip-fire recoil, the gun turns about the hand and slides back.
        float R[9], bk[3];
        if (GunKickNow(g_F, R, bk)) {
            float d[3] = {gp[0] - hand[0], gp[1] - hand[1], gp[2] - hand[2]};
            for (int i = 0; i < 3; ++i) {
                float o[3];
                GunKickRotate(R, M + 4 * i, o);
                memcpy(M + 4 * i, o, 12);
            }
            float o[3];
            GunKickRotate(R, d, o);
            for (int i = 0; i < 3; ++i) gp[i] = hand[i] + o[i] + bk[i];
        }
    }
    {   // bow4p (DrGonzaga 10 Nov): after the shot the bow's front pitches down, then springs back.
        float a = BowDropNow() * 0.0174533f;
        if (a > 1e-4f) {
            float c = cosf(a), s = sinf(a);
            // bow14p (DrGonzaga 11 Nov: with the bow canted it dipped to the right): the front turns
            // towards the ground (world up), about the level axis across the aim.
            const float *wup = v.posAxes + 3, *fw = GetTickCount() - g_lineWT < 3000 ? g_lineW : H + 3;  // the arrow's line
            float ka[3] = {wup[1] * fw[2] - wup[2] * fw[1], wup[2] * fw[0] - wup[0] * fw[2], wup[0] * fw[1] - wup[1] * fw[0]};
            float kl = sqrtf(Dot(ka, ka));
            if (kl < 1e-4f) ka[0] = H[0], ka[1] = H[1], ka[2] = H[2], kl = 1;
            for (int k = 0; k < 3; ++k) ka[k] /= kl;
            auto Pitch = [&](float *q) {  // Rodrigues about ka
                float kx[3] = {ka[1] * q[2] - ka[2] * q[1], ka[2] * q[0] - ka[0] * q[2], ka[0] * q[1] - ka[1] * q[0]}, kd = Dot(ka, q);
                for (int k = 0; k < 3; ++k) q[k] = q[k] * c + kx[k] * s + ka[k] * kd * (1 - c);
            };
            for (int i = 0; i < 3; ++i) Pitch(M + 4 * i);
            float d[3] = {gp[0] - hand[0], gp[1] - hand[1], gp[2] - hand[2]};
            Pitch(d);
            for (int i = 0; i < 3; ++i) gp[i] = hand[i] + d[i];
        }
        float dc = BowDipCurve();  // bow15p: and moves down / forward a little
        if (dc > 1e-3f && BowOut()) {
            const float *wup = v.posAxes + 3, *fw = GetTickCount() - g_lineWT < 3000 ? g_lineW : H + 3;
            for (int i = 0; i < 3; ++i) gp[i] += dc * (-*BowDipDown() * wup[i] + *BowDipForward() * fw[i]) / v.scale;
        }
    }
    if (g_bloodDragon && *HandsZipHookGame() && HandsZipNow() && !strncmp(g_name, "FC3_Props:ZipLine", 17)) {
        // zip1: the zipline's hook is "the gun" for half a second: it stays where the game has it (on the line)
        static DWORD s_t;
        if (GetTickCount() - s_t > 5000) s_t = GetTickCount(), Log("gunattach: zipline hook left where the game has it");
        memcpy(M, gameGun, 64);
    }
    memcpy(s_wrote, M, 64);
    memcpy(g_gunDrawn, M, 64);
    GunAttachGlueTick(gunEnt);  // glue1
    memcpy(g_bowCam, cam, 12), memcpy(g_bowRel, s_rel, 36), g_bowCamOk = true;  // bow2p
    if (BowOut()) {  // string10: where the right hand is on the string's path, for this frame's bow pose
        float RF[9], rhw[3];
        bool hv = RightHandWorld(v, cam, RF, rhw);
        BowStringFrame(gunEnt, M, hv ? rhw : nullptr, RF, hand, v.scale);
    }
    if (g_roll) Moved(gunEnt);
    BYTE *c4Ent = nullptr;
    for (int x = 0; x < nx; ++x) {
        BYTE *arrowEnt = extra[x];
        // Arrow in the game bow's axes (rows), then put back on our bow.
        float *A = (float *)(arrowEnt + 0x20), loc[9], off[3], d[3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) loc[i * 3 + j] = Dot(A + 4 * i, gameGun + 4 * j);
            d[i] = A[12 + i] - gameGun[12 + i];
        }
        for (int j = 0; j < 3; ++j) off[j] = Dot(d, gameGun + 4 * j);
        if (IsArrow(arrowEnt)) {
            bool glued = BowStringArrow(1, loc, off);  // string10: nocked, it sits on our string
            g_handGlued = glued;
        }
        BYTE *xl = *(BYTE **)(arrowEnt + 0x68);
        if (c4 && xl && *(int *)(xl + 0x20) == 20 && !IsArrow(arrowEnt)) {
            // c4hands1: the charge keeps its turn against the detonator as the game animates it,
            // on a detonator turned like the left controller; placed at the left hand.
            float ML[9], gameA[16];
            {  // c4mirror1: is the game's charge a mirrored model? Read from a charge we have not written yet.
                static BYTE *s_detEnt;
                if (arrowEnt != s_detEnt) {
                    s_detEnt = arrowEnt;
                    float cx[3] = {A[5] * A[10] - A[6] * A[9], A[6] * A[8] - A[4] * A[10], A[4] * A[9] - A[5] * A[8]};
                    float det = A[0] * cx[0] + A[1] * cx[1] + A[2] * cx[2];
                    HandsC4Mirror(det < 0);
                    static int s_n;
                    if (s_n < 6) ++s_n, Log("gunattach: C4 charge %p as the game has it: determinant %.3f (%s); rows [%.2f %.2f %.2f] [%.2f %.2f %.2f] [%.2f %.2f %.2f]", arrowEnt, det,
                                            det < 0 ? "mirrored model" : "not mirrored", A[0], A[1], A[2], A[4], A[5], A[6], A[8], A[9], A[10]);
                }
            }
            if (g_bloodDragon && !HandsC4Learnt()) {  // c4learn2: while the hold is learnt the charge stays the game's
                HandsC4(A, A);
                continue;
            }
            // c4steady1 (DrGonzaga 15 Nov: "C4 hand and C4 is very 'jumpy', seems to be shaking between two conflicting
            // poses ... the arm and C4 rotate around different axis"): this runs more than once a frame and the game
            // does not always put the charge back in between, so on those passes "the game's pose" read here was
            // the one written a moment ago: the charge's turn was taken from our own, and the hand (which goes by
            // game pose against ours) fell back to the game's animation. Now the game's pose is only taken when the
            // matrix is not the one we wrote; otherwise the last one the game did write is used again.
            static float s_c4Wrote[16], s_c4Game[16], s_c4Loc[9];
            static BYTE *s_c4Ent;
            static DWORD s_c4LogT;
            static int s_c4Fresh, s_c4Again;
            bool fresh = arrowEnt != s_c4Ent || memcmp(A, s_c4Wrote, 64) != 0;
            if (fresh) memcpy(s_c4Game, A, 64), memcpy(s_c4Loc, loc, 36), s_c4Ent = arrowEnt, ++s_c4Fresh;
            else memcpy(loc, s_c4Loc, 36), ++s_c4Again;
            if (GetTickCount() - s_c4LogT > 10000) {
                if (s_c4LogT && g_verboseLog) Log("gunattach: C4 charge, 10 s: the game's own pose read %d times, our own pose seen instead %d times (the last game pose kept)", s_c4Fresh, s_c4Again);
                s_c4LogT = GetTickCount(), s_c4Fresh = s_c4Again = 0;
            }
            memcpy(gameA, s_c4Game, 64);  // hands23: where the game had the charge (against its own left hand)
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k) ML[i * 3 + k] = s_rel[i * 3] * HL[k] + s_rel[i * 3 + 1] * HL[3 + k] + s_rel[i * 3 + 2] * HL[6 + k];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) A[4 * i + k] = loc[i * 3] * ML[k] + loc[i * 3 + 1] * ML[3 + k] + loc[i * 3 + 2] * ML[6 + k];
                A[12 + i] = handL[i] + g_c4Off[0] * HL[i] + g_c4Off[2] * HL[3 + i] + g_c4Off[1] * HL[6 + i];
            }
            memcpy(s_c4Wrote, A, 64);
            if (g_bloodDragon) HandsC4(gameA, A), c4Ent = arrowEnt;  // hands23; c4rigid1: placed again below
            if (g_roll && !c4Ent) Moved(arrowEnt);  // c4learn2: told once, after the last write
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k)
                A[4 * i + k] = loc[i * 3] * M[k] + loc[i * 3 + 1] * M[4 + k] + loc[i * 3 + 2] * M[8 + k];
            A[12 + i] = gp[i] + off[0] * M[i] + off[1] * M[4 + i] + off[2] * M[8 + i];
        }
        if (IsArrow(arrowEnt) && BowArrowMode()) {
            // bow10p (DrGonzaga 10 Nov; bow8p/9p logs): after the first shot the bow carries no arrow of
            // its own any more; each new arrow lives on the player's right hand bone (1, then 51).
            // bow11p (bow10p log: placed exactly like the first arrow, yet seen 90 deg off): that arrow's
            // model is set up turned (and shifted) against the bow's own arrow. Measure how, from the
            // game's own pose while it holds the arrow nocked and still, then place it through that.
            HandArrow(arrowEnt, loc, off, M, v, cam, s_rel);
            NockBlend(arrowEnt);  // string10
        }
        if (g_roll) Moved(arrowEnt);
    }
    InterlockedIncrement(&g_moved);
    InterlockedIncrement(&g_placed);
    if (g_bloodDragon) {  // hands3: the real controllers, for a hand that is not on the gun
        float FL[9], pL[3], FR[9], pR[3];
        bool okL = HandWorldRaw(0, v, cam, FL, pL), okR = HandWorldRaw(1, v, cam, FR, pR);
        HandsControllers(okL ? FL : nullptr, pL, okR ? FR : nullptr, pR, v.scale, TwoHandHeld());
        float Cw[16];
        if (c4Ent) {
            if (HandsC4Charge(Cw)) {  // c4rigid1: the charge sits on the posed left hand (hands.cpp)
                float *A = (float *)(c4Ent + 0x20);
                for (int i = 0; i < 4; ++i)
                    for (int k = 0; k < 3; ++k) A[4 * i + k] = Cw[4 * i + k];
            }
            if (g_roll) Moved(c4Ent);
        }
        HandsHead(v.pos, v.posAxes, EngineScoped(), cam, v.fwd);  // hands4
    }
    HandsFrame(comp, ent, g_gunBone, M);  // hands: this frame's gun pose for the skin hook
    return r;
}

}  // namespace
int GunAttachArrowKind() { return GetTickCount() - g_arrowKindTick < 300 ? g_arrowKind : 0; }

// bd5: Blood Dragon. The attachment update is at 0x102da9a9, the same thiscall(comp, a, b) and
// the same fields (children comp+0x2c / +0x30, object +0x20 matrix, link +0x68, bone link+0x20),
// read side by side with FC3's. Told-it-moved is 0x1029ce48, thiscall(object, arg).
// scopegun2 (FC3 hand-over 22 Nov, DrGonzaga: "the sniper rifle disappearing when scoped"): scoped guns are the game's
// "hi-res scope" weapon class. Its SetHiResScope (BD+0x10706aa5, thiscall (this, bool on), ret 4; state byte +0x50,
// bUseHiResScope +0x51) is called each frame with "ironsight transition past fHiResLowResScopeSwitchTransitionPoint"
// (+0x4c). Switching on starts the game's own scope render and, through BD+0x10704ec6 (stdcall (&entity, bool), ret 8:
// every mesh part but the scope ring 0xf1ffce61 and 0xa79a90e6 set visible = !on), hides the gun. A stub in front
// turns "on" into "off" while [aim] ScopeShowGun is set, so the gun keeps its parts and the game's scope render is
// skipped (the mod draws its own scope from 0.9 m ahead of the hand, [aim] ScopeCameraAhead, so the gun can't block it).
static volatile BYTE g_showGun = 1;
static volatile LONG g_showGunHits;
bool *GunAttachScopeShowGun() { return (bool *)&g_showGun; }
static void HiResInstallBd(BYTE *bd) {
    static const BYTE kHead[12] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x7e, 0x04, 0x8b, 0x07};
    BYTE *fn = bd + 0x706aa5;
    if (memcmp(fn, kHead, 12) || memcmp(fn + 0x2a, "\x38\x5e\x50", 3)) {
        Log("gunattach: scopegun2: SetHiResScope doesn't match; the game still hides scoped guns");
        return;
    }
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    BYTE *p = t;
    *p++ = 0x80, *p++ = 0x3d, *(BYTE **)p = (BYTE *)&g_showGun, p += 4, *p++ = 0x00;  // cmp byte [flag],0
    *p++ = 0x74, *p++ = 0x15;                                                          // je orig
    *p++ = 0x80, *p++ = 0x7c, *p++ = 0x24, *p++ = 0x04, *p++ = 0x00;                   // cmp byte [esp+4],0
    *p++ = 0x74, *p++ = 0x0e;                                                          // je orig
    *p++ = 0xff, *p++ = 0x05, *(LONG **)p = (LONG *)&g_showGunHits, p += 4;            // inc dword [hits]
    *p++ = 0xc7, *p++ = 0x44, *p++ = 0x24, *p++ = 0x04, *(DWORD *)p = 0, p += 4;       // mov dword [esp+4],0
    memcpy(p, kHead, 7), p += 7;                                                       // orig: push ebp; mov ebp,esp; push esi; mov esi,ecx; push edi
    *p++ = 0xe9, *(int *)p = (int)(fn + 7 - (p + 4)), p += 4;                          // jmp fn+7
    if (!PatchJmp(fn, t, 7)) {
        Log("gunattach: scopegun2: could not patch");
        return;
    }
    Log("gunattach: scopegun2: SetHiResScope (BD+10706aa5) hooked; scoped guns stay visible while [aim] ScopeShowGun is on (%d)", (int)g_showGun);
}
static void HiResLogTick() {
    static LONG s_seen;
    static int s_n;
    static DWORD s_t;
    const LONG h = g_showGunHits;
    if (h == s_seen) return;
    if (s_n < 5 || GetTickCount() - s_t > 60000) {
        ++s_n, s_t = GetTickCount();
        Log("gunattach: scopegun2: the game wanted to hide the scoped gun; kept it (%ld times so far)", h);
    }
    s_seen = h;
}

void GunAttachInstallBd(BYTE *bd) {
    static const BYTE kHead[12] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
    static const BYTE kMovedHead[12] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0x56, 0x8b, 0xf1, 0xff, 0x76, 0x68};
    BYTE *fn = bd + 0x2da9a9, *mv = bd + 0x29ce48;
    if (memcmp(fn, kHead, 12) || memcmp(mv, kMovedHead, 12)) {
        Log("gunattach: Blood Dragon code doesn't match; off");
        return;
    }
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, kHead, 5);  // push ebx; mov ebx,esp; push ecx; push ecx
    t[5] = 0xe9;
    *(int *)(t + 6) = (int)(fn + 5 - (t + 10));
    g_orig = (AttachFn)(void *)t;
    if (!PatchJmp(fn, (void *)Hook, 5)) {
        Log("gunattach: could not patch; off");
        return;
    }
    g_movedFn = mv;
    GunAttachLoad();
    Log("gunattach: attachment update hooked (BD+102da9a9); gun in hand %s", g_on ? "on" : "off");
    {  // scopegun2
        wchar_t path[MAX_PATH];
        IniPath(path);
        g_showGun = GetPrivateProfileIntW(L"aim", L"ScopeShowGun", 1, path) != 0;
    }
    HiResInstallBd(bd);
}

static void Ini(bool save) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    static const wchar_t *const kKeys[9] = {L"Right", L"Up", L"Forward", L"Pitch", L"Yaw", L"Roll",
                                            L"CameraRight", L"CameraUp", L"CameraForward"};
    float *vals[9] = {&g_off[0], &g_off[1], &g_off[2], &g_ang[0], &g_ang[1], &g_ang[2], &g_cam[0], &g_cam[1], &g_cam[2]};
    for (int i = 0; i < 9; ++i) {
        if (save && i < 6 && g_loadedLefty) continue;  // lefty3: the [gun] default stays a right-hand one
        if (save) {
            swprintf(buf, 32, L"%.4f", *vals[i]);
            WritePrivateProfileStringW(L"gun", kKeys[i], buf, path);
        } else if (GetPrivateProfileStringW(L"gun", kKeys[i], L"", buf, 32, path)) {
            *vals[i] = (float)_wtof(buf);
        }
    }
    if (save) WritePrivateProfileStringW(L"gun", L"On", g_on ? L"1" : L"0", path);
    else g_on = GetPrivateProfileIntW(L"gun", L"On", 0, path) != 0;
    static const wchar_t *const kC4[3] = {L"C4ChargeRight", L"C4ChargeUp", L"C4ChargeForward"};  // c4hands1
    for (int i = 0; i < 3; ++i) {
        if (save) {
            swprintf(buf, 32, L"%.4f", g_c4Off[i]);
            WritePrivateProfileStringW(L"gun", kC4[i], buf, path);
        } else if (GetPrivateProfileStringW(L"gun", kC4[i], L"", buf, 32, path)) {
            g_c4Off[i] = (float)_wtof(buf);
        }
    }
    if (save) WritePrivateProfileStringW(L"gun", L"C4BothHands", g_c4Split ? L"1" : L"0", path);
    else g_c4Split = GetPrivateProfileIntW(L"gun", L"C4BothHands", 1, path) != 0;
}

// kick1 (DrGonzaga 2 Nov): hip-fire recoil. Each player shot (not scoped) kicks
// the gun up (degrees, along a per-gun direction turned clockwise from the
// controller's up) and back (m), and it settles back like the scope recoil
// (critically damped: peak ~40 ms, gone in ~0.25 s). Drawn only: shots still
// leave along the unkicked beam. Cost: a few multiplies per gun update.
// kick2 (DrGonzaga 2 Nov): recoil grows with the gun's distance from the head,
// on the same per-gun distances as the gunstock spread (stock.cpp): [3], [4]
// within "accurate within", [0], [1] from "worst at" on.
static float g_kick[5] = {4.0f, 0.025f, 0.0f, 2.0f, 0.0125f};  // up, back at worst (deg, m), direction (deg), up, back when close
float *GunAttachKick() { return g_kick; }
static double s_kickT[4] = {-9, -9, -9, -9};
static float s_kickS[4][2];  // kick2: each shot's up (deg) and back (m), from where the gun was
// aimdev1 (DrGonzaga 8 Nov: dot, beam and recoil should point where the bullets go):
// each shot's real direction against the beam, as a world turn vector (axis *
// angle, radians; the mean over a shotgun's pellets). The gun's kick goes that way
// (its size stays the per-gun setting) and the beam / dot turn exactly that far,
// then both settle back with the kick.
static float s_kickRv[4][3];
static int s_kickCnt[4];
static int s_kickN;
static bool g_kickFollow = true;  // [aim] KickFollowsShots
bool *GunKickFollowsShots() { return &g_kickFollow; }
static double KickClock() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
void GunAttachShot() {
    double t = KickClock(), last = s_kickT[(s_kickN + 3) & 3];
    if (t - last < 0.04) return;  // a shotgun's pellets are one shot
    float d = StockKickT();  // 0 close .. 1 arm out
    int k = s_kickN & 3;
    s_kickS[k][0] = g_kick[3] + (g_kick[0] - g_kick[3]) * d;
    s_kickS[k][1] = g_kick[4] + (g_kick[1] - g_kick[4]) * d;
    s_kickRv[k][0] = s_kickRv[k][1] = s_kickRv[k][2] = 0, s_kickCnt[k] = 0;
    s_kickT[k] = t;
    ++s_kickN;
}
// aimdev1: called for each player bullet (pellet) with the beam and the bullet's
// direction (unit, world), after GunAttachShot.
void GunAttachShotDir(const float beam[3], const float shot[3]) {
    int k = (s_kickN + 3) & 3;
    if (KickClock() - s_kickT[k] > 0.05) return;
    float c[3] = {beam[1] * shot[2] - beam[2] * shot[1], beam[2] * shot[0] - beam[0] * shot[2],
                  beam[0] * shot[1] - beam[1] * shot[0]};
    float sn = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    float cs = beam[0] * shot[0] + beam[1] * shot[1] + beam[2] * shot[2];
    if (sn < 1e-7f) { ++s_kickCnt[k]; return; }
    float ang = atan2f(sn, cs);
    for (int i = 0; i < 3; ++i) s_kickRv[k][i] += c[i] / sn * ang;
    ++s_kickCnt[k];
}
static void RotFromVec(const float v[3], float R[9]) {
    float a = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (a < 1e-7f) {
        static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        memcpy(R, I, sizeof I);
        return;
    }
    float X = v[0] / a, Y = v[1] / a, Z = v[2] / a, c = cosf(a), s = sinf(a), k = 1 - c;
    float r[9] = {c + X * X * k, X * Y * k - Z * s, X * Z * k + Y * s, Y * X * k + Z * s, c + Y * Y * k,
                  Y * Z * k - X * s, Z * X * k - Y * s, Z * Y * k + X * s, c + Z * Z * k};
    memcpy(R, r, sizeof r);
}
// Envelope of shot i now (0 .. 1 at ~40 ms), 0 when over.
static float KickEnv(int i, double now) {
    const float w = 25.0f;
    float t = (float)(now - s_kickT[i]);
    return t > 0 && t < 0.4f ? 2.71828f * w * t * expf(-w * t) : 0;
}
// The kick now, as a world turn R (about the hand) and a slide bk, from the
// controller frame F (rows right, forward, up). false = nothing to do.
bool GunKickNow(const float F[9], float R[9], float bk[3]) {
    double now = KickClock();
    float x = 0, b = 0, rv[3] = {0, 0, 0};
    float tt = g_kick[2] * 0.0174533f;
    float ct = cosf(tt), st = sinf(tt);
    // Default axis: right x forward = up, so turning about (right cos t - up sin t)
    // sends the muzzle towards (up cos t + right sin t).
    float ax0[3];
    for (int i = 0; i < 3; ++i) ax0[i] = F[i] * ct - F[6 + i] * st;
    for (int i = 0; i < 4; ++i) {
        float e = KickEnv(i, now);
        if (e <= 0) continue;
        x += e, b += e * s_kickS[i][1];
        float a = e * s_kickS[i][0] * 0.0174533f;
        const float *v = s_kickRv[i];
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (g_kickFollow && s_kickCnt[i] && l / s_kickCnt[i] > 0.0005f)  // the bullets went off the beam: kick that way
            for (int k = 0; k < 3; ++k) rv[k] += a * v[k] / l;
        else
            for (int k = 0; k < 3; ++k) rv[k] += a * ax0[k];
    }
    if (x < 1e-3f || (rv[0] == 0 && rv[1] == 0 && rv[2] == 0 && b == 0)) return false;
    if (x > 1.6f)  // fast fire doesn't climb for ever
        for (int k = 0; k < 3; ++k) rv[k] *= 1.6f / x;
    if (x > 1.6f) b *= 1.6f / x;
    for (int i = 0; i < 3; ++i) bk[i] = -F[3 + i] * b;
    RotFromVec(rv, R);
    return true;
}
// aimdev1: the beam's turn now: exactly to each bullet's direction at the kick's
// peak, settling back with it. false = nothing (or switched off).
void GunKickRotate(const float R[9], const float v[3], float o[3]) {
    for (int i = 0; i < 3; ++i) o[i] = R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
}

// laser2 (DrGonzaga 2 Nov): the aim beam's look and the hip recoil are per gun
// too, in the same [gun.<name>] section. A gun without them takes the beam
// settings from [aim] (the ones in use before laser2) and the default recoil.
static void BeamIni(bool save, const wchar_t *sec, const wchar_t *path, const char *name) {
    static const wchar_t *const kKeys[11] = {L"BeamLength", L"BeamStart", L"BeamFadeIn", L"BeamWidth", L"BeamOpacity",
                                             L"BeamColour", L"HipKickUp", L"HipKickBack", L"HipKickTurn",
                                             L"HipKickUpClose", L"HipKickBackClose"};
    float col = (float)*LaserColour();
    float *vals[11] = {LaserLength(), LaserStart(), LaserFade(), LaserWidth(), LaserOpacity(), &col,
                       &g_kick[0], &g_kick[1], &g_kick[2], &g_kick[3], &g_kick[4]};
    bool bow = strstr(name, "BOW") || strstr(name, "Bow") || strstr(name, "bow");
    float kDef[11] = {40, 0.05f, 0, 1, 0.75f, 0, bow ? 1.0f : 4.0f, bow ? 0.01f : 0.025f, 0, -1, -1};
    wchar_t buf[32];
    for (int i = 0; i < 11; ++i) {
        if (!save && kDef[i] < 0) kDef[i] = *vals[i - 3] * 0.5f;  // kick2: close = half the gun's arm-out recoil
        if (save) {
            swprintf(buf, 32, L"%.3f", *vals[i]);
            WritePrivateProfileStringW(sec, kKeys[i], buf, path);
        } else if (GetPrivateProfileStringW(sec, kKeys[i], L"", buf, 32, path) ||
                   (i < 6 && GetPrivateProfileStringW(L"aim", kKeys[i], L"", buf, 32, path))) {
            *vals[i] = (float)_wtof(buf);
        } else {
            *vals[i] = kDef[i];
        }
    }
    int ci = (int)(col + 0.5f);
    *LaserColour() = ci < 0 || ci > 3 ? 0 : ci;
    // lasers1 (DrGonzaga 10 Nov): red dot only (no beam) per gun too; a gun without it takes [aim] DotOnly.
    if (save) WritePrivateProfileStringW(sec, wcscmp(sec, L"aim") ? L"BeamDotOnly" : L"DotOnly", *LaserDotOnly() ? L"1" : L"0", path);
    else if (GetPrivateProfileStringW(sec, L"BeamDotOnly", L"", buf, 32, path) ||
             GetPrivateProfileStringW(L"aim", L"DotOnly", L"", buf, 32, path))
        *LaserDotOnly() = _wtoi(buf) != 0;
    // lasers2 (DrGonzaga 10 Nov): laser off for this gun; mines start with it off.
    if (save) WritePrivateProfileStringW(sec, L"BeamOff", *LaserGunOff() ? L"1" : L"0", path);
    else {
        // lasers3 (DrGonzaga 10 Nov): knives / machete (melee, cutting plants) start with it off too.
        char l[96];
        int n = 0;
        for (; name[n] && n < 95; ++n) l[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
        l[n] = 0;
        bool off = strstr(l, "mine") || strstr(l, "knife") || strstr(l, "machete");
        // belt1 (DrGonzaga 11 Nov: a red laser while healing): things in the hand that aren't weapons (the
        // game's first-person props: syringe, bandage, scalpel, meat, zip-line hook) never have one.
        *LaserGunOff() = IsProp(name) || GetPrivateProfileIntW(sec, L"BeamOff", off ? 1 : 0, path) != 0;
    }
    // lasers3 (DrGonzaga 10 Nov): dot brightness per gun, apart from the beam opacity
    // (default = what the dot used before: full, or twice a faint beam's opacity).
    if (save) {
        swprintf(buf, 32, L"%.3f", *LaserDotBright());
        WritePrivateProfileStringW(sec, L"DotBrightness", buf, path);
    } else {
        float def = *LaserOpacity() > 0.5f ? 1.0f : *LaserOpacity() * 2;
        *LaserDotBright() = GetPrivateProfileStringW(sec, L"DotBrightness", L"", buf, 32, path) ? (float)_wtof(buf) : def;
    }
}

namespace {
// sight2 (DrGonzaga 10 Nov): the red dot sight window is saved per gun AND per optic,
// [sight.<gun>.<optic>]. The optic is the gun's own attached object (a child of the
// gun, like the silencer) whose name says sight / scope / reflex / holo / acog / optic;
// none found = "iron". Its names are logged once each time they change.
char g_sightKey[100];  // the gun.optic whose sight settings are loaded
void SightIni(bool save) {
    if (!g_turned[0]) return;
    wchar_t path[MAX_PATH], sec[128], buf[32];
    IniPath(path);
    // pergun1 (DrGonzaga 22 Nov: "keep all scope/laser/reticle settings on a per gun, but NOT per-attachment, basis"):
    // one section per gun, [sight.<gun>]. A gun with none yet starts from its old [sight.<gun>.<optic>] for the
    // optic it carries now (then the other optics it had saved), so tuned guns keep their settings.
    swprintf(sec, 128, L"sight.%hs", g_turned);
    if (!save && !GetPrivateProfileStringW(sec, L"SightDot", L"", buf, 32, path)) {
        wchar_t old[128];
        swprintf(old, 128, L"sight.%hs.%hs", g_turned, g_optic);
        if (GetPrivateProfileStringW(old, L"SightDot", L"", buf, 32, path)) wcscpy(sec, old);
        else {  // any optic of this gun that was saved before
            static wchar_t names[16384];
            DWORD got = GetPrivateProfileSectionNamesW(names, 16384, path);
            wchar_t pre[128];
            swprintf(pre, 128, L"sight.%hs.", g_turned);
            for (wchar_t *q = names; got && *q; q += wcslen(q) + 1)
                if (!_wcsnicmp(q, pre, wcslen(pre))) {
                    wcscpy(sec, q);
                    break;
                }
        }
    }
    // sight3: + roll, the sight's own brightness / softness / thickness, reticle size and style
    static const wchar_t *const kSight[12] = {L"SightRight", L"SightUp", L"SightForward", L"SightWidth", L"SightHeight", L"SightFade",
                                              L"SightRoll", L"SightBrightness", L"SightSoftness", L"SightThickness", L"SightSize",
                                              L"SightReticleTurn"};  // sight4
    const float kSightDef[12] = {0, 0.07f, 0.10f, 0.035f, 0.025f, 0.7f, 0, 1, 0.6f, *LaserSightMoa(), 1, 0};
    if (save) {
        WritePrivateProfileStringW(sec, L"SightDot", *LaserSightOn() ? L"1" : L"0", path);
        WritePrivateProfileStringW(sec, L"ShowBeam", *LaserShowBeam() ? L"1" : L"0", path);  // sight5
        WritePrivateProfileStringW(sec, L"ShowHitDot", *LaserShowDot() ? L"1" : L"0", path);
        WritePrivateProfileStringW(sec, L"BeamBehind", *LaserBeamBehind() ? L"1" : L"0", path);  // occl4
        WritePrivateProfileStringW(sec, L"DotBehind", *LaserDotBehind() ? L"1" : L"0", path);
        WritePrivateProfileStringW(sec, L"NoScope", *EngineScopeGunOff() ? L"1" : L"0", path);  // noscope1
        WritePrivateProfileStringW(sec, L"SightShape", *LaserSightShape() ? L"1" : L"0", path);
        swprintf(buf, 32, L"%d", *LaserSightReticle());
        WritePrivateProfileStringW(sec, L"SightReticle", buf, path);
        for (int i = 0; i < 12; ++i) {
            swprintf(buf, 32, L"%.4f", *LaserSightValue(i));
            WritePrivateProfileStringW(sec, kSight[i], buf, path);
        }
    } else {
        *LaserSightOn() = GetPrivateProfileIntW(sec, L"SightDot", 0, path) != 0;
        // sight5: beam / red dot where it lands; a gun and optic without them take the older per-gun ticks.
        const bool off = *LaserGunOff(), sight = *LaserSightOn();
        *LaserShowBeam() = GetPrivateProfileIntW(sec, L"ShowBeam", !off && !*LaserDotOnly() && !sight, path) != 0;
        *LaserShowDot() = GetPrivateProfileIntW(sec, L"ShowHitDot", !off && (*LaserDotOn() || *LaserDotOnly()) && !sight, path) != 0;
        *LaserBeamBehind() = GetPrivateProfileIntW(sec, L"BeamBehind", 1, path) != 0;  // occl4
        *LaserDotBehind() = GetPrivateProfileIntW(sec, L"DotBehind", 0, path) != 0;
        *EngineScopeGunOff() = GetPrivateProfileIntW(sec, L"NoScope", 0, path) != 0;  // noscope1
        *LaserSightShape() = GetPrivateProfileIntW(sec, L"SightShape", 0, path) ? 1 : 0;
        int rt = GetPrivateProfileIntW(sec, L"SightReticle", 0, path);
        *LaserSightReticle() = rt < 0 || rt > 9 ? 0 : rt;  // sight6: 5 bow pins
        if (IsProp(g_turned)) *LaserShowBeam() = *LaserShowDot() = *LaserSightOn() = false;  // belt1: syringe etc.
        for (int i = 0; i < 12; ++i)
            *LaserSightValue(i) = GetPrivateProfileStringW(sec, kSight[i], L"", buf, 32, path) ? (float)_wtof(buf) : kSightDef[i];
    }
    snprintf(g_sightKey, sizeof g_sightKey, "%s", g_turned);  // pergun1
}
// Game thread, from the gun's own attachment update, at most twice a second.
// g_singleShot: declared near the top (chargeglass3)


void ScanOptic(BYTE *comp) {
    int n = *(int *)(comp + 0x30);
    BYTE **kids = *(BYTE ***)(comp + 0x2c);
    if (n < 0 || n > 32 || IsBadReadPtr(kids, n * 4)) return;
    char found[48] = "iron", all[256] = "";
    for (int i = 0; i < n && i < kMaxKids; ++i) {
        if (!kids[i] || IsBadReadPtr(kids[i] + 0xc, 4)) continue;
        BYTE *ce = *(BYTE **)(kids[i] + 0xc);
        if (!ce || IsBadReadPtr(ce + 0x10, 4)) continue;
        char nm[48], l[48];
        GunName(ce, nm);
        int k = 0;
        for (; nm[k] && k < 47; ++k) l[k] = (char)(nm[k] >= 'A' && nm[k] <= 'Z' ? nm[k] + 32 : nm[k]);
        l[k] = 0;
        if (strlen(all) + strlen(nm) + 3 < sizeof all) strcat(all, all[0] ? ", " : ""), strcat(all, nm);
        if (!strcmp(found, "iron") && !strstr(l, "muzzle") && !strstr(l, "flash") && !strstr(l, "silenc") && !strstr(l, "suppress") &&
            (strstr(l, "sight") || strstr(l, "scope") || strstr(l, "reflex") || strstr(l, "holo") || strstr(l, "acog") ||
             strstr(l, "optic") || strstr(l, "reddot") || strstr(l, "red_dot"))) {
            // keep it short and ini-safe: the part after the last '/' or ':'
            const char *b = nm;
            for (const char *c = nm; *c; ++c)
                if (*c == '/' || *c == ':' || *c == '\\') b = c + 1;
            snprintf(found, sizeof found, "%s", b[0] ? b : nm);
        }
    }
    // chargeglass1 (DrGonzaga 24 Nov: on a new game the Kobracon "hasn't been upgraded yet ... after every bullet, it plays a
    // charging animation that moves the rifle"): the upgraded ones all carry DLC_BD_CALICO_FULLAUTO (logs 18-24 Nov).
    // chargeglass3 (bd158 log): the scoped rifle is DLC_BD_VIPER, and its upgrade kit DLC_BD_VIPER_RELOADKIT gives the magazine.
    g_singleShot = !strncmp(g_turned, "DLC_BD_VIPER", 12) && !strstr(all, "RELOADKIT");
    static char s_all[256];
    if (strcmp(all, s_all)) {
        strcpy(s_all, all);
        Log("sight2: gun %s carries: %s -> optic %s", g_turned, all[0] ? all : "(nothing named)", found);
    }
    if (strcmp(found, g_optic)) {
        snprintf(g_optic, sizeof g_optic, "%s", found);
        SightIni(false);
    }
}
// gunid2: position and angle of one weapon, [gun.<name>]. A weapon with none
// saved yet takes the current settings as its own.
// lefty3: in left-handed mode the left-hand keys LeftRight, LeftUp, LeftForward, LeftPitch, LeftYaw, LeftRoll
// are used instead. A gun with none yet starts from its right-hand values mirrored (Right, Yaw and Roll change
// sign) and saves them, then they are its own; the right-hand values are never changed by left-handed play.
static const wchar_t *const kTypeKeys[6] = {L"Right", L"Up", L"Forward", L"Pitch", L"Yaw", L"Roll"};
static const wchar_t *const kTypeLeftKeys[6] = {L"LeftRight", L"LeftUp", L"LeftForward", L"LeftPitch", L"LeftYaw", L"LeftRoll"};
static void MirrorPose(float *v[6]) { *v[0] = -*v[0], *v[4] = -*v[4], *v[5] = -*v[5]; }
static void TypeKeys(bool save, const wchar_t *sec, const wchar_t *path, const wchar_t *const keys[6], float *vals[6]) {
    wchar_t buf[32];
    for (int i = 0; i < 6; ++i) {
        if (save) {
            swprintf(buf, 32, L"%.4f", *vals[i]);  // fine2: 0.5 mm steps
            WritePrivateProfileStringW(sec, keys[i], buf, path);
        } else if (GetPrivateProfileStringW(sec, keys[i], L"", buf, 32, path)) {
            *vals[i] = (float)_wtof(buf);
        }
    }
}
void TypeIni(bool save, const char *name) {
    if (!name[0]) return;
    wchar_t path[MAX_PATH], sec[64], buf[32];
    IniPath(path);
    swprintf(sec, 64, L"gun.%hs", name);
    const bool lefty = *VrLeftHanded();
    g_loadedLefty = lefty;
    float *vals[6] = {&g_off[0], &g_off[1], &g_off[2], &g_ang[0], &g_ang[1], &g_ang[2]};
    if (!save) BeamIni(false, sec, path, name);  // laser2: each key falls back to [aim] / defaults
    const bool fresh = !save && !GetPrivateProfileStringW(sec, L"Right", L"", buf, 32, path);
    if (fresh) save = true;
    if (save) BeamIni(true, sec, path, name);
    const char *how = save ? "saved" : "loaded";
    if (!lefty) {
        TypeKeys(save, sec, path, kTypeKeys, vals);
    } else {
        if (fresh) {  // a new gun: the current (left-hand) values, mirrored, become its right-hand ones too
            MirrorPose(vals), TypeKeys(true, sec, path, kTypeKeys, vals), MirrorPose(vals);
        } else if (!save) {
            TypeKeys(false, sec, path, kTypeKeys, vals);
        }
        bool haveLeft = !fresh && GetPrivateProfileStringW(sec, L"LeftRight", L"", buf, 32, path);
        if (!save && !haveLeft) MirrorPose(vals), how = "made from the right hand (mirrored) and saved";
        TypeKeys(save || !haveLeft, sec, path, kTypeLeftKeys, vals);
        if (save || !haveLeft) how = save ? "saved (left hand)" : how;
        else how = "loaded (left hand)";
    }
    Log("gunattach: gun %s settings %s: offset %.2f %.2f %.2f, angle %.0f %.0f %.0f", name, how,
        g_off[0], g_off[1], g_off[2], g_ang[0], g_ang[1], g_ang[2]);
}
// lefty3: set this gun's left-hand place and angle back to its right-hand ones, mirrored (then saved).
void TypeLeftFromRight() {
    if (!g_turned[0] || !*VrLeftHanded()) return;
    wchar_t path[MAX_PATH], sec[64];
    IniPath(path);
    swprintf(sec, 64, L"gun.%hs", g_turned);
    float v[6] = {g_off[0], g_off[1], g_off[2], g_ang[0], g_ang[1], g_ang[2]};
    float *vals[6] = {&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]};
    TypeKeys(false, sec, path, kTypeKeys, vals);
    MirrorPose(vals);
    memcpy(g_off, v, 12), memcpy(g_ang, v + 3, 12);
    Log("gunattach: gun %s left hand reset from the right hand (mirrored): offset %.2f %.2f %.2f, angle %.0f %.0f %.0f",
        g_turned, g_off[0], g_off[1], g_off[2], g_ang[0], g_ang[1], g_ang[2]);
}
}  // namespace

// nolaser2 (DrGonzaga 17 Nov: "the laser pointer was present on the takedowns"): a takedown hands you the knife (TANTO)
// in the "cinematic" input mode, then 6ea34fd1; no beam for knives or while either mode is on.
bool GunAttachNoLaser() {
    if (g_bloodDragon) {
        DWORD im = EngineInputMode();
        if (im == 0x1eb7b814 || im == 0x6ea34fd1) return true;
        if (g_name[0] && (strstr(g_name, "TANTO") || strstr(g_name, "KNIFE") || strstr(g_name, "Knife") || strstr(g_name, "MACHETE"))) return true;
    }
    return g_name[0] && IsProp(g_name);
}  // nolaser1
const char *GunAttachName() { return g_turned; }
// noammo1 (DrGonzaga 20 Nov: the Killstar "runs on health, not 'ammo', so the ammo panel on the right wrist shows a blank
// grey square"): weapons with no ammo of their own have no ammo panel.
bool GunAttachNoAmmo() {
    return g_bloodDragon && g_turned[0] && (strstr(g_turned, "KillStar") || strstr(g_turned, "Killstar") || strstr(g_turned, "KILLSTAR"));
}
// laser2: save just the beam / hip recoil of the current gun (Aim page); before
// any gun is known, the beam goes to [aim] as before.
void GunAttachSaveBeam() {
    wchar_t path[MAX_PATH], sec[64];
    IniPath(path);
    if (g_turned[0]) swprintf(sec, 64, L"gun.%hs", g_turned);
    else wcscpy(sec, L"aim");
    BeamIni(true, sec, path, g_turned);
    SightIni(true);  // sight2
}
const char *GunAttachOptic() { return g_optic; }
void GunAttachAxesW(float out[9]) { memcpy(out, g_gunAxesW, 36); }  // lefty2
DWORD GunAttachHandTick() { return g_handTick; }
// c4hands1: C4 out on foot (detonator "IED_Remote" in the right hand): the aim hand is the left.
bool GunAttachC4Split() {
    return g_c4Split && g_on && g_hand && !strncmp(g_turned, "IED", 3) && GetTickCount() - g_handTick < 300 && EngineOnFoot();
}
bool *GunAttachC4SplitOpt() { return &g_c4Split; }
float *GunAttachC4Offset() { return g_c4Off; }
void GunAttachLoad() { Ini(false); }
void GunAttachSave() {
    Ini(true);
    TypeIni(true, g_turned);
    Log("gunattach: saved offset %.2f %.2f %.2f, angle %.0f %.0f %.0f, camera %.2f %.2f %.2f", g_off[0], g_off[1],
        g_off[2], g_ang[0], g_ang[1], g_ang[2], g_cam[0], g_cam[1], g_cam[2]);
}
float *GunAttachAngle() { return g_ang; }
void GunAttachLeftFromRight() { TypeLeftFromRight(); }  // lefty3
bool GunAttachLeftValues() { return g_loadedLefty; }
void GunAttachRelearn() { g_relearn = true; }
float *GunAttachCamera() { return g_cam; }

// car4: the player entity (the gun parent's own object), for vehicle detection.
BYTE *GunAttachPlayer() {
    BYTE *comp = g_curPlayer;  // veh7 (was slot 0)
    if (!comp || IsBadReadPtr(comp + 4, 4)) return nullptr;
    BYTE *c4 = *(BYTE **)(comp + 4);
    if (!c4 || IsBadReadPtr(c4 + 0xc, 4)) return nullptr;
    BYTE *ent = *(BYTE **)(c4 + 0xc);
    return ent && !IsBadReadPtr(ent, 0x70) ? ent : nullptr;
}
float *GunAttachOffset() { return g_off; }
void GunAttachName(BYTE *ent, char *out) { GunName(ent, out); }  // loot2
// scope12: the gun's pose as we placed it (rows right, forward, up, position), for the reticle tilt.
bool GunAttachPose(float m[16]) {
    if (!g_gunEnt || !g_on) return false;
    memcpy(m, g_gunPose, 64);
    return true;
}


bool GunAttachSingleShot() { return g_singleShot; }  // chargeglass1
float GunAttachGameGunDev() { return GetTickCount() - g_gameGunDevT < 250 ? g_gameGunDev : 0; }  // chargeglass2

// glue1 (DrGonzaga 24 Nov: "Do away with the timers altogether, and have the eyebox glued to the gun model somehow? through
// the animation?"): the bolt-action rifle's own bone table (the same table the bow's string uses: entries of 0x90 at the
// model part's +0x80, "matrix out" in model space at +0x50) is read every frame. Its pose at rest is noted while nothing is
// happening (no shot for 4 s, no reload press for 6 s, held 3 s); the rifle glass then moves with the bone nearest to it,
// by how far that bone is from its rest, so it stays on the eyepiece through the bolt and reload animations.
namespace {
BYTE *volatile g_glueBones;
volatile int g_glueN;
float g_glueRest[32][16];
volatile bool g_glueRestOk;
volatile int g_glueBone = -1;
bool g_glueOn = true;  // [scope] ChargeGlue
inline float D3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// q (model space) taken from rest bone R0 to the bone now R1 (rows = axes, [12..14] = position).
void GlueMove(const float *R0, const float *R1, const float *q, float *o, bool point) {
    float d[3], l[3];
    for (int k = 0; k < 3; ++k) d[k] = q[k] - (point ? R0[12 + k] : 0);
    for (int j = 0; j < 3; ++j) {
        float r2 = D3(R0 + 4 * j, R0 + 4 * j);
        l[j] = r2 > 1e-10f ? D3(d, R0 + 4 * j) / r2 : 0;
    }
    for (int k = 0; k < 3; ++k) o[k] = (point ? R1[12 + k] : 0) + l[0] * R1[k] + l[1] * R1[4 + k] + l[2] * R1[8 + k];
}
}  // namespace

void GunAttachGlueTick(BYTE *ent) {  // game thread, every placed frame
    static BYTE *s_ent;
    static DWORD s_chk, s_since, s_evT;
    static float s_maxT, s_maxA;
    static int s_dumps, s_logs;
    const DWORD now = GetTickCount();
    if (!g_singleShot || !g_bloodDragon || !ent) {
        g_glueBones = nullptr, g_glueRestOk = false, s_ent = nullptr;
        return;
    }
    if (ent != s_ent) s_ent = ent, s_since = now, g_glueRestOk = false, g_glueBones = nullptr, s_chk = 0;
    if (now - s_chk > 500) {
        s_chk = now;
        BYTE *comp = BowStringCompBd(ent), *b = nullptr;
        int n = 0;
        if (comp && !IsBadReadPtr(comp, 0x90)) {
            b = *(BYTE **)(comp + 0x80), n = *(int *)(comp + 0x84);
            if (!b || n < 1 || n > 32 || IsBadReadPtr(b, n * 0x90)) b = nullptr, n = 0;
        }
        if (b != g_glueBones || n != g_glueN) {
            g_glueRestOk = false, g_glueBones = b, g_glueN = n;
            if (s_dumps < 3) {
                ++s_dumps;
                if (!b) Log("scope glue: no bone table on the rifle (%s): the glass uses the timers", g_turned);
                else {
                    Log("scope glue: rifle %s has %d bones:", g_turned, n);
                    for (int i = 0; i < n; ++i) {
                        const float *m = (const float *)(b + i * 0x90 + 0x50);
                        Log("scope glue:   bone %d hash %08lx parent %d at (%.3f %.3f %.3f)", i, *(DWORD *)(b + i * 0x90), *(int *)(b + i * 0x90 + 4), m[12], m[13], m[14]);
                    }
                }
            }
        }
    }
    BYTE *b = g_glueBones;
    const int n = g_glueN;
    if (!b) return;
    const DWORD st = InputLastShotT(), xt = InputPadXTime();
    const bool idle = now - s_since > 3000 && (!st || now - st > 4000) && (!xt || now - xt > 6000);
    if (idle) {
        for (int i = 0; i < n; ++i) memcpy(g_glueRest[i], b + i * 0x90 + 0x50, 64);
        if (!g_glueRestOk) Log("scope glue: %s at rest noted; the glass follows its animation from now", g_turned);
        g_glueRestOk = true;
    }
    // One line per shot / reload (10 in all): how far the bone under the glass moved, to see the glue has something to follow.
    const DWORD ev = st > xt ? st : xt;
    const int bi = g_glueBone;
    if (g_glueRestOk && bi >= 0 && bi < n && ev && now - ev < 4000) {
        const float *R0 = g_glueRest[bi], *R1 = (const float *)(b + bi * 0x90 + 0x50);
        float dt[3] = {R1[12] - R0[12], R1[13] - R0[13], R1[14] - R0[14]}, tr = 0;
        for (int j = 0; j < 3; ++j) tr += D3(R0 + 4 * j, R1 + 4 * j) / sqrtf(fmaxf(D3(R0 + 4 * j, R0 + 4 * j) * D3(R1 + 4 * j, R1 + 4 * j), 1e-12f));
        float c = (tr - 1) * 0.5f;
        float a = acosf(c > 1 ? 1 : c < -1 ? -1 : c) * 57.29578f, t = sqrtf(D3(dt, dt));
        s_evT = ev, s_maxT = fmaxf(s_maxT, t), s_maxA = fmaxf(s_maxA, a);
    } else if (s_evT && now - s_evT >= 4000) {
        if (s_logs < 10) ++s_logs, Log("scope glue: after the %s the rifle part under the glass (bone %d) moved up to %.1f cm and %.1f deg", s_evT == st ? "shot" : "reload press", bi, s_maxT * 100, s_maxA);
        s_evT = 0, s_maxT = s_maxA = 0;
    }
}

// Render thread: the rifle glass's middle and axes (world) moved with the rifle's animation. False = nothing to follow.
bool GunAttachGlue(float C[3], float ax[3][3]) {
    if (!g_glueOn || !g_singleShot || !g_glueRestOk) return false;
    BYTE *b = g_glueBones;
    const int n = g_glueN;
    if (!b || n < 1 || IsBadReadPtr(b, n * 0x90)) return false;
    float M[16], l2[3];
    memcpy(M, g_gunDrawn, 64);
    for (int j = 0; j < 3; ++j)
        if ((l2[j] = D3(M + 4 * j, M + 4 * j)) < 1e-10f) return false;
    float rel[3] = {C[0] - M[12], C[1] - M[13], C[2] - M[14]}, p[3];
    for (int j = 0; j < 3; ++j) p[j] = D3(rel, M + 4 * j) / l2[j];
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < n; ++i) {
        const float *r = g_glueRest[i] + 12;
        float d[3] = {r[0] - p[0], r[1] - p[1], r[2] - p[2]}, d2 = D3(d, d);
        if (d2 < bd) bd = d2, best = i;
    }
    if (best < 0) return false;
    g_glueBone = best;
    const float *R0 = g_glueRest[best], *R1 = (const float *)(b + best * 0x90 + 0x50);
    float p2[3];
    GlueMove(R0, R1, p, p2, true);
    for (int k = 0; k < 3; ++k) C[k] = M[12 + k] + p2[0] * M[k] + p2[1] * M[4 + k] + p2[2] * M[8 + k];
    for (int a = 0; a < 3; ++a) {
        float dm[3], dm2[3];
        for (int j = 0; j < 3; ++j) dm[j] = D3(ax[a], M + 4 * j) / l2[j];
        GlueMove(R0, R1, dm, dm2, false);
        float w[3], l = 0;
        for (int k = 0; k < 3; ++k) w[k] = dm2[0] * M[k] + dm2[1] * M[4 + k] + dm2[2] * M[8 + k], l += w[k] * w[k];
        if (l < 1e-12f) return false;
        l = 1 / sqrtf(l);
        for (int k = 0; k < 3; ++k) ax[a][k] = w[k] * l;
    }
    return true;
}
bool *GunAttachGlueOn() { return &g_glueOn; }
int GunAttachGlueState() { return !g_singleShot ? 0 : !g_glueBones ? 1 : !g_glueRestOk ? 2 : 3; }  // glue1: menu status
