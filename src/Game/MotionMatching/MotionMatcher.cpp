#include "MotionMatcher.h"

#include "MotionMath.h"
#include "RootSynth.h"

#include "Animation.h"

#include <chrono>
#include <cmath>

glm::vec3 MotionMatcher::ToRootSpace(const glm::vec3& charPos, float charYaw, float restYaw, const glm::vec3& world) {
    const glm::mat4 inv = glm::inverse(RootSynth::RootMatrix(charPos, charYaw, restYaw));
    return glm::vec3(inv * glm::vec4(world, 1.0f));
}

glm::vec3 MotionMatcher::DirToRootSpace(float charYaw, float restYaw, float yaw) {
    const glm::mat3 invRot(glm::inverse(RootSynth::RootMatrix(glm::vec3(0.0f), charYaw, restYaw)));
    return invRot * MotionMath::YawDir(yaw);
}

void MotionMatcher::TrajectoryFeatures(const Input& in, float restYaw, float* out) {
    for (int k = 0; k < 3; ++k) {
        const glm::vec3 p = ToRootSpace(in.Position, in.Yaw, restYaw, in.FuturePosition[(size_t)k]);
        const glm::vec3 d = DirToRootSpace(in.Yaw, restYaw, in.FutureYaw[(size_t)k]);
        out[2 * k] = p.x;
        out[2 * k + 1] = p.z;
        out[6 + 2 * k] = d.x;
        out[6 + 2 * k + 1] = d.z;
    }
}

void MotionMatcher::WritePose(const MotionPose& pose, const MotionClipStream& stream, const MotionClipStream::Binding& b,
                              std::vector<LocalTRS>& out) {
    for (size_t i = 0; i < pose.Rot.size() && i < b.Nodes.size(); ++i) {
        const int node = b.Nodes[i];
        if (node >= 0 && node < (int)out.size()) out[(size_t)node].R = pose.Rot[i];
    }
    // The fingers: the pose's (the clip's track, or the relaxed pose), dead-blended with the rest.
    const bool fingers = pose.Fingers.size() == stream.Statics.size();
    for (size_t i = 0; i < stream.Statics.size() && i < b.StaticNodes.size(); ++i) {
        const int node = b.StaticNodes[i];
        if (node >= 0 && node < (int)out.size()) out[(size_t)node].R = fingers ? pose.Fingers[i] : stream.Statics[i].Rotation;
    }
    if (b.Pelvis >= 0 && b.Pelvis < (int)out.size()) out[(size_t)b.Pelvis].T = pose.Pelvis * b.PelvisScale;
}

void MotionMatcher::Reset(const MotionDatabase* db, std::uint32_t frame) {
    m_Db = db;
    m_Frame = db && db->Loaded() ? std::min(frame, db->FrameCount() - 1) : 0;
    m_Frac = 0.0f;
    m_HaveOut = false;
    m_OutInterval = 0.0f;
    m_Last = MotionSearchResult{};
    Schedule.Reset();
    Inertializer.Reset();
    Searches = Jumps = IdleRefits = ForcedByExcluded = 0;
    SearchMsTotal = SearchMsMax = 0.0;
    SearchMs.clear();
    m_ShownCount = 0;
    m_RunFirst = m_Frame;
}

void MotionMatcher::EndRun() {
    // File the run just shown (newest first; the oldest drops off when full).
    for (int k = std::min(m_ShownCount, (int)m_Shown.size() - 1); k > 0; --k) m_Shown[(size_t)k] = m_Shown[(size_t)k - 1];
    m_Shown[0] = ShownRun{m_RunFirst, (std::int64_t)m_Frame, 0.0f};
    m_ShownCount = std::min(m_ShownCount + 1, (int)m_Shown.size());
}

bool MotionMatcher::IdleDrifted(const Input& in) const {
    const auto raw = m_Db->Raw(m_Frame);
    float want[12];
    TrajectoryFeatures(in, m_Db->Stream().RestYaw, want);
    // 1 s on (the third trajectory sample): the root's place (dims 19-20 / want 4-5) and facing (25-26 / want 10-11).
    const float travel = std::hypot(raw[19] - want[4], raw[20] - want[5]);
    const float cross = raw[25] * want[11] - raw[26] * want[10], dot = raw[25] * want[10] + raw[26] * want[11];
    return travel > IdleTravel || std::fabs(std::atan2(cross, dot)) > IdleTurn;
}

