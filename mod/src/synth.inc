// synth1 (DrGonzaga 22 Nov: "a 'user friendly' menu, with graphical options, gameplay options, gun customization,
// comfort settings. Then a button down the bottom for debug menu. Operated with the controller ... in the
// synthwave style of the PNGs I've shown you"). Included by menu.cpp inside its namespace, just before Ui().
//
// The VR menu is what both stick clicks open now. Four pages (Graphics, VR, Guns, Controls) with only the
// everyday settings; "Debug menu" at the bottom opens the full tinkering menu (the old one, unchanged), which
// has a "Back to the VR menu" button. Drawn with ImGui into the same panel texture: dark purple with a
// perspective grid and a striped sun, neon cyan and pink outlines with a glow, a rounded futuristic font
// (bin\bdvr-pictures\menu-font.ttf; Windows' Bahnschrift if that file is missing; ImGui's own last).
// Operated by pointing with the gun hand's controller and pulling its trigger (as before), or with the
// walking stick to move between items and A to press; the other stick scrolls; B closes.
// The Controls page shows DrGonzaga's two diagrams (controls-controllers.png, controls-body.png) big on the
// panel beside the menu; with left-handed mode on, controls-*-left.png is used where it exists.

ImFont *g_fBody, *g_fSmall, *g_fSect, *g_fTitle, *g_fRow;  // ctl4: g_fRow = Controls rows, 30% smaller
float g_synthScroll;  // pixels to scroll the page this frame (the other stick)
bool g_synth = true;  // the VR menu (true) or the debug menu (false)
int g_sPage = 1;      // 0 graphics, 1 VR, 2 guns, 3 controls
int g_capRowK = -1, g_capRowO = -1;  // ctl2: the Controls row asking for a control
int g_ctlScreen = 0;  // Controls page: 0 controllers, 1 body, 2 tutorial (tut1)
const float kUiScale = 1 / 1.6f;  // undoes io.FontGlobalScale (set for the debug menu) for the VR menu's real-size fonts
const ImVec4 kCyan(0.25f, 0.95f, 1.0f, 1), kPink(1.0f, 0.28f, 0.82f, 1), kPurple(0.62f, 0.38f, 1.0f, 1),
    kYellow(1.0f, 0.85f, 0.30f, 1), kWhite(0.95f, 0.93f, 1.0f, 1), kDim(0.70f, 0.66f, 0.85f, 1);

ImU32 U32(const ImVec4 &c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// Fonts: called from Setup() before the font texture is made. The first font stays ImGui's own (the debug menu's).
void SynthFonts() {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    wchar_t w[MAX_PATH];
    PicDir(w);
    wcscat(w, L"menu-font.ttf");
    char path[MAX_PATH * 2] = "";
    const char *src = "bdvr-pictures\\menu-font.ttf";
    if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) {
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        swprintf(w, MAX_PATH, L"%ls\\Fonts\\bahnschrift.ttf", win);
        src = "Windows' Bahnschrift";
        if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) w[0] = 0, src = "ImGui's own (no font file found)";
    }
    if (w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof path, nullptr, nullptr);
    ImFontConfig fc;
    fc.OversampleH = 2, fc.OversampleV = 1, fc.PixelSnapH = false;
    auto Add = [&](float px) -> ImFont * {
        ImFont *f = path[0] ? io.Fonts->AddFontFromFileTTF(path, px, &fc) : nullptr;
        if (!f) {
            ImFontConfig d;
            d.SizePixels = px;
            f = io.Fonts->AddFontDefault(&d);
        }
        return f;
    };
    g_fSmall = Add(22), g_fBody = Add(28), g_fSect = Add(34), g_fTitle = Add(58), g_fRow = Add(20);
    Log("menu: VR menu font: %s", src);
}

void GlowRect(ImDrawList *dl, ImVec2 a, ImVec2 b, const ImVec4 &c, float round, float strength) {
    for (int i = 3; i >= 1; --i) {
        float g = i * 3.5f;
        dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), U32(c, 0.09f * strength * (4 - i)), round + g, 0, 3.5f);
    }
    dl->AddRect(a, b, U32(c, fminf(1, 0.65f + 0.3f * strength)), round, 0, 2.5f);
}
void GlowText(ImDrawList *dl, ImFont *f, float size, ImVec2 p, const ImVec4 &c, const char *s, float glow = 1) {
    static const float o[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-1.5f, -1.5f}, {1.5f, 1.5f}, {-1.5f, 1.5f}, {1.5f, -1.5f}};
    if (glow > 0)
        for (auto &d : o) dl->AddText(f, size, ImVec2(p.x + d[0], p.y + d[1]), U32(c, 0.16f * glow), s);
    dl->AddText(f, size, p, U32(c, 1), s);
}
float FontPx(ImFont *f) { return f->FontSize * ImGui::GetIO().FontGlobalScale * kUiScale; }
ImVec2 TextSize(ImFont *f, const char *s) { return f->CalcTextSizeA(FontPx(f), FLT_MAX, 0, s); }

bool NeonButton(const char *label, ImVec2 size, const ImVec4 &c, bool sel = false, ImFont *f = nullptr) {
    if (!f) f = g_fBody;
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##nb", size);
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    float r = fminf(size.y * 0.4f, 22.f);
    if (sel || hov) dl->AddRectFilled(p, q, U32(c, sel ? 0.30f : 0.14f), r);
    GlowRect(dl, p, q, c, r, sel ? 1.5f : hov ? 1.1f : 0.6f);
    ImVec2 ts = TextSize(f, label);
    GlowText(dl, f, FontPx(f), ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), sel ? kWhite : c, label, sel || hov ? 1.2f : 0.6f);
    ImGui::PopID();
    return pressed;
}

