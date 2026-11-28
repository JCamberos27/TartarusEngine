// Unit tests: The first-person body: bone mapping, tuning, the 2D blend tree, clothing hidden from the camera and the body validator.
// (Moved out of UnitTests.cpp; registered by RunUnitTests through RegisterFirstPersonBodyTests.)
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

// A Bone Map renames the standard bones to a rig's own: parsed leniently, and the validator looks for the mapped names.
void TestFirstPersonBodyBoneMap() {
    const auto m = FPBody::ParseBoneMap(" pelvis = Hips ,foot_l=LeftFoot\nfoot_r = RightFoot, junk, =x, y=");
    CHECK(m.size() == 3 && m.at("pelvis") == "Hips" && m.at("foot_l") == "LeftFoot" && m.at("foot_r") == "RightFoot");
    CHECK(FPBody::MappedBone(m, "pelvis") == "Hips" && FPBody::MappedBone(m, "spine_01") == "spine_01");

    FirstPersonBodyComponent cfg;
    cfg.FootIK = true;
    FPBody::ValidationInput in;
    in.Config = &cfg;
    in.HasPieces = in.HasDriverPiece = true;
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

// The body's tuning fields: every float has a tooltip, a range that holds its default, and the
// defaults are the values the body was tuned with (so a scene saved before they existed plays the same).
void TestFirstPersonBodyTuning() {
    if (ComponentRegistry::All().empty()) ComponentRegistry::RegisterEngineComponents();
    FirstPersonBodyComponent body;
    FirstPersonControllerComponent ctrl;
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-5f; };
    CHECK(near(body.EyeSlack, 0.035f) && near(body.ReachSlack, 0.04f) && near(body.ShrugStart, 0.98f) && near(body.ShrugMax, 0.12f));
    CHECK(near(body.TurnEndAngle, 8.0f));
    CHECK(near(body.FootLockDrift, 0.25f) && body.FootLockEnabled && near(body.FootLockRelease, 0.2f) && near(body.FootRayUp, 0.5f) && near(body.FootRayLength, 1.0f) &&
          near(body.FootMaxRaise, 0.25f) && near(body.PelvisMaxRaise, 0.0f) && near(body.FootTiltMax, 25.0f) &&
          near(body.StairEase, 0.2f));
    CHECK(near(ctrl.JumpBufferTime, 0.12f) && near(ctrl.CoyoteTime, 0.10f));
    // Walk is the default gait (the walk clips' speed), Shift the jog (the jog clips').
    CHECK(near(body.WalkSpeed, 1.53f) && near(body.RunSpeed, 3.26f) && body.WalkSpeed < body.RunSpeed);
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
    // The eye: steady at the standing head with no bob, following it fully at 1, offset in the
    // body's frame (x right = model -X).
    const glm::vec3 rest(0.0f, 1.6f, 0.0f), head(0.02f, 1.55f, 0.1f);
    glm::vec3 eye = FirstPersonBodyEye(rest, head, 0.0f, glm::vec3(0.0f));
    CHECK(near(eye.y, 1.6f) && near(eye.z, 0.0f));
    eye = FirstPersonBodyEye(rest, head, 1.0f, glm::vec3(0.1f, 0.05f, 0.2f));
    CHECK(near(eye.x, 0.02f - 0.1f) && near(eye.y, 1.6f) && near(eye.z, 0.3f));
    eye = FirstPersonBodyEye(rest, head, 0.5f, glm::vec3(0.0f));
    CHECK(near(eye.y, 1.575f) && near(eye.z, 0.05f));

    // Angles wrap to (-pi, pi].
    CHECK(near(FirstPersonBodyWrapAngle(3.5f), 3.5f - 6.2831853f) && near(FirstPersonBodyWrapAngle(-3.5f), -3.5f + 6.2831853f));
    CHECK(near(FirstPersonBodyWrapAngle(0.4f), 0.4f) && near(FirstPersonBodyWrapAngle(6.2831853f + 0.4f), 0.4f));

    // The chest's twist toward the view: eased (half the way in the half-life), capped, and never swapping sides in a frame.
    {
        const float dt = 1.0f / 60.0f, lim = glm::radians(60.0f), hl = 0.08f, band = glm::radians(30.0f), pi = 3.14159265f;
        float e = 0.0f;
        int side = 0;
        for (int i = 0; i < 5; ++i) e = FirstPersonBodyEaseTwist(e, side, -1.0f, lim, hl, band, dt); // 0.083 s toward -1 rad
        CHECK(e < -0.45f && e > -0.6f && side == -1);
        CHECK(near(FirstPersonBodyEaseTwist(0.3f, side, 2.0f, lim, 0.0f, band, dt), lim)); // no half-life: capped, at once
        // A 200 deg flick in 0.15 s (the view sweeps round through straight behind and stops 20 deg past it), then the body
        // turns the short way: the chest stays on the flick's side until the view is clearly round the other way, and no
        // frame moves it more than the ease allows.
        e = 0.0f;
        side = 0;
        const float most = 2.0f * lim * (1.0f - std::exp(-0.69314718f * dt / hl)) + 1e-5f;
        float worst = 0.0f, body = 0.0f;
        bool flippedEarly = false;
        for (int i = 0; i < 120; ++i) {
            const float u = std::min(1.0f, i / 9.0f), view = -glm::radians(200.0f) * u * u * (3.0f - 2.0f * u);
            if (i > 9) body = std::min(body + glm::radians(180.0f) * dt, glm::radians(100.0f)); // the body turns the short way (+)
            const float twist = FirstPersonBodyWrapAngle(view - body);
            const float before = e;
            e = FirstPersonBodyEaseTwist(e, side, twist, lim, hl, band, dt);
            worst = std::max(worst, std::abs(e - before));
            if (std::abs(twist) > pi - band && e > 0.0f) flippedEarly = true;
            CHECK(std::abs(e) <= lim + 1e-5f);
        }
        CHECK(worst <= most && !flippedEarly);
        CHECK(e > 0.3f && side == 1); // round the other way in the end, eased there
    }

    // Foot IK: the pelvis drops to the lower foot (capped), rises a little when both are up.
    CHECK(near(FirstPersonBodyFootPelvis(-0.1f, 0.0f, 0.35f, 0.15f), -0.1f) && near(FirstPersonBodyFootPelvis(0.05f, -0.5f, 0.35f, 0.15f), -0.35f));
    CHECK(near(FirstPersonBodyFootPelvis(0.2f, 0.3f, 0.35f, 0.15f), 0.15f) && near(FirstPersonBodyFootPelvis(0.0f, 0.0f, 0.35f, 0.15f), 0.0f));
    // Stairs: the ground as a ramp. A slope is fitted exactly; 20 cm steps on 40 cm treads under a 0.8 m window are a
    // half slope, fitted within a quarter of it whichever way the edges fall (from the ends alone it read 0.27 or 0.53).
    {
        float h[16], height = 0.0f, slope = 0.0f;
        for (int i = 0; i < 16; ++i) h[i] = 0.3f * (0.75f * ((float)i / 15.0f - 0.5f)) + 0.1f;
        FirstPersonBodyGroundRamp(h, 16, 0.75f, height, slope);
        CHECK(near(height, 0.1f) && near(slope, 0.3f));
        float worst = 0.0f;
        for (float shift = 0.0f; shift < 0.4f; shift += 0.01f) {
            for (int i = 0; i < 16; ++i) h[i] = 0.2f * std::floor((0.8f * ((i + 0.5f) / 16.0f - 0.5f) + shift) / 0.4f);
            FirstPersonBodyGroundRamp(h, 16, 0.75f, height, slope);
            worst = std::max(worst, std::abs(slope - 0.5f));
        }
        CHECK(worst < 0.13f);
    }
    // The body's height spring: fed a ramp's rate it follows the ramp without lag; a step it takes smoothly (no
    // overshoot, its acceleration bounded - its speed never jumps).
    {
        float y = 0.0f, v = 0.5f, t = 0.0f;
        for (int i = 0; i < 120; ++i, t += 1.0f / 60.0f) FirstPersonBodyStepSpring(y, v, 0.5f * (t + 1.0f / 60.0f), 0.5f, 0.2f, 1.0f / 60.0f);
        CHECK(std::abs(y - 0.5f * t) < 0.01f);
        y = v = 0.0f;
        float top = 0.0f, jump = 0.0f, last = 0.0f;
        for (int i = 0; i < 120; ++i) {
            FirstPersonBodyStepSpring(y, v, 0.2f, 0.0f, 0.2f, 1.0f / 60.0f);
            top = std::max(top, y);
            jump = std::max(jump, std::abs(v - last));
            last = v;
        }
        CHECK(near(y, 0.2f) && top <= 0.2001f && jump * 60.0f < 20.5f); // at most w^2 * the step (w = 2 / 0.2 s)
    }
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

    // The Quantum pack's hooded coat, hood up: its hood and collar are found, its body and sleeves aren't. Needs the pack (project content).
    const std::string jacketPath = "assets/Characters/Quantum/Models/Clothing/Male/Outerwear/SKM_Coat_Hoodie_Hood.fbx";
    if (!std::filesystem::exists(ProjectPaths::Resolve(jacketPath))) {
        UnitTestSupport::Skip("no Quantum pack: model checks");
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
    Log::Info("FirstPersonBodyClothingHide: hooded coat " + std::to_string(hidden) + " / " + std::to_string(pos.size()) +
              " collar vertices; shoulders at " + std::to_string(jL.y) + " m, neck " + std::to_string(jNeck.y) + " m, top " +
              std::to_string(yMax) + " m.");
    CHECK(jL.y > 1.2f && jL.y < 1.7f && jNeck.y > jL.y); // bones and vertices share the model's upright, metre frame
    // This coat's hood is up: all of it is round the camera.
    CHECK(hidden > 0 && hidden < pos.size() && head > 0 && headHidden == head);
    CHECK(top > 0 && topHidden == top);
    CHECK(low == 0);
}

// The body's setup check: a controller built from the contract tables passes; one missing piece is named.
void TestFirstPersonBodyValidate() {
    FirstPersonBodyComponent cfg;
    cfg.FootIK = true; cfg.WeaponArms = true;
    cfg.SpineAim = 1.0f;
    FPBody::ValidationInput in;
    in.Config = &cfg;
    in.HasPieces = in.HasDriverPiece = true;
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
    CHECK(count(r, FPBody::Severity::Error) == 0 && count(r, FPBody::Severity::Warning) == 0);

    // A missing bone and piece are each named.
    in.HasBone = [](const std::string& b) { return b != "foot_l"; };
    in.PieceNames = {"Quantum_Head"};
    r = FPBody::Validate(in);
    CHECK(has(r, "'foot_l'") && has(r, "'Arms' (Arms Piece)"));
    CHECK(r.front().Level == FPBody::Severity::Warning);
    // The Head Bone is looked up through the Bone Map, as the body finds it.
    cfg.BoneMap = "head = Head";
    in.HasBone = [](const std::string& b) { return b != "head"; };
    in.PieceNames = {"Quantum_Head", "Quantum_Arms"};
    CHECK(!has(FPBody::Validate(in), "(Head Bone)"));
    cfg.BoneMap.clear();
    CHECK(has(FPBody::Validate(in), "(Head Bone)"));
    // A modular body's hands are part of the arms (under a long sleeve they are all there is of them).
    in.HasBone = [](const std::string&) { return true; };
    in.PieceNames = {"Quantum_Head", "Hands"};
    CHECK(!has(FPBody::Validate(in), "(Arms Piece)"));
    FirstPersonBodyComponent armsCfg;
    CHECK(FPBody::IsArmsPiece(armsCfg, "Arms") && FPBody::IsArmsPiece(armsCfg, "Quantum_HANDS") && !FPBody::IsArmsPiece(armsCfg, "Torso"));
    armsCfg.HandsPiece.clear();
    CHECK(!FPBody::IsArmsPiece(armsCfg, "Hands") && FPBody::IsArmsPiece(armsCfg, "Arms"));

    FirstPersonBodyComponent plain;

    // Structural errors come first.
    FPBody::ValidationInput in3;
    in3.Config = &plain;
    r = FPBody::Validate(in3);
    CHECK(!r.empty() && r.front().Level == FPBody::Severity::Error);
}

} // namespace