void MotionMatcher::Update(float dt, const Input& in) {
    m_Searched = m_Jumped = false;
    if (!m_Db || !m_Db->Loaded()) return;
    const MotionDatabase& db = *m_Db;
    for (int k = 0; k < m_ShownCount; ++k) m_Shown[(size_t)k].Age += dt;
    while (m_ShownCount > 0 && m_Shown[(size_t)m_ShownCount - 1].Age > ReselectSeconds) --m_ShownCount;

    // Play on (at PlayRate: speed warping).
    if (m_HaveOut) {
        m_Frac += dt * std::max(PlayRate, 0.0f) * db.Fps();
        std::uint32_t end = db.ClipEnd(m_Frame);
        if (Locked && HoldFrame >= (std::int64_t)m_Frame && HoldFrame < (std::int64_t)end) end = (std::uint32_t)HoldFrame;
        while (m_Frac >= 1.0f && m_Frame < end) {
            ++m_Frame;
            m_Frac -= 1.0f;
        }
        if (m_Frame >= end) m_Frac = 0.0f;
    }

    // Search (never while Locked: the airborne segment plays its own frames).
    // Search at once when the playing frame can't go on: the take ends, or it played into frames the filter refuses
    // (NoSearch near the end, an actor standing up out of a crouch, a jump's flight) - such a frame is never shown.
    const bool outOfFrames = !MotionSearch::Passes(db.Flags(m_Frame), SearchParams) || m_Frame >= db.ClipEnd(m_Frame);
    const std::uint32_t key = (std::uint32_t)SearchParams.Require | ((std::uint32_t)SearchParams.Forbid << 8) |
                              ((std::uint32_t)(SearchParams.DesiredGait + 1) << 16) | ((std::uint32_t)(SearchParams.DesiredStyle + 1) << 24);
    if (Locked) Schedule.Reset(); // the first update after the lock searches at once
    Schedule.Hold = Idle;
    bool refit = false;
    if (Idle && !Locked && !outOfFrames && Schedule.SinceLast() >= Schedule.Interval - 1e-4f && IdleDrifted(in)) {
        refit = true;
        ++IdleRefits;
    }
    if (!Locked && Schedule.Tick(dt, in.DesiredVelocity, in.DesiredYaw, key, outOfFrames || refit)) {
        if (outOfFrames && (db.Flags(m_Frame) & MotionLabel::Excluded)) ++ForcedByExcluded;
        const auto pose = db.Raw(m_Frame);
        for (int d = 0; d < 15; ++d) m_QueryRaw[d] = pose[d];
        TrajectoryFeatures(in, db.Stream().RestYaw, m_QueryRaw + 15);
        float q[MotionDatabase::kDims];
        db.Normalize(m_QueryRaw, q);
        const auto t0 = std::chrono::steady_clock::now();
        MotionSearchParams params = SearchParams;
        if ((outOfFrames || key != m_LastKey) && ForcedPoseWeight != 1.0f)
            for (int g : {MotionFeature::FootPos, MotionFeature::FootVel, MotionFeature::PelvisVel}) params.GroupWeight[g] *= ForcedPoseWeight;
        m_LastKey = key;
        // The reselect history: the run playing (its last ReselectSeconds) and the runs filed within it, each padded by
        // kReselectPad frames (a jump to the frame just before a run shown replays it as surely as one inside it).
        {
            const std::int64_t back = (std::int64_t)std::ceil(ReselectSeconds * db.Fps() * std::max(PlayRate, 1.0f));
            int n = 0;
            params.Avoid[0] = std::max<std::int64_t>(m_RunFirst, (std::int64_t)m_Frame - back) - kReselectPad;
            params.Avoid[1] = (std::int64_t)m_Frame + kReselectPad;
            ++n;
            for (int k = 0; k < m_ShownCount && n < MotionSearchParams::kMaxAvoid; ++k, ++n) {
                params.Avoid[(size_t)(2 * n)] = m_Shown[(size_t)k].First - kReselectPad;
                params.Avoid[(size_t)(2 * n + 1)] = m_Shown[(size_t)k].Last + kReselectPad;
            }
            params.AvoidCount = n;
        }
        m_Last = MotionSearch::Search(db, q, params, (std::int64_t)m_Frame);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++Searches;
        SearchMsTotal += ms;
        SearchMsMax = std::max(SearchMsMax, ms);
        SearchMs.push_back((float)ms);
        m_Searched = true;
        if (m_Last.Frame >= 0 && m_Last.Switched) {
            BeginBlend();
            EndRun();
            m_Frame = (std::uint32_t)m_Last.Frame;
            m_RunFirst = m_Frame;
            m_Frac = 0.0f;
            m_Jumped = true;
            ++Jumps;
        }
    }

    // Pose.
    PoseAndBlend(dt);
}

void MotionMatcher::BeginBlend() {
    if (m_HaveOut) Inertializer.Transition(m_Prev, m_Out, m_OutInterval);
}

void MotionMatcher::ForceFrame(std::uint32_t frame) {
    m_Searched = false;
    if (!m_Db || !m_Db->Loaded()) return;
    frame = std::min(frame, m_Db->FrameCount() - 1);
    if (m_HaveOut && frame != m_Frame) {
        BeginBlend();
        m_Jumped = true;
        ++Jumps;
    }
    if (frame != m_Frame) {
        EndRun();
        m_RunFirst = frame;
    }
    m_Frame = frame;
    m_Frac = 0.0f;
}

void MotionMatcher::PoseAndBlend(float dt) {
    const MotionDatabase& db = *m_Db;
    db.SamplePose(m_Frame, m_Frac, m_Target);
    if (!m_HaveOut) {
        m_Out = m_Prev = m_Target;
        m_HaveOut = true;
        m_OutInterval = 0.0f;
        return;
    }
    m_Prev = m_Out;
    Inertializer.Update(dt, m_Target, m_Out);
    m_OutInterval = dt;
}
