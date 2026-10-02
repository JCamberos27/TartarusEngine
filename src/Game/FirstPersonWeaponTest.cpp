#include "FirstPersonWeaponTest.h"

#include "Camera.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "ShellCasings.h"
#include "World.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int kAkMagazine = 30;
constexpr int kShotgunShells = 6;
constexpr int kPellets = 8;
// The Remington's sight line in its Main bone's space (m): the ghost ring / front post ride 6.6 cm
// over the bore axis. The eye measured on the sights should sit on it.
constexpr float kSightHeight = 0.066f;
constexpr float kSightTolerance = 0.006f;
} // namespace

bool FirstPersonWeaponTest::Ctx::Saw(const std::string& state) const {
    for (const std::string& s : States)
        if (s == state) return true;
    return false;
}

const std::string& FirstPersonWeaponTest::Ctx::State() const { return P->CurrentState(); }

FirstPersonWeaponTest::FirstPersonWeaponTest(bool stockProbe, bool probeAk) : m_Probe(stockProbe), m_ProbeAk(probeAk) {
    // Shared between steps.
    struct Mem { int AkAmmo = 0, ShotgunAmmo = 0, Ammo = 0, Stage = 0; bool Held = false; float HipHand = 0.0f; std::vector<glm::vec4> AkHipHand; };
    auto mem = std::make_shared<Mem>();
    using C = Ctx;
    auto fire = [](C& c) { ++c.Pulls; };
    auto settled = [](C& c, const char* rest) { return c.State() == rest && c.P->Chambered(); };
    auto check = [](C& c, bool ok, const std::string& what) { c.Check(ok, what); };
    auto ammoIs = [check](C& c, int n) { check(c, c.P->Ammo() == n, "ammo " + std::to_string(c.P->Ammo()) + " (want " + std::to_string(n) + ")"); };
    // How much of a reload the left hand spends in the middle of the view, close up (where it
    // covers the sights): the share of frames it is within 22 deg of the view axis and 0.45 m.
    auto handInTheWay = [](C& c, const char* label) {
        int blocking = 0;
        float nearest = 1e9f, anchored = 0.0f;
        for (const glm::vec4& h : c.Hand) {
            const glm::vec3 v(h);
            const float d = glm::length(v);
            const float off = glm::degrees(std::atan2(glm::length(glm::vec2(v.x, v.y)), -v.z));
            if (v.z < 0.0f && d < 0.45f && off < 22.0f) ++blocking;
            nearest = std::min(nearest, d);
            anchored = std::max(anchored, h.w);
        }
        c.Anchored = anchored;
        for (size_t i = 0; i < c.Hand.size(); i += 12) {
            const glm::vec3 v(c.Hand[i]);
            std::printf("[WeaponTest]       %s f%3d view (%.2f %.2f %.2f) right %.0f up %.0f deg, anchor %.2f, shell %.2f m off the hand\n", label, (int)i, v.x, v.y, v.z,
                        glm::degrees(std::atan2(v.x, -v.z)), glm::degrees(std::atan2(v.y, -v.z)), c.Hand[i].w,
                        i < c.Shell.size() ? glm::length(c.Shell[i] - v) : -1.0f);
        }
        const float share = c.Hand.empty() ? 0.0f : (float)blocking / (float)c.Hand.size();
        std::printf("[WeaponTest]     %s: left hand in the way %.0f%% of %d reload frames, nearest %.2f m, anchor up to %.2f\n",
                    label, share * 100.0f, (int)c.Hand.size(), nearest, anchored);
        return share;
    };
    // `n` spent cases out of the port this step, thrown out to the right from beside the camera, and
    // (shell) each while the pump was being worked - none on the shot itself.
    auto ejected = [check](C& c, int n, const char* during = nullptr) {
        check(c, (int)c.Ejections.size() == n, std::to_string(c.Ejections.size()) + " case(s) ejected (want " + std::to_string(n) + ")");
        for (const C::Ejection& e : c.Ejections) {
            char msg[240];
            std::snprintf(msg, sizeof msg, "case thrown right (%.2f of the throw) from the port at (%.3f %.3f %.3f), muzzle (%.3f %.3f %.3f), in %s",
                          e.Right, e.Port.x, e.Port.y, e.Port.z, e.Muzzle.x, e.Muzzle.y, e.Muzzle.z, e.State.c_str());
            // Thrown out to the right, from a port near the eye and well behind the muzzle.
            check(c, e.Right > 0.3f && e.FromEye < 1.0f && e.Port.z < e.Muzzle.z - 0.15f && (!during || e.State == during), msg);
        }
    };
    auto pelletsInCone = [check](C& c, float cone) {
        // Every pellet struck something, each inside the spread cone about the bore, and they
        // spread (not all down one line).
        check(c, (int)c.Hits.size() == kPellets, std::to_string(c.Hits.size()) + " pellet hits (want " + std::to_string(kPellets) + ")");
        float widest = 0.0f;
        for (float a : c.HitAngles) widest = std::max(widest, a);
        char msg[120];
        std::snprintf(msg, sizeof msg, "pellets within the %.1f deg cone (widest %.2f deg)", cone, widest);
        check(c, !c.HitAngles.empty() && widest <= cone + 0.05f && widest > cone * 0.3f, msg);
    };

    m_Steps = {
        {"AK in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f,
         [=](C& c) { check(c, c.P->Slot() == 0, "slot 0"); ammoIs(c, kAkMagazine); }},
        {"AK hip round", fire, [](C& c) { return c.Time > 0.5f; }, 2.0f,
         [=](C& c) {
             ammoIs(c, kAkMagazine - 1);
             check(c, c.Hits.size() == 1, std::to_string(c.Hits.size()) + " hit(s) (want 1)");
             ejected(c, 1);
         }},
        {"AK reload", [](C& c) { c.P->Reload(); }, [](C& c) { return c.Saw("TacReload") && c.State() == "Idle"; }, 8.0f,
         [=](C& c) { ammoIs(c, kAkMagazine); handInTheWay(c, "AK hip"); mem->AkHipHand = c.Hand; }},
        {"AK full auto (0.5 s held)",
         [=](C& c) { c.P->ToggleFireMode(); mem->Held = true; },
         [=](C& c) { if (c.Time > 0.5f) mem->Held = false; return c.Time > 1.0f; }, 3.0f,
         [=](C& c) {
             check(c, c.P->IsFullAuto(), "full auto on");
             const int spent = kAkMagazine - c.P->Ammo();
             check(c, spent >= 4 && spent <= 8, std::to_string(spent) + " rounds in 0.5 s (700 rpm: ~6)");
             ejected(c, spent);
             c.P->ToggleFireMode();
         }},
        {"AK ADS round", [](C& c) { c.Aim = true; }, [](C& c) { return c.State() == "Aim" && c.Time > 0.6f; }, 3.0f,
         [=](C& c) { mem->Ammo = c.P->Ammo(); }},
        {"AK ADS round fires", fire, [](C& c) { return c.Time > 0.4f; }, 2.0f,
         [=](C& c) {
             ammoIs(c, mem->Ammo - 1);
             check(c, c.State() == "Aim", "stays on the sights (" + c.State() + ")");
             ejected(c, 1);
         }},
        // On the sights the free hand's trip to the pouch is anchored to the body (ads.handAnchor),
        // so it stays out of the view about as it does at the hip instead of swinging with the gun.
        {"AK ADS reload", [](C& c) { c.P->Reload(); }, [](C& c) { return c.Saw("TacReload") && c.State() == "Aim"; }, 8.0f,
         [=](C& c) {
             ammoIs(c, kAkMagazine);
             handInTheWay(c, "AK ADS");
             check(c, c.Anchored > 0.5f, "the hand anchor carried the pouch trip (" + std::to_string(c.Anchored) + ")");
             // Off the gun (anchored) the hand is where it was at the hip, frame for frame (the clip is the same).
             float worst = 0.0f;
             int frames = 0;
             for (size_t i = 0; i < c.Hand.size() && i < mem->AkHipHand.size(); ++i)
                 if (c.Hand[i].w > 0.9f) {
                     worst = std::max(worst, glm::length(glm::vec3(c.Hand[i]) - glm::vec3(mem->AkHipHand[i])));
                     ++frames;
                 }
             char msg[160];
             std::snprintf(msg, sizeof msg, "off the gun the hand follows its hip path (%d frames, worst %.1f cm)", frames, worst * 100.0f);
             check(c, frames > 10 && worst < 0.06f, msg);
             c.Aim = false;
             mem->AkAmmo = c.P->Ammo();
         }},
        {"AK cases come to rest", nullptr, [](C& c) { return c.Casings > 0 && c.CasingsAsleep == c.Casings; }, 4.0f,
         [=](C& c) {
             check(c, c.Casings >= 6, std::to_string(c.Casings) + " cases on the ground");
             check(c, c.CasingsAsleep == c.Casings, std::to_string(c.CasingsAsleep) + " of them at rest");
         }},

        {"2: switch to the Remington", [](C& c) { c.P->SelectSlot(1); },
         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f,
         [=](C& c) {
             check(c, c.Saw("Holster"), "AK holstered first");
             check(c, c.Saw("Draw"), "Remington drawn");
             ammoIs(c, kShotgunShells);
             check(c, c.P->Chambered(), "chambered");
         }},
        {"Remington hip round + pump", fire, [=](C& c) { return c.Saw("Pump") && settled(c, "Idle"); }, 5.0f,
         [=](C& c) {
             ammoIs(c, kShotgunShells - 1);
             pelletsInCone(c, c.P->Set().Gameplay.SpreadHip);
             ejected(c, 1, "Pump");
         }},
        {"no round mid-pump",
         [=](C& c) { mem->Stage = 0; mem->Ammo = c.P->Ammo(); fire(c); },
         [=](C& c) {
             if (mem->Stage == 0 && c.State() == "Pump") { fire(c); mem->Stage = 1; } // pulled during the pump
             return mem->Stage == 1 && settled(c, "Idle");
         }, 5.0f,
         [=](C& c) { ammoIs(c, mem->Ammo - 1); }},
        {"partial reload (4 -> 6)", [](C& c) { c.P->Reload(); }, [](C& c) { return c.Saw("ReloadStart") && c.State() == "Idle"; }, 12.0f,
         [=](C& c) {
             mem->HipHand = handInTheWay(c, "hip");
             ammoIs(c, kShotgunShells);
             check(c, c.Saw("ReloadLoop") && c.Saw("ReloadLoopEnd"), "start -> loop -> last shell");
         }},
        {"empty the tube", nullptr,
         [=](C& c) {
             if (c.P->Ammo() > 0 && settled(c, "Idle")) fire(c);
             return c.P->Ammo() == 0 && settled(c, "Idle");
         }, 25.0f,
         [=](C& c) { ammoIs(c, 0); }},
        {"dry trigger", fire, [](C& c) { return c.Time > 0.6f; }, 2.0f,
         [=](C& c) { ammoIs(c, 0); check(c, !c.Saw("Pump"), "no pump on a dry trigger"); }},
        {"empty reload, trigger mid-reload",
         [=](C& c) { mem->Stage = 0; c.P->Reload(); },
         [=](C& c) {
             if (mem->Stage == 0 && c.State() == "ReloadLoop" && c.P->Ammo() >= 2) { fire(c); mem->Stage = 1; }
             return mem->Stage == 1 && c.State() == "Idle";
         }, 20.0f,
         [=](C& c) {
             check(c, c.Saw("ReloadStartEmpty"), "empty start (pumps the first shell in)");
             check(c, c.Saw("ReloadEnd"), "stopped through ReloadEnd");
             check(c, c.P->Ammo() >= 2 && c.P->Ammo() <= 3, "stopped after the shell in hand (" + std::to_string(c.P->Ammo()) + ")");
             check(c, c.P->Chambered(), "chambered by the empty start");
             mem->Ammo = c.P->Ammo();
         }},
        {"fires after a stopped reload", fire, [=](C& c) { return c.Saw("Pump") && settled(c, "Idle"); }, 5.0f,
         [=](C& c) { ammoIs(c, mem->Ammo - 1); }},
        {"shell check (R held)", [](C& c) { c.P->TriggerAction("MagCheck"); }, [](C& c) { return c.Saw("MagCheck") && c.State() == "Idle"; }, 8.0f, nullptr},
        {"inspect, trigger refused",
         [=](C& c) { mem->Stage = 0; mem->Ammo = c.P->Ammo(); c.P->TriggerAction("Inspect"); },
         [=](C& c) {
             if (mem->Stage == 0 && c.State() == "Inspect") { fire(c); mem->Stage = 1; }
             return c.Saw("Inspect") && c.State() == "Idle";
         }, 10.0f,
         [=](C& c) { ammoIs(c, mem->Ammo); }},
        {"melee", [](C& c) { c.P->TriggerAction("Melee"); }, [](C& c) { return c.Saw("Melee") && c.State() == "Idle"; }, 6.0f, nullptr},
        {"ADS: on the sights", [](C& c) { c.Aim = true; }, [](C& c) { return c.State() == "Aim" && c.P->BarrelReport().SightMeasured; }, 6.0f,
         [=](C& c) {
             const FirstPersonBarrelReport& b = c.P->BarrelReport();
             char msg[200];
             std::snprintf(msg, sizeof msg, "eye in Main space (%.4f, %.4f, %.4f) m; sights at (0, %.3f)", b.SightOrigin.x,
                           b.SightOrigin.y, b.SightOrigin.z, kSightHeight);
             check(c, std::fabs(b.SightOrigin.x) < kSightTolerance && std::fabs(b.SightOrigin.y - kSightHeight) < kSightTolerance, msg);
             check(c, b.SightDirection.z < -0.999f, "looking down the bore");
             // How each action is carried onto the sights (the Weapon Inspector's ADS table).
             for (const AdsCarryReport::Entry& e : c.P->AdsReport().Entries)
                 std::printf("[WeaponTest]     carry %-17s gun %.1f cm / %.1f deg, elbow swivel R %.1f / L %.1f deg, %d twist bones%s%s\n",
                             e.State.c_str(), e.GunOffsetCm, e.GunTurnDeg, e.SwivelDeg[0], e.SwivelDeg[1], e.MatchedBones,
                             e.Problem.empty() ? "" : " - ", e.Problem.c_str());
             // A carried action's arm match stays near the aim pose's: a large swing is an elbow
             // bent the wrong way for the whole clip (a chained state measured off the gun).
             for (const AdsCarryReport::Entry& e : c.P->AdsReport().Entries)
                 if (e.Problem.empty())
                     check(c, std::fabs(e.SwivelDeg[0]) < 45.0f && std::fabs(e.SwivelDeg[1]) < 45.0f,
                           e.State + " elbow match " + std::to_string((int)e.SwivelDeg[1]) + " deg" +
                               (e.ContinuesFrom.empty() ? "" : " (continues " + e.ContinuesFrom + ")"));
         }},
        {"ADS round + pump on the sights",
         [=](C& c) { mem->Ammo = c.P->Ammo(); fire(c); }, [=](C& c) { return c.Saw("Pump") && settled(c, "Aim"); }, 5.0f,
         [=](C& c) {
             ammoIs(c, mem->Ammo - 1);
             check(c, !c.Hits.empty(), std::to_string(c.Hits.size()) + " pellet hits");
         }},
        {"ADS reload (from empty)", [](C& c) { c.P->Reload(); },
         [](C& c) { return (c.Saw("ReloadStart") || c.Saw("ReloadStartEmpty")) && c.State() == "Aim"; }, 20.0f,
         [=](C& c) {
             ammoIs(c, kShotgunShells);
             check(c, c.Saw("ReloadStartEmpty"), "empty start, on the sights");
             handInTheWay(c, "ADS");
         }},
        {"sights down", [](C& c) { c.Aim = false; }, [](C& c) { return c.State() == "Idle"; }, 3.0f, nullptr},
        {"two rounds before the swap (a loop needs 2+ to load)", nullptr,
         [=](C& c) {
             if (c.P->Ammo() > kShotgunShells - 2 && settled(c, "Idle")) fire(c);
             return c.P->Ammo() == kShotgunShells - 2 && settled(c, "Idle");
         }, 8.0f, nullptr},
        {"1 mid-reload: back to the AK",
         [=](C& c) { mem->Stage = 0; c.P->Reload(); },
         [=](C& c) {
             if (mem->Stage == 0 && c.State() == "ReloadLoop") { mem->ShotgunAmmo = c.P->Ammo(); c.P->SelectSlot(0); mem->Stage = 1; }
             return mem->Stage == 1 && c.P->Slot() == 0 && c.State() == "Idle";
         }, 30.0f,
         [=](C& c) { ammoIs(c, mem->AkAmmo); }},
        {"2: the Remington kept its shells", [](C& c) { c.P->SelectSlot(1); },
         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 30.0f,
         [=](C& c) { ammoIs(c, mem->ShotgunAmmo); }},
        {"3: unarmed", [](C& c) { c.P->SetEquipped(false); }, [](C& c) { return c.State() == "Holstered"; }, 5.0f,
         [=](C& c) { check(c, !c.P->IsEquipped(), "not equipped"); }},
        {"3 from unarmed", [](C& c) { c.P->SelectSlot(1); }, [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 5.0f,
         [=](C& c) { check(c, c.Saw("Draw"), "drawn"); }},
        {"idle fidget", nullptr, [](C& c) { return c.Saw("Regrip"); }, 25.0f, nullptr},
    };
    m_Ctx.Check = [this](bool ok, const std::string& what) {
        ++m_Checks;
        if (!ok) ++m_Failures;
        std::printf("[WeaponTest]     %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    };
    // Full-auto hold, read by Drive.
    m_Held = [mem] { return mem->Held; };
    if (m_Probe) {
#pragma warning(suppress : 4996)
        const char* pose = std::getenv("STOCK_PROBE_POSE");
        if (pose && *pose && *pose != '0') BuildPoseProbe();
        else BuildProbe();
    }
}

// --- Stock probe ------------------------------------------------------------------------------

namespace {
float SegmentDistance(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

std::string ShotStem(const std::string& s) {
    std::string n;
    for (char ch : s) n += (std::isalnum((unsigned char)ch) || ch == '-') ? ch : '_';
    return n;
}
} // namespace

void FirstPersonWeaponTest::BuildPoseProbe() {
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    const bool ak = m_ProbeAk;
    std::vector<Step> steps = {{"AK in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f, nullptr}};
    if (!ak)
        steps.push_back({"2: switch to the Remington", [](C& c) { c.P->SelectSlot(1); c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f, nullptr});
    steps.push_back({"settle", nullptr, hold(2.0f), 5.0f, nullptr});
    // Each pose held, sampled, then the whole body from three sides.
    auto views = [&](const std::string& stem) {
        static const char* kNames[4] = {"stock", "right", "front_r", "front_l"};
        for (int v = 1; v <= 3; ++v)
            steps.push_back({stem + " view " + kNames[v], [v](C& c) { c.View = v; }, hold(0.1f), 2.0f,
                             [stem, v](C& c) { c.Shot = ShotStem("pose_" + stem + "_" + kNames[v]); }});
    };
    struct Pose { const char* Name; bool Crouch, Aim; float Pitch; glm::vec2 Move; };
    for (const Pose& ps : {Pose{"stand_hip", false, false, 0.0f, {}}, Pose{"stand_aim", false, true, 0.0f, {}},
                           Pose{"stand_aim_up", false, true, 30.0f, {}}, Pose{"stand_aim_down", false, true, -30.0f, {}},
                           Pose{"crouch_hip", true, false, 0.0f, {}}, Pose{"crouch_aim", true, true, 0.0f, {}},
                           Pose{"crouch_aim_up", true, true, 25.0f, {}}, Pose{"crouch_aim_down", true, true, -30.0f, {}},
                           Pose{"crouch_aim_strafe", true, true, 0.0f, {1.0f, 0.0f}}, Pose{"stand_up", false, false, 0.0f, {}}}) {
        const std::string name = ps.Name;
        steps.push_back({name, [ps](C& c) { c.View = 0; c.Crouch = ps.Crouch; c.Aim = ps.Aim; c.Cam->Pitch = ps.Pitch; c.Move = ps.Move; },
                         [ps](C& c) { return c.Time >= (ps.Move.x != 0.0f ? 0.9f : 1.5f) && (!ps.Aim || c.State() == "Aim"); }, 6.0f,
                         [this, name](C&) { PrintSample(name); }});
        views(name);
        steps.push_back({name + " done", [](C& c) { c.View = 0; c.Move = glm::vec2(0.0f); }, hold(0.05f), 2.0f, nullptr});
    }
    steps.push_back({"sights down", [](C& c) { c.Aim = false; c.Crouch = false; c.Cam->Pitch = 0.0f; },
                     [](C& c) { return c.State() == "Idle" && c.Time > 0.5f; }, 3.0f, nullptr});
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::BuildProbe() {
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    const bool ak = m_ProbeAk;
    std::vector<Step> steps = {{"AK in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f, nullptr}};
    if (!ak)
        steps.push_back({"2: switch to the Remington", [](C& c) { c.P->SelectSlot(1); c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f, nullptr});
    steps.push_back({"settle", nullptr, hold(2.0f), 5.0f, nullptr});
    // Held still at each pitch: the settled pose, and a capture.
    for (float pitch : {-89.0f, -85.0f, -80.0f, -75.0f, -60.0f, -45.0f, -30.0f, -15.0f, 0.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
        const std::string name = "idle pitch " + std::to_string((int)pitch);
        steps.push_back({name, [pitch](C& c) { c.Cam->Pitch = pitch; }, hold(1.2f), 5.0f,
                         [this, name, pitch](C& c) { PrintSample(name); c.Shot = ShotStem("idle_pitch_" + std::to_string((int)pitch)); }});
    }
    steps.push_back({"level", [](C& c) { c.Cam->Pitch = 0.0f; }, hold(1.0f), 5.0f, nullptr});
    // Turning at a steady rate (the body lags the view), then stopping.
    for (float rate : {90.0f, 180.0f, 360.0f, -180.0f}) {
        const std::string name = "turn " + std::to_string((int)rate) + " deg/s";
        steps.push_back({name, [name](C& c) { c.LogEvery = 6; c.Label = name; },
                         [rate](C& c) {
                             c.Cam->Yaw += rate * c.Dt;
                             if (std::fabs(c.Time - 0.6f) < c.Dt * 0.5f) c.Shot = ShotStem("turn_" + std::to_string((int)rate));
                             return c.Time >= 1.2f;
                         }, 5.0f, [](C& c) { c.LogEvery = 0; }});
        steps.push_back({"stopped", [](C& c) { c.LogEvery = 12; c.Label = "stopped"; }, hold(1.0f), 5.0f,
                         [this, name](C& c) { c.LogEvery = 0; PrintSample("stopped after " + name); }});
    }
    // Sweeping the pitch.
    struct Sweep { float From, To, Rate; };
    for (const Sweep& s : {Sweep{0.0f, -75.0f, 120.0f}, Sweep{-75.0f, 60.0f, 120.0f}, Sweep{60.0f, 0.0f, 120.0f}}) {
        const std::string name = "sweep " + std::to_string((int)s.From) + " -> " + std::to_string((int)s.To);
        steps.push_back({name, [s, name](C& c) { c.Cam->Pitch = s.From; c.LogEvery = 6; c.Label = name; },
                         [s](C& c) {
                             const float dir = s.To > s.From ? 1.0f : -1.0f;
                             c.Cam->Pitch = std::clamp(c.Cam->Pitch + dir * s.Rate * c.Dt, std::min(s.From, s.To), std::max(s.From, s.To));
                             return c.Cam->Pitch == s.To;
                         }, 5.0f, [](C& c) { c.LogEvery = 0; }});
    }
    // A round (and the Remington's pump) at three pitches, sampled through, captured along the way.
    for (float pitch : {0.0f, -45.0f, 30.0f}) {
        const std::string name = (ak ? "fire pitch " : "fire+pump pitch ") + std::to_string((int)pitch);
        steps.push_back({"to pitch " + std::to_string((int)pitch), [pitch](C& c) { c.Cam->Pitch = pitch; }, hold(1.0f), 5.0f, nullptr});
        steps.push_back({name, [name](C& c) { c.LogEvery = 6; c.Label = name; ++c.Pulls; },
                         [pitch, ak](C& c) {
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (f % 12 == 0) c.Shot = ShotStem("fire_p" + std::to_string((int)pitch) + "_" + std::to_string(1000 + f).substr(1));
                             if (ak) return c.Time >= 0.8f && c.State() == "Idle";
                             return c.Saw("Pump") && c.State() == "Idle" && c.P->Chambered();
                         }, 6.0f, [](C& c) { c.LogEvery = 0; }});
    }
    // The AK's full auto: 0.6 s bursts (~7 rounds) at the hip and on the sights, level and pitched down - the
    // recoil drives the gun back and the muzzle up, into the pocket and past the elbows. From a full magazine.
    if (ak) {
        steps.push_back({"full auto on, full mag", [](C& c) { c.Cam->Pitch = 0.0f; if (!c.P->IsFullAuto()) c.P->ToggleFireMode(); c.P->Reload(); },
                         [](C& c) { return c.Time > 0.5f && c.State() == "Idle"; }, 8.0f, nullptr});
        struct Burst { const char* Name; bool Aim; float Pitch; };
        for (const Burst& b : {Burst{"burst hip pitch 0", false, 0.0f}, Burst{"burst hip pitch -30", false, -30.0f},
                               Burst{"burst aim pitch 0", true, 0.0f}, Burst{"burst aim pitch -30", true, -30.0f}}) {
            const std::string name = b.Name;
            const std::string stem = ShotStem(name);
            steps.push_back({"ready for " + name, [b](C& c) { c.Aim = b.Aim; c.Cam->Pitch = b.Pitch; },
                             [b](C& c) { return c.State() == (b.Aim ? "Aim" : "Idle") && c.Time >= 0.8f; }, 5.0f, nullptr});
            steps.push_back({name, [name](C& c) { c.Trigger = true; c.LogEvery = 3; c.Label = name; },
                             [stem](C& c) {
                                 if (c.Time >= 0.6f) c.Trigger = false;
                                 const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                                 if (f % 12 == 6) c.Shot = stem + "_" + std::to_string(1000 + f).substr(1);
                                 return c.Time >= 1.2f;
                             }, 5.0f, [](C& c) { c.Trigger = false; c.LogEvery = 0; }});
        }
        steps.push_back({"bursts done", [](C& c) { c.Aim = false; c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.State() == "Idle" && c.Time > 0.5f; }, 3.0f, nullptr});
    }
    // On the sights at three pitches.
    for (float pitch : {0.0f, -45.0f, 30.0f}) {
        const std::string name = "aim pitch " + std::to_string((int)pitch);
        steps.push_back({name, [pitch](C& c) { c.Aim = true; c.Cam->Pitch = pitch; },
                         [](C& c) { return c.State() == "Aim" && c.Time >= 1.2f; }, 5.0f,
                         [this, name, pitch](C& c) { PrintSample(name); c.Shot = ShotStem("aim_p" + std::to_string((int)pitch)); }});
    }
    steps.push_back({"sights down", [](C& c) { c.Aim = false; c.Cam->Pitch = 0.0f; },
                     [](C& c) { return c.State() == "Idle" && c.Time > 0.5f; }, 3.0f, nullptr});
    // Moving: a walk, then sprints - straight at three pitches, and turning - each followed by a stop and
    // a turn back round so the body stays near where it started. Sampled through, captured along the way.
    struct Run { const char* Name; bool Sprint; float Pitch, YawRate; };
    for (const Run& r : {Run{"walk", false, 0.0f, 0.0f}, Run{"sprint pitch 0", true, 0.0f, 0.0f}, Run{"sprint pitch -30", true, -30.0f, 0.0f},
                         Run{"sprint pitch 20", true, 20.0f, 0.0f}, Run{"sprint turning 90", true, 0.0f, 90.0f}}) {
        const std::string name = r.Name;
        const std::string stem = ShotStem(name);
        steps.push_back({name, [r, name](C& c) { c.Cam->Pitch = r.Pitch; c.Move = glm::vec2(0.0f, 1.0f); c.Sprint = r.Sprint; c.LogEvery = 6; c.Label = name; },
                         [r, stem](C& c) {
                             c.Cam->Yaw += r.YawRate * c.Dt;
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (f % 20 == 10) c.Shot = stem + "_" + std::to_string(1000 + f).substr(1);
                             return c.Time >= 2.5f;
                         }, 5.0f, [](C& c) { c.Move = glm::vec2(0.0f); c.Sprint = false; }});
        steps.push_back({"stop after " + name, [name](C& c) { c.Label = "stop after " + name; }, [](C& c) { return c.Time >= 1.5f && c.State() == "Idle"; }, 6.0f,
                         [](C& c) { c.LogEvery = 0; }});
        // Back round, so the next run heads back over the same ground.
        steps.push_back({"about turn", [](C& c) { c.Cam->Pitch = 0.0f; }, [](C& c) { c.Cam->Yaw += 180.0f * c.Dt; return c.Time >= 1.0f; }, 5.0f, nullptr});
        steps.push_back({"settle", nullptr, hold(1.0f), 5.0f, nullptr});
    }
    // On the sights while moving and turning (where the elbows come nearest the body): forward, sideways, back,
    // turning on the spot, and forward while turning - each followed by a stop.
    struct AimMove { const char* Name; glm::vec2 Move; float YawRate; };
    for (const AimMove& r : {AimMove{"aim walk forward", {0.0f, 1.0f}, 0.0f}, AimMove{"aim strafe right", {1.0f, 0.0f}, 0.0f},
                             AimMove{"aim strafe left", {-1.0f, 0.0f}, 0.0f}, AimMove{"aim walk back", {0.0f, -1.0f}, 0.0f},
                             AimMove{"aim turning 90", {0.0f, 0.0f}, 90.0f}, AimMove{"aim walk turning -90", {0.0f, 1.0f}, -90.0f}}) {
        const std::string name = r.Name;
        const std::string stem = ShotStem(name);
        steps.push_back({"aim for " + name, [](C& c) { c.Aim = true; c.Cam->Pitch = 0.0f; }, [](C& c) { return c.State() == "Aim" && c.Time >= 0.6f; }, 5.0f, nullptr});
        steps.push_back({name, [r, name](C& c) { c.Move = r.Move; c.LogEvery = 6; c.Label = name; },
                         [r, stem](C& c) {
                             c.Cam->Yaw += r.YawRate * c.Dt;
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (f % 20 == 10) c.Shot = stem + "_" + std::to_string(1000 + f).substr(1);
                             return c.Time >= 2.0f;
                         }, 5.0f, [](C& c) { c.Move = glm::vec2(0.0f); }});
        steps.push_back({"stop after " + name, [name](C& c) { c.Label = "stop after " + name; }, hold(1.0f), 5.0f,
                         [](C& c) { c.LogEvery = 0; c.Aim = false; }});
        steps.push_back({"sights down", nullptr, [](C& c) { return c.State() == "Idle" && c.Time > 0.4f; }, 3.0f, nullptr});
    }
    // The weapon's own animations (where the head moved oddly): a reload at the hip and on the sights, the
    // inspect, the shell check and the melee - sampled every third frame through each.
    auto settledIdle = [](C& c) { return c.State() == "Idle" && c.P->Chambered(); };
    // ... seen from the front right (the body whole, as --npc-test reload shows a soldier's), every 0.15 s.
    auto sampled = [](const char* name, std::function<void(C&)> begin) {
        return [name, begin](C& c) { c.LogEvery = 3; c.Label = name; c.View = 2; begin(c); };
    };
    auto filmed = [](const char* stem, std::function<bool(C&)> done) {
        return [stem, done](C& c) {
            const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
            if (f % 9 == 0) c.Shot = std::string(stem) + "_" + std::to_string(1000 + f).substr(1);
            return done(c);
        };
    };
    auto stopLog = [](C& c) { c.LogEvery = 0; c.View = 0; };
    if (ak) {
        // The AK: a tactical reload (rounds left), the magazine emptied and an empty reload, the inspect, the mag
        // check and the melee at the hip, and a tactical reload on the sights.
        steps.push_back({"a round, to reload", [](C& c) { c.Cam->Pitch = 0.0f; ++c.Pulls; }, [](C& c) { return c.Time >= 0.5f && c.State() == "Idle"; }, 3.0f, nullptr});
        steps.push_back({"anim hip tac reload", sampled("anim hip tac reload", [](C& c) { c.P->Reload(); }),
                         filmed("anim_hip_tac_reload", [](C& c) { return c.Saw("TacReload") && c.State() == "Idle"; }), 10.0f, stopLog});
        steps.push_back({"empty the magazine", [](C& c) { c.Trigger = true; },
                         [](C& c) { if (c.P->Ammo() == 0) c.Trigger = false; return c.P->Ammo() == 0 && c.Time > 0.3f && c.State() == "Idle"; }, 8.0f,
                         [](C& c) { c.Trigger = false; }});
        steps.push_back({"anim hip empty reload", sampled("anim hip empty reload", [](C& c) { c.P->Reload(); }),
                         filmed("anim_hip_empty_reload", [](C& c) { return c.Saw("EmptyReload") && c.State() == "Idle"; }), 10.0f, stopLog});
        steps.push_back({"anim regrip", sampled("anim regrip", [](C& c) { c.P->TriggerAction("Fidget"); }),
                         filmed("anim_regrip", [](C& c) { return c.Saw("Regrip") && c.State() == "Idle"; }), 10.0f, stopLog});
        steps.push_back({"anim inspect", sampled("anim inspect", [](C& c) { c.P->TriggerAction("Inspect"); }),
                         [](C& c) { return c.Saw("Inspect") && c.State() == "Idle"; }, 12.0f, stopLog});
        steps.push_back({"anim mag check", sampled("anim mag check", [](C& c) { c.P->TriggerAction("MagCheck"); }),
                         [](C& c) { return c.Saw("MagCheck") && c.State() == "Idle"; }, 10.0f, stopLog});
        steps.push_back({"anim melee", sampled("anim melee", [](C& c) { c.P->TriggerAction("Melee"); }),
                         [](C& c) { return c.Saw("Melee") && c.State() == "Idle"; }, 8.0f, stopLog});
        steps.push_back({"on the sights", [](C& c) { c.Aim = true; }, [](C& c) { return c.State() == "Aim" && c.Time > 0.6f; }, 5.0f, nullptr});
        steps.push_back({"a round on the sights", [](C& c) { ++c.Pulls; }, [](C& c) { return c.Time >= 0.5f && c.State() == "Aim"; }, 3.0f, nullptr});
        steps.push_back({"anim ADS tac reload", sampled("anim ADS tac reload", [](C& c) { c.P->Reload(); }),
                         filmed("anim_ads_tac_reload", [](C& c) { return c.Saw("TacReload") && c.State() == "Aim"; }), 10.0f, stopLog});
        steps.push_back({"sights down", [](C& c) { c.Aim = false; }, [](C& c) { return c.State() == "Idle" && c.Time > 0.4f; }, 3.0f, nullptr});
        m_Steps = std::move(steps);
        return;
    }
    steps.push_back({"a round, to reload", [](C& c) { c.Cam->Pitch = 0.0f; ++c.Pulls; }, [=](C& c) { return c.Saw("Pump") && settledIdle(c); }, 6.0f, nullptr});
    steps.push_back({"anim hip reload", sampled("anim hip reload", [](C& c) { c.P->Reload(); }),
                     filmed("anim_hip_reload", [](C& c) { return c.Saw("ReloadStart") && c.State() == "Idle"; }), 15.0f, stopLog});
    steps.push_back({"anim inspect", sampled("anim inspect", [](C& c) { c.P->TriggerAction("Inspect"); }),
                     [](C& c) { return c.Saw("Inspect") && c.State() == "Idle"; }, 12.0f, stopLog});
    steps.push_back({"anim shell check", sampled("anim shell check", [](C& c) { c.P->TriggerAction("MagCheck"); }),
                     [](C& c) { return c.Saw("MagCheck") && c.State() == "Idle"; }, 10.0f, stopLog});
    steps.push_back({"anim melee", sampled("anim melee", [](C& c) { c.P->TriggerAction("Melee"); }),
                     [](C& c) { return c.Saw("Melee") && c.State() == "Idle"; }, 8.0f, stopLog});
    steps.push_back({"on the sights", [](C& c) { c.Aim = true; }, [](C& c) { return c.State() == "Aim" && c.Time > 0.6f; }, 5.0f, nullptr});
    steps.push_back({"a round on the sights", [](C& c) { ++c.Pulls; }, [](C& c) { return c.Saw("Pump") && c.State() == "Aim" && c.P->Chambered(); }, 6.0f, nullptr});
    steps.push_back({"anim ADS reload", sampled("anim ADS reload", [](C& c) { c.P->Reload(); }),
                     [](C& c) { return c.Saw("ReloadStart") && c.State() == "Aim"; }, 15.0f, stopLog});
    steps.push_back({"sights down", [](C& c) { c.Aim = false; }, [](C& c) { return c.State() == "Idle" && c.Time > 0.4f; }, 3.0f, nullptr});
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::AfterPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p) {
    if (!m_Probe || !m_Ctx.Cam) return;
    ++m_Frame;
    const Camera& cam = *m_Ctx.Cam;
    Sample s;
    s.Pitch = cam.Pitch;
    s.Roll = cam.Roll;
    s.Yaw = cam.Yaw;
    if (m_HaveYaw && m_Ctx.Dt > 0.0f) s.YawRate = std::remainder(cam.Yaw - m_LastYaw, 360.0f) / m_Ctx.Dt;
    m_LastYaw = cam.Yaw;
    m_HaveYaw = true;
    s.TwistDeg = glm::degrees(body.Twist());
    // The camera's acceleration frame to frame (a hitch in the eye shows as a spike), the largest since the last print.
    if (m_EyeFrames >= 2 && m_Ctx.Dt > 0.0f)
        m_EyeAccelMax = std::max(m_EyeAccelMax, glm::length(cam.Position - 2.0f * m_EyePrev[0] + m_EyePrev[1]) / (m_Ctx.Dt * m_Ctx.Dt));
    m_EyePrev[1] = m_EyePrev[0];
    m_EyePrev[0] = cam.Position;
    m_EyeFrames = std::min(m_EyeFrames + 1, 2);
    s.EyeAccel = m_EyeAccelMax;
    s.Eye = cam.Position;
    s.State = p.CurrentState();
    s.Speed = p.PlanarSpeed();
    glm::vec3 butt, fwd, upper, clav, neck, head, muzzle, bore;
    // The gun's length: butt to muzzle along the bore (the first-person gun; the world one is the same).
    s.GunLength = 0.45f;
    if (p.StockWorld(butt, fwd) && p.MuzzleRay(muzzle, bore)) {
        s.GunLength = std::clamp(glm::dot(muzzle - butt, fwd), 0.3f, 1.5f);
        // Once per weapon, a check that StockWorld found its butt: the muzzle should be the gun's length
        // ahead of it along the bore, with the stock's forward and the muzzle's bore the same way.
        if (m_PrintedGunSlot != p.Slot() && s.State == "Idle") {
            m_PrintedGunSlot = p.Slot();
            std::printf("[StockProbe] gun: muzzle %.1f cm ahead of the butt along the bore, %.1f cm off it; stock forward . bore %.3f\n",
                        glm::dot(muzzle - butt, fwd) * 100.0f, glm::length((muzzle - butt) - fwd * glm::dot(muzzle - butt, fwd)) * 100.0f,
                        glm::dot(fwd, bore));
        }
    }
    // The world gun (split poses): the first-person one moved by the body's world gun shift.
    if (p.StockWorld(butt, fwd) && ((butt += body.WorldGunShift()), true) && body.BoneWorld(world, "upperarm_r", upper) && body.BoneWorld(world, "clavicle_r", clav) &&
        body.BoneWorld(world, "neck_01", neck) && body.BoneWorld(world, "head", head)) {
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        glm::vec3 front = cam.Front();
        front.y = 0.0f;
        front = glm::length(front) > 1e-4f ? glm::normalize(front) : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = glm::normalize(glm::cross(front, up));
        const glm::vec3 d = butt - upper;
        s.StockFromShoulder = glm::vec3(glm::dot(d, right), d.y, glm::dot(d, front));
        const glm::vec3 e = cam.Position - upper;
        s.EyeFromShoulder = glm::vec3(glm::dot(e, right), e.y, glm::dot(e, front));
        s.Shoulder = glm::length(d);
        s.Clavicle = glm::length(butt - clav);
        s.Neck = glm::length(butt - neck);
        s.Head = glm::length(butt - head);
        const glm::vec3 rear = butt + fwd * 0.30f;
        s.NeckGap = SegmentDistance(neck, butt, rear);
        s.HeadGap = SegmentDistance(head, butt, rear);
        // The whole gun: against the neck, the hood keep-out's centre, and the drawn head / neck / hood.
        const glm::vec3 gunFront = butt + fwd * s.GunLength;
        s.WholeNeckGap = SegmentDistance(neck, butt, gunFront);
        s.WholeHoodGap = SegmentDistance(head + up * 0.07f, butt, gunFront);
        float along = 0.0f;
        s.MeshGap = body.HeadMeshGap(world, butt, gunFront, &s.MeshPiece, &along);
        s.MeshAlong = along * s.GunLength;
        body.ElbowTorsoGaps(world, s.ElbowGap);
        for (int h = 0; h < 2; ++h) s.ElbowSwing[h] = glm::degrees(body.WorldElbowSwing(h));
        float torsoAlong = 0.0f;
        s.TorsoGap = body.TorsoMeshGap(world, butt, butt + fwd * 0.45f, nullptr, &torsoAlong);
        s.TorsoAlong = torsoAlong * 0.45f;
        s.HeadTilt = body.WorldHeadTilt();
        s.HeadTiltWeight = body.WorldHeadTiltWeight();
        if (glm::vec3 chest; body.BoneWorld(world, "spine_05", chest) && glm::length(neck - chest) > 1e-4f && glm::length(head - neck) > 1e-4f)
            s.HeadBend = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(neck - chest), glm::normalize(head - neck)), -1.0f, 1.0f)));
        // The world hands against the world gun's grips (the rig's hands, moved with it).
        for (int h = 0; h < 2; ++h) {
            glm::vec3 v, bodyHand;
            if (p.ArmsNodeInView(h == 0 ? "hand_l" : "hand_r", v) && body.BoneWorld(world, h == 0 ? "hand_l" : "hand_r", bodyHand)) {
                const glm::vec3 rigHand = glm::vec3(glm::inverse(cam.ViewMatrix()) * glm::vec4(v, 1.0f)) + body.WorldGunShift();
                s.HandGap[h] = glm::length(bodyHand - rigHand);
            }
        }
        s.GunShift = glm::length(body.WorldGunShift());
        s.Valid = true;
        // The Scene camera: in front of the body and to its right, on the gun's rear and the shoulder - or (pose probe)
        // the whole body from its right, its front right or its front left.
        glm::vec3 target = 0.5f * (butt + 0.5f * (neck + upper)) + fwd * 0.08f;
        m_SceneCamPos = target + front * 1.05f + right * 0.75f + up * 0.12f;
        if (m_Ctx.View > 0) {
            glm::vec3 feet = cam.Position;
            feet.y = neck.y - 1.45f;
            target = glm::vec3(neck.x, feet.y + 0.85f * (neck.y - feet.y) * 0.85f, neck.z);
            const glm::vec3 dir = m_Ctx.View == 1 ? right : m_Ctx.View == 2 ? glm::normalize(front + right * 0.75f) : glm::normalize(front - right * 0.85f);
            m_SceneCamPos = target + dir * 2.4f + up * 0.15f;
        }
        const glm::vec3 look = glm::normalize(target - m_SceneCamPos);
        m_SceneCamYaw = glm::degrees(std::atan2(look.z, look.x));
        m_SceneCamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
        m_HaveSceneCam = true;
    }
    m_Sample = s;
    if (m_Ctx.LogEvery > 0 && m_Frame % m_Ctx.LogEvery == 0) PrintSample(m_Ctx.Label);
}

bool FirstPersonWeaponTest::SceneCamera(glm::vec3& position, float& yaw, float& pitch) const {
    if (!m_Probe || !m_HaveSceneCam) return false;
    position = m_SceneCamPos;
    yaw = m_SceneCamYaw;
    pitch = m_SceneCamPitch;
    return true;
}

void FirstPersonWeaponTest::PrintSample(const std::string& label) const {
    const Sample& s = m_Sample;
    if (!s.Valid) {
        std::printf("[StockProbe] %-24s (no sample: body or stock not found)\n", label.c_str());
        return;
    }
    std::printf("[StockProbe] %-24s pitch %6.1f yawRate %5.0f twist %6.1f %-9s | butt-shoulder R %+5.1f U %+5.1f F %+5.1f (%4.1f) | "
                "clav %4.1f neck %4.1f head %4.1f | rear 30cm to neck %4.1f head %4.1f | shift %4.1f hands off L %4.1f R %4.1f cm | "
                "speed %4.1f eyeAcc %5.1f at (%.1f, %.1f) | gun %3.0fcm to neck %4.1f hood %4.1f mesh %5.1f at %3.0fcm (%s) | elbow to torso L %5.1f R %5.1f cm, swung L %+4.0f R %+4.0f deg | rear 45cm to torso %5.1f at %3.0fcm | head tilt %4.1f (w %.2f) bend %4.1f deg | eye-shoulder R %+5.1f U %+5.1f F %+5.1f roll %5.1f\n",
                label.c_str(), s.Pitch, s.YawRate, s.TwistDeg, s.State.c_str(), s.StockFromShoulder.x * 100.0f,
                s.StockFromShoulder.y * 100.0f, s.StockFromShoulder.z * 100.0f, s.Shoulder * 100.0f, s.Clavicle * 100.0f,
                s.Neck * 100.0f, s.Head * 100.0f, s.NeckGap * 100.0f, s.HeadGap * 100.0f, s.GunShift * 100.0f,
                s.HandGap[0] * 100.0f, s.HandGap[1] * 100.0f, s.Speed, s.EyeAccel, s.Eye.x, s.Eye.z, s.GunLength * 100.0f, s.WholeNeckGap * 100.0f,
                s.WholeHoodGap * 100.0f, s.MeshGap * 100.0f, s.MeshAlong * 100.0f, s.MeshPiece.c_str(), s.ElbowGap[0] * 100.0f,
                s.ElbowGap[1] * 100.0f, s.ElbowSwing[0], s.ElbowSwing[1], s.TorsoGap * 100.0f, s.TorsoAlong * 100.0f, s.HeadTilt, s.HeadTiltWeight,
                s.HeadBend, s.EyeFromShoulder.x * 100.0f, s.EyeFromShoulder.y * 100.0f, s.EyeFromShoulder.z * 100.0f, s.Roll);
    m_EyeAccelMax = 0.0f;
    std::fflush(stdout);
}


void FirstPersonWeaponTest::OnHit(const glm::vec3& point) {
    m_Ctx.Hits.push_back(point);
    glm::vec3 o, d;
    if (m_Ctx.P && m_Ctx.P->MuzzleRay(o, d) && glm::length(point - o) > 1e-4f)
        m_Ctx.HitAngles.push_back(glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(point - o), d), -1.0f, 1.0f))));
}

void FirstPersonWeaponTest::OnCasings(const ShellCasings& casings) {
    Ctx& c = m_Ctx;
    c.Casings = casings.Count();
    c.CasingsAsleep = casings.SleepingCount();
    if (!c.P || !c.Cam) return;
    const int total = c.P->EjectedTotal();
    for (; m_Ejected < total; ++m_Ejected) {
        Ctx::Ejection e;
        e.State = c.P->CurrentState();
        const glm::vec3 t = c.P->LastEjectThrow();
        e.Right = glm::length(t) > 1e-6f ? glm::dot(glm::normalize(t), c.Cam->Right()) : 0.0f;
        e.FromEye = glm::length(c.P->LastEjectPoint() - c.Cam->Position);
        const glm::vec3 r = c.Cam->Right(), u = c.Cam->Up(), f = c.Cam->Front();
        auto view = [&](const glm::vec3& w) { const glm::vec3 d = w - c.Cam->Position; return glm::vec3(glm::dot(d, r), glm::dot(d, u), glm::dot(d, f)); };
        e.Port = view(c.P->LastEjectPoint());
        glm::vec3 o, dir;
        if (c.P->MuzzleRay(o, dir)) e.Muzzle = view(o);
        c.Ejections.push_back(e);
    }
}

void FirstPersonWeaponTest::Drive(FirstPersonPresentation& p, float dt) {
    Ctx& c = m_Ctx;
    c.P = &p;
    c.Dt = dt;
    if (p.CurrentState() != m_LastState) {
        m_LastState = p.CurrentState();
        c.States.push_back(m_LastState);
    }
    if (!Done()) {
        Step& s = m_Steps[m_Step];
        if (!m_Began) {
            m_Began = true;
            c.Time = 0.0f;
            c.States.assign(1, p.CurrentState());
            c.Hits.clear();
            c.HitAngles.clear();
            c.Hand.clear();
            c.Shell.clear();
            c.Ejections.clear();
            std::printf("[WeaponTest] %s\n", s.Name.c_str());
            if (s.Begin) s.Begin(c);
        } else {
            c.Time += dt;
        }
        if (!s.Until || s.Until(c)) {
            if (s.End) s.End(c);
            std::printf("[WeaponTest]     (%.2f s, states:", c.Time);
            for (const std::string& st : c.States) std::printf(" %s", st.c_str());
            std::printf(")\n");
            ++m_Step;
            m_Began = false;
        } else if (c.Time > s.Timeout) {
            c.Check(false, "timed out after " + std::to_string((int)s.Timeout) + " s in state '" + p.CurrentState() + "'");
            std::printf("[WeaponTest]     (states:");
            for (const std::string& st : c.States) std::printf(" %s", st.c_str());
            std::printf(")\n");
            m_Step = m_Steps.size(); // later steps assume this one happened
        }
    }
    m_Shot.clear();
    if (!c.Shot.empty()) {
        m_Shot = c.Shot;
        c.Shot.clear();
    }
    if (glm::vec3 v; !m_Probe && p.CurrentState().find("Reload") != std::string::npos && p.ArmsNodeInView("hand_l", v)) {
        if (c.Hand.size() % 10 == 0 && !Done()) {
            char name[64];
            std::snprintf(name, sizeof name, "%s%s_%03d", p.CurrentState().rfind("Reload", 0) == 0 ? "" : "ak_", c.Aim ? "ads" : "hip", (int)c.Hand.size());
            m_Shot = name;
        }
        c.Hand.push_back(glm::vec4(v, p.HandAnchorWeight()));
    }
    if (glm::vec3 v; c.Shell.size() < c.Hand.size() && p.WeaponNodeInView("Shell", v)) c.Shell.push_back(v);
    const bool held = (m_Held && m_Held()) || c.Trigger;
    p.UpdateTrigger(c.Pulls > 0 || held, c.Pulls > 0 || held);
    c.Pulls = 0;
    std::fflush(stdout);
}
