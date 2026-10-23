// The characters' collide-and-slide (CharacterMover) against real PhysX worlds of box colliders: the player's capsule
// (the custom mover switched on for the test) walked into walls, corners, doorways, steps under low headers, slopes and
// ledges, frame by frame at 60 Hz, as Player::Update moves it.
#include "UnitTestSupport.h"
#include "../Physics/CharacterMover.h"
#include "../Physics/PhysicsWorld.h"
#include "../Core/ProjectSettings.h"
#include "../Game/Components.h"
#include "../Game/World.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kRadius = 0.22f, kHeight = 1.85f; // Apartment Hallway's player

// A world of static boxes (centre, half extents, yaw in degrees), the player's capsule on the custom mover at `feet`.
struct MoverWorld {
    World world;
    bool Ok = false;
    bool WasMover = false;
    explicit MoverWorld(const std::function<void(MoverWorld&)>& build, const glm::vec3& feet) {
        WasMover = ProjectSettings::Physics().UseCustomCharacterMover;
        ProjectSettings::MutablePhysics().UseCustomCharacterMover = true;
        build(*this);
        world.SyncActiveInHierarchy();
        world.RebuildWorldTransformCache();
        PhysicsWorld::Create(world);
        Ok = PhysicsWorld::IsActive();
        if (!Ok) return;
        PhysicsWorld::CreateCharacter(kRadius, kHeight * 0.5f - kRadius, &feet.x);
        Ok = PhysicsWorld::HasCharacter();
    }
    ~MoverWorld() {
        PhysicsWorld::Destroy();
        ProjectSettings::MutablePhysics().UseCustomCharacterMover = WasMover;
    }
    void Box(const glm::vec3& centre, const glm::vec3& half, float yawDeg = 0.0f) {
        const entt::entity e = world.CreateEmptyEntity(centre, glm::vec3(0.0f, yawDeg, 0.0f), glm::vec3(1.0f), "Box");
        ColliderComponent c;
        c.Kind = ColliderComponent::Shape::Box;
        c.HalfExtents = half;
        world.Registry.emplace<ColliderComponent>(e, c);
    }
    void Floor(float top = 0.0f) { Box(glm::vec3(0.0f, top - 0.5f, 0.0f), glm::vec3(30.0f, 0.5f, 30.0f)); }
    glm::vec3 Feet() const { glm::vec3 f; PhysicsWorld::GetCharacterFootPosition(&f.x); return f; }
    // One frame of Player::Update's move: velocity (with gravity) in, what's left of it back.
    struct Frame { glm::vec3 Velocity; bool Grounded; unsigned Flags; float StepDelta; };
    Frame Step(glm::vec3 velocity, bool grounded) {
        velocity.y += -18.0f * kDt;
        const glm::vec3 d = velocity * kDt;
        Frame f{};
        f.Flags = PhysicsWorld::MoveCharacter(&d.x, kDt, grounded && velocity.y <= 0.0f);
        float v[3];
        PhysicsWorld::LastMoverResult(v, &f.Grounded, &f.StepDelta);
        f.Velocity = glm::vec3(v[0], v[1], v[2]);
        if (f.Grounded && f.Velocity.y < 0.0f) f.Velocity.y = 0.0f;
        if ((f.Flags & PhysicsWorld::CC_UP) && f.Velocity.y > 0.0f) f.Velocity.y = 0.0f;
        return f;
    }
    // Walks at `speed` along `dir` (re-asked every frame, as a held key) for `seconds`; calls `each` after every frame.
    void Walk(const glm::vec3& dir, float speed, float seconds, const std::function<void(const Frame&)>& each = {}) {
        bool grounded = true;
        glm::vec3 v(0.0f);
        for (int i = 0, n = (int)std::lround(seconds / kDt); i < n; ++i) {
            const glm::vec3 want = glm::normalize(glm::vec3(dir.x, 0.0f, dir.z)) * speed;
            v.x = want.x;
            v.z = want.z;
            const Frame f = Step(v, grounded);
            v.y = f.Velocity.y;
            grounded = f.Grounded;
            if (each) each(f);
#pragma warning(suppress : 4996)
            if (std::getenv("MOVER_TRACE")) { const glm::vec3 p = Feet(); std::printf("[MoverTrace] %d feet %.3f %.3f %.3f v %.2f %.2f %.2f g %d flags %u step %.3f\n", i, p.x, p.y, p.z, f.Velocity.x, f.Velocity.y, f.Velocity.z, (int)f.Grounded, f.Flags, f.StepDelta); }
        }
    }
};

