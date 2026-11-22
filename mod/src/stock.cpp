// stock1 (DrGonzaga 1 Nov): gunstock accuracy. DrGonzaga plays with a stock on the
// controllers: shots taken with the right controller close to the headset
// (stock at the cheek/shoulder) are accurate, shots with the arm out spread,
// on a sliding scale, per gun ([stock.<gun>] in fc3vr.ini).
// The game's spread (degrees, read at 0x119f1864; the cone is up to 1.5x it)
// already includes skill unlocks and its recoil / movement growth. We set the
// spread ourselves from the distance, so unlocks don't change it, and keep the
// game's growth as a ratio: game spread now / its lowest seen for this gun
// (kept apart for aiming down sights and not). Applies with or without the
// left trigger. Player shots only (bonehook SpreadScale).
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fc3vr.h"

bool g_stockOn = true;  // [aim] GunstockAccuracy
static StockGun g_cur = {0.25f, 0.60f, 0.0f, 3.0f};  // near, far (m), best, worst (degrees)
static char g_gun[48];
static float g_rest[2];          // the game's lowest spread seen for this gun: [0] hip, [1] aiming down sights
static volatile float g_lastDist = -1, g_lastSpread = -1, g_lastGame = -1;
// ads1 (DrGonzaga 8 Nov): you can't run while aiming down a scope, so with the gun up
// at your face (within this gun's "accurate within" distance) you move slower.
// Per gun [stock.<gun>] AdsSpeed (x walking speed), default by kind of gun.
static float g_adsSpeed = 0.55f;
static volatile bool g_adsNear;
static float g_adsDrop = 0.15f;      // adsmove1: [gameplay] AdsHeightDrop (m below the eye that still counts; 1 = off)
static volatile float g_adsDy = -1;  // adsmove1: controller height minus head height, for the menu
static float AdsDefault(const char *n) {
    struct K { const char *k; float v; };
    static const K ks[] = {{"R700", .35f}, {"M700", .35f}, {"Z93", .35f}, {"SVD", .35f}, {"D2", .35f}, {"Sniper", .35f},
                           {"Bushman", .35f}, {"MKG", .4f}, {"PKM", .4f}, {"U100", .4f}, {"M249", .4f}, {"LMG", .4f},
                           {"RPG", .45f}, {"GL94", .45f}, {"M79", .45f}, {"Launcher", .45f}, {"Flame", .45f},
                           {"870", .6f}, {"SPAS", .6f}, {"1887", .6f}, {"M133", .6f}, {"Shotgun", .6f},
                           {"MP5", .65f}, {"MP7", .65f}, {"A2000", .65f}, {"Vector", .65f}, {"Shredder", .65f},
                           {"Skorpion", .65f}, {"UMP", .65f}, {"1911", .75f}, {"6P9", .75f}, {"D50", .75f},
                           {"Desert", .75f}, {"P226", .75f}, {"Pistol", .75f}, {"Shark", .75f}, {"Auto9", .75f},
                           {"BOW", .55f}, {"Bow", .55f}};
    for (const K &k : ks)
        if (strstr(n, k.k)) return k.v;
    return 0.55f;  // assault rifles and the rest
}

