// prompts1 (DrGonzaga 8 Nov): hide one HUD prompt ("press X to switch to normal
// arrow" with explosive arrows) while keeping the others that share its spot
// ("press X to loot" etc.).
// RE (Ghidra, 8 Nov): every on-screen game text comes from the localisation
// lookup FUN_117623b0 (stdcall, 5 args: table, out string handle, mode, text id,
// language; mode 0 = build the text). The string object is {refcount, length,
// capacity, wchar text[]} (FUN_100043a0). Here the lookup is hooked: after it
// runs, a text id seen for the first time is checked once against the block
// list ([hud] HidePromptText, ';'-separated, case-insensitive, default
// "normal arrow"); blocked ids get an empty text. Every later lookup of an id
// is one table probe, no string work.
#include <windows.h>
#include <wchar.h>

#include "fc3vr.h"

namespace {
const DWORD kImageBase = 0x10000000;
volatile bool g_on = true;  // [hud] HidePrompts
wchar_t g_block[8][48];
int g_nblock;

// id -> 1 keep / 2 blank; open addressing, ids never removed (the set is small).
const int kSlots = 8192;
volatile LONG g_id[kSlots];
volatile LONG g_state[kSlots];
// prompts3 g_bdLines removed in bd79 (tidy1): the Blood Dragon text survey is detailed-log only now
volatile LONG g_epoch = 1;  // bumped when the switch changes, so ids are checked again

wchar_t Low(wchar_t c) { return c >= L'A' && c <= L'Z' ? c + 32 : c; }
bool Has(const wchar_t *s, int n, const wchar_t *pat) {
    int m = (int)wcslen(pat);
    for (int i = 0; i + m <= n; ++i) {
        int k = 0;
        while (k < m && Low(s[i + k]) == pat[k]) ++k;
        if (k == m) return true;
    }
    return false;
}


// vrtext1 (DrGonzaga 23 Nov, player feedback on v0.69: the game's prompts name the flat game's pad buttons).
// The game's texts carry action tokens ("HOLD [Loot] TO PILFER") that its UI later turns into a pad button
// picture. bin\bdvr-prompts.txt (DrGonzaga edits it; made from the game's English string table) holds
//   [BUTTON NAMES]  [Loot] = B            -> every text using the token shows the name instead
//   [TEXTS]         #56331 / GAME: ... / VR: new text   -> the whole text replaced (any length)
// The file is read at start and again when it is saved (checked once a second). A replaced text is put in
// the caller's string handle with the game's own string calls: release BD+100056c7 (frees at refcount 0),
// build from wchar_t* BD+10005f33 (refcount 1), the same pair the lookup itself uses.
volatile bool g_vrText = true;  // [hud] VrPrompts
const wchar_t *volatile g_txt[kSlots];  // slot -> final text for state 3 (never freed; reloads are rare)
struct Table {
    int nlab;
    wchar_t act[96][32];
    const wchar_t *lab[96];
    int nrep;
    int id[1024];
    const wchar_t *rep[1024];
};
Table *volatile g_tab;
wchar_t g_file[MAX_PATH];
FILETIME g_fileTime;
volatile LONG g_pollAt;

typedef void(__thiscall *StrReleaseFn)(DWORD *, DWORD *);
typedef void(__thiscall *StrFromFn)(DWORD *, const wchar_t *);
StrReleaseFn g_strRelease;
StrFromFn g_strFrom;

wchar_t *Dup(const wchar_t *a, int n) {
    wchar_t *d = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * 2);
    if (d) memcpy(d, a, n * 2), d[n] = 0;
    return d;
}
bool EqN(const wchar_t *a, int n, const wchar_t *b) {
    for (int i = 0; i < n; ++i)
        if (!b[i] || Low(a[i]) != Low(b[i])) return false;
    return b[n] == 0;
}
// Token at t[i] == '[': length of the name (0 if not a token).
int TokenAt(const wchar_t *t, int n, int i) {
    int j = i + 1;
    while (j < n && j - i < 32 && ((t[j] >= L'A' && t[j] <= L'Z') || (t[j] >= L'a' && t[j] <= L'z') ||
                                   (t[j] >= L'0' && t[j] <= L'9') || t[j] == L'_'))
        ++j;
    return (j < n && t[j] == L']' && j > i + 1) ? j - i - 1 : 0;
}
bool NonButton(const wchar_t *a, int n) {  // date/time and multiplayer fill-ins, not buttons
    static const wchar_t *const k[] = {L"month", L"day", L"year", L"hour", L"minutes", L"map_name", L"game_mode"};
    for (auto w : k)
        if (EqN(a, n, w)) return true;
    return false;
}
bool HasToken(const wchar_t *t, int n) {
    for (int i = 0; i < n; ++i) {
        int m = t[i] == L'[' ? TokenAt(t, n, i) : 0;
        if (m && !NonButton(t + i + 1, m)) return true;
    }
    return false;
}
void LogUnknownTokens(const wchar_t *t, int n) {  // once per token name, so players' logs show new ones
    static wchar_t seen[48][32];
    static int nseen;
    Table *tb = g_tab;
    for (int i = 0; i < n; ++i) {
        int m = t[i] == L'[' ? TokenAt(t, n, i) : 0;
        if (!m || NonButton(t + i + 1, m)) continue;
        bool known = false;
        for (int k = 0; tb && k < tb->nlab && !known; ++k) known = EqN(t + i + 1, m, tb->act[k]);
        for (int k = 0; k < nseen && !known; ++k) known = EqN(t + i + 1, m, seen[k]);
        if (known || nseen >= 48) continue;
        memcpy(seen[nseen], t + i + 1, m * 2), seen[nseen][m] = 0;
        Log("prompts: button token [%ls] has no name in bdvr-prompts.txt (the game's pad picture shows)", seen[nseen]);
        ++nseen;
    }
}
// The final text for an id, or null to leave the game's text alone.
const wchar_t *Rewrite(int id, const wchar_t *t, int n) {
    Table *tb = g_tab;
    if (!tb) return nullptr;
    const wchar_t *src = t;
    int sn = n;
    bool whole = false;
    for (int k = 0; k < tb->nrep; ++k)
        if (tb->id[k] == id) { src = tb->rep[k], sn = (int)wcslen(src), whole = true; break; }
    // How the game writes a new line in this text: "\n" typed out, a real line break, or (none seen) a real one.
    bool typed = false;
    for (int i = 0; i + 1 < n; ++i)
        if (t[i] == L'\\' && t[i + 1] == L'n') { typed = true; break; }
    wchar_t *b = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, 8192 * 2);
    if (!b) return nullptr;
    int m = 0;
    bool hit = whole;
    for (int i = 0; i < sn && m < 8000;) {
        if (whole && !typed && src[i] == L'\\' && i + 1 < sn && src[i + 1] == L'n') { b[m++] = L'\n'; i += 2; continue; }
        int tl = src[i] == L'[' ? TokenAt(src, sn, i) : 0;
        if (tl) {
            const wchar_t *lab = nullptr;
            static wchar_t s_live[64];  // ctl3: the live binding when it differs from the shipped layout (one game thread)
            if (InputTokenLabel(src + i + 1, tl, s_live, 64)) lab = s_live;
            for (int k = 0; k < tb->nlab && !lab; ++k)
                if (EqN(src + i + 1, tl, tb->act[k])) lab = tb->lab[k];
            if (lab) {
                for (const wchar_t *q = lab; *q && m < 8000; ++q) b[m++] = *q;
                i += tl + 2;
                hit = true;
                continue;
            }
        }
        b[m++] = src[i++];
    }
    const wchar_t *r = hit ? Dup(b, m) : nullptr;
    HeapFree(GetProcessHeap(), 0, b);
    return r;
}
void Replace(DWORD *out, const wchar_t *nt) {
    if (!out || !nt || IsBadWritePtr(out, 4)) return;
    if (g_strRelease && g_strFrom) {
        g_strRelease(out, out);
        g_strFrom(out, nt);
        return;
    }
    // Fallback (calls not found): in place when it fits the game's own string.
    BYTE *o = (BYTE *)*out;
    int n = (int)wcslen(nt);
    if (o && !IsBadWritePtr(o, 0xc) && n <= *(int *)(o + 4) && !IsBadWritePtr(o + 0xc, (n + 1) * 2))
        memcpy(o + 0xc, nt, (n + 1) * 2), *(int *)(o + 4) = n;
}
Table *LoadTable() {
    HANDLE f = CreateFileW(g_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    DWORD sz = GetFileSize(f, nullptr), got = 0;
    char *raw = sz && sz < (8u << 20) ? (char *)HeapAlloc(GetProcessHeap(), 0, sz + 1) : nullptr;
    if (raw && !ReadFile(f, raw, sz, &got, nullptr)) got = 0;
    CloseHandle(f);
    if (!raw) return nullptr;
    char *u = raw;
    if (got >= 3 && (BYTE)u[0] == 0xef && (BYTE)u[1] == 0xbb && (BYTE)u[2] == 0xbf) u += 3, got -= 3;
    int wn = MultiByteToWideChar(CP_UTF8, 0, u, (int)got, nullptr, 0);
    wchar_t *w = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (wn + 1) * 2);
    if (!w) { HeapFree(GetProcessHeap(), 0, raw); return nullptr; }
    MultiByteToWideChar(CP_UTF8, 0, u, (int)got, w, wn);
    w[wn] = 0;
    HeapFree(GetProcessHeap(), 0, raw);
    Table *tb = (Table *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Table));
    if (!tb) return nullptr;
    int mode = 0, cur = 0;
    for (wchar_t *p = w; *p;) {
        wchar_t *e = p;
        while (*e && *e != L'\n') ++e;
        wchar_t *next = *e ? e + 1 : e;
        *e = 0;
        while (*p == L' ' || *p == L'\t') ++p;
        wchar_t *end = p + wcslen(p);
        while (end > p && (end[-1] == L'\r' || end[-1] == L' ' || end[-1] == L'\t')) *--end = 0;
        if (*p && *p != L';') {
            if (!_wcsicmp(p, L"[BUTTON NAMES]")) mode = 1;
            else if (!_wcsicmp(p, L"[TEXTS]")) mode = 2;
            else if (mode == 1 && *p == L'[') {
                int tl = TokenAt(p, (int)(end - p), 0);
                wchar_t *eq = tl ? p + tl + 2 : nullptr;
                while (eq && (*eq == L' ' || *eq == L'\t')) ++eq;
                if (eq && *eq == L'=' && tb->nlab < 96) {
                    wchar_t *v = eq + 1;
                    while (*v == L' ' || *v == L'\t') ++v;
                    wchar_t *ve = v;
                    while (*ve && *ve != L';') ++ve;
                    while (ve > v && (ve[-1] == L' ' || ve[-1] == L'\t')) --ve;
                    if (ve > v) {
                        memcpy(tb->act[tb->nlab], p + 1, tl * 2), tb->act[tb->nlab][tl] = 0;
                        tb->lab[tb->nlab++] = Dup(v, (int)(ve - v));
                    }
                }
            } else if (mode == 2 && *p == L'#') cur = _wtoi(p + 1);
            else if (mode == 2 && (p[0] == L'V' || p[0] == L'v') && (p[1] == L'R' || p[1] == L'r') && p[2] == L':') {
                wchar_t *v = p + 3;
                while (*v == L' ' || *v == L'\t') ++v;
                if (*v && cur > 0 && tb->nrep < 1024) tb->id[tb->nrep] = cur, tb->rep[tb->nrep++] = Dup(v, (int)wcslen(v));
                cur = 0;
            }
        }
        p = next;
    }
    HeapFree(GetProcessHeap(), 0, w);
    return tb;
}
bool FileTime(FILETIME *ft) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_file, GetFileExInfoStandard, &a)) { ft->dwLowDateTime = ft->dwHighDateTime = 0; return false; }
    *ft = a.ftLastWriteTime;
    return true;
}
void Reload(bool first) {
    FILETIME ft;
    bool have = FileTime(&ft);
    if (!first && !CompareFileTime(&ft, &g_fileTime)) return;
    g_fileTime = ft;
    Table *tb = have ? LoadTable() : nullptr;
    g_tab = tb;  // the old table is left (small; only on a save)
    if (tb) Log("prompts: bdvr-prompts.txt %s: %d button name(s), %d whole text(s)", first ? "read" : "changed, read again", tb->nlab, tb->nrep);
    else Log("prompts: no bdvr-prompts.txt next to fc3vr.ini; game prompts unchanged");
    if (!first) InterlockedIncrement(&g_epoch);
}
void Poll() {
    LONG now = (LONG)GetTickCount(), at = g_pollAt;
    if (now - at < 1000 || InterlockedCompareExchange(&g_pollAt, now, at) != at) return;
    Reload(false);
}

