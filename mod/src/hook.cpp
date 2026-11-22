// Phase 1: prove we load into the game and own its frames.
// Hooks IDXGISwapChain::Present through the shared vtable and logs what the
// game renders, plus the facts we need later (module bases, memory flags).
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "fc3vr.h"

static wchar_t g_logPath[MAX_PATH];
static char g_exeName[MAX_PATH];

bool g_verboseLog;

// quiet1: lines go into a memory buffer and a background thread writes them
// out every 200 ms, so a slow disk or antivirus scan of fc3vr.log can never
// stall the render or game thread. Crash lines are written straight away.
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static SRWLOCK g_logBufLock = SRWLOCK_INIT;    // guards the buffer being filled
static SRWLOCK g_logWriteLock = SRWLOCK_INIT;  // whoever writes a buffer to disk
static char g_logBuf[2][64 * 1024];
static int g_logCur, g_logLen;
static LONG g_logThread;
static __thread int t_inCrashLog;  // our own probing must not re-enter (crashlog2 died in IsBadReadPtr)

static HANDLE LogFileHandle() {
    // log1: one handle kept open for the whole run. Opening and closing the
    // file for every line made Windows (antivirus) rescan the growing log after
    // each burst of 10 s stats: the 50-80 ms hitches every 20 s on 31 Oct.
    if (g_logFile == INVALID_HANDLE_VALUE)
        g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_logFile;
}

// Caller holds g_logWriteLock. Hands the filled buffer to disk.
static void LogFlushLocked(bool tryBuf) {
    if (tryBuf) {
        if (!TryAcquireSRWLockExclusive(&g_logBufLock)) return;
    } else {
        AcquireSRWLockExclusive(&g_logBufLock);
    }
    int i = g_logCur, n = g_logLen;
    g_logCur ^= 1;
    g_logLen = 0;
    ReleaseSRWLockExclusive(&g_logBufLock);
    if (!n) return;
    HANDLE f = LogFileHandle();
    DWORD written;
    if (f != INVALID_HANDLE_VALUE) WriteFile(f, g_logBuf[i], n, &written, nullptr);
}

void LogFlush() {
    if (!TryAcquireSRWLockExclusive(&g_logWriteLock)) return;  // never block (exit / crash paths)
    LogFlushLocked(true);
    ReleaseSRWLockExclusive(&g_logWriteLock);
}

static DWORD WINAPI LogWriterThread(LPVOID) {
    for (;;) {
        Sleep(200);
        AcquireSRWLockExclusive(&g_logWriteLock);
        LogFlushLocked(false);
        ReleaseSRWLockExclusive(&g_logWriteLock);
    }
}

static void LogWrite(const char *line, int n) {
    if (t_inCrashLog) {
        // Crash: the process may be about to die, so get everything onto disk now,
        // without waiting on a lock this thread might already hold.
        if (TryAcquireSRWLockExclusive(&g_logWriteLock)) {
            LogFlushLocked(true);
            HANDLE f = LogFileHandle();
            DWORD written;
            if (f != INVALID_HANDLE_VALUE) WriteFile(f, line, n, &written, nullptr);
            ReleaseSRWLockExclusive(&g_logWriteLock);
        }
        return;
    }
    if (!g_logThread && !InterlockedExchange(&g_logThread, 1)) {
        HANDLE t = CreateThread(nullptr, 0, LogWriterThread, nullptr, 0, nullptr);
        if (t) SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL), CloseHandle(t);
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_logBufLock);
        if (g_logLen + n <= (int)sizeof g_logBuf[0]) {
            memcpy(g_logBuf[g_logCur] + g_logLen, line, n);
            g_logLen += n;
            ReleaseSRWLockExclusive(&g_logBufLock);
            return;
        }
        ReleaseSRWLockExclusive(&g_logBufLock);
        // Full (a big burst): write it out here rather than lose lines.
        AcquireSRWLockExclusive(&g_logWriteLock);
        LogFlushLocked(false);
        ReleaseSRWLockExclusive(&g_logWriteLock);
    }
}

void Log(const char *fmt, ...) {
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\r';
    line[n++] = '\n';

    LogWrite(line, n);
}

