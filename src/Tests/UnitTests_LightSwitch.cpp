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
#include <cmath>
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
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
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
    // A lamp that is both: a glowing shade and a light on one entity - both follow the switch.
    const entt::entity lamp = w.CreateModelEntity(model, glm::vec3(-3.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Lamp");
    w.Registry.get_or_emplace<RenderableComponent>(lamp).Materials = {glowing};
    w.Registry.emplace<LightComponent>(lamp);
    w.Registry.emplace<LightCircuitComponent>(lamp).Circuit = "Room";
    // The switch, straight ahead of the eye.
    const glm::vec3 at(0.0f, 0.0f, -1.5f);
    const entt::entity sw = w.CreateModelEntity(model, at, glm::vec3(0.0f), glm::vec3(1.0f), "Switch");
    auto& s = w.Registry.emplace<LightSwitchComponent>(sw);
    s.Circuit = "Room";
    s.StartOn = false;
    w.RebuildWorldTransformCache();

    const auto emission = [&] { return w.Registry.get<RenderableComponent>(fitting).Materials.at(0)->Mat.EmissiveStrength; };
    const auto lampGlow = [&] { return w.Registry.get<RenderableComponent>(lamp).Materials.at(0)->Mat.EmissiveStrength; };
    LightSwitch::Update(w, glm::vec3(0.0f), glm::vec3(0, 0, -1), false); // starts: off
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<InactiveTag>(light) && emission() == 0.0f && !LightSwitch::IsOn(w, "Room"));
    CHECK(w.Registry.all_of<InactiveTag>(lamp) && lampGlow() == 0.0f);
    CHECK(glowing->Mat.EmissiveStrength == 2.0f); // the shared material itself is never touched

    const glm::vec3 lo = model->BoundsMin(), hi = model->BoundsMax();
    const glm::vec3 aim = glm::normalize(at + (lo + hi) * 0.5f);
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f), aim) == sw);
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f), -aim) == entt::null);           // looking away
    CHECK(LightSwitch::Pick(w, glm::vec3(0.0f, 0.0f, 4.0f), aim) == entt::null); // out of reach
    LightSwitch::Update(w, glm::vec3(0.0f), aim, true);
    w.RebuildWorldTransformCache();
    CHECK(!w.Registry.all_of<InactiveTag>(light) && emission() == 2.0f && LightSwitch::IsOn(w, "Room"));
    CHECK(!w.Registry.all_of<InactiveTag>(lamp) && lampGlow() == 2.0f);
    LightSwitch::Update(w, glm::vec3(0.0f), aim, true);
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<InactiveTag>(light) && emission() == 0.0f);

    LightSwitch::SetAll(w, true);
    w.RebuildWorldTransformCache();
    CHECK(!w.Registry.all_of<InactiveTag>(light) && w.Registry.get<LightSwitchComponent>(sw).On);
}

// A fitting placed straight from its model has no material slots: it draws its imported materials. The circuit must
// still take their glow out when off (the house's sconces kept glowing because only override slots were swapped).
void LightSwitchImportedGlow() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    // Its own copy of the model, so the imported material can glow without touching the shared one.
    ModelImportSettings settings;
    const auto model = Model::ImportDeferred(path, settings);
    CHECK(model != nullptr && model->MeshCount() > 0);
    if (!model || model->MeshCount() == 0) return;
    const Material saved = model->MeshMaterial(0);
    model->MeshMaterial(0).EmissiveColor = glm::vec3(1.0f, 0.8f, 0.6f);
    model->MeshMaterial(0).EmissiveStrength = 3.0f;
    World w;
    const entt::entity fitting = w.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Sconce");
    auto& r = w.Registry.get_or_emplace<RenderableComponent>(fitting);
    r.Materials.clear(); // no overrides: the imported material draws
    auto& c = w.Registry.emplace<LightCircuitComponent>(fitting);
    c.Circuit = "Hall";
    c.Glow = 0.5f;
    auto& s = w.Registry.emplace<LightSwitchComponent>(fitting);
    s.Circuit = "Hall";
    s.StartOn = false;
    w.RebuildWorldTransformCache();

    // What the renderer draws for submesh 0: the slot if set, else the imported material.
    const auto drawn = [&]() -> float {
        const auto& slots = w.Registry.get<RenderableComponent>(fitting).Materials;
        const Material& m = !slots.empty() && slots[0] ? slots[0]->Mat : model->MeshMaterial(0);
        return glm::length(m.EmissiveColor * m.EmissiveStrength);
    };
    LightSwitch::Update(w, glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0, 0, -1), false); // starts: off
    CHECK(drawn() == 0.0f);
    LightSwitch::Set(w, "Hall", true);
    CHECK(std::abs(drawn() - 0.5f * 3.0f * glm::length(glm::vec3(1.0f, 0.8f, 0.6f))) < 1e-4f);
    LightSwitch::Set(w, "Hall", false);
    CHECK(drawn() == 0.0f);
    CHECK(model->MeshMaterial(0).EmissiveStrength == 3.0f); // the model's own material is never touched

    // A lit switch's toggle: Glow When Off glows while the circuit is off, and goes dark when it's on.
    const entt::entity toggle = w.CreateModelEntity(model, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Toggle");
    w.Registry.get_or_emplace<RenderableComponent>(toggle).Materials.clear();
    auto& tc = w.Registry.emplace<LightCircuitComponent>(toggle);
    tc.Circuit = "Hall";
    tc.GlowWhenOff = true;
    w.RebuildWorldTransformCache();
    const auto toggleGlow = [&]() -> float {
        const auto& slots = w.Registry.get<RenderableComponent>(toggle).Materials;
        const Material& m = !slots.empty() && slots[0] ? slots[0]->Mat : model->MeshMaterial(0);
        return glm::length(m.EmissiveColor * m.EmissiveStrength);
    };
    LightSwitch::Update(w, glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0, 0, -1), false); // starts the new piece: Hall is off
    CHECK(toggleGlow() > 0.0f);
    LightSwitch::Set(w, "Hall", true);
    CHECK(toggleGlow() == 0.0f);
    LightSwitch::Set(w, "Hall", false);
    CHECK(toggleGlow() > 0.0f);
    model->MeshMaterial(0) = saved;
}