void SectionHead(const char *s) {
    ImGui::Dummy(ImVec2(1, 10));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = TextSize(g_fSect, s);
    GlowText(dl, g_fSect, FontPx(g_fSect), p, kPink, s, 1.3f);
    float w = ImGui::GetContentRegionAvail().x, y = p.y + ts.y + 6;
    dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(p.x + w, y + 3), U32(kPink, 0.9f), U32(kCyan, 0.0f), U32(kCyan, 0.0f), U32(kPink, 0.9f));
    ImGui::Dummy(ImVec2(w, ts.y + 16));
}
void SNote(const char *fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushFont(g_fSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

const float kRowH = 54, kCtlW = 420;
// A row: label on the left, switch on the right; the whole row is the button.
bool NeonToggle(const char *label, bool *v) {
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool pressed = ImGui::InvisibleButton("##tg", ImVec2(w, kRowH));
    if (pressed) *v = !*v;
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + kRowH), U32(kPurple, 0.12f), 14);
    ImVec2 ts = TextSize(g_fBody, label);
    dl->AddText(g_fBody, FontPx(g_fBody), ImVec2(p.x + 10, p.y + (kRowH - ts.y) * 0.5f), U32(hov ? kCyan : kWhite, 1), label);
    const float sw = 96, sh = 42;
    ImVec2 a(p.x + w - sw - 12, p.y + (kRowH - sh) * 0.5f), b(a.x + sw, a.y + sh);
    const ImVec4 &c = *v ? kPink : kPurple;
    if (*v) dl->AddRectFilled(a, b, U32(kPink, 0.25f), sh * 0.5f);
    GlowRect(dl, a, b, c, sh * 0.5f, *v ? 1.3f : 0.5f);
    float kx = *v ? b.x - sh * 0.5f : a.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, a.y + sh * 0.5f), sh * 0.5f - 6, U32(*v ? kWhite : kDim, *v ? 1 : 0.6f));
    if (*v) dl->AddCircle(ImVec2(kx, a.y + sh * 0.5f), sh * 0.5f - 2, U32(kPink, 0.7f), 0, 3);
    ImGui::PopID();
    return pressed;
}

// Label left, then  <  value  >  on the right. Pressing the value steps forward too.
bool NeonPicker(const char *label, int *v, const char *const *names, int n) {
    ImGui::PushID(label);
    bool ch = false;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 ts = TextSize(g_fBody, label);
    dl->AddText(g_fBody, FontPx(g_fBody), ImVec2(p.x + 10, p.y + (kRowH - ts.y) * 0.5f), U32(kWhite, 1), label);
    float x0 = p.x + w - kCtlW - 12, bw = 58;
    ImGui::SetCursorScreenPos(ImVec2(x0, p.y + 3));
    if (NeonButton("<", ImVec2(bw, kRowH - 6), kCyan)) *v = (*v + n - 1) % n, ch = true;
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw + 8, p.y + 3));
    float vw = kCtlW - 2 * bw - 16;
    if (ImGui::InvisibleButton("##val", ImVec2(vw, kRowH - 6))) *v = (*v + 1) % n, ch = true;
    bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (*v < 0 || *v >= n) *v = 0;
    const char *s = names[*v];
    ImFont *f = TextSize(g_fBody, s).x > vw - 10 ? g_fSmall : g_fBody;
    ImVec2 vs = TextSize(f, s);
    GlowText(dl, f, FontPx(f), ImVec2(x0 + bw + 8 + (vw - vs.x) * 0.5f, p.y + (kRowH - vs.y) * 0.5f), hov ? kCyan : kPink, s, 1);
    ImGui::SetCursorScreenPos(ImVec2(x0 + bw + 8 + vw + 8, p.y + 3));
    if (NeonButton(">", ImVec2(bw, kRowH - 6), kCyan)) *v = (*v + 1) % n, ch = true;
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, kRowH));
    ImGui::PopID();
    return ch;
}

// Label left, slider right. Returns true when a change is finished (let go), for saving; the value changes live.
bool NeonSlider(const char *label, float *v, float lo, float hi, const char *fmt) {
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 ts = TextSize(g_fBody, label);
    ImGui::GetWindowDrawList()->AddText(g_fBody, FontPx(g_fBody), ImVec2(p.x + 10, p.y + (kRowH - ts.y) * 0.5f), U32(kWhite, 1), label);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - kCtlW - 12, p.y + 3));
    ImGui::SetNextItemWidth(kCtlW);
    ImGui::SliderFloat("##sl", v, lo, hi, fmt);
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, kRowH));
    ImGui::PopID();
    return done;
}
// A percentage slider over a 0-1 value.
bool NeonPercent(const char *label, float *v, float lo = 0, float hi = 1) {
    float pc = *v * 100;
    bool done = NeonSlider(label, &pc, lo * 100, hi * 100, "%.0f%%");
    *v = pc / 100;
    return done;
}

// ---------------- pages ----------------

