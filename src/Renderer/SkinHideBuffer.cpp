#include "SkinHideBuffer.h"
#include "gl.h"

SkinHideBuffer::SkinHideBuffer(const std::vector<std::uint32_t>& bits) {
    glCreateBuffers(1, &m_Buffer);
    const std::uint32_t none = 0;
    glNamedBufferStorage(m_Buffer, (GLsizeiptr)(bits.empty() ? sizeof(none) : bits.size() * sizeof(std::uint32_t)),
                         bits.empty() ? &none : bits.data(), 0);
}

SkinHideBuffer::~SkinHideBuffer() {
    if (m_Buffer) glDeleteBuffers(1, &m_Buffer);
}

void SkinHideBuffer::Bind(unsigned buffer, unsigned binding) {
    static SkinHideBuffer* empty = nullptr; // deliberately leaked: freed with the context, not at exit
    if (!buffer) {
        if (!empty) empty = new SkinHideBuffer({});
        buffer = empty->Id();
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer);
}

VisibleIndexBuffer::VisibleIndexBuffer(const std::vector<std::vector<std::uint32_t>>& perMesh) {
    std::vector<std::uint32_t> all;
    for (const auto& m : perMesh) {
        m_Ranges.push_back({(std::uint32_t)(all.size() * sizeof(std::uint32_t)), (std::uint32_t)m.size()});
        all.insert(all.end(), m.begin(), m.end());
    }
    m_Triangles = all.size() / 3;
    if (all.empty()) all.push_back(0); // a buffer with no storage can't be created
    glCreateBuffers(1, &m_Buffer);
    glNamedBufferStorage(m_Buffer, (GLsizeiptr)(all.size() * sizeof(std::uint32_t)), all.data(), 0);
}

VisibleIndexBuffer::~VisibleIndexBuffer() {
    if (m_Buffer) glDeleteBuffers(1, &m_Buffer);
}
