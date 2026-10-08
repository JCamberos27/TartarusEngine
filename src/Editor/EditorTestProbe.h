#pragma once
#include <imgui.h>
#include <imgui_internal.h>

// --editor-tests support (src/Tests/EditorUiTests.cpp). The harness finds widgets through ImGui's
// test-engine item hooks (IMGUI_ENABLE_TEST_ENGINE, set for the host executable only), which give
// every item's id, rectangle and label. Widgets whose label is only "##v" (a reflected field's
// value widget, say) get a readable name here, so a test can address "Audio Source/Min Distance".
// Outside a test run TestEngineHookItems is false and this is one branch per call.
void EditorTestProbe_Tag(ImGuiID id, const char* tag); // defined by the harness

inline bool EditorTestProbeActive() {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g && g->TestEngineHookItems;
}

// Called by EditorLayer::BeginFrame between the platform backend's NewFrame and ImGui::NewFrame,
// so a test run can drop real OS input queued by the backend and keep only its own.
using EditorTestInputFn = void (*)();
inline EditorTestInputFn& EditorTestInputHook() {
    static EditorTestInputFn fn = nullptr;
    return fn;
}

// Names the item submitted last.
inline void EditorTestTag(const char* tag) {
    if (EditorTestProbeActive()) EditorTestProbe_Tag(ImGui::GetItemID(), tag);
}
