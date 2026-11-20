// Unit tests: First-person weapons: recoil profiles, the weapon definition and its validation, the animation set and its controllers (AKS74U, Remington 870, Revolver .38), the FSM, ADS, procedural motion, casings, bullet holes and zeroing.
// (Moved out of UnitTests.cpp; registered by RunUnitTests through RegisterFirstPersonWeaponsTests.)
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
#include <set>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using json = nlohmann::json;
using UnitTestSupport::g_Checks;
using UnitTestSupport::g_Failures;
using UnitTestSupport::TempDir;
using UnitTestSupport::ReadAll;

namespace {

// --- Camera roll (#165) --------------------------------------------------------------------
void TestRecoilProfile() {
    RecoilAsset asset, loaded;
    asset.Data = WeaponProceduralSettings::Defaults().Recoil; // exercise version-1 migration
    asset.Data.AimPitch = glm::vec2(1.0f);
    asset.Data.AimYaw = glm::vec2(-0.2f);
    asset.Data.AimRecovery = 1.0f;
    asset.Data.AimSmoothing = glm::vec2(50.0f);
    asset.Data.NoiseSide = {-0.001f, 0.001f};
    asset.Data.PushAmount = 0.01f;
    asset.Data.PitchProgression = 0.3f;
    asset.Data.UpProgression = 0.003f;
    asset.Data.SwayPitch = glm::vec2(0.2f);
    asset.Data.SustainedCurves = true;
    asset.Data.SustainedDuration = 0.25f;
    asset.Data.AdsPivot = {0.0f, 0.0f, 0.1f};
    std::string why;
    CHECK(RecoilAsset::FromJsonString(asset.ToJsonString(), loaded, &why));
    CHECK(loaded.ToJsonString() == asset.ToJsonString());
    const auto before = loaded.ToJsonString();
    for (const char* invalid : {"null", "{}", "{\"type\":\"recoil\",\"version\":2,\"recoil\":{}}",
         "{\"type\":\"recoil\",\"version\":1,\"recoil\":{\"duration\":0}}",
         "{\"type\":\"recoil\",\"version\":1,\"recoil\":{\"aimPitch\":[2,1]}}",
         "{\"type\":\"recoil\",\"version\":1,\"recoil\":{\"enabled\":\"yes\"}}",
         "{\"type\":\"recoil\",\"version\":1,\"recoil\":{\"sustainedDuration\":0}}",
         "{\"type\":\"recoil\",\"version\":1,\"recoil\":{\"aimSmoothing\":[-1,10]}}"}) {
        CHECK(!RecoilAsset::FromJsonString(invalid, loaded, &why));
        CHECK(loaded.ToJsonString() == before);
    }
    const auto profile = TempDir() / "shared.recoil";
    CHECK(asset.SaveFile(profile.u8string()));
    // Files outside the project deliberately receive no automatic sidecar.
    const auto profileGuid = AssetGuid::Generate();
    CHECK(AtomicFile::WriteBytes(profile.u8string() + ".meta",
        json{{"guid", profileGuid.ToString()}, {"type", "recoil"}}.dump(), true));
    CHECK(RecoilAsset::LoadFile(profile.u8string(), loaded, &why));
    CHECK(AssetDatabase::AssetType(profile.u8string()) == "recoil");
    FirstPersonAnimationSet weapon;
    weapon.ArmsModel = "arms.fbx";
    weapon.WeaponModel = "weapon.fbx";
    weapon.Controller = "weapon.controller";
    weapon.RecoilProfile = profile.u8string();
    weapon.Procedural.Recoil.BoltBone = "action";
    weapon.Procedural.Recoil.BoltTravel = {0.0f, 0.0f, 0.02f};
    weapon.Procedural.Recoil.BoltCycle = 0.08f;
    weapon.Gameplay = FirstPersonWeaponGameplay{};
    weapon.HasLegacyGameplay = true; // explicitly exercise old descriptor compatibility
    weapon.Gameplay.BurstRounds = 3;
    const auto definition = TempDir() / "profile-weapon.fpsanim";
    CHECK(weapon.SaveFile(definition.u8string()));
    FirstPersonAnimationSet resolved;
    CHECK(FirstPersonAnimationSet::LoadFile(definition.u8string(), resolved, &why));
    CHECK(resolved.Procedural.Recoil.UnityPort && resolved.Procedural.Recoil.Unity.VerticalRecoil == glm::vec2(1.0f));
    CHECK(resolved.Procedural.Recoil.HipProcedural);
    CHECK(resolved.Procedural.Recoil.BoltBone == "action" && resolved.Procedural.Recoil.BoltTravel.z == 0.02f);
    CHECK(resolved.Gameplay.BurstRounds == 3);
    asset.Data.AimPitch = glm::vec2(2.0f);
    CHECK(asset.SaveFile(profile.u8string()));
    CHECK(FirstPersonAnimationSet::LoadFile(definition.u8string(), resolved, &why));
    CHECK(resolved.Procedural.Recoil.Unity.VerticalRecoil == glm::vec2(2.0f));
    const auto moved = TempDir() / "moved-shared.recoil";
    std::error_code moveError;
    std::filesystem::remove(moved, moveError);
    std::filesystem::remove(moved.u8string() + ".meta", moveError);
    std::filesystem::rename(profile, moved, moveError);
    CHECK(!moveError);
    AssetDatabase::NotifyMoved(profile.u8string(), moved.u8string());
    CHECK(FirstPersonAnimationSet::LoadFile(definition.u8string(), resolved, &why));
    CHECK(AssetDatabase::PathKey(resolved.RecoilProfile) == AssetDatabase::PathKey(moved.u8string()));
    const auto resolvedBefore = resolved.ToJsonString();
    std::error_code ec;
    std::filesystem::remove(moved, ec);
    CHECK(!FirstPersonAnimationSet::LoadFile(definition.u8string(), resolved, &why));
    CHECK(resolved.ToJsonString() == resolvedBefore);

    auto settings = WeaponProceduralSettings::Defaults();
    settings.Recoil = loaded.Data;
    settings.Recoil.UnityPort = false; // retain regression coverage for embedded legacy weapon definitions
    settings.Recoil.AimPitch = glm::vec2(1.0f);
    settings.Recoil.AimYaw = glm::vec2(0.0f);
    settings.Recoil.AimSmoothing = glm::vec2(0.0f);
    settings.Recoil.AimRecoveryDelay = 0.2f;
    settings.Recoil.AimRecoverySpeed = 20.0f;
    settings.Recoil.AimRecovery = 1.0f;
    WeaponProceduralInput input;
    input.Dt = 1.0f / 120.0f;
    input.Ads = true;
    WeaponProceduralState state;
    state.OnShot(settings, true);
    CHECK(state.Update(settings, input).AimKick.x == 1.0f);
    CHECK(state.ConsumeAimKick().x == 1.0f);
    CHECK(state.ConsumeAimKick() == glm::vec2(0.0f));
    state.CompensateAim({-1.0f, 0.0f});
    float recovery = 0.0f;
    for (int i = 0; i < 240; ++i) recovery += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(recovery) < 1e-5f); // player already countered all of it
    state.Reset();
    state.OnShot(settings, true);
    state.Update(settings, input);
    state.CompensateAim({-0.4f, 0.0f});
    recovery = 0.0f;
    for (int i = 0; i < 240; ++i) recovery += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(recovery + 0.6f) < 1e-4f);
    state.Reset();
    state.OnShot(settings, true);
    state.Update(settings, input);
    state.CompensateAim({1.0f, 0.0f}); // moving with the kick does not count as compensation
    recovery = 0.0f;
    for (int i = 0; i < 240; ++i) recovery += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(recovery + 1.0f) < 1e-4f);
    state.Reset();
    state.OnShot(settings, true);
    state.SetFiring(true);
    float heldAim = 0.0f;
    for (int i = 0; i < 120; ++i) heldAim += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(heldAim - 1.0f) < 1e-5f); // held fire postpones recovery even at a slow cadence
    state.EndBurst();
    for (int i = 0; i < 120; ++i) heldAim += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(heldAim) < 1e-5f);
    state.Reset();
    state.OnShot(settings, true);
    state.Update(settings, input);
    state.DiscardClampedAim({1.0f, 0.0f});
    recovery = 0.0f;
    for (int i = 0; i < 240; ++i) recovery += state.Update(settings, input).AimKick.x;
    CHECK(std::fabs(recovery) < 1e-5f); // a kick blocked by the pitch limit must not recover
    settings.Recoil.AimAdsScale = 0.5f;
    settings.Recoil.AdsScale = 0.0f;
    state.Reset();
    state.OnShot(settings, true);
    CHECK(state.Update(settings, input).AimKick.x == 0.5f); // independent controller ADS multiplier
    settings.Recoil.AimAdsScale = 1.0f;
    settings.Recoil.AdsScale = 1.0f;

    // Same impulses at several frame rates: controller integration stays constant and all visual layers settle.
    settings.Recoil.AimRecovery = 0.0f;
    settings.Recoil.AimSmoothing = {30.0f, 60.0f};
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
        WeaponProceduralState sim;
        sim.Seed(42);
        input.Dt = dt;
        glm::vec2 integrated(0.0f);
        int shots = 0;
        for (int f = 0; f < (int)(5.0f / dt); ++f) {
            if (shots < 5 && f * dt >= shots * 0.1f) { sim.OnShot(settings, true); ++shots; }
            const auto& pose = sim.Update(settings, input);
            integrated += pose.AimKick;
            CHECK(std::isfinite(pose.Position.x) && std::isfinite(pose.Rotation.x));
        }
        CHECK(std::fabs(integrated.x - 5.0f) < 0.001f);
        CHECK(sim.ActiveShots() == 0 && glm::length(sim.Pose().Position) < 0.001f);
    }
    // Shared asset stays immutable, and runtime states remain independent.
    WeaponProceduralState a, b;
    const auto tuning = loaded.ToJsonString();
    a.OnShot(settings, true);
    a.Update(settings, input);
    CHECK(b.ActiveShots() == 0 && b.Pose().AimKick == glm::vec2(0.0f));
    CHECK(loaded.ToJsonString() == tuning);
    a.Reset();
    CHECK(a.ActiveShots() == 0 && a.Pose().Position == glm::vec3(0.0f));
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
    // Persistent view placement survives serialization and is absent by default in older sets.
    CHECK(again.ViewPosition == glm::vec3(0.0f));
    set.ViewPosition = glm::vec3(0.01f, -0.02f, 0.06f);
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(), again, &error));
    CHECK(again.ViewPosition == set.ViewPosition);
    CHECK(again.AdsLocomotionScale==1.0f);
    set.AdsLocomotionScale=0.25f;
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(),again,&error));
    CHECK(again.AdsLocomotionScale==0.25f);
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a","weaponModel":"w","controller":"c","adsLocomotionScale":2})",again,&error));
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a","weaponModel":"w","controller":"c","adsLocomotionScale":"bad"})",again,&error));
    CHECK(!FirstPersonAnimationSet::FromJsonString(
        R"({"armsModel":"a.fbx","weaponModel":"w.fbx","controller":"c.controller","weaponMaterials":{"aks74u":3}})",
        set, &error));
    CHECK(error.find("weaponMaterials.aks74u") != std::string::npos);
}

