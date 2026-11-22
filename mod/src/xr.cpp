// Phase 2: show the game's finished frame in the headset as a flat,
// world-locked screen. Talks to the active OpenXR runtime (VDXR) directly,
// without the Khronos loader, and does all XR work on the game's render
// thread from inside our Present hook.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "fc3vr.h"

// Screen placement, metres: straight ahead of where your head was at start.
static const float kScreenDistance = 2.5f;
static const float kScreenWidth = 3.2f;

#define XR_FUNCS(X)                       \
    X(xrCreateInstance)                   \
    X(xrDestroyInstance)                  \
    X(xrGetInstanceProperties)            \
    X(xrGetSystem)                        \
    X(xrEnumerateViewConfigurationViews)  \
    X(xrGetD3D11GraphicsRequirementsKHR)  \
    X(xrCreateSession)                    \
    X(xrDestroySession)                   \
    X(xrBeginSession)                     \
    X(xrEndSession)                       \
    X(xrPollEvent)                        \
    X(xrCreateReferenceSpace)             \
    X(xrEnumerateSwapchainFormats)        \
    X(xrCreateSwapchain)                  \
    X(xrDestroySwapchain)                 \
    X(xrEnumerateSwapchainImages)         \
    X(xrAcquireSwapchainImage)            \
    X(xrWaitSwapchainImage)               \
    X(xrReleaseSwapchainImage)            \
    X(xrWaitFrame)                        \
    X(xrBeginFrame)                       \
    X(xrEndFrame)                         \
    X(xrLocateSpace)                      \
    X(xrLocateViews)                      \
    X(xrStringToPath)                     \
    X(xrCreateActionSet)                  \
    X(xrCreateAction)                     \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets)          \
    X(xrSyncActions)                      \
    X(xrGetActionStateBoolean)            \
    X(xrGetActionStateFloat)              \
    X(xrGetActionStateVector2f)           \
    X(xrCreateActionSpace)                \
    X(xrApplyHapticFeedback)              \
    X(xrStopHapticFeedback)

static PFN_xrGetInstanceProcAddr g_gipa;
#define DECLARE_FN(name) static PFN_##name p_##name;
XR_FUNCS(DECLARE_FN)
static PFN_xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties;

enum class XrStage { NotStarted, Failed, Ready };
static XrStage g_stage = XrStage::NotStarted;
static volatile bool g_quitting;  // quit1: the engine is closing (see XrBeforeEngineClose)

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static UINT g_recW, g_recH;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_space = XR_NULL_HANDLE;
static XrSpace g_viewSpace = XR_NULL_HANDLE;  // the headset, for head tracking
static XrTime g_nextDisplayTime;              // when the frame the game renders next will show
static XrPosef g_renderPose;                  // head pose the game is rendering the next frame with
static bool g_renderPoseValid;
static float g_tanX = 1.0f, g_tanY = 0.5625f;  // game's field of view, tan of half-angles
static float g_imgFov[2][4] = {{-1, 1, 0.5625f, -0.5625f}, {-1, 1, 0.5625f, -0.5625f}};  // oc1: each eye image's view: tan left, right, up, down
static float g_eyeTan[2][4];  // oc1: each eye's real view as the headset reports it
static void FovOf(XrFovf *f, int eye) {
    f->angleLeft = atanf(g_imgFov[eye][0]), f->angleRight = atanf(g_imgFov[eye][1]);
    f->angleUp = atanf(g_imgFov[eye][2]), f->angleDown = atanf(g_imgFov[eye][3]);
}
static XrSwapchain g_swapchain = XR_NULL_HANDLE;
static XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
static bool g_sessionRunning;
static UINT g_width, g_height;
static DXGI_FORMAT g_format;
static XrSwapchainImageD3D11KHR g_images[8];
static uint32_t g_imageCount;
// Stereo (alternate eye): the right eye gets its own swapchain; each eye keeps
// showing its last image while the game renders the other eye.
static XrSwapchain g_swapchainR = XR_NULL_HANDLE;
static XrSwapchainImageD3D11KHR g_imagesR[8];
static uint32_t g_imageCountR;
static int g_submitEye = -1;      // eye the frame being submitted was rendered for, -1 = mono
static XrPosef g_pendingEyePose;  // its pose
static XrPosef g_eyePose[2];
static bool g_eyeReady[2];
static LONG g_xrFrames;
// flat1: cutscenes on a 2D screen. The game's picture goes on a flat quad layer
// fixed in the room (placed in front of you as the cutscene starts); the debug
// menu, when open, gets a quad layer of its own.
static XrSwapchain g_menuChain = XR_NULL_HANDLE;
static XrSwapchainImageD3D11KHR g_menuImages[8];
static uint32_t g_menuImageCount;
static bool g_menuChainFailed;
static ID3D11Texture2D *g_menuTex;  // the menu's texture (menu.cpp owns it)
static XrPosef g_menuPose;
static float g_menuW, g_menuH;
static DWORD g_menuTick;
static bool g_flatWas;
static LONG g_flatRecGen;
static float g_scrYaw, g_scrPos[3];

static bool Check(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    Log("XR: %s failed (%d)", what, (int)r);
    return false;
}

// ---- Runtime discovery (what the Khronos loader would normally do) ----

static bool ReadActiveRuntimeJson(wchar_t *out, DWORD len) {
    DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", out, len);
    if (envLen > 0 && envLen < len) return true;
    // In a 32-bit process this reads the WOW6432Node key, i.e. the 32-bit runtime.
    DWORD bytes = len * sizeof(wchar_t);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ,
                        nullptr, out, &bytes) == ERROR_SUCCESS;
}

static bool LibraryPathFromJson(const wchar_t *jsonPath, wchar_t *out, DWORD len) {
    HANDLE f = CreateFileW(jsonPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[8192] = {};
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
    CloseHandle(f);

    const char *key = strstr(buf, "\"library_path\"");
    if (!key) return false;
    const char *q = strchr(key + 14, '"');
    if (!q) return false;
    char path[MAX_PATH] = {};
    size_t n = 0;
    for (const char *p = q + 1; *p && *p != '"' && n < MAX_PATH - 1; ++p) {
        if (*p == '\\' && p[1]) ++p;  // JSON escape: \\ -> backslash
        path[n++] = *p;
    }
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);

    bool absolute = (wpath[0] && wpath[1] == L':') || (wpath[0] == L'\\');
    if (absolute) {
        wcsncpy(out, wpath, len);
    } else {
        wcsncpy(out, jsonPath, len);
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash) slash[1] = 0;
        wcsncat(out, wpath, len - wcslen(out) - 1);
    }
    return true;
}

static bool LoadRuntime() {
    wchar_t json[MAX_PATH], lib[MAX_PATH];
    if (!ReadActiveRuntimeJson(json, MAX_PATH)) {
        Log("XR: no 32-bit OpenXR runtime registered");
        return false;
    }
    if (!LibraryPathFromJson(json, lib, MAX_PATH)) {
        Log("XR: couldn't read library_path from %ls", json);
        return false;
    }
    HMODULE rt = LoadLibraryW(lib);
    if (!rt) {
        Log("XR: LoadLibrary(%ls) failed, error %lu", lib, GetLastError());
        return false;
    }
    auto negotiate =
        (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) {
        Log("XR: runtime has no xrNegotiateLoaderRuntimeInterface");
        return false;
    }
    XrNegotiateLoaderInfo li = {};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof li;
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr = {};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof rr;
    if (!Check(negotiate(&li, &rr), "negotiate") || !rr.getInstanceProcAddr) return false;
    g_gipa = rr.getInstanceProcAddr;
    Log("XR: loaded runtime %ls", lib);
    return true;
}

// ---- Touch controllers ----
// One action per control; the game sees them as an Xbox pad (input.cpp).

enum {
    kA, kB, kX, kY, kMenu, kLStickClick, kRStickClick,  // booleans
    kBoolCount
};
enum { kLTrigger, kRTrigger, kLGrip, kRGrip, kFloatCount };
enum { kLStick, kRStick, kVecCount };
static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction g_boolActs[kBoolCount], g_floatActs[kFloatCount], g_vecActs[kVecCount], g_aimAct, g_hapticAct;
static XrPath g_hands[2];
static XrSpace g_aimSpace[2];
static bool g_controlsAttached;