void TestMoverClipToPlanes() {
    const glm::vec3 wall(-1.0f, 0.0f, 0.0f);
    CHECK(glm::length(CharacterMover::ClipToPlanes(glm::vec3(2.0f, 0.0f, 3.0f), &wall, 1) - glm::vec3(0.0f, 0.0f, 3.0f)) < 1e-5f);
    CHECK(CharacterMover::ClipToPlanes(glm::vec3(-2.0f, 0.0f, 3.0f), &wall, 1) == glm::vec3(-2.0f, 0.0f, 3.0f));
    const glm::vec3 corner[2] = {glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    CHECK(glm::length(CharacterMover::ClipToPlanes(glm::vec3(1.0f, 0.0f, 1.0f), corner, 2)) < 1e-5f);
    // An obtuse corner: along the crease.
    const glm::vec3 open[2] = {glm::normalize(glm::vec3(-1.0f, 0.0f, 0.2f)), glm::normalize(glm::vec3(-1.0f, 0.0f, -0.2f))};
    const glm::vec3 up = CharacterMover::ClipToPlanes(glm::vec3(1.0f, 1.0f, 0.0f), open, 2);
    CHECK(up.x <= 1e-4f && up.y > 0.9f);
    const glm::vec3 three[3] = {glm::vec3(-1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0)};
    CHECK(glm::length(CharacterMover::ClipToPlanes(glm::vec3(1.0f, 1.0f, 1.0f), three, 3)) < 1e-5f);
}

// Along a wall at 30 degrees: the slide keeps nearly all its speed along the wall; into a corner: stopped, not jittering.
void TestMoverWallAndCorner() {
    MoverWorld w([](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(2.0f, 1.5f, 0.0f), glm::vec3(0.1f, 1.5f, 10.0f));  // a wall at x = 1.9
        m.Box(glm::vec3(0.0f, 1.5f, 3.0f), glm::vec3(10.0f, 1.5f, 0.1f));  // and one at z = 2.9: a corner
    }, glm::vec3(1.0f, 0.02f, -3.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    const glm::vec3 dir = glm::normalize(glm::vec3(std::sin(glm::radians(30.0f)), 0.0f, std::cos(glm::radians(30.0f))));
    float alongWall = 1e9f;
    w.Walk(dir, 4.0f, 0.6f, [&](const MoverWorld::Frame& f) { if (f.Flags & PhysicsWorld::CC_SIDES) alongWall = std::min(alongWall, f.Velocity.z); });
    CHECK(alongWall > 4.0f * std::cos(glm::radians(30.0f)) * 0.95f);
    CHECK(w.Feet().x < 1.9f - kRadius + 0.01f); // never in the wall
    // On into the corner, then held there: no creep, no jitter.
    w.Walk(glm::vec3(1.0f, 0.0f, 1.0f), 4.0f, 2.5f);
    const glm::vec3 settled = w.Feet();
    float drift = 0.0f;
    w.Walk(glm::vec3(1.0f, 0.0f, 1.0f), 4.0f, 0.5f, [&](const MoverWorld::Frame&) { drift = std::max(drift, glm::length(w.Feet() - settled)); });
    CHECK(drift < 0.002f);
    CHECK(settled.x < 1.9f - kRadius + 0.01f && settled.z < 2.9f - kRadius + 0.01f);
}

// A 0.92 m doorway (Apartment Hallway's) in a 15 cm wall under a 2.06 m header: straight through at full speed; at 10
// degrees with the capsule's edge over the jamb, past the jamb keeping most of the speed.
void TestMoverDoorway() {
    auto build = [](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(-2.96f, 1.4f, 0.0f), glm::vec3(2.5f, 1.4f, 0.075f));  // the wall left of the opening (ends at x = -0.46)
        m.Box(glm::vec3(2.96f, 1.4f, 0.0f), glm::vec3(2.5f, 1.4f, 0.075f));   // ... right of it (from x = 0.46)
        m.Box(glm::vec3(0.0f, 2.43f, 0.0f), glm::vec3(0.46f, 0.37f, 0.075f)); // the header, from y = 2.06
    };
    {
        MoverWorld w(build, glm::vec3(0.0f, 0.02f, -1.5f));
        CHECK(w.Ok);
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 4.0f, 0.8f);
        CHECK(w.Feet().z > 1.5f); // through, never slowed: 3.2 m in 0.8 s at 4 m/s
    }
    {
        // Edge 8 cm over the left jamb at the wall, heading 10 degrees in.
        const float tilt = glm::radians(10.0f), lateral = -(0.46f - kRadius + 0.08f);
        const glm::vec3 dir(std::sin(tilt), 0.0f, std::cos(tilt));
        const glm::vec3 start(lateral - 1.5f * std::tan(tilt), 0.02f, -1.5f);
        MoverWorld w(build, start);
        if (!w.Ok) return;
        glm::vec3 prev = w.Feet();
        float slowest = 1e9f;
        w.Walk(dir, 4.0f, 1.0f, [&](const MoverWorld::Frame&) {
            const glm::vec3 f = w.Feet();
            if (std::abs(f.z) < 0.5f) slowest = std::min(slowest, glm::length(glm::vec2(f.x - prev.x, f.z - prev.z)) / kDt / 4.0f);
            prev = f;
        });
        CHECK(w.Feet().z > 1.0f);
        CHECK(slowest > 0.6f);
    }
}

