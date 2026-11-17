#include "MotionFeatures.h"

#include "MotionMath.h"
#include "RootSynth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

const char* MotionLabel::GaitName(int g) {
    switch (g) {
        case Idle: return "idle";
        case Walk: return "walk";
        case Jog: return "jog";
        case Run: return "run";
        default: return "?";
    }
}

const char* const* MotionFeatureSet::Names() {
    static const char* const k[kDims] = {
        "footL.x", "footL.y", "footL.z", "footR.x", "footR.y", "footR.z",
        "footVelL.x", "footVelL.y", "footVelL.z", "footVelR.x", "footVelR.y", "footVelR.z",
        "pelvisVel.x", "pelvisVel.y", "pelvisVel.z",
        "traj0.x", "traj0.z", "traj1.x", "traj1.z", "traj2.x", "traj2.z",
        "dir0.x", "dir0.z", "dir1.x", "dir1.z", "dir2.x", "dir2.z",
    };
    return k;
}

void MotionFeatureSet::AddClip(const std::string& name, const MotionClipFrames& in, const RootTrack& root,
                               const MotionFeatureConfig& cfg) {
    const size_t n = root.Size();
    if (n == 0 || in.Pelvis.size() != n || in.Foot[0].size() != n || in.Foot[1].size() != n) return;
    for (int k = 0; k < 3; ++k) TrajFrames[k] = cfg.TrajFrames[k];
    Clip c;
    c.Name = name;
    c.First = FrameCount();
    c.Count = (std::uint32_t)n;
    m_Clips.push_back(c);

    const float fps = in.Fps;
    const auto vel = [&](const std::vector<glm::vec3>& v, size_t i) {
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        return b > a ? (v[b] - v[a]) * (fps / (float)(b - a)) : glm::vec3(0.0f);
    };
    std::vector<char> contact[2];
    for (int k = 0; k < 2; ++k) contact[k] = RootSynth::Contacts(in.Foot[k], fps, cfg.ContactHeight, cfg.ContactSpeed);

    // Airborne: the lower ankle above AirFootHeight with the pelvis risen AirPelvisRise over standing, for AirMinFrames
    // or more. The ankles alone can't tell a hop from a run's flight phase (both lift the lower foot ~0.25 m); the
    // pelvis can: a run's stays under standing height, a jump's rises over it.
    std::vector<char> air(n, 0);
    for (size_t i = 0; i < n; ++i)
        air[i] = std::min(in.Foot[0][i].y, in.Foot[1][i].y) > cfg.AirFootHeight &&
                         in.Pelvis[i].y > cfg.StandingPelvis + cfg.AirPelvisRise
                     ? 1
                     : 0;
    for (size_t i = 0; i < n;) {
        if (!air[i]) { ++i; continue; }
        size_t j = i;
        while (j < n && air[j]) ++j;
        if ((int)(j - i) < cfg.AirMinFrames) std::fill(air.begin() + (long)i, air.begin() + (long)j, 0);
        i = j;
    }

    const float strafeCos = std::cos(cfg.StrafeAngleDeg * MotionMath::kPi / 180.0f);
    const size_t base = m_Data.size();
    m_Data.resize(base + n * kDims);
    for (size_t i = 0; i < n; ++i) {
        const glm::mat4 R = RootSynth::RootMatrix(root.Position[i], root.Yaw[i], in.RestYaw);
        const glm::mat4 inv = glm::inverse(R);
        const glm::mat3 invRot(inv);
        float* f = m_Data.data() + base + i * kDims;
        const auto put3 = [&](int at, const glm::vec3& v) { f[at] = v.x; f[at + 1] = v.y; f[at + 2] = v.z; };
        put3(0, glm::vec3(inv * glm::vec4(in.Foot[0][i], 1.0f)));
        put3(3, glm::vec3(inv * glm::vec4(in.Foot[1][i], 1.0f)));
        put3(6, invRot * vel(in.Foot[0], i));
        put3(9, invRot * vel(in.Foot[1], i));
        put3(12, invRot * vel(in.Pelvis, i));
        for (int k = 0; k < 3; ++k) {
            const size_t j = std::min(n - 1, i + (size_t)std::max(0, cfg.TrajFrames[k]));
            const glm::vec3 p = glm::vec3(inv * glm::vec4(root.Position[j], 1.0f));
            const glm::vec3 d = invRot * MotionMath::YawDir(root.Yaw[j]);
            f[15 + 2 * k] = p.x;
            f[16 + 2 * k] = p.z;
            f[21 + 2 * k] = d.x;
            f[22 + 2 * k] = d.z;
        }

        // Labels.
        const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        const glm::vec3 travel = b > a ? (root.Position[b] - root.Position[a]) * (fps / (float)(b - a)) : glm::vec3(0.0f);
        const float speed = glm::length(travel);
        std::uint8_t gait = MotionLabel::Idle;
        if (speed >= cfg.GaitSpeeds[2]) gait = MotionLabel::Run;
        else if (speed >= cfg.GaitSpeeds[1]) gait = MotionLabel::Jog;
        else if (speed >= cfg.GaitSpeeds[0]) gait = MotionLabel::Walk;
        std::uint8_t flags = 0;
        const float pelvisH = in.Pelvis[i].y;
        if (pelvisH < cfg.ProneHeight) flags |= MotionLabel::Excluded;
        // (Under jog speed: a run leans forward and drops its pelvis as low as a crouch-walk's.)
        else if (pelvisH < cfg.CrouchRatio * cfg.StandingPelvis && speed < cfg.GaitSpeeds[1] && !air[i]) flags |= MotionLabel::Crouch;
        if (speed > cfg.StrafeMinSpeed && glm::dot(travel / speed, MotionMath::YawDir(root.Yaw[i])) < strafeCos)
            flags |= MotionLabel::Strafe;
        if (air[i]) flags |= MotionLabel::Airborne;
        if (i + (size_t)std::max(0, cfg.TrajFrames[2]) >= n) flags |= MotionLabel::NoSearch;
        if (contact[0][i]) flags |= MotionLabel::ContactL;
        if (contact[1][i]) flags |= MotionLabel::ContactR;
        m_Gait.push_back(gait);
        m_Flags.push_back(flags);
    }
}

