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
#include "RenderSettingsEpoch.h"

using namespace FirstPersonBodyDetail;

namespace {

constexpr float kMmChestTwistHalflife = 0.08f;   // s: ... eased toward the view this fast (half the way in this time)
constexpr float kMmChestTwistMax = 60.0f;       // deg: under MM, the chest's twist toward the view at most this far off the body
// MM Spine Aim (the owner, 2026-11-17: looking up or down the torso bends standing; moving, the head looks and the body runs
// as captured): the chest's share of the view's pitch, full below kMmAimFadeFrom m/s, gone by the walk/jog midpoint.
constexpr float kMmAimFadeFrom = 0.1f;
// ... and held over a stride (FirstPersonBody::LateUpdate; the limit itself: FirstPersonBodyEye.cpp).
constexpr float kLookLimitHold = 0.6f;

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

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

} // namespace

void FirstPersonBody::Fail(const std::string& message) {
    Log::Warn("First Person Body: " + message);
    m_Body = entt::null;
}

bool FirstPersonBody::Restart(World& world, Player& player) {
    FirstPersonBodyMotion carry = m_Mm; // (Stop clears m_Mm)
    Stop(world);
    return Start(world, player, &carry);
}

entt::entity FirstPersonBody::FindBody(const entt::registry& reg) {
    auto bodies = reg.view<FirstPersonBodyComponent>(entt::exclude<InactiveTag>);
    return bodies.begin() == bodies.end() ? entt::entity{entt::null} : *bodies.begin(); // the first; a scene has one player
}

std::vector<entt::entity> FirstPersonBody::RiggedPieces(const entt::registry& reg, entt::entity body) {
    // The pieces: the root itself and its children, each a rigged model on the one skeleton.
    auto rigged = [&](entt::entity e) {
        const auto* rc = reg.try_get<RenderableComponent>(e);
        return rc && rc->ModelRef && rc->ModelRef->NodeCount() > 0;
    };
    std::vector<entt::entity> pieces;
    if (body == entt::null || !reg.valid(body)) return pieces;
    if (rigged(body)) pieces.push_back(body);
    if (const auto* h = reg.try_get<HierarchyComponent>(body))
        for (entt::entity child : h->Children)
            if (reg.valid(child) && !reg.all_of<InactiveTag>(child) && rigged(child)) pieces.push_back(child);
    return pieces;
}

entt::entity FirstPersonBody::PickDriver(const entt::registry& reg, const std::vector<entt::entity>& pieces,
                                         const std::map<std::string, std::string>& boneMap, const std::string& headBone) {
    // The driver: the first piece with the whole skeleton (the pelvis, the head, a hand - the torso), whose pose motion matching
    // solves; the others follow it. (Before Phase 6: the first piece with an Animator Controller.)
    const auto bone = [&](const std::string& standard) -> const std::string& { return FPBody::MappedBone(boneMap, standard); };
    for (entt::entity e : pieces) {
        const Model& m = *reg.get<RenderableComponent>(e).ModelRef;
        if (m.NodeIndex(bone(FPBody::kBonePelvis)) >= 0 && m.NodeIndex(bone(headBone)) >= 0 && m.NodeIndex(bone(FPBody::kBoneHand[1])) >= 0)
            return e;
    }
    return entt::null;
}

const Model* FirstPersonBody::SceneDriverModel(const entt::registry& reg) {
    const entt::entity body = FindBody(reg);
    if (body == entt::null) return nullptr;
    const auto& cfg = reg.get<FirstPersonBodyComponent>(body);
    const entt::entity driver = PickDriver(reg, RiggedPieces(reg, body), FPBody::ParseBoneMap(cfg.BoneMap), cfg.HeadBone);
    return driver == entt::null ? nullptr : reg.get<RenderableComponent>(driver).ModelRef.get();
}

