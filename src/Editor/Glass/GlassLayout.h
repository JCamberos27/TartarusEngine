#pragma once
// Layout-aware animation for immediate-mode UI: the clipping guard, animated-height reveals with a
// staggered row cascade, and the input responsiveness policy that keeps every widget clickable and
// hit-test-accurate while it moves.
//
// The two rules this file enforces (docs/EDITOR_DESIGN_SYSTEM.md, "Clipping guard" and "Input
// responsiveness policy"):
//
//  1. Motion that changes size moves the real layout. An opening section's height is the animated
//     value: everything below it is laid out against that height, so what you see is where it is.
//     Its body is laid out at full size behind a clip rect (ClipGuard) the height of the animated
//     region, so text and buttons never pop or bleed past the edge, and the parts outside the clip
//     are not hoverable either: ImGui hit-tests against the window's current ClipRect, which
//     ImGui::PushClipRect narrows (IsMouseHoveringRect intersects with it).
//
//  2. Motion that doesn't change size (a lift, a scale pop, a shake) is drawn at an offset, and the
//     widget registers its hit rect as the union of where it rests and where it is drawn
//     (MotionHitRect). The union is what makes it stable: a tile that grows when hovered stays
//     hovered at its edge instead of flickering in and out as the hover itself changes its size.
//
// Clicks are never blocked or deferred by an animation. A button inside a layout that is moving
// this frame accepts a release anywhere after a press on it (ImGuiButtonFlags_PressedOnClickRelease-
// Anywhere), so a click isn't lost because the button glided out from under the pointer between
// the press and the release.

#include "GlassTokens.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect, ImGuiButtonFlags_

namespace Glass {

// RAII for ImGui::PushClipRect/PopClipRect. Intersects with the current clip rect, so a guard
// never widens what its parent allows.
class ClipGuard {
public:
    ClipGuard(ImVec2 min, ImVec2 max) { ImGui::PushClipRect(min, max, true); }
    ~ClipGuard() { ImGui::PopClipRect(); }
    ClipGuard(const ClipGuard&) = delete;
    ClipGuard& operator=(const ClipGuard&) = delete;
};

// --- Reveals ----------------------------------------------------------------------------------------
// A section body whose height eases between 0 and its natural height:
//
//     const bool open = MyHeader(...);                 // any foldout header
//     if (Glass::BeginReveal(id, open, wasOpen)) {      // false: closed and at rest, skip the body
//         for (int i = 0; i < n; ++i) {
//             Glass::RevealRow row(i);                 // optional: staggered fade + settle per row
//             DrawRow(i);
//         }
//         Glass::EndReveal();
//     }
//
// `wasOpen` is the header's open state on the previous frame (an ImGui TreeNode/CollapsingHeader
// reports the new state the frame it's toggled, and IsItemToggledOpen() tells you it flipped, so
// wasOpen = open ^ ImGui::IsItemToggledOpen()).
//
// The natural height is measured every frame the body is submitted, so content that grows or
// shrinks mid-animation (a field appearing, a nested section opening) is followed, not snapped.
bool BeginReveal(ImGuiID id, bool open, bool wasOpen);
void EndReveal();
// 0..1 openness of the innermost reveal (1 outside any reveal).
float CurrentRevealT();
// True while the innermost reveal is still moving.
bool CurrentRevealMoving();

// One row inside a reveal: fades in and settles from RevealRowShiftPx below, `index` steps behind
// the row above. The offset is applied to the layout cursor and removed after the row, so the row's
// hit rect moves with it and the rows after it are unaffected. No-op outside a reveal or at rest.
class RevealRow {
public:
    explicit RevealRow(int index);
    ~RevealRow();
    RevealRow(const RevealRow&) = delete;
    RevealRow& operator=(const RevealRow&) = delete;
private:
    float m_Shift = 0.0f;
    bool  m_PushedAlpha = false;
};

// --- Input responsiveness ---------------------------------------------------------------------------
// True while any layout animation is running this frame (a reveal moving, or one marked with
// MarkLayoutInMotion). Reset by Glass::Motion().NewFrame() via LayoutMotionNewFrame().
bool LayoutInMotion();
void MarkLayoutInMotion();
void LayoutMotionNewFrame();

// The ButtonBehavior press policy for a widget in a layout that may be moving (see rule above).
ImGuiButtonFlags MotionButtonFlags(ImGuiButtonFlags base = 0);

// The hit rect for a widget drawn at a transform of its resting rect (rule 2).
inline ImRect MotionHitRect(const ImRect& rest, const ImRect& drawn) {
    ImRect r = rest;
    r.Add(drawn);
    return r;
}

// A button whose visuals are drawn by the caller at `drawn` while its layout slot is `rest`.
// Registers the item with the union hit rect and the motion press policy, then reserves `rest` in
// the layout. Returns pressed; hovered / held come back through the out-params.
bool MotionButton(ImGuiID id, const ImRect& rest, const ImRect& drawn, bool* outHovered, bool* outHeld,
                  ImGuiButtonFlags flags = 0);

} // namespace Glass
