#include "FirstPersonPresentation.h"

#include "AnimationSystem.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "RotationMath.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

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

    const std::string armsPath = ProjectPaths::Resolve(m_Set.ArmsModel);
    const std::string weaponPath = ProjectPaths::Resolve(m_Set.WeaponModel);
    auto arms = assets.InstantiateModel(armsPath);
    auto weapon = assets.InstantiateModel(weaponPath);
    if (!arms || !weapon) {
        SetError("could not instantiate arms or weapon model from '" + config.AnimationSet + "'");
        return false;
    }

    // Created after the Play snapshot; Stop destroys them before restoring authored scene data.
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
    m_Offset = config.ViewModelOffset;
    m_Rotation = config.ViewModelRotation;
    m_Scale = config.ViewModelScale;
    // Not validated here: MakePerspective is the engine's one guarded projection constructor
    // (#202) and corrects every degenerate FOV, so an out-of-range value costs a wrong-looking
    // view model, never a broken frame. The Inspector already clamps it to 20..150.
    m_ViewModelFov = config.ViewModelFov;
    m_CameraBone = config.CameraBone;

    if (!AttachAndValidate(assets) || !SetState(m_Set.DefaultState)) {
        Stop(world);
        return false;
    }
    Log::Info("First-person presentation loaded '" + config.AnimationSet + "' with " +
              std::to_string(m_Set.Clips.size()) + " semantic states.");
    return true;
}

void FirstPersonPresentation::Stop(World& world) {
    if (m_Arms != entt::null && world.Registry.valid(m_Arms)) world.DestroyEntityAndChildren(m_Arms);
    if (m_Weapon != entt::null && world.Registry.valid(m_Weapon)) world.DestroyEntityAndChildren(m_Weapon);
    m_Arms = entt::null;
    m_Weapon = entt::null;
    m_ArmsModel.reset();
    m_WeaponModel.reset();
    m_CurrentState.clear();
    m_Set = {};
    m_CameraBone.clear();
    m_ViewModelFov = -1.0f;
    m_CameraBoneWarned = false;
    m_ActionGateArms = false;
    m_ActionGateWeapon = false;
    m_ActionState.clear();
    m_ActionTier = FirstPersonActionTier::None;
    m_Equipped = true;
}

bool FirstPersonPresentation::AttachAndValidate(AssetLibrary& assets) {
    if (!m_ArmsModel || !m_WeaponModel) return false;
    // Resolve every asset up front. This is intentionally all-or-nothing: an incorrect path or
    // skeleton is reported when Play begins, not halfway through a reload animation.
    for (const FirstPersonAnimationClip& clip : m_Set.Clips) {
        if (!clip.ArmsBindPose && ResolveAnimationClip(*m_ArmsModel, clip.ArmsClip, assets) < 0) {
            SetError("state '" + clip.Name + "' cannot attach arms clip '" + clip.ArmsClip + "'");
            return false;
        }
        if (!clip.WeaponClip.empty() && ResolveAnimationClip(*m_WeaponModel, clip.WeaponClip, assets) < 0) {
            SetError("state '" + clip.Name + "' cannot attach weapon clip '" + clip.WeaponClip + "'");
            return false;
        }
    }
    return true;
}

bool FirstPersonPresentation::Play(const FirstPersonAnimationClip& clip) {
    if (!m_ArmsModel || !m_WeaponModel) return false;
    const AnimationWrapMode wrap = clip.Loop ? AnimationWrapMode::Loop : AnimationWrapMode::Once;
    if (clip.ArmsBindPose) {
        m_ArmsModel->StopAnimation();
    } else {
        const int armsClip = m_ArmsModel->FindClipByRef(clip.ArmsClip);
        if (armsClip < 0) {
            SetError("state '" + clip.Name + "' has no attached arms clip");
            return false;
        }
        m_ArmsModel->PlayAnimation(armsClip, clip.Fade, wrap);
    }
    if (clip.WeaponClip.empty()) {
        // The source collection does not contain a weapon counterpart for this state. Resetting
        // to bind pose is deterministic and makes the missing counterpart visible in review.
        m_WeaponModel->StopAnimation();
    } else {
        const int weaponClip = m_WeaponModel->FindClipByRef(clip.WeaponClip);
        if (weaponClip < 0) {
            SetError("state '" + clip.Name + "' has no attached weapon clip");
            return false;
        }
        m_WeaponModel->PlayAnimation(weaponClip, clip.Fade, wrap);
    }
    m_CurrentState = clip.Name;
    return true;
}

