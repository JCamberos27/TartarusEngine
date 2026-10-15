// The opt-in Glass editor theme (src/Editor/Glass/, docs/EDITOR_DESIGN_SYSTEM.md): its style, the
// linear-light surface maths, the motion runtime's zero-memory-at-rest contract, the clipping guard
// and the input responsiveness policy. Headless: a private ImGui context, no GL.

#include "UnitTestSupport.h"

#include "EditorUIPrimitives.h"
#include "Glass/GlassLayout.h"
#include "Glass/GlassMotion.h"
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
    // The 8 px spatial law and the 10-12 px rounding.
    CHECK(style.WindowRounding == Glass::RadiusWindow && style.WindowRounding == 12.0f);
    CHECK(style.FrameRounding == Glass::RadiusControl && style.FrameRounding == 10.0f);
    CHECK(style.TabRounding == 10.0f && style.PopupRounding == 12.0f && style.ChildRounding == 12.0f);
    CHECK(style.WindowPadding.x == 16.0f && style.WindowPadding.y == 16.0f);
    CHECK(style.ItemSpacing.x == 8.0f && style.ItemSpacing.y == 8.0f);
    CHECK(style.FrameBorderSize == 0.0f); // flat fields: depth comes from fill, not outlines
    // Every colour slot was written (no magenta sentinel left behind).
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        const ImVec4 c = style.Colors[i];
        CHECK(!(c.x == 1.0f && c.y == 0.0f && c.z == 1.0f && c.w == 1.0f));
    }
    // Typography and the one accent.
    CHECK(style.Colors[ImGuiCol_Text].x == Glass::TextPrimary.x && style.Colors[ImGuiCol_Text].w == 1.0f);
    CHECK(style.Colors[ImGuiCol_TextDisabled].x == Glass::TextTertiary.x);
    CHECK(style.Colors[ImGuiCol_TabSelectedOverline].z == Glass::Accent.z);
    // Translucent panels at the token opacity.
    CHECK(Near(style.Colors[ImGuiCol_WindowBg].w, Glass::SurfacePanel.opacity, 1e-6f));
    // Calling it twice gives the same style (no accumulation).
    ImGuiStyle again = style;
    ConfigureMacEngineTheme(again);
    CHECK(std::memcmp(again.Colors, style.Colors, sizeof(style.Colors)) == 0);
    CHECK(again.FramePadding.x == style.FramePadding.x && again.WindowRounding == style.WindowRounding);
}

void TestGlassContrast() {
    using EditorUIPrimitives::ContrastRatio;
    const ImVec4 panel = Glass::CompositeOnWallpaper(Glass::SurfacePanel);
    const ImVec4 card = Glass::CompositeOnWallpaper(Glass::SurfaceRaised);
    CHECK(ContrastRatio(Glass::TextPrimary, panel) >= 4.5f);
    CHECK(ContrastRatio(Glass::TextSecondary, panel) >= 4.5f);
    CHECK(ContrastRatio(Glass::TextTertiary, panel) >= 3.0f);
    CHECK(ContrastRatio(Glass::Accent, panel) >= 3.0f);
    CHECK(ContrastRatio(Glass::TextPrimary, card) >= 4.5f);
    CHECK(ContrastRatio(Glass::OnAccent, Glass::Accent) >= 3.0f);
}

void TestGlassLinearBlend() {
    // The corrected colour, blended the way the ImGui GL backend blends (encoded space), lands on
    // the linear-light composite of the original tint.
    const Glass::GlassSurface surfaces[] = {Glass::SurfacePanel, Glass::SurfaceRaised, Glass::SurfaceFloat, Glass::SurfaceHud};
    for (const auto& s : surfaces) {
        const ImVec4 c = Glass::LinearCorrectOver(s.tint, s.opacity, Glass::Wallpaper);
        CHECK(Near(c.w, s.opacity, 1e-6f));
        const float* tint = &s.tint.x;
        const float* back = &Glass::Wallpaper.x;
        const float* got = &c.x;
        for (int k = 0; k < 3; ++k) {
            const float want = Glass::LinearToSrgb(s.opacity * Glass::SrgbToLinear(tint[k]) +
                                                   (1.0f - s.opacity) * Glass::SrgbToLinear(back[k]));
            const float blended = s.opacity * got[k] + (1.0f - s.opacity) * back[k];
            CHECK(Near(blended, want, 2e-3f));
        }
    }
    // The transfer functions invert each other.
    for (float v = 0.0f; v <= 1.0f; v += 0.05f) CHECK(Near(Glass::LinearToSrgb(Glass::SrgbToLinear(v)), v, 1e-4f));
    // Opaque and clear are passed through.
    CHECK(Glass::LinearCorrectOver(Glass::TextPrimary, 1.0f, Glass::Wallpaper).x == Glass::TextPrimary.x);
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

} // namespace

void RegisterGlassTests(UnitTestSupport::TestList& tests) {
    tests.emplace_back("Glass theme: complete style on the 8 px grid", TestGlassThemeStyle);
    tests.emplace_back("Glass theme: text and accent contrast over the translucent surfaces", TestGlassContrast);
    tests.emplace_back("Glass theme: surfaces blend as in linear light", TestGlassLinearBlend);
    tests.emplace_back("Glass motion: no entries and no bytes at rest", TestGlassMotionZeroAtRest);
    tests.emplace_back("Glass motion: abandoned animations are swept", TestGlassMotionStaleSweep);
    tests.emplace_back("Glass motion: fast pointer sweeps don't light rows", TestGlassVelocityGate);
    tests.emplace_back("Glass layout: reveal moves the layout and clips hit-testing", TestGlassRevealClipGuard);
    tests.emplace_back("Glass layout: staggered rows keep ImGui's layout valid", TestGlassRevealRowsKeepLayoutValid);
    tests.emplace_back("Glass layout: moving buttons stay clickable", TestGlassMotionButtonPolicy);
    tests.emplace_back("Glass motion: springs, curves and stagger", TestGlassSprings);
}
