#include "FirstPersonBody.h"
#include "FirstPersonBodyInternal.h"
#include "Profiler.h"
#include "Core/DiagW1.h"
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

// FirstPersonBody: turns spread over a bone chain (the spine's aim at the view) on every piece, and which pieces skin it.

using namespace FirstPersonBodyDetail;

// Tilts the spine by `amount` of the camera's pitch and twists it by `twist` radians, spread evenly over spine_01..spine_05 (pitch about
// the model's X axis, twist about Y), on every piece so they stay one skeleton.
void FirstPersonBody::ApplySpineAim(const Camera& camera, float amount, float twist) {
    const float pitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f)); // up is positive
    // Rotating +Z about +X by theta gives (0, -sin, cos): looking up needs a negative angle.
    // The twist about the model's up turns the chest toward the view (positive = to the left).
    RotateSpine([&](float n) {
        return glm::angleAxis(twist / n, glm::vec3(0.0f, 1.0f, 0.0f)) *
               glm::angleAxis(-pitch * amount / n, glm::vec3(1.0f, 0.0f, 0.0f));
    });
}
void FirstPersonBody::ApplySpineRotation(const glm::quat& modelDelta) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    RotateSpine([&](float n) { return glm::normalize(glm::slerp(none, modelDelta, 1.0f / n)); });
}
void FirstPersonBody::RotateSpine(const std::function<glm::quat(float)>& stepFor) {
    const char* const* kSpine = FPBody::kBoneSpine;
    RotateChain({kSpine[0], kSpine[1], kSpine[2], kSpine[3], kSpine[4]}, stepFor, nullptr, &m_Spine);
}
void FirstPersonBody::RotateChain(const std::vector<std::string>& chain, const std::function<glm::quat(float)>& stepFor,
                                  const std::vector<std::shared_ptr<Model>>* models, const IK::SpineDistribution* dist) {
    PROFILE_SCOPE("DIAG RotateChain");
    ++DiagW1::RotateChains;
    // Scratch kept between calls (several chains a frame, each over every piece's full pose).
    thread_local std::vector<int> parents;
    thread_local std::vector<glm::mat4> globals;
    thread_local IK::Pose pose;
    thread_local std::vector<int> bones, slot; // slot: the bone's place in the chain (its weight and limit)
    const auto& list = models ? *models : m_Models;
    for (size_t k = 0; k < list.size(); ++k) {
        const auto& mp = list[k];
        if (!mp) continue;
        Model& m = *mp;
        pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        bones.clear();
        slot.clear();
        for (size_t c = 0; c < chain.size(); ++c)
            if (const int i = m.NodeIndex(Bone(chain[c])); i >= 0) { bones.push_back(i); slot.push_back((int)c); }
        if (bones.empty()) continue;
        // A piece skinning nothing the chain moves (legs, feet) draws the same either way. The driver always
        // turns: its head and shoulders are read for the camera and the arms.
        if ((k >= m_Pieces.size() || m_Pieces[k] != m_Driver) && !SkinsUnder(m, bones, chain.front())) continue;
        ++DiagW1::RotatePieces;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        // Only the chain and what's above it are read before ApplyLocalPose re-derives the rest.
        globals.resize(pose.size());
        IK::RefreshPath(pose, parents, globals, -1, bones.back());
        // Each bone's share of the turn: even (the bone count) unless the distribution says otherwise.
        float divisor[8], weight[8];
        const size_t count = std::min<size_t>(bones.size(), 5);
        for (size_t b = 0; b < count; ++b) weight[b] = dist ? dist->Weight[std::min(slot[b], 4)] : 1.0f;
        IK::ChainDivisors(weight, (int)count, divisor);
        const float pelvisAlpha = dist ? std::clamp(dist->PelvisAlpha, 0.0f, 1.0f) : 0.0f;
        if (pelvisAlpha > 0.0f) // the pelvis takes its share first; the spine the rest
            if (const int pelvis = m.NodeIndex(Bone(FPBody::kBonePelvis)); pelvis >= 0) {
                IK::RefreshPath(pose, parents, globals, -1, pelvis);
                IK::OffsetBoneOnly(pose, parents, globals, pelvis, glm::vec3(0.0f), stepFor(1.0f / pelvisAlpha),
                                   IK::Position(globals[pelvis]));
                IK::RefreshPath(pose, parents, globals, pelvis, bones.back());
            }
        // Only the next spine bone's global is read before ApplyLocalPose re-derives the lot, so
        // refresh just the path to it rather than the whole upper body after every bone.
        for (size_t b = 0; b < bones.size(); ++b) {
            const int i = bones[b];
            if (b > 0) IK::RefreshPath(pose, parents, globals, bones[b - 1], i);
            float d = b < count ? divisor[b] : (float)bones.size();
            if (pelvisAlpha > 0.0f) d /= 1.0f - pelvisAlpha;
            glm::quat step = stepFor(d);
            if (dist && b < count) step = IK::ClampStepAngle(step, dist->MaxAngle[std::min(slot[b], 4)]);
            IK::OffsetBoneOnly(pose, parents, globals, i, glm::vec3(0.0f), step, IK::Position(globals[i]));
        }
        m.ApplyLocalPose(pose);
    }
}
bool FirstPersonBody::SkinsUnder(const Model& m, const std::vector<int>& roots, const std::string& key) {
    const auto id = std::make_pair(&m, key);
    if (const auto it = m_SkinsUnder.find(id); it != m_SkinsUnder.end()) return it->second;
    std::vector<char> under((size_t)m.NodeCount(), 0);
    for (int r : roots) if (r >= 0 && r < m.NodeCount()) under[r] = 1;
    bool skins = false;
    for (int i = 0; i < m.NodeCount() && !skins; ++i) { // parents first
        if (!under[i] && m.NodeParent(i) >= 0 && under[m.NodeParent(i)]) under[i] = 1;
        skins = under[i] && m.BoneId(m.NodeName(i)) >= 0;
    }
    m_SkinsUnder[id] = skins;
    return skins;
}
bool FirstPersonBody::SkinsUpperBody(const Model& m) {
    if (const auto it = m_SkinsUpperBody.find(&m); it != m_SkinsUpperBody.end()) return it->second;
    std::vector<char> under((size_t)m.NodeCount(), 0);
    int chest = -1;
    for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(Bone(FPBody::kBoneSpine[b]));
    if (chest >= 0) under[chest] = 1;
    for (int s = 0; s < 2; ++s)
        if (const int c = m.NodeIndex(Bone(FPBody::kBoneClavicle[s])); c >= 0) under[c] = 1;
    bool skins = false;
    for (int i = 0; i < m.NodeCount(); ++i) { // parents first
        if (!under[i] && m.NodeParent(i) >= 0 && under[m.NodeParent(i)]) under[i] = 1;
        if (under[i] && m.BoneId(m.NodeName(i)) >= 0) { skins = true; break; }
    }
    m_SkinsUpperBody[&m] = skins;
    return skins;
}
