// Physically based sky and atmosphere - shared definitions (see Atmosphere.h).
//
// After Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
// (the model Unreal's SkyAtmosphere uses): Rayleigh + Mie scattering, ozone absorption, and a
// multiple-scattering approximation, precomputed into small LUTs:
//
//   Transmittance LUT   (256 x 64)   optical depth to the top of the atmosphere, by (r, mu)
//   Multi-scatter LUT   (32 x 32)    isotropic higher-order scattering, by (sun zenith, r)
//   Sky-view LUT        (192 x 108)  in-scattered radiance around the camera, per light
//   Aerial perspective  (32^3)       in-scattering + transmittance per camera froxel
//
// Units are KILOMETRES. The planet's centre is at the origin with +Y up; a world position maps
// to atmosphere space as worldPos * uAtmParams.w + (0, bottomRadius + altitude offset, 0), with
// the scene's sea level at world Y = uAtmParams2.y.

#ifndef ATMOSPHERE_COMMON_GLSL
#define ATMOSPHERE_COMMON_GLSL

const float PI = 3.14159265358979;

layout(std140, binding = 2) uniform AtmosphereBlock {
    vec4 uAtmRayleigh;     // rgb scattering (1/km); w = -1 / Rayleigh scale height (km)
    vec4 uAtmMieScat;      // rgb scattering (1/km); w = -1 / Mie scale height (km)
    vec4 uAtmMieExt;       // rgb extinction (1/km); w = Mie phase anisotropy g
    vec4 uAtmOzone;        // rgb absorption (1/km); w = ozone layer centre altitude (km)
    vec4 uAtmGround;       // rgb ground albedo; w = ozone layer half-width (km)
    vec4 uAtmRadii;        // x bottom radius, y top radius (km), z sky intensity, w multiple-scattering factor
    vec4 uAtmSunDir;       // xyz unit vector toward the sun; w = sun angular radius (rad)
    vec4 uAtmSunIllum;     // rgb sun illuminance above the atmosphere; w = sun disk brightness
    vec4 uAtmMoonDir;      // xyz unit vector toward the moon; w = moon angular radius (rad)
    vec4 uAtmMoonIllum;    // rgb moon illuminance (phase applied); w = moon phase (0 new, 0.5 full)
    vec4 uAtmCamera;       // xyz camera position in atmosphere space (km); w = time (s)
    vec4 uAtmParams;       // x aerial perspective scale, y star brightness, z night airglow, w world units -> km
    vec4 uAtmParams2;      // x aerial perspective range (km), y sea level (world Y), z altitude offset (km), w moon light on
    mat4 uAtmStarRotation; // world direction -> celestial (star) frame
};

// LUT sizes (Atmosphere.h mirrors these).
const vec2 kTransmittanceLutSize = vec2(256.0, 64.0);
const vec2 kMultiScatterLutSize  = vec2(32.0, 32.0);
const vec2 kSkyViewLutSize       = vec2(192.0, 108.0);
const float kApSlices            = 32.0;

// ------------------------------------------------------------------------------------------
// Geometry helpers

// Nearest non-negative hit of a ray with a sphere, or -1.
float RaySphereNearest(vec3 ro, vec3 rd, vec3 c, float radius) {
    vec3 oc = ro - c;
    float b = dot(oc, rd);
    float cc = dot(oc, oc) - radius * radius;
    float disc = b * b - cc;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t0 < 0.0 && t1 < 0.0) return -1.0;
    if (t0 < 0.0) return max(0.0, t1);
    if (t1 < 0.0) return max(0.0, t0);
    return max(0.0, min(t0, t1));
}

