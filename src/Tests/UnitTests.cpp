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
#include "UnitTests.h"

#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "RootMotion.h"
#include "FirstPersonBody.h"
#include "FirstPersonBodyContract.h"
#include "Curve.h"
#include "IK.h"
#include "FirstPersonWeaponWizard.h"
#include "ClipAnalysis.h"
#include "FirstPersonAnimation.h"
#include "FirstPersonAdsCarry.h"
#include "AudioEngine.h"
#include "AI/AiMath.h"
#include "AI/SquadVoice.h"
#include <set>
#include "Log.h" // #178 stack traces
#include "InputMap.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "Components.h"
#include "AtomicFile.h"
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
#include "Player.h"
#include "PostProcessVolume.h"
#include "Tonemapper.h"
#include "ProjectPaths.h"
#include "ProjectWatcher.h"
#include "TextureCache.h"
#include "Model.h"       // texture-path resolver
#include "AssetImport.h" // folder import
#include "ImportQueueManager.h"
#include "SceneSerializer.h" // #121
#include "AnimationSystem.h"           // #123 - the spin systems, driven frame by frame below
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
#include <stb_image_write.h> // async decode test fixture
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
        CHECK(count("Weapon1") == 1);        // the default the file predates comes back
    }

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

// #123 - quaternion is the authoritative transform rotation; Euler is only a stable editor view,
// and v3 Euler scene data migrates to the v4 quaternion representation without changing pose.
void TestQuaternionTransformStorage() {
    auto nearMatrix = [](const glm::mat3& a, const glm::mat3& b, float eps = 1e-4f) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(a[c][r] - b[c][r]) > eps) return false;
        return true;
    };

    TransformComponent transform;
    const glm::vec3 authored(0.0f, 180.0f, 0.0f);
    transform.SetRotationEuler(authored);
    CHECK(glm::length(transform.EulerDegrees() - authored) < 1e-3f); // preserve the authored hint
    CHECK(std::abs(glm::length(transform.Rotation) - 1.0f) < 1e-5f);

    // Cross the old Euler singularity by composing quaternions. The stored orientation remains
    // exact even though no unique Euler triple exists at the middle pose.
    const glm::quat base = QuaternionFromEulerYXZ(glm::vec3(89.9f, 15.0f, -20.0f));
    const glm::quat turned = RotateAboutLocalAxis(base, glm::vec3(0.3f, 1.0f, 0.2f), 120.0f);
    transform.SetRotationQuaternion(turned);
    CHECK(SameRotation(transform.Rotation, turned));
    CHECK(nearMatrix(glm::mat3(ComposeTransform(transform)), glm::mat3_cast(turned)));

    World migrated;
    AssetLibrary assets;
    const std::string v3 = R"({"formatVersion":3,"empties":[{"name":"Legacy","id":0,"parentId":-1,)"
        R"("position":[0,0,0],"rotation":[25,40,-60],"scale":[1,1,1]}]})";
    CHECK(SceneSerializer::LoadFromString(migrated, assets, v3));
    const auto view = migrated.Registry.view<TransformComponent>();
    CHECK(view.size() == 1);
    if (!view.empty()) {
        const auto& loaded = migrated.Registry.get<TransformComponent>(*view.begin());
        CHECK(SameRotation(loaded.Rotation, QuaternionFromEulerYXZ(glm::vec3(25.0f, 40.0f, -60.0f))));
        CHECK(glm::length(loaded.EulerDegrees() - glm::vec3(25.0f, 40.0f, -60.0f)) < 1e-3f);
    }

    const json saved = json::parse(SceneSerializer::SaveToString(migrated, assets));
    CHECK(saved.value("formatVersion", 0) == 4);
    CHECK(saved["empties"].size() == 1);
    CHECK(saved["empties"][0]["rotation"].is_array() && saved["empties"][0]["rotation"].size() == 4);

    World reloaded;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(reloaded, assets2, saved.dump()));
    const auto reloadedView = reloaded.Registry.view<TransformComponent>();
    CHECK(reloadedView.size() == 1);
    if (!reloadedView.empty()) {
        CHECK(SameRotation(reloaded.Registry.get<TransformComponent>(*reloadedView.begin()).Rotation,
                           QuaternionFromEulerYXZ(glm::vec3(25.0f, 40.0f, -60.0f))));
    }

    // File loads upgrade even an empty v3 scene: the format changed independently of whether
    // that particular file happened to contain a transform, and the original remains backed up.
    const auto migrationPath = TempDir() / "quaternion-v3-migration.json";
    const auto backupPath = std::filesystem::path(migrationPath.string() + ".bak");
    std::error_code ec;
    std::filesystem::remove(migrationPath, ec);
    std::filesystem::remove(backupPath, ec);
    const std::string emptyV3 = R"({"formatVersion":3,"empties":[]})";
    CHECK(AtomicFile::WriteBytes(migrationPath, emptyV3, false));
    World emptyMigrated;
    AssetLibrary assets3;
    CHECK(SceneSerializer::Load(emptyMigrated, assets3, migrationPath.string(), true));
    CHECK(json::parse(ReadAll(migrationPath)).value("formatVersion", 0) == 4);
    CHECK(ReadAll(backupPath) == emptyV3);
    std::filesystem::remove(migrationPath, ec);
    std::filesystem::remove(backupPath, ec);
}

// #123 - spinning about a diagonal axis turns the object about that axis. Adding axis * angle to
// the Euler components (the old behaviour) added equal pitch and yaw, which is another rotation.
void TestRotateEulerAboutLocalAxis() {
    auto rotationOf = [](const glm::vec3& euler) { return glm::mat3(ComposeTransform(glm::vec3(0.0f), euler, glm::vec3(1.0f))); };
    auto sameRotation = [&](const glm::vec3& a, const glm::mat3& expected) {
        const glm::mat3 m = rotationOf(a);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(m[c][r] - expected[c][r]) > 1e-4f) return false;
        return true;
    };
    // A diagonal axis from the identity is exactly angleAxis about it, at any angle...
    const glm::vec3 diag(1.0f, 1.0f, 0.0f);
    for (float angle : {10.0f, 90.0f, 200.0f, -45.0f}) {
        const glm::mat3 expected = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::normalize(diag)));
        CHECK(sameRotation(RotateEulerAboutLocalAxis(glm::vec3(0.0f), diag, angle), expected));
    }
    // ...and the axis is left unmoved by the turn (the old add-to-Euler result moved it).
    const glm::mat3 turned = rotationOf(RotateEulerAboutLocalAxis(glm::vec3(0.0f), diag, 90.0f));
    CHECK(glm::length(turned * glm::normalize(diag) - glm::normalize(diag)) < 1e-4f);
    CHECK(glm::length(rotationOf(glm::vec3(90.0f, 90.0f, 0.0f)) * glm::normalize(diag) - glm::normalize(diag)) > 0.1f);
    // The axis is local: the result is the starting orientation followed by the turn.
    const glm::vec3 start(30.0f, -20.0f, 50.0f);
    const glm::mat3 local = rotationOf(start) * glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(60.0f), glm::normalize(diag)));
    CHECK(sameRotation(RotateEulerAboutLocalAxis(start, diag, 60.0f), local));
    // The default spin, +Y from an untilted start, is still plain yaw.
    CHECK(sameRotation(RotateEulerAboutLocalAxis(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 37.0f), rotationOf(glm::vec3(0.0f, 37.0f, 0.0f))));
    // Axis magnitude is irrelevant, and a zero axis or zero angle is a no-op.
    CHECK(sameRotation(RotateEulerAboutLocalAxis(start, diag * 5.0f, 60.0f), local));
    CHECK(RotateEulerAboutLocalAxis(start, glm::vec3(0.0f), 60.0f) == start);
    CHECK(RotateEulerAboutLocalAxis(start, diag, 0.0f) == start);
}

// #123 - the Euler-driven spin systems (Animator, Transform Controller), run frame by frame, turn about the axis they were
// given rather than adding it to the Euler components.
void TestSpinSystemsAboutDiagonalAxis() {
    auto matrixOf = [](const glm::vec3& euler) { return glm::mat3(ComposeTransform(glm::vec3(0.0f), euler, glm::vec3(1.0f))); };
    auto turnAbout = [](const glm::vec3& axis, float deg) { return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis)); };
    auto near = [](const glm::mat3& a, const glm::mat3& b, float eps) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(a[c][r] - b[c][r]) > eps) return false;
        return true;
    };

    // TransformControllerSystem is compiled only into TartarusGame.dll, so run
    // it the way the editor does: load the built module next to the exe and tick it.
    HotReloadGameModule gameModule;
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    gameModule.Initialize(std::filesystem::path(exePath).parent_path() / TARTARUS_GAME_MODULE_FILENAME);

    // Animator: the base orientation turned by |w| * t about w's own direction.
    {
        World world;
        const glm::vec3 base(20.0f, 30.0f, 0.0f);
        entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), base, glm::vec3(1.0f), "a");
        AnimatorComponent anim; anim.SpinDegPerSec = glm::vec3(60.0f, 60.0f, 0.0f);
        world.Registry.emplace<AnimatorComponent>(e, anim);
        UpdateAnimators(world, 0.5f); // initialises and advances t to 0.5 s
        const float angle = glm::length(anim.SpinDegPerSec) * 0.5f;
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(anim.SpinDegPerSec), angle), 1e-3f));
        UpdateAnimators(world, 100.0f); // a long time later: same form, still reversible and bounded
        const float later = std::fmod(glm::length(anim.SpinDegPerSec) * 100.5f, 360.0f);
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(anim.SpinDegPerSec), later), 2e-2f));
    }

    // Transform controller: same rule for RotationDegPerSec.
    {
        World world;
        const glm::vec3 base(0.0f, 15.0f, 0.0f);
        entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), base, glm::vec3(1.0f), "t");
        TransformControllerComponent ctl; ctl.RotationDegPerSec = glm::vec3(0.0f, 45.0f, 45.0f);
        world.Registry.emplace<TransformControllerComponent>(e, ctl);
        gameModule.Tick(world, 2.0f, true);
        CHECK(near(glm::mat3_cast(world.Registry.get<TransformComponent>(e).Rotation),
                   matrixOf(base) * turnAbout(glm::normalize(ctl.RotationDegPerSec), glm::length(ctl.RotationDegPerSec) * 2.0f), 1e-3f));
    }

    gameModule.Shutdown();
}

// #369 - a texture a .mat names as a data map defaults to linear (and a normal map to the NormalMap
// type) when its .meta has no importer block, instead of the sRGB colour default that bends normals.
// An explicit importer block still wins. Only the settings step is exercised - it needs no GL.
void TestMaterialTextureDefaults() {
    namespace fs = std::filesystem;
    const fs::path dir = TempDir() / "mat_tex_defaults";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto make = [&](const char* name, const char* metaJson) {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << "not decoded by this test";
        if (metaJson) std::ofstream(fs::path(p.string() + ".meta"), std::ios::binary) << metaJson;
        return p.generic_string();
    };
    const char* bare = R"({"guid":"a1b2c3d4e5f60718","metaVersion":1,"type":"texture"})";
    AssetLibrary assets;
    using Use = AssetLibrary::TextureUse;

    // A bare sidecar: normal maps become linear NormalMap textures, other data maps linear.
    const std::string normal = make("n.png", bare), rough = make("r.png", bare), albedo = make("a.png", bare);
    assets.ResolveTextureSettings(normal, Use::Normal);
    assets.ResolveTextureSettings(rough, Use::Data);
    assets.ResolveTextureSettings(albedo, Use::Color);
    CHECK(!assets.GetTextureSettings(normal).IsSRGB);
    CHECK(assets.GetTextureSettings(normal).TextureType == TextureImportSettings::Type::NormalMap);
    CHECK(!assets.GetTextureSettings(rough).IsSRGB);
    CHECK(assets.GetTextureSettings(rough).TextureType == TextureImportSettings::Type::Default);
    // Colour maps keep the sRGB default, and nothing is remembered for them.
    CHECK(assets.GetTextureSettings(albedo).IsSRGB);
    CHECK(assets.TextureSettingsMap().count(albedo) == 0);

    // No .meta at all behaves the same as a bare one.
    const std::string noMeta = make("m.png", nullptr);
    assets.ResolveTextureSettings(noMeta, Use::Normal);
    CHECK(!assets.GetTextureSettings(noMeta).IsSRGB);

    // An explicit importer block is an override: even a normal-map slot keeps what the file says.
    const std::string forced = make("f.png", R"({"guid":"0011223344556677","type":"texture","importer":{"isSRGB":true,"textureType":0,"anisoLevel":2}})");
    assets.ResolveTextureSettings(forced, Use::Normal);
    CHECK(assets.GetTextureSettings(forced).IsSRGB);
    CHECK(assets.GetTextureSettings(forced).AnisoLevel == 2);

    // Settings already known in memory are never replaced by a later, different use.
    assets.ResolveTextureSettings(rough, Use::Normal);
    CHECK(assets.GetTextureSettings(rough).TextureType == TextureImportSettings::Type::Default);

    // Nothing was written to any sidecar.
    CHECK(ReadAll(dir / "n.png.meta") == bare);
    fs::remove_all(dir, ec);
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

    TransformControllerComponent ctl;
    ctl.RotationDegPerSec = glm::vec3(1.0f, 0.0f, 0.0f);
    ctl.ScalePulseFrequencyHz = 123.5f;
    world.Registry.emplace<TransformControllerComponent>(src, ctl);

    const std::string preset = SceneSerializer::ComponentToPresetJson(world, src, "Transform Controller");
    CHECK(!preset.empty());
    CHECK(SceneSerializer::PresetComponentName(preset) == "Transform Controller");

    // Applies to an entity that does not have the component yet: it is added, not skipped.
    CHECK(!world.Registry.all_of<TransformControllerComponent>(dst));
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst, preset));
    CHECK(world.Registry.all_of<TransformControllerComponent>(dst));
    const TransformControllerComponent& got = world.Registry.get<TransformControllerComponent>(dst);
    CHECK(got.ScalePulseFrequencyHz == 123.5f);
    CHECK(got.RotationDegPerSec.x == 1.0f && got.RotationDegPerSec.y == 0.0f && got.RotationDegPerSec.z == 0.0f);

    // A field the preset does not mention keeps its current value rather than snapping to the
    // default, so a preset saved before a field existed stays usable.
    world.Registry.get<TransformControllerComponent>(dst).ScalePulseFrequencyHz = 7.0f;
    CHECK(SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"Transform Controller","fields":{"Rotation Deg/Sec":[0.0,0.0,1.0]}})"));
    CHECK(world.Registry.get<TransformControllerComponent>(dst).RotationDegPerSec.z == 1.0f);
    CHECK(world.Registry.get<TransformControllerComponent>(dst).ScalePulseFrequencyHz == 7.0f); // untouched

    // Rejections: malformed, unknown component, missing fields block, and a component that has
    // no generic serialisation. None of these may half-write anything.
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, "not json"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst,
              R"({"preset":1,"component":"NoSuchComponent","fields":{}})"));
    CHECK(!SceneSerializer::ApplyComponentPresetJson(world, assets, dst, R"({"preset":1,"component":"Transform Controller"})"));
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

    // The first-person view-model sub-pass rebuilds a projection from the world's, changing only
    // the FOV. It doesn't know the camera's near/far — it reads them back out of the matrix it
    // was handed, so that readback is the thing to pin down: get it wrong and the arms and the
    // scene they are composited over disagree about where the near plane sits.
    {
        const glm::mat4& worldProj = got;
        const float nearZ = std::fabs(worldProj[3][2] / (worldProj[2][2] - 1.0f));
        const float farZ  = std::fabs(worldProj[3][2] / (worldProj[2][2] + 1.0f));
        CHECK(std::fabs(nearZ - cam.NearPlane) < 1e-4f);
        // far recovers through a near-zero denominator (proj[2][2] + 1), so float rounding is
        // amplified ~5 orders of magnitude here — exact to a few hundredths is the ceiling.
        CHECK(std::fabs(farZ - cam.FarPlane) < 0.05f);

        // What the sub-pass then builds: same clip range, different cone.
        const glm::mat4 vm = MakePerspective(60.0f, 1.5f, nearZ, farZ);
        const glm::mat4 want = glm::perspective(glm::radians(60.0f), 1.5f, nearZ, farZ);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(std::fabs(vm[c][r] - want[c][r]) < 1e-3f);
        // 60 deg is narrower than the world's 75: a larger y scale, i.e. the held weapon is
        // framed tighter than the scene behind it, not stretched to match it.
        CHECK(vm[1][1] > worldProj[1][1]);
    }

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

// The player's acceleration: 0 = the move is the input (as before); otherwise an exponential approach
// that lands in the same place whatever the frame rate, and arrives rather than creeping.
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

