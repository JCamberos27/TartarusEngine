#pragma once

#include <string>
#include <vector>

// `--bake-impostors <scene.json>... --out <dir> [--match <text>]`: octahedral impostor atlases for the scene's LOD
// groups whose LOD 0 draws a model whose path contains `match` (default "Cedrus_Tree_mesh"). One bake per distinct
// LOD 0 (its set of models), from the first group found with it, in the group's own unscaled space.
//
// Each bake is a kFrames x kFrames grid of orthographic views over the upper hemi-octahedron (ImpostorCommon.glsl),
// every frame `frameSize` px, rendered at twice that and box-filtered by coverage, then the empty texels dilated
// from their covered neighbours (no dark fringes once mipped). Per bake, in <out>:
//   <name>_Impostor_Albedo.png       rgb sRGB albedo, a coverage
//   <name>_Impostor_NormalDepth.png  rgb object-space normal (n * 0.5 + 0.5), a depth (0 = the sphere's near side)
//   <name>_Impostor_Mask.png         r AO, g roughness, b translucency, a metallic
// and <out>/impostors.json with each bake's models, bounding sphere and frame layout.
// Needs a GL context, so it opens a hidden window. Returns the exit code.
int BakeImpostors(const std::vector<std::string>& scenes, const std::string& outDir, const std::string& match,
                  const char* argv0);
