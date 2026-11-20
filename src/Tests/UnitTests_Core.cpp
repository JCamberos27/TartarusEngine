// Core, physics and player-config unit tests (moved from UnitTests.cpp): input map, atomic files, UTF-8, log stack traces, frame pacing, player.json, physic materials, the PhysX world sync.
// Pure C++ like the rest of --unit-tests (no window, GL or audio device).
#include "TimeService.h"
#include "UnitTestSupport.h"
#include "../Game/FirstPersonMovementInput.h"

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

// Moved from UnitTests.cpp (pure move).
namespace {
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

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

} // namespace

void RegisterCoreTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"InputMap", TestInputMap});
    tests.push_back({"LogStackTrace", TestLogStackTrace});
    tests.push_back({"FramePacing", TestFramePacing});
    tests.push_back({"PlayerConfigRoundTrip", TestPlayerConfigRoundTrip});
    tests.push_back({"AtomicFile", TestAtomicFile});
    tests.push_back({"Utf8Truncate", TestUtf8Truncate});
    tests.push_back({"PhysicMaterial", TestPhysicMaterial});
    tests.push_back({"PhysicsHitchCatchUp", TestPhysicsHitchCatchUp});
    tests.push_back({"PhysicsWorldSync", TestPhysicsWorldSync});
}
