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
#include "TimeService.h"
#include "UnitTestSupport.h"
#include "../Game/FirstPersonMovementInput.h"
#include "UnitTests.h"

#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "RootMotion.h"
#include "FirstPersonBody.h"
#include "WeaponJankMeter.h"
#include "FirstPersonBodyContract.h"
#include "Curve.h"
#include "IK.h"
#include "FirstPersonWeaponWizard.h"
#include "ClipAnalysis.h"
#include "FirstPersonAnimation.h"
#include "WeaponPrefab.h"
#include "FirstPersonAdsCarry.h"
#include "AudioEngine.h"
#include <set>
#include "Log.h" // #178 stack traces
#include "InputMap.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
#include "Utf8.h"
#include "Camera.h"
#include "PhysicsWorld.h"  // #201 - the physics sync regression test
#include "Combat/Damage.h" // hit regions, damage zones
#include "Npc/NpcRagdoll.h" // the soldier's part table
#include "GameModuleAPI.h"  // QueryFilter / RaycastHit
#include <cmath>   // #202 isfinite
#include <limits>
#include "ComponentReflection.h"
#include "ComponentRegistry.h"
#include "AssetLibrary.h" // #178 preset apply
#include "MaterialAsset.h"
#include "PhysicMaterialAsset.h"
#include "PlayerConfig.h" // #174 - player.json round-trip
#include "ProjectSettings.h"
#include "Player.h"
#include "PostProcessVolume.h"
#include "Tonemapper.h"
#include "Core/Paths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "Model.h"       // texture-path resolver
#include "AssetImport.h" // folder import
#include "ImportQueueManager.h"
#include "SceneSerializer.h" // #121
#include "AnimationSystem.h"           // #123 - the spin systems, driven frame by frame below
#include "GameRules.h"
#include "HotReloadGameModule.h"      // Spin / Transform Controller run inside TartarusGame.dll, not the exe
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetModuleFileNameW: --unit-tests runs before EnginePaths::Init
#undef near            // windows.h #defines these two, and the tests use them as variable names
#undef far
#include "UndoDeltaChain.h"
#include "World.h"
#include "ImpostorLibrary.h"
#include "BulletHoles.h"
#include "ShellCasings.h"
#include "RotationMath.h"
#include "SkyAtmosphere.h"
#include "SkySettings.h"
#include "TimeOfDay.h"

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
#include "Wardrobe.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "Texture.h"
#include "PngWrite.h" // async decode test fixture
#include "EditorLayerInternal.h" // ClipFilterMatch
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using json = nlohmann::json;

namespace UnitTestSupport {

int g_Failures = 0;
int g_Checks = 0;
const char* g_CurrentTest = "";
std::string g_SkipReason;

} // namespace UnitTestSupport

using UnitTestSupport::g_Checks;
using UnitTestSupport::g_CurrentTest;
using UnitTestSupport::g_Failures;
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

namespace {



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
    const auto migrated = InputMap::FromJson(nlohmann::json::array({
        {{"name", "MuzzleAttachment"}, {"positive", 71}}, {{"name", "GripAttachment"}, {"positive", 72}}, {{"name", "OpticAttachment"}, {"positive", 73}}}));
    CHECK(migrated.size() == 3);
    CHECK(migrated[0].Name == "Weapon1" && migrated[0].Positive == 71);
    CHECK(migrated[1].Name == "Weapon2" && migrated[1].Positive == 72);
    CHECK(migrated[2].Name == "Weapon3" && migrated[2].Positive == 73);
    CHECK(InputMap::BindingName(InputMap::kMouseBase + 1) == "Mouse 1" && InputMap::BindingName(87) == "W");

    // A saved action list must not be able to LOSE a default. project/settings.json only holds
    // the actions that existed when it was written, and an unknown name merely warns once - so a
    // default added since then would read as "the key does nothing". That is how the EmptyReload
    // debug binding shipped dead: Defaults() had it, the saved list did not.
    {
        nlohmann::json savedJson = nlohmann::json::array();
        savedJson.push_back({{"name", "Reload"}, {"positive", 71}}); // a deliberate rebind (default is 82)
        auto saved = InputMap::FromJson(savedJson);
        InputMap::MergeDefaults(saved);

        auto count = [&saved](const char* n) {
            int c = 0;
            for (const InputMap::Action& s : saved)
                if (s.Name == n) ++c;
            return c;
        };
        CHECK(saved.size() == InputMap::Defaults().size()); // each default present exactly once
        CHECK(count("Reload") == 1);
        CHECK(saved[0].Positive == 71);      // the file's own binding survives the merge
        CHECK(count("Flashlight") == 1); // the default the file predates comes back
    }

