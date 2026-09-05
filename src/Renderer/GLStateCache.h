#pragma once

// Wraps the highest-traffic OpenGL state-setting calls (shader bind, per-unit texture bind) with
// a same-thread cache, so calling Shader::Bind()/Texture::Bind() with whatever's ALREADY bound —
// which happens constantly when many meshes in a row share a material — is a no-op instead of a
// real driver call. This is a deliberately narrower, safer piece than a full command-queue/
// batching system: only the two calls actually made once per entity per frame in the main draw
// loop are routed through here (Shader.cpp, Texture.cpp). Grid/Sky/selection-outline/preview-
// renderer passes elsewhere each toggle their OWN blend/depth/cull state once per pass already,
// explicitly save the previous value and restore it — correct as-is, and LOWER risk left alone
// than migrated to a shared cache they'd all need to stay perfectly in sync with.
//
// State is a single shared GL context (no threading in this engine), so this is free functions
// over static-like state, not an object you construct - call these the same way you'd call
// glUseProgram/glBindTexture directly.
namespace GLStateCache {
    // Call once per frame, after anything that changes GL state WITHOUT going through this cache
    // (Dear ImGui's own OpenGL3 backend does exactly this internally) - clears the cached values
    // so the next call through this cache doesn't wrongly assume they're still accurate.
    void Invalidate();

    void UseProgram(unsigned int program);
    void BindTexture2D(unsigned int unit, unsigned int texture);
    void BindVertexArray(unsigned int vao);

    // How many of the calls above this frame actually reached the driver vs. were skipped as
    // redundant - read by the Stats overlay, reset by ResetFrameStats() at the top of the frame.
    struct FrameStats {
        int ProgramBinds = 0;
        int ProgramBindsSkipped = 0;
        int TextureBinds = 0;
        int TextureBindsSkipped = 0;
        int VaoBinds = 0;
        int VaoBindsSkipped = 0;
    };
    const FrameStats& GetFrameStats();
    void ResetFrameStats();
}
