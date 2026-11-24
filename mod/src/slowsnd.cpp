// slowsnd1 (DrGonzaga 16 Nov, Blood Dragon: "the weapon sounds are 'fast' (e.g. laser gun) in slow mo and dont'
// line up with the laser frequency coming out. I don't mind reg sounds for slow mo, but slowing weapon sounds
// would be great if possible").
//
// The game's sound engine (Dare) plays through XAudio2 2.7 (it makes the engine with CoCreateInstance at
// BD+10dbdf82). Every sound is an XAudio2 "source voice", and a voice has a frequency ratio: 0.5 plays it at
// half speed, an octave lower. Nothing in the game is hooked. Instead three entries of XAudio2's own source
// voice table are replaced (and one of the engine's, to see each voice as it is made):
//   CreateSourceVoice   the voice is noted; a "no pitch" voice is made as an ordinary one so it can be slowed
//   SubmitSourceBuffer  tells a one-shot or looping effect (given its sound while its queue is empty) from
//                       streamed sound (music and speech arrive in pieces, queued one behind the other)
//   SetFrequencyRatio   the game's own ratio (Doppler and so on) is kept and multiplied by ours
//   DestroyVoice        the voice is forgotten
// While slow motion is on, the ratio of the voices chosen ([slowsnd] Mode: 0 effects only, 1 everything) is
// the game's own times a factor between 1 and the game speed ([slowsnd] Amount). The game can't say which
// effect is a weapon, so "effects" means every one-shot and loop: guns, impacts, footsteps, explosions.
//
// Locks: g_tab guards the table and is never held across an XAudio2 call (XAudio2 calls the game back from
// its own thread, and the game may submit from there). g_life is held by the tick while it sets ratios and
// by DestroyVoice while it forgets a voice, so the tick never touches a voice that is gone.
#include <windows.h>
#include <objbase.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
struct XaBuffer { UINT32 Flags, AudioBytes; const BYTE *pAudioData; UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount; void *pContext; };
#pragma pack(push, 1)
struct XaState { void *ctx; UINT32 queued; UINT64 played; };
#pragma pack(pop)
typedef HRESULT(__stdcall *CreateFn)(void *, void **, const WAVEFORMATEX *, UINT32, float, void *, const void *, const void *);
typedef void(__stdcall *DestroyFn)(void *);
typedef HRESULT(__stdcall *SubmitFn)(void *, const XaBuffer *, const void *);
typedef void(__stdcall *StateFn)(void *, XaState *);
typedef HRESULT(__stdcall *SetFreqFn)(void *, float, UINT32);
typedef HRESULT(__stdcall *SetVolFn)(void *, float, UINT32);
// vol1 (beta feedback 23 Nov: volume controls in the VR menu): IXAudio2Voice::SetVolume is slot 12. The game's own
// volume for each voice is kept and multiplied by ours: master x (effects, or music and speech for streamed voices).
enum { kCreate = 8, kSetVol = 12, kDestroy = 18, kSubmit = 21, kState = 25, kSetFreq = 26 };  // XAudio2 2.7 (checked against the SDK header's tables)
const UINT32 kNoPitch = 0x0002, kEndOfStream = 0x0040;

bool g_on = true;      // [slowsnd] On
int g_mode = 0;        // [slowsnd] Mode: 0 effects only, 1 everything
float g_amount = 1;    // [slowsnd] Amount: 0 = no change .. 1 = as slow as the game

CreateFn g_create;
DestroyFn g_destroy;
SubmitFn g_submit;
StateFn g_state;
SetFreqFn g_setFreq;
SetVolFn g_setVol;
float g_volMaster = 1, g_volFx = 1, g_volStream = 1;  // vol1: [sound] Master, Effects, MusicSpeech (0..1)
volatile LONG g_volEpoch;                             // vol1: bumped when a volume slider moves
void **g_voiceVt;
volatile LONG g_hooked;  // 0 no, 1 the engine's table, 2 the voices' table too
CRITICAL_SECTION g_tab, g_life;

