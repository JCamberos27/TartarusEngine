#include "TifSplitter.h"

#include <tiffio.h>
#include <stb_image_write.h>

// Defined in stb_image_write's implementation section (extern/stb_image_write_impl.cpp) but only
// declared there, not in the header's API part; WritePng16 below uses it to deflate its rows.
// Its buffer is released with free(), stb's default STBIW_FREE.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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

uint32_t Crc32(const uint8_t* data, size_t len) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
}

// #185: 16-bit grayscale PNG. stbi_write_png only writes 8-bit, but its zlib compressor is
// public, so this assembles the chunks around it: PNG stores 16-bit samples big-endian, one
// filter-type byte per row (Sub, which compresses smooth height data well).
bool WritePng16(const std::string& path, int width, int height, const std::vector<uint16_t>& gray) {
    const size_t rowBytes = (size_t)width * 2 + 1;
    const size_t rawSize = rowBytes * (size_t)height;
    if (rawSize > (size_t)INT32_MAX) return false; // stbi_zlib_compress takes an int length
    std::vector<uint8_t> raw(rawSize);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = &raw[(size_t)y * rowBytes];
        row[0] = 1; // filter: Sub (byte-wise difference from the byte 2 to the left)
        const uint16_t* src = &gray[(size_t)y * width];
        for (int x = 0; x < width; ++x) {
            const uint16_t cur = src[x], left = x > 0 ? src[x - 1] : 0;
            row[1 + x * 2]     = (uint8_t)((cur >> 8) - (left >> 8));
            row[1 + x * 2 + 1] = (uint8_t)((cur & 0xFF) - (left & 0xFF));
        }
    }
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(raw.data(), (int)raw.size(), &zlen, 8);
    if (!z) return false;

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const uint8_t* data, size_t len) {
        PutBE32(png, (uint32_t)len);
        const size_t typeAt = png.size();
        png.insert(png.end(), type, type + 4);
        if (len) png.insert(png.end(), data, data + len);
        PutBE32(png, Crc32(&png[typeAt], len + 4));
    };
    std::vector<uint8_t> ihdr;
    PutBE32(ihdr, (uint32_t)width);
    PutBE32(ihdr, (uint32_t)height);
    ihdr.insert(ihdr.end(), {16, 0, 0, 0, 0}); // 16-bit, grayscale, deflate, adaptive filter, no interlace
    chunk("IHDR", ihdr.data(), ihdr.size());
    chunk("IDAT", z, (size_t)zlen);
    chunk("IEND", nullptr, 0);
    std::free(z);

    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    return f.write((const char*)png.data(), (std::streamsize)png.size()) && f.flush();
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

// Full-precision sample planes for a 16/32/64-bit source, one float plane per TIFF sample,
// top-to-bottom. Empty when the source is 8-bit or a layout ReadHighPrecisionPlanes declines.
// #185: normalising these and only then quantising avoids the terracing you get from stretching
// data that TIFFReadRGBAImageOriented has already cut to 256 levels.
using SamplePlanes = std::vector<std::vector<float>>;

// Float counterpart of StretchContrast: maps the plane's finite min..max onto 0..255. Non-finite
// samples (NaN / inf no-data markers in float DEMs) become 0.
void StretchToBytes(const std::vector<float>& plane, std::vector<uint8_t>& out) {
    float lo = INFINITY, hi = -INFINITY;
    for (float v : plane) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    out.resize(plane.size());
    const bool flat = !(hi > lo);
    const double scale = flat ? 0.0 : 255.0 / ((double)hi - (double)lo);
    for (size_t i = 0; i < plane.size(); ++i) {
        float v = plane[i];
        if (!std::isfinite(v) || flat) { out[i] = 0; continue; }
        out[i] = (uint8_t)std::clamp(std::lround(((double)v - lo) * scale), 0l, 255l);
    }
}

// 16-bit counterpart of StretchToBytes (#185). `stretch` false keeps values as they are (only
// sensible for a 16-bit unsigned source, whose samples already are 0..65535).
void ToWords(const std::vector<float>& plane, bool stretch, std::vector<uint16_t>& out) {
    out.resize(plane.size());
    if (!stretch) {
        for (size_t i = 0; i < plane.size(); ++i)
            out[i] = std::isfinite(plane[i]) ? (uint16_t)std::clamp(std::lround(plane[i]), 0l, 65535l) : 0;
        return;
    }
    float lo = INFINITY, hi = -INFINITY;
    for (float v : plane) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const bool flat = !(hi > lo);
    const double scale = flat ? 0.0 : 65535.0 / ((double)hi - (double)lo);
    for (size_t i = 0; i < plane.size(); ++i) {
        float v = plane[i];
        out[i] = (!std::isfinite(v) || flat) ? 0 : (uint16_t)std::clamp(std::lround(((double)v - lo) * scale), 0l, 65535l);
    }
}

