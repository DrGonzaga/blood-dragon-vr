// prewarm1: read the game's single-player data files once at start, on a
// low-priority thread, so Windows keeps them in its file cache (DrGonzaga has
// 64 GB). The game is 32-bit and can't hold them itself, but its streaming
// reads then come from memory instead of the SSD. [perf] Prewarm=0 turns it off.
#include <windows.h>
#include "fc3vr.h"

static const wchar_t *const kFiles[] = {
    L"worlds\\fc3_main\\fc3_main.dat",         L"worlds\\fc3_main\\fc3_main.fat",
    L"worlds\\fc3_main\\fc3_main_english.dat", L"worlds\\fc3_main\\fc3_main_english.fat",
    L"worlds\\fc3_main\\fc3_main_vistas.dat",  L"worlds\\fc3_main\\fc3_main_vistas.fat",
    // bd4: Blood Dragon's world (files that don't exist in this game are skipped)
    L"worlds\\blood_main\\blood_main.dat",         L"worlds\\blood_main\\blood_main.fat",
    L"worlds\\blood_main\\blood_main_english.dat", L"worlds\\blood_main\\blood_main_english.fat",
    L"worlds\\blood_main\\blood_main_vistas.dat",  L"worlds\\blood_main\\blood_main_vistas.fat",
    L"common.dat",                             L"common.fat",
    L"patch.dat",                              L"patch.fat",
    L"shadersobj.dat",                         L"shadersobj.fat",
    L"sound.dat",                              L"sound.fat",
    L"ige.dat",                                L"ige.fat",
};

static DWORD WINAPI PrewarmThread(void *) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Sleep(1000);
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    *slash = 0;  // ...\bin
    const DWORD kChunk = 8 << 20;
    BYTE *buf = (BYTE *)VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    unsigned long long total = 0;
    int files = 0;
    for (const wchar_t *name : kFiles) {
        wchar_t path[MAX_PATH * 2];
        wsprintfW(path, L"%s\\..\\data_win32\\%s", dir, name);
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD got;
        while (ReadFile(h, buf, kChunk, &got, nullptr) && got) total += got;
        CloseHandle(h);
        ++files;
    }
    QueryPerformanceCounter(&b);
    VirtualFree(buf, 0, MEM_RELEASE);
    double s = double(b.QuadPart - a.QuadPart) / f.QuadPart;
    Log("prewarm: read %d files, %llu MB in %.1f s (%.0f MB/s) into the Windows file cache", files, total >> 20, s,
        s > 0 ? (total >> 20) / s : 0.0);
    return 0;
}

void PrewarmStart() {
    wchar_t ini[MAX_PATH];
    GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t *slash = wcsrchr(ini, L'\\');
    if (slash) lstrcpyW(slash + 1, L"fc3vr.ini");
    if (!GetPrivateProfileIntW(L"perf", L"Prewarm", 1, ini)) {
        Log("prewarm: off ([perf] Prewarm=0)");
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, PrewarmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
