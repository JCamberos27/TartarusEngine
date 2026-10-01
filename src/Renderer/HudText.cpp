#include "HudText.h"

#include "EnginePaths.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

// stb_truetype comes with Dear ImGui (imstb_truetype.h); the static copy here is this file's own.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma warning(push, 0)
#include "imstb_truetype.h"
#pragma warning(pop)
#pragma warning(disable : 4505) // the bake/render functions this file leaves unused

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

HudText::HudText() = default;

HudText::~HudText() {
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_Atlas) glDeleteTextures(1, &m_Atlas);
}

bool HudText::EnsureResources() {
    if (m_Tried) return !m_Failed;
    m_Tried = true;
    try {
        m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("HudText.vert.glsl"), ShaderLibrary::ReadFile("HudText.frag.glsl"), "HudText");
    } catch (const std::exception& e) {
        Log::Error(std::string("HudText: ") + e.what());
        m_Failed = true;
        return false;
    }
    std::ifstream in(EnginePaths::Resolve("assets/fonts/JetBrainsMono-Regular.ttf"), std::ios::binary);
    std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ttf.empty()) {
        Log::Error("HudText: assets/fonts/JetBrainsMono-Regular.ttf not found");
        m_Failed = true;
        return false;
    }
    std::vector<unsigned char> bitmap((size_t)kAtlas * kAtlas, 0);
    m_Glyphs.assign(sizeof(stbtt_bakedchar) * 95, 0);
    const int rows = stbtt_BakeFontBitmap(ttf.data(), 0, m_BakePx, bitmap.data(), kAtlas, kAtlas - 8, 32, 95,
                                          reinterpret_cast<stbtt_bakedchar*>(m_Glyphs.data()));
    if (rows <= 0) {
        Log::Error("HudText: the font atlas does not fit");
        m_Failed = true;
        return false;
    }
    stbtt_fontinfo fi;
    if (stbtt_InitFont(&fi, ttf.data(), 0)) {
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
        const float sc = stbtt_ScaleForPixelHeight(&fi, m_BakePx);
        m_Ascent = (float)asc * sc;
        m_Descent = -(float)desc * sc;
    } else {
        m_Ascent = m_BakePx * 0.8f;
        m_Descent = m_BakePx * 0.2f;
    }
    // A solid 4x4 white block in the bottom corner for rectangles and lines.
    for (int y = kAtlas - 6; y < kAtlas - 2; ++y)
        for (int x = kAtlas - 6; x < kAtlas - 2; ++x) bitmap[(size_t)y * kAtlas + (size_t)x] = 255;
    m_WhiteU = (float)(kAtlas - 4) / kAtlas;
    m_WhiteV = (float)(kAtlas - 4) / kAtlas;

    glGenTextures(1, &m_Atlas);
    glBindTexture(GL_TEXTURE_2D, m_Atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlas, kAtlas, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);                 // xy, uv
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float))); // rgba
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void HudText::Begin(int width, int height) {
    m_W = width;
    m_H = height;
    m_Scale = std::max(0.75f, (float)height / 1080.0f);
    m_Verts.clear();
}

void HudText::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const glm::vec4& c) {
    const Vertex a{x0, y0, u0, v0, c.r, c.g, c.b, c.a}, b{x1, y0, u1, v0, c.r, c.g, c.b, c.a},
        d{x1, y1, u1, v1, c.r, c.g, c.b, c.a}, e{x0, y1, u0, v1, c.r, c.g, c.b, c.a};
    m_Verts.insert(m_Verts.end(), {a, b, d, a, d, e});
}

float HudText::LineHeight(float size) const { return size * m_Scale * 1.15f; }

float HudText::Measure(const std::string& text, float size) const {
    if (m_Glyphs.empty()) {
        // Not baked yet (first frame): the font is monospaced at about 0.6 em.
        return (float)text.size() * 0.6f * size * m_Scale;
    }
    const auto* g = reinterpret_cast<const stbtt_bakedchar*>(m_Glyphs.data());
    const float k = size * m_Scale / m_BakePx;
    float w = 0.0f;
    for (unsigned char ch : text) w += (ch >= 32 && ch < 127) ? g[ch - 32].xadvance * k : 0.0f;
    return w;
}

