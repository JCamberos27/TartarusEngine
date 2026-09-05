#include "EditorModuleAPI.h"

namespace {

void OnLoad() {}
void OnUnload() {}

// Keep experimental editor panels and tools here. Rebuild TartarusEditor while the editor is
// running and the host will swap this DLL without closing the scene.
void Draw(const EditorModuleHostAPI& host) {
    if (host.DrawStatusPanel) {
        host.DrawStatusPanel("Live Editor Module",
                             "This panel is drawn by TartarusEditor.dll. Rebuild that target to reload editor tools live.",
                             "Reloadable editor tools are active");
    }
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
