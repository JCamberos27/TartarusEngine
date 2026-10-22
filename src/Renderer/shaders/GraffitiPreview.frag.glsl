#version 460 core
// The Designer's canvas: the composite over a plaster swatch, lit from the top left so the paint's
// relief reads, written as display sRGB (the panel shows it as it is). The second target is the
// same paint with no wall, straight alpha: the viewport's live overlay while spraying onto a wall.
#include "GraffitiCommon.glsl"

in vec2 vUV;
uniform sampler2D uComposite0, uComposite1;
uniform float uRelief;
uniform float uShowThrough;
uniform vec3 uWall; // linear

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oOverlay;

void main() {
    vec4 c0 = texture(uComposite0, vUV);
    float h = texture(uComposite1, vUV).r;
    vec2 t = 1.0 / uCanvas;
    float dx = texture(uComposite1, vUV + vec2(t.x, 0.0)).r - texture(uComposite1, vUV - vec2(t.x, 0.0)).r;
    float dy = texture(uComposite1, vUV + vec2(0.0, t.y)).r - texture(uComposite1, vUV - vec2(0.0, t.y)).r;
    vec3 n = normalize(vec3(-dx * uRelief * 2.5, -dy * uRelief * 2.5, 1.0));
    float light = 0.9 + 0.35 * dot(n, normalize(vec3(-0.5, 0.6, 0.65)));

    vec2 px = vec2(vUV.x, 1.0 - vUV.y) * uCanvas;
    uvec2 q = uvec2(px / 3.0);
    float grit = GRand(GHash(q.x * 0x9E3779B1u ^ GHash(q.y))) * 0.06;
    vec3 wall = uWall * (0.97 + grit);

    float cover = clamp(c0.a, 0.0, 1.0) * mix(1.0, smoothstep(0.0, 0.7, h), uShowThrough);
    vec3 paint = c0.a > 1e-4 ? c0.rgb / c0.a : vec3(0.0);
    vec3 rgb = mix(wall, paint * light, cover);
    oColor = vec4(LinearToSrgb(rgb), 1.0);
    oOverlay = vec4(LinearToSrgb(paint * light), cover);
}
