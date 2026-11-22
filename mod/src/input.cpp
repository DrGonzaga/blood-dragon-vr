// Motion controls, step 1: the game reads the Touch controllers as an Xbox
// pad. FC3_d3d11.dll imports XInputGetState (xinput1_3 ordinal 2); we point
// that import at ours and mix the controllers into pad 1. A real pad still
// works alongside.
//
//   Touch                         Xbox
//   left / right stick (+click)   left / right stick (+click)
//   left / right trigger          LT (aim) / RT (fire)
//   left / right grip             LB / RB
//   A B X Y                       A B X Y
//   menu                          Start
//   left grip + menu              Back
//   right grip + menu             show/hide the HUD (mod)
// (remappable: Debug menu, Buttons page)
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#include "fc3vr.h"

static float g_slow[2] = {1, 1};  // ads1: [aim] AdsSlow on, [gameplay] CrouchSpeed
float *InputSlowSpeeds() { return g_slow; }
static float g_crouchCap = 0.5f;  // adscrouch1: [gameplay] CrouchStickCap, stick share that is full crouched speed
float *InputCrouchCap() { return &g_crouchCap; }
static bool g_abZoom = true;  // zoom1: [aim] ScopeZoomButtons
bool *InputScopeZoomButtons() { return &g_abZoom; }
static volatile bool g_wheelHeld;  // weapon wheel (left grip / LB) held
bool InputWheelHeld() { return g_wheelHeld; }


// scope9: last time the game's aim button (left trigger) was held down.
static volatile DWORD g_aimHeldTick;
bool InputAimHeld() { return g_aimHeldTick && GetTickCount() - g_aimHeldTick < 150; }

typedef DWORD(WINAPI *GetStateFn)(DWORD, XINPUT_STATE *);
static GetStateFn g_origGetState;

static SHORT Axis(float v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return (SHORT)(v * 32767);
}

// firelag1 (DrGonzaga 9 Nov: sometimes a delay between pulling the trigger and the
// first shot, mostly when pointing at enemies). Measures trigger-down to the
// player's first shot; logs only when it is slow (over 150 ms) or no shot came.
static volatile LONG g_fireDown;  // tick the trigger went down, 0 = no shot pending
static void FireNoteTrigger(bool down) {
    static bool s_was;
    DWORD now = GetTickCount();
    if (down && !s_was) InterlockedExchange(&g_fireDown, (LONG)(now | 1));
    if (!down && s_was) {
        LONG t = InterlockedExchange(&g_fireDown, 0);
        const char *gun = GunAttachName();
        if (t && (LONG)(now - (DWORD)t) > 150 && EngineOnFoot() && !strstr(gun, "BOW") && !strstr(gun, "IED"))  // the bow fires on release
            LogV("fire: trigger held %lu ms and no shot, %s (aim %s, gun at face %d)", now - (DWORD)t, gun,
                InputAimHeld() ? "held" : "off", (int)StockGunAtFace());
    }
    s_was = down;
}
static volatile DWORD g_lastShotT;  // turtrig1: when the game last fired
void InputNoteShot() {
    g_lastShotT = GetTickCount();
    LONG t = InterlockedExchange(&g_fireDown, 0);
    if (!t) return;
    LONG d = (LONG)(GetTickCount() - (DWORD)t);  // can be -1: the press tick is stored with its low bit set
    if (d > 150) LogV("fire: first shot %ld ms after the trigger, %s (aim %s, gun at face %d)", d, GunAttachName(),
                     InputAimHeld() ? "held" : "off", (int)StockGunAtFace());
}

static BYTE Trigger(float v) { return (BYTE)(v < 0 ? 0 : v > 1 ? 255 : v * 255); }
// lasers3 (DrGonzaga 10 Nov): the right trigger a little more sensitive: its pull is scaled
// up before it goes to the game, so the game's fire point comes earlier in the pull.
static float g_rtSens = 1.5f;  // [input] RightTriggerSensitivity, 1..3
float *InputTriggerSens() { return &g_rtSens; }

static SHORT Bigger(SHORT a, SHORT b) { return abs(a) >= abs(b) ? a : b; }

// Button mapping (Debug menu "Buttons" page, fc3vr.ini [buttons]): each
// Touch input sends one Xbox input. Defaults are the table above.
enum { kOutNone, kOutA, kOutB, kOutX, kOutY, kOutLB, kOutRB, kOutLT, kOutRT, kOutLS, kOutRS, kOutStart, kOutBack,
       kOutUp, kOutDown, kOutLeft, kOutRight, kOutHud, kOutCount };
static const char *const kOutNames[kOutCount] = {"Nothing", "A", "B", "X", "Y", "LB (left bumper)", "RB (right bumper)",
                                                 "LT (left trigger)", "RT (right trigger)", "Left stick click",
                                                 "Right stick click", "Start", "Back", "D-pad up", "D-pad down",
                                                 "D-pad left", "D-pad right",
                                                 "Show/hide HUD (mod)"};
static const WORD kOutBits[kOutCount] = {0, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                         XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
                                         XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START,
                                         XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                                         XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT, 0};
