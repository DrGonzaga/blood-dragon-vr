// holster (DrGonzaga 10 Nov): weapons live on the body. Put the right hand on a spot
// and press the right grip once: the weapon in hand changes to that spot's weapon.
// Spots match the game's weapon wheel: right hip = wheel right, left hip = wheel left,
// behind the right shoulder = wheel up, behind the left shoulder = wheel down (the
// shoulder zones are big: anywhere behind the head on that side). Each spot's slot can
// be changed on the Gun & body page. A short buzz when the hand reaches a spot, small
// see-through spheres on the hips, and on the chest four spheres with icons (grenade,
// C4, molotov, mine): grip one to pick it and hold the game's throw, let go to throw.
// While the hand is on a spot the game's look stops following the controller, so
// reaching back doesn't swing the view.
//
// The Blood Dragon addresses are at Ready() (slot select, "switch"), NadeInstallBd (throw gadget,
// throw-by-name) and HolsterGameTick (the throwable walk).
// Game calls run on the game thread (engine.cpp, once a frame, with the CPawn the
// vehicle check found). Spots are measured in the recentred XR space against a body
// facing that follows the head's yaw (40 degree dead band, not while looking down).
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "fc3vr.h"

// fc3vr.ini [holster]; saving runs on the menu's save worker (Later), never the game.
static void SaveF(const wchar_t *sec, const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(sec, key, buf, path);
}
static float LoadF(const wchar_t *sec, const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32], d[32];
    IniPath(path);
    swprintf(d, 32, L"%.3f", def);
    GetPrivateProfileStringW(sec, key, d, buf, 32, path);
    return (float)_wtof(buf);
}

namespace {
const DWORD kHashMine = 0xd2dec8ba;  // CRC32 of the weapon type name "proximitymine" (bd29: same table as FC3)

// Spots: 0 right hip, 1 left hip, 2 behind the right shoulder, 3 behind the left
// shoulder, 4-7 chest (grenade, C4, molotov, mine; left to right: grenade, C4, mine, molotov,
// lasers1: DrGonzaga 10 Nov wanted the molotov on the far right).
bool g_on = true, g_buzz = true, g_spheres = true, g_icons = true;
int g_slot[4] = {1, 3, 0, 2};  // game slot (0-3) per weapon spot; wheel guess: up 1, right 2, down 3, left 4
float g_hipDrop = 0.70f, g_hipSide = 0.20f, g_hipFwd = 0.00f, g_radius = 0.16f;
// belt1 (DrGonzaga 11 Nov: "icons the same size, grab area bigger, away from the grenades, like a triangle
// going out/up/down"): the hip grab area also reaches this much further out to the side (away from the
// chest row), widening up/down/front/back by up to the same amount at its outer end. 0 = the plain sphere.
float g_hipWide = 0.10f;  // [holster] HipGrabWider (m)
float g_shBack = 0.00f, g_shDrop = 0.45f;  // shoulder zone: further back than this behind the eyes, no lower than this below them
float g_chestDrop = 0.36f, g_chestFwd = 0.17f;
float g_opacity = 0.45f, g_hipSize = 0.05f, g_chestSize = 0.035f;  // holster4: sphere opacity, hip / chest sphere radius (m)
bool g_neon = false;                                // neon1: [holster] NeonMarks (default on in Blood Dragon)
bool g_handsOn = false;                             // hands1: [holster] HandSpheres
float g_handSize = 0.03f, g_handOpacity = 0.35f;    // radius (m), opacity
// wrist1m (DrGonzaga 13 Nov): throwables menu on the LEFT wrist: four icon circles fixed to the left
// controller (grenade, molotov, C4, mine), seen only on a wrist-check glance (as the ammo panel).
// Right hand on one + right grip: grenade / molotov = the type X throws (it stays larger), C4 / mine
// come out into the hands. The chest row is an option now (off by default).
bool g_wrist = true, g_wristRow = false, g_wristCount = true;
static float g_wCountSize = 0.33f;
static float g_wCountCol[3] = {0.1f, 1.0f, 0.82f};  // numcol1: the numbers' colour (aqua, DrGonzaga's picture)
static float g_wCountBg[4] = {0, 0, 0, 0.9f}, g_wCountOff[3] = {0.15f, 0.15f, 0.16f};  // numcol2: panel rgb + opacity, unlit segments
static int g_wCountStyle = 1;  // numstyle1: 0 plain, 1 digital, 2 neon tube, 3 dot matrix, 4 green CRT  // smallnum1 (DrGonzaga 16 Nov): the counts about a third of their old size
float g_wPos[3] = {0.04f, -0.18f, 0.09f};  // from the left controller: right, up, back (m)
float g_wTilt = 0, g_wTurn = 90, g_wGlance = 40, g_wSize = 0.022f, g_wGap = 0.055f, g_wOpacity = 0.9f;
// palmup1 (DrGonzaga 22 Nov: "only turns on with a certain amount of rotation of the wrist? It's appearing when I hold
// the bow ... present when my palm is up"): the wrist menu also needs the palm turned up, within this many degrees
// of straight up ([holster] WristPalmUpAngle; 180 = any way, as before).
float g_wPalmUp = 50;
volatile LONG g_wristSeen;                 // input thread: the menu faces you and you look at it
volatile LONG g_curKind = -1;              // the game's current throwable: 0 grenade, 2 molotov (-1 unknown)
const int kWristKind[4] = {0, 2, 1, 3};    // wrist icon -> chest kind (grenade, C4, molotov, mine)
// heart1 (DrGonzaga 15 Nov, Blood Dragon: "add this icon ... to grab then throw a cyberheart ... grab the
// cyberheart with the right grip, aim, and throw by releasing right grip, or place it back"): a fifth
// wrist icon (spot 15, icon 9 = his picture). Right grip on it: the heart is "in the hand" (its icon
// rides on the right hand; the game has no held heart). Let go away from the menu: the game's own
// cyber heart throw is pressed ([holster] HeartButton, D-pad left by default), along the swing if it
// was a throw. Let go on the wrist menu: put back, nothing pressed.
bool g_heart = true;
int g_heartBtn = 0;                        // 0 D-pad left, 1 right, 2 up, 3 down
bool g_heartHeld;                          // input thread
volatile LONG g_heartPress;                // the game's throw is pressed until this tick (0 none)
// heart2 (DrGonzaga 15 Nov: "we ... were able to make it the grenade model in hand. Can we do this?"): the heart
// is one of the game's throwables ("Lurer" in its catalogue, beside Frag_Grenade, Molotov and Rock), so it
// can be taken out as the grenade is: picked as the game's throwable (request 24) with the game's throw
// held, which puts the game's own model in the hand; letting go throws. The throwable picked before
// (grenade / molotov) is picked again 1.5 s later so the X quick throw is unchanged. There is no put back
// in this mode (Blood Dragon's throw abort is not found yet). [holster] HeartModel, default on.
bool g_heartModel = true;
volatile LONG g_heartHave = -1;            // hearts owned (-1 not read yet)
LONG g_heartPrev = -1;                     // input thread: the throwable to pick again
DWORD g_heartRestoreT;
float IconGap() { return g_chestSize * 2.2f > 0.075f ? g_chestSize * 2.2f : 0.075f; }
float IconGrab() { return g_chestSize * 1.4f > 0.04f ? g_chestSize * 1.4f : 0.04f; }

int g_state;  // 0 not checked, 1 ready, -1 off
volatile LONG g_req = -1;     // game thread work: 0..3 weapon spot, 20..23 chest throwable
volatile LONG g_spot = -1;    // spot the right hand is on now (-1 none)
volatile LONG g_buzzReq;
volatile DWORD g_onSpotT;     // last tick the hand was on a spot (look steering pauses)
volatile LONG g_have[4] = {-1, -1, -1, -1};  // throwables owned per chest kind (-1 not read yet)
volatile LONG g_throwHold;    // the game's throw is held until this tick (0 none)
DWORD g_throwPressT;
bool g_throwing;
// nade1 (DrGonzaga 11 Nov): hold, arm, throw
bool g_holdToArm = true, g_armTrig = true, g_armTeeth = true, g_physThrow = true;
float g_mouthReach = 0.11f, g_throwSpeed = 1.6f, g_throwTurn = 0.20f;
int g_nade;            // input thread: 0 none, 1 the game's grenade in the hand unarmed (grip held), 2 armed, 3 unarmed, grip let go
bool g_nadeTrigWas;
bool g_nadeArmGrip, g_nadeArmTrig;  // nade5: the grip / trigger was held when arming (which let go throws)
volatile DWORD g_nadeEndT;          // last throw / put back (swing melee and trigger stay off a moment)
volatile LONG g_throwAimUntil;      // physical throw: the game's look turned along g_throwDir until this tick
volatile float g_throwDir[3];
volatile LONG g_nadeNone;           // game thread: the pick found none left
// nade7: the grenade hold-and-arm (grip takes it out, trigger or teeth arm it) was Far Cry 3's; its cook
// hook is not ported. The gadget fire / activate hooks below are shared with the cyber heart (heart3).
typedef void(__attribute__((thiscall)) * GadgetFn1)(void *, void *);
GadgetFn1 g_gFire, g_gActivate, g_gAbort;
void *volatile g_gStrat, *volatile g_gUser;  // the grenade gadget and its user, from its activate
volatile LONG g_abortReq;           // input thread: put the grenade away (game thread calls the abort)
volatile LONG g_abortUntil;         // game's fire skipped until this tick
float g_nearest = 9;
float g_bodyYaw;
bool g_bowSlotRead = true;  // spot5: [holster] BowSlotRead (read the bow's wheel slot from the game)
bool g_bodyYawSet;
char g_last[96];

volatile DWORD g_gStratT;  // heart3: when the throw gadget was last taken out
void __attribute__((thiscall)) GadgetActivate(void *self, void *user) {
    g_gStrat = self, g_gUser = user, g_gStratT = GetTickCount();
    g_gActivate(self, user);
}
void __attribute__((thiscall)) GadgetFire(void *self, void *user) {
    LONG u = g_abortUntil;
    if (u && (LONG)((DWORD)u - GetTickCount()) > 0 && self == g_gStrat) return;  // being put away: no throw
    g_gFire(self, user);
}

// bd15: Blood Dragon. The same slot select, compiled as a plain thiscall: BD+10520550 (inventory, slot),
// ret 4, returns al; inventory = *(CPawn + 0x24) + 0xbd4 (BD+1041ede3); then BD+11243485 (stack: CPawn,
// ret 4) posts "switch". Found from the inventory event handler at BD+112448d8. Throwables, C4 / mines,
// the grenade hold-and-arm and the quiver are not ported yet: weapon spots only.
BYTE *g_bd;
// heart3 (DrGonzaga 15 Nov: "Can you work on a putback fix"): Blood Dragon's throw gadget is the same class as
// FC3's (vtable BD+11596828, found through the function matcher and read: slot 11 fire BD+105e6045, 14
// activate BD+105e5b49, 15 deactivate BD+105e5d7c, 16 abort BD+105e4218, which sets +0x6b like FC3's; all
// thiscall (this, user), ret 4). The same two slots are hooked, so a held throwable can be put away.
const DWORD kGadgetVtBd = 0x1596828;
// heart4 (DrGonzaga 15 Nov: "When I grab a cyberheart, it grabs either grenade or molotov depending upon what was
// selected"): picking "Lurer" by name did set the game's current throwable (inventory + 0xe4), but the game keeps
// that to its first two throwables (BD+1051dc11) and put the grenade / molotov back before the throw. Now the
// game's "use the current throwable" (BD+105239ec, thiscall (inventory, bool), ret 4; three callers) is caught
// at its callers: while a heart is wanted, + 0xe4 is the heart's name hash for that one call only, so the
// held throw takes the heart out and the grenade / molotov choice is never touched.
volatile LONG g_heartUse;                  // input thread: the next throw takes the heart (until this tick)
volatile DWORD g_heartHash = 0xefd99836;   // crc32 of "gadgets:fc3/lurer", re-read from the game's own list
volatile LONG g_heartTook;                 // 1 the game took the heart, -1 it did not
bool g_heartAdjust;                        // heartadj1: menu: hold a heart in the hand for adjusting
volatile bool g_heartAdjOn;
volatile DWORD g_heartAdjEndT;
typedef DWORD(__attribute__((thiscall)) * UseThrowFn)(BYTE *, DWORD);
UseThrowFn g_useThrow;
DWORD __attribute__((thiscall)) UseThrowHook(BYTE *inv, DWORD flag) {
    LONG u = g_heartUse;
    if (!u || (LONG)((DWORD)u - GetTickCount()) <= 0 || IsBadWritePtr(inv + 0xdc, 12)) return g_useThrow(inv, flag);
    InterlockedExchange(&g_heartUse, 0);
    DWORD was = *(DWORD *)(inv + 0xe4), h = g_heartHash;
    *(DWORD *)(inv + 0xe4) = h;
    DWORD r = g_useThrow(inv, flag);
    bool took = *(DWORD *)(inv + 0xdc) == h;
    *(DWORD *)(inv + 0xe4) = was;
    InterlockedExchange(&g_heartTook, took ? 1 : -1);
    Log("holster: cyber heart: the game's throw %s (throwable hash %08lx, the game's own choice %08lx kept)",
        took ? "took the heart out" : "did NOT take the heart", h, was);
    return r;
}
void NadeInstallBd() {
    DWORD *vt = (DWORD *)(g_bd + kGadgetVtBd);
    static const struct { int slot; DWORD at; const char *b; } want[3] = {
        {11, 0x5e6045, "\x55\x8b\xec\x56\x8b\xf1\x8b\x4d\x08\xe8"},
        {14, 0x5e5b49, "\x55\x8b\xec\x83\xec\x18\x53\x56\x8b\x75"},
        {16, 0x5e4218, "\x55\x8b\xec\x56\x51\x8b\xf1\x8b\x0d\x50"}};
    if (IsBadReadPtr(vt, 20 * 4)) return;
    for (auto &w : want)
        if (vt[w.slot] != (DWORD)(g_bd + w.at) || memcmp(g_bd + w.at, w.b, 10)) {
            Log("holster: Blood Dragon's throw gadget doesn't match (slot %d); a held cyber heart can't be put back", w.slot);
            return;
        }
    GadgetFn1 fire = (GadgetFn1)vt[11], act = (GadgetFn1)vt[14], abortFn = (GadgetFn1)vt[16];
    g_gFire = fire, g_gActivate = act;
    void *hf = (void *)GadgetFire, *ha = (void *)GadgetActivate;
    if (!PatchBytes(&vt[11], &hf, 4) || !PatchBytes(&vt[14], &ha, 4)) return;
    g_gAbort = abortFn;
    Log("holster: Blood Dragon's throw gadget hooked (a held cyber heart can be put back)");
    {  // heart4
        static const DWORD site[3] = {0x4ef62f, 0xb11441, 0x1244b37};
        BYTE *fn = g_bd + 0x5239ec;
        bool ok = !memcmp(fn, "\x55\x8b\xec\x83\xec\x0c\x56\x8b\xf1\x8b\x46\x4c", 12);
        for (DWORD a : site) ok = ok && g_bd[a] == 0xe8 && g_bd + a + 5 + *(int *)(g_bd + a + 1) == fn;
        if (ok) {
            g_useThrow = (UseThrowFn)fn;
            for (DWORD a : site) ok = PatchCallTo(g_bd + a, (void *)UseThrowHook) && ok;
        }
        Log("holster: Blood Dragon's throw %s", ok ? "caught (the wrist menu heart is thrown as the game's own heart)" : "NOT matched: the wrist menu heart uses its button instead");
        if (!ok) g_useThrow = nullptr;
    }
}
bool Ready() {
    if (g_state) return g_state > 0;
    g_state = -1;
    BYTE *bd = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    static const BYTE kSel[] = {0x55, 0x8b, 0xec, 0x51, 0x53, 0x56, 0x8b, 0xf1, 0x80, 0xbe, 0xab, 0x00};
    static const BYTE kPostB[] = {0x55, 0x8b, 0xec, 0xf6, 0x05};
    bool ok = BdStage() >= 7 && bd && !memcmp(bd + 0x520550, kSel, sizeof kSel) && !memcmp(bd + 0x1243485, kPostB, sizeof kPostB);
    if (ok) g_bd = bd, g_state = 1, NadeInstallBd();
    for (int k = 0; k < 4; ++k) InterlockedExchange(&g_have[k], 0);
    Log("holster: Blood Dragon's weapon select %s", ok ? "found" : "NOT found: holsters off");
    return ok;
}

typedef BYTE *(__stdcall *CatFn)(DWORD);
typedef int(__attribute__((thiscall)) * CountFn)(BYTE *);
typedef DWORD *(__attribute__((thiscall)) * IdFn)(BYTE *, DWORD *, int);


// wrist1m: the game's own name hash (FUN_11798f80, eax = string; CRC32 of the lower-case name).
DWORD NameHash(const char *n) {
    static int s_ok;
    // bd28: Blood Dragon's is BD+1004f01e (thiscall: ecx = &out, name, 1, 0; ret 0xc), as its select-by-name
    // BD+1051dd91 makes the hash it keeps at inventory + 0xe4 (BD+1051de84: mov ecx,[esi+0xe4]).
    if (!s_ok)
        s_ok = g_bd && !IsBadReadPtr(g_bd + 0x4f01e, 12) && !memcmp(g_bd + 0x4f01e, "\x55\x8b\xec\x8b\x45\x08\x56\x8b\xf1\x85\xc0\x74", 12) &&
               !memcmp(g_bd + 0x51de84, "\x8b\x8e\xe4\x00\x00\x00", 6) ? 1 : -1;
    if (s_ok < 0 || !n[0]) return 0;
    DWORD out = 0;
    BdThisCall(g_bd + 0x4f01e, &out, 3, n, 1, 0);
    return out;
}

int Kind(const char *n) {
    char l[96];
    int i = 0;
    for (; n[i] && i < 95; ++i) l[i] = (char)(n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i]);
    l[i] = 0;
    if (strstr(l, "molotov")) return 2;
    if (strstr(l, "mine")) return 3;
    if (strstr(l, "c4") || strstr(l, "ied") || strstr(l, "remote") || strstr(l, "explosive")) return 1;
    if (strstr(l, "grenade") || strstr(l, "frag")) return 0;
    return -1;
}