// The doorway assist (the player's DoorwayAssist): the same jamb graze as above keeps more of its speed with it on; a wall
// met head on, or slid along at 20 degrees, is the same with it as without.
void TestMoverDoorwayAssist() {
    auto doorway = [](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(-2.96f, 1.4f, 0.0f), glm::vec3(2.5f, 1.4f, 0.075f));
        m.Box(glm::vec3(2.96f, 1.4f, 0.0f), glm::vec3(2.5f, 1.4f, 0.075f));
        m.Box(glm::vec3(0.0f, 2.43f, 0.0f), glm::vec3(0.46f, 0.37f, 0.075f));
    };
    auto graze = [&](float assist) {
        const float tilt = glm::radians(10.0f), lateral = -(0.46f - kRadius + 0.08f);
        const glm::vec3 dir(std::sin(tilt), 0.0f, std::cos(tilt));
        MoverWorld w(doorway, glm::vec3(lateral - 1.5f * std::tan(tilt), 0.02f, -1.5f));
        if (!w.Ok) return -1.0f;
        PhysicsWorld::SetCharacterDoorwayAssist(assist, 35.0f);
        glm::vec3 prev = w.Feet();
        float slowest = 1e9f;
        w.Walk(dir, 4.0f, 1.0f, [&](const MoverWorld::Frame&) {
            const glm::vec3 f = w.Feet();
            if (std::abs(f.z) < 0.5f) slowest = std::min(slowest, glm::length(glm::vec2(f.x - prev.x, f.z - prev.z)) / kDt / 4.0f);
            prev = f;
        });
        return w.Feet().z > 1.0f ? slowest : 0.0f;
    };
    const float off = graze(0.0f), on = graze(0.5f);
    std::printf("[UnitTest] doorway graze: slowest %.2f without the assist, %.2f with it\n", off, on);
    CHECK(on >= off - 0.01f);
    CHECK(on >= 0.75f);

    // Into a flat wall head on: stopped, not nudged sideways. Along one at 20 degrees: the same path either way.
    auto wall = [](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(0.0f, 1.5f, 1.0f), glm::vec3(10.0f, 1.5f, 0.1f)); // z = 0.9
    };
    auto walkWall = [&](float assist, const glm::vec3& dir) {
        MoverWorld w(wall, glm::vec3(0.0f, 0.02f, 0.0f));
        if (!w.Ok) return glm::vec3(1e9f);
        PhysicsWorld::SetCharacterDoorwayAssist(assist, 35.0f);
        w.Walk(dir, 4.0f, 0.6f);
        return w.Feet();
    };
    CHECK(std::abs(walkWall(0.5f, glm::vec3(0, 0, 1)).x) < 1e-3f);
    const glm::vec3 slant(std::sin(glm::radians(70.0f)), 0.0f, std::cos(glm::radians(70.0f))); // 20 degrees off the wall
    CHECK(glm::length(walkWall(0.5f, slant) - walkWall(0.0f, slant)) < 1e-3f);
}