static DWORD ImageSize(HMODULE m) {
    auto dos = (IMAGE_DOS_HEADER *)m;
    auto nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

static bool IsLargeAddressAware() {
    auto m = GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER *)m;
    auto nt = (IMAGE_NT_HEADERS *)((BYTE *)m + dos->e_lfanew);
    return nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE;
}

// ---- Present hook ----

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef ULONG(STDMETHODCALLTYPE *RefFn)(IDXGISwapChain *);
static PresentFn g_origPresent;
static ResizeBuffersFn g_origResize;
static RefFn g_origAddRef, g_origRelease;

static LONG g_frames;
static LARGE_INTEGER g_freq, g_lastStamp;
static LONG g_lastFrames;
static IDXGISwapChain *g_lastChain;
static ID3D11Device *g_gameDev;  // stable1: for the device-removed reason (not AddRef'd)

// stable1: largest free block of address space (a 32-bit game can fail to
// allocate big buffers while plenty is free in total, if it's fragmented).
static SIZE_T LargestFree() {
    SIZE_T best = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (BYTE *p = (BYTE *)0x10000; VirtualQuery(p, &mi, sizeof mi); p = (BYTE *)mi.BaseAddress + mi.RegionSize) {
        if (mi.State == MEM_FREE && mi.RegionSize > best) best = mi.RegionSize;
        if ((BYTE *)mi.BaseAddress + mi.RegionSize < p) break;
    }
    return best;
}


static DWORD WINAPI MemLogThread(void *) {  // stut1
    for (;;) {
        Sleep(10000);
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        PROCESS_MEMORY_COUNTERS_EX pc = {sizeof pc};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc);
        Log("memory: address space free %llu MB of %llu (largest block %lu MB), private %lu MB", ms.ullAvailVirtual >> 20,
            ms.ullTotalVirtual >> 20, (unsigned long)(LargestFree() >> 20), (unsigned long)(pc.PrivateUsage >> 20));
        // wake3 safety net: normal play has 2,200+ MB free; a leak from the wake throttle must not end in a crash
        if ((ms.ullAvailVirtual >> 20) < 1200 && *StereoWakeKeep() > 0) {
            *StereoWakeKeep() = 0;
            Log("[perf] WakeKeep switched OFF for this run: address space is running low (%llu MB free)", ms.ullAvailVirtual >> 20);
        }
    }
}

static void LogSwapChain(IDXGISwapChain *sc) {
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d))) return;
    Log("swapchain %p: %ux%u format=%d buffers=%u windowed=%d swapEffect=%d msaa=%u", (void *)sc,
        d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, d.Windowed,
        (int)d.SwapEffect, d.SampleDesc.Count);
}

// ms_hook_prologue gives our hooks the standard "mov edi,edi; push ebp;
// mov ebp,esp" start, so an overlay that inline-hooks whatever the vtable
// points at (the Ubisoft overlay does this on the first frame) can patch us
// safely instead of copying a prologue it can't relocate.
#define HOOKABLE __attribute__((ms_hook_prologue, noinline))

