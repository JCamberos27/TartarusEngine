#include "FirstPersonPresentation.h"

#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "IK.h"
#include "Log.h"
#include "GameModuleAPI.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "RotationMath.h"
#include "World.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace K = FirstPersonAnimatorContract;

namespace {

glm::quat CameraRotation(const Camera& camera) {
    // Camera::Front is local -Z, so its world transform's +Z basis is -Front.
    glm::mat3 basis(1.0f);
    basis[0] = camera.Right();
    basis[1] = camera.Up();
    basis[2] = -camera.Front();
    return NormalizeRotation(glm::quat_cast(basis));
}

bool FinitePositive(float value) {
    return std::isfinite(value) && value > 0.0f;
}

bool SameIKSetup(const WeaponIKSettings& a, const WeaponIKSettings& b) {
    return a.Enabled == b.Enabled && a.GunBone == b.GunBone && a.RightUpper == b.RightUpper &&
           a.RightLower == b.RightLower && a.RightHand == b.RightHand && a.LeftUpper == b.LeftUpper &&
           a.LeftLower == b.LeftLower && a.LeftHand == b.LeftHand;
}

std::string TrackOr(const AnimatorController& ctrl, const char* wanted, int fallback) {
    for (const auto& t : ctrl.Tracks) if (t == wanted) return t;
    return ctrl.Tracks[std::clamp(fallback, 0, (int)ctrl.Tracks.size() - 1)];
}

} // namespace

void FirstPersonPresentation::SetError(const std::string& message) {
    m_LastError = message;
    Log::Error("First-person presentation: " + message);
}

bool FirstPersonPresentation::Start(World& world, AssetLibrary& assets,
                                    const FirstPersonControllerComponent& config) {
    Stop(world);
    m_LastError.clear();
    for (const std::string* set : {&config.AnimationSet, &config.SecondaryAnimationSet})
        if (!set->empty()) m_SlotSets.push_back(*set);
    if (m_SlotSets.empty()) return true;
    m_SlotAmmo.assign(m_SlotSets.size(), -1);
    m_Config = std::make_shared<FirstPersonControllerComponent>(config);
    m_SlotAssets = &assets;
    m_Slot = 0;
    m_PendingSlot = -1;
    m_WalkSpeed = config.MoveSpeed;
    m_SprintSpeed = config.MoveSpeed * config.SprintMultiplier;
    return StartSet(world, assets, 0, false);
}

void FirstPersonPresentation::Stop(World& world) {
    StopSet(world);
    m_SlotSets.clear();
    m_SlotAmmo.clear();
    m_Slot = 0;
    m_PendingSlot = -1;
    m_SlotAssets = nullptr;
    m_Config.reset();
}

bool FirstPersonPresentation::StartSet(World& world, AssetLibrary& assets, int slot, bool holstered) {
    StopSet(world);
    m_LastError.clear();
    const FirstPersonControllerComponent& config = *m_Config;
    const std::string& animationSet = m_SlotSets[slot];
    m_Slot = slot;

    const std::string setPath = ProjectPaths::Resolve(animationSet);
    m_SetFile = std::filesystem::u8path(setPath);
    {
        std::error_code ec;
        m_SetFileTime = std::filesystem::last_write_time(m_SetFile, ec);
    }
    if (!FirstPersonAnimationSet::LoadFile(setPath, m_Set, &m_LastError)) {
        SetError("could not load '" + animationSet + "': " + m_LastError);
        return false;
    }
    if (!FinitePositive(config.ViewModelScale)) {
        SetError("View Model Scale must be finite and greater than zero");
        return false;
    }

    // The controller: the weapon's own .controller, or - for a v1 clip list - the standard
    // first-person graph built in memory.
    std::shared_ptr<const AnimatorController> ctrl;
    if (!m_Set.Controller.empty()) {
        m_ControllerPath = m_Set.Controller;
        ctrl = GetAnimatorController(m_ControllerPath);
        if (!ctrl) {
            SetError("could not load the Animator Controller '" + m_Set.Controller + "' named by '" + animationSet + "'");
            return false;
        }
    } else {
        m_ControllerPath = "memory:" + animationSet;
        ctrl = std::make_shared<const AnimatorController>(BuildFirstPersonController(m_Set));
        RegisterAnimatorController(m_ControllerPath, ctrl);
    }
    if (ctrl->Layers.empty() || ctrl->Layers[0].States.empty()) {
        SetError("the Animator Controller for '" + animationSet + "' has no states");
        return false;
    }

    const std::string armsPath = ProjectPaths::Resolve(m_Set.ArmsModel);
    const std::string weaponPath = ProjectPaths::Resolve(m_Set.WeaponModel);
    auto arms = assets.InstantiateModel(armsPath);
    auto weapon = assets.InstantiateModel(weaponPath);
    if (!arms || !weapon) {
        SetError("could not instantiate arms or weapon model from '" + animationSet + "'");
        return false;
    }

    // Created after the Play snapshot; Stop destroys them before restoring authored scene data.
    m_World = &world;
    m_ArmsModel = arms;
    m_WeaponModel = weapon;
    // StockWorld's vertices belong to the last weapon: a new one can be allocated where it was freed (a
    // weapon swap), so a pointer compare alone would read the old weapon's indices into the new one's mesh.
    m_StockModel = nullptr;
    m_StockVerts.clear();
    m_Arms = world.CreateModelEntity(std::move(arms), glm::vec3(0.0f), glm::vec3(0.0f),
                                     glm::vec3(config.ViewModelScale), "[Runtime] First Person Arms");
    m_Weapon = world.CreateModelEntity(std::move(weapon), glm::vec3(0.0f), glm::vec3(0.0f),
                                       glm::vec3(config.ViewModelScale), "[Runtime] First Person Weapon");
    for (entt::entity e : {m_Arms, m_Weapon}) {
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        // The weapon casts its shadow from where it really is (in the hands); the arms rig doesn't - with
        // a First Person Body its arms are the body's, which cast their own.
        renderable.CastShadows = e == m_Weapon ? RenderableComponent::ShadowCasting::On : RenderableComponent::ShadowCasting::Off;
        renderable.ReceiveShadows = e == m_Weapon;
        // Routes them into the renderer's view-model sub-pass (own FOV, depth cleared) rather
        // than the world pass. Runtime-only tag: these entities are created here and destroyed
        // by Stop(), so it never reaches a scene file.
        world.Registry.emplace_or_replace<ViewModelTag>(e);
    }
    // .fpsanim material overrides: every submesh whose imported material has the listed name.
    auto applyMaterials = [&](entt::entity e, const std::vector<std::pair<std::string, std::string>>& overrides) {
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        for (const auto& [name, matPath] : overrides) {
            auto mat = assets.LoadMaterial(ProjectPaths::Resolve(matPath));
            if (!mat || mat->Missing) {
                Log::Warn("First-person: material '" + matPath + "' for '" + name + "' could not be loaded.",
                          LogContext::Asset(animationSet));
                continue;
            }
            bool used = false;
            for (int i = 0; i < renderable.ModelRef->MeshCount(); ++i) {
                if (renderable.ModelRef->MeshMaterial(i).Name != name) continue;
                if (i >= (int)renderable.Materials.size()) renderable.Materials.resize(i + 1);
                renderable.Materials[i] = mat;
                used = true;
            }
            if (!used)
                Log::Warn("First-person: no submesh uses a material named '" + name + "' (from '" +
                          animationSet + "').", LogContext::Asset(animationSet));
        }
    };
    applyMaterials(m_Arms, m_Set.ArmsMaterials);
    applyMaterials(m_Weapon, m_Set.WeaponMaterials);
    // The arms run the controller; the weapon mirrors it on its own track. Both keep running
    // while hidden (unarmed), or the controller could never leave its Hidden state.
    auto& armsAnim = world.Registry.emplace_or_replace<AnimatorControllerComponent>(m_Arms);
    armsAnim.Controller = m_ControllerPath;
    armsAnim.Track = TrackOr(*ctrl, "arms", 0);
    armsAnim.UpdateWhenInactive = true;
    auto& weaponAnim = world.Registry.emplace_or_replace<AnimatorControllerComponent>(m_Weapon);
    weaponAnim.Controller = m_ControllerPath;
    weaponAnim.Track = TrackOr(*ctrl, "weapon", 1);
    weaponAnim.Driver = m_Arms;
    weaponAnim.UpdateWhenInactive = true;

    m_Offset = config.ViewModelOffset;
    m_Rotation = config.ViewModelRotation;
    m_Scale = config.ViewModelScale;
    // Not validated here: MakePerspective is the engine's one guarded projection constructor
    // (#202) and corrects every degenerate FOV, so an out-of-range value costs a wrong-looking
    // view model, never a broken frame. The Inspector already clamps it to 20..150.
    m_ViewModelFov = config.ViewModelFov;
    m_CameraBone = config.CameraBone;
    m_Ammo = m_SlotAmmo[slot] >= 0 ? std::min(m_SlotAmmo[slot], m_Set.Gameplay.Magazine) : m_Set.Gameplay.Magazine;
    ResetReloadKey();
    m_RegripDelay = FirstPersonRegripDelay(std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng),
                                           m_Set.Gameplay.RegripMin, m_Set.Gameplay.RegripMax);
    armsAnim.SetInt(K::kAmmo, m_Ammo);
    armsAnim.SetBool(K::kEquipped, true);
    // A weapon swapped in comes out of its holster (Draw) rather than appearing in the hands.
    if (holstered && !AnimatorStartInState(*ctrl, armsAnim, "Holstered"))
        Log::Warn("First-person presentation: '" + animationSet + "' has no 'Holstered' state; it appears without a draw.");

    if (!AttachAndValidate(assets, *ctrl)) {
        StopSet(world); // this weapon's rigs only: the slot list stays for a fallback
        return false;
    }
    m_Procedural.Reset();
    m_Procedural.Seed(m_Rng()); // a fresh recoil pattern every Play, not the same one each time
    m_UsesIK = SetupIK();
    SetupBolt(assets, *ctrl);
    m_Assets = &assets;
    m_Controller = ctrl;
    SetupAdsCarry();
    int states = 0;
    for (const auto& L : ctrl->Layers) states += (int)L.States.size();
    Log::Info("First-person presentation loaded '" + animationSet + "' (" +
              (m_Set.Controller.empty() ? std::string("v1 clip list") : m_Set.Controller) + ", " +
              std::to_string(states) + " states).");
    return true;
}

void FirstPersonPresentation::StopSet(World& world) {
    if (m_WorldWeapon != entt::null && world.Registry.valid(m_WorldWeapon)) world.DestroyEntityAndChildren(m_WorldWeapon);
    m_WorldWeapon = entt::null;
    if (m_Arms != entt::null && world.Registry.valid(m_Arms)) world.DestroyEntityAndChildren(m_Arms);
    if (m_Weapon != entt::null && world.Registry.valid(m_Weapon)) world.DestroyEntityAndChildren(m_Weapon);
    m_World = nullptr;
    m_PendingShots = 0;
    m_Arms = entt::null;
    m_Weapon = entt::null;
    if (m_WeaponModel) m_WeaponModel->SetHiddenNodes({}); // the model outlives Play
    m_SpareMagHidden.clear();
    m_ArmsModel.reset();
    m_WeaponModel.reset();
    m_Set = {};
    m_ControllerPath.clear();
    m_Controller.reset();
    m_Assets = nullptr;
    m_AdsCarry = {};
    m_AdsHolding = false;
    m_CameraBone.clear();
    m_ViewModelFov = -1.0f;
    m_CameraBoneWarned = false;
    m_WeaponSocketWarned = false;
    m_Equipped = true;
    m_HiddenApplied = false;
    m_Ammo = m_Set.Gameplay.Magazine;
    m_Chambered = true;
    m_CycleWait = 0.0f;
    m_CycleSeen = false;
    m_StopReload = false;
    m_FullAuto = false;
    m_FireCooldown = 0.0f;
    m_IdleTime = 0.0f;
    m_ReloadKey = {};
    m_Procedural.Reset();
    m_UsesIK = false;
    m_HaveLook = false;
    m_SetFile.clear();
    m_ReloadPoll = 0.0f;
    m_AdsHold = 0.0f;
    m_Zoom = m_ZoomRate = 0.0f;
}

