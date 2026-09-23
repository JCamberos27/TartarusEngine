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
    m_UsesIK = SetupIK();
    ComputeAimClipSight(assets, *ctrl);
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
    m_HaveAimClipSight = false;
    m_WallBlock = 0.0f;
    m_SetFile.clear();
    m_ReloadPoll = 0.0f;
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
    rig.Offsets.assign(1, IKBoneOffset{});
    rig.Offsets[0].Bone = k.GunBone;

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
        probe.Offsets[0].Position = glm::length(toShoulder) > 1e-6f ? glm::normalize(toShoulder) * armLength * 0.1f : glm::vec3(0.0f);
        probe.Offsets[0].Rotation = glm::angleAxis(glm::radians(5.0f), glm::vec3(1.0f, 0.0f, 0.0f));
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

void FirstPersonPresentation::ComputeAimClipSight(AssetLibrary& assets, const AnimatorController& ctrl) {
    m_HaveAimClipSight = false;
    if (!m_UsesIK || !m_ArmsModel || !m_WeaponModel || m_CameraBone.empty() || m_Set.WeaponSocket.empty()) return;
    // The first state tagged ADS, and its arms clip.
    const AnimatorController::State* aim = nullptr;
    for (const auto& L : ctrl.Layers) {
        for (const auto& st : L.States)
            if (std::find(st.Tags.begin(), st.Tags.end(), K::kTagAds) != st.Tags.end()) { aim = &st; break; }
        if (aim) break;
    }
    if (!aim) return;
    const auto& motion = aim->MotionFor(ctrl.TrackIndex(TrackOr(ctrl, "arms", 0)));
    const int clip = motion.Clip.empty() ? -1 : ResolveAnimationClip(*m_ArmsModel, motion.Clip, assets);
    if (clip < 0) return;

    std::vector<LocalTRS> pose;
    m_ArmsModel->SampleLocalPose(clip, m_ArmsModel->AnimationLength(clip) * 0.5f, AnimationWrapMode::Loop, pose);
    std::vector<int> parents(pose.size());
    for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m_ArmsModel->NodeParent(i);
    std::vector<glm::mat4> globals;
    IK::ComputeGlobals(pose, parents, globals);
    const int head = m_ArmsModel->NodeIndex(m_CameraBone);
    const int socket = m_ArmsModel->NodeIndex(m_Set.WeaponSocket);
    if (head < 0 || socket < 0) return;

    // The eye and view axis in model space: a camera-frame point x sits at
    // anchor + C^-1 (x - ViewModelOffset) / scale (see WriteIK / Update).
    const glm::quat Ci = glm::inverse(NormalizeRotation(QuaternionFromEulerYXZ(m_Set.ViewRotation) * QuaternionFromEulerYXZ(m_Rotation)));
    const glm::vec3 eye = IK::Position(globals[head]) - Ci * m_Offset / m_Scale;
    const glm::vec3 fwd = glm::normalize(Ci * glm::vec3(0.0f, 0.0f, -1.0f));
    // The line: the view axis, from level with the socket to 25 cm further out.
    const float depth = glm::dot(IK::Position(globals[socket]) - eye, fwd);
    const glm::vec3 rear = eye + fwd * depth;
    const glm::vec3 front = rear + fwd * (0.25f / m_Scale);
    const glm::mat4 toSocket = glm::inverse(globals[socket]);
    m_AimClipRear = glm::vec3(toSocket * glm::vec4(rear, 1.0f));
    m_AimClipFront = glm::vec3(toSocket * glm::vec4(front, 1.0f));
    m_HaveAimClipSight = true;

    // Self-check on the real rig: knock the gun 3 cm and 4 degrees off the Aim pose, align, and
    // measure how far the sight line ends up from the view axis.
    {
        IKRigComponent probe;
        probe.Enabled = true;
        probe.Align.Active = true;
        probe.Align.MoveBone = m_Set.Procedural.IK.GunBone;
        probe.Align.RefBone = m_Set.WeaponSocket;
        probe.Align.RearLocal = m_AimClipRear;
        probe.Align.FrontLocal = m_AimClipFront;
        probe.Align.Eye = eye;
        probe.Align.Forward = fwd;
        probe.Align.Weight = 1.0f;
        std::vector<LocalTRS> knocked = pose;
        std::vector<glm::mat4> kg = globals;
        const int gun = m_ArmsModel->NodeIndex(m_Set.Procedural.IK.GunBone);
        IK::OffsetBone(knocked, parents, kg, gun, glm::vec3(0.03f, -0.02f, 0.01f) / m_Scale,
                       glm::angleAxis(glm::radians(4.0f), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f))), IK::Position(kg[gun]));
        IK::ApplyRig(probe, *m_ArmsModel, knocked);
        IK::ComputeGlobals(knocked, parents, kg);
        const glm::vec3 r = glm::vec3(kg[socket] * glm::vec4(m_AimClipRear, 1.0f));
        const glm::vec3 f = glm::vec3(kg[socket] * glm::vec4(m_AimClipFront, 1.0f));
        const float missMm = glm::length((r - eye) - fwd * glm::dot(r - eye, fwd)) * m_Scale * 1000.0f;
        const float missDeg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(f - r), fwd), -1.0f, 1.0f)));
        char msg[192];
        std::snprintf(msg, sizeof msg, "First-person sights: alignment holds the sight line to %.3f mm / %.3f deg of the view axis (probe 3 cm, 4 deg off).",
                      missMm, missDeg);
        if (missMm < 0.5f && missDeg < 0.05f) Log::Info(msg);
        else Log::Warn(msg);
    }

    // The same line in the weapon root's terms, for a Manual setup to start from.
    glm::mat4 weaponRoot(1.0f);
    if (m_WeaponModel->NodeTransform(m_Set.WeaponRoot, weaponRoot)) {
        const glm::mat4 socketToWeapon =
            weaponRoot * glm::inverse(glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation)));
        const glm::vec3 r = glm::vec3(socketToWeapon * glm::vec4(m_AimClipRear, 1.0f));
        const glm::vec3 f = glm::vec3(socketToWeapon * glm::vec4(m_AimClipFront, 1.0f));
        char msg[256];
        std::snprintf(msg, sizeof msg,
                      "First-person sights: the '%s' clip's sight line in the weapon root's space: rear (%.4f, %.4f, %.4f), front (%.4f, %.4f, %.4f).",
                      aim->Name.c_str(), r.x, r.y, r.z, f.x, f.y, f.z);
        Log::Info(msg);
    }
}

