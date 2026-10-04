#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Knife Entertainment packs import. Converts the chosen textures of the
// "Real Blood" and "PRO Effects FPS Muzzle Flashes & Impacts" Unity packs (extracted to plain files
// under one folder) into the engine's runtime texture libraries under project/assets/Effects/Knife/.
// The output is git-ignored (the packs can't be redistributed);
// `TartarusEngine --import-knife-fx <folder holding both packs>` rebuilds it.
//
// Each library is one texture array: every layer a square image holding a grid of cells (variants
// of a decal, or the frames of a flipbook), pre-compressed with full mips - colour as BC3 (RGBA),
// normals as BC5 (the normal's x, y; z is rebuilt). The catalogue of what goes where lives in
// KnifeFxImport.cpp; the game asks for entries by name (KnifeFxLibrary).
namespace KnifeFxImport {

enum class Library : std::uint32_t {
    DecalLarge = 0, // 2048 layers: pools and trails, seen big and close
    DecalSmall,     // 1024: splatter, prints, drips, wounds, bullet holes
    Sprite,         // 1024: flipbook particles - blood mist and bursts, smoke, debris, muzzle flashes
    Count
};
const char* LibraryFile(Library lib); // "decals_large.kfx", ...
int LibraryLayerSize(Library lib);

constexpr std::uint32_t kMagic = 0x3158464Bu; // "KFX1"
constexpr std::uint32_t kVersion = 1;

// Entry flags.
enum EntryFlags : std::uint32_t {
    EntryMask = 1u,     // colour is a white shape in alpha (tinted at runtime: blood, flames)
    EntryAlbedo = 2u,   // colour is a real albedo with coverage in alpha (puddles, holes, debris)
    EntryChannel = 4u,  // came from a channel-packed smoke sheet (white, density in alpha)
};

struct FileHeader {
    std::uint32_t Magic = kMagic;
    std::uint32_t Version = kVersion;
    std::uint32_t Size = 0;         // layer width = height
    std::uint32_t Mips = 0;
    std::uint32_t ColorLayers = 0;
    std::uint32_t NormalLayers = 0;
    std::uint32_t EntryCount = 0;
    std::uint32_t Reserved = 0;
};
static_assert(sizeof(FileHeader) == 32, "FileHeader is a file format");

struct FileEntry {
    char Name[32] = {};
    std::int32_t ColorLayer = -1;
    std::int32_t NormalLayer = -1;  // -1: no normal map
    std::int32_t Cols = 1, Rows = 1;
    std::int32_t Frames = 1;        // cells in use, reading order from the top left
    std::uint32_t Flags = 0;
    float Smoothness = 0.5f;        // the surface's gloss (constant per entry)
    float Aspect = 1.0f;            // a cell's width / height as authored
};
static_assert(sizeof(FileEntry) == 64, "FileEntry is a file format");

struct LibraryData {
    FileHeader Header;
    std::vector<FileEntry> Entries;
    std::vector<std::uint8_t> Color;  // ColorLayers x (all mips, largest first), BC3
    std::vector<std::uint8_t> Normal; // NormalLayers x (all mips), BC5
};
// Bytes of one layer's mip chain (BC3 and BC5 are both 16 bytes per 4x4 block).
std::size_t LayerBytes(int size, int mips);
int MipCount(int size); // down to 4 x 4
bool WriteLibrary(const std::string& path, const LibraryData& lib);
bool ReadLibrary(const std::string& path, LibraryData& lib, std::string* error = nullptr);

// --- image helpers (exposed for the unit tests) ---------------------------------------------------
struct Image {
    int W = 0, H = 0;
    std::vector<std::uint8_t> Px; // RGBA8
};
// Area-averages (or bilinearly enlarges) the source rect [x0, x1) x [y0, y1) into `dst` at (dx, dy) of
// size dw x dh. `alphaWeighted` averages colour by coverage so transparent texels don't bleed in.
void Blit(const Image& src, float x0, float y0, float x1, float y1, Image& dst, int dx, int dy, int dw, int dh, bool alphaWeighted);
Image HalfSize(const Image& src, bool alphaWeighted);
// 4x4-block compression of a whole image (width and height multiples of 4) appended to `out`.
void CompressBC3(const Image& img, std::vector<std::uint8_t>& out);
void CompressBC5(const Image& img, std::vector<std::uint8_t>& out); // from the image's r, g

// Both packs into `outDir`. `assetsDir` holds "Knife Real Blood" and "Knife PRO Effects FPS Muzzle
// Flashes Impacts" as extracted. Logs a line per library; false on any failure.
bool ImportPacks(const std::string& assetsDir, const std::string& outDir);

} // namespace KnifeFxImport
