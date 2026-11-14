#include "GIVolumes.h"

#include "Shader.h"
#include "DefaultTextures.h"
#include "GpuMemTrack.h"
#include "gl.h"
#include "Log.h"
#include "Core/Paths.h"
#include "../Game/World.h"
#include "../Game/Components.h"
#include "../Game/LightSwitch.h"

#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace {

struct GpuVolume {
    glm::vec4 BoxX, BoxY, BoxZ; // world -> box rows: xyz unit axis, w = -dot(axis, centre)
    glm::vec4 HalfFade;         // xyz half extents, w fade metres
    glm::vec4 Dims;             // xyz probe counts, w layer count
    glm::vec4 Params;           // x intensity, y replaces the sky's ambient, z normal bias (m)
    glm::vec4 Brick[GIVolumes::kMaxLayers]; // xyz brick origin (texels), w weight
};
struct GpuBlock {
    glm::ivec4 Count;   // x volumes
    glm::vec4 AtlasInv; // xyz 1 / atlas size
    GpuVolume V[GIVolumes::kMaxVolumes];
};
static_assert(sizeof(GpuVolume) == 160, "std140 layout");
static_assert(sizeof(GpuBlock) == 32 + 160 * GIVolumes::kMaxVolumes, "std140 layout");

std::string NameOf(const World& world, entt::entity e) {
    const auto* n = world.Registry.try_get<NameComponent>(e);
    return n ? n->Name : std::string();
}

} // namespace

GIVolumes::GIVolumes() = default;
GIVolumes::~GIVolumes() { Release(); }

void GIVolumes::Release() {
    if (m_Atlas) {
        GpuMemTrack::Set(GpuMemTrack::Key("GI atlas", this), 0);
        glDeleteTextures(1, &m_Atlas);
    }
    if (m_Ubo) glDeleteBuffers(1, &m_Ubo);
    m_Atlas = m_Ubo = 0;
    m_AtlasKey.clear();
    m_LastUbo.clear();
}

std::unordered_map<std::string, float> GIVolumes::CircuitWeights(const World& world) {
    std::unordered_map<std::string, float> w;
    std::unordered_set<std::string> hasLight;
    auto lights = world.Registry.view<const LightCircuitComponent, const LightComponent>();
    for (auto e : lights) {
        const std::string& c = lights.get<const LightCircuitComponent>(e).Circuit;
        hasLight.insert(c);
        const bool on = !world.Registry.any_of<DeactivatedTag, InactiveTag>(e);
        float& v = w[c];
        v = std::max(v, on ? 1.0f : 0.0f);
    }
    // Circuits with no Light: their switch's state, once started (LightSwitch::IsOn); before Play, on.
    auto switches = world.Registry.view<const LightSwitchComponent>();
    for (auto e : switches) {
        const auto& s = switches.get<const LightSwitchComponent>(e);
        if (hasLight.count(s.Circuit)) continue;
        if (!w.count(s.Circuit)) w[s.Circuit] = s.Started ? (s.On ? 1.0f : 0.0f) : 1.0f;
        else if (s.Started) w[s.Circuit] = s.On ? 1.0f : 0.0f;
    }
    return w;
}

std::vector<glm::ivec3> GIVolumes::Pack(const std::vector<glm::ivec3>& sizes, int maxWidth, glm::ivec3& atlas) {
    std::vector<glm::ivec3> origin(sizes.size(), glm::ivec3(0));
    atlas = glm::ivec3(0);
    int x = 0, z = 0, rowDepth = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const glm::ivec3 s = sizes[i];
        if (x > 0 && x + s.x > maxWidth) { // next row
            z += rowDepth;
            x = 0;
            rowDepth = 0;
        }
        origin[i] = glm::ivec3(x, 0, z);
        x += s.x;
        rowDepth = std::max(rowDepth, s.z);
        atlas.x = std::max(atlas.x, x);
        atlas.y = std::max(atlas.y, s.y);
    }
    atlas.z = z + rowDepth;
    return origin;
}

void GIVolumes::Update(const World& world) {
    Gather(world);

    // The atlas: rebuilt when the baked data or the grids change (keyed by file identity, name and dims).
    std::string key;
    for (const auto& v : m_Volumes) {
        key += v.Name;
        key += '|';
        key += std::to_string((std::uintptr_t)v.Bake) + ':' + std::to_string(v.Dims.x) + 'x' + std::to_string(v.Dims.y) +
               'x' + std::to_string(v.Dims.z) + '/' + std::to_string(v.Layers.size()) + ';';
    }
    if (key != m_AtlasKey) {
        m_AtlasKey = key;
        Rebuild();
    }
    WriteUbo();
}

