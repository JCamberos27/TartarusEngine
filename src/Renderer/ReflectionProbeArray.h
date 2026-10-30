#pragma once
#include <cstdint>
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

    // Sets uProbeCount + per-probe uniforms on `shader` for the draw centroid `viewCenter` (the
    // object's world bounds centre - SceneRenderer calls this per draw, #108).
    // Selects up to kMaxDraw nearest probes by distance, weighted by Importance.
    // No-op (uProbeCount = 0) when no probes exist or the shader has _REFLECTION_PROBES off.
    void Bind(Shader& shader, const glm::vec3& viewCenter) const;
    // Which probes Bind(shader, centre) picks and its leading blend weight to 1/64, in 32 bits: draws with equal keys
    // bind the same probes with blends within 1/128 of each other, so copies of a model can go out as one instanced
    // draw under the first copy's probe uniforms (SceneRenderer) instead of one draw each.
    std::uint32_t SelectionKey(const glm::vec3& centre) const;

    int Count() const { return (int)m_Probes.size(); }

    // Room captures (ReflectionProbeCapture): probe i samples layer `layer` of the captured, prefiltered cube array
    // instead of the sky's specular cube; -1 (what Update() leaves) = the sky. Bind() uploads uProbeLayer[]. The
    // array goes on texture unit 26 (SceneRenderer::ApplyFrameState) whenever it exists.
    struct ProbeVolume { glm::vec3 Center, HalfSize; bool Capture; };
    ProbeVolume Volume(int i) const { return {m_Probes[i].Center, m_Probes[i].HalfSize, m_Probes[i].Capture}; }
    void SetLayer(int i, int layer) { m_Probes[i].Layer = layer; }
    int Layer(int i) const { return m_Probes[i].Layer; }
    void SetCubeArray(unsigned int tex) { m_CubeArray = tex; }
    unsigned int CubeArray() const { return m_CubeArray; }
    // While a capture renders, every probe reads the sky: a probe's own half-written layer is never sampled, and a
    // capture is the same whichever probes were ready when it ran.
    void SetCapturing(bool on) { m_Capturing = on; }

private:
    struct Scored { int idx; float score; };
    int Select(const glm::vec3& centre, Scored (&best)[kMaxDraw], float& total) const; // how many picked
    struct ProbeData {
        glm::vec3 Center;
        glm::vec3 HalfSize;
        float     Importance;
        bool      Capture = false; // ReflectionProbeComponent::CaptureScene
        int       Layer = -1;      // captured cube-array layer, -1 = sky
    };
    std::vector<ProbeData> m_Probes;
    unsigned int m_CubeArray = 0;
    bool m_Capturing = false;
};
