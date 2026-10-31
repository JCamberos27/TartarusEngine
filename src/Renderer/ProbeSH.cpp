#include "ProbeSH.h"
#include <cmath>
#include <cstdlib>

namespace ProbeSH {

std::array<float, 9> Basis(const glm::vec3& d) {
    return {0.282095f,
            0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
            1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.0f * d.z * d.z - 1.0f),
            1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y)};
}

glm::vec3 TexelDirection(int face, int x, int y, int size) {
    const float sc = 2.0f * ((float)x + 0.5f) / (float)size - 1.0f;
    const float tc = 2.0f * ((float)y + 0.5f) / (float)size - 1.0f;
    glm::vec3 d(0.0f);
    switch (face) { // the GL spec's major-axis table, solved for the direction
        case 0: d = glm::vec3(1.0f, -tc, -sc); break;
        case 1: d = glm::vec3(-1.0f, -tc, sc); break;
        case 2: d = glm::vec3(sc, 1.0f, tc); break;
        case 3: d = glm::vec3(sc, -1.0f, -tc); break;
        case 4: d = glm::vec3(sc, -tc, 1.0f); break;
        default: d = glm::vec3(-sc, -tc, -1.0f); break;
    }
    return glm::normalize(d);
}

namespace {
float AreaElement(float x, float y) { return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f)); }
}

float TexelSolidAngle(int x, int y, int size) {
    const float inv = 1.0f / (float)size;
    const float x0 = 2.0f * (float)x * inv - 1.0f, x1 = x0 + 2.0f * inv;
    const float y0 = 2.0f * (float)y * inv - 1.0f, y1 = y0 + 2.0f * inv;
    return AreaElement(x0, y0) - AreaElement(x0, y1) - AreaElement(x1, y0) + AreaElement(x1, y1);
}

Coeffs Project(const float* rgbaFaces, int size) {
    Coeffs c{};
    for (glm::vec3& v : c) v = glm::vec3(0.0f);
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const float* px = rgbaFaces + (((size_t)f * size + y) * size + x) * 4;
                const glm::vec3 radiance(px[0], px[1], px[2]);
                const float w = TexelSolidAngle(x, y, size);
                const std::array<float, 9> b = Basis(TexelDirection(f, x, y, size));
                for (int i = 0; i < 9; ++i) c[i] += radiance * (b[i] * w);
            }
    return c;
}

glm::vec3 Evaluate(const Coeffs& c, const glm::vec3& d) {
    const std::array<float, 9> b = Basis(d);
    glm::vec3 r(0.0f);
    for (int i = 0; i < 9; ++i) r += c[i] * b[i];
    return r;
}

bool Enabled() {
    static const bool on = [] {
        char* e = nullptr;
        size_t n = 0;
        const bool off = _dupenv_s(&e, &n, "TARTARUS_NO_PROBE_SH") == 0 && e && e[0] == '1';
        std::free(e);
        return !off;
    }();
    return on;
}
}
