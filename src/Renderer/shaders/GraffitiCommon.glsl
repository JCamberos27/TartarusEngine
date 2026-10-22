// Shared by the Graffiti compositor's shaders (src/Graffiti/GraffitiCompositor.cpp). Canvas space is
// texels with y down (the image's top row first, as the baked PNGs are written); the canvas FBOs
// keep the image's top at their top row, so a fullscreen pass's vUV.y runs up the image.

uniform vec2 uCanvas; // texels

// lowbias32, the same integer hash as Graffiti::Hash on the CPU: exact on every GPU, so the
// overspray speckle and the peel mask are the same pattern every bake.
uint GHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float GRand(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

// Canvas texel (y down) -> clip space.
vec4 CanvasToClip(vec2 p) { return vec4(p.x / uCanvas.x * 2.0 - 1.0, 1.0 - p.y / uCanvas.y * 2.0, 0.0, 1.0); }

// The layer's gradient (Layer::Gradient): t along `dir` across the canvas, centred.
uniform int uGradientOn;
uniform vec3 uGradientA, uGradientB; // linear
uniform vec2 uGradientDir;           // unit, canvas space (y down)
vec3 GradientAt(vec2 canvasPx, vec3 base) {
    if (uGradientOn == 0) return base;
    float extent = abs(uCanvas.x * uGradientDir.x) + abs(uCanvas.y * uGradientDir.y);
    float t = clamp(dot(canvasPx - uCanvas * 0.5, uGradientDir) / max(extent, 1.0) + 0.5, 0.0, 1.0);
    return mix(uGradientA, uGradientB, t);
}

vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
