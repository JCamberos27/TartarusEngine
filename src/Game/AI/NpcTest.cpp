#include "NpcTest.h"
#include "World.h"

#include "Combat/CombatFx.h"
#include "DevPanel.h"
#include "Combat/PlayerVitals.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h"
#include "NpcDirector.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "SceneSerializer.h"
#include "Components.h"
#include "Npc/NpcRagdoll.h"
#include "Npc/NpcHitboxes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
// The unit an aptwounds run in another room is in (NPC_TEST_ROOM), for its wall check: none while x1 < x0.
struct AptBox { float x0, z0, x1, z1; };
AptBox g_AptExtraRoom{0.0f, 0.0f, -1.0f, -1.0f};
void SetAptExtraRoom(float x0, float z0, float x1, float z1) { g_AptExtraRoom = {x0, z0, x1, z1}; }

int PosePhaseCount(); // the pose scenario's phases (the table is with Pose below)
constexpr float kPosePhaseTime = 2.4f;
constexpr float kTacticsCloseAt = 45.0f; // tactics: the player steps up to a soldier for its rifle butt
// aptfight's beats (director time): the player watches, staggers one, puts the other down by the leg, opens up with his own
// rifle (the crawler spared a while), then steps into 301's entry by the arch.
constexpr float kAptHitAt = 22.0f, kAptLegAt = 26.0f, kAptLegUntil = 40.0f, kAptFireAt = 34.0f, kAptSpareUntil = 48.0f, kAptEntryAt = 52.0f,
                kAptHideAt = 70.0f; // (then out of their sight down the corridor: they come after him, through 301's doorways)
// The soldier nearest the player with a clear line to its chest (fight / aptfight's player shoots it), skipping `skip`.
const Npc* NearestInSight(const Player& player, const NpcDirector& npcs, const std::string& skip) {
    const Npc* target = nullptr;
    float best = 1e9f;
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead || up->Name == skip) continue;
        const glm::vec3 chest = up->Feet + glm::vec3(0.0f, up->Prone ? 0.25f : up->Crouched ? 0.9f : 1.35f, 0.0f);
        glm::vec3 d = chest - player.Cam.Position;
        const float len = glm::length(d);
        if (len < 1e-3f) continue;
        d /= len;
        const float o[3] = {player.Cam.Position.x, player.Cam.Position.y, player.Cam.Position.z}, dd[3] = {d.x, d.y, d.z};
        RaycastHit hit;
        QueryFilter f;
        f.HitTriggers = 0;
        if (PhysicsWorld::RaycastFiltered(o, dd, len + 0.5f, f, hit) && hit.Hit &&
            hit.Entity == (unsigned)entt::to_integral(up->Root) && len < best) {
            best = len;
            target = up.get();
        }
    }
    return target;
}
} // namespace

static const char* EnvVar(const char* n) {
#pragma warning(suppress : 4996)
    return std::getenv(n);
}

NpcTest::NpcTest(const std::string& scenario) : m_Scenario(scenario.empty() ? "watch" : scenario) {
    if (m_Scenario == "fight") m_Duration = 75.0f;
    if (m_Scenario == "die") m_Duration = 60.0f;
    if (m_Scenario == "sandbox") m_Duration = 40.0f;
    if (m_Scenario == "pose") m_Duration = 2.5f + (float)PosePhaseCount() * kPosePhaseTime + 1.0f;
    if (m_Scenario == "deaths") m_Duration = 70.0f;
    if (m_Scenario == "escape" || m_Scenario == "escapeshut") m_Duration = 52.0f;
    if (m_Scenario == "aptwounds") m_Duration = 95.0f;
    if (m_Scenario == "aptfight") m_Duration = 100.0f;
    if (m_Scenario == "tactics") m_Duration = 70.0f;
    if (m_Scenario == "feet") m_Duration = 10.0f;
    if (m_Scenario == "reload") m_Duration = 28.0f;
    if (m_Scenario == "flame") m_Duration = 14.0f;
    if (m_Scenario == "blood") m_Duration = 44.0f;
    if (m_Scenario == "decals") m_Duration = 52.0f;
    if (m_Scenario == "doors") m_Duration = 300.0f; // (done once the route is walked)
    if (m_Scenario == "roam") m_Duration = 420.0f;  // (done once the route is walked)
    if (m_Scenario == "spider") m_Duration = 150.0f; // (done once the route is crawled)
    if (m_Scenario == "spiderhunt") m_Duration = 75.0f;
    if (m_Scenario == "spiderplay") m_Duration = 75.0f;
    if (m_Scenario == "spiderdoor") m_Duration = 45.0f;
    if (m_Scenario == "spiderdeath") m_Duration = 60.0f; // (done once the last body has settled)
    if (m_Scenario == "ragdoll") m_Duration = 240.0f;
    if (const char* t = EnvVar("NPC_TEST_SECONDS")) m_Duration = std::max(5.0f, (float)std::atof(t));
    if (const char* r = EnvVar("NPC_TEST_RECORD")) m_RecordDir = r;
    // aptwounds in another room of Apartment Hallway's north row: the same layout (the dummies side by side, the player 1.6 m off
    // toward the corridor) moved into it - a cramped bedroom, a big living room, a small bedroom with the outside wall close behind
    // the dummies. Each unit is one box for the wall check (its outer walls; not the ones inside it).
    if (const char* room = EnvVar("NPC_TEST_ROOM")) {
        struct Room { const char* Name; float Dx, Dz, X0, Z0, X1, Z1; };
        static const Room kRooms[] = {
            {"302bed", 4.2f, 0.0f, -5.6f, -9.675f, 1.2f, -1.075f},
            {"303living", 15.8f, -1.0f, 1.2f, -9.675f, 10.6f, -1.075f},
            {"303bed2", 16.9f, 2.145f, 1.2f, -9.675f, 10.6f, -1.075f},
        };
        for (const Room& r : kRooms)
            if (m_Room != r.Name && std::string(room) == r.Name) {
                m_Room = r.Name;
                m_RoomShift = glm::vec3(r.Dx, 0.0f, r.Dz);
                SetAptExtraRoom(r.X0, r.Z0, r.X1, r.Z1);
            }
        std::cout << "[NpcTest] room '" << m_Room << "' (shift " << m_RoomShift.x << ", " << m_RoomShift.z << ")" << std::endl;
    }
    std::cout << "[NpcTest] scenario '" << m_Scenario << "', " << m_Duration << " s" << std::endl;
}

void NpcTest::AddSceneSquad(World& world, AssetLibrary& assets) const {
    const bool apartment = m_Scenario == "escape" || m_Scenario == "escapeshut" || m_Scenario == "aptwounds" || m_Scenario == "aptfight";
    if (!apartment) return;
    auto spawns = world.Registry.view<NpcSpawnComponent>();
    if (spawns.begin() != spawns.end()) return; // a scene that has its own
    // What tools/scenes/build_apartment_hallway.py used to put in 301's living room (world space).
    static const char* kSquad = R"json({"empties": [{"Squad Settings": {"Bleed-Out Time": 20.0, "Corpse Time": 14.0, "Cover Peek Step": 0.800000011920929, "Cover Reach": 0.8500000238418579, "Cover Sample Spacing": 0.8999999761581421, "Crawl Speed": 0.85, "Difficulty": 1.0, "Fall Gravity": 18.0, "Foot IK Range": 25.0, "Heavy Hit Damage": 40.0, "High Cover Height": 1.5499999523162842, "Hitbox Range": 60.0, "Limp Speed Scale": 0.6000000238418579, "Limp Time": 6.0, "Low Cover Height": 0.8500000238418579, "Melee Damage": 25.0, "Melee Hit Time": 0.2199999988079071, "Melee Time": 0.550000011920929, "Mesh Check Range": 12.0, "NPC Damage Scale": 0.44999998807907104, "Respawn": true, "Respawn Delay": 3.0, "Squad Size": 2, "Stagger Time": 0.4000000059604645}, "id": 964, "name": "Test Squad Settings", "order": 964, "position": [-8.1, 0.0, -9.175], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}, {"NPC Spawn": {"Bare": false, "Brain": "Squad AI", "Outfit Seed": 1, "Skill": 0.5, "Squad": 0, "Weapon": "AKS-74U"}, "id": 965, "name": "Soldier Spawn A", "order": 965, "position": [-9.1, 0.0, -6.017], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}, {"NPC Spawn": {"Bare": false, "Brain": "Squad AI", "Outfit Seed": 2, "Skill": 0.5, "Squad": 0, "Weapon": "AKS-74U"}, "id": 966, "name": "Soldier Spawn B", "order": 966, "position": [-7.1, 0.0, -6.017], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}]})json";
    std::vector<entt::entity> created;
    if (!SceneSerializer::AppendEntitiesFromString(world, assets, kSquad, created) || created.size() != 3)
        std::cout << "[NpcTest] FAIL could not add the test squad" << std::endl;
}

void NpcTest::PrintCosts(const NpcDirector& npcs) const {
    const auto& t = npcs.ThinkCost();
    const auto& l = npcs.LateCost();
    std::printf("[NpcTest] cost avg/max ms over %d frames: think %.3f/%.3f  late %.3f/%.3f  (AI+late avg %.3f)\n", t.Frames, t.Avg(),
                t.Max, l.Avg(), l.Max, t.Avg() + l.Avg());
    for (int i = 0; i < NpcDirector::SubCount; ++i)
        std::printf("[NpcTest]   %-12s avg %.3f  max %.3f\n", NpcDirector::SubName(i), npcs.SubCost(i).Avg(), npcs.SubCost(i).Max);
}

void NpcTest::Check(bool ok, const std::string& what) {
    ++m_Checks;
    if (!ok) ++m_Failures;
    std::cout << "[NpcTest] " << (ok ? "PASS " : "FAIL ") << what << std::endl;
}

void NpcTest::Drive(Player& player, FirstPersonPresentation& weapon, NpcDirector& npcs, PlayerVitals& vitals, float dt) {
    (void)dt;
    player.ScriptedMove = true;
    player.ScriptMove = glm::vec2(0.0f);
    player.ScriptSprint = false;
    vitals.GodMode = m_Scenario != "die";
    m_Firing = false;
    if (vitals.IsDead()) return;
    // Default: look north over the arena, toward the squad's end.
    float wantYaw = m_Scenario == "sandbox" ? 180.0f : -90.0f, wantPitch = -3.0f; // sandbox: west, toward the squad
    if (m_Scenario == "ragdoll") { wantYaw = m_RViewYaw; wantPitch = -10.0f; } // over the bodies (then away from them)
    if (m_Scenario == "aptwounds") {
        // In 301's living room by the arch (or the test room), looking down the room at the two dummies.
        npcs.SpawnShift = m_RoomShift;
        npcs.TestDummies = true; // (before the director's first spawns: Drive runs before its first Think)
        const glm::vec3 at = glm::vec3(-8.0f, 0.0f, -4.4f) + m_RoomShift;
        glm::vec3 feet;
        PhysicsWorld::GetCharacterFootPosition(&feet.x);
        if (glm::length(glm::vec2(feet.x - at.x, feet.z - at.z)) > 0.3f) {
            PhysicsWorld::SetCharacterFootPosition(&at.x);
            player.Cam.Position = at + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        }
        wantYaw = -90.0f;
        wantPitch = -15.0f;
    }
    if (m_Scenario == "escape" || m_Scenario == "escapeshut") {
        npcs.TestDummies = true; // (the unarmed dummy is the one that's shot)
        // By 301's arch (the way out behind him), then - kEMoveAt s after the shot - off at the window end.
        const bool moved = m_EShotAt >= 0.0f && m_Time - m_EShotAt > kEMoveAt;
        const glm::vec3 at = moved ? glm::vec3(-9.6f, 0.0f, -9.2f) : glm::vec3(-6.9f, 0.0f, -4.6f);
        glm::vec3 feet;
        PhysicsWorld::GetCharacterFootPosition(&feet.x);
        if (glm::length(glm::vec2(feet.x - at.x, feet.z - at.z)) > 0.3f) {
            PhysicsWorld::SetCharacterFootPosition(&at.x);
            player.Cam.Position = at + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        }
        wantYaw = moved ? 90.0f : -90.0f;
        wantPitch = -10.0f;
    }
    if (m_Scenario == "aptfight") {
        // Fighters, not 301's dummies: set before the director's first spawns (Drive runs before its first Think).
        npcs.ForceBrain = 0;
        npcs.ForceWeapon = 0;
        // In the corridor outside 301's front door, looking in through it and the arch; later in the entry, by the arch; last, back
        // out of the door and off down the corridor, out of their sight - to fight on they must come through.
        const bool entry = npcs.Now() >= kAptEntryAt, hidden = npcs.Now() >= kAptHideAt;
        const glm::vec3 at = hidden ? glm::vec3(-2.5f, 0.0f, 0.3f) : entry ? glm::vec3(-6.9f, 0.0f, -2.4f) : glm::vec3(-6.9f, 0.0f, 0.45f);
        glm::vec3 feet;
        PhysicsWorld::GetCharacterFootPosition(&feet.x);
        if (glm::length(glm::vec2(feet.x - at.x, feet.z - at.z)) > 0.3f) {
            PhysicsWorld::SetCharacterFootPosition(&at.x);
            player.Velocity = glm::vec3(0.0f);
            player.Cam.Position = at + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        }
        wantYaw = -90.0f;
        wantPitch = -3.0f;
        m_Target.clear();
        if (npcs.Now() >= kAptFireAt) {
            // His own rifle at whoever he can see (the crawler left alone a while), bursts while settled on it, sights up.
            if (const Npc* target = NearestInSight(player, npcs, npcs.Now() < kAptSpareUntil ? m_ACrawlerSpared : std::string())) {
                const glm::vec3 chest = target->Feet + glm::vec3(0.0f, target->Prone ? 0.25f : target->Crouched ? 0.9f : 1.35f, 0.0f);
                const glm::vec3 d = glm::normalize(chest - player.Cam.Position);
                wantYaw = glm::degrees(std::atan2(d.z, d.x));
                wantPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
                const float err = std::abs(std::remainder(wantYaw - player.Cam.Yaw, 360.0f)) + std::abs(wantPitch - player.Cam.Pitch);
                m_FireHold += dt;
                m_Firing = err < 3.0f && std::fmod(m_FireHold, 1.0f) < 0.45f;
                m_AimErr = err;
                m_Target = target->Name;
            }
        }
        m_Reload = weapon.Ammo() == 0;
    }
    if (m_Scenario == "decals") DecalsDrive(player, wantYaw, wantPitch);
    if (m_Scenario == "doors") DoorsDrive(player, dt, wantYaw, wantPitch);
    if (m_Scenario == "roam") RoamDrive(player, dt, wantYaw, wantPitch);
    if (m_Scenario == "spiderhunt") SpiderHuntDrive(player, vitals, wantYaw, wantPitch);
    if (m_Scenario == "spiderplay") SpiderPlayDrive(player, weapon, dt, wantYaw, wantPitch);
    if (m_Scenario == "spiderdoor") SpiderDoorDrive(player, weapon, wantYaw, wantPitch);
    if (m_Scenario == "spider" || m_Scenario == "spiderdeath") SpiderWatchDrive(player, wantYaw, wantPitch);
    if (m_Scenario == "fight") {
        // Shoot whoever is in sight: the nearest soldier with a clear line to its chest.
        const Npc* target = nullptr;
        float best = 1e9f;
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead) continue;
            const glm::vec3 chest = up->Feet + glm::vec3(0.0f, up->Crouched ? 0.9f : 1.35f, 0.0f);
            glm::vec3 d = chest - player.Cam.Position;
            const float len = glm::length(d);
            if (len < 1e-3f) continue;
            d /= len;
            const float o[3] = {player.Cam.Position.x, player.Cam.Position.y, player.Cam.Position.z}, dd[3] = {d.x, d.y, d.z};
            RaycastHit hit;
            QueryFilter f;
            f.HitTriggers = 0;
            if (PhysicsWorld::RaycastFiltered(o, dd, len + 0.5f, f, hit) && hit.Hit &&
                hit.Entity == (unsigned)entt::to_integral(up->Root) && len < best) {
                best = len;
                target = up.get();
            }
        }
        if (target) {
            const glm::vec3 chest = target->Feet + glm::vec3(0.0f, target->Crouched ? 0.9f : 1.35f, 0.0f);
            const glm::vec3 d = glm::normalize(chest - player.Cam.Position);
            wantYaw = glm::degrees(std::atan2(d.z, d.x));
            wantPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            // Bursts: fire while settled on it.
            const float err = std::abs(std::remainder(wantYaw - player.Cam.Yaw, 360.0f)) + std::abs(wantPitch - player.Cam.Pitch);
            m_FireHold += dt;
            m_Firing = err < 3.0f && std::fmod(m_FireHold, 1.0f) < 0.45f;
            m_AimErr = err;
            m_Target = target->Name;
        } else {
            m_Target.clear();
        }
        m_Reload = weapon.Ammo() == 0;
    }
    if (m_Scenario == "flame") {
        // The muzzle flame: the AK's single shots from the hip (1-3.5 s), then bursts on the sights
        // (4-7 s); then the second slot (the Remington): shots from the hip (8.5-10.5 s), then on the
        // sights (11-13 s).
        m_FireHold += dt;
        const float t = m_FireHold;
        if (t >= 7.5f && !m_FlameSwitched && weapon.SlotCount() > 1) {
            weapon.SelectSlot(1);
            m_FlameSwitched = true;
        }
        const float u = t >= 7.5f ? t - 7.5f : t; // each gun's own clock
        if (u >= 1.0f && u < 3.5f) m_Firing = std::fmod(u, 0.5f) < 0.05f;
        if (u >= 4.0f && u < (t >= 7.5f ? 5.5f : 7.0f)) {
            m_Target = "flame"; // sights up
            m_Firing = u >= 4.5f && std::fmod(u, 1.0f) < 0.3f;
        } else {
            m_Target.clear();
        }
        m_Reload = weapon.Ammo() == 0;
    }
    if (m_Scenario == "tactics") {
        auto aimAt = [&](const glm::vec3& at) {
            const glm::vec3 d = glm::normalize(at - player.Cam.Position);
            wantYaw = glm::degrees(std::atan2(d.z, d.x));
            wantPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            return std::abs(std::remainder(wantYaw - player.Cam.Yaw, 360.0f)) + std::abs(wantPitch - player.Cam.Pitch);
        };
        if (npcs.Now() < kTacticsCloseAt) {
            // Pin a soldier down in cover: bursts just past its head (over low cover, along the face of high cover).
            const Npc* pin = nullptr;
            float best = 1e9f;
            for (const auto& up : npcs.Npcs()) {
                if (!up || up->Dead || up->Doing != Behaviour::CoverFight || up->Cover < 0) continue;
                const float d = glm::length(up->Feet - player.Cam.Position);
                if (d < best) { best = d; pin = up.get(); }
            }
            if (pin) {
                const float err = aimAt(pin->Feet + glm::vec3(0.0f, pin->Crouched ? 1.4f : 1.9f, 0.0f));
                m_AimErr = err;
                m_FireHold += dt;
                m_Firing = err < 4.0f && std::fmod(m_FireHold, 1.0f) < 0.6f;
                m_Target = pin->Name;
            } else {
                m_Target.clear();
            }
            m_Reload = weapon.Ammo() == 0;
        } else {
            // Up close: the player put an arm's length in front of a soldier, to be struck.
            m_Target.clear();
            if (m_TVictim.empty()) {
                const Npc* v = nullptr;
                float best = 1e9f;
                for (const auto& up : npcs.Npcs()) {
                    if (!up || up->Dead || up->Wounded || up->Cct == PhysicsWorld::kNoCharacter) continue;
                    const float d = glm::length(up->Feet - player.Cam.Position);
                    if (d < best) { best = d; v = up.get(); }
                }
                if (v) {
                    const float yaw = v->Body.Yaw();
                    const glm::vec3 feet = v->Feet + glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) * 1.5f + glm::vec3(0.0f, 0.05f, 0.0f);
                    const float f[3] = {feet.x, feet.y, feet.z};
                    PhysicsWorld::SetCharacterFootPosition(f);
                    player.Velocity = glm::vec3(0.0f);
                    player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
                    m_TVictim = v->Name;
                    std::printf("[NpcTest] tactics: the player steps up to %s\n", v->Name.c_str());
                }
            }
            for (const auto& up : npcs.Npcs())
                if (up && !up->Dead && up->Name == m_TVictim) aimAt(up->Feet + glm::vec3(0.0f, 1.4f, 0.0f));
        }
    }
    if (m_LastAmmo >= 0 && weapon.Ammo() < m_LastAmmo) m_PlayerShots += m_LastAmmo - weapon.Ammo();
    m_LastAmmo = weapon.Ammo();
    // Turn the view toward it like a player would (a fast but finite turn).
    const float dy = std::remainder(wantYaw - player.Cam.Yaw, 360.0f), dp = wantPitch - player.Cam.Pitch;
    const float step = 240.0f * dt;
    player.Cam.Yaw += std::clamp(dy, -step, step);
    player.Cam.Pitch += std::clamp(dp, -step, step);
}

bool NpcTest::SceneCamera(glm::vec3& pos, float& yaw, float& pitch) const {
    if (!m_HaveCam) return false;
    pos = m_CamPos;
    yaw = m_CamYaw;
    pitch = m_CamPitch;
    return true;
}

