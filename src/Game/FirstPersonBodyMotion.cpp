#include "FirstPersonBodyMotion.h"

#include "Animation.h"
#include "Core/Env.h"
#include "Core/Paths.h"
#include "Model.h"
#include "MotionMatching/MotionBake.h"
#include "MotionMatching/MotionDebugDraw.h"
#include "MotionMatching/MotionMath.h"
#include "MotionMatching/MotionSearch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace {
constexpr float kPi = 3.14159265358979f;
float Wrap(float a) { return MotionMath::WrapPi(a); }
// The share of the way a spring with half-life `h` closes in `dt`.
float Toward(float dt, float h) { return h > 1e-5f ? 1.0f - std::exp(-0.69314718f * dt / h) : 1.0f; }
} // namespace

// --- the shared database ---------------------------------------------------------------------------------------------

std::vector<float> SharedMotionDatabase::HandOverShoulder(const MotionDatabase& db, const Model& rig, std::vector<float>* chestTilt) {
    const MotionClipStream& stream = db.Stream();
    std::vector<float> out(db.FrameCount(), -1.0f);
    std::vector<LocalTRS> bind;
    rig.BindLocalPose(bind);
    std::vector<glm::mat4> g;
    MotionBake::RootSpaceGlobals(rig, bind, -1, g);
    const auto binding = stream.Bind(rig, 0.0f);
    // The nodes from the hands up to the top: posed per frame, parents first (a node's index is above its parent's).
    std::vector<int> nodes;
    std::vector<char> in((size_t)rig.NodeCount(), 0);
    const int hand[2] = {rig.NodeIndex("hand_l"), rig.NodeIndex("hand_r")}, upper[2] = {rig.NodeIndex("upperarm_l"), rig.NodeIndex("upperarm_r")};
    if (hand[0] < 0 || hand[1] < 0 || upper[0] < 0 || upper[1] < 0) return out;
    for (int h : hand)
        for (int i = h; i >= 0 && !in[(size_t)i]; i = rig.NodeParent(i)) in[(size_t)i] = 1;
    const int chest = rig.NodeIndex("spine_05");
    if (chestTilt) chestTilt->assign(db.FrameCount(), 0.0f);
    const glm::mat3 chestBindInv = chest >= 0 ? glm::inverse(glm::mat3(g[(size_t)chest])) : glm::mat3(1.0f);
    for (int i = 0; i < rig.NodeCount(); ++i)
        if (in[(size_t)i]) nodes.push_back(i);
    std::vector<int> streamBone((size_t)rig.NodeCount(), -1);
    for (size_t b = 0; b < binding.Nodes.size(); ++b)
        if (binding.Nodes[b] >= 0) streamBone[(size_t)binding.Nodes[b]] = (int)b;
    std::vector<glm::mat4> pose = g;
    for (std::uint32_t f = 0; f < db.FrameCount(); ++f) {
        for (int i : nodes) {
            LocalTRS l = bind[(size_t)i];
            if (streamBone[(size_t)i] >= 0) l.R = stream.Rotation(f, streamBone[(size_t)i]);
            const int p = rig.NodeParent(i);
            pose[(size_t)i] = p >= 0 ? pose[(size_t)p] * l.ToMatrix() : l.ToMatrix();
        }
        if (chestTilt && chest >= 0 && in[(size_t)chest]) {
            glm::vec3 up = glm::mat3(pose[(size_t)chest]) * (chestBindInv * glm::vec3(0.0f, 1.0f, 0.0f));
            up = glm::normalize(up);
            (*chestTilt)[f] = glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f)));
        }
        out[f] = std::max(pose[(size_t)hand[0]][3].y - pose[(size_t)upper[0]][3].y, pose[(size_t)hand[1]][3].y - pose[(size_t)upper[1]][3].y);
    }
    return out;
}

void SharedMotionDatabase::Loop::Close(const MotionDatabase& db, std::uint32_t frame, float frac, MotionPose& pose) const {
    if (!Valid || frame < First) return;
    const float into = (float)(frame - First) + frac;
    if (into >= (float)kOverlap) return;
    thread_local MotionPose past;
    db.SamplePose(Last + (frame - First), frac, past); // the frames past the end, where the wrap came from
    const float u = into / (float)kOverlap, w = u * u * (3.0f - 2.0f * u);
    for (size_t k = 0; k < pose.Rot.size() && k < past.Rot.size(); ++k) {
        glm::quat a = past.Rot[k];
        if (glm::dot(a, pose.Rot[k]) < 0.0f) a = -a;
        pose.Rot[k] = glm::normalize(glm::slerp(a, pose.Rot[k], w));
    }
    pose.Pelvis = glm::mix(past.Pelvis, pose.Pelvis, w);
}

