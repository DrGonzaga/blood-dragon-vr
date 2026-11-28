// slowmo1 (DrGonzaga 15 Nov, Blood Dragon: "a left menu button with a clock icon, that gives you a slow
// motion boost ... head/hands move at normal speed. movement of body, bullets, bullet RPM, zoom speed for
// scopes, etc all slowmo"; "Can we add an option for 25 percent, 50 percent speed?").
//
// The game's clock (object at BD+0x11846744, ticked by BD+0x1005e140) turns each frame's real time into
// game time as: game dt = debugGlobalScale * timeScale (+0x60) * real dt. The game's own slow motion
// (script SetSlowMotion, outpost capture, a few scripted moments) sets +0x60 and puts it back to 1.0 when
// done, so it would end ours early. The other factor, the "debug_GlobalTimeScale" float at
// BD+0x11846778 ("Global time scale (slowmo factor for debugging purposes.)"), is set to 1.0 at start and
// never written by the game again: that is the one written here. Everything that runs on game time
// slows (walking, enemies, projectiles, fire rate, reloads, the game's own aim zoom); the game's own
// real-time counters (+0x40 / +0x50) and everything the mod does on its own clock (head, hands, aim,
// menus, haptics) don't.
//
// A right grip on the clock (the wrist menu's sixth icon) switches it on or off. It eases in and out
// (Ramp, real seconds) and ends by itself after Seconds (0 = until touched again), and at once for a
// cutscene, the pause menu, or a load. Before the first write the two multiplies in the game's tick are
// checked to read exactly that float; if not, it stays off and the log says so.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
bool g_on = true;      // [slowmo] On: the clock is on the wrist menu
float g_speed = 0.5f;  // [slowmo] Speed: game speed while on (0.25 or 0.5)
float g_secs = 10;     // [slowmo] Seconds: how long one press lasts (0 = until touched again)
float g_ramp = 0.3f;   // [slowmo] Ramp: real seconds to ease in / out
volatile LONG g_want;  // 1 = on (input thread writes, draw reads)
volatile DWORD g_startT;
float g_cur = 1;               // input thread: the value last written
volatile float *g_scale;       // the game's debug global time scale
int g_found;                   // 0 not looked yet, 1 found, -1 not found
DWORD g_lastT, g_seenT;
// slowbtn1 (bd82; DrGonzaga 15 Nov: "can we put the slow motion button/icon down near the map area as a floating
// button, and have left grip to activate it, so we'll remove it from the wrist menu? and can we make the
// controller give a few vibrates when it starts and finishes?"). The clock floats beside the minimap's own panel
// (offset in metres: to the right of it as you face it, up, towards you); the left hand at it + left grip =
// on / off. [slowmo] Place: 0 by the map (default), 1 the wrist menu as before (right grip).
int g_place = 0;
float g_btn[3] = {-0.14f, 0.0f, 0.0f};  // [slowmo] ButtonRight / ButtonUp / ButtonTowards
float g_btnSize = 0.035f;               // [slowmo] ButtonSize (radius, m)
float g_btnReach = 0.09f;               // [slowmo] ButtonReach (m)
volatile bool g_btnHot;                 // the left hand is at it
volatile DWORD g_buzzT;                 // when slow motion last started / ended
volatile int g_buzzKind;                // 1 started, 2 ended

void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"slowmo", key, buf, path);
}
float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32], d[32];
    IniPath(path);
    swprintf(d, 32, L"%.3f", def);
    GetPrivateProfileStringW(L"slowmo", key, d, buf, 32, path);
    return (float)_wtof(buf);
}

bool Readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

