#include "Texture.h"
#include "Log.h"
#include "gl.h"
#include "GLStateCache.h"
#include "TextureCache.h"
#include "stb_image.h"
#include "stb_dxt.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

TextureLoadStats& TextureLoadStats::Get() {
    static TextureLoadStats s_Stats;
    return s_Stats;
}

namespace {

// Box-filter downsample to fit within maxSize x maxSize, preserving aspect ratio. No new
// dependency (a Lanczos filter would need stb_image_resize, not vendored here) — averages every
// source texel whose footprint falls under each destination texel, which is a large quality win
// over a point sample for the common case of a 4K source getting capped to 2048 or lower (#207).
// sRGB <-> linear for 8-bit values, via a 256-entry table one way and a direct formula back.
float SrgbToLinear(unsigned char v) {
    static const std::array<float, 256> kTable = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return kTable[v];
}
unsigned char LinearToSrgb(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return (unsigned char)std::lround(c * 255.0f);
}

// #156 - averages colour in LINEAR space for an sRGB texture (averaging the encoded bytes made
// downscaled textures too dark and banded), and weights colour by alpha when there is an alpha
// channel so fully transparent texels don't bleed their (often black) colour into the edges.
std::vector<unsigned char> ResizeBox(const unsigned char* src, int srcW, int srcH, int channels,
    int outW, int outH, bool srgb) {
    std::vector<unsigned char> dst((size_t)outW * outH * channels);
    for (int y = 0; y < outH; ++y) {
        // Source row range covering this destination texel's footprint.
        int sy0 = (int)((float)y * srcH / outH);
        int sy1 = std::max(sy0 + 1, (int)((float)(y + 1) * srcH / outH));
        sy1 = std::min(sy1, srcH);
        for (int x = 0; x < outW; ++x) {
            int sx0 = (int)((float)x * srcW / outW);
            int sx1 = std::max(sx0 + 1, (int)((float)(x + 1) * srcW / outW));
            sx1 = std::min(sx1, srcW);

            unsigned char* d = dst.data() + ((size_t)y * outW + x) * channels;
            const int sampleCount = (sy1 - sy0) * (sx1 - sx0);
            // Channel layout: 4 = RGBA, 2 = grey+alpha, else no alpha.
            const int alphaCh = channels == 4 ? 3 : (channels == 2 ? 1 : -1);
            const int colorChannels = alphaCh >= 0 ? channels - 1 : channels;
            float colorSum[3] = {0.0f, 0.0f, 0.0f};
            float alphaSum = 0.0f;
            for (int sy = sy0; sy < sy1; ++sy) {
                const unsigned char* px = src + ((size_t)sy * srcW + sx0) * channels;
                for (int sx = sx0; sx < sx1; ++sx, px += channels) {
                    const float a = alphaCh >= 0 ? px[alphaCh] / 255.0f : 1.0f;
                    alphaSum += a;
                    for (int c = 0; c < colorChannels; ++c) {
                        const float v = srgb ? SrgbToLinear(px[c]) : px[c] / 255.0f;
                        colorSum[c] += v * a;
                    }
                }
            }
            for (int c = 0; c < colorChannels; ++c) {
                // Alpha-weighted mean; an all-transparent footprint falls back to 0.
                const float lin = alphaSum > 0.0f ? colorSum[c] / alphaSum : 0.0f;
                d[c] = srgb ? LinearToSrgb(lin) : (unsigned char)std::lround(std::clamp(lin, 0.0f, 1.0f) * 255.0f);
            }
            if (alphaCh >= 0) d[alphaCh] = (unsigned char)std::lround(alphaSum / sampleCount * 255.0f);
        }
    }
    return dst;
}

std::vector<unsigned char> DownsampleBox(const unsigned char* src, int srcW, int srcH, int channels,
    int maxSize, int& outW, int& outH, bool srgb) {
    float scale = std::min((float)maxSize / srcW, (float)maxSize / srcH);
    outW = std::max(1, (int)(srcW * scale));
    outH = std::max(1, (int)(srcH * scale));
    return ResizeBox(src, srcW, srcH, channels, outW, outH, srgb);
}

// #156 - S3TC (BC1/BC3) is an extension; RGTC (BC4/BC5) is core. Every desktop driver ships it,
// but check rather than upload a format the driver would reject.
bool HasS3tc() {
    static const bool s_Has = [] {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && std::strcmp(e, "GL_EXT_texture_compression_s3tc") == 0) return true;
        }
        return false;
    }();
    return s_Has;
}