// The shotgun options (Remington 870): pellets and spread, a per-round reload, a pump worked after every
// round, and a mount with a translation - parsed, written back, and defaulting to the plain rifle's.

// Phase matching (a weapon's walk sway on the body's feet): the loop follows the parameter without running back, and the gait
// tracker reads the feet's phase at their pace without a jump.
void TestGaitPhaseSync() {
    auto near = [](float a, float b, float e = 1e-3f) { return std::abs(a - b) <= e; };
    bool m = false;
    CHECK(near(AnimatorPhaseStep(0.0f, 0.7f, 0.02f, m), 0.7f) && m); // entering: straight there
    m = true;
    CHECK(near(AnimatorPhaseStep(3.2f, 0.25f, 0.02f, m), 3.25f));
    CHECK(near(AnimatorPhaseStep(3.95f, 0.05f, 0.02f, m), 4.05f)); // across the wrap, forward
    CHECK(near(AnimatorPhaseStep(3.5f, 0.3f, 0.02f, m), 3.45f));   // never back more than 0.05
    m = false; // mid-way on its own clock when the phase comes: caught up at most half again its step, never jumped
    CHECK(near(AnimatorPhaseStep(3.2f, 0.7f, 0.02f, m), 3.23f) && !m);
    CHECK(near(AnimatorPhaseStep(3.2f, 0.215f, 0.02f, m), 3.215f) && m);
    // The feet's spread and lift over a stride (the left ahead at 0, the right lifting after it), at 1.3 strides a second with
    // a step's timing a little uneven: the phase runs at that rate, steadily, and sits on the feet.
    FirstPersonGaitPhase g;
    const float dt = 1.0f / 60.0f, rate = 1.3f;
    float t = 0.0f, worstJump = 0.0f, prev = 0.0f, truth = 0.0f, minRate = 10.0f, maxRate = 0.0f;
    bool wasLocked = false;
    for (int i = 0; i < 600; ++i, t += dt) {
        truth += rate * dt * (1.0f + 0.15f * std::sin(t * 7.3f)); // uneven steps
        const float a = 6.28318531f * truth;
        g.Update(0.3f * std::cos(a), -0.08f * std::sin(a), true, dt);
        if (i > 0 && wasLocked) { float d = g.Phase - prev; d -= std::floor(d + 0.5f); worstJump = std::max(worstJump, std::abs(d)); }
        prev = g.Phase;
        wasLocked = g.Locked; // (locking it goes straight onto the reading; the clips that follow catch up to it)
        if (i > 240) { minRate = std::min(minRate, g.Rate); maxRate = std::max(maxRate, g.Rate); }
    }
    CHECK(g.Locked && minRate > rate * 0.85f && maxRate < rate * 1.15f);
    CHECK(worstJump < 0.04f); // ~ the rate's 0.022 a frame: never a pop
    float e = g.Phase - truth;
    e -= std::floor(e + 0.5f);
    CHECK(std::abs(e) < 0.06f);
    for (int i = 0; i < 90; ++i) g.Update(0.0f, 0.0f, false, dt); // stood: lets go after a second
    CHECK(!g.Locked);
}