float SampleToFloat(const uint8_t* p, uint16_t bits, uint16_t format) {
    switch (bits) {
    case 16:
        if (format == SAMPLEFORMAT_INT) { int16_t v; std::memcpy(&v, p, 2); return (float)v; }
        { uint16_t v; std::memcpy(&v, p, 2); return (float)v; }
    case 32:
        if (format == SAMPLEFORMAT_IEEEFP) { float v; std::memcpy(&v, p, 4); return v; }
        if (format == SAMPLEFORMAT_INT) { int32_t v; std::memcpy(&v, p, 4); return (float)v; }
        { uint32_t v; std::memcpy(&v, p, 4); return (float)v; }
    default: // 64: IEEE double only (checked by the caller)
        { double v; std::memcpy(&v, p, 8); return (float)v; }
    }
}

// Reads every sample of a 16/32/64-bit grayscale or RGB(A) TIFF at full precision. Returns empty
// (and the caller keeps the 8-bit RGBA decode) for anything else: 8-bit data, palette / YCbCr /
// CMYK, a non-top-left orientation, or a read error. Handles stripped and tiled files, both
// contiguous and separate planar layouts.
SamplePlanes ReadHighPrecisionPlanes(TIFF* tif, uint32_t width, uint32_t height,
                                     uint16_t bits, uint16_t spp, const std::string& name) {
    uint16_t format = SAMPLEFORMAT_UINT, photometric = PHOTOMETRIC_MINISBLACK,
             planar = PLANARCONFIG_CONTIG, orientation = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);

    const bool bitsOk = (bits == 16 && format != SAMPLEFORMAT_IEEEFP) || bits == 32 ||
                        (bits == 64 && format == SAMPLEFORMAT_IEEEFP);
    const bool photoOk = photometric == PHOTOMETRIC_MINISBLACK ||
                         photometric == PHOTOMETRIC_MINISWHITE || photometric == PHOTOMETRIC_RGB;
    if (!bitsOk || !photoOk || spp == 0) return {};
    if (orientation != ORIENTATION_TOPLEFT) {
        LogWarn("Non top-left TIFF orientation; normalising the 8-bit decode instead: " + name);
        return {};
    }

    SamplePlanes planes;
    try {
        planes.assign(spp, std::vector<float>((size_t)width * height));
    } catch (const std::bad_alloc&) {
        LogWarn("Not enough memory for a full-precision decode; normalising the 8-bit decode instead: " + name);
        return {};
    }

    const size_t bytesPerSample = bits / 8;
    const bool contig = planar == PLANARCONFIG_CONTIG;
    const uint16_t passes = contig ? 1 : spp; // separate planes are read one sample at a time
    const size_t stride = contig ? spp * bytesPerSample : bytesPerSample; // bytes between pixels

    // Copies one run of `count` pixels starting at image (x, y) out of `src`.
    auto store = [&](const uint8_t* src, uint32_t x, uint32_t y, uint32_t count, uint16_t pass) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* px = src + i * stride;
            size_t dst = (size_t)y * width + x + i;
            if (contig) {
                for (uint16_t c = 0; c < spp; ++c)
                    planes[c][dst] = SampleToFloat(px + c * bytesPerSample, bits, format);
            } else {
                planes[pass][dst] = SampleToFloat(px, bits, format);
            }
        }
    };

    bool ok = true;
    if (TIFFIsTiled(tif)) {
        uint32_t tw = 0, th = 0;
        TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
        std::vector<uint8_t> tile(TIFFTileSize(tif));
        const size_t tileRow = TIFFTileRowSize(tif);
        for (uint16_t pass = 0; ok && pass < passes; ++pass)
            for (uint32_t ty = 0; ok && ty < height; ty += th)
                for (uint32_t tx = 0; ok && tx < width; tx += tw) {
                    if (TIFFReadTile(tif, tile.data(), tx, ty, 0, pass) < 0) { ok = false; break; }
                    const uint32_t w = std::min(tw, width - tx), h = std::min(th, height - ty);
                    for (uint32_t r = 0; r < h; ++r) store(tile.data() + r * tileRow, tx, ty + r, w, pass);
                }
    } else {
        std::vector<uint8_t> line(TIFFScanlineSize(tif));
        for (uint16_t pass = 0; ok && pass < passes; ++pass)
            for (uint32_t y = 0; y < height; ++y) {
                if (TIFFReadScanline(tif, line.data(), y, pass) < 0) { ok = false; break; }
                store(line.data(), 0, y, width, pass);
            }
    }
    if (!ok) {
        LogWarn("Full-precision read failed; normalising the 8-bit decode instead: " + name);
        return {};
    }

    if (photometric == PHOTOMETRIC_MINISWHITE) {
        for (float& v : planes[0]) v = -v; // 0 is white: invert so the stretch maps high -> bright
    }
    return planes;
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

