#include "WorldText.h"

#include "EnginePaths.h"
#include "Frustum.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "World.h"
#include "Components.h"
#include "gl.h"
#include "Core/Profiler.h"

// stb_truetype comes with Dear ImGui (imstb_truetype.h); the static copy here is this file's own (HudText has another).
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma warning(push, 0)
#include "imstb_truetype.h"
#pragma warning(pop)
#pragma warning(disable : 4505) // the bake/render functions this file leaves unused

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace WorldTextLayout {

std::vector<std::uint32_t> DecodeUtf8(const std::string& text) {
    std::vector<std::uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = (unsigned char)text[i];
        int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || i + (size_t)extra >= text.size()) {
            ++i; // a stray continuation byte, or a sequence cut off at the end
            continue;
        }
        std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = (unsigned char)text[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (ok) out.push_back(cp);
        i += ok ? (size_t)extra + 1 : 1;
    }
    return out;
}

const std::vector<std::uint32_t>& Charset() {
    static const std::vector<std::uint32_t> set = [] {
        std::vector<std::uint32_t> s;
        for (std::uint32_t c = 32; c < 127; ++c) s.push_back(c);
        for (std::uint32_t c : {0xB0u /*°*/, 0xD7u /*×*/, 0xB1u /*±*/, 0xB2u /*²*/, 0xB3u /*³*/, 0xB7u /*·*/, 0x2013u /*–*/})
            s.push_back(c);
        std::sort(s.begin(), s.end());
        return s;
    }();
    return set;
}

const Glyph* Font::Find(std::uint32_t cp) const {
    const auto it = std::lower_bound(Codepoints.begin(), Codepoints.end(), cp);
    if (it == Codepoints.end() || *it != cp) return nullptr;
    return &Glyphs[(size_t)(it - Codepoints.begin())];
}

