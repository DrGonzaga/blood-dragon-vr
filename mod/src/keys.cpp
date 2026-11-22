// keys1 (11 Nov, review D5): the mod's keyboard keys, read in one place.
//
// Each module asked Windows for its own keys on every Present: 13 calls per
// eye image, about 2,300 a second, each one a trip into the system. Now the
// Present hook calls KeysPoll, which looks at the keys 30 times a second, and
// a module asks KeyHit(key), which is true once per press. A tap that falls
// between two looks is still caught (Windows reports "pressed since the last
// look").
//
// The keys: F9 recentre and Insert debug menu, always. The rest only with fc3vr.ini
// [debug] DebugKeys=1 (debugkeys1, bd126: a stray press in a public release looked like
// a bug): F4 6DoF, F5 stereo, F6 headset field of view, F7 camera hook, F8 tracking,
// F10 screenshots, Numpad + / - world scale, Numpad 9 render-twice stereo. (F11 is the
// game's own "reload level": never use it.)
#include <windows.h>

#include "fc3vr.h"

namespace {
const int kKeys[] = {VK_F9, VK_INSERT};
const int kDebugKeys[] = {VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F10, VK_ADD, VK_SUBTRACT, VK_NUMPAD9};
int g_debugKeys = -1;  // [debug] DebugKeys, read once
volatile LONG g_hit[256];  // when the key went down (tick, never 0), until a module takes it
bool s_down[256];
}  // namespace

// Render thread, every Present.
void KeysPoll() {
    static DWORD s_last;
    DWORD now = GetTickCount();
    if (now - s_last < 30) return;
    s_last = now;
    if (g_debugKeys < 0) {
        wchar_t path[MAX_PATH];
        IniPath(path);
        g_debugKeys = GetPrivateProfileIntW(L"debug", L"DebugKeys", 0, path) ? 1 : 0;
        Log("keys: F9 recentre and Insert menu; the debug keys (F4-F8, F10, Numpad) are %s", g_debugKeys ? "ON ([debug] DebugKeys=1)" : "off ([debug] DebugKeys=0)");
    }
    auto poll = [&](int k) {
        SHORT s = GetAsyncKeyState(k);
        bool down = (s & 0x8000) != 0;
        if ((down || (s & 1)) && !s_down[k]) InterlockedExchange(&g_hit[k], (LONG)(now | 1));
        s_down[k] = down;
    };
    for (int k : kKeys) poll(k);
    if (g_debugKeys > 0)
        for (int k : kDebugKeys) poll(k);
}

// True once for each press of one of the keys above (any thread). A press nobody asked
// about for a second is dropped.
bool KeyHit(int vk) {
    LONG t = g_hit[vk & 255];
    if (!t || InterlockedCompareExchange(&g_hit[vk & 255], 0, t) != t) return false;
    return GetTickCount() - (DWORD)t < 1000;
}
