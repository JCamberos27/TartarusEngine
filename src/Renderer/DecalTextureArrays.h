#pragma once
#include "DecalTypes.h"
#include <glm/glm.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct TextureCpuData;

// Quadtree sub-allocation of PAGE_SIZE^2 array layers (docs/DECALS.md). A 4096 slot fills a layer,
// 2048 a quadrant, 1024 and 512 the next levels down. Pure CPU, no GL (unit-tested): the texture
// arrays below keep one of these and place every decal's maps at the same rect in all four arrays.
class DecalPageAllocator {
public:
    static constexpr int kMinSlot = 512;
    static constexpr int kLevels = 4; // 4096, 2048, 1024, 512
    static constexpr int kNodes = 1 + 4 + 16 + 64;

    struct Alloc {
        int Layer = -1, X = 0, Y = 0, Size = 0; // texels within the layer
        bool Valid() const { return Layer >= 0; }
    };

    explicit DecalPageAllocator(int maxLayers = 1) : m_MaxLayers(maxLayers < 1 ? 1 : maxLayers) {}

    // The slot a tier gets: a power of two in [kMinSlot, PAGE_SIZE], rounded up.
    static int SlotSizeFor(int tier);

    // Prefers an exact free block, then the smallest larger one to split, then a fresh layer while
    // LayerCount() < MaxLayers(). Invalid when nothing fits (the caller evicts and retries).
    Alloc Allocate(int size);
    // Frees a block Allocate returned; four free siblings merge back into their parent.
    void Free(const Alloc& a);

    // Layers ever opened (they stay open once used, so the GL arrays only grow).
    int LayerCount() const { return (int)m_Layers.size(); }
    int MaxLayers() const { return m_MaxLayers; }
    void SetMaxLayers(int n); // never below LayerCount()
    long long UsedTexels() const { return m_UsedTexels; }

    // Every split node has its four children reachable, no split node has four free children (they
    // should have merged), and the used-texel count matches the used leaves.
    bool CheckInvariants() const;

private:
    enum State : std::uint8_t { kFree = 0, kUsed = 1, kSplit = 2 };
    using Layer = std::array<std::uint8_t, kNodes>;
    static int Base(int level) { return level == 0 ? 0 : level == 1 ? 1 : level == 2 ? 5 : 21; }
    static int Index(int level, int ix, int iy) { return Base(level) + iy * (1 << level) + ix; }
    // The reachable free block at the deepest level <= target in `layer` (exact fit first).
    bool FindFree(const Layer& layer, int target, int& level, int& ix, int& iy) const;
    bool CheckNode(const Layer& layer, int level, int ix, int iy, long long& used) const;

    std::vector<Layer> m_Layers;
    int m_MaxLayers = 1;
    long long m_UsedTexels = 0;
};

// The four decal texture arrays (units 22-25: albedo sRGB BC3, normal BC5, surface BC3, aux BC4),
// PAGE_SIZE^2 layers sub-allocated by DecalPageAllocator, with slots kept resident by LRU under the
// Project Settings budget. Maps are decoded and BCn-encoded on worker threads through the regular
// Texture decode + TextureCache (TextureImportSettings::ForceFormat), then uploaded here on the main
// thread. A slot is pinned (its refcount) by every Request() in the current frame and only an
// unpinned, least-recently-used slot is ever evicted. The normal / surface / aux arrays only exist
// once a resident decal has that map, so albedo-only libraries spend their budget on albedo.
class DecalTextureArrays {
public:
    // Absolute paths; empty = no such map. Tier is the slot size request (DecalAsset::Tier).
    struct Source {
        std::string Albedo, Normal, Surface, Aux;
        // Substrates (Substrate.h): with Surface empty, a surface map packed from these two separate
        // greyscale maps instead - r roughness, b AO (white where one is missing), g 0, a 1.
        std::string Rough, AO;
        // > 0: normal and AO detail derived from the albedo's luminance (SubstrateDesc::Detail).
        float Detail = 0.0f;
        int Tier = 2048;
    };
    struct Slot {
        DecalPageAllocator::Alloc Alloc;
        glm::vec4 Rect{0.0f};  // uv offset + size within the layer
        int MaxLod = 0;        // deepest mip that stays inside the slot (>= 4 texels)
        std::uint32_t TexFlags = 0; // Decals::FLAG_HAS_*_TEX for the maps that decoded
        bool Ready = false;
        bool Failed = false;   // the albedo didn't decode: never retried until Clear()
        std::uint64_t LastUsed = 0;
        int Refs = 0;          // Request()s this frame
    };

    DecalTextureArrays();
    ~DecalTextureArrays();
    DecalTextureArrays(const DecalTextureArrays&) = delete;
    DecalTextureArrays& operator=(const DecalTextureArrays&) = delete;

    // Main thread, once per frame before any Request(): unpins every slot and uploads the decodes
    // that have finished.
    void BeginFrame();
    // The resident slot for these maps, pinned for this frame; null while it decodes (or failed, or
    // the budget is full of pinned slots). Starts the decode the first time it's asked for.
    const Slot* Request(const Source& src);

    // Units 22-25. Arrays that don't exist yet get a 1x1 placeholder array.
    void Bind() const;

    // Megabytes the arrays may grow to (ProjectSettings::Quality().DecalTextureBudgetMB).
    void SetBudgetMB(int mb);
    std::size_t GpuBytes() const;
    int ResidentCount() const;
    int LayerCount() const { return m_GpuLayers; }
    // Drops every slot (hot reload of a decal or its textures); arrays keep their storage.
    void Clear();

    static constexpr int kArrayLevels = 11; // 4096 down to 4x4 (one BC block)
    static constexpr int kMaxInFlight = 3;  // decodes at once

private:
    struct Decoded;
    struct Entry {
        Slot S;
        std::future<std::shared_ptr<Decoded>> Pending;
    };
    enum Kind { kAlbedo = 0, kNormal, kSurface, kAux, kKinds };

    static std::string KeyFor(const Source& s);
    bool AllocateWithEviction(int size, DecalPageAllocator::Alloc& out);
    void EnsureGpu(int layers, const bool want[kKinds]);
    void UploadSlot(Entry& e, Decoded& d);
    std::size_t PerLayerBytes(const bool active[kKinds]) const;
    void UpdateMaxLayers();

    DecalPageAllocator m_Alloc;
    std::unordered_map<std::string, std::unique_ptr<Entry>> m_Slots;
    unsigned int m_Tex[kKinds] = {};
    bool m_Active[kKinds] = {};
    int m_GpuLayers = 0;
    std::size_t m_BudgetBytes = std::size_t(256) << 20;
    std::uint64_t m_Frame = 0;
    int m_InFlight = 0;
    mutable unsigned int m_Placeholder = 0;
    bool m_WarnedBudget = false;
};