int g_mirrorEvery = 1;  // mirror1: desktop window shows 1 in N frames
static HRESULT PresentInner(IDXGISwapChain *sc, UINT sync, UINT flags);
static HOOKABLE HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *sc, UINT sync, UINT flags) {
    // body1: draws made inside Present are ours (beam, HUD, menu), never hidden
    BodyPresent(true);
    HRESULT hr = PresentInner(sc, sync, flags);
    BodyPresent(false);
    return hr;
}
static HRESULT PresentInner(IDXGISwapChain *sc, UINT sync, UINT flags) {
    LONG f = InterlockedIncrement(&g_frames);
    bool verbose = f <= 3;
    if (sc != g_lastChain) {
        g_lastChain = sc;
        Log("first Present on new swapchain (frame %ld)", f);
        LogSwapChain(sc);
        {
            // The game's own immediate context: its vtable lives on the heap,
            // separate from the throwaway device's, so hook it here.
            ID3D11Device *dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(IID_ID3D11Device, (void **)&dev))) {
                g_gameDev = dev;
                ID3D11DeviceContext *ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    BodyHookContext(ctx);  // first: hotpatches the original functions the vtable hooks below call
                    HeadtrackHookContext(ctx, "game immediate");
                    TraceHookContext(ctx);
                    ctx->Release();
                }
                dev->Release();
            }
        }
    }
    if (verbose) Log("Present #%ld enter thread %lu sync=%u flags=0x%x", f, GetCurrentThreadId(), sync, flags);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double secs = double(now.QuadPart - g_lastStamp.QuadPart) / double(g_freq.QuadPart);
    if (secs >= 10.0) {
        // stut1 (2 Nov): the memory figures (a walk over the whole address space) took
        // ~13 ms on the render thread every 10 s, a stutter each time; now on a
        // low-priority thread of their own (MemLogThread).
        Log("frames=%ld fps=%.1f syncInterval=%u, gpu device state 0x%08lx (0 = fine)", f, (f - g_lastFrames) / secs, sync,
            g_gameDev ? (unsigned long)g_gameDev->GetDeviceRemovedReason() : 0ul);
        VrsLog();
        static bool s_memThread;
        if (!s_memThread) {
            s_memThread = true;
            HANDLE h = CreateThread(nullptr, 0, MemLogThread, nullptr, 0, nullptr);
            if (h) SetThreadPriority(h, THREAD_PRIORITY_LOWEST), CloseHandle(h);
        }
        g_lastStamp = now;
        g_lastFrames = f;
    }
    KeysPoll();  // keys1
    VrsOnPresent(sc);  // vrs1: coarse shading is for the game's passes only
    D3DState::ShareBegin();  // state2: the game's state is saved once for all of these
    // pip1: the scope picture (a third render before the eyes, stereo.cpp) is only kept for the lens: nothing is
    // drawn into it and it never reaches the headset or the screen. HudDraw still runs (it counts the HUD passes per picture).
    const bool scopePass = StereoScopePassNow();
    if (ScopeIsScopeImage()) ScopeCapture(sc);  // scope1: the right eye is the scope view while scoped (pip1: the scope picture)
    else if (!scopePass) ComfortDraw(sc), SlowFxDraw(sc), LaserDraw(sc), HolsterDraw(sc), AmmoDraw(sc), SlowmoDraw(sc);  // aim beam (and holster2 body marks) into this eye's image, before it is kept or shown
    HudDraw(sc);    // the game's HUD on its panel (hud.cpp)
    if (!scopePass) ScopeDraw(sc), MenuDraw(sc);  // scope1: the scope lens into the eye's image; debug menu panel, the same way
    D3DState::ShareEnd();
    LaserEyeDone();  // occl2: this eye's depth views are let go
    if (scopePass) return S_OK;  // pip1
    ShotsOnPresent(sc);  // F10: screenshots to bin\\fc3vr_shots (scope2: back on request)
    if (StereoLeftPresent()) return S_OK;  // render-twice stereo: left eye kept, not shown
    HeadtrackBeforeSubmit();
    {
        LARGE_INTEGER a, b, fq;
        StereoBeforeXr();
        QueryPerformanceCounter(&a);
        XrOnPresent(sc);
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&fq);
        StereoNoteXrTime(double(b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart);
    }
    HeadtrackOnPresent();
    StereoOnPresent();
    // mirror1: showing the eye image in the desktop window costs GPU time
    // (Windows scales the large image into the window). Show it only every
    // Nth frame when the menu says so; the headset gets every frame anyway.
    {
        static LONG s_n;
        int every = g_mirrorEvery;
        if (every > 1 && XrSessionRunning() && (++s_n % every) != 0) return S_OK;
    }
    HRESULT hr = g_origPresent(sc, sync, flags);
    if (verbose) Log("Present #%ld returned 0x%08lx", f, (unsigned long)hr);
    return hr;
}

static HOOKABLE HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain *sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                            UINT flags) {
    Log("ResizeBuffers %ux%u format=%d", w, h, (int)fmt);
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d))) {
        if (w == 0 || h == 0) {  // 0 means "the window's size"
            RECT rc;
            GetClientRect(d.OutputWindow, &rc);
            if (!w) w = rc.right;
            if (!h) h = rc.bottom;
        }
        ResForce(d.OutputWindow, &w, &h);
    }
    XrOnSwapChainGoing(sc, "resizing");
    SmaaOnSwapChainGoing();  // smaa1: let go of the game's image so it can be resized
    LaserOnSwapChainGoing();
    ScopeOnSwapChainGoing();
    HudOnSwapChainGoing();
    MenuOnSwapChainGoing();
    VrsOnSwapChainGoing();
    return g_origResize(sc, count, w, h, fmt, flags);
}

