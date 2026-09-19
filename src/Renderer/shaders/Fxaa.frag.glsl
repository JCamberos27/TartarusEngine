#version 460 core
// #162 - FXAA (Lottes' FXAA 3.11 idea in its compact form): finds luma edges in the tonemapped
// LDR image and blends along them. Catches what MSAA can't: specular and shader aliasing,
// alpha-tested edges and thin geometry.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uLdr;
uniform vec2 uTexel; // 1 / resolution

const float kReduceMin = 1.0 / 128.0;
const float kReduceMul = 1.0 / 8.0;
const float kSpanMax   = 8.0;

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgbNW = textureLod(uLdr, vUV + vec2(-1.0, -1.0) * uTexel, 0.0).rgb;
    vec3 rgbNE = textureLod(uLdr, vUV + vec2( 1.0, -1.0) * uTexel, 0.0).rgb;
    vec3 rgbSW = textureLod(uLdr, vUV + vec2(-1.0,  1.0) * uTexel, 0.0).rgb;
    vec3 rgbSE = textureLod(uLdr, vUV + vec2( 1.0,  1.0) * uTexel, 0.0).rgb;
    vec3 rgbM  = textureLod(uLdr, vUV, 0.0).rgb;

    float lumaNW = Luma(rgbNW), lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW), lumaSE = Luma(rgbSE);
    float lumaM  = Luma(rgbM);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // Flat areas: nothing to do (saves the four extra taps and avoids softening texture detail).
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                     ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * kReduceMul, kReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * uTexel;

    vec3 rgbA = 0.5 * (textureLod(uLdr, vUV + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                       textureLod(uLdr, vUV + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (textureLod(uLdr, vUV + dir * -0.5, 0.0).rgb +
                                     textureLod(uLdr, vUV + dir *  0.5, 0.0).rgb);
    float lumaB = Luma(rgbB);
    FragColor = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
