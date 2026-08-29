#include "Texture.h"
#include "Log.h"
#include "gl.h"
#include "GLStateCache.h"
#include "stb_image.h"
#include <algorithm>
#include <vector>

namespace {

// Nearest-neighbor downsample to fit within maxSize x maxSize, preserving aspect ratio. No new
// dependency (a proper box/Lanczos filter would need stb_image_resize, not vendored here) —
// good enough for "cap an oversized source texture", not a quality-critical resize path.
std::vector<unsigned char> DownsampleNearest(const unsigned char* src, int srcW, int srcH, int channels,
    int maxSize, int& outW, int& outH) {
    float scale = std::min((float)maxSize / srcW, (float)maxSize / srcH);
    outW = std::max(1, (int)(srcW * scale));
    outH = std::max(1, (int)(srcH * scale));

    std::vector<unsigned char> dst((size_t)outW * outH * channels);
    for (int y = 0; y < outH; ++y) {
        int sy = std::min(srcH - 1, (int)((y + 0.5f) * srcH / outH));
        for (int x = 0; x < outW; ++x) {
            int sx = std::min(srcW - 1, (int)((x + 0.5f) * srcW / outW));
            const unsigned char* s = src + ((size_t)sy * srcW + sx) * channels;
            unsigned char* d = dst.data() + ((size_t)y * outW + x) * channels;
            for (int c = 0; c < channels; ++c) d[c] = s[c];
        }
    }
    return dst;
}

} // namespace

Texture::Texture(const std::string& path) : Texture(path, TextureImportSettings{}) {}

Texture::Texture(const std::string& path, const TextureImportSettings& settings)
    : m_Path(path), m_Settings(settings) {
    UploadFromFile(settings);
}

void Texture::UploadFromFile(const TextureImportSettings& settings) {
    // Do NOT flip on load: Model.cpp already applies Assimp's aiProcess_FlipUVs to correct
    // the top-left-origin (FBX/glTF) vs bottom-left-origin (OpenGL) mismatch on the mesh's
    // own UVs. Flipping the image here too double-corrects it, leaving UV islands sampling
    // the wrong region of the texture (mirrored vertically relative to where they should be).
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(m_Path.c_str(), &m_Width, &m_Height, &m_Channels, 0);
    if (!data) {
        Log::Error("Texture: failed to load '" + m_Path + "'.");
        return;
    }

    std::vector<unsigned char> resized;
    const unsigned char* uploadData = data;
    int uploadW = m_Width, uploadH = m_Height;
    if (settings.MaxTextureSize > 0 && (m_Width > settings.MaxTextureSize || m_Height > settings.MaxTextureSize)) {
        resized = DownsampleNearest(data, m_Width, m_Height, m_Channels, settings.MaxTextureSize, uploadW, uploadH);
        uploadData = resized.data();
    }

    GLenum format = GL_RGB;
    GLint internalFormat = GL_RGB;
    if (m_Channels == 1) {
        format = GL_RED;
        internalFormat = GL_RED; // no single-channel sRGB format in core GL - not a color texture anyway
    } else if (m_Channels == 3) {
        format = GL_RGB;
        internalFormat = settings.IsSRGB ? GL_SRGB8 : GL_RGB;
    } else if (m_Channels == 4) {
        format = GL_RGBA;
        internalFormat = settings.IsSRGB ? GL_SRGB8_ALPHA8 : GL_RGBA;
    }

    glGenTextures(1, &m_ID);
    glBindTexture(GL_TEXTURE_2D, m_ID);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, uploadW, uploadH, 0, format, GL_UNSIGNED_BYTE, uploadData);

    if (settings.GenerateMipmaps) glGenerateMipmap(GL_TEXTURE_2D);

    GLint wrap = settings.WrapMode == TextureImportSettings::Wrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

    GLint minFilter, magFilter;
    switch (settings.FilterMode) {
        case TextureImportSettings::Filter::Point:
            minFilter = settings.GenerateMipmaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
            magFilter = GL_NEAREST;
            break;
        case TextureImportSettings::Filter::Trilinear:
            minFilter = settings.GenerateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            magFilter = GL_LINEAR;
            break;
        case TextureImportSettings::Filter::Bilinear:
        default:
            // "Bilinear" in Unity's sense still mip-selects, it just doesn't blend BETWEEN mip
            // levels the way Trilinear does - GL_LINEAR_MIPMAP_NEAREST is the matching mode.
            minFilter = settings.GenerateMipmaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
            magFilter = GL_LINEAR;
            break;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

    stbi_image_free(data);
    m_Settings = settings;
}

bool Texture::Reimport(const TextureImportSettings& settings) {
    // Verify the file is still readable BEFORE tearing down the current GL texture, so a
    // missing/corrupt file on disk leaves the existing (still-valid) texture in place rather
    // than leaving this Texture pointing at a deleted GL name.
    int w, h, c;
    if (!stbi_info(m_Path.c_str(), &w, &h, &c)) {
        Log::Error("Texture: cannot reimport '" + m_Path + "' - file is missing or unreadable.");
        return false;
    }

    if (m_ID) glDeleteTextures(1, &m_ID);
    m_ID = 0;
    UploadFromFile(settings);
    return m_ID != 0;
}

Texture::~Texture() {
    if (m_ID) glDeleteTextures(1, &m_ID);
}

void Texture::Bind(unsigned int unit) const {
    GLStateCache::BindTexture2D(unit, m_ID);
}
