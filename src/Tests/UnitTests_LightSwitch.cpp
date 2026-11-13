#include "UnitTestSupport.h"
#include "../Game/LightSwitch.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Game/SceneSerializer.h"
#include "../Renderer/Model.h"
#include "../Renderer/MaterialAsset.h"
#include "../Core/Paths.h"
#include "../Core/Log.h"
#include "AssetLibrary.h"
#include <filesystem>
#include <json.hpp>

namespace {

// The scene's Light Switch and Light Circuit blocks load and save.
void LightSwitchSceneBlocks() {
    using Json = nlohmann::json;
    World world;
    AssetLibrary assets;
    const Json sw = {{"id", 0}, {"name", "Switch"},
                     {"Light Switch", {{"Circuit", "Hall"}, {"Start On", true}, {"Rocker", "Rocker"}, {"Rocker Angle", 12.0}}}};
    const Json lamp = {{"id", 1}, {"name", "Lamp"}, {"Light Circuit", {{"Circuit", "Hall"}, {"Glow", 2.0}}}};
    CHECK(SceneSerializer::LoadFromString(world, assets, Json{{"formatVersion", 4}, {"empties", Json::array({sw, lamp})}}.dump()));
    auto sv = world.Registry.view<LightSwitchComponent>();
    auto cv = world.Registry.view<LightCircuitComponent>();
    CHECK(sv.size() == 1 && cv.size() == 1);
    if (sv.size() == 1) {
        const auto& s = sv.get<LightSwitchComponent>(sv.front());
        CHECK(s.Circuit == "Hall" && s.StartOn && s.Rocker == "Rocker" && s.RockerAngle == 12.0f);
    }
    if (cv.size() == 1) {
        const auto& c = cv.get<LightCircuitComponent>(cv.front());
        CHECK(c.Circuit == "Hall" && c.Glow == 2.0f);
    }
    const std::string saved = SceneSerializer::SaveToString(world);
    CHECK(saved.find("\"Light Switch\"") != std::string::npos && saved.find("\"Light Circuit\"") != std::string::npos);
}

// A circuit starts as its switch says; Interact on the switch turns the light on and the fitting's glow back, and off.
void LightSwitchCircuit() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        Log::Info("LightSwitchCircuit: no test model - skipped.");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    // The light.
    const entt::entity light = w.CreateEmptyEntity(glm::vec3(0.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Bulb");
    w.Registry.emplace<LightComponent>(light);
    w.Registry.emplace<LightCircuitComponent>(light).Circuit = "Room";
    // The fitting: a glowing material.
    const entt::entity fitting = w.CreateModelEntity(model, glm::vec3(3.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Shade");
    auto glowing = std::make_shared<MaterialAsset>();
    glowing->Mat.EmissiveColor = glm::vec3(1.0f, 0.8f, 0.6f);
    glowing->Mat.EmissiveStrength = 2.0f;
    w.Registry.get_or_emplace<RenderableComponent>(fitting).Materials = {glowing};
    w.Registry.emplace<LightCircuitComponent>(fitting).Circuit = "Room";
    // The switch, straight ahead of the eye.
    const glm::vec3 at(0.0f, 0.0f, -1.5f);
    const entt::entity sw = w.CreateModelEntity(model, at, glm::vec3(0.0f), glm::vec3(1.0f), "Switch");
    auto& s = w.Registry.emplace<LightSwitchComponent>(sw);
    s.Circuit = "Room";
    s.StartOn = false;
    w.RebuildWorldTransformCache();

    const auto emission = [&] { return w.Registry.get<RenderableComponent>(fitting).Materials.at(0)->Mat.EmissiveStrength; };
    LightSwitch::Update(w, glm::vec3(0.0f), glm::vec3(0, 0, -1), false); // starts: off
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<InactiveTag>(light) && emission() == 0.0f && !LightSwitch::IsOn(w, "Room"));
    CHECK(glowing->Mat.EmissiveStrength == 2.0f); // the shared material itself is never touched

    const glm::vec3 lo = model->BoundsMin(), hi = model->BoundsMax();
    const glm::vec3 aim = glm::normalize(at + (lo + hi) * 0.5f);
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f), aim) == sw);
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f), -aim) == entt::null);           // looking away
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f, 0.0f, 4.0f), aim) == entt::null); // out of reach
    LightSwitch::Update(w, glm::vec3(0.0f), aim, true);
    w.RebuildWorldTransformCache();
    CHECK(!w.Registry.all_of<InactiveTag>(light) && emission() == 2.0f && LightSwitch::IsOn(w, "Room"));
    LightSwitch::Update(w, glm::vec3(0.0f), aim, true);
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<InactiveTag>(light) && emission() == 0.0f);

    LightSwitch::SetAll(w, true);
    w.RebuildWorldTransformCache();
    CHECK(!w.Registry.all_of<InactiveTag>(light) && w.Registry.get<LightSwitchComponent>(sw).On);
}
} // namespace

void RegisterLightSwitchTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"LightSwitch: scene blocks load and save", LightSwitchSceneBlocks});
    tests.push_back({"LightSwitch: start state, Interact flips the light and the fitting's glow", LightSwitchCircuit});
}