// vrtext2 (bd131, DrGonzaga's bd130 test: nothing changed on screen): the game keeps ONE string object per text
// in its string table and every lookup hands out a shared reference to it, so swapping only the caller's handle
// (bd130) left the table's object, which the HUD reads through other paths, untouched. Now the table's own
// handle is swapped too: language node = map find BD+1037bb5d on table+8 (key = language), node+0x14 = bank,
// bank+0x18/+0x1c = sorted 16-byte entries {id, a, b, ...}, BD+10219c73(bank, a, b) = the table's handle.
// The hook also moved from the wrapper BD+1021a480 to the lookup it calls, BD+1021a34e (5 callers, all paths).
typedef BYTE *(__thiscall *MapFindFn)(void *, DWORD *);
typedef DWORD *(__thiscall *BankGetFn)(void *, DWORD, DWORD);
MapFindFn g_mapFind;
BankGetFn g_bankGet;
const wchar_t *volatile g_orig[kSlots];  // slot -> the game's own text (kept: the table copy gets replaced)
volatile DWORD g_obj[kSlots];            // slot -> string object last put in the table

DWORD *StoreHandle(void *tbl, int id, DWORD lang) {
    if (!g_mapFind || !g_bankGet || !tbl || IsBadReadPtr((BYTE *)tbl + 8, 12)) return nullptr;
    BYTE *map = (BYTE *)tbl + 8;
    DWORD key = lang;
    BYTE *node = g_mapFind(map, &key);
    if (!node || node == map || IsBadReadPtr(node + 0x14, 4)) return nullptr;
    BYTE *bank = *(BYTE **)(node + 0x14);
    if (!bank || IsBadReadPtr(bank + 0x18, 8)) return nullptr;
    DWORD *e = *(DWORD **)(bank + 0x18);
    int lo = 0, hi = *(int *)(bank + 0x1c) - 1;
    if (!e || hi < 0 || hi > 200000 || IsBadReadPtr(e, (hi + 1) * 16)) return nullptr;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1, v = (int)e[mid * 4];
        if (v < id) lo = mid + 1;
        else if (v > id) hi = mid - 1;
        else {
            DWORD *h = g_bankGet(bank, e[mid * 4 + 1], e[mid * 4 + 2]);
            return h && !IsBadWritePtr(h, 4) ? h : nullptr;
        }
    }
    return nullptr;
}
// Put text into the caller's handle and, when it holds the table's object, the table's handle too.
DWORD SetText(void *tbl, int id, DWORD lang, DWORD *out, const wchar_t *text) {
    DWORD was = *out;
    if (g_strRelease && g_strFrom) {
        DWORD *sh = StoreHandle(tbl, id, lang);
        if (sh && *sh == was && was) {
            DWORD tmp = 0;
            g_strFrom(&tmp, text);
            if (tmp) {
                DWORD old = *sh;
                *sh = tmp;
                g_strRelease(&old, &old);  // the table's reference; the caller still holds one
                g_strRelease(out, out);    // the caller's: the old object goes now
                InterlockedIncrement((LONG *)tmp);  // and the caller shares the table's new one (refcount at +0)
                *out = tmp;
                return tmp;
            }
        }
    }
    Replace(out, text);
    return *out;
}
bool Same(BYTE *o, const wchar_t *t) {
    if (!o || IsBadReadPtr(o, 0x10)) return false;
    int n = *(int *)(o + 4), m = (int)wcslen(t);
    return n == m && !IsBadReadPtr(o + 0xc, n * 2) && !memcmp(o + 0xc, t, n * 2);
}