void NpcTest::After(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player) {
    (void)world;
    const float now = npcs.Now();
    m_Time = now;
    m_Shot.clear();
    if (m_Scenario == "deaths") {
        Deaths(world, npcs, now);
        return;
    }
    if (m_Scenario == "escape" || m_Scenario == "escapeshut") {
        Escape(world, npcs, now);
        return;
    }
    if (m_Scenario == "aptwounds") {
        AptWounds(world, npcs, now);
        return;
    }
    if (m_Scenario == "aptfight") {
        AptFight(world, npcs, vitals, player, now);
        return;
    }
    if (m_Scenario == "blood") {
        Blood(world, npcs, now);
        return;
    }
    if (m_Scenario == "ragdoll") {
        Ragdoll(world, npcs, player);
        return;
    }
    if (m_Scenario == "decals") {
        Decals(world, npcs, now);
        return;
    }
    if (m_Scenario == "doors") {
        Doors(world, player);
        return;
    }
    if (m_Scenario == "roam") {
        Roam(world, player);
        return;
    }
    if (m_Scenario == "spider") {
        SpiderRoute(world);
        return;
    }
    if (m_Scenario == "spiderhunt") {
        SpiderHunt(world);
        return;
    }
    if (m_Scenario == "spiderplay") {
        SpiderPlay(world);
        return;
    }
    if (m_Scenario == "spiderdoor") {
        SpiderDoor(world);
        return;
    }
    if (m_Scenario == "spiderdeath") {
        SpiderDeath(world);
        return;
    }
    if (m_Scenario == "feet") {
        Feet(npcs, now);
        return;
    }
    if (m_Scenario == "reload") {
        Reload(world, npcs, now);
        return;
    }
    if (m_Scenario == "flame") {
        // The Scene view off the player's right shoulder, on the muzzle.
        const glm::vec3 eye = player.Cam.Position, front = player.Cam.Front();
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        m_CamPos = eye + right * 0.9f + front * 0.5f + glm::vec3(0.0f, 0.1f, 0.0f);
        const glm::vec3 d = glm::normalize(eye + front * 0.7f - glm::vec3(0.0f, 0.15f, 0.0f) - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        if (now >= m_Duration && !m_Done) {
            m_Done = true;
            Check(m_PlayerShots > 0, "the player fired (" + std::to_string(m_PlayerShots) + " rounds)");
            Check(m_FlameSwitched, "the player switched to the second weapon");
        }
        return;
    }
    if (m_Scenario == "pose") {
        Pose(world, npcs, now);
        if (now >= m_Duration && !m_Done) {
            m_Done = true;
            std::printf("[NpcTest] worst: gun-head %.1f cm (%s), gun-torso unshouldered %.1f cm (%s), elbow %.1f cm (%s), hand off %.1f cm (%s)\n",
                        m_WorstHead * 100.0f, m_WorstHeadAt.c_str(), m_WorstTorso * 100.0f, m_WorstTorsoAt.c_str(), m_WorstElbow * 100.0f,
                        m_WorstElbowAt.c_str(), m_WorstHand * 100.0f, m_WorstHandAt.c_str());
            Check(!m_Probe[0].empty() && !m_Probe[1].empty(), "an AK and a Remington soldier to probe");
            Check(m_PosePhase >= PosePhaseCount(), "every pose ran");
            Check(m_WorstHead >= 0.02f, "the gun stays 2 cm off the head, neck and hood");
            Check(m_WorstElbow >= 0.0f, "the elbows stay out of the torso");
            Check(m_WorstHand <= 0.03f, "the hands stay on the gun (within 3 cm)");
        }
        return;
    }
    int alive = 0;
    bool nan = false;
    for (const auto& up : npcs.Npcs()) {
        if (!up) continue;
        const Npc& n = *up;
        if (n.Dead) {
            if (std::find(m_Dead.begin(), m_Dead.end(), n.Name) == m_Dead.end()) {
                m_Dead.push_back(n.Name);
                ++m_Kills;
                m_LastKill = n.Name;
                m_LastKillAt = now;
                m_KillShots = 0;
                std::cout << "[NpcTest] t=" << now << " " << n.Name << " killed" << std::endl;
            }
            continue;
        }
        ++alive;
        if (!std::isfinite(n.Feet.x) || !std::isfinite(n.Feet.y) || !std::isfinite(n.Feet.z)) nan = true;
        if (n.Mem.Known && m_FirstKnown < 0.0f) m_FirstKnown = now;
        if (n.Doing == Behaviour::CoverFight && m_FirstCover < 0.0f) m_FirstCover = now;
        if (n.ShotsFired > 0 && m_FirstShot < 0.0f) m_FirstShot = now;
        const std::string b = BehaviourName(n.Doing);
        if (std::find(m_BehavioursSeen.begin(), m_BehavioursSeen.end(), b) == m_BehavioursSeen.end()) m_BehavioursSeen.push_back(b);
    }
    if (nan) Check(false, "a soldier's position went NaN");
    m_MaxAlive = std::max(m_MaxAlive, alive);
    if (!vitals.Indicators().empty() && m_FirstDamage < 0.0f) m_FirstDamage = now;
    if (vitals.IsDead()) m_PlayerDied = true;
    if (m_PlayerDied && !vitals.IsDead()) m_PlayerRespawned = true;

    // The log: every soldier, every two seconds.
    if (now >= m_NextLog) {
        m_NextLog = now + 2.0f;
        char line[512];
        std::snprintf(line, sizeof line, "[NpcTest] t=%5.1f player hp=%3.0f%s  shooters=%d  think=%.2fms late=%.2fms", now, vitals.Health(),
                      vitals.IsDead() ? " DEAD" : "", npcs.ShootersNow(), npcs.LastThinkMs(), npcs.LastLateMs());
        std::cout << line << std::endl;
        float fastest = 0.0f;
        if (int awake = 0, asleep = 0, frozen = 0; PhysicsWorld::RagdollCounts(awake, asleep, frozen, &fastest), awake + asleep + frozen > 0)
            std::printf("[NpcTest]   ragdolls awake %d (fastest part %.2f m/s) asleep %d frozen %d  physics %.2f ms\n", awake, fastest, asleep,
                        frozen, PhysicsWorld::GetDebugStats().StepMillis);
        if (m_Scenario == "fight" || m_Scenario == "tactics")
            std::cout << "[NpcTest]   player target='" << m_Target << "' err=" << m_AimErr << " shots=" << m_PlayerShots
                      << " yaw=" << player.Cam.Yaw << std::endl;
        for (const auto& up : npcs.Npcs()) {
            if (!up) continue;
            const Npc& n = *up;
            const int ammo = n.Weapon && n.Weapon->IsActive() ? n.Weapon->Ammo() : -1;
            std::snprintf(line, sizeof line,
                          "[NpcTest]   %-10s %-11s ph%-2d %-10s hp%3.0f sup%.2f %s ammo%2d %s%s%s pos(%5.1f,%4.1f,%5.1f) v%3.1f anim=%s gun=%s",
                          n.Name.c_str(), BehaviourName(n.Doing), n.Phase, RoleName(n.Role), n.Health, n.Suppression,
                          n.Class == WeaponClass::Shotgun ? "870" : "AK ", ammo, n.Mem.Known ? "K" : "-", n.Mem.Visible ? "V" : "-",
                          n.HasAttackToken ? "A" : "-", n.Feet.x, n.Feet.y, n.Feet.z, glm::length(n.Velocity),
                          n.Body.StateName(world).c_str(), n.Weapon && n.Weapon->IsActive() ? n.Weapon->CurrentState().c_str() : "-");
            std::cout << line << std::endl;
        }
    }

    // Tactics: while pinning, a steady stream of near misses past the target's head as well as the player's bursts (rounds
    // that would otherwise go wherever this run's fight sends them) - so the suppression it's under is the same every run.
    if (m_Scenario == "tactics" && now < kTacticsCloseAt && now >= m_TNextCrack)
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Name == m_Target && up->Doing == Behaviour::CoverFight) {
                m_TNextCrack = now + 0.1f;
                npcs.OnPlayerShotLine(player.Cam.Position, up->SightEye + glm::vec3(0.0f, 0.5f, 0.0f));
            }
    // Tactics: the Scene view on a blind fire and on the rifle-butt strike, side on.
    if (m_Scenario == "tactics") {
        const Npc* show = nullptr;
        const char* what = nullptr;
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead) continue;
            if (up->Intent.BlindFire && now - up->LastBlindFire > 0.6f && m_TBlindShots < 3 && now - m_TShotAt > 1.5f) {
                show = up.get();
                what = "blindfire";
            }
            // The strike at its wind-up, its full reach and its recovery (one frame each).
            const float mt = now - up->MeleeAt;
            const float at[3] = {0.08f, 0.24f, 0.42f};
            if (m_TMeleeShots < 3 && mt >= at[m_TMeleeShots] && mt < at[m_TMeleeShots] + 0.05f) { show = up.get(); what = "melee"; }
        }
        if (show) {
            const float yaw = show->Body.Yaw();
            const glm::vec3 left(std::cos(yaw), 0.0f, -std::sin(yaw)), fwd(std::sin(yaw), 0.0f, std::cos(yaw));
            const glm::vec3 chest = show->Feet + glm::vec3(0.0f, show->Crouched ? 0.9f : 1.3f, 0.0f);
            m_CamPos = chest + left * 3.2f + fwd * 0.6f + glm::vec3(0.0f, 0.3f, 0.0f);
            const glm::vec3 d = glm::normalize(chest - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(d.y));
            m_HaveCam = true;
            const int index = what[0] == 'b' ? m_TBlindShots++ : m_TMeleeShots++;
            char name[64];
            std::snprintf(name, sizeof name, "npc_tactics_%s_%d", what, index);
            m_Shot = name;
            m_TShotAt = now;
            std::printf("[NpcTest] %s: %s %s, crouched %d, gun out %.2f, cover %s\n", name, show->Name.c_str(), BehaviourName(show->Doing),
                        (int)show->Crouched, show->BlindLift,
                        show->Cover < 0 ? "none" : npcs.Cover().Points()[(size_t)show->Cover].High ? "high" : "low");
        }
    }
    // A fresh kill: the Scene view on the body as it goes down (0.6 s and 3 s after).
    const Npc* corpse = nullptr;
    if (now - m_LastKillAt < 3.5f)
        for (const auto& up : npcs.Npcs())
            if (up && up->Dead && up->Name == m_LastKill && up->Ragdoll) corpse = up.get();
    if (corpse) {
        const glm::vec3 at = corpse->Ragdoll->Root();
        m_CamPos = at + glm::vec3(2.2f, 1.6f, 2.2f);
        const glm::vec3 d = glm::normalize(at - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(d.y));
        m_HaveCam = true;
        const float since = now - m_LastKillAt;
        if ((m_KillShots == 0 && since > 0.6f) || (m_KillShots == 1 && since > 3.0f)) {
            char name[80];
            std::snprintf(name, sizeof name, "npc_%s_kill%d_%s_%d", m_Scenario.c_str(), m_Kills, m_KillShots == 0 ? "fall" : "rest", m_KillShots);
            m_Shot = name;
            ++m_KillShots;
            std::printf("[NpcTest] %s ragdoll pelvis (%.2f, %.2f, %.2f) %s\n", corpse->Name.c_str(), at.x, at.y, at.z,
                        corpse->Ragdoll->Asleep() ? "asleep" : "moving");
        }
    } else
    // The Scene view: alternately close on one soldier and the whole arena from behind the player.
    {
        const Npc* focus = nullptr;
        int k = 0;
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead) continue;
            if (k++ == m_Focus % std::max(alive, 1)) { focus = up.get(); break; }
        }
        const bool close = (m_ShotIndex % 2) == 0 && focus;
        if (close) {
            // Three and a half metres off its front-left shoulder, chest height.
            const float yaw = focus->Body.Yaw();
            const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw));
            const glm::vec3 chest = focus->Feet + glm::vec3(0.0f, focus->Crouched ? 0.9f : 1.3f, 0.0f);
            // Whichever of a few spots round it has a clear view (no wall between).
            const glm::vec3 offsets[4] = {fwd * 3.0f + left * 1.6f, fwd * 3.0f - left * 1.6f, left * 3.2f + fwd * 0.8f, -left * 3.2f + fwd * 0.8f};
            m_CamPos = chest + offsets[0] + glm::vec3(0.0f, 0.25f, 0.0f);
            for (const glm::vec3& o : offsets) {
                const glm::vec3 at = chest + o + glm::vec3(0.0f, 0.25f, 0.0f);
                glm::vec3 d = at - chest;
                const float len = glm::length(d);
                d /= len;
                const float org[3] = {chest.x, chest.y, chest.z}, dd[3] = {d.x, d.y, d.z};
                RaycastHit hit;
                QueryFilter f;
                f.HitTriggers = 0;
                if (!(PhysicsWorld::RaycastSolid(org, dd, len, f, hit) && hit.Hit)) { m_CamPos = at; break; }
            }
            const glm::vec3 d = glm::normalize(chest - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(d.y));
        } else {
            m_CamPos = player.Cam.Position + glm::vec3(0.0f, 9.0f, 9.0f);
            m_CamYaw = -90.0f;
            m_CamPitch = -32.0f;
        }
        m_HaveCam = true;
        // A close shot waits up to a second and a half for its soldier to fire (the flash in frame).
        const bool firing = close && focus->LastShot >= npcs.Now() - 1e-4f;
        if (now >= m_NextShot && now > 3.0f && (!close || firing || now >= m_NextShot + 1.5f)) {
            m_NextShot = now + 3.0f;
            char name[64];
            std::snprintf(name, sizeof name, "npc_%s_%02d_%s%s", m_Scenario.c_str(), m_ShotIndex, close ? "close" : "wide",
                          firing ? "_firing" : "");
            m_Shot = name;
            ++m_ShotIndex;
            if (close) ++m_Focus;
        }
    }

    // Sandbox: the dev panel's Kill All, once the squad has been up a while.
    if (m_Scenario == "sandbox") {
        if (!m_DevKilled && now >= 20.0f) {
            m_DevKilled = true;
            m_DevKilledCount = DevPanel::KillAll(world, npcs);
        }
        if (m_DevKilled && now >= 20.5f && m_AliveAfterKill < 0) {
            m_AliveAfterKill = alive;
        }
    }

    if (now >= m_Duration && !m_Done) {
        m_Done = true;
        if (m_Scenario == "sandbox") {
            std::printf("[NpcTest] sandbox: %d soldier(s) spawned, %zu spawn point(s), %zu cover points\n", m_MaxAlive,
                        npcs.SpawnPoints().size(), npcs.Cover().Points().size());
            Check(m_MaxAlive >= 3, "the Sandbox squad spawned (" + std::to_string(m_MaxAlive) + " soldiers)");
            Check(npcs.Nav().Valid() && npcs.Nav().PolyCount() > 0, "a navigation mesh was built for the Sandbox");
            Check(!npcs.Cover().Points().empty(), "the Sandbox has cover (" + std::to_string(npcs.Cover().Points().size()) + " points)");
            Check(m_DevKilledCount >= 3 && m_AliveAfterKill == 0, "Kill All killed the squad (" + std::to_string(m_DevKilledCount) + ", " + std::to_string(m_AliveAfterKill) + " left)");
            std::fflush(stdout);
            return;
        }
        std::string seen;
        for (const auto& b : m_BehavioursSeen) seen += b + ", ";
        std::cout << "[NpcTest] behaviours seen: " << seen << std::endl;
        std::printf("[NpcTest] first known %.1f s, first shot %.1f s, first hit on player %.1f s, first in cover %.1f s, max shooters %d\n",
                    m_FirstKnown, m_FirstShot, m_FirstDamage, m_FirstCover, npcs.MaxShootersSeen());
        const NpcDirector::TacticStats& ts = npcs.Tactics();
        std::printf("[NpcTest] tactics: bounds %d (covered %d), cover orders %d, blind fire %d, melee %d (landed %d), backpedals %d\n",
                    ts.Bounds, ts.CoveredBounds, ts.CoverOrders, ts.BlindFires, ts.Melees, ts.MeleeHits, ts.Backpedals);
        std::printf("[NpcTest] tactics: flanks %d (%d found no cover), ", ts.Flanks, ts.FlankFails);
        std::printf("pincers %d, pushes on a hurt player %d, startles %d\n", ts.Pincers, ts.HurtPushes, ts.Startles);
        PrintCosts(npcs);
        std::fflush(stdout);
        Check(m_MaxAlive >= 3, "at least three soldiers spawned (" + std::to_string(m_MaxAlive) + ")");
        Check(npcs.Nav().Valid() && npcs.Nav().PolyCount() > 0, "a navigation mesh was built");
        Check(!npcs.Cover().Points().empty(), "cover points were found (" + std::to_string(npcs.Cover().Points().size()) + ")");
        Check(m_FirstKnown >= 0.0f && m_FirstKnown < 15.0f, "the squad noticed the player within 15 s");
        Check(m_FirstShot >= 0.0f, "the squad fired");
        Check(m_FirstDamage >= 0.0f, "the squad's rounds reached the player");
        Check(m_FirstCover >= 0.0f, "someone fought from cover");
        Check(npcs.MaxShootersSeen() <= 4, "never more than the attack tokens shooting at once");
        {
            // Every gun found its muzzle (no muzzle: its rounds come from nowhere and there's no laser).
            int armed = 0, muzzled = 0;
            for (const auto& up : npcs.Npcs()) {
                if (!up || up->Dead || !up->Weapon || !up->Weapon->IsActive()) continue;
                ++armed;
                if (up->Weapon->BarrelReport().HasMuzzle) ++muzzled;
            }
            Check(armed > 0 && muzzled == armed, "every soldier's gun has its muzzle (" + std::to_string(muzzled) + " of " + std::to_string(armed) + ")");
        }
        if (npcs.Fx) {
            Check(npcs.Fx->ShotsHeard() > 0, "gunfire was heard (" + std::to_string(npcs.Fx->ShotsHeard()) + " reports)");
            Check(npcs.Fx->WhizzesHeard() > 0, "rounds cracked past the player (" + std::to_string(npcs.Fx->WhizzesHeard()) + ")");
        }
        if (m_Scenario == "fight") {
            Check(m_Kills > 0, "the player killed " + std::to_string(m_Kills) + " soldier(s)");
            // The replacements are built from the spare and the gone corpses' bodies, not from Soldier.json again.
            Check(npcs.ReusedBodies() > 0, "a replacement took a pooled body (" + std::to_string(npcs.ReusedBodies()) + " of " +
                                               std::to_string(m_Kills) + " replacements)");
        }
        if (m_Scenario == "tactics") {
            Check(ts.BlindFires > 0, "a pinned soldier blind-fired over its cover (" + std::to_string(ts.BlindFires) + ")");
            Check(ts.Melees > 0 && ts.MeleeHits > 0, "a soldier struck the player at arm's length (" + std::to_string(ts.MeleeHits) + " landed)");
            Check(ts.CoveredBounds * 2 >= ts.Bounds, "most bounds in the player's view went under covering fire (" +
                                                          std::to_string(ts.CoveredBounds) + " of " + std::to_string(ts.Bounds) + ")");
        }
        if (m_Scenario == "die") {
            Check(m_PlayerDied, "the player was killed");
            Check(m_PlayerRespawned, "the player respawned");
        }
    }
}

// --- deaths: hitboxes, hit reactions, deaths -------------------------------------------------------------------------
namespace {
unsigned RootId(const Npc& n) { return (unsigned)entt::to_integral(n.Root); }

struct DeathCase {
    const char* Name;
    int Part;          // the hitbox part shot (NpcRagdoll's order)
    float Damage;      // the weapon's damage per round
    bool LimpAfterFirst, StaggerAfterFirst, DiesFirst;
};
const DeathCase kDeathCases[] = {
    {"head", 2, 34.0f, false, false, true},       // 34 x the head multiplier: dead at once
    {"chest", 1, 34.0f, false, false, false},     // a few rounds
    {"thigh", 7, 40.0f, true, false, false},      // a leg: it limps
    {"forearm", 4, 70.0f, false, true, false},    // a heavy hit on an arm: it staggers, a second kills
};
constexpr int kDeathCaseCount = (int)(sizeof(kDeathCases) / sizeof(kDeathCases[0]));
const char* const kCorpseBones[4] = {"pelvis", "head", "hand_l", "foot_r"};
// The wounded: what touches the floor while it crawls (the calf bone's head is the knee), and when it counts as planted.
const char* const kWoundContacts[4] = {"hand_l", "hand_r", "calf_l", "calf_r"};
constexpr float kPlantedHeight = 0.06f;  // m over the feet
constexpr float kCrawlWatch = 7.5f;      // s of crawling measured from the order (0.6 m/s: the 4 m and the turn)
constexpr float kCrawlShotEvery = 0.7f;
constexpr int kCrawlShots = 6;
constexpr float kViewSettle = 0.15f;     // s on a soldier before a shot of him (the Scene view lags the frame by one)

float Percentile95(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    const size_t k = std::min(v.size() - 1, (size_t)(0.95f * (float)v.size()));
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)k, v.end());
    return v[k];
}

// "ProneDrop@0.0 ProneCrawl@3.4 ..."
std::string StateLog(const std::vector<std::pair<std::string, float>>& states) {
    std::string out;
    char at[32];
    for (const auto& st : states) {
        std::snprintf(at, sizeof at, "@%.1f", st.second);
        out += " " + st.first + at;
    }
    return out;
}
} // namespace

void NpcTest::TrackWound(World& world, const Npc& n, WoundTrack& w, float now, float dt, bool motion) {
    const std::string state = n.Body.StateName(world);
    if (w.States.empty() || w.States.back().first != state) w.States.push_back({state, now - m_DAt});
    if (!motion) return;
    glm::vec3 pelvis(0.0f), contact[4]{};
    if (!n.Body.BoneWorld(world, "pelvis", pelvis)) return;
    bool planted[4];
    for (int k = 0; k < 4; ++k) planted[k] = n.Body.BoneWorld(world, kWoundContacts[k], contact[k]) && contact[k].y - n.Feet.y < kPlantedHeight;
    const float yaw = n.Body.Yaw();
    if (!w.On) { w.On = true; w.From = now; w.Start = n.Feet; }
    if (w.HavePrev && dt <= 1e-4f) return; // no time passed: nothing to measure against
    w.MaxPelvis = std::max(w.MaxPelvis, pelvis.y - n.Feet.y);
    w.TopSpeed = std::max(w.TopSpeed, glm::length(n.Velocity));
    if (state == "InjuredLying" && now - m_DAt - w.States.back().second >= 0.5f)
        w.LyingPelvis = std::max(w.LyingPelvis, pelvis.y - n.Feet.y);
    if (w.HavePrev) {
        w.Seconds += dt;
        const glm::vec3 moved = n.Feet - w.PrevFeet;
        w.Path += glm::length(glm::vec2(moved.x, moved.z));
        w.MaxPop = std::max(w.MaxPop, std::abs(pelvis.y - w.PrevPelvis.y));
        // Crawling, the body lies over the capsule: no pose a metre ahead of it, no lurch from one state to the next.
        if (state.rfind("ProneCrawl", 0) == 0) {
            w.MaxOffset = std::max(w.MaxOffset, glm::length(glm::vec2(pelvis.x - n.Feet.x, pelvis.z - n.Feet.z)));
            const glm::vec3 lurch = (pelvis - w.PrevPelvis) - moved;
            w.MaxLeap = std::max(w.MaxLeap, glm::length(glm::vec2(lurch.x, lurch.z)));
        }
        w.MaxYawRate = std::max(w.MaxYawRate, glm::degrees(std::abs(std::remainder(yaw - w.PrevYaw, 6.2831853f))) / dt);
        // A planted hand or knee should stay where it was put: its slide over the floor, frame by frame.
        for (int k = 0; k < 4; ++k) {
            if (planted[k] && w.PrevPlanted[k]) {
                const glm::vec3 d = contact[k] - w.PrevContact[k];
                const float slide = glm::length(glm::vec2(d.x, d.z));
                w.PlantedSpeed[k].push_back(slide / dt);
                w.Slide[k] += slide;
                w.MaxSlide[k] = std::max(w.MaxSlide[k], w.Slide[k]);
            } else w.Slide[k] = 0.0f;
        }
    }
    // Prone and moving: the dragged leg straight, the crawling one bent (1 = a straight knee: hip to ankle over the
    // thigh and shin's lengths).
    if (n.Prone && glm::length(n.Velocity) > 0.2f) {
        static const char* kLeg[2][3] = {{"thigh_l", "calf_l", "foot_l"}, {"thigh_r", "calf_r", "foot_r"}};
        glm::vec3 leg[2][3]{};
        bool all = true;
        for (int s = 0; s < 2; ++s)
            for (int j = 0; j < 3; ++j) all = n.Body.BoneWorld(world, kLeg[s][j], leg[s][j]) && all;
        if (all) {
            for (int s = 0; s < 2; ++s)
                w.Straight[s] += glm::length(leg[s][2] - leg[s][0]) /
                                 std::max(glm::length(leg[s][1] - leg[s][0]) + glm::length(leg[s][2] - leg[s][1]), 1e-3f);
            ++w.StraightFrames;
        }
    }
    w.HavePrev = true;
    w.PrevFeet = n.Feet;
    w.PrevPelvis = pelvis;
    w.PrevYaw = yaw;
    for (int k = 0; k < 4; ++k) { w.PrevPlanted[k] = planted[k]; w.PrevContact[k] = contact[k]; }
}

namespace {
// How far `p` is inside a wall of Apartment Hallway's 301 and the corridor: 0 inside any of its rooms (the floor plan's rects,
// scene coordinates), else the distance to the nearest (the wall's thickness is all solid).
float AptWallDepth(const glm::vec3& p) {
    using R = AptBox;
    static const R rooms[] = {
        {-10.6f, -9.67f, -5.6f, -3.78f}, // 301 living room
        {-8.2f, -3.78f, -5.6f, -1.07f},  // 301 entry
        {-10.6f, -3.78f, -8.2f, -1.07f}, // 301 bathroom
        {-10.6f, -1.07f, 10.6f, 1.07f},  // the corridor
        {-10.6f, 1.07f, 10.6f, 9.67f},   // the south units (not modelled room by room: anywhere over there counts)
    };
    float best = 1e9f;
    for (const R& r : rooms) {
        const float dx = std::max({r.x0 - p.x, 0.0f, p.x - r.x1}), dz = std::max({r.z0 - p.z, 0.0f, p.z - r.z1});
        best = std::min(best, std::sqrt(dx * dx + dz * dz));
    }
    if (const R& r = g_AptExtraRoom; r.x1 >= r.x0) {
        const float dx = std::max({r.x0 - p.x, 0.0f, p.x - r.x1}), dz = std::max({r.z0 - p.z, 0.0f, p.z - r.z1});
        best = std::min(best, std::sqrt(dx * dx + dz * dz));
    }
    return best;
}
} // namespace

