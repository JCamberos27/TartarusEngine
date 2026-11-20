#include "Tests/FrameCapture.h"

#include "gl.h"

#include "PngWrite.h"

#include <iostream>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace FrameCapture {

std::vector<unsigned char> GrabRegion(int x, int y, int w, int h) {
    std::vector<unsigned char> px((size_t)std::max(w, 0) * std::max(h, 0) * 4);
    if (w > 0 && h > 0) glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

void GrabTopDown(unsigned fbo, int w, int h, std::vector<unsigned char>& out) {
    w = std::max(w, 0);
    h = std::max(h, 0);
    out.resize((size_t)w * h * 4);
    if (w == 0 || h == 0) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // Flip in place, one row at a time: no second full-frame buffer.
    const size_t row = (size_t)w * 4;
    std::vector<unsigned char> tmp(row);
    for (int y = 0; y < h / 2; ++y) {
        unsigned char* a = &out[(size_t)y * row];
        unsigned char* b = &out[(size_t)(h - 1 - y) * row];
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
}

std::vector<unsigned char> GrabTopDown(unsigned fbo, int w, int h) {
    std::vector<unsigned char> px;
    GrabTopDown(fbo, w, h, px);
    return px;
}

std::string WritePngTo(const std::string& dir, const std::string& name, int w, int h, const std::vector<unsigned char>& rgba) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string out = (std::filesystem::path(dir) / (name + ".png")).string();
    if (!PngWrite::ToFile(std::filesystem::path(out).u8string(), w, h, 4, rgba.data(), w * 4)) {
        std::cerr << "[FrameCapture] FAILED to write " << out << std::endl;
        return {};
    }
    return out;
}

} // namespace FrameCapture
