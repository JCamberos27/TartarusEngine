// Motion matching (docs/MOTION_MATCHING_HANDOFF.md, Phase 2): the bake's pieces on synthetic clips - RootSynth on a
// pelvis swaying along a straight path and on a turn in place, foot contacts, the smallest-three packing, the pose
// stream and feature files round-tripped, the takes list. Tests that need the licensed mocap pack (the rig diff) or the
// cooked stream SKIP, never FAIL, when those files are missing (CI has neither).
#include "UnitTestSupport.h"

#include "../Game/MotionMatching/MotionBake.h"
#include "../Game/MotionMatching/MotionClipStream.h"
#include "../Game/MotionMatching/MotionFeatures.h"
#include "../Game/MotionMatching/MotionMath.h"
#include "../Game/MotionMatching/RootSynth.h"

#include "Animation.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace {

namespace fs = std::filesystem;
using MotionMath::kPi;
constexpr float kDeg = 180.0f / kPi;

fs::path MmTemp() {
    std::error_code ec;
    const fs::path p = fs::temp_directory_path(ec) / "TartarusMotionMatchingTests";
    fs::create_directories(p, ec);
    return p;
}

void Skip(const char* test, const std::string& why) { std::printf("[UnitTest] SKIP %s: %s\n", test, why.c_str()); }

// A walker heading along `heading` (radians, 0 = +Z) at `speed`, its pelvis swaying side to side at the stride
// frequency and its hips and shoulders counter-rotating +-10 degrees each step.
RootSynthInput Walker(int frames, float fps, float heading, float speed, float sway, float hipTwistDeg) {
    RootSynthInput in;
    in.Fps = fps;
    const glm::vec3 fwd = MotionMath::YawDir(heading);
    const glm::vec3 side(fwd.z, 0.0f, -fwd.x);
    for (int i = 0; i < frames; ++i) {
        const float t = (float)i / fps;
        const float step = std::sin(2.0f * kPi * 1.0f * t); // one stride (two steps) a second
        in.Pelvis.push_back(fwd * (speed * t) + side * (sway * step) + glm::vec3(0.0f, 0.95f, 0.0f));
        const float twist = hipTwistDeg / kDeg * step;
        // right = -side at facing `heading` (forward = up x right): the hips turn +twist, the shoulders -twist
        const auto rightAt = [&](float yaw) {
            const glm::vec3 f = MotionMath::YawDir(yaw);
            return glm::vec3(-f.z, 0.0f, f.x) * 0.2f;
        };
        in.HipsRight.push_back(rightAt(heading + twist));
        in.ShoulderRight.push_back(rightAt(heading - twist));
    }
    return in;
}

void TestMotionMath() {
    // Odd mirroring keeps a line a line: smoothing a ramp leaves it alone, edges included.
    std::vector<float> ramp;
    for (int i = 0; i < 50; ++i) ramp.push_back(0.3f * (float)i - 2.0f);
    const std::vector<float> s = MotionMath::GaussianSmooth(ramp, 6.0f);
    float worst = 0.0f;
    for (size_t i = 0; i < ramp.size(); ++i) worst = std::max(worst, std::fabs(s[i] - ramp[i]));
    CHECK(worst < 1e-4f);
    // A sine at 1/4 of the kernel's width is mostly removed.
    std::vector<float> wave;
    for (int i = 0; i < 600; ++i) wave.push_back(std::sin(2.0f * kPi * (float)i / 30.0f));
    const std::vector<float> w = MotionMath::GaussianSmooth(wave, 15.0f);
    float amp = 0.0f;
    for (size_t i = 100; i < 500; ++i) amp = std::max(amp, std::fabs(w[i]));
    CHECK(amp < 0.05f);
    // Unwrap removes the 2-pi jumps.
    std::vector<float> a = {3.0f, -3.1f, -2.9f, 2.9f};
    MotionMath::Unwrap(a);
    CHECK(std::fabs(a[1] - (2.0f * kPi - 3.1f)) < 1e-4f);
    CHECK(std::fabs(a[2] - (2.0f * kPi - 2.9f)) < 1e-4f);
    CHECK(std::fabs(a[3] - 2.9f) < 1e-4f); // from 2 pi - 2.9 the short way to 2.9 is back down, not round again
    for (size_t i = 1; i < a.size(); ++i) CHECK(std::fabs(a[i] - a[i - 1]) < kPi);
    CHECK(std::fabs(MotionMath::WrapPi(3.0f * kPi) - kPi) < 1e-4f || std::fabs(MotionMath::WrapPi(3.0f * kPi) + kPi) < 1e-4f);
    CHECK(std::fabs(MotionMath::YawOf(glm::vec3(1, 0, 0)) - kPi * 0.5f) < 1e-5f);

    // Smallest-three: every rotation comes back within 2e-4 rad, q and -q alike.
    std::mt19937 rng(7);
    std::normal_distribution<float> n01;
    float worstAngle = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        glm::quat q = glm::normalize(glm::quat(n01(rng), n01(rng), n01(rng), n01(rng)));
        if (i % 2) q = -q;
        worstAngle = std::max(worstAngle, MotionMath::QuatAngle(q, MotionMath::UnpackQuat(MotionMath::PackQuat(q))));
    }
    const glm::quat axes[] = {glm::quat(1, 0, 0, 0), glm::quat(0, 1, 0, 0), glm::quat(0, 0, 1, 0), glm::quat(0, 0, 0, 1),
                              glm::normalize(glm::quat(1, 1, 0, 0))};
    for (const glm::quat& q : axes)
        worstAngle = std::max(worstAngle, MotionMath::QuatAngle(q, MotionMath::UnpackQuat(MotionMath::PackQuat(q))));
    std::printf("[MotionMatching] smallest-three worst error %.2e rad\n", worstAngle);
    CHECK(worstAngle < 2e-4f);
}