// A 25 cm step under a ceiling 6 cm over the capsule's top once stepped: climbed (the step only as tall as the room);
// with the ceiling lower than that, not climbed and not pushed into.
void TestMoverStepUnderLowCeiling() {
    auto build = [](float ceilingBottom) {
        return [ceilingBottom](MoverWorld& m) {
            m.Floor();
            m.Box(glm::vec3(0.0f, 0.125f, 3.0f), glm::vec3(3.0f, 0.125f, 2.0f));                     // the step, from z = 1
            m.Box(glm::vec3(0.0f, ceilingBottom + 0.5f, 0.0f), glm::vec3(3.0f, 0.5f, 6.0f));         // the ceiling
        };
    };
    {
        MoverWorld w(build(kHeight + 0.25f + 0.06f), glm::vec3(0.0f, 0.02f, 0.0f));
        CHECK(w.Ok);
        if (!w.Ok) return;
        float stepped = 0.0f;
        w.Walk(glm::vec3(0, 0, 1), 3.0f, 1.0f, [&](const MoverWorld::Frame& f) { stepped += f.StepDelta; });
        CHECK(w.Feet().z > 1.5f && w.Feet().y > 0.2f);
        CHECK(stepped > 0.2f); // reported, for the eye to ease over
    }
    {
        MoverWorld w(build(kHeight + 0.1f), glm::vec3(0.0f, 0.02f, 0.0f));
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 3.0f, 1.0f);
        CHECK(w.Feet().z < 1.0f - kRadius + 0.01f && w.Feet().y < 0.05f);
        CHECK(PhysicsWorld::CharacterFitsAt(kHeight * 0.5f - kRadius));
    }
}

// Stairs of 18 cm risers climbed near full speed; a 40 cm box isn't.
void TestMoverStairsAndBlock() {
    {
        MoverWorld w([](MoverWorld& m) {
            m.Floor();
            for (int i = 0; i < 5; ++i) m.Box(glm::vec3(0.0f, 0.09f * (i + 1), 1.0f + 0.3f * i + 1.5f), glm::vec3(1.0f, 0.09f * (i + 1), 1.5f));
        }, glm::vec3(0.0f, 0.02f, 0.0f));
        CHECK(w.Ok);
        if (!w.Ok) return;
        float slowest = 1e9f;
        glm::vec3 prev = w.Feet();
        w.Walk(glm::vec3(0, 0, 1), 3.0f, 1.0f, [&](const MoverWorld::Frame&) {
            const glm::vec3 f = w.Feet();
            if (f.z > 0.5f && f.z < 2.0f) slowest = std::min(slowest, (f.z - prev.z) / kDt / 3.0f);
            prev = f;
        });
        CHECK(w.Feet().y > 0.85f && w.Feet().z > 2.3f); // up all five
        CHECK(slowest > 0.8f);                          // ... without slowing
    }
    MoverWorld b([](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(0.0f, 0.2f, 1.5f), glm::vec3(1.0f, 0.2f, 0.5f));
    }, glm::vec3(0.0f, 0.02f, 0.0f));
    if (!b.Ok) return;
    b.Walk(glm::vec3(0, 0, 1), 3.0f, 1.0f);
    CHECK(b.Feet().y < 0.05f && b.Feet().z < 1.0f - kRadius + 0.01f);
}

