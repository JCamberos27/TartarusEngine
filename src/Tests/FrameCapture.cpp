#include "Tests/FrameCapture.h"

#include "gl.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace FrameCapture {

std::vector<unsigned char> GrabRegion(int x, int y, int w, int h) {
    std::vector<unsigned char> px((size_t)std::max(w, 0) * std::max(h, 0) * 4);
    if (w > 0 && h > 0) glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

std::vector<unsigned char> GrabTopDown(unsigned fbo, int w, int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    std::vector<unsigned char> px = GrabRegion(0, 0, w, h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    std::vector<unsigned char> flipped(px.size());
    for (int y = 0; y < h; ++y)
        std::memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    return flipped;
}

std::string WritePngTo(const std::string& dir, const std::string& name, int w, int h, const std::vector<unsigned char>& rgba) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string out = (std::filesystem::path(dir) / (name + ".png")).string();
    stbi_write_png(out.c_str(), w, h, 4, rgba.data(), w * 4);
    return out;
}

} // namespace FrameCapture