void SGraphics() {
    SectionHead("RESOLUTION");
    {
        static UINT cw, ch, rw, rh;
        static float want;
        if (!cw) ResGetSize(&cw, &ch);
        if (!rw && !XrRecommendedSize(&rw, &rh)) rw = 0;
        if (rw && cw) {
            if (want == 0) want = roundf((float)cw / rw * 20) / 20;
            char fmt[48];
            UINT ww = (UINT)(rw * want + 8) / 16 * 16, wh = (UINT)(rh * want + 8) / 16 * 16;
            snprintf(fmt, sizeof fmt, "x%%.2f  (%u x %u)", ww, wh);
            NeonSlider("Render scale", &want, 0.5f, 2.0f, fmt);
            want = roundf(want * 20) / 20;
            SNote("Now %u x %u per eye%s. x1.00 = the headset's recommended %u x %u. Higher is sharper and costs GPU time.",
                  cw, ch, ResAutoSize() ? " (automatic, from the headset)" : "", rw, rh);
            if (ww != cw || wh != ch) {  // bd123: "Apply now" taken off (DrGonzaga: it doesn't work)
                if (NeonButton("Save for next start", ImVec2(330, 56), kCyan)) ResSetSize(ww, wh, false);
            }
            // autores1: back to automatic (the headset's recommended size at every start)
            static bool s_autoAsked;
            if (!ResAutoSize() && !s_autoAsked && NeonButton("Automatic size from the headset", ImVec2(460, 56), kCyan))
                ResUseAutoSize(), s_autoAsked = true;
            if (s_autoAsked) SNote("Automatic from the next start.");
        } else
            SNote("Waiting for the headset's recommended size.");
    }
    SectionHead("LIVE GRAPHICS");
    if (GfxBloomAvailable()) {
        bool b = GfxBloomOn();
        if (NeonToggle("Bloom (glow round lights)", &b)) GfxSetBloom(b);
    }
    static const char *const kShort[] = {"Post effects", "Fog", "Haze"};
    for (int k = 0; k < 3; ++k) {
        int i = GfxVarFind(kLive[k].var);
        if (i < 0 || GfxVarType(i) != 1) continue;
        bool v = GfxVarBool(i) != kLive[k].inverted;
        if (NeonToggle(kShort[k], &v)) GfxVarSetBool(i, v != kLive[k].inverted);
    }
    {
        static const char *const q[] = {"Off", "Low", "Medium", "High", "Ultra"};
        if (NeonPicker("Smoothing (SMAA)", &g_smaa, q, 5)) SaveF(L"render", L"SMAA", (float)g_smaa);
    }
    // bd123: "Detail distance" taken off (DrGonzaga: it doesn't work); still on the debug menu's Graphics page
    SectionHead("GAME SETTINGS");
    SNote("Set these here rather than in the game's own options: changing them there drops the resolution. "
          "They are written to the game's profile and take effect next time the game starts.");
    for (int i = 0; i < kProfileCount; ++i) {
        const auto &s = kProfile[i];
        char cur[16];
        GfxProfileChosen(i, cur, 16);
        int sel = 0;
        for (int k = 0; k < s.count; ++k)
            if (!strcmp(s.values[k], cur)) sel = k;
        const char *lab = s.label;
        static const char *const kShortName[] = {"Anti-aliasing (MSAA)", "Foliage edges", "Ambient occlusion", "Post effects quality",
                                                 "Shadows", "Water", "Vegetation", "Geometry"};
        if (i < 8) lab = kShortName[i];
        if (NeonPicker(lab, &sel, s.names, s.count)) GfxProfileChoose(i, s.values[sel]);
    }
    bool restart = false;
    for (int i = 0; i < kProfileCount; ++i) {
        char cur[16];
        GfxProfileChosen(i, cur, 16);
        restart |= strcmp(cur, GfxProfileNow(i)) != 0;
    }
    if (restart) {
        ImGui::PushFont(g_fBody);
        ImGui::TextColored(kYellow, "Restart the game to see the changed game settings.");
        ImGui::PopFont();
    }
}

