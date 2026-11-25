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
//   Modified         textures / models reimport, materials reload in place, audio reloads,
//                    placed prefab instances rebuild from the edited .prefab with their
//                    overrides kept (RefreshPrefabInstances, via the "prefab" hook). Files the
//                    editor just wrote itself (AtomicFile) are ignored here.
// After the editor's own handling every change also goes to AssetChangeHooks, where other
// caches (controllers, decals, ...) register for their asset types instead of polling mtimes.
#include "EditorLayer.h"
#include "AssetChangeHooks.h"
#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "AudioEngine.h"
#include "AtomicFile.h"
#include "ComponentRegistry.h"
#include "Enhancers/EnhancerUserState.h" // renamed assets keep their favorites / bookmarks
#include "Components.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "Log.h"
#include "Model.h"
#include "MotionRebake.h"
#include "OutfitSystem.h"
#include "Openable.h" // BakePosed
#include "PhysicsWorld.h"
#include "Core/Paths.h"
#include "ProjectSettings.h"
#include "ProjectWatcher.h"
#include "SceneSerializer.h"
#include "World.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include "RenderSettingsEpoch.h"

namespace fs = std::filesystem;

namespace {

// External changes the handlers below actually applied (an import, a reload, a move) during the current
// SyncProjectChanges: it plays the "done" chime once for the lot. A file-static so EditorLayer.h stays untouched.
int s_SyncApplied = 0;

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
    n += Retarget(world.Render.SkyHdriPath, oldPath, newPath) ? 1 : 0;
    return n;
}

// Whether a file or folder appearing / going / moving changes what EditorLayer's project asset
// index lists: an asset file of a listed kind, or a folder (no extension - it may hold some).
// Library/ never counts: thumbnails are written there while the browser is open.
bool ChangesAssetIndex(const std::string& path) {
    std::string rel = ProjectPaths::Relativize(path);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    for (char& ch : rel) ch = (char)std::tolower((unsigned char)ch);
    if (rel == "library" || rel.rfind("library/", 0) == 0) return false;
    const std::string type = AssetDatabase::AssetType(path);
    if (type == "model" || type == "texture" || type == "material" || type == "audio" || type == "prefab" ||
        type == "animatorcontroller" || type == "firstpersonanimationset" || type == "recoil" || type == "camerashake" || type == "hdri" || type == "script" ||
        type == "decal" || type == "decalpalette" || type == "graffiti")
        return true;
    return type.empty() && fs::path(path).extension().empty();
}

// A wardrobe definition (.wardrobe, any case, as the rest of the sync compares paths).
bool IsWardrobe(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    return ext == ".wardrobe";
}

} // namespace

