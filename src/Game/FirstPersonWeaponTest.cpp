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
#include "GameModuleAPI.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "ShellCasings.h"
#include "WeaponLab.h"
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
        else if (pose && *pose && *pose != '0') { m_PoseProbe = true; BuildPoseProbe(); }
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
#pragma warning(suppress : 4996)
    if (const char* only = std::getenv("STOCK_PROBE_POSE"); only && std::string(only) == "jank") { BuildJankProbe(); return; }
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    const bool ak = m_ProbeAk;
    std::vector<Step> steps = {{"AK in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f, nullptr}};
    if (!ak)
        steps.push_back({"2: switch to the Remington", [](C& c) { c.P->SelectSlot(1); c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f, nullptr});
    steps.push_back({"settle", nullptr, hold(2.0f), 5.0f, nullptr});
    // The regrip (the idle fidget), filmed from the right: the world gun against the first-person one, logged each frame
    // until a little after it has gone back to Idle.
    auto regrip = [&]() {
        steps.push_back({"regrip", [this](C& c) {
                             c.View = 1; c.Aim = false; c.Cam->Pitch = 0.0f; m_RegripLog = true;
                             m_RegripMin = glm::vec3(1e9f); m_RegripMax = glm::vec3(-1e9f);
                             c.P->TriggerAction("Fidget");
                         },
                         [](C& c) {
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (f % 6 == 0) c.Shot = "pose_regrip_" + std::to_string(1000 + f).substr(1);
                             return c.Saw("Regrip") && c.State() == "Idle" && c.Time > 0.3f;
                         }, 10.0f, nullptr});
        // The regrip keeps the stock in the shoulder (its state is shouldered): the world butt stays put against it. Let go
        // there, the world gun swung ~5 cm off the shoulder and back while the first-person one barely moved.
        steps.push_back({"regrip settles", nullptr, hold(0.4f), 2.0f, [this](C& c) {
                             m_RegripLog = false; c.View = 0;
                             const glm::vec3 span = m_RegripMax - m_RegripMin;
                             const float most = std::max(span.x, std::max(span.y, span.z));
                             char buf[160];
                             std::snprintf(buf, sizeof buf, "regrip: the world stock stays in the shoulder (moves %.1f cm)", most * 100.0f);
                             c.Check(most >= 0.0f && most < 0.02f, buf);
                         }});
    };
    // Sprinting into a reload, a mag check, an inspect: the sprint drops to a run for the action (the sprint arms over its
    // clip pulled the hands off the gun), then comes back with sprint still held. Filmed from the front right.
    auto sprintActions = [&]() {
        struct Act { const char* Name; std::function<void(C&)> Start; };
        const Act acts[] = {{"reload", [](C& c) { c.P->SetAmmo(1); c.P->Reload(); }},
                            {"magcheck", [](C& c) { c.P->TriggerAction("MagCheck"); }},
                            {"inspect", [](C& c) { c.P->TriggerAction("Inspect"); }}};
        for (const Act& a : acts) {
            const std::string name = std::string("sprint_") + a.Name;
            steps.push_back({name + ": sprint", [](C& c) { c.View = 0; c.Aim = false; c.Cam->Pitch = 0.0f; c.Move = {0, 1}; c.Sprint = true; },
                             [](C& c) { return c.Time > 1.2f && c.State() == "Sprint"; }, 4.0f,
                             [this](C& c) { m_SprintSeen = c.P->PlanarSpeed(); }});
            steps.push_back({name, [this, a](C& c) { c.View = 2; m_ActSpeed = 0.0f; a.Start(c); },
                             [this, name](C& c) {
                                 // (After the first half second: the body takes that long to come down from the sprint.)
                                 if (c.Time > 0.6f && c.State() != "Sprint" && c.State() != "IdleToSprint") m_ActSpeed = std::max(m_ActSpeed, c.P->PlanarSpeed());
                                 const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                                 if (f % 15 == 0) c.Shot = ShotStem("pose_" + name + "_" + std::to_string(1000 + f).substr(1));
                                 return c.Time > 0.3f && (c.State() == "Sprint" || c.State() == "IdleToSprint" || c.State() == "Walk");
                             }, 8.0f,
                             [this, name](C& c) {
                                 char buf[200];
                                 std::snprintf(buf, sizeof buf, "%s: the sprint drops to a run for it (%.2f m/s, sprinting %.2f)", name.c_str(), m_ActSpeed, m_SprintSeen);
                                 c.Check(m_SprintSeen > 0.0f && m_ActSpeed > 0.5f && m_ActSpeed < 0.85f * m_SprintSeen, buf);
                             }});
            steps.push_back({name + ": sprints again", nullptr, [](C& c) { return c.State() == "Sprint"; }, 4.0f, nullptr});
            steps.push_back({name + ": turn round", [](C& c) { c.Move = {}; c.Sprint = false; c.View = 0; },
                             [](C& c) { return c.Time > 1.0f && c.State() == "Idle"; }, 5.0f, [](C& c) { c.Cam->Yaw += 180.0f; }});
        }
    };
#pragma warning(suppress : 4996)
    if (const char* only = std::getenv("STOCK_PROBE_POSE"); only && std::string(only) == "sprint") { // sprinting into the actions alone
        steps.push_back({"settle more", nullptr, hold(1.0f), 5.0f, nullptr});
        sprintActions();
        m_Steps = std::move(steps);
        return;
    }
#pragma warning(suppress : 4996)
    if (const char* only = std::getenv("STOCK_PROBE_POSE"); only && std::string(only) == "switch") { // weapon switches alone, logged
        steps.push_back({"settle more", nullptr, hold(1.0f), 5.0f, nullptr});
        for (int slot : {1, 0}) {
            const std::string name = std::string("switch to ") + (slot ? "Remington" : "AK");
            steps.push_back({name, [this, slot](C& c) { c.View = 1; c.Aim = false; c.Cam->Pitch = 0.0f; m_RegripLog = true; c.P->SelectSlot(slot); },
                             [slot](C& c) {
                                 const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                                 if (f % 6 == 0) c.Shot = "pose_switch" + std::to_string(slot) + "_" + std::to_string(1000 + f).substr(1);
                                 return c.P->Slot() == slot && c.State() == "Idle" && c.Time > 0.3f;
                             }, 8.0f, nullptr});
            steps.push_back({name + ": settles", nullptr, hold(1.5f), 3.0f, [this](C&) { m_RegripLog = false; }});
        }
        m_Steps = std::move(steps);
        return;
    }
#pragma warning(suppress : 4996)
    if (const char* only = std::getenv("STOCK_PROBE_POSE"); only && std::string(only) == "regrip") { // the regrip alone
        steps.push_back({"settle more", nullptr, hold(2.0f), 5.0f, nullptr});
        regrip();
        m_Steps = std::move(steps);
        return;
    }
#pragma warning(suppress : 4996)
    const char* onlyPose = std::getenv("STOCK_PROBE_POSE");
    const bool reloadsOnly = onlyPose && std::string(onlyPose) == "reload"; // the level reloads alone, at the hip and on the sights
    // Each pose held, sampled, then the whole body from three sides.
    auto views = [&](const std::string& stem) {
        static const char* kNames[4] = {"stock", "right", "front_r", "front_l"};
        for (int v = 1; v <= 3; ++v)
            steps.push_back({stem + " view " + kNames[v], [v](C& c) { c.View = v; }, hold(0.1f), 2.0f,
                             [stem, v](C& c) { c.Shot = ShotStem("pose_" + stem + "_" + kNames[v]); }});
    };
    struct Pose { const char* Name; bool Crouch, Aim; float Pitch; glm::vec2 Move; };
    if (!reloadsOnly)
    for (const Pose& ps : {Pose{"stand_hip", false, false, 0.0f, {}}, Pose{"stand_hip_down", false, false, -40.0f, {}}, Pose{"stand_aim", false, true, 0.0f, {}},
                           Pose{"stand_aim_up", false, true, 30.0f, {}}, Pose{"stand_aim_down", false, true, -30.0f, {}},
                           Pose{"crouch_hip", true, false, 0.0f, {}}, Pose{"crouch_aim", true, true, 0.0f, {}},
                           Pose{"crouch_aim_up", true, true, 25.0f, {}}, Pose{"crouch_aim_down", true, true, -30.0f, {}},
                           Pose{"crouch_aim_strafe", true, true, 0.0f, {1.0f, 0.0f}}, Pose{"stand_up", false, false, 0.0f, {}}}) {
        const std::string name = ps.Name;
        steps.push_back({name, [ps](C& c) { c.View = 0; c.Crouch = ps.Crouch; c.Aim = ps.Aim; c.Cam->Pitch = ps.Pitch; c.Move = ps.Move; },
                         [ps](C& c) { return c.Time >= (ps.Move.x != 0.0f ? 0.9f : 1.5f) && (!ps.Aim || c.State() == "Aim"); }, 6.0f,
                         [this, name](C& c) { PrintSample(name); CheckHold(c, name); }});
        views(name);
        steps.push_back({name + " done", [](C& c) { c.View = 0; c.Move = glm::vec2(0.0f); }, hold(0.05f), 2.0f, nullptr});
    }
    steps.push_back({"sights down", [](C& c) { c.Aim = false; c.Crouch = false; c.Cam->Pitch = 0.0f; },
                     [](C& c) { return c.State() == "Idle" && c.Time > 0.5f; }, 3.0f, nullptr});
    // Reloads at the hip and on the sights, looking level, up and down. The world gun keeps its aim; the support hand's
    // trip off it - the pouch, the shells - is held to the body (FirstPersonBody's hand anchor), with the magazine or shell
    // in it. Each recorded frame by frame (CheckReloadRuns), and captured from the front right through the reload.
    m_ReloadRuns.clear();
    m_ReloadRun = -1;
    struct Reload { const char* Name; bool Aim; float Pitch; };
    std::vector<Reload> reloads = {Reload{"reload_hip_level", false, 0.0f}, Reload{"reload_hip_up", false, 40.0f}, Reload{"reload_hip_down", false, -40.0f},
                                   Reload{"reload_ads_level", true, 0.0f}, Reload{"reload_ads_up", true, 40.0f}, Reload{"reload_ads_down", true, -40.0f}};
    if (reloadsOnly) reloads = {reloads[0], reloads[3]};
    for (const Reload& r : reloads) {
        const std::string name = r.Name;
        steps.push_back({name + " round", [r](C& c) { c.View = 0; c.Aim = r.Aim; c.Cam->Pitch = r.Pitch; ++c.Pulls; },
                         [r](C& c) { return c.Time >= 0.5f && c.State() == (r.Aim ? "Aim" : "Idle"); }, 6.0f, nullptr});
        steps.push_back({name, [this, name](C& c) {
                             c.View = 2;
#pragma warning(suppress : 4996)
                             m_RegripLog = std::getenv("STOCK_PROBE_RELOAD_LOG") != nullptr; // the world gun against the shoulder, each frame
                             m_ReloadRuns.push_back({name, {}, {}, {}, {}, {}, {}});
                             m_ReloadRun = (int)m_ReloadRuns.size() - 1;
                             c.P->Reload();
                         },
                         hold(0.05f), 1.0f, nullptr});
        for (int k = 1; k <= 6; ++k)
            steps.push_back({name + " " + std::to_string(k), nullptr, hold(0.25f), 1.0f,
                             [name, k](C& c) { c.Shot = ShotStem("pose_" + name + "_" + std::to_string(k)); }});
        steps.push_back({name + " done", [this](C&) { m_Settle.clear(); m_SettleTrack = true; },
                         [r](C& c) { return c.State() == (r.Aim ? "Aim" : "Idle") && c.Time > 0.3f; }, 10.0f, [this](C&) { m_ReloadRun = -1; }});
        // The end of the reload: from when the clip has its gun back at the hold (the view's butt within 4 mm of where it
        // rests after), the world gun doesn't move - the shoulder lock came back with the clip, not after it.
        steps.push_back({name + " settles", nullptr, hold(1.0f), 2.0f, [this, name, r](C& c) {
                             m_RegripLog = false;
                             m_SettleTrack = false;
                             if (r.Aim || m_Settle.empty()) return;
                             const glm::vec3 rest = m_Settle.back().View;
                             size_t from = m_Settle.size();
                             while (from > 0 && glm::length(m_Settle[from - 1].View - rest) < 0.004f) --from;
                             glm::vec3 lo(1e9f), hi(-1e9f);
                             float pLo = 1e9f, pHi = -1e9f;
                             for (size_t i = from; i < m_Settle.size(); ++i) {
                                 lo = glm::min(lo, m_Settle[i].World);
                                 hi = glm::max(hi, m_Settle[i].World);
                                 pLo = std::min(pLo, m_Settle[i].Pitch);
                                 pHi = std::max(pHi, m_Settle[i].Pitch);
                             }
                             const glm::vec3 span = hi - lo;
                             const float moved = std::max(span.x, std::max(span.y, span.z));
                             char buf[200];
                             std::snprintf(buf, sizeof buf, "%s: back at the hold the world gun stays put (%zu frames: %.1f cm, %.1f deg)", name.c_str(),
                                           m_Settle.size() - from, moved * 100.0f, pHi - pLo);
                             c.Check(m_Settle.size() - from >= 10 && moved < 0.025f && pHi - pLo < 2.0f, buf); // (the lock's last 0.15 s settle overlaps the clip's)
                         }});
        steps.push_back({name + " reset", [](C& c) { c.View = 0; c.Aim = false; c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.State() == "Idle" && c.Time > 0.3f; }, 4.0f, nullptr});
    }
    steps.push_back({"reloads checked", nullptr, hold(0.0f), 1.0f, [this](C& c) { CheckReloadRuns(c); }});
    if (reloadsOnly) {
        m_Steps = std::move(steps);
        return;
    }
    steps.push_back({"settle before the regrip", nullptr, hold(1.5f), 3.0f, nullptr});
    regrip();
    steps.push_back({"settle before sprinting", nullptr, hold(1.0f), 3.0f, nullptr});
    sprintActions();
    m_Steps = std::move(steps);
}

// The jank sweep: every action the gun has, played on the world body from every way into it, filmed circling the body
// (View 10; NPC_TEST_RECORD for the video) while WeaponJankMeter watches the world gun and hands for anything the clips
// don't do themselves.
void FirstPersonWeaponTest::BuildJankProbe() {
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    static const auto resting = [](const std::string& st) { return st == "Idle" || st == "Aim" || st == "Walk" || st == "Sprint"; };
    const bool ak = m_ProbeAk;
    std::vector<Step> steps = {{"in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f, nullptr}};
    if (!ak)
        steps.push_back({"to the Remington", [](C& c) { c.P->SelectSlot(1); c.Cam->Pitch = 0.0f; },
                         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f, nullptr});
    steps.push_back({"settle", [](C& c) { c.View = 10; }, hold(1.5f), 5.0f, nullptr});
    steps.push_back({"meter on", [this](C&) { m_Jank.Reset(); m_JankTrack = true; }, nullptr, 1.0f, nullptr});
    // One segment: `begin` once, `tick` each frame (time-scripted input), done once an action it started has played out
    // and the gun is resting again (or, for a hold, after `min` seconds); then 0.8 s more to watch it settle.
    auto seg = [&](const std::string& name, std::function<void(C&)> begin, bool action, float min = 0.3f,
                   std::function<void(C&)> tick = nullptr) {
        steps.push_back({name, [this, name, begin](C& c) { m_JankSegment = name; c.View = 10; if (begin) begin(c); },
                         [action, min, tick](C& c) {
                             if (tick) tick(c);
                             bool acted = !action;
                             for (const std::string& st : c.States) acted = acted || !resting(st);
                             return c.Time >= min && acted && resting(c.State());
                         }, 14.0f, nullptr});
        steps.push_back({name + " after", nullptr, hold(0.8f), 2.0f, nullptr});
    };
    auto pullsAt = [](std::vector<float> times) {
        return [times](C& c) {
            for (float t : times) if (c.Time >= t && c.Time - c.Dt < t) ++c.Pulls;
        };
    };
    const int half = ak ? 15 : 3;
    seg("idle", nullptr, false, 1.0f);
    seg("look up and down", nullptr, false, 4.0f, [](C& c) {
        const float t = c.Time;
        c.Cam->Pitch = t < 1.0f ? 40.0f * t : t < 3.0f ? 40.0f - 40.0f * (t - 1.0f) : t < 4.0f ? -40.0f + 40.0f * (t - 3.0f) : 0.0f;
    });
    seg("turn on the spot", nullptr, false, 2.0f, [](C& c) { c.Cam->Yaw += (c.Time < 1.0f ? 90.0f : -90.0f) * c.Dt; });
    seg("hip fire", nullptr, false, 1.6f, pullsAt({0.05f, 0.55f, 1.05f}));
    if (ak) seg("hip burst", [](C& c) { c.Trigger = true; }, false, 0.8f, [](C& c) { if (c.Time > 0.6f) c.Trigger = false; });
    seg("tac reload", [half](C& c) { c.P->SetAmmo(half); c.P->Reload(); }, true);
    seg("empty reload", [](C& c) { c.P->SetAmmo(0); c.P->Reload(); }, true);
    if (!ak) seg("reload cut short", [](C& c) { c.P->SetAmmo(1); c.P->Reload(); }, true, 0.3f, pullsAt({1.3f}));
    seg("mag check", [](C& c) { c.P->TriggerAction("MagCheck"); }, true);
    seg("inspect", [](C& c) { c.P->TriggerAction("Inspect"); }, true);
    seg("fidget", [](C& c) { c.P->TriggerAction("Fidget"); }, true);
    seg("melee", [](C& c) { c.P->TriggerAction("Melee"); }, true);
    seg("sights up", [](C& c) { c.Aim = true; }, false, 0.9f);
    seg("sights fire", nullptr, false, 1.2f, pullsAt({0.05f, 0.6f}));
    seg("sights reload", [half](C& c) { c.P->SetAmmo(half); c.P->Reload(); }, true);
    seg("sights mag check", [](C& c) { c.P->TriggerAction("MagCheck"); }, true); // the Remington's drops the sights for it
    seg("sights down", [](C& c) { c.Aim = false; }, false, 0.9f);
    seg("walk", [](C& c) { c.Move = {0.0f, 1.0f}; }, false, 1.2f);
    seg("walk reload", [half](C& c) { c.P->SetAmmo(half); c.P->Reload(); }, true);
    seg("walk inspect", [](C& c) { c.P->TriggerAction("Inspect"); }, true);
    seg("walk sights", [](C& c) { c.Aim = true; }, false, 1.0f);
    seg("walk sights down", [](C& c) { c.Aim = false; }, false, 0.8f);
    seg("stop", [](C& c) { c.Move = {}; }, false, 1.2f);
    seg("turn round", nullptr, false, 1.0f, [](C& c) { c.Cam->Yaw += 180.0f * c.Dt; });
    seg("crouch", [](C& c) { c.Crouch = true; }, false, 1.2f);
    seg("crouch reload", [half](C& c) { c.P->SetAmmo(half); c.P->Reload(); }, true);
    seg("crouch sights", [](C& c) { c.Aim = true; }, false, 0.9f);
    seg("crouch sights down", [](C& c) { c.Aim = false; }, false, 0.8f);
    seg("stand", [](C& c) { c.Crouch = false; }, false, 1.2f);
    seg("sprint", [](C& c) { c.Move = {0.0f, 1.0f}; c.Sprint = true; }, false, 1.6f);
    seg("sprint stop", [](C& c) { c.Move = {}; c.Sprint = false; }, false, 1.4f);
    seg(ak ? "switch to the Remington" : "switch to the AK", [ak](C& c) { c.P->SelectSlot(ak ? 1 : 0); }, true, 0.5f);
    seg(ak ? "switch back to the AK" : "switch back to the Remington", [ak](C& c) { c.P->SelectSlot(ak ? 0 : 1); }, true, 0.5f);
    steps.push_back({"jank", [this](C&) { m_JankTrack = false; }, nullptr, 1.0f, [this](C& c) { PrintJank(c); }});
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::PrintJank(Ctx& c) {
    std::vector<WeaponJankEvent> ev = m_Jank.Events();
    // Worst first, each against its own threshold.
    auto severity = [this](const WeaponJankEvent& e) {
        using K = WeaponJankEvent::Kind;
        switch (e.What) {
        case K::GunPop: return e.Size / (e.Turn ? m_Jank.PopDeg : m_Jank.PopCm);
        case K::HandPop: return e.Size / (1.5f * m_Jank.PopCm);
        case K::Settle: return e.Size / (e.Turn ? m_Jank.SettleMinDeg : m_Jank.SettleMinCm);
        case K::Grip: return e.Size / m_Jank.GripCm;
        }
        return 0.0f;
    };
    std::stable_sort(ev.begin(), ev.end(), [&](const WeaponJankEvent& a, const WeaponJankEvent& b) { return severity(a) > severity(b); });
    std::printf("[Jank] %zu events (worst first; x = times its threshold)\n", ev.size());
    for (const WeaponJankEvent& e : ev) std::printf("[Jank] %5.1fx  %s\n", severity(e), e.Describe().c_str());
    char buf[120];
    std::snprintf(buf, sizeof buf, "jank sweep: nothing flagged on the world gun and hands (%zu events)", ev.size());
    c.Check(ev.empty(), buf);
}

// The reloads against the one at the hip looking level: frame for frame (they start on the press and play the same clips).
void FirstPersonWeaponTest::CheckReloadRuns(Ctx& c) const {
    const ReloadRun* level = nullptr;
    for (const ReloadRun& r : m_ReloadRuns)
        if (r.Name == "reload_hip_level") level = &r;
    if (!level || level->Hand.size() < 10) {
        c.Check(false, "reloads recorded (the hip reload looking level)");
        return;
    }
    auto meanBore = [](const ReloadRun& r) {
        float sum = 0.0f;
        for (float b : r.Bore) sum += b;
        return r.Bore.empty() ? 0.0f : sum / (float)r.Bore.size();
    };
    auto maxStep = [](const ReloadRun& r) {
        float most = 0.0f;
        for (size_t i = 1; i < r.Hand.size(); ++i) most = std::max(most, glm::length(r.Hand[i] - r.Hand[i - 1]));
        return most;
    };
    const float levelStep = maxStep(*level), levelBore = meanBore(*level);
    for (const ReloadRun& r : m_ReloadRuns) {
        const size_t n = std::min(r.Hand.size(), level->Hand.size());
        // Off the gun (both fully anchored), the hand where the level hip reload has it on the body.
        float apart = 0.0f, magOff = 0.0f, worstMag = 0.0f;
        int anchored = 0;
        for (size_t i = 0; i < n; ++i) {
            if (r.Anchor[i] > 0.9f && level->Anchor[i] > 0.9f) {
                apart = std::max(apart, glm::length(r.Hand[i] - level->Hand[i]));
                ++anchored;
            }
            // (Not across a frame where the clip itself pops the shell or magazine - into the port, out of the pouch: the
            // world copy undoes the sights' anchor as it was applied, a frame behind such a jump.)
            const bool pop = i > 0 && (std::abs(level->MagGap[i] - level->MagGap[i - 1]) > 0.1f || std::abs(r.MagGap[i] - r.MagGap[i - 1]) > 0.1f);
            // Only while the level reload has one in the hand, off the gun (a magazine's centre within 20 cm of the hand):
            // one the clip has let go of, or back in the gun, isn't the hand's to carry.
            // And only while this reload's clips hold it too (its grip measured): a frame after they let go - the old magazine
            // at the pouch, the new one seated - the hand moving off it isn't a part leaving the hand.
            const bool held = level->MagGap[i] >= 0.0f && level->MagGap[i] < 0.2f && level->Anchor[i] > 0.5f && i < r.GripErr.size() &&
                              r.GripErr[i] >= 0.0f;
            if (!pop && held && r.MagGap[i] >= 0.0f) magOff = std::max(magOff, r.MagGap[i] - level->MagGap[i]);
            worstMag = std::max(worstMag, r.MagGap[i]);
        }
        const float step = maxStep(r), bore = meanBore(r) - levelBore;
        std::printf("[StockProbe] %s: %zu frames (%d off the gun), hand %.1f cm from the level hip reload's, magazine/shell %.1f cm further "
                    "from the hand (worst %.1f cm), hand step %.1f cm/frame (level %.1f), bore %+.1f deg from the level reload's\n",
                    r.Name.c_str(), r.Hand.size(), anchored, apart * 100.0f, magOff * 100.0f, worstMag * 100.0f, step * 100.0f,
                    levelStep * 100.0f, bore);
        // The spare magazine / shell only in the hand: hidden before and after, shown for part of the reload.
        if (!r.Spare.empty()) {
            int shown = 0;
            for (float g : r.Spare) shown += g >= 0.0f;
            std::printf("[StockProbe] %s: spare magazine / shell shown %d of %zu frames (first %s, last %s)\n", r.Name.c_str(), shown, r.Spare.size(),
                        r.Spare.front() >= 0.0f ? "shown" : "hidden", r.Spare.back() >= 0.0f ? "shown" : "hidden");
            if (c.P->SpareMagazineGap() >= 0.0f) {
                char sb[200];
                std::snprintf(sb, sizeof sb, "%s: the spare magazine / shell only shows in the hand (%d of %zu frames)", r.Name.c_str(), shown, r.Spare.size());
                c.Check(shown > 0 && r.Spare.front() < 0.0f && r.Spare.back() < 0.0f, sb);
            }
        }
        // The part in the hand keeps the first-person clips' grip on it, every frame: it went to the body by its own share
        // of the hand's move, about itself, and slid in the hand (the wrist in the magazine, a shake).
        {
            float worstErr = 0.0f;
            int heldFrames = 0;
            for (float e : r.GripErr) {
                if (e < 0.0f) continue;
                ++heldFrames;
                worstErr = std::max(worstErr, e);
            }
            std::printf("[StockProbe] %s: held part %d frames, grip off by %.1f cm at worst\n", r.Name.c_str(), heldFrames, worstErr * 100.0f);
            if (heldFrames > 0) {
                char gb[200];
                std::snprintf(gb, sizeof gb, "%s: the magazine / shell keeps the hand's grip (%.1f cm off)", r.Name.c_str(), worstErr * 100.0f);
                c.Check(worstErr < 0.015f, gb);
            }
        }
        if (&r == level) continue;
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s: off the gun the left hand reaches the same place on the body (%.1f cm apart)", r.Name.c_str(), apart * 100.0f);
        c.Check(anchored > 0 && apart < 0.08f, buf);
        std::snprintf(buf, sizeof buf, "%s: the magazine / shell stays in the hand (%.1f cm further than the level reload's)", r.Name.c_str(), magOff * 100.0f);
        c.Check(magOff < 0.03f, buf); // the hand and the shell blend onto the body by their own distances off the gun
        std::snprintf(buf, sizeof buf, "%s: the hand never jumps (%.1f cm in a frame, level %.1f)", r.Name.c_str(), step * 100.0f, levelStep * 100.0f);
        c.Check(step < levelStep + 0.04f, buf);
        if (r.Name.find("_up") != std::string::npos || r.Name.find("_down") != std::string::npos) {
            const float want = r.Name.find("_up") != std::string::npos ? 1.0f : -1.0f;
            // Partway: the gun comes kFirstPersonFreeHandLevel of the way back toward level while the hand is off it - not
            // level (the body hold the user rejected), not at the sky either.
            const float keep = 40.0f * (1.0f - kFirstPersonFreeHandLevel);
            std::snprintf(buf, sizeof buf, "%s: the gun follows the view partway (bore %+.1f deg from the level reload's, ~%.0f wanted)", r.Name.c_str(), bore, keep);
            c.Check(bore * want > keep - 8.0f && bore * want < keep + 12.0f, buf);
        }
    }
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
                             std::printf("[Body] %-22s %-60s plants %2d slide mean %.1f max %.1f cm  hands %.1f / world %.1f cm  bodies %.1f cm  eye bump %.1f m/s2\n",
                                         sg.Name.c_str(), seq.c_str(), (int)b.Slides.size(), mean * 100.0f, worst * 100.0f, b.HandGap * 100.0f,
                                         b.TwinHandGap * 100.0f, b.BodyDiff * 100.0f, b.EyeJerk);
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
                             // The view: no bump harder than a smooth 0.3 s crouch takes (the eye drops ~30 cm).
                             std::snprintf(buf, sizeof buf, "%s: the view moves smoothly (%.1f m/s2)", sg.Name.c_str(), b.EyeJerk);
                             // Stairs too: the camera rides them as a ramp (crossed: the climb, for the log).
                             if (b.FeetHigh - b.FeetLow >= 0.05f) std::printf("[Body]   (crossed %.0f cm of steps)\n", (b.FeetHigh - b.FeetLow) * 100.0f);
                             c.Check(b.EyeJerk < 35.0f, buf);
                             std::snprintf(buf, sizeof buf, "%s: the world body's hands on the gun (%.1f cm)", sg.Name.c_str(), b.TwinHandGap * 100.0f);
                             c.Check(b.TwinHandGap < 0.02f, buf);
                             // The world gun stays on the shoulder and out of the head as the body moves.
                             std::snprintf(buf, sizeof buf, "%s: the world butt stays in the shoulder pocket (worst %.1f cm)", sg.Name.c_str(), b.PocketGap * 100.0f);
                             // Crouched, the hunched chest's pocket sits under the upright one the gap is measured to (CheckHold).
                             c.Check(b.PocketGap < (sg.Name.find("crouch") != std::string::npos ? 0.06f : 0.04f), buf);
                             if (b.HeadMeshGap < 1e8f) {
                                 std::snprintf(buf, sizeof buf, "%s: the world gun clears the head (nearest %.1f cm)", sg.Name.c_str(), b.HeadMeshGap * 100.0f);
                                 c.Check(b.HeadMeshGap >= 0.01f, buf);
                             }
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
    // Crouching and standing on the move: no transition clip, a crossfade the camera rides.
    add({"jog_to_crouch_run", move({0, 1}), 1.2f, {"Locomotion"}, {}});
    add({"crouch_on_the_move", move({0, 1}, false, true), 1.5f, {"CrouchLoco"}, {{0.15f, 1}}});
    add({"stand_on_the_move", move({0, 1}), 1.5f, {"Locomotion"}, {{0.15f, 1}}});
    add({"on_the_move_stop", move({}), 1.6f, {"Stop"}, {}});
    // Stairs (the Movement Course's: ten 20 cm steps up -X to the Deck): up, down, walking, sprinting. The camera must
    // ride them as a ramp. The player is put at their foot, and back where it was after.
    if (m_BodyOnly.empty() || m_BodyOnly.rfind("stairs", 0) == 0) {
        auto home = std::make_shared<std::pair<glm::vec3, float>>();
        auto toFoot = [](C& c) {
            const float f[3] = {-12.4f, 0.02f, -6.0f};
            PhysicsWorld::SetCharacterFootPosition(f);
            c.Move = {};
            c.Sprint = c.Crouch = false;
            c.Cam->Yaw = 180.0f; // facing -X, up the stairs
            c.Cam->Pitch = 0.0f;
        };
        steps.push_back({"stairs: to the foot", [home, toFoot](C& c) {
                             float f[3];
                             PhysicsWorld::GetCharacterFootPosition(f);
                             *home = {glm::vec3(f[0], f[1], f[2]), c.Cam->Yaw};
                             toFoot(c);
                         }, hold(1.5f), 5, nullptr});
        add({"stairs_up", move({0, 1}), 2.2f, {"Locomotion"}, {{0.9f, 1}, {1.2f, 9}}});
        add({"stairs_top_stop", move({}), 1.6f, {"Stop"}, {}});
        steps.push_back({"stairs: face down", [](C& c) { c.Cam->Yaw = 0.0f; }, hold(2.5f), 6, nullptr});
        add({"stairs_down", move({0, 1}), 2.2f, {"Locomotion"}, {{0.9f, 1}, {1.2f, 9}}});
        add({"stairs_bottom_stop", move({}), 1.6f, {"Stop"}, {}});
        steps.push_back({"stairs: to the foot again", toFoot, hold(1.5f), 5, nullptr});
        add({"stairs_walk_up", move({0, 0.45f}), 3.6f, {"Locomotion"}, {{1.8f, 1}, {2.2f, 9}}});
        add({"stairs_walk_stop", move({}), 1.8f, {"StopWalk"}, {}});
        steps.push_back({"stairs: to the foot to sprint", toFoot, hold(1.5f), 5, nullptr});
        add({"stairs_sprint_up", move({0, 1}, true), 1.5f, {"Sprint"}, {{0.7f, 1}, {0.9f, 9}}});
        add({"stairs_sprint_stop", move({}), 2.0f, {"StopRun"}, {}});
        steps.push_back({"stairs: back", [home](C& c) {
                             const float f[3] = {home->first.x, home->first.y + 0.02f, home->first.z};
                             PhysicsWorld::SetCharacterFootPosition(f);
                             c.Cam->Yaw = home->second;
                             c.Move = {};
                         }, hold(1.5f), 5, nullptr});
    }
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
                         [this, name, pitch](C& c) { PrintSample(name); CheckHold(c, name); c.Shot = ShotStem("idle_pitch_" + std::to_string((int)pitch)); }});
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
                         [this, name, pitch](C& c) { PrintSample(name); CheckHold(c, name); c.Shot = ShotStem("aim_p" + std::to_string((int)pitch)); }});
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
            // Each ball's height over the ground under it (on stairs the feet stand on different steps): down within
            // 2 cm of the lower one's is down.
            float over[2];
            for (int f = 0; f < 2; ++f) {
                const float origin[3] = {foot[f].x, foot[f].y + 0.3f, foot[f].z};
                const float down[3] = {0.0f, -1.0f, 0.0f};
                QueryFilter filter;
                filter.HitTriggers = 0;
                RaycastHit hit;
                over[f] = PhysicsWorld::RaycastSolid(origin, down, 1.0f, filter, hit) && hit.Hit ? foot[f].y - hit.Point[1] : foot[f].y - body.Feet().y;
            }
            const float ground = std::min(over[0], over[1]);
            for (int f = 0; f < 2; ++f) {
                const glm::vec2 xz(foot[f].x, foot[f].z);
                const float speed = b.Have && m_Ctx.Dt > 0.0f ? glm::length(xz - glm::vec2(b.LastFoot[f].x, b.LastFoot[f].z)) / m_Ctx.Dt : 1e9f;
                const bool planted = over[f] < ground + 0.02f && speed < 0.5f;
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
    if (m_BodyProbe && m_BodyMeasure && m_Ctx.Dt > 0.0f) {
        // The camera's height in the world: what the player feels - the clips' bob, the hips' drop and the body's stair
        // easing (a steady climb is no acceleration; each step's kink is).
        BodyStats& b = m_Body;
        const float y = cam.Position.y;
        b.FeetLow = std::min(b.FeetLow, body.Feet().y);
        b.FeetHigh = std::max(b.FeetHigh, body.Feet().y);
        if (b.EyeFrames >= 2) b.EyeJerk = std::max(b.EyeJerk, std::abs(y - 2.0f * b.EyeY[0] + b.EyeY[1]) / (m_Ctx.Dt * m_Ctx.Dt));
        b.EyeY[1] = b.EyeY[0];
        b.EyeY[0] = y;
        b.EyeFrames = std::min(b.EyeFrames + 1, 2);
        // STOCK_PROBE_EYE=1: the camera's height (over the feet, and in the world) each frame of a segment, and the body's state.
#pragma warning(suppress : 4996)
        static const bool eyeLog = std::getenv("STOCK_PROBE_EYE") != nullptr;
        if (eyeLog)
            std::printf("[Eye] t %.3f y %.4f world %.4f feet %.4f step %.4f at %.2f %.2f state %s\n", m_Ctx.Time, y, cam.Position.y, body.Feet().y, BodyDebug::Info().StepOffset, cam.Position.x, cam.Position.z,
                        BodyDebug::Info().AnimatorState.c_str());
    }
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
        if (glm::vec3 pelvis; body.BoneWorld(world, "pelvis", pelvis) && glm::length(neck - pelvis) > 1e-4f)
            s.Lean = glm::degrees(std::acos(std::clamp(glm::normalize(neck - pelvis).y, -1.0f, 1.0f)));
        // The world hands against where the hold put them on the world gun.
        for (int h = 0; h < 2; ++h) s.HandGap[h] = body.TwinHandGap(h);
        s.GunShift = glm::length(body.WorldGunShift());
        s.HeadTilt = body.WorldHeadTilt();
        s.HeadTiltWeight = body.WorldHeadTiltWeight();
        // The shoulder lock: the butt against the pocket it seats it in (FirstPersonGunSeat's frame).
        s.PocketGap = -1.0f;
        s.Shouldered = 0.0f;
        if (FirstPersonWorldGunInput gi; p.WorldGunInput(gi)) {
            if (glm::vec3 ul; body.BoneWorld(world, "upperarm_l", ul)) {
                glm::vec3 across = upper - ul;
                across.y = 0.0f;
                const glm::vec3 r = glm::length(across) > 1e-4f ? glm::normalize(across) : right;
                const glm::vec3 f = glm::normalize(glm::cross(up, r));
                s.PocketGap = glm::length(butt - (upper + r * gi.Pocket.x + up * gi.Pocket.y + f * gi.Pocket.z));
                s.Shouldered = gi.Shouldered;
            }
        }
        s.BorePitch = glm::degrees(std::asin(std::clamp(fwd.y, -1.0f, 1.0f)));
        if (glm::vec3 ul, hl; body.BoneWorld(world, "upperarm_l", ul) && body.BoneWorld(world, "hand_l", hl)) {
            glm::vec3 across = upper - ul;
            across.y = 0.0f;
            if (glm::length(across) > 1e-4f) {
                const glm::vec3 r = glm::normalize(across), f = glm::normalize(glm::cross(up, r)), toHand = hl - upper;
                s.HandFromShoulder = glm::vec3(glm::dot(toHand, r), toHand.y, glm::dot(toHand, f));
            }
        }
        if (glm::vec3 ul, ll, hl; body.BoneWorld(world, "upperarm_l", ul) && body.BoneWorld(world, "lowerarm_l", ll) &&
                                  body.BoneWorld(world, "hand_l", hl) && glm::length(ll - ul) > 1e-4f && glm::length(hl - ll) > 1e-4f)
            s.SupportBend = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(ll - ul), glm::normalize(hl - ll)), -1.0f, 1.0f)));
        if (m_SettleTrack) {
            glm::vec3 b1, f1;
            if (p.StockWorld(b1, f1)) {
                const glm::vec3 fromEye = b1 - cam.Position;
                m_Settle.push_back({glm::vec3(glm::dot(fromEye, cam.Right()), glm::dot(fromEye, cam.Up()), glm::dot(fromEye, cam.Front())),
                                    s.StockFromShoulder, s.BorePitch});
            }
        }
        if (m_RegripLog) {
            m_RegripMin = glm::min(m_RegripMin, s.StockFromShoulder);
            m_RegripMax = glm::max(m_RegripMax, s.StockFromShoulder);
            // The first-person gun in the camera's frame and the world gun in the body's (right, up, forward), and the
            // third-person clips' socket difference: what the world gun does through the regrip, beside what the view's does.
            glm::vec3 b1, f1;
            if (p.StockWorld(b1, f1)) {
                const glm::vec3 fromEye = b1 - cam.Position;
                const glm::vec3 inCam(glm::dot(fromEye, cam.Right()), glm::dot(fromEye, cam.Up()), glm::dot(fromEye, cam.Front()));
                const float pitch1 = glm::degrees(std::asin(std::clamp(glm::dot(f1, cam.Up()), -1.0f, 1.0f)));
                const float yaw1 = glm::degrees(std::atan2(glm::dot(f1, cam.Right()), glm::dot(f1, cam.Front())));
                const float yawW = glm::degrees(std::atan2(glm::dot(fwd, right), glm::dot(fwd, front)));
                const glm::mat4 corr = p.ThirdPersonGunCorrection();
                const float corrDeg = glm::degrees(2.0f * std::acos(std::clamp(std::abs(IK::Rotation(corr).w), 0.0f, 1.0f)));
                std::printf("[Regrip] t %.3f %-8s 1P butt %+5.1f %+5.1f %+5.1f pitch %+5.1f yaw %+5.1f | world butt-shoulder %+5.1f %+5.1f %+5.1f "
                            "pitch %+5.1f yaw %+5.1f | 3P corr %4.1f cm %4.1f deg | hands off L %.1f R %.1f cm | shouldered %.2f\n",
                            m_Ctx.Time, s.State.c_str(), inCam.x * 100.0f, inCam.y * 100.0f, inCam.z * 100.0f, pitch1, yaw1,
                            s.StockFromShoulder.x * 100.0f, s.StockFromShoulder.y * 100.0f, s.StockFromShoulder.z * 100.0f, s.BorePitch, yawW,
                            glm::length(glm::vec3(corr[3])) * 100.0f, corrDeg, s.HandGap[0] * 100.0f, s.HandGap[1] * 100.0f, s.Shouldered);
            }
        }
        if (m_ReloadRun >= 0 && m_ReloadRun < (int)m_ReloadRuns.size()) {
            ReloadRun& run = m_ReloadRuns[m_ReloadRun];
            // The hand on the chest: right across the upper arms, up the chest line (spine_05 to the neck), forward.
            glm::vec3 onChest(0.0f);
            if (glm::vec3 ul, hl, chest; body.BoneWorld(world, "upperarm_l", ul) && body.BoneWorld(world, "hand_l", hl) &&
                                         body.BoneWorld(world, "spine_05", chest)) {
                const glm::vec3 r = glm::normalize(upper - ul);
                const glm::vec3 u = glm::normalize((neck - chest) - r * glm::dot(neck - chest, r));
                const glm::vec3 f = glm::cross(u, r), toHand = hl - chest;
                onChest = glm::vec3(glm::dot(toHand, r), glm::dot(toHand, u), glm::dot(toHand, f));
            }
            run.Hand.push_back(onChest);
            run.Bore.push_back(s.BorePitch);
            run.Anchor.push_back(body.TwinAnchorWeight());
            float gap = -1.0f;
            thread_local std::vector<glm::vec3> carried;
            if (glm::vec3 hl; body.BoneWorld(world, "hand_l", hl) && p.WorldCarriedBones(world, carried))
                for (const glm::vec3& b : carried) gap = gap < 0.0f ? glm::length(b - hl) : std::min(gap, glm::length(b - hl));
            run.MagGap.push_back(gap);
            run.Spare.push_back(p.SpareMagazineShown() ? p.SpareMagazineGap() : -1.0f);
            // The held part against the world hand: the first-person clips' grip kept (its distance), and steady.
            float gripErr = -1.0f, clipGap = -1.0f, mostHeld = 0.0f;
            glm::vec3 gripOffset(0.0f);
            thread_local std::vector<float> gripHeld;
            if (glm::vec3 hl; p.CarriedHeld(gripHeld, &clipGap) && body.BoneWorld(world, "hand_l", hl) && p.WorldCarriedBones(world, carried) &&
                              carried.size() == gripHeld.size()) {
                const size_t k = (size_t)(std::max_element(gripHeld.begin(), gripHeld.end()) - gripHeld.begin());
                mostHeld = gripHeld[k];
                if (mostHeld > 0.95f) {
                    gripOffset = carried[k] - hl;
                    gripErr = std::abs(glm::length(gripOffset) - clipGap);
                }
            }
            run.GripErr.push_back(gripErr);
#pragma warning(suppress : 4996)
            static const bool reloadLog = std::getenv("STOCK_PROBE_RELOAD_LOG") != nullptr;
            if (reloadLog)
                std::printf("[Reload] %s %3zu anchor %.2f mag %.1f cm hand %+.1f %+.1f %+.1f cm spare %.1f cm %s | held %.2f "
                            "clip gap %.1f cm err %.1f cm offset %+.1f %+.1f %+.1f | hand off target %.1f cm | state %s\n",
                            run.Name.c_str(), run.Hand.size() - 1, run.Anchor.back(), gap * 100.0f, onChest.x * 100.0f, onChest.y * 100.0f,
                            onChest.z * 100.0f, p.SpareMagazineGap() * 100.0f, p.SpareMagazineShown() ? "shown" : "hidden", mostHeld,
                            clipGap * 100.0f, gripErr * 100.0f, gripOffset.x * 100.0f, gripOffset.y * 100.0f, gripOffset.z * 100.0f, s.HandGap[0] * 100.0f, s.State.c_str());
            // The left hand on the gun: the 3P clips' (in their gun's frame) beside the world body's (in the world gun's).
            if (reloadLog)
                if (const Model* clips = p.ThirdPersonArms(); clips && world.Registry.valid(p.WorldWeaponEntity())) {
                    glm::mat4 sock(1.0f), ch(1.0f);
                    glm::vec3 hl;
                    if (clips->NodeTransform(p.GunSocket(), sock) && clips->NodeTransform(p.Set().Procedural.IK.LeftHand, ch) &&
                        body.BoneWorld(world, "hand_l", hl)) {
                        auto rigid = [](const glm::mat4& m) {
                            glm::mat4 r(1.0f);
                            for (int k = 0; k < 3; ++k) r[k] = glm::vec4(glm::normalize(glm::vec3(m[k])), 0.0f);
                            r[3] = m[3];
                            return r;
                        };
                        const glm::mat4 clipGun = rigid(p.ArmsWorld() * sock), gunW = rigid(world.ComposeWorldTransform(p.WorldWeaponEntity()));
                        const glm::vec3 c = glm::vec3(glm::inverse(clipGun) * p.ArmsWorld() * ch[3]) * 100.0f;
                        const glm::vec3 w = glm::vec3(glm::inverse(gunW) * glm::vec4(hl, 1.0f)) * 100.0f;
                        std::printf("[ReloadHand] %s %3zu clip %+6.1f %+6.1f %+6.1f world %+6.1f %+6.1f %+6.1f anchor %.2f\n", run.Name.c_str(),
                                    run.Hand.size() - 1, c.x, c.y, c.z, w.x, w.y, w.z, run.Anchor.back());
                    }
                }
        }
        if (m_BodyProbe && m_BodyMeasure) {
            // Only while the chest faces the aim: mid-turn (the view ahead of the body) the gun lies across the chest, held
            // off the neck, as it should - the turn's own torso catches up.
            bool facing = true;
            if (glm::vec3 ul; body.BoneWorld(world, "upperarm_l", ul)) {
                glm::vec3 across = upper - ul;
                across.y = 0.0f;
                if (glm::length(across) > 1e-4f) facing = glm::dot(glm::normalize(glm::cross(up, glm::normalize(across))), front) > std::cos(glm::radians(25.0f));
            }
            // (Not a segment's first 0.1 s: the script snaps the view there, and the lock takes a frame or two to follow.)
            if (facing && m_Ctx.Time > 0.1f && s.Shouldered > 0.9f && s.PocketGap >= 0.0f) m_Body.PocketGap = std::max(m_Body.PocketGap, s.PocketGap);
            // STOCK_PROBE_POCKET=1: the shoulder lock each frame of a segment.
#pragma warning(suppress : 4996)
            static const bool pocketLog = std::getenv("STOCK_PROBE_POCKET") != nullptr;
            if (pocketLog)
                std::printf("[Pocket] t %.3f facing %d gap %.1f cm shouldered %.2f shift %.1f cm butt R %+.1f U %+.1f F %+.1f state %s\n", m_Ctx.Time, (int)facing,
                            s.PocketGap * 100.0f, s.Shouldered, s.GunShift * 100.0f, s.StockFromShoulder.x * 100.0f, s.StockFromShoulder.y * 100.0f,
                            s.StockFromShoulder.z * 100.0f, BodyDebug::Info().AnimatorState.c_str());
            if (s.MeshGap >= 0.0f) m_Body.HeadMeshGap = std::min(m_Body.HeadMeshGap, s.MeshGap);
        }
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
            case 10: { // swinging round the front, from the left side (-60) to behind the right shoulder (110) and back every 10 s
                m_OrbitDeg = std::fmod(m_OrbitDeg + 36.0f * m_Ctx.Dt, 360.0f);
                const float a = glm::radians(25.0f + 85.0f * std::sin(glm::radians(m_OrbitDeg)));
                dir = glm::normalize(front * std::cos(a) + right * std::sin(a));
                target = glm::vec3(neck.x, neck.y - 0.3f, neck.z); // close on the gun and the hands
                break;
            }
            default: break;
            }
            m_SceneCamPos = target + dir * (m_Ctx.View == 10 ? 1.5f : 2.4f) + up * (m_Ctx.View == 10 ? 0.25f : 0.15f);
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
    if (m_JankTrack) {
        const WeaponJankFrame f = WeaponJankFrameFor(world, body, p, m_Ctx.Dt, m_JankSegment);
        m_Jank.Push(f);
#pragma warning(suppress : 4996)
        static const bool jankLog = std::getenv("STOCK_PROBE_JANK_LOG") != nullptr;
        if (jankLog && f.Valid) {
            const glm::vec3 e = glm::degrees(glm::eulerAngles(f.GunRot));
            FirstPersonWorldGunInput gi;
            const float lock = p.WorldGunInput(gi) ? gi.Shouldered : 0.0f;
            std::printf("[JankLog] t %7.3f %-34s gun %+6.1f %+6.1f %+6.1f cm rot %+6.1f %+6.1f %+6.1f | handL %+6.1f %+6.1f %+6.1f R %+6.1f %+6.1f %+6.1f | "
                        "anchor %.2f lock %.2f gap %.1f %.1f\n",
                        m_Jank.Time(), f.State.c_str(), f.GunPos.x * 100.0f, f.GunPos.y * 100.0f, f.GunPos.z * 100.0f, e.x, e.y, e.z,
                        f.Hand[0].x * 100.0f, f.Hand[0].y * 100.0f, f.Hand[0].z * 100.0f, f.Hand[1].x * 100.0f, f.Hand[1].y * 100.0f,
                        f.Hand[1].z * 100.0f, body.TwinAnchorWeight(), lock, f.HandGap[0] * 100.0f, f.HandGap[1] * 100.0f);
        }
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
                "speed %4.1f eyeAcc %5.1f at (%.1f, %.1f) | gun %3.0fcm to neck %4.1f hood %4.1f mesh %5.1f at %3.0fcm (%s) | elbow to torso L %5.1f R %5.1f cm, swung L %+4.0f R %+4.0f deg | rear 45cm to torso %5.1f at %3.0fcm | head tilt %4.1f (w %.2f) bend %4.1f lean %4.1f deg | eye-shoulder R %+5.1f U %+5.1f F %+5.1f roll %5.1f | pocket %5.1f cm sh %.2f | support elbow bent %3.0f deg\n",
                label.c_str(), s.Pitch, s.YawRate, s.TwistDeg, s.State.c_str(), s.StockFromShoulder.x * 100.0f,
                s.StockFromShoulder.y * 100.0f, s.StockFromShoulder.z * 100.0f, s.Shoulder * 100.0f, s.Clavicle * 100.0f,
                s.Neck * 100.0f, s.Head * 100.0f, s.NeckGap * 100.0f, s.HeadGap * 100.0f, s.GunShift * 100.0f,
                s.HandGap[0] * 100.0f, s.HandGap[1] * 100.0f, s.Speed, s.EyeAccel, s.Eye.x, s.Eye.z, s.GunLength * 100.0f, s.WholeNeckGap * 100.0f,
                s.WholeHoodGap * 100.0f, s.MeshGap * 100.0f, s.MeshAlong * 100.0f, s.MeshPiece.c_str(), s.ElbowGap[0] * 100.0f,
                s.ElbowGap[1] * 100.0f, s.ElbowSwing[0], s.ElbowSwing[1], s.TorsoGap * 100.0f, s.TorsoAlong * 100.0f, s.HeadTilt, s.HeadTiltWeight,
                s.HeadBend, s.Lean, s.EyeFromShoulder.x * 100.0f, s.EyeFromShoulder.y * 100.0f, s.EyeFromShoulder.z * 100.0f, s.Roll, s.PocketGap * 100.0f, s.Shouldered, s.SupportBend);
    m_EyeAccelMax = 0.0f;
    std::fflush(stdout);
}


void FirstPersonWeaponTest::CheckHold(Ctx& c, const std::string& label) const {
    const Sample& s = m_Sample;
    if (!s.Valid) return;
    char buf[160];
    if (s.Shouldered > 0.9f && s.PocketGap >= 0.0f) {
        std::snprintf(buf, sizeof buf, "%s: the world butt in the shoulder pocket (%.1f cm off)", label.c_str(), s.PocketGap * 100.0f);
        // Crouched too, now the world body straightens over the gun on the sights (kFirstPersonCrouchAimUpright).
        c.Check(s.PocketGap < 0.03f, buf);
    }
    if (s.MeshGap >= 0.0f) {
        std::snprintf(buf, sizeof buf, "%s: the world gun clears the head (%.1f cm)", label.c_str(), s.MeshGap * 100.0f);
        c.Check(s.MeshGap >= 0.01f, buf);
    }
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
