#include "FirstPersonBody.h"
#include "FirstPersonBodyInternal.h"
#include "Profiler.h"
#include "BodyDebugDraw.h"
#include "AnimatorController.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonAnimation.h" // FirstPersonAnimatorContract::kTagHidden: the arms rig hidden (unarmed)

#include "Camera.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "OutfitSystem.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "SkinHideBuffer.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <tuple>
#include <cstdio>
#include <sstream>

// FirstPersonBody: the arms - the pieces' (first person) and the world twins' onto a gun's hold (ArmsLateUpdate), the
// arm shapes copied off the rig, the twist bones, and the hands held off the eye.

using namespace FirstPersonBodyDetail;

namespace {

// The rig's arm shapes onto `pose` (rotations only, every node under the clavicles: the body keeps
// its own bone lengths), blended by `weight` - the clavicles themselves by `weight * clavicleWeight`
// (Clavicle Follow: the rig's collarbones swing freely, with no torso or head in their way).
void CopyArmShape(const Model& m, const Model& rig, float weight, IK::Pose& pose, const std::vector<int>& parents,
                  const std::map<std::string, std::string>& boneMap, float clavicleWeight) {
    const IK::Pose& rigPose = rig.AppliedLocalPose();
    if ((int)rigPose.size() != rig.NodeCount()) return;
    std::vector<char> under(pose.size(), 0), clavicle(pose.size(), 0);
    for (const char* name : {FPBody::kBoneClavicle[0], FPBody::kBoneClavicle[1]})
        if (const int c = m.NodeIndex(FPBody::MappedBone(boneMap, name)); c >= 0) under[c] = clavicle[c] = 1;
    const float clavWeight = weight * std::clamp(clavicleWeight, 0.0f, 1.0f);
    for (int i = 0; i < (int)pose.size(); ++i) {
        if (!under[i] && parents[i] >= 0 && under[parents[i]]) under[i] = 1;
        if (!under[i]) continue;
        const int r = rig.NodeIndex(m.NodeName(i));
        if (r >= 0) pose[i].R = glm::normalize(glm::slerp(pose[i].R, rigPose[r].R, clavicle[i] ? clavWeight : weight));
    }
}

} // namespace