const char *SpotName(int s) {
    static const char *n[] = {"right hip", "left hip", "right shoulder", "left shoulder",
                              "chest: grenade", "chest: C4", "chest: molotov", "chest: mine",
                              "quiver", "quiver", "quiver",  // 8-10: the FC3 quiver, gone (numbers kept)
                              "wrist: grenade", "wrist: molotov", "wrist: C4", "wrist: mine",
                              "wrist: cyber heart", "wrist: slow motion clock"};  // slowmo1: 16
    return s >= 0 && s < 17 ? n[s] : "none";
}

// The body frame and the spot centres (recentred XR space, metres; shoulders use the zone test).
struct Spots {
    float head[3], f[3], r[3], c[8][3];
};
bool GetSpots(Spots &o, bool track) {
    float hm[16];
    if (!EngineHeadPose(hm, o.head)) return false;
    if (track) {
        float fx = -hm[2], fz = -hm[10], fy = -hm[6];
        float yaw = atan2f(fx, -fz);  // 0 = facing -z
        if (!g_bodyYawSet) g_bodyYaw = yaw, g_bodyYawSet = true;
        float d = yaw - g_bodyYaw;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        // holster5 (DrGonzaga 10 Nov): past 40 deg the body is pushed round; inside it, once the
        // head has been still for 0.4 s (holster6, was 0.8), the body glides back under the head (time
        // constant 0.5 s, about 1.5 s to settle), so the chest spheres and the panels on them
        // re-centre. Not while the hand is on a spot or the head looks well down (reaching).
        // "Stopped" = no head turn faster than ~20 deg/s for 0.4 s (tracking jitter is far below).
        static DWORD s_t, s_moveT;
        static float s_yaw;
        DWORD now = GetTickCount();
        float dt = s_t ? (now - s_t) * 0.001f : 0;
        s_t = now;
        if (dt > 0.1f) dt = 0.1f;
        float dy = yaw - s_yaw;
        while (dy > 3.14159265f) dy -= 6.2831853f;
        while (dy < -3.14159265f) dy += 6.2831853f;
        if (fabsf(dy) > 0.35f * (dt > 0.005f ? dt : 0.005f) + 0.002f) s_moveT = now;
        s_yaw = yaw;
        const float band = 0.70f;  // 40 deg
        if (d > band) g_bodyYaw += d - band;
        else if (d < -band) g_bodyYaw += d + band;
        else if (g_spot < 0 && fy > -0.70f && now - s_moveT > 400) g_bodyYaw += d * (1 - expf(-dt / 0.5f));
    }
    float y = g_bodyYaw;
    o.f[0] = sinf(y), o.f[1] = 0, o.f[2] = -cosf(y);
    o.r[0] = -o.f[2], o.r[1] = 0, o.r[2] = o.f[0];
    struct { float side, up, fwd; } sp[8] = {
        {g_hipSide, -g_hipDrop, g_hipFwd}, {-g_hipSide, -g_hipDrop, g_hipFwd}, {0.2f, -0.15f, -0.15f}, {-0.2f, -0.15f, -0.15f},
        {-1.5f * IconGap(), -g_chestDrop, g_chestFwd}, {-0.5f * IconGap(), -g_chestDrop, g_chestFwd},
        {1.5f * IconGap(), -g_chestDrop, g_chestFwd}, {0.5f * IconGap(), -g_chestDrop, g_chestFwd}};  // lasers1: molotov far right
    for (int s = 0; s < 8; ++s) {
        for (int i = 0; i < 3; ++i) o.c[s][i] = o.head[i] + o.r[i] * sp[s].side + o.f[i] * sp[s].fwd;
        o.c[s][1] += sp[s].up;
    }
    return true;
}