// Twins without motion matching (Start needs the licensed mocap, which CI doesn't have): the pieces and their models as
// Start would take them, then MakeTwins.
struct FirstPersonBodyTestAccess {
    static void MakeTwins(FirstPersonBody& b, World& world, entt::entity body, const std::vector<entt::entity>& pieces) {
        b.m_Body = body;
        b.m_Pieces = pieces;
        b.m_Models.clear();
        for (entt::entity p : pieces) b.m_Models.push_back(world.Registry.get<RenderableComponent>(p).ModelRef);
        const auto& outfit = world.Registry.get<CharacterOutfitComponent>(body);
        b.m_OutfitVersion = outfit.Version;
        b.m_OutfitLookVersion = outfit.LookVersion;
        b.MakeTwins(world);
    }
    // What a Restart did to the twins (Stop destroys them, Start makes new ones).
    static void RemakeTwins(FirstPersonBody& b, World& world) {
        for (entt::entity t : b.m_Twins) world.DestroyEntityAndChildren(t);
        b.m_Twins.clear();
        b.m_TwinModels.clear();
        b.MakeTwins(world);
    }
    static const std::vector<entt::entity>& Twins(const FirstPersonBody& b) { return b.m_Twins; }
};

namespace {

// A colourway landing in Play (OutfitSystem::SetColourway bumps only the look version) is copied onto the world twins
// in place: the same twin entities, no structure change for the transform cache, LOD or the gather to rebuild over.
void TestFirstPersonBodyLookOnlyKeepsTwins() {
    World world;
    auto prim = [] { return Model::CreatePrimitive("terrain", "primitive://terrain#0"); }; // CPU-only: no GL in unit tests
    const entt::entity body = world.CreateModelEntity(prim(), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Body");
    world.Registry.emplace<FirstPersonBodyComponent>(body);
    world.Registry.emplace<CharacterOutfitComponent>(body);
    std::vector<entt::entity> pieces;
    for (int i = 0; i < 2; ++i) {
        const entt::entity p = world.CreateModelEntity(prim(), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Piece");
        CHECK(world.SetParent(p, body));
        pieces.push_back(p);
    }
    FirstPersonBody fpb;
    FirstPersonBodyTestAccess::MakeTwins(fpb, world, body, pieces);
    const std::vector<entt::entity> twins = FirstPersonBodyTestAccess::Twins(fpb);
    CHECK(twins.size() == 2);
    world.RebuildWorldTransformCache();
    world.RebuildWorldTransformCache(); // settled

    // The look changes (what SetColourway, UpdateHiding and a layer edit leave on the pieces).
    auto mat = std::make_shared<MaterialAsset>();
    world.Registry.get<RenderableComponent>(pieces[0]).Materials = {mat};
    world.Registry.emplace<OutfitHideTag>(pieces[0]).Hidden = 5;
    world.Registry.emplace<OutfitLayerTag>(pieces[1]).Pull = 0.004f;
    world.Registry.emplace_or_replace<LayerComponent>(pieces[1]).Layer = 3;
    ++world.Registry.get<CharacterOutfitComponent>(body).LookVersion;

    // PlayFrame's choice: a look-only change syncs, never restarts.
    CHECK(!fpb.OutfitChanged(world));
    CHECK(fpb.OutfitLookChanged(world));
    fpb.SyncLook(world);
    CHECK(!fpb.OutfitLookChanged(world));
    CHECK(FirstPersonBodyTestAccess::Twins(fpb) == twins);
    for (entt::entity t : twins) CHECK(world.Registry.valid(t));
    const auto& r = world.Registry;
    CHECK(r.get<RenderableComponent>(twins[0]).Materials.size() == 1 && r.get<RenderableComponent>(twins[0]).Materials[0] == mat);
    CHECK(r.all_of<OutfitHideTag>(twins[0]) && r.get<OutfitHideTag>(twins[0]).Hidden == 5);
    CHECK(r.all_of<OutfitLayerTag>(twins[1]) && r.get<OutfitLayerTag>(twins[1]).Pull == 0.004f);
    CHECK(r.all_of<LayerComponent>(twins[1]) && r.get<LayerComponent>(twins[1]).Layer == 3);

    const std::uint64_t fullWalks = world.TransformCacheFullWalks();
    world.RebuildWorldTransformCache();
    const auto& d = world.TransformStructureChanges();
    CHECK(d.Valid && d.Added.empty() && d.Reparented.empty() && d.Dropped.empty());
    CHECK(world.TransformCacheFullWalks() == fullWalks);

    // The pieces' version still restarts: and a restart's new twins are a structure change (what the look version saves).
    ++world.Registry.get<CharacterOutfitComponent>(body).Version;
    CHECK(fpb.OutfitChanged(world));
    FirstPersonBodyTestAccess::RemakeTwins(fpb, world);
    world.RebuildWorldTransformCache();
    const auto& d2 = world.TransformStructureChanges();
    CHECK(!d2.Valid || !d2.Added.empty() || !d2.Dropped.empty());
    CHECK(FirstPersonBodyTestAccess::Twins(fpb) != twins);
}

} // namespace

void RegisterFirstPersonBodyTests(UnitTestSupport::TestList& tests) {
    tests.insert(tests.end(), {
        {"FirstPersonBodyBoneMap", TestFirstPersonBodyBoneMap},
        {"FirstPersonBodyTuning", TestFirstPersonBodyTuning},
        {"BlendTree2D", TestBlendTree2D},
        {"FirstPersonBodyClothingHide", TestFirstPersonBodyClothingHide},
        {"FirstPersonBodyValidate", TestFirstPersonBodyValidate},
        {"FirstPersonBodyLookOnlyKeepsTwins", TestFirstPersonBodyLookOnlyKeepsTwins},
    });
}