void NpcTest::AptWounds(World& world, NpcDirector& npcs, float now) {
    npcs.HoldFire = true;
    npcs.NoLod = true;
    npcs.TrackDeathPop = true;
    m_WPrevNow = now;
    FirstPersonWeaponGameplay w;
    w.HeadMultiplier = 2.0f;
    w.LimbMultiplier = 0.75f;
    w.FalloffStart = 1000.0f;
    w.FalloffEnd = 2000.0f;
    w.FalloffMin = 1.0f;
    const glm::vec3 eye = glm::vec3(-8.0f, 1.6f, -4.4f) + m_RoomShift;
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    auto shoot = [&](Npc& n, int part, float damage) {
        w.Damage = damage;
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        const glm::vec3 dir = glm::normalize(c - eye);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, RootId(n), c, eye, dir, w, &killed, &head);
    };
    auto setHealth = [&](Npc& n, float hp) {
        n.Health = hp;
        if (auto* h = world.Registry.try_get<HealthComponent>(n.Root)) h->Current = hp;
    };
    auto step = [&](int to) { m_WStep = to; m_WAt = now; };
    // Nothing of a body in a wall: its bones while it animates, its parts once it is a ragdoll.
    auto walls = [&](const Npc& n, const char* what) {
        static const char* const kBones[] = {"head", "pelvis", "hand_l", "hand_r", "foot_l", "foot_r", "spine_03"};
        auto note = [&](const glm::vec3& p, const std::string& at) {
            const float d = AptWallDepth(p);
            if (d > m_WWorstWall) {
                if (d > 0.05f && (int)(d * 20.0f) != (int)(m_WWorstWall * 20.0f)) // (each 5 cm deeper)
                    std::printf("[NpcTest] aptwounds: %s %s %.1f cm into a wall at (%.2f, %.2f, %.2f), %.2f s into step %d\n", what, at.c_str(),
                                d * 100.0f, p.x, p.y, p.z, now - m_WAt, m_WStep);
                m_WWorstWall = d;
                m_WWorstWallAt = std::string(what) + " " + at;
            }
        };
        if (n.Ragdoll && n.Ragdoll->Active())
            for (int part = 0; part < NpcRagdoll::kRagParts; ++part) note(n.Ragdoll->PartPosition(part), "ragdoll part " + std::to_string(part));
        else
            for (const char* b : kBones) {
                glm::vec3 p;
                if (n.Body.BoneWorld(world, b, p)) note(p, b);
            }
    };
    auto noteState = [&](const Npc& n) {
        const std::string st = n.Ragdoll && n.Ragdoll->Active() ? (n.Dead ? "corpse" : "ragdoll") : n.Body.StateName(world);
        if (m_WStates.empty() || m_WStates.back().first != st) m_WStates.push_back({st, now - m_WAt});
    };
    Npc* a = m_WA.empty() ? nullptr : find(m_WA);
    Npc* b = m_WB.empty() ? nullptr : find(m_WB);
    const float t = now - m_WAt;
    switch (m_WStep) {
    case 0: { // the two dummies, standing
        if (now < 2.5f) return;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Dummy && up->Hitboxes && up->Hitboxes->Active()) (up->Weapon ? m_WA : m_WB) = up->Name;
        a = find(m_WA);
        b = find(m_WB);
        if (!a || !b) {
            if (now > 8.0f) { Check(false, "aptwounds: both dummies stand in the test room (" + m_Room + ")"); m_Done = true; }
            return;
        }
        std::printf("[NpcTest] aptwounds: %s (AK) at (%.2f, %.2f), %s (unarmed) at (%.2f, %.2f)\n", a->Name.c_str(), a->Feet.x, a->Feet.z,
                    b->Name.c_str(), b->Feet.x, b->Feet.z);
        shoot(*b, 4, 10.0f); // a forearm: a flinch
        step(1);
        return;
    }
    case 1: // the flinch
        if (t < 0.8f) return;
        Check(!b->Dead && !b->Wounded && b->Health < b->MaxHealth, "aptwounds: a round in the forearm hurts him and he stays up");
        // The gut shot: a torso round that would leave him under 20% puts him down dying.
        npcs.SetWoundChance(1.0f);
        setHealth(*a, 25.0f);
        shoot(*a, 0, 30.0f); // the belly (his forearms cross his chest, holding the AK ready)
        {
            const auto* h = world.Registry.try_get<HealthComponent>(a->Root);
            std::printf("[NpcTest] aptwounds: gut shot on %s: health %.1f (component %.1f / %.1f%s), wounded %d dying %d dead %d rolled %d\n",
                        a->Name.c_str(), a->Health, h ? h->Current : -1.0f, h ? h->Max : -1.0f, h && h->Invulnerable ? ", invulnerable" : "",
                        (int)a->Wounded, (int)a->Dying, (int)a->Dead, (int)a->WoundRolled);
        }
        Check(a->Wounded && a->Dying && !a->Dead && !a->Weapon, "aptwounds: gut-shot, he drops the AK and is dying on his feet");
        m_WStates.clear();
        step(2);
        return;
    case 2: { // the stagger and collapse, in the room
        walls(*a, "gut-shot");
        noteState(*a);
        if ((int)(t * 2.0f) != (int)((t - 1.0f / 60.0f) * 2.0f))
            std::printf("[NpcTest] aptwounds %4.1f s: gut-shot at (%.2f, %.2f) yaw %.0f vel (%.2f, %.2f), move %d to (%.2f, %.2f), agent (%.2f, %.2f) corner (%.2f, %.2f), %s\n", t,
                        a->Feet.x, a->Feet.z, glm::degrees(a->Body.Yaw()), a->Velocity.x, a->Velocity.z, (int)a->Intent.Move,
                        a->Intent.MoveTarget.x, a->Intent.MoveTarget.z, npcs.AgentPosition(*a).x, npcs.AgentPosition(*a).z,
                        npcs.AgentCorner(*a).x, npcs.AgentCorner(*a).z, a->Body.StateName(world).c_str());
        if (t < 12.0f) return;
        glm::vec3 pelvis(0.0f);
        a->Body.BoneWorld(world, "pelvis", pelvis);
        std::string states;
        for (const auto& s : m_WStates) states += " " + s.first + "@" + std::to_string(s.second).substr(0, 4);
        std::printf("[NpcTest] aptwounds: gut-shot states%s; pelvis %.0f cm up at (%.2f, %.2f)\n", states.c_str(), (pelvis.y - a->Feet.y) * 100.0f,
                    a->Feet.x, a->Feet.z);
        Check(a->Collapsed && pelvis.y - a->Feet.y < 0.3f, "aptwounds: the gut-shot one staggers and goes down, lying (pelvis " +
              std::to_string((pelvis.y - a->Feet.y) * 100.0f) + " cm)");
        shoot(*a, 1, 30.0f);
        Check(a->Dead, "aptwounds: one more round finishes him");
        // The leg shot - from in front of him or behind, as he stands (after the fall his facing is the way he lies, and a fall
        // that ended draped over the furniture lies any way).
        m_WLegFromFront = std::sin(b->Body.Yaw()) * (eye.x - b->Feet.x) + std::cos(b->Body.Yaw()) * (eye.z - b->Feet.z) > 0.0f;
        shoot(*b, 7, 20.0f);
        Check(b->Wounded && b->Prone && !b->Dead, "aptwounds: a round in the thigh puts the other down crawling");
        m_WStates.clear();
        m_WPrevFeet = b->Feet;
        step(3);
        return;
    }
    case 3: { // the fall and the crawl, past the corpse
        walls(*b, "crawler");
        if (a && a->Ragdoll && a->Ragdoll->Active()) {
            walls(*a, "gut-shot corpse");
            glm::vec3 pb;
            if (b->Body.BoneWorld(world, "pelvis", pb)) m_WCorpseNear = std::min(m_WCorpseNear, glm::length(glm::vec2(pb.x, pb.z) -
                glm::vec2(a->Ragdoll->PartPosition(0).x, a->Ragdoll->PartPosition(0).z)));
        }
        noteState(*b);
        if (b->DownAt >= 0.0f && m_WLift == 0.0f) {
            m_WLift = -1.0f; // (once: the first landing)
            std::printf("[NpcTest] aptwounds: the leg shot came from %s; he landed front %.2f up\n", m_WLegFromFront ? "in front" : "behind", b->FallFrontUp);
            Check(m_WLegFromFront && b->FallFrontUp <= npcs.RagdollSettings().WoundFaceUp,
                  "aptwounds: shot in the leg from the front, he falls forward onto his belly (front " + std::to_string(b->FallFrontUp) + " up)");
        }
        const glm::vec3 mv = b->Feet - m_WPrevFeet;
        m_WCrawlPath += glm::length(glm::vec2(mv.x, mv.z));
        m_WPrevFeet = b->Feet;
        if (t < 16.0f) return;
        std::string states;
        for (const auto& s : m_WStates) states += " " + s.first + "@" + std::to_string(s.second).substr(0, 4);
        std::printf("[NpcTest] aptwounds: crawler states%s; crawled %.1f m, now at (%.2f, %.2f); nearest the corpse %.2f m\n", states.c_str(),
                    m_WCrawlPath, b->Feet.x, b->Feet.z, m_WCorpseNear);
        Check(b->Recovered && b->CanCrawl ? m_WCrawlPath > 2.0f : true, "aptwounds: the crawler gets going (" + std::to_string(m_WCrawlPath) + " m)");
        shoot(*b, 1, 30.0f);
        Check(b->Dead, "aptwounds: and a round finishes the crawler");
        step(4);
        return;
    }
    case 4: // the crawler finished off: flat, nothing held up, nothing in a wall
        if (m_WLift < 0.0f) m_WLift = 0.0f;
        walls(*b, "crawler corpse");
        // Each part's height over what is under it - the floor, a bed, the other corpse - not over the floor at his feet: a body
        // that dies on a bed lies flat on it; a limb held up in the air is what this looks for.
        if (t > 2.0f && t < 3.5f && b->Ragdoll && b->Ragdoll->Active())
            for (int part : {0, 2, 4, 6, NpcRagdoll::kHandL, NpcRagdoll::kHandR}) {
                const glm::vec3 p = b->Ragdoll->PartPosition(part);
                float lift = p.y - b->Feet.y;
                RaycastHit hits[8];
                const float o[3] = {p.x, p.y + 0.05f, p.z}, down[3] = {0.0f, -1.0f, 0.0f};
                QueryFilter f;
                f.HitTriggers = 0;
                const int k = std::min(8, PhysicsWorld::RaycastAll(o, down, 2.0f, f, hits, 8));
                for (int i = 0; i < k; ++i)
                    if (hits[i].Entity != RootId(*b)) { lift = p.y - hits[i].Point[1]; break; }
                m_WLift = std::max(m_WLift, lift);
            }
        if (t < 3.5f) return;
        Check(m_WLift < 0.3f, "aptwounds: the finished-off crawler lies flat (highest part " + std::to_string(m_WLift * 100.0f) + " cm over what's under it)");
        step(5);
        return;
    case 5: { // both back
        int alive = 0;
        for (const auto& up : npcs.Npcs()) alive += up && !up->Dead && up->Dummy ? 1 : 0;
        if (alive < 2 && t < 12.0f) return;
        Check(alive >= 2, "aptwounds: both dummies come back (" + std::to_string(alive) + ")");
        std::printf("[NpcTest] aptwounds: deepest into a wall %.1f cm (%s)\n", m_WWorstWall * 100.0f, m_WWorstWallAt.c_str());
        Check(m_WWorstWall < 0.12f, "aptwounds: no body goes into a wall (" + std::to_string(m_WWorstWall * 100.0f) + " cm, " + m_WWorstWallAt + ")");
        // On his back: one of the new pair shot in the thigh, his fall sent over backwards.
        m_WB.clear();
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Dummy && !up->Wounded && up->Hitboxes && up->Hitboxes->Active()) m_WB = up->Name;
        b = find(m_WB);
        if (!b) { m_Done = true; return; }
        npcs.TestFallBackwards = true;
        shoot(*b, 9, 20.0f);
        step(6);
        return;
    }
    case 6: // he goes over backwards (the director's test switch) and lands on his back
        if (b && !b->Dead && b->DownAt < 0.0f && t < 6.0f) return;
        m_WGrounded = b ? b->FallFrontUp : 0.0f; // (how he landed: the front's up share)
        std::printf("[NpcTest] aptwounds: fell over backwards: landed front %.2f up (on his %s)\n", m_WGrounded,
                    m_WGrounded > npcs.RagdollSettings().WoundFaceUp ? "back" : "belly");
        Check(m_WGrounded > npcs.RagdollSettings().WoundFaceUp, "aptwounds: (the backwards fall lands him on his back: front " +
              std::to_string(m_WGrounded) + " up)");
        step(7);
        return;
    case 7: { // down on his back, he rolls over and crawls
        if (b) walls(*b, "rolled crawler");
        if (b && !b->Dead && !(b->Recovered && b->CanCrawl && b->Body.StateName(world).rfind("ProneCrawl", 0) == 0) && t < 15.0f) return;
        const bool crawls = b && !b->Dead && b->Recovered && b->CanCrawl;
        std::printf("[NpcTest] aptwounds: knocked onto his back: %s after %.1f s (roll tries %d, %s, lay %.2f up)\n", crawls ? "crawling" : "still down",
                    t, b ? b->RollTries : -1, b && b->TurnedOver ? "turned over by the blend" : "rolled himself", b ? b->FallFrontUp : 0.0f);
        Check(b && b->RollTries > 0, "aptwounds: on his back, he heaves himself over (" + std::to_string(b ? b->RollTries : 0) + " tries)");
        Check(crawls, "aptwounds: ... onto his belly, and crawls (" + std::to_string(t) + " s after landing)");
        m_Done = true;
        step(8);
        return;
    }
    default:
        return;
    }
}

void NpcTest::Escape(World& world, NpcDirector& npcs, float now) {
    npcs.HoldFire = true;
    npcs.NoLod = true;
    npcs.LogCrawl = true;
    const float dt = std::max(0.0f, now - m_EPrevNow);
    m_EPrevNow = now;
    constexpr float kRoomEdge = -3.78f, kHallEdge = -1.07f; // 301's living room ends (its arch) and the front door's line (z)
    // The doors stand open (a closed one would stop him; the navmesh runs through them) - but for escapeshut, where they stay
    // shut and he must not try them: 301's only way out is the front door, so he stays in the flat, and never crawls into it.
    const bool shut = m_Scenario == "escapeshut";
    if (!m_EDoorsOpen && now > 0.5f && !shut) {
        m_EDoorsOpen = true;
        int doors = 0;
        for (auto e : world.Registry.view<OpenableComponent>()) {
            world.Registry.get<OpenableComponent>(e).Target = 1.0f;
            ++doors;
        }
        std::printf("[NpcTest] escape: %d doors opened\n", doors);
    }
    Npc* n = nullptr;
    for (const auto& up : npcs.Npcs())
        if (up && (m_EName.empty() ? (!up->Dead && up->Dummy && !up->Weapon) : up->Name == m_EName)) // the unarmed dummy
            n = up.get();
    if (!n) {
        if (m_EName.empty() && now > 3.0f && !m_EDone) {
            m_EDone = true;
            Check(false, "escape: a dummy stands in 301's living room");
        }
        return;
    }
    if (m_EName.empty()) {
        m_EName = n->Name;
        std::printf("[NpcTest] escape: %s at (%.2f, %.2f)\n", n->Name.c_str(), n->Feet.x, n->Feet.z);
    }
    // One round in the left thigh from the player.
    if (m_EShotAt < 0.0f && now > 2.5f && n->Hitboxes && n->Hitboxes->Active()) {
        FirstPersonWeaponGameplay w;
        w.LimbMultiplier = 0.75f;
        w.FalloffStart = 1000.0f;
        w.FalloffEnd = 2000.0f;
        w.FalloffMin = 1.0f;
        w.Damage = 30.0f;
        const glm::vec3 c = n->Hitboxes->Centre(n->Body, 7);
        const glm::vec3 origin(-6.9f, c.y + 0.8f, -4.6f);
        const glm::vec3 dir = glm::normalize(c - origin);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, RootId(*n), c, origin, dir, w, &killed, &head);
        m_EShotAt = now;
        Check(n->Wounded && n->Prone && !n->Dead, "escape: a round in the thigh puts him down crawling");
    }
    if (m_EShotAt < 0.0f || m_EDone) return;
    const float t = now - m_EShotAt;
    const std::string st = n->Body.StateName(world);
    if (m_EStates.empty() || m_EStates.back().first != st) m_EStates.push_back({n->Ragdoll && !n->Dead ? "ragdoll" : st, t});
    if (m_ECrawlFrom < 0.0f && n->Recovered) m_ECrawlFrom = t;
    if (m_ECrawlFrom >= 0.0f && !n->Dead) {
        const glm::vec3 moved = n->Feet - m_EPrevFeet;
        m_EPath += glm::length(glm::vec2(moved.x, moved.z));
        if (t < kEMoveAt) m_EAwayPath += glm::length(glm::vec2(moved.x, moved.z));
        // Still (under a fifth of the crawl's pace) once the player has gone off and he has had two seconds to turn for the door.
        if (t > kEMoveAt + 2.0f && m_EInHall < 0.0f && glm::length(n->Velocity) < 0.15f) {
            m_EStill += dt;
            m_EStillRun += dt;
            m_ELongestStill = std::max(m_ELongestStill, m_EStillRun);
        } else {
            m_EStillRun = 0.0f;
        }
        if (m_EOutOfRoom < 0.0f && n->Feet.z > kRoomEdge + 0.3f) m_EOutOfRoom = t;
        if (shut && glm::length(glm::vec2(n->Feet.x + 6.9f, n->Feet.z - kHallEdge)) < 1.6f && glm::length(n->Velocity) < 0.15f) m_EAtShutDoor += dt;
        if (shut && n->HasCrawlTo && n->CrawlTo.z > kHallEdge) m_EThroughShut = std::max(m_EThroughShut, n->CrawlTo.z);
        if ((int)(t - dt) != (int)t) {
            const float yaw = n->Body.Yaw();
            const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw));
            std::printf("[NpcTest] escape %2.0f s: at (%.2f, %.2f) yaw %.0f speed %.2f, move %d to (%.2f, %.2f), room ahead %.2f behind %.2f, "
                        "stuck %.1f blocked %.1f, steer yaw %.0f, %s\n", t, n->Feet.x, n->Feet.z, glm::degrees(yaw), glm::length(n->Velocity),
                        (int)n->Intent.Move, n->Intent.MoveTarget.x, n->Intent.MoveTarget.z, npcs.FloorRoom(n->Feet, fwd, 2.0f),
                        npcs.FloorRoom(n->Feet, -fwd, 2.0f), n->CrawlStuck, n->BlockedTime,
                        glm::degrees(std::atan2(n->CrawlSteer.x, n->CrawlSteer.z)), st.c_str());
            // What the floor probe ahead meets (NpcDirector::FloorRoom's sweep).
            const float o[3] = {n->Feet.x, n->Feet.y + 0.3f, n->Feet.z}, v[3] = {fwd.x, 0.0f, fwd.z};
            QueryFilter f;
            f.HitTriggers = 0;
            RaycastHit hit;
            if (PhysicsWorld::SphereCastSolid(o, v, 0.18f, 1.8f, f, hit) && hit.Hit) {
                const auto e = (entt::entity)hit.Entity;
                const auto* nm = world.Registry.valid(e) ? world.Registry.try_get<NameComponent>(e) : nullptr;
                std::printf("[NpcTest] escape     probe ahead hits '%s' (%u) at %.2f m, point (%.2f, %.2f, %.2f); feet y %.2f\n",
                            nm ? nm->Name.c_str() : "?", hit.Entity, hit.Distance, hit.Point[0], hit.Point[1], hit.Point[2], n->Feet.y);
            }
        }
        if (m_EInHall < 0.0f && n->Feet.z > kHallEdge + 0.3f) m_EInHall = t;
    }
    m_EPrevFeet = n->Feet;
    if (t > 42.0f || n->Dead || (m_EInHall >= 0.0f && t > m_EInHall + 3.0f)) {
        m_EDone = true;
        std::string states;
        for (const auto& s : m_EStates) states += " " + s.first + "@" + std::to_string(s.second).substr(0, 4);
        std::printf("[NpcTest] escape: recovered %.1f s, out of the room %.1f s, in the corridor %.1f s after the hit; crawled %.1f m, "
                    "%.1f m of it while the player stood by the way out; still %.1f s in all after it walked off (longest %.1f s); "
                    "now at (%.2f, %.2f); states%s\n",
                    m_ECrawlFrom, m_EOutOfRoom, m_EInHall, m_EPath, m_EAwayPath, m_EStill, m_ELongestStill, n->Feet.x, n->Feet.z, states.c_str());
        Check(!n->Dead, "escape: he is still alive (a crawler doesn't bleed out)");
        if (shut) {
            std::printf("[NpcTest] escapeshut: %.1f s lying still at the shut front door; in the corridor %s\n", m_EAtShutDoor, m_EInHall >= 0.0f ? "yes" : "no");
            Check(m_EInHall < 0.0f, "escapeshut: the front door shut, he doesn't get through it");
            // (Where he may go is the flat: he picks somewhere in it - never past the shut door, though the navmesh runs through it.)
            Check(m_EThroughShut < -1e8f, "escapeshut: he never heads for anywhere past the shut door (" + std::to_string(m_EThroughShut) + ")");
            m_Done = true;
            return;
        }
        Check(m_EAwayPath > 1.5f, "escape: with the player by the way out he crawls away, deeper in (" + std::to_string(m_EAwayPath) + " m)");
        Check(m_EOutOfRoom >= 0.0f, "escape: he crawls out of the living room he was shot in (through the arch)");
        Check(m_EInHall >= 0.0f, "escape: ... and out of the front door into the corridor");
        Check(m_ELongestStill < 1.0f, "escape: he doesn't stop on the way (longest still " + std::to_string(m_ELongestStill) + " s)");
        int stops = 0;
        for (const auto& s : m_EStates) stops += s.first == "ProneCrawlStop" ? 1 : 0;
        std::printf("[NpcTest] escape: the crawl stopped %d times on the way\n", stops);
        Check(stops <= 2, "escape: the crawl doesn't stop-start against the walls and doorframes (" + std::to_string(stops) + " stops)");
        m_Done = true;
    }
}