// belt1: is the hand in hip spot s's (0 right, 1 left) grab area? The sphere of old, plus a cone that
// opens outwards from it (see g_hipWide). *wide = only in the added part.
bool HipHit(const Spots &sp, int s, const float hand[3], float *dist, bool *wide) {
    float d[3] = {hand[0] - sp.c[s][0], hand[1] - sp.c[s][1], hand[2] - sp.c[s][2]};
    float dd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    *dist = dd, *wide = false;
    if (dd <= g_radius) return true;
    if (g_hipWide < 0.005f) return false;
    float out = (d[0] * sp.r[0] + d[2] * sp.r[2]) * (s == 0 ? 1.0f : -1.0f), reach = g_radius + g_hipWide;
    if (out <= 0 || out > reach) return false;
    float fwd = d[0] * sp.f[0] + d[2] * sp.f[2];
    if (sqrtf(d[1] * d[1] + fwd * fwd) > g_radius + g_hipWide * out / reach) return false;
    return *wide = true;
}

// wrist1m: the wrist menu in the recentred XR space: icon centres, and whether it is seen (it faces
// the head and the head looks at it, as hud.cpp's wrist panels; 'shown' widens the angle by 10 deg).
bool WristFrame(float c[6][3], bool *seen, bool shown) {  // slowmo1: 6 (5 the clock)
    float hm[16], head[3], m[16], hp[3];
    if (!g_wrist || !EngineHeadPose(hm, head) || !HeadtrackHandRaw(0, m, hp)) return false;
    const float d2r = 0.01745329f;
    float ca = cosf(g_wTurn * d2r), sa = sinf(g_wTurn * d2r), ct = cosf(g_wTilt * d2r), st = sinf(g_wTilt * d2r);
    float ctr[3], r[3], u[3], n[3];
    for (int i = 0; i < 3; ++i) {
        float R = m[i * 4 + 0], U = m[i * 4 + 1], B = m[i * 4 + 2], n0 = B * ca + R * sa;
        ctr[i] = hp[i] + R * g_wPos[0] + U * g_wPos[1] + B * g_wPos[2];
        r[i] = R * ca - B * sa, n[i] = n0 * ct + U * st, u[i] = U * ct - n0 * st;
    }
    for (int k = 0; k < 4; ++k) {
        float x = g_wristRow ? (k - 1.5f) * g_wGap : ((k & 1) ? 0.5f : -0.5f) * g_wGap;
        float y = g_wristRow ? 0 : ((k & 2) ? -0.5f : 0.5f) * g_wGap;
        for (int i = 0; i < 3; ++i) c[k][i] = ctr[i] + r[i] * x + u[i] * y;
    }
    {  // heart1: the fifth icon, at the end of the row or under the two by two
        float x = g_wristRow ? 2.5f * g_wGap : 0, y = g_wristRow ? 0 : -1.5f * g_wGap;
        for (int i = 0; i < 3; ++i) c[4][i] = ctr[i] + r[i] * x + u[i] * y;
    }
    {  // slowmo1: the clock, the sixth icon: after the heart in the row, above the two by two
        float x = g_wristRow ? 3.5f * g_wGap : 0, y = g_wristRow ? 0 : 1.5f * g_wGap;
        for (int i = 0; i < 3; ++i) c[5][i] = ctr[i] + r[i] * x + u[i] * y;
    }
    if (seen) {
        float th[3] = {head[0] - ctr[0], head[1] - ctr[1], head[2] - ctr[2]};
        float l = sqrtf(th[0] * th[0] + th[1] * th[1] + th[2] * th[2]);
        if (l < 1e-4f) return false;
        th[0] /= l, th[1] /= l, th[2] /= l;
        float lim = cosf((g_wGlance + (shown ? 10.f : 0.f)) * d2r);
        float facing = n[0] * th[0] + n[1] * th[1] + n[2] * th[2];
        float gaze = hm[2] * th[0] + hm[6] * th[1] + hm[10] * th[2];
        // palmup1: the palm's way is the controller's +x for the left hand (OpenXR grip: out of the left palm); in
        // left-handed play this hand is the right controller, whose palm is -x. Space y is up.
        float palm = (*VrLeftHanded() ? -1.f : 1.f) * m[4];
        float plim = cosf((g_wPalmUp + (shown ? 10.f : 0.f)) * d2r);
        *seen = facing > lim && gaze > lim && (g_wPalmUp >= 179.f || palm > plim);
    }
    return true;
}

// Which spot the right hand is on (-1 none). Input thread (it alone turns the body facing).
// *wide (belt1) = on a hip spot's added outer part only (the look is not held still for that).
int FindSpot(float *nearest, bool *wide) {
    Spots sp;
    float m[16], hand[3];
    *nearest = 9, *wide = false;
    float wc[6][3];  // wrist1m: the wrist menu (seen or not is kept for the draw, whatever the right hand does)
    bool wseen = false;
    if (!WristFrame(wc, &wseen, g_wristSeen != 0)) wseen = false;
    InterlockedExchange(&g_wristSeen, wseen ? 1 : 0);
    if (!HeadtrackHandRaw(1, m, hand) || !GetSpots(sp, true)) return -1;  // bow1p: the real right hand (bow mode moves the aim hand)
    float d[3] = {hand[0] - sp.head[0], hand[1] - sp.head[1], hand[2] - sp.head[2]};
    float back = -(d[0] * sp.f[0] + d[2] * sp.f[2]), side = d[0] * sp.r[0] + d[2] * sp.r[2];
    if (back > g_shBack && d[1] > -g_shDrop) return side >= 0 ? 2 : 3;  // big shoulder zones: behind the head
    int best = -1;
    float bd = 1e9f;
    bool bestWide = false;
    for (int s = 0; s < 8; ++s) {  // quiver13 (DrGonzaga 11 Nov): no quiver spheres to grab (8-10); the stick picks the type
        if (s == 2 || s == 3 || (s >= 4 && s < 8 && !g_icons)) continue;
        float dx = hand[0] - sp.c[s][0], dy = hand[1] - sp.c[s][1], dz = hand[2] - sp.c[s][2];
        float dd = sqrtf(dx * dx + dy * dy + dz * dz);
        bool w = false;
        if (s < 2) {
            if (dd < *nearest) *nearest = dd;
            if (!HipHit(sp, s, hand, &dd, &w)) continue;
        } else if (dd > IconGrab()) continue;
        if (s >= 4) dd *= 0.5f;  // the small chest spots win where they overlap the hips
        if (dd < bd) bd = dd, best = s, bestWide = w;
    }
    // wrist1m: the wrist menu's icons (spots 11-14), only while it is seen; they win over the rest
    {
        if (wseen) {
            float grab = g_wSize * 1.5f > 0.03f ? g_wSize * 1.5f : 0.03f, wb = grab;
            for (int k = 0; k < 6; ++k) {  // heart1: 15 = the cyber heart; slowmo1: 16 = the clock
                if ((k == 4 && !(g_bloodDragon && g_heart)) || (k == 5 && !SlowmoOnWrist())) continue;
                float dx = hand[0] - wc[k][0], dy = hand[1] - wc[k][1], dz = hand[2] - wc[k][2];
                float dd = sqrtf(dx * dx + dy * dy + dz * dz);
                if (dd < wb) wb = dd, best = 11 + k, bestWide = false;
            }
        }
    }
    *wide = bestWide;
    return best;
}

// nade1: the right hand at the mouth (pin out with the teeth). Mouth = a little below and in
// front of the eyes, turned with the head.
bool AtMouth() {
    float hm[16], head[3], m[16], hand[3];
    if (!EngineHeadPose(hm, head) || !HeadtrackHandRaw(1, m, hand)) return false;
    float mouth[3];
    for (int i = 0; i < 3; ++i) mouth[i] = head[i] - hm[i * 4 + 2] * 0.07f - hm[i * 4 + 1] * 0.09f;
    float dx = hand[0] - mouth[0], dy = hand[1] - mouth[1], dz = hand[2] - mouth[2];
    return dx * dx + dy * dy + dz * dz < g_mouthReach * g_mouthReach;
}
// nade1: the right hand's recent path (input thread), for the physical throw.
float g_path[16][3];
LONGLONG g_pathT[16];
int g_pathN;
void GrenadeTrack() {
    if (g_nade != 2 && !g_heartHeld) { g_pathN = 0; return; }  // heart1: the held heart is thrown the same way
    float m[16], h[3];
    if (!HeadtrackHandRaw(1, m, h)) return;
    int last = (g_pathN + 15) & 15;
    if (g_pathN && !memcmp(g_path[last], h, 12)) return;  // same pose as last time
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    memcpy(g_path[g_pathN & 15], h, 12), g_pathT[g_pathN & 15] = t.QuadPart, ++g_pathN;
}
// Fastest stretch of the swing in the last 150 ms (each over 30-60 ms): its direction and speed.
bool SwingDir(float dir[3], float *speed) {
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
    const LONGLONG ms = fq.QuadPart / 1000;
    float best = 0;
    int n = g_pathN < 16 ? g_pathN : 16;
    for (int a = 0; a < n; ++a) {
        int ia = (g_pathN - 1 - a) & 15;
        if (now.QuadPart - g_pathT[ia] > 150 * ms) break;
        for (int b = a + 1; b < n; ++b) {
            int ib = (g_pathN - 1 - b) & 15;
            LONGLONG dt = g_pathT[ia] - g_pathT[ib];
            if (dt < 30 * ms) continue;
            if (dt > 60 * ms) break;
            float d[3] = {g_path[ia][0] - g_path[ib][0], g_path[ia][1] - g_path[ib][1], g_path[ia][2] - g_path[ib][2]};
            float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float v = len / (float(dt) / fq.QuadPart);
            if (v > best && len > 1e-4f) best = v, dir[0] = d[0] / len, dir[1] = d[1] / len, dir[2] = d[2] / len;
            break;
        }
    }
    *speed = best;
    if (best <= 0) return false;
    // keep it between 30 deg down and 55 deg up (a lob, not into your feet)
    float hz = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    if (hz < 1e-3f) return false;
    float pitch = atan2f(dir[1], hz);
    if (pitch < -0.52f) pitch = -0.52f;
    if (pitch > 0.96f) pitch = 0.96f;
    float c = cosf(pitch) / hz;
    dir[0] *= c, dir[2] *= c, dir[1] = sinf(pitch);
    return true;
}
}  // namespace