bool FirstPersonPresentation::SetupIK() {
    const WeaponIKSettings& k = m_Set.Procedural.IK;
    if (!k.Enabled || !m_World || !m_ArmsModel) return false;
    for (const std::string* bone : {&k.GunBone, &k.RightUpper, &k.RightLower, &k.RightHand, &k.LeftUpper, &k.LeftLower, &k.LeftHand}) {
        if (m_ArmsModel->NodeIndex(*bone) < 0) {
            Log::Warn("First-person presentation: arms model has no '" + *bone +
                      "' bone; procedural motion moves the whole view model instead of the gun (no IK).");
            return false;
        }
    }
    // Both hands keep the grip they have in the clip, relative to the gun bone, wherever the
    // procedural stack moves it.
    auto& rig = m_World->Registry.emplace_or_replace<IKRigComponent>(m_Arms);
    const auto limb = [&](IKLimb& l, const std::string& upper, const std::string& lower, const std::string& hand) {
        l.Enabled = true;
        l.Upper = upper;
        l.Lower = lower;
        l.End = hand;
        l.Target = k.GunBone;
        l.KeepAnimatedOffset = true;
        l.MatchRotation = true;
        l.Weight = 1.0f;
    };
    limb(rig.LimbA, k.RightUpper, k.RightLower, k.RightHand);
    limb(rig.LimbB, k.LeftUpper, k.LeftLower, k.LeftHand);
    // Two offsets on the gun bone: the ADS-action correction (about the camera bone) first, then
    // the procedural pose on top of wherever that put the gun.
    rig.Offsets.assign(2, IKBoneOffset{});
    rig.Offsets[kAdsOffset].Bone = k.GunBone;
    rig.Offsets[kAdsOffset].PivotBone = m_CameraBone;
    rig.Offsets[kProceduralOffset].Bone = k.GunBone;

    // Self-check on the real rig: pull the gun 10% of an arm's length toward the right shoulder
    // and tip it 5 degrees, solve, and measure how far each hand lands from its grip. Anything
    // beyond a tiny miss means these bones don't form the chains they're named as.
    {
        std::vector<LocalTRS> pose;
        m_ArmsModel->BindLocalPose(pose);
        std::vector<int> parents(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m_ArmsModel->NodeParent(i);
        std::vector<glm::mat4> before, after;
        IK::ComputeGlobals(pose, parents, before);
        const int gun = m_ArmsModel->NodeIndex(k.GunBone);
        const int shoulder = m_ArmsModel->NodeIndex(k.RightUpper);
        const int hands[2] = {m_ArmsModel->NodeIndex(k.RightHand), m_ArmsModel->NodeIndex(k.LeftHand)};
        const float armLength = glm::length(IK::Position(before[hands[0]]) - IK::Position(before[shoulder]));
        IKRigComponent probe = rig;
        const glm::vec3 toShoulder = IK::Position(before[shoulder]) - IK::Position(before[gun]);
        probe.Offsets[kProceduralOffset].Position = glm::length(toShoulder) > 1e-6f ? glm::normalize(toShoulder) * armLength * 0.1f : glm::vec3(0.0f);
        probe.Offsets[kProceduralOffset].Rotation = glm::angleAxis(glm::radians(5.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        IK::ApplyRig(probe, *m_ArmsModel, pose);
        IK::ComputeGlobals(pose, parents, after);
        float worst = 0.0f;
        for (int h : hands) {
            const glm::mat4 want = after[gun] * glm::inverse(before[gun]) * before[h];
            worst = std::max(worst, glm::length(IK::Position(want) - IK::Position(after[h])) / std::max(armLength, 1e-6f));
        }
        char msg[192];
        std::snprintf(msg, sizeof msg, "First-person IK: hands stay on the gun to within %.3f%% of an arm's length (probe offset 10%%).",
                      worst * 100.0f);
        if (worst < 0.005f) Log::Info(msg);
        else Log::Warn(std::string(msg) + " Check the IK bone names in the weapon definition.");
    }
    return true;
}

// Camera-frame procedural pose -> the arms rig's model space. The arms entity is rotated
// camera * C (C = the asset's view rotation, then the scene's tweak) and scaled by m_Scale, so a
// camera-frame vector v is C^-1 v / scale in model space, and a rotation q is C^-1 q C.
// The procedural bolt rides along the travel the weapon's own Fire clip authors: sample that clip,
// and the bolt's farthest point from where it starts is the stroke (weapon model space).
void FirstPersonPresentation::SetupBolt(AssetLibrary& assets, const AnimatorController& ctrl) {
    m_BoltStroke = glm::vec3(0.0f);
    m_HaveMuzzle = false;
    const WeaponRecoilSettings& r = m_Set.Procedural.Recoil;
    if (!m_World || !m_WeaponModel) return;
    const int bolt = m_WeaponModel->NodeIndex(r.BoltBone);
    const int weaponTrack = ctrl.TrackIndex(TrackOr(ctrl, "weapon", 1));
    // A fresh Play measures its own sight line (a saved one wins anyway).
    m_SightMeasured = m_SightLogged = false;
    m_SightSettled = 0.0f;
    m_Barrel = {};
    // The hip-fire clip: the state with the Shot event (else one named Fire) that moves the gun.
    int clip = -1;
    for (int pass = 0; pass < 2 && clip < 0; ++pass)
        for (const auto& L : ctrl.Layers)
            for (const auto& st : L.States) {
                const bool shot = std::any_of(st.Events.begin(), st.Events.end(),
                                              [](const AnimatorController::Event& ev) { return ev.Name == K::kEventShot; });
                if (clip < 0 && (pass == 0 ? shot : st.Name == "Fire") && !st.MotionFor(weaponTrack).Clip.empty())
                    clip = ResolveAnimationClip(*m_WeaponModel, st.MotionFor(weaponTrack).Clip, assets);
            }
    if (r.BoltBone.empty()) { // no bolt (a pump, a revolver): nothing to measure, the muzzle is set by hand
        SetupMuzzle(-1);
        return;
    }
    if (bolt < 0 || clip < 0) {
        Log::Warn("First-person presentation: no '" + r.BoltBone + "' bone or weapon hip-fire clip to measure the bolt from; no procedural bolt.");
        SetupMuzzle(-1);
        return;
    }
    std::vector<int> parents(m_WeaponModel->NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
    std::vector<LocalTRS> pose;
    std::vector<glm::mat4> globals;
    glm::vec3 start(0.0f);
    const float length = m_WeaponModel->AnimationLength(clip);
    for (int k = 0; k <= 60; ++k) {
        m_WeaponModel->SampleLocalPose(clip, length * (float)k / 60.0f, AnimationWrapMode::ClampForever, pose);
        IK::ComputeGlobals(pose, parents, globals);
        const glm::vec3 at = IK::Position(globals[bolt]);
        if (k == 0) start = at;
        else if (glm::length(at - start) > glm::length(m_BoltStroke)) m_BoltStroke = at - start;
    }
    SetupMuzzle(bolt);
    if (r.BoltCycle <= 0.0f) return;
    auto& rig = m_World->Registry.emplace_or_replace<IKRigComponent>(m_Weapon);
    rig.Offsets.assign(1, IKBoneOffset{});
    rig.Offsets[0].Bone = r.BoltBone;
}

namespace {
// A vertical FOV narrowed by `zoom` (magnification), blended by `t` in view-width space.
float ZoomedFov(float fov, float zoom, float t) {
    if (t <= 0.0f || zoom <= 1.0f) return fov;
    const float full = std::tan(glm::radians(fov) * 0.5f);
    const float half = glm::mix(full, full / zoom, t);
    return glm::degrees(2.0f * std::atan(half));
}
} // namespace

float FirstPersonPresentation::ViewModelFov() const {
    return IsActive() ? ZoomedFov(m_ViewModelFov, m_Set.Ads.ViewModelZoom, m_Zoom) : -1.0f;
}

float FirstPersonPresentation::WorldFov(float baseFov) const {
    return IsActive() ? ZoomedFov(baseFov, m_Set.Ads.Zoom, m_Zoom) + m_Procedural.Pose().FovKick : baseFov;
}

float FirstPersonPresentation::LookScale(float baseFov) const {
    // The zoom only: the per-round FOV pulse mustn't wobble the mouse.
    const float fov = IsActive() ? ZoomedFov(baseFov, m_Set.Ads.Zoom, m_Zoom) : baseFov;
    return std::tan(glm::radians(fov) * 0.5f) / std::tan(glm::radians(baseFov) * 0.5f);
}

void FirstPersonPresentation::SetupAdsCarry() {
    m_AdsCarry = {};
    if (!m_ArmsModel || !m_Controller || !m_Assets) return;
    AdsCarryInputs in;
    in.Arms = m_ArmsModel.get();
    in.Assets = m_Assets;
    in.Controller = m_Controller.get();
    in.Settings = &m_Set.Ads;
    in.ArmsTrack = TrackOr(*m_Controller, "arms", 0);
    in.Weapon = m_WeaponModel.get();
    in.WeaponTrack = TrackOr(*m_Controller, "weapon", 1);
    in.GunBone = m_Set.WeaponSocket.empty() ? m_Set.Procedural.IK.GunBone : m_Set.WeaponSocket;
    in.CameraBone = m_CameraBone;
    in.Rig = m_UsesIK && m_World ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    in.AdsOffset = kAdsOffset;
    in.ProceduralOffset = kProceduralOffset;
    m_AdsCarry = BuildAdsCarry(in);
    PublishAdsCarryReport(m_SetFile.u8string(), m_AdsCarry.Report);
    for (const std::string& w : m_AdsCarry.Report.Warnings) Log::Warn("First-person ADS: " + w);
    for (const auto& e : m_AdsCarry.Report.Entries)
        if (!e.Problem.empty()) Log::Warn("First-person ADS: '" + e.State + "' can't be carried onto the sights: it " + e.Problem + ".");
    SetupHandAnchor();
}

void FirstPersonPresentation::SetupHandAnchor() {
    m_AnchorLimb = -1;
    m_AnchorBones.clear();
    const FirstPersonAdsSettings::HandAnchor& anchor = m_Set.Ads.Anchor;
    auto* rig = m_UsesIK && m_World ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    if (!anchor.Enabled || !rig || !m_ArmsModel || !m_WeaponModel) return;
    // The free hand: the limb whose hand the aim pose doesn't hold while an action plays.
    const auto& hold = m_AdsCarry.HoldMask;
    const IKLimb* limbs[2] = {&rig->LimbA, &rig->LimbB};
    for (int i = 0; i < 2 && m_AnchorLimb < 0; ++i) {
        const int end = m_ArmsModel->NodeIndex(limbs[i]->End);
        if (limbs[i]->Enabled && end >= 0 && end < (int)hold.size() && hold[end] <= 0.0f) m_AnchorLimb = i;
    }
    if (m_AnchorLimb < 0) {
        Log::Warn("First-person ADS: the hand anchor needs a hand the aim pose doesn't hold (ads.actionBones); it's off.");
        return;
    }
    // The gun's box: its mesh's bind bounds, in its root's space.
    std::vector<LocalTRS> bind;
    std::vector<glm::mat4> globals;
    std::vector<int> parents(m_WeaponModel->NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
    m_WeaponModel->BindLocalPose(bind);
    IK::ComputeGlobals(bind, parents, globals);
    const int root = m_WeaponModel->NodeIndex(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot);
    const glm::mat4 toRoot = root >= 0 ? glm::inverse(globals[root]) : glm::mat4(1.0f);
    const glm::vec3 lo = m_WeaponModel->BoundsMin(), hi = m_WeaponModel->BoundsMax();
    m_GunBoxMin = glm::vec3(1e30f);
    m_GunBoxMax = glm::vec3(-1e30f);
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
        const glm::vec3 q = glm::vec3(toRoot * glm::vec4(corner, 1.0f));
        m_GunBoxMin = glm::min(m_GunBoxMin, q);
        m_GunBoxMax = glm::max(m_GunBoxMax, q);
    }
    for (const std::string& bone : anchor.Bones) {
        const int i = m_WeaponModel->NodeIndex(bone);
        if (i < 0) Log::Warn("First-person ADS: the weapon has no '" + bone + "' bone for the hand anchor to carry.");
        else m_AnchorBones.push_back(i);
    }
    if (!m_AnchorBones.empty() && m_World->Registry.valid(m_Weapon)) {
        auto& wrig = m_World->Registry.get_or_emplace<IKRigComponent>(m_Weapon);
        wrig.Offsets.resize(kWeaponAnchorOffset + m_AnchorBones.size()); // slot 0 stays the bolt's
        for (size_t k = 0; k < m_AnchorBones.size(); ++k)
            wrig.Offsets[kWeaponAnchorOffset + k] = IKBoneOffset{m_WeaponModel->NodeName(m_AnchorBones[k])};
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "First-person ADS: hand anchor on (limb %c), gun box (%.3f %.3f %.3f)..(%.3f %.3f %.3f) m.",
                  m_AnchorLimb == 0 ? 'A' : 'B', m_GunBoxMin.x, m_GunBoxMin.y, m_GunBoxMin.z, m_GunBoxMax.x,
                  m_GunBoxMax.y, m_GunBoxMax.z);
    Log::Info(msg);
}

// A free hand off the gun goes where it is relative to the eye at the hip, not relative to the
// gun (which the sights have moved), and the weapon bones in it (the shell) go with it.
void FirstPersonPresentation::WriteHandAnchor(IKRigComponent& rig, const AdsCarrySample& carry, const glm::quat& adsR,
                                              const glm::vec3& adsT) {
    m_AnchorWeight = 0.0f;
    rig.LimbA.GoalMove = rig.LimbB.GoalMove = glm::mat4(1.0f);
    IKRigComponent* wrig = !m_AnchorBones.empty() && m_World && m_World->Registry.valid(m_Weapon)
                               ? m_World->Registry.try_get<IKRigComponent>(m_Weapon) : nullptr;
    if (wrig && wrig->Offsets.size() < kWeaponAnchorOffset + m_AnchorBones.size()) wrig = nullptr;
    if (wrig)
        for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
            wrig->Offsets[kWeaponAnchorOffset + k].Position = glm::vec3(0.0f);
            wrig->Offsets[kWeaponAnchorOffset + k].Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
    const AdsCarryAction* a = carry.Action;
    if (m_AnchorLimb < 0 || !a || a->Clip < 0 || carry.Weight <= 0.0f || !m_ArmsModel ||
        (int)rig.HoldPose.size() != m_ArmsModel->NodeCount())
        return;
    IKLimb& limb = m_AnchorLimb == 0 ? rig.LimbA : rig.LimbB;
    const int hand = m_ArmsModel->NodeIndex(limb.End), socket = m_ArmsModel->NodeIndex(limb.Target);
    const int head = m_CameraBone.empty() ? -1 : m_ArmsModel->NodeIndex(m_CameraBone);
    if (hand < 0 || socket < 0 || head < 0) return;
    thread_local std::vector<LocalTRS> pose;
    thread_local std::vector<glm::mat4> clip, held;
    thread_local std::vector<int> parents;
    parents.resize(m_ArmsModel->NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_ArmsModel->NodeParent(i);
    const float t = std::max(carry.Phase, 0.0f) * a->Length;
    m_ArmsModel->SampleLocalPose(a->Clip, t, AnimationWrapMode::ClampForever, pose);
    IK::ComputeGlobals(pose, parents, clip);
    IK::ComputeGlobals(rig.HoldPose, parents, held);
    // The gun as the solve will have it: the held aim pose's, moved by the ADS offset about the eye.
    const glm::vec3 eye = IK::Position(held[head]);
    const glm::mat4 gun = glm::translate(glm::mat4(1.0f), eye + adsT) * glm::mat4_cast(adsR) *
                          glm::translate(glm::mat4(1.0f), -eye) * held[socket];
    // How far the clip's hand is from the gun.
    const glm::mat4 mount = glm::translate(glm::mat4(1.0f), m_Set.WeaponMountOffset) *
                            glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation));
    const glm::vec3 inGun = glm::vec3(glm::inverse(clip[socket] * mount) * glm::vec4(IK::Position(clip[hand]), 1.0f));
    const FirstPersonAdsSettings::HandAnchor& anchor = m_Set.Ads.Anchor;
    const auto weightAt = [&](const glm::vec3& inRoot) {
        const float x = std::clamp((AdsBoxDistance(inRoot, m_GunBoxMin, m_GunBoxMax) - anchor.Near) / (anchor.Far - anchor.Near), 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x) * carry.Weight;
    };
    const float w = weightAt(inGun);
    m_AnchorWeight = w;
    if (w > 0.0f) limb.GoalMove = AdsHandAnchorMove(clip[head], clip[socket], clip[hand], eye, gun, w);
    // The weapon bones it carries, each by its own distance from the gun (a shell in the hand or
    // waiting on the belt moves; one in the gun stays): the same move, in the weapon model's space.
    if (!wrig || a->WeaponClip < 0 || !m_WeaponModel) return;
    const int root = m_WeaponModel->NodeIndex(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot);
    if (root < 0) return;
    thread_local std::vector<LocalTRS> wpose;
    thread_local std::vector<glm::mat4> wglobals;
    thread_local std::vector<int> wparents;
    wparents.resize(m_WeaponModel->NodeCount());
    for (int i = 0; i < (int)wparents.size(); ++i) wparents[i] = m_WeaponModel->NodeParent(i);
    m_WeaponModel->SampleLocalPose(a->WeaponClip, t, AnimationWrapMode::ClampForever, wpose);
    IK::ComputeGlobals(wpose, wparents, wglobals);
    const glm::mat4 fromRoot = glm::inverse(wglobals[root]);
    const glm::mat4 toSocket = mount * fromRoot; // weapon model -> the socket's frame
    const glm::mat4 toArms = gun * toSocket;
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
        const glm::mat4& bone = wglobals[m_AnchorBones[k]];
        const float wb = weightAt(IK::Position(fromRoot * bone));
        if (wb <= 0.0f) continue;
        const glm::mat4 move = AdsHandAnchorMove(clip[head], clip[socket], clip[socket] * toSocket * bone, eye, gun, wb);
        const glm::mat4 local = glm::inverse(toArms) * move * toArms;
        const glm::vec3 p = IK::Position(bone);
        wrig->Offsets[kWeaponAnchorOffset + k].Rotation = QuaternionFromMatrix(local);
        wrig->Offsets[kWeaponAnchorOffset + k].Position = glm::vec3(local * glm::vec4(p, 1.0f)) - p;
    }
}

AdsCarrySample FirstPersonPresentation::SampleAdsCarry(float dt) const {
    const auto* ac = Animator();
    if (!ac || ac->Layers.empty()) return {};
    return EvaluateAdsCarry(ac->Layers[0], m_AdsCarry.Actions, dt, m_AdsHold);
}

// Auto: the bolt rides the bore, so its travel is the barrel's axis. The muzzle is the front face
// of the weapon mesh along that axis: the verts nearest the tip and within a few cm of the line,
// averaged (the booster's ring centres on the bore even if the bolt bone sits a little off it).
// Both are kept in the weapon root's space, which is what the socket moves. A hand-set muzzle
// (muzzle.auto off) wins; what Auto found is still reported, for the Inspector to copy.
void FirstPersonPresentation::SetupMuzzle(int bolt) {
    m_HaveMuzzle = false;
    m_Barrel.Detected = m_Barrel.HasMuzzle = false;
    m_Barrel.Problem.clear();
    if (m_WeaponModel && bolt >= 0 && glm::length(m_BoltStroke) >= 1e-5f) {
        std::vector<int> parents(m_WeaponModel->NodeCount());
        for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
        const int root = m_WeaponModel->NodeIndex(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot);
        std::vector<LocalTRS> bind;
        std::vector<glm::mat4> globals;
        m_WeaponModel->BindLocalPose(bind);
        IK::ComputeGlobals(bind, parents, globals);
        const glm::vec3 axis = -glm::normalize(m_BoltStroke);
        const glm::vec3 origin = IK::Position(globals[bolt]);
        std::vector<glm::vec3> verts;
        std::vector<unsigned int> indices;
        m_WeaponModel->CollisionGeometry(verts, indices);
        const float stroke = glm::length(m_BoltStroke); // ~9 cm on an AK: a scale-free yardstick
        const float radius = stroke * 0.45f;
        float tip = -1e30f;
        const auto offLine = [&](const glm::vec3& v, float t) { return glm::length(v - origin - axis * t); };
        for (const glm::vec3& v : verts) {
            const float t = glm::dot(v - origin, axis);
            if (offLine(v, t) < radius) tip = std::max(tip, t);
        }
        if (tip >= 0.0f) {
            glm::vec3 sum(0.0f);
            int n = 0;
            for (const glm::vec3& v : verts) {
                const float t = glm::dot(v - origin, axis);
                if (t > tip - stroke * 0.06f && offLine(v, t) < radius) { sum += v; ++n; }
            }
            const glm::mat4 rootInv = root >= 0 ? glm::inverse(globals[root]) : glm::mat4(1.0f);
            m_Barrel.Detected = true;
            m_Barrel.DetectedOrigin = glm::vec3(rootInv * glm::vec4(sum / (float)n, 1.0f));
            m_Barrel.DetectedDirection = glm::normalize(glm::mat3(rootInv) * axis);
            char msg[160];
            std::snprintf(msg, sizeof msg, "First-person: bolt stroke %.1f, muzzle %.1f ahead of the bolt (model units x100).",
                          stroke * 100.0f, tip * 100.0f);
            Log::Info(msg);
        }
    }
    const FirstPersonMuzzleSettings& mz = m_Set.Muzzle;
    if (!mz.Auto && glm::length(mz.Direction) > 1e-6f) {
        m_MuzzleLocal = mz.Origin;
        m_BoreLocal = glm::normalize(mz.Direction);
        m_HaveMuzzle = true;
    } else if (mz.Auto && m_Barrel.Detected) {
        m_MuzzleLocal = m_Barrel.DetectedOrigin;
        m_BoreLocal = m_Barrel.DetectedDirection;
        m_HaveMuzzle = true;
    } else {
        m_Barrel.Problem = bolt < 0 || glm::length(m_BoltStroke) < 1e-5f
                               ? "Auto needs the procedural bolt (recoil.boltBone, moved by the hip-fire clip) to find the barrel."
                               : "Auto couldn't find the weapon mesh's front face along the bolt's travel.";
        Log::Warn("First-person: no muzzle - " + m_Barrel.Problem +
                  " Rounds hit nothing and there's no laser; set the muzzle by hand in the weapon Inspector (Barrel & Laser).");
    }
    m_Barrel.HasMuzzle = m_HaveMuzzle;
    PublishBarrelReport(m_SetFile.u8string(), m_Barrel);
}

bool FirstPersonPresentation::ArmsNodeInView(const std::string& node, glm::vec3& out) const {
    glm::mat4 m(1.0f);
    if (!m_ArmsModel || !m_ArmsModel->NodeTransform(node, m)) return false;
    out = glm::vec3(m_View * m_ArmsWorld * m[3]);
    return true;
}

bool FirstPersonPresentation::WeaponNodeInView(const std::string& node, glm::vec3& out) const {
    glm::mat4 m(1.0f);
    if (!m_WeaponModel || !m_WeaponModel->NodeTransform(node, m)) return false;
    out = glm::vec3(m_View * m_WeaponWorld * m[3]);
    return true;
}

bool FirstPersonPresentation::StockWorld(glm::vec3& butt, glm::vec3& forward) const {
    if (!m_WeaponModel || m_Set.WeaponRoot.empty()) return false;
    const Model& m = *m_WeaponModel;
    glm::mat4 root(1.0f);
    if (!m.NodeTransform(m_Set.WeaponRoot, root)) return false;
    auto skinned = [&](int mesh, int v) {
        const ModelMesh::SkinVertex& sv = m.MeshSkinVertices(mesh)[v];
        glm::vec4 p(0.0f);
        float total = 0.0f;
        for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
            if (sv.BoneIDs[k] >= 0 && sv.Weights[k] > 0.0f) {
                p += sv.Weights[k] * (m.FinalBoneMatrix(sv.BoneIDs[k]) * glm::vec4(sv.Position, 1.0f));
                total += sv.Weights[k];
            }
        return total > 0.0f ? glm::vec3(p) / total : sv.Position;
    };
    // The butt is what lies furthest back along the bore (within 1.5 cm of the end). The bore is the muzzle's
    // (root space), else the root's -Z: the Remington's Main bone points down its barrel, the AK's root doesn't.
    const glm::vec3 boreLocal = m_HaveMuzzle ? m_BoreLocal : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 back = -glm::normalize(glm::mat3(root) * boreLocal);
    if (m_StockModel != &m || m_StockBore != boreLocal) {
        m_StockModel = &m;
        m_StockBore = boreLocal;
        m_StockVerts.clear();
        // Not the spare magazine (the AK's rides behind the stock, on the belt): no vertex skinned mostly to
        // one of its bones, or a bone under them.
        std::vector<char> spare(m.BoneCount() > 0 ? m.BoneCount() : 0, 0);
        for (int n = 0; n < m.NodeCount(); ++n)
            for (int a = n; a >= 0; a = m.NodeParent(a))
                if (std::find(m_Set.SpareMagazineBones.begin(), m_Set.SpareMagazineBones.end(), m.NodeName(a)) != m_Set.SpareMagazineBones.end()) {
                    if (const int id = m.BoneId(m.NodeName(n)); id >= 0 && id < (int)spare.size()) spare[id] = 1;
                    break;
                }
        auto onGun = [&](int mesh, int v) {
            const ModelMesh::SkinVertex& sv = m.MeshSkinVertices(mesh)[v];
            int best = -1;
            for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
                if (sv.BoneIDs[k] >= 0 && sv.Weights[k] > 0.0f && (best < 0 || sv.Weights[k] > sv.Weights[best])) best = k;
            return best < 0 || sv.BoneIDs[best] >= (int)spare.size() || !spare[sv.BoneIDs[best]];
        };
        float far = -1e9f;
        for (int i = 0; i < m.MeshCount(); ++i)
            for (int v = 0; v < (int)m.MeshSkinVertices(i).size(); ++v)
                if (onGun(i, v)) far = std::max(far, glm::dot(skinned(i, v), back));
        for (int i = 0; i < m.MeshCount(); ++i)
            for (int v = 0; v < (int)m.MeshSkinVertices(i).size(); ++v)
                if (onGun(i, v) && glm::dot(skinned(i, v), back) > far - 0.015f / std::max(m_Scale, 1e-6f)) m_StockVerts.push_back({i, v});
    }
    if (m_StockVerts.empty()) return false;
    glm::vec3 sum(0.0f);
    for (const auto& [i, v] : m_StockVerts) sum += skinned(i, v);
    butt = glm::vec3(m_WeaponWorld * glm::vec4(sum / (float)m_StockVerts.size(), 1.0f));
    forward = -glm::normalize(glm::mat3(m_WeaponWorld) * back);
    return true;
}

bool FirstPersonPresentation::WorldGunInput(FirstPersonWorldGunInput& out) const {
    if (!m_Equipped) return false;
    glm::vec3 butt, forward;
    if (!StockWorld(butt, forward)) return false;
    const FirstPersonStockLockSettings& sl = m_Set.StockLock;
    const float x = sl.Enabled ? m_StockLockWeight : 0.0f;
    out.ButtWorld = butt;
    out.ForwardWorld = forward;
    // Looking steeply down the pocket lets go (all of it by Release End). Whenever the butt isn't in the pocket - let go,
    // in a reload / inspect / melee, or a weapon with no pocket lock - the gun keeps off the torso instead.
    float held = 1.0f;
    if (sl.ReleaseStart > sl.ReleaseEnd) {
        const float r = std::clamp((m_LookPitch - sl.ReleaseEnd) / (sl.ReleaseStart - sl.ReleaseEnd), 0.0f, 1.0f);
        held = r * r * (3.0f - 2.0f * r);
    }
    out.Shouldered = x * x * (3.0f - 2.0f * x) * held; // eased in and out
    out.TorsoKeepOut = 1.0f - out.Shouldered;
    // The cheek weld is for the sights: at the hip the head stays as the clips have it, so a reload or an inspect
    // starting and ending (states the pocket lock lets go in) no longer nods it 25 degrees each way.
    const float z = std::clamp(m_Zoom, 0.0f, 1.0f);
    out.CheekWeld = (sl.Enabled ? z * z * (3.0f - 2.0f * z) : 0.0f) * held;
    out.Pocket = sl.Pocket;
    out.MaxShift = sl.MaxShift;
    out.HeadTiltDegrees = sl.HeadTilt;
    out.NeckRadius = sl.NeckRadius;
    out.HeadRadius = sl.HeadRadius;
    out.GunLength = sl.GunLength;
    out.MeshClearance = sl.MeshClearance;
    return true;
}

void FirstPersonPresentation::PlaceWorldWeapon(World& world, bool split, const glm::vec3& shift) {
    auto& reg = world.Registry;
    const bool haveWeapon = m_Weapon != entt::null && reg.valid(m_Weapon);
    if (!split || !haveWeapon) {
        if (m_WorldWeapon != entt::null && reg.valid(m_WorldWeapon)) world.DestroyEntityAndChildren(m_WorldWeapon);
        m_WorldWeapon = entt::null;
        if (haveWeapon) reg.remove<OwnerViewOnlyTag>(m_Weapon);
        return;
    }
    if (m_WorldWeapon == entt::null || !reg.valid(m_WorldWeapon)) {
        // The same model object as the first-person gun: one pose (the pump, the shells), drawn twice.
        const RenderableComponent src = reg.get<RenderableComponent>(m_Weapon); // by value: the create below can move the storage
        m_WorldWeapon = world.CreateModelEntity(src.ModelRef, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(m_Scale),
                                                "[Runtime] First Person Weapon (world)");
        auto& dst = reg.get<RenderableComponent>(m_WorldWeapon);
        dst.Materials = src.Materials;
        dst.CastShadows = RenderableComponent::ShadowCasting::On;
        dst.ReceiveShadows = true;
        reg.emplace_or_replace<HiddenFromOwnerTag>(m_WorldWeapon);
        reg.emplace_or_replace<OwnerViewOnlyTag>(m_Weapon);
    }
    const glm::vec3 scale(glm::length(glm::vec3(m_WeaponWorld[0])), glm::length(glm::vec3(m_WeaponWorld[1])),
                          glm::length(glm::vec3(m_WeaponWorld[2])));
    const glm::mat3 r(glm::vec3(m_WeaponWorld[0]) / std::max(scale.x, 1e-6f), glm::vec3(m_WeaponWorld[1]) / std::max(scale.y, 1e-6f),
                      glm::vec3(m_WeaponWorld[2]) / std::max(scale.z, 1e-6f));
    world.SetWorldPose(m_WorldWeapon, glm::vec3(m_WeaponWorld[3]) + shift, glm::normalize(glm::quat_cast(r)));
    reg.get<TransformComponent>(m_WorldWeapon).Scale = scale;
    // Hidden (holstered, unarmed) with the first-person gun.
    const bool hidden = reg.any_of<InactiveTag, DeactivatedTag>(m_Weapon);
    if (hidden != reg.all_of<DeactivatedTag>(m_WorldWeapon)) {
        if (hidden) {
            reg.emplace_or_replace<DeactivatedTag>(m_WorldWeapon);
            reg.emplace_or_replace<InactiveTag>(m_WorldWeapon);
        } else {
            reg.remove<DeactivatedTag, InactiveTag>(m_WorldWeapon);
        }
    }
}

bool FirstPersonPresentation::BarrelAimPoint(glm::vec3& out) const {
    const auto* ac = Animator();
    if (!m_AimPointValid || !ac || !m_Equipped) return false;
    if (!(ac->HasTag(K::kTagIdle) || ac->HasTag(K::kTagReady))) return false;
    out = m_AimPoint;
    return true;
}

bool FirstPersonPresentation::LaserBeam(Laser& out) const {
    const auto* ac = Animator();
    if (!m_Set.Laser.Enabled || !m_AimPointValid || !ac || !m_Equipped || ac->HasTag(K::kTagHidden)) return false;
    // A draw starts with the gun low: its beam would curl across the bottom of the view.
    if (m_SinceUnhidden < 0.35f) return false;
    out.BeamColor = m_Set.Laser.Color * m_Set.Laser.BeamBrightness;
    out.SpotColor = m_Set.Laser.Color * m_Set.Laser.SpotBrightness;
    out.From = m_Muzzle;
    out.To = m_AimPoint;
    out.Normal = m_AimNormal;
    out.Hit = m_AimHit;
    return true;
}

std::vector<FirstPersonPresentation::ShotHit> FirstPersonPresentation::TakeShotHits() {
    std::vector<ShotHit> hits;
    hits.swap(m_ShotHits);
    return hits;
}

void FirstPersonPresentation::WriteIK() {
    if (m_World && m_World->Registry.valid(m_Weapon))
        if (auto* bolt = m_World->Registry.try_get<IKRigComponent>(m_Weapon); bolt && !bolt->Offsets.empty())
            bolt->Offsets[0].Position = m_BoltStroke * m_Procedural.Pose().Bolt;
    if (!m_UsesIK || !m_World || !m_World->Registry.valid(m_Arms)) return;
    auto* rig = m_World->Registry.try_get<IKRigComponent>(m_Arms);
    if (!rig || (int)rig->Offsets.size() <= kProceduralOffset) return;
    const WeaponProceduralPose& p = m_Procedural.Pose();
    const glm::quat C = NormalizeRotation(QuaternionFromEulerYXZ(m_Set.ViewRotation) * QuaternionFromEulerYXZ(m_Rotation));
    const glm::quat Ci = glm::inverse(C);
    rig->Weight = p.IKWeight;
    // With the sights up through a reload or mag check, only the GUN is carried onto Aim's sight
    // line (about the camera bone, in model space); the arms follow it by IK, so the shoulders
    // stay where the body is and nothing has to settle back when the action ends.
    glm::quat adsR(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 adsT(0.0f);
    const AdsCarrySample carry = SampleAdsCarry(m_TickDt);
    adsR = carry.R;
    adsT = carry.T;
    WriteAdsHold(*rig, carry);
    // Actions that keep some of their own gun motion (the mag check tipping the mag into view),
    // on top of the hold, turned about the rear sight so the sights stay near the centre.
    if (carry.Action && m_ArmsModel && !rig->HoldPose.empty())
        if (const auto* motion = m_Set.Ads.GunMotionFor(carry.Action->StateName)) {
            const std::string& gun = m_Set.WeaponSocket.empty() ? m_Set.Procedural.IK.GunBone : m_Set.WeaponSocket;
            const glm::vec3 sight = Ci * glm::vec3(0.0f, 0.0f, -m_Set.Ads.SightPivot) / m_Scale;
            AdsGunMotion(*m_ArmsModel, carry, m_ArmsModel->NodeIndex(gun),
                         m_CameraBone.empty() ? -1 : m_ArmsModel->NodeIndex(m_CameraBone), sight, motion->Rotation,
                         motion->Position, adsR, adsT);
        }
    WriteHandAnchor(*rig, carry, adsR, adsT);
    rig->LimbA.Swivel = carry.Swivel[0];
    rig->LimbB.Swivel = carry.Swivel[1];
    rig->LocalRotations.clear();
    if (carry.Action)
        for (const auto& [bone, d] : carry.Action->Locals)
            rig->LocalRotations.push_back({bone, glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), d, carry.Weight)});
    rig->Offsets[kAdsOffset].Rotation = adsR;
    rig->Offsets[kAdsOffset].Position = adsT;
    rig->Offsets[kProceduralOffset].Position = Ci * p.Position / m_Scale;
    rig->Offsets[kProceduralOffset].Rotation = NormalizeRotation(Ci * p.RotationQuat() * C);
    rig->Offsets[kProceduralOffset].Pivot = Ci * p.Pivot / m_Scale;
}

// With action bones, a carried action plays only on them: the aim clip holds the rest of the
// rig (the gun and the other hand) in the IK's hold pose, still playing from where the aim state
// was when the action began, so the sights keep their idle life instead of freezing.
void FirstPersonPresentation::WriteAdsHold(IKRigComponent& rig, const AdsCarrySample& carry) {
    const AdsCarryResult& r = m_AdsCarry;
    const bool valid = !r.HoldMask.empty() && r.AimClip >= 0 && m_ArmsModel && (int)r.HoldMask.size() == m_ArmsModel->NodeCount();
    // Fully held while the action is anywhere in the blend (a partial hold would let the clip's
    // gun motion through during its crossfades), by how far aim is held; once it has faded out,
    // eased off over kRelease so the held aim clip hands back to the aim state without a step.
    constexpr float kRelease = 0.3f;
    float weight = 0.0f;
    if (valid && carry.Action) {
        weight = std::clamp(m_AdsHold, 0.0f, 1.0f);
        m_AdsHoldLast = weight;
        m_AdsReleaseT = 0.0f;
    } else if (valid && m_AdsHolding) {
        m_AdsReleaseT += m_TickDt;
        const float x = std::clamp(m_AdsReleaseT / kRelease, 0.0f, 1.0f);
        weight = m_AdsHoldLast * (1.0f - x * x * (3.0f - 2.0f * x));
    }
    if (weight <= 0.0f) {
        rig.HoldPose.clear();
        rig.HoldWeights.clear();
        m_AdsHolding = false;
        return;
    }
    if (!m_AdsHolding) {
        // Pick the aim clip up where it was playing (if it's still in the blend), not at 0.
        m_AdsAimTime = 0.0f;
        if (const auto* ac = Animator(); ac && !ac->Layers.empty())
            for (const auto& item : ac->Layers[0].Stack)
                if (item.State == r.Reference) m_AdsAimTime = item.Phase * r.AimLength;
        m_AdsHolding = true;
    } else {
        m_AdsAimTime += m_TickDt;
    }
    m_ArmsModel->SampleLocalPose(r.AimClip, m_AdsAimTime, r.AimLoop ? AnimationWrapMode::Loop : AnimationWrapMode::ClampForever,
                                 rig.HoldPose);
    rig.HoldWeights.resize(r.HoldMask.size());
    for (size_t i = 0; i < r.HoldMask.size(); ++i) rig.HoldWeights[i] = r.HoldMask[i] * weight;
}

void FirstPersonPresentation::ReloadIfChanged(float dt) {
    m_ReloadPoll += dt;
    if (m_SetFile.empty() || m_ReloadPoll < 0.25f) return;
    m_ReloadPoll = 0.0f;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(m_SetFile, ec);
    if (ec || stamp == m_SetFileTime) return;
    m_SetFileTime = stamp;
    FirstPersonAnimationSet fresh;
    std::string why;
    if (!FirstPersonAnimationSet::LoadFile(m_SetFile.u8string(), fresh, &why)) {
        Log::Warn("First-person presentation: kept the running weapon tuning; the edited file doesn't load: " + why);
        return;
    }
    // Numbers (and the IK bones) only: the rigs and controller need a restart of Play to change.
    const bool rebuildIK = !SameIKSetup(m_Set.Procedural.IK, fresh.Procedural.IK);
    const FirstPersonAdsSettings& oldAds = m_Set.Ads;
    const FirstPersonMuzzleSettings& oldMuzzle = m_Set.Muzzle;
    const bool remuzzle = oldMuzzle.Auto != fresh.Muzzle.Auto || oldMuzzle.Origin != fresh.Muzzle.Origin ||
                          oldMuzzle.Direction != fresh.Muzzle.Direction;
    // The saved sight line cleared (Re-measure in the Inspector): measure it afresh.
    const bool resight = m_Set.Gameplay.HasSightLine && !fresh.Gameplay.HasSightLine;
    const bool remeasure = rebuildIK || oldAds.ReferenceState != fresh.Ads.ReferenceState ||
                           oldAds.CarryTag != fresh.Ads.CarryTag || oldAds.MatchElbows != fresh.Ads.MatchElbows ||
                           oldAds.MatchTwist != fresh.Ads.MatchTwist || oldAds.ActionBones != fresh.Ads.ActionBones;
    m_Set.Gameplay = fresh.Gameplay;
    m_Set.Ads = fresh.Ads;
    m_Set.Procedural = fresh.Procedural;
    m_Set.Muzzle = fresh.Muzzle;
    m_Set.Laser = fresh.Laser;
    if (remuzzle && m_WeaponModel)
        SetupMuzzle(glm::length(m_BoltStroke) >= 1e-5f ? m_WeaponModel->NodeIndex(m_Set.Procedural.Recoil.BoltBone) : -1);
    if (resight) {
        m_SightMeasured = m_SightLogged = false;
        m_SightSettled = 0.0f;
        m_Barrel.SightMeasured = false;
        PublishBarrelReport(m_SetFile.u8string(), m_Barrel);
    }
    if (rebuildIK && m_World && m_World->Registry.valid(m_Arms)) {
        m_World->Registry.remove<IKRigComponent>(m_Arms);
        m_UsesIK = SetupIK();
    }
    if (remeasure) SetupAdsCarry();
    m_ReloadKey.HoldSeconds = m_Set.Gameplay.ReloadHoldSeconds;
    m_Ammo = std::min(m_Ammo, m_Set.Gameplay.Magazine);
}

bool FirstPersonPresentation::AttachAndValidate(AssetLibrary& assets, const AnimatorController& ctrl) {
    if (!m_ArmsModel || !m_WeaponModel) return false;
    // Resolve every clip up front. This is intentionally all-or-nothing: an incorrect path or
    // skeleton is reported when Play begins, not halfway through a reload animation.
    const int armsTrack = ctrl.TrackIndex(TrackOr(ctrl, "arms", 0));
    const int weaponTrack = ctrl.TrackIndex(TrackOr(ctrl, "weapon", 1));
    auto check = [&](Model& model, const std::string& clip, const std::string& state, const char* track) {
        if (clip.empty() || ResolveAnimationClip(model, clip, assets) >= 0) return true;
        SetError("state '" + state + "' cannot attach " + track + " clip '" + clip + "'");
        return false;
    };
    // What the driver expects of this weapon, as warnings (a missing Shot / Refill event, a tag or a bone silently
    // turns a feature off): the same list the Weapon Inspector shows.
    {
        FirstPersonWeaponCheckInput wi;
        wi.Set = &m_Set;
        wi.Controller = &ctrl;
        wi.HasArmsBone = [arms = m_ArmsModel.get()](const std::string& b) { return arms->NodeIndex(b) >= 0; };
        wi.HasWeaponBone = [weapon = m_WeaponModel.get()](const std::string& b) { return weapon->NodeIndex(b) >= 0; };
        for (const FPBody::Check& c : FirstPersonWeaponValidate(wi))
            if (c.Level == FPBody::Severity::Warning || c.Level == FPBody::Severity::Error)
                Log::Warn("First-person weapon: " + c.Message + " " + c.Hint);
    }
    for (const auto& L : ctrl.Layers)
        for (const auto& s : L.States)
            for (auto [model, track, name] : {std::tuple<Model*, int, const char*>{m_ArmsModel.get(), armsTrack, "arms"},
                                              std::tuple<Model*, int, const char*>{m_WeaponModel.get(), weaponTrack, "weapon"}}) {
                const auto& m = s.MotionFor(track);
                if (!check(*model, m.Clip, s.Name, name)) return false;
                for (const auto& ch : m.Children)
                    if (!check(*model, ch.Clip, s.Name, name)) return false;
            }
    return true;
}

AnimatorControllerComponent* FirstPersonPresentation::Animator() const {
    if (!m_World || m_Arms == entt::null || !m_World->Registry.valid(m_Arms)) return nullptr;
    return m_World->Registry.try_get<AnimatorControllerComponent>(m_Arms);
}

bool FirstPersonPresentation::HasTag(const char* tag) const {
    const auto* ac = Animator();
    return ac && ac->HasTag(tag);
}

const std::string& FirstPersonPresentation::CurrentState() const {
    static const std::string kNone;
    const auto* ac = Animator();
    return ac ? ac->StateName : kNone;
}

bool FirstPersonPresentation::Fire() {
    auto* ac = Animator();
    if (!ac || !m_Equipped || HasTag(K::kTagHidden)) return false;
    // A tube being loaded a round at a time: the trigger ends the reload after the round in hand
    // (StopReload), and the next pull fires.
    if (m_Set.Gameplay.Reload == FirstPersonWeaponGameplay::ReloadMode::PerRound && ac->HasTag(K::kTagReload)) {
        m_StopReload = m_Ammo > 0;
        return false;
    }
    if (m_Ammo <= 0) return false; // dry: the player has to press Reload themselves
    if (!m_Chambered || ac->HasTag(K::kTagCycling)) return false; // the pump / bolt hasn't been worked yet
    if (WallBlocked()) return false; // tucked off a wall: the muzzle is in it
    // Reloading or otherwise busy hands: no round, whether the state is a hip clip the controller
    // would refuse to interrupt anyway or an ADS one that would otherwise kick procedurally.
    if (ac->HasTag(K::kTagReload) || ac->HasTag(K::kTagBusy)) return false;
    const bool ads = ac->HasTag(K::kTagAds);
    // With recoil.hipProcedural, hip rounds kick procedurally too wherever the gun is simply
    // being held (idle, walking, or still settling from a shot); anywhere else (sprinting, an
    // inspect) the trigger still goes through the controller's Fire state, which cuts it short.
    const bool hipProcedural = m_Set.Procedural.Recoil.HipProcedural &&
                               (ac->HasTag(K::kTagIdle) || ac->HasTag(K::kTagReady));
    if (ads || hipProcedural) {
        // Each round starts its own recoil curves; full-auto overlaps them into a climb.
        m_Procedural.OnShot(m_Set.Procedural, ads);
        m_SinceShot = 0.0f;
        --m_Ammo;
        ShotImpact();
        OnRoundSpent();
        m_IdleTime = 0.0f; // shooting isn't settling: no fidget mid-burst
        return true;
    }
    // Hip fire: the controller plays Fire (or refuses, e.g. mid-reload); the round is spent -
    // and the hip recoil kicks - on its Shot event, so a refused trigger costs nothing.
    ac->SetTrigger(K::kFire);
    return true;
}

void FirstPersonPresentation::FireShot() {
    const auto& g = m_Set.Gameplay;
    if (!m_World || !m_AimPointValid) return;
    // A shotgun's round is several pellets, each down its own line inside the spread cone; the
    // round's shove is shared between them.
    const float spread = HasTag(K::kTagAds) ? g.SpreadAds : g.SpreadHip;
    const int pellets = std::max(1, g.Pellets);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int p = 0; p < pellets; ++p) {
        const glm::vec3 dir = pellets > 1 || spread > 0.0f ? FirstPersonPelletDirection(m_BoreDir, spread, unit(m_Rng), unit(m_Rng))
                                                           : m_BoreDir;
        const float o[3] = {m_Muzzle.x, m_Muzzle.y, m_Muzzle.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        const bool recording = PhysicsWorld::GetQueryRecording();
        PhysicsWorld::SetQueryRecording(false);
        const bool struck = PhysicsWorld::RaycastFiltered(o, d, 300.0f, filter, hit) && hit.Hit;
        PhysicsWorld::SetQueryRecording(recording);
        if (!struck) continue;
        // Bounded, in case nothing drains it (no renderer this session).
        if (m_ShotHits.size() < 256)
            m_ShotHits.push_back({glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]),
                                  glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]), hit.Entity, g.BulletHoleRadius});
        if (g.ImpactImpulse <= 0.0f) continue;
        const auto e = static_cast<entt::entity>(hit.Entity);
        const auto* rb = m_World->Registry.valid(e) ? m_World->Registry.try_get<RigidbodyComponent>(e) : nullptr;
        if (!rb || rb->IsKinematic) continue;
        // Light props: capped at ImpactMaxSpeed of velocity change; heavy ones just get the impulse.
        float impulse = g.ImpactImpulse;
        if (g.ImpactMaxSpeed > 0.0f) impulse = std::min(impulse, g.ImpactMaxSpeed * std::max(rb->Mass, 0.01f));
        const glm::vec3 j = dir * (impulse / (float)pellets);
        const float jv[3] = {j.x, j.y, j.z};
        PhysicsWorld::AddForceAtPosition(hit.Entity, jv, hit.Point, ForceMode::Impulse);
    }
}

