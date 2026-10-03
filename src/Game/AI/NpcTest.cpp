#include "NpcTest.h"

#include "Combat/CombatFx.h"
#include "DevPanel.h"
#include "Combat/PlayerVitals.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h"
#include "NpcDirector.h"
#include "PhysicsWorld.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
int PosePhaseCount(); // the pose scenario's phases (the table is with Pose below)
constexpr float kPosePhaseTime = 2.4f;
constexpr float kTacticsCloseAt = 45.0f; // tactics: the player steps up to a soldier for its rifle butt
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
    if (m_Scenario == "deaths") m_Duration = 60.0f;
    if (m_Scenario == "tactics") m_Duration = 70.0f;
    if (m_Scenario == "feet") m_Duration = 10.0f;
    if (m_Scenario == "reload") m_Duration = 28.0f;
    if (m_Scenario == "flame") m_Duration = 14.0f;
    if (const char* t = EnvVar("NPC_TEST_SECONDS")) m_Duration = std::max(5.0f, (float)std::atof(t));
    if (const char* r = EnvVar("NPC_TEST_RECORD")) m_RecordDir = r;
    std::cout << "[NpcTest] scenario '" << m_Scenario << "', " << m_Duration << " s" << std::endl;
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
        if (m_Scenario == "fight" || m_Scenario == "tactics")
            std::cout << "[NpcTest]   player target='" << m_Target << "' err=" << m_AimErr << " shots=" << m_PlayerShots
                      << " yaw=" << player.Cam.Yaw << std::endl;
        for (const auto& up : npcs.Npcs()) {
            if (!up) continue;
            const Npc& n = *up;
            const int ammo = n.Weapon && n.Weapon->IsActive() ? n.Weapon->Ammo() : -1;
            std::snprintf(line, sizeof line,
                          "[NpcTest]   %-10s %-11s ph%-2d %-10s hp%3.0f sup%.2f %s ammo%2d %s%s%s pos(%5.1f,%4.1f,%5.1f) v%3.1f anim=%s gun=%s %s%s",
                          n.Name.c_str(), BehaviourName(n.Doing), n.Phase, RoleName(n.Role), n.Health, n.Suppression,
                          n.Class == WeaponClass::Shotgun ? "870" : "AK ", ammo, n.Mem.Known ? "K" : "-", n.Mem.Visible ? "V" : "-",
                          n.HasAttackToken ? "A" : "-", n.Feet.x, n.Feet.y, n.Feet.z, glm::length(n.Velocity),
                          n.Body.StateName(world).c_str(), n.Weapon && n.Weapon->IsActive() ? n.Weapon->CurrentState().c_str() : "-",
                          n.Callout.empty() || now - n.CalloutAt > 2.0f ? "" : "\"", n.Callout.empty() || now - n.CalloutAt > 2.0f ? "" : n.Callout.c_str());
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
            std::printf("[NpcTest] sandbox: %d soldier(s) spawned, %zu spawn point(s), %zu cover points, %d radio line(s)\n", m_MaxAlive,
                        npcs.SpawnPoints().size(), npcs.Cover().Points().size(), npcs.Voice().Spoken());
            Check(m_MaxAlive >= 3, "the Sandbox squad spawned (" + std::to_string(m_MaxAlive) + " soldiers)");
            Check(npcs.Nav().Valid() && npcs.Nav().PolyCount() > 0, "a navigation mesh was built for the Sandbox");
            Check(!npcs.Cover().Points().empty(), "the Sandbox has cover (" + std::to_string(npcs.Cover().Points().size()) + " points)");
            Check(m_DevKilledCount >= 3 && m_AliveAfterKill == 0, "Kill All killed the squad (" + std::to_string(m_DevKilledCount) + ", " + std::to_string(m_AliveAfterKill) + " left)");
            CheckRadio(npcs);
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
        CheckRadio(npcs);
        Check(npcs.Voice().Spoken() > 0, "the squad used the radio (" + std::to_string(npcs.Voice().Spoken()) + " lines)");
        if (m_Scenario == "die") {
            Check(m_PlayerDied, "the player was killed");
            Check(m_PlayerRespawned, "the player respawned");
        }
    }
}