// bow1p (DrGonzaga 10 Nov): the left hand takes the bow from a weapon spot (0..3), -1 none.
int HolsterLeftSpot() {
    Spots sp;
    float m[16], hand[3];
    if (!HolsterActive() || !HeadtrackHandRaw(0, m, hand) || !GetSpots(sp, false)) return -1;
    float d[3] = {hand[0] - sp.head[0], hand[1] - sp.head[1], hand[2] - sp.head[2]};
    float back = -(d[0] * sp.f[0] + d[2] * sp.f[2]), side = d[0] * sp.r[0] + d[2] * sp.r[2];
    if (back > g_shBack && d[1] > -g_shDrop) return side >= 0 ? 2 : 3;
    int best = -1;
    float bd = 1e9f;
    for (int s = 0; s < 2; ++s) {
        float dd;
        bool w;
        if (HipHit(sp, s, hand, &dd, &w) && dd < bd) bd = dd, best = s;  // belt1: the same wider hip areas
    }
    return best;
}
void HolsterTakeSpot(int spot) {  // bow1p: switch to that spot's weapon (game thread does it)
    if (spot >= 0 && spot < 4 && HolsterActive()) InterlockedExchange(&g_req, spot);
}
bool *HolsterOn() { return &g_on; }
const char *HolsterSpotName(int s) { return SpotName(s); }  // slowmo1: menu.cpp
bool *HolsterBuzzOn() { return &g_buzz; }
bool *HolsterSpheres() { return &g_spheres; }
bool *HolsterIcons() { return &g_icons; }
bool *HolsterHands() { return &g_handsOn; }
bool *HolsterNeon() { return &g_neon; }
float *HolsterWristCountSize() { return &g_wCountSize; }  // smallnum1
int *HolsterWristCountStyle() { return &g_wCountStyle; }  // numstyle1
float *HolsterWristCountColour() { return g_wCountCol; }  // numcol1
float *HolsterWristCountBg() { return g_wCountBg; }    // numcol2
float *HolsterWristCountOff() { return g_wCountOff; }  // numcol2
bool *HolsterWristOpt(int i) { bool *p[3] = {&g_wrist, &g_wristRow, &g_wristCount}; return i >= 0 && i < 3 ? p[i] : nullptr; }  // wrist1m
float *HolsterWristValue(int i) {
    float *p[10] = {&g_wPos[0], &g_wPos[1], &g_wPos[2], &g_wTilt, &g_wTurn, &g_wGlance, &g_wSize, &g_wGap, &g_wOpacity, &g_wPalmUp};
    return i >= 0 && i < 10 ? p[i] : nullptr;
}
static const wchar_t *kWristKeys[10] = {L"WristRight", L"WristUp", L"WristBack", L"WristTilt", L"WristTurn", L"WristGlanceAngle",
                                       L"WristIconSize", L"WristSpacing", L"WristOpacity", L"WristPalmUpAngle"};
float *HolsterHandValue(int i) { return i ? &g_handOpacity : &g_handSize; }
int *HolsterSlot(int spot) { return spot >= 0 && spot < 4 ? &g_slot[spot] : nullptr; }
float *HolsterValue(int i) {
    float *p[12] = {&g_hipDrop, &g_hipSide, &g_hipFwd, &g_radius, &g_shBack, &g_shDrop, &g_chestDrop, &g_chestFwd,
                    &g_opacity, &g_hipSize, &g_chestSize, &g_hipWide};
    return i >= 0 && i < 12 ? p[i] : nullptr;
}
static const wchar_t *kKeys[12] = {L"HipDrop", L"HipSide", L"HipForward", L"HipRadius", L"ShoulderBehind", L"ShoulderLowest",
                                   L"ChestDrop", L"ChestForward", L"SphereOpacity", L"HipSphereSize", L"ChestSphereSize",
                                   L"HipGrabWider"};
static const wchar_t *kSlotKeys[4] = {L"RightHipSlot", L"LeftHipSlot", L"RightShoulderSlot", L"LeftShoulderSlot"};
void HolsterLoad() {
    SlowmoLoad();  // slowmo1: [slowmo]
    g_on = LoadF(L"holster", L"On", 1) != 0;
    g_buzz = LoadF(L"holster", L"Buzz", 1) != 0;
    g_spheres = LoadF(L"holster", L"Spheres", 1) != 0;
    // wrist1m: the chest row is off by default now (new key, so the old saved "on" doesn't bring it back)
    g_icons = LoadF(L"holster", L"ChestThrowables", 0) != 0;
    g_wrist = LoadF(L"holster", L"WristMenu", 1) != 0, g_wristRow = LoadF(L"holster", L"WristRow", 0) != 0;
    g_wristCount = LoadF(L"holster", L"WristCounts", 1) != 0;
    g_wCountSize = LoadF(L"holster", L"WristCountSize", 0.33f);
    g_wCountStyle = (int)LoadF(L"holster", L"WristCountStyle", 1);
    g_wCountStyle = g_wCountStyle < 0 ? 0 : g_wCountStyle > 4 ? 4 : g_wCountStyle;
    {
        static const wchar_t *ck[3] = {L"WristCountR", L"WristCountG", L"WristCountB"};
        static const float cd[3] = {0.1f, 1.0f, 0.82f};
        for (int i = 0; i < 3; ++i) { float x = LoadF(L"holster", ck[i], cd[i]); g_wCountCol[i] = x < 0 ? 0 : x > 1 ? 1 : x; }
        static const wchar_t *bk[4] = {L"WristCountBgR", L"WristCountBgG", L"WristCountBgB", L"WristCountBgOpacity"};
        static const wchar_t *ok[3] = {L"WristCountUnlitR", L"WristCountUnlitG", L"WristCountUnlitB"};
        static const float bd[4] = {0, 0, 0, 0.9f}, od[3] = {0.15f, 0.15f, 0.16f};
        for (int i = 0; i < 4; ++i) { float x = LoadF(L"holster", bk[i], bd[i]); g_wCountBg[i] = x < 0 ? 0 : x > 1 ? 1 : x; }
        for (int i = 0; i < 3; ++i) { float x = LoadF(L"holster", ok[i], od[i]); g_wCountOff[i] = x < 0 ? 0 : x > 1 ? 1 : x; }
    }
    g_wCountSize = g_wCountSize < 0.15f ? 0.15f : g_wCountSize > 1.5f ? 1.5f : g_wCountSize;
    g_heart = LoadF(L"holster", L"WristHeart", 1) != 0, g_heartBtn = (int)LoadF(L"holster", L"HeartButton", 0) & 3;  // heart1
    g_heartModel = LoadF(L"holster", L"HeartModel", 1) != 0;  // heart2
    for (int i = 0; i < 10; ++i) *HolsterWristValue(i) = LoadF(L"holster", kWristKeys[i], *HolsterWristValue(i));
    for (int i = 0; i < 12; ++i) *HolsterValue(i) = LoadF(L"holster", kKeys[i], *HolsterValue(i));
    g_holdToArm = LoadF(L"holster", L"GrenadeHoldToArm", 1) != 0, g_armTrig = LoadF(L"holster", L"GrenadeArmTrigger", 1) != 0;  // nade1
    g_armTeeth = LoadF(L"holster", L"GrenadeArmTeeth", 1) != 0, g_physThrow = LoadF(L"holster", L"GrenadePhysicalThrow", 1) != 0;
    g_mouthReach = LoadF(L"holster", L"GrenadeMouthReach", g_mouthReach), g_throwSpeed = LoadF(L"holster", L"GrenadeThrowSpeed", g_throwSpeed);
    g_throwTurn = LoadF(L"holster", L"GrenadeThrowTurn", g_throwTurn);
    g_handsOn = LoadF(L"holster", L"HandSpheres", 0) != 0;  // hands1
    g_neon = LoadF(L"holster", L"NeonMarks", g_bloodDragon ? 1.f : 0.f) != 0;  // neon1
    g_handSize = LoadF(L"holster", L"HandSphereSize", g_handSize), g_handOpacity = LoadF(L"holster", L"HandSphereOpacity", g_handOpacity);
    g_bowSlotRead = LoadF(L"holster", L"BowSlotRead", 1) != 0;  // spot5: ini only; 0 = bow spot from the grip only
    for (int i = 0; i < 4; ++i) {
        int v = (int)LoadF(L"holster", kSlotKeys[i], (float)(g_slot[i] + 1)) - 1;
        g_slot[i] = v < 0 ? 0 : v > 3 ? 3 : v;
    }
}
void HolsterSave() {
    SlowmoSave();  // slowmo1
    SaveF(L"holster", L"On", g_on ? 1.f : 0.f);
    SaveF(L"holster", L"Buzz", g_buzz ? 1.f : 0.f);
    SaveF(L"holster", L"Spheres", g_spheres ? 1.f : 0.f);
    SaveF(L"holster", L"ChestThrowables", g_icons ? 1.f : 0.f);
    SaveF(L"holster", L"WristMenu", g_wrist ? 1.f : 0.f), SaveF(L"holster", L"WristRow", g_wristRow ? 1.f : 0.f);
    SaveF(L"holster", L"WristCounts", g_wristCount ? 1.f : 0.f);
    SaveF(L"holster", L"WristCountSize", g_wCountSize);
    SaveF(L"holster", L"WristCountStyle", (float)g_wCountStyle);
    SaveF(L"holster", L"WristCountR", g_wCountCol[0]), SaveF(L"holster", L"WristCountG", g_wCountCol[1]), SaveF(L"holster", L"WristCountB", g_wCountCol[2]);
    SaveF(L"holster", L"WristCountBgR", g_wCountBg[0]), SaveF(L"holster", L"WristCountBgG", g_wCountBg[1]), SaveF(L"holster", L"WristCountBgB", g_wCountBg[2]);
    SaveF(L"holster", L"WristCountBgOpacity", g_wCountBg[3]);
    SaveF(L"holster", L"WristCountUnlitR", g_wCountOff[0]), SaveF(L"holster", L"WristCountUnlitG", g_wCountOff[1]), SaveF(L"holster", L"WristCountUnlitB", g_wCountOff[2]);
    SaveF(L"holster", L"WristHeart", g_heart ? 1.f : 0.f), SaveF(L"holster", L"HeartButton", (float)g_heartBtn);  // heart1
    SaveF(L"holster", L"HeartModel", g_heartModel ? 1.f : 0.f);  // heart2
    for (int i = 0; i < 10; ++i) SaveF(L"holster", kWristKeys[i], *HolsterWristValue(i));
    for (int i = 0; i < 12; ++i) SaveF(L"holster", kKeys[i], *HolsterValue(i));
    SaveF(L"holster", L"GrenadeHoldToArm", g_holdToArm ? 1.f : 0.f), SaveF(L"holster", L"GrenadeArmTrigger", g_armTrig ? 1.f : 0.f);  // nade1
    SaveF(L"holster", L"GrenadeArmTeeth", g_armTeeth ? 1.f : 0.f), SaveF(L"holster", L"GrenadePhysicalThrow", g_physThrow ? 1.f : 0.f);
    SaveF(L"holster", L"GrenadeMouthReach", g_mouthReach), SaveF(L"holster", L"GrenadeThrowSpeed", g_throwSpeed);
    SaveF(L"holster", L"GrenadeThrowTurn", g_throwTurn);
    SaveF(L"holster", L"HandSpheres", g_handsOn ? 1.f : 0.f);  // hands1
    SaveF(L"holster", L"NeonMarks", g_neon ? 1.f : 0.f);  // neon1
    SaveF(L"holster", L"HandSphereSize", g_handSize), SaveF(L"holster", L"HandSphereOpacity", g_handOpacity);
    for (int i = 0; i < 4; ++i) SaveF(L"holster", kSlotKeys[i], (float)(g_slot[i] + 1));
}