void TestAKAdditiveWalking() {
    // The distributable sample omits Weapons. Source checkouts still gate the authored graph.
    if (!std::filesystem::exists(ProjectPaths::Resolve("assets/Weapons"))) {
        UnitTestSupport::Skip("no weapon project data");
        return;
    }
    AnimatorController c;
    std::string error;
    CHECK(AnimatorController::LoadFile(ProjectPaths::Resolve("assets/Weapons/AKS74U/AKS74U.controller"),c,&error));
    CHECK(c.Layers.size()==3);
    if(c.Layers.size()!=3) return;
    const auto& l=c.Layers[1];
    CHECK(l.Name=="Weapon Locomotion" && l.Mode==AnimatorController::Blending::Additive);
    CHECK(l.AdditiveReferenceState=="Idle");
    AnimatorController roundTrip;
    CHECK(AnimatorController::FromJsonString(c.ToJsonString(),roundTrip,&error));
    CHECK(roundTrip.Layers[1].AdditiveReferenceState=="Idle");
    for(const char* name : {"Idle","Walk","IdleToSprint","Sprint","SprintToIdle"}) {
        CHECK(l.FindState(name)>=0);
        const auto& base=c.Layers[0].States[c.Layers[0].FindState(name)];
        CHECK(base.Speed==0 && base.MotionFor(0).Clip.find("Idle.fbx")!=std::string::npos);
    }
    std::vector<AnimatorParam> p={{"Sprint",2,1},{"Speed",0,4},{"Equipped",2,1},{"Jog",2,0}};
    const auto dest=[&](const char* name,float phase) {
        const int t=c.PickTransition(1,l.FindState(name),phase,p);
        return t<0?std::string():l.Transitions[t].To;
    };
    // The jog (Shift) takes the locomotion layer's run clip too, without Sprint (the base layer stays ready to fire), and keeps it.
    p[0].Value=0; p[3].Value=1;
    CHECK(dest("Walk",0.3f)=="IdleToSprint" && dest("Sprint",0.3f).empty() && dest("SprintToIdle",0.5f)=="IdleToSprint");
    p[0].Value=1; p[3].Value=0;
    CHECK(dest("Idle",0.3f)=="IdleToSprint");
    CHECK(dest("Walk",0.3f)=="IdleToSprint");
    CHECK(dest("IdleToSprint",0.5f).empty());
    CHECK(dest("IdleToSprint",1)=="Sprint");
    p[0].Value=0;
    CHECK(dest("Sprint",0.3f)=="SprintToIdle");
    CHECK(dest("SprintToIdle",1)=="Walk");
    p[1].Value=0;
    CHECK(dest("SprintToIdle",1)=="Idle");
    p.push_back({"Aim",2,1});
    p[1].Value=4;
    CHECK(dest("Sprint",0.3f)=="Walk");
    CHECK(dest("IdleToSprint",0.3f)=="Walk");
    CHECK(dest("SprintToIdle",0.3f)=="Walk");
    p.back().Value=0;
    p[0].Value=1; // sprint button stayed held while aiming
    CHECK(dest("Walk",0.3f)=="IdleToSprint");
    p[0].Value=0;
    p[1].Value=0;
    p[2].Value=0;
    CHECK(dest("Walk",0.3f)=="Off");
    // A reload on the base layer does not interrupt the additive walking clock.
    AnimatorControllerComponent ac;
    ac.SetBool("Equipped",true);
    ac.SetFloat("Speed",4);
    CHECK(AnimatorStartInState(c,ac,"TacReload"));
    ac.Layers[0].Stack.back().Phase=0.0f;
    const auto length=[](int,int){return 1.0f;};
    AdvanceAnimator(c,ac,0,length);
    AdvanceAnimator(c,ac,0.11f,length);
    AdvanceAnimator(c,ac,0.11f,length);
    CHECK(ac.StateName=="TacReload");
    CHECK(l.States[ac.Layers[1].Stack.back().State].Name=="Walk");
    CHECK(WeaponAnimationInputSpeed(2,glm::vec2(0),true,4)==0);
    // Weapon Blend Time overrides both animated aim entry and return fades.
    ac=AnimatorControllerComponent{};
    ac.SetBool("Equipped",true);
    ac.SetBool("Aim",true);
    ac.SetFloat("TransitionDuration:Aim",0.8f);
    CHECK(AnimatorStartInState(c,ac,"Idle"));
    AdvanceAnimator(c,ac,0,length);
    CHECK(ac.StateName=="Aim" && ac.Layers[0].Stack.back().FadeDuration==0.8f);
    AdvanceAnimator(c,ac,0.4f,length);
    CHECK(std::abs(AnimatorCrossfadeWeight(ac.Layers[0].Stack.back().Fade)-0.5f)<1e-5f);
    AdvanceAnimator(c,ac,0.41f,length);
    ac.SetBool("Aim",false);
    AdvanceAnimator(c,ac,0,length);
    CHECK(ac.StateName=="Idle" && ac.Layers[0].Stack.back().FadeDuration==0.8f);
}
void TestSprintTransitionWalk() {
    if (!std::filesystem::exists(ProjectPaths::Resolve("assets/Weapons"))) {
        UnitTestSupport::Skip("no weapon project data");
        return;
    }
    for (const char* path : {"assets/Weapons/AKS74U/AKS74U.controller", "assets/Weapons/Remington870/Remington870.controller",
                             "assets/Weapons/Revolver38/Revolver38.controller"}) {
        AnimatorController c;
        std::string error;
        CHECK(AnimatorController::LoadFile(ProjectPaths::Resolve(path), c, &error));
        CHECK(c.Layers.size() == 3);
        if (c.Layers.size() != 3) continue;
        const auto& gait = c.Layers[1];
        const auto& walk = c.Layers[2];
        CHECK(walk.Name == "Weapon Walk" && walk.Mode == AnimatorController::Blending::Additive);
        CHECK(walk.AdditiveReferenceState == "Off" && walk.WeightCurve == "WalkBlend" && walk.WeightCurveLayer == gait.Name);
        CHECK(walk.States[walk.FindState("Walk")].MotionFor(0).Clip.find("Walk.fbx") != std::string::npos);
        CHECK(gait.States[gait.FindState("Walk")].Speed == 0);
        CHECK(gait.States[gait.FindState("Walk")].MotionFor(0).Clip.find("Idle.fbx") != std::string::npos);
        AnimatorController read;
        CHECK(AnimatorController::FromJsonString(c.ToJsonString(), read, &error));
        CHECK(read.Layers[2].WeightCurve == walk.WeightCurve && read.Layers[2].WeightCurveLayer == gait.Name);
        AnimatorControllerComponent ac;
        ac.Layers.resize(3);
        for (const char* state : {"Walk", "IdleToSprint", "SprintToIdle"}) {
            ac.Layers[1].Stack = {{gait.FindState(state), 0.5f, 1.0f}};
            CHECK(AnimatorLayerWeight(c, ac, 2) == 1);
        }
        for (const char* state : {"Idle", "Sprint", "Off"}) {
            ac.Layers[1].Stack = {{gait.FindState(state), 0.5f, 1.0f}};
            CHECK(AnimatorLayerWeight(c, ac, 2) == 0);
        }
        // Walk survives the whole entry/exit clip, and follows the actual eased sprint fade.
        ac.Layers[1].Stack = {{gait.FindState("IdleToSprint"), 1, 1}, {gait.FindState("Sprint"), 0.1f, 0.25f}};
        CHECK(std::abs(AnimatorLayerWeight(c, ac, 2) - (1 - AnimatorCrossfadeWeight(0.25f))) < 1e-6f);
        ac.Layers[1].Stack = {{gait.FindState("Sprint"), 0.3f, 1}, {gait.FindState("SprintToIdle"), 0.1f, 0.5f}};
        CHECK(std::abs(AnimatorLayerWeight(c, ac, 2) - 0.5f) < 1e-6f);
        ac.SetFloat("LayerWeight:Weapon Walk", 0.3f);
        CHECK(std::abs(AnimatorLayerWeight(c, ac, 2) - 0.15f) < 1e-6f);
        // Nested interruptions retain the exact stack contribution; ADS scales it once.
        ac.Layers[1].Stack.push_back({gait.FindState("Walk"), 0, 0.5f});
        CHECK(std::abs(AnimatorLayerWeight(c, ac, 2) - 0.225f) < 1e-6f);
        c.Layers[2].WeightCurveLayer = "Missing";
        CHECK(AnimatorLayerWeight(c, ac, 2) == 0);
        c.Layers[2].WeightCurveLayer = gait.Name;
        ac = {};
        ac.SetBool("Equipped", true);
        ac.SetFloat("Speed", 4);
        ac.SetFloat("WalkRate", 1);
        const auto length = [](int, int) { return 1.0f; };
        const auto advance = [&](int frames) { for (int i = 0; i < frames; ++i) AdvanceAnimator(c, ac, 0.02f, length); };
        advance(40); // Off -> Idle -> Walk includes the authored entry crossfades.
        CHECK(walk.States[ac.Layers[2].Stack.back().State].Name == "Walk");
        CHECK(AnimatorLayerWeight(c, ac, 2) == 1);
        const float phase = ac.Layers[2].Stack.back().Phase;
        ac.SetBool("Sprint", true);
        advance(20);
        CHECK(gait.States[ac.Layers[1].Stack.back().State].Name == "IdleToSprint");
        CHECK(std::abs(ac.Layers[2].Stack.back().Phase - phase - 0.4f) < 1e-5f);
        CHECK(AnimatorLayerWeight(c, ac, 2) == 1);
        advance(60);
        CHECK(gait.States[ac.Layers[1].Stack.back().State].Name == "Sprint");
        CHECK(ac.StateName == "Sprint");
        CHECK(AnimatorLayerWeight(c, ac, 2) == 0);
        const float sprintPhase = ac.Layers[2].Stack.back().Phase;
        ac.SetBool("Sprint", false);
        advance(20);
        CHECK(gait.States[ac.Layers[1].Stack.back().State].Name == "SprintToIdle");
        CHECK(AnimatorLayerWeight(c, ac, 2) == 1);
        CHECK(std::abs(ac.Layers[2].Stack.back().Phase - sprintPhase - 0.4f) < 1e-5f);
        advance(60);
        CHECK(gait.States[ac.Layers[1].Stack.back().State].Name == "Walk");
        CHECK(ac.StateName == "Walk"); // Held base states cannot wait on an exit-time clock.
        CHECK(AnimatorLayerWeight(c, ac, 2) == 1);
        ac.SetFloat("Speed", 0);
        advance(30); // Walk -> Idle keeps the existing 0.4 s fade.
        CHECK(walk.States[ac.Layers[2].Stack.back().State].Name == "Off");
        CHECK(AnimatorLayerWeight(c, ac, 2) == 0);
        ac.SetFloat("Speed", 4);
        advance(15);
        ac.SetBool("Equipped", false);
        advance(15);
        CHECK(walk.States[ac.Layers[2].Stack.back().State].Name == "Off");
        CHECK(AnimatorLayerWeight(c, ac, 2) == 0);
    }
}
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