    // Disabled (no game input): everything reads zero, including unknown names (no crash).
    InputMap::SetEnabled(false);
    CHECK(InputMap::GetAxis("Horizontal") == 0.0f && !InputMap::GetButton("Jump"));
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

// The clip pickers' search: every word, any case, anywhere in the path.
void TestClipFilterMatch() {
    using EditorInternal::ClipFilterMatch;
    const std::string path = "Animations/Mocap/AF_Walk/Walk_Fwd_NoRM.fbx";
    CHECK(ClipFilterMatch(nullptr, path) && ClipFilterMatch("", path) && ClipFilterMatch("   ", path));
    CHECK(ClipFilterMatch("walk", path) && ClipFilterMatch("WALK fwd", path) && ClipFilterMatch("  norm  af_", path));
    CHECK(!ClipFilterMatch("run", path) && !ClipFilterMatch("walk run", path) && !ClipFilterMatch("fwdx", path));
}

void TestFramePacing() {
    const double p = 1.0 / 60.0;
    // Jittered CPU-side frame times around one refresh come out as whole refreshes.
    double unpaced = 0.0, worst = 0.0, total = 0.0, measuredTotal = 0.0;
    const double jitter[] = {0.011, 0.022, 0.0167, 0.014, 0.0194, 0.0155, 0.0179, 0.0167};
    for (int i = 0; i < 800; ++i) {
        const double m = jitter[i % 8] + (i % 8 == 7 ? 8 * p - 0.1336 : 0.0); // the eight sum to eight refreshes
        const double d = Time::PaceDelta(m, p, unpaced);
        if (i > 20) worst = std::max(worst, std::abs(d - p));
        total += d;
        measuredTotal += m;
    }
    CHECK(worst < 0.0005);                               // within half a millisecond of a refresh
    CHECK(std::abs(total - measuredTotal) < p);          // the game clock keeps up with the real one
    // A dropped frame is two refreshes.
    unpaced = 0.0;
    CHECK(std::abs(Time::PaceDelta(2.0 * p + 0.003, p, unpaced) - 2.0 * p) < 0.0005);
    // A refresh rate that is off (59.94 Hz read as 60): no drift.
    unpaced = 0.0, total = 0.0;
    for (int i = 0; i < 6000; ++i) total += Time::PaceDelta(1.0 / 59.94, p, unpaced);
    CHECK(std::abs(total - 6000.0 / 59.94) < 2.0 * p);
    // A stall far off the refresh: handed out as measured, nothing left over.
    unpaced = 0.0;
    const double stall = Time::PaceDelta(0.0, p, unpaced) + Time::PaceDelta(0.1 + 0.4 * p, p, unpaced);
    CHECK(std::abs(stall + unpaced - (0.1 + 0.4 * p)) < 1e-9);
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

    // The player's user folder name, and Build Settings handing its fields to the player.
    PlayerConfig named;
    named.ProductName = "My/Game: 2";
    CHECK(named.UserFolderName() == "My_Game_ 2");
    named.ProductName.clear();
    CHECK(named.UserFolderName() == "TartarusGame");
    ProjectSettings::BuildSettings bs;
    bs.ProductName = "Handed Over";
    bs.Scenes = {"scenes/A.json"};
    bs.Width = 1024;
    bs.OutputDir = "D:/Out";
    PlayerConfig handed;
    static_cast<PlayerBuildFields&>(handed) = bs;
    CHECK(handed.ProductName == "Handed Over" && handed.Width == 1024);
    CHECK(handed.Scenes.size() == 1 && handed.Scenes[0] == "scenes/A.json");

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(old, ec);
}

// --- #201: the PhysX scene tracks the World while playing ----------------------------------
// This lived only in --smoke-test, which CI runs with continue-on-error because the runners have
// no GPU - so the check never actually gated anything. PhysX needs no GL, so it belongs here
// where the exit code is enforced. It is the one test that stands a real PhysX world up.
// Bones and ragdoll parts map to the regions of a hit, which map to the zones the damage multipliers are keyed on.
void TestNpcHitRegions() {
    // Every part of the soldier's table lands where its bone does: pelvis, spine_03, head, the arms, the legs.
    static const HitRegion kWant[NpcRagdoll::kParts] = {HitRegion::Torso, HitRegion::Torso, HitRegion::Head, HitRegion::Arm, HitRegion::Arm,
                                                        HitRegion::Arm, HitRegion::Arm, HitRegion::Leg, HitRegion::Leg, HitRegion::Leg, HitRegion::Leg};
    for (int i = 0; i < NpcRagdoll::kParts; ++i) CHECK(RegionFromPart(i) == kWant[i]);
    CHECK(RegionFromPart(-1) == HitRegion::Torso && RegionFromPart(99) == HitRegion::Torso);
    // Regions to zones: arms and legs are both "limb", with the weapon's limb multiplier.
    CHECK(ZoneOfRegion(HitRegion::Head) == HitZone::Head);
    CHECK(ZoneOfRegion(HitRegion::Torso) == HitZone::Torso);
    CHECK(ZoneOfRegion(HitRegion::Arm) == HitZone::Limb && ZoneOfRegion(HitRegion::Leg) == HitZone::Limb);
    FirstPersonWeaponGameplay w;
    w.Damage = 30.0f;
    w.HeadMultiplier = 2.0f;
    w.LimbMultiplier = 0.5f;
    w.FalloffStart = 1000.0f;
    w.FalloffEnd = 2000.0f;
    CHECK(std::fabs(DamageForHit(w, ZoneOfRegion(RegionFromPart(2)), 5.0f) - 60.0f) < 1e-3f);  // head
    CHECK(std::fabs(DamageForHit(w, ZoneOfRegion(RegionFromPart(1)), 5.0f) - 30.0f) < 1e-3f);  // chest
    CHECK(std::fabs(DamageForHit(w, ZoneOfRegion(RegionFromPart(4)), 5.0f) - 15.0f) < 1e-3f);  // forearm
    CHECK(std::fabs(DamageForHit(w, ZoneOfRegion(RegionFromPart(9)), 5.0f) - 15.0f) < 1e-3f);  // thigh
}

// A hitch past the fixed step's catch-up cap is dropped, not carried: the hitch frame runs the cap's steps and the
// next ordinary frame no extra one.
void TestPhysicsHitchCatchUp() {
    World world;
    PhysicsWorld::Create(world);
    const float fixed = PhysicsWorld::GetDebugStats().FixedStep;
    PhysicsWorld::Step(1.0f, world, {});
    CHECK(PhysicsWorld::GetDebugStats().Substeps == 4);
    PhysicsWorld::Step(fixed * 0.5f, world, {});
    CHECK(PhysicsWorld::GetDebugStats().Substeps == 0);
    PhysicsWorld::Step(fixed * 0.6f, world, {});
    CHECK(PhysicsWorld::GetDebugStats().Substeps == 1);
    PhysicsWorld::Step(fixed * 2.0f, world, {});
    CHECK(PhysicsWorld::GetDebugStats().Substeps == 2);
    PhysicsWorld::Destroy();
}

// The per-bone hitboxes: while active the unscoped queries (the player's shots) hit them and skip the capsule; a scoped one
// (an NPC's) is the other way round; a ray over body parts says which part; a ragdoll's parts answer the same ray.
void TestNpcBodyParts() {
    World world;
    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    if (!PhysicsWorld::IsActive()) return;
    constexpr unsigned kEntity = 4242u;
    const float foot[3] = {0.0f, 0.0f, 0.0f};
    const PhysicsWorld::CharacterId cct = PhysicsWorld::CreateNpcCharacter(kEntity, 0.3f, 0.6f, foot);
    CHECK(cct != PhysicsWorld::kNoCharacter);
    // Eleven short capsules stacked up the capsule's axis, part i at height 0.15 i + 0.1, lying along X.
    PhysicsWorld::HitCapsule caps[NpcRagdoll::kParts];
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        caps[i].Position[1] = 0.15f * (float)i + 0.1f;
        caps[i].HalfLength = 0.1f;
        caps[i].Radius = 0.05f;
    }
    const int boxes = PhysicsWorld::CreateNpcHitboxes(cct, caps, NpcRagdoll::kParts);
    CHECK(boxes >= 0);
    auto settle = [&] { PhysicsWorld::Step(1.0f / 60.0f, world, {}); };
    settle();
    QueryFilter all;
    RaycastHit hit;
    const float along[3] = {1.0f, 0.0f, 0.0f};
    // Down the middle of each part: the player's ray hits that part's hitbox.
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        const float o[3] = {-3.0f, caps[i].Position[1], 0.0f};
        CHECK(PhysicsWorld::RaycastFiltered(o, along, 20.0f, all, hit) && hit.Entity == kEntity);
        PhysicsWorld::BodyPartHit part;
        CHECK(PhysicsWorld::RaycastBodyParts(o, along, 20.0f, part));
        CHECK(part.Kind == 1 && part.Part == i && part.Entity == kEntity);
        CHECK(std::fabs(part.Distance - (3.0f - 0.1f - 0.05f)) < 0.02f); // the capsule's near cap
    }
    // Inside the movement capsule but outside every hitbox: the player's ray passes through; an NPC's (scoped) hits the capsule.
    const float graze[3] = {-3.0f, 0.9f, 0.22f};
    CHECK(!PhysicsWorld::RaycastFiltered(graze, along, 20.0f, all, hit));
    {
        PhysicsWorld::ScopedQueryPolicy npc(0xFFFFFFFEu, /*hitPlayer=*/true);
        CHECK(PhysicsWorld::RaycastFiltered(graze, along, 20.0f, all, hit) && hit.Entity == kEntity);
        // ... and it does not see the hitboxes: down the middle of part 4 it meets the capsule's surface instead (z = -0.3).
        const float mid[3] = {-3.0f, caps[4].Position[1], 0.0f};
        CHECK(PhysicsWorld::RaycastFiltered(mid, along, 20.0f, all, hit) && hit.Distance > 2.5f && hit.Distance < 2.8f);
    }
    // Inactive (a soldier far from the player): the capsule answers every query again.
    PhysicsWorld::SetNpcHitboxesActive(boxes, false);
    settle();
    CHECK(PhysicsWorld::RaycastFiltered(graze, along, 20.0f, all, hit) && hit.Entity == kEntity);
    PhysicsWorld::SetNpcHitboxesActive(boxes, true);
    settle();
    CHECK(!PhysicsWorld::RaycastFiltered(graze, along, 20.0f, all, hit));
    // Moved: the hitboxes go where they are put.
    for (auto& c : caps) c.Position[2] = 1.0f;
    PhysicsWorld::SetNpcHitboxPoses(boxes, caps, NpcRagdoll::kParts);
    settle();
    const float shifted[3] = {-3.0f, caps[2].Position[1], 1.0f};
    PhysicsWorld::BodyPartHit moved;
    CHECK(PhysicsWorld::RaycastBodyParts(shifted, along, 20.0f, moved) && moved.Part == 2);
    PhysicsWorld::DestroyNpcHitboxes(boxes);
    settle();
    PhysicsWorld::BodyPartHit none;
    CHECK(!PhysicsWorld::RaycastBodyParts(shifted, along, 20.0f, none));
    CHECK(PhysicsWorld::RaycastFiltered(graze, along, 20.0f, all, hit) && hit.Entity == kEntity);
    PhysicsWorld::DestroyNpcCharacter(cct);

    // A ragdoll's parts are tagged the same way; its slerp drives can be set and cleared; a shove wakes it.
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kParts];
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        parts[i].Parent = i == 0 ? -1 : i - 1;
        parts[i].Position[0] = 20.0f;
        parts[i].Position[1] = 10.0f + 0.3f * (float)i;
        parts[i].Anchor[0] = 20.0f;
        parts[i].Anchor[1] = parts[i].Position[1] - 0.15f;
        parts[i].HalfLength = 0.1f;
        parts[i].Radius = 0.05f;
    }
    const int ragdoll = PhysicsWorld::CreateRagdoll(kEntity, parts, NpcRagdoll::kParts);
    CHECK(ragdoll >= 0);
    PhysicsWorld::SetRagdollDrive(ragdoll, 700.0f, 60.0f);
    const float ident[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PhysicsWorld::SetRagdollDriveTarget(ragdoll, 3, ident);
    settle();
    const float o5[3] = {17.0f, parts[5].Position[1], 0.0f};
    PhysicsWorld::BodyPartHit rp;
    CHECK(PhysicsWorld::RaycastBodyParts(o5, along, 20.0f, rp) && rp.Kind == 2 && rp.Part == 5 && rp.Entity == kEntity);
    PhysicsWorld::SetRagdollDrive(ragdoll, 0.0f, 0.0f);
    const float j[3] = {0.0f, 0.0f, 5.0f}, at[3] = {20.0f, parts[5].Position[1], 0.0f};
    PhysicsWorld::RagdollImpulse(ragdoll, 5, j, at);
    CHECK(!PhysicsWorld::RagdollAsleep(ragdoll));
    for (int i = 0; i < 30; ++i) settle();
    float p[3], q[4];
    CHECK(PhysicsWorld::GetRagdollPart(ragdoll, 5, p, q) && std::isfinite(p[0]) && std::isfinite(p[1]) && p[1] < parts[5].Position[1]);
    PhysicsWorld::DestroyRagdoll(ragdoll);
    // Ids are generational: a freed slot's next owner gets a new id, and the stale one finds nothing (nor destroys the new one).
    const int ragdoll2 = PhysicsWorld::CreateRagdoll(kEntity, parts, NpcRagdoll::kParts);
    CHECK(ragdoll2 >= 0 && ragdoll2 != ragdoll);
    CHECK(!PhysicsWorld::GetRagdollPart(ragdoll, 5, p, q));
    PhysicsWorld::DestroyRagdoll(ragdoll);
    CHECK(PhysicsWorld::GetRagdollPart(ragdoll2, 5, p, q));
    PhysicsWorld::DestroyRagdoll(ragdoll2);
    // (cct and boxes were destroyed above.) Their slots' next owners get new ids; the stale ones touch nothing.
    const PhysicsWorld::CharacterId cct2 = PhysicsWorld::CreateNpcCharacter(kEntity, 0.3f, 0.6f, foot);
    CHECK(cct2 != PhysicsWorld::kNoCharacter && cct2 != cct);
    float f3[3];
    CHECK(!PhysicsWorld::GetNpcCapsule(cct, f3, nullptr, nullptr) && PhysicsWorld::GetNpcCapsule(cct2, f3, nullptr, nullptr));
    const int boxes2 = PhysicsWorld::CreateNpcHitboxes(cct2, caps, NpcRagdoll::kParts);
    CHECK(boxes2 >= 0 && boxes2 != boxes);
    PhysicsWorld::SetNpcHitboxesActive(boxes, false); // stale: boxes2 stays active
    PhysicsWorld::DestroyNpcHitboxes(boxes);
    settle();
    {
        const float o[3] = {-3.0f, caps[2].Position[1], caps[2].Position[2]}; // (moved to z = 1 above)
        PhysicsWorld::BodyPartHit part;
        CHECK(PhysicsWorld::RaycastBodyParts(o, along, 20.0f, part) && part.Kind == 1 && part.Part == 2);
    }
    PhysicsWorld::DestroyNpcHitboxes(boxes2);
    PhysicsWorld::DestroyNpcCharacter(cct2);
    PhysicsWorld::Destroy(); // (the core stays up for the next test)
}

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

