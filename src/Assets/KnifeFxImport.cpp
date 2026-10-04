#include "KnifeFxImport.h"

#include "Log.h"
#include <stb_dxt.h>
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace KnifeFxImport {
namespace fs = std::filesystem;

const char* LibraryFile(Library lib) {
    switch (lib) {
    case Library::DecalLarge: return "decals_large.kfx";
    case Library::DecalSmall: return "decals_small.kfx";
    case Library::Sprite: return "sprites.kfx";
    default: return "";
    }
}

int LibraryLayerSize(Library lib) { return lib == Library::DecalLarge ? 2048 : 1024; }

int MipCount(int size) {
    int n = 1;
    while (size > 4) { size /= 2; ++n; }
    return n;
}

std::size_t LayerBytes(int size, int mips) {
    std::size_t bytes = 0;
    for (int m = 0; m < mips; ++m) {
        const int s = std::max(4, size >> m);
        bytes += (std::size_t)(s / 4) * (s / 4) * 16;
    }
    return bytes;
}

bool WriteLibrary(const std::string& path, const LibraryData& lib) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    FileHeader h = lib.Header;
    h.EntryCount = (std::uint32_t)lib.Entries.size();
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(lib.Entries.data()), (std::streamsize)(lib.Entries.size() * sizeof(FileEntry)));
    f.write(reinterpret_cast<const char*>(lib.Color.data()), (std::streamsize)lib.Color.size());
    f.write(reinterpret_cast<const char*>(lib.Normal.data()), (std::streamsize)lib.Normal.size());
    return (bool)f;
}

bool ReadLibrary(const std::string& path, LibraryData& lib, std::string* error) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return fail("cannot open");
    f.read(reinterpret_cast<char*>(&lib.Header), sizeof(FileHeader));
    const FileHeader& h = lib.Header;
    if (!f || h.Magic != kMagic) return fail("not a .kfx file");
    if (h.Version != kVersion) return fail("version " + std::to_string(h.Version) + " (expected " + std::to_string(kVersion) + ") - re-run --import-knife-fx");
    if (h.Size < 4 || h.Size > 4096 || h.Mips < 1 || h.Mips > 13 || h.EntryCount > 4096 || h.ColorLayers > 256 || h.NormalLayers > 256)
        return fail("corrupt header");
    lib.Entries.resize(h.EntryCount);
    f.read(reinterpret_cast<char*>(lib.Entries.data()), (std::streamsize)(lib.Entries.size() * sizeof(FileEntry)));
    const std::size_t layer = LayerBytes((int)h.Size, (int)h.Mips);
    lib.Color.resize(layer * h.ColorLayers);
    lib.Normal.resize(layer * h.NormalLayers);
    f.read(reinterpret_cast<char*>(lib.Color.data()), (std::streamsize)lib.Color.size());
    f.read(reinterpret_cast<char*>(lib.Normal.data()), (std::streamsize)lib.Normal.size());
    if (!f) return fail("truncated");
    for (FileEntry& e : lib.Entries) {
        e.Name[sizeof(e.Name) - 1] = 0;
        if (e.ColorLayer < 0 || e.ColorLayer >= (int)h.ColorLayers || e.NormalLayer >= (int)h.NormalLayers || e.Cols < 1 || e.Rows < 1 ||
            e.Frames < 1 || e.Frames > e.Cols * e.Rows)
            return fail(std::string("bad entry '") + e.Name + "'");
    }
    return true;
}

// --- image helpers -------------------------------------------------------------------------------------------------

