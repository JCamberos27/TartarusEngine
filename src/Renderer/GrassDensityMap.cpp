#include "GrassDensityMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Base64 {
namespace {
const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

bool Decode(const std::string& in, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(in.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = Value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((std::uint8_t)((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

std::string Encode(const std::vector<std::uint8_t>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned b0 = in[i], b1 = i + 1 < in.size() ? in[i + 1] : 0u, b2 = i + 2 < in.size() ? in[i + 2] : 0u;
        const unsigned v = (b0 << 16) | (b1 << 8) | b2;
        out += kChars[(v >> 18) & 63];
        out += kChars[(v >> 12) & 63];
        out += i + 1 < in.size() ? kChars[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kChars[v & 63] : '=';
    }
    return out;
}
} // namespace Base64

float GrassDensityMap::Sample(float u, float v) const {
    if (!Valid()) return 1.0f;
    const float gx = std::clamp(u, 0.0f, 1.0f) * (float)(ResX - 1), gz = std::clamp(v, 0.0f, 1.0f) * (float)(ResZ - 1);
    const int i = std::min((int)gx, ResX - 2), j = std::min((int)gz, ResZ - 2);
    const float fx = gx - (float)i, fz = gz - (float)j;
    const float a = At(i, j) + (At(i + 1, j) - At(i, j)) * fx;
    const float b = At(i, j + 1) + (At(i + 1, j + 1) - At(i, j + 1)) * fx;
    return a + (b - a) * fz;
}

float GrassDensityMap::MaxIn(float u0, float v0, float u1, float v1) const {
    if (!Valid()) return 1.0f;
    const auto col = [&](float u, bool hi) {
        const float g = std::clamp(u, 0.0f, 1.0f) * (float)(ResX - 1);
        return std::clamp(hi ? (int)std::ceil(g) : (int)std::floor(g), 0, ResX - 1);
    };
    const auto row = [&](float v, bool hi) {
        const float g = std::clamp(v, 0.0f, 1.0f) * (float)(ResZ - 1);
        return std::clamp(hi ? (int)std::ceil(g) : (int)std::floor(g), 0, ResZ - 1);
    };
    const int i0 = col(std::min(u0, u1), false), i1 = col(std::max(u0, u1), true);
    const int j0 = row(std::min(v0, v1), false), j1 = row(std::max(v0, v1), true);
    std::uint8_t m = 0;
    for (int j = j0; j <= j1 && m < 255; ++j)
        for (int i = i0; i <= i1; ++i) m = std::max(m, Values[(size_t)j * (size_t)ResX + (size_t)i]);
    return m * (1.0f / 255.0f);
}

const std::vector<std::uint8_t>& GrassDensityMap::BareChunks(float sx, float sz, float cell, int cpc, int& outCols) const {
    const float key[4] = {sx, sz, cell, (float)cpc};
    if (std::memcmp(key, m_BareFor, sizeof(key)) != 0) {
        std::memcpy(m_BareFor, key, sizeof(key));
        const int cellsX = (int)std::ceil(sx / cell), cellsZ = (int)std::ceil(sz / cell);
        m_BareCols = (cellsX + cpc - 1) / cpc;
        const int rows = (cellsZ + cpc - 1) / cpc;
        m_Bare.assign((size_t)m_BareCols * (size_t)rows, 0);
        for (int r = 0; r < rows; ++r)
            for (int q = 0; q < m_BareCols; ++q) {
                const float x0 = -0.5f * sx + q * cpc * cell, z0 = -0.5f * sz + r * cpc * cell;
                const float x1 = std::min(x0 + cpc * cell, 0.5f * sx), z1 = std::min(z0 + cpc * cell, 0.5f * sz);
                m_Bare[(size_t)r * m_BareCols + q] = MaxIn(x0 / sx + 0.5f, z0 / sz + 0.5f, x1 / sx + 0.5f, z1 / sz + 0.5f) > 0.0f ? 0 : 1;
            }
    }
    outCols = m_BareCols;
    return m_Bare;
}

bool GrassDensityMap::Decode(int resX, int resZ, const std::string& base64, GrassDensityMap& out) {
    out = GrassDensityMap{};
    if (resX < 2 || resZ < 2 || resX > 8192 || resZ > 8192) return false;
    std::vector<std::uint8_t> bytes;
    if (!Base64::Decode(base64, bytes) || bytes.size() != (size_t)resX * (size_t)resZ) return false;
    out.ResX = resX;
    out.ResZ = resZ;
    out.Values = std::move(bytes);
    return true;
}

std::string GrassDensityMap::Encode(const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes(values.size());
    for (size_t k = 0; k < values.size(); ++k)
        bytes[k] = (std::uint8_t)std::lround(std::clamp(values[k], 0.0f, 1.0f) * 255.0f);
    return Base64::Encode(bytes);
}
