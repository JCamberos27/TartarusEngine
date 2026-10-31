#pragma once

#include <cstdint>
#include <map>
#include <vector>

struct ModelVertex;

// Shared vertex / index storage for unskinned model meshes, so that many different meshes can go out in one
// glMultiDrawElementsIndirect (InstanceBatch.h): an indirect draw reads all its commands through one VAO, and every
// mesh in a page sits in that page's vertex and index buffers at its own base vertex and first index. Pages are a
// fixed size and never move (a mesh's VAO stays valid), a mesh bigger than a page gets a page of its own, and freed
// ranges are reused first-fit. Skinned meshes keep their own buffers (PreSkin reads them as a whole).
namespace MeshArena {

struct Range {
    std::uint32_t Page = 0;        // 1-based; 0 = not in the arena
    std::uint32_t BaseVertex = 0;  // in vertices
    std::uint32_t FirstIndex = 0;  // in indices
    std::uint32_t VertexCount = 0, IndexCount = 0;
};

// Whether meshes go into the arena (TARTARUS_NO_MESH_ARENA=1 turns it off, for A/B benches). Read once.
bool Enabled();
// Places the mesh's vertices and (0-based) indices in a page; false when the arena is off or GL refused.
bool Allocate(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
              Range& out);
void Free(const Range& r);
// The page's VAO, vertex and element buffers (ModelVertex layout, ModelMesh::CreateGpu's attributes).
unsigned PageVao(std::uint32_t page);
unsigned PageVbo(std::uint32_t page);
unsigned PageEbo(std::uint32_t page);
std::size_t PageCount();

// The first-fit free list one page keeps per buffer (exposed for the unit tests).
class FreeList {
public:
    explicit FreeList(std::uint32_t capacity = 0) { Reset(capacity); }
    void Reset(std::uint32_t capacity);
    // The offset of `size` free units, or ~0u when no free run is that long.
    std::uint32_t Take(std::uint32_t size);
    void Give(std::uint32_t offset, std::uint32_t size); // merges with free neighbours
    std::uint32_t FreeUnits() const;
    std::size_t Runs() const { return m_Free.size(); }
private:
    std::map<std::uint32_t, std::uint32_t> m_Free; // offset -> size
};

} // namespace MeshArena
