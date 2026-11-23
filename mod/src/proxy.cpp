// Proxy layer: this DLL can be dropped into Far Cry 3's bin folder as either
// dinput8.dll or version.dll. It loads the real system DLL of the same name and
// forwards every export to it through jump stubs, so the game behaves as normal.
#include <windows.h>
#include <wchar.h>

#include "fc3vr.h"

#define PROXY_EXPORTS(X)            \
    X(0, DirectInput8Create)        \
    X(1, DllCanUnloadNow)           \
    X(2, DllGetClassObject)         \
    X(3, DllRegisterServer)         \
    X(4, DllUnregisterServer)       \
    X(5, GetdfDIJoystick)           \
    X(6, GetFileVersionInfoA)       \
    X(7, GetFileVersionInfoByHandle) \
    X(8, GetFileVersionInfoExA)     \
    X(9, GetFileVersionInfoExW)     \
    X(10, GetFileVersionInfoSizeA)  \
    X(11, GetFileVersionInfoSizeExA) \
    X(12, GetFileVersionInfoSizeExW) \
    X(13, GetFileVersionInfoSizeW)  \
    X(14, GetFileVersionInfoW)      \
    X(15, VerFindFileA)             \
    X(16, VerFindFileW)             \
    X(17, VerInstallFileA)          \
    X(18, VerInstallFileW)          \
    X(19, VerLanguageNameA)         \
    X(20, VerLanguageNameW)         \
    X(21, VerQueryValueA)           \
    X(22, VerQueryValueW)

#define COUNT_ONE(i, name) +1
enum { kProxyCount = 0 PROXY_EXPORTS(COUNT_ONE) };

extern "C" {
FARPROC g_procs[kProxyCount];
}

#define NAME_ONE(i, name) #name,
static const char *const kProxyNames[kProxyCount] = {PROXY_EXPORTS(NAME_ONE)};

// Each stub is a bare jmp through g_procs, so it works for any calling
// convention and argument count without us declaring the real signatures.
#define STUB_ONE(i, name)                       \
    asm(".text\n"                               \
        ".globl _p_" #name "\n"                 \
        "_p_" #name ":\n"                       \
        "    jmp *(_g_procs + " #i " * 4)\n");
PROXY_EXPORTS(STUB_ONE)

static HMODULE g_real;
static bool g_second;  // onestart1: another copy of the mod already started in this process

static bool LoadRealDll(HMODULE self) {
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(self, selfPath, MAX_PATH);
    const wchar_t *name = wcsrchr(selfPath, L'\\');
    name = name ? name + 1 : selfPath;

    wchar_t sysPath[MAX_PATH];
    GetSystemDirectoryW(sysPath, MAX_PATH);  // SysWOW64 for a 32-bit process
    wcscat(sysPath, L"\\");
    wcscat(sysPath, name);

    g_real = LoadLibraryW(sysPath);
    if (!g_real) return false;
    for (int i = 0; i < kProxyCount; ++i) g_procs[i] = GetProcAddress(g_real, kProxyNames[i]);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (!LoadRealDll(inst)) return FALSE;
        // onestart1 (GitHub #1, 23 Nov): the mod can be in bin as both version.dll and dinput8.dll (players
        // with Windows' CPU-count fix need dinput8.dll). Only the first copy to load starts the mod; a
        // second copy just forwards its DLL's calls, so nothing is hooked twice.
        wchar_t mx[64];
        swprintf(mx, 64, L"Local\\bdvr-start-%lu", GetCurrentProcessId());
        HANDLE m = CreateMutexW(nullptr, FALSE, mx);  // kept open for the life of the process
        if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(m);
            g_second = true;
            return TRUE;
        }
        Fc3vrStart(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!g_second) LogFlush();  // quiet1: last buffered lines on a normal exit
    }
    return TRUE;
}
