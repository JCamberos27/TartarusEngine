// The Inspector panel's window frame — Begin("Inspector") + End + visibility — living inside
// TartarusEditor.dll so the panel chrome hot-reloads. Issue #229.
//
// Frame only: the Inspector has no toolbar / search / button chrome, just the window and a
// ~1090-line body (every component editor, the PBR material editor, add-component, per-field
// undo). That body stays host code — EnTT, Components.h and the material shared_ptr never cross
// the DLL boundary — and is drawn into this window through host.DrawInspectorBody().

#include "EditorModuleAPI.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

namespace EditorModuleInspector {

namespace {

// EditorInternal::PushTabChromeText — used to keep the dock tab bar's text white against
// Windows XP's saturated-green tab chrome. Phase 1 item 9 removed that theme; Light's tabs are
// neutral and already pair with its own normal text colour, so this is a permanent no-op now
// (see EditorLayerInternal.h's PanelChromeIsLight for the full explanation).
bool PanelChromeIsLight() {
    return false;
}
void PushTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
}
void PopTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PopStyleColor();
}

} // namespace

void Draw(const EditorModuleHostAPI& host) {
    if (host.GetShowInspector && !host.GetShowInspector()) return;

    bool visible = true;
    PushTabChromeText();
    const bool open = ImGui::Begin("Inspector", &visible, ImGuiWindowFlags_None);
    PopTabChromeText();
    if (host.SetShowInspector) host.SetShowInspector(visible); // capture the title-bar X
    if (!open) { ImGui::End(); return; }

    // The padlock is drawn by the host, right-aligned on the name row — a docked panel has no
    // title bar to hang it off, so it rides the first content row instead (#236 R2).
    if (host.DrawInspectorBody) host.DrawInspectorBody();

    ImGui::End();
}

} // namespace EditorModuleInspector
