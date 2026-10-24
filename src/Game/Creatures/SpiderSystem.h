#pragma once

#include "IK.h"
#include "SpiderLocomotion.h"
#include "SpiderNav.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class AssetLibrary;
struct SpiderComponent;

namespace Spider {
// The solid world (statics and kinematic bodies, not props rolling about, not triggers), counted. StaticOnly: the static
// world alone - the surface graph's bake, which must see just the triangles it is built from, never a door, the player or
// a spider where it happens to be (or the graph differs with the moment it was baked, and the cache keeps that).
// A ray finds the first face it enters, as the unit tests' boxes do: the live one passes through a solid it starts inside
// (PhysX reports that solid at the ray's origin, so a probe begun in a table top or a slab stood the body on its own
// probe's start, and each frame's probe lifted it a quarter metre, up through the slab).
class WorldQuery : public SurfaceQuery {
public:
    explicit WorldQuery(bool staticOnly = false) : StaticOnly(staticOnly) {}
    bool Ray(const glm::vec3& from, const glm::vec3& dir, float maxDistance, SurfaceHit& out) const override;
    bool Occupied(const glm::vec3& centre, float radius) const override;
    bool StaticOnly = false;
    mutable int Rays = 0;
};
} // namespace Spider
class World;

// The enemy spiders in Play: every active entity with a Spider component (a spider prefab's root, its model and Animator
// Controller on a child). Think moves each body over the surfaces toward its goal before the animators run; LatePose,
// after them, steps the legs and puts them onto their footholds with IK (SpiderLocomotion.h has the how).
class SpiderSystem {
public:
    // Who the hunters hunt: the player, as they are this frame.
    struct Target {
        bool Valid = false, Dead = false;
        glm::vec3 Feet{0.0f}, Eye{0.0f}, Velocity{0.0f};
        float Radius = 0.3f;
        bool Loud = false;           // a shot, or a sprint: heard through walls (within ~20 m)
    };
    // What a hunter did to the player this frame (the host applies the damage, the shove and the view's jolt).
    struct PlayerHit {
        float Amount = 0.0f;
        glm::vec3 Source{0.0f};      // the spider
        glm::vec3 Push{0.0f};        // m/s added to the player's velocity
        bool Pounce = false;         // a leap or a drop landed (harder jolt) rather than a bite
    };
    std::vector<PlayerHit> TakePlayerHits() { std::vector<PlayerHit> out; out.swap(m_PlayerHits); return out; }
    // The player's velocity after a hit's shove (`push`, m/s along the ground): it staggers, never throws. Their speed
    // along the ground ends no faster than the shove or than they were already going, so hits from two sides (two
    // attackers, a pounce then a bite) don't stack into a throw across the room.
    static glm::vec3 Shove(const glm::vec3& velocity, const glm::vec3& push);

    // A hunter's state of mind (SpiderAI.cpp).
    enum class Mode : int { Perch, Stalk, Engage, Telegraph, Leap, Bite, Recover, Reposition, Search, Scripted };

    void Think(World& world, AssetLibrary& assets, float dt, const Target* target = nullptr);
    void LatePose(World& world, float dt);
    void Clear() { m_Bodies.clear(); m_Nav.Clear(); m_NavTried = false; m_Barriers.clear(); m_DoorBlocks.clear(); }
    bool Active() const { return !m_Bodies.empty(); }

    // The level's surface graph (SpiderNav.h), baked (or read from its cache) when the first spider appears in Play.
    // Empty when the bake failed: spiders then head straight for their goals.
    const Spider::SurfaceGraph& Nav() const { return m_Nav; }
    // The level's creature barriers (CreatureBarrierComponent), gathered with the graph's bake.
    const std::vector<Spider::Barrier>& Barriers() const { return m_Barriers; }
    // Tests: whether the graph in use - maybe read from the disk cache - is the one the builder makes from this level
    // now. A cache the builder has moved on from (a change to Build without a new SurfaceGraph::kBuildVersion) fails it.
    bool NavMatchesFreshBuild(std::string& why) const;

    // A spider from `prefab` (a scene file whose root carries a Spider component) at `pos` facing `rot`: its root, or
    // entt::null when the prefab won't load.
    entt::entity Spawn(World& world, AssetLibrary& assets, const std::string& prefab, const glm::vec3& pos, const glm::quat& rot);

