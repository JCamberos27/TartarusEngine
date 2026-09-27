#version 460 core
// The physical sky's full-screen pass (drawn first, behind the scene, like the gradient sky):
// sky-view LUT radiance for the sun and the moon, the sun disc with limb darkening, the moon
// with its phase, stars and the Milky Way at night, all behind the volumetric clouds.
#define ATMOSPHERE_LUTS
#include "AtmosphereCommon.glsl"

in vec3 vNearPoint;
in vec3 vFarPoint;
out vec4 FragColor;

layout(binding = 2) uniform sampler2D uSkyViewSunLut;
layout(binding = 3) uniform sampler2D uSkyViewMoonLut;
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
    float edge = 1.0 - smoothstep(cosR, cos(radius * 1.2), c); // anti-aliased rim
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
        float ring = smoothstep(0.35, 0.28, d) - smoothstep(0.28, 0.1, d) * 0.6;
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

vec3 StarLayer(vec3 d, float scale, float density, float brightness, float twinkleAmt) {
    vec3 p = d * scale;
    vec3 cell = floor(p);
    vec3 h = Hash33(cell);
    if (h.x > density) return vec3(0.0);
    vec3 starPos = normalize(cell + 0.15 + 0.7 * Hash33(cell + 31.7));
    float ang = acos(clamp(dot(d, starPos), -1.0, 1.0));
    float sizeRad = 0.00045 * (0.7 + 0.6 * h.y);
    float core = exp(-ang * ang / (sizeRad * sizeRad));
    // Magnitudes follow a steep power law: a handful of bright stars, many faint ones.
    float mag = pow(h.z, 10.0) * 4.0 + 0.03;
    float twinkle = 1.0 + twinkleAmt * sin(uAtmCamera.w * (3.0 + 7.0 * h.y) + h.x * 50.0);
    return StarColor(Hash33(cell + 5.1).x) * core * mag * twinkle * brightness;
}

vec3 NightSky(vec3 dir, float horizonT) {
    vec3 d = normalize(mat3(uAtmStarRotation) * dir);
    // Twinkle more near the horizon, through more air.
    float twinkle = mix(0.6, 0.1, clamp(dir.y * 2.0, 0.0, 1.0));
    vec3 stars = StarLayer(d, 160.0, 0.05, 1.0, twinkle) + StarLayer(d, 380.0, 0.02, 0.5, twinkle)
               + StarLayer(d, 800.0, 0.012, 0.25, twinkle);
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
    vec3 cam = ClampToAtmosphere(uAtmCamera.xyz);

    vec3 L = texture(uSkyViewSunLut, SkyViewUv(cam, dir, uAtmSunDir.xyz)).rgb;
    if (uAtmParams2.w > 0.0)
        L += texture(uSkyViewMoonLut, SkyViewUv(cam, dir, uAtmMoonDir.xyz)).rgb;
    L *= uAtmRadii.z;

    float groundT = RaySphereNearest(cam, dir, vec3(0.0), uAtmRadii.x);
    bool groundHit = groundT >= 0.0;
    if (groundHit) {
        vec3 up = normalize(cam);
        vec3 skyZenith = texture(uSkyViewSunLut, SkyViewUv(cam, up, uAtmSunDir.xyz)).rgb;
        if (uAtmParams2.w > 0.0) skyZenith += texture(uSkyViewMoonLut, SkyViewUv(cam, up, uAtmMoonDir.xyz)).rgb;
        L += GroundRadiance(cam, dir, groundT, skyZenith * PI) * uAtmRadii.z;
    } else {
        vec3 T = TransmittanceToSpace(cam, dir);
        float Tm = dot(T, vec3(1.0 / 3.0));
        // Stars fade out as the sun comes up (the daytime sky would drown them anyway).
        float night = smoothstep(0.08, -0.12, dot(normalize(cam), uAtmSunDir.xyz));
        vec3 space = NightSky(dir, Tm) * night;
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
