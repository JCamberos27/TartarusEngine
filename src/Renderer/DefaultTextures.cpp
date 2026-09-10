#include "DefaultTextures.h"
#include "gl.h"

namespace DefaultTextures {
namespace {

// Enums the minimal core loader header doesn't already define — plain GLenum values.
constexpr GLenum kTexture2D            = 0x0DE1; // GL_TEXTURE_2D
constexpr GLenum kTexture2DArray       = 0x8C1A; // GL_TEXTURE_2D_ARRAY
constexpr GLenum kTextureCubeMapArray  = 0x9009; // GL_TEXTURE_CUBE_MAP_ARRAY
constexpr GLenum kTexMagFilter         = 0x2800; // GL_TEXTURE_MAG_FILTER
constexpr GLenum kRGBA                 = 0x1908; // GL_RGBA
constexpr GLenum kUnsignedByte         = 0x1401; // GL_UNSIGNED_BYTE
constexpr GLenum kDepthComponent       = 0x1902; // GL_DEPTH_COMPONENT
constexpr GLenum kFloat                = 0x1406; // GL_FLOAT

unsigned int MakeColor2D(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    GLuint tex = 0;
    glCreateTextures(kTexture2D, 1, &tex);
    glTextureStorage2D(tex, 1, GL_RGBA8, 1, 1);
    const unsigned char px[4] = {r, g, b, a};
    glTextureSubImage2D(tex, 0, 0, 0, 1, 1, kRGBA, kUnsignedByte, px);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(tex, kTexMagFilter, GL_NEAREST);
    return tex;
}

// A depth texture (array or cube-array) with the shadow-sampler comparison contract, one texel,
// depth 1.0 → every comparison passes → "fully lit".
unsigned int MakeDepth(GLenum target, int layers) {
    GLuint tex = 0;
    glCreateTextures(target, 1, &tex);
    glTextureStorage3D(tex, 1, GL_DEPTH_COMPONENT32F, 1, 1, layers);
    const float one = 1.0f;
    for (int layer = 0; layer < layers; ++layer)
        glTextureSubImage3D(tex, 0, 0, 0, layer, 1, 1, 1, kDepthComponent, kFloat, &one);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(tex, kTexMagFilter, GL_NEAREST);
    glTextureParameteri(tex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(tex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    return tex;
}

} // namespace

unsigned int White() {
    static const unsigned int t = MakeColor2D(255, 255, 255, 255);
    return t;
}
unsigned int FlatNormal() {
    static const unsigned int t = MakeColor2D(128, 128, 255, 255);
    return t;
}
unsigned int Black() {
    static const unsigned int t = MakeColor2D(0, 0, 0, 255);
    return t;
}
unsigned int DepthArray() {
    static const unsigned int t = MakeDepth(kTexture2DArray, 1);
    return t;
}
unsigned int DepthCubeArray() {
    static const unsigned int t = MakeDepth(kTextureCubeMapArray, 6);
    return t;
}

} // namespace DefaultTextures
