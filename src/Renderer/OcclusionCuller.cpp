#include "OcclusionCuller.h"
#include "gl.h"
#include "Shader.h"
#include "GLStateCache.h"
#include "ProjectSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
// A visible object is re-tested this often (frames); a hidden one every frame (its draws wait on the test).
constexpr std::uint32_t kRetestFrames = 8;
// A camera cut: this far, or this much of a turn, between two frames of one view.
constexpr float kCutMetres = 2.0f;
constexpr float kCutCos = 0.866f; // 30 degrees
// Results nobody has read for this long belong to objects gone from view: dropped now and then.
constexpr std::uint32_t kForgetFrames = 600;

const char* kBoxVs = R"(#version 450 core
layout(location = 0) in vec3 aMin;
layout(location = 1) in vec3 aMax;
uniform mat4 uViewProj;
// The box's 12 triangles over its corners (bit 0 = x, bit 1 = y, bit 2 = z at the max side). Drawn without culling.
const int kCorner[36] = int[36](0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5);
void main() {
    int c = kCorner[gl_VertexID];
    vec3 t = vec3(float(c & 1), float((c >> 1) & 1), float((c >> 2) & 1));
    gl_Position = uViewProj * vec4(mix(aMin, aMax, t), 1.0);
}
)";
const char* kBoxFs = R"(#version 450 core
void main() {}
)";

struct BoxResources {
    std::unique_ptr<Shader> Program;
    unsigned Vao = 0, Vbo = 0;
    int ViewProjLoc = -1;
    bool Failed = false;
};
// Never destroyed: the GL context is gone by the time statics are.
BoxResources& Box() {
    static BoxResources* r = new BoxResources();
    return *r;
}
bool EnsureBox() {
    BoxResources& r = Box();
    if (r.Program || r.Failed) return !r.Failed;
    try {
        r.Program = std::make_unique<Shader>(kBoxVs, kBoxFs, "OcclusionBox");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Occlusion] box program failed, culling off: %s\n", e.what());
        r.Program.reset();
    }
    if (!r.Program || r.Program->Program() == 0) {
        r.Failed = true;
        return false;
    }
    r.ViewProjLoc = r.Program->Loc("uViewProj");
    glCreateBuffers(1, &r.Vbo);
    glCreateVertexArrays(1, &r.Vao);
    glVertexArrayVertexBuffer(r.Vao, 0, r.Vbo, 0, 6 * sizeof(float));
    glVertexArrayBindingDivisor(r.Vao, 0, 1);
    for (unsigned a = 0; a < 2; ++a) {
        glEnableVertexArrayAttrib(r.Vao, a);
        glVertexArrayAttribFormat(r.Vao, a, 3, GL_FLOAT, GL_FALSE, a * 3 * sizeof(float));
        glVertexArrayAttribBinding(r.Vao, a, 0);
    }
    return true;
}

bool EnvOff() {
    static const bool off = [] {
        char* e = nullptr;
        size_t n = 0;
        bool v = false;
        if (_dupenv_s(&e, &n, "TARTARUS_OCCLUSION") == 0 && e) v = std::strcmp(e, "0") == 0;
        std::free(e);
        return v;
    }();
    return off;
}
} // namespace

bool OcclusionCuller::Enabled() {
    return ProjectSettings::Quality().OcclusionCulling && ProjectSettings::Quality().DepthPrepass && !EnvOff() &&
           glBeginConditionalRender && glEndConditionalRender && !Box().Failed;
}

OcclusionCuller* OcclusionCuller::ForView(bool editorView, bool ownerView) {
    static OcclusionCuller* views[2] = {new OcclusionCuller(), new OcclusionCuller()};
    if (editorView) return views[0];
    if (ownerView) return views[1];
    return nullptr;
}

bool OcclusionCuller::NearEye(const glm::vec3& wmin, const glm::vec3& wmax, const glm::vec3& eye, float margin) {
    const glm::vec3 d = glm::max(glm::max(wmin - eye, eye - wmax), glm::vec3(0.0f));
    return glm::dot(d, d) < margin * margin;
}

bool OcclusionCuller::IsCut(const glm::vec3& lastEye, const glm::vec3& lastFwd, const glm::vec3& eye, const glm::vec3& fwd) {
    const glm::vec3 moved = eye - lastEye;
    return glm::dot(moved, moved) > kCutMetres * kCutMetres || glm::dot(lastFwd, fwd) < kCutCos;
}

void OcclusionCuller::PadBounds(glm::vec3& wmin, glm::vec3& wmax) {
    // A hair larger than the object: the boxes and the depth program round positions differently.
    const glm::vec3 pad = (wmax - wmin) * 0.01f + glm::vec3(0.02f);
    wmin -= pad;
    wmax += pad;
}

unsigned OcclusionCuller::TakeQuery() {
    if (m_FreeQueries.empty()) {
        m_FreeQueries.resize(256);
        glGenQueries(256, m_FreeQueries.data());
    }
    const unsigned q = m_FreeQueries.back();
    m_FreeQueries.pop_back();
    return q;
}