bool FirstPersonBody::Start(World& world, Player& player, const FirstPersonBodyMotion* carry) {
    *this = FirstPersonBody{};
    BodyDebug::Info() = BodyDebug::Snapshot{};
    auto& reg = world.Registry;
    const entt::entity body = FindBody(reg);
    if (body == entt::null) return false;
    const auto& cfg = reg.get<FirstPersonBodyComponent>(body);

    const std::vector<entt::entity> pieces = RiggedPieces(reg, body);
    if (pieces.empty()) { Fail("needs rigged body pieces (the object's own model, or child objects with models)."); return false; }
    m_BoneMap = FPBody::ParseBoneMap(cfg.BoneMap);
    m_Driver = PickDriver(reg, pieces, m_BoneMap, cfg.HeadBone);
    if (m_Driver == entt::null) {
        Fail("needs a piece with the whole skeleton (the pelvis, the head and the hands: the torso).");
        return false;
    }
    m_Body = body;
    if (const auto* outfit = reg.try_get<CharacterOutfitComponent>(body)) {
        m_OutfitVersion = outfit->Version;
        m_OutfitLookVersion = outfit->LookVersion;
    }

    Model& model = *reg.get<RenderableComponent>(m_Driver).ModelRef;
    m_HeadBone = Bone(cfg.HeadBone); // through the Bone Map, as the driver was picked by it
    m_HeadNode = model.NodeIndex(m_HeadBone);
    if (m_HeadNode < 0)
        Log::Warn("First Person Body: the body has no bone '" + m_HeadBone + "' - the camera stays at the eye height.");
    else
        m_RestHead = glm::vec3(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode)[3]);
    m_HaveBindPosture = false;
    m_EyeClearBuilt = false;
    m_EyeClearPoints.clear();
    m_EyeClear = 0.0f;
    if (const int pelvisNode = model.NodeIndex(Bone(FPBody::kBonePelvis)); pelvisNode >= 0) {
        const glm::mat4 bp = model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, pelvisNode);
        m_BindPelvis = IK::Rotation(bp);
    }
    if (const int chest = model.NodeIndex(Bone(FPBody::kBoneSpine[4])); chest >= 0 && m_HeadNode >= 0) {
        m_BindChest = IK::Rotation(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, chest));
        m_BindHead = IK::Rotation(model.SampleNodeModelSpace(-1, 0.0f, AnimationWrapMode::ClampForever, m_HeadNode));
        m_HaveBindPosture = true;
    }
    m_HaveMetricYaw = false;

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
        const bool isArms = IsArmsPiece(reg, e);
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
    // Player Run / Sprint Speed (when set) move the capsule faster than the clips travel; the clips
    // then play faster to keep pace (Tick's PlayRate).
    m_RunSpeed = std::max(cfg.RunSpeed, 0.01f);
    m_PlayerRunSpeed = cfg.PlayerRunSpeed > 0.0f ? cfg.PlayerRunSpeed : m_RunSpeed;
    // Walk is the default gait and Shift the jog. The walk keeps the jog's ratio of player to clip speed. (Motion matching
    // sets the gait speeds below.)
    m_PlayerWalkSpeed = std::clamp(cfg.WalkSpeed, 0.01f, m_RunSpeed) * m_PlayerRunSpeed / m_RunSpeed;
    m_CrouchHeight = cfg.CrouchHeight;
    m_CrouchSpeed = cfg.CrouchSpeed;
    const float playerMoveSpeed = player.MoveSpeed, playerSprint = player.SprintMultiplier; // (restored if MM can't start)
    player.MoveSpeed = m_PlayerWalkSpeed;
    player.SprintMultiplier = m_PlayerRunSpeed / m_PlayerWalkSpeed;
    player.GaitSpeeds.Active = false;

    // Motion matching: the pose from the database, the capsule at the mocap's gait speeds as captured -
    // walk by default, the jog with Shift, the pack's crouch-walk crouched; strafes (and backwards) at the pack's strafe
    // speeds. (The pack has no sustained sprint: an asset gap, docs/MOTION_MATCHING_HANDOFF.md.)
    {
        size_t driverIndex = 0;
        for (size_t k = 0; k < m_Pieces.size(); ++k)
            if (m_Pieces[k] == m_Driver) driverIndex = k;
        std::string error;
        m_Mm.SetDebug(MmPlayerDebug::Wanted() ? &MmPlayerDebug::Info() : nullptr); // the player's (--npc-test reads it)
        if (!m_Mm.Start(m_Models, driverIndex, error, carry)) {
            // (CI has no licensed assets: no body there - a warning, never an error.) Everything set up above is undone - the
            // pieces' tags and hidden bones, the world twins (which would stand frozen in the bind pose), the player's speeds -
            // so the scene plays as if there were no body.
            Fail(error + " - the body is off.");
            Stop(world);
            player.MoveSpeed = playerMoveSpeed;
            player.SprintMultiplier = playerSprint;
            return false;
        } else {
            const GaitSpeedTable& g = m_Mm.Gaits();
            m_MmSpeeds.Active = true;
            m_MmSpeeds.Forward[0] = g.Walk;
            m_MmSpeeds.Forward[1] = g.Jog;
            m_MmSpeeds.Forward[2] = g.CrouchWalk;
            // Sideways and backwards apart (the walk's sideways takes are slower than its walk backwards).
            const int slotStyle[4] = {MotionLabel::StyleWalk, MotionLabel::StyleJog, MotionLabel::StyleCrouch, MotionLabel::StyleStroll};
            for (int k = 0; k < 3; ++k) {
                m_MmSpeeds.Strafe[k] = g.StyleStrafe(slotStyle[k]);
                m_MmSpeeds.Back[k] = g.StyleBack(slotStyle[k]);
            }
            // The stroll: the Walk action, or a partial stick (Player::StrollStick).
            m_MmSpeeds.Forward[3] = g.Legs[4] > 0 ? g.Stroll : 0.0f;
            m_MmSpeeds.Strafe[3] = g.Legs[4] > 0 ? g.StyleStrafe(MotionLabel::StyleStroll) : 0.0f;
            m_MmSpeeds.Back[3] = g.Legs[4] > 0 ? g.StyleBack(MotionLabel::StyleStroll) : 0.0f;
            // The sprint: slot 4 (a press of Sprint while moving, held - Player::SprintHeld), at the sprint style's measured speeds
            // (the old locomotion's jog loop: docs/MOTION_MATCHING_HANDOFF.md, "Asset gaps").
            m_MmSpeeds.Forward[4] = g.Legs[5] > 0 ? g.Sprint : 0.0f;
            m_MmSpeeds.Strafe[4] = g.Legs[5] > 0 ? g.StyleStrafe(MotionLabel::StyleSprint) : 0.0f;
            m_MmSpeeds.Back[4] = g.Legs[5] > 0 ? g.StyleBack(MotionLabel::StyleSprint) : 0.0f;
            player.GaitSpeeds = m_MmSpeeds;
            char b[400];
            std::snprintf(b, sizeof b,
                          "stroll %.2f / walk %.2f / jog %.2f / crouch %.2f m/s, sideways %.2f / %.2f / %.2f / %.2f, backwards %.2f / %.2f / "
                          "%.2f / %.2f, turn in place %.0f deg/s",
                          m_MmSpeeds.Forward[3], g.Walk, m_MmSpeeds.Forward[1], m_MmSpeeds.Forward[2], m_MmSpeeds.Strafe[3],
                          m_MmSpeeds.Strafe[0], m_MmSpeeds.Strafe[1], m_MmSpeeds.Strafe[2], m_MmSpeeds.Back[3], m_MmSpeeds.Back[0],
                          m_MmSpeeds.Back[1], m_MmSpeeds.Back[2], glm::degrees(m_Mm.TurnInPlaceRate()));
            Log::Info(std::string("First Person Body: motion matching - ") + b + ".");
        }
    }

    Log::Info("First Person Body: '" + (reg.all_of<NameComponent>(body) ? reg.get<NameComponent>(body).Name : std::string("body")) +
              "' is the player's body - " + std::to_string(pieces.size()) + " pieces, " + std::to_string(shadowOnly) +
              " hidden from the camera.");
    return true;
}

bool FirstPersonBody::OutfitChanged(const World& world) const {
    if (m_Body == entt::null || !world.Registry.valid(m_Body)) return false;
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(m_Body);
    return outfit && outfit->Version != m_OutfitVersion;
}

bool FirstPersonBody::OutfitLookChanged(const World& world) const {
    if (m_Body == entt::null || !world.Registry.valid(m_Body)) return false;
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(m_Body);
    return outfit && outfit->LookVersion != m_OutfitLookVersion;
}

void FirstPersonBody::SyncLook(World& world) {
    if (m_Body == entt::null || !world.Registry.valid(m_Body)) return;
    if (const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(m_Body)) m_OutfitLookVersion = outfit->LookVersion;
    for (size_t k = 0; k < m_Twins.size() && k < m_Pieces.size(); ++k) CopyLook(world, k);
}

void FirstPersonBody::Stop(World& world) {
    m_ArmShapeCache.clear(); // keyed by model address: the next Start's models may reuse this run's
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
    m_Mm.Stop();
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
        dst.CastShadows = src.CastShadows;
        dst.ReceiveShadows = src.ReceiveShadows;
        std::vector<int> nodes;
        for (const std::string& b : hiddenBones)
            if (const int n = inst->NodeIndex(b); n >= 0) nodes.push_back(n);
        inst->SetHiddenNodes(nodes);
        reg.emplace_or_replace<HiddenFromOwnerTag>(t);
        reg.emplace_or_replace<OwnerViewOnlyTag>(e);
        m_Twins.push_back(t);
        m_TwinModels.push_back(std::move(inst));
    }
    SyncTwins(world); // (the look too: CopyLook)
}