void TestRootSynthSinePath() {
    // 10 s walking diagonally at 1.4 m/s, the pelvis swaying 4 cm and the hips twisting 10 degrees every step.
    const float fps = 60.0f, heading = 0.6f;
    const RootSynthInput in = Walker(600, fps, heading, 1.4f, 0.04f, 10.0f);
    const RootTrack root = RootSynth::Generate(in, RootSynthParams{});
    CHECK(root.Size() == 600);
    const glm::vec3 fwd = MotionMath::YawDir(heading), side(fwd.z, 0.0f, -fwd.x);
    float lateral = 0.0f, yawErr = 0.0f, ground = 0.0f;
    for (size_t i = 0; i < root.Size(); ++i) {
        lateral = std::max(lateral, std::fabs(glm::dot(root.Position[i], side)));
        yawErr = std::max(yawErr, std::fabs(MotionMath::WrapPi(root.Yaw[i] - heading)));
        ground = std::max(ground, std::fabs(root.Position[i].y));
    }
    std::printf("[MotionMatching] sine path: lateral %.2f cm (sway 4 cm), yaw error %.2f deg\n", lateral * 100.0f, yawErr * kDeg);
    CHECK(ground == 0.0f);
    // The handoff's 0.10 s position Gaussian passes exp(-(2 pi f sigma)^2 / 2) = 82% of a 1 Hz (stride) sway: 3.3 of 4 cm,
    // edges included (no more there than in the middle).
    CHECK(lateral > 0.028f && lateral < 0.036f);
    CHECK(yawErr * kDeg < 1.0f);    // the hips' and shoulders' twists cancel; 0.25 s smooths what's left
    // Travel speed kept to the edges (odd mirroring): no slow-down at the first or last frame.
    const float v0 = glm::length(root.Position[1] - root.Position[0]) * fps;
    const float vEnd = glm::length(root.Position[599] - root.Position[598]) * fps;
    CHECK(std::fabs(v0 - 1.4f) < 0.1f);
    CHECK(std::fabs(vEnd - 1.4f) < 0.1f);
    const RootValidation v = RootSynth::Validate(root, fps, in.HipsRight);
    std::printf("[MotionMatching] sine path: jitter %.1f, facing p95 %.2f deg, speed max %.2f, wobble %.2f cm\n", v.YawJitter,
                v.FacingP95, v.SpeedMax, v.PathWobble);
    CHECK(v.FacingP95 < 12.0f);      // the hips alone twist 10 degrees
    CHECK(v.YawJitter < 50.0f);
    CHECK(v.StepMax < 0.03f);
    CHECK(v.PathWobble > 1.0f && v.PathWobble < 2.5f); // the 3.3 cm sway left in, through the band-pass (~0.7), as RMS

    // Hips only (no shoulders): the 10-degree twist shows more, but stays well inside 20 degrees.
    RootSynthParams hipsOnly;
    hipsOnly.HipsWeight = 1.0f;
    const RootTrack r2 = RootSynth::Generate(in, hipsOnly);
    float yawErr2 = 0.0f;
    for (size_t i = 0; i < r2.Size(); ++i) yawErr2 = std::max(yawErr2, std::fabs(MotionMath::WrapPi(r2.Yaw[i] - heading)));
    std::printf("[MotionMatching] sine path, hips only: yaw error %.2f deg (twist 10), first frame %.2f deg\n", yawErr2 * kDeg,
                MotionMath::WrapPi(r2.Yaw[0] - heading) * kDeg);
    CHECK(yawErr2 * kDeg > yawErr * kDeg);
    CHECK(yawErr2 * kDeg < 5.0f); // the edges smoothed like the middle: 0.25 s passes ~29% of the 1 Hz twist (2.9 of 10 degrees)
}

void TestRootSynthTurnInPlace() {
    // 8 s turning in place at 90 deg/s through +-180 (the unwrap), the pelvis bobbing 3 cm around a spot.
    const float fps = 60.0f, rate = 90.0f / kDeg;
    RootSynthInput in;
    in.Fps = fps;
    for (int i = 0; i < 480; ++i) {
        const float t = (float)i / fps, yaw = 2.5f + rate * t;
        in.Pelvis.push_back(glm::vec3(0.03f * std::sin(2.0f * kPi * 1.5f * t), 0.95f, 0.03f * std::cos(2.0f * kPi * 1.5f * t)));
        const glm::vec3 f = MotionMath::YawDir(yaw);
        in.HipsRight.push_back(glm::vec3(-f.z, 0.0f, f.x));
        in.ShoulderRight.push_back(glm::vec3(-f.z, 0.0f, f.x));
    }
    const RootTrack root = RootSynth::Generate(in, RootSynthParams{});
    float drift = 0.0f, rateErr = 0.0f, jump = 0.0f;
    for (size_t i = 0; i < root.Size(); ++i) {
        drift = std::max(drift, glm::length(root.Position[i]));
        if (i > 0) {
            jump = std::max(jump, std::fabs(root.Yaw[i] - root.Yaw[i - 1]));
            rateErr = std::max(rateErr, std::fabs((root.Yaw[i] - root.Yaw[i - 1]) * fps - rate));
        }
    }
    std::printf("[MotionMatching] turn in place: drift %.2f cm, rate error %.2f deg/s, total turn %.0f deg\n", drift * 100.0f,
                rateErr * kDeg, (root.Yaw.back() - root.Yaw.front()) * kDeg);
    CHECK(drift < 0.025f);           // 0.10 s passes 64% of the 1.5 Hz, 3 cm circle: ~1.9 cm, edges included
    CHECK(rateErr * kDeg < 2.0f);    // a steady rate, edges included
    CHECK(jump < 0.1f);              // no 360-degree flip through +-180
    CHECK(std::fabs((root.Yaw.back() - root.Yaw.front()) * kDeg - 90.0f * 479.0f / 60.0f) < 2.0f);
    const RootValidation v = RootSynth::Validate(root, fps, in.HipsRight);
    CHECK(v.YawJitter < 10.0f);
    CHECK(std::fabs(v.YawRateMax - 90.0f) < 3.0f);
    CHECK(v.FacingP95 == 0.0f); // never moving 0.5 m/s: no facing samples
}

