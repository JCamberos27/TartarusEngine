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
// `up` is the local vertical at the camera; `T` the air's transmittance toward the sun.
vec3 SunDisc(vec3 dir, vec3 up, vec3 T) {
    float radius = uAtmSunDir.w;
    vec3 s = uAtmSunDir.xyz;
    float c = dot(dir, s);
    if (c < cos(radius * 1.4)) return vec3(0.0);
    // Offset from the disc's centre in its own horizontal / vertical frame (small angles).
    vec3 h = cross(s, up);
    h = dot(h, h) > 1e-8 ? normalize(h) : vec3(1.0, 0.0, 0.0);
    vec3 v = cross(h, s);
    vec2 o = vec2(dot(dir, h), dot(dir, v));
    // Refraction near the horizon lifts the lower limb more than the upper, squashing a setting
    // sun into an oval about 0.8 as tall as it is wide.
    float elev = dot(s, up);
    o.y /= mix(0.8, 1.0, smoothstep(-0.01, 0.09, elev));
    float x = length(o) / radius;
    if (x > 1.2) return vec3(0.0);
    float mu = sqrt(max(1.0 - min(x, 1.0) * min(x, 1.0), 0.0));
    // Limb darkening (Neckel & Labs 1994 fit, via Hillaire): the disc's edge is redder and
    // dimmer than its centre.
    vec3 u = vec3(1.0), a = vec3(0.397, 0.503, 0.652);
    vec3 limb = 1.0 - u * (1.0 - pow(vec3(max(mu, 1e-3)), a));
    // A disc of angular radius r subtends pi r^2 steradians: radiance = illuminance / that.
    vec3 radiance = uAtmSunIllum.rgb / (PI * radius * radius) * limb;
    float edge = 1.0 - smoothstep(1.0, 1.2, x); // anti-aliased rim
    vec3 L = radiance * T * edge * uAtmSunIllum.w;
    // Even dimmed a hundredfold by the air, a setting sun is far past where the tone curve turns
    // everything white - yet the eye sees an orange disc it can look straight at. Compress the
    // disc's brightness toward a level that keeps its colour, harder the dimmer the air leaves
    // it: blinding at midday, a deep orange ball on the horizon. (Scaling the whole colour keeps
    // the hue; clipping channels separately would bleach it.)
    const vec3 kLum = vec3(0.2126, 0.7152, 0.0722);
    float t = dot(T, kLum);
    float k = 1.5 + 3000.0 * (t * t) * (t * t);
    return L * (k / (k + dot(L, kLum)));
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

// The near side's maria, in disc coordinates (x right, y up, radius 1, north up): the dark
// "man in the moon" pattern is what makes even a small moon read as the Moon. Each entry is a
// soft blob (centre, radius); overlapping blobs merge into the big irregular seas.
const vec3 kMaria[22] = vec3[](
    vec3( 0.62,  0.28, 0.13), vec3( 0.55, -0.12, 0.16),  // Crisium, Fecunditatis
    vec3( 0.33, -0.30, 0.10),                            // Nectaris
    vec3( 0.30,  0.10, 0.20), vec3( 0.42,  0.05, 0.12),  // Tranquillitatis
    vec3( 0.22,  0.38, 0.16), vec3( 0.02,  0.28, 0.09),  // Serenitatis, Vaporum
    vec3(-0.35,  0.70, 0.10), vec3(-0.10,  0.72, 0.09),  // Frigoris
    vec3( 0.12,  0.70, 0.08), vec3(-0.55,  0.62, 0.09),
    vec3(-0.25,  0.45, 0.24), vec3(-0.12,  0.52, 0.14),  // Imbrium
    vec3(-0.55,  0.25, 0.22), vec3(-0.62,  0.00, 0.20),  // Oceanus Procellarum
    vec3(-0.50, -0.20, 0.16), vec3(-0.72,  0.30, 0.15),
    vec3(-0.25,  0.05, 0.14), vec3(-0.28, -0.18, 0.12),  // Insularum, Cognitum
    vec3(-0.15, -0.38, 0.16), vec3(-0.45, -0.40, 0.10),  // Nubium, Humorum
    vec3( 0.00,  0.08, 0.08));                           // Medii

float BrightSpot(vec2 q, vec2 c, float sharpness) { vec2 d = q - c; return exp(-dot(d, d) * sharpness); }

// Fractal value noise in about [-0.5, 0.5].
float MoonFbm(vec3 p, int octaves) {
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * (ValueNoise3(p) - 0.5);
        norm += amp;
        p = p * 2.03 + 1.7;
        amp *= 0.5;
    }
    return sum / norm;
}