void FirstPersonBody::CopyLook(World& world, size_t k) {
    auto& reg = world.Registry;
    const entt::entity piece = m_Pieces[k], twin = m_Twins[k];
    if (!reg.valid(twin) || !reg.valid(piece)) return;
    const auto& src = reg.get<RenderableComponent>(piece);
    auto& dst = reg.get<RenderableComponent>(twin);
    if (dst.Materials != src.Materials) { dst.Materials = src.Materials; NoteRenderSettingsChanged(); }
    // Copied by value: adding a component can move its storage (and any reference into it).
    const auto* hide = reg.try_get<OutfitHideTag>(piece);
    const auto* twinHide = reg.try_get<OutfitHideTag>(twin);
    if (!hide && twinHide) reg.remove<OutfitHideTag>(twin);
    else if (hide && (!twinHide || twinHide->Buffer != hide->Buffer)) {
        const OutfitHideTag copy = *hide;
        reg.emplace_or_replace<OutfitHideTag>(twin, copy);
    }
    const auto* pull = reg.try_get<OutfitLayerTag>(piece);
    const auto* twinPull = reg.try_get<OutfitLayerTag>(twin);
    if (!pull && twinPull) reg.remove<OutfitLayerTag>(twin);
    else if (pull && (!twinPull || twinPull->Pull != pull->Pull)) {
        const float p = pull->Pull;
        reg.emplace_or_replace<OutfitLayerTag>(twin).Pull = p;
    }
    const auto* layer = reg.try_get<LayerComponent>(piece);
    const auto* twinLayer = reg.try_get<LayerComponent>(twin);
    if (!layer && twinLayer) reg.remove<LayerComponent>(twin);
    else if (layer && (!twinLayer || twinLayer->Layer != layer->Layer)) {
        const int l = layer->Layer;
        reg.emplace_or_replace<LayerComponent>(twin).Layer = l;
    }
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
        // The piece's pose, as drawn (aim and layers included): one body, seen from inside and out (only the arms' targets differ).
        if (!pose.empty()) m_TwinModels[k]->ApplyLocalPose(pose);
        // The piece's look can change without either outfit version: the hiding lands when its coverage has been
        // worked out (often after Start).
        CopyLook(world, k);
    }
}

void FirstPersonBody::PlaceHeadAttachedTwins(World& world) {
    // As OutfitSystem::UpdateAttachments places the pieces (the head piece's head bone skinning matrix, from
    // its own place), but off the head twin: rigid head wear then follows the world head's tilt, this frame.
    auto& reg = world.Registry;
    int headTwin = -1;
    constexpr int kHead = OutfitPieceBodyPart | OutfitPieceHeadAttached; // the race's head: the one head-attached body part
    for (size_t k = 0; k < m_Pieces.size() && k < m_Twins.size(); ++k)
        if (reg.valid(m_Pieces[k]) && reg.all_of<OutfitPieceComponent>(m_Pieces[k]) &&
            (reg.get<OutfitPieceComponent>(m_Pieces[k]).Flags & kHead) == kHead)
            headTwin = (int)k;
    if (headTwin < 0 || !reg.valid(m_Twins[headTwin]) || !m_TwinModels[headTwin]) return;
    const int bone = m_TwinModels[headTwin]->BoneId(m_HeadBone);
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
        const float fit = reg.get<OutfitPieceComponent>(m_Pieces[k]).Fit;
        const glm::mat4 fitted = follow * OutfitSystem::FitMatrix(*m_TwinModels[k], fit);
        world.SetWorldPose(m_Twins[k], glm::vec3(fitted[3]), rot);
        reg.get<TransformComponent>(m_Twins[k]).Scale = scale * (fit > 0.0f ? fit : 1.0f);
    }
}

void FirstPersonBody::BeforePlayerMove(Player& player, Camera& camera) {
    camera.Position -= m_CameraApplied;
    m_CameraApplied = glm::vec3(0.0f);
    player.MaxYawRate = 0.0f;
    player.MinPitch = IsActive() && m_Mm.Active() ? m_LookLimit : -89.0f;
    player.CrouchHeight = IsActive() ? m_CrouchHeight : 0.0f;
    // Crouch Speed is a fraction of the run (jog); the capsule's base is the walk.
    player.CrouchSpeedMultiplier = m_CrouchSpeed * (m_PlayerWalkSpeed > 0.0f ? m_PlayerRunSpeed / m_PlayerWalkSpeed : 1.0f);
    player.GaitSpeeds.Active = false;
    if (!IsActive()) return;
    {
        // Capsule-led: the input moves the capsule (at the gait table's speeds), never the clips.
        player.RootMotionVelocity = glm::vec3(0.0f);
        player.RootMotionWeight = 0.0f;
        player.GaitSpeeds = m_MmSpeeds;
        if (m_Still && m_TurnThreshold > 0.0f) {
            // Turning on the spot: the view turns no faster than the database's turn-in-place clips do (and Max Turn Rate,
            // when set) beyond the threshold, as with the controller's turn clips.
            float rate = glm::degrees(m_Mm.TurnInPlaceRate());
            if (m_MaxTurnRate > 0.0f) rate = rate > 0.0f ? std::min(rate, m_MaxTurnRate) : m_MaxTurnRate;
            player.MaxYawRate = rate;
            player.YawFreeCenter = 90.0f - glm::degrees(m_Yaw);
            player.YawFreeRange = m_TurnThreshold;
        }
    }
}

