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
    std::string SaveEntitiesToString(const World& world, const std::vector<entt::entity>& entities);

    // ADDS the fragment's entities to `world` without clearing it (unlike LoadFromString, which
    // replaces the whole scene), appending every newly created entity to outCreated. Used for
    // paste and for stamping out a prefab instance.
    bool AppendEntitiesFromString(World& world, AssetLibrary& assets, const std::string& data,
        std::vector<entt::entity>& outCreated);

    // Prefabs: one entity (and its descendants) saved to / instantiated from a .prefab file —
    // just an entity-subset fragment with a dedicated extension so the Asset Browser can list
    // and drag them.
    bool SavePrefab(const World& world, entt::entity root, const std::string& path);
    entt::entity InstantiatePrefab(World& world, AssetLibrary& assets, const std::string& path);
}
