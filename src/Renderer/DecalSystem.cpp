#include "DecalSystem.h"
#include "DecalAsset.h"
#include "DecalTextureArrays.h"
#include "World.h"
#include "Components.h"
#include "Frustum.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace {

// Deterministic integer hash (lowbias32): the same seed gives the same jitter on every machine.
uint32_t Hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// [-1, 1] from a seed and a stream number.
float Signed(int seed, uint32_t stream) {
    return (float)(Hash32((uint32_t)seed * 4u + stream) >> 8) / (float)(1u << 23) - 1.0f;
}

float Unit(float v) { return std::clamp(v, 0.0f, 1.0f); }
uint32_t Half2(float a, float b) { return glm::packHalf2x16(glm::vec2(a, b)); }

long long WriteTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}

std::string Resolve(const std::string& p) {
    if (p.empty() || std::filesystem::u8path(p).is_absolute()) return p;
    return ProjectPaths::Resolve(p);
}

// (SortOrder, entity, seed) -> one key, ascending = drawn first (underneath).
uint64_t PackSortKey(int sortOrder, uint32_t entity, int seed) {
    const uint64_t order = (uint64_t)(uint16_t)(std::clamp(sortOrder, -32768, 32767) + 32768);
    return order << 48 | (uint64_t)entity << 16 | (uint64_t)(uint16_t)seed;
}

// The asset's texture set + this instance's values, packed. `fade` is the distance fade.
void PackDecal(const DecalAsset& a, const DecalComponent& dc, const DecalTextureArrays::Slot& slot,
               const DecalGrading& grading, float fade, int frame, Decals::GpuDecal& out) {
    using namespace Decals;
    // Atlas frame inside the slot; its mips stop where a frame is still 4 texels across.
    const int cols = std::max(1, a.AtlasCols), rows = std::max(1, a.AtlasRows);
    const int f = ((frame % (cols * rows)) + cols * rows) % (cols * rows);
    const float fw = slot.Rect.z / (float)cols, fh = slot.Rect.w / (float)rows;
    out.Rect = glm::vec4(slot.Rect.x + (float)(f % cols) * fw, slot.Rect.y + (float)(f / cols) * fh, fw, fh);
    int maxLod = slot.MaxLod;
    const int frameTexels = slot.Alloc.Size / std::max(cols, rows);
    while (maxLod > 0 && (frameTexels >> maxLod) < 4) --maxLod;

    const float op = Unit(dc.Opacity) * fade;
    const float albedoOp = Unit(a.AlbedoOp * dc.AlbedoOp) * op;
    const float normalOp = (slot.TexFlags & FLAG_HAS_NORMAL_TEX) ? Unit(a.NormalOp * dc.NormalOp) * op : 0.0f;
    float roughOp = Unit(a.RoughnessOp * dc.RoughnessOp) * op;
    float metalOp = Unit(a.MetalOp * dc.MetalOp) * op;
    const float aoOp = Unit(a.AOOp * dc.AOOp) * op;
    const float emissiveOp = Unit(a.EmissiveOp * dc.EmissiveOp) * op;
    // Paint brings its own material response wherever it covers.
    if (a.Paint) {
        roughOp = std::max(roughOp, albedoOp);
        metalOp = std::max(metalOp, albedoOp);
    }
    uint32_t ch = 0;
    if (albedoOp > 0.0f) ch |= CH_ALBEDO;
    if (normalOp > 0.0f) ch |= CH_NORMAL;
    if (roughOp > 0.0f) ch |= CH_ROUGHNESS;
    if (metalOp > 0.0f) ch |= CH_METAL;
    if (aoOp > 0.0f) ch |= CH_AO;
    if (emissiveOp > 0.0f && a.Emissive != glm::vec3(0.0f)) ch |= CH_EMISSIVE;
    if (a.HeightBlend > 0.0f) ch |= CH_HEIGHT_BLEND;
    if (a.Paint) ch |= CH_PAINT;

    uint32_t flags = slot.TexFlags;
    if (dc.AffectStatic) flags |= FLAG_STATIC_RECEIVERS;
    if (dc.AffectDynamic) flags |= FLAG_DYNAMIC_RECEIVERS;
    if (a.UVPaint) flags |= FLAG_UV_PAINT;
    if (a.LumaMask) flags |= FLAG_LUMA_MASK;
    if (a.InvertMask) flags |= FLAG_INVERT_MASK;
    flags |= (uint32_t)std::lround(Unit(a.HeightBlend) * 255.0f) << HEIGHT_BLEND_SHIFT;

    out.Ids = glm::uvec4((uint32_t)slot.Alloc.Layer | (uint32_t)maxLod << 8 | ch << 16, (uint32_t)dc.LayerMask, flags, 0u);
    const glm::vec3 tint = a.BaseColor * dc.Tint;
    out.H0 = glm::uvec4(Half2(tint.r, tint.g), Half2(tint.b, albedoOp), Half2(normalOp, roughOp), Half2(metalOp, aoOp));
    out.H1 = glm::uvec4(Half2(a.Emissive.r, a.Emissive.g), Half2(a.Emissive.b, emissiveOp),
                        Half2(a.AngleFadeStart, a.AngleFadeEnd), Half2(a.EdgeFade, a.HeightContrast));
    // The scene's Decal Grading on top of the asset's and the instance's weathering: Age fades and
    // bleaches, Wear peels, buffs and chips.
    const DecalGrading g = a.SceneGrading ? grading : DecalGrading{};
    const float fadeW = Unit(a.Fade + dc.Fade + g.Age * 0.5f);
    const float bleach = Unit(a.Bleach + dc.Bleach + g.Age * 0.6f);
    const float peel = Unit(a.Peel + dc.Peel + g.Wear * 0.5f);
    const float buff = Unit(a.Buff + dc.Buff + g.Wear * 0.3f);
    const float wear = Unit(g.Wear);
    out.H2 = glm::uvec4(Half2(a.Paint ? a.PaintRoughness : a.Roughness, a.Paint ? a.PaintMetal : a.Metal),
                        Half2(a.Paint ? a.Relief : 0.0f, wear), Half2(fadeW, bleach), Half2(peel, buff));
}

} // namespace

