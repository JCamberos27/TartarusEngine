#include "Texture.h"
#include "Log.h"
#include "gl.h"
#include "GLStateCache.h"
#include "TextureCache.h"
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
    // Warm path: pixels already decoded and downsampled by a previous run. PNG decode dominates
    // scene-load time (measured ~4.6s of a ~5.6s cold boot on a 22-texture library), so skipping
    // it is the single biggest startup win available. See TextureCache.h.
    TextureCache::Image cached;
    bool fromCache = TextureCache::Load(m_Path, settings, cached);

    // Owns the decoded pixels only on a cache miss; `uploadData` points into either this, the
    // cache entry, or stb's buffer.
    unsigned char* data = nullptr;
    std::vector<unsigned char> resized;
    const unsigned char* uploadData = nullptr;
    int uploadW = 0, uploadH = 0;

    if (fromCache) {
        m_Width = cached.SourceWidth;
        m_Height = cached.SourceHeight;
        m_Channels = cached.Channels;
        uploadW = cached.Width;
        uploadH = cached.Height;
        uploadData = cached.Pixels.data();
    } else {
        // Do NOT flip on load: Model.cpp already applies Assimp's aiProcess_FlipUVs to correct
        // the top-left-origin (FBX/glTF) vs bottom-left-origin (OpenGL) mismatch on the mesh's
        // own UVs. Flipping the image here too double-corrects it, leaving UV islands sampling
        // the wrong region of the texture (mirrored vertically relative to where they should be).
        stbi_set_flip_vertically_on_load(false);
        data = stbi_load(m_Path.c_str(), &m_Width, &m_Height, &m_Channels, 0);
        if (!data) {
            Log::Error("Texture: failed to load '" + m_Path + "'.");
            return;
        }

        uploadData = data;
        uploadW = m_Width;
        uploadH = m_Height;
        if (settings.MaxTextureSize > 0 && (m_Width > settings.MaxTextureSize || m_Height > settings.MaxTextureSize)) {
            resized = DownsampleNearest(data, m_Width, m_Height, m_Channels, settings.MaxTextureSize, uploadW, uploadH);
            uploadData = resized.data();
        }

        // Bake the result for next time — post-downsample, so the cache stores exactly the
        // bytes glTexImage2D receives below.
        TextureCache::Image entry;
        entry.SourceWidth = m_Width;
        entry.SourceHeight = m_Height;
        entry.Width = uploadW;
        entry.Height = uploadH;
        entry.Channels = m_Channels;
        entry.Pixels.assign(uploadData, uploadData + (size_t)uploadW * uploadH * m_Channels);
        TextureCache::Store(m_Path, settings, entry);
    }

    // Sized internal formats on both branches (#100) — the sRGB branch already used them;
    // the linear branch used to pass bare GL_RGB/GL_RGBA/GL_RED, leaving precision to the driver
    // and blocking immutable-storage attachment.
    GLenum format = GL_RGB;
    GLint internalFormat = GL_RGB8;
    if (m_Channels == 1) {
        format = GL_RED;
        internalFormat = GL_R8; // no single-channel sRGB format in core GL - not a color texture anyway
    } else if (m_Channels == 3) {
        format = GL_RGB;
        internalFormat = settings.IsSRGB ? GL_SRGB8 : GL_RGB8;
    } else if (m_Channels == 4) {
        format = GL_RGBA;
        internalFormat = settings.IsSRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    }

    glGenTextures(1, &m_ID);
    glBindTexture(GL_TEXTURE_2D, m_ID);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    // The decode/cache path hands us tightly packed rows (stride = w*channels). Without this,
    // GL assumes 4-byte row alignment and shears any RGB texture whose width isn't a multiple
    // of 4 (#99). Restored to the 4 default right after.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, uploadW, uploadH, 0, format, GL_UNSIGNED_BYTE, uploadData);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

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

    // Anisotropic filtering — core in GL 4.6. Cleans up textures viewed at a shallow angle
    // (floors, walls receding to the horizon) that trilinear alone leaves blurry. Only
    // meaningful with a mip chain and a linear filter; clamp our request to the driver's max.
    if (settings.GenerateMipmaps && settings.FilterMode != TextureImportSettings::Filter::Point) {
        static GLfloat s_MaxAniso = []() {
            GLint m = 0; glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m);
            return (GLfloat)m;
        }();
        if (s_MaxAniso >= 2.0f)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, s_MaxAniso < 8.0f ? s_MaxAniso : 8.0f);
    }

    if (data) stbi_image_free(data); // null on the cache-hit path, where stb never ran
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