void SVr() {
    SectionHead("WORLD SCALE");  // wscale1 (DrGonzaga 24 Nov: "add a world scale slider to the VR menu")
    {
        static float s_ws = -1;
        static bool s_drag;
        if (!s_drag) s_ws = EngineWorldScale();  // follows Numpad + / - too
        const bool done = NeonSlider("World scale", &s_ws, 0.5f, 2.0f, "x%.2f");
        s_drag = ImGui::IsItemActive();
        s_ws = roundf(s_ws * 100) / 100;
        if (done) EngineSetWorldScale(s_ws);
        SNote("How big the world looks. Above x1.00 the world looks bigger (you feel smaller), below x1.00 it looks smaller (you feel taller). "
              "It changes when you let go of the slider.");
        if (fabsf(EngineWorldScale() - 1) > 0.004f && NeonButton("Back to x1.00", ImVec2(300, 54), kCyan)) EngineSetWorldScale(1);
    }
    SectionHead("TURNING");
    {
        int t = *EngineSnapTurn() ? 1 : 0;
        static const char *const kT[] = {"Smooth", "Snap"};
        if (NeonPicker("Turn with the stick", &t, kT, 2)) *EngineSnapTurn() = t == 1, SaveF(L"vr", L"SnapTurn", (float)t);
        if (*EngineSnapTurn()) {
            static const char *const names[] = {"15 deg", "22.5 deg", "30 deg", "45 deg", "60 deg", "90 deg"};
            static const float val[] = {15, 22.5f, 30, 45, 60, 90};
            int cur = 3;
            for (int i = 0; i < 6; ++i)
                if (fabsf(val[i] - *EngineSnapAngle()) < 0.1f) cur = i;
            if (NeonPicker("Snap angle", &cur, names, 6)) *EngineSnapAngle() = val[cur], SaveF(L"vr", L"SnapTurnAngle", val[cur]);
        } else if (NeonSlider("Smooth turn speed", EngineTurnSpeed(), 45, 360, "%.0f deg/s")) {  // turnspd1 (beta feedback 23 Nov)
            *EngineTurnSpeed() = roundf(*EngineTurnSpeed() / 5) * 5;
            SaveF(L"vr", L"SmoothTurnSpeed", *EngineTurnSpeed());
        }
    }
    if (NeonButton("Recentre view", ImVec2(300, 54), kCyan)) HeadtrackRequestRecenter();
    SectionHead("WALKING");  // walkdir1 (DrGonzaga 23 Nov)
    {
        static const char *const kW[] = {"Where the gun aims", "Where the left hand points", "Where the head looks"};
        if (NeonPicker("Walk direction", InputWalkDir(), kW, 3)) Later(InputMapSave);
    }
    SectionHead("CROUCH");
    if (NeonToggle("Crouch for real to crouch in game", EngineRealCrouch())) SaveF(L"gameplay", L"RealCrouch", *EngineRealCrouch() ? 1.f : 0.f);
    if (NeonToggle("Stick down to crouch / stand", InputCrouchOn())) SaveF(L"aim", L"StickCrouch", *InputCrouchOn() ? 1.f : 0.f);
    SectionHead("MELEE");  // swingvr1 (beta feedback 23 Nov): the swing melee in the VR menu
    if (NeonToggle("Swing the gun hand to melee", InputSwingOn())) SaveF(L"aim", L"SwingMelee", *InputSwingOn() ? 1.f : 0.f);
    if (*InputSwingOn()) {
        if (NeonSlider("Swing speed needed", InputSwingSpeed(), 1.0f, 5.0f, "%.1f m/s"))
            *InputSwingSpeed() = roundf(*InputSwingSpeed() * 10) / 10, SaveF(L"aim", L"SwingSpeed", *InputSwingSpeed());
        SNote("Lower = a gentler swing hits. Higher = fewer hits by accident when you move the gun quickly. Clicking the stick also melees.");
    }
    // vol1 sound sliders removed (DrGonzaga 24 Nov: "Let's remove the volume controls (the music one doesn't work)")
    SectionHead("HOLSTERS");  // holsvr1 (DrGonzaga 23 Nov): holster height and width in the VR menu
    {
        bool hs = false;
        auto cm = [&](const char *label, int i, float lo, float hi) {
            float v = *HolsterValue(i) * 100, was = v;
            if (NeonSlider(label, &v, lo, hi, "%.0f cm")) hs = true;
            if (v != was) *HolsterValue(i) = roundf(v) / 100;  // untouched values keep their exact ini number
        };
        cm("Hip holsters: below your eyes", 0, 30, 110);
        cm("Hip holsters: out to the side", 1, 5, 50);
        cm("Shoulder holsters: reach down to", 5, 10, 90);
        SNote("Smaller numbers below your eyes = higher holsters. Hip width is measured from the middle of your body. "
              "The spheres move as you slide.");
        if (hs) Later(HolsterSave);
    }
    SectionHead("MINIMAP AND SLOW MOTION CLOCK");  // ctl4 (DrGonzaga 24 Nov): instead of the chest throwables' height
    {
        HudPanelCfg *mp = HudPanel(0);
        float v = mp->off[1] * 100;
        if (NeonSlider("Minimap height", &v, -80, 60, "%.0f cm")) Later(HudSave);
        mp->off[1] = roundf(v) / 100;
        if (g_bloodDragon) {
            float c = *SlowmoButton(1) * 100;
            if (NeonSlider("Slow motion clock above the minimap", &c, -40, 40, "%.0f cm")) Later(SlowmoSave);
            *SlowmoButton(1) = roundf(c) / 100;
        }
        SNote("The clock moves with the minimap; its own slider sets how far above it sits (below = minus).");
    }
    if (*HolsterWristOpt(0)) {  // wristvr1 (beta feedback 23 Nov: a player struggled to get cyber hearts out)
        SectionHead("WRIST MENU");
        bool ws = false;
        float *pu = HolsterWristValue(9);
        ws |= NeonSlider("Shows with the palm up within", pu, 15, 180, *pu >= 179.5f ? "any way" : "%.0f deg");
        float cm = *HolsterWristValue(6) * 100;
        if (NeonSlider("Icon size (bigger = easier to grab)", &cm, 1, 6, "%.1f cm")) ws = true;
        *HolsterWristValue(6) = cm / 100;
        SNote("Turn your off-hand palm up and look at it to see the throwables and the cyber heart; the gun hand's grip takes one. "
              "A wider angle shows the menu with less wrist turn; 180 = any way up.");
        if (ws) Later(HolsterSave);
    }
    SectionHead("DAMAGE");  // dmgbd1 (DrGonzaga 22 Nov): the damage slider, now working in Blood Dragon
    if (NeonSlider("Damage I take", &g_damageTaken, 0.5f, 4.0f, "%.2fx"))
        g_damageTaken = roundf(g_damageTaken * 20) / 20, SaveF(L"gameplay", L"DamageTaken", g_damageTaken);
    SNote("1.00x = the game's normal. Enemies still take normal damage.");
    SectionHead("COMFORT VIGNETTE");
    {
        ComfortCfg *c = Comfort();
        bool cs = false;
        cs |= NeonToggle("Comfort vignette", &c->on);
        static const char *const kW[] = {"Always", "Moving", "Turning", "Moving or turning"};
        cs |= NeonPicker("Shows", &c->mode, kW, 4);
        cs |= NeonPercent("Strength", &c->strength);
        cs |= NeonPercent("Size", &c->size, 0.05f, 0.95f);
        cs |= NeonSlider("Fade in", &c->fadeIn, 0, 1, "%.2f s");
        cs |= NeonSlider("Fade out", &c->fadeOut, 0, 1, "%.2f s");
        NeonToggle("Preview it now (not saved)", &c->preview);
        SNote("Darkens the edge of your view while you move with the stick or turn, which helps with motion sickness. "
              "Vehicles count as moving. Size = how far in it reaches.");
        if (cs) Later(ComfortSave);
    }
    if (g_bloodDragon) {
        SectionHead("SLOW MOTION");
        bool hs = false;
        hs |= NeonToggle("Slow motion clock", SlowmoOn());
        hs |= NeonSlider("Time active", SlowmoSeconds(), 0, 60, *SlowmoSeconds() < 0.5f ? "until touched again" : "%.0f s");
        hs |= NeonPercent("Game speed while on", SlowmoSpeed(), 0.05f, 1);
        SlowFxCfg *f = SlowFx();
        bool fx = f->push || f->vig || f->scan;
        if (NeonToggle("Screen effect while on", &fx)) {
            if (fx) f->push = f->vig = true;
            else f->push = f->vig = f->scan = false;
            hs = true;
        }
        if (NeonToggle("Sound effect while on", &f->sound)) hs = true;
        SNote("Fine tuning of the effects, the clock's place and the heartbeat: Debug menu, Guns > Gun & body > wrist menu.");
        if (hs) Later(SlowmoSave);
    }
}

bool SameGun(const wchar_t *a, const char *b) {
    char n[96];
    WideCharToMultiByte(CP_UTF8, 0, a, -1, n, sizeof n, nullptr, nullptr);
    return b[0] && !_stricmp(n, b);
}

