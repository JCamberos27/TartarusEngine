#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Volumetric blood import (docs/BLOOD_FX.md). Converts the KriptoFX "Volumetric Blood Fluids" source
// package (a Unity asset: Houdini vertex-animation-texture fluid sims as EXR + ASCII FBX, and decal
// PNGs) into the engine's runtime files under project/assets/Effects/Blood/. The output is git-ignored
// (the package can't be redistributed); `TartarusEngine --import-blood-fx <package dir>` rebuilds it.
namespace BloodFxImport {

// --- Minimal OpenEXR reader (scanline, HALF or FLOAT channels, NONE / ZIPS / ZIP compression) ------
struct ExrImage {
    int Width = 0, Height = 0;
    std::vector<std::string> Channels; // as stored: alphabetical (B, G, R)
    std::vector<float> Pixels;         // Height rows, top first; per row, per channel, Width floats
    float At(int x, int y, int channel) const { return Pixels[((size_t)y * Channels.size() + channel) * Width + x]; }
    int ChannelIndex(const std::string& name) const;
};
bool ReadExr(const std::vector<std::uint8_t>& file, ExrImage& out, std::string* error = nullptr);
float HalfToFloat(std::uint16_t h);
std::uint16_t FloatToHalf(float f);

// --- Runtime VAT file (.bvat) ---------------------------------------------------------------------
// One Houdini fluid sim. Vertices are a triangle soup whose topology changes every frame, so frames
// are never blended. Vertex v of the soup reads texel (3 * (t % TrisPerRow) + 2 - v % 3, row t / TrisPerRow)
// of its frame's row block, t = v / 3: the shader derives it from gl_VertexID, so the mesh needs no
// vertex buffer at all. Texels are RGBA16UI: xyz = position quantised into that frame's bounds, w =
// octahedral normal (8 + 8 bits). Positions are the Unity mesh's local space mirrored to this engine's
// right-handed axes (z negated), in the sim's own units - a preset's transform scales them to metres.
constexpr std::uint32_t kVatMagic = 0x54415642u; // "BVAT"
constexpr std::uint32_t kVatVersion = 1;
enum VatFlags : std::uint32_t { VatFlagClockwise = 1u }; // corner order 0,1,2 winds clockwise seen from outside

struct VatHeader {
    std::uint32_t Magic = kVatMagic;
    std::uint32_t Version = kVatVersion;
    std::uint32_t Frames = 0;
    std::uint32_t VertexCount = 0;  // per frame (triangles * 3)
    std::uint32_t TexWidth = 0;
    std::uint32_t RowsPerFrame = 0;
    std::uint32_t TrisPerRow = 0;
    std::uint32_t Flags = 0;
    float BoundsMin[3] = {0, 0, 0}; // every frame's live triangles
    float BoundsMax[3] = {0, 0, 0};
    float Origin[3] = {0, 0, 0};    // frame 0's centroid: where the blood leaves the wound
    std::uint32_t Reserved[4] = {0, 0, 0, 0};
};
static_assert(sizeof(VatHeader) == 84, "VatHeader is a file format");

struct VatFrame {
    float Min[3], Max[3];  // quantisation box (live triangles only; a dead one collapses to a point)
    float Centroid[3];
    std::uint32_t LiveTris;
};
static_assert(sizeof(VatFrame) == 40, "VatFrame is a file format");

struct VatData {
    VatHeader Header;
    std::vector<VatFrame> Frames;
    std::vector<std::uint16_t> Texels; // Frames * RowsPerFrame * TexWidth * 4
};
bool WriteVat(const std::string& path, const VatData& vat);
bool ReadVat(const std::string& path, VatData& vat, std::string* error = nullptr);

// Octahedral unit-vector encoding, 8 bits per axis packed as (x << 8) | y.
std::uint16_t PackOctNormal(float x, float y, float z);
void UnpackOctNormal(std::uint16_t packed, float& x, float& y, float& z);

// The texel column / frame-relative row the soup's vertex `v` reads (see above).
inline void VatTexel(std::uint32_t v, std::uint32_t trisPerRow, std::uint32_t& x, std::uint32_t& row) {
    const std::uint32_t t = v / 3u;
    x = 3u * (t % trisPerRow) + 2u - v % 3u;
    row = t / trisPerRow;
}

// Unity's LinearToGammaSpace, which the sims' textures were authored to go through.
float LinearToGamma(float v);

// --- ASCII FBX: the soup's UVs, one per polygon vertex --------------------------------------------
bool ReadFbxAsciiUVs(const std::string& text, std::vector<float>& uvs, std::size_t& polygonVertices, std::string* error);

// Builds one sim. `boundsMin/Max` and `heightOffset` are the Unity material's _boundingMin/Max and
// _HeightOffset; `frames` its _numOfFrames.
bool BuildVat(const ExrImage& pos, const ExrImage& nrm, const std::vector<float>& uvs, std::size_t polygonVertices,
              int frames, float boundsMin, float boundsMax, const float heightOffset[3], VatData& out, std::string* error);

// The whole package: every sim and decal set into `outDir`. Logs a line per item; false on any failure.
bool ImportPackage(const std::string& packageDir, const std::string& outDir);

} // namespace BloodFxImport