static void Ini(bool save) {
    if (!g_gun[0]) return;
    wchar_t path[MAX_PATH], sec[80], buf[32];
    IniPath(path);
    swprintf(sec, 80, L"stock.%hs", g_gun);
    float *v[4] = {&g_cur.nearM, &g_cur.farM, &g_cur.best, &g_cur.worst};
    const wchar_t *k[4] = {L"AccurateWithin", L"WorstAt", L"SpreadAtBest", L"SpreadAtWorst"};
    if (save) {
        for (int i = 0; i < 4; ++i) swprintf(buf, 32, L"%.3f", *v[i]), WritePrivateProfileStringW(sec, k[i], buf, path);
        swprintf(buf, 32, L"%.3f", g_adsSpeed), WritePrivateProfileStringW(sec, L"AdsSpeed", buf, path);
        return;
    }
    g_adsSpeed = GetPrivateProfileStringW(sec, L"AdsSpeed", L"", buf, 32, path) ? (float)_wtof(buf) : AdsDefault(g_gun);
    if (!GetPrivateProfileStringW(sec, k[0], L"", buf, 32, path)) {
        Ini(true);  // a new gun starts from the settings in use
        Log("stock: %s has no gunstock settings yet; starting from the current ones", g_gun);
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (GetPrivateProfileStringW(sec, k[i], L"", buf, 32, path)) *v[i] = (float)_wtof(buf);
    Log("stock: %s: accurate within %.2f m, worst at %.2f m, spread %.2f to %.2f deg", g_gun, g_cur.nearM, g_cur.farM,
        g_cur.best, g_cur.worst);
}

static void CheckGun() {
    const char *n = GunAttachName();
    if (!n || !n[0] || !strcmp(n, g_gun)) return;
    snprintf(g_gun, sizeof g_gun, "%s", n);
    g_rest[0] = g_rest[1] = 0;
    Ini(false);
}

static float HandDistance(float *dy = nullptr) {
    float m[16], hand[3], head[3];
    if (!HeadtrackHand(1, m, hand) || !EngineHeadPose(m, head)) return -1;
    float d[3] = {hand[0] - head[0], hand[1] - head[1], hand[2] - head[2]};
    if (dy) *dy = d[1];  // XR space, y up
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Our spread for a player shot; game = the game's spread for it, ads = left trigger held.
float StockSpread(float game, bool ads) {
    CheckGun();
    float dist = HandDistance();
    if (dist < 0 || !(game >= 0)) return game;
    float &rest = g_rest[ads ? 1 : 0];
    if (game > 1e-4f && (rest <= 0 || game < rest)) rest = game;
    float grow = rest > 1e-4f ? game / rest : 1.0f;  // the game's recoil / movement growth
    if (grow < 1) grow = 1;
    if (grow > 6) grow = 6;
    float span = g_cur.farM - g_cur.nearM;
    float t = span > 0.01f ? (dist - g_cur.nearM) / span : (dist > g_cur.nearM ? 1.0f : 0.0f);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    // twosmooth1 (DrGonzaga 10 Nov): with two-handed mode on, the grips replace the distance
    // to the head: both grips holding (two-handed, or drawing the bow) = this gun's best
    // spread, right grip only = its worst.
    if (*TwoHandOn()) t = TwoHandHeld() || TwoHandSteady() || BowActive() ? 0.0f : 1.0f;  // steady1: pistol hands together
    float s = (g_cur.best + (g_cur.worst - g_cur.best) * t) * grow;
    g_lastDist = dist, g_lastSpread = s, g_lastGame = game;
    return s;
}

StockGun *StockSettings() { return &g_cur; }
float *StockAdsSpeed() { return &g_adsSpeed; }
// Render thread, once a frame: is the gun up at the face? (input.cpp reads the answer.)
void StockTick() {
    CheckGun();
    float dy = -1, d = HandDistance(&dy);
    bool gun = GetTickCount() - GunAttachHandTick() < 400;
    // adsmove1 (DrGonzaga 9 Nov: slowed too early): the gun must also be up at eye height,
    // not just close to the head (a gun held low at the chest is close too). The
    // controller sits a little below the eye when sighting, so up to g_adsDrop below
    // counts. Small margins once on, so it doesn't flicker at the edge.
    g_adsDy = dy;
    bool was = g_adsNear;
    float mn = was ? 0.05f : 0.02f, mh = was ? 0.03f : 0;
    g_adsNear = gun && d >= 0 && d <= g_cur.nearM + mn && (g_adsDrop >= 0.99f || dy >= -(g_adsDrop + mh));
}
float *StockAdsDrop() { return &g_adsDrop; }
float StockAdsHeight() { return g_adsDy; }
bool StockGunAtFace() { return g_adsNear; }
const char *StockGunName() { return g_gun; }
void StockSave() { Ini(true); }
float StockLiveDistance() {
    float d = HandDistance();
    return d;
}
void StockLast(float *dist, float *spread, float *game) { *dist = g_lastDist, *spread = g_lastSpread, *game = g_lastGame; }

// kick2: where the gun is between "accurate within" (0) and "worst at" (1) for
// this gun, for the hip recoil (gunattach.cpp). 1 when the distance is unknown.
float StockKickT() {
    CheckGun();
    if (*TwoHandOn()) return TwoHandHeld() || TwoHandSteady() || BowActive() ? 0.0f : 1.0f;  // twokick1: the grips, like the spread
    float dist = HandDistance();
    if (dist < 0) return 1;
    float span = g_cur.farM - g_cur.nearM;
    float t = span > 0.01f ? (dist - g_cur.nearM) / span : (dist > g_cur.nearM ? 1.0f : 0.0f);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}