bool FirstPersonPresentation::SightLine(glm::vec3& rear, glm::vec3& front) const {
    const WeaponSightSettings& s = m_Set.Procedural.Sight;
    if (s.Mode == WeaponSightSettings::AimClip) {
        rear = m_AimClipRear;
        front = m_AimClipFront;
        return m_HaveAimClipSight;
    }
    if (s.Mode != WeaponSightSettings::Manual || !m_WeaponModel) return false;
    // Weapon-rig bone space -> socket space: socket * mount * weaponRoot^-1 places the weapon
    // model (Update), so a weapon-model point p is at mount * weaponRoot^-1 * p under the socket.
    const std::string& refName = s.ReferenceBone.empty() ? m_Set.WeaponRoot : s.ReferenceBone;
    glm::mat4 weaponRoot(1.0f), ref(1.0f);
    if (!m_WeaponModel->NodeTransform(m_Set.WeaponRoot, weaponRoot) || !m_WeaponModel->NodeTransform(refName, ref))
        return false;
    const glm::mat4 toSocket =
        glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation)) * glm::inverse(weaponRoot) * ref;
    rear = glm::vec3(toSocket * glm::vec4(s.Rear, 1.0f));
    front = glm::vec3(toSocket * glm::vec4(s.Front, 1.0f));
    return glm::length(front - rear) > 1e-6f;
}

