// cpufix1 (GitHub #1, 23 Nov): Blood Dragon crashes at start-up on PCs with 32 or more logical
// processors, with or without the mod: exception 0xC0000005 at systemdetection.dll+0x112d4.
// RE (systemdetection.dll 1.0.0.8, fn +0x110c0): it walks the CPUs with "for (bit = 1; bit <= systemMask;
// bit = 1 << ++i)" from GetProcessAffinityMask, pinning the thread to each one and writing per-CPU bytes
// to stack arrays indexed by i. With bit 31 set in the mask, 1 << 32 is 1 again on x86 (the shift count
// wraps), so the loop never ends and i runs off the stack. Windows' ProcessorCountLie fix hides it by
// reporting fewer CPUs, but that fix also stops our version.dll loading.
// Here only systemdetection.dll's own GetProcessAffinityMask / GetSystemInfo are changed, and only on
// such PCs: it sees at most the first 16 CPUs (its hardware score only; the game's threads are untouched).
// [bd] CpuCountFix=0 turns it off.
#include <windows.h>

#include "fc3vr.h"

namespace {
typedef BOOL(WINAPI *GpamFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef void(WINAPI *GsiFn)(LPSYSTEM_INFO);
GpamFn g_gpam;
GsiFn g_gsi;
const DWORD kCap = 0xffff;
volatile LONG g_done;

BOOL WINAPI HookGpam(HANDLE p, PDWORD_PTR proc, PDWORD_PTR sys) {
    BOOL r = g_gpam(p, proc, sys);
    if (r && proc && sys) {
        *proc &= kCap, *sys &= kCap;
        if (!*sys) *sys = 1;
        if (!*proc) *proc = *sys;
    }
    return r;
}
void WINAPI HookGsi(LPSYSTEM_INFO si) {
    g_gsi(si);
    if (si) {
        si->dwActiveProcessorMask &= kCap;
        if (si->dwNumberOfProcessors > 16) si->dwNumberOfProcessors = 16;
    }
}
void Patch(HMODULE sd) {
    if (!sd || InterlockedExchange(&g_done, 1)) return;
    bool a = PatchImport(sd, "KERNEL32.dll", "GetProcessAffinityMask", (void *)HookGpam, (void **)&g_gpam);
    bool b = PatchImport(sd, "KERNEL32.dll", "GetSystemInfo", (void *)HookGsi, (void **)&g_gsi);
    Log("cpufix: the game's hardware check (systemdetection.dll) sees at most 16 CPUs (%s%s)", a ? "affinity" : "affinity NOT patched",
        b ? ", count" : ", count NOT patched");
}

// ntdll's DLL load notification (Vista+), so the patch is in place before the game calls into the dll.
struct UStr { USHORT len, max; PWSTR buf; };
struct LoadedData { ULONG flags; const UStr *fullName, *baseName; PVOID base; ULONG size; };
typedef VOID(CALLBACK *NotifyFn)(ULONG reason, const LoadedData *data, PVOID ctx);
typedef LONG(NTAPI *RegisterFn)(ULONG, NotifyFn, PVOID, PVOID *);
typedef LONG(NTAPI *UnregisterFn)(PVOID);
PVOID g_cookie;

VOID CALLBACK OnLoad(ULONG reason, const LoadedData *d, PVOID) {
    if (reason != 1 || !d || !d->baseName || !d->baseName->buf) return;  // 1 = loaded
    const UStr *n = d->baseName;
    if (n->len == 19 * 2 && !_wcsnicmp(n->buf, L"systemdetection.dll", 19)) Patch((HMODULE)d->base);
}
}  // namespace

void CpuFixStart() {
    wchar_t ini[MAX_PATH];
    IniPath(ini);
    if (!GetPrivateProfileIntW(L"bd", L"CpuCountFix", 1, ini)) return;
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    if (!(sys & 0x80000000u)) return;  // fewer than 32 CPUs: the game's check is fine as it is
    Log("cpufix: %d or more logical processors (mask %08lx): working round the game's start-up crash", 32, (unsigned long)sys);
    if (HMODULE sd = GetModuleHandleW(L"systemdetection.dll")) {
        Patch(sd);
        return;
    }
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RegisterFn reg = nt ? (RegisterFn)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
    if (!reg || reg(0, OnLoad, nullptr, &g_cookie) != 0) Log("cpufix: can't watch for systemdetection.dll; the game may crash at start-up");
}