void SGunsTwoHanded() {  // ctl4: the VR page's two-handed and gunstock angle sections, on the Guns page now
    SectionHead("BOW");  // bowsteady1 (DrGonzaga 24 Nov)
    if (NeonPercent("Steadying while holding the left trigger", BowSteadySmoothing()))
        SaveF(L"bow", L"SteadySmoothing", *BowSteadySmoothing());
    SNote("With the bow, hold the left trigger to steady your aim (higher = steadier but slower to follow). 0%% = off.");
    SectionHead("TWO-HANDED GUNS");  // ctl4: moved here from the VR page (DrGonzaga 24 Nov)
    {
        int m = *TwoHandStockAim() ? 0 : 1;
        static const char *const kM[] = {"Gunstock", "Two-handed"};
        if (NeonPicker("Aiming mode", &m, kM, 2)) *TwoHandStockAim() = m == 0, SaveF(L"twohand", L"StockAim", m == 0 ? 1.f : 0.f);
        SNote(m == 0 ? "Gunstock: the gun points where your gun hand points; the other hand steadies it."
                     : "Two-handed: the gun points along the line between your hands (no stock).");
        if (m == 0) {  // mount1 (DrGonzaga 24 Nov): the stock mount angle, turned about the cheek point
            SectionHead("GUNSTOCK ANGLE");
            bool ms = false;
            float *a = MountAngle(0), *b = MountAngle(1);
            ms |= NeonSlider("Gun front up / down", a, -30, 30, "%.2f deg");
            ms |= NeonSlider("Gun front left / right", b, -30, 30, "%.2f deg");
            *a = roundf(*a * 4) / 4, *b = roundf(*b * 4) / 4;  // 0.25 deg steps
            if (NeonButton("Fine:  up -", ImVec2(170, 50), kCyan)) *a -= 0.25f, ms = true;
            ImGui::SameLine();
            if (NeonButton("up +", ImVec2(110, 50), kCyan)) *a += 0.25f, ms = true;
            ImGui::SameLine();
            if (NeonButton("left -", ImVec2(120, 50), kCyan)) *b -= 0.25f, ms = true;
            ImGui::SameLine();
            if (NeonButton("right +", ImVec2(130, 50), kCyan)) *b += 0.25f, ms = true;
            ImGui::SameLine();
            if (NeonButton("Back to 0", ImVec2(170, 50), kPink)) *a = *b = 0, ms = true;
            *a = *a < -30 ? -30 : *a > 30 ? 30 : *a, *b = *b < -30 ? -30 : *b > 30 ? 30 : *b;
            SNote("Turns every gun, its aim, beam, red dot, sight window and scope together, for a stock that holds the controller at an "
                  "angle. Each gun's own position and angle stay as you tuned them. 0 = as before.");
            float k[3];
            const char *gun = "";
            const int kind = MountCheekKind(k, &gun);
            if (kind == 2) SNote("Turns about your cheek point (learnt): %.0f cm behind, %.0f cm above the controller.", k[2] * 100, k[1] * 100);
            else if (kind == 1) SNote("Turns about your cheek point, taken from the %s's rifle scope: %.0f cm behind, %.0f cm above the controller.", gun, k[2] * 100, k[1] * 100);
            else SNote("No cheek point yet: turns about the controller. Learn it below so scopes stay at your eye.");
            const float left = MountCheekLearnLeft();
            if (left > 0) {
                SNote("Hold your normal aim with your cheek on the stock... %.0f", ceilf(left));
            } else {
                if (NeonButton("Learn my cheek point in 6 s", ImVec2(420, 54), kCyan)) MountCheekLearnIn(6);
                if (kind == 2) {
                    ImGui::SameLine();
                    if (NeonButton("Back to the default", ImVec2(330, 54), kCyan)) MountCheekDefault();
                }
                if (MountCheekMsg()[0]) SNote("%s", MountCheekMsg());
            }
            if (ms) Later(MountSave);
        }
    }
}

void SGuns() {
    SGunsTwoHanded();
    const char *gun = GunAttachName();
    SectionHead("THIS GUN");
    if (!gun[0]) {
        SNote("Take a gun in your hand: the settings here are saved for each gun (and each sight on it).");
        return;
    }
    ImGui::PushFont(g_fSect);
    ImGui::TextColored(kCyan, "%s", gun);
    ImGui::PopFont();
    SNote("Optic: %s. Everything below is for this gun; switch guns and open the menu again for another.", GunAttachOptic());
    bool bs = false;
    SectionHead("LASER AND RED DOT");
    bs |= NeonToggle("Laser beam", LaserShowBeam());
    bs |= NeonToggle("Red dot where it lands", LaserShowDot());
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 ts = TextSize(g_fBody, "Colour");
        ImGui::GetWindowDrawList()->AddText(g_fBody, FontPx(g_fBody), ImVec2(p0.x + 10, p0.y + (64 - ts.y) * 0.5f), U32(kWhite, 1), "Colour");
        static const ImVec4 sw[4] = {ImVec4(1, 0.15f, 0.15f, 1), ImVec4(0.2f, 1, 0.3f, 1), ImVec4(0.25f, 0.55f, 1, 1), ImVec4(1, 0.6f, 0.1f, 1)};
        static const char *const nm[4] = {"red", "green", "blue", "orange"};
        for (int i = 0; i < 4; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 12 - (4 - i) * 76.f, p0.y + 2));
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(nm[i], ImVec2(60, 60))) *LaserColour() = i, bs = true;
            bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused(), on = *LaserColour() == i;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 c(p.x + 30, p.y + 30);
            dl->AddCircleFilled(c, 20, U32(sw[i], 1));
            if (on || hov) {
                dl->AddCircle(c, 27, U32(on ? kWhite : kCyan, 1), 0, 3);
                dl->AddCircle(c, 31, U32(on ? kPink : kCyan, 0.4f), 0, 4);
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(w, 64));
    }
    SectionHead("RETICLE");
    bs |= NeonToggle("Reticle through the sight", LaserSightOn());
    {
        static const char *const rets[10] = {"Dot", "Holo ring", "Chevron", "Cross", "Triangle", "Bow pins",
                                             "Target lock", "Tri-prong", "Hex", "Cyber ring"};
        bs |= NeonPicker("Reticle", LaserSightReticle(), rets, 10);
    }
    bool ss = false;
    SectionHead("SCOPE");
    if (ScopeGun()[0] && SameGun(ScopeGun(), gun)) {
        int t = *EngineScopeGunOff() ? 2 : *ScopeRifle() ? 1 : 0;
        static const char *const kS[] = {"Floating lens", "Scope on the rifle", "No scope"};
        if (NeonPicker("Scope type", &t, kS, 3)) {
            *EngineScopeGunOff() = t == 2, bs = true;
            *ScopeRifle() = t == 1, ss = true;
        }
        if (t != 2) {
            ss |= NeonToggle("Square scope", ScopeSquare());
            static const char *const styles[] = {"MRAD (long range)", "Blue dot", "Green prism", "Black duplex",
                                                 "Amber chevron", "Green ring", "Ruby fine cross", "Cyan mil-dot",
                                                 "Neon brackets", "Cyber hex", "Tri-prong", "Scanner"};
            ss |= NeonPicker("Scope reticle", ScopeStyle(), styles, 12);
        }
    } else
        SNote("Scope choices show here once this gun's scope has been used (aim it once).");
    SNote("Lining up sights, beam length, scope eye box, recoil and the rest: Debug menu, Guns section.");
    if (bs) Later(GunAttachSaveBeam);
    if (ss) Later(ScopeSave);
}

