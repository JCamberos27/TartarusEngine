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
                RegionSkin& skin = r == 0 ? ps.Head : ps.Torso;
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
        }
        const RegionSkin& skin = region == Region::Head ? ps.Head : ps.Torso;
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

void NpcBody::RotateSpine(const glm::quat& modelDelta) {
    if (AngleOf(modelDelta) < 1e-6f) return;
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    for (size_t pk = 0; pk < m_Models.size(); ++pk) {
        if (!PieceTakes(pk, kSkinsSpine)) continue;
        Model& m = *m_Models[pk];
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        int spine[5], n = 0;
        for (int k = 0; k < 5; ++k)
            if (const int b = m.NodeIndex(FPBody::kBoneSpine[k]); b >= 0) spine[n++] = b;
        if (n == 0) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        const glm::quat step = glm::normalize(glm::slerp(kNone, modelDelta, 1.0f / (float)n));
        for (int k = 0; k < n; ++k) IK::OffsetBone(pose, parents, globals, spine[k], glm::vec3(0.0f), step, IK::Position(globals[(size_t)spine[k]]));
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
        !rig->NodeTransform(FPBody::kBoneUpperArm[0], rl) || !rig->NodeTransform(FPBody::kBoneUpperArm[1], rr)) {
        m_HaveRigOffset = false;
        return m_Eye; // no rig posed yet: the head bone
    }
    (void)rootW;
    if (!cameraBone.empty()) rig->NodeTransform(cameraBone, cb);
    // The rig's shoulders off its camera, in the camera's frame. The clips sway the shoulders about the
    // camera; only a slow drift of this is followed, so the sway shows on the arms and not on the gun.
    const glm::mat4 rigWorld = world.ComposeWorldTransform(armsRig);
    const glm::vec3 fromEye = glm::mat3(rigWorld) * (0.5f * (glm::vec3(rl[3]) + glm::vec3(rr[3])) - glm::vec3(cb[3]));
    const glm::vec3 inCamera(glm::dot(fromEye, cam.Right()), glm::dot(fromEye, cam.Up()), glm::dot(fromEye, cam.Front()));
    if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
    m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);
    // ... put on the body's own shoulders, a little slack toward the gun so the support arm never locks straight.
    const glm::vec3 shoulders = 0.5f * (sl + sr);
    return shoulders - (cam.Right() * m_RigEyeToShoulders.x + cam.Up() * m_RigEyeToShoulders.y +
                        cam.Front() * (m_RigEyeToShoulders.z + m_Set.ReachSlack));
}

