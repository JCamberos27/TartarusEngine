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

void SkinHideBuffer::Bind(unsigned buffer) {
    static SkinHideBuffer* empty = nullptr; // deliberately leaked: freed with the context, not at exit
    if (!buffer) {
        if (!empty) empty = new SkinHideBuffer({});
        buffer = empty->Id();
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBinding, buffer);
}