// The GI bake works the circuits in the edit world (SetAll) and then Stops them: the fittings get their own materials back,
// the lights their authored state, and Play starts every switch from Start On again.
void LightSwitchStopRestores() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const entt::entity light = w.CreateEmptyEntity(glm::vec3(0.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Bulb");
    w.Registry.emplace<LightComponent>(light);
    w.Registry.emplace<LightCircuitComponent>(light).Circuit = "Room";
    const entt::entity fitting = w.CreateModelEntity(model, glm::vec3(3.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Shade");
    auto glowing = std::make_shared<MaterialAsset>();
    glowing->Mat.EmissiveColor = glm::vec3(1.0f, 0.8f, 0.6f);
    glowing->Mat.EmissiveStrength = 2.0f;
    w.Registry.get_or_emplace<RenderableComponent>(fitting).Materials = {glowing};
    w.Registry.emplace<LightCircuitComponent>(fitting).Circuit = "Room";
    const entt::entity sw = w.CreateEmptyEntity(glm::vec3(0.0f, 0.0f, -1.5f), glm::vec3(0.0f), glm::vec3(1.0f), "Switch");
    auto& s = w.Registry.emplace<LightSwitchComponent>(sw);
    s.Circuit = "Room";
    s.StartOn = false;
    w.RebuildWorldTransformCache();

    LightSwitch::SetAll(w, false); // the bake's passes
    LightSwitch::SetAll(w, true);
    LightSwitch::Stop(w);           // the bake is done
    const auto& slots = w.Registry.get<RenderableComponent>(fitting).Materials;
    CHECK(slots.size() == 1 && slots[0] == glowing);
    CHECK(!w.Registry.all_of<DeactivatedTag>(light));
    CHECK(!w.Registry.get<LightSwitchComponent>(sw).Started && !w.Registry.get<LightCircuitComponent>(light).Started);

    LightSwitch::Update(w, glm::vec3(0.0f), glm::vec3(0, 0, -1), false); // Play starts: off, as Start On says
    w.RebuildWorldTransformCache();
    CHECK(w.Registry.all_of<DeactivatedTag>(light) && !LightSwitch::IsOn(w, "Room"));
    CHECK(w.Registry.get<RenderableComponent>(fitting).Materials.at(0)->Mat.EmissiveStrength == 0.0f);
}
// Two switches on one circuit (a hall with a switch at each end): flipping either one flips the circuit, and both
// switches always show the same state.
void LightSwitchTwoSwitchesOneCircuit() {
    const std::string path = ProjectPaths::Resolve("assets/Test/UVTransform/uv_transform_quad.gltf");
    if (!std::filesystem::exists(path)) {
        UnitTestSupport::Skip("no test model (assets/Test/UVTransform)");
        return;
    }
    const auto model = Model::ImportDeferred(path, ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    World w;
    const entt::entity light = w.CreateEmptyEntity(glm::vec3(5.0f, 2.0f, -2.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Bulb");
    w.Registry.emplace<LightComponent>(light);
    w.Registry.emplace<LightCircuitComponent>(light).Circuit = "Stairs";
    // One switch at each end, 10 m apart: each is only in reach from its own end.
    const glm::vec3 atA(0.0f, 0.0f, -1.5f), atB(10.0f, 0.0f, -1.5f);
    const glm::vec3 eyeA(0.0f), eyeB(10.0f, 0.0f, 0.0f);
    const entt::entity a = w.CreateModelEntity(model, atA, glm::vec3(0.0f), glm::vec3(1.0f), "Switch Bottom");
    const entt::entity b = w.CreateModelEntity(model, atB, glm::vec3(0.0f), glm::vec3(1.0f), "Switch Top");
    w.Registry.emplace<LightSwitchComponent>(a).Circuit = "Stairs";
    w.Registry.emplace<LightSwitchComponent>(b).Circuit = "Stairs";
    w.RebuildWorldTransformCache();

    const glm::vec3 mid = (model->BoundsMin() + model->BoundsMax()) * 0.5f;
    const glm::vec3 aimA = glm::normalize(atA + mid - eyeA), aimB = glm::normalize(atB + mid - eyeB);
    const auto on = [&](entt::entity e) { return w.Registry.get<LightSwitchComponent>(e).On; };
    const auto lit = [&] { return !w.Registry.all_of<DeactivatedTag>(light); };
    LightSwitch::Update(w, eyeA, aimA, false); // starts: off (both say Start On = false)
    CHECK(!on(a) && !on(b) && !lit());
    CHECK(LightSwitch::Pick(w, eyeA, aimA) == a && LightSwitch::Pick(w, eyeB, aimB) == b);
    CHECK(LightSwitch::Pick(w, eyeA, glm::normalize(atB + mid - eyeA)) == entt::null); // the far switch: out of reach
    LightSwitch::Update(w, eyeA, aimA, true); // bottom: on
    CHECK(on(a) && on(b) && lit() && LightSwitch::IsOn(w, "Stairs"));
    LightSwitch::Update(w, eyeB, aimB, true); // top: off
    CHECK(!on(a) && !on(b) && !lit() && !LightSwitch::IsOn(w, "Stairs"));
    LightSwitch::Update(w, eyeB, aimB, true); // top: on
    CHECK(on(a) && on(b) && lit());
    LightSwitch::Update(w, eyeA, aimA, true); // bottom: off
    CHECK(!on(a) && !on(b) && !lit());
    CHECK(LightSwitch::StartOnConflicts(w).empty());
}

// Switches on one circuit that disagree on Start On: StartOnConflicts names the circuit, Play start warns once, and
// the circuit (and every switch on it) starts as the first-found switch says.
void LightSwitchStartOnDisagreement() {
    World w;
    const entt::entity light = w.CreateEmptyEntity(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Bulb");
    w.Registry.emplace<LightComponent>(light);
    w.Registry.emplace<LightCircuitComponent>(light).Circuit = "Hall";
    const auto addSwitch = [&](const char* name, const char* circuit, bool startOn) {
        const entt::entity e = w.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), name);
        auto& s = w.Registry.emplace<LightSwitchComponent>(e);
        s.Circuit = circuit;
        s.StartOn = startOn;
        return e;
    };
    const entt::entity h1 = addSwitch("Hall A", "Hall", true);
    const entt::entity h2 = addSwitch("Hall B", "Hall", false);
    addSwitch("Den A", "Den", false);
    addSwitch("Den B", "Den", false); // agrees: no warning
    w.RebuildWorldTransformCache();

    const std::vector<std::string> conflicts = LightSwitch::StartOnConflicts(w);
    CHECK(conflicts.size() == 1 && conflicts[0] == "Hall");

    const auto lastSeq = [] { const auto& es = Log::Entries(); return es.empty() ? 0ull : es.back().Seq; };
    const auto warnings = [](unsigned long long after, const std::string& text) {
        int n = 0;
        for (const LogEntry& e : Log::Entries())
            if (e.Seq > after && e.Level == LogLevel::Warning && e.Message.find(text) != std::string::npos) ++n;
        return n;
    };
    const unsigned long long before = lastSeq();
    LightSwitch::Update(w, glm::vec3(0.0f), glm::vec3(0, 0, -1), false); // Play starts
    CHECK(warnings(before, "circuit 'Hall' disagree on Start On") == 1);
    CHECK(warnings(before, "circuit 'Den'") == 0);
    // The first-found switch decides, and every switch on the circuit takes that state.
    const bool first = w.Registry.get<LightSwitchComponent>(h1).On;
    CHECK(first == w.Registry.get<LightSwitchComponent>(h2).On);
    CHECK(LightSwitch::IsOn(w, "Hall") == first && !w.Registry.all_of<DeactivatedTag>(light) == first);
    // Started: a later frame doesn't warn again.
    const unsigned long long after = lastSeq();
    LightSwitch::Update(w, glm::vec3(0.0f), glm::vec3(0, 0, -1), false);
    CHECK(warnings(after, "disagree on Start On") == 0);

    // Made to agree: no conflict left.
    w.Registry.get<LightSwitchComponent>(h2).StartOn = true;
    CHECK(LightSwitch::StartOnConflicts(w).empty());
}
} // namespace

void RegisterLightSwitchTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"LightSwitch: two switches on one circuit flip it together and stay in sync", LightSwitchTwoSwitchesOneCircuit});
    tests.push_back({"LightSwitch: switches disagreeing on Start On are reported and warned once", LightSwitchStartOnDisagreement});
    tests.push_back({"LightSwitch: Stop puts what SetAll did back as authored (the GI bake)", LightSwitchStopRestores});
    tests.push_back({"LightSwitch: scene blocks load and save", LightSwitchSceneBlocks});
    tests.push_back({"LightSwitch: start state, Interact flips the light and the fitting's glow", LightSwitchCircuit});
    tests.push_back({"LightSwitch: a fitting drawing its imported materials loses its glow when off", LightSwitchImportedGlow});
}
