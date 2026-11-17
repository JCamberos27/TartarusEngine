#include "MotionMatcher.h"

#include "MotionMath.h"
#include "RootSynth.h"

#include "Animation.h"

#include <chrono>

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
    for (size_t i = 0; i < stream.Statics.size() && i < b.StaticNodes.size(); ++i) {
        const int node = b.StaticNodes[i];
        if (node >= 0 && node < (int)out.size()) out[(size_t)node].R = stream.Statics[i].Rotation;
    }
    if (b.Pelvis >= 0 && b.Pelvis < (int)out.size()) out[(size_t)b.Pelvis].T = pose.Pelvis * b.PelvisScale;
}

void MotionMatcher::Reset(const MotionDatabase* db, std::uint32_t frame) {
    m_Db = db;
    m_Frame = db && db->Loaded() ? std::min(frame, db->FrameCount() - 1) : 0;
    m_Frac = 0.0f;
    m_HaveOut = false;
    m_Last = MotionSearchResult{};
    Schedule.Reset();
    Inertializer.Reset();
    Searches = Jumps = 0;
    SearchMsTotal = SearchMsMax = 0.0;
    SearchMs.clear();
}

void MotionMatcher::Update(float dt, const Input& in) {
    m_Searched = m_Jumped = false;
    if (!m_Db || !m_Db->Loaded()) return;
    const MotionDatabase& db = *m_Db;

    // Play on.
    if (m_HaveOut) {
        m_Frac += dt * db.Fps();
        const std::uint32_t end = db.ClipEnd(m_Frame);
        while (m_Frac >= 1.0f && m_Frame < end) {
            ++m_Frame;
            m_Frac -= 1.0f;
        }
        if (m_Frame >= end) m_Frac = 0.0f;
    }

    // Search.
    // Search at once when the playing frame can't go on: the take ends, or it played into frames the filter refuses
    // (NoSearch near the end, an actor standing up out of a crouch, a jump's flight) - such a frame is never shown.
    const bool outOfFrames = !MotionSearch::Passes(db.Flags(m_Frame), SearchParams) || m_Frame >= db.ClipEnd(m_Frame);
    const std::uint32_t key = (std::uint32_t)SearchParams.Require | ((std::uint32_t)SearchParams.Forbid << 8) |
                              ((std::uint32_t)(SearchParams.DesiredGait + 1) << 16);
    if (Schedule.Tick(dt, in.DesiredVelocity, in.DesiredYaw, key, outOfFrames)) {
        const auto pose = db.Raw(m_Frame);
        for (int d = 0; d < 15; ++d) m_QueryRaw[d] = pose[d];
        TrajectoryFeatures(in, db.Stream().RestYaw, m_QueryRaw + 15);
        float q[MotionDatabase::kDims];
        db.Normalize(m_QueryRaw, q);
        const auto t0 = std::chrono::steady_clock::now();
        m_Last = MotionSearch::Search(db, q, SearchParams, (std::int64_t)m_Frame);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++Searches;
        SearchMsTotal += ms;
        SearchMsMax = std::max(SearchMsMax, ms);
        SearchMs.push_back((float)ms);
        m_Searched = true;
        if (m_Last.Frame >= 0 && m_Last.Switched) {
            if (m_HaveOut) Inertializer.Transition(m_Prev, m_Out, dt);
            m_Frame = (std::uint32_t)m_Last.Frame;
            m_Frac = 0.0f;
            m_Jumped = true;
            ++Jumps;
        }
    }

    // Pose.
    db.SamplePose(m_Frame, m_Frac, m_Target);
    if (!m_HaveOut) {
        m_Out = m_Prev = m_Target;
        m_HaveOut = true;
        return;
    }
    m_Prev = m_Out;
    Inertializer.Update(dt, m_Target, m_Out);
}
