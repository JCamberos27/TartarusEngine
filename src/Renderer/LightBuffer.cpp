#include "LightBuffer.h"
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

void LightBuffer::AddDirectional(const glm::vec3& dirWorld, const glm::vec3& colorLinear, float intensity) {
    if ((int)m_Lights.size() >= kMaxLights) return;
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    GpuLight l{};
    l.PositionType = glm::vec4(0.0f, 0.0f, 0.0f, (float)Type::Directional);
    l.ColorRange   = glm::vec4(colorLinear * intensity, 0.0f);
    l.DirCutoff    = glm::vec4(d, -1.0f);
    l.Params       = glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);
    m_Lights.push_back(l);
}

void LightBuffer::AddPoint(const glm::vec3& posWorld, const glm::vec3& colorLinear, float intensity, float range,
                          int shadowSlot) {
    if ((int)m_Lights.size() >= kMaxLights) return;
    GpuLight l{};
    l.PositionType = glm::vec4(posWorld, (float)Type::Point);
    l.ColorRange   = glm::vec4(colorLinear * intensity, range);
    l.DirCutoff    = glm::vec4(0.0f, 0.0f, -1.0f, -1.0f);
    l.Params       = glm::vec4(-1.0f, (float)shadowSlot, 0.0f, 0.0f); // .y = point cube-shadow slot, -1 = none (#119)
    m_Lights.push_back(l);
}

void LightBuffer::AddSpot(const glm::vec3& posWorld, const glm::vec3& dirWorld, const glm::vec3& colorLinear,
                          float intensity, float range, float cosOuter, float cosInner, int shadowSlot) {
    if ((int)m_Lights.size() >= kMaxLights) return;
    glm::vec3 d = glm::length(dirWorld) > 1e-8f ? glm::normalize(dirWorld) : glm::vec3(0, -1, 0);
    GpuLight l{};
    l.PositionType = glm::vec4(posWorld, (float)Type::Spot);
    l.ColorRange   = glm::vec4(colorLinear * intensity, range);
    l.DirCutoff    = glm::vec4(d, cosOuter);
    l.Params       = glm::vec4(cosInner, (float)shadowSlot, 0.0f, 0.0f); // .y = spot shadow slot, -1 = none (#119)
    m_Lights.push_back(l);
}

void LightBuffer::Upload() {
    EnsureCreated();
    uint32_t count = (uint32_t)m_Lights.size();
    glNamedBufferSubData(m_Ssbo, 0, sizeof(uint32_t), &count);
    if (count > 0) {
        glNamedBufferSubData(m_Ssbo, kHeaderBytes,
                             (GLsizeiptr)(count * sizeof(GpuLight)), m_Lights.data());
    }
}

void LightBuffer::Bind(unsigned int binding) const {
    if (m_Ssbo) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, m_Ssbo);
}
