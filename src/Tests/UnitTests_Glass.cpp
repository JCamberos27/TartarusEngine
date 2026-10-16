// The opt-in Black Glass editor theme (src/Editor/Glass/, docs/EDITOR_DESIGN_SYSTEM.md): its style,
// the compositor's CPU side (finding and clearing a floating window's solid look, the arrival
// pose), the motion runtime's zero-memory-at-rest contract, the clipping guard and the input
// responsiveness policy. Headless: a private ImGui context, no GL.

#include "UnitTestSupport.h"

#include "EditorUIPrimitives.h"
#include "Glass/GlassCompositor.h"
#include "Glass/GlassLayout.h"
#include "Glass/GlassMotion.h"
#include "Glass/GlassMotionHooks.h"
#include "Glass/GlassTheme.h"
#include "Glass/GlassTokens.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

// A throwaway ImGui context for the duration of one test (restores the previous one).
struct HeadlessImGui {
    ImGuiContext* previous;
    ImGuiContext* context;
    HeadlessImGui() {
        previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        Glass::Motion().Reset();
        Glass::Motion().SetReduceMotion(false);
    }
    ~HeadlessImGui() {
        Glass::Motion().Reset();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    // One frame: NewFrame, the motion clock, `body` inside a fixed window, Render.
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        Glass::Motion().NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 500.0f), ImGuiCond_Always);
        ImGui::Begin("GlassTest", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        body();
        ImGui::End();
        ImGui::Render();
    }
};

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

void TestGlassThemeStyle() {
    HeadlessImGui gui;
    ImGuiStyle style;
    ConfigureMacEngineTheme(style);
    // 14 px on floating surfaces, smaller radii nested inside; the 8 px spatial law.
    CHECK(style.WindowRounding == Glass::GlassRadius && style.WindowRounding == 14.0f);
    CHECK(style.PopupRounding == 14.0f);
    CHECK(style.FrameRounding == Glass::RadiusControl && style.FrameRounding == 8.0f);
    CHECK(style.TabRounding == 8.0f && style.ChildRounding == 10.0f);
    CHECK(style.WindowPadding.x == 16.0f && style.WindowPadding.y == 16.0f);
    CHECK(style.ItemSpacing.x == 8.0f && style.ItemSpacing.y == 8.0f);
    CHECK(style.FrameBorderSize == 0.0f); // flat fields: depth comes from fill, not outlines
    CHECK(style.WindowBorderSize == 1.0f && style.PopupBorderSize == 1.0f); // the edge glass re-colours
    // Every colour slot was written (no magenta sentinel left behind).
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 c = style.Colors[i];
        CHECK(!(c.x == 1.0f && c.y == 0.0f && c.z == 1.0f && c.w == 1.0f));
    }
    // Solid where you work: docked panels and the solid fallbacks are opaque.
    const ImVec4 bg = style.Colors[ImGuiCol_WindowBg];
    CHECK(bg.x == Glass::Panel.x && bg.w == 1.0f);
    CHECK(style.Colors[ImGuiCol_PopupBg].x == Glass::PopupSolid.x && style.Colors[ImGuiCol_PopupBg].w == 1.0f);
    // A floating window's title bar is its body colour, so the compositor clears both together.
    for (ImGuiCol t : {ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_TitleBgCollapsed})
        CHECK(std::memcmp(&style.Colors[t], &bg, sizeof(bg)) == 0);
    CHECK(style.Colors[ImGuiCol_MenuBarBg].x == 0.0f && style.Colors[ImGuiCol_MenuBarBg].w == 1.0f);
    // Monochrome: no hue in the chrome. Every UI colour is a grey (axis / status colours are
    // content and aren't ImGuiCol slots).
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 c = style.Colors[i];
        CHECK(Near(c.x, c.y, 0.02f) && Near(c.y, c.z, 0.02f));
    }
    CHECK(style.Colors[ImGuiCol_TabSelectedOverline].x == Glass::Accent.x);
    CHECK(style.Colors[ImGuiCol_Text].x == Glass::TextPrimary.x && style.Colors[ImGuiCol_Text].w == 1.0f);
    // Calling it twice gives the same style (no accumulation).
    ImGuiStyle again = style;
    ConfigureMacEngineTheme(again);
    CHECK(std::memcmp(again.Colors, style.Colors, sizeof(style.Colors)) == 0);
    CHECK(again.FramePadding.x == style.FramePadding.x && again.WindowRounding == style.WindowRounding);
}

