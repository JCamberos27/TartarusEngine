#version 460 core
// Flipbook particles: one quad per instance from gl_VertexID, no vertex buffer.
// A sprite faces the camera (turned by its rotation), or stands along a world axis - its velocity, a
// barrel - turned about it to face the eye, the texture's u along the axis.

struct FxSprite {
    vec4 PosSize;  // xyz world, w height
    vec4 AxisRot;  // xyz axis (|axis| = length / height; 0 = camera-facing), w rotation
    vec4 Color;    // rgb linear, a opacity
    vec4 Params;   // x erosion, y softness, z aspect (w / h), w cell blend
    ivec4 Tex;     // x colour layer (-1 procedural), y normal layer (-1 none), z cell a, w cell b
    ivec4 Info;    // x cols, y rows, z mode (0 blood, 1 lit, 2 additive), w entry flags
    vec4 Extra;    // x smoothness, y seed
};
layout(std430, binding = 10) readonly buffer FxSprites { FxSprite uSprites[]; };

uniform mat4 uView;
uniform mat4 uProj;

out vec2 vUV;          // 0..1 across the quad, v up
out vec3 vWorld;
out vec3 vRight;       // world: the quad's u and v directions, and towards the eye
out vec3 vUp;
out vec3 vFacing;
out float vViewDepth;
flat out int vSprite;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                 vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    vSprite = gl_BaseInstance + gl_InstanceID;
    FxSprite s = uSprites[vSprite];
    vec2 c = kCorners[gl_VertexID];
    vec3 viewPos = (uView * vec4(s.PosSize.xyz, 1.0)).xyz;
    float h = s.PosSize.w;
    vec3 right, up; // view space, scaled to the half-extents
    float axisLen = length(s.AxisRot.xyz);
    if (axisLen > 1e-4) {
        vec3 axis = mat3(uView) * (s.AxisRot.xyz / axisLen);
        vec3 side = cross(axis, -viewPos);
        if (dot(side, side) < 1e-10) side = cross(axis, vec3(0.0, 0.0, 1.0));
        right = axis * (0.5 * h * axisLen);
        up = normalize(side) * (0.5 * h);
    } else {
        float cr = cos(s.AxisRot.w), sr = sin(s.AxisRot.w);
        right = vec3(cr, sr, 0.0) * (0.5 * h * s.Params.z);
        up = vec3(-sr, cr, 0.0) * (0.5 * h);
    }
    viewPos += right * c.x + up * c.y;
    gl_Position = uProj * vec4(viewPos, 1.0);
    mat3 toWorld = transpose(mat3(uView)); // the view's rotation is orthonormal
    vWorld = (inverse(uView) * vec4(viewPos, 1.0)).xyz;
    vRight = normalize(toWorld * right);
    vUp = normalize(toWorld * up);
    vFacing = normalize(cross(vRight, vUp));
    vUV = c * 0.5 + 0.5;
    vViewDepth = -viewPos.z;
}
