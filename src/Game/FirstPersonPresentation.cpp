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
    }
    m_Offset = config.ViewModelOffset;
    m_Rotation = config.ViewModelRotation;
    m_Scale = config.ViewModelScale;

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
    const glm::quat rotation = NormalizeRotation(cameraRotation * QuaternionFromEulerYXZ(m_Rotation));
    // ViewModelOffset is expressed in the camera's local frame. Its default negative Z moves the
    // model in front of a camera whose local forward is -Z.
    const glm::vec3 position = camera.Position + cameraRotation * m_Offset;
    for (entt::entity e : {m_Arms, m_Weapon}) {
        world.SetWorldPose(e, position, rotation);
        world.Registry.get<TransformComponent>(e).Scale = glm::vec3(m_Scale);
    }
}
