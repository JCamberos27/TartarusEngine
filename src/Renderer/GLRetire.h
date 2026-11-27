#pragma once

#include <cstddef>

// Textures replaced mid-frame that this frame's ImGui draw list may still name. The editor UI is built before some
// render targets resize (the Game view's image is recorded, then the Game View Render resizes its target), and Dear
// ImGui binds the recorded names only at the end of the frame: a name deleted in between was a GL error or, once
// reused, one frame of another texture. A retired texture lives until Flush, which the frame calls after ImGui's
// draw. Main thread only.
namespace GLRetire {
// Deletes `texture` at the next Flush instead of now (0 is ignored).
void Texture(unsigned int texture);
// Deletes everything retired so far: once a frame after the ImGui draw, and at shutdown while the context lives.
void Flush();
// How many textures are waiting (tests).
std::size_t Pending();
} // namespace GLRetire
