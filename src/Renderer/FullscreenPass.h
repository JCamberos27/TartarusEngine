#pragma once

// The pieces every fullscreen post pass (tonemap, bloom, SSAO, TAA, upscale, modal blur, opaque copy) used to
// spell out for itself: a colour texture with its framebuffer, and the GL state around a fullscreen-triangle draw.

// One 2D colour texture (immutable storage, clamp-to-edge) and the framebuffer that renders into it. Owned:
// both die with the object. Ensure() is cheap to call every frame; it only reallocates when the size, format,
// filter or mip count changes.
class ColorTarget {
public:
    ColorTarget() = default;
    ~ColorTarget() { Release(); }
    ColorTarget(const ColorTarget&) = delete;
    ColorTarget& operator=(const ColorTarget&) = delete;
    ColorTarget(ColorTarget&& o) noexcept { *this = static_cast<ColorTarget&&>(o); }
    ColorTarget& operator=(ColorTarget&& o) noexcept;

    // format: a sized GL internal format (GL_RGBA16F, GL_R8, ...). linear: bilinear (trilinear with mips) or
    // nearest sampling. levels: mip levels to allocate, 0 = the full chain down to 1x1. `name` labels the
    // framebuffer-completeness log. Returns IsValid().
    bool Ensure(int width, int height, unsigned int format, const char* name, bool linear = true, int levels = 1);
    void Release();

    void Bind() const; // binds the framebuffer and sets the viewport to the whole target

    unsigned int Texture() const { return m_Tex; }
    unsigned int Fbo() const { return m_Fbo; }
    int Width() const { return m_Width; }
    int Height() const { return m_Height; }
    bool IsValid() const { return m_Fbo != 0 && m_Complete; }

private:
    unsigned int m_Tex = 0, m_Fbo = 0;
    unsigned int m_Format = 0;
    int m_Width = 0, m_Height = 0, m_Levels = 0;
    bool m_Linear = true, m_Complete = false;
};

// Scope for attribute-less fullscreen-triangle draws (Fullscreen.vert.glsl). On entry: depth test, depth writes,
// blending, face culling and the scissor test off, and the shared empty vertex array bound. On exit: the
// caller's toggles back and GLStateCache invalidated (the passes bind textures and framebuffers directly).
//
// Scopes nest: only the outermost one reads and restores the caller's state, so a post chain wrapped in one
// scope (ViewPipeline) queries GL once instead of once per pass. The blend function, framebuffer and viewport
// are not restored: each pass binds its own target, and every pass that blends sets its own function (Bloom
// puts GL's default back after its additive upsample).
class FullscreenPass {
public:
    FullscreenPass();
    ~FullscreenPass();
    FullscreenPass(const FullscreenPass&) = delete;
    FullscreenPass& operator=(const FullscreenPass&) = delete;

    static void Draw(); // the covering triangle: glDrawArrays(GL_TRIANGLES, 0, 3)

private:
    bool m_Outer = false;
    bool m_DepthTest = false, m_DepthMask = true, m_Blend = false, m_CullFace = false, m_Scissor = false;
};
