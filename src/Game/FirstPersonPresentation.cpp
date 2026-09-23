#include "FirstPersonPresentation.h"

#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "RotationMath.h"
#include "World.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

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

// ADS recoil, 0..1 over seconds since the shot: a fast linear rise, then an exponential settle
// back onto the sight picture. Short enough that semi-auto tapping never stacks into a drift.
float RecoilKick(float t, const FirstPersonWeaponGameplay& g) {
    if (t < 0.0f) return 0.0f;
    if (t < g.RecoilRise) return t / g.RecoilRise;
    return std::exp(-(t - g.RecoilRise) / g.RecoilSettle);
}

// Past this the kick is invisible; the recoil clock stops.
constexpr float kRecoilDone = 0.6f;

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
    m_RecoilTime = -1.0f;
    m_FullAuto = false;
    m_FireCooldown = 0.0f;
    m_AimBobWeight = 0.0f;
    m_AimBobPhase = 0.0f;
    m_IdleTime = 0.0f;
    m_ReloadKey = {};
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
    if (ac->HasTag(K::kTagAds)) {
        // Re-enter the rise at the current kick rather than from 0, so full-auto shots chain
        // into a shake instead of snapping back onto the sights between rounds.
        m_RecoilTime = RecoilKick(m_RecoilTime, m_Set.Gameplay) * m_Set.Gameplay.RecoilRise;
        --m_Ammo;
        return true;
    }
    // Hip fire: the controller plays Fire (or refuses, e.g. mid-reload); the round is spent on
    // its Shot event, so a refused trigger costs nothing.
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

void FirstPersonPresentation::Tick(float dt, float planarSpeed, bool sprinting, bool aiming) {
    auto* ac = Animator();
    if (!ac) return;
    const FirstPersonWeaponGameplay& g = m_Set.Gameplay;
    if (m_RecoilTime >= 0.0f) {
        m_RecoilTime += dt;
        if (m_RecoilTime > kRecoilDone) m_RecoilTime = -1.0f;
    }
    m_FireCooldown = std::max(0.0f, m_FireCooldown - dt);

    ac->SetFloat(K::kSpeed, planarSpeed);
    ac->SetBool(K::kSprint, sprinting);
    ac->SetBool(K::kAim, aiming);
    ac->SetBool(K::kEquipped, m_Equipped);
    ac->SetInt(K::kAmmo, m_Ammo);

    // The ADS walk bob follows whatever pose is up: it eases in while moving in an ADS state
    // and back out otherwise (stopping, or a reload taking over), so it never pops.
    const bool ads = ac->HasTag(K::kTagAds);
    const float bobTarget = ads ? std::min(planarSpeed / g.BobFullSpeed, 1.0f) : 0.0f;
    m_AimBobWeight += (bobTarget - m_AimBobWeight) * std::min(1.0f, dt * g.BobEase);
    m_AimBobPhase = std::fmod(m_AimBobPhase + dt * planarSpeed * glm::two_pi<float>() / g.BobStride,
                              glm::two_pi<float>());

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

void FirstPersonPresentation::Update(World& world, const Camera& camera) {
    if (!IsActive() || !world.Registry.valid(m_Arms) || !world.Registry.valid(m_Weapon)) return;
    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Arms);
    if (ac) {
        // What the controller did last frame: rounds spent on hip fire, a reload that landed.
        if (ac->EventFired(K::kEventShot)) m_Ammo = std::max(0, m_Ammo - 1);
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

    const FirstPersonWeaponGameplay& g = m_Set.Gameplay;
    const glm::quat cameraRotation = CameraRotation(camera);
    // ADS recoil kicks the whole view model about the eye (the camera bone below stays pinned),
    // in the camera's own frame so it reads as muzzle-up whatever the asset's axis convention.
    const float kick = RecoilKick(m_RecoilTime, g);
    const glm::quat recoil =
        glm::angleAxis(glm::radians(g.RecoilPitchDegrees * kick), glm::vec3(1.0f, 0.0f, 0.0f));
    // The asset's axis correction is the inner factor (it describes the models' own space), then
    // the scene's View Model Rotation as a tweak on top of that.
    const glm::quat rotation = NormalizeRotation(cameraRotation * recoil *
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
    const glm::vec3 bob = m_AimBobWeight * glm::vec3(g.BobSide * std::sin(m_AimBobPhase),
                                                     g.BobVertical * std::sin(2.0f * m_AimBobPhase), 0.0f);
    position += cameraRotation * (m_Offset + kick * g.RecoilOffset + bob);

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
