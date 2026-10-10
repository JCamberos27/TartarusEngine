// NpcBody's weapon hold: the player's world body's solve (FirstPersonBody::ArmsLateUpdate and the shoulder
// lock in FirstPersonBody::LateUpdate), for an enemy soldier - the same Quantum body holding the same
// first-person rigs, so the same steps and the same numbers (NpcHoldSettings, the player's First Person Body).
//
// The rig is the weapon's first-person arms, posed on a camera like the player's. The eye is hung off the
// body's shoulders as the rig's camera is off the rig's (WeaponEye), which puts the rig's hands where the
// body's arms reach them. HoldWeapon then moves the gun the little left to seat the stock in the shoulder
// pocket and keep it out of the head, neck and torso, and solves every piece's arms onto the rig's hands.
#include "NpcBody.h"

#include "Camera.h"
#include "Components.h"
#include "FirstPersonBody.h"
#include "FirstPersonBodyContract.h"
#include "IK.h"
#include "Model.h"
#include "ModelVertex.h"
#include "World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace {

// Model space: the Quantum / UE mannequin faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
const glm::quat kNone(1.0f, 0.0f, 0.0f, 0.0f);

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }
float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }
float AngleOf(const glm::quat& q) { return 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)); }

// The collarbone turned about its inner end so the upper arm heads for `want` (model space), by at most
// `maxAngle` - never slid, which stretched the skin between neck and shoulder. Returns the turn made.
glm::quat TurnClavicle(IK::Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int clav, int upper,
                       const glm::vec3& want, float maxAngle) {
    const glm::vec3 root = IK::Position(globals[(size_t)clav]);
    const glm::vec3 from = IK::Position(globals[(size_t)upper]) - root, to = want - root;
    if (maxAngle <= 0.0f || glm::dot(from, from) < 1e-8f || glm::dot(to, to) < 1e-8f) return kNone;
    glm::quat q(glm::normalize(from), glm::normalize(to));
    const float angle = AngleOf(q);
    if (angle < 1e-5f) return kNone;
    if (angle > maxAngle) q = glm::slerp(kNone, q, maxAngle / angle);
    IK::OffsetBone(pose, parents, globals, clav, glm::vec3(0.0f), q, root);
    return q;
}

float SegmentGap(const std::vector<glm::vec3>& points, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = std::max(glm::dot(ab, ab), 1e-9f);
    float best = 1e9f;
    for (const glm::vec3& p : points) {
        const float t = std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
        best = std::min(best, glm::length(a + ab * t - p));
    }
    return points.empty() ? -1.0f : best;
}

} // namespace

int NpcBody::ArmsPiece() const {
    if (m_ArmsIndex >= 0) return m_ArmsIndex;
    for (size_t k = 0; k < m_Pieces.size(); ++k)
        if (m_Pieces[k] == m_Driver) return (int)k;
    return -1;
}

void NpcBody::SkinnedRegion(const World& world, Region region, std::vector<glm::vec3>& out) const {
    out.clear();
    const auto& reg = world.Registry;
    if (m_Skins.size() != m_Models.size()) m_Skins.assign(m_Models.size(), PieceSkin{});
    for (size_t k = 0; k < m_Models.size(); ++k) {
        if (!m_Models[k] || !reg.valid(m_Pieces[k])) continue;
        const Model& m = *m_Models[k];
        PieceSkin& ps = m_Skins[k];
        if (!ps.Built || ps.M != &m) {
            ps = PieceSkin{};
            ps.M = &m;
            ps.Built = true;
            struct Key {
                const void* Verts; int Meshes; size_t Count;
                bool operator==(const Key& o) const { return Verts == o.Verts && Meshes == o.Meshes && Count == o.Count; }
            };
            struct KeyHash {
                size_t operator()(const Key& k) const { return std::hash<const void*>()(k.Verts) * 31u ^ (size_t)k.Meshes ^ k.Count * 131u; }
            };
            static std::unordered_map<Key, std::shared_ptr<const SkinTables>, KeyHash> s_Tables;
            const Key key{m.MeshCount() > 0 ? (const void*)m.MeshSkinVertices(0).data() : nullptr, m.MeshCount(),
                          m.MeshCount() > 0 ? m.MeshSkinVertices(0).size() : 0u};
            if (auto found = s_Tables.find(key); found != s_Tables.end()) {
                ps.T = found->second;
            } else {
            auto tables = std::make_shared<SkinTables>();
            for (int r = 0; r < 2; ++r) {
                // Vertices whose heaviest bone is the region's: the neck and head, or the pelvis and spine.
                std::vector<int> ids;
                if (r == 0)
                    for (const char* b : {"neck_01", "neck_02", "head"}) ids.push_back(m.BoneId(b));
                else
                    for (const char* b : {"pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05"}) ids.push_back(m.BoneId(b));
                std::vector<std::pair<int, int>> verts;
                for (int i = 0; i < m.MeshCount(); ++i) {
                    const auto& sv = m.MeshSkinVertices(i);
                    for (int v = 0; v < (int)sv.size(); ++v) {
                        int top = -1;
                        float w = 0.0f;
                        for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
                            if (sv[(size_t)v].BoneIDs[j] >= 0 && sv[(size_t)v].Weights[j] > w) { w = sv[(size_t)v].Weights[j]; top = sv[(size_t)v].BoneIDs[j]; }
                        if (top >= 0 && std::find(ids.begin(), ids.end(), top) != ids.end()) verts.push_back({i, v});
                    }
                }
                // Thinned as the player's are: one vertex per 1 cm cell (the gun and elbows move in 1 cm steps and
                // keep 5-6 cm off; a soldier is seen from further away than the player's own body).
                const float kCell = r == 0 ? 0.012f : 0.025f; // the torso is a big smooth surface
                std::unordered_set<std::uint64_t> taken;
                size_t n = 0;
                for (const auto& iv : verts) {
                    const glm::vec3& p = m.MeshSkinVertices(iv.first)[(size_t)iv.second].Position;
                    const auto cell = [&](float x) { return (std::uint64_t)(std::int64_t)std::floor(x / kCell) & 0x1FFFFFu; };
                    if (taken.insert(cell(p.x) | cell(p.y) << 21 | cell(p.z) << 42).second) verts[n++] = iv;
                }
                verts.resize(n);
                RegionSkin& skin = r == 0 ? tables->Head : tables->Torso;
                std::unordered_map<int, int> slot;
                for (const auto& [i, v] : verts) {
                    const ModelMesh::SkinVertex& sv = m.MeshSkinVertices(i)[(size_t)v];
                    RegionPoint rp{sv.Position, 0, {}, {}};
                    float total = 0.0f;
                    for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
                        if (sv.BoneIDs[j] >= 0 && sv.Weights[j] > 0.0f) {
                            auto [it, added] = slot.emplace(sv.BoneIDs[j], (int)skin.Bones.size());
                            if (added) skin.Bones.push_back(sv.BoneIDs[j]);
                            rp.Bone[rp.Count] = (std::uint16_t)it->second;
                            rp.Weight[rp.Count++] = sv.Weights[j];
                            total += sv.Weights[j];
                        }
                    if (total <= 0.0f) continue;
                    for (int j = 0; j < rp.Count; ++j) rp.Weight[j] /= total;
                    skin.Points.push_back(rp);
                }
            }
            if (s_Tables.size() > 64) s_Tables.clear();
            s_Tables[key] = tables;
            ps.T = tables;
            }
        }
        if (!ps.T) continue;
        const RegionSkin& skin = region == Region::Head ? ps.T->Head : ps.T->Torso;
        if (skin.Points.empty()) continue;
        const glm::mat4 toWorld = world.ComposeWorldTransform(m_Pieces[k]);
        thread_local std::vector<glm::mat4> palette;
        palette.resize(skin.Bones.size());
        for (size_t b = 0; b < skin.Bones.size(); ++b) palette[b] = toWorld * m.FinalBoneMatrix(skin.Bones[b]);
        const size_t first = out.size();
        out.resize(first + skin.Points.size());
        glm::vec3* o = out.data() + first;
        for (const RegionPoint& rp : skin.Points) {
            glm::vec3 p(0.0f);
            for (int j = 0; j < rp.Count; ++j) {
                const glm::mat4& M = palette[rp.Bone[j]];
                p += rp.Weight[j] * (glm::vec3(M[0]) * rp.Pos.x + glm::vec3(M[1]) * rp.Pos.y + glm::vec3(M[2]) * rp.Pos.z + glm::vec3(M[3]));
            }
            *o++ = p;
        }
    }
}

