#include "MeshArena.h"
#include "ModelVertex.h"
#include "gl.h"
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
std::vector<std::unique_ptr<Page>> g_Pages;

bool NewPage(std::uint32_t verts, std::uint32_t indices, bool dedicated) {
    auto p = std::make_unique<Page>();
    glCreateBuffers(1, &p->Vbo);
    glCreateBuffers(1, &p->Ebo);
    glNamedBufferStorage(p->Vbo, (GLsizeiptr)verts * (GLsizeiptr)sizeof(ModelVertex), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glNamedBufferStorage(p->Ebo, (GLsizeiptr)indices * (GLsizeiptr)sizeof(std::uint32_t), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateVertexArrays(1, &p->Vao);
    ConfigureModelVao(p->Vao, p->Vbo, p->Ebo);
    p->Verts.Reset(verts);
    p->Indices.Reset(indices);
    p->Dedicated = dedicated;
    g_Pages.push_back(std::move(p));
    return true;
}
} // namespace

bool Enabled() {
    static const bool on = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool off = _dupenv_s(&e, &n, "TARTARUS_NO_MESH_ARENA") == 0 && e && e[0] == '1';
        std::free(e);
        return !off;
    }();
    return on;
}

bool Allocate(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
              Range& out, unsigned srcBuffer, std::size_t srcVertexOffset, std::size_t srcIndexOffset) {
    if (!Enabled() || !vertexCount || !indexCount) return false;
    const bool big = vertexCount > kPageVertices || indexCount > kPageIndices;
    std::uint32_t page = ~0u, v = ~0u, i = ~0u;
    if (!big) {
        for (std::uint32_t k = 0; k < (std::uint32_t)g_Pages.size() && page == ~0u; ++k) {
            Page& p = *g_Pages[k];
            if (p.Dedicated || !p.Vao) continue;
            v = p.Verts.Take(vertexCount);
            if (v == ~0u) continue;
            i = p.Indices.Take(indexCount);
            if (i == ~0u) { p.Verts.Give(v, vertexCount); continue; }
            page = k;
        }
    }
    if (page == ~0u) {
        if (!NewPage(big ? vertexCount : kPageVertices, big ? indexCount : kPageIndices, big)) return false;
        page = (std::uint32_t)g_Pages.size() - 1;
        v = g_Pages[page]->Verts.Take(vertexCount);
        i = g_Pages[page]->Indices.Take(indexCount);
    }
    const Page& p = *g_Pages[page];
    if (srcBuffer) {
        glCopyNamedBufferSubData(srcBuffer, p.Vbo, (GLintptr)srcVertexOffset, (GLintptr)v * (GLintptr)sizeof(ModelVertex),
                                 (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex));
        glCopyNamedBufferSubData(srcBuffer, p.Ebo, (GLintptr)srcIndexOffset, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4);
    } else {
        glNamedBufferSubData(p.Vbo, (GLintptr)v * (GLintptr)sizeof(ModelVertex), (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex),
                             vertices);
        glNamedBufferSubData(p.Ebo, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4, indices);
    }
    out = {page + 1, v, i, vertexCount, indexCount};
    return true;
}

void Free(const Range& r) {
    if (!r.Page || r.Page > g_Pages.size()) return;
    Page& p = *g_Pages[r.Page - 1];
    if (p.Dedicated) { // its one mesh is gone: give the memory back (the slot stays, so page numbers don't shift)
        glDeleteBuffers(1, &p.Vbo);
        glDeleteBuffers(1, &p.Ebo);
        glDeleteVertexArrays(1, &p.Vao);
        p.Vao = p.Vbo = p.Ebo = 0;
        p.Verts.Reset(0);
        p.Indices.Reset(0);
        return;
    }
    p.Verts.Give(r.BaseVertex, r.VertexCount);
    p.Indices.Give(r.FirstIndex, r.IndexCount);
}

unsigned PageVao(std::uint32_t page) { return page && page <= g_Pages.size() ? g_Pages[page - 1]->Vao : 0u; }
unsigned PageVbo(std::uint32_t page) { return page && page <= g_Pages.size() ? g_Pages[page - 1]->Vbo : 0u; }
unsigned PageEbo(std::uint32_t page) { return page && page <= g_Pages.size() ? g_Pages[page - 1]->Ebo : 0u; }
std::size_t PageCount() { return g_Pages.size(); }

// --- Skinned pages ----------------------------------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kSkinPageVertices = (64u << 20) / sizeof(ModelVertex);
constexpr std::uint32_t kSkinPageIndices = (16u << 20) / sizeof(std::uint32_t);
std::vector<std::unique_ptr<Page>> g_SkinPages;
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

