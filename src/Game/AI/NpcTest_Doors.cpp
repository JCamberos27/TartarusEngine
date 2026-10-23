// --npc-test doors: the player walked through every door of Apartment Hallway, all of them open - straight, grazing either
// jamb, strafing and sprinting on a diagonal - and measured on each pass: how long it took, how much of the asked-for speed
// survived within half a metre of the wall, how long it was stuck, and whether the capsule ever ended up inside anything.
#include "NpcTest.h"
#include "Components.h"
#include "Openable.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr float kDoorWallZ = 0.032f;  // the frame's wall centre, door-local +Z of the leaf's origin (build_apartment_hallway.py)
constexpr float kRun = 1.2f;          // each pass starts this far before the wall and ends this far past it
constexpr float kSettle = 0.2f;       // seconds stood still at the start of a pass
constexpr float kNear = 0.5f;         // the speed is judged within this of the wall plane
const char* const kPassNames[] = {"straight", "graze hinge", "graze latch", "strafe", "sprint diagonal"};
constexpr int kPasses = 5;

glm::vec3 Flat(glm::vec3 v) {
    v.y = 0.0f;
    const float l = glm::length(v);
    return l > 1e-6f ? v / l : glm::vec3(0.0f);
}
float YawOf(const glm::vec3& d) { return glm::degrees(std::atan2(d.z, d.x)); }
glm::vec3 Turn(const glm::vec3& d, float deg) { // about +Y, the same sense as yaw (toward +Z from +X)
    const float a = glm::radians(deg), c = std::cos(a), s = std::sin(a);
    return {d.x * c - d.z * s, 0.0f, d.x * s + d.z * c};
}
} // namespace

void NpcTest::DoorsSetUp(World& world) {
    auto view = world.Registry.view<OpenableComponent, TransformComponent>();
    for (auto e : view) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        const auto* pn = h && h->Parent != entt::null ? world.Registry.try_get<NameComponent>(h->Parent) : nullptr;
        if (!pn || pn->Name.size() < 4 || pn->Name.compare(pn->Name.size() - 4, 4, "Door") != 0) continue;
        const glm::mat4 m = world.GetCachedWorldTransform(e); // shut: Play hasn't opened anything yet
        DoorSpot d;
        d.Name = pn->Name;
        d.Normal = Flat(glm::vec3(m[2]));
        d.Along = Flat(glm::vec3(m[0])); // toward the latch side (a left hinge's pin is at local -X)
        d.Centre = glm::vec3(m[3]) + d.Normal * kDoorWallZ;
        d.Centre.y = 0.0f;
        d.Leaf = (std::uint32_t)entt::to_integral(e);
        m_DDoors.push_back(d);
    }
    std::sort(m_DDoors.begin(), m_DDoors.end(), [](const DoorSpot& a, const DoorSpot& b) { return a.Name < b.Name; });
    Openable::SetAll(world, 1.0f);
#pragma warning(suppress : 4996)
    const char* trace = std::getenv("NPC_TEST_DOORS_TRACE");
    m_DTrace = trace && *trace && *trace != '0';
    std::printf("[NpcTest] doors: %zu doors, all opened\n", m_DDoors.size());
}

// The pass `m_DPass` of door `m_DDoor`: where it starts, which way it heads, and how the keys are held.
void NpcTest::DoorsPassSetUp(const Player& player) {
    const DoorSpot& d = m_DDoors[(size_t)m_DDoor];
    const int k = m_DPass;
    // Alternate sides: even passes go along the door's +Z, odd ones back.
    const float side = (k % 2 == 0) ? 1.0f : -1.0f;
    const glm::vec3 n = d.Normal * side;
    DoorRun& r = m_DRun;
    r = DoorRun{};
    r.Dir = n;
    r.Sprint = k == 4;
    r.Strafe = k == 3;
    glm::vec3 heading = n;
    glm::vec3 start = d.Centre - n * kRun;
    if (k == 1 || k == 2) {
        // A graze: arrive at the wall with the capsule's edge 8 cm over the jamb, heading 10 degrees in toward the middle.
        const float jamb = k == 1 ? -1.0f : 1.0f; // the hinge side is local -X
        const float half = 0.46f, lateral = half - player.Size.x * 0.5f + 0.08f;
        const glm::vec3 out = d.Along * jamb;
        const float tilt = 10.0f;
        heading = glm::normalize(n - out * std::tan(glm::radians(tilt)));
        start = d.Centre + out * (lateral + kRun * std::tan(glm::radians(tilt))) - n * kRun;
    } else if (k == 4) {
        heading = Turn(n, 25.0f);
        start = d.Centre - heading * (kRun / std::cos(glm::radians(25.0f)));
    }
    r.Heading = heading;
    r.Start = start;
    r.Speed = player.MoveSpeed * (r.Sprint ? player.SprintMultiplier : 1.0f);
    r.Budget = 2.0f * (2.0f * kRun / std::max(r.Speed, 0.1f)) + 1.0f;
    r.MinRatio = 1e9f;
}

