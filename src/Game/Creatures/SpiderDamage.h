#pragma once

#include "Combat/BloodFx.h"
#include "IK.h"
#include "PhysicsWorld.h"
#include "SpiderLocomotion.h"
#include "SpiderRagdoll.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class SpiderSystem;
class World;
struct FirstPersonWeaponGameplay;

// What rounds do to the enemy spiders (SpiderSystem.h moves and poses them; this hurts and kills them):
//   - hitboxes: one query-only capsule per body part (Spider.rig.json "parts": the cephalothorax, the abdomen and three
//     segments of each leg), posed every frame on the animated, IK'd and flinched bones, so the player's rounds stop on
//     the spider (and never punch a hole in the wall behind it) and say which part they struck.
//   - damage by zone (the rig's "zones"): the head critical, the abdomen soft (and bleeding hardest), a leg light - a leg
//     round staggers it, curls that leg up a moment and leaves it limping. A heavy round (a shotgun's, or past
//     kHeavyDamage) into a spider on a wall or the ceiling knocks it off: it falls, lands and rights itself.
//   - a flinch: a damped spring kicking the struck part's bones along the round, added after the leg IK.
//   - death: the body handed to a 26-part ragdoll built from the same table, carrying the bones' velocity, its legs
//     folded under it by a limp motor that then fades (SpiderRagdoll.h); it settles, sleeps and stays.
// The ichor (BloodPalette::Ichor) is the host's to throw: Blood() makes the BloodFx hit of a round.
class SpiderDamage {
public:
    static constexpr float kHeavyDamage = 25.0f;

    // One round into a spider, as it landed.
    struct Result {
        unsigned Entity = 0xFFFFFFFFu;
        int Part = -1;                          // the rig's part (hitbox and ragdoll share the index)
        Spider::PartZone Zone = Spider::PartZone::Abdomen;
        float Damage = 0.0f;                    // after the zone and the range
        bool Living = false;                    // it was alive when hit (else a corpse)
        bool Killed = false, KnockedOff = false;
        glm::vec3 Point{0.0f}, Origin{0.0f}, Direction{0.0f, 0.0f, -1.0f};
        int Pellets = 1;
    };
    // A round of `weapon` (one of `pellets` rays) into `entity` at `point`, fired from `origin` along `dir`. False when
    // `entity` is not a spider (alive or dead): the round is somebody else's. True: it stopped in the spider.
    bool OnHit(World& world, SpiderSystem& spiders, unsigned entity, const glm::vec3& point, const glm::vec3& origin,
               const glm::vec3& dir, const FirstPersonWeaponGameplay& weapon, int pellets, Result& out);
    // The same for a round of `damage` before its zone (tests; the Blood Lab).
    bool OnHit(World& world, SpiderSystem& spiders, unsigned entity, const glm::vec3& point, const glm::vec3& origin,
               const glm::vec3& dir, float damage, int pellets, Result& out);
    // The BloodFx hit of that round: the spider's ichor, the abdomen's heavier.
    static BloodFx::Hit Blood(const Result& r);
    // The damage x of a zone in `rig` (Head 2.5, Abdomen 1.2, Leg 0.5 by default).
    static float ZoneMultiplier(const Spider::RigDef& rig, Spider::PartZone zone);
    static bool Heavy(float damage, int pellets) { return pellets > 1 || damage > kHeavyDamage; }

    // After SpiderSystem::LatePose, every frame of Play: the hurt legs and limps, the flinches onto the pose, the
    // hitboxes onto the bones, the dead onto their ragdolls.
    void LateUpdate(World& world, SpiderSystem& spiders, float dt);
    void Clear() { m_Bodies.clear(); }
    // A spider about to be destroyed (F12): its hitboxes and ragdoll go now, before its entity does - a query between the
    // two would otherwise hit a hitbox reporting an entity that no longer exists.
    void Forget(entt::entity root);

    // For the blood: whether `entity` is a spider's root, where its body is now (a corpse's pool spreads from under it),
    // and the mesh space a wound splat on part `part` near `point` is pinned in.
    bool Owns(unsigned entity) const;
    bool BodyCentre(const World& world, unsigned entity, glm::vec3& centre) const;
    bool SplatSpace(const World& world, unsigned entity, int part, const glm::vec3& point, BloodFx::SplatSpace& out) const;