void MotionFeatureSet::Finish() {
    const size_t n = FrameCount();
    m_Mean.assign(kDims, 0.0f);
    m_Std.assign(kDims, 1.0f);
    if (n == 0) return;
    for (int d = 0; d < kDims; ++d) {
        double s = 0.0, sq = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double v = m_Data[i * kDims + (size_t)d];
            s += v;
            sq += v * v;
        }
        const double mean = s / (double)n;
        m_Mean[(size_t)d] = (float)mean;
        m_Std[(size_t)d] = (float)std::max(1e-4, std::sqrt(std::max(0.0, sq / (double)n - mean * mean)));
    }
}

namespace {
template <class T> void Put(std::vector<char>& b, const T& v) {
    const char* p = reinterpret_cast<const char*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
void PutStr(std::vector<char>& b, const std::string& s) {
    Put(b, (std::uint16_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}
struct Reader {
    const std::vector<char>& B;
    size_t At = 0;
    bool Ok = true;
    template <class T> T Get() {
        T v{};
        if (At + sizeof(T) > B.size()) { Ok = false; return v; }
        std::memcpy(&v, B.data() + At, sizeof(T));
        At += sizeof(T);
        return v;
    }
    std::string Str() {
        const std::uint16_t n = Get<std::uint16_t>();
        if (!Ok || At + n > B.size()) { Ok = false; return {}; }
        std::string s(B.data() + At, n);
        At += n;
        return s;
    }
    void Raw(void* dst, size_t n) {
        if (At + n > B.size()) { Ok = false; return; }
        std::memcpy(dst, B.data() + At, n);
        At += n;
    }
};
} // namespace

bool MotionFeatureSet::Save(const std::string& path, std::string* error) const {
    std::vector<char> b;
    b.reserve(Bytes() + 4096);
    b.insert(b.end(), {'T', 'M', 'M', 'D'});
    Put(b, kVersion);
    Put(b, FrameCount());
    Put(b, (std::uint32_t)kDims);
    Put(b, (std::uint32_t)m_Clips.size());
    for (int k = 0; k < 3; ++k) Put(b, (std::uint32_t)TrajFrames[k]);
    for (int d = 0; d < kDims; ++d) {
        PutStr(b, Names()[d]);
        Put(b, d < (int)m_Mean.size() ? m_Mean[(size_t)d] : 0.0f);
        Put(b, d < (int)m_Std.size() ? m_Std[(size_t)d] : 1.0f);
    }
    for (const Clip& c : m_Clips) {
        PutStr(b, c.Name);
        Put(b, c.First);
        Put(b, c.Count);
    }
    b.insert(b.end(), (const char*)m_Data.data(), (const char*)(m_Data.data() + m_Data.size()));
    b.insert(b.end(), m_Gait.begin(), m_Gait.end());
    b.insert(b.end(), m_Flags.begin(), m_Flags.end());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(b.data(), (std::streamsize)b.size())) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool MotionFeatureSet::Load(const std::string& path, std::string* error) {
    std::vector<char> buf;
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            if (error) *error = "cannot read " + path;
            return false;
        }
        buf.resize((size_t)f.tellg());
        f.seekg(0);
        f.read(buf.data(), (std::streamsize)buf.size());
    }
    Reader r{buf};
    char magic[4];
    r.Raw(magic, 4);
    const std::uint32_t version = r.Get<std::uint32_t>(), frames = r.Get<std::uint32_t>(), dims = r.Get<std::uint32_t>(),
                        clips = r.Get<std::uint32_t>();
    if (!r.Ok || std::memcmp(magic, "TMMD", 4) != 0 || version != kVersion || dims != (std::uint32_t)kDims) {
        if (error) *error = path + ": not a version " + std::to_string(kVersion) + " .mmdb";
        return false;
    }
    for (int k = 0; k < 3; ++k) TrajFrames[k] = (int)r.Get<std::uint32_t>();
    m_Mean.assign(kDims, 0.0f);
    m_Std.assign(kDims, 1.0f);
    for (int d = 0; d < kDims; ++d) {
        r.Str();
        m_Mean[(size_t)d] = r.Get<float>();
        m_Std[(size_t)d] = r.Get<float>();
    }
    m_Clips.resize(clips);
    for (Clip& c : m_Clips) {
        c.Name = r.Str();
        c.First = r.Get<std::uint32_t>();
        c.Count = r.Get<std::uint32_t>();
    }
    m_Data.resize((size_t)frames * kDims);
    r.Raw(m_Data.data(), m_Data.size() * sizeof(float));
    m_Gait.resize(frames);
    r.Raw(m_Gait.data(), frames);
    m_Flags.resize(frames);
    r.Raw(m_Flags.data(), frames);
    if (!r.Ok || r.At != buf.size()) {
        if (error) *error = path + ": truncated or corrupt";
        m_Data.clear(); m_Gait.clear(); m_Flags.clear(); m_Clips.clear();
        return false;
    }
    return true;
}