void FirstPersonPresentation::OnRoundSpent() {
    if (!m_Set.Gameplay.CycleAfterShot) return;
    m_Chambered = false;
    m_CycleSeen = false;
    m_CycleWait = m_Set.Gameplay.CycleDelay;
}

void FirstPersonPresentation::SelectSlot(int slot) {
    if (!IsActive() || slot < 0 || slot >= SlotCount()) return;
    if (slot == m_Slot) {
        m_PendingSlot = -1;
        SetEquipped(true);
        return;
    }
    // Put this one away first; Tick swaps the rigs once it's holstered (at once if it already is).
    m_PendingSlot = slot;
    m_Equipped = false;
    if (auto* ac = Animator()) ac->SetBool(K::kEquipped, false);
}

void FirstPersonPresentation::CycleSlot(int step) {
    if (!IsActive() || step == 0) return;
    // Positions 0..n-1 are the slots, n is unarmed.
    const int n = SlotCount();
    const int at = m_PendingSlot >= 0 ? m_PendingSlot : (m_Equipped ? m_Slot : n);
    const int next = ((at + (step > 0 ? 1 : -1)) % (n + 1) + (n + 1)) % (n + 1);
    if (next == n) SetEquipped(false);
    else SelectSlot(next);
}

void FirstPersonPresentation::SwapToPendingSlot() {
    if (m_PendingSlot < 0 || !m_World || !m_SlotAssets || !m_Config) return;
    World& world = *m_World;
    const int slot = m_PendingSlot, previous = m_Slot;
    m_PendingSlot = -1;
    m_SlotAmmo[previous] = m_Ammo;
    const float walk = m_WalkSpeed, sprint = m_SprintSpeed;
    if (!StartSet(world, *m_SlotAssets, slot, true)) {
        // Keep the weapon that was in hand rather than leaving the player with nothing.
        Log::Error("First-person presentation: couldn't switch to '" + m_SlotSets[slot] + "'; keeping '" +
                   m_SlotSets[previous] + "'.");
        if (!StartSet(world, *m_SlotAssets, previous, true)) return;
    }
    m_WalkSpeed = walk;
    m_SprintSpeed = sprint;
    SetEquipped(true);
}

