#include "GraffitiCompositor.h"
#include "GLFramebufferCheck.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace Graffiti {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kMaxImageSide = 1024; // letters and stamps are downsampled to this (a 2K glyph is 16 MB)
constexpr int kBakeLevels = 9;      // composite colour mips, for the bake's push-pull edge padding

// The GL state a compositor pass touches, put back on scope exit.
struct StateGuard {
    GLint Fbo = 0, Viewport[4] = {};
    GLboolean Blend, Depth, Scissor, Cull, Srgb;
    StateGuard() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &Fbo);
        glGetIntegerv(GL_VIEWPORT, Viewport);
        Blend = glIsEnabled(GL_BLEND);
        Depth = glIsEnabled(GL_DEPTH_TEST);
        Scissor = glIsEnabled(GL_SCISSOR_TEST);
        Cull = glIsEnabled(GL_CULL_FACE);
        Srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_BLEND);
    }
    ~StateGuard() {
        // glBlendFunci (per attachment) goes behind the render-state shadow (gl.h): forget what it
        // believes so the restores below really reach the driver.
        GLStateShadow_Invalidate();
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)Fbo);
        glViewport(Viewport[0], Viewport[1], Viewport[2], Viewport[3]);
        auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
        set(GL_BLEND, Blend);
        set(GL_DEPTH_TEST, Depth);
        set(GL_SCISSOR_TEST, Scissor);
        set(GL_CULL_FACE, Cull);
        set(GL_FRAMEBUFFER_SRGB, Srgb);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(0);
        GLStateCache::Invalidate();
    }
    StateGuard(const StateGuard&) = delete;
    StateGuard& operator=(const StateGuard&) = delete;
};

unsigned int MakeTex(GLenum format, int w, int h, int levels = 1) {
    unsigned int t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, levels, format, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

unsigned int MakeFbo(const char* owner, const unsigned int* colors, int n, int w, int h) {
    unsigned int f = 0;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    GLenum bufs[4];
    for (int i = 0; i < n; ++i) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, colors[i], 0);
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    glDrawBuffers(n, bufs);
    GLFramebufferCheck::Complete(owner, f, w, h);
    return f;
}

void ClearTarget(unsigned int fbo, int attachments) {
    static const float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (int i = 0; i < attachments; ++i) glClearBufferfv(GL_COLOR, i, kZero);
}

void BindTex(int unit, unsigned int tex) { GLStateCache::BindTexture2D((unsigned)unit, tex); }

glm::vec2 GradientDir(float degrees) {
    const float a = degrees * (kPi / 180.0f);
    return {std::cos(a), std::sin(a)};
}

void SetGradient(const Shader& s, const Layer& l) {
    s.SetInt("uGradientOn", l.Gradient ? 1 : 0);
    s.SetVec3("uGradientA", SrgbToLinear(l.Color));
    s.SetVec3("uGradientB", SrgbToLinear(l.GradientTo));
    s.SetVec2("uGradientDir", GradientDir(l.GradientAngle));
}

void AppendBytes(void* context, void* data, int size) { static_cast<std::string*>(context)->append((const char*)data, (size_t)size); }

} // namespace

Compositor::Compositor() = default;
Compositor::~Compositor() { Release(); }