static VrPad g_pads[2];
static volatile LONG g_padIdx = -1;

static XrPath Path(const char *s) {
    XrPath p = XR_NULL_PATH;
    p_xrStringToPath(g_instance, s, &p);
    return p;
}

static XrAction MakeAction(const char *name, const char *label, XrActionType type, bool bothHands) {
    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    strcpy(ai.actionName, name);
    strcpy(ai.localizedActionName, label);
    ai.actionType = type;
    if (bothHands) ai.countSubactionPaths = 2, ai.subactionPaths = g_hands;
    XrAction a = XR_NULL_HANDLE;
    Check(p_xrCreateAction(g_actionSet, &ai, &a), name);
    return a;
}

// Once per instance: the actions and the Touch controller bindings.
static bool ControlsCreate() {
    if (g_actionSet) return true;
    if (!p_xrCreateActionSet || !p_xrStringToPath) return false;
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gameplay");
    strcpy(si.localizedActionSetName, "Gameplay");
    if (!Check(p_xrCreateActionSet(g_instance, &si, &g_actionSet), "xrCreateActionSet")) return false;
    g_hands[0] = Path("/user/hand/left");
    g_hands[1] = Path("/user/hand/right");

    static const char *boolNames[kBoolCount] = {"a", "b", "x", "y", "menu", "lstick_click", "rstick_click"};
    static const char *floatNames[kFloatCount] = {"ltrigger", "rtrigger", "lgrip", "rgrip"};
    static const char *vecNames[kVecCount] = {"lstick", "rstick"};
    for (int i = 0; i < kBoolCount; ++i)
        g_boolActs[i] = MakeAction(boolNames[i], boolNames[i], XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    for (int i = 0; i < kFloatCount; ++i)
        g_floatActs[i] = MakeAction(floatNames[i], floatNames[i], XR_ACTION_TYPE_FLOAT_INPUT, false);
    for (int i = 0; i < kVecCount; ++i)
        g_vecActs[i] = MakeAction(vecNames[i], vecNames[i], XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    g_aimAct = MakeAction("aim", "Hand aim", XR_ACTION_TYPE_POSE_INPUT, true);
    g_hapticAct = MakeAction("rumble", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);  // rumble1

    const struct { XrAction a; const char *path; } b[] = {
        {g_boolActs[kA], "/user/hand/right/input/a/click"},
        {g_boolActs[kB], "/user/hand/right/input/b/click"},
        {g_boolActs[kX], "/user/hand/left/input/x/click"},
        {g_boolActs[kY], "/user/hand/left/input/y/click"},
        {g_boolActs[kMenu], "/user/hand/left/input/menu/click"},
        {g_boolActs[kLStickClick], "/user/hand/left/input/thumbstick/click"},
        {g_boolActs[kRStickClick], "/user/hand/right/input/thumbstick/click"},
        {g_floatActs[kLTrigger], "/user/hand/left/input/trigger/value"},
        {g_floatActs[kRTrigger], "/user/hand/right/input/trigger/value"},
        {g_floatActs[kLGrip], "/user/hand/left/input/squeeze/value"},
        {g_floatActs[kRGrip], "/user/hand/right/input/squeeze/value"},
        {g_vecActs[kLStick], "/user/hand/left/input/thumbstick"},
        {g_vecActs[kRStick], "/user/hand/right/input/thumbstick"},
        {g_aimAct, "/user/hand/left/input/aim/pose"},
        {g_aimAct, "/user/hand/right/input/aim/pose"},
        {g_hapticAct, "/user/hand/left/output/haptic"},
        {g_hapticAct, "/user/hand/right/output/haptic"},
    };
    XrActionSuggestedBinding sb[sizeof b / sizeof b[0]];
    for (size_t i = 0; i < sizeof b / sizeof b[0]; ++i) sb[i] = {b[i].a, Path(b[i].path)};
    XrInteractionProfileSuggestedBinding ps = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    ps.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    ps.countSuggestedBindings = sizeof sb / sizeof sb[0];
    ps.suggestedBindings = sb;
    return Check(p_xrSuggestInteractionProfileBindings(g_instance, &ps), "controller bindings");
}

static void ControlsAttach() {
    g_controlsAttached = false;
    g_aimSpace[0] = g_aimSpace[1] = XR_NULL_HANDLE;
    if (!ControlsCreate()) return;
    XrSessionActionSetsAttachInfo at = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &g_actionSet;
    if (!Check(p_xrAttachSessionActionSets(g_session, &at), "attach controller actions")) return;
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo asi = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.action = g_aimAct;
        asi.subactionPath = g_hands[h];
        asi.poseInActionSpace.orientation.w = 1;
        Check(p_xrCreateActionSpace(g_session, &asi, &g_aimSpace[h]), "hand space");
    }
    g_controlsAttached = true;
    Log("XR: Touch controllers set up");
}

static bool GetBool(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_session, &gi, &st)) && st.isActive && st.currentState;
}
static float GetFloat(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(p_xrGetActionStateFloat(g_session, &gi, &st)) && st.isActive ? st.currentState : 0;
}
static XrVector2f GetVec(XrAction a, bool *active) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    bool ok = XR_SUCCEEDED(p_xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive;
    if (active && ok) *active = true;
    return ok ? st.currentState : XrVector2f{0, 0};
}

// rumble1 (1 Nov): the game's pad rumble (XInputSetState, input.cpp) on the
// Touch controllers. Heavy (left) motor and light (right) motor: each hand
// gets the stronger of the two, times the Rumble strength. Renewed in short
// pulses so it stops by itself if the game goes quiet.
bool g_leftHanded;  // lefty1: [vr] LeftHanded
bool *VrLeftHanded() { return &g_leftHanded; }
static void HapticsUpdate() {
    static float s_last[2];
    static DWORD s_sent[2];
    if (!g_hapticAct || !p_xrApplyHapticFeedback) return;
    float heavy, light;
    InputRumble(&heavy, &light);
    float amp = (heavy > light ? heavy : light) * InputRumbleStrength();
    if (amp > 1) amp = 1;
    float game = amp, buzz = HolsterBuzz(), bb = BowBuzz();
    if (bb > buzz) buzz = bb;  // bow1p  // holster1: short buzz on the right hand entering a body spot
    DWORD t = GetTickCount();
    for (int h = 0; h < 2; ++h) {
        float hb = h == 1 ? buzz : BowBuzzLeft();
        if (TwoHandSteadyBuzz() > hb) hb = TwoHandSteadyBuzz();  // steadybuzz1
        if (HandsBuzz() > hb) hb = HandsBuzz();  // capture2
        if (SlowmoBuzz() > hb) hb = SlowmoBuzz();  // slowbtn1: both hands, as slow motion starts and ends
        if (SlowFxBuzz() > hb) hb = SlowFxBuzz();  // slowfx1: the heartbeat while it is on
        if (h == 0 && CyberEyeBuzz() > hb) hb = CyberEyeBuzz();  // cybereye1  // bowfeel2: the nock tick in the bow hand too
        amp = hb > game ? hb : game;
        XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = g_hapticAct;
        hi.subactionPath = g_hands[g_leftHanded ? 1 - h : h];  // lefty1: the gun hand's buzz goes to the left controller
        if (amp < 0.02f) {
            if (s_last[h] > 0) p_xrStopHapticFeedback(g_session, &hi), s_last[h] = 0;
            continue;
        }
        if (fabsf(amp - s_last[h]) < 0.03f && t - s_sent[h] < 60) continue;
        XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
        v.duration = 100000000;  // 100 ms, renewed while the game keeps rumbling
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        v.amplitude = amp;
        p_xrApplyHapticFeedback(g_session, &hi, (const XrHapticBaseHeader *)&v);
        s_last[h] = amp, s_sent[h] = t;
    }
}