void Blit(const Image& src, float x0, float y0, float x1, float y1, Image& dst, int dx, int dy, int dw, int dh, bool alphaWeighted) {
    const float sx = (x1 - x0) / (float)dw, sy = (y1 - y0) / (float)dh;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0}, wsum = 0.0f, asum = 0.0f;
            if (sx >= 1.0f && sy >= 1.0f) { // shrinking: the box of source texels under this one
                const float fx0 = x0 + (float)x * sx, fy0 = y0 + (float)y * sy;
                const int ix0 = std::clamp((int)std::floor(fx0), 0, src.W - 1), iy0 = std::clamp((int)std::floor(fy0), 0, src.H - 1);
                const int ix1 = std::clamp((int)std::ceil(fx0 + sx), ix0 + 1, src.W), iy1 = std::clamp((int)std::ceil(fy0 + sy), iy0 + 1, src.H);
                for (int v = iy0; v < iy1; ++v)
                    for (int u = ix0; u < ix1; ++u) {
                        const std::uint8_t* p = &src.Px[((size_t)v * src.W + u) * 4];
                        const float w = alphaWeighted ? (float)p[3] / 255.0f + 1e-4f : 1.0f;
                        for (int c = 0; c < 3; ++c) acc[c] += (float)p[c] * w;
                        wsum += w;
                        asum += (float)p[3];
                        acc[3] += 1.0f;
                    }
                for (int c = 0; c < 3; ++c) acc[c] /= wsum;
                acc[3] = asum / acc[3];
            } else { // enlarging: bilinear
                const float fx = std::clamp(x0 + ((float)x + 0.5f) * sx - 0.5f, 0.0f, (float)(src.W - 1));
                const float fy = std::clamp(y0 + ((float)y + 0.5f) * sy - 0.5f, 0.0f, (float)(src.H - 1));
                const int u0 = (int)fx, v0 = (int)fy, u1 = std::min(u0 + 1, src.W - 1), v1 = std::min(v0 + 1, src.H - 1);
                const float tx = fx - (float)u0, ty = fy - (float)v0;
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int u, int v) { return (float)src.Px[((size_t)v * src.W + u) * 4 + c]; };
                    acc[c] = (at(u0, v0) * (1 - tx) + at(u1, v0) * tx) * (1 - ty) + (at(u0, v1) * (1 - tx) + at(u1, v1) * tx) * ty;
                }
            }
            std::uint8_t* o = &dst.Px[((size_t)(dy + y) * dst.W + dx + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = (std::uint8_t)std::clamp((int)std::lround(acc[c]), 0, 255);
        }
    }
}

Image HalfSize(const Image& src, bool alphaWeighted) {
    Image out;
    out.W = std::max(1, src.W / 2);
    out.H = std::max(1, src.H / 2);
    out.Px.resize((size_t)out.W * out.H * 4);
    Blit(src, 0.0f, 0.0f, (float)src.W, (float)src.H, out, 0, 0, out.W, out.H, alphaWeighted);
    return out;
}

void CompressBC3(const Image& img, std::vector<std::uint8_t>& out) {
    std::uint8_t block[64], dst[16];
    for (int by = 0; by < img.H; by += 4)
        for (int bx = 0; bx < img.W; bx += 4) {
            for (int y = 0; y < 4; ++y) std::memcpy(&block[y * 16], &img.Px[((size_t)(by + y) * img.W + bx) * 4], 16);
            stb_compress_dxt_block(dst, block, 1, STB_DXT_HIGHQUAL);
            out.insert(out.end(), dst, dst + 16);
        }
}

void CompressBC5(const Image& img, std::vector<std::uint8_t>& out) {
    std::uint8_t block[32], dst[16];
    for (int by = 0; by < img.H; by += 4)
        for (int bx = 0; bx < img.W; bx += 4) {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const std::uint8_t* p = &img.Px[((size_t)(by + y) * img.W + bx + x) * 4];
                    block[(y * 4 + x) * 2] = p[0];
                    block[(y * 4 + x) * 2 + 1] = p[1];
                }
            stb_compress_bc5_block(dst, block);
            out.insert(out.end(), dst, dst + 16);
        }
}

