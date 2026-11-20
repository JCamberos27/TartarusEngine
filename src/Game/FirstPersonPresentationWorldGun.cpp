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

// FirstPersonPresentation: the gun as every other view sees it - the world copy, its placement off the first-person gun
// (the stock lock's input, the third-person hold), and the parts the free hand carries.

using namespace FirstPersonPresentationDetail;

namespace {

// A carried part in the free hand (the first-person clips'): its centre within kCarriedHeld of the hand - let go past
// kCarriedLoose (a magazine held by its base has its centre 10-15 cm from the hand bone) - and off its resting place
// (seated in the gun, parked in the pouch) by kCarriedOff: the hand resting on the handguard is as close to the seated
// magazine as one holding it.
constexpr float kCarriedHeld = 0.18f, kCarriedLoose = 0.24f;
constexpr float kCarriedRest = 0.01f, kCarriedOff = 0.04f;

} // namespace

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
glm::mat4 FirstPersonPresentation::ThirdPersonGunCorrection() const {
    if (!m_ArmsClip1PModel || !m_ArmsClip3PModel || m_ArmsClip1PModel->AppliedLocalPose().empty() ||
        m_ArmsClip3PModel->AppliedLocalPose().empty())
        return glm::mat4(1.0f);
    const std::string& gun = m_Set.WeaponSocket.empty() ? m_Set.Procedural.IK.GunBone : m_Set.WeaponSocket;
    glm::mat4 first(1.0f), third(1.0f);
    if (gun.empty() || !m_ArmsClip1PModel->NodeTransform(gun, first) || !m_ArmsClip3PModel->NodeTransform(gun, third))
        return glm::mat4(1.0f);
    return third * glm::inverse(first);
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
    // A reload, inspect, mag check or melee from the hip: the world body keeps the gun level with its chest (see ArmsLateUpdate).
    out.Action = HasTag("Reload") || HasTag("Busy") ? 1.0f - z * z * (3.0f - 2.0f * z) : 0.0f;
    out.Pocket = sl.Pocket;
    out.MaxShift = sl.MaxShift;
    out.HeadTiltDegrees = sl.HeadTilt;
    out.NeckRadius = sl.NeckRadius;
    out.HeadRadius = sl.HeadRadius;
    out.GunLength = sl.GunLength;
    out.MeshClearance = sl.MeshClearance;
    return true;
}
void FirstPersonPresentation::PlaceWorldWeapon(World& world, bool split, const glm::mat4& worldFromFirstPerson,
                                               const glm::mat4* holdingHand) {
    auto& reg = world.Registry;
    // The free hand's grip at rest (weapon space), for HandAnchor's Off.
    if (const std::string& st = CurrentState(); !HoldsOnBody() && (st == "Idle" || st == "Walk" || st == "Aim"))
        if (glm::mat4 h(1.0f); ClipHandInWeapon(h)) {
            m_RestGrip = m_HaveRestGrip ? m_RestGrip + (glm::vec3(h[3]) - m_RestGrip) * 0.2f : glm::vec3(h[3]);
            m_HaveRestGrip = true;
        }
    const bool haveWeapon = m_Weapon != entt::null && reg.valid(m_Weapon);
    if (!split || !haveWeapon) {
        if (m_WorldWeapon != entt::null && reg.valid(m_WorldWeapon)) world.DestroyEntityAndChildren(m_WorldWeapon);
        m_WorldWeapon = entt::null;
        m_WorldWeaponModel.reset();
        m_WorldWeaponParticles = entt::null; m_WorldWeaponEmitters.clear();
        for (auto e : m_WeaponAttachmentEntities) if (reg.valid(e)) reg.remove<OwnerViewOnlyTag>(e);
        if (haveWeapon) reg.remove<OwnerViewOnlyTag>(m_Weapon);
        m_WorldWeaponDelta = glm::mat4(1.0f);
        return;
    }
    if (m_WorldWeapon == entt::null || !reg.valid(m_WorldWeapon)) {
        // The same model object as the first-person gun: one pose (the pump, the shells), drawn twice. With a hand anchor
        // its own model, posed from the first-person one's (PoseWorldWeapon): the bones the free hand carries differ.
        const RenderableComponent src = reg.get<RenderableComponent>(m_Weapon); // by value: the create below can move the storage
        std::shared_ptr<Model> model = src.ModelRef;
        m_WorldWeaponModel.reset();
        if (!m_AnchorBones.empty() && m_Assets && !m_Set.WeaponModel.empty())
            if (std::shared_ptr<Model> own = m_Assets->InstantiateModel(ProjectPaths::Resolve(m_Set.WeaponModel))) {
                model = m_WorldWeaponModel = own;
                own->SetHiddenNodes(m_SpareMagHidden); // the spare magazine / shell out of the hand (UpdateSpareMagazine)
            }
        m_WorldWeapon = world.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(m_Scale),
                                                "[Runtime] First Person Weapon (world)");
        auto& dst = reg.get<RenderableComponent>(m_WorldWeapon);
        dst.Materials = src.Materials;
        dst.CastShadows = RenderableComponent::ShadowCasting::On;
        dst.ReceiveShadows = true;
        reg.emplace_or_replace<HiddenFromOwnerTag>(m_WorldWeapon);
        reg.emplace_or_replace<OwnerViewOnlyTag>(m_Weapon);
        for (auto e : m_WeaponAttachmentEntities) if (reg.valid(e)) reg.emplace_or_replace<OwnerViewOnlyTag>(e);
        SetupWorldWeaponParticles();
    }
    const glm::mat4 placed = worldFromFirstPerson * m_WeaponWorld;
    const glm::vec3 scale(glm::length(glm::vec3(placed[0])), glm::length(glm::vec3(placed[1])), glm::length(glm::vec3(placed[2])));
    const glm::mat3 r(glm::vec3(placed[0]) / std::max(scale.x, 1e-6f), glm::vec3(placed[1]) / std::max(scale.y, 1e-6f),
                      glm::vec3(placed[2]) / std::max(scale.z, 1e-6f));
    world.SetWorldPose(m_WorldWeapon, glm::vec3(placed[3]), glm::normalize(glm::quat_cast(r)));
    m_WorldWeaponDelta = worldFromFirstPerson;
    reg.get<TransformComponent>(m_WorldWeapon).Scale = scale;
    if (m_WorldWeaponModel) PoseWorldWeapon(world, holdingHand);
    if (reg.valid(m_WeaponParticles) && reg.valid(m_WorldWeaponParticles))
        reg.get<TransformComponent>(m_WorldWeaponParticles) = reg.get<TransformComponent>(m_WeaponParticles);
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
// The world copy's own pose: the first-person gun's, except the bones the free hand carries. Those leave the sights'
// anchor (WriteHandAnchor's, held to the eye: the world body's hand isn't), and one in the free hand (CarriedHeld) takes
// the hand's move onto the body exactly as the hand does - the same share, about the hand - so the two go as one: no
// sliding between them, no wrist in the magazine. One in the gun, or let go (the AK's old magazine), stays where the clip
// has it on the gun.
void FirstPersonPresentation::PoseWorldWeapon(World& world, const glm::mat4* holdingHand) {
    if (!m_WeaponModel || !m_WorldWeaponModel) return;
    const std::vector<LocalTRS>& src = m_WeaponModel->AppliedLocalPose();
    Model& dst = *m_WorldWeaponModel;
    if ((int)src.size() != dst.NodeCount()) return; // the first-person gun not posed yet: the bind pose stands
    thread_local std::vector<LocalTRS> pose;
    thread_local std::vector<glm::mat4> bones, targets;
    thread_local std::vector<float> held;
    pose = src;
    if (!CarriedBonePoses(bones)) return;
    const bool haveHeld = CarriedHeldFrom(bones, held) && held.size() == bones.size(); // (the poses just taken: not again)
    const glm::mat4 toModel = glm::inverse(world.ComposeWorldTransform(m_WorldWeapon));
    targets.assign(m_AnchorBones.size(), glm::mat4(1.0f));
    // With the world hand as solved: a part in the hand is held as the first-person clips hold it (its place off their hand),
    // on the world hand - wherever the third-person clips have taken that hand (a stow re-timed, a reach the arm can't
    // make). Taking only the hand anchor's move, it left the clips' hand path only as far as the anchor did: re-timed, the
    // AK's old magazine hung in the air under the gun.
    glm::mat4 clipHand(1.0f), heldOn(1.0f);
    const bool onHand = haveHeld && holdingHand && ClipHandInWeapon(clipHand);
    if (onHand) {
        auto rigid = [](const glm::mat4& m) {
            glm::mat4 r(1.0f);
            for (int c = 0; c < 3; ++c) r[c] = glm::vec4(glm::normalize(glm::vec3(m[c])), 0.0f);
            r[3] = m[3];
            return r;
        };
        heldOn = rigid(toModel * *holdingHand) * glm::inverse(rigid(clipHand)); // the clips' hand onto the world one (model space)
    }
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
        const float wb = onHand ? std::clamp(held[k], 0.0f, 1.0f) : 0.0f;
        targets[k] = wb > 0.0f ? FirstPersonPartialMove(heldOn, bones[k], wb) : bones[k];
    }
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) dst.SetPoseNodeModelSpace(pose, m_AnchorBones[k], targets[k]);
    dst.ApplyLocalPose(pose);
}
bool FirstPersonPresentation::HandAnchor(FirstPersonHandAnchor& out) const {
    const auto* rig = m_AnchorLimb >= 0 && m_World && m_World->Registry.valid(m_Arms) ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    if (!rig) return false;
    const IKLimb& limb = m_AnchorLimb == 0 ? rig->LimbA : rig->LimbB;
    out.Hand = limb.End;
    out.Socket = limb.Target;
    out.Mount = glm::translate(glm::mat4(1.0f), m_Set.WeaponMountOffset) * glm::mat4_cast(QuaternionFromEulerYXZ(m_Set.WeaponMountRotation));
    out.BoxMin = m_WorldGunBoxMin;
    out.BoxMax = m_WorldGunBoxMax;
    out.Near = m_Set.Ads.Anchor.Near;
    out.Far = m_Set.Ads.Anchor.Far;
    out.Active = HoldsOnBody();
    // Off its grip from 3 cm, all of it from 10: the world gun's levelling (FirstPersonBody) comes back as the hand does,
    // not when the state ends - looking up or down, the gun swung 20 degrees back to the aim after the reload was over.
    out.Off = 1.0f;
    if (glm::mat4 h(1.0f); m_HaveRestGrip && ClipHandInWeapon(h)) {
        const float t = std::clamp((glm::length(glm::vec3(h[3]) - m_RestGrip) * glm::length(glm::vec3(m_WeaponWorld[0])) - 0.03f) / 0.07f, 0.0f, 1.0f);
        out.Off = t * t * (3.0f - 2.0f * t);
    }
    return true;
}
bool FirstPersonPresentation::ClipHandInWeapon(glm::mat4& out) const {
    const auto* rig = m_AnchorLimb >= 0 && m_World && m_World->Registry.valid(m_Arms) ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    if (!rig || !m_ArmsModel) return false;
    const IKLimb& limb = m_AnchorLimb == 0 ? rig->LimbA : rig->LimbB;
    const Model* clips = m_ArmsClip1PModel && !m_ArmsClip1PModel->AppliedLocalPose().empty() ? m_ArmsClip1PModel.get() : m_ArmsModel.get();
    glm::mat4 socket(1.0f), live(1.0f), hand(1.0f);
    if (!clips->NodeTransform(limb.Target, socket) || !clips->NodeTransform(limb.End, hand) || !m_ArmsModel->NodeTransform(limb.Target, live))
        return false;
    const glm::mat4 gunOnSocket = glm::inverse(m_ArmsWorld * live) * m_WeaponWorld;
    out = glm::inverse(gunOnSocket) * glm::inverse(socket) * hand;
    return true;
}
bool FirstPersonPresentation::CarriedHeld(std::vector<float>& held, float* heldGap) const {
    thread_local std::vector<glm::mat4> bones;
    held.clear();
    if (heldGap) *heldGap = -1.0f;
    return CarriedBonePoses(bones) && CarriedHeldFrom(bones, held, heldGap);
}