static void ControlsUpdate() {
    if (!g_controlsAttached || g_sessionState != XR_SESSION_STATE_FOCUSED) {
        g_padIdx = -1;
        return;
    }
    XrActiveActionSet as = {g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &as;
    if (XR_FAILED(p_xrSyncActions(g_session, &sync))) return;

    VrPad p = {};
    XrVector2f l = GetVec(g_vecActs[kLStick], &p.active), r = GetVec(g_vecActs[kRStick], &p.active);
    p.lx = l.x, p.ly = l.y, p.rx = r.x, p.ry = r.y;
    p.lt = GetFloat(g_floatActs[kLTrigger]);
    p.rt = GetFloat(g_floatActs[kRTrigger]);
    p.lgrip = GetFloat(g_floatActs[kLGrip]);
    p.rgrip = GetFloat(g_floatActs[kRGrip]);
    p.a = GetBool(g_boolActs[kA]), p.b = GetBool(g_boolActs[kB]);
    p.x = GetBool(g_boolActs[kX]), p.y = GetBool(g_boolActs[kY]);
    p.menu = GetBool(g_boolActs[kMenu]);
    p.lclick = GetBool(g_boolActs[kLStickClick]), p.rclick = GetBool(g_boolActs[kRStickClick]);
    const bool lefty = g_leftHanded;  // lefty1: read once, so a frame is all one way

    for (int h = 0; h < 2; ++h) {
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (g_aimSpace[h] && g_nextDisplayTime &&
            XR_SUCCEEDED(p_xrLocateSpace(g_aimSpace[h], g_space, g_nextDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            p.handValid[h] = true;
            memcpy(p.handQ[h], &loc.pose.orientation, 16);
            memcpy(p.handP[h], &loc.pose.position, 12);
        }
    }
    // lefty1 (DrGonzaga 22 Nov: left-handed mode): the two controllers swap roles here, at the one place the
    // whole mod reads them, so everything that was the right hand's (gun, aim, fire, holsters, melee, turning)
    // follows the left controller and the left hand's (walking, two-handed grip, wrist menu, cyber-eye)
    // the right. Face buttons swap with them (A<->X, B<->Y). The Menu button stays on the left controller
    // (it has no twin: the right one's is the headset's own).
    if (lefty) {
        auto sw = [](float &a, float &b) { float t = a; a = b; b = t; };
        auto swb = [](bool &a, bool &b) { bool t = a; a = b; b = t; };
        sw(p.lx, p.rx), sw(p.ly, p.ry), sw(p.lt, p.rt), sw(p.lgrip, p.rgrip);
        swb(p.a, p.x), swb(p.b, p.y), swb(p.lclick, p.rclick), swb(p.handValid[0], p.handValid[1]);
        for (int i = 0; i < 4; ++i) sw(p.handQ[0][i], p.handQ[1][i]);
        for (int i = 0; i < 3; ++i) sw(p.handP[0][i], p.handP[1][i]);
    }
    LONG next = g_padIdx < 0 ? 0 : 1 - g_padIdx;
    g_pads[next] = p;
    InterlockedExchange(&g_padIdx, next);

    // quiet1: only when the state changes (at most once a second), not every 10 s.
    static DWORD lastLog;
    static int lastState = -1;
    int state = (p.active ? 1 : 0) | (p.handValid[0] ? 2 : 0) | (p.handValid[1] ? 4 : 0);
    if (state != lastState && GetTickCount() - lastLog > 1000) {
        lastLog = GetTickCount();
        lastState = state;
        Log("XR: controllers %s, left hand %s, right hand %s", p.active ? "active" : "not seen",
            p.handValid[0] ? "tracked" : "-", p.handValid[1] ? "tracked" : "-");
    }
}

bool XrGetPad(VrPad *out) {
    LONG i = g_padIdx;
    if (i < 0) return false;
    *out = g_pads[i];
    return out->active;
}

// ---- Instance, session, swapchain ----

static bool g_haveVisMask;
static PFN_xrGetVisibilityMaskKHR p_xrGetVisibilityMaskKHR;

static bool CreateInstance() {
    g_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&p_xrCreateInstance);
    g_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
           (PFN_xrVoidFunction *)&p_xrEnumerateInstanceExtensionProperties);
    if (!p_xrCreateInstance) return false;

    // mask0 (review N8, measuring only): does the runtime offer the lens mask?
    const char *exts[2] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    uint32_t nexts = 1;
    {
        static XrExtensionProperties props[96];
        uint32_t n = 0;
        for (auto &e : props) e = {XR_TYPE_EXTENSION_PROPERTIES};
        if (p_xrEnumerateInstanceExtensionProperties &&
            XR_SUCCEEDED(p_xrEnumerateInstanceExtensionProperties(nullptr, 96, &n, props)))
            for (uint32_t i = 0; i < n && i < 96; ++i)
                if (!strcmp(props[i].extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME)) g_haveVisMask = true;
        Log("XR: runtime lists %u extensions; lens mask (XR_KHR_visibility_mask) %s", n, g_haveVisMask ? "offered" : "NOT offered");
        if (g_haveVisMask) exts[nexts++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
    }
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Far Cry 3 VR");
    strcpy(ci.applicationInfo.engineName, "fc3vr");
    ci.applicationInfo.applicationVersion = 2;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = nexts;
    ci.enabledExtensionNames = exts;
    if (!Check(p_xrCreateInstance(&ci, &g_instance), "xrCreateInstance")) return false;

#define LOAD_FN(name) g_gipa(g_instance, #name, (PFN_xrVoidFunction *)&p_##name);
    XR_FUNCS(LOAD_FN)
#undef LOAD_FN
    if (g_haveVisMask) g_gipa(g_instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&p_xrGetVisibilityMaskKHR);

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (p_xrGetInstanceProperties && XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &props)))
        Log("XR: runtime \"%s\" %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
            XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(p_xrGetSystem(g_instance, &si, &g_system), "xrGetSystem (is the headset connected?)")) return false;
    // The runtime's recommended size per eye (1.0 on the menu's resolution scale).
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    if (p_xrEnumerateViewConfigurationViews &&
        XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                          &n, views)) && n >= 1) {
        g_recW = views[0].recommendedImageRectWidth, g_recH = views[0].recommendedImageRectHeight;
        Log("XR: recommended size per eye %ux%u (max %ux%u)", g_recW, g_recH, views[0].maxImageRectWidth,
            views[0].maxImageRectHeight);
    }
    return true;
}
bool XrImageSize(UINT *w, UINT *h) {  // flat1
    if (!g_width || !g_height) return false;
    *w = g_width, *h = g_height;
    return true;
}

// flat1 (render thread, from MenuDraw): show the menu panel as its own layer this frame.
void XrSetMenuLayer(ID3D11Texture2D *tex, const float c[3], float yaw, float w, float h) {
    g_menuTex = tex;
    g_menuPose.position = {c[0], c[1], c[2]};
    g_menuPose.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_menuW = w, g_menuH = h;
    g_menuTick = GetTickCount();
}

// flat1: copy the menu texture into its own headset swapchain (made the first time).
static bool MenuLayerCopy(ID3D11DeviceContext *ctx) {
    if (!g_menuTex || g_menuChainFailed || GetTickCount() - g_menuTick > 100) return false;
    D3D11_TEXTURE2D_DESC td;
    g_menuTex->GetDesc(&td);
    if (!g_menuChain) {
        int64_t formats[64];
        uint32_t count = 0;
        p_xrEnumerateSwapchainFormats(g_session, 64, &count, formats);
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        static const DXGI_FORMAT kWant[2] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
        for (DXGI_FORMAT w : kWant)
            for (uint32_t i = 0; i < count && f == DXGI_FORMAT_UNKNOWN; ++i)
                if (formats[i] == w) f = w;
        XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = f, sci.sampleCount = 1, sci.width = td.Width, sci.height = td.Height;
        sci.faceCount = 1, sci.arraySize = 1, sci.mipCount = 1;
        for (auto &img : g_menuImages) img = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_UNKNOWN ||
            !Check(p_xrCreateSwapchain(g_session, &sci, &g_menuChain), "xrCreateSwapchain (menu)") ||
            !Check(p_xrEnumerateSwapchainImages(g_menuChain, 8, &g_menuImageCount, (XrSwapchainImageBaseHeader *)g_menuImages),
                   "xrEnumerateSwapchainImages (menu)")) {
            Log("XR: no menu layer for the 2D cutscene screen (format %d)", (int)td.Format);
            if (g_menuChain) p_xrDestroySwapchain(g_menuChain);
            g_menuChain = XR_NULL_HANDLE, g_menuChainFailed = true;
            return false;
        }
        Log("XR: menu layer for the 2D cutscene screen made (%ux%u, format %d)", td.Width, td.Height, (int)f);
    }
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!Check(p_xrAcquireSwapchainImage(g_menuChain, &ai, &index), "acquire (menu)")) return false;
    bool ok = Check(p_xrWaitSwapchainImage(g_menuChain, &wi), "wait image (menu)");
    if (ok && index < g_menuImageCount) ctx->CopyResource(g_menuImages[index].texture, g_menuTex);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(g_menuChain, &ri);
    return ok && index < g_menuImageCount;
}

