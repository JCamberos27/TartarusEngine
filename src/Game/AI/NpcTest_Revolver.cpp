// --npc-test revolver: the .38 (WeaponLab slot 3) end to end, headless. See NpcTest.h for the outline.
// Everything runs on the scenario's own clock (m_FireHold): without a squad the director's clock stands still.
#include "NpcTest.h"

#include "BodyDebugDraw.h"
#include "FirstPersonPresentation.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

struct NpcTest::RevolverRun {
    FirstPersonPresentation* Weapon = nullptr;
    int Cam = 0; // 0 shoulder, 1 gun close-up, 2 floor (cases), 3 body from the side, 4/5 chest close (look-around)
    // Fire mechanics, from the weapon's animated bones (view space, last frame's pose).
    bool HaveRest = false;
    glm::vec3 HammerRest{0.0f}, TriggerRest{0.0f};
    float HammerMax = 0.0f, TriggerMax = 0.0f;   // largest travel from rest in the fire windows (m)
    float HammerEnd = 0.0f;                      // travel from rest at the end of the windows (m)
    bool HaveAngle = false;
    glm::vec3 Chamber{1.0f, 0.0f, 0.0f};
    float Unwrapped = 0.0f, HipFrom = 0.0f, HipTurn = 0.0f; // the cylinder over the six hip shots (deg)
    float MinStep = 0.0f, MaxStep = 0.0f, MinStepAt = -1.0f, MaxStepAt = -1.0f; // per-frame turn extremes (deg)
    float WorstBack = 0.0f, WorstBackAt = -1.0f;    // against the shots' own direction (deg, <= 0)
    std::vector<std::pair<float, float>> ShotSpin; // per shot: (time, cylinder turn over the next 0.4 s)
    int LastAmmo = -1;
    float SpinFrom = 0.0f, SpinAt = -1.0f;
    // Ejections by window.
    struct Eject { float At; std::string Model; };
    std::vector<Eject> Ejects;
    // Casings at rest.
    int CasesBefore2 = -1, SleepBefore2 = -1, CasesEnd = -1, SleepEnd = -1;
    // Aim.
    float BoreToView = -1.0f;
    // Slots.
    int AkAmmoLeft = -1, ShotgunAmmoLeft = -1, RevolverBack = -1, AkBack = -1, ShotgunBack = -1, SlotAfterCycle = -1;
    bool Inspected = false, MagChecked = false, Meleed = false;
    std::string InspectState, MagCheckState, MeleeState, SprintState;
    float InspectHammer = 0.0f, InspectSpin = 0.0f;
    std::string States, LastState; // every controller state change: "t:name"
    bool ShotgunInspected = false;
    // The trigger finger: index_03_r's distance from hand_r (the wrist; a distance, so the recoil's turn of the
    // whole hand doesn't count), at rest and its largest change on the hip shots.
    float FingerRest = -1.0f, FingerCurl = 0.0f;
    // The muzzle light: per shot, the brightest within 0.1 s and how far it sat from the muzzle; still lit 0.3 s on.
    int ShotsSeen = 0, ShotsLit = 0;
    float LitPeak = 0.0f, LitFar = 0.0f, LitLate = 0.0f, ShotAt = -1.0f;
    int Tracers = 0; // the most tracer streaks in flight while the player fires
    // Sights-up kick, per weapon (0 revolver, 1 AK): the procedural pose's largest offset after a shot, the IK weight
    // carrying it, the camera's punch, and how far the gun really moved in the view (the right hand, from its spot before the shot).
    // Flip: the bore's largest swing off the view from where it sat before the shot (deg); Climb: the view's own (deg).
    struct Kick { float Pos = 0.0f, Rot = 0.0f, Cam = 0.0f, Moved = 0.0f, MinIK = 1.0f, Flip = 0.0f, Climb = 0.0f, RestBore = 0.0f, RestPitch = 0.0f;
                  bool HaveRest = false; glm::vec3 Rest{0.0f}; };
    Kick AdsKick[3];
    // Shot to shot: the bore's direction in the view 0.1 s after each revolver round, off where it was as the round left
    // (degrees: right, up). The AK's kicks differ shot to shot; the revolver's are to as well.
    struct Spread { float At = -1.0f; glm::vec3 Rest{0.0f}; };
    Spread SpreadNow;
    std::vector<glm::vec2> ShotDirs;
    // The gun on the hand: the weapon's Root from the right hand (view space, mm), its largest change over a window -
    // 0 standing on the sights (8-10 s), 1 walking from the hip (44-46 s), 2 walking on the sights (110.5-115 s).
    // Stretch: shoulder-hand / arm length (most). HandOnBone / GunOnBone: hand_r and the drawn gun's Root each from the arms'
    // own gun bone (ik_hand_gun), most change - which side slides.
    struct Slip { bool Have = false; glm::vec3 First{0.0f}, FirstHand{0.0f}, FirstGun{0.0f}; float Most = 0.0f, Stretch = 0.0f, HandOnBone = 0.0f, GunOnBone = 0.0f; };
    Slip Slips[3]; // 0 revolver on the sights, 1 the AK on the sights, 2 revolver from the hip (the Fire clip)
};

