#include "DecalSystem.h"
#include "DecalAsset.h"
#include "DecalTextureArrays.h"
#include "World.h"
#include "Components.h"
#include "Frustum.h"
#include "LiveReload.h"
#include "ProjectPaths.h"
#include "ProjectSettings.h"
#include "Log.h"
#include "TimeService.h"
#include "gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// TARTARUS_NO_SUBSTRATE=1: every substrate off (before / after comparisons).
bool SubstratesOff() {
    static const bool off = [] {
#pragma warning(suppress : 4996)
        const char* v = std::getenv("TARTARUS_NO_SUBSTRATE");
        return v && *v && *v != '0';
    }();
    return off;
}

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

// Every field of a DecalComponent, compared: the cached instance is rebuilt when any differs. A field added
// to DecalComponent has to be added here too (the size check below catches a new one).
bool SameComponent(const DecalComponent& a, const DecalComponent& b) {
    return a.Size == b.Size && a.Tint == b.Tint && a.Opacity == b.Opacity && a.AlbedoOp == b.AlbedoOp && a.NormalOp == b.NormalOp &&
           a.RoughnessOp == b.RoughnessOp && a.MetalOp == b.MetalOp && a.AOOp == b.AOOp && a.EmissiveOp == b.EmissiveOp &&
           a.SortOrder == b.SortOrder && a.LayerMask == b.LayerMask && a.AffectStatic == b.AffectStatic &&
           a.AffectDynamic == b.AffectDynamic && a.Seed == b.Seed && a.Frame == b.Frame && a.RotationJitter == b.RotationJitter &&
           a.ScaleJitter == b.ScaleJitter && a.RandomFlip == b.RandomFlip && a.Fade == b.Fade && a.Bleach == b.Bleach &&
           a.Peel == b.Peel && a.Buff == b.Buff && a.SubstrateMode == b.SubstrateMode && a.SubTiling == b.SubTiling &&
           a.SubTint == b.SubTint && a.SubStrength == b.SubStrength && a.SubRoughMin == b.SubRoughMin &&
           a.SubRoughMax == b.SubRoughMax && a.SubMirror == b.SubMirror && a.SubCropX0 == b.SubCropX0 && a.SubCropY0 == b.SubCropY0 &&
           a.SubCropX1 == b.SubCropX1 && a.SubCropY1 == b.SubCropY1 && a.AssetPath == b.AssetPath && a.SubPreset == b.SubPreset &&
           a.SubAlbedo == b.SubAlbedo && a.SubNormal == b.SubNormal && a.SubRoughness == b.SubRoughness && a.SubAO == b.SubAO;
}
static_assert(sizeof(DecalComponent) == 6 * sizeof(std::string) + 152,
              "DecalComponent changed: compare the new field in SameComponent (DecalSystem.cpp), then update this size");

// (SortOrder, entity) -> one key, ascending = drawn first (underneath).
uint64_t PackSortKey(int sortOrder, uint32_t entity) {
    const uint64_t order = (uint64_t)(uint16_t)(std::clamp(sortOrder, -32768, 32767) + 32768);
    return order << 32 | (uint64_t)entity;
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
    // Age sun-bleaches paint, not dirt: the bleach lifts a colour toward a pale grey, which turned
    // a dark grime stain into a pale puddle on a dark floor. Dark tints (grime, stains) take less of it.
    const float tintLuma = glm::dot(tint, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    const float ageBleach = g.Age * 0.6f * glm::smoothstep(0.05f, 0.4f, tintLuma);
    const float bleach = Unit(a.Bleach + dc.Bleach + ageBleach);
    const float peel = Unit(a.Peel + dc.Peel + g.Wear * 0.5f);
    const float buff = Unit(a.Buff + dc.Buff + g.Wear * 0.3f);
    const float wear = Unit(g.Wear);
    out.H2 = glm::uvec4(Half2(a.Paint ? a.PaintRoughness : a.Roughness, a.Paint ? a.PaintMetal : a.Metal),
                        Half2(a.Paint ? a.Relief : 0.0f, wear), Half2(fadeW, bleach), Half2(peel, buff));
}

// The box's inverse as the record's three WorldToBox rows.
void WorldToBoxRows(const glm::mat4& boxToWorld, glm::vec4 rows[3]) {
    const glm::mat4 inv = glm::inverse(boxToWorld);
    for (int r = 0; r < 3; ++r) rows[r] = glm::vec4(inv[0][r], inv[1][r], inv[2][r], inv[3][r]);
}

// The cull's view of a box: its world AABB half-extent (from the bounds' half axes).
glm::vec3 ExtentOf(const Decals::GpuDecalBounds& b) {
    return glm::abs(glm::vec3(b.AxisX)) + glm::abs(glm::vec3(b.AxisY)) + glm::abs(glm::vec3(b.AxisZ));
}

DecalTextureArrays::Source SourceFor(const DecalAsset& a) {
    DecalTextureArrays::Source src;
    src.Albedo = Resolve(a.AlbedoPath);
    src.Normal = Resolve(a.NormalPath);
    src.Surface = Resolve(a.SurfacePath);
    src.Aux = Resolve(a.AuxPath);
    src.Tier = a.Tier;
    src.Seal();
    return src;
}

constexpr uint32_t kGhostEntity = 0xFFFFFFFFu, kRuntimeEntity = 0xFFFFFFFFu;

} // namespace

// ---- DecalPerf ------------------------------------------------------------------------------------

