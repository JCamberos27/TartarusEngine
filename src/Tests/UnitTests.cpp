// #173 - headless unit tests: `TartarusEngine.exe --unit-tests`.
//
// Runs before any window, GL context or audio device exists, so it works on a CI runner with no
// GPU (unlike --smoke-test). Almost everything here is pure C++: undo deltas,
// GUIDs, atomic file writes, texture-cache keys, material JSON robustness, the component
// registry, the Animator Controller, asset identity / GUID-following references and the project
// file watcher (#132). Each CHECK prints on failure; the run returns the number of failed checks (0 = pass).
//
// The one exception is PhysicsWorldSync (#201), which stands up a real PhysX world. PhysX needs
// no GL, and that check previously lived only in --smoke-test, which CI runs with
// continue-on-error for want of a GPU - so it never gated anything. It runs last, and shuts the
// PhysX core back down when it is done.
//
// Deliberately no test framework dependency: a CHECK macro and a list of functions is all this
// needs, and it keeps the engine's third-party surface unchanged.
#include "UnitTests.h"

#include "AnimatorController.h"
#include "AudioEngine.h"
#include "Log.h" // #178 stack traces
#include "InputMap.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
#include "Camera.h"
#include "PhysicsWorld.h"  // #201 - the physics sync regression test
#include "GameModuleAPI.h"  // QueryFilter / RaycastHit
#include <cmath>   // #202 isfinite
#include <limits>
#include "ComponentReflection.h"
#include "ComponentRegistry.h"
#include "AssetLibrary.h" // #178 preset apply
#include "MaterialAsset.h"
#include "PhysicMaterialAsset.h"
#include "PlayerConfig.h" // #174 - player.json round-trip
#include "PostProcessVolume.h"
#include "Tonemapper.h"
#include "ProjectPaths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "SceneSerializer.h" // #121
#include "UndoDeltaChain.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

int g_Failures = 0;
int g_Checks = 0;
const char* g_CurrentTest = "";

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++g_Checks;                                                                              \
        if (!(cond)) {                                                                           \
            ++g_Failures;                                                                        \
            std::cout << "[UnitTest] FAIL " << g_CurrentTest << ": " #cond " (" << __FILE__     \
                      << ":" << __LINE__ << ")\n";                                               \
        }                                                                                        \
    } while (0)

std::filesystem::path TempDir() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / "TartarusUnitTests";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// --- Camera roll (#165) --------------------------------------------------------------------
void TestCameraRoll() {
    auto near = [](glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 1e-4f; };
    Camera cam; // yaw -90: looking down -Z, right = +X, up = +Y
    CHECK(near(cam.Right(), {1, 0, 0}));
    CHECK(near(cam.Up(), {0, 1, 0}));
    const glm::mat4 level = cam.ViewMatrix();
    cam.Roll = 90.0f;
    CHECK(near(cam.Front(), {0, 0, -1}));   // roll never changes where the camera looks
    CHECK(near(cam.Right(), {0, 1, 0}));
    CHECK(near(cam.Up(), {-1, 0, 0}));
    // A point to the camera's rolled right lands on view-space +X.
    const glm::vec4 v = cam.ViewMatrix() * glm::vec4(cam.Position + glm::vec3(0, 1, -5), 1.0f);
    CHECK(v.x > 0.99f && std::abs(v.y) < 1e-4f);
    cam.Roll = 0.0f;
    const glm::mat4 back = cam.ViewMatrix();
    bool same = true;
    for (int c = 0; c < 4; ++c) same &= near(glm::vec3(level[c]), glm::vec3(back[c]));
    CHECK(same);
}

// --- LOD Group (#163) ---------------------------------------------------------------------
// #171 - Doppler velocities: metres per second between frames, zero when there's no previous
// frame and for teleports.
void TestDopplerVelocity() {
    const glm::vec3 v = AudioEngine::FrameVelocity(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.1f);
    CHECK(std::abs(v.x - 10.0f) < 1e-4f && v.y == 0.0f && v.z == 0.0f);
    CHECK(AudioEngine::FrameVelocity(glm::vec3(0.0f), glm::vec3(1.0f), 0.0f) == glm::vec3(0.0f));
    CHECK(AudioEngine::FrameVelocity(glm::vec3(0.0f), glm::vec3(50.0f, 0.0f, 0.0f), 0.1f) == glm::vec3(0.0f));
}

// #145 - Input Manager actions: keys give full tilt, the stick adds its analog value, Alt keys
// count like primaries, and the JSON round trip keeps bindings while dropping malformed entries.
void TestInputMap() {
    InputMap::Action a;
    a.Name = "Vertical";
    a.Positive = 87; a.Negative = 83; a.AltPositive = 265; a.GamepadAxis = 1; a.InvertGamepadAxis = true;
    std::set<int> down;
    float stick = 0.0f;
    InputMap::Source src{
        [&](int code) { return down.count(code) > 0; },
        [](int) { return false; },
        [&](int) { return stick; },
    };
    CHECK(InputMap::Evaluate(a, src) == 0.0f);
    down = {87};
    CHECK(InputMap::Evaluate(a, src) == 1.0f && InputMap::EvaluateButton(a, src));
    down = {83};
    CHECK(InputMap::Evaluate(a, src) == -1.0f && !InputMap::EvaluateButton(a, src));
    down = {87, 83};
    CHECK(InputMap::Evaluate(a, src) == 0.0f);
    down = {265};
    CHECK(InputMap::Evaluate(a, src) == 1.0f);
    down.clear();
    stick = -0.5f; // stick pushed up halfway (GLFW: up is negative), inverted
    CHECK(std::abs(InputMap::Evaluate(a, src) - 0.5f) < 1e-6f);
    down = {87};
    CHECK(InputMap::Evaluate(a, src) == 1.0f); // clamped

    nlohmann::json j = InputMap::ToJson({a});
    j.push_back({{"name", 5}});                        // wrong type: dropped
    j.push_back({{"name", "Bad"}, {"positive", "x"}}); // wrong-typed binding: kept, unbound
    const auto back = InputMap::FromJson(j);
    CHECK(back.size() == 2);
    if (back.size() == 2) {
        CHECK(back[0].Name == "Vertical" && back[0].Positive == 87 && back[0].Negative == 83 &&
              back[0].AltPositive == 265 && back[0].GamepadAxis == 1 && back[0].InvertGamepadAxis);
        CHECK(back[1].Name == "Bad" && back[1].Positive == InputMap::kNone);
    }
    CHECK(InputMap::FromJson(nlohmann::json::object()).empty());
    CHECK(InputMap::BindingName(InputMap::kMouseBase + 1) == "Mouse 1" && InputMap::BindingName(87) == "W");

    // Disabled (no game input): everything reads zero, including unknown names (no crash).
    InputMap::SetEnabled(false);
    CHECK(InputMap::GetAxis("Horizontal") == 0.0f && !InputMap::GetButton("Jump"));
}