// On foot and playing: the right grip is the holsters', not the game's.
bool HolsterActive() {
    return g_on && g_state >= 0 && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() && !MenuOpen() && !GamePaused() && !CyberEyeOn();
}
// engine.cpp look steering: hold the look still while the hand is on a spot (and just after).
// quiver8 (DrGonzaga 11 Nov: arrows scattered a few degrees after the first shots): drawing the bow
// brings the right hand past the quiver / shoulder spots, which froze the game's look (the arrow's
// direction) on an old aim; never while an arrow is drawn or just loosed.
bool HolsterAimPaused() { return HolsterActive() && GetTickCount() - g_onSpotT < 250 && !BowArrowsAlongAim(); }

int HolsterSpot(float *nearest) {
    if (nearest) *nearest = g_nearest;
    return g_spot;
}
const char *HolsterLast() { return g_last; }

// Input thread (input.cpp Mix), each pad read.
void HolsterInput(const VrPad &p) {
    SlowmoTick();  // slowmo1: eases the game's time scale, ends it by time / cutscene / pause
    static bool s_grip;
    float nearD;
    bool active = HolsterActive();
    bool wide = false;
    int spot = active ? FindSpot(&nearD, &wide) : -1;
    if (active) g_nearest = nearD;
    LONG was = InterlockedExchange(&g_spot, spot);
    DWORD now = GetTickCount();
    if (spot >= 0 && !wide) g_onSpotT = now;  // belt1: the added outer part of a hip area doesn't hold the look
    if (spot >= 0 && spot != was && g_buzz) InterlockedExchange(&g_buzzReq, 1);
    bool grip = p.rgrip > (s_grip ? 0.35f : 0.6f);  // a little hysteresis
    bool press = grip && !s_grip, release = !grip && s_grip;
    s_grip = grip;
    if (InterlockedExchange(&g_nadeNone, 0) && (g_nade || g_heartHeld)) g_nade = 0, g_throwing = false;  // nade1: none to hold (heart2: or no heart)
    GrenadeTrack();  // nade1: hand path for the physical throw
    {  // heartadj1 (DrGonzaga 15 Nov: "an option in debug menu for me to position hand/cyberheart pose (maybe have it
       // appear in hand when I adjust, like the bow/arrow)"): with the menu open and the box ticked, a heart is
       // taken out and held (the game's throw held, as from the wrist menu) so the Guns page sliders ("Lurer")
       // can be set while looking at it; unticking or closing the menu puts it back (never thrown).
        static bool s_adj;
        bool want = g_heartAdjust && MenuOpen() && g_bloodDragon && g_useThrow && g_on && g_state >= 0 && EngineOnFoot() && !EngineInVehicle() &&
                    !EngineCutscene() && !GamePaused() && g_heartHave != 0;
        g_heartAdjOn = want;
        if (want && !s_adj) {
            s_adj = true;
            InterlockedExchange(&g_heartTook, 0);
            InterlockedExchange(&g_heartUse, (LONG)(now + 1500));
            g_throwPressT = now;
            InterlockedExchange(&g_throwHold, (LONG)(now + 3600000));
            Log("holster: cyber heart held for adjusting (menu)");
        } else if (!want && s_adj) {
            s_adj = false, g_heartAdjust = false, g_heartAdjEndT = now;
            InterlockedExchange(&g_abortReq, 1);
            InterlockedExchange(&g_throwHold, (LONG)(now + 500));
            g_nadeEndT = now;
            Log("holster: cyber heart put back after adjusting");
        }
        if (s_adj) {
            InterlockedExchange(&g_wristSeen, 0);
            return;
        }
    }
    if (!active) {
        if (g_throwing) g_throwing = false, InterlockedExchange(&g_throwHold, 0);
        g_nade = 0, g_heartHeld = false;
        // wrist1m: with the debug menu open the wrist menu still shows (to set it up), nothing to grab
        float wc[6][3];
        bool seen = false;
        if (!(MenuOpen() && g_on && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() && !GamePaused() &&
              WristFrame(wc, &seen, g_wristSeen != 0)))
            seen = false;
        InterlockedExchange(&g_wristSeen, seen ? 1 : 0);
        return;
    }
    if (g_heartHeld && g_heartModel) {  // heart2: the game holds it; letting go throws
        if (!g_throwing) { g_heartHeld = false; g_heartRestoreT = now + 300; return; }  // none owned
        if (!release) return;
        g_heartHeld = false, g_throwing = false, g_nadeEndT = now;
        if (spot >= 11 && g_gAbort) {  // heart3: let go on the wrist menu: put back
            InterlockedExchange(&g_abortReq, 1);
            InterlockedExchange(&g_throwHold, (LONG)(now + 500));  // the throw is let go 0.5 s later, its fire skipped
            g_heartRestoreT = now + 2000;
            InterlockedExchange(&g_buzzReq, 1);
            snprintf(g_last, sizeof g_last, "cyber heart put back");
            return;
        }
        DWORD end = g_throwPressT + 400;
        float dir[3], sp = 0;
        if (g_physThrow && SwingDir(dir, &sp) && sp >= g_throwSpeed) {
            DWORD e2 = now + (DWORD)(g_throwTurn * 1000);
            if ((LONG)(e2 - end) > 0) end = e2;
            memcpy((void *)g_throwDir, dir, 12);
            InterlockedExchange(&g_throwAimUntil, (LONG)(end + 80));
        }
        InterlockedExchange(&g_throwHold, (LONG)((LONG)(end - now) > 0 ? end : now));
        g_heartRestoreT = end + 1500;
        Log("holster: cyber heart thrown (the game's own)");
        return;
    }
    if (g_heartHeld) {  // heart1: in the hand until the grip is let go
        if (!release) return;
        g_heartHeld = false, g_nadeEndT = now;
        if (spot >= 11) {
            snprintf(g_last, sizeof g_last, "cyber heart put back");
            Log("holster: cyber heart put back on the wrist menu");
            InterlockedExchange(&g_buzzReq, 1);
            return;
        }
        DWORD end = now + 250;
        float dir[3], sp = 0;
        bool swing = g_physThrow && SwingDir(dir, &sp) && sp >= g_throwSpeed;
        if (swing) {
            DWORD e2 = now + (DWORD)(g_throwTurn * 1000) + 150;
            if ((LONG)(e2 - end) > 0) end = e2;
            memcpy((void *)g_throwDir, dir, 12);
            InterlockedExchange(&g_throwAimUntil, (LONG)(end + 80));
        }
        InterlockedExchange(&g_heartPress, (LONG)end);
        snprintf(g_last, sizeof g_last, "cyber heart thrown");
        Log("holster: cyber heart thrown (%s)", swing ? "along the swing" : "where the hand points");
        return;
    }
    if (g_heartRestoreT && !g_throwing && (LONG)(now - g_heartRestoreT) >= 0) {  // heart2: back to the grenade / molotov
        g_heartRestoreT = 0;
        if ((g_heartPrev == 0 || g_heartPrev == 2) && g_req < 0) InterlockedExchange(&g_req, 40 + g_heartPrev);
    }
    if (press && spot == 16) {  // slowmo1: the clock: slow motion on / off
        SlowmoToggle();
        InterlockedExchange(&g_buzzReq, 1);
        snprintf(g_last, sizeof g_last, "slow motion %s", SlowmoActive() ? "on" : "off");
        return;
    }
    if (press && spot == 15) {
        g_heartHeld = true;
        InterlockedExchange(&g_buzzReq, 1);
        if (g_heartModel) {  // heart2: the game's own heart, as its grenade
            g_heartPrev = -1, g_heartRestoreT = 0;  // heart4: the grenade / molotov choice is not touched any more
            InterlockedExchange(&g_heartTook, 0);
            InterlockedExchange(&g_heartUse, (LONG)(now + 1500));
            InterlockedExchange(&g_req, 24);
            g_throwing = true, g_throwPressT = now;
            InterlockedExchange(&g_throwHold, (LONG)(now + 600000));
        }
        Log("holster: cyber heart taken from the wrist menu (%s)", g_heartModel ? "the game's own, its throw held" : "icon in the hand");
        return;
    }
    // nade7: 1 = the game's grenade in the hand, not armed (its throw held, the cook kept back); stays
    // (3) after the grip is let go. Right trigger (or the hand at the mouth: pin out with the teeth)
    // = 2: the kept-back cook is made (the fuse starts); letting go of the grip throws, or, if the grip
    // wasn't held when arming, letting go of the trigger. A holster weapon (or C4 / mine) puts the
    // unarmed grenade away (the game's abort). Physical throw: let go mid-swing, it flies along it.
    if (g_nade == 1 || g_nade == 3) {
        bool trig = p.rt > 0.6f;
        bool teeth = g_armTeeth && AtMouth();
        if (release && g_nade == 1) g_nade = 3;  // let go unarmed: stays in the hand
        if ((g_armTrig && trig && !g_nadeTrigWas) || teeth) {
            g_nade = 2, g_nadeArmGrip = grip, g_nadeArmTrig = trig;
            InterlockedExchange(&g_buzzReq, 1);
            LogV("holster: grenade armed (%s)", teeth ? "teeth" : "trigger");
        }
        g_nadeTrigWas = trig;
    } else if (g_nade == 2) {
        bool throwNow = g_nadeArmGrip ? release : (g_nadeArmTrig && p.rt < 0.3f);
        if (!g_nadeArmGrip && press) g_nadeArmGrip = true, g_nadeArmTrig = false;  // gripped after arming: grip throws
        if (throwNow) {
            g_nade = 0, g_throwing = false;
            DWORD end = g_throwPressT + 400;  // held at least 0.4 s in all, so a quick arm still throws
            float dir[3], sp = 0;
            if (g_physThrow && SwingDir(dir, &sp) && sp >= g_throwSpeed) {
                DWORD e2 = now + (DWORD)(g_throwTurn * 1000);
                if ((LONG)(e2 - end) > 0) end = e2;
                memcpy((void *)g_throwDir, dir, 12);
                InterlockedExchange(&g_throwAimUntil, (LONG)(end + 80));
                LogV("holster: physical throw %.1f m/s, dir %.2f %.2f %.2f", sp, dir[0], dir[1], dir[2]);
            }
            InterlockedExchange(&g_throwHold, (LONG)((LONG)(end - now) > 0 ? end : now));
            g_nadeEndT = now;
        }
    } else if (g_throwing && g_nade == 0 && release) {  // old way (hold to arm off): let go throws
        g_throwing = false;
        DWORD end = g_throwPressT + 400;
        InterlockedExchange(&g_throwHold, (LONG)((LONG)(end - now) > 0 ? end : now));
        g_nadeEndT = now;
    }
    if (g_nade == 2 && press) return;  // nade4: armed: the next let go throws it; no weapon swap meanwhile
    if ((g_nade == 1 || g_nade == 3) && press && spot >= 0 && spot <= 3 && !(BowActive() && spot == BowQuiverSpot())) {
        // nade7: a holster weapon: the game's abort first, then the weapon switch below; the throw is
        // let go 0.5 s later, its fire skipped (abort window 1.5 s)
        g_nade = 0, g_throwing = false, g_nadeEndT = now;
        InterlockedExchange(&g_abortReq, 1);
        InterlockedExchange(&g_throwHold, (LONG)(now + 500));
        snprintf(g_last, sizeof g_last, "grenade put away");
    }
    if (g_nade == 3 && press) {  // gripped again (not at a weapon spot)
        if (spot != 5 && spot != 7) { g_nade = 1, g_nadeTrigWas = p.rt > 0.6f; return; }
    }
    if (!press || spot < 0) return;
    if (spot >= 11) {  // wrist1m: the wrist menu: select the X-button throwable, or take C4 / mines out
        if (g_nade == 1 || g_nade == 3) {  // a chest grenade in the hand, unarmed: put away first
            g_nade = 0, g_throwing = false, g_nadeEndT = now;
            InterlockedExchange(&g_abortReq, 1);
            InterlockedExchange(&g_throwHold, (LONG)(now + 500));
        }
        InterlockedExchange(&g_req, 40 + kWristKind[spot - 11]);
        InterlockedExchange(&g_buzzReq, 1);
        return;
    }
    if ((g_nade == 1 || g_nade == 3) && (spot == 5 || spot == 7)) {  // nade7: C4 / mine: put away first
        g_nade = 0, g_throwing = false, g_nadeEndT = now;
        InterlockedExchange(&g_abortReq, 1);
        InterlockedExchange(&g_throwHold, (LONG)(now + 500));
    }
    if (spot == 5 || spot == 7) {  // chest C4 / mine: into the hand like a weapon (trigger uses it)
        InterlockedExchange(&g_req, 20 + (spot - 4));
    } else if (spot >= 4 && spot < 8) {  // chest: pick that throwable
        InterlockedExchange(&g_req, 20 + (spot - 4));
        g_throwing = true, g_throwPressT = now;
        // nade6 (DrGonzaga 11 Nov: "the molotov can be held indefinitely anyway, we're just working on
        // grenades"): the molotov is the game's own as before (held while gripped, let go throws)
        if (g_holdToArm && spot == 4 && !g_bloodDragon) g_nade = 1, g_nadeTrigWas = p.rt > 0.6f;  // nade7: in the hand, not armed
        InterlockedExchange(&g_throwHold, (LONG)(now + 600000));  // the game's throw held (it takes the grenade out)
    } else if (BowActive() && spot == BowQuiverSpot()) {
        // bow1p: with the two-handed bow out, the bow's own spot is the quiver (bow.cpp takes the arrow)
        // spot1 (DrGonzaga 11 Nov): only there; the other hip / shoulder spots swap weapon as usual
    } else {
        InterlockedExchange(&g_req, spot);
    }
}

