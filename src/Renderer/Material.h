#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "ShaderPropType.h"

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
    std::shared_ptr<Texture> HeightMap;   // parallax occlusion mapping (white = high)
    float ParallaxScale  = 0.02f;
    std::shared_ptr<Texture> DetailAlbedoMap; // x2 detail: 50% grey leaves the colour unchanged
    std::shared_ptr<Texture> DetailNormalMap;
    glm::vec2 DetailTiling{4.0f};         // detail maps' UV tiling (on the mesh UVs)

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
    // dedupe, not just shared pointers. FNV-1a over the raw bytes; a 64-bit collision that
    // would swap two genuinely different materials for a frame is not a practical concern.
    std::uint64_t Hash() const {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p, std::size_t n) {
            const unsigned char* b = static_cast<const unsigned char*>(p);
            for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
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
            DetailTiling.x, DetailTiling.y,
        };
        mix(scalars, sizeof(scalars));
        const std::uint32_t flags = (Triplanar ? 1u : 0u)
                                  | (SubsurfaceEnabled ? 2u : 0u)
                                  | (ReflectionProbes ? 4u : 0u)
                                  | (AlphaClip ? 8u : 0u)
                                  | (NormalFlipY ? 16u : 0u)
                                  | (DoubleSided ? 32u : 0u)
                                  | (UseVertexColor ? 64u : 0u);
        mix(&flags, sizeof(flags));
        const Texture* const texs[] = {
            AlbedoMap.get(), NormalMap.get(), MetallicRoughnessMap.get(), MetallicMap.get(),
            RoughnessMap.get(), AOMap.get(), EmissiveMap.get(),
            ClearCoatMap.get(), ThicknessMap.get(),
            HeightMap.get(), DetailAlbedoMap.get(), DetailNormalMap.get(),
        };
        mix(texs, sizeof(texs));
        return h;
    }
};
