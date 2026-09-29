#include "FirstPersonWeaponTest.h"

#include "Camera.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "World.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
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

FirstPersonWeaponTest::FirstPersonWeaponTest(bool stockProbe) : m_Probe(stockProbe) {
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
    if (m_Probe) BuildProbe();
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

void FirstPersonWeaponTest::BuildProbe() {
    using C = Ctx;
    auto hold = [](float s) { return [s](C& c) { return c.Time >= s; }; };
    std::vector<Step> steps = {
        {"AK in hand at Play", nullptr, [](C& c) { return c.State() == "Idle"; }, 30.0f, nullptr},
        {"3: switch to the Remington", [](C& c) { c.P->SelectSlot(1); c.Cam->Pitch = 0.0f; },
         [](C& c) { return c.P->Slot() == 1 && c.State() == "Idle"; }, 180.0f, nullptr},
        {"settle", nullptr, hold(2.0f), 5.0f, nullptr},
    };
    // Held still at each pitch: the settled pose, and a capture.
    for (float pitch : {-75.0f, -60.0f, -45.0f, -30.0f, -15.0f, 0.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
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
    // A round and its pump at three pitches, sampled through, captured along the way.
    for (float pitch : {0.0f, -45.0f, 30.0f}) {
        const std::string name = "fire+pump pitch " + std::to_string((int)pitch);
        steps.push_back({"to pitch " + std::to_string((int)pitch), [pitch](C& c) { c.Cam->Pitch = pitch; }, hold(1.0f), 5.0f, nullptr});
        steps.push_back({name, [name](C& c) { c.LogEvery = 6; c.Label = name; ++c.Pulls; },
                         [pitch](C& c) {
                             const int f = (int)std::lround(c.Time / std::max(c.Dt, 1e-4f));
                             if (f % 12 == 0) c.Shot = ShotStem("fire_p" + std::to_string((int)pitch) + "_" + std::to_string(1000 + f).substr(1));
                             return c.Saw("Pump") && c.State() == "Idle" && c.P->Chambered();
                         }, 6.0f, [](C& c) { c.LogEvery = 0; }});
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
    m_Steps = std::move(steps);
}

void FirstPersonWeaponTest::AfterPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p) {
    if (!m_Probe || !m_Ctx.Cam) return;
    ++m_Frame;
    const Camera& cam = *m_Ctx.Cam;
    Sample s;
    s.Pitch = cam.Pitch;
    s.Yaw = cam.Yaw;
    if (m_HaveYaw && m_Ctx.Dt > 0.0f) s.YawRate = std::remainder(cam.Yaw - m_LastYaw, 360.0f) / m_Ctx.Dt;
    m_LastYaw = cam.Yaw;
    m_HaveYaw = true;
    s.TwistDeg = glm::degrees(body.Twist());
    s.State = p.CurrentState();
    glm::vec3 butt, fwd, upper, clav, neck, head;
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
        s.Shoulder = glm::length(d);
        s.Clavicle = glm::length(butt - clav);
        s.Neck = glm::length(butt - neck);
        s.Head = glm::length(butt - head);
        const glm::vec3 rear = butt + fwd * 0.30f;
        s.NeckGap = SegmentDistance(neck, butt, rear);
        s.HeadGap = SegmentDistance(head, butt, rear);
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
        // The Scene camera: in front of the body and to its right, on the gun's rear and the shoulder.
        const glm::vec3 target = 0.5f * (butt + 0.5f * (neck + upper)) + fwd * 0.08f;
        m_SceneCamPos = target + front * 1.05f + right * 0.75f + up * 0.12f;
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
                "clav %4.1f neck %4.1f head %4.1f | rear 30cm to neck %4.1f head %4.1f | shift %4.1f hands off L %4.1f R %4.1f cm\n",
                label.c_str(), s.Pitch, s.YawRate, s.TwistDeg, s.State.c_str(), s.StockFromShoulder.x * 100.0f,
                s.StockFromShoulder.y * 100.0f, s.StockFromShoulder.z * 100.0f, s.Shoulder * 100.0f, s.Clavicle * 100.0f,
                s.Neck * 100.0f, s.Head * 100.0f, s.NeckGap * 100.0f, s.HeadGap * 100.0f, s.GunShift * 100.0f,
                s.HandGap[0] * 100.0f, s.HandGap[1] * 100.0f);
    std::fflush(stdout);
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
    if (!c.Shot.empty()) {
        m_Shot = c.Shot;
        c.Shot.clear();
    }
    if (glm::vec3 v; !m_Probe && p.CurrentState().rfind("Reload", 0) == 0 && p.ArmsNodeInView("hand_l", v)) {
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