glm::vec3 NpcBody::HoldWeapon(World& world, entt::entity armsRig, entt::entity weapon, const FirstPersonWorldGunInput* gun, const Camera& cam,
                              float dt, bool meshChecks) {
    if (!IsActive() || m_PoseExternal) return glm::vec3(0.0f);
    auto& reg = world.Registry;
    const bool haveRig = gun && armsRig != entt::null && reg.valid(armsRig) && reg.all_of<RenderableComponent>(armsRig) &&
                         reg.get<RenderableComponent>(armsRig).ModelRef;
    // Drawn, the arms take the rig's hands from the first frame; holstered they ease back to the clips'.
    if (haveRig) m_ArmsWeight = 1.0f;
    else m_ArmsWeight -= m_ArmsWeight * Follow(dt, 0.1f);
    m_Hold = NpcHoldReport{};
    if (!haveRig || m_ArmsWeight < 1e-3f) {
        m_GunShift = glm::vec3(0.0f);
        m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
        m_ElbowClear[0] = m_ElbowClear[1] = 0.0f;
        m_HaveRigLine = false;
        m_CheekWeld = 0.0f;
        return glm::vec3(0.0f);
    }
    const Model& rig = *reg.get<RenderableComponent>(armsRig).ModelRef;
    if ((int)rig.AppliedLocalPose().size() != rig.NodeCount()) return glm::vec3(0.0f);
    const glm::mat4 rootW = RootWorld();
    const glm::mat4 toModel = glm::inverse(rootW);
    const glm::mat3 toModel3(toModel);
    const glm::quat toModelRot = IK::Rotation(toModel);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const int armsK = ArmsPiece();

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
    const bool torsoPoints = meshNow && m_Set.ElbowClearance > 0.0f;
    if (torsoPoints) SkinnedRegion(world, Region::Torso, m_TorsoPoints);
    else if (!meshChecks) m_TorsoPoints.clear();
    if (!meshChecks) { m_MeshPush = glm::vec3(0.0f); m_WeldShare = 1.0f; }
    {
        glm::vec3 ul(0.0f), ur(0.0f), neck(0.0f), head(0.0f);
        if (BoneWorld(FPBody::kBoneUpperArm[0], ul) && BoneWorld(FPBody::kBoneUpperArm[1], ur) && BoneWorld("neck_01", neck) &&
            BoneWorld("head", head)) {
            glm::vec3 across = ur - ul;
            across.y = 0.0f;
            const glm::vec3 right = glm::length(across) > 1e-4f ? glm::normalize(across) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 front = glm::normalize(glm::cross(up, right));
            // At the low ready (not aiming) the butt sits lower and further in - the front of the shoulder, on the chest -
            // with the muzzle down; held in the aiming pocket the stock rode up beside the hood.
            const float ready = 1.0f - std::clamp(m_AimWeight, 0.0f, 1.0f);
            const glm::vec3 pocket = ur + right * (gun->Pocket.x - 0.03f * ready) + up * (gun->Pocket.y - 0.07f * ready) +
                                     front * (gun->Pocket.z + 0.04f * ready);
            glm::vec3 shift = (pocket - gun->ButtWorld) * std::clamp(gun->Shouldered, 0.0f, 1.0f);
            const glm::vec3 fwd = glm::length(gun->ForwardWorld) > 1e-4f ? glm::normalize(gun->ForwardWorld) : front;
            const struct { glm::vec3 Centre; float Radius; } keepOut[2] = {{neck, gun->NeckRadius}, {head + up * 0.07f, gun->HeadRadius}};
            for (int pass = 0; pass < 4; ++pass)
                for (const auto& k : keepOut) {
                    const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                    const glm::vec3 ab = b - a;
                    const float t = std::clamp(glm::dot(k.Centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                    glm::vec3 away = a + ab * t - k.Centre;
                    const float d = glm::length(away);
                    if (d >= k.Radius) continue;
                    away = d > 1e-4f ? away / d : right;
                    shift += away * (k.Radius - d);
                }
            // The drawn head, neck and hood: straight out of them from the hood sphere's centre, as far as it takes.
            const glm::vec3 beforeMesh = shift;
            if (meshNow && gun->MeshClearance > 0.0f) {
                SkinnedRegion(world, Region::Head, m_HeadPoints);
                const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                const glm::vec3 centre = keepOut[1].Centre;
                const glm::vec3 ab = b - a;
                const float t = std::clamp(glm::dot(centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                glm::vec3 away = a + ab * t - centre;
                away = glm::length(away) > 1e-4f ? glm::normalize(away) : right;
                const float room = std::max(0.0f, gun->MaxShift - glm::length(shift));
                shift += away * FirstPersonBodyClearPush(m_HeadPoints, a, b, away, gun->MeshClearance, room, 0.01f);
            }
            // ... and the drawn torso whenever the butt isn't in the pocket, straight out from the chest, by that weight.
            glm::vec3 chest(0.0f);
            if (torsoPoints && gun->MeshClearance > 0.0f && gun->TorsoKeepOut > 1e-3f && BoneWorld("spine_05", chest)) {
                const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                const glm::vec3 ab = b - a;
                const float t = std::clamp(glm::dot(chest - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                glm::vec3 away = a + ab * t - chest;
                away = glm::length(away) > 1e-4f ? glm::normalize(away) : front;
                const float room = std::max(0.0f, gun->MaxShift - glm::length(shift));
                shift += away * FirstPersonBodyClearPush(m_TorsoPoints, a, b, away, gun->MeshClearance, room, 0.01f) *
                         std::clamp(gun->TorsoKeepOut, 0.0f, 1.0f);
            }
            if (meshNow) m_MeshPush = shift - beforeMesh;
            else if (meshChecks) shift += m_MeshPush;
            if (const float len = glm::length(shift); len > gun->MaxShift && len > 1e-6f) shift *= gun->MaxShift / len;
            shiftTarget = shift * m_ArmsWeight;
        }
    }
    m_GunShift += (shiftTarget - m_GunShift) * Follow(dt, 0.05f);
    for (entt::entity e : {armsRig, weapon})
        if (e != entt::null && reg.valid(e)) reg.get<TransformComponent>(e).Position += m_GunShift;
    m_Hold.Shift = glm::length(m_GunShift);

    // 2. The rig's hands (and its arms, for the elbows' bend plane), with the gun where it now is.
    const glm::mat4 rigWorld = world.ComposeWorldTransform(armsRig); // (moved with the gun just above)
    glm::vec3 handPos[2]{};
    glm::quat handRot[2]{};
    bool haveHand[2] = {false, false};
    glm::vec3 rigShoulder[2]{}, rigElbow[2]{};
    bool haveRigElbow[2] = {false, false};
    struct Finger { std::string Name; glm::quat Rot; };
    std::vector<Finger> fingers;
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f), u(1.0f), l(1.0f);
        if (!rig.NodeTransform(FPBody::kBoneHand[s], h)) continue;
        const glm::mat4 w = rigWorld * h;
        handPos[s] = glm::vec3(w[3]);
        handRot[s] = IK::Rotation(w);
        haveHand[s] = true;
        if (rig.NodeTransform(FPBody::kBoneUpperArm[s], u) && rig.NodeTransform(FPBody::kBoneLowerArm[s], l)) {
            rigShoulder[s] = glm::vec3(rigWorld * u[3]);
            rigElbow[s] = glm::vec3(rigWorld * l[3]);
            haveRigElbow[s] = true;
        }
        const int rigHand = rig.NodeIndex(FPBody::kBoneHand[s]);
        for (int i = rigHand + 1; i < rig.NodeCount(); ++i) {
            int p = rig.NodeParent(i);
            while (p > rigHand) p = rig.NodeParent(p);
            if (p != rigHand) continue;
            glm::mat4 g(1.0f);
            if (rig.NodeTransform(rig.NodeName(i), g)) fingers.push_back({rig.NodeName(i), glm::normalize(toModelRot * IK::Rotation(rigWorld * g))});
        }
    }

    // 3. Every piece's arms onto them. The source (the arms piece) works out the shoulders' and chest's moves
    // and the elbows'; the rest take the same (one skeleton, one model space: the pieces sit on the root).
    std::vector<size_t> order;
    if (armsK >= 0) order.push_back((size_t)armsK);
    for (size_t k = 0; k < m_Models.size(); ++k)
        if ((int)k != armsK) order.push_back(k);
    const float shoulderMax = glm::radians(std::clamp(m_Set.ShoulderMaxAngle, 0.0f, 90.0f));
    const float leanMax = glm::radians(std::clamp(m_Set.ReachLeanMax, 0.0f, 60.0f));
    const glm::mat3 camToModel = toModel3 * glm::mat3(cam.Right(), cam.Up(), cam.Front()); // the camera's frame, model space
    glm::quat clavTurn[2] = {kNone, kNone}, chestTurn = kNone;
    float swivelPlane[2] = {0.0f, 0.0f};
    bool sourceDone = false;
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    const float w = m_ArmsWeight;
    for (size_t k : order) {
        if (k >= m_Models.size() || !m_Models[k]) continue;
        Model& m = *m_Models[k];
        const int upperN[2] = {m.NodeIndex(FPBody::kBoneUpperArm[0]), m.NodeIndex(FPBody::kBoneUpperArm[1])};
        if (upperN[0] < 0 && upperN[1] < 0) continue;
        const bool source = (int)k == armsK || (armsK < 0 && !sourceDone);
        const bool armsDrawn = source || PieceTakes(k, kSkinsArms);
        if (!armsDrawn && !(sourceDone && AngleOf(chestTurn) > 1e-5f && PieceTakes(k, kSkinsSpine))) continue;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
        if (!armsDrawn) {
            // Draws the chest and head but not the arms (a balaclava, a head): only the chest's lean.
            IK::ComputeGlobals(pose, parents, globals);
            int chest = -1;
            for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(FPBody::kBoneSpine[b]);
            if (chest >= 0) IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), chestTurn, IK::Position(globals[(size_t)chest]));
            m.ApplyLocalPose(pose);
            continue;
        }
        // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve then only
        // fixes the hands.
        FirstPersonBodyCopyArmShape(m, rig, w, pose, parents, m_Set.ClavicleFollow);
        IK::ComputeGlobals(pose, parents, globals);
        const bool isSource = !sourceDone;
        sourceDone = true;
        int chest = -1;
        for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(FPBody::kBoneSpine[b]);
        // Reach Lean: a hand still out of reach once its collarbone has turned all it may - the chest leans to it.
        if (chest >= 0 && isSource && leanMax > 0.0f) {
            glm::vec3 from(0.0f), to(0.0f);
            float worst = 0.0f;
            for (int s = 0; s < 2; ++s) {
                if (!haveHand[s]) continue;
                const int upper = upperN[s], lower = m.NodeIndex(FPBody::kBoneLowerArm[s]), hand = m.NodeIndex(FPBody::kBoneHand[s]),
                          clav = m.NodeIndex(FPBody::kBoneClavicle[s]);
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
            const int upper = upperN[s], lower = m.NodeIndex(FPBody::kBoneLowerArm[s]), hand = m.NodeIndex(FPBody::kBoneHand[s]);
            if (upper < 0 || lower < 0 || hand < 0) continue;
            const int clav = m.NodeIndex(FPBody::kBoneClavicle[s]);
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
                        glm::vec3 aim = rigTrust > 0.0f ? glm::normalize(want) * rigTrust : glm::vec3(0.0f);
                        if (glm::dot(pole, pole) > 1e-8f) aim += glm::normalize(pole) * (1.0f - rigTrust);
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
        // Fingers: each takes the rig finger's model-space rotation (its position stays on its own bone).
        for (const Finger& f : fingers) {
            const int i = m.NodeIndex(f.Name);
            if (i < 0) continue;
            const int par = parents[(size_t)i];
            const glm::quat parentRot = par >= 0 ? IK::Rotation(globals[(size_t)par]) : kNone;
            pose[(size_t)i].R = glm::slerp(pose[(size_t)i].R, glm::normalize(glm::inverse(parentRot) * f.Rot), w);
            IK::RefreshGlobals(pose, parents, globals, i);
        }
        m.ApplyLocalPose(pose);
    }

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
                    Model& m = *m_Models[pk];
                    IK::Pose pose = m.AppliedLocalPose();
                    if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
                    const int n1 = m.NodeIndex("neck_01"), n2 = m.NodeIndex("neck_02");
                    if (n1 < 0) continue;
                    parents.resize(pose.size());
                    for (int i = 0; i < (int)pose.size(); ++i) parents[(size_t)i] = m.NodeParent(i);
                    IK::ComputeGlobals(pose, parents, globals);
                    IK::OffsetBone(pose, parents, globals, n1, glm::vec3(0.0f), n2 >= 0 ? half : turn, IK::Position(globals[(size_t)n1]));
                    if (n2 >= 0) IK::OffsetBone(pose, parents, globals, n2, glm::vec3(0.0f), half, IK::Position(globals[(size_t)n2]));
                    m.ApplyLocalPose(pose);
                }
            }
        }
    }
    // The eye (perception, the weapon's next camera) follows the head as it now is.
    if (m_DriverModel) {
        glm::mat4 hd(1.0f);
        if (m_DriverModel->NodeTransform("head", hd)) m_Eye = glm::vec3(rootW * hd[3]);
    }
    return m_GunShift;
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
            glm::mat4 h(1.0f);
            if (rig && rig->NodeTransform(FPBody::kBoneHand[s], h)) r.Hand[s] = glm::length(glm::vec3(world.ComposeWorldTransform(armsRig) * h[3]) - c);
        }
    }
    return r;
}
