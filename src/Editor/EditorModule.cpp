#include "EditorModuleAPI.h"

#include <imgui.h>

namespace EditorModuleToolbar {
    void Draw(const EditorModuleHostAPI& host);
}
namespace EditorModuleConsole {
    void Draw(const EditorModuleHostAPI& host);
}
namespace EditorModuleStats {
    void Draw(const EditorModuleHostAPI& host);
}

namespace {

void OnLoad() {}
void OnUnload() {}

// This DLL compiles its own ImGui translation units, so before touching ImGui it has to be pointed
// at the host's single ImGuiContext (where all ImGui state actually lives) and at the host's
// allocator pair. Cheap enough to re-assert every frame, and doing it here rather than in OnLoad
// means it is always in place even if the host ever swaps contexts.
bool BindImGuiToHost(const EditorModuleHostAPI& host) {
    if (!host.GetImGuiContext || !host.GetImGuiAllocators) return false;

    EditorModuleImGuiAllocFn allocFn = nullptr;
    EditorModuleImGuiFreeFn freeFn = nullptr;
    void* userData = nullptr;
    host.GetImGuiAllocators(&allocFn, &freeFn, &userData);
    if (allocFn && freeFn) ImGui::SetAllocatorFunctions(allocFn, freeFn, userData);

    ImGuiContext* context = static_cast<ImGuiContext*>(host.GetImGuiContext());
    if (!context) return false;
    ImGui::SetCurrentContext(context);
    return true;
}

// Keep reloadable editor panels and tools here. Rebuild TartarusEditor while the editor is running
// and the host will swap this DLL without closing the scene.
void Draw(const EditorModuleHostAPI& host) {
    if (!BindImGuiToHost(host)) return;
    EditorModuleToolbar::Draw(host); // top chrome first
    EditorModuleConsole::Draw(host);
    EditorModuleStats::Draw(host);
}

const EditorModuleAPI kAPI{
    kEditorModuleAPIVersion,
    &OnLoad,
    &OnUnload,
    &Draw,
};

} // namespace

extern "C" __declspec(dllexport) const EditorModuleAPI* TartarusGetEditorModuleAPI() {
    return &kAPI;
}
