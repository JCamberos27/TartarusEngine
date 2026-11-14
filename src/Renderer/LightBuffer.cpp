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

// The packing the three Add* share: Params = (x, y, excluded-layer bits, volumetric scale clamped to >= 0).
LightBuffer::GpuLight LightBuffer::Pack(const glm::vec4& positionType, const glm::vec4& colorRange, const glm::vec4& dirCutoff,
                                        float paramX, float paramY, std::uint32_t excludedLayers, float volumetric) {
    GpuLight l{};
    l.PositionType = positionType;
    l.ColorRange   = colorRange;
    l.DirCutoff    = dirCutoff;
    l.Params       = glm::vec4(paramX, paramY, LayerBits(excludedLayers), volumetric < 0.0f ? 0.0f : volumetric);
    l.BoxX = glm::vec4(1, 0, 0, 0);
    l.BoxY = glm::vec4(0, 1, 0, 0);
    l.BoxZ = glm::vec4(0, 0, 1, 0);
    l.BoxHalf = glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
    return l;
}

void LightBuffer::AddDirectional(const glm::vec3& dirWorld, const glm::vec3& colorLinear, float intensity,
                                 bool sampleSunShadow, std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    // Params.y: cascade map, as spot/point slots
    const GpuLight l = Pack(glm::vec4(0.0f, 0.0f, 0.0f, (float)Type::Directional), glm::vec4(colorLinear * intensity, 0.0f),
                            glm::vec4(d, -1.0f), -1.0f, sampleSunShadow ? 0.0f : -1.0f, excludedLayers, volumetric);
    // Insert right after the existing directional run, not at the end - keeps every directional
    // light packed at the front of the array so the shader can loop just uDirectionalCount
    // entries instead of scanning the whole buffer (#188).
    m_Lights.insert(m_Lights.begin() + m_DirectionalCount, l);
    ++m_DirectionalCount;
}

void LightBuffer::AddPoint(const glm::vec3& posWorld, const glm::vec3& colorLinear, float intensity, float range,
                          int shadowSlot, std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    // Params.y = point cube-shadow slot, -1 = none (#119)
    m_Lights.push_back(Pack(glm::vec4(posWorld, (float)Type::Point), glm::vec4(colorLinear * intensity, range),
                            glm::vec4(0.0f, 0.0f, -1.0f, -1.0f), -1.0f, (float)shadowSlot, excludedLayers, volumetric));
}

void LightBuffer::AddSpot(const glm::vec3& posWorld, const glm::vec3& dirWorld, const glm::vec3& colorLinear,
                          float intensity, float range, float cosOuter, float cosInner, int shadowSlot,
                          std::uint32_t excludedLayers, float volumetric) {
    if ((int)m_Lights.size() >= kMaxLights) { m_Overflowed = true; return; }
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    // Params.y = spot shadow slot, -1 = none (#119)
    m_Lights.push_back(Pack(glm::vec4(posWorld, (float)Type::Spot), glm::vec4(colorLinear * intensity, range),
                            glm::vec4(d, cosOuter), cosInner, (float)shadowSlot, excludedLayers, volumetric));
}

void LightBuffer::BoundLast(const glm::mat3& axes, const glm::vec3& centre, const glm::vec3& half, float fade) {
    if ((int)m_Lights.size() <= m_DirectionalCount) return;
    GpuLight& l = m_Lights.back();
    glm::vec4* rows[3] = {&l.BoxX, &l.BoxY, &l.BoxZ};
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 a = glm::length(axes[i]) > 1e-8f ? glm::normalize(axes[i]) : glm::vec3(i == 0, i == 1, i == 2);
        *rows[i] = glm::vec4(a, -glm::dot(a, centre));
    }
    l.BoxHalf = glm::vec4(glm::max(half, glm::vec3(1e-3f)), fade > 1e-3f ? fade : 1e-3f);
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
