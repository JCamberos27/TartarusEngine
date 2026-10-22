#include "DecalTextureArrays.h"
#include "Texture.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>

// ---- DecalPageAllocator ---------------------------------------------------------------------

int DecalPageAllocator::SlotSizeFor(int tier) {
    int size = kMinSlot;
    while (size < tier && size < Decals::PAGE_SIZE) size <<= 1;
    return size;
}

void DecalPageAllocator::SetMaxLayers(int n) {
    m_MaxLayers = std::max({n, 1, LayerCount()});
}

bool DecalPageAllocator::FindFree(const Layer& layer, int target, int& level, int& ix, int& iy) const {
    // Walk the reachable nodes: a free node is a candidate block, a used one stops, a split one descends. The
    // exact level wins outright; otherwise the deepest (smallest) larger block, so a fresh 4K layer
    // isn't split while a free 2K quadrant elsewhere would do.
    int bestLevel = -1, bestX = 0, bestY = 0;
    struct Node { int L, X, Y; };
    Node stack[kNodes];
    int top = 0;
    stack[top++] = {0, 0, 0};
    while (top > 0) {
        const Node n = stack[--top];
        const std::uint8_t s = layer[Index(n.L, n.X, n.Y)];
        if (s == kFree) {
            if (n.L == target) { level = n.L; ix = n.X; iy = n.Y; return true; }
            if (n.L < target && n.L > bestLevel) { bestLevel = n.L; bestX = n.X; bestY = n.Y; }
        } else if (s == kSplit && n.L + 1 < kLevels) {
            for (int c = 3; c >= 0; --c) stack[top++] = {n.L + 1, n.X * 2 + (c & 1), n.Y * 2 + (c >> 1)};
        }
    }
    if (bestLevel < 0) return false;
    level = bestLevel; ix = bestX; iy = bestY;
    return true;
}

DecalPageAllocator::Alloc DecalPageAllocator::Allocate(int size) {
    size = SlotSizeFor(size);
    int target = 0;
    while ((Decals::PAGE_SIZE >> target) > size) ++target;

    // Exact fits anywhere first, then the tightest split, then a new layer.
    int layerIdx = -1, level = 0, ix = 0, iy = 0;
    for (int pass = 0; pass < 2 && layerIdx < 0; ++pass) {
        int bestLevel = -1;
        for (int l = 0; l < LayerCount(); ++l) {
            int lv, x, y;
            if (!FindFree(m_Layers[l], target, lv, x, y)) continue;
            if (pass == 0 && lv != target) continue;
            if (lv > bestLevel) { bestLevel = lv; layerIdx = l; level = lv; ix = x; iy = y; }
            if (pass == 0) break;
        }
    }
    if (layerIdx < 0) {
        if (LayerCount() >= m_MaxLayers) return {};
        Layer fresh{};
        fresh.fill(kFree);
        m_Layers.push_back(fresh);
        layerIdx = LayerCount() - 1;
        level = 0; ix = 0; iy = 0;
    }
    Layer& layer = m_Layers[layerIdx];
    // Split down to the target, always taking the first child.
    while (level < target) {
        layer[Index(level, ix, iy)] = kSplit;
        ++level;
        ix *= 2;
        iy *= 2;
        for (int c = 0; c < 4; ++c) layer[Index(level, ix + (c & 1), iy + (c >> 1))] = kFree;
    }
    layer[Index(level, ix, iy)] = kUsed;
    m_UsedTexels += (long long)size * size;
    Alloc a;
    a.Layer = layerIdx;
    a.Size = size;
    a.X = ix * size;
    a.Y = iy * size;
    return a;
}

void DecalPageAllocator::Free(const Alloc& a) {
    if (!a.Valid() || a.Layer >= LayerCount()) return;
    int level = 0;
    while ((Decals::PAGE_SIZE >> level) > a.Size) ++level;
    int ix = a.X / a.Size, iy = a.Y / a.Size;
    Layer& layer = m_Layers[a.Layer];
    if (layer[Index(level, ix, iy)] != kUsed) return;
    layer[Index(level, ix, iy)] = kFree;
    m_UsedTexels -= (long long)a.Size * a.Size;
    while (level > 0) {
        const int px = ix / 2, py = iy / 2;
        bool allFree = true;
        for (int c = 0; c < 4; ++c) allFree &= layer[Index(level, px * 2 + (c & 1), py * 2 + (c >> 1))] == kFree;
        if (!allFree) break;
        --level;
        ix = px;
        iy = py;
        layer[Index(level, ix, iy)] = kFree;
    }
}