void TestGlassContrast() {
    using EditorUIPrimitives::ContrastRatio;
    for (const ImVec4& surface : {Glass::Panel, Glass::Card, Glass::Raised, Glass::GlassFallback}) {
        CHECK(ContrastRatio(Glass::TextPrimary, surface) >= 4.5f);
        CHECK(ContrastRatio(Glass::TextSecondary, surface) >= 4.5f);
        CHECK(ContrastRatio(Glass::TextTertiary, surface) >= 3.0f);
    }
    CHECK(ContrastRatio(Glass::OnAccent, Glass::Accent) >= 4.5f);
    // Glass over the brightest thing a scene can show: the blur's highlights are capped at
    // GlassCeiling, then the 25% black tint goes over them. White text still reads at 4.5:1.
    const float worst = Glass::GlassCeiling * (1.0f - Glass::GlassTint.w);
    CHECK(ContrastRatio(Glass::TextPrimary, ImVec4(worst, worst, worst, 1.0f)) >= 4.5f);
    // The glass material is the spec: a 25% black tint and a 15% white edge.
    CHECK(Glass::GlassTint.x == 0.0f && Near(Glass::GlassTint.w, 0.25f, 1e-6f));
    CHECK(Glass::GlassEdge.x == 1.0f && Near(Glass::GlassEdge.w, 0.15f, 1e-6f));
}

void TestGlassCompositorConvert() {
    HeadlessImGui gui;
    ImGuiStyle& style = ImGui::GetStyle();
    ConfigureMacEngineTheme(style);
    // A floating window, an overlay with a translucent background, and one with none.
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(100.0f, 80.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240.0f, 160.0f), ImGuiCond_Always);
    ImGui::Begin("Floating", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("glass");
    ImGui::Separator();
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(420.0f, 80.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("overlay");
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(420.0f, 300.0f), ImGuiCond_Always);
    ImGui::Begin("##bare", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("bare");
    ImGui::End();
    ImGui::Render();

    ImGuiWindow* floating = ImGui::FindWindowByName("Floating");
    ImGuiWindow* hint = ImGui::FindWindowByName("##hint");
    ImGuiWindow* bare = ImGui::FindWindowByName("##bare");
    CHECK(floating && hint && bare);
    if (!floating || !hint || !bare) return;

    // The background is found, at the colour ImGui drew it (alpha overrides included).
    const ImU32 panel = ImGui::GetColorU32(ImGuiCol_WindowBg);
    CHECK(Glass::FindWindowBackground(floating->DrawList, floating->Rect()) == panel);
    const ImU32 hintBg = Glass::FindWindowBackground(hint->DrawList, hint->Rect());
    CHECK(hintBg != 0 && ((hintBg >> IM_COL32_A_SHIFT) & 0xFF) == 89u); // 0.35 * 255
    CHECK(Glass::FindWindowBackground(bare->DrawList, bare->Rect()) == 0);

    // Converting: the body and title bar go transparent, the border and the separator (both
    // hairline) become the 15% white edge, the text is untouched.
    ImDrawList* dl = floating->DrawList;
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    int textBefore = 0;
    for (const ImDrawVert& v : dl->VtxBuffer) textBefore += v.col == text ? 1 : 0;
    const ImU32 clear[2] = {panel, ImGui::GetColorU32(ImGuiCol_TitleBgActive)};
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 edge = Glass::U32(Glass::GlassEdge);
    const int cleared = Glass::ConvertToGlass(dl, clear, 2, border, edge);
    CHECK(cleared > 0);
    int textAfter = 0, edges = 0, solid = 0;
    for (const ImDrawVert& v : dl->VtxBuffer) {
        textAfter += v.col == text ? 1 : 0;
        edges += v.col == edge ? 1 : 0;
        solid += (v.col == panel || v.col == border) ? 1 : 0;
    }
    CHECK(textAfter == textBefore && textAfter > 0);
    CHECK(edges > 0);
    CHECK(solid == 0);
    CHECK(Glass::FindWindowBackground(dl, floating->Rect()) == 0); // nothing left to clear
}

void TestGlassifySolids() {
    HeadlessImGui gui;
    ImGui::NewFrame();
    ImGui::Render();
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);
    dl.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f));
    const ImU32 black = IM_COL32(0, 0, 0, 255), raised = IM_COL32(0x0E, 0x0E, 0x0E, 255);
    const ImU32 pressed = IM_COL32(0x2A, 0x2A, 0x2A, 255), text = IM_COL32(0xE6, 0xE8, 0xE6, 255);
    const ImU32 dim = IM_COL32(0x6A, 0x6C, 0x6A, 255);
    const ImU32 hue = IM_COL32(0x20, 0x08, 0x08, 255), veil = IM_COL32(0x0E, 0x0E, 0x0E, 128);
    for (ImU32 c : {black, raised, pressed, text, hue, veil, dim}) dl.AddRectFilled(ImVec2(0, 0), ImVec2(10, 10), c);
    const int vtx = dl.VtxBuffer.Size / 7; // AddRectFilled without rounding: 4 vertices each
    CHECK(Glass::GlassifySolids(&dl) == vtx * 4); // black, raised, pressed and the grey text
    auto at = [&](int i) { return dl.VtxBuffer[i * vtx].col; };
    CHECK(at(0) == IM_COL32(0, 0, 0, 77));                       // black: a light inset veil
    CHECK((at(1) & ~IM_COL32_A_MASK) == 0u && ((at(1) >> IM_COL32_A_SHIFT) & 0xFF) > 77u); // raised: dark glass
    CHECK((at(2) & ~IM_COL32_A_MASK) == IM_COL32(255, 255, 255, 0)); // pressed / hairline: a white lift
    CHECK(at(3) == text);                                        // primary text untouched
    CHECK(at(4) == hue);                                         // a colour with hue untouched
    CHECK(at(5) == veil);                                        // already translucent: untouched
    const int up = Glass::LiftGreyText(0x6C) - 0x6C;             // grey text: lifted by its brightest channel
    CHECK(up > 0 && at(6) == IM_COL32(0x6A + up, 0x6C + up, 0x6A + up, 255));
}