// A 40 degree ramp walked up; a 60 degree one not.
void TestMoverSlopes() {
    auto ramp = [](float deg) {
        return [deg](MoverWorld& m) {
            m.Floor();
            // A slab tilted about X, its top surface rising along +Z from z = 1.
            const float a = glm::radians(deg), len = 4.0f;
            const glm::vec3 centre(0.0f, std::sin(a) * len * 0.5f - std::cos(a) * 0.25f, 1.0f + std::cos(a) * len * 0.5f + std::sin(a) * 0.25f);
            const entt::entity e = m.world.CreateEmptyEntity(centre, glm::vec3(-deg, 0.0f, 0.0f), glm::vec3(1.0f), "Ramp");
            ColliderComponent c;
            c.Kind = ColliderComponent::Shape::Box;
            c.HalfExtents = glm::vec3(2.0f, 0.25f, len * 0.5f);
            m.world.Registry.emplace<ColliderComponent>(e, c);
        };
    };
    {
        MoverWorld w(ramp(40.0f), glm::vec3(0.0f, 0.02f, 0.0f));
        CHECK(w.Ok);
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 3.0f, 1.2f);
        CHECK(w.Feet().y > 1.0f);
    }
    {
        MoverWorld w(ramp(60.0f), glm::vec3(0.0f, 0.02f, 0.0f));
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 3.0f, 1.5f);
        CHECK(w.Feet().y < 0.45f); // no higher than a step
    }
}

// On a ledge: the axis 0.3 radius past the edge, it stands; 0.8 radius past, it slides off and falls.
void TestMoverLedge() {
    auto build = [](MoverWorld& m) { m.Box(glm::vec3(0.0f, 0.5f, -5.0f), glm::vec3(5.0f, 0.5f, 5.0f)); }; // a block to z = 0, top y = 1
    {
        MoverWorld w(build, glm::vec3(0.0f, 1.02f, 0.3f * kRadius));
        CHECK(w.Ok);
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 0.0f, 0.5f);
        CHECK(w.Feet().y > 0.95f);
    }
    {
        MoverWorld w(build, glm::vec3(0.0f, 1.02f, 0.8f * kRadius));
        if (!w.Ok) return;
        w.Walk(glm::vec3(0, 0, 1), 0.0f, 0.8f);
        CHECK(w.Feet().y < 0.5f);
    }
}