void NpcBody::WarmHoldTables(const World& world) {
    SkinnedRegion(world, Region::Head, m_HeadPoints); // builds every piece's head and torso tables
    m_HeadPoints.clear();
}

void NpcBody::RotateSpine(const glm::quat& modelDelta) {
    if (AngleOf(modelDelta) < 1e-6f || !m_DriverModel) return;
    Model& m = *m_DriverModel;
    IK::Pose& pose = m_Pose;
    pose = m.AppliedLocalPose();
    const int n = m_DriverSpineCount;
    if (n == 0 || (int)pose.size() != m.NodeCount()) return;
    IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
    OffsetSpine([&](float d) { return glm::normalize(glm::slerp(kNone, modelDelta, 1.0f / d)); });
    m.ApplyLocalPose(pose);
    m_PiecesStale = true;
}

void NpcBody::SyncPieces() {
    m_PiecesStale = false;
    if (!m_DriverModel) return;
    const auto& src = m_DriverModel->AppliedLocalPose();
    if ((int)src.size() != m_DriverModel->NodeCount()) return;
    for (size_t k = 0; k < m_Models.size() && k < m_UpperMap.size(); ++k) {
        if ((int)k == m_DriverIndex || m_UpperMap[k].empty() || !m_Models[k]) continue;
        if (!PieceTakes(k, kSkinsSpine | kSkinsArms | kSkinsNeck)) continue;
        Model& m = *m_Models[k];
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if ((int)pose.size() != m.NodeCount()) continue;
        // Spine stabilization also shifts the spine to cancel inherited pelvis sway. Share those
        // translations as well as the rotations, so clothing follows the stabilized torso.
        // Other upper-body bones keep their own offsets.
        for (const auto& [pn, dn] : m_UpperMap[k]) {
            pose[(size_t)pn].R = src[(size_t)dn].R;
            if (std::find(m_DriverSpine, m_DriverSpine + m_DriverSpineCount, dn) != m_DriverSpine + m_DriverSpineCount)
                pose[(size_t)pn].T = src[(size_t)dn].T;
        }
        m.ApplyLocalPose(pose);
    }
}