void Find() {
    g_found = -1;
    if (!g_bloodDragon) { Log("slowmo: only in Blood Dragon (the Far Cry 3 address is not known)"); return; }
    BYTE *mod = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    if (!mod) { Log("slowmo: FC3_d3d11.dll not loaded; slow motion off"); return; }
    BYTE *f = BdAt(mod, 0x1846778);
    const DWORD sites[2] = {0x5e218, 0x5e231};  // movss xmm1, [debugGlobalScale] (f3 0f 10 0d <addr>), the clock tick
    for (DWORD s : sites) {
        BYTE *c = BdAt(mod, s);
        if (!Readable(c, 8) || c[0] != 0xf3 || c[1] != 0x0f || c[2] != 0x10 || c[3] != 0x0d || *(BYTE **)(c + 4) != f) {
            Log("slowmo: the game's clock tick does not match at BD+%08lx; slow motion off", (unsigned long)(0x10000000 + s));
            return;
        }
    }
    if (!Readable(f, 4)) { Log("slowmo: the game's time scale cannot be read; slow motion off"); return; }
    float v = *(volatile float *)f;
    if (!(v > 0.05f && v < 20.f)) { Log("slowmo: the game's time scale reads %.3f; slow motion off", v); return; }
    MEMORY_BASIC_INFORMATION mi;  // the game's .data: writable already
    if (!VirtualQuery(f, &mi, sizeof mi) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
        Log("slowmo: the game's time scale is not writable (protect %lx); slow motion off", (unsigned long)mi.Protect);
        return;
    }
    g_scale = (volatile float *)f, g_found = 1;
    Log("slowmo: the game's global time scale found (BD+11846778, now %.2f); clock on the wrist menu %s, speed %.0f%%, %s",
        v, g_on ? "shown" : "hidden", g_speed * 100, g_secs > 0 ? "ends by itself" : "until touched again");
}

void Stop(const char *why) {
    if (InterlockedExchange(&g_want, 0)) g_buzzT = GetTickCount(), g_buzzKind = 2, SlowFxEvent(2), Log("slowmo: off (%s)", why);  // slowfx1
}
}  // namespace

// slowbtn1: the floating clock's centre (recentred XR frame): beside the minimap panel, along level axes that turn
// with where the head faces (as the body-anchored panels do).
bool SlowmoButtonPos(float p[3]) {
    float c[3], hm[16], head[3];
    if (!HudPanelPlace(0, c) || !EngineHeadPose(hm, head)) return false;
    float f[3] = {-hm[2], 0, -hm[10]};
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (l < 1e-3f) f[0] = 0, f[2] = -1, l = 1;
    f[0] /= l, f[2] /= l;
    const float r[3] = {-f[2], 0, f[0]};
    const float side = *VrLeftHanded() ? -g_btn[0] : g_btn[0];  // lefty1: the other side of the minimap
    for (int i = 0; i < 3; ++i) p[i] = c[i] + r[i] * side - f[i] * g_btn[2];
    p[1] += g_btn[1];
    return true;
}
bool SlowmoOnWrist() { return SlowmoShown() && g_place == 1; }
bool SlowmoByMap() { return SlowmoShown() && g_place == 0; }
int *SlowmoPlace() { return &g_place; }
float *SlowmoButton(int i) { return i >= 0 && i < 3 ? &g_btn[i] : i == 3 ? &g_btnSize : &g_btnReach; }
// xr.cpp haptics, both controllers: three quick ticks as it starts, two longer ones as it ends.
float SlowmoBuzz() {
    int kind = g_buzzKind;
    if (!kind) return 0;
    DWORD t = GetTickCount() - g_buzzT;
    if (kind == 1) return t < 420 && t % 140 < 70 ? 0.7f : 0.0f;
    return t < 560 && t % 280 < 170 ? 0.5f : 0.0f;
}
// Input thread, each pad read, before the bow / two-handed hold / weapon wheel look at the left grip.
void SlowmoInput(VrPad &p) {
    static bool s_grip, s_own;
    bool grip = p.lgrip > (s_grip ? 0.35f : 0.6f), press = grip && !s_grip;
    s_grip = grip;
    bool hot = false;
    float b[3], m[16], hp[3];
    if (SlowmoByMap() && !CyberEyeOn() && !MenuOpen() && !GamePaused() && !EngineCutscene() && (EngineOnFoot() || EngineInVehicle()) &&
        SlowmoButtonPos(b) && HeadtrackHandRaw(0, m, hp)) {
        float d[3] = {hp[0] - b[0], hp[1] - b[1], hp[2] - b[2]};
        hot = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < g_btnReach * g_btnReach;
    }
    g_btnHot = hot;
    if (press && hot) s_own = true, SlowmoToggle();
    if (!grip) s_own = false;
    if (s_own) p.lgrip = 0;  // ours: not the bow, the two-handed hold or the weapon wheel
}
// Render thread, each eye, after the wrist marks (hook.cpp).
static bool g_showSecs = false;  // slowglow1 (DrGonzaga 16 Nov): no seconds counter, the cues are enough
bool *SlowmoShowSeconds() { return &g_showSecs; }