bool XrRecommendedSize(UINT *w, UINT *h) {
    if (!g_recW || !g_recH) return false;
    *w = g_recW, *h = g_recH;
    return true;
}

// The game's back buffer is BGRA; pick a swapchain format we can CopyResource into.
static DXGI_FORMAT PickFormat(DXGI_FORMAT game) {
    int64_t formats[64];
    uint32_t count = 0;
    p_xrEnumerateSwapchainFormats(g_session, 64, &count, formats);
    char list[512] = "";
    for (uint32_t i = 0; i < count; ++i) {
        char one[16];
        snprintf(one, sizeof one, "%d ", (int)formats[i]);
        strncat(list, one, sizeof list - strlen(list) - 1);
    }
    Log("XR: runtime swapchain formats: %s", list);

    // The game's pixels are already gamma-encoded, so an _SRGB swapchain shows
    // them as the monitor would; plain UNORM would look washed out.
    DXGI_FORMAT wanted[2] = {game, game};
    if (game == DXGI_FORMAT_B8G8R8A8_UNORM) wanted[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (game == DXGI_FORMAT_R8G8B8A8_UNORM) wanted[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    for (DXGI_FORMAT w : wanted)
        for (uint32_t i = 0; i < count; ++i)
            if (formats[i] == w) return w;
    return DXGI_FORMAT_UNKNOWN;
}

// crash3: does the headset runtime hook the game's D3D11 device? The 0xdededede
// crash (FC3+114f2eab) showed VR-runtime code between the game's view creation
// and d3d11. Log which module owns the device's create functions, once before
// and once after the session is made.
static void LogDeviceHooks(ID3D11Device *dev, const char *when) {
    static const struct { int slot; const char *name; } kSlots[] = {
        {5, "CreateTexture2D"}, {7, "CreateShaderResourceView"}, {8, "CreateUnorderedAccessView"},
        {9, "CreateRenderTargetView"}, {10, "CreateDepthStencilView"}};
    void **vt = *(void ***)dev;
    for (auto &k : kSlots) {
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)vt[k.slot], &m))
            GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t *b = wcsrchr(name, L'\\');
        Log("XR: %s: device %s -> %ls+0x%lx", when, k.name, b ? b + 1 : name,
            (unsigned long)((BYTE *)vt[k.slot] - (BYTE *)m));
    }
}