SharedMotionDatabase::Loop SharedMotionDatabase::FindSprintLoop(const MotionDatabase& db) {
    // One full stride, cut on the gait's phase: from a left-foot touchdown to the next (the ContactL label's rising edge),
    // fast, straight and searchable throughout, with the crossfade's frames past it in the same take. Scored on the seam -
    // the bones' mean angle (deg) and the feet / pelvis features (positions and velocities, in their group's std) between
    // its first frame and the one after its last - and on its speed change (a dash's drive phase isn't a sprint). (The first
    // picker compared bones and velocities over any 0.5-0.95 s window: it cut a stride 3 frames short in a sprint start, the
    // right foot 21 cm off at the seam - a hitch every stride, "something about the animation doesn't seem right".) The pack
    // has 15 such strides; the best is at the top of a long dash.
    Loop best;
    const std::uint32_t n = db.FrameCount();
    if (n < 2) return best;
    const float fps = db.Fps();
    std::vector<char> ok(n, 0);
    std::vector<float> speed(n, 0.0f);
    for (std::uint32_t f = 0; f < n; ++f) {
        speed[f] = FirstPersonBodyMotion::RootSpeed(db, f);
        const std::uint8_t fl = db.Flags(f);
        if (fl & (MotionSearch::kAlwaysForbid | MotionLabel::Crouch | MotionLabel::Airborne | MotionLabel::Strafe)) continue;
        if (db.Gait(f) < MotionLabel::Jog || speed[f] < 3.0f || speed[f] > 5.0f) continue;
        if (std::fabs(FirstPersonBodyMotion::RootYawRate(db, f)) > 25.0f * kPi / 180.0f) continue;
        ok[f] = 1;
    }
    std::vector<std::uint32_t> touch;
    for (std::uint32_t f = 1; f < n; ++f)
        if ((db.Flags(f) & MotionLabel::ContactL) && !(db.Flags(f - 1) & MotionLabel::ContactL) && db.ClipOf(f) == db.ClipOf(f - 1)) touch.push_back(f);
    const int nb = (int)db.Stream().Bones().size();
    const std::uint32_t minLen = (std::uint32_t)std::lround(0.45f * fps), maxLen = (std::uint32_t)std::lround(0.95f * fps);
    float bestCost = 1e30f;
    for (size_t i = 0; i + 1 < touch.size(); ++i) {
        const std::uint32_t a = touch[i], b = touch[i + 1];
        if (db.ClipOf(a) != db.ClipOf(b) || b - a < minLen || b - a > maxLen) continue;
        if (b + (std::uint32_t)Loop::kOverlap + 2 >= n || db.ClipOf(b + (std::uint32_t)Loop::kOverlap + 2) != db.ClipOf(a)) continue;
        bool all = true;
        for (std::uint32_t f = a; f <= b && all; ++f) all = ok[f] != 0;
        if (!all) continue;
        float angle = 0.0f;
        for (int k = 0; k < nb; ++k) angle += MotionMath::QuatAngle(db.Stream().Rotation(a, k), db.Stream().Rotation(b, k));
        angle = angle / (float)std::max(nb, 1) * 180.0f / kPi;
        const auto ra = db.Raw(a), rb = db.Raw(b);
        float feat = 0.0f;
        for (int d = 0; d < 15; ++d) feat += std::fabs(ra[(size_t)d] - rb[(size_t)d]) / std::max(db.Scale(d), 1e-3f);
        feat /= 15.0f;
        const float dv = std::fabs(speed[b] - speed[a]);
        const float cost = angle + 2.0f * feat + 3.0f * dv;
        if (cost < bestCost) {
            bestCost = cost;
            best.Valid = true;
            best.First = a;
            best.Last = b;
            best.Seam = cost;
        }
    }
    if (best.Valid) {
        float sum = 0.0f;
        for (std::uint32_t f = best.First; f <= best.Last; ++f) sum += speed[f];
        best.Speed = sum / (float)(best.Last - best.First + 1);
        best.Clip = db.Clip(db.ClipOf(best.First)).Name;
    }
    return best;
}