// Lunar albedo at disc point q (|q| <= 1) whose surface normal is n: dark maria of varying
// depth on bright highlands, with Tycho's rays and the brightest young craters.
float MoonAlbedo(vec2 q, vec3 n) {
    // Warp the lookup so the seas come out as irregular, flowing shapes - evaluated straight, the
    // blobs they're built from read as stamped-on circles.
    vec2 w = q + 0.45 * vec2(MoonFbm(n * 2.5, 4), MoonFbm(n * 2.5 + 5.2, 4));
    float field = 0.0;
    for (int i = 0; i < 22; ++i) {
        vec2 d = (w - kMaria[i].xy) / kMaria[i].z;
        field += exp(-dot(d, d) * 1.1);
    }
    // Soft, ragged shorelines, and shading within each sea.
    float mare = smoothstep(0.1, 1.0, field + MoonFbm(n * 9.0, 4) * 0.6);
    float depth = mare * (0.44 + 0.4 * MoonFbm(n * 6.0 + 3.0, 3));
    float albedo = (1.0 - depth) * (0.93 + 0.18 * MoonFbm(n * 24.0, 3));
    // Tycho's rays (noise over the direction away from it - a vector, so there's no seam) and
    // the brightest craters, all kept faint: at the moon's size they're a glint, not a dot.
    vec2 t = q - vec2(-0.12, -0.68);
    float tl = length(t);
    float rays = pow(ValueNoise3(vec3(t / max(tl, 1e-4) * 9.0, 3.1)), 3.0) * exp(-tl * 2.2);
    albedo += 0.5 * (0.25 * rays + 0.25 * BrightSpot(q, vec2(-0.12, -0.68), 400.0)
                   + 0.15 * BrightSpot(q, vec2(-0.22, 0.22), 500.0) + 0.2 * BrightSpot(q, vec2(-0.60, 0.38), 1200.0));
    return albedo;
}

// `night` is 1 once the sun is down (the moon then shows its full detail); `pixelAngle` sets the
// anti-aliased rim.
vec3 MoonDisc(vec3 dir, float night, float pixelAngle) {
    float radius = uAtmMoonDir.w;
    vec3 m = uAtmMoonDir.xyz;
    float c = dot(dir, m);
    if (c < cos(radius * 1.15)) return vec3(0.0);
    // Screen-right and up across the disc, as seen looking toward it with north up.
    vec3 right = normalize(cross(m, abs(m.y) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0)));
    vec3 up = cross(right, m);
    vec2 q = vec2(dot(dir, right), dot(dir, up)) / sin(radius);
    float r = length(q);
    float rim = max(pixelAngle / sin(radius), 0.02); // one pixel, in disc units
    if (r > 1.0 + rim) return vec3(0.0);
    vec2 qc = r > 1.0 ? q / r : q;
    vec3 n = qc.x * right + qc.y * up - sqrt(max(1.0 - dot(qc, qc), 0.0)) * m; // faces the viewer
    // Lommel-Seeliger: regolith scatters like a dusty surface, not a matte (Lambert) one, so a
    // full moon is evenly bright right to its rim instead of darkening toward it, and the
    // terminator stays crisp.
    vec3 sunDir = uAtmSunDir.xyz;
    float mu0 = max(dot(n, sunDir), 0.0), mu = max(dot(n, -m), 1e-3);
    float lit = 2.0 * mu0 / (mu0 + mu);
    // Opposition surge: the full moon is brighter still, as shadows between grains vanish.
    float phaseAngle = acos(clamp(dot(-m, sunDir), -1.0, 1.0));
    lit *= 1.0 + 0.25 * exp(-phaseAngle / 0.1);
    // Earthshine: sunlight off the Earth faintly lights the dark side, most around new moon.
    float earthshine = 0.02 * (1.0 - uAtmMoonIllum.w);
    float albedo = MoonAlbedo(qc, n);
    // Brightness. Physically the disc is thousands of times brighter than the night sky and would
    // print as a white blob; the eye adapts to it locally and sees its face. It's placed where
    // the tone curve keeps that detail - scaled to the scene's sun, so it tracks exposure - and
    // washed out by day, when the moon is a pale ghost on the blue.
    float level = dot(uAtmSunIllum.rgb, vec3(0.2126, 0.7152, 0.0722)) * mix(0.16, 0.22, night);
    vec3 tint = vec3(1.0, 0.97, 0.92); // grey regolith, faintly warm
    float edge = 1.0 - smoothstep(1.0 - rim, 1.0 + rim, r);
    return tint * level * albedo * (lit + earthshine) * edge;
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
        if (uAtmParams2.w > 0.0) space += MoonDisc(dir, night, pixelAngle);
        L += space * T + SunDisc(dir, normalize(cam), T);
    }

    if (uCloudsOn == 1) {
        vec2 uv = (gl_FragCoord.xy - uViewportRect.xy) / uViewportRect.zw;
        vec4 c = texture(uClouds, uv);
        L = L * c.a + c.rgb;
    }

    // A whisper of noise breaks up 8-bit banding in the smooth gradients after tonemapping.
    float n = InterleavedGradientNoise(gl_FragCoord.xy, uFrame) - 0.5;
    L *= 1.0 + n * 0.012;
    // Capped well below half-float max: see SkyHdri.frag - bloom sums texels. Scaled as a whole
    // so a bright coloured source keeps its hue.
    FragColor = vec4(L * min(1.0, 1000.0 / max(max(L.r, L.g), max(L.b, 1e-6))), 1.0);
}
