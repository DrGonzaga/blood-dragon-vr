// vehgame1 (DrGonzaga 9 Nov: "vehicle detection has been an ongoing challenge...
// get it done properly"): ask the game whether the player is in a vehicle,
// instead of guessing from the seat bone and the camera height (which crouching,
// healing, swimming and pauses kept fooling).
// FC3_d3d11.dll (Ghidra):
// - 0x113770a0 (ecx = a pawn's CPawn component, no stack args, returns eax) is the
//   game's own "vehicle this pawn is in" (0 = none): it reads the pawn's vehicle id
//   and returns that vehicle's component. 154 callers; the enter-vehicle state
//   0x119f7a10 sends "IsInVehicle" when it returns non-zero.
// - The player's CPawn component: entity+0x88 = component array, +0x8c = count;
//   a component's vtable slot 1 (thiscall) returns its type info: count at +4, ids
//   from +8. CPawn's own type (0x102535c0, lazily set up, flag bit 1 of 0x1287eb38)
//   has its id at 0x1287eb50; type info id #3 (+0x14) matches it for a CPawn
//   (0x10c0dad0 checks exactly this). Read-only: no game state is changed.
// Called from the game camera update (engine.cpp AimMatrix, game thread), once a frame.
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

namespace {
int g_state;  // 0 not checked, 1 ready, -1 off
DWORD g_fn;
DWORD *g_typeFlag, *g_typeId;
BYTE *g_ent, *g_pawn;  // cached for this player entity
DWORD g_lookT;
BYTE *volatile g_vehComp;  // turride1: the vehicle component the game last gave
typedef BYTE *(__attribute__((thiscall)) * TypeInfoFn)(BYTE *comp);

BYTE *FindPawn(BYTE *ent) {
    if (!(*g_typeFlag & 1)) return nullptr;
    DWORD id = *g_typeId;
    if (IsBadReadPtr(ent + 0x88, 8)) return nullptr;
    BYTE **arr = *(BYTE ***)(ent + 0x88);
    DWORD n = *(DWORD *)(ent + 0x8c);
    if (!arr || n == 0 || n > 256 || IsBadReadPtr(arr, n * 4)) return nullptr;
    for (DWORD i = 0; i < n; ++i) {
        BYTE *c = arr[i];
        if (!c || IsBadReadPtr(c, 4)) continue;
        DWORD *vt = *(DWORD **)c;
        if (!vt || IsBadReadPtr(vt + 1, 4) || IsBadCodePtr((FARPROC)vt[1])) continue;
        BYTE *ti = ((TypeInfoFn)vt[1])(c);
        if (!ti || IsBadReadPtr(ti, 0x18)) continue;
        if (*(DWORD *)(ti + 4) > 3 && *(DWORD *)(ti + 0x14) == id) return c;
    }
    return nullptr;
}

BYTE *CallGetVehicle(BYTE *pawn) { return (BYTE *)BdCdeclCall((void *)g_fn, 1, pawn); }  // bd8: a plain cdecl(pawn)
}  // namespace

// 1 = the game says the player is in a vehicle, 0 = not, -1 = can't tell (fall back).
int VehGameInVehicle(BYTE *ent) {
    if (g_state == 0) {
        g_state = -1;
        // bd8: Blood Dragon. The same function is 0x103df556 (same name hash fd1ba782, cdecl with the
        // pawn on the stack); CPawn's type is set up by 0x103a02b5: struct 0x11872128 (id at +0x14),
        // flag bit 1 of 0x11872140.
        BYTE *bd = g_bloodDragon && BdEngineOn() && BdStage() >= 7 ? (BYTE *)GetModuleHandleW(L"FC3_d3d11.dll") : nullptr;
        if (bd) {
            static const BYTE kHead[] = {0x55, 0x8b, 0xec, 0x51, 0x51, 0x8b, 0x4d, 0x08, 0x85, 0xc9, 0x75, 0x04, 0x33, 0xc0, 0xc9, 0xc3};
            BYTE *f = BdAt(bd, 0x3df556), *ty = BdAt(bd, 0x3a02b5);
            if (!memcmp(f, kHead, sizeof kHead) && ty[0] == 0xf6 && ty[1] == 0x05 && *(DWORD *)(ty + 2) == (DWORD)BdAt(bd, 0x1872140) &&
                ty[8] == 0xbe && *(DWORD *)(ty + 9) == (DWORD)BdAt(bd, 0x1872128)) {
                g_fn = (DWORD)f, g_typeFlag = (DWORD *)(BdAt(bd, 0x1872140)), g_typeId = (DWORD *)(BdAt(bd, 0x187213c)), g_state = 1;
            }
        }
        Log("vehgame: the game's own vehicle record: %s", g_state > 0 ? "ready" : "not found (game changed?); old test used");
    }
    if (g_state < 0 || !ent) return -1;
    if (ent != g_ent || !g_pawn) {
        DWORD t = GetTickCount();
        if (ent == g_ent && t - g_lookT < 1000) return -1;  // looked recently and found none
        g_ent = ent, g_lookT = t, g_pawn = FindPawn(ent);
        static BYTE *s_logged;
        if (s_logged != ent || g_pawn) s_logged = ent, Log("vehgame: player %p: pawn component %s %p", ent, g_pawn ? "found" : "NOT found", g_pawn);
        if (!g_pawn) return -1;
    }
    // Still this player's component (the entity could have been reloaded in place)?
    BYTE *c4 = !IsBadReadPtr(g_pawn + 4, 4) ? *(BYTE **)(g_pawn + 4) : nullptr;
    if (!c4 || IsBadReadPtr(c4 + 0xc, 4) || *(BYTE **)(c4 + 0xc) != ent) {
        g_pawn = nullptr;
        return -1;
    }
    BYTE *vc = CallGetVehicle(g_pawn);
    g_vehComp = vc;  // turride1
    return vc ? 1 : 0;
}
// turride1: the object of the vehicle the player is in (its matrix at +0x20, place at +0x50), or null.
BYTE *VehGameVehicleEnt() {
    BYTE *vc = g_vehComp;
    if (!vc || IsBadReadPtr(vc + 4, 4)) return nullptr;
    BYTE *c4 = *(BYTE **)(vc + 4);
    if (!c4 || IsBadReadPtr(c4 + 0xc, 4)) return nullptr;
    BYTE *e = *(BYTE **)(c4 + 0xc);
    return e && !IsBadReadPtr(e + 0x20, 64) ? e : nullptr;
}
BYTE *VehGamePawn() { return g_pawn; }  // holster1: game thread, after VehGameInVehicle
void VehGameForget() { g_ent = g_pawn = nullptr; }
