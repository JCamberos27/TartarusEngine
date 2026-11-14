#include "GIData.h"

#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace GI {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kY0 = 0.282094792f; // Y_00
constexpr float kY1 = 0.488602512f; // Y_1m / its axis component
constexpr char kMagic[4] = {'T', 'G', 'I', '1'};
constexpr std::uint32_t kVersion = 1;

float AreaElement(float x, float y) { return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f)); }

struct Writer {
    std::vector<std::uint8_t> out;
    void Raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
    void U32(std::uint32_t v) { Raw(&v, 4); }
    void I32(std::int32_t v) { Raw(&v, 4); }
    void F32(float v) { Raw(&v, 4); }
    void Vec3(const glm::vec3& v) { F32(v.x); F32(v.y); F32(v.z); }
    void Str(const std::string& s) { U32((std::uint32_t)s.size()); Raw(s.data(), s.size()); }
};

struct Reader {
    const std::uint8_t* p;
    std::size_t left;
    bool ok = true;
    bool Raw(void* dst, std::size_t n) {
        if (!ok || n > left) { ok = false; return false; }
        std::memcpy(dst, p, n);
        p += n;
        left -= n;
        return true;
    }
    std::uint32_t U32() { std::uint32_t v = 0; Raw(&v, 4); return v; }
    std::int32_t I32() { std::int32_t v = 0; Raw(&v, 4); return v; }
    float F32() { float v = 0.0f; Raw(&v, 4); return v; }
    glm::vec3 Vec3() { glm::vec3 v; v.x = F32(); v.y = F32(); v.z = F32(); return v; }
    std::string Str() {
        const std::uint32_t n = U32();
        if (!ok || n > left || n > 4096) { ok = false; return {}; }
        std::string s((const char*)p, n);
        p += n;
        left -= n;
        return s;
    }
};

} // namespace

glm::vec3 Evaluate(const ProbeSH& sh, const glm::vec3& n) {
    const glm::vec4 b(1.0f, n.x, n.y, n.z);
    return glm::max(glm::vec3(glm::dot(sh.c[0], b), glm::dot(sh.c[1], b), glm::dot(sh.c[2], b)), glm::vec3(0.0f));
}

void Projector::Add(const glm::vec3& dir, const glm::vec3& radiance, float solidAngle) {
    const glm::vec4 y(kY0, kY1 * dir.x, kY1 * dir.y, kY1 * dir.z);
    for (int c = 0; c < 3; ++c) m_L[c] += y * (radiance[c] * solidAngle);
    m_Weight += solidAngle;
}

ProbeSH Projector::Finish() const {
    ProbeSH out;
    if (!(m_Weight > 0.0f)) return out;
    const float norm = 4.0f * kPi / m_Weight; // exact sphere coverage whatever the sampling's rounding
    for (int c = 0; c < 3; ++c) {
        const glm::vec4 L = m_L[c] * norm;
        // E(n)/pi = sum_l (A_l/pi) L_lm Y_lm(n), A_0 = pi, A_1 = 2pi/3.
        out.c[c] = glm::vec4(kY0 * L.x, (2.0f / 3.0f) * kY1 * L.y, (2.0f / 3.0f) * kY1 * L.z, (2.0f / 3.0f) * kY1 * L.w);
    }
    return out;
}

float CubeTexelSolidAngle(int x, int y, int size) {
    const float inv = 1.0f / (float)size;
    const float u0 = 2.0f * x * inv - 1.0f, u1 = 2.0f * (x + 1) * inv - 1.0f;
    const float v0 = 2.0f * y * inv - 1.0f, v1 = 2.0f * (y + 1) * inv - 1.0f;
    return AreaElement(u0, v0) - AreaElement(u0, v1) - AreaElement(u1, v0) + AreaElement(u1, v1);
}

glm::vec3 CubeTexelDir(int face, int x, int y, int size) {
    const float u = 2.0f * (x + 0.5f) / (float)size - 1.0f; // sc
    const float v = 2.0f * (y + 0.5f) / (float)size - 1.0f; // tc
    glm::vec3 d;
    switch (face) {
        case 0: d = {1.0f, -v, -u}; break;  // +X
        case 1: d = {-1.0f, -v, u}; break;  // -X
        case 2: d = {u, 1.0f, v}; break;    // +Y
        case 3: d = {u, -1.0f, -v}; break;  // -Y
        case 4: d = {u, -v, 1.0f}; break;   // +Z
        default: d = {-u, -v, -1.0f}; break; // -Z
    }
    return glm::normalize(d);
}

Box Box::FromTransform(const glm::mat4& world, const glm::vec3& size) {
    Box b;
    b.Center = glm::vec3(world[3]);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 col(world[i]);
        const float len = glm::length(col);
        b.Axis[i] = len > 1e-8f ? col / len : glm::vec3(i == 0, i == 1, i == 2);
        b.Half[i] = std::abs(size[i]) * len * 0.5f;
    }
    return b;
}