namespace {
DecalSystem* s_Current = nullptr;
}

DecalSystem::DecalSystem() : m_Textures(std::make_unique<DecalTextureArrays>()) { s_Current = this; }

DecalSystem* DecalSystem::Current() { return s_Current; }

DecalSystem::~DecalSystem() {
    if (s_Current == this) s_Current = nullptr;
    if (m_DecalBuf) glDeleteBuffers(1, &m_DecalBuf);
    if (m_BoundsBuf) glDeleteBuffers(1, &m_BoundsBuf);
}

std::shared_ptr<DecalAsset> DecalSystem::GetAsset(const std::string& projectRelPath) {
    // Accepts project-relative or absolute (DecalComponent::AssetPath is absolute in memory, often the
    // lower-cased path key); the cache key is the absolute path, lower case with forward slashes.
    const std::string abs = Resolve(projectRelPath);
    std::string key = abs;
    for (char& c : key) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    if (auto it = m_Assets.find(key); it != m_Assets.end()) return it->second;
    auto asset = DecalAsset::Load(abs);
    m_Assets[key] = asset;
    m_AssetTimes[key] = WriteTime(abs);
    return asset;
}

namespace {
DecalTextureArrays::Source SourceFor(const DecalAsset& a) {
    DecalTextureArrays::Source src;
    src.Albedo = Resolve(a.AlbedoPath);
    src.Normal = Resolve(a.NormalPath);
    src.Surface = Resolve(a.SurfacePath);
    src.Aux = Resolve(a.AuxPath);
    src.Tier = a.Tier;
    return src;
}
} // namespace

void DecalSystem::Refresh(const std::string& path) {
    const std::shared_ptr<DecalAsset> old = GetAsset(path);
    if (old && !old->Missing) m_Textures->Drop(SourceFor(*old));
    const std::string abs = Resolve(path);
    std::string key = abs;
    for (char& c : key) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c); // GetAsset's key
    m_Assets[key] = DecalAsset::Load(abs);
    m_AssetTimes[key] = WriteTime(abs);
    if (!m_Assets[key]->Missing) m_Textures->Drop(SourceFor(*m_Assets[key]));
}

bool DecalSystem::Resident(const std::string& path) {
    const std::shared_ptr<DecalAsset> a = GetAsset(path);
    return a && !a->Missing && m_Textures->Resident(SourceFor(*a));
}

void DecalSystem::PushRuntime(const DecalInstance& d) { m_Runtime.push_back(d); }

Decals::GpuDecalBounds DecalSystem::BoundsFor(const glm::mat4& boxToWorld) {
    Decals::GpuDecalBounds b;
    b.Centre = glm::vec4(glm::vec3(boxToWorld[3]), 0.0f);
    b.AxisX = glm::vec4(glm::vec3(boxToWorld[0]) * 0.5f, 0.0f);
    b.AxisY = glm::vec4(glm::vec3(boxToWorld[1]) * 0.5f, 0.0f);
    b.AxisZ = glm::vec4(glm::vec3(boxToWorld[2]) * 0.5f, 0.0f);
    return b;
}