    // What a test reads back about one spider (by its root).
    struct Stats {
        glm::vec3 Pos{0.0f}, Up{0.0f, 1.0f, 0.0f}, Forward{0.0f, 0.0f, -1.0f};
        glm::vec3 SurfaceNormal{0.0f, 1.0f, 0.0f}, LegNormal{0.0f}; // under the body; the planted legs' mean
        float Speed = 0.0f, SurfaceGap = 0.0f;
        bool Ready = false, Attached = false, Arrived = false;
        int Lifted[2] = {0, 0};      // legs of each gait group mid-step now
        int BothLifted = 0;          // frames ever with both groups lifted at once
        int Steps = 0;               // steps finished, all legs
        int Grounded = 0;            // legs standing on something now
        float TipError = 0.0f;       // the worst leg's IK miss this frame (m): tip vs where the gait put the foot
        float PlantedSlide = 0.0f;   // the worst planted foot's travel this frame (m): should be none
        int Rays = 0;                // surface rays this frame
        int RouteNodes = 0;          // the current route's length in graph nodes (0: heading straight for the goal)
        int Replans = 0;             // routes planned so far
        Mode AiMode = Mode::Scripted;
        bool Aware = false, HasToken = false;
        int Bites = 0, Pounces = 0, Drops = 0, Leaps = 0; // landed on the player / leaps and drops tried
        float StuckFor = 0.0f, WorstStuck = 0.0f; // seconds it has meant to move and not (now / the longest)
        glm::vec3 WorstStuckAt{0.0f}, WorstStuckGoal{0.0f}; int WorstStuckMode = -1; // where, heading where, doing what
        int SunkFrames = 0;          // frames its head or abdomen was inside a surface
        int DoorsOpened = 0;         // shut doors it pushed open on its way
        glm::vec3 Goal{0.0f};        // where it is heading (the component's goal)
        bool HasGoal = false;
    };
    bool Inspect(entt::entity root, Stats& out) const;
    // One of the spider's sounds (snd.foley.spider.<element>: tap, hiss, screech, bite, pounce, hurt, death, thud, splat)
    // at `pos`, when the audio is up.
    static void Sound(const char* element, const glm::vec3& pos, float gain = 1.0f);
    // Spider sounds started so far (and asked for with no files to play: the manifest lacks them), for tests.
    static int SoundsStarted() { return s_SoundsStarted; }
    static int SoundsMissing() { return s_SoundsMissing; }

