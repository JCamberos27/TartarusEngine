#include "BloodFxImport.h"

#include "Log.h"
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace BloodFxImport {
namespace {

void Fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
}

bool ReadAll(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

bool ReadText(const std::string& path, std::string& out) {
    std::vector<std::uint8_t> bytes;
    if (!ReadAll(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

template <typename T> T Get(const std::vector<std::uint8_t>& b, size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

// A Unity material's float (`- _name: 1.5`) or colour (`- _name: {r: .., g: .., b: ..}`) property.
bool MatFloat(const std::string& text, const std::string& name, float& out) {
    const std::string key = "- " + name + ": ";
    for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + 1)) {
        const char* s = text.c_str() + p + key.size();
        if (*s == '{') continue;
        out = std::strtof(s, nullptr);
        return true;
    }
    return false;
}
bool MatColor(const std::string& text, const std::string& name, float out[3]) {
    const std::string key = "- " + name + ": {";
    const size_t p = text.find(key);
    if (p == std::string::npos) return false;
    const char* tags[3] = {"r: ", "g: ", "b: "};
    for (int i = 0; i < 3; ++i) {
        const size_t q = text.find(tags[i], p);
        if (q == std::string::npos) return false;
        out[i] = std::strtof(text.c_str() + q + 3, nullptr);
    }
    return true;
}

// Reads the comma-separated numbers of an ASCII FBX array (`Name: *N { a: 1,2,3 }`) found at or after `from`.
bool FbxArray(const std::string& text, const std::string& name, size_t from, std::vector<double>& out, size_t* end = nullptr) {
    const size_t p = text.find(name + ": *", from);
    if (p == std::string::npos) return false;
    const size_t count = std::strtoull(text.c_str() + p + name.size() + 3, nullptr, 10);
    const size_t a = text.find("a:", p);
    if (a == std::string::npos) return false;
    out.clear();
    out.reserve(count);
    const char* s = text.c_str() + a + 2;
    for (size_t i = 0; i < count; ++i) {
        char* next = nullptr;
        const double v = std::strtod(s, &next);
        if (next == s) return false;
        out.push_back(v);
        s = next;
        while (*s == ',' || *s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') ++s;
    }
    if (end) *end = (size_t)(s - text.c_str());
    return true;
}

} // namespace

int ExrImage::ChannelIndex(const std::string& name) const {
    for (size_t i = 0; i < Channels.size(); ++i)
        if (Channels[i] == name) return (int)i;
    return -1;
}

float HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (std::uint32_t)(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // subnormal: normalise
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::uint16_t FloatToHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint16_t sign = (std::uint16_t)((x >> 16) & 0x8000u);
    const int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
    const std::uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) return (std::uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp >= 31) return (std::uint16_t)(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return sign;
        const std::uint32_t m = (mant | 0x800000u) >> (1 - exp);
        return (std::uint16_t)(sign | ((m + 0x1000u) >> 13));
    }
    std::uint32_t h = ((std::uint32_t)exp << 10) | (mant >> 13);
    if (mant & 0x1000u) ++h; // round half up (carries into the exponent correctly)
    return (std::uint16_t)(sign | h);
}

bool ReadExr(const std::vector<std::uint8_t>& b, ExrImage& out, std::string* error) {
    if (b.size() < 8 || Get<std::uint32_t>(b, 0) != 20000630u) { Fail(error, "not an OpenEXR file"); return false; }
    const std::uint32_t version = Get<std::uint32_t>(b, 4);
    if (version & 0x200u) { Fail(error, "tiled EXR is not supported"); return false; }
    if (version & 0x1000u) { Fail(error, "multi-part EXR is not supported"); return false; }
    size_t p = 8;
    struct Chan { std::string Name; int Type; };
    std::vector<Chan> chans;
    int compression = -1, x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    while (p < b.size() && b[p] != 0) {
        const size_t nameEnd = std::find(b.begin() + p, b.end(), 0) - b.begin();
        const std::string name(b.begin() + p, b.begin() + nameEnd);
        p = nameEnd + 1;
        const size_t typeEnd = std::find(b.begin() + p, b.end(), 0) - b.begin();
        p = typeEnd + 1;
        if (p + 4 > b.size()) break;
        const int size = Get<int>(b, p);
        p += 4;
        if (size < 0 || p + (size_t)size > b.size()) { Fail(error, "truncated EXR header"); return false; }
        if (name == "channels") {
            size_t q = p;
            while (q < p + size && b[q] != 0) {
                const size_t e = std::find(b.begin() + q, b.begin() + p + size, 0) - b.begin();
                Chan c{std::string(b.begin() + q, b.begin() + e), Get<int>(b, e + 1)};
                if (Get<int>(b, e + 9) != 1 || Get<int>(b, e + 13) != 1) { Fail(error, "subsampled EXR channels are not supported"); return false; }
                chans.push_back(c);
                q = e + 17;
            }
        } else if (name == "compression") {
            compression = b[p];
        } else if (name == "dataWindow") {
            x0 = Get<int>(b, p); y0 = Get<int>(b, p + 4); x1 = Get<int>(b, p + 8); y1 = Get<int>(b, p + 12);
        }
        p += size;
    }
    ++p; // header terminator
    if (chans.empty() || x1 < x0 || y1 < y0) { Fail(error, "EXR header has no channels / data window"); return false; }
    if (compression != 0 && compression != 2 && compression != 3) { Fail(error, "EXR compression " + std::to_string(compression) + " is not supported (NONE/ZIPS/ZIP only)"); return false; }
    for (const Chan& c : chans)
        if (c.Type != 1 && c.Type != 2) { Fail(error, "EXR channel '" + c.Name + "' is not HALF or FLOAT"); return false; }

    out.Width = x1 - x0 + 1;
    out.Height = y1 - y0 + 1;
    out.Channels.clear();
    for (const Chan& c : chans) out.Channels.push_back(c.Name);
    out.Pixels.assign((size_t)out.Width * out.Height * chans.size(), 0.0f);

    size_t lineBytes = 0;
    for (const Chan& c : chans) lineBytes += (size_t)out.Width * (c.Type == 1 ? 2 : 4);
    const int linesPerBlock = compression == 3 ? 16 : 1;
    const int blocks = (out.Height + linesPerBlock - 1) / linesPerBlock;
    if (p + (size_t)blocks * 8 > b.size()) { Fail(error, "truncated EXR offset table"); return false; }
    std::vector<std::uint8_t> raw, tmp;
    for (int bi = 0; bi < blocks; ++bi) {
        const std::uint64_t off = Get<std::uint64_t>(b, p + (size_t)bi * 8);
        if (off + 8 > b.size()) { Fail(error, "EXR block offset out of range"); return false; }
        const int y = Get<int>(b, (size_t)off) - y0;
        const int size = Get<int>(b, (size_t)off + 4);
        if (size < 0 || off + 8 + (std::uint64_t)size > b.size() || y < 0 || y >= out.Height) { Fail(error, "bad EXR block"); return false; }
        const int lines = std::min(linesPerBlock, out.Height - y);
        const size_t rawSize = lineBytes * lines;
        const std::uint8_t* src = b.data() + off + 8;
        // A block no smaller than its raw size was stored raw (OpenEXR's rule) - unless it carries a zlib
        // header, which only a writer that ignored the rule would leave (tests write stored deflate blocks).
        const bool zlibHeader = size >= 2 && (src[0] & 0x0F) == 8 && ((src[0] << 8) | src[1]) % 31 == 0;
        if (compression != 0 && ((size_t)size < rawSize || ((size_t)size != rawSize && zlibHeader))) {
            tmp.resize(rawSize);
            const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(tmp.data()), (int)rawSize,
                                                    reinterpret_cast<const char*>(src), size);
            if (got != (int)rawSize) { Fail(error, "EXR zlib block failed to inflate"); return false; }
            for (size_t i = 1; i < rawSize; ++i) tmp[i] = (std::uint8_t)(tmp[i - 1] + tmp[i] - 128);
            raw.resize(rawSize);
            const size_t half = (rawSize + 1) / 2;
            for (size_t i = 0; i < half; ++i) raw[2 * i] = tmp[i];
            for (size_t i = 0; half + i < rawSize; ++i) raw[2 * i + 1] = tmp[half + i];
        } else {
            if ((size_t)size != rawSize) { Fail(error, "EXR uncompressed block has the wrong size"); return false; }
            raw.assign(src, src + rawSize);
        }
        size_t q = 0;
        for (int l = 0; l < lines; ++l) {
            for (size_t c = 0; c < chans.size(); ++c) {
                float* dst = &out.Pixels[((size_t)(y + l) * chans.size() + c) * out.Width];
                for (int x = 0; x < out.Width; ++x) {
                    if (chans[c].Type == 1) {
                        std::uint16_t h;
                        std::memcpy(&h, raw.data() + q, 2);
                        dst[x] = HalfToFloat(h);
                        q += 2;
                    } else {
                        std::memcpy(&dst[x], raw.data() + q, 4);
                        q += 4;
                    }
                }
            }
        }
    }
    return true;
}

float LinearToGamma(float v) {
    v = std::max(v, 0.0f);
    return std::max(1.055f * std::pow(v, 0.416666667f) - 0.055f, 0.0f);
}

std::uint16_t PackOctNormal(float x, float y, float z) {
    const float l1 = std::abs(x) + std::abs(y) + std::abs(z);
    if (l1 < 1e-20f) return (std::uint16_t)((128u << 8) | 128u);
    x /= l1; y /= l1; z /= l1;
    float u = x, v = z; // y is the "up" pole: the octahedron is folded over the xz plane
    if (y < 0.0f) {
        u = (1.0f - std::abs(z)) * (x >= 0.0f ? 1.0f : -1.0f);
        v = (1.0f - std::abs(x)) * (z >= 0.0f ? 1.0f : -1.0f);
    }
    const auto q = [](float s) { return (std::uint32_t)std::clamp((int)std::lround((s * 0.5f + 0.5f) * 255.0f), 0, 255); };
    return (std::uint16_t)((q(u) << 8) | q(v));
}

void UnpackOctNormal(std::uint16_t packed, float& x, float& y, float& z) {
    const float u = (float)(packed >> 8) / 255.0f * 2.0f - 1.0f;
    const float v = (float)(packed & 0xFFu) / 255.0f * 2.0f - 1.0f;
    x = u;
    z = v;
    y = 1.0f - std::abs(u) - std::abs(v);
    if (y < 0.0f) {
        const float ox = x, oz = z;
        x = (1.0f - std::abs(oz)) * (ox >= 0.0f ? 1.0f : -1.0f);
        z = (1.0f - std::abs(ox)) * (oz >= 0.0f ? 1.0f : -1.0f);
    }
    const float l = std::sqrt(x * x + y * y + z * z);
    x /= l; y /= l; z /= l;
}

bool WriteVat(const std::string& path, const VatData& vat) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(&vat.Header), sizeof(VatHeader));
    f.write(reinterpret_cast<const char*>(vat.Frames.data()), (std::streamsize)(vat.Frames.size() * sizeof(VatFrame)));
    f.write(reinterpret_cast<const char*>(vat.Texels.data()), (std::streamsize)(vat.Texels.size() * sizeof(std::uint16_t)));
    return (bool)f;
}