std::shared_ptr<const SharedMotionDatabase> SharedMotionDatabase::Get(const std::string& base, std::string* error, const Model* rig) {
    static std::mutex s_Lock;
    static std::map<std::string, std::weak_ptr<const SharedMotionDatabase>> s_Loaded;
    std::lock_guard<std::mutex> lock(s_Lock);
    if (auto live = s_Loaded[base].lock()) return live;
    auto shared = std::make_shared<SharedMotionDatabase>();
    if (!shared->Db.Load(base, error)) return nullptr;
    std::vector<float> tilt;
    if (rig) {
        const std::vector<float> over = HandOverShoulder(shared->Db, *rig, &tilt);
        const MotionDatabase& d = shared->Db;
        std::vector<char> bad(d.FrameCount(), 0);
        // The ankles' sideways gap in root space: the axis (x or z) and its sign from the standing idles (left minus right).
        std::vector<float> gapX, gapZ;
        for (std::uint32_t f = 0; f < d.FrameCount(); f += 7)
            if (d.Gait(f) == MotionLabel::Idle && !(d.Flags(f) & (MotionLabel::Crouch | MotionSearch::kAlwaysForbid))) {
                const auto r = d.Raw(f);
                gapX.push_back(r[0] - r[3]);
                gapZ.push_back(r[2] - r[5]);
            }
        const auto med = [](std::vector<float> v) {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        const float mx = med(gapX), mz = med(gapZ);
        const int axis = std::fabs(mx) >= std::fabs(mz) ? 0 : 2;
        const float sign = (axis == 0 ? mx : mz) >= 0.0f ? 1.0f : -1.0f;
        std::printf("[MotionMatching] standing feet gap (left - right, root space): x %.3f m, z %.3f m - the %s axis\n", mx, mz, axis == 0 ? "x" : "z");
        int raw = 0, searchable = 0, hist[5] = {0, 0, 0, 0, 0};
        const float edges[5] = {-0.15f, -0.10f, -0.05f, 0.0f, 0.05f};
        for (std::uint32_t f = 0; f < d.FrameCount(); ++f) {
            if (d.Flags(f) & (MotionSearch::kAlwaysForbid | MotionLabel::Airborne)) continue;
            ++searchable;
            for (int k = 0; k < 5; ++k) hist[k] += over[f] > edges[k];
            const bool bent = !(d.Flags(f) & MotionLabel::Crouch) && tilt[f] > kBentOverDeg &&
                              FirstPersonBodyMotion::RootSpeed(d, f) < kBentOverSpeed;
            if (bent) ++shared->BentOverFrames;
            bool crossed = false;
            if (d.Gait(f) >= MotionLabel::Walk && !(d.Flags(f) & MotionLabel::Strafe)) {
                const auto r = d.Raw(f);
                crossed = sign * (r[(size_t)axis] - r[(size_t)axis + 3]) < -kCrossedFeet;
                if (crossed) ++shared->CrossedFrames;
            }
            if (over[f] <= kHandsUpMargin && !bent && !crossed) continue;
            ++raw;
            const auto& clip = d.Clip(d.ClipOf(f));
            const std::uint32_t from = (std::uint32_t)std::max<std::int64_t>(clip.First, (std::int64_t)f - kHandsUpPad);
            const std::uint32_t to = std::min<std::uint32_t>(clip.First + clip.Count - 1, f + (std::uint32_t)kHandsUpPad);
            for (std::uint32_t j = from; j <= to; ++j) bad[j] = 1;
        }
        for (std::uint32_t f = 0; f < d.FrameCount(); ++f) // (airborne frames stay: the jumps play locked)
            if (d.Flags(f) & MotionLabel::Airborne) bad[f] = 0;
        shared->HandsUpFrames = shared->Db.ExcludeFrames(bad);
        std::printf("[MotionMatching] hands over the shoulders, bent over or legs crossed: %d of %d searchable frames (%u bent over %.0f deg, %u crossed %.2f m; %u excluded with the pads); "
                    "share over -0.15/-0.10/-0.05/0/+0.05 m: %.1f/%.1f/%.1f/%.1f/%.1f %%\n", raw, searchable, shared->BentOverFrames, kBentOverDeg, shared->CrossedFrames, kCrossedFeet,
                    shared->HandsUpFrames, 100.0f * hist[0] / std::max(1, searchable), 100.0f * hist[1] / std::max(1, searchable),
                    100.0f * hist[2] / std::max(1, searchable), 100.0f * hist[3] / std::max(1, searchable), 100.0f * hist[4] / std::max(1, searchable));
    }
    const MotionDatabase& db = shared->Db;
    shared->Gaits = GaitSpeedTable::Measure(db);
    std::printf("[MotionMatching] %s: %u frames, %u excluded as capture glitches (a bone over %.0f deg in a frame, %d either side), %u "
                "as the way to and from prone (%.1f s)\n", base.c_str(), db.FrameCount(), db.GlitchFrames(), MotionDatabase::kGlitchDegrees,
                MotionDatabase::kGlitchPad, db.ProneApproachFrames(), MotionDatabase::kProneApproachSeconds);
    // The turn on the spot: the idle frames whose root turns (over 30 deg/s), standing, searchable; their p75 rate.
    std::vector<float> rates;
    for (std::uint32_t f = 0; f < db.FrameCount(); ++f) {
        const std::uint8_t fl = db.Flags(f);
        if (db.Gait(f) != MotionLabel::Idle || (fl & (MotionLabel::Crouch | MotionLabel::Airborne | MotionSearch::kAlwaysForbid))) continue;
        const float r = std::fabs(FirstPersonBodyMotion::RootYawRate(db, f));
        if (r > 30.0f * kPi / 180.0f && FirstPersonBodyMotion::RootSpeed(db, f) < 0.5f) rates.push_back(r);
    }
    if (!rates.empty()) {
        std::sort(rates.begin(), rates.end());
        shared->TurnInPlaceRate = rates[(size_t)(0.75f * (float)(rates.size() - 1))];
    }
    const int idle = db.Stream().FindClip("Stand_Idle_Neutral_Take001");
    shared->IdleFrame = idle >= 0 ? db.Clip(idle).First + 60 : 0;
    const int crouchIdle = db.Stream().FindClip("Crouch_Idles_Take001");
    shared->CrouchIdleFrame = crouchIdle >= 0 ? db.Clip(crouchIdle).First + 60 : shared->IdleFrame;
    // With the rig's chest tilts: each reference is its take's typical pose - the searchable idle frame (crouched for the
    // crouch) whose chest tilt is the take's median. (A fixed second in, the crouched reference could be the actor still
    // standing: Spine Stability then stood every crouch's chest up toward it - the owner's "goofy" crouch.)
    if (!tilt.empty()) {
        const auto pick = [&](int clip, bool crouched, std::uint32_t& out) {
            if (clip < 0) return;
            std::vector<std::pair<float, std::uint32_t>> v;
            const auto& c = db.Clip(clip);
            for (std::uint32_t f = c.First; f < c.First + c.Count; ++f) {
                const std::uint8_t fl = db.Flags(f);
                if (db.Gait(f) != MotionLabel::Idle || (fl & MotionSearch::kAlwaysForbid) || ((fl & MotionLabel::Crouch) != 0) != crouched) continue;
                v.push_back({tilt[f], f});
            }
            if (v.empty()) return;
            std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(v.size() / 2), v.end());
            std::printf("[MotionMatching] Spine Stability's %s reference: %s @ %.1f s, chest tilt %.1f deg (the take's median of %zu "
                        "frames; was @ 1.0 s, %.1f deg)\n", crouched ? "crouched" : "standing", c.Name.c_str(),
                        db.SecondsInClip(v[v.size() / 2].second), v[v.size() / 2].first, v.size(), tilt[out]);
            out = v[v.size() / 2].second;
        };
        pick(idle, false, shared->IdleFrame);
        pick(crouchIdle, true, shared->CrouchIdleFrame);
    }
    shared->SprintLoop = FindSprintLoop(db);
    if (shared->SprintLoop.Valid)
        std::printf("[MotionMatching] sprint loop: %s @ %.2f-%.2f s (%u frames), %.2f m/s, seam %.3f\n", shared->SprintLoop.Clip.c_str(),
                    db.SecondsInClip(shared->SprintLoop.First), db.SecondsInClip(shared->SprintLoop.Last),
                    shared->SprintLoop.Last - shared->SprintLoop.First, shared->SprintLoop.Speed, shared->SprintLoop.Seam);
    else
        std::printf("[MotionMatching] sprint loop: none found (the sprint matches as before)\n");
    s_Loaded[base] = shared;
    return shared;
}