void FirstPersonBody::Tick(World& world, const Player& player, const Camera& camera, float dt) {
    if (!IsActive()) return;
    // The player's own eye, before the presentation's view punch goes on (FirstPersonPresentation::Update): whatever
    // is on the camera past this by LateUpdate is that punch (LateUpdate keeps it).
    m_CameraBase = camera.Position;
    m_HaveCameraBase = true;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    for (entt::entity e : m_Pieces)
        if (auto* tag = reg.valid(e) ? reg.try_get<PlayerBodyTag>(e) : nullptr) {
            // Live, for the Inspector - clothing keeping its own (Start's), not reset to the body's.
            tag->NearHide = FirstPersonBodyPieceNearHide(cfg.NearHide, cfg.ClothingNearHide, tag->Clothing);
            tag->NearHideWidth = tag->Clothing ? std::max(cfg.ClothingNearHideWidth, 0.0f) : 0.0f;
        }
    BodyDebug::Clear();

    // Stand at the capsule's feet, facing the view.
    if (PhysicsWorld::HasCharacter()) {
        float f[3];
        PhysicsWorld::GetCharacterFootPosition(f);
        // The body's height is its own, in the world: on stairs the capsule climbs each step in a few frames (Jolt's
        // stair walk), a staircase the camera on the head felt as a kink every step (~180 m/s^2 on the Sandbox stairs).
        // The body follows the ground's ramp instead: the ground sampled along the travel over a window two treads
        // long and averaged - on stairs their slope; standing still the window closes to the ground under the capsule -
        // plus the hips' drop to the lower foot, on one critically damped spring fed the ramp's climb, so it follows a
        // flight without lag. The legs' IK reaches each step meanwhile.
        const float rise = m_HaveCapsule ? f[1] - m_LastCapsuleY : 0.0f;
        const float capsuleRate = dt > 0.0f ? rise / dt : 0.0f;
        if (!m_HaveBodyY || std::abs(m_BodyY - f[1]) > 1.0f) { m_BodyY = f[1]; m_BodyVy = 0.0f; m_HaveBodyY = true; } // a teleport
        float target = f[1], rate = capsuleRate;
        const bool onFeet = cfg.FootIK && player.Grounded;
        if (onFeet && dt > 0.0f) {
            constexpr int kRays = 16;
            constexpr float kWindow = 0.8f; // metres at a run; it closes below 1 m/s
            const float speed = glm::length(m_GroundVelocity);
            const float window = kWindow * std::min(speed, 1.0f);
            float ramp = 0.0f, slope = 0.0f, uneven = 0.0f;
            if (window > 0.02f) {
                const glm::vec3 along = m_GroundVelocity / speed;
                float heights[kRays];
                QueryFilter filter;
                filter.HitTriggers = 0;
                for (int i = 0; i < kRays; ++i) {
                    const float s = window * ((i + 0.5f) / kRays - 0.5f);
                    const float origin[3] = {f[0] + along.x * s, f[1] + 0.5f, f[2] + along.z * s};
                    const float down[3] = {0.0f, -1.0f, 0.0f};
                    RaycastHit hit;
                    // Off an edge, deeper than two stairs (a gap the capsule bridges), or a wall's top: the capsule's own
                    // ground, so a drop never pulls the body down into it.
                    const bool ground = PhysicsWorld::RaycastSolid(origin, down, 1.0f, filter, hit) && hit.Hit && hit.Normal[1] > 0.5f &&
                                        hit.Point[1] - f[1] > -0.42f;
                    heights[i] = ground ? std::min(hit.Point[1] - f[1], 0.45f) : 0.0f;
                    uneven = std::max(uneven, ground ? std::abs(heights[i]) : 1.0f); // no ground there: not level
                }
                FirstPersonBodyGroundRamp(heights, kRays, window * (kRays - 1) / kRays, ramp, slope);
            }
            float hips = 0.0f;
            if (m_HaveFootGround)
                hips = FirstPersonBodyFootPelvis(m_FootGround[0] - ramp, m_FootGround[1] - ramp, std::min(cfg.FootIKMaxDrop, 0.3f), cfg.PelvisMaxRaise);
            // The body's height off the capsule's eases off past 0.35 m toward 0.4 m (walking a ledge's edge, the hips
            // dropped to a foot over the drop), and with it the ramp's climb fed to the spring: a hard limit, the body
            // stopped against it with a jolt.
            float off = ramp + hips * m_FootWeight, free = 1.0f;
            if (const float over = std::abs(off) - 0.35f; over > 0.0f) {
                const float t = std::tanh(over / 0.05f);
                off = std::copysign(0.35f + 0.05f * t, off);
                free = 1.0f - t * t;
            }
            target = f[1] + off;
            // The ramp climbs at its slope times the speed. Level under the window, the ground's own motion (a lift)
            // is the capsule's: followed at its rate - as is the capsule's, held at the limit.
            // (A lift's pace only: faster, the capsule is popping up a step or off an edge.)
            const float level = std::clamp(1.0f - uneven / 0.03f, 0.0f, 1.0f);
            const float ground = std::clamp(capsuleRate, -1.5f, 1.5f);
            rate = slope * speed + (ground - slope * speed) * level;
            rate = ground + (rate - ground) * free;
        }
        FirstPersonBodyStepSpring(m_BodyY, m_BodyVy, target, rate, onFeet ? cfg.StairEase : 0.03f, dt);
        m_BodyY = std::clamp(m_BodyY, f[1] - 0.5f, f[1] + 0.5f); // a safety: the target stays within 0.4 m
        m_StepOffset = m_BodyY - f[1];
        m_LastCapsuleY = f[1];
        m_GroundVelocity = glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z);
        m_HaveCapsule = true;
        m_Feet = glm::vec3(f[0], f[1] + m_StepOffset, f[2]);
    } else {
        m_Feet = camera.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    }
    m_ViewYaw = FirstPersonBodyYaw(camera.Front(), m_ViewYaw);
    if (!m_HaveHeading) { m_Yaw = m_ViewYaw; m_HaveHeading = true; }
    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    TickMotionMatching(world, player, cfg, dt);
}

// The gait's phase, read off the feet (their spread along the travel against their height difference) - the clips'
// feet as LateUpdate had them before foot IK: after it, stairs and slopes lift one foot over the other and skew the
// phase (the bob and the weapon's sway dipped off the footfalls on stairs).
void FirstPersonBody::UpdateGaitPhase(const Player& player, float dt) {
    const glm::vec2 move = FirstPersonBodyLocalMove(glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z), m_Yaw);
    const float speed = glm::length(move);
    if (player.Grounded && speed > 0.3f && m_HaveGaitFeet) {
        const glm::vec2 dir = move / speed;
        const glm::vec3 d = m_GaitFeet[0] - m_GaitFeet[1]; // driver model space (the body's frame: kRight, up, kForward)
        m_Gait.Update(glm::dot(d, kRight) * dir.x + glm::dot(d, kForward) * dir.y, d.y, true, dt);
    } else {
        m_Gait.Update(0.0f, 0.0f, false, dt);
    }
}