void TestRootSynthContactsAndSlide() {
    // A foot planted 0.4 s, swinging 0.4 s at 1.5 m/s; the planted foot creeps 1 cm/s, and one stray 1-frame lift flickers.
    const float fps = 60.0f;
    std::vector<glm::vec3> foot;
    float z = 0.0f;
    for (int i = 0; i < 480; ++i) {
        const int phase = i % 48;
        const bool stance = phase < 24;
        z += (stance ? 0.01f : 1.5f) / fps;
        foot.push_back(glm::vec3(0.0f, stance ? 0.08f : 0.08f + 0.1f * std::sin(kPi * (float)(phase - 24) / 24.0f), z));
    }
    foot[100].y = 0.3f; // flicker inside a stance (frame 100 is phase 4)
    const std::vector<char> c = RootSynth::Contacts(foot, fps, 0.15f, 0.6f);
    int planted = 0;
    for (char x : c) planted += x;
    std::printf("[MotionMatching] contacts: %d of 480 frames planted\n", planted);
    CHECK(c[100] == 1);                 // the flicker filled
    CHECK(planted > 180 && planted < 260);
    RootTrack root;
    for (int i = 0; i < 480; ++i) {
        root.Position.push_back(glm::vec3(0.0f));
        root.Yaw.push_back(0.0f);
    }
    std::vector<glm::vec3> feet[2] = {foot, foot};
    std::vector<char> contacts[2] = {c, c};
    const RootValidation v = RootSynth::Validate(root, fps, {}, feet, contacts);
    std::printf("[MotionMatching] slide: %.2f cm/s while planted (1 cm/s creep)\n", v.FootSlide[0]);
    CHECK(v.FootSlide[0] > 0.5f && v.FootSlide[0] < 1.5f);
    CHECK(v.ContactFrames[0] > 150);
}

// A tiny synthetic stream: two clips, three bones.
MotionClipStream SyntheticStream() {
    MotionClipStream s;
    s.Reset(60.0f, {"pelvis", "thigh_l", "calf_l"});
    s.SourcePelvisHeight = 0.95f;
    s.RestYaw = 0.25f;
    s.Statics.push_back({"index_01_l", glm::angleAxis(0.4f, glm::vec3(0, 0, 1))});
    for (int c = 0; c < 2; ++c) {
        const int n = c == 0 ? 90 : 61;
        std::vector<MotionClipStream::RootSample> root;
        std::vector<glm::vec3> pelvis;
        std::vector<glm::quat> rot;
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / 60.0f;
            root.push_back({glm::vec3(1.2f * t, 0.0f, (float)c), 0.3f * t + (float)c});
            pelvis.push_back(glm::vec3(0.01f * std::sin(t), 0.95f + 0.02f * std::cos(5.0f * t), 0.0f));
            for (int b = 0; b < 3; ++b) rot.push_back(glm::angleAxis(0.5f * t * (float)(b + 1), glm::normalize(glm::vec3(1.0f, (float)b, 0.5f))));
        }
        s.AddClip({c == 0 ? "Walk" : "Turn", c == 0 ? "walk;forward" : "turn", 0, 0, 1.0f + (float)c}, root, pelvis, rot);
    }
    return s;
}

void TestClipStreamRoundTrip() {
    const MotionClipStream s = SyntheticStream();
    CHECK(s.FrameCount() == 151);
    CHECK(s.Clips().size() == 2 && s.Clips()[1].First == 90);
    CHECK(s.PoseBytes() == 151u * (12u + 12u + 3u * 6u));
    const std::string path = (MmTemp() / "synthetic.mmanim").string();
    std::string err;
    CHECK(s.Save(path, &err));
    MotionClipStream l;
    CHECK(l.Load(path, &err));
    CHECK(l.FrameCount() == 151 && l.Bones().size() == 3 && l.Clips().size() == 2);
    CHECK(l.Clips()[0].Name == "Walk" && l.Clips()[0].Tags == "walk;forward" && l.Clips()[1].SourceStart == 2.0f);
    CHECK(l.Statics.size() == 1 && l.Statics[0].Name == "index_01_l");
    CHECK(l.RestYaw == 0.25f && l.SourcePelvisHeight == 0.95f);
    float worst = 0.0f;
    for (std::uint32_t f = 0; f < l.FrameCount(); ++f) {
        CHECK(l.Root(f).Position == s.Root(f).Position);
        for (int b = 0; b < 3; ++b) worst = std::max(worst, MotionMath::QuatAngle(l.Rotation(f, b), s.Rotation(f, b)));
    }
    CHECK(worst < 1e-6f); // the packed words themselves round-trip
    CHECK(std::fabs(l.ClipSeconds(0) - 89.0f / 60.0f) < 1e-5f);
    CHECK(l.FindClip("Turn") == 1 && l.FindClip("nope") == -1);

    // Root interpolation, clamped to the clip.
    const MotionClipStream::RootSample mid = l.RootAt(0, 0.5f / 60.0f);
    CHECK(std::fabs(mid.Position.x - 1.2f * 0.5f / 60.0f) < 1e-5f);
    CHECK(l.RootAt(1, 99.0f).Position == l.Root(150).Position);
    // RootMatrix: the rest facing maps onto the root's facing.
    const glm::mat4 R = l.RootMatrixAt(0, 1.0f);
    const glm::vec3 restFwd = MotionMath::YawDir(l.RestYaw);
    const glm::vec3 dir = glm::vec3(R * glm::vec4(restFwd, 0.0f));
    CHECK(std::fabs(MotionMath::WrapPi(MotionMath::YawOf(dir) - l.RootAt(0, 1.0f).Yaw)) < 1e-4f);

    // Binding by name onto a node list: missing bones are -1, the pelvis translation is scaled.
    const std::vector<std::string> nodes = {"root", "pelvis", "calf_l", "index_01_l", "spine_01"};
    const MotionClipStream::Binding b = l.BindNames(nodes, 1, 1.9f);
    CHECK(b.Bound == 2 && b.Nodes[0] == 1 && b.Nodes[1] == -1 && b.Nodes[2] == 2 && b.StaticNodes[0] == 3);
    CHECK(std::fabs(b.PelvisScale - 2.0f) < 1e-5f);
    std::vector<LocalTRS> pose(nodes.size());
    pose[4].R = glm::angleAxis(1.0f, glm::vec3(0, 1, 0));
    l.SamplePose(0, 0.75f, b, pose);
    CHECK(MotionMath::QuatAngle(pose[2].R, glm::angleAxis(0.5f * 0.75f * 3.0f, glm::normalize(glm::vec3(1, 2, 0.5f)))) < 2e-3f);
    CHECK(std::fabs(pose[1].T.y - 2.0f * (0.95f + 0.02f * std::cos(5.0f * 0.75f))) < 2e-3f);
    CHECK(MotionMath::QuatAngle(pose[3].R, glm::angleAxis(0.4f, glm::vec3(0, 0, 1))) < 1e-5f);
    CHECK(MotionMath::QuatAngle(pose[4].R, glm::angleAxis(1.0f, glm::vec3(0, 1, 0))) < 1e-6f); // untouched

    // Corrupt files are refused, not half-read.
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bytes.resize(bytes.size() - 7);
        std::ofstream(path, std::ios::binary | std::ios::trunc).write(bytes.data(), (std::streamsize)bytes.size());
    }
    MotionClipStream bad;
    CHECK(!bad.Load(path, &err) && bad.FrameCount() == 0);
    CHECK(!bad.Load((MmTemp() / "missing.mmanim").string(), &err));
}

