// turret1 (DrGonzaga 9 Nov): mounted guns (the boat turret in the last story
// mission). Found in FC3_d3d11.dll (Ghidra):
// - Every mounted gun has a CMountedWeaponInputListener (vtable 0x11fcabc0,
//   built by 0x102f0ee0). Its slot 3 (0x112a5e70, thiscall, ret 8) is the
//   per-frame update: it turns the stick (this+0xc / +0x10, set by slot 1,
//   0x112a6270, from the game's input) into the gunner's aim angles.
// - this+8 = the mounted gun's CMountedWeapon component; *(comp+0x10)+0x40/+0x44
//   is the entity id of whoever is using it (-1/-1 = nobody): 0x11370460 looks it
//   up and returns that user's CPawn. An entity's own id is at entity+8/+0xc.
// So: the update running with our player as the user = we're on a mounted gun.
// The mod used to blank the right stick's up/down whenever it thought you were
// in a vehicle (the turret counts as one), so the gun couldn't tilt.
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

namespace {
// push ebp; mov ebp,esp; and esp,-16; sub esp,0x114; cmp byte [ebp+0xc],0

typedef void(__attribute__((thiscall)) * UpdateFn)(BYTE *self, void *ctx, int paused);
UpdateFn g_orig;
volatile DWORD g_tick;       // last update with us as the gunner
BYTE *volatile g_gun;        // that mounted gun's component
volatile LONG g_otherUser;   // updates where someone else is the gunner (diagnostic)
bool g_installed;

void __attribute__((thiscall)) UpdateHook(BYTE *self, void *ctx, int paused) {
    g_orig(self, ctx, paused);
    if (paused & 0xff) return;
    BYTE *mw = self && !IsBadReadPtr(self + 8, 4) ? *(BYTE **)(self + 8) : nullptr;
    BYTE *x = mw && !IsBadReadPtr(mw + 0x10, 4) ? *(BYTE **)(mw + 0x10) : nullptr;
    if (!x || IsBadReadPtr(x + 0x40, 8)) return;
    DWORD lo = *(DWORD *)(x + 0x40), hi = *(DWORD *)(x + 0x44);
    if ((lo & hi) == 0xffffffff) return;  // nobody on it
    BYTE *pl = GunAttachPlayer();
    if (!pl || IsBadReadPtr(pl + 8, 8)) return;
    if (*(DWORD *)(pl + 8) != lo || *(DWORD *)(pl + 0xc) != hi) {
        if (InterlockedIncrement(&g_otherUser) == 1 && g_bloodDragon) {
            static int s_n;  // bd13: the ids, a few times, in case the player's id sits elsewhere in Blood Dragon
            if (s_n++ < 6) Log("turret: a mounted gun used by %08lx:%08lx; the player is %08lx:%08lx", hi, lo, *(DWORD *)(pl + 0xc), *(DWORD *)(pl + 8));
        }
        return;
    }
    g_gun = mw;
    g_tick = GetTickCount();
}
}  // namespace

// Settings ([vr] TurretSteadyView, TurretHeight, TurretLaser).
bool g_turretSteady = true;   // view at the player's place, not the game's swinging turret camera
float g_turretHeight = 0.0f;  // m added to the eye height on a mounted gun
bool g_turretLaser = true;    // aiming beam along the mounted gun
int g_turretAbove = 1;
float g_turretLift = 0.3f;
int g_turretRide = 1;         // turride1: [vr] TurretRideVehicle, the view fixed in the vehicle's frame and turned with it    // turabove2 (DrGonzaga 17 Nov: "make the player higher on the turret"): [vr] TurretAboveLift, m        // turabove1: [vr] TurretSteadyAbove, the view a set height above the gun, no shake
int g_turretDragon = 1;       // turdragon1 (DrGonzaga 20 Nov): [vr] TurretDragonRules, the helicopter's view rules on the final level's armoured dragon
int g_turretAnchor = 1;       // turanchor1 (DrGonzaga 16 Nov): [vr] TurretAnchor, 1 = view anchored to the gun's mount (engine.cpp), 0 = the player's place
bool g_turretMotion = true;   // bd13 (DrGonzaga 12 Nov): the mounted gun follows the right controller ([vr] TurretMotion)

// bd13: Blood Dragon. The same listener class: vtable BD+1158bd00, slot 3 (BD+1158bd0c) =
// BD+10583763 (thiscall(self, ctx, paused), ret 8), slot 1 = BD+10583558 (the stick, to
// self+0xc / +0x10). The user id sits at *(comp+0x10)+0x40/+0x44 as in FC3 (getter BD+1044f62f).
void TurretInstallBd(BYTE *bd) {
    if (g_installed || !bd) return;
    static const BYTE kPro[] = {0x53, 0x8b, 0xdc, 0x51, 0x51, 0x83, 0xe4, 0xf0, 0x83, 0xc4, 0x04, 0x55};
    DWORD *slot = (DWORD *)(bd + 0x158bd0c);
    BYTE *fn = bd + 0x583763;
    if (IsBadReadPtr(slot, 4) || *slot != (DWORD)fn || slot[-2] != (DWORD)(bd + 0x583558) || memcmp(fn, kPro, sizeof kPro)) {
        Log("turret: Blood Dragon's mounted gun code doesn't match; turret mode off");
        return;
    }
    g_orig = (UpdateFn)fn;
    void *hk = (void *)UpdateHook;
    if (!PatchBytes(slot, &hk, 4)) {
        Log("turret: couldn't patch the mounted gun update; turret mode off");
        return;
    }
    g_installed = true;
    Log("turret: mounted gun update hooked (BD+10583763)");
}

// On a mounted gun now (300 ms hold so a paused frame or two doesn't drop out).
bool EngineTurret() { return g_installed && g_tick && GetTickCount() - g_tick < 300; }

// Where the mounted gun's entity is (for the log on getting on), false = unknown.
bool TurretGunPos(float p[3]) {
    BYTE *mw = g_gun;
    if (!mw || IsBadReadPtr(mw + 4, 4)) return false;
    BYTE *c4 = *(BYTE **)(mw + 4);
    BYTE *ent = c4 && !IsBadReadPtr(c4 + 0xc, 4) ? *(BYTE **)(c4 + 0xc) : nullptr;
    if (!ent || IsBadReadPtr(ent + 0x50, 12)) return false;
    memcpy(p, ent + 0x50, 12);
    return true;
}
BYTE *TurretGunEnt() {
    if (!EngineTurret()) return nullptr;
    BYTE *mw = g_gun;
    if (!mw || IsBadReadPtr(mw + 4, 4)) return nullptr;
    BYTE *c4 = *(BYTE **)(mw + 4);
    return c4 && !IsBadReadPtr(c4 + 0xc, 4) ? *(BYTE **)(c4 + 0xc) : nullptr;
}
LONG TurretOtherUsers() { return InterlockedExchange(&g_otherUser, 0); }