// Started 10 cm inside a wall: out of it on the first move.
void TestMoverDepenetrates() {
    MoverWorld w([](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(1.0f, 1.5f, 0.0f), glm::vec3(0.5f, 1.5f, 5.0f)); // its face at x = 0.5
    }, glm::vec3(0.5f - kRadius + 0.1f, 0.02f, 0.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    w.Walk(glm::vec3(0, 0, 1), 0.0f, kDt);
    CHECK(w.Feet().x <= 0.5f - kRadius + 0.005f);
    CHECK(PhysicsWorld::CharacterFitsAt(kHeight * 0.5f - kRadius));
}

// A jump into a ceiling: the up flag, and no more rising.
void TestMoverCeiling() {
    MoverWorld w([](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(0.0f, kHeight + 0.3f + 0.5f, 0.0f), glm::vec3(3.0f, 0.5f, 3.0f)); // 30 cm over the head
    }, glm::vec3(0.0f, 0.02f, 0.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    glm::vec3 v(0.0f, 5.5f, 0.0f);
    bool hit = false;
    for (int i = 0; i < 20 && !hit; ++i) {
        const MoverWorld::Frame f = w.Step(v, false);
        v = f.Velocity;
        hit = (f.Flags & PhysicsWorld::CC_UP) != 0;
        if (hit) CHECK(v.y <= 0.0f);
    }
    CHECK(hit);
    CHECK(w.Feet().y < 0.3f + 0.01f);
}

constexpr float kNpcRadius = 0.28f, kNpcStandCyl = 0.6f, kNpcCrouchCyl = 0.33f; // NpcDirector's soldier
constexpr unsigned kNpcEntity = 777u;

glm::vec3 NpcFeet(PhysicsWorld::CharacterId id) {
    glm::vec3 f(0.0f);
    PhysicsWorld::GetNpcCapsule(id, &f.x, nullptr, nullptr);
    return f;
}

// A soldier's capsule on the mover: made, walked into a wall (stopped by it, the wall's side kept), crouched and stood up
// again (only where there's room), found by his entity, and released.
void TestMoverNpcBody() {
    MoverWorld w([](MoverWorld& m) {
        m.Floor();
        m.Box(glm::vec3(0.0f, 1.5f, 3.0f), glm::vec3(5.0f, 1.5f, 0.1f));            // a wall at z = 2.9
        m.Box(glm::vec3(4.0f, 1.3f + 0.5f, 0.0f), glm::vec3(0.6f, 0.5f, 0.6f));     // a table top at 1.3 m, over x = 4
    }, glm::vec3(-6.0f, 0.02f, 0.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    const glm::vec3 start(0.0f, 0.02f, 0.0f);
    const PhysicsWorld::CharacterId id = PhysicsWorld::CreateNpcCharacter(kNpcEntity, kNpcRadius, kNpcStandCyl, &start.x);
    CHECK(id != PhysicsWorld::kNoCharacter);
    if (id == PhysicsWorld::kNoCharacter) return;
    CHECK(PhysicsWorld::NpcCharacterOf(kNpcEntity) == id);
    float feet[3], r = 0.0f, cyl = 0.0f;
    CHECK(PhysicsWorld::GetNpcCapsule(id, feet, &r, &cyl) && std::fabs(r - kNpcRadius) < 1e-5f && std::fabs(cyl - kNpcStandCyl) < 1e-5f);

    // Into the wall at 3.2 m/s (NpcDirector's gravity included): stopped a skin short of it, never inside.
    float fall = -1.0f;
    for (int i = 0; i < 90; ++i) {
        fall -= 18.0f * kDt;
        const glm::vec3 d = glm::vec3(0.0f, fall, 3.2f) * kDt;
        if (PhysicsWorld::MoveNpcCharacter(id, &d.x, kDt) & PhysicsWorld::CC_DOWN) fall = -1.0f;
    }
    const glm::vec3 f = NpcFeet(id);
    CHECK(f.z < 2.9f - kNpcRadius + 0.005f && f.z > 2.9f - kNpcRadius - 0.05f);
    CHECK(std::fabs(f.y) < 0.03f);

    // Crouch, then stand: room in the open; none under the table.
    CHECK(PhysicsWorld::ResizeNpcCharacter(id, kNpcCrouchCyl));
    CHECK(std::fabs(NpcFeet(id).y - f.y) < 1e-4f);
    CHECK(PhysicsWorld::NpcFitsAt(id, kNpcStandCyl));
    const glm::vec3 underTable(4.0f, 0.0f, 0.0f);
    PhysicsWorld::SetNpcFootPosition(id, &underTable.x);
    CHECK(!PhysicsWorld::NpcFitsAt(id, kNpcStandCyl));
    CHECK(PhysicsWorld::NpcFitsAt(id, kNpcCrouchCyl));

    PhysicsWorld::DestroyNpcCharacter(id);
    CHECK(PhysicsWorld::NpcCharacterOf(kNpcEntity) == PhysicsWorld::kNoCharacter);
    CHECK(!PhysicsWorld::GetNpcCapsule(id, feet, nullptr, nullptr));
}

// D8: nobody stands on anybody. Dropped onto a soldier's head (a little off his axis), the player slides off to the floor
// and is never grounded up there.
void TestMoverNoStandingOnCharacters() {
    MoverWorld w([](MoverWorld& m) { m.Floor(); }, glm::vec3(0.12f, 2.2f, 0.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    const glm::vec3 at(0.0f, 0.0f, 0.0f);
    const PhysicsWorld::CharacterId id = PhysicsWorld::CreateNpcCharacter(kNpcEntity, kNpcRadius, kNpcStandCyl, &at.x);
    CHECK(id != PhysicsWorld::kNoCharacter);
    if (id == PhysicsWorld::kNoCharacter) return;
    bool groundedHigh = false;
    glm::vec3 v(0.0f);
    bool grounded = false;
    for (int i = 0; i < 120; ++i) {
        const MoverWorld::Frame f = w.Step(v, grounded);
        v = glm::vec3(0.0f, f.Velocity.y, 0.0f);
        grounded = f.Grounded;
        if (grounded && w.Feet().y > 0.5f) groundedHigh = true;
    }
    CHECK(!groundedHigh);
    CHECK(w.Feet().y < 0.05f);
    const glm::vec3 p = w.Feet();
    CHECK(glm::length(glm::vec2(p.x, p.z)) > kNpcRadius + kRadius - 0.03f); // beside him, not inside
}

// A soldier's move is where everybody else's sweeps find him that same frame (no simulate in between here): the player
// walking into where he just stepped stops at his capsule.
void TestMoverNpcSeenSameFrame() {
    MoverWorld w([](MoverWorld& m) { m.Floor(); }, glm::vec3(0.0f, 0.02f, 0.0f));
    CHECK(w.Ok);
    if (!w.Ok) return;
    const glm::vec3 aside(3.0f, 0.02f, 1.5f);
    const PhysicsWorld::CharacterId id = PhysicsWorld::CreateNpcCharacter(kNpcEntity, kNpcRadius, kNpcStandCyl, &aside.x);
    CHECK(id != PhysicsWorld::kNoCharacter);
    if (id == PhysicsWorld::kNoCharacter) return;
    const glm::vec3 step(-3.0f, -0.01f, 0.0f); // into the player's path in one move
    PhysicsWorld::MoveNpcCharacter(id, &step.x, kDt);
    CHECK(std::fabs(NpcFeet(id).x) < 0.01f);
    w.Walk(glm::vec3(0.0f, 0.0f, 1.0f), 4.0f, 0.6f);
    CHECK(w.Feet().z < 1.5f - kNpcRadius - kRadius + 0.01f);
}

} // namespace

void RegisterCharacterMoverTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"MoverClipToPlanes", TestMoverClipToPlanes});
    tests.push_back({"MoverWallAndCorner", TestMoverWallAndCorner});
    tests.push_back({"MoverDoorway", TestMoverDoorway});
    tests.push_back({"MoverDoorwayAssist", TestMoverDoorwayAssist});
    tests.push_back({"MoverStepUnderLowCeiling", TestMoverStepUnderLowCeiling});
    tests.push_back({"MoverStairsAndBlock", TestMoverStairsAndBlock});
    tests.push_back({"MoverSlopes", TestMoverSlopes});
    tests.push_back({"MoverLedge", TestMoverLedge});
    tests.push_back({"MoverDepenetrates", TestMoverDepenetrates});
    tests.push_back({"MoverCeiling", TestMoverCeiling});
    tests.push_back({"MoverNpcBody", TestMoverNpcBody});
    tests.push_back({"MoverNoStandingOnCharacters", TestMoverNoStandingOnCharacters});
    tests.push_back({"MoverNpcSeenSameFrame", TestMoverNpcSeenSameFrame});
}