// Radio: no squad ever had two lines on air at once (a cut line counts up to where it was cut), and the
// lines spoken came from more than one kind of event.
void NpcTest::CheckRadio(NpcDirector& npcs) {
    int overlaps = 0;
    for (int s = 0; s < 4; ++s) overlaps += npcs.Voice().FirstOverlap(s) >= 0 ? 1 : 0;
    bool seen[(int)Bark::Count] = {};
    int kinds = 0;
    for (const BarkPlayed& b : npcs.Voice().History()) {
        if (!seen[(int)b.Event]) { seen[(int)b.Event] = true; ++kinds; }
        std::printf("[NpcTest] radio t=%6.2f squad %d UNIT-%d %-13s p%d%s%s \"%s\"\n", b.Start, b.Squad, b.Unit, BarkKey(b.Event), b.Priority,
                    b.Responder ? " (copy)" : "", b.Cut >= 0.0f ? " (cut)" : "", b.Text.c_str());
    }
    Check(overlaps == 0, "no squad had two radio lines on air at once");
    Check(kinds >= 2, "the radio carried " + std::to_string(kinds) + " kinds of call");
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
} // namespace

void NpcTest::Deaths(World& world, NpcDirector& npcs, float now) {
    (void)world;
    npcs.HoldFire = true;
    npcs.NoLod = true;
    npcs.TrackDeathPop = true;
    npcs.SetWoundChance(1.0f);
    npcs.Frozen = m_DStep < 4; // still for the rays and the kills; the brain runs for the wounded
    m_Shot.clear();
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
            if (PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == 1 && bp.Part == part && bp.Entity == RootId(*a)) ++right;
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
            for (int k = 0; k < NpcRagdoll::kParts; ++k) m_DCorpseBefore[k] = corpse->Ragdoll->PartPosition(k);
            for (int k = 0; k < 4; ++k) corpse->Body.BoneWorld(kCorpseBones[k], m_DBoneBefore[k]);
            // From the side at the chest part's middle.
            const glm::vec3 c = corpse->Ragdoll->PartPosition(1);
            const float o[3] = {c.x - 3.0f, c.y, c.z}, d[3] = {1.0f, 0.0f, 0.0f};
            PhysicsWorld::BodyPartHit bp;
            const bool part = PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == 2 && bp.Entity == RootId(*corpse);
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
    case 4: { // two soldiers go down wounded
        Npc* a = fresh();
        Npc* b = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && up.get() != a && !up->Dead && !up->Wounded && !used(*up) && up->Hitboxes && up->Hitboxes->Active() &&
                up->Cct != PhysicsWorld::kNoCharacter)
                b = up.get();
        if (!a || !b) { if (now - m_DAt > 25.0f) { Check(false, "two soldiers to wound"); step(7); } break; }
        for (Npc* n : {a, b}) {
            m_DUsed.push_back(n->Name);
            n->Health = 30.0f;
            w.Damage = 12.0f;
            shoot(*n, 1, 12.0f); // the chest: 18 left, under 20%
        }
        m_DNpc[0] = a->Name;
        m_DNpc[1] = b->Name;
        Check(a->Wounded && b->Wounded, "a body hit under 20% health can leave a soldier wounded (the chance forced to 1)");
        Check(a->Callout == "Unit down, need assist", "a wounded soldier calls 'Unit down, need assist'");
        Check(a->Doing == Behaviour::Wounded, "it is in the Wounded behaviour");
        m_DWoundSpeed = 0.0f;
        step(5);
        break;
    }
    case 5: { // crawling; then the next hit kills one, the other bleeds out
        Npc* a = find(m_DNpc[0]);
        Npc* b = find(m_DNpc[1]);
        if (!a || !b) { step(7); break; }
        const float t = now - m_DAt;
        if (!a->Dead && t > 0.5f) m_DWoundSpeed = std::max(m_DWoundSpeed, glm::length(a->Velocity));
        if (!m_DKneelChecked && t > 0.5f) {
            m_DKneelChecked = true;
            Check(a->Intent.Crouch && a->Crouched, "a wounded soldier is down on a knee (crouched)");
        }
        if (t > 4.0f && !a->Dead && a->Wounded) {
            std::printf("[NpcTest] wounded: top speed %.2f m/s\n", m_DWoundSpeed);
            Check(m_DWoundSpeed <= 0.75f, "a wounded soldier crawls (top speed " + std::to_string(m_DWoundSpeed) + " m/s)");
            Check(!b->Dead && b->Wounded, "an unhit wounded soldier is still alive after 4 s");
            shoot(*a, 8, 20.0f); // any hit
            Check(a->Dead, "a wounded soldier dies on the next hit");
        }
        if (t > 22.5f) {
            Check(b->Dead, "a wounded soldier bleeds out after 20 s");
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