static bool CreateSessionFor(ID3D11Device *dev, const DXGI_SWAP_CHAIN_DESC &desc) {
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req), "graphics requirements"))
        return false;

    IDXGIDevice *dxgiDev = nullptr;
    IDXGIAdapter *adapter = nullptr;
    DXGI_ADAPTER_DESC ad = {};
    if (SUCCEEDED(dev->QueryInterface(IID_IDXGIDevice, (void **)&dxgiDev)) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
        adapter->GetDesc(&ad);
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    if (memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) != 0)
        Log("XR: warning, game GPU (%ls) isn't the one the headset wants", ad.Description);

    XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = dev;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    if (g_verboseLog) LogDeviceHooks(dev, "before session");  // tidy2: crash3 check, detailed log only
    if (!Check(p_xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return false;
    if (g_verboseLog) LogDeviceHooks(dev, "after session");  // tidy2: crash3 check, detailed log only

    XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(p_xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace")) return false;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(p_xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace), "view space")) return false;
    ControlsAttach();

    g_format = PickFormat(desc.BufferDesc.Format);
    if (g_format == DXGI_FORMAT_UNKNOWN) {
        Log("XR: runtime offers no format compatible with the game's (%d); need a shader copy",
            (int)desc.BufferDesc.Format);
        return false;
    }
    g_width = desc.BufferDesc.Width;
    g_height = desc.BufferDesc.Height;

    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = g_format;
    sc.sampleCount = 1;
    sc.width = g_width;
    sc.height = g_height;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (!Check(p_xrCreateSwapchain(g_session, &sc, &g_swapchain), "xrCreateSwapchain")) return false;

    for (auto &img : g_images) img = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    if (!Check(p_xrEnumerateSwapchainImages(g_swapchain, 8, &g_imageCount,
                                            (XrSwapchainImageBaseHeader *)g_images),
               "xrEnumerateSwapchainImages"))
        return false;
    if (!Check(p_xrCreateSwapchain(g_session, &sc, &g_swapchainR), "xrCreateSwapchain (right eye)")) return false;
    if (g_verboseLog) LogDeviceHooks(dev, "after swapchains");  // tidy2: crash3 check, detailed log only
    for (auto &img : g_imagesR) img = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    if (!Check(p_xrEnumerateSwapchainImages(g_swapchainR, 8, &g_imageCountR,
                                            (XrSwapchainImageBaseHeader *)g_imagesR),
               "xrEnumerateSwapchainImages (right eye)"))
        return false;
    g_eyeReady[0] = g_eyeReady[1] = false;
    Log("XR: session ready to start, screen %ux%u format %d, %u swapchain images", g_width, g_height,
        (int)g_format, g_imageCount);
    return true;
}

static IDXGISwapChain *g_sessionChain;
// cybereye1: thin dark scan lines over the cyber-eye's screen: a small picture of its own (made
// once), see-through between the lines, laid over the screen as one more layer.
static XrSwapchain g_scanChain = XR_NULL_HANDLE;
static bool g_scanFailed;
static const int32_t kScanW = 8, kScanH = 480;  // 120 lines
static bool ScanLayer(ID3D11DeviceContext *ctx) {
    float s = CyberEyeScanlines();
    if (s < 0.01f || g_scanFailed) return false;
    if (g_scanChain) return true;
    int64_t formats[64];
    uint32_t count = 0;
    p_xrEnumerateSwapchainFormats(g_session, 64, &count, formats);
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    static const DXGI_FORMAT kWant[2] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
    for (DXGI_FORMAT w : kWant)
        for (uint32_t i = 0; i < count && f == DXGI_FORMAT_UNKNOWN; ++i)
            if (formats[i] == w) f = w;
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = f, sci.sampleCount = 1, sci.width = kScanW, sci.height = kScanH;
    sci.faceCount = 1, sci.arraySize = 1, sci.mipCount = 1;
    XrSwapchainImageD3D11KHR imgs[8];
    for (auto &img : imgs) img = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    uint32_t n = 0, index = 0;
    bool ok = f != DXGI_FORMAT_UNKNOWN && Check(p_xrCreateSwapchain(g_session, &sci, &g_scanChain), "xrCreateSwapchain (scan lines)") &&
              Check(p_xrEnumerateSwapchainImages(g_scanChain, 8, &n, (XrSwapchainImageBaseHeader *)imgs), "xrEnumerateSwapchainImages (scan lines)");
    if (ok) {
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        ok = Check(p_xrAcquireSwapchainImage(g_scanChain, &ai, &index), "acquire (scan lines)") && index < n;
        if (ok) {
            ok = Check(p_xrWaitSwapchainImage(g_scanChain, &wi), "wait image (scan lines)");
            if (ok) {
                static BYTE px[kScanW * kScanH * 4];
                BYTE a = (BYTE)(s * 255.0f + 0.5f);
                for (int y = 0; y < kScanH; ++y)
                    for (int x = 0; x < kScanW; ++x) {
                        BYTE *q = px + (y * kScanW + x) * 4;
                        q[0] = q[1] = q[2] = 0, q[3] = (y & 3) >= 2 ? a : 0;
                    }
                ctx->UpdateSubresource(imgs[index].texture, 0, nullptr, px, kScanW * 4, 0);
            }
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(g_scanChain, &ri);
        }
    }
    if (!ok) {
        Log("XR: no scan lines for the cyber-eye screen (the layer could not be made)");
        if (g_scanChain) p_xrDestroySwapchain(g_scanChain);
        g_scanChain = XR_NULL_HANDLE, g_scanFailed = true;
        return false;
    }
    Log("XR: scan line layer for the cyber-eye screen made (strength %.2f)", s);
    return true;
}

static IDXGISwapChain *g_closedChain;  // quit2: the swapchain of an engine that has closed; no new session for it

static void DestroySession() {
    SmaaOnSwapChainGoing();
    if (g_swapchain) p_xrDestroySwapchain(g_swapchain);
    if (g_swapchainR) p_xrDestroySwapchain(g_swapchainR);
    g_swapchainR = XR_NULL_HANDLE;
    if (g_menuChain) p_xrDestroySwapchain(g_menuChain);  // flat1
    if (g_scanChain) p_xrDestroySwapchain(g_scanChain);  // cybereye1
    g_scanChain = XR_NULL_HANDLE, g_scanFailed = false;
    g_menuChain = XR_NULL_HANDLE, g_menuImageCount = 0, g_menuChainFailed = false, g_menuTex = nullptr, g_flatWas = false;
    g_imageCountR = 0;
    g_eyeReady[0] = g_eyeReady[1] = false;
    g_submitEye = -1;
    if (g_session) p_xrDestroySession(g_session);  // also frees the space
    g_swapchain = XR_NULL_HANDLE;
    g_space = XR_NULL_HANDLE;
    g_viewSpace = XR_NULL_HANDLE;
    g_aimSpace[0] = g_aimSpace[1] = XR_NULL_HANDLE;
    g_controlsAttached = false;
    g_padIdx = -1;
    g_session = XR_NULL_HANDLE;
    g_renderPoseValid = false;
    g_nextDisplayTime = 0;
    g_sessionRunning = false;
    g_sessionState = XR_SESSION_STATE_UNKNOWN;
    g_imageCount = 0;
    g_sessionChain = nullptr;
}

// The instance lives for the whole run; the session is rebuilt whenever the
// game's swapchain is replaced or resized.
static void Init(IDXGISwapChain *sc) {
    g_stage = XrStage::Failed;
    DXGI_SWAP_CHAIN_DESC desc;
    ID3D11Device *dev = nullptr;
    if (FAILED(sc->GetDesc(&desc)) || FAILED(sc->GetDevice(IID_ID3D11Device, (void **)&dev))) {
        Log("XR: couldn't get the game's device");
        return;
    }
    bool ok = (g_instance || (LoadRuntime() && CreateInstance())) && CreateSessionFor(dev, desc);
    dev->Release();  // the session keeps its own reference
    if (ok) {
        g_stage = XrStage::Ready;
        g_sessionChain = sc;
    } else {
        DestroySession();
        Log("XR: disabled for this run; the game continues flat");
    }
}

void XrOnSwapChainGoing(IDXGISwapChain *sc, const char *why) {
    if (g_stage != XrStage::Ready || sc != g_sessionChain) return;
    Log("XR: game swapchain %s; closing the headset session", why);
    DestroySession();
    g_stage = XrStage::NotStarted;  // rebuilt on the next Present
}

// ---- Per frame ----

static void PollEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (p_xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *e = (XrEventDataSessionStateChanged *)&ev;
            g_sessionState = e->state;
            Log("XR: session state %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_sessionRunning = Check(p_xrBeginSession(g_session, &bi), "xrBeginSession");
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                p_xrEndSession(g_session);
                g_sessionRunning = false;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_sessionRunning = false;
                DestroySession();
                g_stage = XrStage::Failed;
                Log("XR: runtime ended the session; continuing flat");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_sessionRunning = false;
            g_stage = XrStage::Failed;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The headset's own per-eye field of view and eye separation.
static bool g_headsetFovKnown;
static float g_headsetTanX, g_headsetTanY, g_ipd = 0.064f;

// mask0 (review N8, measuring only, once): how much of each eye's picture the headset
// can never show. The game draws a centred picture (+-tx, +-ty) but each eye's real
// view is off centre, and inside the real view the lens hides the corners (the
// runtime's hidden mesh, x and y as tangents at 1 m). Counted on a 128 x 128 grid.
static void MeasureWaste(const XrView *views, float tx, float ty) {
    static bool done;
    if (done) return;
    done = true;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const XrFovf &f = views[eye].fov;
        float l = tanf(f.angleLeft), r = tanf(f.angleRight), u = tanf(f.angleUp), d = tanf(f.angleDown);
        static XrVector2f vtx[1024];
        static uint32_t idx[3072];
        XrVisibilityMaskKHR m = {XR_TYPE_VISIBILITY_MASK_KHR};
        m.vertexCapacityInput = 1024, m.vertices = vtx, m.indexCapacityInput = 3072, m.indices = idx;
        XrResult res = XR_ERROR_FUNCTION_UNSUPPORTED;
        if (p_xrGetVisibilityMaskKHR)
            res = p_xrGetVisibilityMaskKHR(g_session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye,
                                           XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &m);
        uint32_t ni = XR_SUCCEEDED(res) && m.vertexCountOutput <= 1024 && m.indexCountOutput <= 3072 ? m.indexCountOutput : 0;
        int outside = 0, hidden = 0;
        const int N = 128;
        for (int gy = 0; gy < N; ++gy)
            for (int gx = 0; gx < N; ++gx) {
                float x = ((gx + 0.5f) / N * 2 - 1) * tx, y = ((gy + 0.5f) / N * 2 - 1) * ty;
                if (x < l || x > r || y < d || y > u) { ++outside; continue; }
                for (uint32_t k = 0; k + 2 < ni; k += 3) {
                    const XrVector2f &a = vtx[idx[k]], &b = vtx[idx[k + 1]], &c = vtx[idx[k + 2]];
                    float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
                    float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
                    float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
                    if (d1 == 0 && d2 == 0 && d3 == 0) continue;  // an empty triangle (VDXR's whole "mask" is one)
                    if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))) { ++hidden; break; }
                }
            }
        float minx = 9, maxx = -9, miny = 9, maxy = -9;
        for (uint32_t i = 0; i < (ni ? m.vertexCountOutput : 0); ++i)
            minx = fminf(minx, vtx[i].x), maxx = fmaxf(maxx, vtx[i].x), miny = fminf(miny, vtx[i].y), maxy = fmaxf(maxy, vtx[i].y);
        Log("XR: waste check, %s eye: real view %.1f / %.1f / %.1f / %.1f deg (left right up down); of the picture drawn, "
            "%.1f%% is outside the real view and %.1f%% is inside it but hidden by the lens mask (mask: result %d, %u points, "
            "%u triangles, x %.2f..%.2f y %.2f..%.2f)",
            eye ? "right" : "left", f.angleLeft * 57.2958f, f.angleRight * 57.2958f, f.angleUp * 57.2958f, f.angleDown * 57.2958f,
            outside * 100.0f / (N * N), hidden * 100.0f / (N * N), (int)res, ni ? m.vertexCountOutput : 0, ni / 3, minx, maxx, miny, maxy);
    }
}