// --- aptfight: the squad as fighters in Apartment Hallway ------------------------------------------------------------
namespace {
// The rooms round 301 (x0, z0, x1, z1: wall centre lines), for which room a soldier is in and when it passes a doorway.
struct AptRoom { const char* Name; float X0, Z0, X1, Z1; };
const AptRoom kAptRooms[] = {
    {"301 living", -10.6f, -9.675f, -5.6f, -3.775f},
    {"301 entry", -8.2f, -3.775f, -5.6f, -1.075f},
    {"301 bath", -10.6f, -3.775f, -8.2f, -1.075f},
    {"corridor", -10.6f, -1.075f, 10.6f, 1.075f},
};
// 301's openings: a wall along x (at z = Line) or along z (at x = Line), the opening's centre along it and half its width.
struct AptOpening { const char* Name; bool AlongX; float Line, Centre, Half; };
const AptOpening kAptOpenings[] = {
    {"301 arch", true, -3.775f, -6.9f, 0.8f},
    {"301 front door", true, -1.075f, -6.9f, 0.46f},
    {"301 bathroom door", false, -8.2f, -2.425f, 0.46f},
};
int AptRoomOf(const glm::vec3& p) {
    for (int i = 0; i < (int)(sizeof(kAptRooms) / sizeof(kAptRooms[0])); ++i) {
        const AptRoom& r = kAptRooms[i];
        if (p.x > r.X0 + 0.2f && p.x < r.X1 - 0.2f && p.z > r.Z0 + 0.2f && p.z < r.Z1 - 0.2f) return i;
    }
    return -1;
}
const AptOpening* AptDoorway(const glm::vec3& p) {
    for (const AptOpening& o : kAptOpenings) {
        const float across = o.AlongX ? p.z - o.Line : p.x - o.Line, along = o.AlongX ? p.x - o.Centre : p.z - o.Centre;
        if (std::abs(across) < 0.45f && std::abs(along) < o.Half) return &o;
    }
    return nullptr;
}
// The solid world (walls, frames, the doors; not characters or ragdolls) between two points.
bool AptSolidBetween(const glm::vec3& a, const glm::vec3& b, RaycastHit* out = nullptr) {
    glm::vec3 d = b - a;
    const float len = glm::length(d);
    if (len < 1e-3f) return false;
    d /= len;
    const float o[3] = {a.x, a.y, a.z}, v[3] = {d.x, d.y, d.z};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    const bool blocked = PhysicsWorld::RaycastSolid(o, v, len, f, hit) && hit.Hit;
    if (out) *out = hit;
    return blocked;
}
std::string AptHitName(World& world, const RaycastHit& hit) {
    const auto e = (entt::entity)hit.Entity;
    const auto* nm = world.Registry.valid(e) ? world.Registry.try_get<NameComponent>(e) : nullptr;
    return nm ? nm->Name : std::string("?");
}
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace

NpcTest::AptTrack& NpcTest::ATrack(const std::string& name) {
    for (AptTrack& t : m_ATrack)
        if (t.Name == name) return t;
    m_ATrack.emplace_back();
    m_ATrack.back().Name = name;
    return m_ATrack.back();
}

void NpcTest::AptFight(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player, float now) {
    npcs.HoldFire = false;
    npcs.NoLod = true;           // every soldier posed every frame: a held pose would read as a pop
    npcs.TrackDeathPop = true;
    npcs.LogCover = true;
    const float dt = std::max(0.0f, now - m_EPrevNow);
    m_EPrevNow = now;
    char msg[512];
    auto finding = [&](const char* text) { // the first few are printed (each counted regardless)
        if (m_AFindingsLogged++ < 80) std::printf("[NpcTest] aptfight %5.1f: %s\n", now, text);
    };
    // The doors stand open (the navmesh runs through them; a shut one is a wall to the soldiers).
    if (!m_ADoorsOpen && now > 0.5f) {
        m_ADoorsOpen = true;
        for (auto e : world.Registry.view<OpenableComponent>()) world.Registry.get<OpenableComponent>(e).Target = 1.0f;
    }
    // Can a soldier get through each opening at all (the navmesh, shrunk by his capsule's radius, from a pace one side to a pace
    // the other)? A path much longer than the straight line, or none, means it doesn't fit.
    if (!m_ADoorPaths && now > 3.0f) {
        m_ADoorPaths = true;
        for (const AptOpening& o : kAptOpenings) {
            const glm::vec3 across = o.AlongX ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 mid = o.AlongX ? glm::vec3(o.Centre, 0.0f, o.Line) : glm::vec3(o.Line, 0.0f, o.Centre);
            glm::vec3 a, b;
            const bool on = npcs.Nav().Closest(mid - across * 0.8f, a, glm::vec3(0.3f, 1.0f, 0.3f)) &&
                            npcs.Nav().Closest(mid + across * 0.8f, b, glm::vec3(0.3f, 1.0f, 0.3f));
            const float path = on ? npcs.Nav().PathLength(a, b) : -1.0f;
            std::printf("[NpcTest] aptfight: %s (%.2f m wide): %s, path %.2f m for %.2f m straight\n", o.Name, 2.0f * o.Half,
                        on ? "both sides on the navmesh" : "a side off the navmesh", path, on ? glm::length(b - a) : 0.0f);
        }
    }
    // Spawned before the switch took (it shouldn't: Drive sets it before the first Think): down once, back as fighters.
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && up->Dummy) {
            ++m_ADummiesCulled;
            npcs.Kill(world, *up, glm::vec3(0.0f, 0.0f, 1.0f), up->Feet, 0.0f);
        }
    glm::vec3 playerFeet;
    PhysicsWorld::GetCharacterFootPosition(&playerFeet.x);
    const glm::vec3 eye = player.Cam.Position;
    auto findNpc = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    auto flat = [](const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); };
    // A round from the player's eye into hitbox `part` of `n` (the scripted hits: the same path as his rifle's).
    auto shoot = [&](Npc& n, int part, float damage) {
        FirstPersonWeaponGameplay w;
        w.LimbMultiplier = 0.75f;
        w.FalloffStart = 1000.0f;
        w.FalloffEnd = 2000.0f;
        w.FalloffMin = 1.0f;
        w.Damage = damage;
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, RootId(n), c, eye, glm::normalize(c - eye), w, &killed, &head);
    };

    // --- every soldier, every frame ---
    int alive = 0;
    for (const auto& up : npcs.Npcs()) {
        if (!up) continue;
        Npc& n = *up;
        AptTrack& t = ATrack(n.Name);
        if (n.Dead) {
            if (!t.Dead) {
                t.Dead = true;
                ++m_ADeaths;
                std::printf("[NpcTest] aptfight %5.1f: %s killed at (%.2f, %.2f)%s, %d hits\n", now, n.Name.c_str(), n.Feet.x, n.Feet.z,
                            n.Prone ? " on the floor" : "", n.Hits);
            }
            // The corpse at rest in a small room: no part through a wall or the floor (3 s on, once).
            if (n.Ragdoll && now - n.DiedAt > 3.0f &&
                std::find(m_ACorpsesChecked.begin(), m_ACorpsesChecked.end(), n.Name) == m_ACorpsesChecked.end()) {
                m_ACorpsesChecked.push_back(n.Name);
                if (n.DeathPop >= 0.0f) m_AWorstDeathPop = std::max(m_AWorstDeathPop, n.DeathPop);
                const glm::vec3 pelvis = n.Ragdoll->PartPosition(0);
                for (int part = 1; part < NpcRagdoll::kRagParts; ++part) {
                    const glm::vec3 at = n.Ragdoll->PartPosition(part);
                    RaycastHit hit, floor;
                    // (Under the floor: the floor straight below the pelvis is above the part's centre.)
                    const bool under = AptSolidBetween(pelvis, glm::vec3(pelvis.x, pelvis.y - 2.0f, pelvis.z), &floor) && at.y < floor.Point[1] - 0.02f;
                    if (AptSolidBetween(pelvis, at, &hit) || under) {
                        ++m_ACorpseInWall;
                        std::snprintf(msg, sizeof msg, "%s's corpse: part %d at (%.2f, %.2f, %.2f) through '%s' from the pelvis (%.2f, %.2f, %.2f)",
                                      n.Name.c_str(), part, at.x, at.y, at.z, hit.Hit ? AptHitName(world, hit).c_str() : "the floor", pelvis.x,
                                      pelvis.y, pelvis.z);
                        finding(msg);
                    }
                }
                std::printf("[NpcTest] aptfight %5.1f: %s's corpse at rest: pelvis (%.2f, %.2f, %.2f) %s, death pop %.1f cm\n", now, n.Name.c_str(),
                            pelvis.x, pelvis.y, pelvis.z, n.Ragdoll->Asleep() ? "asleep" : "moving", n.DeathPop * 100.0f);
            }
            continue;
        }
        ++alive;
        if (n.Mem.Known && m_FirstKnown < 0.0f) m_FirstKnown = now;
        if (n.ShotsFired > 0 && m_FirstShot < 0.0f) m_FirstShot = now;
        if (n.Doing == Behaviour::CoverFight && m_FirstCover < 0.0f) m_FirstCover = now;
        {
            const std::string b = BehaviourName(n.Doing);
            if (std::find(m_BehavioursSeen.begin(), m_BehavioursSeen.end(), b) == m_BehavioursSeen.end()) m_BehavioursSeen.push_back(b);
        }
        if (!Finite(n.Feet) || !Finite(n.Eye) || !std::isfinite(n.AimYaw) || !std::isfinite(n.AimPitch)) {
            if (!m_DevKilled) {
                std::snprintf(msg, sizeof msg, "%s went NaN (feet %f %f %f)", n.Name.c_str(), n.Feet.x, n.Feet.y, n.Feet.z);
                finding(msg);
            }
            m_DevKilled = true; // (reused here: a NaN was seen)
        }
        const bool down = n.Wounded || n.Ragdoll; // Jacob's wounded work is measured by its own scenarios
        // Stuck: asked to go somewhere (more than a stride off) and not 35 cm on in 3 s.
        const bool wantsMove = n.Intent.Move && !down && flat(n.Intent.MoveTarget, n.Feet) > 0.8f;
        if (wantsMove) {
            if (t.StuckFrom < 0.0f || flat(n.Feet, t.StuckAt) > 0.35f) {
                t.StuckFrom = now;
                t.StuckAt = n.Feet;
            } else if (now - t.StuckFrom > 3.0f) {
                ++m_AStuck;
                int corners = 0;
                const int ts = npcs.Crowd().TargetState(n.Agent, &corners);
                const glm::vec3 corner = npcs.Crowd().NextCorner(n.Agent), cv = npcs.Crowd().Velocity(n.Agent);
                std::snprintf(msg, sizeof msg, "%s stuck 3 s at (%.2f, %.2f) going to (%.2f, %.2f), %s ph%d, blocked %.1f, speed %.2f, crouched %d; "
                              "crowd: target state %d, %d corners, next (%.2f, %.2f), wants %.2f m/s, goal (%.2f, %.2f) set %.1f s ago",
                              n.Name.c_str(), n.Feet.x, n.Feet.z, n.Intent.MoveTarget.x, n.Intent.MoveTarget.z, BehaviourName(n.Doing), n.Phase,
                              n.BlockedTime, glm::length(n.Velocity), (int)n.Crouched, ts, corners, corner.x, corner.z, glm::length(cv), n.Goal.x,
                              n.Goal.z, now - n.GoalSetAt);
                finding(msg);
                t.StuckFrom = now;
                t.StuckAt = n.Feet;
            }
        } else {
            t.StuckFrom = -1.0f;
        }
        // Through a wall: the step since last frame crosses the solid world (knee and chest height) ...
        if (t.Have && n.Cct != PhysicsWorld::kNoCharacter) {
            t.Path += flat(n.Feet, t.PrevFeet);
            for (float h : {0.5f, 1.0f}) {
                RaycastHit hit;
                if (AptSolidBetween(t.PrevFeet + glm::vec3(0.0f, h, 0.0f), n.Feet + glm::vec3(0.0f, h, 0.0f), &hit)) {
                    ++m_AWallSteps;
                    std::snprintf(msg, sizeof msg, "%s stepped through '%s' at %.1f m: (%.2f, %.2f) -> (%.2f, %.2f)", n.Name.c_str(),
                                  AptHitName(world, hit).c_str(), h, t.PrevFeet.x, t.PrevFeet.z, n.Feet.x, n.Feet.z);
                    finding(msg);
                    break;
                }
            }
        }
        // ... the capsule 10 cm or more into one ...
        if (n.Cct != PhysicsWorld::kNoCharacter && !down) {
            bool in = false;
            std::string what;
            for (int k = 0; k < 8 && !in; ++k) {
                const float a = (float)k * 0.785398f;
                const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
                for (float h : {0.5f, 1.0f}) {
                    RaycastHit hit;
                    const glm::vec3 c = n.Feet + glm::vec3(0.0f, h, 0.0f);
                    if (AptSolidBetween(c, c + dir * 0.2f, &hit)) { in = true; what = AptHitName(world, hit); break; }
                }
            }
            if (in && !t.CapsuleIn) {
                ++m_ACapsuleInWall;
                std::snprintf(msg, sizeof msg, "%s's capsule is in '%s' at (%.2f, %.2f), %s", n.Name.c_str(), what.c_str(), n.Feet.x, n.Feet.z,
                              BehaviourName(n.Doing));
                finding(msg);
            }
            t.CapsuleIn = in;
        }
        // ... or off the walkable mesh (it is eroded by the capsule's radius: the centre never needs to leave it).
        if (npcs.Nav().Valid() && !down) {
            glm::vec3 snapped;
            const float off = npcs.Nav().Closest(n.Feet, snapped, glm::vec3(2.0f, 1.0f, 2.0f)) ? flat(snapped, n.Feet) : 9.0f;
            m_AWorstOffNav = std::max(m_AWorstOffNav, off);
            const bool offNav = off > 0.25f;
            if (offNav && !t.OffNav) {
                ++m_AOffNav;
                std::snprintf(msg, sizeof msg, "%s is %.2f m off the navmesh at (%.2f, %.2f)", n.Name.c_str(), off, n.Feet.x, n.Feet.z);
                finding(msg);
            }
            t.OffNav = offNav;
        }
        // The gun's muzzle out through a wall (the head to the muzzle crosses it).
        if (n.Weapon && n.Weapon->IsActive() && !down) {
            glm::vec3 fp, muzzle, bore;
            RaycastHit hit;
            const bool in = n.Weapon->MuzzleFrames(fp, muzzle, bore) && AptSolidBetween(n.Eye, muzzle, &hit);
            if (in) {
                t.GunInWall += dt;
                m_AWorstGunInWall = std::max(m_AWorstGunInWall, t.GunInWall);
                if (!t.GunIn) {
                    ++m_AGunInWallEvents;
                    std::snprintf(msg, sizeof msg, "%s's muzzle through '%s' (eye (%.2f, %.2f, %.2f), muzzle (%.2f, %.2f, %.2f)), %s ph%d aim %d",
                                  n.Name.c_str(), AptHitName(world, hit).c_str(), n.Eye.x, n.Eye.y, n.Eye.z, muzzle.x, muzzle.y, muzzle.z,
                                  BehaviourName(n.Doing), n.Phase, (int)n.Intent.Aim);
                    finding(msg);
                }
            } else {
                t.GunInWall = 0.0f;
            }
            t.GunIn = in;
        }
        // Rooms and doorways: a pass from one of 301's rooms to the next; standing about in an opening.
        if (const int room = AptRoomOf(n.Feet); room >= 0) {
            if (t.Room >= 0 && room != t.Room) {
                ++m_ADoorPasses;
                std::printf("[NpcTest] aptfight %5.1f: %s %s -> %s (%s)\n", now, n.Name.c_str(), kAptRooms[t.Room].Name, kAptRooms[room].Name,
                            BehaviourName(n.Doing));
            }
            t.Room = room;
        }
        if (const AptOpening* o = AptDoorway(n.Feet); o && !down && glm::length(n.Velocity) < 0.2f) {
            t.DoorStill += dt;
            if (t.DoorStill > 3.0f && t.DoorStill - dt <= 3.0f) {
                ++m_ADoorLingers;
                std::snprintf(msg, sizeof msg, "%s has stood 3 s in %s at (%.2f, %.2f), %s ph%d", n.Name.c_str(), o->Name, n.Feet.x, n.Feet.z,
                              BehaviourName(n.Doing), n.Phase);
                finding(msg);
            }
            m_AWorstDoorStill = std::max(m_AWorstDoorStill, t.DoorStill);
        } else {
            t.DoorStill = 0.0f;
        }
        // Reloads: begun, finished with a full magazine in time, and the gun fired again after.
        if (n.Weapon && n.Weapon->IsActive()) {
            const int ammo = n.Weapon->Ammo();
            if (n.Reloading && !t.Reloading) {
                ++m_AReloads;
                t.ReloadAmmo = ammo;
                t.ReloadFrom = now;
            } else if (!n.Reloading && t.Reloading && t.ReloadFrom >= 0.0f) {
                const float took = now - t.ReloadFrom;
                m_AWorstReload = std::max(m_AWorstReload, took);
                if (ammo > t.ReloadAmmo && took < 6.0f) {
                    ++m_AReloadsDone;
                    t.ReloadedAt = now;
                    t.ShotsAtReload = n.ShotsFired;
                } else {
                    ++m_AReloadsBad;
                    std::snprintf(msg, sizeof msg, "%s's reload ended after %.1f s with %d rounds (began with %d), state %s", n.Name.c_str(), took,
                                  ammo, t.ReloadAmmo, n.Weapon->CurrentState().c_str());
                    finding(msg);
                }
                t.ReloadFrom = -1.0f;
            } else if (n.Reloading && t.ReloadFrom >= 0.0f && now - t.ReloadFrom > 6.0f) {
                ++m_AReloadsBad;
                std::snprintf(msg, sizeof msg, "%s has been reloading 6 s (state %s, ammo %d)", n.Name.c_str(), n.Weapon->CurrentState().c_str(), ammo);
                finding(msg);
                t.ReloadFrom = -1.0f;
            }
            if (t.ReloadedAt >= 0.0f && n.ShotsFired > t.ShotsAtReload) {
                ++m_ARefireAfterReload;
                t.ReloadedAt = -1.0f;
            }
            t.Reloading = n.Reloading;
        }
        // Pose pops: a hand, foot or the head jumping 20 cm off the body in one frame (the body's own move taken out).
        {
            static const char* kPopBones[5] = {"head", "hand_l", "hand_r", "foot_l", "foot_r"};
            glm::vec3 rel[5];
            bool all = true;
            for (int k = 0; k < 5; ++k) {
                glm::vec3 b(0.0f);
                all = n.Body.BoneWorld(world, kPopBones[k], b) && all;
                rel[k] = b - n.Feet;
                if (!Finite(b)) {
                    if (!m_DevKilled) {
                        std::snprintf(msg, sizeof msg, "%s's %s went NaN", n.Name.c_str(), kPopBones[k]);
                        finding(msg);
                    }
                    m_DevKilled = true;
                }
            }
            // (Not its first moments: a replacement is put in place out of sight, its first frame from the spawn's placement.)
            if (all && t.HaveRel && !down && dt > 0.0f && now - n.SpawnedAt > 0.25f) {
                int worst = 0;
                for (int k = 1; k < 5; ++k)
                    if (glm::length(rel[k] - t.PrevRel[k]) > glm::length(rel[worst] - t.PrevRel[worst])) worst = k;
                const float jump = glm::length(rel[worst] - t.PrevRel[worst]);
                if (jump > m_AWorstPop) {
                    m_AWorstPop = jump;
                    m_AWorstPopAt = n.Name + " " + kPopBones[worst] + " (" + n.Body.StateName(world) + ", " + BehaviourName(n.Doing) + ")";
                }
                if (jump > 0.2f) {
                    ++m_APops;
                    const glm::vec3 a = t.PrevRel[worst], b = rel[worst];
                    std::snprintf(msg, sizeof msg, "%s's %s jumped %.0f cm in a frame, off the feet (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) (%s, gun %s, "
                                  "%s ph%d, crouched %d, speed %.2f, %.1f s after spawning; feet y %.2f (was %.2f), body root off them %.2f m, "
                                  "body yaw %.0f -> %.0f deg, last hit %.2f s ago)",
                                  n.Name.c_str(), kPopBones[worst], jump * 100.0f, a.x, a.y, a.z, b.x, b.y, b.z, n.Body.StateName(world).c_str(),
                                  n.Weapon && n.Weapon->IsActive() ? n.Weapon->CurrentState().c_str() : "-", BehaviourName(n.Doing), n.Phase,
                                  (int)n.Crouched, glm::length(n.Velocity), now - n.SpawnedAt, n.Feet.y, t.PrevFeet.y,
                                  glm::length(n.Body.Feet() - n.Feet), t.PrevYaw, glm::degrees(n.Body.Yaw()),
                                  now - n.LastHurt);
                    finding(msg);
                    if (m_AFindingsLogged <= 80)
                        std::printf("[NpcTest]     animator was %s\n[NpcTest]     animator now %s\n[NpcTest]     gun was %s\n[NpcTest]     gun now "
                                    "tuck %.2f (tuck yaw %.0f), camera yaw %.0f pitch %.0f, aim yaw %.0f\n", t.PrevAnim.c_str(),
                                    n.Body.AnimDebug(world).c_str(), t.PrevGun.c_str(), n.GunTuck, n.TuckYaw, n.WeaponCam.Yaw, n.WeaponCam.Pitch, n.AimYaw);
                }
            }
            t.PrevAnim = n.Body.AnimDebug(world);
            {
                char g[128];
                std::snprintf(g, sizeof g, "tuck %.2f (tuck yaw %.0f), camera yaw %.0f pitch %.0f, aim yaw %.0f", n.GunTuck, n.TuckYaw,
                              n.WeaponCam.Yaw, n.WeaponCam.Pitch, n.AimYaw);
                t.PrevGun = g;
            }
            t.HaveRel = all && !down;
            t.PrevYaw = glm::degrees(n.Body.Yaw());
            for (int k = 0; k < 5; ++k) t.PrevRel[k] = rel[k];
        }
        t.Have = n.Cct != PhysicsWorld::kNoCharacter;
        t.PrevFeet = n.Feet;
        t.Wounded = n.Wounded;
    }
    m_MaxAlive = std::max(m_MaxAlive, alive);
    // Rounds on the player (god mode: each shows as a hit indicator, a new arc or one refreshed - its age back to 0): frames hit.
    {
        float youngest = 1e9f;
        for (const auto& ind : vitals.Indicators()) youngest = std::min(youngest, ind.Age);
        if ((int)vitals.Indicators().size() > m_AIndicators || youngest < m_AYoungest - 1e-5f) {
            ++m_AHitsOnPlayer;
            if (m_FirstDamage < 0.0f) m_FirstDamage = now;
        }
        m_AIndicators = (int)vitals.Indicators().size();
        m_AYoungest = youngest;
    }

    // --- the player fights back ---
    // A heavy round in a forearm: a stagger (he should be back on the gun after it).
    if (now >= kAptHitAt && m_AStaggered.empty()) {
        Npc* best = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && !up->Wounded && up->Hitboxes && up->Hitboxes->Active() &&
                (!best || flat(up->Feet, playerFeet) < flat(best->Feet, playerFeet)))
                best = up.get();
        if (best) {
            m_AStaggered = best->Name;
            m_AStaggerAt = now;
            m_AStaggerShotsBefore = best->ShotsFired;
            shoot(*best, 4, 60.0f); // 45 after the limb multiplier: over Heavy Hit Damage
            m_AStaggerSeen = best->StaggerUntil > now && !best->Dead;
            std::printf("[NpcTest] aptfight %5.1f: %s staggered by a forearm round (hp %.0f, stagger until %.2f, %s)\n", now, best->Name.c_str(),
                        best->Health, best->StaggerUntil, BehaviourName(best->Doing));
        }
    }
    if (!m_AStaggered.empty())
        if (const Npc* s = findNpc(m_AStaggered); s && !s->Dead && now > s->StaggerUntil) m_AStaggerShotsAfter = s->ShotsFired;
    // A thigh: down on his belly, crawling off, while the other one fights on. (Landed on his back he rolls himself over - a few
    // heaves, Wound Roll Tries 1.8 s apart, then the blend turns him over: still on his back well after those, he's given up on and
    // the next soldier up is shot instead, while there's time left to watch a crawl.)
    if (!m_ACrawler.empty() && now < kAptLegUntil)
        if (const Npc* c = findNpc(m_ACrawler); c && !c->Dead && c->DownAt >= 0.0f && !c->CanCrawl && now - c->DownAt > 6.5f) {
            ++m_ACrawlsOnBack;
            std::printf("[NpcTest] aptfight %5.1f: %s landed on his back: he lies there; the next one is shot in the thigh\n", now, c->Name.c_str());
            m_ACrawler.clear();
            m_ACrawlerSpared.clear();
        }
    if (now >= kAptLegAt && m_ACrawler.empty() && now < kAptLegUntil) {
        Npc* pick = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && !up->Wounded && up->Hitboxes && up->Hitboxes->Active() && up->Name != m_AStaggered) pick = up.get();
        if (!pick)
            for (const auto& up : npcs.Npcs())
                if (up && !up->Dead && !up->Wounded && up->Hitboxes && up->Hitboxes->Active()) pick = up.get();
        if (pick) {
            shoot(*pick, 7, 30.0f);
            m_ACrawler = m_ACrawlerSpared = pick->Name;
            m_ACrawlAt = now;
            m_ACrawlStartDist = m_ACrawlBestDist = flat(pick->Feet, playerFeet);
            // (A squadmate still on his feet: one down on the floor himself has let his gun go. Picked again with each new crawler -
            // the last one's squadmate may be this crawler.)
            m_AOther.clear();
            m_AOtherShotsAtCrawl = -1;
            m_AOtherShotsAfterCrawl = 0;
            for (const auto& up : npcs.Npcs())
                if (up && !up->Dead && !up->Wounded && up.get() != pick) { m_AOther = up->Name; m_AOtherShotsAtCrawl = up->ShotsFired; }
            std::printf("[NpcTest] aptfight %5.1f: %s shot in the thigh: wounded %d prone %d, %.1f m from the player; %s fights on (%d rounds so far)\n",
                        now, pick->Name.c_str(), (int)pick->Wounded, (int)pick->Prone, m_ACrawlStartDist, m_AOther.c_str(), m_AOtherShotsAtCrawl);
            Check(pick->Wounded && pick->Prone && !pick->Dead, "aptfight: a round in the thigh puts " + pick->Name + " down crawling");
        }
    }
    if (!m_ACrawler.empty() && now < kAptSpareUntil) {
        if (const Npc* c = findNpc(m_ACrawler); c && !c->Dead) m_ACrawlBestDist = std::max(m_ACrawlBestDist, flat(c->Feet, playerFeet));
        if (const Npc* o = findNpc(m_AOther); o && !o->Dead && m_AOtherShotsAtCrawl >= 0)
            m_AOtherShotsAfterCrawl = o->ShotsFired - m_AOtherShotsAtCrawl;
    }
    m_AFightOn = now >= kAptFireAt;

    // --- the log: every soldier, every two seconds ---
    if (now >= m_NextLog) {
        m_NextLog = now + 2.0f;
        std::printf("[NpcTest] t=%5.1f player (%.2f, %.2f) hp=%3.0f hits on him %d  shooters=%d  think=%.2fms late=%.2fms  target='%s' shots=%d\n", now,
                    playerFeet.x, playerFeet.z, vitals.Health(), m_AHitsOnPlayer, npcs.ShootersNow(), npcs.LastThinkMs(), npcs.LastLateMs(),
                    m_Target.c_str(), m_PlayerShots);
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead) continue;
            const Npc& n = *up;
            const int ammo = n.Weapon && n.Weapon->IsActive() ? n.Weapon->Ammo() : -1;
            const int room = AptRoomOf(n.Feet);
            std::printf("[NpcTest]   %-10s %-11s ph%-2d %-10s hp%3.0f sup%.2f ammo%2d %s%s%s pos(%6.2f,%6.2f) %-10s v%3.1f blk%.1f cover%d %s anim=%s gun=%s\n",
                        n.Name.c_str(), BehaviourName(n.Doing), n.Phase, RoleName(n.Role), n.Health, n.Suppression, ammo, n.Mem.Known ? "K" : "-",
                        n.Mem.Visible ? "V" : "-", n.HasAttackToken ? "A" : "-", n.Feet.x, n.Feet.z, room >= 0 ? kAptRooms[room].Name : "(between)",
                        glm::length(n.Velocity), n.BlockedTime, n.Cover, n.Why.c_str(), n.Body.StateName(world).c_str(),
                        n.Weapon && n.Weapon->IsActive() ? n.Weapon->CurrentState().c_str() : "-");
        }
    }

    // --- the verdict ---
    if (now < m_Duration || m_Done) return;
    m_Done = true;
    std::string seen;
    for (const auto& b : m_BehavioursSeen) seen += b + ", ";
    std::printf("[NpcTest] aptfight: behaviours seen: %s\n", seen.c_str());
    std::printf("[NpcTest] aptfight: first known %.1f s, first shot %.1f s, first hit on the player %.1f s (%d hits), first in cover %.1f s\n",
                m_FirstKnown, m_FirstShot, m_FirstDamage, m_AHitsOnPlayer, m_FirstCover);
    std::printf("[NpcTest] aptfight: stuck %d, steps through walls %d, capsule in a wall %d, off the navmesh %d (worst %.2f m), gun through a wall %d "
                "(longest %.2f s)\n", m_AStuck, m_AWallSteps, m_ACapsuleInWall, m_AOffNav, m_AWorstOffNav, m_AGunInWallEvents, m_AWorstGunInWall);
    std::printf("[NpcTest] aptfight: doorway passes %d, 3 s+ in a doorway %d (longest %.1f s); reloads %d (done %d, broken %d, longest %.1f s), "
                "fired again after %d\n", m_ADoorPasses, m_ADoorLingers, m_AWorstDoorStill, m_AReloads, m_AReloadsDone, m_AReloadsBad, m_AWorstReload,
                m_ARefireAfterReload);
    std::printf("[NpcTest] aptfight: pose pops %d (worst %.1f cm: %s); deaths %d (worst death pop %.1f cm), corpse parts through walls %d; "
                "soldiers %zu; player shots %d; dummies culled %d\n", m_APops, m_AWorstPop * 100.0f, m_AWorstPopAt.c_str(), m_ADeaths,
                m_AWorstDeathPop * 100.0f, m_ACorpseInWall, m_ATrack.size(), m_PlayerShots, m_ADummiesCulled);
    std::printf("[NpcTest] aptfight: stagger %s (seen %d, rounds %d -> %d); crawler %s (%.1f -> %.1f m from the player; %d before him landed on "
                "their backs), %s fired %d rounds meanwhile\n", m_AStaggered.c_str(), (int)m_AStaggerSeen, m_AStaggerShotsBefore, m_AStaggerShotsAfter,
                m_ACrawler.c_str(), m_ACrawlStartDist, m_ACrawlBestDist, m_ACrawlsOnBack, m_AOther.c_str(), m_AOtherShotsAfterCrawl);
    for (const AptTrack& t : m_ATrack) std::printf("[NpcTest] aptfight:   %s walked %.1f m\n", t.Name.c_str(), t.Path);
    PrintCosts(npcs);
    std::fflush(stdout);
    Check(m_ADummiesCulled == 0, "aptfight: the soldiers spawned as fighters, not dummies");
    Check(m_MaxAlive >= 2, "aptfight: two soldiers up at once (" + std::to_string(m_MaxAlive) + ")");
    Check(m_FirstKnown >= 0.0f && m_FirstKnown < 10.0f, "aptfight: the squad noticed the player within 10 s");
    Check(m_FirstShot >= 0.0f && m_FirstShot < 15.0f, "aptfight: the squad opened fire within 15 s");
    Check(m_AHitsOnPlayer > 0, "aptfight: their rounds reached the player (" + std::to_string(m_AHitsOnPlayer) + ")");
    Check(m_FirstCover >= 0.0f, "aptfight: a soldier took cover and fought from it (first at " + std::to_string(m_FirstCover) + " s)");
    Check(m_AStuck == 0, "aptfight: nobody stuck 3 s on the way somewhere (" + std::to_string(m_AStuck) + ")");
    Check(m_AWallSteps == 0, "aptfight: no step through a wall or doorframe (" + std::to_string(m_AWallSteps) + ")");
    Check(m_ACapsuleInWall == 0, "aptfight: no capsule 10 cm into a wall (" + std::to_string(m_ACapsuleInWall) + ")");
    Check(m_AOffNav == 0, "aptfight: everyone on the navmesh (worst " + std::to_string(m_AWorstOffNav) + " m off)");
    Check(m_AWorstGunInWall < 0.25f, "aptfight: no gun through a wall over a quarter second (" + std::to_string(m_AGunInWallEvents) +
                                         " times, longest " + std::to_string(m_AWorstGunInWall) + " s)");
    Check(m_ADoorPasses > 0, "aptfight: they went through 301's doorways (" + std::to_string(m_ADoorPasses) + " passes)");
    Check(m_ADoorLingers == 0, "aptfight: nobody stood about in a doorway (longest " + std::to_string(m_AWorstDoorStill) + " s)");
    Check(m_AReloadsDone > 0 && m_AReloadsBad == 0, "aptfight: reloads finished full (" + std::to_string(m_AReloadsDone) + " of " +
                                                       std::to_string(m_AReloads) + ", " + std::to_string(m_AReloadsBad) + " broken)");
    Check(m_ARefireAfterReload > 0, "aptfight: a soldier fired again after reloading (" + std::to_string(m_ARefireAfterReload) + ")");
    Check(!m_DevKilled, "aptfight: no NaN in a soldier's position, aim or pose");
    Check(m_APops == 0, "aptfight: no pose pop over 20 cm in a frame (worst " + std::to_string(m_AWorstPop * 100.0f) + " cm)");
    Check(!m_AStaggered.empty() && m_AStaggerSeen, "aptfight: a heavy round staggered " + m_AStaggered);
    Check(m_AStaggerShotsAfter > m_AStaggerShotsBefore, "aptfight: ... and he fought on after it (" + std::to_string(m_AStaggerShotsBefore) +
                                                            " -> " + std::to_string(m_AStaggerShotsAfter) + " rounds)");
    Check(!m_ACrawler.empty() && m_ACrawlBestDist > m_ACrawlStartDist + 1.0f,
          "aptfight: the leg-shot crawler got away from the player (" + std::to_string(m_ACrawlStartDist) + " -> " + std::to_string(m_ACrawlBestDist) + " m)");
    Check(m_AOtherShotsAfterCrawl > 0, "aptfight: the other soldier kept fighting while he crawled (" +
                                           (m_AOther.empty() ? std::string("no squadmate on his feet") : m_AOther + ", " + std::to_string(m_AOtherShotsAfterCrawl) + " rounds") + ")");
    Check(m_ADeaths >= 2, "aptfight: the player killed soldiers (" + std::to_string(m_ADeaths) + ")");
    Check(m_ATrack.size() > 2, "aptfight: the dead were replaced (" + std::to_string(m_ATrack.size()) + " soldiers in all)");
    Check(m_AWorstDeathPop <= 0.15f, "aptfight: no death pose pop over 15 cm (" + std::to_string(m_AWorstDeathPop * 100.0f) + " cm)");
    Check(m_ACorpseInWall == 0, "aptfight: no corpse through a wall or the floor (" + std::to_string(m_ACorpseInWall) + " parts)");
    const float cost = npcs.ThinkCost().Avg() + npcs.LateCost().Avg();
    Check(cost < 2.0f, "aptfight: the AI and the bodies' late pose under 2 ms a frame on average (" + std::to_string(cost) + " ms)");
}

