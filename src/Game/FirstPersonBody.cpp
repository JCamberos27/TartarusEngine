#include "FirstPersonBody.h"
#include "Profiler.h"
#include "BodyDebugDraw.h"
#include "FirstPersonBodyContract.h"

#include "Camera.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "IK.h"
#include "Log.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "SkinHideBuffer.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <cstdio>
#include <sstream>

namespace {

// Model space: the Quantum / UE mannequin family faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};

glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// Frame-rate independent approach toward a target over `seconds` (0 = snap).
float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }

// A node's position in its model's space as posed now.
glm::vec3 ModelPoint(const Model& m, int node) {
    glm::mat4 g(1.0f);
    return m.NodeTransform(m.NodeName(node), g) ? glm::vec3(g[3]) : glm::vec3(0.0f);
}

// A node's position in its model's bind pose; the driver's when the piece lacks it (one skeleton). False
// when neither has it.
bool BindPoint(const Model& piece, const Model& driver, const std::string& node, glm::vec3& out) {
    for (const Model* m : {&piece, &driver})
        if (const int n = m->NodeIndex(node); n >= 0) {
            out = glm::vec3(m->SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, n)[3]);
            return true;
        }
    return false;
}

// A clothing piece's collar (FirstPersonBodyCollarVertices) as a hide buffer; null when none of it is. `bitsOut`, when
// given, gets the same bits on the CPU (the camera probe). Cached per model and drop: an outfit change in Play
// restarts the body, and the pieces it keeps mustn't hitch.
std::shared_ptr<SkinHideBuffer> CollarVerts(const std::shared_ptr<Model>& pieceRef, const Model& driver, const std::string& neckBone,
                                            const std::array<std::string, 2>& shoulderBones, float drop,
                                            std::shared_ptr<const std::vector<std::uint8_t>>* bitsOut = nullptr) {
    if (bitsOut) bitsOut->reset();
    if (drop < 0.0f || !pieceRef) return nullptr;
    struct Entry {
        float Drop;
        std::weak_ptr<Model> Alive; // the key's model, while it lives (a freed address can be reused)
        std::shared_ptr<SkinHideBuffer> Buffer;
        std::shared_ptr<const std::vector<std::uint8_t>> Bits;
    };
    // Deliberately leaked, like SkinHideBuffer's empty buffer: its buffers go with the GL context, not at exit.
    static auto& cache = *new std::map<const Model*, Entry>();
    for (auto it = cache.begin(); it != cache.end();) it = it->second.Alive.expired() ? cache.erase(it) : std::next(it);
    const Model& piece = *pieceRef;
    if (auto it = cache.find(&piece); it != cache.end() && it->second.Drop == drop) {
        if (bitsOut) *bitsOut = it->second.Bits;
        return it->second.Buffer;
    }

    std::shared_ptr<SkinHideBuffer> buffer;
    std::shared_ptr<const std::vector<std::uint8_t>> bits;
    glm::vec3 neck, shoulderL, shoulderR;
    if (BindPoint(piece, driver, neckBone, neck) && BindPoint(piece, driver, shoulderBones[0], shoulderL) &&
        BindPoint(piece, driver, shoulderBones[1], shoulderR)) {
        std::vector<glm::vec3> positions;
        std::vector<unsigned int> indices;
        piece.CollisionGeometry(positions, indices);
        auto collar = FirstPersonBodyCollarVertices(positions, neck, shoulderL, shoulderR, drop);
        if (std::find(collar.begin(), collar.end(), (std::uint8_t)1) != collar.end()) {
            buffer = std::make_shared<SkinHideBuffer>(OutfitCoverage::Pack(collar));
            bits = std::make_shared<const std::vector<std::uint8_t>>(std::move(collar));
        }
    }
    cache[&piece] = Entry{drop, pieceRef, buffer, bits};
    if (bitsOut) *bitsOut = bits;
    return buffer;
}

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

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

} // namespace

float FirstPersonBodyYaw(const glm::vec3& front, float fallback) {
    const glm::vec2 flat(front.x, front.z);
    if (glm::dot(flat, flat) < 1e-8f) return fallback;
    return std::atan2(front.x, front.z); // rotating +Z by this about +Y gives (sin, 0, cos)
}

glm::quat FirstPersonBodyShoulderLineTurn(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float weight) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0.0f || glm::dot(bodyAcross, bodyAcross) < 1e-10f || glm::dot(rigAcross, rigAcross) < 1e-10f) return none;
    const glm::quat full(glm::normalize(bodyAcross), glm::normalize(rigAcross));
    return glm::normalize(glm::slerp(none, full, weight));
}

glm::vec3 FirstPersonBodyShoulderLineTilt(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float tilt) {
    const float lb = glm::length(bodyAcross), lr = glm::length(rigAcross);
    if (lb < 1e-5f || lr < 1e-5f) return rigAcross;
    const glm::vec3 b = bodyAcross / lb, r = rigAcross / lr;
    const glm::vec2 flat(r.x, r.z);
    const float flatLen = glm::length(flat);
    if (flatLen < 1e-5f) return rigAcross; // a vertical line has no blade to keep
    const float y = glm::mix(b.y, r.y, std::clamp(tilt, 0.0f, 1.0f));
    const float horiz = std::sqrt(std::max(1.0f - y * y, 0.0f));
    return glm::vec3(flat.x / flatLen * horiz, y, flat.y / flatLen * horiz) * lr;
}

float FirstPersonBodyArmedEyeLift(float pitchRadians) {
    return 1.0f - glm::smoothstep(glm::radians(25.0f), glm::radians(60.0f), std::abs(pitchRadians));
}

float FirstPersonBodySpineAim(float pitchRadians, float spineAim, float spineAimDown) {
    return std::clamp(pitchRadians < 0.0f ? spineAimDown : spineAim, 0.0f, 1.0f);
}

float FirstPersonBodyWrapAngle(float radians) {
    const float twoPi = 6.28318530718f;
    radians = std::fmod(radians, twoPi);
    if (radians > 3.14159265359f) radians -= twoPi;
    else if (radians <= -3.14159265359f) radians += twoPi;
    return radians;
}

bool FirstPersonBodyShouldTurn(float offset, float thresholdDegrees) {
    return thresholdDegrees > 0.0f && std::abs(offset) > glm::radians(thresholdDegrees);
}

glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw) {
    const glm::quat r = YawRotation(yaw);
    const glm::vec3 v(worldVelocity.x, 0.0f, worldVelocity.z);
    return {glm::dot(v, r * kRight), glm::dot(v, r * kForward)};
}

float FirstPersonBodyLookDown(float pitchRadians, float startDegrees) {
    const float start = glm::radians(std::clamp(startDegrees, 0.0f, 89.0f));
    const float t = std::clamp((-pitchRadians - start) / (glm::radians(90.0f) - start), 0.0f, 1.0f);
    return std::sin(t * glm::radians(90.0f)); // as a head's forward swing: most of it in the first half
}

float FirstPersonBodyClearPush(const std::vector<glm::vec3>& points, const glm::vec3& a, const glm::vec3& b, const glm::vec3& dir,
                               float clearance, float maxPush, float step) {
    if (clearance <= 0.0f || points.empty()) return 0.0f;
    const glm::vec3 ab = b - a;
    const float abLen2 = std::max(glm::dot(ab, ab), 1e-9f);
    const float c2 = clearance * clearance;
    auto clearAt = [&](float s) {
        const glm::vec3 o = a + dir * s;
        for (const glm::vec3& p : points) {
            const float t = std::clamp(glm::dot(p - o, ab) / abLen2, 0.0f, 1.0f);
            const glm::vec3 d = p - (o + ab * t);
            if (glm::dot(d, d) < c2) return false;
        }
        return true;
    };
    step = std::max(step, 1e-3f);
    for (float s = 0.0f; s < maxPush; s += step)
        if (clearAt(s)) return s;
    return maxPush;
}

float FirstPersonBodyElbowGap(const std::vector<glm::vec3>& points, const glm::vec3& shoulder, const glm::vec3& elbow, const glm::vec3& hand) {
    return FirstPersonBodyElbowGapAbove(points, shoulder, elbow, hand, -1.0f);
}

float FirstPersonBodyElbowGapAbove(const std::vector<glm::vec3>& points, const glm::vec3& shoulder, const glm::vec3& elbow,
                                   const glm::vec3& hand, float floor) {
    // Stops as soon as the gap is known to be at most `floor` (a point that close): the search only
    // asks whether an angle beats the best so far.
    const float floor2 = floor >= 0.0f ? floor * floor : -1.0f;
    const glm::vec3 a = shoulder + (elbow - shoulder) * 0.5f, c = elbow + (hand - elbow) * 0.5f;
    auto seg2 = [](const glm::vec3& p, const glm::vec3& s0, const glm::vec3& s1) {
        const glm::vec3 d = s1 - s0;
        const float t = std::clamp(glm::dot(p - s0, d) / std::max(glm::dot(d, d), 1e-9f), 0.0f, 1.0f);
        const glm::vec3 e = p - (s0 + d * t);
        return glm::dot(e, e);
    };
    float best = 1e18f;
    for (const glm::vec3& p : points) {
        best = std::min(best, std::min(seg2(p, a, elbow), seg2(p, elbow, c)));
        if (best <= floor2) break;
    }
    return std::sqrt(best);
}

float FirstPersonBodyElbowClearSwivel(const std::vector<glm::vec3>& points, const glm::vec3& shoulder, const glm::vec3& elbow,
                                      const glm::vec3& hand, float clearance, float maxAngle, float step, float prefer) {
    if (clearance <= 0.0f || points.empty() || maxAngle <= 0.0f) return 0.0f;
    const glm::vec3 line = hand - shoulder;
    if (glm::dot(line, line) < 1e-8f) return 0.0f;
    const glm::vec3 axis = glm::normalize(line);
    // Only what can come near the arm: a box around it, grown by the clearance and the elbow's swing.
    const float reach = glm::length(elbow - shoulder) + clearance;
    const glm::vec3 lo = glm::min(shoulder, hand) - glm::vec3(reach), hi = glm::max(shoulder, hand) + glm::vec3(reach);
    thread_local std::vector<glm::vec3> nearby;
    nearby.clear();
    for (const glm::vec3& p : points)
        if (glm::all(glm::greaterThanEqual(p, lo)) && glm::all(glm::lessThanEqual(p, hi))) nearby.push_back(p);
    auto gapAt = [&](float angle, float floor) {
        const glm::vec3 e = shoulder + glm::angleAxis(angle, axis) * (elbow - shoulder);
        return FirstPersonBodyElbowGapAbove(nearby, shoulder, e, hand, floor);
    };
    const float here = gapAt(0.0f, -1.0f);
    if (here >= clearance) return 0.0f;
    step = std::max(step, glm::radians(1.0f));
    float best = 0.0f, bestGap = here;
    for (float a = step; a <= maxAngle + 1e-5f; a += step)
        for (const float s : {prefer < 0.0f ? -a : a, prefer < 0.0f ? a : -a}) {
            // A gap at most bestGap (< clearance) changes nothing below, so its scan may stop there.
            const float g = gapAt(s, bestGap);
            if (g >= clearance) return s;
            if (g > bestGap) { bestGap = g; best = s; }
        }
    return best;
}

float FirstPersonBodyFootPelvis(float offL, float offR, float maxDrop, float maxRaise) {
    return std::clamp(std::min(offL, offR), -std::max(maxDrop, 0.0f), std::max(maxRaise, 0.0f));
}

glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset) {
    const float b = std::clamp(bob, 0.0f, 1.0f);
    return restHead + (head - restHead) * b + kRight * offset.x + glm::vec3(0.0f, offset.y, 0.0f) + kForward * offset.z;
}

float FirstPersonBodyPieceNearHide(float nearHide, float clothingNearHide, bool clothing) {
    const float body = std::max(nearHide, 0.0f);
    return clothing ? std::max(body, clothingNearHide) : body;
}

bool FirstPersonBodyNearHidden(const glm::vec3& p, const glm::vec3& eye, const glm::vec3& right, float radius, float width) {
    if (radius <= 0.0f) return false;
    const glm::vec3 d = p - eye;
    if (width <= 0.0f) return glm::dot(d, d) < radius * radius;
    const float side = glm::dot(d, right);
    const glm::vec3 rest = d - right * side;
    return (side * side) / (width * width) + glm::dot(rest, rest) / (radius * radius) < 1.0f;
}

std::vector<std::uint8_t> FirstPersonBodyCollarVertices(const std::vector<glm::vec3>& bindPositions, const glm::vec3& neck,
                                                        const glm::vec3& shoulderL, const glm::vec3& shoulderR, float drop) {
    const glm::vec2 line(shoulderL.x - shoulderR.x, shoulderL.z - shoulderR.z);
    const float span = glm::length(line);
    if (drop < 0.0f || span < 1e-3f) return {};
    const glm::vec2 across = line / span, ahead(-across.y, across.x);
    // A little past the shoulder joints: the cloth over them (the shoulder tops) turns up toward the eye.
    const float halfWidth = span * 0.5f * 1.15f, halfDepth = 0.3f;
    const float floor = std::min(shoulderL.y, shoulderR.y) - drop;
    std::vector<std::uint8_t> out(bindPositions.size(), 0);
    for (size_t i = 0; i < bindPositions.size(); ++i) {
        const glm::vec3& p = bindPositions[i];
        const glm::vec2 d(p.x - neck.x, p.z - neck.z);
        out[i] = p.y >= floor && std::abs(glm::dot(d, across)) <= halfWidth && std::abs(glm::dot(d, ahead)) <= halfDepth;
    }
    return out;
}

void FirstPersonBody::Fail(const std::string& message) {
    m_LastError = message;
    Log::Warn("First Person Body: " + message);
    m_Body = entt::null;
}

