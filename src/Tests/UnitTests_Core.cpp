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
#include <map>
#include "Log.h" // #178 stack traces
#include "FrameLoads.h"
#include "Tests/PerfG0.h"
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
#include "Profiler.h"
#include "Tests/PerfBench.h"
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
    const std::string path = (TempDir() / "tartarus_ut.physicmaterial").string();
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

// Gravity and the layer collision matrix, snapshotted at Create, follow a settings change during Play.
void TestPhysicsApplySettings() {
    ProjectSettings::PhysicsSettings& p = ProjectSettings::MutablePhysics();
    const ProjectSettings::PhysicsSettings saved = p;
    CHECK(!PhysicsWorld::ApplySettings()); // nothing running
    World world;
    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    if (PhysicsWorld::IsActive()) {
        CHECK(!PhysicsWorld::ApplySettings()); // same settings as Create: nothing to do
        p.Gravity.y = saved.Gravity.y * 0.5f - 1.0f;
        CHECK(PhysicsWorld::ApplySettings());
        CHECK(!PhysicsWorld::ApplySettings()); // applied once
        p.LayerCollisionMask[3] ^= 1u << 5;
        CHECK(PhysicsWorld::ApplySettings());
        CHECK(!PhysicsWorld::ApplySettings());
        PhysicsWorld::Destroy();
    }
    p = saved;
}

// A Physic Material saved during Play reaches the running colliders that use it, and only those.
void TestPhysicsRefreshPhysicMaterial() {
    const std::string path = (TempDir() / "tartarus_ut_live.physicmaterial").string();
    PhysicMaterialAsset m;
    m.DynamicFriction = 0.2f;
    CHECK(m.SaveFile(path));
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    for (int i = 0; i < 3; ++i) {
        const entt::entity e = world.CreateEmptyEntity(glm::vec3(3.0f * i, 0.0f, 0.0f), zero, one, "Slab");
        ColliderComponent col;
        col.Kind = ColliderComponent::Shape::Box;
        if (i < 2) col.Material = path; // two use it, one doesn't
        world.Registry.emplace<ColliderComponent>(e, col);
    }
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    CHECK(PhysicsWorld::RefreshPhysicMaterial(world, path) == 0); // nothing running
    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    m.DynamicFriction = 0.9f;
    CHECK(m.SaveFile(path));
    CHECK(PhysicsWorld::RefreshPhysicMaterial(world, path) == 2);
    CHECK(PhysicsWorld::RefreshPhysicMaterial(world, path + ".other") == 0);
    PhysicsWorld::Destroy();
    std::error_code ec;
    std::filesystem::remove(path, ec);
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

// --- FrameLoads: off by default, Take() hands over a frame's loads once, Summarize groups them by kind ------------
// --perf-bench's "unscoped" time: a scope inside another is Nested (not counted twice), and the frame's time outside
// every top-level scope is what LastFrameUnscopedMs reports. Only lower bounds: the sleeps last at least as long.
void TestProfilerUnscoped() {
    using namespace std::chrono_literals;
    Profiler::BeginFrame();
    {
        PROFILE_SCOPE("Unscoped Test Outer");
        { PROFILE_SCOPE("Unscoped Test Inner"); std::this_thread::sleep_for(5ms); }
    }
    std::this_thread::sleep_for(20ms); // outside every scope
    Profiler::BeginFrame();
    int outer = 0, inner = 0;
    for (const Profiler::Entry& e : Profiler::GetLastFrame()) {
        if (e.Name == "Unscoped Test Outer") { ++outer; CHECK(!e.Nested); CHECK(e.Milliseconds >= 4.5f); }
        if (e.Name == "Unscoped Test Inner") { ++inner; CHECK(e.Nested); }
    }
    CHECK(outer == 1 && inner == 1);
    CHECK(Profiler::LastFrameWallMs() >= 24.5f);
    CHECK(Profiler::LastFrameUnscopedMs() >= 19.5f);
    Profiler::BeginFrame(); // leave an empty frame behind
}

// --perf-bench's G-0 keys: scope names become lowercase words joined by '_'.
void TestPerfG0Keys() {
    CHECK(PerfG0::KeyOf("Scene: Classify Visible") == "scene_classify_visible");
    CHECK(PerfG0::KeyOf("Sun: Fit + Gather") == "sun_fit_gather");
    CHECK(PerfG0::KeyOf("SSAO Depth From Pre-pass") == "ssao_depth_from_pre_pass");
    CHECK(PerfG0::KeyOf("  ") == "");
}

// --perf-bench's HALF report: top-level scopes land in their cpu.* bucket (prefix rules too, unmapped ones in "other"),
// and each AddHalf key keeps its own samples (a lagged GPU key can trail the CPU ones).
void TestPerfBenchHalves() {
    CHECK(std::string(PerfBench::CpuBucket("Physics Step")) == "physics");
    CHECK(std::string(PerfBench::CpuBucket("Animators")) == "anim");
    CHECK(std::string(PerfBench::CpuBucket("First Person IK")) == "anim");
    CHECK(std::string(PerfBench::CpuBucket("Player Shots + Impacts")) == "sim");
    CHECK(std::string(PerfBench::CpuBucket("Audio Update")) == "audio");
    CHECK(std::string(PerfBench::CpuBucket("UI Module Draw")) == "ui");
    CHECK(std::string(PerfBench::CpuBucket("IBL Bake (Sky)")) == "render");
    CHECK(std::string(PerfBench::CpuBucket("Swap Buffers")) == "wait");
    CHECK(std::string(PerfBench::CpuBucket("Asset Pump + Outfit Hiding")) == "assets");
    CHECK(std::string(PerfBench::CpuBucket("Nobody Mapped This")) == "other");
    CHECK(std::string(PerfBench::CpuBucket("UI")) == "other"); // "UI *" needs the space
    PerfBench bench;
    CHECK(bench.LastHalf("gpu.frame") == -1.0);
    bench.AddHalf("gpu.frame", 4.0);
    bench.AddHalf("gpu.frame", 6.0);
    CHECK(bench.LastHalf("gpu.frame") == 6.0);
    CHECK(bench.LastHalf("cpu.sim") == -1.0);
}

void TestFrameLoads() {
    std::vector<FrameLoads::Event> got;
    FrameLoads::Enable(false);
    FrameLoads::Note("shader", "ignored", 1.0);
    FrameLoads::Take(got);
    CHECK(got.empty());
    FrameLoads::Enable(true);
    FrameLoads::Note("texture upload", "assets/a/rock.png", 2.0);
    FrameLoads::Note("shader", "SSAO", 5.5);
    FrameLoads::Note("texture upload", "assets\\b\\moss.png", 1.0);
    FrameLoads::Take(got);
    CHECK(got.size() == 3);
    CHECK(FrameLoads::Summarize(got) == "texture upload x2 3.00 ms (rock.png moss.png); shader x1 5.50 ms (SSAO)");
    FrameLoads::Take(got); // taken already: the next frame starts empty
    CHECK(got.empty());
    for (int i = 0; i < 5; ++i) FrameLoads::Note("model load (sync)", "m" + std::to_string(i), 1.0);
    FrameLoads::Take(got);
    CHECK(FrameLoads::Summarize(got) == "model load (sync) x5 5.00 ms (m0 m1 m2 ...)");
    FrameLoads::Enable(false);
}

} // namespace

