// --npc-test mmview (docs/MOTION_MATCHING_HANDOFF.md, Phase 2): the baked motion matching takes played on a bare Quantum
// body in LocomotionLab's open floor, with the generated root drawn, and the bake checked.
//
// At the start, every cooked clip (Manny.mmanim + Manny.mmdb) is checked offline on the Quantum rig: the stream binds,
// the budgets hold, the root never jumps, its yaw doesn't wobble (RootSynth::Validate's jitter), it faces where the hips
// face on the forward takes (p95 under 20 degrees), the planted feet rebuilt from the stream and the root stay put, the
// .mmdb's foot features match the rebuilt feet, and the labels make sense (runs have run frames, jumps airborne ones,
// crouches crouched ones, idles idle ones).
// Then a playlist of takes plays, 7 s each, re-centred on one spot in front of the player: the body posed from the stream,
// its entity placed by the generated root. Drawn (the Game view's HUD lines): the root's path (past grey, ahead yellow),
// the root's facing (green), the hips' facing (red), each ankle (blue while planted, white in the air), and a cyan
// tick while the take is labelled airborne.
// TARTARUS_MMVIEW_CLIPS='Name[@start seconds];...' picks the takes. With nothing baked (CI) it SKIPs.
#include "NpcTest.h"

#include "AssetLibrary.h"
#include "Components.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "FirstPersonBodyMotion.h"
#include "FirstPersonPresentation.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"
#include "../MotionMatching/MotionBake.h"
#include "../MotionMatching/MotionClipStream.h"
#include "../MotionMatching/MotionFeatures.h"
#include "../MotionMatching/MotionMath.h"
#include "../MotionMatching/RootSynth.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace {

constexpr float kClipSeconds = 7.0f;
const glm::vec3 kStage(-14.0f, 0.0f, -12.0f);    // where each take is centred: LocomotionLab's empty south-west quarter
const glm::vec3 kPlayerAt(-14.0f, 0.05f, -18.0f); // the player stands 6 m south of it and turns to follow the body

struct PlayItem {
    std::string Name;
    float Start = 0.0f;
};

std::vector<PlayItem> Playlist() {
    std::vector<PlayItem> out;
    const std::string env = Env::Str(Env::Var::MmViewClips);
    if (!env.empty()) {
        std::stringstream ss(env);
        for (std::string item; std::getline(ss, item, ';');) {
            if (item.empty()) continue;
            PlayItem p;
            const size_t at = item.find('@');
            p.Name = item.substr(0, at);
            if (at != std::string::npos) p.Start = (float)std::atof(item.c_str() + at + 1);
            out.push_back(p);
        }
        if (!out.empty()) return out;
    }
    // One of each kind: curves, a strafe, fast snakes, a turn in place, a start, a crouch, both jumps, the _FO idle,
    // a transition (the two unexplained suffixes, _FO and _M_L, are here for the owner to look at).
    return {{"Walk_Circles_Take001", 6.0f},       {"Jog_Strafe_Square_Take001", 6.0f}, {"Run_Snakes_Take001", 20.0f},
            {"Stand_Turn_360_Take001", 6.0f},     {"Stand_to_Run_Fixed_Take001", 4.0f}, {"Crouch_Snakes_Take001", 10.0f},
            {"Stand_Jump_InPlace_Take001", 3.0f}, {"Stand_Jump_360_M_L_Take001", 8.0f}, {"Stand_Idle_Neutral_FO_Take001", 5.0f},
            {"Transitions_Movement_Take001", 4.0f}};
}

// Model-space transforms of a few nodes from a local pose, walking only their ancestors (the full rig is ~350 nodes).
struct FkChain {
    std::vector<int> Order; // the needed nodes, parents first
    std::vector<int> Parent;
    void Build(const Model& m, const std::vector<int>& targets) {
        std::vector<char> need((size_t)m.NodeCount(), 0);
        for (int t : targets)
            for (int i = t; i >= 0; i = m.NodeParent(i)) need[(size_t)i] = 1;
        Order.clear();
        for (int i = 0; i < m.NodeCount(); ++i)
            if (need[(size_t)i]) Order.push_back(i);
        Parent.resize((size_t)m.NodeCount());
        for (int i = 0; i < m.NodeCount(); ++i) Parent[(size_t)i] = m.NodeParent(i);
    }
    void Run(const std::vector<LocalTRS>& pose, std::vector<glm::mat4>& g) const {
        g.resize(pose.size());
        for (int i : Order) {
            const glm::mat4 local = pose[(size_t)i].ToMatrix();
            const int p = Parent[(size_t)i];
            g[(size_t)i] = p >= 0 ? AffineMul(g[(size_t)p], local) : local;
        }
    }
};