// --- the body's motion -----------------------------------------------------------------------------------------------

float FirstPersonBodyMotion::SprintSpeed() const {
    if (!m_Shared) return 0.0f;
    return m_Shared->SprintLoop.Valid && Config.SprintLoop ? kSprintOfLoop * m_Shared->SprintLoop.Speed : m_Shared->Gaits.Run;
}

float FirstPersonBodyMotion::RootSpeed(const MotionDatabase& db, std::uint32_t frame) {
    const auto& c = db.Clip(db.ClipOf(frame));
    const std::uint32_t a = std::max(c.First, frame >= 5 ? frame - 5 : 0u), b = std::min(c.First + c.Count - 1, frame + 5);
    if (b <= a) return 0.0f;
    glm::vec3 d = db.Stream().Root(b).Position - db.Stream().Root(a).Position;
    d.y = 0.0f;
    return glm::length(d) * db.Fps() / (float)(b - a);
}

float FirstPersonBodyMotion::RootYawRate(const MotionDatabase& db, std::uint32_t frame) {
    const auto& c = db.Clip(db.ClipOf(frame));
    const std::uint32_t a = std::max(c.First, frame >= 5 ? frame - 5 : 0u), b = std::min(c.First + c.Count - 1, frame + 5);
    if (b <= a) return 0.0f;
    return Wrap(db.Stream().Root(b).Yaw - db.Stream().Root(a).Yaw) * db.Fps() / (float)(b - a);
}

bool FirstPersonBodyMotion::Start(const std::vector<std::shared_ptr<Model>>& models, size_t driver, std::string& error) {
    Stop();
    if (Env::IsSet(Env::Var::MmCrouch)) Config.CrouchFromWalk = Env::Str(Env::Var::MmCrouch) != "pack";
    std::string err;
    auto shared = SharedMotionDatabase::Get(ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny"), &err,
                                            driver < models.size() ? models[driver].get() : nullptr);
    if (!shared) { error = "the motion matching database isn't baked (" + err + ")"; return false; }
    const MotionClipStream& stream = shared->Db.Stream();
    // The driver's pelvis height sets the translation scale; every piece shares it (one skeleton).
    float pelvisScale = 1.0f;
    if (driver < models.size() && models[driver]) {
        std::vector<LocalTRS> bind;
        models[driver]->BindLocalPose(bind);
        std::vector<glm::mat4> g;
        MotionBake::RootSpaceGlobals(*models[driver], bind, -1, g);
        const int p = models[driver]->NodeIndex("pelvis");
        const auto b = stream.Bind(*models[driver], p >= 0 ? g[(size_t)p][3].y : 0.0f);
        if (b.Pelvis < 0 || b.Bound < (int)stream.Bones().size() * 9 / 10) {
            error = "the body's driver doesn't have the stream's bones (" + std::to_string(b.Bound) + " of " +
                    std::to_string(stream.Bones().size()) + " bound)";
            return false;
        }
        pelvisScale = b.PelvisScale;
    }
    m_Bind.clear();
    m_BindPose.clear();
    for (const auto& m : models) {
        MotionClipStream::Binding b;
        std::vector<LocalTRS> bind;
        if (m) {
            m->BindLocalPose(bind);
            b = stream.Bind(*m, 0.0f);
            b.PelvisScale = pelvisScale;
        }
        m_Bind.push_back(std::move(b));
        m_BindPose.push_back(std::move(bind));
    }
    m_Shared = shared;
    m_Matcher.Reset(&m_Shared->Db, m_Shared->IdleFrame);
    m_HaveYaw = false;
    m_Air = Air::None;
    m_Looping = false;
    m_WarpRate = 1.0f;
    MmPlayerDebug::Info() = MmPlayerDebug::Snapshot{};
    return true;
}

