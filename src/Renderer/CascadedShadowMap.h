#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include "DepthArrayTarget.h"

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

    // Practical split blend: 0 = uniform splits, 1 = fully logarithmic. Higher packs the first
    // cascades tighter around the camera (sharper near shadows, coarser far ones). Clamped to
    // [0, 0.98]; 0.75 is the long-standing default. Takes effect on the next Update().
    void SetSplitLambda(float lambda);
    float SplitLambda() const { return m_SplitLambda; }

    // Sun PCF kernel radius in shadow-map texels, before SunShadow's per-cascade widening: the
    // light's apparent size sets it, the light's Shadow Softness scales it. Softness <= 0 returns
    // 0, which the shader reads as "hard": one hardware 2x2 compare, no Poisson disk.
    static float SunKernelTexels(float angularDeg, float softness);

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
    // Cascade i's target again, without clearing it: to add casters after the pass (the grass).
    void Resume(int i) const;

    // Single-pass alternative to Begin(i): binds an FBO with the whole array attached (a layered
    // target), sets the viewport and clears every cascade at once. The caster draws then pick
    // their layer in the vertex shader (gl_Layer), one instance per cascade. Only valid when
    // LayeredSupported().
    void BeginLayered(unsigned mask = ~0u) const; // clears only `mask`'s layers (PlanRender)
    // Whether the driver lets a vertex shader write gl_Layer (ARB_shader_viewport_layer_array or
    // AMD_vertex_shader_layer). Checked once; needs a current GL context.
    static bool LayeredSupported();
    // GL_ARB_shader_viewport_layer_array: the vertex shader can also pick the viewport (and so the scissor box) per
    // cascade, which BeginLayeredScissor needs.
    static bool ViewportIndexSupported();

    unsigned int DepthArray() const { return m_Target.Texture(); }
    int Resolution() const { return m_Resolution; }
    int Count() const { return m_Count; }
    const glm::mat4& LightViewProj(int i) const { return m_LightViewProj[i]; }
    // Far cascades on alternate frames: cascade 2 renders on even frames, 3 on odd ones, and each is held (sampled
    // as rendered, with its own matrix) on the frame between. The near two render every frame. A held layer is
    // only reused when it was rendered the frame before, with the same light direction and texel size, and its
    // rendered area - the slice then, grown by kHoldTexels - still covers this frame's slice; otherwise it renders
    // now. Call after Update(); returns the cascades to render this frame (bit c = cascade c). Every cascade when
    // called twice in one frame (two views sharing the map), with fewer than 4 cascades, or with
    // TARTARUS_NO_CASCADE_ALTERNATE set. Shadows past cascade 1 (wind, things moving) update at half rate.
    unsigned PlanRender(std::uint64_t frame);
    // After the casters went into `mask`'s layers: they now hold this frame's fit.
    void MarkRendered(unsigned mask, std::uint64_t frame);
    // What a receiver samples cascade i with: this frame's fit, or a held layer's own.
    const glm::mat4& SampleViewProj(int i) const { return m_SampleViewProj[i]; }
    glm::vec4 SampleTexelWorldSizesVec4() const;
    // The extra reach (texels) a cascade renders with when it will be held a frame: room for a frame of camera turn.
    static constexpr float kHoldTexels = 64.0f;
    // Cascade split far-distances in *view space* (positive), one per cascade; [i] is the far
    // edge of cascade i. Packed into a vec4 for the shader (unused lanes hold the last split).
    glm::vec4 SplitDepthsVec4() const;
    // World units covered by one shadow-map texel in cascade i (= 2*fit-radius / resolution).
    // The shader scales its normal offset + depth bias by this so one bias value isn't wrong
    // for cascade 0 and cascade 3 at the same time (#117). Packed into a vec4 like the splits.
    glm::vec4 TexelWorldSizesVec4() const;

    // The layered pass's scissor boxes (ShadowDepth.vert routes each cascade to its own viewport): each cascade only
    // rasterises ReadRect, the texels a receiver in its slice can read. The rest of a layer keeps the clear depth and
    // is never sampled. Call after BeginLayered; EndLayeredScissor turns the test off again.
    void BeginLayeredScissor(float kernelTexels, float normalBias) const;
    static void EndLayeredScissor();
    // Cascade c's readable rectangle in its light clip space (x, y): see the definition.
    void ReadRect(int c, float kernelTexels, float normalBias, glm::vec2& lo, glm::vec2& hi) const;

    // Whether a caster with these world bounds can shadow anything that samples cascade `c`. A
    // cascade's light box is a square around a sphere around its view slice, far wider than the
    // slice; only fragments whose view depth selects the cascade (or blends into it from the one
    // before, over the shader's 12% band) read it, and those fragments lie in that depth range of
    // the view frustum. Seen along the light, a caster can only darken what its own footprint
    // covers, so it is needed iff its light-space footprint - grown by the PCF kernel
    // (`kernelTexels`, before the per-cascade widening SunShadow applies) and the normal offset -
    // overlaps the slice's. Cascade 0 tests against ReadRect (the near slice's bounding box and the
    // eye's surroundings, where the view model draws at its own FOV): what misses it would be
    // scissored away anyway.
    bool CasterReachesSlice(int c, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                            float kernelTexels, float normalBias) const;
    // Cascade c's receiver slice corners in its light clip space (x, y), from Update().
    const std::array<glm::vec2, 8>& SliceCorners(int c) const { return m_SliceCorners[c]; }

