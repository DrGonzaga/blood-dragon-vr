// ptr2 (11 Nov, review D4 / S3): a write check that does not write.
//
// The mod asks "is this game memory there?" in about 95 places through
// Windows' IsBadReadPtr / IsBadWritePtr / IsBadCodePtr. The read forms only
// read, which is harmless and costs next to nothing, so they stay as they are.
// IsBadWritePtr is the bad one: it reads a byte of each page and WRITES IT
// BACK, not atomically, so on a structure another thread is using it can undo
// that thread's write.
//
// Here a write check is: the plain read check (the memory is there right now),
// then "may these pages be written?" asked of Windows (VirtualQuery), which
// touches nothing. That answer is remembered per thread for a second: a
// region's protection all but never changes, and the read check has already
// shown the memory is there.
//
// ptr1 answered every check with VirtualQuery and remembered it for one clock
// tick only. Measured on DrGonzaga's PC: about 1,000 lookups a second at 11 us
// each, 0.1 ms per displayed frame. Too much, hence this version (measured:
// 0 to 3 lookups in 10 seconds).
// fc3vr.h points the three Windows names at MemBad, so no caller changed.
// [debug] SafePtr=0 goes back to the old calls.
#define FC3VR_MEMCHECK_IMPL
#include <windows.h>

#include "fc3vr.h"

bool g_safePtr = true;

namespace {
struct Region {
    BYTE *lo, *hi;
    bool write;
    DWORD when;
};
thread_local Region tl_r[8];
thread_local int tl_next;
}  // namespace

// True when [p, p+n) is NOT all there to read (or to write, if write).
BOOL MemBad(const void *p, size_t n, bool write) {
    if (!g_safePtr) return write ? IsBadWritePtr((void *)p, n) : IsBadReadPtr(p, n);
    if (IsBadReadPtr(p, n)) return TRUE;  // there now? (a read only)
    if (!write || !n) return FALSE;
    BYTE *a = (BYTE *)p, *end = a + n;
    DWORD now = GetTickCount();
    for (Region &r : tl_r)
        if (r.write && now - r.when < 1000 && a >= r.lo && end <= r.hi) return FALSE;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return TRUE;
        DWORD pr = mi.Protect & 0xff;
        bool w = pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY;
        bool r = w || pr == PAGE_READONLY || pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE;
        if (!r || (write && !w)) return TRUE;
        BYTE *lo = (BYTE *)mi.BaseAddress, *hi = lo + mi.RegionSize;
        if (hi <= a) return TRUE;  // (never expected)
        if ((BYTE *)p >= lo && end <= hi) {  // all in one region: remember it
            Region &c = tl_r[tl_next++ & 7];
            c.lo = lo, c.hi = hi, c.write = w, c.when = now;
        }
        a = hi;
    }
    return FALSE;
}