namespace {
float Pulse(float t, float at) { return t >= at && t < at + 0.05f ? 1.0f : 0.0f; }
bool Once(float t, float at, float dt) { return t >= at && t - dt < at; }
} // namespace

void NpcTest::OnPlayerEjection(const std::string& model) {
    if (m_Rev) m_Rev->Ejects.push_back({m_FireHold, model});
}

void NpcTest::RevolverDrive(Player& player, FirstPersonPresentation& weapon, float dt) {
    if (!m_Rev) m_Rev = std::make_shared<RevolverRun>();
    RevolverRun& R = *m_Rev;
    R.Weapon = &weapon;
    m_FireHold += dt;
    const float t = m_FireHold;
    m_Action.clear();
    if (t < 2.0f) { m_PlayerShots = 0; m_LastAmmo = -1; } // the switch's 30 -> 6 is not shots
    if (t >= 0.5f && !m_FlameSwitched && weapon.SlotCount() > 2) {
        weapon.SelectSlot(2);
        m_FlameSwitched = true;
    }
    // --- 0-31 s: sights, fire, a partial reload, six from the hip, a full reload ----------------------
    if ((t >= 6.0f && t < 12.0f) || (t >= 53.0f && t < 55.4f) || (t >= 109.5f && t < 115.5f)) m_Target = "revolver";
    else m_Target.clear(); // sights up
    m_Firing = Pulse(t, 10.5f) > 0.0f || Pulse(t, 11.5f) > 0.0f;
    for (int i = 0; i < 6; ++i) m_Firing = m_Firing || Pulse(t, 20.0f + 0.45f * (float)i) > 0.0f;
    m_Reload = (t >= 12.6f && t < 12.7f) || (t >= 23.4f && t < 23.5f);

    // The hammer, the trigger and the cylinder through the shots (the pose is last frame's).
    const bool onRevolver = weapon.Slot() == 2;
    // Relative to the cylinder's centre, so the recoil and sway of the whole gun don't count (view space).
    glm::vec3 hammer(0.0f), trigger(0.0f), cyl(0.0f), bullet(0.0f), b3(0.0f), b5(0.0f);
    const bool bones = onRevolver && weapon.WeaponNodeInView("Hammer", hammer) && weapon.WeaponNodeInView("Trigger", trigger) &&
                       weapon.WeaponNodeInView("Cylinder", cyl) && weapon.WeaponNodeInView("Bullet1", bullet) &&
                       weapon.WeaponNodeInView("Bullet3", b3) && weapon.WeaponNodeInView("Bullet5", b5);
    hammer -= cyl;
    trigger -= cyl;
    // The hip shots (hipFireClip: they play the Fire clip; the sights-up ones are procedural and move nothing).
    const bool restMoment = t >= 19.7f && t < 19.8f;
    const bool fireWindow = t >= 19.8f && t < 23.3f;
    if (bones && restMoment) R.HipFrom = R.Unwrapped;
    if (bones && t >= 23.3f && R.HipTurn == 0.0f && R.HipFrom != 0.0f) R.HipTurn = R.Unwrapped - R.HipFrom;
    if (bones && restMoment) {
        R.HammerRest = hammer;
        R.TriggerRest = trigger;
        R.HaveRest = true;
        R.HaveAngle = false;
    }
    if (bones) {
        // Chamber 1 about the cylinder's own axis (the normal of chambers 1, 3, 5), in 3D: seen from the hip
        // the cylinder is oblique to the view, so a 2D angle on screen would understate the turn.
        const glm::vec3 axis = glm::normalize(glm::cross(b3 - bullet, b5 - bullet));
        glm::vec3 v = bullet - cyl;
        v = glm::normalize(v - axis * glm::dot(v, axis));
        if (R.HaveAngle) {
            const float raw = glm::degrees(std::atan2(glm::dot(glm::cross(R.Chamber, v), axis), glm::dot(R.Chamber, v)));
            // Modulo one chamber: the six look alike, so a cut by 60 degrees is no turn at all.
            const float step = std::remainder(raw, 60.0f);
            R.Unwrapped += step;
            if (fireWindow && step < R.MinStep) { R.MinStep = step; R.MinStepAt = t; }
            if (fireWindow && step > R.MaxStep) { R.MaxStep = step; R.MaxStepAt = t; }
        }
        R.Chamber = v;
        R.HaveAngle = true;
    }
    if (bones && fireWindow && R.HaveRest) {
        R.HammerMax = std::max(R.HammerMax, glm::length(hammer - R.HammerRest));
        R.TriggerMax = std::max(R.TriggerMax, glm::length(trigger - R.TriggerRest));
        R.HammerEnd = glm::length(hammer - R.HammerRest);
    }
    // Per shot: the cylinder's turn over the 0.4 s after the round left.
    if (onRevolver && R.LastAmmo >= 0 && weapon.Ammo() < R.LastAmmo && fireWindow) { R.SpinFrom = R.Unwrapped; R.SpinAt = t; }
    if (R.SpinAt >= 0.0f && t >= R.SpinAt + 0.4f) { R.ShotSpin.push_back({R.SpinAt, R.Unwrapped - R.SpinFrom}); R.SpinAt = -1.0f; }
    // The trigger finger through the hip shots (it curls onto the trigger and lets it go).
    if (glm::vec3 tip, hand; onRevolver && weapon.ArmsNodeInView("index_03_r", tip) && weapon.ArmsNodeInView("hand_r", hand)) {
        const float d = glm::length(tip - hand);
        if (restMoment) R.FingerRest = d;
        if (fireWindow && R.FingerRest > 0.0f) R.FingerCurl = std::max(R.FingerCurl, std::abs(d - R.FingerRest));
    }
    // The muzzle light on every revolver shot (sights and hip), and the tracers.
    if (onRevolver && R.LastAmmo >= 0 && weapon.Ammo() < R.LastAmmo && t < 31.0f) { ++R.ShotsSeen; R.ShotAt = t; }
    if (glm::vec3 so, sd; onRevolver && t < 31.0f && weapon.MuzzleRay(so, sd)) {
        const glm::vec3 f = player.Cam.Front(), r = player.Cam.Right(), u = player.Cam.Up();
        const glm::vec3 inView(glm::dot(sd, r), glm::dot(sd, u), glm::dot(sd, f));
        if (R.ShotAt == t) { R.SpreadNow.At = t; R.SpreadNow.Rest = inView; }
        else if (R.SpreadNow.At >= 0.0f && t - R.SpreadNow.At >= 0.1f) {
            const auto ang = [](const glm::vec3& v) { return glm::vec2(glm::degrees(std::atan2(v.x, v.z)), glm::degrees(std::atan2(v.y, v.z))); };
            R.ShotDirs.push_back(ang(inView) - ang(R.SpreadNow.Rest));
            R.SpreadNow.At = -1.0f;
        }
    }
    if (MuzzleState) {
        float lit = 0.0f;
        glm::vec3 at(0.0f), fpMuzzle, worldMuzzle, bore;
        int tracers = 0;
        MuzzleState(lit, at, tracers);
        if ((t >= 10.0f && t < 12.6f) || (t >= 19.8f && t < 23.4f)) R.Tracers = std::max(R.Tracers, tracers);
        const bool justFired = R.ShotAt >= 0.0f && t - R.ShotAt < 0.1f;
        if (justFired && lit > 0.0f) {
            R.LitPeak = std::max(R.LitPeak, lit);
            if (weapon.MuzzleFrames(fpMuzzle, worldMuzzle, bore))
                R.LitFar = std::max(R.LitFar, std::min(glm::length(at - fpMuzzle), glm::length(at - worldMuzzle)));
            if (R.ShotsLit < R.ShotsSeen) ++R.ShotsLit;
        }
        if (R.ShotAt >= 0.0f && t - R.ShotAt >= 0.3f && t - R.ShotAt < 0.4f) R.LitLate = std::max(R.LitLate, lit);
    }
    // The kick: the revolver's first shot on the sights (10.5), the AK's (54.0), the revolver's first from the hip (20.0).
    {
        const int k = t >= 10.0f && t < 11.4f ? 0 : t >= 53.6f && t < 54.4f ? 1 : t >= 19.8f && t < 20.45f ? 2 : -1;
        static const float kShotAt[3] = {10.5f, 54.0f, 20.0f};
        glm::vec3 root(0.0f), mo, md;
        if (k >= 0 && weapon.ArmsNodeInView("hand_r", root) && weapon.MuzzleRay(mo, md)) {
            RevolverRun::Kick& K = R.AdsKick[k];
            const float bore = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(md), player.Cam.Front()), -1.0f, 1.0f)));
            const bool before = t < kShotAt[k];
            if (before) { K.Rest = root; K.RestBore = bore; K.RestPitch = player.Cam.Pitch; K.HaveRest = true; }
            else {
                if (K.HaveRest) {
                    K.Flip = std::max(K.Flip, std::abs(bore - K.RestBore));
                    K.Climb = std::max(K.Climb, player.Cam.Pitch - K.RestPitch);
                }
                const WeaponProceduralPose& p = weapon.ProceduralPose();
                K.Pos = std::max(K.Pos, glm::length(p.Position));
                K.Rot = std::max(K.Rot, glm::length(p.Rotation));
                K.Cam = std::max(K.Cam, glm::length(p.CameraKick));
                K.MinIK = std::min(K.MinIK, p.IKWeight);
                if (K.HaveRest) K.Moved = std::max(K.Moved, glm::length(root - K.Rest));
                if (K.HaveRest && t < kShotAt[k] + 0.45f) // the first shot's kick over time
                    std::printf("[NpcTest] revolver: kick trace %s +%.3f s: hand %.1f mm, bore off %.2f deg, view climb %.2f deg\n",
                                k == 0 ? "revolver-ads" : k == 1 ? "AK-ads" : "revolver-hip", t - kShotAt[k],
                                glm::length(root - K.Rest) * 1000.0f, std::abs(bore - K.RestBore), player.Cam.Pitch - K.RestPitch);
            }
        }
    }
    R.LastAmmo = onRevolver ? weapon.Ammo() : -1;
    if (weapon.CurrentState() != R.LastState) {
        R.LastState = weapon.CurrentState();
        char b[64];
        std::snprintf(b, sizeof b, " %.2f:%s", t, R.LastState.c_str());
        R.States += b;
    }

    // Where the bore points against where the eye looks, sights up and still.
    if (Once(t, 9.8f, dt)) {
        glm::vec3 o, d;
        if (weapon.MuzzleRay(o, d)) R.BoreToView = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(d), player.Cam.Front()), -1.0f, 1.0f)));
    }

    // --- 31-51 s: the other clips ----------------------------------------------------------------
    if (Once(t, 31.0f, dt)) { m_Action = "Inspect"; R.HammerRest = hammer; R.SpinFrom = R.Unwrapped; }
    if (t >= 31.0f && t < 38.0f && bones) {
        R.InspectHammer = std::max(R.InspectHammer, glm::length(hammer - R.HammerRest));
        R.InspectSpin = std::max(R.InspectSpin, std::abs(R.Unwrapped - R.SpinFrom));
    }
    if (Once(t, 31.5f, dt)) { R.InspectState = weapon.CurrentState(); R.Inspected = R.InspectState == "Inspect"; }
    if (Once(t, 38.5f, dt)) m_Action = "MagCheck";
    if (Once(t, 39.0f, dt)) { R.MagCheckState = weapon.CurrentState(); R.MagChecked = R.MagCheckState == "MagCheck"; }
    if (Once(t, 42.0f, dt)) m_Action = "Melee";
    if (Once(t, 42.2f, dt)) { R.MeleeState = weapon.CurrentState(); R.Meleed = R.MeleeState == "Melee"; }
    if (t >= 43.5f && t < 49.5f) player.ScriptMove = glm::vec2(0.0f, 1.0f);
    if (t >= 46.5f && t < 49.5f) player.ScriptSprint = true;
    if (Once(t, 48.5f, dt)) R.SprintState = weapon.CurrentState();

    // --- 51-77 s: slots, the AK and the 870 still fire, eject and reload, ammo kept ----------------
    if (Once(t, 51.5f, dt)) weapon.SelectSlot(0);
    for (int i = 0; i < 3; ++i) m_Firing = m_Firing || Pulse(t, 54.0f + 0.4f * (float)i) > 0.0f;
    if (t >= 55.6f && t < 55.7f) m_Reload = true;
    if (Once(t, 59.8f, dt)) R.AkAmmoLeft = weapon.Ammo();
    if (Once(t, 60.0f, dt)) weapon.SelectSlot(1);
    m_Firing = m_Firing || Pulse(t, 62.0f) > 0.0f || Pulse(t, 63.6f) > 0.0f;
    if (Once(t, 65.8f, dt)) R.ShotgunAmmoLeft = weapon.Ammo();
    if (Once(t, 66.0f, dt)) weapon.SelectSlot(2);
    if (Once(t, 68.3f, dt)) R.RevolverBack = weapon.Ammo();
    if (Once(t, 68.5f, dt)) weapon.SelectSlot(0);
    if (Once(t, 70.3f, dt)) R.AkBack = weapon.Ammo();
    if (Once(t, 70.5f, dt)) weapon.SelectSlot(1);
    if (Once(t, 72.8f, dt)) R.ShotgunBack = weapon.Ammo();
    if (Once(t, 73.0f, dt)) weapon.CycleSlot(1); // the scroll wheel, one notch
    if (Once(t, 75.0f, dt)) R.SlotAfterCycle = weapon.Slot();

    // --- 77-103 s: the 3P body while the view looks around, through the inspect, a reload and the mag check -----
    // A sweep left, right, up and down (yaw +-60, pitch +-35), 1.2 s a leg, over each clip.
    m_RevLook = glm::vec2(0.0f);
    if (Once(t, 76.0f, dt)) weapon.SelectSlot(2);
    auto sweep = [&](float from) {
        const float u = t - from;
        static const glm::vec2 legs[] = {{0, 0}, {60, 0}, {-60, 0}, {0, 35}, {0, -35}, {0, 0}};
        if (u < 0.0f || u >= 6.0f) return;
        const int i = std::min((int)(u / 1.2f), 4);
        const float k = std::clamp(u / 1.2f - (float)i, 0.0f, 1.0f);
        m_RevLook = glm::mix(legs[i], legs[i + 1], k * k * (3.0f - 2.0f * k));
    };
    if (Once(t, 78.0f, dt)) m_Action = "Inspect";
    sweep(78.2f);
    if (t >= 86.0f && t < 86.1f) m_Reload = true; // after the Fire clip (a reload asked for during it is dropped)
    if (t >= 84.9f && t < 85.0f) m_Firing = true; // one round out, so the reload runs
    sweep(86.3f);
    if (Once(t, 95.5f, dt)) m_Action = "MagCheck";
    sweep(95.7f);
    sweep(102.0f); // a control: the same sweep in plain Idle
    // --- 109-116 s: walking on the sights (the gun is to stay locked in the hand) -------------------------
    if (t >= 110.5f && t < 115.0f) player.ScriptMove = glm::vec2(0.0f, 1.0f);
    {
        const int w = t >= 8.0f && t < 10.0f ? 0 : t >= 44.0f && t < 46.0f ? 1 : t >= 111.0f && t < 115.0f ? 2 : -1;
        glm::vec3 hand(0.0f), root(0.0f);
        if (w >= 0 && onRevolver && weapon.ArmsNodeInView("hand_r", hand) && weapon.WeaponNodeInView("Root", root)) {
            RevolverRun::Slip& S = R.Slips[w];
            // In the gun's own frame (Root, Cylinder, Trigger), so the gun and hands turning together isn't counted:
            // Most = the right hand's slide on it, HandOnBone = the left's, GunOnBone = the arms' gun bone's.
            glm::vec3 gCyl(0.0f), gTrig(0.0f), handL(0.0f), bone(0.0f);
            if (weapon.WeaponNodeInView("Cylinder", gCyl) && weapon.WeaponNodeInView("Trigger", gTrig) && weapon.ArmsNodeInView("hand_l", handL) &&
                weapon.ArmsNodeInView("ik_hand_gun", bone)) {
                const glm::vec3 x = glm::normalize(gCyl - root), z = glm::normalize(glm::cross(x, gTrig - root)), y = glm::cross(z, x);
                const auto local = [&](const glm::vec3& p) { const glm::vec3 d = p - root; return glm::vec3(glm::dot(d, x), glm::dot(d, y), glm::dot(d, z)); };
                if (!S.Have) { S.First = local(hand); S.FirstHand = local(handL); S.FirstGun = x; S.Have = true; }
                S.Most = std::max(S.Most, glm::length(local(hand) - S.First));
                S.HandOnBone = std::max(S.HandOnBone, glm::length(local(handL) - S.FirstHand));
                S.GunOnBone = std::max(S.GunOnBone, glm::degrees(std::acos(std::clamp(glm::dot(x, S.FirstGun), -1.0f, 1.0f)))); // the gun's tilt in the view (deg)
            }
            if (glm::vec3 up, lo; weapon.ArmsNodeInView("upperarm_r", up) && weapon.ArmsNodeInView("lowerarm_r", lo))
                S.Stretch = std::max(S.Stretch, glm::length(hand - up) / std::max(glm::length(lo - up) + glm::length(hand - lo), 1e-4f));
        }
    }
    // The body through the sweeps, every 0.3 s: its state, how far the view is off its heading, the spine's twist.
    if (t >= 78.0f && std::fmod(t, 0.3f) < dt) {
        const BodyDebug::Snapshot& b = BodyDebug::Info();
        if (b.Valid)
            std::printf("[NpcTest] revolver: body t %.1f look %+.0f/%+.0f arms '%s' body '%s' view off %+.0f twist %+.0f turning %d still %d\n", t,
                        m_RevLook.x, m_RevLook.y, weapon.CurrentState().c_str(), b.AnimatorState.c_str(), b.ViewOffsetDeg, b.TwistDeg,
                        (int)b.Turning, (int)b.Still);
    }

    // The Scene view's framing for the coming shots.
    R.Cam = (t >= 3.2f && t < 3.7f) || (t >= 20.85f && t < 21.05f) || (t >= 66.6f && t < 67.6f) ? 1
          : (t >= 19.3f && t < 19.75f) || (t >= 23.0f && t < 23.35f) || (t >= 30.2f && t < 30.8f) ? 2
          : (t >= 43.5f && t < 51.0f) ? 3
          : (t >= 25.2f && t < 29.6f) ? 5 // the second reload from ahead and to the left: the free hand and both arms
          : t >= 77.0f ? (std::fmod(t, 2.4f) < 1.2f ? 4 : 5) : 0;
}

