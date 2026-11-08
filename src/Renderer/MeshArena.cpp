#include "MeshArena.h"
#include "ModelVertex.h"
#include "gl.h"
#include "Core/Env.h"
#include "Core/Log.h"
#include <cstddef>
#include <cstdlib>
#include <memory>

void ConfigureModelVao(unsigned vao, unsigned vbo, unsigned ebo); // ModelMesh.cpp

namespace MeshArena {

void FreeList::Reset(std::uint32_t capacity) {
    m_Free.clear();
    if (capacity) m_Free[0] = capacity;
}

std::uint32_t FreeList::Take(std::uint32_t size) {
    if (!size) size = 1;
    for (auto it = m_Free.begin(); it != m_Free.end(); ++it) {
        if (it->second < size) continue;
        const std::uint32_t at = it->first, left = it->second - size;
        m_Free.erase(it);
        if (left) m_Free[at + size] = left;
        return at;
    }
    return ~0u;
}

void FreeList::Give(std::uint32_t offset, std::uint32_t size) {
    if (!size) size = 1;
    auto next = m_Free.lower_bound(offset);
    if (next != m_Free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) { // grows the run before it
            prev->second += size;
            if (next != m_Free.end() && prev->first + prev->second == next->first) {
                prev->second += next->second;
                m_Free.erase(next);
            }
            return;
        }
    }
    if (next != m_Free.end() && offset + size == next->first) {
        size += next->second;
        m_Free.erase(next);
    }
    m_Free[offset] = size;
}

std::uint32_t FreeList::FreeUnits() const {
    std::uint32_t n = 0;
    for (const auto& [o, s] : m_Free) n += s;
    return n;
}

