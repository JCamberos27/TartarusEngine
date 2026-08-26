#include "Texture.h"
#include "gl.h"
#include "stb_image.h"
#include <iostream>

Texture::Texture(const std::string& path) : m_Path(path) {
    // Do NOT flip on load: Model.cpp already applies Assimp's aiProcess_FlipUVs to correct
    // the top-left-origin (FBX/glTF) vs bottom-left-origin (OpenGL) mismatch on the mesh's
    // own UVs. Flipping the image here too double-corrects it, leaving UV islands sampling
    // the wrong region of the texture (mirrored vertically relative to where they should be).
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(path.c_str(), &m_Width, &m_Height, &m_Channels, 0);
    if (!data) {
        std::cerr << "Failed to load texture: " << path << std::endl;
        return;
    }

    GLenum format = GL_RGB;
    if (m_Channels == 1) format = GL_RED;
    else if (m_Channels == 3) format = GL_RGB;
    else if (m_Channels == 4) format = GL_RGBA;

    glGenTextures(1, &m_ID);
    glBindTexture(GL_TEXTURE_2D, m_ID);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, m_Width, m_Height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_image_free(data);
}

Texture::~Texture() {
    if (m_ID) glDeleteTextures(1, &m_ID);
}

void Texture::Bind(unsigned int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_ID);
}