// --- AssetGuid ------------------------------------------------------------------------------
void TestAssetGuid() {
    const AssetGuid g = AssetGuid::Generate();
    CHECK(g.IsValid());
    CHECK(AssetGuid::FromString(g.ToString()) == g);
    CHECK(AssetGuid::Generate() != g);
    CHECK(!AssetGuid::FromString("").IsValid());
    CHECK(!AssetGuid::FromString("not-a-guid").IsValid());
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

    // WriteJson with invalid UTF-8 in a string: used to throw out of dump(); now writes (U+FFFD) and returns true.
    const auto jsonPath = TempDir() / "atomic_invalid_utf8.json";
    nlohmann::json bad8;
    bad8["name"] = std::string("ab\xC3(cd"); // 0xC3 followed by '(' is not valid UTF-8
    CHECK(AtomicFile::WriteJson(jsonPath, bad8));
    CHECK(ReadAll(jsonPath).find("\"name\"") != std::string::npos);
    std::filesystem::remove(jsonPath, ec);
}

// --- Utf8TruncateBytes: a multi-byte character straddling the cut is dropped whole -----------
void TestUtf8Truncate() {
    std::string s = "abc\xC3\xA9"; // "abc" + e-acute (2 bytes); a 4-byte cut would split it
    Utf8TruncateBytes(s, 4);
    CHECK(s == "abc");
    s = "abc\xC3\xA9";
    Utf8TruncateBytes(s, 5); // fits exactly
    CHECK(s == "abc\xC3\xA9");
    s = "x\xE2\x82\xAC"; // 3-byte euro sign after 'x'
    Utf8TruncateBytes(s, 3);
    CHECK(s == "x");
    s = "short";
    Utf8TruncateBytes(s, 40);
    CHECK(s == "short");
}