// Motion matching (FirstPersonBodyMotion): the pose is the matcher's (any Animator Controller left on a piece is held); the
// matcher runs on the capsule's state and turns the heading; the body stands at the capsule's feet (the root clamped there).
void FirstPersonBody::TickMotionMatching(World& world, const Player& player, const FirstPersonBodyComponent& cfg, float dt) {
    auto& reg = world.Registry;
    // (A piece that still carries an Animator Controller - a scene from before Phase 6 - is held: the matcher poses every piece.)
    for (entt::entity e : m_Pieces)
        if (auto* ac = reg.valid(e) ? reg.try_get<AnimatorControllerComponent>(e) : nullptr) { ac->SkipUpdate = true; ac->SkippedTime = 0.0f; }

    FirstPersonBodyMotion::Input in;
    in.Dt = dt;
    in.Feet = m_Feet - glm::vec3(0.0f, m_StepOffset, 0.0f); // the capsule's own feet
    in.Velocity = player.Velocity;
    in.Wish = player.WishVelocity;
    in.ViewYaw = m_ViewYaw;
    in.Grounded = player.Grounded;
    in.Crouched = player.Crouched;
    in.Sprint = player.Sprinting;
    // The style asked for: the gait slot the capsule moves in (Player::DirectionalSpeeds).
    in.Style = player.Gait == 1   ? MotionLabel::StyleJog
               : player.Gait == 3 ? MotionLabel::StyleStroll
               : player.Gait == 4 ? MotionLabel::StyleSprint
                                  : MotionLabel::StyleWalk;
    in.Jumped = player.Jumped;
    in.JumpSpeed = player.JumpSpeed;
    in.Gravity = player.Gravity;
    in.TurnThreshold = glm::radians(std::max(cfg.TurnThreshold, 0.0f));
    in.TurnEndAngle = glm::radians(std::max(cfg.TurnEndAngle, 0.0f));
    // The predicted trajectory moves as the capsule does: PlayerApproachVelocity's ease (time constant GroundAccelTime
    // speeding up, GroundDecelTime slowing down; in the air AirAccelTime both ways) and its arrive, then the move by the new
    // velocity each frame. (A spring with the same halflife ends the same, but on the way a stop ran up to 5 cm ahead of
    // the capsule's at 0.1 s, just where the stop's frames are chosen.)
    in.CapsuleLaw = true;
    in.AccelTime = player.Grounded ? player.GroundAccelTime : player.AirAccelTime;
    in.DecelTime = player.Grounded ? player.GroundDecelTime : player.AirAccelTime;
    in.SnapToGoal = kPlayerArriveSnap; // PlayerApproachVelocity's arrive
    const float radius = std::max(0.05f, player.Size.x * 0.5f);
    in.Sweep = [radius](const glm::vec3& from, const glm::vec3& to, glm::vec3& stopAt, glm::vec3& normal) {
        glm::vec3 d = to - from;
        d.y = 0.0f;
        const float len = glm::length(d);
        if (len < 1e-6f) return false;
        d /= len;
        // A wall, not a stair: a sphere just over the capsule's step and another at head height both stop within half a
        // metre of each other. (One sphere at knee height read every riser of the steep stairs as a wall, and one over the
        // step height read the flight's later risers a second ahead: the predicted path stopped, and the matcher played
        // idle or slow frames on the climb.) Low obstacles - a 1 m ledge - aren't predicted; the capsule still stops there.
        const glm::vec3 o = from + glm::vec3(0.0f, 0.4f + radius, 0.0f), head = from + glm::vec3(0.0f, 1.5f, 0.0f);
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit, high;
        if (!PhysicsWorld::SphereCastSolid(&o.x, &d.x, radius, len, f, hit) || !hit.Hit) return false;
        if (std::fabs(hit.Normal[1]) > 0.7f) return false; // a floor or ramp, not a wall
        if (!PhysicsWorld::SphereCastSolid(&head.x, &d.x, radius, len, f, high) || !high.Hit || high.Distance > hit.Distance + 0.5f) return false;
        stopAt = from + d * std::max(0.0f, hit.Distance - 0.01f);
        normal = glm::vec3(hit.Normal[0], 0.0f, hit.Normal[2]);
        return true;
    };
    m_Mm.Tick(in, m_Yaw);

    m_Twist = FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw);
    m_Still = m_Mm.Still();
    m_Turning = m_Mm.Turning();
    m_TurnThreshold = cfg.TurnThreshold;
    m_MaxTurnRate = cfg.MaxTurnRate;
    m_Grounded = player.Grounded;
    m_Move = FirstPersonBodyLocalMove(glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z), m_Yaw);
    m_IdleTime = m_Still ? m_IdleTime + dt : 0.0f;
    m_MoveTime = m_Still ? 0.0f : m_MoveTime + dt;
    world.SetWorldPose(m_Body, m_Feet, YawRotation(m_Yaw));
    UpdateGaitPhase(player, dt);

    BodyDebug::Snapshot& d = BodyDebug::Info();
    d.Valid = true;
    d.AnimatorState = FPBody::kStateMotionMatching;
    d.StateTime = 0.0f;
    d.Turning = m_Turning;
    d.BodyYawDeg = glm::degrees(m_Yaw);
    d.ViewOffsetDeg = glm::degrees(FirstPersonBodyWrapAngle(m_ViewYaw - m_Yaw));
    d.TwistDeg = glm::degrees(m_Twist);
    d.Still = m_Still;
    d.IdleTime = m_IdleTime;
    d.MoveTime = m_MoveTime;
    d.MoveSpeed = glm::length(m_Move);
    // The clips' travel as shown: the matched frame's own root speed at the play rate it is warped to.
    d.RootSpeed = m_Mm.Airborne() ? 0.0f : m_Mm.AnimSpeed() * m_Mm.PlayRate();
    d.StepOffset = m_StepOffset;
    d.ArmsWeight = m_ArmsWeight;
    d.LastTrigger = m_Mm.Airborne() ? "Airborne (MM)" : std::string();
    d.LastTriggerAgo = 0.0f;
    if (BodyDebug::Enabled()) {
        const glm::vec3 base = m_Feet + glm::vec3(0.0f, 0.03f, 0.0f);
        const auto heading = [&](float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); };
        BodyDebug::Arrow(base, base + heading(m_Yaw) * 0.9f, glm::vec4(1.0f, 0.85f, 0.2f, 1.0f));
        BodyDebug::Arrow(base, base + heading(m_ViewYaw) * 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    }
}

