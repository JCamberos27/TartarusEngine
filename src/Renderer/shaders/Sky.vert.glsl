#version 460 core
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
    // Exactly on the far plane (window depth 1.0, inside the inclusive clip volume): drawn after the
    // opaque pass under GL_LEQUAL, only pixels no geometry covered pass the cleared depth of 1.0.
    gl_Position = vec4(p, 1.0, 1.0);
}