void Line(std::vector<float>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
    out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
}

} // namespace

struct NpcTest::MmViewRun {
    bool Started = false, Skipped = false, Validated = false;
    MotionClipStream Stream;
    MotionFeatureSet Db;
    std::shared_ptr<Model> Body;
    entt::entity Entity = entt::null;
    MotionClipStream::Binding Bind;
    std::vector<LocalTRS> BindPose, Pose;
    FkChain Fk;
    int Thigh[2] = {-1, -1}, Foot[2] = {-1, -1}, Pelvis = -1;
    std::vector<PlayItem> Items;
    std::vector<int> ItemClip;
    std::vector<glm::vec3> ItemOffset;
    float Clock = 0.0f;
    int Item = -1, Frames = 0, Finite = 0;
    glm::vec3 Watch = kStage + glm::vec3(0.0f, 0.9f, 0.0f); // the body's pelvis (world): the views follow it
    std::vector<float> Lines;
    MotionFeatureConfig Labels;
};

float NpcTest::MmViewSeconds() { return (float)Playlist().size() * kClipSeconds + 1.5f; }

void NpcTest::MmViewDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    if (!m_Mm) m_Mm = std::make_shared<MmViewRun>();
    MmViewRun& r = *m_Mm;
    if (!r.Started) {
        PhysicsWorld::SetCharacterFootPosition(&kPlayerAt.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kPlayerAt + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        weapon.SetEquipped(false); // unarmed: nothing in front of the body
    }
    // Follow the body like a camera operator: aim a little under the pelvis, so the feet and the root's lines show.
    const glm::vec3 d = r.Watch - glm::vec3(0.0f, 0.35f, 0.0f) - player.Cam.Position;
    wantYaw = glm::degrees(std::atan2(d.z, d.x));
    wantPitch = glm::degrees(std::atan2(d.y, std::max(0.5f, glm::length(glm::vec2(d.x, d.z)))));
    r.Clock += dt;
}