int SynthPic() {  // the picture shown for the Controls screen picked
    if (g_ctlScreen == 2) return kPicTutorial;  // tut1
    int i = g_ctlScreen == 0 ? kPicControllers : kPicBody;
    if (*VrLeftHanded()) {
        int l = g_ctlScreen == 0 ? kPicControllersL : kPicBodyL;
        if (g_picSrv[l]) i = l;
    }
    return i;
}

void SControls() {
    SectionHead("CONTROLS");
    // pics1 (DrGonzaga 24 Nov: "remove the tutorial PNG, and the two controllers PNG for now (keep it with the body PNG pic")):
    // only the body picture; the controllers / tutorial buttons are gone (the files and code stay for later).
    g_ctlScreen = 1;
    int pic = SynthPic();
    g_picShow = pic;  // the big picture beside the menu
    ImGui::Dummy(ImVec2(1, 8));
    if (g_picSrv[pic] && g_picH[pic]) {
        float w = ImGui::GetContentRegionAvail().x - 20, h = w * g_picH[pic] / g_picW[pic];
        if (h > 430) h = 430, w = h * g_picW[pic] / g_picH[pic];
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)g_picSrv[pic], ImVec2(w, h));
        GlowRect(ImGui::GetWindowDrawList(), p, ImVec2(p.x + w, p.y + h), g_ctlScreen == 2 ? kPurple : g_ctlScreen ? kPink : kCyan, 6, 0.8f);
        SNote(g_picOn ? "Shown big on the panel beside the menu too." : "Big picture beside the menu is off (below).");
    } else if (g_picState)
        SNote("Reading the picture...");
    else
        SNote("%s is not in the game's bin\\bdvr-pictures folder yet (or could not be read).", kPicFile[pic]);
    if (NeonToggle("Big picture beside the menu", &g_picOn)) SaveF(L"menu", L"SidePicture", g_picOn ? 1.f : 0.f);
    {
        float cm = g_picHeight * 100;
        if (NeonSlider("Big picture height", &cm, 30, 120, "%.0f cm")) SaveF(L"menu", L"SidePictureHeight", cm / 100);
        g_picHeight = cm / 100;
    }
    if (NeonButton("Reload pictures", ImVec2(300, 54), kPurple)) g_picWant = true;
    SNote("Replace controls-body.png in bin\\bdvr-pictures and press Reload. "
          "For left-handed play, controls-body-left.png is used if it is there.");
    // remap1 (DrGonzaga 23 Nov) then ctl2 (DrGonzaga 23 Nov, controls revamp: "action -> control (group of actions) ... include
    // thumbsticks"): the actions by situation, each with the control that does it; "Change" then press the control.
    // The table underneath is still input.cpp's [buttons] map (Touch input -> pad output); a row is a pad output.
    bool ms = false;
    auto Busy = [](int kind, int out) { return InputCapturing() && g_capRowK == kind && g_capRowO == out; };  // this row is asking now
    // One row: action name left, its control(s) in the middle, "Change" right.
    auto Row = [&](const char *label, const char *bound, int kind, int out, bool fixed = false) {
        ImGui::PushID(label);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float lw = w - kCtlW - 24;
        ImFont *lf = g_fRow;  // ctl4 (DrGonzaga 24 Nov: text overlapped)
        ImVec2 ts = TextSize(lf, label);
        dl->AddText(lf, FontPx(lf), ImVec2(p.x + 10, p.y + (kRowH - ts.y) * 0.5f), U32(fixed ? kDim : kWhite, 1), label);
        bool busy = Busy(kind, out);
        char tmp[96];
        const char *v = busy ? tmp : bound[0] ? bound : "(none)";
        if (busy) snprintf(tmp, sizeof tmp, "Press a control... %.0f", ceilf(InputCaptureLeft()));
        bool clr = !fixed && kind % 10 == 0 && out > 0;  // ctl3: Clear on action rows
        float x0 = p.x + w - kCtlW - 12 - (clr ? 112 : 0), cw = fixed ? kCtlW : kCtlW - 150;
        ImFont *f = g_fRow;
        ImVec2 vs = TextSize(f, v);
        GlowText(dl, f, FontPx(f), ImVec2(x0 + (cw - vs.x) * 0.5f, p.y + (kRowH - vs.y) * 0.5f), busy ? kYellow : fixed ? kDim : kPink, v, fixed ? 0.3f : 1);
        if (!fixed) {
            ImGui::SetCursorScreenPos(ImVec2(x0 + cw + 8, p.y + 3));
            if (NeonButton(busy ? "Cancel" : "Change", ImVec2(142, kRowH - 6), busy ? kYellow : kCyan)) {
                if (busy) InputCaptureCancel();
                else g_capRowK = kind, g_capRowO = out, InputCaptureBegin(kind, out);
            }
            if (clr && !busy) {
                ImGui::SetCursorScreenPos(ImVec2(x0 + cw + 8 + 150, p.y + 3));
                if (NeonButton("Clear", ImVec2(104, kRowH - 6), kPurple)) InputClear(out, kind >= 10), ms = true;
            }
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(w, kRowH));
        ImGui::PopID();
    };
    char b1[160];
    auto Act = [&](const char *label, int out) { Row(label, InputBoundTo(out, b1, sizeof b1), 0, out); };
    auto VAct = [&](const char *label, int out) { Row(label, InputVehBoundTo(out, b1, sizeof b1), 10, out); };
    static const char *const kIn[] = {"A", "B", "X", "Y", "Left grip", "Right grip", "Left stick click", "Right stick click", "Menu",
                                      "Left grip + Menu", "Right grip + Menu", "Left trigger", "Right trigger", "Turning stick up",
                                      "X + Y together", "A + B together", "Turning stick down", "Left thumbrest", "Right thumbrest"};
    static const char *const kFace[] = {"A", "B", "X", "Y"};
    // pad outputs (input.cpp kOut*)
    enum { oA = 1, oB, oX, oY, oLB, oRB, oLT, oRT, oLS, oRS, oStart, oBack, oUp, oDown, oLeft, oRight };
    SectionHead("CONTROLS: ON FOOT");
    SNote("Press Change, let go of everything, then press the control you want (buttons, triggers, thumbstick clicks, R thumbstick "
          "up or down, thumbrests, X+Y, A+B). If that control had another job, the two swap. Jobs the game does with one button "
          "move together (you'll be told which). The grips can't be picked here: they steady your guns.");
    if (*InputCaptureMsg() && !InputCapturing()) SNote("%s", InputCaptureMsg());
    else if (InputCapturing()) SNote("%s", InputCaptureMsg());
    Row("Walk", *InputSwapSticks() ? "R thumbstick" : "L thumbstick", 0, 0, true);
    Row("Turn", *InputSwapSticks() ? "L thumbstick left / right" : "R thumbstick left / right", 0, 0, true);
    if (NeonToggle("Swap sticks (walk with the right, turn with the left)", InputSwapSticks())) ms = true;
    Act("Jump / climb", oA);
    Act("Crouch / stand", oB);
    Act("Sprint", oLS);
    Act("Use / loot / pilfer, reload, get in vehicles, turrets", oX);
    Act("Last weapon (tap) / heal (hold)", oY);
    Act("Melee / takedown", oRS);
    Act("Throw a D20 (aim with right controller)", oRight);
    Act("Throw a cyber heart (aim with right controller)", oLeft);
    Act("Cyber-eye", oUp);
    Act("D-pad down (game)", oDown);
    SectionHead("CONTROLS: WEAPONS");
    Act("Fire", oRT);
    Act("Steady scope or bow", oLT);
    {
        int ti = *InputThrowIn();
        Row("Throw the wrist grenade (aim with right controller)", ti >= 0 && ti < 19 ? kIn[ti] : "?", 1, 0);
        static const char *const kX[] = {"Throw the wrist grenade", "Minimap on / off", "Game map"};
        if (NeonPicker("That control with a gun in hand", InputXMode(), kX, 3)) ms = true;
    }
    Row("Scope zoom in (while holding LT)", kFace[*InputZoomBtn(0) & 3], 2, 0);
    Row("Scope zoom out (while holding LT)", kFace[*InputZoomBtn(1) & 3], 3, 0);
    
    Row("Place / set off C4", "with Steady / Fire", 0, 0, true);
    if (NeonToggle("Left grip opens the weapon wheel on foot", InputLeftGripWheel())) ms = true;
    SectionHead("CONTROLS: MENUS AND MAP");
    if (NeonToggle("Menu = map; hold left trigger + Menu = pause", InputMenuMinimap())) ms = true;
    if (*InputMenuMinimap()) {
        Row("Map", "Menu", 0, 0, true);
        Row("Pause", "Left grip + Menu, or hold LT + Menu", 0, 0, true);
        SNote("Turn the switch above off to put map and pause on controls of your own.");
    } else {
        Act("Map", oBack);
        Act("Pause", oStart);
    }
    Row("Select / back", "A / X", 0, 0, true);
    Row("Map waypoint", "B", 0, 0, true);
    Row("VR menu (this one)", "Both thumbstick clicks", 0, 0, true);
    SNote("The game's own menus and map always keep these buttons, whatever you bind above.");
    // ctl3 (phase 2): vehicles and mounted guns have their own table; the grips are free here.
    SectionHead("CONTROLS: VEHICLES AND TURRETS");
    SNote("Used while driving, in a boat or on a mounted gun. The grips can be picked here.");
    Row("Steer", *InputSwapSticks() ? "R thumbstick" : "L thumbstick", 0, 0, true);
    VAct("Accelerate / turret fire", oRT);
    VAct("Brake / reverse", oLT);
    VAct("Get in / out (hold), switch seat", oX);
    VAct("Handbrake", oA);  // ctl4: DrGonzaga confirmed the game's A is the handbrake
    SNote("The game's other buttons in vehicles (what each does depends on the vehicle):");
    static const struct { const char *n; int o; } kVo[] = {{"Game's B", oB}, {"Game's Y (weapon)", oY},
        {"Game's LB", oLB}, {"Game's RB", oRB}, {"Game's left stick click", oLS}, {"Game's right stick click", oRS}};
    for (auto &v : kVo)
        VAct(v.n, v.o);
    if (NeonButton("Reset controls to the defaults", ImVec2(460, 54), kPurple)) {
        InputShipDefaults();  // ctl3: both tables, the layout Setup ships
        ms = true;
    }
    if (!XrThumbrestsBound()) SNote("Thumbrests: these controllers don't report them (Quest-style controllers only).");
    if (*VrLeftHanded()) SNote("Left-handed: the names are the gun hand's buttons (A means the button under your gun hand's thumb).");
    SectionHead("GESTURES");
    {
        static const char *const kG[][2] = {
            {"Holsters", "reach to a holster and press the right grip to draw that weapon."},
            {"Wrist menu", "turn your left palm up; pick with the right grip."},
            {"Slow motion", "press the left grip on the clock button by the minimap."},
            {"Cyber-eye", "left hand to the left side of your head, hold the left grip."},
            {"Current objective", "left hand to the left side of your head, tap the left grip."},
            {"Bow", "put the bow in the top weapon slot. Take it from your right shoulder with the left grip, and arrows from your "
                    "right shoulder with the right grip."},
            {"Melee", "swing the knife hand."},
            {"Crouch", "crouch for real (the control above works too)."},
            {"Two hands on a gun", "left grip near the front of a rifle. With a pistol, bring your left hand to the gun."},
            {"Grips", "fixed on foot and with weapons, to steady the guns."},
        };
        for (auto &g : kG) SNote("%s: %s", g[0], g[1]);
    }
    if (ms) Later(InputMapSave);
}