void EditorLayer::SyncProjectChanges(World& world, AssetLibrary& assets) {
    if (!ProjectWatcher::IsRunning()) { RefreshPrefabInstances(world, assets); return; } // the editor's own Apply to Prefab still queues
    bool overflowed = false;
    const std::vector<ProjectWatcher::Change> changes = ProjectWatcher::Drain(300, overflowed);
    s_SyncApplied = 0;
    // Written or renamed by the editor itself: not news, though caches still hear of it (AssetChangeHooks).
    auto bySelf = [this](const ProjectWatcher::Change& c) {
        if (AtomicFile::WrittenBySelfRecently(c.Path)) return true;
        for (const auto& move : m_SelfMoves)
            if (SameFile(move.first, c.OldPath) && SameFile(move.second, c.Path)) return true;
        return false;
    };
    const bool external = overflowed || !std::all_of(changes.begin(), changes.end(), bySelf);
    if (external) {
        FinishGlobalUndo(world,true);
        JournalUnrecordedChange(world); // #15 - the rebase below would fold it into the baseline
    }
    if (overflowed) {
        // Too many changes at once for the OS journal (a branch switch, a huge copy): rescan.
        Log::Info("Project watcher: many files changed at once - rescanning the project.");
        AssetDatabase::ScanProject();
        InvalidateScenesListing();
        InvalidateProjectAssetIndex();
        ++s_SyncApplied;
        AssetChangeHooks::Event e;
        e.Type = AssetChangeHooks::Event::Kind::Rescan;
        s_SyncApplied += AssetChangeHooks::Dispatch(e);
    }
    for (const auto& c : changes) {
        using K = ProjectWatcher::Change::Kind;
        const bool self = bySelf(c); // before the handlers: OnExternalMove consumes the m_SelfMoves entry
        if (c.Type == K::Modified) {
            const std::string changed = fs::path(c.Path).lexically_normal().string();
            m_AssetThumbs.Invalidate(changed);
            m_SoundWaveforms.Invalidate(changed);
            m_SoundWaveforms.Invalidate(ProjectPaths::Relativize(changed));
        }
        else if (ChangesAssetIndex(c.Path) || (c.Type == K::Renamed && ChangesAssetIndex(c.OldPath)))
            InvalidateProjectAssetIndex(); // the Asset Browser's list of files on disk
        switch (c.Type) {
            case K::Renamed:  OnExternalMove(world, assets, c.OldPath, c.Path); break;
            case K::Added:    OnExternalAdd(world, assets, c.Path); break;
            case K::Removed:  OnExternalRemove(world, assets, c.Path); break;
            case K::Modified: OnExternalModify(world, assets, c.Path); break;
        }
        using HK = AssetChangeHooks::Event::Kind;
        AssetChangeHooks::Event e;
        e.Type = c.Type == K::Renamed ? HK::Moved : c.Type == K::Added ? HK::Added : c.Type == K::Removed ? HK::Removed : HK::Modified;
        e.Path = c.Path;
        e.OldPath = c.OldPath;
        e.AssetType = AssetDatabase::AssetType(c.Path);
        e.BySelf = self;
        int applied = AssetChangeHooks::Dispatch(e);
        // A Physic Material saved (the Inspector or outside) while playing: the colliders using it pick it up now.
        if (e.AssetType == "physicmaterial" && e.Type != HK::Removed && PhysicsWorld::RefreshPhysicMaterial(world, c.Path) > 0)
            ++applied;
        // A wardrobe edited (it has no asset type: no .meta): the Character Outfit Inspector reads it again, as Rescan would.
        if (IsWardrobe(c.Path) && OutfitSystem::ForgetCatalog(ProjectPaths::Relativize(c.Path)))
            ++applied;
        if (!c.OldPath.empty() && IsWardrobe(c.OldPath))
            OutfitSystem::ForgetCatalog(ProjectPaths::Relativize(c.OldPath));
        if (!self) s_SyncApplied += applied;
    }
    if (RefreshPrefabInstances(world, assets) > 0) ++s_SyncApplied; // queued by the "prefab" hook above
    if (!external) return;
    ++m_ExternalSyncs;
    if (s_SyncApplied > 0) EditorSound::Play(EditorSound::Event::SyncDone); // heard from the background too
    // Refresh after external library/reference mutations - now, not at the next EnsureUndoScene:
    // anything changing the scene in between (later this frame) would be folded into it (#15).
    m_CurrentUndoScene.clear();
    m_AssetsPtr = &assets;
    EnsureUndoScene(world);
    if (!m_CurrentUndoScene.empty()) m_OutfitLandedSeen = OutfitSystem::LandedGeneration();
}

