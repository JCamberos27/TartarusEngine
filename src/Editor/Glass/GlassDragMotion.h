#pragma once
// The drag ghost (category 8 in GlassTokens.h). Host-only, every theme.
//
// A drag source that opts in (ImGuiDragDropFlags_SourceNoPreviewTooltip + DragGhostLabel inside its
// BeginDragDropSource block) gets a small dark chip with a glyph and label that trails the cursor
// with a slight lag instead of ImGui's tooltip. When the drag ends the chip doesn't just vanish:
//
//   * dropped on a target, it flies into the target along a short arc, shrinking as it fades;
//   * cancelled (released over nothing), it drifts back to where the drag started and fades.
//
// ImGui's own targets are detected through the payload's Delivery flag; a custom drop zone that
// handles the release itself (the Hierarchy's reorder strips) calls DragGhostDelivered.

#include <imgui.h>

namespace Glass {

// Inside BeginDragDropSource: what the chip shows this frame.
void DragGhostLabel(const char* glyph, const char* label);
// A custom drop zone took the drop; the chip flies to `target` (screen space).
void DragGhostDelivered(ImVec2 target);
// Once a frame, after every panel (EditorLayer::EndFrame).
void DrawDragGhost();

} // namespace Glass
