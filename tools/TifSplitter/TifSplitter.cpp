#include "TifSplitter.h"

#include <tiffio.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

// Every console write in this file goes through these, instead of raw std::cout/std::cerr:
// batch mode runs several files at once on separate threads, and without a shared lock two
// workers' lines can interleave mid-write (this was already a latent bug even before batch
// mode existed - ExtractChannels/TileAndWrite have logged from std::async workers since they
// were written, just never with more than one file in flight to expose it).
std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

void LogInfo(const std::string& msg) {
    std::lock_guard<std::mutex> lock(LogMutex());
    std::cout << "[Info] " << msg << "\n";
}
void LogSuccess(const std::string& msg) {
    std::lock_guard<std::mutex> lock(LogMutex());
    std::cout << "[Success] " << msg << "\n";
}
void LogError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(LogMutex());
    std::cerr << "[Error] " << msg << "\n";
}
void LogWarn(const std::string& msg) {
    std::lock_guard<std::mutex> lock(LogMutex());
    std::cerr << "[Warn] " << msg << "\n";
}

// Interleaved, row-major, top-to-bottom pixel data. Channels is 1 (grayscale) or 4 (RGBA) -
// the only two shapes this tool ever produces.
struct PixelBuffer {
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    std::vector<uint8_t> Data;
};

bool WritePng(const std::string& path, const PixelBuffer& buf) {
    int strideBytes = buf.Width * buf.Channels;
    return stbi_write_png(path.c_str(), buf.Width, buf.Height, buf.Channels, buf.Data.data(), strideBytes) != 0;
}

// In-place min..max -> 0..255 stretch. A flat (single-value) channel is left untouched rather
// than divide by zero.
void StretchContrast(std::vector<uint8_t>& channel) {
    if (channel.empty()) return;
    uint8_t lo = 255, hi = 0;
    for (uint8_t v : channel) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi <= lo) return;
    float scale = 255.0f / (float)(hi - lo);
    for (uint8_t& v : channel) v = (uint8_t)std::lround((v - lo) * scale);
}

unsigned int WorkerCount() {
    unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 4u : n;
}

// Runs `work` for every index in [0, count). `workers` <= 1 runs strictly in-line (no thread
// spawned at all) - this is what a file processed as part of a batch uses, since the batch
// dispatcher already has every hardware thread busy across different files, and stacking a
// second pool of per-file async tasks on top would only oversubscribe the machine. A standalone
// single-file run instead passes WorkerCount(), batched that many std::async calls at a time -
// a plain unbounded std::async per tile would be fine for a handful of tiles, but a small
// TileSize on a large heightmap can produce thousands of them, and firing that many OS threads
// at once would do more harm than the concurrency is worth.
bool RunBounded(size_t count, size_t workers, const std::function<bool(size_t)>& work) {
    if (workers <= 1) {
        bool allOk = true;
        for (size_t i = 0; i < count; ++i) allOk = work(i) && allOk;
        return allOk;
    }

    bool allOk = true;
    for (size_t start = 0; start < count; start += workers) {
        size_t end = std::min(count, start + workers);
        std::vector<std::future<bool>> futures;
        futures.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
            futures.push_back(std::async(std::launch::async, work, i));
        }
        for (auto& f : futures) allOk = f.get() && allOk;
    }
    return allOk;
}

bool ExtractChannels(const PixelBuffer& rgba, const TifSplitterOptions& options,
    const std::string& outDir, const std::string& stem, bool parallel) {
    static const char* kNames[4] = {"R", "G", "B", "A"};
    size_t pixelCount = (size_t)rgba.Width * rgba.Height;

    return RunBounded(4, parallel ? WorkerCount() : 1, [&](size_t c) {
        PixelBuffer channel;
        channel.Width = rgba.Width;
        channel.Height = rgba.Height;
        channel.Channels = 1;
        channel.Data.resize(pixelCount);
        for (size_t p = 0; p < pixelCount; ++p) channel.Data[p] = rgba.Data[p * 4 + c];

        if (options.NormalizeHeightmaps) StretchContrast(channel.Data);

        std::string outPath = outDir + "/" + stem + "_" + kNames[c] + ".png";
        bool wrote = WritePng(outPath, channel);
        if (wrote) LogSuccess("Wrote channel " + std::string(kNames[c]) + " -> " + outPath);
        else LogError("Failed to write '" + outPath + "'.");
        return wrote;
    });
}

