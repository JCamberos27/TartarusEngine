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
    if (config.AnimationSet.empty()) return true;

    const std::string setPath = ProjectPaths::Resolve(config.AnimationSet);
    m_SetFile = std::filesystem::u8path(setPath);
    {
        std::error_code ec;
        m_SetFileTime = std::filesystem::last_write_time(m_SetFile, ec);
    }
    if (!FirstPersonAnimationSet::LoadFile(setPath, m_Set, &m_LastError)) {
        SetError("could not load '" + config.AnimationSet + "': " + m_LastError);
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
            SetError("could not load the Animator Controller '" + m_Set.Controller + "' named by '" + config.AnimationSet + "'");
            return false;
        }
    } else {
        m_ControllerPath = "memory:" + config.AnimationSet;
        ctrl = std::make_shared<const AnimatorController>(BuildFirstPersonController(m_Set));
        RegisterAnimatorController(m_ControllerPath, ctrl);
    }
    if (ctrl->Layers.empty() || ctrl->Layers[0].States.empty()) {
        SetError("the Animator Controller for '" + config.AnimationSet + "' has no states");
        return false;
    }

    const std::string armsPath = ProjectPaths::Resolve(m_Set.ArmsModel);
    const std::string weaponPath = ProjectPaths::Resolve(m_Set.WeaponModel);
    auto arms = assets.InstantiateModel(armsPath);
    auto weapon = assets.InstantiateModel(weaponPath);
    if (!arms || !weapon) {
        SetError("could not instantiate arms or weapon model from '" + config.AnimationSet + "'");
        return false;
    }

    // Created after the Play snapshot; Stop destroys them before restoring authored scene data.
    m_World = &world;
    m_ArmsModel = arms;
    m_WeaponModel = weapon;
    m_Arms = world.CreateModelEntity(std::move(arms), glm::vec3(0.0f), glm::vec3(0.0f),
                                     glm::vec3(config.ViewModelScale), "[Runtime] First Person Arms");
    m_Weapon = world.CreateModelEntity(std::move(weapon), glm::vec3(0.0f), glm::vec3(0.0f),
                                       glm::vec3(config.ViewModelScale), "[Runtime] First Person Weapon");
    for (entt::entity e : {m_Arms, m_Weapon}) {
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        renderable.CastShadows = RenderableComponent::ShadowCasting::Off;
        renderable.ReceiveShadows = false;
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
                          LogContext::Asset(config.AnimationSet));
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
                          config.AnimationSet + "').", LogContext::Asset(config.AnimationSet));
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
    m_WalkSpeed = config.MoveSpeed;
    m_SprintSpeed = config.MoveSpeed * config.SprintMultiplier;
    // Not validated here: MakePerspective is the engine's one guarded projection constructor
    // (#202) and corrects every degenerate FOV, so an out-of-range value costs a wrong-looking
    // view model, never a broken frame. The Inspector already clamps it to 20..150.
    m_ViewModelFov = config.ViewModelFov;
    m_CameraBone = config.CameraBone;
    m_Ammo = m_Set.Gameplay.Magazine;
    ResetReloadKey();
    m_RegripDelay = FirstPersonRegripDelay(std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng),
                                           m_Set.Gameplay.RegripMin, m_Set.Gameplay.RegripMax);
    armsAnim.SetInt(K::kAmmo, m_Ammo);
    armsAnim.SetBool(K::kEquipped, true);

    if (!AttachAndValidate(assets, *ctrl)) {
        Stop(world);
        return false;
    }
    m_Procedural.Reset();
    m_Procedural.Seed(m_Rng()); // a fresh recoil pattern every Play, not the same one each time
    m_UsesIK = SetupIK();
    SetupBolt(assets, *ctrl);
    SetupAdsActions(assets, *ctrl);
    int states = 0;
    for (const auto& L : ctrl->Layers) states += (int)L.States.size();
    Log::Info("First-person presentation loaded '" + config.AnimationSet + "' (" +
              (m_Set.Controller.empty() ? std::string("v1 clip list") : m_Set.Controller) + ", " +
              std::to_string(states) + " states).");
    return true;
}

