#pragma once

// Per-frame render statistics, filled in by the scene-draw pass (SceneRenderer::RenderScene)
// and displayed by the editor's Stats overlay. The editor can count entities itself, but only
// the renderer knows how many draw calls actually issued, how many were frustum-culled, and
// whether the frame's light budgets overflowed.
//
// Lives in the Renderer layer (not the Editor) so SceneRenderer has no dependency on the
// editor: EditorLayer re-exports it as EditorLayer::RenderStats for its existing call sites
// (audit #359).
struct RenderStats {
    int DrawCalls = 0;
    int Triangles = 0;
    int Vertices = 0;
    int PointLights = 0;
    int Culled = 0; // entities skipped by frustum culling this frame - not drawn at all
    // #204: the scene has more active lights than the forward LightBuffer can hold
    // (LightBuffer::kMaxLights) - the excess were silently dropped before this existed.
    bool LightBufferOverflowed = false;
    // #204: the global per-frame cluster light-index list (ClusterGrid::GLOBAL_INDEX_CAPACITY)
    // ran out of room this frame, so at least one cluster's reserved block was truncated and
    // may be missing lights that should be shading it (#208: adapted from a per-cluster cap
    // to this shared-list capacity).
    bool ClusterSaturated = false;
};