void NpcTest::Deaths(World& world, NpcDirector& npcs, float now) {
    (void)world;
    npcs.HoldFire = true;
    npcs.NoLod = true;
    npcs.TrackDeathPop = true;
    // The kills die outright; the wound steps (4 on) always go down.
    npcs.SetWoundChance(m_DStep >= 4 ? 1.0f : 0.0f);
    npcs.SetLegDownChance(m_DStep >= 4 ? 1.0f : 0.0f);
    npcs.Frozen = m_DStep < 4; // still for the rays and the kills; the brain runs for the wounded
    m_Shot.clear();
    const float dt = std::max(0.0f, now - m_DPrevNow);
    m_DPrevNow = now;
    FirstPersonWeaponGameplay w;
    w.HeadMultiplier = 2.0f;
    w.LimbMultiplier = 0.75f;
    w.FalloffStart = 1000.0f;
    w.FalloffEnd = 2000.0f;
    w.FalloffMin = 1.0f;
    auto used = [&](const Npc& n) { return std::find(m_DUsed.begin(), m_DUsed.end(), n.Name) != m_DUsed.end(); };
    auto fresh = [&]() -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && !up->Wounded && !used(*up) && up->Hitboxes && up->Hitboxes->Active() && up->Cct != PhysicsWorld::kNoCharacter)
                return up.get();
        return nullptr;
    };
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    // A round down a part's middle: from behind it, or from the outside for an arm.
    auto from = [&](const Npc& n, int part, glm::vec3& origin, glm::vec3& dir) {
        const float yaw = n.Body.Yaw();
        const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw));
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        dir = fwd;
        if (part == 3 || part == 4) dir = -left;
        else if (part == 5 || part == 6) dir = left;
        origin = c - dir * 3.0f;
    };
    auto shoot = [&](Npc& n, int part, float damage) {
        w.Damage = damage;
        glm::vec3 origin, dir;
        from(n, part, origin, dir);
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, RootId(n), c, origin, dir, w, &killed, &head);
        return killed;
    };
    auto step = [&](int to) { m_DStep = to; m_DAt = now; };
    auto setHealth = [&](Npc& n, float hp) {
        n.Health = hp;
        if (auto* h = world.Registry.try_get<HealthComponent>(n.Root)) h->Current = hp;
    };

    // The Scene view watches the soldier being shot (for NPC_TEST_RECORD): placed off his side when he
    // becomes the target, then it only turns to keep his pelvis in frame as he falls. The wounded (step 5) are
    // watched square from the side and followed as they move and turn (eased, so a turn doesn't swing the view);
    // m_DCamOn picks the crawler or the dying one.
    const int camOn = m_DStep == 5 ? m_DCamOn : 0;
    if (Npc* t = m_DNpc[camOn].empty() ? nullptr : find(m_DNpc[camOn])) {
        glm::vec3 pelvis;
        if (t->Body.BoneWorld(world, "pelvis", pelvis)) {
            const float yaw = t->Body.Yaw();
            const glm::vec3 side(std::cos(yaw), 0.0f, -std::sin(yaw)), fwd(std::sin(yaw), 0.0f, std::cos(yaw));
            const glm::vec3 beside = pelvis + side * 3.2f + glm::vec3(0.0f, 0.9f, 0.0f);
            if (m_DCamFor != t->Name) {
                m_DCamFor = t->Name;
                m_DCamSince = now;
                m_CamPos = m_DStep == 5 ? beside : pelvis + side * 3.2f + fwd * 1.6f + glm::vec3(0.0f, 0.6f, 0.0f);
            } else if (m_DStep == 5 && !t->Dead) {
                m_CamPos += (beside - m_CamPos) * std::min(1.0f, dt * 2.0f);
            }
            const glm::vec3 d = glm::normalize(pelvis - glm::vec3(0.0f, 0.2f, 0.0f) - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            m_HaveCam = true;
        }
    }
    switch (m_DStep) {
    case 0: { // wait for a squad with hitboxes
        int ready = 0;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Hitboxes && up->Hitboxes->Active()) ++ready;
        if (now > 3.0f && ready >= 3) step(1);
        break;
    }
    case 1: { // a ray at every bone's middle names that bone; a grazing ray hits the capsule only for an NPC's own queries
        if (now - m_DAt < 0.3f) break;
        Npc* a = fresh();
        if (!a) break;
        int right = 0;
        std::string wrong;
        for (int part = 0; part < NpcRagdoll::kParts; ++part) {
            glm::vec3 origin, dir;
            from(*a, part, origin, dir);
            const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
            PhysicsWorld::BodyPartHit bp;
            if (PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == PhysicsWorld::kBodyPartHitbox && bp.Part == part && bp.Entity == RootId(*a)) ++right;
            else wrong += std::string(" ") + NpcPartDefOf(part).Bone + "->" + std::to_string(bp.Part);
        }
        std::printf("[NpcTest] hitbox rays: %d / %d parts named correctly%s\n", right, NpcRagdoll::kParts, wrong.c_str());
        Check(right == NpcRagdoll::kParts, "a ray down each bone's hitbox names that bone" + wrong);
        // The movement capsule is skipped by the player's rays but not by the soldiers': a ray just inside the capsule's
        // edge at thigh height misses every hitbox. (PhysX gives a controller's query actor 0.8 of its radius.)
        float capFeet[3], capRadius = 0.3f;
        PhysicsWorld::GetNpcCapsule(a->Cct, capFeet, &capRadius, nullptr);
        const float side = 0.8f * capRadius * 0.9f;
        const float yaw = a->Body.Yaw();
        const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw));
        // (From in front, down the line it faces: open ground, not a wall at its back.)
        const glm::vec3 g = a->Feet + glm::vec3(0.0f, 0.55f, 0.0f) + left * side + fwd * 1.2f;
        const float o[3] = {g.x, g.y, g.z}, d[3] = {-fwd.x, -fwd.y, -fwd.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit;
        const bool playerHit = PhysicsWorld::RaycastFiltered(o, d, 2.4f, f, hit) && hit.Hit && hit.Entity == RootId(*a);
        bool soldierHit = false;
        {
            PhysicsWorld::ScopedQueryPolicy policy(0xFFFFFFFEu, /*hitPlayer=*/true);
            soldierHit = PhysicsWorld::RaycastFiltered(o, d, 2.4f, f, hit) && hit.Hit && hit.Entity == RootId(*a);
        }
        Check(!playerHit, "the player's shots pass through the movement capsule's empty corners");
        Check(soldierHit, "a soldier's own queries still meet the movement capsule");
        step(2);
        break;
    }
    case 2: { // the kills
        if (m_DCase >= kDeathCaseCount) { step(3); break; }
        if (m_DNpc[0].empty()) {
            Npc* n = fresh();
            if (!n) break;
            const DeathCase& dc = kDeathCases[m_DCase];
            m_DNpc[0] = n->Name;
            m_DUsed.push_back(n->Name);
            const bool first = shoot(*n, dc.Part, dc.Damage);
            if (dc.DiesFirst) Check(first && n->Dead, std::string(dc.Name) + ": one round kills");
            else {
                Check(!n->Dead, std::string(dc.Name) + ": the first round doesn't kill");
                Check(!npcs.RagdollSettings().HitFlinch || npcs.FlinchActive(n->Index), std::string(dc.Name) + ": a round that doesn't kill kicks the struck region (hit flinch)");
            }
            if (dc.LimpAfterFirst) Check(n->LimpUntil > npcs.Now(), std::string(dc.Name) + ": a leg hit leaves it limping");
            else Check(n->LimpUntil < npcs.Now(), std::string(dc.Name) + ": no limp");
            if (dc.StaggerAfterFirst)
                Check(n->StaggerUntil > npcs.Now() && n->ReactionLeft >= 0.39f, std::string(dc.Name) + ": a heavy hit staggers it (aim paused 0.4 s)");
            int rounds = 1;
            while (!n->Dead && rounds < 12) { shoot(*n, dc.Part, dc.Damage); ++rounds; }
            std::printf("[NpcTest] %s: dead after %d round(s)\n", dc.Name, rounds);
            Check(n->Dead, std::string(dc.Name) + ": dead after " + std::to_string(rounds) + " round(s)");
            m_DAt = now;
            break;
        }
        Npc* n = find(m_DNpc[0]);
        if (!n) { m_DNpc[0].clear(); ++m_DCase; break; }
        if (n->Ragdoll || now - m_DAt > 1.0f) {
            ++m_DDeaths;
            const bool ragdoll = n->Ragdoll != nullptr;
            if (ragdoll) ++m_DRagdolls;
            Check(ragdoll, std::string(kDeathCases[m_DCase].Name) + ": a ragdoll took the body");
            if (ragdoll) {
                std::printf("[NpcTest] %s: pose pop %.1f cm between the last animated frame and the first ragdoll one\n",
                            kDeathCases[m_DCase].Name, n->DeathPop * 100.0f);
                Check(n->DeathPop >= 0.0f && n->DeathPop <= 0.15f,
                      std::string(kDeathCases[m_DCase].Name) + ": no pose pop over 15 cm (" + std::to_string(n->DeathPop * 100.0f) + " cm)");
                m_DWorstPop = std::max(m_DWorstPop, n->DeathPop);
                Check(n->Ragdoll->DriveLeft() > 0.0f || npcs.Now() - n->DiedAt > n->Ragdoll->DriveFadeTime(),
                      std::string(kDeathCases[m_DCase].Name) + ": the joints start powered");
            }
            m_DNpc[0].clear();
            ++m_DCase;
        }
        break;
    }
    case 3: { // a corpse is shot
        Npc* corpse = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && up->Dead && up->Ragdoll) { corpse = up.get(); break; }
        if (!corpse) { if (now - m_DAt > 10.0f) { Check(false, "a corpse to shoot"); step(4); } break; }
        const bool asleep = corpse->Ragdoll->Asleep();
        if (!asleep && now - corpse->DiedAt < 7.0f) break; // let it settle
        if (m_DNpc[1].empty()) {
            m_DNpc[1] = corpse->Name;
            m_DCorpseAsleep = asleep;
            std::printf("[NpcTest] corpse %s %.1f s after death\n", asleep ? "asleep" : "still moving", now - corpse->DiedAt);
            Check(asleep || !npcs.RagdollSettings().PoweredRagdoll, "a powered corpse has come to rest and sleeps within 7 s");
            {
                // At rest it lies, not kneels: no knee folded past 60 degrees (thigh centre -> calf centre -> foot centre).
                float worst = 0.0f;
                for (int side = 0; side < 2; ++side) {
                    const glm::vec3 a = corpse->Ragdoll->PartPosition(7 + 2 * side), b = corpse->Ragdoll->PartPosition(8 + 2 * side), f = corpse->Ragdoll->PartPosition(14 + side);
                    const glm::vec3 u = glm::normalize(b - a), v = glm::normalize(f - b);
                    worst = std::max(worst, glm::degrees(std::acos(std::clamp(glm::dot(u, v), -1.0f, 1.0f))));
                }
                std::printf("[NpcTest] corpse at rest: worst knee bend %.0f deg\n", worst);
                Check(worst < 60.0f || !npcs.RagdollSettings().PoweredRagdoll, "a powered corpse at rest lies, its knees not folded past 60 degrees (" + std::to_string(worst) + ")");
            }
            for (int k = 0; k < NpcRagdoll::kParts; ++k) m_DCorpseBefore[k] = corpse->Ragdoll->PartPosition(k);
            for (int k = 0; k < 4; ++k) corpse->Body.BoneWorld(kCorpseBones[k], m_DBoneBefore[k]);
            // From the side at the chest part's middle.
            const glm::vec3 c = corpse->Ragdoll->PartPosition(1);
            const float o[3] = {c.x - 3.0f, c.y, c.z}, d[3] = {1.0f, 0.0f, 0.0f};
            PhysicsWorld::BodyPartHit bp;
            const bool part = PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == PhysicsWorld::kBodyPartRagdoll && bp.Entity == RootId(*corpse);
            Check(part, "a ray at a corpse names a ragdoll part");
            w.Damage = 34.0f;
            npcs.OnPlayerHit(world, RootId(*corpse), c, glm::vec3(o[0], o[1], o[2]), glm::vec3(1.0f, 0.0f, 0.0f), w, nullptr, nullptr);
            Check(!corpse->Ragdoll->Asleep(), std::string("a round wakes the corpse") + (m_DCorpseAsleep ? "" : " (it was still moving)"));
            m_DAt = now;
            break;
        }
        Npc* c = find(m_DNpc[1]);
        if (!c || !c->Ragdoll) { step(4); break; }
        if (now - m_DAt > 0.6f) {
            float moved = 0.0f;
            for (int k = 0; k < NpcRagdoll::kParts; ++k) moved += glm::length(c->Ragdoll->PartPosition(k) - m_DCorpseBefore[k]);
            float drawn = 0.0f;
            for (int k = 0; k < 4; ++k) {
                glm::vec3 bone(0.0f);
                c->Body.BoneWorld(kCorpseBones[k], bone);
                drawn += glm::length(bone - m_DBoneBefore[k]);
            }
            std::printf("[NpcTest] corpse shot: parts moved %.1f cm in all, the drawn bones %.1f cm\n", moved * 100.0f, drawn * 100.0f);
            Check(moved > 0.10f, "the corpse reacts to a shot (parts moved " + std::to_string(moved * 100.0f) + " cm in all)");
            Check(drawn > 0.05f, "the drawn body follows the shoved corpse (" + std::to_string(drawn * 100.0f) + " cm)");
            step(4);
        }
        break;
    }
    case 4: { // two soldiers go down wounded: one shot in the leg (on its belly), one in the chest (doubled over, dying)
        Npc* a = fresh();
        Npc* b = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && up.get() != a && !up->Dead && !up->Wounded && !used(*up) && up->Hitboxes && up->Hitboxes->Active() &&
                up->Cct != PhysicsWorld::kNoCharacter)
                b = up.get();
        if (!a || !b) { if (now - m_DAt > 25.0f) { Check(false, "two soldiers to wound"); step(7); } break; }
        m_DUsed.push_back(a->Name);
        m_DUsed.push_back(b->Name);
        // (Damage reads the soldier's Health component: set it, not only the copy.)
        // A player's round (34; 25.5 on a leg) that would kill: it goes down instead, on a tenth of its health.
        setHealth(*a, 20.0f);
        shoot(*a, 7, 34.0f); // the left thigh
        setHealth(*b, 30.0f);
        shoot(*b, 1, 34.0f); // the chest
        m_DNpc[0] = a->Name;
        m_DNpc[1] = b->Name;
        Check(a->Wounded && b->Wounded && !a->Dead && !b->Dead,
              "a leg or body round that would kill can leave a soldier wounded instead (the chances forced to 1)");
        Check(std::abs(a->Health - 0.1f * a->MaxHealth) < 0.5f, "... on a tenth of its health (" + std::to_string(a->Health) + ")");
        shoot(*a, 1, 34.0f); // the rest of the burst, the same frame
        Check(!a->Dead, "the rest of the burst that dropped it doesn't finish it before it has fallen");
        Check(a->Doing == Behaviour::Wounded, "it is in the Wounded behaviour");
        Check(a->Prone && a->HurtLeg == 0, "the leg-shot soldier goes down on its belly, dragging the left leg");
        Check(!a->Weapon && a->Dropped, "... and drops its gun");
        Check(a->FallPending || a->Ragdoll, "... and goes down with the physics (the wound fall), not the collapse clip");
        Check(b->Dying && !b->Prone && !b->Weapon, "the chest-shot one drops its gun and is dying, on its feet");
        m_DCrawlAt = -1.0f;
        m_DCamOn = 0;
        step(5);
        break;
    }
    case 5: { // the crawler is ordered off to its side and measured, then killed; the dying one is measured till it bleeds out
        Npc* a = find(m_DNpc[0]);
        Npc* b = find(m_DNpc[1]);
        if (!a || !b) { step(7); break; }
        const float t = now - m_DAt;
        const bool crawling = m_DCrawlAt >= 0.0f && !m_DCrawlDone;
        if (!a->Dead) TrackWound(world, *a, m_DCrawl, now, dt, crawling);
        if (!b->Dead) TrackWound(world, *b, m_DDying, now, dt, true);
        if (!m_DKneelChecked && t > 0.3f) {
            m_DKneelChecked = true;
            m_DFallRagdoll = a->Ragdoll.get();
            Check(!a->Dead && a->Ragdoll && a->Body.StateName(world) == "ragdoll",
                  "the leg-shot one goes down as a ragdoll, alive (" + a->Body.StateName(world) + ")");
            Check(!a->Hitboxes || a->Hitboxes->Active(), "... his hitboxes still on while he falls");
        }
        // Down from the fall: he is low, then on his belly in the prone idle (or on his back, lying as he fell).
        if (!m_DDownChecked && !a->Dead && a->DownAt >= 0.0f) {
            m_DDownChecked = true;
            glm::vec3 pelvis(0.0f);
            a->Body.BoneWorld("pelvis", pelvis);
            std::printf("[NpcTest] wound fall: down %.2f s after the hit on his %s (front %.2f up), pelvis %.0f cm off the floor, state %s\n",
                        a->FallTime, a->CanCrawl ? "belly" : "back", a->FallFrontUp, (pelvis.y - a->Feet.y) * 100.0f, a->Body.StateName(world).c_str());
            Check(a->FallTime > 0.0f && a->FallTime <= npcs.RagdollSettings().WoundRecoverMaxTime + 0.05f,
                  "the fall is over within Wound Recover Max Time (" + std::to_string(a->FallTime) + " s)");
            Check(pelvis.y - a->Feet.y < 0.35f, "... and leaves him low (pelvis " + std::to_string((pelvis.y - a->Feet.y) * 100.0f) + " cm)");
            if (a->CanCrawl) Check(!a->Ragdoll && a->Body.Recovering(), "on his belly: the ragdoll lets go and the animated pose blends in");
            else Check(a->Ragdoll && a->Body.StateName(world) == "ragdoll", "on his back: he stays down as he fell (no crawl)");
        }
        if (!m_DHandoverChecked && !a->Dead && a->Recovered && a->CanCrawl) {
            m_DHandoverChecked = true;
            std::printf("[NpcTest] wound fall handover: %.1f cm the first animated frame, %.1f cm the most in a frame over the blend; %s\n",
                        a->HandoverJump * 100.0f, a->BlendJump * 100.0f, a->Body.StateName(world).c_str());
            Check(a->HandoverJump >= 0.0f && a->BlendJump >= 0.0f && a->BlendJump <= 0.05f,
                  "no bone jumps over 5 cm in a frame as the animated pose takes him back (" + std::to_string(a->BlendJump * 100.0f) + " cm)");
            Check(now - a->DownAt <= 2.5f * npcs.RagdollSettings().WoundBlendTime + 0.1f, "... blended in within 2.5x Wound Blend Time (" + std::to_string(now - a->DownAt) + " s)");
            Check(a->Body.StateName(world).rfind("Prone", 0) == 0 && a->Body.StateName(world) != "ProneDrop",
                  "... into the prone idle (" + a->Body.StateName(world) + ")");
        }
        // On his back he never crawls: a while lying there, then the next round kills him - as the same ragdoll.
        if (!m_DFaceUpKilled && !a->Dead && a->DownAt >= 0.0f && !a->CanCrawl && now - a->DownAt > 2.0f) {
            m_DFaceUpKilled = true;
            m_DCrawlDone = true;
            Check(a->Body.StateName(world) == "ragdoll" && glm::length(glm::vec2(a->Velocity.x, a->Velocity.z)) < 0.3f, "on his back he lies there, no crawl");
            shoot(*a, 1, 20.0f);
            Check(a->Dead && a->Ragdoll && a->Ragdoll.get() == m_DFallRagdoll, "a round kills him where he lies, the same ragdoll going on (no pop)");
        }
        if (!m_DDropShot && t > 1.8f && !a->Dead) {
            m_DDropShot = true;
            m_Shot = "wounded_prone_drop";
        }
        // The collapse over: an order to crawl 4 m off to its side, so it has to turn as well as crawl (the brain's own
        // cover pick finds nothing in the Arena). Whichever side has open floor.
        if (m_DCrawlAt < 0.0f && !a->Dead && t > 0.5f && ((a->Recovered && a->CanCrawl) || (t > 6.0f && a->DownAt < 0.0f))) {
            Check(t <= 6.0f, "the fall ends within 6 s (" + std::to_string(t) + " s)");
            const float yaw = a->Body.Yaw();
            const glm::vec3 left(std::cos(yaw), 0.0f, -std::sin(yaw));
            glm::vec3 goal = a->Feet + left * 4.0f;
            bool open = false;
            const NavMesh& nav = npcs.Nav();
            for (float s : {1.0f, -1.0f}) {
                const glm::vec3 g = a->Feet + left * (4.0f * s);
                glm::vec3 on(0.0f);
                if (nav.Valid() && nav.Closest(g, on) && glm::length(glm::vec2(on.x - g.x, on.z - g.z)) < 0.5f && nav.Walkable(a->Feet, on)) {
                    goal = on;
                    open = true;
                    break;
                }
            }
            Check(open, "open floor 4 m to one side of the crawler");
            npcs.ScriptedMove(a->Index, goal, Gait::Walk);
            m_DCrawlAt = now;
            std::printf("[NpcTest] crawl ordered %.1f s after the hit: 4 m %s (cover %d)\n", t,
                        glm::dot(goal - a->Feet, left) > 0.0f ? "to its left" : "to its right", a->Cover);
        }
        const float c = m_DCrawlAt >= 0.0f ? now - m_DCrawlAt : -1.0f;
        // The dying one's shots: each as its state begins (a beat in), once the view has been on it a moment.
        if (!b->Dead && m_DWantShot.empty()) {
            static const struct { const char* Shot; const char* State; const char* Alt; float After; } kDying[] = {
                {"dying_idle", "InjuredIdle", nullptr, 0.6f},
                {"dying_walk", "InjuredWalk", "InjuredWalkStart", 0.5f},
                {"dying_drop", "InjuredDrop", nullptr, 0.4f},
                {"dying_lying", "InjuredLying", nullptr, 1.0f},
            };
            for (const auto& d : kDying) {
                if (std::find(m_DDyingShots.begin(), m_DDyingShots.end(), d.Shot) != m_DDyingShots.end()) continue;
                float began = -1.0f;
                for (const auto& st : m_DDying.States)
                    if (st.first == d.State || (d.Alt && st.first == d.Alt)) { began = st.second; break; }
                if (began >= 0.0f && t - began >= d.After) { m_DWantShot = d.Shot; break; }
            }
        }
        // The view's subject: the dying one for its shot or once the crawler is dead, else the crawler.
        m_DCamOn = (!m_DWantShot.empty() || a->Dead) ? 1 : 0;
        const bool settled = now - m_DCamSince >= kViewSettle;
        if (!m_DWantShot.empty() && m_DCamFor == b->Name && settled) {
            m_Shot = m_DWantShot;
            m_DDyingShots.push_back(m_DWantShot);
            m_DWantShot.clear();
        } else if (crawling && m_DCrawlShots < kCrawlShots && m_DCamFor == a->Name && settled &&
                   c >= 0.35f + kCrawlShotEvery * (float)m_DCrawlShots) {
            m_Shot = "crawl_side_" + std::to_string(m_DCrawlShots++);
        }
        // Mid-crawl: the pose on its belly.
        if (!m_DProneChecked && crawling && c > 2.0f) {
            m_DProneChecked = true;
            glm::vec3 head(0.0f), pelvis(0.0f), foot(0.0f);
            a->Body.BoneWorld("head", head);
            a->Body.BoneWorld("pelvis", pelvis);
            a->Body.BoneWorld("foot_l", foot);
            const float footDrop = foot.y - a->Feet.y;
            std::printf("[NpcTest] prone: %s, head %.0f cm, pelvis %.0f cm off the floor; dragged foot %.0f cm up\n",
                        a->Body.StateName(world).c_str(), (head.y - a->Feet.y) * 100.0f, (pelvis.y - a->Feet.y) * 100.0f, footDrop * 100.0f);
            Check(a->Body.StateName(world).rfind("ProneCrawl", 0) == 0, "then it crawls (" + a->Body.StateName(world) + ")");
            bool drop = false;
            for (const auto& st : m_DCrawl.States) drop = drop || st.first == "ProneDrop";
            Check(!drop, "the collapse clip (ProneDrop) never played:" + StateLog(m_DCrawl.States));
            Check(head.y - a->Feet.y < 0.5f && pelvis.y - a->Feet.y < 0.45f, "on its belly (head and pelvis low)");
            Check(footDrop < 0.2f, "the dragged foot is on the floor (" + std::to_string(footDrop * 100.0f) + " cm)");
        }
        // The crawl measured: then the next hit kills it.
        if (crawling && c > kCrawlWatch && !a->Dead) {
            m_DCrawlDone = true;
            npcs.EndScriptedMove(a->Index);
            const WoundTrack& track = m_DCrawl;
            const glm::vec3 net = a->Feet - track.Start;
            const float covered = glm::length(glm::vec2(net.x, net.z));
            float p95[4], worstP95 = 0.0f, worstSlide = 0.0f;
            for (int k = 0; k < 4; ++k) {
                p95[k] = Percentile95(track.PlantedSpeed[k]);
                worstP95 = std::max(worstP95, p95[k]);
                worstSlide = std::max(worstSlide, track.MaxSlide[k]);
            }
            const float straight[2] = {track.StraightFrames ? track.Straight[0] / (float)track.StraightFrames : 0.0f,
                                       track.StraightFrames ? track.Straight[1] / (float)track.StraightFrames : 0.0f};
            std::printf("[NpcTest] crawl metrics: %.2f m in %.1f s (path %.2f m, avg %.2f m/s, top %.2f m/s); pelvis max %.0f cm, pop %.1f cm/frame; "
                        "yaw rate max %.0f deg/s; planted slide p95 m/s (longest cm) hand_l %.3f (%.1f) hand_r %.3f (%.1f) knee_l %.3f (%.1f) "
                        "knee_r %.3f (%.1f) [%d/%d/%d/%d frames]; knees %.2f / %.2f straight over %d moving frames; states%s\n",
                        covered, track.Seconds, track.Path, track.Seconds > 0.0f ? track.Path / track.Seconds : 0.0f, track.TopSpeed, track.MaxPelvis * 100.0f,
                        track.MaxPop * 100.0f, track.MaxYawRate, p95[0], track.MaxSlide[0] * 100.0f, p95[1], track.MaxSlide[1] * 100.0f, p95[2],
                        track.MaxSlide[2] * 100.0f, p95[3], track.MaxSlide[3] * 100.0f, (int)track.PlantedSpeed[0].size(), (int)track.PlantedSpeed[1].size(),
                        (int)track.PlantedSpeed[2].size(), (int)track.PlantedSpeed[3].size(), straight[0], straight[1], track.StraightFrames,
                        StateLog(track.States).c_str());
            Check(covered >= 2.5f, "a wounded soldier ordered to crawl gets 2.5 m in " + std::to_string(kCrawlWatch) + " s (" +
                  std::to_string(covered) + " m)");
            Check(worstP95 < 0.15f, "the planted hands and knees hold the floor (95th percentile slide " + std::to_string(worstP95) +
                  " m/s, longest " + std::to_string(worstSlide * 100.0f) + " cm)");
            Check(track.MaxPop < 0.03f, "no pelvis pop while crawling (" + std::to_string(track.MaxPop * 100.0f) + " cm in a frame)");
            std::printf("[NpcTest] crawl body: pelvis at most %.0f cm from the capsule, lurch at most %.1f cm in a frame\n", track.MaxOffset * 100.0f,
                        track.MaxLeap * 100.0f);
            Check(track.MaxOffset < 0.35f, "the crawling body lies over its capsule (pelvis " + std::to_string(track.MaxOffset * 100.0f) + " cm off)");
            // (The crawl's own surge of the hips runs to ~4.5 cm a frame at the desperate crawl's 1.6x; a snap was 24.)
            Check(track.MaxLeap < 0.06f, "no lurch between the crawl's states (" + std::to_string(track.MaxLeap * 100.0f) + " cm in a frame)");
            Check(track.MaxYawRate <= 30.0f, "it turns on its belly no faster than 30 deg/s (" + std::to_string(track.MaxYawRate) + ")");
            Check(track.StraightFrames > 0 && straight[0] > 0.97f && straight[0] > straight[1],
                  "crawling, the dragged leg is straight, the crawling one bent (" + std::to_string(straight[0]) + " / " +
                  std::to_string(straight[1]) + ")");
            Check(!b->Dead && b->Wounded, "an unhit wounded soldier is still alive after " + std::to_string((int)t) + " s");
            shoot(*a, 8, 20.0f); // any hit
            Check(a->Dead, "a wounded soldier dies on the next hit");
            m_DProneKillAt = now;
        }
        // Finished off on the floor: no freeze in the crawl's pose, no fall to make - it goes slack and lies flat. From 2 s on,
        // nothing of it (a hand, a forearm, the head) stays held up off the floor.
        if (m_DProneKillAt >= 0.0f && a->Dead && a->Ragdoll && a->Ragdoll->Active()) {
            const float since = now - m_DProneKillAt;
            if (since >= 2.0f && since < 3.5f)
                for (int part : {0, 2, 4, 6, NpcRagdoll::kHandL, NpcRagdoll::kHandR}) // (0: the pelvis - no backside in the air)
                    m_DProneWorstLift = std::max(m_DProneWorstLift, a->Ragdoll->PartPosition(part).y - a->Feet.y);
            if (since >= 3.5f && m_DProneKillAt > -0.5f) {
                std::printf("[NpcTest] finished off crawling: by 2 s the highest pelvis, hand, forearm or head %.0f cm off the floor; strongest muscle %.2f\n",
                            m_DProneWorstLift * 100.0f, a->Ragdoll->Motor().Strength());
                Check(m_DProneWorstLift < 0.3f, "finished off on the floor, he goes slack and lies flat (nothing held up: " +
                      std::to_string(m_DProneWorstLift * 100.0f) + " cm)");
                m_DProneKillAt = -1.0f;
            }
        }
        if (t > 22.5f) {
            Check(b->Dead, "a wounded soldier bleeds out after 20 s");
            const WoundTrack& track = m_DDying;
            const std::string states = StateLog(track.States);
            std::printf("[NpcTest] dying metrics: path %.2f m, top %.2f m/s; pelvis max %.0f cm, lying %.0f cm, pop %.1f cm/frame; yaw rate max "
                        "%.0f deg/s; shots %d; states%s\n", track.Path, track.TopSpeed, track.MaxPelvis * 100.0f, track.LyingPelvis * 100.0f,
                        track.MaxPop * 100.0f, track.MaxYawRate, (int)m_DDyingShots.size(), states.c_str());
            // The states in order: doubled over where it stands, a few steps, down, lying.
            auto seen = [&](const char* name, const char* alt, int from) {
                for (int i = std::max(from, 0); i < (int)track.States.size(); ++i)
                    if (track.States[(size_t)i].first == name || (alt && track.States[(size_t)i].first == alt)) return i;
                return -1;
            };
            const int idle = seen("InjuredIdle", nullptr, 0);
            const int walk = idle < 0 ? -1 : seen("InjuredWalk", "InjuredWalkStart", idle + 1);
            const int drop = walk < 0 ? -1 : seen("InjuredDrop", nullptr, walk + 1);
            const int lying = drop < 0 ? -1 : seen("InjuredLying", nullptr, drop + 1);
            Check(lying >= 0, "the chest-shot one stands hurt, walks, drops and lies, in that order (" + std::to_string(idle) + " " +
                  std::to_string(walk) + " " + std::to_string(drop) + " " + std::to_string(lying) + ":" + states + ")");
            Check(track.LyingPelvis >= 0.0f && track.LyingPelvis < 0.25f, "... and lies flat (pelvis " + std::to_string(track.LyingPelvis * 100.0f) + " cm)");
            step(7);
        }
        break;
    }
    default:
        break;
    }
    if (now >= m_Duration && !m_Done) {
        m_Done = true;
        PrintCosts(npcs);
        Check(m_DStep >= 7, "every stage of the scenario ran (reached " + std::to_string(m_DStep) + ")");
        Check(m_DDeaths == kDeathCaseCount && m_DRagdolls == kDeathCaseCount, "every kill ended in a ragdoll (" + std::to_string(m_DRagdolls) + ")");
        std::printf("[NpcTest] worst pose pop %.1f cm\n", m_DWorstPop * 100.0f);
    }
    // Early finish once the last stage is through.
    if (m_DStep >= 7 && !m_Done && now - m_DAt > 1.0f) m_Duration = now;
}