glm::vec3 NpcBody::WeaponEye(const World& world, entt::entity armsRig, const std::string& cameraBone, const Camera& cam, float dt) {
    const auto& reg = world.Registry;
    const glm::mat4 rootW = RootWorld();
    glm::vec3 sl(0.0f), sr(0.0f);
    const Model* rig = armsRig != entt::null && reg.valid(armsRig) && reg.all_of<RenderableComponent>(armsRig)
                           ? reg.get<RenderableComponent>(armsRig).ModelRef.get()
                           : nullptr;
    glm::mat4 rl(1.0f), rr(1.0f), cb(1.0f);
    if (!rig || rig->AppliedLocalPose().empty() || !BoneWorld(FPBody::kBoneUpperArm[0], sl) || !BoneWorld(FPBody::kBoneUpperArm[1], sr) ||
        !rig->NodeTransform(FPBody::kBoneUpperArm[0], rl) || !rig->NodeTransform(FPBody::kBoneUpperArm[1], rr) || !m_HaveShouldersAnimated) {
        m_HaveRigOffset = m_HaveShoulders = false;
        return m_Eye; // no rig posed yet: the head bone
    }
    if (!cameraBone.empty()) rig->NodeTransform(cameraBone, cb);
    // The rig's shoulders off its camera, in the camera's frame. The clips sway the shoulders about the
    // camera; only a slow drift of this is followed, so the sway shows on the arms and not on the gun.
    const glm::mat4 rigWorld = world.ComposeWorldTransform(armsRig);
    const glm::vec3 fromEye = glm::mat3(rigWorld) * (0.5f * (glm::vec3(rl[3]) + glm::vec3(rr[3])) - glm::vec3(cb[3]));
    const glm::vec3 inCamera(glm::dot(fromEye, cam.Right()), glm::dot(fromEye, cam.Up()), glm::dot(fromEye, cam.Front()));
    if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
    m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);
    // ... put on the body's own shoulders, a little slack toward the gun so the support arm never locks straight - the
    // player's camera's solve (FirstPersonBody::LateUpdate), step for step. The shoulders the eye hangs off are the clips'
    // eased (only Head Bob of their motion about a slow average, never more than Eye Slack behind), with what the spine
    // passes did to them (the aim's pitch and twist, the stance) on in full.
    const glm::vec3 shouldersNow = glm::vec3(glm::inverse(rootW) * glm::vec4(0.5f * (sl + sr), 1.0f)); // model space
    const glm::vec3 toRest = shouldersNow - m_ShouldersAnimated;
    if (!m_HaveShoulders) { m_ShouldersSlow = m_Shoulders = m_ShouldersAnimated; m_HaveShoulders = true; }
    m_ShouldersSlow += (m_ShouldersAnimated - m_ShouldersSlow) * Follow(dt, 0.5f);
    const glm::vec3 shoulderTarget = m_ShouldersSlow + (m_ShouldersAnimated - m_ShouldersSlow) * std::clamp(m_Set.HeadBob, 0.0f, 1.0f);
    m_Shoulders += (shoulderTarget - m_Shoulders) * Follow(dt, m_Set.CameraSmoothing);
    const glm::vec3 gap = m_ShouldersAnimated - m_Shoulders;
    if (const float gapLen = glm::length(gap); gapLen > m_Set.EyeSlack) m_Shoulders = m_ShouldersAnimated - gap / gapLen * m_Set.EyeSlack;
    // Armed Eye Offset: the rig's camera sits lower against its shoulders than an eye does; lifted back up looking level
    // (without it the gun rode ~10 cm low, into the chest, and a reload's keep-out shoved it down to the belt).
    const float viewPitch = std::asin(std::clamp(cam.Front().y, -1.0f, 1.0f));
    const glm::vec3 lift = (kRight * m_Set.ArmedEyeOffset.x + glm::vec3(0.0f, m_Set.ArmedEyeOffset.y, 0.0f) + kForward * m_Set.ArmedEyeOffset.z) *
                           FirstPersonBodyArmedEyeLift(viewPitch);
    glm::vec3 eye = glm::vec3(rootW * glm::vec4(m_Shoulders + toRest + lift, 1.0f)) -
                    (cam.Right() * m_RigEyeToShoulders.x + cam.Up() * m_RigEyeToShoulders.y + cam.Front() * (m_RigEyeToShoulders.z + m_Set.ReachSlack));
    glm::vec3 flat = cam.Front();
    flat.y = 0.0f;
    if (glm::dot(flat, flat) > 1e-8f) eye += glm::normalize(flat) * (m_Set.LookDownPush * FirstPersonBodyLookDown(viewPitch, m_Set.LookDownStart));
    return eye;
}

