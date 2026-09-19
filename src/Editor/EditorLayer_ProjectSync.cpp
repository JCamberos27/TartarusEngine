// #132 - keeping the open editor in step with the project folder while files change outside it
// (Explorer, git, an image editor): Unity's "Refresh". ProjectWatcher reports settled changes;
// this applies them to the Asset Database (GUIDs / .meta), the AssetLibrary (what the Asset
// Browser lists and what's loaded) and every reference in the open scene.
//
//   Renamed / moved  the asset keeps its GUID (its .meta follows it, or is moved for it), the
//                    library re-keys the loaded object in place, and every reference in the
//                    scene (reflected asset fields, models, prefab links, the sky) is re-pointed.
//   Added            gets a GUID and is imported into the library, filed in the Asset Browser
//                    folder matching its folder on disk.
//   Removed          leaves the library; objects still using it keep their loaded copy until the
//                    scene is reloaded; its now-orphaned .meta is deleted (as Unity does).
//   Modified         textures / models reimport, materials reload in place, prefab links refresh.
//                    Files the editor just wrote itself (AtomicFile) are ignored.
#include "EditorLayer.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "AtomicFile.h"
#include "ComponentRegistry.h"
#include "Components.h"
#include "EditorSettings.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"
#include "ProjectWatcher.h"
#include "SceneSerializer.h"
#include "World.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string AbsoluteOf(const std::string& stored) {
    if (stored.empty() || fs::path(stored).is_absolute()) return stored;
    return ProjectPaths::Resolve(stored);
}

bool SameFile(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && AssetDatabase::PathKey(a) == AssetDatabase::PathKey(b);
}

// `stored` (absolute or project-relative, maybe with a "#name" suffix) re-pointed from
// oldPath to newPath in the same style; unchanged when it references something else.
bool Retarget(std::string& stored, const std::string& oldPath, const std::string& newPath) {
    if (stored.empty()) return false;
    const size_t hash = stored.find('#');
    const std::string file = stored.substr(0, hash);
    if (!SameFile(AbsoluteOf(file), oldPath)) return false;
    const std::string suffix = hash == std::string::npos ? std::string() : stored.substr(hash);
    stored = (fs::path(file).is_absolute() ? newPath : ProjectPaths::Relativize(newPath)) + suffix;
    return true;
}

// Every reference to oldPath in the open scene, re-pointed. Returns how many changed.
int RetargetSceneReferences(World& world, const std::string& oldPath, const std::string& newPath) {
    int n = 0;
    auto& reg = world.Registry;
    for (const RegisteredComponent& rc : ComponentRegistry::All()) {
        bool anyPathField = false;
        for (const ReflectField& f : rc.Meta.Fields)
            anyPathField |= f.Type == ReflectFieldType::AssetRef || (f.Type == ReflectFieldType::String && f.AssetPath);
        if (!anyPathField) continue;
        for (entt::entity e : reg.storage<entt::entity>()) {
            if (!reg.valid(e) || !rc.Has(reg, e)) continue;
            void* comp = rc.Get(reg, e);
            for (const ReflectField& f : rc.Meta.Fields) {
                if (f.Type != ReflectFieldType::AssetRef && !(f.Type == ReflectFieldType::String && f.AssetPath)) continue;
                n += Retarget(*static_cast<std::string*>(f.Address(comp)), oldPath, newPath) ? 1 : 0;
            }
        }
    }
    // Placed models are per-entity instances that each carry the file path.
    for (auto [e, rend] : reg.view<RenderableComponent>().each())
        if (rend.ModelRef && SameFile(AbsoluteOf(rend.ModelRef->Path()), oldPath)) { rend.ModelRef->SetPath(newPath); ++n; }
    for (auto [e, pi] : reg.view<PrefabInstanceComponent>().each()) n += Retarget(pi.SourcePath, oldPath, newPath) ? 1 : 0;
    n += Retarget(world.SkyHdriPath, oldPath, newPath) ? 1 : 0;
    return n;
}

} // namespace

void EditorLayer::SyncProjectChanges(World& world, AssetLibrary& assets) {
    if (!ProjectWatcher::IsRunning()) return;
    bool overflowed = false;
    const std::vector<ProjectWatcher::Change> changes = ProjectWatcher::Drain(300, overflowed);
    if (overflowed) {
        // Too many changes at once for the OS journal (a branch switch, a huge copy): rescan.
        Log::Info("Project watcher: many files changed at once - rescanning the project.");
        AssetDatabase::ScanProject();
        InvalidateScenesListing();
    }
    for (const auto& c : changes) {
        using K = ProjectWatcher::Change::Kind;
        switch (c.Type) {
            case K::Renamed:  OnExternalMove(world, assets, c.OldPath, c.Path); break;
            case K::Added:    OnExternalAdd(world, assets, c.Path); break;
            case K::Removed:  OnExternalRemove(world, assets, c.Path); break;
            case K::Modified: OnExternalModify(world, assets, c.Path); break;
        }
    }
}

