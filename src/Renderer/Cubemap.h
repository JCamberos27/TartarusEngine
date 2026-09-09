#pragma once
#include <string>
#include <memory>

// A GL_TEXTURE_CUBE_MAP loaded from a .hdr equirectangular image via stbi_loadf.
// Non-copyable; create via LoadHdr(). The GL texture has a full mip chain so it can be
// used directly as the env map source for IblProbe::BakeFromCubemap() (prefilter sampling).
class Cubemap {
public:
    ~Cubemap();
    Cubemap(const Cubemap&) = delete;
    Cubemap& operator=(const Cubemap&) = delete;

    // Loads a .hdr equirectangular panorama and converts it to a cubemap with a mip chain.
    // faceSize: output cube face resolution. exposure: linear scale applied to HDR values.
    // Returns nullptr on I/O failure, unsupported format, or GL error.
    // .exr is explicitly unsupported — the function logs a clear error and returns nullptr.
    static std::shared_ptr<Cubemap> LoadHdr(const std::string& path,
                                            int faceSize = 512,
                                            float exposure = 1.0f);

    unsigned int Texture()  const { return m_Tex;      }
    int          FaceSize() const { return m_FaceSize;  }

private:
    Cubemap() = default;
    unsigned int m_Tex      = 0;
    int          m_FaceSize = 0;
};
