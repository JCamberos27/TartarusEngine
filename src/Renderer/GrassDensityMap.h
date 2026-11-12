#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A Grass Area's density mask (GrassAreaComponent::DensityMap): ResX x ResZ 8-bit samples spread evenly over the
// area's rectangle, sample (0, 0) at its local (-SizeX/2, -SizeZ/2) corner, row-major along X, base64 in the scene.
// 255 = the full density, 0 = no blades: a tool paints the canopy's shade and cuts out the logs, rocks and trunks
// (tools/scenes/build_forest.py). The GPU scatter (Grass.comp) samples it bilinearly, the same as Sample().
struct GrassDensityMap {
    int ResX = 0, ResZ = 0;
    std::vector<std::uint8_t> Values; // ResX * ResZ
private:
    mutable std::vector<std::uint8_t> m_Bare;
    mutable float m_BareFor[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    mutable int m_BareCols = 0;
public:

    bool Valid() const { return ResX >= 2 && ResZ >= 2 && (int)Values.size() == ResX * ResZ; }
    float At(int i, int j) const { return Values[(size_t)j * (size_t)ResX + (size_t)i] * (1.0f / 255.0f); }
    // Density at (u, v) in [0, 1] across the area (clamped), bilinear.
    float Sample(float u, float v) const;
    // The highest density over a [u0, u1] x [v0, v1] rectangle: every sample whose cell touches it.
    float MaxIn(float u0, float v0, float u1, float v1) const;

    // Per chunk of an area's grid (cpc cells of `cell` metres a side over sx x sz), 1 where the mask leaves it bare:
    // built once for those numbers and kept, so chunk selection doesn't scan the mask every view.
    const std::vector<std::uint8_t>& BareChunks(float sx, float sz, float cell, int cpc, int& outCols) const;

    static bool Decode(int resX, int resZ, const std::string& base64, GrassDensityMap& out);
    static std::string Encode(const std::vector<float>& values); // 0..1 per sample
};

namespace Base64 {
bool Decode(const std::string& in, std::vector<std::uint8_t>& out);
std::string Encode(const std::vector<std::uint8_t>& in);
}