void TestPlayerAcceleration() {
    const glm::vec3 run(0.0f, 0.0f, 4.5f);
    CHECK(PlayerApproachVelocity(glm::vec3(0.0f), run, 1.0f / 60.0f, 0.0f, 0.0f) == run);
    CHECK(PlayerApproachVelocity(run, glm::vec3(0.0f), 1.0f / 60.0f, 0.07f, 0.0f) == glm::vec3(0.0f)); // no decel time: stops dead
    auto after = [&](float hz, float seconds, glm::vec3 from, glm::vec3 to) {
        glm::vec3 v = from;
        for (int i = 0, n = (int)std::lround(seconds * hz); i < n; ++i) v = PlayerApproachVelocity(v, to, 1.0f / hz, 0.07f, 0.05f);
        return v;
    };
    // 0.07 s: 63% of the way; the same at 30, 144 and 300 fps.
    const float at30 = after(30.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z, at144 = after(144.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z,
                at300 = after(300.0f, 0.07f * 3.0f, glm::vec3(0.0f), run).z;
    CHECK(std::abs(at30 - at300) < 0.05f && std::abs(at144 - at300) < 0.05f && std::abs(at300 - 4.5f * (1.0f - std::exp(-3.0f))) < 0.05f);
    CHECK(after(144.0f, 0.5f, glm::vec3(0.0f), run) == run);             // arrives
    CHECK(after(144.0f, 0.4f, run, glm::vec3(0.0f)) == glm::vec3(0.0f)); // and stops
    // Slowing down uses the deceleration time: quicker here than speeding up.
    const float up = after(144.0f, 0.05f, glm::vec3(0.0f), run).z, down = 4.5f - after(144.0f, 0.05f, run, glm::vec3(0.0f)).z;
    CHECK(down > up);
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
// Bones and ragdoll parts map to the regions of a hit, which map to the zones the damage multipliers are keyed on.
void TestNpcHitRegions() {
    CHECK(RegionFromBone("head") == HitRegion::Head);
    CHECK(RegionFromBone("neck_01") == HitRegion::Torso);
    CHECK(RegionFromBone("spine_03") == HitRegion::Torso);
    CHECK(RegionFromBone("pelvis") == HitRegion::Torso);
    CHECK(RegionFromBone("clavicle_l") == HitRegion::Torso);
    CHECK(RegionFromBone("upperarm_l") == HitRegion::Arm);
    CHECK(RegionFromBone("lowerarm_r") == HitRegion::Arm);
    CHECK(RegionFromBone("hand_r") == HitRegion::Arm);
    CHECK(RegionFromBone("index_01_l") == HitRegion::Arm);
    CHECK(RegionFromBone("thigh_l") == HitRegion::Leg);
    CHECK(RegionFromBone("calf_r") == HitRegion::Leg);
    CHECK(RegionFromBone("foot_l") == HitRegion::Leg);
    CHECK(RegionFromBone("ball_r") == HitRegion::Leg);
    CHECK(RegionFromBone("nonsense") == HitRegion::Torso);
    CHECK(RegionFromBone(nullptr) == HitRegion::Torso);
    // Every part of the soldier's table lands where its bone does.
    for (int i = 0; i < NpcRagdoll::kParts; ++i) CHECK(RegionFromPart(i) == RegionFromBone(NpcPartDefOf(i).Bone));
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

// IK::SetGlobals sets several bones in one pass and lands exactly where setting them one by one does.
void TestIKSetGlobalsBatch() {
    IK::Pose pose(6);
    const std::vector<int> parents = {-1, 0, 1, 1, 3, 4};
    pose[0].S = glm::vec3(0.8f);
    for (int i = 1; i < 6; ++i) {
        pose[(size_t)i].T = glm::vec3(0.1f * (float)i, 0.5f, 0.05f);
        pose[(size_t)i].R = glm::angleAxis(0.2f * (float)i, glm::normalize(glm::vec3(1, (float)i, 2)));
    }
    std::vector<glm::mat4> g0;
    IK::ComputeGlobals(pose, parents, g0);
    const glm::quat turn = glm::angleAxis(0.7f, glm::vec3(0, 1, 0));
    const std::vector<IK::GlobalTarget> targets = {
        {1, IK::Position(g0[1]) + glm::vec3(0.1f, 0.0f, 0.0f), turn * IK::Rotation(g0[1])},
        {3, IK::Position(g0[3]) + glm::vec3(0.0f, 0.2f, 0.0f), turn * IK::Rotation(g0[3])},
        {5, IK::Position(g0[5]) + glm::vec3(-0.1f, 0.1f, 0.0f), IK::Rotation(g0[5])}};
    IK::Pose a = pose, b = pose;
    std::vector<glm::mat4> ga = g0, gb = g0;
    IK::SetGlobals(a, parents, ga, targets);
    for (const auto& t : targets) IK::SetGlobal(b, parents, gb, t.Node, t.Pos, t.Rot);
    for (size_t i = 0; i < pose.size(); ++i) {
        CHECK(glm::length(a[i].T - b[i].T) < 1e-5f);
        CHECK(std::fabs(glm::dot(a[i].R, b[i].R)) > 1.0f - 1e-5f);
        CHECK(glm::length(IK::Position(ga[i]) - IK::Position(gb[i])) < 1e-5f);
    }
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

// --- #202 / #122: a scene survives save -> load -> save unchanged ---------------------------
// The serializer is the one place where a silent mistake costs authored work: a field dropped on
// load, an unstable entity order, a component written but not read. None of that shows up as a
// The physical sky (World::SkySource::Atmosphere): the clock's sun/moon/star placement, the
// CPU transmittance the directional light is filtered by, the day/night light hand-off, the
// presets, and the scene file round trip. All CPU-side - the GPU passes are covered by the
// smoke_sky_* scenes.
void TestPhysicalSky() {
    using namespace TimeOfDay;
    auto near = [](float a, float b, float eps) { return std::abs(a - b) <= eps; };

    // Equinox at the equator: the noon sun is overhead, 06:00 on the eastern horizon (+X),
    // 18:00 on the western (-X).
    const glm::vec3 noon = SunDirection(12.0f, 80.0f, 0.0f, 0.0f);
    CHECK(noon.y > 0.99f);
    const glm::vec3 dawn = SunDirection(6.0f, 80.0f, 0.0f, 0.0f);
    CHECK(std::abs(dawn.y) < 0.03f && dawn.x > 0.95f);
    CHECK(SunDirection(18.0f, 80.0f, 0.0f, 0.0f).x < -0.95f);
    // 38 N at the June solstice: noon elevation 90 - 38 + 23.44 = 75.4 deg, due south (+Z).
    const glm::vec3 june = SunDirection(12.0f, 172.0f, 38.0f, 0.0f);
    CHECK(near(glm::degrees(Elevation(june)), 75.44f, 0.6f));
    CHECK(june.z > 0.0f && std::abs(june.x) < 1e-3f);
    // Turning the compass rotates the sky about +Y and nothing else.
    const glm::vec3 turned = SunDirection(12.0f, 172.0f, 38.0f, 90.0f);
    CHECK(near(glm::length(turned), 1.0f, 1e-4f) && near(turned.y, june.y, 1e-5f));
    CHECK(std::abs(turned.z) < 1e-3f);
    // Day length: 12 h at the equinox anywhere, polar day and night past the circles.
    CHECK(near(SunsetHour(80.0f, 45.0f), 18.0f, 0.1f));
    CHECK(SunsetHour(172.0f, 80.0f) == 24.0f);
    CHECK(SunsetHour(355.0f, 80.0f) == 12.0f);
    CHECK(SunsetHour(172.0f, 38.0f) > 19.0f && SunsetHour(172.0f, 38.0f) < 19.6f);

    // The moon: new = dark, full = lit, and a full moon sits opposite the sun.
    CHECK(MoonIllumination(0.0f) < 1e-4f && MoonIllumination(0.5f) > 0.9999f);
    CHECK(near(MoonIllumination(0.25f), 0.5f, 1e-4f));
    const glm::vec3 fullMoon = MoonDirection(0.0f, 80.0f, 0.0f, 0.0f, 0.5f);
    CHECK(glm::dot(fullMoon, SunDirection(0.0f, 80.0f, 0.0f, 0.0f)) < -0.9f);
    CHECK(MoonPhase(80.0f, 0.0f, 0.3f) >= 0.0f && MoonPhase(80.0f, 0.0f, 0.3f) < 1.0f);

    // The star field turns rigidly about the celestial pole, which stays put.
    const glm::mat3 stars = StarRotation(3.0f, 100.0f, 38.0f, 0.0f);
    CHECK(near(glm::determinant(stars), 1.0f, 1e-4f));
    const glm::vec3 pole(0.0f, std::sin(glm::radians(38.0f)), -std::cos(glm::radians(38.0f)));
    CHECK(glm::length(stars * pole - pole) < 1e-4f);

    // Transmittance: blue is scattered most, the horizon passes far less than the zenith, the
    // planet blocks everything below it, and thinner air higher up lets more through.
    SkySettings s;
    const glm::vec3 zenith = SkyAtmosphere::Transmittance(s, 0.0f, glm::vec3(0, 1, 0));
    CHECK(zenith.b < zenith.g && zenith.g < zenith.r);
    CHECK(zenith.r > 0.9f && zenith.b > 0.7f && zenith.b < 0.82f);
    const glm::vec3 horizon = SkyAtmosphere::Transmittance(s, 0.0f, glm::normalize(glm::vec3(0.0f, 0.02f, 1.0f)));
    CHECK(horizon.r < zenith.r * 0.7f && horizon.b < horizon.r * 0.5f);
    CHECK(glm::length(SkyAtmosphere::Transmittance(s, 0.0f, glm::normalize(glm::vec3(0.0f, -0.2f, 1.0f)))) == 0.0f);
    CHECK(SkyAtmosphere::Transmittance(s, 5.0f, glm::vec3(0, 1, 0)).b > zenith.b);
    s.RayleighScale = 0.0f; s.MieScale = 0.0f; s.OzoneScale = 0.0f;
    CHECK(glm::length(SkyAtmosphere::Transmittance(s, 0.0f, glm::vec3(0, 1, 0)) - glm::vec3(1.0f)) < 1e-5f);

    // Resolved lighting: a warm, dimmed sun by day; the moon (brightened for the night) after.
    SkyAtmosphere sky; // no GL until PrepareFrame
    SkySettings clock;
    clock.TimeOfDayHours = 12.0f;
    SkyLighting day = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(!day.LightIsMoon && day.LightDir.y > 0.9f && day.NightBoost == 1.0f);
    CHECK(day.LightRadiance.r > day.LightRadiance.b && day.LightRadiance.r < 5.0f && day.LightRadiance.b > 3.0f);
    clock.TimeOfDayHours = 0.5f;
    const float phase = MoonPhase(clock.DayOfYear, clock.TimeOfDayHours, 0.0f);
    clock.MoonPhaseOffset = 0.5f - phase - std::floor(0.5f - phase); // full moon
    SkyLighting night = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(night.LightIsMoon && night.LightDir.y > 0.2f && night.MoonLitFraction > 0.99f);
    CHECK(night.NightBoost > 16.0f);
    CHECK(glm::length(night.LightRadiance) < glm::length(day.LightRadiance));
    CHECK(night.SunDir.y < 0.0f);
    clock.MoonEnabled = false;
    night = sky.ResolveLighting(clock, glm::vec3(0, -1, 0), glm::vec3(5.0f), 0.53f, glm::vec3(0, 2, 0));
    CHECK(!night.LightIsMoon && glm::length(night.LightRadiance) == 0.0f);
    // Clock off: the directional light's own aim is the sun.
    SkySettings authored;
    authored.TimeOfDayEnabled = false;
    const glm::vec3 aim = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f));
    const SkyLighting fixed = sky.ResolveLighting(authored, aim, glm::vec3(5.0f), 0.53f, glm::vec3(0.0f));
    CHECK(glm::dot(fixed.SunDir, -aim) > 0.9999f && glm::dot(fixed.LightDir, -aim) > 0.9999f);

    // Cloud wind at a high frame rate: each frame's step is tiny, and the texture offsets must
    // still advance steadily. (They once wrapped at a period that wasn't a whole number of tiles
    // and, with the step lost to rounding, flipped between two layouts every frame.)
    {
        SkyAtmosphere windSky; // Tick touches no GL
        SkySettings w;
        w.CloudWindSpeed = 8.0f;
        const float tile = 45.0f; // the weather map's period at CloudScale 1
        glm::vec2 prev = windSky.Clouds().WeatherOffset();
        float travelled = 0.0f, worstStep = 0.0f;
        for (int i = 0; i < 2400; ++i) { // 10 s at 240 fps
            windSky.Tick(w, 1.0f / 240.0f, false);
            const glm::vec2 cur = windSky.Clouds().WeatherOffset();
            glm::vec2 step = cur - prev;
            step -= glm::round(step / tile) * tile; // wrapping by a whole tile is invisible
            travelled += glm::length(step);
            worstStep = std::max(worstStep, glm::length(step));
            prev = cur;
        }
        CHECK(worstStep < 1e-3f);                  // no jumps
        CHECK(near(travelled, 0.08f, 0.004f));     // 8 m/s for 10 s
    }

    // Presets change the look but keep the location and quality; the timed ones land where named.
    SkySettings p;
    p.LatitudeDegrees = -33.0f;
    p.CloudQuality = 3;
    ApplySkyPreset(p, SkyPreset::Overcast);
    CHECK(p.CloudCoverage > 0.9f && p.LatitudeDegrees == -33.0f && p.CloudQuality == 3);
    ApplySkyPreset(p, SkyPreset::Sunset);
    const float sunsetElev = glm::degrees(Elevation(SunDirection(p.TimeOfDayHours, p.DayOfYear, p.LatitudeDegrees, 0.0f)));
    CHECK(sunsetElev > -0.5f && sunsetElev < 4.0f);
    ApplySkyPreset(p, SkyPreset::Night);
    CHECK(MoonIllumination(MoonPhase(p.DayOfYear, p.TimeOfDayHours, p.MoonPhaseOffset)) > 0.99f);
    CHECK(SunDirection(p.TimeOfDayHours, p.DayOfYear, p.LatitudeDegrees, 0.0f).y < 0.0f);
    SkySettings untouched = p;
    ApplySkyPreset(untouched, SkyPreset::Custom);
    CHECK(untouched == p);
    for (int i = 0; i < (int)SkyPreset::Count; ++i) CHECK(std::string(SkyPresetName((SkyPreset)i)) != "?");

    // Scene files: every field round-trips, old scenes load the defaults, bad values clamp.
    World w;
    w.SkySourceMode = World::SkySource::Atmosphere;
    w.Sky.CloudCoverage = 0.77f;
    w.Sky.RayleighTint = glm::vec3(0.5f, 1.0f, 0.625f);
    w.Sky.CloudQuality = 2;
    w.Sky.MoonEnabled = false;
    w.Sky.NightBrightness = 2.5f;
    w.Sky.TimeOfDayHours = 17.25f;
    AssetLibrary assets;
    const std::string saved = SceneSerializer::SaveToString(w, assets);
    World r;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(r, assets2, saved));
    CHECK(r.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(r.Sky == w.Sky);
    World old;
    AssetLibrary assets3;
    CHECK(SceneSerializer::LoadFromString(old, assets3, R"({"formatVersion":4,"skySource":2})"));
    CHECK(old.SkySourceMode == World::SkySource::Atmosphere && old.Sky == SkySettings{});
    World clamped;
    AssetLibrary assets4;
    CHECK(SceneSerializer::LoadFromString(clamped, assets4,
        R"({"formatVersion":4,"skySource":9,"sky":{"cloudCoverage":7,"cloudQuality":9,"latitude":-400,"clouds":"yes"}})"));
    CHECK(clamped.SkySourceMode == World::SkySource::Atmosphere);
    CHECK(clamped.Sky.CloudCoverage == 1.0f && clamped.Sky.CloudQuality == 3 && clamped.Sky.LatitudeDegrees == -90.0f);
    CHECK(clamped.Sky.CloudsEnabled); // a non-boolean is ignored, keeping the default
}

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

    TransformControllerComponent ctl; ctl.RotationDegPerSec = glm::vec3(0.0f, 0.0f, 1.0f); ctl.ScalePulseFrequencyHz = 42.5f;
    a.Registry.emplace<TransformControllerComponent>(parent, ctl);

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
            CHECK(b.Registry.all_of<TransformControllerComponent>(e));
            const auto& tc = b.Registry.get<TransformControllerComponent>(e);
            CHECK(tc.ScalePulseFrequencyHz == 42.5f && tc.RotationDegPerSec.z == 1.0f);
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
struct TestEntry { UndoDelta::Patch Delta; int Tag = 0; };

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

// The Animator linter: a sound controller is clean; each structural mistake is named and located.
void TestAnimatorLint() {
    using AC = AnimatorController;
    auto has = [](const std::vector<AnimatorLint::Issue>& v, const char* t) {
        for (const auto& i : v) if (i.Message.find(t) != std::string::npos) return true;
        return false;
    };
    AC c;
    c.Parameters = {{"Speed", AC::ParamType::Float, 0}, {"Go", AC::ParamType::Trigger, 0}, {"On", AC::ParamType::Bool, 0}};
    AC::State idle, run, dead;
    idle.Name = "Idle"; idle.Motions = {AC::Motion{"idle.fbx#i"}};
    run.Name = "Run"; run.Motions = {AC::Motion{"run.fbx#r"}};
    dead.Name = "Orphan"; dead.Motions = {AC::Motion{"o.fbx#o"}};
    c.Layers[0].States = {idle, run, dead};
    c.Layers[0].DefaultState = "Idle";
    AC::Transition t;
    t.From = "Idle"; t.To = "Run"; t.Conditions = {{"Go", AC::Op::If, 0}};
    AC::Transition back = t;
    back.From = "Run"; back.To = "Idle"; back.Conditions = {{"Speed", AC::Op::Less, 0.1f}};
    c.Layers[0].Transitions = {t, back};
    auto r = AnimatorLint::Check(c);
    CHECK(has(r, "'Orphan' can never be reached") && !has(r, "'Run' can never"));
    // a state's speed parameter and a condition on a missing parameter, a numeric test on a Bool
    c.Layers[0].States[0].SpeedParam = "Nope";
    c.Layers[0].Transitions[1].Conditions = {{"On", AC::Op::Greater, 0.5f}};
    c.Layers[0].Transitions[0].Conditions = {{"Gone", AC::Op::If, 0}};
    r = AnimatorLint::Check(c);
    CHECK(has(r, "speed parameter 'Nope'") && has(r, "tests 'Gone'") && has(r, "compares 'On'"));
    CHECK(r.front().Severity == AnimatorLint::Level::Error);
    // a transition to a deleted state, and two parameters with one name
    c.Layers[0].Transitions[0].To = "Deleted";
    c.Parameters.push_back({"Speed", AC::ParamType::Float, 0});
    r = AnimatorLint::Check(c);
    CHECK(has(r, "doesn't exist") && has(r, "Two parameters are named 'Speed'"));
    bool located = false;
    for (const auto& i : r) if (i.Message.find("points at a state") != std::string::npos && i.Transition == 0) located = true;
    CHECK(located);
    // a state that doesn't loop and nothing leaves; a no-condition no-exit-time transition
    AC d;
    AC::State a, b;
    a.Name = "A"; a.Motions = {AC::Motion{"a#a"}};
    b.Name = "B"; b.Loop = false; b.Motions = {AC::Motion{"b#b"}};
    d.Layers[0].States = {a, b};
    AC::Transition ab; ab.From = "A"; ab.To = "B";
    d.Layers[0].Transitions = {ab};
    r = AnimatorLint::Check(d);
    CHECK(has(r, "'B' doesn't loop") && has(r, "never fires"));
}

// The new-weapon wizard matches animation files to the graph's states by the tail of their names.
void TestFirstPersonWeaponWizard() {
    const std::vector<std::string> files = {
        "a/AKS-74U_A_FP_Idle.fbx", "a/AKS-74U_A_FP_IdleToSprint.fbx", "a/AKS-74U_A_FP_Sprint.fbx",
        "a/AKS-74U_A_FP_Mag_Check.fbx", "a/AKS-74U_A_FP_Tac_Reload.fbx", "a/AKS-74U_A_FP_Empty_Reload.fbx",
        "a/AKS-74U_A_FP_Fire.fbx", "a/AKS-74U_A_FP_Holster.fbx"};
    CHECK(FPWizard::PickClip("Idle", files) == "a/AKS-74U_A_FP_Idle.fbx");
    CHECK(FPWizard::PickClip("Sprint", files) == "a/AKS-74U_A_FP_Sprint.fbx");        // not IdleToSprint
    CHECK(FPWizard::PickClip("IdleToSprint", files) == "a/AKS-74U_A_FP_IdleToSprint.fbx");
    CHECK(FPWizard::PickClip("MagCheck", files) == "a/AKS-74U_A_FP_Mag_Check.fbx");   // two words
    CHECK(FPWizard::PickClip("TacReload", files) == "a/AKS-74U_A_FP_Tac_Reload.fbx");
    CHECK(FPWizard::PickClip("EmptyReload", files) == "a/AKS-74U_A_FP_Empty_Reload.fbx");
    CHECK(FPWizard::PickClip("Melee", files).empty());
    CHECK(FPWizard::PickClip("Idle", {"a/AM_Stand_Idle_01.fbx", "a/AM_Stand_Idle_Turn_L090.fbx"}) == "a/AM_Stand_Idle_01.fbx"); // numbered take
    CHECK(FPWizard::PickClip("Nonsense", files).empty());
    // "reload" alone is the tactical reload when there is no better name.
    CHECK(FPWizard::PickClip("TacReload", {"x/Reload.fbx"}) == "x/Reload.fbx");
    const auto set = FPWizard::Build("arms.fbx", "gun.fbx", {{"Idle", "i.fbx", ""}, {"Fire", "f.fbx", "wf.fbx"}, {"Melee", "", ""}});
    CHECK(set.Clips.size() == 2 && set.Clips[0].Loop && !set.Clips[1].Loop && set.Find("Fire")->WeaponClip == "wf.fbx");
    CHECK(BuildFirstPersonController(set).Layers[0].FindState("Fire") >= 0);
}

// The standard body locomotion graph: the builder gives the tuned reference (14 states, 21 parameters,
// 63 transitions), passes the lint and the body contract, and its clips are filled by role.
void TestFirstPersonBodyController() {
    const std::vector<std::string> roles = FPBody::LocomotionRoles();
    CHECK(roles.size() > 40 && std::find(roles.begin(), roles.end(), std::string("Loco_Walk_Fwd")) != roles.end());
    AnimatorController c = FPBody::BuildLocomotionController([](const std::string& r) { return "clips/AM_" + r + ".fbx"; });
    CHECK(c.Layers[0].States.size() == 14 && c.Parameters.size() == 21 && c.Layers[0].Transitions.size() == 63);
    // The gait plays at PlayRate (the body's, so the feet keep up with a player faster than the clips).
    CHECK(c.Layers[0].States[c.Layers[0].FindState("Locomotion")].SpeedParam == "PlayRate" &&
          c.Layers[0].States[c.Layers[0].FindState("CrouchLoco")].SpeedParam.empty());
    CHECK(c.Layers[0].DefaultState == "Locomotion" && c.Layers[0].FindState("CrouchStop") >= 0);
    // The tuned numbers survived: Stop leaves Locomotion at 0.36, the jog forward child sits at 3.264 m/s.
    bool stop = false;
    for (const auto& t : c.Layers[0].Transitions)
        if (t.From == "Locomotion" && t.To == "Stop") stop = std::abs(t.Offset - 0.36f) < 1e-5f;
    CHECK(stop);
    const auto& loco = c.Layers[0].States[c.Layers[0].FindState("Locomotion")].Motions[0];
    CHECK(loco.Is2D() && loco.Children.size() == 18 && loco.Children[9].Clip == "clips/AM_Loco_Jog_Fwd.fbx" &&
          std::abs(loco.Children[9].ThresholdY - 3.264f) < 1e-4f);
    // Round trip through the file format, then the checks: nothing to report.
    AnimatorController back;
    CHECK(AnimatorController::FromJsonString(c.ToJsonString(), back));
    CHECK(back.Layers[0].Transitions.size() == 63);
    int problems = 0;
    for (const auto& i : AnimatorLint::Check(c)) problems += i.Severity != AnimatorLint::Level::Info;
    CHECK(problems == 0);
    FirstPersonBodyComponent all;
    all.TurnThreshold = 55.0f; all.StartStopClips = true; all.CrouchHeight = 1.2f; all.FootIK = true; all.WeaponArms = true;
    FPBody::ValidationInput in;
    in.Config = &all;
    in.HasPieces = in.HasDriverPiece = in.ControllerSet = true;
    in.Controller = &c;
    for (const auto& k : FPBody::Validate(in)) CHECK(k.Level != FPBody::Severity::Warning && k.Level != FPBody::Severity::Error);
    // Role matching: "Loco_Walk_Fwd" is that file, not the Fwd_Left one.
    CHECK(FPBody::PickLocomotionClip("Loco_Walk_Fwd", {"a/AM_Loco_Walk_Fwd_Left.fbx", "a/AM_Loco_Walk_Fwd.fbx"}) == "a/AM_Loco_Walk_Fwd.fbx");
    CHECK(FPBody::PickLocomotionClip("Loco_Walk_Fwd", {"a/AM_Loco_Walk_Fwd_Left.fbx"}).empty());
}

// A foot near the contact threshold chatters: the debounce fills short gaps and drops short blips (cyclic).
void TestClipContactDebounce() {
    auto run = [](const char* bits, int minRun) {
        std::vector<char> v;
        for (const char* c = bits; *c; ++c) v.push_back(*c == '1');
        ClipAnalysis::Debounce(v, minRun);
        std::string out;
        for (char b : v) out += b ? '1' : '0';
        return out;
    };
    CHECK(run("0011010111000000", 2) == "0011111111000000");   // the 0 gaps inside the contact are filled
    CHECK(run("0000100000111000", 2) == "0000000000111000");   // a one-sample blip is dropped
    CHECK(run("1110000000000111", 2) == "1110000000000111");   // a run wrapping the ends stays one run
    CHECK(run("1111", 2) == "1111" && run("0000", 2) == "0000");
}

// A Bone Map renames the standard bones to a rig's own: parsed leniently, and the validator looks for the mapped names.
void TestFirstPersonBodyBoneMap() {
    const auto m = FPBody::ParseBoneMap(" pelvis = Hips ,foot_l=LeftFoot\nfoot_r = RightFoot, junk, =x, y=");
    CHECK(m.size() == 3 && m.at("pelvis") == "Hips" && m.at("foot_l") == "LeftFoot" && m.at("foot_r") == "RightFoot");
    CHECK(FPBody::MappedBone(m, "pelvis") == "Hips" && FPBody::MappedBone(m, "spine_01") == "spine_01");

    FirstPersonBodyComponent cfg;
    cfg.FootIK = true;
    FPBody::ValidationInput in;
    in.Config = &cfg;
    in.HasPieces = in.HasDriverPiece = in.ControllerSet = true;
    const std::set<std::string> rigBones = {"head", "Hips", "thigh_l", "calf_l", "LeftFoot", "thigh_r", "calf_r", "RightFoot"};
    in.HasBone = [&](const std::string& b) { return rigBones.count(b) > 0; };
    auto hasBoneWarning = [](const std::vector<FPBody::Check>& r) {
        for (const auto& c : r)
            if (c.Message.find("no bone") != std::string::npos) return true;
        return false;
    };
    CHECK(hasBoneWarning(FPBody::Validate(in)));                 // "pelvis", "foot_l" ... are not on this rig
    cfg.BoneMap = "pelvis = Hips, foot_l = LeftFoot, foot_r = RightFoot";
    CHECK(!hasBoneWarning(FPBody::Validate(in)));                // mapped: every foot IK bone is found
}

// A clip trim: only the range plays, and it is the clip's length; time 0 is the range's start.
void TestClipTrim() {
    AnimationClip clip;
    clip.DurationTicks = 100.0f;
    clip.TicksPerSecond = 50.0f;                       // a 2 s take
    CHECK(std::abs(clip.LengthSeconds() - 2.0f) < 1e-5f);
    CHECK(std::abs(WrappedClipTicks(clip, 0.5f, AnimationWrapMode::Loop) - 25.0f) < 1e-4f);
    clip.StartTicks = 25.0f;                           // 0.5 s .. the end
    CHECK(std::abs(clip.LengthSeconds() - 1.5f) < 1e-5f);
    CHECK(std::abs(WrappedClipTicks(clip, 0.0f, AnimationWrapMode::Loop) - 25.0f) < 1e-4f);
    CHECK(std::abs(WrappedClipTicks(clip, 1.0f, AnimationWrapMode::Loop) - 75.0f) < 1e-4f);
    CHECK(std::abs(WrappedClipTicks(clip, 1.6f, AnimationWrapMode::Loop) - 30.0f) < 1e-4f);    // wraps inside the range
    CHECK(std::abs(WrappedClipTicks(clip, 9.0f, AnimationWrapMode::ClampForever) - 100.0f) < 1e-4f);
    clip.EndTicks = 75.0f;                             // 0.5 s .. 1.5 s
    CHECK(std::abs(clip.LengthSeconds() - 1.0f) < 1e-5f);
    CHECK(std::abs(WrappedClipTicks(clip, 9.0f, AnimationWrapMode::ClampForever) - 75.0f) < 1e-4f);
    clip.EndTicks = 500.0f;                            // past the take: clamped to it
    CHECK(std::abs(clip.LengthSeconds() - 1.5f) < 1e-5f);
}

// A weapon setup is checked against the driver's contract: a full controller is clean, each missing
// piece (event, tag, parameter, bone) is named.
void TestFirstPersonWeaponValidate() {
    using namespace FirstPersonAnimatorContract;
    using PT = AnimatorController::ParamType;
    FirstPersonAnimationSet set;
    set.Controller = "c.controller";
    AnimatorController ctrl;
    for (const char* n : {kSpeed, kWalkRate, kSprintRate}) ctrl.Parameters.push_back({n, PT::Float, 0.0f});
    for (const char* n : {kSprint, kAim, kEquipped}) ctrl.Parameters.push_back({n, PT::Bool, 0.0f});
    ctrl.Parameters.push_back({kAmmo, PT::Int, 0.0f});
    for (const char* n : {kFire, kReload, kMagCheck, kInspect, kMelee, kFidget}) ctrl.Parameters.push_back({n, PT::Trigger, 0.0f});
    AnimatorController::State idle, fire, reload, aim, hidden;
    idle.Name = "Idle"; idle.Tags = {kTagIdle, kTagReady};
    fire.Name = "Fire"; fire.Events.push_back({kEventShot, 0.1f});
    reload.Name = "Reload"; reload.Tags = {kTagReload, kTagBusy}; reload.Events.push_back({kEventRefill, 0.7f});
    aim.Name = "Aim"; aim.Tags = {kTagAds};
    hidden.Name = "Holstered"; hidden.Tags = {kTagHidden};
    ctrl.Layers[0].States = {idle, fire, reload, aim, hidden};
    set.Ads.ReferenceState = "Aim";
    FirstPersonWeaponCheckInput in;
    in.Set = &set;
    in.Controller = &ctrl;
    in.HasArmsBone = [](const std::string&) { return true; };
    in.HasWeaponBone = [](const std::string&) { return true; };
    auto count = [](const std::vector<FPBody::Check>& c, FPBody::Severity l) {
        int n = 0;
        for (const auto& x : c) n += x.Level == l;
        return n;
    };
    auto has = [](const std::vector<FPBody::Check>& c, const char* t) {
        for (const auto& x : c) if (x.Message.find(t) != std::string::npos) return true;
        return false;
    };
    std::vector<FPBody::Check> r = FirstPersonWeaponValidate(in);
    CHECK(count(r, FPBody::Severity::Warning) == 0 && count(r, FPBody::Severity::Error) == 0);

    AnimatorController broken = ctrl;
    broken.Layers[0].States[1].Events.clear();                     // no Shot
    broken.Layers[0].States[4].Tags.clear();                       // no Hidden
    broken.Parameters.erase(broken.Parameters.begin());            // no Speed
    in.Controller = &broken;
    in.HasArmsBone = [](const std::string& b) { return b != "ik_hand_gun"; };
    r = FirstPersonWeaponValidate(in);
    CHECK(has(r, "'Shot' event") && has(r, "'Hidden'") && has(r, "'Speed'") && has(r, "'ik_hand_gun'"));
    CHECK(r.front().Level == FPBody::Severity::Warning);

    FirstPersonWeaponCheckInput none;
    none.Set = &set;
    set.Controller.clear();
    r = FirstPersonWeaponValidate(none);
    CHECK(!r.empty() && r.front().Level == FPBody::Severity::Error);
}

// The body's tuning fields: every float has a tooltip, a range that holds its default, and the
// defaults are the values the body was tuned with (so a scene saved before they existed plays the same).
void TestFirstPersonBodyTuning() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    FirstPersonBodyComponent body;
    FirstPersonControllerComponent ctrl;
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    CHECK(near(body.EyeSlack, 0.035f) && near(body.ReachSlack, 0.04f) && near(body.ShrugStart, 0.98f) && near(body.ShrugMax, 0.12f));
    CHECK(near(body.TurnLagFloor, 90.0f) && near(body.TurnLagMargin, 5.0f) && near(body.TurnEndAngle, 8.0f) && near(body.TurnMinTime, 0.3f) &&
          near(body.TurnTimeout, 4.0f) && near(body.TurnMoveEase, 0.08f));
    CHECK(near(body.StartIdleTime, 0.25f) && near(body.StartMaxMove, 0.6f) && near(body.StopMinRunTime, 0.6f) && near(body.StopMinRunTimeCrouched, 0.7f) &&
          near(body.StopMinSpeed, 1.2f) && near(body.StopMinSpeedCrouched, 0.6f) && near(body.StopDebounce, 0.05f) && near(body.StopRunForward, 0.7f));
    CHECK(near(body.FootLockDrift, 0.12f) && near(body.FootPlantedHeight, 0.05f) && near(body.FootRayUp, 0.5f) && near(body.FootRayLength, 1.0f) &&
          near(body.FootMaxRaise, 0.25f) && near(body.PelvisMaxRaise, 0.0f) && near(body.FootTiltMax, 25.0f) && near(body.StairPopRise, 0.03f) &&
          near(body.StairPopRate, 2.5f) && near(body.StairEase, 0.09f) && near(body.AirborneDelay, 0.15f));
    CHECK(near(ctrl.JumpBufferTime, 0.12f) && near(ctrl.CoyoteTime, 0.10f));
    for (const RegisteredComponent& rc : ComponentRegistry::All()) {
        const bool isBody = std::string(rc.Meta.Name) == "First Person Body", isCtrl = std::string(rc.Meta.Name) == "First Person Controller";
        if (!isBody && !isCtrl) continue;
        void* inst = isBody ? (void*)&body : (void*)&ctrl;
        for (const ReflectField& f : rc.Meta.Fields) {
            CHECK(f.Tooltip && *f.Tooltip);
            if (f.Type != ReflectFieldType::Float || f.Max <= f.Min) continue;
            const float v = *static_cast<float*>(f.Address(inst));
            CHECK(v >= f.Min - 1e-6f && v <= f.Max + 1e-6f);
        }
    }
}

// --- Root motion: extraction, in-place poses, loops, blending (RootMotion.h) --------------------
// #405 - 2D blend trees and the first-person body's frame maths.
void TestBlendTree2D() {
    auto near = [](float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; };
    using AC = AnimatorController;
    // Idle in the middle, walks at their velocities (x right, y forward), a jog further out.
    std::vector<AC::BlendChild> ch = {
        {"idle", 0.0f, 1.0f, 0.0f},  {"fwd", 0.0f, 1.0f, 1.5f},   {"bwd", 0.0f, 1.0f, -1.2f},
        {"left", -1.3f, 1.0f, 0.0f}, {"right", 1.6f, 1.0f, 0.0f}, {"jog", 0.0f, 1.0f, 3.3f},
    };
    auto sum = [](const std::vector<float>& w) { float t = 0.0f; for (float v : w) t += v; return t; };
    // On a child: all of it.
    for (size_t i = 0; i < ch.size(); ++i) {
        const auto w = AnimatorBlendWeights2D(ch, ch[i].Threshold, ch[i].ThresholdY);
        CHECK(near(w[i], 1.0f) && near(sum(w), 1.0f));
    }
    // Half way to the walk forward: idle and walk share it, nothing else.
    auto w = AnimatorBlendWeights2D(ch, 0.0f, 0.75f);
    CHECK(near(w[0], 0.5f) && near(w[1], 0.5f) && near(w[2] + w[3] + w[4] + w[5], 0.0f));
    // Diagonal forward-left: forward and left both, no right or backward.
    w = AnimatorBlendWeights2D(ch, -0.6f, 0.7f);
    CHECK(w[1] > 0.1f && w[3] > 0.1f && near(w[4], 0.0f) && near(w[2], 0.0f) && near(sum(w), 1.0f));
    // Beyond the jog: the jog.
    w = AnimatorBlendWeights2D(ch, 0.0f, 6.0f);
    CHECK(near(w[5], 1.0f));
    // Every weight in [0, 1], summing to 1, anywhere.
    bool ok = true;
    for (float x = -4.0f; x <= 4.0f; x += 0.37f)
        for (float y = -4.0f; y <= 4.0f; y += 0.41f) {
            const auto ww = AnimatorBlendWeights2D(ch, x, y);
            for (float v : ww) ok = ok && v >= -1e-5f && v <= 1.0f + 1e-5f;
            ok = ok && near(sum(ww), 1.0f);
        }
    CHECK(ok);

    // AnimatorMotionWeights picks 1D or 2D from the motion; the JSON keeps the Y side.
    AC::Motion m;
    m.BlendParam = "MoveX";
    m.BlendParamY = "MoveY";
    m.Children = ch;
    std::vector<AnimatorParam> params = {{"MoveX", 0, 1.6f}, {"MoveY", 0, 0.0f}};
    CHECK(near(AnimatorMotionWeights(m, params)[4], 1.0f));
    AC ctrl;
    ctrl.Parameters = {{"MoveX"}, {"MoveY"}};
    ctrl.Tracks = {"main"};
    AC::Layer L;
    L.Name = "Base Layer";
    AC::State st;
    st.Name = "Locomotion";
    st.Motions = {m};
    L.States = {st};
    L.DefaultState = "Locomotion";
    ctrl.Layers = {L};
    AC back;
    CHECK(AC::FromJsonString(ctrl.ToJsonString(), back));
    const AC::Motion& bm = back.Layers[0].States[0].Motions[0];
    CHECK(bm.Is2D() && bm.BlendParamY == "MoveY" && bm.Children.size() == ch.size() &&
          near(bm.Children[2].ThresholdY, -1.2f) && near(bm.Children[4].Threshold, 1.6f));
    // A 1D tree stays 1D (no Y written or read back).
    ctrl.Layers[0].States[0].Motions[0].BlendParamY.clear();
    CHECK(AC::FromJsonString(ctrl.ToJsonString(), back) && !back.Layers[0].States[0].Motions[0].Is2D());

    // The body's frame. A model facing +Z turned to look down -Z (a camera at yaw -90).
    const float yaw = FirstPersonBodyYaw(glm::vec3(0.0f, -0.3f, -1.0f));
    CHECK(near(std::abs(yaw), 3.14159265f));
    CHECK(near(FirstPersonBodyYaw(glm::vec3(0.0f, 1.0f, 0.0f), 0.25f), 0.25f)); // straight up: keep the last
    // Facing -Z with +Y up, the right hand points to +X (the camera's own Right()).
    glm::vec2 lm = FirstPersonBodyLocalMove(glm::vec3(0.0f, 5.0f, -2.0f), yaw);
    CHECK(near(lm.x, 0.0f) && near(lm.y, 2.0f));
    lm = FirstPersonBodyLocalMove(glm::vec3(1.5f, 0.0f, 0.0f), yaw);
    CHECK(near(lm.x, 1.5f) && near(lm.y, 0.0f));
    // Facing +Z (yaw 0): right is -X, as the mannequin's own frame.
    lm = FirstPersonBodyLocalMove(glm::vec3(-1.0f, 0.0f, 1.0f), 0.0f);
    CHECK(near(lm.x, 1.0f) && near(lm.y, 1.0f));
    // A player faster than the clips: its run is the jog's speed, its sprint the run clip's, linear between;
    // unset (the clips' own speeds) it is the speed itself.
    CHECK(near(FirstPersonBodyClipSpeed(4.5f, 4.5f, 6.8f, 3.264f, 4.736f), 3.264f));
    CHECK(near(FirstPersonBodyClipSpeed(6.8f, 4.5f, 6.8f, 3.264f, 4.736f), 4.736f));
    CHECK(near(FirstPersonBodyClipSpeed(2.25f, 4.5f, 6.8f, 3.264f, 4.736f), 1.632f));
    CHECK(near(FirstPersonBodyClipSpeed(4.0f, 3.26f, 4.72f, 3.26f, 4.72f), 4.0f) && near(FirstPersonBodyClipSpeed(1.0f, 3.26f, 4.72f, 3.26f, 4.72f), 1.0f));
    // ... and its gait plays that much faster, within Max Play Rate, none of it under root motion or standing.
    CHECK(near(FirstPersonBodyPlayRate(4.5f, 3.264f, 1.0f, 1.5f), 4.5f / 3.264f));
    CHECK(near(FirstPersonBodyPlayRate(9.0f, 3.0f, 1.0f, 1.5f), 1.5f));
    CHECK(near(FirstPersonBodyPlayRate(4.5f, 3.264f, 0.0f, 1.5f), 1.0f) && near(FirstPersonBodyPlayRate(0.0f, 0.0f, 1.0f, 1.5f), 1.0f));
    CHECK(near(FirstPersonBodyPlayRate(3.0f, 3.0f, 1.0f, 1.5f), 1.0f)); // the clips' own speed: as authored
    // The eye: steady at the standing head with no bob, following it fully at 1, offset in the
    // body's frame (x right = model -X).
    const glm::vec3 rest(0.0f, 1.6f, 0.0f), head(0.02f, 1.55f, 0.1f);
    glm::vec3 eye = FirstPersonBodyEye(rest, head, 0.0f, glm::vec3(0.0f));
    CHECK(near(eye.y, 1.6f) && near(eye.z, 0.0f));
    eye = FirstPersonBodyEye(rest, head, 1.0f, glm::vec3(0.1f, 0.05f, 0.2f));
    CHECK(near(eye.x, 0.02f - 0.1f) && near(eye.y, 1.6f) && near(eye.z, 0.3f));
    eye = FirstPersonBodyEye(rest, head, 0.5f, glm::vec3(0.0f));
    CHECK(near(eye.y, 1.575f) && near(eye.z, 0.05f));

    // Turn in place: angles wrap to (-pi, pi]; the body turns once the view is past the threshold.
    CHECK(near(FirstPersonBodyWrapAngle(3.5f), 3.5f - 6.2831853f) && near(FirstPersonBodyWrapAngle(-3.5f), -3.5f + 6.2831853f));
    CHECK(near(FirstPersonBodyWrapAngle(0.4f), 0.4f) && near(FirstPersonBodyWrapAngle(6.2831853f + 0.4f), 0.4f));
    CHECK(!FirstPersonBodyShouldTurn(glm::radians(40.0f), 55.0f) && FirstPersonBodyShouldTurn(glm::radians(-60.0f), 55.0f));
    CHECK(!FirstPersonBodyShouldTurn(glm::radians(170.0f), 0.0f)); // 0 = always faces the view

    // Foot IK: the pelvis drops to the lower foot (capped), rises a little when both are up.
    CHECK(near(FirstPersonBodyFootPelvis(-0.1f, 0.0f, 0.35f, 0.15f), -0.1f) && near(FirstPersonBodyFootPelvis(0.05f, -0.5f, 0.35f, 0.15f), -0.35f));
    CHECK(near(FirstPersonBodyFootPelvis(0.2f, 0.3f, 0.35f, 0.15f), 0.15f) && near(FirstPersonBodyFootPelvis(0.0f, 0.0f, 0.35f, 0.15f), 0.0f));
    // The world gun's mesh keep-out: a gun through two head points is pushed down until both are 5 cm clear.
    {
        const std::vector<glm::vec3> headPoints = {glm::vec3(0.0f), glm::vec3(0.0f, 0.05f, 0.0f)};
        const glm::vec3 a(-0.5f, 0.0f, 0.0f), b(0.5f, 0.0f, 0.0f), down(0.0f, -1.0f, 0.0f);
        CHECK(std::fabs(FirstPersonBodyClearPush(headPoints, a, b, down, 0.05f, 0.3f, 0.01f) - 0.05f) < 0.011f);
        CHECK(FirstPersonBodyClearPush(headPoints, a + glm::vec3(0.0f, -0.2f, 0.0f), b + glm::vec3(0.0f, -0.2f, 0.0f), down, 0.05f, 0.3f, 0.01f) == 0.0f); // clear
        CHECK(FirstPersonBodyClearPush(headPoints, a, b, down, 0.05f, 0.02f, 0.01f) == 0.02f);   // can't clear within the limit
        CHECK(FirstPersonBodyClearPush({}, a, b, down, 0.05f, 0.3f, 0.01f) == 0.0f);        // nothing to clear
        CHECK(FirstPersonBodyClearPush(headPoints, a, b, down, 0.0f, 0.3f, 0.01f) == 0.0f);       // off
    }
    // Elbow Clearance: an arm reaching straight ahead with its elbow 20 cm under the line, on a torso point. It swings
    // about the shoulder-hand line until 6 cm clear - 20 degrees at 5-degree steps (a 6.9 cm chord) - the side asked for first.
    {
        const glm::vec3 shoulder(0.0f), elbow(0.0f, -0.2f, -0.25f), hand(0.0f, 0.0f, -0.5f);
        const std::vector<glm::vec3> torso = {elbow};
        CHECK(FirstPersonBodyElbowGap(torso, shoulder, elbow, hand) < 1e-5f);
        CHECK(FirstPersonBodyElbowGap({}, shoulder, elbow, hand) > 1e3f);
        const float swing = FirstPersonBodyElbowClearSwivel(torso, shoulder, elbow, hand, 0.06f, glm::radians(90.0f), glm::radians(5.0f));
        CHECK(std::fabs(std::fabs(swing) - glm::radians(20.0f)) < 1e-3f);
        CHECK(FirstPersonBodyElbowClearSwivel(torso, shoulder, elbow, hand, 0.06f, glm::radians(90.0f), glm::radians(5.0f), -1.0f) < 0.0f);
        CHECK(FirstPersonBodyElbowClearSwivel(torso, shoulder, elbow, hand, 0.06f, glm::radians(90.0f), glm::radians(5.0f), 1.0f) > 0.0f);
        const glm::vec3 swung = shoulder + glm::angleAxis(swing, glm::vec3(0.0f, 0.0f, -1.0f)) * (elbow - shoulder);
        CHECK(FirstPersonBodyElbowGap(torso, shoulder, swung, hand) >= 0.06f);             // really clear, the hand where it was
        CHECK(FirstPersonBodyElbowClearSwivel({glm::vec3(1.0f, 0.0f, 0.0f)}, shoulder, elbow, hand, 0.06f, 1.5f, 0.1f) == 0.0f); // already clear
        CHECK(FirstPersonBodyElbowClearSwivel(torso, shoulder, elbow, hand, 0.0f, 1.5f, 0.1f) == 0.0f);                          // off
    }
    // The swivel search only measures the points its swing can come near: the angle it picks is the one a
    // search measuring every point picks, over random arms in a random cloud.
    {
        std::uint32_t seed = 12345u;
        auto rnd = [&](float lo, float hi) { seed = seed * 1664525u + 1013904223u; return lo + (hi - lo) * float(seed >> 8) / 16777216.0f; };
        auto rndVec = [&](float r) { return glm::vec3(rnd(-r, r), rnd(-r, r), rnd(-r, r)); };
        int same = 0, cases = 0;
        for (int trial = 0; trial < 200; ++trial) {
            const glm::vec3 shoulder = rndVec(0.1f), hand = shoulder + glm::vec3(rnd(-0.2f, 0.2f), rnd(-0.2f, 0.1f), rnd(-0.6f, -0.3f));
            const glm::vec3 elbow = (shoulder + hand) * 0.5f + rndVec(0.15f);
            std::vector<glm::vec3> cloud(400);
            for (glm::vec3& p : cloud) p = (shoulder + hand) * 0.5f + rndVec(0.35f);
            const float clearance = rnd(0.02f, 0.08f), prefer = rnd(-1.0f, 1.0f);
            const float maxAngle = glm::radians(90.0f), step = glm::radians(5.0f);
            // Every point, every angle, in the search's order.
            const glm::vec3 axis = glm::normalize(hand - shoulder);
            auto gapAt = [&](float angle) {
                return FirstPersonBodyElbowGap(cloud, shoulder, shoulder + glm::angleAxis(angle, axis) * (elbow - shoulder), hand);
            };
            float expect = 0.0f;
            if (const float here = gapAt(0.0f); here < clearance) {
                float bestGap = here;
                bool done = false;
                for (float a = step; a <= maxAngle + 1e-5f && !done; a += step)
                    for (const float s : {prefer < 0.0f ? -a : a, prefer < 0.0f ? a : -a}) {
                        const float g = gapAt(s);
                        if (g >= clearance) { expect = s; done = true; break; }
                        if (g > bestGap) { bestGap = g; expect = s; }
                    }
            }
            ++cases;
            same += FirstPersonBodyElbowClearSwivel(cloud, shoulder, elbow, hand, clearance, maxAngle, step, prefer) == expect;
        }
        CHECK(same == cases);
    }
    // The look-down push: none at or above its start, all of it straight down, rising as a sine between.
    CHECK(near(FirstPersonBodyLookDown(0.0f, 25.0f), 0.0f) && near(FirstPersonBodyLookDown(glm::radians(30.0f), 25.0f), 0.0f));
    CHECK(near(FirstPersonBodyLookDown(glm::radians(-25.0f), 25.0f), 0.0f) && near(FirstPersonBodyLookDown(glm::radians(-90.0f), 25.0f), 1.0f));
    CHECK(near(FirstPersonBodyLookDown(glm::radians(-57.5f), 25.0f), std::sqrt(0.5f)));
    CHECK(FirstPersonBodyLookDown(glm::radians(-40.0f), 25.0f) < FirstPersonBodyLookDown(glm::radians(-70.0f), 25.0f));
    // The chest onto the arms rig's stance: a squared shoulder line turned onto one bladed 20 degrees (left
    // forward) is a 20 degree yaw; none at weight 0 or for a degenerate line.
    {
        const glm::vec3 squared(1.0f, 0.0f, 0.0f), bladed(std::cos(glm::radians(20.0f)), 0.0f, std::sin(glm::radians(20.0f)));
        const glm::quat q = FirstPersonBodyShoulderLineTurn(squared * 0.38f, bladed * 0.37f, 1.0f);
        CHECK(glm::length(q * squared - bladed) < 1e-4f);
        CHECK(std::abs(glm::degrees(2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f))) - 20.0f) < 0.01f);
        const glm::quat half = FirstPersonBodyShoulderLineTurn(squared, bladed, 0.5f);
        CHECK(std::abs(glm::degrees(2.0f * std::acos(std::clamp(std::abs(half.w), 0.0f, 1.0f))) - 10.0f) < 0.01f);
        CHECK(near(FirstPersonBodyShoulderLineTurn(squared, bladed, 0.0f).w, 1.0f));
        CHECK(near(FirstPersonBodyShoulderLineTurn(glm::vec3(0.0f), bladed, 1.0f).w, 1.0f));
        // The rig's tilt (a reload lifting its left shoulder, 30 degrees) is taken only by Clavicle Follow; its
        // blade and length are kept whatever the share.
        const glm::vec3 lifted = glm::vec3(bladed.x * std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f)),
                                           bladed.z * std::cos(glm::radians(30.0f))) * 0.37f;
        const glm::vec3 none = FirstPersonBodyShoulderLineTilt(squared * 0.38f, lifted, 0.0f);
        const glm::vec3 all = FirstPersonBodyShoulderLineTilt(squared * 0.38f, lifted, 1.0f);
        const glm::vec3 some = FirstPersonBodyShoulderLineTilt(squared * 0.38f, lifted, 0.5f);
        CHECK(glm::length(none - bladed * 0.37f) < 1e-4f); // the body's level line, the rig's blade
        CHECK(glm::length(all - lifted) < 1e-4f);
        CHECK(near(glm::length(some), 0.37f) && near(some.y / 0.37f, 0.25f)); // halfway in sine: 0.5 * sin 30
        CHECK(near(std::atan2(some.z, some.x), glm::radians(20.0f)));
        CHECK(glm::length(FirstPersonBodyShoulderLineTilt(glm::vec3(0.0f), lifted, 0.0f) - lifted) < 1e-4f); // degenerate: unchanged
    }
    // Armed Eye Offset: all of it within 25 degrees of level, none from 60 up or down, eased between - the same both ways.
    {
        CHECK(near(FirstPersonBodyArmedEyeLift(0.0f), 1.0f) && near(FirstPersonBodyArmedEyeLift(glm::radians(25.0f)), 1.0f));
        CHECK(near(FirstPersonBodyArmedEyeLift(glm::radians(-25.0f)), 1.0f));
        CHECK(near(FirstPersonBodyArmedEyeLift(glm::radians(-60.0f)), 0.0f) && near(FirstPersonBodyArmedEyeLift(glm::radians(-89.0f)), 0.0f));
        CHECK(near(FirstPersonBodyArmedEyeLift(glm::radians(60.0f)), 0.0f) && near(FirstPersonBodyArmedEyeLift(glm::radians(89.0f)), 0.0f));
        const float mid = FirstPersonBodyArmedEyeLift(glm::radians(-42.5f));
        CHECK(mid > 0.4f && mid < 0.6f && near(FirstPersonBodyArmedEyeLift(glm::radians(42.5f)), mid));
        CHECK(FirstPersonBodyArmedEyeLift(glm::radians(-35.0f)) > FirstPersonBodyArmedEyeLift(glm::radians(-50.0f)));
    }
    // The spine's share of the pitch: Spine Aim up, Spine Aim Down down.
    CHECK(near(FirstPersonBodySpineAim(glm::radians(30.0f), 0.6f, 0.9f), 0.6f) && near(FirstPersonBodySpineAim(glm::radians(-30.0f), 0.6f, 0.9f), 0.9f));
    // Armed, the world torso leans no further than 0.2 rad plus its share of the view's pitch down (less looking up).
    CHECK(near(FirstPersonBodyAimLeanMost(0.0f, 0.9f), 0.2f));
    CHECK(near(FirstPersonBodyAimLeanMost(glm::radians(-30.0f), 0.9f), 0.2f + glm::radians(30.0f) * 0.9f));
    CHECK(FirstPersonBodyAimLeanMost(glm::radians(30.0f), 0.6f) < 0.2f && FirstPersonBodyAimLeanMost(glm::radians(-89.0f), 1.0f) <= 1.2f);
}

