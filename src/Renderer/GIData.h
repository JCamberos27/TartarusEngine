#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Baked global illumination (docs: the GI Volume component, GIVolumes.h): the CPU side - L1 spherical harmonics, a
// volume's probe grid, the .gi bake file and a process-wide cache of loaded files. No GL here, so all of it is unit
// tested.
//
// A GI Volume is an oriented box (a room) with a grid of probes about Spacing metres apart. Each probe holds, per
// LAYER, the diffuse light arriving at it as L1 SH, already convolved with the cosine lobe: for a surface normal n the
// probe's irradiance is a + dot(b, n) per colour channel (Evaluate), in the same units as the sky irradiance cube
// (IblIrradiance: E / pi, so a uniform environment of radiance L reads L, and the model shader multiplies by albedo).
// A layer is one light circuit (its lamps, baked alone) or the Outside (moon, sun and sky through the windows, always
// on, circuit name ""). At runtime each layer is weighted by its circuit's state and the layers are added up.
namespace GI {

// One colour channel's convolved L1: x = a (DC), yzw = b (the linear band, world axes).
using SH4 = glm::vec4;
// R, G, B.
struct ProbeSH { SH4 c[3]{SH4(0.0f), SH4(0.0f), SH4(0.0f)}; };

// Irradiance (E / pi) for normal n. Can go slightly negative for very directional light (L1 ringing): clamped at 0.
glm::vec3 Evaluate(const ProbeSH& sh, const glm::vec3& n);

// Accumulates radiance samples into L1 SH. Add() each sample with its solid angle (sr); Finish() returns the
// cosine-convolved, 1/pi-scaled coefficients Evaluate() expects.
class Projector {
public:
    void Add(const glm::vec3& dir, const glm::vec3& radiance, float solidAngle);
    ProbeSH Finish() const;
    float TotalSolidAngle() const { return m_Weight; }
private:
    glm::vec4 m_L[3]{glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)}; // raw L_00, L_1x, L_1y, L_1z per channel
    float m_Weight = 0.0f;
};

// The solid angle of texel (x, y) of a size x size cube face (any face: they are symmetric).
float CubeTexelSolidAngle(int x, int y, int size);
// The world direction through the centre of texel (x, y) of cube face `face` (GL order +X -X +Y -Y +Z -Z, the GL
// cube-map face convention: row 0 is the top of the face as GL samples it).
glm::vec3 CubeTexelDir(int face, int x, int y, int size);

// An oriented box from a world transform and a full Size (local), like the light bounds boxes.
struct Box {
    glm::vec3 Center{0.0f};
    glm::vec3 Axis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // unit
    glm::vec3 Half{0.5f};
    static Box FromTransform(const glm::mat4& world, const glm::vec3& size);
    glm::vec3 ToLocal(const glm::vec3& p) const;
    glm::vec3 ToWorld(const glm::vec3& local) const;
    // 1 well inside, falling linearly to 0 over `fade` metres at the faces (LightBoxFade in the shader); fade <= 0 is a
    // hard edge.
    float Fade(const glm::vec3& p, float fade) const;
};

// Probes along each axis for a box of half-extents `half` at `spacing`: one at each face and about `spacing` apart,
// at least 2.
glm::ivec3 GridDims(const glm::vec3& half, float spacing);
// Probe (i, j, k)'s position in box-local space.
glm::vec3 ProbeLocal(const glm::ivec3& ijk, const glm::ivec3& dims, const glm::vec3& half);
inline int ProbeIndex(const glm::ivec3& ijk, const glm::ivec3& dims) { return ijk.x + dims.x * (ijk.y + dims.y * ijk.z); }

// One baked layer of one volume.
struct Layer {
    std::string Circuit; // "" = Outside
    std::vector<ProbeSH> Probes; // dims.x * dims.y * dims.z, ProbeIndex order
};

// One volume's bake: the box and grid it was baked for, and its layers.
struct VolumeBake {
    std::string Name;   // the GI Volume entity's name
    Box Bounds;
    glm::ivec3 Dims{0};
    std::vector<Layer> Layers;
    // Trilinear irradiance of one layer at box-local point q for normal n (the CPU twin of the shader's lookup).
    glm::vec3 Sample(int layer, const glm::vec3& q, const glm::vec3& n) const;
};

struct BakeFile {
    std::vector<VolumeBake> Volumes;
    const VolumeBake* Find(const std::string& name) const;
};

// .gi files: "TGI1", little-endian, coefficients as 16-bit floats (24 bytes a probe a layer).
bool Save(const BakeFile& file, const std::string& path, std::string* err = nullptr);
bool Load(BakeFile& file, const std::string& path, std::string* err = nullptr);
std::vector<std::uint8_t> Serialize(const BakeFile& file);
bool Deserialize(BakeFile& file, const std::uint8_t* data, std::size_t size, std::string* err = nullptr);

// Loaded bake files by absolute path, reloaded when the file on disk changes (a fresh bake shows without a scene
// reload). Null when missing or unreadable. Main thread only.
std::shared_ptr<const BakeFile> Cached(const std::string& absPath);
void ClearCache();

// Probes inside walls or furniture see mostly backfaces: FillInvalid() replaces each invalid probe's SH in one layer
// (call it per layer) with the average of its valid neighbours (6-connected, spreading outward until every reachable probe is filled), so
// they don't sample as dark blotches. Probes with no valid probe anywhere stay as they are.
void FillInvalid(std::vector<ProbeSH>& probes, const std::vector<bool>& valid, const glm::ivec3& dims);

// Circuit names from a GI Volume's Circuits field: ';' or ',' separated, trimmed, empties dropped.
std::vector<std::string> SplitCircuits(const std::string& s);

} // namespace GI