static void LearnHeadsetViews(XrTime t) {
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_viewSpace;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(p_xrLocateViews(g_session, &li, &vs, 2, &n, views)) || n != 2) return;
    float tx = 0, ty = 0;
    for (auto &v : views) {
        tx = fmaxf(tx, fmaxf(tanf(-v.fov.angleLeft), tanf(v.fov.angleRight)));
        ty = fmaxf(ty, fmaxf(tanf(v.fov.angleUp), tanf(-v.fov.angleDown)));
    }
    if (tx < 0.1f || ty < 0.1f) return;
    float dx = views[1].pose.position.x - views[0].pose.position.x, dy = views[1].pose.position.y - views[0].pose.position.y,
          dz = views[1].pose.position.z - views[0].pose.position.z;
    float ipd = sqrtf(dx * dx + dy * dy + dz * dz);
    if (ipd > 0.04f && ipd < 0.09f) g_ipd = ipd;
    for (int e = 0; e < 2; ++e) {
        const XrFovf &f = views[e].fov;
        g_eyeTan[e][0] = tanf(f.angleLeft), g_eyeTan[e][1] = tanf(f.angleRight), g_eyeTan[e][2] = tanf(f.angleUp), g_eyeTan[e][3] = tanf(f.angleDown);
    }
    g_headsetTanX = tx, g_headsetTanY = ty;
    g_headsetFovKnown = true;
    Log("XR: headset eye fov L %.1f R %.1f U %.1f D %.1f deg (left eye), %.1f x %.1f deg symmetric, IPD %.1f mm",
        views[0].fov.angleLeft * 57.2958f, views[0].fov.angleRight * 57.2958f, views[0].fov.angleUp * 57.2958f,
        views[0].fov.angleDown * 57.2958f, 2 * atanf(tx) * 57.2958f, 2 * atanf(ty) * 57.2958f, g_ipd * 1000);
    MeasureWaste(views, tx, ty);
}

bool XrGetHeadsetFov(float *tanX, float *tanY) {
    if (!g_headsetFovKnown) return false;
    *tanX = g_headsetTanX, *tanY = g_headsetTanY;
    return true;
}

float XrGetIpd() { return g_ipd; }

// Paired stereo: the game draws left then right as fast as it can, and the
// headset gets both together, so each headset frame has two fresh eyes
// (needs the game to run at twice the headset rate). F3 toggles; otherwise
// eyes alternate one per headset frame.
static volatile bool g_paired = false;  // the game manages ~118 fps at 3840x3744: too slow for 90 Hz pairs

static bool CopyBackBuffer(IDXGISwapChain *sc, XrSwapchain chain, XrSwapchainImageD3D11KHR *images) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!Check(p_xrAcquireSwapchainImage(chain, &ai, &index), "acquire") ||
        !Check(p_xrWaitSwapchainImage(chain, &wi), "wait image"))
        return false;
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void **)&back))) {
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        back->GetDevice(&dev);
        dev->GetImmediateContext(&ctx);
        SmaaCopy(ctx, back, images[index].texture);  // smaa1: plain copy, or SMAA when chosen
        ctx->Release();
        dev->Release();
        back->Release();
    }
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    p_xrReleaseSwapchainImage(chain, &ri);
    return true;
}

// Render-twice stereo: the game has just drawn the left eye into the back
// buffer and is about to draw the right one; keep the left image.
bool XrHoldLeftEye(const float q[4], const float p[3]) {
    if (g_quitting || g_stage != XrStage::Ready || !g_sessionRunning || !g_sessionChain ||
        !(g_sessionState == XR_SESSION_STATE_VISIBLE || g_sessionState == XR_SESSION_STATE_FOCUSED))
        return false;
    if (!CopyBackBuffer(g_sessionChain, g_swapchain, g_images)) return false;
    XrSetSubmitEye(0, q, p);
    g_eyePose[0] = g_pendingEyePose;
    g_eyeReady[0] = true;
    g_submitEye = -1;
    return true;
}

