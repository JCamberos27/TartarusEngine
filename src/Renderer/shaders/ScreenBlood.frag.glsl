#version 460 core
// Screen blood (docs/BLOOD_FX.md, v2): Real Blood's splash cell (white shape in alpha, its normal map), drawn over
// the finished (display-referred) frame as a wet film on the lens: dark red where it's thick, thinner and
// lighter at the edges, a glint where its surface turns toward a light above.
uniform sampler2DArray uColor;
uniform sampler2DArray uNormal;
uniform int uColorLayer;
uniform int uNormalLayer;
uniform vec2 uGrid;
uniform int uFrame;
uniform float uOpacity;

in vec2 vUV;
out vec4 FragColor;

vec3 Cell(int layer) {
    vec2 c = vec2(float(uFrame % int(uGrid.x)), float(uFrame / int(uGrid.x)));
    return vec3((c + vec2(vUV.x, 1.0 - vUV.y)) / uGrid, float(layer));
}

void main() {
    float mask = texture(uColor, Cell(uColorLayer)).a;
    float a = smoothstep(0.08, 0.35, mask);
    if (a * uOpacity < 0.005) discard;
    vec3 n = vec3(0.0, 0.0, 1.0);
    if (uNormalLayer >= 0) {
        vec2 xy = texture(uNormal, Cell(uNormalLayer)).rg * 2.0 - 1.0;
        n = normalize(vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0))));
    }
    float thick = smoothstep(0.3, 0.9, mask);
    vec3 col = mix(vec3(0.52, 0.04, 0.035), vec3(0.22, 0.008, 0.006), thick); // thin edges let light through
    vec3 L = normalize(vec3(-0.35, 0.65, 0.68));
    float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 48.0);
    col += vec3(0.9, 0.75, 0.7) * spec * 0.55 + col * 0.25 * max(dot(n, L), 0.0);
    FragColor = vec4(col, a * uOpacity * mix(0.75, 0.95, thick));
}