// The world transform cache across small structure changes (what an outfit piece landing does: create + attach,
// re-parent, destroy a subtree, a destroy straight through the registry (also with a create the same frame), a
// transform removed and added back): every
// frame's cached matrices match a fresh compose, each moved or new one is reported changed, membership changes are
// flagged, and none of these frames takes the full walk.
void TestTransformCacheStructureChanges() {
    World world;
    std::uint32_t rng = 12345;
    auto rand = [&](std::uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
    auto randVec = [&] { return glm::vec3((float)rand(100) * 0.1f - 5.0f, (float)rand(100) * 0.1f, (float)rand(100) * 0.1f - 5.0f); };
    std::vector<entt::entity> live;
    for (int i = 0; i < 200; ++i) {
        const entt::entity e = world.CreateEmptyEntity(randVec(), glm::vec3(0.0f, (float)rand(360), 0.0f), glm::vec3(1.0f), "E");
        if (!live.empty() && rand(3) != 0) world.AttachChildRaw(e, live[rand((std::uint32_t)live.size())]);
        live.push_back(e);
    }
    std::map<entt::entity, glm::mat4> last;
    auto near = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > 1e-3f) return false;
        return true;
    };
    auto check = [&](bool membershipExpected) {
        world.RebuildWorldTransformCache();
        const auto& changedList = world.ChangedWorldTransforms();
        const std::set<entt::entity> changed(changedList.begin(), changedList.end());
        std::map<entt::entity, glm::mat4> now;
        bool allMatch = true, allReported = true;
        for (entt::entity e : world.Registry.view<TransformComponent>()) {
            const glm::mat4 cached = world.GetCachedWorldTransform(e);
            allMatch = allMatch && near(cached, world.ComposeWorldTransform(e));
            const auto it = last.find(e);
            if (it == last.end() || !near(it->second, cached)) allReported = allReported && changed.count(e) != 0;
            now[e] = cached;
        }
        CHECK(allMatch);
        CHECK(allReported);
        if (membershipExpected) CHECK(world.TransformCacheMembershipChanged());
        last = std::move(now);
    };
    check(true);
    const std::uint64_t fullAfterLoad = world.TransformCacheFullWalks(); // the first build
    auto pick = [&] { return live[rand((std::uint32_t)live.size())]; };
    auto prune = [&] {
        live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !world.Registry.valid(e); }), live.end());
    };
    for (int frame = 0; frame < 70; ++frame) {
        bool membership = false;
        switch (frame % 7) {
        case 0: { // a piece lands: created and attached under a live entity
            const entt::entity e = world.CreateEmptyEntity(randVec(), glm::vec3(0.0f), glm::vec3(1.0f), "Piece");
            world.AttachChildRaw(e, pick());
            live.push_back(e);
            membership = true;
            break;
        }
        case 1: world.SetParent(pick(), rand(4) == 0 ? entt::entity{entt::null} : pick()); break; // cycles refused
        case 2: world.DestroyEntityAndChildren(pick()); prune(); membership = true; break;
        case 3: world.Registry.destroy(pick()); prune(); membership = true; break; // children keep a dead parent id
        case 4: { // a transform removed under its children, then added back next time round
            const entt::entity e = pick();
            if (world.Registry.all_of<TransformComponent>(e)) world.Registry.remove<TransformComponent>(e);
            break;
        }
        case 5:
            for (entt::entity e : live)
                if (!world.Registry.all_of<TransformComponent>(e)) world.Registry.emplace<TransformComponent>(e, randVec(), glm::vec3(0.0f), glm::vec3(1.0f));
            break;
        case 6: { // a parent destroyed straight through the registry and a piece created the same frame: the pool
                  // sizes and the hierarchy version balance, yet the parent's children are now roots
            entt::entity parent = entt::null;
            for (entt::entity e : live)
                if (const auto* h = world.Registry.try_get<HierarchyComponent>(e); h && !h->Children.empty()) { parent = e; break; }
            if (parent != entt::null) world.Registry.destroy(parent);
            const entt::entity piece = world.CreateEmptyEntity(randVec(), glm::vec3(0.0f), glm::vec3(1.0f), "Piece");
            world.Registry.emplace<HierarchyComponent>(piece); // as a loader does: no hierarchy version bump
            live.push_back(piece);
            prune();
            membership = true;
            break;
        }
        }
        world.Registry.get_or_emplace<TransformComponent>(pick()).Position += glm::vec3(0.5f, 0.0f, 0.0f); // and one moves
        check(membership);
    }
    CHECK(world.TransformCacheFullWalks() == fullAfterLoad);
}

