#include "Grid.h"
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
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kFragmentSrc = R"(
#version 330 core
in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

uniform vec3 uCameraPos;
uniform float uMinorSpacing;
uniform float uMajorEvery;
uniform float uFadeDistance;

float GridLine(vec2 coord, float scale) {
    vec2 c = coord / scale;
    vec2 deriv = fwidth(c);
    vec2 g = abs(fract(c - 0.5) - 0.5) / max(deriv, vec2(1e-6));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

void main() {
    float t = -vNearPoint.y / (vFarPoint.y - vNearPoint.y);
    if (t <= 0.0) discard; // ground plane is behind the camera along this ray

    vec3 worldPos = vNearPoint + t * (vFarPoint - vNearPoint);

    float minor = GridLine(worldPos.xz, uMinorSpacing);
    float major = GridLine(worldPos.xz, uMinorSpacing * uMajorEvery);

    vec3 color = vec3(0.55);
    float alpha = max(minor * 0.35, major * 0.75);

    float axisWidth = fwidth(worldPos.x) * 1.5 + 0.02;
    // Fade the coloured axis lines out as they approach the world origin: that patch belongs to
    // the transform gizmo, and a full-brightness bar running straight through the manipulator
    // reads as clutter (#42 P25). Also drop the peak alpha so they hint at the axes rather than
    // laser through the scene.
    float originClear = smoothstep(0.4, 2.0, length(worldPos.xz));
    if (abs(worldPos.x) < axisWidth) { color = vec3(0.62, 0.28, 0.28); alpha = max(alpha, 0.55 * originClear); } // Z axis (world X=0)
    if (abs(worldPos.z) < axisWidth) { color = vec3(0.30, 0.42, 0.66); alpha = max(alpha, 0.55 * originClear); } // X axis (world Z=0)

    float dist = length(worldPos.xz - uCameraPos.xz);
    float fade = clamp(1.0 - dist / uFadeDistance, 0.0, 1.0);
    alpha *= fade * fade;

    if (alpha <= 0.003) discard;
    FragColor = vec4(color, alpha);
}
)";

} // namespace

Grid::Grid() {
    m_Shader = std::make_unique<Shader>(kVertexSrc, kFragmentSrc);
    glGenVertexArrays(1, &m_VAO); // no attributes: the vertex shader generates positions from gl_VertexID
}

Grid::~Grid() {
    glDeleteVertexArrays(1, &m_VAO);
}

void Grid::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
                 float minorSpacing, float majorEvery, float fadeDistance) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    m_Shader->Bind();
    m_Shader->SetMat4("uInvViewProj", invViewProj);
    m_Shader->SetVec3("uCameraPos", cameraPos);
    m_Shader->SetFloat("uMinorSpacing", minorSpacing);
    m_Shader->SetFloat("uMajorEvery", majorEvery);
    m_Shader->SetFloat("uFadeDistance", fadeDistance);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
