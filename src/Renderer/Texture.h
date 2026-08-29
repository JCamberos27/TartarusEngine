#pragma once
#include <string>

// Mirrors the knobs a Unity-style Texture Importer would expose. Stored per-asset-path in
// AssetLibrary (see AssetLibrary::TextureSettings/SetTextureSettings) and applied whenever a
// Texture is constructed or re-imported — see Texture::Reimport.
struct TextureImportSettings {
    enum class Type { Default, NormalMap, Sprite2D, Cubemap };
    enum class Filter { Point, Bilinear, Trilinear };
    enum class Wrap { Repeat, ClampToEdge };

    // Informational for now — nothing downstream (Model's material extraction, the Inspector's
    // map slots) branches on it yet. Kept here rather than left out because it's the field the
    // rest of this struct's meaning depends on (e.g. sRGB should only ever be true for a color
    // texture, never a normal/mask map) — see AssetImporterInspector's tooltip on this field.
    Type TextureType = Type::Default;

    bool GenerateMipmaps = true;
    // Decodes the source as sRGB-encoded on sample (GL_SRGB8_ALPHA8/GL_SRGB8 internal format)
    // instead of raw bytes — correct for an authored color/albedo texture, wrong for a data
    // map (normal, mask, roughness) where the numbers ARE the linear values already.
    bool IsSRGB = true;
    // Kept for parity with Unity's importer and persisted with the rest of the settings, but
    // this engine never evicts CPU pixel data after upload in a way this flag would toggle —
    // it has no effect on behavior today. Not wired to anything rather than faked.
    bool IsReadable = false;
    Filter FilterMode = Filter::Bilinear;
    Wrap WrapMode = Wrap::Repeat;
    // Downscales on import (nearest-neighbor) if the source exceeds this in either dimension,
    // preserving aspect ratio. 0 or negative = no limit.
    int MaxTextureSize = 2048;
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
