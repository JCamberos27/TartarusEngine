#pragma once

#include <glm/glm.hpp>

class HdrTarget;
class OpaqueColorCopy;
class Ssao;

// Per-viewport inputs for one scene draw (audit #359, pass 1).
//
// Bundles what used to be the positional parameters of main.cpp's `drawScene` lambda into one
// explicit value, so the Scene tab, the docked Game view and maximized play all call the same
// renderer entry point with a named struct instead of a long argument list. This is the first
// reviewable slice of the SceneRenderer extraction: the draw *body* still lives in main.cpp for
// now; only the call boundary is formalized here. The RenderStats out-param stays a separate
// argument so this header carries no dependency on the editor layer.
//
// GL state contract for the scene-draw pass that consumes this (documented per AC3):
//   On entry the caller must have:
//     - bound the destination framebuffer (the HdrTarget MSAA FBO) and set the viewport,
//     - run this frame's shadow / IBL / cluster prerequisite passes,
//     - left GL_DEPTH_TEST enabled, glDepthMask(GL_TRUE), GL_BLEND disabled.
//   On return the pass restores that baseline: depth writes on, blending off, the model
//   shader's uAlphaBlend = 0, active texture unit 0. It does NOT restore the framebuffer
//   binding or viewport — the caller owns those.
struct RenderFrameContext {
    glm::mat4 View{1.0f};
    glm::mat4 Proj{1.0f};
    glm::vec3 ViewPos{0.0f};

    bool Unlit      = false; // shading mode: skip all lighting, sky-ambient only
    bool EditorView = false; // Scene tab only — honors per-entity / per-layer visibility hides
    int  DebugView  = 0;     // model shader uDebugView: 1 Normals, 2 Cascades, 3 Mip

    // Transparent-pass refraction source: the caller's own HDR target + opaque-color copy for
    // this viewport (Scene and Game each keep their own, sized independently). Both null =
    // no refraction capture (offscreen / preview paths).
    HdrTarget*       TxHdr     = nullptr;
    OpaqueColorCopy* TxCapture = nullptr;

    // The caller's per-viewport SSAO instance; null or !IsValid() -> the draw runs without AO.
    Ssao* SsaoSrc = nullptr;
};
