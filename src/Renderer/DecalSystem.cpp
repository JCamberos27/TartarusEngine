#include "DecalSystem.h"
#include "DecalAsset.h"
#include "DecalTextureArrays.h"
#include "World.h"
#include "Components.h"
#include "Frustum.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"
#include "Log.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

// An absolute path as a cache key: forward slashes, lower case (Windows paths are case-insensitive).
std::string NormalPath(std::string p) {
    for (char& c : p) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    return p;
}

DecalSystem* s_Active = nullptr; // MaterialSubstrate()'s system: the one main.cpp owns

// A placed decal's Custom substrate (DecalComponent::Sub*).
SubstrateDesc CustomSubstrate(const DecalComponent& dc) {
    SubstrateDesc s;
    s.Albedo = dc.SubAlbedo;
    s.Normal = dc.SubNormal;
    s.Roughness = dc.SubRoughness;
    s.AO = dc.SubAO;
    s.Tiling = std::max(dc.SubTiling, 0.01f);
    s.Tint = dc.SubTint;
    s.Strength = Unit(dc.SubStrength);
    s.RoughRange = glm::vec2(Unit(dc.SubRoughMin), Unit(dc.SubRoughMax));
    s.Mirror = dc.SubMirror;
    s.Crop = glm::clamp(glm::vec4(dc.SubCropX0, dc.SubCropY0, dc.SubCropX1, dc.SubCropY1), glm::vec4(0.0f), glm::vec4(1.0f));
    return s;
}

// (SortOrder, entity, seed) -> one key, ascending = drawn first (underneath).
uint64_t PackSortKey(int sortOrder, uint32_t entity, int seed) {
    const uint64_t order = (uint64_t)(uint16_t)(std::clamp(sortOrder, -32768, 32767) + 32768);
    return order << 48 | (uint64_t)entity << 16 | (uint64_t)(uint16_t)seed;
}

// The asset's texture set + this instance's values, packed. `fade` is the distance fade.
void PackDecal(const DecalAsset& a, const DecalComponent& dc, const DecalTextureArrays::Slot& slot,
               const DecalGrading& g, float fade, int frame, Decals::GpuDecal& out) {
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
    const float fadeW = Unit(a.Fade + dc.Fade + g.Age * 0.5f);
    const float bleach = Unit(a.Bleach + dc.Bleach + g.Age * 0.6f);
    const float peel = Unit(a.Peel + dc.Peel + g.Wear * 0.5f);
    const float buff = Unit(a.Buff + dc.Buff + g.Wear * 0.3f);
    const float wear = Unit(g.Wear);
    out.H2 = glm::uvec4(Half2(a.Paint ? a.PaintRoughness : a.Roughness, a.Paint ? a.PaintMetal : a.Metal),
                        Half2(a.Paint ? a.Relief : 0.0f, wear), Half2(fadeW, bleach), Half2(peel, buff));
}

} // namespace

DecalSystem::DecalSystem() : m_Textures(std::make_unique<DecalTextureArrays>()) { s_Active = this; }

DecalSystem::~DecalSystem() {
    if (s_Active == this) s_Active = nullptr;
    if (m_DecalBuf) glDeleteBuffers(1, &m_DecalBuf);
    if (m_BoundsBuf) glDeleteBuffers(1, &m_BoundsBuf);
    if (m_SubBuf) glDeleteBuffers(1, &m_SubBuf);
}

uint32_t DecalSystem::SubstrateIndex(const SubstrateDesc& desc) {
    // TARTARUS_NO_SUBSTRATE=1: every substrate off (before / after comparisons).
    static const bool kOff = [] {
#pragma warning(suppress : 4996)
        const char* v = std::getenv("TARTARUS_NO_SUBSTRATE");
        return v && *v && *v != '0';
    }();
    if (kOff || !desc.Enabled()) return 0;
    // Looked up by the key as given, then by the resolved files: a material's absolute paths and a
    // .decal's project-relative ones name the same maps, and share one entry (and one slot).
    const std::string key = desc.Key();
    uint32_t idx;
    if (auto it = m_SubIndex.find(key); it != m_SubIndex.end()) {
        idx = it->second;
    } else {
        SubstrateDesc norm = desc;
        for (std::string* p : {&norm.Albedo, &norm.Normal, &norm.Roughness, &norm.AO}) *p = NormalPath(Resolve(*p));
        const std::string normKey = norm.Key();
        if (auto nt = m_SubIndex.find(normKey); nt != m_SubIndex.end()) {
            m_SubIndex.emplace(key, nt->second);
            return SubstrateIndex(desc);
        }
        if ((int)m_Subs.size() >= Decals::MAX_SUBSTRATES) return 0;
        SubstrateEntry e;
        e.Key = normKey;
        e.Src.Albedo = norm.Albedo;
        e.Src.Normal = norm.Normal;
        e.Src.Rough = norm.Roughness;
        e.Src.AO = norm.AO;
        e.Src.Detail = norm.Detail;
        e.Src.Tier = 2048;
        e.Gpu.Crop = desc.Crop;
        e.Gpu.TintStrength = glm::vec4(desc.Tint, Unit(desc.Strength));
        e.Gpu.Params = glm::vec4(std::max(desc.Tiling, 0.01f), desc.RoughRange.x, desc.RoughRange.y, desc.Mirror ? 1.0f : 0.0f);
        idx = (uint32_t)m_Subs.size();
        m_Subs.push_back(std::move(e));
        m_SubIndex.emplace(key, idx);
        m_SubIndex.emplace(normKey, idx);
    }
    m_Subs[idx].Used = m_FrameNo;
    RefreshSubstrate(idx);
    return m_Subs[idx].Ready ? idx + 1 : 0;
}