// Async loading's CPU halves (AssetLibrary::RequestModelAsync's workers run these): no GL anywhere, so
// they work here, headless. A deferred import is complete except for its GPU objects, and a texture decode
// honours Max Size. The Vivian head/body also pin the female-import fix: upright, at head height.
void TestAsyncImportCpu() {
    auto deferred = [](const std::string& rel) {
        return Model::ImportDeferred(ProjectPaths::Resolve(rel), ModelImportSettings{});
    };
    // The model half needs the Quantum pack's meshes (project content from the shared Drive, absent
    // from a packaged engine and from a clone that hasn't fetched them).
    const bool quantum = std::filesystem::exists(
        ProjectPaths::Resolve("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head.fbx"));
    if (quantum) {
        const auto head = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head.fbx");
        CHECK(head && head->NeedsGpuUpload());
        CHECK(head && head->MeshCount() > 1);
        CHECK(head && head->BoundsMax().y > 1.6f && head->BoundsMin().y > 1.3f); // on the neck, not lying at the feet
        const auto body = deferred("assets/Characters/Quantum/Models/Female/Body/Parts/SKM_F_Vivian_Body.fbx");
        CHECK(body && body->BoundsMax().y > 1.4f && body->BoundsMax().z < 0.5f); // upright, not stretched along Z
        // Offsets carrying a group node's frame, not the mesh node's (the jackets' "Cloth" group): chest height.
        const auto jacket = deferred("assets/Characters/Quantum/Models/Clothing/Male/Outerwear/SKM_Leather_Jacket.fbx");
        CHECK(jacket && jacket->BoundsMax().y > 1.3f && jacket->BoundsMax().z < 0.6f);
        const auto oldHead = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head_Old.fbx");
        CHECK(oldHead && oldHead->BoundsMax().y > 1.6f && oldHead->BoundsMin().y > 1.3f);
    } else {
        UnitTestSupport::Skip("no Quantum pack: model checks");
    } // the models are destroyed here with nothing uploaded, which must not touch GL

    const auto dir = TempDir();
    const std::string png = (dir / "async_decode.png").string();
    std::vector<unsigned char> px(64 * 32 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 7);
    CHECK(PngWrite::ToFile(std::filesystem::path(png).u8string(), 64, 32, 4, px.data(), 64 * 4));
    TextureImportSettings s;
    s.MaxTextureSize = 16;
    TextureCpuData d = Texture::DecodeFile(png, s);
    CHECK(d.Ok && d.SourceWidth == 64 && d.SourceHeight == 32 && d.Width == 16 && d.Height == 8 && d.Channels == 4);
    CHECK(d.Pixels.size() == (size_t)16 * 8 * 4 && d.GLFormat == 0);
    const auto pending = Texture::CreatePending(std::move(d));
    CHECK(pending && pending->IsPendingUpload() && !pending->IsValid() && pending->Width() == 64);
    CHECK(Texture::CreatePending(Texture::DecodeFile((dir / "missing.png").string(), s)) == nullptr);
    std::error_code ec;
    std::filesystem::remove(png, ec);
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
    // A folder with or without its trailing '/' is one folder; "inside" means a real child.
    const std::string sub = (dir / "Sub").generic_string();
    CHECK(AssetDatabase::SamePath(sub, sub + "/"));
    CHECK(!AssetDatabase::SamePath(a, ""));
    CHECK(AssetDatabase::IsUnderPath(a, sub + "/"));
    CHECK(AssetDatabase::IsUnderPath((dir / "sub" / "deep" / "x.png").string(), sub));
    CHECK(!AssetDatabase::IsUnderPath(sub, sub));
    CHECK(!AssetDatabase::IsUnderPath((dir / "Sub2" / "x.png").string(), sub)); // a sibling sharing a prefix
#ifdef _WIN32
    CHECK(AssetDatabase::PathKey("C:/") == "c:/");
    CHECK(AssetDatabase::IsUnderPath("C:/x/y.png", "C:/"));
#endif
    // The raw-text GUID scan (what a corrupt .meta and the thumbnail prune read).
    CHECK(AssetDatabase::FindMetaGuid("{\"guid\": \"0123456789ABCDEF\"}") == "0123456789abcdef");
    CHECK(AssetDatabase::FindMetaGuid("<<<<<<< HEAD\n  \"guid\"\t:\n\"aaaaaaaaaaaaaaaa\",") == "aaaaaaaaaaaaaaaa");
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": 7, \"x\": {\"guid\":\"1111222233334444\"}") == "1111222233334444");
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdeg\"").empty()); // not hex
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdef0\"").empty()); // 17 digits
    CHECK(AssetDatabase::FindMetaGuid("\"guid\": \"0123456789abcdef").empty());    // cut off

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
    using Changes = std::vector<ProjectWatcher::Change>;
    auto has = [](const Changes& v, K kind, const fs::path& p, const fs::path& old = {}) {
        for (const auto& c : v)
            if (c.Type == kind && AssetDatabase::PathKey(c.Path) == AssetDatabase::PathKey(p.string()) &&
                (old.empty() || AssetDatabase::PathKey(c.OldPath) == AssetDatabase::PathKey(old.string())))
                return true;
        return false;
    };
    // Drains until `done` holds for everything gathered (or 30 s pass: a loaded machine delivers late), then four quiet
    // polls (200 ms) more so a stray extra event still shows. Start() returns with the watch armed. Without `done`, the
    // first quiet spell after anything arrived ends it; that stopped early when a step's events came in two bursts.
    auto settle = [&](const std::function<bool(const Changes&)>& done = {}) {
        Changes all;
        bool overflow = false;
        int quiet = 0;
        for (int i = 0; i < 600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto got = ProjectWatcher::Drain(150, overflow);
            all.insert(all.end(), got.begin(), got.end());
            quiet = got.empty() ? quiet + 1 : 0;
            if (!all.empty() && quiet >= 4 && (!done || done(all))) break;
        }
        return all;
    };

    const fs::path a = dir / "a.png";
    CHECK(AtomicFile::WriteBytes(a, "1", true));
    std::ofstream(dir / "Library" / "cache.bin") << "x"; // ignored folder
    auto c1 = settle([&](const Changes& v) { return has(v, K::Added, a) || has(v, K::Modified, a); });
    CHECK(has(c1, K::Added, a) || has(c1, K::Modified, a));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find("Library") != std::string::npos; }));
    CHECK(std::none_of(c1.begin(), c1.end(), [](const auto& c) { return c.Path.find(".tmp-") != std::string::npos; }));

    const fs::path b = dir / "b.png";
    fs::rename(a, b, ec);
    auto c2 = settle([&](const Changes& v) { return has(v, K::Renamed, b, a); });
    CHECK(has(c2, K::Renamed, b, a));

    const fs::path moved = dir / "sub" / "b.png";
    fs::rename(b, moved, ec);                       // across folders: Removed + Added, paired
    auto c3 = settle([&](const Changes& v) { return has(v, K::Renamed, moved, b); });
    CHECK(has(c3, K::Renamed, moved, b));

    std::ofstream(moved, std::ios::app) << "more";
    auto c4 = settle([&](const Changes& v) { return has(v, K::Modified, moved); });
    CHECK(has(c4, K::Modified, moved));

    const fs::path t = dir / "temp.png";              // created and deleted before it settles
    std::ofstream(t) << "x";
    fs::remove(t, ec);
    fs::remove(moved, ec);
    auto c5 = settle([&](const Changes& v) { return has(v, K::Removed, moved); });
    CHECK(has(c5, K::Removed, moved));
    CHECK(!has(c5, K::Added, t) && !has(c5, K::Removed, t));

    // A file written continuously must not starve other settled changes.
    {
        const fs::path busy = dir / "busy.png", quiet = dir / "quiet.png";
        std::atomic<bool> writing{true};
        std::thread writer([&] {
            while (writing) { std::ofstream(busy, std::ios::app) << "x"; std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
        });
        std::ofstream(quiet) << "q";
        Changes got;
        bool overflow = false;
        for (int i = 0; i < 600 && !has(got, K::Added, quiet) && !has(got, K::Modified, quiet); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto d = ProjectWatcher::Drain(200, overflow);
            got.insert(got.end(), d.begin(), d.end());
        }
        writing = false;
        writer.join();
        CHECK(has(got, K::Added, quiet) || has(got, K::Modified, quiet));
        settle();
    }

    // Two Removed + one Added sharing a file name is ambiguous: no move is guessed.
    {
        fs::create_directories(dir / "d1", ec);
        fs::create_directories(dir / "d2", ec);
        fs::create_directories(dir / "d3", ec);
        std::ofstream(dir / "d1" / "m.png") << "1";
        std::ofstream(dir / "d2" / "m.png") << "2";
        settle([&](const Changes& v) {
            return (has(v, K::Added, dir / "d1" / "m.png") || has(v, K::Modified, dir / "d1" / "m.png")) &&
                   (has(v, K::Added, dir / "d2" / "m.png") || has(v, K::Modified, dir / "d2" / "m.png"));
        });
        fs::remove(dir / "d1" / "m.png", ec);
        fs::remove(dir / "d2" / "m.png", ec);
        std::ofstream(dir / "d3" / "m.png") << "3";
        auto c6 = settle([&](const Changes& v) {
            return has(v, K::Removed, dir / "d1" / "m.png") && has(v, K::Removed, dir / "d2" / "m.png") &&
                   has(v, K::Added, dir / "d3" / "m.png");
        });
        CHECK(has(c6, K::Removed, dir / "d1" / "m.png"));
        CHECK(has(c6, K::Removed, dir / "d2" / "m.png"));
        CHECK(has(c6, K::Added, dir / "d3" / "m.png"));
        CHECK(std::none_of(c6.begin(), c6.end(), [](const auto& c) { return c.Type == K::Renamed; }));
    }

    // bin/obj folders are ordinary now.
    {
        fs::create_directories(dir / "bin", ec);
        std::ofstream(dir / "bin" / "x.png") << "x";
        auto c7 = settle([&](const Changes& v) { return has(v, K::Added, dir / "bin" / "x.png") || has(v, K::Modified, dir / "bin" / "x.png"); });
        CHECK(has(c7, K::Added, dir / "bin" / "x.png") || has(c7, K::Modified, dir / "bin" / "x.png"));
    }

    ProjectWatcher::Stop();
    CHECK(!ProjectWatcher::IsRunning());
    fs::remove_all(dir, ec);
}




