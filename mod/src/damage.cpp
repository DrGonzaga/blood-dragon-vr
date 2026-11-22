// dmg1 (DrGonzaga 2 Nov): scale the damage the player takes, without touching
// the damage enemies take (keeps the low time to kill).
// Far Cry 3 (Ghidra): the single-player player's health component is
// CFCXCountersComponentPlayerSP, vtable 0x12008e38. Its damage-stim handler
// 0x110d1a40 (slot +0x16c, only in this vtable and the CFCXCountersComponentPlayer
// base one, so never AI or animals) works out the damage in *dmg, then calls
// slot +0x100 (0x110c6ef0: zero the damage while invulnerable, [this+0x2c4] > 0),
// the damage manager, and sets health through slot +0x110.
// We replace that slot in the player's vtable only: run the game's own function,
// then multiply *dmg. Costs nothing per frame (runs only when hit). Bullets, melee,
// explosions, arrows, animals and falls all come through here; fire's
// burn-over-time doesn't.
//
// dmgbd1 (22 Nov, global review: DrGonzaga asked for the slider in Blood Dragon). Read
// side by side in both DLLs: Blood Dragon's PlayerSP vtable is BD+115c51e0 (its
// slot 1 jumps to the type getter that pushes the "CFCXCountersComponentPlayerSP"
// name; the Player base vtable BD+115c4fc0 has a plain 'ret 8' in the same slot).
// One slot was added before the damage ones, so everything is 4 bytes on: the
// stim handler BD+1076bcdc is slot +0x170 and calls slot +0x104 at BD+1076c357
// with (stim, &dmg) just as FC3's calls +0x100; slot +0x104 = BD+1076e4a8, the same
// function as FC3's 0x110c6ef0 byte for byte except the invulnerable flag at
// [this+0x2d0]. Only the Blood Dragon install is kept: the FC3 one lives in the
// FC3 project's source.
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

float g_damageTaken = 1.0f;  // [gameplay] DamageTaken

typedef void(__fastcall *ModFn)(void *self, void *edx, void *stim, float *dmg);
static ModFn g_orig;
static volatile LONG g_logged;

// thiscall in the game: ecx = self, 2 stack args, ret 8. fastcall with a dummy
// edx has the same layout.
static void __fastcall DamageMod(void *self, void *edx, void *stim, float *dmg) {
    g_orig(self, edx, stim, dmg);
    if (!dmg) return;
    float before = *dmg, s = g_damageTaken;
    if (before > 0 && s != 1.0f) *dmg = before * s;
    if (before > 0 && InterlockedIncrement(&g_logged) <= 12) {
        int type = stim && !IsBadReadPtr((BYTE *)stim + 0x30, 4) ? *(int *)((BYTE *)stim + 0x30) : -1;
        Log("damage: player hit, stim type %d, damage %.2f -> %.2f (x%.2f)", type, before, *dmg, s);
    }
}

void DamageInstallBd(BYTE *bd) {
    DWORD *vt = (DWORD *)(bd + 0x15c51e0);
    BYTE *stim = bd + 0x76bcdc, *mod = bd + 0x76e4a8, *site = bd + 0x76c351;
    // the modifier: push ebp; mov ebp,esp; movss xmm1,[ecx+2d0h]; xorps xmm0,xmm0 ...
    static const BYTE kMod[] = {0x55, 0x8b, 0xec, 0xf3, 0x0f, 0x10, 0x89, 0xd0, 0x02, 0x00, 0x00, 0x0f, 0x57, 0xc0};
    // the handler's call of it: mov eax,[ebx]; push edi; push esi; mov ecx,ebx; call [eax+104h]
    static const BYTE kSite[] = {0x8b, 0x03, 0x57, 0x56, 0x8b, 0xcb, 0xff, 0x90, 0x04, 0x01, 0x00, 0x00};
    bool ok = !IsBadReadPtr(vt, 0x174) && vt[0x170 / 4] == (DWORD)stim && vt[0x104 / 4] == (DWORD)mod &&
              !IsBadReadPtr(mod, sizeof kMod) && !memcmp(mod, kMod, sizeof kMod) &&
              !IsBadReadPtr(site, sizeof kSite) && !memcmp(site, kSite, sizeof kSite);
    if (!ok) {
        Log("damage: Blood Dragon's player health code doesn't match; damage slider off");
        return;
    }
    g_orig = (ModFn)(void *)mod;
    void *hk = (void *)DamageMod;
    if (!PatchBytes(&vt[0x104 / 4], &hk, 4)) return;
    Log("damage: player damage hook in (BD+1076e4a8, x%.2f)", g_damageTaken);
}