glm::vec3 Box::ToLocal(const glm::vec3& p) const {
    const glm::vec3 d = p - Center;
    return {glm::dot(Axis[0], d), glm::dot(Axis[1], d), glm::dot(Axis[2], d)};
}

glm::vec3 Box::ToWorld(const glm::vec3& q) const { return Center + Axis[0] * q.x + Axis[1] * q.y + Axis[2] * q.z; }

float Box::Fade(const glm::vec3& p, float fade) const {
    const glm::vec3 in3 = Half - glm::abs(ToLocal(p));
    const float m = std::min(in3.x, std::min(in3.y, in3.z));
    if (fade <= 0.0f) return m >= 0.0f ? 1.0f : 0.0f;
    return std::clamp(m / fade, 0.0f, 1.0f);
}

glm::ivec3 GridDims(const glm::vec3& half, float spacing) {
    const float s = std::max(spacing, 0.05f);
    glm::ivec3 d;
    for (int i = 0; i < 3; ++i) d[i] = std::clamp((int)std::ceil(2.0f * half[i] / s - 1e-4f) + 1, 2, 256);
    return d;
}

glm::vec3 ProbeLocal(const glm::ivec3& ijk, const glm::ivec3& dims, const glm::vec3& half) {
    glm::vec3 q;
    for (int i = 0; i < 3; ++i) q[i] = -half[i] + 2.0f * half[i] * (float)ijk[i] / (float)std::max(dims[i] - 1, 1);
    return q;
}

glm::vec3 VolumeBake::Sample(int layer, const glm::vec3& q, const glm::vec3& n) const {
    if (layer < 0 || layer >= (int)Layers.size()) return glm::vec3(0.0f);
    const auto& P = Layers[layer].Probes;
    if ((int)P.size() != Dims.x * Dims.y * Dims.z) return glm::vec3(0.0f);
    glm::vec3 g;
    for (int i = 0; i < 3; ++i) {
        const float h = std::max(Bounds.Half[i], 1e-6f);
        g[i] = std::clamp((q[i] + h) / (2.0f * h), 0.0f, 1.0f) * (float)(Dims[i] - 1);
    }
    const glm::ivec3 i0 = glm::min(glm::ivec3(glm::floor(g)), Dims - 2);
    const glm::vec3 f = g - glm::vec3(i0);
    ProbeSH acc;
    for (int c = 0; c < 8; ++c) {
        const glm::ivec3 o(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        const float w = (o.x ? f.x : 1.0f - f.x) * (o.y ? f.y : 1.0f - f.y) * (o.z ? f.z : 1.0f - f.z);
        const ProbeSH& s = P[ProbeIndex(i0 + o, Dims)];
        for (int ch = 0; ch < 3; ++ch) acc.c[ch] += s.c[ch] * w;
    }
    return Evaluate(acc, n);
}

const VolumeBake* BakeFile::Find(const std::string& name) const {
    for (const auto& v : Volumes)
        if (v.Name == name) return &v;
    return nullptr;
}

std::vector<std::uint8_t> Serialize(const BakeFile& file) {
    Writer w;
    w.Raw(kMagic, 4);
    w.U32(kVersion);
    w.U32((std::uint32_t)file.Volumes.size());
    for (const auto& v : file.Volumes) {
        w.Str(v.Name);
        w.Vec3(v.Bounds.Center);
        for (int i = 0; i < 3; ++i) w.Vec3(v.Bounds.Axis[i]);
        w.Vec3(v.Bounds.Half);
        w.I32(v.Dims.x); w.I32(v.Dims.y); w.I32(v.Dims.z);
        w.U32((std::uint32_t)v.Layers.size());
        const std::size_t n = (std::size_t)v.Dims.x * v.Dims.y * v.Dims.z;
        for (const auto& l : v.Layers) {
            w.Str(l.Circuit);
            for (std::size_t p = 0; p < n; ++p) {
                const ProbeSH sh = p < l.Probes.size() ? l.Probes[p] : ProbeSH{};
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 4; ++k) {
                        const std::uint16_t h = glm::packHalf1x16(sh.c[c][k]);
                        w.Raw(&h, 2);
                    }
            }
        }
    }
    return std::move(w.out);
}