void FirstPersonPresentation::Stop(World& world) {
    if (m_Arms != entt::null && world.Registry.valid(m_Arms)) world.DestroyEntityAndChildren(m_Arms);
    if (m_Weapon != entt::null && world.Registry.valid(m_Weapon)) world.DestroyEntityAndChildren(m_Weapon);
    m_World = nullptr;
    m_Arms = entt::null;
    m_Weapon = entt::null;
    m_ArmsModel.reset();
    m_WeaponModel.reset();
    m_Set = {};
    m_ControllerPath.clear();
    m_CameraBone.clear();
    m_ViewModelFov = -1.0f;
    m_CameraBoneWarned = false;
    m_WeaponSocketWarned = false;
    m_Equipped = true;
    m_HiddenApplied = false;
    m_Ammo = m_Set.Gameplay.Magazine;
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
    int clip = -1;
    for (const auto& L : ctrl.Layers)
        for (const auto& st : L.States)
            if (st.Name == "Fire" && clip < 0 && !st.MotionFor(weaponTrack).Clip.empty())
                clip = ResolveAnimationClip(*m_WeaponModel, st.MotionFor(weaponTrack).Clip, assets);
    if (bolt < 0 || clip < 0) {
        Log::Warn("First-person presentation: no '" + r.BoltBone + "' bone or weapon Fire clip to measure the bolt from; no procedural bolt.");
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
    SetupMuzzle(bolt, parents);
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
    return IsActive() ? ZoomedFov(m_ViewModelFov, m_Set.Gameplay.AdsViewModelZoom, m_Zoom) : -1.0f;
}

float FirstPersonPresentation::WorldFov(float baseFov) const {
    return IsActive() ? ZoomedFov(baseFov, m_Set.Gameplay.AdsZoom, m_Zoom) : baseFov;
}

float FirstPersonPresentation::LookScale(float baseFov) const {
    const float fov = WorldFov(baseFov);
    return std::tan(glm::radians(fov) * 0.5f) / std::tan(glm::radians(baseFov) * 0.5f);
}

void FirstPersonPresentation::SetupAdsActions(AssetLibrary& assets, const AnimatorController& ctrl) {
    m_AdsActions.clear();
    if (!m_ArmsModel || ctrl.Layers.empty() || m_Set.WeaponSocket.empty()) return;
    const auto& L = ctrl.Layers[0];
    const int armsTrack = ctrl.TrackIndex(TrackOr(ctrl, "arms", 0));
    const int socket = m_ArmsModel->NodeIndex(m_Set.WeaponSocket);
    const int head = m_CameraBone.empty() ? -1 : m_ArmsModel->NodeIndex(m_CameraBone);
    std::vector<int> parents(m_ArmsModel->NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_ArmsModel->NodeParent(i);
    // The socket at a state's first frame, relative to the camera bone (which sits on the eye).
    auto firstFrame = [&](const std::string& state, glm::mat4& out, std::vector<LocalTRS>& pose) {
        const int si = L.FindState(state);
        if (si < 0 || socket < 0) return false;
        const std::string& path = L.States[si].MotionFor(armsTrack).Clip;
        const int clip = path.empty() ? -1 : ResolveAnimationClip(*m_ArmsModel, path, assets);
        if (clip < 0) return false;
        std::vector<glm::mat4> globals;
        m_ArmsModel->SampleLocalPose(clip, 0.0f, AnimationWrapMode::ClampForever, pose);
        IK::ComputeGlobals(pose, parents, globals);
        const glm::vec3 eye = head >= 0 ? IK::Position(globals[head]) : glm::vec3(0.0f);
        out = glm::translate(glm::mat4(1.0f), -eye) * globals[socket];
        return true;
    };
    glm::mat4 aim;
    std::vector<LocalTRS> aimPose, hipPose;
    if (!firstFrame("Aim", aim, aimPose)) return;
    std::vector<glm::mat4> aimGlobals, hipGlobals;
    IK::ComputeGlobals(aimPose, parents, aimGlobals);
    const IKRigComponent* rig = m_UsesIK && m_World ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    for (const char* name : {"TacReload", "EmptyReload", "MagCheck"}) {
        glm::mat4 hip;
        if (!firstFrame(name, hip, hipPose)) continue;
        const glm::mat4 c = aim * glm::inverse(hip);
        AdsAction a;
        a.State = L.FindState(name);
        a.R = QuaternionFromMatrix(c);
        a.T = glm::vec3(c[3]);
        // Solve the clip's first frame with its correction, as Play will, and measure how far each
        // elbow has to swing about its shoulder->hand line to land on Aim's.
        if (rig && (int)rig->Offsets.size() > kProceduralOffset) {
            IKRigComponent probe = *rig;
            probe.Weight = 1.0f;
            probe.Offsets[kAdsOffset].Rotation = a.R;
            probe.Offsets[kAdsOffset].Position = a.T;
            probe.Offsets[kProceduralOffset] = IKBoneOffset{rig->Offsets[kProceduralOffset].Bone};
            probe.LimbA.Swivel = probe.LimbB.Swivel = 0.0f;
            IK::ApplyRig(probe, *m_ArmsModel, hipPose);
            IK::ComputeGlobals(hipPose, parents, hipGlobals);
            const IKLimb* limbs[2] = {&probe.LimbA, &probe.LimbB};
            for (int arm = 0; arm < 2; ++arm) {
                const int up = m_ArmsModel->NodeIndex(limbs[arm]->Upper), lo = m_ArmsModel->NodeIndex(limbs[arm]->Lower),
                          end = m_ArmsModel->NodeIndex(limbs[arm]->End);
                if (up < 0 || lo < 0 || end < 0) continue;
                const glm::vec3 s = IK::Position(hipGlobals[up]);
                const glm::vec3 n = glm::normalize(IK::Position(hipGlobals[end]) - s);
                glm::vec3 from = IK::Position(hipGlobals[lo]) - s, to = IK::Position(aimGlobals[lo]) - IK::Position(aimGlobals[up]);
                from -= n * glm::dot(from, n);
                to -= n * glm::dot(to, n);
                if (glm::length(from) < 1e-6f || glm::length(to) < 1e-6f) continue;
                a.Swivel[arm] = std::atan2(glm::dot(n, glm::cross(from, to)), glm::dot(from, to));
            }
            // With the swivel on, the solved chain lands on Aim's; what's left is the helper
            // bones the clip keys but IK doesn't solve (the forearm / upper-arm twists, which
            // spread the wrist's roll along the skin). Record their local correction too.
            std::vector<LocalTRS> solved;
            glm::mat4 unused;
            firstFrame(name, unused, solved);
            probe.LimbA.Swivel = a.Swivel[0];
            probe.LimbB.Swivel = a.Swivel[1];
            IK::ApplyRig(probe, *m_ArmsModel, solved);
            for (const IKLimb* limb : limbs) {
                const int up = m_ArmsModel->NodeIndex(limb->Upper), lo = m_ArmsModel->NodeIndex(limb->Lower),
                          end = m_ArmsModel->NodeIndex(limb->End);
                if (up < 0 || lo < 0 || end < 0) continue;
                for (int i = 0; i < m_ArmsModel->NodeCount(); ++i) {
                    const int p = parents[i];
                    if (i == lo || i == end || (p != up && p != lo)) continue; // direct children of the chain only
                    const glm::quat d = glm::normalize(aimPose[i].R * glm::inverse(glm::normalize(solved[i].R)));
                    if (std::fabs(d.w) < 0.99999f) a.Locals.push_back({m_ArmsModel->NodeName(i), d});
                }
            }
        }
        m_AdsActions.push_back(a);
    }
}

// The bolt rides the bore, so its travel is the barrel's axis. The muzzle is the front face of the
// weapon mesh along that axis: the verts nearest the tip and within a few cm of the line, averaged
// (the booster's ring centres on the bore even if the bolt bone sits a little off it). Both are
// kept in the weapon root's space, which is what the socket moves.
void FirstPersonPresentation::SetupMuzzle(int bolt, const std::vector<int>& parents) {
    if (glm::length(m_BoltStroke) < 1e-5f) return;
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
    if (tip < 0.0f) return;
    glm::vec3 sum(0.0f);
    int n = 0;
    for (const glm::vec3& v : verts) {
        const float t = glm::dot(v - origin, axis);
        if (t > tip - stroke * 0.06f && offLine(v, t) < radius) { sum += v; ++n; }
    }
    const glm::mat4 rootInv = root >= 0 ? glm::inverse(globals[root]) : glm::mat4(1.0f);
    m_MuzzleLocal = glm::vec3(rootInv * glm::vec4(sum / (float)n, 1.0f));
    m_BoreLocal = glm::normalize(glm::mat3(rootInv) * axis);
    m_HaveMuzzle = true;
    char msg[160];
    std::snprintf(msg, sizeof msg, "First-person: bolt stroke %.1f, muzzle %.1f ahead of the bolt (model units x100).",
                  stroke * 100.0f, tip * 100.0f);
    Log::Info(msg);
}

bool FirstPersonPresentation::BarrelAimPoint(glm::vec3& out) const {
    const auto* ac = Animator();
    if (!m_AimPointValid || !ac || !m_Equipped) return false;
    if (!(ac->HasTag(K::kTagIdle) || ac->StateName == "Walk" || ac->StateName == "Fire")) return false;
    out = m_AimPoint;
    return true;
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
    float swivel[2], adsW = 0.0f;
    const AdsAction* action = nullptr;
    AdsActionCorrection(m_TickDt, adsR, adsT, swivel, &action, &adsW);
    rig->LimbA.Swivel = swivel[0];
    rig->LimbB.Swivel = swivel[1];
    rig->LocalRotations.clear();
    if (action)
        for (const auto& [bone, d] : action->Locals)
            rig->LocalRotations.push_back({bone, glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), d, adsW)});
    rig->Offsets[kAdsOffset].Rotation = adsR;
    rig->Offsets[kAdsOffset].Position = adsT;
    rig->Offsets[kProceduralOffset].Position = Ci * p.Position / m_Scale;
    rig->Offsets[kProceduralOffset].Rotation = NormalizeRotation(Ci * p.RotationQuat() * C);
    rig->Offsets[kProceduralOffset].Pivot = Ci * p.Pivot / m_Scale;
}

