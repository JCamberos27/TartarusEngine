#include "FirstPersonPresentation.h"
#include "FirstPersonPresentationInternal.h"
#include "FirstPersonMovementInput.h"
#include "GameRules.h"
#include "LiveReload.h"
#include "WeaponRules.h"
#include "WeaponPrefab.h"

#include "AnimationSystem.h"
#include "Audio/WeaponAudio.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "IK.h"
#include "Log.h"
#include "GameModuleAPI.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ParticleSystem.h"
#include "SceneSerializer.h"
#include "Core/Paths.h"
#include "RotationMath.h"
#include "World.h"
#include "TimeService.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <unordered_map>

// FirstPersonPresentation: the view - placing the rigs on the camera each frame (PlaceRigs: the sight line, the bore ray),
// the view punch and lean on the camera, the corner peek, and the spare magazine.

using namespace FirstPersonPresentationDetail;
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

} // namespace

void FirstPersonPresentation::RemoveViewKick(Camera& camera) {
    if (!m_KickApplied) return;
    camera.Pitch -= m_KickAngles.x;
    camera.Yaw -= m_KickAngles.y;
    camera.Roll -= m_KickAngles.z;
    camera.Position -= m_KickOffset;
    m_KickApplied = false;
    m_KickAngles = m_KickOffset = glm::vec3(0.0f);
}
void FirstPersonPresentation::RefreshViewKick(Camera& camera) {
    if (!IsActive() || !m_KickApplied) return;
    RemoveViewKick(camera);
    // This frame's aim climb, as Update takes it; the look baseline moves with it, so next frame doesn't read the
    // climb as the player's own look.
    const glm::vec2 climb = m_Procedural.ConsumeAimKick();
    const glm::vec2 before(camera.Pitch, camera.Yaw);
    camera.Pitch = std::clamp(camera.Pitch + climb.x, -89.0f, 89.0f);
    camera.Yaw += climb.y;
    m_Procedural.DiscardClampedAim(glm::vec2(climb.x - (camera.Pitch - before.x), 0.0f));
    m_PlayerLookBaseline += glm::vec2(camera.Pitch, camera.Yaw) - before;
    ApplyViewPunch(camera);
}
void FirstPersonPresentation::ApplyViewPunch(Camera& camera) {
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
            // keeps the near plane off the wall; tunable from FirstPersonControllerComponent
            RaycastHit hit;
            QueryFilter filter;
            filter.HitTriggers = 0;
            if (PhysicsWorld::SphereCastFiltered(origin, d, m_EyeRadius, std::fabs(side), filter, hit) && hit.Hit) {
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
// Hides the spare magazine bones unless the left hand holds them (see the header).
void FirstPersonPresentation::UpdateSpareMagazine(const glm::vec3& armsPos, const glm::quat& armsRot,
                                                  const glm::vec3& weaponPos, const glm::quat& weaponRot) {
    m_SpareMagGap = -1.0f; // none this frame unless measured below
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
        const float gap = glm::length(w - handWorld) / scale;
        m_SpareMagGap = m_SpareMagGap < 0.0f ? gap : std::min(m_SpareMagGap, gap);
        if (gap > m_Set.SpareMagazineGrabDistance) hide.push_back(n);
    }
    if (hide == m_SpareMagHidden) return;
    m_SpareMagHidden = hide;
    m_WeaponModel->SetHiddenNodes(hide);
    // The world copy's own model (PoseWorldWeapon re-applies its pose each frame) hides the same: the clip, not the world
    // body's hand, decides whether the hand has it.
    if (m_WorldWeaponModel) m_WorldWeaponModel->SetHiddenNodes(hide);
    // (SetHiddenNodes re-derives the palette of an externally posed model itself: it shows or goes this frame.)
}
// Pins the arms to the camera and the weapon to the arms' gun socket, from the rigs' current
// posed node globals. Update runs it before the animators have posed this frame, so on its own
// the gun rode last frame's hands - a frame behind every clip, sway and bob change, which read as
// the left hand sliding on the handguard. LateUpdate runs it again once they have.
void FirstPersonPresentation::PlaceRigs(World& world, const Camera& camera,const Camera* placementCamera,bool finalPass) {
    const Camera& placement=placementCamera?*placementCamera:camera;
    const glm::quat cameraRotation = CameraRotation(placement);
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
    glm::vec3 position = placement.Position;
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
    position += cameraRotation * m_Set.ViewPosition;

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

    // Align the selected optic's sight axis with the camera while preserving the authored
    // animation's arms/gun relationship. Store the correction in camera space so changing
    // optics while looking around lerps from the previous point without dragging the view.
    glm::mat4 optic(1), referenceOptic(1), animatedRoot(1);
    glm::vec3 opticTarget(0);
    glm::quat opticTurn(1, 0, 0, 0);
    if (m_Attachments.Pose(world, m_WeaponParticles, Gameplay::AttachmentKind::Optic, optic) &&
        m_Attachments.ReferenceOpticPose(world, m_WeaponParticles, referenceOptic) &&
        m_WeaponModel->NodeTransform(m_Set.WeaponRoot.empty() ? "root" : m_Set.WeaponRoot, animatedRoot)) {
        const glm::mat4 weaponPose = glm::translate(glm::mat4(1), weaponPosition) * glm::mat4_cast(weaponRotation) *
                                   glm::scale(glm::mat4(1), glm::vec3(m_Scale));
        const glm::mat4 sight = weaponPose * animatedRoot * m_ParticleBindInverse * optic;
        const glm::mat4 referenceSight = weaponPose * animatedRoot * m_ParticleBindInverse * referenceOptic;
        const glm::quat sightRotation = QuaternionFromMatrix(sight);
        const glm::quat referenceRotation = QuaternionFromMatrix(referenceSight);
        opticTurn = NormalizeRotation(glm::inverse(cameraRotation) * referenceRotation * glm::inverse(sightRotation) * cameraRotation);
        const glm::vec3 sightCamera = glm::inverse(cameraRotation) * (glm::vec3(sight[3]) - camera.Position);
        const glm::vec3 referenceCamera = glm::inverse(cameraRotation) * (glm::vec3(referenceSight[3]) - camera.Position);
        opticTarget = referenceCamera - opticTurn * sightCamera;
    }
    m_OpticOffset = opticTarget;
    m_OpticRotation = opticTurn;
    const glm::quat correction = NormalizeRotation(cameraRotation * glm::slerp(identity, m_OpticRotation, m_AdsHold) * glm::inverse(cameraRotation));
    const glm::vec3 opticShift = cameraRotation * (m_OpticOffset * m_AdsHold);
    position = camera.Position + correction * (position - camera.Position) + opticShift;
    rigRotation = NormalizeRotation(correction * rigRotation);
    weaponPosition = camera.Position + correction * (weaponPosition - camera.Position) + opticShift;
    weaponRotation = NormalizeRotation(correction * weaponRotation);
    world.SetWorldPose(m_Arms, position, rigRotation);
    m_ArmsWorld = glm::translate(glm::mat4(1), position) * glm::mat4_cast(rigRotation) * glm::scale(glm::mat4(1), glm::vec3(m_Scale));
    world.SetWorldPose(m_Weapon, weaponPosition, weaponRotation);
    m_WeaponWorld = glm::translate(glm::mat4(1.0f), weaponPosition) * glm::mat4_cast(weaponRotation) *
                    glm::scale(glm::mat4(1.0f), glm::vec3(m_Scale));
    world.Registry.get<TransformComponent>(m_Weapon).Scale = glm::vec3(m_Scale);
    UpdateSpareMagazine(position, rigRotation, weaponPosition, weaponRotation);

    // Where the barrel points: down the bore from the muzzle to the first thing it meets. (Once a frame: Update's early
    // placement measured the sight line too - it settled at twice the rate - and cast a second 300 m ray nothing read.)
    if (finalPass) {
        m_AimPointValid = false;
        m_AimHit = false;
    }
    glm::mat4 rootPose(1.0f);
    m_RootSeenValid = false;
    if (m_WeaponModel->NodeTransform(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot, rootPose)) {
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
        m_RootWorld = W;
        if (world.Registry.valid(m_WeaponParticles)) {
            const glm::mat4 particlePose = rootPose * m_ParticleBindInverse;
            auto& t = world.Registry.get<TransformComponent>(m_WeaponParticles);
            t.Position = glm::vec3(particlePose[3]);
            const glm::vec3 scale(glm::length(glm::vec3(particlePose[0])), glm::length(glm::vec3(particlePose[1])), glm::length(glm::vec3(particlePose[2])));
            t.Scale = scale;
            t.SetRotationQuaternion(glm::quat_cast(glm::mat3(glm::vec3(particlePose[0]) / std::max(scale.x, 1e-6f), glm::vec3(particlePose[1]) / std::max(scale.y, 1e-6f), glm::vec3(particlePose[2]) / std::max(scale.z, 1e-6f))));
        }
        m_RootSeen = seen * W;
        m_RootSeenValid = true;
    }
    if (finalPass && m_HaveMuzzle && m_RootSeenValid) {
        const glm::mat4 W = m_RootWorld, SW = m_RootSeen;
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