void FirstPersonPresentation::ToggleFireMode() {
    if (!IsActive() || !m_Equipped) return;
    if (!m_Set.Gameplay.AllowFullAuto) {
        Log::Info("Fire mode: Semi-Auto (this weapon has no full-auto)");
        return;
    }
    m_FullAuto = !m_FullAuto;
    Log::Info(std::string("Fire mode: ") + (m_FullAuto ? "Full-Auto" : "Semi-Auto"));
}

void FirstPersonPresentation::UpdateTrigger(bool pressed, bool held) {
    if (m_FullAuto) {
        if (!held || m_FireCooldown > 0.0f) return;
    } else if (!pressed) {
        return;
    }
    if (Fire()) m_FireCooldown = 60.0f / m_Set.Gameplay.RoundsPerMinute;
}

void FirstPersonPresentation::UpdateReloadKey(bool down, float dt) {
    switch (m_ReloadKey.Update(down, dt)) {
        case FirstPersonReloadInput::Reload: Reload(); break;
        case FirstPersonReloadInput::MagCheck: TriggerAction(K::kMagCheck); break;
        case FirstPersonReloadInput::None: break;
    }
}

bool FirstPersonPresentation::Reload() {
    auto* ac = Animator();
    if (!ac || !m_Equipped || ac->HasTag(K::kTagReload) || m_Ammo >= m_Set.Gameplay.Magazine) return false;
    m_StopReload = false;
    ac->SetBool(K::kStopReload, false);
    ac->SetInt(K::kAmmo, m_Ammo);
    ac->SetTrigger(K::kReload);
    return true;
}