void FirstPersonBody::CopyArmShapeCached(const Model& m, const Model& rig, float weight, std::vector<LocalTRS>& pose,
                                         const std::vector<int>& parents, float clavicleWeight) {
    if (pose.size() != parents.size() || (int)pose.size() != m.NodeCount()) {
        CopyArmShape(m, rig, weight, pose, parents, m_BoneMap, clavicleWeight);
        return;
    }
    ArmShapeCache* entry = nullptr;
    for (ArmShapeCache& e : m_ArmShapeCache)
        if (e.Body == &m && e.Rig == &rig && e.BodyNodes == m.NodeCount() && e.RigNodes == rig.NodeCount()) { entry = &e; break; }
    if (!entry) {
        // The same pairs CopyArmShape finds by name each call (the clavicles through the Bone Map).
        const int clavicles[2] = {m.NodeIndex(FPBody::MappedBone(m_BoneMap, FPBody::kBoneClavicle[0])),
                                  m.NodeIndex(FPBody::MappedBone(m_BoneMap, FPBody::kBoneClavicle[1]))};
        m_ArmShapeCache.push_back({&m, &rig, m.NodeCount(), rig.NodeCount(), {}});
        entry = &m_ArmShapeCache.back();
        FirstPersonBodyArmShapeLinksFrom(m.NodeCount(), parents, clavicles, [&](int i) { return rig.NodeIndex(m.NodeName(i)); }, entry->Links);
    }
    FirstPersonBodyCopyArmShape(entry->Links, rig, weight, pose, clavicleWeight);
}
bool FirstPersonBody::IsArmsPiece(const entt::registry& reg, entt::entity e) const {
    if (!reg.valid(m_Body) || !reg.valid(e)) return false;
    const auto* cfg = reg.try_get<FirstPersonBodyComponent>(m_Body);
    const auto* name = reg.try_get<NameComponent>(e);
    return cfg && name && FPBody::IsArmsPiece(*cfg, name->Name);
}
void FirstPersonBody::CaptureArms(std::vector<ArmHangPose>& out) const {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    out.assign(m_Models.size(), ArmHangPose{});
    for (size_t k = 0; k < m_Models.size(); ++k) {
        if (!m_Models[k]) continue;
        const Model& m = *m_Models[k];
        const IK::Pose& pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        for (int s = 0; s < 2; ++s)
            if (const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s])); u >= 0) {
                out[k].Rot[s] = IK::Rotation(globals[(size_t)u]);
                out[k].Have[s] = true;
            }
    }
}
void FirstPersonBody::HangArms(const std::vector<ArmHangPose>& saved, float weight) {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    for (size_t k = 0; k < m_Models.size() && k < saved.size(); ++k) {
        if (!m_Models[k] || !(saved[k].Have[0] || saved[k].Have[1])) continue;
        Model& m = *m_Models[k];
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        bool changed = false;
        for (int s = 0; s < 2; ++s) {
            const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s]));
            if (u < 0 || !saved[k].Have[s] || parents[(size_t)u] < 0) continue;
            const glm::quat now = IK::Rotation(globals[(size_t)u]);
            const glm::quat want = glm::slerp(now, saved[k].Rot[s], std::clamp(weight, 0.0f, 1.0f));
            const glm::quat parent = IK::Rotation(globals[(size_t)parents[(size_t)u]]);
            pose[(size_t)u].R = glm::normalize(glm::inverse(parent) * want);
            changed = true;
        }
        if (changed) m.ApplyLocalPose(pose);
    }
}
float FirstPersonBody::ArmDownDeg() const {
    const Model* m = nullptr;
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k)
        if (m_Pieces[k] == m_Driver) m = m_Models[k].get();
    if (!m) return 0.0f;
    float worst = 0.0f;
    for (int s = 0; s < 2; ++s) {
        glm::mat4 u(1.0f), l(1.0f);
        if (!m->NodeTransform(Bone(FPBody::kBoneUpperArm[s]), u) || !m->NodeTransform(Bone(FPBody::kBoneLowerArm[s]), l)) continue;
        const glm::vec3 d = glm::vec3(l[3]) - glm::vec3(u[3]);
        if (glm::length(d) < 1e-5f) continue;
        worst = std::max(worst, glm::degrees(std::acos(std::clamp(-glm::normalize(d).y, -1.0f, 1.0f))));
    }
    return worst;
}
// The arms' twist bones after an arm solve: each forearm's twist bones carry the hand's roll by how far down the forearm
// they sit, each upper arm's undo its roll toward the shoulder (IK::SpreadTwist). Their skin then turns along the limb
// instead of wringing at the wrist and the shoulder.
bool FirstPersonArmTwist::Apply(Model& m, float* roll, float* residual) {
    auto it = m_Rigs.find(&m);
    if (it == m_Rigs.end() || it->second.Nodes != m.NodeCount()) {
        Rig r;
        r.Nodes = m.NodeCount();
        m.BindLocalPose(r.Bind);
        std::vector<int> parents(m.NodeCount());
        std::vector<std::string> names(m.NodeCount());
        for (int i = 0; i < m.NodeCount(); ++i) { parents[i] = m.NodeParent(i); names[i] = m.NodeName(i); }
        for (int s = 0; s < 2; ++s) {
            r.Upper[s] = m.NodeIndex(FPBody::kBoneUpperArm[s]);
            r.Lower[s] = m.NodeIndex(FPBody::kBoneLowerArm[s]);
            r.Hand[s] = m.NodeIndex(FPBody::kBoneHand[s]);
            if (r.Upper[s] < 0 || r.Lower[s] < 0 || r.Hand[s] < 0) continue;
            r.UpperTwist[s] = IK::FindTwistBones(r.Bind, parents, names, r.Upper[s], r.Lower[s]);
            r.LowerTwist[s] = IK::FindTwistBones(r.Bind, parents, names, r.Lower[s], r.Hand[s]);
        }
        it = m_Rigs.insert_or_assign(&m, std::move(r)).first;
    }
    const Rig& r = it->second;
    thread_local IK::Pose pose; // (scratch kept between frames: no per-frame allocation)
    pose = m.AppliedLocalPose();
    if ((int)pose.size() != m.NodeCount() || pose.size() != r.Bind.size()) return false;
    bool any = false;
    for (int s = 0; s < 2; ++s) {
        if (r.Upper[s] < 0 || r.Lower[s] < 0 || r.Hand[s] < 0) continue;
        if (!r.LowerTwist[s].empty()) {
            const float turn = IK::EndRoll(pose, r.Bind, r.Lower[s], r.Hand[s]);
            IK::SpreadTwist(pose, r.Bind, r.Lower[s], r.Hand[s], r.LowerTwist[s], turn, true);
            float most = 0.0f;
            for (const IK::TwistBone& t : r.LowerTwist[s]) most = std::max(most, t.Share);
            if (roll) roll[s] = glm::degrees(turn);
            if (residual) residual[s] = glm::degrees(turn * (1.0f - most));
            any = true;
        }
        if (!r.UpperTwist[s].empty()) {
            IK::SpreadTwist(pose, r.Bind, r.Upper[s], r.Lower[s], r.UpperTwist[s], IK::SegmentRoll(pose, r.Bind, r.Upper[s], r.Lower[s]), false);
            any = true;
        }
    }
    if (any) m.ApplyLocalPose(pose);
    return any;
}
void FirstPersonBody::ArmTwist(const std::vector<std::shared_ptr<Model>>& models) {
    PROFILE_SCOPE("FPB arm twist");
    for (const auto& mp : models)
        if (mp) m_ArmTwist.Apply(*mp);
}
float FirstPersonAnchorWeight(float distance, float nearDist, float farDist) {
    if (farDist <= nearDist) return distance > nearDist ? 1.0f : 0.0f;
    const float x = std::clamp((distance - nearDist) / (farDist - nearDist), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
glm::mat4 FirstPersonPartialMove(const glm::mat4& move, const glm::mat4& m, float weight) {
    const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    const glm::quat turn = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), IK::Rotation(move), weight);
    const glm::vec3 from(m[3]), to(move * m[3]);
    glm::mat4 out = glm::translate(glm::mat4(1.0f), glm::mix(from, to, weight)) * glm::mat4_cast(glm::normalize(turn * IK::Rotation(m)));
    for (int c = 0; c < 3; ++c) out[c] *= scale[c];
    return out;
}
void FirstPersonBody::ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt, const Camera* camera,
                                     const glm::mat4& thirdPersonGun, const Model* thirdPersonRig, const std::string& gunSocket) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) return;
    const FirstPersonBodyComponent& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Spine = cfg.Spine;
    BodyDebug::Info().TwinHandGap[0] = BodyDebug::Info().TwinHandGap[1] = -1.0f; // (none this frame until the twins' solve)
    // The world twins start from the pieces' pose as it stands now: the same clips, spine passes and feet - one body. From
    // here both get their arms onto a gun: the pieces onto the rig's hands, the twins onto the world gun's hold.
    {
    PROFILE_SCOPE("FPB sync twins");
    SyncTwins(world);
    }
    const bool enabled = cfg.WeaponArms;
    const bool haveRig = enabled && weaponArms != entt::null && reg.valid(weaponArms) &&
                         reg.all_of<RenderableComponent>(weaponArms) && viewModelFov > 0.0f;
    // The rig keeps posing (its hands are the targets) but is neither drawn nor casts a shadow: the
    // body's arms, which follow it, are what is seen and what shadows.
    if (haveRig) {
        if (!reg.all_of<PoseSourceTag>(weaponArms)) reg.emplace<PoseSourceTag>(weaponArms);
        m_PoseSource = weaponArms;
    } else if (m_PoseSource != entt::null) {
        if (reg.valid(m_PoseSource)) reg.remove<PoseSourceTag>(m_PoseSource); // Weapon Arms off: it is the visible arms again
        m_PoseSource = entt::null;
    }
    // Holstered, the rig is inactive: the arms ease back to the locomotion clips' pose, but out of the
    // view-model pass at once (there they would show as a hand at the bottom of the view).
    const bool follow = haveRig && !reg.all_of<InactiveTag>(weaponArms);
    // Drawn, the arms take the rig's hands from the first frame: eased in, the draw (and Play's first
    // frames) would show the hands out of the idle pose while the gun is already up. Only off eases.
    // LateUpdate already set this frame's weight (so the eye used it, not last frame's): re-decided here from where it
    // started the frame, with this rig's own state - the same unless LateUpdate's guess at it was off.
    m_ViewModelFovOk = viewModelFov > 0.0f;
    if (m_ArmsWeightEased) m_ArmsWeight = m_ArmsWeightPrev;
    m_ArmsWeightEased = false;
    if (follow) m_ArmsWeight = 1.0f;
    else m_ArmsWeight -= m_ArmsWeight * Follow(dt, cfg.ArmsEaseOut);
    const bool viewModelArms = follow;
    // Easing off a holstered gun (a weapon swap's holster to draw, or going unarmed) the arms piece is hidden
    // from the camera, below.
    const bool armsHidden = haveRig && !follow && m_ArmsWeight > 1e-3f;
    // Clothing's sleeves go with the arms (PlayerBodyTag::SleeveBones): into the view-model pass, or hidden with
    // them - left in view, an empty sleeve hung where the arm had been.
    for (entt::entity e : m_Pieces)
        if (auto* tag = reg.valid(e) ? reg.try_get<PlayerBodyTag>(e) : nullptr) {
            tag->SleevesInViewModel = viewModelArms && tag->HasSleeves;
            tag->SleevesHidden = armsHidden && tag->HasSleeves;
        }
    // The body's arms piece goes into the view-model pass with the gun (its hands then sit where the
    // rig's do); off, it is an ordinary piece of the body again.
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (e == m_Body || !reg.valid(e) || !IsArmsPiece(reg, e)) continue;
        if (viewModelArms) {
            if (!reg.all_of<ViewModelTag>(e)) {
                reg.emplace<ViewModelTag>(e);
                // (Once: every draw re-adds the tag, and Stop only needs each piece once.)
                if (std::find(m_ArmsTagged.begin(), m_ArmsTagged.end(), e) == m_ArmsTagged.end()) m_ArmsTagged.push_back(e);
            }
        } else if (reg.all_of<ViewModelTag>(e)) {
            reg.remove<ViewModelTag>(e);
        }
        // Easing off a holstered gun the pose is still the rig's (its hands, no gun to hold): shown, it
        // would be a pair of hands hanging in the view for a few frames. Hidden until it has settled.
        auto& rc = reg.get<RenderableComponent>(e);
        // Per piece: the arms and the hands may both be pieces, each with its own setting to go back to.
        auto saved = std::find_if(m_ArmsEasedOut.begin(), m_ArmsEasedOut.end(), [e](const auto& s) { return s.first == e; });
        if (armsHidden) {
            if (saved == m_ArmsEasedOut.end()) m_ArmsEasedOut.emplace_back(e, (int)rc.CastShadows);
            rc.CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
        } else if (saved != m_ArmsEasedOut.end()) {
            rc.CastShadows = (RenderableComponent::ShadowCasting)saved->second;
            m_ArmsEasedOut.erase(saved);
        }
    }
    if (m_ArmsWeight < 1e-3f || !haveRig) {
        m_HaveShoulderAnchor[0] = m_HaveShoulderAnchor[1] = false;
        m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
        m_WorldHaveShoulderAnchor[0] = m_WorldHaveShoulderAnchor[1] = false;
        m_WorldHaveElbowAim[0] = m_WorldHaveElbowAim[1] = false;
        m_WorldGunDelta = glm::mat4(1.0f);
        if (!m_Twins.empty()) PlaceHeadAttachedTwins(world);
        // (Unarmed too: what the camera sees of the body doesn't need the rig.)
        if (camera && (BodyDebug::Enabled() || BodyDebug::ProbeRequested())) CameraProbe(world, *camera, viewModelFov, dt);
        if (camera && BodyDebug::ProbeRequested()) InsideProbe(world, *camera);
        return;
    }

    const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
    if (!rig || rig->AppliedLocalPose().empty()) return;
    const IK::Pose& rigPose = rig->AppliedLocalPose();
    if ((int)rigPose.size() != rig->NodeCount()) return;
    const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);
    // The frame the shoulders are held in: the chest's view (LateUpdate), else the rig's.
    const glm::mat4 anchorFrame = m_HaveChestView ? m_ChestView : rigWorld;
    const glm::mat4 anchorInverse = glm::inverse(anchorFrame);
    // Arm Steadiness: the rig's arms are fixed to the view, but the body's shoulders and chest sway with
    // the gait. With the hands pinned to the rig's, that sway came out at the elbows (the support arm,
    // nearly straight, most of all). Steady, each shoulder is held at a slow average of where it sits
    // relative to the eye (in the chest's view frame), and each elbow bends in the plane the rig's does.
    const float steady = std::clamp(cfg.ArmSteadiness, 0.0f, 1.0f) * m_ArmsWeight;

    struct Side { const char *Clavicle, *Upper, *Lower, *Hand; };
    static const Side kSides[2] = {{FPBody::kBoneClavicle[0], FPBody::kBoneUpperArm[0], FPBody::kBoneLowerArm[0], FPBody::kBoneHand[0]},
                                   {FPBody::kBoneClavicle[1], FPBody::kBoneUpperArm[1], FPBody::kBoneLowerArm[1], FPBody::kBoneHand[1]}};
    float gripInset = 0.0f, fingerCurl = 0.0f; // the body's settings plus the worn race's (a skeleton's)
    OutfitSystem::RaceGrip(world, m_Body, gripInset, fingerCurl);
    gripInset += cfg.GripInset;
    fingerCurl += cfg.FingerCurlExtra;

    // What the arms reach for (world): each hand, Grip Inset's way, and the upper arm and elbow (its bend plane) of the
    // arms posed by `Shape`, whose arm shapes and fingers the solve copies.
    struct ArmTargets {
        const Model* Shape = nullptr;
        glm::vec3 Pos[2]{}, Palm[2]{}, Shoulder[2]{}, Elbow[2]{};
        glm::quat Rot[2]{};
        bool Have[2] = {false, false}, HaveElbow[2] = {false, false};
    };
    // `from`'s arms as placed by `place` (its model space to world).
    auto readTargets = [&](const Model& from, const glm::mat4& place, ArmTargets& out) {
        out = ArmTargets{};
        out.Shape = &from;
        for (int s = 0; s < 2; ++s) {
            glm::mat4 h(1.0f), u(1.0f), l(1.0f);
            if (!from.NodeTransform(kSides[s].Hand, h)) continue;
            const glm::mat4 w = place * h;
            out.Pos[s] = glm::vec3(w[3]);
            out.Rot[s] = IK::Rotation(w);
            out.Palm[s] = FirstPersonBodyPalmDirection(from, place, s);
            out.Have[s] = true;
            if (from.NodeTransform(kSides[s].Upper, u) && from.NodeTransform(kSides[s].Lower, l)) {
                out.Shoulder[s] = glm::vec3(place * u[3]);
                out.Elbow[s] = glm::vec3(place * l[3]);
                out.HaveElbow[s] = true;
            }
        }
    };
    // The first person: the rig's hands, exactly - the arms piece is drawn with the gun in the same projection.
    ArmTargets firstPerson;
    readTargets(*rig, rigWorld, firstPerson);

    // The arms onto `t`'s hands: the pieces (first person), then the world twins (the world gun's hold). Solved once, on
    // the arms piece (else the driver); every other piece that skins the upper body takes the rig's arm shape (fingers,
    // twist bones) and then the solved bones as they came out - the chest's lean, the collarbones, the arms - so the
    // pieces stay one skeleton (as NpcBody::SyncPieces hands the soldiers' upper body round). Each call keeps its own
    // steadiness and elbow state.
    const float shoulderMax = glm::radians(std::clamp(cfg.ShoulderMaxAngle, 0.0f, 90.0f));
    const float leanMax = glm::radians(std::clamp(cfg.ReachLeanMax, 0.0f, 60.0f));
    auto solveArms = [&](const std::vector<std::shared_ptr<Model>>& models, const std::vector<entt::entity>& ents, const ArmTargets& t,
                         glm::vec3 (&anchor)[2], bool (&haveAnchor)[2], glm::vec3 (&elbowAim)[2], bool (&haveElbowAim)[2],
                         bool firstPersonView) {
        size_t source = models.size();
        for (size_t k = 0; k < models.size() && k < m_Pieces.size() && source == models.size(); ++k)
            if (models[k] && reg.valid(m_Pieces[k]) && IsArmsPiece(reg, m_Pieces[k]) && models[k]->NodeIndex(Bone(kSides[0].Upper)) >= 0)
                source = k;
        for (size_t k = 0; k < models.size() && k < m_Pieces.size() && source == models.size(); ++k)
            if (models[k] && m_Pieces[k] == m_Driver) source = k;
        if (source >= models.size() || source >= ents.size() || !reg.valid(ents[source]) || !t.Shape) return;
        Model& m = *models[source];
        // Scratch kept between frames (and between the pieces' and the twins' solves): no per-frame allocation.
        thread_local IK::Pose pose;
        thread_local std::vector<int> parents;
        thread_local std::vector<glm::mat4> globals;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) return;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(ents[source]);
        const glm::mat4 toModel = glm::inverse(pieceWorld);
        const glm::quat toModelRot = IK::Rotation(toModel);

        // Arm Steadiness filters the walk's sway, so it reads the shoulders as the body's own clips have them: read after
        // the rig's arm shapes, a reload's shoulder move was held back like sway and eased home late.
        IK::ComputeGlobals(pose, parents, globals);
        glm::vec3 ownShoulder[2]{};
        for (int s = 0; s < 2; ++s)
            if (const int u = m.NodeIndex(Bone(kSides[s].Upper)); u >= 0)
                ownShoulder[s] = glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[u]), 1.0f));

        // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve then only fixes the hands.
        CopyArmShapeCached(m, *t.Shape, m_ArmsWeight, pose, parents, cfg.ClavicleFollow);
        FirstPersonBodyCurlFingers(m, pose, fingerCurl * m_ArmsWeight);
        IK::ComputeGlobals(pose, parents, globals);

        // Reach Lean: a hand still out of reach once its collarbone has turned all it may (the gun pitches with the whole
        // view, the chest only by Spine Aim) - the chest leans toward it, at the top of the spine, by at most Reach Lean Max.
        int chest = -1; // the top spine bone the piece has
        for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(Bone(FPBody::kBoneSpine[b]));
        bool leaned = false;
        if (chest >= 0 && leanMax > 0.0f) {
            int upper[2], lower[2], hand[2], clav[2];
            for (int s = 0; s < 2; ++s) {
                upper[s] = m.NodeIndex(Bone(kSides[s].Upper));
                lower[s] = m.NodeIndex(Bone(kSides[s].Lower));
                hand[s] = m.NodeIndex(Bone(kSides[s].Hand));
                clav[s] = m.NodeIndex(Bone(kSides[s].Clavicle));
            }
            if (glm::quat q; FirstPersonBodyReachLean(globals, upper, lower, hand, clav, t.Have, t.Pos, toModel, chest, shoulderMax, leanMax,
                                                      m_ArmsWeight, q)) {
                IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), q, IK::Position(globals[chest]));
                leaned = true;
                if (firstPersonView) BodyDebug::Info().ReachLean = glm::degrees(FirstPersonBodyAngleOf(q));
            }
        }
        for (int s = 0; s < 2; ++s) {
            if (!t.Have[s]) continue;
            const int upper = m.NodeIndex(Bone(kSides[s].Upper)), lower = m.NodeIndex(Bone(kSides[s].Lower)),
                      hand = m.NodeIndex(Bone(kSides[s].Hand));
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const glm::vec3 target = glm::vec3(toModel * glm::vec4(t.Pos[s] + t.Palm[s] * gripInset * m_ArmsWeight, 1.0f));
            const glm::quat rot = glm::normalize(toModelRot * t.Rot[s]);
            const int clav = m.NodeIndex(Bone(kSides[s].Clavicle));
            if (firstPersonView) {
                // Before any shoulder move: how far the hand is, of the arm's length, and how far the body's shoulder is from the rig's.
                const glm::vec3 a = IK::Position(globals[upper]);
                const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                if (armLen > 1e-5f) BodyDebug::Info().Reach[s] = glm::length(target - a) / armLen;
                if (t.HaveElbow[s]) BodyDebug::Info().ShoulderGap[s] = glm::length(glm::vec3(pieceWorld * glm::vec4(a, 1.0f)) - t.Shoulder[s]);
            }
            float turned = 0.0f; // radians the collarbone has turned so far this frame
            // Steady: the shoulder back to its slow average in the anchor frame, so the gait's sway doesn't reach the arm.
            // Bigger departures (a crouch, a stop clip) are followed, trailing by at most Arm Steady Max. The shoulders move
            // by turning each clavicle about its root (the collarbone's inner end) - never by sliding it, which stretched the
            // skin between the neck and the shoulder into a hunch.
            if (steady > 0.0f && clav >= 0) {
                const glm::vec3 shoulderWorld = ownShoulder[s];
                const glm::vec3 inFrame = glm::vec3(anchorInverse * glm::vec4(shoulderWorld, 1.0f));
                if (!haveAnchor[s]) { anchor[s] = inFrame; haveAnchor[s] = true; }
                anchor[s] += (inFrame - anchor[s]) * Follow(dt, cfg.ArmSteadyTime);
                glm::vec3 shift = glm::vec3(anchorFrame * glm::vec4(anchor[s], 1.0f)) - shoulderWorld;
                const float maxShift = std::max(cfg.ArmSteadyMax, 0.0f);
                if (const float len = glm::length(shift); len > maxShift) {
                    // Past the limit the anchor is dragged along, so it never trails by more.
                    shift *= len > 1e-6f ? maxShift / len : 0.0f;
                    anchor[s] = glm::vec3(anchorInverse * glm::vec4(shoulderWorld + shift, 1.0f));
                }
                const glm::vec3 want = IK::Position(globals[upper]) + glm::mat3(toModel) * shift * steady;
                turned += FirstPersonBodyAngleOf(FirstPersonBodyTurnClavicle(pose, parents, globals, clav, upper, want, shoulderMax));
                if (firstPersonView) BodyDebug::Info().ArmSteadyShift[s] = glm::length(shift);
            }
            // A hand beyond the arm's reach: the shoulder shrugs toward it instead of the arm stretching (the two
            // skeletons' shoulders can sit a little apart).
            if (clav >= 0) {
                const glm::vec3 a = IK::Position(globals[upper]);
                const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                const glm::vec3 toTarget = target - a;
                const float dist = glm::length(toTarget);
                const float excess = std::min(dist - armLen * cfg.ShrugStart, cfg.ShrugMax);
                if (excess > 1e-4f && dist > 1e-5f) {
                    FirstPersonBodyTurnClavicle(pose, parents, globals, clav, upper, a + toTarget / dist * excess * m_ArmsWeight, shoulderMax - turned);
                    if (firstPersonView) BodyDebug::Info().ShrugShift[s] = glm::length(glm::mat3(pieceWorld) * (IK::Position(globals[upper]) - a));
                }
            }
            // Steady: the elbow into the rig's bend plane, as the solve's pole. The solve keeps the plane the copied arm shape
            // has, and that shape is the rig's relative to the body's chest - so the chest's sway, and the pitch Spine Aim
            // leaves out, swung the elbow about the shoulder-hand line. Measured in the view's frame, the rig's plane holds still.
            IK::TwoBoneHint hint;
            if (steady > 0.0f && t.HaveElbow[s]) {
                const glm::vec3 a = IK::Position(globals[upper]), b = IK::Position(globals[lower]), c = IK::Position(globals[hand]);
                const glm::vec3 line = glm::mix(c, target, std::clamp(m_ArmsWeight, 0.0f, 1.0f)) - a; // the solve's own target, from the shoulder
                const float lab = glm::length(b - a), lcb = glm::length(c - b);
                if (glm::dot(line, line) > 1e-8f && lab > 1e-6f && lcb > 1e-6f) {
                    const glm::vec3 axis = glm::normalize(line);
                    const glm::vec3 rigBend = glm::mat3(toModel) * (t.Elbow[s] - t.Shoulder[s]);
                    const glm::vec3 want = rigBend - axis * glm::dot(rigBend, axis);
                    // A near-straight rig arm (the melee's thrust and its recovery) has no bend plane to speak of: its elbow's
                    // few millimetres off the line swing anywhere frame to frame, while the body's arm (its shoulder nearer the
                    // hand) is already well bent. Turned onto that noise the elbow spun; left alone it looped out behind the
                    // back. So the elbow heads down and a little out - where an elbow hangs under a rifle - and onto the rig's
                    // plane as the rig's arm bends: how far its elbow stands off the line, as a share of its upper arm, none
                    // under 5%, all of it from 20%.
                    const float rigLen = std::max(glm::length(rigBend), 1e-4f);
                    const float trust = glm::smoothstep(0.05f, 0.2f, glm::length(want) / rigLen);
                    const glm::vec3 outward = s == 0 ? -kRight : kRight; // the upper arm's side (_l is the body's left)
                    glm::vec3 pole = glm::vec3(0.0f, -1.0f, 0.0f) + outward * 0.5f;
                    pole -= axis * glm::dot(pole, axis);
                    glm::vec3 aim = trust > 0.0f ? glm::normalize(want) * trust : glm::vec3(0.0f);
                    if (glm::dot(pole, pole) > 1e-8f) aim += glm::normalize(pole) * (1.0f - trust);
                    // Where the elbow heads is eased, and turns at most so fast: the rig's clips jump (the melee's 2-frame
                    // blend in), and in a tight fold - the melee's recovery, the body's shoulder nearer the gun than the
                    // rig's - the aim swings as fast as the shoulder-to-hand line does. Followed outright, the elbow whipped
                    // round. Held in the chest's view frame.
                    const glm::mat3 modelToAnchor = glm::mat3(anchorInverse) * glm::mat3(pieceWorld);
                    if (glm::dot(aim, aim) > 1e-8f) {
                        const glm::vec3 aimTo = glm::normalize(modelToAnchor * aim);
                        if (!haveElbowAim[s]) { elbowAim[s] = aimTo; haveElbowAim[s] = true; }
                        const float angle = std::acos(std::clamp(glm::dot(elbowAim[s], aimTo), -1.0f, 1.0f));
                        if (angle > 1e-5f) {
                            const float step = std::min(angle * Follow(dt, cfg.ElbowEase), glm::radians(cfg.ElbowMaxRate) * dt);
                            glm::vec3 turnAxis = glm::cross(elbowAim[s], aimTo);
                            if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::cross(elbowAim[s], glm::vec3(0.0f, 1.0f, 0.0f));
                            if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::vec3(1.0f, 0.0f, 0.0f);
                            elbowAim[s] = glm::normalize(glm::angleAxis(step, glm::normalize(turnAxis)) * elbowAim[s]);
                        }
                    }
                    if (haveElbowAim[s]) aim = glm::inverse(modelToAnchor) * elbowAim[s]; // the solve takes it off the line itself
                    // The body's own elbow must stand off the line for the turn to mean anything (2% .. 8% of the upper arm):
                    // the solved elbow's distance off the shoulder-hand line is the upper arm times the sine of the bend's
                    // angle at the shoulder (the triangle of the two bones and the reach the solve will make).
                    const float lat = std::clamp(glm::length(line), 1e-4f, (lab + lcb) * 0.9999f);
                    const float cosAtShoulder = std::clamp((lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat), -1.0f, 1.0f);
                    const float bodyTrust = glm::smoothstep(0.02f, 0.08f, std::sqrt(std::max(0.0f, 1.0f - cosAtShoulder * cosAtShoulder)));
                    if (bodyTrust > 0.0f && glm::dot(aim, aim) > 1e-8f) {
                        hint.HintWeight = steady * bodyTrust;
                        hint.Pole = a + aim;
                        hint.HavePole = true;
                    }
                }
            }
            IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, m_ArmsWeight, 0.0f,
                             hint.HintWeight > 0.0f ? &hint : nullptr);
        }
        // How far each of the hands ended up from its target (off the gun, when the arm can't reach).
        for (int s = 0; s < 2; ++s)
            if (const int hand = m.NodeIndex(Bone(kSides[s].Hand)); t.Have[s] && hand >= 0) {
                const float gap = glm::length(glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[hand]), 1.0f)) - t.Pos[s]);
                if (firstPersonView) {
                    BodyDebug::Info().HandGap[s] = gap;
                } else {
                    m_TwinHandGap[s] = gap;
                    BodyDebug::Info().TwinHandGap[s] = gap;
                }
            }
        m.ApplyLocalPose(pose);

        // The rest of the pieces: the rig's arm shape and fingers (what the solve leaves alone), then the solved bones.
        thread_local std::vector<std::string> solved;
        solved.clear();
        if (leaned) solved.push_back(m.NodeName(chest));
        for (int s = 0; s < 2; ++s)
            for (const char* b : {kSides[s].Clavicle, kSides[s].Upper, kSides[s].Lower, kSides[s].Hand}) solved.push_back(Bone(b));
        float pieceGap = 0.0f;
        for (size_t k = 0; k < models.size() && k < ents.size() && k < m_Pieces.size(); ++k) {
            if (k == source || !models[k] || !reg.valid(ents[k])) continue;
            Model& o = *models[k];
            // A piece skinning nothing the solve moves (legs, feet) draws the same either way. The driver always takes it:
            // its shoulders are read for the camera.
            if (m_Pieces[k] != m_Driver && !SkinsUpperBody(o)) continue;
            thread_local IK::Pose other;
            other = o.AppliedLocalPose();
            if (other.empty() || (int)other.size() != o.NodeCount()) continue;
            parents.resize(other.size());
            for (int i = 0; i < (int)other.size(); ++i) parents[i] = o.NodeParent(i);
            CopyArmShapeCached(o, *t.Shape, m_ArmsWeight, other, parents, cfg.ClavicleFollow);
            FirstPersonBodyCurlFingers(o, other, fingerCurl * m_ArmsWeight);
            for (const std::string& name : solved)
                if (const int from = m.NodeIndex(name), to = o.NodeIndex(name); from >= 0 && to >= 0) other[to].R = pose[from].R;
            o.ApplyLocalPose(other);
            // Diagnostics: a piece whose skeleton doesn't rest as the source's would leave its hands off the gun.
            if (firstPersonView) {
                const glm::mat4 otherWorld = world.ComposeWorldTransform(ents[k]);
                for (int s = 0; s < 2; ++s)
                    if (const int hand = o.NodeIndex(Bone(kSides[s].Hand)); t.Have[s] && hand >= 0)
                        pieceGap = std::max(pieceGap, glm::length(glm::vec3(otherWorld * o.PoseNodeModelSpace(other, hand)[3]) - t.Pos[s]));
            }
        }
        if (firstPersonView) BodyDebug::Info().PieceHandGap = pieceGap;
    };
    {
    PROFILE_SCOPE("FPB arms: pieces");
    solveArms(m_Models, m_Pieces, firstPerson, m_ShoulderAnchor, m_HaveShoulderAnchor, m_ElbowAim, m_HaveElbowAim, true);
    ArmTwist(m_Models);
    }

    // The world twins (every view but the player's own, and the shadows): the same body and the same solve, onto the world
    // gun's hold - the weapon's third-person clips (arms3p: lower, off the centreline, into the right shoulder). The world gun
    // is the first-person one moved by where the 3P clips hold their gun against where the first-person clips hold theirs
    // (`thirdPersonGun`, the rig's model space), so the rig's recoil and sway still carry it; the 3P clips' hands, elbows and
    // fingers go on it as they hold it. Without 3P clips, the rig's own grip moves with the gun.
    m_WorldGunDelta = glm::mat4(1.0f);
    if (!m_Twins.empty()) {
        PROFILE_SCOPE("FPB arms: twins");
        m_WorldGunDelta = rigWorld * thirdPersonGun * glm::inverse(rigWorld);
        ArmTargets hold = firstPerson;
        glm::mat4 first(1.0f), third(1.0f);
        if (thirdPersonRig && !gunSocket.empty() && (int)thirdPersonRig->AppliedLocalPose().size() == thirdPersonRig->NodeCount() &&
            rig->NodeTransform(gunSocket, first) && thirdPersonRig->NodeTransform(gunSocket, third))
            readTargets(*thirdPersonRig, rigWorld * first * glm::inverse(third), hold); // the 3P clips with their gun on the rig's
        const glm::quat turn = IK::Rotation(m_WorldGunDelta);
        for (int s = 0; s < 2; ++s) {
            hold.Pos[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Pos[s], 1.0f));
            hold.Rot[s] = glm::normalize(turn * hold.Rot[s]);
            hold.Palm[s] = turn * hold.Palm[s];
            hold.Shoulder[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Shoulder[s], 1.0f));
            hold.Elbow[s] = glm::vec3(m_WorldGunDelta * glm::vec4(hold.Elbow[s], 1.0f));
        }
        solveArms(m_TwinModels, m_Twins, hold, m_WorldShoulderAnchor, m_WorldHaveShoulderAnchor, m_WorldElbowAim, m_WorldHaveElbowAim, false);
        ArmTwist(m_TwinModels);
        PlaceHeadAttachedTwins(world);
    }

    // Debug (the Inspector's readout, and the Scene overlay's Gizmos > Player body): the play camera's view
    // and each arm as the arms rig has it (yellow) against the body's (magenta).
    PROFILE_SCOPE("FPB arms debug");
    if (camera) {
        const glm::vec3 eye = camera->Position, f = camera->Front(), r = camera->Right(), u = camera->Up();
        BodyDebug::Info().NearPlane = camera->NearPlane;
        if (BodyDebug::Enabled()) {
            // Near plane (red), then the world's frustum (grey) and the view-model pass's (cyan) out to 40 cm.
            const float aspect = 16.0f / 9.0f;
            auto frustum = [&](float fovDeg, float dist, const glm::vec4& c, bool spokes) {
                const float h = std::tan(glm::radians(fovDeg) * 0.5f) * dist, w = h * aspect;
                const glm::vec3 m = eye + f * dist, p[4] = {m - r * w - u * h, m + r * w - u * h, m + r * w + u * h, m - r * w + u * h};
                for (int i = 0; i < 4; ++i) {
                    BodyDebug::Line(p[i], p[(i + 1) % 4], c);
                    if (spokes) BodyDebug::Line(eye, p[i], c);
                }
            };
            frustum(camera->Fov, camera->NearPlane, glm::vec4(1.0f, 0.1f, 0.1f, 1.0f), false);
            frustum(camera->Fov, 0.4f, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f), true);
            if (viewModelFov > 0.0f) {
                frustum(viewModelFov, camera->NearPlane, glm::vec4(1.0f, 0.4f, 0.1f, 1.0f), false);
                frustum(viewModelFov, 0.4f, glm::vec4(0.1f, 0.9f, 1.0f, 1.0f), true);
            }
            BodyDebug::Cross(eye, 0.02f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
        }
        auto segDist = [&](const glm::vec3& a, const glm::vec3& b) {
            const glm::vec3 ab = b - a;
            const float t = glm::dot(ab, ab) > 1e-10f ? std::clamp(glm::dot(eye - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f) : 0.0f;
            return glm::length(eye - (a + ab * t));
        };
        // One side's clavicle -> upper arm -> forearm -> hand, world, as `m` stands in `modelWorld`.
        auto chain = [&](const Model& m, const glm::mat4& modelWorld, int s, glm::vec3 (&p)[4]) {
            const IK::Pose& pose = m.AppliedLocalPose();
            if ((int)pose.size() != m.NodeCount()) return false;
            const char* names[4] = {kSides[s].Clavicle, kSides[s].Upper, kSides[s].Lower, kSides[s].Hand};
            for (int j = 0; j < 4; ++j) {
                const int node = m.NodeIndex(Bone(names[j]));
                if (node < 0) return false;
                p[j] = glm::vec3(modelWorld * m.PoseNodeModelSpace(pose, node)[3]);
            }
            return true;
        };
        size_t armsK = m_Models.size();
        for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
            if (m_Models[k] && reg.valid(m_Pieces[k]) && IsArmsPiece(reg, m_Pieces[k])) { armsK = k; break; }
        }
        for (int s = 0; s < 2; ++s) {
            glm::vec3 p[4];
            if (chain(*rig, rigWorld, s, p)) {
                BodyDebug::Info().EyeToUpperArmRig[s] = segDist(p[1], p[2]);
                for (int j = 0; j < 3; ++j) BodyDebug::Line(p[j], p[j + 1], glm::vec4(1.0f, 0.9f, 0.1f, 1.0f)); // the rig's
            }
            if (armsK < m_Models.size() && chain(*m_Models[armsK], world.ComposeWorldTransform(m_Pieces[armsK]), s, p)) {
                BodyDebug::Info().EyeToUpperArmBody[s] = segDist(p[1], p[2]);
                for (int j = 0; j < 3; ++j) BodyDebug::Line(p[j], p[j + 1], glm::vec4(1.0f, 0.3f, 0.9f, 1.0f)); // the body's
            }
        }
        if (BodyDebug::Enabled() || BodyDebug::ProbeRequested()) CameraProbe(world, *camera, viewModelFov, dt);
        if (BodyDebug::ProbeRequested()) InsideProbe(world, *camera);
    }
}