void FirstPersonBody::LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms,
                                 const std::string& rigCameraBone) {
    if (!IsActive()) return;
    auto& reg = world.Registry;
    if (!reg.valid(m_Body)) { m_Body = entt::null; return; }
    const auto& cfg = reg.get<FirstPersonBodyComponent>(m_Body);
    m_Spine = cfg.Spine;

    if (!reg.valid(m_Driver)) { m_Body = entt::null; return; }
    // Motion matching: this step's pose onto every piece (the controller's update was held), before anything reads it.
    if (m_Mm.Active()) {
        m_Mm.Pose(m_Models);
        CaptureMmClip();
    }
    // The presentation's view punch on the camera this frame (Update / RefreshViewKick put it on after Tick): the walk
    // bob, landing dip, lean and ADS camera share. It stays on - the camera goes to the body's eye plus this, below.
    const glm::vec3 viewKick = m_HaveCameraBase ? camera.Position - m_CameraBase : glm::vec3(0.0f);
    m_HaveCameraBase = false; // this frame's Tick's only

    // This frame's Weapon Arms weight, before anything here reads it - the eye most of all. (Set only in ArmsLateUpdate,
    // a draw's first frame put the eye where the unarmed one is while the arms took the rig's hands: a pop.) The rig as
    // ArmsLateUpdate will find it: hidden by the arms animator's Hidden tag as of this frame's update, which
    // FirstPersonPresentation::LateUpdate (after this) turns into its InactiveTag.
    {
        bool follow = cfg.WeaponArms && m_ViewModelFovOk && weaponArms != entt::null && reg.valid(weaponArms) &&
                      reg.all_of<RenderableComponent>(weaponArms);
        if (follow) {
            if (reg.all_of<InactiveTag>(weaponArms) && !reg.all_of<DeactivatedTag>(weaponArms)) follow = false; // off for another reason
            else if (const auto* armsAc = reg.try_get<AnimatorControllerComponent>(weaponArms))
                follow = !armsAc->HasTag(FirstPersonAnimatorContract::kTagHidden);
            else follow = !reg.all_of<InactiveTag>(weaponArms);
        }
        m_ArmsWeightPrev = m_ArmsWeight;
        if (follow) m_ArmsWeight = 1.0f;
        else m_ArmsWeight -= m_ArmsWeight * Follow(dt, cfg.ArmsEaseOut);
        m_ArmsWeightEased = true;
    }

    BodyDebug::Info().ChestPitchClipDeg = ChestPitchDeg();
    TrunkAngles(BodyDebug::Info().TrunkClipDeg);
    if (const Model* drv = DriverModel(); drv)
        if (glm::mat4 pv(1.0f); drv->NodeTransform(Bone(FPBody::kBonePelvis), pv)) BodyDebug::Info().PelvisHeightClip = pv[3].y;
    // The A/B switch (FirstPersonBodyComponent::MmPostLayers): off, motion matching's pose is drawn with foot IK only.
    const bool mmLayers = cfg.MmPostLayers;
    BodyDebug::Info().ChestPitchStableDeg = ChestPitchDeg();
    // The clips' feet, before foot IK moves them: what next Tick reads the gait's phase off.
    m_HaveGaitFeet = FootPoints(world, m_GaitFeet);
    {
    PROFILE_SCOPE("FPB foot IK");
    ApplyFootIK(world, cfg, dt); // the pelvis and legs first: everything after reads the final pose
    }
    if (IK::FootSlideMeterOn()) { // the feet as drawn (a visible piece that has them), for the --npc-test slide meter
        const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
        for (const auto& mp : m_Models) {
            glm::mat4 n0(1.0f), n1(1.0f), b0(1.0f), b1(1.0f);
            if (!mp || !mp->NodeTransform(Bone(FPBody::kBoneFoot[0]), n0) || !mp->NodeTransform(Bone(FPBody::kBoneFoot[1]), n1)) continue;
            const glm::quat yaw = YawRotation(m_Yaw);
            const glm::vec3 feet[2] = {m_Feet + yaw * (scale * glm::vec3(n0[3])), m_Feet + yaw * (scale * glm::vec3(n1[3]))};
            // The balls too: a foot pivoting on its ball (or heel) is turning, not sliding (FootSlideMeterTotals::MaxContactDrift).
            const bool balls = mp->NodeTransform(Bone(FPBody::kBoneBall[0]), b0) && mp->NodeTransform(Bone(FPBody::kBoneBall[1]), b1);
            const glm::vec3 ball[2] = {m_Feet + yaw * (scale * glm::vec3(b0[3])), m_Feet + yaw * (scale * glm::vec3(b1[3]))};
            IK::FootSlideMeterFor("player").Step(feet, m_Feet.y, dt, balls ? &ball : nullptr);
            BodyDebug::Info().DrawnFoot[0] = feet[0];
            BodyDebug::Info().DrawnFoot[1] = feet[1];
            break;
        }
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
        for (size_t k = 0; k < m_Pieces.size(); ++k) {
            if (m_Pieces[k] == m_Body || !IsArmsPiece(reg, m_Pieces[k])) continue;
            const Model& m = *m_Models[k];
            // Scratch kept between calls: this runs five or six times a frame.
            thread_local IK::Pose pose;
            thread_local std::vector<int> parents;
            thread_local std::vector<glm::mat4> globals;
            pose = m.AppliedLocalPose();
            const int ul = m.NodeIndex(Bone(FPBody::kBoneUpperArm[0])), ur = m.NodeIndex(Bone(FPBody::kBoneUpperArm[1]));
            if (pose.empty() || (int)pose.size() != m.NodeCount() || ul < 0 || ur < 0) break;
            parents.resize(pose.size());
            for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m.NodeParent(i);
            CopyArmShapeCached(m, *armRig, m_ArmsWeight, pose, parents, cfg.ClavicleFollow);
            // Only the upper arms are read: the root-to-each path, not every node (the same products in the same order,
            // so the same bits; the rest of `globals` is stale scratch, never read).
            globals.resize(pose.size());
            IK::RefreshPath(pose, parents, globals, -1, ul);
            IK::RefreshPath(pose, parents, globals, -1, ur);
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
    // Unarmed, the chest's pitch at the view is capped (Spine Aim Max Up / Down, eased in): past it the head looks on alone.
    const float aimUnarmed = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    float spineAim = FirstPersonBodyCappedSpineAim(viewPitch, FirstPersonBodySpineAim(viewPitch, cfg.SpineAim, cfg.SpineAimDown),
                                                   glm::radians(viewPitch >= 0.0f ? cfg.SpineAimMaxUp : cfg.SpineAimMaxDown), aimUnarmed);
    // Motion matching, unarmed: the aim fades out with speed (FirstPersonBodyMmAimFade) - moving, the head looks and the body
    // moves as captured. (Armed, the chest still carries the arms rig's aim.)
    if (m_Mm.Active()) {
        const GaitSpeedTable& g = m_Mm.Gaits();
        const float fade = FirstPersonBodyMmAimFade(glm::length(m_Move), kMmAimFadeFrom, 0.5f * (g.Walk + g.Jog));
        spineAim *= 1.0f - aimUnarmed * (1.0f - fade);
        BodyDebug::Info().MmAimFade = fade;
    }
    // The torso's forward bend (BodyDebug): pelvis to the top spine bone off the vertical, in the driver's model space.
    const auto torsoBend = [&]() {
        glm::mat4 p(1.0f), c(1.0f);
        if (!model || !model->NodeTransform(Bone(FPBody::kBonePelvis), p) || !model->NodeTransform(Bone(FPBody::kBoneSpine[4]), c)) return 0.0f;
        const glm::vec3 d = glm::vec3(c[3]) - glm::vec3(p[3]);
        return glm::degrees(std::atan2(glm::dot(d, kForward), d.y));
    };
    BodyDebug::Info().ClipBendDeg = torsoBend();
    BodyDebug::Info().ViewPitchDeg = glm::degrees(viewPitch);
    // The pitch only: the chest's heading (Spine Twist, or the rig's stance) is closed-loop below, measured off the
    // shoulders - the clips' own spine yaw is in it, so an open-loop twist here was only ever a first guess.
    if (mmLayers && spineAim > 0.0f) {
        // Arm Hang, unarmed (armed, the arms take the rig's pose and reach the gun: ArmsLateUpdate).
        const float hang = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        thread_local std::vector<ArmHangPose> arms;
        if (hang > 1e-3f) CaptureArms(arms);
        ApplySpineAim(camera, spineAim, 0.0f);
        BodyDebug::Info().ArmDownAimDeg = ArmDownDeg();
        if (hang > 1e-3f) HangArms(arms, hang);
    } else {
        BodyDebug::Info().ArmDownAimDeg = ArmDownDeg();
    }
    BodyDebug::Info().ArmDownDeg = ArmDownDeg();
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
                if (FirstPersonBodyAngleOf(turn) > 0.002f) ApplySpineRotation(turn);
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
    // Unarmed, the chest's heading: Spine Twist of the view's turn off the body.
    if (mmLayers && cfg.SpineTwist > 0.0f && !matched) {
        // Motion matching: an additive yaw - the view off the character facing (which turns as the mocap turns), capped,
        // spread over the spine - on top of the clip's chest. Not a closed loop: the gait's own counter-rotation of the
        // shoulders against the hips passes untouched (re-aimed every frame, the shoulder line was held square to the view:
        // the jog's chest swung 1.36x the clip's against the pelvis).
        // Eased (kMmChestTwistHalflife, FirstPersonBodyEaseTwist): instant, a 180 flick wound the chest 60 deg off the hips in 4
        // frames (1400 deg/s) while the clip's turned 5, and a flick past straight behind swung it 120 deg across in one frame.
        // (Holding the chest on its side through straight behind was tried: it kept the view looking back over the shoulder, and
        // on a 200 flick the jacket filled 15 % of the lower view against 9 % on the instant twist; eased across, 11 %.)
        const float lim = glm::radians(kMmChestTwistMax);
        m_TwistEased = FirstPersonBodyEaseTwist(m_TwistEased, m_Twist, lim, kMmChestTwistHalflife, dt);
        const float twist = m_TwistEased * std::min(cfg.SpineTwist, 1.0f);
        if (std::abs(twist) > 0.002f) ApplySpineAim(camera, 0.0f, twist);
    } else {
        m_TwistEased = 0.0f; // not drawn (armed, or the layers off): it comes back in from square, eased
    }
    if (!haveHead) return;

    // The camera into the head: smoothed in the body's own frame, so it never trails the move.
    // What the chest's tilt does to the head goes on in full, so the shoulders stay the same distance from the eye as
    // the view pitches. The head bob is procedural (the weapon's Camera Motion: a dip on each footfall, the landing's -
    // the view punch kept on top below), so the eye takes none of the clips' own bob: it follows the head's slow
    // average (its standing height, a crouch, a stop's lean), trailing the head by at most Eye Slack and pulled back
    // in over 40 ms past it - as the armed eye follows the shoulders.
    glm::vec3 tilt(0.0f);
    if (model->NodeTransform(model->NodeName(m_HeadNode), head)) tilt = glm::vec3(head[3]) - headAnimated;
    FirstPersonBodyFollowShoulders(m_HeadEased, m_HaveEye, headAnimated, cfg.EyeSlack, 0.04f, dt);
    // Motion matching, unarmed: the eye rides the mocap head's own motion by MM Head Ride (the owner, 2026-11-17: the
    // stabilized eye over a body bobbing as captured felt stiff, and the neck rose into the view at every jogging step).
    const float headRide = std::clamp(cfg.MmHeadRide, 0.0f, 1.0f) * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    const float armed = std::clamp(m_ArmsWeight, 0.0f, 1.0f);
    // With the sights up the ride steadies as the procedural bob it stands in for did (SetEyeRideScale).
    const float eyeRide = std::clamp(cfg.EyeRide, 0.0f, 1.0f) * std::clamp(m_EyeRideScale, 0.0f, 1.0f);
    m_CameraRide = headRide + eyeRide * armed;
    BodyDebug::Info().CameraRide = m_CameraRide;
    m_Eye = FirstPersonBodyEye(m_RestHead, glm::mix(m_HeadEased, headAnimated, headRide), 1.0f, cfg.CameraOffset);
    BodyDebug::Info().HeadHeight = headAnimated.y * reg.get<TransformComponent>(m_Body).Scale.y;

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
            const glm::vec3 inCamera = FirstPersonBodyRigEyeToShoulders(rigWorld, sl, sr, cb, camera);
            // The clips sway their shoulders about the camera; that must not move the view, so only a
            // slow drift of this offset is followed.
            if (!m_HaveRigOffset) { m_RigEyeToShoulders = inCamera; m_HaveRigOffset = true; }
            m_RigEyeToShoulders += (inCamera - m_RigEyeToShoulders) * Follow(dt, 0.4f);

            const glm::vec3 shoulders = shouldersNow();
            const glm::vec3 toRest = shoulders - shouldersAnimated; // the chest's tilt at the shoulders
            // The eye follows the shoulders' slow average - not the bind pose, whose shoulders sit elsewhere than the
            // standing pose's, which would put the shoulders (and so the hands' reach) that far off - plus Eye Ride of their
            // motion about it. Held at the average, the gun (on the rig, on the eye) and the hands stayed level while the
            // torso bobbed 5-9 cm under them, and the arms read as a separate animation; riding, the hands keep to the chest
            // and the procedural bob gives way by the same share (CameraRide).
            // The eye may trail or under-follow the shoulders by only Eye Slack: a clip that throws the
            // shoulders about (a stop pulling the body back) would otherwise carry them out of the
            // arms' reach and a hand would come off the gun. Bigger motions move the camera with them.
            // Past the slack the eye is pulled back in fast (40 ms) rather than snapped to it: snapped, its speed jumped
            // in one frame - a kink felt as a jolt when the body crouched or stood on the move.
            FirstPersonBodyFollowShoulders(m_Shoulders, m_HaveShoulders, shouldersAnimated, cfg.EyeSlack, 0.04f, dt);
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
            const glm::vec3 lift = FirstPersonBodyArmedEyeOffset(cfg.ArmedEyeOffset, viewPitch);
            const glm::vec3 rode = glm::mix(m_Shoulders, shouldersAnimated, eyeRide);
            const glm::vec3 fromShoulders = m_Feet + yaw * (t.Scale * (rode + toRest + lift)) - offset;
            // Remember where this puts the eye against the head's: unarmed the eye keeps that height, or
            // the camera would jump when the gun is holstered. Taken as it is (both eyes are already smoothed),
            // and held from the frame the arms start to ease off.
            const glm::vec3 delta = glm::inverse(yaw) * (fromShoulders - eyeWorld);
            if (!m_HaveArmedEye || m_ArmsWeight > 0.99f) m_ArmedEyeDelta = delta;
            m_HaveArmedEye = true;
            eyeWorld = glm::mix(eyeWorld, fromShoulders, std::min(m_ArmsWeight, 1.0f));
        }
    }
    if (m_HaveArmedEye) eyeWorld += yaw * m_ArmedEyeDelta * (1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f));
    // Looking down, the eye comes forward over the chest as a head does, pitching at the neck. The camera
    // rides at the head bone (the skull's base, over the neck), so straight down it looked into the
    // torso's neck opening: the torso was seen from inside (culled away) and the legs through it.
    eyeWorld += flat * (cfg.LookDownPush * FirstPersonBodyLookDown(viewPitch, cfg.LookDownStart));
    // ... and on, as far as it takes to keep the drawn upper torso past the near plane (unarmed: armed, the eye is the rig's,
    // and the chest still pitches with the aim).
    // Past what the push can do, the look-down limit (motion matching, unarmed - the owner, 2026-11-17: "limit the amount we can
    // look down a little bit so it doesn't clip"): the steepest pitch from which the opening is out of the view.
    {
        const float share = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        float limit = -89.0f;
        const float want = viewPitch < glm::radians(-10.0f) && share > 1e-3f ? share * EyeBodyPush(world, camera, eyeWorld, flat, limit) : 0.0f;
        if (share < 0.5f) limit = -89.0f;
        m_LookLimitAge += dt;
        if (limit >= m_LookLimitHeld || m_LookLimitAge > kLookLimitHold) {
            m_LookLimitHeld = limit;
            m_LookLimitAge = 0.0f;
        }
        m_LookLimit += (m_LookLimitHeld - m_LookLimit) * Follow(dt, m_LookLimitHeld > m_LookLimit ? 0.06f : 0.3f);
        BodyDebug::Info().LookLimitDeg = m_LookLimit;
        // (In at once - eased in, it trailed an eye riding the jog's bob and the neck showed for a frame or two at a step - out gently.)
        m_EyeClear = want > m_EyeClear ? want : m_EyeClear + (want - m_EyeClear) * Follow(dt, 0.15f);
        eyeWorld += flat * m_EyeClear;
        BodyDebug::Info().EyeBodyPush = m_EyeClear;
    }
    // The lean (a corner peek): the kick's sideways step is the head swinging over on the hips, and the arms rig - placed
    // off the camera - swings with it. The eye alone moving would put the rig's hands up to 30 cm off the body's
    // shoulders and a hand off the gun, so the spine leans the same way: the shoulders (and the head) go over with the
    // view and the arms reach the gun from there. Rolled about the view's flat forward, after the eye is worked out (the
    // camera is the unleaned eye plus the whole kick), sized so the shoulders' midpoint moves sideways as far as the kick
    // does: estimated off the spine's length, measured, trimmed once (the spine's spread and limits set the pivot). The
    // kick's drop and the rest (bob, landing dip, ADS camera share) stay on the camera alone - centimetres, which Arm
    // Steadiness (held in the camera's frame, below) and the elbows take up.
    {
        glm::vec3 flatRight = camera.Right();
        flatRight.y = 0.0f;
        const float len = glm::length(flatRight);
        const float side = len > 1e-4f ? glm::dot(viewKick, flatRight / len) : 0.0f;
        glm::mat4 root(1.0f);
        if (std::abs(side) > 1e-3f && haveShoulders && model->NodeTransform(Bone(FPBody::kBoneSpine[0]), root)) {
            const glm::vec3 rightModel = glm::inverse(yaw) * (flatRight / len);
            // About up x right: turning a point above the pivot by a positive angle carries it toward rightModel.
            const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), rightModel));
            const float scale = std::max(t.Scale.x, 1e-3f);
            const float want = side / scale;
            const glm::vec3 start = shouldersNow();
            const float lever = glm::length(start - glm::vec3(root[3]));
            if (lever > 0.05f) {
                float moved = 0.0f;
                for (int pass = 0; pass < 2; ++pass) {
                    const float angle = std::asin(std::clamp((want - moved) / lever, -0.9f, 0.9f));
                    if (std::abs(angle) < 1e-4f) break;
                    ApplySpineRotation(glm::angleAxis(angle, axis));
                    moved = glm::dot(shouldersNow() - start, rightModel);
                }
            }
        }
    }
    BodyDebug::Info().TorsoBendDeg = torsoBend();
    if (BodyDebug::Enabled()) {
        const glm::vec3 shoulders = m_Feet + yaw * (t.Scale * m_Shoulders);
        BodyDebug::Cross(eyeWorld, 0.04f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));       // the camera
        BodyDebug::Cross(shoulders, 0.04f, glm::vec4(1.0f, 0.3f, 1.0f, 1.0f));      // the shoulders the eye is anchored to
        BodyDebug::Line(shoulders, eyeWorld, glm::vec4(1.0f, 0.3f, 1.0f, 0.7f));
    }
    // The camera: the body's eye with the view punch kept on top. What this adds is measured from the camera as it came
    // in (the player's own eye + the kick), so it is the eye less the player's own eye: next frame RemoveViewKick takes
    // the kick off and BeforePlayerMove this, and the camera is the player's own eye again - nothing accumulates.
    BodyDebug::Info().BodyEye = eyeWorld;
    const glm::vec3 cameraIn = camera.Position;
    camera.Position = eyeWorld + viewKick;
    m_CameraApplied = camera.Position - cameraIn;
    BodyDebug::Info().EyeApplied = m_CameraApplied;
    // Arm Steadiness holds the shoulders in this frame: it moves with the camera (the eye, the bob, the lean - the arms
    // rig hangs off it) and the chest's pitch, so only the gait's sway shows in it (the rig's own frame pitches with the
    // view, the chest less).
    m_ChestView = glm::mat4(glm::vec4(chestRight, 0.0f), glm::vec4(chestUp, 0.0f), glm::vec4(chestFront, 0.0f), glm::vec4(camera.Position, 1.0f));
    m_HaveChestView = true;
    {
        const float unarmed = 1.0f - std::clamp(m_ArmsWeight, 0.0f, 1.0f);
        BodyDebug::Info().HandEyePush[0] = BodyDebug::Info().HandEyePush[1] = 0.0f;
        if (mmLayers && unarmed > 1e-3f) KeepHandsFromEye(glm::inverse(yaw) * (camera.Position - m_Feet) / std::max(t.Scale.y, 1e-3f), unarmed);
    }
    PostureMetrics(world, camera, dt);
}