glm::mat4 DecalSystem::VariationFor(int seed, float rotJitterDeg, float scaleJitter, bool flip) {
    const float angle = glm::radians(Signed(seed, 1) * rotJitterDeg);
    const float s = std::max(1.0f + Signed(seed, 2) * scaleJitter, 0.01f);
    const bool flipX = flip && (Hash32((uint32_t)seed * 4u + 3u) & 1u) != 0;
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, glm::vec3(flipX ? -s : s, s, 1.0f));
}

int DecalSystem::FrameFor(int seed, int frames) {
    if (frames <= 1) return 0;
    return (int)(Hash32((uint32_t)seed * 4u + 0u) % (uint32_t)frames);
}

void DecalSystem::SortDeterministic(std::vector<DecalInstance>& v) {
    std::stable_sort(v.begin(), v.end(), [](const DecalInstance& a, const DecalInstance& b) { return a.SortKey < b.SortKey; });
}

bool DecalSystem::OverlapsAabb(const Decals::GpuDecalBounds& b, const glm::vec3& mn, const glm::vec3& mx) {
    const glm::vec3 bc = 0.5f * (mn + mx), bh = 0.5f * (mx - mn);
    const glm::vec3 axes[3] = {glm::vec3(b.AxisX), glm::vec3(b.AxisY), glm::vec3(b.AxisZ)};
    const glm::vec3 d = glm::vec3(b.Centre) - bc;
    const glm::vec3 ext = glm::abs(axes[0]) + glm::abs(axes[1]) + glm::abs(axes[2]);
    for (int i = 0; i < 3; ++i)
        if (std::abs(d[i]) > ext[i] + bh[i]) return false;
    for (const glm::vec3& a : axes) {
        const float len = glm::length(a);
        if (len < 1e-6f) continue;
        const glm::vec3 n = a / len;
        if (std::abs(glm::dot(d, n)) > len + glm::dot(bh, glm::abs(n))) return false;
    }
    return true;
}

void DecalSystem::BinCluster(const std::vector<Decals::GpuDecalBounds>& bounds, const glm::vec3& mn, const glm::vec3& mx,
                             int cap, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < (uint32_t)bounds.size(); ++i)
        if (OverlapsAabb(bounds[i], mn, mx)) out.push_back(i);
    if ((int)out.size() > cap) out.erase(out.begin(), out.end() - cap);
}