void TestGlassCompositorPose() {
    HeadlessImGui gui;
    ImGui::NewFrame();
    ImGui::Render();
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushTexture(ImGui::GetIO().Fonts->TexRef);
    dl.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f));
    dl.AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(30.0f, 30.0f), IM_COL32(255, 255, 255, 200));
    const ImRect before = Glass::DrawListBounds(&dl);
    CHECK(Near(before.Max.x, 100.0f, 1e-4f));

    Glass::SurfacePose identity;
    CHECK(identity.Identity());
    Glass::ApplyPose(&dl, identity); // a no-op
    CHECK(dl.VtxBuffer[0].pos.x >= 9.0f);

    Glass::SurfacePose pose;
    pose.pivot = ImVec2(10.0f, 10.0f);
    pose.scale = 0.5f;
    pose.offset = ImVec2(0.0f, 4.0f);
    pose.alpha = 0.5f;
    CHECK(!pose.Identity());
    Glass::ApplyPose(&dl, pose);
    bool inside = true, faded = true;
    for (const ImDrawVert& v : dl.VtxBuffer) {
        inside = inside && v.pos.x >= 9.0f && v.pos.x <= 21.0f && v.pos.y >= 13.0f && v.pos.y <= 25.0f;
        const unsigned int a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        faded = faded && (a == 0 || a == 100);
    }
    CHECK(inside);
    CHECK(faded);
    // The clip rect moved with it: (0,0)-(100,100) about (10,10) at 50%, down 4.
    const ImRect after = Glass::DrawListBounds(&dl);
    CHECK(Near(after.Min.x, 5.0f, 1e-4f) && Near(after.Min.y, 9.0f, 1e-4f));
    CHECK(Near(after.Max.x, 55.0f, 1e-4f) && Near(after.Max.y, 59.0f, 1e-4f));
}

void TestGlassArrivalMotion() {
    // Every arrival curve starts at rest-at-zero and lands on 1; the window pop overshoots a little
    // and settles; the arrival poses start visibly smaller / lower than where they land.
    for (const Glass::MotionSpec* s : {&Glass::FloatIn, &Glass::TooltipIn, &Glass::WindowFade, &Glass::PanelIn, &Glass::RimGlint}) {
        CHECK(Glass::EaseProgress(*s, 0.0f) == 0.0f);
        CHECK(Near(Glass::EaseProgress(*s, 1.0f), 1.0f, 1e-3f));
    }
    float maxPop = 0.0f;
    for (float t = 0.0f; t <= 1.0f; t += 0.01f) maxPop = std::max(maxPop, Glass::EaseProgress(Glass::WindowPop, t));
    CHECK(maxPop > 1.01f && maxPop < 1.12f);
    CHECK(Near(Glass::EaseProgress(Glass::WindowPop, 1.0f), 1.0f, 0.01f));
    CHECK(Glass::DampingRatio(Glass::WindowPop.p0, Glass::WindowPop.p1, Glass::WindowPop.p2) < 1.0f);
    float maxGlide = 0.0f;
    for (float t = 0.0f; t <= 1.0f; t += 0.01f) maxGlide = std::max(maxGlide, Glass::EaseProgress(Glass::SelectionGlide, t));
    CHECK(maxGlide > 1.02f); // the selection visibly lands
    CHECK(Near(Glass::EaseProgress(Glass::SelectionGlide, 1.0f), 1.0f, 0.01f));
    CHECK(Glass::FloatInFromScale < 1.0f && Glass::WindowPopFromScale < 1.0f && Glass::PanelInRisePx > 0.0f);
}

void TestGlassMotionZeroAtRest() {
    HeadlessImGui gui;
    Glass::ImAnimContext& m = Glass::Motion();
    const ImGuiID id = 0x1234u;
    // Asking about a value at rest creates nothing.
    gui.Frame([&] { CHECK(m.HoverGlow(id, false) == 0.0f); });
    CHECK(m.LiveCount() == 0 && m.HeldBytes() == 0);
    // Hover: an entry exists while the glow rises...
    float v = 0.0f;
    for (int f = 0; f < 4; ++f) gui.Frame([&] { v = m.HoverGlow(id, true); });
    CHECK(v > 0.0f && v < 1.0f);
    CHECK(m.LiveCount() == 1);
    for (int f = 0; f < 30; ++f) gui.Frame([&] { v = m.HoverGlow(id, true); });
    CHECK(Near(v, 1.0f, 1e-3f));
    // ...and is gone, with every byte, once it has settled back on rest.
    for (int f = 0; f < 60; ++f) gui.Frame([&] { v = m.HoverGlow(id, false); });
    CHECK(v == 0.0f);
    CHECK(m.LiveCount() == 0);
    CHECK(m.HeldBytes() == 0);
    // A pulse is gone the frame after it reports finished.
    gui.Frame([&] { m.Pulse(id, 0.05f); });
    CHECK(m.PulseActive(id));
    float p = 0.0f;
    for (int f = 0; f < 10; ++f) gui.Frame([&] { p = m.PulseProgress(id); });
    CHECK(p == 1.0f && !m.PulseActive(id) && m.HeldBytes() == 0);
    // Reduce Motion: everything lands at once, nothing is held.
    m.SetReduceMotion(true);
    gui.Frame([&] { CHECK(m.HoverGlow(id, true) == 1.0f); });
    CHECK(m.LiveCount() == 0);
    m.SetReduceMotion(false);
}