void Compositor::EnsureGl() {
    if (m_Splat) return;
    const std::string fullscreen = ShaderLibrary::ReadFile("ScreenBlur.vert.glsl");
    m_Splat = std::make_unique<Shader>(ShaderLibrary::ReadFile("GraffitiSplat.vert.glsl"),
                                       ShaderLibrary::ReadFile("GraffitiSplat.frag.glsl"), "Graffiti Splat");
    m_Quad = std::make_unique<Shader>(ShaderLibrary::ReadFile("GraffitiQuad.vert.glsl"),
                                      ShaderLibrary::ReadFile("GraffitiQuad.frag.glsl"), "Graffiti Quad");
    m_Fill = std::make_unique<Shader>(fullscreen, ShaderLibrary::ReadFile("GraffitiFill.frag.glsl"), "Graffiti Fill");
    m_Blur = std::make_unique<Shader>(fullscreen, ShaderLibrary::ReadFile("GraffitiBlur.frag.glsl"), "Graffiti Blur");
    m_Composite = std::make_unique<Shader>(fullscreen, ShaderLibrary::ReadFile("GraffitiComposite.frag.glsl"), "Graffiti Composite");
    m_Bake = std::make_unique<Shader>(fullscreen, ShaderLibrary::ReadFile("GraffitiBake.frag.glsl"), "Graffiti Bake");
    m_PreviewShader = std::make_unique<Shader>(fullscreen, ShaderLibrary::ReadFile("GraffitiPreview.frag.glsl"), "Graffiti Preview");

    glGenVertexArrays(1, &m_EmptyVao);
    glGenVertexArrays(1, &m_SplatVao);
    glGenBuffers(1, &m_SplatVbo);
    glBindVertexArray(m_SplatVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_SplatVbo);
    const GLsizei stride = sizeof(Splat);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Splat, Shape));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Splat, Params));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Splat, Color));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 2, GL_UNSIGNED_INT, stride, (void*)offsetof(Splat, Seed));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Splat, Time));
    for (GLuint a = 0; a < 5; ++a) glVertexAttribDivisor(a, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Compositor::EnsureTargets(int w, int h) {
    if (m_W == w && m_H == h && m_Layer.Fbo) return;
    FreeTargets();
    m_W = w;
    m_H = h;
    auto pair = [&](const char* owner, GLenum extraFormat, int levels) {
        Target t;
        t.Color = MakeTex(GL_RGBA16F, w, h, levels);
        t.Extra = MakeTex(extraFormat, w, h);
        const unsigned int c[2] = {t.Color, t.Extra};
        t.Fbo = MakeFbo(owner, c, 2, w, h);
        return t;
    };
    m_Layer = pair("Graffiti layer", GL_R16F, 1);
    m_LiveT = pair("Graffiti live layer", GL_R16F, 1);
    m_Below = pair("Graffiti below", GL_RGBA16F, 1);
    m_C[0] = pair("Graffiti composite 0", GL_RGBA16F, kBakeLevels);
    m_C[1] = pair("Graffiti composite 1", GL_RGBA16F, kBakeLevels);
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
    for (int i = 0; i < 4; ++i) {
        m_BlurTex[i] = MakeTex(GL_R16F, hw, hh);
        m_BlurFbo[i] = MakeFbo("Graffiti blur", &m_BlurTex[i], 1, hw, hh);
    }
    m_Preview = MakeTex(GL_RGBA8, w, h);
    m_Overlay = MakeTex(GL_RGBA8, w, h);
    const unsigned int previews[2] = {m_Preview, m_Overlay};
    m_PreviewFbo = MakeFbo("Graffiti preview", previews, 2, w, h);
    m_Out[0] = MakeTex(GL_RGBA8, w, h);
    m_Out[1] = MakeTex(GL_RGBA8, w, h);
    m_Out[2] = MakeTex(GL_RGBA8, w, h);
    m_Out[3] = MakeTex(GL_R8, w, h);
    m_OutFbo = MakeFbo("Graffiti bake", m_Out, 4, w, h);
    glGenBuffers(4, m_Pbo);
    for (int i = 0; i < 4; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_Pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * (i == 3 ? 1 : 4), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Compositor::FreeTargets() {
    FinishInFlight(); // the PBOs go with the targets: a bake still reading must land first, not be lost
    m_Composited = false;
    for (Target* t : {&m_Layer, &m_LiveT, &m_Below, &m_C[0], &m_C[1]}) {
        if (t->Fbo) glDeleteFramebuffers(1, &t->Fbo);
        const unsigned int tex[2] = {t->Color, t->Extra};
        if (t->Color) glDeleteTextures(2, tex);
        *t = Target{};
    }
    if (m_BlurFbo[0]) {
        glDeleteFramebuffers(4, m_BlurFbo);
        glDeleteTextures(4, m_BlurTex);
    }
    if (m_PreviewFbo) {
        glDeleteFramebuffers(1, &m_PreviewFbo);
        const unsigned int previews[2] = {m_Preview, m_Overlay};
        glDeleteTextures(2, previews);
    }
    if (m_OutFbo) {
        glDeleteFramebuffers(1, &m_OutFbo);
        glDeleteTextures(4, m_Out);
        glDeleteBuffers(4, m_Pbo);
    }
    std::memset(m_BlurFbo, 0, sizeof(m_BlurFbo));
    std::memset(m_BlurTex, 0, sizeof(m_BlurTex));
    std::memset(m_Out, 0, sizeof(m_Out));
    std::memset(m_Pbo, 0, sizeof(m_Pbo));
    m_PreviewFbo = m_Preview = m_Overlay = m_OutFbo = 0;
    m_W = m_H = 0;
    m_LiveLayer = -1;
}

void Compositor::ForgetImages() {
    ++m_ImageEpoch;
    for (auto& [path, img] : m_Images)
        if (img.Tex) glDeleteTextures(1, &img.Tex);
    m_Images.clear();
}

void Compositor::Release() {
    FreeTargets();
    ForgetImages();
    if (m_SplatVbo) glDeleteBuffers(1, &m_SplatVbo);
    if (m_SplatVao) glDeleteVertexArrays(1, &m_SplatVao);
    if (m_EmptyVao) glDeleteVertexArrays(1, &m_EmptyVao);
    m_SplatVbo = m_SplatVao = m_EmptyVao = 0;
    m_SplatCapacity = 0;
    m_Splat.reset();
    m_Quad.reset();
    m_Fill.reset();
    m_Blur.reset();
    m_Composite.reset();
    m_Bake.reset();
    m_PreviewShader.reset();
}

size_t Compositor::GpuBytes() const {
    if (!m_W) return 0;
    const size_t px = (size_t)m_W * m_H;
    size_t b = px * (8 + 2) * 2;           // layer and live: RGBA16F + R16F
    b += px * (8 + 8);                     // below
    b += px * (8 * 4 / 3 + 8) * 2;         // the composites, colour mipped
    b += px / 4 * 2 * 4;                   // four half-size R16F blurs
    b += px * 4 * 2;                       // preview and overlay
    b += px * (4 + 4 + 4 + 1) * 2;         // bake targets and their PBOs
    return b;
}

// --- Images ----------------------------------------------------------------------------------------

const Compositor::Image& Compositor::LoadImage(const std::string& path) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    bool retry = false;
    auto it = m_Images.find(path);
    if (it != m_Images.end()) {
        // A failed read is tried again after a short while (the file may have appeared or been fixed).
        if (it->second.Tex || it->second.M.Valid || it->second.FailedAt < 0.0 || now - it->second.FailedAt < 2.0) return it->second;
        m_Images.erase(it);
        retry = true;
    }
    Image& img = m_Images[path];
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px) {
        img.FailedAt = now;
        Log::Warn("Graffiti: couldn't read the image '" + path + "'.", LogContext::Asset(path));
        return img;
    }
    // Halve (2x2 box) until it fits: letters are never drawn near their native 2K.
    std::vector<unsigned char> buf(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    while (std::max(w, h) > kMaxImageSide && w > 1 && h > 1) {
        const int nw = w / 2, nh = h / 2;
        std::vector<unsigned char> half((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const auto at = [&](int xx, int yy) { return (int)buf[((size_t)yy * w + xx) * 4 + c]; };
                    half[((size_t)y * nw + x) * 4 + c] =
                        (unsigned char)((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        buf.swap(half);
        w = nw;
        h = nh;
    }
    // Ink box from the coverage: alpha when the image has any, else luminance (white-on-black).
    bool hasAlpha = false;
    for (size_t i = 3; i < buf.size() && !hasAlpha; i += 4) hasAlpha = buf[i] < 250;
    int left = w, right = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = &buf[((size_t)y * w + x) * 4];
            const int cover = hasAlpha ? p[3] : (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
            if (cover > 32) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    img.Width = w;
    img.Height = h;
    img.M.Valid = true;
    img.M.Aspect = (float)w / (float)std::max(h, 1);
    img.M.InkLeft = right >= left ? (float)left / (float)w : 0.0f;
    img.M.InkRight = right >= left ? (float)(right + 1) / (float)w : 1.0f;

    const int levels = 1 + (int)std::floor(std::log2((float)std::max(w, h)));
    glGenTextures(1, &img.Tex);
    glBindTexture(GL_TEXTURE_2D, img.Tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Past its edge an image is transparent, so a letter's outline mip fades out round it.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    static const float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, kClear);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLStateCache::Invalidate();
    if (retry) ++m_ImageEpoch; // it failed before and reads now: programs built on its absence are stale
    return img;
}

void Compositor::RetryFailedImages() {
    std::vector<std::string> due;
    for (const auto& [path, img] : m_Images)
        if (!img.Tex && !img.M.Valid && img.FailedAt >= 0.0) due.push_back(path);
    for (const std::string& path : due) LoadImage(path); // (only re-reads those past the delay)
}

ImageMetrics Compositor::Metrics(const std::string& absolutePath) { return LoadImage(absolutePath).M; }

glm::vec3 Compositor::SamplePreview(const glm::vec2& uv) {
    if (!m_PreviewFbo) return glm::vec3(0.0f);
    StateGuard guard;
    const int x = std::clamp((int)(uv.x * (float)m_W), 0, m_W - 1);
    const int y = std::clamp((int)((1.0f - uv.y) * (float)m_H), 0, m_H - 1); // uv y down, GL rows up
    unsigned char px[4] = {};
    glBindFramebuffer(GL_FRAMEBUFFER, m_PreviewFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    return glm::vec3(px[0], px[1], px[2]) / 255.0f;
}

MetricsFn Compositor::MetricsCallback() {
    return [this](const std::string& path) { return Metrics(path); };
}

// --- Drawing ---------------------------------------------------------------------------------------

void Compositor::Fullscreen() {
    glBindVertexArray(m_EmptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Compositor::DrawSplats(const Splat* splats, size_t count, bool erase) {
    if (!count) return;
    glBindBuffer(GL_ARRAY_BUFFER, m_SplatVbo);
    if (count > m_SplatCapacity) {
        m_SplatCapacity = std::max(count, m_SplatCapacity * 2);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m_SplatCapacity * sizeof(Splat)), nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * sizeof(Splat)), splats);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    if (erase) {
        glBlendFunci(0, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied over
        glBlendFunci(1, GL_ONE, GL_ONE);                 // thickness piles up
    }
    m_Splat->Bind();
    m_Splat->SetVec2("uCanvas", glm::vec2((float)m_W, (float)m_H));
    m_Splat->SetFloat("uDotPx", std::max(1.0f, (float)m_W / 1024.0f));
    glBindVertexArray(m_SplatVao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count);
    glDisable(GL_BLEND);
}

void Compositor::DrawLayer(const Piece& piece, const LayerProgram& lp, const Target& into) {
    const Layer& l = piece.Layers[(size_t)lp.Layer];
    ClearTarget(into.Fbo, 2);
    glViewport(0, 0, m_W, m_H);
    const glm::vec2 canvas((float)m_W, (float)m_H);
    if (l.Type == LayerType::Fill) {
        m_Fill->Bind();
        m_Fill->SetVec2("uCanvas", canvas);
        m_Fill->SetVec3("uFill", SrgbToLinear(l.Color));
        SetGradient(*m_Fill, l);
        Fullscreen();
    } else if (!lp.Quads.empty()) {
        m_Quad->Bind();
        m_Quad->SetVec2("uCanvas", canvas);
        m_Quad->SetInt("uImage", 0);
        SetGradient(*m_Quad, l);
        glEnable(GL_BLEND);
        glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendFunci(1, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // a letter over another covers its relief
        glBindVertexArray(m_EmptyVao);
        for (const Quad& q : lp.Quads) {
            if (q.Reveal <= 0.0f || q.Texture < 0) continue;
            // (Program::Textures is shared by the whole piece; the layer's quads index it.)
            const Image* img = nullptr;
            if (const auto it = m_Images.find(m_TextureNames[(size_t)q.Texture]); it != m_Images.end()) img = &it->second;
            if (!img || !img->Tex) continue;
            BindTex(0, img->Tex);
            const float scale = 0.6f + 0.4f * q.Reveal;
            m_Quad->SetVec2("uCentre", q.Centre);
            m_Quad->SetVec2("uHalf", q.Half * scale);
            m_Quad->SetFloat("uAngle", q.Angle);
            m_Quad->SetFloat("uGrow", q.OutlinePx * 1.6f);
            m_Quad->SetVec3("uFill", q.Fill);
            m_Quad->SetVec3("uOutline", q.OutlineColor);
            // The mip whose texels are about the outline width blurs the coverage out that far.
            const float texelsPerPx = (float)img->Width / std::max(2.0f * q.Half.x * scale, 1.0f);
            m_Quad->SetFloat("uOutlineLod", q.OutlinePx > 0.0f ? std::log2(std::max(q.OutlinePx * texelsPerPx * 1.5f, 1.0f)) : -1.0f);
            m_Quad->SetInt("uLuma", q.Luma ? 1 : 0);
            m_Quad->SetInt("uTextureColor", q.TextureColor ? 1 : 0);
            m_Quad->SetInt("uOwnColor", q.OwnColor ? 1 : 0);
            m_Quad->SetFloat("uReveal", q.Reveal);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_BLEND);
    }
    for (const LayerProgram::Run& r : lp.Runs) DrawSplats(lp.Splats.data() + r.Begin, r.End - r.Begin, r.Erase);
}

void Compositor::BlurLayer(const Piece& piece, const Layer& layer, unsigned int coverageTex) {
    const bool outline = layer.Outline && layer.Type != LayerType::Text;
    if (!outline && !layer.Shadow) return;
    const int hw = std::max(1, m_W / 2), hh = std::max(1, m_H / 2);
    glViewport(0, 0, hw, hh);
    m_Blur->Bind();
    m_Blur->SetInt("uSrc", 0);
    const float H = (float)piece.Height;
    // Two Gaussian passes per axis, 9 taps each (sigma about 2 steps): a width w (texels) wants a
    // sigma of about w / 2 to reach it at the outline's threshold.
    auto blur = [&](float widthPx, int tmp, int dst) {
        const float step = std::max(widthPx * 0.5f / (2.0f * 1.41421356f), 0.5f);
        const glm::vec2 du(step / (float)m_W, 0.0f), dv(0.0f, step / (float)m_H);
        for (int pass = 0; pass < 2; ++pass) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFbo[tmp]);
            BindTex(0, pass == 0 ? coverageTex : m_BlurTex[dst]);
            m_Blur->SetInt("uChannel", pass == 0 ? 3 : 0);
            m_Blur->SetVec2("uStep", du);
            Fullscreen();
            glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFbo[dst]);
            BindTex(0, m_BlurTex[tmp]);
            m_Blur->SetInt("uChannel", 0);
            m_Blur->SetVec2("uStep", dv);
            Fullscreen();
        }
    };
    if (outline) blur(layer.OutlineWidth * H, 0, 1);
    if (layer.Shadow) blur(std::max(layer.ShadowBlur * H, 1.0f), 2, 3);
    glViewport(0, 0, m_W, m_H);
}

void Compositor::Composite(const Piece& piece, const Layer& l, const Target& below, const Target& layerT, const Target& dst) {
    BlurLayer(piece, l, layerT.Color);
    glBindFramebuffer(GL_FRAMEBUFFER, dst.Fbo);
    glViewport(0, 0, m_W, m_H);
    m_Composite->Bind();
    BindTex(0, below.Color);
    BindTex(1, below.Extra);
    BindTex(2, layerT.Color);
    BindTex(3, layerT.Extra);
    BindTex(4, m_BlurTex[1]);
    BindTex(5, m_BlurTex[3]);
    m_Composite->SetInt("uBelow0", 0);
    m_Composite->SetInt("uBelow1", 1);
    m_Composite->SetInt("uLayer0", 2);
    m_Composite->SetInt("uLayer1", 3);
    m_Composite->SetInt("uBlur", 4);
    m_Composite->SetInt("uBlurShadow", 5);
    m_Composite->SetInt("uMode", (int)l.Mode);
    m_Composite->SetFloat("uOpacity", l.Opacity);
    m_Composite->SetInt("uClip", l.Clip ? 1 : 0);
    m_Composite->SetInt("uOutlineOn", l.Outline && l.Type != LayerType::Text ? 1 : 0);
    m_Composite->SetVec3("uOutline", SrgbToLinear(l.OutlineColor));
    m_Composite->SetInt("uShadowOn", l.Shadow ? 1 : 0);
    m_Composite->SetVec3("uShadow", SrgbToLinear(l.ShadowColor));
    // Offset as a fraction of the canvas height, y down -> uv, y up.
    m_Composite->SetVec2("uShadowOffset", glm::vec2(l.ShadowOffset.x * (float)m_H / (float)m_W, -l.ShadowOffset.y));
    m_Composite->SetFloat("uShadowOpacity", l.ShadowOpacity);
    m_Composite->SetFloat("uMetal", l.Metal);
    m_Composite->SetFloat("uGlow", l.Glow);
    Fullscreen();
}

// Composites program layers [first, last) starting from `start` (null: an empty canvas) and returns
// the m_C index holding the result.
int Compositor::ComposeFrom(const Piece& piece, const Program& program, int first, int last, const Target* start) {
    int cur = 0;
    const Target* below = start;
    if (!below) {
        ClearTarget(m_C[0].Fbo, 2);
        below = &m_C[0];
    }
    for (int i = first; i < last; ++i) {
        const LayerProgram& lp = program.Layers[(size_t)i];
        DrawLayer(piece, lp, m_Layer);
        const int dst = below == &m_C[cur] ? 1 - cur : cur;
        Composite(piece, piece.Layers[(size_t)lp.Layer], *below, m_Layer, m_C[dst]);
        cur = dst;
        below = &m_C[cur];
    }
    if (below != &m_C[0] && below != &m_C[1]) { // nothing composited: copy the start through
        glCopyImageSubData(below->Color, GL_TEXTURE_2D, 0, 0, 0, 0, m_C[0].Color, GL_TEXTURE_2D, 0, 0, 0, 0, m_W, m_H, 1);
        glCopyImageSubData(below->Extra, GL_TEXTURE_2D, 0, 0, 0, 0, m_C[0].Extra, GL_TEXTURE_2D, 0, 0, 0, 0, m_W, m_H, 1);
        cur = 0;
    }
    return below == &m_C[1] ? 1 : 0;
}

void Compositor::DrawPreview(const Piece& piece, const Target& final) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_PreviewFbo);
    glViewport(0, 0, m_W, m_H);
    m_PreviewShader->Bind();
    BindTex(0, final.Color);
    BindTex(1, final.Extra);
    m_PreviewShader->SetInt("uComposite0", 0);
    m_PreviewShader->SetInt("uComposite1", 1);
    m_PreviewShader->SetVec2("uCanvas", glm::vec2((float)m_W, (float)m_H));
    m_PreviewShader->SetFloat("uRelief", piece.Relief);
    m_PreviewShader->SetFloat("uShowThrough", piece.ShowThrough);
    m_PreviewShader->SetVec3("uWall", m_Wall);
    Fullscreen();
}

void Compositor::Render(const Piece& piece, const Program& program) {
    EnsureGl();
    StateGuard guard;
    EnsureTargets(program.Width, program.Height);
    m_TextureNames = program.Textures;
    for (const std::string& t : program.Textures) LoadImage(t);
    m_Final = ComposeFrom(piece, program, 0, (int)program.Layers.size(), nullptr);
    m_Composited = true;
    DrawPreview(piece, m_C[m_Final]);
}

bool Compositor::Repreview(const Piece& piece) {
    if (!m_Composited || !m_PreviewFbo || m_LiveLayer >= 0) return false;
    StateGuard guard;
    DrawPreview(piece, m_C[m_Final]);
    return true;
}

void Compositor::BeginLive(const Piece& piece, const Program& program, int programLayer) {
    EnsureGl();
    StateGuard guard;
    EnsureTargets(program.Width, program.Height);
    m_TextureNames = program.Textures;
    for (const std::string& t : program.Textures) LoadImage(t);
    m_LiveLayer = std::clamp(programLayer, 0, std::max(0, (int)program.Layers.size() - 1));
    // The layers below, once, into m_Below; the live layer's paint so far into its own target.
    const int below = ComposeFrom(piece, program, 0, m_LiveLayer, nullptr);
    glCopyImageSubData(m_C[below].Color, GL_TEXTURE_2D, 0, 0, 0, 0, m_Below.Color, GL_TEXTURE_2D, 0, 0, 0, 0, m_W, m_H, 1);
    glCopyImageSubData(m_C[below].Extra, GL_TEXTURE_2D, 0, 0, 0, 0, m_Below.Extra, GL_TEXTURE_2D, 0, 0, 0, 0, m_W, m_H, 1);
    if (m_LiveLayer < (int)program.Layers.size()) DrawLayer(piece, program.Layers[(size_t)m_LiveLayer], m_LiveT);
    else ClearTarget(m_LiveT.Fbo, 2);
}

void Compositor::LiveSplats(const Splat* splats, size_t count) {
    if (!Live() || !count) return;
    StateGuard guard;
    glBindFramebuffer(GL_FRAMEBUFFER, m_LiveT.Fbo);
    glViewport(0, 0, m_W, m_H);
    // Consecutive splats of one kind in one draw.
    size_t begin = 0;
    for (size_t i = 1; i <= count; ++i)
        if (i == count || (splats[i].Flags & kSplatErase) != (splats[begin].Flags & kSplatErase)) {
            DrawSplats(splats + begin, i - begin, (splats[begin].Flags & kSplatErase) != 0);
            begin = i;
        }
}

void Compositor::LiveComposite(const Piece& piece, const Program& program) {
    if (!Live() || m_LiveLayer >= (int)program.Layers.size()) return;
    StateGuard guard;
    const LayerProgram& live = program.Layers[(size_t)m_LiveLayer];
    Composite(piece, piece.Layers[(size_t)live.Layer], m_Below, m_LiveT, m_C[0]);
    m_Final = 0;
    m_Composited = true;
    // The layers above: re-drawn and stacked on (usually none: you spray on the top layer).
    for (int i = m_LiveLayer + 1; i < (int)program.Layers.size(); ++i) {
        const LayerProgram& lp = program.Layers[(size_t)i];
        DrawLayer(piece, lp, m_Layer);
        Composite(piece, piece.Layers[(size_t)lp.Layer], m_C[m_Final], m_Layer, m_C[1 - m_Final]);
        m_Final = 1 - m_Final;
    }
    DrawPreview(piece, m_C[m_Final]);
}

// --- Bake ------------------------------------------------------------------------------------------

void Compositor::StartBake(const Piece& piece, const Program& program) {
    FinishInFlight(); // (a Render that resizes the targets does the same)
    Render(piece, program);
    StateGuard guard;
    const Target& c = m_C[m_Final];
    glBindTexture(GL_TEXTURE_2D, c.Color);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_OutFbo);
    glViewport(0, 0, m_W, m_H);
    m_Bake->Bind();
    BindTex(0, c.Color);
    BindTex(1, c.Extra);
    m_Bake->SetInt("uComposite0", 0);
    m_Bake->SetInt("uComposite1", 1);
    m_Bake->SetVec2("uCanvas", glm::vec2((float)m_W, (float)m_H));
    m_Bake->SetFloat("uRelief", piece.Relief);
    m_Bake->SetFloat("uShowThrough", piece.ShowThrough);
    m_Bake->SetInt("uGlowOn", piece.GlowStrength > 0.0f ? 1 : 0);
    glUniform1ui(m_Bake->Loc("uSeed"), piece.Seed);
    Fullscreen();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int i = 0; i < 4; ++i) {
        glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_Pbo[i]);
        glReadPixels(0, 0, m_W, m_H, i == 3 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    m_Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

void Compositor::FinishInFlight() {
    if (!m_Fence) return;
    const GLenum r = glClientWaitSync((GLsync)m_Fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    glDeleteSync((GLsync)m_Fence);
    m_Fence = nullptr;
    if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) return;
    BakeImages images;
    if (ReadBack(images)) {
        m_Stash = std::move(images);
        m_HasStash = true;
    }
}

bool Compositor::PollBake(BakeImages& out, bool wait) {
    if (m_HasStash) {
        out = std::move(m_Stash);
        m_Stash = BakeImages{};
        m_HasStash = false;
        return true;
    }
    if (!m_Fence) return false;
    const GLenum r = glClientWaitSync((GLsync)m_Fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? 5000000000ull : 0ull);
    if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) return false;
    glDeleteSync((GLsync)m_Fence);
    m_Fence = nullptr;
    return ReadBack(out);
}

bool Compositor::ReadBack(BakeImages& out) {
    out.Width = m_W;
    out.Height = m_H;
    const size_t px = (size_t)m_W * m_H;
    out.Albedo.resize(px * 4);
    out.Normal.resize(px * 3);
    out.Surface.resize(px * 4);
    out.Aux.resize(px);
    // GL rows run bottom up: flip into the top-first rows a PNG wants.
    for (int i = 0; i < 4; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_Pbo[i]);
        const int comp = i == 3 ? 1 : 4;
        const auto* src = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)(px * comp), GL_MAP_READ_BIT));
        if (!src) continue;
        for (int y = 0; y < m_H; ++y) {
            const uint8_t* row = src + (size_t)(m_H - 1 - y) * m_W * comp;
            if (i == 1) {
                uint8_t* dst = out.Normal.data() + (size_t)y * m_W * 3;
                for (int x = 0; x < m_W; ++x) std::memcpy(dst + x * 3, row + x * 4, 3);
            } else {
                std::vector<uint8_t>& dst = i == 0 ? out.Albedo : i == 2 ? out.Surface : out.Aux;
                std::memcpy(dst.data() + (size_t)y * m_W * comp, row, (size_t)m_W * comp);
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

BakeFiles EncodePngs(const BakeImages& im) {
    BakeFiles f;
    if (im.Width <= 0 || im.Height <= 0) return f;
    // stb's encoder is deterministic for a fixed level; set it explicitly so nothing else's choice
    // changes these bytes.
    stbi_write_png_compression_level = 8;
    stbi_write_png_to_func(AppendBytes, &f.Albedo, im.Width, im.Height, 4, im.Albedo.data(), im.Width * 4);
    stbi_write_png_to_func(AppendBytes, &f.Normal, im.Width, im.Height, 3, im.Normal.data(), im.Width * 3);
    stbi_write_png_to_func(AppendBytes, &f.Surface, im.Width, im.Height, 4, im.Surface.data(), im.Width * 4);
    stbi_write_png_to_func(AppendBytes, &f.Aux, im.Width, im.Height, 1, im.Aux.data(), im.Width);
    return f;
}

} // namespace Graffiti
