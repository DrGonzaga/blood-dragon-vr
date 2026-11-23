// Render resolution: in windowed mode the game sizes everything from its
// window's client area, and Windows keeps the window within the screen, so the
// headset got a monitor-sized, monitor-shaped image. Forcing the swapchain size
// alone doesn't work: the game checks it every frame against the window and
// rebuilds it (res4 froze that way). So we tell the game its window's client
// area is the size in fc3vr.ini (GetClientRect, through FC3_d3d11.dll's import
// table); DXGI scales the result into the real window on the monitor.
// GamerProfile.xml must be windowed (Borderless="0", Fullscreen="0").
//
// fc3vr.ini (next to version.dll):
//   [render]
//   Width=2880
//   Height=2808      (0 for either leaves the size to the game)
//   No Width/Height at all (autores1, bd135): the headset's recommended size x AutoScale (default 1.0),
//   asked from the OpenXR runtime at start-up; 2880x2808 if the headset can't be reached.
//   AutoScale=1.0
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

#include "fc3vr.h"

static UINT g_width = 2880, g_height = 2808;
static volatile bool g_off;  // gave up: the game kept rebuilding its swapchain
static bool g_auto;            // autores1: size to come from the headset
static HANDLE g_autoReady;     // autores1: set when the headset answered (or gave up)

typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                      IDXGISwapChain **);
static CreateSwapChainFn g_origCreate;

void IniPath(wchar_t *path) {
    // review1: worked out once (this is called for every ini read and write)
    static wchar_t s_path[MAX_PATH];
    static volatile LONG s_state;  // 0 not yet, 1 being written, 2 ready
    if (s_state == 2) {
        wcscpy(path, s_path);
        return;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&IniPath, &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    wcscpy(slash ? slash + 1 : path, L"fc3vr.ini");
    if (InterlockedCompareExchange(&s_state, 1, 0) == 0) wcscpy(s_path, path), InterlockedExchange(&s_state, 2);
}

static void ReadIni() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    int w = (int)GetPrivateProfileIntW(L"render", L"Width", -1, path);
    int h = (int)GetPrivateProfileIntW(L"render", L"Height", -1, path);
    if (w < 0 || h < 0) {
        g_auto = true;  // autores1: no saved size, ask the headset
        return;
    }
    g_width = (UINT)w, g_height = (UINT)h;
}

// autores1: the headset's recommended size, before the game asks its window's size.
static DWORD WINAPI AutoSize(LPVOID) {
    wchar_t path[MAX_PATH], buf[32];
    IniPath(path);
    GetPrivateProfileStringW(L"render", L"AutoScale", L"1.0", buf, 32, path);
    float scale = (float)_wtof(buf);
    if (scale < 0.5f || scale > 2.0f) scale = 1.0f;
    UINT rw = 0, rh = 0;
    DWORD t0 = GetTickCount();
    if (XrEarlyRecommendedSize(&rw, &rh)) {
        g_width = (UINT)(rw * scale + 8) / 16 * 16, g_height = (UINT)(rh * scale + 8) / 16 * 16;
        Log("res: render size %ux%u from the headset (recommended %ux%u x %.2f, %lu ms)", g_width, g_height, rw, rh,
            scale, GetTickCount() - t0);
    } else
        Log("res: headset's recommended size not available at start; using %ux%u", g_width, g_height);
    XrEarlyEnd();
    SetEvent(g_autoReady);
    return 0;
}

// Only the game's own window (class "Nomad"), never a probe device's.
static bool IsGameWindow(HWND w) {
    wchar_t cls[32];
    return w && GetClassNameW(w, cls, 32) && !wcscmp(cls, L"Nomad");
}

// Only logs now: what size the game asked for.
bool ResForce(HWND w, UINT *width, UINT *height) {
    if (!IsGameWindow(w)) return false;
    static int logged;
    if (++logged <= 20) Log("res: game swapchain size %ux%u", *width, *height);
    // Safety: six swapchains within 3 s means the game is fighting the size; stop.
    static DWORD times[6];
    static int n;
    DWORD now = GetTickCount();
    times[n++ % 6] = now;
    if (n >= 6 && now - times[n % 6] < 3000 && !g_off) {
        g_off = true;
        Log("res: the game keeps rebuilding its swapchain; leaving the size to the game");
    }
    return false;
}

typedef BOOL(WINAPI *GetClientRectFn)(HWND, LPRECT);
static GetClientRectFn g_origGetClientRect;