bool FirstPersonPresentation::CarriedHeldFrom(const std::vector<glm::mat4>& bones, std::vector<float>& held, float* heldGap) const {
    held.clear();
    if (heldGap) *heldGap = -1.0f;
    const auto* rig = m_AnchorLimb >= 0 && m_World && m_World->Registry.valid(m_Arms) ? m_World->Registry.try_get<IKRigComponent>(m_Arms) : nullptr;
    if (!rig || !m_ArmsModel) return false;
    // The clips' own hand on their gun (the raw "arms" rig: the arms rig's own has the sights' anchor on it, held to the
    // eye), put in the weapon model's space through where the gun really sits on the socket.
    const IKLimb& limb = m_AnchorLimb == 0 ? rig->LimbA : rig->LimbB;
    const Model* clips = m_ArmsClip1PModel && !m_ArmsClip1PModel->AppliedLocalPose().empty() ? m_ArmsClip1PModel.get() : m_ArmsModel.get();
    glm::mat4 socket(1.0f), live(1.0f), hand(1.0f);
    if (!clips->NodeTransform(limb.Target, socket) || !clips->NodeTransform(limb.End, hand) || !m_ArmsModel->NodeTransform(limb.Target, live))
        return false;
    const glm::mat4 gunOnSocket = glm::inverse(m_ArmsWorld * live) * m_WeaponWorld;
    const glm::vec3 handInGun = glm::vec3(glm::inverse(gunOnSocket) * glm::inverse(socket) * hand[3]);
    const float toMetres = glm::length(glm::vec3(m_WeaponWorld[0]));
    float most = -1.0f;
    for (size_t k = 0; k < bones.size(); ++k) {
        const glm::vec3 centre = glm::vec3(bones[k] * glm::vec4(k < m_AnchorCentres.size() ? m_AnchorCentres[k] : glm::vec3(0.0f), 1.0f));
        const float gap = glm::length(centre - handInGun) * toMetres;
        const float off = k < m_AnchorRest.size() ? glm::length(centre - m_AnchorRest[k]) * toMetres : 1.0f;
        const float h = (1.0f - FirstPersonAnchorWeight(gap, kCarriedHeld, kCarriedLoose)) * FirstPersonAnchorWeight(off, kCarriedRest, kCarriedOff);
        held.push_back(h);
        if (h > most) {
            most = h;
            if (heldGap) *heldGap = gap;
        }
    }
    return !held.empty();
}
// The carried bones' model-space poses on the first-person gun, the sights' anchor (WriteHandAnchor, held to the eye) undone.
bool FirstPersonPresentation::CarriedBonePoses(std::vector<glm::mat4>& out) const {
    out.clear();
    if (!m_WeaponModel) return false;
    const std::vector<LocalTRS>& src = m_WeaponModel->AppliedLocalPose();
    if ((int)src.size() != m_WeaponModel->NodeCount()) return false;
    thread_local std::vector<glm::mat4> globals;
    thread_local std::vector<int> parents;
    parents.resize(m_WeaponModel->NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
    IK::ComputeGlobals(src, parents, globals);
    for (size_t k = 0; k < m_AnchorBones.size(); ++k) {
        glm::mat4 bone = globals[m_AnchorBones[k]];
        if (k < m_AnchorApplied.size()) {
            // IK::ApplyRig's offset (OffsetBone about the bone's own place before it: x -> R (x - c) + c + P), undone.
            const auto& [P, R] = m_AnchorApplied[k];
            const glm::vec3 c = glm::vec3(bone[3]) - P;
            bone = glm::translate(glm::mat4(1.0f), c) * glm::mat4_cast(glm::inverse(R)) * glm::translate(glm::mat4(1.0f), -(c + P)) * bone;
        }
        out.push_back(bone);
    }
    return true;
}
bool FirstPersonPresentation::MuzzleFrames(glm::vec3& firstPerson, glm::vec3& worldCopy, glm::vec3& bore) const {
    if (!m_HaveMuzzle || !m_RootSeenValid) return false;
    firstPerson = glm::vec3(m_RootWorld * glm::vec4(m_MuzzleLocal, 1.0f));
    worldCopy = glm::vec3(m_WorldWeaponDelta * glm::vec4(firstPerson, 1.0f));
    const glm::vec3 b = glm::mat3(m_RootWorld) * m_BoreLocal;
    if (glm::dot(b, b) < 1e-12f) return false;
    bore = glm::normalize(b);
    return true;
}
