// online1 (DrGonzaga 12 Nov, Blood Dragon): every pause waited a long time on Ubisoft's online
// service, which no longer answers (the game asks "onlineconfigservice.ubi.com" for its online
// settings and then tries to log in). A patched-DLL mod exists for it, but for another build of
// the game (2 May 2018; DrGonzaga's is 17 Oct 2019), so it can't be used with our hooks. Instead
// the game DLL's own network calls are answered at once, as if the PC had no internet:
// name lookups fail and connections to anything but this PC are refused. Only FC3_d3d11.dll's
// imports are changed (Steam and Ubisoft Connect use their own DLLs and are not touched).
// fc3vr.ini [online] Block=0 turns it off. The first few names / addresses go into the log.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

namespace {
typedef int(WSAAPI *GaiFn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef struct hostent *(WSAAPI *GhbnFn)(const char *);
typedef int(WSAAPI *ConnFn)(SOCKET, const struct sockaddr *, int);
typedef void(WSAAPI *SetErrFn)(int);
GaiFn g_gai;
GhbnFn g_ghbn;
ConnFn g_conn;
SetErrFn g_setErr;
volatile LONG g_lines, g_blocked;

bool Local(const char *n) {
    if (!n || !*n) return true;  // own address / wildcard
    if (!_stricmp(n, "localhost")) return true;
    bool numeric = true;  // an address, not a name: no lookup happens, connect decides
    for (const char *p = n; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ':' || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == '%'))
            numeric = false;
    if (numeric) return true;
    char me[256];
    DWORD cap = sizeof me;
    if (GetComputerNameA(me, &cap) && !_stricmp(n, me)) return true;
    return false;
}
void Note(const char *what, const char *who) {
    InterlockedIncrement(&g_blocked);
    if (InterlockedIncrement(&g_lines) <= 24) Log("online: %s %s: answered \"no internet\" at once", what, who);
}
int WSAAPI HookGai(const char *node, const char *svc, const struct addrinfo *hints, struct addrinfo **res) {
    if (Local(node)) return g_gai(node, svc, hints, res);
    Note("name lookup", node);
    if (res) *res = nullptr;
    if (g_setErr) g_setErr(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
}
struct hostent *WSAAPI HookGhbn(const char *name) {
    if (Local(name)) return g_ghbn(name);
    Note("name lookup", name);
    if (g_setErr) g_setErr(WSAHOST_NOT_FOUND);
    return nullptr;
}
int WSAAPI HookConn(SOCKET s, const struct sockaddr *a, int len) {
    bool local = true;
    char who[64] = "?";
    if (a && a->sa_family == AF_INET && len >= (int)sizeof(sockaddr_in)) {
        const BYTE *ip = (const BYTE *)&((const sockaddr_in *)a)->sin_addr;
        local = ip[0] == 127 || (ip[0] | ip[1] | ip[2] | ip[3]) == 0;
        wsprintfA(who, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], (unsigned)(((const BYTE *)&((const sockaddr_in *)a)->sin_port)[0] << 8 | ((const BYTE *)&((const sockaddr_in *)a)->sin_port)[1]));
    } else if (a && a->sa_family == AF_INET6 && len >= (int)sizeof(sockaddr_in6)) {
        const BYTE *ip = (const BYTE *)&((const sockaddr_in6 *)a)->sin6_addr;
        bool zero = true;
        for (int i = 0; i < 15; ++i) zero = zero && ip[i] == 0;
        local = zero && ip[15] <= 1;
        lstrcpyA(who, "an IPv6 address");
    }
    if (local) return g_conn(s, a, len);
    Note("connection to", who);
    if (g_setErr) g_setErr(WSAENETUNREACH);
    return SOCKET_ERROR;
}

// Point every import slot of `mod` that holds `target` at `hook`.
int PatchImport(HMODULE mod, void *target, void *hook) {
    if (!target) return 0;
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
        const char *dll = (const char *)(base + imp->Name);
        if (_stricmp(dll, "WS2_32.dll") && _stricmp(dll, "WSOCK32.dll")) continue;
        for (IMAGE_THUNK_DATA *f = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk); f->u1.Function; ++f)
            if ((void *)f->u1.Function == target && PatchBytes(&f->u1.Function, &hook, 4)) ++n;
    }
    return n;
}
}  // namespace

void OnlineInstall(HMODULE game) {
    if (!g_bloodDragon) return;
    wchar_t path[MAX_PATH];
    IniPath(path);
    if (!IniGetInt(L"online", L"Block", 1, path)) {
        Log("online: the game's online calls are left alone ([online] Block=0)");
        return;
    }
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) ws = LoadLibraryW(L"ws2_32.dll");
    if (!ws) return;
    g_gai = (GaiFn)(void *)GetProcAddress(ws, "getaddrinfo");
    g_ghbn = (GhbnFn)(void *)GetProcAddress(ws, "gethostbyname");
    g_conn = (ConnFn)(void *)GetProcAddress(ws, "connect");
    g_setErr = (SetErrFn)(void *)GetProcAddress(ws, "WSASetLastError");
    int a = PatchImport(game, (void *)g_gai, (void *)HookGai), b = PatchImport(game, (void *)g_ghbn, (void *)HookGhbn),
        c = PatchImport(game, (void *)g_conn, (void *)HookConn);
    Log("online: the game's own internet calls answered at once as \"no internet\" (name lookups %d+%d, connections %d hooked; "
        "[online] Block=0 turns this off)", a, b, c);
}
long OnlineBlocked() { return g_blocked; }

// scan1 (DrGonzaga 12 Nov, Blood Dragon): the game's deliberate "old TV" scanlines and screen
// overlays are hard on the eyes in VR. The known fix is to blank the class name "COverlayTrack"
// in the game DLL with a hex editor, so the cinematic overlay tracks are never made. Done here in
// memory instead (the file on disk stays as Steam shipped it), as soon as the DLL is mapped and
// before the game first registers that class (0x111ffd30 does it lazily; its done-flag is bit 1
// of 0x11f02da8). fc3vr.ini [graphics] NoScanlines=0 keeps the scanlines.
void ScanlinesInstall(HMODULE game) {
    if (!g_bloodDragon) return;
    wchar_t path[MAX_PATH];
    IniPath(path);
    if (!IniGetInt(L"graphics", L"NoScanlines", 1, path)) {
        Log("scanlines: left as the game has them ([graphics] NoScanlines=0)");
        return;
    }
    BYTE *bd = (BYTE *)game;
    char *name = (char *)(bd + 0x16ec8d8);
    DWORD *flag = (DWORD *)(bd + 0x1f02da8);
    if (IsBadReadPtr(name, 16) || memcmp(name, "COverlayTrack", 14)) {
        Log("scanlines: the overlay class name isn't where expected; left alone");
        return;
    }
    bool late = !IsBadReadPtr(flag, 4) && (*flag & 1);
    static const BYTE zero[16] = {0};
    bool ok = PatchBytes(name, zero, 8) && PatchBytes(name + 8, zero, 5);
    Log("scanlines: overlay tracks %s%s", ok ? "switched off (class name blanked in memory)" : "could not be switched off",
        late ? "; NOTE the game had already registered the class, so this may not take effect" : "");
}