bool DecalPageAllocator::CheckNode(const Layer& layer, int level, int ix, int iy, long long& used) const {
    const std::uint8_t s = layer[Index(level, ix, iy)];
    const long long size = Decals::PAGE_SIZE >> level;
    if (s == kUsed) { used += size * size; return true; }
    if (s == kFree) return true;
    if (s != kSplit || level + 1 >= kLevels) return false;
    bool allFree = true;
    for (int c = 0; c < 4; ++c) {
        const int cx = ix * 2 + (c & 1), cy = iy * 2 + (c >> 1);
        if (!CheckNode(layer, level + 1, cx, cy, used)) return false;
        allFree &= layer[Index(level + 1, cx, cy)] == kFree;
    }
    return !allFree;
}

bool DecalPageAllocator::CheckInvariants() const {
    long long used = 0;
    for (const Layer& layer : m_Layers)
        if (!CheckNode(layer, 0, 0, 0, used)) return false;
    return used == m_UsedTexels && LayerCount() <= m_MaxLayers;
}

// ---- DecalTextureArrays ---------------------------------------------------------------------

namespace {
constexpr unsigned kFormat[4] = {
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, // albedo
    GL_COMPRESSED_RG_RGTC2,                 // normal
    GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,       // surface
    GL_COMPRESSED_RED_RGTC1,                // aux
};
constexpr int kBlockBytes[4] = {16, 16, 16, 8};
constexpr std::uint32_t kHasFlag[4] = {0, Decals::FLAG_HAS_NORMAL_TEX, Decals::FLAG_HAS_SURFACE_TEX,
                                       Decals::FLAG_HAS_AUX_TEX};

std::size_t LevelBytes(int kind, int level) {
    const std::size_t blocks = (std::size_t)std::max(1, (Decals::PAGE_SIZE >> level) / 4);
    return blocks * blocks * (std::size_t)kBlockBytes[kind];
}

TextureImportSettings SettingsFor(int kind, int size) {
    using S = TextureImportSettings;
    S s;
    s.GenerateMipmaps = true;
    s.MaxTextureSize = size;
    s.CompressionMode = S::Compression::Normal;
    s.FilterMode = S::Filter::Trilinear;
    s.WrapMode = S::Wrap::ClampToEdge;
    s.IsSRGB = kind == 0;
    s.TextureType = kind == 1 ? S::Type::NormalMap : S::Type::Default;
    s.ForceFormat = kind == 1 ? S::ForcedFormat::BC5 : kind == 3 ? S::ForcedFormat::BC4 : S::ForcedFormat::BC3;
    return s;
}

// A map decoded raw (8-bit, no mips) at exactly size^2, skipping the texture cache (from memory); empty
// when it's missing or another size.
TextureCpuData RawMap(const std::string& path, int size) {
    TextureCpuData t;
    if (path.empty()) return t;
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return t;
    TextureImportSettings s;
    s.GenerateMipmaps = false;
    s.MaxTextureSize = size;
    s.CompressionMode = TextureImportSettings::Compression::None;
    s.IsSRGB = false;
    t = Texture::DecodeMemory(path, std::move(bytes), 0, 0, s);
    if (!t.Ok || t.GLFormat != 0 || t.Width != size || t.Height != size || t.Channels < 1 ||
        t.Pixels.size() < (size_t)size * size * t.Channels) {
        Log::Warn("Decals: substrate map '" + path + "' isn't " + std::to_string(size) + "^2 - left out.");
        t = TextureCpuData{};
    }
    return t;
}

// Separable box blur of radius r, edges clamped (running sums: O(n) whatever r).
void BoxBlur(std::vector<float>& v, int size, int r) {
    std::vector<float> tmp(v.size());
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float>& src = pass == 0 ? v : tmp;
        std::vector<float>& dst = pass == 0 ? tmp : v;
        for (int line = 0; line < size; ++line) {
            auto idx = [&](int i) -> size_t {
                i = std::clamp(i, 0, size - 1);
                return pass == 0 ? (size_t)line * size + i : (size_t)i * size + line;
            };
            float sum = 0.0f;
            for (int i = -r; i <= r; ++i) sum += src[idx(i)];
            for (int i = 0; i < size; ++i) {
                dst[idx(i)] = sum / (float)(2 * r + 1);
                sum += src[idx(i + r + 1)] - src[idx(i - r)];
            }
        }
    }
}

