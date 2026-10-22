#version 460 core
// One layer onto the paint below it (the compositor's ping-pong): the layer's shadow under it, its
// outline round it (stamps and spray; letters draw their own), clipping, opacity and blend mode, in
// premultiplied linear colour. The second target carries the paint's height (thickness), glow and
// metal through the stack for the bake.
in vec2 vUV;

uniform sampler2D uBelow0, uBelow1; // composite so far: colour, (height, glow, metal, -)
uniform sampler2D uLayer0, uLayer1; // this layer: colour, thickness
uniform sampler2D uBlur, uBlurShadow; // this layer's coverage, blurred for its outline and for its shadow
uniform int uMode;                  // Graffiti::Blend
uniform float uOpacity;
uniform int uClip;
uniform int uOutlineOn;
uniform vec3 uOutline;              // linear
uniform int uShadowOn;
uniform vec3 uShadow;               // linear
uniform vec2 uShadowOffset;         // uv, +y up the image
uniform float uShadowOpacity;
uniform float uMetal, uGlow;

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oAux;

void main() {
    vec4 below = texture(uBelow0, vUV);
    vec4 aux = texture(uBelow1, vUV);
    float keep = uClip != 0 ? below.a : 1.0;
    vec4 L = texture(uLayer0, vUV) * (uOpacity * keep);
    float thick = texture(uLayer1, vUV).r * uOpacity * keep;

    if (uOutlineOn != 0) {
        float o = smoothstep(0.02, 0.12, texture(uBlur, vUV).r) * uOpacity * keep;
        thick += o * 0.6 * (1.0 - L.a);
        L = vec4(L.rgb + uOutline * o * (1.0 - L.a), L.a + o * (1.0 - L.a));
    }
    if (uShadowOn != 0) {
        float s = clamp(texture(uBlurShadow, vUV - uShadowOffset).r, 0.0, 1.0) * uShadowOpacity * uOpacity * keep;
        below = vec4(uShadow * s + below.rgb * (1.0 - s), s + below.a * (1.0 - s));
    }

    vec4 res;
    float la = L.a;
    if (uMode == 4) { // Erase: takes the paint below away where this layer has paint
        res = below * (1.0 - la);
        oColor = res;
        oAux = vec4(aux.r * (1.0 - la), aux.g * (1.0 - la), aux.b * (1.0 - la), 1.0);
        return;
    }
    if (uMode == 1) { // Multiply (W3C, premultiplied)
        res.rgb = L.rgb * below.rgb + L.rgb * (1.0 - below.a) + below.rgb * (1.0 - la);
        res.a = la + below.a - la * below.a;
    } else if (uMode == 2) { // Screen
        res.rgb = L.rgb + below.rgb - L.rgb * below.rgb;
        res.a = la + below.a - la * below.a;
    } else if (uMode == 3) { // Add
        res.rgb = L.rgb + below.rgb;
        res.a = min(la + below.a, 1.0);
    } else { // Normal
        res = L + below * (1.0 - la);
    }
    oColor = res;
    oAux = vec4(aux.r + thick, mix(aux.g, uGlow, la), mix(aux.b, uMetal, la), 1.0);
}
