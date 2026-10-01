#pragma once

#include <cstdint>
#include <vector>

// A piece's hidden vertices on the GPU (docs/CHARACTER_OUTFITS.md): one bit per vertex of its model, in
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

// A piece's triangles with at least one vertex not hidden (the same bits as SkinHideBuffer), each sub-mesh's
// in its own range of one element buffer. Drawn instead of the mesh's own indices, so what clothing covers
// entirely costs nothing - no vertex shading, no rasterizing, in any pass (a quarter of an outfit's
// triangles, most of a body under its clothes). Triangles partly hidden are kept; the vertex shader drops
// those. Vertex numbers are the mesh's own, so gl_VertexID, the hide bits and the bone palette still line up.
class VisibleIndexBuffer {
public:
    // `perMesh`: each sub-mesh's kept indices, local to the sub-mesh.
    explicit VisibleIndexBuffer(const std::vector<std::vector<std::uint32_t>>& perMesh);
    ~VisibleIndexBuffer();
    VisibleIndexBuffer(const VisibleIndexBuffer&) = delete;
    VisibleIndexBuffer& operator=(const VisibleIndexBuffer&) = delete;

    struct Range {
        std::uint32_t Offset = 0; // bytes into Id()
        std::uint32_t Count = 0;  // indices; 0 = the sub-mesh is hidden entirely
    };
    unsigned Id() const { return m_Buffer; }
    int MeshCount() const { return (int)m_Ranges.size(); }
    const Range& MeshRange(int i) const { return m_Ranges[(size_t)i]; }
    std::uint64_t Triangles() const { return m_Triangles; }

private:
    unsigned m_Buffer = 0;
    std::vector<Range> m_Ranges;
    std::uint64_t m_Triangles = 0;
};