// Substrate detail (SubstrateDesc::Detail): the albedo's fine luminance relief - lightly smoothed,
// minus its ~1 cm average - as a height, positive up. Empty when the albedo doesn't decode at size.
std::vector<float> DetailHeight(const std::string& albedo, int size) {
    const TextureCpuData a = RawMap(albedo, size);
    if (a.Pixels.empty()) return {};
    const size_t n = (size_t)size * size;
    std::vector<float> l(n);
    const int ch = a.Channels;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* p = &a.Pixels[i * ch];
        l[i] = ch >= 3 ? (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f : p[0] / 255.0f;
    }
    std::vector<float> wide = l;
    BoxBlur(l, size, 1);
    BoxBlur(wide, size, 12);
    for (size_t i = 0; i < n; ++i) l[i] -= wide[i];
    return l;
}

// The substrate's normal map with the detail's slopes whiteout-blended in (OpenGL green up), BC5.
TextureCpuData DetailNormal(const std::string& normal, const std::vector<float>& h, float detail, int size) {
    const TextureCpuData base = RawMap(normal, size);
    const float gain = 40.0f * detail;
    auto H = [&](int x, int y) { return h[(size_t)std::clamp(y, 0, size - 1) * size + std::clamp(x, 0, size - 1)]; };
    std::vector<unsigned char> rgba((size_t)size * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const size_t i = (size_t)y * size + x;
            // Image y runs down; the normal's +y is up the image.
            const glm::vec2 d(-(H(x + 1, y) - H(x - 1, y)) * 0.5f * gain, (H(x, y + 1) - H(x, y - 1)) * 0.5f * gain);
            glm::vec3 b(0.0f, 0.0f, 1.0f);
            if (!base.Pixels.empty() && base.Channels >= 2) {
                const unsigned char* p = &base.Pixels[i * base.Channels];
                b = glm::vec3(p[0] / 127.5f - 1.0f, p[1] / 127.5f - 1.0f, base.Channels >= 3 ? p[2] / 127.5f - 1.0f : 1.0f);
            }
            const glm::vec3 nn = glm::normalize(glm::vec3(glm::vec2(b) + d, std::max(b.z, 0.05f)));
            rgba[i * 4 + 0] = (unsigned char)std::lround(std::clamp(nn.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[i * 4 + 1] = (unsigned char)std::lround(std::clamp(nn.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[i * 4 + 2] = (unsigned char)std::lround(std::clamp(nn.z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
            rgba[i * 4 + 3] = 255;
        }
    return Texture::DecodeMemory(normal.empty() ? std::string("substrate-detail-normal") : normal + "#detail", std::move(rgba),
                                 size, size, SettingsFor(1, size));
}

// A substrate's surface map (DecalTextureArrays::Source::Rough / AO): both greyscale maps decoded
// raw at the slot size and packed r roughness, g 0, b AO (times the detail's cavities when there are
// any), a 1, then BC3-encoded with mips like any surface map.
TextureCpuData PackSurface(const std::string& rough, const std::string& ao, const std::vector<float>& h, float detail,
                           int size) {
    const TextureCpuData r = RawMap(rough, size), a = RawMap(ao, size);
    if (r.Pixels.empty() && a.Pixels.empty() && h.empty()) return {};
    std::vector<unsigned char> rgba((size_t)size * size * 4);
    for (size_t i = 0, n = (size_t)size * size; i < n; ++i) {
        float occ = a.Pixels.empty() ? 1.0f : a.Pixels[i * a.Channels] / 255.0f;
        if (!h.empty()) occ *= std::clamp(1.0f + h[i] * 7.0f * detail, 0.3f, 1.0f); // pits below their surroundings
        rgba[i * 4 + 0] = r.Pixels.empty() ? 255 : r.Pixels[i * r.Channels];
        rgba[i * 4 + 1] = 0;
        rgba[i * 4 + 2] = (unsigned char)std::lround(occ * 255.0f);
        rgba[i * 4 + 3] = 255;
    }
    const std::string name = rough.empty() ? (ao.empty() ? std::string("substrate-detail-surface") : ao) : rough;
    return Texture::DecodeMemory(name, std::move(rgba), size, size, SettingsFor(2, size));
}
} // namespace

struct DecalTextureArrays::Decoded {
    TextureCpuData Data[kKinds];
    bool Has[kKinds] = {};
};

DecalTextureArrays::DecalTextureArrays() { UpdateMaxLayers(); }

DecalTextureArrays::~DecalTextureArrays() {
    m_Slots.clear(); // waits out any decode still running (std::async futures block on destruction)
    for (unsigned int& t : m_Tex)
        if (t) glDeleteTextures(1, &t);
    if (m_Placeholder) glDeleteTextures(1, &m_Placeholder);
}

std::string DecalTextureArrays::KeyFor(const Source& s) {
    return std::to_string(DecalPageAllocator::SlotSizeFor(s.Tier)) + "|" + s.Albedo + "|" + s.Normal + "|" + s.Surface +
           "|" + s.Aux +
           (s.Rough.empty() && s.AO.empty() && s.Detail <= 0.0f ? std::string()
                                                              : "|pack|" + s.Rough + "|" + s.AO + "|" + std::to_string(s.Detail));
}

std::size_t DecalTextureArrays::PerLayerBytes(const bool active[kKinds]) const {
    std::size_t bytes = 0;
    for (int k = 0; k < kKinds; ++k) {
        if (k != kAlbedo && !active[k]) continue;
        for (int l = 0; l < kArrayLevels; ++l) bytes += LevelBytes(k, l);
    }
    return bytes;
}

void DecalTextureArrays::UpdateMaxLayers() {
    const std::size_t perLayer = PerLayerBytes(m_Active);
    const int layers = (int)std::max<std::size_t>(1, m_BudgetBytes / std::max<std::size_t>(perLayer, 1));
    m_Alloc.SetMaxLayers(layers);
    if (m_Alloc.LayerCount() * perLayer > m_BudgetBytes && !m_WarnedBudget) {
        m_WarnedBudget = true;
        Log::Warn("Decals: the texture arrays are over their budget (" + std::to_string(m_BudgetBytes >> 20) +
                  " MB, Project Settings > Quality) since a normal / surface / aux map came in; raise it or use fewer.");
    }
}

void DecalTextureArrays::SetBudgetMB(int mb) {
    const std::size_t bytes = (std::size_t)std::max(16, mb) << 20;
    if (bytes == m_BudgetBytes) return;
    m_BudgetBytes = bytes;
    m_WarnedBudget = false;
    UpdateMaxLayers();
}

std::size_t DecalTextureArrays::GpuBytes() const {
    if (m_GpuLayers == 0) return 0;
    bool active[kKinds];
    for (int k = 0; k < kKinds; ++k) active[k] = m_Tex[k] != 0;
    return PerLayerBytes(active) * (std::size_t)m_GpuLayers;
}

int DecalTextureArrays::ResidentCount() const {
    int n = 0;
    for (const auto& [key, e] : m_Slots) n += e->S.Ready ? 1 : 0;
    return n;
}

void DecalTextureArrays::Clear() {
    for (auto& [key, e] : m_Slots) {
        if (e->Pending.valid()) e->Pending.wait();
        m_Alloc.Free(e->S.Alloc);
    }
    m_Slots.clear();
    m_InFlight = 0;
    m_Deferred = false;
}

void DecalTextureArrays::Drop(const Source& src) {
    const auto it = m_Slots.find(KeyFor(src));
    if (it == m_Slots.end()) return;
    Entry& e = *it->second;
    if (e.Pending.valid()) { // its decode is of the old files: let it land, then throw it away
        e.Pending.wait();
        --m_InFlight;
    }
    if (e.S.Alloc.Valid()) m_Alloc.Free(e.S.Alloc);
    m_Slots.erase(it);
}

bool DecalTextureArrays::Resident(const Source& src) const {
    const auto it = m_Slots.find(KeyFor(src));
    return it != m_Slots.end() && it->second->S.Ready;
}

bool DecalTextureArrays::AllocateWithEviction(int size, DecalPageAllocator::Alloc& out) {
    for (;;) {
        out = m_Alloc.Allocate(size);
        if (out.Valid()) return true;
        // The least recently used resident slot nobody asked for this frame. Pending ones keep
        // their space: their decode will land in it.
        auto victim = m_Slots.end();
        for (auto it = m_Slots.begin(); it != m_Slots.end(); ++it) {
            const Slot& s = it->second->S;
            if (!s.Ready || s.Refs > 0) continue;
            if (victim == m_Slots.end() || s.LastUsed < victim->second->S.LastUsed) victim = it;
        }
        if (victim == m_Slots.end()) return false;
        // A slot asked for last frame too: what's in view doesn't fit the budget, and the decals take
        // each other's space in turn (some missing each frame) for as long as the view holds.
        if (victim->second->S.LastUsed + 1 >= m_Frame) {
            m_LastThrash = m_Frame;
            if (!m_WarnedThrash) {
                m_WarnedThrash = true;
                Log::Warn("Decals: the decals in view need more texture memory than the budget (" +
                          std::to_string(m_BudgetBytes >> 20) +
                          " MB, Project Settings > Quality), so some drop out in turn; raise it or use fewer different decals.");
            }
        }
        m_Alloc.Free(victim->second->S.Alloc);
        m_Slots.erase(victim);
    }
}

void DecalTextureArrays::EnsureGpu(int layers, const bool want[kKinds]) {
    layers = std::max(layers, m_GpuLayers);
    for (int k = 0; k < kKinds; ++k) {
        if (!(want[k] || m_Active[k] || k == kAlbedo)) continue;
        const bool fresh = m_Tex[k] == 0;
        if (!fresh && layers <= m_GpuLayers) continue;
        unsigned int tex = 0;
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &tex);
        glTextureStorage3D(tex, kArrayLevels, kFormat[k], Decals::PAGE_SIZE, Decals::PAGE_SIZE, layers);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_MAX_LEVEL, kArrayLevels - 1);
        const float aniso = 8.0f;
        glTextureParameterfv(tex, GL_TEXTURE_MAX_ANISOTROPY, &aniso);
        // Growing keeps every resident page: a GPU-side copy, no re-decode.
        if (!fresh && m_GpuLayers > 0) {
            for (int l = 0; l < kArrayLevels; ++l) {
                const int s = std::max(4, Decals::PAGE_SIZE >> l);
                glCopyImageSubData(m_Tex[k], GL_TEXTURE_2D_ARRAY, l, 0, 0, 0, tex, GL_TEXTURE_2D_ARRAY, l, 0, 0, 0, s, s,
                                   m_GpuLayers);
            }
        }
        if (m_Tex[k]) glDeleteTextures(1, &m_Tex[k]);
        m_Tex[k] = tex;
        m_Active[k] = true;
    }
    m_GpuLayers = layers;
}

void DecalTextureArrays::UploadSlot(Entry& e, Decoded& d) {
    Slot& s = e.S;
    if (!d.Has[kAlbedo]) {
        s.Failed = true;
        m_Alloc.Free(s.Alloc);
        s.Alloc = {};
        return;
    }
    const bool before[kKinds] = {m_Active[0], m_Active[1], m_Active[2], m_Active[3]};
    EnsureGpu(m_Alloc.LayerCount(), d.Has);
    if (before[1] != m_Active[1] || before[2] != m_Active[2] || before[3] != m_Active[3]) UpdateMaxLayers();
    const int size = s.Alloc.Size;
    for (int k = 0; k < kKinds; ++k) {
        if (!d.Has[k]) continue;
        const TextureCpuData& t = d.Data[k];
        std::size_t off = 0;
        for (int l = 0; l <= s.MaxLod && l < (int)t.LevelSizes.size(); ++l) {
            const int ls = size >> l;
            glCompressedTextureSubImage3D(m_Tex[k], l, s.Alloc.X >> l, s.Alloc.Y >> l, s.Alloc.Layer, ls, ls, 1, kFormat[k],
                                          (GLsizei)t.LevelSizes[l], t.Pixels.data() + off);
            off += t.LevelSizes[l];
        }
        s.TexFlags |= kHasFlag[k];
    }
    s.Ready = true;
}

void DecalTextureArrays::BeginFrame() {
    ++m_Frame;
    m_Deferred = false;
    for (auto& [key, e] : m_Slots) {
        e->S.Refs = 0;
        if (!e->Pending.valid()) continue;
        if (e->Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        std::shared_ptr<Decoded> d;
        try { d = e->Pending.get(); } catch (...) {} // a worker that threw (out of memory) counts as failed
        --m_InFlight;
        if (d) {
            UploadSlot(*e, *d);
        } else {
            e->S.Failed = true;
            m_Alloc.Free(e->S.Alloc);
            e->S.Alloc = {};
        }
    }
}

const DecalTextureArrays::Slot* DecalTextureArrays::Request(const Source& src) {
    if (src.Albedo.empty()) return nullptr;
    const std::string key = KeyFor(src);
    if (auto it = m_Slots.find(key); it != m_Slots.end()) {
        Slot& s = it->second->S;
        s.LastUsed = m_Frame;
        ++s.Refs;
        return s.Ready ? &s : nullptr;
    }
    if (m_InFlight >= kMaxInFlight) {
        m_Deferred = true;
        return nullptr; // asked again next frame
    }

    const int size = DecalPageAllocator::SlotSizeFor(src.Tier);
    // Budget as if this decal's extra maps were already in: the layer count is what they'd cost.
    const bool saved[kKinds] = {m_Active[0], m_Active[1], m_Active[2], m_Active[3]};
    m_Active[kNormal] |= !src.Normal.empty();
    m_Active[kNormal] |= src.Detail > 0.0f;
    m_Active[kSurface] |= !src.Surface.empty() || !src.Rough.empty() || !src.AO.empty() || src.Detail > 0.0f;
    m_Active[kAux] |= !src.Aux.empty();
    UpdateMaxLayers();
    std::copy(std::begin(saved), std::end(saved), m_Active);
    DecalPageAllocator::Alloc alloc;
    const bool placed = AllocateWithEviction(size, alloc);
    UpdateMaxLayers();
    if (!placed) return nullptr; // every slot is pinned by this frame's decals: try again next frame

    auto e = std::make_unique<Entry>();
    Slot& s = e->S;
    s.Alloc = alloc;
    s.LastUsed = m_Frame;
    s.Refs = 1;
    s.MaxLod = 0;
    while ((size >> (s.MaxLod + 1)) >= 4 && s.MaxLod + 1 < kArrayLevels) ++s.MaxLod;
    const float inv = 1.0f / (float)Decals::PAGE_SIZE;
    s.Rect = glm::vec4((float)alloc.X * inv, (float)alloc.Y * inv, (float)size * inv, (float)size * inv);

    Texture::InitGpuCaps(); // the BCn formats the worker encodes depend on the driver (main thread, idempotent)
    const std::string paths[kKinds] = {src.Albedo, src.Normal, src.Surface, src.Aux};
    const float detail = src.Detail;
    const bool pack = src.Surface.empty() && (!src.Rough.empty() || !src.AO.empty() || detail > 0.0f);
    e->Pending = std::async(std::launch::async, [p0 = paths[0], p1 = paths[1], p2 = paths[2], p3 = paths[3], size, pack,
                                                 rough = src.Rough, ao = src.AO, detail]() {
        auto d = std::make_shared<Decoded>();
        const std::string* ps[kKinds] = {&p0, &p1, &p2, &p3};
        const std::vector<float> h = detail > 0.0f ? DetailHeight(p0, size) : std::vector<float>();
        for (int k = 0; k < kKinds; ++k) {
            const bool packed = k == kSurface && pack;
            const bool derivedNormal = k == kNormal && !h.empty();
            if (ps[k]->empty() && !packed && !derivedNormal) continue;
            d->Data[k] = packed        ? PackSurface(rough, ao, h, detail, size)
                       : derivedNormal ? DetailNormal(*ps[k], h, detail, size)
                                       : Texture::DecodeFile(*ps[k], SettingsFor(k, size));
            const TextureCpuData& t = d->Data[k];
            d->Has[k] = t.Ok && t.GLFormat == kFormat[k] && t.Width == size && t.Height == size && !t.LevelSizes.empty();
            if (!d->Has[k]) d->Data[k] = TextureCpuData{};
        }
        return d;
    });
    ++m_InFlight;
    m_Slots.emplace(key, std::move(e));
    return nullptr;
}

void DecalTextureArrays::Bind() const {
    if (!m_Placeholder) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_Placeholder);
        glTextureStorage3D(m_Placeholder, 1, GL_RGBA8, 1, 1, 1);
        const unsigned char px[4] = {0, 0, 0, 0};
        glTextureSubImage3D(m_Placeholder, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTextureParameteri(m_Placeholder, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Placeholder, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const int units[kKinds] = {Decals::UNIT_ALBEDO, Decals::UNIT_NORMAL, Decals::UNIT_SURFACE, Decals::UNIT_AUX};
    for (int k = 0; k < kKinds; ++k) glBindTextureUnit((unsigned)units[k], m_Tex[k] ? m_Tex[k] : m_Placeholder);
}
