#include "FirstPersonWeaponTest.h"

#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "BodyDebugDraw.h"
#include "Model.h"
#include "Audio/WeaponAudio.h"
#include "Camera.h"
#include "Components.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "ProjectPaths.h"
#include "ShellCasings.h"
#include "World.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

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
    // The muzzle flash on its first frames (PRO Effects' layers over the flame), captured as `stem`_<frame>.
    auto flashShots = [](C& c, const char* stem) {
        const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
        if (f == 1 || f == 2 || f == 4 || f == 30) c.Shot = std::string(stem) + "_" + std::to_string(f);
    };
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
        {"AK hip round", fire, [=](C& c) { flashShots(c, "muzzle_ak_hip"); return c.Time > 0.5f; }, 2.0f,
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
        {"AK ADS optic to iron sights", [](C& c) { c.P->CycleAttachment(Scripting::AttachmentKind::Optic); },
         [](C& c) { if(c.Time > .3f) c.Shot="optic_iron_ads"; return c.Time > .35f; }, 2.0f,
         [=](C& c) { check(c,c.P->AttachmentCount(Scripting::AttachmentKind::Optic)==2,"both authored optic choices loaded");
                      check(c,c.State()=="Aim","optic change keeps ADS active"); }},
        {"AK ADS optic to red dot", [](C& c) { c.P->CycleAttachment(Scripting::AttachmentKind::Optic); },
         [](C& c) { if(c.Time > .3f) c.Shot="optic_red_dot_ads"; return c.Time > .35f; }, 2.0f,
         [=](C& c) { check(c,c.State()=="Aim","red dot change keeps ADS active"); }},
        {"AK ADS round fires", fire, [=](C& c) { flashShots(c, "muzzle_ak_ads"); return c.Time > 0.4f; }, 2.0f,
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

        {"Scroll down: switch to the Remington", [](C& c) { c.P->CycleSlot(1); },
         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f,
         [=](C& c) {
             check(c, c.Saw("Holster"), "AK holstered first");
             check(c, c.Saw("Draw"), "Remington drawn");
             ammoIs(c, kShotgunShells);
             check(c, c.P->Chambered(), "chambered");
         }},
        {"Remington hip round + pump", fire, [=](C& c) { flashShots(c, "muzzle_870_hip"); return c.Saw("Pump") && settled(c, "Idle"); }, 5.0f,
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
    if (!m_Probe) WeaponAudio::Get().SetRecording(true); // the audio checks at the end read every emission
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
#pragma warning(suppress : 4996)
        const char* sprint = std::getenv("STOCK_PROBE_SPRINT");
        m_SprintProbe = sprint && *sprint && *sprint != '0';
#pragma warning(suppress : 4996)
        const char* gait = std::getenv("STOCK_PROBE_GAIT");
        m_GaitProbe = gait && *gait && *gait != '0';
#pragma warning(suppress : 4996)
        const char* bodyProbe = std::getenv("STOCK_PROBE_BODY");
        m_BodyProbe = bodyProbe && *bodyProbe && *bodyProbe != '0';
        // STOCK_PROBE_BODY=<text> (not 1): only the locomotion segments whose names contain it, no gun section.
        if (m_BodyProbe && std::string(bodyProbe) != "1") m_BodyOnly = bodyProbe;
        if (m_BodyProbe) BuildBodyProbe();
        else if (m_GaitProbe) BuildGaitProbe();
        else if (m_SprintProbe) BuildSprintProbe();
        else if (pose && *pose && *pose != '0') BuildPoseProbe();
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

void FirstPersonWeaponTest::BuildSprintProbe() {
    using C = Ctx;
    std::vector<Step> steps = {{"AK in hand", nullptr, [](C& c) { return c.State() == "Idle"; }, 30, nullptr}};
    if (!m_ProbeAk)
        steps.push_back({"switch to shotgun", [](C& c) { c.P->SelectSlot(1); }, [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 30, nullptr});
    steps.push_back({"walk", [](C& c) { c.View = 2; c.Move = {0,1}; }, [](C& c) { return c.Time > 1; }, 4, nullptr});
    steps.push_back({"to sprint", [](C& c) { c.Sprint = true; }, [](C& c) { return c.Time > 1.6f && c.State() == "Sprint"; }, 4, nullptr});
    steps.push_back({"from sprint into walk", [](C& c) { c.Sprint = false; }, [](C& c) { return c.Time > 1.6f && c.State() == "Walk"; }, 4, nullptr});
    steps.push_back({"aim walking", [](C& c) { c.Aim = true; }, [](C& c) { return c.Time > 0.7f && c.State() == "Aim"; }, 4, nullptr});
    steps.push_back({"stop", [](C& c) { c.Aim = false; c.Move = {}; }, [](C& c) { return c.Time > 0.5f && c.State() == "Idle"; }, 4,
        [this](C& c) {
            for (const char* state : {"IdleToSprint", "SprintToIdle"})
                c.Check(m_SprintSamples.count(std::string(state) + "_1") > 0, std::string("captured moving ") + state);
        }});
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::BuildBodyProbe() {
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    static const char* kViews[10] = {"stock", "right", "front_r", "front_l", "left", "back_r", "back_l", "arm_r", "arm_l", "legs"};
    std::vector<Step> steps = {{"AK in hand", nullptr, [](C& c) { return c.State() == "Idle"; }, 30, nullptr}};
    if (!m_ProbeAk)
        steps.push_back({"switch to the Remington", [](C& c) { c.P->SelectSlot(1); },
                         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 30, nullptr});
    steps.push_back({"settle", nullptr, hold(2.0f), 5, nullptr});
    const std::string gun = m_ProbeAk ? "ak" : "rem";

    // --- Locomotion: one segment per move, the body's states and its planted feet measured throughout. -----------
    struct Shot { float At; int View; };
    struct Seg {
        std::string Name;
        std::function<void(C&)> Begin;
        float Hold;
        std::vector<std::string> Expect; // each must play during the segment (a prefix: "StartTurn" matches L and R)
        std::vector<Shot> Shots;
        bool Slide = true;               // check the planted feet hold
    };
    auto add = [&](const Seg& sg) {
        if (!m_BodyOnly.empty() && sg.Name.find(m_BodyOnly) == std::string::npos) return;
        steps.push_back({"body " + sg.Name,
                         [this, sg](C& c) {
                             m_Body = BodyStats{};
                             m_BodyMeasure = true;
                             c.View = sg.Shots.empty() ? 0 : sg.Shots.front().View;
                             if (sg.Begin) sg.Begin(c);
                         },
                         [sg, gun](C& c) {
                             for (const Shot& s : sg.Shots) {
                                 if (c.Time >= s.At - 0.15f && c.Time < s.At) c.View = s.View; // the camera there first
                                 if (c.Time >= s.At && c.Time < s.At + c.Dt) c.Shot = ShotStem("body_" + gun + "_" + sg.Name + "_" + kViews[s.View]);
                             }
                             return c.Time >= sg.Hold;
                         },
                         sg.Hold + 5.0f,
                         [this, sg](C& c) {
                             m_BodyMeasure = false;
                             const BodyStats& b = m_Body;
                             std::string seq;
                             for (const std::string& s : b.States) seq += (seq.empty() ? "" : ">") + s;
                             float mean = 0.0f, worst = 0.0f;
                             for (float s : b.Slides) { mean += s; worst = std::max(worst, s); }
                             if (!b.Slides.empty()) mean /= (float)b.Slides.size();
                             std::printf("[Body] %-22s %-60s plants %2d slide mean %.1f max %.1f cm  hands %.1f / world %.1f cm  bodies %.1f cm\n",
                                         sg.Name.c_str(), seq.c_str(), (int)b.Slides.size(), mean * 100.0f, worst * 100.0f, b.HandGap * 100.0f,
                                         b.TwinHandGap * 100.0f, b.BodyDiff * 100.0f);
                             if (!b.PlantLog.empty()) std::printf("[Body]   plants:%s\n", b.PlantLog.c_str());
                             for (const std::string& want : sg.Expect) {
                                 bool seen = false;
                                 for (const std::string& s : b.States) seen = seen || s.rfind(want, 0) == 0;
                                 c.Check(seen, sg.Name + ": plays " + want + " (" + seq + ")");
                             }
                             char buf[160];
                             if (sg.Slide && !b.Slides.empty()) {
                                 std::snprintf(buf, sizeof buf, "%s: planted feet hold (mean %.1f cm, worst %.1f cm)", sg.Name.c_str(), mean * 100.0f, worst * 100.0f);
                                 // The MC clips' own feet drift 0.5-3 cm a plant by this measure (--clip-report): within ~2 cm of that.
                                 c.Check(mean < 0.04f && worst < 0.07f, buf);
                             }
                             std::snprintf(buf, sizeof buf, "%s: the world body's hands on the gun (%.1f cm)", sg.Name.c_str(), b.TwinHandGap * 100.0f);
                             c.Check(b.TwinHandGap < 0.02f, buf);
                             c.View = 0;
                         }});
    };
    auto move = [](glm::vec2 m, bool sprint = false, bool crouch = false) {
        return [m, sprint, crouch](C& c) { c.Move = m; c.Sprint = sprint; c.Crouch = crouch; c.Cam->Pitch = 0.0f; };
    };
    add({"idle", move({}), 1.5f, {"Locomotion"}, {{1.2f, 2}}});
    add({"jog_start", move({0, 1}), 0.7f, {"Start"}, {{0.25f, 1}, {0.5f, 2}}});
    add({"jog", move({0, 1}), 1.4f, {"Locomotion"}, {{0.8f, 1}, {1.1f, 9}}});
    add({"jog_stop", move({}), 1.6f, {"Stop"}, {{0.15f, 1}, {0.4f, 2}}});
    add({"walk_start", move({0, 0.45f}), 0.9f, {"StartWalk"}, {{0.4f, 1}}});
    add({"walk", move({0, 0.45f}), 1.4f, {"Locomotion"}, {{0.9f, 9}}});
    add({"walk_stop", move({}), 1.8f, {"StopWalk"}, {{0.3f, 1}}});
    add({"strafe_left_start", move({-1, 0}), 0.7f, {"Start"}, {{0.3f, 2}}});
    add({"strafe_left", move({-1, 0}), 1.0f, {"Locomotion"}, {{0.6f, 2}}});
    add({"strafe_left_stop", move({}), 1.6f, {"Stop"}, {{0.2f, 2}}});
    add({"back_start", move({0, -1}), 0.7f, {"Start"}, {}});
    add({"back", move({0, -1}), 1.0f, {"Locomotion"}, {{0.6f, 1}}});
    add({"pivot_to_fwd", move({0, 1}), 1.2f, {"PivotJog"}, {{0.1f, 1}, {0.25f, 1}, {0.4f, 1}}});
    add({"pivot_stop", move({}), 1.6f, {"Stop"}, {}});
    add({"tap_right", move({1, 0}), 0.15f, {"Start"}, {}, false});
    add({"tap_right_release", move({}), 1.4f, {"IdleStep"}, {{0.3f, 2}}});
    add({"sprint_start", move({0, 1}, true), 1.0f, {"StartRun"}, {{0.4f, 1}}});
    add({"sprint", move({0, 1}, true), 1.6f, {"Sprint"}, {{0.6f, 1}, {1.0f, 2}, {1.3f, 9}}});
    add({"sprint_diagonal", move({-0.7071f, 0.7071f}, true), 1.6f, {"Sprint"}, {{0.8f, 2}, {1.2f, 4}}});
    add({"sprint_stop", move({}), 2.0f, {"StopRun"}, {{0.2f, 1}, {0.5f, 1}}});
    add({"turn_on_spot", [](C& c) { c.Move = {}; c.Cam->Yaw += 90.0f; }, 2.2f, {"Turn"}, {{0.5f, 2}, {1.0f, 2}}, false});
    add({"turning_start", [](C& c) { c.Cam->Yaw -= 110.0f; c.Move = {0, 1}; }, 1.2f, {"StartTurn"}, {{0.3f, 2}, {0.7f, 2}}, false});
    add({"turning_start_jog", move({0, 1}), 1.0f, {"Locomotion"}, {}});
    add({"turning_start_stop", move({}), 1.6f, {"Stop"}, {}});
    add({"crouch", move({}, false, true), 1.6f, {"CrouchLoco"}, {{1.2f, 2}}});
    add({"crouch_start", move({0, 1}, false, true), 0.9f, {"CrouchStart"}, {{0.4f, 1}}});
    add({"crouch_walk", move({0, 1}, false, true), 1.4f, {"CrouchLoco"}, {{0.8f, 1}, {1.1f, 9}}});
    add({"crouch_pivot", move({0, -1}, false, true), 1.4f, {"CrouchPivot"}, {{0.3f, 1}}});
    add({"crouch_stop", move({}, false, true), 2.0f, {"CrouchStop"}, {{0.3f, 1}}});
    add({"stand_up", move({}), 1.5f, {"Locomotion"}, {}});
    if (!m_BodyOnly.empty()) {
        m_Steps = std::move(steps);
        return;
    }
    // An idle fidget comes 10-20 s into standing still with the gun lowered.
    steps.push_back({"body fidget", [this](C& c) { m_Body = BodyStats{}; m_BodyMeasure = true; c.Move = {}; c.View = 2; m_FidgetSince = -1.0f; },
                     [this, gun](C& c) {
                         bool fidget = false;
                         for (const std::string& s : m_Body.States) fidget = fidget || s == "Fidget";
                         if (fidget && m_FidgetSince < 0.0f) m_FidgetSince = c.Time;
                         if (m_FidgetSince >= 0.0f && c.Time - m_FidgetSince >= 1.2f && c.Time - m_FidgetSince < 1.2f + c.Dt)
                             c.Shot = ShotStem("body_" + gun + "_fidget_front_r");
                         return m_FidgetSince >= 0.0f && c.Time - m_FidgetSince >= 1.6f;
                     },
                     26.0f, [this](C& c) {
                         m_BodyMeasure = false;
                         std::string seq;
                         for (const std::string& s : m_Body.States) seq += (seq.empty() ? "" : ">") + s;
                         std::printf("[Body] %-22s %s\n", "fidget", seq.c_str());
                         c.View = 0;
                     }});
    steps.push_back({"after the fidget", nullptr, [](C& c) { return c.Time > 4.0f; }, 6, nullptr});

    // --- The gun: held still in each pose, both bodies' hands on it, from all round and close. -----------------
    auto pose = [&](const std::string& name, bool crouch, bool aim, float pitch, glm::vec2 mv, std::vector<int> views) {
        steps.push_back({"hold " + name,
                         [crouch, aim, pitch, mv](C& c) { c.View = 0; c.Crouch = crouch; c.Aim = aim; c.Cam->Pitch = pitch; c.Move = mv; c.Sprint = false; },
                         [aim](C& c) { return c.Time >= 1.4f && (!aim || c.State() == "Aim"); }, 6,
                         [this, name](C& c) {
                             PrintSample("hold " + name);
                             std::printf("[Hold] %-16s hand gap own L %.1f R %.1f  world L %.1f R %.1f cm  wrist roll L %.0f R %.0f, at the wrist L %.0f R %.0f deg\n",
                                         name.c_str(), BodyDebug::Info().HandGap[0] * 100.0f, BodyDebug::Info().HandGap[1] * 100.0f, m_HoldGap[0] * 100.0f,
                                         m_HoldGap[1] * 100.0f, m_Wrist[0], m_Wrist[1], m_WristLeft[0], m_WristLeft[1]);
                             c.Check(m_HoldGap[0] < 0.02f && m_HoldGap[1] < 0.02f, name + ": world hands on the gun");
                         }});
        for (int v : views)
            steps.push_back({name + " view " + kViews[v], [v](C& c) { c.View = v; }, hold(0.12f), 2,
                             [name, v, gun](C& c) { c.Shot = ShotStem("hold_" + gun + "_" + name + "_" + kViews[v]); }});
    };
    pose("idle", false, false, 0.0f, {}, {1, 2, 3, 4, 5, 6, 7, 8});
    pose("aim", false, true, 0.0f, {}, {1, 2, 3, 4, 5, 6, 7, 8});
    pose("aim_up", false, true, 50.0f, {}, {1, 2, 7, 8});
    pose("aim_down", false, true, -55.0f, {}, {1, 2, 7, 8});
    pose("hip_up", false, false, 60.0f, {}, {1, 2});
    pose("hip_down", false, false, -60.0f, {}, {1, 2});
    pose("crouch", true, false, 0.0f, {}, {1, 2, 5});
    pose("crouch_aim", true, true, 0.0f, {}, {1, 2, 7, 8, 9});
    pose("crouch_aim_up", true, true, 45.0f, {}, {1, 2, 7});
    pose("crouch_aim_down", true, true, -55.0f, {}, {1, 2, 7});
    pose("aim_strafe", false, true, 0.0f, {1, 0}, {2, 7});
    steps.push_back({"guns down", [](C& c) { c.Aim = false; c.Crouch = false; c.Move = {}; c.Cam->Pitch = 0.0f; },
                     [](C& c) { return c.State() == "Idle" && c.Time > 1.0f; }, 5, nullptr});
    // Moving with the gun: the jog and the sprint, filmed close.
    steps.push_back({"jog with gun", [](C& c) { c.Move = {0, 1}; c.View = 7; },
                     [gun](C& c) {
                         if (std::abs(c.Time - 1.0f) < c.Dt * 0.5f) c.Shot = ShotStem("hold_" + gun + "_jog_arm_r");
                         return c.Time > 1.3f;
                     }, 4, nullptr});
    steps.push_back({"sprint with gun", [](C& c) { c.Sprint = true; c.View = 7; },
                     [gun](C& c) {
                         if (std::abs(c.Time - 1.2f) < c.Dt * 0.5f) c.Shot = ShotStem("hold_" + gun + "_sprint_arm_r");
                         if (c.Time > 1.3f) c.View = 8;
                         if (std::abs(c.Time - 1.6f) < c.Dt * 0.5f) c.Shot = ShotStem("hold_" + gun + "_sprint_arm_l");
                         return c.Time > 1.7f;
                     }, 4, [](C& c) { c.Sprint = false; c.Move = {}; c.View = 0; }});
    steps.push_back({"settle 2", nullptr, [](C& c) { return c.State() == "Idle" && c.Time > 1.0f; }, 6, nullptr});
    // Firing, the actions, the reloads: filmed through, the world hands' worst gap to the gun noted.
    auto action = [&](const std::string& name, std::function<void(C&)> begin, std::function<bool(C&)> done, float seconds) {
        steps.push_back({"act " + name, [this, begin](C& c) { c.View = 2; m_ActGap = 0.0f; begin(c); },
                         [this, name, done, gun](C& c) {
                             const int frame = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (frame > 0 && frame % 9 == 0 && frame <= 99) c.Shot = ShotStem("act_" + gun + "_" + name + "_" + std::to_string(frame));
                             if (frame % 9 == 6) c.View = (frame / 9) % 2 ? 8 : 2; // alternate front right and the left arm close
                             m_ActGap = std::max(m_ActGap, std::max(m_HoldGap[0], m_HoldGap[1]));
                             return done(c);
                         }, seconds,
                         [this, name](C& c) {
                             std::printf("[Hold] act %-12s worst world hand gap %.1f cm\n", name.c_str(), m_ActGap * 100.0f);
                             c.View = 0;
                             c.Trigger = false;
                         }});
    };
    action("fire", [](C& c) { c.Trigger = true; }, [](C& c) { return c.Time > 0.8f; }, 3);
    steps.push_back({"after fire", [](C& c) { c.Trigger = false; }, [](C& c) { return c.State() == "Idle" && c.Time > 0.6f; }, 6, nullptr});
    if (m_ProbeAk) {
        action("tac_reload", [](C& c) { c.P->SetAmmo(3); c.P->Reload(); }, [](C& c) { return c.Time > 0.5f && !c.P->IsReloading(); }, 10);
        action("empty_reload", [](C& c) { c.P->SetAmmo(0); c.P->Reload(); }, [](C& c) { return c.Time > 0.5f && !c.P->IsReloading(); }, 10);
    } else {
        action("reload", [](C& c) { c.P->SetAmmo(4); c.P->Reload(); }, [](C& c) { return c.Time > 0.5f && !c.P->IsReloading(); }, 15);
    }
    action("inspect", [](C& c) { c.P->TriggerAction("Inspect"); }, [](C& c) { return c.Saw("Inspect") && c.State() == "Idle"; }, 12);
    action("mag_check", [](C& c) { c.P->TriggerAction("MagCheck"); }, [](C& c) { return c.Saw("MagCheck") && c.State() == "Idle"; }, 10);
    action("melee", [](C& c) { c.P->TriggerAction("Melee"); }, [](C& c) { return c.Saw("Melee") && c.State() == "Idle"; }, 8);
    action("holster", [](C& c) { c.P->SetEquipped(false); }, [](C& c) { return c.Time > 1.2f; }, 4);
    action("draw", [](C& c) { c.P->SetEquipped(true); }, [](C& c) { return c.State() == "Idle" && c.Time > 1.0f; }, 6);
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::BuildGaitProbe() {
    using C = Ctx;
    std::vector<Step> steps = {{"AK in hand", nullptr, [](C& c) { return c.State() == "Idle"; }, 30, nullptr}};
    struct Seg { std::string Name; glm::vec2 Move; bool Sprint, Crouch; };
    std::vector<Seg> segs = {{"stand", {0, 0}, false, false}};
    const struct { const char* Name; glm::vec2 Dir; } dirs[] = {
        {"fwd", {0, 1}}, {"bwd", {0, -1}}, {"left", {-1, 0}}, {"right", {1, 0}},
        {"fwd_left", {-0.7071f, 0.7071f}}, {"bwd_right", {0.7071f, -0.7071f}}, {"fwd_right", {0.7071f, 0.7071f}}, {"bwd_left", {-0.7071f, -0.7071f}}};
    for (const auto& gait : {std::pair<const char*, float>{"walk", 0.45f}, {"jog", 1.0f}})
        for (const auto& d : dirs) segs.push_back({std::string(gait.first) + "_" + d.Name, d.Dir * gait.second, false, false});
    for (const auto& d : dirs) segs.push_back({std::string("crouch_") + d.Name, d.Dir, false, true});
    segs.push_back({"sprint_fwd", {0, 1}, true, false});
    segs.push_back({"sprint_fwd_left", {-0.7071f, 0.7071f}, true, false});
    segs.push_back({"sprint_fwd_right", {0.7071f, 0.7071f}, true, false});
    // With --smoke-shots: a few frames of these, the whole body from the Scene camera (1 right side, 2 front right).
    const std::map<std::string, int> shotViews = {{"walk_fwd", 2}, {"jog_fwd", 1}, {"sprint_fwd", 1},
                                                  {"jog_fwd_left", 2}, {"jog_bwd", 2}, {"crouch_fwd", 1}};
    for (const Seg& s : segs) {
        const std::string name = s.Name;
        const auto shot = shotViews.find(name);
        const int view = shot == shotViews.end() ? 0 : shot->second;
        steps.push_back({name, [this, s, name, view](C& c) {
                             c.Move = s.Move; c.Sprint = s.Sprint; c.Crouch = s.Crouch; c.Cam->Pitch = 0.0f; c.View = view;
                             m_Gait[0] = m_Gait[1] = GaitStats{};
                             m_GaitSegment = name;
                             m_GaitTravel = s.Move;
                         },
                         [name, view](C& c) {
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (view && f >= 90 && f <= 120 && f % 10 == 0) c.Shot = ShotStem("gait_" + name + "_" + std::to_string(f));
                             return c.Time >= 2.4f;
                         }, 5.0f,
                         [this, name, s](C& c) {
                             m_GaitSegment.clear();
                             c.View = 0;
                             auto mean = [](const GaitStats& g, float v) { return g.Frames ? v / (float)g.Frames : 0.0f; };
                             for (int v = 0; v < 2; ++v) {
                                 const GaitStats& g = m_Gait[v];
                                 std::printf("[Gait] %-17s %-5s lean %5.1f (%5.1f..%5.1f) side %5.1f, chest on hips %5.1f (%5.1f..%5.1f) deg, pelvis %.3f m, %d frames\n",
                                             name.c_str(), v ? "world" : "own", mean(g, g.Lean), g.LeanMin, g.LeanMax, mean(g, g.Side),
                                             mean(g, g.Twist), g.TwistMin, g.TwistMax, mean(g, g.Pelvis), g.Frames);
                             }
                             const GaitStats &own = m_Gait[0], &w = m_Gait[1];
                             c.Check(own.Frames > 30 && w.Frames > 30, name + ": measured");
                             if (glm::length(s.Move) < 0.01f) {
                                 // Standing, nothing to steady: both views the same body.
                                 const float d = std::fabs(mean(own, own.Lean) - mean(w, w.Lean));
                                 c.Check(d < 1.0f, name + ": the world body stands as the player's own (" + std::to_string(d) + " deg apart)");
                             } else {
                                 // Moving, the world body carries the gait's own torso: its lean swings over the stride.
                                 c.Check(w.LeanMax - w.LeanMin > 0.5f, name + ": the world torso moves with the gait (" +
                                                                           std::to_string(w.LeanMax - w.LeanMin) + " deg of lean swing)");
                             }
                             c.Move = {}; c.Sprint = false; c.Crouch = false;
                         }});
        // Back where it started (unmeasured), so the run stays on the open floor.
        if (glm::length(s.Move) > 0.01f)
            steps.push_back({name + " back", [s](C& c) { c.Move = -s.Move; c.Sprint = false; c.Crouch = s.Crouch; },
                             [s](C& c) { return c.Time >= (s.Sprint ? 3.4f : 2.4f); }, 6.0f,
                             [](C& c) { c.Move = {}; c.Crouch = false; }});
    }
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
    if (m_BodyProbe) {
        m_HoldGap[0] = body.TwinHandGap(0);
        m_HoldGap[1] = body.TwinHandGap(1);
        for (int s = 0; s < 2; ++s) { m_Wrist[s] = body.WristRoll(s); m_WristLeft[s] = body.WristResidual(s); }
    }
    // Body probe: the body's states, its planted feet (the world body's, as every other view sees them), and both
    // bodies' hands.
    if (m_BodyProbe && m_BodyMeasure) {
        BodyStats& b = m_Body;
        const std::string& state = BodyDebug::Info().AnimatorState;
        if (!state.empty() && (b.States.empty() || b.States.back() != state)) b.States.push_back(state);
        // The ball of each foot: what stays put on the ground from the foot landing flat until it pushes off (the
        // ankle rolls forward over it, which is no slide).
        static const char* kFeet[2] = {"ball_l", "ball_r"};
        glm::vec3 foot[2];
        if (body.BoneWorld(world, kFeet[0], foot[0]) && body.BoneWorld(world, kFeet[1], foot[1])) {
            const float ground = std::min(foot[0].y, foot[1].y);
            for (int f = 0; f < 2; ++f) {
                const glm::vec2 xz(foot[f].x, foot[f].z);
                const float speed = b.Have && m_Ctx.Dt > 0.0f ? glm::length(xz - glm::vec2(b.LastFoot[f].x, b.LastFoot[f].z)) / m_Ctx.Dt : 1e9f;
                const bool planted = foot[f].y < ground + 0.02f && speed < 0.5f;
                if (planted) {
                    if (b.PlantFrames[f] == 0) {
                        b.PlantStart[f] = foot[f];
                        b.PlantSlide[f] = 0.0f;
                    }
                    ++b.PlantFrames[f];
                    b.PlantSlide[f] = std::max(b.PlantSlide[f], glm::length(xz - glm::vec2(b.PlantStart[f].x, b.PlantStart[f].z)));
                } else {
                    if (b.PlantFrames[f] >= 5) {
                        b.Slides.push_back(b.PlantSlide[f]);
                        char one[96];
                        std::snprintf(one, sizeof one, " %s%.1f@%s", f ? "R" : "L", b.PlantSlide[f] * 100.0f, state.c_str());
                        b.PlantLog += one;
                    }
                    b.PlantFrames[f] = 0;
                }
                b.LastFoot[f] = foot[f];
            }
            b.Have = true;
        }
        b.HandGap = std::max(b.HandGap, std::max(BodyDebug::Info().HandGap[0], BodyDebug::Info().HandGap[1]));
        b.TwinHandGap = std::max(b.TwinHandGap, std::max(m_HoldGap[0], m_HoldGap[1]));
        if (glm::vec3 own, out; body.BoneWorld(world, "hand_r", own, false) && body.BoneWorld(world, "hand_r", out, true))
            b.BodyDiff = std::max(b.BodyDiff, glm::length(own - out));
        ++b.Frames;
    }
    // Gait probe: past the segment's first second (the gait settled), the torso against the hips, both bodies.
    if (m_GaitProbe && !m_GaitSegment.empty() && m_Ctx.Time > 1.0f && m_Ctx.Cam) {
        const glm::vec3 front = m_Ctx.Cam->Front();
        const glm::vec3 fwd = glm::normalize(glm::vec3(front.x, 0.0f, front.z) + glm::vec3(1e-6f, 0.0f, 0.0f));
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        // The travel's direction (the view's frame), else the view's forward standing.
        const glm::vec3 travel = glm::length(m_GaitTravel) > 0.01f ? glm::normalize(right * m_GaitTravel.x + fwd * m_GaitTravel.y) : fwd;
        const glm::vec3 across = glm::cross(travel, glm::vec3(0.0f, 1.0f, 0.0f));
        auto heading = [](const glm::vec3& v) { return glm::degrees(std::atan2(v.x, v.z)); };
        for (int v = 0; v < 2; ++v) {
            glm::vec3 pelvis, neck, thigh[2], arm[2], foot[2];
            auto bone = [&](const char* name, glm::vec3& out) { return body.BoneWorld(world, name, out, v == 1); };
            if (!bone("pelvis", pelvis) || !bone("neck_01", neck) || !bone("thigh_l", thigh[0]) || !bone("thigh_r", thigh[1]) ||
                !bone("upperarm_l", arm[0]) || !bone("upperarm_r", arm[1]) || !bone("foot_l", foot[0]) || !bone("foot_r", foot[1]))
                continue;
            const glm::vec3 up = neck - pelvis;
            const float lean = glm::degrees(std::atan2(glm::dot(up, travel), std::max(up.y, 1e-3f)));
            const float side = glm::degrees(std::atan2(glm::dot(up, -across), std::max(up.y, 1e-3f)));
            float twist = heading(arm[0] - arm[1]) - heading(thigh[0] - thigh[1]);
            while (twist > 180.0f) twist -= 360.0f;
            while (twist <= -180.0f) twist += 360.0f;
            GaitStats& g = m_Gait[v];
            ++g.Frames;
            g.Lean += lean;
            g.Side += side;
            g.Twist += twist;
            g.Pelvis += pelvis.y - std::min(foot[0].y, foot[1].y);
            g.LeanMin = std::min(g.LeanMin, lean);
            g.LeanMax = std::max(g.LeanMax, lean);
            g.TwistMin = std::min(g.TwistMin, twist);
            g.TwistMax = std::max(g.TwistMax, twist);
        }
    }
    if (m_SprintProbe) {
        const auto* animator = world.Registry.try_get<AnimatorControllerComponent>(p.ArmsEntity());
        const auto controller = GetAnimatorController(p.Set().Controller);
        if (animator && controller && animator->Layers.size() >= 3 && !animator->Layers[1].Stack.empty()) {
            const auto& item = animator->Layers[1].Stack.back();
            const auto& state = controller->Layers[1].States[item.State];
            if ((state.Name == "IdleToSprint" || state.Name == "SprintToIdle") && item.Phase >= 0.2f && item.Phase < 0.95f) {
                const int sample = item.Phase < 0.45f ? 0 : item.Phase < 0.7f ? 1 : 2;
                const std::string key = state.Name + "_" + std::to_string(sample);
                if (m_SprintSamples.insert(key).second) {
                    const float weight = AnimatorLayerWeight(*controller, *animator, 2);
                    const auto& walk = animator->Layers[2].Stack.back();
                    m_Ctx.Check(weight > 0.0f && controller->Layers[2].States[walk.State].Name == "Walk", key + " has additive walking");
                    std::printf("[SprintWalk] %s phase %.3f walk weight %.3f walk phase %.3f\n", state.Name.c_str(), item.Phase, weight, walk.Phase);
                    m_Shot = key;
                }
            }
        }
    }
    static std::set<int> cameraChecked;
    if(!m_Probe && m_Ctx.Aim && p.IsReloading() && m_Ctx.Time>.4f && !cameraChecked.count(p.Slot())) {
        cameraChecked.insert(p.Slot());
        const auto* render=world.Registry.try_get<RenderableComponent>(p.ArmsEntity());
        const auto* weaponRender=world.Registry.try_get<RenderableComponent>(p.WeaponEntity());
        const auto* animator=world.Registry.try_get<AnimatorControllerComponent>(p.ArmsEntity());
        const auto controller=GetAnimatorController(p.Set().Controller);
        m_Ctx.Check(render && render->ModelRef && animator && controller,"camera action sampler has live arms and animator");
        if(render && render->ModelRef && animator && controller) {
            AssetLibrary assets;
            // A moving socket stands in for an exported camera bone; use actual reload keys.
            ActionCameraSettings settings; settings.Enabled=true; settings.Node=p.Set().Procedural.IK.GunBone; settings.PositionScale=1;
            Model& arms=*render->ModelRef;
            Model* weapon=weaponRender?weaponRender->ModelRef.get():nullptr;
            auto sample=[&](const AnimatorControllerComponent& ac) {
                return AnimatorActionCamera(arms,weapon,assets,*controller,ac,settings,"",glm::quat(1,0,0,0),1,1);
            };
            auto full=*animator; full.Layers[0].Stack={full.Layers[0].Stack.back()}; full.Layers[0].Stack[0].Fade=1;
            const auto effect=sample(full);
            const auto testNode=settings.Node;
            settings.Node=p.Set().ActionCamera.Node;
            const auto headMotion=sample(full);
            std::printf("[WeaponTest]     head camera motion: %.3f deg, %.3f cm\n",
                glm::degrees(2*std::acos(std::clamp(std::abs(headMotion.Rotation.w),0.0f,1.0f))),glm::length(headMotion.Position)*100);
            settings.Node=testNode;
            m_Ctx.Check(glm::length(effect.Position)>1e-5f || std::abs(effect.Rotation.w-1)>1e-5f,"camera channel samples original action motion during ADS reload");
            const int idle=controller->Layers[0].FindState("Idle");
            auto fade=full; fade.Layers[0].Stack.push_back({idle,0,.5f});
            const auto half=sample(fade);
            m_Ctx.Check(glm::length(half.Position-effect.Position*.5f)<1e-5f &&
                std::abs(glm::dot(half.Rotation,glm::slerp(effect.Rotation,glm::quat(1,0,0,0),.5f)))>.99999f,
                "camera action crossfades back to neutral");
            settings.AdsScale=0;
            const auto off=sample(full);
            m_Ctx.Check(glm::length(off.Position)==0 && std::abs(off.Rotation.w-1)<1e-5f,"camera action ADS scale zero disables motion");
        }
    }
    if(!m_Probe && m_Ctx.Aim && p.CurrentState()=="Aim" && m_Ctx.Time>0.5f && m_Ctx.Time<=0.5f+m_Ctx.Dt) {
        const auto* animator=world.Registry.try_get<AnimatorControllerComponent>(p.ArmsEntity());
        m_Ctx.Check(animator && std::abs(animator->GetFloat("LayerWeight:Weapon Locomotion")-p.Set().AdsLocomotionScale)<1e-4f,
                    "ADS locomotion weight reaches the weapon animator");
        m_Ctx.Check(animator && std::abs(animator->GetFloat("LayerWeight:Weapon Walk")-p.Set().AdsLocomotionScale)<1e-4f,
                    "ADS walk weight reaches the weapon animator");
    }
    if (!m_Probe) RecordAudioFrame(world, p);
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
    // The world gun (split poses): the first-person one moved by the body's world gun delta.
    const glm::mat4& gunDelta = body.WorldGunDelta();
    if (p.StockWorld(butt, fwd) && ((butt = glm::vec3(gunDelta * glm::vec4(butt, 1.0f)), fwd = glm::normalize(glm::mat3(gunDelta) * fwd)), true) && body.BoneWorld(world, "upperarm_r", upper) && body.BoneWorld(world, "clavicle_r", clav) &&
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
        if (glm::vec3 chest; body.BoneWorld(world, "spine_05", chest) && glm::length(neck - chest) > 1e-4f && glm::length(head - neck) > 1e-4f)
            s.HeadBend = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(neck - chest), glm::normalize(head - neck)), -1.0f, 1.0f)));
        // The world hands against where the hold put them on the world gun.
        for (int h = 0; h < 2; ++h) s.HandGap[h] = body.TwinHandGap(h);
        s.GunShift = glm::length(glm::vec3(gunDelta[3]));
        s.Valid = true;
        // The Scene camera: in front of the body and to its right, on the gun's rear and the shoulder - or (pose probe)
        // the whole body from its right, its front right or its front left.
        glm::vec3 target = 0.5f * (butt + 0.5f * (neck + upper)) + fwd * 0.08f;
        m_SceneCamPos = target + front * 1.05f + right * 0.75f + up * 0.12f;
        if (m_Ctx.View > 0) {
            glm::vec3 feet = cam.Position;
            feet.y = neck.y - 1.45f;
            target = glm::vec3(neck.x, feet.y + 0.85f * (neck.y - feet.y) * 0.85f, neck.z);
            glm::vec3 dir = glm::normalize(front - right * 0.85f);
            switch (m_Ctx.View) {
            case 1: dir = right; break;
            case 2: dir = glm::normalize(front + right * 0.75f); break;
            case 4: dir = -right; break;                              // left side
            case 5: dir = glm::normalize(-front + right * 0.75f); break; // back right
            case 6: dir = glm::normalize(-front - right * 0.75f); break; // back left
            default: break;
            }
            m_SceneCamPos = target + dir * 2.4f + up * 0.15f;
            // Close: an arm (shoulder to hand) from in front and to its side, or the legs from the right.
            glm::vec3 a, b;
            const bool arm = m_Ctx.View == 7 || m_Ctx.View == 8;
            const std::string side = m_Ctx.View == 7 ? "_r" : "_l";
            if (arm && body.BoneWorld(world, "upperarm" + side, a) && body.BoneWorld(world, "hand" + side, b)) {
                target = 0.5f * (a + b);
                dir = glm::normalize(front * 0.8f + (m_Ctx.View == 7 ? right : -right) * 0.6f + up * 0.15f);
                m_SceneCamPos = target + dir * 0.85f;
            } else if (m_Ctx.View == 9 && body.BoneWorld(world, "calf_l", a) && body.BoneWorld(world, "calf_r", b)) {
                target = 0.5f * (a + b);
                m_SceneCamPos = target + glm::normalize(right * 0.9f + front * 0.4f) * 1.5f + up * 0.1f;
            }
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
    if (Done() && !m_Probe && !m_AudioChecked) {
        m_AudioChecked = true;
        CheckAudio();
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

// --- Audio ------------------------------------------------------------------------------------------------------

void FirstPersonWeaponTest::RecordAudioFrame(const World& world, const FirstPersonPresentation& p) {
    AudioFrame f;
    f.Slot = p.Slot();
    f.Controller = p.Set().Controller;
    if (const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(p.ArmsEntity())) {
        f.State = ac->StateName;
        f.Phase = ac->StateTime;
    }
    const std::vector<WeaponAudio::Emitted>& t = WeaponAudio::Get().Transcript();
    for (; m_AudioSeen < t.size(); ++m_AudioSeen) f.Keys.push_back(t[m_AudioSeen].Key);
    m_AudioFrames.push_back(std::move(f));
}

// The audio is data on the controllers (snd.* events); this replays the whole run's frames against them: every event the
// animator crossed must have been played, in order, by the frame after it crossed (+-1 frame), and nothing else of the
// animator's keys may have been. Then the gear sounds the script's actions should have made (ADS, fire mode, dry
// trigger, equip / unequip, the shot layers), counted.
namespace {
// "snd.ak.mag_out@95" -> "snd.ak.mag_out": the event's lead before its contact is not part of the key.
std::string BaseKey(const std::string& name) { return name.substr(0, name.find('@')); }
} // namespace

void FirstPersonWeaponTest::CheckAudio() {
    Ctx& c = m_Ctx;
    std::printf("[WeaponTest] audio (snd.* events against the animator, %d frames)\n", (int)m_AudioFrames.size());
    // The controllers by slot.
    AnimatorController ctrl[2];
    bool have[2] = {false, false};
    for (const AudioFrame& f : m_AudioFrames)
        if (f.Slot >= 0 && f.Slot < 2 && !have[f.Slot] && !f.Controller.empty())
            have[f.Slot] = AnimatorController::LoadFile(ProjectPaths::Resolve(f.Controller), ctrl[f.Slot]);
    c.Check(have[0] && have[1], "both weapons' controllers loaded");
    if (!(have[0] && have[1])) return;

    struct Expect { int Frame; std::string Key; };
    std::vector<Expect> expected;
    std::vector<Expect> observed;
    std::set<std::string> animatorKeys;
    for (const AnimatorController& a : ctrl)
        for (const AnimatorController::State& s : a.Layers[0].States)
            for (const AnimatorController::Event& e : s.Events)
                if (e.Name.rfind("snd.", 0) == 0) animatorKeys.insert(BaseKey(e.Name));
    c.Check(!animatorKeys.empty(), std::to_string(animatorKeys.size()) + " snd.* keys on the controllers' states");

    std::string prevState;
    float prevPhase = 0.0f;
    int prevSlot = -1;
    for (int i = 0; i < (int)m_AudioFrames.size(); ++i) {
        const AudioFrame& f = m_AudioFrames[i];
        for (const std::string& k : f.Keys)
            if (animatorKeys.count(k)) observed.push_back({i, k});
        if (f.Slot < 0 || f.Slot > 1 || f.State.empty()) continue;
        const AnimatorController::Layer& L = ctrl[f.Slot].Layers[0];
        const int si = L.FindState(f.State);
        if (si < 0) continue;
        const AnimatorController::State& st = L.States[si];
        const bool entered = f.State != prevState || f.Slot != prevSlot || f.Phase < prevPhase - 1e-6f;
        const float from = entered ? -1e-6f : prevPhase;
        // The controller's own rule (AnimatorController.cpp CrossEvents): events in (from, to], per pass for a loop.
        if (f.Phase > from) {
            const int first = st.Loop ? (int)std::floor(std::max(from, 0.0f)) : 0;
            const int last = st.Loop ? (int)std::floor(f.Phase) : 0;
            for (int k = first; k <= last; ++k)
                for (const AnimatorController::Event& e : st.Events) {
                    const float t = e.Time + (st.Loop ? (float)k : 0.0f);
                    if (t > from && t <= f.Phase && e.Name.rfind("snd.", 0) == 0) expected.push_back({i, BaseKey(e.Name)});
                }
        }
        prevState = f.State;
        prevPhase = f.Phase;
        prevSlot = f.Slot;
    }
    std::printf("[WeaponTest]     %d snd.* events crossed, %d played\n", (int)expected.size(), (int)observed.size());
    c.Check(!expected.empty(), "the script crossed snd.* events");
    c.Check(expected.size() == observed.size(), "every crossed event was played, nothing else of the animator's (" + std::to_string(expected.size()) + " vs " + std::to_string(observed.size()) + ")");
    int worst = 0, wrongKey = 0;
    for (size_t i = 0; i < expected.size() && i < observed.size(); ++i) {
        if (expected[i].Key != observed[i].Key) {
            if (++wrongKey <= 5)
                std::printf("[WeaponTest]       #%d expected %s (frame %d), played %s (frame %d)\n", (int)i, expected[i].Key.c_str(), expected[i].Frame, observed[i].Key.c_str(), observed[i].Frame);
            continue;
        }
        worst = std::max(worst, std::abs(observed[i].Frame - 1 - expected[i].Frame)); // played the frame after the crossing
    }
    c.Check(wrongKey == 0, "played in the animator's order");
    c.Check(worst <= 1, "each within +-1 frame of the event time (worst " + std::to_string(worst) + ")");

    // Gear sounds the script's actions make, and the shot layers.
    std::map<std::string, int> count;
    bool all2D = true;
    for (const WeaponAudio::Emitted& e : WeaponAudio::Get().Transcript()) {
        ++count[e.Key];
        all2D &= e.At2D;
    }
    auto want = [&](const char* key, int n) {
        c.Check(count[key] == n, std::string(key) + " x" + std::to_string(count[key]) + " (want " + std::to_string(n) + ")");
    };
    auto atLeast = [&](const char* key, int n) {
        c.Check(count[key] >= n, std::string(key) + " x" + std::to_string(count[key]) + " (want " + std::to_string(n) + "+)");
    };
    want("snd.ak.firemode", 2);          // full auto on, off
    // ADS is the shared foley's (snd.foley.weapon.*): the AK and the Remington each aim twice. Drawing and putting away are the
    // guns' own draw / holster takes from their clips; the shared equip / unequip rattle is only for a gun without them.
    want("snd.foley.weapon.ads_in", 2);
    want("snd.foley.weapon.ads_out", 2);
    want("snd.870.dry_fire", 1);         // one pull on the empty tube
    want("snd.foley.weapon.equip", 0);
    want("snd.foley.weapon.unequip", 0);
    // One draw and one holster per switch, nothing doubled: 4 draws (back to the AK, 2 again, 3 from unarmed, the Remington's
    // first) and 4 holsters, across the two guns.
    const int draws = count["snd.ak.draw"] + count["snd.870.draw"], holsters = count["snd.ak.holster"] + count["snd.870.holster"];
    c.Check(draws == 4, "draws x" + std::to_string(draws) + " (want 4)");
    c.Check(holsters == 4, "holsters x" + std::to_string(holsters) + " (want 4)");
    atLeast("snd.ak.fire_close", 5);     // every round plays close / mech / sub (the AK fires 7)
    atLeast("snd.870.fire_close", 8);
    atLeast("snd.ak.fire_mech", 5);
    atLeast("snd.ak.fire_tail", 3);      // ... the tail on every 2nd of a burst, the first shot of one always
    c.Check(all2D, "the player's gun is all 2D");
}