// The world body's hand anchor: on the gun within Near, on the body from Far, a smoothstep between; and a frame moved
// part of the way by a rigid move - none of it, all of it, and half its turn at half its weight, its scale kept.
void TestFirstPersonHandAnchor() {
    CHECK(FirstPersonAnchorWeight(0.0f, 0.05f, 0.15f) == 0.0f);
    CHECK(FirstPersonAnchorWeight(0.05f, 0.05f, 0.15f) == 0.0f);
    CHECK(std::abs(FirstPersonAnchorWeight(0.10f, 0.05f, 0.15f) - 0.5f) < 1e-5f);
    CHECK(FirstPersonAnchorWeight(0.15f, 0.05f, 0.15f) == 1.0f);
    CHECK(FirstPersonAnchorWeight(0.40f, 0.05f, 0.15f) == 1.0f);
    const glm::mat4 move = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.3f)) *
                           glm::mat4_cast(glm::angleAxis(glm::radians(40.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::mat4 hand = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, 1.2f, -0.4f)) *
                           glm::mat4_cast(glm::angleAxis(glm::radians(15.0f), glm::vec3(0.0f, 1.0f, 0.0f))) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto near = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c)
            if (glm::length(a[c] - b[c]) > 1e-4f) return false;
        return true;
    };
    CHECK(near(FirstPersonPartialMove(move, hand, 0.0f), hand));
    CHECK(near(FirstPersonPartialMove(move, hand, 1.0f), move * hand));
    const glm::mat4 half = FirstPersonPartialMove(move, hand, 0.5f);
    CHECK(glm::length(glm::vec3(half[3]) - 0.5f * (glm::vec3(hand[3]) + glm::vec3((move * hand)[3]))) < 1e-4f);
    CHECK(std::abs(glm::length(glm::vec3(half[0])) - 2.0f) < 1e-4f);
    const glm::quat turned = IK::Rotation(half) * glm::inverse(IK::Rotation(hand));
    CHECK(std::abs(glm::degrees(glm::angle(turned)) - 20.0f) < 0.01f);
}