Font Bake(const std::vector<unsigned char>& ttf, int atlasSize, float pixelHeight) {
    Font font;
    stbtt_fontinfo fi;
    if (ttf.empty() || !stbtt_InitFont(&fi, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return font;
    const float scale = stbtt_ScaleForPixelHeight(&fi, pixelHeight);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    font.Ascent = (float)asc * scale / pixelHeight;

    // The edge sits at 128; each texel off it moves the value by 128 / padding, so the field spans `padding`
    // texels either side - room for an outline and for minification.
    const int padding = 8;
    const unsigned char onEdge = 128;
    const float distScale = 128.0f / (float)padding;
    font.Atlas.assign((size_t)atlasSize * (size_t)atlasSize, 0);
    font.AtlasSize = atlasSize;
    int penX = 1, penY = 1, rowH = 0;
    for (std::uint32_t cp : Charset()) {
        Glyph g;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&fi, (int)cp, &adv, &lsb);
        g.Advance = (float)adv * scale / pixelHeight;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* sdf = stbtt_GetCodepointSDF(&fi, scale, (int)cp, padding, onEdge, distScale, &w, &h, &xoff, &yoff);
        if (sdf && w > 0 && h > 0) {
            if (penX + w + 1 > atlasSize) { penX = 1; penY += rowH + 1; rowH = 0; }
            if (penY + h + 1 > atlasSize) {
                stbtt_FreeSDF(sdf, nullptr);
                return Font{}; // doesn't fit
            }
            for (int y = 0; y < h; ++y)
                std::copy(sdf + (size_t)y * (size_t)w, sdf + (size_t)y * (size_t)w + (size_t)w,
                          font.Atlas.begin() + (std::ptrdiff_t)((size_t)(penY + y) * (size_t)atlasSize + (size_t)penX));
            const float a = (float)atlasSize;
            g.Uv = glm::vec4((float)penX / a, (float)penY / a, (float)(penX + w) / a, (float)(penY + h) / a);
            // stb's offsets are pixels from the pen, y down; ours are line heights, y up.
            g.Lo = glm::vec2((float)xoff, -(float)(yoff + h)) / pixelHeight;
            g.Hi = glm::vec2((float)(xoff + w), -(float)yoff) / pixelHeight;
            g.Visible = true;
            penX += w + 1;
            rowH = std::max(rowH, h);
        }
        if (sdf) stbtt_FreeSDF(sdf, nullptr);
        font.Codepoints.push_back(cp);
        font.Glyphs.push_back(g);
    }
    return font;
}

std::vector<Quad> Layout(const Font& font, const std::string& text, int align, glm::vec2* extent) {
    std::vector<Quad> quads;
    if (extent) *extent = glm::vec2(0.0f);
    if (!font.Valid()) return quads;
    const std::vector<std::uint32_t> cps = DecodeUtf8(text);
    const Glyph* fallback = font.Find('?');
    constexpr float kLineStep = 1.1f;

    // Split into lines first: each is aligned on its own width.
    std::vector<std::pair<size_t, size_t>> lines; // [begin, end) into cps
    size_t begin = 0;
    for (size_t i = 0; i <= cps.size(); ++i)
        if (i == cps.size() || cps[i] == '\n') { lines.emplace_back(begin, i); begin = i + 1; }
    const float blockH = (float)lines.size() * kLineStep - (kLineStep - 1.0f);
    float baseline = blockH * 0.5f - font.Ascent;
    float halfW = 0.0f;
    for (const auto& [b, e] : lines) {
        float width = 0.0f;
        for (size_t i = b; i < e; ++i) {
            const Glyph* g = font.Find(cps[i]);
            if (!g) g = fallback;
            if (g) width += g->Advance;
        }
        float pen = align == 0 ? 0.0f : align == 2 ? -width : -0.5f * width;
        halfW = std::max(halfW, align == 1 ? 0.5f * width : width);
        for (size_t i = b; i < e; ++i) {
            const Glyph* g = font.Find(cps[i]);
            if (!g) g = fallback;
            if (!g) continue;
            if (g->Visible)
                quads.push_back({glm::vec2(pen, baseline) + g->Lo, glm::vec2(pen, baseline) + g->Hi, g->Uv});
            pen += g->Advance;
        }
        baseline -= kLineStep;
    }
    if (extent) *extent = glm::vec2(halfW, 0.5f * blockH);
    return quads;
}

float Fade(float distance, float fadeStart, float cullDistance) {
    if (distance >= cullDistance) return 0.0f;
    if (distance <= fadeStart || cullDistance <= fadeStart) return 1.0f;
    const float t = (distance - fadeStart) / (cullDistance - fadeStart);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

} // namespace WorldTextLayout

namespace {
constexpr unsigned kGlyphBinding = 15, kAtlasUnit = 0;
static_assert(sizeof(glm::vec4) * 6 == 96, "std430 layout");
} // namespace

WorldTextRenderer& WorldTextRenderer::Get() {
    static WorldTextRenderer* r = new WorldTextRenderer();
    return *r;
}

bool WorldTextRenderer::EnsureResources() {
    if (m_Tried) return !m_Failed;
    m_Tried = true;
    std::ifstream in(EnginePaths::Resolve("assets/fonts/JetBrainsMono-Regular.ttf"), std::ios::binary);
    const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    m_Font = WorldTextLayout::Bake(ttf);
    if (!m_Font.Valid()) {
        Log::Error("WorldText: could not bake assets/fonts/JetBrainsMono-Regular.ttf");
        m_Failed = true;
        return false;
    }
    try {
        m_Program = std::make_unique<Shader>(ShaderLibrary::ReadFile("WorldText.vert.glsl"), ShaderLibrary::ReadFile("WorldText.frag.glsl"), "WorldText");
    } catch (const std::exception& e) {
        Log::Error(std::string("WorldText: ") + e.what());
        m_Failed = true;
        return false;
    }
    const int n = m_Font.AtlasSize;
    const int levels = 1 + (int)std::floor(std::log2((float)n));
    glCreateTextures(GL_TEXTURE_2D, 1, &m_Atlas);
    glTextureStorage2D(m_Atlas, levels, GL_R8, n, n);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(m_Atlas, 0, 0, 0, n, n, GL_RED, GL_UNSIGNED_BYTE, m_Font.Atlas.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateTextureMipmap(m_Atlas);
    glTextureParameteri(m_Atlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_Atlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_Atlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Atlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Floor markings are seen edge-on: without anisotropy the mip is picked by the squashed axis and the strokes
    // across it melt away.
    GLint maxAniso = 1;
    glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    const GLfloat aniso = (GLfloat)std::clamp(maxAniso, 1, 16);
    glTextureParameterfv(m_Atlas, GL_TEXTURE_MAX_ANISOTROPY, &aniso);
    glCreateVertexArrays(1, &m_Vao);
    m_Font.Atlas.clear();
    m_Font.Atlas.shrink_to_fit(); // on the GPU now
    return true;
}

int WorldTextRenderer::Draw(World& world, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye, bool editorView) {
    m_Stats = {};
    auto& reg = world.Registry;
    const auto labels = reg.view<WorldTextComponent, TransformComponent>();
    if (labels.begin() == labels.end() || !EnsureResources()) return 0;

    // Layouts by text and alignment: labels rarely change, and many share a text ("1 m").
    struct Cached { std::vector<WorldTextLayout::Quad> Quads; glm::vec2 Extent{0.0f}; };
    static std::unordered_map<std::string, Cached> layouts;
    if (layouts.size() > 4096) layouts.clear();

    const Frustum frustum = Frustum::FromViewProj(proj * view);
    const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]), camUp(view[0][1], view[1][1], view[2][1]);
    const float exposure = std::exp2(-world.ExposureEV); // undo the tonemapper's exposure: Brightness is on-screen
    static std::vector<GpuGlyph> onTop;
    m_Glyphs.clear();
    onTop.clear();
    for (const entt::entity e : labels) {
        const auto& t = labels.get<WorldTextComponent>(e);
        if (t.Text.empty() || reg.all_of<InactiveTag>(e)) continue;
        if (editorView && reg.all_of<HiddenInSceneTag>(e)) continue;
        ++m_Stats.Labels;
        const glm::mat4 m = world.GetCachedWorldTransform(e);
        const glm::vec3 anchor(m[3]);
        const float fade = WorldTextLayout::Fade(glm::length(anchor - eye), t.FadeStart, t.CullDistance);
        if (fade <= 0.0f) { ++m_Stats.Culled; continue; }

        const glm::vec3 sx(m[0]), sy(m[1]);
        glm::vec3 right, up;
        if (t.Face == WorldTextComponent::Billboard) {
            right = camRight * glm::length(sx);
            up = camUp * glm::length(sy);
        } else if (t.Face == WorldTextComponent::Upright) {
            glm::vec3 toEye = eye - anchor;
            toEye.y = 0.0f;
            const float len = glm::length(toEye);
            const glm::vec3 r = len > 1e-4f ? glm::normalize(glm::cross(-toEye / len, glm::vec3(0, 1, 0))) : camRight;
            right = r * glm::length(sx);
            up = glm::vec3(0, 1, 0) * glm::length(sy);
        } else {
            right = sx;
            up = sy;
        }
        right *= t.Size;
        up *= t.Size;

        const std::string key = t.Text + char('0' + std::clamp(t.Align, 0, 2));
        auto it = layouts.find(key);
        if (it == layouts.end()) {
            Cached c;
            c.Quads = WorldTextLayout::Layout(m_Font, t.Text, std::clamp(t.Align, 0, 2), &c.Extent);
            it = layouts.emplace(key, std::move(c)).first;
        }
        const Cached& lay = it->second;
        if (lay.Quads.empty()) continue;
        // The block's bounding sphere (left/right alignment hangs it to one side: the half width covers both).
        const float radius = glm::length(right) * lay.Extent.x * 1.05f + glm::length(up) * lay.Extent.y * 1.05f;
        bool inside = true;
        for (const auto& p : frustum.Planes)
            if (p.SignedDistance(anchor) < -radius) { inside = false; break; }
        if (!inside) { ++m_Stats.Culled; continue; }

        const glm::vec3 col = glm::max(t.Color, glm::vec3(0.0f)) * (std::max(t.Brightness, 0.0f) * exposure);
        auto& out = t.OnTop ? onTop : m_Glyphs;
        for (const auto& q : lay.Quads) {
            GpuGlyph g;
            g.Anchor = glm::vec4(anchor, 0.0f);
            g.Right = glm::vec4(right, std::clamp(t.Outline, 0.0f, 0.4f));
            g.Up = glm::vec4(up, 0.0f);
            g.Rect = glm::vec4(q.Lo, q.Hi);
            g.Uv = q.Uv;
            g.Color = glm::vec4(col, fade);
            out.push_back(g);
        }
    }
    const size_t depthTested = m_Glyphs.size();
    m_Glyphs.insert(m_Glyphs.end(), onTop.begin(), onTop.end());
    m_Stats.Glyphs = (int)m_Glyphs.size();
    if (m_Glyphs.empty()) return 0;
    PROFILE_GPU_SCOPE("World Text");

    if (m_Glyphs.size() > m_Capacity) {
        if (m_Buffer) glDeleteBuffers(1, &m_Buffer);
        m_Capacity = std::max<size_t>(m_Glyphs.size() * 2, 1024);
        glCreateBuffers(1, &m_Buffer);
        glNamedBufferStorage(m_Buffer, (GLsizeiptr)(m_Capacity * sizeof(GpuGlyph)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    glNamedBufferSubData(m_Buffer, 0, (GLsizeiptr)(m_Glyphs.size() * sizeof(GpuGlyph)), m_Glyphs.data());

    Shader& prog = *m_Program;
    prog.Bind();
    prog.SetMat4("uViewProj", proj * view);
    prog.SetInt("uAtlas", (int)kAtlasUnit);
    glBindTextureUnit(kAtlasUnit, m_Atlas);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kGlyphBinding, m_Buffer);
    glBindVertexArray(m_Vao);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied
    // Floor and wall markings sit on their surface: pulled towards the eye so they don't fight it for depth.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -4.0f);
    int draws = 0;
    if (depthTested > 0) {
        glEnable(GL_DEPTH_TEST);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)depthTested, 0);
        ++draws;
    }
    if (m_Glyphs.size() > depthTested) {
        glDisable(GL_DEPTH_TEST);
        glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, (GLsizei)(m_Glyphs.size() - depthTested), (GLuint)depthTested);
        ++draws;
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    GLStateCache::Invalidate();
    return draws;
}
