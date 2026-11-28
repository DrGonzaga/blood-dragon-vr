// Render-twice stereo: the game's frame render is run once per eye (and once more for the scope
// picture), plus the worker wake-up hook and the frame timing / GPU profile lines.
#include <windows.h>
#include <utility>
#include <d3d11.h>
#include <stdio.h>

#include "fc3vr.h"


static const DWORD kImageBase = 0x10000000;

// Inline hook: the first 6 bytes (push ebp; mov ebp, esp; and esp, -n) move to a trampoline.
// bd2: keep = how many whole instructions' bytes move (Blood Dragon's prologue is push ebx; mov ebx, esp;
// push ecx; push ecx = 5), and the DLL is passed in.
static void *InlineHook(DWORD va, const BYTE *prologue, int len, void *hook, const char *what, int keep, BYTE *m) {
    BYTE *target = BdAt(m, va);
    if (!m || memcmp(target, prologue, len)) {
        Log("stereo: %s doesn't match; not hooked", what);
        return nullptr;
    }
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, target, keep);
    t[keep] = 0xe9;
    *(DWORD *)(t + keep + 1) = (DWORD)(target + keep) - (DWORD)(t + keep + 5);
    BYTE patch[8];
    memcpy(patch, target, 8);
    patch[0] = 0xe9;
    *(DWORD *)(patch + 1) = (DWORD)hook - (DWORD)(target + 5);
    if (keep > 5) patch[5] = 0x90;
    if (!PatchBytes(target, patch, 8)) {
        Log("stereo: %s could not be patched; not hooked", what);
        return nullptr;
    }
    Log("stereo: %s hooked", what);
    return t;
}

// Render-twice stereo (Numpad 9): the renderer's per-frame draw draws everything for one camera,
// ending with the back buffer. Run it for the left eye, keep the back buffer, run it again for
// the right eye; Present then sends both to the headset. (kBdRender below; Far Cry 3's was 0x115c1a80.)
typedef void(__stdcall *RenderFn)(void *, DWORD, DWORD, DWORD);
static RenderFn g_origRender;
static volatile bool g_twice = true;  // Numpad 9; on from the start (DrGonzaga keeps it on)
static LONG g_twiceFrames, g_heldFrames;