// #123 - decomposes keep the Euler triple nearest the old one instead of flipping to an
// equivalent (the alternate YXZ solution and +-360 wraps are the same rotation).
void TestNearestEuler() {
    auto sameRotation = [](const glm::vec3& a, const glm::vec3& b) {
        const glm::mat4 ma = ComposeTransform(glm::vec3(0.0f), a, glm::vec3(1.0f));
        const glm::mat4 mb = ComposeTransform(glm::vec3(0.0f), b, glm::vec3(1.0f));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(ma[c][r] - mb[c][r]) > 1e-4f) return false;
        return true;
    };
    const glm::vec3 flipped(180.0f, 0.0f, 180.0f);
    const glm::vec3 near0 = NearestEquivalentEuler(flipped, glm::vec3(0.0f, 170.0f, 0.0f));
    CHECK(sameRotation(near0, flipped));
    CHECK(std::abs(near0.x) < 1e-3f && std::abs(near0.y - 180.0f) < 1e-3f && std::abs(near0.z) < 1e-3f);
    const glm::vec3 e(30.0f, -20.0f, 50.0f);
    CHECK(NearestEquivalentEuler(e, e) == e);
    const glm::vec3 wrapped = NearestEquivalentEuler(glm::vec3(10.0f, -170.0f, 0.0f), glm::vec3(10.0f, 185.0f, 0.0f));
    CHECK(std::abs(wrapped.y - 190.0f) < 1e-3f && sameRotation(wrapped, glm::vec3(10.0f, -170.0f, 0.0f)));
    const glm::vec3 alt(180.0f - 25.0f, 40.0f + 180.0f, -60.0f + 180.0f);
    CHECK(sameRotation(alt, glm::vec3(25.0f, 40.0f, -60.0f)));
    const glm::vec3 back = NearestEquivalentEuler(alt, glm::vec3(20.0f, 45.0f, -55.0f));
    CHECK(glm::length(back - glm::vec3(25.0f, 40.0f, -60.0f)) < 1e-3f);
}

// #170 - a Physic Material asset round-trips through its file and overrides the collider's own
// surface values; a missing file leaves them alone.
void TestPhysicMaterial() {
    const std::string path = (std::filesystem::temp_directory_path() / "tartarus_ut.physicmaterial").string();
    PhysicMaterialAsset m;
    m.DynamicFriction = 0.05f; m.StaticFriction = 0.1f; m.Bounciness = 0.9f; m.FrictionCombine = 1; m.BounceCombine = 3;
    CHECK(m.SaveFile(path));
    PhysicMaterialAsset back;
    CHECK(PhysicMaterialAsset::LoadFile(path, back));
    CHECK(back.DynamicFriction == 0.05f && back.StaticFriction == 0.1f && back.Bounciness == 0.9f &&
          back.FrictionCombine == 1 && back.BounceCombine == 3);
    ColliderComponent c;
    c.Material = path;
    const ColliderComponent r = ResolvePhysicMaterial(c);
    CHECK(r.Friction == 0.05f && r.Bounciness == 0.9f && r.BounceCombine == 3);
    { std::ofstream(path) << R"({"dynamicFriction": "x", "bounciness": 7, "frictionCombine": 9})"; }
    CHECK(PhysicMaterialAsset::LoadFile(path, back));
    CHECK(back.DynamicFriction == 0.6f && back.Bounciness == 1.0f && back.FrictionCombine == 0);
    std::filesystem::remove(path);
    c.Material = path;
    CHECK(ResolvePhysicMaterial(c).Friction == c.Friction);
    CHECK(!PhysicMaterialAsset::LoadFile(path, back));
}

// #162 / #203 - Post-process Volumes: full weight inside the box, linear fade over the blend
// distance, nothing beyond; overrides blend in priority order.
void TestPostProcessVolume() {
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    const glm::vec3 size(4.0f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(10.0f, 0.0f, 0.0f)) == 1.0f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(11.9f, 0.0f, 0.0f)) == 1.0f);
    CHECK(std::abs(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(13.0f, 0.0f, 0.0f)) - 0.5f) < 1e-4f);
    CHECK(PostVolumeWeight(false, size, 2.0f, 1.0f, at, glm::vec3(20.0f, 0.0f, 0.0f)) == 0.0f);
    CHECK(PostVolumeWeight(true, size, 2.0f, 0.4f, at, glm::vec3(1000.0f)) == 0.4f);
    const glm::mat4 scaled = glm::scale(at, glm::vec3(2.0f)); // box now 8 m wide
    CHECK(PostVolumeWeight(false, size, 0.0f, 1.0f, scaled, glm::vec3(13.5f, 0.0f, 0.0f)) == 1.0f);

    World world;
    const entt::entity a = world.Registry.create();
    world.Registry.emplace<TransformComponent>(a);
    auto& va = world.Registry.emplace<PostProcessVolumeComponent>(a);
    va.Global = true; va.OverrideSaturation = true; va.Saturation = -100.0f; va.Priority = 1;
    const entt::entity b = world.Registry.create();
    world.Registry.emplace<TransformComponent>(b);
    auto& vb = world.Registry.emplace<PostProcessVolumeComponent>(b);
    vb.Global = true; vb.OverrideSaturation = true; vb.Saturation = 50.0f; vb.OverrideExposure = true; vb.ExposureEV = 2.0f;
    vb.Weight = 0.5f; vb.Priority = 0;
    world.RebuildWorldTransformCache();
    PostSettings p;
    p.Saturation = 0.0f; p.ExposureEV = 0.0f;
    ApplyPostProcessVolumes(world, glm::vec3(0.0f), p);
    CHECK(p.Saturation == -100.0f);                  // b (priority 0) then a (priority 1, weight 1) wins
    CHECK(std::abs(p.ExposureEV - 1.0f) < 1e-5f);    // only b overrides exposure, at weight 0.5
    world.Registry.emplace<DeactivatedTag>(a);
    world.RebuildWorldTransformCache();
    p.Saturation = 0.0f;
    ApplyPostProcessVolumes(world, glm::vec3(0.0f), p);
    CHECK(std::abs(p.Saturation - 25.0f) < 1e-4f);   // inactive volumes don't apply
}

void TestLodGroup() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity group = world.CreateEmptyEntity(zero, zero, one, "Group");
    world.Registry.emplace<LODGroupComponent>(group).Size = 2.0f;
    entt::entity lv[3];
    for (int i = 0; i < 3; ++i) {
        lv[i] = world.CreateEmptyEntity(zero, zero, one, "LOD" + std::to_string(i));
        world.Registry.emplace<RenderableComponent>(lv[i]);
        CHECK(world.SetParent(lv[i], group));
    }
    // 90 degree FOV: yScale = 1, so a 2 m object at distance d is 1/d of the view tall.
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    auto shown = [&](float dist) {
        CHECK(world.ApplyLod(glm::vec3(0.0f, 0.0f, dist), proj) == 1);
        int visible = -1, count = 0;
        for (int i = 0; i < 3; ++i)
            if (!world.Registry.all_of<LodCulledTag>(lv[i])) { visible = i; ++count; }
        return count <= 1 ? visible : -2;
    };
    CHECK(shown(1.0f) == 0);   // 100% of the view  -> LOD 0 (>= 0.6)
    CHECK(shown(2.0f) == 1);   // 50%               -> LOD 1 (>= 0.3)
    CHECK(shown(5.0f) == 2);   // 20%               -> LOD 2 (>= 0.1)
    CHECK(shown(20.0f) == -1); // 5%, below the last used level -> culled
    world.Registry.destroy(group);
    CHECK(world.ApplyLod(zero, proj) == 0); // no groups left: nothing stays tagged
    CHECK(world.Registry.view<LodCulledTag>().empty());
}

// --- Active in hierarchy (#201) -------------------------------------------------------------
void TestActiveInHierarchy() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity parent = world.CreateEmptyEntity(zero, zero, one, "Parent");
    const entt::entity child = world.CreateEmptyEntity(zero, zero, one, "Child");
    const entt::entity grandchild = world.CreateEmptyEntity(zero, zero, one, "Grandchild");
    CHECK(world.SetParent(child, parent));
    CHECK(world.SetParent(grandchild, child));
    auto inactive = [&](entt::entity e) { return world.Registry.all_of<InactiveTag>(e); };

    world.Registry.emplace<DeactivatedTag>(parent);
    world.SyncActiveInHierarchy();
    CHECK(inactive(parent) && inactive(child) && inactive(grandchild)); // whole subtree hides

    world.Registry.emplace<DeactivatedTag>(child);
    world.Registry.remove<DeactivatedTag>(parent);
    world.SyncActiveInHierarchy();
    CHECK(!inactive(parent) && inactive(child) && inactive(grandchild));

    world.Registry.remove<DeactivatedTag>(child);
    world.SyncActiveInHierarchy();
    CHECK(!inactive(parent) && !inactive(child) && !inactive(grandchild)); // nothing left behind
}