void Check(int id, DWORD *out, DWORD ret, void *tbl, DWORD lang) {
    BYTE *o = (BYTE *)*out;  // out: the caller's string handle (its own stack)
    LONG ep = g_epoch;
    unsigned h = ((unsigned)id * 2654435761u) >> 19;  // 13 bits
    int slot = -1;
    for (int probe = 0; probe < 32; ++probe) {
        int s = (h + probe) & (kSlots - 1);
        LONG cur = g_id[s];
        if (cur == id) {
            LONG st = g_state[s];
            if ((st >> 2) == ep) {
                if ((st & 3) == 2 && o && !IsBadWritePtr(o, 0x10) && *(int *)(o + 4) > 0)  // blocked ids only
                    *(int *)(o + 4) = 0, *(wchar_t *)(o + 0xc) = 0;
                else if ((st & 3) == 3 && g_txt[s] && (DWORD)o != g_obj[s])  // vrtext2: table reloaded, or a copy
                    g_obj[s] = SetText(tbl, id, lang, out, g_txt[s]);
                return;
            }
            slot = s;
            break;  // seen before the switch changed: decide again below
        }
        if (cur == 0) {
            LONG was = InterlockedCompareExchange(&g_id[s], id, 0);
            if (was != 0 && was != id) continue;
            slot = s;
            break;
        }
    }
    // First time (this epoch): look at the text once.
    int n = 0;
    const wchar_t *t = nullptr;
    if (o && !IsBadReadPtr(o, 0x10)) {
        n = *(int *)(o + 4);
        if (n > 0 && n < 4000 && !IsBadReadPtr(o + 0xc, n * 2)) t = (const wchar_t *)(o + 0xc);
        else n = 0;
    }
    // The game's own text: kept the first time, because the table's copy is replaced from then on.
    if (slot >= 0 && !g_orig[slot] && t) g_orig[slot] = Dup(t, n);
    const wchar_t *orig = slot >= 0 && g_orig[slot] ? g_orig[slot] : t;
    int on = orig ? (int)wcslen(orig) : 0;
    bool blank = false;
    // vrtext3 (bd132): a text with its own VR: line in bdvr-prompts.txt is never hidden by the word list
    // (DrGonzaga rewrote the cyber-eye tagging tutorial, which has "zooming" in the game's wording).
    bool own = false;
    if (Table *tb = g_vrText ? g_tab : nullptr)
        for (int k = 0; k < tb->nrep && !own; ++k) own = tb->id[k] == id;
    for (int i = 0; orig && g_on && !own && i < g_nblock && !blank; ++i) blank = Has(orig, on, g_block[i]);
    if (blank) {
        char a[96];
        int k = 0;
        for (; k < on && k < 95; ++k) a[k] = orig[k] < 128 ? (char)orig[k] : '?';
        a[k] = 0;
        HMODULE m = GetModuleHandleW(L"FC3_d3d11.dll");
        Log("prompts: hiding text %d \"%s\" (asked for by %s+%08lx)", id, a, "BD", ret - (DWORD)m + kImageBase);
        if (o && !IsBadWritePtr(o, 0x10)) *(int *)(o + 4) = 0, *(wchar_t *)(o + 0xc) = 0;
    } else if (orig && g_verboseLog) {  // tidy1 (bd79): the bd33 text survey is done; detailed log only
        char a[96];
        int k = 0;
        for (; k < on && k < 95; ++k) a[k] = orig[k] < 128 ? (char)orig[k] : '?';
        a[k] = 0;
        Log("prompts: text %d \"%s\"", id, a);
    }
    const wchar_t *nt = nullptr;  // vrtext1
    DWORD obj = 0;
    if (!blank && orig) {
        if (HasToken(orig, on)) {
            LogUnknownTokens(orig, on);
            Log("prompts: text %d shown (has buttons)", id);  // so DrGonzaga can see which entries the game uses
        }
        if (g_vrText) nt = Rewrite(id, orig, on);
        const wchar_t *want = nt ? nt : orig;  // also puts the game's text back after a change or switch-off
        if (!Same(o, want)) {
            obj = SetText(tbl, id, lang, out, want);
            if (nt) {
                char a[96];
                int k = 0;
                for (; nt[k] && k < 95; ++k) a[k] = nt[k] < 128 ? (char)nt[k] : '?';
                a[k] = 0;
                Log("prompts: text %d now \"%s\"", id, a);
            }
        } else obj = (DWORD)o;
    }
    if (slot >= 0) {
        if (nt) g_txt[slot] = nt, g_obj[slot] = obj;
        InterlockedExchange(&g_state[slot], (ep << 2) | (blank ? 2 : nt ? 3 : 1));
    }
}