namespace DecalPerf {
namespace {
struct State {
    std::uint64_t Frame = ~0ull;
    double FramePrepare = 0.0, FrameSubmit = 0.0;
    std::vector<float> Totals; // per frame: prepare + submit (ms)
    std::vector<std::uint64_t> FrameNos;
    std::vector<float> Prepares; // per frame: the prepare part
    double SumPrepare = 0.0, SumSubmit = 0.0, MaxTotal = 0.0;
    std::uint64_t MaxFrame = 0, Calls = 0, Dropouts = 0;
};
State& S() {
    static State s;
    return s;
}
long long NowNs() {
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Close() {
    State& s = S();
    if (s.Frame == ~0ull) return;
    const double total = s.FramePrepare + s.FrameSubmit;
    s.Totals.push_back((float)total);
    s.FrameNos.push_back(s.Frame);
    s.Prepares.push_back((float)s.FramePrepare);
    s.SumPrepare += s.FramePrepare;
    s.SumSubmit += s.FrameSubmit;
    if (total > s.MaxTotal) {
        s.MaxTotal = total;
        s.MaxFrame = s.Frame;
    }
    s.FramePrepare = s.FrameSubmit = 0.0;
}
void Roll() {
    State& s = S();
    const std::uint64_t f = Time::FrameCount();
    if (f == s.Frame) return;
    Close();
    s.Frame = f;
    if (!s.Totals.empty() && s.Totals.size() % 600 == 0) Report();
}
} // namespace

void AddPrepare(double ms) {
    Roll();
    S().FramePrepare += ms;
    ++S().Calls;
}
void AddSubmit(double ms) {
    Roll();
    S().FrameSubmit += ms;
}
void AddDropout() { ++S().Dropouts; }
void Report() {
    const State& s = S();
    if (s.Totals.empty()) return;
    std::vector<float> t = s.Totals;
    std::sort(t.begin(), t.end());
    const double n = (double)t.size();
    std::printf("[DecalPerf] frames %zu, prepares %llu: prepare %.4f ms/frame, submit %.4f ms/frame, median %.4f, p99 %.4f, "
                "max %.3f ms (frame %llu), frames over 1 ms %d, over 4 ms %d, dropouts %llu\n",
                t.size(), (unsigned long long)s.Calls, s.SumPrepare / n, s.SumSubmit / n, t[t.size() / 2],
                t[std::min(t.size() - 1, (size_t)(n * 0.99))], s.MaxTotal, (unsigned long long)s.MaxFrame,
                (int)(t.end() - std::upper_bound(t.begin(), t.end(), 1.0f)),
                (int)(t.end() - std::upper_bound(t.begin(), t.end(), 4.0f)), (unsigned long long)s.Dropouts);
    // The worst frames after the first 60 (the scene's load and first decodes): the hitches in play.
    std::vector<size_t> worst;
    for (size_t i = 60; i < s.Totals.size(); ++i) worst.push_back(i);
    const size_t k = std::min<size_t>(6, worst.size());
    std::partial_sort(worst.begin(), worst.begin() + (std::ptrdiff_t)k, worst.end(),
                      [&](size_t a, size_t b) { return s.Totals[a] > s.Totals[b]; });
    if (k > 0) {
        std::printf("[DecalPerf]   worst after the first 60 frames:");
        for (size_t i = 0; i < k; ++i)
            std::printf(" %.3f ms (frame %llu, prepare %.3f)", s.Totals[worst[i]], (unsigned long long)s.FrameNos[worst[i]],
                        s.Prepares[worst[i]]);
        std::printf("\n");
    }
    std::fflush(stdout);
}
SubmitScope::SubmitScope() : Start(NowNs()) {}
SubmitScope::~SubmitScope() { AddSubmit((double)(NowNs() - Start) * 1e-6); }
} // namespace DecalPerf

// ---- cached state ---------------------------------------------------------------------------------

// One .decal, read once: its resolved maps as a sealed Source (the slot key hashed once).
struct DecalSystem::AssetEntry {
    std::string Path; // absolute, as first asked for
    std::shared_ptr<DecalAsset> Asset;
    DecalTextureArrays::Source Src; // its maps (when the asset loaded)
    bool Usable = false;            // loaded, with an albedo
    long long Time = 0;             // the file's write time when read (hot reload)
};

// One DecalComponent's decal, rebuilt when the component, its transform or its asset changes.
struct DecalSystem::StaticDecal {
    uint32_t Entity = 0;
    std::uint64_t Seen = 0; // the frame it was last walked in
    // What it was built from.
    DecalComponent Comp;
    glm::mat4 World{1.0f};
    std::uint32_t AssetGen = 0;
    // Built: null Asset = nothing to draw (no asset, no albedo, no opacity, no receivers).
    AssetEntry* Asset = nullptr;
    glm::mat4 BoxToWorld{1.0f};
    glm::vec4 WorldToBox[3] = {};
    Decals::GpuDecalBounds Bounds{};
    glm::vec3 Centre{0.0f}, Ext{0.0f};
    float ExtLen = 0.0f;
    std::uint64_t SortKey = 0;
    std::uint32_t SortSeed = 0;
    int Frame = 0;
    bool HasSub = false, SubAuto = false;
    SubstrateDesc SubDesc;
    SubRef Sub;
    // The packed record at full opacity (no substrate bits), and the slot it was packed against.
    bool Packed = false;
    const DecalTextureArrays::Slot* Slot = nullptr;
    DecalPageAllocator::Alloc SlotAlloc;
    glm::vec4 SlotRect{0.0f};
    int SlotLod = 0;
    std::uint32_t SlotFlags = 0;
    std::uint64_t GradingGen = 0;
    Decals::GpuDecal Gpu{};
    // A reveal (SetReveal): its maps, the aux swapped for the order.
    bool RevealValid = false;
    std::string RevealAux;
    DecalTextureArrays::Source RevealSrc;
    std::uint64_t DrawnAt = 0; // the Prepare it last drew in (DecalPerf dropouts)
};

// A runtime decal's (or a pushed instance's) geometry, derived once per submission.
struct DecalSystem::RuntimeExtra {
    glm::vec4 WorldToBox[3];
    Decals::GpuDecalBounds Bounds;
    glm::vec3 Centre, Ext;
    float ExtLen;
    std::uint64_t SortKey;
    std::uint32_t SortSeed;
};

// One decal this view draws: the sort's keys, its distance (the MAX_VISIBLE cut), and its record.
struct DecalSystem::VisItem {
    std::uint64_t Key;
    std::uint32_t Seed;
    std::uint32_t Seq; // category (components, the ghost, pushed, runtime) << 28 | gather order: the stable sort's ties
    float Dist;
    std::uint32_t Ref; // m_ViewGpu / m_ViewBounds
};

namespace {
bool VisLess(const std::uint64_t ak, std::uint32_t as, std::uint32_t aq, std::uint64_t bk, std::uint32_t bs, std::uint32_t bq) {
    if (ak != bk) return ak < bk;
    if (as != bs) return as < bs;
    return aq < bq;
}
} // namespace

DecalSystem::DecalSystem() : m_Textures(std::make_unique<DecalTextureArrays>()) { s_Active = this; }

DecalSystem* DecalSystem::Current() { return s_Active; }

DecalSystem::~DecalSystem() {
    if (s_Active == this) s_Active = nullptr;
    DecalPerf::Report();
    for (int i = 0; i < kRing; ++i) {
        if (m_DecalBuf[i]) glDeleteBuffers(1, &m_DecalBuf[i]);
        if (m_BoundsBuf[i]) glDeleteBuffers(1, &m_BoundsBuf[i]);
    }
    if (m_SubBuf) glDeleteBuffers(1, &m_SubBuf);
}

// ---- substrates -----------------------------------------------------------------------------------

DecalSystem::SubRef DecalSystem::ResolveSubstrate(const SubstrateDesc& desc) {
    // Looked up by the desc as given, then by its resolved files: a material's absolute paths and a
    // .decal's project-relative ones name the same maps, and share one entry (and one slot).
    const std::uint64_t key = desc.Hash();
    if (auto it = m_SubIndex.find(key); it != m_SubIndex.end()) return {it->second, m_Subs[it->second].Gen};
    SubstrateDesc norm = desc;
    for (std::string* p : {&norm.Albedo, &norm.Normal, &norm.Roughness, &norm.AO}) *p = NormalPath(Resolve(*p));
    const std::uint64_t normKey = norm.Hash();
    if (auto nt = m_SubIndex.find(normKey); nt != m_SubIndex.end()) {
        m_SubIndex.emplace(key, nt->second);
        m_Subs[nt->second].Keys.push_back(key);
        return {nt->second, m_Subs[nt->second].Gen};
    }
    uint32_t idx;
    if ((int)m_Subs.size() < Decals::MAX_SUBSTRATES) {
        idx = (uint32_t)m_Subs.size();
        m_Subs.emplace_back();
    } else {
        // Full: the entry asked for least recently, if that was over ~10 s ago (dragging a Custom
        // substrate's sliders makes a new entry per value; they'd fill the table for good otherwise).
        idx = ~0u;
        for (uint32_t i = 0; i < (uint32_t)m_Subs.size(); ++i)
            if (m_FrameNo - m_Subs[i].Used >= 600 && (idx == ~0u || m_Subs[i].Used < m_Subs[idx].Used)) idx = i;
        if (idx == ~0u) return {};
        for (std::uint64_t k : m_Subs[idx].Keys) m_SubIndex.erase(k);
        const std::uint32_t gen = m_Subs[idx].Gen + 1;
        m_Subs[idx] = SubstrateEntry{};
        m_Subs[idx].Gen = gen;
    }
    SubstrateEntry& e = m_Subs[idx];
    e.Keys.push_back(normKey);
    if (key != normKey) e.Keys.push_back(key);
    e.Src.Albedo = norm.Albedo;
    e.Src.Normal = norm.Normal;
    e.Src.Rough = norm.Roughness;
    e.Src.AO = norm.AO;
    e.Src.Detail = norm.Detail;
    e.Src.Tier = 2048;
    e.Src.Seal();
    e.Gpu.Crop = desc.Crop;
    e.Gpu.TintStrength = glm::vec4(desc.Tint, Unit(desc.Strength));
    e.Gpu.Params = glm::vec4(std::max(desc.Tiling, 0.01f), desc.RoughRange.x, desc.RoughRange.y, desc.Mirror ? 1.0f : 0.0f);
    e.Used = m_FrameNo;
    m_SubIndex.emplace(key, idx);
    m_SubIndex.emplace(normKey, idx);
    return {idx, e.Gen};
}

uint32_t DecalSystem::TouchSubstrate(SubRef& ref, const SubstrateDesc& desc) {
    if (SubstratesOff() || !desc.Enabled()) return 0;
    if (ref.Idx >= (uint32_t)m_Subs.size() || m_Subs[ref.Idx].Gen != ref.Gen) {
        ref = ResolveSubstrate(desc);
        if (ref.Idx == ~0u) return 0;
    }
    SubstrateEntry& e = m_Subs[ref.Idx];
    e.Used = m_FrameNo;
    RefreshSubstrate(ref.Idx);
    // Outside Prepare (a material bound mid-frame) a record not uploaded yet would be read stale: 0 until
    // the next Prepare uploads it.
    return e.Ready && (m_InPrepare || e.Uploaded) ? ref.Idx + 1 : 0;
}

uint32_t DecalSystem::SubstrateIndex(const SubstrateDesc& desc) {
    SubRef ref;
    return TouchSubstrate(ref, desc);
}

void DecalSystem::RefreshSubstrate(uint32_t idx) {
    SubstrateEntry& e = m_Subs[idx];
    if (e.Frame != m_FrameNo) {
        // Once per frame: pin the slot (it may have moved since an eviction) and refresh the record.
        e.Frame = m_FrameNo;
        const DecalTextureArrays::Slot* slot = m_Textures->Request(e.Src.Key, e.Src);
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
                e.Uploaded = false;
                m_SubDirty = true;
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
    if (!s_Active || !desc.Enabled()) return 0;
    MaterialSub& m = s_Active->m_MaterialSubs[&desc];
    if (!m.Desc.SameAs(desc)) { // a material seen for the first time, or edited
        m.Desc = desc;
        m.Ref = {};
    }
    return (int)s_Active->TouchSubstrate(m.Ref, desc);
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
    static std::vector<Decals::GpuSubstrate> up;
    up.resize(m_Subs.size());
    for (size_t i = 0; i < m_Subs.size(); ++i) {
        up[i] = m_Subs[i].Gpu;
        m_Subs[i].Uploaded = true;
    }
    glNamedBufferSubData(m_SubBuf, 0, (GLsizeiptr)(up.size() * sizeof(Decals::GpuSubstrate)), up.data());
}

// ---- assets ---------------------------------------------------------------------------------------

DecalSystem::AssetEntry* DecalSystem::Asset(const std::string& path) {
    // Accepts project-relative or absolute (DecalComponent::AssetPath is absolute in memory, often the
    // lower-cased path key); the cache key is the absolute path, lower case with forward slashes.
    const std::string abs = Resolve(path);
    std::string key = NormalPath(abs);
    if (auto it = m_Assets.find(key); it != m_Assets.end()) return it->second.get();
    auto e = std::make_unique<AssetEntry>();
    e->Path = abs;
    e->Asset = DecalAsset::Load(abs);
    e->Time = WriteTime(abs);
    e->Usable = e->Asset && !e->Asset->Missing && !e->Asset->AlbedoPath.empty();
    if (e->Asset && !e->Asset->Missing) e->Src = SourceFor(*e->Asset);
    AssetEntry* raw = e.get();
    m_Assets.emplace(std::move(key), std::move(e));
    m_AssetList.push_back(raw);
    return raw;
}

std::shared_ptr<DecalAsset> DecalSystem::GetAsset(const std::string& projectRelPath) { return Asset(projectRelPath)->Asset; }

void DecalSystem::Reload(AssetEntry& a) {
    // Its old maps go (they may be what changed), the .decal is read again, and its new maps decode afresh.
    if (a.Asset && !a.Asset->Missing) m_Textures->Drop(a.Src);
    a.Asset = DecalAsset::Load(a.Path);
    a.Time = WriteTime(a.Path);
    a.Usable = a.Asset && !a.Asset->Missing && !a.Asset->AlbedoPath.empty();
    a.Src = a.Asset && !a.Asset->Missing ? SourceFor(*a.Asset) : DecalTextureArrays::Source{};
    if (a.Asset && !a.Asset->Missing) m_Textures->Drop(a.Src);
    ++m_AssetGen; // every cached instance re-reads its asset (and the substrate presets)
}

void DecalSystem::Refresh(const std::string& path) {
    const size_t before = m_Assets.size();
    AssetEntry* a = Asset(path);
    if (m_Assets.size() != before) { // just read: only its maps to re-decode
        if (a->Asset && !a->Asset->Missing) m_Textures->Drop(a->Src);
        return;
    }
    Reload(*a);
}

bool DecalSystem::Resident(const std::string& path) {
    const AssetEntry* a = Asset(path);
    return a->Asset && !a->Asset->Missing && m_Textures->Resident(a->Src);
}

bool DecalSystem::Resident(const std::string& path, uint32_t entity) {
    const auto reveal = m_Reveals.find(entity);
    if (reveal == m_Reveals.end()) return Resident(path);
    const AssetEntry* a = Asset(path);
    if (!a->Asset || a->Asset->Missing || reveal->second.Aux.empty()) return false;
    DecalTextureArrays::Source src = a->Src;
    src.Aux = reveal->second.Aux; // as Prepare asks for it while the piece goes on
    src.Key = 0;
    return m_Textures->Resident(src);
}

void DecalSystem::SetReveal(uint32_t entity, const std::string& aux, float threshold, float band) {
    RevealOverride& r = m_Reveals[entity];
    if (r.Aux != aux) r.Aux = aux;
    r.Threshold = threshold;
    r.Band = band;
}

// ---- runtime decals -------------------------------------------------------------------------------

void DecalSystem::PushRuntime(const DecalInstance& d) {
    m_Runtime.push_back(d);
    m_RuntimeDirty = true;
}

void DecalSystem::BeginRuntimeFrame() {
    m_Runtime.clear();
    m_RuntimeDecals.clear();
    m_RuntimeDirty = true;
}

void DecalSystem::SubmitRuntime(const RuntimeDecal& d) {
    if (d.Maps && !d.Maps->Albedo.empty()) {
        m_RuntimeDecals.push_back(d);
        m_RuntimeDirty = true;
    }
}

void DecalSystem::DeriveRuntime() {
    // Once per submission (not per view): each box's inverse, bounds and key, then the draw order -
    // keys, then seeds, ties in submission order (pushed instances first), as the stable sort had it.
    m_RuntimeDirty = false;
    const size_t pushed = m_Runtime.size(), total = pushed + m_RuntimeDecals.size();
    m_RuntimeExtra.resize(total);
    for (size_t i = 0; i < total; ++i) {
        RuntimeExtra& x = m_RuntimeExtra[i];
        const glm::mat4& box = i < pushed ? m_Runtime[i].BoxToWorld : m_RuntimeDecals[i - pushed].BoxToWorld;
        WorldToBoxRows(box, x.WorldToBox);
        x.Bounds = BoundsFor(box);
        x.Centre = glm::vec3(x.Bounds.Centre);
        x.Ext = ExtentOf(x.Bounds);
        x.ExtLen = glm::length(x.Ext);
        if (i < pushed) {
            x.SortKey = m_Runtime[i].SortKey;
            x.SortSeed = m_Runtime[i].SortSeed;
        } else {
            const RuntimeDecal& r = m_RuntimeDecals[i - pushed];
            const uint32_t k = (uint32_t)(i - pushed);
            x.SortKey = PackSortKey(r.SortOrder, kRuntimeEntity);
            x.SortSeed = r.Id ? r.Id : k;
        }
    }
    m_RuntimeOrder.resize(total);
    for (uint32_t i = 0; i < (uint32_t)total; ++i) m_RuntimeOrder[i] = i;
    std::sort(m_RuntimeOrder.begin(), m_RuntimeOrder.end(), [&](uint32_t a, uint32_t b) {
        const RuntimeExtra &x = m_RuntimeExtra[a], &y = m_RuntimeExtra[b];
        return VisLess(x.SortKey, x.SortSeed, a, y.SortKey, y.SortSeed, b);
    });
}

uint64_t DecalSystem::SortKeyFor(int sortOrder, uint32_t entity) { return PackSortKey(sortOrder, entity); }

bool DecalSystem::Contributes(const Decals::GpuDecal& g) {
    using namespace Decals;
    const uint32_t ch = (g.Ids.x >> 16) & (CH_ALBEDO | CH_NORMAL | CH_ROUGHNESS | CH_METAL | CH_AO | CH_EMISSIVE);
    return ch != 0 || (g.Ids.x >> SUBSTRATE_SHIFT) != 0 || (g.Ids.z & (FLAG_SUBSTRATE_AUTO | FLAG_LIQUID)) != 0;
}

void DecalSystem::PackRuntime(const RuntimeDecal& r, const DecalTextureArrays::Slot& slot, Decals::GpuDecal& out, float fade) {
    using namespace Decals;
    const int cols = std::max(1, r.Cols), rows = std::max(1, r.Rows);
    const int f = ((r.Frame % (cols * rows)) + cols * rows) % (cols * rows);
    const float fw = slot.Rect.z / (float)cols, fh = slot.Rect.w / (float)rows;
    out.Rect = glm::vec4(slot.Rect.x + (float)(f % cols) * fw, slot.Rect.y + (float)(f / cols) * fh, fw, fh);
    int maxLod = slot.MaxLod;
    const int frameTexels = slot.Alloc.Size / std::max(cols, rows);
    while (maxLod > 0 && (frameTexels >> maxLod) < 4) --maxLod;

    // The distance fade on every channel, as a component decal's opacity (PackDecal).
    const float albedoOp = Unit(r.AlbedoOp) * fade;
    const float normalOp = (slot.TexFlags & FLAG_HAS_NORMAL_TEX) ? Unit(r.NormalOp) * fade : 0.0f;
    const float roughOp = Unit(r.RoughnessOp) * fade, metalOp = Unit(r.MetalOp) * fade, aoOp = Unit(r.AOOp) * fade;
    uint32_t ch = 0;
    if (albedoOp > 0.0f) ch |= CH_ALBEDO;
    if (normalOp > 0.0f) ch |= CH_NORMAL;
    if (roughOp > 0.0f) ch |= CH_ROUGHNESS;
    if (metalOp > 0.0f) ch |= CH_METAL;
    if (aoOp > 0.0f) ch |= CH_AO;
    uint32_t flags = slot.TexFlags;
    if (r.AffectStatic) flags |= FLAG_STATIC_RECEIVERS;
    if (r.AffectDynamic) flags |= FLAG_DYNAMIC_RECEIVERS;
    const bool reveal = r.Reveal && (slot.TexFlags & FLAG_HAS_AUX_TEX);
    if (reveal) flags |= FLAG_REVEAL;
    if (r.Liquid) flags |= FLAG_LIQUID;
    out.Ids = glm::uvec4((uint32_t)slot.Alloc.Layer | (uint32_t)maxLod << 8 | ch << 16, 0xFFFFFFFFu, flags, 0u);
    out.H0 = glm::uvec4(Half2(r.Tint.r, r.Tint.g), Half2(r.Tint.b, albedoOp), Half2(normalOp, roughOp), Half2(metalOp, aoOp));
    // A liquid's cover gain sits where the glow colour would (runtime decals never glow).
    out.H1 = glm::uvec4(Half2(r.Liquid ? std::max(r.CoverGain, 0.0f) : 0.0f, 0.0f), Half2(0.0f, 0.0f),
                        Half2(r.AngleFadeStart, r.AngleFadeEnd), Half2(r.EdgeFade, 4.0f));
    out.H2 = glm::uvec4(Half2(r.Roughness, r.Metal), Half2(0.0f, Unit(r.Wear)), Half2(Unit(r.Fade), 0.0f),
                        reveal ? Half2(std::max(r.RevealThreshold, 0.0f), std::max(r.RevealBand, 1e-3f)) : Half2(0.0f, 0.0f));
}

void DecalSystem::SetGhost(const DecalComponent& d, const glm::mat4& boxWorld) {
    if (!m_Ghost) m_Ghost = std::make_unique<DecalComponent>();
    *m_Ghost = d;
    m_GhostWorld = boxWorld;
    m_GhostActive = true;
}

// ---- pure helpers ---------------------------------------------------------------------------------

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
    std::stable_sort(v.begin(), v.end(), [](const DecalInstance& a, const DecalInstance& b) {
        return a.SortKey != b.SortKey ? a.SortKey < b.SortKey : a.SortSeed < b.SortSeed;
    });
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

// ---- the frame ------------------------------------------------------------------------------------

void DecalSystem::BeginFrame(World& world) {
    m_Textures->SetBudgetMB(ProjectSettings::Quality().DecalTextureBudgetMB);
    // Hot reload (editor and development builds): an edited .decal is re-read, and only its maps
    // re-decoded, within about a second. Each frame stats its share of the cached files, round robin.
    if (LiveReload::Enabled() && !m_AssetList.empty()) {
        const size_t n = m_AssetList.size(), share = (n + 59) / 60;
        bool changed = false;
        for (size_t i = 0; i < share; ++i) {
            AssetEntry& a = *m_AssetList[m_ReloadCursor++ % n];
            const long long now = WriteTime(a.Path);
            if (now == a.Time) continue;
            Reload(a);
            changed = true;
        }
        if (changed) SubstratePresets::ClearCache(); // a .decal naming a preset: it may be what changed too
    }
    m_Textures->BeginFrame();
    ++m_FrameNo;
    // Substrates used in the last ~10 s stay pinned: a few slots shared by whole walls, which would
    // otherwise pop off and back on as the view turns away and the decals' textures make room.
    for (uint32_t i = 0; i < (uint32_t)m_Subs.size(); ++i)
        if (m_FrameNo - m_Subs[i].Used < 600) RefreshSubstrate(i);
    auto& reg = world.Registry;
    for (auto it = m_Reveals.begin(); it != m_Reveals.end();) // a reveal whose entity is gone (Play stopped)
        it = reg.valid((entt::entity)it->first) ? std::next(it) : m_Reveals.erase(it);
}

void DecalSystem::RefreshStatic(World& world) {
    if (&world != m_StaticWorld) { // another world: its entity numbers name other things
        m_Static.clear();
        m_StaticWorld = &world;
        m_StaticOrderDirty = true;
    }
    auto& reg = world.Registry;
    for (auto [entity, dc] : reg.view<const DecalComponent>().each()) {
        if (reg.all_of<InactiveTag>(entity)) continue;
        const uint32_t id = (uint32_t)entt::to_integral(entity);
        std::unique_ptr<StaticDecal>& slot = m_Static[id];
        const bool fresh = !slot;
        if (fresh) {
            slot = std::make_unique<StaticDecal>();
            slot->Entity = id;
        }
        StaticDecal& s = *slot;
        s.Seen = m_FrameNo;
        const glm::mat4 boxWorld = world.GetCachedWorldTransform(entity);
        if (!fresh && s.AssetGen == m_AssetGen && boxWorld == s.World && SameComponent(s.Comp, dc)) continue;

        // Rebuild: the box, its inverse and bounds, the key, the substrate; the record repacks on its next draw.
        const bool wasDrawn = s.Asset != nullptr;
        const std::uint64_t oldKey = s.SortKey;
        const std::uint32_t oldSeed = s.SortSeed;
        s.Comp = dc;
        s.World = boxWorld;
        s.AssetGen = m_AssetGen;
        s.Packed = false;
        s.RevealValid = false;
        s.Sub = {};
        s.HasSub = s.SubAuto = false;
        s.Asset = nullptr;
        if (!dc.AssetPath.empty() && dc.Opacity > 0.0f && (dc.AffectStatic || dc.AffectDynamic)) {
            AssetEntry* a = Asset(dc.AssetPath);
            if (a->Usable) {
                const DecalAsset& asset = *a->Asset;
                s.Asset = a;
                s.BoxToWorld = boxWorld * VariationFor(dc.Seed, dc.RotationJitter, dc.ScaleJitter, dc.RandomFlip) *
                               glm::scale(glm::mat4(1.0f), glm::max(dc.Size, glm::vec3(0.001f)));
                WorldToBoxRows(s.BoxToWorld, s.WorldToBox);
                s.Bounds = BoundsFor(s.BoxToWorld);
                s.Centre = glm::vec3(s.Bounds.Centre);
                s.Ext = ExtentOf(s.Bounds);
                s.ExtLen = glm::length(s.Ext);
                s.SortKey = PackSortKey(dc.SortOrder, id);
                s.SortSeed = (uint32_t)dc.Seed;
                s.Frame = dc.Frame >= 0 ? dc.Frame : FrameFor(dc.Seed, asset.AtlasCols * asset.AtlasRows);
                // Substrate: Auto (the receiver material's when it has one, else the asset's - only for an asset
                // that has one, so graffiti never reveals the wall), Asset, Custom (this decal's own) or Off.
                if (dc.SubstrateMode == 2) {
                    s.SubDesc = CustomSubstrate(dc);
                    s.HasSub = true;
                } else if (dc.SubstrateMode == 4) {
                    if (!dc.SubPreset.empty()) {
                        s.SubDesc = SubstratePresets::Get(dc.SubPreset);
                        s.HasSub = true;
                    }
                } else if (dc.SubstrateMode != 3 && asset.Substrate.Enabled()) {
                    s.SubDesc = asset.Substrate;
                    s.HasSub = true;
                    s.SubAuto = dc.SubstrateMode == 0;
                }
            }
        }
        if (fresh || wasDrawn != (s.Asset != nullptr) || oldKey != s.SortKey || oldSeed != s.SortSeed) m_StaticOrderDirty = true;
    }
    for (auto it = m_Static.begin(); it != m_Static.end();) { // gone, or inactive
        if (it->second->Seen == m_FrameNo) {
            ++it;
            continue;
        }
        it = m_Static.erase(it);
        m_StaticOrderDirty = true;
    }
    if (m_StaticOrderDirty) {
        // Their draw order: (SortOrder, entity, seed) - unique per entity, so no ties to keep.
        m_StaticOrderDirty = false;
        m_StaticSorted.clear();
        for (auto& [id, s] : m_Static)
            if (s->Asset) m_StaticSorted.push_back(s.get());
        std::sort(m_StaticSorted.begin(), m_StaticSorted.end(), [](const StaticDecal* a, const StaticDecal* b) {
            return a->SortKey != b->SortKey ? a->SortKey < b->SortKey : a->SortSeed < b->SortSeed;
        });
    }
}

void DecalSystem::PackStatic(StaticDecal& s, const DecalTextureArrays::Slot& slot) {
    PackDecal(*s.Asset->Asset, s.Comp, slot, m_Grading, 1.0f, s.Frame, s.Gpu);
    for (int r = 0; r < 3; ++r) s.Gpu.WorldToBox[r] = s.WorldToBox[r];
    if (s.SubAuto) s.Gpu.Ids.z |= Decals::FLAG_SUBSTRATE_AUTO;
    s.Packed = true;
    s.Slot = &slot;
    s.SlotAlloc = slot.Alloc;
    s.SlotRect = slot.Rect;
    s.SlotLod = slot.MaxLod;
    s.SlotFlags = slot.TexFlags;
    s.GradingGen = m_GradingGen;
}

int DecalSystem::Prepare(World& world, const glm::mat4& viewProj, const glm::vec3& cameraPos, float maxDistance,
                         const DecalGrading& grading, bool includeGhost) {
    struct Timer {
        std::chrono::steady_clock::time_point T0 = std::chrono::steady_clock::now();
        ~Timer() { DecalPerf::AddPrepare(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count()); }
    } timer;
    ++m_PrepareNo;
    if (grading.Age != m_Grading.Age || grading.Wear != m_Grading.Wear) {
        m_Grading = grading;
        ++m_GradingGen; // every cached record repacks
    }
    // The frame's bookkeeping once, whatever the number of views: uploads, the LRU's frame, substrate
    // pins, hot reload, and the components' cache.
    const std::uint64_t frame = Time::FrameCount();
    if (frame != m_LastFrame) {
        m_LastFrame = frame;
        BeginFrame(world);
        RefreshStatic(world);
    } else if (&world != m_StaticWorld) {
        RefreshStatic(world);
    }
    if (m_RuntimeDirty) DeriveRuntime();
    m_InPrepare = true;

    const Frustum frustum = Frustum::FromViewProj(viewProj);
    const float fadeStart = maxDistance * 0.8f;
    auto fadeAt = [&](float d) { return d > fadeStart ? 1.0f - (d - fadeStart) / std::max(maxDistance - fadeStart, 1e-3f) : 1.0f; };
    // Frustum and distance; -1 = out.
    auto consider = [&](const glm::vec3& c, const glm::vec3& ext, float extLen) -> float {
        if (!frustum.Intersects(AABB{c - ext, c + ext})) return -1.0f;
        const float d = std::max(0.0f, glm::length(c - cameraPos) - extLen);
        return d > maxDistance ? -1.0f : d;
    };
    auto dropout = [&](std::uint64_t drawnAt) {
        if (drawnAt != 0 && m_PrepareNo - drawnAt <= 2) DecalPerf::AddDropout();
    };
    m_Vis.clear();
    m_ViewGpu.clear();
    m_ViewBounds.clear();
    auto emit = [&](const Decals::GpuDecal& g, const Decals::GpuDecalBounds& b, std::uint64_t key, std::uint32_t seed,
                    std::uint32_t seq, float d) {
        m_Vis.push_back({key, seed, seq, d, (std::uint32_t)m_ViewGpu.size()});
        m_ViewGpu.push_back(g);
        m_ViewBounds.push_back(b);
    };

    // The components, in their (cached) draw order.
    const bool anyReveal = !m_Reveals.empty();
    Decals::GpuDecal g;
    for (StaticDecal* sp : m_StaticSorted) {
        StaticDecal& s = *sp;
        const float d = consider(s.Centre, s.Ext, s.ExtLen);
        if (d < 0.0f) continue;
        const float fade = fadeAt(d);
        const DecalAsset& asset = *s.Asset->Asset;
        const auto reveal = anyReveal ? m_Reveals.find(s.Entity) : m_Reveals.end();
        if (reveal == m_Reveals.end()) {
            // Only resident textures draw; the rest decode in the background and show up a frame or so later.
            const DecalTextureArrays::Slot* slot = m_Textures->Request(s.Asset->Src.Key, s.Asset->Src);
            if (!slot) {
                dropout(s.DrawnAt);
                continue;
            }
            if (!s.Packed || s.GradingGen != m_GradingGen || s.Slot != slot || s.SlotAlloc.Layer != slot->Alloc.Layer ||
                s.SlotAlloc.X != slot->Alloc.X || s.SlotAlloc.Y != slot->Alloc.Y || s.SlotAlloc.Size != slot->Alloc.Size ||
                s.SlotRect != slot->Rect || s.SlotLod != slot->MaxLod || s.SlotFlags != slot->TexFlags)
                PackStatic(s, *slot);
            if (fade < 1.0f) { // in the distance fade's band: packed again at this opacity
                PackDecal(asset, s.Comp, *slot, m_Grading, fade, s.Frame, g);
                for (int r = 0; r < 3; ++r) g.WorldToBox[r] = s.WorldToBox[r];
                if (s.SubAuto) g.Ids.z |= Decals::FLAG_SUBSTRATE_AUTO;
            } else {
                g = s.Gpu;
            }
        } else {
            // A piece going on (SetReveal): its reveal order in place of the aux map. Its own maps are asked
            // for too, so they're resident by the time the reveal is cleared (it doesn't blink out then).
            const RevealOverride& rv = reveal->second;
            if (!s.RevealValid || s.RevealAux != rv.Aux) {
                s.RevealSrc = s.Asset->Src;
                s.RevealSrc.Aux = rv.Aux;
                s.RevealSrc.Seal();
                s.RevealAux = rv.Aux;
                s.RevealValid = true;
            }
            m_Textures->Request(s.Asset->Src.Key, s.Asset->Src);
            const DecalTextureArrays::Slot* slot = m_Textures->Request(s.RevealSrc.Key, s.RevealSrc);
            if (!slot) {
                dropout(s.DrawnAt);
                continue;
            }
            PackDecal(asset, s.Comp, *slot, m_Grading, fade, s.Frame, g);
            for (int r = 0; r < 3; ++r) g.WorldToBox[r] = s.WorldToBox[r];
            if (!(slot->TexFlags & Decals::FLAG_HAS_AUX_TEX)) { // the order didn't decode: nothing shows yet
                dropout(s.DrawnAt);
                continue;
            }
            g.Ids.x &= ~((uint32_t)Decals::CH_EMISSIVE << 16); // the aux is the order now, not the glow mask
            g.Ids.z |= Decals::FLAG_REVEAL;
            g.H2.w = Half2(std::max(rv.Threshold, 0.0f), std::max(rv.Band, 1e-3f));
            if (s.SubAuto) g.Ids.z |= Decals::FLAG_SUBSTRATE_AUTO;
        }
        const uint32_t sub = s.HasSub ? TouchSubstrate(s.Sub, s.SubDesc) : 0;
        g.Ids.x |= sub << Decals::SUBSTRATE_SHIFT;
        if (!Contributes(g)) continue; // nothing left to show (faded out at the far distance)
        emit(g, s.Bounds, s.SortKey, s.SortSeed, 0u, d);
        s.DrawnAt = m_PrepareNo;
    }
    const size_t staticCount = m_Vis.size();

    // The editor's placement ghost: the real decal, drawn on top, in the editor's own views only. Built
    // from scratch each view (one decal).
    if (includeGhost && m_GhostActive && m_Ghost) {
        const DecalComponent& dc = *m_Ghost;
        AssetEntry* a = dc.AssetPath.empty() || dc.Opacity <= 0.0f || (!dc.AffectStatic && !dc.AffectDynamic) ? nullptr
                                                                                                             : Asset(dc.AssetPath);
        if (a && a->Usable) {
            const DecalAsset& asset = *a->Asset;
            const glm::mat4 box = m_GhostWorld * VariationFor(dc.Seed, dc.RotationJitter, dc.ScaleJitter, dc.RandomFlip) *
                                  glm::scale(glm::mat4(1.0f), glm::max(dc.Size, glm::vec3(0.001f)));
            const Decals::GpuDecalBounds b = BoundsFor(box);
            const glm::vec3 ext = ExtentOf(b);
            const float d = consider(glm::vec3(b.Centre), ext, glm::length(ext));
            const DecalTextureArrays::Slot* slot = d < 0.0f ? nullptr : m_Textures->Request(a->Src.Key, a->Src);
            if (slot) {
                const int atlasFrame = dc.Frame >= 0 ? dc.Frame : FrameFor(dc.Seed, asset.AtlasCols * asset.AtlasRows);
                PackDecal(asset, dc, *slot, m_Grading, fadeAt(d), atlasFrame, g);
                WorldToBoxRows(box, g.WorldToBox);
                uint32_t sub = 0;
                if (dc.SubstrateMode == 2) {
                    sub = SubstrateIndex(CustomSubstrate(dc));
                } else if (dc.SubstrateMode == 4) {
                    if (!dc.SubPreset.empty()) sub = SubstrateIndex(SubstratePresets::Get(dc.SubPreset));
                } else if (dc.SubstrateMode != 3 && asset.Substrate.Enabled()) {
                    sub = SubstrateIndex(asset.Substrate);
                    if (dc.SubstrateMode == 0) g.Ids.z |= Decals::FLAG_SUBSTRATE_AUTO;
                }
                g.Ids.x |= sub << Decals::SUBSTRATE_SHIFT;
                if (Contributes(g)) emit(g, b, PackSortKey(dc.SortOrder, kGhostEntity), (uint32_t)dc.Seed, 1u << 28, d);
            }
        }
    }
    const size_t ghostEnd = m_Vis.size();

    // The runtime decals (blood, bullet holes, pushed instances), in their draw order.
    const size_t pushed = m_Runtime.size();
    for (uint32_t i = 0; i < (uint32_t)m_RuntimeOrder.size(); ++i) {
        const uint32_t idx = m_RuntimeOrder[i];
        const RuntimeExtra& x = m_RuntimeExtra[idx];
        const float d = consider(x.Centre, x.Ext, x.ExtLen);
        if (d < 0.0f) continue;
        if (idx < pushed) {
            g = m_Runtime[idx].Gpu;
        } else {
            const RuntimeDecal& r = m_RuntimeDecals[idx - pushed];
            const DecalTextureArrays::Slot* slot = m_Textures->Request(r.Maps->Key ? r.Maps->Key : DecalTextureArrays::KeyOf(*r.Maps), *r.Maps);
            if (!slot) continue;
            PackRuntime(r, *slot, g, fadeAt(d));
        }
        for (int k = 0; k < 3; ++k) g.WorldToBox[k] = x.WorldToBox[k];
        if (!Contributes(g)) continue;
        emit(g, x.Bounds, x.SortKey, x.SortSeed, (idx < pushed ? 2u : 3u) << 28 | i, d);
    }

    // One draw order: the three runs (each already in order) merged - the components first on a tie, then
    // the ghost, then the runtime decals, as the stable sort over the gather order had them.
    auto less = [](const VisItem& a, const VisItem& b) { return VisLess(a.Key, a.Seed, a.Seq, b.Key, b.Seed, b.Seq); };
    if (ghostEnd > staticCount && m_Vis.size() > ghostEnd) { // the ghost into the runtime run
        size_t at = ghostEnd;
        while (at < m_Vis.size() && less(m_Vis[at], m_Vis[staticCount])) ++at;
        std::rotate(m_Vis.begin() + (std::ptrdiff_t)staticCount, m_Vis.begin() + (std::ptrdiff_t)staticCount + 1,
                    m_Vis.begin() + (std::ptrdiff_t)at);
    }
    if (staticCount > 0 && m_Vis.size() > staticCount && less(m_Vis[staticCount], m_Vis[staticCount - 1])) {
        static std::vector<VisItem> merged;
        merged.resize(m_Vis.size());
        std::merge(m_Vis.begin(), m_Vis.begin() + (std::ptrdiff_t)staticCount, m_Vis.begin() + (std::ptrdiff_t)staticCount,
                   m_Vis.end(), merged.begin(), less);
        m_Vis.swap(merged);
    }

    // Over budget: the nearest MAX_VISIBLE stay, in draw order.
    if ((int)m_Vis.size() > Decals::MAX_VISIBLE) {
        m_Cut.resize(m_Vis.size());
        for (uint32_t i = 0; i < (uint32_t)m_Cut.size(); ++i) m_Cut[i] = i;
        std::nth_element(m_Cut.begin(), m_Cut.begin() + Decals::MAX_VISIBLE, m_Cut.end(), [&](uint32_t a, uint32_t b) {
            return m_Vis[a].Dist != m_Vis[b].Dist ? m_Vis[a].Dist < m_Vis[b].Dist : a < b;
        });
        m_Cut.resize(Decals::MAX_VISIBLE);
        std::sort(m_Cut.begin(), m_Cut.end());
        for (uint32_t i = 0; i < (uint32_t)m_Cut.size(); ++i) m_Vis[i] = m_Vis[m_Cut[i]];
        m_Vis.resize(Decals::MAX_VISIBLE);
    }

    m_Visible = (int)m_Vis.size();
    m_UploadDecals.resize(m_Visible);
    m_UploadBounds.resize(m_Visible);
    m_NearestDynamic = 1e30f;
    for (int i = 0; i < m_Visible; ++i) {
        const VisItem& v = m_Vis[i];
        m_UploadDecals[i] = m_ViewGpu[v.Ref];
        m_UploadDecals[i].Ids.w = (uint32_t)i;
        m_UploadBounds[i] = m_ViewBounds[v.Ref];
        if (m_UploadDecals[i].Ids.z & Decals::FLAG_DYNAMIC_RECEIVERS) m_NearestDynamic = std::min(m_NearestDynamic, v.Dist);
    }
    // The next buffer of the ring: the last views' copies may still be read by the GPU.
    m_Ring = (m_Ring + 1) % kRing;
    if (!m_DecalBuf[m_Ring]) {
        glCreateBuffers(1, &m_DecalBuf[m_Ring]);
        glNamedBufferStorage(m_DecalBuf[m_Ring], (GLsizeiptr)Decals::MAX_VISIBLE * (GLsizeiptr)sizeof(Decals::GpuDecal), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);
        glCreateBuffers(1, &m_BoundsBuf[m_Ring]);
        glNamedBufferStorage(m_BoundsBuf[m_Ring], (GLsizeiptr)Decals::MAX_VISIBLE * (GLsizeiptr)sizeof(Decals::GpuDecalBounds),
                             nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    m_InPrepare = false;
    FlushSubstrates();
    if (m_Visible > 0) {
        glNamedBufferSubData(m_DecalBuf[m_Ring], 0, (GLsizeiptr)(m_Visible * sizeof(Decals::GpuDecal)), m_UploadDecals.data());
        glNamedBufferSubData(m_BoundsBuf[m_Ring], 0, (GLsizeiptr)(m_Visible * sizeof(Decals::GpuDecalBounds)), m_UploadBounds.data());
    }
    return m_Visible;
}

void DecalSystem::BindForCull() const {
    if (!m_BoundsBuf[m_Ring]) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_BOUNDS, m_BoundsBuf[m_Ring]);
}

void DecalSystem::BindForShading() const {
    if (m_DecalBuf[m_Ring]) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_DECALS, m_DecalBuf[m_Ring]);
    if (m_SubBuf) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, Decals::BIND_SUBSTRATES, m_SubBuf);
    m_Textures->Bind();
}
