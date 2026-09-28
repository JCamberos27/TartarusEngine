#pragma once

#include <cstdint>
#include <vector>

// A piece's hidden vertices on the GPU (CHARACTER_OUTFITS.md): one bit per vertex of its model, in
// the order Model::CollisionGeometry lists them (sub-mesh by sub-mesh). ModelVertex.glsl reads it as
// the SSBO at binding kBinding, by gl_VertexID plus the sub-mesh's first vertex (uHideVertBase).
class SkinHideBuffer {
public:
    static constexpr unsigned kBinding = 20;
    // The player's collar in the camera's own view (PlayerBodyTag::CollarVerts): the same layout, another block.
    static constexpr unsigned kCollarBinding = 21;

    explicit SkinHideBuffer(const std::vector<std::uint32_t>& bits);
    ~SkinHideBuffer();
    SkinHideBuffer(const SkinHideBuffer&) = delete;
    SkinHideBuffer& operator=(const SkinHideBuffer&) = delete;

    unsigned Id() const { return m_Buffer; }
    // Binds `buffer` (0 = a one-word empty buffer, so the block is never unbound) at `binding`.
    static void Bind(unsigned buffer, unsigned binding = kBinding);

private:
    unsigned m_Buffer = 0;
};