// Character outfits: a small wardrobe's items are classified from their folders, and the rules
// (pairs, a hat's fitted haircut, clears, covers, skin) resolve an outfit to the right pieces.
void TestWardrobe() {
    const char* text = R"({
      "root": "assets/Chars",
      "skinMaterials": ["M_Body", "M_Head"],
      "excludeFolders": ["StaticMesh"],
      "slots": [
        {"id": "Hair", "folders": ["Hair"], "headAttached": true},
        {"id": "Hat", "folders": ["Hats", "WithHair"], "headAttached": true},
        {"id": "Top", "folders": ["Tops"]},
        {"id": "Outerwear", "folders": ["Outerwear"]},
        {"id": "Pants", "folders": ["Pants"]},
        {"id": "Shoes", "folders": ["Shoes"]},
        {"id": "Wrist L", "folders": ["Accessories"], "nameSuffix": "_L"}
      ],
      "bodies": {
        "Male": {"parts": [{"part": "Torso", "model": "Body/Torso.fbx"}, {"part": "Legs", "model": "Body/Legs.fbx"},
                           {"part": "Feet", "model": "Body/Feet.fbx"}],
                 "alternates": {"ShoeFeet": "Body/Feet_Shoes.fbx"},
                 "races": [{"name": "European", "head": "Body/Head.fbx", "skin": ""},
                           {"name": "Afro", "head": "Body/Head_Afro.fbx", "skin": "_Afro", "parts": {"Torso": "Body/Torso_Afro.fbx"}}]},
        "Female": {"parts": [{"part": "Torso", "model": "F/Body.fbx"}], "races": [{"name": "European", "head": "F/Head.fbx"}]}
      },
      "covers": [{"slot": "Pants", "nameLacksAll": ["Shorts"], "hide": ["Legs"]},
                 {"slot": "Shoes", "skin": false, "replace": "Feet", "with": "ShoeFeet"}],
      "clears": [{"slot": "Hat", "pathHasAny": ["/WithHair/"], "clear": "Hair"},
                 {"slot": "Outerwear", "nameHasAny": ["Shirt"], "clear": "Top"}],
      "pairs": [{"whenSlot": "Shoes", "whenNameHas": "Boots", "slot": "Pants", "suffix": "_Inboots"}],
      "items": [{"path": "Clothing/Male/Tops/SKM_Secret.fbx", "hidden": true}]
    })";
    Wardrobe::Wardrobe w;
    std::string err;
    CHECK(Wardrobe::Parse(text, w, &err));
    CHECK(w.Bodies[0].Parts.size() == 3 && w.Bodies[0].Parts[0].second == "assets/Chars/Body/Torso.fbx");
    CHECK(!Wardrobe::Parse("{}", w, &err) && !err.empty());
    CHECK(Wardrobe::Parse(text, w, &err));

    CHECK(Wardrobe::PrettyName("SKM_F_Hoodie_Zipper_Hood") == "Hoodie Zipper Hood");
    CHECK(Wardrobe::PrettyName("SKM_Tshirt_Tucked") == "T-Shirt Tucked");
    CHECK(Wardrobe::SkinBase(w, "assets/M/M_Body_Afro.mat") == "M_Body" && Wardrobe::SkinBase(w, "assets/M/M_Cap.mat").empty());

    std::vector<Wardrobe::Item> cat;
    auto add = [&](const std::string& rel, std::vector<std::string> mats = {}) {
        Wardrobe::Item it;
        const bool ok = Wardrobe::Classify(w, "assets/Chars/" + rel, mats, it);
        if (ok) cat.push_back(it);
        return ok;
    };
    CHECK(add("Clothing/Male/Pants/SKM_Jeans.fbx"));
    CHECK(add("Clothing/Male/Pants/SKM_Jeans_Inboots.fbx"));
    CHECK(add("Clothing/Male/Pants/SKM_Shorts.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Boots.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Sneakers.fbx"));
    CHECK(add("Clothing/Male/Shoes/SKM_Flip_Flops.fbx", {"assets/M/M_Body.mat"}));
    CHECK(add("Clothing/Male/Hats/SKM_Cap.fbx"));
    CHECK(add("Clothing/Male/Hats/WithHair/SKM_Cap_Hair.fbx"));
    CHECK(add("Hair/SKM_Bobcut.fbx"));
    CHECK(add("Hair/SKM_Bobcut_Cap.fbx"));
    CHECK(add("Clothing/Male/Tops/SKM_Tshirt.fbx"));
    CHECK(add("Clothing/Male/Outerwear/SKM_Jacket_Shirt.fbx"));
    CHECK(add("Clothing/Female/Tops/SKM_F_Tshirt.fbx"));
    CHECK(add("Clothing/Male/Accessories/SKM_Watch_L.fbx"));
    CHECK(!add("Clothing/Male/Accessories/SKM_Watch_R.fbx"));      // no Wrist R slot
    CHECK(!add("Clothing/Male/Glasses/StaticMesh/SM_Glasses.fbx")); // excluded folder
    CHECK(!add("Clothing/Male/Tops/SKM_Secret.fbx"));              // hidden override
    CHECK(cat.back().Slot == "Wrist L");
    CHECK(cat[7].Slot == "Hat" && cat[12].Sex == Wardrobe::Gender::Female && cat[5].Skin && !cat[4].Skin);
    Wardrobe::MarkVariants(w, cat);
    CHECK(cat[1].Variant && !cat[0].Variant && cat[9].Variant && !cat[8].Variant); // Jeans_Inboots, Bobcut_Cap

    auto pieceFor = [](const Wardrobe::Resolved& r, const std::string& slot) {
        for (const auto& p : r.Pieces) if (p.Slot == slot) return p.Path;
        return std::string();
    };
    Wardrobe::Request req;
    req.Race = "European";
    req.Items = {{"Pants", "assets/Chars/Clothing/Male/Pants/SKM_Jeans.fbx"}, {"Shoes", "assets/Chars/Clothing/Male/Shoes/SKM_Boots.fbx"},
                 {"Hair", "assets/Chars/Hair/SKM_Bobcut.fbx"}, {"Hat", "assets/Chars/Clothing/Male/Hats/SKM_Cap.fbx"},
                 {"Top", "assets/Chars/Clothing/Male/Tops/SKM_Tshirt.fbx"}};
    Wardrobe::Resolved r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Pants")) == "SKM_Jeans_Inboots");  // boots -> the pants cut for them
    CHECK(Wardrobe::Stem(pieceFor(r, "Hair")) == "SKM_Bobcut_Cap");      // a cap -> the haircut fitted to it
    CHECK(pieceFor(r, "Legs").empty());                                   // pants cover the legs
    CHECK(Wardrobe::Stem(pieceFor(r, "Feet")) == "Feet_Shoes");           // shoes -> feet cut for shoes
    CHECK(Wardrobe::Stem(pieceFor(r, "Head")) == "Head" && !pieceFor(r, "Top").empty());
    // And back: sneakers take the plain jeans, no hat the plain cut; a WithHair cap clears the hair.
    req.Items["Pants"] = "assets/Chars/Clothing/Male/Pants/SKM_Jeans_Inboots.fbx";
    req.Items["Shoes"] = "assets/Chars/Clothing/Male/Shoes/SKM_Sneakers.fbx";
    req.Items["Hair"] = "assets/Chars/Hair/SKM_Bobcut_Cap.fbx";
    req.Items["Hat"] = "assets/Chars/Clothing/Male/Hats/WithHair/SKM_Cap_Hair.fbx";
    req.Items["Outerwear"] = "assets/Chars/Clothing/Male/Outerwear/SKM_Jacket_Shirt.fbx";
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Pants")) == "SKM_Jeans");
    CHECK(pieceFor(r, "Hair").empty() && pieceFor(r, "Top").empty() && !pieceFor(r, "Outerwear").empty());
    req.Items.erase("Hat");
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Hair")) == "SKM_Bobcut");          // no hat: the plain cut again
    // Flip-flops carry skin: the feet stay; shorts leave the legs; the Afro race brings its torso and head.
    req.Items = {{"Shoes", "assets/Chars/Clothing/Male/Shoes/SKM_Flip_Flops.fbx"}, {"Pants", "assets/Chars/Clothing/Male/Pants/SKM_Shorts.fbx"}};
    req.Race = "Afro";
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(Wardrobe::Stem(pieceFor(r, "Feet")) == "Feet" && !pieceFor(r, "Legs").empty());
    CHECK(Wardrobe::Stem(pieceFor(r, "Torso")) == "Torso_Afro" && Wardrobe::Stem(pieceFor(r, "Head")) == "Head_Afro");
    // A female request ignores male items.
    req.Sex = Wardrobe::Gender::Female;
    req.Items = {{"Top", "assets/Chars/Clothing/Male/Tops/SKM_Tshirt.fbx"}};
    r = Wardrobe::Resolve(w, cat, req);
    CHECK(pieceFor(r, "Top").empty() && Wardrobe::Stem(pieceFor(r, "Torso")) == "Body");

    // Skin: the race's variant when it exists, else the base.
    const Wardrobe::RaceDef& afro = w.Bodies[0].Races[1];
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Body.mat", [](const std::string&) { return true; }) == "assets/M/M_Body_Afro.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Head.mat", [](const std::string&) { return false; }) == "assets/M/M_Head.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, w.Bodies[0].Races[0], "assets/M/M_Body_Afro.mat", nullptr) == "assets/M/M_Body.mat");
    CHECK(Wardrobe::SkinMaterialFor(w, afro, "assets/M/M_Cap.mat", nullptr) == "assets/M/M_Cap.mat");

    const auto colours = Wardrobe::Colourways("a/M_Hoodie_Blue.mat", {"a/M_Hoodie_Red.mat", "a/readme.txt", "a/M_Hoodie_Black.mat"});
    CHECK(colours.size() == 3 && Wardrobe::Stem(colours[0]) == "M_Hoodie_Black");

    // Layers: each hides what pokes through it from the layers under it - and nothing else.
    Wardrobe::Wardrobe lw;
    CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Top"}, {"id": "Outerwear"}, {"id": "Pants"}, {"id": "Shoes"},
                                        {"id": "Hair", "headAttached": true}, {"id": "Bag", "layer": 1, "hides": false}],
                              "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}},
                              "layers": [{"slot": "Top", "nameHasAny": ["Tucked"], "layer": 2},
                                         {"slot": "Shoes", "nameHasAny": ["Boots"], "layer": 5},
                                         {"slot": "Top", "nameHasAny": ["Hood_Up"], "over": ["Hair"]}]})", lw, &err));
    auto layer = [&](const char* slot, const char* item, bool body = false) { return Wardrobe::LayerOf(lw, slot, item, body); };
    const auto head = layer("Head", "a/Head.fbx", true), hair = layer("Hair", "a/SKM_Hair_Long.fbx");
    const auto tee = layer("Top", "a/SKM_Tshirt.fbx"), tucked = layer("Top", "a/SKM_Tshirt_Tucked.fbx");
    const auto hoodUp = layer("Top", "a/SKM_Hoodie_Hood_Up.fbx"), jacket = layer("Outerwear", "a/SKM_Jacket.fbx");
    const auto jeans = layer("Pants", "a/SKM_Jeans.fbx"), sneakers = layer("Shoes", "a/SKM_Sneakers.fbx"), boots = layer("Shoes", "a/SKM_Boots.fbx");
    CHECK(Wardrobe::Hides(jacket, "Top", tee) && !Wardrobe::Hides(tee, "Outerwear", jacket)); // the shirt under the jacket
    CHECK(Wardrobe::Hides(tee, "Head", head) && !Wardrobe::Hides(head, "Top", tee));          // the body under everything
    CHECK(Wardrobe::Hides(hoodUp, "Hair", hair) && !Wardrobe::Hides(tee, "Hair", hair));      // a hood that's up takes the hair
    CHECK(!Wardrobe::Hides(hair, "Head", head));                                              // hair cards hide nothing
    CHECK(Wardrobe::Hides(tee, "Pants", jeans) && Wardrobe::Hides(jeans, "Top", tucked));     // tucked or not
    CHECK(Wardrobe::Hides(jeans, "Shoes", sneakers) && Wardrobe::Hides(boots, "Pants", jeans)); // hems over shoes, into boots
    CHECK(!Wardrobe::Hides(layer("Bag", "a/SKM_Bag.fbx"), "Torso", head) && !Wardrobe::Hides(tee, "Top", tee));

    // Compatibility rules: tags from names, materials and earlier tags; hard and soft excludes; styles.
    Wardrobe::Wardrobe cw;
    CHECK(Wardrobe::Parse(R"({"slots": [{"id": "Top", "folders": ["Tops"]}, {"id": "Outerwear", "folders": ["Outerwear"]},
                                        {"id": "Shoes", "folders": ["Shoes"]}],
                              "bodies": {"Male": {"parts": [{"part": "Torso", "model": "T.fbx"}]}},
                              "tags": [{"slot": "Outerwear", "materialHasAny": ["Shirt"], "add": ["BuiltInTop"]},
                                       {"slot": "Outerwear", "lacksTags": ["BuiltInTop"], "add": ["Open"]},
                                       {"slot": "Top", "nameHasAny": ["Hoodie"], "add": ["Thick", "Sport"]},
                                       {"slot": "Shoes", "nameHasAny": ["Flip"], "add": ["Summer"]}],
                              "clears": [{"slot": "Outerwear", "tags": ["BuiltInTop"], "clear": "Top"}],
                              "excludes": [{"a": {"tags": ["Open"]}, "b": {"tags": ["Thick"]}},
                                           {"soft": true, "a": {"slot": "Outerwear"}, "b": {"slot": "Shoes", "nameHasAny": ["Flip"]}}],
                              "styles": [{"name": "Sport", "fill": {"Top": 1.0}}, {"name": "Summer", "fill": {"Shoes": 1.0}}]})", cw, &err));
    std::vector<Wardrobe::Item> ci;
    auto addc = [&](const std::string& rel, std::vector<std::string> mats = {}) {
        Wardrobe::Item it;
        CHECK(Wardrobe::Classify(cw, rel, mats, it));
        ci.push_back(it);
    };
    addc("Tops/SKM_Hoodie.fbx");
    addc("Tops/SKM_Tee.fbx");
    addc("Outerwear/SKM_Jacket.fbx", {"m/M_Jacket.mat", "m/M_TShirt_Black.mat"});
    addc("Outerwear/SKM_Bomber.fbx", {"m/M_Bomber.mat"});
    addc("Shoes/SKM_Flip_Flops.fbx");
    CHECK(Wardrobe::HasTag(ci[2], "BuiltInTop") && !Wardrobe::HasTag(ci[2], "Open") && Wardrobe::HasTag(ci[3], "Open"));
    CHECK(Wardrobe::Conflicts(cw, ci[3], ci[0]) && !Wardrobe::Conflicts(cw, ci[3], ci[1])); // open jacket: no hoodie
    CHECK(Wardrobe::Conflicts(cw, ci[2], ci[1]));                                            // it clears the top
    CHECK(Wardrobe::Conflicts(cw, ci[3], ci[4]) && !Wardrobe::Conflicts(cw, ci[3], ci[4], false)); // soft
    CHECK(Wardrobe::InStyle(cw, ci[0], cw.Styles[0]) && !Wardrobe::InStyle(cw, ci[4], cw.Styles[0]) &&
          Wardrobe::InStyle(cw, ci[1], cw.Styles[1])); // untagged fits any style
    Wardrobe::Request cr;
    cr.Items = {{"Outerwear", ci[3].Path}, {"Top", ci[0].Path}, {"Shoes", ci[4].Path}};
    const auto cres = Wardrobe::Resolve(cw, ci, cr);
    CHECK(cres.Dropped == std::vector<std::string>{"Top"} && cres.Clashes.size() == 1);

    const Wardrobe::Diff d = Wardrobe::MakeDiff({{"Torso", "a/T.fbx"}, {"Hat", "a/Cap.fbx"}, {"Hair", "a/H.fbx"}},
                                                {{"Torso", "a/T.fbx"}, {"Hair", "a/H2.fbx"}, {"Top", "a/Top.fbx"}});
    CHECK(d.Create.size() == 1 && d.Create[0].Slot == "Top" && d.Remodel.size() == 1 && d.Remodel[0].Slot == "Hair");
    CHECK(d.Destroy.size() == 1 && d.Destroy[0] == "Hat");
}