void FirstPersonBodyMotion::Stop() {
    m_Shared.reset();
    m_Bind.clear();
    m_BindPose.clear();
    m_Lines.clear();
    m_Matcher = MotionMatcher{};
    MmPlayerDebug::Info() = MmPlayerDebug::Snapshot{};
}

void FirstPersonBodyMotion::EnterAir(const Input& in, const MotionMatcher::Input& query) {
    const MotionDatabase& db = m_Shared->Db;
    MotionSearchParams p;
    p.Require = MotionLabel::Airborne;
    p.Forbid = MotionLabel::Crouch;
    float traj[12], q[MotionDatabase::kDims];
    MotionMatcher::TrajectoryFeatures(query, db.Stream().RestYaw, traj);
    const auto pose = db.Raw(m_Matcher.Frame());
    MotionSearch::BuildQuery(db, pose.data(), traj, q);
    const MotionSearchResult r = MotionSearch::Search(db, q, p, -1);
    m_Air = Air::Rising;
    m_AirRate = 1.0f;
    if (r.Frame < 0) { m_Matcher.Locked = false; return; } // no jump in the database: keep matching on the ground
    const auto& clip = db.Clip(db.ClipOf((std::uint32_t)r.Frame));
    const std::uint32_t last = clip.First + clip.Count - 1;
    std::uint32_t first = (std::uint32_t)r.Frame, end = (std::uint32_t)r.Frame;
    while (first > clip.First && (db.Flags(first - 1) & MotionLabel::Airborne)) --first;
    while (end < last && (db.Flags(end + 1) & MotionLabel::Airborne)) ++end;
    const std::uint32_t takeoff = in.Jumped ? (std::uint32_t)std::lround(Config.TakeoffSeconds * db.Fps()) : 0u;
    const std::uint32_t from = first > clip.First + takeoff ? first - takeoff : clip.First;
    // Time-warped to the flight: a jump's (up and down at JumpSpeed) or, falling, half speed until the landing.
    const float flight = in.Jumped && in.Gravity < 0.0f ? 2.0f * in.JumpSpeed / -in.Gravity : 0.0f;
    const float segment = (float)(end - from) / db.Fps();
    m_AirRate = flight > 0.0f ? std::clamp(segment / flight, 0.35f, 1.0f) : 0.5f;
    m_AirLast = end;
    m_Matcher.Locked = true;
    m_Matcher.HoldFrame = end;
    m_Matcher.ForceFrame(from, in.Dt);
}