// Puts each foot on the ground under it: a ray down from the animated foot gives how far the ground
// is above / below the capsule's, the pelvis drops to the lower foot, and the legs are re-solved to
// the offset feet (tilted toward the ground while planted). On every piece so they stay one skeleton.
bool FirstPersonBody::FootPoints(const World& world, glm::vec3 (&out)[2]) const {
    const auto& reg = world.Registry;
    if (!IsActive() || !reg.valid(m_Body) || !reg.valid(m_Driver)) return false;
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return false;
    const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 n(1.0f);
        if (!rc->ModelRef->NodeTransform(Bone(FPBody::kBoneFoot[s]), n)) return false;
        out[s] = scale * glm::vec3(n[3]);
    }
    return true;
}

bool FirstPersonBody::FootHeights(const World& world, float (&out)[2]) const {
    const auto& reg = world.Registry;
    if (!IsActive() || !reg.valid(m_Body) || !reg.valid(m_Driver)) return false;
    const auto* rc = reg.try_get<RenderableComponent>(m_Driver);
    if (!rc || !rc->ModelRef) return false;
    // As ApplyFootIK places the animated feet: the driver's model space sits on the feet, scaled by the root.
    const float scale = std::max(reg.get<TransformComponent>(m_Body).Scale.y, 1e-3f);
    for (int s = 0; s < 2; ++s) {
        glm::mat4 n(1.0f);
        if (!rc->ModelRef->NodeTransform(Bone(FPBody::kBoneFoot[s]), n)) return false;
        out[s] = scale * n[3].y;
    }
    return true;
}