bool ReadVat(const std::string& path, VatData& vat, std::string* error) {
    std::vector<std::uint8_t> b;
    if (!ReadAll(path, b)) { Fail(error, "can't read " + path); return false; }
    if (b.size() < sizeof(VatHeader)) { Fail(error, "truncated"); return false; }
    std::memcpy(&vat.Header, b.data(), sizeof(VatHeader));
    const VatHeader& h = vat.Header;
    if (h.Magic != kVatMagic || h.Version != kVatVersion) { Fail(error, "not a version " + std::to_string(kVatVersion) + " .bvat"); return false; }
    if (h.Frames == 0 || h.Frames > 4096 || h.TexWidth == 0 || h.TexWidth > 8192 || h.RowsPerFrame == 0 || h.TrisPerRow == 0 ||
        h.VertexCount == 0 || h.VertexCount % 3 != 0 || h.VertexCount / 3 > (std::uint64_t)h.TrisPerRow * h.RowsPerFrame ||
        3u * h.TrisPerRow > h.TexWidth) {
        Fail(error, "bad header");
        return false;
    }
    const size_t framesBytes = (size_t)h.Frames * sizeof(VatFrame);
    const size_t texels = (size_t)h.Frames * h.RowsPerFrame * h.TexWidth * 4;
    if (b.size() != sizeof(VatHeader) + framesBytes + texels * 2) { Fail(error, "size doesn't match the header"); return false; }
    vat.Frames.resize(h.Frames);
    std::memcpy(vat.Frames.data(), b.data() + sizeof(VatHeader), framesBytes);
    vat.Texels.resize(texels);
    std::memcpy(vat.Texels.data(), b.data() + sizeof(VatHeader) + framesBytes, texels * 2);
    return true;
}