bool FirstPersonPresentation::AdsActionCorrection(float dt, glm::quat& rotation, glm::vec3& translation,
                                                  float swivel[2], const AdsAction** action, float* weight) const {
    rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    translation = glm::vec3(0.0f);
    if (swivel) swivel[0] = swivel[1] = 0.0f;
    if (action) *action = nullptr;
    if (weight) *weight = 0.0f;
    const auto* ac = Animator();
    if (!ac || ac->Layers.empty() || m_AdsHold <= 0.0f || m_AdsActions.empty()) return false;
    // Each crossfade entry fades in over everything beneath it; walk down from the top, with each
    // fade advanced the way the controller is about to (so the offset matches the pose it poses).
    const auto& stack = ac->Layers[0].Stack;
    float remaining = 1.0f, total = 0.0f, best = 0.0f;
    const AdsAction* dominant = nullptr;
    for (int k = (int)stack.size() - 1; k >= 0 && remaining > 0.0f; --k) {
        float fade = stack[k].Fade;
        if (dt > 0.0f && fade < 1.0f)
            fade = stack[k].FadeDuration > 0.0f ? std::min(1.0f, fade + dt / stack[k].FadeDuration) : 1.0f;
        const float w = k == 0 ? remaining : remaining * AnimatorCrossfadeWeight(fade);
        remaining -= w;
        for (const AdsAction& a : m_AdsActions)
            if (a.State == stack[k].State) {
                total += w;
                if (w > best) { best = w; dominant = &a; }
            }
    }
    if (!dominant) return false;
    const float w = total * m_AdsHold;
    rotation = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), dominant->R, w);
    translation = dominant->T * w;
    if (swivel) { swivel[0] = dominant->Swivel[0] * w; swivel[1] = dominant->Swivel[1] * w; }
    if (action) *action = dominant;
    if (weight) *weight = w;
    return true;
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
    m_Set.Gameplay = fresh.Gameplay;
    m_Set.Procedural = fresh.Procedural;
    if (rebuildIK && m_World && m_World->Registry.valid(m_Arms)) {
        m_World->Registry.remove<IKRigComponent>(m_Arms);
        m_UsesIK = SetupIK();
    }
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
    if (m_Ammo <= 0) return false; // dry: the player has to press Reload themselves
    const bool ads = ac->HasTag(K::kTagAds);
    // With recoil.hipProcedural, hip rounds kick procedurally too wherever the gun is simply
    // being held (idle, walking, or still settling from a shot); anywhere else (sprinting, an
    // inspect) the trigger still goes through the controller's Fire state, which cuts it short.
    const bool hipProcedural = m_Set.Procedural.Recoil.HipProcedural &&
                               (ac->HasTag(K::kTagIdle) || ac->StateName == "Walk" || ac->StateName == "Fire");
    if (ads || hipProcedural) {
        // Each round starts its own recoil curves; full-auto overlaps them into a climb.
        m_Procedural.OnShot(m_Set.Procedural, ads);
        --m_Ammo;
        ShotImpact();
        m_IdleTime = 0.0f; // shooting isn't settling: no fidget mid-burst
        return true;
    }
    // Hip fire: the controller plays Fire (or refuses, e.g. mid-reload); the round is spent -
    // and the hip recoil kicks - on its Shot event, so a refused trigger costs nothing.
    ac->SetTrigger(K::kFire);
    return true;
}