// The third-person jank meter: a smooth ease is quiet, a one-frame step is one pop, a drift after the state has begun is a
// settle, a hand off its grip is flagged, and a shot's kick isn't.
void TestWeaponJankMeter() {
    auto run = [](auto pos, auto setup) {
        WeaponJankMeter m;
        for (int i = 0; i < 120; ++i) {
            WeaponJankFrame f;
            f.Dt = 1.0f / 60.0f;
            f.State = i < 60 ? "TacReload" : "Idle";
            f.Resting = i >= 60;
            f.GunPos = pos(i);
            setup(f, i);
            m.Push(f);
        }
        return m.Events();
    };
    auto none = [](WeaponJankFrame&, int) {};
    // An ease in over 30 frames (smoothstep): no pop, and over before the Idle: no settle.
    auto ease = run([](int i) { const float t = std::clamp(i / 30.0f, 0.0f, 1.0f); return glm::vec3(0.07f * t * t * (3.0f - 2.0f * t), 0, 0); }, none);
    CHECK(ease.empty());
    // A 2 cm step: a pop (one event though it disturbs two frames), at about 2 cm.
    auto step = run([](int i) { return glm::vec3(i >= 20 ? 0.02f : 0.0f, 0, 0); }, none);
    CHECK(step.size() == 1 && step[0].What == WeaponJankEvent::Kind::GunPop && step[0].Size > 1.5f && step[0].State == "TacReload");
    // The same step flagged as a shot's kick: nothing.
    auto kick = run([](int i) { return glm::vec3(i >= 20 ? 0.02f : 0.0f, 0, 0); }, [](WeaponJankFrame& f, int i) { f.Recoil = i >= 18 && i < 30; });
    CHECK(kick.empty());
    // 7 cm slid in slowly half a second into the Idle: a settle of about 7 cm.
    auto slide = run([](int i) { const float t = std::clamp((i - 90) / 25.0f, 0.0f, 1.0f); return glm::vec3(0, 0.07f * t * t * (3.0f - 2.0f * t), 0); }, none);
    bool settled = false;
    for (const WeaponJankEvent& e : slide) settled = settled || (e.What == WeaponJankEvent::Kind::Settle && e.Size > 5.0f && e.State == "Idle");
    CHECK(settled);
    // The same slide in the reload (not a resting state): the action's own move, nothing.
    auto inAction = run([](int i) { const float t = std::clamp((i - 20) / 25.0f, 0.0f, 1.0f); return glm::vec3(0, 0.07f * t * t * (3.0f - 2.0f * t), 0); }, none);
    CHECK(inAction.empty());
    // The left hand 3 cm off its grip for a stretch: one Grip event on that side.
    auto grip = run([](int) { return glm::vec3(0.0f); }, [](WeaponJankFrame& f, int i) { if (i > 40 && i < 50) f.HandGap[0] = 0.03f; });
    CHECK(grip.size() == 1 && grip[0].What == WeaponJankEvent::Kind::Grip && grip[0].Side == 0 && std::abs(grip[0].Size - 3.0f) < 0.01f);
}

