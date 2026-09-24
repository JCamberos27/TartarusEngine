#version 460 core
in vec2 vUV;
in vec4 vColor;
flat in int vKind;
uniform float uTime;
out vec4 FragColor;

float hash(float n) { return fract(sin(n) * 43758.5453); }
float noise(float x) {
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), f);
}

void main() {
    if (vKind == 0) {
        // Beam: a soft core across it, and the dust it lights - a slow drift of brighter motes
        // along its length, so it reads as light in the air rather than a painted line.
        float across = exp(-vUV.y * vUV.y * 3.0);
        float d = vUV.x;
        float dust = 0.55 + 0.3 * noise(d * 9.0 - uTime * 0.7) + 0.15 * noise(d * 37.0 + uTime * 1.9);
        float mote = pow(noise(d * 61.0 - uTime * 0.35), 18.0) * 3.0;
        FragColor = vec4(vColor.rgb * vColor.a * across * (dust + mote), 1.0);
    } else if (vKind == 1) {
        // Spot: an over-bright core (washing out to white, the way a camera sees a laser dot)
        // in a tight glow and a wider, faint halo. uv 1 = the quad's edge = 6 core radii.
        float r = length(vUV) * 6.0;
        float core = 1.0 - smoothstep(0.55, 1.0, r);
        float glow = exp(-r * r * 0.5) * 0.45 + exp(-r * 1.1) * 0.12;
        vec3 c = mix(vColor.rgb, vec3(max(vColor.r, max(vColor.g, vColor.b))), core * 0.55);
        float flicker = 0.93 + 0.07 * noise(uTime * 23.0);
        FragColor = vec4(c * (core + glow) * flicker, 1.0);
    } else {
        // Bullet hole: a black bore with a slightly ragged edge in a soft darkened ring (the
        // crushed, scorched surface round it). uv 1 = the quad's edge = 2.5 hole radii.
        float seed = vColor.a * 40.0;
        float ang = atan(vUV.y, vUV.x);
        float r = length(vUV) * 2.5;
        float ragged = 1.0 + 0.10 * sin(ang * 5.0 + seed) + 0.06 * sin(ang * 11.0 + seed * 2.3);
        float bore = 1.0 - smoothstep(0.82, 1.0, r / ragged);
        float ring = (1.0 - smoothstep(1.0, 2.5, r / (0.9 + 0.1 * ragged)));
        float a = max(bore, ring * ring * 0.55);
        if (a < 0.003) discard;
        FragColor = vec4(vColor.rgb * (1.0 - bore * 0.9), a);
    }
}
