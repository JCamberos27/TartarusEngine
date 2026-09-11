#pragma once
#include <string>

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

    Type TextureType = Type::Default;

    bool GenerateMipmaps = true;
    // Decodes the source as sRGB-encoded on sample (GL_SRGB8_ALPHA8/GL_SRGB8 internal format)
    // instead of raw bytes — correct for an authored color/albedo texture, wrong for a data
    // map (normal, mask, roughness) where the numbers ARE the linear values already.
    bool IsSRGB = true;
    Filter FilterMode = Filter::Bilinear;
    Wrap WrapMode = Wrap::Repeat;
    // Downscales on import (nearest-neighbor) if the source exceeds this in either dimension,
    // preserving aspect ratio. 0 or negative = no limit.
    int MaxTextureSize = 2048;
};

// Cumulative timing across every Texture::UploadFromFile call this process has made (audit
// ARCH-201 / #375). Decode covers the TextureCache hit-or-miss path (stb_image + DownsampleBox on
// a miss); Upload covers the glCreateTextures/.../glGenerateTextureMipmap block. Not thread-safe —
// fine today since all texture loading is main-thread-only (that's the ARCH-201 finding). Reset()
// lets a caller (e.g. --asset-load-bench) isolate one batch's cost instead of the process total.
struct TextureLoadStats {
    int Count = 0;
    double DecodeMs = 0.0;
    double UploadMs = 0.0;

    static TextureLoadStats& Get();
    static void Reset() { Get() = TextureLoadStats{}; }
};

// A single 2D GL texture loaded from disk via stb_image (PNG/JPG/TGA/BMP/...).
class Texture {
public:
    explicit Texture(const std::string& path);
    Texture(const std::string& path, const TextureImportSettings& settings);
    ~Texture();

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
    unsigned int GLHandle() const { return m_ID; } // for ImGui::Image thumbnails in the Asset Browser
    const TextureImportSettings& ImportSettings() const { return m_Settings; }

private:
    void UploadFromFile(const TextureImportSettings& settings);

    unsigned int m_ID = 0;
    int m_Width = 0, m_Height = 0, m_Channels = 0;
    std::string m_Path;
    TextureImportSettings m_Settings;
};
