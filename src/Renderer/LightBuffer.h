#pragma once
#include <vector>
#include <glm/glm.hpp>

// GPU-side scene light list for the lighting overhaul.
//
// Replaces the old per-frame string-built uniform arrays (uPointLightPos[i], uLightDir, ...)
// with one std430 SSBO bound at binding = 0:
//
//   layout(std430, binding = 0) readonly buffer LightBuffer {
//       uint  uLightCount;
//       uint  uDirectionalCount; // + 8 bytes implicit pad
//       Light uLights[];         // starts at byte 16
//   };
//
// Every light kind (directional / point / spot) lives in the same array; the shader branches on
// the type packed into PositionType.w. This is the shape the clustered-forward cull pass will
// consume later, so nothing about the storage changes when that lands.
//
// Directional lights are kept packed at the FRONT of the array (AddDirectional inserts rather
// than appends), and uDirectionalCount says how many - so the directional-light shading pass can
// loop `for (i < uDirectionalCount)` instead of scanning every light in the buffer and skipping
// non-directional ones by type (#188).
class LightBuffer {
public:
    // Forward path cap. Raised once clustered culling is in; 256 * 64B = 16 KB, trivial.
    static constexpr int kMaxLights = 256;

    enum class Type : int { Directional = 0, Point = 1, Spot = 2 };

    LightBuffer() = default;
    ~LightBuffer();
    LightBuffer(const LightBuffer&) = delete;
    LightBuffer& operator=(const LightBuffer&) = delete;

    void Clear() { m_Lights.clear(); m_DirectionalCount = 0; m_Overflowed = false; }

    // `colorLinear` is the light's colour, `intensity` its scalar strength — stored pre-multiplied,
    // matching what the old shader received. Drops lights past kMaxLights rather than crashing or
    // corrupting the buffer — but that drop is no longer silent: it flips m_Overflowed so the
    // caller can warn (#204).
    void AddDirectional(const glm::vec3& dirWorld, const glm::vec3& colorLinear, float intensity);
    void AddPoint(const glm::vec3& posWorld, const glm::vec3& colorLinear, float intensity, float range,
                  int shadowSlot = -1);
    void AddSpot(const glm::vec3& posWorld, const glm::vec3& dirWorld, const glm::vec3& colorLinear,
                 float intensity, float range, float cosOuter, float cosInner, int shadowSlot = -1);

    // (Lazily creates the SSBO on first call.) Uploads the current list.
    void Upload();
    // glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, ...). Bind after Upload(), before draws.
    void Bind(unsigned int binding = 0) const;

    int Count() const { return (int)m_Lights.size(); }

    // True if an Add* call this frame (since the last Clear()) was refused because the buffer was
    // already at kMaxLights (#204). Also settable by a caller that stops enumerating scene lights
    // itself once the buffer reports full, so entities it never got to are still counted as a drop.
    bool Overflowed() const { return m_Overflowed; }
    void MarkOverflowed() { m_Overflowed = true; }

private:
    struct GpuLight {
        glm::vec4 PositionType; // xyz = world pos (point/spot); w = Type
        glm::vec4 ColorRange;   // rgb = colour * intensity; a = range (metres; unused for directional)
        glm::vec4 DirCutoff;    // xyz = normalized aim dir (spot/directional); w = spot outer-cone cos (-1 = none)
        glm::vec4 Params;       // x = spot inner-cone cos; y = shadow slot (-1 none, set later); zw spare
    };

    std::vector<GpuLight> m_Lights;
    int m_DirectionalCount = 0; // how many of m_Lights' front entries are directional (#188)
    bool m_Overflowed = false; // set when an Add* was dropped for being past kMaxLights (#204)
    unsigned int m_Ssbo = 0;

    void EnsureCreated();
};