// --- #121: asset data migrates out of scene files into .meta, and never clobbers it ---------
void TestAssetMetaMigration() {
    auto migrate = [](const std::string& entry, const std::string& meta) {
        return json::parse(SceneSerializer::MigrateAssetMetaFields(entry, meta));
    };

    // A bare sidecar takes everything the old scene entry had. textureImport lands on "importer".
    const std::string entry = R"({"path":"t.png","folder":"Props","displayName":"Crate",)"
                              R"("labels":["a","b"],"textureImport":{"isSRGB":false,"anisoLevel":4}})";
    json add = migrate(entry, R"({"guid":"g","type":"texture"})");
    CHECK(add["folder"] == "Props");
    CHECK(add["displayName"] == "Crate");
    CHECK(add["labels"].size() == 2);
    CHECK(add["importer"]["isSRGB"] == false && add["importer"]["anisoLevel"] == 4);
    CHECK(!add.contains("textureImport")); // renamed, not carried through under the old key

    // The sidecar wins wherever it already has a value: the scene copy is the stale one.
    add = migrate(entry, R"({"folder":"Newer","importer":{"isSRGB":true}})");
    CHECK(!add.contains("folder") && !add.contains("importer"));
    CHECK(add["displayName"] == "Crate" && add["labels"].size() == 2); // the rest still migrates

    // Nothing to do -> "{}", so the caller skips the .meta write entirely.
    CHECK(SceneSerializer::MigrateAssetMetaFields(entry,
              R"({"folder":"F","displayName":"D","labels":[],"importer":{}})") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields(R"({"path":"t.png"})", "{}") == "{}");

    // modelImport maps onto the same "importer" key.
    add = migrate(R"({"modelImport":{"globalScale":0.01}})", "{}");
    CHECK(add["importer"]["globalScale"] == 0.01);

    // Malformed input on either side is never a reason to guess at a sidecar write.
    CHECK(SceneSerializer::MigrateAssetMetaFields("not json", "{}") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields(entry, "not json") == "{}");
    CHECK(SceneSerializer::MigrateAssetMetaFields("[1,2]", "{}") == "{}");
}

// --- #178: component presets round-trip through the scene field encoding --------------------
void TestComponentPreset() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    World world;
    AssetLibrary assets;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity src = world.CreateEmptyEntity(zero, zero, one, "Source");
    const entt::entity dst = world.CreateEmptyEntity(zero, zero, one, "Target");

    SpinComponent spin;
    spin.Axis = glm::vec3(1.0f, 0.0f, 0.0f);
    spin.Speed = 123.5f;
    world.Registry.emplace<SpinComponent>(src, spin);

    const std::string preset = SceneSerializer::ComponentToPresetJson(world, src, "Spin");
    CHECK(!preset.empty());
    CHECK(SceneSerializer::PresetComponentName(preset) == "Spin");

    // Applies to an entity that does not have the component yet: it is added, not skipped.
    CHECK(!world.Registry.all_of<SpinComponent>(dst));
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst, preset));
    CHECK(world.Registry.all_of<SpinComponent>(dst));
    const SpinComponent& got = world.Registry.get<SpinComponent>(dst);
    CHECK(got.Speed == 123.5f);
    CHECK(got.Axis.x == 1.0f && got.Axis.y == 0.0f && got.Axis.z == 0.0f);

    // A field the preset does not mention keeps its current value rather than snapping to the
    // default, so a preset saved before a field existed stays usable.
    world.Registry.get<SpinComponent>(dst).Speed = 7.0f;
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"Spin","fields":{"Axis":[0.0,0.0,1.0]}})"));
    CHECK(world.Registry.get<SpinComponent>(dst).Axis.z == 1.0f);
    CHECK(world.Registry.get<SpinComponent>(dst).Speed == 7.0f); // untouched

    // Rejections: malformed, unknown component, missing fields block, and a component that has
    // no generic serialisation. None of these may half-write anything.
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, "not json"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"NoSuchComponent","fields":{}})"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, R"({"preset":1,"component":"Spin"})"));
    CHECK(SceneSerializer::PresetComponentName("not json").empty());
    CHECK(SceneSerializer::PresetComponentName(R"({"component":"NoSuchComponent"})").empty());

    // A component the source entity does not have yields no preset at all.
    CHECK(SceneSerializer::ComponentToPresetJson(world, src, "NoSuchComponent").empty());
}

// --- #178: Warning/Error log entries carry a resolvable call stack -------------------------
void TestLogStackTrace() {
    Log::Clear();
    Log::Info("stack-test info");
    Log::Warn("stack-test warn");
    Log::Error("stack-test error");

    const std::vector<LogEntry>& entries = Log::Entries();
    const LogEntry* info = nullptr; const LogEntry* warn = nullptr; const LogEntry* err = nullptr;
    for (const LogEntry& e : entries) {
        if (e.Message == "stack-test info")  info = &e;
        if (e.Message == "stack-test warn")  warn = &e;
        if (e.Message == "stack-test error") err = &e;
    }
    CHECK(info && warn && err);
    if (!info || !warn || !err) return;

    // Info is deliberately not instrumented; Warning and Error are.
    CHECK(info->Stack.empty());
    CHECK(!warn->Stack.empty());
    CHECK(!err->Stack.empty());

    // Asserted in BOTH configurations now that Release ships a PDB. That is the point of this
    // change: before it, resolution returned nothing in Release and the Console stack traces did
    // nothing in the build the desktop shortcut actually runs. These assertions are what fails if
    // the Release debug-info setting is ever dropped again - a silent "" would otherwise look
    // exactly like a pass.
    const std::string text = Log::ResolveStack(err->Stack);
    CHECK(!text.empty());
    CHECK(text.find("TestLogStackTrace") != std::string::npos); // the function that logged
    // Nearest frame first, and Log own frames are filtered by source file, so neither the
    // machinery nor its file may appear.
    CHECK(text.find("Log.cpp") == std::string::npos);
    CHECK(text.find("Log::Error") == std::string::npos);

    CHECK(Log::ResolveStack({}).empty());
    Log::Clear();
}