bool TileAndWrite(const PixelBuffer& rgba, const TifSplitterOptions& options,
    const std::string& outDir, const std::string& stem, bool parallel) {
    int tileSize = options.TileSize;
    int tilesX = (rgba.Width + tileSize - 1) / tileSize;
    int tilesY = (rgba.Height + tileSize - 1) / tileSize;
    LogInfo("Tiling into " + std::to_string(tilesX) + "x" + std::to_string(tilesY) + " tiles of "
        + std::to_string(tileSize) + "x" + std::to_string(tileSize) + "...");

    return RunBounded((size_t)tilesX * tilesY, parallel ? WorkerCount() : 1, [&](size_t index) {
        int tx = (int)(index % tilesX);
        int ty = (int)(index / tilesX);
        int w = std::min(tileSize, rgba.Width - tx * tileSize);
        int h = std::min(tileSize, rgba.Height - ty * tileSize);

        PixelBuffer tile;
        tile.Width = w;
        tile.Height = h;
        tile.Channels = rgba.Channels;
        tile.Data.resize((size_t)w * h * rgba.Channels);
        for (int y = 0; y < h; ++y) {
            const uint8_t* srcRow = &rgba.Data[((size_t)(ty * tileSize + y) * rgba.Width + (size_t)tx * tileSize) * rgba.Channels];
            uint8_t* dstRow = &tile.Data[(size_t)y * w * rgba.Channels];
            std::memcpy(dstRow, srcRow, (size_t)w * rgba.Channels);
        }

        std::string outPath = outDir + "/" + stem + "_X" + std::to_string(tx) + "_Y" + std::to_string(ty) + ".png";
        bool wrote = WritePng(outPath, tile);
        if (wrote) LogSuccess("Wrote tile -> " + outPath);
        else LogError("Failed to write '" + outPath + "'.");
        return wrote;
    });
}

// The shared core behind both ProcessTif (standalone single file - parallelSubtasks=true) and
// ProcessFile (one file within a batch - parallelSubtasks=false, since the batch dispatcher is
// already the layer providing concurrency).
bool DecodeAndExport(const TifSplitterOptions& options, bool parallelSubtasks) {
    std::error_code existsErr;
    if (!std::filesystem::exists(options.InputFilePath, existsErr)) {
        LogError("Input file does not exist: " + options.InputFilePath);
        return false;
    }

    TIFF* tif = TIFFOpen(options.InputFilePath.c_str(), "r");
    if (!tif) {
        // libtiff already prints its own diagnostic (via its default warning/error handlers)
        // before returning null here, so this just confirms the overall outcome.
        LogError("Failed to open/parse TIFF: " + options.InputFilePath);
        return false;
    }

    uint32_t width = 0, height = 0;
    uint16_t bitsPerSample = 8, samplesPerPixel = 1;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);

    if (width == 0 || height == 0) {
        LogError("TIFF reports zero width/height: " + options.InputFilePath);
        TIFFClose(tif);
        return false;
    }

    LogInfo("Loading TIF: " + options.InputFilePath + " (" + std::to_string(width) + "x" + std::to_string(height)
        + ", " + std::to_string(samplesPerPixel) + " channel(s), " + std::to_string(bitsPerSample) + "-bit)...");

    std::vector<uint32_t> raster;
    try {
        raster.resize((size_t)width * height);
    } catch (const std::bad_alloc&) {
        LogError("Out of memory decoding a " + std::to_string(width) + "x" + std::to_string(height) + " image.");
        TIFFClose(tif);
        return false;
    }

    // See the header comment for why this (rather than hand-decoded scanlines) is the right
    // call here, not a shortcut: it's the one libtiff entry point that correctly normalizes
    // every source layout to 8-bit RGBA, which is also all the engine's own loader can use.
    int decoded = TIFFReadRGBAImageOriented(tif, width, height, raster.data(), ORIENTATION_TOPLEFT, 0);
    TIFFClose(tif);
    if (!decoded) {
        LogError("libtiff failed to decode pixel data for: " + options.InputFilePath);
        return false;
    }

    PixelBuffer rgba;
    rgba.Width = (int)width;
    rgba.Height = (int)height;
    rgba.Channels = 4;
    rgba.Data.resize((size_t)width * height * 4);
    for (size_t i = 0; i < raster.size(); ++i) {
        uint32_t px = raster[i];
        rgba.Data[i * 4 + 0] = (uint8_t)TIFFGetR(px);
        rgba.Data[i * 4 + 1] = (uint8_t)TIFFGetG(px);
        rgba.Data[i * 4 + 2] = (uint8_t)TIFFGetB(px);
        rgba.Data[i * 4 + 3] = (uint8_t)TIFFGetA(px);
    }
    raster.clear();
    raster.shrink_to_fit();

    if (options.FlipNormalY) {
        for (size_t p = 0, n = (size_t)width * height; p < n; ++p) {
            rgba.Data[p * 4 + 1] = (uint8_t)(255 - rgba.Data[p * 4 + 1]);
        }
        LogInfo("Flipped Green channel (DirectX -> OpenGL normal convention): " + options.InputFilePath);
    }

    std::error_code mkdirErr;
    std::filesystem::create_directories(options.OutputDirectory, mkdirErr);
    if (mkdirErr) {
        LogError("Could not create output directory '" + options.OutputDirectory + "': " + mkdirErr.message());
        return false;
    }

    std::string stem = std::filesystem::path(options.InputFilePath).stem().string();
    bool ok = true;

    if (options.ExtractIndividualChannels) {
        ok = ExtractChannels(rgba, options, options.OutputDirectory, stem, parallelSubtasks) && ok;
    }
    if (options.TileSize > 0) {
        ok = TileAndWrite(rgba, options, options.OutputDirectory, stem, parallelSubtasks) && ok;
    }
    if (!options.ExtractIndividualChannels && options.TileSize <= 0) {
        std::string outPath = options.OutputDirectory + "/" + stem + ".png";
        bool wrote = WritePng(outPath, rgba);
        if (wrote) {
            LogSuccess("Successfully compiled: " + options.InputFilePath + " -> " + outPath);
        } else {
            LogError("Failed to write '" + outPath + "'.");
        }
        ok = wrote && ok;
    }

    return ok;
}

