// Model skinning: the bone-palette ring buffer and the compute pre-skin pass.

#include "Model.h"
#include "DecalSystem.h"
#include "SkinHideBuffer.h"
#include "Log.h"
#include "Texture.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"
#include "Core/Env.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "ModelDrawInternal.h"

using ModelInternal::CachedMaterialLocs;
using ModelInternal::MaterialLocs;

namespace {
// Bone palettes (SSBO binding 1, #104) live in one ring buffer split into per-frame slices. Each
// skinned model uploads its palette once per frame into its own range of the current slice and
// binds that range; the shadow cascades, depth pre-pass and main pass that follow only rebind
// it. It used to be one MAX_BONES buffer rewritten before every skinned draw, so each upload had
// to wait on (or be copied around) the draws still queued against the previous contents.
// Process-lifetime, never freed (like the other engine-lifetime GL objects).
constexpr int kBoneRingSlices = 3; // frames in flight a slice waits out before it's rewritten
constexpr GLsizeiptr kBoneSliceBytes = 2 * 1024 * 1024; // 32768 matrices per frame
unsigned int g_BoneRing = 0;
GLsizeiptr g_BoneAlign = 256;
int g_BoneSlice = 0;
GLsizeiptr g_BoneCursor = 0;
// Advances at every new frame and whenever a full slice wraps onto itself mid-frame, so a model's
// remembered range (Model::BoneUpload) is only trusted while nothing can have overwritten it.
std::uint64_t g_BoneEpoch = 0;
GLintptr g_BoundOffset = -1;
GLsizeiptr g_BoundSize = 0;

// Model::PreSkin's output (SSBO binding 26, SkinnedVertex.glsl): this frame's skinned vertices, model after model.
// One buffer, rewritten each frame: GL runs the next frame's skinning after this frame's draws that read it.
constexpr unsigned kSkinSourceBinding = 25;  // the mesh's ModelVertex buffer, while PreSkin.comp.glsl runs
constexpr unsigned kSkinnedBinding = 26;
constexpr GLsizeiptr kSkinnedVertexBytes = 20; // kSkinnedWords * 4
constexpr GLsizeiptr kSkinnedMaxBytes = 128ll * 1024 * 1024; // GL's minimum SSBO size limit; 6.7M vertices
unsigned int g_SkinnedBuffer = 0;
GLsizeiptr g_SkinnedBytes = 0;
bool g_SkinnedBound = false;
std::uint64_t g_SkinFrame = 1; // PreSkinRange::Frame of the skinning the buffer holds now
// Counts BeginRenderFrame calls. A model no pass drew last frame (behind the camera and outside every shadow view, or
// in no view at all) isn't pre-skinned: if a pass draws it after all, that pass skins it in its vertex shader, and
// from the next frame on it is pre-skinned again.
std::uint64_t g_RenderFrame = 2;
// Which model sits where in the buffer. Bumped when a call lays models out differently from the call before it (or the
// buffer is reallocated); while it holds, a model whose pose hasn't changed still has its vertices in place, so it isn't
// dispatched again: an idle body, a corpse, a character whose animation is paused costs nothing on the GPU.
std::uint64_t g_SkinLayout = 1;
// G5-0 counters (Model::TakeSkinFrameStats): this frame's, the models the last PreSkin dispatched (with their vertex
// counts) and the skinned models a pass drew since then. Pointers are only compared, never followed.
bool g_SkinStatsOn = false; // Model::SetSkinStatsEnabled: off unless --perf-bench asks
Model::SkinFrameStats g_SkinStats;
std::vector<std::pair<const void*, std::uint64_t>> g_LastDispatched;
std::unordered_set<const void*> g_DrawnSinceSkin;

// The vertex shaders declare the block whether or not they read it: keep binding 26 on a real buffer.
void EnsureSkinnedBinding() {
    if (!g_SkinnedBuffer) {
        glCreateBuffers(1, &g_SkinnedBuffer);
        glNamedBufferStorage(g_SkinnedBuffer, 256, nullptr, 0);
        g_SkinnedBytes = 256;
    }
    if (g_SkinnedBound) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSkinnedBinding, g_SkinnedBuffer);
    g_SkinnedBound = true;
}