enum { kInA, kInB, kInX, kInY, kInLGrip, kInRGrip, kInLClick, kInRClick, kInMenu, kInGripMenu, kInRGripMenu, kInLT, kInRT, kInStickUp, kInXY, kInAB, kInCount };
static const char *const kInNames[kInCount] = {"A", "B", "X", "Y", "Left grip", "Right grip", "Left stick click",
                                               "Right stick click", "Menu button", "Left grip + Menu", "Right grip + Menu",
                                               "Left trigger", "Right trigger",
                                               "Right stick pushed up hard", "X + Y together", "A + B together"};
static const int kInDefaults[kInCount] = {kOutA, kOutB, kOutX, kOutY, kOutLB, kOutRB, kOutLS, kOutRS, kOutStart, kOutBack,
                                          kOutNone, kOutLT, kOutRT, kOutRight, kOutLeft, kOutUp};
static int g_map[kInCount] = {kOutA,  kOutB,     kOutX,  kOutY,  kOutLB, kOutRB,   kOutLS,   kOutRS,
                              kOutStart, kOutBack, kOutNone, kOutLT, kOutRT, kOutRight, kOutLeft, kOutUp};

int InputMapCount() { return kInCount; }
const char *InputMapName(int i) { return i >= 0 && i < kInCount ? kInNames[i] : ""; }
int *InputMap(int i) { return &g_map[i < 0 || i >= kInCount ? 0 : i]; }
int InputOutputCount() { return kOutCount; }
const char *InputOutputName(int o) { return o >= 0 && o < kOutCount ? kOutNames[o] : "?"; }
void InputMapDefaults() { memcpy(g_map, kInDefaults, sizeof g_map); }

static const wchar_t *const kInKeys[kInCount] = {L"A", L"B", L"X", L"Y", L"LeftGrip", L"RightGrip", L"LeftStickClick",
                                                 L"RightStickClick", L"Menu", L"LeftGripMenu", L"RightGripMenu", L"LeftTrigger",
                                                 L"RightTrigger", L"RightStickUp", L"XY", L"AB"};
// grip1 (DrGonzaga 3 Nov): holding the right grip keeps the controller tracked
// (the Quest dropped it when held dead still), so on foot the grip sends
// nothing and X sends what the grip was set to (grenade; grip2, DrGonzaga 3 Nov). Off foot as mapped.
static bool g_gripHold = true;
bool *InputGripHoldPtr() { return &g_gripHold; }
// lasers1 (DrGonzaga 10 Nov): with grenades and molotovs on the chest, X (on foot, gun in
// hand) shows / hides the minimap instead of throwing. Same gating as qte1, so in
// cutscenes and button-mash scenes X still goes to the game as mapped.
static int g_xMode = 2;  // [buttons] XButtonMode: 0 grenade (the grip's job), 1 minimap on/off, 2 game map (map1x, DrGonzaga 10 Nov)
// map1x (DrGonzaga 10 Nov): the left grip opened the game's weapon wheel; with the
// holsters it does nothing on foot unless chosen (two-handed grip still works).
static bool g_lgripWheel = false;  // [buttons] LeftGripWheel
bool *InputLeftGripWheel() { return &g_lgripWheel; }
int *InputXMode() { return &g_xMode; }
// wrist1m (DrGonzaga 13 Nov): the Menu button opens the game's full map (the pad's Back); holding the
// left trigger (or left grip) and tapping Menu is the game's pause menu (Start). While the game is
// paused Menu goes to the game as mapped, so it closes the pause menu.
static bool g_menuMap = true;  // [buttons] MenuMinimap
bool *InputMenuMinimap() { return &g_menuMap; }
static volatile bool g_xThrow;  // X held as the grenade throw now
static int s_throwOut = -1;  // heartadj1: the output the held throw is pressed on (-1 none)
bool InputThrowAiming() { return g_xThrow || HolsterThrowing(); }
// lasers1 (DrGonzaga 10 Nov): reaching behind the shoulder turned the walk, as the game walks
// where its look (the right controller) points. Now the left stick walks where the left
// controller points: the stick is turned by (left controller heading - game look heading).
static bool g_moveLeft = true;  // [buttons] WalkWithLeftHand
bool *InputMoveByLeftHand() { return &g_moveLeft; }
static void MoveByLeftHand(VrPad &p) {
    if (!g_moveLeft || p.lx * p.lx + p.ly * p.ly < 0.0225f || MenuOpen() || !EngineOnFoot() || EngineInVehicle() ||
        EngineTurret() || EngineCutscene())
        return;
    static float s_delta;
    static DWORD s_t;
    DWORD now = GetTickCount();
    EngineView v;
    float m[16], gy, gf[2], gp;
    if (EngineLastView(&v) && HeadtrackHand(0, m) && EngineGameLook(&gy, gf, &gp)) {
        const float *r = v.basis, *u = v.basis + 3, *f = v.basis + 6;
        float d[2];
        for (int i = 0; i < 2; ++i) d[i] = -(r[i] * m[2] + u[i] * m[6] - f[i] * m[10]);  // controller forward (as laser.cpp)
        // Pointing well up or down: keep the last heading (no flip when the hand is near vertical).
        if (d[0] * d[0] + d[1] * d[1] > 0.09f && gf[0] * gf[0] + gf[1] * gf[1] > 0.04f)
            s_delta = atan2f(d[1], d[0]) - atan2f(gf[1], gf[0]), s_t = now;
    }
    if (!s_t || now - s_t > 1000) return;
    float c = cosf(s_delta), sn = sinf(s_delta), x = p.lx, y = p.ly;
    p.ly = y * c + x * sn, p.lx = x * c - y * sn;  // forward / right turned by s_delta (anticlockwise seen from above)
}
void InputMapLoad() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    g_gripHold = GetPrivateProfileIntW(L"buttons", L"GripHoldsGun", 1, path) != 0;
    g_xMode = GetPrivateProfileIntW(L"buttons", L"XButtonMode", 2, path);  // map1x (was XButton 0/1)
    if (g_xMode < 0 || g_xMode > 2) g_xMode = 2;
    // wrist1m: X is the quick throw now (tap throws the wrist menu's type, hold cooks); switched over
    // once, after that the Buttons page choice is kept.
    if (!GetPrivateProfileIntW(L"buttons", L"QuickThrowSet", 0, path)) {
        g_xMode = 0;
        WritePrivateProfileStringW(L"buttons", L"QuickThrowSet", L"1", path);
        WritePrivateProfileStringW(L"buttons", L"XButtonMode", L"0", path);
    }
    g_menuMap = GetPrivateProfileIntW(L"buttons", L"MenuMinimap", 1, path) != 0;
    g_lgripWheel = GetPrivateProfileIntW(L"buttons", L"LeftGripWheel", 0, path) != 0;
    g_moveLeft = GetPrivateProfileIntW(L"buttons", L"WalkWithLeftHand", 1, path) != 0;
    for (int i = 0; i < kInCount; ++i) {
        int v = GetPrivateProfileIntW(L"buttons", kInKeys[i], kInDefaults[i], path);
        g_map[i] = v >= 0 && v < kOutCount ? v : kInDefaults[i];
    }
}
void InputMapSave() {
    wchar_t path[MAX_PATH], buf[16];
    IniPath(path);
    for (int i = 0; i < kInCount; ++i) {
        swprintf(buf, 16, L"%d", g_map[i]);
        WritePrivateProfileStringW(L"buttons", kInKeys[i], buf, path);
    }
    WritePrivateProfileStringW(L"buttons", L"GripHoldsGun", g_gripHold ? L"1" : L"0", path);
    swprintf(buf, 16, L"%d", g_xMode);
    WritePrivateProfileStringW(L"buttons", L"XButtonMode", buf, path);
    WritePrivateProfileStringW(L"buttons", L"MenuMinimap", g_menuMap ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"buttons", L"LeftGripWheel", g_lgripWheel ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"buttons", L"WalkWithLeftHand", g_moveLeft ? L"1" : L"0", path);
}