// --- #202: a scene whose parentIds form a loop must not load a cyclic hierarchy -------------
void TestHierarchyCycleRepair() {
    // Built with AttachChildRaw, which is exactly what the scene loader uses: it only refuses
    // self-parenting, so this is the shape a hand-edited or badly merged scene file produces.
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    const entt::entity a = world.CreateEmptyEntity(zero, zero, one, "A");
    const entt::entity b = world.CreateEmptyEntity(zero, zero, one, "B");
    const entt::entity c = world.CreateEmptyEntity(zero, zero, one, "C");

    world.AttachChildRaw(b, a);   // B under A
    world.AttachChildRaw(c, b);   // C under B
    world.AttachChildRaw(a, c);   // ...and A under C: a three-entity loop

    CHECK(world.RepairHierarchyCycles() == 1);

    // Every entity now reaches a root in a bounded walk.
    auto reachesRoot = [&](entt::entity e) {
        int hops = 0;
        for (entt::entity w = e; w != entt::null; ) {
            if (++hops > 64) return false;
            const auto* h = world.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        return true;
    };
    CHECK(reachesRoot(a) && reachesRoot(b) && reachesRoot(c));

    // The cut is minimal: only the link that closed the loop went, so the rest of the chain
    // is still parented and no Children list keeps a stale entry.
    int parented = 0;
    for (entt::entity e : {a, b, c})
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e); h && h->Parent != entt::null) ++parented;
    CHECK(parented == 2);
    for (entt::entity e : {a, b, c}) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h) continue;
        for (entt::entity kid : h->Children) {
            const auto* kh = world.Registry.try_get<HierarchyComponent>(kid);
            CHECK(kh && kh->Parent == e); // no orphaned child entry left behind
        }
    }

    // Idempotent, and a healthy hierarchy is left completely alone.
    CHECK(world.RepairHierarchyCycles() == 0);

    World clean;
    const entt::entity p1 = clean.CreateEmptyEntity(zero, zero, one, "P");
    const entt::entity k1 = clean.CreateEmptyEntity(zero, zero, one, "K");
    CHECK(clean.SetParent(k1, p1));
    CHECK(clean.RepairHierarchyCycles() == 0);
    CHECK(clean.Registry.get<HierarchyComponent>(k1).Parent == p1);

    // End to end: this is the path that actually matters - a scene FILE whose parentIds form a
    // loop must not produce a cyclic world. Two empties, each claiming the other as its parent.
    World loaded;
    AssetLibrary assets;
    const std::string sceneJson = R"({"formatVersion":3,"empties":[
        {"name":"X","id":1,"parentId":2,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
        {"name":"Y","id":2,"parentId":1,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}]})";
    CHECK(SceneSerializer::LoadFromString(loaded, assets, sceneJson));
    int loadedRoots = 0, loadedCount = 0;
    for (entt::entity e : loaded.Registry.view<HierarchyComponent>()) {
        ++loadedCount;
        // Bounded walk that BREAKS, not just CHECKs: a surviving cycle would otherwise spin
        // here forever and hang the whole suite instead of failing it.
        int hops = 0;
        bool terminated = true;
        for (entt::entity w = e; w != entt::null; ) {
            if (++hops > 64) { terminated = false; break; }
            const auto* h = loaded.Registry.try_get<HierarchyComponent>(w);
            w = h ? h->Parent : entt::null;
        }
        CHECK(terminated);
        const auto* h = loaded.Registry.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null) ++loadedRoots;
    }
    CHECK(loadedCount == 2);
    CHECK(loadedRoots == 1); // one link cut, the other kept
}

// --- #202: a degenerate camera frustum must not produce a NaN projection -------------------
void TestCameraFrustumValidation() {
    auto finite = [](const glm::mat4& m) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r])) return false;
        return true;
    };

    // The guard is at the point of use, so it holds whatever the values came from.
    Camera cam;
    cam.NearPlane = 10.0f; cam.FarPlane = 10.0f;      // far == near: divides by zero
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = 0.0f;  cam.FarPlane = 100.0f;     // non-positive near
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = -5.0f; cam.FarPlane = -10.0f;     // both wrong, and inverted
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.NearPlane = 0.1f;  cam.FarPlane = 1000.0f;
    CHECK(finite(cam.ProjectionMatrix(0.0f)));        // zero aspect
    CHECK(finite(cam.ProjectionMatrix(std::numeric_limits<float>::quiet_NaN())));
    cam.Orthographic = true;
    cam.NearPlane = 5.0f; cam.FarPlane = 5.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    // The ortho path divides by the half-height too: glm::ortho computes 2/(top-bottom).
    cam.NearPlane = 0.1f; cam.FarPlane = 100.0f;
    cam.OrthoHalfHeight = 0.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.OrthoHalfHeight = -3.0f;
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    cam.OrthoHalfHeight = std::numeric_limits<float>::quiet_NaN();
    CHECK(finite(cam.ProjectionMatrix(16.0f / 9.0f)));
    // A usable ortho camera is still passed through untouched.
    cam.OrthoHalfHeight = 8.0f;
    {
        const glm::mat4 want = glm::ortho(-8.0f * 1.5f, 8.0f * 1.5f, -8.0f, 8.0f, 0.1f, 100.0f);
        const glm::mat4 have = cam.ProjectionMatrix(1.5f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(have[c][r] - want[c][r]) < 1e-6f);
    }
    cam.Orthographic = false;
    cam.NearPlane = 0.1f; cam.FarPlane = 1000.0f;

    // MakePerspective is the shared chokepoint every direct call site now goes through, so the
    // same guarantees have to hold when it is called on its own.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(finite(MakePerspective(60.0f, inf, 0.1f, 100.0f)));   // zero-height viewport: w/0 == inf
    CHECK(finite(MakePerspective(60.0f, nan, 0.1f, 100.0f)));   // 0x0 viewport: 0/0 == NaN
    CHECK(finite(MakePerspective(60.0f, 0.0f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, -2.0f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(0.0f, 1.5f, 0.1f, 100.0f)));   // degenerate fov
    CHECK(finite(MakePerspective(180.0f, 1.5f, 0.1f, 100.0f))); // tan(90 deg) is infinite
    CHECK(finite(MakePerspective(nan, 1.5f, 0.1f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, 1.5f, 0.0f, 100.0f)));
    CHECK(finite(MakePerspective(60.0f, 1.5f, 50.0f, 50.0f)));  // far == near
    CHECK(finite(MakePerspective(60.0f, 1.5f, 100.0f, 1.0f)));  // inverted
    CHECK(finite(MakePerspective(60.0f, 1.5f, nan, inf)));
    // Valid input is reproduced exactly - the chokepoint must not reshape good cameras.
    {
        const glm::mat4 want = glm::perspective(glm::radians(55.0f), 1.5f, 0.2f, 300.0f);
        const glm::mat4 have = MakePerspective(55.0f, 1.5f, 0.2f, 300.0f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(have[c][r] - want[c][r]) < 1e-6f);
    }

    // A sane frustum is left exactly alone - the guard must not quietly reshape good cameras.
    cam.NearPlane = 0.3f; cam.FarPlane = 250.0f;
    const glm::mat4 expected = glm::perspective(glm::radians(cam.Fov), 1.5f, 0.3f, 250.0f);
    const glm::mat4 got = cam.ProjectionMatrix(1.5f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(std::fabs(got[c][r] - expected[c][r]) < 1e-6f);

    // And a scene carrying an unusable CameraComponent is corrected on load, not just tolerated.
    World world;
    AssetLibrary assets;
    const std::string sceneJson = R"({"formatVersion":3,"empties":[{"name":"Cam","id":1,"parentId":-1,)"
        R"("position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],)"
        R"("Camera":{"Field of View":0.0,"Near":0.0,"Far":-5.0}}]})";
    CHECK(SceneSerializer::LoadFromString(world, assets, sceneJson));
    int cams = 0;
    for (entt::entity e : world.Registry.view<CameraComponent>()) {
        ++cams;
        const auto& cc = world.Registry.get<CameraComponent>(e);
        CHECK(cc.NearPlane > 0.0f);
        CHECK(cc.FarPlane > cc.NearPlane);
        CHECK(cc.FovDegrees > 0.0f && cc.FovDegrees < 180.0f);
    }
    CHECK(cams == 1);
}

