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

// FirstPersonBody: the eye and the body's own mesh - how far the eye moves forward so the near plane doesn't cut into the
// torso looking down, the look-down limit, and hands kept off the eye.

using namespace FirstPersonBodyDetail;

namespace {

constexpr float kHandEyeMin = 0.30f, kHandEyeSoft = 0.06f; // m (KeepHandsFromEye)

// Looking down, the drawn upper torso is kept this far past the camera's near plane (m), the eye moved forward over it by
// at most kEyeBodyPushMax (FirstPersonBody::EyeBodyPush).
constexpr float kEyeBodyClearance = 0.03f, kEyeBodyPushMax = 0.10f; // (past 10 cm the camera hangs ahead of the body: the
                                                                     // look-down limit takes over)
constexpr float kNeckRimReach = 0.12f; // m: the torso's open border this close to the neck is the neck opening
constexpr float kNeckRimMargin = 0.1f;  // the view's frustum widened this much (of its half-width) for the rim
// The look-down limit: never above this (degrees; past it the opening isn't the eye's to fix), held over a stride.
constexpr float kLookLimitHighest = -45.0f;

} // namespace

void FirstPersonBody::KeepHandsFromEye(const glm::vec3& eyeModel, float weight) {
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    // The targets from the driver (every piece is one skeleton): a hand closer than the minimum goes out along the line
    // from the eye, to a soft maximum of its distance and the minimum.
    const Model* driver = DriverModel();
    if (!driver) return;
    glm::vec3 target[2];
    bool push[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f);
        if (!driver->NodeTransform(Bone(FPBody::kBoneHand[s]), h)) continue;
        const glm::vec3 d = glm::vec3(h[3]) - eyeModel;
        const float dist = glm::length(d);
        if (dist < 1e-4f || dist > kHandEyeMin + 3.0f * kHandEyeSoft) continue;
        const float x = dist - kHandEyeMin;
        const float want = kHandEyeMin + 0.5f * (x + std::sqrt(x * x + kHandEyeSoft * kHandEyeSoft)); // soft max(dist, min)
        if (want - dist < 1e-4f) continue;
        target[s] = eyeModel + d / dist * want;
        push[s] = true;
        BodyDebug::Info().HandEyePush[s] = (want - dist) * std::clamp(weight, 0.0f, 1.0f); // (model units: metres on the body)
    }
    if (!push[0] && !push[1]) return;
    for (const auto& mp : m_Models) {
        if (!mp) continue;
        Model& m = *mp;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        bool changed = false;
        for (int s = 0; s < 2; ++s) {
            if (!push[s]) continue;
            const int u = m.NodeIndex(Bone(FPBody::kBoneUpperArm[s])), l = m.NodeIndex(Bone(FPBody::kBoneLowerArm[s])),
                      e = m.NodeIndex(Bone(FPBody::kBoneHand[s]));
            if (u < 0 || l < 0 || e < 0) continue;
            const glm::quat handRot = IK::Rotation(globals[(size_t)e]);
            changed |= IK::SolveTwoBone(pose, parents, globals, u, l, e, target[s], &handRot, std::clamp(weight, 0.0f, 1.0f));
        }
        if (changed) m.ApplyLocalPose(pose);
    }
}
float FirstPersonBody::EyeBodyPush(World& world, const Camera& camera, const glm::vec3& eye, const glm::vec3& flat, float& lookLimitDeg) {
    lookLimitDeg = -89.0f;
    auto& reg = world.Registry;
    if (!m_EyeClearBuilt) {
        // The body's own pieces (not the arms, clothing or a Hidden Part): the vertices mostly on the upper spine and the
        // neck that the camera pass draws (not on a Camera Hidden Bone or the head) - every one, thinned to ~600; and the neck
        // opening's rim: the mesh's open border (its vertices welded by bind position - UV seams split them) within
        // kNeckRimReach of the bind neck_01 -> head segment. Clothing adds only its collar cut's edge to the rim.
        m_EyeClearBuilt = true;
        m_EyeClearPoints.clear();
        std::vector<EyeClearPoint> rim;
        for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
            const entt::entity e = m_Pieces[k];
            if (!m_Models[k] || !reg.valid(e) || IsArmsPiece(reg, e) || reg.all_of<ViewModelTag>(e)) continue;
            const auto* tag = reg.try_get<PlayerBodyTag>(e);
            if (!tag || tag->CameraHidden) continue;
            if (tag->Clothing) {
                // Clothing's collar is cut away for the camera (CollarBits): the cut's edge is an opening too, and through it
                // a jacket's lining shows looking down. Its rim = the drawn vertices of the triangles the cut splits.
                const std::vector<std::uint8_t>* collar = tag->CollarBits.get();
                if (!collar) continue;
                const Model& m = *m_Models[k];
                size_t base = 0;
                for (int mi = 0; mi < m.MeshCount(); ++mi) {
                    const size_t count = m.MeshSkinVertices(mi).size();
                    const auto& idx = m.Mesh(mi).LocalIndices();
                    const auto hidden = [&](std::uint32_t vi) { return base + vi < collar->size() && (*collar)[base + vi] != 0; };
                    std::vector<char> onCut(count, 0);
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        const std::uint32_t tv[3] = {idx[t], idx[t + 1], idx[t + 2]};
                        if (tv[0] >= count || tv[1] >= count || tv[2] >= count) continue;
                        const int cut = (int)hidden(tv[0]) + (int)hidden(tv[1]) + (int)hidden(tv[2]);
                        if (cut == 0 || cut == 3) continue;
                        for (const std::uint32_t vi : tv)
                            if (!hidden(vi)) onCut[vi] = 1;
                    }
                    for (std::uint32_t vi = 0; vi < (std::uint32_t)count; ++vi)
                        if (onCut[vi]) rim.push_back({k, mi, vi, true});
                    base += count;
                }
                continue;
            }
            const Model& m = *m_Models[k];
            int want[4];
            const char* names[4] = {FPBody::kBoneSpine[2], FPBody::kBoneSpine[3], FPBody::kBoneSpine[4], "neck_01"};
            for (int i = 0; i < 4; ++i) want[i] = m.BoneId(Bone(names[i]));
            const int neckNode = m.NodeIndex(Bone("neck_01")), headNode = m.NodeIndex(m_HeadBone);
            if (neckNode >= 0 && headNode >= 0) {
                const glm::vec3 n0(m.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, neckNode)[3]);
                const glm::vec3 n1(m.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, headNode)[3]);
                for (int mi = 0; mi < m.MeshCount(); ++mi) {
                    const auto& bind = m.Mesh(mi).LocalPositions();
                    const auto& idx = m.Mesh(mi).LocalIndices();
                    if (bind.size() != m.MeshSkinVertices(mi).size()) continue;
                    std::map<std::tuple<int, int, int>, std::uint32_t> weld;
                    std::vector<std::uint32_t> id(bind.size());
                    for (std::uint32_t vi = 0; vi < (std::uint32_t)bind.size(); ++vi) {
                        const glm::ivec3 q(glm::round(bind[vi] * 1e4f));
                        id[vi] = weld.emplace(std::make_tuple(q.x, q.y, q.z), vi).first->second;
                    }
                    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edges;
                    for (size_t t = 0; t + 2 < idx.size(); t += 3)
                        for (int k3 = 0; k3 < 3; ++k3) {
                            std::uint32_t a = id[idx[t + k3]], b = id[idx[t + (k3 + 1) % 3]];
                            if (a > b) std::swap(a, b);
                            ++edges[{a, b}];
                        }
                    std::vector<char> onRim(bind.size(), 0);
                    for (const auto& [edge, count] : edges)
                        if (count == 1) onRim[edge.first] = onRim[edge.second] = 1;
                    const glm::vec3 axis = n1 - n0;
                    for (std::uint32_t vi = 0; vi < (std::uint32_t)bind.size(); ++vi) {
                        if (!onRim[vi]) continue;
                        const float t = std::clamp(glm::dot(bind[vi] - n0, axis) / std::max(glm::dot(axis, axis), 1e-9f), 0.0f, 1.0f);
                        if (glm::length(bind[vi] - (n0 + axis * t)) < kNeckRimReach) rim.push_back({k, mi, vi, true});
                    }
                }
            }
            for (int mi = 0; mi < m.MeshCount(); ++mi) {
                const auto& verts = m.MeshSkinVertices(mi);
                for (std::uint32_t vi = 0; vi < (std::uint32_t)verts.size(); ++vi) {
                    const ModelMesh::SkinVertex& v = verts[vi];
                    int top = -1;
                    float best = 0.0f, hidden = 0.0f;
                    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                        const int b = v.BoneIDs[i];
                        if (b < 0) continue;
                        if (v.Weights[i] > best) { best = v.Weights[i]; top = b; }
                        for (int h = 0; h < 8 && tag->CameraHideBones[h] >= 0; ++h)
                            if (tag->CameraHideBones[h] == b) hidden += v.Weights[i];
                    }
                    if (top < 0 || hidden > 0.5f || std::find(std::begin(want), std::end(want), top) == std::end(want)) continue;
                    m_EyeClearPoints.push_back({k, mi, vi, false});
                }
            }
        }
        if (m_EyeClearPoints.size() > 600) {
            std::vector<EyeClearPoint> thin;
            const size_t stride = (m_EyeClearPoints.size() + 599) / 600;
            for (size_t i = 0; i < m_EyeClearPoints.size(); i += stride) thin.push_back(m_EyeClearPoints[i]);
            m_EyeClearPoints.swap(thin);
        }
        m_EyeClearPoints.insert(m_EyeClearPoints.end(), rim.begin(), rim.end());
        if (const Model* drv = DriverModel()) {
            m_NeckNode = drv->NodeIndex(Bone("neck_01"));
            m_HeadRimNode = drv->NodeIndex(m_HeadBone);
        }
        BodyDebug::Info().NeckRimPoints = (int)rim.size();
    }
    if (m_EyeClearPoints.empty()) return 0.0f;
    // The points as drawn now (skinned on the CPU, world space).
    thread_local std::vector<glm::vec3> points, rimPoints;
    points.clear();
    rimPoints.clear();
    size_t lastPiece = (size_t)-1;
    glm::mat4 pieceWorld(1.0f);
    for (const EyeClearPoint& c : m_EyeClearPoints) {
        if (c.Piece >= m_Models.size() || !m_Models[c.Piece] || !reg.valid(m_Pieces[c.Piece])) continue;
        if (c.Piece != lastPiece) { pieceWorld = world.ComposeWorldTransform(m_Pieces[c.Piece]); lastPiece = c.Piece; }
        const Model& m = *m_Models[c.Piece];
        const auto& verts = m.MeshSkinVertices(c.Mesh);
        if (c.Vertex >= verts.size()) continue;
        const ModelMesh::SkinVertex& v = verts[c.Vertex];
        glm::mat4 skin(0.0f);
        float total = 0.0f;
        for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
            if (v.BoneIDs[i] >= 0) { skin += m.FinalBoneMatrix(v.BoneIDs[i]) * v.Weights[i]; total += v.Weights[i]; }
        if (total > 1e-4f) (c.Rim ? rimPoints : points).push_back(glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f)));
    }
    // The opening's outward axis (neck_01 -> head, posed) and centre: the eye sees in only from its outer side.
    glm::vec3 rimAxis(0.0f, 1.0f, 0.0f), rimCentre(0.0f);
    bool rimOpen = false;
    if (!rimPoints.empty()) {
        for (const glm::vec3& p : rimPoints) rimCentre += p;
        rimCentre /= (float)rimPoints.size();
        glm::mat4 a(1.0f), b(1.0f);
        const Model* drv = DriverModel();
        if (drv && m_NeckNode >= 0 && m_HeadRimNode >= 0 && drv->NodeTransform(drv->NodeName(m_NeckNode), a) &&
            drv->NodeTransform(drv->NodeName(m_HeadRimNode), b) && reg.valid(m_Body)) {
            const glm::vec3 axis = glm::mat3(world.ComposeWorldTransform(m_Body)) * (glm::vec3(b[3]) - glm::vec3(a[3]));
            if (glm::length(axis) > 1e-5f) rimAxis = glm::normalize(axis);
        }
        rimOpen = true;
    }
    // A point is in the way when it is nearer than the near plane plus the clearance along the view and inside the view's
    // frustum at that depth (or at the near plane, if nearer): the near plane slices there, and what's behind it shows.
    const glm::vec3 f = camera.Front(), u = camera.Up(), r = camera.Right();
    const float nearPlane = std::max(camera.NearPlane, 0.01f), need = nearPlane + kEyeBodyClearance;
    const float tanHalf = std::tan(glm::radians(camera.Fov) * 0.5f), kAspect = 16.0f / 9.0f;
    // Only a point nearer than `need` to some tried eye (on eye + flat * [0, kEyeBodyPushMax]) can block, so the rest are
    // dropped once here instead of tested by every push step and every bisection probe (up to ~110 sweeps a frame).
    thread_local std::vector<glm::vec3> nearPoints;
    nearPoints.clear();
    const float reach = need + kEyeBodyPushMax + 1e-3f;
    for (const glm::vec3& p : points)
        if (glm::dot(p - eye, p - eye) < reach * reach) nearPoints.push_back(p);
    const auto blockedAs = [&](const glm::vec3& at, const glm::vec3& vf, const glm::vec3& vu, const glm::vec3& vr) {
        if (rimOpen && glm::dot(at - rimCentre, rimAxis) > 0.0f)
            for (const glm::vec3& p : rimPoints) {
                const glm::vec3 d = p - at;
                const float z = glm::dot(d, vf);
                // (The view widened by kNeckRimMargin: a rim edge can cross its border between two rim vertices outside it.)
                const float w = z * tanHalf * (1.0f + kNeckRimMargin);
                if (z > 0.0f && std::abs(glm::dot(d, vu)) <= w && std::abs(glm::dot(d, vr)) <= w * kAspect) return true;
            }
        for (const glm::vec3& p : nearPoints) {
            const glm::vec3 d = p - at;
            const float z = glm::dot(d, vf);
            if (z <= 0.0f || z >= need) continue;
            const float zz = std::max(z, nearPlane);
            if (std::abs(glm::dot(d, vu)) <= zz * tanHalf && std::abs(glm::dot(d, vr)) <= zz * tanHalf * kAspect) return true;
        }
        return false;
    };
    const auto blocked = [&](const glm::vec3& at) { return blockedAs(at, f, u, r); };
    float push = kEyeBodyPushMax;
    for (float s = 0.0f; s <= kEyeBodyPushMax; s += 0.005f)
        if (!blocked(eye + flat * s)) { push = s; break; }
    // The look-down limit: the steepest pitch whose view some push (up to kEyeBodyPushMax, 1 cm steps) clears - the push
    // follows the pitch, so judged from the eye as pushed now it held the view at what an unpushed eye clears (jogging,
    // -66 deg). Bisected (steeper is never clearer), to ~0.5 deg.
    const auto clearAt = [&](float pitch) {
        Camera c = camera;
        c.Pitch = pitch;
        const glm::vec3 cf = c.Front(), cu = c.Up(), cr = c.Right();
        for (float s = 0.0f; s <= kEyeBodyPushMax + 1e-4f; s += 0.01f)
            if (!blockedAs(eye + flat * s, cf, cu, cr)) return true;
        return false;
    };
    if (clearAt(-89.0f)) {
        lookLimitDeg = -89.0f;
    } else {
        float lo = -89.0f, hi = kLookLimitHighest; // lo blocked, hi clear (or the highest the limit goes)
        for (int k = 0; k < 7; ++k) {
            const float mid = 0.5f * (lo + hi);
            (clearAt(mid) ? hi : lo) = mid;
        }
        lookLimitDeg = hi;
    }
    return push;
}