int EditorLayer::RefreshPrefabInstances(World& world, AssetLibrary& assets) {
    if (m_PrefabRefreshQueue.empty() && !m_PrefabRefreshAll) return 0;
    std::vector<std::string> queued = std::move(m_PrefabRefreshQueue);
    m_PrefabRefreshQueue.clear();
    const bool all = m_PrefabRefreshAll;
    m_PrefabRefreshAll = false;
    const bool joinsUndo = m_PrefabRefreshJoinsUndo && m_GlobalUndoActive;
    m_PrefabRefreshJoinsUndo = false;
    // Play and Prefab Mode end by reloading the scene from a snapshot taken before them, which
    // rebuilds every instance from the file as it is then, with the snapshot's overrides.
    if (m_InPlayMode || InPrefabMode()) return 0;

    // Which placed instances are stale: built from content the file no longer holds, or a
    // missing-source placeholder whose .prefab is back. Each source is checked once.
    struct Source { bool Stale = false; std::string Name; };
    std::unordered_map<std::string, Source> sources; // by PathKey
    auto matches = [&](const PrefabInstanceComponent& pi) {
        return !pi.SourcePath.empty() &&
               (all || std::any_of(queued.begin(), queued.end(), [&](const std::string& q) { return SameFile(q, pi.SourcePath); }));
    };
    for (auto [root, pi] : world.Registry.view<const PrefabInstanceComponent>().each()) {
        if (!matches(pi)) continue;
        auto [it, fresh] = sources.try_emplace(AssetDatabase::PathKey(pi.SourcePath));
        if (fresh) {
            it->second.Stale = SceneSerializer::PrefabChangedSinceInstanced(pi.SourcePath);
            it->second.Name = fs::u8path(pi.SourcePath).filename().u8string();
        }
        std::error_code ec;
        if (pi.Missing && fs::exists(fs::u8path(pi.SourcePath), ec)) it->second.Stale = true;
    }
    int instances = 0;
    std::string names;
    for (auto [root, pi] : world.Registry.view<const PrefabInstanceComponent>().each())
        if (matches(pi) && sources[AssetDatabase::PathKey(pi.SourcePath)].Stale) ++instances;
    for (const auto& [key, src] : sources)
        if (src.Stale) names += (names.empty() ? "" : ", ") + src.Name;
    if (instances == 0) return 0;

    // The scene as it stands, with every override diffed against what its instance was built
    // from (the pristine cache still holds the old content), reloaded: each instance is rebuilt
    // from the file now and its overrides replayed on top, like an undo step loading a snapshot.
    // The scene file would store the same links and overrides, so no undo step and no "*".
    // An open step that wrote the file (Apply to Prefab) takes the rebuild in (its commit sees the
    // scene with the applied override gone); any other open edit is committed first.
    if (!joinsUndo) FinishGlobalUndo(world, true);
    // The selection, by order (a member of an instance by its root's order and prefab-local index:
    // rebuilt members get new orders).
    const std::vector<int> selected = CaptureSelectedOrders(world);
    const std::string snapshot = !m_GlobalUndoActive && UndoBaselineIsCurrent() ? m_CurrentUndoScene : SceneSerializer::SaveToString(world, assets);
    {
        struct Applying { bool& Flag; Applying(bool& f) : Flag(f) { Flag = true; } ~Applying() { Flag = false; } } applying(m_GlobalUndoApplying);
        if (!SceneSerializer::LoadFromString(world, assets, snapshot)) {
            Log::Error("Couldn't rebuild the prefab instances from the edited .prefab; reload the scene to see the change.");
            return 0;
        }
    }
    RestoreSelectionByOrder(world, selected);
    // The baseline is the World as it now saves: diffed against the new content, an override that
    // now equals the file is gone (Apply to Prefab's own value). Left as the old snapshot, the next
    // Undo journaled that difference as an unrecorded change and took back only that.
    // While --bake-gi has the doors posed, no baseline at all: EnsureUndoScene takes the real one once they're back.
    m_CurrentUndoScene = Openable::BakePosed() ? std::string() : SceneSerializer::SaveToString(world, assets);
    m_ChangesOutsideUndo = 0;
    Log::Info("Updated " + std::to_string(instances) + " prefab instance" + (instances == 1 ? "" : "s") + " from " + names +
              " (their overrides kept).");
    return instances;
}

void EditorLayer::OnExternalMove(World& world, AssetLibrary& assets, const std::string& oldPath, const std::string& newPath) {
    // #129 - the editor's own Rename already handled this move; drop the watcher's echo of it.
    for (auto it = m_SelfMoves.begin(); it != m_SelfMoves.end(); ++it)
        if (SameFile(it->first, oldPath) && SameFile(it->second, newPath)) { m_SelfMoves.erase(it); return; }
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

    bool listed = false;
    const int refs = ApplyAssetMove(world, assets, oldPath, newPath, listed);
    ++s_SyncApplied;
    Log::Info("'" + ProjectPaths::Relativize(oldPath) + "' was moved to '" + ProjectPaths::Relativize(newPath) +
              "' outside the editor" + (listed || refs ? " - " + std::to_string(refs) + " reference(s) in the scene updated." : "."));
}

