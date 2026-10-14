// A skinned vertex as PreSkin.comp.glsl writes it and the model, depth and outline vertex shaders read it
// (uUseSkinning == 2): mesh-space position (3 floats), then the normal and the tangent as unit vectors packed
// octahedrally into two snorm16 each (angle error well under 0.01 degrees). 20 bytes a vertex.
#pragma once
const uint kSkinnedWords = 5u;

uint PackUnitOct(vec3 n) {
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    if (!(l1 > 1e-20)) return packSnorm2x16(vec2(0.0)); // degenerate (or NaN): decodes to +Z
    n /= l1;
    vec2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(e);
}

vec3 UnpackUnitOct(uint p) {
    vec2 e = unpackSnorm2x16(p);
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
