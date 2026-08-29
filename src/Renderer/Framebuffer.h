#pragma once

// A resizable offscreen render target: one color texture (sampleable, for ImGui::Image or a
// glBlitFramebuffer destination) plus a depth-only renderbuffer (needed for correct 3D
// rendering into it, never sampled). Used by GameViewPanel to render the scene at a locked
// aspect ratio/resolution independent of the window/editor-viewport size.
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // No-op if `width`/`height` already match the current size — cheap to call every frame.
    // Lazily creates the underlying GL objects on first call (avoids needing a live GL context
    // at construction time, matching this codebase's other GL-owning classes).
    void Resize(int width, int height);

    void Bind() const;   // binds this FBO and sets glViewport(0, 0, Width, Height)
    static void BindDefault(int windowWidth, int windowHeight); // rebinds FBO 0 + restores viewport

    unsigned int ColorTexture() const { return m_ColorTexture; }
    unsigned int Handle() const { return m_Fbo; }
    int Width() const { return m_Width; }
    int Height() const { return m_Height; }
    bool IsValid() const { return m_Fbo != 0; }

private:
    unsigned int m_Fbo = 0;
    unsigned int m_ColorTexture = 0;
    unsigned int m_DepthRbo = 0;
    int m_Width = 0;
    int m_Height = 0;

    void Release();
    void Create(int width, int height);
};
