#pragma once
#include <string>
#include <vector>
#include <functional>
#include <filesystem>

// Converts a single high-bit-depth / multi-channel TIFF (e.g. a packed PBR sheet or a giant
// terrain heightmap) into one or more engine-ready 8-bit PNGs.
//
// Decoding goes through libtiff's TIFFReadRGBAImageOriented rather than hand-rolling
// per-bit-depth/per-sample-format scanline decoding: it already correctly handles every source
// layout libtiff supports (8/16/32-bit integer or float samples, tiled or stripped, palette,
// YCbCr, CMYK...) and hands back a plain top-to-bottom 8-bit RGBA buffer. Since the engine's own
// Texture loader (stb_image) only ever reads 8-bit-per-channel PNGs anyway, decoding further
// than that would produce precision this tool's own output format can't carry — see the
// "why 8-bit" note in TifSplitter.cpp for the full reasoning.
struct TifSplitterOptions {
    std::string InputFilePath;
    std::string OutputDirectory = "./Output";

    // Writes Input_R.png / Input_G.png / Input_B.png / Input_A.png in addition to (or instead
    // of, if TileSize is also 0) the combined image — e.g. Metallic/Roughness/AO/Smoothness
    // packed into one RGBA sheet. A source TIFF that only ever had 1 real channel (e.g. a
    // grayscale heightmap) will still produce 4 files this way, since TIFFReadRGBAImageOriented
    // always normalizes to RGBA — G/B/A will just mirror R in that case.
    bool ExtractIndividualChannels = false;

    // Inverts the decoded Green channel — converts a normal map authored in DirectX's tangent
    // space (+Y up) to OpenGL's (+Y down), which is what this engine's shaders expect.
    bool FlipNormalY = false;

    // Per exported channel, stretches its actual min..max value range to fill the full 0..255
    // range. A 16-bit heightmap that only used a narrow slice of its range (common — real-world
    // elevation data rarely spans the full 0..65535) would otherwise decode to a nearly flat,
    // hard-to-see gray; this restores visible contrast. Only applied to channel-split output
    // (ExtractIndividualChannels) — a combined RGBA image is left as decoded, since
    // independently stretching R/G/B/A of a color image would shift its actual colors.
    bool NormalizeHeightmaps = true;

    // 0 = write the whole image as one PNG. Above 0, splits it into a grid of square PNGs of
    // this size (the edge tiles are shorter/narrower where the source doesn't divide evenly)
    // named Input_X{col}_Y{row}.png — for terrain heightmaps too large to load as one texture.
    int TileSize = 0;
};

// Input/output/threading configuration for a whole batch run — one or more TIFFs converted in
// one call, potentially across many worker threads at once.
struct BatchTifOptions {
    // Explicit file list — e.g. paths chosen via the editor's own multi-select file dialog, or
    // repeated -f/--files arguments on the CLI. Combined with a directory scan below, if both
    // are given; duplicates (the same file reachable both ways) are only processed once.
    std::vector<std::filesystem::path> InputFiles;

    // If set, every .tif/.tiff directly inside this folder is added to the batch too.
    std::filesystem::path InputDirectory;
    // Also descend into InputDirectory's subfolders. Ignored if InputDirectory is empty.
    bool Recursive = false;

    std::filesystem::path OutputDirectory = "./Output";

    // Same meaning as the single-file options above, applied to every file in the batch.
    bool ExtractIndividualChannels = true;
    bool FlipNormalY = true;
    bool NormalizeHeightmaps = true;
    int TileSize = 0;

    // Caps how many files convert concurrently. 0 = std::thread::hardware_concurrency() (or 4
    // if the platform can't report it). Each file's own internal work (channel/tile export)
    // runs sequentially within its worker rather than spawning further threads of its own —
    // see ProcessFile's comment for why nesting parallelism here would backfire.
    int MaxThreads = 0;
};

// Snapshot handed to BatchProgressCallback after each file finishes (success or failure).
struct BatchProgress {
    size_t TotalFiles = 0;
    size_t CompletedFiles = 0; // successes + failures so far — i.e. "finished", not "succeeded"
    size_t FailedFiles = 0;
    std::string CurrentFileName; // the file whose completion just triggered this callback
};

// IMPORTANT: invoked from whichever worker thread just finished that file — never guaranteed to
// be the thread that called ProcessBatch. A callback that touches UI state (an ImGui progress
// bar, for instance) must marshal the update back to the UI thread itself rather than act on it
// directly here.
using BatchProgressCallback = std::function<void(const BatchProgress&)>;

class TifConverter {
public:
    // Converts exactly one file per `options`. Returns false on any failure (missing/corrupt
    // input, unwritable output directory, a libtiff decode error) after logging the reason —
    // never throws, never crashes on a bad or unsupported file. Uses its own internal worker
    // threads for channel/tile export (fine in isolation — there's no other concurrent work to
    // compete with).
    static bool ProcessTif(const TifSplitterOptions& options);

    // Converts one file AS PART OF a batch: resolves its own output location (mirroring
    // InputDirectory's subfolder structure under OutputDirectory when `file` came from a
    // directory scan — e.g. Raw/Rocks/Height.tif -> Output/Rocks/Height_R.png) and applies the
    // batch's shared conversion flags. Deliberately does its channel/tile export sequentially,
    // not in parallel — ProcessBatch already keeps every worker thread busy across different
    // files, so a second layer of per-file parallelism would only oversubscribe the machine
    // rather than finish any faster. Safe to call directly (outside ProcessBatch) for a single
    // file that still needs output-mirroring math, but ProcessTif is simpler for that case.
    static bool ProcessFile(const std::filesystem::path& file, const BatchTifOptions& options);

    // Collects the input set (InputFiles plus a directory scan, if InputDirectory is set),
    // then converts every file using up to MaxThreads (or hardware_concurrency()) concurrent
    // workers — one file per worker at a time, pulled from a shared queue, so a batch of
    // unevenly-sized files still keeps every worker busy until the whole set is done rather
    // than idling a fast worker while a slow one grinds through a big file. Prints a
    // [Info]/[Success]/[Error] line per stage (thread-safe — concurrent workers' output never
    // interleaves mid-line) and a final summary (counts, elapsed time, and every failure with
    // its reason) when done.
    static void ProcessBatch(const BatchTifOptions& options, BatchProgressCallback progressCB = nullptr);
};