void TestGlassMotionStaleSweep() {
    HeadlessImGui gui;
    Glass::ImAnimContext& m = Glass::Motion();
    gui.Frame([&] { m.HoverGlow(7u, true); m.HoverGlow(8u, true); });
    CHECK(m.LiveCount() == 2);
    // Its widget stops being drawn mid-animation (window closed, row scrolled away): swept.
    for (int f = 0; f < Glass::ImAnimContext::kStaleFrames + 2; ++f) gui.Frame([&] { m.HoverGlow(8u, true); });
    CHECK(m.LiveCount() == 1);
    CHECK(!m.Live(7u) && m.Live(8u));
}

void TestGlassVelocityGate() {
    HeadlessImGui gui;
    Glass::ImAnimContext& m = Glass::Motion();
    ImGuiIO& io = ImGui::GetIO();
    // A fast sweep: 120 px per 1/60 s frame = 7200 px/s, far past the gate.
    float x = 10.0f;
    for (int f = 0; f < 8; ++f) {
        io.AddMousePosEvent(x, 200.0f);
        x += 120.0f;
        if (x > 700.0f) x = 10.0f;
        gui.Frame([] {});
    }
    CHECK(m.SweepGateClosed());
    // A row the sweep crosses doesn't light...
    io.AddMousePosEvent(x, 200.0f);
    float v = 1.0f;
    gui.Frame([&] { v = m.HoverGlow(99u, true); });
    CHECK(v == 0.0f && m.LiveCount() == 0);
    // ...but once the pointer settles, it does.
    for (int f = 0; f < 30; ++f) gui.Frame([&] { v = m.HoverGlow(99u, true); });
    CHECK(!m.SweepGateClosed());
    CHECK(v > 0.5f);
}

void TestGlassRevealClipGuard() {
    HeadlessImGui gui;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID reveal = 0xBEEFu;
    bool open = false, wasOpen = false;
    ImRect lastRect;
    bool lastHovered = false, firstHovered = false;
    float afterY = 0.0f, startY = 0.0f, revealT = 0.0f;
    auto body = [&] {
        startY = ImGui::GetCursorScreenPos().y;
        if (Glass::BeginReveal(reveal, open, wasOpen)) {
            revealT = Glass::CurrentRevealT();
            for (int i = 0; i < 12; ++i) {
                ImGui::PushID(i);
                ImGui::Button("row", ImVec2(200.0f, 20.0f));
                if (i == 0) firstHovered = ImGui::IsItemHovered();
                if (i == 11) { lastRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); lastHovered = ImGui::IsItemHovered(); }
                ImGui::PopID();
            }
            Glass::EndReveal();
        }
        afterY = ImGui::GetCursorScreenPos().y;
    };
    gui.Frame(body); // closed at rest: the body costs nothing
    CHECK(Near(afterY, startY, 0.5f));
    // Open it; a few frames in it's partway.
    wasOpen = false; open = true;
    gui.Frame(body);
    wasOpen = true;
    gui.Frame(body);
    CHECK(revealT > 0.0f && revealT < 1.0f);
    CHECK(Glass::LayoutInMotion());
    // The layout below follows the animated height, not the full one.
    const float full = 12.0f * (20.0f + ImGui::GetStyle().ItemSpacing.y);
    CHECK(afterY - startY < full - 1.0f);
    CHECK(afterY - startY > 0.0f);
    // The last row is laid out (its rect is known) but sits below the clip edge: the pointer over
    // it doesn't hover it.
    io.AddMousePosEvent(lastRect.GetCenter().x, lastRect.GetCenter().y);
    gui.Frame(body);
    CHECK(startY + full * revealT < lastRect.Min.y); // the clip edge this frame is above the row
    CHECK(!lastHovered);
    // The first row is inside the clip and stays live mid-animation.
    io.AddMousePosEvent(lastRect.GetCenter().x, startY + 10.0f);
    gui.Frame(body);
    CHECK(firstHovered);
    // Settled open: everything is reachable, and the reveal's state is gone.
    for (int f = 0; f < 40; ++f) gui.Frame(body);
    CHECK(revealT == 1.0f);
    CHECK(Near(afterY - startY, full, 0.5f));
    io.AddMousePosEvent(lastRect.GetCenter().x, lastRect.GetCenter().y);
    gui.Frame(body);
    gui.Frame(body);
    CHECK(lastHovered);
    CHECK(Glass::Motion().LiveCount() == 0);
}