// Camera-frame procedural pose -> the arms rig's model space. The arms entity is rotated
// camera * C (C = the asset's view rotation, then the scene's tweak) and scaled by m_Scale, so a
// camera-frame vector v is C^-1 v / scale in model space, and a rotation q is C^-1 q C.
void FirstPersonPresentation::WriteIK() {
    if (!m_UsesIK || !m_World || !m_World->Registry.valid(m_Arms)) return;
    auto* rig = m_World->Registry.try_get<IKRigComponent>(m_Arms);
    if (!rig || rig->Offsets.empty()) return;
    const WeaponProceduralPose& p = m_Procedural.Pose();
    const glm::quat C = NormalizeRotation(QuaternionFromEulerYXZ(m_Set.ViewRotation) * QuaternionFromEulerYXZ(m_Rotation));
    const glm::quat Ci = glm::inverse(C);
    rig->Weight = p.IKWeight;
    rig->Offsets[0].Position = Ci * p.Position / m_Scale;
    rig->Offsets[0].Rotation = NormalizeRotation(Ci * p.RotationQuat() * C);
    rig->Offsets[0].Pivot = Ci * p.Pivot / m_Scale;

    // Sight alignment toward the eye the camera is placed on this frame: the camera bone's
    // last pose, which is what Update anchored the rig with.
    IKLineAlign& a = rig->Align;
    a.Active = false;
    const WeaponSightSettings& sight = m_Set.Procedural.Sight;
    glm::mat4 anchor(1.0f);
    if (sight.Mode != WeaponSightSettings::Off && p.AdsWeight > 0.0f && !m_Set.WeaponSocket.empty() &&
        m_ArmsModel && m_ArmsModel->NodeTransform(m_CameraBone, anchor) && SightLine(a.RearLocal, a.FrontLocal)) {
        a.Active = true;
        a.MoveBone = m_Set.Procedural.IK.GunBone;
        a.RefBone = m_Set.WeaponSocket;
        a.Eye = glm::vec3(anchor[3]) - Ci * m_Offset / m_Scale;
        a.Forward = glm::normalize(Ci * glm::vec3(0.0f, 0.0f, -1.0f));
        a.Distance = sight.EyeDistance > 0.0f ? sight.EyeDistance / m_Scale : 0.0f;
        a.Weight = std::clamp(sight.Weight, 0.0f, 1.0f) * p.AdsWeight;
    }
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
    // Gun pulled back from a wall: optionally no firing into it.
    if (m_Set.Procedural.Wall.BlockFire && m_Procedural.Pose().WallBlock > 0.5f) return false;
    if (ac->HasTag(K::kTagAds)) {
        // Each round starts its own recoil curves; full-auto overlaps them into a climb.
        m_Procedural.OnShot(m_Set.Procedural, true);
        --m_Ammo;
        return true;
    }
    // Hip fire: the controller plays Fire (or refuses, e.g. mid-reload); the round is spent -
    // and the hip recoil kicks - on its Shot event, so a refused trigger costs nothing.
    ac->SetTrigger(K::kFire);
    return true;
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
    in.WallBlock = m_WallBlock;
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
        // Wall check: how far into "too close" the view is, straight ahead of the eye.
        m_WallBlock = 0.0f;
        const WeaponWallSettings& wall = m_Set.Procedural.Wall;
        if (wall.Enabled && wall.Distance > 0.0f) {
            const glm::vec3 ahead = camera.Front();
            const float from[3] = {camera.Position.x, camera.Position.y, camera.Position.z};
            const float along[3] = {ahead.x, ahead.y, ahead.z};
            RaycastHit wallHit;
            QueryFilter wallFilter;
            wallFilter.HitTriggers = 0;
            if (PhysicsWorld::SphereCastFiltered(from, along, std::max(wall.Radius, 0.005f), wall.Distance, wallFilter, wallHit) &&
                wallHit.Hit)
                // Starts at the edge of the range, full with the wall 30% of the way out.
                m_WallBlock = std::clamp((wall.Distance - wallHit.Distance) / (wall.Distance * 0.7f), 0.0f, 1.0f);
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
            m_Procedural.OnShot(m_Set.Procedural, false);
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
    glm::vec3 position = camera.Position;
    glm::mat4 anchor(1.0f);
    if (!m_CameraBone.empty() && m_ArmsModel && m_ArmsModel->NodeTransform(m_CameraBone, anchor)) {
        position -= rotation * (m_Scale * glm::vec3(anchor[3]));
    } else if (!m_CameraBone.empty() && !m_CameraBoneWarned) {
        m_CameraBoneWarned = true;
        Log::Warn("First-person presentation: arms model has no '" + m_CameraBone +
                  "' bone; placing the view model's root on the camera instead.");
    }
    position += cameraRotation * (m_Offset + procPosition);

    world.SetWorldPose(m_Arms, position, rotation);
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
    glm::quat weaponRotation = rotation;
    if (!m_Set.WeaponSocket.empty() && m_ArmsModel && m_WeaponModel) {
        glm::mat4 socket(1.0f), weaponRoot(1.0f);
        if (m_ArmsModel->NodeTransform(m_Set.WeaponSocket, socket) &&
            m_WeaponModel->NodeTransform(m_Set.WeaponRoot, weaponRoot)) {
            const glm::mat4 mount =
                socket * glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation)) *
                glm::inverse(weaponRoot);
            weaponPosition = position + rotation * (m_Scale * glm::vec3(mount[3]));
            weaponRotation = NormalizeRotation(rotation * QuaternionFromMatrix(mount));
        } else if (!m_WeaponSocketWarned) {
            m_WeaponSocketWarned = true;
            Log::Warn("First-person presentation: arms model has no '" + m_Set.WeaponSocket +
                      "' socket or weapon model has no '" + m_Set.WeaponRoot +
                      "' root; the weapon keeps the arms' pose instead.");
        }
    }

    world.SetWorldPose(m_Weapon, weaponPosition, weaponRotation);
    world.Registry.get<TransformComponent>(m_Weapon).Scale = glm::vec3(m_Scale);
}