// dpad2 (DrGonzaga 2 Nov): two buttons pressed together (within 70 ms) are a
// combo input of their own; the single buttons are then not sent. A single
// press waits at most 70 ms for its partner (a tap shorter than that is still sent).
namespace {
struct Pair {
    int st;  // 0 idle, 1 waiting for the partner, 2 single button, 3 combo
    DWORD t0, tapUntil;
    bool tapA;
};
}  // namespace
static bool PairInput(bool a, bool b, Pair &s, bool &outA, bool &outB) {
    DWORD t = GetTickCount();
    outA = outB = false;
    if (s.st == 0 && (a || b)) s.st = 1, s.t0 = t;
    if (s.st == 1) {
        if (a && b) s.st = 3;
        else if (!a && !b) s.st = 0, s.tapUntil = t + 60;  // quick tap: send it now
        else if (t - s.t0 >= 70) s.st = 2;
        else s.tapA = a;  // which one is waiting
    }
    if ((s.st == 2 || s.st == 3) && !a && !b) s.st = 0;
    if (s.st == 2) outA = a, outB = b;
    if (s.st == 0 && t < s.tapUntil) (s.tapA ? outA : outB) = true;
    return s.st == 3;
}

// dpad1: on foot, pushing the right stick up hard (away from you) is an input
// of its own, held while the stick is held (hold and let go, e.g. to throw).
static bool g_stickUp;
bool InputStickUpHeld() { return g_stickUp; }  // dice1
static bool StickUp(VrPad &p, bool ok) {
    if (BowState() >= 0 || BowClassic()) ok = false;  // bowtrig1: not with the bow in the left hand (as before)
    if (!ok || p.ry < 0.4f) g_stickUp = false;
    else if (p.ry > 0.85f && fabsf(p.rx) < 0.6f) {
        if (!g_stickUp) Log("input: right stick pushed up");
        g_stickUp = true;
    }
    if (g_stickUp) p.ry = 0;  // no gun tilt while it's held
    return g_stickUp;
}

