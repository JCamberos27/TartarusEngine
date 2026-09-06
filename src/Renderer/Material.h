#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <glm/glm.hpp>

class Texture;

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
        };
        mix(scalars, sizeof(scalars));
        const std::uint32_t flags = Triplanar ? 1u : 0u;
        mix(&flags, sizeof(flags));
        const Texture* const texs[] = {
            AlbedoMap.get(), NormalMap.get(), MetallicRoughnessMap.get(), MetallicMap.get(),
            RoughnessMap.get(), AOMap.get(), EmissiveMap.get(),
        };
        mix(texs, sizeof(texs));
        return h;
    }
};
