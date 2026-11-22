// body1 (31 Oct): hide the player's own body and arms (seen when turning round
// with 6DoF). The game has no switch for it, so we find its draws on the GPU:
// DrawIndexed / DrawIndexedInstanced on the game's immediate context are
// hotpatched (the engine may call cached function pointers, so the vtable alone
// isn't enough) and each draw gets a signature: index buffer size and offset,
// index count, start index, base vertex. A live "body hunt" on the Debug page
// halves the set of draws seen while looking at the body until one is left
// (you answer "gone" or "still there" each time), and saved signatures are
// skipped while "Hide my body" is on. Our own drawing (beam, HUD, menu, all in
// Present) is never touched.
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "fc3vr.h"

namespace {

typedef void(STDMETHODCALLTYPE *DrawIndexedFn)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInstFn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *SetIBFn)(ID3D11DeviceContext *, ID3D11Buffer *, DXGI_FORMAT, UINT);
DrawIndexedFn o_di;
DrawIndexedInstFn o_dii;
SetIBFn o_ib;
ID3D11DeviceContext *g_ctx;
volatile bool g_inPresent;

ID3D11Buffer *g_ib;
UINT g_ibSize, g_ibOff;

// Open-addressing set of 64-bit signatures (0 = empty).
struct Set {
    static const int kSize = 1 << 15;
    unsigned long long *v;
    int n;
    void Init() {
        if (!v) v = (unsigned long long *)VirtualAlloc(nullptr, kSize * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Clear();
    }
    void Clear() {
        if (v) memset(v, 0, kSize * 8);
        n = 0;
    }
    bool Has(unsigned long long s) const {
        if (!v || !n) return false;
        for (unsigned i = (unsigned)(s ^ (s >> 29)) & (kSize - 1);; i = (i + 1) & (kSize - 1)) {
            if (v[i] == s) return true;
            if (!v[i]) return false;
        }
    }
    bool Add(unsigned long long s) {
        if (!v || n >= kSize / 2) return false;
        for (unsigned i = (unsigned)(s ^ (s >> 29)) & (kSize - 1);; i = (i + 1) & (kSize - 1)) {
            if (v[i] == s) return false;
            if (!v[i]) {
                v[i] = s, ++n;
                return true;
            }
        }
    }
};

Set g_shown;   // hands2: the saved draws ticked "show" (the hands): drawn only while the hands are on the gun
volatile bool g_anyShown;
Set g_hidden;  // skipped now: saved body draws (if on) or the hunt's test half
Set g_seen;    // collected while the hunt watches
volatile bool g_collect;
unsigned long long *g_cand;  // hunt candidates (array)
int g_ncand, g_lo, g_hi;
int g_phase;  // 0 idle, 1 collecting, 2 testing, 3 one left
DWORD g_collectUntil;
unsigned long long g_saved[64];
int g_nsaved;
bool g_hideOn = true;
bool g_show[64];  // hands1: saved draw i is shown again ([body] Show<i>=1), to find which ones are the hands
LONG g_skipped;

unsigned long long Sig(UINT count, UINT start, INT base) {
    unsigned long long s = 1469598103934665603ull;
    auto mix = [&](unsigned long long x) { s = (s ^ x) * 1099511628211ull; };
    mix(g_ibSize), mix(g_ibOff), mix(count), mix(start), mix((unsigned)base);
    return s ? s : 1;
}

bool Skip(UINT count, UINT start, INT base) {
    if (g_inPresent || (!g_hidden.n && !g_collect && !g_shown.n)) return false;
    unsigned long long s = Sig(count, start, base);
    if (g_collect) g_seen.Add(s);
    // hands2: with VR hands on, a ticked part is only drawn while the arms are on the gun (or the mod's menu is open, to pick parts)
    if (g_hidden.Has(s) || (g_shown.n && g_hideOn && g_bloodDragon && *HandsOn() && !HandsLive() && !MenuOpen() && !HandsGameArmsNow() && g_shown.Has(s))) {
        ++g_skipped;
        return true;
    }
    return false;
}

void STDMETHODCALLTYPE HDI(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    if (c == g_ctx && Skip(n, s, b)) return;
    o_di(c, n, s, b);
}
void STDMETHODCALLTYPE HDII(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    if (c == g_ctx && Skip(n, s, b)) return;
    o_dii(c, n, inst, s, b, si);
}
void STDMETHODCALLTYPE HIB(ID3D11DeviceContext *c, ID3D11Buffer *ib, DXGI_FORMAT f, UINT off) {
    if (c == g_ctx) {
        if (ib != g_ib) {
            g_ib = ib, g_ibSize = 0;
            if (ib) {
                D3D11_BUFFER_DESC d;
                ib->GetDesc(&d);
                g_ibSize = d.ByteWidth;
            }
        }
        g_ibOff = off;
    }
    o_ib(c, ib, f, off);
}

// Hotpatch: "mov edi,edi" at the entry and 5 spare bytes before it.
bool HotPatch(void *fn, void *hook, void **orig, const char *name) {
    BYTE *p = (BYTE *)fn;
    if (p[0] != 0x8b || p[1] != 0xff) {
        Log("body: %s at %p not hotpatchable (%02x %02x %02x); off", name, fn, p[0], p[1], p[2]);
        return false;
    }
    for (int i = 1; i <= 5; ++i)
        if (p[-i] != 0x90 && p[-i] != 0xcc) {
            Log("body: %s at %p has no room before it; off", name, fn);
            return false;
        }
    *orig = p + 2;
    DWORD old;
    VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old);
    p[-5] = 0xe9;
    *(int *)(p - 4) = (int)((BYTE *)hook - p);
    *(volatile WORD *)p = 0xf9eb;  // jmp $-5
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return true;
}

