// Graphics settings for the menu (menu.cpp, Graphics page).
//
// Live: the engine's own debug variables ("gfx_disablePost" and so on). Each
// is a static object registered at start-up in a list (head 0x12829bac):
// +0 vtable (0x11f3d9e0 = on/off, 0x11f3d9e8 = number), +4 name, +0xc / +0x10
// links, +0x14 value. The renderer reads the value every frame
// (gfx_disablePost at 0x118a6495, gfx_disableFog at 0x119c00f2), so changing it
// takes effect at once. Our changes are kept in fc3vr.ini [gfxlive] and put
// back on the next run.
//
// Next launch: settings the game only reads at start (MSAA, ambient
// occlusion, ...) live in Documents\My Games\Far Cry 3\GamerProfile.xml. The
// menu keeps them in fc3vr.ini [graphics] and we write them into the profile
// when the game starts, before it reads it, touching nothing else (the game's
// own menu would also reset the render size).
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "fc3vr.h"

namespace {

const DWORD kImageBase = 0x10000000, kVarHead = 0x12829bac, kBoolVt = 0x11f3d9e0, kFloatVt = 0x11f3d9e8;

struct Var {
    const char *name;
    BYTE *value;
    int type;  // 1 on/off, 2 number
};
Var g_vars[1024];
// Blood Dragon (gfxbd1): same list and layout, other addresses. Head 0x11846644 (set by the
// base constructor 0x1005507f), on/off vtable 0x114fc680, number vtable 0x114fc688.
const DWORD kBdVarHead = 0x11846644, kBdBoolVt = 0x114fc680, kBdFloatVt = 0x114fc688;
BYTE *Game() { return (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll"); }
int g_nvars;
bool g_scanned;

bool Readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return (BYTE *)p + n <= (BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

void Scan() {
    if (g_scanned) return;
    BYTE *fc3 = Game();
    if (!fc3) return;
    g_scanned = true;
    BYTE *node = *(BYTE **)(BdAt(fc3, (g_bloodDragon ? kBdVarHead : kVarHead)));
    void *bvt = BdAt(fc3, (g_bloodDragon ? kBdBoolVt : kBoolVt)), *fvt = BdAt(fc3, (g_bloodDragon ? kBdFloatVt : kFloatVt));
    int other = 0;
    for (int guard = 0; node && guard < 4000 && g_nvars < 1024; ++guard) {
        if (!Readable(node, 0x18)) break;
        void *vt = *(void **)node;
        const char *name = *(const char **)(node + 4);
        if (name && Readable(name, 4)) {
            int type = vt == bvt ? 1 : vt == fvt ? 2 : 0;
            if (type) g_vars[g_nvars++] = {name, node + 0x14, type};
            else ++other;
        }
        node = *(BYTE **)(node + 0x10);
    }
    Log("gfx: %d engine variables (%d on/off or number, %d other kinds)", g_nvars + other, g_nvars, other);
}

void IniFile(wchar_t *p) { IniPath(p); }

// ---- GamerProfile.xml ----

// prof2 (2 Nov): DrGonzaga's game keeps its profile in OneDrive\Documents, which the
// Documents known folder doesn't point at. Try it, then the OneDrive folders.
bool ProfilePath(wchar_t *out) {
    // prof3: plain string joins (prof2's swprintf %s didn't take wide strings in this build).
    wchar_t base[3][MAX_PATH] = {};
    wchar_t *docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        wcsncpy(base[0], docs, MAX_PATH - 1);
        CoTaskMemFree(docs);
    }
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"OneDrive", env, MAX_PATH)) wcscpy(base[1], env), wcscat(base[1], L"\\Documents");
    if (GetEnvironmentVariableW(L"USERPROFILE", env, MAX_PATH)) wcscpy(base[2], env), wcscat(base[2], L"\\OneDrive\\Documents");
    for (auto &b : base) {
        if (!b[0] || wcslen(b) > MAX_PATH - 48) continue;
        wcscpy(out, b), wcscat(out, g_bloodDragon ? L"\\My Games\\Far Cry 3 Blood Dragon\\GamerProfile.xml" : L"\\My Games\\Far Cry 3\\GamerProfile.xml");
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

char *ReadAll(const wchar_t *path, DWORD *size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    DWORD n = GetFileSize(f, nullptr), got = 0;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (buf && (!ReadFile(f, buf, n, &got, nullptr) || got != n)) HeapFree(GetProcessHeap(), 0, buf), buf = nullptr;
    CloseHandle(f);
    if (buf) buf[n] = 0, *size = n;
    return buf;
}

// The value of ` key="..."` (first one), or false.
bool GetAttr(const char *xml, const char *key, char *out, int len) {
    char pat[64];
    snprintf(pat, sizeof pat, " %s=\"", key);
    const char *p = strstr(xml, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e || e - p >= len) return false;
    memcpy(out, p, e - p), out[e - p] = 0;
    return true;
}

char g_profile[16][16];  // the profile's values when the game started, per kProfile entry
bool g_haveProfile;

}  // namespace
bool GfxProfilePath(wchar_t *out) { return ProfilePath(out); }  // lang1: prompts.cpp reads the game's language

// Settings the game reads at start, from GamerProfile.xml.
const GfxProfileSetting kProfile[] = {
    {"MSAALevel", "Anti-aliasing (MSAA)", {"0", "2", "4", "8"}, {"Off", "2x", "4x", "8x"}, 4},
    {"AlphaToCoverage", "Smooth foliage edges (alpha to coverage)", {"0", "1", "2"}, {"Off", "On", "High"}, 3},
    {"SSAOLevel", "Ambient occlusion", {"0", "1", "2", "3", "4", "5", "6"},
     {"Off", "SSAO", "HBAO", "HBAO (compute)", "HDAO", "HDAO (compute)", "HDAO (compute, blur)"}, 7},
    {"PostFxQuality", "Post effects quality", {"low", "medium", "high", "veryhigh"}, {"Low", "Medium", "High", "Very high"}, 4},
    {"ShadowQuality", "Shadows", {"low", "medium", "high", "veryhigh", "ultrahigh"}, {"Low", "Medium", "High", "Very high", "Ultra"}, 5},
    {"WaterQuality", "Water", {"low", "medium", "high", "veryhigh"}, {"Low", "Medium", "High", "Very high"}, 4},
    {"VegetationQuality", "Vegetation", {"low", "medium", "high", "veryhigh"}, {"Low", "Medium", "High", "Very high"}, 4},
    {"GeometryQuality", "Geometry", {"low", "medium", "high", "veryhigh", "ultrahigh"}, {"Low", "Medium", "High", "Very high", "Ultra"}, 5},
};
const int kProfileCount = sizeof kProfile / sizeof kProfile[0];

// At start-up, before the game reads its profile: write the menu's choices in.
void GfxStartup() {
    wchar_t path[MAX_PATH], ini[MAX_PATH];
    if (!ProfilePath(path)) {
        Log("gfx: GamerProfile.xml not found");
        return;
    }
    Log("gfx: GamerProfile.xml at %ls", path);
    IniFile(ini);
    DWORD size;
    char *xml = ReadAll(path, &size);
    if (!xml) return;
    if ((BYTE)xml[0] == 0xff || (BYTE)xml[0] == 0xfe) {
        Log("gfx: GamerProfile.xml isn't plain text; next-launch settings off");
        HeapFree(GetProcessHeap(), 0, xml);
        return;
    }
    // Build the new text with each chosen value swapped in.
    size_t cap = size + 1024;
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
    memcpy(out, xml, size + 1);
    int changed = 0;
    for (int i = 0; i < kProfileCount; ++i) {
        const auto &s = kProfile[i];
        GetAttr(out, s.key, g_profile[i], 16);
        wchar_t wkey[64], wval[16];
        MultiByteToWideChar(CP_ACP, 0, s.key, -1, wkey, 64);
        GetPrivateProfileStringW(L"graphics", wkey, L"", wval, 16, ini);
        if (!wval[0]) continue;
        char val[16];
        WideCharToMultiByte(CP_ACP, 0, wval, -1, val, 16, nullptr, nullptr);
        bool known = false;
        for (int k = 0; k < s.count; ++k) known |= !strcmp(s.values[k], val);
        if (!known || !strcmp(val, g_profile[i])) continue;
        char pat[64];
        snprintf(pat, sizeof pat, " %s=\"", s.key);
        char *p = strstr(out, pat);
        if (!p) continue;
        p += strlen(pat);
        char *e = strchr(p, '"');
        size_t oldLen = e - p, newLen = strlen(val), tail = strlen(e) + 1;
        memmove(p + newLen, e, tail);
        memcpy(p, val, newLen);
        Log("gfx: %s %s -> %s (from the menu)", s.key, g_profile[i], val);
        strcpy(g_profile[i], val);
        ++changed;
        (void)oldLen;
    }
    // bd10 (DrGonzaga 12 Nov): Blood Dragon's profile is kept right for VR by the mod itself, so a
    // reset profile can't undo it: windowed (the headset-size picture needs it, see res.cpp) and
    // multithreaded rendering off (the FC3 hard rule). fc3vr.ini [render] KeepProfile=0 stops this.
    if (g_bloodDragon && GetPrivateProfileIntW(L"render", L"KeepProfile", 1, ini)) {
        static const char *const kKeep[][2] = {{"Fullscreen", "0"}, {"Borderless", "0"}, {"D3D11MultithreadedRendering", "0"}};
        for (auto &k : kKeep) {
            char pat[64], was[16] = "";
            snprintf(pat, sizeof pat, " %s=\"", k[0]);
            char *p = strstr(out, pat);
            if (!p || !GetAttr(out, k[0], was, 16) || !strcmp(was, k[1])) continue;
            p += strlen(pat);
            char *e = strchr(p, '"');
            size_t newLen = strlen(k[1]);
            memmove(p + newLen, e, strlen(e) + 1);
            memcpy(p, k[1], newLen);
            Log("gfx: %s %s -> %s (kept right for VR)", k[0], was, k[1]);
            ++changed;
        }
    }
    g_haveProfile = true;
    if (changed) {
        wchar_t bak[MAX_PATH];
        wcscpy(bak, path), wcscat(bak, L".fc3vr-backup");
        CopyFileW(path, bak, TRUE);  // the first one only
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD wrote = 0;
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, out, (DWORD)strlen(out), &wrote, nullptr);
            CloseHandle(f);
        }
        Log("gfx: GamerProfile.xml updated (%d settings, %lu bytes)", changed, wrote);
    }
    HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, xml);
}