void NpcTest::DoorsDrive(Player& player, float dt, float& wantYaw, float& wantPitch) {
    player.ScriptMove = glm::vec2(0.0f);
    wantPitch = -3.0f;
    wantYaw = player.Cam.Yaw;
    m_DDt = dt;
    // The shut checks: stood still in the middle of the opening, facing through it.
    if (m_DShut >= 0 && m_DShut < (int)m_DDoors.size()) {
        const DoorSpot& d = m_DDoors[(size_t)m_DShut];
        if (!m_DShutPlaced) {
            const glm::vec3 feet = d.Centre + glm::vec3(0.0f, 0.02f, 0.0f);
            PhysicsWorld::SetCharacterFootPosition(&feet.x);
            player.Velocity = glm::vec3(0.0f);
            player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
            m_DShutPlaced = true;
        }
        wantYaw = player.Cam.Yaw = YawOf(d.Normal);
        return;
    }
    if (!m_DReady || m_DDone || m_DDoor >= (int)m_DDoors.size()) return;
    DoorRun& r = m_DRun;
    if (!r.Placed) {
        const glm::vec3 feet = r.Start + glm::vec3(0.0f, 0.02f, 0.0f);
        PhysicsWorld::SetCharacterFootPosition(&feet.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        r.Placed = true;
        r.Clock = 0.0f;
    }
    // The view: along the heading (a strafe looks along the wall, the move key held sideways).
    const glm::vec3 face = r.Strafe ? Turn(r.Heading, -90.0f) : r.Heading;
    wantYaw = YawOf(face);
    player.Cam.Yaw = wantYaw;
    player.ScriptSprint = r.Sprint;
    if (r.Clock < kSettle) return;
    if (r.Strafe) {
        const float yaw = glm::radians(wantYaw);
        const glm::vec3 right(-std::sin(yaw), 0.0f, std::cos(yaw));
        player.ScriptMove = glm::vec2(glm::dot(right, r.Heading) >= 0.0f ? 1.0f : -1.0f, 0.0f);
    } else {
        player.ScriptMove = glm::vec2(0.0f, 1.0f);
    }
}

void NpcTest::Doors(World& world, const Player& player) {
    if (!m_DReady) {
        if (m_DDoors.empty()) DoorsSetUp(world);
        m_DWait += m_DDt;
        if (m_DWait < 1.0f) return; // the leaves swinging open
        m_DReady = true;
        if (m_DDoors.empty()) {
            Check(false, "doors: Apartment Hallway's doors found");
            m_Done = true;
            return;
        }
        DoorsPassSetUp(player);
        return;
    }
    if (m_DShut >= 0) {
        DoorsShut(world);
        return;
    }
    if (m_DDone) return;
    DoorRun& r = m_DRun;
    const DoorSpot& d = m_DDoors[(size_t)m_DDoor];
    glm::vec3 feet;
    PhysicsWorld::GetCharacterFootPosition(&feet.x);
    const float dt = m_DDt;
    r.Clock += dt;
    if (r.Clock >= kSettle && r.HavePrev && dt > 0.0f) {
        r.Time += dt;
        const float along = glm::dot(feet - d.Centre, r.Dir);
        const glm::vec3 v = (feet - r.Prev) / dt;
        const float speed = glm::length(glm::vec2(v.x, v.z)), ratio = speed / std::max(r.Speed, 0.01f);
        // Judged once up to speed (past the accel ramp) and near the wall.
        if (std::abs(along) <= kNear) r.MinRatio = std::min(r.MinRatio, ratio);
        if (m_DTrace) // NPC_TEST_DOORS_TRACE=1: every frame of every pass
            std::printf("[NpcTest]   t %.3f along %+.3f lateral %+.3f height %.3f speed %.2f (%.2f)\n", r.Time, along,
                        glm::dot(feet - d.Centre, d.Along), feet.y, speed, ratio);
        if (r.Time > 0.25f && ratio < 0.25f) r.Stuck += dt;
        float pos[3], rad = 0.0f, cyl = 0.0f;
        if (PhysicsWorld::GetCharacterCapsule(pos, &rad, &cyl) && !PhysicsWorld::CharacterFitsAt(cyl)) ++r.PenFrames;
        const bool through = along >= kRun - 0.3f;
        if (through || r.Time > r.Budget) {
            if (r.MinRatio > 1e8f) r.MinRatio = 0.0f;
            const char* name = kPassNames[m_DPass];
            const bool graze = m_DPass == 1 || m_DPass == 2;
            const float need = graze ? 0.5f : 0.85f;
            const bool ok = through && r.MinRatio >= need && r.Stuck <= 0.15f && r.PenFrames == 0;
            std::printf("[NpcTest] door %s %s: %s, time %.2f s, minSpeed %.2f, stuck %.2f s, penFrames %d\n", d.Name.c_str(), name,
                        through ? "through" : "NOT THROUGH", r.Time, r.MinRatio, r.Stuck, r.PenFrames);
            ++m_DTotal;
            if (ok) ++m_DOk;
            if (r.MinRatio < m_DWorst) {
                m_DWorst = r.MinRatio;
                m_DWorstAt = d.Name + "/" + name;
            }
            m_DWorstStuck = std::max(m_DWorstStuck, r.Stuck);
            m_DPen += r.PenFrames;
            if (!through) m_DFailedThrough.push_back(d.Name + "/" + name);
            if (++m_DPass >= kPasses) {
                m_DPass = 0;
                ++m_DDoor;
            }
            if (m_DDoor >= (int)m_DDoors.size()) {
                m_DDone = true;
                std::printf("[NpcTest] doors: %d/%d passes ok, worst minSpeed %.2f at %s, worst stuck %.2f s, penFrames %d\n", m_DOk,
                            m_DTotal, m_DWorst, m_DWorstAt.c_str(), m_DWorstStuck, m_DPen);
                Check(m_DFailedThrough.empty(), "doors: every pass gets through (" + std::to_string(m_DFailedThrough.size()) + " didn't)");
                Check(m_DOk == m_DTotal, "doors: every pass keeps its speed, never stuck or inside a wall (" + std::to_string(m_DOk) + "/" +
                                             std::to_string(m_DTotal) + ")");
                m_DShut = 0; // then a door shut on him, a few times
                return;
            }
            DoorsPassSetUp(player);
            return;
        }
    }
    r.Prev = feet;
    r.HavePrev = true;
}

// A door shut on the player standing in its opening: it stops short of him (not latched) and never swings into him.
void NpcTest::DoorsShut(World& world) {
    constexpr int kShutDoors = 3;
    constexpr float kShutWait = 1.5f; // two Open Times
    if (!m_DShutPlaced) return;
    const DoorSpot& d = m_DDoors[(size_t)m_DShut];
    const auto leaf = static_cast<entt::entity>(d.Leaf);
    auto* o = world.Registry.valid(leaf) ? world.Registry.try_get<OpenableComponent>(leaf) : nullptr;
    if (!o) {
        Check(false, "doors: " + d.Name + " has its leaf");
        m_Done = true;
        return;
    }
    if (m_DShutClock == 0.0f) o->Target = 0.0f;
    m_DShutClock += m_DDt;
    float pos[3], rad = 0.0f, cyl = 0.0f;
    if (m_DShutClock > 0.1f && PhysicsWorld::GetCharacterCapsule(pos, &rad, &cyl) && !PhysicsWorld::CharacterFitsAt(cyl)) ++m_DShutPen;
    if (m_DShutClock < kShutWait) return;
    const bool stopped = o->Progress > 0.02f;
    std::printf("[NpcTest] door %s shut on the player: %s at %.2f open\n", d.Name.c_str(), stopped ? "stopped" : "SHUT", o->Progress);
    if (stopped) ++m_DShutOk;
    o->Target = 1.0f;
    m_DShutClock = 0.0f;
    m_DShutPlaced = false;
    if (++m_DShut < std::min(kShutDoors, (int)m_DDoors.size())) return;
    Check(m_DShutOk == std::min(kShutDoors, (int)m_DDoors.size()),
          "doors: a door shut on the player stops short of him (" + std::to_string(m_DShutOk) + "/" + std::to_string(kShutDoors) + ")");
    Check(m_DShutPen == 0, "doors: ... and never swings into him (" + std::to_string(m_DShutPen) + " frames inside)");
    m_Done = true;
}