    // What a test reads back about one spider (by its root).
    struct View {
        bool Ready = false, Dead = false, Asleep = false;
        int Parts = 0, Hitboxes = -1, Ragdoll = -1;
        int Hits = 0;
        float LastDamage = 0.0f;
        int LastPart = -1;
        float Flinch = 0.0f;        // the largest flinch angle now (rad)
        float LegCurlMax = 0.0f;    // the most curled hurt leg now (0..1)
        float SpeedScale = 1.0f;    // the limp
        float DeadFor = 0.0f, AsleepAt = -1.0f;
        float MotorStrength = 0.0f;
        float LegFold = 0.0f;       // dead: the mean angle (deg) each leg's tip has turned about the body since death (the curl)
        float LowestPart = 0.0f;    // dead: the lowest capsule's bottom (world y)
        glm::vec3 Centre{0.0f};     // dead: the root part's centre
        glm::vec3 Up{0.0f, 1.0f, 0.0f}; // dead: the body's up (its back) as it lies now
        bool Finite = true;         // dead: every part's pose finite
        float MaxSpeed = 0.0f;      // dead: the fastest part's speed now (m/s)
    };
    bool Inspect(entt::entity root, View& out) const;
    // A part by its rig name (-1: none), and where its capsule's centre is now (world).
    int PartIndex(const std::string& name) const;
    bool PartCentre(entt::entity root, int part, glm::vec3& out) const;
    const Spider::RigDef& Rig() const { return m_Rig; }

private:
    struct Body {
        entt::entity Root = entt::null, Model = entt::null;
        bool Ready = false;
        int Count = 0;
        std::vector<int> Parents;
        std::array<int, Spider::kMaxParts> Node{}, StartNode{}, EndNode{};
        std::array<glm::vec3, Spider::kLegs> LegOut{}; // each leg's hip out from the body (model space)
        IK::Pose Pose;
        std::vector<glm::mat4> Globals;
        int Hitboxes = -1;
        std::array<PhysicsWorld::HitCapsule, Spider::kMaxParts> Caps{};
        std::array<glm::vec3, Spider::kMaxParts> Centre{}, PrevCentre{}; // world, this frame and last
        float PrevDt = 0.0f;
        bool HavePrev = false;
        // the flinch: per part, a rotation vector (model space) on a critically damped spring
        std::array<glm::vec3, Spider::kMaxParts> FlAngle{}, FlVel{};
        bool Flinching = false;
        std::array<float, Spider::kLegs> CurlAge{}; // seconds since that leg was struck (< 0: never / done)
        float LimpAge = -1.0f;
        // the score
        int Hits = 0, LastPart = -1;
        float LastDamage = 0.0f;
        // death
        bool Dead = false;
        int Ragdoll = -1;
        float DeadFor = 0.0f;
        SpiderRagdoll::LimpMotor Motor;
        SpiderRagdoll::Settler Settle;
        std::vector<int> Owner;            // per node: the part that carries it (-1: none, it keeps its pose)
        std::vector<glm::mat4> Offset;     // per node: its world = its part's capsule world * Offset
        std::array<glm::mat4, Spider::kMaxParts> PartWorld{};
        glm::vec3 UpInRoot{0.0f, 1.0f, 0.0f}; // the body's up at death, in the root part's frame
        std::array<glm::vec3, Spider::kLegs> TipAtDeath{}; // each leg tip relative to the root part, its frame at death
        glm::vec3 RootOffset{0.0f};        // the root entity from the root part's centre at death (world)
        bool PoseWritten = false;
    };
    Body* Find(unsigned entity);
    const Body* Find(unsigned entity) const;
    bool Resolve(World& world, Body& b);
    void Kill(World& world, SpiderSystem& spiders, Body& b, int part, const glm::vec3& dir, float damage, const glm::vec3& point);
    void Flinch(Body& b, int part, const glm::vec3& dirWorld, float damage, const glm::mat4& model);
    void PoseLiving(World& world, SpiderSystem& spiders, Body& b, float dt);
    void PoseDead(World& world, Body& b, float dt);
    int RootPart() const;

    std::vector<Body> m_Bodies;
    std::uint64_t m_Generation = 0;
    std::string m_RigPath;
    Spider::RigDef m_Rig;
    bool m_RigOk = false;
};