glm::vec3 NpcBody::HoldWeapon(World& world, entt::entity armsRig, entt::entity weapon, const FirstPersonWorldGunInput* gun, const Camera& cam,
                              float dt, bool meshChecks, const Model* thirdPersonRig, const std::string& gunSocket) {
    if (!IsActive() || m_PoseExternal || m_WeaponReleased) return glm::vec3(0.0f);
    auto& reg = world.Registry;
    if (weapon != entt::null && reg.valid(weapon)) TrackGun(world.WorldSpaceTransform(weapon).Position, dt);
    const bool haveRig = gun && armsRig != entt::null && reg.valid(armsRig) && reg.all_of<RenderableComponent>(armsRig) &&
                         reg.get<RenderableComponent>(armsRig).ModelRef;
    // Drawn, the arms take the rig's hands from the first frame; holstered they ease back to the clips'.
    if (haveRig) m_ArmsWeight = 1.0f;
    else m_ArmsWeight -= m_ArmsWeight * Follow(dt, 0.1f);
    m_Hold = NpcHoldReport{};
    m_HaveHandTarget[0] = m_HaveHandTarget[1] = false;
    if (!haveRig || m_ArmsWeight < 1e-3f) {
        if (m_PiecesStale) SyncPieces();
        m_GunShift = glm::vec3(0.0f);
        m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
        m_ElbowClear[0] = m_ElbowClear[1] = 0.0f;
        m_HaveRigLine = false;
        m_CheekWeld = 0.0f;
        return glm::vec3(0.0f);
    }
    const Model& rig = *reg.get<RenderableComponent>(armsRig).ModelRef;
    if ((int)rig.AppliedLocalPose().size() != rig.NodeCount()) {
        if (m_PiecesStale) SyncPieces();
        return glm::vec3(0.0f);
    }
    const glm::mat4 rootW = RootWorld();
    const glm::mat4 toModel = glm::inverse(rootW);
    const glm::mat3 toModel3(toModel);
    const glm::quat toModelRot = IK::Rotation(toModel);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    // The solve runs on the driver (the whole skeleton; SyncPieces hands the result to the drawn pieces).
    const int armsK = m_DriverIndex >= 0 ? m_DriverIndex : ArmsPiece();

    // The rig's stance for the chest (LateUpdate's shoulder line next frame): its left-from-right upper arm,
    // in the camera's frame.
    {
        glm::mat4 rl(1.0f), rr(1.0f);
        if (rig.NodeTransform(FPBody::kBoneUpperArm[0], rl) && rig.NodeTransform(FPBody::kBoneUpperArm[1], rr)) {
            const glm::vec3 across = glm::mat3(world.ComposeWorldTransform(armsRig)) * (glm::vec3(rl[3]) - glm::vec3(rr[3]));
            const glm::vec3 inCam(glm::dot(across, cam.Right()), glm::dot(across, cam.Up()), glm::dot(across, cam.Front()));
            if (!m_HaveRigLine) { m_RigShoulderLine = inCam; m_HaveRigLine = true; }
            m_RigShoulderLine += (inCam - m_RigShoulderLine) * Follow(dt, 0.15f);
        }
    }

    // 1. The gun: its butt into the right shoulder pocket while shouldered, and clear of the neck and head
    // always (and of the torso when not shouldered: a reload tucks it under the arm). As the player's world gun.
    glm::vec3 shiftTarget(0.0f);
    // The drawn surfaces are skinned and searched every third frame (staggered between soldiers); what they gave -
    // the gun's push out of them, the elbows' swing, the cheek weld's share - holds and eases in between.
    const bool meshNow = meshChecks && (m_HoldFrame++ % 3) == (unsigned)m_HoldStagger;
    if (meshNow && m_PiecesStale) SyncPieces(); // the drawn surfaces are skinned from the pieces' own poses
    const bool torsoPoints = meshNow && m_Set.ElbowClearance > 0.0f;
    if (torsoPoints) SkinnedRegion(world, Region::Torso, m_TorsoPoints);
    else if (!meshChecks) m_TorsoPoints.clear();
    if (!meshChecks) { m_MeshPush = glm::vec3(0.0f); m_WeldShare = 1.0f; }
    {
        FirstPersonGunSeatBody body;
        if (BoneWorld(FPBody::kBoneUpperArm[0], body.UpperL) && BoneWorld(FPBody::kBoneUpperArm[1], body.UpperR) &&
            BoneWorld("neck_01", body.Neck) && BoneWorld("head", body.Head)) {
            glm::vec3 chest(0.0f);
            if (BoneWorld("spine_05", chest)) body.Chest = &chest;
            if (meshNow && gun->MeshClearance > 0.0f) SkinnedRegion(world, Region::Head, m_HeadPoints);
            if (meshNow) body.HeadPoints = &m_HeadPoints;
            if (torsoPoints) body.TorsoPoints = &m_TorsoPoints;
            const glm::vec3 shift = FirstPersonGunSeat(*gun, body, meshNow ? &m_MeshPush : nullptr, meshChecks ? &m_MeshPush : nullptr);
            shiftTarget = shift * m_ArmsWeight;
        }
    }
    m_GunShift += (shiftTarget - m_GunShift) * Follow(dt, 0.05f);
    for (entt::entity e : {armsRig, weapon})
        if (e != entt::null && reg.valid(e)) reg.get<TransformComponent>(e).Position += m_GunShift;
    m_Hold.Shift = glm::length(m_GunShift);

    // 2. The rig's hands (and its arms, for the elbows' bend plane), with the gun where it now is.
    const glm::mat4 rigWorld = world.ComposeWorldTransform(armsRig); // (moved with the gun just above)
    // Whose grip the arms take: the 3P clips' (their hands, elbows and fingers where they hold their gun, put on this rig's
    // gun), else the rig's. One skeleton, so the same node indices.
    const Model* gripRig = &rig;
    glm::mat4 gripWorld = rigWorld;
    if (thirdPersonRig && !gunSocket.empty() && thirdPersonRig->NodeCount() == rig.NodeCount() &&
        (int)thirdPersonRig->AppliedLocalPose().size() == thirdPersonRig->NodeCount()) {
        glm::mat4 first(1.0f), third(1.0f);
        if (rig.NodeTransform(gunSocket, first) && thirdPersonRig->NodeTransform(gunSocket, third)) {
            gripRig = thirdPersonRig;
            gripWorld = rigWorld * first * glm::inverse(third);
        }
    }
    glm::vec3 handPos[2]{};
    glm::quat handRot[2]{};
    bool haveHand[2] = {false, false};
    glm::vec3 rigShoulder[2]{}, rigElbow[2]{};
    bool haveRigElbow[2] = {false, false};
    // What the arms read by name - the rig's fingers, the driver's matching nodes, the arm-shape pairs, the driver's arm
    // and chest bones - found once per rig, then read by index every frame.
    if (m_FingerRig != &rig || m_FingerNodes != rig.NodeCount()) {
        m_FingerRig = &rig;
        m_FingerNodes = rig.NodeCount();
        m_FingerList.clear();
        for (int s = 0; s < 2; ++s) {
            const int rigHand = rig.NodeIndex(FPBody::kBoneHand[s]);
            if (rigHand < 0) continue;
            for (int i = rigHand + 1; i < rig.NodeCount(); ++i) {
                int p = rig.NodeParent(i);
                while (p > rigHand) p = rig.NodeParent(p);
                if (p == rigHand)
                    m_FingerList.push_back({i, s, rig.NodeName(i), m_DriverModel ? m_DriverModel->NodeIndex(rig.NodeName(i)) : -1});
            }
        }
        m_ArmLinks.clear();
        m_DriverArm = ArmBones{};
        if (m_DriverModel && (int)m_DriverParents.size() == m_DriverModel->NodeCount()) {
            const Model& d = *m_DriverModel;
            FirstPersonBodyArmShapeLinks(d, rig, m_DriverParents, m_ArmLinks);
            for (int s = 0; s < 2; ++s) {
                m_DriverArm.Upper[s] = d.NodeIndex(FPBody::kBoneUpperArm[s]);
                m_DriverArm.Lower[s] = d.NodeIndex(FPBody::kBoneLowerArm[s]);
                m_DriverArm.Hand[s] = d.NodeIndex(FPBody::kBoneHand[s]);
                m_DriverArm.Clav[s] = d.NodeIndex(FPBody::kBoneClavicle[s]);
            }
            for (int b = 4; b >= 0 && m_DriverArm.Chest < 0; --b) m_DriverArm.Chest = d.NodeIndex(FPBody::kBoneSpine[b]);
        }
    }
    struct Finger { const FingerNode* Node; glm::quat Rot; };
    thread_local std::vector<Finger> fingers;
    fingers.clear();
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f), u(1.0f), l(1.0f);
        if (!gripRig->NodeTransform(FPBody::kBoneHand[s], h)) continue;
        const glm::mat4 w = gripWorld * h;
        handPos[s] = glm::vec3(w[3]);
        handRot[s] = IK::Rotation(w);
        haveHand[s] = true;
        m_HandTarget[s] = handPos[s];
        m_HaveHandTarget[s] = true;
        if (gripRig->NodeTransform(FPBody::kBoneUpperArm[s], u) && gripRig->NodeTransform(FPBody::kBoneLowerArm[s], l)) {
            rigShoulder[s] = glm::vec3(gripWorld * u[3]);
            rigElbow[s] = glm::vec3(gripWorld * l[3]);
            haveRigElbow[s] = true;
        }
        for (const FingerNode& f : m_FingerList) {
            if (f.Hand != s) continue;
            glm::mat4 g(1.0f);
            if (gripRig->NodeTransformAt(f.Node, g)) fingers.push_back({&f, glm::normalize(toModelRot * IK::Rotation(gripWorld * g))});
        }
    }

    // 3. Every piece's arms onto them. The source (the arms piece) works out the shoulders' and chest's moves
    // and the elbows'; the rest take the same (one skeleton, one model space: the pieces sit on the root).
    thread_local std::vector<size_t> order;
    order.clear();
    if (armsK >= 0) order.push_back((size_t)armsK);
    if (m_DriverIndex < 0)
        for (size_t k = 0; k < m_Models.size(); ++k)
            if ((int)k != armsK) order.push_back(k);
    const float shoulderMax = glm::radians(std::clamp(m_Set.ShoulderMaxAngle, 0.0f, 90.0f));
    const float leanMax = glm::radians(std::clamp(m_Set.ReachLeanMax, 0.0f, 60.0f));
    const glm::mat3 camToModel = toModel3 * glm::mat3(cam.Right(), cam.Up(), cam.Front()); // the camera's frame, model space
    glm::quat clavTurn[2] = {kNone, kNone}, chestTurn = kNone;
    float swivelPlane[2] = {0.0f, 0.0f};
    bool sourceDone = false;
    std::vector<glm::mat4>& globals = m_Globals;
    const float w = m_ArmsWeight;
    for (size_t k : order) {
        if (k >= m_Models.size() || !m_Models[k]) continue;
        Model& m = *m_Models[k];
        // The driver's bones and parents are known; another piece (no driver) looks its own up.
        const bool driver = (int)k == m_DriverIndex && !m_ArmLinks.empty();
        ArmBones bones = m_DriverArm;
        if (!driver) {
            bones = ArmBones{};
            for (int s = 0; s < 2; ++s) {
                bones.Upper[s] = m.NodeIndex(FPBody::kBoneUpperArm[s]);
                bones.Lower[s] = m.NodeIndex(FPBody::kBoneLowerArm[s]);
                bones.Hand[s] = m.NodeIndex(FPBody::kBoneHand[s]);
                bones.Clav[s] = m.NodeIndex(FPBody::kBoneClavicle[s]);
            }
            for (int b = 4; b >= 0 && bones.Chest < 0; --b) bones.Chest = m.NodeIndex(FPBody::kBoneSpine[b]);
        }
        const int* upperN = bones.Upper;
        if (upperN[0] < 0 && upperN[1] < 0) continue;
        const bool source = (int)k == armsK || (armsK < 0 && !sourceDone);
        const bool armsDrawn = source || PieceTakes(k, kSkinsArms);
        if (!armsDrawn && !(sourceDone && AngleOf(chestTurn) > 1e-5f && PieceTakes(k, kSkinsSpine))) continue;
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        if (!driver) {
            m_PieceParents.resize(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) m_PieceParents[(size_t)i] = m.NodeParent(i);
        }
        const std::vector<int>& parents = driver ? m_DriverParents : m_PieceParents;
        const int chest = bones.Chest;
        if (!armsDrawn) {
            // Draws the chest and head but not the arms (a balaclava, a head): only the chest's lean.
            IK::ComputeGlobals(pose, parents, globals);
            if (chest >= 0) IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), chestTurn, IK::Position(globals[(size_t)chest]));
            m.ApplyLocalPose(pose);
            continue;
        }
        // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve then only
        // fixes the hands.
        if (driver) FirstPersonBodyCopyArmShape(m_ArmLinks, *gripRig, w, pose, m_Set.ClavicleFollow);
        else FirstPersonBodyCopyArmShape(m, *gripRig, w, pose, parents, m_Set.ClavicleFollow);
        IK::ComputeGlobals(pose, parents, globals);
        const bool isSource = !sourceDone;
        sourceDone = true;
        // Reach Lean: a hand still out of reach once its collarbone has turned all it may - the chest leans to it.
        if (chest >= 0 && isSource && leanMax > 0.0f) {
            glm::vec3 from(0.0f), to(0.0f);
            float worst = 0.0f;
            for (int s = 0; s < 2; ++s) {
                if (!haveHand[s]) continue;
                const int upper = upperN[s], lower = bones.Lower[s], hand = bones.Hand[s], clav = bones.Clav[s];
                if (upper < 0 || lower < 0 || hand < 0) continue;
                const glm::vec3 a = IK::Position(globals[(size_t)upper]);
                const float armLen = glm::length(IK::Position(globals[(size_t)lower]) - a) +
                                     glm::length(IK::Position(globals[(size_t)hand]) - IK::Position(globals[(size_t)lower]));
                const float clavReach = clav >= 0 ? glm::length(a - IK::Position(globals[(size_t)clav])) * shoulderMax : 0.0f;
                const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
                const float dist = glm::length(target - a);
                const float excess = dist - armLen * 0.995f - clavReach;
                if (excess > worst && dist > 1e-5f) { worst = excess; from = a; to = a + (target - a) / dist * excess; }
            }
            if (worst > 1e-4f) {
                const glm::vec3 root = IK::Position(globals[(size_t)chest]);
                if (glm::dot(from - root, from - root) > 1e-8f && glm::dot(to - root, to - root) > 1e-8f) {
                    glm::quat q(glm::normalize(from - root), glm::normalize(to - root));
                    if (const float angle = AngleOf(q); angle > leanMax) q = glm::slerp(kNone, q, leanMax / angle);
                    q = glm::slerp(kNone, q, w);
                    IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), q, root);
                    chestTurn = q;
                }
            }
        } else if (chest >= 0 && !isSource && AngleOf(chestTurn) > 1e-5f) {
            IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), chestTurn, IK::Position(globals[(size_t)chest]));
        }
        for (int s = 0; s < 2; ++s) {
            if (!haveHand[s]) continue;
            const int upper = upperN[s], lower = bones.Lower[s], hand = bones.Hand[s];
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const int clav = bones.Clav[s];
            const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
            const glm::quat rot = glm::normalize(toModelRot * handRot[s]);
            if (!isSource && clav >= 0 && AngleOf(clavTurn[s]) > 1e-5f)
                IK::OffsetBone(pose, parents, globals, clav, glm::vec3(0.0f), clavTurn[s], IK::Position(globals[(size_t)clav]));
            // A hand beyond the arm's reach: the shoulder shrugs toward it instead of the arm stretching.
            if (isSource && clav >= 0) {
                const glm::vec3 a = IK::Position(globals[(size_t)upper]);
                const float armLen = glm::length(IK::Position(globals[(size_t)lower]) - a) +
                                     glm::length(IK::Position(globals[(size_t)hand]) - IK::Position(globals[(size_t)lower]));
                const glm::vec3 toTarget = target - a;
                const float dist = glm::length(toTarget);
                const float excess = std::min(dist - armLen * m_Set.ShrugStart, m_Set.ShrugMax);
                if (excess > 1e-4f && dist > 1e-5f)
                    clavTurn[s] = TurnClavicle(pose, parents, globals, clav, upper, a + toTarget / dist * excess * w, shoulderMax);
            }
            IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, w);
            // The elbow into the rig's bend plane, eased and rate-limited in the camera's frame (worked out on the
            // source, given to all). A near-straight rig arm has no plane to speak of: the elbow then heads down and
            // a little out, where an elbow hangs under a rifle.
            if (haveRigElbow[s]) {
                const glm::vec3 a = IK::Position(globals[(size_t)upper]), b = IK::Position(globals[(size_t)lower]),
                                c = IK::Position(globals[(size_t)hand]);
                const glm::vec3 line = c - a;
                if (glm::dot(line, line) > 1e-8f) {
                    const glm::vec3 axis = glm::normalize(line);
                    const glm::vec3 rigBend = toModel3 * (rigElbow[s] - rigShoulder[s]);
                    const glm::vec3 have = (b - a) - axis * glm::dot(b - a, axis);
                    if (isSource) {
                        const glm::vec3 want = rigBend - axis * glm::dot(rigBend, axis);
                        const float rigLen = std::max(glm::length(rigBend), 1e-4f);
                        const float rigTrust = glm::smoothstep(0.05f, 0.2f, glm::length(want) / rigLen);
                        const glm::vec3 outward = s == 0 ? -kRight : kRight;
                        glm::vec3 pole = glm::vec3(0.0f, -1.0f, 0.0f) + outward * 0.5f;
                        pole -= axis * glm::dot(pole, axis);
                        // The support elbow hangs under the gun (kFirstPersonSupportHang of the way) while the hand is on it.
                        const float hang = s == 0 ? kFirstPersonSupportHang * (1.0f - std::clamp(m_FreeHand, 0.0f, 1.0f)) : 0.0f;
                        const float trust = rigTrust * (1.0f - hang);
                        glm::vec3 aim = trust > 0.0f ? glm::normalize(want) * trust : glm::vec3(0.0f);
                        if (glm::dot(pole, pole) > 1e-8f) aim += glm::normalize(pole) * (1.0f - trust);
                        if (glm::dot(aim, aim) > 1e-8f) {
                            const glm::vec3 aimTo = glm::normalize(glm::transpose(camToModel) * aim);
                            if (!m_HaveElbowAim[s]) { m_ElbowAim[s] = aimTo; m_HaveElbowAim[s] = true; }
                            const float angle = std::acos(std::clamp(glm::dot(m_ElbowAim[s], aimTo), -1.0f, 1.0f));
                            if (angle > 1e-5f) {
                                const float step = std::min(angle * Follow(dt, 0.06f), glm::radians(540.0f) * dt);
                                glm::vec3 turnAxis = glm::cross(m_ElbowAim[s], aimTo);
                                if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::cross(m_ElbowAim[s], up);
                                if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::vec3(1.0f, 0.0f, 0.0f);
                                m_ElbowAim[s] = glm::normalize(glm::angleAxis(step, glm::normalize(turnAxis)) * m_ElbowAim[s]);
                            }
                        }
                        swivelPlane[s] = 0.0f;
                        if (m_HaveElbowAim[s]) {
                            const glm::vec3 eased = camToModel * m_ElbowAim[s];
                            const glm::vec3 aimed = eased - axis * glm::dot(eased, axis);
                            const float upperLen = std::max(glm::length(b - a), 1e-4f);
                            const float bodyTrust = glm::smoothstep(0.02f, 0.08f, glm::length(have) / upperLen);
                            if (bodyTrust > 0.0f && glm::dot(aimed, aimed) > 1e-8f) {
                                const glm::vec3 hh = glm::normalize(have), ww = glm::normalize(aimed);
                                swivelPlane[s] = std::atan2(glm::dot(glm::cross(hh, ww), axis), glm::dot(hh, ww)) * bodyTrust * w;
                            }
                        }
                    }
                    if (std::abs(swivelPlane[s]) > 1e-4f) {
                        const glm::quat keep = IK::Rotation(globals[(size_t)hand]);
                        IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, c, &keep, 1.0f, swivelPlane[s]);
                    }
                }
            }
            // Elbow Clearance: the rig tucks its elbows to a narrower body; each swings out about its shoulder-to-hand
            // line until the arm around it clears the drawn torso, the hand staying on the gun. Eased in fast, out slower.
            if (meshChecks && !m_TorsoPoints.empty()) {
                if (isSource) {
                    if (meshNow) {
                    auto at = [&](int node) { return glm::vec3(rootW * glm::vec4(IK::Position(globals[(size_t)node]), 1.0f)); };
                    m_ElbowWant[s] = FirstPersonBodyElbowClearSwivel(m_TorsoPoints, at(upper), at(lower), at(hand), m_Set.ElbowClearance,
                                                                     glm::radians(90.0f), glm::radians(5.0f), m_ElbowClear[s]);
                    }
                    const float want = m_ElbowWant[s];
                    const float ease = std::abs(want) > std::abs(m_ElbowClear[s]) ? 0.03f : 0.15f;
                    m_ElbowClear[s] += (want - m_ElbowClear[s]) * Follow(dt, ease);
                }
                if (std::abs(m_ElbowClear[s]) > 1e-4f) {
                    const glm::quat keep = IK::Rotation(globals[(size_t)hand]);
                    IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, IK::Position(globals[(size_t)hand]), &keep, 1.0f, m_ElbowClear[s]);
                }
            }
        }
        // Fingers: each takes the rig finger's model-space rotation (its position stays on its own bone). They come
        // parents first, so only each finger's own global needs redoing for the next to read its parent's.
        for (const Finger& f : fingers) {
            const int i = driver ? f.Node->Driver : m.NodeIndex(f.Node->Name);
            if (i < 0 || i >= (int)pose.size()) continue;
            const int par = parents[(size_t)i];
            const glm::quat parentRot = par >= 0 ? IK::Rotation(globals[(size_t)par]) : kNone;
            pose[(size_t)i].R = glm::slerp(pose[(size_t)i].R, glm::normalize(glm::inverse(parentRot) * f.Rot), w);
            const glm::mat4 local = pose[(size_t)i].ToMatrix();
            globals[(size_t)i] = par >= 0 ? globals[(size_t)par] * local : local;
        }
        m.ApplyLocalPose(pose);
    }

    // 3b. A hand signal: the support hand comes off the gun and chops the way an order sends the squad (forward and a
    // little up, at arm's length), then goes back. The gun stays in the right hand meanwhile.
    m_SignalLeft = std::max(0.0f, m_SignalLeft - dt);
    const float signalWant = m_SignalLeft > 0.0f ? 1.0f : 0.0f;
    m_SignalWeight += (signalWant - m_SignalWeight) * Follow(dt, signalWant > m_SignalWeight ? 0.1f : 0.16f);
    if (m_SignalWeight > 1e-3f && m_DriverModel && m_DriverIndex >= 0) {
        Model& m = *m_DriverModel;
        IK::Pose& pose = m_Pose;
        pose = m.AppliedLocalPose();
        const int upper = m.NodeIndex(FPBody::kBoneUpperArm[0]), lower = m.NodeIndex(FPBody::kBoneLowerArm[0]),
                  hand = m.NodeIndex(FPBody::kBoneHand[0]);
        glm::vec3 dir = toModel3 * m_SignalDir;
        dir.y = 0.0f;
        if ((int)pose.size() == m.NodeCount() && upper >= 0 && lower >= 0 && hand >= 0 && glm::dot(dir, dir) > 1e-6f) {
            IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
            const glm::vec3 a = IK::Position(m_Globals[(size_t)upper]);
            const float armLen = glm::length(IK::Position(m_Globals[(size_t)lower]) - a) +
                                 glm::length(IK::Position(m_Globals[(size_t)hand]) - IK::Position(m_Globals[(size_t)lower]));
            const float sw = m_SignalWeight;
            const glm::vec3 reach = glm::normalize(glm::normalize(dir) * 0.9f + up * 0.2f);
            IK::SolveTwoBone(pose, m_DriverParents, m_Globals, upper, lower, hand, a + reach * armLen * 0.97f, nullptr, sw);
            // A flat blade of a hand, not the grip: the fingers straighten (to the bind pose, which is open) ...
            m.BindLocalPose(m_BindScratch);
            for (int i = hand + 1; i < m.NodeCount(); ++i) {
                int p = m_DriverParents[(size_t)i];
                while (p > hand) p = m_DriverParents[(size_t)p];
                if (p == hand) pose[(size_t)i].R = glm::slerp(pose[(size_t)i].R, m_BindScratch[(size_t)i].R, sw);
            }
            IK::ComputeGlobals(pose, m_DriverParents, m_Globals);
            // ... laid along the reach with the thumb up (measured on the hand's own fingers, whatever its bone axes).
            const int middle = m.NodeIndex("middle_01_l"), thumb = m.NodeIndex("thumb_01_l");
            if (middle >= 0 && thumb >= 0) {
                const glm::vec3 h = IK::Position(m_Globals[(size_t)hand]);
                const glm::vec3 along = IK::Position(m_Globals[(size_t)middle]) - h;
                if (glm::dot(along, along) > 1e-8f) {
                    const glm::quat lay(glm::normalize(along), reach);
                    glm::vec3 t = lay * (IK::Position(m_Globals[(size_t)thumb]) - h);
                    t -= reach * glm::dot(t, reach);
                    glm::vec3 u = up - reach * glm::dot(up, reach);
                    glm::quat roll(1.0f, 0.0f, 0.0f, 0.0f);
                    if (glm::dot(t, t) > 1e-8f && glm::dot(u, u) > 1e-8f) roll = glm::quat(glm::normalize(t), glm::normalize(u));
                    const glm::quat turn = glm::slerp(kNone, glm::normalize(roll * lay), sw);
                    IK::OffsetBone(pose, m_DriverParents, m_Globals, hand, glm::vec3(0.0f), turn, h);
                }
            }
            m.ApplyLocalPose(pose);
        }
    }

    // 3c. The arms' twist bones carry the solve's roll along the limbs (the player body's pass).
    if (m_DriverModel) m_ArmTwist.Apply(*m_DriverModel);

    // 4. On the sights the head comes down onto the stock: the neck tilts it toward the eye (the camera,
    // moved with the gun), by the cheek weld and at most Head Tilt.
    const float lockWant = std::clamp(gun->CheekWeld, 0.0f, 1.0f) * w;
    m_CheekWeld += (lockWant - m_CheekWeld) * Follow(dt, 0.1f);
    if (m_CheekWeld > 1e-3f && gun->HeadTiltDegrees > 0.0f && m_DriverModel) {
        glm::mat4 nk(1.0f), hd(1.0f);
        if (m_DriverModel->NodeTransform("neck_01", nk) && m_DriverModel->NodeTransform("head", hd)) {
            const glm::vec3 neck(nk[3]), headNow(hd[3]);
            const glm::vec3 eye = glm::vec3(toModel * glm::vec4(cam.Position + m_GunShift, 1.0f));
            // The head sits behind and below the eye (the player's Camera Offset: 8 cm up, 10 cm forward of the head).
            const glm::vec3 headWant = eye - (glm::vec3(0.0f, 0.08f, 0.0f) + kForward * 0.10f);
            m_Hold.EyeFromHead = eye - headNow;
            // A shooter's head comes over and down onto the stock - canted toward it and nodding forward - never back:
            // the sights sit only a few centimetres ahead of the head bone, and chased outright (the player's eye
            // offset is 10 cm ahead) the neck tipped the head back to look over the gun.
            const glm::vec3 from = headNow - neck;
            glm::vec3 to = headWant - neck;
            to.z = std::max(to.z, from.z + 0.03f);
            if (glm::length(from) > 1e-4f && glm::length(to) > 1e-4f) {
                glm::quat turn(glm::normalize(from), glm::normalize(to));
                const float most = glm::radians(gun->HeadTiltDegrees);
                if (const float angle = AngleOf(turn); angle > most && angle > 1e-5f) turn = glm::slerp(kNone, turn, most / angle);
                turn = glm::slerp(kNone, turn, m_CheekWeld);
                // ... onto the stock, not into it: the drawn head (skinned for the gun's own keep-out above) turned about
                // the neck must stay 2.5 cm off the gun; past that the weld gives way (the largest share that clears).
                if (meshNow && !m_HeadPoints.empty()) {
                    const glm::vec3 fwd = glm::length(gun->ForwardWorld) > 1e-4f ? glm::normalize(gun->ForwardWorld) : glm::vec3(rootW[2]);
                    const glm::vec3 a = gun->ButtWorld + m_GunShift, b = a + fwd * 0.6f;
                    const glm::vec3 pivot = glm::vec3(rootW * glm::vec4(neck, 1.0f));
                    const glm::quat yawRot = YawRotation(m_Yaw);
                    auto clears = [&](float share) {
                        const glm::quat r = yawRot * glm::slerp(kNone, turn, share) * glm::inverse(yawRot);
                        std::vector<glm::vec3>& moved = m_TorsoScratch;
                        moved.resize(m_HeadPoints.size());
                        for (size_t i = 0; i < m_HeadPoints.size(); ++i) moved[i] = pivot + r * (m_HeadPoints[i] - pivot);
                        return SegmentGap(moved, a, b) >= 0.025f;
                    };
                    float lo = 1.0f;
                    if (!clears(1.0f)) {
                        float hi = 1.0f;
                        lo = 0.0f;
                        for (int it = 0; it < 6; ++it) {
                            const float mid = 0.5f * (lo + hi);
                            (clears(mid) ? lo : hi) = mid;
                        }
                    }
                    m_WeldShare = lo;
                }
                if (meshChecks && m_WeldShare < 1.0f) turn = glm::slerp(kNone, turn, m_WeldShare);
                m_Hold.CheekTilt = glm::degrees(AngleOf(turn));
                const glm::quat half = glm::normalize(glm::slerp(kNone, turn, 0.5f));
                for (size_t pk = 0; pk < m_Models.size(); ++pk) {
                    if (!PieceTakes(pk, kSkinsNeck)) continue;
                    if (m_DriverIndex >= 0 && (int)pk != m_DriverIndex) continue; // SyncPieces passes it on
                    Model& m = *m_Models[pk];
                    IK::Pose& pose = m_Pose;
                    pose = m.AppliedLocalPose();
                    if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
                    const bool driver = (int)pk == m_DriverIndex && (int)m_DriverParents.size() == m.NodeCount();
                    const int n1 = driver ? m_DriverNeck : m.NodeIndex("neck_01");
                    const int n2 = driver ? m_DriverNeck2 : m.NodeIndex("neck_02");
                    if (n1 < 0) continue;
                    if (!driver) {
                        m_PieceParents.resize(pose.size());
                        for (int i = 0; i < (int)pose.size(); ++i) m_PieceParents[(size_t)i] = m.NodeParent(i);
                    }
                    const std::vector<int>& parents = driver ? m_DriverParents : m_PieceParents;
                    IK::ComputeGlobals(pose, parents, globals);
                    IK::OffsetBone(pose, parents, globals, n1, glm::vec3(0.0f), n2 >= 0 ? half : turn, IK::Position(globals[(size_t)n1]));
                    if (n2 >= 0) IK::OffsetBone(pose, parents, globals, n2, glm::vec3(0.0f), half, IK::Position(globals[(size_t)n2]));
                    m.ApplyLocalPose(pose);
                }
            }
        }
    }
    m_PiecesStale = true;
    SyncPieces();
    // The eye (perception, the weapon's next camera) follows the head as it now is.
    if (m_DriverModel) {
        glm::mat4 hd(1.0f);
        if (m_DriverModel->NodeTransform("head", hd)) m_Eye = glm::vec3(rootW * hd[3]);
    }
    return m_GunShift;
}

