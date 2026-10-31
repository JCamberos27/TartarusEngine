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
// `srcBuffer` != 0: the vertices and indices are copied on the GPU from that buffer at those byte offsets (an
// UploadStaging region) instead of read from the pointers.
bool Allocate(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
              Range& out, unsigned srcBuffer = 0, std::size_t srcVertexOffset = 0, std::size_t srcIndexOffset = 0);
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

// Skinned meshes: their own pool of vertex / index pages, kept apart from the arena above (a skinned mesh never joins an
// indirect draw). Each mesh keeps its own VAO, whose vertex binding starts at the mesh's first vertex, so gl_VertexID
// and PreSkin's source range stay per mesh, as with a buffer of its own. Creating a buffer per mesh made the driver
// allocate storage mid-Play: glNamedBufferStorage alone took 2-5 ms for some outfit pieces (AssetLibrary::PumpAsync).
// TARTARUS_NO_SKIN_ARENA=1 (or TARTARUS_NO_MESH_ARENA=1) turns it off: one buffer pair per skinned mesh, as before.
struct SkinRange {
    std::uint32_t Page = 0;        // 1-based; 0 = not pooled
    std::uint32_t FirstVertex = 0; // in vertices (its byte offset is SSBO-aligned)
    std::uint32_t FirstIndex = 0;  // in indices
    std::uint32_t VertexCount = 0, IndexCount = 0; // as taken (the vertex count rounded up to the alignment)
};
bool SkinEnabled();
// Places a skinned mesh's vertices and indices in a skinned page (copied from `srcBuffer` at those offsets, or from the
// pointers when it is 0). False when the pool is off.
bool AllocateSkinned(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
                     SkinRange& out, unsigned srcBuffer = 0, std::size_t srcVertexOffset = 0, std::size_t srcIndexOffset = 0);
void FreeSkinned(const SkinRange& r);
unsigned SkinPageVbo(std::uint32_t page);
unsigned SkinPageEbo(std::uint32_t page);
// Makes sure a skinned page with room is ready before anything asks for one (a new page is the one big allocation
// left): called once per asset pump, it creates the first page, and the next one when the last is three quarters full.
void WarmSkinned();
// A vertex array for a skinned mesh's own VAO, from a batch made ahead of time with the ModelVertex layout already
// set (the caller binds its buffers: glVertexArrayVertexBuffer / glVertexArrayElementBuffer). glCreateVertexArrays
// returns a name, and a vertex array's first use builds it, so with the driver's threaded optimization each waited
// for the driver thread to drain everything queued before it: 2-6 ms mid-Play. WarmSkinned refills the batch.
unsigned TakeVao();

} // namespace MeshArena