// Everything that follows a file moving from oldPath to newPath once its GUID / .meta are
// sorted: the library entry, the open scene's references, the current / last / build scene
// paths. Returns how many scene references changed; `listed` = the library had it.
int EditorLayer::ApplyAssetMove(World& world, AssetLibrary& assets, const std::string& oldPath,
                                const std::string& newPath, bool& listed) {
    const std::string type = AssetDatabase::AssetType(newPath);
    listed = assets.RenamePath(oldPath, newPath);
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
        NoteChangeOutsideUndo(); // references rewritten in the World, not by an undo step
    }
    return refs;
}

// #129 - Rename in the Asset Browser renames the real file (and its .meta, keeping the GUID),
// like Unity, instead of only setting a display name. Keys may be project-relative or absolute;
// newKey is in the same style as oldKey. File paths and GUID sidecars join global history.
bool EditorLayer::RenameAssetFile(World& world, AssetLibrary& assets, const std::string& oldKey,
                                  const std::string& newKey, std::string& error) {
    const std::string oldAbs = AbsoluteOf(oldKey), newAbs = AbsoluteOf(newKey);
    std::error_code ec;
    if (fs::exists(newAbs, ec) && !SameFile(oldAbs, newAbs)) { error = "a file with that name already exists"; return false; }
    PushUndo(world,"Rename Asset"); // path fixups/library keys are authored scene changes as well
    AtomicFile::NotifyWillChange(oldAbs);AtomicFile::NotifyWillChange(newAbs);
    AtomicFile::NotifyWillChange(oldAbs+".meta");AtomicFile::NotifyWillChange(newAbs+".meta");
    fs::rename(oldAbs, newAbs, ec);
    if (ec) { error = ec.message(); return false; }
    if (AssetDatabase::GuidForPath(oldAbs).IsValid()) {
        AssetDatabase::NotifyMoved(oldAbs, newAbs); // moves the .meta, GUID unchanged
    } else if (fs::exists(oldAbs + ".meta", ec)) {
        fs::rename(oldAbs + ".meta", newAbs + ".meta", ec);
        AssetDatabase::EnsureGuid(newAbs);
    }
    m_SelfMoves.emplace_back(oldAbs, newAbs);
    if (m_SelfMoves.size() > 64) m_SelfMoves.erase(m_SelfMoves.begin()); // watcher off: don't grow forever

    // The library keys by the stored style, the scene fixups by absolute path.
    assets.RenamePath(oldKey, newKey);
    bool listed = false;
    const int refs = ApplyAssetMove(world, assets, oldAbs, newAbs, listed);
    // Editor Enhancers: favorites, Inspector bookmarks and Inspector tabs follow the file.
    {
        auto& us = Enhancers::EnhancerUserState::Get();
        bool changed = false;
        auto rekey = [&](std::vector<Enhancers::EditorRef>& refs) {
            bool any = false;
            for (auto& r : refs)
                if (r.Kind == Enhancers::RefKind::Asset && r.Path == oldKey) { r.Path = newKey; r.Label = fs::path(newKey).filename().string(); any = true; }
            return any;
        };
        for (auto& page : us.FavoritePages) changed |= rekey(page.Items);
        changed |= rekey(us.InspectorBookmarks);
        if (changed) us.MarkDirty();
        auto& ts = Enhancers::TabState::Get();
        if (rekey(ts.Inspector.Tabs) | rekey(ts.Inspector.Closed) | rekey(ts.Starred)) ts.MarkDirty();
    }
    m_AssetThumbs.Invalidate(oldKey);
    m_SoundWaveforms.Invalidate(oldKey);
    InvalidateProjectAssetIndex();
    InvalidateScenesListing();
    Log::Info("Renamed '" + fs::path(oldAbs).filename().string() + "' to '" + fs::path(newAbs).filename().string() + "'" +
              (refs ? " - " + std::to_string(refs) + " reference(s) in the scene updated." : "."));
    return true;
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
        // Registered in memory only: it's found again from disk on every load, so it isn't
        // written to project/settings.json (#14).
        const std::string rel = fs::path(ProjectPaths::Relativize(path)).parent_path().generic_string();
        if (!rel.empty() && !fs::path(rel).is_absolute()) assets.SetAssetFolder(path, assets.AddFolderPath(rel));
    }
    if (imported || type == "scene") {
        if (!AtomicFile::WrittenBySelfRecently(path)) ++s_SyncApplied; // a file the editor itself just created isn't news
        Log::Info("Imported '" + ProjectPaths::Relativize(path) + "' (added outside the editor).");
    }
}

