#pragma once
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
};