// input.cpp: press the game's throw while true (from 150 ms after the pick, once the game has switched).
bool HolsterThrowing() { return g_throwing && HolsterActive(); }  // lasers1: laser.cpp throw stub
bool HolsterThrowHeld() {
    LONG until = g_throwHold;
    if (!until) return false;
    DWORD now = GetTickCount();
    if ((LONG)(now - (DWORD)until) >= 0) {
        InterlockedCompareExchange(&g_throwHold, 0, until);
        return false;
    }
    return now - g_throwPressT >= 150;
}

// nade1: input.cpp: the right trigger is ours (arms the grenade) while one is in the hand, and
// stays off until it's let go after the throw, so the gun doesn't fire.
bool HolsterTriggerOwned(float rt) {
    static bool s_hold;
    if (g_nade || g_heartHeld) s_hold = true;  // heart1
    else if (rt < 0.2f) s_hold = false;
    return s_hold;
}
// input.cpp: no swing melee while a grenade is in the hand, or just thrown (the throw is a swing).
bool HolsterSwingBlocked() { return g_nade || g_heartHeld || GetTickCount() - g_nadeEndT < 700; }
// heart1: input.cpp: the pad button of the game's cyber heart throw while it is pressed for a throw (0 = none).
WORD HolsterHeartButton() {
    LONG u = g_heartPress;
    if (!u) return 0;
    if ((LONG)(GetTickCount() - (DWORD)u) >= 0) {
        InterlockedCompareExchange(&g_heartPress, 0, u);
        return 0;
    }
    static const WORD kBtn[4] = {0x0004, 0x0008, 0x0001, 0x0002};  // XInput D-pad left, right, up, down
    return kBtn[g_heartBtn & 3];
}
bool *HolsterHeartOn() { return &g_heart; }
bool *HolsterHeartModel() { return &g_heartModel; }
bool *HolsterHeartAdjust() { return &g_heartAdjust; }
// input.cpp: the game still gets the throw button with the menu open (and for the put-back just after)
bool HolsterHeartAdjHold() { return g_heartAdjOn || (g_heartAdjEndT && GetTickCount() - g_heartAdjEndT < 600); }
int *HolsterHeartBtn() { return &g_heartBtn; }
// engine.cpp look steering: during a physical throw, the look goes along the swing (XR space).
bool HolsterThrowAim(float m[16]) {
    LONG u = g_throwAimUntil;
    if (!u || (LONG)((DWORD)u - GetTickCount()) <= 0) return false;
    float f[3] = {g_throwDir[0], g_throwDir[1], g_throwDir[2]}, r[3], up[3];
    r[0] = -f[2], r[1] = 0, r[2] = f[0];  // f x (0,1,0)
    float l = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (l < 1e-3f) return false;
    r[0] /= l, r[2] /= l;
    up[0] = r[1] * f[2] - r[2] * f[1], up[1] = r[2] * f[0] - r[0] * f[2], up[2] = r[0] * f[1] - r[1] * f[0];
    memset(m, 0, 64);
    for (int i = 0; i < 3; ++i) m[i * 4 + 0] = r[i], m[i * 4 + 1] = up[i], m[i * 4 + 2] = -f[i];
    m[15] = 1;
    return true;
}
bool HolsterThrowAimActive() {
    LONG u = g_throwAimUntil;
    return u && (LONG)((DWORD)u - GetTickCount()) > 0;
}
int HolsterGrenadeState() { return g_nade; }

bool *HolsterNadeOpt(int i) {
    bool *p[4] = {&g_holdToArm, &g_armTrig, &g_armTeeth, &g_physThrow};
    return i >= 0 && i < 4 ? p[i] : nullptr;
}
float *HolsterNadeValue(int i) {
    float *p[3] = {&g_mouthReach, &g_throwSpeed, &g_throwTurn};
    return i >= 0 && i < 3 ? p[i] : nullptr;
}

// xr.cpp haptics: a short buzz for the right hand (0..1), polled each frame.
float HolsterBuzz() {
    static DWORD s_until;
    DWORD t = GetTickCount();
    if (InterlockedExchange(&g_buzzReq, 0)) s_until = t + 45;
    return (LONG)(s_until - t) > 0 ? 0.55f : 0.0f;
}

