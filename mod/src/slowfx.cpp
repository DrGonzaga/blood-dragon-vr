// slowfx1 (DrGonzaga 16 Nov, Blood Dragon: "Can we make a build with a few togglable options, and we can decide
// on a combo. Neon push, vignette, haptics, sound, scanlines, anything cheap and possibly effective? That I
// can adjust / toggle in real to find a combo?").
//
// Cues that tell you slow motion is on, each with its own tick and sliders (menu: Guns page, "Wrist menu
// settings", under the slow motion clock), all changeable while playing:
//   colour push   the whole picture multiplied by a colour (a real grade: darks stay dark)
//   vignette      a coloured edge that fades in, and can throb with the heartbeat
//   scan lines    faint dark lines, the same rows in both eyes
//   heartbeat     a lub-dub on the controllers
//   whoosh        a short sound as it starts and as it ends (made here, played through Windows' own sound)
//   ring          a glowing ring round the floating clock that drains with the time left
// The picture cues are two full-screen quads drawn by laser.cpp into each eye's image before the beam and
// the marks, from the same numbers for both eyes; nothing is hooked in the game and nothing is read back.
// They are left out while scoped (one eye's image is the scope there) and in the cyber-eye.
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
SlowFxCfg g = {
    true,  0.35f, {0.75f, 0.45f, 1.0f},        // push
    true,  0.50f, 0.55f, {1.0f, 0.10f, 0.60f}, 0.30f,  // vignette, throb
    false, 0.15f, 360,                          // scan lines
    true,  0.50f, 60,                           // heartbeat
    true,  0.70f,                               // whoosh
    false,                                      // ring (slowglow1: off)
    true,  0.80f,                               // glow (slowglow1)
    0.6f,  0.4f,                                // fade in / out
    false,                                      // preview
};
float g_env;          // 0..1, how far the look is faded in (render thread)
LARGE_INTEGER g_envAt, g_qf;
DWORD g_beat0;        // when the heartbeat began (slow motion's start)