// --- pose: the weapon hold, close up ------------------------------------------------------------------
namespace {
struct PosePhase {
    const char* Name;
    bool Aim;
    float Ahead, Side, Up;   // the aim point off the soldier's chest, in its home frame (m)
    bool Crouch = false;
    float MoveSide = 0.0f;   // strafe this far (m, + = right) while aiming
    float MoveAhead = 0.0f;  // run this far ahead
    Gait Pace = Gait::Walk;
    bool Reload = false;
    bool Signal = false;     // a hand signal (forward and to the right) held through the phase
};
const PosePhase kPhases[] = {
    {"aim_level", true, 15.0f, 0.0f, 0.0f},
    {"aim_up", true, 12.0f, 0.0f, 8.0f},
    {"aim_down", true, 4.0f, 0.0f, -1.5f},
    {"aim_right", true, 10.0f, 10.0f, 0.0f},
    {"aim_left", true, 10.0f, -8.0f, 0.0f},
    {"hip", false, 15.0f, 0.0f, 0.0f},
    {"look_right", false, 4.0f, 8.0f, 0.5f},  // off the sights the head turns past the chest
    {"look_left", false, 4.0f, -8.0f, -1.0f},
    {"crouch_aim", true, 15.0f, 0.0f, 0.0f, true},
    {"crouch_hip", false, 15.0f, 0.0f, 0.0f, true},
    {"strafe_right", true, 15.0f, 0.0f, 0.0f, false, 4.0f},
    {"strafe_left", true, 15.0f, 0.0f, 0.0f, false, -4.0f},
    {"reload", true, 15.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, Gait::Walk, true},
    {"signal", false, 15.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, Gait::Walk, false, true},
    {"jog", false, 15.0f, 0.0f, 0.0f, false, 0.0f, 7.0f, Gait::Jog},
    {"sprint", false, 15.0f, 0.0f, 0.0f, false, 0.0f, -7.0f, Gait::Run},
};
constexpr float kPhaseTime = kPosePhaseTime;
constexpr int kPhaseCount = (int)(sizeof(kPhases) / sizeof(kPhases[0]));
int PosePhaseCount() { return kPhaseCount; }
} // namespace