bool FirstPersonBody::Start(World& world, Player& player) {
    *this = FirstPersonBody{};
    BodyDebug::Info() = BodyDebug::Snapshot{};
    auto& reg = world.Registry;
    auto bodies = reg.view<FirstPersonBodyComponent>(entt::exclude<InactiveTag>);
    if (bodies.begin() == bodies.end()) return false;
    const entt::entity body = *bodies.begin(); // the first; a scene has one player
    const auto& cfg = reg.get<FirstPersonBodyComponent>(body);

    // The pieces: the root itself and its children, each a rigged model on the one skeleton.
    auto rigged = [&](entt::entity e) {
        const auto* rc = reg.try_get<RenderableComponent>(e);
        return rc && rc->ModelRef && rc->ModelRef->NodeCount() > 0;
    };
    std::vector<entt::entity> pieces;
    if (rigged(body)) pieces.push_back(body);
    if (const auto* h = reg.try_get<HierarchyComponent>(body))
        for (entt::entity child : h->Children)
            if (reg.valid(child) && !reg.all_of<InactiveTag>(child) && rigged(child)) pieces.push_back(child);
    // The first piece with an Animator Controller drives; the others follow it.
    for (entt::entity e : pieces)
        if (reg.all_of<AnimatorControllerComponent>(e)) { m_Driver = e; break; }
    if (pieces.empty()) { Fail("needs rigged body pieces (the object's own model, or child objects with models)."); return false; }
    if (m_Driver == entt::null) {
        Fail("needs an Animator Controller on one of its pieces (e.g. assets/Animations/Controllers/fps_body_locomotion.controller).");
        return false;
    }
    m_Body = body;
    if (const auto* outfit = reg.try_get<CharacterOutfitComponent>(body)) m_OutfitVersion = outfit->Version;

    // The clips' travel comes out of the pose and is handed to the Player - never applied to
    // an object: this class places the body itself. Turns stay in the pose: the view owns the yaw.
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    ac.RootMotion.Mode = (int)RootMotionMode::InPlace;
    ac.RootMotion.Rotation = true; // the turn clips' yaw is read (DeltaYaw) to turn the body; the pose stays facing forward
    ac.RootMotion.Vertical = false;

    Model& model = *reg.get<RenderableComponent>(m_Driver).ModelRef;
    m_HeadNode = model.NodeIndex(cfg.HeadBone);
    if (m_HeadNode < 0)
        Log::Warn("First Person Body: the body has no bone '" + cfg.HeadBone + "' - the camera stays at the eye height.");
    else
        m_RestHead = glm::vec3(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode)[3]);

    m_BoneMap = FPBody::ParseBoneMap(cfg.BoneMap);
    m_ShoulderNode[0] = model.NodeIndex(Bone(FPBody::kBoneUpperArm[0]));
    m_ShoulderNode[1] = model.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));

    auto names = [](const std::string& csv) {
        std::vector<std::string> out;
        std::stringstream list(csv);
        for (std::string name; std::getline(list, name, ',');)
            if (!(name = Trim(name)).empty()) out.push_back(name);
        return out;
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::vector<std::string> hiddenBones = names(cfg.HiddenBones), hiddenParts = names(cfg.HiddenParts);
    int shadowOnly = 0;
    for (entt::entity e : pieces) {
        Model* m = reg.get<RenderableComponent>(e).ModelRef.get();
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones) {
            const int n = m->NodeIndex(b);
            if (n >= 0) nodes.push_back(n);
            else if (e == m_Driver) Log::Warn("First Person Body: Hidden Bones names '" + b + "', which the body doesn't have.");
        }
        m->SetHiddenNodes(nodes);
        auto& bodyTag = reg.emplace_or_replace<PlayerBodyTag>(e);
        bodyTag.NearHide = std::max(cfg.NearHide, 0.0f);
        // The torso's shoulders, out of the camera's view (the arms piece keeps its own).
        const std::string pieceName = lower(reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string());
        const std::string armsName = lower(cfg.ArmsPiece);
        const bool isArms = !armsName.empty() && pieceName.find(armsName) != std::string::npos;
        if (!isArms) {
            int count = 0;
            for (const std::string& b : names(cfg.CameraHiddenBones))
                if (const int id = m->BoneId(Bone(b)); id >= 0 && count < 8) bodyTag.CameraHideBones[count++] = id;
        }
        // Clothing (an outfit piece that isn't a body part): its sleeves go into the view-model pass with
        // the arms (PlayerBodyTag). The sleeve bones: the Camera Hidden Bones and everything under them.
        bodyTag.HasSleeves = bodyTag.HasHeadBones = false;
        std::fill(std::begin(bodyTag.SleeveBones), std::end(bodyTag.SleeveBones), 0u);
        std::fill(std::begin(bodyTag.HeadBones), std::end(bodyTag.HeadBones), 0u);
        const auto* piece = reg.try_get<OutfitPieceComponent>(e);
        if (!isArms && piece && !(piece->Flags & OutfitPieceBodyPart)) {
            // Marks the palette bones at and under `roots` in `bits`; true if any.
            auto markUnder = [&](const std::vector<std::string>& roots, std::uint32_t (&bits)[16]) {
                bool any = false;
                std::vector<char> under((size_t)m->NodeCount(), 0);
                for (int i = 0; i < m->NodeCount(); ++i) { // parents come before their children
                    const int p = m->NodeParent(i);
                    under[(size_t)i] = (p >= 0 && under[(size_t)p]) ||
                                       std::find(roots.begin(), roots.end(), m->NodeName(i)) != roots.end();
                    if (!under[(size_t)i]) continue;
                    const int id = m->BoneId(m->NodeName(i));
                    if (id < 0 || id >= 16 * 32) continue;
                    bits[id >> 5] |= 1u << (id & 31);
                    any = true;
                }
                return any;
            };
            // The sleeves: the arm proper, from the upper arm down. Not the collarbones - with the gun up
            // they turn toward the eye, and a garment's shoulder (bulkier than skin) then wrapped the camera:
            // its inside showed as a black flap, swinging with the walk.
            const std::vector<std::string> arms = {Bone(FPBody::kBoneUpperArm[0]), Bone(FPBody::kBoneUpperArm[1])};
            bodyTag.HasSleeves = markUnder(arms, bodyTag.SleeveBones);
            // Never in the camera's view, like the bare torso's shoulders (Camera Hidden Bones): the neck and
            // everything above it (a hood, a collar), the collarbones' cloth (the shoulder tops), and what the
            // neck's parent moves alone (the collar's base, right under the eye).
            std::string headRoot = Bone(cfg.HeadBone), collarBase;
            if (const int h = m->NodeIndex(headRoot); h >= 0 && m->NodeParent(h) >= 0) {
                const int neck = m->NodeParent(h);
                std::string lowerNeck = m->NodeName(neck);
                for (char& c : lowerNeck) c = (char)std::tolower((unsigned char)c);
                if (lowerNeck.find("neck") != std::string::npos) {
                    headRoot = m->NodeName(neck);
                    if (m->NodeParent(neck) >= 0) collarBase = m->NodeName(m->NodeParent(neck));
                }
            }
            bodyTag.HasHeadBones = markUnder({headRoot, Bone(FPBody::kBoneClavicle[0]), Bone(FPBody::kBoneClavicle[1])},
                                             bodyTag.HeadBones);
            // ... minus the arms (the collarbones' subtree holds them): those are the sleeves' to show or hide.
            for (int w = 0; w < 16; ++w) bodyTag.HeadBones[w] &= ~bodyTag.SleeveBones[w];
            if (!collarBase.empty())
                if (const int id = m->BoneId(collarBase); id >= 0 && id < 16 * 32) {
                    bodyTag.HeadBones[id >> 5] |= 1u << (id & 31);
                    bodyTag.HasHeadBones = true;
                }
            // Bones can't tell everything: some packs skin a hood or a scarf to the chest bone (the Quantum
            // open-hood winter jacket, the warm hat's braids). Clothing near the eye isn't drawn in the camera's
            // view either (Clothing Near Hide, wider to the sides); the chest, ~25-30 cm below and ahead, still
            // is when looking down. Tick keeps these up to date with the component.
            bodyTag.Clothing = true;
            bodyTag.NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, true);
            bodyTag.NearHideWidth = std::max(cfg.ClothingNearHideWidth, 0.0f);
            // ... and by where it sits rather than what moves it: what's around the neck in the bind pose.
            bodyTag.CollarVerts = CollarVerts(reg.get<RenderableComponent>(e).ModelRef, *reg.get<RenderableComponent>(m_Driver).ModelRef, headRoot,
                                              {Bone(FPBody::kBoneUpperArm[0]), Bone(FPBody::kBoneUpperArm[1])}, cfg.CollarHideDrop,
                                              &bodyTag.CollarBits);
        }
        m_Models.push_back(reg.get<RenderableComponent>(e).ModelRef);
        m_Pieces.push_back(e);
        if (e != m_Driver)
            if (auto* pac = reg.try_get<AnimatorControllerComponent>(e)) {
                pac->Driver = m_Driver;
                // The same root-motion handling as the driver's, or the pieces' poses part company
                // (a stripped turn on one, the turn left in the pose on another).
                const auto& dm = reg.get<AnimatorControllerComponent>(m_Driver).RootMotion;
                pac->RootMotion.Mode = dm.Mode;
                pac->RootMotion.Bone = dm.Bone;
                pac->RootMotion.Rotation = dm.Rotation;
                pac->RootMotion.Vertical = dm.Vertical;
            }
        // A hidden piece is only left out of the camera's own view (the camera is inside the head - and so
        // is whatever an outfit hangs on it: hair, a hat, glasses). Its shadow, the Scene view and other
        // views keep it.
        const std::string name = lower(reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string());
        const auto* outfitPiece = reg.try_get<OutfitPieceComponent>(e);
        bool hide = outfitPiece && (outfitPiece->Flags & OutfitPieceHeadAttached);
        for (const std::string& part : hiddenParts) hide = hide || name.find(lower(part)) != std::string::npos;
        if (e != body && hide) {
            reg.get_or_emplace<PlayerBodyTag>(e).CameraHidden = true;
            ++shadowOnly;
        }
    }

    // Split poses: the world twins (only Weapon Arms poses the body's arms differently per view).
    if (cfg.WeaponArms) MakeTwins(world);

    // The input asks the blend tree for speeds its clips have.
    m_RunSpeed = std::max(cfg.RunSpeed, 0.01f);
    m_CrouchHeight = cfg.CrouchHeight;
    m_CrouchSpeed = cfg.CrouchSpeed;
    player.MoveSpeed = m_RunSpeed;
    player.SprintMultiplier = std::max(cfg.SprintSpeed, 0.01f) / m_RunSpeed;

    Log::Info("First Person Body: '" + (reg.all_of<NameComponent>(body) ? reg.get<NameComponent>(body).Name : std::string("body")) +
              "' is the player's body - " + std::to_string(pieces.size()) + " pieces, " + std::to_string(shadowOnly) +
              " hidden from the camera, root motion at responsiveness " + std::to_string(cfg.Responsiveness).substr(0, 4) + ".");
    return true;
}

bool FirstPersonBody::OutfitChanged(const World& world) const {
    if (m_Body == entt::null || !world.Registry.valid(m_Body)) return false;
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(m_Body);
    return outfit && outfit->Version != m_OutfitVersion;
}

void FirstPersonBody::Stop(World& world) {
    m_HeadVerts.clear(); // keyed by model address: the next Start's models may reuse this run's
    m_TorsoVerts.clear();
    BodyDebug::Info() = BodyDebug::Snapshot{};
    BodyDebug::Clear();
    if (m_PoseSource != entt::null && world.Registry.valid(m_PoseSource)) world.Registry.remove<PoseSourceTag>(m_PoseSource);
    for (entt::entity e : m_ArmsTagged)
        if (world.Registry.valid(e)) world.Registry.remove<ViewModelTag>(e);
    for (const auto& m : m_Models)
        if (m) m->SetHiddenNodes({}); // model instances outlive Play; the rest is snapshot state
    for (entt::entity e : m_Pieces)
        if (world.Registry.valid(e)) world.Registry.remove<PlayerBodyTag, OwnerViewOnlyTag>(e);
    for (entt::entity e : m_Twins)
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    *this = FirstPersonBody{};
}

// A world twin per piece: the same model (an instance: its own pose), materials, shadows and outfit hiding,
// drawn in every view but the player's own camera, which draws the piece itself. Root-level objects, placed
// onto their piece every frame (SyncTwins), so the outfit's own hierarchy never sees them.
void FirstPersonBody::MakeTwins(World& world) {
    auto& reg = world.Registry;
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    std::vector<std::string> hiddenBones;
    {
        std::stringstream list(cfg.HiddenBones);
        for (std::string name; std::getline(list, name, ',');)
            if (!(name = Trim(name)).empty()) hiddenBones.push_back(name);
    }
    for (size_t k = 0; k < m_Pieces.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        std::shared_ptr<Model> inst = m_Models[k]->CreateInstance();
        const std::string name = reg.all_of<NameComponent>(e) ? reg.get<NameComponent>(e).Name : std::string("piece");
        const entt::entity t = world.CreateModelEntity(inst, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] World " + name);
        // Copied by value: adding a component can move its storage (and any reference into it).
        const RenderableComponent src = reg.get<RenderableComponent>(e);
        auto& dst = reg.get<RenderableComponent>(t);
        dst.Materials = src.Materials;
        dst.CastShadows = src.CastShadows;
        dst.ReceiveShadows = src.ReceiveShadows;
        if (const auto* hide = reg.try_get<OutfitHideTag>(e)) {
            const OutfitHideTag copy = *hide;
            reg.emplace_or_replace<OutfitHideTag>(t, copy);
        }
        if (const auto* layer = reg.try_get<LayerComponent>(e)) {
            const LayerComponent copy = *layer;
            reg.emplace_or_replace<LayerComponent>(t, copy);
        }
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones)
            if (const int n = inst->NodeIndex(b); n >= 0) nodes.push_back(n);
        inst->SetHiddenNodes(nodes);
        reg.emplace_or_replace<HiddenFromOwnerTag>(t);
        reg.emplace_or_replace<OwnerViewOnlyTag>(e);
        m_Twins.push_back(t);
        m_TwinModels.push_back(std::move(inst));
    }
    SyncTwins(world);
}

