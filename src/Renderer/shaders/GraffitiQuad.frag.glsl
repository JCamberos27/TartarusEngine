#version 460 core
// A stamp or a letter. Coverage from the image's alpha (or luminance for white-on-black packs); a
// letter's outline from a blurrier mip of that coverage (the border is clamped to transparent), so
// each letter carries its own outline and overlapping letters stack like painted ones.
#include "GraffitiCommon.glsl"

in vec2 vUV;
in vec2 vCanvas;

uniform sampler2D uImage; // sRGB: reads linear
uniform vec3 uFill, uOutline; // linear
uniform float uOutlineLod;    // the mip whose blur reaches the outline width; < 0: no outline
uniform int uLuma, uTextureColor, uOwnColor;
uniform float uReveal;        // 0..1: the Designer's cascade-in (1 in a bake)

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oThick;

float Coverage(vec4 t) { return uLuma != 0 ? dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)) : t.a; }

void main() {
    vec4 t = texture(uImage, vUV);
    float a = Coverage(t);
    vec3 fill = uTextureColor != 0 ? t.rgb / max(t.a, 1e-4) : (uOwnColor != 0 ? uFill : GradientAt(vCanvas, uFill));
    vec3 rgb = fill * a;
    if (uOutlineLod >= 0.0) {
        float o = smoothstep(0.03, 0.2, Coverage(textureLod(uImage, vUV, uOutlineLod)));
        rgb += uOutline * o * (1.0 - a);
        a += o * (1.0 - a);
    }
    a *= uReveal;
    rgb *= uReveal;
    if (a <= 0.0) discard;
    oColor = vec4(rgb, a);
    oThick = vec4(a * 0.8, 0.0, 0.0, a);
}