void SlowmoDraw(IDXGISwapChain *sc) {
    if (!SlowmoByMap() || CyberEyeOn() || GamePaused() || EngineCutscene() || EngineScoped() || !(EngineOnFoot() || EngineInVehicle())) return;
    float b[3];
    EngineView v;
    if (!SlowmoButtonPos(b) || !EngineLastView(&v) || !v.sixdof) return;
    const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
    auto world = [&](const float *p, float *w) {
        float o[3] = {p[0] / v.scale, p[1] / v.scale, p[2] / v.scale};
        for (int i = 0; i < 3; ++i) w[i] = v.base[i] + fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2];
    };
    const bool hot = g_btnHot, on = SlowmoActive();
    const float R = g_btnSize * (hot ? 1.3f : on ? 1.2f : 1.0f);
    LaserMark m[3];
    int n = 0;
    LaserMark &d = m[n++];
    world(b, d.pos);
    d.radius = R / v.scale, d.icon = 0, d.val = 0;
    d.rgba[0] = on ? 1.0f : 0.75f, d.rgba[1] = on ? 0.75f : 0.9f, d.rgba[2] = on ? 0.35f : 1.0f, d.rgba[3] = hot ? 0.5f : on ? 0.4f : 0.25f;
    LaserMark &k = m[n++];
    world(b, k.pos);
    k.radius = R * 0.8f / v.scale, k.icon = 10, k.val = 0;
    if (on) k.rgba[0] = 1.0f, k.rgba[1] = 0.75f, k.rgba[2] = 0.3f;
    else k.rgba[0] = 0.2f, k.rgba[1] = 1.0f, k.rgba[2] = 1.0f;
    k.rgba[3] = hot || on ? 1.0f : 0.7f;
    float left = SlowmoLeft();
    if (left >= 0 && g_showSecs) {  // slowglow1: off by default
        LaserMark &c = m[n++];
        float q[3] = {b[0], b[1] - R * 0.95f, b[2]};
        world(q, c.pos);
        c.radius = R * 0.55f / v.scale, c.icon = 8, c.val = ceilf(left > 99 ? 99 : left);
        c.rgba[0] = 1.0f, c.rgba[1] = 0.8f, c.rgba[2] = 0.4f, c.rgba[3] = 1.0f;
    }
    HolsterNeonMarks(m, n);  // slowring1: same pink ring and cyan outline icon as the wrist menu buttons
    LaserDrawMarks(sc, m, n);
}