void TestFeaturesAndLabels() {
    // 3 s at 1.5 m/s along +Z facing +Z (rest facing +Z), then 3 s at 1.5 m/s along +X still facing +Z (a strafe),
    // feet planted alternately; then a crouched second take.
    const float fps = 60.0f;
    MotionClipFrames fr;
    fr.Fps = fps;
    fr.RestYaw = 0.0f;
    RootTrack root;
    for (int i = 0; i < 360; ++i) {
        const float t = (float)i / fps;
        const glm::vec3 p = i < 180 ? glm::vec3(0.0f, 0.0f, 1.5f * t) : glm::vec3(1.5f * (t - 3.0f), 0.0f, 4.5f);
        root.Position.push_back(p);
        root.Yaw.push_back(0.0f);
        fr.Pelvis.push_back(p + glm::vec3(0.0f, 0.95f, 0.0f));
        fr.Foot[0].push_back(p + glm::vec3(0.1f, 0.09f, 0.0f));
        fr.Foot[1].push_back(p + glm::vec3(-0.1f, 0.09f, 0.0f));
    }
    MotionFeatureConfig cfg;
    MotionFeatureSet db;
    db.AddClip("Synthetic", fr, root, cfg, MotionClipIntent::FromTags("walk;card"));
    for (glm::vec3& p : fr.Pelvis) p.y = 0.6f;
    db.AddClip("Crouched", fr, root, cfg, MotionClipIntent::FromTags("crouch;card"));
    db.Finish();
    CHECK(db.FrameCount() == 720 && db.Clips().size() == 2 && db.Clips()[1].First == 360);
    CHECK(db.Style(30) == MotionLabel::StyleWalk && db.Style(390) == MotionLabel::StyleCrouch);
    float f[MotionFeatureSet::kDims];
    db.Decode(30, f); // (16-bit: within a 65534th of each dimension's range)
    CHECK(std::fabs(f[0] - 0.1f) < 1e-4f && std::fabs(f[1] - 0.09f) < 1e-4f); // left foot, root space
    CHECK(std::fabs(f[14] - 1.5f) < 1e-3f);                                        // pelvis velocity forward
    CHECK(std::fabs(f[19]) < 1e-4f && std::fabs(f[20] - 1.5f) < 1e-3f);           // root at +60 frames: 1.5 m ahead
    CHECK(std::fabs(f[25]) < 1e-4f && std::fabs(f[26] - 1.0f) < 1e-4f);           // still facing ahead
    CHECK(db.Gait(30) == MotionLabel::Walk && !(db.Flags(30) & MotionLabel::Strafe));
    CHECK(db.Flags(250) & MotionLabel::Strafe);
    CHECK(!(db.Flags(30) & MotionLabel::Crouch) && (db.Flags(390) & MotionLabel::Crouch));
    CHECK(db.Flags(359) & MotionLabel::NoSearch);
    CHECK(!(db.Flags(299) & MotionLabel::NoSearch) && (db.Flags(300) & MotionLabel::NoSearch));
    CHECK(!(db.Flags(30) & MotionLabel::Airborne));
    const std::string path = (MmTemp() / "synthetic.mmdb").string();
    std::string err;
    CHECK(db.Save(path, &err));
    MotionFeatureSet l;
    CHECK(l.Load(path, &err));
    CHECK(l.FrameCount() == 720 && l.Clips()[1].Name == "Crouched" && l.TrajFrames[2] == 60);
    float lf[MotionFeatureSet::kDims];
    l.Decode(30, lf);
    CHECK(lf[20] == f[20] && l.Flags(250) == db.Flags(250) && l.Mean()[20] == db.Mean()[20] && l.Style(390) == db.Style(390));
    CHECK(l.Std()[0] > 0.0f);
}

// The finger tracks (32-bit smallest-three): within 0.3 deg of the rotation packed, every way round.
void TestPackQuat32() {
    std::mt19937 rng(77);
    std::normal_distribution<float> n(0.0f, 1.0f);
    float worst = 0.0f;
    for (int k = 0; k < 20000; ++k) {
        glm::quat q = glm::normalize(glm::quat(n(rng), n(rng), n(rng), n(rng)));
        if (k % 4 == 0) q = -q;
        worst = std::max(worst, MotionMath::QuatAngle(q, MotionMath::UnpackQuat32(MotionMath::PackQuat32(q))));
    }
    std::printf("[MotionMatching] 32-bit quaternion: worst error %.3f deg over 20000\n", worst * kDeg);
    CHECK(worst * kDeg < 0.3f);
    CHECK(MotionMath::QuatAngle(glm::quat(1, 0, 0, 0), MotionMath::UnpackQuat32(MotionMath::PackQuat32(glm::quat(1, 0, 0, 0)))) * kDeg < 0.3f);
    // The twist about an axis: the swing-twist decomposition's.
    const glm::vec3 x(1, 0, 0);
    CHECK(std::fabs(MotionMath::TwistAngle(glm::angleAxis(0.7f, x), x) - 0.7f) < 1e-5f);
    CHECK(std::fabs(MotionMath::TwistAngle(glm::angleAxis(0.7f, glm::vec3(0, 0, 1)), x)) < 1e-5f);
    const float both = MotionMath::TwistAngle(glm::angleAxis(0.4f, glm::vec3(0, 1, 0)) * glm::angleAxis(-2.9f, x), x);
    CHECK(std::fabs(both + 2.9f) < 0.05f);
}

