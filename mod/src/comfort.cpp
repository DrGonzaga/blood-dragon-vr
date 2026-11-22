// comfort1 (DrGonzaga 22 Nov: "Can we add a comfort vignette as well? And make it adjustable?"):
// a dark edge round the view that closes in while you move with the stick and/or turn, which many people
// find stops motion sickness. Adjustable strength (how dark), size (how far in it reaches), when it shows
// (always / moving / turning / moving or turning, vehicles count as moving), fade in / out and colour.
// Drawn by laser.cpp's full-screen effect quad (the slow-motion vignette's), into each eye before the
// beam and marks, so both eyes match. Off while scoped, in the cyber-eye, on the 2D cutscene screen and
// while the game is paused. Settings: [comfort] in fc3vr.ini.
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
ComfortCfg g = {false, 0.85f, 0.45f, 0.15f, 0.35f, 3, {0, 0, 0}, false};
float g_env, g_now;
LARGE_INTEGER g_at, g_qf;

float Cl(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    if (!GetPrivateProfileStringW(L"comfort", key, L"", buf, 32, path) || !buf[0]) return def;
    return (float)_wtof(buf);
}
void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"comfort", key, buf, path);
}
}  // namespace

ComfortCfg *Comfort() { return &g; }

void ComfortLoad() {
    g.on = LoadF(L"Vignette", 0) != 0;
    g.strength = Cl(LoadF(L"Strength", g.strength), 0, 1);
    g.size = Cl(LoadF(L"Size", g.size), 0.05f, 0.95f);
    g.mode = (int)Cl(LoadF(L"When", (float)g.mode), 0, 3);
    g.fadeIn = Cl(LoadF(L"FadeIn", g.fadeIn), 0, 2), g.fadeOut = Cl(LoadF(L"FadeOut", g.fadeOut), 0, 2);
    static const wchar_t *ck[3] = {L"ColourR", L"ColourG", L"ColourB"};
    for (int i = 0; i < 3; ++i) g.colour[i] = Cl(LoadF(ck[i], g.colour[i]), 0, 1);
    static const char *const kWhen[4] = {"always", "while moving", "while turning", "while moving or turning"};
    Log("comfort: vignette %s (strength %.2f, size %.2f, %s)", g.on ? "on" : "off ([comfort] Vignette)", g.strength, g.size, kWhen[g.mode]);
}

void ComfortSave() {
    SaveF(L"Vignette", g.on), SaveF(L"Strength", g.strength), SaveF(L"Size", g.size), SaveF(L"When", (float)g.mode);
    SaveF(L"FadeIn", g.fadeIn), SaveF(L"FadeOut", g.fadeOut);
    static const wchar_t *ck[3] = {L"ColourR", L"ColourG", L"ColourB"};
    for (int i = 0; i < 3; ++i) SaveF(ck[i], g.colour[i]);
}

// Render thread, each eye (hook.cpp), before the slow-motion look, the beam and the marks.
void ComfortDraw(IDXGISwapChain *sc) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf);
    float dt = g_at.QuadPart ? float(t.QuadPart - g_at.QuadPart) / float(g_qf.QuadPart) : 0;
    g_at = t;
    if (dt > 0.1f) dt = 0.1f;
    bool moving = false, turning = false;
    VrPad p;
    if (XrGetPad(&p) && !MenuOpen()) {  // p is already swapped for left-handed play: l = the walking stick
        moving = sqrtf(p.lx * p.lx + p.ly * p.ly) > 0.2f || EngineInVehicle();
        turning = fabsf(p.rx) > 0.3f;
    }
    bool want = g.preview || (g.on && (g.mode == 0 || (g.mode == 1 && moving) || (g.mode == 2 && turning) || (g.mode == 3 && (moving || turning))));
    if (want) g_env = g.fadeIn > 0.01f ? fminf(1, g_env + dt / g.fadeIn) : 1;
    else g_env = g.fadeOut > 0.01f ? fmaxf(0, g_env - dt / g.fadeOut) : 0;
    g_now = g_env * g_env * (3 - 2 * g_env);
    if (g_now < 0.002f || EngineScoped() || CyberEyeOn() || EngineFlat() || (GamePaused() && !g.preview)) return;
    LaserFx fx = {};
    fx.gain[0] = fx.gain[1] = fx.gain[2] = 1;
    for (int i = 0; i < 3; ++i) fx.vig[i] = g.colour[i];
    fx.vig[3] = Cl(g_now * g.strength, 0, 1);
    fx.vigStart = Cl(1 - g.size, 0.05f, 0.95f);
    LaserDrawFx(sc, fx);
}
