#include "ThirdPersonLocomotion.h"

#include "AnimationSystem.h"
#include "Log.h"
#include "Model.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

const char* const ThirdPersonLocomotion::kFolder = "assets/Animations/Mobility01/";

namespace {

const char* const kDir[8] = {"F", "FR", "R", "BR_BkPd", "B", "BL_BkPd", "L", "FL"};
const char* const kGait[4] = {"Walk", "Jog", "Run", "CrouchWalk"};

float Follow(float dt, float seconds) { return seconds > 1e-4f ? 1.0f - std::exp(-dt / seconds) : 1.0f; }

std::string Ref(const std::string& file) { return std::string(ThirdPersonLocomotion::kFolder) + file; }

// A looping clip's ground speed: how fast its lower foot moves back while it bears the weight (model space, m/s).
float MeasureSpeed(const Model& m, int clip) {
    const float len = m.AnimationLength(clip);
    const int footL = m.NodeIndex("foot_l"), footR = m.NodeIndex("foot_r");
    if (len <= 0.0f || footL < 0 || footR < 0) return 0.0f;
    std::vector<int> parents(m.NodeCount());
    for (int i = 0; i < m.NodeCount(); ++i) parents[i] = m.NodeParent(i);
    const int n = 60;
    std::vector<glm::vec3> l(n), r(n);
    IK::Pose pose;
    std::vector<glm::mat4> g;
    for (int k = 0; k < n; ++k) {
        m.SampleLocalPose(clip, len * k / n, AnimationWrapMode::Loop, pose);
        IK::ComputeGlobals(pose, parents, g);
        l[k] = IK::Position(g[footL]);
        r[k] = IK::Position(g[footR]);
    }
    std::vector<float> speeds;
    for (int k = 0; k < n; ++k) {
        const int j = (k + 1) % n;
        const bool left = l[k].y < r[k].y && l[j].y < r[j].y;
        const glm::vec3 d = left ? l[j] - l[k] : r[j] - r[k];
        if (!left && !(r[k].y < l[k].y && r[j].y < l[j].y)) continue; // the feet changing over
        speeds.push_back(glm::length(glm::vec2(d.x, d.z)) / (len / n));
    }
    if (speeds.empty()) return 0.0f;
    std::sort(speeds.begin(), speeds.end());
    return speeds[speeds.size() / 2];
}

// Accumulates weighted poses (a running normalized blend).
void Accumulate(IK::Pose& acc, float& total, const IK::Pose& p, float w) {
    if (w <= 1e-4f) return;
    if (total <= 0.0f || acc.size() != p.size()) {
        acc = p;
        total = w;
        return;
    }
    total += w;
    const float t = w / total;
    for (size_t i = 0; i < acc.size(); ++i) acc[i] = LocalTRS::Blend(acc[i], p[i], t);
}

} // namespace

std::string ThirdPersonLocomotion::StandIdle() { return Ref("MOB1_Stand_Relaxed_Idle_v2_IPC.fbx"); }
std::string ThirdPersonLocomotion::CrouchIdle() { return Ref("MOB1_Crouch_Idle_V2_IPC.fbx"); }

bool ThirdPersonLocomotion::Bind(Model& m, AssetLibrary& assets) {
    Rig& r = m_Rigs[&m];
    if (r.Ok) return true;
    r = Rig{};
    for (int gait = 0; gait < GaitCount; ++gait)
        for (int d = 0; d < 8; ++d) {
            r.Clips[gait][d] = -1;
            if (gait == Run && (d >= 3 && d <= 5)) continue; // no run backward: the jog's, faster
            r.Clips[gait][d] = ResolveAnimationClip(m, Ref(std::string("MOB1_") + kGait[gait] + "_" + kDir[d] + "_Loop_IPC.fbx"), assets);
            if (r.Clips[gait][d] < 0) return false;
            r.Length[gait][d] = m.AnimationLength(r.Clips[gait][d]);
        }
    r.Idle = ResolveAnimationClip(m, StandIdle(), assets);
    r.CrouchIdle = ResolveAnimationClip(m, CrouchIdle(), assets);
    r.TurnL = ResolveAnimationClip(m, Ref("MOB1_Stand_Rlx_Turn_In_Place_L_Loop_IPC.fbx"), assets);
    r.TurnR = ResolveAnimationClip(m, Ref("MOB1_Stand_Rlx_Turn_In_Place_R_Loop_IPC.fbx"), assets);
    r.CrouchTurnL = ResolveAnimationClip(m, Ref("MOB1_Crouch_Rlx_Turn_In_Place_L_Loop_IPC.fbx"), assets);
    r.CrouchTurnR = ResolveAnimationClip(m, Ref("MOB1_Crouch_Rlx_Turn_In_Place_R_Loop_IPC.fbx"), assets);
    r.Air = ResolveAnimationClip(m, Ref("Split_Jumps/MOB1_Jog_F_Jump_RU_Air_IPC.fbx"), assets);
    if (r.Idle < 0 || r.CrouchIdle < 0 || r.TurnL < 0 || r.TurnR < 0 || r.CrouchTurnL < 0 || r.CrouchTurnR < 0 || r.Air < 0) return false;
    r.Root = m.NodeIndex("root");
    if (!m_Measured) {
        for (int gait = 0; gait < GaitCount; ++gait)
            if (const float s = MeasureSpeed(m, r.Clips[gait][0]); s > 0.2f) m_Speed[gait] = s;
        m_Measured = true;
        char line[160];
        std::snprintf(line, sizeof line, "3P locomotion: clip ground speeds walk %.3f jog %.3f run %.3f crouch %.3f (model units/s)", m_Speed[Walk],
                      m_Speed[Jog], m_Speed[Run], m_Speed[Crouch]);
        Log::Info(line);
    }
    r.Ok = true;
    return true;
}

