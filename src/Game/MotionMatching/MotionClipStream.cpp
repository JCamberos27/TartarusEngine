#include "MotionClipStream.h"

#include "RootSynth.h"

#include "Animation.h"
#include "Model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

// Little binary writer / reader over a byte vector (the files are read whole: one read, then parsed in memory).
struct Out {
    std::vector<char> B;
    template <class T> void Put(const T& v) {
        const char* p = reinterpret_cast<const char*>(&v);
        B.insert(B.end(), p, p + sizeof(T));
    }
    void Str(const std::string& s) {
        const std::uint16_t n = (std::uint16_t)std::min<size_t>(s.size(), 65535);
        Put(n);
        B.insert(B.end(), s.begin(), s.begin() + n);
    }
    void Raw(const void* p, size_t n) { B.insert(B.end(), (const char*)p, (const char*)p + n); }
};
struct In {
    const char* P = nullptr;
    size_t N = 0, At = 0;
    bool Ok = true;
    template <class T> T Get() {
        T v{};
        if (At + sizeof(T) > N) { Ok = false; return v; }
        std::memcpy(&v, P + At, sizeof(T));
        At += sizeof(T);
        return v;
    }
    std::string Str() {
        const std::uint16_t n = Get<std::uint16_t>();
        if (!Ok || At + n > N) { Ok = false; return {}; }
        std::string s(P + At, n);
        At += n;
        return s;
    }
    bool Raw(void* dst, size_t n) {
        if (At + n > N) { Ok = false; return false; }
        std::memcpy(dst, P + At, n);
        At += n;
        return true;
    }
};

bool ReadWhole(const std::string& path, std::vector<char>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n < 0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    return (bool)f.read(out.data(), n);
}

bool WriteWhole(const std::string& path, const std::vector<char>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(b.data(), (std::streamsize)b.size());
    return (bool)f;
}

} // namespace

void MotionClipStream::Reset(float fps, const std::vector<std::string>& bones) {
    m_Fps = fps;
    m_Bones = bones;
    m_Clips.clear();
    m_Root.clear();
    m_Pelvis.clear();
    m_Rot.clear();
    Statics.clear();
}

void MotionClipStream::AddClip(const Clip& info, const std::vector<RootSample>& root, const std::vector<glm::vec3>& pelvis,
                               const std::vector<glm::quat>& rotations) {
    const size_t n = root.size(), nb = m_Bones.size();
    if (pelvis.size() != n || rotations.size() != n * nb || n == 0) return;
    Clip c = info;
    c.First = (std::uint32_t)m_Root.size();
    c.Count = (std::uint32_t)n;
    m_Clips.push_back(c);
    m_Root.insert(m_Root.end(), root.begin(), root.end());
    m_Pelvis.insert(m_Pelvis.end(), pelvis.begin(), pelvis.end());
    m_Rot.reserve(m_Rot.size() + rotations.size());
    for (const glm::quat& q : rotations) m_Rot.push_back(MotionMath::PackQuat(q));
}

