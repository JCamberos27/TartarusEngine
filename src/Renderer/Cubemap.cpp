#include "Cubemap.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Log.h"
#include "gl.h"
#include "stb_image.h"
#include <glm/glm.hpp>

namespace {

struct FaceBasis { float forward[3]; float right[3]; float up[3]; };
const FaceBasis kFaces[6] = {
    {{ 1,  0,  0}, { 0,  0, -1}, { 0, -1,  0}}, // +X
    {{-1,  0,  0}, { 0,  0,  1}, { 0, -1,  0}}, // -X
    {{ 0,  1,  0}, { 1,  0,  0}, { 0,  0,  1}}, // +Y
    {{ 0, -1,  0}, { 1,  0,  0}, { 0,  0, -1}}, // -Y
    {{ 0,  0,  1}, { 1,  0,  0}, { 0, -1,  0}}, // +Z
    {{ 0,  0, -1}, {-1,  0,  0}, { 0, -1,  0}}, // -Z
};

} // namespace

Cubemap::~Cubemap() {
    if (m_Tex) glDeleteTextures(1, &m_Tex);
}

std::shared_ptr<Cubemap> Cubemap::LoadHdr(const std::string& path, int faceSize, float exposure) {
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".exr") {
        Log::Error("Cubemap: .exr is not supported — convert to .hdr first: " + path);
        return nullptr;
    }

    stbi_set_flip_vertically_on_load(true);
    int w = 0, h = 0, c = 0;
    float* data = stbi_loadf(path.c_str(), &w, &h, &c, 3);
    stbi_set_flip_vertically_on_load(false);
    if (!data) {
        Log::Error("Cubemap: failed to load HDR image '" + path + "': " + stbi_failure_reason());
        return nullptr;
    }

    // Upload equirectangular map as a plain 2D texture.
    unsigned int equiTex = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &equiTex);
    glTextureStorage2D(equiTex, 1, GL_RGB16F, w, h);
    glTextureSubImage2D(equiTex, 0, 0, 0, w, h, GL_RGB, GL_FLOAT, data);
    glTextureParameteri(equiTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(equiTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(equiTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(equiTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(data);

    // Compute mip count for faceSize (floor(log2(faceSize)) + 1).
    int numMips = 1;
    for (int s = faceSize >> 1; s > 0; s >>= 1) ++numMips;

    // Allocate the output cubemap.
    unsigned int cubeTex = 0;
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &cubeTex);
    glTextureStorage2D(cubeTex, numMips, GL_RGB16F, faceSize, faceSize);
    glTextureParameteri(cubeTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(cubeTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(cubeTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(cubeTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(cubeTex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Render equirect -> 6 cube faces using the shared Ibl vertex + equirect fragment shaders.
    Shader cvtShader(ShaderLibrary::ReadFile("Ibl.vert.glsl"),
                     ShaderLibrary::ReadFile("IblEquirect.frag.glsl"));
    unsigned int fbo = 0, vao = 0;
    glCreateFramebuffers(1, &fbo);
    glGenVertexArrays(1, &vao);

    const GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasCull      = glIsEnabled(GL_CULL_FACE);
    const GLboolean wasBlend     = glIsEnabled(GL_BLEND);
    GLint prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, equiTex);
    cvtShader.Bind();
    cvtShader.SetInt("uEquirect", 0);
    cvtShader.SetFloat("uExposure", exposure);

    for (int face = 0; face < 6; ++face) {
        const FaceBasis& fb = kFaces[face];
        cvtShader.SetVec3("uFaceForward", glm::vec3(fb.forward[0], fb.forward[1], fb.forward[2]));
        cvtShader.SetVec3("uFaceRight",   glm::vec3(fb.right[0],   fb.right[1],   fb.right[2]));
        cvtShader.SetVec3("uFaceUp",      glm::vec3(fb.up[0],      fb.up[1],      fb.up[2]));

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0, cubeTex, 0, face);
        const GLenum draw[1] = {GL_COLOR_ATTACHMENT0};
        glNamedFramebufferDrawBuffers(fbo, 1, draw);
        glViewport(0, 0, faceSize, faceSize);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glGenerateTextureMipmap(cubeTex);

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &equiTex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (wasDepthTest) glEnable(GL_DEPTH_TEST);
    if (wasCull)      glEnable(GL_CULL_FACE);
    if (wasBlend)     glEnable(GL_BLEND);
    glDepthMask((GLboolean)prevDepthMask);

    auto cube = std::shared_ptr<Cubemap>(new Cubemap());
    cube->m_Tex      = cubeTex;
    cube->m_FaceSize = faceSize;
    return cube;
}