void FirstPersonBody::SyncTwins(World& world) {
    auto& reg = world.Registry;
    for (size_t k = 0; k < m_Twins.size() && k < m_Pieces.size(); ++k) {
        if (!reg.valid(m_Twins[k]) || !reg.valid(m_Pieces[k])) continue;
        const glm::mat4 w = world.ComposeWorldTransform(m_Pieces[k]);
        const glm::vec3 scale(glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])), glm::length(glm::vec3(w[2])));
        const glm::mat3 r(glm::vec3(w[0]) / std::max(scale.x, 1e-6f), glm::vec3(w[1]) / std::max(scale.y, 1e-6f),
                          glm::vec3(w[2]) / std::max(scale.z, 1e-6f));
        world.SetWorldPose(m_Twins[k], glm::vec3(w[3]), glm::normalize(glm::quat_cast(r)));
        reg.get<TransformComponent>(m_Twins[k]).Scale = scale;
        const bool inactive = reg.all_of<InactiveTag>(m_Pieces[k]);
        if (inactive != reg.all_of<InactiveTag>(m_Twins[k])) {
            if (inactive) reg.emplace_or_replace<InactiveTag>(m_Twins[k]);
            else reg.remove<InactiveTag>(m_Twins[k]);
        }
        const IK::Pose& pose = m_Models[k]->AppliedLocalPose();
        if (!pose.empty()) m_TwinModels[k]->ApplyLocalPose(pose);
    }
}

void FirstPersonBody::PlaceHeadAttachedTwins(World& world) {
    // As OutfitSystem::UpdateAttachments places the pieces (the head piece's head bone skinning matrix, from
    // its own place), but off the head twin: rigid head wear then follows the world head's tilt, this frame.
    auto& reg = world.Registry;
    int headTwin = -1;
    for (size_t k = 0; k < m_Pieces.size() && k < m_Twins.size(); ++k)
        if (reg.valid(m_Pieces[k]) && reg.all_of<OutfitPieceComponent>(m_Pieces[k]) && reg.get<OutfitPieceComponent>(m_Pieces[k]).Slot == "Head")
            headTwin = (int)k;
    if (headTwin < 0 || !reg.valid(m_Twins[headTwin]) || !m_TwinModels[headTwin]) return;
    const int bone = m_TwinModels[headTwin]->BoneId("head");
    if (bone < 0) return;
    const glm::mat4 follow = world.ComposeWorldTransform(m_Twins[headTwin]) * m_TwinModels[headTwin]->FinalBoneMatrix(bone);
    const glm::vec3 scale(glm::length(glm::vec3(follow[0])), glm::length(glm::vec3(follow[1])), glm::length(glm::vec3(follow[2])));
    if (scale.x < 1e-6f || scale.y < 1e-6f || scale.z < 1e-6f) return;
    const glm::quat rot = glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(follow[0]) / scale.x, glm::vec3(follow[1]) / scale.y, glm::vec3(follow[2]) / scale.z)));
    for (size_t k = 0; k < m_Pieces.size() && k < m_Twins.size(); ++k) {
        if (!reg.valid(m_Pieces[k]) || !reg.valid(m_Twins[k]) || !reg.all_of<OutfitPieceComponent>(m_Pieces[k])) continue;
        const int flags = reg.get<OutfitPieceComponent>(m_Pieces[k]).Flags;
        if (!(flags & OutfitPieceHeadAttached) || (flags & OutfitPieceBodyPart)) continue;
        if (!m_TwinModels[k] || m_TwinModels[k]->BoneCount() > 0) continue; // skinned: it follows by its bones
        world.SetWorldPose(m_Twins[k], glm::vec3(follow[3]), rot);
        reg.get<TransformComponent>(m_Twins[k]).Scale = scale;
    }
}

void FirstPersonBody::BeforePlayerMove(Player& player, Camera& camera) {
    camera.Position -= m_CameraApplied;
    m_CameraApplied = glm::vec3(0.0f);
    player.MaxYawRate = 0.0f;
    player.CrouchHeight = IsActive() ? m_CrouchHeight : 0.0f;
    player.CrouchSpeedMultiplier = m_CrouchSpeed;
    if (!IsActive()) return;
    if (m_Still) {
        // Aiming is free within the turn threshold either side of the body; the limit is on the turn
        // beyond it, which is what the feet have to step round to (the body's yaw is atan2(x, z) of the
        // view, and Camera::Yaw is measured from +X toward +Z: 90 degrees minus it).
        player.MaxYawRate = m_MaxTurnRate;
        player.YawFreeCenter = 90.0f - glm::degrees(m_Yaw);
        player.YawFreeRange = m_TurnThreshold;
    }
    // Airborne, the input steers (there is no travel in a fall clip to follow).
    player.RootMotionVelocity = m_RootVelocity;
    // The landing clip's own travel would stop a running player dead: while it plays, the input steers.
    player.RootMotionWeight = player.Grounded && !m_InLand ? 1.0f - std::clamp(m_Responsiveness, 0.0f, 1.0f) : 0.0f;
}