// Catch the game's final Release of its swapchain (on exit or a display mode
// change) so the headset session shuts down while the device is still alive.
static HOOKABLE ULONG STDMETHODCALLTYPE HookRelease(IDXGISwapChain *sc) {
    if (sc == g_lastChain) {
        ULONG refs = g_origAddRef(sc);
        g_origRelease(sc);
        if (refs == 2) {
            Log("game swapchain %p being destroyed", (void *)sc);
            XrOnSwapChainGoing(sc, "destroyed");
            LaserOnSwapChainGoing();
            ScopeOnSwapChainGoing();
            HudOnSwapChainGoing();
            MenuOnSwapChainGoing();
            VrsOnSwapChainGoing();
            g_lastChain = nullptr;
        }
    }
    return g_origRelease(sc);
}

static bool PatchVtable(void **vtable, int index, void *hook, void **orig) {
    *orig = vtable[index];
    return PatchBytes(&vtable[index], &hook, sizeof(void *));
}

static void ModuleNameOf(void *addr, wchar_t *out, DWORD len) {
    HMODULE m;
    wcscpy(out, L"?");
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m)) {
        wchar_t full[MAX_PATH];
        GetModuleFileNameW(m, full, MAX_PATH);
        const wchar_t *b = wcsrchr(full, L'\\');
        wcsncpy(out, b ? b + 1 : full, len - 1);
        out[len - 1] = 0;
    }
}

// Overlays (Ubisoft, Steam, RTSS, ...) may have patched the vtable before us
// and re-patch it later, saving our hook as "their original". If we then call
// them as *our* original, the two hooks call each other until the stack
// overflows. So we always call dxgi's own function, read from the unpatched
// copy of the vtable in dxgi.dll on disk.
static void *DiskVtableEntry(void **slot) {
    HMODULE mod;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)slot, &mod))
        return nullptr;
    DWORD size = ImageSize(mod);
    DWORD rva = (DWORD)((BYTE *)slot - (BYTE *)mod);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(mod, path, MAX_PATH);
    HMODULE img = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!img) return nullptr;
    BYTE *base = (BYTE *)((ULONG_PTR)img & ~(ULONG_PTR)3);
    auto nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    ULONG_PTR preferred = nt->OptionalHeader.ImageBase;
    ULONG_PTR onDisk = *(ULONG_PTR *)(base + rva);
    FreeLibrary(img);

    ULONG_PTR real = onDisk - preferred + (ULONG_PTR)mod;
    if (real < (ULONG_PTR)mod || real >= (ULONG_PTR)mod + size) return nullptr;
    return (void *)real;
}

bool HookSlot(void **vtable, int index, void *hook, void **orig, const char *what) {
    void *current = vtable[index];
    void *real = DiskVtableEntry(&vtable[index]);
    wchar_t owner[64];
    ModuleNameOf(current, owner, 64);
    Log("%s: vtable slot holds %p (%ls), original %p%s", what, current, owner, real,
        real && real != current ? " - another hook was already here" : "");
    if (!PatchVtable(vtable, index, hook, orig)) return false;
    if (real) *orig = real;
    return true;
}