// --- #174: player.json keeps every field it was given -------------------------------------
// player.json is the ONLY thing a built game is configured by, and nothing in the editor reads
// it back, so a field dropped in Save() or missed in Load() is invisible until someone ships a
// build and finds their icon or splash gone. Values are spot-checked individually rather than
// by comparing the two structs wholesale: a field dropped symmetrically (never written, never
// read) would compare equal and pass, which is exactly how the scene round-trip test could have
// been fooled (#356).
void TestPlayerConfigRoundTrip() {
    const std::filesystem::path path = TempDir() / "player_roundtrip.json";

    PlayerConfig out;
    out.ProductName      = "Round Trip";
    out.CompanyName      = "Some Studio";
    out.Version          = "2.3.4";
    out.Scenes           = {"scenes/First.json", "scenes/Second.json"};
    out.Width            = 1600;
    out.Height           = 900;
    out.Fullscreen       = false;
    out.VSync            = false;
    out.DevelopmentBuild = true;
    out.IconPath         = "assets/branding/player_icon.png";
    out.SplashPath       = "assets/branding/player_splash.png";
    CHECK(out.Save(path.string()));

    PlayerConfig in;
    CHECK(PlayerConfig::Load(path.string(), in));
    CHECK(in.ProductName == "Round Trip");
    CHECK(in.CompanyName == "Some Studio");
    CHECK(in.Version == "2.3.4");
    CHECK(in.Scenes.size() == 2);
    CHECK(in.Scenes.size() == 2 && in.Scenes[0] == "scenes/First.json");
    CHECK(in.Scenes.size() == 2 && in.Scenes[1] == "scenes/Second.json");
    CHECK(in.Width == 1600 && in.Height == 900);
    CHECK(!in.Fullscreen && !in.VSync);
    CHECK(in.DevelopmentBuild);
    CHECK(in.IconPath == "assets/branding/player_icon.png");
    CHECK(in.SplashPath == "assets/branding/player_splash.png");

    // A player.json written before branding existed must still load, leaving both empty so the
    // game keeps the engine icon and shows no splash rather than chasing a missing file.
    const std::filesystem::path old = TempDir() / "player_old.json";
    {
        std::ofstream f(old);
        f << R"({"productName":"Old Build","scenes":["scenes/Only.json"],"width":800,"height":600})";
    }
    PlayerConfig legacy;
    CHECK(PlayerConfig::Load(old.string(), legacy));
    CHECK(legacy.ProductName == "Old Build");
    CHECK(legacy.IconPath.empty() && legacy.SplashPath.empty());
    CHECK(legacy.Fullscreen); // untouched keys keep their defaults

    // A missing file is a "run as the editor" signal, not an error.
    CHECK(!PlayerConfig::Load((TempDir() / "definitely_absent.json").string(), legacy));

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(old, ec);
}

// --- #201: the PhysX scene tracks the World while playing ----------------------------------
// This lived only in --smoke-test, which CI runs with continue-on-error because the runners have
// no GPU - so the check never actually gated anything. PhysX needs no GL, so it belongs here
// where the exit code is enforced. It is the one test that stands a real PhysX world up.
void TestPhysicsWorldSync() {
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    // A static floor slab under the origin, and a ray straight down onto it.
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -1.0f, 0.0f), zero, one, "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(10.0f, 0.5f, 10.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();

    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    if (!PhysicsWorld::IsActive()) return;

    const float origin[3] = {0.0f, 20.0f, 0.0f}, down[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter all;
    RaycastHit hit;
    auto hitsFloor = [&] {
        return PhysicsWorld::RaycastFiltered(origin, down, 100.0f, all, hit) &&
               hit.Entity == entt::to_integral(floorE);
    };
    auto resync = [&] {
        world.SyncActiveInHierarchy();
        world.RebuildWorldTransformCache();
        PhysicsWorld::Step(0.0f, world, {}); // frozen step: sync only, no simulation
    };

    CHECK(hitsFloor()); // built at Play-enter

    // Deactivated mid-play: the actor must leave the scene, not linger as an invisible blocker.
    world.Registry.emplace_or_replace<DeactivatedTag>(floorE);
    resync();
    CHECK(!hitsFloor());

    // Re-activated: built again.
    world.Registry.remove<DeactivatedTag>(floorE);
    resync();
    CHECK(hitsFloor());

    // An entity that gains a Collider while playing gets an actor: a second slab above the
    // first, which the ray must now stop on instead.
    const entt::entity lidE = world.CreateEmptyEntity(glm::vec3(0.0f, 5.0f, 0.0f), zero, one, "Lid");
    world.Registry.emplace<ColliderComponent>(lidE, col);
    resync();
    CHECK(PhysicsWorld::RaycastFiltered(origin, down, 100.0f, all, hit));
    CHECK(hit.Entity == entt::to_integral(lidE));

    // Destroyed mid-play: its actor goes with it, and the ray falls through to the floor again.
    world.DestroyEntityAndChildren(lidE);
    resync();
    CHECK(hitsFloor());

    // Losing the Collider component alone is enough to drop the actor.
    world.Registry.remove<ColliderComponent>(floorE);
    resync();
    CHECK(!PhysicsWorld::RaycastFiltered(origin, down, 100.0f, all, hit));

    PhysicsWorld::Destroy();
    PhysicsWorld::Shutdown(); // release the session-lifetime core too (#167)
}

// --- #202 / #122: a scene survives save -> load -> save unchanged ---------------------------
// The serializer is the one place where a silent mistake costs authored work: a field dropped on
// load, an unstable entity order, a component written but not read. None of that shows up as a
// crash - the scene just quietly comes back different. Round-tripping and comparing the two
// documents catches the whole class at once, which is what #202 asks for under "scene
// round-trips" and what #121/#342 changed enough of to be worth pinning down.
void TestSceneRoundTrip() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();

    World a;
    AssetLibrary assetsA;
    const glm::vec3 zero(0.0f), one(1.0f);

    // A deliberately awkward scene: hierarchy, a nameless entity, non-default transforms, and
    // components spanning every reflected field type (bool, int, float, vec3, colour, string,
    // enum) plus the tag-style components that are written as flags.
    const entt::entity parent = a.CreateEmptyEntity(glm::vec3(1.5f, -2.25f, 3.0f),
                                                    glm::vec3(0.0f, 45.0f, 0.0f), one, "Parent");
    const entt::entity child  = a.CreateEmptyEntity(glm::vec3(0.5f, 0.0f, 0.0f), zero,
                                                    glm::vec3(2.0f, 2.0f, 2.0f), "Child");
    CHECK(a.SetParent(child, parent));
    const entt::entity nameless = a.CreateEmptyEntity(zero, zero, one, ""); // #122 - must survive
    // Entities only, no CreateBox: a box carries a primitive Renderable, and building one calls
    // into GL, which does not exist here. That is why this suite stays on empties.
    const entt::entity box = a.CreateEmptyEntity(glm::vec3(0.0f, 1.0f, 0.0f),
                                                 glm::vec3(0.0f, 30.0f, 0.0f),
                                                 glm::vec3(2.0f, 0.5f, 2.0f), "Box");

    SpinComponent spin; spin.Axis = glm::vec3(0.0f, 0.0f, 1.0f); spin.Speed = 42.5f;
    a.Registry.emplace<SpinComponent>(parent, spin);

    CameraComponent cam; cam.FovDegrees = 72.5f; cam.NearPlane = 0.25f; cam.FarPlane = 750.0f;
    a.Registry.emplace<CameraComponent>(child, cam);

    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Sphere;   // an enum, round-tripped by label
    col.HalfExtents = glm::vec3(1.25f, 0.0f, 0.0f);
    col.Center = glm::vec3(0.0f, 0.5f, 0.0f);
    a.Registry.emplace<ColliderComponent>(box, col);

    a.Registry.emplace<TagComponent>(nameless, TagComponent{"Ball"});
    a.Registry.emplace<LayerComponent>(box, LayerComponent{3});
    a.Registry.emplace<StaticTag>(box);
    a.Registry.emplace<DeactivatedTag>(nameless);
    a.SyncActiveInHierarchy();

    const std::string first = SceneSerializer::SaveToString(a, assetsA);
    CHECK(!first.empty());

    World b;
    AssetLibrary assetsB;
    CHECK(SceneSerializer::LoadFromString(b, assetsB, first));
    const std::string second = SceneSerializer::SaveToString(b, assetsB);

    // Compared as parsed JSON, not as text: key order and whitespace are not the contract, the
    // data is. A mismatch here means the scene came back different from the one that was saved.
    const json j1 = json::parse(first, nullptr, false);
    const json j2 = json::parse(second, nullptr, false);
    CHECK(!j1.is_discarded() && !j2.is_discarded());
    CHECK(j1 == j2);

    // Spot-check the values themselves, so a round-trip that is merely self-consistent (both
    // sides dropping the same field) still fails.
    int found = 0;
    for (entt::entity e : b.Registry.view<NameComponent>()) {
        const std::string& n = b.Registry.get<NameComponent>(e).Name;
        if (n == "Parent") {
            ++found;
            CHECK(b.Registry.all_of<SpinComponent>(e));
            const auto& sp = b.Registry.get<SpinComponent>(e);
            CHECK(sp.Speed == 42.5f && sp.Axis.z == 1.0f);
            CHECK(b.Registry.all_of<HierarchyComponent>(e));
            CHECK(b.Registry.get<HierarchyComponent>(e).Children.size() == 1);
        } else if (n == "Child") {
            ++found;
            CHECK(b.Registry.all_of<CameraComponent>(e));
            const auto& cc = b.Registry.get<CameraComponent>(e);
            CHECK(cc.FovDegrees == 72.5f && cc.NearPlane == 0.25f && cc.FarPlane == 750.0f);
        } else if (n == "Box") {
            ++found;
            CHECK(b.Registry.all_of<ColliderComponent>(e));
            const auto& cl = b.Registry.get<ColliderComponent>(e);
            CHECK(cl.Kind == ColliderComponent::Shape::Sphere);
            CHECK(cl.HalfExtents.x == 1.25f && cl.Center.y == 0.5f);
            CHECK(b.Registry.all_of<StaticTag>(e));
            CHECK(b.Registry.all_of<LayerComponent>(e) && b.Registry.get<LayerComponent>(e).Layer == 3);
        }
    }
    CHECK(found == 3);

    // The nameless entity is still there, still deactivated, still tagged (#122).
    int tagged = 0;
    for (entt::entity e : b.Registry.view<TagComponent>())
        if (b.Registry.get<TagComponent>(e).Tag == "Ball") {
            ++tagged;
            CHECK(b.Registry.all_of<DeactivatedTag>(e));
        }
    CHECK(tagged == 1);
}