struct Voice {
    void *v;
    float game;     // the ratio the game asked for
    float applied;  // the multiplier in force
    bool stream;    // sound arriving in queued pieces (music, speech)
    bool loop;
    float gameVol;  // vol1: the volume the game asked for
    float vol;      // vol1: our multiplier in force
};
enum { kMax = 768 };
Voice g_v[kMax];
int g_n;
volatile float g_factor = 1;  // what effects are multiplied by now
// counters for the log
LONG g_made, g_gone, g_noPitch, g_submits, g_streamSubmits, g_gameSets, g_ourSets, g_fail, g_full;
DWORD g_logT;

Voice *Find(void *v) {
    for (int i = 0; i < g_n; ++i)
        if (g_v[i].v == v) return &g_v[i];
    return nullptr;
}
float VolMult(const Voice &e) { return g_volMaster * (e.stream ? g_volStream : g_volFx); }  // vol1
float Mult(const Voice &e) {
    float f = g_factor;
    if (!g_on || f > 0.995f) return 1;
    return g_mode == 1 || !e.stream ? f : 1;
}

void __stdcall HDestroy(void *self) {
    EnterCriticalSection(&g_life);
    EnterCriticalSection(&g_tab);
    if (Voice *e = Find(self)) *e = g_v[--g_n], ++g_gone;
    LeaveCriticalSection(&g_tab);
    LeaveCriticalSection(&g_life);
    g_destroy(self);
}
HRESULT __stdcall HSetFreq(void *self, float ratio, UINT32 opset) {
    float m = 1;
    EnterCriticalSection(&g_tab);
    if (Voice *e = Find(self)) e->game = ratio, m = e->applied = Mult(*e);
    ++g_gameSets;
    LeaveCriticalSection(&g_tab);
    return g_setFreq(self, ratio * m, opset);
}
HRESULT __stdcall HSetVol(void *self, float v, UINT32 opset) {  // vol1
    float m = 1;
    EnterCriticalSection(&g_tab);
    if (Voice *e = Find(self)) e->gameVol = v, m = e->vol = VolMult(*e);
    LeaveCriticalSection(&g_tab);
    return g_setVol(self, v * m, opset);
}
HRESULT __stdcall HSubmit(void *self, const XaBuffer *b, const void *wma) {
    XaState st = {};
    g_state(self, &st);
    float set = 0, vset = -1;
    EnterCriticalSection(&g_tab);
    ++g_submits;
    if (Voice *e = Find(self)) {
        bool loop = b && b->LoopCount != 0;
        if (st.queued == 0) e->stream = false, e->loop = loop;        // a fresh sound on an idle voice
        else if (!loop) e->stream = true, ++g_streamSubmits;         // another piece behind the last: streamed
        float m = Mult(*e);
        if (fabsf(m - e->applied) > 0.004f) e->applied = m, set = e->game * m;
        float vm = VolMult(*e);  // vol1: effects and music / speech can have their own volume
        if (fabsf(vm - e->vol) > 0.004f) e->vol = vm, vset = e->gameVol * vm;
    }
    LeaveCriticalSection(&g_tab);
    if (vset >= 0 && g_setVol) g_setVol(self, vset, 0);
    if (set > 0) {
        if (FAILED(g_setFreq(self, set, 0))) InterlockedIncrement(&g_fail);
        InterlockedIncrement(&g_ourSets);
    }
    return g_submit(self, b, wma);
}
HRESULT __stdcall HCreate(void *self, void **ppVoice, const WAVEFORMATEX *fmt, UINT32 flags, float maxRatio, void *cb, const void *sends, const void *chain) {
    if (flags & kNoPitch) flags &= ~kNoPitch, InterlockedIncrement(&g_noPitch);
    HRESULT hr = g_create(self, ppVoice, fmt, flags, maxRatio, cb, sends, chain);
    if (FAILED(hr) || !ppVoice || !*ppVoice) return hr;
    void **vt = *(void ***)*ppVoice;
    if (InterlockedCompareExchange(&g_hooked, 2, 1) == 1) {
        g_voiceVt = vt;
        g_destroy = (DestroyFn)vt[kDestroy], g_submit = (SubmitFn)vt[kSubmit], g_state = (StateFn)vt[kState], g_setFreq = (SetFreqFn)vt[kSetFreq];
        g_setVol = (SetVolFn)vt[kSetVol];
        void *hd = (void *)HDestroy, *hs = (void *)HSubmit, *hf = (void *)HSetFreq, *hv = (void *)HSetVol;
        bool ok = PatchBytes(&vt[kDestroy], &hd, 4) && PatchBytes(&vt[kSubmit], &hs, 4) && PatchBytes(&vt[kSetFreq], &hf, 4);
        if (ok && !PatchBytes(&vt[kSetVol], &hv, 4)) Log("slowsnd: volume hook NOT in: the VR menu's volume sliders won't work");
        Log("slowsnd: the game's first sound voice made (%u Hz, %u channel(s), format %u, flags %lx); its table %s", fmt ? (unsigned)fmt->nSamplesPerSec : 0,
            fmt ? (unsigned)fmt->nChannels : 0, fmt ? (unsigned)fmt->wFormatTag : 0, (unsigned long)flags, ok ? "hooked" : "NOT hooked: sounds stay as they are");
    }
    if (vt == g_voiceVt) {
        EnterCriticalSection(&g_tab);
        if (g_n < kMax) g_v[g_n++] = Voice{*ppVoice, 1, 1, false, false, 1, 1};
        else ++g_full;
        ++g_made;
        LeaveCriticalSection(&g_tab);
    }
    return hr;
}

