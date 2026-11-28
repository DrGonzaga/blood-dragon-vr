// ubi1 (GitHub #8, DrGonzaga 26 Nov): which build of Blood Dragon's engine (FC3_d3d11.dll) is loaded, and
// where each part of it the mod uses sits in that build.
//
// The mod's addresses were all found in the Steam copy's DLL (built 17 Oct 2019, 28,693,672 bytes,
// md5 6f3bf955...). The Ubisoft Connect copy ships its own DLL (built 7 Dec 2021, 28,687,360 bytes,
// md5 c060144a...): the same code, compiled again, but linked in a different order, so nearly every
// function and global sits somewhere else. Every address the mod uses was matched in it (26 Nov,
// bdvr/ubi1/: each code address by a byte window that is unique in BOTH DLLs, whole function bodies
// compared, data by the code that reads it, vtables slot by slot, the type hashes by their values).
//
// All Blood Dragon addresses in the source stay the Steam ones (logs and notes keep one numbering);
// every place that turns one into a pointer goes through BdAt(), which looks it up for the loaded build.
// An unknown build is treated as Steam, exactly as before: each feature's own byte checks then switch
// it off if the code doesn't match.
#include "fc3vr.h"

namespace {
struct Pair { DWORD steam, ubi; };  // RVAs (address - 0x10000000)
const Pair kUbi[] = {
#include "gamever_ubi.inc"
};
const int kUbiN = sizeof kUbi / sizeof kUbi[0];

int s_build = -1;
BYTE *s_mod;
}  // namespace

static int Detect(BYTE *mod) {
    if (!mod) return kBdBuildUnknown;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(mod + dos->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage, sum = nt->OptionalHeader.CheckSum;
    if (ts == 0x5da83ea7 && size == 0x2282000 && sum == 0x1b6b1de) return kBdBuildSteam;
    if (ts == 0x61af0518 && size == 0x2281000 && sum == 0x1b61a23) return kBdBuildUbi;
    Log("game: unknown FC3_d3d11.dll build (time stamp %08lx, image size %08lx, check sum %08lx): the Steam addresses are tried "
        "and each feature checks its own code", (unsigned long)ts, (unsigned long)size, (unsigned long)sum);
    return kBdBuildUnknown;
}

int BdBuild() {
    if (s_build < 0) {
        BYTE *mod = (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll");
        if (!mod) return kBdBuildUnknown;  // not loaded yet: ask again later
        s_mod = mod;
        s_build = Detect(mod);
        if (s_build == kBdBuildUbi) {
            for (int i = 1; i < kUbiN; ++i)
                if (kUbi[i].steam <= kUbi[i - 1].steam) Log("game: ubi1 address table out of order at %d", i);
        }
        Log("game: Blood Dragon engine build: %s", s_build == kBdBuildSteam ? "Steam (17 Oct 2019)"
                                                   : s_build == kBdBuildUbi ? "Ubisoft Connect (7 Dec 2021): addresses translated"
                                                                            : "unknown");
    }
    return s_build;
}

const char *BdBuildName() {
    int b = BdBuild();
    return b == kBdBuildSteam ? "Steam" : b == kBdBuildUbi ? "Ubisoft Connect" : "unknown";
}

BYTE *BdAt(const void *mod, DWORD steam) {
    DWORD rva = steam >= 0x10000000 ? steam - 0x10000000 : steam;
    if (BdBuild() != kBdBuildUbi) return (BYTE *)mod + rva;
    int lo = 0, hi = kUbiN - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kUbi[mid].steam == rva) return (BYTE *)mod + kUbi[mid].ubi;
        if (kUbi[mid].steam < rva) lo = mid + 1;
        else hi = mid - 1;
    }
    // Not in the table: a place the mod uses that wasn't matched. Hand back a block of zeros, so the
    // feature's own byte checks fail and switch it off instead of patching a wrong spot.
    static BYTE s_none[256];
    static LONG s_said;
    if (s_said < 20) {
        InterlockedIncrement(&s_said);
        Log("game: ubi1: BD+%08lx has no Ubisoft Connect address; that feature stays off", (unsigned long)(rva + 0x10000000));
    }
    return s_none;
}