void NpcTest::Pose(World& world, NpcDirector& npcs, float now) {
    (void)world;
    npcs.Frozen = true;
    npcs.HoldFire = true;
    npcs.MeshChecksEverywhere = true;
    // The two probes: the first AK and the first Remington.
    Npc* probe[2] = {nullptr, nullptr};
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        const int k = up->Class == WeaponClass::Shotgun ? 1 : 0;
        if (m_Probe[k].empty() && now > 0.5f) {
            m_Probe[k] = up->Name;
            m_ProbeHome[k] = up->Feet;
        }
        if (up->Name == m_Probe[0]) probe[0] = up.get();
        if (up->Name == m_Probe[1]) probe[1] = up.get();
    }
    // Everyone else stands easy.
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && up.get() != probe[0] && up.get() != probe[1]) {
            up->Intent = NpcIntent{};
            up->Intent.LookPoint = up->Feet + glm::vec3(0.0f, 1.6f, 10.0f);
        }
    if (!probe[0] || !probe[1] || now < 2.0f) return;
    if (m_PosePhase < 0) { m_PosePhase = 0; m_PhaseStart = now; }
    float t = now - m_PhaseStart;
    if (t >= kPhaseTime) {
        ++m_PosePhase;
        m_PhaseStart = now;
        t = 0.0f;
        m_PoseShots = 0;
    }
    if (m_PosePhase >= kPhaseCount) return;
    const PosePhase& ph = kPhases[m_PosePhase];
    // Home frame: facing south (toward the arena), as they spawn.
    const glm::vec3 fwd(0.0f, 0.0f, 1.0f), right(-1.0f, 0.0f, 0.0f);
    for (int k = 0; k < 2; ++k) {
        Npc& n = *probe[k];
        NpcIntent in;
        const glm::vec3 base = m_ProbeHome[k];
        const glm::vec3 chest = n.Feet + glm::vec3(0.0f, ph.Crouch ? 0.95f : 1.45f, 0.0f);
        const glm::vec3 aimAt = glm::vec3(chest.x, base.y + (ph.Crouch ? 0.95f : 1.45f), base.z) + fwd * ph.Ahead + right * ph.Side +
                                glm::vec3(0.0f, ph.Up, 0.0f);
        in.Aim = ph.Aim;
        in.AimPoint = aimAt;
        in.LookPoint = aimAt;
        in.Crouch = ph.Crouch;
        in.Reload = ph.Reload && t < 0.2f;
        if (ph.Reload && t < 0.05f && n.Weapon) n.Weapon->SetAmmo(n.Class == WeaponClass::Shotgun ? 4 : 12);
        if (ph.Signal && t < 0.05f) n.Body.Signal(fwd + right * 0.6f, kPhaseTime);
        if (ph.MoveSide != 0.0f || ph.MoveAhead != 0.0f) {
            // Out and back within the phase: the first half there, the second half home.
            const glm::vec3 there = base + right * ph.MoveSide + fwd * ph.MoveAhead;
            in.Move = true;
            in.MoveTarget = t < kPhaseTime * 0.55f ? there : base;
            in.Pace = ph.Pace;
            in.FaceAim = ph.Aim;
        } else if (glm::length(glm::vec2(n.Feet.x - base.x, n.Feet.z - base.z)) > 0.3f) {
            in.Move = true; // drifted (a sprint's stop): walk home
            in.MoveTarget = base;
            in.Pace = Gait::Walk;
            in.FaceAim = true;
        }
        n.Intent = in;
    }

    // Measured once a phase, settled (1.2 s in); four views of each probe near the end.
    auto measure = [&](Npc& n) {
        if (!n.Weapon || !n.Weapon->IsActive()) return;
        FirstPersonWorldGunInput gun;
        glm::vec3 muzzle, dir;
        if (!n.Weapon->WorldGunInput(gun) || !n.Weapon->MuzzleRay(muzzle, dir)) return;
        const glm::vec3 shift = n.Body.GunShift();
        const NpcHoldReport r = n.Body.MeasureHold(world, n.Weapon->ArmsEntity(), gun.ButtWorld + shift, muzzle + shift);
        std::printf("[NpcTest] pose %-12s %-4s gun-head %5.1f cm  gun-torso %5.1f cm  elbows %5.1f / %5.1f cm  hands off %4.1f / %4.1f cm  shift %4.1f cm  cheek %4.1f deg  eye(%5.1f,%5.1f,%5.1f)cm  %s\n",
                    ph.Name, n.Class == WeaponClass::Shotgun ? "870" : "AK", r.GunHead * 100.0f, r.GunTorso * 100.0f, r.Elbow[0] * 100.0f,
                    r.Elbow[1] * 100.0f, r.Hand[0] * 100.0f, r.Hand[1] * 100.0f, r.Shift * 100.0f, r.CheekTilt,
                    r.EyeFromHead.x * 100.0f, r.EyeFromHead.y * 100.0f, r.EyeFromHead.z * 100.0f, n.Weapon->CurrentState().c_str());
        const std::string at = std::string(ph.Name) + (n.Class == WeaponClass::Shotgun ? " 870" : " AK");
        // The stock rests on the shoulder (the torso) by design when shouldered; elsewhere it should stand off.
        if (r.GunHead >= 0.0f && r.GunHead < m_WorstHead) { m_WorstHead = r.GunHead; m_WorstHeadAt = at; }
        if (r.GunTorso >= 0.0f && r.GunTorso < m_WorstTorso && gun.Shouldered < 0.5f) { m_WorstTorso = r.GunTorso; m_WorstTorsoAt = at; }
        for (int s = 0; s < 2; ++s) {
            if (r.Elbow[s] >= 0.0f && r.Elbow[s] < m_WorstElbow) { m_WorstElbow = r.Elbow[s]; m_WorstElbowAt = at + (s ? " right" : " left"); }
            // Signalling, the support hand is off the gun by design.
            if (!ph.Signal && r.Hand[s] > m_WorstHand) { m_WorstHand = r.Hand[s]; m_WorstHandAt = at + (s ? " right" : " left"); }
        }
    };
    if (t >= 1.2f && t - 1.0f / 60.0f < 1.2f) {
        measure(*probe[0]);
        measure(*probe[1]);
        std::fflush(stdout);
    }
    // Views: right side, front-right, front-left, high front-right; AK probe then Remington probe.
    static const struct { float Yaw, Pitch, Dist; const char* Name; } kViews[4] = {
        {-90.0f, -5.0f, 2.3f, "right"}, {-35.0f, -8.0f, 2.2f, "front_r"}, {40.0f, -8.0f, 2.2f, "front_l"}, {-20.0f, -48.0f, 2.3f, "top"}};
    const float shotStart = ph.Reload ? 0.7f : 1.3f;
    const int view = (int)std::floor((t - shotStart) / 0.12f);
    if (view >= 0 && view < 8) {
        Npc& n = *probe[view / 4];
        const auto& v = kViews[view % 4];
        const float yaw = n.Body.Yaw() + glm::radians(v.Yaw);
        const glm::vec3 chest = n.Feet + glm::vec3(0.0f, n.Crouched ? 0.95f : 1.35f, 0.0f);
        const glm::vec3 dirFlat(std::sin(yaw), 0.0f, std::cos(yaw));
        const float pitch = glm::radians(v.Pitch);
        m_CamPos = chest + (dirFlat * std::cos(pitch) - glm::vec3(0.0f, std::sin(pitch), 0.0f)) * v.Dist;
        const glm::vec3 d = glm::normalize(chest - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        // A few frames into its window (the view has the camera by then), once.
        if (view >= m_PoseShots && t - shotStart - (float)view * 0.12f > 0.05f) {
            char name[96];
            std::snprintf(name, sizeof name, "pose_%02d_%s_%s_%s", m_PosePhase, ph.Name, view < 4 ? "ak" : "870", v.Name);
            m_Shot = name;
            m_PoseShots = view + 1;
        }
    }
}

// --- reload: the reloads and the regrip, against the player's ----------------------------------------------------------
// The two probes (an AK, a Remington), the AI frozen, each facing the arena: aimed, then reloading where they stand; at the
// hip, then reloading; an empty reload; the idle regrip. Through every one the butt's place against the right
// shoulder (in the aim's flat frame - what --stock-probe prints for the player's body as "butt-shoulder R U F") and the
// hands off the rig's are logged every third frame, and the Scene view takes the AK probe from its front right.
namespace {
struct ReloadPhase { const char* Name; float Start; bool Aim; int Ammo; bool Fidget; };
const ReloadPhase kReloadPhases[] = {
    {"aim", 0.0f, true, -1, false},       {"aim_reload", 2.0f, true, 12, false}, {"hip", 7.0f, false, -1, false},
    {"hip_reload", 9.0f, false, 12, false}, {"empty_reload", 14.0f, true, 0, false}, {"regrip", 19.5f, false, -1, true},
};
constexpr float kReloadEnd = 25.0f;
} // namespace

void NpcTest::Reload(World& world, NpcDirector& npcs, float now) {
    npcs.Frozen = true;
    npcs.HoldFire = true;
    npcs.MeshChecksEverywhere = true;
    Npc* probe[2] = {nullptr, nullptr};
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        const int k = up->Class == WeaponClass::Shotgun ? 1 : 0;
        if (m_Probe[k].empty() && now > 0.5f) { m_Probe[k] = up->Name; m_ProbeHome[k] = up->Feet; }
        if (up->Name == m_Probe[0]) probe[0] = up.get();
        if (up->Name == m_Probe[1]) probe[1] = up.get();
    }
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && up.get() != probe[0] && up.get() != probe[1]) {
            up->Intent = NpcIntent{};
            up->Intent.LookPoint = up->Feet + glm::vec3(0.0f, 1.6f, 10.0f);
        }
    if (!probe[0] || !probe[1] || now < 2.0f) return;
    const float t = now - 2.0f;
    int phase = 0;
    for (int i = 0; i < (int)(sizeof kReloadPhases / sizeof kReloadPhases[0]); ++i)
        if (t >= kReloadPhases[i].Start) phase = i;
    const ReloadPhase& ph = kReloadPhases[phase];
    const bool entered = phase != m_PosePhase;
    m_PosePhase = phase;
    const glm::vec3 fwd(0.0f, 0.0f, 1.0f);
    for (int k = 0; k < 2; ++k) {
        Npc& n = *probe[k];
        NpcIntent in;
        const glm::vec3 aimAt = m_ProbeHome[k] + glm::vec3(0.0f, 1.45f, 0.0f) + fwd * 15.0f;
        in.Aim = ph.Aim;
        in.AimPoint = in.LookPoint = aimAt;
        if (entered && n.Weapon && ph.Ammo >= 0) {
            n.Weapon->SetAmmo(k == 1 ? std::min(ph.Ammo, 4) : ph.Ammo);
            in.Reload = true;
        }
        // The regrip once the hip carry has settled into Idle (a trigger set mid-transition is dropped).
        if (ph.Fidget && n.Weapon && !(m_Fidgets >> k & 1) && t - ph.Start > 0.6f && n.Weapon->CurrentState() == "Idle") {
            n.WeaponAction = "Fidget";
            m_Fidgets |= 1u << k;
        }
        if (glm::length(glm::vec2(n.Feet.x - m_ProbeHome[k].x, n.Feet.z - m_ProbeHome[k].z)) > 0.3f) {
            in.Move = true;
            in.MoveTarget = m_ProbeHome[k];
            in.Pace = Gait::Walk;
            in.FaceAim = true;
        }
        n.Intent = in;
        // The butt against the right shoulder and the hands off the rig's, every third frame.
        if (!n.Weapon || !n.Weapon->IsActive() || (m_Frame++ % 3) != 0) continue;
        FirstPersonWorldGunInput gun;
        glm::vec3 shoulder;
        if (!n.Weapon->WorldGunInput(gun) || !n.Body.BoneWorld("upperarm_r", shoulder)) continue;
        glm::vec3 front = n.WeaponCam.Front();
        front.y = 0.0f;
        front = glm::normalize(front + glm::vec3(1e-5f));
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 d = gun.ButtWorld + n.Body.GunShift() - shoulder;
        const glm::vec3 e = n.WeaponCam.Position - shoulder;
        const NpcHoldReport r = n.Body.MeasureHold(world, n.Weapon->ArmsEntity(), gun.ButtWorld, gun.ButtWorld);
        std::printf("[NpcTest] reload %-12s %-3s t %5.2f %-12s | butt-shoulder R %+5.1f U %+5.1f F %+5.1f | hands off L %4.1f R %4.1f cm | eye-shoulder R %+5.1f U %+5.1f F %+5.1f | shift %4.1f | cam pitch %5.1f roll %5.1f\n",
                    ph.Name, k ? "870" : "AK", t - ph.Start, n.Weapon->CurrentState().c_str(), glm::dot(d, right) * 100.0f, d.y * 100.0f,
                    glm::dot(d, front) * 100.0f, r.Hand[0] * 100.0f, r.Hand[1] * 100.0f, glm::dot(e, right) * 100.0f, e.y * 100.0f,
                    glm::dot(e, front) * 100.0f, glm::length(n.Body.GunShift()) * 100.0f, n.WeaponCam.Pitch, n.WeaponCam.Roll);
        if (!ph.Fidget || phase == 0) {
            for (int s = 0; s < 2; ++s)
                if (r.Hand[s] > m_WorstHand) { m_WorstHand = r.Hand[s]; m_WorstHandAt = std::string(ph.Name) + (k ? " 870" : " AK") + (s ? " right" : " left"); }
        }
        if (n.Weapon->CurrentState().find("Reload") != std::string::npos) m_Reloads |= 1 << (phase * 2 + k);
        if (n.Weapon->CurrentState() == "Regrip") m_Reloads |= 1 << (16 + k);
    }
    // The AK probe from its front right, every 0.15 s.
    Npc& n = *probe[0];
    const float yaw = n.Body.Yaw() + glm::radians(-40.0f);
    const glm::vec3 chest = n.Feet + glm::vec3(0.0f, 1.3f, 0.0f);
    m_CamPos = chest + glm::vec3(std::sin(yaw), 0.08f, std::cos(yaw)) * 2.0f;
    const glm::vec3 d = glm::normalize(chest - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    m_HaveCam = true;
    if (t >= m_NextShot) {
        m_NextShot = t + 0.15f;
        char name[64];
        std::snprintf(name, sizeof name, "reload_%03d_%s", m_ShotIndex++, ph.Name);
        m_Shot = name;
    }
    if (t >= kReloadEnd && !m_Done) {
        m_Done = true;
        std::printf("[NpcTest] reload: worst hand off %.1f cm (%s)\n", m_WorstHand * 100.0f, m_WorstHandAt.c_str());
        Check((m_Reloads & 0x3CC) == 0x3CC, "both probes reloaded aimed, at the hip and empty");
        Check((m_Reloads >> 16 & 1) != 0, "the AK regripped");
        Check(m_WorstHand <= 0.03f, "the hands stay on the rig's through the reloads (within 3 cm)");
    }
}

// --- feet: foot IK on the Arena's ramp ---------------------------------------------------------------------------------
// One soldier, the AI frozen: each foot's height over the ground under it measured on the flat, then standing across the
// ramp (one foot uphill of the other) and facing up it. With the feet on the ground both stay at the flat's height.
namespace {
const glm::vec3 kRampCentre(0.0f, 2.0f, -7.0f); // "Plinth Ramp": 14 degrees, rising toward -z
bool FootGap(const Npc& n, const char* bone, float& gap) {
    glm::vec3 foot;
    if (!n.Body.BoneWorld(bone, foot)) return false;
    const float o[3] = {foot.x, foot.y + 0.5f, foot.z}, d[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    if (!PhysicsWorld::RaycastSolid(o, d, 1.5f, f, hit) || !hit.Hit) return false;
    gap = foot.y - hit.Point[1];
    return true;
}
} // namespace

void NpcTest::Feet(NpcDirector& npcs, float now) {
    npcs.Frozen = true;
    npcs.HoldFire = true;
    npcs.FootIKEverywhere = true;
    Npc* n = nullptr;
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && (m_FProbe.empty() || up->Name == m_FProbe)) { n = up.get(); break; }
    if (!n || n->Cct == PhysicsWorld::kNoCharacter) return;
    m_FProbe = n->Name;
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead) { up->Intent = NpcIntent{}; up->Intent.LookPoint = up->Feet + glm::vec3(0.0f, 1.6f, 10.0f); }
    // Stages: 0 flat (measure at 2.5 s), 1 across the ramp (placed at 3 s, measured at 5.5 s), 2 up it (6 s, 8.5 s).
    struct Stage { float Place, Measure; glm::vec3 Look; const char* Name; };
    static const Stage kStages[3] = {{-1.0f, 2.5f, glm::vec3(0.0f, 0.0f, 10.0f), "flat"},
                                     {3.0f, 5.5f, glm::vec3(10.0f, 0.0f, 0.0f), "across"},
                                     {6.0f, 8.5f, glm::vec3(0.0f, 0.0f, -10.0f), "uphill"}};
    for (int s = 0; s < 3; ++s) {
        const Stage& st = kStages[s];
        if (st.Place >= 0.0f && now >= st.Place && m_FStage < s) {
            m_FStage = s;
            const float p[3] = {kRampCentre.x, kRampCentre.y, kRampCentre.z};
            PhysicsWorld::SetNpcFootPosition(n->Cct, p);
        }
    }
    const Stage& cur = kStages[std::max(m_FStage, 0)];
    n->Intent.LookPoint = n->Feet + glm::vec3(0.0f, 1.6f, 0.0f) + cur.Look;
    for (int s = 0; s < 3; ++s) {
        const Stage& st = kStages[s];
        if (now < st.Measure || m_FMeasured > s) continue;
        m_FMeasured = s + 1;
        float l = 0.0f, r = 0.0f;
        const bool ok = FootGap(*n, "foot_l", l) && FootGap(*n, "foot_r", r);
        std::printf("[NpcTest] feet %-6s at (%.2f, %.2f, %.2f): left %.1f cm, right %.1f cm over the ground\n", st.Name, n->Feet.x, n->Feet.y,
                    n->Feet.z, l * 100.0f, r * 100.0f);
        if (s == 0) {
            m_FFlat = 0.5f * (l + r);
            Check(ok, "the feet were measured on the flat");
        } else {
            Check(ok && n->Feet.y > 0.3f, std::string("the soldier stands on the ramp (") + st.Name + ")");
            Check(ok && std::abs(l - m_FFlat) < 0.04f && std::abs(r - m_FFlat) < 0.04f,
                  std::string("both feet on the ramp's surface, ") + st.Name + " (within 4 cm of the flat's " +
                      std::to_string((int)std::lround(m_FFlat * 100.0f)) + " cm)");
        }
        m_Shot = std::string("feet_") + st.Name;
    }
    // The Scene view on its legs, side on (for the shots).
    {
        const float yaw = n->Body.Yaw() + glm::radians(-90.0f);
        const glm::vec3 hip = n->Feet + glm::vec3(0.0f, 0.6f, 0.0f);
        m_CamPos = hip + glm::vec3(std::sin(yaw), 0.15f, std::cos(yaw)) * 2.4f;
        const glm::vec3 d = glm::normalize(hip - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
    }
    if (now >= 9.0f && !m_Done) {
        m_Done = true;
        Check(m_FMeasured == 3, "every stage ran");
    }
}