bool FirstPersonPresentation::SetState(const std::string& state) {
    if (state == m_CurrentState) return true;
    const FirstPersonAnimationClip* clip = m_Set.Find(state);
    if (!clip) {
        SetError("unknown semantic state '" + state + "'");
        return false;
    }
    return Play(*clip);
}

bool FirstPersonPresentation::StartAction(const std::string& state, FirstPersonActionTier tier) {
    const FirstPersonAnimationClip* clip = m_Set.Find(state);
    if (!clip) {
        SetError("unknown semantic state '" + state + "'");
        return false;
    }
    if (!Play(*clip)) return false;
    // A bind-pose arms slot or an absent weapon clip is never "playing", so it can't gate
    // completion - only the rig(s) actually running a timed clip do (mirrors Play()'s own check).
    m_ActionGateArms = !clip->ArmsBindPose;
    m_ActionGateWeapon = !clip->WeaponClip.empty();
    m_ActionState = state;
    m_ActionTier = tier;
    return true;
}

bool FirstPersonPresentation::ActionFinished() const {
    if (!m_ArmsModel || !m_WeaponModel) return true;
    const bool armsDone = !m_ActionGateArms || !m_ArmsModel->IsPlayingAnimation();
    const bool weaponDone = !m_ActionGateWeapon || !m_WeaponModel->IsPlayingAnimation();
    return armsDone && weaponDone;
}

bool FirstPersonPresentation::TriggerAction(const std::string& state) {
    if (!IsActive()) return false;
    const FirstPersonActionTier tier = FirstPersonTierOf(state);
    if (tier == FirstPersonActionTier::None) {
        SetError("state '" + state + "' is not a triggerable action");
        return false;
    }
    if (state == "Draw" && m_Equipped) return false;
    if (state == "Holster" && !m_Equipped) return false;
    // A holstered weapon can't fire, reload, inspect, mag-check or melee - only Draw is live.
    if (tier != FirstPersonActionTier::Equip && !m_Equipped) return false;
    if (!FirstPersonCanInterrupt(state, tier, m_ActionState, m_ActionTier)) return false;
    if (!StartAction(state, tier)) return false;
    if (state == "Draw") m_Equipped = true;
    else if (state == "Holster") m_Equipped = false;
    return true;
}

void FirstPersonPresentation::Tick(float planarSpeed, bool sprinting, bool aiming) {
    if (!IsActive()) return;
    if (!m_ActionState.empty()) {
        if (!ActionFinished()) return; // a one-shot or locomotion transition is still holding
        m_ActionState.clear();
        m_ActionTier = FirstPersonActionTier::None;
    }
    const std::string resting = FirstPersonRestingState(planarSpeed, sprinting, aiming);
    if (resting == m_CurrentState) return;
    const std::string via = FirstPersonTransitionVia(m_CurrentState, resting);
    if (!via.empty() && m_Set.Find(via)) {
        StartAction(via, FirstPersonActionTier::Transition);
        return;
    }
    SetState(resting);
}

void FirstPersonPresentation::Update(World& world, const Camera& camera) {
    if (!IsActive() || !world.Registry.valid(m_Arms) || !world.Registry.valid(m_Weapon)) return;
    const glm::quat cameraRotation = CameraRotation(camera);
    // The asset's axis correction is the inner factor (it describes the models' own space), then
    // the scene's View Model Rotation as a tweak on top of that.
    const glm::quat rotation = NormalizeRotation(cameraRotation *
                                                 QuaternionFromEulerYXZ(m_Set.ViewRotation) *
                                                 QuaternionFromEulerYXZ(m_Rotation));

    // Placement is anchored on a rig bone, not on the model's root. These rigs are authored
    // standing in their own scene - feet at y=0, head near y=1.56 - so parking the ROOT on the
    // camera (the old behaviour) stacks that ~1.5 m of authored height on top of the view and
    // leaves the arms and weapon floating overhead. Instead solve for the root that puts the
    // camera bone exactly on the camera: the world point of a local point is
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
    position += cameraRotation * m_Offset;

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
    // which is the whole point of the separate rig. Without this the weapon clip's root is the
    // only thing moving the gun and it barely moves at all (Sprint 14.5 cm, Holster 21.7 cm,
    // Draw 18.2 cm and Aim 67.8 cm of socket-to-weapon error).
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
