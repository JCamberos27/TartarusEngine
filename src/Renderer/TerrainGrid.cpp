#include "TerrainGrid.h"
#include "ModelVertex.h"

#include <algorithm>
#include <cmath>

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool B64Decode(const std::string& in, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(in.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = B64Value(c);
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

std::string B64Encode(const std::vector<std::uint8_t>& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned b0 = in[i], b1 = i + 1 < in.size() ? in[i + 1] : 0u, b2 = i + 2 < in.size() ? in[i + 2] : 0u;
        const unsigned v = (b0 << 16) | (b1 << 8) | b2;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[v & 63] : '=';
    }
    return out;
}
} // namespace

namespace {
// The cell and in-cell fractions of a local point, clamped to the grid.
void Locate(const TerrainGrid& g, float x, float z, int& i, int& j, float& fx, float& fz) {
    const float gx = std::clamp((x + 0.5f * g.SizeX) / g.CellX(), 0.0f, (float)(g.ResX - 1));
    const float gz = std::clamp((z + 0.5f * g.SizeZ) / g.CellZ(), 0.0f, (float)(g.ResZ - 1));
    i = std::min((int)gx, g.ResX - 2);
    j = std::min((int)gz, g.ResZ - 2);
    fx = gx - (float)i;
    fz = gz - (float)j;
}
} // namespace

float TerrainGrid::Height(float x, float z) const {
    if (!Valid()) return 0.0f;
    int i, j;
    float fx, fz;
    Locate(*this, x, z, i, j, fx, fz);
    const float h00 = At(i, j), h10 = At(i + 1, j), h01 = At(i, j + 1), h11 = At(i + 1, j + 1);
    if (fx + fz <= 1.0f) return h00 + fx * (h10 - h00) + fz * (h01 - h00);
    return h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
}

glm::vec3 TerrainGrid::Normal(float x, float z) const {
    if (!Valid()) return glm::vec3(0.0f, 1.0f, 0.0f);
    int i, j;
    float fx, fz;
    Locate(*this, x, z, i, j, fx, fz);
    const float cx = CellX(), cz = CellZ();
    const float h00 = At(i, j), h10 = At(i + 1, j), h01 = At(i, j + 1), h11 = At(i + 1, j + 1);
    float dx, dz; // slope along X and Z on this triangle
    if (fx + fz <= 1.0f) { dx = (h10 - h00) / cx; dz = (h01 - h00) / cz; }
    else                 { dx = (h11 - h01) / cx; dz = (h11 - h10) / cz; }
    return glm::normalize(glm::vec3(-dx, 1.0f, -dz));
}

void TerrainGrid::RangeIn(float x0, float z0, float x1, float z1, float& outMin, float& outMax) const {
    outMin = outMax = Height(x0, z0);
    if (!Valid()) return;
    for (float h : {Height(x1, z0), Height(x0, z1), Height(x1, z1)}) { outMin = std::min(outMin, h); outMax = std::max(outMax, h); }
    const float cx = CellX(), cz = CellZ();
    const int i0 = std::max(0, (int)std::ceil((std::min(x0, x1) + 0.5f * SizeX) / cx));
    const int i1 = std::min(ResX - 1, (int)std::floor((std::max(x0, x1) + 0.5f * SizeX) / cx));
    const int j0 = std::max(0, (int)std::ceil((std::min(z0, z1) + 0.5f * SizeZ) / cz));
    const int j1 = std::min(ResZ - 1, (int)std::floor((std::max(z0, z1) + 0.5f * SizeZ) / cz));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) { const float h = At(i, j); outMin = std::min(outMin, h); outMax = std::max(outMax, h); }
}

void TerrainGrid::BuildMesh(float uvTile, std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices) const {
    vertices.clear();
    indices.clear();
    if (!Valid()) return;
    const float cx = CellX(), cz = CellZ();
    const float inv = uvTile > 1e-4f ? 1.0f / uvTile : 1.0f;
    vertices.resize((size_t)ResX * (size_t)ResZ);
    for (int j = 0; j < ResZ; ++j) {
        for (int i = 0; i < ResX; ++i) {
            ModelVertex& v = vertices[(size_t)j * (size_t)ResX + (size_t)i];
            const float x = -0.5f * SizeX + (float)i * cx, z = -0.5f * SizeZ + (float)j * cz;
            v.Position = glm::vec3(x, At(i, j), z);
            const float hl = At(std::max(i - 1, 0), j), hr = At(std::min(i + 1, ResX - 1), j);
            const float hd = At(i, std::max(j - 1, 0)), hu = At(i, std::min(j + 1, ResZ - 1));
            const float dx = (hr - hl) / (cx * (float)(std::min(i + 1, ResX - 1) - std::max(i - 1, 0)));
            const float dz = (hu - hd) / (cz * (float)(std::min(j + 1, ResZ - 1) - std::max(j - 1, 0)));
            v.Normal = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
            v.Tangent = glm::normalize(glm::vec3(1.0f, dx, 0.0f));
            v.TangentSign = 1.0f;
            v.UV = glm::vec2(x, z) * inv;
        }
    }
    indices.reserve((size_t)(ResX - 1) * (size_t)(ResZ - 1) * 6);
    for (int j = 0; j + 1 < ResZ; ++j) {
        for (int i = 0; i + 1 < ResX; ++i) {
            const unsigned a = (unsigned)(j * ResX + i), b = a + 1, c = a + (unsigned)ResX, d = c + 1;
            // Counter-clockwise seen from above (+Y), split along b-c (Height()'s diagonal).
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
}

bool TerrainGrid::Decode(int resX, int resZ, float sizeX, float sizeZ, float heightMin, float heightMax,
                         const std::string& base64, TerrainGrid& out) {
    out = TerrainGrid{};
    if (resX < 2 || resZ < 2 || !(sizeX > 0.0f) || !(sizeZ > 0.0f) || resX > 4097 || resZ > 4097) return false;
    std::vector<std::uint8_t> bytes;
    if (!B64Decode(base64, bytes) || bytes.size() != (size_t)resX * (size_t)resZ * 2) return false;
    out.ResX = resX;
    out.ResZ = resZ;
    out.SizeX = sizeX;
    out.SizeZ = sizeZ;
    out.Heights.resize((size_t)resX * (size_t)resZ);
    const float scale = (heightMax - heightMin) / 65535.0f;
    out.MinHeight = 1e30f;
    out.MaxHeight = -1e30f;
    for (size_t k = 0; k < out.Heights.size(); ++k) {
        const unsigned q = (unsigned)bytes[2 * k] | ((unsigned)bytes[2 * k + 1] << 8); // little-endian
        const float h = heightMin + (float)q * scale;
        out.Heights[k] = h;
        out.MinHeight = std::min(out.MinHeight, h);
        out.MaxHeight = std::max(out.MaxHeight, h);
    }
    return true;
}

std::string TerrainGrid::Encode(const std::vector<float>& heights, float heightMin, float heightMax) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(heights.size() * 2);
    const float range = heightMax - heightMin;
    for (float h : heights) {
        const float t = range > 0.0f ? std::clamp((h - heightMin) / range, 0.0f, 1.0f) : 0.0f;
        const unsigned q = (unsigned)std::lround(t * 65535.0f);
        bytes.push_back((std::uint8_t)(q & 0xFFu));
        bytes.push_back((std::uint8_t)(q >> 8));
    }
    return B64Encode(bytes);
}
