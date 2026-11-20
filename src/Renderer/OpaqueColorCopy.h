#pragma once

#include "FullscreenPass.h"

// Captures the resolved opaque scene color into a separate RGBA16F texture with a full mip chain
// so the _TRANSMISSION refraction lobe can sample it with roughness-blurred LODs.
// The texture is (re)allocated lazily on the first CopyFrom() call and on resize; the copy is a blit.
class OpaqueColorCopy {
public:
    OpaqueColorCopy() = default;
    ~OpaqueColorCopy();
    OpaqueColorCopy(const OpaqueColorCopy&) = delete;
    OpaqueColorCopy& operator=(const OpaqueColorCopy&) = delete;

    // Copy mip-level 0 from srcTex (RGBA16F, no mips) and generate the mip chain.
    // srcTex must be a GL_TEXTURE_2D; width/height is the resolved image size.
    void CopyFrom(unsigned int srcTex, int width, int height);

    unsigned int Texture() const { return m_Copy.Texture(); }

private:
    ColorTarget  m_Copy;        // RGBA16F, full mip chain, trilinear
    unsigned int m_ReadFbo = 0; // the source attached each call (the caller's texture may be reallocated)
};