void NpcTest::RevolverAfter(const Player& player) {
    if (!m_Rev) return;
    RevolverRun& R = *m_Rev;
    const float t = m_FireHold;
    const glm::vec3 eye = player.Cam.Position, front = player.Cam.Front();
    const glm::vec3 flat = glm::normalize(glm::vec3(front.x, 0.0f, front.z));
    const glm::vec3 right = glm::normalize(glm::cross(flat, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 feet = eye - glm::vec3(0.0f, 1.62f, 0.0f);
    glm::vec3 look = eye + front * 0.6f - glm::vec3(0.0f, 0.15f, 0.0f);
    m_CamPos = eye + right * 0.9f + front * 0.5f + glm::vec3(0.0f, 0.1f, 0.0f);
    glm::vec3 fpMuzzle, worldMuzzle, bore;
    if (R.Cam == 1 && R.Weapon && R.Weapon->MuzzleFrames(fpMuzzle, worldMuzzle, bore)) {
        const glm::vec3 gun = worldMuzzle - bore * 0.09f; // about the cylinder
        m_CamPos = gun + right * 0.28f + glm::vec3(0.0f, 0.06f, 0.0f) + flat * 0.05f;
        look = gun;
    } else if (R.Cam == 2) {
        m_CamPos = feet + flat * 1.3f + right * 0.9f + glm::vec3(0.0f, 0.7f, 0.0f);
        look = feet + flat * 0.4f + right * 0.3f;
    } else if (R.Cam == 3) {
        m_CamPos = feet + right * 3.2f + flat * 0.6f + glm::vec3(0.0f, 1.1f, 0.0f);
        look = feet + glm::vec3(0.0f, 1.0f, 0.0f);
    } else if (R.Cam == 4 || R.Cam == 5) {
        // The look-around: close on the chest from ahead and to one side (4 right, 5 left), fixed to the default
        // facing (-Z) rather than the view, so the turning view swings the arms past a still camera.
        const glm::vec3 ahead(0.0f, 0.0f, -1.0f), side(1.0f, 0.0f, 0.0f);
        m_CamPos = feet + ahead * 1.3f + side * (R.Cam == 4 ? 0.8f : -0.8f) + glm::vec3(0.0f, 1.35f, 0.0f);
        look = feet + glm::vec3(0.0f, 1.15f, 0.0f);
    }
    const glm::vec3 d = glm::normalize(look - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    m_HaveCam = true;

    static const struct { float At; const char* Name; } kShots[] = {
        {2.5f, "hip_idle"}, {3.6f, "gun_closeup"}, {7.0f, "ads_enter"}, {9.5f, "ads_steady"}, {10.52f, "ads_fire1_flash"},
        {10.6f, "ads_fire1_hammer"}, {10.8f, "ads_fire1_recoil"}, {11.52f, "ads_fire2_flash"}, {12.4f, "ads_exit"},
        {13.0f, "reload1_crane"}, {13.6f, "reload1_eject"}, {14.2f, "reload1_cases"}, {15.0f, "reload1_insert1"},
        {16.3f, "reload1_insert2"}, {17.6f, "reload1_insert3"}, {18.3f, "reload1_close"}, {19.6f, "reload1_floor"},
        {20.02f, "hip_fire1_flash"}, {20.2f, "hip_fire1_recoil"}, {20.99f, "hip_fire3_trigger_closeup"}, {22.5f, "hip_empty"}, {23.25f, "before_reload2_floor"},
        {24.4f, "reload2_eject"}, {25.0f, "reload2_cases"}, {25.6f, "reload2_front_1"}, {26.6f, "reload2_front_2"}, {27.6f, "reload2_front_3"}, {28.6f, "reload2_front_4"}, {30.7f, "reload2_floor"},
        {31.8f, "inspect_1"}, {32.4f, "inspect_fp_metal"}, {33.0f, "inspect_2"}, {34.2f, "inspect_3"}, {35.4f, "inspect_4"}, {36.6f, "inspect_5"},
        {39.3f, "magcheck_1"}, {40.3f, "magcheck_2"}, {42.25f, "melee_1"}, {42.6f, "melee_2"},
        {45.0f, "walk"}, {47.0f, "idle_to_sprint"}, {48.5f, "sprint"}, {49.8f, "sprint_to_idle"}, {50.8f, "idle_after"},
        {51.7f, "holster"}, {52.8f, "ak_draw"}, {54.05f, "ak_fire"}, {56.5f, "ak_reload"}, {60.4f, "shotgun_draw"},
        {62.05f, "shotgun_fire"}, {62.8f, "shotgun_pump"}, {66.3f, "revolver_draw"}, {67.5f, "revolver_draw_closeup"},
        {75.2f, "after_cycle"},
        // The look-around: each leg's end (yaw right, left, up, down), side view and shoulder view alternately.
        {79.35f, "look_inspect_right"}, {80.55f, "look_inspect_left"}, {81.75f, "look_inspect_up"}, {82.95f, "look_inspect_down"},
        {87.45f, "look_reload_right"}, {88.65f, "look_reload_left"}, {89.85f, "look_reload_up"}, {91.05f, "look_reload_down"},
        {96.85f, "look_magcheck_right"}, {98.05f, "look_magcheck_left"}, {99.25f, "look_magcheck_up"}, {100.45f, "look_magcheck_down"},
        {103.15f, "look_idle_right"}, {104.35f, "look_idle_left"}, {105.55f, "look_idle_up"}, {106.75f, "look_idle_down"},
        {112.0f, "ads_walk_1"}, {112.6f, "ads_walk_2"}, {113.2f, "ads_walk_3"}};
    if (m_ShotIndex < (int)(sizeof kShots / sizeof kShots[0]) && t >= kShots[m_ShotIndex].At) {
        char name[64];
        std::snprintf(name, sizeof name, "revolver_%02d_%s", m_ShotIndex, kShots[m_ShotIndex].Name);
        m_Shot = name;
        ++m_ShotIndex;
    }
    if (t >= 12.55f && m_RevAmmoAtReload1 < 0) m_RevAmmoAtReload1 = m_LastAmmo;
    if (t >= 19.8f && m_RevAmmoAfterReload1 < 0) m_RevAmmoAfterReload1 = m_LastAmmo;
    if (CasingState && t >= 23.3f && R.CasesBefore2 < 0) CasingState(R.CasesBefore2, R.SleepBefore2);
    if (CasingState && t >= 30.9f && R.CasesEnd < 0) CasingState(R.CasesEnd, R.SleepEnd);
    if (t >= 31.0f && m_RevAmmoAtEnd < 0) { m_RevAmmoAtEnd = m_LastAmmo; m_RevShots = m_PlayerShots; }
    if (t < m_Duration || m_Done) return;
    m_Done = true;

    auto ejected = [&](float from, float to, std::string* models) {
        int n = 0;
        for (const auto& e : R.Ejects)
            if (e.At >= from && e.At < to) {
                ++n;
                if (models) *models += (models->empty() ? "" : ", ") + e.Model.substr(e.Model.find_last_of("/\\") + 1);
            }
        return n;
    };
    std::string partialModels, fullModels;
    const int ejFire = ejected(10.0f, 12.6f, nullptr) + ejected(19.8f, 23.4f, nullptr);
    const int ejPartial = ejected(12.6f, 19.8f, &partialModels), ejFull = ejected(23.4f, 31.0f, &fullModels);
    const int ejAk = ejected(53.9f, 59.8f, nullptr), ejShotgun = ejected(61.9f, 65.8f, nullptr);
    const float net = R.HipTurn;
    R.WorstBack = net >= 0.0f ? R.MinStep : -R.MaxStep;
    R.WorstBackAt = net >= 0.0f ? R.MinStepAt : R.MaxStepAt;
    std::string spins;
    for (const auto& s : R.ShotSpin) { char b[32]; std::snprintf(b, sizeof b, "%s%.0f", spins.empty() ? "" : " ", s.second); spins += b; }
    std::printf("[NpcTest] revolver: shots %d, ammo before reload %d, after %d, at end %d\n", m_RevShots, m_RevAmmoAtReload1,
                m_RevAmmoAfterReload1, m_RevAmmoAtEnd);
    std::printf("[NpcTest] revolver: hammer travel %.1f mm (back to %.1f mm), trigger %.1f mm, cylinder per shot [%s] deg, worst backwards step %.1f deg at %.2f s\n",
                R.HammerMax * 1000.0f, R.HammerEnd * 1000.0f, R.TriggerMax * 1000.0f, spins.c_str(), R.WorstBack, R.WorstBackAt);
    std::printf("[NpcTest] revolver: bore vs view on the sights %.2f deg\n", R.BoreToView);
    std::printf("[NpcTest] revolver: ejections fire %d, partial %d [%s], full %d [%s], AK %d, 870 %d\n", ejFire, ejPartial,
                partialModels.c_str(), ejFull, fullModels.c_str(), ejAk, ejShotgun);
    std::printf("[NpcTest] revolver: cases %d (%d asleep) before the 2nd reload, %d (%d asleep) at the end\n", R.CasesBefore2,
                R.SleepBefore2, R.CasesEnd, R.SleepEnd);
    std::printf("[NpcTest] revolver: states inspect '%s', magcheck '%s', melee '%s', sprint '%s'; inspect hammer %.1f mm, cylinder %.0f deg\n",
                R.InspectState.c_str(), R.MagCheckState.c_str(), R.MeleeState.c_str(), R.SprintState.c_str(),
                R.InspectHammer * 1000.0f, R.InspectSpin);
    std::printf("[NpcTest] revolver: ammo AK %d -> %d, 870 %d -> %d, revolver back %d; slot after one scroll %d\n", R.AkAmmoLeft,
                R.AkBack, R.ShotgunAmmoLeft, R.ShotgunBack, R.RevolverBack, R.SlotAfterCycle);
    std::printf("[NpcTest] revolver: states%s\n", R.States.c_str());
    for (int k = 0; k < 3; ++k)
        std::printf("[NpcTest] revolver: kick %s: gun moved %.1f mm in view, bore flipped %.2f deg off the view, view climbed %.2f deg "
                    "(procedural pose %.1f mm / %.2f deg)\n",
                    k == 0 ? "revolver on the sights" : k == 1 ? "AK on the sights" : "revolver from the hip", R.AdsKick[k].Moved * 1000.0f,
                    R.AdsKick[k].Flip, R.AdsKick[k].Climb, R.AdsKick[k].Pos * 1000.0f, R.AdsKick[k].Rot);
    float yawMin = 1e9f, yawMax = -1e9f;
    std::string dirs;
    for (size_t i = 0; i < R.ShotDirs.size(); ++i) {
        char b[48];
        std::snprintf(b, sizeof b, "%s(%+.1f, %+.1f)", i ? " " : "", R.ShotDirs[i].x, R.ShotDirs[i].y);
        dirs += b;
        if (i >= 2) { yawMin = std::min(yawMin, R.ShotDirs[i].x); yawMax = std::max(yawMax, R.ShotDirs[i].x); } // the hip six
    }
    std::printf("[NpcTest] revolver: kick direction 0.1 s after each round (right, up deg; 2 sights, 6 hip): %s\n", dirs.c_str());
    std::printf("[NpcTest] revolver: right hand on the gun (in the gun's frame, most slide): sights standing %.1f mm, hip walking %.1f mm, "
                "sights walking %.1f mm; right arm straightness (most) %.3f / %.3f / %.3f\n", R.Slips[0].Most * 1000.0f,
                R.Slips[1].Most * 1000.0f, R.Slips[2].Most * 1000.0f, R.Slips[0].Stretch, R.Slips[1].Stretch, R.Slips[2].Stretch);
    std::printf("[NpcTest] revolver: left hand on the gun, and the gun's tilt in the view (standing sights / hip walk / sights walk): left %.1f / %.1f / %.1f mm, "
                "tilt %.1f / %.1f / %.1f deg\n", R.Slips[0].HandOnBone * 1000.0f, R.Slips[1].HandOnBone * 1000.0f,
                R.Slips[2].HandOnBone * 1000.0f, R.Slips[0].GunOnBone, R.Slips[1].GunOnBone, R.Slips[2].GunOnBone);
    std::printf("[NpcTest] revolver: trigger finger curls %.1f mm toward the wrist; muzzle light on %d of %d shots, peak %.2f, "
                "%.2f m from the muzzle at most, %.2f left 0.3 s on; tracers in flight %d\n",
                R.FingerCurl * 1000.0f, R.ShotsLit, R.ShotsSeen, R.LitPeak, R.LitFar, R.LitLate, R.Tracers);
    Check(m_FlameSwitched, "the player switched to the revolver (slot 3)");
    Check(m_RevShots == 8, "8 rounds fired (" + std::to_string(m_RevShots) + ")");
    Check(m_RevAmmoAtReload1 == 4, "4 rounds left before the partial reload");
    Check(m_RevAmmoAfterReload1 == 6, "6 rounds after the partial reload");
    Check(m_RevAmmoAtEnd == 6, "6 rounds after the full reload");
    Check(R.HammerMax > 0.003f, "the hammer moves on the hip shots");
    Check(R.TriggerMax > 0.001f, "the trigger moves on the hip shots");
    std::printf("[NpcTest] revolver: cylinder over the six hip shots %.0f deg\n", R.HipTurn);
    Check(std::abs(std::abs(R.HipTurn) - 360.0f) < 30.0f, "the cylinder turns one chamber per hip shot (6 x 60 = 360 deg)");
    Check(R.WorstBack > -5.0f, "the cylinder never steps backwards while firing");
    Check(R.SprintState == "Sprint", "the arms sprint");
    Check(R.ShotDirs.size() == 8 && yawMax - yawMin > 1.0f, "the hip kicks vary side to side shot to shot (over 1 deg)");
    Check(R.FingerCurl > 0.001f && R.FingerCurl < 0.03f, "the trigger finger curls on the hip shots");
    Check(R.ShotsSeen == 8 && R.ShotsLit == R.ShotsSeen, "the muzzle light comes on for every shot");
    Check(R.LitFar < 0.3f, "the muzzle light sits at the muzzle");
    Check(R.LitLate == 0.0f, "the muzzle light is out 0.3 s after a shot");
    Check(R.Tracers == 0, "the player's rounds draw no tracers (as the AK's and the 870's)");
    Check(ejFire == 0, "no case leaves a revolver on firing");
    Check(ejPartial == 6, "the partial reload drops 6 (2 spent, 4 live)");
    Check(ejFull == 6, "the full reload drops 6 cases");
    Check(R.CasesBefore2 > 0 && R.SleepBefore2 == R.CasesBefore2, "the cases land and sleep");
    Check(R.CasesEnd > 0 && R.SleepEnd == R.CasesEnd, "the second reload's cases land and sleep");
    Check(R.Inspected && R.MagChecked && R.Meleed, "inspect, mag check and melee play");
    Check(ejAk == 3, "the AK still ejects one case per shot (3)");
    Check(ejShotgun == 2, "the 870 still ejects one shell per pump (2)");
    Check(R.RevolverBack == 6 && R.AkBack == R.AkAmmoLeft && R.ShotgunBack == R.ShotgunAmmoLeft, "each slot keeps its ammo");
}
