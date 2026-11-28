// Unit tests: the first-person jank meter (FirstPersonJankMeter) and the arm bridge's free-function steps (the arms' solve in
// FirstPersonBody::ArmsLateUpdate: the shoulders' follow, the collarbone turn, the reach lean, the arm-shape links).
// Registered by RunUnitTests through RegisterFirstPersonJankTests.
#include "UnitTestSupport.h"
#include "FirstPersonBody.h"
#include "FirstPersonJankMeter.h"
#include "IK.h"

#include <algorithm>
#include <cmath>

namespace {

using Kind = FirstPersonJankEvent::Kind;

// A walk script: `seconds` at 60 Hz, the gait phase running at `rate` cycles a second, each hand's height off `y(i, phase)`.
template <typename Fn>
FirstPersonJankMeter RunWalk(float seconds, float rate, Fn y, FirstPersonJankMeter m = FirstPersonJankMeter()) {
    m.Reset();
    float phase = 0.0f;
    const int frames = (int)(seconds * 60.0f);
    for (int i = 0; i < frames; ++i) {
        FirstPersonJankSample s;
        s.Dt = 1.0f / 60.0f;
        s.State = "Walk";
        s.Speed = 1.5f;
        phase = std::fmod(phase + rate / 60.0f, 1.0f);
        s.GaitPhase = phase;
        s.Hand[0] = glm::vec3(-0.1f, y(i, phase), 0.3f);
        s.Hand[1] = glm::vec3(0.1f, y(i, phase), 0.3f);
        m.Push(s);
    }
    return m;
}

void TestFirstPersonJankMeterBounce() {
    // A 8 mm (peak to peak) bob at the step rate (two a stride): a cycle is measured, and it is under the line.
    auto calm = RunWalk(3.0f, 1.1f, [](int, float ph) { return 0.004f * std::sin(6.2831853f * 2.0f * ph); });
    CHECK(calm.Events().empty());
    CHECK(calm.LastCycleMm() > 6.0f && calm.LastCycleMm() < 9.0f);
    // The same at 30 mm: one Bounce event per cycle that ran (merged by state within a tenth of a second does not apply
    // across a stride), at about its size.
    auto bounce = RunWalk(3.0f, 1.1f, [](int, float ph) { return 0.015f * std::sin(6.2831853f * 2.0f * ph); });
    CHECK(!bounce.Events().empty());
    CHECK(bounce.Worst(Kind::Bounce) > 27.0f && bounce.Worst(Kind::Bounce) < 31.0f);
    CHECK(bounce.Events().front().What == Kind::Bounce && bounce.Events().front().State == "Walk");
    // Standing (no gait phase) or crawling under MinBounceSpeed: nothing is measured, however much the hands move.
    FirstPersonJankMeter still;
    for (int i = 0; i < 180; ++i) {
        FirstPersonJankSample s;
        s.Dt = 1.0f / 60.0f;
        s.State = "Idle";
        s.GaitPhase = -1.0f;
        s.Hand[0] = s.Hand[1] = glm::vec3(0.0f, 0.02f * std::sin(i * 0.2f), 0.3f);
        still.Push(s);
    }
    CHECK(still.Events().empty() && still.LastCycleMm() < 0.0f);
    // A cycle that ran through a state change is the transition's (a draw, an aim), not the walk's.
    FirstPersonJankMeter mixed;
    float phase = 0.0f;
    for (int i = 0; i < 180; ++i) {
        FirstPersonJankSample s;
        s.Dt = 1.0f / 60.0f;
        s.State = i == 40 ? "Aim" : i < 40 ? "Walk" : "Aim";
        s.Speed = 1.5f;
        phase = std::fmod(phase + 1.1f / 60.0f, 1.0f);
        s.GaitPhase = phase;
        s.Hand[0] = s.Hand[1] = glm::vec3(0.0f, 0.02f * std::sin(6.2831853f * 2.0f * phase), 0.3f);
        mixed.Push(s);
    }
    bool bouncedInWalkCycleThatSpansTheChange = false;
    for (const auto& e : mixed.Events()) bouncedInWalkCycleThatSpansTheChange = bouncedInWalkCycleThatSpansTheChange || (e.What == Kind::Bounce && e.Time < 1.3f);
    CHECK(!bouncedInWalkCycleThatSpansTheChange);
    // A shot inside a cycle spoils it too: the kick is not a bounce.
    FirstPersonJankMeter shot;
    phase = 0.0f;
    for (int i = 0; i < 180; ++i) {
        FirstPersonJankSample s;
        s.Dt = 1.0f / 60.0f;
        s.State = "Walk";
        s.Speed = 1.5f;
        s.Recoil = i >= 5 && i < 12;
        phase = std::fmod(phase + 1.1f / 60.0f, 1.0f);
        s.GaitPhase = phase;
        s.Hand[0] = s.Hand[1] = glm::vec3(0.0f, 0.0f, 0.3f);
        if (s.Recoil) s.Hand[0].y = s.Hand[1].y = 0.03f;
        shot.Push(s);
    }
    CHECK(shot.Worst(Kind::Bounce) == 0.0f);
}

void TestFirstPersonJankMeterEvents() {
    auto run = [](auto setup) {
        FirstPersonJankMeter m;
        for (int i = 0; i < 120; ++i) {
            FirstPersonJankSample s;
            s.Dt = 1.0f / 60.0f;
            s.State = "Reload";
            setup(s, i);
            m.Push(s);
        }
        return m;
    };
    // A hand that eases over 30 frames (smoothstep): no pop. A 2 cm step: one pop (two disturbed frames), at about 2 cm.
    auto ease = run([](FirstPersonJankSample& s, int i) { const float t = std::clamp(i / 30.0f, 0.0f, 1.0f); s.Hand[1].x = 0.07f * t * t * (3.0f - 2.0f * t); });
    CHECK(ease.Events().empty());
    auto step = run([](FirstPersonJankSample& s, int i) { s.Hand[1].x = i >= 20 ? 0.02f : 0.0f; });
    CHECK(step.Events().size() == 1 && step.Events()[0].What == Kind::HandPop && step.Events()[0].Side == 1 && step.Events()[0].Size > 1.5f);
    // ... the same step as a shot's kick: nothing. At 30 Hz steps the threshold is scaled, so the same smooth ease is still quiet.
    auto kick = run([](FirstPersonJankSample& s, int i) { s.Hand[1].x = i >= 20 ? 0.02f : 0.0f; s.Recoil = i >= 18 && i < 30; });
    CHECK(kick.Events().empty());
    // A hand 3 cm off the rig's grip for a stretch: one Grip event on that side.
    auto grip = run([](FirstPersonJankSample& s, int i) { if (i > 40 && i < 50) s.HandGap[0] = 0.03f; });
    CHECK(grip.Events().size() == 1 && grip.Events()[0].What == Kind::Grip && grip.Events()[0].Side == 0 && std::abs(grip.Events()[0].Size - 3.0f) < 0.01f);
    // A hand asked 5% past the arm's length: a Stretch of 5%. Within it: nothing.
    auto stretch = run([](FirstPersonJankSample& s, int i) { s.Reach[1] = i > 30 && i < 40 ? 1.05f : 0.97f; });
    CHECK(stretch.Events().size() == 1 && stretch.Events()[0].What == Kind::Stretch && std::abs(stretch.Events()[0].Size - 5.0f) < 0.01f);
    // The body's arm 3 cm from the eye: Clip; the camera seeing the inside of the body: Clip at 0.
    auto near = run([](FirstPersonJankSample& s, int i) { if (i > 50 && i < 60) s.EyeToArm[0] = 0.03f; });
    CHECK(near.Events().size() == 1 && near.Events()[0].What == Kind::Clip && std::abs(near.Events()[0].Size - 3.0f) < 0.01f);
    auto inside = run([](FirstPersonJankSample& s, int i) { if (i == 70) s.InsideRays = 12; });
    CHECK(inside.Events().size() == 1 && inside.Events()[0].What == Kind::Clip && inside.Events()[0].Size == 0.0f);
    // An invalid frame (holstered) restarts the path: a hand that is somewhere else when the gun returns is not a pop.
    auto holster = run([](FirstPersonJankSample& s, int i) { s.Valid = i < 50 || i >= 70; s.Hand[1].x = i >= 70 ? 0.2f : 0.0f; });
    CHECK(holster.Events().empty());
    // Describe names the kind, side, state and unit.
    CHECK(grip.Events()[0].Describe().find("off grip") != std::string::npos && grip.Events()[0].Describe().find("Reload") != std::string::npos);
}

// root(0) -> clavicle(1) -> upper arm(2) -> lower arm(3) -> hand(4), laid along +x from the root.
struct BridgeArm {
    IK::Pose pose{5};
    std::vector<int> parents{-1, 0, 1, 2, 3};
    std::vector<glm::mat4> g;
    BridgeArm() {
        pose[1].T = glm::vec3(0.10f, 0.0f, 0.0f);
        pose[2].T = glm::vec3(0.15f, 0.0f, 0.0f);
        pose[3].T = glm::vec3(0.30f, 0.0f, 0.0f);
        pose[4].T = glm::vec3(0.25f, 0.0f, 0.0f);
        IK::ComputeGlobals(pose, parents, g);
    }
    glm::vec3 P(int i) const { return IK::Position(g[(size_t)i]); }
};

void TestFirstPersonBodyBridgeSteps() {
    // Follow: a snap at no time, the exponential share otherwise, and two half steps make one whole one (frame-rate independent).
    CHECK(FirstPersonBodyFollow(0.016f, 0.0f) == 1.0f);
    CHECK(std::abs(FirstPersonBodyFollow(0.5f, 0.5f) - (1.0f - std::exp(-1.0f))) < 1e-6f);
    const float half = FirstPersonBodyFollow(1.0f / 120.0f, 0.2f), whole = FirstPersonBodyFollow(1.0f / 60.0f, 0.2f);
    CHECK(std::abs((1.0f - (1.0f - half) * (1.0f - half)) - whole) < 1e-6f);

    // The shoulders the eye hangs off: starts on the animation, follows it slowly, and never trails by more than the slack.
    {
        glm::vec3 eased(0.0f);
        bool have = false;
        FirstPersonBodyFollowShoulders(eased, have, glm::vec3(1.0f, 0.0f, 0.0f), 0.035f, 0.0f, 1.0f / 60.0f);
        CHECK(have && glm::length(eased - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-6f); // the first frame lands on it
        glm::vec3 jump(1.2f, 0.0f, 0.0f); // the animation moves 20 cm at once: held back to the slack at no pull-in time
        FirstPersonBodyFollowShoulders(eased, have, jump, 0.035f, 0.0f, 1.0f / 60.0f);
        CHECK(std::abs(glm::length(jump - eased) - 0.035f) < 1e-4f);
        // A small sway (1 cm) inside the slack comes through only as the slow average: well under half of it in a step.
        glm::vec3 e2(0.0f);
        bool h2 = false;
        FirstPersonBodyFollowShoulders(e2, h2, glm::vec3(0.0f), 0.035f, 0.04f, 1.0f / 60.0f);
        FirstPersonBodyFollowShoulders(e2, h2, glm::vec3(0.01f, 0.0f, 0.0f), 0.035f, 0.04f, 1.0f / 60.0f);
        CHECK(e2.x > 0.0f && e2.x < 0.005f);
        // Slack 0 + big move, with a pull-in time, closes the excess gradually (not in one frame).
        glm::vec3 e3(0.0f);
        bool h3 = false;
        FirstPersonBodyFollowShoulders(e3, h3, glm::vec3(0.0f), 0.02f, 0.04f, 1.0f / 60.0f);
        FirstPersonBodyFollowShoulders(e3, h3, glm::vec3(0.3f, 0.0f, 0.0f), 0.02f, 0.04f, 1.0f / 60.0f);
        CHECK(glm::length(glm::vec3(0.3f, 0.0f, 0.0f) - e3) > 0.02f);
    }

    // Turn Clavicle: the upper arm heads for the target by at most maxAngle, by TURNING the collarbone (the shoulder never
    // slides off the collarbone's length, the collarbone's own root stays put).
    {
        BridgeArm a;
        const glm::vec3 clav = a.P(1), upper = a.P(2);
        const float len = glm::length(upper - clav);
        const glm::vec3 want = clav + glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + 0.2f * glm::vec3(1.0f, 0.0f, 0.0f)) * 0.5f; // ~79 deg off
        auto turned = [&](float maxAngle, float& angleOut) {
            BridgeArm b;
            std::vector<LocalTRS> pose = b.pose;
            const glm::quat q = FirstPersonBodyTurnClavicle(pose, b.parents, b.g, 1, 2, want, maxAngle);
            angleOut = FirstPersonBodyAngleOf(q);
            return b;
        };
        float angle = 0.0f;
        BridgeArm capped = turned(glm::radians(10.0f), angle);
        CHECK(std::abs(angle - glm::radians(10.0f)) < 1e-3f);
        CHECK(glm::length(capped.P(1) - clav) < 1e-5f);                                   // the collarbone's root stays
        CHECK(std::abs(glm::length(capped.P(2) - capped.P(1)) - len) < 1e-5f);            // the shoulder keeps its distance from it
        BridgeArm full = turned(glm::radians(170.0f), angle);
        const glm::vec3 dir = glm::normalize(full.P(2) - full.P(1)), wantDir = glm::normalize(want - clav);
        CHECK(glm::dot(dir, wantDir) > 0.9999f);                                          // all the way: the upper arm heads for it
        BridgeArm none = turned(0.0f, angle);
        CHECK(angle == 0.0f && glm::length(none.P(2) - upper) < 1e-6f);                   // none allowed: untouched
    }

    // Reach Lean: nothing while the hand is within what the arm and the collarbone give; once it is past, a lean toward it
    // capped at Reach Lean Max, and `weight` of it.
    {
        BridgeArm a;
        const int upper[2] = {2, -1}, lower[2] = {3, -1}, hand[2] = {4, -1}, clav[2] = {1, -1};
        const bool have[2] = {true, false};
        const float shoulderMax = glm::radians(25.0f), leanMax = glm::radians(15.0f);
        glm::quat turn(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 inReach[2] = {a.P(4), glm::vec3(0.0f)};
        CHECK(!FirstPersonBodyReachLean(a.g, upper, lower, hand, clav, have, inReach, glm::mat4(1.0f), 0, shoulderMax, leanMax, 1.0f, turn));
        glm::vec3 far[2] = {a.P(2) + glm::vec3(0.0f, 0.9f, 0.0f), glm::vec3(0.0f)}; // 90 cm straight up from the shoulder
        CHECK(FirstPersonBodyReachLean(a.g, upper, lower, hand, clav, have, far, glm::mat4(1.0f), 0, shoulderMax, leanMax, 1.0f, turn));
        CHECK(std::abs(FirstPersonBodyAngleOf(turn) - leanMax) < 1e-3f);
        glm::quat halfTurn(1.0f, 0.0f, 0.0f, 0.0f);
        CHECK(FirstPersonBodyReachLean(a.g, upper, lower, hand, clav, have, far, glm::mat4(1.0f), 0, shoulderMax, leanMax, 0.5f, halfTurn));
        CHECK(std::abs(FirstPersonBodyAngleOf(halfTurn) - 0.5f * leanMax) < 1e-3f);
        const bool none[2] = {false, false}; // no hand to reach (an arm with no gun): no lean
        CHECK(!FirstPersonBodyReachLean(a.g, upper, lower, hand, clav, none, far, glm::mat4(1.0f), 0, shoulderMax, leanMax, 1.0f, turn));
    }

    // Arm-shape links: the nodes under a clavicle in order, each paired with its rig counterpart, the clavicle itself flagged.
    {
        const std::vector<int> parents{-1, 0, 1, 2, 3, 0};   // 0 root, 1 clavicle, 2..4 its arm, 5 a sibling spine node
        const int clavicles[2] = {1, -1};
        std::vector<FirstPersonArmShapeLink> links;
        FirstPersonBodyArmShapeLinksFrom(6, parents, clavicles, [](int i) { return i >= 1 && i <= 4 ? i + 10 : -1; }, links);
        CHECK(links.size() == 4);
        if (links.size() == 4) {
            CHECK(links[0].Body == 1 && links[0].Rig == 11 && links[0].Clavicle);
            CHECK(links[3].Body == 4 && links[3].Rig == 14 && !links[3].Clavicle);
        }
    }
}

} // namespace

void RegisterFirstPersonJankTests(UnitTestSupport::TestList& tests) {
    tests.insert(tests.end(), {
        {"FirstPersonJankMeterBounce", TestFirstPersonJankMeterBounce},
        {"FirstPersonJankMeterEvents", TestFirstPersonJankMeterEvents},
        {"FirstPersonBodyBridgeSteps", TestFirstPersonBodyBridgeSteps},
    });
}