void SynthBackground(ImDrawList *dl) {
    const float W = (float)kTexW, H = (float)kTexH;
    dl->AddRectFilled(ImVec2(6, 6), ImVec2(W - 6, H - 6), IM_COL32(14, 5, 34, 246), 30);
    // the sun: horizontal stripes, yellow to pink, gaps widening towards the horizon
    const float hz = H * 0.70f, cx = W * 0.62f, R = 190;
    for (float y = hz - R; y < hz; y += 3) {
        float dy = hz - y, t = 1 - dy / R;
        float gap = t > 0.45f ? (t - 0.45f) * 26 : 0;
        if (gap > 0 && fmodf(hz - y, 22) < gap) continue;
        float hw = sqrtf(fmaxf(0, R * R - dy * dy));
        ImVec4 c(1, 0.85f - 0.6f * t, 0.3f + 0.35f * t, 0.30f);
        dl->AddRectFilled(ImVec2(cx - hw, y), ImVec2(cx + hw, y + 3), ImGui::GetColorU32(c));
    }
    dl->AddRectFilledMultiColor(ImVec2(10, hz - 70), ImVec2(W - 10, hz), IM_COL32(255, 60, 200, 0), IM_COL32(255, 60, 200, 0),
                                IM_COL32(255, 60, 200, 60), IM_COL32(255, 60, 200, 60));
    // the grid
    for (int k = 1; k <= 12; ++k) {
        float t = k / 12.f, y = hz + (H - 10 - hz) * t * t;
        dl->AddLine(ImVec2(10, y), ImVec2(W - 10, y), IM_COL32(255, 60, 210, (int)(40 + 110 * t)), 1.5f + t);
    }
    for (int k = -14; k <= 14; ++k) {
        float xb = cx + k * 130.f;
        dl->AddLine(ImVec2(cx + k * 18.f, hz), ImVec2(xb, H - 10), IM_COL32(255, 60, 210, 90), 1.5f);
    }
    dl->AddLine(ImVec2(10, hz), ImVec2(W - 10, hz), IM_COL32(255, 120, 230, 200), 2.5f);
    GlowRect(dl, ImVec2(12, 12), ImVec2(W - 12, H - 12), kCyan, 26, 0.9f);
}

