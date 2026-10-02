#version 460 core
// #177 - one camera-facing quad per particle instance. The quad's corners come from
// gl_VertexID (two triangles, no vertex buffer); position/size/colour are per-instance.
// A flame instance (aFlame.w > 0) is a tongue instead: a stretched billboard (Unity's, as the
// muzzle flash was authored for) standing on the position, aFlame.w metres along the world axis
// aFlame.xyz and turned about that axis to face the camera. Seen down its axis it foreshortens.
layout(location = 0) in vec4 aPosSize; // xyz world position, w world-space diameter (flame: width)
layout(location = 1) in vec4 aColor;   // rgb (already scaled by intensity), a alpha
layout(location = 2) in vec4 aFlame;   // xyz axis, w length (0 = a plain disc)
layout(location = 3) in float aSeed;   // flame variety, 0..1

uniform mat4 uView;
uniform mat4 uProj;

out vec2 vCorner; // -1..1 across the quad
out vec4 vColor;
out float vSeed;
flat out float vIsFlame;

const vec2 kCorners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));

void main() {
    vec2 c = kCorners[gl_VertexID];
    vec4 viewPos = uView * vec4(aPosSize.xyz, 1.0);
    if (aFlame.w > 0.0) {
        vec3 axis = normalize(mat3(uView) * aFlame.xyz);
        vec3 side = cross(axis, -viewPos.xyz); // across the tongue, square to the eye
        if (dot(side, side) < 1e-10) side = cross(axis, vec3(0.0, 0.0, 1.0));
        side = normalize(side);
        // The pivot (0.48 of the length) sets the base just behind the position.
        float along = (c.y * 0.5 + 0.5 - 0.02) * aFlame.w;
        viewPos.xyz += axis * along + side * (c.x * 0.5 * aPosSize.w);
    } else {
        viewPos.xy += c * (aPosSize.w * 0.5); // expand in view space = always faces the camera
    }
    gl_Position = uProj * viewPos;
    vCorner = c;
    vColor = aColor;
    vSeed = aSeed;
    vIsFlame = aFlame.w > 0.0 ? 1.0 : 0.0;
}
