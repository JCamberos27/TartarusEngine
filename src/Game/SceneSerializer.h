#pragma once
#include <string>

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
    // have to touch disk on every step.
    std::string SaveToString(const World& world);
    bool LoadFromString(World& world, AssetLibrary& assets, const std::string& data);
}