// PreSkin.comp.glsl, built on first use, and bound (null when it doesn't build: everything skins per pass).
Shader* BindPreSkinProgram() {
    static std::unique_ptr<Shader> program;
    static bool failed = false;
    if (failed) return nullptr;
    if (!program) {
        try {
            program = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("PreSkin.comp.glsl"), "PreSkin");
        } catch (const std::exception& e) {
            failed = true;
            Log::Error(std::string("Pre-skinning is off (characters skin in each pass's vertex shader): ") + e.what());
            return nullptr;
        }
    }
    Shader& cs = *program;
    cs.Bind();
    // ModelVertex's layout in words, from the struct itself (the value cache makes these free after the first call).
    static_assert(sizeof(ModelVertex) % 4 == 0 && sizeof(int) == 4 && sizeof(float) == 4, "PreSkin reads ModelVertex as words");
    cs.SetInt("uStride", (int)(sizeof(ModelVertex) / 4));
    cs.SetInt("uOffNormal", (int)(offsetof(ModelVertex, Normal) / 4));
    cs.SetInt("uOffTangent", (int)(offsetof(ModelVertex, Tangent) / 4));
    cs.SetInt("uOffBones", (int)(offsetof(ModelVertex, BoneIDs) / 4));
    cs.SetInt("uOffWeights", (int)(offsetof(ModelVertex, Weights) / 4));
    return &cs;
}

// Skins `count` vertices of `sourceVbo` (ModelVertex[]) with the palette on binding 1 into the buffer on binding 26,
// from vertex `outBase` on. `cs` is BindPreSkinProgram's, still bound.
void DispatchPreSkin(Shader& cs, unsigned sourceVbo, unsigned count, unsigned outBase, std::size_t sourceOffset = 0) {
    static const int countLoc = cs.Loc("uCount"), baseLoc = cs.Loc("uOutBase");
    // A mesh in a shared skinned page: just its range (MeshArena keeps the offset SSBO-aligned).
    if (sourceOffset) glBindBufferRange(GL_SHADER_STORAGE_BUFFER, kSkinSourceBinding, sourceVbo, (GLintptr)sourceOffset, (GLsizeiptr)count * (GLsizeiptr)sizeof(ModelVertex));
    else glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSkinSourceBinding, sourceVbo);
    cs.SetInt(countLoc, (int)count);
    cs.SetInt(baseLoc, (int)outBase);
    glDispatchCompute((count + 63) / 64, 1, 1);
}

void EnsureBoneRing() {
    if (g_BoneRing) return;
    glCreateBuffers(1, &g_BoneRing);
    glNamedBufferStorage(g_BoneRing, kBoneRingSlices * kBoneSliceBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    GLint align = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
    if (align > 0) g_BoneAlign = align;
}

void BindBoneRange(GLintptr offset, GLsizeiptr size) {
    if (offset == g_BoundOffset && size == g_BoundSize) return;
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, g_BoneRing, offset, size);
    g_BoundOffset = offset;
    g_BoundSize = size;
}
} // namespace

void Model::BeginRenderFrame() {
    g_BoneSlice = (g_BoneSlice + 1) % kBoneRingSlices;
    g_BoneCursor = 0;
    ++g_BoneEpoch;
    ++g_SkinFrame; // last frame's skinned vertices are stale
    ++g_RenderFrame;
}

const std::vector<glm::mat4>* Model::SkinPalette(int& count) const {
    if (m_PaletteDirty) ResolvePalette();
    const bool posed = m_ExternalPose || (m_Anim.Clip >= 0 && m_Anim.Clip < AnimationCount()) || m_FadeDuration > 0.0f;
    const std::vector<glm::mat4>& palette =
        posed && m_FinalBoneMatrices.size() >= (size_t)MAX_BONES ? m_FinalBoneMatrices : m_D->BindPoseBones;
    // Only the rig's own bones (#113), not the whole MAX_BONES palette.
    count = std::clamp(m_D->BoneCounter, 1, MAX_BONES);
    return palette.size() < (size_t)count ? nullptr : &palette;
}

void Model::BindBonePalette(const std::vector<glm::mat4>& palette, int count) const {
    EnsureBoneRing();
    const GLsizeiptr bytes = count * (GLsizeiptr)sizeof(glm::mat4);
    BoneUpload& u = m_BoneUpload;
    if (u.Epoch != g_BoneEpoch || u.Version != m_PoseVersion || u.Palette != palette.data() || u.Count != count) {
        const GLsizeiptr slot = (bytes + g_BoneAlign - 1) / g_BoneAlign * g_BoneAlign;
        if (g_BoneCursor + slot > kBoneSliceBytes) { g_BoneCursor = 0; ++g_BoneEpoch; } // slice full: reuse it
        u.Offset = (std::ptrdiff_t)(g_BoneSlice * kBoneSliceBytes + g_BoneCursor);
        g_BoneCursor += slot;
        glNamedBufferSubData(g_BoneRing, (GLintptr)u.Offset, bytes, palette.data());
        u.Epoch = g_BoneEpoch;
        u.Version = m_PoseVersion;
        u.Palette = palette.data();
        u.Count = count;
    }
    BindBoneRange((GLintptr)u.Offset, bytes);
}