void TestAssetPackImport() {
    namespace fs = std::filesystem;
    const fs::path base = TempDir() / "asset_pack_import";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path pack = base / "Library" / "Gun";
    auto touch = [](const fs::path& p) {
        std::error_code e;
        fs::create_directories(p.parent_path(), e);
        std::ofstream(p, std::ios::binary) << "x";
    };
    touch(pack / "Models" / "Gun.fbx");
    touch(pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png");
    touch(pack / "Textures" / "Gun_Normal.png");
    touch(pack / "Textures" / "Detail.png");
    touch(pack / "Models" / "Detail.png"); // closer to the model: must win over Textures/Detail.png
    touch(pack / "Source" / "Gun.blend");
    const std::string models = (pack / "Models").string();
    auto same = [](const std::string& a, const fs::path& b) {
        std::error_code e;
        return fs::equivalent(a, b, e);
    };

    // The FBX points at the author's machine; the file sits in the sibling Textures/ tree.
    CHECK(same(Model::ResolveTexturePathIn(models, "C:\\Users\\author\\Gun_Normal.png"), pack / "Textures" / "Gun_Normal.png"));
    // Case differs and it's two folders down.
    CHECK(same(Model::ResolveTexturePathIn(models, "gun_albedo_transparency.png"), pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png"));
    // Spelling and format differ: separators dropped, .tga shipped as .png.
    CHECK(same(Model::ResolveTexturePathIn(models, "D:/tex/gun_AlbedoTransparency.tga"), pack / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png"));
    CHECK(same(Model::ResolveTexturePathIn(models, "Detail.png"), pack / "Models" / "Detail.png"));
    // Not anywhere: a clean path that doesn't exist, never a wrong file.
    CHECK(!fs::exists(Model::ResolveTexturePathIn(models, "Missing_Roughness.png"), ec));

    // Textures renamed to <Set>_<MapType> after export: found through the material's name.
    using Map = Model::TextureSetMap;
    const fs::path garage = base / "Library" / "Garage";
    touch(garage / "Models" / "Crate.fbx");
    touch(garage / "Textures" / "Garage_Props" / "Garage_Props_Base_Color.png");
    touch(garage / "Textures" / "Garage_Props" / "Garage_Props_Normal.png");
    touch(garage / "Textures" / "Garage_Props_Dark" / "Garage_Props_Dark_Base_Color.png");
    touch(garage / "Textures" / "Bed_Blanket_Clean" / "Bed_Blanket_Clean_Base_Color.png");
    touch(garage / "Textures" / "Bed_Frame_A_Base_Color.png");
    touch(garage / "Textures" / "Bed_Frame_B_Base_Color.png");
    touch(garage / "Textures" / "Mat_Sofa_Base_Color.png");
    touch(garage / "Textures" / "T_Crate_AO.png");
    const std::string gm = (garage / "Models").string();
    CHECK(same(Model::FindTextureSetMap(gm, {"Garage_props", "Crate"}, Map::Albedo),
               garage / "Textures" / "Garage_Props" / "Garage_Props_Base_Color.png")); // exact set beats "_Dark"
    CHECK(same(Model::FindTextureSetMap(gm, {"M_Garage_Props_Mat"}, Map::Normal),
               garage / "Textures" / "Garage_Props" / "Garage_Props_Normal.png"));
    CHECK(Model::FindTextureSetMap(gm, {"Garage_props"}, Map::Roughness).empty());
    CHECK(same(Model::FindTextureSetMap(gm, {"Bed_blanket"}, Map::Albedo),
               garage / "Textures" / "Bed_Blanket_Clean" / "Bed_Blanket_Clean_Base_Color.png")); // one variant
    CHECK(Model::FindTextureSetMap(gm, {"Bed_Frame"}, Map::Albedo).empty());                   // two: no guess
    CHECK(same(Model::FindTextureSetMap(gm, {"mat_sofa"}, Map::Albedo), garage / "Textures" / "Mat_Sofa_Base_Color.png"));
    CHECK(same(Model::FindTextureSetMap(gm, {"1364", "Crate"}, Map::Occlusion), garage / "Textures" / "T_Crate_AO.png"));

    // Classification.
    CHECK(AssetImport::ImportKind("a/b.FBX") == "model");
    CHECK(AssetImport::ImportKind("a/b.Png") == "texture");
    CHECK(AssetImport::ImportKind("a/manifest.json").empty());
    CHECK(AssetImport::IsSourceOnlyFile("a/Gun.blend"));
    CHECK(AssetImport::IsSourceOnlyFile("a/Thumbs.db"));
    CHECK(!AssetImport::IsSourceOnlyFile("a/Gun.fbx"));

    // Texture kind from the file name.
    using TK = AssetImport::TextureKind;
    CHECK(AssetImport::GuessTextureKind("a/Crate_Normal.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Door_1_W_1_K_Nm.tga") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/T_Quantum_Arms_Unity_Normal.1003.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Sofa_Normal_OpenGL_4K.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Wall_Normal_Map.png") == TK::Normal);
    CHECK(AssetImport::GuessTextureKind("a/Crate_Roughness.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/Mat_Sofa_Ambient_Occlusion.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/T_Body.1002_Occlusion.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/Floor_Mask_Map.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/AKS74U_Metallic.png") == TK::Data);
    CHECK(AssetImport::GuessTextureKind("a/AKS74U_Albedo_Transparency.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/Garage_Props_Base_Color.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/Clocks_Emissive.png") == TK::Color);
    CHECK(AssetImport::GuessTextureKind("a/proto_grid_dark.png") == TK::Color);
    CHECK(!AssetLibrary::DefaultTextureSettings("a/Crate_Roughness.png").IsSRGB);
    CHECK(AssetLibrary::DefaultTextureSettings("a/Crate_Normal.png").TextureType == TextureImportSettings::Type::NormalMap);
    CHECK(AssetLibrary::DefaultTextureSettings("a/Crate_Albedo.png").IsSRGB);

    // A dropped folder is copied whole, layout kept, source files left behind.
    const fs::path assets = base / "project" / "assets";
    const fs::path project = base / "project", library = base / "library";
    const fs::path privateAssets = assets / "External";
    auto importDir = [&](const fs::path& source, const std::string& selectedLibrary, const std::string& kind = "") {
        return fs::path(AssetImport::ProjectImportDirectory(project.string(), source.string(), selectedLibrary, kind)).lexically_normal();
    };
    // Destination selection works before the target directories have been created.
    CHECK(importDir(library / "Gun", library.string()) == privateAssets.lexically_normal());
    CHECK(importDir(library / "Gun" / "a.fbx", library.string(), "Models") == (privateAssets / "Models").lexically_normal());
    CHECK(importDir(library, library.string()) == privateAssets.lexically_normal());
    CHECK(importDir(base / "library-other" / "a.fbx", library.string(), "Models") == (assets / "Models").lexically_normal());
    CHECK(importDir(library / ".." / "outside" / "a.png", library.string(), "Textures") == (assets / "Textures").lexically_normal());
    CHECK(importDir(pack, "", "Models") == (assets / "Models").lexically_normal());
    {   // a destination inside the source folder is rejected up front
        const AssetImport::FolderCopy inside = AssetImport::CopyFolderInto(pack.string(), (pack / "Models").string());
        CHECK(inside.Error == "destination is inside the source folder");
        CHECK(inside.Files.empty());
        const AssetImport::FolderCopy self = AssetImport::CopyFolderInto(pack.string(), pack.string());
        CHECK(self.Error == "destination is inside the source folder");
    }
    const AssetImport::FolderCopy privateCopy = AssetImport::CopyFolderInto(pack.string(), privateAssets.string());
    CHECK(privateCopy.Error.empty() && same(privateCopy.Folder, privateAssets / "Gun"));
    CHECK(fs::exists(privateAssets / "Gun" / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png", ec));
    const AssetImport::FolderCopy first = AssetImport::CopyFolderInto(pack.string(), assets.string());
    CHECK(first.Error.empty());
    CHECK(same(first.Folder, assets / "Gun"));
    CHECK(first.Files.size() == 5);
    CHECK(first.Skipped == 1);
    CHECK(fs::exists(assets / "Gun" / "Textures" / "Gun_Textures" / "Gun_Albedo_Transparency.png", ec));
    CHECK(!fs::exists(assets / "Gun" / "Source" / "Gun.blend", ec));
    // ...and the copied model still finds its textures.
    CHECK(same(Model::ResolveTexturePathIn((assets / "Gun" / "Models").string(), "C:/elsewhere/Gun_Normal.png"),
               assets / "Gun" / "Textures" / "Gun_Normal.png"));
    // Identical-file check behind "already in the project - using that copy".
    touch(base / "same_a.bin");
    touch(base / "same_b.bin");
    { std::ofstream(base / "diff.bin", std::ios::binary) << "y"; }
    CHECK(AssetImport::SameContents((base / "same_a.bin").string(), (base / "same_b.bin").string()));
    CHECK(!AssetImport::SameContents((base / "same_a.bin").string(), (base / "diff.bin").string()));
    CHECK(!AssetImport::SameContents((base / "same_a.bin").string(), (base / "missing.bin").string()));

    // A batch's summary counts what each import returned.
    {
        ImportQueueManager queue;
        queue.Enqueue({"a.fbx", "b.fbx", "c.png", "d.txt"});
        int calls = 0;
        queue.Update([&](const std::string& p) -> std::string {
            ++calls;
            return p == "d.txt" ? "" : p == "c.png" ? "texture" : "model";
        }, 10);
        CHECK(calls == 4);
        CHECK(!queue.IsActive());
        CHECK(queue.LastSummary().find("2 models, 1 texture") != std::string::npos);
        CHECK(queue.LastSummary().find("1 failed") != std::string::npos);
        queue.Enqueue({"e.fbx", "f.fbx"});
        queue.Update([](const std::string&) -> std::string { return "model"; }, 1);
        queue.CancelRemaining();
        CHECK(queue.LastSummary().find("1 model") != std::string::npos);
        CHECK(queue.LastSummary().find("1 cancelled") != std::string::npos);
    }

    // A second drop of the same folder never overwrites the first.
    const AssetImport::FolderCopy second = AssetImport::CopyFolderInto(pack.string(), assets.string());
    CHECK(same(second.Folder, assets / "Gun (2)"));

    fs::remove_all(base, ec);
}

// Every test writes under temp_directory_path(), and several runs can go at once (worktrees, CI-style queues): two runs
// sharing %TEMP% wiped each other's fixture folders mid-test (the watcher and asset-identity tests failed "under load").
// Each run gets its own folder under the user's temp folder, removed when the run ends.
struct PrivateTempDir {
    std::filesystem::path Dir;
    std::wstring OldTmp, OldTemp;
    bool HadTmp = false, HadTemp = false;
    static bool Get(const wchar_t* name, std::wstring& out) {
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n == 0) return false;
        out.resize(n);
        out.resize(GetEnvironmentVariableW(name, out.data(), n));
        return true;
    }
    PrivateTempDir() {
        std::error_code ec;
        Dir = std::filesystem::temp_directory_path(ec) / ("TartarusUnitTests-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(Dir, ec);
        if (!std::filesystem::create_directories(Dir, ec) || ec) { Dir.clear(); return; }
        HadTmp = Get(L"TMP", OldTmp);
        HadTemp = Get(L"TEMP", OldTemp);
        SetEnvironmentVariableW(L"TMP", Dir.wstring().c_str());
        SetEnvironmentVariableW(L"TEMP", Dir.wstring().c_str());
    }
    ~PrivateTempDir() {
        if (Dir.empty()) return;
        SetEnvironmentVariableW(L"TMP", HadTmp ? OldTmp.c_str() : nullptr);
        SetEnvironmentVariableW(L"TEMP", HadTemp ? OldTemp.c_str() : nullptr);
        std::error_code ec;
        std::filesystem::remove_all(Dir, ec);
    }
    PrivateTempDir(const PrivateTempDir&) = delete;
    PrivateTempDir& operator=(const PrivateTempDir&) = delete;
};
} // namespace

void UnitTestSupport::EnsureEngineComponents() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
}

int RunUnitTests(const char* filter) {
    const PrivateTempDir privateTemp;
    UnitTestSupport::TestList tests = {
        {"AssetGuid", TestAssetGuid},
        {"AtomicFile", TestAtomicFile},
        {"Utf8Truncate", TestUtf8Truncate},
        {"AssetPackImport", TestAssetPackImport},
        {"AsyncImportCpu", TestAsyncImportCpu},
        {"AssetIdentity", TestAssetIdentity},
        {"ProjectWatcher", TestProjectWatcher},
        {"PhysicMaterial", TestPhysicMaterial},
        {"PostProcessVolume", TestPostProcessVolume},
        {"DopplerVelocity", TestDopplerVelocity},
        {"InputMap", TestInputMap},
        {"AssetMetaMigration", TestAssetMetaMigration},
        {"LogStackTrace", TestLogStackTrace},
        {"PlayerConfigRoundTrip", TestPlayerConfigRoundTrip},
        {"ClipFilterMatch", TestClipFilterMatch},
        {"FramePacing", TestFramePacing},
        {"NpcHitRegions", TestNpcHitRegions},
        {"NpcBodyParts", TestNpcBodyParts},
        {"PhysicsHitchCatchUp", TestPhysicsHitchCatchUp},
        {"PhysicsWorldSync", TestPhysicsWorldSync},
    };
    RegisterRagdollTests(tests);
    RegisterLaunchOptionsTests(tests);
    RegisterAnimationTests(tests);
    RegisterMotionMatchingTests(tests); // UnitTests_MotionMatching.cpp
    RegisterMotionMatchingRuntimeTests(tests); // UnitTests_MotionMatchingRuntime.cpp
    RegisterEditorTests(tests);
    RegisterEnhancerTests(tests);
    RegisterRecoilPortTests(tests);
    RegisterSwayPortTests(tests);
    RegisterRenderTests(tests); // UnitTests_Render.cpp
    RegisterGrassTests(tests); // UnitTests_Grass.cpp
    RegisterWorldTextTests(tests); // UnitTests_WorldText.cpp
    RegisterOpenableTests(tests); // UnitTests_Openable.cpp
    RegisterLightSwitchTests(tests); // UnitTests_LightSwitch.cpp
    RegisterCinematicTests(tests); // UnitTests_Cinematic.cpp
    RegisterFirstPersonWeaponsTests(tests); // UnitTests_FirstPersonWeapons.cpp
    RegisterOutfitsTests(tests); // UnitTests_Outfits.cpp
    RegisterFirstPersonBodyTests(tests); // UnitTests_FirstPersonBody.cpp
    RegisterCharacterMoverTests(tests); // UnitTests_CharacterMover.cpp
    RegisterEngineTests(tests);
    RegisterGameplayTests(tests);
    RegisterBloodTests(tests);
    RegisterGlassTests(tests);
    RegisterEditorSoundTests(tests); // UnitTests_EditorSound.cpp
    RegisterAudioTests(tests); // UnitTests_Audio.cpp
    RegisterDecalRenderTests(tests); // UnitTests_DecalRender.cpp
    RegisterDecalRuntimeTests(tests); // UnitTests_DecalRuntime.cpp
    RegisterDecalAssetTests(tests); // UnitTests_DecalAssets.cpp
    RegisterGraffitiTests(tests); // UnitTests_Graffiti.cpp
    RegisterGizmoIconTests(tests); // UnitTests_GizmoIcons.cpp
    RegisterAssetLibraryTests(tests); // UnitTests_AssetLibrary.cpp
    {
        std::set<std::string> names;
        for (const auto& t : tests)
            if (!names.insert(t.first).second) {
                std::cout << "[UnitTest] FAIL two tests are both named \"" << t.first << "\"\n";
                ++g_Failures;
            }
    }
    int ran = 0, skipped = 0;
    for (const auto& [name, fn] : tests) {
        if(filter && std::string(name).find(filter)==std::string::npos) continue;
        ++ran;
        g_CurrentTest = name;
        // Named before it runs, with both streams flushed (tests and the engine also print through printf and the
        // log), so a test that hangs until a CI timeout is the last line in the log.
        std::cout << "[UnitTest] RUN  " << name << std::endl;
        std::fflush(stdout);
        UnitTestSupport::g_SkipReason.clear();
        const int before = g_Failures, checksBefore = g_Checks;
        const auto started = std::chrono::steady_clock::now();
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw " << e.what() << "\n";
        } catch (...) {
            ++g_Failures;
            std::cout << "[UnitTest] FAIL " << name << ": threw a non-std exception\n";
        }
        const std::string& skip = UnitTestSupport::g_SkipReason;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const std::string took = " (" + std::to_string(ms) + " ms)";
        if (g_Failures != before) std::cout << "[UnitTest] FAIL " << name << took << "\n";
        else if (!skip.empty() && g_Checks == checksBefore) { ++skipped; std::cout << "[UnitTest] SKIP " << name << ": " << skip << took << "\n"; }
        else if (!skip.empty()) std::cout << "[UnitTest] PASS " << name << " (partly skipped: " << skip << ")" << took << "\n";
        else std::cout << "[UnitTest] PASS " << name << took << "\n";
        std::cout.flush();
        std::fflush(stdout);
    }
    // A filter that matches nothing is a typo, not a pass.
    if (filter && ran == 0) {
        std::cout << "[UnitTest] FAIL no test name contains \"" << filter << "\"\n";
        ++g_Failures;
    }
    std::cout << "[UnitTest] " << g_Checks << " checks, " << g_Failures << " failure(s), " << ran << " test(s) run, " << skipped
              << " skipped\n";
    std::cout.flush();
    return g_Failures;
}