void FirstPersonPresentation::ShotImpact() {
    const auto& g = m_Set.Gameplay;
    if (!m_World || !m_AimPointValid || g.ImpactImpulse <= 0.0f) return;
    const float o[3] = {m_Muzzle.x, m_Muzzle.y, m_Muzzle.z}, d[3] = {m_BoreDir.x, m_BoreDir.y, m_BoreDir.z};
    RaycastHit hit;
    QueryFilter filter;
    filter.HitTriggers = 0;
    const bool recording = PhysicsWorld::GetQueryRecording();
    PhysicsWorld::SetQueryRecording(false);
    const bool struck = PhysicsWorld::RaycastFiltered(o, d, 300.0f, filter, hit) && hit.Hit;
    PhysicsWorld::SetQueryRecording(recording);
    if (!struck) return;
    const auto e = static_cast<entt::entity>(hit.Entity);
    const auto* rb = m_World->Registry.valid(e) ? m_World->Registry.try_get<RigidbodyComponent>(e) : nullptr;
    if (!rb || rb->IsKinematic) return;
    // Light props: capped at ImpactMaxSpeed of velocity change; heavy ones just get the impulse.
    float impulse = g.ImpactImpulse;
    if (g.ImpactMaxSpeed > 0.0f) impulse = std::min(impulse, g.ImpactMaxSpeed * std::max(rb->Mass, 0.01f));
    const glm::vec3 j = m_BoreDir * impulse;
    const float jv[3] = {j.x, j.y, j.z};
    PhysicsWorld::AddForceAtPosition(hit.Entity, jv, hit.Point, ForceMode::Impulse);
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
    m_Equipped = equipped;
    if (auto* ac = Animator()) ac->SetBool(K::kEquipped, equipped);
}