// The value the game started with, and the one chosen for next time.
const char *GfxProfileNow(int i) { return g_haveProfile && i >= 0 && i < kProfileCount ? g_profile[i] : ""; }
void GfxProfileChosen(int i, char *out, int len) {
    wchar_t ini[MAX_PATH], wkey[64], wval[16];
    IniFile(ini);
    MultiByteToWideChar(CP_ACP, 0, kProfile[i].key, -1, wkey, 64);
    GetPrivateProfileStringW(L"graphics", wkey, L"", wval, 16, ini);
    WideCharToMultiByte(CP_ACP, 0, wval, -1, out, len, nullptr, nullptr);
    if (!out[0]) snprintf(out, len, "%s", GfxProfileNow(i));
}
void GfxProfileChoose(int i, const char *val) {
    wchar_t ini[MAX_PATH], wkey[64], wval[16];
    IniFile(ini);
    MultiByteToWideChar(CP_ACP, 0, kProfile[i].key, -1, wkey, 64);
    MultiByteToWideChar(CP_ACP, 0, val, -1, wval, 16);
    WritePrivateProfileStringW(L"graphics", wkey, wval, ini);
    Log("gfx: %s = %s chosen for the next launch", kProfile[i].key, val);
}

// ---- Live engine variables ----

int GfxVarCount() {
    Scan();
    return g_nvars;
}
const char *GfxVarName(int i) { return g_vars[i].name; }
int GfxVarType(int i) { return g_vars[i].type; }
int GfxVarFind(const char *name) {
    Scan();
    for (int i = 0; i < g_nvars; ++i)
        if (!strcmp(g_vars[i].name, name)) return i;
    return -1;
}
bool GfxVarBool(int i) { return *(BYTE *)g_vars[i].value != 0; }
float GfxVarFloat(int i) { return *(float *)g_vars[i].value; }

