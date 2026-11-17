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

#include <glm/gtc/quaternion.hpp>

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
    db.AddClip("Synthetic", fr, root, cfg);
    for (glm::vec3& p : fr.Pelvis) p.y = 0.6f;
    db.AddClip("Crouched", fr, root, cfg);
    db.Finish();
    CHECK(db.FrameCount() == 720 && db.Clips().size() == 2 && db.Clips()[1].First == 360);
    const float* f = db.Features(30);
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
    CHECK(l.Features(30)[20] == f[20] && l.Flags(250) == db.Flags(250) && l.Mean()[20] == db.Mean()[20]);
    CHECK(l.Std()[0] > 0.0f);
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
    // The repo's own list parses and leaves the exaggerated takes out.
    const std::string repo = ProjectPaths::Resolve("../tools/mm/takes.csv");
    std::vector<MotionTake> all;
    if (!fs::exists(repo)) return Skip("takes.csv", "tools/mm/takes.csv not found");
    CHECK(MotionBake::ReadTakes(repo, all, err));
    int included = 0;
    for (const MotionTake& m : all) {
        included += m.Include ? 1 : 0;
        if (m.HasTag("exaggerated")) CHECK(!m.Include);
    }
    CHECK(all.size() == 139 && included > 50);
}

// Needs the pack (TARTARUS_MM_SOURCE) and the Quantum body: the rest pose and bone lengths of the core bones agree.
void TestRigDiffWithPack() {
    const char* name = "Motion matching: pack rig vs Quantum rest pose";
    MotionBakeConfig cfg;
    std::string err;
    const std::string cfgPath = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny.mmconfig");
    if (!cfg.Load(cfgPath, err)) return Skip(name, err);
    const std::string source = MotionBake::SourceDir(cfg);
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
    CHECK(poseMB <= 150.0 && featMB <= 40.0 && secs <= 2.0);
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
    tests.push_back({"Motion matching: pack rig vs Quantum rest pose", TestRigDiffWithPack});
    tests.push_back({"Motion matching: cooked stream budgets", TestCookedStreamBudgets});
}