// Active-in-hierarchy patched from the transform cache's structure delta (pieces created under deactivated and active
// parents, re-parented across them, destroyed with or without their children, a destroy and a create in one frame, a
// transform removed and added back, a checkbox flipped): the InactiveTag set is what a full SyncActiveInHierarchy
// gives, every frame; and the small structure changes do take the patch.
void TestActiveInHierarchyPatch() {
    World world;
    std::uint32_t rng = 777;
    auto rand = [&](std::uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
    std::vector<entt::entity> live;
    auto make = [&](const char* name) {
        return world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), name);
    };
    for (int i = 0; i < 200; ++i) {
        const entt::entity e = make("E");
        if (!live.empty() && rand(3) != 0) world.AttachChildRaw(e, live[rand((std::uint32_t)live.size())]);
        if (rand(8) == 0) world.Registry.emplace<DeactivatedTag>(e);
        live.push_back(e);
    }
    auto inactiveSet = [&] {
        std::vector<std::uint32_t> out;
        for (entt::entity e : world.Registry.view<InactiveTag>()) out.push_back(entt::to_integral(e));
        std::sort(out.begin(), out.end());
        return out;
    };
    auto pick = [&] { return live[rand((std::uint32_t)live.size())]; };
    auto prune = [&] {
        live.erase(std::remove_if(live.begin(), live.end(), [&](entt::entity e) { return !world.Registry.valid(e); }), live.end());
    };
    world.RebuildWorldTransformCache();
    int structureFrames = 0, patched = 0, mismatches = 0;
    for (int frame = 0; frame < 200; ++frame) {
        bool structure = true;
        while (live.size() < 20) live.push_back(make("Root")); // destroys can empty the pool
        switch (frame % 8) {
        case 0: case 1: { // a piece (sometimes with a child) lands under some entity
            const entt::entity e = make("Piece");
            world.AttachChildRaw(e, pick());
            live.push_back(e);
            if (rand(2) == 0) {
                const entt::entity c = make("Sub");
                world.AttachChildRaw(c, e);
                live.push_back(c);
            }
            break;
        }
        case 2: { // re-parented (SetParent keeps the world pose, so both ends need a transform; cycles refused)
            auto pickT = [&] {
                for (int tries = 0; tries < 64; ++tries)
                    if (const entt::entity e = pick(); world.Registry.all_of<TransformComponent>(e)) return e;
                return entt::entity{entt::null};
            };
            const entt::entity child = pickT(), parent = rand(4) == 0 ? entt::entity{entt::null} : pickT();
            if (child != entt::null) world.SetParent(child, parent);
            break;
        }
        case 3: world.DestroyEntityAndChildren(pick()); prune(); break;
        case 4: world.Registry.destroy(pick()); prune(); break; // children keep a dead parent id: roots now
        case 5: { // a destroy straight through the registry and a create in the same frame
            world.Registry.destroy(pick());
            prune();
            const entt::entity e = make("Piece");
            if (!live.empty()) world.AttachChildRaw(e, pick());
            live.push_back(e);
            break;
        }
        case 6: { // a transform removed in place (or added back): the full sync's job
            const entt::entity e = pick();
            if (world.Registry.all_of<TransformComponent>(e)) world.Registry.remove<TransformComponent>(e);
            else world.Registry.emplace<TransformComponent>(e);
            structure = false;
            break;
        }
        default: { // a checkbox flipped: the full sync's job
            const entt::entity e = pick();
            if (world.Registry.all_of<DeactivatedTag>(e)) world.Registry.remove<DeactivatedTag>(e);
            else world.Registry.emplace<DeactivatedTag>(e);
            structure = false;
            break;
        }
        }
        const std::uint64_t fullBefore = world.ActiveFullSyncs();
        world.RebuildWorldTransformCache();
        if (structure && world.TransformStructureChanges().Valid) {
            ++structureFrames;
            patched += world.ActiveFullSyncs() == fullBefore ? 1 : 0;
        }
        const auto got = inactiveSet();
        world.SyncActiveInHierarchy(); // from scratch: changes nothing when the patch was right
        if (got != inactiveSet()) {
            if (++mismatches == 1) std::printf("[ActiveInHierarchyPatch] frame %d: %zu inactive patched, %zu from scratch\n", frame,
                                               got.size(), inactiveSet().size());
        }
    }
    std::printf("[ActiveInHierarchyPatch] %d of %d structure-change frames patched\n", patched, structureFrames);
    CHECK(mismatches == 0);
    CHECK(structureFrames > 100);
    CHECK(patched * 3 > structureFrames * 2);
}

void RegisterCoreTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"ActiveInHierarchyPatch", TestActiveInHierarchyPatch});
    tests.push_back({"TransformCacheStructureChanges", TestTransformCacheStructureChanges});
    tests.push_back({"InputMap", TestInputMap});
    tests.push_back({"LogStackTrace", TestLogStackTrace});
    tests.push_back({"FramePacing", TestFramePacing});
    tests.push_back({"PlayerConfigRoundTrip", TestPlayerConfigRoundTrip});
    tests.push_back({"AtomicFile", TestAtomicFile});
    tests.push_back({"Utf8Truncate", TestUtf8Truncate});
    tests.push_back({"FrameLoads", TestFrameLoads});
    tests.push_back({"ProfilerUnscoped", TestProfilerUnscoped});
    tests.push_back({"PerfG0Keys", TestPerfG0Keys});
    tests.push_back({"PerfBenchHalves", TestPerfBenchHalves});
    tests.push_back({"PhysicMaterial", TestPhysicMaterial});
    tests.push_back({"PhysicsHitchCatchUp", TestPhysicsHitchCatchUp});
    tests.push_back({"PhysicsApplySettings", TestPhysicsApplySettings});
    tests.push_back({"PhysicsRefreshPhysicMaterial", TestPhysicsRefreshPhysicMaterial});
    tests.push_back({"PhysicsWorldSync", TestPhysicsWorldSync});
}
