// outpost1 (8 Nov): reset outposts any time (DrGonzaga, modelled on a Nexus mod
// for the old 1.05 game, which NOPs the same checks). The game's own options
// menu handler for "OnResetOutpost" (0x110662d0) only resets when BOTH every
// outpost is liberated (0x110665d8: [obj+0x130] == count from 0x11860360,
// bl) AND the story is finished (0x1106662f: progress record +0x34 == 3, al):
//   0x1106663b test bl,bl / je 0x11066758 / test al,al / je 0x11066758
// Turning both je into NOPs lets the game's own confirmation + reset run
// whenever the menu offers it. Null checks before it are untouched. Bytes are
// all relative, so the relocated DLL compares fine. Gameplay tab checkbox.
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

// outpost2 (DrGonzaga 16 Nov 2026: "we had a 'reset outposts anytime' mod from Far Cry 3, can we add this to Blood
// Dragon?"). Blood Dragon's handler for the same menu item ("OnResetOutpost", registered at BD+108205fb with
// handler BD+1081bcd9) has the same two checks: byte [ebp-2] = every outpost liberated ([0x11898c50]+0x134 vs
// 0x107efe97), al = progress record +0x34 == 3 (story finished). At BD+1081bd7a:
//   cmp [ebp-2],bl / je 1081bd87 (the "not yet" message) / cmp al,bl / jne 1081be8b (confirmation + reset)
// The first five bytes become jmp 1081be8b, so the confirmation and reset always run; the null checks before
// it (no outpost list or no progress record: the message) are untouched.
static const DWORD kSiteBd = 0x1081bd7a;
static const BYTE kOrigBd[5] = {0x38, 0x5d, 0xfe, 0x74, 0x08};
static const BYTE kPatchBd[5] = {0xe9, 0x0c, 0x01, 0x00, 0x00};
static const BYTE kRestBd[8] = {0x3a, 0xc3, 0x0f, 0x85, 0x04, 0x01, 0x00, 0x00};  // cmp al,bl / jne 1081be8b (left alone, checked)
static const DWORD kImageBase = 0x10000000;
static BYTE *g_site;
bool g_outpostAnytime = true;  // [gameplay] OutpostResetAnytime

bool OutpostAvailable() { return g_site != nullptr; }

void OutpostApply() {
    if (g_site) PatchBytes(g_site, g_outpostAnytime ? kPatchBd : kOrigBd, 5);  // patch1: one step
}

void OutpostInstallBd(BYTE *bd) {
    if (!bd) return;
    BYTE *s = bd + (kSiteBd - kImageBase);
    if ((memcmp(s, kOrigBd, 5) && memcmp(s, kPatchBd, 5)) || memcmp(s + 5, kRestBd, 8)) {
        Log("outpost: Blood Dragon's reset-outposts code doesn't match; left as it is");
        return;
    }
    g_site = s;
    OutpostApply();
    Log("outpost: reset outposts any time %s (Blood Dragon, BD+1081bd7a)", g_outpostAnytime ? "ON" : "off");
}
