#include "Screenshot.h"
#include "ProjectPaths.h"
#include "Log.h"
#include "gl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <vector>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace Screenshot {

std::string SaveBackbuffer(int width, int height) {
    if (width <= 0 || height <= 0) return {};

    std::vector<unsigned char> px((size_t)width * height * 4);
    // FBO 0's read buffer is GL_BACK by default for a double-buffered context; RGBA8 rows are
    // always 4-byte aligned so no glPixelStorei is needed.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    // OpenGL reads bottom-left origin; flip rows so the PNG is the right way up.
    std::vector<unsigned char> img((size_t)width * height * 4);
    const size_t stride = (size_t)width * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(&img[(size_t)(height - 1 - y) * stride], &px[(size_t)y * stride], stride);

    std::error_code ec;
    const std::string dir = ProjectPaths::Resolve("screenshots");
    std::filesystem::create_directories(dir, ec);

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "shot_%04d%02d%02d_%02d%02d%02d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    const std::string path = (std::filesystem::path(dir) / name).generic_string();

    stbi_write_png_compression_level = 6;
    if (stbi_write_png(path.c_str(), width, height, 4, img.data(), width * 4)) {
        Log::Info("Screenshot saved: " + path);
        return path;
    }
    Log::Error("Screenshot: couldn't write " + path);
    return {};
}

} // namespace Screenshot