// Staggered rows shift the layout cursor and take the shift back after the row. That must leave
// ImGui's layout valid when a row is the last thing in its window: the stagger outlives the
// height animation, so for a few frames the reveal is at rest while its last rows still settle.
// (A SetCursorPos-style move past CursorMaxPos with no item after it asserts at End().)
void TestGlassRevealRowsKeepLayoutValid() {
    HeadlessImGui gui;
    const ImGuiID reveal = 0xC0DEu;
    bool open = false, wasOpen = false;
    float afterY = 0.0f, startY = 0.0f;
    bool sawStaggerAtRest = false;
    auto body = [&] {
        startY = ImGui::GetCursorScreenPos().y;
        if (Glass::BeginReveal(reveal, open, wasOpen)) {
            const bool moving = Glass::CurrentRevealMoving();
            for (int i = 0; i < 10; ++i) {
                Glass::RevealRow row(i);
                ImGui::PushID(i);
                ImGui::Button("row", ImVec2(200.0f, 20.0f));
                ImGui::PopID();
            }
            const float shiftedRowGap = ImGui::GetCursorScreenPos().y - startY;
            if (!moving && shiftedRowGap > 0.0f && Glass::Motion().LiveCount() > 0) sawStaggerAtRest = true;
            Glass::EndReveal();
        }
        afterY = ImGui::GetCursorScreenPos().y;
        // Nothing after this: the window ends right on the reveal's last row.
    };
    gui.Frame(body);
    open = true;
    gui.Frame(body);
    wasOpen = true;
    for (int f = 0; f < 60; ++f) gui.Frame(body);
    CHECK(sawStaggerAtRest);
    // Settled: exactly the natural height, no residue from the shifts.
    const float full = 10.0f * (20.0f + ImGui::GetStyle().ItemSpacing.y);
    CHECK(Near(afterY - startY, full, 0.5f));
    CHECK(Glass::Motion().LiveCount() == 0);
}

// Drawing under a table's rows after an ImGuiListClipper loop (the Objects panel's gliding
// selection panel): no cell is current there, which ImGui's own background-channel push/pop
// can't handle (it indexes the columns with -1). Glass::TableBackground can, and restores the
// channel and clip rect it found.
void TestGlassTableBackgroundAfterClipper() {
    HeadlessImGui gui;
    bool sawNoCell = false, restored = false, drewInBackground = false;
    gui.Frame([&] {
        if (!ImGui::BeginTable("##t", 2)) return;
        ImGuiListClipper clipper;
        clipper.Begin(200, 20.0f);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 20.0f);
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i);
            }
        ImGuiTable* t = ImGui::GetCurrentTable();
        sawNoCell = t->CurrentColumn < 0;
        const int channel = t->DrawSplitter->_Current;
        const ImRect clip = ImGui::GetCurrentWindow()->ClipRect;
        {
            Glass::TableBackground under;
            drewInBackground = t->DrawSplitter->_Current == t->Bg2DrawChannelCurrent;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(10, 10), IM_COL32_WHITE);
        }
        const ImRect after = ImGui::GetCurrentWindow()->ClipRect;
        restored = t->DrawSplitter->_Current == channel && after.Min.x == clip.Min.x && after.Max.y == clip.Max.y;
        ImGui::EndTable();
    });
    CHECK(sawNoCell);
    CHECK(drewInBackground);
    CHECK(restored);
}

void TestGlassMotionButtonPolicy() {
    HeadlessImGui gui;
    ImGuiIO& io = ImGui::GetIO();
    // Rule 2: the hit rect is the union of where it rests and where it's drawn.
    const ImRect rest(ImVec2(0, 0), ImVec2(10, 10)), drawn(ImVec2(5, -4), ImVec2(15, 6));
    const ImRect hit = Glass::MotionHitRect(rest, drawn);
    CHECK(hit.Min.x == 0 && hit.Min.y == -4 && hit.Max.x == 15 && hit.Max.y == 10);
    // At rest a press is the standard click-release; in a moving layout, release anywhere counts.
    bool flagsAtRest = false, flagsMoving = false;
    gui.Frame([&] { flagsAtRest = (Glass::MotionButtonFlags() & ImGuiButtonFlags_PressedOnClickReleaseAnywhere) != 0; });
    gui.Frame([&] { Glass::MarkLayoutInMotion(); flagsMoving = (Glass::MotionButtonFlags() & ImGuiButtonFlags_PressedOnClickReleaseAnywhere) != 0; });
    CHECK(!flagsAtRest && flagsMoving);
    // A caller's own press policy is never overridden.
    CHECK(Glass::MotionButtonFlags(ImGuiButtonFlags_PressedOnClick) == ImGuiButtonFlags_PressedOnClick);

    // Functional: a button that glides 40 px per frame while the mouse is held on it still
    // registers the click when released after it has moved out from under the pointer.
    float y = 100.0f;
    bool pressed = false;
    auto body = [&] {
        Glass::MarkLayoutInMotion();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImRect r(p, p + ImVec2(120.0f, 24.0f));
        bool hov = false, held = false;
        pressed = Glass::MotionButton(ImGui::GetID("glider"), r, r, &hov, &held);
    };
    gui.Frame(body);
    io.AddMousePosEvent(70.0f, y + 12.0f);
    gui.Frame(body);
    io.AddMouseButtonEvent(0, true);
    gui.Frame(body);
    CHECK(!pressed);
    y += 40.0f;
    gui.Frame(body);
    y += 40.0f; // well clear of the pointer now
    io.AddMouseButtonEvent(0, false);
    gui.Frame(body);
    CHECK(pressed);
}

