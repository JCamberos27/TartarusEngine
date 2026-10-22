#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>

// A decal definition (.decal JSON, decalVersion 1) — what a DecalComponent instance points at.
// Textures are project-relative paths; every map but albedo is optional. Instance-level
// overrides (size, tint, per-channel opacity, weathering) live on DecalComponent.
//
// {
//   "decalVersion": 1,
//   "textures": { "albedo": "...png", "normal": "", "surface": "", "aux": "" },
//   "lumaMask": true,              // opacity from albedo luminance (white-on-black alpha packs)
//   "invertMask": false,           // dark marks on a light background (peeling paint maps)
//   "tier": 2048,                  // 512 | 1024 | 2048 | 4096 slot size in the decal arrays
//   "atlas": { "cols": 1, "rows": 1 },
//   "defaultSize": [1, 1, 0.25],   // metres: width, height, projection depth
//   "channels": { "albedo": 1, "normal": 0, "roughness": 0, "metal": 0, "ao": 0, "emissive": 0 },
//   "baseColor": [1, 1, 1], "roughness": 0.6, "metal": 0, "emissive": [0, 0, 0],
//   "angleFade": [0.25, 0.05],     // cos start, cos end
//   "edgeFade": 0.08, "heightBlend": 0, "heightContrast": 4,
//   "paint": { "enabled": false, "roughness": 0.35, "metal": 0, "relief": 0.3, "uv": false },
//   "weathering": { "fade": 0, "bleach": 0, "peel": 0, "buff": 0 },
//   "category": "Graffiti/Letters", "tags": ["graffiti", "letter"]
// }
struct DecalAsset {
    std::string Path; // absolute path to the .decal file
    std::string Name; // filename stem
    bool Missing = false;

    std::string AlbedoPath, NormalPath, SurfacePath, AuxPath; // project-relative
    bool LumaMask = false;
    bool InvertMask = false;
    int Tier = 2048;
    int AtlasCols = 1, AtlasRows = 1;
    glm::vec3 DefaultSize{1.0f, 1.0f, 0.25f};

    // Channel opacities (0 disables the channel).
    float AlbedoOp = 1.0f, NormalOp = 0.0f, RoughnessOp = 0.0f, MetalOp = 0.0f, AOOp = 0.0f,
          EmissiveOp = 0.0f;
    glm::vec3 BaseColor{1.0f};
    float Roughness = 0.6f, Metal = 0.0f;
    glm::vec3 Emissive{0.0f};

    float AngleFadeStart = 0.25f, AngleFadeEnd = 0.05f; // cosines
    float EdgeFade = 0.08f;
    float HeightBlend = 0.0f, HeightContrast = 4.0f;

    bool Paint = false, UVPaint = false;
    float PaintRoughness = 0.35f, PaintMetal = 0.0f, Relief = 0.3f;
    float Fade = 0.0f, Bleach = 0.0f, Peel = 0.0f, Buff = 0.0f;

    std::string Category;
    std::string Tags; // comma separated

    static std::shared_ptr<DecalAsset> Load(const std::string& path);
    bool Save() const;
};
