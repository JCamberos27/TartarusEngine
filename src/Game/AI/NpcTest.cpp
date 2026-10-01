#include "NpcTest.h"

#include "Combat/CombatFx.h"
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
} // namespace

static const char* EnvVar(const char* n) {
#pragma warning(suppress : 4996)
    return std::getenv(n);
}

NpcTest::NpcTest(const std::string& scenario) : m_Scenario(scenario.empty() ? "watch" : scenario) {
    if (m_Scenario == "fight") m_Duration = 75.0f;
    if (m_Scenario == "die") m_Duration = 60.0f;
    if (m_Scenario == "pose") m_Duration = 2.5f + (float)PosePhaseCount() * kPosePhaseTime + 1.0f;
    if (const char* t = EnvVar("NPC_TEST_SECONDS")) m_Duration = std::max(5.0f, (float)std::atof(t));
    if (const char* r = EnvVar("NPC_TEST_RECORD")) m_RecordDir = r;
    std::cout << "[NpcTest] scenario '" << m_Scenario << "', " << m_Duration << " s" << std::endl;
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
    float wantYaw = -90.0f, wantPitch = -3.0f;
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
        static int lastAmmo = -1;
        if (lastAmmo >= 0 && weapon.Ammo() < lastAmmo) m_PlayerShots += lastAmmo - weapon.Ammo();
        lastAmmo = weapon.Ammo();
        m_Reload = weapon.Ammo() == 0;
    }
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
        if (m_Scenario == "fight")
            std::cout << "[NpcTest]   player target='" << m_Target << "' err=" << m_AimErr << " shots=" << m_PlayerShots
                      << " yaw=" << player.Cam.Yaw << std::endl;
        for (const auto& up : npcs.Npcs()) {
            if (!up) continue;
            const Npc& n = *up;
            const int ammo = n.Weapon && n.Weapon->IsActive() ? n.Weapon->Ammo() : -1;
            std::snprintf(line, sizeof line,
                          "[NpcTest]   %-10s %-11s ph%-2d %-10s hp%3.0f %s ammo%2d %s%s%s pos(%5.1f,%4.1f,%5.1f) v%3.1f anim=%s gun=%s %s%s",
                          n.Name.c_str(), BehaviourName(n.Doing), n.Phase, RoleName(n.Role), n.Health,
                          n.Class == WeaponClass::Shotgun ? "870" : "AK ", ammo, n.Mem.Known ? "K" : "-", n.Mem.Visible ? "V" : "-",
                          n.HasAttackToken ? "A" : "-", n.Feet.x, n.Feet.y, n.Feet.z, glm::length(n.Velocity),
                          n.Body.StateName(world).c_str(), n.Weapon && n.Weapon->IsActive() ? n.Weapon->CurrentState().c_str() : "-",
                          n.Callout.empty() || now - n.CalloutAt > 2.0f ? "" : "\"", n.Callout.empty() || now - n.CalloutAt > 2.0f ? "" : n.Callout.c_str());
            std::cout << line << std::endl;
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

    if (now >= m_Duration && !m_Done) {
        m_Done = true;
        std::string seen;
        for (const auto& b : m_BehavioursSeen) seen += b + ", ";
        std::cout << "[NpcTest] behaviours seen: " << seen << std::endl;
        std::printf("[NpcTest] first known %.1f s, first shot %.1f s, first hit on player %.1f s, first in cover %.1f s, max shooters %d\n",
                    m_FirstKnown, m_FirstShot, m_FirstDamage, m_FirstCover, npcs.MaxShootersSeen());
        std::fflush(stdout);
        Check(m_MaxAlive >= 3, "at least three soldiers spawned (" + std::to_string(m_MaxAlive) + ")");
        Check(npcs.Nav().Valid() && npcs.Nav().PolyCount() > 0, "a navigation mesh was built");
        Check(!npcs.Cover().Points().empty(), "cover points were found (" + std::to_string(npcs.Cover().Points().size()) + ")");
        Check(m_FirstKnown >= 0.0f && m_FirstKnown < 15.0f, "the squad noticed the player within 15 s");
        Check(m_FirstShot >= 0.0f, "the squad fired");
        Check(m_FirstDamage >= 0.0f, "the squad's rounds reached the player");
        Check(m_FirstCover >= 0.0f, "someone fought from cover");
        Check(npcs.MaxShootersSeen() <= 4, "never more than the attack tokens shooting at once");
        if (npcs.Fx) {
            Check(npcs.Fx->ShotsHeard() > 0, "gunfire was heard (" + std::to_string(npcs.Fx->ShotsHeard()) + " reports)");
            Check(npcs.Fx->WhizzesHeard() > 0, "rounds cracked past the player (" + std::to_string(npcs.Fx->WhizzesHeard()) + ")");
        }
        if (m_Scenario == "fight") Check(m_Kills > 0, "the player killed " + std::to_string(m_Kills) + " soldier(s)");
        if (m_Scenario == "die") {
            Check(m_PlayerDied, "the player was killed");
            Check(m_PlayerRespawned, "the player respawned");
        }
    }
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
    {"low_ready", false, 15.0f, 0.0f, 0.0f},
    {"look_right", false, 4.0f, 8.0f, 0.5f},  // off the sights the head turns past the chest
    {"look_left", false, 4.0f, -8.0f, -1.0f},
    {"crouch_aim", true, 15.0f, 0.0f, 0.0f, true},
    {"crouch_low", false, 15.0f, 0.0f, 0.0f, true},
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