// The third-person hold (FirstPersonPresentation::ThirdPersonGunCorrection): each weapon's controller plays an
// "arms3p" clip wherever it plays an "arms" one - that clip's ThirdPerson/ *_A_3P_* version (tools/weapons/add_3p_track.py).
void TestThirdPersonArmsTrack() {
    for (const char* rel : {"assets/Weapons/AKS74U/AKS74U.controller", "assets/Weapons/Remington870/Remington870.controller",
                            "assets/Weapons/Revolver38/Revolver38.controller"}) {
        const std::string path = ProjectPaths::Resolve(rel);
        AnimatorController ctrl;
        if (!std::filesystem::exists(path)) { // a source checkout without weapons; a broken file fails below
            UnitTestSupport::Skip("no " + path);
            continue;
        }
        CHECK(AnimatorController::LoadFile(path, ctrl));
        const bool has = std::find(ctrl.Tracks.begin(), ctrl.Tracks.end(), "arms3p") != ctrl.Tracks.end();
        CHECK(has);
        if (!has) continue;
        const int arms = ctrl.TrackIndex("arms"), third = ctrl.TrackIndex("arms3p");
        CHECK(arms != third);
        for (const auto& layer : ctrl.Layers)
            for (const auto& s : layer.States) {
                const auto& a = s.MotionFor(arms);
                const auto& b = s.MotionFor(third);
                if (a.Clip.empty()) continue;
                std::string want = a.Clip;
                const size_t fp = want.find("/FirstPerson/"), tag = want.find("_A_FP_");
                CHECK(fp != std::string::npos && tag != std::string::npos);
                if (fp == std::string::npos || tag == std::string::npos) continue;
                want.replace(tag, 6, "_A_3P_");
                want.replace(fp, 13, "/ThirdPerson/");
                CHECK(b.Clip == want);
            }
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
    if (!std::filesystem::exists(path)) { // a source checkout without weapons; a broken file fails below
        UnitTestSupport::Skip("no " + path);
        return;
    }
    if (!AnimatorController::LoadFile(path, ctrl)) {
        CHECK(!"the controller loads");
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
    CHECK(Gameplay::ResolveWeaponGameplay("assets/Weapons/Remington870/Remington870.prefab", set.Gameplay, error));

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
            CHECK(!set.WeaponMaterials.empty());
            if (!set.WeaponMaterials.empty())
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

// The .38 Special revolver (project/assets/Weapons/Revolver38, tools/weapons/make_revolver38_*.py): one Reload state
// that ejects all six (one Eject event per chamber, eject.origins) and refills once the crane shuts; every state's
// clips attach to the rigs; the definition beside it validates clean.
void TestRevolver38Assets() {
    namespace K = FirstPersonAnimatorContract;
    const std::string path = ProjectPaths::Resolve("assets/Weapons/Revolver38/Revolver38.controller");
    AnimatorController ctrl;
    if (!std::filesystem::exists(path)) { // a source checkout without weapons; a broken file fails below
        UnitTestSupport::Skip("no " + path);
        return;
    }
    if (!AnimatorController::LoadFile(path, ctrl)) {
        CHECK(!"the controller loads");
        return;
    }
    constexpr int kCylinder = 6;
    AnimatorControllerComponent ac;
    const auto oneSecond = [](int, int) { return 1.0f; };
    int ammo = kCylinder, ejects = 0, refills = 0;
    auto step = [&](float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 1.0f / 60.0f) {
            ac.SetInt(K::kAmmo, ammo);
            AdvanceAnimator(ctrl, ac, 1.0f / 60.0f, oneSecond);
            for (const std::string& e : ac.FiredEvents) {
                if (e == K::kEventEject) ++ejects;
                if (e == K::kEventRefill) { ammo = kCylinder; ++refills; }
            }
            ac.FiredEvents.clear();
            for (const char* tr : {K::kFire, K::kReload}) ac.ResetTrigger(tr);
        }
    };
    ac.SetBool(K::kEquipped, true);
    step(0.5f);
    CHECK(ac.StateName == "Idle");
    // Full: Reload does nothing.
    ac.SetTrigger(K::kReload);
    step(0.1f);
    CHECK(ac.StateName == "Idle");
    // Two left: the crane swings out, six come out, one refill, back to Idle.
    ammo = 2;
    ac.SetTrigger(K::kReload);
    step(0.1f);
    CHECK(ac.StateName == "Reload" && ac.HasTag(K::kTagReload));
    ac.SetTrigger(K::kFire);
    step(0.1f);
    CHECK(ac.StateName == "Reload"); // a shot can't interrupt the reload
    step(2.0f);
    CHECK(ejects == kCylinder && refills == 1 && ammo == kCylinder);
    CHECK(ac.StateName == "Idle");
    // A shot: Fire plays and comes back.
    ac.SetTrigger(K::kFire);
    step(0.1f);
    CHECK(ac.StateName == "Fire");
    step(1.5f);
    CHECK(ac.StateName == "Idle");

    FirstPersonAnimationSet set;
    std::string error;
    CHECK(FirstPersonAnimationSet::LoadFile(ProjectPaths::Resolve("assets/Weapons/Revolver38/Revolver38.fpsanim"), set, &error));
    CHECK(Gameplay::ResolveWeaponGameplay("assets/Weapons/Revolver38/Revolver38.prefab", set.Gameplay, error));
    CHECK(set.Gameplay.Magazine == kCylinder && set.Gameplay.Reload == FirstPersonWeaponGameplay::ReloadMode::Magazine &&
          !set.Gameplay.CycleAfterShot && set.Gameplay.Pellets <= 1 && !set.Muzzle.Auto);
    CHECK(set.Eject.When == FirstPersonEjectSettings::Trigger::Event && set.Eject.Origins.size() == (size_t)kCylinder &&
          !set.Eject.LiveModel.empty() && !set.StockLock.Enabled && set.WeaponRoot == "Root");
    if (std::filesystem::exists(ProjectPaths::Resolve(set.ArmsModel)) && std::filesystem::exists(ProjectPaths::Resolve(set.WeaponModel))) {
        const auto arms = Model::ImportDeferred(ProjectPaths::Resolve(set.ArmsModel), ModelImportSettings{});
        const auto weapon = Model::ImportDeferred(ProjectPaths::Resolve(set.WeaponModel), ModelImportSettings{});
        CHECK(arms && weapon);
        if (arms && weapon) {
            std::map<std::string, std::shared_ptr<Model>> clips;
            auto attaches = [&](Model& target, const std::string& ref) {
                auto& src = clips[ref];
                if (!src) src = Model::ImportDeferred(ProjectPaths::Resolve(ref), ModelImportSettings{});
                return src && src->OwnAnimationCount() > 0 && target.AttachClip(*src, 0, ref, ref) >= 0;
            };
            const int at = ctrl.TrackIndex("arms"), wt = ctrl.TrackIndex("weapon"), tt = ctrl.TrackIndex("arms3p");
            for (const auto& st : ctrl.Layers[0].States) {
                const bool armsOk = attaches(*arms, st.MotionFor(at).Clip);
                const bool weaponOk = attaches(*weapon, st.MotionFor(wt).Clip);
                const bool thirdOk = tt < 0 || attaches(*arms, st.MotionFor(tt).Clip);
                CHECK(armsOk && weaponOk && thirdOk);
                if (!armsOk || !weaponOk || !thirdOk)
                    std::printf("  state '%s' can't attach its %s clip\n", st.Name.c_str(), !armsOk ? "arms" : !weaponOk ? "weapon" : "3P");
            }
            CHECK(arms->NodeIndex(set.WeaponSocket) >= 0 && weapon->NodeIndex(set.WeaponRoot) >= 0);
            for (const char* bone : {"Cylinder", "Bullet1", "Bullet6"}) CHECK(weapon->NodeIndex(bone) >= 0);
        }
    }
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
    set.Gameplay = FirstPersonWeaponGameplay{};
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

// --- Procedural animation: the first-person weapon stack --------------------------------------
void TestWeaponProcedural() {
    // Settings round-trip through the .fpsanim "procedural" block.
    const WeaponProceduralSettings defaults = WeaponProceduralSettings::Defaults();
    WeaponProceduralSettings loaded;
    std::string error;
    CHECK(WeaponProceduralSettings::FromJson(defaults.ToJson(), loaded, &error));
    CHECK(std::fabs(loaded.Recoil.Rotation.X.Evaluate(0.2f) - defaults.Recoil.Rotation.X.Evaluate(0.2f)) < 1e-4f);
    CHECK(loaded.Bob.SprintStride == defaults.Bob.SprintStride && loaded.Bob.Ease == defaults.Bob.Ease);
    CHECK(loaded.StateOffsets.size() == defaults.StateOffsets.size() && loaded.IK.GunBone == "ik_hand_gun");
    WeaponProceduralSettings untouched = defaults;
    CHECK(!WeaponProceduralSettings::FromJson(json::parse(R"({"recoil":{"duration":0}})"), untouched, &error));
    CHECK(error.find("duration") != std::string::npos);
    CHECK(!WeaponProceduralSettings::FromJson(json::parse(R"({"breath":{"position":{"x":[[0,"a"]]}}})"), untouched, &error));
    CHECK(untouched.Recoil.Duration == defaults.Recoil.Duration); // a bad load changes nothing
    // Partial blocks keep everything they don't name.
    CHECK(WeaponProceduralSettings::FromJson(json::parse(R"({"lean":{"angle":20}})"), untouched, &error));
    CHECK(untouched.Lean.Angle == 20.0f && untouched.Lean.Offset == defaults.Lean.Offset);

    // One layer at a time, everything else off, so each can be read in isolation.
    WeaponProceduralSettings s = defaults;
    s.Sway.Enabled = s.Breath.Enabled = s.Lean.Enabled = false;
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
        CHECK(std::fabs(p.CameraRoll + k.Lean.Angle) < 0.05f && std::fabs(p.CameraSide - k.Lean.Offset) < 0.01f);
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

    // Values the Inspector can produce mid-edit never break the stack: zero lengths stay finite,
    // a Min Rate above Max Rate is read in order, and switching IK off keeps the motion (the
    // presentation moves the whole view model instead) rather than fading it away.
    {
        WeaponProceduralSettings z = WeaponProceduralSettings::Defaults();
        z.Recoil.Duration = 0.0f;
        z.Bob.WalkStride = z.Bob.SprintStride = 0.0f;
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
        CHECK(std::fabs(run(st, l, in, 2.0f).CameraRoll - l.Lean.Angle) < 0.05f);
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
        b.CameraMotion.Enabled = false;
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

        // Lean: the head arcs over (slides Offset, drops a little, rolls Angle); the gun's roll
        // lags behind it early on.
        {
            WeaponProceduralSettings ls = f;
            ls.Lean.Enabled = true;
            WeaponProceduralState lst;
            WeaponProceduralInput right;
            right.Lean = 1.0f;
            const WeaponProceduralPose early = run(lst, ls, right, 0.08f);
            const float headFrac = -early.CameraRoll / ls.Lean.Angle;
            const float gunFrac = -early.Rotation.z / ls.Lean.WeaponRoll;
            CHECK(headFrac > 0.02f && gunFrac < headFrac);
            const WeaponProceduralPose full = run(lst, ls, right, 3.0f);
            const float radius = ls.Lean.Offset / std::sin(glm::radians(ls.Lean.Angle));
            CHECK(std::fabs(full.CameraSide - ls.Lean.Offset) < 2e-3f);
            CHECK(std::fabs(full.CameraOffset.y + radius * (1.0f - std::cos(glm::radians(ls.Lean.Angle)))) < 2e-3f);
            CHECK(std::fabs(full.CameraRoll + ls.Lean.Angle) < 0.1f && std::fabs(full.Rotation.z + ls.Lean.WeaponRoll) < 0.1f);
            Camera view;
            const glm::vec3 unrolledRight=view.Right();
            view.Roll=full.CameraRoll;
            CHECK(glm::dot(view.Up(),unrolledRight)>0); // right peek tilts right in the actual camera basis
            WeaponProceduralState leftState;
            right.Lean=-1;
            const auto leftPose=run(leftState,ls,right,3.0f);
            view.Roll=leftPose.CameraRoll;
            CHECK(leftPose.CameraSide<0 && glm::dot(view.Up(),unrolledRight)<0);
        }

        // The new blocks round-trip.
        WeaponProceduralSettings edited = defaults;
        edited.Jump.LandPitch = 2.5f;
        edited.CameraMotion.SprintBob = glm::vec2(0.02f, 0.9f);
        edited.Obstruction.Reach = 1.1f;
        edited.Breath.ExertionScale = 3.0f;
        edited.Recoil.FovPunch = -0.4f;
        WeaponProceduralSettings back = WeaponProceduralSettings::Defaults();
        std::string err;
        CHECK(WeaponProceduralSettings::FromJson(edited.ToJson(), back, &err));
        CHECK(back.Jump.LandPitch == 2.5f && back.CameraMotion.SprintBob == glm::vec2(0.02f, 0.9f) &&
              back.Obstruction.Reach == 1.1f && back.Breath.ExertionScale == 3.0f && back.Recoil.FovPunch == -0.4f);
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
        // A revolver's cylinder: one origin per chamber and a live-round model, both round-tripped; absent, neither is written.
        const std::string cyl = R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c.controller", "eject": {"model": "assets/case.fbx",
            "liveModel": "assets/round.fbx", "origins": [[0.01, 0.02, 0.03], [0.04, 0.05, 0.06]], "trigger": "event"}})";
        FirstPersonAnimationSet rev;
        CHECK(FirstPersonAnimationSet::FromJsonString(cyl, rev, &why));
        CHECK(rev.Eject.LiveModel == "assets/round.fbx" && rev.Eject.Origins.size() == 2 && rev.Eject.Origins[1] == glm::vec3(0.04f, 0.05f, 0.06f));
        FirstPersonAnimationSet revBack;
        CHECK(FirstPersonAnimationSet::FromJsonString(rev.ToJsonString(), revBack, &why));
        CHECK(revBack.Eject.LiveModel == rev.Eject.LiveModel && revBack.Eject.Origins == rev.Eject.Origins);
        CHECK(json::parse(set.ToJsonString())["eject"].count("origins") == 0 && json::parse(set.ToJsonString())["eject"].count("liveModel") == 0);
        CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c", "eject": {"model": "m.fbx", "origins": [[0, 1]]}})", bad, &why));
        CHECK(!FirstPersonAnimationSet::FromJsonString(R"({"armsModel": "a.fbx", "weaponModel": "w.fbx", "controller": "c", "eject": {"model": "m.fbx", "origins": 3}})", bad, &why));
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
    set.HasLegacyGameplay = true; // old descriptor write/read compatibility; current prefabs own these stats
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
    CHECK(none.Muzzle.Auto && none.Laser.Enabled && std::abs(none.Gameplay.BulletHoleRadius - 0.004f) < 1e-6f);
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

} // namespace

