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

void Check(int id, DWORD *out, DWORD ret) {
    BYTE *o = (BYTE *)*out;  // out: the caller's string handle (its own stack)
    LONG ep = g_epoch;
    unsigned h = ((unsigned)id * 2654435761u) >> 19;  // 13 bits
    for (int probe = 0; probe < 32; ++probe) {
        int s = (h + probe) & (kSlots - 1);
        LONG cur = g_id[s];
        if (cur == id) {
            LONG st = g_state[s];
            if ((st >> 2) == ep) {
                if ((st & 3) == 2 && o && !IsBadWritePtr(o, 0x10) && *(int *)(o + 4) > 0)  // blocked ids only
                    *(int *)(o + 4) = 0, *(wchar_t *)(o + 0xc) = 0;
                return;
            }
            break;  // seen before the switch changed: decide again below
        }
        if (cur == 0) {
            LONG was = InterlockedCompareExchange(&g_id[s], id, 0);
            if (was != 0 && was != id) continue;
            break;
        }
    }
    // First time (this epoch): look at the text once.
    int n = 0;
    const wchar_t *t = nullptr;
    if (o && !IsBadReadPtr(o, 0x10)) {
        n = *(int *)(o + 4);
        if (n > 0 && n < 2000 && !IsBadReadPtr(o + 0xc, n * 2)) t = (const wchar_t *)(o + 0xc);
        else n = 0;
    }
    bool blank = false;
    for (int i = 0; t && g_on && i < g_nblock && !blank; ++i) blank = Has(t, n, g_block[i]);
    if (blank) {
        char a[96];
        int k = 0;
        for (; k < n && k < 95; ++k) a[k] = t[k] < 128 ? (char)t[k] : '?';
        a[k] = 0;
        HMODULE m = GetModuleHandleW(L"FC3_d3d11.dll");
        Log("prompts: hiding text %d \"%s\" (asked for by %s+%08lx)", id, a, "BD", ret - (DWORD)m + kImageBase);
        if (!IsBadWritePtr(o, 0x10)) *(int *)(o + 4) = 0, *(wchar_t *)(o + 0xc) = 0;
    } else if (t && g_verboseLog) {  // tidy1 (bd79): the bd33 text survey is done; detailed log only
        char a[96];
        int k = 0;
        for (; k < n && k < 95; ++k) a[k] = t[k] < 128 ? (char)t[k] : '?';
        a[k] = 0;
        Log("prompts: text %d \"%s\"", id, a);
    }
    for (int probe = 0; probe < 32; ++probe) {
        int s = (h + probe) & (kSlots - 1);
        if (g_id[s] == id) { InterlockedExchange(&g_state[s], (ep << 2) | (blank ? 2 : 1)); return; }
    }
}

// prompts3 (DrGonzaga 13 Nov, Blood Dragon: "LS to zoom" while scoped): Blood Dragon's lookup is the wrapper
// BD+1021a480, thiscall(table, out string handle, text id, language), ret 0xc, 816 callers; it calls
// BD+1021a34e with mode 0. Same string object.
typedef DWORD(__thiscall *BdLookupFn)(void *, DWORD *, int, DWORD);
BdLookupFn g_bdOrig;
DWORD __thiscall BdHook(void *tbl, DWORD *out, int id, DWORD lang) {
    DWORD r = g_bdOrig(tbl, out, id, lang);
    if (id != 0 && id != -1 && out && !IsBadReadPtr(out, 4)) Check(id, out, (DWORD)__builtin_return_address(0));
    return r;
}
void LoadList(const wchar_t *def) {
    wchar_t ini[MAX_PATH], buf[400];
    IniPath(ini);
    g_on = GetPrivateProfileIntW(L"hud", L"HidePrompts", 1, ini) != 0;
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

void PromptsInstallBd(BYTE *bd) {
    static const BYTE kPro[12] = {0x55, 0x8b, 0xec, 0x8b, 0x55, 0x10, 0x51, 0x8b, 0xc4, 0xff, 0x75, 0x0c};
    BYTE *target = bd + 0x21a480;
    if (IsBadReadPtr(target, 12) || memcmp(target, kPro, 12)) {
        Log("prompts: Blood Dragon text lookup doesn't match; prompt hiding off");
        return;
    }
    LoadList(L"zoom;hold breath;steady");
    BYTE *t = (BYTE *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, target, 6);  // push ebp; mov ebp,esp; mov edx,[ebp+0x10]
    t[6] = 0xe9;
    *(DWORD *)(t + 7) = (DWORD)(target + 6) - (DWORD)(t + 11);
    g_bdOrig = (BdLookupFn)t;
    BYTE patch[6] = {0xe9, 0, 0, 0, 0, 0x90};
    *(DWORD *)(patch + 1) = (DWORD)BdHook - (DWORD)(target + 5);
    PatchBytes(target, patch, 6);
    Log("prompts: Blood Dragon text lookup hooked, %d text(s) to hide%s", g_nblock, g_on ? "" : " (switch off)");
}

bool *PromptsHideOn() { return (bool *)&g_on; }
void PromptsChanged() { InterlockedIncrement(&g_epoch); }