// Both hits (t0 <= t1) of a ray with a sphere; false when it misses.
bool RaySphere(vec3 ro, vec3 rd, float radius, out float t0, out float t1) {
    float b = dot(ro, rd);
    float cc = dot(ro, ro) - radius * radius;
    float disc = b * b - cc;
    if (disc < 0.0) { t0 = t1 = -1.0; return false; }
    float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

float FromUnitToSubUv(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float FromSubUvToUnit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

// World position -> atmosphere space (km).
vec3 WorldToAtmosphere(vec3 worldPos) {
    vec3 p = worldPos * uAtmParams.w;
    p.y -= uAtmParams2.y * uAtmParams.w;
    p.y += uAtmRadii.x + uAtmParams2.z;
    return p;
}

// Keeps a point at least 1 m above the ground and inside the atmosphere's top.
vec3 ClampToAtmosphere(vec3 p) {
    float r = length(p);
    float lo = uAtmRadii.x + 0.001, hi = uAtmRadii.y - 0.001;
    return r < lo ? p * (lo / max(r, 1e-6)) : (r > hi ? p * (hi / r) : p);
}

// ------------------------------------------------------------------------------------------
// Participating medium

struct MediumSample {
    vec3 scatRayleigh;
    vec3 scatMie;
    vec3 scattering;
    vec3 extinction;
};

MediumSample SampleMedium(vec3 p) {
    float h = max(length(p) - uAtmRadii.x, 0.0);
    float dR = exp(uAtmRayleigh.w * h);
    float dM = exp(uAtmMieScat.w * h);
    float dO = max(0.0, 1.0 - abs(h - uAtmOzone.w) / max(uAtmGround.w, 1e-3));
    MediumSample s;
    s.scatRayleigh = uAtmRayleigh.rgb * dR;
    s.scatMie = uAtmMieScat.rgb * dM;
    s.scattering = s.scatRayleigh + s.scatMie;
    s.extinction = s.scatRayleigh + uAtmMieExt.rgb * dM + uAtmOzone.rgb * dO;
    return s;
}

float RayleighPhase(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }

// Cornette-Shanks: Henyey-Greenstein with the Rayleigh-like (1 + c^2) term, a better fit for
// real aerosols' strong forward peak plus some back-scatter.
float MiePhase(float g, float c) {
    float g2 = g * g;
    float k = 3.0 / (8.0 * PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

float HenyeyGreenstein(float g, float c) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// ------------------------------------------------------------------------------------------
// Transmittance LUT parameterization (Bruneton 2017): x = distance to the top boundary,
// remapped per-altitude so the horizon gets its fair share of texels; y = altitude.

vec2 TransmittanceUvFromRMu(float r, float mu) {
    float Rb = uAtmRadii.x, Rt = uAtmRadii.y;
    float H = sqrt(max(Rt * Rt - Rb * Rb, 0.0));
    float rho = sqrt(max(r * r - Rb * Rb, 0.0));
    float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = Rt - r, dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-6);
    float xR = rho / max(H, 1e-6);
    return vec2(FromUnitToSubUv(xMu, kTransmittanceLutSize.x), FromUnitToSubUv(xR, kTransmittanceLutSize.y));
}

void RMuFromTransmittanceUv(vec2 uv, out float r, out float mu) {
    float xMu = FromSubUvToUnit(uv.x, kTransmittanceLutSize.x);
    float xR = FromSubUvToUnit(uv.y, kTransmittanceLutSize.y);
    float Rb = uAtmRadii.x, Rt = uAtmRadii.y;
    float H = sqrt(max(Rt * Rt - Rb * Rb, 0.0));
    float rho = H * xR;
    r = sqrt(rho * rho + Rb * Rb);
    float dMin = Rt - r, dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

#ifdef ATMOSPHERE_LUTS
layout(binding = 0) uniform sampler2D uTransmittanceLut;
layout(binding = 1) uniform sampler2D uMultiScatterLut;

vec3 SampleTransmittance(float r, float mu) {
    return texture(uTransmittanceLut, TransmittanceUvFromRMu(r, mu)).rgb;
}

// Transmittance from point p (atmosphere space) toward direction dir, out to space. Zero when
// the planet is in the way (with a soft edge the width of the sun so a setting sun fades out
// instead of popping).
vec3 TransmittanceToSpace(vec3 p, vec3 dir) {
    float r = length(p);
    vec3 up = p / r;
    float mu = dot(dir, up);
    vec3 t = SampleTransmittance(r, mu);
    float sinHorizon = uAtmRadii.x / r;
    float cosHorizon = -sqrt(max(1.0 - sinHorizon * sinHorizon, 0.0));
    float fade = smoothstep(cosHorizon - 0.01, cosHorizon + 0.01, mu);
    return t * fade;
}

vec3 SampleMultiScatter(vec3 p, vec3 lightDir) {
    float r = length(p);
    float mu = dot(p / r, lightDir);
    vec2 uv = vec2(clamp(0.5 + 0.5 * mu, 0.0, 1.0),
                   clamp((r - uAtmRadii.x) / (uAtmRadii.y - uAtmRadii.x), 0.0, 1.0));
    uv = vec2(FromUnitToSubUv(uv.x, kMultiScatterLutSize.x), FromUnitToSubUv(uv.y, kMultiScatterLutSize.y));
    return texture(uMultiScatterLut, uv).rgb;
}

// Single + (approximated) multiple scattering along ro + rd * [0, tMax], lit by one light.
// Returns in-scattered radiance per unit illuminance times `illum`; `throughput` receives the
// path's transmittance. `groundBounce` adds the lit ground's reflection when the ray hits it.
vec3 IntegrateScattering(vec3 ro, vec3 rd, float tMax, int sampleCount, vec3 lightDir, vec3 illum,
                         bool groundBounce, out vec3 throughput) {
    throughput = vec3(1.0);
    float Rb = uAtmRadii.x, Rt = uAtmRadii.y;

    // Clip the ray to the atmosphere shell (and the ground).
    float tTop0, tTop1;
    if (!RaySphere(ro, rd, Rt, tTop0, tTop1) || tTop1 < 0.0) return vec3(0.0);
    float tStart = max(tTop0, 0.0);
    float tEnd = tTop1;
    float g0, g1;
    bool hitsGround = RaySphere(ro, rd, Rb, g0, g1) && g0 > 0.0;
    if (hitsGround) tEnd = min(tEnd, g0);
    tEnd = min(tEnd, tMax);
    if (tEnd <= tStart) return vec3(0.0);

    float c = dot(rd, lightDir);
    float phaseR = RayleighPhase(c);
    float phaseM = MiePhase(uAtmMieExt.w, c);

    vec3 L = vec3(0.0);
    float dt = (tEnd - tStart) / float(sampleCount);
    float t = tStart;
    for (int i = 0; i < sampleCount; ++i) {
        float tNew = tStart + (float(i) + 0.3) * dt; // fixed sub-step offset (Hillaire)
        float segment = tNew - t;
        t = tNew;
        vec3 p = ro + rd * t;
        MediumSample m = SampleMedium(p);
        vec3 sampleT = exp(-m.extinction * dt);

        vec3 toLight = TransmittanceToSpace(p, lightDir);
        vec3 phaseScat = m.scatRayleigh * phaseR + m.scatMie * phaseM;
        vec3 ms = SampleMultiScatter(p, lightDir) * m.scattering * uAtmRadii.w;
        vec3 S = illum * (toLight * phaseScat + ms);

        // Energy-conserving analytic integration over the step (Hillaire, eq. 11).
        vec3 Sint = (S - S * sampleT) / max(m.extinction, vec3(1e-7));
        L += throughput * Sint;
        throughput *= sampleT;
    }

    if (groundBounce && hitsGround && tEnd == g0 && g0 <= tMax) {
        vec3 p = ro + rd * g0;
        vec3 n = normalize(p);
        vec3 toLight = TransmittanceToSpace(p, lightDir);
        L += illum * toLight * throughput * max(dot(n, lightDir), 0.0) * uAtmGround.rgb / PI;
    }
    return L;
}

// The planet's surface seen along `dir` from `cam` (which hits it at distance t): lit by the sun,
// the moon and the sky (`skyIrradiance`, e.g. pi x the zenith radiance), seen through the air
// in between. The sky-view LUT already holds the in-scattering along that path; this is the
// light from the ground itself, which it leaves out. Without it the world beyond the scene's own
// floor is a black band under the horizon.
vec3 GroundRadiance(vec3 cam, vec3 dir, float t, vec3 skyIrradiance) {
    vec3 gp = cam + dir * t;
    vec3 n = normalize(gp);
    vec3 sunLit = uAtmSunIllum.rgb * TransmittanceToSpace(gp, uAtmSunDir.xyz) * max(dot(n, uAtmSunDir.xyz), 0.0);
    vec3 moonLit = uAtmMoonIllum.rgb * uAtmMoonIllum.w * uAtmParams2.w
                 * TransmittanceToSpace(gp, uAtmMoonDir.xyz) * max(dot(n, uAtmMoonDir.xyz), 0.0);
    // Transmittance camera -> ground, integrated along the ray (the LUT-ratio shortcut breaks down
    // right at the horizon and drew a bright seam there).
    vec3 od = vec3(0.0);
    float dt = t / 8.0;
    for (int i = 0; i < 8; ++i) od += SampleMedium(cam + dir * ((float(i) + 0.5) * dt)).extinction * dt;
    vec3 viewT = exp(-od);
    return (sunLit + moonLit + skyIrradiance) * uAtmGround.rgb / PI * viewT;
}
#endif // ATMOSPHERE_LUTS

// ------------------------------------------------------------------------------------------
// Sky-view LUT parameterization (Hillaire): x = azimuth relative to the light, squashed toward
// the light; y = view zenith, split at the horizon with extra resolution right at it.

void UvToSkyViewParams(vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos) {
    uv = vec2(FromSubUvToUnit(uv.x, kSkyViewLutSize.x), FromSubUvToUnit(uv.y, kSkyViewLutSize.y));
    float Rb = uAtmRadii.x;
    float vHorizon = sqrt(max(viewHeight * viewHeight - Rb * Rb, 0.0));
    float cosBeta = vHorizon / viewHeight;
    float beta = acos(clamp(cosBeta, -1.0, 1.0));
    float zenithHorizonAngle = PI - beta;
    float viewZenithAngle;
    if (uv.y < 0.5) {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithAngle = zenithHorizonAngle * coord;
    } else {
        float coord = uv.y * 2.0 - 1.0;
        viewZenithAngle = zenithHorizonAngle + beta * coord * coord;
    }
    viewZenithCos = cos(viewZenithAngle);
    lightViewCos = -(uv.x * uv.x * 2.0 - 1.0);
}

vec2 SkyViewParamsToUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight) {
    float Rb = uAtmRadii.x;
    float vHorizon = sqrt(max(viewHeight * viewHeight - Rb * Rb, 0.0));
    float cosBeta = vHorizon / viewHeight;
    float beta = acos(clamp(cosBeta, -1.0, 1.0));
    float zenithHorizonAngle = PI - beta;
    vec2 uv;
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
    if (!intersectGround) {
        float coord = viewZenithAngle / zenithHorizonAngle;
        coord = 1.0 - sqrt(max(1.0 - coord, 0.0));
        uv.y = coord * 0.5;
    } else {
        float coord = (viewZenithAngle - zenithHorizonAngle) / max(beta, 1e-6);
        uv.y = sqrt(max(coord, 0.0)) * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return vec2(FromUnitToSubUv(uv.x, kSkyViewLutSize.x), FromUnitToSubUv(uv.y, kSkyViewLutSize.y));
}

// The sky-view LUT's (viewZenithCos, lightViewCos) for a world direction, relative to `lightDir`.
vec2 SkyViewUv(vec3 camPos, vec3 dir, vec3 lightDir) {
    float viewHeight = length(camPos);
    vec3 up = camPos / viewHeight;
    float viewZenithCos = dot(dir, up);
    // Azimuth relative to the light, measured in the plane perpendicular to up.
    vec3 sideLight = lightDir - up * dot(lightDir, up);
    vec3 sideView = dir - up * dot(dir, up);
    float lenL = length(sideLight), lenV = length(sideView);
    float lightViewCos = (lenL > 1e-5 && lenV > 1e-5) ? dot(sideLight, sideView) / (lenL * lenV) : 1.0;
    bool intersectGround = RaySphereNearest(camPos, dir, vec3(0.0), uAtmRadii.x) >= 0.0;
    return SkyViewParamsToUv(intersectGround, viewZenithCos, lightViewCos, viewHeight);
}

// ------------------------------------------------------------------------------------------
// Hashing / noise shared by the sky, stars and clouds.

float Hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
vec3 Hash33(vec3 p3) {
    p3 = fract(p3 * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}
// Interleaved gradient noise (Jimenez 2014): a cheap per-pixel dither that hides banding and,
// offset per frame, feeds temporal accumulation.
float InterleavedGradientNoise(vec2 pixel, float frame) {
    pixel += 5.588238 * mod(frame, 64.0);
    return fract(52.9829189 * fract(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

#endif // ATMOSPHERE_COMMON_GLSL