// bowmenu1 (DrGonzaga 13 Nov): the left grip as pressed, before the bow / two-handed hold takes it
// from the game, so left grip + Menu is still the pause menu with the bow (or a gun) held by it.
static bool g_lgripRaw;
static void Mix(const VrPad &p, XINPUT_GAMEPAD &g, bool stickUp) {
    bool lgrip = p.lgrip > 0.5f, rgrip = p.rgrip > 0.5f;
    float in[kInCount] = {};
    static Pair s_xy, s_ab;
    bool x = false, y = false, a = false, b2 = false;
    in[kInXY] = PairInput(p.x, p.y, s_xy, x, y);
    in[kInAB] = PairInput(p.a, p.b, s_ab, a, b2);
    in[kInA] = a, in[kInB] = b2, in[kInX] = x, in[kInY] = y;
    in[kInStickUp] = stickUp;
    // A grip + Menu combo replaces both.
    if (p.menu && lgrip) in[kInGripMenu] = 1, in[kInRGrip] = rgrip;
    else if (p.menu && rgrip) in[kInRGripMenu] = 1, in[kInLGrip] = lgrip;
    else in[kInMenu] = p.menu, in[kInLGrip] = lgrip, in[kInRGrip] = rgrip;
    in[kInLClick] = p.lclick, in[kInRClick] = p.rclick;
    in[kInLT] = p.lt, in[kInRT] = p.rt * (g_rtSens < 1 ? 1 : g_rtSens > 3 ? 3 : g_rtSens);  // lasers3
    WORD b = 0;
    float lt = 0, rt = 0;
    bool hud = false;
    int map[kInCount];
    memcpy(map, g_map, sizeof map);
    // grip3 (DrGonzaga 3 Nov): leaving the pause / map screen with X threw a grenade:
    // X is the game's back button there, and the grenade mapping took over as the
    // menu closed with X still held. Now the game's own mapping is used while it is
    // paused and for 0.5 s after; each X press keeps the mapping it started with.
    {
        static bool s_xGrenade, s_xWas;
        static DWORD s_pausedAt;
        DWORD now = GetTickCount();
        if (GamePaused()) s_pausedAt = now;
        // qte1 (DrGonzaga 10 Nov: kept dying in a "mash B" struggle scene, pressing X, his B):
        // the scene used the on-foot camera, so X became the grenade. Now only with a
        // weapon in hand (none in such scenes), as it's for throwing grenades.
        bool armed = now - GunAttachHandTick() < 300;
        bool ok = g_gripHold && EngineOnFoot() && armed && now - s_pausedAt > 500;
        bool xDown = in[kInX] > 0.5f;
        bool xPress = xDown && !s_xWas;
        if (!xDown || !s_xWas) s_xGrenade = ok;  // decided when X goes down
        s_xWas = xDown;
        // lasers1: X = minimap on/off (not in cutscenes or vehicles, and only with a gun in hand, as qte1).
        static bool s_xMap;
        if (xPress) s_xMap = s_xGrenade && g_xMode == 1 && !EngineCutscene() && !EngineInVehicle() && !MenuOpen();
        if (!xDown) s_xMap = false;
        if (xPress && s_xMap) HudMinimapToggle();
        if (s_xMap) map[kInX] = kOutNone;
        // map1x: X opens the game's map (the pad's Back, as left grip + Menu), same gating.
        if (s_xGrenade && g_xMode == 2 && !EngineCutscene() && !EngineInVehicle() && !MenuOpen()) map[kInX] = kOutBack;
        g_xThrow = xDown && s_xGrenade && g_xMode == 0;
        // wrist1m: Menu = the game's map, left trigger (or left grip) + Menu = pause. Each press keeps
        // the job it started with (decided when it goes down), so closing the pause menu never opens the map.
        if (g_menuMap) {
            static bool s_mWas, s_mOurs, s_mPause;
            bool mDown = in[kInMenu] > 0.5f;
            if (mDown && !s_mWas) {
                s_mOurs = !GamePaused() && now - s_pausedAt > 500 && !EngineCutscene() && !MenuOpen();
                s_mPause = p.lt > 0.5f || g_lgripRaw;  // bowmenu1
            }
            if (!mDown) s_mOurs = false;
            s_mWas = mDown;
            if (s_mOurs) map[kInMenu] = s_mPause ? kOutStart : kOutBack;
            map[kInGripMenu] = kOutStart;
        }
        // gripkeep1 (DrGonzaga 15 Nov, bd62 big problem: "When I selected the rifle and went to zoom in, I threw a
        // grenade (both sessions) the first time"): the first zoom of a session builds the scope lens and the game
        // stalls for about 0.2 s; "on foot" is a 0.2 s freshness test, so for that moment the held right grip was
        // no longer ours and its own game mapping (throw) went through. Now whose grip it is gets decided when
        // it goes down and stays so until it is let go; a stall can't change it.
        static bool s_rgWas, s_rgOurs;
        static DWORD s_footT;
        bool rgDown = in[kInRGrip] > 0.5f;
        if (ok || HolsterActive()) s_footT = now;
        bool oursNow = ok || HolsterActive() || now - s_footT < 1500;
        if (rgDown && !s_rgWas) s_rgOurs = oursNow;
        else if (rgDown && oursNow) s_rgOurs = true;
        if (!rgDown) s_rgOurs = false;
        s_rgWas = rgDown;
        if (ok || s_rgOurs) map[kInRGrip] = kOutNone;
        HolsterInput(p);  // holster1: the right grip draws from the body spots and holds the weapon
        if (HolsterTriggerOwned(p.rt) && !EngineTurret()) in[kInRT] = 0;  // nade1: the trigger arms the grenade, the gun doesn't fire (turtrig1: never on a mounted gun)
        if (HolsterActive() || s_rgOurs) map[kInRGrip] = kOutNone;
        // holster2: a throwable picked from the chest: the game's throw (the grip's own mapping) while held.
        if (HolsterThrowHeld()) in[kInRGrip] = 1, map[kInRGrip] = g_map[kInRGrip] != kOutNone ? g_map[kInRGrip] : g_map[kInX];
        s_throwOut = HolsterThrowHeld() ? map[kInRGrip] : -1;  // heartadj1
        if (s_xGrenade && g_xMode == 0) map[kInX] = g_map[kInRGrip];  // lasers1: minimap mode never throws
    }
    for (int i = 0; i < kInCount; ++i) {
        int o = map[i];
        if (o == kOutHud) {
            hud |= in[i] > 0.5f;
            continue;
        }
        if (o == kOutLT) lt = in[i] > lt ? in[i] : lt;
        else if (o == kOutRT) rt = in[i] > rt ? in[i] : rt;
        else if (in[i] > 0.5f) b |= kOutBits[o];
    }
    // nohud1 (DrGonzaga 9 Nov): the HUD show/hide button shortcut hid the HUD by accident
    // (right grip held to stop sticking + Menu). The HUD is now switched only from the
    // debug menu (HUD page); a button still mapped to it does nothing.
    (void)hud;
    g.wButtons |= b;
    if (BowCancelTap()) g.wButtons |= XINPUT_GAMEPAD_X;  // unnock10: the game's reload ends the draw
    BYTE blt = Trigger(lt), brt = Trigger(rt);
    if (blt > g.bLeftTrigger) g.bLeftTrigger = blt;
    if (!CyberEyeOn() && CyberEyeRecent()) g.bLeftTrigger = 0;  // cyoff1: just after the binoculars, no aim
    if (g.bLeftTrigger > 30) g_aimHeldTick = GetTickCount();  // scope9: the game's aim (left trigger) is held
    if (EngineScopeSteadyOnly()) g.bLeftTrigger = 0;  // steadyonly1: the trigger only steadies the rifle scope (no game zoom)
    if (brt > g.bRightTrigger) g.bRightTrigger = brt;
    {   // turtrig1 (DrGonzaga 21 Nov: "sometimes I need to grab the turret with right grip before they work ... make them
        // work off right trigger alone"): bd107 log, a ground turret: trigger pressed 0.45 s after getting on (before the
        // game had switched to its mounted-gun controls) and held 2.5 s with no shot; the next press fired. On a mounted
        // gun, a trigger held 1.5 s with no shot is let go for 80 ms so the game sees a fresh press (at most every 2 s).
        static DWORD s_downT, s_relUntil, s_pulseT;
        static bool s_was;
        static int s_n;
        DWORD now = GetTickCount();
        bool down = g.bRightTrigger > 30 && EngineTurret() && !MenuOpen();
        if (down && !s_was) s_downT = now;
        s_was = down;
        DWORD since = s_downT;
        if (down && (LONG)(g_lastShotT - since) < 0 && now - since > 1500 && now - s_pulseT > 2000) {
            s_relUntil = now + 80, s_pulseT = now;
            if (s_n < 30) ++s_n, Log("turret: trigger held %lu ms with no shot: let go for a moment so the game sees a fresh press", now - since);
        }
        if (down && (LONG)(s_relUntil - now) > 0) g.bRightTrigger = 0;
        if (down && (LONG)(g_lastShotT - since) >= 0) s_pulseT = now;  // firing: leave it alone
    }
    FireNoteTrigger(g.bRightTrigger > 30 && !MenuOpen());
    g.sThumbLX = Bigger(g.sThumbLX, Axis(p.lx));
    g.sThumbLY = Bigger(g.sThumbLY, Axis(p.ly));
    g.sThumbRX = Bigger(g.sThumbRX, Axis(p.rx));
    g.sThumbRY = Bigger(g.sThumbRY, Axis(p.ry));
}