void ThirdPersonLocomotion::Reset() {
    m_Fresh = true;
    m_Phase = 0.0f;
}

bool ThirdPersonLocomotion::Apply(const Model& m, IK::Pose& pose, const ThirdPersonMove& move, float dt) {
    const auto it = m_Rigs.find(&m);
    if (it == m_Rigs.end() || !it->second.Ok || (int)pose.size() != m.NodeCount()) return false;
    const Rig& r = it->second;
    const float speed = glm::length(move.Velocity);
    const float crouch = std::clamp(move.Crouch, 0.0f, 1.0f);

    // Weights, eased: moving, crouched, in the air, turning on the spot; the heading of travel.
    const float movingWant = std::clamp((speed - 0.15f) / 0.45f, 0.0f, 1.0f);
    const float turnWant = movingWant < 0.5f && std::abs(move.TurnRate) > 0.6f ? 1.0f : 0.0f;
    if (m_Fresh) {
        m_Moving = movingWant;
        m_CrouchW = crouch;
        m_Air = move.Grounded ? 0.0f : 1.0f;
        m_Turn = 0.0f;
        m_Fresh = false;
    }
    m_Moving += (movingWant - m_Moving) * Follow(dt, 0.12f);
    m_CrouchW += (crouch - m_CrouchW) * Follow(dt, 0.1f);
    m_Air += ((move.Grounded ? 0.0f : 1.0f) - m_Air) * Follow(dt, move.Grounded ? 0.08f : 0.15f);
    m_Turn += (turnWant - m_Turn) * Follow(dt, 0.15f);
    if (speed > 0.1f) {
        const float want = std::atan2(move.Velocity.x, move.Velocity.y);
        float d = want - m_Heading;
        while (d > glm::pi<float>()) d -= glm::two_pi<float>();
        while (d < -glm::pi<float>()) d += glm::two_pi<float>();
        m_Heading += d * Follow(dt, 0.1f);
        while (m_Heading > glm::pi<float>()) m_Heading -= glm::two_pi<float>();
        while (m_Heading < -glm::pi<float>()) m_Heading += glm::two_pi<float>();
    }

    // The gait by speed: walk -> jog -> run standing, the crouch walk crouched; each played at the rate that keeps its feet down.
    const float ms = std::max(move.ModelScale, 1e-6f);
    const float sw = m_Speed[Walk] * ms, sj = m_Speed[Jog] * ms, sr = m_Speed[Run] * ms;
    float gaitPos = speed <= sw ? 0.0f : speed <= sj ? (speed - sw) / std::max(sj - sw, 0.1f) : 1.0f + std::min((speed - sj) / std::max(sr - sj, 0.1f), 1.0f);
    m_GaitPos += (gaitPos - m_GaitPos) * Follow(dt, 0.15f);
    struct Use { int Gait; float W; };
    Use stand[2] = {{Walk, 1.0f}, {Jog, 0.0f}};
    if (m_GaitPos <= 1.0f) stand[0] = {Walk, 1.0f - m_GaitPos}, stand[1] = {Jog, m_GaitPos};
    else stand[0] = {Jog, 2.0f - m_GaitPos}, stand[1] = {Run, m_GaitPos - 1.0f};
    const float standClipSpeed = (stand[0].W * m_Speed[stand[0].Gait] + stand[1].W * m_Speed[stand[1].Gait]) * ms;
    const float clipSpeed = glm::mix(standClipSpeed, m_Speed[Crouch] * ms, m_CrouchW);
    m_Rate = std::clamp(speed / std::max(clipSpeed, 0.1f), 0.5f, 1.6f);

    // The two directions either side of the heading.
    const float slot = (m_Heading < 0.0f ? m_Heading + glm::two_pi<float>() : m_Heading) / glm::quarter_pi<float>();
    const int d0 = (int)std::floor(slot) % 8, d1 = (d0 + 1) % 8;
    const float dt1 = slot - std::floor(slot);
    auto clipOf = [&](int gait, int d) {
        if (r.Clips[gait][d] >= 0) return std::pair<int, float>{r.Clips[gait][d], r.Length[gait][d]};
        return std::pair<int, float>{r.Clips[Jog][d], r.Length[Jog][d]};
    };
    // One phase for every cycle in the blend: advanced by the blend's own cycle length.
    float cycle = 0.0f, cw = 0.0f;
    for (const Use& u : stand)
        for (int k = 0; k < 2; ++k) {
            const float w = u.W * (k ? dt1 : 1.0f - dt1) * (1.0f - m_CrouchW);
            cycle += clipOf(u.Gait, k ? d1 : d0).second * w;
            cw += w;
        }
    for (int k = 0; k < 2; ++k) {
        const float w = (k ? dt1 : 1.0f - dt1) * m_CrouchW;
        cycle += clipOf(Crouch, k ? d1 : d0).second * w;
        cw += w;
    }
    cycle = cw > 0.0f ? cycle / cw : 1.0f;
    if (m_Moving > 1e-3f) m_Phase = std::fmod(m_Phase + dt * m_Rate / std::max(cycle, 0.1f), 1.0f);

    // Sample and blend.
    float total = 0.0f;
    m_Acc.clear();
    const float moveW = m_Moving * (1.0f - m_Air);
    auto addClip = [&](int clip, float len, float t, AnimationWrapMode wrap, float w) {
        if (w <= 1e-4f || clip < 0) return;
        m.SampleLocalPose(clip, len > 0.0f ? t * len : t, wrap, m_A);
        Accumulate(m_Acc, total, m_A, w);
    };
    if (moveW > 1e-3f) {
        for (const Use& u : stand)
            for (int k = 0; k < 2; ++k) {
                const auto [clip, len] = clipOf(u.Gait, k ? d1 : d0);
                addClip(clip, len, m_Phase, AnimationWrapMode::Loop, moveW * u.W * (k ? dt1 : 1.0f - dt1) * (1.0f - m_CrouchW));
            }
        for (int k = 0; k < 2; ++k) {
            const auto [clip, len] = clipOf(Crouch, k ? d1 : d0);
            addClip(clip, len, m_Phase, AnimationWrapMode::Loop, moveW * (k ? dt1 : 1.0f - dt1) * m_CrouchW);
        }
    }
    const float stillW = (1.0f - m_Moving) * (1.0f - m_Air);
    if (stillW > 1e-3f) {
        m_IdleTime += dt;
        if (m_Turn > 1e-3f) m_TurnTime += dt * std::clamp(std::abs(move.TurnRate) / 1.5f, 0.6f, 1.5f);
        const bool left = move.TurnRate > 0.0f;
        addClip(r.Idle, 0.0f, m_IdleTime, AnimationWrapMode::Loop, stillW * (1.0f - m_Turn) * (1.0f - m_CrouchW));
        addClip(r.CrouchIdle, 0.0f, m_IdleTime, AnimationWrapMode::Loop, stillW * (1.0f - m_Turn) * m_CrouchW);
        addClip(left ? r.TurnL : r.TurnR, 0.0f, m_TurnTime, AnimationWrapMode::Loop, stillW * m_Turn * (1.0f - m_CrouchW));
        addClip(left ? r.CrouchTurnL : r.CrouchTurnR, 0.0f, m_TurnTime, AnimationWrapMode::Loop, stillW * m_Turn * m_CrouchW);
    }
    addClip(r.Air, 0.0f, 0.0f, AnimationWrapMode::ClampForever, m_Air);
    if (total <= 0.0f || m_Acc.size() != pose.size()) return false;
    const LocalTRS root = r.Root >= 0 ? pose[r.Root] : LocalTRS{};
    pose = m_Acc;
    if (r.Root >= 0) pose[r.Root] = root;
    return true;
}