void EditorLayer::OnExternalRemove(World& /*world*/, AssetLibrary& assets, const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return; // already back (a save that deletes then rewrites)
    const std::string type = AssetDatabase::AssetType(path);
    AssetDatabase::ForgetPath(path);
    const int dropped = assets.ForgetRemoved(path); // a file, or everything under a deleted folder
    // Unity deletes the sidecar of an asset deleted outside it.
    AtomicFile::NotifyWillChange(path + ".meta");
    if (fs::exists(path + ".meta", ec) && fs::remove(path + ".meta", ec))
        Log::Info("Removed '" + ProjectPaths::Relativize(path) + ".meta' (its asset was deleted).");
    if (type == "scene" || type.empty()) InvalidateScenesListing();
    // A deleted .prefab: its placed instances keep their content, and their overrides keep saving
    // against what they were built from (the cache), not lost to an unreadable file.
    if (SameFile(m_CurrentScenePath, path))
        Log::Warn("The open scene's file was deleted outside the editor. Save to write it back.");
    if (dropped > 0)
        Log::Warn("'" + ProjectPaths::Relativize(path) + "' was deleted outside the editor (" + std::to_string(dropped) +
                  " library entr" + (dropped == 1 ? "y" : "ies") + " removed). Objects using it keep their loaded copy until the scene is reloaded.");
}

void EditorLayer::OnExternalModify(World& world, AssetLibrary& assets, const std::string& path) {
    if (AtomicFile::WrittenBySelfRecently(path)) return; // our own save
    // Motion matching (hot reload): a saved bake config rebakes; a rewritten database reloads (no library entry either way).
    std::string mmBase;
    if (MotionRebake::IsConfig(path)) { MotionRebake::Request(path); ++s_SyncApplied; return; }
    if (MotionRebake::IsBakedDatabase(path, &mmBase)) { MotionRebake::NotifyDatabaseChanged(mmBase); return; }
    const std::string type = AssetDatabase::AssetType(path);
    const std::string listed = assets.FindListed(path);
    // A new file written the safe way (a temp file renamed over the target, as editors and
    // AtomicFile do) arrives as a modification: if the library doesn't list it yet, it's new.
    if (listed.empty() && type != "scene" && type != "prefab") { OnExternalAdd(world, assets, path); return; }
    bool reloaded = false;
    if (type == "texture" && !listed.empty()) reloaded = assets.ReimportTexture(listed);
    else if (type == "model" && !listed.empty()) {
        reloaded = assets.ReimportModel(listed);
        // Placed copies share the new meshes; their empty slots take the model's material remap,
        // as a scene load does, so a submesh the file gained gets its .mat now, not at the next open.
        if (reloaded) {
            const std::string key = AssetDatabase::PathKey(listed);
            for (auto [e, rc] : world.Registry.view<RenderableComponent>().each())
                if (rc.ModelRef && AssetDatabase::PathKey(rc.ModelRef->Path()) == key)
                    assets.ApplyMaterialRemap(*rc.ModelRef, rc.Materials);
                    NoteRenderSettingsChanged();
        }
    }
    else if (type == "material" && !listed.empty()) reloaded = assets.ReloadMaterial(listed);
    else if (type == "prefab") reloaded = !listed.empty(); // its instances: the "prefab" hook
    else if (type == "audio") reloaded = AudioEngine::Reload(path); // preloaded clips; streamed ones re-read on play
    else if (type == "scene") {
        InvalidateScenesListing();
        if (SameFile(m_CurrentScenePath, path))
            Log::Warn("The open scene changed on disk outside the editor. File > Revert Scene loads that version.");
    }
    if (reloaded) {
        ++s_SyncApplied;
        Log::Info("Reloaded '" + ProjectPaths::Relativize(path) + "' (changed outside the editor).");
    }
}