// Make a throwaway device + swapchain on a hidden window just to find the
// swapchain vtable that dxgi.dll shares with the game's own swapchain.
static bool InstallPresentHook() {
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create) {
        Log("D3D11CreateDeviceAndSwapChain not found");
        return false;
    }

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fc3vr_dummy";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                               wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *sc = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                        &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        void **vtable = *(void ***)sc;
        g_origAddRef = (RefFn)DiskVtableEntry(&vtable[1]);
        if (!g_origAddRef) g_origAddRef = (RefFn)vtable[1];
        ok = HookSlot(vtable, 2, (void *)HookRelease, (void **)&g_origRelease, "Release") &&
             HookSlot(vtable, 8, (void *)HookPresent, (void **)&g_origPresent, "Present") &&
             HookSlot(vtable, 13, (void *)HookResize, (void **)&g_origResize, "ResizeBuffers");
        Log("swapchain vtable %p, Present hook %s", (void *)vtable, ok ? "installed" : "FAILED");
    } else {
        Log("dummy device creation failed hr=0x%08lx", (unsigned long)hr);
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

static void LogModule(const wchar_t *name) {
    HMODULE m = GetModuleHandleW(name);
    if (m)
        Log("module %ls base=%p size=0x%lx", name, (void *)m, (unsigned long)ImageSize(m));
    else
        Log("module %ls not loaded", name);
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

// Log access violations so a crash leaves a trace of where it happened.
static LONG g_crashLogs;

// Readable without touching it: VirtualQuery, so no exception can happen here.
static bool Readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (BYTE *)p + n <= (BYTE *)mi.BaseAddress + mi.RegionSize;
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (t_inCrashLog) return EXCEPTION_CONTINUE_SEARCH;
    // dmg2: Windows' own harmless, handled exceptions (device setup polling in
    // SPFILEQ/SETUPAPI, IsBadReadPtr probes in KERNEL32/KERNELBASE) used up all
    // 20 log slots on 2 Nov, so the real crash later went unrecorded. Skip them.
    {
        HMODULE sm = nullptr;
        wchar_t sn[MAX_PATH] = L"";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &sm))
            GetModuleFileNameW(sm, sn, MAX_PATH);
        const wchar_t *b = wcsrchr(sn, L'\\');
        b = b ? b + 1 : sn;
        if (code == EXCEPTION_ACCESS_VIOLATION && (!_wcsicmp(b, L"SPFILEQ.dll") || !_wcsicmp(b, L"SETUPAPI.dll") ||
                                                  !_wcsicmp(b, L"KERNEL32.dll") || !_wcsicmp(b, L"KERNELBASE.dll")))
            return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_crashLogs) > 20) return EXCEPTION_CONTINUE_SEARCH;
    t_inCrashLog = 1;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m))
        GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t *base = wcsrchr(name, L'\\');
    Log("exception 0x%08lx at %p (%ls+0x%lx) thread %lu", (unsigned long)code, addr, base ? base + 1 : name,
        (unsigned long)((BYTE *)addr - (BYTE *)m), GetCurrentThreadId());
    if (g_gameDev) {
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        Log("  gpu device state 0x%08lx (0 = fine), address space free %llu MB, largest block %lu MB",
            (unsigned long)g_gameDev->GetDeviceRemovedReason(), ms.ullAvailVirtual >> 20,
            (unsigned long)(LargestFree() >> 20));
    }
    // Outside any module (a call through freed memory, like the 0xdededede crash
    // of 30 Oct), or in the graphics driver / D3D (hud1 crash): say who called,
    // from the return addresses on the stack.
    bool gfx = base && (!_wcsicmp(base + 1, L"nvwgf2um.dll") || !_wcsicmp(base + 1, L"d3d11.dll"));
    if (!m || gfx) {
        CONTEXT *c = ep->ContextRecord;
        Log("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx,
            c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        BYTE *fc3 = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll"), *self = (BYTE *)&__ImageBase;
        DWORD fsz = fc3 ? ImageSize((HMODULE)fc3) : 0, ssz = ImageSize((HMODULE)self);
        DWORD tid = GetCurrentThreadId();
        Log("  thread %lu is %s", tid, tid == StereoRenderThread() ? "the render thread" : tid == EngineGameThread() ? "the game thread" : "another thread");
        if (tid == StereoRenderThread()) {
            char st[256];
            StereoCrashState(st, sizeof st);
            Log("  stereo: %s", st);
        }
        // Only within this thread's stack (TIB: StackBase at fs:[4]).
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        DWORD *sp = (DWORD *)c->Esp, *top = (DWORD *)tib->StackBase;
        if (sp < (DWORD *)tib->StackLimit || sp >= top) top = sp;
        for (int i = 0; i < 64 && sp + i + 8 <= top; i += 8)
            Log("  esp+%03x: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3],
                sp[i + 4], sp[i + 5], sp[i + 6], sp[i + 7]);
        int found = 0, other = 0;
        for (DWORD *q = sp; q < top && found < 32; ++q) {
            BYTE *v = (BYTE *)*q;
            bool inFc3 = fc3 && v >= fc3 + 0x1000 && v < fc3 + fsz, inSelf = v >= self + 0x1000 && v < self + ssz;
            if (!inFc3 && !inSelf) {
                // Code of another module (d3d11, the driver, the headset runtime): name it.
                HMODULE om = nullptr;
                MEMORY_BASIC_INFORMATION mi;
                if (other < 40 && v > (BYTE *)0x10000 && VirtualQuery(v, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                    (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)v, &om)) {
                    wchar_t on[MAX_PATH] = L"?";
                    GetModuleFileNameW(om, on, MAX_PATH);
                    const wchar_t *ob = wcsrchr(on, L'\\');
                    ++other;
                    Log("  stack +%03x: %ls+%lx", (unsigned)((q - sp) * 4), ob ? ob + 1 : on, (unsigned long)(v - (BYTE *)om));
                }
                continue;
            }
            ++found;
            if (inFc3) Log("  stack +%03x: FC3+%08lx", (unsigned)((q - sp) * 4), (unsigned long)(v - fc3 + 0x10000000));
            else Log("  stack +%03x: fc3vr+%lx", (unsigned)((q - sp) * 4), (unsigned long)(v - self));
        }
        // What ecx/esi pointed into (the freed object's neighbourhood may name it).
        DWORD regs[2] = {c->Edi, c->Eax};
        for (DWORD r : regs)
            if (Readable((void *)r, 32)) {
                DWORD *o = (DWORD *)r;
                Log("  [%08lx]: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", r, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7]);
            }
        // crash2 (8 Nov, FC3+114f2eab hunt): name the D3D textures the stack points at, so the
        // log says which of the game's render targets was being set up.
        if (void *tv = HudTextureVtable()) {
            int named = 0;
            DWORD *last = sp + 0x400 < top ? sp + 0x400 : top;
            for (DWORD *q = sp; q < last && named < 8; ++q) {
                DWORD p = *q;
                if (p < 0x10000 || (p & 3) || !Readable((void *)p, 4) || *(void **)p != tv) continue;
                bool dup = false;
                for (DWORD *k = sp; k < q; ++k) dup |= *k == p;
                if (dup) continue;
                D3D11_TEXTURE2D_DESC d = {};
                ((ID3D11Texture2D *)p)->GetDesc(&d);
                ++named;
                Log("  stack +%03x: texture %08lx %ux%u format %d, %u slices, %u mips, %u samples, bind 0x%x, misc 0x%x",
                    (unsigned)((q - sp) * 4), p, d.Width, d.Height, (int)d.Format, d.ArraySize, d.MipLevels, d.SampleDesc.Count,
                    d.BindFlags, d.MiscFlags);
            }
        }
    }
    t_inCrashLog = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The game's own top-level window: a sign that start-up probing is over and