float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    if (!GetPrivateProfileStringW(L"slowfx", key, L"", buf, 32, path) || !buf[0]) return def;
    return (float)_wtof(buf);
}
void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"slowfx", key, buf, path);
}
float Cl(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// The heartbeat's shape at time t (ms) into slow motion: 1 on the "lub", 0.7 on the "dub", else 0.
float Beat(DWORD t, float *soft) {
    float bpm = Cl(g.heartBpm, 30, 180);
    DWORD period = (DWORD)(60000.f / bpm), ph = t % (period ? period : 1);
    if (soft) {  // for the vignette's throb: a quick rise and a slow fall over the beat
        float x = ph / (float)period;
        *soft = x < 0.08f ? x / 0.08f : expf(-(x - 0.08f) * 5.f);
    }
    if (ph < 70) return 1.f;
    if (ph >= 190 && ph < 250) return 0.7f;
    return 0.f;
}

// ---- the whoosh: two short sounds made once (and again when the volume changes), played by a thread of
// their own so a slow start of Windows' player never holds up the game or the input thread ----
struct Wav { BYTE *data; DWORD size; };
Wav g_wav[2];
float g_wavVol = -1;
HANDLE g_sndEvent, g_sndThread;
volatile LONG g_sndWant;  // 1 start, 2 end
CRITICAL_SECTION g_sndLock;
bool g_sndInit;

void MakeWav(Wav &w, bool up, float vol) {
    const int rate = 22050;
    const float secs = up ? 0.55f : 0.8f;
    const int n = (int)(rate * secs);
    DWORD size = 44 + n * 2;
    BYTE *d = (BYTE *)malloc(size);
    if (!d) return;
    auto put32 = [&](int at, DWORD v) { memcpy(d + at, &v, 4); };
    auto put16 = [&](int at, WORD v) { memcpy(d + at, &v, 2); };
    memcpy(d, "RIFF", 4), put32(4, size - 8), memcpy(d + 8, "WAVEfmt ", 8), put32(16, 16), put16(20, 1), put16(22, 1);
    put32(24, rate), put32(28, rate * 2), put16(32, 2), put16(34, 16), memcpy(d + 36, "data", 4), put32(40, n * 2);
    // Noise through a band-pass whose centre sweeps (down as slow motion starts, up as it ends), over a low sine
    // that sweeps the same way.
    unsigned seed = up ? 0x1234567u : 0x7654321u;
    float lp = 0, bp = 0, phase = 0;
    for (int i = 0; i < n; ++i) {
        float x = i / (float)n, s = up ? x : 1 - x;       // s: 1 = high, 0 = low
        float fc = 220.f * powf(12.f, s);                 // 220 Hz .. 2.6 kHz
        float f = 2 * sinf(3.14159265f * fc / rate), q = 0.35f;
        seed = seed * 1664525u + 1013904223u;
        float noise = ((seed >> 9) & 0x7fff) / 16384.f - 1.f;
        float hp = noise - lp - q * bp;                   // state-variable filter
        bp += f * hp, lp += f * bp;
        float tone = 60.f + 170.f * s;
        phase += 6.2831853f * tone / rate;
        float env = up ? sinf(3.14159265f * powf(x, 0.7f)) : (x < 0.04f ? x / 0.04f : expf(-(x - 0.04f) * 4.2f));
        if (x > 0.95f) env *= (1 - x) / 0.05f;
        float v = (bp * 0.5f + sinf(phase) * 0.2f) * env * vol;
        v = v > 1 ? 1 : v < -1 ? -1 : v;
        short sm = (short)(v * 30000);
        memcpy(d + 44 + i * 2, &sm, 2);
    }
    w.data = d, w.size = size;
}
DWORD WINAPI SoundThread(void *) {
    for (;;) {
        WaitForSingleObject(g_sndEvent, INFINITE);
        LONG want = InterlockedExchange(&g_sndWant, 0);
        if (want < 1 || want > 2) continue;
        EnterCriticalSection(&g_sndLock);
        float vol = Cl(g.soundVol, 0, 1);
        if (vol != g_wavVol) {
            PlaySoundA(nullptr, nullptr, 0);  // nothing of ours is playing from the old buffers
            for (Wav &w : g_wav) free(w.data), w.data = nullptr, w.size = 0;
            MakeWav(g_wav[0], false, vol), MakeWav(g_wav[1], true, vol);
            g_wavVol = vol;
        }
        const Wav &w = g_wav[want - 1];
        if (w.data) PlaySoundA((LPCSTR)w.data, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        LeaveCriticalSection(&g_sndLock);
    }
    return 0;
}
void Whoosh(int kind) {
    if (!g.sound || g.soundVol < 0.01f) return;
    if (!g_sndInit) {
        g_sndInit = true;
        InitializeCriticalSection(&g_sndLock);
        g_sndEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_sndThread = g_sndEvent ? CreateThread(nullptr, 0, SoundThread, nullptr, 0, nullptr) : nullptr;
        Log("slowfx: whoosh sound %s", g_sndThread ? "ready (Windows' default sound device)" : "NOT available (no thread)");
    }
    if (!g_sndThread) return;
    InterlockedExchange(&g_sndWant, kind);
    SetEvent(g_sndEvent);
}
}  // namespace

SlowFxCfg *SlowFx() { return &g; }

void SlowFxLoad() {
    g.push = LoadF(L"Push", 1) != 0, g.pushStrength = Cl(LoadF(L"PushStrength", g.pushStrength), 0, 1);
    g.vig = LoadF(L"Vignette", 1) != 0, g.vigStrength = Cl(LoadF(L"VignetteStrength", g.vigStrength), 0, 1);
    g.vigStart = Cl(LoadF(L"VignetteStart", g.vigStart), 0.1f, 0.95f), g.vigThrob = Cl(LoadF(L"VignetteThrob", g.vigThrob), 0, 1);
    static const wchar_t *pk[3] = {L"PushR", L"PushG", L"PushB"}, *vk[3] = {L"VignetteR", L"VignetteG", L"VignetteB"};
    for (int i = 0; i < 3; ++i) g.pushCol[i] = Cl(LoadF(pk[i], g.pushCol[i]), 0, 1), g.vigCol[i] = Cl(LoadF(vk[i], g.vigCol[i]), 0, 1);
    g.scan = LoadF(L"Scanlines", 0) != 0, g.scanStrength = Cl(LoadF(L"ScanlineStrength", g.scanStrength), 0, 1);
    g.scanLines = Cl(LoadF(L"ScanlineCount", g.scanLines), 60, 1000);
    g.heart = LoadF(L"Heartbeat", 1) != 0, g.heartStrength = Cl(LoadF(L"HeartbeatStrength", g.heartStrength), 0, 1);
    g.heartBpm = Cl(LoadF(L"HeartbeatPerMinute", g.heartBpm), 30, 180);
    g.sound = LoadF(L"Whoosh", 1) != 0, g.soundVol = Cl(LoadF(L"WhooshVolume", g.soundVol), 0, 1);
    g.ring = LoadF(L"CountdownRing", 0) != 0;  // slowglow1: new key, off (DrGonzaga 16 Nov: no countdown ring)
    g.glow = LoadF(L"Glow", 1) != 0, g.glowStrength = Cl(LoadF(L"GlowStrength", 0.8f), 0, 1);
    g.fadeIn = Cl(LoadF(L"FadeIn", g.fadeIn), 0, 3), g.fadeOut = Cl(LoadF(L"FadeOut", g.fadeOut), 0, 3);
}
void SlowFxSave() {
    SaveF(L"Push", g.push), SaveF(L"PushStrength", g.pushStrength);
    SaveF(L"Vignette", g.vig), SaveF(L"VignetteStrength", g.vigStrength), SaveF(L"VignetteStart", g.vigStart), SaveF(L"VignetteThrob", g.vigThrob);
    static const wchar_t *pk[3] = {L"PushR", L"PushG", L"PushB"}, *vk[3] = {L"VignetteR", L"VignetteG", L"VignetteB"};
    for (int i = 0; i < 3; ++i) SaveF(pk[i], g.pushCol[i]), SaveF(vk[i], g.vigCol[i]);
    SaveF(L"Scanlines", g.scan), SaveF(L"ScanlineStrength", g.scanStrength), SaveF(L"ScanlineCount", g.scanLines);
    SaveF(L"Heartbeat", g.heart), SaveF(L"HeartbeatStrength", g.heartStrength), SaveF(L"HeartbeatPerMinute", g.heartBpm);
    SaveF(L"Whoosh", g.sound), SaveF(L"WhooshVolume", g.soundVol);
    SaveF(L"CountdownRing", g.ring);
    SaveF(L"Glow", g.glow), SaveF(L"GlowStrength", g.glowStrength);
    SaveF(L"FadeIn", g.fadeIn), SaveF(L"FadeOut", g.fadeOut);
}

// slowmo.cpp: slow motion has just started (1) or ended (2). Any thread.
void SlowFxEvent(int kind) {
    if (kind == 1) g_beat0 = GetTickCount();
    Whoosh(kind);
}
// The menu's "play it" buttons.
void SlowFxTestSound(int kind) { Whoosh(kind); }

// xr.cpp haptics, both controllers: the heartbeat while slow motion (or the preview) is on.
float SlowFxBuzz() {
    if (!g.heart || !(SlowmoActive() || g.preview)) return 0;
    return Beat(GetTickCount() - g_beat0, nullptr) * g.heartStrength;
}

// Render thread, each eye, before the beam and the marks (hook.cpp).
void SlowFxDraw(IDXGISwapChain *sc) {
    if (!g_bloodDragon) return;
    const bool want = (SlowmoActive() || g.preview) && !EngineCutscene() && !GamePaused();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf);
    float dt = g_envAt.QuadPart ? float(t.QuadPart - g_envAt.QuadPart) / float(g_qf.QuadPart) : 0;
    g_envAt = t;
    if (dt > 0.1f) dt = 0.1f;
    if (want) g_env = g.fadeIn > 0.01f ? fminf(1, g_env + dt / g.fadeIn) : 1;
    else g_env = g.fadeOut > 0.01f ? fmaxf(0, g_env - dt / g.fadeOut) : 0;
    if (CyberEyeOn() || EngineScoped()) return;
    const float e = g_env * g_env * (3 - 2 * g_env);  // eased
    if (e > 0.002f) {
        LaserFx fx = {};
        fx.gain[0] = fx.gain[1] = fx.gain[2] = 1;
        if (g.push)
            for (int i = 0; i < 3; ++i) fx.gain[i] = Cl(1 + e * g.pushStrength * (1.5f * g.pushCol[i] - 1), 0, 2);
        if (g.vig) {
            float soft = 0;
            Beat(GetTickCount() - g_beat0, &soft);
            memcpy(fx.vig, g.vigCol, 12);
            fx.vig[3] = Cl(e * g.vigStrength * (1 - g.vigThrob * 0.6f * (1 - soft)), 0, 1);
            fx.vigStart = g.vigStart;
        }
        if (g.scan) fx.scan = e * g.scanStrength, fx.scanLines = g.scanLines;
        LaserDrawFx(sc, fx);
    }
    // the ring round the floating clock: all of it while untimed, draining with the time left otherwise
    float b[3];
    EngineView v;
    if ((g.ring || g.glow) && e > 0.002f && SlowmoByMap() && SlowmoActive() && SlowmoButtonPos(b) && EngineLastView(&v) && v.sixdof) {
        const float *fr = v.posAxes, *wu = v.posAxes + 3, *ff = v.posAxes + 6;
        float o[3] = {b[0] / v.scale, b[1] / v.scale, b[2] / v.scale}, w[3];
        for (int i = 0; i < 3; ++i) w[i] = v.base[i] + fr[i] * o[0] + wu[i] * o[1] - ff[i] * o[2];
        float left = SlowmoLeft(), secs = *SlowmoSeconds(), soft = 0;
        Beat(GetTickCount() - g_beat0, &soft);
        float frac = left >= 0 && secs > 0 ? Cl(left / secs, 0, 1) : 1;
        if (g.glow) {  // slowglow1: a warm glow behind the clock, breathing with the heartbeat
            const float gc[4] = {1.0f, 0.55f, 0.2f, e * g.glowStrength * (0.75f + 0.25f * soft)};
            LaserDrawGlow(sc, w, *SlowmoButton(3) * 2.6f / v.scale, gc);
        }
        const float rgba[4] = {1.0f, 0.75f, 0.3f, e * (0.7f + 0.3f * soft)};
        if (g.ring) LaserDrawRing(sc, w, *SlowmoButton(3) * 1.9f / v.scale, frac, rgba);
    }
}
