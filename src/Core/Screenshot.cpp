#include "Screenshot.h"
#include "ProjectPaths.h"
#include "Log.h"
#include "gl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <filesystem>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace Screenshot {

std::string Dir() {
    const std::string d = ProjectPaths::Resolve("screenshots");
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

std::vector<unsigned char> GrabRegion(int x, int y, int w, int h) {
    std::vector<unsigned char> px((size_t)std::max(w, 0) * std::max(h, 0) * 4);
    if (w > 0 && h > 0)
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

static std::string SanitizeName(std::string s) {
    if (s.empty()) return "untitled";
    for (char& c : s)
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return s;
}

std::string Save(const unsigned char* pixels, int w, int h, bool flipY,
                 int format, const std::string& sceneName) {
    if (!pixels || w <= 0 || h <= 0) return {};

    const size_t stride = (size_t)w * 4;
    std::vector<unsigned char> img((size_t)w * h * 4);
    if (flipY) {
        for (int row = 0; row < h; ++row)
            std::memcpy(&img[(size_t)(h - 1 - row) * stride], &pixels[(size_t)row * stride], stride);
    } else {
        std::memcpy(img.data(), pixels, img.size());
    }

    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const bool jpg = (format == 1);
    char name[128];
    std::snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d.%s",
                  SanitizeName(sceneName).c_str(),
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, jpg ? "jpg" : "png");
    const std::string path = (std::filesystem::path(Dir()) / name).generic_string();

    int ok;
    if (jpg) {
        ok = stbi_write_jpg(path.c_str(), w, h, 4, img.data(), 92);
    } else {
        stbi_write_png_compression_level = 6;
        ok = stbi_write_png(path.c_str(), w, h, 4, img.data(), w * 4);
    }
    if (ok) {
        Log::Info("Screenshot -> " + path + "  (" + std::to_string(w) + "x" + std::to_string(h) + ")");
        return path;
    }
    Log::Error("Screenshot: couldn't write " + path);
    return {};
}

std::string SaveBackbuffer(int width, int height, int format, const std::string& sceneName) {
    if (width <= 0 || height <= 0) return {};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<unsigned char> px = GrabRegion(0, 0, width, height);
    return Save(px.data(), width, height, /*flipY=*/true, format, sceneName);
}

void ShowInFolder(const std::string& path) {
#if defined(_WIN32)
    std::string arg = "/select,\"" + std::filesystem::path(path).make_preferred().string() + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

std::string ShutterClipPath() {
    const std::string path = (std::filesystem::path(Dir()) / ".shutter.wav").generic_string();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;

    // A short "click-clack": two decaying filtered-noise bursts. Mono 16-bit @ 44100.
    const int rate = 44100;
    const int n = rate * 14 / 100; // 140 ms
    std::vector<int16_t> s((size_t)n, 0);
    uint32_t rng = 0x9E3779B9u;
    auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((int)(rng >> 9) & 0x3FFF) / 8192.0f - 1.0f; };
    float lp = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t1 = (float)i / rate;
        float t2 = t1 - 0.055f;
        float env = 0.0f;
        if (t1 >= 0.0f) env += std::exp(-t1 * 140.0f);
        if (t2 >= 0.0f) env += 0.7f * std::exp(-t2 * 120.0f);
        float x = noise() * env;
        lp += 0.45f * (x - lp);           // gentle low-pass so it's a "thock", not a hiss
        int v = (int)(lp * 22000.0f);
        s[i] = (int16_t)(v < -32767 ? -32767 : (v > 32767 ? 32767 : v));
    }

    struct { char r[4]; uint32_t sz; char w[4]; char f[4]; uint32_t f16; uint16_t fmt, ch;
             uint32_t sr, br; uint16_t ba, bps; char d[4]; uint32_t ds; } h = {
        {'R','I','F','F'}, 0, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, 1,
        (uint32_t)rate, (uint32_t)rate * 2, 2, 16, {'d','a','t','a'}, (uint32_t)(s.size() * 2) };
    h.sz = 36 + h.ds;

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return {};
    std::fwrite(&h, sizeof(h), 1, fp);
    std::fwrite(s.data(), 2, s.size(), fp);
    std::fclose(fp);
    return path;
}

} // namespace Screenshot
