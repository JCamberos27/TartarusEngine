#pragma once
#include <string>

// A single 2D GL texture loaded from disk via stb_image (PNG/JPG/TGA/BMP/...).
class Texture {
public:
    explicit Texture(const std::string& path);
    ~Texture();

    void Bind(unsigned int unit = 0) const;

    int Width() const { return m_Width; }
    int Height() const { return m_Height; }
    bool IsValid() const { return m_ID != 0; }
    const std::string& Path() const { return m_Path; }
    unsigned int GLHandle() const { return m_ID; } // for ImGui::Image thumbnails in the Asset Browser

private:
    unsigned int m_ID = 0;
    int m_Width = 0, m_Height = 0, m_Channels = 0;
    std::string m_Path;
};