// A clip with finger tracks plays them, one without plays the relaxed pose; they survive the file.
void TestFingerTracks() {
    MotionClipStream s;
    s.Reset(60.0f, {"pelvis"});
    s.Statics.push_back({"index_01_l", glm::angleAxis(0.4f, glm::vec3(0, 0, 1))});
    s.Statics.push_back({"thumb_01_r", glm::angleAxis(-0.2f, glm::vec3(1, 0, 0))});
    std::vector<MotionClipStream::RootSample> root(60);
    std::vector<glm::vec3> pelvis(60, glm::vec3(0.0f, 0.95f, 0.0f));
    std::vector<glm::quat> rot(60, glm::quat(1, 0, 0, 0)), fingers;
    for (int i = 0; i < 60; ++i) {
        fingers.push_back(glm::angleAxis(0.02f * (float)i, glm::vec3(0, 0, 1)));
        fingers.push_back(glm::angleAxis(-0.01f * (float)i, glm::normalize(glm::vec3(1, 1, 0))));
    }
    s.AddClip({"Hands", "stand;idle", 0, 0, 0.0f}, root, pelvis, rot, &fingers);
    s.AddClip({"NoHands", "walk", 0, 0, 0.0f}, root, pelvis, rot);
    CHECK(s.HasFingers(0) && !s.HasFingers(1) && s.FingerBytes() == 60u * 2u * 4u);
    CHECK(s.PoseBytes() == 120u * (12u + 12u + 6u) + 60u * 2u * 4u);
    CHECK(s.ClipAt(0) == 0 && s.ClipAt(59) == 0 && s.ClipAt(60) == 1 && s.ClipAt(119) == 1 && s.ClipAt(120) == -1);
    const std::string path = (MmTemp() / "fingers.mmanim").string();
    std::string err;
    CHECK(s.Save(path, &err));
    MotionClipStream l;
    CHECK(l.Load(path, &err));
    CHECK(l.HasFingers(0) && !l.HasFingers(1));
    float worst = 0.0f;
    for (int i = 0; i < 60; ++i)
        for (int k = 0; k < 2; ++k) worst = std::max(worst, MotionMath::QuatAngle(l.FingerRotation(0, (std::uint32_t)i, k), fingers[(size_t)(2 * i + k)]));
    CHECK(worst * kDeg < 0.3f);
    CHECK(MotionMath::QuatAngle(l.FingerRotation(1, 75, 0), s.Statics[0].Rotation) < 1e-6f);
    // Posing: the track where there is one, the relaxed pose where not.
    const std::vector<std::string> nodes = {"pelvis", "index_01_l", "thumb_01_r"};
    const MotionClipStream::Binding b = l.BindNames(nodes, 0, 0.95f);
    std::vector<LocalTRS> pose(nodes.size());
    l.FramePose(30, b, pose);
    CHECK(MotionMath::QuatAngle(pose[1].R, fingers[60]) * kDeg < 0.3f && MotionMath::QuatAngle(pose[2].R, fingers[61]) * kDeg < 0.3f);
    l.SamplePose(0, 30.5f / 60.0f, b, pose);
    CHECK(MotionMath::QuatAngle(pose[1].R, glm::angleAxis(0.02f * 30.5f, glm::vec3(0, 0, 1))) * kDeg < 0.3f);
    l.FramePose(90, b, pose);
    CHECK(MotionMath::QuatAngle(pose[1].R, s.Statics[0].Rotation) < 1e-6f && MotionMath::QuatAngle(pose[2].R, s.Statics[1].Rotation) < 1e-6f);
}

// What a take was shot for: its tags name its styles and whether its flights are jumps.
MotionClipFrames StyleFrames(const std::vector<float>& speed, RootTrack& root, int liftFrom = -1, int liftTo = -1) {
    MotionClipFrames fr;
    fr.Fps = 60.0f;
    glm::vec3 p(0.0f);
    for (size_t i = 0; i < speed.size(); ++i) {
        if (i > 0) p.z += speed[i] / 60.0f;
        root.Position.push_back(p);
        root.Yaw.push_back(0.0f);
        const bool lift = (int)i >= liftFrom && (int)i < liftTo;
        fr.Pelvis.push_back(p + glm::vec3(0.0f, lift ? 1.1f : 0.95f, 0.0f));
        fr.Foot[0].push_back(p + glm::vec3(0.1f, lift ? 0.3f : 0.09f, 0.0f));
        fr.Foot[1].push_back(p + glm::vec3(-0.1f, lift ? 0.3f : 0.09f, 0.0f));
    }
    return fr;
}

void TestStyleIntent() {
    using L = MotionLabel::Style;
    const auto styles = [](const char* tags) { return MotionClipIntent::FromTags(tags).Styles; };
    CHECK(styles("walk;card") == std::vector<int>{L::StyleWalk});
    CHECK(styles("stroll;strafe;mirrored") == std::vector<int>{L::StyleStroll});
    CHECK((styles("run;loop;forward") == std::vector<int>{L::StyleWalk, L::StyleRun}));
    CHECK(styles("stand;start;fixed;style=jog") == std::vector<int>{L::StyleJog});
    CHECK((styles("accelerationdeceleration;accel;style=walk+jog+run") == std::vector<int>{L::StyleWalk, L::StyleJog, L::StyleRun}));
    CHECK(styles("stand;idle").empty() && styles("transitions;transition").empty());
    CHECK(MotionClipIntent::FromTags("stand;jump").Jump && !MotionClipIntent::FromTags("jog;card").Jump);

    // A mixed take: standing, a walk, a run, a stop - each frame the nearest of its styles' cruise speeds to its
    // stride-averaged speed; the standing before takes the walk's, the stop the run's.
    std::vector<float> speed;
    for (int i = 0; i < 60; ++i) speed.push_back(0.0f);
    for (int i = 0; i < 180; ++i) speed.push_back(1.17f);
    for (int i = 0; i < 180; ++i) speed.push_back(3.9f);
    for (int i = 0; i < 90; ++i) speed.push_back(0.0f);
    RootTrack root;
    const MotionClipFrames fr = StyleFrames(speed, root);
    MotionFeatureConfig cfg;
    MotionFeatureSet set;
    set.AddClip("Mixed", fr, root, cfg, MotionClipIntent::FromTags("run;loop"));
    CHECK(set.Style(10) == L::StyleWalk && set.Style(150) == L::StyleWalk && set.Style(330) == L::StyleRun && set.Style(500) == L::StyleRun);
    set.AddClip("OneStyle", fr, root, cfg, MotionClipIntent::FromTags("jog;card"));
    bool allJog = true;
    for (std::uint32_t f = 510; f < set.FrameCount(); ++f) allJog = allJog && set.Style(f) == L::StyleJog;
    CHECK(allJog);

    // Airborne: a flight is a jump only in a jump take (a stride's flight passes the same test).
    std::vector<float> still(240, 0.0f);
    RootTrack r2;
    const MotionClipFrames hop = StyleFrames(still, r2, 100, 130);
    MotionFeatureSet air;
    air.AddClip("JogFlight", hop, r2, cfg, MotionClipIntent::FromTags("jog;card"));
    air.AddClip("Jump", hop, r2, cfg, MotionClipIntent::FromTags("stand;jump"));
    CHECK(!(air.Flags(110) & MotionLabel::Airborne) && (air.Flags(240 + 110) & MotionLabel::Airborne));
}

