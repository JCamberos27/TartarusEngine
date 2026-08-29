#include "TintOverlayRenderer.h"
#include "Model.h"
#include "Shader.h"
#include "gl.h"

namespace {

// Only position needs to transform correctly - unused attributes (normal/UV/tangent/bones) in
// the same VAO are simply never read by this shader, which is fine; Model::Draw()'s material/
// bone uniform uploads become harmless no-ops against a shader that never declares those names.
const char* kOverlayVertexSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
void main() {
    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);
}
)";

const char* kOverlayFragmentSrc = R"(
#version 330 core
out vec4 FragColor;
uniform vec3 uTintColor;
uniform float uAlpha;
void main() {
    FragColor = vec4(uTintColor, uAlpha);
}
)";

} // namespace

TintOverlayRenderer::TintOverlayRenderer() = default;
TintOverlayRenderer::~TintOverlayRenderer() = default;

void TintOverlayRenderer::Render(Model& model, const glm::mat4& modelMatrix, const glm::mat4& view,
    const glm::mat4& proj, const glm::vec3& tintColor, float alpha) {
    if (!m_Shader) m_Shader = std::make_unique<Shader>(kOverlayVertexSrc, kOverlayFragmentSrc);

    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLboolean prevPolyOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLint prevDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE); // never writes depth itself - can't corrupt what draws after it

    // GL_LEQUAL alone isn't reliable for "redraw exactly on top of itself": the two draws
    // (the real material pass, then this overlay) don't always round to bit-identical depth
    // values - close enough to usually pass, but which of the two "wins" at a given pixel can
    // flip as the camera moves, which is exactly what read as flickering. A small negative
    // polygon offset nudges this draw's depth reliably closer to the camera instead of relying
    // on exact equality, the standard fix for coplanar/overlay z-fighting (decals, outlines).
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    m_Shader->Bind();
    m_Shader->SetMat4("uModel", modelMatrix);
    m_Shader->SetMat4("uView", view);
    m_Shader->SetMat4("uProj", proj);
    m_Shader->SetVec3("uTintColor", tintColor);
    m_Shader->SetFloat("uAlpha", alpha);

    model.Draw(*m_Shader);

    if (!prevPolyOffset) glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDepthFunc((GLenum)prevDepthFunc);
    if (!prevBlend) glDisable(GL_BLEND);
}
