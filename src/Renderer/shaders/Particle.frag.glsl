#version 460 core
// #177 - soft round particle: alpha falls off smoothly towards the quad's edge.
// Flame instances (the AK105 muzzle flash from the Tactical Shooter pack's M_Muzzle graph): the
// texture packs four flame masks in its R/G/B/A channels, two flames side by side; the seed picks
// the channel and the side. The mask is a smoky body's alpha, its square the emission (tinted and
// timed by the particle colour), which fades out toward the tip. Output is premultiplied.
in vec2 vCorner;
in vec4 vColor;
in float vSeed;
flat in float vIsFlame;
out vec4 FragColor;

uniform sampler2D uFlameTex;

// M_Muzzle's body is lit grey (base colour 0.25 in gamma); this stands in for it in daylight.
const vec3 kFlameBody = vec3(0.12, 0.115, 0.11);

void main() {
    if (vIsFlame > 0.5) {
        vec2 q = vCorner * 0.5 + 0.5;           // q.y: 0 at the base, 1 at the tip
        float side = step(0.5, vSeed) - 0.5;    // -0.5 / +0.5
        float chan = floor(4.0 * fract(2.0 * vSeed));
        vec2 uv = vec2(0.365 + 0.27 * q.x + 0.5 * side, 1.0 - q.y);
        vec4 t = texture(uFlameTex, uv);
        float m = chan < 0.5 ? t.r : chan < 1.5 ? t.g : chan < 2.5 ? t.b : t.a;
        m = clamp(m, 0.0, 1.0);
        float a = vColor.a * m;
        vec3 glow = vColor.rgb * (m * m) * pow(max(1.0 - q.y, 1e-4), 0.1);
        FragColor = vec4(kFlameBody * a + glow, a);
        return;
    }
    float r2 = dot(vCorner, vCorner);
    if (r2 >= 1.0) discard;
    float soft = 1.0 - smoothstep(0.0, 1.0, r2);
    FragColor = vec4(vColor.rgb, vColor.a * soft);
}