// Twist flips unrolled (the joints stay put), spikes slerped across, the rest left alone.
void TestGlitchRepair() {
    // root -> thigh -> calf -> foot, each child along its parent's +X (the bones' long axes).
    const std::vector<int> parents = {-1, 0, 1, 2};
    std::vector<LocalTRS> bind(4);
    bind[1].T = glm::vec3(0.1f, 0.0f, 0.0f);
    bind[2].T = glm::vec3(0.45f, 0.0f, 0.0f);
    bind[3].T = glm::vec3(0.42f, 0.0f, 0.0f);
    const int frames = 160;
    std::vector<std::vector<LocalTRS>> clean((size_t)frames, bind);
    for (int f = 0; f < frames; ++f) {
        const float t = (float)f / 60.0f;
        clean[(size_t)f][1].R = glm::angleAxis(0.5f * std::sin(5.0f * t), glm::vec3(0, 0, 1)) * glm::angleAxis(0.1f * t, glm::vec3(1, 0, 0));
        clean[(size_t)f][2].R = glm::angleAxis(0.4f + 0.3f * std::sin(5.0f * t), glm::vec3(0, 0, 1));
        clean[(size_t)f][3].R = glm::angleAxis(0.2f * std::cos(5.0f * t), glm::vec3(0, 1, 0));
    }
    std::vector<std::vector<LocalTRS>> poses = clean;
    // The thigh flips 172 deg about its own axis for frames 40-89, the child taking the inverse (the world pose the same);
    // the calf alone flips for 100-119; the foot spikes 60 deg for frames 130-132.
    const glm::quat flip = glm::angleAxis(3.0f, glm::vec3(1, 0, 0));
    for (int f = 40; f < 90; ++f) {
        poses[(size_t)f][1].R = poses[(size_t)f][1].R * flip;
        poses[(size_t)f][2].R = glm::inverse(flip) * poses[(size_t)f][2].R;
    }
    for (int f = 100; f < 120; ++f) {
        poses[(size_t)f][2].R = poses[(size_t)f][2].R * flip;
        poses[(size_t)f][3].R = glm::inverse(flip) * poses[(size_t)f][3].R;
    }
    for (int f = 130; f < 133; ++f) poses[(size_t)f][3].R = poses[(size_t)f][3].R * glm::angleAxis(1.05f, glm::vec3(0, 1, 0));
    const std::vector<char> repair = {0, 1, 1, 1};
    const MotionBake::RepairStats st = MotionBake::RepairGlitches(parents, bind, repair, 60.0f, poses);
    std::printf("[MotionMatching] repair: %d twist flips, %d spikes (%d frames)\n", st.Flips, st.Spikes, st.SpikeFrames);
    CHECK(st.Flips == 4 && st.Spikes == 1 && st.SpikeFrames == 3);
    float worst = 0.0f, worstPos = 0.0f;
    std::vector<glm::mat4> a, b;
    for (int f = 0; f < frames; ++f) {
        if (f >= 129 && f <= 133) continue; // the spike: slerped, not the original
        for (int i = 1; i < 4; ++i) worst = std::max(worst, MotionMath::QuatAngle(poses[(size_t)f][(size_t)i].R, clean[(size_t)f][(size_t)i].R));
        MotionBake::Globals(parents, poses[(size_t)f], a);
        MotionBake::Globals(parents, clean[(size_t)f], b);
        worstPos = std::max(worstPos, glm::length(glm::vec3(a[3][3]) - glm::vec3(b[3][3])));
    }
    std::printf("[MotionMatching] repair: worst rotation off the clean take %.3f deg, foot %.4f mm\n", worst * kDeg, worstPos * 1000.0f);
    CHECK(worst * kDeg < 1.0f && worstPos < 1e-4f);
    // The spike: back between its neighbours (within the motion of 5 frames).
    for (int f = 130; f < 133; ++f) CHECK(MotionMath::QuatAngle(poses[(size_t)f][3].R, clean[(size_t)f][3].R) * kDeg < 3.0f);
    // Clean data comes back unchanged.
    std::vector<std::vector<LocalTRS>> again = clean;
    const MotionBake::RepairStats none = MotionBake::RepairGlitches(parents, bind, repair, 60.0f, again);
    CHECK(none.Flips == 0 && none.Spikes == 0);
    for (int f = 0; f < frames; ++f)
        for (int i = 1; i < 4; ++i) CHECK(again[(size_t)f][(size_t)i].R == clean[(size_t)f][(size_t)i].R);
    CHECK(none.RolledAfter == none.RolledBefore);
    // The whole leg flipping with the thigh - the calf and the foot riding along (their local rotations untouched), the knee
    // bent 23-40 deg - for frames 40-89: the thigh is turned back and the leg rides back with it. (Counter-rotating the calf, as
    // for a pure twist, left it rolled: the flip back, about the thigh's axis, read as a twist of the bent calf under 90 deg -
    // the pack's Transitions_Movement takes, 2026-11-18.)
    {
        std::vector<std::vector<LocalTRS>> leg = clean;
        for (int f = 40; f < 90; ++f) leg[(size_t)f][1].R = leg[(size_t)f][1].R * flip;
        const MotionBake::RepairStats rs = MotionBake::RepairGlitches(parents, bind, repair, 60.0f, leg);
        float off = 0.0f;
        for (int f = 0; f < frames; ++f)
            for (int i = 1; i < 4; ++i) off = std::max(off, MotionMath::QuatAngle(leg[(size_t)f][(size_t)i].R, clean[(size_t)f][(size_t)i].R));
        std::printf("[MotionMatching] repair, the leg riding the thigh's flip: %d flips, worst rotation off the clean take %.3f deg; "
                    "rolled calf frames %d as captured, %d repaired\n", rs.Flips, off * kDeg, rs.RolledBefore[2], rs.RolledAfter[2]);
        CHECK(rs.Flips == 2 && off * kDeg < 1.0f);
        CHECK(rs.RolledAfter[1] == 0 && rs.RolledAfter[2] == 0 && rs.RolledAfter[3] == 0);
    }
    // A flip over three frames (57 deg each - no one frame over kFlipDeg) and back the same way, the calf counter-rotating:
    // both caught (one frame's threshold caught neither), the thigh back on the clean take throughout.
    {
        std::vector<std::vector<LocalTRS>> slow = clean;
        for (int f = 40; f < 93; ++f) {
            const float k = f < 43 ? (float)(f - 39) / 3.0f : f < 90 ? 1.0f : (float)(92 - f) / 3.0f;
            const glm::quat part = glm::angleAxis(3.0f * k, glm::vec3(1, 0, 0));
            slow[(size_t)f][1].R = slow[(size_t)f][1].R * part;
            slow[(size_t)f][2].R = glm::inverse(part) * slow[(size_t)f][2].R;
        }
        const MotionBake::RepairStats rs = MotionBake::RepairGlitches(parents, bind, repair, 60.0f, slow);
        float off = 0.0f;
        for (int f = 0; f < frames; ++f) off = std::max(off, MotionMath::QuatAngle(slow[(size_t)f][1].R, clean[(size_t)f][1].R));
        std::printf("[MotionMatching] repair, a flip over three frames: %d flips, the thigh's worst off the clean take %.3f deg\n", rs.Flips, off * kDeg);
        CHECK(rs.Flips == 2 && off * kDeg < 1.0f && rs.RolledAfter[1] == 0 && rs.RolledAfter[2] == 0);
    }
}