DWORD WINAPI Install(void *) {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    _wcslwr(exe);
    if (!wcsstr(exe, L"blooddragon")) return 0;  // slow motion is Blood Dragon's only
    HMODULE xa = LoadLibraryW(L"XAudio2_7.dll");  // kept loaded: the table patched below lives in it
    if (!xa) { Log("slowsnd: XAudio2_7.dll not found; sounds are not slowed"); return 0; }
    static const GUID clsid = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
    static const GUID iid = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
    HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUnknown *eng = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void **)&eng);
    if (FAILED(hr) || !eng) {
        Log("slowsnd: no XAudio2 engine could be made (%lx); sounds are not slowed", (unsigned long)hr);
        if (SUCCEEDED(ci)) CoUninitialize();
        return 0;
    }
    void **vt = *(void ***)eng;
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vt[kCreate], &owner);
    bool ok = false;
    if (owner == xa) {
        g_create = (CreateFn)vt[kCreate];
        void *hc = (void *)HCreate;
        InterlockedExchange(&g_hooked, 1);
        ok = PatchBytes(&vt[kCreate], &hc, 4);
        if (!ok) InterlockedExchange(&g_hooked, 0);
    }
    eng->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
    Log("slowsnd: XAudio2 2.7 %s", ok ? "hooked: the game's sounds can be slowed with slow motion" : owner == xa ? "could not be patched; sounds are not slowed" : "is not the engine expected; sounds are not slowed");
    return 0;
}

float LoadF(const wchar_t *key, float def) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    if (!GetPrivateProfileStringW(L"slowsnd", key, L"", buf, 32, path) || !buf[0]) return def;
    return (float)_wtof(buf);
}
void SaveF(const wchar_t *key, float v) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    swprintf(buf, 32, L"%.3f", v);
    WritePrivateProfileStringW(L"slowsnd", key, buf, path);
}
}  // namespace

// hook.cpp, as the mod starts (before the game has made its sound engine).
void SlowSndStart() {
    InitializeCriticalSection(&g_tab);
    InitializeCriticalSection(&g_life);
    CloseHandle(CreateThread(nullptr, 0, Install, nullptr, 0, nullptr));
}
void SlowSndLoad() {
    g_on = LoadF(L"On", 1) != 0;
    g_mode = LoadF(L"Mode", 0) != 0 ? 1 : 0;
    float a = LoadF(L"Amount", 1);
    g_amount = a < 0 ? 0 : a > 1 ? 1 : a;
}
void SlowSndSave() { SaveF(L"On", g_on ? 1.f : 0.f), SaveF(L"Mode", (float)g_mode), SaveF(L"Amount", g_amount); }
// vol1: [sound] Master / Effects / MusicSpeech, 0..1 (1 = the game's own volume)
static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
void SoundVolLoad() {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    float *v[3] = {&g_volMaster, &g_volFx, &g_volStream};
    const wchar_t *k[3] = {L"Master", L"Effects", L"MusicSpeech"};
    for (int i = 0; i < 3; ++i)
        *v[i] = 1;  // vol1 sliders removed 24 Nov: always the game's own level (an old saved value is ignored)
    (void)buf, (void)k, (void)Clamp01;
    InterlockedIncrement(&g_volEpoch);
}
void SoundVolSave() {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    float v[3] = {g_volMaster, g_volFx, g_volStream};
    const wchar_t *k[3] = {L"Master", L"Effects", L"MusicSpeech"};
    for (int i = 0; i < 3; ++i) swprintf(buf, 32, L"%.2f", v[i]), WritePrivateProfileStringW(L"sound", k[i], buf, path);
}
float *SoundVol(int i) { return i == 0 ? &g_volMaster : i == 1 ? &g_volFx : &g_volStream; }
void SoundVolChanged() { InterlockedIncrement(&g_volEpoch); }
bool *SlowSndOn() { return &g_on; }
int *SlowSndMode() { return &g_mode; }
float *SlowSndAmount() { return &g_amount; }
int SlowSndState() { return (int)g_hooked; }
int SlowSndVoices() { return g_n; }

