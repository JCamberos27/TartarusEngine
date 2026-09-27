#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Mirrors the knobs a Unity-style Texture Importer would expose. Stored per-asset-path in
// AssetLibrary (see AssetLibrary::TextureSettings/SetTextureSettings) and applied whenever a
// Texture is constructed or re-imported — see Texture::Reimport.
struct TextureImportSettings {
    // Cubemap import doesn't exist in this engine (#198) — dropped rather than shipping an enum
    // value nothing implements. Drives real behavior in Texture::Reimport: NormalMap forces
    // IsSRGB off (normal data is never color-encoded), Sprite2D clamps to edge with no mipmaps.
    enum class Type { Default, NormalMap, Sprite2D };
    enum class Filter { Point, Bilinear, Trilinear };
    enum class Wrap { Repeat, ClampToEdge };
    // #156 - GPU block compression (Unity's Compression). None uploads raw 8-bit pixels. Normal /
    // High Quality encode BCn on import (High runs extra endpoint refinement, ~40% slower to
    // bake): BC1 for opaque colour, BC3 with alpha, BC4 for single-channel data and BC5 for
    // two-channel data. 4-8x less VRAM than RGBA8. Encoded once and kept in Library/Textures.
    enum class Compression { None, Normal, HighQuality };

    Type TextureType = Type::Default;

    bool GenerateMipmaps = true;
    // Decodes the source as sRGB-encoded on sample (GL_SRGB8_ALPHA8/GL_SRGB8 internal format)
    // instead of raw bytes — correct for an authored color/albedo texture, wrong for a data
    // map (normal, mask, roughness) where the numbers ARE the linear values already.
    bool IsSRGB = true;
    Filter FilterMode = Filter::Bilinear;
    Wrap WrapMode = Wrap::Repeat;
    // Downscales on import (box filter; in linear space for sRGB textures) if the source exceeds
    // this in either dimension, preserving aspect ratio. 0 or negative = no limit.
    int MaxTextureSize = 2048;
    // #156 - Unity's Aniso Level: max anisotropic filtering samples (1 = off). Clamped to what the
    // driver supports; only applies with mipmaps and a non-Point filter. Sampler state only, so
    // it isn't part of TextureCache's pixel hash.
    int AnisoLevel = 8;
    Compression CompressionMode = Compression::None;
};

// Cumulative timing across every texture this process has loaded (audit ARCH-201 / #375).
// Decode covers the TextureCache hit-or-miss path (stb_image + DownsampleBox on a miss), which
// may run on an AsyncAssetLoader worker; Upload covers the GL block, always on the main thread.
// Add() is locked so workers can report; Get()/Reset() are for a single-threaded reader such as
// --asset-load-bench, which isolates one batch's cost instead of the process total.
struct TextureLoadStats {
    int Count = 0;
    double DecodeMs = 0.0;
    double UploadMs = 0.0;

    static TextureLoadStats& Get();
    static void Reset() { Get() = TextureLoadStats{}; }
    static void AddDecode(double ms);
    static void AddUpload(double ms); // also counts the texture
};

// A texture decoded on the CPU and ready for upload: everything Texture's GL half needs and
// nothing that touches GL, so it can be produced on any thread (Texture::DecodeFile/DecodeMemory)
// and turned into a Texture later on the main thread (Texture(TextureCpuData&&)).
struct TextureCpuData {
    std::string Path;                 // file path, or the embedded texture's name
    TextureImportSettings Settings;   // as requested (the cache key), not the effective ones
    bool Ok = false;
    int SourceWidth = 0, SourceHeight = 0, Channels = 0; // as authored
    int Width = 0, Height = 0;                           // as uploaded (after Max Size)
    // Tightly packed Width*Height*Channels bytes, or - when GLFormat != 0 - every BCn mip level's
    // blocks back to back, LevelSizes[i] bytes each.
    std::vector<unsigned char> Pixels;
    uint32_t GLFormat = 0;
    std::vector<uint32_t> LevelSizes;
    // #113 - an embedded source is kept so the texture can be reimported later.
    std::vector<unsigned char> Memory;
    int MemRawW = 0, MemRawH = 0;
};