void Apply() {
    // what is skipped: the hunt's test half while testing, else the saved list (if on)
    g_hidden.Clear();
    if (g_phase == 2) {
        int mid = (g_lo + g_hi) / 2;
        for (int i = g_lo; i < mid; ++i) g_hidden.Add(g_cand[i]);
    } else if (g_phase == 3) {
        g_hidden.Add(g_cand[g_lo]);
    }
    g_shown.Clear();
    if (g_hideOn)
        for (int i = 0; i < g_nsaved; ++i) {
            if (!g_show[i]) g_hidden.Add(g_saved[i]);
            else if (g_phase == 0) g_shown.Add(g_saved[i]);
        }
    g_anyShown = g_shown.n > 0;
}

void Ini(bool save) {
    wchar_t path[MAX_PATH], key[16], buf[32];
    IniPath(path);
    if (save) {
        WritePrivateProfileStringW(L"body", nullptr, nullptr, path);
        swprintf(buf, 32, L"%d", g_nsaved);
        WritePrivateProfileStringW(L"body", L"Count", buf, path);
        WritePrivateProfileStringW(L"body", L"Hide", g_hideOn ? L"1" : L"0", path);
        for (int i = 0; i < g_nsaved; ++i) {
            swprintf(key, 16, L"Draw%d", i);
            swprintf(buf, 32, L"%016llx", g_saved[i]);
            WritePrivateProfileStringW(L"body", key, buf, path);
            if (g_show[i]) {
                swprintf(key, 16, L"Show%d", i);
                WritePrivateProfileStringW(L"body", key, L"1", path);
            }
        }
        return;
    }
    g_hideOn = GetPrivateProfileIntW(L"body", L"Hide", 1, path) != 0;
    int n = GetPrivateProfileIntW(L"body", L"Count", 0, path);
    g_nsaved = 0;
    for (int i = 0; i < n && i < 64; ++i) {
        swprintf(key, 16, L"Draw%d", i);
        if (GetPrivateProfileStringW(L"body", key, L"", buf, 32, path)) {
            g_saved[g_nsaved] = _wcstoui64(buf, nullptr, 16);
            swprintf(key, 16, L"Show%d", i);
            g_show[g_nsaved++] = GetPrivateProfileIntW(L"body", key, 0, path) != 0;
        }
    }
}

}  // namespace

