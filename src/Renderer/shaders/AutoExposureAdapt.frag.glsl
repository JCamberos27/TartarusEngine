#version 460 core
// #162 - auto exposure, pass 2: eases the stored exposure offset (EV) toward what the metered
// luminance wants, at Unity's separate brighten/darken speeds. Writes a 1x1 R32F texel the
// tonemap pass reads straight from the GPU (no readback stall).
in vec2 vUV;
out float FragEv;

uniform sampler2D uLum;   // log2 luminance with a full mip chain
uniform float uTopMip;    // the 1x1 level
uniform sampler2D uPrev;  // last frame's EV (1x1)
uniform int   uReset;     // 1 = jump straight to the target (first frame / just enabled)
uniform float uMinEv, uMaxEv;
uniform float uSpeedUp, uSpeedDown, uDt;

void main() {
    const float kLog2MidGrey = -2.473931; // log2(0.18)
    float target = clamp(textureLod(uLum, vec2(0.5), uTopMip).r - kLog2MidGrey, uMinEv, uMaxEv);
    if (uReset != 0) { FragEv = target; return; }
    float prev = texelFetch(uPrev, ivec2(0), 0).r;
    // A brighter scene than we're exposed for (target > prev) adapts at Speed Up.
    float speed = target > prev ? uSpeedUp : uSpeedDown;
    FragEv = prev + (target - prev) * (1.0 - exp(-uDt * speed));
}