static void SaveLive(int i) {
    wchar_t ini[MAX_PATH], wkey[96], wval[32];
    IniFile(ini);
    MultiByteToWideChar(CP_ACP, 0, g_vars[i].name, -1, wkey, 96);
    if (g_vars[i].type == 1) swprintf(wval, 32, L"%d", GfxVarBool(i) ? 1 : 0);
    else swprintf(wval, 32, L"%.4f", GfxVarFloat(i));
    WritePrivateProfileStringW(L"gfxlive", wkey, wval, ini);
}
void GfxVarSetBool(int i, bool on) {
    *(BYTE *)g_vars[i].value = on ? 1 : 0;
    Log("gfx: %s = %d", g_vars[i].name, on);
    SaveLive(i);
}
void GfxVarSetFloat(int i, float v) {
    *(float *)g_vars[i].value = v;
    SaveLive(i);
}

// Put back what the menu changed last time (once the game is running).
void GfxApplyLive() {
    Scan();
    wchar_t ini[MAX_PATH];
    IniFile(ini);
    static wchar_t keys[8192];
    DWORD n = GetPrivateProfileStringW(L"gfxlive", nullptr, L"", keys, 8192, ini);
    int applied = 0;
    for (wchar_t *k = keys; k < keys + n && *k; k += wcslen(k) + 1) {
        char name[96];
        WideCharToMultiByte(CP_ACP, 0, k, -1, name, 96, nullptr, nullptr);
        int i = GfxVarFind(name);
        if (i < 0) continue;
        wchar_t v[32];
        GetPrivateProfileStringW(L"gfxlive", k, L"", v, 32, ini);
        if (g_vars[i].type == 1) *(BYTE *)g_vars[i].value = _wtoi(v) ? 1 : 0;
        else *(float *)g_vars[i].value = (float)_wtof(v);
        ++applied;
    }
    if (applied) Log("gfx: %d engine variables set from fc3vr.ini [gfxlive]", applied);
    if (!GetPrivateProfileIntW(L"gfx", L"Bloom", 1, ini)) GfxSetBloom(false);
}
bool GfxBloomOn();
void GfxSetBloom(bool on);
void GfxResetLive() {
    wchar_t ini[MAX_PATH];
    IniFile(ini);
    WritePrivateProfileStringW(L"gfxlive", nullptr, nullptr, ini);
    if (!GfxBloomOn()) GfxSetBloom(true);
    Log("gfx: [gfxlive] cleared; the game's defaults return on the next launch");
}