bool MotionClipStream::Save(const std::string& path, std::string* error) const {
    Out o;
    o.Raw("TMMA", 4);
    o.Put(kVersion);
    o.Put(m_Fps);
    o.Put((std::uint32_t)m_Root.size());
    o.Put((std::uint32_t)m_Bones.size());
    o.Put((std::uint32_t)Statics.size());
    o.Put((std::uint32_t)m_Clips.size());
    o.Put(SourcePelvisHeight);
    o.Put(RestYaw);
    for (const std::string& b : m_Bones) o.Str(b);
    for (const StaticBone& s : Statics) {
        o.Str(s.Name);
        o.Put(s.Rotation.x); o.Put(s.Rotation.y); o.Put(s.Rotation.z); o.Put(s.Rotation.w);
    }
    for (const Clip& c : m_Clips) {
        o.Str(c.Name);
        o.Str(c.Tags);
        o.Put(c.First);
        o.Put(c.Count);
        o.Put(c.SourceStart);
    }
    o.B.reserve(o.B.size() + PoseBytes());
    for (const RootSample& r : m_Root) { o.Put(r.Position.x); o.Put(r.Position.z); o.Put(r.Yaw); }
    o.Raw(m_Pelvis.data(), m_Pelvis.size() * sizeof(glm::vec3));
    o.Raw(m_Rot.data(), m_Rot.size() * sizeof(MotionMath::PackedQuat));
    if (!WriteWhole(path, o.B)) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

bool MotionClipStream::Load(const std::string& path, std::string* error) {
    std::vector<char> buf;
    if (!ReadWhole(path, buf)) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    In in{buf.data(), buf.size()};
    char magic[4];
    in.Raw(magic, 4);
    const std::uint32_t version = in.Get<std::uint32_t>();
    if (!in.Ok || std::memcmp(magic, "TMMA", 4) != 0 || version != kVersion) {
        if (error) *error = path + ": not a version " + std::to_string(kVersion) + " .mmanim";
        return false;
    }
    const float fps = in.Get<float>();
    const std::uint32_t frames = in.Get<std::uint32_t>(), bones = in.Get<std::uint32_t>(),
                        statics = in.Get<std::uint32_t>(), clips = in.Get<std::uint32_t>();
    SourcePelvisHeight = in.Get<float>();
    RestYaw = in.Get<float>();
    std::vector<std::string> names(bones);
    for (auto& n : names) n = in.Str();
    Reset(fps, names);
    Statics.resize(statics);
    for (StaticBone& s : Statics) {
        s.Name = in.Str();
        s.Rotation.x = in.Get<float>(); s.Rotation.y = in.Get<float>();
        s.Rotation.z = in.Get<float>(); s.Rotation.w = in.Get<float>();
    }
    m_Clips.resize(clips);
    for (Clip& c : m_Clips) {
        c.Name = in.Str();
        c.Tags = in.Str();
        c.First = in.Get<std::uint32_t>();
        c.Count = in.Get<std::uint32_t>();
        c.SourceStart = in.Get<float>();
    }
    m_Root.resize(frames);
    for (RootSample& r : m_Root) {
        r.Position.x = in.Get<float>();
        r.Position.y = 0.0f;
        r.Position.z = in.Get<float>();
        r.Yaw = in.Get<float>();
    }
    m_Pelvis.resize(frames);
    in.Raw(m_Pelvis.data(), m_Pelvis.size() * sizeof(glm::vec3));
    m_Rot.resize((size_t)frames * bones);
    in.Raw(m_Rot.data(), m_Rot.size() * sizeof(MotionMath::PackedQuat));
    bool clipsOk = true;
    for (const Clip& c : m_Clips) clipsOk = clipsOk && c.Count > 0 && (size_t)c.First + c.Count <= frames;
    if (!in.Ok || in.At != buf.size() || !clipsOk) {
        if (error) *error = path + ": truncated or corrupt";
        Reset(60.0f, {});
        return false;
    }
    return true;
}

int MotionClipStream::BoneIndex(const std::string& name) const {
    for (size_t i = 0; i < m_Bones.size(); ++i)
        if (m_Bones[i] == name) return (int)i;
    return -1;
}

int MotionClipStream::FindClip(const std::string& name) const {
    for (size_t i = 0; i < m_Clips.size(); ++i)
        if (m_Clips[i].Name == name) return (int)i;
    return -1;
}

float MotionClipStream::ClipSeconds(int clip) const {
    if (clip < 0 || clip >= (int)m_Clips.size()) return 0.0f;
    return (float)(m_Clips[(size_t)clip].Count - 1) / m_Fps;
}

size_t MotionClipStream::PoseBytes() const {
    return m_Root.size() * 3 * sizeof(float) + m_Pelvis.size() * sizeof(glm::vec3) +
           m_Rot.size() * sizeof(MotionMath::PackedQuat);
}

MotionClipStream::RootSample MotionClipStream::RootAt(int clip, float seconds) const {
    if (clip < 0 || clip >= (int)m_Clips.size()) return {};
    const Clip& c = m_Clips[(size_t)clip];
    const float f = std::clamp(seconds * m_Fps, 0.0f, (float)(c.Count - 1));
    const std::uint32_t i0 = (std::uint32_t)f, i1 = std::min(i0 + 1, c.Count - 1);
    const float t = f - (float)i0;
    const RootSample& a = m_Root[c.First + i0];
    const RootSample& b = m_Root[c.First + i1];
    RootSample r;
    r.Position = glm::mix(a.Position, b.Position, t);
    r.Yaw = a.Yaw + (b.Yaw - a.Yaw) * t;
    return r;
}

glm::mat4 MotionClipStream::RootMatrixAt(int clip, float seconds) const {
    const RootSample r = RootAt(clip, seconds);
    return RootSynth::RootMatrix(r.Position, r.Yaw, RestYaw);
}

MotionClipStream::Binding MotionClipStream::BindNames(const std::vector<std::string>& nodeNames, int pelvisNode,
                                                      float pelvisHeight) const {
    Binding b;
    const auto find = [&](const std::string& n) {
        for (size_t i = 0; i < nodeNames.size(); ++i)
            if (nodeNames[i] == n) return (int)i;
        return -1;
    };
    b.Nodes.resize(m_Bones.size());
    for (size_t i = 0; i < m_Bones.size(); ++i) {
        b.Nodes[i] = find(m_Bones[i]);
        if (b.Nodes[i] >= 0) ++b.Bound;
    }
    b.StaticNodes.resize(Statics.size());
    for (size_t i = 0; i < Statics.size(); ++i) b.StaticNodes[i] = find(Statics[i].Name);
    b.Pelvis = pelvisNode;
    b.PelvisScale = pelvisHeight > 0.0f && SourcePelvisHeight > 0.0f ? pelvisHeight / SourcePelvisHeight : 1.0f;
    return b;
}

MotionClipStream::Binding MotionClipStream::Bind(const Model& model, float pelvisHeight) const {
    std::vector<std::string> names((size_t)model.NodeCount());
    for (int i = 0; i < model.NodeCount(); ++i) names[(size_t)i] = model.NodeName(i);
    return BindNames(names, model.NodeIndex("pelvis"), pelvisHeight);
}

void MotionClipStream::FramePose(std::uint32_t frame, const Binding& binding, std::vector<LocalTRS>& pose) const {
    if (frame >= FrameCount()) return;
    const size_t nb = m_Bones.size();
    for (size_t i = 0; i < nb && i < binding.Nodes.size(); ++i) {
        const int node = binding.Nodes[i];
        if (node >= 0 && node < (int)pose.size()) pose[(size_t)node].R = Rotation(frame, (int)i);
    }
    for (size_t i = 0; i < Statics.size() && i < binding.StaticNodes.size(); ++i) {
        const int node = binding.StaticNodes[i];
        if (node >= 0 && node < (int)pose.size()) pose[(size_t)node].R = Statics[i].Rotation;
    }
    if (binding.Pelvis >= 0 && binding.Pelvis < (int)pose.size()) pose[(size_t)binding.Pelvis].T = m_Pelvis[frame] * binding.PelvisScale;
}

void MotionClipStream::SamplePose(int clip, float seconds, const Binding& binding, std::vector<LocalTRS>& pose) const {
    if (clip < 0 || clip >= (int)m_Clips.size()) return;
    const Clip& c = m_Clips[(size_t)clip];
    const float f = std::clamp(seconds * m_Fps, 0.0f, (float)(c.Count - 1));
    const std::uint32_t i0 = (std::uint32_t)f, i1 = std::min(i0 + 1, c.Count - 1);
    const float t = f - (float)i0;
    const std::uint32_t g0 = c.First + i0, g1 = c.First + i1;
    FramePose(g0, binding, pose);
    if (t <= 1e-4f || g0 == g1) return;
    const size_t nb = m_Bones.size();
    for (size_t i = 0; i < nb && i < binding.Nodes.size(); ++i) {
        const int node = binding.Nodes[i];
        if (node < 0 || node >= (int)pose.size()) continue;
        const glm::quat a = Rotation(g0, (int)i);
        glm::quat b = Rotation(g1, (int)i);
        if (glm::dot(a, b) < 0.0f) b = -b;
        pose[(size_t)node].R = glm::normalize(a * (1.0f - t) + b * t);
    }
    if (binding.Pelvis >= 0 && binding.Pelvis < (int)pose.size())
        pose[(size_t)binding.Pelvis].T = glm::mix(m_Pelvis[g0], m_Pelvis[g1], t) * binding.PelvisScale;
}
