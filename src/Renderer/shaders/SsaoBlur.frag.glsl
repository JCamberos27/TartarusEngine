#version 460 core
in vec2 vUV;
out float FragColor;

// Ssao::Compute's half-resolution occlusion (R) and each texel's view-space z (G).
uniform sampler2D uSsao;
// The full-resolution depth pre-pass, and the projection it was drawn with.
uniform sampler2D uDepth;
uniform mat4 uProjection;

float ViewZ(float depth) {
    float ndc = depth * 2.0 - 1.0;
    return uProjection[3][3] > 0.5 ? (ndc - uProjection[3][2]) / uProjection[2][2]
                                   : -uProjection[3][2] / (ndc + uProjection[2][2]);
}

// Full resolution: a 4x4 box over the half-resolution occlusion around this pixel (the noise tile's
// size, so its pattern averages out), each texel weighed by how close its surface's depth is to this
// pixel's - the upsample keeps occlusion from bleeding across silhouettes.
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uDepth, p, 0).r;
    if (depth >= 0.9999) { FragColor = 1.0; return; } // sky: nothing reads it
    float z = ViewZ(depth);
    ivec2 halfSize = textureSize(uSsao, 0);
    ivec2 h0 = ((p - 1) >> 1) - 1;
    // Relative depth tolerance: a few percent of the distance (surfaces that slope away still blend).
    float tolerance = max(abs(z) * 0.04, 0.02);
    float sum = 0.0, weights = 0.0, nearest = 1.0, nearestGap = 1e9;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            vec2 s = texelFetch(uSsao, clamp(h0 + ivec2(x, y), ivec2(0), halfSize - 1), 0).rg;
            float gap = abs(s.g - z);
            float w = max(1.0 - gap / tolerance, 0.0);
            sum += s.r * w;
            weights += w;
            if (gap < nearestGap) { nearestGap = gap; nearest = s.r; }
        }
    // Nothing in the footprint is this surface (a thin sliver): take the closest in depth.
    FragColor = weights > 1e-3 ? sum / weights : nearest;
}
