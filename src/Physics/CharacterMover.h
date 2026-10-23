#pragma once

#include <glm/glm.hpp>

#include <vector>

// The characters' collide-and-slide (the player's and the soldiers'): a capsule swept through the world in a few slides,
// creases between two walls followed, steps climbed only as high as the room over them allows, ledges only stood on
// while the capsule's axis is over them, and the floor followed down slopes and steps. It knows the world only through
// IMoverWorld's sweeps, so the unit tests can run it against any world; PhysicsWorld runs it against the PhysX scene.
//
// Positions are the capsule's feet (the bottom of its lower sphere); +Y is up.

struct MoverCapsule {
    float Radius = 0.3f;
    float HalfHeight = 0.6f; // the cylinder's half length (the centre is Radius + HalfHeight over the feet)
};

struct MoverHit {
    float Distance = 0.0f;       // along the sweep, to the touch
    glm::vec3 Point{0.0f};       // world
    glm::vec3 Normal{0.0f};      // world, out of what was hit
    const void* Actor = nullptr; // whatever the world wants to know it by again (PhysX: the actor and the shape)
    const void* Shape = nullptr;
    bool Character = false;      // another character (its capsule or a hitbox): never ground, never a step
};

class IMoverWorld {
public:
    virtual ~IMoverWorld() = default;
    // The first thing the capsule, feet at `feet`, meets moving `dist` along `dir` (unit length). Something it already
    // overlaps only counts while the move goes deeper into it.
    virtual bool Sweep(const MoverCapsule& cap, const glm::vec3& feet, const glm::vec3& dir, float dist, MoverHit& out) = 0;
    // The capsule at `feet` inside something: how to get out of the deepest of it (false: it's clear).
    virtual bool Penetration(const MoverCapsule& cap, const glm::vec3& feet, glm::vec3& outPush) = 0;
    // A ray from `from` along `dir` (unit length), up to `dist`: the first solid surface (its own normal, not a sweep's).
    virtual bool Raycast(const glm::vec3& from, const glm::vec3& dir, float dist, MoverHit& out) = 0;
};

struct MoverSettings {
    float Skin = 0.02f;            // kept between the capsule and everything it touches
    float StepHeight = 0.35f;      // climbed without a jump (no higher than the room above allows)
    float WalkableCos = 0.6428f;   // cos of the steepest floor stood on (50 degrees)
    float StepDownSnap = 0.3f;     // followed down a step (or a slope) while walking
    float LedgeTolerance = 0.6f;   // stood on an edge only while it touches within this much of the radius from the axis
    float DoorAssist = 0.0f;       // 0..1: grazing a jamb's corner while walking into a doorway eases the capsule past it
    float DoorAssistMaxAngle = 35.0f; // degrees: how squarely the move must meet the jamb for the assist
    int MaxSlides = 5;
    int MaxDepenetrations = 4;
};

// What the move left under the feet.
struct MoverFloor {
    bool Found = false;      // something under the feet within the probe
    bool Walkable = false;   // ... that can be stood on (shallow enough, not a character, the axis over it)
    bool Perched = false;    // ... found, but only the capsule's rim is on it: slides off
    float Gap = 0.0f;        // from the feet down to it
    MoverHit Hit;
};

// The record of one move, for the debug overlay.
struct MoverDebug {
    struct Plane { glm::vec3 Point, Normal; int Kind; };        // Kind 0 wall, 1 walkable, 2 character
    struct Step { glm::vec3 At; float Headroom; bool Taken; };
    std::vector<Plane> Planes;
    std::vector<glm::vec3> Creases;                              // pairs: from, to
    std::vector<Step> Steps;
    std::vector<glm::vec3> Nudges;                               // pairs: from, to (the doorway assist)
    glm::vec3 Feet{0.0f};
    MoverFloor Floor;
    void Clear() { Planes.clear(); Creases.clear(); Steps.clear(); Nudges.clear(); }
};

struct MoverResult {
    glm::vec3 Feet{0.0f};
    glm::vec3 Velocity{0.0f};  // the velocity asked for, less what ran into anything hit
    unsigned Flags = 0;        // MoverSides / MoverUp / MoverDown
    bool Grounded = false;
    MoverFloor Floor;
    float StepDelta = 0.0f;    // how far a step up or down (or a snap) moved the feet: the eye eases over it
    static constexpr int kMaxHits = 8;
    int HitCount = 0;
    MoverHit Hits[kMaxHits];   // every side and floor touch of the move (pushing bodies, riding platforms)
};

enum MoverFlag : unsigned { MoverSides = 1, MoverUp = 2, MoverDown = 4 }; // as PhysicsWorld's CC_ flags

namespace CharacterMover {

// One frame's move of `delta` from `feet`. `velocity` is what asked for it (it comes back clipped). `wasGrounded`: on the
// floor at the start; `snapToGround`: follow the floor down (walking, not jumping or falling).
MoverResult Move(IMoverWorld& world, const MoverCapsule& cap, const glm::vec3& feet, const glm::vec3& velocity,
                 const glm::vec3& delta, const MoverSettings& settings, bool wasGrounded, bool snapToGround,
                 MoverDebug* debug = nullptr);

// `v` slid along every plane it runs into: one plane takes off the part into it, two the part off their crease, three
// stop it. Never turns `v` back on itself.
glm::vec3 ClipToPlanes(const glm::vec3& v, const glm::vec3* normals, int count);

// What's under the capsule at `feet`, looked for `probe` down.
MoverFloor FindFloor(IMoverWorld& world, const MoverCapsule& cap, const glm::vec3& feet, float probe, const MoverSettings& settings);

} // namespace CharacterMover
