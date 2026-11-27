#include "MotionFeatures.h"

#include "MotionMath.h"
#include "RootSynth.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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

const char* MotionLabel::StyleName(int s) {
    switch (s) {
        case StyleStand: return "stand";
        case StyleStroll: return "stroll";
        case StyleWalk: return "walk";
        case StyleJog: return "jog";
        case StyleRun: return "run";
        case StyleSprint: return "sprint";
        case StyleCrouch: return "crouch";
        default: return "?";
    }
}

int MotionLabel::StyleFromName(const std::string& name) {
    for (int k = 0; k < StyleCount; ++k)
        if (name == StyleName(k)) return k;
    return -1;
}

MotionClipIntent MotionClipIntent::FromTags(const std::string& tags) {
    MotionClipIntent out;
    std::vector<std::string> t;
    for (size_t at = 0; at <= tags.size();) {
        const size_t end = std::min(tags.find(';', at), tags.size());
        if (end > at) t.push_back(tags.substr(at, end - at));
        at = end + 1;
    }
    for (const std::string& tag : t) {
        if (tag == "jump") out.Jump = true;
        if (tag.rfind("style=", 0) != 0) continue;
        const std::string list = tag.substr(6);
        for (size_t at = 0; at <= list.size();) {
            const size_t end = std::min(list.find('+', at), list.size());
            const int st = MotionLabel::StyleFromName(list.substr(at, end - at));
            if (st >= 0 && std::find(out.Styles.begin(), out.Styles.end(), st) == out.Styles.end()) out.Styles.push_back(st);
            at = end + 1;
        }
    }
    if (out.Styles.empty() && !t.empty()) {
        const std::string& group = t.front();
        if (group == "run") out.Styles = {MotionLabel::StyleWalk, MotionLabel::StyleRun};
        else if (const int st = MotionLabel::StyleFromName(group); st > MotionLabel::StyleStand) out.Styles = {st};
    }
    return out;
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
                               const MotionFeatureConfig& cfg, const MotionClipIntent& intent) {
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
    // Only a jump take's flights are jumps (MotionClipIntent: a stride's flight passes the same test).
    std::vector<char> air(n, 0);
    for (size_t i = 0; i < n && intent.Jump; ++i)
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

    // The style: the take's one, or per frame the nearest of its styles' cruise speeds to the stride-averaged speed. A start
    // is the style of the cruise it reaches (its first plateau), a stop of the cruise it leaves (its last) - a sprint's stop
    // brakes through walk speeds, but it is the sprint's - and a frame under the walk-from speed takes the nearest moving
    // frame's.
    std::vector<std::uint8_t> style(n, (std::uint8_t)(intent.Styles.size() == 1 ? intent.Styles[0] : MotionLabel::StyleStand));
    if (intent.Styles.size() > 1) {
        std::vector<float> speed(n, 0.0f), avg(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const size_t a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
            glm::vec3 d = root.Position[b] - root.Position[a];
            d.y = 0.0f;
            speed[i] = b > a ? glm::length(d) * fps / (float)(b - a) : 0.0f;
        }
        const long half = std::max(1L, (long)std::lround(0.5f * cfg.StyleWindowSeconds * fps));
        double sum = 0.0;
        long lo = 0, hi = -1;
        for (long i = 0; i < (long)n; ++i) {
            while (hi < std::min((long)n - 1, i + half)) sum += speed[(size_t)++hi];
            while (lo < i - half) sum -= speed[(size_t)lo++];
            avg[(size_t)i] = (float)(sum / (double)(hi - lo + 1));
        }
        const auto nearest = [&](float v) {
            int best = intent.Styles[0];
            for (int st : intent.Styles)
                if (st != MotionLabel::StyleCrouch &&
                    (best == MotionLabel::StyleCrouch || std::fabs(v - cfg.StyleSpeeds[st]) < std::fabs(v - cfg.StyleSpeeds[best])))
                    best = st;
            return (std::uint8_t)best;
        };
        std::vector<char> moving(n, 0);
        for (size_t i = 0; i < n; ++i) moving[i] = avg[i] >= cfg.GaitSpeeds[0];
        const long look = std::max(1L, (long)std::lround(0.5f * fps)); // a plateau: no faster within half a second
        for (long a = 0; a < (long)n;) {
            if (!moving[(size_t)a]) { ++a; continue; }
            long b = a;
            while (b < (long)n && moving[(size_t)b]) ++b;
            long first = a, last = b - 1;
            for (long k = a; k < b; ++k) {
                bool top = true;
                for (long j = k + 1; j < std::min(b, k + look + 1) && top; ++j) top = avg[(size_t)j] <= avg[(size_t)k];
                if (top) { first = k; break; }
            }
            for (long k = b - 1; k >= a; --k) {
                bool top = true;
                for (long j = std::max(a, k - look); j < k && top; ++j) top = avg[(size_t)j] <= avg[(size_t)k];
                if (top) { last = k; break; }
            }
            for (long i = a; i < b; ++i)
                style[(size_t)i] = nearest(avg[(size_t)(i < first ? first : i > last && last >= first ? last : i)]);
            a = b;
        }
        // Stops and standing frames: the nearest moving frame's style (back, then on).
        long lastMoving = -1;
        std::vector<long> prev(n, -1), next(n, -1);
        for (long i = 0; i < (long)n; ++i) { if (moving[(size_t)i]) lastMoving = i; prev[(size_t)i] = lastMoving; }
        lastMoving = -1;
        for (long i = (long)n - 1; i >= 0; --i) { if (moving[(size_t)i]) lastMoving = i; next[(size_t)i] = lastMoving; }
        std::vector<std::uint8_t> fixed = style;
        for (long i = 0; i < (long)n; ++i) {
            if (moving[(size_t)i]) continue;
            const long p = prev[(size_t)i], q = next[(size_t)i];
            const long src = p < 0 ? q : q < 0 ? p : (i - p <= q - i ? p : q);
            if (src >= 0) fixed[(size_t)i] = style[(size_t)src];
        }
        style.swap(fixed);
    }
    const bool mixedCrouch = intent.Styles.size() > 1 &&
                             std::find(intent.Styles.begin(), intent.Styles.end(), (int)MotionLabel::StyleCrouch) != intent.Styles.end();

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
        // (A mixed take's crouched frames are its crouch, when it holds one.)
        m_Style.push_back(mixedCrouch && (flags & MotionLabel::Crouch) ? (std::uint8_t)MotionLabel::StyleCrouch : style[i]);
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
    // Quantize: each dimension's range in 65534 steps around its middle.
    m_QScale.assign(kDims, 1.0f);
    m_QOffset.assign(kDims, 0.0f);
    m_QError.assign(kDims, 0.0f);
    for (int d = 0; d < kDims; ++d) {
        float lo = m_Data[(size_t)d], hi = lo;
        for (size_t i = 0; i < n; ++i) {
            lo = std::min(lo, m_Data[i * kDims + (size_t)d]);
            hi = std::max(hi, m_Data[i * kDims + (size_t)d]);
        }
        m_QOffset[(size_t)d] = 0.5f * (lo + hi);
        m_QScale[(size_t)d] = std::max((hi - lo) / 65534.0f, 1e-7f);
    }
    m_Q.resize(n * kDims);
    for (size_t i = 0; i < n; ++i)
        for (int d = 0; d < kDims; ++d) {
            const float x = m_Data[i * kDims + (size_t)d];
            const float q = std::round((x - m_QOffset[(size_t)d]) / m_QScale[(size_t)d]);
            m_Q[i * kDims + (size_t)d] = (std::int16_t)std::clamp(q, -32767.0f, 32767.0f);
            const float back = (float)m_Q[i * kDims + (size_t)d] * m_QScale[(size_t)d] + m_QOffset[(size_t)d];
            m_QError[(size_t)d] = std::max(m_QError[(size_t)d], std::fabs(back - x));
        }
    std::vector<float>().swap(m_Data);
}