void DecalSystem::RefreshSubstrate(uint32_t idx) {
    SubstrateEntry& e = m_Subs[idx];
    if (e.Frame != m_FrameNo) {
        // Once per Prepare(): pin the slot (it may have moved since an eviction) and refresh the record.
        e.Frame = m_FrameNo;
        const DecalTextureArrays::Slot* slot = m_Textures->Request(e.Src);
        if (slot && !e.Logged) {
            e.Logged = true;
            Log::Info("Decals: substrate " + std::to_string(idx + 1) + " ready (" + ProjectPaths::Relativize(e.Src.Albedo) +
                      ", layer " + std::to_string(slot->Alloc.Layer) + ", " + std::to_string(slot->Alloc.Size) + " px, map flags " +
                      std::to_string(slot->TexFlags) + ", tiling " + std::to_string(e.Gpu.Params.x) + ")");
        }
        e.Ready = slot != nullptr;
        if (slot) {
            const glm::uvec4 ids((uint32_t)slot->Alloc.Layer, (uint32_t)slot->MaxLod, slot->TexFlags, 0u);
            if (e.Gpu.Rect != slot->Rect || e.Gpu.Ids != ids) {
                e.Gpu.Rect = slot->Rect;
                e.Gpu.Ids = ids;
                m_SubDirty = true;
                if (!m_InPrepare) FlushSubstrates(); // a material bind mid-frame: its draw reads it next
            }
        }
    }
}

bool DecalSystem::Streaming() const {
    for (const SubstrateEntry& e : m_Subs)
        if (e.Used == m_FrameNo && !e.Ready) return true;
    // Over budget, the decals in view swap in and out for as long as it holds: nothing to wait for.
    return m_Textures->Busy() && !m_Textures->Thrashing();
}

int DecalSystem::MaterialSubstrate(const SubstrateDesc& desc) {
    return s_Active && desc.Enabled() ? (int)s_Active->SubstrateIndex(desc) : 0;
}

void DecalSystem::FlushSubstrates() {
    if (!m_SubBuf) {
        glCreateBuffers(1, &m_SubBuf);
        glNamedBufferStorage(m_SubBuf, (GLsizeiptr)Decals::MAX_SUBSTRATES * (GLsizeiptr)sizeof(Decals::GpuSubstrate), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);
        m_SubDirty = !m_Subs.empty();
    }
    if (!m_SubDirty || m_Subs.empty()) return;
    m_SubDirty = false;
    std::vector<Decals::GpuSubstrate> up(m_Subs.size());
    for (size_t i = 0; i < m_Subs.size(); ++i) up[i] = m_Subs[i].Gpu;
    glNamedBufferSubData(m_SubBuf, 0, (GLsizeiptr)(up.size() * sizeof(Decals::GpuSubstrate)), up.data());
}

void DecalSystem::ClearSubstrates() {
    m_Subs.clear();
    m_SubIndex.clear();
    m_SubDirty = false;
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
        if (changed) {
            m_Textures->Clear();
            ClearSubstrates();
            SubstratePresets::ClearCache();
        }
    }
    m_Textures->BeginFrame();
    ++m_FrameNo;
    m_InPrepare = true;
    // Substrates used in the last ~10 s stay pinned: a few slots shared by whole walls, which would
    // otherwise pop off and back on as the view turns away and the decals' textures make room.
    for (uint32_t i = 0; i < (uint32_t)m_Subs.size(); ++i)
        if (m_FrameNo - m_Subs[i].Used < 600) RefreshSubstrate(i);

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
        // Substrate: Auto (the receiver material's when it has one, else the asset's - only for an asset
        // that has one, so graffiti never reveals the wall), Asset, Custom (this decal's own) or Off.
        uint32_t sub = 0;
        if (dc.SubstrateMode == 2) {
            sub = SubstrateIndex(CustomSubstrate(dc));
        } else if (dc.SubstrateMode == 4) {
            if (!dc.SubPreset.empty()) sub = SubstrateIndex(SubstratePresets::Get(dc.SubPreset));
        } else if (dc.SubstrateMode != 3 && asset->Substrate.Enabled()) {
            sub = SubstrateIndex(asset->Substrate);
            if (dc.SubstrateMode == 0) inst.Gpu.Ids.z |= Decals::FLAG_SUBSTRATE_AUTO;
        }
        inst.Gpu.Ids.x |= sub << Decals::SUBSTRATE_SHIFT;
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
    m_InPrepare = false;
    FlushSubstrates();
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
    if (m_SubBuf) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_SUBSTRATES, m_SubBuf);
    m_Textures->Bind();
}