// swing1: a fast swing of the right hand presses melee (right stick click,
// the game's melee on a pad) for a moment. Speed is measured over at least
// 30 ms of the controller's movement; a short pause stops one swing counting twice.
static bool g_swingOn = true;
static float g_swingSpeed = 2.5f;  // m/s
bool *InputSwingOn() { return &g_swingOn; }
float *InputSwingSpeed() { return &g_swingSpeed; }
static bool SwingMelee(const VrPad &p) {
    static float s_pos[8][3];
    static LONGLONG s_t[8];
    static int s_n;
    static LONGLONG s_until, s_quiet;
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
    LONGLONG t = now.QuadPart, ms = fq.QuadPart / 1000;
    if (!g_swingOn || !p.handValid[1]) return false;
    const float *h = p.handP[1];
    int last = (s_n + 7) & 7;
    if (!s_n || memcmp(s_pos[last], h, 12)) {  // a new pose
        memcpy(s_pos[s_n & 7], h, 12), s_t[s_n & 7] = t, ++s_n;
        if (s_n >= 8 && t > s_quiet) {
            for (int back = 1; back < 8; ++back) {
                int k = (s_n - 1 - back) & 7;
                LONGLONG dt = t - s_t[k];
                if (dt < 30 * ms) continue;
                if (dt > 120 * ms) break;
                float d[3] = {h[0] - s_pos[k][0], h[1] - s_pos[k][1], h[2] - s_pos[k][2]};
                float v = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / (float(dt) / fq.QuadPart);
                if (v > g_swingSpeed) {
                    s_until = t + 120 * ms, s_quiet = t + 600 * ms;
                    Log("input: swing %.1f m/s: melee", v);
                }
                break;
            }
        }
    }
    return t < s_until;
}