bool Deserialize(BakeFile& file, const std::uint8_t* data, std::size_t size, std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    Reader r{data, size};
    char magic[4];
    if (!r.Raw(magic, 4) || std::memcmp(magic, kMagic, 4) != 0) return fail("not a .gi file");
    if (r.U32() != kVersion) return fail("unsupported .gi version");
    const std::uint32_t count = r.U32();
    if (!r.ok || count > 4096) return fail("bad volume count");
    BakeFile out;
    out.Volumes.resize(count);
    for (auto& v : out.Volumes) {
        v.Name = r.Str();
        v.Bounds.Center = r.Vec3();
        for (int i = 0; i < 3; ++i) v.Bounds.Axis[i] = r.Vec3();
        v.Bounds.Half = r.Vec3();
        v.Dims = {r.I32(), r.I32(), r.I32()};
        if (!r.ok || glm::any(glm::lessThan(v.Dims, glm::ivec3(2))) || glm::any(glm::greaterThan(v.Dims, glm::ivec3(256))))
            return fail("bad grid size");
        const std::uint32_t layers = r.U32();
        if (!r.ok || layers > 64) return fail("bad layer count");
        const std::size_t n = (std::size_t)v.Dims.x * v.Dims.y * v.Dims.z;
        if (n * 24 > r.left) return fail("truncated");
        v.Layers.resize(layers);
        for (auto& l : v.Layers) {
            l.Circuit = r.Str();
            if (!r.ok || n * 24 > r.left) return fail("truncated");
            l.Probes.resize(n);
            for (auto& sh : l.Probes)
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 4; ++k) {
                        std::uint16_t h = 0;
                        r.Raw(&h, 2);
                        sh.c[c][k] = glm::unpackHalf1x16(h);
                    }
        }
        if (!r.ok) return fail("truncated");
    }
    file = std::move(out);
    return true;
}

bool Save(const BakeFile& file, const std::string& path, std::string* err) {
    const std::vector<std::uint8_t> bytes = Serialize(file);
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp; return false; }
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!f) { if (err) *err = "write failed"; return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { if (err) *err = ec.message(); return false; }
    return true;
}

bool Load(BakeFile& file, const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Deserialize(file, bytes.data(), bytes.size(), err);
}

namespace {
struct CacheEntry {
    std::filesystem::file_time_type Time{};
    std::uintmax_t Size = 0;
    std::shared_ptr<const BakeFile> File;
    std::chrono::steady_clock::time_point Checked{};
};
std::unordered_map<std::string, CacheEntry>& Cache() {
    static std::unordered_map<std::string, CacheEntry> c;
    return c;
}
} // namespace

std::shared_ptr<const BakeFile> Cached(const std::string& absPath) {
    if (absPath.empty()) return nullptr;
    // The disk is asked at most once a second per file (every GI Volume asks every frame, most for the same file).
    {
        auto it = Cache().find(absPath);
        const auto now = std::chrono::steady_clock::now();
        if (it != Cache().end() && now - it->second.Checked < std::chrono::seconds(1)) return it->second.File;
    }
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(absPath, ec);
    if (ec) { auto& miss = Cache()[absPath]; miss.File = nullptr; miss.Checked = std::chrono::steady_clock::now(); return nullptr; }
    const auto size = std::filesystem::file_size(absPath, ec);
    auto& e = Cache()[absPath];
    e.Checked = std::chrono::steady_clock::now();
    if (e.File && e.Time == time && e.Size == size) return e.File;
    e.Time = time;
    e.Size = size;
    auto f = std::make_shared<BakeFile>();
    e.File = Load(*f, absPath) ? std::shared_ptr<const BakeFile>(f) : nullptr;
    return e.File;
}

void ClearCache() { Cache().clear(); }

void FillInvalid(std::vector<ProbeSH>& probes, const std::vector<bool>& valid, const glm::ivec3& dims) {
    const int n = dims.x * dims.y * dims.z;
    if ((int)probes.size() != n || (int)valid.size() != n) return;
    std::vector<char> done(n);
    for (int i = 0; i < n; ++i) done[i] = valid[i] ? 1 : 0;
    const glm::ivec3 nb[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (;;) {
        std::vector<int> ring;
        std::vector<ProbeSH> vals;
        for (int z = 0; z < dims.z; ++z)
            for (int y = 0; y < dims.y; ++y)
                for (int x = 0; x < dims.x; ++x) {
                    const glm::ivec3 p(x, y, z);
                    const int i = ProbeIndex(p, dims);
                    if (done[i]) continue;
                    ProbeSH sum;
                    int cnt = 0;
                    for (const auto& o : nb) {
                        const glm::ivec3 q = p + o;
                        if (glm::any(glm::lessThan(q, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(q, dims))) continue;
                        const int j = ProbeIndex(q, dims);
                        if (!done[j]) continue;
                        for (int c = 0; c < 3; ++c) sum.c[c] += probes[j].c[c];
                        ++cnt;
                    }
                    if (!cnt) continue;
                    for (int c = 0; c < 3; ++c) sum.c[c] /= (float)cnt;
                    ring.push_back(i);
                    vals.push_back(sum);
                }
        if (ring.empty()) break;
        for (std::size_t k = 0; k < ring.size(); ++k) {
            probes[ring[k]] = vals[k];
            done[ring[k]] = 1;
        }
    }
}

std::vector<std::string> SplitCircuits(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        const auto a = cur.find_first_not_of(" \t");
        if (a != std::string::npos) {
            const auto b = cur.find_last_not_of(" \t");
            std::string t = cur.substr(a, b - a + 1);
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == ';' || c == ',') flush();
        else cur += c;
    }
    flush();
    return out;
}

} // namespace GI