const char* FormatName(GLenum f) {
    switch (f) {
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:        return "BC1";
        case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:       return "sRGB BC1";
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:       return "BC3";
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return "sRGB BC3";
        case GL_COMPRESSED_RED_RGTC1:                return "BC4";
        case GL_COMPRESSED_RG_RGTC2:                 return "BC5";
        case GL_SRGB8_ALPHA8: return "sRGB RGBA8";
        case GL_SRGB8:        return "sRGB RGB8";
        case GL_RGBA8:        return "RGBA8";
        case GL_RGB8:         return "RGB8";
        case GL_RG8:          return "RG8";
        case GL_R8:           return "R8";
        default:              return "?";
    }
}

// #156 - encodes `px` (w x h, `channels` bytes per texel, stb layout: 1 grey, 2 grey+alpha,
// 3 RGB, 4 RGBA) to BCn, with a CPU-built mip chain when `mips` is set: glGenerateMipmap can't
// run on a compressed texture, so each level is box-filtered from the one above (linear-space
// for sRGB, alpha-weighted) and then encoded. Returns false, leaving `out` untouched, when the
// format needs S3TC and the driver lacks it.
bool EncodeBlockCompressed(const unsigned char* px, int w, int h, int channels, bool srgb, bool mips,
                           bool highQuality, TextureCache::Image& out) {
    // Working layout per format: 4 = RGBA for BC1/BC3, 1 = R for BC4, 2 = RG for BC5. A colour
    // (sRGB) grey image has no 1/2-channel sRGB BC format, so it's expanded to RGBA like the
    // uncompressed path does.
    const bool dxt = srgb || channels >= 3;
    if (dxt && !HasS3tc()) return false;
    const int wc = dxt ? 4 : channels;
    const size_t texels = (size_t)w * h;

    std::vector<unsigned char> level(texels * wc);
    bool opaque = true;
    for (size_t i = 0; i < texels; ++i) {
        const unsigned char* s = px + i * channels;
        unsigned char* d = level.data() + i * wc;
        if (!dxt) { for (int c = 0; c < wc; ++c) d[c] = s[c]; continue; }
        const bool grey = channels <= 2;
        d[0] = s[0];
        d[1] = grey ? s[0] : s[1];
        d[2] = grey ? s[0] : s[2];
        d[3] = channels == 4 ? s[3] : channels == 2 ? s[1] : 255;
        opaque &= d[3] == 255;
    }

    GLenum format;
    int blockBytes;
    if (!dxt) {
        format = wc == 1 ? GL_COMPRESSED_RED_RGTC1 : GL_COMPRESSED_RG_RGTC2;
        blockBytes = wc == 1 ? 8 : 16;
    } else if (opaque) {
        format = srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        blockBytes = 8;
    } else {
        format = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        blockBytes = 16;
    }

    out.GLFormat = format;
    out.LevelSizes.clear();
    out.Pixels.clear();
    const int mode = highQuality ? STB_DXT_HIGHQUAL : STB_DXT_NORMAL;
    int lw = w, lh = h;
    for (;;) {
        const int bx = (lw + 3) / 4, by = (lh + 3) / 4;
        const size_t start = out.Pixels.size();
        out.Pixels.resize(start + (size_t)bx * by * blockBytes);
        unsigned char* dst = out.Pixels.data() + start;
        unsigned char block[16 * 4];
        for (int y = 0; y < by; ++y) {
            for (int x = 0; x < bx; ++x, dst += blockBytes) {
                // Gather 4x4 texels, clamping at the edge so partial blocks repeat the border.
                for (int j = 0; j < 4; ++j) {
                    const int sy = std::min(y * 4 + j, lh - 1);
                    for (int i = 0; i < 4; ++i) {
                        const int sx = std::min(x * 4 + i, lw - 1);
                        std::memcpy(block + (j * 4 + i) * wc, level.data() + ((size_t)sy * lw + sx) * wc, (size_t)wc);
                    }
                }
                if (wc == 1)      stb_compress_bc4_block(dst, block);
                else if (wc == 2) stb_compress_bc5_block(dst, block);
                else              stb_compress_dxt_block(dst, block, blockBytes == 16 ? 1 : 0, mode);
            }
        }
        out.LevelSizes.push_back((uint32_t)(out.Pixels.size() - start));
        if (!mips || (lw == 1 && lh == 1)) break;
        const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        // Same filter as the Max Size downsample (2 channels = grey+alpha, as stb decodes it).
        level = ResizeBox(level.data(), lw, lh, wc, nw, nh, srgb);
        lw = nw; lh = nh;
    }
    return true;
}

} // namespace

