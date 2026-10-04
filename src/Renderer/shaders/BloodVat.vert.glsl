#version 460 core
// Volumetric blood (docs/BLOOD_FX.md): one Houdini fluid sim, played back from its vertex-animation
// texture. The mesh is a triangle soup with no vertex buffer - vertex v reads its own texel, so the
// texel comes from gl_VertexID (BloodFxImport::VatTexel). Shaded by ModelFragment.glsl like any
// other surface (sun + shadows, clustered lights, IBL, fog), with the instance tint as vertex colour.

layout(binding = 16) uniform usampler2D uVatTex; // RGBA16UI: xyz quantised position, w octahedral normal

struct BloodSpray {
    mat4 Model;
    vec4 Tint;      // rgb albedo multiplier, a unused
    vec4 ClipPlane; // world plane; the fluid on its negative side is clipped (an obstacle it met)
    ivec4 Info;     // x = frame, y = first frame-bounds entry of this sim
};
layout(std430, binding = 7) readonly buffer BloodSprays { BloodSpray uSprays[]; };
layout(std430, binding = 8) readonly buffer BloodFrames { vec4 uFrameBounds[]; }; // per frame: min, max

uniform mat4 uView;
uniform mat4 uProj;
uniform int uTrisPerRow;
uniform int uRowsPerFrame;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;
out vec4 vColor;
out float vHidden;
out vec3 vBindPos;    // ModelFragment's blood splats: none on the fluid itself
out vec3 vBindNormal;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[1]; };

vec3 UnpackOct(uint p) {
    vec2 e = vec2(float(p >> 8u), float(p & 255u)) / 255.0 * 2.0 - 1.0;
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main() {
    BloodSpray s = uSprays[gl_BaseInstance + gl_InstanceID];
    int frame = s.Info.x;
    uint v = uint(gl_VertexID);
    uint t = v / 3u;
    ivec2 tc = ivec2(int(3u * (t % uint(uTrisPerRow)) + 2u - v % 3u), frame * uRowsPerFrame + int(t / uint(uTrisPerRow)));
    uvec4 q = texelFetch(uVatTex, tc, 0);
    vec3 lo = uFrameBounds[s.Info.y + 2 * frame].xyz;
    vec3 hi = uFrameBounds[s.Info.y + 2 * frame + 1].xyz;
    vec3 local = lo + vec3(q.xyz) * (1.0 / 65535.0) * (hi - lo);

    vec4 world = s.Model * vec4(local, 1.0);
    vWorldPos = world.xyz;
    // The sims' baked normals point into the fluid (as their corner order winds the other way): out of it here.
    vNormal = -normalize(mat3(s.Model) * UnpackOct(q.w));
    vec3 T = normalize(abs(vNormal.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), vNormal) : cross(vec3(1.0, 0.0, 0.0), vNormal));
    vTBN = mat3(T, cross(vNormal, T), vNormal);
    vUV = vec2(0.0);
    vColor = vec4(s.Tint.rgb, 1.0);
    vHidden = 0.0;
    vBindPos = vec3(0.0);
    vBindNormal = vec3(0.0, 1.0, 0.0);
    gl_ClipDistance[0] = dot(vec4(world.xyz, 1.0), s.ClipPlane);
    gl_Position = uProj * (uView * world);
}
