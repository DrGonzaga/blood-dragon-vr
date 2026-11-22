// settings1 (11 Nov, review Batch C): fc3vr.ini kept in memory.
//
// Windows' GetPrivateProfile* / WritePrivateProfileString open and parse the
// whole file on every call (about 3 ms per write, measured 3 Nov). The per-gun
// settings did that on the game and render threads at every weapon change
// (30+ calls, and 25 writes for a gun seen for the first time): a hitch.
//
// Now the file is read once, on first use, into a small table. Reads come from
// the table. A write changes the table at once and is queued for one
// background thread, which writes it to the file with the same Windows call as
// before, so the file keeps its comments, order and format. fc3vr.h points the
// three Windows names at the functions here, so no other file changed.
//
// Edits made to fc3vr.ini by hand while the game runs are not seen until the
// next start. [debug] IniCache=0 goes back to reading and writing directly.
#define FC3VR_SETTINGS_IMPL
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#ifndef SETTINGS_TEST
#include "fc3vr.h"
#endif

namespace {

// Small growable array and text copies on the process heap (no C++ library
// containers: they add a quarter of a megabyte to the DLL).
void *Grow(void *p, size_t bytes) {
    HANDLE h = GetProcessHeap();
    return p ? HeapReAlloc(h, 0, p, bytes) : HeapAlloc(h, 0, bytes);
}
void Free(void *p) {
    if (p) HeapFree(GetProcessHeap(), 0, p);
}
wchar_t *Dup(const wchar_t *s, size_t n) {
    wchar_t *d = (wchar_t *)Grow(nullptr, (n + 1) * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t)), d[n] = 0;
    return d;
}
template <class T> struct Arr {
    T *v;
    size_t n, cap;
    T *Add() {  // one more at the end (not filled in); null if out of memory
        if (n == cap) {
            size_t c = cap ? cap * 2 : 16;
            T *nv = (T *)Grow(v, c * sizeof(T));
            if (!nv) return nullptr;
            v = nv, cap = c;
        }
        return &v[n++];
    }
    void Remove(size_t i) { memmove(v + i, v + i + 1, (--n - i) * sizeof(T)); }
};

struct Entry {
    wchar_t *key, *val;
};
struct Section {
    wchar_t *name;
    Arr<Entry> e;
};
struct Op {
    wchar_t *sec, *key, *val;  // key / val null = delete, as in the Windows call
};

Arr<Section> g_tab;
Arr<Op> g_queue;
SRWLOCK g_tabLock = SRWLOCK_INIT;    // the table
SRWLOCK g_queueLock = SRWLOCK_INIT;  // the queue of writes
SRWLOCK g_drainLock = SRWLOCK_INIT;  // one writer to the file at a time, in order
volatile LONG g_state;               // 0 not read yet, 1 table in use, 2 switched off (direct)
HANDLE g_wake;
wchar_t g_path[MAX_PATH];
const size_t kQueueMax = 2048;

bool Same(const wchar_t *a, const wchar_t *b) {
    for (; *a && *b; ++a, ++b)
        if (towlower(*a) != towlower(*b)) return false;
    return !*a && !*b;
}

// What Windows gives back for a value: blanks off both ends, one pair of quotes off.
wchar_t *Clean(const wchar_t *s, size_t n) {
    while (n && (s[0] == L' ' || s[0] == L'\t')) ++s, --n;
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r')) --n;
    if (n >= 2 && (s[0] == L'"' || s[0] == L'\'') && s[n - 1] == s[0]) ++s, n -= 2;
    return Dup(s, n);
}

void FreeSection(Section *s) {
    for (size_t i = 0; i < s->e.n; ++i) Free(s->e.v[i].key), Free(s->e.v[i].val);
    Free(s->e.v), Free(s->name);
}

Section *AddSection(const wchar_t *name, size_t n) {
    wchar_t *d = Dup(name, n);
    Section *s = d ? g_tab.Add() : nullptr;
    if (!s) return Free(d), nullptr;
    s->name = d, s->e = Arr<Entry>{};
    return s;
}
void AddEntry(Section *s, wchar_t *key, wchar_t *val) {  // takes the two copies over
    Entry *e = key && val && key[0] ? s->e.Add() : nullptr;
    if (!e) return Free(key), Free(val);
    e->key = key, e->val = val;
}

void Parse(const wchar_t *t, size_t len) {
    Section *cur = nullptr;
    for (size_t i = 0; i < len;) {
        size_t eol = i;
        while (eol < len && t[eol] != L'\n') ++eol;
        const wchar_t *l = t + i;
        size_t n = eol - i;
        i = eol + 1;
        while (n && (l[0] == L' ' || l[0] == L'\t')) ++l, --n;
        while (n && (l[n - 1] == L'\r' || l[n - 1] == L' ' || l[n - 1] == L'\t')) --n;
        if (!n || l[0] == L';') continue;
        if (l[0] == L'[') {
            size_t close = 1;
            while (close < n && l[close] != L']') ++close;
            size_t at = g_tab.n;
            cur = AddSection(l + 1, close - 1) ? &g_tab.v[at] : nullptr;
            continue;
        }
        if (!cur) continue;
        size_t eq = 0;
        while (eq < n && l[eq] != L'=') ++eq;
        if (eq == n) continue;  // no '=': not a setting
        AddEntry(cur, Clean(l, eq), Clean(l + eq + 1, n - eq - 1));
    }
}

