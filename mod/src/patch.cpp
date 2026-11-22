// patch1 (11 Nov, review D1 / S1): the one place that writes into game code
// and tables.
//
// The mod changes a few dozen spots in the game: a call's target, a jump at
// the start of a function, a slot in a table of functions, a handful of bytes.
// Each module did that with its own VirtualProtect + write + VirtualProtect,
// and several used a plain memcpy, which another thread running that code at
// the same moment can see half done. Here every write of up to 8 bytes lands
// in ONE step (a longer one goes in 8-byte steps), the page's protection is
// put back afterwards, and the processor's code cache is told.
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

// lock1 (12 Nov): one patch at a time. Two threads patching in the same page (the
// input hook and the window-size hook both write the game's import table at
// start-up) could interleave: A unlocks, B unlocks (and remembers "unlocked" as
// the state to put back), A writes and locks again, B writes into a locked page.
// That crashed about one launch in five since patch1 (log: "exception ... at
// VERSION.dll" between the two input hook lines; the game had to be started again).
static SRWLOCK g_patchLock = SRWLOCK_INIT;
struct PatchHold {
    PatchHold() { AcquireSRWLockExclusive(&g_patchLock); }
    ~PatchHold() { ReleaseSRWLockExclusive(&g_patchLock); }
};

bool PatchBytes(void *at, const void *bytes, size_t n) {
    PatchHold hold;
    BYTE *p = (BYTE *)at;
    const BYTE *b = (const BYTE *)bytes;
    DWORD old;
    // Unlock exactly what is touched: a last step that is not 4 or 8 bytes reads and writes 8.
    size_t tail = n % 8, span = tail && tail != 4 ? n - tail + 8 : n;
    if (!p || !n || !VirtualProtect(p, span, PAGE_EXECUTE_READWRITE, &old)) return false;
    for (size_t i = 0; i < n; i += 8) {
        size_t m = n - i < 8 ? n - i : 8;
        if (m == 4) {
            LONG v;
            memcpy(&v, b + i, 4);
            InterlockedExchange((LONG *)(p + i), v);
        } else {  // 8 bytes in one step; fewer: the bytes after them are written back as they were
            LONG64 cur, nw;
            do {
                cur = *(volatile LONG64 *)(p + i);
                nw = cur;
                memcpy(&nw, b + i, m);
            } while (InterlockedCompareExchange64((LONG64 *)(p + i), nw, cur) != cur);
        }
    }
    VirtualProtect(p, span, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

// Point the call (E8) or jump (E9) instruction at `site` to `target`. The caller has
// checked what is there.
bool PatchCallTo(BYTE *site, const void *target) {
    LONG rel = (LONG)((DWORD)target - (DWORD)(site + 5));
    return PatchBytes(site + 1, &rel, 4);
}

// Put "jmp hook" at fn, filling up to n bytes (5 to 8) with NOPs.
bool PatchJmp(BYTE *fn, const void *hook, size_t n) {
    BYTE j[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    if (n < 5 || n > 8) return false;
    *(int *)(j + 1) = (int)((BYTE *)hook - (fn + 5));
    return PatchBytes(fn, j, n);
}