void FirstPersonBody::Tick(World& world, const Player& player, const Camera& camera, float dt) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Responsiveness = cfg.Responsiveness;
    for (entt::entity e : m_Pieces)
        if (auto* tag = reg.valid(e) ? reg.try_get<PlayerBodyTag>(e) : nullptr) {
            // Live, for the Inspector - clothing keeping its own (Start's), not reset to the body's.
            tag->NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, tag->Clothing);
            tag->NearHideWidth = tag->Clothing ? std::max(cfg.ClothingNearHideWidth, 0.0f) : 0.0f;
        }
    BodyDebug::Clear();
    m_SinceTrigger += dt;

    // Stand at the capsule's feet, facing the view.
    if (PhysicsWorld::HasCharacter()) {
        float f[3];
        PhysicsWorld::GetCharacterFootPosition(f);
        // A stair pops the capsule up (or down) in a frame: the body (and so the camera on its head)
        // stays where it was and eases to the new height; the legs' IK reaches the step meanwhile.
        const float rise = f[1] - m_LastCapsuleY;
        if (cfg.FootIK && m_HaveCapsule && player.Grounded && m_LastGrounded && dt > 0.0f && std::abs(rise) > cfg.StairPopRise &&
            std::abs(rise) / dt > cfg.StairPopRate)
            m_StepOffset = std::clamp(m_StepOffset - rise, -0.3f, 0.3f);
        m_StepOffset -= m_StepOffset * Follow(dt, player.Grounded && cfg.FootIK ? cfg.StairEase : 0.03f);
        m_LastCapsuleY = f[1];
        m_LastGrounded = player.Grounded;
        m_HaveCapsule = true;
        m_Feet = glm::vec3(f[0], f[1] + m_StepOffset, f[2]);
    } else {
        m_Feet = camera.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    }
    m_ViewYaw = FirstPersonBodyYaw(camera.Front(), m_ViewYaw);
    if (!m_HaveHeading) { m_Yaw = m_ViewYaw; m_HaveHeading = true; }
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    auto fireTrigger = [&](AnimatorControllerComponent& a, const char* name) { a.SetTrigger(name); m_LastTrigger = name; m_SinceTrigger = 0.0f; };

    // The movement, in the body's frame, eased so the gait changes smoothly.
    const glm::vec2 target = FirstPersonBodyLocalMove(player.WishVelocity, m_Yaw);
    // Letting go at speed, the gait holds for the moment a stop clip is being picked (the blend would
    // otherwise slow the body on its own first, and the stop clip's own travel come on top of it).
    const bool holdForStop = cfg.StartStopClips && glm::length(target) < 0.01f && m_IdleTime < cfg.StopDebounce &&
                         glm::length(m_Move) > (player.Crouched ? cfg.StopMinSpeedCrouched : cfg.StopMinSpeed);
    if (!holdForStop) m_Move += (target - m_Move) * Follow(dt, cfg.ParamSmoothing);
    m_AirTime = player.Grounded ? 0.0f : m_AirTime + dt;
    m_Grounded = player.Grounded;
    m_InLand = reg.valid(m_Driver) && reg.get<AnimatorControllerComponent>(m_Driver).InState(FPBody::kStateLand);

    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);

    // The heading. Moving, the body faces the view (a quick ease, no pop). Standing still with a
    // Turn Threshold, it keeps its heading until the view is that far off, then a turn clip carries
    // it round (the clip's own yaw turns it, so the feet plant).
    const bool still = player.Grounded && glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) < 0.1f &&
                       glm::length(m_Move) < 0.2f;
    m_Still = still && cfg.TurnThreshold > 0.0f;
    m_MaxTurnRate = cfg.MaxTurnRate;
    m_TurnThreshold = cfg.TurnThreshold;
    float offset = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    if (cfg.TurnThreshold <= 0.0f) {
        m_Yaw = m_ViewYaw;
        m_Turning = false;
    } else if (m_Turning) {
        m_TurnTime += dt;
        if (ac.InState(FPBody::kStateTurn) || ac.InState(FPBody::kStateCrouchTurn)) {
            const float step = glm::radians(ac.RootMotion.DeltaYaw);
            m_Yaw += step;
            m_TurnDone += step;
        }
        offset = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
        // Done when the clip has played out (a 180 takes longer than a 90), the view is reached, or
        // the player moves off; the timeout is only a guard.
        const bool clipDone = m_TurnTime > cfg.TurnMinTime && (!ac.InState(FPBody::kStateTurn) || ac.StateTime >= 0.97f);
        if (!still || std::abs(offset) < glm::radians(cfg.TurnEndAngle) || clipDone || m_TurnTime > cfg.TurnTimeout) {
            Log::Info("First Person Body: turned " + std::to_string(glm::degrees(m_TurnDone)).substr(0, 6) + " deg in " +
                      std::to_string(m_TurnTime).substr(0, 4) + " s, " + std::to_string(glm::degrees(offset)).substr(0, 6) + " deg off the view.");
            m_Turning = false;
        }
    } else if (still) {
        if (FirstPersonBodyShouldTurn(offset, cfg.TurnThreshold)) {
            m_Turning = true;
            m_TurnTime = 0.0f;
            m_TurnDone = 0.0f;
            ac.SetFloat(FPBody::kTurnAngle, std::clamp(glm::degrees(offset), -180.0f, 180.0f));
        }
    } else {
        m_Yaw += offset * Follow(dt, cfg.TurnMoveEase);
    }
    // The view can outrun a turn clip: the body never lags it by more than this (a bounded slide of
    // the feet beats a chest twisted right round). Keep Mouse Sensitivity low enough that the turn clips keep up.
    if (cfg.TurnThreshold > 0.0f) {
        const float lag = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw), maxLag = glm::radians(std::max(cfg.TurnThreshold + cfg.TurnLagMargin, cfg.TurnLagFloor));
        if (std::abs(lag) > maxLag) m_Yaw = m_ViewYaw - std::copysign(maxLag, lag);
    }
    m_Yaw = FirstPersonBodyWrapAngle(m_Yaw);
    m_Twist = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    ac.SetBool(FPBody::kTurning, m_Turning);

    // Starting and stopping: a clip for each, picked by the direction, only from plain locomotion (a
    // trigger fired elsewhere would wait and go off later). A stop needs a moment of no input
    // (tapping between keys isn't one: 50 ms) and some speed to shed.
    {
        const glm::vec2 wishLocal = FirstPersonBodyLocalMove(player.WishVelocity, m_Yaw);
        const float wishLen = glm::length(wishLocal);
        const bool wantsMove = wishLen > 0.1f;
        const bool plain = cfg.StartStopClips && player.Grounded && (ac.InState(FPBody::kStateLocomotion) || ac.InState(FPBody::kStateCrouchLoco)) && !m_Turning;
        ac.SetBool(FPBody::kMoving, wantsMove);
        // Standing still, dropping into or rising out of a crouch plays its transition clip; on the
        // move it is just the crossfade between the gaits.
        if (cfg.StartStopClips && player.Grounded && !wantsMove && glm::length(m_Move) < 0.4f && player.Crouched != m_WasCrouched) {
            if (player.Crouched && ac.InState(FPBody::kStateLocomotion)) fireTrigger(ac, FPBody::kCrouchDown);
            if (!player.Crouched && ac.InState(FPBody::kStateCrouchLoco)) fireTrigger(ac, FPBody::kCrouchUp);
        }
        m_WasCrouched = player.Crouched;
        // How far the clips carry the body: logged, to tune them against the feel.
        const bool inStop = ac.InState(FPBody::kStateStop) || ac.InState(FPBody::kStateStopRun), inStart = ac.InState(FPBody::kStateStart);
        const float travel = glm::length(glm::vec2(ac.RootMotion.DeltaPosition.x, ac.RootMotion.DeltaPosition.z));
        if (inStop) m_StopDistance += travel;
        else if (m_StopDistance > 0.0f) {
            Log::Info("First Person Body: the stop clip carried the body " + std::to_string(m_StopDistance).substr(0, 4) + " m.");
            m_StopDistance = 0.0f;
        }
        if (inStart) m_StartDistance += travel;
        else if (m_StartDistance > 0.0f) {
            Log::Info("First Person Body: the start clip carried the body " + std::to_string(m_StartDistance).substr(0, 4) + " m.");
            m_StartDistance = 0.0f;
        }
        if (wantsMove) {
            const glm::vec2 dir = wishLocal / wishLen;
            if (plain && m_IdleTime >= cfg.StartIdleTime && glm::length(m_Move) < cfg.StartMaxMove) {
                ac.SetFloat(FPBody::kStartX, dir.x);
                ac.SetFloat(FPBody::kStartY, dir.y);
                fireTrigger(ac, FPBody::kStart);
            }
            m_LastDir = dir;
            m_LastSprint = glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) > m_RunSpeed * 1.05f;
            m_IdleTime = 0.0f;
            m_MoveTime += dt;
        } else {
            const float before = m_IdleTime;
            m_IdleTime += dt;
            // A stop needs a run to stop from: a tap of the keys (or a step or two) just eases to a halt.
            const bool ranEnough = m_MoveTime >= (player.Crouched ? cfg.StopMinRunTimeCrouched : cfg.StopMinRunTime);
            if (before < cfg.StopDebounce && m_IdleTime >= cfg.StopDebounce) m_MoveTime = 0.0f;
            if (plain && ranEnough && before < cfg.StopDebounce && m_IdleTime >= cfg.StopDebounce &&
                glm::length(m_Move) > (player.Crouched ? cfg.StopMinSpeedCrouched : cfg.StopMinSpeed)) {
                ac.SetFloat(FPBody::kStopX, m_LastDir.x);
                ac.SetFloat(FPBody::kStopY, m_LastDir.y);
                fireTrigger(ac, m_LastSprint && m_LastDir.y > cfg.StopRunForward ? FPBody::kStopRun : FPBody::kStop);
            }
        }
    }

    ac.SetFloat(FPBody::kMoveX, m_Move.x);
    ac.SetFloat(FPBody::kMoveY, m_Move.y);
    ac.SetFloat(FPBody::kSpeed, glm::length(m_Move));
    ac.SetBool(FPBody::kSprint, glm::length(glm::vec2(player.WishVelocity.x, player.WishVelocity.z)) > m_RunSpeed * 1.05f);
    ac.SetBool(FPBody::kGrounded, player.Grounded);
    ac.SetBool(FPBody::kCrouched, player.Crouched);
    // Off the ground for a moment (not a step down a stair): falling.
    ac.SetBool(FPBody::kAirborne, m_AirTime > cfg.AirborneDelay);
    if (player.Jumped) fireTrigger(ac, FPBody::kJump);

    // What the body is doing, for the Inspector's live readout and the Scene viewport's overlay.
    {
        BodyDebug::Snapshot& d = BodyDebug::Info();
        d.Valid = true;
        d.AnimatorState = ac.StateName;
        d.StateTime = ac.StateTime;
        d.Turning = m_Turning;
        d.ViewOffsetDeg = glm::degrees(FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw));
        d.TwistDeg = glm::degrees(m_Twist);
        d.Still = m_Still;
        d.IdleTime = m_IdleTime;
        d.MoveTime = m_MoveTime;
        d.MoveSpeed = glm::length(m_Move);
        d.RootSpeed = glm::length(m_RootVelocity);
        d.StepOffset = m_StepOffset;
        d.ArmsWeight = m_ArmsWeight;
        d.LastTrigger = m_LastTrigger;
        d.LastTriggerAgo = m_SinceTrigger;
        if (BodyDebug::Enabled()) {
            const glm::vec3 base = m_Feet + glm::vec3(0.0f, 0.03f, 0.0f);
            const auto heading = [&](float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); };
            BodyDebug::Arrow(base, base + heading(m_Yaw) * 0.9f, glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));      // the body's heading
            BodyDebug::Arrow(base, base + heading(m_ViewYaw) * 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));   // where the view faces
            if (cfg.TurnThreshold > 0.0f) {                                                                // the turn threshold wedge
                const float t = glm::radians(cfg.TurnThreshold);
                const glm::vec4 wedge(1.0f, 0.55f, 0.15f, 0.9f);
                BodyDebug::Line(base, base + heading(m_Yaw + t) * 1.0f, wedge);
                BodyDebug::Line(base, base + heading(m_Yaw - t) * 1.0f, wedge);
                for (int i = 0; i < 12; ++i)
                    BodyDebug::Line(base + heading(m_Yaw - t + 2.0f * t * (float)i / 12.0f) * 1.0f,
                                    base + heading(m_Yaw - t + 2.0f * t * (float)(i + 1) / 12.0f) * 1.0f, wedge);
            }
            if (glm::length(m_RootVelocity) > 0.05f)                                                       // what the clips carry the capsule at
                BodyDebug::Arrow(base + glm::vec3(0.0f, 0.1f, 0.0f), base + glm::vec3(0.0f, 0.1f, 0.0f) + m_RootVelocity * 0.4f, glm::vec4(0.2f, 0.9f, 1.0f, 1.0f));
            if (std::abs(m_StepOffset) > 0.005f)                                                           // a stair being eased out
                BodyDebug::Line(base, base + glm::vec3(0.0f, m_StepOffset, 0.0f), glm::vec4(1.0f, 0.2f, 0.8f, 1.0f));
        }
    }
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,
                                 const std::string& rigCameraBone) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);

    // This step's root motion, for the capsule's next move.
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    const auto& ac = reg.get<AnimatorControllerComponent>(m_Driver);
    const glm::vec3 d = ac.RootMotion.DeltaPosition;
    m_RootVelocity = dt > 0.0f && !m_Turning ? glm::vec3(d.x, 0.0f, d.z) / dt : glm::vec3(0.0f);

    {
    PROFILE_SCOPE("FPB foot IK");
    ApplyFootIK(world, cfg, dt); // the pelvis and legs first: everything after reads the final pose
    }
    PROFILE_SCOPE("FPB spine + shoulders");

    // The chest follows the view's pitch first, so the head (and the camera on it) and the
    // shoulders are where they will be drawn.
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    const Model* model = rc ? rc->ModelRef.get() : nullptr;
    glm::mat4 head(1.0f);
    const bool haveHead = model && m_HeadNode >= 0 && model->NodeTransform(model->NodeName(m_HeadNode), head);
    const glm::vec3 headAnimated = glm::vec3(head[3]); // the clips' head, before the chest tilts
    const bool haveShoulders = model && m_ShoulderNode[0] >= 0 && m_ShoulderNode[1] >= 0;
    // The shoulders as the arms will be drawn: the arms piece with the rig's arm shape on it (the
    // clavicles move the shoulders), else the driver's. Model space.
    const Model* armRig = weaponArms != entt::null && reg.valid(weaponArms) && reg.all_of<RenderableComponent>(weaponArms)
                              ? reg.get<RenderableComponent>(weaponArms).ModelRef.get()
                              : nullptr;
    auto shoulderPair = [&](glm::vec3& left, glm::vec3& right) {
        left = right = glm::vec3(0.0f);
        if (haveShoulders) {
            left = ModelPoint(*model, m_ShoulderNode[0]);
            right = ModelPoint(*model, m_ShoulderNode[1]);
        }
        if (!armRig || armRig->AppliedLocalPose().empty()) return;
        std::string want = cfg.ArmsPiece;
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        for (size_t k = 0; k < m_Pieces.size() && !want.empty(); ++k) {
            if (m_Pieces[k] == m_Body || !reg.valid(m_Pieces[k]) || !reg.all_of<NameComponent>(m_Pieces[k])) continue;
            std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            if (name.find(want) == std::string::npos) continue;
            const Model& m = *m_Models[k];
            IK::Pose pose = m.AppliedLocalPose();
            const int ul = m.NodeIndex(Bone(FPBody::kBoneUpperArm[0])), ur = m.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));
            if (pose.empty() || (int)pose.size() != m.NodeCount() || ul < 0 || ur < 0) break;
            std::vector<int> parents(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
            CopyArmShape(m, *armRig, m_ArmsWeight, pose, parents, m_BoneMap, cfg.ClavicleFollow);
            std::vector<glm::mat4> globals;
            IK::ComputeGlobals(pose, parents, globals);
            left = IK::Position(globals[ul]);
            right = IK::Position(globals[ur]);
            break;
        }
    };
    auto shouldersNow = [&]() {
        glm::vec3 l, r;
        shoulderPair(l, r);
        return 0.5f * (l + r);
    };
    const glm::vec3 shouldersAnimated = shouldersNow();
    const float viewPitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f));
    const float spineAim = FirstPersonBodySpineAim(viewPitch, cfg.SpineAim, cfg.SpineAimDown);
    if (spineAim > 0.0f || cfg.SpineTwist > 0.0f) ApplySpineAim(camera, spineAim, m_Twist * cfg.SpineTwist);
    // Armed, the chest takes the arms rig's stance: its shoulder line, measured against the camera. The
    // rig is authored bladed (the left shoulder ~13 cm ahead of the right, ~20 degrees): squared to the
    // view, the body's left shoulder sat ~7 cm behind the rig's, and the support hand came off the gun.
    const float lineMatch = std::clamp(cfg.ShoulderLineMatch, 0.0f, 1.0f) * std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    bool matched = false;
    if (armRig && lineMatch > 1e-3f && reg.valid(weaponArms)) {
        glm::mat4 sl(1.0f), sr(1.0f);
        if (armRig->NodeTransform(FPBody::kBoneUpperArm[0], sl) && armRig->NodeTransform(FPBody::kBoneUpperArm[1], sr)) {
            const glm::vec3 across = glm::mat3(world.ComposeWorldTransform(weaponArms)) * (glm::vec3(sl[3]) - glm::vec3(sr[3]));
            const glm::vec3 inCamera(glm::dot(across, camera.Right()), glm::dot(across, camera.Up()), glm::dot(across, camera.Front()));
            if (!m_HaveRigShoulderLine) { m_RigShoulderLine = inCamera; m_HaveRigShoulderLine = true; }
            m_RigShoulderLine += (inCamera - m_RigShoulderLine) * Follow(dt, 0.15f);
            const glm::vec3 rigWorld = camera.Right() * m_RigShoulderLine.x + camera.Up() * m_RigShoulderLine.y + camera.Front() * m_RigShoulderLine.z;
            const glm::vec3 rigModel = glm::inverse(YawRotation(m_Yaw)) * rigWorld;
            // Unarmed the chest squares to the view; easing between, the target is between the two.
            const glm::vec3 square(std::cos(m_Twist), 0.0f, -std::sin(m_Twist));
            const glm::vec3 target = glm::mix(square * glm::length(rigModel), rigModel, lineMatch);
            // Twice: the clavicles the rig lends the arms move the shoulders a little as the chest turns.
            for (int pass = 0; pass < 2; ++pass) {
                glm::vec3 l, r;
                shoulderPair(l, r);
                // The rig's line tilts as its collarbones swing (a reload's reach lifts its support shoulder):
                // matched whole, the chest rolled that shoulder up at the camera. The body takes the rig's tilt
                // only by Clavicle Follow; the stance's blade (its turn about the vertical) is matched in full.
                const glm::vec3 lineTarget = FirstPersonBodyShoulderLineTilt(l - r, target, cfg.ClavicleFollow);
                const glm::quat turn = FirstPersonBodyShoulderLineTurn(l - r, lineTarget, 1.0f);
                if (2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f)) > 0.002f) ApplySpineRotation(turn);
            }
            matched = true;
            auto blade = [](const glm::vec3& v) { return glm::degrees(std::atan2(v.z, v.x)); }; // + = left shoulder forward
            glm::vec3 l, r;
            shoulderPair(l, r);
            const glm::vec3 chest = YawRotation(m_Yaw) * (l - r);
            BodyDebug::Info().RigBlade = blade(m_RigShoulderLine);
            BodyDebug::Info().ChestBlade = blade(glm::vec3(glm::dot(chest, camera.Right()), 0.0f, glm::dot(chest, camera.Front())));
        }
    }
    if (!matched) m_HaveRigShoulderLine = false;
    // Close the loop on the chest's heading: the clips' own spine yaw (a turn leads with the chest)
    // and the pitch tilt would leave the shoulder line off the view's, and one shoulder then sits
    // further from the gun than the other (the arm stretches). Turn the spine by what is left.
    if (cfg.SpineTwist > 0.0f && !matched) {
        glm::vec3 l, r;
        shoulderPair(l, r);
        const glm::vec3 across = l - r; // model +X (the mannequin's left) when the chest faces forward
        if (across.x * across.x + across.z * across.z > 1e-6f) {
            const float chestYaw = std::atan2(-across.z, across.x); // rotating +X about +Y by t gives z = -sin t
            const float error = FirstPersonBodyWrapAngle(m_Twist - chestYaw);
            if (std::abs(error) > 0.002f) ApplySpineAim(camera, 0.0f, error * std::min(cfg.SpineTwist, 1.0f));
        }
    }
    if (!haveHead) return;

    // The camera into the head: smoothed in the body's own frame, so it never trails the move.
    // The bob is the clips' head motion; what the chest's tilt does to the head goes on in full,
    // so the shoulders stay the same distance from the eye as the view pitches.
    glm::vec3 tilt(0.0f);
    if (model->NodeTransform(model->NodeName(m_HeadNode), head)) tilt = glm::vec3(head[3]) - headAnimated;
    const glm::vec3 target = FirstPersonBodyEye(m_RestHead, headAnimated, cfg.HeadBob, cfg.CameraOffset);
    if (!m_HaveEye) { m_Eye = target; m_HaveEye = true; }
    m_Eye += (target - m_Eye) * Follow(dt, cfg.CameraSmoothing);

    const auto& t = reg.get<TransformComponent>(m_Body);
    const glm::quat yaw = YawRotation(m_Yaw);
    glm::vec3 eyeWorld = m_Feet + yaw * (t.Scale * (m_Eye + tilt));

    // The chest's view: the view's heading, pitched only as far as the chest is (its share of the view's
    // pitch). Arm Steadiness holds the shoulders in it. (The eye is hung off the shoulders in the view's
    // own frame - the rig's shoulders pivot with the whole view, so that is where the hands are reached
    // from; looking down, the chest pitching further, Spine Aim Down, keeps the camera out of it.)
    const float chestPitch = viewPitch * spineAim;
    const glm::vec3 flat(std::sin(m_ViewYaw), 0.0f, std::cos(m_ViewYaw));
    const glm::vec3 chestFront = flat * std::cos(chestPitch) + glm::vec3(0.0f, std::sin(chestPitch), 0.0f);
    const glm::quat toChest(camera.Front(), chestFront); // the view's frame onto the chest's (about the view's right)
    const glm::vec3 chestRight = toChest * camera.Right(), chestUp = toChest * camera.Up();

    if (m_ArmsWeight <= 1e-3f) { m_HaveShoulders = false; m_HaveRigOffset = false; }
    // Weapon arms: the eye goes where the arms rig's is relative to its shoulders, measured on the
    // rig (camera bone to its upper arms, in the camera's frame), put on the body's own shoulders.
    // The rig's hands are placed relative to the eye, so they are then within the body's reach.
    if (m_ArmsWeight > 1e-3f && haveShoulders && weaponArms != entt::null && reg.valid(weaponArms) &&
        reg.all_of<RenderableComponent>(weaponArms)) {
        const Model* rig = reg.get<RenderableComponent>(weaponArms).ModelRef.get();
        glm::mat4 sl(1.0f), sr(1.0f), cb(1.0f);
        if (rig && rig->NodeTransform(FPBody::kBoneUpperArm[0], sl) && rig->NodeTransform(FPBody::kBoneUpperArm[1], sr)) {
            if (!rigCameraBone.empty()) rig->NodeTransform(rigCameraBone, cb);
            const glm::mat4 rigWorld = world.ComposeWorldTransform(weaponArms);
            const glm::vec3 fromEye = glm::mat3(rigWorld) * (0.5f * (glm::vec3(sl[3]) + glm::vec3(sr[3])) - glm::vec3(cb[3]));
            const glm::vec3 inCamera(glm::dot(fromEye, camera.Right()), glm::dot(fromEye, camera.Up()), glm::dot(fromEye, camera.Front()));
            // The clips sway their shoulders about the camera; that must not move the view, so only a
            // slow drift of this offset is followed.
            if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
            m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);

            const glm::vec3 shoulders = shouldersNow();
            const glm::vec3 toRest = shoulders - shouldersAnimated; // the chest's tilt at the shoulders
            // The eye follows Head Bob of the shoulders' motion about their slow average - not about
            // the bind pose, whose shoulders sit elsewhere than the standing pose's, which would put
            // the shoulders (and so the hands' reach) that far off.
            if (!m_HaveShoulders) { m_ShouldersSlow = m_Shoulders = shouldersAnimated; m_HaveShoulders = true; }
            m_ShouldersSlow += (shouldersAnimated - m_ShouldersSlow) * Follow(dt, 0.5f);
            const glm::vec3 shoulderTarget = m_ShouldersSlow + (shouldersAnimated - m_ShouldersSlow) * std::clamp(cfg.HeadBob, 0.0f, 1.0f);
            m_Shoulders += (shoulderTarget - m_Shoulders) * Follow(dt, cfg.CameraSmoothing);
            // The eye may trail or under-follow the shoulders by only this much: a clip that throws the
            // shoulders about (a stop pulling the body back) would otherwise carry them out of the
            // arms' reach and a hand would come off the gun. Bigger motions move the camera with them.
            const float kEyeSlack = cfg.EyeSlack;
            const glm::vec3 gap = shouldersAnimated - m_Shoulders;
            if (const float gapLen = glm::length(gap); gapLen > kEyeSlack) m_Shoulders = shouldersAnimated - gap / gapLen * kEyeSlack;
            BodyDebug::Info().EyeSlack = glm::length(shouldersAnimated - m_Shoulders);
            // A little slack: the shoulders sit this much further toward the gun than the rig's, so the
            // support arm is never at full stretch (a straight arm jumps at every small motion).
            const float kReachSlack = cfg.ReachSlack;
            const glm::vec3 offset = camera.Right() * m_RigEyeToShoulders.x + camera.Up() * m_RigEyeToShoulders.y +
                                     camera.Front() * (m_RigEyeToShoulders.z + kReachSlack);
            // Armed Eye Offset: the rig's camera sits lower against its shoulders than an eye does; lifted back up -
            // looking level, where the chest's top was in the view's corner. It goes looking down (the chest is then
            // under the eye, and lifted, part of it came out of Near Hide and its cut edge showed) and looking up (the
            // lifted gun took the support hand past its reach, off the handguard).
            const glm::vec3 lift = (kRight * cfg.ArmedEyeOffset.x + glm::vec3(0.0f, cfg.ArmedEyeOffset.y, 0.0f) + kForward * cfg.ArmedEyeOffset.z) *
                                   FirstPersonBodyArmedEyeLift(viewPitch);
            const glm::vec3 fromShoulders = m_Feet + yaw * (t.Scale * (m_Shoulders + toRest + lift)) - offset;
            // Remember where this puts the eye against the head's: unarmed the eye keeps that height, or
            // the camera would jump when the gun is holstered.
            const glm::vec3 delta = glm::inverse(yaw) * (fromShoulders - eyeWorld);
            if (!m_HaveArmedEye) { m_ArmedEyeDelta = delta; m_HaveArmedEye = true; }
            if (m_ArmsWeight > 0.99f) m_ArmedEyeDelta += (delta - m_ArmedEyeDelta) * Follow(dt, 0.5f);
            eyeWorld = glm::mix(eyeWorld, fromShoulders, std::min(m_ArmsWeight, 1.0f));
        }
    }
    if (m_HaveArmedEye) eyeWorld += yaw * m_ArmedEyeDelta * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    // Looking down, the eye comes forward over the chest as a head does, pitching at the neck. The camera
    // rides at the head bone (the skull's base, over the neck), so straight down it looked into the
    // torso's neck opening: the torso was seen from inside (culled away) and the legs through it.
    eyeWorld += flat * (cfg.LookDownPush * FirstPersonBodyLookDown(viewPitch, cfg.LookDownStart));
    if (BodyDebug::Enabled()) {
        const glm::vec3 shoulders = m_Feet + yaw * (t.Scale * m_Shoulders);
        BodyDebug::Cross(eyeWorld, 0.04f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));       // the camera
        BodyDebug::Cross(shoulders, 0.04f, glm::vec4(1.0f, 0.3f, 1.0f, 1.0f));      // the shoulders the eye is anchored to
        BodyDebug::Line(shoulders, eyeWorld, glm::vec4(1.0f, 0.3f, 1.0f, 0.7f));
    }
    m_CameraApplied = eyeWorld - camera.Position;
    camera.Position = eyeWorld;
    // Arm Steadiness holds the shoulders in this frame: it moves with the eye and the chest's pitch, so
    // only the gait's sway shows in it (the rig's own frame pitches with the view, the chest less).
    m_ChestView = glm::mat4(glm::vec4(chestRight, 0.0f), glm::vec4(chestUp, 0.0f), glm::vec4(chestFront, 0.0f), glm::vec4(eyeWorld, 1.0f));
    m_HaveChestView = true;
}

