#pragma once
// Shared by ModelDraw.cpp and ModelSkinning.cpp (not a public header).

#include "Shader.h"

namespace ModelInternal {

// #194: per-material uniform locations for one Draw() call, resolved ONCE before the per-mesh
// loop (below) instead of re-hashing all these literal names for every mesh of every model.
struct MaterialLocs {
    int baseColor, metallic, roughness, emissiveColor, triplanar, triplanarScale, triplanarBreakup;
    int hasAlbedo, albedoMap;
    int hasNormal, normalMap;
    int hasMetallicRoughness, metallicRoughnessMap;
    int hasMetallic, metallicMap;
    int hasRoughness, roughnessMap;
    int hasAO, aoMap;
    int hasEmissive, emissiveMap;
    int alphaClip, alphaCutoff; // #101
    int hasHeight, heightMap, hasDetailAlbedo, detailAlbedoMap, hasDetailNormal, detailNormalMap; // #102
    int hasGrunge, grungeMap; // docs/DECALS.md
    // UploadBoneMatrices and DrawDepthOnly (the shadow / depth pre-pass programs).
    int useSkinning, skinBase, uvTiling, uvOffset, depthAlbedo, alphaTest;
    // SetSurfaceOptions / SetFoliageUniforms and the per-draw uniforms of the multi-program draw: resolved here too, so
    // a material bind does no name lookups at all (a forest binds a few thousand a frame).
    int normalStrength, normalFlipY, doubleSided, useVertexColor, noSpecularHighlights, noGlossyReflections;
    int parallaxScale, detailTiling, grungeStrength, grungeScale, receiveDecals, noWetness, foliageSurface;
    int materialSubstrate, foliage, windVertexMask, translucency, windStrength, windFrequency, windFlutter, windHeight;
    int opacity, hideVertBase, clothInterior, model, normalMatrix;
};

// The program's MaterialLocs, resolved once and kept on the Shader (Shader::LocationBlock).
const MaterialLocs& CachedMaterialLocs(Shader& shader);

} // namespace ModelInternal
