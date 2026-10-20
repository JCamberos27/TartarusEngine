#version 460 core
// World text: a signed-distance glyph (0.5 on the edge), antialiased by its screen-space rate of change,
// with an optional dark rim. Premultiplied output, linear HDR.
in vec2 vUV;
in vec4 vColor;
in float vOutline;
uniform sampler2D uAtlas;
out vec4 FragColor;

void main() {
    float d = texture(uAtlas, vUV).r;
    // Half a pixel of the field, measured along the steeper screen axis (edge-on labels change fast one way).
    float w = clamp(length(vec2(dFdx(d), dFdy(d))) * 0.7, 1e-4, 0.25);
    float fill = smoothstep(0.5 - w, 0.5 + w, d);
    float edge = 0.5 - vOutline;
    float cover = vOutline > 0.0 ? smoothstep(edge - w, edge + w, d) : fill;
    float a = cover * vColor.a;
    if (a < 1e-3) discard;
    FragColor = vec4(vColor.rgb * fill * vColor.a, a);
}