// Puts each foot on the ground under it: a ray down from the animated foot gives how far the ground
// is above / below the capsule's, the pelvis drops to the lower foot, and the legs are re-solved to
// the offset feet (tilted toward the ground while planted). On every piece so they stay one skeleton.
bool FirstPersonBody::BoneWorld(const World& world, const std::string& standard, glm::vec3& out) const {
    if (!IsActive() || !world.Registry.valid(m_Body)) return false;
    const auto& reg = world.Registry;
    std::string want = reg.get<FirstPersonBodyComponent>(m_Body).ArmsPiece;
    for (char& c : want) c = (char)std::tolower((unsigned char)c);
    auto isArms = [&](size_t k) {
        if (want.empty() || !reg.all_of<NameComponent>(m_Pieces[k])) return false;
        std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        return name.find(want) != std::string::npos;
    };
    // The world twins when poses are split (what every view but the player's camera shows), else the pieces.
    const bool twins = !m_Twins.empty();
    const auto& ents = twins ? m_Twins : m_Pieces;
    const auto& models = twins ? m_TwinModels : m_Models;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k < m_Pieces.size() && k < models.size() && k < ents.size(); ++k) {
            if (!reg.valid(ents[k]) || !models[k]) continue;
            if (pass == 0 ? !isArms(k) : m_Pieces[k] != m_Driver) continue;
            glm::mat4 n(1.0f);
            if (!models[k]->NodeTransform(Bone(standard), n)) continue;
            out = glm::vec3(world.ComposeWorldTransform(ents[k]) * n[3]);
            return true;
        }
    return false;
}

void FirstPersonBody::SkinnedPoints(const World& world, BodyRegion region, std::vector<glm::vec3>& points, std::vector<int>* pieceOf) const {
    points.clear();
    if (pieceOf) pieceOf->clear();
    if (!IsActive()) return;
    const auto& reg = world.Registry;
    const bool twins = !m_Twins.empty();
    const auto& ents = twins ? m_Twins : m_Pieces;
    const auto& models = twins ? m_TwinModels : m_Models;
    auto& cache = region == BodyRegion::Head ? m_HeadVerts : m_TorsoVerts;
    for (size_t k = 0; k < ents.size() && k < models.size(); ++k) {
        if (!reg.valid(ents[k]) || !models[k]) continue;
        const Model& m = *models[k];
        auto found = cache.find(&m);
        if (found == cache.end()) {
            // Vertices whose heaviest bone is the region's: the neck and head, or the pelvis and spine.
            std::vector<int> ids;
            if (region == BodyRegion::Head)
                for (const std::string& b : {std::string("neck_01"), std::string("neck_02"), reg.get<FirstPersonBodyComponent>(m_Body).HeadBone})
                    ids.push_back(m.BoneId(Bone(b)));
            else
                for (const char* b : {"pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05"}) ids.push_back(m.BoneId(Bone(b)));
            std::vector<std::pair<int, int>> verts;
            for (int i = 0; i < m.MeshCount(); ++i) {
                const auto& sv = m.MeshSkinVertices(i);
                for (int v = 0; v < (int)sv.size(); ++v) {
                    int top = -1;
                    float w = 0.0f;
                    for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
                        if (sv[v].BoneIDs[j] >= 0 && sv[v].Weights[j] > w) { w = sv[v].Weights[j]; top = sv[v].BoneIDs[j]; }
                    if (top >= 0 && std::find(ids.begin(), ids.end(), top) != ids.end()) verts.push_back({i, v});
                }
            }
            // The torso is a big mesh (a hoodie is tens of thousands of vertices) and its surface is smooth: every
            // few thousandth vertex is plenty to keep an elbow out of it, and keeps the frame cheap.
            constexpr size_t kMaxTorsoPoints = 3000;
            if (region == BodyRegion::Torso && verts.size() > kMaxTorsoPoints) {
                const size_t stride = (verts.size() + kMaxTorsoPoints - 1) / kMaxTorsoPoints;
                size_t n = 0;
                for (size_t i = 0; i < verts.size(); i += stride) verts[n++] = verts[i];
                verts.resize(n);
            }
            // The head is dense (a detailed face, its mouth and eyes: ~50k vertices) for what only keeps a gun a
            // few centimetres off it: one vertex per 5 mm cell is the same surface to within 4.3 mm - well under
            // the 1 cm steps the gun is pushed out in - at a tenth of the skinning each frame.
            if (region == BodyRegion::Head) {
                constexpr float kCell = 0.005f;
                std::unordered_set<std::uint64_t> taken;
                taken.reserve(verts.size());
                size_t n = 0;
                for (const auto& iv : verts) {
                    const glm::vec3& p = m.MeshSkinVertices(iv.first)[iv.second].Position;
                    const auto cell = [&](float x) { return (std::uint64_t)(std::int64_t)std::floor(x / kCell) & 0x1FFFFFu; };
                    if (taken.insert(cell(p.x) | cell(p.y) << 21 | cell(p.z) << 42).second) verts[n++] = iv;
                }
                verts.resize(n);
            }
            found = cache.emplace(&m, std::move(verts)).first;
        }
        if (found->second.empty()) continue;
        const glm::mat4 toWorld = world.ComposeWorldTransform(ents[k]);
        // The palette once per piece, not a lookup per influence per vertex; the sums are the same.
        thread_local std::vector<glm::mat4> palette;
        palette.resize((size_t)std::max(m.BoneCount(), 0));
        for (int b = 0; b < (int)palette.size(); ++b) palette[b] = m.FinalBoneMatrix(b);
        points.reserve(points.size() + found->second.size());
        for (const auto& [i, v] : found->second) {
            const ModelMesh::SkinVertex& sv = m.MeshSkinVertices(i)[v];
            glm::vec4 p(0.0f);
            float total = 0.0f;
            const glm::vec4 pos(sv.Position, 1.0f);
            for (int j = 0; j < MAX_BONE_INFLUENCE; ++j)
                if (sv.BoneIDs[j] >= 0 && sv.Weights[j] > 0.0f) {
                    const glm::mat4& bone = sv.BoneIDs[j] < (int)palette.size() ? palette[sv.BoneIDs[j]] : glm::mat4(1.0f);
                    p += sv.Weights[j] * (bone * pos);
                    total += sv.Weights[j];
                }
            if (total <= 0.0f) continue;
            points.push_back(glm::vec3(toWorld * glm::vec4(glm::vec3(p) / total, 1.0f)));
            if (pieceOf) pieceOf->push_back((int)k);
        }
    }
}

void FirstPersonBody::ElbowTorsoGaps(const World& world, float (&gaps)[2]) const {
    gaps[0] = gaps[1] = -1.0f;
    std::vector<glm::vec3> torso;
    SkinnedPoints(world, BodyRegion::Torso, torso);
    if (torso.empty()) return;
    static const char* const kArm[2][3] = {{"upperarm_l", "lowerarm_l", "hand_l"}, {"upperarm_r", "lowerarm_r", "hand_r"}};
    for (int s = 0; s < 2; ++s) {
        glm::vec3 a, b, c;
        if (BoneWorld(world, kArm[s][0], a) && BoneWorld(world, kArm[s][1], b) && BoneWorld(world, kArm[s][2], c))
            gaps[s] = FirstPersonBodyElbowGap(torso, a, b, c);
    }
}

float FirstPersonBody::MeshGap(const World& world, BodyRegion region, const glm::vec3& a, const glm::vec3& b, std::string* piece, float* along) const {
    std::vector<glm::vec3> points;
    std::vector<int> pieceOf;
    SkinnedPoints(world, region, points, &pieceOf);
    const glm::vec3 ab = b - a;
    const float abLen2 = std::max(glm::dot(ab, ab), 1e-9f);
    float best = -1.0f;
    for (size_t n = 0; n < points.size(); ++n) {
        const float t = std::clamp(glm::dot(points[n] - a, ab) / abLen2, 0.0f, 1.0f);
        const float d = glm::length(points[n] - (a + ab * t));
        if (best < 0.0f || d < best) {
            best = d;
            if (along) *along = t;
            if (piece) {
                const entt::entity e = m_Pieces[pieceOf[n]];
                *piece = world.Registry.all_of<NameComponent>(e) ? world.Registry.get<NameComponent>(e).Name : std::string();
            }
        }
    }
    return best;
}