void TestGlassSprings() {
    // Notifications are under-damped (they overshoot); the shake decays inside its duration.
    CHECK(Glass::DampingRatio(Glass::ToastPop.p0, Glass::ToastPop.p1, Glass::ToastPop.p2) < 1.0f);
    CHECK(Glass::DampingRatio(Glass::SnapPop.p0, Glass::SnapPop.p1, Glass::SnapPop.p2) < 1.0f);
    const Glass::ShakeSpec& s = Glass::ErrorShake;
    CHECK(Near(Glass::DampingRatio(s.mass, s.stiffness, s.damping), 0.2f, 1e-3f));
    CHECK(Glass::SpringImpulse(0.0f, s.mass, s.stiffness, s.damping, s.amplitudePx) == 0.0f);
    float peak = 0.0f;
    for (float t = 0.0f; t < 0.2f; t += 0.0005f)
        peak = std::max(peak, Glass::SpringImpulse(t, s.mass, s.stiffness, s.damping, s.amplitudePx));
    CHECK(Near(peak, s.amplitudePx, s.amplitudePx * 0.02f));
    CHECK(std::fabs(Glass::SpringImpulse(s.durationMs * 0.001f, s.mass, s.stiffness, s.damping, s.amplitudePx)) < s.amplitudePx * 0.05f);
    // The pop overshoots past 1 and lands on 1.
    float maxPop = 0.0f;
    for (float t = 0.0f; t <= 1.0f; t += 0.01f) maxPop = std::max(maxPop, Glass::EaseProgress(Glass::ToastPop, t));
    CHECK(maxPop > 1.02f);
    CHECK(Near(Glass::EaseProgress(Glass::ToastPop, 1.0f), 1.0f, 0.02f));
    // The structural curve is monotone and pinned at both ends.
    CHECK(Glass::EaseProgress(Glass::RevealOpen, 0.0f) == 0.0f);
    CHECK(Near(Glass::EaseProgress(Glass::RevealOpen, 1.0f), 1.0f, 1e-4f));
    float prev = 0.0f;
    bool monotone = true;
    for (float t = 0.0f; t <= 1.0f; t += 0.01f) {
        const float e = Glass::EaseProgress(Glass::RevealOpen, t);
        monotone = monotone && e + 1e-5f >= prev;
        prev = e;
    }
    CHECK(monotone);
    // Stagger: one step per index, capped.
    CHECK(Near(Glass::StaggerDelay(3, 20.0f, 8), 0.060f, 1e-6f));
    CHECK(Glass::StaggerDelay(50, 20.0f, 8) == Glass::StaggerDelay(8, 20.0f, 8));
    CHECK(Glass::StaggerDelay(-2, 20.0f, 8) == 0.0f);
}

// Round 2: colour and 2D tweens, the procedural helpers, clips and paths, all zero at rest.
void TestGlassMotionRound2ZeroAtRest() {
    HeadlessImGui gui;
    Glass::ImAnimContext& m = Glass::Motion();
    const ImVec4 grey(0.5f, 0.5f, 0.5f, 1.0f), red(0.9f, 0.2f, 0.2f, 1.0f);
    // A colour at rest makes nothing; one heading away holds one entry and lands back to none.
    gui.Frame([&] { CHECK(m.AnimateColor(1u, grey, grey, Glass::FocusIn).x == grey.x); });
    CHECK(m.LiveCount() == 0 && m.HeldBytes() == 0);
    ImVec4 c = grey;
    for (int f = 0; f < 3; ++f) gui.Frame([&] { c = m.AnimateColor(1u, red, grey, Glass::FocusIn, Glass::FocusOut); });
    CHECK(m.LiveCount() == 1 && c.x > grey.x && c.x < red.x);
    for (int f = 0; f < 60; ++f) gui.Frame([&] { c = m.AnimateColor(1u, grey, grey, Glass::FocusIn, Glass::FocusOut); });
    CHECK(Near(c.x, grey.x, 1e-3f) && m.LiveCount() == 0 && m.HeldBytes() == 0);
    // A 2D glide departs from last frame's target and is forgotten once it lands.
    ImVec2 v(0, 0);
    for (int f = 0; f < 2; ++f) gui.Frame([&] { v = m.Follow2(2u, ImVec2(100, 40), ImVec2(0, 0), Glass::DragGhostFollow); });
    CHECK(v.x > 0.0f && v.x < 100.0f && m.LiveCount() == 1);
    for (int f = 0; f < 30; ++f) gui.Frame([&] { v = m.Follow2(2u, ImVec2(100, 40), ImVec2(100, 40), Glass::DragGhostFollow); });
    CHECK(Near(v.x, 100.0f, 0.1f) && m.LiveCount() == 0 && m.HeldBytes() == 0);
    // Noise and Oscillate are pure functions of the clock: bounded, moving, holding nothing.
    float n0 = 0.0f, n1 = 0.0f;
    gui.Frame([&] { n0 = m.Noise(3u, 2.0f, 5.0f); });
    for (int f = 0; f < 20; ++f) gui.Frame([&] { n1 = m.Noise(3u, 2.0f, 5.0f); CHECK(std::fabs(m.Oscillate(0.25f)) <= 1.0f); });
    CHECK(std::fabs(n0) <= 5.0f && std::fabs(n1) <= 5.0f && n0 != n1);
    CHECK(m.LiveCount() == 0 && m.HeldBytes() == 0);
    // The reject wiggle: moves, then returns to 0 with nothing held.
    float peak = 0.0f, w = 0.0f;
    gui.Frame([&] { m.TriggerWiggle(4u, Glass::InvalidWiggle); });
    for (int f = 0; f < 40; ++f) gui.Frame([&] { w = m.Wiggle(4u, Glass::InvalidWiggle); peak = std::max(peak, std::fabs(w)); });
    CHECK(peak > 0.5f && w == 0.0f && m.LiveCount() == 0 && m.HeldBytes() == 0);
    // Reduce Motion: a colour lands at once and the procedural helpers are still.
    m.SetReduceMotion(true);
    gui.Frame([&] {
        CHECK(m.AnimateColor(1u, red, grey, Glass::FocusIn).x == red.x);
        CHECK(m.Noise(3u, 2.0f, 5.0f) == 0.0f && m.Oscillate(1.0f) == 0.0f);
    });
    CHECK(m.LiveCount() == 0);
    m.SetReduceMotion(false);
}