// crouch2: on foot, pulling the right stick down hard (towards you) presses
// crouch (B on a pad, a toggle in the game) once per pull: pull to crouch,
// pull again to stand (A also stands up, as in the game).
static bool g_crouchOn = true;
bool *InputCrouchOn() { return &g_crouchOn; }
static bool StickCrouch(const VrPad &p, bool onFoot) {
    static bool s_armed = true;
    static DWORD s_until;
    DWORD t = GetTickCount();
    if (p.ry > -0.4f) s_armed = true;
    if (g_crouchOn && onFoot && s_armed && p.ry < -0.85f && fabsf(p.rx) < 0.6f) {
        s_armed = false, s_until = t + 120;
        LogV("input: right stick pulled down: crouch / stand");
    }
    return t < s_until;
}

static DWORD WINAPI HookGetState(DWORD user, XINPUT_STATE *st) {
    DWORD r;
    if (!g_verboseLog) {
        r = g_origGetState(user, st);
    } else {
        // review1: how long the real pad read takes (a missing pad can be slow), detailed log only
        static LARGE_INTEGER s_fq;
        static volatile LONG s_n, s_usSum, s_usMax, s_last;
        LARGE_INTEGER a, b;
        if (!s_fq.QuadPart) QueryPerformanceFrequency(&s_fq);
        QueryPerformanceCounter(&a);
        r = g_origGetState(user, st);
        QueryPerformanceCounter(&b);
        LONG us = (LONG)((b.QuadPart - a.QuadPart) * 1000000 / s_fq.QuadPart);
        InterlockedIncrement(&s_n), InterlockedExchangeAdd(&s_usSum, us);
        if (us > s_usMax) s_usMax = us;
        LONG now = (LONG)GetTickCount(), last = s_last;
        if ((DWORD)(now - last) > 10000 && InterlockedCompareExchange(&s_last, now, last) == last) {
            LONG n = InterlockedExchange(&s_n, 0), sum = InterlockedExchange(&s_usSum, 0), mx = InterlockedExchange(&s_usMax, 0);
            if (n) Log("input: real pad reads in 10 s: %ld, average %.3f ms, longest %.3f ms (last: pad %lu %s)", n, sum / 1000.0 / n, mx / 1000.0, user, r == ERROR_SUCCESS ? "connected" : "not connected");
        }
    }
    VrPad p;
    if (user != 0 || !st) return r;
    if (!XrGetPad(&p)) {
        // pad1: report pad 1 as connected (idle) even before the Touch
        // controllers are live, so the game never decides there's no pad and
        // stops listening (DrGonzaga needed a real Xbox pad plugged in).
        if (r != ERROR_SUCCESS) memset(st, 0, sizeof *st), r = ERROR_SUCCESS;
        return r;
    }
    if (r != ERROR_SUCCESS) memset(st, 0, sizeof *st);
    MenuInput(p);
    // zoom1 (DrGonzaga 6 Nov): while scoped, A zooms in and X zooms out a step; the
    // game doesn't get them. A press that started scoped stays ours until let go.
    {
        // zoom2 (DrGonzaga 6 Nov): X zooms out, not B.
        static bool s_a, s_x, s_ownA, s_ownX;
        bool sc = g_abZoom && EngineScopePictureActive() && !MenuOpen();  // steadyonly1: also the auto rifle scope
        if (p.a && !s_a && sc) s_ownA = true, ScopeZoomStep(+1);
        if (p.x && !s_x && sc) s_ownX = true, ScopeZoomStep(-1);
        s_a = p.a, s_x = p.x;
        if (!p.a) s_ownA = false;
        if (!p.x) s_ownX = false;
        if (s_ownA) p.a = false;
        if (s_ownX) p.x = false;
    }
    // bow1p: the physical bow (left grip holds the bow) owns the right trigger; its draw holds the game's fire.
    const bool cy = CyberEyeInput(p);  // cybereye1: left grip at the head is ours; while on, no bow / two-handed hold
    SlowmoInput(p);  // slowbtn1: left grip at the floating clock is ours
    g_lgripRaw = p.lgrip > 0.5f;  // bowmenu1
    HandsLeftGrip(g_lgripRaw);  // hands24
    bool bowRt = false, bow = !cy && BowInput(p, &bowRt);
    if (bow) p.rt = 0, p.lgrip = 0;  // the right grip still reaches the holsters (the arrow spot is skipped there)
    if (!cy && TwoHandInput(p)) p.lgrip = 0;  // twohand1: left grip holding the gun, not the weapon wheel
    // map1x: on foot the left grip alone does nothing (no weapon wheel) unless chosen;
    // left grip + Menu (the map) and vehicles stay as they were.
    else if (!g_lgripWheel && !p.menu && EngineOnFoot() && !EngineInVehicle() && !MenuOpen()) p.lgrip = 0;
    bool up = StickUp(p, EngineOnFoot() && !EngineTurret() && p.lgrip <= 0.5f && !MenuOpen());  // turmove1: not on a mounted gun
    MoveByLeftHand(p);  // lasers1
    Mix(p, st->Gamepad, up);
    if (bow) st->Gamepad.bRightTrigger = bowRt ? 255 : 0;
    // wheel1: while LB (left grip: the game's weapon wheel) is held, the game
    // gets the right stick exactly as it is, to pick a weapon: no look
    // steering, no body turn, no swing melee.
    bool wheel = (st->Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) && !MenuOpen();
    static bool s_wheel;
    g_wheelHeld = wheel;  // hud: cut-outs back on the main panel while the wheel is up
    if (wheel != s_wheel) {
        s_wheel = wheel;
        LogV("input: weapon wheel %s", wheel ? "held: right stick to the game" : "let go");
    }
    // quiver11: no melee at all with the bow in the left hand (reaching for arrows set it off). The
    // press itself had slipped into this comment (bow1p), so the if below took the roomscale lines
    // instead: roomscale only walked during a swing, and the body was left behind otherwise.
    if (SwingMelee(p) && !cy && !MenuOpen() && !wheel && BowState() < 0 && !HolsterSwingBlocked()) st->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    // room1: your own left stick always wins; otherwise roomscale may walk the body.
    if (fabsf(p.lx) > 0.15f || fabsf(p.ly) > 0.15f) EngineUserMoved();
    else if (!wheel && !MenuOpen()) EngineRoomStick(&st->Gamepad.sThumbLX, &st->Gamepad.sThumbLY);
    if (StickCrouch(p, EngineOnFoot() && !EngineTurret() && !wheel && !MenuOpen()))  // turmove1
        st->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    if (EngineCrouchPress() && EngineOnFoot() && !MenuOpen()) st->Gamepad.wButtons |= XINPUT_GAMEPAD_B;  // realcrouch1
    if (CyberEyeDpadUp() && !MenuOpen()) st->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;  // cybereye1: the game's binoculars
    if (!MenuOpen()) st->Gamepad.wButtons |= HolsterHeartButton();  // heart1: the cyber heart throw
    {   // ads1: slower on foot with the gun at your face or crouched
        float k = 1, cap = 1;
        // move2 (DrGonzaga 8 Nov: could still run with the gun up): bringing the gun to the
        // face lets go of the stick for 150 ms, which ends the game's sprint, then walks slower.
        // adsmove1 (DrGonzaga 9 Nov: walking, he stopped first and then slow-walked): only
        // when the game is sprinting, i.e. sprint (stick click) was pressed since the stick
        // last came back to the centre. Plain walking goes straight into the slow walk.
        static bool s_face, s_sprint;
        static DWORD s_faceT;
        bool moving = fabsf(p.lx) > 0.15f || fabsf(p.ly) > 0.15f;
        if (!moving) s_sprint = false;
        else if ((st->Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && !s_face) s_sprint = true;
        bool face = g_slow[0] > 0.5f && StockGunAtFace() && EngineOnFoot() && !EngineInVehicle();
        if (face && !s_face) s_faceT = s_sprint ? GetTickCount() : GetTickCount() - 1000, s_sprint = false;
        s_face = face;
        if (face && GetTickCount() - s_faceT < 150) {
            st->Gamepad.sThumbLX = st->Gamepad.sThumbLY = 0;
            st->Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_THUMB;
        }
        if (EngineOnFoot() && !EngineInVehicle() && (fabsf(p.lx) > 0.15f || fabsf(p.ly) > 0.15f)) {  // your own stick only (not roomscale)
            // adscrouch1 (DrGonzaga 8 Nov): the game caps crouched walking well below full stick
            // (fWalkingMaxSpeedCrouch), so scaling a full stick did nothing while crouched.
            // Crouched and slowed, first bring the stick down to that cap, then slow from there
            // (plain crouched walking and crouch sprint are left alone).
            if (g_slow[0] > 0.5f && StockGunAtFace()) k *= *StockAdsSpeed();
            if (EngineCrouched()) k *= g_slow[1];
            if (k < 0.999f && EngineCrouched()) {
                float m = sqrtf(p.lx * p.lx + p.ly * p.ly);
                if (m > g_crouchCap && m > 0.01f) cap = g_crouchCap / (m > 1 ? 1 : m);
            }
        }
        if (k < 0.999f) {
            if (k < 0.05f) k = 0.05f;
            float ks = k * cap;
            st->Gamepad.sThumbLX = (SHORT)(st->Gamepad.sThumbLX * ks), st->Gamepad.sThumbLY = (SHORT)(st->Gamepad.sThumbLY * ks);
            if (k < 0.9f) st->Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_THUMB;  // no sprinting
        }
    }
    if (wheel) {
        EngineSetRightX(0), EngineSetRightY(0);
    } else {
        // The headset does up/down. On foot the render camera is levelled anyway, so
        // the stick's up/down only tilts the gun (aimfix1): it lets you aim at things
        // above and below until motion controls aim the gun.
        // turret1 (DrGonzaga 9 Nov): except on a mounted gun, where up/down tilts the gun.
        if (EngineHorizonLock() && !EngineOnFoot() && !EngineTurret()) st->Gamepad.sThumbRY = 0;
        if ((BowState() >= 0 || BowClassic()) && st->Gamepad.sThumbRY > 0) st->Gamepad.sThumbRY = 0;  // quiver12: stick up does nothing with the bow (as before)
        EngineSetRightX(st->Gamepad.sThumbRX);
        EngineSetRightY(Axis(p.ry));  // snap1: the stick's own up/down (sThumbRY may be zeroed above)
        // lookaim1: on foot, the stick turns our body direction and the game gets
        // the stick that steers its look to the controller (or head).
        if (EngineAimHeadLive()) EngineSteerStick(&st->Gamepad.sThumbRX, &st->Gamepad.sThumbRY);
    }
    // Debug menu open (menu.cpp): the controllers drive it, the game gets nothing,
    // until every button is let go after it closes.
    static bool menuHold;
    if (MenuOpen()) menuHold = true;
    if (menuHold) {
        const XINPUT_GAMEPAD &g = st->Gamepad;
        bool any = g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30;
        if (!MenuOpen() && !any) menuHold = false;
        else {
            memset(&st->Gamepad, 0, sizeof st->Gamepad);
            // heartadj1: the heart held for adjusting from the menu: the game still gets its throw button, nothing else
            int o = s_throwOut;
            if (HolsterHeartAdjHold() && o >= 0) {
                if (o == kOutLT) st->Gamepad.bLeftTrigger = 255;
                else if (o == kOutRT) st->Gamepad.bRightTrigger = 255;
                else if (o != kOutHud && o != kOutNone) st->Gamepad.wButtons |= kOutBits[o];
            }
        }
    }
    // Games may skip a state whose packet number hasn't moved.
    static DWORD packet;
    static XINPUT_GAMEPAD last;
    if (memcmp(&last, &st->Gamepad, sizeof last)) ++packet;
    last = st->Gamepad;
    st->dwPacketNumber += packet;
    static bool logged;
    if (!logged) logged = true, Log("input: Touch controllers feeding pad 1%s", r == ERROR_SUCCESS ? " (with a real pad)" : "");
    return ERROR_SUCCESS;
}

// rumble1: the game's rumble for pad 1 goes to the Touch controllers (xr.cpp).
// A real pad only rumbles too if [input] RumbleRealPad is 1 (it's usually on the desk).
typedef DWORD(WINAPI *SetStateFn)(DWORD, XINPUT_VIBRATION *);
static SetStateFn g_origSetState;
static volatile WORD g_motor[2];
static volatile DWORD g_motorTick;
static float g_rumbleStrength = 1.0f;
static bool g_rumbleRealPad;
float *InputRumbleStrengthPtr() { return &g_rumbleStrength; }
bool *InputRumbleRealPadPtr() { return &g_rumbleRealPad; }
float InputRumbleStrength() { return g_rumbleStrength; }
void InputRumble(float *heavy, float *light) {
    bool fresh = GetTickCount() - g_motorTick < 3000;  // the game always sends a stop, but just in case
    *heavy = fresh ? g_motor[0] / 65535.f : 0, *light = fresh ? g_motor[1] / 65535.f : 0;
}
static DWORD WINAPI HookSetState(DWORD user, XINPUT_VIBRATION *v) {
    if (user == 0 && v) {
        g_motor[0] = v->wLeftMotorSpeed, g_motor[1] = v->wRightMotorSpeed, g_motorTick = GetTickCount();
        static bool logged;
        if (!logged && (v->wLeftMotorSpeed || v->wRightMotorSpeed)) logged = true, Log("input: game rumble seen (heavy %u, light %u): sent to the Touch controllers", v->wLeftMotorSpeed, v->wRightMotorSpeed);
        if (!g_rumbleRealPad) {
            XINPUT_VIBRATION off = {};
            g_origSetState(user, &off);
            return ERROR_SUCCESS;
        }
    }
    DWORD r = g_origSetState(user, v);
    return user == 0 ? ERROR_SUCCESS : r;
}

// Point an import of a module, by ordinal, at our function.
static bool PatchOrdinal(HMODULE mod, const char *dll, WORD ordinal, void *hook, void **orig) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char *)(base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *funcs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) || IMAGE_ORDINAL(names->u1.Ordinal) != ordinal) continue;
            *orig = (void *)funcs->u1.Function;
            return PatchBytes(&funcs->u1.Function, &hook, 4);
        }
    }
    return false;
}

static DWORD WINAPI WaitForGame(LPVOID) {
    HMODULE fc3 = nullptr;
    for (int i = 0; i < 180000 && !(fc3 = GetModuleHandleW(L"FC3_d3d11.dll")); ++i) Sleep(1);
    if (!fc3) return 0;
    ScanlinesInstall(fc3);  // scan1: first, before the game registers its classes
    OnlineInstall(fc3);  // online1: before the game first asks for its online service
    bool ok = PatchOrdinal(fc3, "XINPUT1_3.dll", 2, (void *)HookGetState, (void **)&g_origGetState);
    Log("input: XInputGetState %s", ok ? "hooked (Touch controllers act as pad 1)" : "not found; controllers off");
    bool okr = PatchOrdinal(fc3, "XINPUT1_3.dll", 3, (void *)HookSetState, (void **)&g_origSetState);
    Log("input: XInputSetState %s", okr ? "hooked (game rumble to the Touch controllers)" : "not found; no rumble");
    return 0;
}

void InputStart() { CloseHandle(CreateThread(nullptr, 0, WaitForGame, nullptr, 0, nullptr)); }