void FirstPersonBodyMotion::Tick(const Input& in, float& yaw) {
    m_Lines.clear();
    if (!m_Shared || !(in.Dt > 0.0f)) return;
    const MotionDatabase& db = m_Shared->Db;
    const float dt = in.Dt;
    const float prevYaw = yaw;
    glm::vec3 wish(in.Wish.x, 0.0f, in.Wish.z);
    const float wishSpeed = glm::length(wish);
    const glm::vec3 vel(in.Velocity.x, 0.0f, in.Velocity.z);
    m_CapsuleSpeed = glm::length(vel);

    // The facing wanted: the view's while moving; standing still the heading holds until the view is Turn Threshold off
    // it, then turns to it (and stops Turn End Angle short).
    m_Still = wishSpeed < 0.1f;
    const float twist = Wrap(in.ViewYaw - yaw);
    if (!m_HaveYaw) { m_DesiredYaw = yaw; m_HaveYaw = true; }
    if (!m_Still || in.TurnThreshold <= 0.0f) {
        m_DesiredYaw = in.ViewYaw;
        m_Turning = false;
    } else {
        if (!m_Turning && std::fabs(twist) > in.TurnThreshold) m_Turning = true;
        if (m_Turning) {
            m_DesiredYaw = in.ViewYaw;
            if (std::fabs(twist) < std::max(in.TurnEndAngle, 2.0f * kPi / 180.0f)) m_Turning = false;
        }
        if (!m_Turning) m_DesiredYaw = yaw + Wrap(m_DesiredYaw - yaw) * (1.0f - Toward(dt, 0.05f)); // settles on the heading
    }

    // The filters: crouch, the gait (sprint: the run, pulled harder).
    // (Crouched from the walk, the matcher plays standing frames - the body lowers them; `crouched` is still the player's.)
    const bool crouched = in.Crouched;
    const bool crouch = crouched && !Config.CrouchFromWalk;
    int gait = m_Shared->Gaits.GaitFor(wishSpeed, crouch);
    // Sprint is the run gait while Shift is held and the move heads along the view - slowed in a turn too (the gait is in
    // the schedule's filter key: flipping between run and jog as a look about slowed the run forced searches every sweep).
    // A sprint sideways is the jog strafe's speed (the pack has no run strafes): its gait follows its speed.
    const bool ahead = wishSpeed > 0.1f && std::fabs(Wrap(MotionMath::YawOf(wish) - in.ViewYaw)) < 50.0f * kPi / 180.0f;
    if (in.Sprint && !crouched && ahead) gait = MotionLabel::Run;
    const bool runPace = wishSpeed > 0.5f * (m_Shared->Gaits.Jog + m_Shared->Gaits.Run);
    MotionSearchParams params;
    params.DesiredGait = gait;
    if (gait == MotionLabel::Run && runPace) params.GaitWeight *= 2.0f; // (slowed in a turn, the trajectory decides)
    params.TransitionCost = Config.TransitionCost;
    // No dead ends: crouched, the jumps' deep landings (a few frames of Crouch before the actor stands) won the search,
    // played into a standing frame at once and forced another search - every frame (the crouch legs, 30-50 jumps a second).
    params.MinRunway = Config.MinRunway;
    // Strafing (the move over 50 degrees off the view): where the root goes outweighs the pose - the pack's strafes are
    // few (crouched, 8 legs), and at the default weights diagonal walks won them.
    if (wishSpeed > 0.1f && std::fabs(Wrap(MotionMath::YawOf(wish) - in.ViewYaw)) > 50.0f * kPi / 180.0f)
        params.GroupWeight[MotionFeature::TrajPos] *= 2.0f;
    if (crouch) params.Require = MotionLabel::Crouch;
    else params.Forbid = MotionLabel::Airborne | MotionLabel::Crouch;

    // The trajectory, from the capsule as it is now.
    TrajectoryState now;
    now.Position = glm::vec3(in.Feet.x, in.Feet.y, in.Feet.z);
    now.Velocity = vel;
    now.Yaw = yaw;
    now.YawRate = m_YawRate;
    m_Predictor.Params.VelocityHalflife = std::max(in.VelocityHalflife, 0.02f);
    m_Predictor.Params.FacingHalflife = m_Still ? Config.TurnFacingHalflife : Config.MovingFacingHalflife;
    m_Predictor.Sweep = in.Sweep;
    m_Predictor.SetState(now);
    const auto ts = db.TrajSeconds();
    TrajectoryState future[3];
    m_Predictor.Predict(wish, m_DesiredYaw, ts.data(), 3, future);
    MotionMatcher::Input q;
    q.Position = now.Position;
    q.Yaw = yaw;
    for (int k = 0; k < 3; ++k) {
        q.FuturePosition[(size_t)k] = future[k].Position;
        q.FutureYaw[(size_t)k] = future[k].Yaw;
    }
    // What the search schedule watches for an input change: the move as the player asks it, in the view's frame while
    // moving (the heading follows the view, so a mouse sweep alone changes nothing the query is made of - in world space
    // every sweep forced searches: 6-8 jumps a second looking about), the facing wanted standing still (a turn on the spot).
    // (Its speed the gait's own: the run slowing in a turn isn't a new input - looking about at a run, every sweep's slowdown
    // forced searches.)
    const glm::vec3 nominal = wishSpeed > 0.1f ? wish / wishSpeed * m_Shared->Gaits.SpeedFor(gait, crouch) : glm::vec3(0.0f);
    // (The trajectory itself keeps the real speed: the predictor's goal is `wish`.)
    q.DesiredVelocity = m_Still ? wish : glm::angleAxis(-in.ViewYaw, glm::vec3(0.0f, 1.0f, 0.0f)) * nominal;
    q.DesiredYaw = m_Still ? m_DesiredYaw : 0.0f;

    // Airborne: the label-locked segment.
    m_AirTime = in.Grounded ? 0.0f : m_AirTime + dt;
    if ((m_Air == Air::None || m_Air == Air::Landing) && (in.Jumped || (!in.Grounded && m_AirTime > Config.AirDelay))) {
        EnterAir(in, q);
    } else if ((m_Air == Air::Rising || m_Air == Air::Holding) && in.Grounded && !in.Jumped) {
        // Landed: the take's own landing plays a moment, locked, then matching takes over (dead-blended). Not on the move:
        // the pack's jumps land in place, and held there under a running capsule the planted feet slid ~25 cm.
        m_Air = Air::Landing;
        m_LandTimer = m_CapsuleSpeed > 1.0f ? 0.0f : Config.LandSeconds;
        const auto& clip = db.Clip(db.ClipOf(m_Matcher.Frame()));
        const std::uint32_t last = clip.First + clip.Count - 1;
        m_Matcher.HoldFrame = std::min<std::int64_t>(last, (std::int64_t)m_AirLast + (std::int64_t)std::lround(Config.LandSeconds * db.Fps()));
        if (m_LandTimer > 0.0f && m_Matcher.Frame() < m_AirLast) m_Matcher.ForceFrame(m_AirLast, dt);
    }
    if (m_Air == Air::Rising && m_Matcher.Frame() >= m_AirLast) m_Air = Air::Holding;
    if (m_Air == Air::Landing) {
        m_LandTimer -= dt;
        if (m_LandTimer <= 0.0f) {
            m_Air = Air::None;
            m_Matcher.Locked = false;
            m_Matcher.HoldFrame = -1;
        }
    }

    // Speed warping: the matched frame plays at the capsule's pace (within MinWarp..MaxWarp).
    m_AnimSpeed = RootSpeed(db, m_Matcher.Frame());
    float rate = 1.0f;
    if (m_AnimSpeed > 0.3f && m_CapsuleSpeed > 0.3f) rate = std::clamp(m_CapsuleSpeed / m_AnimSpeed, Config.MinWarp, Config.MaxWarp);
    m_WarpRate += (rate - m_WarpRate) * Toward(dt, 0.08f);
    m_HeadingSettled = std::fabs(m_YawRate) < Config.SprintLoopSettleRate ? m_HeadingSettled + dt : 0.0f;
    // The sprint loop: sprinting ahead at speed, the loop plays locked (the heading still follows the view); a start, a stop,
    // a reversal (the capsule's speed drops), a jump or a crouch hands back to matching.
    const SharedMotionDatabase::Loop& loop = m_Shared->SprintLoop;
    const float runSpeed = std::max(SprintSpeed(), 0.1f);
    const bool sprinting = loop.Valid && Config.SprintLoop && in.Sprint && !crouched && ahead && in.Grounded && m_Air == Air::None;
    if (m_Looping && (!sprinting || m_CapsuleSpeed < Config.SprintLoopExit * runSpeed)) {
        m_Looping = false;
        m_Matcher.AdjustTarget = nullptr;
        if (m_Air == Air::None) { // (a jump owns the lock it just took)
            m_Matcher.Locked = false;
            m_Matcher.HoldFrame = -1;
        }
    } else if (!m_Looping && sprinting && m_CapsuleSpeed > Config.SprintLoopEnter * runSpeed && m_HeadingSettled >= Config.SprintLoopSettle) {
        // (Only once the heading has settled: entered out of a reversal's pivot, the dead blend carried the pivot's crossing
        // legs into the stride for 0.2 s.)
        // In at the loop frame whose feet are most like the pose now (the stride's phase).
        const auto here = db.Raw(m_Matcher.Frame());
        std::uint32_t enter = loop.First;
        float bestCost = 1e30f;
        for (std::uint32_t f = loop.First; f < loop.Last; ++f) {
            const auto r = db.Raw(f);
            float c = 0.0f;
            for (int d = 0; d < 15; ++d) c += (here[(size_t)d] - r[(size_t)d]) * (here[(size_t)d] - r[(size_t)d]) / (db.Scale(d) * db.Scale(d));
            if (c < bestCost) { bestCost = c; enter = f; }
        }
        m_Looping = true;
        m_Matcher.ForceFrame(enter, dt);
        m_Matcher.Locked = true;
        m_Matcher.HoldFrame = loop.Last + 2; // (past the end: the wrap below keeps the frame's fraction)
        const SharedMotionDatabase* shared = m_Shared.get();
        m_Matcher.AdjustTarget = [shared](std::uint32_t f, float frac, MotionPose& p) { shared->SprintLoop.Close(shared->Db, f, frac, p); };
    }
    m_Matcher.SearchParams = params;
    m_Matcher.ForcedPoseWeight = Config.ForcedPoseWeight;
    m_Matcher.PlayRate = m_Air == Air::Rising ? m_AirRate : m_Air == Air::None ? m_WarpRate : 1.0f;
    m_Matcher.Update(dt, q);
    if (m_Looping && m_Matcher.Frame() >= loop.Last) {
        // Closed, the end is the start: the wrap re-poses the same pose there (the frame's fraction kept).
        m_Matcher.LoopTo(loop.First + (m_Matcher.Frame() - loop.Last), dt);
        m_Matcher.Repose();
    }

    // Orientation warping. Standing still (a turn on the spot) the heading turns as the matched frame's root does, plus a
    // spring toward the facing wanted. Moving, it only follows the view (a short spring): the clip's own turn added on top
    // fed back - a turning clip carried the heading past the view, the next query asked to turn back, and the body lunged
    // from one turning clip to the other whenever the view moved (the owner's play test, 2026-11-17).
    const float animTurn = m_Still && m_Air == Air::None ? RootYawRate(db, m_Matcher.Frame()) * m_Matcher.PlayRate * dt : 0.0f;
    yaw += animTurn;
    const float halflife = m_Air != Air::None ? Config.AirFacingHalflife : m_Still ? Config.TurnFacingHalflife : Config.MovingFacingHalflife;
    float turn = Wrap(m_DesiredYaw - yaw) * Toward(dt, halflife);
    if (!m_Still && m_Air == Air::None && Config.MaxMovingYawRate > 0.0f)
        turn = std::clamp(turn, -Config.MaxMovingYawRate * dt, Config.MaxMovingYawRate * dt);
    yaw += turn;
    yaw = Wrap(yaw);
    m_YawRate = Wrap(yaw - prevYaw) / dt;

    // Diagnostics.
    MmPlayerDebug::Snapshot& d = MmPlayerDebug::Info();
    const MotionMatcher& m = m_Matcher;
    d.Valid = true;
    d.Frame = m.Frame();
    d.Clip = db.Clip(db.ClipOf(m.Frame())).Name;
    d.Flags = db.Flags(m.Frame());
    d.Gait = db.Gait(m.Frame());
    d.DesiredGait = gait;
    d.Crouch = crouch;
    d.Sprint = in.Sprint;
    d.Airborne = m_Air != Air::None;
    d.Locked = m.Locked;
    d.Looping = m_Looping;
    d.Still = m_Still;
    d.Turning = m_Turning;
    d.Searched = m.SearchedThisUpdate();
    d.Jumped = m.JumpedThisUpdate();
    d.Searches = m.Searches;
    d.Jumps = m.Jumps;
    d.ForcedByInput = m.Schedule.ForcedByInput;
    d.ForcedByEnd = m.Schedule.ForcedByEnd;
    d.LastCost = m.LastResult().Cost;
    d.PlayingCost = m.LastResult().CurrentCost;
    d.SearchMsAvg = m.Searches > 0 ? m.SearchMsTotal / m.Searches : 0.0;
    d.SearchMsMax = m.SearchMsMax;
    d.PlayRate = m.PlayRate;
    d.AnimSpeed = m_AnimSpeed;
    d.CapsuleSpeed = m_CapsuleSpeed;
    d.Yaw = yaw;
    d.DesiredYaw = m_DesiredYaw;
    d.YawRate = m_YawRate;
    d.TurnInPlaceRate = m_Shared->TurnInPlaceRate;
    {
        const auto raw = db.Raw(m.Frame()); // where the matched frame's root is 1 s on, in its own root space (+Z ahead)
        const float x = raw[15 + 4], z = raw[15 + 5];
        d.MatchedTravel = std::hypot(x, z);
        d.MatchedTravelDeg = d.MatchedTravel > 1e-4f ? std::fabs(std::atan2(x, z)) * 180.0f / kPi : 0.0f;
    }
    d.PoseStepDeg = 0.0f;
    for (size_t i = 0; i < m.Pose().Rot.size() && i < m_LastOut.Rot.size(); ++i) {
        const float a = MotionMath::QuatAngle(m.Pose().Rot[i], m_LastOut.Rot[i]) * 180.0f / kPi;
        if (a > d.PoseStepDeg) {
            d.PoseStepDeg = a;
            d.PoseStepBone = db.Stream().Bones()[i];
        }
    }
    m_LastOut = m.Pose();
    d.RootOffset = 0.0f; // the body is placed at the capsule's feet (FirstPersonBody): the root is clamped there
    if (m.SearchedThisUpdate()) {
        const auto raw = db.Raw(m.Frame());
        for (int k = 0; k < 27; ++k) {
            d.Query[k] = m.LastQueryRaw()[k];
            d.Matched[k] = raw[(size_t)k];
        }
        const float* a = m.LastQueryRaw() + 15;
        d.TrajErr = std::hypot(a[4] - raw[15 + 4], a[5] - raw[15 + 5]);
    }

    // Debug draw.
    const float rest = db.Stream().RestYaw;
    MotionDebugDraw::Trajectory(m_Lines, now.Position, yaw, rest, m.LastQueryRaw() + 15, glm::vec4(1.0f, 0.85f, 0.1f, 1.0f), 0.04f);
    const auto matched = db.Raw(m.Frame());
    MotionDebugDraw::Trajectory(m_Lines, now.Position, yaw, rest, matched.data() + 15, glm::vec4(1.0f, 0.2f, 0.9f, 1.0f), 0.06f);
    MotionDebugDraw::Character(m_Lines, now.Position, yaw, glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    MotionDebugDraw::Feet(m_Lines, now.Position, yaw, rest, matched.data(), d.Flags);
    if (m.JumpedThisUpdate())
        MotionDebugDraw::Line(m_Lines, now.Position + glm::vec3(0, 2.0f, 0), now.Position + glm::vec3(0, 2.3f, 0), glm::vec4(1, 0.2f, 0.9f, 1));
    d.Lines = m_Lines;
}

void FirstPersonBodyMotion::Pose(const std::vector<std::shared_ptr<Model>>& models) {
    if (!m_Shared) return;
    const MotionClipStream& stream = m_Shared->Db.Stream();
    for (size_t k = 0; k < models.size() && k < m_Bind.size(); ++k) {
        if (!models[k] || m_BindPose[k].empty() || (int)m_BindPose[k].size() != models[k]->NodeCount()) continue;
        m_Scratch = m_BindPose[k];
        MotionMatcher::WritePose(m_Matcher.Pose(), stream, m_Bind[k], m_Scratch);
        models[k]->ApplyLocalPose(m_Scratch);
    }
}

bool FirstPersonBodyMotion::ReferencePose(size_t piece, bool crouched, std::vector<LocalTRS>& out) const {
    if (!m_Shared || piece >= m_Bind.size() || m_BindPose[piece].empty()) return false;
    MotionPose p;
    m_Shared->Db.SamplePose(crouched ? m_Shared->CrouchIdleFrame : m_Shared->IdleFrame, 0.0f, p);
    out = m_BindPose[piece];
    MotionMatcher::WritePose(p, m_Shared->Db.Stream(), m_Bind[piece], out);
    return true;
}

namespace MmPlayerDebug {
Snapshot& Info() {
    static Snapshot s;
    return s;
}
} // namespace MmPlayerDebug