static BOOL WINAPI HookGetClientRect(HWND w, LPRECT rc) {
    BOOL ok = g_origGetClientRect(w, rc);
    if (ok && rc && !g_off && IsGameWindow(w) && !IsIconic(w)) {
        if (g_autoReady) WaitForSingleObject(g_autoReady, 10000);  // autores1: once, then it stays set
        rc->left = rc->top = 0;
        rc->right = g_width;
        rc->bottom = g_height;
    }
    return ok;
}

// Point one import of a module at our function.
bool PatchImport(HMODULE mod, const char *dll, const char *name, void *hook, void **orig) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char *)(base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *funcs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)n->Name, name)) continue;
            *orig = (void *)funcs->u1.Function;
            return PatchBytes(&funcs->u1.Function, &hook, 4);
        }
    }
    return false;
}

static DWORD WINAPI WaitForGame(LPVOID) {
    HMODULE fc3 = nullptr;
    for (int i = 0; i < 180000 && !(fc3 = GetModuleHandleW(L"FC3_d3d11.dll")); ++i) Sleep(1);
    if (!fc3) return 0;
    bool ok = PatchImport(fc3, "USER32.dll", "GetClientRect", (void *)HookGetClientRect, (void **)&g_origGetClientRect);
    Log("res: game window size reported as %ux%u: %s", g_width, g_height, ok ? "hooked" : "FAILED (left to the game)");
    return 0;
}

#define HOOKABLE __attribute__((ms_hook_prologue, noinline))

static HOOKABLE HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory *f, IUnknown *dev,
                                                             DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    DXGI_SWAP_CHAIN_DESC d;
    if (desc) {
        d = *desc;
        if (d.Windowed) ResForce(d.OutputWindow, &d.BufferDesc.Width, &d.BufferDesc.Height);
        if (IsGameWindow(d.OutputWindow)) d.BufferUsage |= DXGI_USAGE_SHADER_INPUT;  // smaa1: SMAA reads it directly
        desc = &d;
    }
    return g_origCreate(f, dev, desc, out);
}

static DWORD WINAPI WaitForDxgi(LPVOID) {
    // Hook before the game makes its swapchain: as soon as dxgi.dll is in.
    HMODULE dxgi = nullptr;
    for (int i = 0; i < 180000 && !(dxgi = GetModuleHandleW(L"dxgi.dll")); ++i) Sleep(1);
    if (!dxgi) return 0;
    typedef HRESULT(WINAPI * CreateFactoryFn)(REFIID, void **);
    auto create = (CreateFactoryFn)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory *f = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void **)&f))) {
        Log("res: couldn't make a DXGI factory; render size left to the game");
        return 0;
    }
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"dxgi.dll", &pinned);
    HookSlot(*(void ***)f, 10, (void *)HookCreateSwapChain, (void **)&g_origCreate, "CreateSwapChain");
    f->Release();
    return 0;
}

void ResStart() {
    ReadIni();
    if (g_auto) {
        g_autoReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        XrEarlyBegin();
        CloseHandle(CreateThread(nullptr, 0, AutoSize, nullptr, 0, nullptr));
        Log("res: no render size in fc3vr.ini; asking the headset");
    } else if (!g_width || !g_height) {
        Log("res: render size left to the game (fc3vr.ini)");
        return;
    }
    if (!g_auto) Log("res: render size %ux%u (fc3vr.ini)", g_width, g_height);
    CloseHandle(CreateThread(nullptr, 0, WaitForGame, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, WaitForDxgi, nullptr, 0, nullptr));
}

// Menu (Graphics page): the render size. Saved for the next launch; "now"
// also changes what the game is told, and it rebuilds its swapchain (and we
// the headset session) on its next frame.
void ResGetSize(UINT *w, UINT *h) { *w = g_width, *h = g_height; }
bool ResAutoSize() { return g_auto; }
void ResUseAutoSize() {
    wchar_t path[MAX_PATH];
    IniPath(path);
    WritePrivateProfileStringW(L"render", L"Width", nullptr, path);
    WritePrivateProfileStringW(L"render", L"Height", nullptr, path);
    Log("res: saved size removed; the next start uses the headset's recommended size");
}
void ResSetSize(UINT w, UINT h, bool now) {
    wchar_t path[MAX_PATH], buf[16];
    IniPath(path);
    swprintf(buf, 16, L"%u", w);
    WritePrivateProfileStringW(L"render", L"Width", buf, path);
    swprintf(buf, 16, L"%u", h);
    WritePrivateProfileStringW(L"render", L"Height", buf, path);
    Log("res: render size %ux%u saved%s", w, h, now ? ", applying now" : " for the next launch");
    if (now && g_width && g_height) g_width = w, g_height = h;
}