static LARGE_INTEGER g_qpf;
static double Ms(LARGE_INTEGER a, LARGE_INTEGER b) { return double(b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart; }
static volatile int g_phase;  // 1 while drawing the left eye, 2 while drawing the scope picture (pip1)

static volatile bool g_rightEye;

// The render call ends with the game's own Present, so the left eye's
// Present is caught (StereoLeftPresent): its image is kept for the headset
// and it never reaches the screen, and the pose isn't refreshed between eyes.
static DWORD g_renderThread;
DWORD StereoRenderThread() { return g_renderThread; }

// crash4: where in the two-eye render the render thread was when it crashed.
// onehud1 (hud.cpp): which eye the frame render is drawing now: 0 left, 1 right, -1 neither (single render, or between frames).
int StereoEyeNow() { return g_phase == 1 ? 0 : g_rightEye ? 1 : -1; }
void StereoCrashState(char *out, int n) {
    snprintf(out, n, "%s%s", g_phase == 1 ? "drawing the left eye" : g_rightEye ? "drawing the right eye" : "outside the eye renders",
             g_twice ? "" : " (single render)");
}

// Timing (logged every 10 s while render-twice is on): CPU time of each
// eye's render call, the game's own work between frames, the headset wait,
// and GPU time per eye from timestamp queries.
static double g_tLeft, g_tRight, g_tGap, g_tXr, g_gLeft, g_gRight;
static LONG g_tCount, g_gCount;
static LARGE_INTEGER g_lastExit;
static LONG g_hitches, g_hitchLogs;
static double g_hitchWorst, g_prevL, g_prevR, g_prevXr;
static double g_xrNow;
// Smoothed for the menu (menu.cpp): CPU left, CPU right, headset wait, GPU left, GPU right (ms).
static float g_ema[5];
void StereoFrameTimes(float out[5]) { memcpy(out, g_ema, sizeof g_ema); }
// pip1: timings of scoped frames, kept apart so the scope picture's cost can be read: [1] scoped the scope1 way
// (right eye = scope view, flat), [2] scoped with the third render (scope picture, both eyes stereo).
static struct PipStat {
    double cpuS, cpuL, cpuR, xr, gpuS, gpuL, gpuR;
    LONG n, gn, late;
} g_pip[3];
static float g_pipEma[2];  // CPU, GPU of the scope picture (menu)
static void PipGpu(int kind, double s, double l, double r) {
    PipStat &p = g_pip[kind];
    p.gpuS += s, p.gpuL += l, p.gpuR += r, ++p.gn;
    if (kind == 2) g_pipEma[1] += 0.05f * (float(s) - g_pipEma[1]);
}
void StereoScopeTimes(float out[2]) { memcpy(out, g_pipEma, sizeof g_pipEma); }
bool StereoScopePassNow() { return g_phase == 2; }
bool StereoTwice() { return g_twice; }
void StereoSetTwice(bool on) {
    if (on != g_twice) g_twice = on, Log("stereo: render-twice stereo %s (menu)", on ? "ON" : "OFF");
}

void StereoNoteXrTime(double ms) { g_xrNow += ms; }

// The right eye's GPU time ends at its Present, before the headset wait
// (which used to be counted in it).
static struct GpuSet *g_gpuCur;
static ID3D11DeviceContext *g_gpuCtx;
static void GpuEndRight();
void StereoBeforeXr() { GpuEndRight(); }

static struct GpuSet {
    ID3D11Query *disjoint, *ts[3];
    ID3D11Query *tsScope;  // pip1: end of the scope picture (it runs before the left eye)
    bool pending, scope;
    BYTE kind;  // pip1: 0 not scoped, 1 scoped the scope1 way (flat), 2 scoped with the scope picture (pip1)
    // gpu3: a timestamp at every render-target change and command list on
    // the render thread, so GPU time can be split by pass.
    ID3D11Query *sq[1024];
    short slabel[1024];
    BYTE seye[1024];
    int ns;
} g_gpu[4];
static char g_segName[400][56];
static long g_segCnt[2][400];
static int g_nseg;
static double g_segMs[2][400];
static LONG g_segFrames;
static volatile bool g_segOn;
static DWORD g_segUntil;
static int g_gpuNext;
static GpuSet *g_gpuFrame;  // the set being filled this frame

static ID3D11Query *MakeQuery(ID3D11Device *dev, D3D11_QUERY type) {
    D3D11_QUERY_DESC d = {type, 0};
    ID3D11Query *q = nullptr;
    dev->CreateQuery(&d, &q);
    return q;
}

static int SegLabel(const char *name) {
    for (int i = 0; i < g_nseg; ++i)
        if (!strcmp(g_segName[i], name)) return i;
    if (g_nseg >= 400) return 399;
    lstrcpynA(g_segName[g_nseg], name, 56);
    return g_nseg++;
}

void StereoGpuMark(const char *name) {
    GpuSet *g = g_gpuFrame;
    if (!g_segOn || !g || !g_gpuCtx || GetCurrentThreadId() != g_renderThread) return;
    if (g->ns >= 1024 || !g->sq[g->ns]) return;
    int k = g->ns++;
    g->slabel[k] = (short)SegLabel(name), g->seye[k] = g_rightEye ? 1 : 0;
    ++g_segCnt[g->seye[k]][g->slabel[k]];
    g_gpuCtx->End(g->sq[k]);
}
bool StereoGpuProfiling() { return g_segOn && g_gpuFrame && GetCurrentThreadId() == g_renderThread; }
void StereoGpuProfile() {
    g_segUntil = GetTickCount() + 10000;
    g_segOn = true;
    Log("stereo: GPU pass profile for 10 s");
}

static void GpuEndRight() {
    if (g_gpuCur && g_gpuCtx && GetCurrentThreadId() == g_renderThread) StereoGpuMark("(end of eye)");
    if (g_gpuCur && g_gpuCtx && GetCurrentThreadId() == g_renderThread) g_gpuCtx->End(g_gpuCur->ts[2]), g_gpuCur = nullptr;
}

static GpuSet *GpuBegin(ID3D11DeviceContext *ctx) {
    if (!ctx) return nullptr;
    // Collect the oldest set first.
    for (auto &g : g_gpu) {
        if (!g.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t[3], tS = 0;
        if (ctx->GetData(g.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        bool ok = !dj.Disjoint;
        for (int i = 0; i < 3 && ok; ++i)
            ok = ctx->GetData(g.ts[i], &t[i], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (ok && g.scope) ok = g.tsScope && ctx->GetData(g.tsScope, &tS, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        g.pending = false;
        if (!ok) continue;
        {   // pip1: the scope picture's own GPU time (it runs first); the left eye starts after it
            const double f = 1000.0 / dj.Frequency, sMs = g.scope && tS >= t[0] ? double(tS - t[0]) * f : 0.0;
            if (g.scope) t[0] = tS;
            if (g.kind) PipGpu(g.kind, sMs, double(t[1] - t[0]) * f, double(t[2] - t[1]) * f);
        }
        if (g.ns > 1) {
            UINT64 prev = 0;
            bool sok = true;
            for (int k = 0; k < g.ns && sok; ++k) {
                UINT64 t2;
                sok = ctx->GetData(g.sq[k], &t2, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (sok && k && g.seye[k] == g.seye[k - 1] && t2 >= prev)
                    g_segMs[g.seye[k - 1]][g.slabel[k - 1]] += double(t2 - prev) * 1000.0 / dj.Frequency;
                prev = t2;
            }
            if (sok) ++g_segFrames;
        }
        g_gLeft += double(t[1] - t[0]) * 1000.0 / dj.Frequency;
        g_gRight += double(t[2] - t[1]) * 1000.0 / dj.Frequency;
        g_ema[3] += 0.05f * (float(double(t[1] - t[0]) * 1000.0 / dj.Frequency) - g_ema[3]);
        g_ema[4] += 0.05f * (float(double(t[2] - t[1]) * 1000.0 / dj.Frequency) - g_ema[4]);
        ++g_gCount;
    }
    GpuSet &g = g_gpu[g_gpuNext];
    if (g.pending) return nullptr;
    if (!g.disjoint) {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return nullptr;
        g.disjoint = MakeQuery(dev, D3D11_QUERY_TIMESTAMP_DISJOINT);
        for (auto &q : g.ts) q = MakeQuery(dev, D3D11_QUERY_TIMESTAMP);
        g.tsScope = MakeQuery(dev, D3D11_QUERY_TIMESTAMP);
        dev->Release();
        if (!g.disjoint || !g.ts[0] || !g.ts[1] || !g.ts[2]) return nullptr;
    }
    if (g_segOn && !g.sq[0]) {  // lazygpu1 (bd127): a set's 1,024 per-pass queries only once the 10 s profile is asked for
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            for (auto &q : g.sq) q = MakeQuery(dev, D3D11_QUERY_TIMESTAMP);
            dev->Release();
            Log("stereo: GPU pass profile queries made for set %d", (int)(&g - g_gpu));
        }
    }
    g_gpuNext = (g_gpuNext + 1) & 3;
    g.ns = 0, g.scope = false, g.kind = 0;
    ctx->Begin(g.disjoint);
    ctx->End(g.ts[0]);
    return &g;
}

// bd2: Blood Dragon's frame render (0x100db864) is thiscall with three arguments (ret 0xc)
// where Far Cry 3's is stdcall with four; the renderer object and its fields are the same.
static const DWORD kBdRender = 0x100db864;
static const BYTE kBdRenderPrologue[17] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55, 0x8b, 0x6b, 0x04, 0x89, 0x6c};
typedef void(__attribute__((thiscall)) *RenderFnBd)(void *, DWORD, DWORD, DWORD);
static inline void OrigRender(void *renderer, DWORD a, DWORD b, DWORD c) { BdThisCall((void *)g_origRender, renderer, 3, a, b, c); }
static void RenderBody(void *renderer, DWORD a, DWORD b, DWORD c) {
    g_renderThread = GetCurrentThreadId();
    if (g_twice && EngineActive() && !EngineFlat()) {  // flat1: the 2D screen needs one picture a frame
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        ID3D11DeviceContext *ctx = TraceContext();
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        GpuSet *g = GpuBegin(ctx);
        g_gpuFrame = g, g_gpuCtx = ctx;
        g_xrNow = 0;
        // pip1: the scope picture first (a third render, eye 2), so both eyes of this frame get this frame's lens.
        // Its Present is kept off the headset (hook.cpp).
        const bool pip = EngineScopePipWanted(), mono = !pip && EngineScopeMono();
        LARGE_INTEGER tS0 = t0;
        if (pip) {
            EngineForceEye(2);
            g_phase = 2;
            StereoGpuMark("(start of scope picture)");
            OrigRender(renderer, a, b, c);
            StereoGpuMark("(end of scope picture)");
            g_phase = 0;
            if (g && g->tsScope) ctx->End(g->tsScope), g->scope = true;
            QueryPerformanceCounter(&t0);
        }
        if (g) g->kind = pip ? 2 : mono ? 1 : 0;
        EngineForceEye(0);
        g_phase = 1;
        StereoGpuMark("(start of eye)");
        OrigRender(renderer, a, b, c);
        StereoGpuMark("(end of eye)");
        g_phase = 0;
        if (g) ctx->End(g->ts[1]);
        QueryPerformanceCounter(&t1);
        EngineForceEye(1);
        g_gpuCur = g, g_gpuCtx = ctx;
        g_rightEye = true;
        StereoGpuMark("(start of eye)");
        OrigRender(renderer, a, b, c);
        g_rightEye = false;
        EngineForceEye(-1);
        GpuEndRight();  // if the right eye's Present didn't
        g_gpuFrame = nullptr;
        if (g) ctx->End(g->disjoint), g->pending = true;
        QueryPerformanceCounter(&t2);
        // Stutters: a frame that started more than 1.5 headset frames (16.7 ms) after the last one.
        static LARGE_INTEGER lastStart;
        if (pip || mono) {  // pip1: scoped frames' own timings
            PipStat &p = g_pip[pip ? 2 : 1];
            p.cpuS += Ms(tS0, t0), p.cpuL += Ms(t0, t1), p.cpuR += Ms(t1, t2) - g_xrNow, p.xr += g_xrNow, ++p.n;
            if (lastStart.QuadPart && Ms(lastStart, tS0) > 13.9) ++p.late;  // over 1.25 headset frames
            if (pip) g_pipEma[0] += 0.05f * (float(Ms(tS0, t0)) - g_pipEma[0]);
        }
        t0 = tS0;  // the frame's own start (the scope picture is part of the left eye's time below)
        if (lastStart.QuadPart) {
            double iv = Ms(lastStart, t0);
            if (iv > 16.7) {
                ++g_hitches;
                if (iv > g_hitchWorst) g_hitchWorst = iv;
                if (g_hitchLogs < 10)
                    ++g_hitchLogs, Log("stereo: stutter: %.1f ms since the last frame (that frame: left eye %.2f, right %.2f, "
                                       "headset wait %.2f; game between frames %.2f)",
                                       iv, g_prevL, g_prevR, g_prevXr, g_lastExit.QuadPart ? Ms(g_lastExit, t0) : 0.0);
            }
        }
        lastStart = t0;
        g_prevL = Ms(t0, t1), g_prevR = Ms(t1, t2) - g_xrNow, g_prevXr = g_xrNow;
        if (g_lastExit.QuadPart) g_tGap += Ms(g_lastExit, t0);
        g_tLeft += Ms(t0, t1);
        g_tRight += Ms(t1, t2) - g_xrNow;
        g_ema[0] += 0.05f * (float(Ms(t0, t1)) - g_ema[0]);
        g_ema[1] += 0.05f * (float(Ms(t1, t2) - g_xrNow) - g_ema[1]);
        g_ema[2] += 0.05f * (float(g_xrNow) - g_ema[2]);
        g_tXr += g_xrNow;
        ++g_tCount;
        g_lastExit = t2;
        InterlockedIncrement(&g_twiceFrames);
    } else {
        g_lastExit.QuadPart = 0;
        OrigRender(renderer, a, b, c);
    }
}

static void __attribute__((thiscall)) HookRenderBd(void *renderer, DWORD a, DWORD b, DWORD c) { RenderBody(renderer, a, b, c); }

bool StereoLeftPresent() {
    if (g_phase != 1) return false;
    g_phase = 0;
    int eye = -1;
    float q[4], p[3];
    bool held = EngineLastEye(&eye, q, p) && eye == 0 && XrHoldLeftEye(q, p);
    if (held) InterlockedIncrement(&g_heldFrames);
    return held;
}

// Worker wake-ups (Numpad 3): the job system's kick (0x106c9570, thiscall,
// mask on the stack) goes through all 12 workers and wakes every sleeping
// one with SetEvent. prof3: ~3500 SetEvents a frame, about a third of the
// frame on the render thread. Workers spin a little, then sleep: state
// (+0x110) 1 = running, 0 = asleep on the event (+0x114), 2 = kicked.
// This version wakes at most g_wakeCap sleeping workers per kick; running
// ones still get "kicked" (1 -> 2, so they look for work again before
// sleeping), and sleeping ones beyond the cap are left asleep. Woken workers
// kick again as they take jobs, so more wake up when there's more work.
static const DWORD kKick = 0x106c9570, kKickMask = 0x121a1798;
static const BYTE kKickPrologue[7] = {0x8b, 0x44, 0x24, 0x04, 0x57, 0x8b, 0xf9};
static const int kWakeCap = 1;  // wake1 test: 1 is best (0 would be the game's own behaviour: wake all)
static DWORD *g_kickMask;

// fast1 (12 Nov): with the cap of one, the 11 Nov CPU profile still has the SetEvent call
// at 10% of each eye's time: nearly every kick finds some worker asleep and wakes
// it, even while others are awake and about to look for work anyway. g_wakeKeep
// (Debug page, [perf] WakeKeep; 0 = as before) wakes a sleeper only while fewer
// than that many of the kick's workers are awake. Measured 11 Nov (same quiet spot,
// both eyes' CPU): 0 -> 7.2 ms, 1 -> 7.0, 3 -> 7.4, 5 -> 5.9, 6 -> 6.0; SetEvent fell
// from 10% to 2% of an eye. Low values do nothing because the awake count keeps
// dropping below them; with 5 the pool stays warm and takes jobs at once.
static volatile bool g_wakeFast = true;  // wakefast1: [perf] WakeFast
static volatile LONG g_fastWakes;
bool *StereoWakeFast() { return (bool *)&g_wakeFast; }
static volatile int g_wakeKeep = 5;  // lean1: 5 measured best on DrGonzaga's PC (0 = the old behaviour)
int *StereoWakeKeep() { return (int *)&g_wakeKeep; }

static void __attribute__((thiscall)) MyKick(BYTE *self, DWORD mask) {
    if (!(*g_kickMask & mask)) return;
    DWORD **b = *(DWORD ***)(self + 8), **e = *(DWORD ***)(self + 0xc);
    if (!b) return;
    // review1: no shared counters here (this runs on every worker thread, thousands of times a frame)
    const int keep = g_wakeKeep;
    if (keep > 0) {
        // wake3 (12 Nov): each worker empties its inbox (+0xe0) only when woken, and the game
        // pushes a note to every eligible worker's inbox whenever a job group turns busy. Workers
        // left asleep for good leaked 2-3 MB/s until the 32-bit address space ran out. So once per
        // tick of the system clock, the first kick to see the new tick wakes every sleeper,
        // whatever the mask, as the game's own kick would.
        static volatile LONG s_tick;
        const LONG now = (LONG)GetTickCount(), was = s_tick;
        if (now != was && InterlockedCompareExchange(&s_tick, now, was) == was) {
            for (DWORD **w = b; w < e; ++w) {
                BYTE *k = (BYTE *)*w;
                if (k && InterlockedExchange((LONG *)(k + 0x110), 2) == 0) SetEvent(*(HANDLE *)(k + 0x114));
            }
            return;
        }
        int awake = 0;
        BYTE *sleeper = nullptr;
        for (DWORD **w = b; w < e; ++w) {
            BYTE *k = (BYTE *)*w;
            if (!k || !(*(DWORD *)(k + 0xfc) & mask)) continue;
            // running -> kicked (it looks for work again before sleeping); already kicked counts too
            if (InterlockedCompareExchange((LONG *)(k + 0x110), 2, 1) != 0) ++awake;
            else if (!sleeper) sleeper = k;
        }
        if (awake < keep) {
            if (sleeper && InterlockedExchange((LONG *)(sleeper + 0x110), 2) == 0) SetEvent(*(HANDLE *)(sleeper + 0x114));
        } else if (sleeper && g_wakeFast) {
            // wakefast1 (DrGonzaga 15 Nov: the first shot after loading drops the picture to 3-5 fps for about a
            // second, while the bd66 log shows every frame presented on time: the game's update was crawling and the
            // old picture was shown again). "Awake" counts workers that are busy as well. When "keep" of them are
            // tied up in long jobs (loading what the first shot needs), nobody was woken for new work until the
            // once-a-clock-tick wake above, and that clock moves every 15.6 ms: each step of the game's update
            // waited that long. Now, when enough look awake, one sleeper is still woken about every 0.3 ms.
            static volatile LONG s_ft;
            const LONG ft = (LONG)(__builtin_ia32_rdtsc() >> 20), w2 = s_ft;
            if (ft != w2 && InterlockedCompareExchange(&s_ft, ft, w2) == w2 && InterlockedExchange((LONG *)(sleeper + 0x110), 2) == 0)
                SetEvent(*(HANDLE *)(sleeper + 0x114)), InterlockedIncrement(&g_fastWakes);
        }
        return;
    }
    const int cap = kWakeCap;
    int woken = 0;
    for (DWORD **w = b; w < e; ++w) {
        BYTE *k = (BYTE *)*w;
        if (!k || !(*(DWORD *)(k + 0xfc) & mask)) continue;
        LONG *st = (LONG *)(k + 0x110);
        if (cap && woken >= cap) {
            InterlockedCompareExchange(st, 2, 1);  // running: look again before sleeping; asleep: leave it
            continue;
        }
        if (InterlockedExchange(st, 2) == 0) {
            SetEvent(*(HANDLE *)(k + 0x114));
            ++woken;
        }
    }
}

// fast1: after locking a buffer to write vertices into, the game checks the whole
// range with IsBadWritePtr (FC3+1154fc78, its only use), which touches every page of
// it: 3.6% of each eye's CPU time in the 11 Nov CPU profile. Direct3D has just handed the
// game that pointer; "is it null" is the only check with any meaning.
static bool g_fastCheck = true;  // [perf] FastBufferCheck
bool *StereoFastCheck() { return &g_fastCheck; }
static BOOL(WINAPI *g_origIsBad)(LPVOID, UINT_PTR);
static BOOL WINAPI FastIsBadWritePtr(LPVOID p, UINT_PTR n) {
    if (!g_fastCheck) return g_origIsBad(p, n);
    return p == nullptr;
}
static void HookBufferCheck() {
    HMODULE m = GetModuleHandleW(L"FC3_d3d11.dll");
    bool ok = m && PatchImport(m, "KERNEL32.dll", "IsBadWritePtr", (void *)FastIsBadWritePtr, (void **)&g_origIsBad);
    Log("stereo: the game's buffer check %s", ok ? "replaced by a quick one" : "not found; left as it is");
}

// bd2: Blood Dragon's kick is byte for byte the same function (0x100136c0, mask 0x1182c218).
static const DWORD kBdKick = 0x100136c0, kBdKickMask = 0x1182c218;
static void HookKick() {
    BYTE *m = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    const DWORD kick = g_bloodDragon ? kBdKick : kKick, mask = g_bloodDragon ? kBdKickMask : kKickMask;
    BYTE *t = BdAt(m, kick);
    // the whole "test [mask], eax" must match too: 85 05 <mask address>
    if (!m || memcmp(t, kKickPrologue, sizeof kKickPrologue) || t[7] != 0x85 || t[8] != 0x05 ||
        *(DWORD *)(t + 9) != (DWORD)BdAt(m, mask)) {
        Log("stereo: worker kick doesn't match; not hooked");
        return;
    }
    g_kickMask = (DWORD *)(BdAt(m, mask));
    BYTE patch[8];
    memcpy(patch, t, 8);
    patch[0] = 0xe9;
    *(DWORD *)(patch + 1) = (DWORD)MyKick - (DWORD)(t + 5);
    PatchBytes(t, patch, 8);
    Log("stereo: worker kick hooked (wake at most %d per kick)", kWakeCap);
}

static void HookBuilder() {
    static bool tried;
    if (tried) return;
    tried = true;
    if (BdStage() < 2) {
        Log("stereo: Blood Dragon stage %d: render-twice stereo and worker wake-ups not hooked", BdStage());
        g_twice = false;
        return;
    }
    HookKick();
    HookBufferCheck();
    g_origRender = (RenderFn)InlineHook(kBdRender, kBdRenderPrologue, sizeof kBdRenderPrologue, (void *)HookRenderBd,
                                        "frame render (Blood Dragon)", 5, (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll"));
}

void StereoOnPresent() {
    static DWORD start;
    static bool dumped;
    if (!EngineActive()) return;
    if (!start) start = GetTickCount();
    if (!dumped && GetTickCount() - start > 1000) dumped = true, HookBuilder();  // was 20 s (left from the view probe): stereo started late


    // Numpad 9: render-twice stereo.
    if (KeyHit(VK_NUMPAD9) && g_origRender) {
        g_twice = !g_twice;
        Log("stereo: render-twice stereo %s (Numpad 9)", g_twice ? "ON" : "OFF");
    }
    static DWORD lastLog;
    if (g_twice && GetTickCount() - lastLog > 10000) {
        lastLog = GetTickCount();
        LogV("stereo: render-twice: %ld frames, %ld left eyes held (last 10 s)", InterlockedExchange(&g_twiceFrames, 0),
            InterlockedExchange(&g_heldFrames, 0));
        if (g_tCount) {
            Log("stereo: timing per frame (ms): CPU left eye %.2f, right eye %.2f, headset wait %.2f, game between "
                "frames %.2f; GPU left %.2f, right %.2f (%ld GPU samples) [wake %d]",
                g_tLeft / g_tCount, g_tRight / g_tCount, g_tXr / g_tCount, g_tGap / g_tCount,
                g_gCount ? g_gLeft / g_gCount : 0.0, g_gCount ? g_gRight / g_gCount : 0.0, g_gCount, (int)g_wakeKeep);
        }
            for (int k = 1; k <= 2; ++k) {  // pip1
                PipStat &p = g_pip[k];
                if (!p.n) continue;
                Log("stereo: scoped frames %s: %ld frames, %ld late; CPU scope picture %.2f, left %.2f, right %.2f, headset wait %.2f; "
                    "GPU scope picture %.2f, left %.2f, right %.2f (%ld GPU samples)",
                    k == 2 ? "WITH the scope picture (both eyes 3D)" : "the old way (flat while scoped)", p.n, p.late, p.cpuS / p.n,
                    p.cpuL / p.n, p.cpuR / p.n, p.xr / p.n, p.gn ? p.gpuS / p.gn : 0.0, p.gn ? p.gpuL / p.gn : 0.0,
                    p.gn ? p.gpuR / p.gn : 0.0, p.gn);
                p = PipStat();
            }
            {
                LONG fw = InterlockedExchange(&g_fastWakes, 0);
                if (fw) Log("stereo: workers: %ld extra wake-ups while enough looked awake (wakefast %s)", fw, g_wakeFast ? "on" : "off");
            }
        if (g_tCount) {
            if (g_segOn && GetTickCount() > g_segUntil) {
                g_segOn = false;
                if (g_segFrames) {
                    static int ord[400];
                    for (int i = 0; i < g_nseg; ++i) ord[i] = i;
                    for (int i = 0; i < g_nseg; ++i)
                        for (int j = i + 1; j < g_nseg; ++j)
                            if (g_segMs[0][ord[j]] + g_segMs[1][ord[j]] > g_segMs[0][ord[i]] + g_segMs[1][ord[i]])
                                std::swap(ord[i], ord[j]);
                    double tl = 0, tr = 0;
                    for (int i = 0; i < g_nseg; ++i) tl += g_segMs[0][i], tr += g_segMs[1][i];
                    Log("stereo: GPU by pass over %ld frames (ms per frame, left/right; total %.2f/%.2f):", g_segFrames,
                        tl / g_segFrames, tr / g_segFrames);
                    for (int i = 0; i < g_nseg && i < 60; ++i)
                        Log("stereo:   %-52s %.3f / %.3f  (binds/frame %.1f / %.1f)", g_segName[ord[i]],
                            g_segMs[0][ord[i]] / g_segFrames, g_segMs[1][ord[i]] / g_segFrames,
                            g_segCnt[0][ord[i]] / double(g_segFrames), g_segCnt[1][ord[i]] / double(g_segFrames));
                }
                memset(g_segMs, 0, sizeof g_segMs), memset(g_segCnt, 0, sizeof g_segCnt), g_segFrames = 0;
            }
            Log("stereo: stutters (frames more than 16.7 ms apart): %ld, worst %.1f ms", InterlockedExchange(&g_hitches, 0),
                g_hitchWorst);
            g_hitchWorst = 0, g_hitchLogs = 0;
        }
        g_tLeft = g_tRight = g_tXr = g_tGap = g_gLeft = g_gRight = 0;
        g_tCount = g_gCount = 0;
    }

}