// spot1 / bowspot2: which holster spot holds the bow = the spot a grip took it from (Blood Dragon; Far
// Cry 3's reader of the game's weapon slots is not ported).
static volatile LONG g_bowSpot = -1;  // the bow's spot 0-3 (-1 not known yet)
static int s_selSpot = -1;            // the spot whose weapon a grip last took out, and when
static DWORD s_selT;
int HolsterBowSpot() { return (int)g_bowSpot; }
// Game thread (engine.cpp), once a frame, with the player's CPawn component (or null).
void HolsterGameTick(BYTE *pawn) {
    static DWORD s_scanT;
    AmmoGameTick(pawn);  // ammo2
    LONG req = InterlockedExchange(&g_req, -1);
    DWORD now = GetTickCount();
    bool scan = (g_icons || g_wrist) && g_on && now - s_scanT > 500;
    bool wrist = req >= 40 && req <= 43;  // wrist1m: picked on the wrist menu (no throw held)
    if (wrist) req -= 20;
    if (InterlockedExchange(&g_abortReq, 0)) {  // heart3: put the held throwable away (the game's abort)
        BYTE *st = (BYTE *)g_gStrat;
        bool ok = g_gAbort && st && g_gUser && g_bd && !IsBadReadPtr(st, 0x100) && *(DWORD *)st == (DWORD)(g_bd + kGadgetVtBd) && now - g_gStratT < 600000;
        if (ok) {
            InterlockedExchange(&g_abortUntil, (LONG)(now + 1500));
            g_gAbort(st, g_gUser);
        }
        Log("holster: cyber heart put back: %s", ok ? "the game's abort called" : "the game's throw gadget was not seen, so it may still be thrown");
    }
    {  // bowspot2: the bow's spot = the spot a grip took it from (Far Cry 3's slot reader is not ported)
        static bool s_bowWas;
        bool bowNow = BowActive();
        if (bowNow && !s_bowWas && s_selSpot >= 0 && now - s_selT < 4000 && InterlockedExchange(&g_bowSpot, s_selSpot) != s_selSpot)
            Log("holster: the bow is kept at the %s (taken from there)", SpotName(s_selSpot));
        s_bowWas = bowNow;
    }
    if ((req < 0 && !scan) || !pawn || !Ready()) return;
    BYTE *owner = !IsBadReadPtr(pawn + 0x24, 4) ? *(BYTE **)(pawn + 0x24) : nullptr;
    if (!owner || IsBadReadPtr(owner + 0xbd4, 0x100)) {
        if (req >= 0) Log("holster: player inventory not readable; request %ld dropped", req);
        return;
    }
    if (req == 21 || req == 23) {
        // bd16: C4 / mine by weapon type (BD+10520462 (inventory, &crc32 of the type name), ret 4, al), as the
        // game's own key handlers at BD+11243541.. do. Which type is which is a first guess: C4 = IED_Explosive,
        // mine = the next types the handlers know (BD+118196d0, IED_Incendiary, IED_Silent), first one owned.
        static const DWORD kC4[] = {0x181967c}, kMine[] = {0x18196d0, 0x1819680, 0x1819684};
        const DWORD *t = req == 21 ? kC4 : kMine;
        int n = req == 21 ? 1 : 3, got = -1;
        // bd29 (DrGonzaga 13 Nov: mines "none owned"; his log names the weapon "weapons:FC3/ProximityMine"): the mine
        // as FC3 does it. Find weapon BD+1051ca34 (thiscall inventory, &name hash; ret 4; same hash table as FC3,
        // mine d2dec8ba), select BD+10522b8e (thiscall inventory, weapon, 0; ret 8), then the same "switch" post.
        if (req == 23 && !memcmp(g_bd + 0x51ca34, "\x55\x8b\xec\x83\xec\x0c\x8b\x41", 8) &&
            !memcmp(g_bd + 0x522b8e, "\x55\x8b\xec\x83\xec\x0c\x56\x8b\xf1\x80", 10)) {
            DWORD hash = kHashMine;
            BYTE *w = (BYTE *)BdThisCall(g_bd + 0x51ca34, owner + 0xbd4, 1, &hash);
            if (w) {
                BdThisCall(g_bd + 0x522b8e, owner + 0xbd4, 2, w, 0);
                BdThisCall(g_bd + 0x1243485, pawn, 1, pawn);
            }
            snprintf(g_last, sizeof g_last, "%s: %s", SpotName(req - 16), w ? "in hand" : "none owned");
            Log("holster: grip at %s%s: %s (by weapon name)", wrist ? "the wrist menu, " : "", SpotName(req - 16), w ? "taken into the hand" : "none owned");
            return;
        }
        for (int i = 0; i < n && got < 0; ++i)
            if ((DWORD)BdThisCall(g_bd + 0x520462, owner + 0xbd4, 1, g_bd + t[i]) & 0xff) got = i;
        if (got >= 0) BdThisCall(g_bd + 0x1243485, pawn, 1, pawn);
        snprintf(g_last, sizeof g_last, "%s: %s", SpotName(req - 16), got >= 0 ? "in hand" : "none owned");
        Log("holster: grip at %s%s: %s (type %08lx)", wrist ? "the wrist menu, " : "", SpotName(req - 16), got >= 0 ? "taken into the hand" : "none owned",
            got >= 0 ? *(DWORD *)(g_bd + t[got]) : 0ul);
        return;
    }
    if (req < 0 || req >= 20) {
        // bd16: throwables, as BD+1051dd91 (select by name) walks them: catalogue = BD+10427d10 (ecx = *(BD+11846780),
        // stack 0), count / id = its vtable +0x74 / +0x78, item = BD+1051d017 (inventory, id), owned *(item+0x3c)+0x60.
        s_scanT = now;
        int want = req >= 20 ? req - 20 : -1;
        bool done = false;
        LONG have[4] = {0, 0, 0, 0}, hearts = 0;
        BYTE *inv = owner + 0xbd4, *glob = *(BYTE **)(g_bd + 0x1846780);
        DWORD curHash = *(DWORD *)(inv + 0xe4);  // wrist1m: the game's current throwable (its name's hash)
        LONG cur = -1;
        BYTE *cat = glob && !IsBadReadPtr(glob + 0x68, 8) ? (BYTE *)BdThisCall(g_bd + 0x427d10, glob, 1, 0) : nullptr;
        if (cat && !IsBadReadPtr(cat, 4)) {
            DWORD *vt = *(DWORD **)cat;
            int n = ((CountFn)vt[0x74 / 4])(cat);
            static DWORD s_logged[16];
            for (int i = 0; i < n && i < 64; ++i) {
                DWORD out[4] = {};
                DWORD *id = ((IdFn)vt[0x78 / 4])(cat, out, i);
                if (!id) continue;
                BYTE *item = (BYTE *)BdThisCall(g_bd + 0x51d017, inv, 1, id);
                if (!item || IsBadReadPtr(item, 0x40)) continue;
                BYTE *x = *(BYTE **)(item + 0x3c);
                int cnt = x && !IsBadReadPtr(x + 0x60, 4) ? *(int *)(x + 0x60) : 0;
                BYTE *a = *(BYTE **)(item + 4), *b = a && !IsBadReadPtr(a + 0xc, 4) ? *(BYTE **)(a + 0xc) : nullptr;
                if (!b || IsBadReadPtr(b + 0x10, 4)) continue;
                BYTE *blk = *(BYTE **)(b + 0x10);
                const char *name = blk && !IsBadReadPtr(blk + 0xc, 32) ? (const char *)(blk + 0xc) : "";
                int k = Kind(name);
                if (strstr(name, "Lurer") || strstr(name, "lurer")) {  // heart2: the cyber heart
                    hearts += cnt;
                    if (cnt > 0) g_heartHash = NameHash(name) ? NameHash(name) : g_heartHash;  // heart4
                    if (want == 4 && cnt > 0) done = true;
                }
                bool seen = false;
                for (int j = 0; j < 16; ++j) seen |= s_logged[j] == *id;
                if (!seen) {
                    for (int j = 0; j < 16; ++j)
                        if (!s_logged[j]) { s_logged[j] = *id; break; }
                    Log("holster: throwable \"%.60s\" x%d -> %s", name, cnt,
                        k == 0 ? "grenade" : k == 1 ? "C4" : k == 2 ? "molotov" : k == 3 ? "mine" : "not on the chest");
                }
                if (k < 0) continue;
                if (curHash && curHash != 0xffffffff && NameHash(name) == curHash) cur = k;  // wrist1m
                have[k] += cnt;
                if (k == want && cnt > 0 && !done) BdThisCall(g_bd + 0x51dd91, inv, 1, b + 0x10), done = true;
            }
        }
        have[1] = have[1] ? have[1] : -1, have[3] = have[3] ? have[3] : -1;  // C4 / mines: not counted (unknown)
        for (int k = 0; k < 4; ++k) InterlockedExchange(&g_have[k], have[k]);
        InterlockedExchange(&g_heartHave, hearts);
        if (want == 4) {  // heart2
            snprintf(g_last, sizeof g_last, "cyber heart: %s", done ? "in hand, let go to throw" : "none left");
            Log("holster: cyber heart from the wrist menu: %s", done ? "owned, the held throw takes it" : "none owned");
            if (!done) InterlockedExchange(&g_heartUse, 0), InterlockedExchange(&g_throwHold, 0), InterlockedExchange(&g_nadeNone, 1);
            return;
        }
        if (done && want >= 0) cur = want;
        if (InterlockedExchange(&g_curKind, cur) != cur) Log("holster: the game's throwable is now %s (hash %08lx)", cur == 0 ? "the grenade" : cur == 2 ? "the molotov" : "unknown", curHash);
        if (req >= 20) {
            snprintf(g_last, sizeof g_last, "%s: %s", SpotName(req - 16), done ? "picked, let go to throw" : "none left");
            Log("holster: grip at %s%s: %s", wrist ? "the wrist menu, " : "", SpotName(req - 16), done ? "picked" : "none owned");
            if (!done && !wrist) InterlockedExchange(&g_throwHold, 0), InterlockedExchange(&g_nadeNone, 1);
        }
        return;
    }
    int slot = g_slot[req];
    bool ok = ((DWORD)BdThisCall(g_bd + 0x520550, owner + 0xbd4, 1, slot) & 0xff) != 0;
    if (ok) BdThisCall(g_bd + 0x1243485, pawn, 1, pawn), s_selSpot = req, s_selT = GetTickCount();
    snprintf(g_last, sizeof g_last, "%s (slot %d): %s", SpotName(req), slot + 1, ok ? "switched" : "empty, or already in hand");
    Log("holster: grip at %s: slot %d %s", SpotName(req), slot + 1, ok ? "selected" : "not changed (empty, or already this weapon)");
}