int DecalSystem::Prepare(World& world, const glm::mat4& viewProj, const glm::vec3& cameraPos, float maxDistance,
                         const DecalGrading& grading) {
    m_Grading = grading;
    m_Textures->SetBudgetMB(ProjectSettings::Quality().DecalTextureBudgetMB);
    // Hot reload: an edited .decal is re-read (and its textures re-decoded) within about a second.
    if (++m_ReloadTick >= 60) {
        m_ReloadTick = 0;
        bool changed = false;
        for (auto& [key, time] : m_AssetTimes) {
            const std::string path = m_Assets[key] ? m_Assets[key]->Path : key; // the path as first loaded
            const long long now = WriteTime(path);
            if (now == time) continue;
            time = now;
            m_Assets[key] = DecalAsset::Load(path);
            changed = true;
        }
        if (changed) m_Textures->Clear();
    }
    m_Textures->BeginFrame();

    const Frustum frustum = Frustum::FromViewProj(viewProj);
    const float fadeStart = maxDistance * 0.8f;
    m_Frame.clear();
    std::vector<float> dist; // parallel to m_Frame, for the MAX_VISIBLE cut
    auto consider = [&](const DecalInstance& d) -> float {
        const Decals::GpuDecalBounds b = BoundsFor(d.BoxToWorld);
        const glm::vec3 c(b.Centre);
        const glm::vec3 ext = glm::abs(glm::vec3(b.AxisX)) + glm::abs(glm::vec3(b.AxisY)) + glm::abs(glm::vec3(b.AxisZ));
        if (!frustum.Intersects(AABB{c - ext, c + ext})) return -1.0f;
        return std::max(0.0f, glm::length(c - cameraPos) - glm::length(ext));
    };

    auto& reg = world.Registry;
    for (auto [entity, dc] : reg.view<const DecalComponent>().each()) {
        if (dc.AssetPath.empty() || reg.all_of<InactiveTag>(entity)) continue;
        if (dc.Opacity <= 0.0f || (!dc.AffectStatic && !dc.AffectDynamic)) continue;
        const std::shared_ptr<DecalAsset> asset = GetAsset(dc.AssetPath);
        if (!asset || asset->Missing || asset->AlbedoPath.empty()) continue;
        DecalInstance inst;
        inst.BoxToWorld = world.GetCachedWorldTransform(entity) *
                          VariationFor(dc.Seed, dc.RotationJitter, dc.ScaleJitter, dc.RandomFlip) *
                          glm::scale(glm::mat4(1.0f), glm::max(dc.Size, glm::vec3(0.001f)));
        const float d = consider(inst);
        if (d < 0.0f || d > maxDistance) continue;
        // Only resident textures draw; the rest decode in the background and show up a frame or so later.
        DecalTextureArrays::Source src;
        src.Albedo = Resolve(asset->AlbedoPath);
        src.Normal = Resolve(asset->NormalPath);
        src.Surface = Resolve(asset->SurfacePath);
        src.Aux = Resolve(asset->AuxPath);
        src.Tier = asset->Tier;
        const DecalTextureArrays::Slot* slot = m_Textures->Request(src);
        if (!slot) continue;
        const float fade = d > fadeStart ? 1.0f - (d - fadeStart) / std::max(maxDistance - fadeStart, 1e-3f) : 1.0f;
        const int frame = dc.Frame >= 0 ? dc.Frame : FrameFor(dc.Seed, asset->AtlasCols * asset->AtlasRows);
        PackDecal(*asset, dc, *slot, m_Grading, fade, frame, inst.Gpu);
        inst.SortKey = PackSortKey(dc.SortOrder, (uint32_t)entt::to_integral(entity), dc.Seed);
        m_Frame.push_back(inst);
        dist.push_back(d);
    }
    for (const DecalInstance& r : m_Runtime) {
        const float d = consider(r);
        if (d < 0.0f || d > maxDistance) continue;
        m_Frame.push_back(r);
        dist.push_back(d);
    }
    m_Runtime.clear();

    // Over budget: the nearest MAX_VISIBLE stay.
    if ((int)m_Frame.size() > Decals::MAX_VISIBLE) {
        std::vector<uint32_t> order(m_Frame.size());
        for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
        std::nth_element(order.begin(), order.begin() + Decals::MAX_VISIBLE, order.end(),
                         [&](uint32_t a, uint32_t b) { return dist[a] != dist[b] ? dist[a] < dist[b] : a < b; });
        order.resize(Decals::MAX_VISIBLE);
        std::sort(order.begin(), order.end()); // back to gather order, so the stable sort's ties hold
        std::vector<DecalInstance> kept;
        kept.reserve(order.size());
        for (uint32_t i : order) kept.push_back(m_Frame[i]);
        m_Frame.swap(kept);
    }
    SortDeterministic(m_Frame);

    m_UploadDecals.resize(m_Frame.size());
    m_UploadBounds.resize(m_Frame.size());
    for (size_t i = 0; i < m_Frame.size(); ++i) {
        Decals::GpuDecal g = m_Frame[i].Gpu;
        const glm::mat4 inv = glm::inverse(m_Frame[i].BoxToWorld);
        for (int r = 0; r < 3; ++r) g.WorldToBox[r] = glm::vec4(inv[0][r], inv[1][r], inv[2][r], inv[3][r]);
        g.Ids.w = (uint32_t)i;
        m_UploadDecals[i] = g;
        m_UploadBounds[i] = BoundsFor(m_Frame[i].BoxToWorld);
    }
    m_Visible = (int)m_Frame.size();
    if (!m_DecalBuf) {
        glCreateBuffers(1, &m_DecalBuf);
        glNamedBufferStorage(m_DecalBuf, (GLsizeiptr)Decals::MAX_VISIBLE * (GLsizeiptr)sizeof(Decals::GpuDecal), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);
        glCreateBuffers(1, &m_BoundsBuf);
        glNamedBufferStorage(m_BoundsBuf, (GLsizeiptr)Decals::MAX_VISIBLE * (GLsizeiptr)sizeof(Decals::GpuDecalBounds),
                             nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    if (m_Visible > 0) {
        glNamedBufferSubData(m_DecalBuf, 0, (GLsizeiptr)(m_Visible * sizeof(Decals::GpuDecal)), m_UploadDecals.data());
        glNamedBufferSubData(m_BoundsBuf, 0, (GLsizeiptr)(m_Visible * sizeof(Decals::GpuDecalBounds)), m_UploadBounds.data());
    }
    return m_Visible;
}

void DecalSystem::BindForCull() const {
    if (!m_DecalBuf) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_BOUNDS, m_BoundsBuf);
}

void DecalSystem::BindForShading() const {
    if (m_DecalBuf) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_DECALS, m_DecalBuf);
    m_Textures->Bind();
}
