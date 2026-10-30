#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "ShaderPropType.h"
#include "Substrate.h"

class Texture;

// One value for a linked-shader property that is NOT one of the built-in PBR fields below
// (audit #354). Only the member matching Type is meaningful. Texture props keep both the resolved
// pointer (for binding) and the path (for saving).
struct MaterialProp {
    ShaderPropType Type = ShaderPropType::Float;
    float                    F = 0.0f;
    glm::vec4                V{0.0f, 0.0f, 0.0f, 1.0f};
    bool                     B = false;
    int                      I = 0;
    std::string              TexPath;
    std::shared_ptr<Texture> Tex;
};

// PBR (metallic-roughness workflow) material. Maps are optional; when absent the
// corresponding scalar factor is used uniformly across the surface.
struct Material {
    // The source file's material name (FBX/glTF), set on import. Lets a data file pick submeshes
    // by material instead of by submesh index (e.g. .fpsanim "weaponMaterials"). Not saved.
    std::string Name;

    glm::vec3 BaseColor{1.0f};
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    glm::vec3 EmissiveColor{0.0f};
    float EmissiveStrength = 1.0f;

    // World-space triplanar UV projection instead of the mesh's own UVs - for a surface (like a
    // scaled primitive cube used as level geometry) whose UVs don't tile sanely across faces of
    // very different sizes/aspect ratios. TriplanarScale is texture tiles per world unit.
    bool Triplanar = false;
    float TriplanarScale = 1.0f;

    std::shared_ptr<Texture> AlbedoMap;
    std::shared_ptr<Texture> NormalMap;
    // glTF-style packed map: G = roughness, B = metallic. Takes priority over the two
    // separate maps below when present (that's the glTF metallic-roughness convention).
    std::shared_ptr<Texture> MetallicRoughnessMap;
    // Standalone maps (red channel used) for asset packs that ship metallic and roughness
    // as separate grayscale textures instead of one packed glTF-style texture.
    std::shared_ptr<Texture> MetallicMap;
    std::shared_ptr<Texture> RoughnessMap;
    std::shared_ptr<Texture> AOMap;
    std::shared_ptr<Texture> EmissiveMap;

    // PR10: Clear Coat + Anisotropy
    float ClearCoat           = 0.0f;   // layer strength [0,1]; 0 = disabled
    float ClearCoatRoughness  = 0.5f;
    float Anisotropy          = 0.0f;   // [-1,1]; 0 = isotropic
    float AnisotropyRotation  = 0.0f;   // [0,1] maps to [0°,360°] in tangent plane
    std::shared_ptr<Texture> ClearCoatMap; // optional mask (.r channel)

    // PR11: Sheen/cloth + Subsurface translucency
    glm::vec3 Sheen{0.0f};             // tint color; zero = sheen disabled
    float SheenRoughness      = 0.5f;
    glm::vec3 SubsurfaceColor{1.0f, 0.8f, 0.6f}; // transmitted tint
    float Thickness           = 0.5f;  // surface thickness [0,1]
    std::shared_ptr<Texture> ThicknessMap; // optional per-texel thickness (.r)

    // PR12: Transmission + refraction
    float TransmissionStrength = 0.0f; // [0,1]; 0 = opaque
    float IOR                  = 1.5f; // index of refraction (glass=1.5, water=1.33)

    // #354: the ShaderAsset variant selector turns a lobe's `#ifdef` on when its authored
    // strength is nonzero. Subsurface and reflection probes have no natural "off" value
    // (Thickness defaults to 0.5, probes are a scene resource), so they are explicit opt-ins.
    bool SubsurfaceEnabled = false; // -> _SUBSURFACE variant
    bool ReflectionProbes  = false; // -> _REFLECTION_PROBES variant (parallax box reflections)

    // #101 — alpha cutout (Unity "Cutout" / glTF alphaMode MASK): fragments whose albedo-map
    // alpha is below AlphaCutoff are discarded, in the main pass AND the shadow passes. Off by
    // default: an opaque material's albedo alpha is NOT treated as coverage.
    bool  AlphaClip   = false;
    float AlphaCutoff = 0.5f;

