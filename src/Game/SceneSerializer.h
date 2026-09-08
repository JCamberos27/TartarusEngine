#pragma once
#include <string>
#include <vector>
#include <entt/entt.hpp>

class World;
class AssetLibrary;

// Persists the placed level geometry, imported models, transforms, and any custom
// PBR material overrides to a JSON file — and reloads them, re-importing each
// referenced asset from its original path via AssetLibrary.
namespace SceneSerializer {
    // Also persists the Asset Browser's virtual folder structure and any renamed assets
    // (AssetLibrary), so organizing your assets into folders survives a relaunch.
    bool Save(const World& world, const AssetLibrary& assets, const std::string& path);

    // Clears world.Boxes/world.Models and repopulates them from the file.
    // Returns false (leaving world untouched) if the file doesn't exist or fails to parse.
    bool Load(World& world, AssetLibrary& assets, const std::string& path);

    // Every full scene (not an entity-subset fragment) carries a "formatVersion" integer at its
    // root, written by Save/SaveToString and checked on every load (#195). A file whose
    // formatVersion is newer than this build's is still loaded best-effort — unrecognized fields
    // are simply ignored by the existing optional-field reads — but is very likely to be missing
    // data this build doesn't know how to read, so the load also logs to the Console and stashes
    // a warning here for the caller to surface loudly (e.g. a modal dialog). Returns empty if the
    // most recent Load/LoadFromString/AppendEntitiesFromString call had no such warning; reading
    // it clears it, so the same warning is never shown twice.
    std::string TakeLoadWarning();

    // Same serialization, in-memory — used for the editor's undo/redo history so it doesn't
    // have to touch disk on every step. Entity-only (no AssetLibrary state) — used for the
    // play-mode enter/exit snapshot, where nothing asset-related can change anyway.
    std::string SaveToString(const World& world);
    // AssetLibrary-aware overload — used by undo/redo, so an asset import/rename/delete/folder
    // move is undoable too, not just entity edits. LoadFromString restores this as a true
    // replace (see its own comment) rather than the file-Load path's additive merge, so an
    // asset created or deleted after this snapshot was taken is correctly gone/restored on
    // load, not left merged with whatever's currently in the library.
    std::string SaveToString(const World& world, const AssetLibrary& assets);
    bool LoadFromString(World& world, AssetLibrary& assets, const std::string& data);

    // --- Entity subsets (clipboard + prefabs) ---------------------------------------------
    // The same JSON schema as a full scene, restricted to `entities` (plus, implicitly, all of
    // their descendants — copying a parent always brings its children). Sky settings and the
    // asset library are omitted, since a fragment shouldn't overwrite either on paste.
    // flattenPrefabInstances: bake any prefab-instance subtree in the selection in full instead
    // of writing it as a link stub (#236 A2). True when producing a self-contained .prefab file;
    // false (the default) for the clipboard, so a copied instance stays linked to its source.
    std::string SaveEntitiesToString(const World& world, const std::vector<entt::entity>& entities,
        bool flattenPrefabInstances = false);

    // ADDS the fragment's entities to `world` without clearing it (unlike LoadFromString, which
    // replaces the whole scene), appending every newly created entity to outCreated. Used for
    // paste and for stamping out a prefab instance.
    bool AppendEntitiesFromString(World& world, AssetLibrary& assets, const std::string& data,
        std::vector<entt::entity>& outCreated);

    // Prefabs: one entity (and its descendants) saved to / instantiated from a .prefab file.
    // SavePrefab writes a self-contained fragment (no nested links). InstantiatePrefab stamps a
    // PrefabInstanceComponent on the returned root (#236 A2) so the placement is a live instance
    // linked to `path`, not a detached copy; `outAll`, if given, receives every entity created.
    bool SavePrefab(const World& world, entt::entity root, const std::string& path);
    entt::entity InstantiatePrefab(World& world, AssetLibrary& assets, const std::string& path,
        std::vector<entt::entity>* outAll = nullptr);

    // --- Prefab per-field overrides, editor helpers (#302 Part B) -------------------------
    // `component` is a ReflectComponent::Name or the specials "Transform" / "Name".
    // True if `entity` (a member of a live prefab instance) currently holds a value for
    // (component, field) that differs from the .prefab it came from. Always false for a field
    // on the instance ROOT ("Transform" / "Name") — those are per-instance by design — and for
    // an entity that isn't part of a prefab instance. The parsed .prefab is cached; call
    // ClearPrefabPristineCache() when a scene loads or a .prefab is reimported.
    bool IsPrefabFieldOverridden(const World& world, entt::entity entity,
                                 const char* component, const char* field);
    // Set (component, field) on `entity` back to its .prefab value. No-op if not overridden.
    void RevertPrefabField(World& world, AssetLibrary& assets, entt::entity entity,
                           const char* component, const char* field);
    void ClearPrefabPristineCache();
}
