#include "EditorLayer.h"
#include "ScriptIDE.h"

// The Script IDE as the rest of the editor sees it. It is created on first use and drawn by
// EditorLayer::Draw; these entry points work before that too.
void EditorLayer::OpenScriptIDE(const std::string& path, int line, int column) {
    if (!m_ScriptIDE) m_ScriptIDE = std::make_unique<ScriptIDE>();
    if (path.empty()) m_ScriptIDE->Show();
    else m_ScriptIDE->Open(path, line, column);
}
bool EditorLayer::HasUnsavedScripts() const { return m_ScriptIDE && m_ScriptIDE->HasUnsavedFiles(); }
bool EditorLayer::ScriptIDEHasKeyboardFocus() const { return m_ScriptIDE && m_ScriptIDE->OwnsKeyboard(); }
bool EditorLayer::SaveScripts() { return !m_ScriptIDE || m_ScriptIDE->SaveAll(); }
void EditorLayer::DiscardScripts() { if (m_ScriptIDE) m_ScriptIDE->DiscardAll(); }