    // #102 / #113 — surface options (Unity Standard / glTF equivalents). Defaults are identity.
    glm::vec2 UVTiling{1.0f};          // applied to every map (not triplanar)
    glm::vec2 UVOffset{0.0f};
    float NormalStrength = 1.0f;       // scales the normal map's slope
    bool  NormalFlipY    = false;      // DirectX-style normal map (green channel points down)
    bool  DoubleSided    = false;      // no back-face culling; back faces lit with a flipped normal
    bool  UseVertexColor = false;      // albedo (and alpha) x the mesh's vertex colour
    // Unity Standard's Forward Rendering Options: off = no highlight from lights / no sky or probe
    // reflection, for a fully matte surface whatever the roughness.
    bool  SpecularHighlights = true;
    bool  GlossyReflections  = true;
    std::shared_ptr<Texture> HeightMap;   // parallax occlusion mapping (white = high)
    float ParallaxScale  = 0.02f;
    std::shared_ptr<Texture> DetailAlbedoMap; // x2 detail: 50% grey leaves the colour unchanged
    std::shared_ptr<Texture> DetailNormalMap;
    glm::vec2 DetailTiling{4.0f};         // detail maps' UV tiling (on the mesh UVs)
    // Grunge (docs/DECALS.md): a grime map laid over the whole surface triplanar in world space (so it
    // ignores the UVs), multiplied into the albedo and roughening where it's dark. Unit 21.
    std::shared_ptr<Texture> GrungeMap;
    float GrungeStrength = 0.0f;          // 0 = off
    float GrungeScale    = 0.5f;          // tiles per metre
    bool  ReceiveDecals  = true;          // clustered decals project onto this surface
    // Foliage (docs: FoliageWind.glsl). Opt-in; off = the vertex and fragment shaders behave exactly as before.
    // Vertex wind: a world-space gust (global direction + time, Model::SetWind) scaled by a height mask (the mesh's
    // local Y over WindHeight, or the vertex colour's red when UseVertexColor is on) bends it, and a small
    // per-vertex flutter shakes the leaves. Shadows use the same offset. Translucency adds sun light through the leaf.
    bool  Foliage        = false;
    float WindStrength   = 0.15f;         // metres the top of WindHeight sways in a full gust
    float WindFrequency  = 1.0f;          // gust speed multiplier
    float WindFlutter    = 0.3f;          // 0 = all slow bend, 1 = all leaf flutter
    float WindHeight     = 6.0f;          // metres of local Y that map to a full wind mask
    float Translucency   = 0.0f;          // back-lit sun transmission, tinted by albedo; 0 = off
    // Substrate (Substrate.h): the material under the paint. Where the Grunge map's peel mask says the
    // paint is gone it shows instead (colour, normal, roughness, AO), and peeling-paint decals on Auto
    // reveal it too. Its maps live in the decal texture arrays (uMaterialSubstrate), not on units.
    SubstrateDesc Substrate;

    // Shell fur (FurRenderer): after the surface itself, copies of it pushed out along the (skinned) normal, each
    // alpha-tested against per-strand cells so the stack reads as hair. Only the opaque camera pass draws them:
    // they cast no shadows (the surface does) and skip the SSAO pre-pass. Not part of Hash(): the surface's own
    // BindMaterial reads none of it, and FurRenderer binds its own uniforms.
    struct FurDesc {
        bool  Enabled = false;
        float Length  = 0.015f;            // metres from the skin to the longest tips (white in the mask)
        float Density = 220.0f;            // strand cells per UV unit, across the 0-1 square
        int   Shells  = 16;                // the most shells drawn, up close; fewer with distance, none far away
        float Gravity = 0.3f;              // droop at the tips, as a share of Length
        glm::vec3 Tint{1.0f};              // x the albedo map
        float Rim     = 0.35f;             // back-lit sheen along the silhouette
        std::shared_ptr<Texture> Mask;     // R: where it grows and how long; none = everywhere, full length
        bool operator==(const FurDesc& o) const {
            return Enabled == o.Enabled && Length == o.Length && Density == o.Density && Shells == o.Shells &&
                   Gravity == o.Gravity && Tint == o.Tint && Rim == o.Rim && Mask == o.Mask;
        }
    };
    FurDesc Fur;
    static constexpr int kMaxFurShells = 32;

