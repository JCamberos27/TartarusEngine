#include "WeaponFxRenderer.h"

#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kStride = 10;              // pos3 uv2 color4 kind1
constexpr int kBeamSegments = 40;        // packed toward the emitter, where it's brightest
constexpr float kBeamRange = 150.0f;     // metres drawn; past that it's gone in the haze
constexpr float kBeamHalfWidth = 0.0015f; // a 3 mm beam
constexpr float kBeamFalloff = 2.5f;     // metres: the glow near the emitter
constexpr float kBeamBend = 4.0f;        // metres over which a view-model emitter eases onto the true path
constexpr float kSpotCore = 0.004f;      // metres: the dot's radius
constexpr float kSpotQuad = 6.0f;        // the quad's half-size, in core radii (the halo)
constexpr float kHoleQuad = 2.5f;        // in hole radii (the darkened ring)
} // namespace

WeaponFxRenderer::WeaponFxRenderer() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("WeaponFx.vert.glsl"),
                                        ShaderLibrary::ReadFile("WeaponFx.frag.glsl"), "WeaponFx");
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    const GLsizei stride = kStride * (GLsizei)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(9 * sizeof(float)));
    glBindVertexArray(0);
}

WeaponFxRenderer::~WeaponFxRenderer() {
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void WeaponFxRenderer::AddBeam(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color, float emitterScale) {
    if (glm::length(to - from) > 1e-3f) m_Beams.push_back({from, to, color, emitterScale});
}

void WeaponFxRenderer::AddSpot(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& color) {
    if (glm::length(normal) > 0.5f) m_Spots.push_back({center, glm::normalize(normal), glm::vec3(0.0f), color, kSpotCore, 0.0f});
}

void WeaponFxRenderer::AddHole(const glm::vec3& center, const glm::vec3& normal, const glm::vec3& tangent, float radius,
                               float seed) {
    if (glm::length(normal) > 0.5f && radius > 0.0f)
        m_Holes.push_back({center, glm::normalize(normal), tangent, glm::vec3(0.015f, 0.013f, 0.012f), radius, seed});
}

void WeaponFxRenderer::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, int viewportHeight,
                            float time) {
    std::vector<float>& V = m_Verts;
    V.clear();
    auto vert = [&](const glm::vec3& p, float u, float v, const glm::vec3& c, float a, float kind) {
        V.insert(V.end(), {p.x, p.y, p.z, u, v, c.r, c.g, c.b, a, kind});
    };
    // Metres a pixel spans at `p`: the floor for things that must never go sub-pixel.
    const float pxScale = 2.0f / (std::max(std::abs(proj[1][1]), 1e-4f) * (float)std::max(viewportHeight, 1));
    auto pixelAt = [&](const glm::vec3& p) { return glm::length(p - eye) * pxScale; };

    // A square on the surface, lifted just off it (more with distance, as depth precision drops).
    auto decal = [&](const Decal& d, float halfSize, float kind, float alpha) {
        glm::vec3 t = d.Tangent;
        if (glm::length(t) < 0.5f)
            t = std::abs(d.Normal.y) < 0.9f ? glm::cross(d.Normal, glm::vec3(0, 1, 0)) : glm::cross(d.Normal, glm::vec3(1, 0, 0));
        t = glm::normalize(t - d.Normal * glm::dot(t, d.Normal));
        const glm::vec3 b = glm::cross(d.Normal, t);
        const glm::vec3 c = d.Center + d.Normal * (0.001f + glm::length(d.Center - eye) * 0.0004f);
        const glm::vec3 p00 = c - t * halfSize - b * halfSize, p10 = c + t * halfSize - b * halfSize;
        const glm::vec3 p11 = c + t * halfSize + b * halfSize, p01 = c - t * halfSize + b * halfSize;
        vert(p00, -1, -1, d.Color, alpha, kind); vert(p10, 1, -1, d.Color, alpha, kind); vert(p11, 1, 1, d.Color, alpha, kind);
        vert(p00, -1, -1, d.Color, alpha, kind); vert(p11, 1, 1, d.Color, alpha, kind); vert(p01, -1, 1, d.Color, alpha, kind);
    };

    // Holes first (alpha-blended, darkening), then the light (additive) over them.
    for (const Decal& h : m_Holes) decal(h, h.Radius * kHoleQuad, 2.0f, h.Seed);
    const size_t holeVerts = V.size() / kStride;

    const glm::mat4 viewInv = glm::inverse(view);
    for (const Beam& beam : m_Beams) {
        const glm::vec3 dir = glm::normalize(beam.To - beam.From);
        const float length = std::min(glm::length(beam.To - beam.From), kBeamRange);
        const float bend = std::min(kBeamBend, length);
        glm::vec3 prevL(0.0f), prevR(0.0f);
        float prevD = 0.0f, prevA = 0.0f;
        for (int i = 0; i <= kBeamSegments; ++i) {
            const float s = (float)i / (float)kBeamSegments;
            const float d = length * s * s;
            glm::vec3 p = beam.From + dir * d;
            if (beam.EmitterScale != 1.0f && bend > 0.0f) {
                const float t = std::clamp(d / bend, 0.0f, 1.0f);
                glm::vec4 inView = view * glm::vec4(p, 1.0f);
                const float f = beam.EmitterScale + (1.0f - beam.EmitterScale) * (t * t * (3.0f - 2.0f * t));
                inView.x *= f;
                inView.y *= f;
                p = glm::vec3(viewInv * inView);
            }
            glm::vec3 side = glm::cross(dir, eye - p);
            const float sl = glm::length(side);
            side = sl > 1e-7f ? side / sl : glm::vec3(0.0f);
            // Never thinner than about a pixel and a half; spread that wide it dims to match, so
            // far off it fades out instead of turning into a thick red line.
            const float half = std::max(kBeamHalfWidth, 0.75f * pixelAt(p));
            const float energy = kBeamHalfWidth / half;
            const float glow = 0.35f + 0.65f * std::exp(-d / kBeamFalloff);
            const float a = energy * glow;
            const glm::vec3 L = p - side * half, R = p + side * half;
            if (i > 0) {
                vert(prevL, prevD, -1, beam.Color, prevA, 0); vert(prevR, prevD, 1, beam.Color, prevA, 0); vert(R, d, 1, beam.Color, a, 0);
                vert(prevL, prevD, -1, beam.Color, prevA, 0); vert(R, d, 1, beam.Color, a, 0); vert(L, d, -1, beam.Color, a, 0);
            }
            prevL = L; prevR = R; prevD = d; prevA = a;
        }
    }
    for (const Decal& s : m_Spots) {
        const float core = std::max(s.Radius, 1.3f * pixelAt(s.Center));
        decal(s, core * kSpotQuad, 1.0f, 1.0f);
    }
    m_Beams.clear();
    m_Spots.clear();
    m_Holes.clear();
    if (V.empty()) return;

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    if (V.size() > m_Capacity) {
        glBufferData(GL_ARRAY_BUFFER, V.size() * sizeof(float), V.data(), GL_DYNAMIC_DRAW);
        m_Capacity = V.size();
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, V.size() * sizeof(float), V.data());
    }
    m_Shader->Bind();
    m_Shader->SetMat4("uViewProj", proj * view);
    m_Shader->SetFloat("uTime", time);

    const GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);
    GLint prevDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -2.0f);
    const GLsizei total = (GLsizei)(V.size() / kStride);
    if (holeVerts > 0) {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)holeVerts);
    }
    if ((GLsizei)holeVerts < total) {
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        glDrawArrays(GL_TRIANGLES, (GLint)holeVerts, total - (GLsizei)holeVerts);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
    glDepthFunc((GLenum)prevDepthFunc);
    glDepthMask(GL_TRUE);
    if (cullWasOn) glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    GLStateCache::Invalidate(); // raw program/VAO/buffer binds above bypass the cache
}
