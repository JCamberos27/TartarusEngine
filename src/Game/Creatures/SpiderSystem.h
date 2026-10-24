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

    // A hunter's state of mind (SpiderAI.cpp).
    enum class Mode : int { Perch, Stalk, Engage, Telegraph, Leap, Bite, Recover, Reposition, Search, Scripted };

    void Think(World& world, AssetLibrary& assets, float dt, const Target* target = nullptr);
    void LatePose(World& world, float dt);
    void Clear() { m_Bodies.clear(); m_Nav.Clear(); m_NavTried = false; }
    bool Active() const { return !m_Bodies.empty(); }

    // The level's surface graph (SpiderNav.h), baked (or read from its cache) when the first spider appears in Play.
    // Empty when the bake failed: spiders then head straight for their goals.
    const Spider::SurfaceGraph& Nav() const { return m_Nav; }

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
        float TapIn = 0.0f;          // seconds until another leg tap may sound (eight legs would chatter)
        bool WasAttached = true;     // last frame: holding on (a landing is heard)
        // The hunter (SpiderAI.cpp).
        struct Mind {
            Mode State = Mode::Perch;
            float Clock = 0.0f;          // seconds in this state
            bool Aware = false, Token = false, Struck = false;
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
    void BuildNav();
    // Where the mover heads this frame: the route's next waypoint (as far along the surface it is on as the route stays
    // on it), or the goal itself on its last stretch or without a graph.
    glm::vec3 Steer(Body& b, const glm::vec3& goal, float dt);
    // The hunter's decision for this frame: sets the component's goal / run, triggers attacks and leaps (SpiderAI.cpp).
    void Hunt(World& world, Body& b, SpiderComponent& sc, const Target& t, float dt);
    bool SeesPlayer(const Body& b, const SpiderComponent& sc, const Target& t) const;
    int PickPerch(const glm::vec3& near) const;
    static inline int s_SoundsStarted = 0, s_SoundsMissing = 0;
    int Attackers() const;

    std::vector<PlayerHit> m_PlayerHits;
    int m_SpawnCount = 0;

    Spider::SurfaceGraph m_Nav;
    bool m_NavTried = false;

    std::vector<Body> m_Bodies;
    std::uint64_t m_Generation = 0;
    std::string m_RigPath, m_RigText;
    Spider::RigDef m_Rig;
    bool m_RigOk = false;
    std::string m_PrefabPath, m_PrefabText;
};