// prompts3 (DrGonzaga 13 Nov): text lookup. vrtext2 (bd131): hooked one level down, at BD+1021a34e
// thiscall(table, out string handle, mode, text id, language), ret 0x10; mode 0 = build the text. Its callers
// are the id wrapper BD+1021a480 (816 callers) and four other wrappers. Same string object.
typedef DWORD(__thiscall *BdLookupFn)(void *, DWORD *, int, int, DWORD);
BdLookupFn g_bdOrig;
DWORD __thiscall BdHook(void *tbl, DWORD *out, int mode, int id, DWORD lang) {
    DWORD r = g_bdOrig(tbl, out, mode, id, lang);
    if (mode != 0) return r;
    Poll();  // vrtext1: bdvr-prompts.txt saved?
    if (id != 0 && id != -1 && out && !IsBadReadPtr(out, 4))
        Check(id, out, (DWORD)__builtin_return_address(0), tbl, lang);
    return r;
}
void LoadList(const wchar_t *def) {
    wchar_t ini[MAX_PATH], buf[400];
    IniPath(ini);
    g_on = GetPrivateProfileIntW(L"hud", L"HidePrompts", 1, ini) != 0;
    g_vrText = GetPrivateProfileIntW(L"hud", L"VrPrompts", 1, ini) != 0;  // vrtext1
    wcscpy(g_file, ini);
    if (wchar_t *sl = wcsrchr(g_file, L'\\')) wcscpy(sl + 1, L"bdvr-prompts.txt");
    GetPrivateProfileStringW(L"hud", L"HidePromptText", def, buf, 400, ini);
    for (wchar_t *p = buf; *p && g_nblock < 8;) {
        while (*p == L';' || *p == L' ') ++p;
        int k = 0;
        while (*p && *p != L';' && k < 47) g_block[g_nblock][k++] = Low(*p++);
        while (k > 0 && g_block[g_nblock][k - 1] == L' ') --k;
        g_block[g_nblock][k] = 0;
        if (k) ++g_nblock;
        while (*p && *p != L';') ++p;
    }
}
}  // namespace
void PromptsRefresh() { InterlockedIncrement(&g_epoch); }  // ctl3: bindings changed