bool HasTifExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".tif" || ext == ".tiff";
}

// Collects InputFiles plus a directory scan (if InputDirectory is set), de-duplicated by
// resolved path so the same file listed both explicitly and found by the scan is only
// converted once.
std::vector<std::filesystem::path> CollectBatchFiles(const BatchTifOptions& options) {
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> seen;

    auto tryAdd = [&](const std::filesystem::path& p) {
        std::error_code ec;
        std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
        if (ec) canon = p;
        if (seen.insert(canon).second) result.push_back(p);
    };

    for (const auto& f : options.InputFiles) {
        if (!HasTifExtension(f)) {
            LogWarn("Skipping non-TIFF file in explicit list: " + f.string());
            continue;
        }
        tryAdd(f);
    }

    if (!options.InputDirectory.empty()) {
        std::error_code existsEc;
        if (!std::filesystem::exists(options.InputDirectory, existsEc) ||
            !std::filesystem::is_directory(options.InputDirectory, existsEc)) {
            LogError("Input directory does not exist: " + options.InputDirectory.string());
        } else {
            std::error_code scanEc;
            if (options.Recursive) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         options.InputDirectory, std::filesystem::directory_options::skip_permission_denied, scanEc)) {
                    if (entry.is_regular_file() && HasTifExtension(entry.path())) tryAdd(entry.path());
                }
            } else {
                for (const auto& entry : std::filesystem::directory_iterator(
                         options.InputDirectory, std::filesystem::directory_options::skip_permission_denied, scanEc)) {
                    if (entry.is_regular_file() && HasTifExtension(entry.path())) tryAdd(entry.path());
                }
            }
            if (scanEc) LogWarn("Directory scan of '" + options.InputDirectory.string() + "' reported: " + scanEc.message());
        }
    }

    return result;
}

