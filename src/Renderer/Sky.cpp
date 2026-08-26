#include "Sky.h"
#include "Shader.h"
#include "gl.h"

namespace {

const char* kVertexSrc = R"(
#version 330 core
out vec3 vNearPoint;
out vec3 vFarPoint;

uniform mat4 uInvViewProj;

const vec2 kQuad[6] = vec2[](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);

vec3 UnprojectPoint(float x, float y, float z) {
    vec4 p = uInvViewProj * vec4(x, y, z, 1.0);
    return p.xyz / p.w;
}

void main() {
    vec2 p = kQuad[gl_VertexID];
    vNearPoint = UnprojectPoint(p.x, p.y, -1.0);
    vFarPoint = UnprojectPoint(p.x, p.y, 1.0);
    gl_Position = vec4(p, 0.9999, 1.0); // just inside the far plane so it never clips
}
)";

const char* kFragmentSrc = R"(
#version 330 core
in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

uniform vec3 uHorizonColor;
uniform vec3 uZenithColor;

void main() {
    vec3 dir = normalize(vFarPoint - vNearPoint);
    float t = clamp(dir.y, 0.0, 1.0);
    vec3 color = mix(uHorizonColor, uZenithColor, pow(t, 0.5));
    FragColor = vec4(color, 1.0);
}
)";

} // namespace

Sky::Sky() {
    m_Shader = std::make_unique<Shader>(kVertexSrc, kFragmentSrc);
    glGenVertexArrays(1, &m_VAO); // no attributes: the vertex shader generates positions from gl_VertexID
}

Sky::~Sky() {
    glDeleteVertexArrays(1, &m_VAO);
}

void Sky::Draw(const glm::mat4& view, const glm::mat4& proj,
               const glm::vec3& horizonColor, const glm::vec3& zenithColor) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    m_Shader->Bind();
    m_Shader->SetMat4("uInvViewProj", invViewProj);
    m_Shader->SetVec3("uHorizonColor", horizonColor);
    m_Shader->SetVec3("uZenithColor", zenithColor);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}
