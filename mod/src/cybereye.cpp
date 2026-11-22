// cybereye1 (DrGonzaga 13 Nov, Blood Dragon): the cyber-eye (the game's binoculars, D-pad up) from a
// gesture. Left hand up beside the head:
//   left grip pressed and let go  = the objective marker shows for 10 s (it is hidden otherwise);
//   left grip held (about 0.5 s)  = cyber-eye on / off, as a switch (we press D-pad up for the game).
// While it is on the picture is the game's own flat one on a smaller screen that stays in front of
// the eyes (xr.cpp), the game's look follows the head, heavily smoothed (engine.cpp AimMatrix), the
// triggers go to the game as they are (right = zoom in, left = zoom out), and thin scan lines are
// drawn over the screen (the game's own stay off: they can't be switched live).
// The game isn't asked whether its binoculars are really up: the switch is ours. If the two ever get
// out of step (the game refused, or dropped them itself), the log shows the game's field of view
// while on ("cybereye: the game's view ..."), for an automatic check later.
#include "fc3vr.h"
#include <math.h>
#include <stdio.h>
#include <wchar.h>

namespace {
bool g_on = true;
float g_reach = 0.18f;    // [cybereye] HeadReach: m from the left temple
float g_hold = 0.45f;     // [cybereye] HoldSeconds
float g_scrW = 0.9f, g_scrD = 1.0f;  // [cybereye] ScreenWidth / ScreenDistance (m)
float g_smooth = 0.30f;   // [cybereye] Smoothing (s)
float g_scan = 0.35f;     // [cybereye] Scanlines (0 = none .. 1 = black)
float g_objSecs = 10.0f;  // [cybereye] ObjectiveSeconds
bool g_level = true;      // [cybereye] Level: the screen turns against the head's tilt (level horizon)
// cyhold1 (bd83; DrGonzaga 15 Nov: "currently it's hold to turn on/off, I wanted it to be on only so long as left grip
// is held near the headset"): on while the grip that began at the head stays held; letting go switches it off.
bool g_holdKeep = true;   // [cybereye] HoldToKeep (0 = the old switch: hold once for on, again for off)
DWORD g_onAt;             // when it went on
DWORD g_offAt;            // letting go so soon after it came on that the two D-pad presses would run together: off at this time
volatile bool g_cy;
volatile DWORD g_dpadUntil, g_buzzUntil, g_objUntil;
bool g_objOurs;

float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    if (!GetPrivateProfileStringW(L"cybereye", key, L"", buf, 32, path) || !buf[0]) return def;
    return (float)_wtof(buf);
}
// The left hand beside the head: within reach of a point at the left temple.
bool AtHead() {
    float hm[16], head[3], m[16], hand[3];
    if (!EngineHeadPose(hm, head) || !HeadtrackHandRaw(0, m, hand)) return false;
    float d2 = 0, h2 = 9;
    const float side = *VrLeftHanded() ? -0.07f : 0.07f;  // lefty1: the right temple, for the right hand
    for (int i = 0; i < 3; ++i) {
        float c = head[i] - hm[i * 4 + 0] * side, d = hand[i] - c;
        d2 += d * d;
    }
    // not while the left hand is at the gun (a rifle or pistol held up to the eye with both hands)
    float rm[16], rh[3];
    if (HeadtrackHandRaw(1, rm, rh)) h2 = (rh[0] - hand[0]) * (rh[0] - hand[0]) + (rh[1] - hand[1]) * (rh[1] - hand[1]) + (rh[2] - hand[2]) * (rh[2] - hand[2]);
    return d2 < g_reach * g_reach && h2 > 0.22f * 0.22f;
}
}  // namespace

void CyberEyeLoad() {
    g_on = LoadF(L"On", 1) != 0;
    g_reach = LoadF(L"HeadReach", g_reach), g_hold = LoadF(L"HoldSeconds", g_hold);
    g_scrW = LoadF(L"ScreenWidth", g_scrW), g_scrD = LoadF(L"ScreenDistance", g_scrD);
    g_smooth = LoadF(L"Smoothing", g_smooth), g_scan = LoadF(L"Scanlines", g_scan);
    g_objSecs = LoadF(L"ObjectiveSeconds", g_objSecs);
    g_level = LoadF(L"Level", 1) != 0;
    g_holdKeep = LoadF(L"HoldToKeep", 1) != 0;  // cyhold1
    Log("cybereye: left hand at the head %s (reach %.2f m, hold %.2f s, screen %.2f m wide at %.2f m, smoothing %.2f s, scan lines %.2f, objective %.0f s)",
        g_on ? "on" : "off ([cybereye] On)", g_reach, g_hold, g_scrW, g_scrD, g_smooth, g_scan, g_objSecs);
}

