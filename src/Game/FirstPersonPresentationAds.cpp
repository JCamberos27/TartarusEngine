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

// FirstPersonPresentation: the sights - the ADS zoom and look scale, actions carried onto the sights (the hold, the hand
// anchor), and the IK rig's offsets written each frame (WriteIK).

using namespace FirstPersonPresentationDetail;

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
    // The anchor's box (this view's and the world body's): the bind vertices, but not those skinned mostly to the spare
    // magazine (or under it).
    {
        const Model& m = *m_WeaponModel;
        std::vector<char> spare(m.BoneCount() > 0 ? m.BoneCount() : 0, 0);
        for (int n = 0; n < m.NodeCount(); ++n)
            for (int a = n; a >= 0; a = m.NodeParent(a))
                if (std::find(m_Set.SpareMagazineBones.begin(), m_Set.SpareMagazineBones.end(), m.NodeName(a)) != m_Set.SpareMagazineBones.end()) {
                    if (const int id = m.BoneId(m.NodeName(n)); id >= 0 && id < (int)spare.size()) spare[id] = 1;
                    break;
                }
        m_WorldGunBoxMin = glm::vec3(1e30f);
        m_WorldGunBoxMax = glm::vec3(-1e30f);
        for (int i = 0; i < m.MeshCount(); ++i)
            for (const ModelMesh::SkinVertex& sv : m.MeshSkinVertices(i)) {
                int best = -1;
                for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
                    if (sv.BoneIDs[k] >= 0 && sv.Weights[k] > 0.0f && (best < 0 || sv.Weights[k] > sv.Weights[best])) best = k;
                if (best >= 0 && sv.BoneIDs[best] < (int)spare.size() && spare[sv.BoneIDs[best]]) continue;
                const glm::vec3 q = glm::vec3(toRoot * glm::vec4(sv.Position, 1.0f));
                m_WorldGunBoxMin = glm::min(m_WorldGunBoxMin, q);
                m_WorldGunBoxMax = glm::max(m_WorldGunBoxMax, q);
            }
        if (m_WorldGunBoxMin.x > m_WorldGunBoxMax.x) { m_WorldGunBoxMin = m_GunBoxMin; m_WorldGunBoxMax = m_GunBoxMax; }
    }
    for (const std::string& bone : anchor.Bones) {
        const int i = m_WeaponModel->NodeIndex(bone);
        if (i < 0) Log::Warn("First-person ADS: the weapon has no '" + bone + "' bone for the hand anchor to carry.");
        else m_AnchorBones.push_back(i);
    }
    // Each carried bone's mesh centre: the bind vertices skinned mostly to it or a bone under it, in its own space. Through
    // each bone's skin offset, as the skin draws them: the Remington's Shell was bound 28 cm from its node's bind place.
    m_AnchorCentres.assign(m_AnchorBones.size(), glm::vec3(0.0f));
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
        const Model& m = *m_WeaponModel;
        // Palette id -> the vertex's place in the carried bone's space: (carried <- bone) * offset.
        std::vector<glm::mat4> toCarried(m.BoneCount() > 0 ? m.BoneCount() : 0, glm::mat4(0.0f));
        std::vector<char> under(toCarried.size(), 0);
        const glm::mat4 carriedInv = glm::inverse(globals[m_AnchorBones[k]]);
        for (int n = 0; n < m.NodeCount(); ++n)
            for (int a = n; a >= 0; a = m.NodeParent(a))
                if (a == m_AnchorBones[k]) {
                    glm::mat4 offset(1.0f);
                    if (const int id = m.BoneId(m.NodeName(n)); id >= 0 && id < (int)under.size() && m.BoneOffset(m.NodeName(n), offset)) {
                        under[id] = 1;
                        toCarried[id] = carriedInv * globals[n] * offset;
                    }
                    break;
                }
        glm::vec3 sum(0.0f);
        int count = 0;
        for (int i = 0; i < m.MeshCount(); ++i)
            for (const ModelMesh::SkinVertex& sv : m.MeshSkinVertices(i)) {
                int best = -1;
                for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
                    if (sv.BoneIDs[j] >= 0 && sv.Weights[j] > 0.0f && (best < 0 || sv.Weights[j] > sv.Weights[best])) best = j;
                if (best >= 0 && sv.BoneIDs[best] < (int)under.size() && under[sv.BoneIDs[best]]) {
                    sum += glm::vec3(toCarried[sv.BoneIDs[best]] * glm::vec4(sv.Position, 1.0f));
                    ++count;
                }
            }
        if (count > 0) m_AnchorCentres[k] = sum / (float)count;
    }
    // ... and where each rests (the bind pose: seated in the gun, or parked in the pouch), in the weapon model's space.
    m_AnchorRest.clear();
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) m_AnchorRest.push_back(glm::vec3(globals[m_AnchorBones[k]] * glm::vec4(m_AnchorCentres[k], 1.0f)));
    if (!m_AnchorBones.empty() && m_World->Registry.valid(m_Weapon)) {
        auto& wrig = m_World->Registry.get_or_emplace<IKRigComponent>(m_Weapon);
        wrig.Offsets.resize(kWeaponAnchorOffset + m_AnchorBones.size()); // slot 0 stays the bolt's
        for (size_t k = 0; k < m_AnchorBones.size(); ++k)
            wrig.Offsets[kWeaponAnchorOffset + k] = IKBoneOffset{m_WeaponModel->NodeName(m_AnchorBones[k])};
    }
    char msg[320];
    std::snprintf(msg, sizeof msg, "First-person ADS: hand anchor on (limb %c), bind bounds (%.3f %.3f %.3f)..(%.3f %.3f %.3f) m, "
                  "anchor box (%.3f %.3f %.3f)..(%.3f %.3f %.3f) m.",
                  m_AnchorLimb == 0 ? 'A' : 'B', m_GunBoxMin.x, m_GunBoxMin.y, m_GunBoxMin.z, m_GunBoxMax.x,
                  m_GunBoxMax.y, m_GunBoxMax.z, m_WorldGunBoxMin.x, m_WorldGunBoxMin.y, m_WorldGunBoxMin.z, m_WorldGunBoxMax.x,
                  m_WorldGunBoxMax.y, m_WorldGunBoxMax.z);
    Log::Info(msg);
}
// A free hand off the gun goes where it is relative to the eye at the hip, not relative to the
// gun (which the sights have moved), and the weapon bones in it (the shell) go with it.
void FirstPersonPresentation::WriteHandAnchor(IKRigComponent& rig, const AdsCarrySample& carry, const glm::quat& adsR,
                                              const glm::vec3& adsT) {
    rig.LimbA.GoalMove = rig.LimbB.GoalMove = glm::mat4(1.0f);
    IKRigComponent* wrig = !m_AnchorBones.empty() && m_World && m_World->Registry.valid(m_Weapon)
                               ? m_World->Registry.try_get<IKRigComponent>(m_Weapon) : nullptr;
    if (wrig && wrig->Offsets.size() < kWeaponAnchorOffset + m_AnchorBones.size()) wrig = nullptr;
    m_AnchorApplied.assign(m_AnchorBones.size(), {glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
    if (wrig)
        for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
            const float rw = std::clamp(wrig->Weight, 0.0f, 1.0f); // as IK::ApplyRig scales them
            m_AnchorApplied[k] = {wrig->Offsets[kWeaponAnchorOffset + k].Position * rw,
                                  glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::normalize(wrig->Offsets[kWeaponAnchorOffset + k].Rotation), rw)};
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
        const float x = std::clamp((AdsBoxDistance(inRoot, m_WorldGunBoxMin, m_WorldGunBoxMax) - anchor.Near) / (anchor.Far - anchor.Near), 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x) * carry.Weight;
    };
    const float w = weightAt(inGun);
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
void FirstPersonPresentation::WriteIK() {
    if (m_World && m_World->Registry.valid(m_Weapon))
        if (auto* bolt = m_World->Registry.try_get<IKRigComponent>(m_Weapon); bolt && !bolt->Offsets.empty())
            bolt->Offsets[0].Position = m_BoltStroke * m_Procedural.Pose().Bolt;
    if (!m_World || !m_World->Registry.valid(m_Arms)) return;
    auto* rig = m_World->Registry.try_get<IKRigComponent>(m_Arms);
    if (!rig) return;
    // On the sights too: walking there layers the walk additively onto the aim pose, and a grip read from that blend let the
    // hands slide on the gun (the .38: right 1 mm, left 2 mm in the gun's own frame; 0 with this).
    rig->PreserveBaseGrip = ActionBlocksSprint() || m_AdsHold > 0.0f;
    const WeaponProceduralPose& p = m_Procedural.Pose();
    const glm::quat C = NormalizeRotation(QuaternionFromEulerYXZ(m_Set.ViewRotation) * QuaternionFromEulerYXZ(m_Rotation));
    const glm::quat Ci = glm::inverse(C);
    rig->SwayEnabled=m_Set.Procedural.Sway.Enabled;
    rig->SwaySettings=m_Set.Procedural.Sway.Unity;
    rig->SwayBeforeOffset=m_UsesIK?kProceduralOffset:0;
    rig->SwayModelToComponent=C;
    rig->SwayModelToWorld=IK::Rotation(m_ArmsWorld);
    rig->SwayMetresPerUnit=m_Scale;
    rig->Weight = p.IKWeight;
    if (!m_UsesIK || (int)rig->Offsets.size()<=kProceduralOffset) return;
    rig->LimbA.CurveWeight = m_IKCurves.RightHand;
    rig->LimbB.CurveWeight = m_IKCurves.LeftHand;
    rig->LookCurveWeight = m_IKCurves.Look;
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
    {   // hand-vs-gun offsets (metres to the arms rig's units)
        const WeaponIKSettings& ik = m_Set.Procedural.IK;
        rig->LimbA.GripPosition = ik.RightHandPosition / m_Scale;
        rig->LimbA.GripRotation = ik.RightHandRotation;
        rig->LimbB.GripPosition = ik.LeftHandPosition / m_Scale;
        rig->LimbB.GripRotation = ik.LeftHandRotation;
    }
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
