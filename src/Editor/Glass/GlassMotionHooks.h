#pragma once
// The host's EditorMotionHooks table (EditorMotionHooks.h): Glass::Motion() behind plain C entry
// points, for the shared widgets and the reloadable editor module. Host-only.

#include "../EditorMotionHooks.h"

#include <imgui.h>

namespace Glass {

// The one table, filled on first use. EditorLayer installs it with EditorTheme::SetMotionHooks
// and hands it to the module (EditorModuleHostAPI::Motion).
const EditorMotionHooks& HostMotionHooks();

// The shared checkmark (also used by the success toast): a two-stroke tick drawn on along its path
// to `t`, inside the square at `pos` of side `size`.
void DrawCheckmark(ImDrawList* dl, ImVec2 pos, float size, float t, ImU32 col, float thickness);

} // namespace Glass