bool FirstPersonBody::BoneWorld(const World& world, const std::string& standard, glm::vec3& out, bool worldTwins) const {
    glm::mat4 m(1.0f);
    if (!BoneWorldMatrix(world, standard, m, worldTwins)) return false;
    out = glm::vec3(m[3]);
    return true;
}

bool FirstPersonBody::BoneWorldMatrix(const World& world, const std::string& standard, glm::mat4& out, bool worldTwins) const {
    if (!IsActive() || !world.Registry.valid(m_Body)) return false;
    const auto& reg = world.Registry;
    auto isArms = [&](size_t k) { return IsArmsPiece(reg, m_Pieces[k]); };
    // The world twins when poses are split (what every view but the player's camera shows), else the pieces.
    const bool twins = worldTwins && !m_Twins.empty();
    const auto& ents = twins ? m_Twins : m_Pieces;
    const auto& models = twins ? m_TwinModels : m_Models;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k < m_Pieces.size() && k < models.size() && k < ents.size(); ++k) {
            if (!reg.valid(ents[k]) || !models[k]) continue;
            if (pass == 0 ? !isArms(k) : m_Pieces[k] != m_Driver) continue;
            glm::mat4 n(1.0f);
            if (!models[k]->NodeTransform(Bone(standard), n)) continue;
            out = world.ComposeWorldTransform(ents[k]) * n;
            return true;
        }
    return false;
}

const Model* FirstPersonBody::DriverModel() const {
    for (size_t k = 0; k < m_Pieces.size() && k < m_Models.size(); ++k)
        if (m_Pieces[k] == m_Driver) return m_Models[k].get();
    return nullptr;
}

bool FirstPersonBody::ChestTurn(glm::quat& out) const {
    const Model* m = DriverModel();
    glm::mat4 c(1.0f);
    if (!m || !m_HaveBindPosture || !m->NodeTransform(Bone(FPBody::kBoneSpine[4]), c)) return false;
    out = glm::normalize(IK::Rotation(c) * glm::inverse(m_BindChest));
    return true;
}

float FirstPersonBody::ChestPitchDeg() const {
    glm::quat turn;
    if (!ChestTurn(turn)) return 0.0f;
    const glm::vec3 up = turn * glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::degrees(std::atan2(glm::dot(up, kForward), up.y));
}