bool FirstPersonPresentation::TriggerAction(const std::string& trigger) {
    auto* ac = Animator();
    if (!ac || !m_Equipped || ac->HasTag(K::kTagHidden)) return false;
    ac->SetTrigger(trigger);
    return true;
}

void FirstPersonPresentation::SetEquipped(bool equipped) {
    if (!IsActive()) return;
    if (!equipped) m_PendingSlot = -1;
    m_Equipped = equipped;
    if (auto* ac = Animator()) ac->SetBool(K::kEquipped, equipped);
}

void FirstPersonPresentation::Tick(float dt, const glm::vec3& velocity, bool sprinting, bool aiming, float lean, bool grounded) {
    auto* ac = Animator();
    if (!ac) return;
    // Switching weapons: once the one in hand is put away, the other one's rigs come in.
    if (m_PendingSlot >= 0 && ac->HasTag(K::kTagHidden) && !ac->InTransition) {
        SwapToPendingSlot();
        return;
    }
    ReloadIfChanged(dt);
    const FirstPersonWeaponGameplay& g = m_Set.Gameplay;
    const float planarSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
    m_FireCooldown = std::max(0.0f, m_FireCooldown - dt);

    ac->SetFloat(K::kSpeed, planarSpeed);
    m_PlanarSpeed = planarSpeed;
    m_SinceShot += dt;
    m_SinceUnhidden = ac->HasTag(K::kTagHidden) ? 0.0f : m_SinceUnhidden + dt;
    ac->SetBool(K::kSprint, sprinting);
    // The aim press drives the corner peek, even up against cover; tucked off a wall with no
    // peek to lean out on, the sights can't come up.
    m_Aiming = aiming;
    if (WallBlocked() && m_PeekSide == 0) aiming = false;
    ac->SetBool(K::kAim, aiming);
    m_AdsHold = m_Set.Ads.AimHoldTime > 0.0f
                    ? std::clamp(m_AdsHold + (aiming ? dt : -dt) / m_Set.Ads.AimHoldTime, 0.0f, 1.0f)
                    : (aiming ? 1.0f : 0.0f);
    m_TickDt = dt;
    // ADS zoom: in while the sights are up - Aim, or a reload / mag check carried onto them -
    // and out otherwise. A critically damped spring eases both ends, and a re-press mid-way
    // turns around smoothly instead of restarting.
    {
        bool onSights = ac->HasTag(K::kTagAds);
        for (const AdsCarryAction& a : m_AdsCarry.Actions) onSights = onSights || (aiming && a.State == ac->State);
        const float target = onSights && m_Equipped ? 1.0f : 0.0f;
        const float time = m_Set.Ads.ZoomTime;
        if (time <= 0.0f) {
            m_Zoom = target;
            m_ZoomRate = 0.0f;
        } else if (dt > 0.0f) {
            const float w = 4.7f / time; // ~98% settled after `time`
            const float x = m_Zoom - target;
            const float e = std::exp(-w * dt);
            const float next = (x + (m_ZoomRate + w * x) * dt) * e;
            m_ZoomRate = (m_ZoomRate - (m_ZoomRate + w * x) * w * dt) * e;
            m_Zoom = std::clamp(target + next, 0.0f, 1.0f);
        }
    }
    // Stock lock: shouldered in the weapon's listed states, or anything carried on the sights.
    {
        const FirstPersonStockLockSettings& sl = m_Set.StockLock;
        bool shouldered = false;
        if (sl.Enabled && m_Equipped && !ac->HasTag(K::kTagHidden)) {
            shouldered = m_Zoom > 0.5f;
            for (const std::string& tag : sl.Tags) shouldered = shouldered || ac->HasTag(tag.c_str());
        }
        const float step = sl.BlendTime > 0.0f ? dt / sl.BlendTime : 1.0f;
        m_StockLockWeight = std::clamp(m_StockLockWeight + (shouldered ? step : -step), 0.0f, 1.0f);
    }
    ac->SetBool(K::kEquipped, m_Equipped);
    ac->SetInt(K::kAmmo, m_Ammo);
    // A tube loaded a round at a time: the load loop leaves on LastRound (load it and finish) or
    // StopReload (the trigger was pulled: finish the round in hand), which lasts the reload out.
    if (g.Reload == FirstPersonWeaponGameplay::ReloadMode::PerRound) {
        if (!ac->HasTag(K::kTagReload)) m_StopReload = false;
        ac->SetBool(K::kLastRound, m_Ammo >= g.Magazine - 1);
        ac->SetBool(K::kStopReload, m_StopReload);
    }
    // A manual action: CycleDelay after the round, work it (the Cycle trigger, retried until the
    // controller takes it - it waits out a reload or a draw); chambered once that state is done.
    if (g.CycleAfterShot && !m_Chambered) {
        if (ac->HasTag(K::kTagCycling)) {
            m_CycleSeen = true;
        } else if (m_CycleSeen) {
            m_Chambered = true;
        } else {
            m_CycleWait -= dt;
            if (m_CycleWait <= 0.0f && m_Equipped && !ac->HasTag(K::kTagHidden)) ac->SetTrigger(K::kCycle);
        }
    }

    // The procedural stack, fed from what the controller is doing and how the camera moved.
    WeaponProceduralInput in;
    in.Dt = dt;
    if (m_HaveLook && dt > 0.0f) {
        float yaw = m_LookYaw - m_PrevLookYaw;
        yaw = std::remainder(yaw, 360.0f); // yaw is unbounded; never read a wrap as a flick
        in.LookRate = glm::vec2(yaw, m_LookPitch - m_PrevLookPitch) / dt;
    }
    m_PrevLookYaw = m_LookYaw;
    m_PrevLookPitch = m_LookPitch;
    in.Velocity = glm::vec3(glm::dot(velocity, m_FlatRight), 0.0f, -glm::dot(velocity, m_FlatForward));
    in.Sprinting = sprinting && planarSpeed > 0.1f;
    in.VerticalVelocity = velocity.y;
    in.Grounded = grounded;
    in.WallDistance = m_Equipped ? m_WallDistance : -1.0f;
    in.WallFacesUp = m_WallFacesUp;
    in.WallSide = m_WallSide;
    in.Ads = ac->HasTag(K::kTagAds);
    in.IKOff = ac->HasTag(K::kTagHidden) || (!m_Set.Procedural.IK.OffTag.empty() && ac->HasTag(m_Set.Procedural.IK.OffTag.c_str()));
    in.Lean = m_Equipped ? std::clamp(lean + m_PeekLean, -1.0f, 1.0f) : 0.0f;
    in.WalkSpeed = m_WalkSpeed;
    in.SprintSpeed = m_SprintSpeed;
    in.StateName = &ac->StateName;
    in.StateTags = &ac->StateTags;
    const WeaponProceduralPose& pose = m_Procedural.Update(m_Set.Procedural, in);
    ac->SetFloat(K::kWalkRate, pose.WalkRate);
    ac->SetFloat(K::kSprintRate, pose.SprintRate);
    WriteIK();

    // The occasional fidget only interrupts a settled idle, never a pose the player asked for.
    if (m_Equipped && ac->HasTag(K::kTagIdle) && !ac->InTransition) {
        m_IdleTime += dt;
        if (m_IdleTime >= m_RegripDelay) {
            ac->SetTrigger(K::kFidget);
            m_RegripDelay = FirstPersonRegripDelay(std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng),
                                                   g.RegripMin, g.RegripMax);
            m_IdleTime = 0.0f;
        }
    } else {
        m_IdleTime = 0.0f;
    }
}