void OcclusionCuller::BeginFrame(const void* world, const glm::vec3& eye, const glm::vec3& forward, float nearZ) {
    ++m_Frame;
    m_Eye = eye;
    // The near plane clips a box the eye is about to enter; such a box is never predicted hidden.
    m_NearMargin = 0.25f + 4.0f * nearZ;

    // Results back from the GPU, in issue order, without waiting for any.
    m_Revealed = 0;
    while (m_PendingHead < m_Pending.size()) {
        const Pending& p = m_Pending[m_PendingHead];
        GLint ready = 0;
        glGetQueryObjectiv(p.Query, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) break;
        GLint passed = 0;
        glGetQueryObjectiv(p.Query, GL_QUERY_RESULT, &passed);
        if (p.Epoch == m_Epoch) {
            if (auto it = m_State.find(p.Key); it != m_State.end()) it->second.Hidden = passed == 0;
            if (p.Suspect && passed) ++m_Revealed;
        }
        m_FreeQueries.push_back(p.Query);
        ++m_PendingHead;
    }
    if (m_PendingHead == m_Pending.size()) {
        m_Pending.clear();
        m_PendingHead = 0;
    } else if (m_PendingHead > 4096) {
        m_Pending.erase(m_Pending.begin(), m_Pending.begin() + (std::ptrdiff_t)m_PendingHead);
        m_PendingHead = 0;
    }

    // A cut (or another world): what was hidden from the old view says nothing about the new one.
    const glm::vec3 fwd = glm::dot(forward, forward) > 1e-8f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    if (world != m_World || (m_HaveLast && IsCut(m_LastEye, m_LastFwd, eye, fwd))) {
        m_State.clear();
        ++m_Epoch;
    }
    m_World = world;
    m_LastEye = eye;
    m_LastFwd = fwd;
    m_HaveLast = true;

    if (m_Frame % kForgetFrames == 0)
        for (auto it = m_State.begin(); it != m_State.end();)
            it = m_Frame - it->second.TestedFrame > kForgetFrames ? m_State.erase(it) : std::next(it);
}

bool OcclusionCuller::Suspect(std::uint32_t key, const glm::vec3& wmin, const glm::vec3& wmax) const {
    const auto it = m_State.find(key);
    if (it == m_State.end() || !it->second.Hidden) return false;
    glm::vec3 lo = wmin, hi = wmax;
    PadBounds(lo, hi);
    return !NearEye(lo, hi, m_Eye, m_NearMargin);
}

void OcclusionCuller::Test(const std::vector<Candidate>& candidates, const glm::mat4& viewProj, std::vector<unsigned>& queries) {
    queries.assign(candidates.size(), 0u);
    if (candidates.empty() || !EnsureBox()) return;

    // Which candidates are tested: every suspect, every object without a result, and the visible ones due again.
    static std::vector<std::uint32_t> tested;
    static std::vector<float> boxes;
    tested.clear();
    boxes.clear();
    for (std::uint32_t i = 0; i < (std::uint32_t)candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        glm::vec3 lo = c.Min, hi = c.Max;
        PadBounds(lo, hi);
        if (NearEye(lo, hi, m_Eye, m_NearMargin)) continue; // drawn: the eye is at it
        auto [it, fresh] = m_State.try_emplace(c.Key);
        Entry& e = it->second;
        if (fresh) {
            // Spread the re-tests of a view's worth of new objects over the period, not all on one frame.
            e.TestedFrame = m_Frame - ((c.Key * 2654435761u) >> 16) % kRetestFrames;
        } else if (!c.Suspect && m_Frame - e.TestedFrame < kRetestFrames) {
            continue;
        }
        if (!fresh) e.TestedFrame = m_Frame;
        tested.push_back(i);
        boxes.insert(boxes.end(), {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z});
    }
    if (tested.empty()) return;

    BoxResources& r = Box();
    glNamedBufferData(r.Vbo, (GLsizeiptr)(boxes.size() * sizeof(float)), boxes.data(), GL_DYNAMIC_DRAW);

    GLint depthFunc = GL_LEQUAL;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    GLboolean depthWrites = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrites);
    const bool cull = glIsEnabled(GL_CULL_FACE);
    const bool offset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    glDepthFunc(GL_LEQUAL); // a box face level with the depth counts as showing
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); // a box seen from inside its far faces still tests
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_DEPTH_CLAMP); // and one past the far plane is not clipped away
    r.Program->Bind();
    glUniformMatrix4fv(r.ViewProjLoc, 1, GL_FALSE, &viewProj[0][0]);
    GLStateCache::BindVertexArray(r.Vao);

    for (std::uint32_t b = 0; b < (std::uint32_t)tested.size(); ++b) {
        const std::uint32_t i = tested[b];
        const unsigned q = TakeQuery();
        glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 36, 1, b);
        glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
        m_Pending.push_back({q, candidates[i].Key, m_Epoch, candidates[i].Suspect});
        if (candidates[i].Suspect) queries[i] = q;
    }

    glDisable(GL_DEPTH_CLAMP);
    if (offset) glEnable(GL_POLYGON_OFFSET_FILL);
    if (cull) glEnable(GL_CULL_FACE);
    glDepthMask(depthWrites);
    glDepthFunc((GLenum)depthFunc);
}
