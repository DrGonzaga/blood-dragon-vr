// loot1 (1 Nov): skip the looting animation (DrGonzaga). When the player loots
// a body, 0x11107270 either plays the loot animation (walks the player to
// the body and reaches down) or, when the player is moving, loots straight
// away by sending "TryLooting" (the branch at 0x111076d5). The jump at
// 0x111073dd decides; making it always taken loots straight away, as the game
// already does on the move. VR page: "Skip looting animations".
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

// loot2 (1 Nov): Far Cry 3's plants needed their own animation (a stub decided per loot type);
// Blood Dragon has no plants, so the jump is simply made always taken (bd19 below).
static BYTE *g_site, g_patch[6];
bool g_skipLoot = true;  // [gameplay] SkipLootAnimation

static void Write(const BYTE *b) {
    if (g_site) PatchBytes(g_site, b, 6);  // patch1: one step, safe to switch while playing
}

static const BYTE *g_origBytes;
void LootApply() { Write(g_skipLoot ? g_patch : g_origBytes); }

// bd19: Blood Dragon. The same function is BD+10762557; its test is at BD+107628f9: call BD+10760fee;
// test al,al; je BD+10762ac4 (loot straight away, "TryLooting"); cmp byte [ebp-1],0; je BD+10762ac4;
// then the animation. The first je (BD+10762900) becomes a jmp. No plants in Blood Dragon, so no stub.
void LootInstallBd(BYTE *bd) {
    static const BYTE kBd[6] = {0x0f, 0x84, 0xbe, 0x01, 0x00, 0x00};
    static const BYTE kBdSkip[6] = {0xe9, 0xbf, 0x01, 0x00, 0x00, 0x90};
    static const BYTE kBefore[2] = {0x84, 0xc0}, kAfter[4] = {0x80, 0x7d, 0xff, 0x00};
    BYTE *s = bd + 0x762900;
    if ((memcmp(s, kBd, 6) && memcmp(s, kBdSkip, 6)) || memcmp(s - 2, kBefore, 2) || memcmp(s + 6, kAfter, 4)) {
        Log("loot: Blood Dragon's looting code doesn't match; animations left as they are");
        return;
    }
    g_site = s, g_origBytes = kBd;
    memcpy(g_patch, kBdSkip, 6);
    LootApply();
    Log("loot: skip looting animations %s", g_skipLoot ? "ON" : "off");
}