Texture::Texture(const std::string& path) : Texture(path, TextureImportSettings{}) {}

Texture::Texture(const std::string& path, const TextureImportSettings& settings)
    : m_Path(path), m_Settings(settings) {
    UploadFromFile(settings);
}

Texture::Texture(const std::string& name, std::vector<unsigned char> bytes, int rawWidth, int rawHeight,
                 const TextureImportSettings& settings)
    : m_Memory(std::move(bytes)), m_MemRawW(rawWidth), m_MemRawH(rawHeight), m_Path(name), m_Settings(settings) {
    UploadFromFile(settings);
}

void Texture::UploadFromFile(const TextureImportSettings& settings) {
    // TextureType now actually drives behavior (#198), authoritative regardless of the individual
    // toggles — so a texture typed NormalMap can never upload as sRGB (the Inspector already
    // flips the checkbox when you pick the type, but this covers scenes saved before that existed
    // too), and Sprite2D gets the clamp-to-edge/no-mipmap treatment a sprite atlas wants by
    // default. Only affects GL upload parameters below, not the decoded pixels, so the cache
    // lookups a few lines down stay keyed on the original `settings`.
    TextureImportSettings effective = settings;
    if (effective.TextureType == TextureImportSettings::Type::NormalMap) {
        effective.IsSRGB = false;
    } else if (effective.TextureType == TextureImportSettings::Type::Sprite2D) {
        effective.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
        effective.GenerateMipmaps = false;
    }

    // ARCH-201 / #375: cumulative decode-vs-upload timing, read by --asset-load-bench and anything
    // else that wants to know where scene-load time actually goes. decodeStart covers both the
    // TextureCache hit path and the stb_image + DownsampleBox miss path below; uploadStart begins
    // right after (see the matching TextureLoadStats::Get() update after the GL block).
    const auto decodeStart = std::chrono::steady_clock::now();

    // Warm path: pixels already decoded and downsampled by a previous run. PNG decode dominates
    // scene-load time (measured ~4.6s of a ~5.6s cold boot on a 22-texture library), so skipping
    // it is the single biggest startup win available. See TextureCache.h.
    TextureCache::Image cached;
    const bool fromMemory = !m_Memory.empty(); // #113 — embedded: no file, no disk cache
    bool fromCache = !fromMemory && TextureCache::Load(m_Path, settings, cached);

    // Owns the decoded pixels only on a cache miss; `uploadData` points into either this, the
    // cache entry, or stb's buffer.
    unsigned char* data = nullptr;
    std::vector<unsigned char> resized;
    const unsigned char* uploadData = nullptr;
    int uploadW = 0, uploadH = 0;
    // #156 - set when the pixels are BCn blocks (from the cache, or encoded below).
    const TextureCache::Image* compressed = nullptr;
    TextureCache::Image encoded;

    if (fromCache) {
        if (cached.GLFormat != 0) compressed = &cached;
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
        if (fromMemory && m_MemRawW > 0) {
            // Raw RGBA8 from the model file. malloc'd so the stbi_image_free below owns it too.
            const size_t n = (size_t)m_MemRawW * m_MemRawH * 4;
            if (m_Memory.size() >= n && (data = (unsigned char*)malloc(n)) != nullptr) {
                std::memcpy(data, m_Memory.data(), n);
                m_Width = m_MemRawW; m_Height = m_MemRawH; m_Channels = 4;
            }
        } else if (fromMemory) {
            data = stbi_load_from_memory(m_Memory.data(), (int)m_Memory.size(), &m_Width, &m_Height, &m_Channels, 0);
        } else {
            data = stbi_load(m_Path.c_str(), &m_Width, &m_Height, &m_Channels, 0);
        }
        if (!data) {
            Log::Error("Texture: failed to load '" + m_Path + "'.", LogContext::Asset(m_Path));
            return;
        }

        uploadData = data;
        uploadW = m_Width;
        uploadH = m_Height;
        if (settings.MaxTextureSize > 0 && (m_Width > settings.MaxTextureSize || m_Height > settings.MaxTextureSize)) {
            resized = DownsampleBox(data, m_Width, m_Height, m_Channels, settings.MaxTextureSize, uploadW, uploadH,
                                    effective.IsSRGB);
            uploadData = resized.data();
        }

        // #156 - block-compress on import. Falls back to the raw upload if the format isn't
        // available (no S3TC), which is then what gets cached under these settings.
        if (settings.CompressionMode != TextureImportSettings::Compression::None &&
            EncodeBlockCompressed(uploadData, uploadW, uploadH, m_Channels, effective.IsSRGB,
                                  effective.GenerateMipmaps,
                                  settings.CompressionMode == TextureImportSettings::Compression::HighQuality,
                                  encoded)) {
            compressed = &encoded;
        }

        // Bake the result for next time — post-downsample (and post-encode), so the cache
        // stores exactly the bytes the upload below receives.
        TextureCache::Image entry;
        entry.SourceWidth = m_Width;
        entry.SourceHeight = m_Height;
        entry.Width = uploadW;
        entry.Height = uploadH;
        entry.Channels = m_Channels;
        if (!fromMemory) {
            if (compressed) {
                entry.GLFormat = encoded.GLFormat;
                entry.LevelSizes = encoded.LevelSizes;
                entry.Pixels = encoded.Pixels;
            } else {
                entry.Pixels.assign(uploadData, uploadData + (size_t)uploadW * uploadH * m_Channels);
            }
            TextureCache::Store(m_Path, settings, entry);
        }
    }

    const auto uploadStart = std::chrono::steady_clock::now();

    // Sized internal formats on both branches (#100) — the sRGB branch already used them;
    // the linear branch used to pass bare GL_RGB/GL_RGBA/GL_RED, leaving precision to the driver
    // and blocking immutable-storage attachment.
    GLenum format = GL_RGB;
    GLint internalFormat = GL_RGB8;
    // #94 — grey (1-channel) and grey+alpha (2-channel) images. 2-channel used to fall through
    // to GL_RGB, so glTextureSubImage2D read w*h*3 bytes out of a w*h*2 buffer (heap over-read,
    // garbled texture); 1-channel uploaded as plain GL_R8 and a greyscale albedo rendered RED.
    // Colour (sRGB) ones are expanded to RGBA so they get sRGB decoding like any other colour
    // texture (core GL has no 1/2-channel sRGB formats); data ones stay compact and are
    // swizzled so every channel a shader reads sees the grey value (and G as alpha).
    std::vector<unsigned char> expanded;
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    bool useSwizzle = false;
    if ((m_Channels == 1 || m_Channels == 2) && effective.IsSRGB && !compressed) { // BCn: expanded at encode
        const size_t px = (size_t)uploadW * uploadH;
        expanded.resize(px * 4);
        for (size_t i = 0; i < px; ++i) {
            const unsigned char g = uploadData[i * m_Channels];
            expanded[i * 4 + 0] = g;
            expanded[i * 4 + 1] = g;
            expanded[i * 4 + 2] = g;
            expanded[i * 4 + 3] = m_Channels == 2 ? uploadData[i * 2 + 1] : 255;
        }
        uploadData = expanded.data();
        format = GL_RGBA;
        internalFormat = GL_SRGB8_ALPHA8;
    } else if (m_Channels == 1) {
        format = GL_RED;
        internalFormat = GL_R8;
        swizzle[1] = swizzle[2] = GL_RED; swizzle[3] = GL_ONE;
        useSwizzle = true;
    } else if (m_Channels == 2) {
        format = GL_RG;
        internalFormat = GL_RG8;
        swizzle[1] = swizzle[2] = GL_RED; swizzle[3] = GL_GREEN;
        useSwizzle = true;
    } else if (m_Channels == 3) {
        format = GL_RGB;
        internalFormat = effective.IsSRGB ? GL_SRGB8 : GL_RGB8;
    } else if (m_Channels == 4) {
        format = GL_RGBA;
        internalFormat = effective.IsSRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    }

    // Immutable storage + Direct State Access (#96): no glBindTexture to set this texture up,
    // so importing a texture mid-frame can't disturb whatever's bound for rendering. Immutable
    // storage needs the full mip level count up front.
    int levels = 1;
    if (effective.GenerateMipmaps) {
        int longEdge = uploadW > uploadH ? uploadW : uploadH;
        while (longEdge > 1) { longEdge >>= 1; ++levels; }
    }

    glCreateTextures(GL_TEXTURE_2D, 1, &m_ID);
    if (compressed) {
        // #156 - BCn: every mip level was built and encoded on the CPU; upload them as-is.
        // Grey sRGB images were expanded to RGBA before encoding (and so aren't swizzled).
        const GLenum cf = (GLenum)compressed->GLFormat;
        levels = (int)compressed->LevelSizes.size();
        glTextureStorage2D(m_ID, levels, cf, uploadW, uploadH);
        const unsigned char* blocks = compressed->Pixels.data();
        m_GpuBytes = 0;
        for (int l = 0; l < levels; ++l) {
            const int lw = std::max(1, uploadW >> l), lh = std::max(1, uploadH >> l);
            const GLsizei size = (GLsizei)compressed->LevelSizes[(size_t)l];
            glCompressedTextureSubImage2D(m_ID, l, 0, 0, lw, lh, cf, size, blocks);
            blocks += size;
            m_GpuBytes += (size_t)size;
        }
        m_GpuFormatName = FormatName(cf);
    } else {
        glTextureStorage2D(m_ID, levels, (GLenum)internalFormat, uploadW, uploadH);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        // The decode/cache path hands us tightly packed rows (stride = w*channels). Without this,
        // GL assumes 4-byte row alignment and shears any RGB texture whose width isn't a multiple
        // of 4 (#99). Restored to the 4 default right after.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(m_ID, 0, 0, 0, uploadW, uploadH, format, GL_UNSIGNED_BYTE, uploadData);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        // Bytes per texel as the driver stores it (RGB8 is padded to 4 on every desktop GPU);
        // a full mip chain adds a third.
        const int bpp = (internalFormat == GL_R8) ? 1 : (internalFormat == GL_RG8) ? 2 : 4;
        m_GpuBytes = (size_t)uploadW * uploadH * bpp;
        if (levels > 1) m_GpuBytes += m_GpuBytes / 3;
        m_GpuFormatName = FormatName((GLenum)internalFormat);
    }

    if (useSwizzle) { // #94
        glTextureParameteri(m_ID, GL_TEXTURE_SWIZZLE_R, swizzle[0]);
        glTextureParameteri(m_ID, GL_TEXTURE_SWIZZLE_G, swizzle[1]);
        glTextureParameteri(m_ID, GL_TEXTURE_SWIZZLE_B, swizzle[2]);
        glTextureParameteri(m_ID, GL_TEXTURE_SWIZZLE_A, swizzle[3]);
    }

    if (effective.GenerateMipmaps && !compressed) glGenerateTextureMipmap(m_ID);

    GLint wrap = effective.WrapMode == TextureImportSettings::Wrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTextureParameteri(m_ID, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(m_ID, GL_TEXTURE_WRAP_T, wrap);

    GLint minFilter, magFilter;
    switch (effective.FilterMode) {
        case TextureImportSettings::Filter::Point:
            minFilter = effective.GenerateMipmaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
            magFilter = GL_NEAREST;
            break;
        case TextureImportSettings::Filter::Trilinear:
            minFilter = effective.GenerateMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
            magFilter = GL_LINEAR;
            break;
        case TextureImportSettings::Filter::Bilinear:
        default:
            // "Bilinear" in Unity's sense still mip-selects, it just doesn't blend BETWEEN mip
            // levels the way Trilinear does - GL_LINEAR_MIPMAP_NEAREST is the matching mode.
            minFilter = effective.GenerateMipmaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
            magFilter = GL_LINEAR;
            break;
    }
    glTextureParameteri(m_ID, GL_TEXTURE_MIN_FILTER, minFilter);
    glTextureParameteri(m_ID, GL_TEXTURE_MAG_FILTER, magFilter);

    // Anisotropic filtering — core in GL 4.6. Cleans up textures viewed at a shallow angle
    // (floors, walls receding to the horizon) that trilinear alone leaves blurry. Only
    // meaningful with a mip chain and a linear filter; clamp our request to the driver's max.
    if (effective.GenerateMipmaps && effective.FilterMode != TextureImportSettings::Filter::Point) {
        static GLfloat s_MaxAniso = []() {
            GLint m = 0; glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m);
            return (GLfloat)m;
        }();
        // #156 - per-texture Aniso Level (was a hard-coded 8x).
        const GLfloat want = (GLfloat)std::clamp(effective.AnisoLevel, 1, 16);
        if (s_MaxAniso >= 2.0f && want > 1.0f) {
            GLfloat aniso = want < s_MaxAniso ? want : s_MaxAniso;
            glTextureParameterfv(m_ID, GL_TEXTURE_MAX_ANISOTROPY, &aniso);
        }
    }

    if (data) stbi_image_free(data); // null on the cache-hit path, where stb never ran
    m_Settings = settings;

    const auto uploadEnd = std::chrono::steady_clock::now();
    TextureLoadStats& stats = TextureLoadStats::Get();
    stats.Count += 1;
    stats.DecodeMs += std::chrono::duration<double, std::milli>(uploadStart - decodeStart).count();
    stats.UploadMs += std::chrono::duration<double, std::milli>(uploadEnd - uploadStart).count();
}