// Skin hiding: a sphere of skin under a cap of cloth - the covered half is found, the uncovered half
// isn't, erosion opens the edge ring, and the bits pack 32 to a word.
void TestOutfitCoverage() {
    auto sphere = [](float r, bool upperOnly) {
        OutfitCoverage::Mesh m;
        const int rings = 16, segs = 24;
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const float th = glm::pi<float>() * (float)i / rings, ph = glm::two_pi<float>() * (float)j / segs;
                m.Positions.push_back(r * glm::vec3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)));
            }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const unsigned a = (unsigned)(i * segs + j), b = (unsigned)(i * segs + (j + 1) % segs);
                const unsigned c = a + segs, d = b + segs;
                if (upperOnly && i >= rings / 2) continue;
                // Outward-facing winding (the normals point away from the centre).
                m.Indices.insert(m.Indices.end(), {a, b, c, b, d, c});
            }
        return m;
    };
    const OutfitCoverage::Mesh skin = sphere(0.10f, false), cap = sphere(0.12f, true);
    const auto normals = OutfitCoverage::VertexNormals(skin);
    CHECK(glm::dot(normals[(size_t)(4 * 24)], glm::normalize(skin.Positions[(size_t)(4 * 24)])) > 0.9f);
    std::vector<std::uint8_t> covered = OutfitCoverage::Covered(skin, cap);
    int upper = 0, upperCovered = 0, lower = 0, lowerCovered = 0;
    for (size_t v = 0; v < skin.Positions.size(); ++v) {
        const float y = skin.Positions[v].y;
        if (y > 0.03f) { ++upper; upperCovered += covered[v]; }
        if (y < -0.03f) { ++lower; lowerCovered += covered[v]; }
    }
    CHECK(upper > 0 && upperCovered == upper);
    CHECK(lower > 0 && lowerCovered == 0);
    // Cloth 1.5 cm inside the skin (skin poking through) still counts; 10 cm away doesn't.
    CHECK(OutfitCoverage::Covered(skin, sphere(0.085f, true))[(size_t)(2 * 24)] == 1);
    CHECK(OutfitCoverage::Covered(skin, sphere(0.075f, true))[(size_t)(2 * 24)] == 1); // a shirt 2.5 cm out through a jacket
    CHECK(OutfitCoverage::Covered(skin, sphere(0.06f, true))[(size_t)(2 * 24)] == 0);
    CHECK(OutfitCoverage::Covered(skin, sphere(0.20f, true))[(size_t)(2 * 24)] == 0);
    // The head's rule (Backed): a neck in a collar standing 3 cm off it. Just under the rim the skin can be
    // seen over it with nothing behind (the torso's hidden too): hiding it would open a hole, so it stays.
    // Deep in a tube reaching well past it, nothing sees it. Poking through a shirt, the shirt's behind.
    auto tube = [](float r, float y0, float y1) {
        OutfitCoverage::Mesh m;
        const int rings = 20, segs = 24;
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const float ph = glm::two_pi<float>() * (float)j / segs;
                m.Positions.emplace_back(r * std::cos(ph), y0 + (y1 - y0) * (float)i / rings, r * std::sin(ph));
            }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const unsigned a = (unsigned)(i * segs + j), b = (unsigned)(i * segs + (j + 1) % segs);
                const unsigned c = a + segs, d = b + segs;
                m.Indices.insert(m.Indices.end(), {a, c, b, b, c, d}); // outward-facing
            }
        return m;
    };
    const OutfitCoverage::Mesh neck = tube(0.06f, 0.0f, 0.20f);
    CHECK(glm::dot(OutfitCoverage::VertexNormals(neck)[(size_t)(8 * 24)], glm::vec3(1.0f, 0.0f, 0.0f)) > 0.9f);
    const size_t underRim = (size_t)(8 * 24); // y = 8 cm, facing +X
    const OutfitCoverage::Mesh collar = tube(0.09f, -0.30f, 0.10f), wrap = tube(0.09f, -0.30f, 0.50f);
    // 2 cm under the rim is inside the edge band (kEdgeBand): drawn, so a collar swinging off it opens no hole.
    // 6 cm under it, the band is past and it's hidden.
    CHECK(OutfitCoverage::Hidden(neck, collar)[underRim] == 0);
    CHECK(OutfitCoverage::Hidden(neck, collar)[(size_t)(4 * 24)] == 1);
    CHECK(OutfitCoverage::Hidden(neck, collar, true)[underRim] == 0); // the head's keeps it
    CHECK(OutfitCoverage::Hidden(neck, wrap, true)[underRim] == 1);
    // Skin poking 5 mm through a shirt front: from every way it's seen, the shirt's right behind it.
    auto sheet = [](float half, float z) {
        OutfitCoverage::Mesh m;
        const int n = 20;
        for (int i = 0; i <= n; ++i)
            for (int j = 0; j <= n; ++j) m.Positions.emplace_back(-half + 2 * half * j / n, -half + 2 * half * i / n, z);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const unsigned a = (unsigned)(i * (n + 1) + j), b = a + 1, c = a + n + 1, d = c + 1;
                m.Indices.insert(m.Indices.end(), {a, b, c, b, d, c}); // facing +Z
            }
        return m;
    };
    const OutfitCoverage::Mesh chest = sheet(0.05f, 0.0f), shirt = sheet(0.30f, -0.005f);
    const size_t middle = (size_t)(10 * 21 + 10);
    CHECK(OutfitCoverage::PokeDepth(chest, shirt, OutfitCoverage::kPokeReach)[middle] > 0.0f);
    CHECK(OutfitCoverage::Hidden(chest, shirt, true)[middle] == 1);
    const int before = (int)std::count(covered.begin(), covered.end(), (std::uint8_t)1);
    OutfitCoverage::Erode(skin, covered, 1);
    const int after = (int)std::count(covered.begin(), covered.end(), (std::uint8_t)1);
    CHECK(after < before && after > 0 && covered[0] == 1); // the pole stays, the hem ring opens
    const auto bits = OutfitCoverage::Pack({1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    CHECK(bits.size() == 2 && bits[0] == 0x9u && bits[1] == 0x1u);
}

// The Quantum pack's wardrobe (project/assets/Characters/Quantum/Quantum.wardrobe) against the real
// files: every body part it names exists, each slot finds items, and the pack's pairs resolve. (Coverage
// on the real meshes needs them loaded - GL - so it's checked in the editor, not here.)
void TestWardrobeQuantum() {
    // The Quantum pack is project content, not engine data: a packaged engine (CI's standalone check)
    // runs without it, and then there's nothing to test.
    if (!std::filesystem::exists(ProjectPaths::Resolve("assets/Characters/Quantum/Quantum.wardrobe"))) {
        Log::Info("WardrobeQuantum: no Quantum pack in this project - skipped.");
        return;
    }
    AssetLibrary assets;
    std::string err;
    const auto cat = OutfitSystem::LoadCatalog(assets, "assets/Characters/Quantum/Quantum.wardrobe", true, &err);
    CHECK(cat != nullptr);
    if (!cat) { Log::Warn("Quantum wardrobe: " + err); return; }
    CHECK(cat->Items.size() > 100);
    auto exists = [](const std::string& rel) { return std::filesystem::exists(ProjectPaths::Resolve(rel)); };
    for (const auto& body : cat->W.Bodies) {
        for (const auto& [part, model] : body.Parts) CHECK(exists(model));
        for (const auto& race : body.Races) {
            CHECK(exists(race.Head));
            for (const auto& [part, model] : race.Parts) CHECK(exists(model));
        }
        for (const auto& [name, model] : body.Alternates) CHECK(exists(model));
    }
    for (const auto& slot : cat->W.Slots)
        for (int g = 0; g < 2; ++g) {
            const auto n = cat->ForSlot(slot.Id, (Wardrobe::Gender)g).size();
            Log::Info("Quantum wardrobe: " + slot.Id + " " + Wardrobe::GenderName((Wardrobe::Gender)g) + " " + std::to_string(n));
            if (slot.Id != "Collar") CHECK(n > 0);
        }
    auto item = [&](const std::string& rel) {
        const auto* it = cat->Find("assets/Characters/Quantum/Models/" + rel);
        CHECK(it != nullptr);
        return it ? it->Path : std::string();
    };
    auto pieceFor = [](const Wardrobe::Resolved& r, const std::string& slot) {
        for (const auto& p : r.Pieces) if (p.Slot == slot) return Wardrobe::Stem(p.Path);
        return std::string();
    };
    Wardrobe::Request req;
    req.Race = "Afro";
    req.Items = {{"Pants", item("Clothing/Male/Pants/SKM_Jeans.fbx")}, {"Shoes", item("Clothing/Male/Shoes/SKM_Boots.fbx")},
                 {"Top", item("Clothing/Male/Tops/SKM_Tshirt.fbx")}, {"Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx")}};
    Wardrobe::Resolved r = Wardrobe::Resolve(cat->W, cat->Items, req);
    CHECK(pieceFor(r, "Pants") == "SKM_Jeans_Inboots" && pieceFor(r, "Feet") == "Quantum_Feet_Shoes");
    CHECK(pieceFor(r, "Legs").empty() && pieceFor(r, "Head") == "Quantum_Head_Afro" && pieceFor(r, "Torso") == "Quantum_Torso_Afro");
    CHECK(pieceFor(r, "Balaclava") == "SM_Balaclava_Crime");
    // No hair in this wardrobe, and on the head only the balaclava, hoods and the black (classic) glasses:
    // no hats, headphones or aviators.
    req = {};
    req.Sex = Wardrobe::Gender::Female;
    req.Items = {{"Balaclava", item("Clothing/Female/Balaclava/SM_F_Balaclava_Crime.fbx")},
                 {"Glasses", item("Clothing/Female/Glasses/SKM_F_Glasses_Classic.fbx")}};
    r = Wardrobe::Resolve(cat->W, cat->Items, req);
    CHECK(pieceFor(r, "Balaclava") == "SM_F_Balaclava_Crime" && pieceFor(r, "Glasses") == "SKM_F_Glasses_Classic");
    CHECK(pieceFor(r, "Torso") == "SKM_F_Vivian_Body");
    CHECK(cat->Find("assets/Characters/Quantum/Models/Hair/SKM_Hair_Short.fbx") == nullptr);
    CHECK(!cat->W.Slot("Hat") && !cat->W.Slot("Headphones"));
    for (const auto& it : cat->Items) CHECK(it.Path.find("/Hats/") == std::string::npos && it.Path.find("Aviator") == std::string::npos);
    // A hood that's up goes over the balaclava, and only that way round (else each cuts holes in the other).
    for (const char* hood : {"Tops/SKM_Hoodie_Hood_Up", "Tops/SKM_Hoodie_Zipper_Hood", "Outerwear/SKM_Coat_Hoodie_Hood"}) {
        const std::string slot = std::string(hood).rfind("Tops", 0) == 0 ? "Top" : "Outerwear";
        const auto h = Wardrobe::LayerOf(cat->W, slot, item(std::string("Clothing/Male/") + hood + ".fbx"), false);
        const auto b = Wardrobe::LayerOf(cat->W, "Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx"), false);
        CHECK(Wardrobe::Hides(h, "Balaclava", b, slot) && !Wardrobe::Hides(b, slot, h, "Balaclava"));
    }
    // A parka is too bulky for a bag.
    r = Wardrobe::Resolve(cat->W, cat->Items, [&] { Wardrobe::Request m; m.Items = {{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Winter_Closed_Hood_Closed.fbx")},
                                                                                  {"Bag", item("Clothing/Male/Bags/SKM_Backpack_Sport.fbx")}}; return m; }());
    CHECK(pieceFor(r, "Bag").empty() && !pieceFor(r, "Outerwear").empty());

    // What goes together: jackets with a top of their own take the top off, an open one keeps a thin top
    // but not a hoodie, boots cut to go under the pants keep the plain pants, socks are their own slot.
    auto male = [&](std::map<std::string, std::string> items) {
        Wardrobe::Request m;
        m.Items = std::move(items);
        return Wardrobe::Resolve(cat->W, cat->Items, m);
    };
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Leather_Jacket.fbx")}, {"Top", item("Clothing/Male/Tops/SKM_Hoodie.fbx")}});
    CHECK(pieceFor(r, "Top").empty() && r.Dropped.size() == 1);
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_M65.fbx")}, {"Top", item("Clothing/Male/Tops/SKM_Hoodie.fbx")}});
    CHECK(pieceFor(r, "Top").empty() && !pieceFor(r, "Outerwear").empty());
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_M65.fbx")}, {"Top", item("Clothing/Male/Tops/SKM_Tshirt.fbx")}});
    CHECK(pieceFor(r, "Top") == "SKM_Tshirt" && r.Dropped.empty());
    r = male({{"Shoes", item("Clothing/Male/Shoes/SKM_Boots_Inboots.fbx")}, {"Pants", item("Clothing/Male/Pants/SKM_Jeans.fbx")},
              {"Socks", item("Clothing/Male/Shoes/SKM_Socks.fbx")}});
    CHECK(pieceFor(r, "Pants") == "SKM_Jeans" && pieceFor(r, "Socks") == "SKM_Socks" && pieceFor(r, "Shoes") == "SKM_Boots_Inboots");
    // A hood that's up: the balaclava stays under it (the hood's layered over it).
    r = male({{"Top", item("Clothing/Male/Tops/SKM_Hoodie_Hood_Up.fbx")}, {"Balaclava", item("Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx")}});
    CHECK(pieceFor(r, "Balaclava") == "SM_Balaclava_Crime" && pieceFor(r, "Top") == "SKM_Hoodie_Hood_Up");
    r = male({{"Outerwear", item("Clothing/Male/Outerwear/SKM_Jacket_Classic_Tie.fbx")},
              {"Shoes", item("Clothing/Male/Shoes/SKM_Flip_Flops.fbx")}});
    CHECK(!pieceFor(r, "Shoes").empty() && r.Clashes.size() == 1); // a style clash warns, it doesn't undress

    // Randomize: whole outfits (top or a jacket that covers the chest, pants, shoes), nothing the rules
    // must take off, no clash, no variant picked directly - and the same seed gives the same outfit.
    for (int g = 0; g < 2; ++g)
        for (unsigned seed = 1; seed <= 300; ++seed) {
            Wardrobe::Request base;
            base.Sex = (Wardrobe::Gender)g;
            std::string style;
            const auto want = Wardrobe::Randomize(cat->W, cat->Items, base, seed, {}, "", &style);
            CHECK(!style.empty() && Wardrobe::Randomize(cat->W, cat->Items, base, seed).Items == want.Items);
            const auto got = Wardrobe::Resolve(cat->W, cat->Items, want);
            CHECK(got.Dropped.empty() && got.Clashes.empty());
            CHECK(!pieceFor(got, "Pants").empty() && !pieceFor(got, "Shoes").empty());
            for (const auto& [slot, path] : want.Items) CHECK(cat->Find(path) && !cat->Find(path)->Variant);
        }
    // Locked slots stay as they are.
    Wardrobe::Request locked;
    locked.Items = {{"Top", item("Clothing/Male/Tops/SKM_Shirt_Hawaii.fbx")}};
    CHECK(Wardrobe::Randomize(cat->W, cat->Items, locked, 7, {"Top"}).Items.at("Top") == locked.Items["Top"]);
    CHECK(Wardrobe::FindStyle(cat->W, "Formal") && Wardrobe::FindStyle(cat->W, "Formal")->Gender == 0);
}

// Async loading's CPU halves (AssetLibrary::RequestModelAsync's workers run these): no GL anywhere, so
// they work here, headless. A deferred import is complete except for its GPU objects, and a texture decode
// honours Max Size. The Vivian head/body also pin the female-import fix: upright, at head height.
void TestAsyncImportCpu() {
    auto deferred = [](const std::string& rel) {
        return Model::ImportDeferred(ProjectPaths::Resolve(rel), ModelImportSettings{});
    };
    // The model half needs the Quantum pack (project content, absent from a packaged engine).
    const bool quantum = std::filesystem::exists(ProjectPaths::Resolve("assets/Characters/Quantum"));
    if (quantum) {
        const auto head = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head.fbx");
        CHECK(head && head->NeedsGpuUpload());
        CHECK(head && head->MeshCount() > 1);
        CHECK(head && head->BoundsMax().y > 1.6f && head->BoundsMin().y > 1.3f); // on the neck, not lying at the feet
        const auto body = deferred("assets/Characters/Quantum/Models/Female/Body/Parts/SKM_F_Vivian_Body.fbx");
        CHECK(body && body->BoundsMax().y > 1.4f && body->BoundsMax().z < 0.5f); // upright, not stretched along Z
        // Offsets carrying a group node's frame, not the mesh node's (the jackets' "Cloth" group): chest height.
        const auto m65 = deferred("assets/Characters/Quantum/Models/Clothing/Male/Outerwear/SKM_Jacket_M65.fbx");
        CHECK(m65 && m65->BoundsMax().y > 1.3f && m65->BoundsMax().z < 0.6f);
        const auto oldHead = deferred("assets/Characters/Quantum/Models/Female/Body/Heads/SKM_F_Vivian_Head_Old.fbx");
        CHECK(oldHead && oldHead->BoundsMax().y > 1.6f && oldHead->BoundsMin().y > 1.3f);
    } else {
        Log::Info("AsyncImportCpu: no Quantum pack - model checks skipped.");
    } // the models are destroyed here with nothing uploaded, which must not touch GL

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "TartarusUnitTests";
    std::filesystem::create_directories(dir, ec);
    const std::string png = (dir / "async_decode.png").string();
    std::vector<unsigned char> px(64 * 32 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = (unsigned char)(i * 7);
    CHECK(stbi_write_png(png.c_str(), 64, 32, 4, px.data(), 64 * 4) != 0);
    TextureImportSettings s;
    s.MaxTextureSize = 16;
    TextureCpuData d = Texture::DecodeFile(png, s);
    CHECK(d.Ok && d.SourceWidth == 64 && d.SourceHeight == 32 && d.Width == 16 && d.Height == 8 && d.Channels == 4);
    CHECK(d.Pixels.size() == (size_t)16 * 8 * 4 && d.GLFormat == 0);
    const auto pending = Texture::CreatePending(std::move(d));
    CHECK(pending && pending->IsPendingUpload() && !pending->IsValid() && pending->Width() == 64);
    CHECK(Texture::CreatePending(Texture::DecodeFile((dir / "missing.png").string(), s)) == nullptr);
    std::filesystem::remove(png, ec);
}

// The player's clothing around the camera (#480): its Near Hide survives the per-frame refresh and reaches
// the shoulders to the sides, and a garment's collar is found by where it sits in the bind pose.
void TestFirstPersonBodyClothingHide() {
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    // The body keeps the component's Near Hide; clothing at least Clothing Near Hide - never less than the body's.
    CHECK(near(FirstPersonBodyPieceNearHide(0.1f, 0.2f, false), 0.1f) && near(FirstPersonBodyPieceNearHide(0.1f, 0.2f, true), 0.2f));
    CHECK(near(FirstPersonBodyPieceNearHide(0.25f, 0.2f, true), 0.25f) && near(FirstPersonBodyPieceNearHide(-1.0f, 0.0f, false), 0.0f));
    // What Tick applies every frame: a clothing piece keeps its own (the regression: it was reset to the body's).
    {
        FirstPersonBodyComponent cfg;
        PlayerBodyTag cloth, skin;
        cloth.Clothing = true;
        for (PlayerBodyTag* t : {&cloth, &skin}) t->NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, t->Clothing);
        CHECK(near(cloth.NearHide, cfg.ClothingNearHide) && near(skin.NearHide, cfg.NearHide) && cloth.NearHide > skin.NearHide);
    }
    // The hide: a sphere with no width; stretched to the sides with one, but no further below (the chest).
    const glm::vec3 eye(0.0f, 1.6f, 0.0f), right(1.0f, 0.0f, 0.0f);
    CHECK(FirstPersonBodyNearHidden(eye + glm::vec3(0.0f, -0.15f, 0.0f), eye, right, 0.2f, 0.0f));
    CHECK(!FirstPersonBodyNearHidden(eye + glm::vec3(0.22f, -0.05f, 0.0f), eye, right, 0.2f, 0.0f));
    CHECK(FirstPersonBodyNearHidden(eye + glm::vec3(0.22f, -0.05f, 0.0f), eye, right, 0.2f, 0.28f)); // a shoulder top
    CHECK(FirstPersonBodyNearHidden(eye + glm::vec3(-0.2f, -0.1f, 0.05f), eye, right, 0.2f, 0.28f));
    CHECK(!FirstPersonBodyNearHidden(eye + glm::vec3(0.0f, -0.28f, 0.1f), eye, right, 0.2f, 0.28f)); // the chest, looking down
    CHECK(!FirstPersonBodyNearHidden(eye + glm::vec3(0.3f, 0.0f, 0.0f), eye, right, 0.2f, 0.28f));
    CHECK(!FirstPersonBodyNearHidden(eye, eye, right, 0.0f, 0.28f)); // off

    // The collar, in a mannequin's frame (+Z ahead, shoulders 0.36 m apart at 1.45 m, the neck above them).
    const glm::vec3 neck(0.0f, 1.52f, -0.02f), shoulderL(0.18f, 1.45f, 0.0f), shoulderR(-0.18f, 1.45f, 0.0f);
    const std::vector<glm::vec3> points = {
        {0.0f, 1.55f, 0.08f},  // the collar's front
        {0.0f, 1.62f, -0.15f}, // a hood lying on the back
        {0.15f, 1.5f, 0.0f},   // a shoulder top
        {0.0f, 1.3f, 0.12f},   // the chest
        {0.4f, 1.46f, 0.0f},   // a sleeve, out along a T-pose arm
        {0.0f, 1.0f, 0.1f},    // the hem
    };
    auto collar = FirstPersonBodyCollarVertices(points, neck, shoulderL, shoulderR, 0.02f);
    CHECK(collar == std::vector<std::uint8_t>({1, 1, 1, 0, 0, 0}));
    CHECK(FirstPersonBodyCollarVertices(points, neck, shoulderL, shoulderR, -1.0f).empty()); // off
    CHECK(FirstPersonBodyCollarVertices(points, neck, shoulderL, shoulderL, 0.02f).empty()); // no shoulder line

    // The Quantum pack's winter jacket, hood up: its hood and collar are found, its body and sleeves aren't. Needs the pack (project content).
    const std::string jacketPath = "assets/Characters/Quantum/Models/Clothing/Male/Outerwear/SKM_Jacket_Winter_Open_Hood_Closed.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(jacketPath))) {
        Log::Info("FirstPersonBodyClothingHide: no Quantum pack - model checks skipped.");
        return;
    }
    const auto jacket = Model::ImportDeferred(ProjectPaths::Resolve(jacketPath), ModelImportSettings{});
    CHECK(jacket != nullptr);
    if (!jacket) return;
    auto bind = [&](const char* name, glm::vec3& out) {
        const int n = jacket->NodeIndex(name);
        if (n >= 0) out = glm::vec3(jacket->SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, n)[3]);
        return n >= 0;
    };
    glm::vec3 jNeck, jL, jR;
    CHECK(bind("neck_01", jNeck) && bind("upperarm_l", jL) && bind("upperarm_r", jR));
    std::vector<glm::vec3> pos;
    std::vector<unsigned int> idx;
    jacket->CollisionGeometry(pos, idx);
    CHECK(!pos.empty());
    collar = FirstPersonBodyCollarVertices(pos, jNeck, jL, jR, 0.02f);
    CHECK(collar.size() == pos.size());
    size_t hidden = 0, top = 0, topHidden = 0, low = 0, head = 0, headHidden = 0;
    float yMax = -1e9f;
    for (const auto& p : pos) yMax = std::max(yMax, p.y);
    for (size_t i = 0; i < pos.size(); ++i) {
        hidden += collar[i];
        if (pos[i].y > yMax - 0.05f) { ++top; topHidden += collar[i]; }             // the hood's crown / collar rim
        if (pos[i].y < std::min(jL.y, jR.y) - 0.1f && collar[i]) ++low;          // nothing from the chest down
        if (pos[i].y > jNeck.y) { ++head; headHidden += collar[i]; }                 // the hood, up round the head
    }
    Log::Info("FirstPersonBodyClothingHide: winter jacket " + std::to_string(hidden) + " / " + std::to_string(pos.size()) +
              " collar vertices; shoulders at " + std::to_string(jL.y) + " m, neck " + std::to_string(jNeck.y) + " m, top " +
              std::to_string(yMax) + " m.");
    CHECK(jL.y > 1.2f && jL.y < 1.7f && jNeck.y > jL.y); // bones and vertices share the model's upright, metre frame
    // This jacket's hood is up: its fur is most of the mesh, and all of it is round the camera.
    CHECK(hidden > 0 && hidden < pos.size() && head > 0 && headHidden == head);
    CHECK(top > 0 && topHidden == top);
    CHECK(low == 0);
}

// The body's setup check: a controller built from the contract tables passes; one missing piece is named.
void TestFirstPersonBodyValidate() {
    FirstPersonBodyComponent cfg;
    cfg.TurnThreshold = 55.0f; cfg.StartStopClips = true; cfg.CrouchHeight = 1.2f; cfg.FootIK = true; cfg.WeaponArms = true;
    cfg.SpineAim = 1.0f;
    AnimatorController full;
    for (const auto& p : FPBody::Params()) full.Parameters.push_back({p.Name, p.Type, 0.0f});
    for (const auto& st : FPBody::States()) {
        AnimatorController::State state;
        state.Name = st.Name;
        if (state.Name == FPBody::kStateJump || state.Name == FPBody::kStateFall) state.Tags.push_back(FPBody::kTagAirborne);
        full.Layers[0].States.push_back(state);
    }
    FPBody::ValidationInput in;
    in.Config = &cfg;
    in.HasPieces = in.HasDriverPiece = in.ControllerSet = true;
    in.Controller = &full;
    in.HasBone = [](const std::string&) { return true; };
    in.PieceNames = {"Quantum_Head", "Quantum_Arms"};
    auto count = [](const std::vector<FPBody::Check>& c, FPBody::Severity level) {
        int n = 0;
        for (const auto& x : c) if (x.Level == level) ++n;
        return n;
    };
    auto has = [](const std::vector<FPBody::Check>& c, const char* text) {
        for (const auto& x : c) if (x.Message.find(text) != std::string::npos) return true;
        return false;
    };
    std::vector<FPBody::Check> r = FPBody::Validate(in);
    CHECK(FPBody::Worst(r) == FPBody::Severity::Info && count(r, FPBody::Severity::Warning) == 0); // only the root-motion note

    // A missing parameter, state, bone, tag and piece are each named.
    AnimatorController broken = full;
    broken.Parameters.erase(std::remove_if(broken.Parameters.begin(), broken.Parameters.end(),
                            [](const AnimatorController::Parameter& p) { return p.Name == FPBody::kTurnAngle; }), broken.Parameters.end());
    auto& sts = broken.Layers[0].States;
    sts.erase(std::remove_if(sts.begin(), sts.end(), [](const AnimatorController::State& st) { return st.Name == FPBody::kStateStop; }), sts.end());
    for (auto& st : sts) st.Tags.clear();
    in.Controller = &broken;
    in.HasBone = [](const std::string& b) { return b != "foot_l"; };
    in.PieceNames = {"Quantum_Head"};
    r = FPBody::Validate(in);
    CHECK(has(r, "'TurnAngle'") && has(r, "state 'Stop'") && has(r, "'foot_l'") && has(r, "'Airborne' tag") && has(r, "'Arms' (Arms Piece)"));
    CHECK(r.front().Level == FPBody::Severity::Warning);

    // Features that are off need nothing: a bare controller is fine for a body with every option off.
    FirstPersonBodyComponent plain;
    AnimatorController bare;
    bare.Parameters = {{FPBody::kMoveX, AnimatorController::ParamType::Float, 0}, {FPBody::kMoveY, AnimatorController::ParamType::Float, 0},
                       {FPBody::kSpeed, AnimatorController::ParamType::Float, 0}, {FPBody::kSprint, AnimatorController::ParamType::Bool, 0},
                       {FPBody::kGrounded, AnimatorController::ParamType::Bool, 0}, {FPBody::kAirborne, AnimatorController::ParamType::Bool, 0},
                       {FPBody::kJump, AnimatorController::ParamType::Trigger, 0}};
    AnimatorController::State loco; loco.Name = FPBody::kStateLocomotion;
    AnimatorController::State jump; jump.Name = FPBody::kStateJump;
    AnimatorController::State fall; fall.Name = FPBody::kStateFall;
    AnimatorController::State land; land.Name = FPBody::kStateLand;
    bare.Layers[0].States = {loco, jump, fall, land};
    FPBody::ValidationInput in2;
    in2.Config = &plain; in2.HasPieces = in2.HasDriverPiece = in2.ControllerSet = true; in2.Controller = &bare;
    in2.PieceNames = {"Quantum_Head"};
    CHECK(count(FPBody::Validate(in2), FPBody::Severity::Warning) == 0);

    // Structural errors come first.
    FPBody::ValidationInput in3;
    in3.Config = &plain;
    r = FPBody::Validate(in3);
    CHECK(!r.empty() && r.front().Level == FPBody::Severity::Error);
}

void TestRootMotion() {
    const float kPi = 3.14159265f;
    auto near = [](float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; };
    auto nearV = [](const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) { return glm::length(a - b) <= eps; };
    auto nearM = [](const glm::mat4& a, const glm::mat4& b, float eps = 1e-3f) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > eps) return false;
        return true;
    };
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    RootMotionSettings s; // rotation on, vertical off

    // A walk: 1.4 m/s forward (+Z) for a 1 s loop, bobbing and swaying as it goes.
    auto walk = [&](float t) {
        return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.9f + 0.03f * std::sin(t * 2.0f * kPi), 1.4f * t)) *
               glm::rotate(glm::mat4(1.0f), 0.05f * std::sin(t * 2.0f * kPi), glm::vec3(1.0f, 0.0f, 0.0f));
    };
    RootMotionDelta d = RootMotionBetween(walk, 0.2f, 0.5f, 1.0f, true, s);
    CHECK(nearV(d.Translation, glm::vec3(0.0f, 0.0f, 0.42f)) && near(d.Yaw, 0.0f));
    d = RootMotionBetween(walk, 0.9f, 1.1f, 1.0f, true, s);   // across the loop: keeps walking
    CHECK(nearV(d.Translation, glm::vec3(0.0f, 0.0f, 0.28f)));
    d = RootMotionBetween(walk, 0.5f, 2.5f, 1.0f, true, s);   // two whole passes in one step
    CHECK(nearV(d.Translation, glm::vec3(0.0f, 0.0f, 2.8f)));
    d = RootMotionBetween(walk, 0.8f, 1.3f, 1.0f, false, s);  // a one-shot stops at its end
    CHECK(nearV(d.Translation, glm::vec3(0.0f, 0.0f, 0.28f)));
    const RootMotionDelta back = RootMotionBetween(walk, 0.5f, 0.2f, 1.0f, true, s);
    CHECK(nearV(back.Translation, glm::vec3(0.0f, 0.0f, -0.42f)));
    // In place: the root stays over its first frame, still bobbing.
    const glm::mat4 ip = RootMotionInPlace(walk(0.25f), walk(0.0f), s);
    CHECK(near(ip[3].x, 0.0f) && near(ip[3].z, 0.0f) && near(ip[3].y, walk(0.25f)[3].y));

    // A turn on the spot: 90 degrees over 1 s.
    auto turn = [&](float t) { return glm::rotate(glm::mat4(1.0f), 0.5f * kPi * t, up); };
    d = RootMotionBetween(turn, 0.0f, 1.0f, 1.0f, false, s);
    CHECK(near(d.Yaw, 0.5f * kPi) && nearV(d.Translation, glm::vec3(0.0f)));
    RootMotionSettings noTurn;
    noTurn.Rotation = false;
    CHECK(near(RootMotionBetween(turn, 0.0f, 1.0f, 1.0f, false, noTurn).Yaw, 0.0f));
    CHECK(nearM(RootMotionInPlace(turn(0.6f), turn(0.0f), noTurn), turn(0.6f))); // the turn stays in the pose

    // A curving walk whose root starts off-centre and already turned: the object's motion times the
    // in-place pose must land exactly where the clip put the root, at every time.
    auto curve = [&](float t) {
        const float yaw = 0.3f + 0.8f * t;
        return glm::translate(glm::mat4(1.0f), glm::vec3(0.5f + std::sin(t) * 2.0f, 0.95f, -0.3f + t * t)) *
               glm::rotate(glm::mat4(1.0f), yaw, up) * glm::rotate(glm::mat4(1.0f), 0.1f * t, glm::vec3(0.0f, 0.0f, 1.0f));
    };
    for (float t : {0.0f, 0.37f, 0.9f}) {
        const glm::mat4 moved = RootMotionBetween(curve, 0.0f, t, 1.0f, false, s).Matrix();
        CHECK(nearM(moved * RootMotionInPlace(curve(t), curve(0.0f), s), curve(t)));
    }
    // Deltas chain: [0, 0.4] then [0.4, 0.9] is [0, 0.9].
    const RootMotionDelta a = RootMotionBetween(curve, 0.0f, 0.4f, 1.0f, false, s);
    const RootMotionDelta b = RootMotionBetween(curve, 0.4f, 0.9f, 1.0f, false, s);
    CHECK(nearM(a.Then(b).Matrix(), RootMotionBetween(curve, 0.0f, 0.9f, 1.0f, false, s).Matrix()));

    // Height: kept in the pose unless Vertical is on.
    auto hop = [&](float t) { return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f + t, 0.0f)); };
    CHECK(near(RootMotionBetween(hop, 0.0f, 0.5f, 1.0f, false, s).Translation.y, 0.0f));
    RootMotionSettings vertical;
    vertical.Vertical = true;
    CHECK(near(RootMotionBetween(hop, 0.0f, 0.5f, 1.0f, false, vertical).Translation.y, 0.5f));

    // Mixing (crossfades, blend trees) and the crossfade stack's weights.
    RootMotionMix mix;
    RootMotionDelta one, two;
    one.Translation = glm::vec3(0.0f, 0.0f, 1.0f);
    two.Translation = glm::vec3(0.0f, 0.0f, 3.0f);
    two.Yaw = 1.0f;
    mix.Add(one, 1.0f);
    mix.Add(two, 3.0f);
    CHECK(nearV(mix.Result().Translation, glm::vec3(0.0f, 0.0f, 2.5f)) && near(mix.Result().Yaw, 0.75f));
    CHECK(RootMotionMix{}.Result().IsZero());
    std::vector<float> w = AnimatorStackWeights({1.0f});
    CHECK(w.size() == 1 && near(w[0], 1.0f));
    w = AnimatorStackWeights({1.0f, 0.5f});
    CHECK(near(w[0], 0.5f) && near(w[1], 0.5f));
    w = AnimatorStackWeights({1.0f, 1.0f, 0.25f});
    CHECK(near(w[0], 0.0f) && near(w[0] + w[1] + w[2], 1.0f) && near(w[2], AnimatorCrossfadeWeight(0.25f)));

    // A state opting out round-trips; the default isn't written.
    AnimatorController c;
    c.Layers[0].States.resize(2);
    c.Layers[0].States[0].Name = "Walk";
    c.Layers[0].States[1].Name = "Drift";
    c.Layers[0].States[1].RootMotion = false;
    AnimatorController back2;
    CHECK(AnimatorController::FromJsonString(c.ToJsonString(), back2));
    CHECK(back2.Layers[0].States[0].RootMotion && !back2.Layers[0].States[1].RootMotion);
    CHECK(c.ToJsonString().find("rootMotion") != std::string::npos);

    // The component options serialize through reflection with stable keys.
    AnimatorControllerComponent ac;
    ac.RootMotion.Mode = (int)RootMotionMode::Apply;
    ac.RootMotion.Bone = "root";
    ac.RootMotion.Rotation = false;
    ac.RootMotion.Vertical = true;
    const RegisteredComponent* reg = nullptr;
    for (const auto& r : ComponentRegistry::All())
        if (std::string(r.Meta.Name) == "Animator Controller") reg = &r;
    CHECK(reg != nullptr);
    if (reg) {
        int found = 0;
        for (const ReflectField& f : reg->Meta.Fields) {
            const std::string key = ReflectFieldKey(f);
            if (key == "rootMotion") { ++found; CHECK(*(int*)f.Address(&ac) == 1); }
            if (key == "rootMotionBone") { ++found; CHECK(*(std::string*)f.Address(&ac) == "root"); }
            if (key == "rootMotionRotation") { ++found; CHECK(!*(bool*)f.Address(&ac)); }
            if (key == "rootMotionVertical") { ++found; CHECK(*(bool*)f.Address(&ac)); }
        }
        CHECK(found == 4);
    }
}

