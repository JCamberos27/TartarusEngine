#version 460 core
// The physical sky's full-screen pass (drawn first, behind the scene, like the gradient sky):
// sky-view LUT radiance for the sun and the moon, the sun disc with limb darkening, the moon
// with its phase, stars and the Milky Way at night, all behind the volumetric clouds.
#define ATMOSPHERE_LUTS
#define SKY_VIEW_LUTS
#include "AtmosphereCommon.glsl"

in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

layout(binding = 4) uniform sampler2D uClouds; // this view's resolved clouds: rgb radiance, a transmittance

uniform int uCloudsOn;
uniform vec4 uViewportRect; // x, y, width, height of the view in the framebuffer
uniform float uFrame;

// --- Sun ----------------------------------------------------------------------------------
vec3 SunDisc(vec3 dir) {
    float radius = uAtmSunDir.w;
    float c = dot(dir, uAtmSunDir.xyz);
    float cosR = cos(radius);
    if (c < cos(radius * 1.2)) return vec3(0.0);
    float angle = acos(clamp(c, -1.0, 1.0));
    float x = clamp(angle / radius, 0.0, 1.0);
    float mu = sqrt(max(1.0 - x * x, 0.0));
    // Limb darkening (Neckel & Labs 1994 fit, via Hillaire): the disc's edge is redder and
    // dimmer than its centre.
    vec3 u = vec3(1.0), a = vec3(0.397, 0.503, 0.652);
    vec3 limb = 1.0 - u * (1.0 - pow(vec3(max(mu, 1e-3)), a));
    // A disc of angular radius r subtends pi r^2 steradians: radiance = illuminance / that.
    vec3 radiance = uAtmSunIllum.rgb / (PI * radius * radius) * limb;
    float edge = smoothstep(cos(radius * 1.2), cosR, c); // anti-aliased rim
    return radiance * edge * uAtmSunIllum.w;
}

// --- Moon ---------------------------------------------------------------------------------
float ValueNoise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = Hash33(i).x, n100 = Hash33(i + vec3(1, 0, 0)).x;
    float n010 = Hash33(i + vec3(0, 1, 0)).x, n110 = Hash33(i + vec3(1, 1, 0)).x;
    float n001 = Hash33(i + vec3(0, 0, 1)).x, n101 = Hash33(i + vec3(1, 0, 1)).x;
    float n011 = Hash33(i + vec3(0, 1, 1)).x, n111 = Hash33(i + vec3(1, 1, 1)).x;
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Procedural lunar albedo: dark maria, bright highlands, a scatter of craters.
float MoonAlbedo(vec3 n) {
    float maria = ValueNoise3(n * 2.2) * 0.6 + ValueNoise3(n * 4.7) * 0.4;
    float albedo = mix(1.15, 0.62, smoothstep(0.45, 0.65, maria));
    float craters = 0.0;
    for (int i = 0; i < 3; ++i) {
        float s = 9.0 * pow(2.3, float(i));
        vec3 cell = floor(n * s);
        vec3 h = Hash33(cell + float(i) * 17.0);
        vec3 center = (cell + 0.2 + 0.6 * h) / s;
        float d = length(n - center) * s;
        float ring = (1.0 - smoothstep(0.28, 0.35, d)) - (1.0 - smoothstep(0.1, 0.28, d)) * 0.6;
        craters += ring * (h.z > 0.55 ? 0.35 : 0.0) / float(i + 1);
    }
    return max(albedo + craters * 0.5, 0.2);
}

vec3 MoonDisc(vec3 dir) {
    float radius = uAtmMoonDir.w;
    vec3 m = uAtmMoonDir.xyz;
    float c = dot(dir, m);
    if (c < cos(radius * 1.15)) return vec3(0.0);
    // Point on the visible hemisphere under this view direction.
    vec3 right = normalize(cross(abs(m.y) > 0.99 ? vec3(1, 0, 0) : vec3(0, 1, 0), m));
    vec3 up = cross(m, right);
    vec2 q = vec2(dot(dir, right), dot(dir, up)) / sin(radius);
    float r2 = dot(q, q);
    if (r2 > 1.0) return vec3(0.0);
    vec3 n = q.x * right + q.y * up - sqrt(1.0 - r2) * m; // faces the viewer
    // Lit by the sun: this is what draws the phase (crescent, gibbous, full).
    float lit = max(dot(n, uAtmSunDir.xyz), 0.0);
    // A hint of earthshine on the dark side.
    float earthshine = 0.012;
    float albedo = MoonAlbedo(n);
    vec3 radiance = uAtmMoonIllum.rgb / (PI * radius * radius) * albedo * (lit * 1.5 + earthshine);
    float edge = 1.0 - smoothstep(0.96, 1.0, r2);
    return radiance * edge;
}

// --- Stars and the Milky Way --------------------------------------------------------------
vec3 StarColor(float t) {
    // Rough blackbody tint from ~3000 K (orange) to ~12000 K (blue-white).
    return mix(vec3(1.0, 0.72, 0.48), mix(vec3(1.0, 0.96, 0.9), vec3(0.72, 0.82, 1.0), t), smoothstep(0.0, 0.5, t));
}