    // How a hurt or dead spider moves (SpiderDamage.h writes these): a struck leg drawn up off its foothold for a moment, a
    // limp, and a dead body handed to its ragdoll, neither moved nor posed here from then on.
    struct Hurt {
        std::array<float, Spider::kLegs> LegCurl{}; // 0..1: the leg's foot drawn up toward its hip
        float SpeedScale = 1.0f;                    // x its speed
        bool Dead = false;
    };
    Hurt* HurtOf(entt::entity root);
    // Wakes a hunter: it was shot, or heard a shot. It turns on the player wherever they are.
    void Alarm(entt::entity root);
    // A gunshot at `origin`: every hunter within `radius` m hears it.
    void HearShot(const glm::vec3& origin, float radius = 25.0f);
    // Knocks a spider off whatever it holds, moving at `velocity` (world, m/s): it falls, lands and rights itself.
    bool Detach(entt::entity root, const glm::vec3& velocity);

private:
    struct Body {
        entt::entity Root = entt::null, ModelEntity = entt::null;
        bool Ready = false, GaitStarted = false;
        Spider::Mover Mover;
        Spider::Gait Gait;
        std::array<std::vector<int>, Spider::kLegs> Chains{};
        std::array<glm::vec3, Spider::kLegs> TipOffset{};
        int Thorax = -1;
        std::vector<int> Parents;
        IK::Pose Pose;
        std::vector<glm::mat4> Globals;
        std::array<glm::vec3, Spider::kLegs> PrevFoot{};
        std::array<bool, Spider::kLegs> PrevPlanted{};
        // The route to the goal: graph nodes' positions and normals, the one being headed for, what it was planned to.
        std::vector<glm::vec3> Route, RouteNormal;
        std::vector<int> RouteScratch;
        int RouteAt = 0;
        glm::vec3 RouteGoal{0.0f};
        float RouteAge = 0.0f;
        bool RouteFailed = false;
        // How its routes weigh floor, wall and ceiling: the hunter makes climbing cheap to stalk, the floor to charge.
        Spider::PathCosts Costs;
        // A shut door on its way (OpenableComponent): it goes to where it presses on it - the leaf's middle when it swings
        // away, past its latch edge when it swings toward the spider (clear of the swing) - pushes it open, and waits.
        entt::entity Door = entt::null;
        glm::vec3 DoorPress{0.0f}, DoorFace{0.0f};
        float DoorFor = 0.0f;        // seconds since it found the door
        bool DoorPushed = false;
        std::vector<glm::vec4> PerchAvoid; // the far sides of shut doors met while settling (see PickPerch)
        float TapIn = 0.0f;          // seconds until another leg tap may sound (eight legs would chatter)
        bool WasAttached = true;     // last frame: holding on (a landing is heard)
        // The hunter (SpiderAI.cpp).
        struct Mind {
            Mode State = Mode::Perch;
            float Searched = 0.0f;       // Search: seconds spent looking about where it last knew the player was
            float Clock = 0.0f;          // seconds in this state
            bool Aware = false, Token = false, Struck = false;
            bool Alarmed = false;        // shot or heard a shot since its last think
            bool Hold = false;           // facing its goal without moving (the tell, a bite)
            bool FromDrop = false, Landed = false; // this leap is a drop from above; it struck the player
            float SeenAgo = 1e9f;        // seconds since it last saw or heard the player
            float LookIn = 0.0f;         // seconds to the next sight check (staggered between spiders)
            float PounceIn = 0.0f, BiteIn = 0.0f; // cooldowns
            float AimIn = 0.0f;          // seconds to re-aiming at a spot over the player
            int BitesInRow = 0;
            int Perch = -1;              // graph node it waits on
            glm::vec3 LastKnown{0.0f};
            bool Dropper = false;        // waits overhead and drops (the others charge)
        } Ai;
        Stats Last;
        SpiderSystem::Hurt Pain; // (SpiderDamage)
    };
    bool Resolve(World& world, AssetLibrary& assets, Body& b);
    void BuildNav(const World& world);
    // Where the mover heads this frame: the route's next waypoint (as far along the surface it is on as the route stays
    // on it), or the goal itself on its last stretch or without a graph.
    glm::vec3 Steer(Body& b, const glm::vec3& goal, float dt);
    // A shut door ahead on its way: pushes it open (Openable::Open) and holds while it swings; returns where to head
    // meanwhile and sets `hold` while it waits.
    glm::vec3 Doors(World& world, Body& b, const glm::vec3& aim, float dt, bool& hold);
    // The hunter's decision for this frame: sets the component's goal / run, triggers attacks and leaps (SpiderAI.cpp).
    void Hunt(World& world, Body& b, SpiderComponent& sc, const Target& t, float dt);
    bool SeesPlayer(const Body& b, const SpiderComponent& sc, const Target& t) const;
    bool LeapClear(const Body& b, const SpiderComponent& sc, const Target& t, float dist) const;
    int Reach(const Body& b) const; // the graph piece it is on (-1: none near) - its goals are picked inside it
    // `avoid`: planes (normal, offset) a perch must not lie behind - the far side of a shut door it won't push while it
    // only settles.
    int PickPerch(const glm::vec3& near, int component, const std::vector<glm::vec4>* avoid = nullptr) const;
    static inline int s_SoundsStarted = 0, s_SoundsMissing = 0;
    int Attackers() const;

    std::vector<PlayerHit> m_PlayerHits;
    int m_SpawnCount = 0;

    Spider::SurfaceGraph m_Nav;
    bool m_NavTried = false;
    std::vector<Spider::Barrier> m_Barriers;
    // The graph nodes each door's leaf stands over as it hangs now: open or swinging, blocked to routes (a route never runs
    // up the wall behind an open door); shut, its doorway weighed (pushing it costs time). Redone as a leaf moves.
    struct DoorBlock {
        entt::entity Door = entt::null;
        float Progress = -1.0f;      // (none yet: a shut door is weighed from the first frame)
        std::vector<int> Nodes;      // blocked under its open leaf, or weighed in its shut doorway
        bool Shut = false;
    };
    std::vector<DoorBlock> m_DoorBlocks;
    void UpdateDoorBlocks(World& world);

    std::vector<Body> m_Bodies;
    std::uint64_t m_Generation = 0;
    std::string m_RigPath, m_RigText;
    Spider::RigDef m_Rig;
    bool m_RigOk = false;
    std::string m_PrefabPath, m_PrefabText;
};