// Per eye (hook.cpp, after the beam): see-through spheres on the hips, and on the chest
// spheres with the throwable icons. On foot with 6DoF only (they sit on your real body).
void HolsterDraw(IDXGISwapChain *sc) {
    bool hol = HolsterActive() && (g_spheres || g_icons);
    bool wristOk = g_wrist && !CyberEyeOn() && g_on && g_state >= 0 && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() && !GamePaused();  // wrist1m (also with the debug menu open)
    // hands1 (DrGonzaga 12 Nov): small see-through spheres where your hands (controllers) are, each gone
    // while that hand holds something. Same marks, same single draw as the holster spheres.
    bool hands = g_handsOn && !CyberEyeOn() && EngineOnFoot() && !EngineInVehicle() && !EngineCutscene() && !MenuOpen() && !GamePaused();
    if (!hol && !hands && !wristOk) return;
    EngineView v;
    Spots sp;
    if (!EngineLastView(&v) || !v.sixdof) return;
    if (hol && !GetSpots(sp, false)) hol = false;
    if (!hol) memset(&sp, 0, sizeof sp);
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    auto world = [&](const float *c, float *w) {
        float o[3] = {c[0] / v.scale, c[1] / v.scale, c[2] / v.scale};
        for (int i = 0; i < 3; ++i) w[i] = v.base[i] + fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2];
    };
    LaserMark m[32];
    int n = 0, at = g_spot;
    LONG curKind = g_curKind;
    auto sphere = [&](int s, float r) {
        LaserMark &k = m[n++];
        world(sp.c[s], k.pos);
        bool hot = at == s;
        k.radius = (hot ? r * 1.25f : r) / v.scale, k.icon = 0;
        k.rgba[0] = 0.75f, k.rgba[1] = 0.9f, k.rgba[2] = 1.0f;
        k.rgba[3] = hot ? (g_opacity * 1.8f > 1 ? 1 : g_opacity * 1.8f) : g_opacity;
    };
    if (hands) {
        // In a hand: a weapon seen in the player's hand in the last 0.3 s (right hand; the bow sits in the
        // left), a grenade taken from the chest, or both hands on the gun.
        bool gun = GetTickCount() - GunAttachHandTick() < 300, bow = gun && BowOut();
        bool full[2] = {bow || TwoHandHeld(), (gun && !bow) || (bow && BowRtHeld()) || g_nade != 0};
        for (int h = 0; h < 2; ++h) {
            float hm[16], hp[3];
            if (full[h] || !HeadtrackHandRaw(h, hm, hp)) continue;
            LaserMark &k = m[n++];
            world(hp, k.pos);
            k.radius = g_handSize / v.scale, k.icon = 0;
            k.rgba[0] = 0.75f, k.rgba[1] = 0.9f, k.rgba[2] = 1.0f, k.rgba[3] = g_handOpacity;
        }
    }
    if (hol && g_spheres) sphere(0, g_hipSize), sphere(1, g_hipSize);
    if (hol && g_icons)
        for (int s = 4; s < 8; ++s) {
            LONG have = g_have[s - 4];
            sphere(s, g_chestSize);
            if (have == 0) m[n - 1].rgba[3] *= 0.4f;
            LaserMark &k = m[n++];
            world(sp.c[s], k.pos);
            bool hot = at == s || ((s == 4 || s == 6) && curKind == s - 4);  // wrist1m: the X-button type stays larger
            k.radius = (hot ? g_chestSize * 0.95f : g_chestSize * 0.75f) / v.scale, k.icon = 1 + (s - 4);
            k.rgba[0] = 0.9f, k.rgba[1] = 0.9f, k.rgba[2] = 0.85f;
            float op = g_opacity * 1.8f > 1 ? 1 : g_opacity * 1.8f;  // holster6: icons follow the opacity slider
            k.rgba[3] = have == 0 ? g_opacity * 0.45f : hot ? (op * 1.3f > 1 ? 1 : op * 1.3f) : op;
        }
    // wrist1m: the wrist menu: a disc, the icon and (grenade / molotov) how many are left; fades in
    // and out with the glance (~0.15 s, by time: this runs once per eye)
    if (wristOk) {
        static float s_fade;
        static DWORD s_t;
        DWORD now = GetTickCount();
        float dt = s_t ? (now - s_t) * 0.001f : 0, want = g_wristSeen ? 1.f : 0.f, wc[6][3];
        s_t = now;
        if (dt > 0.1f) dt = 0.1f;
        s_fade = want > s_fade ? fminf(want, s_fade + dt / 0.15f) : fmaxf(want, s_fade - dt / 0.15f);
        if (s_fade > 0.001f && WristFrame(wc, nullptr, true))
            for (int w = 0; w < 4 && n + 3 <= 32; ++w) {
                int kind = kWristKind[w];
                LONG have = g_have[kind];
                bool hot = at == 11 + w, sel = (kind == 0 || kind == 2) && curKind == kind;
                float R = g_wSize * (hot ? 1.3f : sel ? 1.2f : 1.0f), op = g_wOpacity * s_fade * (have == 0 ? 0.35f : 1.f);
                LaserMark &d = m[n++];
                world(wc[w], d.pos);
                d.radius = R / v.scale, d.icon = 0;
                d.rgba[0] = sel ? 1.0f : 0.75f, d.rgba[1] = sel ? 0.75f : 0.9f, d.rgba[2] = sel ? 0.35f : 1.0f;
                d.rgba[3] = op * (hot ? 0.5f : sel ? 0.4f : 0.25f);
                LaserMark &k = m[n++];
                world(wc[w], k.pos);
                k.radius = R * 0.8f / v.scale, k.icon = 1 + kind;
                k.rgba[0] = 0.9f, k.rgba[1] = 0.9f, k.rgba[2] = 0.85f, k.rgba[3] = op;
                if (g_wristCount && (kind == 0 || kind == 2) && have >= 0) {  // the number, low on the icon
                    LaserMark &c = m[n++];
                    float p[3] = {wc[w][0], wc[w][1] - R * (0.95f - 0.3f * (1 - g_wCountSize)), wc[w][2]};  // smallnum1: smaller, a little higher
                    world(p, c.pos);
                    c.radius = R * 0.55f * g_wCountSize / v.scale, c.icon = 8, c.val = (float)(have > 99 ? 99 : have) + g_wCountStyle * 10000.f;
                    c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 memcpy(c.bg, g_wCountBg, 16), memcpy(c.off, g_wCountOff, 12), c.off[3] = 1;  // numcol2
                }
            }
        // heart1: the cyber heart, on the menu (dimmed while it is in the hand) and riding on the right hand
        if (g_bloodDragon && g_heart && s_fade > 0.001f && n + 2 <= 32 && WristFrame(wc, nullptr, true)) {
            bool hot = at == 15;
            float R = g_wSize * (hot ? 1.3f : 1.0f), op = g_wOpacity * s_fade * (g_heartHeld || g_heartHave == 0 ? 0.35f : 1.f);
            LaserMark &d = m[n++];
            world(wc[4], d.pos);
            d.radius = R / v.scale, d.icon = 0;
            d.rgba[0] = 0.75f, d.rgba[1] = 0.9f, d.rgba[2] = 1.0f, d.rgba[3] = op * (hot ? 0.5f : 0.25f);
            LaserMark &k = m[n++];
            world(wc[4], k.pos);
            k.radius = R * 0.8f / v.scale, k.icon = 9;
            k.rgba[0] = 0.9f, k.rgba[1] = 0.9f, k.rgba[2] = 0.85f, k.rgba[3] = op;
            LONG hh = g_heartHave;
            if (g_wristCount && hh >= 0 && n < 32) {  // heart2: how many are left
                LaserMark &c = m[n++];
                float p[3] = {wc[4][0], wc[4][1] - R * (0.95f - 0.3f * (1 - g_wCountSize)), wc[4][2]};  // smallnum1
                world(p, c.pos);
                c.radius = R * 0.55f * g_wCountSize / v.scale, c.icon = 8, c.val = (float)(hh > 99 ? 99 : hh) + g_wCountStyle * 10000.f;
                c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 c.rgba[0] = g_wCountCol[0], c.rgba[1] = g_wCountCol[1], c.rgba[2] = g_wCountCol[2], c.rgba[3] = op;  // numcol1
 memcpy(c.bg, g_wCountBg, 16), memcpy(c.off, g_wCountOff, 12), c.off[3] = 1;  // numcol2
            }
        }
        // slowmo1: the clock; while slow motion is on its disc is amber and (timed) the seconds left show under it
        if (SlowmoOnWrist() && s_fade > 0.001f && n + 3 <= 32 && WristFrame(wc, nullptr, true)) {
            bool hot = at == 16, on = SlowmoActive();
            float R = g_wSize * (hot ? 1.3f : on ? 1.2f : 1.0f), op = g_wOpacity * s_fade;
            LaserMark &d = m[n++];
            world(wc[5], d.pos);
            d.radius = R / v.scale, d.icon = 0;
            d.rgba[0] = on ? 1.0f : 0.75f, d.rgba[1] = on ? 0.75f : 0.9f, d.rgba[2] = on ? 0.35f : 1.0f;  // on = the "chosen" colours
            d.rgba[3] = op * (hot ? 0.5f : on ? 0.4f : 0.25f);
            LaserMark &k = m[n++];
            world(wc[5], k.pos);
            k.radius = R * 0.8f / v.scale, k.icon = 10;
            if (on) k.rgba[0] = 1.0f, k.rgba[1] = 0.75f, k.rgba[2] = 0.3f;
            else if (g_neon) k.rgba[0] = 0.2f, k.rgba[1] = 1.0f, k.rgba[2] = 1.0f;
            else k.rgba[0] = 0.9f, k.rgba[1] = 0.9f, k.rgba[2] = 0.85f;
            k.rgba[3] = op;
            float left = SlowmoLeft();
            if (left >= 0 && *SlowmoShowSeconds()) {  // slowglow1
                LaserMark &c = m[n++];
                float p[3] = {wc[5][0], wc[5][1] - R * 0.95f, wc[5][2]};
                world(p, c.pos);
                c.radius = R * 0.55f / v.scale, c.icon = 8, c.val = ceilf(left > 99 ? 99 : left);
                c.rgba[0] = 1.0f, c.rgba[1] = 0.8f, c.rgba[2] = 0.4f, c.rgba[3] = op;
            }
        }
        float hm[16], hp[3];
        if (g_heartHeld && !g_heartModel && n < 32 && HeadtrackHandRaw(1, hm, hp)) {
            float p[3];
            for (int i = 0; i < 3; ++i) p[i] = hp[i] - hm[i * 4 + 2] * 0.06f + hm[i * 4 + 1] * 0.03f;  // a little ahead of and above the grip
            LaserMark &k = m[n++];
            world(p, k.pos);
            k.radius = g_wSize * 1.3f / v.scale, k.icon = 9;
            k.rgba[0] = 0.9f, k.rgba[1] = 0.9f, k.rgba[2] = 0.85f, k.rgba[3] = g_wOpacity;
        }
    }
    HolsterNeonMarks(m, n);
    LaserDrawMarks(sc, m, n);
}

// neon1, pulled out by slowring1 (DrGonzaga 16 Nov: the floating slow-motion button should have the same pink
// ring look as the left-hand buttons): spheres become cyan wire globes; a sphere behind an icon becomes a
// plain magenta ring and its icon a glowing cyan outline.
void HolsterNeonMarks(LaserMark *m, int n) {
    if (g_neon) {
        for (int i = 0; i < n; ++i) {
            LaserMark &k = m[i];
            if (k.icon == 8) continue;  // wrist1m: the count stays a number
            if (k.icon == 0) {
                bool backing = i + 1 < n && m[i + 1].icon != 0 && m[i + 1].icon != 8;
                bool sel = k.rgba[0] == 1.0f && k.rgba[2] == 0.35f;  // the wrist menu's chosen type: a cyan ring
                k.icon = backing ? 24 : 16;
                if (backing && !sel) k.rgba[0] = 1.0f, k.rgba[1] = 0.15f, k.rgba[2] = 0.8f;
                else k.rgba[0] = 0.1f, k.rgba[1] = 0.9f, k.rgba[2] = 1.0f;
            } else {
                if (k.icon <= 4) k.rgba[0] = 0.2f, k.rgba[1] = 1.0f, k.rgba[2] = 1.0f;
                k.icon += 16;
            }
        }
    }
}

// holster4: the chest row's centre and the body facing it uses (hud.cpp anchor 4, the
// minimap moving with the chest spheres). False until the holsters have a body facing.
bool HolsterChestFrame(float c[3], float f[3], float r[3]) {
    Spots sp;
    if (!g_on || !g_bodyYawSet || !GetSpots(sp, false)) return false;
    for (int i = 0; i < 3; ++i) c[i] = (sp.c[4][i] + sp.c[6][i]) * 0.5f, f[i] = sp.f[i], r[i] = sp.r[i];  // the two outer ones
    return true;
}
