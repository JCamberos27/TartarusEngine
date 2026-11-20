#pragma once
// SceneSerializer's undo-snapshot entry points that take an already-parsed snapshot. Kept out of
// SceneSerializer.h so its many includers don't all pull in json.hpp.
#include <string>
#include <json.hpp>

class World;
class AssetLibrary;

namespace SceneSerializer {
    // LoadFromString on a snapshot the caller has already parsed (undo / redo get it parsed from
    // UndoDelta::Pop), so a multi-MB scene isn't parsed twice.
    bool LoadFromJson(World& world, AssetLibrary& assets, const nlohmann::json& root);
    // What SaveToString(world, assets) returns right after LoadFromJson(world, assets, loaded)
    // succeeded, without serializing every entity again: `loadedText` (the text `loaded` was
    // parsed from) when the AssetLibrary half still matches, else `loaded` with the live
    // AssetLibrary half. Relies on a snapshot loading back to the same scene, which the editor's
    // "scenes:" tests check on every shipped scene.
    std::string SnapshotAfterLoad(const nlohmann::json& loaded, const std::string& loadedText, const AssetLibrary& assets);
}
