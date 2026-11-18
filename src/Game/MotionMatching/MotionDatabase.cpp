#include "MotionDatabase.h"

#include <algorithm>
#include <chrono>
#include <cmath>

const char* MotionFeature::GroupName(int g) {
    switch (g) {
        case FootPos: return "footPos";
        case FootVel: return "footVel";
        case PelvisVel: return "pelvisVel";
        case TrajPos: return "trajPos";
        case TrajDir: return "trajDir";
        default: return "?";
    }
}

bool MotionDatabase::Load(const std::string& base, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    MotionClipStream s;
    MotionFeatureSet f;
    if (!s.Load(base + ".mmanim", error) || !f.Load(base + ".mmdb", error)) {
        m_Frames = 0;
        return false;
    }
    const bool ok = Adopt(std::move(s), std::move(f), error);
    m_LoadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

bool MotionDatabase::Adopt(MotionClipStream&& stream, MotionFeatureSet&& features, std::string* error) {
    m_Stream = std::move(stream);
    m_Feat = std::move(features);
    m_LoadSeconds = 0.0;
    return Build(error);
}

bool MotionDatabase::Build(std::string* error) {
    m_Frames = 0;
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        m_Small.clear();
        m_Large.clear();
        m_ClipOf.clear();
        return false;
    };
    const std::uint32_t n = m_Stream.FrameCount();
    if (n == 0) return fail("the stream is empty");
    if (m_Feat.FrameCount() != n) return fail("the stream and the features cover different frames");
    if (m_Stream.Clips().size() != m_Feat.Clips().size()) return fail("the stream and the features list different clips");
    for (size_t c = 0; c < m_Stream.Clips().size(); ++c) {
        const auto& a = m_Stream.Clips()[c];
        const auto& b = m_Feat.Clips()[c];
        if (a.Name != b.Name || a.First != b.First || a.Count != b.Count)
            return fail("clip " + std::to_string(c) + " differs between the stream and the features (" + a.Name + ")");
    }
    if (m_Feat.Mean().size() != (size_t)kDims || m_Feat.Std().size() != (size_t)kDims) return fail("the features have no statistics");
    if (!m_Feat.Quantized()) return fail("the features aren't finished (MotionFeatureSet::Finish)");

    m_ClipOf.assign(n, -1);
    for (size_t c = 0; c < m_Stream.Clips().size(); ++c) {
        const auto& clip = m_Stream.Clips()[c];
        for (std::uint32_t i = 0; i < clip.Count; ++i) m_ClipOf[clip.First + i] = (int)c;
    }
    for (std::uint32_t i = 0; i < n; ++i)
        if (m_ClipOf[i] < 0) return fail("frame " + std::to_string(i) + " is in no clip");
    // The approach to prone (kProneApproachSeconds), then glitches (kGlitchDegrees): excluded before the runway and the
    // boxes see the flags.
    m_ProneApproach = 0;
    {
        const int pad = (int)std::lround(kProneApproachSeconds * m_Stream.Fps());
        std::vector<char> near(n, 0);
        for (const auto& clip : m_Stream.Clips())
            for (std::uint32_t k = 0; k < clip.Count; ++k) {
                const std::uint32_t i = clip.First + k;
                if (!(m_Feat.Flags(i) & MotionLabel::Excluded)) continue;
                const std::uint32_t from = (std::uint32_t)std::max<std::int64_t>(clip.First, (std::int64_t)i - pad);
                const std::uint32_t to = std::min<std::uint32_t>(clip.First + clip.Count - 1, i + (std::uint32_t)pad);
                for (std::uint32_t j = from; j <= to; ++j) near[j] = 1;
            }
        for (std::uint32_t i = 0; i < n; ++i)
            if (near[i] && !(m_Feat.Flags(i) & MotionLabel::Excluded)) {
                m_Feat.AddFlags(i, MotionLabel::Excluded);
                ++m_ProneApproach;
            }
    }
    m_Glitches = 0;
    {
        const float limit = kGlitchDegrees * 3.14159265f / 180.0f * 60.0f / std::max(1.0f, m_Stream.Fps());
        const int nb = (int)m_Stream.Bones().size();
        std::vector<char> bad(n, 0);
        std::vector<glm::quat> prev((size_t)nb), cur((size_t)nb);
        for (const auto& clip : m_Stream.Clips()) {
            for (int b = 0; b < nb; ++b) prev[(size_t)b] = m_Stream.Rotation(clip.First, b);
            for (std::uint32_t k = 1; k < clip.Count; ++k) {
                const std::uint32_t i = clip.First + k;
                bool glitch = false;
                for (int b = 0; b < nb; ++b) {
                    cur[(size_t)b] = m_Stream.Rotation(i, b);
                    glitch = glitch || MotionMath::QuatAngle(prev[(size_t)b], cur[(size_t)b]) > limit;
                }
                std::swap(prev, cur);
                if (!glitch) continue;
                const std::uint32_t from = (std::uint32_t)std::max<std::int64_t>(clip.First, (std::int64_t)i - 1 - kGlitchPad);
                const std::uint32_t to = std::min<std::uint32_t>(clip.First + clip.Count - 1, i + (std::uint32_t)kGlitchPad);
                for (std::uint32_t j = from; j <= to; ++j) bad[j] = 1;
            }
        }
        for (std::uint32_t i = 0; i < n; ++i)
            if (bad[i] && !(m_Feat.Flags(i) & MotionLabel::Excluded)) {
                m_Feat.AddFlags(i, MotionLabel::Excluded);
                ++m_Glitches;
            }
    }
    // The idle takes (tag "idle" in the clip's ';'-separated tags): MotionLabel::IdleTake.
    for (const auto& clip : m_Stream.Clips()) {
        bool idle = false;
        for (size_t at = 0; at <= clip.Tags.size() && !idle;) {
            const size_t end = std::min(clip.Tags.find(';', at), clip.Tags.size());
            idle = clip.Tags.compare(at, end - at, "idle") == 0;
            at = end + 1;
        }
        if (idle)
            for (std::uint32_t k = 0; k < clip.Count; ++k) m_Feat.AddFlags(clip.First + k, MotionLabel::IdleTake);
    }
    // Per-group scale: the mean of the group's standard deviations.
    m_Mean.assign(m_Feat.Mean().begin(), m_Feat.Mean().end());
    m_Scale.assign(kDims, 1.0f);
    for (int g = 0; g < MotionFeature::GroupCount; ++g) {
        const int a = MotionFeature::kGroupFirst[g], b = MotionFeature::kGroupFirst[g + 1];
        float s = 0.0f;
        for (int d = a; d < b; ++d) s += m_Feat.Std()[(size_t)d];
        s = std::max(1e-4f, s / (float)(b - a));
        for (int d = a; d < b; ++d) m_Scale[(size_t)d] = s;
    }
    m_QA.assign(kDims, 1.0f);
    m_QB.assign(kDims, 0.0f);
    for (int d = 0; d < kDims; ++d) {
        m_QA[(size_t)d] = m_Feat.QScale(d) / m_Scale[(size_t)d];
        m_QB[(size_t)d] = (m_Feat.QOffset(d) - m_Mean[(size_t)d]) / m_Scale[(size_t)d];
    }

    m_Frames = n;
    Index();
    return true;
}

