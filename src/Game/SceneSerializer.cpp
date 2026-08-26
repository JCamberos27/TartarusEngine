#include "SceneSerializer.h"
#include "World.h"
#include "AssetLibrary.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"

#include <json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>

using json = nlohmann::json;

namespace {

json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 JsonToVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() != 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

std::string PathOrEmpty(const std::shared_ptr<Texture>& tex) {
    return tex ? tex->Path() : std::string();
}

std::shared_ptr<Texture> LoadIfPresent(AssetLibrary& assets, const json& obj, const char* key) {
    if (!obj.contains(key)) return nullptr;
    std::string path = obj[key].get<std::string>();
    if (path.empty()) return nullptr;
    return assets.LoadTexture(path);
}

json BuildSceneJson(const World& world) {
    json root;
    root["skyHorizonColor"] = Vec3ToJson(world.SkyHorizonColor);
    root["skyZenithColor"] = Vec3ToJson(world.SkyZenithColor);

    json boxes = json::array();
    for (const auto& box : world.Boxes) {
        boxes.push_back({
            {"center", Vec3ToJson(box.Center)},
            {"size", Vec3ToJson(box.Size)},
            {"color", Vec3ToJson(box.Color)},
            {"rotation", Vec3ToJson(box.RotationEuler)},
            {"alive", box.Alive},
            {"name", box.Name},
        });
    }
    root["boxes"] = boxes;

    json models = json::array();
    for (const auto& pm : world.Models) {
        json m;
        m["path"] = pm.ModelRef->Path();
        m["name"] = pm.Name;
        m["position"] = Vec3ToJson(pm.Position);
        m["rotation"] = Vec3ToJson(pm.RotationEuler);
        m["scale"] = Vec3ToJson(pm.Scale);
        m["soundPath"] = pm.SoundPath;

        auto mat = pm.ModelRef->MaterialOverride();
        if (mat) {
            m["material"] = {
                {"baseColor", Vec3ToJson(mat->BaseColor)},
                {"metallic", mat->Metallic},
                {"roughness", mat->Roughness},
                {"emissiveColor", Vec3ToJson(mat->EmissiveColor)},
                {"emissiveStrength", mat->EmissiveStrength},
                {"albedoMap", PathOrEmpty(mat->AlbedoMap)},
                {"normalMap", PathOrEmpty(mat->NormalMap)},
                {"metallicRoughnessMap", PathOrEmpty(mat->MetallicRoughnessMap)},
                {"metallicMap", PathOrEmpty(mat->MetallicMap)},
                {"roughnessMap", PathOrEmpty(mat->RoughnessMap)},
                {"aoMap", PathOrEmpty(mat->AOMap)},
                {"emissiveMap", PathOrEmpty(mat->EmissiveMap)},
            };
        }
        models.push_back(m);
    }
    root["models"] = models;
    return root;
}

bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root) {
    world.Boxes.clear();
    world.Models.clear();
    if (root.contains("skyHorizonColor") || root.contains("skyZenithColor")) {
        world.SkyHorizonColor = JsonToVec3(root.value("skyHorizonColor", json::array({0.53f, 0.72f, 0.86f})), glm::vec3(0.53f, 0.72f, 0.86f));
        world.SkyZenithColor = JsonToVec3(root.value("skyZenithColor", json::array({0.20f, 0.40f, 0.75f})), glm::vec3(0.20f, 0.40f, 0.75f));
    } else if (root.contains("skyColor")) {
        // Migrate scenes saved before the sky became a horizon/zenith gradient — keep the old
        // flat color as the horizon and fall back to the default zenith blue above it.
        world.SkyHorizonColor = JsonToVec3(root["skyColor"], glm::vec3(0.53f, 0.72f, 0.86f));
        world.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    } else {
        world.SkyHorizonColor = glm::vec3(0.53f, 0.72f, 0.86f);
        world.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    }

    if (root.contains("boxes")) {
        for (const auto& b : root["boxes"]) {
            WorldBox box;
            box.Center = JsonToVec3(b.value("center", json::array({0, 0, 0})));
            box.Size = JsonToVec3(b.value("size", json::array({1, 1, 1})), glm::vec3(1.0f));
            box.Color = JsonToVec3(b.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
            box.RotationEuler = JsonToVec3(b.value("rotation", json::array({0, 0, 0})));
            box.Alive = b.value("alive", true);
            box.Name = b.value("name", std::string());
            world.Boxes.push_back(box);
        }
    }

    if (root.contains("models")) {
        for (const auto& m : root["models"]) {
            std::string modelPath = m.value("path", "");
            if (modelPath.empty()) continue;

            auto model = assets.LoadModel(modelPath);

            PlacedModel pm;
            pm.ModelRef = model;
            pm.Name = m.value("name", std::string("Model"));
            pm.Position = JsonToVec3(m.value("position", json::array({0, 0, 0})));
            pm.RotationEuler = JsonToVec3(m.value("rotation", json::array({0, 0, 0})));
            pm.Scale = JsonToVec3(m.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            pm.SoundPath = m.value("soundPath", std::string());
            if (!pm.SoundPath.empty()) assets.RegisterSound(pm.SoundPath);

            if (m.contains("material")) {
                const json& mj = m["material"];
                auto mat = std::make_shared<Material>();
                mat->BaseColor = JsonToVec3(mj.value("baseColor", json::array({1, 1, 1})), glm::vec3(1.0f));
                mat->Metallic = mj.value("metallic", 0.0f);
                mat->Roughness = mj.value("roughness", 0.5f);
                mat->EmissiveColor = JsonToVec3(mj.value("emissiveColor", json::array({0, 0, 0})));
                mat->EmissiveStrength = mj.value("emissiveStrength", 1.0f);
                mat->AlbedoMap = LoadIfPresent(assets, mj, "albedoMap");
                mat->NormalMap = LoadIfPresent(assets, mj, "normalMap");
                mat->MetallicRoughnessMap = LoadIfPresent(assets, mj, "metallicRoughnessMap");
                mat->MetallicMap = LoadIfPresent(assets, mj, "metallicMap");
                mat->RoughnessMap = LoadIfPresent(assets, mj, "roughnessMap");
                mat->AOMap = LoadIfPresent(assets, mj, "aoMap");
                mat->EmissiveMap = LoadIfPresent(assets, mj, "emissiveMap");
                model->SetMaterialOverride(mat);
            } else {
                // Model instances are cached/shared by path in AssetLibrary — if an earlier
                // action set an override on this same Model, restoring a snapshot from before
                // that must clear it, or the "old" state would still show the override.
                model->SetMaterialOverride(nullptr);
            }

            world.Models.push_back(pm);
        }
    }

    return true;
}

// Persists the Asset Browser's whole library (not just what's placed in the scene) plus its
// virtual folder structure and any renamed assets — otherwise an imported-but-unused asset,
// or one you'd organized into a folder, would simply vanish on the next launch.
void AppendAssetLibraryJson(json& root, const AssetLibrary& assets) {
    json modelPaths = json::array();
    for (const auto& m : assets.Models()) modelPaths.push_back(m->Path());
    root["libraryModels"] = modelPaths;

    json texPaths = json::array();
    for (const auto& t : assets.Textures()) texPaths.push_back(t->Path());
    root["libraryTextures"] = texPaths;

    root["librarySounds"] = assets.Sounds();
    root["assetFolders"] = assets.Folders();

    json meta = json::array();
    auto findOrCreate = [&](const std::string& key) -> json& {
        for (auto& entry : meta) {
            if (entry["path"] == key) return entry;
        }
        meta.push_back({{"path", key}});
        return meta.back();
    };
    for (const auto& [key, folder] : assets.AssetFolders()) findOrCreate(key)["folder"] = folder;
    for (const auto& [key, name] : assets.DisplayNames()) findOrCreate(key)["displayName"] = name;
    root["assetMeta"] = meta;
}

void ApplyAssetLibraryJson(AssetLibrary& assets, const json& root) {
    if (root.contains("libraryModels")) {
        for (const auto& p : root["libraryModels"]) assets.LoadModel(p.get<std::string>());
    }
    if (root.contains("libraryTextures")) {
        for (const auto& p : root["libraryTextures"]) assets.LoadTexture(p.get<std::string>());
    }
    if (root.contains("librarySounds")) {
        for (const auto& p : root["librarySounds"]) assets.RegisterSound(p.get<std::string>());
    }
    if (root.contains("assetFolders")) {
        for (const auto& f : root["assetFolders"]) assets.CreateFolder(f.get<std::string>());
    }
    if (root.contains("assetMeta")) {
        for (const auto& entry : root["assetMeta"]) {
            std::string path = entry.value("path", std::string());
            if (path.empty()) continue;
            if (entry.contains("folder")) assets.SetAssetFolder(path, entry["folder"].get<std::string>());
            if (entry.contains("displayName")) assets.SetDisplayName(path, entry["displayName"].get<std::string>());
        }
    }
}

} // namespace

bool SceneSerializer::Save(const World& world, const AssetLibrary& assets, const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "SceneSerializer: failed to open '" << path << "' for writing" << std::endl;
        return false;
    }
    json root = BuildSceneJson(world);
    AppendAssetLibraryJson(root, assets);
    out << root.dump(2);
    return true;
}

bool SceneSerializer::Load(World& world, AssetLibrary& assets, const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        std::cerr << "SceneSerializer: failed to parse '" << path << "': " << e.what() << std::endl;
        return false;
    }
    ApplyAssetLibraryJson(assets, root); // before ApplySceneJson: harmless either order, but library assets should exist first
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveToString(const World& world) {
    return BuildSceneJson(world).dump();
}

bool SceneSerializer::LoadFromString(World& world, AssetLibrary& assets, const std::string& data) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        std::cerr << "SceneSerializer: failed to parse undo snapshot: " << e.what() << std::endl;
        return false;
    }
    return ApplySceneJson(world, assets, root);
}