// One layer of the star field: at most one star per cell of a grid over the unit sphere,
// `scale` cells across. The eight cells around the sample are all checked, so a star's glow can
// reach across a cell border.
vec3 StarLayer(vec3 d, float scale, float density, float brightness, float twinkleAmt, float pixelAngle) {
    vec3 p = d * scale;
    vec3 base = floor(p - 0.5);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        vec3 cell = base + vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        vec3 h = Hash33(cell);
        if (h.x > density) continue;
        vec3 starPos = normalize(cell + 0.15 + 0.7 * Hash33(cell + 31.7));
        float ang = acos(clamp(dot(d, starPos), -1.0, 1.0));
        float sizeRad = 0.00045 * (0.7 + 0.6 * h.y);
        // A star narrower than a pixel, sampled at pixel centres, flickers as the camera turns:
        // widen it to about half a pixel and dim it to match, keeping its total light. Capped at a
        // sixth of a cell, the furthest a star can be from the cells searched above.
        float width = clamp(0.5 * pixelAngle, sizeRad, max(sizeRad, 0.16 / scale));
        float core = exp(-ang * ang / (width * width)) * (sizeRad * sizeRad) / (width * width);
        // Magnitudes follow a steep power law: a handful of bright stars, many faint ones.
        float mag = pow(h.z, 10.0) * 4.0 + 0.03;
        float twinkle = 1.0 + twinkleAmt * sin(uAtmCamera.w * (3.0 + 7.0 * h.y) + h.x * 50.0);
        sum += StarColor(Hash33(cell + 5.1).x) * core * mag * twinkle;
    }
    return sum * brightness;
}

vec3 NightSky(vec3 dir, float horizonT, float pixelAngle) {
    vec3 d = normalize(mat3(uAtmStarRotation) * dir);
    // Twinkle more near the horizon, through more air.
    float twinkle = mix(0.6, 0.1, clamp(dir.y * 2.0, 0.0, 1.0));
    // The fainter layers use coarser grids with more stars per cell (the same counts as finer,
    // sparser grids) so a pixel-wide star still fits inside the cells searched.
    vec3 stars = StarLayer(d, 160.0, 0.05, 1.0, twinkle, pixelAngle)
               + StarLayer(d, 250.0, 0.046, 0.5, twinkle, pixelAngle)
               + StarLayer(d, 300.0, 0.085, 0.25, twinkle, pixelAngle);
    // Milky Way: a band around the galactic plane, mottled with dust lanes.
    const vec3 kGalacticNormal = normalize(vec3(0.25, 0.45, 0.86));
    float band = exp(-pow(dot(d, kGalacticNormal), 2.0) * 28.0);
    float mottle = ValueNoise3(d * 9.0) * 0.6 + ValueNoise3(d * 23.0) * 0.4;
    float dust = smoothstep(0.35, 0.75, ValueNoise3(d * 14.0 + 3.0));
    vec3 milky = vec3(0.55, 0.6, 0.8) * band * mix(0.4, 1.2, mottle) * (1.0 - 0.6 * dust) * 0.06;
    float airglow = uAtmParams.z;
    vec3 glow = vec3(0.10, 0.16, 0.12) * airglow * mix(1.0, 0.35, clamp(dir.y, 0.0, 1.0)); // brightest low down
    return (stars + milky) * uAtmParams.y * horizonT + glow;
}

void main() {
    vec3 dir = normalize(vFarPoint - vNearPoint);
    // The angle one pixel spans (taken here, in uniform control flow, for the star field).
    float pixelAngle = max(length(dFdx(dir)), length(dFdy(dir)));
    vec3 cam = ClampToAtmosphere(uAtmCamera.xyz);

    vec3 L = SkyRadiance(cam, dir) * uAtmRadii.z;

    float groundT = RaySphereNearest(cam, dir, vec3(0.0), uAtmRadii.x);
    bool groundHit = groundT >= 0.0;
    if (groundHit) {
        L += GroundRadiance(cam, dir, groundT, SkyAmbientTerm(0)) * uAtmRadii.z;
    } else {
        vec3 T = TransmittanceToSpace(cam, dir);
        float Tm = dot(T, vec3(1.0 / 3.0));
        // Stars fade out as the sun comes up (the daytime sky would drown them anyway).
        float night = 1.0 - smoothstep(-0.12, 0.08, dot(normalize(cam), uAtmSunDir.xyz));
        vec3 space = night > 0.0 ? NightSky(dir, Tm, pixelAngle) * night : vec3(0.0);
        if (uAtmParams2.w > 0.0) space += MoonDisc(dir);
        L += space * T + SunDisc(dir) * T;
    }

    if (uCloudsOn == 1) {
        vec2 uv = (gl_FragCoord.xy - uViewportRect.xy) / uViewportRect.zw;
        vec4 c = texture(uClouds, uv);
        L = L * c.a + c.rgb;
    }

    // A whisper of noise breaks up 8-bit banding in the smooth gradients after tonemapping.
    float n = InterleavedGradientNoise(gl_FragCoord.xy, uFrame) - 0.5;
    L *= 1.0 + n * 0.012;
    // Capped well below half-float max: see SkyHdri.frag - bloom sums texels.
    FragColor = vec4(min(L, vec3(1000.0)), 1.0);
}