// the real renderer is coming up.
static BOOL CALLBACK FindGameWindow(HWND w, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *(HWND *)out = w;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI MainThread(LPVOID) {
    GfxStartup();  // menu's next-launch graphics settings into GamerProfile.xml, before the game reads it
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastStamp);

    // Wait for the game's window and for D3D11, so we never change its load
    // order or hook a probe device that systemdetection.dll throws away.
    HWND gameWnd = nullptr;
    for (int i = 0; i < 1800; ++i) {
        if (!gameWnd) EnumWindows(FindGameWindow, (LPARAM)&gameWnd);
        if (gameWnd && GetModuleHandleW(L"d3d11.dll") && GetModuleHandleW(L"dxgi.dll")) break;
        Sleep(100);
    }
    if (!gameWnd || !GetModuleHandleW(L"d3d11.dll")) {
        Log("game window or d3d11.dll not seen after 180 s (window=%p); giving up.", (void *)gameWnd);
        return 0;
    }
    Log("game window %p up and d3d11.dll loaded; waiting 3 s", (void *)gameWnd);
    Sleep(3000);

    // Pin both so they can never unload while our hook points into them.
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"d3d11.dll", &pinned);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &pinned);
    LogModule(L"FC3_d3d11.dll");
    LogModule(L"dxgi.dll");
    LogModule(L"upc_r1_loader.dll");
    LogModule(L"uplay_r1_loader.dll");  // bd1: Blood Dragon's older Ubisoft loader
    InstallPresentHook();
    return 0;
}

bool g_bloodDragon;
static int BdIni(const wchar_t *key, int def) {
    wchar_t path[MAX_PATH];
    IniPath(path);
    return (int)GetPrivateProfileIntW(L"bd", key, def, path);
}
bool BdEngineOn() { static int v = -1; if (v < 0) v = BdIni(L"Engine", 1) != 0; return v; }
int BdStage() { static int v = -1; if (v < 0) v = BdIni(L"Stage", 9); return v; }