Section *FindSec(const wchar_t *sec) {
    for (size_t i = 0; i < g_tab.n; ++i)
        if (Same(g_tab.v[i].name, sec)) return &g_tab.v[i];
    return nullptr;
}
Entry *FindKey(Section *s, const wchar_t *key) {
    if (s)
        for (size_t i = 0; i < s->e.n; ++i)
            if (Same(s->e.v[i].key, key)) return &s->e.v[i];
    return nullptr;
}

void TableSet(const wchar_t *sec, const wchar_t *key, const wchar_t *val) {
    Section *s = FindSec(sec);
    if (!key) {  // whole section goes
        if (s) FreeSection(s), g_tab.Remove(s - g_tab.v);
        return;
    }
    Entry *e = FindKey(s, key);
    if (!val) {  // one key goes
        if (e) Free(e->key), Free(e->val), s->e.Remove(e - s->e.v);
        return;
    }
    wchar_t *v = Clean(val, wcslen(val));
    if (!v) return;
    if (e) {
        Free(e->val), e->val = v;
        return;
    }
    if (!s) s = AddSection(sec, wcslen(sec));
    if (!s) return Free(v);
    AddEntry(s, Clean(key, wcslen(key)), v);
}

UINT ToInt(const wchar_t *p) {
    while (*p == L' ' || *p == L'\t') ++p;
    bool neg = *p == L'-';
    if (*p == L'-' || *p == L'+') ++p;
    UINT n = 0;
    while (*p >= L'0' && *p <= L'9') n = n * 10 + (UINT)(*p++ - L'0');
    return neg ? (UINT)(0 - n) : n;
}

DWORD CopyOut(const wchar_t *src, size_t n, wchar_t *out, DWORD size) {
    if (!out || !size) return 0;
    if (n > size - 1) n = size - 1;
    memcpy(out, src, n * sizeof(wchar_t));
    out[n] = 0;
    return (DWORD)n;
}

#ifndef SETTINGS_TEST
// Take the queued writes to the file, oldest first. Called by the writer
// thread and by IniFlush; g_drainLock keeps them in order.
void Drain() {
    for (;;) {
        AcquireSRWLockExclusive(&g_queueLock);
        bool any = g_queue.n != 0;
        Op op = {};
        if (any) op = g_queue.v[0], g_queue.Remove(0);
        ReleaseSRWLockExclusive(&g_queueLock);
        if (!any) return;
        WritePrivateProfileStringW(op.sec, op.key, op.val, g_path);
        Free(op.sec), Free(op.key), Free(op.val);
    }
}

DWORD WINAPI Writer(void *) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Sleep(150);  // let a burst (a new gun's defaults, a dragged slider) gather
        AcquireSRWLockExclusive(&g_drainLock);
        Drain();
        ReleaseSRWLockExclusive(&g_drainLock);
    }
}

void Load() {
    IniPath(g_path);
    if (!GetPrivateProfileIntW(L"debug", L"IniCache", 1, g_path)) {
        Log("settings: [debug] IniCache=0: fc3vr.ini read and written directly, as before");
        g_state = 2;
        return;
    }
    HANDLE f = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    char *raw = nullptr;
    DWORD size = 0, got = 0;
    if (f != INVALID_HANDLE_VALUE) {
        size = GetFileSize(f, nullptr);
        if (size != INVALID_FILE_SIZE && size && size < (16u << 20)) raw = (char *)Grow(nullptr, size);
        if (raw && (!ReadFile(f, raw, size, &got, nullptr) || got != size)) Free(raw), raw = nullptr;
        CloseHandle(f);
    }
    if (raw && size >= 2 && (BYTE)raw[0] == 0xff && (BYTE)raw[1] == 0xfe) {  // UTF-16 file
        Parse((const wchar_t *)(raw + 2), (size - 2) / 2);
    } else if (raw) {
        int n = MultiByteToWideChar(CP_ACP, 0, raw, (int)size, nullptr, 0);
        wchar_t *w = n > 0 ? (wchar_t *)Grow(nullptr, n * sizeof(wchar_t)) : nullptr;
        if (w) MultiByteToWideChar(CP_ACP, 0, raw, (int)size, w, n), Parse(w, n);
        Free(w);
    }
    Free(raw);
    size_t keys = 0;
    for (size_t i = 0; i < g_tab.n; ++i) keys += g_tab.v[i].e.n;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = g_wake ? CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr) : nullptr;
    if (!t) {  // no writer: stay with the old direct way
        Log("settings: could not start the settings writer; fc3vr.ini read and written directly");
        g_state = 2;  // (the table just stays unused)
        return;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    Log("settings: fc3vr.ini read once into memory (%u sections, %u settings); changes are written in the background",
        (unsigned)g_tab.n, (unsigned)keys);
    g_state = 1;
}