// cybermenu1 (DrGonzaga 15 Nov: "adjustable smoothing and size for cyber-eye"): the settings on the menu
bool *CyberEyeLevel() { return &g_level; }
bool *CyberEyeHoldKeep() { return &g_holdKeep; }  // cyhold1
float *CyberEyeSmoothPtr() { return &g_smooth; }
float *CyberEyeWidthPtr() { return &g_scrW; }
float *CyberEyeDistPtr() { return &g_scrD; }
float *CyberEyeScanPtr() { return &g_scan; }
void CyberEyeSave() {
    wchar_t path[MAX_PATH], b[32];
    IniPath(path);
    const struct { const wchar_t *k; float v; } kv[4] = {{L"Smoothing", g_smooth}, {L"ScreenWidth", g_scrW}, {L"ScreenDistance", g_scrD}, {L"Scanlines", g_scan}};
    for (auto &e : kv) swprintf(b, 32, L"%.3f", e.v), WritePrivateProfileStringW(L"cybereye", e.k, b, path);
    WritePrivateProfileStringW(L"cybereye", L"Level", g_level ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"cybereye", L"HoldToKeep", g_holdKeep ? L"1" : L"0", path);  // cyhold1
}
bool CyberEyeOn() { return g_cy; }
// cyoff1 (DrGonzaga 22 Nov: "when closing the binoculars, if I accidentally press the left trigger, a scope window
// appears ... It just flashes"): the game keeps its binocular zoom for a moment after we let go, so the left trigger
// then looked like scoping. True while on and for 0.8 s after: no scope picture and no aim from the left trigger.
bool CyberEyeRecent() {
    static DWORD s_last;
    DWORD t = GetTickCount();
    if (g_cy) s_last = t;
    return s_last && t - s_last < 800;
}
float CyberEyeSmoothing() { return g_smooth < 0 ? 0 : g_smooth; }
float CyberEyeScanlines() { return g_scan < 0 ? 0 : g_scan > 1 ? 1 : g_scan; }
void CyberEyeScreen(float *w, float *d) {
    *w = g_scrW < 0.2f ? 0.2f : g_scrW > 3 ? 3 : g_scrW;
    *d = g_scrD < 0.3f ? 0.3f : g_scrD > 5 ? 5 : g_scrD;
}
bool CyberEyeDpadUp() { return (LONG)(g_dpadUntil - GetTickCount()) > 0; }
float CyberEyeBuzz() { return (LONG)(g_buzzUntil - GetTickCount()) > 0 ? 0.6f : 0.0f; }

// Input thread, each pad read, before anything else looks at the left grip. True while the
// cyber-eye is on.
bool CyberEyeInput(VrPad &p) {
    static bool s_grip, s_own, s_fired;
    static DWORD s_t0;
    DWORD now = GetTickCount();
    // the objective marker goes back to hidden
    if (g_objOurs && (LONG)(g_objUntil - now) <= 0) g_objOurs = false, g_hideObjIcon = 1, Log("cybereye: objective marker hidden again");
    bool play = g_on && !EngineInVehicle() && !EngineCutscene() && !MenuOpen();
    if (g_cy && (!g_on || EngineInVehicle() || EngineCutscene())) {
        g_cy = false, g_offAt = 0;
        Log("cybereye: off (a vehicle or cutscene began)");
    }
    if (g_cy && g_offAt && (LONG)(now - g_offAt) >= 0) {  // cyhold1: the grip was let go
        g_cy = false, g_offAt = 0, g_dpadUntil = now + 150, g_buzzUntil = now + 60;
        Log("cybereye: left grip let go: cyber-eye OFF: back to VR (D-pad up pressed for the game)");
    }
    bool grip = p.lgrip > (s_grip ? 0.35f : 0.6f);
    bool press = grip && !s_grip, release = !grip && s_grip;
    s_grip = grip;
    if (press && play && EngineOnFoot() && !GamePaused() && AtHead()) s_own = true, s_fired = false, s_t0 = now;
    if (s_own) {
        p.lgrip = 0;  // ours: not the bow, the two-handed hold or the weapon wheel
        if (grip && !s_fired && now - s_t0 >= (DWORD)(g_hold * 1000)) {
            s_fired = true;
            if (g_holdKeep && g_cy && g_offAt) g_offAt = 0;  // cyhold1: held again before it had gone off: it just stays on
            else {
                g_cy = !g_cy;
                g_onAt = now, g_offAt = 0;
                g_dpadUntil = now + 150, g_buzzUntil = now + 90;
                Log("cybereye: left grip held at the head: cyber-eye %s (D-pad up pressed for the game)", g_cy ? "ON: flat screen, the look follows the head" : "OFF: back to VR");
            }
        }
        if (release) {
            // cyhold1: on only while held. Off no sooner than 450 ms after it came on, so the game sees two presses.
            if (s_fired && g_cy && g_holdKeep) g_offAt = (LONG)(now - g_onAt) >= 450 ? now | 1 : (g_onAt + 450) | 1;
            if (!s_fired && !g_cy) {
                if (g_hideObjIcon || g_objOurs) {
                    g_objOurs = true, g_hideObjIcon = 0, g_objUntil = now + (DWORD)(g_objSecs * 1000), g_buzzUntil = now + 40;
                    Log("cybereye: left grip pressed at the head: objective marker shown for %.0f s", g_objSecs);
                } else {
                    Log("cybereye: left grip pressed at the head: the objective marker is already set to show");
                }
            }
            s_own = false;
        }
    }
    return g_cy;
}