// --- Animator Controller: transitions, exit time, Any state, triggers, JSON round trip (#175) --
void TestAnimatorController() {
    using AC = AnimatorController;
    auto state = [](const char* name, const char* clip, float speed, bool loop) {
        AC::State s;
        s.Name = name;
        s.Motions = {AC::Motion{clip, "", {}}};
        s.Speed = speed;
        s.Loop = loop;
        return s;
    };
    auto tr = [](const char* from, const char* to, std::vector<AC::Condition> conds, bool exit, float exitTime, float dur) {
        AC::Transition t;
        if (std::string(from) == "Any") t.FromKind = AC::Source::Any;
        else if (std::string(from) == "Entry") t.FromKind = AC::Source::Entry;
        else t.From = from;
        t.To = to;
        t.Conditions = std::move(conds);
        t.HasExitTime = exit;
        t.ExitTime = exitTime;
        t.Duration = dur;
        return t;
    };
    AC c;
    c.Parameters = {{"Speed", AC::ParamType::Float, 0.0f}, {"Jump", AC::ParamType::Trigger, 0.0f},
                    {"Grounded", AC::ParamType::Bool, 1.0f}};
    AC::Layer& L = c.Layers[0];
    L.States = {state("Idle", "idle", 1.0f, true), state("Walk", "walk", 1.0f, true),
                state("Run", "run", 1.2f, true), state("Jump", "jump", 1.0f, false)};
    L.DefaultState = "Idle";
    L.Transitions = {tr("Idle", "Walk", {{"Speed", AC::Op::Greater, 0.1f}}, false, 0.9f, 0.2f),
                     tr("Walk", "Idle", {{"Speed", AC::Op::Less, 0.1f}}, false, 0.9f, 0.2f),
                     tr("Walk", "Run", {{"Speed", AC::Op::Greater, 3.0f}}, false, 0.9f, 0.2f),
                     tr("Any", "Jump", {{"Jump", AC::Op::If, 0.0f}}, false, 0.9f, 0.1f),
                     tr("Jump", "Idle", {}, true, 0.95f, 0.2f),
                     tr("Idle", "Run", {}, false, 0.9f, 0.2f)}; // no condition, no exit time: never

    CHECK(L.DefaultStateIndex() == 0);
    CHECK(L.FindState("Run") == 2);
    CHECK(L.FindState("Nope") == -1);

    std::vector<AnimatorParam> p = {{"Speed", 0, 0.0f}, {"Jump", 3, 0.0f}, {"Grounded", 2, 1.0f}};
    CHECK(c.PickTransition(0, 0, 0.0f, p) == -1);            // idle, not moving
    p[0].Value = 1.0f;
    CHECK(c.PickTransition(0, 0, 0.0f, p) == 0);             // Idle -> Walk
    CHECK(c.PickTransition(0, 1, 0.0f, p) == -1);            // walking, not fast enough to run
    p[0].Value = 4.0f;
    CHECK(c.PickTransition(0, 1, 0.0f, p) == 2);             // Walk -> Run
    p[1].Value = 1.0f;                                       // SetTrigger("Jump")
    CHECK(c.PickTransition(0, 2, 0.3f, p) == 3);             // Any -> Jump beats nothing else
    CHECK(p[1].Value == 0.0f);                               // ... and consumed the trigger
    CHECK(c.PickTransition(0, 3, 0.5f, p) == -1);            // Jump waits for its exit time
    CHECK(c.PickTransition(0, 3, 0.96f, p) == 4);            // Jump -> Idle at 95%
    p[1].Value = 1.0f;
    CHECK(c.PickTransition(0, 3, 0.2f, p) == -1);            // Any never re-enters the state it targets
    CHECK(p[1].Value == 1.0f);                               // an unused trigger stays set
    std::vector<AnimatorParam> none;
    CHECK(c.PickTransition(0, 0, 0.0f, none) == -1);         // missing parameters never match
    CHECK(c.PickTransition(0, -1, 0.0f, p) == -1);
    CHECK(c.PickTransition(0, 99, 0.0f, p) == -1);
    CHECK(c.PickTransition(5, 0, 0.0f, p) == -1);            // no such layer

    // ... unless it may transition to itself (Fire spam restarting the Fire state).
    L.Transitions[3].CanTransitionToSelf = true;
    CHECK(c.PickTransition(0, 3, 0.2f, p) == 3);
    L.Transitions[3].CanTransitionToSelf = false;

    // Priority: an Any-State transition that respects it only enters a higher-priority state.
    L.States[3].Priority = 2;
    L.States[2].Priority = 3;                                // "Run" stands in for a reload here
    L.Transitions[3].RespectPriority = true;
    p[1].Value = 1.0f;
    CHECK(c.PickTransition(0, 2, 0.3f, p) == -1);            // Jump (2) can't cut Run (3) short
    CHECK(p[1].Value == 1.0f);
    CHECK(c.PickTransition(0, 0, 0.3f, p) == 3);             // but it does interrupt Idle (0)
    L.States[2].Priority = L.States[3].Priority = 0;
    L.Transitions[3].RespectPriority = false;

    // JSON round trip keeps everything, in format v2.
    L.States[3].Tags = {"Air"};
    L.States[3].Events = {{"Land", 0.9f}};
    L.States[2].SpeedParam = "Speed";
    L.Transitions[4].Interruptible = false;
    L.Transitions[4].Offset = 0.25f;
    AC back;
    CHECK(AC::FromJsonString(c.ToJsonString(), back));
    CHECK(back.Layers.size() == 1 && back.Tracks.size() == 1);
    const AC::Layer& B = back.Layers[0];
    CHECK(back.Parameters.size() == 3 && back.Parameters[1].Type == AC::ParamType::Trigger);
    CHECK(B.States.size() == 4 && B.States[2].Speed == 1.2f && !B.States[3].Loop);
    CHECK(B.States[0].MotionFor(0).Clip == "idle" && B.States[2].SpeedParam == "Speed");
    CHECK(B.States[3].HasTag("Air") && B.States[3].Events.size() == 1 && B.States[3].Events[0].Time == 0.9f);
    CHECK(B.Transitions.size() == 6 && B.Transitions[3].FromKind == AC::Source::Any);
    CHECK(B.Transitions[4].HasExitTime && B.Transitions[4].ExitTime == 0.95f);
    CHECK(!B.Transitions[4].Interruptible && B.Transitions[4].Offset == 0.25f);
    CHECK(B.Transitions[1].Conditions.size() == 1 && B.Transitions[1].Conditions[0].Mode == AC::Op::Less);
    CHECK(B.DefaultState == "Idle");

    // v1 files (flat states, one clip each, "from":"Any") load as a single base layer.
    AC v1;
    CHECK(AC::FromJsonString(R"({"defaultState":"B","parameters":[{"name":"go","type":"trigger"}],
        "states":[{"name":"A","clip":"a.fbx","loop":true},{"name":"B","clip":"b.fbx","speed":2}],
        "transitions":[{"from":"Any","to":"A","conditions":[{"param":"go","mode":"if"}]},
                       {"from":"A","to":"B","hasExitTime":true,"exitTime":1.0}]})", v1));
    CHECK(v1.Layers.size() == 1 && v1.Tracks.size() == 1);
    CHECK(v1.Layers[0].States.size() == 2 && v1.Layers[0].States[1].MotionFor(0).Clip == "b.fbx");
    CHECK(v1.Layers[0].States[1].Speed == 2.0f && v1.Layers[0].DefaultStateIndex() == 1);
    CHECK(v1.Layers[0].Transitions[0].FromKind == AC::Source::Any && v1.Layers[0].Transitions[1].From == "A");

    // Robustness: garbage is rejected, unknown enum names fall back, bad entries are skipped.
    AC junk;
    CHECK(!AC::FromJsonString("{not json", junk));
    CHECK(!AC::FromJsonString("[1,2]", junk));
    CHECK(AC::FromJsonString(R"({"parameters":[{"name":"x","type":"banana"},{"type":"int"}],
        "states":[{"name":"A","speed":"fast"},5],"transitions":[{"from":"A","to":"A",
        "conditions":[{"param":"x","mode":"sideways"}]}]})", junk));
    CHECK(junk.Parameters.size() == 1 && junk.Parameters[0].Type == AC::ParamType::Float);
    CHECK(junk.Layers[0].States.size() == 1 && junk.Layers[0].States[0].Speed == 1.0f);
    CHECK(junk.Layers[0].Transitions.size() == 1 && junk.Layers[0].Transitions[0].Conditions[0].Mode == AC::Op::Greater);

    // The component's setters create parameters on first use and keep their type.
    AnimatorControllerComponent comp;
    comp.SetFloat("Speed", 2.5f);
    comp.SetTrigger("Jump");
    comp.SetBool("Grounded", true);
    CHECK(comp.Params.size() == 3);
    CHECK(comp.GetFloat("Speed") == 2.5f && comp.GetFloat("Jump") == 1.0f && comp.GetFloat("Grounded") == 1.0f);
    comp.ResetTrigger("Jump");
    CHECK(comp.GetFloat("Jump") == 0.0f && comp.GetFloat("Missing") == 0.0f);

    // 1D blend weights: clamp at the ends, split linearly between neighbours, order-independent.
    const std::vector<AC::BlendChild> kids = {{"run", 4.0f, 1.0f}, {"idle", 0.0f, 1.0f}, {"walk", 1.5f, 1.0f}};
    auto w = AnimatorBlendWeights(kids, -1.0f);
    CHECK(w[1] == 1.0f && w[0] == 0.0f && w[2] == 0.0f);
    w = AnimatorBlendWeights(kids, 9.0f);
    CHECK(w[0] == 1.0f);
    w = AnimatorBlendWeights(kids, 2.75f);
    CHECK(std::abs(w[2] - 0.5f) < 1e-5f && std::abs(w[0] - 0.5f) < 1e-5f && w[1] == 0.0f);
    CHECK(AnimatorBlendWeights({}, 1.0f).empty());

    // Bone masks: an included subtree, minus an excluded branch inside it.
    AC::Layer masked;
    masked.MaskInclude = {"spine"};
    masked.MaskExclude = {"arm_r"};
    const std::vector<std::string> names = {"root", "spine", "arm_l", "arm_r", "hand_r", "leg"};
    const std::vector<int> parents = {-1, 0, 1, 1, 3, 0};
    const auto m = AnimatorMaskWeights(masked, names, parents);
    CHECK(m[0] == 0.0f && m[1] == 1.0f && m[2] == 1.0f && m[3] == 0.0f && m[4] == 0.0f && m[5] == 0.0f);
    const auto all = AnimatorMaskWeights(AC::Layer{}, names, parents);
    CHECK(all[0] == 1.0f && all[5] == 1.0f);

    // --- The runtime, without a model: every state lasts 1 s. -------------------------------------
    const auto oneSecond = [](int, int) { return 1.0f; };
    AC run;
    run.Parameters = {{"Go", AC::ParamType::Trigger, 0.0f}, {"Ammo", AC::ParamType::Int, 3.0f}};
    AC::Layer& R = run.Layers[0];
    R.States = {state("Idle", "", 1.0f, true), state("Reload", "", 1.0f, false), state("Fire", "", 1.0f, false)};
    R.States[1].Priority = 3;
    R.States[1].Events = {{"Refill", 1.0f}, {"Start", 0.0f}};
    R.States[2].Priority = 2;
    R.States[0].Events = {{"Loop", 0.5f}};
    R.Transitions = {tr("Any", "Reload", {{"Go", AC::Op::If, 0.0f}, {"Ammo", AC::Op::Less, 30.0f}}, false, 0.0f, 0.1f),
                     tr("Any", "Fire", {{"Go", AC::Op::If, 0.0f}}, false, 0.0f, 0.0f),
                     tr("Reload", "Exit", {}, true, 1.0f, 0.2f),
                     tr("Fire", "Exit", {}, true, 1.0f, 0.0f)};
    R.Transitions[0].RespectPriority = R.Transitions[1].RespectPriority = true;
    AnimatorControllerComponent rc;
    AdvanceAnimator(run, rc, 0.0f, oneSecond);
    CHECK(rc.Started && rc.StateName == "Idle" && rc.Params.size() == 2);
    AdvanceAnimator(run, rc, 0.6f, oneSecond);
    CHECK(rc.EventFired("Loop") && std::abs(rc.StateTime - 0.6f) < 1e-4f);
    AdvanceAnimator(run, rc, 1.0f, oneSecond);
    CHECK(rc.EventFired("Loop"));                            // a looping state's event fires every pass
    rc.SetTrigger("Go");
    AdvanceAnimator(run, rc, 0.01f, oneSecond);
    CHECK(rc.StateName == "Reload" && rc.InTransition);      // crossfading in
    CHECK(rc.EventFired("Start") && !rc.EventFired("Refill"));
    CHECK(rc.GetFloat("Go") == 0.0f);                        // consumed
    rc.SetTrigger("Go");
    AdvanceAnimator(run, rc, 0.3f, oneSecond);
    CHECK(rc.StateName == "Reload");                         // Fire (2) can't interrupt Reload (3)
    CHECK(!rc.InTransition);                                 // the 0.1 s fade finished
    rc.ResetTrigger("Go");
    AdvanceAnimator(run, rc, 0.8f, oneSecond);               // crosses 1.0: the refill lands, then Exit
    CHECK(rc.EventFired("Refill"));
    CHECK(rc.StateName == "Idle" && rc.InTransition);        // back through Entry to the default
    CHECK(rc.Layers[0].Stack.size() == 2);                   // Reload still fading out underneath
    CHECK(rc.History.size() == 2 && rc.History[0].From == "Idle" && rc.History[0].To == "Reload" && rc.History[0].Transition == 0);
    CHECK(rc.History[0].Why.find("Ammo < 30.00 (is 3.00)") != std::string::npos);
    CHECK(rc.History[1].From == "Reload" && rc.History[1].Why == "exit time");
    rc.SetTrigger("Go");
    rc.SetInt("Ammo", 30);                                   // full: Reload's condition fails, Fire wins
    AdvanceAnimator(run, rc, 0.05f, oneSecond);
    CHECK(rc.StateName == "Fire" && !rc.InTransition);       // a 0 s transition cuts the stack
    CHECK(rc.Layers[0].Stack.size() == 1);

    // Non-interruptible transitions block everything until their crossfade ends.
    AC lock;
    lock.Parameters = {{"A", AC::ParamType::Trigger, 0.0f}, {"B", AC::ParamType::Trigger, 0.0f}};
    lock.Layers[0].States = {state("S0", "", 1.0f, true), state("S1", "", 1.0f, true), state("S2", "", 1.0f, true)};
    lock.Layers[0].Transitions = {tr("S0", "S1", {{"A", AC::Op::If, 0.0f}}, false, 0.0f, 0.5f),
                                  tr("S1", "S2", {{"B", AC::Op::If, 0.0f}}, false, 0.0f, 0.1f)};
    lock.Layers[0].Transitions[0].Interruptible = false;
    AnimatorControllerComponent lc;
    AdvanceAnimator(lock, lc, 0.0f, oneSecond);
    lc.SetTrigger("A");
    AdvanceAnimator(lock, lc, 0.1f, oneSecond);
    CHECK(lc.StateName == "S1");
    lc.SetTrigger("B");
    AdvanceAnimator(lock, lc, 0.1f, oneSecond);
    CHECK(lc.StateName == "S1");                             // still fading: B waits
    AdvanceAnimator(lock, lc, 0.5f, oneSecond);
    CHECK(lc.StateName == "S2");                             // fade done, B fires
}

void TestFirstPersonAnimationSet() {
    const std::string valid = R"({
        "armsModel":"models/fps/arms_base.fbx",
        "weaponModel":"models/fps/aks74u_base.fbx",
        "controller":"weapons/aks74u.controller",
        "weaponSocket":"ik_hand_gun",
        "weaponRoot":"root",
        "weaponMountRotation":[0,90,90]
    })";
    FirstPersonAnimationSet set;
    std::string error;
    CHECK(FirstPersonAnimationSet::FromJsonString(valid, set, &error));
    CHECK(error.empty());
    CHECK(set.ArmsModel == "models/fps/arms_base.fbx");
    CHECK(set.WeaponModel == "models/fps/aks74u_base.fbx");
    CHECK(set.Controller == "weapons/aks74u.controller");

    // The weapon rides the arms rig's gun socket instead of being handed the arms' pose.
    CHECK(set.WeaponSocket == "ik_hand_gun");
    CHECK(set.WeaponRoot == "root");
    CHECK(set.WeaponMountRotation.x == 0.0f && set.WeaponMountRotation.y == 90.0f &&
          set.WeaponMountRotation.z == 90.0f);
    // Omitted mount fields stay neutral: no socket means "share the arms' pose", the old
    // behaviour, rather than snapping the weapon to a socket it was never told about.
    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller"})",
        set, &error));
    CHECK(set.WeaponSocket.empty() && set.WeaponRoot.empty());
    CHECK(set.WeaponMountRotation.x == 0.0f && set.WeaponMountRotation.y == 0.0f &&
          set.WeaponMountRotation.z == 0.0f);
    CHECK(FirstPersonAnimationSet::FromJsonString(valid, set, &error)); // back to the full set

    // No controller: the load fails and leaves the live set untouched. (Format v1's flat
    // "clips" list is no longer read.)
    const FirstPersonAnimationSet original = set;
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","clips":[{"name":"Idle","arms":"i.fbx"}]})",
        set, &error));
    CHECK(error.find("controller") != std::string::npos);
    CHECK(set.Controller == original.Controller && set.WeaponSocket == original.WeaponSocket);

    // A mount needs both ends. Naming only one is a typo that would otherwise fall back to the
    // shared pose while looking configured, so it fails the load instead.
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","weaponSocket":"ik_hand_gun","controller":"c.controller"})",
        set, &error));
    CHECK(error.find("weaponRoot") != std::string::npos);
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","weaponRoot":"root","controller":"c.controller"})",
        set, &error));
    CHECK(error.find("weaponSocket") != std::string::npos);

    // Material overrides, keyed by the FBX's material name, round-trip through the v2 writer.
    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller",
            "weaponMaterials":{"aks74u":"m/ak.mat","cartridge":"m/round.mat"}})", set, &error));
    CHECK(set.WeaponMaterials.size() == 2 && set.ArmsMaterials.empty());
    FirstPersonAnimationSet again;
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), again, &error));
    CHECK(again.WeaponMaterials == set.WeaponMaterials);
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","weaponMaterials":{"aks74u":3}})",
        set, &error));
    CHECK(error.find("weaponMaterials.aks74u") != std::string::npos);
}

// The shotgun options (Remington 870): pellets and spread, a per-round reload, a pump worked after every
// round, and a mount with a translation - parsed, written back, and defaulting to the plain rifle's.
void TestFirstPersonShotgunSet() {
    using G = FirstPersonWeaponGameplay;
    FirstPersonAnimationSet set;
    std::string error;
    // An AK-style file: none of it set, and none of it written back.
    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","weaponSocket":"ik_hand_gun","weaponRoot":"root",
            "weaponMountRotation":[0,90,90],"gameplay":{"magazine":30}})", set, &error));
    CHECK(set.Gameplay.Pellets == 1 && set.Gameplay.SpreadHip == 0.0f && set.Gameplay.SpreadAds == 0.0f);
    CHECK(set.Gameplay.Reload == G::ReloadMode::Magazine && !set.Gameplay.CycleAfterShot);
    CHECK(set.WeaponMountOffset == glm::vec3(0.0f));
    {
        const std::string text = set.ToJsonString();
        for (const char* key : {"pellets", "spread", "\"reload\"", "cycle", "weaponMountOffset"})
            CHECK(text.find(key) == std::string::npos);
    }

    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","weaponSocket":"ik_hand_gun","weaponRoot":"Main",
            "weaponMountRotation":[-90,90,90],"weaponMountOffset":[0,0,-0.016],
            "gameplay":{"magazine":6,"allowFullAuto":false,"pellets":8,"spread":{"hip":3.5,"ads":2.2},
                        "reload":"perRound","cycle":{"enabled":true,"delay":0.12}}})", set, &error));
    CHECK(set.Gameplay.Pellets == 8 && std::abs(set.Gameplay.SpreadHip - 3.5f) < 1e-5f && std::abs(set.Gameplay.SpreadAds - 2.2f) < 1e-5f);
    CHECK(set.Gameplay.Reload == G::ReloadMode::PerRound);
    CHECK(set.Gameplay.CycleAfterShot && std::abs(set.Gameplay.CycleDelay - 0.12f) < 1e-5f);
    CHECK(std::abs(set.WeaponMountOffset.z + 0.016f) < 1e-6f);
    FirstPersonAnimationSet back;
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), back, &error));
    CHECK(back.Gameplay.Pellets == 8 && back.Gameplay.Reload == G::ReloadMode::PerRound && back.Gameplay.CycleAfterShot);
    CHECK(std::abs(back.Gameplay.SpreadAds - 2.2f) < 1e-5f && std::abs(back.WeaponMountOffset.z + 0.016f) < 1e-6f);

    // Out-of-range numbers clamp; an unknown reload mode is a typo worth failing the load for.
    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","gameplay":{"pellets":0,"spread":{"hip":-4,"ads":90}}})",
        set, &error));
    CHECK(set.Gameplay.Pellets == 1 && set.Gameplay.SpreadHip == 0.0f && set.Gameplay.SpreadAds == 45.0f);
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","gameplay":{"reload":"tube"}})", set, &error));
    CHECK(error.find("gameplay.reload") != std::string::npos);

    // Pellets stay inside their cone, and fill it (not all bunched at the centre).
    const glm::vec3 bore = glm::normalize(glm::vec3(0.2f, -0.1f, -1.0f));
    CHECK(FirstPersonPelletDirection(bore, 0.0f, 0.7f, 0.3f) == bore);
    float widest = 0.0f;
    bool inside = true;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 d = FirstPersonPelletDirection(bore, 3.0f, (float)(i % 20) / 19.0f, (float)(i / 20) / 20.0f);
        const float deg = glm::degrees(std::acos(std::clamp(glm::dot(d, bore), -1.0f, 1.0f)));
        inside = inside && deg <= 3.0f + 1e-3f && std::abs(glm::length(d) - 1.0f) < 1e-4f;
        widest = std::max(widest, deg);
    }
    CHECK(inside);
    CHECK(widest > 2.9f);
    // Straight up or down the bore still has a cone (no degenerate cross product).
    CHECK(std::abs(glm::degrees(std::acos(glm::dot(FirstPersonPelletDirection(glm::vec3(0, 1, 0), 2.0f, 1.0f, 0.25f),
                                                    glm::vec3(0, 1, 0)))) - 2.0f) < 1e-2f);

    // The setup check asks a per-round / pump weapon's controller for what the driver will use.
    {
        FirstPersonAnimationSet shotgun;
        shotgun.Gameplay.Reload = G::ReloadMode::PerRound;
        shotgun.Gameplay.CycleAfterShot = true;
        AnimatorController bare;
        FirstPersonWeaponCheckInput in;
        in.Set = &shotgun;
        in.Controller = &bare;
        std::string all;
        for (const FPBody::Check& c : FirstPersonWeaponValidate(in)) all += c.Message + "\n";
        CHECK(all.find("LoadRound") != std::string::npos && all.find("LastRound") != std::string::npos &&
              all.find("StopReload") != std::string::npos && all.find("Cycling") != std::string::npos &&
              all.find("'Cycle'") != std::string::npos);
        CHECK(all.find("Refill") == std::string::npos); // a tube has no magazine to refill
    }
}