static void SubmitFrame(IDXGISwapChain *sc) {
    if (g_paired && g_submitEye == 0 && HeadtrackActive() && g_renderPoseValid) {
        // Left eye: hold it and let the game draw the right eye straight away.
        g_submitEye = -1;
        if (CopyBackBuffer(sc, g_swapchain, g_images)) {
            g_eyePose[0] = g_pendingEyePose;
            g_eyeReady[0] = true;
        }
        return;
    }

    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    LARGE_INTEGER w0, w1, qf;
    QueryPerformanceCounter(&w0);
    if (!Check(p_xrWaitFrame(g_session, nullptr, &fs), "xrWaitFrame")) return;
    QueryPerformanceCounter(&w1);
    QueryPerformanceFrequency(&qf);
    {
        // Pacing (judder hunt): display slots the runtime skipped between our
        // frames, and how long xrWaitFrame held us; logged every 900 frames.
        static XrTime lastDisplay;
        static LONG frames, skipped, late;
        static double waitSum, waitMax;
        double wms = double(w1.QuadPart - w0.QuadPart) * 1000.0 / qf.QuadPart;
        waitSum += wms;
        if (wms > waitMax) waitMax = wms;
        if (lastDisplay && fs.predictedDisplayPeriod > 0) {
            double slots = double(fs.predictedDisplayTime - lastDisplay) / double(fs.predictedDisplayPeriod);
            if (slots > 1.5) ++late, skipped += (LONG)(slots - 0.5) - 1;
        }
        lastDisplay = fs.predictedDisplayTime;
        if (++frames == 900) {
            LogV("XR: pacing over 900 frames: %ld times a display slot was skipped (%ld slots), xrWaitFrame avg %.2f ms "
                "max %.2f ms, display period %.2f ms",
                late, skipped, waitSum / frames, waitMax, fs.predictedDisplayPeriod / 1e6);
            frames = skipped = late = 0, waitSum = waitMax = 0;
        }
    }
    if (!Check(p_xrBeginFrame(g_session, nullptr), "xrBeginFrame")) return;
    g_nextDisplayTime = fs.predictedDisplayTime + fs.predictedDisplayPeriod;
    if (!g_headsetFovKnown) LearnHeadsetViews(fs.predictedDisplayTime);

    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad menuQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};  // flat1
    XrCompositionLayerQuad scanQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};  // cybereye1
    const XrCompositionLayerBaseHeader *layers[3] = {(XrCompositionLayerBaseHeader *)&quad, (XrCompositionLayerBaseHeader *)&menuQuad, nullptr};
    XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO};
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    const bool flat = EngineFlat();  // flat1
    if (flat != g_flatWas) {
        g_flatWas = flat;
        g_eyeReady[0] = g_eyeReady[1] = false;  // back in VR: no old eye images
        g_flatRecGen = HeadtrackRecentreGen() - 1;  // place the screen now
    }
    if (flat && g_flatRecGen != HeadtrackRecentreGen()) {
        // In front of where you look (level), at head height; again after a recentre.
        g_flatRecGen = HeadtrackRecentreGen();
        const XrQuaternionf &q = g_renderPose.orientation;
        float fx = -2 * (q.x * q.z + q.y * q.w), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        g_scrYaw = g_renderPoseValid && fx * fx + fz * fz > 1e-4f ? atan2f(-fx, -fz) : 0.0f;
        g_scrPos[0] = g_renderPose.position.x, g_scrPos[1] = g_renderPose.position.y, g_scrPos[2] = g_renderPose.position.z;
        Log("XR: 2D cutscene screen placed (%.1f m ahead, %.1f m wide)", g_cutScreen[0], g_cutScreen[1]);
    }
    int eye = !flat && HeadtrackActive() && g_renderPoseValid ? g_submitEye : -1;
    g_submitEye = -1;
    XrSwapchain chain = eye == 1 ? g_swapchainR : g_swapchain;
    XrSwapchainImageD3D11KHR *images = eye == 1 ? g_imagesR : g_images;
    if (fs.shouldRender) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (Check(p_xrAcquireSwapchainImage(chain, &ai, &index), "acquire") &&
            Check(p_xrWaitSwapchainImage(chain, &wi), "wait image")) {
            ID3D11Texture2D *back = nullptr;
            bool menuLayer = false, scanLayer = false;
            const bool cyber = flat && CyberEyeOn();  // cybereye1
            if (SUCCEEDED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void **)&back))) {
                ID3D11Device *dev = nullptr;
                ID3D11DeviceContext *ctx = nullptr;
                back->GetDevice(&dev);
                dev->GetImmediateContext(&ctx);
                SmaaCopy(ctx, back, images[index].texture);  // smaa1: plain copy, or SMAA when chosen
                if (flat) menuLayer = MenuLayerCopy(ctx);  // flat1
                if (cyber) scanLayer = ScanLayer(ctx);
                ctx->Release();
                dev->Release();
                back->Release();
            }
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(chain, &ri);

            if (flat) {
                // flat1: the cutscene on its screen (the eye image's shape), the menu in front.
                float d = g_cutScreen[0] < 0.5f ? 0.5f : g_cutScreen[0], w = g_cutScreen[1] < 0.2f ? 0.2f : g_cutScreen[1];
                float sy = sinf(g_scrYaw), cy = cosf(g_scrYaw);
                quad.space = g_space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g_swapchain;
                // movie1: a 16:9 movie screen = the middle band of the picture (engine.cpp FlatCamera
                // widens the view to suit). Nothing else is sent, so all around it is black.
                int32_t bandH = (int32_t)g_height;
                if (g_cutWide && g_width * 9 < g_height * 16) bandH = (int32_t)(g_width * 9 / 16) & ~1;
                quad.subImage.imageRect.offset = {0, ((int32_t)g_height - bandH) / 2};
                quad.subImage.imageRect.extent = {(int32_t)g_width, bandH};
                quad.pose.orientation = {0, sinf(g_scrYaw * 0.5f), 0, cosf(g_scrYaw * 0.5f)};
                quad.pose.position = {g_scrPos[0] - sy * d, g_scrPos[1] + g_cutScreen[2], g_scrPos[2] - cy * d};
                if (cyber) {
                    // cybereye1: a smaller screen that stays in front of the eyes (the picture itself
                    // follows the head, smoothed), with scan lines over it.
                    CyberEyeScreen(&w, &d);
                    quad.space = g_viewSpace;
                    quad.pose.orientation = {0, 0, 0, 1};
                    quad.pose.position = {0, 0, -d};
                    // cyberlevel1 (DrGonzaga 15 Nov: "rotate with head as well, so horizon stays level"): the screen is
                    // turned about the line of sight against the head's tilt, so the picture's horizon stays level.
                    if (*CyberEyeLevel() && g_renderPoseValid) {
                        const XrQuaternionf &q = g_renderPose.orientation;
                        float ry = 2 * (q.x * q.y + q.w * q.z), uy = 1 - 2 * (q.x * q.x + q.z * q.z);
                        float a = -atan2f(ry, uy);
                        quad.pose.orientation = {0, 0, sinf(a * 0.5f), cosf(a * 0.5f)};
                    }
                }
                quad.size.width = w;
                quad.size.height = w * bandH / g_width;
                fe.layerCount = 1;
                if (scanLayer) {
                    scanQuad = quad;
                    scanQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    scanQuad.subImage.swapchain = g_scanChain;
                    scanQuad.subImage.imageRect.offset = {0, 0};
                    scanQuad.subImage.imageRect.extent = {kScanW, kScanH};
                    layers[1] = (XrCompositionLayerBaseHeader *)&scanQuad;
                    fe.layerCount = 2;
                    if (menuLayer) layers[2] = (XrCompositionLayerBaseHeader *)&menuQuad;
                }
                if (menuLayer) {
                    menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    menuQuad.space = g_space;
                    menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    menuQuad.subImage.swapchain = g_menuChain;
                    D3D11_TEXTURE2D_DESC td;
                    g_menuTex->GetDesc(&td);
                    menuQuad.subImage.imageRect.extent = {(int32_t)td.Width, (int32_t)td.Height};
                    menuQuad.pose = g_menuPose;
                    menuQuad.size.width = g_menuW, menuQuad.size.height = g_menuH;
                    ++fe.layerCount;
                }
                fe.layers = layers;
                goto done;
            }
            if (eye >= 0) {
                g_eyePose[eye] = g_pendingEyePose;
                g_eyeReady[eye] = true;
            }

            if (eye >= 0 && g_eyeReady[0] && g_eyeReady[1]) {
                // Stereo: each eye shows its latest image at the pose it was rendered from.
                // scope1: while the right eye is the scope view, both eyes see the left image.
                bool mono = EngineScopeMono();  // pip1: not with the scope picture (both eyes are real)
                for (int e = 0; e < 2; ++e) {
                    XrCompositionLayerProjectionView &v = views[e];
                    v.pose = g_eyePose[mono ? 0 : e];
                    FovOf(&v.fov, mono ? 0 : e);  // oc1: the view each image was drawn with
                    v.subImage.swapchain = e == 1 && !mono ? g_swapchainR : g_swapchain;
                    v.subImage.imageRect.extent = {(int32_t)g_width, (int32_t)g_height};
                }
                proj.space = g_space;
                proj.viewCount = 2;
                proj.views = views;
                layers[0] = (XrCompositionLayerBaseHeader *)&proj;
                fe.layerCount = 1;
                fe.layers = layers;
                goto done;
            }

            if (eye >= 0) {
                // flat1: one eye fresh, the other not yet (first frame back from the
                // 2D screen): both eyes see this one for that frame.
                for (auto &v : views) {
                    v.pose = g_eyePose[eye];
                    FovOf(&v.fov, eye);
                    v.subImage.swapchain = chain;
                    v.subImage.imageRect.extent = {(int32_t)g_width, (int32_t)g_height};
                }
                proj.space = g_space;
                proj.viewCount = 2;
                proj.views = views;
                layers[0] = (XrCompositionLayerBaseHeader *)&proj;
                fe.layerCount = 1;
                fe.layers = layers;
                goto done;
            }

            // movie2 (DrGonzaga 17 Nov: "the cutscene doesn't" go 2D): a story video outside a level (the HUD panel isn't
            // drawn there, so movie1 never saw it) goes on the plain screen below, shaped 16:9 (the game stretches the
            // video over the whole picture).
            const bool movieOut = !EngineActive() && MoviePlaying();
            if (eye < 0 && HeadtrackActive() && g_renderPoseValid && !movieOut) {
                // Head tracking: the image was rendered from the head pose, so
                // show it as a view from that pose (both eyes see the same
                // image until stereo), and the runtime corrects for latency.
                for (auto &v : views) {
                    v.pose = g_renderPose;
                    v.fov.angleLeft = -atanf(g_tanX);
                    v.fov.angleRight = atanf(g_tanX);
                    v.fov.angleUp = atanf(g_tanY);
                    v.fov.angleDown = -atanf(g_tanY);
                    v.subImage.swapchain = chain;
                    v.subImage.imageRect.extent = {(int32_t)g_width, (int32_t)g_height};
                }
                proj.space = g_space;
                proj.viewCount = 2;
                proj.views = views;
                layers[0] = (XrCompositionLayerBaseHeader *)&proj;
                fe.layerCount = 1;
                fe.layers = layers;
                goto done;
            }
            quad.space = g_space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = chain;
            quad.subImage.imageRect.extent = {(int32_t)g_width, (int32_t)g_height};
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.z = -kScreenDistance;
            quad.size.width = kScreenWidth;
            quad.size.height = movieOut ? kScreenWidth * 9.0f / 16.0f : kScreenWidth * g_height / g_width;  // movie2
            fe.layerCount = 1;
            fe.layers = layers;
        }
    }
done:
    Check(p_xrEndFrame(g_session, &fe), "xrEndFrame");

    LONG n = InterlockedIncrement(&g_xrFrames);
    if (n == 1 || n % 900 == 0) LogV("XR: submitted %ld frames to the headset", n);
}

// Quitting (quit1). On quit the game leaves its main loop and tears the
// engine down (0x117b5ec0, called from RunGame at 0x117b7e8c) without ever
// releasing its swapchain, so our headset session was still open while the
// D3D objects it uses were freed: the headset runtime's own thread then
// called into freed memory (0xdededede) a few seconds later. We now close
// the session before that teardown starts.
static CRITICAL_SECTION g_presentLock;


static void LockInit() {
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) InitializeCriticalSection(&g_presentLock), once = 2;
    while (once != 2) Sleep(0);
}