void NpcBody::TrackGun(const glm::vec3& position, float dt) {
    if (dt < 1e-4f) return;
    if (m_HaveGunPrev) {
        const glm::vec3 v = (position - m_GunPrev) / dt;
        // A teleport (a respawn, a held frame catching up) is no velocity worth keeping; the rest is eased over ~3 frames.
        if (glm::length(v) < 40.0f) m_GunVelocity += (v - m_GunVelocity) * std::min(1.0f, dt * 40.0f);
    }
    m_GunPrev = position;
    m_HaveGunPrev = true;
}

void NpcBody::ReleaseWeaponHold() {
    m_WeaponReleased = true;
    m_ArmsWeight = 0.0f;
    m_GunShift = glm::vec3(0.0f);
    m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
    m_ElbowClear[0] = m_ElbowClear[1] = 0.0f;
    m_HaveRigLine = false;
    m_CheekWeld = 0.0f;
    m_Hold = NpcHoldReport{};
}

NpcHoldReport NpcBody::MeasureHold(const World& world, entt::entity armsRig, const glm::vec3& butt, const glm::vec3& muzzle) const {
    NpcHoldReport r = m_Hold;
    std::vector<glm::vec3> head, torso;
    SkinnedRegion(world, Region::Head, head);
    SkinnedRegion(world, Region::Torso, torso);
    r.GunHead = SegmentGap(head, butt, muzzle);
    r.GunTorso = SegmentGap(torso, butt, muzzle);
    const auto& reg = world.Registry;
    const Model* rig = armsRig != entt::null && reg.valid(armsRig) && reg.all_of<RenderableComponent>(armsRig)
                           ? reg.get<RenderableComponent>(armsRig).ModelRef.get()
                           : nullptr;
    for (int s = 0; s < 2; ++s) {
        glm::vec3 a(0.0f), b(0.0f), c(0.0f);
        if (BoneWorld(FPBody::kBoneUpperArm[s], a) && BoneWorld(FPBody::kBoneLowerArm[s], b) && BoneWorld(FPBody::kBoneHand[s], c)) {
            r.Elbow[s] = torso.empty() ? -1.0f : FirstPersonBodyElbowGap(torso, a, b, c);
            // Against the grip the arms reached for (the 3P clips' where the weapon has them - their support hand isn't the
            // first-person rig's), else the rig's hand.
            glm::mat4 h(1.0f);
            if (m_HaveHandTarget[s]) r.Hand[s] = glm::length(m_HandTarget[s] - c);
            else if (rig && rig->NodeTransform(FPBody::kBoneHand[s], h)) r.Hand[s] = glm::length(glm::vec3(world.ComposeWorldTransform(armsRig) * h[3]) - c);
        }
    }
    return r;
}