void SlowmoLoad() {
    g_on = LoadF(L"On", 1) != 0;
    float s = LoadF(L"Speed", 0.5f);
    g_speed = s < 0.05f ? 0.05f : s > 1.f ? 1.f : s;  // slowmo3: slider 5-100 %
    g_secs = LoadF(L"Seconds", 10);
    if (g_secs < 0) g_secs = 0;
    g_ramp = LoadF(L"Ramp", 0.3f);
    if (g_ramp < 0) g_ramp = 0;
    g_place = LoadF(L"Place", 0) != 0 ? 1 : 0;  // slowbtn1
    g_showSecs = LoadF(L"ShowSeconds", 0) != 0;  // slowglow1
    g_btn[0] = LoadF(L"ButtonRight", -0.14f), g_btn[1] = LoadF(L"ButtonUp", 0), g_btn[2] = LoadF(L"ButtonTowards", 0);
    for (float &x : g_btn) x = x < -0.6f ? -0.6f : x > 0.6f ? 0.6f : x;
    float z = LoadF(L"ButtonSize", 0.035f), rr = LoadF(L"ButtonReach", 0.09f);
    g_btnSize = z < 0.01f ? 0.01f : z > 0.15f ? 0.15f : z;
    g_btnReach = rr < 0.04f ? 0.04f : rr > 0.3f ? 0.3f : rr;
    SlowFxLoad();  // slowfx1
    SlowSndLoad();  // slowsnd1
    SoundVolLoad();  // vol1
}
void SlowmoSave() {
    SaveF(L"On", g_on ? 1.f : 0.f);
    SaveF(L"Speed", g_speed);
    SaveF(L"Seconds", g_secs);
    SaveF(L"Ramp", g_ramp);
    SaveF(L"Place", (float)g_place);  // slowbtn1
    SaveF(L"ShowSeconds", g_showSecs ? 1.f : 0.f);
    SaveF(L"ButtonRight", g_btn[0]), SaveF(L"ButtonUp", g_btn[1]), SaveF(L"ButtonTowards", g_btn[2]);
    SaveF(L"ButtonSize", g_btnSize), SaveF(L"ButtonReach", g_btnReach);
    SlowFxSave();  // slowfx1
    SlowSndSave();  // slowsnd1
}

bool *SlowmoOn() { return &g_on; }
float *SlowmoSpeed() { return &g_speed; }
float *SlowmoSeconds() { return &g_secs; }
float *SlowmoRamp() { return &g_ramp; }
bool SlowmoShown() { return g_on && g_bloodDragon && g_found >= 0; }
bool SlowmoActive() { return g_want != 0; }
int SlowmoFound() { return g_found; }
float SlowmoNow() { return g_cur; }

// Seconds left while on and timed (-1 otherwise).
float SlowmoLeft() {
    if (!g_want || g_secs <= 0) return -1;
    float left = g_secs - (GetTickCount() - g_startT) * 0.001f;
    return left < 0 ? 0 : left;
}

// holster.cpp: a right grip on the clock.
void SlowmoToggle() {
    if (g_found == 0) Find();
    if (g_found < 0) return;
    if (g_want) { Stop("clock touched again"); return; }
    g_startT = GetTickCount();
    InterlockedExchange(&g_want, 1);
    g_buzzT = g_startT, g_buzzKind = 1;  // slowbtn1
    SlowFxEvent(1);  // slowfx1
    Log("slowmo: on, %.0f%% speed, %s", g_speed * 100, g_secs > 0 ? "timed" : "until touched again");
}

// Input thread, each pad read (holster.cpp HolsterInput).
void SlowmoTick() {
    if (!g_bloodDragon) return;
    if (g_found == 0) Find();
    if (g_found < 0) return;
    DWORD now = GetTickCount();
    float dt = g_lastT ? (now - g_lastT) * 0.001f : 0;
    g_lastT = now;
    if (dt > 0.1f) dt = 0.1f;
    if (EngineOnFoot() || EngineInVehicle()) g_seenT = now;
    bool cut = EngineCutscene(), paused = GamePaused(), gone = now - g_seenT > 1500;
    if (g_want) {
        if (!g_on) Stop("clock switched off in the menu");
        else if (cut) Stop("cutscene");
        else if (paused) Stop("game paused");
        else if (gone) Stop("not playing (loading or dead)");
        else if (g_secs > 0 && now - g_startT >= (DWORD)(g_secs * 1000)) Stop("time up");
    }
    float target = g_want ? g_speed : 1.f, v = g_cur;
    if (cut || paused || gone || g_ramp <= 0.001f) v = target;  // no easing out into a cutscene or menu
    else {
        float step = dt / g_ramp * fabsf(1.f - g_speed);
        if (step < 1e-4f) step = 1e-4f;
        v = v < target ? fminf(target, v + step) : fmaxf(target, v - step);
    }
    if (v != g_cur || (v != 1.f && *g_scale != v)) {
        g_cur = v;
        *g_scale = v;
    }
    SlowSndTick(g_cur);  // slowsnd1: the game's sounds follow
}
