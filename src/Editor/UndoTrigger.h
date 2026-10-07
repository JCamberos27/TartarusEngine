#pragma once

// When the global undo should open a snapshot (EditorLayer::BeginGlobalUndoFrame). A snapshot is
// a full scene serialization, and closing it is another, so opening one on input that can't edit
// anything costs a hitch for nothing. Camera navigation is that input: a right/middle press over
// the Scene view, any key while that drag is held (WASDQE, Shift), Alt+left-drag orbit, and bare
// modifier presses. Whatever edit follows navigation comes with its own click or key, which still
// opens a snapshot. Pure, so the unit tests can pin it down.
namespace UndoTrigger {

struct Frame {
    bool ItemActive = false;            // ImGui::IsAnyItemActive()
    bool LeftClicked = false;
    bool RightClicked = false;
    bool MiddleClicked = false;
    bool OtherClicked = false;          // X1 / X2
    bool OverViewport = false;          // EditorLayer::IsMouseOverSceneViewport()
    bool NavDragHeld = false;           // a right/middle drag that began over the viewport is held
    bool AltDown = false;
    bool GizmoHovered = false;          // ImGuizmo::IsOver(): a left press there is a handle drag
    bool KeyPressed = false;            // a non-modifier key went down this frame
};

inline bool OpensSnapshot(const Frame& f) {
    if (f.ItemActive) return true;
    if (f.OtherClicked) return true;
    if (f.LeftClicked && !(f.OverViewport && f.AltDown && !f.GizmoHovered)) return true; // Alt+LMB = orbit
    if ((f.RightClicked || f.MiddleClicked) && !f.OverViewport) return true;
    if (f.KeyPressed && !f.NavDragHeld) return true;
    return false;
}

} // namespace UndoTrigger
