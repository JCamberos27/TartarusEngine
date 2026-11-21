#pragma once
#include <imgui.h>
#include <imgui_internal.h>

// --editor-tests: ImGui calls ImGuiTestEngineHook_* in whichever module holds its core (TartarusCore.dll,
// or the exe and TartarusEditor.dll each without the core DLL). ImGuiTestHooks.cpp, built with ImGui,
// defines those as forwarders that call through this table, which the harness stores in
// ImGuiContext::TestEngine (unused otherwise) while a test run is active. So items from every module,
// the module-drawn menu bar, Console and Assets grid included, are addressable by label.
struct EditorTestHookTable {
    void (*ItemAdd)(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) = nullptr;
    void (*ItemInfo)(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) = nullptr;
};
