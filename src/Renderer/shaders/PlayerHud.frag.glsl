#version 460 core
// The combat HUD over the finished game image: a health bar (bottom left), a red vignette that
// pulses on each hit and stays while health is low, arcs round the centre toward whoever hit the
// player, the hitmarker, and the death fade with a respawn ring. Sizes in pixels times uScale.
in vec2 vUV;
out vec4 FragColor;

uniform vec2  uSize;
uniform float uScale;
uniform float uHealth;        // 0..1
uniform float uHurt;          // 0..1 hit pulse
uniform int   uDead;
uniform float uDeathFade;     // 0..1
uniform float uRespawn;       // 0..1
uniform int   uProtected;
uniform int   uArcs;
uniform float uArcAngle[8];   // radians clockwise from straight ahead (= up on screen)
uniform float uArcAlpha[8];
uniform float uHitmarker;     // 0..1
uniform int   uHitKill;
uniform int   uHitHead;
uniform float uTime;

float band(float d, float r0, float r1) {
    return clamp(d - r0 + 0.5, 0.0, 1.0) * clamp(r1 - d + 0.5, 0.0, 1.0);
}
float boxMask(vec2 p, vec2 lo, vec2 hi) {
    vec2 a = clamp(p - lo + 0.5, 0.0, 1.0) * clamp(hi - p + 0.5, 0.0, 1.0);
    return a.x * a.y;
}
vec4 over(vec4 dst, vec4 src) { return vec4(mix(dst.rgb, src.rgb, src.a), src.a + dst.a * (1.0 - src.a)); }

void main() {
    vec2 frag = gl_FragCoord.xy;
    vec2 p = frag - uSize * 0.5;
    float s = uScale;
    vec4 col = vec4(0.0);

    // Vignette: hurt pulse, plus a slow throb under 35% health.
    vec2 uv = frag / uSize;
    float edge = smoothstep(0.35, 1.05, length((uv - 0.5) * vec2(uSize.x / uSize.y, 1.0)) * 1.25);
    float low = clamp((0.35 - uHealth) / 0.35, 0.0, 1.0);
    float throb = low * (0.55 + 0.25 * sin(uTime * 5.0));
    float vig = clamp(edge * max(uHurt * 0.85, throb), 0.0, 0.85);
    col = over(col, vec4(0.55, 0.02, 0.02, vig));

    // Damage direction arcs, 22 degrees wide, on a ring round the centre.
    float d = length(p);
    float ang = atan(p.x, p.y); // clockwise from up
    for (int i = 0; i < uArcs && i < 8; ++i) {
        float da = abs(mod(ang - uArcAngle[i] + 3.14159265, 6.2831853) - 3.14159265);
        float w = smoothstep(0.42, 0.22, da);
        float ring = band(d, 92.0 * s, 104.0 * s) * w;
        col = over(col, vec4(0.95, 0.12, 0.08, ring * uArcAlpha[i] * 0.9));
    }

    // Hitmarker: red only for a lethal headshot; gold for a nonlethal headshot, white otherwise.
    if (uHitmarker > 0.0) {
        vec2 q = abs(p);
        float diag = abs(q.x - q.y) * 0.7071;
        float along = (q.x + q.y) * 0.7071;
        float tick = clamp(1.6 * s - diag + 0.5, 0.0, 1.0) * band(along, 7.0 * s, 15.0 * s);
        vec3 c = (uHitKill == 1 && uHitHead == 1) ? vec3(1.0, 0.18, 0.12) : (uHitHead == 1 ? vec3(1.0, 0.8, 0.25) : vec3(1.0));
        col = over(col, vec4(c, tick * uHitmarker));
    }

    // Health bar, bottom left: a dark track, the fill (white -> red as it drops), a spawn-
    // protection shimmer.
    if (uDead == 0) {
        vec2 lo = vec2(40.0, 40.0) * s, hi = lo + vec2(260.0, 10.0) * s;
        float track = boxMask(frag, lo - 2.0 * s, hi + 2.0 * s);
        col = over(col, vec4(0.0, 0.0, 0.0, 0.45 * track));
        float fillX = mix(lo.x, hi.x, clamp(uHealth, 0.0, 1.0));
        float fill = boxMask(frag, lo, vec2(fillX, hi.y));
        vec3 c = mix(vec3(0.95, 0.15, 0.1), vec3(0.95), smoothstep(0.25, 0.6, uHealth));
        if (uProtected == 1) c = mix(c, vec3(0.45, 0.8, 1.0), 0.5 + 0.5 * sin(uTime * 10.0));
        col = over(col, vec4(c, 0.9 * fill));
    }

    // Dead: fade to a dark red, and a ring filling clockwise until the respawn.
    if (uDead == 1) {
        col = over(col, vec4(0.08, 0.0, 0.0, 0.75 * uDeathFade));
        float arc = band(d, 26.0 * s, 31.0 * s) * step(ang < 0.0 ? ang / 6.2831853 + 1.0 : ang / 6.2831853, uRespawn);
        float trackRing = band(d, 26.0 * s, 31.0 * s);
        col = over(col, vec4(1.0, 1.0, 1.0, 0.18 * trackRing * uDeathFade));
        col = over(col, vec4(1.0, 0.85, 0.8, 0.9 * arc * uDeathFade));
    }

    if (col.a <= 0.002) discard;
    FragColor = col;
}
