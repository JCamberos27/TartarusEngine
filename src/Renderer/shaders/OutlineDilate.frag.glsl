#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask; // silhouette mask: 2x the scene's size, 4x MSAA resolved, linear filtered
uniform vec3 uTexel;     // xy = 1.0 / SCENE size: a fetch at a scene pixel centre lands between
                         // four mask texels, so linear filtering returns 16-sample coverage (0..1)
uniform vec3 uColor;
uniform int uRadius;     // outline width, in scene pixels
void main() {
    // Anti-aliased ring. The mask is sampled on circles at sub-pixel offsets rather than on the
    // pixel grid, and its coverage is continuous, so the ring's value varies smoothly with how
    // far this pixel is from the silhouette: no stair-steps on its inner or outer edge.
    float here = texture(uMask, vUV).r;
    if (here > 0.999) discard;               // fully inside the selection: leave the surface alone
    const int kDirections = 16;
    const float kStep = 6.28318530718 / float(kDirections);
    float ring = here;
    for (int ri = 1; ri <= 2; ++ri) {
        float r = float(uRadius) * float(ri) * 0.5;
        for (int i = 0; i < kDirections; ++i) {
            float a = (float(i) + 0.5 * float(ri - 1)) * kStep; // the two circles interleave
            ring = max(ring, texture(uMask, vUV + vec2(cos(a), sin(a)) * r * uTexel.xy).r);
        }
    }
    float alpha = ring * (1.0 - here);       // an edge pixel the mesh partly covers blends in
    if (alpha < 0.004) discard;
    FragColor = vec4(uColor, alpha);
}
