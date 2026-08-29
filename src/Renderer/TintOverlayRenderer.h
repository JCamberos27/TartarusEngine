#pragma once
#include <memory>
#include <glm/glm.hpp>

class Model;
class Shader;

// Draws a Model as a flat, translucent tinted overlay directly into whatever framebuffer is
// currently bound (the main viewport). Two call sites use this for two different reasons:
//  - The "drag an asset from the Browser into the Viewport" placement preview - shows where a
//    not-yet-committed model would land before you release the mouse.
//  - A selection highlight wash over an already-placed, currently-selected mesh's own surface -
//    the existing inverted-hull outline only draws a thin rim around the silhouette, which reads
//    poorly on a small or thin object; this washes the whole visible surface in the same
//    outline color at low alpha so the selected mesh is unambiguous at a glance.
// Deliberately NOT the full PBR material shader: a flat tint reads unambiguously as "this is a
// UI overlay, not real shading" (the ghost's whole point), and it sidesteps needing to bind
// textures/lighting for a draw that isn't part of the normal material pass. Bone/skinning
// transforms are intentionally skipped too - an animated model's overlay previews its bind pose,
// a fine approximation for both use cases above.
class TintOverlayRenderer {
public:
    TintOverlayRenderer();
    ~TintOverlayRenderer();

    // Depth-tested against whatever's already in the bound framebuffer so it's correctly hidden
    // behind other geometry in front of it, but never writes depth itself, so it can't corrupt
    // the real scene's depth buffer for anything drawn after it this frame. Uses GL_LEQUAL for
    // the duration of this draw (restored after) rather than whatever depth func is currently
    // set — this is often used to redraw a mesh exactly on top of itself (the selection-
    // highlight case), which needs equal depth values to pass; the engine's default GL_LESS
    // would reject every fragment as "not strictly closer" and the overlay would render nothing.
    // GL_LEQUAL alone still isn't reliable on its own for that same-mesh-twice case, though —
    // the two draws' depth values are usually but not always bit-identical, so which one "wins"
    // a given pixel can flip as the camera moves (visible as flickering). A small negative
    // glPolygonOffset is applied too, nudging this draw reliably closer to the camera instead of
    // depending on exact equality — the standard fix for coplanar/overlay z-fighting.
    void Render(Model& model, const glm::mat4& modelMatrix, const glm::mat4& view, const glm::mat4& proj,
        const glm::vec3& tintColor, float alpha);

private:
    std::unique_ptr<Shader> m_Shader;
};