void FirstPersonBody::ApplyFootIK(World& world, const FirstPersonBodyComponent& cfg, float dt) {
    auto& reg = world.Registry;
    const bool on = cfg.FootIK && m_Grounded && PhysicsWorld::HasCharacter() && reg.valid(m_Driver) &&
                    !reg.get<AnimatorControllerComponent>(m_Driver).HasTag(FPBody::kTagAirborne);
    m_FootWeight += ((on ? 1.0f : 0.0f) - m_FootWeight) * Follow(dt, cfg.FootIKFade);
    if (m_FootWeight < 1e-3f) {
        m_HaveFoot = false;
        m_FootPlanted[0] = m_FootPlanted[1] = false;
        m_FootLockWeight[0] = m_FootLockWeight[1] = 0.0f;
        return;
    }
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return;
    const Model& drv = *rc->ModelRef;
    const IK::Pose driverPose = drv.AppliedLocalPose();
    if (driverPose.empty() || (int)driverPose.size() != drv.NodeCount()) return;
    const int footNode[2] = {drv.NodeIndex(Bone(FPBody::kBoneFoot[0])), drv.NodeIndex(Bone(FPBody::kBoneFoot[1]))};
    if (footNode[0] < 0 || footNode[1] < 0) return;

    const auto& t = reg.get<TransformComponent>(m_Body);
    const float scale = std::max(t.Scale.y, 1e-3f);
    const glm::quat yaw = YawRotation(m_Yaw);
    std::vector<int> parents(driverPose.size());
    for (int i = 0; i < (int)driverPose.size(); ++i) parents[i] = drv.NodeParent(i);
    std::vector<glm::mat4> globals;
    IK::ComputeGlobals(driverPose, parents, globals);

    const float maxDrop = std::max(cfg.FootIKMaxDrop, 0.0f);
    float animHeight[2];
    glm::vec3 lockShift[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // world, horizontal: animated foot -> pinned foot
    // No pinning while the body turns on the spot (the feet must step) or the heading swings.
    const bool yawSteady = !m_Turning && std::abs(FirstPersonBodyWrapAngle(m_Yaw - m_FootYaw)) < glm::radians(2.0f);
    m_FootYaw = m_Yaw;
    for (int s = 0; s < 2; ++s) {
        const glm::vec3 footWorld = m_Feet + yaw * (scale * IK::Position(globals[footNode[s]]));
        animHeight[s] = footWorld.y - m_Feet.y;
        // Foot lock: a planted foot stays where it landed instead of sliding when the animation and the
        // capsule's travel disagree a little; it lets go when the foot lifts or the mismatch gets big.
        const bool planted = animHeight[s] < cfg.FootPlantedHeight && yawSteady;
        if (planted) {
            if (!m_FootPlanted[s]) { m_FootPlanted[s] = true; m_FootLock[s] = footWorld; }
            const glm::vec3 drift(footWorld.x - m_FootLock[s].x, 0.0f, footWorld.z - m_FootLock[s].z);
            if (glm::dot(drift, drift) > cfg.FootLockDrift * cfg.FootLockDrift) m_FootLock[s] = footWorld; // too far: plant again here
        } else {
            m_FootPlanted[s] = false;
        }
        m_FootLockWeight[s] += ((m_FootPlanted[s] ? 1.0f : 0.0f) - m_FootLockWeight[s]) * Follow(dt, m_FootPlanted[s] ? cfg.FootLockEaseIn : cfg.FootLockEaseOut);
        lockShift[s] = glm::vec3(m_FootLock[s].x - footWorld.x, 0.0f, m_FootLock[s].z - footWorld.z) * (m_FootLockWeight[s] * m_FootWeight);
        float offset = 0.0f;
        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        const float origin[3] = {footWorld.x, footWorld.y + cfg.FootRayUp, footWorld.z};
        const float down[3] = {0.0f, -1.0f, 0.0f};
        QueryFilter filter;
        filter.HitTriggers = 0;
        RaycastHit hit;
        const bool rayHit = PhysicsWorld::RaycastSolid(origin, down, cfg.FootRayLength, filter, hit) && hit.Hit;
        if (rayHit) {
            offset = std::clamp(hit.Point[1] - m_Feet.y, -maxDrop, cfg.FootMaxRaise);
            normal = glm::normalize(glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]));
            if (normal.y < 0.5f) normal = glm::vec3(0.0f, 1.0f, 0.0f); // a wall, not a floor
        }
        if (BodyDebug::Enabled()) {
            const glm::vec3 from(origin[0], origin[1], origin[2]);
            const glm::vec3 to = rayHit ? glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]) : from + glm::vec3(0.0f, -cfg.FootRayLength, 0.0f);
            BodyDebug::Line(from, to, rayHit ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.3f, 0.3f, 1.0f)); // the ground ray
            if (rayHit) BodyDebug::Cross(to, 0.04f, glm::vec4(0.3f, 1.0f, 0.4f, 1.0f));
            BodyDebug::Cross(footWorld, 0.05f, m_FootPlanted[s] ? glm::vec4(0.3f, 1.0f, 0.4f, 1.0f) : glm::vec4(1.0f, 0.4f, 0.3f, 1.0f)); // the animated foot: green planted
            if (m_FootPlanted[s]) BodyDebug::Cross(m_FootLock[s], 0.03f, glm::vec4(0.3f, 0.9f, 1.0f, 1.0f));                            // where it is pinned
        }
        if (!m_HaveFoot) { m_FootOffset[s] = offset; m_FootNormal[s] = normal; }
        m_FootOffset[s] += (offset - m_FootOffset[s]) * Follow(dt, cfg.FootOffsetEase);
        m_FootNormal[s] = glm::normalize(m_FootNormal[s] + (normal - m_FootNormal[s]) * Follow(dt, cfg.FootNormalEase));
    }
    m_HaveFoot = true;
    {
        BodyDebug::Snapshot& d = BodyDebug::Info();
        d.FootWeight = m_FootWeight;
        for (int s = 0; s < 2; ++s) { d.FootOffset[s] = m_FootOffset[s]; d.FootPlanted[s] = m_FootPlanted[s]; d.FootLock[s] = m_FootLockWeight[s]; }
    }

    const float pelvisDelta = FirstPersonBodyFootPelvis(m_FootOffset[0], m_FootOffset[1], maxDrop, cfg.PelvisMaxRaise) * m_FootWeight;
    static const char* const kLegs[2][3] = {{FPBody::kBoneThigh[0], FPBody::kBoneCalf[0], FPBody::kBoneFoot[0]}, {FPBody::kBoneThigh[1], FPBody::kBoneCalf[1], FPBody::kBoneFoot[1]}};
    const glm::quat yawInverse = glm::inverse(yaw);
    for (const auto& mp : m_Models) {
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        const int pelvis = m.NodeIndex(Bone(FPBody::kBonePelvis));
        if (pelvis < 0) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        IK::ComputeGlobals(pose, parents, globals);
        IK::OffsetBone(pose, parents, globals, pelvis, glm::vec3(0.0f, pelvisDelta / scale, 0.0f),
                       glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f));
        for (int s = 0; s < 2; ++s) {
            const int thigh = m.NodeIndex(Bone(kLegs[s][0])), calf = m.NodeIndex(Bone(kLegs[s][1])), foot = m.NodeIndex(Bone(kLegs[s][2]));
            if (thigh < 0 || calf < 0 || foot < 0) continue;
            const glm::vec3 target = IK::Position(globals[foot]) +
                                     glm::vec3(0.0f, (m_FootOffset[s] * m_FootWeight - pelvisDelta) / scale, 0.0f) +
                                     yawInverse * lockShift[s] / scale;
            // The foot lies on the ground while it is planted: its own rotation tilted to the normal.
            const float planted = 1.0f - std::clamp((animHeight[s] - 0.06f) / 0.09f, 0.0f, 1.0f);
            const glm::vec3 normal = yawInverse * m_FootNormal[s];
            const float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), glm::radians(cfg.FootTiltMax)) * planted * m_FootWeight;
            glm::quat footRot = IK::Rotation(globals[foot]);
            const glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
            if (angle > 1e-4f && glm::dot(axis, axis) > 1e-8f) footRot = glm::angleAxis(angle, glm::normalize(axis)) * footRot;
            // Flat ground and nothing pinned: leave the clip's own pose alone.
            if (glm::length(target - IK::Position(globals[foot])) < 5e-4f && angle < 1e-3f) continue;
            IK::SolveTwoBone(pose, parents, globals, thigh, calf, foot, target, &footRot, 1.0f);
        }
        m.ApplyLocalPose(pose);
    }
}

// Tilts the spine by `amount` of the camera's pitch and twists it by `twist` radians, spread evenly over spine_01..spine_05 (pitch about
// the model's X axis, twist about Y), on every piece so they stay one skeleton.
void FirstPersonBody::ApplySpineAim(const Camera& camera, float amount, float twist) {
    const float pitch = std::asin(std::clamp(camera.Front().y, -1.0f, 1.0f)); // up is positive
    // Rotating +Z about +X by theta gives (0, -sin, cos): looking up needs a negative angle.
    // The twist about the model's up turns the chest toward the view (positive = to the left).
    RotateSpine([&](int n) {
        return glm::angleAxis(twist / (float)n, glm::vec3(0.0f, 1.0f, 0.0f)) *
               glm::angleAxis(-pitch * amount / (float)n, glm::vec3(1.0f, 0.0f, 0.0f));
    });
}

void FirstPersonBody::ApplySpineRotation(const glm::quat& modelDelta) {
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    RotateSpine([&](int n) { return glm::normalize(glm::slerp(none, modelDelta, 1.0f / (float)n)); });
}

void FirstPersonBody::RotateSpine(const std::function<glm::quat(int)>& stepFor) {
    const char* const* kSpine = FPBody::kBoneSpine;
    RotateChain({kSpine[0], kSpine[1], kSpine[2], kSpine[3], kSpine[4]}, stepFor);
}