// The Remington 870's own controller (project/assets/Weapons/Remington870), driven the way
// FirstPersonPresentation drives it - every state 1 s long: the pump after a round, a reload loading
// one shell per LoadRound until LastRound, an empty start chambering its round, the trigger stopping a
// reload after the shell in hand, and a swapped-in weapon drawing out of Holstered.
void TestRemingtonController() {
    namespace K = FirstPersonAnimatorContract;
    const std::string path = ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.controller");
    AnimatorController ctrl;
    if (!AnimatorController::LoadFile(path, ctrl)) {
        std::printf("  (skipped: no %s)\n", path.c_str());
        return;
    }
    constexpr int kMagazine = 6;
    AnimatorControllerComponent ac;
    const auto oneSecond = [](int, int) { return 1.0f; };
    int ammo = kMagazine, loads = 0;
    bool stop = false;
    auto step = [&](float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) {
            ac.SetInt(K::kAmmo, ammo);
            ac.SetBool(K::kLastRound, ammo >= kMagazine - 1);
            ac.SetBool(K::kStopReload, stop);
            AdvanceAnimator(ctrl, ac, 1.0f / 60.0f, oneSecond);
            for (const std::string& e : ac.FiredEvents)
                if (e == K::kEventLoadRound) { ammo = std::min(kMagazine, ammo + 1); ++loads; }
            for (const char* tr : {K::kFire, K::kReload, K::kCycle}) ac.ResetTrigger(tr);
        }
    };
    ac.SetBool(K::kEquipped, true);
    step(0.5f);
    CHECK(ac.StateName == "Idle");

    // The pump: Cycle plays it (tagged Cycling, so no firing), and it returns to Idle.
    ac.SetTrigger(K::kCycle);
    step(0.1f);
    CHECK(ac.StateName == "Pump" && ac.HasTag(K::kTagCycling));
    ac.SetTrigger(K::kFire);
    step(0.1f);
    CHECK(ac.StateName == "Pump"); // a round can't interrupt the pump
    step(1.5f);
    CHECK(ac.StateName == "Idle");

    // Partial: 2 of 6 -> start, three loops, the last shell, back to Idle full.
    ammo = 2;
    loads = 0;
    ac.SetTrigger(K::kReload);
    step(0.1f);
    CHECK(ac.StateName == "ReloadStart" && ac.HasTag(K::kTagReload));
    ac.SetTrigger(K::kCycle); // a pump owed from before waits the reload out
    step(0.1f);
    CHECK(ac.StateName == "ReloadStart");
    step(6.0f);
    CHECK(ammo == kMagazine && loads == 4);
    CHECK(ac.StateName == "Idle");

    // Empty: the empty start chambers one (LoadRound), then loads on.
    ammo = 0;
    loads = 0;
    ac.SetTrigger(K::kReload);
    step(0.1f);
    CHECK(ac.StateName == "ReloadStartEmpty");
    step(1.0f);
    CHECK(ammo == 1);
    // The trigger mid-reload: the shell in hand goes in, then the hand goes back to the pump.
    step(0.3f);
    CHECK(ac.StateName == "ReloadLoop");
    stop = true;
    step(1.0f);
    CHECK(ac.StateName == "ReloadEnd" && ammo == 2);
    stop = false;
    step(1.5f);
    CHECK(ac.StateName == "Idle" && ammo == 2);

    // One short of full: straight from the start to the last shell.
    ammo = kMagazine - 1;
    loads = 0;
    ac.SetTrigger(K::kReload);
    step(1.2f);
    CHECK(ac.StateName == "ReloadLoopEnd");
    step(1.5f);
    CHECK(ammo == kMagazine && loads == 1 && ac.StateName == "Idle");

    // A weapon swapped in starts holstered (hidden) and draws.
    AnimatorControllerComponent swapped;
    swapped.SetBool(K::kEquipped, true);
    CHECK(AnimatorStartInState(ctrl, swapped, "Holstered"));
    CHECK(swapped.StateName == "Holstered" && swapped.HasTag(K::kTagHidden));
    AdvanceAnimator(ctrl, swapped, 1.0f / 60.0f, oneSecond);
    CHECK(swapped.StateName == "Draw");
    CHECK(!AnimatorStartInState(ctrl, swapped, "NoSuchState"));

    // The definition beside it loads, and asks nothing of this controller it lacks.
    FirstPersonAnimationSet set;
    std::string error;
    CHECK(FirstPersonAnimationSet::LoadFile(ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.fpsanim"), set, &error));

    // Every state's clips attach to the rigs they play on - what Play checks before the weapon may
    // start (a one-frame export, say, carries no take and wouldn't). Needs the arms (Quantum pack).
    if (std::filesystem::exists(ProjectPaths::Resolve(set.ArmsModel))) {
        const auto arms = Model::ImportDeferred(ProjectPaths::Resolve(set.ArmsModel), ModelImportSettings{});
        const auto weapon = Model::ImportDeferred(ProjectPaths::Resolve(set.WeaponModel), ModelImportSettings{});
        CHECK(arms && weapon);
        if (arms && weapon) {
            // ResolveAnimationClip's steps, headless: the clip file has a take, and it attaches to the rig.
            std::map<std::string, std::shared_ptr<Model>> clips;
            auto attaches = [&](Model& target, const std::string& ref) {
                auto& src = clips[ref];
                if (!src) src = Model::ImportDeferred(ProjectPaths::Resolve(ref), ModelImportSettings{});
                return src && src->OwnAnimationCount() > 0 && target.AttachClip(*src, 0, ref, ref) >= 0;
            };
            const int at = ctrl.TrackIndex("arms"), wt = ctrl.TrackIndex("weapon");
            for (const auto& st : ctrl.Layers[0].States) {
                const bool armsOk = attaches(*arms, st.MotionFor(at).Clip);
                const bool weaponOk = attaches(*weapon, st.MotionFor(wt).Clip);
                CHECK(armsOk && weaponOk);
                if (!armsOk || !weaponOk) std::printf("  state '%s' can't attach its %s clip\n", st.Name.c_str(), armsOk ? "weapon" : "arms");
            }
            CHECK(arms->NodeIndex(set.WeaponSocket) >= 0 && weapon->NodeIndex(set.WeaponRoot) >= 0);
            bool material = false;
            for (int i = 0; i < weapon->MeshCount(); ++i) material = material || weapon->MeshMaterial(i).Name == set.WeaponMaterials[0].first;
            CHECK(material);
        }
    }
    CHECK(set.Gameplay.Magazine == kMagazine && set.Gameplay.Reload == FirstPersonWeaponGameplay::ReloadMode::PerRound &&
          set.Gameplay.CycleAfterShot && set.Gameplay.Pellets > 1 && !set.Muzzle.Auto);
    FirstPersonWeaponCheckInput in;
    in.Set = &set;
    in.Controller = &ctrl;
    for (const FPBody::Check& c : FirstPersonWeaponValidate(in)) {
        CHECK(c.Level != FPBody::Severity::Warning && c.Level != FPBody::Severity::Error);
        if (c.Level == FPBody::Severity::Warning || c.Level == FPBody::Severity::Error) std::printf("  %s\n", c.Message.c_str());
    }
}

// Animator v2 - the standard first-person graph (BuildFirstPersonController), driven through the
// real controller runtime exactly as FirstPersonPresentation drives it, without a World or Model:
// every state lasts 1 s. This pins the behaviour the old hard-coded C++ state machine had.
void TestFirstPersonAnimationFSM() {
    namespace K = FirstPersonAnimatorContract;
    FirstPersonAnimationSet set;
    set.ArmsModel = "arms.fbx";
    set.WeaponModel = "weapon.fbx";
    const char* withWeapon[] = {"IdleToSprint", "Sprint", "SprintToIdle", "Holster", "Fire", "TacReload",
                                "EmptyReload", "Inspect", "MagCheck", "Melee"};
    for (const char* n : {"Idle", "Walk", "IdleToSprint", "Sprint", "SprintToIdle", "Aim", "Draw", "Holster",
                          "Fire", "TacReload", "EmptyReload", "Inspect", "MagCheck", "Melee", "Regrip"}) {
        FirstPersonAnimationClip c;
        c.Name = n;
        c.ArmsClip = std::string("FP_") + n + ".fbx";
        for (const char* w : withWeapon) if (std::string(w) == n) c.WeaponClip = std::string("W_") + n + ".fbx";
        c.Loop = std::string(n) == "Idle" || std::string(n) == "Walk" || std::string(n) == "Sprint" || std::string(n) == "Aim";
        c.Fade = 0.1f;
        set.Clips.push_back(c);
    }
    set.DefaultState = "Idle";
    const AnimatorController ctrl = BuildFirstPersonController(set);
    CHECK(ctrl.Tracks.size() == 2 && ctrl.Tracks[0] == "arms" && ctrl.Tracks[1] == "weapon");
    const auto& L = ctrl.Layers[0];
    CHECK(L.States.size() == 16); // the 15 clip states + Holstered
    CHECK(L.States[L.FindState("Idle")].MotionFor(1).Empty());               // no weapon Idle clip: bind pose
    CHECK(L.States[L.FindState("Fire")].MotionFor(1).Clip == "W_Fire.fbx");
    CHECK(L.States[L.FindState("Aim")].HasTag(K::kTagAds));
    CHECK(L.States[L.FindState("Holstered")].HasTag(K::kTagHidden));
    CHECK(L.States[L.FindState("Holstered")].MotionFor(0).Clip == "FP_Holster.fbx"); // holds Holster's end

    // Round trip through JSON keeps the graph intact.
    AnimatorController back;
    CHECK(AnimatorController::FromJsonString(ctrl.ToJsonString(), back));
    CHECK(back.Layers[0].States.size() == L.States.size() && back.Layers[0].Transitions.size() == L.Transitions.size());

    AnimatorControllerComponent ac;
    const auto oneSecond = [](int, int) { return 1.0f; };
    std::vector<std::string> events;
    auto run = [&](float seconds) {
        events.clear();
        for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) {
            AdvanceAnimator(ctrl, ac, 1.0f / 60.0f, oneSecond);
            events.insert(events.end(), ac.FiredEvents.begin(), ac.FiredEvents.end());
        }
    };
    auto fired = [&](const char* e) { return (int)std::count(events.begin(), events.end(), std::string(e)); };
    auto input = [&](float speed, bool sprint, bool aim) {
        ac.SetFloat(K::kSpeed, speed);
        ac.SetBool(K::kSprint, sprint);
        ac.SetBool(K::kAim, aim);
    };

    run(0.1f);
    CHECK(ac.StateName == "Idle" && ac.HasTag(K::kTagIdle));
    CHECK(ac.GetFloat(K::kAmmo) == 30.0f && ac.GetFloat(K::kEquipped) == 1.0f); // defaults from the set

    // Locomotion: walk, aim while walking (Aim beats Walk), sprint drops ADS, releasing sprint
    // with aim held goes straight back to the sights - no SprintToIdle on the way.
    input(2.0f, false, false); run(0.2f);
    CHECK(ac.StateName == "Walk");
    input(2.0f, false, true); run(0.2f);
    CHECK(ac.StateName == "Aim" && ac.HasTag(K::kTagAds));
    input(2.0f, true, true); run(0.2f);
    CHECK(ac.StateName == "Sprint");
    input(2.0f, false, true); run(0.05f);
    CHECK(ac.StateName == "Aim");
    input(0.0f, true, true); run(0.2f);
    CHECK(ac.StateName == "Aim");                 // the sprint key alone, standing: still ADS
    input(0.0f, false, false); run(0.2f);
    CHECK(ac.StateName == "Idle");

    // Idle -> Sprint plays the IdleToSprint clip first; leaving Sprint plays SprintToIdle.
    input(2.0f, true, false); run(0.05f);
    CHECK(ac.StateName == "IdleToSprint");
    run(1.1f);
    CHECK(ac.StateName == "Sprint");
    input(0.0f, false, false); run(0.05f);
    CHECK(ac.StateName == "SprintToIdle");
    input(2.0f, true, false); run(0.05f);
    CHECK(ac.StateName == "Sprint");              // re-pressed partway out: straight back
    input(0.0f, false, false); run(0.05f);
    CHECK(ac.StateName == "SprintToIdle");
    run(1.2f);
    CHECK(ac.StateName == "Idle");
    // A tap: let go partway in and it's straight back to rest, not the rest of IdleToSprint.
    input(2.0f, true, false); run(0.05f);
    CHECK(ac.StateName == "IdleToSprint");
    input(2.0f, false, false); run(0.05f);
    CHECK(ac.StateName == "Walk");
    input(0.0f, false, false); run(0.3f);
    CHECK(ac.StateName == "Idle");

    // Hip fire plays Fire (a Shot per trigger, restarting on spam) and returns to Idle.
    ac.SetTrigger(K::kFire); run(0.05f);
    CHECK(ac.StateName == "Fire" && fired(K::kEventShot) == 1);
    ac.SetTrigger(K::kFire); run(0.05f);
    CHECK(ac.StateName == "Fire" && fired(K::kEventShot) == 1); // restarted: a second round
    run(1.2f);
    CHECK(ac.StateName == "Idle");

    // A tactical reload can't be cut short by firing, refills exactly once when it completes,
    // and hands back to the sights when aim is still held.
    ac.SetInt(K::kAmmo, 12);
    ac.SetTrigger(K::kReload); run(0.05f);
    CHECK(ac.StateName == "TacReload" && ac.HasTag(K::kTagReload));
    ac.SetTrigger(K::kFire); run(0.1f);
    CHECK(ac.StateName == "TacReload");
    ac.ResetTrigger(K::kFire);
    ac.SetTrigger(K::kInspect); run(0.1f);
    CHECK(ac.StateName == "TacReload");
    ac.ResetTrigger(K::kInspect);
    input(0.0f, false, true);
    run(1.0f);
    CHECK(fired(K::kEventRefill) == 1);
    CHECK(ac.StateName == "Aim");
    input(0.0f, false, false); run(0.2f);

    // An empty magazine picks EmptyReload.
    ac.SetInt(K::kAmmo, 0);
    ac.SetTrigger(K::kReload); run(0.05f);
    CHECK(ac.StateName == "EmptyReload");

    // Holster outranks a reload (which then never refills), ends hidden, and nothing but Draw
    // leaves it.
    ac.SetBool(K::kEquipped, false); run(0.05f);
    CHECK(ac.StateName == "Holster");
    run(1.2f);
    CHECK(fired(K::kEventRefill) == 0);
    CHECK(ac.StateName == "Holstered" && ac.HasTag(K::kTagHidden));
    ac.SetTrigger(K::kFire); ac.SetTrigger(K::kMelee); run(0.1f);
    CHECK(ac.StateName == "Holstered");
    ac.ResetTrigger(K::kFire); ac.ResetTrigger(K::kMelee);
    ac.SetBool(K::kEquipped, true); run(0.05f);
    CHECK(ac.StateName == "Draw" && !ac.HasTag(K::kTagHidden));
    ac.SetBool(K::kEquipped, false); run(0.3f);
    CHECK(ac.StateName == "Draw");                // Holster can't cut Draw short ...
    run(1.0f);
    CHECK(ac.StateName == "Holster");             // ... it waits for it
    ac.SetBool(K::kEquipped, true); run(1.5f);
    CHECK(ac.StateName == "Draw" || ac.StateName == "Idle");

    // Idle fidget: only from Idle, on the Fidget trigger.
    run(1.2f);
    CHECK(ac.StateName == "Idle");
    ac.SetTrigger(K::kFidget); run(0.05f);
    CHECK(ac.StateName == "Regrip");
    ac.SetTrigger(K::kMelee); run(0.05f);
    CHECK(ac.StateName == "Melee");               // anything real cuts a fidget off

    // A v2 weapon definition names its controller and carries its gameplay numbers.
    FirstPersonAnimationSet v2;
    std::string error;
    CHECK(FirstPersonAnimationSet::FromJsonString(R"({"armsModel":"a.fbx","weaponModel":"w.fbx",
        "controller":"weapons/w.controller","gameplay":{"magazine":20,"rpm":600,"allowFullAuto":false,
        "recoil":{"pitch":2.0}}})", v2, &error));
    CHECK(v2.Controller == "weapons/w.controller");
    CHECK(v2.Gameplay.Magazine == 20 && v2.Gameplay.RoundsPerMinute == 600.0f && !v2.Gameplay.AllowFullAuto);
    // A pre-procedural recoil block becomes curves of the same shape: the 2 degree kick peaks at
    // the old rise time (defaults for what's unspecified), ADS only.
    CHECK(std::fabs(v2.Procedural.Recoil.Duration - (0.035f + 5.0f * 0.08f)) < 1e-5f);
    CHECK(std::fabs(v2.Procedural.Recoil.Rotation.X.Evaluate(0.035f / v2.Procedural.Recoil.Duration) - 2.0f) < 1e-4f);
    CHECK(v2.Procedural.Recoil.HipScale == 0.0f && v2.Procedural.Recoil.PitchRange == glm::vec2(1.0f));
    FirstPersonAnimationSet again;
    CHECK(FirstPersonAnimationSet::FromJsonString(v2.ToJsonString(), again, &error));
    CHECK(again.Controller == v2.Controller && again.Gameplay.Magazine == 20 && !again.Gameplay.AllowFullAuto);
    CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel":"a.fbx","weaponModel":"w.fbx"})", v2, &error));
    CHECK(error.find("controller") != std::string::npos);
    CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel":"a.fbx","weaponModel":"w.fbx",
        "controller":"c.controller","gameplay":{"rpm":0}})", v2, &error));

    // Regrip fidget: 10-20 s of idle, whatever the random sample.
    CHECK(FirstPersonRegripDelay(0.0f) == 10.0f);
    CHECK(FirstPersonRegripDelay(1.0f) == 20.0f);
    CHECK(FirstPersonRegripDelay(-3.0f) == 10.0f && FirstPersonRegripDelay(7.0f) == 20.0f);

    // Reload key: a tap reloads on release, a hold mag-checks once at the threshold.
    {
        using In = FirstPersonReloadInput;
        const float dt = 1.0f / 60.0f;
        FirstPersonReloadButton b;
        CHECK(b.Update(false, dt) == In::None);             // idle
        CHECK(b.Update(true, dt) == In::None);              // pressed
        CHECK(b.Update(true, dt) == In::None);              // still short of the hold
        CHECK(b.Update(false, dt) == In::Reload);           // quick release = tap
        CHECK(b.Update(false, dt) == In::None);             // fires once

        int magChecks = 0;
        CHECK(b.Update(true, dt) == In::None);
        for (int i = 0; i < 60; ++i) magChecks += b.Update(true, dt) == In::MagCheck;
        CHECK(magChecks == 1);                              // one MagCheck per hold, however long
        CHECK(b.Update(false, dt) == In::None);             // releasing a hold doesn't also reload

        // Crosses the threshold on the frame the hold reaches it, not before.
        FirstPersonReloadButton h;
        h.Update(true, 0.0f);
        CHECK(h.Update(true, FirstPersonReloadButton::kHoldSeconds * 0.9f) == In::None);
        CHECK(h.Update(true, FirstPersonReloadButton::kHoldSeconds * 0.2f) == In::MagCheck);
    }
}

// ADS animations: the "ads" settings block (and its pre-block gameplay keys), the generator's
// tags and ADS_<action> variants, and how much of a crossfade stack a carried action owns.
void TestFirstPersonAds() {
    namespace K = FirstPersonAnimatorContract;
    std::string error;
    const std::string head = R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller",)";

    // Legacy: the zoom under gameplay still loads, into the ads block.
    FirstPersonAnimationSet legacy;
    CHECK(FirstPersonAnimationSet::FromJsonString(
        head + R"("gameplay":{"adsZoom":1.5,"adsViewModelZoom":1.2,"adsZoomTime":0.1}})", legacy, &error));
    CHECK(legacy.Ads.Zoom == 1.5f && legacy.Ads.ViewModelZoom == 1.2f && legacy.Ads.ZoomTime == 0.1f);
    CHECK(legacy.Ads.ReferenceState == "Aim" && legacy.Ads.CarryTag == K::kTagAdsCarry);

    // The block wins over the legacy keys, clamps, and round-trips.
    FirstPersonAnimationSet set;
    CHECK(FirstPersonAnimationSet::FromJsonString(
        head + R"("gameplay":{"adsZoom":3.0},"ads":{"zoom":1.3,"viewModelZoom":99,"zoomTime":0.16,
        "referenceState":"Sights","carryTag":"Carry","matchElbows":false,"matchTwist":false,"aimHoldTime":0.3}})",
        set, &error));
    CHECK(set.Ads.Zoom == 1.3f && set.Ads.ViewModelZoom == 4.0f && set.Ads.ZoomTime == 0.16f);
    CHECK(set.Ads.ReferenceState == "Sights" && set.Ads.CarryTag == "Carry");
    CHECK(!set.Ads.MatchElbows && !set.Ads.MatchTwist && set.Ads.AimHoldTime == 0.3f);
    FirstPersonAnimationSet again;
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), again, &error));
    CHECK(again.Ads.Zoom == set.Ads.Zoom && again.Ads.ViewModelZoom == set.Ads.ViewModelZoom &&
          again.Ads.ReferenceState == "Sights" && again.Ads.CarryTag == "Carry" && !again.Ads.MatchElbows &&
          !again.Ads.MatchTwist && again.Ads.AimHoldTime == 0.3f);
    // An empty reference ("first ADS-tagged state") survives the round trip too.
    CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{"referenceState":""}})", set, &error));
    CHECK(set.Ads.ReferenceState.empty());
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), again, &error) && again.Ads.ReferenceState.empty());

    // Every known tag has a description; custom tags don't.
    for (const auto& t : K::kKnownTags) CHECK(K::KnownTagDescription(t.Name) == t.Description);
    CHECK(K::KnownTagDescription("MyTag") == nullptr);

    // The generator: carry and busy tags, and an ADS clip becomes an aim-routed state.
    FirstPersonAnimationSet v1;
    v1.ArmsModel = "arms.fbx";
    v1.WeaponModel = "weapon.fbx";
    for (const char* n : {"Idle", "Walk", "Sprint", "Fire", "Aim", "TacReload", "EmptyReload", "MagCheck", "Inspect",
                          "Melee", "Regrip", "ADS_TacReload"}) {
        FirstPersonAnimationClip c;
        c.Name = n;
        c.ArmsClip = std::string("FP_") + n + ".fbx";
        c.Loop = std::string(n) == "Idle" || std::string(n) == "Aim";
        c.Fade = 0.1f;
        v1.Clips.push_back(c);
    }
    v1.DefaultState = "Idle";
    const AnimatorController ctrl = BuildFirstPersonController(v1);
    const auto& L = ctrl.Layers[0];
    auto state = [&](const char* n) -> const AnimatorController::State& { return L.States[L.FindState(n)]; };
    CHECK(L.FindState("ADS TacReload") >= 0 && L.FindState("ADS MagCheck") < 0 && L.FindState("ADS_TacReload") < 0);
    CHECK(state("TacReload").HasTag(K::kTagAdsCarry) && state("TacReload").HasTag(K::kTagReload));
    CHECK(state("MagCheck").HasTag(K::kTagAdsCarry) && state("MagCheck").HasTag(K::kTagBusy));
    CHECK(state("Inspect").HasTag(K::kTagBusy) && !state("Inspect").HasTag(K::kTagAdsCarry));
    CHECK(state("Walk").HasTag(K::kTagReady) && state("Fire").HasTag(K::kTagReady) && !state("Sprint").HasTag(K::kTagReady));
    CHECK(state("Melee").HasTag(K::kTagBusy) && state("Regrip").HasTag(K::kTagIKOff));
    const auto& adsReload = state("ADS TacReload");
    CHECK(adsReload.HasTag(K::kTagAds) && adsReload.HasTag(K::kTagReload) && !adsReload.HasTag(K::kTagAdsCarry));
    CHECK(adsReload.MotionFor(0).Clip == "FP_ADS_TacReload.fbx" && adsReload.Priority == state("TacReload").Priority);

    AnimatorControllerComponent ac;
    const auto oneSecond = [](int, int) { return 1.0f; };
    auto run = [&](float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) AdvanceAnimator(ctrl, ac, 1.0f / 60.0f, oneSecond);
    };
    run(0.1f);
    ac.SetInt(K::kAmmo, 12);
    ac.SetTrigger(K::kReload); run(0.05f);
    CHECK(ac.StateName == "TacReload");                       // hip: the hip clip
    run(1.5f);
    ac.SetBool(K::kAim, true); run(0.2f);
    CHECK(ac.StateName == "Aim");
    ac.SetTrigger(K::kReload); run(0.05f);
    CHECK(ac.StateName == "ADS TacReload" && ac.HasTag(K::kTagAds)); // aiming: the ADS clip
    run(1.5f);
    CHECK(ac.StateName == "Aim");                             // and back to the sights
    ac.SetTrigger(K::kMagCheck); run(0.05f);
    CHECK(ac.StateName == "MagCheck");                        // no ADS clip: the hip one (carried)
    ac.SetInt(K::kAmmo, 0);
    run(1.5f);
    ac.SetTrigger(K::kReload); run(0.05f);
    CHECK(ac.StateName == "EmptyReload");                     // no ADS_EmptyReload either

    // EvaluateAdsCarry: each crossfade entry fades in over what's beneath it.
    std::vector<AdsCarryAction> actions(1);
    actions[0].State = 2;
    actions[0].T = glm::vec3(0.1f, 0.0f, 0.0f);
    actions[0].R = glm::angleAxis(glm::radians(10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    actions[0].Swivel[0] = 0.4f;
    AnimatorLayerRuntime layer;
    layer.Stack = {{1, 0.0f, 1.0f, 0.0f}};
    CHECK(EvaluateAdsCarry(layer, actions, 0.0f, 1.0f).Action == nullptr); // not playing
    layer.Stack = {{2, 0.0f, 1.0f, 0.0f}};
    AdsCarrySample full = EvaluateAdsCarry(layer, actions, 0.0f, 1.0f);
    CHECK(full.Action == &actions[0] && full.Weight == 1.0f && std::fabs(full.T.x - 0.1f) < 1e-6f &&
          std::fabs(full.Swivel[0] - 0.4f) < 1e-6f);
    CHECK(EvaluateAdsCarry(layer, actions, 0.0f, 0.0f).Weight == 0.0f);    // not aiming: nothing
    CHECK(std::fabs(EvaluateAdsCarry(layer, actions, 0.0f, 0.5f).Weight - 0.5f) < 1e-6f);
    // Leaving the action: Aim fading in on top at half way leaves the action 1 - w(0.5).
    layer.Stack = {{2, 0.9f, 1.0f, 0.3f}, {0, 0.0f, 0.5f, 0.3f}};
    const float w = AnimatorCrossfadeWeight(0.5f);
    AdsCarrySample leaving = EvaluateAdsCarry(layer, actions, 0.0f, 1.0f);
    CHECK(leaving.Action == &actions[0] && std::fabs(leaving.Weight - (1.0f - w)) < 1e-5f);
    CHECK(std::fabs(leaving.T.x - 0.1f * (1.0f - w)) < 1e-6f);
    // dt predicts the fade the controller is about to advance to.
    const float next = AnimatorCrossfadeWeight(0.5f + 0.03f / 0.3f);
    CHECK(std::fabs(EvaluateAdsCarry(layer, actions, 0.03f, 1.0f).Weight - (1.0f - next)) < 1e-5f);
    // Action bones: the left arm by default, a list in the file, empty = carry the whole clip.
    {
        FirstPersonAnimationSet bones;
        CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{}})", bones, &error));
        CHECK(bones.Ads.ActionBones == std::vector<std::string>{"clavicle_l"});
        CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{"actionBones":["clavicle_l","clavicle_r"]}})", bones, &error));
        FirstPersonAnimationSet back;
        CHECK(FirstPersonAnimationSet::FromJsonString(bones.ToJsonString(), back, &error));
        CHECK(back.Ads.ActionBones == (std::vector<std::string>{"clavicle_l", "clavicle_r"}));
        CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{"actionBones":[]}})", bones, &error));
        CHECK(bones.Ads.ActionBones.empty());
        CHECK(FirstPersonAnimationSet::FromJsonString(bones.ToJsonString(), back, &error) && back.Ads.ActionBones.empty());
    }

    // AdsGunMotionMove: the kept share of a clip's own gun motion, about the rear sight.
    {
        auto nearM = [](const glm::mat4& a, const glm::mat4& b) {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (std::fabs(a[c][r] - b[c][r]) > 1e-4f) return false;
            return true;
        };
        const glm::mat4 aim = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.05f, -0.2f));
        const glm::mat4 hip = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, -0.15f, -0.3f)) *
                              glm::mat4_cast(glm::angleAxis(0.3f, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::mat4 now = hip * glm::translate(glm::mat4(1.0f), glm::vec3(0.02f, -0.08f, 0.03f)) *
                              glm::mat4_cast(glm::angleAxis(0.6f, glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f))));
        const glm::vec3 sight(0.0f, 0.0f, -0.25f);
        CHECK(nearM(AdsGunMotionMove(aim, hip, hip, sight, 0.6f, 0.2f), glm::mat4(1.0f)));  // first frame: none
        CHECK(nearM(AdsGunMotionMove(aim, hip, now, sight, 0.0f, 0.0f), glm::mat4(1.0f)));  // nothing kept: locked
        CHECK(nearM(AdsGunMotionMove(aim, hip, now, sight, 1.0f, 1.0f) * aim, aim * glm::inverse(hip) * now)); // all
        // Turn only: the gun turns about the sight, which stays put.
        const glm::mat4 turned = AdsGunMotionMove(aim, hip, now, sight, 0.5f, 0.0f);
        CHECK(glm::length(glm::vec3(turned * glm::vec4(sight, 1.0f)) - sight) < 1e-4f);
        CHECK(!nearM(turned, glm::mat4(1.0f)));
    }
    // Gun motion settings: the reloads and the mag check by default, per state in the file.
    {
        FirstPersonAnimationSet gm;
        CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{}})", gm, &error));
        CHECK(gm.Ads.GunMotionFor("MagCheck") && gm.Ads.GunMotionFor("TacReload") && gm.Ads.GunMotionFor("EmptyReload"));
        CHECK(std::fabs(gm.Ads.GunMotionFor("TacReload")->Rotation - 0.18f) < 1e-6f);
        // A file that lists none keeps none.
        FirstPersonAnimationSet none;
        CHECK(FirstPersonAnimationSet::FromJsonString(head + R"("ads":{"gunMotion":{}}})", none, &error));
        CHECK(none.Ads.GunMotions.empty());
        CHECK(FirstPersonAnimationSet::FromJsonString(
            head + R"("ads":{"gunMotion":{"TacReload":{"rotation":0.3,"position":2}},"sightPivot":0.3}})", gm, &error));
        CHECK(!gm.Ads.GunMotionFor("MagCheck") && gm.Ads.GunMotionFor("TacReload")->Position == 1.0f);
        FirstPersonAnimationSet back;
        CHECK(FirstPersonAnimationSet::FromJsonString(gm.ToJsonString(), back, &error));
        CHECK(back.Ads.GunMotionFor("TacReload") && std::fabs(back.Ads.GunMotionFor("TacReload")->Rotation - 0.3f) < 1e-6f &&
              std::fabs(back.Ads.SightPivot - 0.3f) < 1e-6f);
    }

    // Entering: the action fading in over Aim.
    layer.Stack = {{0, 0.5f, 1.0f, 0.0f}, {2, 0.0f, 0.25f, 0.3f}};
    CHECK(std::fabs(EvaluateAdsCarry(layer, actions, 0.0f, 1.0f).Weight - AnimatorCrossfadeWeight(0.25f)) < 1e-5f);
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