// quit3 (11 Nov): the close call comes on the game's main thread while its
// render thread can still be in the middle of a frame. Closing the session
// there freed headset images and our own D3D objects under the render thread
// and used the D3D context from two threads at once: now and then the graphics
// driver's worker thread crashed in that same millisecond (spot5 18:24 and
// review2 19:00, nvwgf2um). So the main thread now only asks, and the render
// thread does the closing at its next Present (between frames, where it is the
// only user). If the render thread has already stopped, or doesn't answer in
// half a second, the main thread closes as before.
static volatile LONG g_closeReq;       // 1 = main thread is waiting for the render thread to close
static HANDLE g_closeDone;
static volatile DWORD g_lastPresentTick, g_presentThread;

static void CloseForQuit(const char *who) {
    g_quitting = true;
    EnterCriticalSection(&g_presentLock);
    Log("XR: the game is closing its engine; closing the headset session first (%s)", who);
    if (g_sessionChain) g_closedChain = g_sessionChain;
    if (g_stage == XrStage::Ready) DestroySession();
    LaserOnSwapChainGoing();
    ScopeOnSwapChainGoing();
    HudOnSwapChainGoing();
    MenuOnSwapChainGoing();
    g_stage = XrStage::NotStarted;  // a restarted engine gets a new session on its first Present
    LeaveCriticalSection(&g_presentLock);
    g_quitting = false;
}

extern "C" void __cdecl XrBeforeEngineClose() {
    LockInit();
    IniFlush();  // settings1: any setting still waiting to be written
    if (!g_closeDone) g_closeDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool renderAlive = g_presentThread && g_presentThread != GetCurrentThreadId() &&
                       GetTickCount() - g_lastPresentTick < 250;
    if (g_closeDone && renderAlive) {
        ResetEvent(g_closeDone);
        InterlockedExchange(&g_closeReq, 1);
        // ptr2: DrGonzaga's first quits all ended "render thread did not answer": by the time
        // the game calls this, its render thread is finishing its last frame and does not
        // present again. So in practice this wait is a pause that lets that frame finish
        // before the main thread closes (the crash was in the same millisecond as the old
        // immediate close). A quarter of a second covers many frames.
        if (WaitForSingleObject(g_closeDone, 250) == WAIT_OBJECT_0) return;
        // no answer: take the request back, unless the render thread has just picked it up
        if (InterlockedCompareExchange(&g_closeReq, 0, 1) != 1) {
            WaitForSingleObject(g_closeDone, 2000);
            return;
        }
        CloseForQuit("main thread, render thread did not answer");
        return;
    }
    CloseForQuit("main thread, render thread idle");
}

extern "C" {
void *g_closeFn;
void CloseStub();
}
asm(".globl _CloseStub\n"
    "_CloseStub:\n"
    "    pushal\n"
    "    call _XrBeforeEngineClose\n"
    "    popal\n"
    "    jmp *_g_closeFn\n");

// quitbd1: Blood Dragon. 0x100037bc runs the main loop (0x10659907) and then calls the engine
// teardown 0x1000372c at 0x100037d0 (FC3: 0x117b7e8c calling 0x117b5ec0).
void XrQuitHookInstallBd(BYTE *bd) {
    BYTE *call = bd + 0x37d0, *fn = bd + 0x372c, *loop = bd + 0x37cb;
    if (IsBadReadPtr(loop, 10) || call[0] != 0xe8 || call + 5 + *(int *)(call + 1) != fn || loop[0] != 0xe8 ||
        loop + 5 + *(int *)(loop + 1) != bd + 0x659907) {
        Log("XR: Blood Dragon's engine close call not where expected; quit hook off");
        return;
    }
    LockInit();
    g_closeFn = fn;
    PatchCallTo(call, (void *)CloseStub);
    Log("XR: quit hook installed (BD+100037d0)");
}

static void OnPresent(IDXGISwapChain *sc);

void XrOnPresent(IDXGISwapChain *sc) {
    g_presentThread = GetCurrentThreadId(), g_lastPresentTick = GetTickCount();
    if (g_closeReq && InterlockedCompareExchange(&g_closeReq, 0, 1) == 1) {  // quit3
        CloseForQuit("render thread");
        SetEvent(g_closeDone);
        return;
    }
    if (g_quitting) return;
    LockInit();
    EnterCriticalSection(&g_presentLock);
    OnPresent(sc);
    LeaveCriticalSection(&g_presentLock);
}

static void OnPresent(IDXGISwapChain *sc) {
    if (g_stage == XrStage::Ready && sc != g_sessionChain) XrOnSwapChainGoing(g_sessionChain, "replaced");
    // quit2: the game still presents a frame or two while it shuts down; opening a
    // new headset session then failed and the runtime crashed (trim2, 1 Nov).
    if (g_stage == XrStage::NotStarted && sc == g_closedChain) return;
    if (g_stage == XrStage::NotStarted) g_closedChain = nullptr, Init(sc);
    if (g_stage == XrStage::Failed) return;
    PollEvents();
    if (g_sessionRunning) ControlsUpdate(), (g_controlsAttached && g_sessionState == XR_SESSION_STATE_FOCUSED ? HapticsUpdate() : (void)0);
    if (g_sessionRunning && (g_sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
                             g_sessionState == XR_SESSION_STATE_VISIBLE ||
                             g_sessionState == XR_SESSION_STATE_FOCUSED || g_sessionState == XR_SESSION_STATE_READY))
        SubmitFrame(sc);
}

bool XrGetHeadOrientation(float q[4]) {
    if (g_stage != XrStage::Ready || !g_sessionRunning || !g_viewSpace || !g_nextDisplayTime) return false;
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(p_xrLocateSpace(g_viewSpace, g_space, g_nextDisplayTime, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    g_renderPose = loc.pose;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) g_renderPose.position = {0, 0, 0};
    g_renderPoseValid = true;
    q[0] = loc.pose.orientation.x, q[1] = loc.pose.orientation.y, q[2] = loc.pose.orientation.z,
    q[3] = loc.pose.orientation.w;
    return true;
}

void XrSetGameFov(float tanHalfX, float tanHalfY) {
    g_tanX = tanHalfX;
    g_tanY = tanHalfY;
}
// oc1: the view one eye's image is drawn with (eye < 0: both), as tangents.
void XrSetImageFov(int eye, float l, float r, float u, float d) {
    for (int e = 0; e < 2; ++e)
        if (eye < 0 || eye == e) g_imgFov[e][0] = l, g_imgFov[e][1] = r, g_imgFov[e][2] = u, g_imgFov[e][3] = d;
}
bool XrGetEyeTan(int eye, float t[4]) {
    if (!g_headsetFovKnown || eye < 0 || eye > 1) return false;
    memcpy(t, g_eyeTan[eye], 16);
    return true;
}

void XrGetLastPose(float q[4], float p[3]) {
    q[0] = g_renderPose.orientation.x, q[1] = g_renderPose.orientation.y, q[2] = g_renderPose.orientation.z,
    q[3] = g_renderPose.orientation.w;
    p[0] = g_renderPose.position.x, p[1] = g_renderPose.position.y, p[2] = g_renderPose.position.z;
}

void XrSetSubmitPose(const float q[4], const float p[3]) {
    g_renderPose.orientation = {q[0], q[1], q[2], q[3]};
    g_renderPose.position = {p[0], p[1], p[2]};
    g_renderPoseValid = true;
}

void XrSetSubmitEye(int eye, const float q[4], const float p[3]) {
    // Eye position: half the IPD along the head's own x axis.
    float x = (eye == 0 ? -0.5f : 0.5f) * g_ipd;
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float ex = x * (1 - 2 * (qy * qy + qz * qz)), ey = x * 2 * (qx * qy + qz * qw), ez = x * 2 * (qx * qz - qy * qw);
    g_pendingEyePose.orientation = {q[0], q[1], q[2], q[3]};
    g_pendingEyePose.position = {p[0] + ex, p[1] + ey, p[2] + ez};
    g_renderPose = g_pendingEyePose;
    g_renderPoseValid = true;
    g_submitEye = eye;
}

// mirror1: the headset is showing frames (the desktop window may then skip some).
bool XrSessionRunning() {
    return g_sessionRunning && (g_sessionState == XR_SESSION_STATE_VISIBLE || g_sessionState == XR_SESSION_STATE_FOCUSED);
}