void FirstPersonBody::RotateChain(const std::vector<std::string>& chain, const std::function<glm::quat(int)>& stepFor,
                                  const std::vector<std::shared_ptr<Model>>* models) {
    std::vector<int> parents;
    std::vector<glm::mat4> globals;
    const auto& list = models ? *models : m_Models;
    for (size_t k = 0; k < list.size(); ++k) {
        const auto& mp = list[k];
        if (!mp) continue;
        Model& m = *mp;
        IK::Pose pose = m.AppliedLocalPose();
        if (pose.empty() || (int)pose.size() != m.NodeCount()) continue;
        std::vector<int> bones;
        for (const std::string& name : chain)
            if (const int i = m.NodeIndex(Bone(name)); i >= 0) bones.push_back(i);
        if (bones.empty()) continue;
        // A piece skinning nothing the chain moves (legs, feet) draws the same either way. The driver always
        // turns: its head and shoulders are read for the camera and the arms.
        if ((k >= m_Pieces.size() || m_Pieces[k] != m_Driver) && !SkinsUnder(m, bones, chain.front())) continue;
        parents.resize(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
        // Only the chain and what's above it are read before ApplyLocalPose re-derives the rest.
        globals.resize(pose.size());
        IK::RefreshPath(pose, parents, globals, -1, bones.back());
        const glm::quat step = stepFor((int)bones.size());
        // Only the next spine bone's global is read before ApplyLocalPose re-derives the lot, so
        // refresh just the path to it rather than the whole upper body after every bone.
        for (size_t b = 0; b < bones.size(); ++b) {
            const int i = bones[b];
            if (b > 0) IK::RefreshPath(pose, parents, globals, bones[b - 1], i);
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

void FirstPersonBody::ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt, const Camera* camera,
                                     const FirstPersonWorldGunInput* gun) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) return;
    const FirstPersonBodyComponent& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    // The world twins start from the pieces' pose as it stands now (the clips, the spine, the feet): from
    // here the pieces' arms go to the rig's hands, the twins' to the world gun's.
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
        if (e == m_Body || !reg.valid(e) || !reg.all_of<NameComponent>(e)) continue;
        std::string name = reg.get<NameComponent>(e).Name, want = reg.get<FirstPersonBodyComponent>(m_Body).ArmsPiece;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        if (want.empty() || name.find(want) == std::string::npos) continue;
        if (viewModelArms) {
            if (!reg.all_of<ViewModelTag>(e)) { reg.emplace<ViewModelTag>(e); m_ArmsTagged.push_back(e); }
        } else if (reg.all_of<ViewModelTag>(e)) {
            reg.remove<ViewModelTag>(e);
        }
        // Easing off a holstered gun the pose is still the rig's (its hands, no gun to hold): shown, it
        // would be a pair of hands hanging in the view for a few frames. Hidden until it has settled.
        auto& rc = reg.get<RenderableComponent>(e);
        const bool easeOut = armsHidden;
        if (easeOut && !m_ArmsEasedOut) { m_ArmsShadow = (int)rc.CastShadows; m_ArmsEasedOut = true; }
        if (easeOut) rc.CastShadows = RenderableComponent::ShadowCasting::ShadowsOnly;
        else if (m_ArmsEasedOut) { rc.CastShadows = (RenderableComponent::ShadowCasting)m_ArmsShadow; m_ArmsEasedOut = false; }
    }
    if (m_ArmsWeight < 1e-3f || !haveRig) {
        m_HaveShoulderAnchor[0] = m_HaveShoulderAnchor[1] = false;
        m_HaveElbowAim[0] = m_HaveElbowAim[1] = false;
        m_WorldHaveShoulderAnchor[0] = m_WorldHaveShoulderAnchor[1] = false;
        m_WorldHaveElbowAim[0] = m_WorldHaveElbowAim[1] = false;
        m_WorldElbowClear[0] = m_WorldElbowClear[1] = 0.0f;
        m_WorldGunShift = glm::vec3(0.0f);
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
    std::string armsName = cfg.ArmsPiece;
    for (char& c : armsName) c = (char)std::tolower((unsigned char)c);

    struct Side { const char *Clavicle, *Upper, *Lower, *Hand; };
    static const Side kSides[2] = {{FPBody::kBoneClavicle[0], FPBody::kBoneUpperArm[0], FPBody::kBoneLowerArm[0], FPBody::kBoneHand[0]},
                                   {FPBody::kBoneClavicle[1], FPBody::kBoneUpperArm[1], FPBody::kBoneLowerArm[1], FPBody::kBoneHand[1]}};
    // The rig's hands, exactly: the arms piece is drawn with the gun in the same projection.
    glm::vec3 handPos[2]{};
    glm::quat handRot[2]{};
    bool haveHand[2] = {false, false};
    glm::vec3 rigShoulder[2]{}, rigElbow[2]{}; // world: the rig's upper arm and elbow (its bend plane)
    bool haveRigElbow[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        glm::mat4 h(1.0f), u(1.0f), l(1.0f);
        if (!rig->NodeTransform(kSides[s].Hand, h)) continue;
        const glm::mat4 w = rigWorld * h;
        handPos[s] = glm::vec3(w[3]);
        handRot[s] = IK::Rotation(w);
        haveHand[s] = true;
        if (rig->NodeTransform(kSides[s].Upper, u) && rig->NodeTransform(kSides[s].Lower, l)) {
            rigShoulder[s] = glm::vec3(rigWorld * u[3]);
            rigElbow[s] = glm::vec3(rigWorld * l[3]);
            haveRigElbow[s] = true;
        }
    }

    // The arms onto the hands in handPos / handRot: the pieces (first person), then - with the targets
    // moved to the world gun - the twins. Each keeps its own steadiness and elbow state.
    auto solveArms = [&](const std::vector<std::shared_ptr<Model>>& models, const std::vector<entt::entity>& ents,
                         glm::vec3 (&anchor)[2], bool (&haveAnchor)[2], glm::vec3 (&elbowAim)[2], bool (&haveElbowAim)[2],
                         bool debug) {
        // The shoulders' moves (steadying, shrug) are worked out on one piece - the arms, else the first - and
        // the same world offset is given to every piece's clavicle, so the arms stay on the torso's shoulders.
        // Worked out per piece, the arms came off the chest (in any view but the camera's, and in shadows).
        std::vector<size_t> order;
        for (size_t k = 0; k < m_Models.size(); ++k) {
            bool isArms = false;
            if (!armsName.empty() && reg.valid(m_Pieces[k]) && reg.all_of<NameComponent>(m_Pieces[k])) {
                std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
                for (char& c : name) c = (char)std::tolower((unsigned char)c);
                isArms = name.find(armsName) != std::string::npos;
            }
            if (isArms) order.insert(order.begin(), k);
            else order.push_back(k);
        }
        glm::quat clavTurn[2] = {glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}; // world: the source's, given to the rest
        bool sourceDone = false;
        // The shoulders move by turning each clavicle about its root (the collarbone's inner end) - never by
        // sliding it, which stretched the skin between the neck and the shoulder into a hunch. Turned so the
        // upper arm heads for `want` (model space), by at most `maxAngle`; returns the turn made (model space).
        const float shoulderMax = glm::radians(std::clamp(cfg.ShoulderMaxAngle, 0.0f, 90.0f));
        auto turnClavicle = [](IK::Pose& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals, int clav, int upper,
                               const glm::vec3& want, float maxAngle) {
            const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::vec3 root = IK::Position(globals[clav]);
            const glm::vec3 from = IK::Position(globals[upper]) - root, to = want - root;
            if (maxAngle <= 0.0f || glm::dot(from, from) < 1e-8f || glm::dot(to, to) < 1e-8f) return none;
            glm::quat q(glm::normalize(from), glm::normalize(to));
            const float angle = 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f));
            if (angle < 1e-5f) return none;
            if (angle > maxAngle) q = glm::slerp(none, q, maxAngle / angle);
            IK::OffsetBone(pose, parents, globals, clav, glm::vec3(0.0f), q, root);
            return q;
        };
        auto angleOf = [](const glm::quat& q) { return 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)); };
        // Reach Lean: a hand still out of reach once its collarbone has turned all it may (the gun pitches
        // with the whole view, the chest only by Spine Aim) - the chest leans toward it, at the top of the
        // spine, by at most Reach Lean Max. Worked out on the source piece like the shoulders, given to all.
        glm::quat chestTurn(1.0f, 0.0f, 0.0f, 0.0f); // world
        const float leanMax = glm::radians(std::clamp(cfg.ReachLeanMax, 0.0f, 60.0f));

        std::vector<int> parents;
        std::vector<glm::mat4> globals;
        for (size_t k : order) {
            if (k >= models.size() || !models[k]) continue;
            Model& m = *models[k];
            // The source (the arms) and the driver are always solved: they are what the rest is read off.
            if (sourceDone && m_Pieces[k] != m_Driver && !SkinsUpperBody(m)) continue;
            IK::Pose pose = m.AppliedLocalPose();
            if (pose.empty() || (int)pose.size() != m.NodeCount() || !reg.valid(ents[k])) continue;
            parents.resize(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);

            const glm::mat4 pieceWorld = world.ComposeWorldTransform(ents[k]);
            // Arm Steadiness filters the walk's sway, so it reads the shoulders as the body's own clips have them:
            // read after the rig's arm shapes, a reload's shoulder move was held back like sway and eased home late.
            glm::vec3 ownShoulder[2]{};
            if (!sourceDone) {
                IK::ComputeGlobals(pose, parents, globals);
                for (int s = 0; s < 2; ++s)
                    if (const int u = m.NodeIndex(Bone(kSides[s].Upper)); u >= 0)
                        ownShoulder[s] = glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[u]), 1.0f));
            }

            // The rig's arm shapes first, so the elbows bend the way the animation has them; the solve
            // then only fixes the hands.
            CopyArmShape(m, *rig, m_ArmsWeight, pose, parents, m_BoneMap, cfg.ClavicleFollow);

            const glm::mat4 toModel = glm::inverse(pieceWorld);
            // The first piece in `order` works the shoulder moves out; the rest copy them. (What of the torso's
            // shoulder that comes near the eye is left undrawn - Near Hide.)
            const bool isSource = !sourceDone;
            sourceDone = true;
            const glm::quat toModelRot = IK::Rotation(toModel);
            IK::ComputeGlobals(pose, parents, globals);
            int chest = -1; // the top spine bone the piece has
            for (int b = 4; b >= 0 && chest < 0; --b) chest = m.NodeIndex(Bone(FPBody::kBoneSpine[b]));
            if (chest >= 0 && isSource && leanMax > 0.0f) {
                // The side furthest out of reach, counting what its collarbone can still give.
                glm::vec3 from(0.0f), to(0.0f);
                float worst = 0.0f;
                for (int s = 0; s < 2; ++s) {
                    if (!haveHand[s]) continue;
                    const int upper = m.NodeIndex(Bone(kSides[s].Upper)), lower = m.NodeIndex(Bone(kSides[s].Lower)),
                              hand = m.NodeIndex(Bone(kSides[s].Hand)), clav = m.NodeIndex(Bone(kSides[s].Clavicle));
                    if (upper < 0 || lower < 0 || hand < 0) continue;
                    const glm::vec3 a = IK::Position(globals[upper]);
                    const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                    const float clavReach = clav >= 0 ? glm::length(a - IK::Position(globals[clav])) * shoulderMax : 0.0f;
                    const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
                    const float dist = glm::length(target - a);
                    const float excess = dist - armLen * 0.995f - clavReach;
                    if (excess > worst && dist > 1e-5f) { worst = excess; from = a; to = a + (target - a) / dist * excess; }
                }
                if (worst > 1e-4f) {
                    const glm::vec3 root = IK::Position(globals[chest]);
                    if (glm::dot(from - root, from - root) > 1e-8f && glm::dot(to - root, to - root) > 1e-8f) {
                        glm::quat q(glm::normalize(from - root), glm::normalize(to - root));
                        if (const float angle = angleOf(q); angle > leanMax) q = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), q, leanMax / angle);
                        q = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), q, m_ArmsWeight);
                        IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), q, root);
                        chestTurn = glm::normalize(glm::inverse(toModelRot) * q * toModelRot);
                        if (debug) BodyDebug::Info().ReachLean = glm::degrees(angleOf(q));
                    }
                }
            } else if (chest >= 0 && !isSource && angleOf(chestTurn) > 1e-5f) {
                IK::OffsetBone(pose, parents, globals, chest, glm::vec3(0.0f), glm::normalize(toModelRot * chestTurn * glm::inverse(toModelRot)),
                               IK::Position(globals[chest]));
            }
            for (int s = 0; s < 2; ++s) {
                if (!haveHand[s]) continue;
                const int upper = m.NodeIndex(Bone(kSides[s].Upper)), lower = m.NodeIndex(Bone(kSides[s].Lower)),
                          hand = m.NodeIndex(Bone(kSides[s].Hand));
                if (upper < 0 || lower < 0 || hand < 0) continue;
                const glm::vec3 target = glm::vec3(toModel * glm::vec4(handPos[s], 1.0f));
                const glm::quat rot = glm::normalize(toModelRot * handRot[s]);
                const int clav = m.NodeIndex(Bone(kSides[s].Clavicle));
                if (isSource) {
                    // Before any shoulder move: how far the hand is, of the arm's length, and how far the body's
                    // shoulder is from the rig's.
                    const glm::vec3 a = IK::Position(globals[upper]);
                    const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                    if (armLen > 1e-5f) if (debug) BodyDebug::Info().Reach[s] = glm::length(target - a) / armLen;
                    if (haveRigElbow[s]) if (debug) BodyDebug::Info().ShoulderGap[s] = glm::length(glm::vec3(pieceWorld * glm::vec4(a, 1.0f)) - rigShoulder[s]);
                }
                if (!isSource && clav >= 0 && angleOf(clavTurn[s]) > 1e-5f)
                    IK::OffsetBone(pose, parents, globals, clav, glm::vec3(0.0f), glm::normalize(toModelRot * clavTurn[s] * glm::inverse(toModelRot)),
                                   IK::Position(globals[clav]));
                const glm::quat pieceRot = glm::inverse(toModelRot);
                float turned = 0.0f; // radians the source's clavicle has turned so far this frame
                // Steady: the shoulder back to its slow average in the anchor frame, so the gait's sway doesn't
                // reach the arm. Bigger departures (a crouch, a stop clip) are followed, trailing by at most
                // Arm Steady Max.
                if (steady > 0.0f && clav >= 0 && isSource) {
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
                    const glm::quat q = turnClavicle(pose, parents, globals, clav, upper, want, shoulderMax);
                    clavTurn[s] = glm::normalize(pieceRot * q * toModelRot) * clavTurn[s];
                    turned += angleOf(q);
                    if (debug) BodyDebug::Info().ArmSteadyShift[s] = glm::length(shift);
                }
                // A hand beyond the arm's reach: the shoulder shrugs toward it instead of the arm
                // stretching (the two skeletons' shoulders can sit a little apart).
                if (clav >= 0 && isSource) {
                    const glm::vec3 a = IK::Position(globals[upper]);
                    const float armLen = glm::length(IK::Position(globals[lower]) - a) + glm::length(IK::Position(globals[hand]) - IK::Position(globals[lower]));
                    const glm::vec3 toTarget = target - a;
                    const float dist = glm::length(toTarget);
                    const float excess = std::min(dist - armLen * cfg.ShrugStart, cfg.ShrugMax);
                    if (excess > 1e-4f && dist > 1e-5f) {
                        const glm::vec3 want = a + toTarget / dist * excess * m_ArmsWeight;
                        const glm::quat q = turnClavicle(pose, parents, globals, clav, upper, want, shoulderMax - turned);
                        clavTurn[s] = glm::normalize(pieceRot * q * toModelRot) * clavTurn[s];
                        if (debug) BodyDebug::Info().ShrugShift[s] = glm::length(glm::mat3(pieceWorld) * (IK::Position(globals[upper]) - a));
                    }
                }
                IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, target, &rot, m_ArmsWeight);
                // Steady: the elbow into the rig's bend plane. The solve keeps the plane the copied arm shape
                // has, and that shape is the rig's relative to the body's chest - so the chest's sway, and the
                // pitch Spine Aim leaves out, swung the elbow about the shoulder-hand line. Measured in the
                // view's frame, the rig's plane holds still.
                if (steady > 0.0f && haveRigElbow[s]) {
                    const glm::vec3 a = IK::Position(globals[upper]), b = IK::Position(globals[lower]), c = IK::Position(globals[hand]);
                    const glm::vec3 line = c - a;
                    if (glm::dot(line, line) > 1e-8f) {
                        const glm::vec3 axis = glm::normalize(line);
                        const glm::vec3 rigBend = glm::mat3(toModel) * (rigElbow[s] - rigShoulder[s]);
                        const glm::vec3 have = (b - a) - axis * glm::dot(b - a, axis);
                        const glm::vec3 want = rigBend - axis * glm::dot(rigBend, axis);
                        // A near-straight rig arm (the melee's thrust and its recovery) has no bend plane to speak of:
                        // its elbow's few millimetres off the line swing anywhere frame to frame, while the body's arm
                        // (its shoulder nearer the hand) is already well bent. Turned onto that noise the elbow spun;
                        // left alone it looped out behind the back. So the elbow heads down and a little out - where
                        // an elbow hangs under a rifle - and onto the rig's plane as the rig's arm bends: how far its
                        // elbow stands off the line, as a share of its upper arm, none under 5%, all of it from 20%.
                        const float upperLen = std::max(glm::length(b - a), 1e-4f), rigLen = std::max(glm::length(rigBend), 1e-4f);
                        const float rigTrust = glm::smoothstep(0.05f, 0.2f, glm::length(want) / rigLen);
                        const glm::vec3 outward = s == 0 ? -kRight : kRight; // the upper arm's side (_l is the body's left)
                        glm::vec3 pole = glm::vec3(0.0f, -1.0f, 0.0f) + outward * 0.5f;
                        pole -= axis * glm::dot(pole, axis);
                        glm::vec3 aim = rigTrust > 0.0f ? glm::normalize(want) * rigTrust : glm::vec3(0.0f);
                        if (glm::dot(pole, pole) > 1e-8f) aim += glm::normalize(pole) * (1.0f - rigTrust);
                        // Where the elbow heads is eased, and turns at most so fast: the rig's clips jump (the melee's
                        // 2-frame blend in), and in a tight fold - the melee's recovery, the body's shoulder nearer the
                        // gun than the rig's - the aim swings as fast as the shoulder-to-hand line does. Followed
                        // outright, the elbow whipped round. Held in the chest's view frame, worked out once a frame
                        // (on the source piece) and given to the rest, so every piece's elbow agrees.
                        const glm::mat3 modelToAnchor = glm::mat3(anchorInverse) * glm::mat3(pieceWorld);
                        if (isSource && glm::dot(aim, aim) > 1e-8f) {
                            const glm::vec3 aimTo = glm::normalize(modelToAnchor * aim);
                            if (!haveElbowAim[s]) { elbowAim[s] = aimTo; haveElbowAim[s] = true; }
                            const float angle = std::acos(std::clamp(glm::dot(elbowAim[s], aimTo), -1.0f, 1.0f));
                            if (angle > 1e-5f) {
                                constexpr float kElbowEase = 0.06f, kElbowMaxRate = glm::radians(540.0f); // seconds; per second
                                const float step = std::min(angle * Follow(dt, kElbowEase), kElbowMaxRate * dt);
                                glm::vec3 turnAxis = glm::cross(elbowAim[s], aimTo);
                                if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::cross(elbowAim[s], glm::vec3(0.0f, 1.0f, 0.0f));
                                if (glm::dot(turnAxis, turnAxis) < 1e-10f) turnAxis = glm::vec3(1.0f, 0.0f, 0.0f);
                                elbowAim[s] = glm::normalize(glm::angleAxis(step, glm::normalize(turnAxis)) * elbowAim[s]);
                            }
                        }
                        if (haveElbowAim[s]) {
                            const glm::vec3 eased = glm::inverse(modelToAnchor) * elbowAim[s];
                            aim = eased - axis * glm::dot(eased, axis);
                        }
                        // The body's own elbow must stand off the line for the turn to mean anything (2% .. 8%).
                        const float bodyTrust = glm::smoothstep(0.02f, 0.08f, glm::length(have) / upperLen);
                        if (bodyTrust > 0.0f && glm::dot(aim, aim) > 1e-8f) {
                            const glm::vec3 h = glm::normalize(have), w = glm::normalize(aim);
                            const float swivel = std::atan2(glm::dot(glm::cross(h, w), axis), glm::dot(h, w));
                            if (std::abs(swivel) > 1e-4f) {
                                const glm::quat keep = IK::Rotation(globals[hand]);
                                IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, c, &keep, 1.0f, swivel * steady * bodyTrust);
                            }
                        }
                    }
                }
                // Elbow Clearance (the world twins only - the player's own view shows the rig's pose): the rig tucks
                // its elbows to a narrower body, and in its bend plane they went into this one's chest. Each elbow
                // swings out about its shoulder-to-hand line until the arm around it clears the drawn torso; the
                // hand stays on the gun. Worked out on the source piece, eased (in fast, out slower), given to all.
                if (!debug && cfg.ElbowClearance > 0.0f && !m_TorsoPointBuffer.empty()) {
                    if (isSource) {
                        PROFILE_SCOPE("FPB elbow swivel search");
                        auto at = [&](int node) { return glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[node]), 1.0f)); };
                        const float want = FirstPersonBodyElbowClearSwivel(m_TorsoPointBuffer, at(upper), at(lower), at(hand), cfg.ElbowClearance,
                                                                           glm::radians(90.0f), glm::radians(5.0f), m_WorldElbowClear[s]);
                        const float ease = std::abs(want) > std::abs(m_WorldElbowClear[s]) ? 0.03f : 0.15f;
                        m_WorldElbowClear[s] += (want - m_WorldElbowClear[s]) * Follow(dt, ease);
                    }
                    if (std::abs(m_WorldElbowClear[s]) > 1e-4f) {
                        const glm::quat keep = IK::Rotation(globals[hand]);
                        IK::SolveTwoBone(pose, parents, globals, upper, lower, hand, IK::Position(globals[hand]), &keep, 1.0f, m_WorldElbowClear[s]);
                    }
                }
            }
            // Debug: how far each of the body's hands ended up from the rig's (off the gun, when the arm can't reach).
            if (isSource)
                for (int s = 0; s < 2; ++s)
                    if (const int hand = m.NodeIndex(Bone(kSides[s].Hand)); haveHand[s] && hand >= 0)
                        if (debug) BodyDebug::Info().HandGap[s] = glm::length(glm::vec3(pieceWorld * glm::vec4(IK::Position(globals[hand]), 1.0f)) - handPos[s]);
            m.ApplyLocalPose(pose);
        }
    };
    {
    PROFILE_SCOPE("FPB arms: pieces");
    solveArms(m_Models, m_Pieces, m_ShoulderAnchor, m_HaveShoulderAnchor, m_ElbowAim, m_HaveElbowAim, true);
    }

    // The world gun: the first-person gun moved so its butt sits in the body's right shoulder pocket while
    // shouldered, and pushed clear of the neck and head always - the rig holds the stock in by the chin and
    // carries the gun high across the chest sprinting, which in any other view went through the hood.
    // Measured on the twins as the clips and spine have them (before their arms move).
    glm::vec3 shiftTarget(0.0f);
    bool torsoSkinned = false; // m_TorsoPointBuffer holds the twins' torso as posed now
    if (gun && !m_Twins.empty()) {
        PROFILE_SCOPE("FPB world gun clearance");
        int armsTwin = -1, driverTwin = -1;
        for (size_t k = 0; k < m_Pieces.size(); ++k) {
            if (m_Pieces[k] == m_Driver) driverTwin = (int)k;
            if (armsTwin < 0 && !armsName.empty() && reg.valid(m_Pieces[k]) && reg.all_of<NameComponent>(m_Pieces[k])) {
                std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
                for (char& c : name) c = (char)std::tolower((unsigned char)c);
                if (name.find(armsName) != std::string::npos) armsTwin = (int)k;
            }
        }
        if (armsTwin < 0) armsTwin = driverTwin;
        auto twinPoint = [&](int k, const std::string& bone, glm::vec3& out) {
            if (k < 0 || !reg.valid(m_Twins[k])) return false;
            glm::mat4 n(1.0f);
            if (!m_TwinModels[k]->NodeTransform(Bone(bone), n)) return false;
            out = glm::vec3(world.ComposeWorldTransform(m_Twins[k]) * n[3]);
            return true;
        };
        glm::vec3 ul(0.0f), ur(0.0f), neck(0.0f), head(0.0f);
        if (twinPoint(armsTwin, FPBody::kBoneUpperArm[0], ul) && twinPoint(armsTwin, FPBody::kBoneUpperArm[1], ur) &&
            twinPoint(driverTwin, "neck_01", neck) && twinPoint(driverTwin, cfg.HeadBone, head)) {
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            glm::vec3 across = ur - ul;
            across.y = 0.0f;
            const glm::vec3 right = glm::length(across) > 1e-4f ? glm::normalize(across) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 front = glm::normalize(glm::cross(up, right));
            const glm::vec3 pocket = ur + right * gun->Pocket.x + up * gun->Pocket.y + front * gun->Pocket.z;
            glm::vec3 shift = (pocket - gun->ButtWorld) * std::clamp(gun->Shouldered, 0.0f, 1.0f);
            // Keep-outs: the neck, and the head (with a hood on it) centred a little over the head bone.
            const glm::vec3 fwd = glm::length(gun->ForwardWorld) > 1e-4f ? glm::normalize(gun->ForwardWorld) : front;
            const struct { glm::vec3 Centre; float Radius; } keepOut[2] = {{neck, gun->NeckRadius}, {head + up * 0.07f, gun->HeadRadius}};
            for (int pass = 0; pass < 4; ++pass)
                for (const auto& k : keepOut) {
                    const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                    const glm::vec3 ab = b - a;
                    const float t = std::clamp(glm::dot(k.Centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                    const glm::vec3 closest = a + ab * t;
                    glm::vec3 away = closest - k.Centre;
                    const float d = glm::length(away);
                    if (d >= k.Radius) continue;
                    away = d > 1e-4f ? away / d : right; // dead on: out to the gun's side
                    shift += away * (k.Radius - d);
                }
            // The drawn head, neck and hood, whatever the outfit: the spheres are only their rough shape (a
            // sprinting rig carries the stock under the chin, through a hood the spheres miss). Out of them
            // straight away from the hood sphere's centre, as far as it takes.
            if (gun->MeshClearance > 0.0f) {
                { PROFILE_SCOPE("FPB skin head"); SkinnedPoints(world, BodyRegion::Head, m_HeadPointBuffer); }
                PROFILE_SCOPE("FPB push head");
                const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                const glm::vec3 centre = keepOut[1].Centre;
                const glm::vec3 ab = b - a;
                const float t = std::clamp(glm::dot(centre - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                glm::vec3 away = a + ab * t - centre;
                away = glm::length(away) > 1e-4f ? glm::normalize(away) : right;
                const float room = std::max(0.0f, gun->MaxShift - glm::length(shift));
                shift += away * FirstPersonBodyClearPush(m_HeadPointBuffer, a, b, away, gun->MeshClearance, room, 0.01f);
            }
            // ... and the drawn torso whenever the butt isn't in the pocket (a reload tucks it under the arm; looking
            // steeply down, a shouldered stock would lie down the chest): straight out from the chest, by that weight.
            glm::vec3 chest(0.0f);
            if (gun->MeshClearance > 0.0f && gun->TorsoKeepOut > 1e-3f && twinPoint(driverTwin, "spine_05", chest)) {
                { PROFILE_SCOPE("FPB skin torso (gun)"); SkinnedPoints(world, BodyRegion::Torso, m_TorsoPointBuffer); }
                torsoSkinned = true;
                PROFILE_SCOPE("FPB push torso");
                const glm::vec3 a = gun->ButtWorld + shift, b = a + fwd * gun->GunLength;
                const glm::vec3 ab = b - a;
                const float t = std::clamp(glm::dot(chest - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
                glm::vec3 away = a + ab * t - chest;
                away = glm::length(away) > 1e-4f ? glm::normalize(away) : front;
                const float room = std::max(0.0f, gun->MaxShift - glm::length(shift));
                shift += away * FirstPersonBodyClearPush(m_TorsoPointBuffer, a, b, away, gun->MeshClearance, room, 0.01f) *
                         std::clamp(gun->TorsoKeepOut, 0.0f, 1.0f);
            }
            if (const float len = glm::length(shift); len > gun->MaxShift && len > 1e-6f) shift *= gun->MaxShift / len;
            shiftTarget = shift * std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        }
    }
    m_WorldGunShift += (shiftTarget - m_WorldGunShift) * Follow(dt, 0.05f);
    BodyDebug::Info().WorldGunShift = glm::length(m_WorldGunShift);

    if (!m_Twins.empty()) {
        for (int s = 0; s < 2; ++s) {
            handPos[s] += m_WorldGunShift;
            rigShoulder[s] += m_WorldGunShift;
            rigElbow[s] += m_WorldGunShift;
        }
        // The drawn torso the world elbows keep out of (Elbow Clearance), as the clips and spine pose it.
        {
        PROFILE_SCOPE("FPB twins torso skin");
        // (Nothing has moved the twins since the gun's torso keep-out skinned them.)
        if (cfg.ElbowClearance > 0.0f) { if (!torsoSkinned) SkinnedPoints(world, BodyRegion::Torso, m_TorsoPointBuffer); }
        else m_TorsoPointBuffer.clear();
        }
        {
        PROFILE_SCOPE("FPB arms: twins");
        solveArms(m_TwinModels, m_Twins, m_WorldShoulderAnchor, m_WorldHaveShoulderAnchor, m_WorldElbowAim, m_WorldHaveElbowAim, false);
        }
        // The world head over the stock: the neck tilts it toward where the eye is for the world gun (the
        // camera, moved with it), by the pocket lock's weight and at most Head Tilt.
        const float lock = gun ? std::clamp(gun->CheekWeld, 0.0f, 1.0f) * std::clamp(m_ArmsWeight, 0.0f, 1.0f) : 0.0f;
        m_WorldHeadTiltDeg = 0.0f;
        m_WorldHeadTiltWeight = lock;
        int driverTwin = -1;
        for (size_t k = 0; k < m_Pieces.size(); ++k)
            if (m_Pieces[k] == m_Driver) driverTwin = (int)k;
        if (camera && lock > 1e-3f && gun->HeadTiltDegrees > 0.0f && driverTwin >= 0 && reg.valid(m_Twins[driverTwin])) {
            const Model& dm = *m_TwinModels[driverTwin];
            const int neckNode = dm.NodeIndex(Bone("neck_01")), headNode = dm.NodeIndex(Bone(cfg.HeadBone));
            if (neckNode >= 0 && headNode >= 0) {
                const glm::mat4 toModel = glm::inverse(world.ComposeWorldTransform(m_Twins[driverTwin]));
                const glm::vec3 neck = ModelPoint(dm, neckNode), headNow = ModelPoint(dm, headNode);
                const glm::vec3 eye = glm::vec3(toModel * glm::vec4(camera->Position + m_WorldGunShift, 1.0f));
                // The head sits the eye's offset behind and below the eye (the component's Camera Offset).
                const glm::vec3 headWant = eye - (kRight * cfg.CameraOffset.x + glm::vec3(0.0f, cfg.CameraOffset.y, 0.0f) + kForward * cfg.CameraOffset.z);
                const glm::vec3 from = headNow - neck, to = headWant - neck;
                if (glm::length(from) > 1e-4f && glm::length(to) > 1e-4f) {
                    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
                    glm::quat turn(glm::normalize(from), glm::normalize(to));
                    const float angle = 2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f));
                    const float most = glm::radians(gun->HeadTiltDegrees);
                    if (angle > most && angle > 1e-5f) turn = glm::slerp(none, turn, most / angle);
                    turn = glm::slerp(none, turn, lock);
                    m_WorldHeadTiltDeg = glm::degrees(2.0f * std::acos(std::clamp(std::abs(turn.w), 0.0f, 1.0f)));
                    RotateChain({"neck_01", "neck_02"}, [&](int n) { return glm::normalize(glm::slerp(none, turn, 1.0f / (float)n)); },
                                &m_TwinModels);
                }
            }
        }
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
            if (!m_Models[k] || !reg.valid(m_Pieces[k]) || !reg.all_of<NameComponent>(m_Pieces[k])) continue;
            std::string name = reg.get<NameComponent>(m_Pieces[k]).Name;
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            if (!armsName.empty() && name.find(armsName) != std::string::npos) { armsK = k; break; }
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
        if (BodyDebug::Enabled()) CameraProbe(world, *camera, viewModelFov, dt);
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
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k) {
        const entt::entity e = m_Pieces[k];
        if (!m_Models[k] || !reg.valid(e) || reg.all_of<InactiveTag>(e)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(e);
        const auto* tag = reg.try_get<PlayerBodyTag>(e);
        if (!rc || !tag || tag->CameraHidden || rc->CastShadows == RenderableComponent::ShadowCasting::ShadowsOnly) continue;
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
                // Skin mostly on a Hidden Bone is collapsed to a point (Model::SetHiddenNodes): nothing is drawn there.
                if (glm::length(glm::vec3(skin[0])) < 0.5f * total) continue;
                // Which pass draws it, and so which hides and which FOV apply (SceneRenderer).
                const bool sleeveInViewModel = tag->SleevesInViewModel && sleeve > 0.5f;
                const bool viewModel = pieceViewModel || sleeveInViewModel;
                if (!viewModel) {
                    if (inCollar || hideBones > 0.5f) continue;
                    if (tag->HasHeadBones && head > 0.5f) continue;
                    if (tag->SleevesHidden && sleeve > 0.5f) continue;
                }
                const glm::vec3 p = glm::vec3(pieceWorld * skin * glm::vec4(v.Position, 1.0f));
                const glm::vec3 d = p - eye;
                if (pieceViewModel) {
                    // The arms piece: drawn whole in the view-model pass.
                } else if (sleeveInViewModel) {
                    if (glm::dot(d, d) < tag->NearHide * tag->NearHide) continue;
                } else if (FirstPersonBodyNearHidden(p, eye, flatRight, tag->NearHide, tag->NearHideWidth)) {
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
