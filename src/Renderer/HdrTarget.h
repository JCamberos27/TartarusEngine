#pragma once

// The scene's HDR render target for the lighting overhaul.
//
// Everything 3D (sky, PBR meshes, and — once wired — the editor's grid / selection / gizmo
// overlays) renders into a multisampled RGBA16F colour + DEPTH_COMPONENT32F depth attachment,
// so lighting maths runs in linear HDR with real MSAA edges instead of the old straight-to-RGB8
// path that baked Reinhard + gamma into the model shader. After the scene pass, ResolveTo()
// blits the multisample buffer down to a single-sample RGBA16F texture that a later fullscreen
// tonemap pass samples (exposure -> ACES/AgX -> gamma) to produce the 8-bit image ImGui shows.
//
// Depth is kept as a sampleable texture (not a renderbuffer) because SSAO and, later, soft
// shadows / screen-space effects need to read it. Uses DSA (GL 4.5) throughout.
class HdrTarget {
public:
    HdrTarget() = default;
    ~HdrTarget();
    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    // Lazily (re)creates the GL objects. `samples` is clamped to [1, GL_MAX_SAMPLES]; 1 means
    // no MSAA (the resolve blit then degenerates to a plain copy). No-op when the size and
    // sample count already match, so it's cheap to call every frame.
    void Resize(int width, int height, int samples);

    // Binds the multisample FBO and sets glViewport(0, 0, Width, Height). Call before the
    // scene pass; leave it bound for the editor overlay passes too.
    void BindForRender() const;

    // Resolves the multisample colour into ResolvedColorTexture(). Call once, after every pass
    // that draws into this target for the frame is done. Does not touch the currently bound
    // draw framebuffer afterwards — the caller rebinds whatever it needs next.
    void ResolveTo() const;

    unsigned int ResolvedColorTexture() const { return m_ResolveColor; } // RGBA16F, linear-filtered, sampleable

    // Single-sample DEPTH_COMPONENT32F (#121) — lazily created on first call rather than at
    // Resize()/Create() time (#206). Until something (a future SSAO / screen-space pass) actually
    // calls this, the resolve target has no depth attachment and ResolveTo() blits colour only:
    // no wasted VRAM (~33 MB at 4K) or bandwidth for a texture nothing reads. Once called, it
    // stays provisioned (and re-provisioned across Resize()) for the life of this HdrTarget.
    unsigned int ResolvedDepthTexture() const;
    unsigned int MultisampleFbo() const { return m_MsFbo; }
    unsigned int DepthTexture() const { return m_MsDepth; }              // GL_TEXTURE_2D_MULTISAMPLE, DEPTH_COMPONENT32F

    int Width() const { return m_Width; }
    int Height() const { return m_Height; }
    int Samples() const { return m_Samples; }
    bool IsValid() const { return m_MsFbo != 0; }

private:
    unsigned int m_MsFbo = 0, m_MsColor = 0, m_MsDepth = 0;
    unsigned int m_ResolveFbo = 0, m_ResolveColor = 0;
    mutable unsigned int m_ResolveDepth = 0; // 0 until ResolvedDepthTexture() is first called
    mutable bool m_DepthRequested = false;   // sticky, so a later Resize()/Create() still provisions it
    int m_Width = 0, m_Height = 0, m_Samples = 0;

    void Release();
    void Create(int width, int height, int samples);
    void CreateResolveDepth() const; // allocates + parameterizes m_ResolveDepth (not attached)
};
