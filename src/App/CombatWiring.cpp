#include "CombatWiring.h"

#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "AI/Npc.h"
#include "AI/NpcDirector.h"
#include "AI/NpcTest.h"
#include "Audio/FoleyAudio.h"
#include "BulletHoles.h"
#include "Combat/BloodFx.h"
#include "Combat/BloodLab.h"
#include "Combat/CombatFx.h"
#include "Combat/FxSprites.h"
#include "Combat/ImpactFx.h"
#include "Components.h"
#include "Creatures/SpiderDamage.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h" // QueryFilter / RaycastHit, kPlayerEntity
#include "KnifeFxLibrary.h"
#include "Model.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "SprayCan.h"
#include "World.h"

namespace {

void WorldImpactWith(const CombatWiringInputs& in, std::uint32_t entity, const glm::vec3& point, const glm::vec3& normal,
                     const glm::vec3& dir, float radius) {
    World& world = *in.world;
    ImpactFx& impactFx = *in.impactFx;
    const glm::vec3 p = point, n = normal;
    const std::string surface = impactFx.SurfaceOf(world, entity);
    impactFx.Spawn(surface, p, n, dir);
    int decal = -1;
    if (ImpactFx::IsStatic(world, entity)) decal = KnifeFxLibrary::Get().Find(ImpactFx::HoleEntry(surface));
    in.bulletHoles->Add(world, static_cast<entt::entity>(entity), point, normal, radius, decal, ImpactFx::HoleSize(surface, radius),
                        ImpactFx::HoleRim(surface));
}

// What the crosshair is on: a soldier's part (entity, part) or the world (entity, normal). False: nothing in reach.
bool AimAt(const CombatWiringInputs& in, glm::vec3& point, glm::vec3& normal, unsigned& entity, int& part, bool& body) {
    const Player& player = *in.player;
    const glm::vec3 o = player.Cam.Position, d = player.Cam.Front();
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    PhysicsWorld::BodyPartHit bp;
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit h;
    const bool world_ = PhysicsWorld::RaycastFiltered(oo, dd, 200.0f, f, h) && h.Hit;
    if (PhysicsWorld::RaycastBodyParts(oo, dd, 200.0f, bp) && (!world_ || glm::length(glm::vec3(bp.Point[0], bp.Point[1], bp.Point[2]) - o) < h.Distance)) {
        point = glm::vec3(bp.Point[0], bp.Point[1], bp.Point[2]);
        normal = -d;
        entity = bp.Entity;
        part = bp.Part;
        body = true;
        return true;
    }
    if (!world_) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
    entity = h.Entity;
    part = -1;
    body = false;
    return true;
}

// Blood on a soldier is pinned to the bone of the part it struck, in the bind pose every one of his pieces
// shares (the driver's); on a loose prop, to its mesh.
// A mesh's world -> bind transform through its bone nearest `point` (as posed now), or its own for a rigid
// one; false when its nearest bone is further than `reach` (the blood isn't on that piece).
bool MeshSpace(World& world, entt::entity e, const glm::vec3& point, float reach, glm::mat4& worldToBind) {
    const auto* r = world.Registry.valid(e) ? world.Registry.try_get<RenderableComponent>(e) : nullptr;
    if (!r || !r->ModelRef) return false;
    const Model& m = *r->ModelRef;
    const glm::mat4 w = world.GetCachedWorldTransform(e);
    if (!m.HasBones()) { worldToBind = glm::inverse(w); return true; }
    int best = -1;
    float bestD = reach;
    for (int i = 0; i < m.NodeCount(); ++i) {
        const int b = m.BoneId(m.NodeName(i));
        glm::mat4 node;
        if (b < 0 || !m.NodeTransformAt(i, node)) continue;
        if (const float d = glm::length(glm::vec3(w * node[3]) - point); d < bestD) { bestD = d; best = b; }
    }
    if (best < 0) return false;
    worldToBind = glm::inverse(w * m.FinalBoneMatrix(best));
    return true;
}

unsigned GunKey(const std::string& model) { return (unsigned)(std::hash<std::string>{}(model) | 1u); }

} // namespace

CombatWiring::~CombatWiring() {
    if (m_StepListener) FoleyAudio::Get().SetStepListener(nullptr);
}

void CombatWiring::WorldImpact(std::uint32_t entity, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir,
                               float radius) const {
    WorldImpactWith(m_In, entity, point, normal, dir, radius);
}

