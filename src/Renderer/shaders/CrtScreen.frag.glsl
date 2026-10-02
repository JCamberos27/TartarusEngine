#version 460 core
// The editor's CRT screen (Preferences > General > CRT Screen): the whole editor frame as a white-
// phosphor monitor. A port of the launch screen's tube (tools/launcher/crt.fx), toned down so text
// stays readable for hours: barrel curvature (optional), phosphor glow, scanlines, a slight
// convergence error toward the edges, a slow refresh band and a vignette. No aperture grille: its
// colour fringes fight small text.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTex;
uniform vec2 uSize;      // the frame in device pixels
uniform float uTime;     // seconds
uniform float uCurve;    // 0 = a flat screen, 1 = the launcher's tube
uniform float uStrength; // 0..1, how strong the scanlines / glow / vignette are

// Barrel distortion: the picture bulges like the face of a tube. Must match CrtScreen::Curve, which
// maps the mouse through the same curve so clicks land on what is drawn under the cursor.
vec2 Curve(vec2 uv) {
    uv = uv * 2.0 - 1.0;
    vec2 offset = abs(uv.yx) / vec2(6.0, 4.6);
    uv = uv + uv * offset * offset * uCurve;
    return uv * 0.5 + 0.5;
}

vec3 Tap(vec2 uv) { return texture(uTex, uv).rgb; }

void main() {
    vec2 uv = Curve(vUV);
    // Past the glass: the black of the bezel, with a soft falloff at the tube's edge.
    vec2 edge = uCurve > 0.0 ? smoothstep(0.0, 0.004, uv) * smoothstep(0.0, 0.004, 1.0 - uv) : vec2(1.0);
    if (edge.x * edge.y <= 0.0) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    vec2 px = 1.0 / uSize;
    vec2 fromCentre = uv - 0.5;

    // Convergence error: red and blue slip apart a little toward the edges.
    float spread = (0.4 + 3.0 * dot(fromCentre, fromCentre)) * px.x * uStrength;
    vec3 col;
    col.r = texture(uTex, uv + vec2(spread, 0.0)).r;
    col.g = texture(uTex, uv).g;
    col.b = texture(uTex, uv - vec2(spread, 0.0)).b;

    // Phosphor glow: a near ring and a wider, fainter one, so white text blooms on the black.
    vec3 glow = Tap(uv + px * vec2(1.5, 0.0)) + Tap(uv - px * vec2(1.5, 0.0))
              + Tap(uv + px * vec2(0.0, 1.5)) + Tap(uv - px * vec2(0.0, 1.5));
    vec3 halo = Tap(uv + px * vec2(4.0, 2.5)) + Tap(uv + px * vec2(-4.0, 2.5))
              + Tap(uv + px * vec2(4.0, -2.5)) + Tap(uv + px * vec2(-4.0, -2.5))
              + Tap(uv + px * vec2(7.0, 0.0)) + Tap(uv - px * vec2(7.0, 0.0));
    col += (glow * 0.05 + halo * 0.025) * uStrength;

    // Scanlines: one every 3 device pixels (never finer, or they alias), bright lines blooming over
    // the gap so text keeps its weight.
    float lines = uSize.y / 3.0;
    float scan = sin(uv.y * lines * 6.2831853);
    float lum = dot(col, vec3(0.3, 0.59, 0.11));
    col *= mix(1.0 - 0.32 * uStrength, 1.0, clamp(0.5 + 0.5 * scan + lum * 0.4, 0.0, 1.0));

    // A slow refresh band rolling down the glass, and the tube's faint flicker.
    float band = fract(uv.y - uTime * 0.08);
    col *= 1.0 + 0.035 * uStrength * smoothstep(0.88, 1.0, band) * (1.0 - smoothstep(0.995, 1.0, band));
    col *= 1.0 - 0.012 * uStrength * (0.5 + 0.5 * sin(uTime * 113.0) * sin(uTime * 7.3));

    // Vignette, and a soft glass highlight at the top left.
    float vig = pow(clamp(16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y), 0.0, 1.0), 0.12 * uStrength);
    col *= vig;
    vec2 shine = uv - vec2(0.22, 0.88);
    col += 0.014 * uStrength * clamp(1.0 - length(shine * vec2(1.0, 1.6)) * 2.2, 0.0, 1.0);

    FragColor = vec4(clamp(col, 0.0, 1.0) * edge.x * edge.y, 1.0);
}
