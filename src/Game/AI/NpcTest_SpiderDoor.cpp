// --npc-test spiderdoor: a hunter behind 301's shut front door, the player out in the hallway. Woken by a burst, it comes
// through the entry, pushes the door open the way the player opens it (clear of the leaf's swing, which comes into the
// room toward it), goes through and reaches the player. The door stays open.
#include "NpcTest.h"

#include "Components.h"
#include "Creatures/SpiderSystem.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h"
#include "Openable.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <cmath>
#include <cstdio>

namespace {
constexpr float kDt = 1.0f / 60.0f;
constexpr float kShootAt = 2.0f; // the burst that wakes it
constexpr float kEnd = 40.0f;
} // namespace

void NpcTest::SpiderDoorDrive(Player& player, FirstPersonPresentation& weapon, float& wantYaw, float& wantPitch) {
    player.ScriptMove = glm::vec2(0.0f);
    wantPitch = 0.0f;
    if (!m_SDReady) return;
    if (!m_SDPlaced) {
        PhysicsWorld::SetCharacterFootPosition(&m_SDStand.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = m_SDStand + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        m_SDPlaced = true;
    }
    wantYaw = 180.0f; // down the hallway, not at the door
    m_Firing = m_SDClock >= kShootAt && m_SDClock < kShootAt + 1.0f;
    if (m_Firing && m_SDAmmoStart < 0) m_SDAmmoStart = weapon.Ammo();
    if (m_SDAmmoStart >= 0) m_SDAmmoSpent = std::max(m_SDAmmoSpent, m_SDAmmoStart - weapon.Ammo());
}

void NpcTest::SpiderDoor(World& world) {
    if (m_SDDone) return;
    m_SDClock += kDt;
    auto& reg = world.Registry;
    if (m_SDStage == 0) {
        m_SDStage = 1;
        // 301's front door: the openable under the "301 Front Door" group.
        for (auto e : reg.view<OpenableComponent, NameComponent>()) {
            const auto* h = reg.try_get<HierarchyComponent>(e);
            const auto* pn = h && h->Parent != entt::null ? reg.try_get<NameComponent>(h->Parent) : nullptr;
            if (pn && pn->Name == "301 Front Door") m_SDDoor = (std::uint32_t)e;
        }
        Check(m_SDDoor != 0xFFFFFFFFu, "spiderdoor: 301's front door is in the scene");
        return;
    }
    const entt::entity door = (entt::entity)m_SDDoor;
    if (m_SDStage == 1) { // the door has started (its first Play frame): read it, place the player and the hunter
        if (!reg.valid(door) || !reg.get<OpenableComponent>(door).Started) return;
        if (!Openable::Leaf(world, door, m_SDHinge, m_SDToLatch, m_SDWidth, m_SDSwing)) {
            Check(false, "spiderdoor: 301's front door reads as a hinged leaf");
            m_SDDone = m_Done = true;
            return;
        }
        m_SDStage = 2;
        const glm::vec3 mid = m_SDHinge + m_SDToLatch * (0.5f * m_SDWidth);
        m_SDShutAtStart = reg.get<OpenableComponent>(door).Progress == 0.0f;
        m_SDStand = glm::vec3(mid.x, 0.02f, mid.z) - m_SDSwing * 1.6f; // out in the hallway, facing the door's front
        const glm::vec3 drop = glm::vec3(mid.x, 1.0f, mid.z) + m_SDSwing * 3.8f, down(0.0f, -1.0f, 0.0f); // in the living room
        RaycastHit h;
        QueryFilter f;
        f.HitTriggers = 0;
        if (SpawnHunter && PhysicsWorld::RaycastSolid(&drop.x, &down.x, 3.0f, f, h) && h.Hit)
            m_SDRoot = SpawnHunter(glm::vec3(h.Point[0], h.Point[1], h.Point[2]), 0.0f);
        std::printf("[NpcTest] spiderdoor: door hinge %.2f %.2f %.2f, width %.2f, swings toward %.2f %.2f; player at %.2f %.2f, "
                    "hunter at %.2f %.2f %.2f\n", m_SDHinge.x, m_SDHinge.y, m_SDHinge.z, m_SDWidth, m_SDSwing.x, m_SDSwing.z, m_SDStand.x,
                    m_SDStand.z, h.Point[0], h.Point[1], h.Point[2]);
        Check(m_SDRoot != 0xFFFFFFFFu, "spiderdoor: a hunter in 301, behind its front door");
        m_SDReady = true;
        return;
    }

    const auto& o = reg.get<OpenableComponent>(door);
    SpiderSystem::Stats st;
    const entt::entity root = (entt::entity)m_SDRoot;
    if (Spiders && reg.valid(root) && Spiders->Inspect(root, st) && st.Ready) {
        const glm::vec3 mid = m_SDHinge + m_SDToLatch * (0.5f * m_SDWidth);
        // While the leaf swings it sweeps a quarter disc on its swing side: the spider is never in it.
        if (o.Progress > 0.0f && o.Progress < 1.0f && glm::dot(glm::vec3(st.Pos.x - mid.x, 0.0f, st.Pos.z - mid.z), m_SDSwing) > 0.0f &&
            glm::length(glm::vec2(st.Pos.x - m_SDHinge.x, st.Pos.z - m_SDHinge.z)) < m_SDWidth + 0.15f)
            ++m_SDSwept;
        if (o.Progress > 0.0f && m_SDOpenedAt < 0.0f) {
            m_SDOpenedAt = m_SDClock;
            std::printf("[NpcTest] spiderdoor: the door starts to open at %.1f s, the hunter at %.2f %.2f %.2f\n", m_SDClock, st.Pos.x,
                        st.Pos.y, st.Pos.z);
        }
        if (m_SDReachedAt < 0.0f && glm::length(st.Pos - m_SDStand) < 1.5f) m_SDReachedAt = m_SDClock;
        if (std::fabs(st.Pos.x) > 10.72f || std::fabs(st.Pos.z) > 9.795f || st.Pos.y > 2.75f || st.Pos.y < -0.15f) ++m_SDOutside;
        m_SDPushes = st.DoorsOpened;
        m_SDAttacks = st.Bites + st.Pounces + st.Drops;
        m_SDWorstStuck = std::max(m_SDWorstStuck, st.WorstStuck);
    }
    if (m_SDClock < kEnd) return;

    std::printf("[NpcTest] spiderdoor: %d rounds fired; door pushed %d time(s), opening at %.1f s, at %.2f open now; reached the "
                "player at %.1f s, %d attack(s); %d frames in the leaf's swing; longest stall %.1f s\n", m_SDAmmoSpent, m_SDPushes,
                m_SDOpenedAt, o.Progress, m_SDReachedAt, m_SDAttacks, m_SDSwept, m_SDWorstStuck);
    Check(m_SDShutAtStart, "spiderdoor: the door is shut when the hunter wakes");
    Check(m_SDAmmoSpent > 0, "spiderdoor: the player fires down the hallway");
    Check(m_SDPushes >= 1, "spiderdoor: the hunter pushes the shut door open");
    Check(m_SDSwept == 0, "spiderdoor: the swinging leaf never sweeps through the hunter (it pushes from clear of the swing)");
    Check(o.Progress >= 0.999f && o.Target > 0.5f, "spiderdoor: the door is left open");
    Check(m_SDReachedAt >= 0.0f, "spiderdoor: the hunter comes through the door to the player in the hallway");
    Check(m_SDAttacks > 0, "spiderdoor: and attacks them");
    Check(m_SDOutside == 0, "spiderdoor: the hunter never leaves the storey");
    Check(m_SDWorstStuck < 2.0f, "spiderdoor: it never stalls for 2 s while it means to move");
    SpiderJank("spiderdoor");
    m_SDDone = m_Done = true;
}
