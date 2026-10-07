#pragma once
#include "EnhancerCore.h"

#include <json.hpp>
#include <map>
#include <string>
#include <vector>

// Editor Enhancers - per-user, per-project navigation state: bookmarks (scenes, entities,
// folders, inspector targets) and the per-scene default parent. Later phases add the tab
// strips and favorites pages here too.
//
// Why per-user AND per-project: bookmarks are personal workflow (two teammates bookmark different
// things), so they don't belong in the committed project folder - but they're meaningless across
// projects, and UserPaths is one folder per user. So: one file per project under UserPaths,
// named by a hash of the project root (enhancers_<hash>.json).
//
// Writes follow EditorSettings' pattern exactly: MarkDirty() is free to call from any UI action;
// Flush() does at most one atomic write per frame. Because the write goes through AtomicFile, the
// global file undo (EditorFileHistory) journals it like editor_prefs.json - bookmarking is
// undoable, and EditorLayer::ReloadHistoryFiles reloads this file when an undo restores it.
//
// Unknown top-level keys are preserved across a load/save round trip, so a file written by a
// newer build (with sections this one doesn't know) isn't stripped by an older one.
namespace Enhancers {

class EnhancerUserState {
public:
    static EnhancerUserState& Get();

    std::vector<EditorRef> SceneBookmarks;     // vHierarchy scene selector stars
    std::vector<EditorRef> EntityBookmarks;    // vHierarchy nav-bar chips
    std::vector<EditorRef> FolderBookmarks;    // vFolders nav-bar chips
    std::vector<EditorRef> InspectorBookmarks; // vInspector nav-bar chips (entities or assets)
    // Scene GUID -> OrderComponent value of that scene's "default parent" (vHierarchy D key):
    // new objects created with no explicit parent land under it.
    std::map<std::string, int> DefaultParents;

    void Load();                       // from Path(); a missing / bad file leaves defaults
    void Reset();                      // in-memory defaults (tests, project switch)
    void MarkDirty() { m_Dirty = true; }
    void Flush();                      // atomic write if dirty (call once per frame)
    bool IsDirty() const { return m_Dirty; }

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& root);

    // %LOCALAPPDATA%\TartarusEngine\enhancers_<hash>.json for the current ProjectPaths::Root().
    // Re-derived on every call (cheap) so a project switch can't write into the wrong file.
    static std::string Path();
    // 16 hex chars of FNV-1a over the normalized (lower-case, '/'-separated, no trailing '/')
    // project root. Exposed for tests.
    static std::string ProjectKey(const std::string& projectRoot);

    static constexpr int kVersion = 1;
    static constexpr std::size_t kMaxBookmarks = 64;

private:
    EnhancerUserState() = default;
    nlohmann::json m_Unknown = nlohmann::json::object(); // keys this build doesn't own
    bool m_Dirty = false;
};

} // namespace Enhancers