void MotionFeatureSet::Decode(std::uint32_t frame, float* out) const {
    if (!m_Q.empty()) {
        const std::int16_t* q = Quantized(frame);
        for (int d = 0; d < kDims; ++d) out[d] = (float)q[d] * m_QScale[(size_t)d] + m_QOffset[(size_t)d];
        return;
    }
    const float* x = m_Data.data() + (size_t)frame * kDims;
    for (int d = 0; d < kDims; ++d) out[d] = x[d];
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
    // As MotionClipStream's: `count` records of at least `each` bytes still fit (a corrupt header count is refused before
    // anything is sized by it).
    bool Fits(std::uint64_t count, std::uint64_t each) {
        if (each > 0 && count > (std::uint64_t)(B.size() - At) / each) Ok = false;
        return Ok;
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
        Put(b, d < (int)m_QScale.size() ? m_QScale[(size_t)d] : 1.0f);
        Put(b, d < (int)m_QOffset.size() ? m_QOffset[(size_t)d] : 0.0f);
    }
    Put(b, (std::uint8_t)(HasNormalization() ? 1 : 0));
    if (HasNormalization())
        for (int d = 0; d < kDims; ++d) {
            Put(b, m_NormMean[(size_t)d]);
            Put(b, m_NormScale[(size_t)d]);
        }
    for (const Clip& c : m_Clips) {
        PutStr(b, c.Name);
        Put(b, c.First);
        Put(b, c.Count);
    }
    if (m_Q.size() != (size_t)FrameCount() * kDims) {
        if (error) *error = "the features aren't finished (MotionFeatureSet::Finish)";
        return false;
    }
    b.insert(b.end(), (const char*)m_Q.data(), (const char*)(m_Q.data() + m_Q.size()));
    b.insert(b.end(), m_Gait.begin(), m_Gait.end());
    b.insert(b.end(), m_Flags.begin(), m_Flags.end());
    b.insert(b.end(), m_Style.begin(), m_Style.end());
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f || !f.write(b.data(), (std::streamsize)b.size())) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool MotionFeatureSet::Load(const std::string& path, std::string* error) {
    std::vector<char> buf;
    {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
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
    if (!r.Ok || std::memcmp(magic, "TMMD", 4) != 0 || version < 2 || version > kVersion || dims != (std::uint32_t)kDims) {
        if (error) *error = path + ": not a version 2-" + std::to_string(kVersion) + " .mmdb";
        return false;
    }
    for (int k = 0; k < 3; ++k) TrajFrames[k] = (int)r.Get<std::uint32_t>();
    m_Mean.assign(kDims, 0.0f);
    m_Std.assign(kDims, 1.0f);
    m_QScale.assign(kDims, 1.0f);
    m_QOffset.assign(kDims, 0.0f);
    m_QError.clear();
    for (int d = 0; d < kDims; ++d) {
        r.Str();
        m_Mean[(size_t)d] = r.Get<float>();
        m_Std[(size_t)d] = r.Get<float>();
        m_QScale[(size_t)d] = r.Get<float>();
        m_QOffset[(size_t)d] = r.Get<float>();
    }
    m_NormMean.clear();
    m_NormScale.clear();
    if (version >= 3 && r.Get<std::uint8_t>() != 0) {
        m_NormMean.assign(kDims, 0.0f);
        m_NormScale.assign(kDims, 1.0f);
        for (int d = 0; d < kDims; ++d) {
            m_NormMean[(size_t)d] = r.Get<float>();
            m_NormScale[(size_t)d] = r.Get<float>();
        }
    }
    // The normalization divides by Std (or the frozen scale) and the search by QScale: a zero, negative or non-finite one (a
    // corrupt file) would put NaN or infinity in every cost. And every count against what the file has left (a clip is at
    // least 10 bytes, a frame kDims 16-bit features and its gait, flag and style bytes).
    bool scalesOk = true;
    for (int d = 0; d < kDims; ++d)
        scalesOk = scalesOk && std::isfinite(m_Mean[(size_t)d]) && std::isfinite(m_QOffset[(size_t)d]) &&
                   std::isfinite(m_Std[(size_t)d]) && m_Std[(size_t)d] > 0.0f && std::isfinite(m_QScale[(size_t)d]) &&
                   m_QScale[(size_t)d] > 0.0f;
    for (size_t d = 0; d < m_NormMean.size(); ++d)
        scalesOk = scalesOk && std::isfinite(m_NormMean[d]) && std::isfinite(m_NormScale[d]) && m_NormScale[d] > 0.0f;
    if (!scalesOk || !r.Fits(clips, 10) || !r.Fits(frames, kDims * sizeof(std::int16_t) + 3)) {
        if (error) *error = path + ": corrupt header";
        m_Q.clear(); m_Gait.clear(); m_Flags.clear(); m_Style.clear(); m_Clips.clear();
        return false;
    }
    m_Clips.resize(clips);
    for (Clip& c : m_Clips) {
        c.Name = r.Str();
        c.First = r.Get<std::uint32_t>();
        c.Count = r.Get<std::uint32_t>();
    }
    m_Data.clear();
    m_Q.resize((size_t)frames * kDims);
    r.Raw(m_Q.data(), m_Q.size() * sizeof(std::int16_t));
    m_Gait.resize(frames);
    r.Raw(m_Gait.data(), frames);
    m_Flags.resize(frames);
    r.Raw(m_Flags.data(), frames);
    m_Style.resize(frames);
    r.Raw(m_Style.data(), frames);
    bool clipsOk = true;
    for (size_t k = 0; k < m_Clips.size(); ++k) {
        clipsOk = clipsOk && m_Clips[k].Count > 0 && (std::uint64_t)m_Clips[k].First + m_Clips[k].Count <= frames;
        if (k > 0) clipsOk = clipsOk && m_Clips[k].First >= m_Clips[k - 1].First + m_Clips[k - 1].Count;
    }
    if (!r.Ok || r.At != buf.size() || !clipsOk) {
        if (error) *error = path + ": truncated or corrupt";
        m_Q.clear(); m_Gait.clear(); m_Flags.clear(); m_Style.clear(); m_Clips.clear();
        return false;
    }
    return true;
}