// Mirroring: the rest pose mirrors to itself (though the right side's bone axes are turned), twice is the identity, and a
// mirrored foot is where the other foot was, reflected.
void TestMirrorPose() {
    const std::vector<std::string> names = {"root", "pelvis", "thigh_l", "foot_l", "thigh_r", "foot_r", "spine_01"};
    const std::vector<int> parents = {-1, 0, 1, 2, 1, 4, 1};
    // The left leg's rest, in the root frame (Z up, the character facing +Y, its left at +X); the right leg its reflection
    // with the bones' axes turned 180 deg about their X (as on the Manny rig).
    const glm::mat4 M = glm::mat4(glm::mat3(glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)));
    const glm::mat4 F = glm::mat4_cast(glm::angleAxis(kPi, glm::vec3(1, 0, 0)));
    std::vector<glm::mat4> g(7, glm::mat4(1.0f));
    g[1] = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0.95f));
    g[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, 0, 0.9f)) * glm::mat4_cast(glm::angleAxis(0.5f * kPi, glm::vec3(0, 1, 0)));
    g[3] = g[2] * glm::translate(glm::mat4(1.0f), glm::vec3(0.85f, 0, 0)) * glm::mat4_cast(glm::angleAxis(-0.4f, glm::vec3(0, 0, 1)));
    g[4] = M * g[2] * M * F;
    g[5] = M * g[3] * M * F;
    g[6] = g[1] * glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0.1f));
    std::vector<LocalTRS> bind(7);
    for (int i = 1; i < 7; ++i) {
        const glm::mat4 local = glm::inverse(g[(size_t)parents[(size_t)i]]) * g[(size_t)i];
        bind[(size_t)i].T = glm::vec3(local[3]);
        bind[(size_t)i].R = glm::normalize(glm::quat_cast(glm::mat3(local)));
    }
    const MotionBake::MirrorRig rig = MotionBake::BuildMirror(names, parents, bind, 0);
    CHECK(std::fabs(std::fabs(rig.Normal.x) - 1.0f) < 1e-6f && rig.Mirror[2] == 4 && rig.Mirror[5] == 3 && rig.Mirror[6] == 6);
    std::vector<LocalTRS> rest = bind;
    MotionBake::MirrorPose(rig, rest);
    float worst = 0.0f;
    for (int i = 1; i < 7; ++i) worst = std::max(worst, MotionMath::QuatAngle(rest[(size_t)i].R, bind[(size_t)i].R));
    CHECK(worst < 1e-4f);
    // A posed frame.
    std::vector<LocalTRS> pose = bind;
    pose[1].T += glm::vec3(0.03f, 0.2f, -0.05f);
    pose[1].R = glm::angleAxis(0.3f, glm::normalize(glm::vec3(0.2f, 0.4f, 1.0f)));
    pose[2].R = pose[2].R * glm::angleAxis(0.6f, glm::normalize(glm::vec3(0, 1, 0.3f)));
    pose[3].R = pose[3].R * glm::angleAxis(-0.5f, glm::vec3(0, 0, 1));
    pose[4].R = pose[4].R * glm::angleAxis(-0.2f, glm::vec3(0, 1, 0));
    pose[6].R = glm::angleAxis(0.25f, glm::vec3(0, 0, 1));
    std::vector<LocalTRS> m = pose;
    MotionBake::MirrorPose(rig, m);
    std::vector<glm::mat4> gp, gm;
    MotionBake::Globals(parents, pose, gp);
    MotionBake::Globals(parents, m, gm);
    const float footL = glm::length(glm::vec3(gm[3][3]) - glm::vec3(M * gp[5][3]));
    const float footR = glm::length(glm::vec3(gm[5][3]) - glm::vec3(M * gp[3][3]));
    const float pelvis = glm::length(glm::vec3(gm[1][3]) - glm::vec3(M * gp[1][3]));
    CHECK(footL < 1e-5f && footR < 1e-5f && pelvis < 1e-5f);
    std::vector<LocalTRS> twice = m;
    MotionBake::MirrorPose(rig, twice);
    worst = 0.0f;
    float worstT = 0.0f;
    for (int i = 1; i < 7; ++i) {
        worst = std::max(worst, MotionMath::QuatAngle(twice[(size_t)i].R, pose[(size_t)i].R));
        worstT = std::max(worstT, glm::length(twice[(size_t)i].T - pose[(size_t)i].T));
    }
    CHECK(worst < 1e-4f && worstT < 1e-5f);
}