// --- blood: the volumetric blood on every kind of hit ----------------------------------------------
// scenes/BloodTest.json: four soldiers in a line with a wall 1.5 m behind them, frozen, holding fire. One is wounded in
// the chest, one killed through the chest (the spray meets the wall), one through the head, and the chest kill's
// corpse is shot. The Scene view watches each from the side; shots at the moments the spray is up, falling and down.
void NpcTest::Blood(World& world, NpcDirector& npcs, float now) {
    npcs.HoldFire = true;
    npcs.NoLod = true;
    npcs.Frozen = true;
    m_Shot.clear();
    struct Case { const char* Name; int Part; float Damage; bool Corpse; };
    static const Case kCases[] = {{"wound", 1, 12.0f, false},
                                  {"chest_kill", 1, 400.0f, false},
                                  {"head_kill", 2, 400.0f, false},
                                  {"corpse", 1, 30.0f, true},
                                  {"point_blank", 1, 12.0f, false}}; // a round from a metre away: back onto the gun
    constexpr int kCaseCount = (int)(sizeof(kCases) / sizeof(kCases[0]));
    static const float kShotTimes[] = {0.1f, 0.3f, 0.6f, 1.0f, 1.8f};
    constexpr int kShotCount = (int)(sizeof(kShotTimes) / sizeof(kShotTimes[0]));
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    // The Scene view off the soldier's left, a little in front, level with the chest.
    auto frame = [&](const Npc& n) {
        glm::vec3 chest = n.Feet + glm::vec3(0.0f, 1.2f, 0.0f);
        if (n.Hitboxes && n.Hitboxes->Active()) chest = n.Hitboxes->Centre(n.Body, 1);
        if (m_BCamSet) return;
        m_BCamSet = true;
        // Square to the spray's line (it leaves along -Z toward the wall), a little in front and above.
        m_CamPos = chest + glm::vec3(-2.6f, 0.4f, 0.1f);
        const glm::vec3 d = glm::normalize(chest + glm::vec3(0.0f, -0.45f, -0.8f) - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
    };
    switch (m_BStep) {
    case 0: { // a line of soldiers with hitboxes, sorted west to east
        std::vector<const Npc*> ready;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Hitboxes && up->Hitboxes->Active()) ready.push_back(up.get());
        if (now < 3.0f || ready.size() < 3) break;
        std::sort(ready.begin(), ready.end(), [](const Npc* a, const Npc* b) { return a->Feet.x < b->Feet.x; });
        m_BLine.clear();
        for (const Npc* n : ready) m_BLine.push_back(n->Name);
        Check(BloodStats && BloodStats->Loaded, "the volumetric blood data is imported and loaded");
        m_BStep = 1;
        m_BAt = now;
        break;
    }
    case 1: { // the cases, 3 s apart
        if (m_BCase >= kCaseCount) { m_BStep = 4; m_BAt = now; m_BSurface = 0; m_BFired = false; break; }
        const Case& c = kCases[m_BCase];
        // The wound and the kills each on their own soldier; the corpse is the chest kill's.
        const bool pointBlank = std::string(c.Name) == "point_blank";
        const std::string& who = m_BLine[std::min<size_t>(c.Corpse ? 1 : pointBlank ? 3 : (size_t)m_BCase, m_BLine.size() - 1)];
        Npc* n = find(who);
        if (!n) { ++m_BCase; break; }
        if (!m_BFired) {
            if (now - m_BAt < 0.5f) break;
            m_BCamSet = false;
            frame(*n);
            FirstPersonWeaponGameplay w;
            w.Damage = c.Damage;
            w.FalloffStart = 1000.0f;
            w.FalloffEnd = 2000.0f;
            w.FalloffMin = 1.0f;
            glm::vec3 target = n->Feet + glm::vec3(0.0f, 1.2f, 0.0f);
            if (!c.Corpse && n->Hitboxes && n->Hitboxes->Active()) target = n->Hitboxes->Centre(n->Body, c.Part);
            if (c.Corpse && n->Ragdoll) target = n->Ragdoll->PartPosition(c.Part);
            // From the player's side of the line (+Z), level: out through the back, toward the wall.
            const glm::vec3 dir = c.Corpse ? glm::normalize(glm::vec3(0.0f, -0.6f, -1.0f)) : glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 origin = target - dir * (pointBlank ? 1.0f : 4.0f);
            m_BGearBefore = BloodStats ? BloodStats->GearSplats : 0;
            bool killed = false, head = false;
            const bool hit = npcs.OnPlayerHit(world, RootId(*n), target, origin, dir, w, &killed, &head);
            m_BSpawnBefore = BloodStats ? BloodStats->SpraysSpawned : 0;
            m_BPuffsBefore = BloodStats ? BloodStats->Puffs : 0;
            m_BHeadshotsBefore = BloodStats ? BloodStats->BigHeadshots : 0;
            m_BFired = true;
            m_BFiredAt = now;
            m_BShot = 0;
            std::printf("[NpcTest] blood %s: %s hit=%d killed=%d head=%d\n", c.Name, who.c_str(), hit ? 1 : 0, killed ? 1 : 0, head ? 1 : 0);
            break;
        }
        frame(*n);
        const float t = now - m_BFiredAt;
        if (m_BShot < kShotCount && t >= kShotTimes[m_BShot]) {
            char name[64];
            std::snprintf(name, sizeof(name), "blood_%d_%s_%02d", m_BCase, c.Name, (int)(kShotTimes[m_BShot] * 10.0f + 0.5f));
            m_Shot = name;
            ++m_BShot;
        }
        if (!m_BChecked && t > 0.05f && BloodStats) {
            // The hit threw a spray (the director queued the flesh hit; the host drained it into the blood).
            m_BChecked = true;
            Check(BloodStats->SpraysSpawned > m_BSpawnBefore,
                  std::string("blood ") + c.Name + ": a spray thrown (" + std::to_string(BloodStats->SpraysSpawned - m_BSpawnBefore) + ")");
            if (std::string(c.Name) == "chest_kill") {
                Check(BloodStats->LastSprayClipped, "blood chest_kill: the spray meets the wall behind (clip plane set)");
                Check(BloodStats->LastExited && BloodStats->LastExitFound, "blood chest_kill: the round exits where his back really is");
            }
            if (!c.Corpse)
                Check(BloodStats->Puffs > m_BPuffsBefore, std::string("blood ") + c.Name + ": the impact puff (burst, mist, droplets)");
            if (std::string(c.Name) == "head_kill")
                Check(BloodStats->BigHeadshots > m_BHeadshotsBefore, "blood head_kill: a big headshot (the gore sound over the flesh hit)");
            if (pointBlank)
                Check(BloodStats->GearSplats > m_BGearBefore,
                      "blood point_blank: blood back on the player's gun / hands (" + std::to_string(BloodStats->GearSplats - m_BGearBefore) + ")");
        }
        if (t >= 3.0f) { ++m_BCase; m_BFired = false; m_BChecked = false; m_BAt = now; }
        break;
    }
    case 2: // every spray has fallen and is gone; the pools have spread: an overview of what's left
        if (now - m_BAt < 5.0f && !m_BLine.empty()) {
            // Close on the wounded soldier (still standing): the entry wound and the run below it.
            if (const Npc* w = find(m_BLine[0])) {
                const glm::vec3 chest = w->Feet + glm::vec3(0.0f, 1.25f, 0.0f);
                m_CamPos = chest + glm::vec3(0.35f, 0.15f, 1.15f);
                const glm::vec3 d = glm::normalize(chest - glm::vec3(0.0f, 0.1f, 0.0f) - m_CamPos);
                m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
                m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
                m_HaveCam = true;
                if (!m_BCloseUp && now - m_BAt > 4.0f) { m_Shot = "blood_wounded"; m_BCloseUp = true; }
            }
            break;
        }
        if (now - m_BAt < 16.0f) {
            m_CamPos = glm::vec3(-2.5f, 3.2f, 0.5f);
            const glm::vec3 d = glm::normalize(glm::vec3(0.5f, 0.0f, -5.0f) - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            m_HaveCam = true;
            if (!m_BOverview && now - m_BAt > 15.0f) { m_Shot = "blood_overview"; m_BOverview = true; }
            break;
        }
        if (BloodStats) {
            std::printf("[NpcTest] blood: %d sprays thrown, %d still playing, GPU %.3f ms avg / %.3f ms worst frame\n",
                        BloodStats->SpraysSpawned, BloodStats->ActiveSprays, BloodStats->GpuMsAvg, BloodStats->GpuMsMax);
            Check(BloodStats->ActiveSprays == 0, "every spray played out and was dropped");
            std::printf("[NpcTest] blood: %d stains, %d pools\n", BloodStats->DecalsSpawned, BloodStats->PoolsSpawned);
            Check(BloodStats->DecalsSpawned >= 6, "the blood left stains (floor splats, wall spatter)");
            Check(BloodStats->PoolsSpawned >= 2, "a pool spread under each of the two corpses");
            std::printf("[NpcTest] blood: %d splats on bodies\n", BloodStats->SplatsSpawned);
            Check(BloodStats->SplatsSpawned >= 4, "the hits left blood on the bodies (entry, exit, the run below)");
        }
        m_Done = true;
        m_BStep = 3;
        break;
    case 4: { // rounds into each surface panel: its own burst and its own textured hole
        static const char* const kPanels[] = {"Metal Plate", "Wood Panel", "Brick Panel", "Glass Pane", "Mud Bank", "Tile Panel", "Concrete Block"};
        constexpr int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
        if (m_BSurface >= kPanelCount || !ShootWorld || !BloodStats) { m_BStep = 2; m_BAt = now; break; }
        glm::vec3 centre(0.0f);
        bool found = false;
        for (auto [e, name] : world.Registry.view<NameComponent>().each())
            if (name.Name == kPanels[m_BSurface]) { centre = glm::vec3(world.GetCachedWorldTransform(e)[3]); found = true; }
        if (!found) { ++m_BSurface; break; }
        // Square on, from the room side (+X), a little above.
        m_CamPos = centre + glm::vec3(1.6f, 0.35f, 0.25f);
        const glm::vec3 look = glm::normalize(centre - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        const float t = now - m_BAt;
        if (!m_BFired && t > 0.4f) {
            m_BImpactsBefore = BloodStats->Impacts;
            m_BHolesBefore = BloodStats->TexturedHoles;
            for (int k = 0; k < 3; ++k) // three rounds, a hand's width apart
                ShootWorld(centre + glm::vec3(3.0f, 0.25f - 0.2f * (float)k, -0.2f + 0.2f * (float)k), glm::vec3(-1.0f, 0.0f, 0.0f));
            m_BFired = true;
            m_BFiredAt = now;
        }
        if (m_BFired) {
            const float s = now - m_BFiredAt;
            std::string panel = kPanels[m_BSurface];
            for (char& c : panel) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
            if (s >= 0.08f && s - (float)(1.0 / 60.0) < 0.08f) m_Shot = "surface_" + panel + "_burst";
            if (s >= 1.6f) {
                m_Shot = "surface_" + panel + "_hole";
                Check(BloodStats->Impacts >= m_BImpactsBefore + 3 && BloodStats->TexturedHoles >= m_BHolesBefore + 3,
                      std::string("surface ") + kPanels[m_BSurface] + ": its burst and textured holes (" +
                          std::to_string(BloodStats->TexturedHoles - m_BHolesBefore) + ")");
                ++m_BSurface;
                m_BFired = false;
                m_BAt = now;
            }
        }
        break;
    }
    default:
        break;
    }
}

// --- ragdoll: the deaths, watched from four sides ------------------------------------------------------------------------
namespace {
// scenes/RagdollTest.json's NPC Spawns, in order.
enum RagSpawn { RS_Open, RS_Ak, RS_Shotgun, RS_Crate, RS_Stairs, RS_Ledge, RS_Wall, RS_Sprinter, RS_Jogger, RS_Strafer, RS_PileA, RS_PileB, RS_Count };
struct RagCase {
    const char* Name;
    int Spawn;
    int Part;        // the hitbox part struck
    float From;      // degrees round the body the round comes from: 0 its front, 90 its left, 180 behind, -90 its right
    float Damage;
    int Move;        // 0 stands, 1 sprints, 2 jogs, 3 strafes
    glm::vec3 MoveTo;
};
const RagCase kRagCases[] = {
    {"open_head_front", RS_Open, 2, 0.0f, 120.0f, 0, {}},
    {"ak_chest_left", RS_Ak, 1, 90.0f, 45.0f, 0, {}},
    {"shotgun_back", RS_Shotgun, 1, 180.0f, 150.0f, 0, {}},
    {"crate_chest_front", RS_Crate, 1, 0.0f, 150.0f, 0, {}},
    {"stairs_back", RS_Stairs, 1, 180.0f, 60.0f, 0, {}},
    {"ledge_back", RS_Ledge, 0, 180.0f, 60.0f, 0, {}},
    {"wall_chest_front", RS_Wall, 1, 0.0f, 60.0f, 0, {}},
    {"sprint_thigh", RS_Sprinter, 7, 0.0f, 45.0f, 1, {9.0f, 0.0f, 4.0f}},
    {"jog_chest_right", RS_Jogger, 1, -90.0f, 45.0f, 2, {-9.0f, 0.0f, 8.0f}},
    {"strafe_head", RS_Strafer, 2, 0.0f, 120.0f, 3, {}},
    {"pile_first", RS_PileA, 1, 0.0f, 45.0f, 0, {}},
    {"pile_onto", RS_PileB, 1, 0.0f, 60.0f, 0, {}},
};
constexpr int kRagCaseCount = (int)(sizeof(kRagCases) / sizeof(kRagCases[0]));
// Moments (seconds after the round) each death is frozen at and shot; then once more at rest.
constexpr float kRagMoments[] = {0.15f, 0.4f, 0.8f, 1.6f};
constexpr int kRagMomentCount = (int)(sizeof(kRagMoments) / sizeof(kRagMoments[0]));
constexpr float kRagRestBy = 7.0f;   // a body still moving this long after the round fails the rest check
struct RagView { const char* Name; float Yaw, Pitch, Dist; bool Ground; };
const RagView kRagViews[4] = {{"front", 0.0f, -14.0f, 1.9f, false}, {"side", 90.0f, -14.0f, 1.9f, false},
                              {"high", 225.0f, -55.0f, 2.1f, false}, {"ground", 150.0f, 0.0f, 1.8f, true}};
constexpr int kFramesPerView = 3; // the camera is placed, the next frame draws with it, the third is kept

Npc* NpcAtSpawn(NpcDirector& npcs, int spawn) {
    for (const auto& up : npcs.Npcs())
        if (up && up->SpawnIndex == spawn) return up.get();
    return nullptr;
}
glm::vec3 BodyCentre(const Npc& n) {
    return n.Ragdoll ? 0.5f * (n.Ragdoll->PartPosition(0) + n.Ragdoll->PartPosition(1)) : n.Feet + glm::vec3(0.0f, 1.0f, 0.0f);
}
} // namespace

void NpcTest::Ragdoll(World& world, NpcDirector& npcs, const Player& player) {
    (void)player;
    const float now = npcs.Now();
    npcs.HoldFire = true;
    npcs.NoLod = true;
    m_Shot.clear();
    if (m_RCase < 0) {
        // Every soldier in before the first death.
        int alive = 0;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead) ++alive;
        if (now < 2.0f || alive < RS_Count) {
            if (now > 20.0f && !m_Done) {
                Check(false, "every RagdollTest soldier spawned (" + std::to_string(alive) + " of " + std::to_string(RS_Count) + ")");
                m_Done = true;
            }
            return;
        }
        m_RCase = 0;
        m_RStage = 0;
        m_RAt = now;
    }
    // Every corpse and gun that goes must have been out of sight for Unseen Time first.
    {
        static std::vector<std::pair<std::string, float>> seen; // name -> last time the player could see it (-1: never checked)
        for (auto& [name, last] : seen) {
            bool there = false;
            for (const auto& up : npcs.Npcs())
                if (up && up->Name == name) there = true;
            if (!there && last > -1e8f) {
                Check(now - last >= npcs.Settings().UnseenTime - 0.05f,
                      name + "'s corpse went only after " + std::to_string(npcs.Settings().UnseenTime) + " s out of sight (" + std::to_string(now - last) + " s)");
                last = -1e9f;
            }
        }
        PlayerSnapshot ps;
        ps.Valid = true;
        ps.Eye = player.Cam.Position;
        ps.Forward = player.Cam.Front();
        for (const auto& up : npcs.Npcs()) {
            if (!up || !up->Dead || !up->Ragdoll) continue;
            auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& e) { return e.first == up->Name; });
            if (it == seen.end()) { seen.push_back({up->Name, now}); it = seen.end() - 1; }
            for (int part : {0, 1, 2})
                if (NpcDirector::PlayerSees(ps, up->Ragdoll->PartPosition(part))) it->second = now;
        }
    }

    // A frozen moment: four views of the body, then time goes on.
    if (m_RFreeze) {
        Npc* n = m_RName.empty() ? nullptr : NpcAtSpawn(npcs, m_RCase < kRagCaseCount ? kRagCases[m_RCase].Spawn : RS_Open);
        const int view = m_RFrame / kFramesPerView, sub = m_RFrame % kFramesPerView;
        if (!n || view >= 4) {
            m_RFreeze = false;
            ++m_RMoment;
            return;
        }
        const RagView& v = kRagViews[view];
        const glm::vec3 c = BodyCentre(*n);
        const float yaw = m_RYaw + glm::radians(v.Yaw);
        const glm::vec3 flat(std::sin(yaw), 0.0f, std::cos(yaw));
        const float pitch = glm::radians(v.Pitch);
        m_CamPos = c + (flat * std::cos(pitch) - glm::vec3(0.0f, std::sin(pitch), 0.0f)) * v.Dist;
        if (v.Ground) {
            float lowest = c.y;
            if (n->Ragdoll)
                for (int k = 0; k < NpcRagdoll::kRagParts; ++k) lowest = std::min(lowest, n->Ragdoll->PartPosition(k).y);
            m_CamPos.y = lowest + 0.15f;
        }
        const glm::vec3 d = glm::normalize(c - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        if (sub == kFramesPerView - 1) {
            char name[128];
            const bool rest = m_RMoment >= kRagMomentCount;
            char when[16];
            if (rest) std::snprintf(when, sizeof when, "rest");
            else std::snprintf(when, sizeof when, "%04dms", (int)std::lround(kRagMoments[m_RMoment] * 1000.0f));
            std::snprintf(name, sizeof name, "rag_%02d_%s_%s_%s", m_RCase, m_RName.c_str(), when, v.Name);
            m_Shot = name;
        }
        ++m_RFrame;
        return;
    }

    // The persistence stage, after every death.
    if (m_RCase >= kRagCaseCount) {
        if (m_RLookAway == 0) {
            // Looking at the bodies until the last is past Corpse Time: all still there (the check above fails any that went while seen).
            if (now - m_RLastDeath > npcs.Settings().CorpseTime + 1.0f) {
                int corpses = 0;
                for (const auto& up : npcs.Npcs())
                    if (up && up->Dead) ++corpses;
                std::printf("[NpcTest] ragdoll: %d corpse(s) and %d loose gun(s) while looked at, %.0f s after the first death\n", corpses,
                            npcs.LooseGuns(), now - m_RKilledAt);
                m_RLookAway = 1;
                m_RLookAwayAt = now;
                m_RViewYaw = 90.0f; // turn round: nothing of them in view
            }
        } else if (now - m_RLookAwayAt > npcs.Settings().UnseenTime + 1.5f) {
            int corpses = 0;
            for (const auto& up : npcs.Npcs())
                if (up && up->Dead) ++corpses;
            Check(corpses == 0, "out of sight past Corpse Time, every corpse has gone (" + std::to_string(corpses) + " left)");
            Check(npcs.LooseGuns() == 0, "out of sight past their Lifetime, every dropped gun has gone (" + std::to_string(npcs.LooseGuns()) + " left)");
            std::printf("[NpcTest] ragdoll worst: self %.1f cm (%s), ground %.1f cm (%s), joint %.1f deg (%s)\n", m_RWorstSelf * 100.0f,
                        m_RWorstSelfAt.c_str(), m_RWorstGround * 100.0f, m_RWorstGroundAt.c_str(), m_RWorstJoint, m_RWorstJointAt.c_str());
            PrintCosts(npcs);
            m_Done = true;
            m_Duration = now;
        }
        return;
    }

    const RagCase& rc = kRagCases[m_RCase];
    Npc* n = NpcAtSpawn(npcs, rc.Spawn);
    if (!n) {
        Check(false, std::string(rc.Name) + ": its soldier is there");
        ++m_RCase;
        m_RStage = 0;
        m_RAt = now;
        return;
    }
    // The soldier's part centre (its hitbox's middle: the bone to the next).
    auto partCentre = [&](int part) {
        const NpcPartDef& d = NpcPartDefOf(part);
        glm::vec3 a(0.0f), b(0.0f);
        if (!n->Body.BoneWorld(world, d.Bone, a)) return n->Feet + glm::vec3(0.0f, 1.2f, 0.0f);
        if (d.End && n->Body.BoneWorld(world, d.End, b)) return 0.5f * (a + b);
        return a + glm::vec3(0.0f, 0.08f, 0.0f);
    };
    if (m_RStage == 0) {
        // Getting going (the movers), then the round.
        const float t = now - m_RAt;
        const float yaw0 = n->Body.Yaw();
        const glm::vec3 fwd0(std::sin(yaw0), 0.0f, std::cos(yaw0));
        if (rc.Move != 0 && !n->Dead) {
            NpcIntent in;
            in.Move = true;
            if (rc.Move == 3) {
                in.MoveTarget = n->Feet + glm::vec3(4.0f, 0.0f, 0.0f);
                in.Pace = Gait::Walk;
                in.Aim = true;
                in.FaceAim = true;
                in.AimPoint = in.LookPoint = n->Feet + glm::vec3(0.0f, 1.45f, 10.0f);
            } else {
                in.MoveTarget = rc.MoveTo;
                in.Pace = rc.Move == 1 ? Gait::Run : Gait::Jog;
                in.LookPoint = rc.MoveTo + glm::vec3(0.0f, 1.6f, 0.0f);
            }
            n->Intent = in;
        }
        const float speed = glm::length(glm::vec2(n->Velocity.x, n->Velocity.z));
        const float want = rc.Move == 1 ? 3.8f : rc.Move == 2 ? 2.4f : rc.Move == 3 ? 1.0f : 0.0f;
        const bool ready = rc.Move == 0 ? t > 0.6f : ((speed >= want && t > 1.0f) || t > 4.0f);
        if (!ready) return;
        // The round: along its line through the part, from the side the case names.
        const float from = glm::radians(rc.From);
        const glm::vec3 left0(std::cos(yaw0), 0.0f, -std::sin(yaw0));
        const glm::vec3 travel = -(fwd0 * std::cos(from) + left0 * std::sin(from));
        const glm::vec3 point = partCentre(rc.Part);
        std::printf("[NpcTest] ragdoll %s: %s killed at %.1f m/s by a %.0f-damage round into %s from %.0f deg\n", rc.Name, n->Name.c_str(), speed,
                    rc.Damage, NpcPartDefOf(rc.Part).Bone, rc.From);
        npcs.Kill(world, *n, travel, point, 30.0f + 0.4f * rc.Damage, rc.Part);
        m_RName = rc.Name;
        m_RYaw = yaw0; // (the views turn about the body's facing at death)
        if (m_RCase == 0) m_RKilledAt = now;
        m_RLastDeath = now;
        m_RAt = now;
        m_RStage = 1;
        m_RMoment = 0;
        m_RCaseSelf = m_RCaseGround = m_RCaseJoint = m_RGroundNow = 0.0f;
        m_RCaseGroundAt.clear();
        m_RCaseJointAt.clear();
        return;
    }
    // Falling and lying: every frame's checks, the frozen moments, then the case's verdict.
    const float t = now - m_RAt;
    if (n->Ragdoll) {
        const int id = n->Ragdoll->Id();
        int a = -1, b = -1;
        const float self = PhysicsWorld::RagdollWorstSelfPenetration(id, &a, &b);
        if (self > m_RCaseSelf) m_RCaseSelf = self;
        if (self > m_RWorstSelf) {
            m_RWorstSelf = self;
            m_RWorstSelfAt = std::string(rc.Name) + " " + NpcRagdollDefOf(a).Bone + "/" + NpcRagdollDefOf(b).Bone + " at " + std::to_string(t) + " s";
        }
        float groundNow = 0.0f;
        for (int k = 0; k < NpcRagdoll::kRagParts; ++k) {
            // Into the ground: the capsule's lowest point against the solid world under it.
            float p[3], q[4], r = 0.05f, hl = 0.1f;
            if (!PhysicsWorld::GetRagdollPart(id, k, p, q) || !PhysicsWorld::GetRagdollPartShape(id, k, &r, &hl)) continue;
            const glm::vec3 c(p[0], p[1], p[2]), axis = glm::quat(q[3], q[0], q[1], q[2]) * glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 end = axis.y < 0.0f ? c + axis * hl : c - axis * hl;
            const float o[3] = {end.x, end.y + 0.4f, end.z}, down[3] = {0.0f, -1.0f, 0.0f};
            RaycastHit hit;
            // (Only a point really inside something counts: a hand hanging past a ledge's edge is under its top, not in it.)
            const float bottom[3] = {end.x, end.y - r + 0.005f, end.z}, probe[3] = {0.0f, -1.0f, 0.0f};
            RaycastHit inside;
            const bool in = PhysicsWorld::RaycastSolid(bottom, probe, 0.001f, QueryFilter{}, inside) && inside.Hit && inside.Distance < 1e-4f;
            if (in && PhysicsWorld::RaycastSolid(o, down, 2.0f, QueryFilter{}, hit) && hit.Hit && hit.Distance > 1e-3f) {
                const float into = hit.Point[1] - (end.y - r);
                groundNow = std::max(groundNow, into);
                if (into > m_RCaseGround) { m_RCaseGround = into; m_RCaseGroundAt = std::string(NpcRagdollDefOf(k).Bone) + " at " + std::to_string(t).substr(0, 4) + " s"; }
                if (into > m_RWorstGround) { m_RWorstGround = into; m_RWorstGroundAt = std::string(rc.Name) + " " + NpcRagdollDefOf(k).Bone; }
            }
            // Past its range.
            float pos[3], lo[3], hi[3];
            if (k > 0 && PhysicsWorld::GetRagdollJoint(id, k, pos, lo, hi))
                for (int ax = 0; ax < 3; ++ax) {
                    const float over = glm::degrees(std::max(lo[ax] - pos[ax], pos[ax] - hi[ax]));
                    if (over > m_RCaseJoint) {
                        static const char* const kAxisName[3] = {"twist", "swing y", "swing z"};
                        m_RCaseJoint = over;
                        m_RCaseJointAt = std::string(NpcRagdollDefOf(k).Bone) + " " + kAxisName[ax] + " at " + std::to_string(t).substr(0, 4) + " s";
                    }
                    if (over > m_RWorstJoint) {
                        static const char* const kAxes[3] = {"twist", "swing y", "swing z"};
                        m_RWorstJoint = over;
                        m_RWorstJointAt = std::string(rc.Name) + " " + NpcRagdollDefOf(k).Bone + " " + kAxes[ax];
                    }
                }
        }
        m_RGroundNow = groundNow;
    }
    // (At rest: the body asleep, and its gun too: a gun thrown clear may still be rolling when the body goes to sleep.)
    const bool rest = n->Ragdoll && ((n->Ragdoll->Asleep() && (!n->Dropped || n->Dropped->Asleep())) || t > kRagRestBy) && t > 2.0f;
    if (m_RMoment < kRagMomentCount ? t >= kRagMoments[m_RMoment] : (rest && m_RMoment == kRagMomentCount)) {
        m_RFreeze = true;
        m_RFrame = 0;
        return;
    }
    if (m_RMoment <= kRagMomentCount) return; // the rest moment is still to come
    // The verdict.
    const std::string nm = rc.Name;
    Check(n->Ragdoll != nullptr, nm + ": a ragdoll took the body");
    if (n->Ragdoll) {
        std::printf("[NpcTest] ragdoll %s: self %.1f cm, ground %.1f cm (%s), joints %.1f deg past range (%s), %s after %.2f s (grounded at %.2f s)\n",
                    rc.Name, m_RCaseSelf * 100.0f, m_RCaseGround * 100.0f, m_RCaseGroundAt.c_str(), m_RCaseJoint, m_RCaseJointAt.c_str(),
                    n->Ragdoll->Asleep() ? "asleep" : "still moving", t, n->Ragdoll->Motor().GroundedAt());
        Check(m_RCaseSelf < 0.03f, nm + ": no part of the body inside another (worst " + std::to_string(m_RCaseSelf * 100.0f) + " cm)");
        Check(m_RCaseGround < 0.06f, nm + ": no part driven into the ground at impact (worst " + std::to_string(m_RCaseGround * 100.0f) + " cm)");
        Check(m_RGroundNow < 0.01f, nm + ": at rest, nothing inside the ground (" + std::to_string(m_RGroundNow * 100.0f) + " cm)");
        Check(m_RCaseJoint < 15.0f, nm + ": no joint bent past its range (worst " + std::to_string(m_RCaseJoint) + " deg over, " + m_RCaseJointAt + ")");
        Check(n->Ragdoll->Asleep(), nm + ": the body comes to rest and sleeps within " + std::to_string((int)kRagRestBy) + " s");
        if (!n->Ragdoll->Asleep()) {
            // What keeps it up: the fastest part, and the gun's own motion.
            float lin = 0.0f, ang = 0.0f;
            PhysicsWorld::RagdollMotion(n->Ragdoll->Id(), &lin, &ang);
            BodyState gs;
            const bool g = n->Dropped && PhysicsWorld::GetBodyState((unsigned)entt::to_integral(n->Dropped->Entity()), gs) && gs.Valid;
            std::printf("[NpcTest] ragdoll %s: still moving: fastest part %.3f m/s %.2f rad/s, settle %.2f, resting %d; gun %.3f m/s %.2f rad/s\n", rc.Name, lin, ang,
                        n->Ragdoll->Motor().Settle(), (int)n->Ragdoll->Motor().ForcedToRest(),
                        g ? glm::length(glm::vec3(gs.Velocity[0], gs.Velocity[1], gs.Velocity[2])) : -1.0f,
                        g ? glm::length(glm::vec3(gs.AngularVelocity[0], gs.AngularVelocity[1], gs.AngularVelocity[2])) : -1.0f);
        }
    }
    if (n->Dropped) {
        NpcDroppedWeapon& g = *n->Dropped;
        const glm::vec3 at = g.Position(world);
        const float o[3] = {at.x, at.y + 0.5f, at.z}, down[3] = {0.0f, -1.0f, 0.0f};
        RaycastHit hit;
        const float above = PhysicsWorld::RaycastSolid(o, down, 3.0f, QueryFilter{}, hit) && hit.Hit ? at.y - hit.Point[1] : 9.0f;
        const unsigned inside = n->Ragdoll ? PhysicsWorld::RagdollPartsOverlapping((unsigned)entt::to_integral(g.Entity()), n->Ragdoll->Id(), 0xFFFFu, -0.015f) : 0u;
        std::printf("[NpcTest] ragdoll %s: gun released after %.2f s (delay %.2f), %s, %.1f cm over the ground, inside parts 0x%x\n", rc.Name,
                    g.Released() ? g.ReleaseDelay() : -1.0f, g.ReleaseDelay(), g.Asleep() ? "asleep" : "moving", above * 100.0f, inside);
        Check(g.Released() && !g.Held(), nm + ": the gun left the dead hand");
        Check(g.Asleep(), nm + ": the gun came to rest");
        const bool onBody = n->Ragdoll && PhysicsWorld::RagdollPartsOverlapping((unsigned)entt::to_integral(g.Entity()), n->Ragdoll->Id(), 0xFFFFu, 0.05f) != 0u;
        Check(above < 0.15f || onBody, nm + ": the gun lies on the ground or the body, not hanging in the air (" + std::to_string(above * 100.0f) + " cm over the ground)");
        Check(inside == 0u, nm + ": the gun is not inside the body");
    }
    // On to the next.
    ++m_RCase;
    m_RStage = 0;
    m_RAt = now;
    m_RName.clear();
}