void Model::SetSkinStatsEnabled(bool on) {
    g_SkinStatsOn = on;
    if (!on) {
        g_SkinStats = {};
        g_LastDispatched.clear();
        g_DrawnSinceSkin.clear();
    }
}

bool Model::SkinStatsEnabled() { return g_SkinStatsOn; }

Model::SkinFrameStats Model::TakeSkinFrameStats() {
    const SkinFrameStats s = g_SkinStats;
    g_SkinStats = {};
    return s;
}

void Model::NotePosedDraw(double ms) {
    ++g_SkinStats.PosedDraws;
    g_SkinStats.PosedDrawMs += ms;
}

bool Model::UploadBoneMatrices(Shader& shader) const {
    // #98 — skin whenever there are bones: the clip's pose while playing, else the bind pose.
    const bool skinning = m_D->BoneCounter > 0;
    if (skinning) {
        m_PreSkin.Drawn = g_RenderFrame;
        if (g_SkinStatsOn) g_DrawnSinceSkin.insert(this); // G5-0: a pass drew it
    }
    const MaterialLocs& locs = CachedMaterialLocs(shader);
    EnsureBoneRing();
    EnsureSkinnedBinding();
    if (!skinning) {
        shader.SetInt(locs.useSkinning, 0);
        // The vertex shader still declares the block: keep binding 1 on a real range (whatever
        // palette was bound last is fine - it's never read), never dangling.
        if (g_BoundOffset < 0) BindBoneRange(0, (GLsizeiptr)sizeof(glm::mat4));
        return false;
    }
    int count = 0;
    const std::vector<glm::mat4>* palette = SkinPalette(count);
    const PreSkinRange& p = m_PreSkin;
    if (palette && p.Frame == g_SkinFrame && p.Version == m_PoseVersion && p.Palette == palette->data() && p.Count == count) {
        shader.SetInt(locs.useSkinning, 2); // skinned already this frame: the palette isn't read
        if (g_BoundOffset < 0) BindBoneRange(0, (GLsizeiptr)sizeof(glm::mat4));
        return true;
    }
    shader.SetInt(locs.useSkinning, 1);
    if (palette) BindBonePalette(*palette, count);
    return false;
}