void BodyHookContext(ID3D11DeviceContext *ctx) {
    if (g_ctx) return;
    g_ctx = ctx;
    g_hidden.Init(), g_seen.Init(), g_shown.Init();
    g_cand = (unsigned long long *)VirtualAlloc(nullptr, Set::kSize * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Ini(false);
    Apply();
    void **vt = *(void ***)ctx;
    bool ok = HotPatch(vt[19], (void *)HIB, (void **)&o_ib, "IASetIndexBuffer") &&
              HotPatch(vt[12], (void *)HDI, (void **)&o_di, "DrawIndexed") &&
              HotPatch(vt[20], (void *)HDII, (void **)&o_dii, "DrawIndexedInstanced");
    Log("body: draw hooks %s, %d body draws saved (hide %s)", ok ? "installed" : "NOT installed", g_nsaved,
        g_hideOn ? "on" : "off");
}
void BodyPresent(bool in) { g_inPresent = in; }

// Called by the menu every frame it is drawn.
void BodyTick() {
    if (g_phase == 1 && GetTickCount() > g_collectUntil) {
        g_collect = false;
        g_ncand = 0;
        for (int i = 0; i < Set::kSize; ++i)
            if (g_seen.v[i]) {
                bool saved = false;
                for (int k = 0; k < g_nsaved; ++k) saved |= g_saved[k] == g_seen.v[i];
                if (!saved) g_cand[g_ncand++] = g_seen.v[i];
            }
        g_lo = 0, g_hi = g_ncand;
        g_phase = g_ncand > 1 ? 2 : g_ncand == 1 ? 3 : 0;
        Log("body: hunt collected %d draws", g_ncand);
        Apply();
    }
}
const char *BodyStatus(int *left) {
    *left = g_hi - g_lo;
    switch (g_phase) {
    case 1: return "watching for 1 s: keep looking at your body";
    case 2: return "half the candidates hidden: is the part you watch gone?";
    case 3: return "one draw left, hidden now: is the part gone?";
    default: return g_nsaved ? "idle" : "idle (nothing saved yet)";
    }
}
int BodyPhase() { return g_phase; }
void BodyStartHunt() {
    g_seen.Clear();
    g_phase = 1, g_collectUntil = GetTickCount() + 1000;
    Apply();  // the saved ones stay hidden, so the hunt finds the next part
    g_collect = true;
    Log("body: hunt started");
}
void BodyAnswer(bool gone) {
    if (g_phase == 2) {
        int mid = (g_lo + g_hi) / 2;
        if (gone) g_hi = mid;
        else g_lo = mid;
        if (g_hi - g_lo <= 1) g_phase = g_hi > g_lo ? 3 : 0;
        Log("body: hunt answer %s, %d left", gone ? "gone" : "still there", g_hi - g_lo);
    } else if (g_phase == 3) {
        if (gone && g_nsaved < 64) {
            g_show[g_nsaved] = false;
            g_saved[g_nsaved++] = g_cand[g_lo];
            Log("body: draw %016llx saved as body (%d saved)", g_cand[g_lo], g_nsaved);
            Ini(true);
        } else {
            Log("body: last draw wasn't it; hunt ended");
        }
        g_phase = 0;
    }
    Apply();
}
void BodyCancel() {
    g_phase = 0, g_collect = false;
    Apply();
}
bool *BodyHideOn() { return &g_hideOn; }
void BodyHideChanged() {
    Ini(true);
    Apply();
}
int BodySaved() { return g_nsaved; }
bool BodyAnyShown() { return g_anyShown; }
bool *BodyShow(int i) { return i >= 0 && i < g_nsaved ? &g_show[i] : nullptr; }
void BodyShowChanged() {
    Ini(true);
    Apply();
    char line[80];
    int at = 0;
    for (int i = 0; i < g_nsaved; ++i)
        if (g_show[i]) at += snprintf(line + at, sizeof line - at, " %d", i + 1);
    line[at] = 0;
    Log("body: parts shown again:%s", at ? line : " none");
}
void BodyClear() {
    memset(g_show, 0, sizeof g_show);
    g_nsaved = 0;
    Ini(true);
    Apply();
    Log("body: saved body draws cleared");
}
