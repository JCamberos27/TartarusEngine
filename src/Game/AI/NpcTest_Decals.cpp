// --npc-test decals (NpcTest.h): bullet holes and blood put at fixed spots on the Apartment Hallway's corridor (the north
// wall by apartment 304 and the floor in front of it), then shot from fixed Scene-view cameras. Everything is placed by
// the script on the fixed 60 Hz step, so two builds shoot the same picture: the before / after of the blood and bullet
// holes moving onto the clustered decals (docs/DECALS.md).
#include "NpcTest.h"

#include "NpcDirector.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
// The corridor's north wall face (z = +1, solid from x -10.45 to -6.36) and the floor in front of it.
constexpr float kWallZ = 0.995f;
const glm::vec3 kPlayerFeet(-9.95f, 0.02f, -0.5f); // off to the west, out of the cameras' view
constexpr float kFireAt = 2.0f;

struct DecalShot {
    const char* Name;
    float At; // seconds into Play
    glm::vec3 Eye, Target;
};
// Fresh (the stains still spreading), then late (spread, the pool grown).
const DecalShot kShots[] = {
    {"decals_wall_fresh", 6.0f, {-8.0f, 1.45f, -0.85f}, {-8.15f, 1.0f, 1.0f}},
    {"decals_holes_fresh", 7.0f, {-8.5f, 1.3f, 0.3f}, {-8.5f, 1.3f, 1.0f}},
    {"decals_floor_fresh", 8.0f, {-8.2f, 1.7f, -0.55f}, {-8.35f, 0.0f, 0.4f}},
    {"decals_wall", 33.0f, {-8.0f, 1.45f, -0.85f}, {-8.15f, 1.0f, 1.0f}},
    {"decals_holes", 34.0f, {-8.5f, 1.3f, 0.3f}, {-8.5f, 1.3f, 1.0f}},
    {"decals_floor", 35.0f, {-8.2f, 1.7f, -0.55f}, {-8.35f, 0.0f, 0.4f}},
};
constexpr int kShotCount = (int)(sizeof(kShots) / sizeof(kShots[0]));
constexpr float kSettle = 0.5f; // the camera goes in place this long before its shot (temporal effects settle)

void YawPitch(const glm::vec3& from, const glm::vec3& to, float& yaw, float& pitch) {
    const glm::vec3 d = glm::normalize(to - from);
    yaw = glm::degrees(std::atan2(d.z, d.x));
    pitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}
} // namespace

void NpcTest::DecalsDrive(Player& player, float& wantYaw, float& wantPitch) {
    if (!m_KPlaced) { // by the corridor's west end, looking along the north wall
        const float f[3] = {kPlayerFeet.x, kPlayerFeet.y, kPlayerFeet.z};
        PhysicsWorld::SetCharacterFootPosition(f);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kPlayerFeet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        player.Cam.Yaw = 45.0f;
        player.Cam.Pitch = -10.0f;
        m_KPlaced = true;
    }
    wantYaw = 45.0f; // at the wall, toward the holes and the stains
    wantPitch = -10.0f;
}

void NpcTest::Decals(World& world, NpcDirector& npcs, float) {
    (void)world;
    // Its own clock: the hallway may have no squad, and the director's clock only runs with one.
    // (The host steps Play by a fixed 1/60 s under --npc-test.)
    m_KClock += 1.0f / 60.0f;
    const float now = m_KClock;
    m_Time = now;
    npcs.HoldFire = true;
    npcs.Frozen = true;
    m_Shot.clear();
    if (m_KStep == 0 && now >= kFireAt) {
        // A loose group of rounds into the wall at chest height, square on, and three into the floor at a slant.
        int hits = 0;
        for (int i = 0; i < 12; ++i) {
            const glm::vec3 at(-8.95f + 0.13f * (float)(i % 6) + 0.04f * (float)(i / 6), 1.05f + 0.17f * (float)(i % 4), kWallZ);
            const glm::vec3 dir = glm::normalize(glm::vec3(0.05f * (float)((i % 3) - 1), -0.03f, 1.0f));
            hits += ShootWorld && ShootWorld(at - dir * 1.2f, dir) ? 1 : 0;
        }
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 at(-7.85f + 0.18f * (float)i, 0.0f, 0.15f + 0.1f * (float)i);
            const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -1.0f, 0.45f));
            hits += ShootWorld && ShootWorld(at - dir * 1.2f, dir) ? 1 : 0;
        }
        if (SpawnBlood) {
            SpawnBlood(0, glm::vec3(-7.45f, 1.3f, kWallZ), glm::vec3(0.0f, 0.0f, -1.0f), 0.6f);
            SpawnBlood(1, glm::vec3(-8.45f, 0.0f, 0.45f), glm::vec3(0.0f, 1.0f, 0.0f), 0.5f);
        }
        std::printf("[NpcTest] decals: %d of 15 rounds struck\n", hits);
        Check(hits == 15, "every round struck the wall or the floor (" + std::to_string(hits) + " of 15)");
        m_KStep = 1;
        m_KAt = now;
    }
    // The cameras: each in place kSettle before its shot.
    while (m_KShot < kShotCount && now >= kShots[m_KShot].At) {
        m_Shot = kShots[m_KShot].Name;
        ++m_KShot;
    }
    if (m_KShot < kShotCount) {
        const DecalShot& s = kShots[m_KShot];
        if (now >= s.At - kSettle) {
            m_CamPos = s.Eye;
            YawPitch(s.Eye, s.Target, m_CamYaw, m_CamPitch);
            m_HaveCam = true;
        }
    }
    if (m_KShot >= kShotCount && !m_Done) {
        m_Done = true;
        if (BloodStats) {
            Check(BloodStats->Loaded, "the blood data is loaded");
            Check(BloodStats->DecalsSpawned > 0, "blood stains landed (" + std::to_string(BloodStats->DecalsSpawned) + ")");
            Check(BloodStats->TexturedHoles > 0, "textured bullet holes on the static wall (" +
                                                     std::to_string(BloodStats->TexturedHoles) + ")");
        }
    }
}