void MotionDatabase::Index() {
    const std::uint32_t n = m_Frames;
    // The runway: back from each take's end, how long the filterable labels stay as they are.
    m_Runway.assign(n, 1);
    for (const auto& clip : m_Stream.Clips())
        for (std::uint32_t k = clip.Count; k-- > 1;) {
            const std::uint32_t i = clip.First + k - 1;
            if ((m_Feat.Flags(i) & kRunwayMask) == (m_Feat.Flags(i + 1) & kRunwayMask))
                m_Runway[i] = (std::uint16_t)std::min<int>(65535, m_Runway[i + 1] + 1);
        }


    // Boxes: blocks of 16 and 64 frames over the whole database (a block may straddle two clips; the bounds stay valid).
    const auto boxes = [&](int size, std::vector<Box>& out) {
        out.assign((n + (std::uint32_t)size - 1) / (std::uint32_t)size, Box{});
        for (size_t b = 0; b < out.size(); ++b) {
            Box& box = out[b];
            std::fill(std::begin(box.Min), std::end(box.Min), (std::int16_t)32767);
            std::fill(std::begin(box.Max), std::end(box.Max), (std::int16_t)-32767);
            const std::uint32_t from = (std::uint32_t)b * (std::uint32_t)size, to = std::min(n, from + (std::uint32_t)size);
            for (std::uint32_t i = from; i < to; ++i) {
                const std::int16_t* x = Quantized(i);
                for (int d = 0; d < kDims; ++d) {
                    box.Min[d] = std::min(box.Min[d], x[d]);
                    box.Max[d] = std::max(box.Max[d], x[d]);
                }
                box.FlagOr |= Flags(i);
                box.FlagAnd &= Flags(i);
                box.GaitMin = std::min(box.GaitMin, Gait(i));
                box.GaitMax = std::max(box.GaitMax, Gait(i));
                box.Styles |= (std::uint8_t)(1u << std::min<int>(Style(i), 7));
            }
        }
    };
    boxes(kSmallBox, m_Small);
    boxes(kLargeBox, m_Large);
}

