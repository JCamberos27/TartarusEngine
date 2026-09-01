#pragma once
#include <vector>
#include <glm/glm.hpp>

// GPU-side scene light list for the lighting overhaul.
//
// Replaces the old per-frame string-built uniform arrays (uPointLightPos[i], uLightDir, ...)
// with one std430 SSBO bound at binding = 0:
//
//   layout(std430, binding = 0) readonly buffer LightBuffer {
//       uint  uLightCount;   // + 12 bytes implicit pad
//       Light uLights[];     // starts at byte 16
//   };
//
// Every light kind (directional / point / spot) lives in the same array; the shader branches on
// the type packed into PositionType.w. This is the shape the clustered-forward cull pass will
// consume later, so nothing about the storage changes when that lands.
class LightBuffer {
public:
    // Forward path cap. Raised once clustered culling is in; 256 * 64B = 16 KB, trivial.
    static constexpr int kMaxLights = 256;

    enum class Type : int { Directional = 0, Point = 1, Spot = 2 };

    LightBuffer() = default;
    ~LightBuffer();
    LightBuffer(const LightBuffer&) = delete;
    LightBuffer& operator=(const LightBuffer&) = delete;

    void Clear() { m_Lights.clear(); }

    // `colorLinear` is the light's colour, `intensity` its scalar strength — stored pre-multiplied,
    // matching what the old shader received. Silently drops lights past kMaxLights.
    void AddDirectional(const glm::vec3& dirWorld, const glm::vec3& colorLinear, float intensity);
    void AddPoint(const glm::vec3& posWorld, const glm::vec3& colorLinear, float intensity, float range);
    void AddSpot(const glm::vec3& posWorld, const glm::vec3& dirWorld, const glm::vec3& colorLinear,
                 float intensity, float range, float cosOuter, float cosInner);

    // (Lazily creates the SSBO on first call.) Uploads the current list.
    void Upload();
    // glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, ...). Bind after Upload(), before draws.
    void Bind(unsigned int binding = 0) const;

    int Count() const { return (int)m_Lights.size(); }

private:
    struct GpuLight {
        glm::vec4 PositionType; // xyz = world pos (point/spot); w = Type
        glm::vec4 ColorRange;   // rgb = colour * intensity; a = range (metres; unused for directional)
        glm::vec4 DirCutoff;    // xyz = normalized aim dir (spot/directional); w = spot outer-cone cos (-1 = none)
        glm::vec4 Params;       // x = spot inner-cone cos; y = shadow slot (-1 none, set later); zw spare
    };

    std::vector<GpuLight> m_Lights;
    unsigned int m_Ssbo = 0;

    void EnsureCreated();
};