bool NewSkinPage(std::uint32_t verts, std::uint32_t indices, bool dedicated) {
    auto p = std::make_unique<Page>();
    glCreateBuffers(1, &p->Vbo);
    glCreateBuffers(1, &p->Ebo);
    glNamedBufferStorage(p->Vbo, (GLsizeiptr)verts * (GLsizeiptr)sizeof(ModelVertex), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glNamedBufferStorage(p->Ebo, (GLsizeiptr)indices * (GLsizeiptr)sizeof(std::uint32_t), nullptr, GL_DYNAMIC_STORAGE_BIT);
    p->Verts.Reset(verts);
    p->Indices.Reset(indices);
    p->Dedicated = dedicated;
    g_SkinPages.push_back(std::move(p));
    return true;
}
} // namespace

bool SkinEnabled() {
    static const bool on = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool off = _dupenv_s(&e, &n, "TARTARUS_NO_SKIN_ARENA") == 0 && e && e[0] == '1';
        std::free(e);
        return !off;
    }();
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
    for (const auto& p : g_SkinPages) {
        if (p->Dedicated || !p->Vbo) continue;
        freeVerts += p->Verts.FreeUnits();
        freeIndices += p->Indices.FreeUnits();
    }
    if (freeVerts < kSkinPageVertices / 4 || freeIndices < kSkinPageIndices / 4) NewSkinPage(kSkinPageVertices, kSkinPageIndices, false);
}

bool AllocateSkinned(const ModelVertex* vertices, std::uint32_t vertexCount, const std::uint32_t* indices, std::uint32_t indexCount,
                     SkinRange& out, unsigned srcBuffer, std::size_t srcVertexOffset, std::size_t srcIndexOffset) {
    if (!SkinEnabled() || !vertexCount || !indexCount) return false;
    const std::uint32_t align = SkinAlignVertices();
    const std::uint32_t takeVerts = (vertexCount + align - 1) / align * align;
    const bool big = takeVerts > kSkinPageVertices || indexCount > kSkinPageIndices;
    std::uint32_t page = ~0u, v = ~0u, i = ~0u;
    if (!big) {
        for (std::uint32_t k = 0; k < (std::uint32_t)g_SkinPages.size() && page == ~0u; ++k) {
            Page& p = *g_SkinPages[k];
            if (p.Dedicated || !p.Vbo) continue;
            v = p.Verts.Take(takeVerts);
            if (v == ~0u) continue;
            i = p.Indices.Take(indexCount);
            if (i == ~0u) { p.Verts.Give(v, takeVerts); continue; }
            page = k;
        }
    }
    if (page == ~0u) {
        if (!NewSkinPage(big ? takeVerts : kSkinPageVertices, big ? indexCount : kSkinPageIndices, big)) return false;
        page = (std::uint32_t)g_SkinPages.size() - 1;
        v = g_SkinPages[page]->Verts.Take(takeVerts);
        i = g_SkinPages[page]->Indices.Take(indexCount);
    }
    const Page& p = *g_SkinPages[page];
    if (srcBuffer) {
        glCopyNamedBufferSubData(srcBuffer, p.Vbo, (GLintptr)srcVertexOffset, (GLintptr)v * (GLintptr)sizeof(ModelVertex),
                                 (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex));
        glCopyNamedBufferSubData(srcBuffer, p.Ebo, (GLintptr)srcIndexOffset, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4);
    } else {
        glNamedBufferSubData(p.Vbo, (GLintptr)v * (GLintptr)sizeof(ModelVertex), (GLsizeiptr)vertexCount * (GLsizeiptr)sizeof(ModelVertex),
                             vertices);
        glNamedBufferSubData(p.Ebo, (GLintptr)i * 4, (GLsizeiptr)indexCount * 4, indices);
    }
    out = {page + 1, v, i, takeVerts, indexCount};
    return true;
}

void FreeSkinned(const SkinRange& r) {
    if (!r.Page || r.Page > g_SkinPages.size()) return;
    Page& p = *g_SkinPages[r.Page - 1];
    if (p.Dedicated) {
        glDeleteBuffers(1, &p.Vbo);
        glDeleteBuffers(1, &p.Ebo);
        p.Vbo = p.Ebo = 0;
        p.Verts.Reset(0);
        p.Indices.Reset(0);
        return;
    }
    p.Verts.Give(r.FirstVertex, r.VertexCount);
    p.Indices.Give(r.FirstIndex, r.IndexCount);
}

unsigned SkinPageVbo(std::uint32_t page) { return page && page <= g_SkinPages.size() ? g_SkinPages[page - 1]->Vbo : 0u; }
unsigned SkinPageEbo(std::uint32_t page) { return page && page <= g_SkinPages.size() ? g_SkinPages[page - 1]->Ebo : 0u; }

} // namespace MeshArena