void TestGlassClipsAndPaths() {
    HeadlessImGui gui;
    Glass::ImAnimContext& m = Glass::Motion();
    // A two-key clip: 0 -> 1 over 100 ms. Its instance is an entry only while it plays.
    constexpr ImGuiID kClip = 0x54434C50u, kInst = 0x54494E53u, kCh = 0x5443484Eu;
    const Glass::ClipKey keys[] = {{kCh, 0.0f, 0.0f, Glass::EaseId::OutCubic}, {kCh, 100.0f, 1.0f, Glass::EaseId::OutCubic}};
    Glass::ImAnimContext::DefineClip(kClip, keys, 2);
    CHECK(Glass::ImAnimContext::ClipDefined(kClip));
    CHECK(Near(Glass::ImAnimContext::ClipSeconds(kClip), 0.1f, 1e-3f));
    gui.Frame([&] { m.PlayClip(kInst, kClip); });
    CHECK(m.ClipPlaying(kInst) && m.LiveCount() == 1);
    float v = -1.0f;
    gui.Frame([&] { v = m.ClipFloat(kInst, kCh, 1.0f); });
    CHECK(v > 0.0f && v < 1.0f);
    for (int f = 0; f < 20; ++f) gui.Frame([&] { v = m.ClipFloat(kInst, kCh, 1.0f); });
    CHECK(Near(v, 1.0f, 1e-3f) && !m.ClipPlaying(kInst) && m.LiveCount() == 0 && m.HeldBytes() == 0);
    // Reduce Motion skips a clip entirely.
    m.SetReduceMotion(true);
    gui.Frame([&] { m.PlayClip(kInst, kClip); });
    CHECK(!m.ClipPlaying(kInst) && m.LiveCount() == 0);
    m.SetReduceMotion(false);
    // Paths: endpoints, constant-speed midpoint, and a morph that is path a at 0 and b at 1.
    constexpr ImGuiID kA = 0x50415441u, kB = 0x50415442u;
    const Glass::PathSeg a[] = {{Glass::PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(1, 0)}};
    const Glass::PathSeg b[] = {{Glass::PathSeg::Line, ImVec2(0, 0), ImVec2(0, 0), ImVec2(1, 1)}};
    Glass::ImAnimContext::DefinePath(kA, ImVec2(0, 0), a, 1);
    Glass::ImAnimContext::DefinePath(kB, ImVec2(0, 1), b, 1);
    CHECK(Near(Glass::ImAnimContext::PathPoint(kA, 0.0f).x, 0.0f, 1e-3f));
    CHECK(Near(Glass::ImAnimContext::PathPoint(kA, 1.0f).x, 1.0f, 1e-3f));
    CHECK(Near(Glass::ImAnimContext::PathPoint(kA, 0.5f).x, 0.5f, 0.02f));
    CHECK(Near(Glass::ImAnimContext::PathMorph(kA, kB, 0.5f, 0.0f).y, 0.0f, 0.02f));
    CHECK(Near(Glass::ImAnimContext::PathMorph(kA, kB, 0.5f, 1.0f).y, 1.0f, 0.02f));
}

