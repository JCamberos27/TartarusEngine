#pragma once
#include <imgui.h>
#include <imgui_internal.h>

// --editor-tests: the host and TartarusEditor.dll each compile their own ImGui core but share one
// ImGuiContext, so an item submitted from the module goes through the MODULE's ItemAdd and calls
// the module's ImGuiTestEngineHook_* functions, not the host's. The module defines those as
// forwarders (EditorModuleTestHooks.cpp) that call through this table, which the host's harness
// stores in ImGuiContext::TestEngine (unused otherwise) while a test run is active. That makes
// the menu bar, Console, Assets grid and the other module-drawn widgets addressable by label too.
struct EditorTestHookTable {
    void (*ItemAdd)(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) = nullptr;
    void (*ItemInfo)(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) = nullptr;
};