// Input thread, each pad read (slowmo.cpp SlowmoTick), with the game speed written this tick (1 = normal).
void SlowSndTick(float speed) {
    if (g_hooked < 2) return;
    static int s_mode = -1;
    static bool s_on;
    float f = g_on ? 1 + g_amount * (speed - 1) : 1;
    f = f < 0.05f ? 0.05f : f > 1 ? 1 : f;
    DWORD now = GetTickCount();
    if (fabsf(f - g_factor) > 0.004f || g_mode != s_mode || g_on != s_on) {
        g_factor = f, s_mode = g_mode, s_on = g_on;
        // every voice whose multiplier is now different (the sounds already playing: loops above all)
        EnterCriticalSection(&g_life);
        static struct { void *v; float r; } s_set[kMax];
        int n = 0;
        EnterCriticalSection(&g_tab);
        for (int i = 0; i < g_n; ++i) {
            float m = Mult(g_v[i]);
            if (fabsf(m - g_v[i].applied) > 0.004f) g_v[i].applied = m, s_set[n].v = g_v[i].v, s_set[n++].r = g_v[i].game * m;
        }
        LeaveCriticalSection(&g_tab);
        for (int i = 0; i < n; ++i) {
            if (FAILED(g_setFreq(s_set[i].v, s_set[i].r, 0))) InterlockedIncrement(&g_fail);
            InterlockedIncrement(&g_ourSets);
        }
        LeaveCriticalSection(&g_life);
    }
    static LONG s_volEp = -1;  // vol1: a volume slider moved: every voice gets the new volume
    if (g_volEpoch != s_volEp && g_setVol) {
        s_volEp = g_volEpoch;
        EnterCriticalSection(&g_life);
        static struct { void *v; float r; } s_vs[kMax];
        int n = 0;
        EnterCriticalSection(&g_tab);
        for (int i = 0; i < g_n; ++i) {
            float vm = VolMult(g_v[i]);
            if (fabsf(vm - g_v[i].vol) > 0.004f) g_v[i].vol = vm, s_vs[n].v = g_v[i].v, s_vs[n++].r = g_v[i].gameVol * vm;
        }
        LeaveCriticalSection(&g_tab);
        for (int i = 0; i < n; ++i) g_setVol(s_vs[i].v, s_vs[i].r, 0);
        LeaveCriticalSection(&g_life);
    }
    if ((f < 0.995f && now - g_logT > 10000) || (!g_logT && g_made > 0)) {  // what was seen, while it matters
        g_logT = now ? now : 1;
        int streams = 0, loops = 0;
        EnterCriticalSection(&g_tab);
        for (int i = 0; i < g_n; ++i) streams += g_v[i].stream, loops += g_v[i].loop;
        int n = g_n;
        LeaveCriticalSection(&g_tab);
        Log("slowsnd: sounds x%.2f (%s): %d voices (%d streamed, %d looping); made %ld (%ld were no-pitch), gone %ld, submits %ld (%ld streamed pieces), "
            "ratio set by the game %ld times and by us %ld (%ld refused)%s",
            f, g_mode ? "everything" : "effects only", n, streams, loops, g_made, g_noPitch, g_gone, g_submits, g_streamSubmits, g_gameSets, g_ourSets, g_fail,
            g_full ? "; TABLE FULL" : "");
    }
}