bool ReadFbxAsciiUVs(const std::string& text, std::vector<float>& uvs, std::size_t& polygonVertices, std::string* error) {
    if (text.rfind("; FBX", 0) != 0) { Fail(error, "not an ASCII FBX"); return false; }
    std::vector<double> poly, uv, uvIndex;
    if (!FbxArray(text, "PolygonVertexIndex", 0, poly)) { Fail(error, "no PolygonVertexIndex"); return false; }
    for (size_t i = 0; i < poly.size(); ++i)
        if ((poly[i] < 0) != (i % 3 == 2)) { Fail(error, "the mesh isn't all triangles"); return false; }
    const size_t layer = text.find("LayerElementUV:");
    if (layer == std::string::npos) { Fail(error, "no UV layer"); return false; }
    size_t afterUv = 0;
    if (!FbxArray(text, "UV", layer, uv, &afterUv) || !FbxArray(text, "UVIndex", afterUv, uvIndex)) { Fail(error, "no UV / UVIndex arrays"); return false; }
    const size_t mapping = text.find("MappingInformationType: \"ByPolygonVertex\"", layer);
    if (mapping == std::string::npos || mapping > afterUv) { Fail(error, "UVs aren't per polygon vertex"); return false; }
    if (uvIndex.size() != poly.size()) { Fail(error, "UVIndex count != polygon vertex count"); return false; }
    polygonVertices = poly.size();
    uvs.resize(polygonVertices * 2);
    for (size_t i = 0; i < polygonVertices; ++i) {
        const size_t k = (size_t)uvIndex[i];
        if (2 * k + 1 >= uv.size()) { Fail(error, "UVIndex out of range"); return false; }
        uvs[2 * i] = (float)uv[2 * k];
        uvs[2 * i + 1] = (float)uv[2 * k + 1];
    }
    return true;
}

