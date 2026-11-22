// movie1 (DrGonzaga 16 Nov: "The 2D cutscenes are still in 'menu' format (not 16:9) and still have scene
// background"). Blood Dragon's story scenes are Bink videos, not engine cinematics, so the cinematic flag and
// the 2D cutscene screen (engine.cpp flat1) never see them: the game draws the video as part of its HUD
// picture, which the mod shows on the HUD panel (nearly square, as it is the eye picture's shape) with the 3D
// scene around it. FC3_d3d11.dll calls binkw32.dll through its import table (BD+114d8920..114d8964); the
// slots for BinkOpen, BinkClose, BinkDoFrame and BinkSetWillLoop are pointed here. A video counts as playing
// while it gets frames (BinkDoFrame in the last 0.3 s) and is not set to loop (menu backgrounds loop).
// hud.cpp then shows the HUD picture on a 16:9 screen in front of you with black around it ([hud] Movie*).
#include <windows.h>
#include <string.h>

#include "fc3vr.h"

namespace {
typedef void *(__stdcall *OpenFn)(const char *, DWORD);
typedef void(__stdcall *CloseFn)(void *);
typedef int(__stdcall *FrameFn)(void *);
typedef void(__stdcall *LoopFn)(void *, int);
OpenFn g_open;
CloseFn g_close;
FrameFn g_frame;
LoopFn g_loop;
struct Clip {
    void *h;
    bool loop;
    DWORD t;  // last frame
    char name[64];
};
Clip g_clips[8];
SRWLOCK g_lock = SRWLOCK_INIT;
volatile DWORD g_lastMovieT;
volatile bool g_playing;

Clip *Find(void *h) {
    for (Clip &c : g_clips)
        if (c.h == h) return &c;
    return nullptr;
}
void *__stdcall Open(const char *name, DWORD flags) {
    void *h = g_open(name, flags);
    const char *b = name;
    if (name && !IsBadStringPtrA(name, 260))
        for (const char *p = name; *p; ++p)
            if (*p == '\\' || *p == '/') b = p + 1;
    DWORD w = 0, ht = 0, fr = 0;
    if (h && !IsBadReadPtr(h, 12)) w = ((DWORD *)h)[0], ht = ((DWORD *)h)[1], fr = ((DWORD *)h)[2];
    const bool named = name && !IsBadStringPtrA(name, 260) && (unsigned char)name[0] >= 0x20 && name[0] != 'B';
    Log("movie: video opened: %s (%lux%lu, %lu frames, flags %08lx)%s", named && b ? b : "(from memory or a file handle)", w, ht, fr, flags, h ? "" : " - failed");
    if (h) {
        AcquireSRWLockExclusive(&g_lock);
        Clip *c = Find(nullptr);
        if (c) {
            c->h = h, c->loop = false, c->t = 0;
            strncpy(c->name, named && b ? b : "video", sizeof c->name - 1), c->name[sizeof c->name - 1] = 0;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return h;
}
void __stdcall Close(void *h) {
    AcquireSRWLockExclusive(&g_lock);
    if (Clip *c = Find(h)) c->h = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
    g_close(h);
}
int __stdcall Frame(void *h) {
    AcquireSRWLockExclusive(&g_lock);
    if (Clip *c = Find(h)) {
        c->t = GetTickCount();
        if (!c->loop) g_lastMovieT = c->t;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_frame(h);
}
void __stdcall Loop(void *h, int on) {
    AcquireSRWLockExclusive(&g_lock);
    if (Clip *c = Find(h)) c->loop = on != 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_loop(h, on);
}
bool Slot(BYTE *bd, DWORD va, const char *name, void *hook, void **orig) {
    void **slot = (void **)(bd + (va - 0x10000000));
    HMODULE bk = GetModuleHandleW(L"binkw32.dll");
    void *want = bk ? (void *)GetProcAddress(bk, name) : nullptr;
    if (IsBadReadPtr(slot, 4) || !want || *slot != want) {
        Log("movie: import %s not where expected; videos stay as they are", name);
        return false;
    }
    *orig = *slot;
    return PatchBytes(slot, &hook, 4);
}
}  // namespace

bool g_movieOn = true;  // [hud] MovieScreen
bool g_movieComic = true;  // movie3: [hud] MovieForComic
static bool g_comicWas;

void MovieInstallBd(BYTE *bd) {
    static bool s_done;
    if (s_done || !bd) return;
    s_done = true;
    bool ok = Slot(bd, 0x114d8964, "_BinkOpen@8", (void *)Open, (void **)&g_open) &&
              Slot(bd, 0x114d893c, "_BinkClose@4", (void *)Close, (void **)&g_close) &&
              Slot(bd, 0x114d8930, "_BinkDoFrame@4", (void *)Frame, (void **)&g_frame) &&
              Slot(bd, 0x114d8954, "_BinkSetWillLoop@8", (void *)Loop, (void **)&g_loop);
    Log("movie: Blood Dragon's video calls %s", ok ? "watched (story videos go on a 16:9 screen)" : "NOT all watched");
}

// A story video is playing now (render thread; also logs the start and end once each).
bool MoviePlaying() {
    bool now = g_movieOn && g_lastMovieT && GetTickCount() - g_lastMovieT < 300;
    // movie3 (DrGonzaga 17 Nov: "very first cutscene still has the 3d background"): the opening story comic is not a
    // video (no BinkOpen); the game is in input mode b090be8c meanwhile (17 Nov logs, before the helicopter), and
    // draws it with its menus. Treated as a story video too: [hud] MovieForComic.
    const bool comic = g_movieOn && g_movieComic && EngineInputMode() == 0xb090be8c;
    if (comic != g_comicWas) g_comicWas = comic, Log("movie: story comic (input mode b090be8c) %s", comic ? "on: the 16:9 screen" : "over");
    now = now || comic;
    if (now != g_playing) {
        g_playing = now;
        char nm[64] = "";
        AcquireSRWLockShared(&g_lock);
        for (Clip &c : g_clips)
            if (c.h && !c.loop && GetTickCount() - c.t < 1000) strcpy(nm, c.name);
        ReleaseSRWLockShared(&g_lock);
        Log("movie: %s %s", now ? "story video on the 16:9 screen:" : "story video over; back to the HUD panel", nm);
    }
    return now;
}