// --- AssetGuid ------------------------------------------------------------------------------
void TestAssetGuid() {
    const AssetGuid g = AssetGuid::Generate();
    CHECK(g.IsValid());
    CHECK(AssetGuid::FromString(g.ToString()) == g);
    CHECK(AssetGuid::Generate() != g);
    CHECK(!AssetGuid::FromString("").IsValid());
    CHECK(!AssetGuid::FromString("not-a-guid").IsValid());
}

// --- UndoDelta chain: push three states, pop them back in reverse ---------------------------
struct TestEntry { std::string Delta; int Tag = 0; };

void TestUndoDeltaChain() {
    const std::string s1 = R"({"entities":[{"name":"A","x":1}],"sky":1})";
    const std::string s2 = R"({"entities":[{"name":"A","x":2}],"sky":1})";
    const std::string s3 = R"({"entities":[{"name":"A","x":2},{"name":"B","x":5}],"sky":3})";

    std::vector<TestEntry> stack;
    std::string base;
    UndoDelta::Push(stack, base, TestEntry{{}, 1}, s1);
    UndoDelta::Push(stack, base, TestEntry{{}, 2}, s2);
    UndoDelta::Push(stack, base, TestEntry{{}, 3}, s3);
    CHECK(stack.size() == 3);
    CHECK(stack.back().Delta.empty()); // the top is held in full

    auto same = [](const std::string& a, const std::string& b) {
        return nlohmann::json::parse(a) == nlohmann::json::parse(b);
    };
    TestEntry e;
    std::string full;
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 3 && same(full, s3));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 2 && same(full, s2));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Ok);
    CHECK(e.Tag == 1 && same(full, s1));
    CHECK(UndoDelta::Pop(stack, base, e, full) == UndoDelta::PopResult::Empty);

    // Malformed input never throws out of the helpers.
    std::string out;
    CHECK(!UndoDelta::ApplyPatch(s1, "{not json", out));
    CHECK(!UndoDelta::ApplyPatch(s1, "", out));
    CHECK(UndoDelta::MakePatch("{broken", s1).empty());
}

// --- AtomicFile ----------------------------------------------------------------------------
void TestAtomicFile() {
    const auto path = TempDir() / "atomic.txt";
    CHECK(AtomicFile::WriteBytes(path, "first", true));
    CHECK(ReadAll(path) == "first");
    CHECK(AtomicFile::WriteBytes(path, "second, longer", true)); // replace in place
    CHECK(ReadAll(path) == "second, longer");
    // A directory that can't exist: must fail cleanly and leave nothing behind.
    const auto bad = TempDir() / "atomic.txt" / "child" / "x.txt"; // parent is a FILE
    CHECK(!AtomicFile::WriteBytes(bad, "nope", true));
    CHECK(ReadAll(path) == "second, longer");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// --- TextureCache keys: pixel-affecting settings change the hash, sampler state doesn't ------
void TestTextureCacheHash() {
    TextureImportSettings a;
    const uint64_t h = TextureCache::HashSettings(a);

    TextureImportSettings b = a; b.AnisoLevel = 16;
    CHECK(TextureCache::HashSettings(b) == h);
    b = a; b.FilterMode = TextureImportSettings::Filter::Point;
    CHECK(TextureCache::HashSettings(b) == h);
    b = a; b.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
    CHECK(TextureCache::HashSettings(b) == h);

    b = a; b.MaxTextureSize = 512;
    CHECK(TextureCache::HashSettings(b) != h);
    b = a; b.IsSRGB = false;
    CHECK(TextureCache::HashSettings(b) != h);
    b = a; b.CompressionMode = TextureImportSettings::Compression::Normal;
    const uint64_t hc = TextureCache::HashSettings(b);
    CHECK(hc != h);
    b.GenerateMipmaps = false; // compressed entries bake their mips, so this matters now
    CHECK(TextureCache::HashSettings(b) != hc);
    b = a; b.GenerateMipmaps = false; // uncompressed: mips are generated on the GPU, not cached
    CHECK(TextureCache::HashSettings(b) == h);
}

// --- MaterialAsset: wrong-typed JSON must load with defaults, never throw ---------------------
void TestMaterialRobustness() {
    const auto path = TempDir() / "bad.mat";
    const char* cases[] = {
        R"({"baseColor":"red","metallic":"very","roughness":[1,2],"properties":5})",
        R"({"baseColor":[1,2],"emissiveColor":null,"renderQueue":"x","opacity":{}})",
        R"([1,2,3])",
        R"({"albedoMap":42,"normalMap":{"a":1},"shader":7})",
    };
    for (const char* json : cases) {
        CHECK(AtomicFile::WriteBytes(path, json, true));
        bool threw = false;
        std::shared_ptr<MaterialAsset> m;
        try { m = MaterialAsset::Load(path.string(), nullptr); } catch (...) { threw = true; }
        CHECK(!threw);
    }
    CHECK(AtomicFile::WriteBytes(path, "{ this is not json", true));
    bool threw = false;
    try { (void)MaterialAsset::Load(path.string(), nullptr); } catch (...) { threw = true; }
    CHECK(!threw);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // #208 - a material follows its shader by GUID when the .shader is renamed.
    const auto dir = TempDir() / "shaderguid";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto oldShader = dir / "Old.shader", newShader = dir / "New.shader", mat = dir / "m.mat";
    CHECK(AtomicFile::WriteBytes(oldShader, "Vertex { engine://ModelVertex.glsl }", true));
    // A temp folder is outside the project, where EnsureGuid won't write .meta files: provide one.
    const AssetGuid fresh = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(dir / "Old.shader.meta", json({{"guid", fresh.ToString()}, {"type", "shader"}}).dump(), true));
    const AssetGuid g = AssetDatabase::EnsureGuid(oldShader.string());
    CHECK(g == fresh);
    CHECK(g.IsValid());
    const json mj = {{"matVersion", 2}, {"shader", oldShader.string()}, {"shaderGuid", g.ToString()}};
    CHECK(AtomicFile::WriteBytes(mat, mj.dump(), true));
    std::filesystem::rename(oldShader, newShader, ec);
    AssetDatabase::NotifyMoved(oldShader.string(), newShader.string());
    auto moved = MaterialAsset::Load(mat.string(), nullptr);
    CHECK(moved && std::filesystem::path(moved->ShaderPath) == newShader);
    // No GUID (or an unknown one) keeps the stored path.
    CHECK(AtomicFile::WriteBytes(mat, json({{"matVersion", 2}, {"shader", "missing.shader"}}).dump(), true));
    auto kept = MaterialAsset::Load(mat.string(), nullptr);
    CHECK(kept && kept->ShaderPath == "missing.shader");
    std::filesystem::remove_all(dir, ec);
}