void FirstPersonPresentation::Tick(float dt, const glm::vec3& velocity, bool sprinting, bool aiming, float lean) {
    auto* ac = Animator();
    if (!ac) return;
    ReloadIfChanged(dt);
    const FirstPersonWeaponGameplay& g = m_Set.Gameplay;
    const float planarSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
    m_FireCooldown = std::max(0.0f, m_FireCooldown - dt);

    ac->SetFloat(K::kSpeed, planarSpeed);
    ac->SetBool(K::kSprint, sprinting);
    ac->SetBool(K::kAim, aiming);
    m_AdsHold = std::clamp(m_AdsHold + (aiming ? dt : -dt) / 0.15f, 0.0f, 1.0f);
    m_TickDt = dt;
    // ADS zoom: in while the sights are up - Aim, or a reload / mag check carried onto them -
    // and out otherwise. A critically damped spring eases both ends, and a re-press mid-way
    // turns around smoothly instead of restarting.
    {
        bool onSights = ac->HasTag(K::kTagAds);
        for (const AdsAction& a : m_AdsActions) onSights = onSights || (aiming && a.State == ac->State);
        const float target = onSights && m_Equipped ? 1.0f : 0.0f;
        const float time = m_Set.Gameplay.AdsZoomTime;
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
    ac->SetBool(K::kEquipped, m_Equipped);
    ac->SetInt(K::kAmmo, m_Ammo);

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
    in.Ads = ac->HasTag(K::kTagAds);
    in.IKOff = ac->HasTag(K::kTagHidden) || (!m_Set.Procedural.IK.OffTag.empty() && ac->HasTag(m_Set.Procedural.IK.OffTag.c_str()));
    in.Lean = m_Equipped ? lean : 0.0f;
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
        m_KickOffset = camera.Right() * side;
        camera.Position += m_KickOffset;
        m_KickApplied = true;
    }

    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Arms);
    if (ac) {
        // What the controller did last frame: rounds spent on hip fire, a reload that landed.
        if (ac->EventFired(K::kEventShot)) {
            m_Ammo = std::max(0, m_Ammo - 1);
            m_Procedural.OnShot(m_Set.Procedural, false, /*cycleBolt=*/false); // the Fire clip cycles it
            ShotImpact();
        }
        if (ac->EventFired(K::kEventRefill)) m_Ammo = m_Set.Gameplay.Magazine;
        ac->FiredEvents.clear();
        // Triggers live for exactly one controller update: an input the controller refused
        // (fire during a reload, say) is dropped rather than firing late.
        for (const char* t : {K::kFire, K::kReload, K::kMagCheck, K::kInspect, K::kMelee, K::kFidget}) ac->ResetTrigger(t);
    }

    // Unarmed hides the rigs the way an unticked "active" box does. DeactivatedTag is what keeps
    // InactiveTag from being recomputed away by World::SyncActiveInHierarchy; the animators keep
    // running (UpdateWhenInactive) so Draw can bring them back.
    const bool hidden = ac && ac->HasTag(K::kTagHidden);
    if (hidden != m_HiddenApplied) {
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

    PlaceRigs(world, camera);
}