void PromptsInstallBd(BYTE *bd) {
    // vrtext2: the lookup itself (push ebp; mov ebp,esp; sub esp,0x208; mov eax,[ebp+0x14]), and the
    // wrapper's call to it (BD+1021a493) as a second check that this is the right function.
    static const BYTE kPro[12] = {0x55, 0x8b, 0xec, 0x81, 0xec, 0x08, 0x02, 0x00, 0x00, 0x8b, 0x45, 0x14};
    static const BYTE kWrap[12] = {0x55, 0x8b, 0xec, 0x8b, 0x55, 0x10, 0x51, 0x8b, 0xc4, 0xff, 0x75, 0x0c};
    BYTE *target = bd + 0x21a34e, *wrap = bd + 0x21a480;
    if (IsBadReadPtr(target, 12) || memcmp(target, kPro, 12) || IsBadReadPtr(wrap, 24) || memcmp(wrap, kWrap, 12) ||
        wrap[0x13] != 0xe8 || wrap + 0x18 + *(int *)(wrap + 0x14) != target) {
        Log("prompts: Blood Dragon text lookup doesn't match; prompt hiding off");
        return;
    }
    LoadList(L"zoom;hold breath;steady");
    {  // vrtext1: the game's string release / build-from-text (checked by their first bytes)
        static const BYTE kRel[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08, 0x8b, 0x06, 0x85, 0xc0, 0x74, 0x15, 0x83, 0xc9, 0xff, 0xf0, 0x0f, 0xc1, 0x08};
        static const BYTE kFrom[] = {0x55, 0x8b, 0xec, 0x33, 0xc0, 0x56, 0x8b, 0xf1, 0x39, 0x45, 0x08, 0x74, 0x09, 0xff, 0x75, 0x08, 0xe8};
        BYTE *rel = bd + 0x56c7, *from = bd + 0x5f33;
        if (!IsBadReadPtr(rel, sizeof kRel) && !memcmp(rel, kRel, sizeof kRel) && !IsBadReadPtr(from, sizeof kFrom) &&
            !memcmp(from, kFrom, sizeof kFrom))
            g_strRelease = (StrReleaseFn)rel, g_strFrom = (StrFromFn)from;
        else Log("prompts: game string calls don't match; VR texts only where they fit the game's own text");
        // vrtext2: the string table's language map find and bank fetch (the lookup's own calls)
        BYTE *find = bd + 0x37bb5d, *get = bd + 0x219c73;
        static const BYTE kFind[] = {0x55, 0x8b, 0xec, 0x8b, 0x51, 0x08, 0x33, 0xc0, 0x56, 0x85, 0xd2};
        static const BYTE kGet[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45, 0x08, 0x56, 0x89, 0x45, 0xfc};
        if (!memcmp(find, kFind, sizeof kFind) && !memcmp(get, kGet, sizeof kGet) && target[0x1b] == 0xe8 &&
            target + 0x20 + *(int *)(target + 0x1c) == find)
            g_mapFind = (MapFindFn)find, g_bankGet = (BankGetFn)get;
        else Log("prompts: game string table calls don't match; only the asking code sees the VR texts");
        Reload(true);
    }
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, target, 9);  // push ebp; mov ebp,esp; sub esp,0x208
    t[9] = 0xe9;
    *(DWORD *)(t + 10) = (DWORD)(target + 9) - (DWORD)(t + 14);
    g_bdOrig = (BdLookupFn)t;
    BYTE patch[9] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    *(DWORD *)(patch + 1) = (DWORD)BdHook - (DWORD)(target + 5);
    PatchBytes(target, patch, 9);
    Log("prompts: Blood Dragon text lookup hooked, %d text(s) to hide%s", g_nblock, g_on ? "" : " (switch off)");
}

bool *PromptsHideOn() { return (bool *)&g_on; }
bool *PromptsVrTextOn() { return (bool *)&g_vrText; }  // vrtext1
void PromptsChanged() { InterlockedIncrement(&g_epoch); }