std::uint32_t MotionDatabase::ExcludeFrames(const std::vector<char>& bad) {
    std::uint32_t added = 0;
    for (std::uint32_t i = 0; i < m_Frames && i < (std::uint32_t)bad.size(); ++i)
        if (bad[i] && !(m_Feat.Flags(i) & MotionLabel::Excluded)) {
            m_Feat.AddFlags(i, MotionLabel::Excluded);
            ++added;
        }
    if (added) Index();
    return added;
}

std::uint32_t MotionDatabase::ClipEnd(std::uint32_t frame) const {
    const auto& c = Clip(ClipOf(frame));
    return c.First + c.Count - 1;
}

void MotionDatabase::Normalize(const float* raw, float* out) const {
    for (int d = 0; d < kDims; ++d) out[d] = (raw[d] - m_Mean[(size_t)d]) / m_Scale[(size_t)d];
}

void MotionDatabase::Denormalize(const float* norm, float* out) const {
    for (int d = 0; d < kDims; ++d) out[d] = norm[d] * m_Scale[(size_t)d] + m_Mean[(size_t)d];
}

float MotionDatabase::SecondsInClip(std::uint32_t frame) const {
    return (float)(frame - Clip(ClipOf(frame)).First) / Fps();
}

void MotionDatabase::SamplePose(std::uint32_t frame, float frac, MotionPose& out) const {
    const size_t nb = m_Stream.Bones().size();
    out.Rot.resize(nb);
    frame = std::min(frame, m_Frames - 1);
    const std::uint32_t next = std::min(frame + 1, ClipEnd(frame));
    frac = next == frame ? 0.0f : std::clamp(frac, 0.0f, 1.0f);
    const int clip = ClipOf(frame);
    const size_t nf = m_Stream.Statics.size();
    out.Fingers.resize(nf);
    for (size_t k = 0; k < nf; ++k) {
        const glm::quat a = m_Stream.FingerRotation(clip, frame, (int)k);
        if (frac <= 1e-4f || !m_Stream.HasFingers(clip)) {
            out.Fingers[k] = a;
            continue;
        }
        glm::quat q = m_Stream.FingerRotation(clip, next, (int)k);
        if (glm::dot(a, q) < 0.0f) q = -q;
        out.Fingers[k] = glm::normalize(a * (1.0f - frac) + q * frac);
    }
    for (size_t b = 0; b < nb; ++b) {
        const glm::quat a = m_Stream.Rotation(frame, (int)b);
        if (frac <= 1e-4f) {
            out.Rot[b] = a;
            continue;
        }
        glm::quat q = m_Stream.Rotation(next, (int)b);
        if (glm::dot(a, q) < 0.0f) q = -q;
        out.Rot[b] = glm::normalize(a * (1.0f - frac) + q * frac);
    }
    out.Pelvis = glm::mix(m_Stream.Pelvis(frame), m_Stream.Pelvis(next), frac);
}

std::array<float, 3> MotionDatabase::TrajSeconds() const {
    return {(float)m_Feat.TrajFrames[0] / Fps(), (float)m_Feat.TrajFrames[1] / Fps(), (float)m_Feat.TrajFrames[2] / Fps()};
}

std::array<float, MotionDatabase::kDims> MotionDatabase::Raw(std::uint32_t frame) const {
    std::array<float, kDims> out{};
    m_Feat.Decode(frame, out.data());
    return out;
}

std::array<float, MotionDatabase::kDims> MotionDatabase::Normalized(std::uint32_t frame) const {
    std::array<float, kDims> out{};
    const std::int16_t* q = Quantized(frame);
    for (int d = 0; d < kDims; ++d) out[(size_t)d] = (float)q[d] * m_QA[(size_t)d] + m_QB[(size_t)d];
    return out;
}

size_t MotionDatabase::FeatureBytes() const {
    return m_Feat.Bytes() + (m_Small.size() + m_Large.size()) * sizeof(Box) + m_ClipOf.size() * sizeof(int) +
           m_Runway.size() * sizeof(std::uint16_t);
}