void EditorLayer::OnExternalMove(World& world, AssetLibrary& assets, const std::string& oldPath, const std::string& newPath) {
    const std::string type = AssetDatabase::AssetType(newPath);
    const std::string oldType = AssetDatabase::AssetType(oldPath);
    if (type.empty() && oldType.empty()) return; // not an asset (a .txt, settings.json, ...)
    if (type.empty()) { OnExternalRemove(world, assets, oldPath); return; } // renamed to a non-asset
    if (oldType.empty()) { OnExternalAdd(world, assets, newPath); return; } // a non-asset became one

    // Identity: the .meta moved along (Explorer/git move both) -> the GUID follows on its own; the
    // asset moved alone -> move its .meta for it, so references by GUID keep working.
    std::error_code ec;
    const bool metaFollowed = fs::exists(newPath + ".meta", ec);
    if (!metaFollowed && fs::exists(oldPath + ".meta", ec)) {
        AssetDatabase::NotifyMoved(oldPath, newPath);
    } else {
        AssetDatabase::ForgetPath(oldPath);
        AssetDatabase::EnsureGuid(newPath);
    }

    const bool listed = assets.RenamePath(oldPath, newPath);
    const int refs = RetargetSceneReferences(world, oldPath, newPath);

    if (type == "scene") {
        if (SameFile(m_CurrentScenePath, oldPath)) m_CurrentScenePath = newPath;
        if (SameFile(EditorSettings::Get().LastScenePath, oldPath)) {
            EditorSettings::Get().LastScenePath = newPath;
            EditorSettings::Save();
        }
        bool buildChanged = false;
        for (std::string& sc : ProjectSettings::MutableBuild().Scenes)
            if (SameFile(ProjectPaths::Resolve(sc), oldPath)) { sc = ProjectPaths::Relativize(newPath); buildChanged = true; }
        if (buildChanged) ProjectSettings::Save();
        InvalidateScenesListing();
    }
    if (type == "prefab") SceneSerializer::ClearPrefabPristineCache();
    if (refs > 0) { // the scene file's stored paths are stale until it's saved again
        m_SavedUndoDepth = -1;
        m_Dirty = true;
    }

    Log::Info("'" + ProjectPaths::Relativize(oldPath) + "' was moved to '" + ProjectPaths::Relativize(newPath) +
              "' outside the editor" + (listed || refs ? " - " + std::to_string(refs) + " reference(s) in the scene updated." : "."));
}

void EditorLayer::OnExternalAdd(World& world, AssetLibrary& assets, const std::string& path) {
    const std::string type = AssetDatabase::AssetType(path);
    if (type.empty()) return;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    if (!assets.FindListed(path).empty()) { OnExternalModify(world, assets, path); return; } // replaced
    AssetDatabase::EnsureGuid(path);

    bool imported = true;
    if (type == "texture") assets.LoadTexture(path);
    else if (type == "model") assets.LoadModel(path);
    else if (type == "audio") assets.RegisterSound(path);
    else if (type == "prefab") assets.RegisterPrefab(path);
    else if (type == "material") assets.LoadMaterial(path);
    else imported = false; // scenes, shaders, controllers, scripts, HDRIs: listed from disk / by reference
    if (type == "scene") InvalidateScenesListing();

    if (imported && assets.AssetFolder(path).empty()) {
        // File it in the Asset Browser folder matching its folder on disk (creating the path).
        const std::string rel = fs::path(ProjectPaths::Relativize(path)).parent_path().generic_string();
        if (!rel.empty() && !fs::path(rel).is_absolute()) {
            std::string prefix;
            for (const auto& part : fs::path(rel)) {
                prefix = prefix.empty() ? part.generic_string() : prefix + "/" + part.generic_string();
                assets.CreateFolder(prefix);
            }
            assets.SetAssetFolder(path, rel);
        }
    }
    if (imported || type == "scene")
        Log::Info("Imported '" + ProjectPaths::Relativize(path) + "' (added outside the editor).");
}

void EditorLayer::OnExternalRemove(World& /*world*/, AssetLibrary& assets, const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return; // already back (a save that deletes then rewrites)
    const std::string type = AssetDatabase::AssetType(path);
    AssetDatabase::ForgetPath(path);
    const int dropped = assets.ForgetRemoved(path); // a file, or everything under a deleted folder
    // Unity deletes the sidecar of an asset deleted outside it.
    if (fs::exists(path + ".meta", ec) && fs::remove(path + ".meta", ec))
        Log::Info("Removed '" + ProjectPaths::Relativize(path) + ".meta' (its asset was deleted).");
    if (type == "scene" || type.empty()) InvalidateScenesListing();
    if (type == "prefab") SceneSerializer::ClearPrefabPristineCache();
    if (SameFile(m_CurrentScenePath, path))
        Log::Warn("The open scene's file was deleted outside the editor. Save to write it back.");
    if (dropped > 0)
        Log::Warn("'" + ProjectPaths::Relativize(path) + "' was deleted outside the editor (" + std::to_string(dropped) +
                  " library entr" + (dropped == 1 ? "y" : "ies") + " removed). Objects using it keep their loaded copy until the scene is reloaded.");
}

void EditorLayer::OnExternalModify(World& world, AssetLibrary& assets, const std::string& path) {
    if (AtomicFile::WrittenBySelfRecently(path)) return; // our own save
    const std::string type = AssetDatabase::AssetType(path);
    const std::string listed = assets.FindListed(path);
    // A new file written the safe way (a temp file renamed over the target, as editors and
    // AtomicFile do) arrives as a modification: if the library doesn't list it yet, it's new.
    if (listed.empty() && type != "scene" && type != "prefab") { OnExternalAdd(world, assets, path); return; }
    bool reloaded = false;
    if (type == "texture" && !listed.empty()) reloaded = assets.ReimportTexture(listed);
    else if (type == "model" && !listed.empty()) reloaded = assets.ReimportModel(listed);
    else if (type == "material" && !listed.empty()) reloaded = assets.ReloadMaterial(listed);
    else if (type == "prefab") { SceneSerializer::ClearPrefabPristineCache(); reloaded = !listed.empty(); }
    else if (type == "scene") {
        InvalidateScenesListing();
        if (SameFile(m_CurrentScenePath, path))
            Log::Warn("The open scene changed on disk outside the editor. File > Revert Scene loads that version.");
    }
    if (reloaded) Log::Info("Reloaded '" + ProjectPaths::Relativize(path) + "' (changed outside the editor).");
}
