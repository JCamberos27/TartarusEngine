#pragma once
#include <memory>

class Texture;
class Shader;

// Renders one channel (R/G/B/A) of a source Texture, broadcast to grayscale, into an offscreen
// texture suitable for ImGui::Image — backs the per-channel isolation toggles on the asset
// Inspector's texture preview (see EditorLayer::DrawAssetImportInspector). Lazily creates its GL
// resources (an FBO, its color texture, and an attribute-less VAO) on first use and releases them
// in its destructor — the same RAII convention Texture/Grid/Sky already follow for their own GL
// objects, rather than EditorLayer holding raw GL handles directly.
class ChannelPreviewRenderer {
public:
    ChannelPreviewRenderer();
    ~ChannelPreviewRenderer();

    // channel: -1 = combined RGBA passthrough, 0..3 = isolate R/G/B/A as grayscale. Returns the
    // GL texture handle of the result, sized previewW x previewH. Cheap (one fullscreen-triangle
    // draw), but still meant to be called only when the source/channel actually changed, not
    // unconditionally every frame — see the caller's own render-state cache.
    unsigned int Render(const Texture& source, int channel, int previewW, int previewH);

    // The GL texture handle from the most recent Render() call, without re-rendering — for a
    // caller that tracks its own "has anything actually changed" state and only wants to fetch
    // the existing result on frames where it hasn't.
    unsigned int Handle() const { return m_ColorTex; }

private:
    unsigned int m_FBO = 0;
    unsigned int m_ColorTex = 0;
    unsigned int m_VAO = 0;
    int m_TexW = 0, m_TexH = 0; // current color attachment size, so a display-size change reallocates it
    std::unique_ptr<Shader> m_Shader;
};