void SynthUi() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)kTexW, (float)kTexH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.05f, 0.30f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.26f, 0.08f, 0.42f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.34f, 0.10f, 0.50f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kPink);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kCyan);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.02f, 0.13f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.08f, 0.02f, 0.18f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.62f, 0.38f, 1.0f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, kPink);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kCyan);
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, kCyan);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 34);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 30);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 20);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("VR menu###synth", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::SetWindowFontScale(kUiScale);
    ImGui::PushFont(g_fBody);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    SynthBackground(dl);
    // title
    {
        const char *t = "BLOOD DRAGON VR";
        ImVec2 ts = TextSize(g_fTitle, t);
        GlowText(dl, g_fTitle, FontPx(g_fTitle), ImVec2(36, 30), kPink, t, 1.6f);
        const char *s = *VrLeftHanded() ? "SETTINGS  //  LEFT-HANDED" : "SETTINGS";
        GlowText(dl, g_fBody, FontPx(g_fBody), ImVec2(36 + ts.x + 26, 30 + ts.y - TextSize(g_fBody, s).y - 6), kCyan, s, 0.8f);
    }
    // the four pages, the debug menu and close
    static const char *const kPages[4] = {"GRAPHICS", "VR", "GUNS", "CONTROLS"};
    static const ImVec4 *const kPageCol[4] = {&kCyan, &kPink, &kCyan, &kPink};
    for (int i = 0; i < 4; ++i) {
        ImGui::SetCursorPos(ImVec2(36, 140 + i * 96.f));
        if (NeonButton(kPages[i], ImVec2(250, 76), *kPageCol[i], g_sPage == i, g_fSect)) g_sPage = i;
    }
    ImGui::SetCursorPos(ImVec2(36, kTexH - 190.f));
    if (NeonButton("DEBUG MENU", ImVec2(250, 64), kYellow)) g_synth = false, Log("menu: debug menu (from the VR menu)");
    ImGui::SetCursorPos(ImVec2(36, kTexH - 110.f));
    if (NeonButton("CLOSE  (B)", ImVec2(250, 64), kPurple)) g_open = false;
    // the page
    ImVec2 ca(320, 128), cb(kTexW - 34.f, kTexH - 34.f);
    GlowRect(dl, ImVec2(ca.x + 2, ca.y + 2), ImVec2(cb.x - 2, cb.y - 2), g_sPage & 1 ? kPink : kCyan, 20, 0.7f);
    ImGui::SetCursorPos(ca);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 14));
    if (ImGui::BeginChild("page", ImVec2(cb.x - ca.x, cb.y - ca.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollWithMouse)) {
        // (a child window already takes its parent's font scale)
        // the other stick scrolls the page wherever the pointer is (io's wheel only reaches the hovered window)
        if (fabsf(g_synthScroll) > 0.01f) ImGui::SetScrollY(ImGui::GetScrollY() - g_synthScroll);
        switch (g_sPage) {
        case 0: SGraphics(); break;
        case 1: SVr(); break;
        case 2: SGuns(); break;
        default: SControls(); break;
        }
        ImGui::Dummy(ImVec2(1, 20));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(9);
    ImGui::PopStyleColor(14);
    (void)io;
}