// Which full-precision sample feeds output channel c (R/G/B/A), or -1 for none. Mirrors how
// TIFFReadRGBAImageOriented expands the source: gray fills R/G/B, a second sample is alpha.
int SourceSampleFor(int c, size_t spp) {
    if (spp == 0) return -1;
    if (spp < 3) return c < 3 ? 0 : (spp == 2 ? 1 : -1);
    if (c < 3) return c;
    return spp >= 4 ? 3 : -1;
}

bool ExtractChannels(const PixelBuffer& rgba, const SamplePlanes& planes, bool rawIs16Bit,
    const TifSplitterOptions& options, const std::string& outDir, const std::string& stem, bool parallel) {
    static const char* kNames[4] = {"R", "G", "B", "A"};
    size_t pixelCount = (size_t)rgba.Width * rgba.Height;

    return RunBounded(4, parallel ? WorkerCount() : 1, [&](size_t c) {
        const int src16 = SourceSampleFor((int)c, planes.size());
        if (options.Output16Bit && src16 >= 0) {
            // #185: full-precision channel straight to a 16-bit PNG.
            std::vector<uint16_t> words;
            ToWords(planes[src16], options.NormalizeHeightmaps || !rawIs16Bit, words);
            if (options.FlipNormalY && c == 1)
                for (uint16_t& v : words) v = (uint16_t)(65535 - v);
            std::string outPath = outDir + "/" + stem + "_" + kNames[c] + ".png";
            bool wrote = WritePng16(outPath, rgba.Width, rgba.Height, words);
            if (wrote) LogSuccess("Wrote 16-bit channel " + std::string(kNames[c]) + " -> " + outPath);
            else LogError("Failed to write '" + outPath + "'.");
            return wrote;
        }

        PixelBuffer channel;
        channel.Width = rgba.Width;
        channel.Height = rgba.Height;
        channel.Channels = 1;
        const int src = SourceSampleFor((int)c, planes.size());
        if (options.NormalizeHeightmaps && src >= 0) {
            StretchToBytes(planes[src], channel.Data); // #185: stretch first, quantise last
            // Same result as the 8-bit path's flip-then-stretch: stretching 255-v swaps min/max.
            if (options.FlipNormalY && c == 1)
                for (uint8_t& v : channel.Data) v = (uint8_t)(255 - v);
        } else {
            channel.Data.resize(pixelCount);
            for (size_t p = 0; p < pixelCount; ++p) channel.Data[p] = rgba.Data[p * 4 + c];
            if (options.NormalizeHeightmaps) StretchContrast(channel.Data);
        }

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

    // Split, normalised channels of a high-bit-depth source are built from full-precision
    // samples (#185); read those before the RGBA decode below moves libtiff's read position.
    // Float samples are read this way whatever the options: TIFFReadRGBAImageOriented rejects
    // them, so these planes are the only decode a float DEM gets.
    uint16_t sampleFormat = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    const bool isFloat = sampleFormat == SAMPLEFORMAT_IEEEFP;
    SamplePlanes planes;
    if (bitsPerSample > 8 &&
        ((options.ExtractIndividualChannels && (options.NormalizeHeightmaps || options.Output16Bit)) || isFloat)) {
        planes = ReadHighPrecisionPlanes(tif, width, height, bitsPerSample, samplesPerPixel,
                                         options.InputFilePath);
        if (!planes.empty()) LogInfo("Read full " + std::to_string(bitsPerSample)
                                     + "-bit precision samples: " + options.InputFilePath);
    }

    // Everything else (combined image, tiles, un-normalised channels) goes through the one
    // libtiff entry point that correctly converts every source layout to 8-bit RGBA, which is
    // also all the engine's own loader reads.
    // Float data has no fixed range for it to map from, so it's expected to fail here; the
    // planes above stand in (#185).
    int decoded = isFloat ? 0 : TIFFReadRGBAImageOriented(tif, width, height, raster.data(), ORIENTATION_TOPLEFT, 0);
    TIFFClose(tif);

    PixelBuffer rgba;
    rgba.Width = (int)width;
    rgba.Height = (int)height;
    rgba.Channels = 4;
    rgba.Data.resize((size_t)width * height * 4);
    if (!decoded) {
        if (planes.empty()) {
            LogError("libtiff failed to decode pixel data for: " + options.InputFilePath);
            return false;
        }
        // Each channel stretched over its own min..max: the only meaningful 8-bit view of samples
        // with no inherent range. Alpha is opaque unless the file carries one.
        LogInfo("Mapping each channel's value range to 0..255 for 8-bit output: " + options.InputFilePath);
        std::vector<uint8_t> bytes;
        for (int c = 0; c < 4; ++c) {
            const int src = SourceSampleFor(c, planes.size());
            if (src >= 0) StretchToBytes(planes[src], bytes);
            for (size_t p = 0, n = (size_t)width * height; p < n; ++p)
                rgba.Data[p * 4 + c] = src >= 0 ? bytes[p] : 255;
        }
    }
    for (size_t i = 0; decoded && i < raster.size(); ++i) {
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

    std::string stem = options.OutputStem.empty()
        ? std::filesystem::path(options.InputFilePath).stem().string() : options.OutputStem;
    bool ok = true;

    if (options.Output16Bit && options.ExtractIndividualChannels && planes.empty()) {
        LogWarn("16-bit output needs a 16/32/64-bit grayscale or RGB source; writing 8-bit channels for: "
                + options.InputFilePath);
    }
    if (options.ExtractIndividualChannels) {
        const bool rawIs16Bit = bitsPerSample == 16 && sampleFormat == SAMPLEFORMAT_UINT;
        ok = ExtractChannels(rgba, planes, rawIs16Bit, options, options.OutputDirectory, stem, parallelSubtasks) && ok;
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

bool TifConverter::ProcessFile(const std::filesystem::path& file, const BatchTifOptions& options,
                               const std::string& outputStem) {
    TifSplitterOptions perFile;
    perFile.OutputStem = outputStem;
    perFile.Output16Bit = options.Output16Bit;
    perFile.InputFilePath = file.string();
    perFile.OutputDirectory = ComputeMirroredOutputDir(file, options).string();
    perFile.ExtractIndividualChannels = options.ExtractIndividualChannels;
    perFile.FlipNormalY = options.FlipNormalY;
    perFile.NormalizeHeightmaps = options.NormalizeHeightmaps;
    perFile.TileSize = options.TileSize;
    return DecodeAndExport(perFile, /*parallelSubtasks=*/false);
}

BatchProgress TifConverter::ProcessBatch(const BatchTifOptions& options, BatchProgressCallback progressCB) {
    auto startTime = std::chrono::steady_clock::now();

    std::vector<std::filesystem::path> files = CollectBatchFiles(options);
    if (files.empty()) {
        LogWarn("No .tif/.tiff files found to process.");
        return {};
    }

    // #185: a.tif and a.tiff (or A.tif and a.tif - Windows paths are case-insensitive) landing in
    // the same output folder would write the same a.png / a_R.png ... and silently overwrite
    // each other. Every member of such a group keeps its extension in the name: a_tif, a_tiff.
    std::vector<std::string> outputStems(files.size());
    {
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            return s;
        };
        std::map<std::string, std::vector<size_t>> byTarget;
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string key = lower((ComputeMirroredOutputDir(files[i], options) / files[i].stem()).generic_string());
            byTarget[key].push_back(i);
        }
        for (const auto& [key, members] : byTarget) {
            if (members.size() < 2) continue;
            std::map<std::string, int> seen; // A.tif + a.tif: same name even with the extension
            for (size_t i : members) {
                const std::filesystem::path& f = files[i];
                std::string ext = f.extension().string();
                if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
                outputStems[i] = f.stem().string() + "_" + ext;
                const int n = seen[lower(outputStems[i])]++;
                if (n > 0) outputStems[i] += "_" + std::to_string(n + 1);
                LogWarn("'" + f.string() + "' shares its output name with another input; writing it as "
                        + outputStems[i] + "*.png");
            }
        }
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
                ok = ProcessFile(file, options, outputStems[index]);
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
    return progress; // every worker has joined, so this is the final, race-free tally
}