bool Texture::Reimport(const TextureImportSettings& settings) {
    int w, h, c;
    if (m_Memory.empty() && !stbi_info(m_Path.c_str(), &w, &h, &c)) {
        Log::Error("Texture: cannot reimport '" + m_Path + "' - file is missing or unreadable.", LogContext::Asset(m_Path));
        return false;
    }

    // #156 - build the new texture first and only then drop the old one. stbi_info succeeding
    // doesn't mean the full decode will (truncated data, out of memory); deleting first left
    // this Texture at GL name 0, rendering black, which the comment above used to promise
    // couldn't happen.
    const unsigned int previous = m_ID;
    const int prevW = m_Width, prevH = m_Height, prevC = m_Channels;
    m_ID = 0;
    UploadFromFile(settings);
    if (m_ID == 0) {
        m_ID = previous;
        m_Width = prevW; m_Height = prevH; m_Channels = prevC;
        Log::Error("Texture: reimport of '" + m_Path + "' failed - kept the previous version.", LogContext::Asset(m_Path));
        return false;
    }
    if (previous) glDeleteTextures(1, &previous);
    return true;
}

Texture::~Texture() {
    if (m_ID) glDeleteTextures(1, &m_ID);
}

void Texture::Bind(unsigned int unit) const {
    GLStateCache::BindTexture2D(unit, m_ID);
}