void GIVolumes::Gather(const World& world) {
    std::vector<Volume> vols;
    auto view = world.Registry.view<const TransformComponent, const GIVolumeComponent>();
    for (auto [e, tc, gv] : view.each()) {
        if (world.Registry.any_of<InactiveTag, DeactivatedTag>(e)) continue;
        if ((int)vols.size() >= kMaxVolumes) break;
        Volume v;
        v.Name = NameOf(world, e);
        v.Bounds = GI::Box::FromTransform(world.GetCachedWorldTransform(e), gv.Size);
        v.Spacing = gv.Spacing;
        v.Dims = GI::GridDims(v.Bounds.Half, gv.Spacing);
        v.Fade = std::max(gv.Fade, 0.0f);
        v.Intensity = std::max(gv.Intensity, 0.0f);
        v.File = m_Override ? m_Override
                            : (gv.BakeFile.empty() ? nullptr : GI::Cached(ProjectPaths::Resolve(gv.BakeFile)));
        v.Bake = v.File ? v.File->Find(v.Name) : nullptr;
        if (!v.Bake || v.Bake->Dims != v.Dims) continue; // not baked, or baked for another grid: re-bake
        for (int l = 0; l < (int)v.Bake->Layers.size() && (int)v.Layers.size() < kMaxLayers; ++l) {
            v.Layers.push_back(l);
            if (v.Bake->Layers[l].Circuit.empty()) v.ReplacesSky = true;
        }
        vols.push_back(std::move(v));
    }

    // Layer weights.
    const auto weights = CircuitWeights(world);
    for (auto& v : vols) {
        v.Weights.clear();
        for (int l : v.Layers) {
            const std::string& c = v.Bake->Layers[l].Circuit;
            float w;
            if (m_SoloOn) w = c == m_Solo ? 1.0f : 0.0f;
            else if (c.empty()) w = 1.0f;
            else { auto it = weights.find(c); w = it != weights.end() ? it->second : 1.0f; }
            v.Weights.push_back(w);
        }
    }
    m_Volumes = std::move(vols);
}