void NpcTest::MmViewValidate() {
    MmViewRun& r = *m_Mm;
    const MotionClipStream& s = r.Stream;
    const MotionFeatureSet& db = r.Db;
    constexpr float kDeg = 180.0f / MotionMath::kPi;
    std::printf("[MmView] %-46s %6s %6s %6s %7s %6s %6s %6s %7s  %s\n", "clip", "frames", "step", "fac95", "jitter", "wobble",
                "slideL", "slideR", "featCm", "gait i/w/j/r crouch strafe air");
    float worstStep = 0.0f, worstFacing = 0.0f, worstJitter = 0.0f, worstSlide = 0.0f, worstFeat = 0.0f, worstSlideGap = 0.0f;
    std::string worstStepAt, worstFacingAt, worstJitterAt, worstSlideAt, worstFeatAt, worstSlideGapAt;
    int labelFails = 0;
    std::vector<glm::mat4> g;
    std::vector<LocalTRS> pose;
    for (size_t c = 0; c < s.Clips().size(); ++c) {
        const MotionClipStream::Clip& clip = s.Clips()[c];
        const size_t n = clip.Count;
        RootTrack root;
        std::vector<glm::vec3> hips(n), feet[2] = {std::vector<glm::vec3>(n), std::vector<glm::vec3>(n)};
        std::vector<glm::vec3> source[2] = {std::vector<glm::vec3>(n), std::vector<glm::vec3>(n)}; // the .mmdb's (the take's own)
        float feat = 0.0f, step = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const std::uint32_t f = clip.First + (std::uint32_t)i;
            pose = r.BindPose;
            s.FramePose(f, r.Bind, pose);
            r.Fk.Run(pose, g);
            const MotionClipStream::RootSample& rs = s.Root(f);
            root.Position.push_back(rs.Position);
            root.Yaw.push_back(rs.Yaw);
            if (i > 0) step = std::max(step, glm::length(rs.Position - s.Root(f - 1).Position));
            const glm::mat4 R = RootSynth::RootMatrix(rs.Position, rs.Yaw, s.RestYaw);
            hips[i] = glm::vec3(R * (g[(size_t)r.Thigh[1]][3] - g[(size_t)r.Thigh[0]][3]));
            for (int k = 0; k < 2; ++k) {
                const glm::vec3 local(g[(size_t)r.Foot[k]][3]); // the body under its root: the root's own space
                feet[k][i] = glm::vec3(R * glm::vec4(local, 1.0f));
                const float* ft = db.Features(f) + 3 * k;
                feat = std::max(feat, glm::length(local - glm::vec3(ft[0], ft[1], ft[2])) * 100.0f);
                source[k][i] = glm::vec3(R * glm::vec4(ft[0], ft[1], ft[2], 1.0f));
            }
        }
        std::vector<char> contacts[2];
        for (int k = 0; k < 2; ++k) contacts[k] = RootSynth::Contacts(source[k], s.Fps(), r.Labels.ContactHeight, r.Labels.ContactSpeed);
        const RootValidation v = RootSynth::Validate(root, s.Fps(), hips, feet, contacts);
        const RootValidation src = RootSynth::Validate(root, s.Fps(), hips, source, contacts);
        const float slideGap = std::max(std::fabs(v.FootSlide[0] - src.FootSlide[0]), std::fabs(v.FootSlide[1] - src.FootSlide[1]));
        int gait[4] = {0, 0, 0, 0}, crouch = 0, strafe = 0, air = 0;
        for (std::uint32_t f = clip.First; f < clip.First + clip.Count; ++f) {
            ++gait[std::min<int>(3, db.Gait(f))];
            crouch += db.Flags(f) & MotionLabel::Crouch ? 1 : 0;
            strafe += db.Flags(f) & MotionLabel::Strafe ? 1 : 0;
            air += db.Flags(f) & MotionLabel::Airborne ? 1 : 0;
        }
        const bool forward = ("|" + clip.Tags + "|").find("forward") != std::string::npos;
        const float slide = std::max(v.FootSlide[0], v.FootSlide[1]);
        std::printf("[MmView] %-46s %6zu %6.3f %6.1f %7.1f %6.2f %6.1f %6.1f %7.3f  %d/%d/%d/%d %d %d %d%s\n", clip.Name.c_str(), n,
                    step, v.FacingP95, v.YawJitter, v.PathWobble, v.FootSlide[0], v.FootSlide[1], feat, gait[0], gait[1], gait[2], gait[3], crouch,
                    strafe, air, forward ? "  forward" : "");
        if (step > worstStep) { worstStep = step; worstStepAt = clip.Name; }
        if (forward && v.FacingP95 > worstFacing) { worstFacing = v.FacingP95; worstFacingAt = clip.Name; }
        if (v.YawJitter > worstJitter) { worstJitter = v.YawJitter; worstJitterAt = clip.Name; }
        if (slide > worstSlide) { worstSlide = slide; worstSlideAt = clip.Name; }
        if (slideGap > worstSlideGap) { worstSlideGap = slideGap; worstSlideGapAt = clip.Name; }
        if (feat > worstFeat) { worstFeat = feat; worstFeatAt = clip.Name; }
        // Labels that must be there.
        const auto has = [&](const char* p) { return clip.Name.rfind(p, 0) == 0; };
        bool ok = true;
        if (has("Run_")) ok = gait[MotionLabel::Run] + gait[MotionLabel::Jog] > 0;
        if (has("Stand_Jump_InPlace") || has("Stand_Jump_360_M_L")) ok = air > 0;
        if (has("Crouch_") && !has("Crouch_to_")) ok = crouch * 3 > (int)n;
        if (has("Stand_Idle_")) ok = gait[MotionLabel::Idle] * 100 >= (int)n * 95 && air == 0;
        if (!ok) {
            ++labelFails;
            std::printf("[MmView]   labels don't fit %s\n", clip.Name.c_str());
        }
    }
    char b[256];
    std::snprintf(b, sizeof b, "the root never jumps: worst frame step %.3f m (%s), under 0.15", worstStep, worstStepAt.c_str());
    Check(worstStep < 0.15f, b);
    std::snprintf(b, sizeof b, "the root faces where the hips do on forward takes: worst p95 %.1f deg (%s), under 20", worstFacing,
                  worstFacingAt.c_str());
    Check(worstFacing < 20.0f, b);
    std::snprintf(b, sizeof b, "the root's yaw doesn't wobble: worst jitter %.0f (%s), under 300", worstJitter, worstJitterAt.c_str());
    Check(worstJitter < 300.0f, b);
    // The planted feet, rebuilt from the stream on the Quantum rig and placed by the root, slide no more than the take's
    // own feet (the .mmdb's, from the source rig) - the mocap's own heel-to-toe roll under the contact rule is ~5-15 cm/s.
    std::snprintf(b, sizeof b, "planted feet slide as in the take (root space rebuilt): worst gap %.2f cm/s (%s), under 0.5; "
                  "worst slide %.1f cm/s (%s)", worstSlideGap, worstSlideGapAt.c_str(), worstSlide, worstSlideAt.c_str());
    Check(worstSlideGap < 0.5f, b);
    std::snprintf(b, sizeof b, "the .mmdb's foot features match the stream: worst %.3f cm (%s), under 1", worstFeat, worstFeatAt.c_str());
    Check(worstFeat < 1.0f, b);
    Check(labelFails == 0, "every take's labels fit it (" + std::to_string(labelFails) + " don't)");
    (void)kDeg;
}