    // Values of a linked shader's custom (non-built-in) properties, keyed by property name
    // ("_Foo"), pushed as `u<Foo>` uniforms by BindMaterialDataDriven. Lives here rather than on
    // MaterialAsset (#104) so the Inspector's Material-level Get*/Set* reach them too.
    std::unordered_map<std::string, MaterialProp> ExtraProps;
    // #104 — the linked shader's custom keywords switched on for this material (anything in its
    // Keywords{} block that the engine doesn't drive itself; see ShaderAsset::IsBuiltinKeyword).
    std::vector<std::string> ShaderKeywords;

    // #192: a value hash of everything BindMaterial (Model.cpp) uploads — the scalar/vector
    // factors plus the identity of each bound texture. The draw loop sorts by this and
    // GLStateCache skips BindMaterial when it matches the last-bound one, so value-identical
    // materials on separate objects (every placed primitive gets its own Material instance)
    // dedupe, not just shared pointers. An FNV-style mix over the raw words; a 64-bit collision that
    // would swap two genuinely different materials for a frame is not a practical concern.
    // Mixed a 32-bit word at a time over four independent lanes, folded at the end: this runs per
    // mesh per pass, and one serial multiply chain over ~60 words was latency-bound (the byte-wise
    // FNV loop before it showed up in the shadow passes). Runtime-only: never saved or compared
    // across builds.
    // Worked out once per frame per material (it runs per mesh per pass: main, SSAO pre-pass, every shadow map):
    // the cache holds until NextHashFrame(), which the frame loop calls before anything renders. A copy starts
    // without it, so a material copied and changed within a frame hashes its own values. Code that must see an
    // edit made earlier in the same frame (preview keys) calls ComputeHash().
    std::uint64_t Hash() const {
        if (m_HashCache.Frame == s_HashFrame) return m_HashCache.Value;
        m_HashCache.Value = ComputeHash();
        m_HashCache.Frame = s_HashFrame;
        return m_HashCache.Value;
    }
    // A new frame: every material's Hash() is worked out again on first use (values may have been edited since).
    static void NextHashFrame() {
        if (++s_HashFrame == 0) s_HashFrame = 1;
        // Foliage wind: one clock value per frame, so the depth pre-pass, the shadow passes and the main pass bend a
        // vertex identically (a mismatch fails the main pass's LEQUAL against the pre-pass depth).
        static const auto t0 = std::chrono::steady_clock::now();
        s_WindTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
    static std::uint32_t HashFrame() { return s_HashFrame; }
    static float WindTime() { return s_WindTime; }
    static glm::vec2 WindDirection() { return s_WindDir; }
    static void SetWindDirection(glm::vec2 d) { if (glm::dot(d, d) > 1e-6f) s_WindDir = glm::normalize(d); }

    // ShaderVariantKeyForCached's per-frame slot, for the one shader this material links.
    bool CachedVariantKey(const void* shader, std::uint32_t& key) const {
        if (m_VariantCache.Frame != s_HashFrame || m_VariantOwner != shader) return false;
        key = (std::uint32_t)m_VariantCache.Value;
        return true;
    }
    void StoreVariantKey(const void* shader, std::uint32_t key) const {
        m_VariantCache.Value = key;
        m_VariantCache.Frame = s_HashFrame;
        m_VariantOwner = shader;
    }

    // Hash() folded with every ExtraProp (BindMaterialDataDriven's redundant-bind key, #99), cached per frame
    // like Hash(): hashing each custom property's name and values ran per mesh per pass.
    std::uint64_t HashWithExtraProps() const {
        if (m_ExtraHashCache.Frame == s_HashFrame) return m_ExtraHashCache.Value;
        size_t hash = (size_t)Hash();
        auto mix = [&hash](size_t v) { hash ^= v + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2); };
        for (const auto& [name, p] : ExtraProps) {
            mix(std::hash<std::string>{}(name));
            mix(std::hash<float>{}(p.F));
            for (int c = 0; c < 4; ++c) mix(std::hash<float>{}(p.V[c]));
            mix((size_t)p.B); mix((size_t)p.I);
            mix(std::hash<const void*>{}(p.Tex.get()));
        }
        m_ExtraHashCache.Value = hash;
        m_ExtraHashCache.Frame = s_HashFrame;
        return hash;
    }