void FirstPersonPresentation::RemoveViewKick(Camera& camera) {
    if (!m_KickApplied) return;
    camera.Pitch -= m_KickAngles.x;
    camera.Yaw -= m_KickAngles.y;
    camera.Roll -= m_KickAngles.z;
    camera.Position -= m_KickOffset;
    m_KickApplied = false;
    m_KickAngles = m_KickOffset = glm::vec3(0.0f);
}

void FirstPersonPresentation::Update(World& world, Camera& camera) {
    if (!IsActive() || !world.Registry.valid(m_Arms) || !world.Registry.valid(m_Weapon)) return;

    // The player's own look, before any punch goes on: the sway reads how fast it turns.
    RemoveViewKick(camera);
    m_LookYaw = camera.Yaw;
    m_LookPitch = camera.Pitch;
    if (!m_HaveLook) {
        m_PrevLookYaw = m_LookYaw;
        m_PrevLookPitch = m_LookPitch;
        m_HaveLook = true;
    }
    {
        glm::vec3 f = camera.Front();
        f.y = 0.0f;
        m_FlatForward = glm::length(f) > 1e-5f ? glm::normalize(f) : glm::vec3(0.0f, 0.0f, -1.0f);
        m_FlatRight = glm::normalize(glm::cross(m_FlatForward, glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    UpdateCornerPeek(camera);
    // Aim climb: unlike the punch below it stays - the player has to pull it back down.
    {
        const glm::vec2 climb = m_Procedural.Pose().AimKick;
        camera.Pitch = std::clamp(camera.Pitch + climb.x, -89.0f, 89.0f);
        camera.Yaw += climb.y;
    }
    // Recoil view punch and lean, on the camera the whole frame renders from.
    {
        const WeaponProceduralPose& p = m_Procedural.Pose();
        // The lean's side step stops short of walls, or it would put the eye through them; the
        // roll shrinks with it, so leaning against a wall reads as blocked rather than broken.
        float side = p.CameraSide, roll = p.CameraRoll;
        if (std::fabs(side) > 1e-4f) {
            const glm::vec3 dir = camera.Right() * (side > 0.0f ? 1.0f : -1.0f);
            const float origin[3] = {camera.Position.x, camera.Position.y, camera.Position.z};
            const float d[3] = {dir.x, dir.y, dir.z};
            constexpr float kEyeRadius = 0.12f; // keeps the near plane off the wall
            RaycastHit hit;
            QueryFilter filter;
            filter.HitTriggers = 0;
            if (PhysicsWorld::SphereCastFiltered(origin, d, kEyeRadius, std::fabs(side), filter, hit) && hit.Hit) {
                const float room = std::max(0.0f, hit.Distance - 0.02f);
                const float fraction = room / std::fabs(side);
                side *= fraction;
                roll *= fraction;
            }
        }
        // Never punch the view past straight up/down, where the look basis flips.
        const float pitch = std::clamp(camera.Pitch + p.CameraKick.x, -89.0f, 89.0f) - camera.Pitch;
        m_KickAngles = glm::vec3(pitch, p.CameraKick.y, roll);
        camera.Pitch += m_KickAngles.x;
        camera.Yaw += m_KickAngles.y;
        camera.Roll += m_KickAngles.z;
        m_KickOffset = camera.Right() * (side + p.CameraOffset.x) + camera.Up() * p.CameraOffset.y - camera.Front() * p.CameraOffset.z;
        camera.Position += m_KickOffset;
        m_KickApplied = true;
    }
    // What's in front of the gun, for it pulling back off walls - probed from where the eye
    // actually is this frame (leaned, bobbed) and along the rolled barrel line, so peeking past a
    // corner doesn't hit the corner the body is behind. The nearer hit wins, so a wall on the
    // gun's side or a door frame counts as well as what's dead ahead; its surface says which way
    // to tuck (up off a table, aside off an edge beside the barrel).
    m_WallDistance = -1.0f;
    m_WallFacesUp = false;
    m_WallSide = 0.0f;
    if (const auto& ob = m_Set.Procedural.Obstruction; ob.Enabled && ob.Reach > 0.0f) {
        const glm::vec3 f = camera.Front(), right = camera.Right(), up = camera.Up();
        const float d[3] = {f.x, f.y, f.z};
        QueryFilter filter;
        filter.HitTriggers = 0;
        for (const glm::vec3& from : {camera.Position, camera.Position + right * ob.ProbeOffset.x + up * ob.ProbeOffset.y}) {
            const float origin[3] = {from.x, from.y, from.z};
            RaycastHit hit;
            if (PhysicsWorld::SphereCastSolid(origin, d, 0.04f, ob.Reach, filter, hit) && hit.Hit &&
                (m_WallDistance < 0.0f || hit.Distance < m_WallDistance)) {
                const glm::vec3 n(hit.Normal[0], hit.Normal[1], hit.Normal[2]);
                m_WallDistance = hit.Distance;
                m_WallFacesUp = n.y > 0.6f;
                m_WallSide = glm::dot(n, right);
            }
        }
    }

    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Arms);
    if (ac) {
        // What the controller did last frame: rounds spent on hip fire, a reload that landed.
        if (ac->EventFired(K::kEventShot)) {
            m_Ammo = std::max(0, m_Ammo - 1);
            m_Procedural.OnShot(m_Set.Procedural, false, /*cycleBolt=*/false); // the Fire clip cycles it
            m_SinceShot = 0.0f;
            ShotImpact();
            OnRoundSpent();
        }
        if (ac->EventFired(K::kEventRefill)) m_Ammo = m_Set.Gameplay.Magazine;
        if (ac->EventFired(K::kEventLoadRound)) {
            // Into an empty gun the load clip chambers it itself (the empty start works the pump).
            if (m_Ammo == 0) m_Chambered = true;
            m_Ammo = std::min(m_Set.Gameplay.Magazine, m_Ammo + 1);
        }
        ac->FiredEvents.clear();
        // Triggers live for exactly one controller update: an input the controller refused
        // (fire during a reload, say) is dropped rather than firing late.
        for (const char* t : {K::kFire, K::kReload, K::kMagCheck, K::kInspect, K::kMelee, K::kFidget, K::kCycle}) ac->ResetTrigger(t);
    }

    ApplyHidden(world);
    PlaceRigs(world, camera);
}

// Aiming with cover just ahead leans out around it: whichever side clears the edge soonest (the
// gun's side, right, on a tie), picked when the aim starts and kept while it's held, and only as
// far out as clearing the edge takes. `camera` is the player's own view (no lean or punch on it),
// so the test always runs from where the body actually is.
void FirstPersonPresentation::UpdateCornerPeek(const Camera& camera) {
    const WeaponLeanSettings& l = m_Set.Procedural.Lean;
    if (!l.Enabled || !l.CornerPeek || !m_Aiming || !m_Equipped || l.Offset <= 0.0f || l.PeekRange <= 0.0f) {
        m_PeekLean = 0.0f;
        m_PeekSide = 0;
        return;
    }
    QueryFilter filter;
    filter.HitTriggers = 0;
    const glm::vec3 eye = camera.Position, f = camera.Front();
    // Walked off from the cover (still aiming): the peek eases back in.
    if (m_PeekSide != 0) {
        const glm::vec2 moved(eye.x - m_PeekFrom.x, eye.z - m_PeekFrom.z);
        if (glm::length(moved) > l.PeekRange + 0.5f) {
            m_PeekLean = 0.0f;
            m_PeekSide = 0;
        }
    }
    glm::vec3 right = camera.Right();
    right.y = 0.0f;
    right = glm::length(right) > 1e-5f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
    auto cast = [&](const glm::vec3& from, const glm::vec3& dir, float radius, float range, float& dist) {
        const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        if (PhysicsWorld::SphereCastSolid(o, d, radius, range, filter, hit) && hit.Hit) { dist = hit.Distance; return true; }
        return false;
    };
    // Cover straight ahead, close enough to peek from?
    float cover = 0.0f;
    if (!cast(eye, f, 0.05f, l.PeekRange, cover)) {
        // Nothing in the way: no peek - but keep one that's already out (the view is past the
        // edge now and may see clear), until the aim is released.
        if (m_PeekSide == 0) m_PeekLean = 0.0f;
        return;
    }
    // How far out each side the view clears the edge: step out, stopping where the head can't
    // move (a wall beside it), and look past the cover's depth.
    constexpr int kSteps = 10;
    auto clearAt = [&](int side) {
        for (int k = 1; k <= kSteps; ++k) {
            const float off = l.Offset * (float)k / (float)kSteps;
            float blocked = 0.0f;
            if (cast(eye, right * (float)side, 0.12f, off, blocked)) return -1.0f; // no room to lean out
            float ahead = 0.0f;
            if (!cast(eye + right * (off * (float)side), f, 0.05f, cover + 0.6f, ahead) || ahead > cover + 0.45f) return off;
        }
        return -1.0f;
    };
    if (m_PeekSide == 0) {
        const float r = clearAt(1), lft = clearAt(-1);
        m_PeekSide = r >= 0.0f && (lft < 0.0f || r <= lft + 1e-3f) ? 1 : (lft >= 0.0f ? -1 : 0);
        if (m_PeekSide == 0) { m_PeekLean = 0.0f; return; }
        m_PeekFrom = eye;
    }
    const float need = clearAt(m_PeekSide);
    // Out as far as clearing the edge takes (full out if it can't be measured from here any more).
    const float amount = need < 0.0f ? std::fabs(m_PeekLean) : std::min(1.0f, (need + l.PeekMargin) / l.Offset);
    m_PeekLean = (float)m_PeekSide * amount;
}

void FirstPersonPresentation::LateUpdate(World& world, const Camera& camera) {
    if (!IsActive()) return;
    // Again once the animators have run: the frame Holster hands over to Holstered would
    // otherwise render visible, in Holstered's pose (Holster's first frame).
    ApplyHidden(world);
    PlaceRigs(world, camera);
    for (; m_PendingShots > 0; --m_PendingShots) FireShot();
}

// Unarmed hides the rigs the way an unticked "active" box does. DeactivatedTag is what keeps
// InactiveTag from being recomputed away by World::SyncActiveInHierarchy; the animators keep
// running (UpdateWhenInactive) so Draw can bring them back.
void FirstPersonPresentation::ApplyHidden(World& world) {
    const auto* ac = Animator();
    const bool hidden = ac && ac->HasTag(K::kTagHidden);
    if (hidden == m_HiddenApplied) return;
    for (entt::entity e : {m_Arms, m_Weapon}) {
        if (hidden) {
            world.Registry.emplace_or_replace<DeactivatedTag>(e);
            world.Registry.emplace_or_replace<InactiveTag>(e);
        } else {
            world.Registry.remove<DeactivatedTag, InactiveTag>(e);
        }
    }
    m_HiddenApplied = hidden;
}

// Pins the arms to the camera and the weapon to the arms' gun socket, from the rigs' current
// posed node globals. Update runs it before the animators have posed this frame, so on its own
// the gun rode last frame's hands - a frame behind every clip, sway and bob change, which read as
// the left hand sliding on the handguard. LateUpdate runs it again once they have.
void FirstPersonPresentation::UpdateSpareMagazine(const glm::vec3& armsPos, const glm::quat& armsRot,
                                                  const glm::vec3& weaponPos, const glm::quat& weaponRot) {
    if (!m_ArmsModel || !m_WeaponModel || m_Set.SpareMagazineBones.empty()) return;
    glm::mat4 hand(1.0f);
    if (!m_ArmsModel->NodeTransform(m_Set.Procedural.IK.LeftHand, hand)) return;
    const glm::vec3 handWorld = armsPos + armsRot * (m_Scale * glm::vec3(hand[3]));
    const float scale = std::max(m_Scale, 1e-6f);
    std::vector<int> hide;
    for (const std::string& bone : m_Set.SpareMagazineBones) {
        glm::mat4 g(1.0f);
        const int n = m_WeaponModel->NodeIndex(bone);
        if (n < 0 || !m_WeaponModel->NodeTransform(bone, g)) continue; // a collapsed node keeps its pivot
        const glm::vec3 w = weaponPos + weaponRot * (m_Scale * glm::vec3(g[3]));
        if (glm::length(w - handWorld) / scale > m_Set.SpareMagazineGrabDistance) hide.push_back(n);
    }
    if (hide == m_SpareMagHidden) return;
    m_SpareMagHidden = hide;
    m_WeaponModel->SetHiddenNodes(hide);
    // Re-derive this frame's palette from the animator's pose, so it shows or goes this frame.
    if (m_WeaponModel->HasExternalPose()) {
        const std::vector<LocalTRS> pose = m_WeaponModel->AppliedLocalPose();
        m_WeaponModel->ApplyLocalPose(pose);
    }
}

void FirstPersonPresentation::PlaceRigs(World& world, const Camera& camera) {
    const glm::quat cameraRotation = CameraRotation(camera);
    // Without IK (switched off, or a rig lacking the gun bone or arm chains) the procedural pose
    // moves the whole view model about the eye instead, in the camera's own frame - faded by the
    // same weight IK uses, so Off-tagged states still play as authored.
    const WeaponProceduralPose& proc = m_Procedural.Pose();
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat procRotation = m_UsesIK ? identity : glm::slerp(identity, proc.RotationQuat(), proc.IKWeight);
    const glm::vec3 procPosition = m_UsesIK ? glm::vec3(0.0f) : proc.Position * proc.IKWeight;
    // The asset's axis correction is the inner factor (it describes the models' own space), then
    // the scene's View Model Rotation as a tweak on top of that.
    const glm::quat rotation = NormalizeRotation(cameraRotation * procRotation *
                                                 QuaternionFromEulerYXZ(m_Set.ViewRotation) *
                                                 QuaternionFromEulerYXZ(m_Rotation));

    // Placement is anchored on a rig bone, not on the model's root. These rigs are authored
    // standing in their own scene - feet at y=0, head near y=1.56 - so parking the ROOT on the
    // camera stacks that ~1.5 m of authored height on top of the view and leaves the arms and
    // weapon floating overhead. Instead solve for the root that puts the camera bone exactly on
    // the camera: the world point of a local point is
    //     position + rotation * (scale * local)
    // so     position = camera.Position - rotation * (scale * boneLocal).
    // Rotation still comes from the camera, so the rig tracks pitch and the bone stays pinned
    // through it; ViewModelOffset then reads as a residual nudge in the camera's frame.
    //
    //
    // Without IK, an ADS reload / mag check carries the whole rig instead (the IK path moves only
    // the gun - see WriteIK), by that clip's correction C about the camera bone:
    //     world(x) = camera + rotation * scale * (C.R * (x - bone) + C.T)
    glm::vec3 position = camera.Position;
    glm::quat actionR(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 actionT(0.0f);
    if (!m_UsesIK) {
        const AdsCarrySample carry = SampleAdsCarry(0.0f);
        actionR = carry.R;
        actionT = carry.T;
    }
    glm::quat rigRotation = NormalizeRotation(rotation * actionR);
    position += rotation * (m_Scale * actionT);
    glm::mat4 anchor(1.0f);
    if (!m_CameraBone.empty() && m_ArmsModel && m_ArmsModel->NodeTransform(m_CameraBone, anchor)) {
        position -= rigRotation * (m_Scale * glm::vec3(anchor[3]));
    } else if (!m_CameraBone.empty() && !m_CameraBoneWarned) {
        m_CameraBoneWarned = true;
        Log::Warn("First-person presentation: arms model has no '" + m_CameraBone +
                  "' bone; placing the view model's root on the camera instead.");
    }
    position += cameraRotation * (m_Offset + procPosition);

    // The hip carry (Aim.HipPosition / HipRotation): the whole rig, arms and gun together, turned
    // about the gun socket and moved in the camera's frame, by how far the sights are down. Rigid,
    // so every hand contact holds; applied here rather than through IK so Draw and Regrip (IK
    // off) carry it too instead of the gun settling onto it afterwards. "Sights down" is the
    // zoom's, not the ADS tag's: a pump or reload carried on the sights isn't tagged ADS, and
    // the offset fading back in there dropped the gun after every aimed shot.
    const WeaponAimSettings& aim = m_Set.Procedural.Aim;
    const float hip = 1.0f - std::clamp(m_Zoom, 0.0f, 1.0f);
    if (hip > 0.0f && (aim.HipPosition != glm::vec3(0.0f) || aim.HipRotation != glm::vec3(0.0f))) {
        WeaponProceduralPose h;
        h.Rotation = aim.HipRotation;
        const glm::quat turn = NormalizeRotation(cameraRotation * glm::slerp(identity, h.RotationQuat(), hip) *
                                                 glm::inverse(cameraRotation));
        glm::vec3 pivot = position;
        glm::mat4 socket(1.0f);
        const std::string& gun = m_Set.WeaponSocket.empty() ? m_Set.Procedural.IK.GunBone : m_Set.WeaponSocket;
        if (m_ArmsModel && !gun.empty() && m_ArmsModel->NodeTransform(gun, socket))
            pivot = position + rigRotation * (m_Scale * glm::vec3(socket[3]));
        position = pivot + turn * (position - pivot) + cameraRotation * (aim.HipPosition * hip);
        rigRotation = NormalizeRotation(turn * rigRotation);
    }

    world.SetWorldPose(m_Arms, position, rigRotation);
    world.Registry.get<TransformComponent>(m_Arms).Scale = glm::vec3(m_Scale);
    m_ArmsWorld = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rigRotation) * glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale));
    m_View = camera.ViewMatrix();

    // The weapon is parented to the arms rig's gun socket rather than handed the same pose
    // blind (see FirstPersonAnimationSet::WeaponSocket). Both rigs live on entities with this
    // pose, so their posed node globals speak one root space and the attachment solves directly:
    //
    //     weaponEntity * weaponRoot = armsEntity * socket * mount
    //  -> weaponEntity = armsEntity * (socket * mount * weaponRoot^-1)
    //
    // `socket` and `weaponRoot` are the posed node globals - the bind walk while nothing is
    // playing - so the gun tracks the hands rigidly wherever they go. Only the weapon's ROOT is
    // overridden: its own clip channels (bolt, trigger, magazine, ...) still animate underneath,
    // which is the whole point of the separate rig.
    glm::vec3 weaponPosition = position;
    glm::quat weaponRotation = rigRotation;
    if (!m_Set.WeaponSocket.empty() && m_ArmsModel && m_WeaponModel) {
        glm::mat4 socket(1.0f), weaponRoot(1.0f);
        if (m_ArmsModel->NodeTransform(m_Set.WeaponSocket, socket) &&
            m_WeaponModel->NodeTransform(m_Set.WeaponRoot, weaponRoot)) {
            const glm::mat4 mount =
                socket * glm::translate(glm::mat4(1.0f), m_Set.WeaponMountOffset) *
                glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation)) * glm::inverse(weaponRoot);
            weaponPosition = position + rigRotation * (m_Scale * glm::vec3(mount[3]));
            weaponRotation = NormalizeRotation(rigRotation * QuaternionFromMatrix(mount));
        } else if (!m_WeaponSocketWarned) {
            m_WeaponSocketWarned = true;
            Log::Warn("First-person presentation: arms model has no '" + m_Set.WeaponSocket +
                      "' socket or weapon model has no '" + m_Set.WeaponRoot +
                      "' root; the weapon keeps the arms' pose instead.");
        }
    }

    world.SetWorldPose(m_Weapon, weaponPosition, weaponRotation);
    m_WeaponWorld = glm::translate(glm::mat4(1.0f), weaponPosition) * glm::mat4_cast(weaponRotation) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale));
    world.Registry.get<TransformComponent>(m_Weapon).Scale = glm::vec3(m_Scale);
    UpdateSpareMagazine(position, rigRotation, weaponPosition, weaponRotation);

    // Where the barrel points: down the bore from the muzzle to the first thing it meets.
    m_AimPointValid = false;
    m_AimHit = false;
    glm::mat4 rootPose(1.0f);
    if (m_HaveMuzzle && m_WeaponModel->NodeTransform(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot, rootPose)) {
        const glm::mat4 W = glm::translate(glm::mat4(1.0f), weaponPosition) * glm::mat4_cast(weaponRotation) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale)) * rootPose;
        // The gun is drawn at its own FOV (the view-model pass). On screen that is the world
        // projection of the gun stretched about the view axis: view-space x and y times
        // tan(worldFov/2) / tan(viewModelFov/2), at the same depth. Aim from the barrel as SEEN -
        // the muzzle, bore and sight line through that same stretch - so the rounds, the laser and
        // its dot leave the barrel the player sees and the beam is a straight line. On the sights
        // (the view axis, which the stretch leaves where it is) nothing moves.
        glm::mat4 seen(1.0f);
        if (const float vmFov = ViewModelFov(); vmFov > 0.0f && camera.Fov > 0.0f) {
            const float k = std::tan(glm::radians(camera.Fov) * 0.5f) / std::tan(glm::radians(vmFov) * 0.5f);
            const glm::mat4 V = camera.ViewMatrix();
            seen = glm::inverse(V) * glm::scale(glm::mat4(1.0f), glm::vec3(k, k, 1.0f)) * V;
        }
        const glm::mat4 SW = seen * W;
        const glm::vec3 muzzle = glm::vec3(SW * glm::vec4(m_MuzzleLocal, 1.0f));
        const glm::vec3 bore = glm::normalize(glm::mat3(SW) * m_BoreLocal);
        const FirstPersonWeaponGameplay& gp = m_Set.Gameplay;
        // No saved sight line: measure it while the sights are up and steady (settled on the
        // sights, standing still, not just fired, not peeking) - the eye then looks straight
        // down it. Smoothed over the steady spell so breathing and sway average out.
        if (!gp.HasSightLine) {
            const auto* sac = Animator();
            const bool steady = sac && sac->HasTag(K::kTagAds) && m_Zoom > 0.999f && m_AdsHold >= 1.0f &&
                                m_PlanarSpeed < 0.1f && m_SinceShot > 0.6f && m_PeekLean == 0.0f;
            m_SightSettled = steady ? m_SightSettled + m_TickDt : 0.0f;
            if (m_SightSettled > 0.25f) {
                const glm::mat4 Wi = glm::inverse(W);
                const glm::vec3 o = glm::vec3(Wi * glm::vec4(camera.Position, 1.0f));
                const glm::vec3 d = glm::normalize(glm::mat3(Wi) * camera.Front());
                const float k = m_SightMeasured ? 1.0f - std::exp(-m_TickDt / 0.4f) : 1.0f;
                m_SightOrigin = glm::mix(m_SightOrigin, o, k);
                m_SightDirection = glm::normalize(glm::mix(m_SightDirection, d, k));
                m_SightMeasured = true;
                if (!m_SightLogged && m_SightSettled > 1.5f) {
                    m_SightLogged = true;
                    // For the Inspector's Save Measured Sight Line.
                    m_Barrel.SightMeasured = true;
                    m_Barrel.SightOrigin = m_SightOrigin;
                    m_Barrel.SightDirection = m_SightDirection;
                    PublishBarrelReport(m_SetFile.u8string(), m_Barrel);
                    char msg[320];
                    std::snprintf(msg, sizeof msg,
                                  "First-person: sight line measured - save it in the .fpsanim's gameplay block: "
                                  "\"sightLine\": {\"origin\": [%.5f, %.5f, %.5f], \"direction\": [%.6f, %.6f, %.6f]}",
                                  m_SightOrigin.x, m_SightOrigin.y, m_SightOrigin.z, m_SightDirection.x,
                                  m_SightDirection.y, m_SightDirection.z);
                    Log::Info(msg);
                }
            }
        }
        // Zeroed: from the muzzle to the point ZeroDistance metres down the sight line.
        glm::vec3 dir = bore;
        if (gp.ZeroDistance > 0.0f && (gp.HasSightLine || m_SightMeasured)) {
            const glm::vec3 so = glm::vec3(SW * glm::vec4(gp.HasSightLine ? gp.SightOrigin : m_SightOrigin, 1.0f));
            const glm::vec3 sd = glm::normalize(glm::mat3(SW) * (gp.HasSightLine ? gp.SightDirection : m_SightDirection));
            const glm::vec3 zeroAt = so + sd * gp.ZeroDistance;
            if (glm::length(zeroAt - muzzle) > 1e-3f) dir = glm::normalize(zeroAt - muzzle);
        }
        constexpr float kRange = 300.0f;
        const float o[3] = {muzzle.x, muzzle.y, muzzle.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        // Plumbing, not a gameplay query: kept out of the physics debug overlay's raycast lines.
        const bool recording = PhysicsWorld::GetQueryRecording();
        PhysicsWorld::SetQueryRecording(false);
        const bool struck = PhysicsWorld::RaycastFiltered(o, d, kRange, filter, hit) && hit.Hit;
        PhysicsWorld::SetQueryRecording(recording);
        m_AimPoint = muzzle + dir * (struck ? hit.Distance : kRange);
        m_AimHit = struck;
        if (struck) m_AimNormal = glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]);
        m_Muzzle = muzzle;
        m_BoreDir = dir;
        m_AimPointValid = true;
    }
}
