#pragma once
#include <glm/glm.hpp>
#include <vector>

class Shader;
class World;

// PR14 (#333): manages the set of placed reflection probes for a frame.
//
// ReflectionProbeArray is NOT a GL resource owner — probes share the global sky IBL textures
// (irradiance cube + prefiltered specular + BRDF LUT) baked by IblProbe. The array's job is
// to collect probe box data from the scene, select the 2 nearest probes for a given draw-call
// centroid, and upload their positions/half-extents/blend weights to the shader.
//
// Parallax box projection in the shader corrects the sky specular reflection vector to the
// probe's box boundary, giving room-appropriate reflections without per-probe scene captures.
// Zero probes → Bind() sets uProbeCount=0 → shader falls through to the unmodified sky IBL
// path, producing a bit-identical frame to the pre-PR14 state.
class ReflectionProbeArray {
public:
    static constexpr int kMaxDraw = 2; // probes uploaded per draw call

    // Rebuilds the CPU-side probe list from all entities carrying a ReflectionProbeComponent.
    // Call once per frame, before any drawScene.
    void Update(const World& world);

    // Sets uProbeCount + per-probe uniforms on `shader` for the draw centroid `viewCenter`.
    // Selects up to kMaxDraw nearest probes by distance, weighted by Importance.
    // No-op (uProbeCount = 0) when no probes exist or the shader has _REFLECTION_PROBES off.
    void Bind(Shader& shader, const glm::vec3& viewCenter) const;

    int Count() const { return (int)m_Probes.size(); }

private:
    struct ProbeData {
        glm::vec3 Center;
        glm::vec3 HalfSize;
        float     Importance;
    };
    std::vector<ProbeData> m_Probes;
};