namespace {

// --- the catalogue -------------------------------------------------------------------------------------------------
constexpr const char* kRealBlood = "Knife Real Blood/Knife/Real Blood/";
constexpr const char* kProFx = "Knife PRO Effects FPS Muzzle Flashes Impacts/Knife/PRO Effects FPS Muzzle flashes & Impacts/";

enum class Mode { Albedo, MaskR, MaskA, Channel };

struct Source {
    const char* Name;
    Library Lib;
    const char* Pack;   // kRealBlood / kProFx
    const char* Color;  // path in the pack; "dir/|key" = the first file in dir whose name holds key
    Mode ColorMode;
    const char* Normal; // nullptr: none
    int SrcCols, SrcRows;
    int DstCols, DstRows;
    int Stride = 1;     // every Stride-th source cell (flipbooks thinned to fit)
    int Count = 0;      // cells taken; 0 = all
    float Smoothness = 0.5f;
};

// What the game uses of the two packs. Cells are read in reading order (channel-packed smoke: the
// red channel's 16 frames, then green's, ...), so a thinned flipbook keeps its whole timeline.
const Source kSources[] = {
    // Pools (Real Blood "Puddles/Smooth"; "BigPuddle" has baked highlights and holes that don't match the rest).
    {"pool_smooth", Library::DecalLarge, kRealBlood, "Textures/Puddles/Smooth/decal_meash_decal_AlbedoTransparency (3).tga", Mode::Albedo,
     "Textures/Puddles/Smooth/decal_meash_decal_Normal (1) fixed.tga", 2, 2, 2, 2, 1, 0, 0.92f},
    // Splatter (Real Blood "SmallPuddles", "Puddles/medium", "Damage"): more shapes for the thrown blood, drawn in the
    // palette's colour like every stain (docs/BLOOD_FX.md, v3).
    {"splat_small", Library::DecalSmall, kRealBlood, "Textures/SmallPuddles/Puddle_small_size_AlbedoTransparency.tga", Mode::Albedo,
     "Textures/SmallPuddles/Puddle_small_size_Normal fixed.tga", 2, 2, 2, 2, 1, 0, 0.9f},
    {"splat_wide", Library::DecalSmall, kRealBlood, "Textures/SmallPuddles/low1_blood_puddles_AlbedoTransparency 1.png", Mode::Albedo,
     "Textures/SmallPuddles/low1_blood_puddles_Normal 1.png", 2, 2, 2, 2, 1, 0, 0.9f},
    {"splat_medium", Library::DecalSmall, kRealBlood, "Textures/Puddles/medium/Puddle_medium_size_1_AlbedoTransparency.tga", Mode::Albedo,
     "Textures/Puddles/medium/Puddle_medium_size_1_Normal fixed.tga", 2, 2, 2, 2, 1, 0, 0.9f},
    {"drops", Library::DecalSmall, kRealBlood, "Textures/Damage/decal_meash_decal_AlbedoTransparency (2).tga", Mode::Albedo,
     "Textures/Damage/decal_meash_decal_Normal (1).tga", 4, 4, 4, 4, 1, 0, 0.9f},
    // Prints (Real Blood).
    {"footprint", Library::DecalSmall, kRealBlood, "Textures/Decals/FootPrint Albedo.tga", Mode::Albedo, "Textures/Decals/FootPrint Normal.tga",
     4, 2, 4, 2, 1, 0, 0.8f},
    // Wall drips running down: 8 x 4 reveal flipbooks, the shape in the red channel.
    {"leak1", Library::DecalSmall, kRealBlood, "Textures/Leaks/Blood_drop_1-1.png", Mode::MaskR, "Textures/Leaks/Blood_drop_1-1 normal.png", 8, 4, 8, 4,
     1, 0, 0.92f},
    {"leak2", Library::DecalSmall, kRealBlood, "Textures/Leaks/Blood_drop_1-2.png", Mode::MaskR, "Textures/Leaks/Blood_drop_1-2 normal.png", 8, 4, 8, 4,
     1, 0, 0.92f},
    {"leak3", Library::DecalSmall, kRealBlood, "Textures/Leaks/Blood_drop_1-3.png", Mode::MaskR, "Textures/Leaks/Blood_drop_1-3 normal.png", 8, 4, 8, 4,
     1, 0, 0.92f},
    // Bullet holes per surface (PRO Effects "Decals"), 4 variants each.
    {"hole_asphalt", Library::DecalSmall, kProFx, "Decals/Textures/Asphalt/|Albedo", Mode::Albedo, "Decals/Textures/Asphalt/|Normal", 2, 2, 2, 2, 1, 0, 0.2f},
    {"hole_brick", Library::DecalSmall, kProFx, "Decals/Textures/Bricks/|Albedo", Mode::Albedo, "Decals/Textures/Bricks/|Normal", 2, 2, 2, 2, 1, 0, 0.2f},
    {"hole_concrete", Library::DecalSmall, kProFx, "Decals/Textures/Concrete 1/|Albedo", Mode::Albedo, "Decals/Textures/Concrete 1/|Normal", 2, 2, 2, 2,
     1, 0, 0.25f},
    {"hole_glass", Library::DecalSmall, kProFx, "Decals/Textures/Glass/|Albedo", Mode::Albedo, "Decals/Textures/Glass/|Normal", 2, 2, 2, 2, 1, 0, 0.9f},
    {"hole_rock", Library::DecalSmall, kProFx, "Decals/Textures/Rock/|Albedo", Mode::Albedo, "Decals/Textures/Rock/|Normal", 2, 2, 2, 2, 1, 0, 0.25f},
    {"hole_wood", Library::DecalSmall, kProFx, "Decals/Textures/Wood 1/|Albedo", Mode::Albedo, "Decals/Textures/Wood 1/|Normal", 2, 2, 2, 2, 1, 0, 0.3f},
    {"hole_metal", Library::DecalSmall, kProFx, "Decals/Textures/metal 1/|Albedo", Mode::Albedo, "Decals/Textures/metal 1/|Normal", 2, 2, 2, 2, 1, 0, 0.6f},
    {"hole_metal_thin", Library::DecalSmall, kProFx, "Decals/Textures/metal 2/|Albedo", Mode::Albedo, "Decals/Textures/metal 2/|Normal", 2, 2, 2, 2, 1, 0,
     0.6f},
    {"hole_mud", Library::DecalSmall, kProFx, "Decals/Textures/mud 1/|Albedo", Mode::Albedo, "Decals/Textures/mud 1/|Normal", 2, 2, 2, 2, 1, 0, 0.35f},
    {"hole_sand", Library::DecalSmall, kProFx, "Decals/Textures/sand/|Albedo", Mode::Albedo, "Decals/Textures/sand/|Normal", 2, 2, 2, 2, 1, 0, 0.15f},
    {"hole_tile", Library::DecalSmall, kProFx, "Decals/Textures/tile/|Albedo", Mode::Albedo, "Decals/Textures/tile/|Normal", 2, 2, 2, 2, 1, 0, 0.7f},
    // Blood particles (Real Blood "Common"): white shapes, eroded and tinted at runtime.
    {"blood_hit", Library::Sprite, kRealBlood, "Textures/Common/Sheets/Blood_1-2.png", Mode::MaskR, "Textures/Common/Sheets/Blood_1-2_n.png", 4, 4, 4, 4,
     1, 0, 0.95f},
    {"blood_burst", Library::Sprite, kRealBlood, "Textures/Common/Sheets/Blood_Albedo 2.png", Mode::MaskR, "Textures/Common/Sheets/Blood_Albedo 2_n.png",
     8, 8, 4, 4, 4, 16, 0.95f},
    {"blood_jet", Library::Sprite, kRealBlood, "Textures/Common/Sheets/blood_jet_1.png", Mode::MaskR, "Textures/Common/Sheets/blood_jet_1_n.png", 4, 8, 4,
     4, 2, 16, 0.95f},
    {"blood_fan", Library::Sprite, kRealBlood, "Textures/Common/Sheets/Blood_1-5.png", Mode::MaskR, "Textures/Common/Sheets/Blood_1-5_n.png", 4, 4, 4, 4,
     1, 0, 0.95f},
    {"blood_drop", Library::Sprite, kRealBlood, "Textures/Common/Particles/blood particle 2.png", Mode::MaskR,
     "Textures/Common/Particles/blood particle 2 normal.png", 1, 1, 1, 1, 1, 0, 0.95f},
    {"blood_cloud", Library::Sprite, kRealBlood, "Textures/Common/ParticleCloudWhite.png", Mode::MaskA, nullptr, 1, 1, 1, 1, 1, 0, 0.3f},
    // Impact smoke (PRO Effects channel-packed sheets: 4 x 4 frames in each of r, g, b, a), every 4th frame.
    {"smoke_impact", Library::Sprite, kProFx, "Particles/Textures/Sheets/Smoke chanel sheet 5-2.tga", Mode::Channel, nullptr, 4, 4, 4, 4, 4, 16, 0.1f},
    {"smoke_muzzle", Library::Sprite, kProFx, "Particles/Textures/Sheets/Smoke chanel sheet 1.tga", Mode::Channel, nullptr, 4, 4, 4, 4, 4, 16, 0.1f},
    {"smoke_gun", Library::Sprite, kProFx, "Particles/Textures/Sheets/Smoke chanel sheet 4.tga", Mode::Channel, nullptr, 4, 4, 4, 4, 4, 16, 0.1f},
    // Debris (PRO Effects impact pieces), 4 variants each.
    {"debris_rock", Library::Sprite, kProFx, "Particles/Textures/Rocks/Rocks 1/Plane 1-1_Decals_AlbedoTransparency.tga", Mode::Albedo,
     "Particles/Textures/Rocks/Rocks 1/Plane 1-1_Decals_Normal.tga", 2, 2, 2, 2, 1, 0, 0.25f},
    {"debris_concrete", Library::Sprite, kProFx, "Particles/Textures/Rocks/Rocks 2/Plane_1-1_Material_27_AlbedoTransparency.tga", Mode::Albedo,
     "Particles/Textures/Rocks/Rocks 2/Plane_1-1_Material_27_Normal.tga", 2, 2, 2, 2, 1, 0, 0.25f},
    {"debris_wood", Library::Sprite, kProFx, "Particles/Textures/Wood/Plane 1-1_Decals_AlbedoTransparency.tga", Mode::Albedo,
     "Particles/Textures/Wood/Plane 1-1_Decals_Normal.tga", 2, 2, 2, 2, 1, 0, 0.3f},
    {"debris_glass", Library::Sprite, kProFx, "Particles/Textures/Glass/GlassPieces_Albedo.tga", Mode::MaskA, nullptr, 2, 2, 2, 2, 1, 0, 0.95f},
    // Muzzle flashes (PRO Effects "Shoot FX"): white shapes in red, coloured at runtime.
    {"muzzle_star", Library::Sprite, kProFx, "Shoot FX/Textures/muzzleflash_1_blurred.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
    {"muzzle_star_sharp", Library::Sprite, kProFx, "Shoot FX/Textures/muzzleflash_1.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
    {"muzzle_burst", Library::Sprite, kProFx, "Shoot FX/Textures/8.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
    {"muzzle_side", Library::Sprite, kProFx, "Shoot FX/Textures/10.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
    {"muzzle_front", Library::Sprite, kProFx, "Shoot FX/Textures/7.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
    {"glow", Library::Sprite, kProFx, "Shoot FX/Textures/blurred circle.png", Mode::MaskR, nullptr, 1, 1, 1, 1, 1, 0, 0.0f},
};

std::string Resolve(const std::string& root, const char* rel) {
    const std::string r(rel);
    const size_t bar = r.find('|');
    if (bar == std::string::npos) return root + r;
    const std::string dir = root + r.substr(0, bar), key = r.substr(bar + 1);
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec)) {
        const std::string n = e.path().filename().u8string();
        if (n.find(key) != std::string::npos && e.path().extension() != ".meta") names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    return names.empty() ? dir + "<no " + key + ">" : dir + names.front();
}

bool LoadImage(const std::string& path, Image& img) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return false;
    img.W = w;
    img.H = h;
    img.Px.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

// Rewrites a source image's texels into what the library stores for its mode.
void ToMode(Image& img, Mode mode, int channel) {
    for (size_t i = 0; i < img.Px.size(); i += 4) {
        std::uint8_t* p = &img.Px[i];
        switch (mode) {
        case Mode::Albedo: break;
        case Mode::MaskR: p[3] = p[0]; p[0] = p[1] = p[2] = 255; break;
        case Mode::MaskA: p[0] = p[1] = p[2] = 255; break;
        case Mode::Channel: p[3] = p[channel]; p[0] = p[1] = p[2] = 255; break;
        }
    }
}

struct Built {
    FileEntry Entry;
    std::vector<std::uint8_t> Color, Normal; // one layer's mip chain each (Normal empty: none)
    std::string Error;
};

// Packs one source into a layer: its picked cells into the destination grid, then mips and compression.
void BuildSource(const std::string& assetsDir, const Source& s, Built& out) {
    const int size = LibraryLayerSize(s.Lib), mips = MipCount(size);
    const std::string root = assetsDir + "/" + s.Pack;
    const std::string colorPath = Resolve(root, s.Color);
    Image color, normal;
    if (!LoadImage(colorPath, color)) { out.Error = "cannot load " + colorPath; return; }
    const bool hasNormal = s.Normal != nullptr;
    if (hasNormal) {
        const std::string normalPath = Resolve(root, s.Normal);
        if (!LoadImage(normalPath, normal)) { out.Error = "cannot load " + normalPath; return; }
    }
    const int perChannel = s.SrcCols * s.SrcRows;
    const int available = s.ColorMode == Mode::Channel ? perChannel * 4 : perChannel;
    const int count = s.Count > 0 ? s.Count : (available + s.Stride - 1) / s.Stride;
    if (count > s.DstCols * s.DstRows || (count - 1) * s.Stride >= available) { out.Error = "grid doesn't fit"; return; }

    Image layer, nlayer;
    layer.W = layer.H = nlayer.W = nlayer.H = size;
    layer.Px.assign((size_t)size * size * 4, 0);
    if (s.ColorMode == Mode::Albedo) // transparent texels take the average colour (no dark fringes in the mips)
        for (size_t i = 0; i < layer.Px.size(); i += 4) { layer.Px[i] = 90; layer.Px[i + 1] = 10; layer.Px[i + 2] = 8; }
    else
        for (size_t i = 0; i < layer.Px.size(); i += 4) layer.Px[i] = layer.Px[i + 1] = layer.Px[i + 2] = 255;
    nlayer.Px.assign((size_t)size * size * 4, 0);
    for (size_t i = 0; i < nlayer.Px.size(); i += 4) { nlayer.Px[i] = nlayer.Px[i + 1] = 128; nlayer.Px[i + 2] = 255; nlayer.Px[i + 3] = 255; }

    const float cw = (float)color.W / (float)s.SrcCols, ch = (float)color.H / (float)s.SrcRows;
    const int dw = size / s.DstCols, dh = size / s.DstRows;
    int frames = 0;
    Image channelImg[4];
    for (int k = 0; k < count; ++k) {
        const int idx = k * s.Stride;
        const int channel = s.ColorMode == Mode::Channel ? idx / perChannel : 0;
        const int cell = idx % perChannel;
        const float x0 = (float)(cell % s.SrcCols) * cw, y0 = (float)(cell / s.SrcCols) * ch;
        const Image* src = &color;
        Image converted;
        if (s.ColorMode == Mode::Channel) {
            if (channelImg[channel].Px.empty()) { channelImg[channel] = color; ToMode(channelImg[channel], Mode::Channel, channel); }
            src = &channelImg[channel];
        } else if (k == 0) {
            ToMode(color, s.ColorMode, 0);
        }
        const int dx = (k % s.DstCols) * dw, dy = (k / s.DstCols) * dh;
        Blit(*src, x0, y0, x0 + cw, y0 + ch, layer, dx, dy, dw, dh, s.ColorMode == Mode::Albedo);
        if (hasNormal) {
            const float nw = (float)normal.W / (float)s.SrcCols, nh = (float)normal.H / (float)s.SrcRows;
            const float nx0 = (float)(cell % s.SrcCols) * nw, ny0 = (float)(cell / s.SrcCols) * nh;
            Blit(normal, nx0, ny0, nx0 + nw, ny0 + nh, nlayer, dx, dy, dw, dh, false);
        }
        // A cell counts while it holds anything (some sheets end early).
        std::uint8_t peak = 0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) peak = std::max(peak, layer.Px[((size_t)(dy + y) * size + dx + x) * 4 + 3]);
        if (peak > 8) frames = k + 1;
    }
    if (frames == 0) { out.Error = "every cell is empty"; return; }

    Image mip = layer, nmip = nlayer;
    for (int m = 0; m < mips; ++m) {
        if (m > 0) { mip = HalfSize(mip, s.ColorMode == Mode::Albedo); nmip = HalfSize(nmip, false); }
        CompressBC3(mip, out.Color);
        if (hasNormal) CompressBC5(nmip, out.Normal);
    }
    FileEntry& e = out.Entry;
    std::memcpy(e.Name, s.Name, std::min(std::strlen(s.Name), sizeof(e.Name) - 1));
    e.Cols = s.DstCols;
    e.Rows = s.DstRows;
    e.Frames = frames;
    e.Flags = s.ColorMode == Mode::Albedo ? EntryAlbedo : s.ColorMode == Mode::Channel ? (EntryMask | EntryChannel) : EntryMask;
    e.Smoothness = s.Smoothness;
    e.Aspect = cw / ch; // the authored cell's shape (the layer's cells may be stretched to fit its grid)
}

} // namespace

bool ImportPacks(const std::string& assetsDir, const std::string& outDir) {
    for (const char* pack : {kRealBlood, kProFx})
        if (!fs::is_directory(fs::u8path(assetsDir + "/" + pack))) {
            Log::Error("KnifeFx import: '" + assetsDir + "' has no " + pack + " - pass the folder holding both extracted packs");
            return false;
        }
    std::error_code ec;
    fs::create_directories(fs::u8path(outDir), ec);

    constexpr size_t n = sizeof(kSources) / sizeof(kSources[0]);
    std::vector<Built> built(n);
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            for (size_t i = next++; i < n; i = next++) BuildSource(assetsDir, kSources[i], built[i]);
        });
    for (auto& w : workers) w.join();

    bool ok = true;
    size_t total = 0;
    for (int l = 0; l < (int)Library::Count; ++l) {
        const Library lib = (Library)l;
        LibraryData data;
        data.Header.Size = (std::uint32_t)LibraryLayerSize(lib);
        data.Header.Mips = (std::uint32_t)MipCount(LibraryLayerSize(lib));
        std::string names;
        for (size_t i = 0; i < n; ++i) {
            if (kSources[i].Lib != lib) continue;
            Built& b = built[i];
            if (!b.Error.empty()) {
                Log::Error(std::string("KnifeFx import: ") + kSources[i].Name + ": " + b.Error);
                ok = false;
                continue;
            }
            b.Entry.ColorLayer = (std::int32_t)data.Header.ColorLayers++;
            data.Color.insert(data.Color.end(), b.Color.begin(), b.Color.end());
            if (!b.Normal.empty()) {
                b.Entry.NormalLayer = (std::int32_t)data.Header.NormalLayers++;
                data.Normal.insert(data.Normal.end(), b.Normal.begin(), b.Normal.end());
            }
            data.Entries.push_back(b.Entry);
            names += std::string(names.empty() ? "" : ", ") + b.Entry.Name + (b.Entry.Frames > 1 ? "(" + std::to_string(b.Entry.Frames) + ")" : "");
        }
        const std::string path = (fs::u8path(outDir) / LibraryFile(lib)).u8string();
        if (!WriteLibrary(path, data)) {
            Log::Error("KnifeFx import: cannot write " + path);
            ok = false;
            continue;
        }
        const size_t bytes = data.Color.size() + data.Normal.size();
        total += bytes;
        Log::Info(std::string("KnifeFx import: ") + LibraryFile(lib) + " - " + std::to_string(data.Header.ColorLayers) + " layers + " +
                  std::to_string(data.Header.NormalLayers) + " normal maps at " + std::to_string(data.Header.Size) + ", " +
                  std::to_string(bytes / (1024 * 1024)) + " MB: " + names);
    }
    Log::Info("KnifeFx import: " + std::string(ok ? "done" : "FAILED") + ", " + std::to_string(total / (1024 * 1024)) + " MB in " + outDir);
    return ok;
}

} // namespace KnifeFxImport
