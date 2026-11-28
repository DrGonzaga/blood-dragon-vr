// string10 (DrGonzaga 11 Nov, thread "Make bowstring follow controllers 1:1"): the bow's string and the
// nocked arrow follow the right hand.
//
// Seven probes (string1p-8p) looked for the draw clip in the arms' animation graph and found
// nothing to scrub. This goes round the animation instead. The bow model has its own small table
// of bones (CGraphicComponent, found on the bow object with the game's own lookup 0x11706640 and
// type 0x10064250 + 0x18, exactly as the game's weapon parts do, e.g. 0x11246830): at component
// +0x80 an array of 0x90-byte entries, count at +0x84, each entry
//   +0x00 name hash, +0x04 parent index, +0x08 mode, +0x10 matrix in, +0x50 matrix out (model space)
// Each frame the animation fills "matrix in" with a mode (1 = relative to the parent, 2 = model
// space); the skin update 0x116eff50 then resolves them into "matrix out" (0x100c5ae0) and builds
// the skinning matrices from that (0x116e9b70, or a job for more than 20 bones). The game's own
// code poses bones the same way (turret barrels, wheels, ammo belts: 0x115dbdf0 writes mode 2).
//
// So one small hook sits inside the skin update, just before it asks "does this model need
// resolving?" (0x116effbb). For every other model it is one pointer compare. For the bow:
//   learning (once): the game's own draw is left to play on its own timer and every pose it goes
//     through is recorded, rest to full draw, indexed by how far the string's middle has moved.
//     The result is kept in bin\fc3vr_bowstring.bin.
//   driving (from then on): the whole bow pose is set from that record at the string travel the
//     right hand asks for, as mode 2 matrices, so the string sits at the hand, limbs bending as
//     the game drew them. The game's own draw animation no longer shows on the bow.
// The hand's travel is geometric: the right controller's nock point is taken into the bow's own
// space and measured along the string's path, so the string is where the hand is. The draw
// power follows the same travel (full at the "Full draw" distance or the string's own limit,
// whichever is nearer). The nocked arrow is glued to the string: its resting place on the bow is
// noted whenever the game holds it nocked and still, and it slides back with the string.
//
// Cost: one compare per skinned model update; for the bow, 16 floats per bone (about 10 bones).
// Off switches: [twohand] BowString=0 (Bow page), [debug] BowStringHook=0 (no code patch at all).
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
const DWORD kImageBase = 0x10000000;
enum { kMaxB = 32, kMaxS = 48, kRec = 240, kEntry = 0x90 };

bool g_on = true;  // [twohand] BowString
bool g_hooked;
void *g_skinBack;
BYTE *g_bdComp;

// The record: ns poses of nb bones, u = the string's travel at each (m, game units), rising.
struct Table {
    DWORD magic, size;
    int nb, ns, nock;
    float dmax, p0[3], dir[3];  // the nock bone at rest and the way it travels (model space, unit)
    DWORD hash[kMaxB];
    float u[kMaxS];
    float m[kMaxS][kMaxB][16];
};
const DWORD kMagic = 0x31534246;  // "FBS1"
Table g_tab, g_new;
volatile bool g_tabOk;
SRWLOCK g_tabLock = SRWLOCK_INIT;
volatile bool g_saveReq;

// From the game thread to the skin hook.
BYTE *volatile g_comp, *volatile g_bones;
volatile LONG g_n, g_mode;  // mode: 0 leave alone, 1 learn, 2 drive
volatile DWORD g_compTick;
volatile float g_dVis;     // the string travel to show (m, game units)
volatile float g_gameAmt;  // the game's own draw amount 0..1
volatile LONG g_busy, g_skinCalls, g_learnFails;
volatile DWORD g_skinSeen;
// Geometry for the input thread.
volatile float g_geoK, g_geoPull;
volatile DWORD g_geoTick;
char g_status[160] = "waiting for the bow";

float Dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void TablePath(wchar_t *path) {
    IniPath(path);
    wchar_t *s = wcsrchr(path, L'\\');
    wcscpy(s ? s + 1 : path, L"fc3vr_bowstring.bin");
}

void SaveTable() {
    static Table s_copy;
    AcquireSRWLockShared(&g_tabLock);
    bool ok = g_tabOk;
    if (ok) memcpy(&s_copy, &g_tab, sizeof s_copy);
    ReleaseSRWLockShared(&g_tabLock);
    wchar_t path[MAX_PATH];
    TablePath(path);
    if (!ok) {
        DeleteFileW(path);
        return;
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log("bowstring: could not write fc3vr_bowstring.bin (the string is learnt again next session)");
        return;
    }
    DWORD wr = 0;
    WriteFile(f, &s_copy, sizeof s_copy, &wr, nullptr);
    CloseHandle(f);
    Log("bowstring: record saved (fc3vr_bowstring.bin, %lu bytes)", wr);
}