// --- Procedural animation: the first-person weapon stack --------------------------------------
void TestWeaponProcedural() {
    // Settings round-trip through the .fpsanim "procedural" block.
    const WeaponProceduralSettings defaults = WeaponProceduralSettings::Defaults();
    WeaponProceduralSettings loaded;
    std::string error;
    CHECK(WeaponProceduralSettings::FromJson(defaults.ToJson(), loaded, &error));
    CHECK(std::fabs(loaded.Recoil.Rotation.X.Evaluate(0.2f) - defaults.Recoil.Rotation.X.Evaluate(0.2f)) < 1e-4f);
    CHECK(std::fabs(loaded.Bob.Sprint.Z.Evaluate(0.3f) - defaults.Bob.Sprint.Z.Evaluate(0.3f)) < 1e-4f);
    CHECK(loaded.StateOffsets.size() == defaults.StateOffsets.size() && loaded.IK.GunBone == "ik_hand_gun");
    WeaponProceduralSettings untouched = defaults;
    CHECK(!WeaponProceduralSettings::FromJson(json::parse(R"({"recoil":{"duration":0}})"), untouched, &error));
    CHECK(error.find("duration") != std::string::npos);
    CHECK(!WeaponProceduralSettings::FromJson(json::parse(R"({"sway":{"lookRotation":"big"}})"), untouched, &error));
    CHECK(!WeaponProceduralSettings::FromJson(json::parse(R"({"bob":{"walk":{"x":[[0,"a"]]}}})"), untouched, &error));
    CHECK(untouched.Recoil.Duration == defaults.Recoil.Duration); // a bad load changes nothing
    // Partial blocks keep everything they don't name.
    CHECK(WeaponProceduralSettings::FromJson(json::parse(R"({"lean":{"angle":20}})"), untouched, &error));
    CHECK(untouched.Lean.Angle == 20.0f && untouched.Lean.Offset == defaults.Lean.Offset);

    // One layer at a time, everything else off, so each can be read in isolation.
    WeaponProceduralSettings s = defaults;
    s.Sway.Enabled = s.Bob.Enabled = s.Breath.Enabled = s.Lean.Enabled = false;
    s.StateOffsets.clear();
    const auto run = [](WeaponProceduralState& st, const WeaponProceduralSettings& set, WeaponProceduralInput in,
                        float seconds) {
        in.Dt = 1.0f / 120.0f;
        for (float t = 0.0f; t < seconds; t += in.Dt) st.Update(set, in);
        return st.Pose();
    };

    // Recoil: a shot kicks the muzzle up and back, and it settles back to rest.
    {
        WeaponProceduralState st;
        st.OnShot(s, true);
        const WeaponProceduralPose early = run(st, s, {}, 0.06f);
        CHECK(early.Rotation.x > 0.5f && early.Position.z > 0.005f);
        CHECK(early.CameraKick.x > 0.0f);
        const WeaponProceduralPose rest = run(st, s, {}, 1.5f);
        CHECK(std::fabs(rest.Rotation.x) < 0.02f && std::fabs(rest.Position.z) < 1e-4f && st.ActiveShots() == 0);
    }
    // Full auto overlaps shots into a climb bigger than any single kick.
    {
        WeaponProceduralState single, burst;
        single.OnShot(s, true);
        float singlePeak = 0.0f, burstPeak = 0.0f;
        WeaponProceduralInput in;
        in.Dt = 1.0f / 120.0f;
        for (int f = 0; f < 120; ++f) singlePeak = std::max(singlePeak, single.Update(s, in).Rotation.x);
        for (int f = 0; f < 120; ++f) {
            if (f % 10 == 0) burst.OnShot(s, true); // ~720 rpm
            burstPeak = std::max(burstPeak, burst.Update(s, in).Rotation.x);
        }
        CHECK(burstPeak > singlePeak * 1.5f);
    }
    // Aim climb moves the real aim and only its recoverable share comes back; the bolt cycles
    // back and home within its cycle. Off by default (defaults have no climb and no bolt).
    {
        WeaponProceduralSettings a = s;
        a.Recoil.AimPitch = glm::vec2(0.5f);
        a.Recoil.AimYaw = glm::vec2(0.1f);
        a.Recoil.AimRecovery = 0.5f;
        a.Recoil.AimRecoveryDelay = 0.1f;
        a.Recoil.AimRecoverySpeed = 20.0f;
        a.Recoil.BoltCycle = 0.08f;
        a.Recoil.AdsScale = 1.0f;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Dt = 1.0f / 120.0f;
        glm::vec2 aim(0.0f), peak(0.0f);
        float boltPeak = 0.0f;
        for (int f = 0; f < 240; ++f) {
            if (f < 30 && f % 10 == 0) st.OnShot(a, true); // three rounds
            const WeaponProceduralPose& p = st.Update(a, in);
            aim += p.AimKick;
            peak = glm::max(peak, aim);
            if (f < 10) boltPeak = std::max(boltPeak, p.Bolt);
        }
        CHECK(std::fabs(peak.x - 1.5f) < 0.02f && std::fabs(peak.y - 0.3f) < 0.01f);
        CHECK(std::fabs(aim.x - 0.75f) < 0.02f); // half of it recovered
        CHECK(boltPeak > 0.95f && st.Pose().Bolt == 0.0f);
        WeaponProceduralSettings back;
        CHECK(WeaponProceduralSettings::FromJson(a.ToJson(), back, &error));
        CHECK(back.Recoil.AimPitch == a.Recoil.AimPitch && back.Recoil.BoltCycle == a.Recoil.BoltCycle &&
              back.Recoil.AimRecovery == a.Recoil.AimRecovery && !back.Recoil.HipProcedural);
        WeaponProceduralState plain;
        plain.OnShot(s, true);
        CHECK(plain.Update(s, in).AimKick == glm::vec2(0.0f) && plain.Pose().Bolt == 0.0f);
    }
    // A disabled recoil does nothing.
    {
        WeaponProceduralSettings off = s;
        off.Recoil.Enabled = false;
        WeaponProceduralState st;
        st.OnShot(off, true);
        CHECK(st.ActiveShots() == 0);
    }

    // Sway: turning right, the gun lags (yaws left of the view) and settles when the turn stops.
    {
        WeaponProceduralSettings sw = s;
        sw.Sway.Enabled = true;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.LookRate = glm::vec2(200.0f, 0.0f);
        const WeaponProceduralPose turning = run(st, sw, in, 0.5f);
        CHECK(turning.Rotation.y > 0.5f && turning.Position.x < 0.0f);
        CHECK(turning.Rotation.y <= sw.Sway.MaxRotation * 1.5f);
        const WeaponProceduralPose settled = run(st, sw, {}, 3.0f);
        CHECK(std::fabs(settled.Rotation.y) < 0.02f);
    }

    // Per-state offsets blend in on a state name or a tag, and back out.
    {
        WeaponProceduralSettings so = s;
        so.StateOffsets = {{"Sprint", glm::vec3(0.0f, -0.02f, 0.0f), glm::vec3(0.0f, 0.0f, 10.0f), 0.2f, 0.2f}};
        WeaponProceduralState st;
        const std::string sprint = "Sprint", idle = "Idle";
        const std::vector<std::string> noTags, sprintTag = {"Sprint"};
        WeaponProceduralInput in;
        in.StateName = &sprint;
        in.StateTags = &noTags;
        WeaponProceduralPose p = run(st, so, in, 0.5f);
        CHECK(std::fabs(p.Position.y + 0.02f) < 1e-4f && std::fabs(p.Rotation.z - 10.0f) < 1e-3f);
        in.StateName = &idle;
        p = run(st, so, in, 0.5f);
        CHECK(std::fabs(p.Position.y) < 1e-5f);
        in.StateTags = &sprintTag;
        p = run(st, so, in, 0.5f);
        CHECK(std::fabs(p.Position.y + 0.02f) < 1e-4f);
    }

    // IK fades out in states tagged IKOff; the lean rolls the camera; clip rates follow speed.
    {
        WeaponProceduralSettings k = s;
        k.Lean.Enabled = true;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.IKOff = true;
        in.Lean = 1.0f;
        in.WalkSpeed = 6.0f;     // references left at 0 follow the player's own speeds
        in.SprintSpeed = 9.6f;
        in.Velocity = glm::vec3(0.0f, 0.0f, -3.0f);
        const WeaponProceduralPose p = run(st, k, in, 2.0f);
        CHECK(p.IKWeight == 0.0f);
        CHECK(std::fabs(p.CameraRoll - k.Lean.Angle) < 0.05f && std::fabs(p.CameraSide - k.Lean.Offset) < 0.01f);
        CHECK(p.WalkRate == k.Locomotion.MinRate);                   // 3 / 6 m/s, clamped up to the minimum
        in.Velocity = glm::vec3(0.0f, 0.0f, -7.2f);
        CHECK(std::fabs(run(st, k, in, 0.1f).WalkRate - 1.2f) < 1e-4f);
        in.WalkSpeed = 3.5f;     // a scene's own tuning: full walk speed plays at 1x
        in.Velocity = glm::vec3(0.0f, 0.0f, -3.5f);
        CHECK(std::fabs(run(st, k, in, 0.1f).WalkRate - 1.0f) < 1e-4f);
        k.Locomotion.WalkReference = 7.0f; // an explicit reference wins
        CHECK(std::fabs(run(st, k, in, 0.1f).WalkRate - 0.6f) < 1e-4f);
        in.IKOff = false;
        CHECK(run(st, k, in, 0.5f).IKWeight == 1.0f);
    }

    // Bob follows the stride and scales with speed; standing still, it fades away.
    {
        WeaponProceduralSettings b = s;
        b.Bob.Enabled = true;
        b.Bob.HipScale = 1.0f;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Velocity = glm::vec3(0.0f, 0.0f, -3.5f);
        in.Dt = 1.0f / 120.0f;
        float maxSide = 0.0f;
        for (int f = 0; f < 480; ++f) maxSide = std::max(maxSide, std::fabs(st.Update(b, in).Position.x));
        CHECK(maxSide > 0.002f && maxSide < 0.0035f);
        in.Velocity = glm::vec3(0.0f);
        CHECK(std::fabs(run(st, b, in, 2.0f).Position.x) < 1e-4f);
    }

    // Values the Inspector can produce mid-edit never break the stack: zero lengths stay finite,
    // a Min Rate above Max Rate is read in order, and switching IK off keeps the motion (the
    // presentation moves the whole view model instead) rather than fading it away.
    {
        WeaponProceduralSettings z = WeaponProceduralSettings::Defaults();
        z.Recoil.Duration = 0.0f;
        z.Bob.WalkStride = z.Bob.SprintStride = 0.0f;
        z.Bob.WalkFullSpeed = 0.0f;
        z.Breath.Period = 0.0f;
        z.Locomotion.MinRate = 1.3f;
        z.Locomotion.MaxRate = 0.7f;
        z.IK.Enabled = false;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Velocity = glm::vec3(0.0f, 0.0f, -20.0f);
        in.WalkSpeed = 4.0f;
        st.OnShot(z, true);
        in.Dt = 0.0f;                        // a paused frame right after the shot
        st.Update(z, in);
        const WeaponProceduralPose p = run(st, z, in, 0.5f);
        const auto finite = [](const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        CHECK(finite(p.Position) && finite(p.Rotation));
        CHECK(p.WalkRate == 1.3f);           // 20 / 4 m/s, clamped to the larger of the two limits
        CHECK(p.IKWeight == 1.0f);
    }

    // Sprinting straightens up out of a lean unless the weapon allows leaning while sprinting.
    {
        WeaponProceduralSettings l = s;
        l.Lean.Enabled = true;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Lean = -1.0f;
        in.Sprinting = true;
        CHECK(std::fabs(run(st, l, in, 2.0f).CameraRoll) < 1e-3f);
        l.Lean.WhileSprinting = true;
        CHECK(std::fabs(run(st, l, in, 2.0f).CameraRoll + l.Lean.Angle) < 0.05f);
        std::string err;
        WeaponProceduralSettings back = WeaponProceduralSettings::Defaults();
        CHECK(WeaponProceduralSettings::FromJson(l.ToJson(), back, &err) && back.Lean.WhileSprinting);
    }

    // Feel layers: jump and land, the camera, walls, exertion, the per-round punch, soft sway.
    {
        WeaponProceduralSettings f = s;
        f.Recoil.Enabled = false;
        // In the air, rising pulls the gun down and dips the muzzle; a hard landing kicks it
        // down further, and it settles back to rest.
        WeaponProceduralState st;
        WeaponProceduralInput air;
        air.Grounded = false;
        air.VerticalVelocity = 5.0f;
        const WeaponProceduralPose rising = run(st, f, air, 0.5f);
        CHECK(rising.Position.y < -0.005f && rising.Rotation.x < -0.5f);
        air.VerticalVelocity = -8.0f;
        run(st, f, air, 0.5f);
        WeaponProceduralInput ground;
        float lowest = 0.0f, nod = 0.0f;
        for (int i = 0; i < 40; ++i) {
            const WeaponProceduralPose& p = run(st, f, ground, 1.0f / 120.0f);
            lowest = std::min(lowest, p.Position.y);
            nod = std::min(nod, p.CameraKick.x);
        }
        CHECK(lowest < -0.02f && nod < -0.5f); // the gun drops and the view nods
        const WeaponProceduralPose settled = run(st, f, ground, 3.0f);
        CHECK(glm::length(settled.Position) < 1e-3f && std::fabs(settled.CameraOffset.y) < 1e-3f);
        // A step down (below MinImpact) doesn't kick.
        WeaponProceduralState soft;
        air.VerticalVelocity = -1.0f;
        run(soft, f, air, 0.2f);
        float softLow = 0.0f;
        for (int i = 0; i < 40; ++i) softLow = std::min(softLow, run(soft, f, ground, 1.0f / 120.0f).Position.y);
        CHECK(softLow > -0.001f);

        // Walls, stage 1: just inside reach the gun only slides back, as far as the wall is in,
        // still aimed.
        WeaponProceduralState wall;
        WeaponProceduralInput near;
        near.WallDistance = f.Obstruction.Reach - 0.05f;
        const WeaponProceduralPose retracted = run(wall, f, near, 1.5f);
        CHECK(std::fabs(retracted.Position.z - 0.05f) < 2e-3f && retracted.Obstruction < 0.01f &&
              std::fabs(retracted.Rotation.x) < 0.05f);
        // Stage 2: pressed against it, the gun tucks (low ready) and is blocked.
        near.WallDistance = 0.1f;
        const WeaponProceduralPose pushed = run(wall, f, near, 1.5f);
        CHECK(pushed.Obstruction > 0.95f && pushed.Position.z > f.Obstruction.MaxRetract && pushed.Rotation.x < -10.0f);
        CHECK(pushed.Obstruction >= f.Obstruction.BlockAt);
        // A surface that faces up tucks the muzzle up instead.
        WeaponProceduralState table;
        WeaponProceduralInput top = near;
        top.WallFacesUp = true;
        CHECK(run(table, f, top, 1.5f).Rotation.x > 10.0f);
        // An edge on the right of the barrel (its surface facing left) pushes the muzzle left
        // and the gun aside, instead of dropping it; mirrored on the left.
        WeaponProceduralState edgeR, edgeL;
        WeaponProceduralInput byEdge = near;
        byEdge.WallSide = -0.9f;
        const WeaponProceduralPose pushedLeft = run(edgeR, f, byEdge, 1.5f);
        CHECK(pushedLeft.Rotation.y > 10.0f && pushedLeft.Position.x < 0.0f && pushedLeft.Rotation.x > -6.0f);
        byEdge.WallSide = 0.9f;
        const WeaponProceduralPose pushedRight = run(edgeL, f, byEdge, 1.5f);
        CHECK(pushedRight.Rotation.y < -10.0f && pushedRight.Position.x > 0.0f);
        near.WallDistance = -1.0f;
        CHECK(run(wall, f, near, 1.5f).Obstruction < 0.01f);
        near.WallDistance = f.Obstruction.Reach + 0.1f; // beyond reach: nothing
        CHECK(run(wall, f, near, 1.5f).Obstruction < 0.01f);

        // Head bob and strafe roll: only while moving on the ground.
        WeaponProceduralSettings fb = f;
        fb.Bob.Enabled = true;
        WeaponProceduralState cam;
        WeaponProceduralInput walk;
        walk.Velocity = glm::vec3(3.0f, 0.0f, 0.0f);
        float dip = 0.0f, roll = 0.0f;
        for (int i = 0; i < 240; ++i) {
            const WeaponProceduralPose& p = run(cam, fb, walk, 1.0f / 120.0f);
            dip = std::min(dip, p.CameraOffset.y);
            roll = std::min(roll, p.CameraRoll);
        }
        CHECK(dip < -0.002f && roll < -0.5f); // strafing right rolls into it
        WeaponProceduralInput still;
        const WeaponProceduralPose rest = run(cam, fb, still, 3.0f);
        CHECK(std::fabs(rest.CameraOffset.y) < 1e-4f && std::fabs(rest.CameraRoll) < 0.02f);

        // Exertion: breathing gets bigger after a sprint, and recovers.
        WeaponProceduralSettings b = f;
        b.Breath.Enabled = true;
        b.Bob.Enabled = b.CameraMotion.Enabled = false;
        auto breathSpan = [&](WeaponProceduralState& bs, WeaponProceduralInput in2) {
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < 600; ++i) {
                const float y = run(bs, b, in2, 1.0f / 120.0f).Position.y;
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
            return hi - lo;
        };
        WeaponProceduralState calm, winded;
        const float calmSpan = breathSpan(calm, still);
        WeaponProceduralInput sprint;
        sprint.Sprinting = true;
        run(winded, b, sprint, 6.0f);
        CHECK(breathSpan(winded, still) > calmSpan * 1.4f);

        // The per-round punch: a roll and an FOV pulse that spring back to nothing.
        WeaponProceduralSettings pr = s;
        WeaponProceduralState ps;
        ps.OnShot(pr, false);
        float peakFov = 0.0f, peakRoll = 0.0f;
        for (int i = 0; i < 30; ++i) {
            const WeaponProceduralPose& p = run(ps, pr, still, 1.0f / 120.0f);
            peakFov = std::max(peakFov, p.FovKick);
            peakRoll = std::max(peakRoll, std::fabs(p.CameraRoll));
        }
        CHECK(peakFov > pr.Recoil.FovPunch * pr.Recoil.HipScale * 0.3f && peakRoll > 0.03f);
        CHECK(std::fabs(run(ps, pr, still, 2.0f).FovKick) < 1e-3f);

        // Sway eases into its limit instead of passing it, however hard the flick.
        WeaponProceduralSettings sw = f;
        sw.Sway.Enabled = true;
        WeaponProceduralState ss;
        WeaponProceduralInput flick;
        flick.LookRate = glm::vec2(5000.0f, 0.0f);
        const WeaponProceduralPose swung = run(ss, sw, flick, 1.0f);
        CHECK(std::fabs(swung.Rotation.y) <= sw.Sway.MaxRotation + 1e-3f && std::fabs(swung.Rotation.y) > sw.Sway.MaxRotation * 0.9f);

        // Lean: the head arcs over (slides Offset, drops a little, rolls Angle); the gun's roll
        // lags behind it early on.
        {
            WeaponProceduralSettings ls = f;
            ls.Lean.Enabled = true;
            WeaponProceduralState lst;
            WeaponProceduralInput right;
            right.Lean = 1.0f;
            const WeaponProceduralPose early = run(lst, ls, right, 0.08f);
            const float headFrac = early.CameraRoll / ls.Lean.Angle;
            const float gunFrac = -early.Rotation.z / ls.Lean.WeaponRoll;
            CHECK(headFrac > 0.02f && gunFrac < headFrac);
            const WeaponProceduralPose full = run(lst, ls, right, 3.0f);
            const float radius = ls.Lean.Offset / std::sin(glm::radians(ls.Lean.Angle));
            CHECK(std::fabs(full.CameraSide - ls.Lean.Offset) < 2e-3f);
            CHECK(std::fabs(full.CameraOffset.y + radius * (1.0f - std::cos(glm::radians(ls.Lean.Angle)))) < 2e-3f);
            CHECK(std::fabs(full.CameraRoll - ls.Lean.Angle) < 0.1f && std::fabs(full.Rotation.z + ls.Lean.WeaponRoll) < 0.1f);
        }

        // The new blocks round-trip.
        WeaponProceduralSettings edited = defaults;
        edited.Jump.LandPitch = 2.5f;
        edited.CameraMotion.SprintBob = glm::vec2(0.02f, 0.9f);
        edited.Obstruction.Reach = 1.1f;
        edited.Breath.ExertionScale = 3.0f;
        edited.Recoil.FovPunch = -0.4f;
        edited.Sway.LookSmoothing = 9.0f;
        WeaponProceduralSettings back = WeaponProceduralSettings::Defaults();
        std::string err;
        CHECK(WeaponProceduralSettings::FromJson(edited.ToJson(), back, &err));
        CHECK(back.Jump.LandPitch == 2.5f && back.CameraMotion.SprintBob == glm::vec2(0.02f, 0.9f) &&
              back.Obstruction.Reach == 1.1f && back.Breath.ExertionScale == 3.0f && back.Recoil.FovPunch == -0.4f &&
              back.Sway.LookSmoothing == 9.0f);
    }
}

// --- Procedural animation: curves and IK -----------------------------------------------------
// Bullet holes stay on what was hit: a world-space hole stays put, one in an entity follows it
// (moved and turned), a destroyed entity takes its holes, and past capacity the oldest go.
// Spent cases are only ever removed out of the camera's sight: once left behind, or oldest first when
// too many have piled up. The .fpsanim eject block round-trips.
void TestShellCasings() {
    using L = ShellCasings::LifeInput;
    ShellCasings::Settings s;
    s.DespawnDistance = 10.0f;
    s.SoftCap = 3;
    s.HardCap = 5;
    const glm::vec3 player(0.0f);
    auto has = [](const std::vector<int>& v, int i) { return std::find(v.begin(), v.end(), i) != v.end(); };
    // Near: kept, seen or not. Far: kept while seen, removed once not.
    {
        const std::vector<L> cases = {{{1, 0, 0}, false, 0}, {{20, 0, 0}, true, 1}, {{20, 0, 0}, false, 2}, {{2, 0, 0}, true, 3}};
        const auto r = ShellCasings::PickRemovals(cases, player, s);
        CHECK(r.size() == 1);
        CHECK(has(r, 2));
    }
    // Over the soft cap: the oldest unseen go first; seen ones stay even when older.
    {
        const std::vector<L> cases = {{{1, 0, 0}, true, 0}, {{1, 0, 0}, false, 1}, {{1, 0, 0}, false, 2},
                                      {{1, 0, 0}, false, 3}, {{1, 0, 0}, true, 4}};
        const auto r = ShellCasings::PickRemovals(cases, player, s);
        CHECK(r.size() == 2);
        CHECK(has(r, 1) && has(r, 2));
        CHECK(!has(r, 0) && !has(r, 4));
    }
    // All in view: nothing goes below the hard cap ...
    {
        std::vector<L> cases;
        for (int i = 0; i < 5; ++i) cases.push_back({{30.0f, 0, 0}, true, (std::uint64_t)i});
        CHECK(ShellCasings::PickRemovals(cases, player, s).empty());
        // ... and past it, only the oldest.
        cases.push_back({{30.0f, 0, 0}, true, 5});
        const auto r = ShellCasings::PickRemovals(cases, player, s);
        CHECK(r.size() == 1 && has(r, 0));
    }
    // The view test: ahead in, behind out, off to the side out, a sphere straddling the edge in.
    {
        const glm::vec3 eye(0.0f), front(0, 0, -1), right(1, 0, 0), up(0, 1, 0);
        CHECK(ShellCasings::InView(eye, front, right, up, 60.0f, 1.0f, {0, 0, -5}, 0.02f));
        CHECK(!ShellCasings::InView(eye, front, right, up, 60.0f, 1.0f, {0, 0, 5}, 0.02f));
        CHECK(!ShellCasings::InView(eye, front, right, up, 60.0f, 1.0f, {10, 0, -5}, 0.02f));
        const float edge = 5.0f * std::tan(glm::radians(30.0f));
        CHECK(ShellCasings::InView(eye, front, right, up, 60.0f, 1.0f, {edge + 0.01f, 0, -5}, 0.05f));
        CHECK(ShellCasings::InView(eye, front, right, up, 60.0f, 1.0f, {0, 0, 0.01f}, 0.05f)); // at the eye
    }
    // eject: parsed, clamped, and written back.
    {
        const std::string text = R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c.controller", "eject": {"model": "assets/c.fbx", "material": "assets/c.mat",
            "origin": [0.1, 0.2, 0.3], "direction": [2, 0, 0], "speed": 99, "trigger": "event"}})";
        FirstPersonAnimationSet set;
        std::string why;
        CHECK(FirstPersonAnimationSet::FromJsonString(text, set, &why));
        CHECK(set.Eject.Enabled && set.Eject.Model == "assets/c.fbx" && set.Eject.Material == "assets/c.mat");
        CHECK(set.Eject.Direction == glm::vec3(1, 0, 0));
        CHECK(set.Eject.Speed == 50.0f);
        CHECK(set.Eject.When == FirstPersonEjectSettings::Trigger::Event);
        FirstPersonAnimationSet back;
        CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), back, &why));
        CHECK(back.Eject.Model == set.Eject.Model && back.Eject.Origin == set.Eject.Origin &&
              back.Eject.When == FirstPersonEjectSettings::Trigger::Event);
        FirstPersonAnimationSet none;
        CHECK(FirstPersonAnimationSet::FromJsonString(R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c.controller"})", none, &why));
        CHECK(!none.Eject.Enabled);
        CHECK(json::parse(none.ToJsonString()).count("eject") == 0);
        FirstPersonAnimationSet bad;
        CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c", "eject": {"trigger": "shot"}})", bad, &why));
        CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c", "eject": {"model": "m.fbx", "trigger": "pump"}})", bad, &why));
    }
}

void TestBulletHoles() {
    auto near = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; };
    World world;
    BulletHoleList holes;
    std::vector<BulletHoleList::Placed> out;
    holes.Add(world, entt::null, glm::vec3(1, 2, 3), glm::vec3(0, 0, 2), 0.003f);
    holes.Resolve(world, out);
    CHECK(out.size() == 1);
    CHECK(std::abs(out[0].Radius - 0.003f) < 1e-7f); // each hole keeps the size its weapon gave it
    CHECK(near(out[0].Position, glm::vec3(1, 2, 3)));
    CHECK(near(out[0].Normal, glm::vec3(0, 0, 1)));
    CHECK(std::abs(glm::dot(out[0].Tangent, out[0].Normal)) < 1e-4f);

    const entt::entity crate = world.Registry.create();
    auto& t = world.Registry.emplace<TransformComponent>(crate);
    t.Position = glm::vec3(10, 0, 0);
    t.Scale = glm::vec3(2.0f);
    world.RebuildWorldTransformCache();
    holes.Add(world, crate, glm::vec3(11, 0, 0), glm::vec3(1, 0, 0)); // its +X face
    // Knocked over: moved and turned 90 degrees about Y, so +X now faces -Z.
    t.Position = glm::vec3(0, 0, -5);
    t.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    world.RebuildWorldTransformCache();
    holes.Resolve(world, out);
    CHECK(out.size() == 2);
    CHECK(near(out[1].Position, glm::vec3(0, 0, -6)));
    CHECK(near(out[1].Normal, glm::vec3(0, 0, -1)));
    CHECK(std::abs(glm::dot(out[1].Tangent, out[1].Normal)) < 1e-4f);

    world.Registry.destroy(crate);
    holes.Resolve(world, out);
    CHECK(out.size() == 1 && holes.Size() == 1);

    for (std::size_t i = 0; i < BulletHoleList::kCapacity + 10; ++i)
        holes.Add(world, entt::null, glm::vec3((float)i, 0, 0), glm::vec3(0, 1, 0));
    CHECK(holes.Size() == BulletHoleList::kCapacity);
    holes.Resolve(world, out);
    bool hasNewest = false, hasOldest = false;
    for (const auto& h : out) {
        if (near(h.Position, glm::vec3((float)(BulletHoleList::kCapacity + 9), 0, 0))) hasNewest = true;
        if (near(h.Position, glm::vec3(1, 2, 3))) hasOldest = true;
    }
    CHECK(hasNewest && !hasOldest);
    holes.Clear();
    CHECK(holes.Size() == 0);
}

// Zeroing settings: a zero distance and a saved sight line survive a save/load; no sight line
// stays none, and a zero-length direction is rejected as none.
void TestWeaponZero() {
    FirstPersonAnimationSet set;
    std::string error;
    CHECK(FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"w.controller"})", set, &error));
    set.Gameplay.ZeroDistance = 50.0f;
    set.Gameplay.HasSightLine = true;
    set.Gameplay.SightOrigin = glm::vec3(0.28f, 0.058f, 0.0f);
    set.Gameplay.SightDirection = glm::normalize(glm::vec3(-1.0f, -0.004f, 0.0f));
    auto roundTrip = [&](const FirstPersonAnimationSet& in, FirstPersonAnimationSet& out) {
        return FirstPersonAnimationSet::FromJsonString(in.ToJsonString(), out, &error);
    };
    FirstPersonAnimationSet back;
    const bool loaded = roundTrip(set, back);
    CHECK(loaded);
    if (!loaded) return;
    CHECK(std::abs(back.Gameplay.ZeroDistance - 50.0f) < 1e-4f);
    CHECK(back.Gameplay.HasSightLine);
    CHECK(glm::length(back.Gameplay.SightOrigin - set.Gameplay.SightOrigin) < 1e-5f);
    CHECK(glm::length(back.Gameplay.SightDirection - set.Gameplay.SightDirection) < 1e-5f);
    set.Gameplay.HasSightLine = false;
    FirstPersonAnimationSet none;
    CHECK(roundTrip(set, none) && !none.Gameplay.HasSightLine);

    // The barrel and laser: defaults, then a hand-set muzzle, a laser and a hole size round trip.
    CHECK(none.Muzzle.Auto && none.Laser.Enabled && std::abs(none.Gameplay.BulletHoleRadius - 0.012f) < 1e-6f);
    set.Muzzle.Auto = false;
    set.Muzzle.Origin = glm::vec3(0.0f, 0.05f, -0.4f);
    set.Muzzle.Direction = glm::vec3(0.0f, 0.0f, -2.0f);
    set.Laser.Enabled = false;
    set.Laser.Color = glm::vec3(0.02f, 1.0f, 0.05f);
    set.Laser.BeamBrightness = 0.5f;
    set.Laser.SpotBrightness = 4.0f;
    set.Gameplay.BulletHoleRadius = 0.0028f;
    FirstPersonAnimationSet barrel;
    CHECK(roundTrip(set, barrel));
    CHECK(!barrel.Muzzle.Auto && glm::length(barrel.Muzzle.Origin - set.Muzzle.Origin) < 1e-5f);
    CHECK(glm::length(barrel.Muzzle.Direction - glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-5f); // normalized on load
    CHECK(!barrel.Laser.Enabled && glm::length(barrel.Laser.Color - set.Laser.Color) < 1e-5f);
    CHECK(std::abs(barrel.Laser.BeamBrightness - 0.5f) < 1e-5f && std::abs(barrel.Laser.SpotBrightness - 4.0f) < 1e-5f);
    CHECK(std::abs(barrel.Gameplay.BulletHoleRadius - 0.0028f) < 1e-6f);
    // A hand-set muzzle needs a direction.
    FirstPersonAnimationSet bad;
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"w.controller","muzzle":{"auto":false,"direction":[0,0,0]}})",
        bad, &error));

    // What Play found is kept per weapon file, whatever the path spelling.
    FirstPersonBarrelReport report;
    report.Detected = report.HasMuzzle = true;
    report.DetectedOrigin = glm::vec3(1.0f, 2.0f, 3.0f);
    PublishBarrelReport("Assets/FPS/Test/../Test/Gun.fpsanim", report);
    const FirstPersonBarrelReport* found = FindBarrelReport("assets/fps/test/gun.fpsanim");
    CHECK(found && found->Detected && glm::length(found->DetectedOrigin - report.DetectedOrigin) < 1e-6f);
    CHECK(!FindBarrelReport("assets/fps/test/other.fpsanim"));
}