// --- Component registry: unique names / keys, enum labels well-formed --------------------------
void TestComponentRegistry() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    CHECK(!ComponentRegistry::All().empty());
    std::set<std::string> names;
    for (const RegisteredComponent& rc : ComponentRegistry::All()) {
        CHECK(rc.Meta.Name && *rc.Meta.Name);
        CHECK(names.insert(rc.Meta.Name).second); // no duplicate display names (they're JSON keys)
        CHECK(rc.Has && rc.Get && rc.Add && rc.Remove);
        std::set<std::string> fields;
        for (const ReflectField& f : rc.Meta.Fields) {
            CHECK(fields.insert(f.Name).second);
            if (f.Type == ReflectFieldType::Enum && !f.EditorHidden) {
                CHECK(f.EnumLabels != nullptr && f.EnumCount > 0);
                for (int i = 0; f.EnumLabels && i < f.EnumCount; ++i) CHECK(*ReflectEnumLabel(f, i) != '\0');
                CHECK(*ReflectEnumLabel(f, f.EnumCount) == '\0'); // out of range -> ""
            }
        }
    }
}

// --- Animator Controller: transitions, exit time, Any state, triggers, JSON round trip (#175) --
void TestAnimatorController() {
    using AC = AnimatorController;
    AC c;
    c.Parameters = {{"Speed", AC::ParamType::Float, 0.0f}, {"Jump", AC::ParamType::Trigger, 0.0f},
                    {"Grounded", AC::ParamType::Bool, 1.0f}};
    c.States = {{"Idle", "idle", 1.0f, true}, {"Walk", "walk", 1.0f, true},
                {"Run", "run", 1.2f, true}, {"Jump", "jump", 1.0f, false}};
    c.DefaultState = "Idle";
    AC::Transition idleWalk{"Idle", "Walk", {{"Speed", AC::Op::Greater, 0.1f}}, false, 0.9f, 0.2f};
    AC::Transition walkIdle{"Walk", "Idle", {{"Speed", AC::Op::Less, 0.1f}}, false, 0.9f, 0.2f};
    AC::Transition walkRun{"Walk", "Run", {{"Speed", AC::Op::Greater, 3.0f}}, false, 0.9f, 0.2f};
    AC::Transition anyJump{"Any", "Jump", {{"Jump", AC::Op::If, 0.0f}}, false, 0.9f, 0.1f};
    AC::Transition jumpIdle{"Jump", "Idle", {}, true, 0.95f, 0.2f};
    AC::Transition broken{"Idle", "Run", {}, false, 0.9f, 0.2f}; // no condition, no exit time: never
    c.Transitions = {idleWalk, walkIdle, walkRun, anyJump, jumpIdle, broken};

    CHECK(c.DefaultStateIndex() == 0);
    CHECK(c.FindState("Run") == 2);
    CHECK(c.FindState("Nope") == -1);

    std::vector<AnimatorParam> p = {{"Speed", 0, 0.0f}, {"Jump", 3, 0.0f}, {"Grounded", 2, 1.0f}};
    CHECK(c.PickTransition(0, 0.0f, p) == -1);            // idle, not moving
    p[0].Value = 1.0f;
    CHECK(c.PickTransition(0, 0.0f, p) == 0);             // Idle -> Walk
    CHECK(c.PickTransition(1, 0.0f, p) == -1);            // walking, not fast enough to run
    p[0].Value = 4.0f;
    CHECK(c.PickTransition(1, 0.0f, p) == 2);             // Walk -> Run
    p[1].Value = 1.0f;                                   // SetTrigger("Jump")
    CHECK(c.PickTransition(2, 0.3f, p) == 3);             // Any -> Jump beats nothing else
    CHECK(p[1].Value == 0.0f);                           // ... and consumed the trigger
    CHECK(c.PickTransition(3, 0.5f, p) == -1);            // Jump waits for its exit time
    CHECK(c.PickTransition(3, 0.96f, p) == 4);            // Jump -> Idle at 95%
    p[1].Value = 1.0f;
    CHECK(c.PickTransition(3, 0.2f, p) == -1);            // Any never re-enters the state it targets
    CHECK(p[1].Value == 1.0f);                           // an unused trigger stays set
    std::vector<AnimatorParam> none;
    CHECK(c.PickTransition(0, 0.0f, none) == -1);         // missing parameters never match
    CHECK(c.PickTransition(-1, 0.0f, p) == -1);
    CHECK(c.PickTransition(99, 0.0f, p) == -1);

    // JSON round trip keeps everything.
    AC back;
    CHECK(AC::FromJsonString(c.ToJsonString(), back));
    CHECK(back.Parameters.size() == 3 && back.Parameters[1].Type == AC::ParamType::Trigger);
    CHECK(back.States.size() == 4 && back.States[2].Speed == 1.2f && !back.States[3].Loop);
    CHECK(back.Transitions.size() == 6 && back.Transitions[3].From == "Any");
    CHECK(back.Transitions[4].HasExitTime && back.Transitions[4].ExitTime == 0.95f);
    CHECK(back.Transitions[1].Conditions.size() == 1 && back.Transitions[1].Conditions[0].Mode == AC::Op::Less);
    CHECK(back.DefaultState == "Idle");

    // Robustness: garbage is rejected, unknown enum names fall back, bad entries are skipped.
    AC junk;
    CHECK(!AC::FromJsonString("{not json", junk));
    CHECK(!AC::FromJsonString("[1,2]", junk));
    CHECK(AC::FromJsonString(R"({"parameters":[{"name":"x","type":"banana"},{"type":"int"}],
        "states":[{"name":"A","speed":"fast"},5],"transitions":[{"from":"A","to":"A",
        "conditions":[{"param":"x","mode":"sideways"}]}]})", junk));
    CHECK(junk.Parameters.size() == 1 && junk.Parameters[0].Type == AC::ParamType::Float);
    CHECK(junk.States.size() == 1 && junk.States[0].Speed == 1.0f);
    CHECK(junk.Transitions.size() == 1 && junk.Transitions[0].Conditions[0].Mode == AC::Op::Greater);

    // The component's setters create parameters on first use and keep their type.
    AnimatorControllerComponent comp;
    comp.SetFloat("Speed", 2.5f);
    comp.SetTrigger("Jump");
    comp.SetBool("Grounded", true);
    CHECK(comp.Params.size() == 3);
    CHECK(comp.GetFloat("Speed") == 2.5f && comp.GetFloat("Jump") == 1.0f && comp.GetFloat("Grounded") == 1.0f);
    comp.ResetTrigger("Jump");
    CHECK(comp.GetFloat("Jump") == 0.0f && comp.GetFloat("Missing") == 0.0f);
}