bool BuildVat(const ExrImage& pos, const ExrImage& nrm, const std::vector<float>& uvs, std::size_t polygonVertices,
              int frames, float boundsMin, float boundsMax, const float heightOffset[3], VatData& out, std::string* error) {
    const int W = pos.Width, H = pos.Height;
    if (nrm.Width != W || nrm.Height != H) { Fail(error, "position and normal textures differ in size"); return false; }
    if (frames <= 0 || H % frames != 0) { Fail(error, "texture height " + std::to_string(H) + " isn't a whole number of " + std::to_string(frames) + " frames"); return false; }
    if (polygonVertices == 0 || polygonVertices % 3 != 0 || W < 3) { Fail(error, "bad vertex count"); return false; }
    const int pr = pos.ChannelIndex("R"), pg = pos.ChannelIndex("G"), pb = pos.ChannelIndex("B");
    const int nr = nrm.ChannelIndex("R"), ng = nrm.ChannelIndex("G"), nb = nrm.ChannelIndex("B");
    if (pr < 0 || pg < 0 || pb < 0 || nr < 0 || ng < 0 || nb < 0) { Fail(error, "textures need R, G and B channels"); return false; }

    const std::uint32_t rpf = (std::uint32_t)(H / frames);
    const std::uint32_t tpr = (std::uint32_t)(W / 3);
    const std::uint32_t tris = (std::uint32_t)(polygonVertices / 3);
    if ((tris + tpr - 1) / tpr > rpf) { Fail(error, "the soup doesn't fit a frame's rows"); return false; }
    // The soup's UVs must be exactly the gl_VertexID layout the shader assumes.
    for (std::uint32_t v = 0; v < polygonVertices; ++v) {
        std::uint32_t x, row;
        VatTexel(v, tpr, x, row);
        const int ux = (int)std::floor(uvs[2 * v] * W);
        const int uy = (int)std::floor((1.0f - uvs[2 * v + 1]) * H);
        if (ux != (int)x || uy != (int)row) {
            Fail(error, "vertex " + std::to_string(v) + "'s UV reads texel (" + std::to_string(ux) + ", " + std::to_string(uy) +
                            "), not the expected (" + std::to_string(x) + ", " + std::to_string(row) + ")");
            return false;
        }
    }

    VatHeader& h = out.Header;
    h = VatHeader{};
    h.Frames = (std::uint32_t)frames;
    h.VertexCount = (std::uint32_t)polygonVertices;
    h.TexWidth = (std::uint32_t)W;
    h.RowsPerFrame = rpf;
    h.TrisPerRow = tpr;
    out.Frames.assign(frames, VatFrame{});
    out.Texels.assign((size_t)frames * rpf * W * 4, 0);

    const float expand = boundsMax - boundsMin;
    std::vector<float> P(polygonVertices * 3), N(polygonVertices * 3);
    std::vector<char> live(tris);
    float allMin[3] = {1e30f, 1e30f, 1e30f}, allMax[3] = {-1e30f, -1e30f, -1e30f};
    double windingSum = 0.0;
    for (int f = 0; f < frames; ++f) {
        // Decode the frame into Unity mesh space, then mirror z into this engine's handedness.
        for (std::uint32_t v = 0; v < polygonVertices; ++v) {
            std::uint32_t x, row;
            VatTexel(v, tpr, x, row);
            const int y = f * (int)rpf + (int)row;
            const float px = LinearToGamma(pos.At(x, y, pr)) * expand + boundsMin;
            const float py = LinearToGamma(pos.At(x, y, pg)) * expand + boundsMin;
            const float pz = LinearToGamma(pos.At(x, y, pb)) * expand + boundsMin;
            P[3 * v + 0] = -px + heightOffset[0];
            P[3 * v + 1] = pz + heightOffset[1];
            P[3 * v + 2] = -(py + heightOffset[2]);
            const float qx = LinearToGamma(nrm.At(x, y, nr)) * 2.0f - 1.0f;
            const float qy = LinearToGamma(nrm.At(x, y, ng)) * 2.0f - 1.0f;
            const float qz = LinearToGamma(nrm.At(x, y, nb)) * 2.0f - 1.0f;
            N[3 * v + 0] = -qx;
            N[3 * v + 1] = qz;
            N[3 * v + 2] = -qy;
        }
        VatFrame& fr = out.Frames[f];
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        double c[3] = {0, 0, 0};
        std::uint32_t liveTris = 0;
        for (std::uint32_t t = 0; t < tris; ++t) {
            const float* a = &P[9 * t];
            const float e1[3] = {a[3] - a[0], a[4] - a[1], a[5] - a[2]};
            const float e2[3] = {a[6] - a[0], a[7] - a[1], a[8] - a[2]};
            const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float area2 = cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2];
            live[t] = area2 > 1e-12f;
            if (!live[t]) continue;
            ++liveTris;
            for (int k = 0; k < 3; ++k)
                for (int i = 0; i < 3; ++i) {
                    mn[i] = std::min(mn[i], a[3 * k + i]);
                    mx[i] = std::max(mx[i], a[3 * k + i]);
                    c[i] += a[3 * k + i];
                }
            const float* n = &N[9 * t];
            const float ns[3] = {n[0] + n[3] + n[6], n[1] + n[4] + n[7], n[2] + n[5] + n[8]};
            windingSum += (cr[0] * ns[0] + cr[1] * ns[1] + cr[2] * ns[2]) > 0.0f ? 1.0 : -1.0;
        }
        if (liveTris == 0) for (int i = 0; i < 3; ++i) { mn[i] = 0.0f; mx[i] = 0.0f; }
        for (int i = 0; i < 3; ++i) {
            fr.Min[i] = mn[i];
            fr.Max[i] = mx[i];
            fr.Centroid[i] = liveTris ? (float)(c[i] / (3.0 * liveTris)) : 0.0f;
            allMin[i] = std::min(allMin[i], mn[i]);
            allMax[i] = std::max(allMax[i], mx[i]);
        }
        fr.LiveTris = liveTris;
        // Quantise. Dead triangles all land on texel value 0: three equal corners, nothing rasterised.
        float scale[3];
        for (int i = 0; i < 3; ++i) scale[i] = mx[i] - mn[i] > 1e-6f ? 65535.0f / (mx[i] - mn[i]) : 0.0f;
        for (std::uint32_t v = 0; v < polygonVertices; ++v) {
            std::uint32_t x, row;
            VatTexel(v, tpr, x, row);
            std::uint16_t* texel = &out.Texels[(((size_t)f * rpf + row) * W + x) * 4];
            if (!live[v / 3]) { texel[0] = texel[1] = texel[2] = 0; texel[3] = PackOctNormal(0, 1, 0); continue; }
            for (int i = 0; i < 3; ++i)
                texel[i] = (std::uint16_t)std::clamp((int)std::lround((P[3 * v + i] - mn[i]) * scale[i]), 0, 65535);
            texel[3] = PackOctNormal(N[3 * v], N[3 * v + 1], N[3 * v + 2]);
        }
    }
    for (int i = 0; i < 3; ++i) {
        h.BoundsMin[i] = allMin[i];
        h.BoundsMax[i] = allMax[i];
        h.Origin[i] = out.Frames[0].Centroid[i];
    }
    if (windingSum < 0.0) h.Flags |= VatFlagClockwise;
    return true;
}

