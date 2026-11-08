#include "LightBuffer.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include "gl.h"

#include <cstdint>

namespace {
constexpr int kHeaderBytes = 16; // uint count + 12 pad, so the struct array starts 16-aligned (std430)
}

LightBuffer::~LightBuffer() {
    if (m_Ssbo) glDeleteBuffers(1, &m_Ssbo);
}

void LightBuffer::EnsureCreated() {
    if (m_Ssbo) return;
    glCreateBuffers(1, &m_Ssbo);
    glNamedBufferStorage(m_Ssbo, kHeaderBytes + kMaxLights * (GLsizeiptr)sizeof(GpuLight),
                         nullptr, GL_DYNAMIC_STORAGE_BIT);
}

namespace {
// The excluded-layer bits ride in a float slot; the shader reads them back with floatBitsToUint.
float LayerBits(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
} // namespace

void LightBuffer::AddDirectional(const glm::vec3& dirWorld, const glm::vec3& colorLinear, float intensity,
                                 bool sampleSunShadow, std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    GpuLight l{};
    l.PositionType = glm::vec4(0.0f, 0.0f, 0.0f, (float)Type::Directional);
    l.ColorRange   = glm::vec4(colorLinear * intensity, 0.0f);
    l.DirCutoff    = glm::vec4(d, -1.0f);
    l.Params       = glm::vec4(-1.0f, sampleSunShadow ? 0.0f : -1.0f, 0.0f, 0.0f); // y: cascade map, as spot/point slots
    l.Params.z     = LayerBits(excludedLayers);
    l.Params.w     = volumetric < 0.0f ? 0.0f : volumetric;
    // Insert right after the existing directional run, not at the end - keeps every directional
    // light packed at the front of the array so the shader can loop just uDirectionalCount
    // entries instead of scanning the whole buffer (#188).
    m_Lights.insert(m_Lights.begin() + m_DirectionalCount, l);
    ++m_DirectionalCount;
}

void LightBuffer::AddPoint(const glm::vec3& posWorld, const glm::vec3& colorLinear, float intensity, float range,
                          int shadowSlot, std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    GpuLight l{};
    l.PositionType = glm::vec4(posWorld, (float)Type::Point);
    l.ColorRange   = glm::vec4(colorLinear * intensity, range);
    l.DirCutoff    = glm::vec4(0.0f, 0.0f, -1.0f, -1.0f);
    l.Params       = glm::vec4(-1.0f, (float)shadowSlot, 0.0f, 0.0f); // .y = point cube-shadow slot, -1 = none (#119)
    l.Params.z     = LayerBits(excludedLayers);
    l.Params.w     = volumetric < 0.0f ? 0.0f : volumetric;
    m_Lights.push_back(l);
}

void LightBuffer::AddSpot(const glm::vec3& posWorld, const glm::vec3& dirWorld, const glm::vec3& colorLinear,
                          float intensity, float range, float cosOuter, float cosInner, int shadowSlot,
                          std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    GpuLight l{};
    l.PositionType = glm::vec4(posWorld, (float)Type::Spot);
    l.ColorRange   = glm::vec4(colorLinear * intensity, range);
    l.DirCutoff    = glm::vec4(d, cosOuter);
    l.Params       = glm::vec4(cosInner, (float)shadowSlot, 0.0f, 0.0f); // .y = spot shadow slot, -1 = none (#119)
    l.Params.z     = LayerBits(excludedLayers);
    l.Params.w     = volumetric < 0.0f ? 0.0f : volumetric;
    m_Lights.push_back(l);
}

void LightBuffer::SortLocalNearestFirst(const glm::vec3& eye) {
    auto gap = [&](const GpuLight& l) {
        const float d = glm::length(glm::vec3(l.PositionType) - eye) - l.ColorRange.a;
        return d > 0.0f ? d : 0.0f;
    };
    const size_t first = (size_t)m_DirectionalCount;
    if (m_Lights.size() < first + 2) return;
    // Each light's key once, then a stable sort of the order (ties keep their place, as the insertion sort did).
    static std::vector<std::pair<float, std::uint32_t>> keyed;
    static std::vector<GpuLight> sorted;
    keyed.clear();
    for (size_t i = first; i < m_Lights.size(); ++i) keyed.emplace_back(gap(m_Lights[i]), (std::uint32_t)i);
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    sorted.clear();
    for (const auto& k : keyed) sorted.push_back(m_Lights[k.second]);
    std::copy(sorted.begin(), sorted.end(), m_Lights.begin() + (std::ptrdiff_t)first);
}

void LightBuffer::Upload() {
    EnsureCreated();
    const uint32_t count = (uint32_t)m_Lights.size();
    const uint32_t header[2] = {count, (uint32_t)m_DirectionalCount}; // light count, then directional count
    glNamedBufferSubData(m_Ssbo, 0, sizeof header, header);
    if (count > 0) {
        glNamedBufferSubData(m_Ssbo, kHeaderBytes,
                             (GLsizeiptr)(count * sizeof(GpuLight)), m_Lights.data());
    }
}

void LightBuffer::Bind(unsigned int binding) const {
    if (m_Ssbo) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, m_Ssbo);
}
