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

// FirstPersonBody's diagnostics: what the camera sees of the body (the probes the npc tests and the Inspector read) and
// the posture readings (BodyDebug::Info, the motion matching layer's per-bone offsets).

using namespace FirstPersonBodyDetail;

void FirstPersonBody::CaptureMmClip() {
    const Model* m = DriverModel();
    const MotionDatabase* db = m_Mm.Database();
    if (!m || !db) return;
    const auto& bones = db->Stream().Bones();
    if (m_MmLayerNodes.size() != bones.size()) {
        m_MmLayerNodes.assign(bones.size(), -1);
        for (size_t i = 0; i < bones.size(); ++i) m_MmLayerNodes[i] = m->NodeIndex(bones[i]);
    }
    const IK::Pose& pose = m->AppliedLocalPose();
    m_MmClipRot.resize(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        const int n = m_MmLayerNodes[i];
        m_MmClipRot[i] = n >= 0 && n < (int)pose.size() ? pose[(size_t)n].R : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}
bool FirstPersonBody::TrunkAngles(float (&out)[9]) const {
    const Model* m = DriverModel();
    glm::mat4 p(1.0f);
    glm::quat chest;
    if (!m || !m_HaveBindPosture || !ChestTurn(chest) || !m->NodeTransform(Bone(FPBody::kBonePelvis), p)) return false;
    const glm::quat pelvis = glm::normalize(IK::Rotation(p) * glm::inverse(m_BindPelvis));
    const auto angles = [](const glm::quat& q, float* o) { // yaw (+ toward the body's right), roll (+ toward its left), pitch (+ forward)
        const glm::vec3 f = q * kForward, u = q * glm::vec3(0.0f, 1.0f, 0.0f);
        o[0] = glm::degrees(std::atan2(glm::dot(f, kRight), glm::dot(f, kForward)));
        o[1] = glm::degrees(std::atan2(glm::dot(u, -kRight), u.y));
        o[2] = glm::degrees(std::atan2(glm::dot(u, kForward), u.y));
    };
    angles(pelvis, out);
    angles(chest, out + 3);
    angles(glm::normalize(glm::inverse(pelvis) * chest), out + 6);
    return true;
}
void FirstPersonBody::PostureMetrics(const World& world, const Camera& camera, float dt) {
    BodyDebug::Snapshot& d = BodyDebug::Info();
    if (dt > 0.0f && m_HaveMetricYaw) d.BodyYawRateDeg = glm::degrees(FirstPersonBodyWrapAngle(m_Yaw - m_MetricYaw)) / dt;
    m_MetricYaw = m_Yaw;
    m_HaveMetricYaw = true;
    const Model* m = DriverModel();
    if (!m || !world.Registry.valid(m_Body)) return;
    TrunkAngles(d.TrunkDeg);
    if (m_Mm.Active() && m_MmClipRot.size() == m_MmLayerNodes.size() && m_Mm.Database()) {
        const IK::Pose& pose = m->AppliedLocalPose();
        d.LayerDeg.resize(m_MmLayerNodes.size());
        for (size_t i = 0; i < m_MmLayerNodes.size(); ++i) {
            const int n = m_MmLayerNodes[i];
            d.LayerDeg[i] = n >= 0 && n < (int)pose.size() ? glm::degrees(MotionMath::QuatAngle(m_MmClipRot[i], pose[(size_t)n].R)) : 0.0f;
        }
        d.LayerBones = &m_Mm.Database()->Stream().Bones();
    }
    glm::quat chest;
    if (ChestTurn(chest)) {
        const glm::vec3 up = chest * glm::vec3(0.0f, 1.0f, 0.0f);
        d.ChestPitchDeg = glm::degrees(std::atan2(glm::dot(up, kForward), up.y));
        d.ChestRollDeg = glm::degrees(std::atan2(glm::dot(up, -kRight), up.y));
        glm::mat4 h(1.0f);
        if (m_HeadNode >= 0 && m->NodeTransform(m->NodeName(m_HeadNode), h)) {
            // The head's turn off the bind pose, in the chest's frame (the chest's own turn taken out).
            const glm::quat head = glm::normalize(IK::Rotation(h) * glm::inverse(m_BindHead));
            const glm::vec3 hu = glm::inverse(chest) * (head * glm::vec3(0.0f, 1.0f, 0.0f));
            d.HeadPitchDeg = glm::degrees(std::atan2(glm::dot(hu, kForward), hu.y));
            const glm::vec3 hw = head * glm::vec3(0.0f, 1.0f, 0.0f);
            d.HeadWorldPitchDeg = glm::degrees(std::atan2(glm::dot(hw, kForward), hw.y));
        }
    }
    const float scale = std::max(world.Registry.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    const glm::quat yaw = YawRotation(m_Yaw);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 hand(1.0f), sh(1.0f);
        if (!m->NodeTransform(Bone(FPBody::kBoneHand[s]), hand) || !m->NodeTransform(Bone(FPBody::kBoneUpperArm[s]), sh)) continue;
        d.HandOverShoulder[s] = scale * (hand[3].y - sh[3].y);
        const glm::vec3 handWorld = m_Feet + yaw * (scale * glm::vec3(hand[3]));
        const glm::vec3 toHand = handWorld - camera.Position;
        d.HandToEye[s] = glm::length(toHand);
        d.HandInView[s] = d.HandToEye[s] < 0.8f && d.HandToEye[s] > 1e-4f &&
                          glm::dot(toHand / d.HandToEye[s], camera.Front()) > std::cos(glm::radians(35.0f));
    }
    if (glm::mat4 pv(1.0f); m->NodeTransform(Bone(FPBody::kBonePelvis), pv)) d.PelvisHeight = scale * pv[3].y;
    glm::mat4 fl(1.0f), fr(1.0f);
    if (m->NodeTransform(Bone(FPBody::kBoneFoot[0]), fl) && m->NodeTransform(Bone(FPBody::kBoneFoot[1]), fr))
        d.FeetGap = scale * glm::dot(glm::vec3(fl[3]) - glm::vec3(fr[3]), -kRight);
}
void FirstPersonBody::InsideProbe(World& world, const Camera& camera) {
    auto& info = BodyDebug::Info();
    info.ProbeRays = -1;
    if (++m_InsideProbeTick % 3) return; // every third frame (the grid costs ~0.2 s: test runs only)
    auto& reg = world.Registry;
    const glm::vec3 eye = camera.Position, f = camera.Front(), r = camera.Right(), u = camera.Up();
    glm::vec3 flatRight(r.x, 0.0f, r.z);
    flatRight = glm::dot(flatRight, flatRight) > 1e-8f ? glm::normalize(flatRight) : glm::vec3(1.0f, 0.0f, 0.0f);
    const float nearPlane = std::max(camera.NearPlane, 0.01f), tanHalf = std::tan(glm::radians(camera.Fov) * 0.5f), kAspect = 16.0f / 9.0f;
    auto bit = [](const std::uint32_t (&bits)[16], int b) { return b >= 0 && b < 16 * 32 && ((bits[b >> 5] >> (b & 31)) & 1u) != 0u; };
    struct Tri { glm::vec3 A, B, C; int Piece, Bone; };
    thread_local std::vector<Tri> tris;
    thread_local std::vector<glm::vec3> pos;
    thread_local std::vector<char> drawn;
    thread_local std::vector<int> top;
    tris.clear();
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (!m_Models[k] || !reg.valid(e) || reg.all_of<InactiveTag>(e) || reg.all_of<ViewModelTag>(e)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(e);
        const auto* tag = reg.try_get<PlayerBodyTag>(e);
        if (!rc || !tag || tag->CameraHidden || rc->CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        const Model& m = *m_Models[k];
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(e);
        const std::vector<std::uint8_t>* collar = tag->CollarBits.get();
        size_t vertexIndex = 0;
        for (int mi = 0; mi < m.MeshCount(); ++mi) {
            const auto& verts = m.MeshSkinVertices(mi);
            const auto& idx = m.Mesh(mi).LocalIndices();
            pos.resize(verts.size());
            drawn.assign(verts.size(), 0);
            top.assign(verts.size(), -1);
            for (size_t vi = 0; vi < verts.size(); ++vi, ++vertexIndex) {
                const ModelMesh::SkinVertex& v = verts[vi];
                const bool inCollar = collar && vertexIndex < collar->size() && (*collar)[vertexIndex];
                glm::mat4 skin(0.0f);
                float total = 0.0f, hideBones = 0.0f, head = 0.0f, sleeve = 0.0f, best = 0.0f;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                    const int b = v.BoneIDs[i];
                    if (b < 0) continue;
                    const float w = v.Weights[i];
                    skin += m.FinalBoneMatrix(b) * w;
                    total += w;
                    for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                        if (tag->CameraHideBones[h] == b) hideBones += w;
                    if (bit(tag->HeadBones, b)) head += w;
                    if (bit(tag->SleeveBones, b)) sleeve += w;
                    if (w > best) { best = w; top[vi] = b; }
                }
                if (total <= 1e-4f) continue;
                pos[vi] = glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f));
                if (glm::length(glm::vec3(skin[0])) < 0.5f * total) continue;  // a Camera Hidden Bone's collapse
                if (tag->SleevesInViewModel && sleeve > 0.5f) continue;         // (drawn in the view-model pass)
                if (inCollar || hideBones > 0.5f || (tag->HasHeadBones && head > 0.5f) || (tag->SleevesHidden && sleeve > 0.5f)) continue;
                if (FirstPersonBodyNearHidden(pos[vi], eye, flatRight, tag->NearHide, tag->NearHideWidth)) continue;
                drawn[vi] = 1;
            }
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                const unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
                if (a >= verts.size() || b >= verts.size() || c >= verts.size() || !drawn[a] || !drawn[b] || !drawn[c]) continue;
                // Only what can be in the view (its centroid within the frustum widened by 20 %, within 3 m).
                const glm::vec3 d = (pos[a] + pos[b] + pos[c]) / 3.0f - eye;
                const float z = glm::dot(d, f);
                if (z < -0.1f || z > 3.0f) continue;
                const float zz = std::max(z, 0.1f);
                if (std::abs(glm::dot(d, u)) > 1.2f * zz * tanHalf + 0.1f || std::abs(glm::dot(d, r)) > 1.2f * zz * tanHalf * kAspect + 0.1f) continue;
                tris.push_back({pos[a], pos[b], pos[c], (int)k, top[a]});
            }
        }
    }
    constexpr int kW = 24, kH = 14;
    // Each ray tests only the triangles whose screen footprint holds its grid point: a triangle wholly in front of the eye
    // projects to the triangle of its projected corners, so a ray through screen point (sx, sy) can only hit one whose
    // projected box holds it; one reaching behind the eye is tested by every ray. Each cell keeps the triangles' order, so the
    // nearest hit (and a tie) is the brute force's. (Brute force - every ray against every triangle - was two thirds of an
    // mmplayer run's CPU: ~0.2 s a probe on the look-down legs.)
    thread_local std::vector<std::vector<int>> cells;
    cells.resize((size_t)kW * kH);
    for (auto& c : cells) c.clear();
    {
        const float toX = 1.0f / (tanHalf * kAspect), toY = 1.0f / tanHalf;
        for (int ti = 0; ti < (int)tris.size(); ++ti) {
            const Tri& t = tris[(size_t)ti];
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            bool behind = false;
            for (const glm::vec3& v : {t.A, t.B, t.C}) {
                const glm::vec3 d = v - eye;
                const float z = glm::dot(d, f);
                if (z <= 1e-4f) { behind = true; break; }
                const float px = glm::dot(d, r) / z * toX, py = glm::dot(d, u) / z * toY; // -1..1 across the grid
                lo[0] = std::min(lo[0], px); hi[0] = std::max(hi[0], px);
                lo[1] = std::min(lo[1], py); hi[1] = std::max(hi[1], py);
            }
            // Grid point x sits at screen ((x + 0.5) / kW * 2 - 1): the points inside [lo, hi] (widened a hair, so rounding never
            // drops a triangle whose edge runs through a grid point - a wider box only adds rays that miss).
            for (int a = 0; a < 2; ++a) { lo[a] -= 1e-3f; hi[a] += 1e-3f; }
            const int x0 = behind ? 0 : std::max(0, (int)std::ceil((lo[0] + 1.0f) * 0.5f * kW - 0.5f));
            const int x1 = behind ? kW - 1 : std::min(kW - 1, (int)std::floor((hi[0] + 1.0f) * 0.5f * kW - 0.5f));
            const int y0 = behind ? 0 : std::max(0, (int)std::ceil((lo[1] + 1.0f) * 0.5f * kH - 0.5f));
            const int y1 = behind ? kH - 1 : std::min(kH - 1, (int)std::floor((hi[1] + 1.0f) * 0.5f * kH - 0.5f));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) cells[(size_t)y * kW + x].push_back(ti);
        }
    }
    int hits = 0, inside = 0;
    float sumX = 0.0f, sumY = 0.0f; // where on the screen the inside is seen (-1..1, + right / up)
    std::map<std::pair<int, int>, int> what;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float sx = ((x + 0.5f) / kW * 2.0f - 1.0f) * tanHalf * kAspect, sy = ((y + 0.5f) / kH * 2.0f - 1.0f) * tanHalf;
            const glm::vec3 dir = glm::normalize(f + r * sx + u * sy);
            const float tMin = nearPlane / std::max(glm::dot(dir, f), 1e-3f);
            float bestT = 1e9f;
            const Tri* bestTri = nullptr;
            bool back = false;
            for (const int ti : cells[(size_t)y * kW + x]) {
                const Tri& t = tris[(size_t)ti];
                // Moller-Trumbore, both sides.
                const glm::vec3 e1 = t.B - t.A, e2 = t.C - t.A, p = glm::cross(dir, e2);
                const float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-10f) continue;
                const float inv = 1.0f / det;
                const glm::vec3 s = eye - t.A;
                const float bu = glm::dot(s, p) * inv;
                if (bu < 0.0f || bu > 1.0f) continue;
                const glm::vec3 q = glm::cross(s, e1);
                const float bv = glm::dot(dir, q) * inv;
                if (bv < 0.0f || bu + bv > 1.0f) continue;
                const float tt = glm::dot(e2, q) * inv;
                if (tt < tMin || tt >= bestT) continue;
                bestT = tt;
                bestTri = &t;
                back = glm::dot(glm::cross(e1, e2), dir) > 0.0f; // counter-clockwise front faces: a back face faces along the ray
            }
            if (!bestTri) continue;
            ++hits;
            if (back) {
                ++inside;
                ++what[{bestTri->Piece, bestTri->Bone}];
                sumX += sx / (tanHalf * kAspect);
                sumY += sy / tanHalf;
            }
        }
    info.ProbeRays = kW * kH;
    info.ProbeRayHits = hits;
    info.ProbeInsideRays = inside;
    info.ProbeInsideWhat.clear();
    info.ProbeInsideAt = inside > 0 ? glm::vec2(sumX, sumY) / (float)inside : glm::vec2(0.0f);
    int most = 0;
    for (const auto& [key, n] : what)
        if (n > most) {
            most = n;
            const Model& m = *m_Models[(size_t)key.first];
            std::string bone = "?";
            for (int i = 0; i < m.NodeCount(); ++i)
                if (m.BoneId(m.NodeName(i)) == key.second) { bone = m.NodeName(i); break; }
            const entt::entity pe = m_Pieces[(size_t)key.first];
            info.ProbeInsideWhat = (reg.all_of<NameComponent>(pe) ? reg.get<NameComponent>(pe).Name : std::string("piece")) + "/" + bone;
        }
}
// Which of the body's pieces the camera actually sees nearest the eye, and the bone that moves it there. Every
// piece is skinned on the CPU with this frame's pose and put through the renderer's own hides (Camera Hidden
// Bones, Near Hide, clothing's head, sleeve and collar masks); what's left
// inside the view's frustum (the world's FOV, or the view-model pass's for the arms) is measured. Overlay
// only: the Inspector's readout, and a log line whenever something is within 12 cm of the eye.
void FirstPersonBody::CameraProbe(World& world, const Camera& camera, float viewModelFov, float dt) {
    auto& reg = world.Registry;
    const glm::vec3 eye = camera.Position, f = camera.Front(), r = camera.Right(), u = camera.Up();
    constexpr float kAspect = 16.0f / 9.0f, kClose = 0.12f;
    glm::vec3 flatRight(r.x, 0.0f, r.z);
    flatRight = glm::dot(flatRight, flatRight) > 1e-8f ? glm::normalize(flatRight) : glm::vec3(1.0f, 0.0f, 0.0f);
    auto bit = [](const std::uint32_t (&bits)[16], int b) { return b >= 0 && b < 16 * 32 && ((bits[b >> 5] >> (b & 31)) & 1u) != 0u; };

    float bestDepth = 1e9f, bestDist = 0.0f;
    int bestBone = -1, close = 0;
    size_t bestPiece = 0;
    bool bestViewModel = false;
    int cutNear = 0, cutCollar = 0, cutBones = 0, cutHead = 0;
    const auto inView = [&](const glm::vec3& d, bool viewModel) {
        const float z = glm::dot(d, f);
        if (z <= 0.01f) return false;
        const float tanHalf = std::tan(glm::radians(viewModel ? viewModelFov : camera.Fov) * 0.5f);
        return std::abs(glm::dot(d, u)) <= z * tanHalf && std::abs(glm::dot(d, r)) <= z * tanHalf * kAspect;
    };
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (!m_Models[k] || !reg.valid(e) || reg.all_of<InactiveTag>(e)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(e);
        const auto* tag = reg.try_get<PlayerBodyTag>(e);
        if (!rc || !tag || rc->CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
        if (tag->CameraHidden) { // a Hidden Part (the head): what of it is in the view is a cut you see into
            const Model& hm = *m_Models[k];
            const glm::mat4 hw = world.ComposeWorldTransform(e);
            for (int mi = 0; mi < hm.MeshCount(); ++mi)
                for (const ModelMesh::SkinVertex& v : hm.MeshSkinVertices(mi)) {
                    glm::mat4 skin(0.0f);
                    float total = 0.0f;
                    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                        if (v.BoneIDs[i] >= 0) { skin += hm.FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; total += v.Weights[i]; }
                    if (total > 1e-4f && inView(glm::vec3(hw * skin * glm::vec4(v.Position, 1.0f)) - eye, false)) ++cutHead;
                }
            continue;
        }
        const Model& m = *m_Models[k];
        const glm::mat4 pieceWorld = world.ComposeWorldTransform(e);
        const bool pieceViewModel = reg.all_of<ViewModelTag>(e) && viewModelFov > 0.0f;
        const std::vector<std::uint8_t>* collar = tag->CollarBits.get();
        size_t vertexIndex = 0; // across the sub-meshes, as the collar's bits count them
        for (int mi = 0; mi < m.MeshCount(); ++mi) {
            for (const ModelMesh::SkinVertex& v : m.MeshSkinVertices(mi)) {
                const bool inCollar = collar && vertexIndex < collar->size() && (*collar)[vertexIndex];
                ++vertexIndex;
                glm::mat4 skin(0.0f);
                float total = 0.0f, hideBones = 0.0f, head = 0.0f, sleeve = 0.0f, top = 0.0f;
                int bone = -1;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                    const int b = v.BoneIDs[i];
                    if (b < 0) continue;
                    const float w = v.Weights[i];
                    skin += m.FinalBoneMatrix(b) * w;
                    total += w;
                    for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                        if (tag->CameraHideBones[h] == b) hideBones += w;
                    if (bit(tag->HeadBones, b)) head += w;
                    if (bit(tag->SleeveBones, b)) sleeve += w;
                    if (w > top) { top = w; bone = b; }
                }
                if (total <= 1e-4f) continue;
                const glm::vec3 p = glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f));
                const glm::vec3 d = p - eye;
                // Skin mostly on a Hidden Bone is collapsed to a point (Model::SetHiddenNodes): nothing is drawn there.
                if (glm::length(glm::vec3(skin[0])) < 0.5f * total) {
                    if (!pieceViewModel && inView(d, false)) ++cutBones;
                    continue;
                }
                // Which pass draws it, and so which hides and which FOV apply (SceneRenderer).
                const bool sleeveInViewModel = tag->SleevesInViewModel && sleeve > 0.5f;
                const bool viewModel = pieceViewModel || sleeveInViewModel;
                if (!viewModel) {
                    if (inCollar || hideBones > 0.5f) { if (inView(d, false)) ++cutCollar; continue; }
                    if (tag->HasHeadBones && head > 0.5f) { if (inView(d, false)) ++cutHead; continue; }
                    if (tag->SleevesHidden && sleeve > 0.5f) continue;
                }
                if (pieceViewModel) {
                    // The arms piece: drawn whole in the view-model pass.
                } else if (sleeveInViewModel) {
                    if (glm::dot(d, d) < tag->NearHide * tag->NearHide) { if (inView(d, true)) ++cutNear; continue; }
                } else if (FirstPersonBodyNearHidden(p, eye, flatRight, tag->NearHide, tag->NearHideWidth)) {
                    if (inView(d, false)) ++cutNear;
                    continue;
                }
                const float z = glm::dot(d, f);
                if (z <= 0.0f) continue;
                const float tanHalf = std::tan(glm::radians(viewModel ? viewModelFov : camera.Fov) * 0.5f);
                if (std::abs(glm::dot(d, u)) > z * tanHalf || std::abs(glm::dot(d, r)) > z * tanHalf * kAspect) continue;
                if (z < kClose) ++close;
                if (z < bestDepth) {
                    bestDepth = z; bestDist = glm::length(d); bestBone = bone; bestPiece = k; bestViewModel = viewModel;
                }
            }
        }
    }

    auto& info = BodyDebug::Info();
    info.ProbeCutNear = cutNear;
    info.ProbeCutCollar = cutCollar;
    info.ProbeCutBones = cutBones;
    info.ProbeCutHead = cutHead;
    info.ProbeDepth = bestDepth < 1e8f ? bestDepth : 0.0f;
    info.ProbePiece.clear();
    info.ProbeBone.clear();
    if (bestDepth >= 1e8f) return;
    const Model& m = *m_Models[bestPiece];
    for (int i = 0; i < m.NodeCount(); ++i)
        if (m.BoneId(m.NodeName(i)) == bestBone) { info.ProbeBone = m.NodeName(i); break; }
    if (reg.all_of<NameComponent>(m_Pieces[bestPiece])) info.ProbePiece = reg.get<NameComponent>(m_Pieces[bestPiece]).Name;
    info.ProbeViewModel = bestViewModel;
    // A line while something is close, at most five a second (and at once for anything nearer than the last).
    m_ProbeLogTimer -= dt;
    if (bestDepth < kClose && (m_ProbeLogTimer <= 0.0f || bestDepth < m_ProbeLogged - 0.01f)) {
        char line[320];
        std::snprintf(line, sizeof(line),
                      "Camera probe: '%s' in view %.1f cm ahead of the eye (%.1f cm away), mostly moved by '%s', %d vertices within %.0f cm - %s pass.",
                      info.ProbePiece.c_str(), bestDepth * 100.0f, bestDist * 100.0f, info.ProbeBone.c_str(), close, kClose * 100.0f,
                      bestViewModel ? "view-model" : "world");
        Log::Info(line);
        m_ProbeLogTimer = 0.2f;
        m_ProbeLogged = bestDepth;
    }
    if (m_ProbeLogTimer <= 0.0f) m_ProbeLogged = 1e9f;
}