bool TableSane(const Table &t) {
    if (t.magic != kMagic || t.size != sizeof(Table) || t.nb < 2 || t.nb > kMaxB || t.ns < 2 || t.ns > kMaxS || t.nock < 0 || t.nock >= t.nb) return false;
    if (!(t.dmax > 0.05f && t.dmax < 1.5f) || t.u[0] != 0) return false;
    for (int i = 1; i < t.ns; ++i)
        if (!(t.u[i] > t.u[i - 1])) return false;
    float dl = Dot3(t.dir, t.dir);
    return dl > 0.98f && dl < 1.02f;
}

// The path is measured from the STRING: of the bones that travel most of the way (string and arrow
// move together), the one rearmost along the pull at rest. (string10 took the arrow's own bone, which
// sits at the arrow's point, so the hand always read as past full draw.)
void Rebase(Table &t) {
    int pick = t.nock;
    float back = -1e9f;
    for (int b = 0; b < t.nb; ++b) {
        const float *a = t.m[0][b] + 12, *c = t.m[t.ns - 1][b] + 12;
        float d[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        if (Dot3(d, t.dir) < 0.6f * t.dmax) continue;
        float r = Dot3(a, t.dir);
        if (r > back) back = r, pick = b;
    }
    const float *a = t.m[0][pick] + 12;
    for (int k = 0; k < 3; ++k) t.p0[k] = a[k];
    Log("bowstring: pull measured from bone %d (hash %08lx) resting at (%.3f %.3f %.3f); travel bone %d", pick, t.hash[pick], t.p0[0], t.p0[1],
        t.p0[2], t.nock);
}

void LoadTable() {
    wchar_t path[MAX_PATH];
    TablePath(path);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD rd = 0;
    bool ok = ReadFile(f, &g_new, sizeof g_new, &rd, nullptr) && rd == sizeof g_new && TableSane(g_new);
    CloseHandle(f);
    if (!ok) {
        Log("bowstring: fc3vr_bowstring.bin not usable; the string is learnt again at the next full draw");
        return;
    }
    Rebase(g_new);
    memcpy(&g_tab, &g_new, sizeof g_tab);
    g_tabOk = true;
    Log("bowstring: record loaded (%d poses of %d bones, string travel %.1f cm)", g_tab.ns, g_tab.nb, g_tab.dmax * 100);
}

// ---- the skin hook's two jobs (one model's update at a time: g_busy) ----
float (*g_rec)[kMaxB][16];  // the learning draw, pose by pose
int g_recN;

void Build(const BYTE *bones, int n) {
    const float *rest = &g_rec[0][0][0];
    float best = 0;
    int bb = -1, bi = 0, moved = 0;
    for (int b = 0; b < n; ++b) {
        float mx = 0;
        for (int i = 1; i < g_recN; ++i) {
            const float *p = g_rec[i][b] + 12, *q = rest + b * 16 + 12;
            float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]}, l = sqrtf(Dot3(d, d));
            if (l > mx) mx = l;
            if (l > best) best = l, bb = b, bi = i;
        }
        if (mx > 0.002f) ++moved;
    }
    if (bb < 0 || best < 0.08f || best > 1.5f) {
        LONG nf = InterlockedIncrement(&g_learnFails);
        if (nf <= 3) {
            Log("bowstring: the draw moved no bone far enough (%d poses, furthest bone %d moved %.1f cm); not learnt (try %ld)", g_recN, bb,
                best * 100, nf);
            for (int b = 0; b < n && nf == 1; ++b) {
                const float *q = rest + b * 16 + 12, *p = g_rec[g_recN - 1][b] + 12;
                Log("bowstring:   bone %d hash %08lx parent %d rest (%.3f %.3f %.3f) end (%.3f %.3f %.3f)", b, *(DWORD *)(bones + b * kEntry),
                    *(int *)(bones + b * kEntry + 4), q[0], q[1], q[2], p[0], p[1], p[2]);
            }
        }
        return;
    }
    Table &t = g_new;
    memset(&t, 0, sizeof t);
    t.magic = kMagic, t.size = sizeof(Table), t.nb = n, t.nock = bb, t.dmax = best;
    const float *q0 = rest + bb * 16 + 12, *q1 = g_rec[bi][bb] + 12;
    for (int k = 0; k < 3; ++k) t.p0[k] = q0[k], t.dir[k] = (q1[k] - q0[k]) / best;
    for (int b = 0; b < n; ++b) t.hash[b] = *(DWORD *)(bones + b * kEntry);
    // The poses on the way out, each further along than the last (2 mm steps), the furthest last.
    static int s_idx[kRec];
    static float s_u[kRec];
    int c = 0;
    s_idx[c] = 0, s_u[c++] = 0;
    for (int i = 1; i <= bi; ++i) {
        const float *p = g_rec[i][bb] + 12;
        float d[3] = {p[0] - q0[0], p[1] - q0[1], p[2] - q0[2]}, u = Dot3(d, t.dir);
        if (i == bi) {
            if (u <= s_u[c - 1] && c > 1) --c;  // (the furthest replaces a neighbour within 2 mm)
            s_idx[c] = i, s_u[c++] = u;
        } else if (u > s_u[c - 1] + 0.002f && u < best - 0.002f) {
            s_idx[c] = i, s_u[c++] = u;
        }
    }
    if (c < 2) return;
    int ns = c > kMaxS ? kMaxS : c;
    for (int j = 0; j < ns; ++j) {
        int src = ns == c ? j : (int)((long long)j * (c - 1) / (ns - 1));
        t.u[j] = s_u[src];
        memcpy(t.m[j], g_rec[s_idx[src]], sizeof(float) * 16 * n);
    }
    t.ns = ns, t.u[0] = 0, t.dmax = t.u[ns - 1];
    if (!TableSane(t)) {
        Log("bowstring: the recorded draw did not make a usable record (%d poses); not learnt", c);
        return;
    }
    Rebase(t);
    AcquireSRWLockExclusive(&g_tabLock);
    memcpy(&g_tab, &t, sizeof g_tab);
    g_tabOk = true;
    ReleaseSRWLockExclusive(&g_tabLock);
    g_saveReq = true;
    Log("bowstring: LEARNT from the game's draw: %d poses kept of %d, %d of %d bones move, string = bone %d (hash %08lx), travel %.1f cm, "
        "path (%.2f %.2f %.2f) from (%.3f %.3f %.3f)",
        ns, g_recN, moved, n, bb, t.hash[bb], t.dmax * 100, t.dir[0], t.dir[1], t.dir[2], t.p0[0], t.p0[1], t.p0[2]);
}