// Mirrors `file`'s subfolder position under InputDirectory into OutputDirectory - e.g.
// Raw/Rocks/Height.tif (with InputDirectory=Raw) writes to Output/Rocks/. A file outside
// InputDirectory entirely (an explicit -f path unrelated to -d, or no -d given at all) just
// goes straight into OutputDirectory with no subfolder.
std::filesystem::path ComputeMirroredOutputDir(const std::filesystem::path& file, const BatchTifOptions& options) {
    if (options.InputDirectory.empty()) return options.OutputDirectory;

    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(file.parent_path(), options.InputDirectory, ec);
    if (ec) return options.OutputDirectory;

    bool underRoot = std::all_of(rel.begin(), rel.end(), [](const std::filesystem::path& part) {
        return part.native() != std::filesystem::path("..").native();
    });
    if (!underRoot) return options.OutputDirectory;

    return options.OutputDirectory / rel;
}

} // namespace

bool TifConverter::ProcessTif(const TifSplitterOptions& options) {
    return DecodeAndExport(options, /*parallelSubtasks=*/true);
}

bool TifConverter::ProcessFile(const std::filesystem::path& file, const BatchTifOptions& options) {
    TifSplitterOptions perFile;
    perFile.InputFilePath = file.string();
    perFile.OutputDirectory = ComputeMirroredOutputDir(file, options).string();
    perFile.ExtractIndividualChannels = options.ExtractIndividualChannels;
    perFile.FlipNormalY = options.FlipNormalY;
    perFile.NormalizeHeightmaps = options.NormalizeHeightmaps;
    perFile.TileSize = options.TileSize;
    return DecodeAndExport(perFile, /*parallelSubtasks=*/false);
}

void TifConverter::ProcessBatch(const BatchTifOptions& options, BatchProgressCallback progressCB) {
    auto startTime = std::chrono::steady_clock::now();

    std::vector<std::filesystem::path> files = CollectBatchFiles(options);
    if (files.empty()) {
        LogWarn("No .tif/.tiff files found to process.");
        return;
    }

    unsigned int workerCount = options.MaxThreads > 0 ? (unsigned int)options.MaxThreads : WorkerCount();
    unsigned int actualWorkers = std::min<unsigned int>(workerCount, (unsigned int)files.size());

    LogInfo("Starting batch: " + std::to_string(files.size()) + " file(s), "
        + std::to_string(actualWorkers) + " worker thread(s)...");

    BatchProgress progress;
    progress.TotalFiles = files.size();
    std::mutex progressMutex;

    struct FailureRecord { std::string FileName; std::string Reason; };
    std::vector<FailureRecord> failures;
    std::mutex failuresMutex;

    std::atomic<size_t> nextIndex{0};

    auto worker = [&]() {
        for (;;) {
            size_t index = nextIndex.fetch_add(1);
            if (index >= files.size()) return;

            const std::filesystem::path& file = files[index];
            std::string fileName = file.filename().string();
            LogInfo("Importing asset: " + file.string());

            bool ok = false;
            std::string errorReason;
            try {
                ok = ProcessFile(file, options);
                if (!ok) errorReason = "conversion failed - see [Error] log line(s) above for detail";
            } catch (const std::exception& e) {
                errorReason = e.what();
            } catch (...) {
                errorReason = "unknown exception";
            }

            if (!ok) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back({fileName, errorReason});
            }

            BatchProgress snapshot;
            {
                std::lock_guard<std::mutex> lock(progressMutex);
                progress.CompletedFiles++;
                if (!ok) progress.FailedFiles++;
                progress.CurrentFileName = fileName;
                snapshot = progress;
            }
            // Invoked here, off the shared lock, from whatever worker finished this file - see
            // the header's note on BatchProgressCallback about not being the calling thread.
            if (progressCB) progressCB(snapshot);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(actualWorkers);
    for (unsigned int i = 0; i < actualWorkers; ++i) workers.emplace_back(worker);
    for (auto& t : workers) t.join();

    double elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();

    std::lock_guard<std::mutex> lock(LogMutex());
    std::cout << "\n===== Batch Summary =====\n";
    std::cout << "Processed:  " << progress.CompletedFiles << " / " << progress.TotalFiles << "\n";
    std::cout << "Succeeded:  " << (progress.CompletedFiles - progress.FailedFiles) << "\n";
    std::cout << "Failed:     " << progress.FailedFiles << "\n";
    std::cout << "Time taken: " << std::fixed << std::setprecision(2) << elapsedSeconds << "s\n";
    if (!failures.empty()) {
        std::cout << "Failures:\n";
        for (const auto& f : failures) std::cout << "  - " << f.FileName << ": " << f.Reason << "\n";
    }
    std::cout << "==========================\n";
}