// True when the table is in use.
bool Ready() {
    if (g_state) return g_state == 1;
    AcquireSRWLockExclusive(&g_tabLock);
    if (!g_state) Load();
    ReleaseSRWLockExclusive(&g_tabLock);
    return g_state == 1;
}
#endif

}  // namespace

#ifndef SETTINGS_TEST

UINT IniGetInt(LPCWSTR sec, LPCWSTR key, INT def, LPCWSTR path) {
    if (!Ready() || !sec || !key) return GetPrivateProfileIntW(sec, key, def, path);
    AcquireSRWLockShared(&g_tabLock);
    Entry *e = FindKey(FindSec(sec), key);
    UINT v = e ? ToInt(e->val) : (UINT)def;
    ReleaseSRWLockShared(&g_tabLock);
    return v;
}

DWORD IniGetStr(LPCWSTR sec, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR path) {
    if (!Ready() || !sec) return GetPrivateProfileStringW(sec, key, def, out, size, path);
    if (!out || !size) return 0;
    DWORD n;
    AcquireSRWLockShared(&g_tabLock);
    Section *s = FindSec(sec);
    if (!key) {  // the section's key names, each ended by 0, the list by another 0; cut if it doesn't fit, like Windows
        DWORD at = 0;
        for (size_t i = 0; s && i < s->e.n; ++i) {
            DWORD kl = (DWORD)wcslen(s->e.v[i].key);
            if (at + kl + 2 > size) {  // no room for this name, its 0 and the last 0
                DWORD room = size >= 2 ? size - 2 : 0;
                if (room > at) memcpy(out + at, s->e.v[i].key, (room - at) * sizeof(wchar_t)), at = room;
                out[size - 1] = 0;
                if (size >= 2) out[size - 2] = 0;
                ReleaseSRWLockShared(&g_tabLock);
                return room;
            }
            memcpy(out + at, s->e.v[i].key, (kl + 1) * sizeof(wchar_t));
            at += kl + 1;
        }
        out[at] = 0;
        n = at ? at - 1 : 0;
    } else {
        Entry *e = FindKey(s, key);
        if (e) n = CopyOut(e->val, wcslen(e->val), out, size);
        else n = CopyOut(def ? def : L"", def ? wcslen(def) : 0, out, size);
    }
    ReleaseSRWLockShared(&g_tabLock);
    return n;
}

BOOL IniSet(LPCWSTR sec, LPCWSTR key, LPCWSTR val, LPCWSTR path) {
    if (!Ready() || !sec) return WritePrivateProfileStringW(sec, key, val, path);
    AcquireSRWLockExclusive(&g_tabLock);
    TableSet(sec, key, val);
    ReleaseSRWLockExclusive(&g_tabLock);

    wchar_t *qs = Dup(sec, wcslen(sec)), *qk = key ? Dup(key, wcslen(key)) : nullptr, *qv = val ? Dup(val, wcslen(val)) : nullptr;
    AcquireSRWLockExclusive(&g_queueLock);
    bool merged = false;
    if (key && val && qv)  // the same setting still waiting (a dragged slider): just replace its value
        for (size_t i = g_queue.n; i-- > 0;) {
            Op &o = g_queue.v[i];
            if (!Same(o.sec, sec)) continue;
            if (o.key && o.val && Same(o.key, key)) Free(o.val), o.val = qv, qv = nullptr, merged = true;
            if (merged || !o.key || Same(o.key, key)) break;  // a delete in between: keep the order
        }
    bool ok = merged;
    if (!merged && qs && (qk || !key) && (qv || !val)) {
        Op *o = g_queue.Add();
        if (o) o->sec = qs, o->key = qk, o->val = qv, qs = qk = qv = nullptr, ok = true;
    }
    bool full = g_queue.n > kQueueMax;
    ReleaseSRWLockExclusive(&g_queueLock);
    Free(qs), Free(qk), Free(qv);  // whatever the queue didn't take
    if (!ok) return WritePrivateProfileStringW(sec, key, val, g_path);  // out of memory: write it here
    if (full) IniFlush();  // never expected; keeps the queue bounded
    else SetEvent(g_wake);
    return TRUE;
}

// Write anything still waiting, on the calling thread (used when the game quits).
void IniFlush() {
    if (g_state != 1) return;
    AcquireSRWLockExclusive(&g_drainLock);
    Drain();
    ReleaseSRWLockExclusive(&g_drainLock);
}

#endif