private:
    DepthArrayTarget m_Target; // the cascade array (its own per-layer FBOs below, so no FBOs of its own)
    // One FBO per cascade, each attached to its own layer once in Configure(). Re-pointing a
    // single FBO's attachment in every Begin() made the driver revalidate it once per cascade.
    std::array<unsigned int, kMaxCascades> m_Fbos{};
    unsigned int m_LayeredFbo = 0; // every layer attached, for BeginLayered()
    int m_Resolution = 2048;
    int m_Count = 4;
    float m_SplitLambda = 0.75f;

    std::array<glm::mat4, kMaxCascades> m_LightViewProj{};
    std::array<float, kMaxCascades> m_SplitFar{};
    std::array<float, kMaxCascades> m_TexelWorld{}; // 2*radius/resolution per cascade, from Update()
    // The receiver slice of each cascade (see CasterReachesSlice): its 8 corners in that cascade's
    // light clip space (x, y only - the projection is orthographic).
    std::array<std::array<glm::vec2, 8>, kMaxCascades> m_SliceCorners{};
    // Each slice's convex hull edge normals and the slice's extent along each (separating axes for
    // CasterReachesSlice, worked out once per Update rather than per caster).
    struct SliceAxis { glm::vec2 N; float Min, Max; };
    std::array<std::array<SliceAxis, 8>, kMaxCascades> m_SliceAxes{};
    std::array<int, kMaxCascades> m_SliceAxisCount{};
    std::array<glm::vec2, kMaxCascades> m_SliceMin{}, m_SliceMax{};
    // The slice's bounds within the shadow distance (PlanRender's hold test): the last cascade's slice runs on to the
    // camera's far plane, and that part swings far on any turn.
    std::array<glm::vec2, kMaxCascades> m_HoldMin{}, m_HoldMax{};
    glm::vec3 m_Eye{0.0f}; // the fitted view's eye (BeginLayeredScissor's view-model reach)
    glm::vec3 m_LightDir{0.0f, -1.0f, 0.0f}; // Update()'s, normalised
    // Alternate-frame far cascades (PlanRender): what each layer holds and what receivers sample it with.
    std::array<glm::mat4, kMaxCascades> m_SampleViewProj{};
    std::array<float, kMaxCascades> m_SampleTexelWorld{};
    std::array<glm::mat4, kMaxCascades> m_HeldViewProj{};
    std::array<float, kMaxCascades> m_HeldTexelWorld{};
    std::array<glm::vec3, kMaxCascades> m_HeldLightDir{};
    std::array<glm::vec2, kMaxCascades> m_HeldLo{}, m_HeldHi{}; // the slice it was rendered for, grown by the hold
    std::array<std::uint64_t, kMaxCascades> m_HeldFrame{};
    std::array<float, kMaxCascades> m_HoldTexels{}; // extra reach this frame's render of the cascade carries
    std::uint64_t m_LastPlanFrame = ~std::uint64_t(0);
    mutable bool m_CompleteChecked = false; // audit GL-204 — one-shot FBO completeness check in Begin()

    void Release();
};