void Fc3vrStart(HMODULE self) {
    GetModuleFileNameW(self, g_logPath, MAX_PATH);
    wchar_t *slash = wcsrchr(g_logPath, L'\\');
    wcscpy(slash ? slash + 1 : g_logPath, L"fc3vr.log");
    {
        // log1: start a fresh log once it passes 8 MB (the old one is kept as fc3vr.old.log).
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &a) && (a.nFileSizeHigh || a.nFileSizeLow > (8u << 20))) {
            wchar_t old[MAX_PATH];
            wcscpy(old, g_logPath);
            wcscpy(wcsrchr(old, L'\\') + 1, L"fc3vr.old.log");
            MoveFileExW(g_logPath, old, MOVEFILE_REPLACE_EXISTING);
        }
    }

    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char *base = strrchr(exe, '\\');
    strcpy(g_exeName, base ? base + 1 : exe);

    wchar_t selfName[MAX_PATH];
    GetModuleFileNameW(self, selfName, MAX_PATH);
    const wchar_t *selfBase = wcsrchr(selfName, L'\\');

    g_bloodDragon = _strnicmp(g_exeName, "fc3_blooddragon", 15) == 0;  // bd1
    bool isGame = g_bloodDragon || _strnicmp(g_exeName, "farcry3", 7) == 0;
    // The build's name comes from the Makefile (make NAME=...), not from an edit here.
#ifndef FC3VR_BUILD
#define FC3VR_BUILD "dev"
#endif
    Log("fc3vr phase6 " FC3VR_BUILD " loaded as %ls into %s (LAA=%d)%s", selfBase ? selfBase + 1 : selfName, g_exeName,
        IsLargeAddressAware(), isGame ? "" : " - not the game, staying idle");
    if (g_bloodDragon) Log("game: Far Cry 3 Blood Dragon (engine hooks: %s, stage %d; only the hooks whose Blood Dragon addresses are known)", BdEngineOn() ? "on" : "off", BdStage());
    if (!isGame) return;
    {
        wchar_t ini[MAX_PATH];
        IniPath(ini);
        g_verboseLog = GetPrivateProfileIntW(L"debug", L"VerboseLog", 0, ini) != 0;
        Log("diagnostic logging %s ([debug] VerboseLog in fc3vr.ini)", g_verboseLog ? "on" : "off");
        g_safePtr = GetPrivateProfileIntW(L"debug", L"SafePtr", 1, ini) != 0;  // ptr1
        g_shareState = GetPrivateProfileIntW(L"debug", L"ShareState", 1, ini) != 0;  // state2
        if (!g_shareState) Log("overlays: each saves the game's drawing state itself ([debug] ShareState=0)");
        if (!g_safePtr) Log("memory checks: the old Windows calls ([debug] SafePtr=0)");
    }
    AddVectoredExceptionHandler(0, CrashLogger);
    ResStart();
    PrewarmStart();
    InputStart();
    SlowSndStart();  // slowsnd1: XAudio2 hooked before the game makes its sound engine
    CloseHandle(CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr));
}

// bd9: see fc3vr.h.
asm(".text\n.globl _BdThisCall\n_BdThisCall:\n"
    "    pushl %ebp\n    movl %esp, %ebp\n"
    "    movl 16(%ebp), %ecx\n"
    "1:  testl %ecx, %ecx\n    jle 2f\n"
    "    pushl 16(%ebp,%ecx,4)\n    decl %ecx\n    jmp 1b\n"
    "2:  movl 12(%ebp), %ecx\n"
    "    call *8(%ebp)\n"
    "    movl %ebp, %esp\n    popl %ebp\n    ret\n"
    ".globl _BdCdeclCall\n_BdCdeclCall:\n"
    "    pushl %ebp\n    movl %esp, %ebp\n"
    "    movl 12(%ebp), %ecx\n"
    "1:  testl %ecx, %ecx\n    jle 2f\n"
    "    pushl 12(%ebp,%ecx,4)\n    decl %ecx\n    jmp 1b\n"
    "2:  call *8(%ebp)\n"
    "    movl %ebp, %esp\n    popl %ebp\n    ret\n");