namespace {
// 64 MiB of vertices (128-byte ModelVertex: 512k of them) and 16 MiB of indices per page: big enough that a forest's
// few hundred meshes share a handful of pages, small enough that the last, part-filled page wastes little.
constexpr std::uint32_t kPageVertices = (64u << 20) / sizeof(ModelVertex);
constexpr std::uint32_t kPageIndices = (16u << 20) / sizeof(std::uint32_t);

struct Page {
    GLuint Vao = 0, Vbo = 0, Ebo = 0;
    FreeList Verts, Indices;
    bool Dedicated = false; // one oversized mesh
};

// Creates a page's vertex and index storage. A page is tens of MiB, so the allocation can fail; on failure
// the buffers are deleted again and false is returned.
bool CreatePageBuffers(Page& p, std::uint32_t verts, std::uint32_t indices) {
    while (glGetError() != GL_NO_ERROR) {} // drain, so only these calls' errors are seen below
    glCreateBuffers(1, &p.Vbo);
    glCreateBuffers(1, &p.Ebo);
    glNamedBufferStorage(p.Vbo, (GLsizeiptr)verts * (GLsizeiptr)sizeof(ModelVertex), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glNamedBufferStorage(p.Ebo, (GLsizeiptr)indices * (GLsizeiptr)sizeof(std::uint32_t), nullptr, GL_DYNAMIC_STORAGE_BIT);
    if (glGetError() == GL_NO_ERROR) return true;
    glDeleteBuffers(1, &p.Vbo);
    glDeleteBuffers(1, &p.Ebo);
    p.Vbo = p.Ebo = 0;
    Log::Error("MeshArena: out of GPU memory allocating a mesh page");
    return false;
}

// A list of pages with first-fit suballocation; used once for the static pages (which carry a VAO each) and once
// for the skin pages (plain buffers).
class PagePool {
public:
    explicit PagePool(bool withVao) : m_WithVao(withVao) {}

    // Returns false if the page's buffer storage could not be allocated.
    bool NewPage(std::uint32_t verts, std::uint32_t indices, bool dedicated) {
        auto p = std::make_unique<Page>();
        if (!CreatePageBuffers(*p, verts, indices)) return false;
        if (m_WithVao) {
            glCreateVertexArrays(1, &p->Vao);
            ConfigureModelVao(p->Vao, p->Vbo, p->Ebo);
        }
        p->Verts.Reset(verts);
        p->Indices.Reset(indices);
        p->Dedicated = dedicated;
        m_Pages.push_back(std::move(p));
        return true;
    }

    // Takes from one page; if the index take fails, the vertex take is given back.
    static bool TakeFrom(Page& p, std::uint32_t verts, std::uint32_t indices, std::uint32_t& vOff, std::uint32_t& iOff) {
        vOff = p.Verts.Take(verts);
        if (vOff == ~0u) return false;
        iOff = p.Indices.Take(indices);
        if (iOff == ~0u) { p.Verts.Give(vOff, verts); return false; }
        return true;
    }

    // First-fit over the shared (non-dedicated) pages. Returns the page index, or ~0u if none has room.
    std::uint32_t TryTake(std::uint32_t verts, std::uint32_t indices, std::uint32_t& vOff, std::uint32_t& iOff) {
        for (std::uint32_t k = 0; k < (std::uint32_t)m_Pages.size(); ++k) {
            Page& p = *m_Pages[k];
            if (p.Dedicated || !p.Vbo) continue;
            if (TakeFrom(p, verts, indices, vOff, iOff)) return k;
        }
        return ~0u;
    }

    // Finds room for a mesh: a shared page if it fits one, else a new page (dedicated when the mesh is bigger than a
    // standard page). Returns the page index, or ~0u if a new page could not be created.
    std::uint32_t Reserve(std::uint32_t verts, std::uint32_t indices, std::uint32_t& vOff, std::uint32_t& iOff) {
        const bool big = verts > kPageVertices || indices > kPageIndices;
        std::uint32_t page = big ? ~0u : TryTake(verts, indices, vOff, iOff);
        if (page != ~0u) return page;
        if (!NewPage(big ? verts : kPageVertices, big ? indices : kPageIndices, big)) return ~0u;
        page = (std::uint32_t)m_Pages.size() - 1;
        TakeFrom(*m_Pages[page], verts, indices, vOff, iOff);
        return page;
    }

    // Returns a range to its page (page1 is 1-based). A dedicated page's one mesh is gone: its memory goes back (the
    // slot stays, so page numbers don't shift).
    void Give(std::uint32_t page1, std::uint32_t vOff, std::uint32_t vCount, std::uint32_t iOff, std::uint32_t iCount) {
        if (!page1 || page1 > m_Pages.size()) return;
        Page& p = *m_Pages[page1 - 1];
        if (p.Dedicated) {
            glDeleteBuffers(1, &p.Vbo);
            glDeleteBuffers(1, &p.Ebo);
            if (m_WithVao) glDeleteVertexArrays(1, &p.Vao);
            p.Vao = p.Vbo = p.Ebo = 0;
            p.Verts.Reset(0);
            p.Indices.Reset(0);
            return;
        }
        p.Verts.Give(vOff, vCount);
        p.Indices.Give(iOff, iCount);
    }

    // Writes the mesh into its taken range, from memory or from a source buffer.
    void Upload(std::uint32_t page, std::uint32_t v, std::uint32_t i, const ModelVertex* vertices, std::uint32_t vertexCount,
                const std::uint32_t* indices, std::uint32_t indexCount, unsigned srcBuffer, std::size_t srcVertexOffset,
                std::size_t srcIndexOffset) const {
        const Page& p = *m_Pages[page];
        if (srcBuffer) {
            glCopyNamedBufferSubData(srcBuffer, p.Vbo, (GLintptr)srcVertexOffset, (GLintptr)v * (GLintptr)sizeof(ModelVertex),
                                     (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex));
            glCopyNamedBufferSubData(srcBuffer, p.Ebo, (GLintptr)srcIndexOffset, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4);
        } else {
            glNamedBufferSubData(p.Vbo, (GLintptr)v * (GLintptr)sizeof(ModelVertex), (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex),
                                 vertices);
            glNamedBufferSubData(p.Ebo, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4, indices);
        }
    }

    unsigned Vao(std::uint32_t page) const { return page && page <= m_Pages.size() ? m_Pages[page - 1]->Vao : 0u; }
    unsigned Vbo(std::uint32_t page) const { return page && page <= m_Pages.size() ? m_Pages[page - 1]->Vbo : 0u; }
    unsigned Ebo(std::uint32_t page) const { return page && page <= m_Pages.size() ? m_Pages[page - 1]->Ebo : 0u; }
    const std::vector<std::unique_ptr<Page>>& Pages() const { return m_Pages; }

private:
    bool m_WithVao;
    std::vector<std::unique_ptr<Page>> m_Pages;
};

PagePool g_Static(true);
PagePool g_Skin(false);
} // namespace

bool Enabled() {
    static const bool on = !Env::Flag(Env::Var::NoMeshArena);
    return on;
}

bool Allocate(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
              Range& out, unsigned srcBuffer, std::size_t srcVertexOffset, std::size_t srcIndexOffset) {
    if (!Enabled() || !vertexCount || !indexCount) return false;
    std::uint32_t v = ~0u, i = ~0u;
    const std::uint32_t page = g_Static.Reserve(vertexCount, indexCount, v, i);
    if (page == ~0u) return false;
    g_Static.Upload(page, v, i, vertices, vertexCount, indices, indexCount, srcBuffer, srcVertexOffset, srcIndexOffset);
    out = {page + 1, v, i, vertexCount, indexCount};
    return true;
}

void Free(const Range& r) { g_Static.Give(r.Page, r.BaseVertex, r.VertexCount, r.FirstIndex, r.IndexCount); }

unsigned PageVao(std::uint32_t page) { return g_Static.Vao(page); }
unsigned PageVbo(std::uint32_t page) { return g_Static.Vbo(page); }
unsigned PageEbo(std::uint32_t page) { return g_Static.Ebo(page); }

// --- Skinned pages ----------------------------------------------------------------------------------------------------
namespace {
std::vector<GLuint> g_SpareVaos;
constexpr int kVaoBatch = 256;

// The names come with the ModelVertex attribute layout already set (over no buffers): the driver builds a vertex
// array on its first use, and that first use waited on the driver thread too (2-6 ms each mid-Play). A mesh taking
// one then only points it at its page.
void RefillVaos() {
    GLuint ids[kVaoBatch];
    glCreateVertexArrays(kVaoBatch, ids);
    for (GLuint v : ids) ConfigureModelVao(v, 0, 0);
    g_SpareVaos.insert(g_SpareVaos.end(), ids, ids + kVaoBatch);
}

// Vertices per SSBO offset step: PreSkin binds a mesh's range of its page, whose byte offset must be a multiple of
// GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT. Every take is a multiple of this, so every offset is too.
std::uint32_t SkinAlignVertices() {
    static const std::uint32_t n = [] {
        GLint align = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
        if (align <= 0) align = 256;
        std::uint32_t v = 1;
        while ((v * sizeof(ModelVertex)) % (std::uint32_t)align) ++v;
        return v;
    }();
    return n;
}

} // namespace

bool SkinEnabled() {
    static const bool on = !Env::Flag(Env::Var::NoSkinArena);
    return on && Enabled();
}

unsigned TakeVao() {
    if (g_SpareVaos.empty()) RefillVaos();
    const GLuint v = g_SpareVaos.back();
    g_SpareVaos.pop_back();
    return v;
}

void WarmSkinned() {
    if (!SkinEnabled()) return;
    if (g_SpareVaos.size() < kVaoBatch / 4) RefillVaos();
    std::uint32_t freeVerts = 0, freeIndices = 0;
    for (const auto& p : g_Skin.Pages()) {
        if (p->Dedicated || !p->Vbo) continue;
        freeVerts += p->Verts.FreeUnits();
        freeIndices += p->Indices.FreeUnits();
    }
    if (freeVerts < kPageVertices / 4 || freeIndices < kPageIndices / 4) g_Skin.NewPage(kPageVertices, kPageIndices, false);
}

bool AllocateSkinned(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
                     SkinRange& out, unsigned srcBuffer, std::size_t srcVertexOffset, std::size_t srcIndexOffset) {
    if (!SkinEnabled() || !vertexCount || !indexCount) return false;
    const std::uint32_t align = SkinAlignVertices();
    const std::uint32_t takeVerts = (vertexCount + align - 1) / align * align;
    std::uint32_t v = ~0u, i = ~0u;
    const std::uint32_t page = g_Skin.Reserve(takeVerts, indexCount, v, i);
    if (page == ~0u) return false;
    g_Skin.Upload(page, v, i, vertices, vertexCount, indices, indexCount, srcBuffer, srcVertexOffset, srcIndexOffset);
    out = {page + 1, v, i, takeVerts, indexCount};
    return true;
}

void FreeSkinned(const SkinRange& r) { g_Skin.Give(r.Page, r.FirstVertex, r.VertexCount, r.FirstIndex, r.IndexCount); }

unsigned SkinPageVbo(std::uint32_t page) { return g_Skin.Vbo(page); }
unsigned SkinPageEbo(std::uint32_t page) { return g_Skin.Ebo(page); }

} // namespace MeshArena
