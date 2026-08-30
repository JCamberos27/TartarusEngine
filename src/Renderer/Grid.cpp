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
    // The coloured axis lines are a local orientation hint, not scene furniture. Fade them out
    // right at the origin (that patch belongs to the transform gizmo — a bar through the
    // manipulator reads as clutter, #42 P25) AND fade them out again past a short radius, so
    // from an empty Front view they no longer laser across the whole viewport (audit #86).
    float r = length(worldPos.xz);
    float gizmoClear = smoothstep(0.4, 2.0, r);            // 0 at origin, 1 past ~2 units
    float shortRange = 1.0 - smoothstep(6.0, 20.0, r);     // 1 up to ~6 units, 0 past ~20
    float axisVis = 0.42 * gizmoClear * shortRange;
    if (abs(worldPos.x) < axisWidth) { color = vec3(0.58, 0.30, 0.30); alpha = max(alpha, axisVis); } // Z axis (world X=0)
    if (abs(worldPos.z) < axisWidth) { color = vec3(0.32, 0.42, 0.60); alpha = max(alpha, axisVis); } // X axis (world Z=0)

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