void GIVolumes::Rebuild() {
    if (m_Atlas) {
        GpuMemTrack::Set(GpuMemTrack::Key("GI atlas", this), 0);
        glDeleteTextures(1, &m_Atlas);
        m_Atlas = 0;
    }
    m_BrickOrigin.clear();
    m_AtlasSize = glm::ivec3(0);
    if (m_Volumes.empty()) return;

    std::vector<glm::ivec3> sizes;
    for (const auto& v : m_Volumes)
        for (std::size_t l = 0; l < v.Layers.size(); ++l) sizes.push_back({v.Dims.x, v.Dims.y, 3 * v.Dims.z});
    const GLint max3d = 2048; // GL_MAX_3D_TEXTURE_SIZE's guaranteed minimum on GL 4.3+
    m_BrickOrigin = Pack(sizes, std::min(512, (int)max3d), m_AtlasSize);
    if (m_AtlasSize.z > max3d || m_AtlasSize.y > max3d) {
        Log::Warn("[GI] atlas " + std::to_string(m_AtlasSize.x) + "x" + std::to_string(m_AtlasSize.y) + "x" +
                  std::to_string(m_AtlasSize.z) + " is past the GL limit " + std::to_string(max3d) + ": GI off");
        m_Volumes.clear();
        m_BrickOrigin.clear();
        return;
    }

    std::vector<std::uint16_t> texels((std::size_t)m_AtlasSize.x * m_AtlasSize.y * m_AtlasSize.z * 4, 0);
    auto put = [&](const glm::ivec3& t, const glm::vec4& v) {
        const std::size_t i = (((std::size_t)t.z * m_AtlasSize.y + t.y) * m_AtlasSize.x + t.x) * 4;
        for (int k = 0; k < 4; ++k) texels[i + k] = glm::packHalf1x16(v[k]);
    };
    int brick = 0;
    for (const auto& v : m_Volumes)
        for (int l : v.Layers) {
            const glm::ivec3 o = m_BrickOrigin[brick++];
            const auto& probes = v.Bake->Layers[l].Probes;
            for (int z = 0; z < v.Dims.z; ++z)
                for (int y = 0; y < v.Dims.y; ++y)
                    for (int x = 0; x < v.Dims.x; ++x) {
                        const GI::ProbeSH& sh = probes[GI::ProbeIndex({x, y, z}, v.Dims)];
                        for (int c = 0; c < 3; ++c) put(o + glm::ivec3(x, y, z + c * v.Dims.z), sh.c[c]);
                    }
        }

    glCreateTextures(GL_TEXTURE_3D, 1, &m_Atlas);
    glTextureStorage3D(m_Atlas, 1, GL_RGBA16F, m_AtlasSize.x, m_AtlasSize.y, m_AtlasSize.z);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage3D(m_Atlas, 0, 0, 0, 0, m_AtlasSize.x, m_AtlasSize.y, m_AtlasSize.z, GL_RGBA, GL_HALF_FLOAT,
                        texels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTextureParameteri(m_Atlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_Atlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_Atlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Atlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Atlas, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    GpuMemTrack::Set(GpuMemTrack::Key("GI atlas", this), texels.size() * sizeof(std::uint16_t));
    Log::Info("[GI] atlas " + std::to_string(m_AtlasSize.x) + "x" + std::to_string(m_AtlasSize.y) + "x" +
              std::to_string(m_AtlasSize.z) + ", " + std::to_string(m_Volumes.size()) + " volumes, " +
              std::to_string(brick) + " bricks");
}

void GIVolumes::WriteUbo() {
    GpuBlock b{};
    b.Count = glm::ivec4((m_Enabled && m_Atlas) ? (int)m_Volumes.size() : 0, 0, 0, 0);
    b.AtlasInv = glm::vec4(1.0f / glm::vec3(glm::max(m_AtlasSize, glm::ivec3(1))), 0.0f);
    int brick = 0;
    for (std::size_t i = 0; i < m_Volumes.size(); ++i) {
        const Volume& v = m_Volumes[i];
        GpuVolume& g = b.V[i];
        glm::vec4* rows[3] = {&g.BoxX, &g.BoxY, &g.BoxZ};
        for (int a = 0; a < 3; ++a) *rows[a] = glm::vec4(v.Bounds.Axis[a], -glm::dot(v.Bounds.Axis[a], v.Bounds.Center));
        g.HalfFade = glm::vec4(v.Bounds.Half, v.Fade);
        g.Dims = glm::vec4(glm::vec3(v.Dims), (float)v.Layers.size());
        g.Params = glm::vec4(v.Intensity, v.ReplacesSky ? 1.0f : 0.0f, 0.25f * v.Spacing, 0.0f);
        for (std::size_t l = 0; l < v.Layers.size(); ++l) {
            const glm::ivec3 o = brick < (int)m_BrickOrigin.size() ? m_BrickOrigin[brick] : glm::ivec3(0);
            g.Brick[l] = glm::vec4(glm::vec3(o), v.Weights[l]);
            ++brick;
        }
    }
    if (!m_Ubo) {
        glCreateBuffers(1, &m_Ubo);
        glNamedBufferData(m_Ubo, sizeof(GpuBlock), nullptr, GL_DYNAMIC_DRAW);
    }
    // Only what is used, and only when it changed (a switch flip, a moved volume).
    const std::size_t bytes = 32 + 160 * std::max<std::size_t>(m_Volumes.size(), 1);
    const float* f = reinterpret_cast<const float*>(&b);
    if (m_LastUbo.size() == bytes / 4 && std::memcmp(m_LastUbo.data(), f, bytes) == 0) return;
    m_LastUbo.assign(f, f + bytes / 4);
    glNamedBufferSubData(m_Ubo, 0, (GLsizeiptr)bytes, &b);
}

void GIVolumes::Bind(Shader& program) const {
    const bool on = m_Enabled && m_Atlas && !m_Volumes.empty();
    program.SetInt("uGIEnabled", on ? 1 : 0);
    glBindTextureUnit(kUnit, m_Atlas ? m_Atlas : DefaultTextures::Volume());
    if (m_Ubo) glBindBufferBase(GL_UNIFORM_BUFFER, kUboBinding, m_Ubo);
}

glm::vec3 GIVolumes::SampleCPU(const glm::vec3& p, const glm::vec3& n, float* fadeOut) const {
    glm::vec3 sum(0.0f);
    float wSum = 0.0f;
    if (m_Enabled)
        for (const auto& v : m_Volumes) {
            const glm::vec3 pb = p + n * (0.25f * v.Spacing);
            const float f = v.Bounds.Fade(pb, v.Fade);
            if (f <= 0.0f) continue;
            const glm::vec3 q = v.Bounds.ToLocal(pb);
            glm::vec3 e(0.0f); // the SH is in world axes: evaluated with the world normal
            for (std::size_t l = 0; l < v.Layers.size(); ++l)
                if (v.Weights[l] > 0.0f) e += v.Bake->Sample(v.Layers[l], q, n) * v.Weights[l];
            sum += e * v.Intensity * f;
            wSum += f;
        }
    if (fadeOut) *fadeOut = std::min(wSum, 1.0f);
    return wSum > 1.0f ? sum / wSum : sum;
}
