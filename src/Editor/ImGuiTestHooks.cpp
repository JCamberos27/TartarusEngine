// ImGui's test-engine hooks, built with ImGui's core (tartarus_imgui_core) so whichever module holds
// ImGui has them: forwarders to the --editor-tests harness through the table it puts in
// ImGuiContext::TestEngine (see EditorTestHooks.h). Outside a test run TestEngineHookItems is false
// and ImGui never calls these.

#include "EditorTestHooks.h"

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    const auto* table = ctx ? static_cast<const EditorTestHookTable*>(ctx->TestEngine) : nullptr;
    if (table && table->ItemAdd) table->ItemAdd(ctx, id, bb, data);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    const auto* table = ctx ? static_cast<const EditorTestHookTable*>(ctx->TestEngine) : nullptr;
    if (table && table->ItemInfo) table->ItemInfo(ctx, id, label, flags);
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