void TestTakesCsv() {
    const fs::path p = MmTemp() / "takes.csv";
    std::ofstream(p) << "# comment\nname,group,pattern,take,seconds,frames,include,trim_start,trim_end,tags,notes\n"
                        "Walk_Loop_Take001,Walk,Walk_Loop,1,177.7,10662,1,1.000,91.000,walk;loop;forward,\n"
                        "Walk_Loop_S_Take001,Walk,Walk_Loop,1,233.0,13980,0,1.000,0.000,walk;loop;forward;exaggerated,exaggerated\r\n";
    std::vector<MotionTake> t;
    std::string err;
    CHECK(MotionBake::ReadTakes(p.string(), t, err));
    CHECK(t.size() == 2);
    if (t.size() == 2) {
        CHECK(t[0].Include && !t[1].Include);
        CHECK(t[0].TrimEnd == 91.0f && t[0].HasTag("forward") && t[1].HasTag("exaggerated"));
        CHECK(t[0].TagString() == "walk;loop;forward");
    }
    // A mirrored row names its source take.
    std::ofstream(p) << "name,source,group,include,trim_start,trim_end,tags\n"
                        "Jog_Strafe_Star_Take001_Mirror,Jog_Strafe_Star_Take001,Jog,1,1.000,0.000,jog;strafe;mirrored\n";
    std::vector<MotionTake> mt;
    CHECK(MotionBake::ReadTakes(p.string(), mt, err) && mt.size() == 1);
    if (mt.size() == 1) CHECK(mt[0].Mirrored() && mt[0].File() == "Jog_Strafe_Star_Take001" && mt[0].Name == "Jog_Strafe_Star_Take001_Mirror");
    // The repo's own list parses: the exaggerated takes and the dodges out, every other take in, each mirror naming a take
    // that is in.
    const std::string repo = ProjectPaths::Resolve("../tools/mm/takes.csv");
    std::vector<MotionTake> all;
    if (!fs::exists(repo)) return Skip("takes.csv", "tools/mm/takes.csv not found");
    CHECK(MotionBake::ReadTakes(repo, all, err));
    int included = 0, mirrors = 0;
    for (const MotionTake& m : all) {
        included += m.Include ? 1 : 0;
        if (m.HasTag("exaggerated") || m.HasTag("dodge")) CHECK(!m.Include);
        else CHECK(m.Include);
        if (!m.Mirrored()) continue;
        ++mirrors;
        // (Its source: the unmirrored row baked from the same file - a pack take by name, a project clip by its path.)
        const bool sourceIn = std::any_of(all.begin(), all.end(), [&](const MotionTake& o) { return !o.Mirrored() && o.File() == m.File() && o.Include; });
        CHECK(sourceIn && m.HasTag("strafe"));
    }
    CHECK(all.size() > 139 && included > 100 && mirrors > 0);
}

// Needs the pack (TARTARUS_MM_SOURCE) and the Quantum body: the rest pose and bone lengths of the core bones agree.
void TestRigDiffWithPack() {
    const char* name = "Motion matching: pack rig vs Quantum rest pose";
    MotionBakeConfig cfg;
    std::string err;
    const std::string cfgPath = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny.mmconfig");
    if (!cfg.Load(cfgPath, err)) return Skip(name, err);
    const std::string source = MotionBake::SourceDir();
    if (source.empty()) return Skip(name, "TARTARUS_MM_SOURCE not set");
    const fs::path rig = fs::path(source) / cfg.SourceRig;
    if (!fs::exists(rig) || !fs::exists(cfg.TargetRig)) return Skip(name, "rig files missing");
    ModelImportSettings s;
    s.AnimationOnly = true;
    s.ImportNormals = false;
    s.MaterialImportMode = ModelImportSettings::MaterialMode::None;
    Model a(rig.string(), s), b(cfg.TargetRig, s);
    float worst = 0.0f;
    int missing = 0;
    for (const MotionBake::BoneDiff& d : MotionBake::RigDiff(a, b, cfg.Bones)) {
        missing += d.InSource && d.InTarget ? 0 : 1;
        worst = std::max(worst, d.LengthPct);
    }
    std::printf("[MotionMatching] rig diff: worst core bone length %.3f%%, %d missing\n", worst, missing);
    CHECK(missing == 0);
    CHECK(worst <= cfg.RigLengthPct);
}

// Needs the cooked stream (--bake-motion): it loads inside the budgets.
void TestCookedStreamBudgets() {
    const char* name = "Motion matching: cooked stream budgets";
    const std::string base = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny");
    if (!fs::exists(base + ".mmanim") || !fs::exists(base + ".mmdb")) return Skip(name, "Manny.mmanim / .mmdb not baked");
    const auto t0 = std::chrono::steady_clock::now();
    MotionClipStream s;
    MotionFeatureSet db;
    std::string err;
    CHECK(s.Load(base + ".mmanim", &err));
    CHECK(db.Load(base + ".mmdb", &err));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double poseMB = (double)s.PoseBytes() / 1048576.0, featMB = (double)db.Bytes() / 1048576.0;
    std::printf("[MotionMatching] cooked: %zu clips, %u frames, poses %.1f MB, features %.1f MB, load %.2f s\n", s.Clips().size(),
                s.FrameCount(), poseMB, featMB, secs);
    CHECK(s.FrameCount() == db.FrameCount() && s.Clips().size() == db.Clips().size());
    // The caps are the bake's own (Manny.mmconfig).
    MotionBakeConfig cfg;
    CHECK(cfg.Load(ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny.mmconfig"), err));
    CHECK(poseMB <= cfg.BudgetPoseMB && featMB <= cfg.BudgetFeatureMB && secs <= cfg.BudgetLoadSeconds);
}

} // namespace

void RegisterMotionMatchingTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Motion matching: smoothing, unwrap, smallest-three", TestMotionMath});
    tests.push_back({"Motion matching: RootSynth on a swaying sine path", TestRootSynthSinePath});
    tests.push_back({"Motion matching: RootSynth turning in place", TestRootSynthTurnInPlace});
    tests.push_back({"Motion matching: foot contacts and slide", TestRootSynthContactsAndSlide});
    tests.push_back({"Motion matching: pose stream round trip and binding", TestClipStreamRoundTrip});
    tests.push_back({"Motion matching: features and labels", TestFeaturesAndLabels});
    tests.push_back({"Motion matching: takes.csv", TestTakesCsv});
    tests.push_back({"Motion matching: 32-bit quaternions and twist angles", TestPackQuat32});
    tests.push_back({"Motion matching: finger tracks", TestFingerTracks});
    tests.push_back({"Motion matching: style and jump intent", TestStyleIntent});
    tests.push_back({"Motion matching: glitch repair (twist flips, spikes)", TestGlitchRepair});
    tests.push_back({"Motion matching: mirrored poses", TestMirrorPose});
    tests.push_back({"Motion matching: pack rig vs Quantum rest pose", TestRigDiffWithPack});
    tests.push_back({"Motion matching: cooked stream budgets", TestCookedStreamBudgets});
}