// A single 2D GL texture loaded from disk via stb_image (PNG/JPG/TGA/BMP/...).
class Texture {
public:
    explicit Texture(const std::string& path);
    Texture(const std::string& path, const TextureImportSettings& settings);
    // #113 — a texture embedded in a model file (.glb / FBX embedded media). `bytes` is either an
    // encoded image (PNG/JPG..., rawWidth == 0) or raw RGBA8 pixels of rawWidth x rawHeight.
    // `name` is only an identity for logs and Path(); nothing is read from disk, and the texture
    // cache (keyed by file path) is bypassed.
    Texture(const std::string& name, std::vector<unsigned char> bytes, int rawWidth, int rawHeight,
            const TextureImportSettings& settings);
    // Uploads pixels decoded earlier, possibly on another thread. Main thread only (GL). A failed
    // decode (data.Ok false) gives an invalid texture, exactly like a failed file load.
    explicit Texture(TextureCpuData&& data);
    // A texture whose pixels are decoded but not uploaded yet: no GL, so it can be created on a
    // worker and handed to a Material, which keeps the same pointer when FinishUpload() (main
    // thread) turns it into a real GL texture. Null if the decode failed.
    static std::shared_ptr<Texture> CreatePending(TextureCpuData&& data);
    bool IsPendingUpload() const { return m_Pending != nullptr; }
    void FinishUpload();
    ~Texture();

    // The CPU half of loading a texture: TextureCache lookup, or stb_image decode + Max Size
    // downsample + optional BCn encode + TextureCache store on a miss. No GL, so safe on any
    // thread once InitGpuCaps() has run on the main thread (the BCn formats depend on the driver).
    static TextureCpuData DecodeFile(const std::string& path, const TextureImportSettings& settings);
    static TextureCpuData DecodeMemory(const std::string& name, std::vector<unsigned char> bytes, int rawWidth,
                                       int rawHeight, const TextureImportSettings& settings);
    // Queries the driver capabilities DecodeFile/DecodeMemory need. Main thread, once a GL context
    // is current; called by the first texture upload anyway, and by AsyncAssetLoader before it
    // starts its workers.
    static void InitGpuCaps();

    void Bind(unsigned int unit = 0) const;

    // Re-reads the source file from disk and re-uploads it with new settings applied. The old
    // GL texture name is deleted and a new one generated in its place — every existing caller
    // reads GLHandle() fresh at draw time (ImGui::Image, Bind) rather than caching it, so this
    // is safe to call on a Texture already referenced by a live Material. Returns false (and
    // leaves the previous GL texture and dimensions untouched) if the file can't be re-read.
    bool Reimport(const TextureImportSettings& settings);

    int Width() const { return m_Width; }
    int Height() const { return m_Height; }
    int SourceChannels() const { return m_Channels; }
    bool IsValid() const { return m_ID != 0; }
    const std::string& Path() const { return m_Path; }
    // #132 - the file was renamed or moved outside the editor; the uploaded pixels stay valid.
    void SetPath(const std::string& path) { m_Path = path; }
    unsigned int GLHandle() const { return m_ID; } // for ImGui::Image thumbnails in the Asset Browser
    const TextureImportSettings& ImportSettings() const { return m_Settings; }
    // #156 - what actually got uploaded, for the importer's memory readout: GPU bytes including
    // the mip chain, and a short format name ("RGBA8", "sRGB BC1", ...).
    size_t GpuBytes() const { return m_GpuBytes; }
    const char* GpuFormatName() const { return m_GpuFormatName; }

private:
    void UploadFromFile(const TextureImportSettings& settings);
    // The GL half: creates m_ID from `data` and fills the size/format fields. Leaves m_ID 0 if
    // the decode failed.
    void Upload(const TextureCpuData& data);

    Texture() = default; // CreatePending

    std::unique_ptr<TextureCpuData> m_Pending; // CreatePending, until FinishUpload
    std::vector<unsigned char> m_Memory; // #113 — embedded source, empty for file textures
    int m_MemRawW = 0, m_MemRawH = 0;

    unsigned int m_ID = 0;
    int m_Width = 0, m_Height = 0, m_Channels = 0;
    size_t m_GpuBytes = 0;
    const char* m_GpuFormatName = "";
    std::string m_Path;
    TextureImportSettings m_Settings;
};
