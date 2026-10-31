#pragma once

#include <array>
#include <glm/glm.hpp>

// Second-order (L2, nine coefficient) spherical harmonics for the room probes' diffuse (ReflectionProbeCapture).
// The model shader used to fetch the captured cube's widest GGX mip along N for every pixel inside a probe box; the
// same function, projected once at capture time, evaluates from nine uniforms instead (ModelFragment.glsl ProbeSH).
namespace ProbeSH {

using Coeffs = std::array<glm::vec3, 9>;

// The nine real SH basis functions at unit direction `d` (the order ModelFragment.glsl's EvalProbeSH uses).
std::array<float, 9> Basis(const glm::vec3& d);

// A GL cube map face texel's direction (face 0..5 = +X -X +Y -Y +Z -Z; row 0 = t 0, as glGetTextureImage returns).
glm::vec3 TexelDirection(int face, int x, int y, int size);
// Its solid angle on the unit sphere (the six faces sum to 4 pi).
float TexelSolidAngle(int x, int y, int size);

// Projects six `size` x `size` RGBA float faces, back to back, onto L2: the reconstruction sum(c_i * Y_i(d)) is the
// cube's own value along d, low-passed (the widest mip is smooth, so little is lost).
Coeffs Project(const float* rgbaFaces, int size);

glm::vec3 Evaluate(const Coeffs& c, const glm::vec3& d);

// Project Settings-free switch: TARTARUS_NO_PROBE_SH=1 makes the shader fetch the cube again (the old path).
bool Enabled();
}
