#pragma once
#include <glm/glm.hpp>
#include <array>

// Cascaded shadow maps for the directional sun.
//
// One GL_TEXTURE_2D_ARRAY of DEPTH_COMPONENT32F layers (one per cascade), sampled in the model
// shader as a sampler2DArrayShadow. Each frame Update() splits the active render camera's view
// range into `Count()` depth slices, fits a texel-snapped orthographic light frustum around
// each slice, and hands back the light view-projection matrices + the slice far-depths. The
// caller then renders scene depth once per cascade via Begin(i).
//
// Fixed contract for now: 4 cascades, resolution from EditorSettings, PCF filtering done in the
// shader. PCSS / contact-hardening is a later toggle.
class CascadedShadowMap {
public:
    static constexpr int kMaxCascades = 4;

    CascadedShadowMap() = default;
    ~CascadedShadowMap();
    CascadedShadowMap(const CascadedShadowMap&) = delete;
    CascadedShadowMap& operator=(const CascadedShadowMap&) = delete;

    // Lazily (re)creates the depth array when resolution/count change. `count` is clamped to
    // [1, kMaxCascades].
    void Configure(int resolution, int count);

    // Recomputes cascade splits + light matrices for this frame. `lightDir` is the direction the
    // sunlight travels (normalized inside). `camView`/`camProj` are the matrices of whichever
    // view is about to be rendered; `shadowDistance` caps how far cascades reach (world units).
    // #160: `casterMin`/`casterMax` (optional) bound every shadow caster in world space; each
    // cascade's light-space near plane is pulled back far enough to include all of them, so a
    // tall or distant caster outside the view still shadows into it. Without bounds, the old
    // fixed 50 m pullback is used.
    void Update(const glm::mat4& camView, const glm::mat4& camProj,
                const glm::vec3& lightDir, float shadowDistance,
                const glm::vec3* casterMin = nullptr, const glm::vec3* casterMax = nullptr);

    // Binds the shadow FBO targeting cascade `i`'s layer, sets the viewport, clears its depth.
    // Caller then draws occluders with LightViewProj(i). Does NOT restore the previous
    // framebuffer — the caller captured and restores it.
    void Begin(int i) const;

    // Single-pass alternative to Begin(i): binds an FBO with the whole array attached (a layered
    // target), sets the viewport and clears every cascade at once. The caster draws then pick
    // their layer in the vertex shader (gl_Layer), one instance per cascade. Only valid when
    // LayeredSupported().
    void BeginLayered() const;
    // Whether the driver lets a vertex shader write gl_Layer (ARB_shader_viewport_layer_array or
    // AMD_vertex_shader_layer). Checked once; needs a current GL context.
    static bool LayeredSupported();

    unsigned int DepthArray() const { return m_DepthArray; }
    int Resolution() const { return m_Resolution; }
    int Count() const { return m_Count; }
    const glm::mat4& LightViewProj(int i) const { return m_LightViewProj[i]; }
    // Cascade split far-distances in *view space* (positive), one per cascade; [i] is the far
    // edge of cascade i. Packed into a vec4 for the shader (unused lanes hold the last split).
    glm::vec4 SplitDepthsVec4() const;
    // World units covered by one shadow-map texel in cascade i (= 2*fit-radius / resolution).
    // The shader scales its normal offset + depth bias by this so one bias value isn't wrong
    // for cascade 0 and cascade 3 at the same time (#117). Packed into a vec4 like the splits.
    glm::vec4 TexelWorldSizesVec4() const;

    // Whether a caster with these world bounds can shadow anything that samples cascade `c`. A
    // cascade's light box is a square around a sphere around its view slice, far wider than the
    // slice; only fragments whose view depth selects the cascade (or blends into it from the one
    // before, over the shader's 12% band) read it, and those fragments lie in that depth range of
    // the view frustum. Seen along the light, a caster can only darken what its own footprint
    // covers, so it is needed iff its light-space footprint - grown by the PCF kernel
    // (`kernelTexels`, before the per-cascade widening SunShadow applies) and the normal offset -
    // overlaps the slice's. Cascade 0 always answers true: the view model draws at its own FOV, so
    // its fragments can sit outside the world view's frustum near the eye.
    bool CasterReachesSlice(int c, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                            float kernelTexels, float normalBias) const;

private:
    unsigned int m_DepthArray = 0;
    // One FBO per cascade, each attached to its own layer once in Configure(). Re-pointing a
    // single FBO's attachment in every Begin() made the driver revalidate it once per cascade.
    std::array<unsigned int, kMaxCascades> m_Fbos{};
    unsigned int m_LayeredFbo = 0; // every layer attached, for BeginLayered()
    int m_Resolution = 2048;
    int m_Count = 4;

    std::array<glm::mat4, kMaxCascades> m_LightViewProj{};
    std::array<float, kMaxCascades> m_SplitFar{};
    std::array<float, kMaxCascades> m_TexelWorld{}; // 2*radius/resolution per cascade, from Update()
    // The receiver slice of each cascade (see CasterReachesSlice): its 8 corners in that cascade's
    // light clip space (x, y only - the projection is orthographic).
    std::array<std::array<glm::vec2, 8>, kMaxCascades> m_SliceCorners{};
    mutable bool m_CompleteChecked = false; // audit GL-204 — one-shot FBO completeness check in Begin()

    void Release();
};