float MaxDiff(const float *a, const float *b, int n) {
    float m = 0;
    for (int i = 0; i < n; ++i) {
        float d = fabsf(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

void Learn(const BYTE *bones, int n) {
    static float s_rest[kMaxB][16], s_prev[kMaxB][16];
    static bool s_haveRest, s_on, s_wait;  // s_wait: this draw has been dealt with; nothing more until it is let go
    static int s_still;
    static DWORD s_t0, s_full;
    if (!g_rec) {
        g_rec = (float (*)[kMaxB][16])VirtualAlloc(nullptr, sizeof(float) * 16 * kMaxB * kRec, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_rec) return;
    }
    float cur[kMaxB][16];
    for (int b = 0; b < n; ++b) memcpy(cur[b], bones + b * kEntry + 0x50, 64);
    DWORD now = GetTickCount();
    const int nf = n * 16;
    bool rt = BowRtHeld() && BowState() == 2;
    if (!rt) {
        s_wait = false;
        if (s_on) {
            s_on = false;
            if (s_full) Build(bones, n);
            else Log("bowstring: let go before the game's draw was full (%d poses); the next full draw is recorded", g_recN);
        }
        // the bow at rest: a pose that has held still for a few frames with nothing drawn
        s_still = MaxDiff(&cur[0][0], &s_prev[0][0], nf) < 2e-4f ? s_still + 1 : 0;
        memcpy(s_prev, cur, sizeof(float) * nf);
        if (s_still >= 8 && g_gameAmt < 0.001f) memcpy(s_rest, cur, sizeof(float) * nf), s_haveRest = true;
        return;
    }
    if (s_wait || g_tabOk) return;
    if (!s_on) {
        if (!s_haveRest) memcpy(s_rest, cur, sizeof(float) * nf);
        memcpy(g_rec[0], s_rest, sizeof(float) * nf);
        g_recN = 1, s_on = true, s_t0 = now, s_full = 0, s_still = 0;
        Log("bowstring: recording the game's own draw (hold the draw until it is full)");
    }
    if (g_recN < kRec && MaxDiff(&cur[0][0], &g_rec[g_recN - 1][0][0], nf) > 2e-4f) memcpy(g_rec[g_recN++], cur, sizeof(float) * nf);
    if (g_gameAmt >= 0.99f && !s_full) s_full = now | 1;
    if ((s_full && now - s_full > 600) || now - s_t0 > 8000) {
        s_on = false;
        if (s_full) Build(bones, n);
        else Log("bowstring: the game's draw never became full in 8 s; not learnt"), InterlockedIncrement(&g_learnFails);
        s_wait = true;
    }
}

void Drive(BYTE *comp, BYTE *bones, int n) {
    AcquireSRWLockShared(&g_tabLock);
    if (g_tabOk && g_tab.nb == n) {
        const Table &t = g_tab;
        float d = g_dVis, sc = *(float *)(comp + 0x90);
        d = d < 0 ? 0 : d > t.dmax ? t.dmax : d;
        int s = 0;
        while (s < t.ns - 2 && t.u[s + 1] < d) ++s;
        float w = (d - t.u[s]) / (t.u[s + 1] - t.u[s]);
        w = w < 0 ? 0 : w > 1 ? 1 : w;
        // the resolve multiplies the position by the model's scale; the record already holds it
        float inv = sc > 1e-4f ? 1 / sc : 1;
        for (int b = 0; b < n; ++b) {
            BYTE *e = bones + b * kEntry;
            float *o = (float *)(e + 0x10);
            const float *a = t.m[s][b], *c = t.m[s + 1][b];
            for (int k = 0; k < 16; ++k) o[k] = a[k] + (c[k] - a[k]) * w;
            o[12] *= inv, o[13] *= inv, o[14] *= inv;
            *(int *)(e + 8) = 2;
        }
        *(BYTE *)(comp + 0x8c) |= 3;  // needs resolving (as the game's own bone writers set it)
    }
    ReleaseSRWLockShared(&g_tabLock);
}

extern "C" void __cdecl __attribute__((force_align_arg_pointer)) BowSkinPre(BYTE *comp) {
    if (g_bloodDragon && comp == g_handsComp) HandsSkin(comp);  // hands1
    if (comp != g_comp || !comp) return;
    LONG mode = g_mode;
    if (!mode || GetTickCount() - g_compTick > 300) return;
    if (InterlockedExchange(&g_busy, 1)) return;
    BYTE *bones = *(BYTE **)(comp + 0x80);
    int n = *(int *)(comp + 0x84);
    if (bones && bones == g_bones && n == g_n) {
        InterlockedIncrement(&g_skinCalls);
        g_skinSeen = GetTickCount() | 1;
        if (mode == 1) Learn(bones, n);
        else Drive(comp, bones, n);
    }
    InterlockedExchange(&g_busy, 0);
}

// bowbd1: Blood Dragon, at 0x102d10e7, in place of "lea ecx,[esi+0x60]; test byte [ecx+0x2c],1".
// hands1: also stops for the player's model (hands.cpp).
__attribute__((naked)) void SkinStubBd() {
    __asm__ volatile(
        "cmpl %1, %%esi\n\t"
        "je 2f\n\t"
        "cmpl _g_handsComp, %%esi\n\t"
        "jne 1f\n\t"
        "2:\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %%esi\n\t"
        "call _BowSkinPre\n\t"
        "addl $4, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "1:\n\t"
        "leal 0x60(%%esi), %%ecx\n\t"
        "testb $1, 0x2c(%%ecx)\n\t"
        "jmp *%0\n\t" ::"m"(g_skinBack),
        "m"(g_comp));
}

// The string's nock point at travel d, as a move from its resting place (model space).
bool NockDelta(float d, float out[3]) {
    bool ok = false;
    AcquireSRWLockShared(&g_tabLock);
    if (g_tabOk) {
        const Table &t = g_tab;
        d = d < 0 ? 0 : d > t.dmax ? t.dmax : d;
        int s = 0;
        while (s < t.ns - 2 && t.u[s + 1] < d) ++s;
        float w = (d - t.u[s]) / (t.u[s + 1] - t.u[s]);
        w = w < 0 ? 0 : w > 1 ? 1 : w;
        const float *a = t.m[s][t.nock] + 12, *c = t.m[s + 1][t.nock] + 12;
        for (int k = 0; k < 3; ++k) out[k] = a[k] + (c[k] - a[k]) * w - t.m[0][t.nock][12 + k];
        ok = true;
    }
    ReleaseSRWLockShared(&g_tabLock);
    return ok;
}

// The bow's graphic component and its bone table, checked. Game thread.
BYTE *CompOf(BYTE *ent, BYTE **bones, int *n) {
    if (!ent || IsBadReadPtr(ent, 0x70)) return nullptr;
    BYTE *comp;
    if (!g_bdComp) return nullptr;
    comp = (BYTE *)BdThisCall(g_bdComp, ent, 0);  // bowbd1: BD has one call for "this object's graphic component" (0x1024409a)
    if (!comp || IsBadReadPtr(comp, 0x230)) return nullptr;
    BYTE *b = *(BYTE **)(comp + 0x80);
    int c = *(int *)(comp + 0x84);
    if (!b || c < 2 || c > kMaxB || IsBadWritePtr(b, c * kEntry)) return nullptr;
    for (int i = 0; i < c; ++i) {
        int par = *(int *)(b + i * kEntry + 4), mode = *(int *)(b + i * kEntry + 8);
        if (par < -1 || par >= c || mode < 0 || mode > 4) return nullptr;
    }
    *bones = b, *n = c;
    return comp;
}

BYTE *volatile g_stratEnt;  // the bow object as the game's bow code names it
volatile DWORD g_stratTick;  // when the game's bow code last ran (its object is alive then)
}  // namespace

// hands1: Blood Dragon's "this object's graphic component" (null if not hooked or none).
BYTE *BowStringCompBd(BYTE *ent) {
    if (!g_bdComp || !ent || IsBadReadPtr(ent, 0x70)) return nullptr;
    return (BYTE *)BdThisCall(g_bdComp, ent, 0);
}
bool *BowStringOn() { return &g_on; }
const char *BowStringStatus() { return g_status; }
bool BowStringLearning() { return g_hooked && g_on && g_mode == 1 && g_skinSeen; }
bool BowStringDriving() { return g_hooked && g_on && g_mode == 2; }

void BowStringRelearn() {
    AcquireSRWLockExclusive(&g_tabLock);
    g_tabOk = false;
    ReleaseSRWLockExclusive(&g_tabLock);
    g_learnFails = 0;
    MenuSaveLater(SaveTable);  // removes the file
    Log("bowstring: record dropped; the next full draw is recorded");
}

// bow.cpp, after the game's bow update: its draw amount, and the bow object it works on.
void BowStringGame(BYTE *strategy, float amount) {
    g_gameAmt = amount;
    BYTE *ent = nullptr;
    if (!IsBadReadPtr(strategy + 0x48, 4)) {
        BYTE *a = *(BYTE **)(strategy + 0x48);
        if (a && !IsBadReadPtr(a + 4, 4)) {
            BYTE *b = *(BYTE **)(a + 4);
            if (b && !IsBadReadPtr(b + 0xc, 4)) ent = *(BYTE **)(b + 0xc);
        }
    }
    g_stratEnt = ent, g_stratTick = GetTickCount();
}

// Input thread: the draw 0..1 and the pull (m) from where the hand is on the string's path.
bool BowStringGeo(float *k, float *pull) {
    if (!BowStringDriving() || GetTickCount() - g_geoTick > 150) return false;
    *k = g_geoK, *pull = g_geoPull;
    return true;
}

// gunattach.cpp, game thread, each frame the bow is placed: M = the bow's world matrix as drawn
// (rows right, forward, up, position), rightHand / RF = the right controller's world position and
// frame (rows right, forward, up; null = not tracked), leftHand = the bow hand's world position.
void BowStringFrame(BYTE *bowEnt, const float *M, const float *rightHand, const float *RF, const float *leftHand, float scale) {
    if (!g_hooked) return;
    DWORD now = GetTickCount();
    static BYTE *s_ent, *s_comp;
    static DWORD s_chk, s_since, s_stat;
    static bool s_match, s_alt;
    static int s_dumps;
    if (bowEnt != s_ent || now - s_chk > 500) {
        s_chk = now;
        BYTE *bones = nullptr, *comp;
        int n = 0;
        BYTE *alt = now - g_stratTick < 200 ? (BYTE *)g_stratEnt : nullptr;  // only while the game's bow code is running on it
        comp = CompOf(s_alt && alt ? alt : bowEnt, &bones, &n);
        if (!comp && alt && alt != bowEnt) comp = CompOf(alt, &bones, &n);
        if (comp != s_comp || bones != g_bones || n != g_n || bowEnt != s_ent) {
            g_comp = nullptr;
            s_since = now, g_skinSeen = 0;
            s_match = false;
            if (comp) {
                AcquireSRWLockShared(&g_tabLock);
                s_match = g_tabOk && g_tab.nb == n;
                for (int b = 0; b < n && s_match; ++b) s_match = g_tab.hash[b] == *(DWORD *)(bones + b * kEntry);
                bool had = g_tabOk;
                ReleaseSRWLockShared(&g_tabLock);
                if (s_dumps < 2) {
                    ++s_dumps;
                    Log("bowstring: bow object %p (the game's bow code names %p), model part %p: %d bones at %p, flags %02x %02x, skin job %p, "
                        "skeleton %p, next %p, owner %p; saved record %s",
                        bowEnt, alt, comp, n, bones, *(BYTE *)(comp + 0x5c), *(BYTE *)(comp + 0x5e), *(void **)(comp + 0x150), *(void **)(comp + 0x48),
                        *(void **)(comp + 0x224), *(void **)(comp + 0x1f0), s_match ? "fits" : had ? "is for another model" : "none yet");
                    for (int b = 0; b < n; ++b) {
                        const BYTE *e = bones + b * kEntry;
                        const float *p = (const float *)(e + 0x50) + 12;
                        Log("bowstring:   bone %d hash %08lx parent %d mode %d at (%.3f %.3f %.3f)", b, *(DWORD *)e, *(int *)(e + 4), *(int *)(e + 8), p[0],
                            p[1], p[2]);
                    }
                }
            } else if (s_dumps < 2) {
                ++s_dumps;
                Log("bowstring: no bone table found on the bow object %p (nor on %p); the string stays the game's", bowEnt, alt);
            }
            g_bones = bones, g_n = n;
            g_comp = comp;
        }
        s_ent = bowEnt, s_comp = comp;
    }
    if (!s_comp) {
        g_mode = 0;
        snprintf(g_status, sizeof g_status, "the bow's bone table was not found (see the log)");
        return;
    }
    g_compTick = now;
    bool active = BowActive();
    // the record may have been learnt or dropped since the last look
    if (g_tabOk && !s_match && g_mode != 2) {
        AcquireSRWLockShared(&g_tabLock);
        s_match = g_tabOk && g_tab.nb == g_n;
        for (int b = 0; b < g_n && s_match; ++b) s_match = g_tab.hash[b] == *(DWORD *)(g_bones + b * kEntry);
        ReleaseSRWLockShared(&g_tabLock);
    }
    if (!g_tabOk) s_match = false;
    LONG mode = !g_on || !active ? 0 : s_match ? 2 : 1;
    // Two failed tries: no more learning this session (the draw goes back to following the hands
    // as before; "Learn the string again" on the Bow page tries again).
    const bool gaveUp = mode == 1 && g_learnFails >= 2;
    if (gaveUp) mode = 0;
    if (g_saveReq) g_saveReq = false, MenuSaveLater(SaveTable);
    // The skin update never came by for this model part: try the other candidate once, and say so.
    static LONG s_lastMode;
    if (mode && !s_lastMode) s_since = now;  // (counted from when the string work starts)
    s_lastMode = mode;
    if (mode && !g_skinSeen && now - s_since > 4000) {
        s_since = now;
        static int s_said;
        if (s_said++ < 3)
            Log("bowstring: the bow's model part %p was not updated through the skin step in 4 s (flags %02x %02x, job %p, skeleton %p); %s", s_comp,
                *(BYTE *)(s_comp + 0x5c), *(BYTE *)(s_comp + 0x5e), *(void **)(s_comp + 0x150), *(void **)(s_comp + 0x48),
                g_stratEnt && g_stratEnt != bowEnt ? "trying the other object" : "no other object to try");
        if (g_stratEnt && g_stratEnt != bowEnt) s_alt = !s_alt, s_ent = nullptr;
        else if (s_said >= 3) InterlockedExchange(&g_learnFails, 2);  // nothing to work with: leave the string to the game
    }
    float d = 0;
    bool geo = false;
    float p0[3], dir[3], dmax = 0;
    if (mode == 2) {
        AcquireSRWLockShared(&g_tabLock);
        memcpy(p0, g_tab.p0, 12), memcpy(dir, g_tab.dir, 12), dmax = g_tab.dmax;
        ReleaseSRWLockShared(&g_tabLock);
    }
    if (mode == 2 && rightHand && scale > 1e-4f) {
        const float *np = BowNockPoint();  // right, up, forward on the right controller (m)
        float rel[3], h[3];
        for (int k = 0; k < 3; ++k)
            rel[k] = rightHand[k] + (np[0] * RF[k] + np[1] * RF[6 + k] + np[2] * RF[3 + k]) / scale - M[12 + k];
        for (int j = 0; j < 3; ++j) {
            float l2 = Dot3(M + 4 * j, M + 4 * j);
            h[j] = l2 > 1e-8f ? Dot3(rel, M + 4 * j) / l2 - p0[j] : 0;
        }
        d = Dot3(h, dir);
        // the resting string from the bow hand, in the player's own metres
        float pw[3], br[3];
        for (int k = 0; k < 3; ++k) pw[k] = M[12 + k] + p0[0] * M[k] + p0[1] * M[4 + k] + p0[2] * M[8 + k], br[k] = pw[k] - leftHand[k];
        float brace = sqrtf(Dot3(br, br)) * scale;
        float span = *BowFullDraw() - brace;
        if (span < 0.10f) span = 0.10f;
        if (span > dmax * scale) span = dmax * scale;
        float k = d * scale / span;
        g_geoK = k < 0 ? 0 : k > 1 ? 1 : k;
        g_geoPull = d > 0 ? d * scale : 0;
        g_geoTick = now;
        geo = true;
        // the first draws of a session go in the log (once a second while nocked), to check the fit
        static DWORD s_log;
        static int s_logN;
        if (BowState() == 2 && now - s_log > 1000 && (s_logN < 40 || g_verboseLog)) {
            s_log = now, ++s_logN;
            Log("bowstring: hand %.1f cm along the string's path (limit %.1f), resting string %.1f cm from the bow hand, draw %.2f", d * 100,
                dmax * 100, brace * 100, (double)g_geoK);
        }
    }
    // What the string shows: at the hand while nocked (eased in over 150 ms from the nock, so the
    // string comes to the hand rather than jumping), back to rest in 45 ms after the release.
    static float s_vis, s_rel;
    static DWORD s_relMs;
    static bool s_wasNocked;
    DWORD ms = BowClockMs();
    if (mode == 2 && BowState() == 2 && geo) {
        float x = (float)(ms - BowNockMs()) / 150.0f;
        x = x < 0 ? 0 : x > 1 ? 1 : x;
        float dd = d < 0 ? 0 : d > dmax ? dmax : d;
        s_vis = dd * x * x * (3 - 2 * x);
        s_wasNocked = true;
    } else {
        if (s_wasNocked) s_wasNocked = false, s_rel = s_vis, s_relMs = ms;
        float x = (float)(ms - s_relMs) / 45.0f;
        s_vis = x >= 0 && x < 1 ? s_rel * (1 - x) * (1 - x) : 0;
    }
    g_dVis = s_vis;
    g_mode = mode;
    if (now - s_stat > 500) {
        s_stat = now;
        if (!g_on) snprintf(g_status, sizeof g_status, "off");
        else if (s_match)
            snprintf(g_status, sizeof g_status, "learnt: string travel %.0f cm; string now at %.0f cm%s", dmax > 0 ? dmax * 100 : g_tab.dmax * 100, s_vis * 100,
                     active ? "" : " (bow not in the left hand)");
        else if (gaveUp) snprintf(g_status, sizeof g_status, "could not be learnt from the game's draw (see the log); string left to the game");
        else if (!g_skinSeen && active) snprintf(g_status, sizeof g_status, "waiting for the bow's model to update (see the log if this stays)");
        else snprintf(g_status, sizeof g_status, "not learnt yet: nock an arrow, draw and HOLD for 2 seconds, once");
    }
}

// gunattach.cpp, game thread: an arrow as the game poses it, in the bow's own axes (loc = turn,
// off = place). kind 0 = the bow's own arrow, 1 = an arrow from the player's hand. Notes where it
// rests when the game holds it nocked and still; returns true (loc / off replaced) when the arrow
// is to sit on our string instead.
static struct {
    float loc[9], off[3], pl[9], po[3];
    int still;
    bool ok;
} g_arrowRest[2];
static volatile bool g_handRestOk, g_handRestLoaded;
bool BowStringHandRestKnown() {
    if (!g_handRestLoaded) {  // before the first arrow: is there a saved record?
        wchar_t path[MAX_PATH], b[8];
        IniPath(path);
        g_handRestOk = GetPrivateProfileStringW(L"bowstring", L"HandArrowRest", L"", b, 8, path) > 0;
        g_handRestLoaded = true;
    }
    return g_handRestOk;
}
bool BowStringArrow(int kind, float loc[9], float off[3]) {
    if (!g_hooked || !g_on || kind < 0 || kind > 1 || !g_tabOk) return false;
    auto &s = g_arrowRest;
    auto &a = s[kind];
    static bool s_loaded;
    if (!s_loaded) {  // hands9: the hand arrow's rest, saved once learnt ([bowstring] HandArrowRest)
        s_loaded = true;
        wchar_t path[MAX_PATH], b[400];
        IniPath(path);
        float f[12];
        if (GetPrivateProfileStringW(L"bowstring", L"HandArrowRest", L"", b, 400, path) &&
            swscanf(b, L"%f %f %f %f %f %f %f %f %f %f %f %f", f, f + 1, f + 2, f + 3, f + 4, f + 5, f + 6, f + 7, f + 8, f + 9, f + 10, f + 11) == 12) {
            memcpy(s[1].loc, f, 36), memcpy(s[1].off, f + 9, 12), s[1].ok = true;
            Log("bowstring: hand arrow's rest on the bow from the saved record (%.3f %.3f %.3f)", f[9], f[10], f[11]);
        }
    }
    // hands9: with the VR hands holding the player's arm bones, the game's pose of a hand arrow is the
    // controller's, not its animation's (bd37 / bd38 learnt wrong rests from that): no learning then.
    bool ours = kind == 1 && HandsLive();
    if (ours) a.still = 0;
    if (!ours && g_gameAmt < 0.001f && !BowRtHeld() && g_dVis < 1e-4f) {
        float ch = MaxDiff(loc, a.pl, 9), cp = MaxDiff(off, a.po, 3);
        a.still = ch < 0.002f && cp < 0.001f ? a.still + 1 : 0;
        memcpy(a.pl, loc, 36), memcpy(a.po, off, 12);
        if (a.still == 20 && Dot3(off, off) < 1.5f * 1.5f) {
            float dir[3], best = 0;
            AcquireSRWLockShared(&g_tabLock);
            memcpy(dir, g_tab.dir, 12);
            ReleaseSRWLockShared(&g_tabLock);
            for (int i = 0; i < 3; ++i) best = fmaxf(best, fabsf(Dot3(loc + 3 * i, dir)));
            // hands9: Blood Dragon's hand arrow rests at (-0.285 -0.117 -0.050) in every log before the VR
            // hands; with them on, anything else is the controller's pose that slipped through: not taken.
            bool odd = kind == 1 && g_bloodDragon && *HandsOn() &&
                       (fabsf(off[0] + 0.285f) > 0.05f || fabsf(off[1] + 0.117f) > 0.05f || fabsf(off[2] + 0.050f) > 0.05f);
            if (odd && best > 0.9f) {
                static int s_odd;
                if (s_odd++ < 12) Log("bowstring: hand arrow seen at rest at (%.3f %.3f %.3f): not its usual place, not taken", off[0], off[1], off[2]);
            }
            if (best > 0.9f && !odd) {  // lying along the string's path: nocked, not being fetched
                bool first = !a.ok || MaxDiff(loc, a.loc, 9) > 0.02f || MaxDiff(off, a.off, 3) > 0.01f;
                memcpy(a.loc, loc, 36), memcpy(a.off, off, 12), a.ok = true;
                if (kind == 1) g_handRestOk = g_handRestLoaded = true;
                if (first) Log("bowstring: %s arrow rests on the bow at (%.3f %.3f %.3f)", kind ? "hand" : "the bow's own", off[0], off[1], off[2]);
                if (first && kind == 1) {
                    wchar_t path[MAX_PATH], b[400];
                    IniPath(path);
                    swprintf(b, 400, L"%.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", loc[0], loc[1], loc[2], loc[3], loc[4], loc[5], loc[6], loc[7],
                             loc[8], off[0], off[1], off[2]);
                    WritePrivateProfileStringW(L"bowstring", L"HandArrowRest", b, path);
                }
            }
        }
    } else {
        a.still = 0;
    }
    if (g_mode != 2 || BowArrowMode() != 3 || !a.ok) return false;
    float dl[3];
    if (!NockDelta(g_dVis, dl)) return false;
    memcpy(loc, a.loc, 36);
    for (int k = 0; k < 3; ++k) off[k] = a.off[k] + dl[k];
    return true;
}

// bowbd1: Blood Dragon. Skin update 0x102d107f (FC3 0x116eff50), the same test at 0x102d10e7;
// the bone table layout is the same (entries of 0x90 at +0x80, count +0x84, flags +0x8c).
void BowStringInstallBd(BYTE *bd) {
    wchar_t path[MAX_PATH];
    IniPath(path);
    if (!GetPrivateProfileIntW(L"debug", L"BowStringHook", 1, path)) {
        Log("bowstring: off ([debug] BowStringHook=0)");
        return;
    }
    BYTE *site = BdAt(bd, 0x2d10e7), *cf = BdAt(bd, 0x24409a);
    static const BYTE kSite[9] = {0x8d, 0x4e, 0x60, 0xf6, 0x41, 0x2c, 0x01, 0x74, 0x41};
    static const BYTE kComp[4] = {0x56, 0x8b, 0xf1, 0xe8};
    if (IsBadReadPtr(site, 9) || memcmp(site, kSite, 9) || memcmp(cf, kComp, 4) || memcmp(cf + 8, "\x83\xc0\x18\x50\x8b\xce\xe8", 7)) {
        Log("bowstring: Blood Dragon's skin update is not where expected; the string stays the game's");
        return;
    }
    g_skinBack = site + 7;
    g_bdComp = cf;
    LoadTable();
    if (!PatchJmp(site, (void *)SkinStubBd, 7)) return;
    g_hooked = true;
    Log("bowstring: Blood Dragon skin update hooked; string follows the hand %s", g_tabOk ? "from the saved record" : "after one full draw");
}