void TestGlassRound2Tokens() {
    // The press release springs back with a small overshoot and settles inside its duration.
    const Glass::MotionSpec& r = Glass::PressRelease;
    const float zeta = Glass::DampingRatio(r.p0, r.p1, r.p2);
    CHECK(zeta > 0.5f && zeta < 0.9f);
    CHECK(zeta * std::sqrt(r.p1 / r.p0) >= 5.0f);
    CHECK(Near(Glass::EaseProgress(r, 1.0f), 1.0f, 0.01f));
    // The reject wiggle is gentler than the compile-failure shake.
    CHECK(Glass::InvalidWiggle.amplitudePx < Glass::ErrorShake.amplitudePx);
    CHECK(Glass::DampingRatio(Glass::InvalidWiggle.mass, Glass::InvalidWiggle.stiffness, Glass::InvalidWiggle.damping) >
          Glass::DampingRatio(Glass::ErrorShake.mass, Glass::ErrorShake.stiffness, Glass::ErrorShake.damping));
    // Every new curve starts at 0 and lands on 1.
    for (const Glass::MotionSpec* s : {&Glass::PressDown, &Glass::CheckDraw, &Glass::CheckErase, &Glass::FocusIn, &Glass::FocusOut,
                                       &Glass::RowFlash, &Glass::ScrollEase, &Glass::DockRimGlint, &Glass::TransportMorph,
                                       &Glass::PlayFrameIn, &Glass::PlayFrameOut, &Glass::CameraFrame, &Glass::DragGhostFollow,
                                       &Glass::DragGhostIn, &Glass::DropFly, &Glass::DropReturn, &Glass::DropMarkerGlide,
                                       &Glass::ProgressGlide, &Glass::ToastCheckDraw}) {
        CHECK(Near(Glass::EaseProgress(*s, 0.0f), 0.0f, 1e-4f));
        CHECK(Near(Glass::EaseProgress(*s, 1.0f), 1.0f, 1e-3f));
    }
    CHECK(Glass::CameraFrame.durationMs <= Glass::CameraFrameMaxMs);
    CHECK(Glass::SaveCheckDrawMs < Glass::SaveCheckFadeAtMs && Glass::SaveCheckFadeAtMs < Glass::SaveCheckTotalMs);
}

// The shared widgets through the motion bridge: a checkbox press and a segmented glide settle back
// to nothing held.
void TestGlassBridgeWidgetsSettle() {
    HeadlessImGui gui;
    EditorTheme::SetMotionHooks(&Glass::HostMotionHooks());
    Glass::ImAnimContext& m = Glass::Motion();
    bool on = false;
    int seg = 0;
    const char* labels[] = {"A", "B", "C"};
    auto body = [&] {
        EditorUIPrimitives::Checkbox("check", &on);
        EditorUIPrimitives::Segmented("seg", &seg, labels, 3);
    };
    gui.Frame(body);
    CHECK(m.LiveCount() == 0);
    on = true;
    seg = 2;
    gui.Frame(body);
    CHECK(m.LiveCount() > 0); // the tick draws on, the pill glides
    for (int f = 0; f < 60; ++f) gui.Frame(body);
    CHECK(m.LiveCount() == 0 && m.HeldBytes() == 0);
    EditorTheme::SetMotionHooks(nullptr);
}

} // namespace

void RegisterGlassTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Glass motion: colour / 2D tweens and procedural helpers hold nothing at rest", TestGlassMotionRound2ZeroAtRest);
    tests.emplace_back("Glass motion: keyframe clips and paths", TestGlassClipsAndPaths);
    tests.emplace_back("Glass motion: round 2 tokens", TestGlassRound2Tokens);
    tests.emplace_back("Glass motion: bridged widgets settle to nothing held", TestGlassBridgeWidgetsSettle);
    tests.emplace_back("Glass theme: complete, monochrome style on the 8 px grid", TestGlassThemeStyle);
    tests.emplace_back("Glass theme: text contrast on solid surfaces and over the brightest glass", TestGlassContrast);
    tests.emplace_back("Glass compositor: finds and clears a floating window's solid look", TestGlassCompositorConvert);
    tests.emplace_back("Glass compositor: arrival pose moves vertices and clip rects", TestGlassCompositorPose);
    tests.emplace_back("Glass: panel solids become washes, text and hues untouched", TestGlassifySolids);
    tests.emplace_back("Glass motion: arrival curves and springs", TestGlassArrivalMotion);
    tests.emplace_back("Glass motion: no entries and no bytes at rest", TestGlassMotionZeroAtRest);
    tests.emplace_back("Glass motion: abandoned animations are swept", TestGlassMotionStaleSweep);
    tests.emplace_back("Glass motion: fast pointer sweeps don't light rows", TestGlassVelocityGate);
    tests.emplace_back("Glass layout: reveal moves the layout and clips hit-testing", TestGlassRevealClipGuard);
    tests.emplace_back("Glass layout: staggered rows keep ImGui's layout valid", TestGlassRevealRowsKeepLayoutValid);
    tests.emplace_back("Glass layout: drawing under a table after a clipped loop", TestGlassTableBackgroundAfterClipper);
    tests.emplace_back("Glass layout: moving buttons stay clickable", TestGlassMotionButtonPolicy);
    tests.emplace_back("Glass motion: springs, curves and stagger", TestGlassSprings);
}