// ---- Bloom (bloom1) ----
// The post-process shader gets its BLOOM_ENABLED permutation bit only when the post settings'
// byte +0x56 is set: a "cmp [..+0x56] / je" pair. Turning that je into a jmp leaves bloom out of
// the final image; putting the je back returns it. Takes effect on the next frame.
// BD: 0x10125ca0 cmp [edx+0x56],cl / 0x10125ca3 je 0x10125cca (in 0x10124d91, FC3's 0x11569490).
static BYTE *BloomJump() {
    BYTE *bd = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
    if (!bd) return nullptr;
    BYTE *q = BdAt(bd, 0x10125ca3);
    if (IsBadReadPtr(q - 3, 5) || (q[0] != 0x74 && q[0] != 0xeb) || q[1] != 0x25 || q[-3] != 0x38 || q[-2] != 0x4a || q[-1] != 0x56) return nullptr;
    return q;
}
bool GfxBloomAvailable() { return BloomJump() != nullptr; }

bool GfxBloomOn() {
    BYTE *p = BloomJump();
    return !p || p[0] == 0x74;
}
void GfxSetBloom(bool on) {
    BYTE *p = BloomJump();
    if (!p) return;
    BYTE op = on ? 0x74 : 0xeb;
    if (!PatchBytes(p, &op, 1)) return;
    wchar_t ini[MAX_PATH];
    IniFile(ini);
    WritePrivateProfileStringW(L"gfx", L"Bloom", on ? L"1" : L"0", ini);
    Log("gfx: bloom %s", on ? "ON" : "OFF");
}
