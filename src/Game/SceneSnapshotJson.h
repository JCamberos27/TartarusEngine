#pragma once
// SceneSerializer's undo-snapshot load from an already-parsed snapshot. Kept out of
// SceneSerializer.h so its many includers don't all pull in json.hpp.
#include <string>
#include <json.hpp>

class World;
class AssetLibrary;

namespace SceneSerializer {
    // LoadFromString on a snapshot the caller has already parsed (undo / redo get it parsed from
    // UndoDelta::Pop), so a multi-MB scene isn't parsed twice.
    bool LoadFromJson(World& world, AssetLibrary& assets, const nlohmann::json& root);
}