void TestCurve() {
    auto near = [](float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; };
    Curve empty;
    CHECK(empty.Evaluate(0.3f) == 0.0f);
    CHECK(Curve::Constant(2.5f).Evaluate(-4.0f) == 2.5f && Curve::Constant(2.5f).Evaluate(9.0f) == 2.5f);

    const Curve line = Curve::Line(0.0f, 0.0f, 2.0f, 4.0f);
    CHECK(near(line.Evaluate(1.0f), 2.0f));          // a line stays a line under Hermite
    CHECK(near(line.Evaluate(0.5f), 1.0f));
    CHECK(line.Evaluate(-1.0f) == 0.0f && line.Evaluate(3.0f) == 4.0f); // clamped, not extrapolated
    CHECK(line.Evaluate(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

    const Curve ease = Curve::EaseInOut();
    CHECK(near(ease.Evaluate(0.5f), 0.5f));
    CHECK(ease.Evaluate(0.05f) < 0.05f);             // flat start
    CHECK(ease.Evaluate(0.95f) > 0.95f);             // flat end

    const Curve kick = Curve::Kick(0.1f);
    CHECK(near(kick.Evaluate(0.1f), 1.0f));
    CHECK(near(kick.Evaluate(1.0f), 0.0f));
    CHECK(kick.Evaluate(0.05f) > 0.3f);              // it rises fast

    // AddKey keeps the shape it lands on.
    Curve edited = ease;
    const float before = edited.Evaluate(0.3f);
    const int k = edited.AddKey(0.3f);
    CHECK(edited.Keys.size() == 3 && k == 1);
    CHECK(near(edited.Evaluate(0.3f), before));

    Curve round;
    CHECK(Curve::FromJson(kick.ToJson(), round));
    CHECK(round.Keys.size() == kick.Keys.size());
    CHECK(near(round.Evaluate(0.37f), kick.Evaluate(0.37f), 1e-3f));
    Curve untouched = line;
    CHECK(!Curve::FromJson(json::parse(R"([[0,1],["x",2]])"), untouched));
    CHECK(!Curve::FromJson(json::parse(R"({"t":1})"), untouched));
    CHECK(untouched.Keys.size() == 2);               // a bad load leaves the curve alone
    CHECK(Curve::FromJson(json::parse(R"([[1,5],[0,3]])"), untouched) && untouched.Keys[0].Time == 0.0f); // sorted
}

void TestIKSolver() {
    // root (scaled, as imported rigs are) -> upper -> lower -> end, plus a child of the end.
    IK::Pose pose(5);
    const std::vector<int> parents = {-1, 0, 1, 2, 3};
    pose[0].S = glm::vec3(0.5f);
    pose[0].R = glm::angleAxis(0.4f, glm::normalize(glm::vec3(1, 2, 3)));
    pose[1].T = glm::vec3(0.0f, 1.0f, 0.0f);
    pose[2].T = glm::vec3(1.0f, 0.0f, 0.0f);
    pose[2].R = glm::angleAxis(0.5f, glm::vec3(0, 0, 1)); // a bent elbow
    pose[3].T = glm::vec3(1.0f, 0.0f, 0.0f);
    pose[4].T = glm::vec3(0.2f, 0.0f, 0.0f);
    std::vector<glm::mat4> g;
    IK::ComputeGlobals(pose, parents, g);
    const glm::vec3 a = IK::Position(g[1]), b = IK::Position(g[2]), c = IK::Position(g[3]);
    const float reach = glm::length(b - a) + glm::length(c - b);
    const glm::vec3 bendAxis = glm::normalize(glm::cross(c - a, b - a));
    const glm::vec3 childOffset = glm::inverse(IK::Rotation(g[3])) * (IK::Position(g[4]) - c);

    // Reachable: the end lands on the target, bone lengths hold, the elbow bends the same way.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::vec3 target = a + glm::normalize(glm::vec3(0.4f, -0.3f, 0.5f)) * (reach * 0.7f);
        const glm::quat targetRot = glm::angleAxis(1.0f, glm::normalize(glm::vec3(0, 1, 1)));
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, target, &targetRot, 1.0f));
        const glm::vec3 na = IK::Position(gg[1]), nb = IK::Position(gg[2]), nc = IK::Position(gg[3]);
        CHECK(glm::length(nc - target) < 1e-4f);
        CHECK(std::fabs(glm::length(nb - na) - glm::length(b - a)) < 1e-4f);
        CHECK(std::fabs(glm::length(nc - nb) - glm::length(c - b)) < 1e-4f);
        CHECK(glm::length(na - a) < 1e-5f);                    // the shoulder never moves
        CHECK(std::fabs(glm::dot(IK::Rotation(gg[3]), targetRot)) > 1.0f - 1e-5f);
        // The child rides the end rigidly.
        CHECK(glm::length(glm::inverse(IK::Rotation(gg[3])) * (IK::Position(gg[4]) - nc) - childOffset) < 1e-4f);
    }
    // Same bend side: re-solving onto the end's own spot changes nothing, so the elbow is never
    // flipped through the limb.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, c, nullptr, 1.0f));
        CHECK(glm::length(IK::Position(gg[2]) - b) < 1e-4f);
        CHECK(glm::dot(glm::normalize(glm::cross(IK::Position(gg[3]) - a, IK::Position(gg[2]) - a)), bendAxis) > 0.999f);
    }
    // A tiny reach still lands: a target swung ~2e-4 rad off the current a->c line (a hand
    // holding a gun through the idle) must be met exactly, not left short by a "near parallel"
    // cut-off - that was the left hand flickering on the AK's handguard.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::vec3 ac = c - a;
        const glm::vec3 side = glm::normalize(glm::cross(ac, bendAxis));
        const glm::vec3 target = c + side * (glm::length(ac) * 2e-4f);
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, target, nullptr, 1.0f));
        CHECK(glm::length(IK::Position(gg[3]) - target) < glm::length(ac) * 2e-5f);
    }
    // Swivel turns the solved elbow about the root->target line by exactly that angle, and the
    // end still lands on the target.
    {
        IK::Pose p0 = pose, p1 = pose;
        std::vector<glm::mat4> g0 = g, g1 = g;
        const glm::vec3 target = a + glm::normalize(glm::vec3(0.4f, -0.3f, 0.5f)) * (reach * 0.7f);
        CHECK(IK::SolveTwoBone(p0, parents, g0, 1, 2, 3, target, nullptr, 1.0f));
        CHECK(IK::SolveTwoBone(p1, parents, g1, 1, 2, 3, target, nullptr, 1.0f, 0.3f));
        CHECK(glm::length(IK::Position(g1[3]) - target) < 1e-4f);
        const glm::vec3 n = glm::normalize(target - a);
        glm::vec3 e0 = IK::Position(g0[2]) - a, e1 = IK::Position(g1[2]) - a;
        e0 -= n * glm::dot(e0, n);
        e1 -= n * glm::dot(e1, n);
        CHECK(std::fabs(std::atan2(glm::dot(n, glm::cross(e0, e1)), glm::dot(e0, e1)) - 0.3f) < 1e-3f);
    }
    // Out of reach: the chain straightens toward the target instead of breaking.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::vec3 dir = glm::normalize(glm::vec3(-1.0f, 0.2f, 0.1f));
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, a + dir * reach * 3.0f, nullptr, 1.0f));
        const glm::vec3 nc = IK::Position(gg[3]);
        CHECK(glm::dot(glm::normalize(nc - a), dir) > 0.9999f);
        CHECK(std::fabs(glm::length(nc - a) - reach) < reach * 1e-3f);
    }
    // Weight 0 is the input pose; weight 0.5 goes halfway.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::vec3 target = a + glm::vec3(0.3f, 0.3f, 0.3f);
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, target, nullptr, 0.0f));
        CHECK(glm::length(IK::Position(gg[3]) - c) < 1e-6f);
        CHECK(IK::SolveTwoBone(p, parents, gg, 1, 2, 3, target, nullptr, 0.5f));
        CHECK(glm::length(IK::Position(gg[3]) - glm::mix(c, target, 0.5f)) < 1e-4f);
    }
    // A zero-length bone is refused, and bad indices are.
    {
        IK::Pose p = pose;
        p[2].T = glm::vec3(0.0f);
        std::vector<glm::mat4> gg;
        IK::ComputeGlobals(p, parents, gg);
        CHECK(!IK::SolveTwoBone(p, parents, gg, 1, 2, 3, a, nullptr, 1.0f));
        CHECK(!IK::SolveTwoBone(p, parents, gg, 1, 2, 99, a, nullptr, 1.0f));
    }
    // OffsetBone moves a bone and its subtree rigidly, rotating about the pivot.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::quat spin = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
        const glm::vec3 pivot = c + glm::vec3(0.1f, 0.0f, 0.0f);
        IK::OffsetBone(p, parents, gg, 3, glm::vec3(0.0f, 0.2f, 0.0f), spin, pivot);
        CHECK(glm::length(IK::Position(gg[3]) - (pivot + spin * (c - pivot) + glm::vec3(0, 0.2f, 0))) < 1e-4f);
        CHECK(glm::length(glm::inverse(IK::Rotation(gg[3])) * (IK::Position(gg[4]) - IK::Position(gg[3])) - childOffset) < 1e-4f);
        CHECK(glm::length(IK::Position(gg[2]) - b) < 1e-6f); // the parent is untouched
    }
    // AimBone turns its axis onto the target, within the limit.
    {
        IK::Pose p = pose;
        std::vector<glm::mat4> gg = g;
        const glm::vec3 target = c + glm::vec3(0.0f, 0.0f, 2.0f);
        IK::AimBone(p, parents, gg, 3, glm::vec3(1, 0, 0), target, 180.0f, 1.0f);
        const glm::vec3 axis = IK::Rotation(gg[3]) * glm::vec3(1, 0, 0);
        CHECK(glm::dot(glm::normalize(axis), glm::normalize(target - IK::Position(gg[3]))) > 0.9999f);
    }
}

} // namespace

// --- Importing an asset pack laid out <Asset>/Models/x.fbx + <Asset>/Textures/.../x_*.png -----
// The Quantum Hawaiian shirt's loose short sleeve over the arms: the skin Hidden works out in the bind pose
// must stay under the cloth once the arms swing down (both meshes CPU-skinned, every bone under upperarm_l/_r
// turned about its shoulder). With a one-ring edge, 13-30 hidden arm vertices came out as holes. Needs the pack.
void TestOutfitEdgeBandPosed() {
    const char* armsPath = "assets/Characters/Quantum/FirstPerson/Quantum_Arms_FP.fbx";
    const char* shirtPath = "assets/Characters/Quantum/Models/Clothing/Male/Tops/SKM_Shirt_Hawaii.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(armsPath)) || !std::filesystem::exists(ProjectPaths::Resolve(shirtPath))) {
        Log::Info("OutfitEdgeBandPosed: no Quantum pack - skipped.");
        return;
    }
    // The model's geometry with its upper arms turned down by `deg` (0 = the bind pose).
    auto posed = [](const char* path, float deg, OutfitCoverage::Mesh& out) {
        auto model = Model::ImportDeferred(ProjectPaths::Resolve(path), ModelImportSettings{});
        if (!model) return false;
        out = {};
        model->CollisionGeometry(out.Positions, out.Indices);
        std::vector<glm::mat4> boneM(512, glm::mat4(1.0f));
        for (const char* side : {"_l", "_r"}) {
            const int ua = model->NodeIndex(std::string("upperarm") + side), la = model->NodeIndex(std::string("lowerarm") + side);
            if (ua < 0 || la < 0) return false;
            const glm::vec3 s(model->SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, ua)[3]);
            const glm::vec3 e(model->SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, la)[3]);
            const glm::vec3 axis = glm::normalize(glm::cross(e - s, glm::vec3(0.0f, -1.0f, 0.0f)));
            const glm::mat4 r = glm::translate(glm::mat4(1.0f), s) * glm::rotate(glm::mat4(1.0f), glm::radians(deg), axis) *
                                glm::translate(glm::mat4(1.0f), -s);
            std::vector<char> under((size_t)model->NodeCount(), 0);
            for (int i = 0; i < model->NodeCount(); ++i) { // parents come before their children
                const int p = model->NodeParent(i);
                under[(size_t)i] = i == ua || (p >= 0 && under[(size_t)p]);
                const int id = under[(size_t)i] ? model->BoneId(model->NodeName(i)) : -1;
                if (id >= 0 && id < 512) boneM[(size_t)id] = r;
            }
        }
        size_t k = 0;
        for (int mi = 0; mi < model->MeshCount(); ++mi)
            for (const auto& v : model->MeshSkinVertices(mi)) {
                glm::mat4 m(0.0f);
                float tw = 0.0f;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                    if (v.BoneIDs[i] >= 0 && v.BoneIDs[i] < 512) { m += boneM[(size_t)v.BoneIDs[i]] * v.Weights[i]; tw += v.Weights[i]; }
                if (k < out.Positions.size() && tw > 1e-4f) out.Positions[k] = glm::vec3(m * glm::vec4(out.Positions[k], 1.0f));
                ++k;
            }
        return k == out.Positions.size();
    };
    OutfitCoverage::Mesh arms, shirt;
    CHECK(posed(armsPath, 0.0f, arms) && posed(shirtPath, 0.0f, shirt));
    const auto hidden = OutfitCoverage::Hidden(arms, shirt);
    const size_t hiddenN = std::count(hidden.begin(), hidden.end(), (std::uint8_t)1);
    CHECK(hiddenN > 300); // the sleeves' skin is still hidden - the band isn't the whole arm
    // Nothing hidden past the sleeve near its hem: the rolled cuff sits over ~20 arm vertices only in the bind
    // pose (they "poke through" it there), and hiding them opened the black patch in the Scene view.
    {
        const auto shirtCov = OutfitCoverage::Covered(arms, shirt);
        std::map<std::pair<unsigned, unsigned>, int> edges;
        for (size_t t = 0; t + 2 < shirt.Indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const unsigned x = shirt.Indices[t + k], y = shirt.Indices[t + (k + 1) % 3];
                ++edges[{std::min(x, y), std::max(x, y)}];
            }
        std::vector<glm::vec3> hem; // the open edges out along the arms: the sleeves' hems (UV seams are harmless here)
        for (const auto& [e, n] : edges)
            if (n == 1)
                for (unsigned v : {e.first, e.second})
                    if (std::abs(shirt.Positions[v].x) > 0.25f) hem.push_back(shirt.Positions[v]);
        CHECK(!hem.empty());
        size_t pastHem = 0;
        for (size_t v = 0; v < hidden.size(); ++v) {
            if (!hidden[v] || shirtCov[v]) continue;
            for (const auto& q : hem)
                if (glm::distance(q, arms.Positions[v]) < OutfitCoverage::kEdgeBand) { ++pastHem; break; }
        }
        CHECK(pastHem == 0);
    }
    for (float deg : {20.0f, 35.0f, 50.0f}) {
        OutfitCoverage::Mesh pa, ps;
        CHECK(posed(armsPath, deg, pa) && posed(shirtPath, deg, ps));
        const auto covered = OutfitCoverage::Covered(pa, ps);
        const auto poke = OutfitCoverage::PokeDepth(pa, ps, OutfitCoverage::kPokeReach);
        size_t holes = 0;
        for (size_t v = 0; v < hidden.size() && v < covered.size(); ++v) holes += hidden[v] && !covered[v] && poke[v] <= 0.0f;
        Log::Info("OutfitEdgeBandPosed: " + std::to_string(hiddenN) + " arm vertices hidden; " + std::to_string((int)deg) +
                  " deg down, " + std::to_string(holes) + " out from under the sleeve.");
        CHECK(holes == 0);
    }
}

// The balaclava rides the head bone under a hood that's up: only what pokes out through the hood is hidden
// (Hidden's `rigid`). Hiding the covered rest showed through the hood's face opening as missing balaclava
// round the ears, the ears behind it. And the ears stay hidden under the balaclava. Needs the Quantum pack.
void TestOutfitRigidUnderHood() {
    const std::string C = "assets/Characters/Quantum/Models/Clothing/Male/";
    const std::string headPath = "assets/Characters/Quantum/Models/Young/Quantum_Young_Head.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(C + "Tops/SKM_Hoodie_Hood_Up.fbx")) || !std::filesystem::exists(ProjectPaths::Resolve(headPath))) {
        Log::Info("OutfitRigidUnderHood: no Quantum pack - skipped.");
        return;
    }
    auto geo = [](const std::string& rel) {
        OutfitCoverage::Mesh m;
        auto mm = Model::ImportDeferred(ProjectPaths::Resolve(rel), ModelImportSettings{});
        if (mm) mm->CollisionGeometry(m.Positions, m.Indices);
        return m;
    };
    const auto bala = geo(C + "Balaclava/SM_Balaclava_Crime.fbx"), hood = geo(C + "Tops/SKM_Hoodie_Hood_Up.fbx"), head = geo(headPath);
    auto roundEars = [](const glm::vec3& p) { return std::abs(p.x) > 0.06f && p.y > 1.58f && p.y < 1.76f; };
    const auto hb = OutfitCoverage::Hidden(bala, hood, false, true);
    size_t n = 0, hidden = 0;
    for (size_t v = 0; v < hb.size(); ++v)
        if (roundEars(bala.Positions[v])) { ++n; hidden += hb[v]; }
    const auto hh = OutfitCoverage::Hidden(head, bala, true);
    size_t en = 0, ehidden = 0;
    for (size_t v = 0; v < hh.size(); ++v)
        if (roundEars(head.Positions[v])) { ++en; ehidden += hh[v]; }
    Log::Info("OutfitRigidUnderHood: balaclava round the ears " + std::to_string(hidden) + " / " + std::to_string(n) + " hidden by the hood; head " +
              std::to_string(ehidden) + " / " + std::to_string(en) + " by the balaclava.");
    CHECK(n > 0 && hidden * 20 < n);   // was 631 / 824
    CHECK(en > 0 && ehidden * 10 > en * 8); // the ears: ~89 % (5012 of 5647 once, when the band covered the head)
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

// The squad radio: one speaker per squad, priority preemption, per-event cooldowns, "copy" responders.
// The enemy AI's pure rules (AiMath): perception, memory, accuracy and the squad's tactics.
void TestAiMath() {
    // Perception: nearer, more central, more of the body, moving, and firing all notice faster; out of range or
    // behind, not at all.
    {
        DetectionInput base;
        const float r = DetectionRate(base);
        CHECK(r > 0.0f);
        DetectionInput far = base; far.Distance = 60.0f;
        DetectionInput side = base; side.AngleDeg = 60.0f;
        DetectionInput part = base; part.VisiblePoints = 1;
        DetectionInput crouch = base; crouch.TargetCrouched = true;
        DetectionInput fire = base; fire.TargetFiring = true; fire.TargetCrouched = true;
        DetectionInput behind = base; behind.AngleDeg = 120.0f;
        DetectionInput hidden = base; hidden.VisiblePoints = 0;
        DetectionInput alert = base; alert.Alertness = 1.0f;
        CHECK(DetectionRate(far) < r);
        CHECK(DetectionRate(side) < r);
        CHECK(DetectionRate(part) < r);
        CHECK(DetectionRate(crouch) < r);
        CHECK(DetectionRate(fire) > r);
        CHECK(DetectionRate(behind) == 0.0f);
        CHECK(DetectionRate(hidden) == 0.0f);
        CHECK(DetectionRate(alert) > r);
    }
    // Memory: awareness builds to Known once (true on that update only); unseen, an unknown target fades and a known
    // one's position grows uncertain.
    {
        TargetMemory m;
        const glm::vec3 at(3.0f, 0.0f, 4.0f), vel(1.0f, 0.0f, 0.0f);
        int became = 0;
        for (int i = 0; i < 40; ++i) became += UpdateMemory(m, true, at, vel, 1.0f, 0.1f * i, 0.1f) ? 1 : 0;
        CHECK(m.Known && became == 1);
        CHECK(glm::length(m.LastKnown - at) < 1e-5f);
        const float seen = m.LastSeen;
        UpdateMemory(m, false, at, vel, 0.0f, seen + 1.0f, 1.0f);
        CHECK(m.Known && !m.Visible && m.Uncertainty > 0.0f);
        CHECK(glm::length(m.Predicted(seen + 1.0f) - (at + vel)) < 1e-4f);
        CHECK(glm::length(m.Predicted(seen + 10.0f) - (at + vel * 1.5f)) < 1e-4f); // carried on 1.5 s at most
        TargetMemory u;
        UpdateMemory(u, true, at, vel, 2.0f, 0.0f, 0.2f);
        CHECK(!u.Known && u.Awareness > 0.3f);
        UpdateMemory(u, false, at, vel, 0.0f, 5.0f, 2.0f);
        CHECK(u.Awareness < 0.3f);
    }
    // Accuracy: settles over time on target, worse at range and suppressed, never above 0.85; reaction time drops
    // with skill and difficulty.
    {
        AccuracyInput a;
        a.TimeOnTarget = 2.0f;
        const float settled = HitProbability(a);
        AccuracyInput fresh = a; fresh.TimeOnTarget = 0.0f;
        AccuracyInput far = a; far.Distance = 60.0f;
        AccuracyInput pinned = a; pinned.Suppression = 1.0f;
        AccuracyInput best = a; best.Skill = 1.0f; best.Difficulty = 2.0f; best.Distance = 2.0f;
        CHECK(HitProbability(fresh) < settled);
        CHECK(HitProbability(far) < settled);
        CHECK(HitProbability(pinned) < settled);
        CHECK(HitProbability(best) <= 0.85f);
        CHECK(ReactionTime(1.0f, 1.0f, false, 0.5f) < ReactionTime(0.0f, 1.0f, false, 0.5f));
        CHECK(ReactionTime(0.5f, 2.0f, false, 0.5f) < ReactionTime(0.5f, 1.0f, false, 0.5f));
        CHECK(ReactionTime(0.5f, 1.0f, true, 0.5f) > ReactionTime(0.5f, 1.0f, false, 0.5f));
    }
    // Tactics.
    {
        CHECK(MayBound(false, false, 0.0f));         // unseen: go
        CHECK(!MayBound(true, false, 0.5f));         // seen, nobody covering: wait
        CHECK(MayBound(true, true, 0.0f));           // covered: go
        CHECK(MayBound(true, false, 1.6f));          // waited long enough: go anyway
        CHECK(WantsBlindFire(0.8f, 2.5f, true, 3.0f));
        CHECK(!WantsBlindFire(0.3f, 2.5f, true, 3.0f)); // not pinned
        CHECK(!WantsBlindFire(0.8f, 1.0f, true, 3.0f)); // not for long
        CHECK(!WantsBlindFire(0.8f, 2.5f, false, 3.0f));
        CHECK(!WantsBlindFire(0.8f, 2.5f, true, 20.0f)); // no idea where any more
        CHECK(WantsMelee(1.2f, 10.0f, 5.0f));
        CHECK(!WantsMelee(3.0f, 10.0f, 5.0f));        // out of reach
        CHECK(!WantsMelee(1.2f, 90.0f, 5.0f));        // off to the side
        CHECK(!WantsMelee(1.2f, 10.0f, 0.5f));        // cooling down
        const glm::vec3 deaths[2] = {glm::vec3(0.0f), glm::vec3(20.0f, 0.0f, 0.0f)};
        const float times[2] = {10.0f, 10.0f};
        CHECK(std::abs(DangerScale(glm::vec3(0.0f), deaths, times, 2, 10.0f) - 0.3f) < 1e-4f);
        CHECK(DangerScale(glm::vec3(2.5f, 0.0f, 0.0f), deaths, times, 2, 10.0f) > 0.3f);
        CHECK(DangerScale(glm::vec3(10.0f, 0.0f, 0.0f), deaths, times, 2, 10.0f) == 1.0f); // far from both
        CHECK(DangerScale(glm::vec3(0.0f), deaths, times, 2, 45.0f) == 1.0f);              // long ago
        CHECK(DangerScale(glm::vec3(0.0f), deaths, times, 0, 10.0f) == 1.0f);
    }
}

void TestSquadVoice() {
    namespace fs = std::filesystem;
    const glm::vec3 pos(0.0f);
    // The built-in tables: unique keys, wounded outranks everything but nothing outranks the others' copy.
    {
        std::set<std::string> keys;
        for (int i = 0; i < (int)Bark::Count; ++i) keys.insert(BarkKey((Bark)i));
        CHECK(keys.size() == (size_t)Bark::Count);
        CHECK(BarkDefaults(Bark::Wounded).Priority > BarkDefaults(Bark::ManDown).Priority);
        CHECK(BarkDefaults(Bark::Copy).Priority == 0);
    }
    // Priority and cooldown.
    {
        SquadVoice v;
        v.SetAudio(false);
        CHECK(v.Say(0.0f, 0, 1, 11, Bark::Contact, pos));
        CHECK(v.Busy(0, 0.5f));
        CHECK(!v.Say(0.5f, 0, 2, 12, Bark::Reloading, pos));            // busy, lower priority
        CHECK(!v.Say(0.5f, 0, 2, 12, Bark::Contact, pos));              // equal priority and cooling down
        CHECK(v.Say(0.5f, 0, 2, 12, Bark::Wounded, pos));               // higher: cuts in
        CHECK(v.History().size() == 2);
        CHECK(std::abs(v.History()[0].Cut - 0.5f) < 1e-5f);
        CHECK(v.Cuts() == 1);
        CHECK(v.FirstOverlap(0) < 0);
        const float free = v.History()[1].End + 0.5f;
        CHECK(!v.Busy(0, free));
        CHECK(!v.Say(free, 0, 2, 12, Bark::Wounded, pos));              // wounded cools down (4 s)
        CHECK(v.Say(free + v.Cooldown(Bark::Wounded), 0, 2, 12, Bark::Wounded, pos));
        // A second squad has its own channel.
        CHECK(v.Say(0.6f, 1, 5, 15, Bark::Reloading, pos));
        CHECK(v.FirstOverlap(1) < 0);
        // A copy never cuts anything.
        SquadVoice w;
        w.SetAudio(false);
        CHECK(w.Say(0.0f, 0, 1, 1, Bark::Idle, pos));
        CHECK(!w.Say(0.1f, 0, 2, 2, Bark::Copy, pos));
        const auto sub = w.TakeSubtitles();
        CHECK(sub.size() == 1 && !sub[0].Text.empty() && sub[0].Unit == 1);
        CHECK(w.TakeSubtitles().empty());
    }
    // Responders answer "copy" after the line, never over it.
    {
        SquadVoice v;
        v.SetAudio(false);
        int copies = 0;
        float t = 0.0f;
        for (int k = 0; k < 40; ++k) {
            t += 40.0f;
            CHECK(v.Say(t, 0, 1, 1, Bark::ManDown, pos, 2, 2, pos));
            const float end = v.History().back().End;
            for (float u = t; u < end + 3.0f; u += 0.05f) v.Update(u);
        }
        for (const BarkPlayed& b : v.History()) copies += b.Responder ? 1 : 0;
        CHECK(copies > 5 && copies < 40);
        CHECK(v.FirstOverlap(0) < 0);
        for (size_t i = 1; i < v.History().size(); ++i)
            if (v.History()[i].Responder) CHECK(v.History()[i].Start >= v.History()[i - 1].End && v.History()[i].Speaker == 2);
    }
    // A manifest sets priorities, cooldowns, texts and the clip lengths that hold the channel.
    {
        const fs::path dir = fs::temp_directory_path() / "tartarus_squadvoice_test";
        std::error_code ec;
        fs::create_directories(dir, ec);
        {
            std::ofstream m(dir / "manifest.json");
            m << R"({"sampleRate":22050,"voices":2,"events":{
                "contact":{"priority":2,"cooldown":1.0,"lines":[{"text":"Test contact.","files":["a.wav","b.wav"],"duration":[3.0,4.0]}]},
                "reloading":{"priority":6,"cooldown":0.0,"lines":[{"text":"Test reload.","files":["c.wav","d.wav"],"duration":[1.0,1.0]}]}}})";
        }
        SquadVoice v;
        v.SetAudio(false);
        CHECK(v.LoadManifest(dir.string()));
        CHECK(v.Priority(Bark::Contact) == 2 && v.Priority(Bark::Reloading) == 6);
        CHECK(v.Say(0.0f, 0, 1, 7, Bark::Contact, pos));                // voice 1: 4 s
        CHECK(std::abs(v.History()[0].End - 4.0f) < 1e-4f && v.History()[0].Text == "Test contact.");
        CHECK(v.Say(1.0f, 0, 2, 8, Bark::Reloading, pos));              // the manifest made reloading outrank contact
        CHECK(v.History()[0].Cut >= 0.0f);
        CHECK(v.FirstOverlap(0) < 0);
        fs::remove_all(dir, ec);
    }
    // A long random session over three squads: nothing overlaps, no event repeats inside its cooldown.
    {
        SquadVoice v;
        v.SetAudio(false);
        std::uint32_t rng = 12345u;
        auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) & 0xFFFF; };
        for (float t = 0.0f; t < 900.0f; t += 0.1f) {
            const int tries = (int)(rnd() % 3u);
            for (int k = 0; k < tries; ++k) {
                const int squad = (int)(rnd() % 3u);
                const Bark b = (Bark)(rnd() % (unsigned)Bark::Count);
                v.Say(t, squad, squad * 4 + (int)(rnd() % 4u), 1, b, pos, (int)(rnd() % 4u) - 1 + squad * 4);
            }
            v.Update(t);
        }
        CHECK(v.Spoken() > 100);
        for (int s = 0; s < 3; ++s) CHECK(v.FirstOverlap(s) < 0);
        float last[3][(int)Bark::Count];
        for (auto& row : last) for (float& f : row) f = -1e9f;
        bool cooldownsHold = true;
        for (const BarkPlayed& b : v.History()) {
            if (b.Responder) continue;
            if (b.Start - last[b.Squad][(int)b.Event] < v.Cooldown(b.Event) - 1e-3f) cooldownsHold = false;
            last[b.Squad][(int)b.Event] = b.Start;
        }
        CHECK(cooldownsHold);
    }
    // The real manifest, when the project is around (the generator's output): every event has lines.
    {
        SquadVoice v;
        v.SetAudio(false);
        const std::string root = ProjectPaths::Resolve("assets/Audio/Voice/combine");
        if (fs::exists(fs::path(root) / "manifest.json")) {
            CHECK(v.LoadManifest(root));
            for (int i = 0; i < (int)Bark::Count; ++i) CHECK(v.Say(1000.0f * (float)(i + 1), 0, i, 1, (Bark)i, pos));
            for (const BarkPlayed& b : v.History()) CHECK(!b.File.empty() && b.End > b.Start + 0.3f && fs::exists(fs::path(root) / b.File));
        }
    }
}

int RunUnitTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"AssetGuid", TestAssetGuid},
        {"UndoDeltaChain", TestUndoDeltaChain},
        {"AtomicFile", TestAtomicFile},
        {"TextureCacheHash", TestTextureCacheHash},
        {"OutfitEdgeBandPosed", TestOutfitEdgeBandPosed},
        {"OutfitRigidUnderHood", TestOutfitRigidUnderHood},
        {"AssetPackImport", TestAssetPackImport},
        {"MaterialRobustness", TestMaterialRobustness},
        {"ComponentRegistry", TestComponentRegistry},
        {"AnimatorController", TestAnimatorController},
        {"RootMotion", TestRootMotion},
        {"FirstPersonBodyValidate", TestFirstPersonBodyValidate},
        {"FirstPersonBodyClothingHide", TestFirstPersonBodyClothingHide},
        {"Wardrobe", TestWardrobe},
        {"OutfitCoverage", TestOutfitCoverage},
        {"WardrobeQuantum", TestWardrobeQuantum},
        {"AsyncImportCpu", TestAsyncImportCpu},
        {"FirstPersonBodyTuning", TestFirstPersonBodyTuning},
        {"FirstPersonWeaponValidate", TestFirstPersonWeaponValidate},
        {"ClipTrim", TestClipTrim},
        {"FirstPersonBodyBoneMap", TestFirstPersonBodyBoneMap},
        {"ClipContactDebounce", TestClipContactDebounce},
        {"FirstPersonBodyController", TestFirstPersonBodyController},
        {"FirstPersonWeaponWizard", TestFirstPersonWeaponWizard},
        {"AnimatorLint", TestAnimatorLint},
        {"BlendTree2D", TestBlendTree2D},
        {"FirstPersonAnimationSet", TestFirstPersonAnimationSet},
        {"FirstPersonAnimationFSM", TestFirstPersonAnimationFSM},
        {"FirstPersonShotgunSet", TestFirstPersonShotgunSet},
        {"RemingtonController", TestRemingtonController},
        {"FirstPersonAds", TestFirstPersonAds},
        {"BulletHoles", TestBulletHoles},
        {"ShellCasings", TestShellCasings},
        {"WeaponZero", TestWeaponZero},
        {"Curve", TestCurve},
        {"IKSolver", TestIKSolver},
        {"WeaponProcedural", TestWeaponProcedural},
        {"AssetIdentity", TestAssetIdentity},
        {"ProjectWatcher", TestProjectWatcher},
        {"LodGroup", TestLodGroup},
        {"NearestEuler", TestNearestEuler},
        {"QuaternionTransformStorage", TestQuaternionTransformStorage},
        {"RotateEulerAboutLocalAxis", TestRotateEulerAboutLocalAxis},
        {"SpinSystemsAboutDiagonalAxis", TestSpinSystemsAboutDiagonalAxis},
        {"MaterialTextureDefaults", TestMaterialTextureDefaults},
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
        {"PlayerAcceleration", TestPlayerAcceleration},
        {"FramePacing", TestFramePacing},
        {"NpcHitRegions", TestNpcHitRegions},
        {"IKSetGlobalsBatch", TestIKSetGlobalsBatch},
        {"NpcBodyParts", TestNpcBodyParts},
        {"PhysicsWorldSync", TestPhysicsWorldSync},
        {"PhysicalSky", TestPhysicalSky},
        {"SquadVoice", TestSquadVoice},
        {"AiMath", TestAiMath},
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