bool ImportPackage(const std::string& packageDir, const std::string& outDir) {
    namespace fs = std::filesystem;
    const fs::path src = fs::u8path(packageDir) / "BloodResources";
    const fs::path dst = fs::u8path(outDir);
    std::error_code ec;
    fs::create_directories(dst / "decals", ec);
    if (!fs::is_directory(src)) {
        Log::Error("BloodFx import: '" + packageDir + "' has no BloodResources folder - pass the VolumetricBloodFX package folder");
        return false;
    }
    struct Sim { const char* Name; const char* Dir; const char* Mesh; const char* Pos; const char* Nrm; const char* Mat; };
    static const Sim kSims[] = {
        {"blood1", "Blood1", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "Blood.mat"},
        {"blood2_left", "Blood2/Left", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood2_right", "Blood2/Right", "blood.fbx", "blood5_pos.exr", "blood.exr", "blood.mat"},
        {"blood2_vertical", "Blood2/Vertical", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "Blood.mat"},
        {"blood3", "blood3", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood4", "blood4", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood5", "blood5", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood6", "blood6", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood7", "blood7", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood8", "blood8", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
        {"blood9", "blood9", "blood_mesh.fbx", "blood_pos.exr", "blood_norm.exr", "blood.mat"},
    };
    bool ok = true;
    size_t totalBytes = 0;
    for (const Sim& s : kSims) {
        const fs::path dir = src / s.Dir;
        std::string err, matText, fbxText;
        std::vector<std::uint8_t> posBytes, nrmBytes;
        ExrImage pos, nrm;
        std::vector<float> uvs;
        size_t pv = 0;
        float bmin = 0, bmax = 0, frames = 0, ho[3] = {0, 0, 0};
        if (!ReadText((dir / s.Mat).u8string(), matText) || !MatFloat(matText, "_boundingMin", bmin) ||
            !MatFloat(matText, "_boundingMax", bmax) || !MatFloat(matText, "_numOfFrames", frames) || !MatColor(matText, "_HeightOffset", ho)) {
            err = "material " + std::string(s.Mat) + " is missing or lacks the VAT properties";
        } else if (!ReadText((dir / s.Mesh).u8string(), fbxText) || !ReadFbxAsciiUVs(fbxText, uvs, pv, &err)) {
            if (err.empty()) err = "can't read " + std::string(s.Mesh);
        } else if (!ReadAll((dir / s.Pos).u8string(), posBytes) || !ReadExr(posBytes, pos, &err)) {
            if (err.empty()) err = "can't read " + std::string(s.Pos);
        } else if (!ReadAll((dir / s.Nrm).u8string(), nrmBytes) || !ReadExr(nrmBytes, nrm, &err)) {
            if (err.empty()) err = "can't read " + std::string(s.Nrm);
        }
        VatData vat;
        if (err.empty() && !BuildVat(pos, nrm, uvs, pv, (int)frames, bmin, bmax, ho, vat, &err) && err.empty()) err = "build failed";
        const fs::path outFile = dst / (std::string(s.Name) + ".bvat");
        if (err.empty() && !WriteVat(outFile.u8string(), vat)) err = "can't write " + outFile.u8string();
        if (!err.empty()) {
            Log::Error(std::string("BloodFx import: ") + s.Name + ": " + err);
            ok = false;
            continue;
        }
        const VatHeader& h = vat.Header;
        const size_t bytes = sizeof(VatHeader) + vat.Frames.size() * sizeof(VatFrame) + vat.Texels.size() * 2;
        totalBytes += bytes;
        std::ostringstream m;
        m.setf(std::ios::fixed);
        m.precision(1);
        m << "BloodFx import: " << s.Name << ": " << h.Frames << " frames, " << h.VertexCount / 3 << " tris, "
          << h.RowsPerFrame << " rows/frame, origin (" << h.Origin[0] << ", " << h.Origin[1] << ", " << h.Origin[2]
          << "), bounds (" << h.BoundsMin[0] << ", " << h.BoundsMin[1] << ", " << h.BoundsMin[2] << ")-(" << h.BoundsMax[0]
          << ", " << h.BoundsMax[1] << ", " << h.BoundsMax[2] << "), last-frame centroid (" << vat.Frames.back().Centroid[0]
          << ", " << vat.Frames.back().Centroid[1] << ", " << vat.Frames.back().Centroid[2] << "), "
          << ((h.Flags & VatFlagClockwise) ? "clockwise" : "counter-clockwise") << ", " << bytes / 1024 << " KB";
        Log::Info(m.str());
    }

    struct DecalSet { const char* Name; const char* Dir; const char* Norm; const char* Mask; };
    static const DecalSet kDecals[] = {
        {"blood1", "Blood1", "Decal_norm.png", "Decal_mask.png"},
        {"blood2_left", "Blood2/Left", "Blood_normal.png", "Blood_Mask.png"},
        {"blood2_right", "Blood2/Right", "decal.png", "decal_mask.png"},
        {"blood2_vertical", "Blood2/Vertical", "Decal_norm.png", "Decal_mask.png"},
        {"char", "Blood2/Vertical", "char_decal_norm.png", "char_decal_mask.png"},
        {"blood3", "blood3", "decal_norm.png", "decal_mask.png"},
        {"blood4", "blood4", "decal_norm.png", "decal_mask.png"},
        {"blood6", "blood6", "decal_norm.png", "decal_mask.png"},
        {"blood7", "blood7", "decal_norm.png", "decal_mask.png"},
        {"blood8", "blood8", "decal_normal.png", "decal_mask.png"},
        {"blood9", "blood9", "decal_norm.png", "decal_mask.png"},
        {"attached", "AttachedBlood", "Decal_norm.png", "Decal_mask.png"},
    };
    for (const DecalSet& d : kDecals) {
        const fs::path dir = src / d.Dir;
        fs::copy_file(dir / d.Norm, dst / "decals" / (std::string(d.Name) + "_norm.png"), fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::copy_file(dir / d.Mask, dst / "decals" / (std::string(d.Name) + "_mask.png"), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            Log::Error(std::string("BloodFx import: decal ") + d.Name + ": " + ec.message());
            ok = false;
        }
    }
    fs::copy_file(src / "AttachedBlood" / "DecalLookup.png", dst / "decals" / "lookup.png", fs::copy_options::overwrite_existing, ec);
    if (ec) { Log::Error("BloodFx import: DecalLookup.png: " + ec.message()); ok = false; }

    std::ofstream readme(dst / "README.md", std::ios::trunc);
    readme << "# Volumetric blood data (generated, git-ignored)\n\n"
              "Converted from the KriptoFX \"Volumetric Blood Fluids\" Unity asset (v1.0.3), which can't be\n"
              "redistributed, so this folder is not in git. Rebuild it with:\n\n"
              "    TartarusEngine.exe --import-blood-fx \"<path to the VolumetricBloodFX package folder>\"\n\n"
              "See docs/BLOOD_FX.md.\n";
    Log::Info("BloodFx import: " + std::string(ok ? "done" : "FAILED") + ", " + std::to_string(totalBytes / (1024 * 1024)) +
              " MB of sims written to " + dst.u8string());
    return ok;
}

} // namespace BloodFxImport