void RegisterFirstPersonWeaponsTests(UnitTestSupport::TestList& tests) {
    tests.insert(tests.end(), {
        {"RecoilProfile", TestRecoilProfile},
        {"FirstPersonWeaponWizard", TestFirstPersonWeaponWizard},
        {"FirstPersonWeaponValidate", TestFirstPersonWeaponValidate},
        {"FirstPersonAnimationSet", TestFirstPersonAnimationSet},
        {"Gait phase sync", TestGaitPhaseSync},
        {"AK additive ADS walking", TestAKAdditiveWalking},
        {"Sprint transition additive walk", TestSprintTransitionWalk},
        {"FirstPersonShotgunSet", TestFirstPersonShotgunSet},
        {"FirstPersonHandAnchor", TestFirstPersonHandAnchor},
        {"WeaponJankMeter", TestWeaponJankMeter},
        {"ThirdPersonArmsTrack", TestThirdPersonArmsTrack},
        {"RemingtonController", TestRemingtonController},
        {"Revolver38Assets", TestRevolver38Assets},
        {"FirstPersonAnimationFSM", TestFirstPersonAnimationFSM},
        {"FirstPersonAds", TestFirstPersonAds},
        {"WeaponProcedural", TestWeaponProcedural},
        {"ShellCasings", TestShellCasings},
        {"BulletHoles", TestBulletHoles},
        {"WeaponZero", TestWeaponZero},
    });
}
