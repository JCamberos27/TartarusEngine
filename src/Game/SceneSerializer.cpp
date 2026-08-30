#include "SceneSerializer.h"
#include "World.h"
#include "AssetLibrary.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"

#include "Log.h"

#include <json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <set>
#include <functional>
#include <algorithm>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

using json = nlohmann::json;

namespace {

// A non-finite component anywhere in the scene (nan/inf slipped past the Inspector, or a
// corrupt file) serializes as JSON `null` / a bare `nan` token, neither of which reloads —
// the whole scene is then lost. Scrub to 0 at the one choke point every vector passes
// through, on both write and read, and say so in the Console (#34 D4).
inline float FiniteOr(float x, float fallback, const char* what) {
    if (std::isfinite(x)) return x;
    Log::Warn(std::string("Scene: non-finite ") + what + " value replaced with " + std::to_string(fallback));
    return fallback;
}

json Vec3ToJson(const glm::vec3& v) {
    return json::array({FiniteOr(v.x, 0.0f, "vector"), FiniteOr(v.y, 0.0f, "vector"), FiniteOr(v.z, 0.0f, "vector")});
}

glm::vec3 JsonToVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() != 3) return fallback;
    if (!j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return fallback;
    return glm::vec3(FiniteOr(j[0].get<float>(), fallback.x, "vector"),
                     FiniteOr(j[1].get<float>(), fallback.y, "vector"),
                     FiniteOr(j[2].get<float>(), fallback.z, "vector"));
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

// Components that any entity kind can carry, written/read as one shared block so a box, a
// placed model, and a bare (Renderable-less) entity all round-trip them identically. Each is
// omitted entirely when absent, keeping saves of simple scenes as small as they were before
// these components existed — and keeping every field optional on load, so an older file that
// predates any of them still parses.
void WriteCommonComponents(json& j, const World& world, entt::entity entity) {
    if (const auto* order = world.Registry.try_get<OrderComponent>(entity)) j["order"] = order->Value;
    if (const auto* tag = world.Registry.try_get<TagComponent>(entity)) j["tag"] = tag->Tag;
    if (world.Registry.all_of<InactiveTag>(entity)) j["active"] = false;
    if (world.Registry.all_of<StaticTag>(entity)) j["static"] = true;

    if (const auto* light = world.Registry.try_get<LightComponent>(entity)) {
        j["light"] = {
            {"kind", light->Kind == LightComponent::Type::Spot ? "spot" : "point"},
            {"color", Vec3ToJson(light->Color)},
            {"intensity", light->Intensity},
            {"range", light->Range},
            {"spotAngle", light->SpotAngleDegrees},
        };
    }
    // Boxes get a Collider from World::CreateBox already; this only records one that was added
    // manually (to a placed model, via the Inspector's Add Component).
    if (const auto* collider = world.Registry.try_get<ColliderComponent>(entity)) {
        j["collider"] = {{"isTrigger", collider->IsTrigger}};
    }
    if (const auto* cam = world.Registry.try_get<CameraComponent>(entity)) {
        j["camera"] = {
            {"fov", cam->FovDegrees},
            {"near", cam->NearPlane},
            {"far", cam->FarPlane},
        };
    }
}

void ReadCommonComponents(const json& j, World& world, entt::entity entity) {
    if (j.contains("tag")) world.Registry.emplace_or_replace<TagComponent>(entity, j["tag"].get<std::string>());
    if (!j.value("active", true)) world.Registry.emplace_or_replace<InactiveTag>(entity);
    if (j.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(entity);

    if (j.contains("light")) {
        const json& l = j["light"];
        LightComponent light;
        light.Kind = l.value("kind", std::string("point")) == "spot"
            ? LightComponent::Type::Spot : LightComponent::Type::Point;
        light.Color = JsonToVec3(l.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
        light.Intensity = l.value("intensity", 5.0f);
        light.Range = l.value("range", 12.0f);
        light.SpotAngleDegrees = l.value("spotAngle", 35.0f);
        world.Registry.emplace_or_replace<LightComponent>(entity, light);
    }
    if (j.contains("collider")) {
        ColliderComponent collider;
        collider.IsTrigger = j["collider"].value("isTrigger", false);
        world.Registry.emplace_or_replace<ColliderComponent>(entity, collider);
    }
    if (j.contains("camera")) {
        const json& c = j["camera"];
        CameraComponent cam;
        cam.FovDegrees = c.value("fov", 60.0f);
        cam.NearPlane = c.value("near", 0.1f);
        cam.FarPlane = c.value("far", 1000.0f);
        world.Registry.emplace_or_replace<CameraComponent>(entity, cam);
    }
}

// EnTT views iterate a pool newest-entity-first. Writing in that order would reverse the scene
// on every save (load re-creates in file order, the next save reverses it again), so the file
// churns and the Hierarchy's ordering flips each time. Collecting and reversing restores
// creation order, which is stable across any number of round-trips.
template <typename View>
std::vector<entt::entity> InCreationOrder(const entt::registry& reg, View view) {
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::sort(entities.begin(), entities.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = reg.try_get<OrderComponent>(a);
        const auto* ob = reg.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b; // stable tiebreak for pre-OrderComponent data
    });
    return entities;
}

// When `only` is non-null, just the entities it names are written (an entity-subset fragment
// for the clipboard or a prefab) and the sky settings are left out, since pasting a couple of
// objects must not also overwrite the destination scene's environment.
json BuildSceneJson(const World& world, const std::set<entt::entity>* only = nullptr) {
    json root;
    auto included = [&](entt::entity e) { return !only || only->count(e) > 0; };
    if (!only) {
        root["skyHorizonColor"] = Vec3ToJson(world.SkyHorizonColor);
        root["skyZenithColor"] = Vec3ToJson(world.SkyZenithColor);
    }

    // Every box/model entity gets a stable 0-based id (assigned in the exact order written
    // below) so HierarchyComponent parent links — engine-internal entt::entity handles — can
    // round-trip through JSON as plain integers instead.
    std::unordered_map<entt::entity, int> idOf;
    int nextId = 0;
    auto assignId = [&](entt::entity e) { idOf[e] = nextId++; };
    auto parentIdOf = [&](entt::entity e) -> int {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) return -1;
        auto it = idOf.find(hier->Parent);
        return it != idOf.end() ? it->second : -1;
    };

    // An entity whose real parent isn't part of this subset becomes a "false root" once
    // written — parentIdOf reports -1 for it, exactly like a genuinely unparented entity. But
    // its TransformComponent is still LOCAL (relative to that real, excluded parent); writing
    // those raw numbers under a parentId of -1 would have a reload place it using local
    // coordinates as if they were world coordinates, landing it wherever that ratio happens to
    // put it instead of where it visually was. Decomposing the live world-space matrix instead
    // keeps a copy/prefab of a child object anchored where it actually appeared. An entity
    // whose parent IS included, or that's genuinely unparented, is untouched (returned as-is) —
    // this only ever changes behavior for the subset case (`only != nullptr`), never a full
    // scene save, since `included()` is unconditionally true there.
    auto effectiveTransform = [&](entt::entity e) -> TransformComponent {
        const auto& t = world.Registry.get<TransformComponent>(e);
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        bool becomingFalseRoot = hier && hier->Parent != entt::null && !included(hier->Parent);
        if (!becomingFalseRoot) return t;

        glm::mat4 worldMatrix = world.ComposeWorldTransform(e);
        glm::vec3 pos, scale, skew; glm::vec4 persp; glm::quat rot;
        glm::decompose(worldMatrix, scale, rot, pos, skew, persp);
        float ex, ey, ez;
        glm::extractEulerAngleYXZ(glm::mat4_cast(rot), ey, ex, ez);

        TransformComponent out;
        out.Position = pos;
        out.Scale = scale;
        out.RotationEuler = glm::degrees(glm::vec3(ex, ey, ez));
        return out;
    };

    json boxes = json::array();
    auto boxView = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent, const LevelGeometryTag>();
    auto modelViewForIds = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    // Entities with no mesh at all — lights and plain empties used as grouping pivots. They
    // live in their own array rather than "models" because every "models" entry needs a `path`
    // to reload geometry from, and these have none.
    auto emptyViewForIds = world.Registry.view<const TransformComponent, const NameComponent>(
        entt::exclude<RenderableComponent>);

    std::vector<entt::entity> boxEntities = InCreationOrder(world.Registry, boxView);
    std::vector<entt::entity> modelEntities = InCreationOrder(world.Registry, modelViewForIds);
    std::vector<entt::entity> emptyEntities = InCreationOrder(world.Registry, emptyViewForIds);

    for (auto entity : boxEntities) { if (included(entity)) assignId(entity); }
    for (auto entity : modelEntities) { if (included(entity)) assignId(entity); }
    for (auto entity : emptyEntities) { if (included(entity)) assignId(entity); }

    for (auto entity : boxEntities) {
        if (!included(entity)) continue;
        TransformComponent transform = effectiveTransform(entity);
        const auto& name = boxView.get<const NameComponent>(entity);
        const auto& renderable = boxView.get<const RenderableComponent>(entity);
        json b = {
            {"center", Vec3ToJson(transform.Position)},
            {"size", Vec3ToJson(transform.Scale)},
            {"color", Vec3ToJson(renderable.ModelRef->MeshMaterial(0).BaseColor)},
            {"rotation", Vec3ToJson(transform.RotationEuler)},
            {"name", name.Name},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        WriteCommonComponents(b, world, entity);
        boxes.push_back(b);
    }
    root["boxes"] = boxes;

    json empties = json::array();
    for (auto entity : emptyEntities) {
        if (!included(entity)) continue;
        TransformComponent transform = effectiveTransform(entity);
        const auto& name = emptyViewForIds.get<const NameComponent>(entity);
        json e = {
            {"name", name.Name},
            {"position", Vec3ToJson(transform.Position)},
            {"rotation", Vec3ToJson(transform.RotationEuler)},
            {"scale", Vec3ToJson(transform.Scale)},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        WriteCommonComponents(e, world, entity);
        empties.push_back(e);
    }
    root["empties"] = empties;

    json models = json::array();
    auto modelView = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : modelEntities) {
        if (!included(entity)) continue;
        TransformComponent transform = effectiveTransform(entity);
        const auto& name = modelView.get<const NameComponent>(entity);
        const auto& renderable = modelView.get<const RenderableComponent>(entity);
        const auto* audio = world.Registry.try_get<AudioSourceComponent>(entity);

        json m;
        m["path"] = renderable.ModelRef->Path();
        m["name"] = name.Name;
        m["position"] = Vec3ToJson(transform.Position);
        m["rotation"] = Vec3ToJson(transform.RotationEuler);
        m["scale"] = Vec3ToJson(transform.Scale);
        m["soundPath"] = audio ? audio->SoundPath : std::string();
        m["id"] = idOf[entity];
        m["parentId"] = parentIdOf(entity);

        auto mat = renderable.ModelRef->MaterialOverride();
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
        WriteCommonComponents(m, world, entity);
        models.push_back(m);
    }
    root["models"] = models;
    return root;
}

// `clearFirst` false ADDS to the existing scene instead of replacing it — the difference
// between loading a scene and pasting/instantiating a fragment into one. `outCreated`, when
// given, collects every entity this call created so the caller can select or offset them.
bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst = true, std::vector<entt::entity>* outCreated = nullptr) {
    auto created = [&](entt::entity e) { if (outCreated) outCreated->push_back(e); };
    if (clearFirst) world.Registry.clear();
    if (!clearFirst) {
        // A fragment carries no environment settings (BuildSceneJson omits them for subsets),
        // and must not disturb the scene's own.
    } else
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

    // Reconstructs HierarchyComponent parent links from the "id"/"parentId" fields written by
    // BuildSceneJson — both entries are created first (order-independent), then parents are
    // applied in a second pass so a parentId can point forward or backward in the file.
    std::unordered_map<int, entt::entity> idToEntity;
    std::vector<std::pair<entt::entity, int>> pendingParents;

    // OrderComponent (see Components.h): a full load restores it verbatim so save->load->save is
    // order-stable; a paste/prefab append (!clearFirst) keeps the fresh trailing value CreateXxx
    // already assigned, so it lands after the current scene. Legacy files with no "order" field
    // get sequential file-order values here, matching the old boxes->models->empties behaviour.
    int fallbackOrder = 0;
    int maxLoadedOrder = -1;
    auto applyOrder = [&](entt::entity e, const json& j) {
        if (!clearFirst) return;
        int order = j.value("order", fallbackOrder);
        fallbackOrder = std::max(fallbackOrder, order) + 1;
        maxLoadedOrder = std::max(maxLoadedOrder, order);
        world.Registry.emplace_or_replace<OrderComponent>(e, order);
    };

    if (root.contains("boxes")) {
        for (const auto& b : root["boxes"]) {
            // "alive" is a pre-ECS field: a shot-dead box used to be soft-deleted (kept in the
            // file, flagged inert) rather than removed, so an old save can still contain one —
            // skip creating it at all, which is externally indistinguishable from the old
            // soft-deleted state (nothing before ever displayed or collided with a dead box).
            if (!b.value("alive", true)) continue;

            glm::vec3 center = JsonToVec3(b.value("center", json::array({0, 0, 0})));
            glm::vec3 size = JsonToVec3(b.value("size", json::array({1, 1, 1})), glm::vec3(1.0f));
            glm::vec3 color = JsonToVec3(b.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
            glm::vec3 rotation = JsonToVec3(b.value("rotation", json::array({0, 0, 0})));
            std::string name = b.value("name", std::string());
            entt::entity e = world.CreateBox(center, size, color, rotation, name);
            ReadCommonComponents(b, world, e);
            applyOrder(e, b);
            created(e);

            int id = b.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = b.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("models")) {
        for (const auto& m : root["models"]) {
            std::string modelPath = m.value("path", "");
            if (modelPath.empty()) continue;

            auto model = assets.LoadModel(modelPath);

            std::string name = m.value("name", std::string("Model"));
            glm::vec3 position = JsonToVec3(m.value("position", json::array({0, 0, 0})));
            glm::vec3 rotation = JsonToVec3(m.value("rotation", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(m.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            std::string soundPath = m.value("soundPath", std::string());
            if (!soundPath.empty()) assets.RegisterSound(soundPath);

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

            entt::entity e = world.CreateModelEntity(model, position, rotation, scale, name);
            if (!soundPath.empty()) world.Registry.emplace<AudioSourceComponent>(e, soundPath);
            ReadCommonComponents(m, world, e);
            applyOrder(e, m);
            created(e);

            int id = m.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = m.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("empties")) {
        for (const auto& en : root["empties"]) {
            glm::vec3 position = JsonToVec3(en.value("position", json::array({0, 0, 0})));
            glm::vec3 rotation = JsonToVec3(en.value("rotation", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(en.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            entt::entity e = world.CreateEmptyEntity(position, rotation, scale,
                en.value("name", std::string("Empty")));
            ReadCommonComponents(en, world, e);
            applyOrder(e, en);
            created(e);

            int id = en.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = en.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (clearFirst && maxLoadedOrder >= 0) world.EnsureNextOrderAtLeast(maxLoadedOrder + 1);

    for (const auto& [child, parentId] : pendingParents) {
        auto it = idToEntity.find(parentId);
        if (it != idToEntity.end()) world.AttachChildRaw(child, it->second);
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
    root["libraryPrefabs"] = assets.Prefabs();
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
    for (const auto& [key, labels] : assets.LabelsMap()) findOrCreate(key)["labels"] = labels;
    for (const auto& [key, s] : assets.TextureSettingsMap()) {
        findOrCreate(key)["textureImport"] = {
            {"textureType", (int)s.TextureType}, {"generateMipmaps", s.GenerateMipmaps},
            {"isSRGB", s.IsSRGB}, {"isReadable", s.IsReadable}, {"filterMode", (int)s.FilterMode},
            {"wrapMode", (int)s.WrapMode}, {"maxTextureSize", s.MaxTextureSize},
        };
    }
    for (const auto& [key, s] : assets.ModelSettingsMap()) {
        findOrCreate(key)["modelImport"] = {
            {"globalScale", s.GlobalScale}, {"importNormals", s.ImportNormals},
            {"importAnimations", s.ImportAnimations}, {"importSkeleton", s.ImportSkeleton},
            {"optimizeGraph", s.OptimizeGraph}, {"materialImportMode", (int)s.MaterialImportMode},
        };
    }
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
    if (root.contains("libraryPrefabs")) {
        for (const auto& p : root["libraryPrefabs"]) assets.RegisterPrefab(p.get<std::string>());
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
            if (entry.contains("labels")) {
                std::set<std::string> labels;
                for (const auto& l : entry["labels"]) labels.insert(l.get<std::string>());
                assets.SetLabels(path, labels);
            }
            // Settings are applied AND reimported here (rather than only stored) because
            // LoadModel/LoadTexture above already imported this asset with default settings —
            // this is the first point in the load sequence where the saved settings are known.
            if (entry.contains("textureImport")) {
                const auto& t = entry["textureImport"];
                TextureImportSettings s;
                s.TextureType = (TextureImportSettings::Type)t.value("textureType", 0);
                s.GenerateMipmaps = t.value("generateMipmaps", true);
                s.IsSRGB = t.value("isSRGB", true);
                s.IsReadable = t.value("isReadable", false);
                s.FilterMode = (TextureImportSettings::Filter)t.value("filterMode", 1);
                s.WrapMode = (TextureImportSettings::Wrap)t.value("wrapMode", 0);
                s.MaxTextureSize = t.value("maxTextureSize", 2048);
                assets.SetTextureSettings(path, s);
                assets.ReimportTexture(path);
            }
            if (entry.contains("modelImport")) {
                const auto& m = entry["modelImport"];
                ModelImportSettings s;
                s.GlobalScale = m.value("globalScale", 1.0f);
                s.ImportNormals = m.value("importNormals", true);
                s.ImportAnimations = m.value("importAnimations", true);
                s.ImportSkeleton = m.value("importSkeleton", true);
                s.OptimizeGraph = m.value("optimizeGraph", true);
                s.MaterialImportMode = (ModelImportSettings::MaterialMode)m.value("materialImportMode", 0);
                assets.SetModelSettings(path, s);
                assets.ReimportModel(path);
            }
        }
    }
}

} // namespace

bool SceneSerializer::Save(const World& world, const AssetLibrary& assets, const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        Log::Error("Scene: failed to open '" + path + "' for writing.");
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
        Log::Error("Scene: failed to parse '" + path + "': " + e.what());
        return false;
    }
    ApplyAssetLibraryJson(assets, root); // before ApplySceneJson: harmless either order, but library assets should exist first
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveToString(const World& world) {
    return BuildSceneJson(world).dump();
}

std::string SceneSerializer::SaveToString(const World& world, const AssetLibrary& assets) {
    json root = BuildSceneJson(world);
    // Marks this snapshot as carrying real AssetLibrary state, so LoadFromString below knows to
    // treat it as a full replace rather than mistaking an entity-only snapshot (the play-mode
    // one) for "nothing changed, don't touch the library."
    root["hasAssetLibrarySnapshot"] = true;
    AppendAssetLibraryJson(root, assets);
    return root.dump();
}

bool SceneSerializer::LoadFromString(World& world, AssetLibrary& assets, const std::string& data) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse snapshot: ") + e.what());
        return false;
    }
    // Only a snapshot built via the AssetLibrary-aware SaveToString() overload (undo/redo)
    // carries this - the entity-only overload (the play-mode snapshot) has no library state at
    // all, and touching the library there would affect the Asset Browser on exiting Play for no
    // reason. This is a TRUE replace (an asset created/deleted/renamed after this snapshot was
    // taken ends up correctly gone/restored, not merged with whatever's here now) but a CHEAP
    // one: PruneToKeepSet only drops cache entries for assets that shouldn't survive - it never
    // touches anything that's staying, so a texture/model that's already loaded and still wanted
    // is never re-decoded or re-imported from disk. Only genuinely-missing entries (something
    // that was deleted after this snapshot, now being undone back) cost a real reimport, via the
    // ordinary LoadModel/LoadTexture calls ApplyAssetLibraryJson below already makes - an earlier
    // version of this used a full assets.Clear() instead, which reimported the ENTIRE library on
    // every single undo/redo step, however small the actual edit — a real, serious regression
    // (multi-second stalls long enough to trip a GPU driver watchdog on a real project's worth
    // of assets), not just a style choice being reverted here.
    if (root.value("hasAssetLibrarySnapshot", false)) {
        std::set<std::string> keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders;
        if (root.contains("libraryModels")) for (const auto& p : root["libraryModels"]) keepModels.insert(p.get<std::string>());
        if (root.contains("libraryTextures")) for (const auto& p : root["libraryTextures"]) keepTextures.insert(p.get<std::string>());
        if (root.contains("librarySounds")) for (const auto& p : root["librarySounds"]) keepSounds.insert(p.get<std::string>());
        if (root.contains("libraryPrefabs")) for (const auto& p : root["libraryPrefabs"]) keepPrefabs.insert(p.get<std::string>());
        if (root.contains("assetFolders")) for (const auto& f : root["assetFolders"]) keepFolders.insert(f.get<std::string>());
        assets.PruneToKeepSet(keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders);
        assets.ClearMetadataOnly();
        ApplyAssetLibraryJson(assets, root);
    }
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveEntitiesToString(const World& world, const std::vector<entt::entity>& entities) {
    // Descendants come along automatically: a fragment that kept a parent but dropped its
    // children would paste back as a visibly different object than the one that was copied.
    std::set<entt::entity> included;
    std::function<void(entt::entity)> addWithChildren = [&](entt::entity e) {
        if (e == entt::null || !world.Registry.valid(e) || !included.insert(e).second) return;
        if (const auto* hier = world.Registry.try_get<HierarchyComponent>(e)) {
            for (entt::entity child : hier->Children) addWithChildren(child);
        }
    };
    for (entt::entity e : entities) addWithChildren(e);

    return BuildSceneJson(world, &included).dump();
}

bool SceneSerializer::AppendEntitiesFromString(World& world, AssetLibrary& assets,
    const std::string& data, std::vector<entt::entity>& outCreated) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse entity fragment: ") + e.what());
        return false;
    }
    return ApplySceneJson(world, assets, root, /*clearFirst=*/false, &outCreated);
}

bool SceneSerializer::SavePrefab(const World& world, entt::entity root, const std::string& path) {
    if (!world.Registry.valid(root)) return false;
    std::ofstream out(path);
    if (!out.is_open()) {
        Log::Error("Prefab: failed to open '" + path + "' for writing.");
        return false;
    }
    // Re-parsed and re-dumped with indentation so a prefab file is human-readable/diffable,
    // unlike the compact in-memory clipboard form the same function produces.
    out << json::parse(SaveEntitiesToString(world, {root})).dump(2);
    Log::Info("Saved prefab '" + path + "'.");
    return true;
}

entt::entity SceneSerializer::InstantiatePrefab(World& world, AssetLibrary& assets, const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        Log::Error("Prefab: '" + path + "' could not be opened.");
        return entt::null;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    std::vector<entt::entity> created;
    if (!AppendEntitiesFromString(world, assets, buffer.str(), created) || created.empty()) {
        return entt::null;
    }
    // The prefab's root is whichever created entity has no parent inside the fragment.
    for (entt::entity e : created) {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) return e;
    }
    return created.front();
}
