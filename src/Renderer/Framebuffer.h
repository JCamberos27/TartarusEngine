#pragma once

// A resizable offscreen display target: one RGB8 colour texture (sampleable, for ImGui::Image or a
// glBlitFramebuffer destination) and nothing else. The Scene and Game tabs' tonemapped images, the FXAA
// intermediate and the modal-blur capture: the 3D draws happen in the HDR targets (HdrTarget), and what goes
// into these afterwards - the tonemap, the grid, the HUD - draws with depth testing off, so there is no depth
// buffer (a DEPTH24_STENCIL8 one used to come with every target and every resize: ~8 MB each at 1080p).
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
    int m_Width = 0;
    int m_Height = 0;

    void Release();
    void Create(int width, int height);
};