void FirstPersonPresentation::LateUpdate(World& world, const Camera& camera) {
    if (IsActive()) PlaceRigs(world, camera);
}

// Pins the arms to the camera and the weapon to the arms' gun socket, from the rigs' current
// posed node globals. Update runs it before the animators have posed this frame, so on its own
// the gun rode last frame's hands - a frame behind every clip, sway and bob change, which read as
// the left hand sliding on the handguard. LateUpdate runs it again once they have.
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
    if (!m_UsesIK) AdsActionCorrection(0.0f, actionR, actionT);
    const glm::quat rigRotation = NormalizeRotation(rotation * actionR);
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

    world.SetWorldPose(m_Arms, position, rigRotation);
    world.Registry.get<TransformComponent>(m_Arms).Scale = glm::vec3(m_Scale);

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
                socket * glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation)) *
                glm::inverse(weaponRoot);
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
    world.Registry.get<TransformComponent>(m_Weapon).Scale = glm::vec3(m_Scale);

    // Where the barrel points: down the bore from the muzzle to the first thing it meets.
    m_AimPointValid = false;
    glm::mat4 rootPose(1.0f);
    if (m_HaveMuzzle && m_WeaponModel->NodeTransform(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot, rootPose)) {
        const glm::mat4 W = glm::translate(glm::mat4(1.0f), weaponPosition) * glm::mat4_cast(weaponRotation) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale)) * rootPose;
        const glm::vec3 muzzle = glm::vec3(W * glm::vec4(m_MuzzleLocal, 1.0f));
        const glm::vec3 dir = glm::normalize(glm::mat3(W) * m_BoreLocal);
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
        m_Muzzle = muzzle;
        m_BoreDir = dir;
        m_AimPointValid = true;
    }
}