void HudText::Glyphs(float x, float y, const std::string& text, float size, const glm::vec4& color) {
    const auto* g = reinterpret_cast<const stbtt_bakedchar*>(m_Glyphs.data());
    const float k = size * m_Scale / m_BakePx;
    const float baseline = y + m_Ascent * k;
    float cx = x;
    for (unsigned char ch : text) {
        if (ch < 32 || ch >= 127) continue;
        const stbtt_bakedchar& b = g[ch - 32];
        const float w = (float)(b.x1 - b.x0), h = (float)(b.y1 - b.y0);
        if (w > 0.0f && h > 0.0f) {
            const float gx = cx + b.xoff * k, gy = baseline + b.yoff * k;
            Quad(gx, gy, gx + w * k, gy + h * k, (float)b.x0 / kAtlas, (float)b.y0 / kAtlas, (float)b.x1 / kAtlas, (float)b.y1 / kAtlas, color);
        }
        cx += b.xadvance * k;
    }
}

float HudText::Text(float x, float y, const std::string& text, float size, const glm::vec4& color, Align align, bool shadow) {
    if (text.empty() || !EnsureResources()) return 0.0f;
    const float w = Measure(text, size);
    if (align == Align::Center) x -= w * 0.5f;
    else if (align == Align::Right) x -= w;
    if (shadow) Glyphs(x + 1.5f * m_Scale, y + 1.5f * m_Scale, text, size, glm::vec4(0.0f, 0.0f, 0.0f, color.a * 0.65f));
    Glyphs(x, y, text, size, color);
    return w;
}

void HudText::Rect(float x, float y, float w, float h, const glm::vec4& color) {
    if (!EnsureResources()) return;
    Quad(x, y, x + w, y + h, m_WhiteU, m_WhiteV, m_WhiteU, m_WhiteV, color);
}

void HudText::Tri(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec4& color) {
    if (!EnsureResources()) return;
    m_Verts.push_back({a.x, a.y, m_WhiteU, m_WhiteV, color.r, color.g, color.b, color.a});
    m_Verts.push_back({b.x, b.y, m_WhiteU, m_WhiteV, color.r, color.g, color.b, color.a});
    m_Verts.push_back({c.x, c.y, m_WhiteU, m_WhiteV, color.r, color.g, color.b, color.a});
}

void HudText::Line(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color) {
    const glm::vec2 d = b - a;
    const float len = glm::length(d);
    if (len < 1e-4f) return;
    const glm::vec2 n = glm::vec2(-d.y, d.x) / len * (0.5f * thickness);
    Tri(a + n, a - n, b - n, color);
    Tri(a + n, b - n, b + n, color);
}

void HudText::Chevron(const glm::vec2& c, float angle, float size, float thickness, const glm::vec4& color) {
    // Screen up is -y; angle clockwise from up.
    const glm::vec2 fwd(std::sin(angle), -std::cos(angle)), side(-fwd.y, fwd.x);
    const glm::vec2 tip = c + fwd * size * 0.5f;
    const glm::vec2 l = c - fwd * size * 0.5f + side * size * 0.6f, r = c - fwd * size * 0.5f - side * size * 0.6f;
    Line(l, tip, thickness, color);
    Line(r, tip, thickness, color);
}

void HudText::Flush(unsigned int dstFbo) {
    if (m_Verts.empty() || m_W <= 0 || m_H <= 0 || !EnsureResources()) {
        m_Verts.clear();
        return;
    }
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, m_W, m_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_Shader->Bind();
    m_Shader->SetVec2("uSize", glm::vec2((float)m_W, (float)m_H));
    m_Shader->SetInt("uAtlas", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_Atlas);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m_Verts.size() * sizeof(Vertex)), m_Verts.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_Verts.size());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    GLStateCache::Invalidate();
    m_Verts.clear();
}