    std::uint64_t ComputeHash() const {
        std::uint32_t words[80];
        std::size_t n = 0;
        auto put = [&words, &n](const void* p, std::size_t bytes) {
            std::memcpy(words + n, p, bytes);
            n += bytes / 4;
        };
        const float scalars[] = {
            BaseColor.x, BaseColor.y, BaseColor.z, Metallic, Roughness,
            EmissiveColor.x * EmissiveStrength, EmissiveColor.y * EmissiveStrength,
            EmissiveColor.z * EmissiveStrength, TriplanarScale,
            ClearCoat, ClearCoatRoughness, Anisotropy, AnisotropyRotation,
            Sheen.x, Sheen.y, Sheen.z, SheenRoughness,
            SubsurfaceColor.x, SubsurfaceColor.y, SubsurfaceColor.z, Thickness,
            TransmissionStrength, IOR, AlphaCutoff,
            UVTiling.x, UVTiling.y, UVOffset.x, UVOffset.y, NormalStrength, ParallaxScale,
            DetailTiling.x, DetailTiling.y, GrungeStrength, GrungeScale,
            WindStrength, WindFrequency, WindFlutter, WindHeight, Translucency,
        };
        put(scalars, sizeof(scalars));
        const std::uint32_t flags = (Triplanar ? 1u : 0u)
                                  | (SubsurfaceEnabled ? 2u : 0u)
                                  | (ReflectionProbes ? 4u : 0u)
                                  | (AlphaClip ? 8u : 0u)
                                  | (NormalFlipY ? 16u : 0u)
                                  | (DoubleSided ? 32u : 0u)
                                  | (UseVertexColor ? 64u : 0u)
                                  | (SpecularHighlights ? 0u : 128u)
                                  | (GlossyReflections ? 0u : 256u)
                                  | (ReceiveDecals ? 0u : 512u)
                                  | (Foliage ? 1024u : 0u);
        put(&flags, sizeof(flags));
        const Texture* const texs[] = {
            AlbedoMap.get(), NormalMap.get(), MetallicRoughnessMap.get(), MetallicMap.get(),
            RoughnessMap.get(), AOMap.get(), EmissiveMap.get(),
            ClearCoatMap.get(), ThicknessMap.get(),
            HeightMap.get(), DetailAlbedoMap.get(), DetailNormalMap.get(), GrungeMap.get(),
        };
        static_assert(sizeof(scalars) + sizeof(flags) + sizeof(texs) <= sizeof(words), "Material::Hash words");
        put(texs, sizeof(texs));
        std::uint64_t lane[4] = {1469598103934665603ull, 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull};
        std::size_t i = 0;
        for (; i + 4 <= n; i += 4)
            for (int k = 0; k < 4; ++k) {
                lane[k] = (lane[k] ^ words[i + k]) * 1099511628211ull;
                lane[k] ^= lane[k] >> 32;
            }
        for (; i < n; ++i) { lane[0] = (lane[0] ^ words[i]) * 1099511628211ull; lane[0] ^= lane[0] >> 32; }
        std::uint64_t h = lane[0];
        for (int k = 1; k < 4; ++k) { h = (h ^ lane[k]) * 1099511628211ull; h ^= h >> 29; }
        if (Substrate.Enabled()) { h = (h ^ Substrate.Hash()) * 1099511628211ull; h ^= h >> 29; }
        return h;
    }

private:
    struct HashCache {
        std::uint64_t Value = 0;
        std::uint32_t Frame = 0; // 0 = none (s_HashFrame is never 0)
        HashCache() = default;
        HashCache(const HashCache&) {}
        HashCache& operator=(const HashCache&) { Frame = 0; return *this; }
    };
    mutable HashCache m_HashCache;
    mutable HashCache m_ExtraHashCache;
    mutable HashCache m_VariantCache; // a copy drops it (HashCache), so the owner below needs no reset
    mutable const void* m_VariantOwner = nullptr;
    inline static std::uint32_t s_HashFrame = 1;
    inline static float s_WindTime = 0.0f;
    inline static glm::vec2 s_WindDir{0.8f, 0.6f}; // world XZ, unit length
};
