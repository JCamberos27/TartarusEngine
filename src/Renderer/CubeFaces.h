#pragma once

// Face-local bases for GL_TEXTURE_CUBE_MAP layers 0..5 (+X, -X, +Y, -Y, +Z, -Z): the direction a face looks along, and
// the world directions its u (right) and v (up) run. v is flipped relative to world up on the four side faces,
// matching OpenGL's (left-handed, +Y-down) cube-face convention - get this wrong and reflections come out mirrored.
// One table for every cube the engine renders or bakes (IblProbe, Cubemap, ReflectionProbeCapture).
namespace CubeFaces {
struct FaceBasis { float forward[3]; float right[3]; float up[3]; };
const FaceBasis kFaces[6] = {
    {{ 1,  0,  0}, { 0,  0, -1}, { 0, -1,  0}}, // +X
    {{-1,  0,  0}, { 0,  0,  1}, { 0, -1,  0}}, // -X
    {{ 0,  1,  0}, { 1,  0,  0}, { 0,  0,  1}}, // +Y
    {{ 0, -1,  0}, { 1,  0,  0}, { 0,  0, -1}}, // -Y
    {{ 0,  0,  1}, { 1,  0,  0}, { 0, -1,  0}}, // +Z
    {{ 0,  0, -1}, {-1,  0,  0}, { 0, -1,  0}}, // -Z
};
} // namespace CubeFaces