// Every closure below captures `in` (pointers) by value: they outlive this call, and main's objects outlive them.
void CombatWiring::Wire(const CombatWiringInputs& in) {
    m_In = in;
    BloodFx& bloodFx = *in.bloodFx;
    BloodLab& bloodLab = *in.bloodLab;
    NpcTest* const npcTest = in.npcTest;

    bloodFx.SetSprites(in.fxSprites);
    in.impactFx->SetSprites(in.fxSprites);
    in.combatFx->SetSprites(in.fxSprites); // PRO Effects' muzzle layers (Muzzle Style 1)

    if (npcTest)
        npcTest->ShootWorld = [in](const glm::vec3& from, const glm::vec3& dir) { // --npc-test blood: rounds into the surfaces
            const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
            QueryFilter f;
            f.HitTriggers = 0;
            RaycastHit h;
            if (!PhysicsWorld::RaycastFiltered(o, d, 20.0f, f, h) || !h.Hit) return false;
            const glm::vec3 p(h.Point[0], h.Point[1], h.Point[2]), n(h.Normal[0], h.Normal[1], h.Normal[2]);
            in.combatFx->Impact(*in.world, h.Entity, p);
            WorldImpactWith(in, h.Entity, p, n, dir, 0.0027f); // a 5.45 mm round
            return true;
        };
    if (npcTest) // --npc-test decals: stains at fixed spots
        npcTest->SpawnBlood = [in](int kind, const glm::vec3& at, const glm::vec3& n, float size) {
            kind == 0 ? in.bloodFx->SpatterWallAt(at, n, size) : in.bloodFx->SpawnPoolAt(at, n, size);
        };
    if (npcTest) { // --npc-test decals: the spray can
        npcTest->Spray = [in](const std::string& piece, const glm::vec3& from, const glm::vec3& dir, const std::string& painter, float seconds) {
            SprayCanAction a;
            a.Piece = piece; a.From = from; a.Direction = dir; a.Painter = painter; a.Seconds = seconds;
            return in.sprayCans->Start(*in.world, a) != entt::null;
        };
        npcTest->SprayNpc = [in](const std::string& piece, const glm::vec3& feet, float yawDeg, float seconds) {
            World& world = *in.world;
            const entt::entity npc = world.CreateEmptyEntity(feet, glm::vec3(0.0f, yawDeg, 0.0f), glm::vec3(1.0f), "Tenant (painter)");
            SprayCanAction a = SprayCanAction::ForCharacter(world, npc, piece);
            a.Seconds = seconds;
            return in.sprayCans->Start(world, a) != entt::null;
        };
    }

    // F7 -> Blood Lab: fire any blood / impact effect at the crosshair.
    bloodLab.Actions.FireAtCrosshair = [in](float damage, int pellets, bool head) {
        World& world = *in.world;
        NpcDirector& npcDirector = *in.npcDirector;
        glm::vec3 p, n;
        unsigned e = 0;
        int part = -1;
        bool body = false;
        if (!AimAt(in, p, n, e, part, body)) return;
        const glm::vec3 o = in.player->Cam.Position, d = glm::normalize(p - o);
        if (!body) { WorldImpactWith(in, e, p, n, d, 0.0027f); return; }
        if (head) // on the head whatever part was under the crosshair
            for (const auto& npc : npcDirector.Npcs())
                if (npc && (unsigned)entt::to_integral(npc->Root) == e && npc->Hitboxes && npc->Hitboxes->Active()) p = npc->Hitboxes->Centre(npc->Body, 2);
        FirstPersonWeaponGameplay w = in.firstPersonPresentation->Set().Gameplay;
        w.Damage = damage;
        w.Pellets = pellets;
        bool killed = false, headHit = false;
        for (int i = 0; i < pellets; ++i)
            npcDirector.OnPlayerHit(world, e, p + glm::vec3((float)(i % 3) - 1.0f, (float)(i / 3) - 1.0f, 0.0f) * (pellets > 1 ? 0.06f : 0.0f), o,
                                    glm::normalize(p - o), w, &killed, &headHit);
    };
    bloodLab.Actions.PoolAtCrosshair = [in] {
        glm::vec3 p, n;
        unsigned e = 0;
        int part = -1;
        bool body = false;
        if (AimAt(in, p, n, e, part, body) && !body && n.y > 0.6f) in.bloodFx->SpawnPoolAt(p, n, 1.2f);
    };
    bloodLab.Actions.DripsAtCrosshair = [in] {
        glm::vec3 p, n;
        unsigned e = 0;
        int part = -1;
        bool body = false;
        if (AimAt(in, p, n, e, part, body) && !body) in.bloodFx->SpatterWallAt(p, n, 0.6f);
    };
    bloodLab.Actions.ImpactAtCrosshair = [in] {
        glm::vec3 p, n;
        unsigned e = 0;
        int part = -1;
        bool body = false;
        if (AimAt(in, p, n, e, part, body) && !body) {
            in.combatFx->Impact(*in.world, e, p);
            WorldImpactWith(in, e, p, n, glm::normalize(p - in.player->Cam.Position), 0.0027f);
        }
    };
    bloodLab.Actions.ClearAll = [in] {
        in.bloodFx->Clear();
        in.fxSprites->Clear();
        in.bulletHoles->Clear();
    };

    FoleyAudio::Get().SetStepListener([bloodFx = in.bloodFx, player = in.player](int walker, const glm::vec3& feet, const glm::vec3& vel, int foot) {
        // Bloody footprints out of fresh blood; the player's point the way they face (turning on the spot too).
        bloodFx->OnFootstep(walker, feet, vel, foot, walker < 0 ? player->Cam.Front() : glm::vec3(0.0f));
    });
    m_StepListener = true; // dropped in the destructor, before the blood and the player are destroyed
    bloodFx.SetBodyLookup([in](unsigned entity, glm::vec3& centre) { // where a corpse lies, for its pool
        if (in.spiderDamage->BodyCentre(*in.world, entity, centre)) return true;
        for (const auto& n : in.npcDirector->Npcs()) {
            if (!n || (unsigned)entt::to_integral(n->Root) != entity) continue;
            // The middle of the body: pelvis toward chest (its pool spreads from under there).
            centre = n->Ragdoll ? glm::mix(n->Ragdoll->PartPosition(0), n->Ragdoll->PartPosition(1), 0.4f) : n->Feet + glm::vec3(0.0f, 0.9f, 0.0f);
            return true;
        }
        return false;
    });
    bloodFx.SetMemberResolver([in](unsigned key) -> unsigned { // the gun out now, if it's that one
        const FirstPersonPresentation& firstPersonPresentation = *in.firstPersonPresentation;
        if (!firstPersonPresentation.IsActive()) return 0xFFFFFFFFu;
        const entt::entity g = firstPersonPresentation.WeaponEntity();
        if (g == entt::null || GunKey(firstPersonPresentation.Set().WeaponModel) != key) return 0xFFFFFFFFu;
        return (unsigned)entt::to_integral(g);
    });
    bloodFx.SetSplatSpace([in](unsigned entity, int part, bool corpse, const glm::vec3& point, BloodFx::SplatSpace& out) {
        World& world = *in.world;
        const FirstPersonPresentation& firstPersonPresentation = *in.firstPersonPresentation;
        if (entity == kPlayerEntity) { // the player's own body pieces, arms and gun near the point
            out.Group = entity;
            glm::mat4 w2b;
            // Tight reach: the view-model arms and the world body's copies stand in different places, and blood
            // meant for the hands the player sees mustn't land on the hidden ones (or the other way round).
            for (auto e : world.Registry.view<PlayerBodyTag, RenderableComponent>())
                if (!world.Registry.all_of<PoseSourceTag>(e) && MeshSpace(world, e, point, 0.16f, w2b))
                    out.Members.emplace_back((unsigned)entt::to_integral(e), w2b);
            // The gun: speckles off a point-blank hit - never the player's own wound (kBodyOnlyPart). Keyed by its model,
            // so the blood comes back with it after a weapon swap rebuilds it.
            if (part != BloodFx::kBodyOnlyPart && firstPersonPresentation.IsActive())
                if (entt::entity g = firstPersonPresentation.WeaponEntity(); g != entt::null && MeshSpace(world, g, point, 0.6f, w2b)) {
                    out.Keys.resize(out.Members.size(), 0u);
                    out.Members.emplace_back((unsigned)entt::to_integral(g), w2b);
                    out.Keys.push_back(GunKey(firstPersonPresentation.Set().WeaponModel));
                }
            return !out.Members.empty();
        }
        for (const auto& n : in.npcDirector->Npcs()) {
            if (!n || (unsigned)entt::to_integral(n->Root) != entity) continue;
            const Model* m = n->Body.DriverModel();
            const entt::entity driver = n->Body.Driver();
            if (!m || driver == entt::null || !world.Registry.valid(driver)) return false;
            std::string bone = part >= 0 ? (corpse ? NpcRagdollDefOf(part).Bone : NpcPartDefOf(part).Bone) : "";
            if (bone.empty()) { // the nearest part's bone
                float best = 1e9f;
                for (int i = 0; i < NpcRagdoll::kParts; ++i) {
                    glm::vec3 at;
                    if (!n->Body.BoneWorld(world, NpcPartDefOf(i).Bone, at)) continue;
                    if (const float d = glm::length(at - point); d < best) { best = d; bone = NpcPartDefOf(i).Bone; }
                }
            }
            if (bone.empty()) return false;
            // Each mesh through its own copy of that bone, as posed now: pieces don't share a bind pose.
            out.Group = entity;
            auto add = [&](entt::entity piece) {
                const auto* r = world.Registry.valid(piece) ? world.Registry.try_get<RenderableComponent>(piece) : nullptr;
                if (!r || !r->ModelRef) return;
                const int b = r->ModelRef->BoneId(bone);
                if (b < 0) return;
                out.Members.emplace_back((unsigned)entt::to_integral(piece),
                                         glm::inverse(world.GetCachedWorldTransform(piece) * r->ModelRef->FinalBoneMatrix(b)));
            };
            add(driver);
            for (entt::entity p : n->Body.Pieces())
                if (p != driver) add(p);
            (void)m;
            return !out.Members.empty();
        }
        if (in.spiderDamage->Owns(entity)) return in.spiderDamage->SplatSpace(world, entity, part, point, out);
        const entt::entity e = static_cast<entt::entity>(entity);
        if (!world.Registry.valid(e) || !world.Registry.all_of<RigidbodyComponent, RenderableComponent>(e)) return false;
        out.Group = entity;
        out.Members.emplace_back(entity, glm::inverse(world.GetCachedWorldTransform(e)));
        return true;
    });
    bloodFx.SetPlayerGear([in](glm::vec3* points, glm::vec3* normals, int max) {
        World& world = *in.world;
        const FirstPersonPresentation& firstPersonPresentation = *in.firstPersonPresentation;
        // Along the barrel from the muzzle back, and the hands on it (the view model's world positions).
        int n = 0;
        glm::vec3 fpMuzzle, worldMuzzle, bore;
        if (!firstPersonPresentation.IsActive() || !firstPersonPresentation.MuzzleFrames(fpMuzzle, worldMuzzle, bore)) return 0;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 side = glm::normalize(glm::cross(bore, up));
        for (float t : {0.08f, 0.2f, 0.35f}) {
            if (n >= max) break;
            points[n] = fpMuzzle - bore * t + up * 0.02f;
            normals[n++] = glm::normalize(up + bore * 0.6f);
        }
        for (auto e : world.Registry.view<PlayerBodyTag, RenderableComponent, ViewModelTag>()) { // the arms the player sees
            const auto& r = world.Registry.get<RenderableComponent>(e);
            if (!r.ModelRef || !r.ModelRef->HasBones()) continue;
            for (const char* hand : {"hand_r", "hand_l"}) {
                glm::mat4 node;
                if (n >= max || !r.ModelRef->NodeTransform(hand, node)) continue;
                points[n] = glm::vec3(world.GetCachedWorldTransform(e) * node[3]) + up * 0.03f;
                normals[n++] = glm::normalize(up + bore * 0.5f + side * (hand[5] == 'r' ? 0.3f : -0.3f));
            }
            break; // one piece carries the hand bones for all
        }
        return n;
    });
    bloodFx.SetSplatMembers([in](unsigned group, std::vector<unsigned>& out) {
        World& world = *in.world;
        if (group == kPlayerEntity) return true;
        for (const auto& n : in.npcDirector->Npcs())
            if (n && (unsigned)entt::to_integral(n->Root) == group) return true;
        if (in.spiderDamage->Owns(group)) return true;
        const entt::entity e = static_cast<entt::entity>(group);
        if (!world.Registry.valid(e) || !world.Registry.all_of<RigidbodyComponent, RenderableComponent>(e)) return false;
        out.push_back(group);
        return true;
    });
    bloodFx.SetPropRay([in](const glm::vec3& o, const glm::vec3& d, float maxD, glm::vec3& p, glm::vec3& n, unsigned& entity) {
        World& world = *in.world;
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit h;
        if (!PhysicsWorld::RaycastFiltered(oo, dd, maxD, f, h) || !h.Hit) return false;
        const entt::entity e = static_cast<entt::entity>(h.Entity);
        if (!world.Registry.valid(e) || !world.Registry.all_of<RigidbodyComponent, RenderableComponent>(e)) return false;
        p = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
        n = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
        entity = h.Entity;
        return true;
    });
    in.npcDirector->Fx = in.combatFx;
}