// --- #132: asset identity - path keys, asset types, GUID-following references ------------------
void TestAssetIdentity() {
    namespace fs = std::filesystem;
    // One file, three spellings, one key.
    const fs::path dir = TempDir() / "identity";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Sub", ec);
    const std::string a = (dir / "Sub" / "Tex.png").string();
    CHECK(AssetDatabase::PathKey(a) == AssetDatabase::PathKey((dir / "sub" / "tex.png").generic_string()));
    CHECK(AssetDatabase::PathKey(a) == AssetDatabase::PathKey((dir / "Sub" / "." / "Tex.png").string()));
    CHECK(AssetDatabase::PathKey(a) != AssetDatabase::PathKey((dir / "Sub" / "Tex2.png").string()));

    // Asset types by extension; .json only under project/scenes, never the editor's safety copies.
    CHECK(AssetDatabase::AssetType("x/a.PNG") == "texture");
    CHECK(AssetDatabase::AssetType("x/a.controller") == "animatorcontroller");
    CHECK(AssetDatabase::AssetType("x/a.tescript") == "script");
    CHECK(AssetDatabase::AssetType("x/a.shader") == "shader");
    CHECK(AssetDatabase::AssetType("x/a.txt").empty());
    CHECK(AssetDatabase::AssetType("x/settings.json").empty());
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("scenes/Level.json")) == "scene");
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("scenes/Level.recovery.json")).empty());
    CHECK(AssetDatabase::AssetType(ProjectPaths::Resolve("settings.json")).empty());

    // A reference ("file#clip") follows its file to a new name by GUID; the suffix survives.
    const std::string model = (dir / "Hero.fbx").string(), moved = (dir / "Sub" / "HeroRig.fbx").string();
    CHECK(AtomicFile::WriteBytes(model, "not really an fbx", true));
    const AssetGuid g = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(model + ".meta", json({{"guid", g.ToString()}, {"type", "model"}}).dump(), true));
    const std::string ref = model + "#Run";
    const std::string refGuid = AssetDatabase::RefGuid(ref);
    CHECK(refGuid == g.ToString());
    CHECK(AssetDatabase::FollowRef(ref, refGuid) == ref);      // still there: unchanged
    fs::rename(model, moved, ec);
    AssetDatabase::NotifyMoved(model, moved);                    // moves the .meta too
    CHECK(fs::exists(moved + ".meta", ec) && !fs::exists(model + ".meta", ec));
    const std::string followed = AssetDatabase::FollowRef(ref, refGuid);
    CHECK(AssetDatabase::PathKey(followed.substr(0, followed.find('#'))) == AssetDatabase::PathKey(moved));
    CHECK(followed.size() > 4 && followed.compare(followed.size() - 4, 4, "#Run") == 0);
    CHECK(AssetDatabase::FollowRef("Run", "") == "Run");        // an own clip name: not a file
    CHECK(AssetDatabase::FollowRef(ref, "0000000000000001") == ref); // unknown GUID: unchanged
    CHECK(AssetDatabase::RefGuid("primitive://cube#3").empty());

    // A material follows a renamed texture by the GUID it saved.
    const std::string tex = (dir / "Albedo.png").string(), tex2 = (dir / "Sub" / "Albedo2.png").string();
    CHECK(AtomicFile::WriteBytes(tex, "png", true));
    const AssetGuid tg = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(tex + ".meta", json({{"guid", tg.ToString()}, {"type", "texture"}}).dump(), true));
    CHECK(AssetDatabase::EnsureGuid(tex) == tg);
    MaterialAsset ma;
    ma.Path = (dir / "m.mat").string();
    ma.AlbedoMapPath = tex;
    CHECK(ma.Save());
    CHECK(ReadAll(ma.Path).find(tg.ToString()) != std::string::npos); // "textureGuids" written
    fs::rename(tex, tex2, ec);
    AssetDatabase::NotifyMoved(tex, tex2);
    auto back = MaterialAsset::Load(ma.Path, nullptr);
    CHECK(back && AssetDatabase::PathKey(back->AlbedoMapPath) == AssetDatabase::PathKey(tex2));
    fs::remove_all(dir, ec);
}

// --- #132: the project watcher reports adds, renames, moves, edits and deletes ----------------
void TestProjectWatcher() {
    namespace fs = std::filesystem;
    using K = ProjectWatcher::Change::Kind;
    const fs::path dir = TempDir() / "watch";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Library", ec);
    fs::create_directories(dir / "sub", ec);
    CHECK(ProjectWatcher::Start(dir.string()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto settle = [&] {
        std::vector<ProjectWatcher::Change> all;
        bool overflow = false;
        for (int i = 0; i < 80; ++i) { // up to 4 s (slow CI runners)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto got = ProjectWatcher::Drain(150, overflow);
            all.insert(all.end(), got.begin(), got.end());
            if (!all.empty() && got.empty()) break;
        }
        return all;
    };
    auto has = [](const std::vector<ProjectWatcher::Change>& v, K kind, const fs::path& p, const fs::path& old = {}) {
        for (const auto& c : v)
            if (c.Type == kind && AssetDatabase::PathKey(c.Path) == AssetDatabase::PathKey(p.string()) &&
                (old.empty() || AssetDatabase::PathKey(c.OldPath) == AssetDatabase::PathKey(old.string())))
                return true;
        return false;
    };

    const fs::path a = dir / "a.png";
    CHECK(AtomicFile::WriteBytes(a, "1", true));
    std::ofstream(dir / "Library" / "cache.bin") << "x"; // ignored folder
    auto c1 = settle();
    CHECK(has(c1, K::Added, a) || has(c1, K::Modified, a));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find("Library") != std::string::npos; }));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find(".tmp-") != std::string::npos; }));

    const fs::path b = dir / "b.png";
    fs::rename(a, b, ec);
    auto c2 = settle();
    CHECK(has(c2, K::Renamed, b, a));

    const fs::path moved = dir / "sub" / "b.png";
    fs::rename(b, moved, ec);                       // across folders: Removed + Added, paired
    auto c3 = settle();
    CHECK(has(c3, K::Renamed, moved, b));

    std::ofstream(moved, std::ios::app) << "more";
    auto c4 = settle();
    CHECK(has(c4, K::Modified, moved));

    const fs::path t = dir / "temp.png";              // created and deleted before it settles
    std::ofstream(t) << "x";
    fs::remove(t, ec);
    fs::remove(moved, ec);
    auto c5 = settle();
    CHECK(has(c5, K::Removed, moved));
    CHECK(!has(c5, K::Added, t) && !has(c5, K::Removed, t));

    ProjectWatcher::Stop();
    CHECK(!ProjectWatcher::IsRunning());
    fs::remove_all(dir, ec);
}

} // namespace

int RunUnitTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"AssetGuid", TestAssetGuid},
        {"UndoDeltaChain", TestUndoDeltaChain},
        {"AtomicFile", TestAtomicFile},
        {"TextureCacheHash", TestTextureCacheHash},
        {"MaterialRobustness", TestMaterialRobustness},
        {"ComponentRegistry", TestComponentRegistry},
        {"AnimatorController", TestAnimatorController},
        {"AssetIdentity", TestAssetIdentity},
        {"ProjectWatcher", TestProjectWatcher},
        {"LodGroup", TestLodGroup},
        {"NearestEuler", TestNearestEuler},
        {"PhysicMaterial", TestPhysicMaterial},
        {"PostProcessVolume", TestPostProcessVolume},
        {"DopplerVelocity", TestDopplerVelocity},
        {"InputMap", TestInputMap},
        {"ActiveInHierarchy", TestActiveInHierarchy},
        {"CameraRoll", TestCameraRoll},
        {"AssetMetaMigration", TestAssetMetaMigration},
        {"ComponentPreset", TestComponentPreset},
        {"LogStackTrace", TestLogStackTrace},
        {"HierarchyCycleRepair", TestHierarchyCycleRepair},
        {"CameraFrustumValidation", TestCameraFrustumValidation},
        {"SceneRoundTrip", TestSceneRoundTrip},
        {"PlayerConfigRoundTrip", TestPlayerConfigRoundTrip},
        {"PhysicsWorldSync", TestPhysicsWorldSync},
    };
    for (const auto& [name, fn] : tests) {
        g_CurrentTest = name;
        const int before = g_Failures;
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw " << e.what() << "\n";
        } catch (...) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw a non-std exception\n";
        }
        std::cout << "[UnitTest] " << (g_Failures == before ? "PASS " : "FAIL ") << name << "\n";
    }
    std::cout << "[UnitTest] " << g_Checks << " checks, " << g_Failures << " failure(s)\n";
    std::cout.flush();
    return g_Failures;
}
