#include "FirstPersonWeaponTest.h"

#include "FirstPersonPresentation.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>

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

FirstPersonWeaponTest::FirstPersonWeaponTest() {
    // Shared between steps.
    struct Mem { int AkAmmo = 0, ShotgunAmmo = 0, Ammo = 0, Stage = 0; bool Held = false; float HipHand = 0.0f; };
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
         [=](C& c) { ammoIs(c, kAkMagazine - 1); check(c, c.Hits.size() == 1, std::to_string(c.Hits.size()) + " hit(s) (want 1)"); }},
        {"AK reload", [](C& c) { c.P->Reload(); }, [](C& c) { return c.Saw("TacReload") && c.State() == "Idle"; }, 8.0f,
         [=](C& c) { ammoIs(c, kAkMagazine); }},
        {"AK full auto (0.5 s held)",
         [=](C& c) { c.P->ToggleFireMode(); mem->Held = true; },
         [=](C& c) { if (c.Time > 0.5f) mem->Held = false; return c.Time > 1.0f; }, 3.0f,
         [=](C& c) {
             check(c, c.P->IsFullAuto(), "full auto on");
             const int spent = kAkMagazine - c.P->Ammo();
             check(c, spent >= 4 && spent <= 8, std::to_string(spent) + " rounds in 0.5 s (700 rpm: ~6)");
             c.P->ToggleFireMode();
         }},
        {"AK ADS round", [](C& c) { c.Aim = true; }, [](C& c) { return c.State() == "Aim" && c.Time > 0.6f; }, 3.0f,
         [=](C& c) { mem->Ammo = c.P->Ammo(); }},
        {"AK ADS round fires", fire, [](C& c) { return c.Time > 0.4f; }, 2.0f,
         [=](C& c) { ammoIs(c, mem->Ammo - 1); check(c, c.State() == "Aim", "stays on the sights (" + c.State() + ")"); c.Aim = false; mem->AkAmmo = c.P->Ammo(); }},

        {"3: switch to the Remington", [](C& c) { c.P->SelectSlot(1); },
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
        {"3: the Remington kept its shells", [](C& c) { c.P->SelectSlot(1); },
         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 30.0f,
         [=](C& c) { ammoIs(c, mem->ShotgunAmmo); }},
        {"2: unarmed", [](C& c) { c.P->SetEquipped(false); }, [](C& c) { return c.State() == "Holstered"; }, 5.0f,
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
}

void FirstPersonWeaponTest::OnHit(const glm::vec3& point) {
    m_Ctx.Hits.push_back(point);
    glm::vec3 o, d;
    if (m_Ctx.P && m_Ctx.P->MuzzleRay(o, d) && glm::length(point - o) > 1e-4f)
        m_Ctx.HitAngles.push_back(glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(point - o), d), -1.0f, 1.0f))));
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
    if (glm::vec3 v; p.CurrentState().rfind("Reload", 0) == 0 && p.ArmsNodeInView("hand_l", v)) {
        if (c.Hand.size() % 10 == 0 && !Done()) {
            char name[64];
            std::snprintf(name, sizeof name, "%s_%03d", c.Aim ? "ads" : "hip", (int)c.Hand.size());
            m_Shot = name;
        }
        c.Hand.push_back(glm::vec4(v, p.HandAnchorWeight()));
    }
    if (glm::vec3 v; c.Shell.size() < c.Hand.size() && p.WeaponNodeInView("Shell", v)) c.Shell.push_back(v);
    const bool held = m_Held && m_Held();
    p.UpdateTrigger(c.Pulls > 0 || held, c.Pulls > 0 || held);
    c.Pulls = 0;
    std::fflush(stdout);
}