void Model::PreSkin(const std::vector<Model*>& models) {
    // G5-0: what the last call skinned and no pass drew since was skinned for nothing (off-screen).
    const bool stats = g_SkinStatsOn;
    if (stats) {
        for (const auto& [model, verts] : g_LastDispatched)
            if (!g_DrawnSinceSkin.count(model)) {
                ++g_SkinStats.OffscreenModels;
                g_SkinStats.OffscreenVertices += verts;
            }
        g_LastDispatched.clear();
        g_DrawnSinceSkin.clear();
    }
    ++g_SkinFrame; // a second call in one frame rewrites the buffer: what the first one skinned is stale
    static const bool disabled = Env::Flag(Env::Var::NoPreskin);
    static const bool skinUnseen = Env::Flag(Env::Var::NoSkinCull);
    if (disabled || models.empty()) return;

    // What gets skinned: each model with bones and every sub-mesh on the GPU, once, while the buffer's cap allows
    // (anything past it skins in the vertex shader).
    static std::vector<std::pair<Model*, const std::vector<glm::mat4>*>> work;
    static std::vector<int> counts;
    work.clear();
    counts.clear();
    GLsizeiptr vertices = 0;
    int unseen = 0;
    for (Model* m : models) {
        if (!m || m->m_D->BoneCounter <= 0 || m->m_PreSkin.Frame == g_SkinFrame) continue;
        // No pass drew it last frame or yet this frame: leave it to the vertex shader if one does.
        if (!skinUnseen && m->m_PreSkin.Drawn + 1 < g_RenderFrame) { ++unseen; continue; }
        bool uploaded = true;
        GLsizeiptr n = 0;
        for (const auto& mesh : m->m_D->Meshes) {
            if (mesh->NeedsUpload()) { uploaded = false; break; }
            n += mesh->VertexCount();
        }
        int count = 0;
        const std::vector<glm::mat4>* palette = uploaded && n > 0 ? m->SkinPalette(count) : nullptr;
        if (!palette || (vertices + n) * kSkinnedVertexBytes > kSkinnedMaxBytes) continue;
        m->m_PreSkin.Frame = g_SkinFrame; // claimed (a model listed twice is skinned once); filled in below
        m->m_PreSkin.Base = (int)vertices;
        vertices += n;
        work.emplace_back(m, palette);
        counts.push_back(count);
    }
    if (stats) {
        const int skinned = (int)std::count_if(models.begin(), models.end(), [](const Model* m) { return m && m->m_D->BoneCounter > 0; });
        g_SkinStats.Listed += skinned;
        g_SkinStats.Unseen += unseen;
        g_SkinStats.Fallback += skinned - unseen - (int)work.size();
    }
    if (work.empty()) return;
    Shader* cs = BindPreSkinProgram();
    if (!cs) {
        for (const auto& item : work) item.first->m_PreSkin.Frame = ~0ull; // not skinned: they skin per pass
        if (stats) g_SkinStats.Fallback += (int)work.size();
        return;
    }

    // Grown, never shrunk, with headroom so a respawn doesn't reallocate it.
    if (vertices * kSkinnedVertexBytes > g_SkinnedBytes) {
        const GLsizeiptr bytes = std::min(kSkinnedMaxBytes, vertices * kSkinnedVertexBytes * 3 / 2);
        if (g_SkinnedBuffer) glDeleteBuffers(1, &g_SkinnedBuffer);
        glCreateBuffers(1, &g_SkinnedBuffer);
        glNamedBufferStorage(g_SkinnedBuffer, bytes, nullptr, 0); // written and read by the GPU only
        g_SkinnedBytes = bytes;
        g_SkinnedBound = false;
        ++g_SkinLayout; // a new buffer holds nothing yet
    }
    EnsureSkinnedBinding();

    // The same models at the same offsets as the last call: the buffer still holds their vertices from then.
    struct LayoutEntry { const Model* model; const void* data; int base; bool operator!=(const LayoutEntry& o) const { return model != o.model || data != o.data || base != o.base; } };
    static std::vector<LayoutEntry> lastLayout, layout;
    layout.clear();
    for (const auto& item : work) layout.push_back({item.first, item.first->m_D.get(), item.first->m_PreSkin.Base});
    if (layout.size() != lastLayout.size() || !std::equal(layout.begin(), layout.end(), lastLayout.begin(),
                                                           [](const LayoutEntry& a, const LayoutEntry& b) { return !(a != b); }))
        ++g_SkinLayout;
    std::swap(layout, lastLayout);

    bool dispatched = false;
    for (size_t w = 0; w < work.size(); ++w) {
        Model& m = *work[w].first;
        const std::vector<glm::mat4>& palette = *work[w].second;
        PreSkinRange& p = m.m_PreSkin;
        const bool unchanged = p.Layout == g_SkinLayout && p.Version == m.m_PoseVersion && p.Palette == palette.data() && p.Count == counts[w];
        if (!unchanged) {
            m.BindBonePalette(palette, counts[w]);
            unsigned first = (unsigned)p.Base;
            for (const auto& mesh : m.m_D->Meshes) {
                const unsigned n = mesh->VertexCount();
                if (n) DispatchPreSkin(*cs, mesh->VertexBuffer(), n, first, mesh->VertexBufferOffset());
                first += n;
            }
            dispatched = true;
            if (stats) {
                const std::uint64_t verts = first - (unsigned)p.Base;
                ++g_SkinStats.Dispatched;
                g_SkinStats.DispatchedVertices += verts;
                g_LastDispatched.emplace_back(&m, verts);
            }
        } else if (stats) {
            ++g_SkinStats.Idle;
        }
        p.Version = m.m_PoseVersion;
        p.Palette = palette.data();
        p.Count = counts[w];
        p.Layout = g_SkinLayout;
    }
    // Every pass after this reads the vertices in its vertex shader.
    if (dispatched) glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

bool Model::PreSkinSelfTest(std::string& why) {
    // Vertices with every influence count from none to eight (and a bone index past the palette), random
    // positions, normals and tangents, skinned by a random palette on the GPU and on the CPU.
    constexpr int kBones = 6, kVerts = 300;
    std::uint32_t seed = 12345u;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f; };
    std::vector<glm::mat4> palette(kBones);
    for (glm::mat4& b : palette) {
        const glm::vec3 axis = glm::normalize(glm::vec3(rnd(), rnd(), rnd()) + glm::vec3(0.0f, 0.0f, 1.5f));
        b = glm::translate(glm::mat4(1.0f), glm::vec3(rnd(), rnd(), rnd())) * glm::rotate(glm::mat4(1.0f), rnd() * 3.0f, axis) *
            glm::scale(glm::mat4(1.0f), glm::vec3(1.0f + 0.3f * rnd()));
    }
    std::vector<ModelVertex> verts(kVerts);
    for (int i = 0; i < kVerts; ++i) {
        ModelVertex& v = verts[(size_t)i];
        v.Position = glm::vec3(rnd(), rnd(), rnd()) * 2.0f;
        v.Normal = glm::normalize(glm::vec3(rnd(), rnd(), rnd()) + glm::vec3(0.0f, 0.01f, 0.0f));
        v.Tangent = glm::normalize(glm::cross(v.Normal, glm::vec3(0.3f, 0.9f, 0.1f)));
        const int influences = i % (MAX_BONE_INFLUENCE + 1);
        for (int k = 0; k < influences; ++k) {
            v.BoneIDs[k] = (i * 7 + k * 3) % (kBones + 1); // kBones itself: past the palette, clamped like the shaders
            v.Weights[k] = 0.1f + 0.5f * (rnd() * 0.5f + 0.5f);
        }
    }
    auto cpuSkin = [&](const ModelVertex& v, glm::vec3& p, glm::vec3& n, glm::vec3& t) {
        glm::mat4 m(0.0f);
        float total = 0.0f;
        for (int k = 0; k < MAX_BONE_INFLUENCE; ++k)
            if (v.BoneIDs[k] >= 0) { m += palette[(size_t)std::clamp(v.BoneIDs[k], 0, kBones - 1)] * v.Weights[k]; total += v.Weights[k]; }
        if (total <= 0.0001f) m = glm::mat4(1.0f);
        p = glm::vec3(m * glm::vec4(v.Position, 1.0f));
        n = glm::normalize(glm::mat3(m) * v.Normal);
        t = glm::normalize(glm::mat3(m) * v.Tangent);
    };
    auto unpackOct = [](std::uint32_t bits) {
        const float x = std::max((float)(std::int16_t)(bits & 0xffffu) / 32767.0f, -1.0f);
        const float y = std::max((float)(std::int16_t)(bits >> 16) / 32767.0f, -1.0f);
        glm::vec3 n(x, y, 1.0f - std::abs(x) - std::abs(y));
        const float t = std::max(-n.z, 0.0f);
        n.x += n.x >= 0.0f ? -t : t;
        n.y += n.y >= 0.0f ? -t : t;
        return glm::normalize(n);
    };

    Shader* cs = BindPreSkinProgram();
    if (!cs) { why = "PreSkin.comp.glsl didn't build"; return false; }
    GLuint src = 0, bones = 0, out = 0;
    glCreateBuffers(1, &src);
    glCreateBuffers(1, &bones);
    glCreateBuffers(1, &out);
    glNamedBufferStorage(src, (GLsizeiptr)(verts.size() * sizeof(ModelVertex)), verts.data(), 0);
    glNamedBufferStorage(bones, (GLsizeiptr)(palette.size() * sizeof(glm::mat4)), palette.data(), 0);
    constexpr unsigned kOutBase = 7; // not the buffer's start, as a model after the first lands
    const GLsizeiptr outBytes = ((GLsizeiptr)kOutBase + kVerts) * kSkinnedVertexBytes;
    glNamedBufferStorage(out, outBytes, nullptr, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, bones);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSkinnedBinding, out);
    DispatchPreSkin(*cs, src, (unsigned)kVerts, kOutBase);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    std::vector<std::uint32_t> words((size_t)outBytes / 4);
    glGetNamedBufferSubData(out, 0, outBytes, words.data());
    // Put the bindings the draws rely on back.
    g_BoundOffset = -1;
    g_BoundSize = 0;
    g_SkinnedBound = false;
    glDeleteBuffers(1, &src);
    glDeleteBuffers(1, &bones);
    glDeleteBuffers(1, &out);

    float worstPos = 0.0f, worstDir = 1.0f;
    for (int i = 0; i < kVerts; ++i) {
        glm::vec3 p, n, t;
        cpuSkin(verts[(size_t)i], p, n, t);
        const std::uint32_t* w = &words[(size_t)(kOutBase + (unsigned)i) * 5];
        glm::vec3 gp;
        std::memcpy(&gp, w, sizeof gp);
        worstPos = std::max(worstPos, glm::length(gp - p) / std::max(1.0f, glm::length(p)));
        worstDir = std::min({worstDir, glm::dot(unpackOct(w[3]), n), glm::dot(unpackOct(w[4]), t)});
    }
    if (worstPos > 1e-4f || worstDir < 0.99999f) {
        why = "GPU skinning differs from the CPU's: position error " + std::to_string(worstPos) + ", direction cos " +
              std::to_string(worstDir);
        return false;
    }
    return true;
}
