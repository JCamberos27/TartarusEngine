#pragma once

#include <string>
#include <vector>

// The harnesses' framebuffer-to-PNG captures (--smoke-shots, --npc-test, --editor-shot, F12).
namespace FrameCapture {

// Read `w` x `h` RGBA8 pixels out of the bound READ framebuffer from (x, y), GL's bottom-left origin.
std::vector<unsigned char> GrabRegion(int x, int y, int w, int h);
// The harnesses' PNG captures: framebuffer `fbo`'s w x h pixels, top row first (GL reads bottom up). Leaves the read
// framebuffer at 0.
std::vector<unsigned char> GrabTopDown(unsigned fbo, int w, int h);
// Writes `rgba` (w x h, top row first) to dir/name.png, making dir if needed. Returns the path.
std::string WritePngTo(const std::string& dir, const std::string& name, int w, int h, const std::vector<unsigned char>& rgba);

} // namespace FrameCapture