void NpcTest::MmView(World& world) {
    if (!m_Mm) m_Mm = std::make_shared<MmViewRun>();
    MmViewRun& r = *m_Mm;
    if (m_Done) return;
    if (!r.Started) {
        r.Started = true;
        const std::string base = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny");
        MotionBakeConfig cfg;
        std::string err;
        if (!std::filesystem::exists(base + ".mmanim") || !std::filesystem::exists(base + ".mmdb")) {
            std::printf("[NpcTest] SKIP mmview: %s.mmanim / .mmdb not baked (--bake-motion)\n", base.c_str());
            r.Skipped = m_Done = true;
            return;
        }
        if (!cfg.Load(ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny.mmconfig"), err) ||
            !std::filesystem::exists(cfg.TargetRig) || !Assets) {
            std::printf("[NpcTest] SKIP mmview: %s\n", err.empty() ? "the target rig is missing" : err.c_str());
            r.Skipped = m_Done = true;
            return;
        }
        r.Labels = cfg.Labels;
        const auto t0 = std::chrono::steady_clock::now();
        const bool loaded = r.Stream.Load(base + ".mmanim", &err) && r.Db.Load(base + ".mmdb", &err);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Check(loaded, "the stream and features load" + (loaded ? std::string() : ": " + err));
        if (!loaded) { m_Done = true; return; }
        const double poseMB = (double)r.Stream.PoseBytes() / 1048576.0, featMB = (double)r.Db.Bytes() / 1048576.0;
        char b[256];
        std::snprintf(b, sizeof b, "budgets: %zu clips, %.2f h, poses %.1f MB (150), features %.1f MB (40), load %.2f s (2)",
                      r.Stream.Clips().size(), (double)r.Stream.FrameCount() / r.Stream.Fps() / 3600.0, poseMB, featMB, secs);
        Check(poseMB <= 150.0 && featMB <= 40.0 && secs <= 2.0, b);
        Check(r.Stream.FrameCount() == r.Db.FrameCount() && r.Stream.Clips().size() == r.Db.Clips().size(),
              "the stream and the features cover the same frames");

        r.Body = Assets->LoadModel(cfg.TargetRig);
        if (!r.Body || r.Body->NodeCount() == 0) {
            Check(false, "the Quantum body loads");
            m_Done = true;
            return;
        }
        r.Body = r.Body->CreateInstance();
        r.Body->BindLocalPose(r.BindPose);
        {
            std::vector<glm::mat4> g;
            MotionBake::RootSpaceGlobals(*r.Body, r.BindPose, -1, g);
            const int p = r.Body->NodeIndex("pelvis");
            r.Bind = r.Stream.Bind(*r.Body, p >= 0 ? g[(size_t)p][3].y : 0.0f);
        }
        int statics = 0;
        for (int n : r.Bind.StaticNodes) statics += n >= 0 ? 1 : 0;
        std::snprintf(b, sizeof b, "every streamed bone is on the Quantum body (%d of %zu, %d of %zu static), pelvis scale %.4f",
                      r.Bind.Bound, r.Stream.Bones().size(), statics, r.Stream.Statics.size(), r.Bind.PelvisScale);
        Check(r.Bind.Bound == (int)r.Stream.Bones().size() && statics == (int)r.Stream.Statics.size() && r.Bind.Pelvis >= 0, b);
        r.Pelvis = r.Bind.Pelvis;
        r.Thigh[0] = r.Body->NodeIndex("thigh_l");
        r.Thigh[1] = r.Body->NodeIndex("thigh_r");
        r.Foot[0] = r.Body->NodeIndex("foot_l");
        r.Foot[1] = r.Body->NodeIndex("foot_r");
        r.Fk.Build(*r.Body, {r.Thigh[0], r.Thigh[1], r.Foot[0], r.Foot[1], r.Pelvis});
        const auto v0 = std::chrono::steady_clock::now();
        MmViewValidate();
        std::printf("[MmView] validated %u frames in %.2f s\n", r.Stream.FrameCount(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - v0).count());

        r.Entity = world.CreateModelEntity(r.Body, kStage, glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] MM View Body");
        Assets->ApplyMaterialRemap(*r.Body, world.Registry.get<RenderableComponent>(r.Entity).Materials);
        // The playlist: each take re-centred on the stage over the part that plays.
        r.Items = Playlist();
        int found = 0;
        for (const PlayItem& it : r.Items) {
            const int c = r.Stream.FindClip(it.Name);
            r.ItemClip.push_back(c);
            glm::vec3 lo(1e9f), hi(-1e9f);
            if (c >= 0)
                for (float t = it.Start; t <= it.Start + kClipSeconds; t += 0.25f) {
                    const glm::vec3 p = r.Stream.RootAt(c, t).Position;
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
            r.ItemOffset.push_back(c >= 0 ? kStage - 0.5f * (lo + hi) * glm::vec3(1, 0, 1) : kStage);
            found += c >= 0 ? 1 : 0;
            if (c < 0) std::printf("[MmView] playlist: no clip named %s\n", it.Name.c_str());
        }
        Check(found == (int)r.Items.size(), "every playlist take is in the stream (" + std::to_string(found) + " of " +
                                                std::to_string(r.Items.size()) + ")");
        m_HaveCam = true; // the Scene view follows the body (below)
    }
    if (r.Skipped || !r.Body) return;

    // Playback.
    const int item = std::min((int)r.Items.size() - 1, (int)(r.Clock / kClipSeconds));
    if (r.Clock >= (float)r.Items.size() * kClipSeconds) {
        m_Done = true;
        char b[160];
        std::snprintf(b, sizeof b, "played %zu takes, %d frames, every pose finite (%d)", r.Items.size(), r.Frames, r.Finite);
        Check(r.Frames > 0 && r.Finite == r.Frames, b);
        return;
    }
    const int clip = r.ItemClip[(size_t)item];
    if (item != r.Item) {
        r.Item = item;
        std::printf("[MmView] %5.1f s  playing %s from %.1f s (tags %s)\n", r.Clock, r.Items[(size_t)item].Name.c_str(),
                    r.Items[(size_t)item].Start, clip >= 0 ? r.Stream.Clips()[(size_t)clip].Tags.c_str() : "-");
    }
    r.Lines.clear();
    if (clip < 0) return;
    const float t = r.Items[(size_t)item].Start + (r.Clock - (float)item * kClipSeconds);
    const glm::vec3 offset = r.ItemOffset[(size_t)item];
    r.Pose = r.BindPose;
    r.Stream.SamplePose(clip, t, r.Bind, r.Pose);
    r.Body->ApplyLocalPose(r.Pose);
    const MotionClipStream::RootSample rs = r.Stream.RootAt(clip, t);
    world.SetWorldPose(r.Entity, offset + rs.Position,
                       glm::angleAxis(rs.Yaw - r.Stream.RestYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    // The Scene view: three-quarters from behind-left of the stage's south-west, 4.5 m off the root, following it.
    r.Watch = glm::vec3(glm::translate(glm::mat4(1.0f), offset) * RootSynth::RootMatrix(rs.Position, rs.Yaw, r.Stream.RestYaw) *
                        glm::vec4(0.0f, 0.9f, 0.0f, 1.0f));
    {
        const glm::vec3 look = offset + rs.Position + glm::vec3(0.0f, 0.75f, 0.0f);
        m_CamPos = look + glm::vec3(-3.0f, 1.9f, -3.2f);
        const glm::vec3 d = glm::normalize(look - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    }
    ++r.Frames;
    bool finite = true;
    for (const LocalTRS& l : r.Pose) finite = finite && std::isfinite(l.R.w) && std::isfinite(l.T.x) && std::isfinite(l.T.y);
    r.Finite += finite ? 1 : 0;

    // The lines: the root's path 2 s back (grey) and 1 s ahead (yellow), its facing (green), the hips' (red), the ankles.
    const MotionClipStream::Clip& c = r.Stream.Clips()[(size_t)clip];
    const float lift = 0.02f;
    for (float k = -2.0f; k < 1.0f; k += 1.0f / 30.0f) {
        if (t + k < 0.0f || t + k + 1.0f / 30.0f > r.Stream.ClipSeconds(clip)) continue;
        const glm::vec3 a = offset + r.Stream.RootAt(clip, t + k).Position, b = offset + r.Stream.RootAt(clip, t + k + 1.0f / 30.0f).Position;
        Line(r.Lines, a + glm::vec3(0, lift, 0), b + glm::vec3(0, lift, 0),
             k < 0.0f ? glm::vec4(0.6f, 0.6f, 0.6f, 0.8f) : glm::vec4(1.0f, 0.85f, 0.1f, 0.95f));
    }
    const glm::vec3 at = offset + rs.Position + glm::vec3(0, lift, 0);
    Line(r.Lines, at, at + MotionMath::YawDir(rs.Yaw) * 0.7f, glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    Line(r.Lines, at - glm::vec3(0.12f, 0, 0), at + glm::vec3(0.12f, 0, 0), glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    Line(r.Lines, at - glm::vec3(0, 0, 0.12f), at + glm::vec3(0, 0, 0.12f), glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    std::vector<glm::mat4> g;
    r.Fk.Run(r.Pose, g);
    const glm::mat4 W = glm::translate(glm::mat4(1.0f), offset) * RootSynth::RootMatrix(rs.Position, rs.Yaw, r.Stream.RestYaw);
    const glm::vec3 hipsR = glm::vec3(W * (g[(size_t)r.Thigh[1]][3] - g[(size_t)r.Thigh[0]][3]));
    const glm::vec3 hf = RootSynth::ForwardFromRight(hipsR);
    const glm::vec3 pelvisG = glm::vec3(W * g[(size_t)r.Pelvis][3]);
    const glm::vec3 pelvisFloor(pelvisG.x, lift, pelvisG.z);
    Line(r.Lines, pelvisFloor, pelvisFloor + hf * 0.5f, glm::vec4(1.0f, 0.25f, 0.2f, 1.0f));
    Line(r.Lines, pelvisFloor, pelvisG, glm::vec4(1.0f, 0.25f, 0.2f, 0.35f));
    const std::uint32_t frame = c.First + (std::uint32_t)std::clamp((int)std::lround(t * r.Stream.Fps()), 0, (int)c.Count - 1);
    const std::uint8_t flags = r.Db.Flags(frame);
    for (int k = 0; k < 2; ++k) {
        const glm::vec3 f = glm::vec3(W * g[(size_t)r.Foot[k]][3]);
        const bool planted = flags & (k == 0 ? MotionLabel::ContactL : MotionLabel::ContactR);
        const glm::vec4 col = planted ? glm::vec4(0.2f, 0.5f, 1.0f, 1.0f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.8f);
        Line(r.Lines, f - glm::vec3(0.07f, 0, 0), f + glm::vec3(0.07f, 0, 0), col);
        Line(r.Lines, f - glm::vec3(0, 0, 0.07f), f + glm::vec3(0, 0, 0.07f), col);
        Line(r.Lines, f, glm::vec3(f.x, lift, f.z), glm::vec4(col.r, col.g, col.b, 0.4f));
    }
    if (flags & MotionLabel::Airborne) Line(r.Lines, pelvisG, pelvisG + glm::vec3(0, 0.6f, 0), glm::vec4(0.1f, 1.0f, 1.0f, 1.0f));
    // A log line every second: what the labels say.
    if ((int)(r.Clock * 60.0f) % 60 == 0)
        std::printf("[MmView] %5.1f s  %s %.1f s: speed %.2f m/s, gait %s%s%s%s\n", r.Clock, c.Name.c_str(), t,
                    glm::length(r.Stream.RootAt(clip, t + 0.05f).Position - r.Stream.RootAt(clip, t - 0.05f).Position) / 0.1f,
                    MotionLabel::GaitName(r.Db.Gait(frame)), flags & MotionLabel::Crouch ? " crouch" : "",
                    flags & MotionLabel::Strafe ? " strafe" : "", flags & MotionLabel::Airborne ? " airborne" : "");
}

void NpcTest::DebugLines(std::vector<float>& out) const {
    if (m_Mm) out.insert(out.end(), m_Mm->Lines.begin(), m_Mm->Lines.end());
    MmDbLines(out); // NpcTest_MmDb.cpp
    if (m_Scenario == "mmplayer" || m_Scenario == "mmstress") { // the player's matcher (FirstPersonBodyMotion)
        const auto& lines = MmPlayerDebug::Info().Lines;
        out.insert(out.end(), lines.begin(), lines.end());
    }
}
