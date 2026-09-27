// Volumetric clouds - shared definitions (see VolumetricClouds.h). Include after
// AtmosphereCommon.glsl with ATMOSPHERE_LUTS and SKY_VIEW_LUTS defined: cloud lighting uses the atmosphere's
// transmittance (so sunset light is red by the time it reaches the clouds) and its sky-view LUT
// (for the sky's ambient light on them).
//
// The cloud layer is a spherical shell around the same planet as the atmosphere, in the same
// kilometre units. Density is Schneider's model (Horizon: Zero Dawn, 2015/2017): a weather map
// says where clouds form and what kind, a height profile per cloud type shapes them vertically,
// Perlin-Worley noise builds the base, and Worley detail noise erodes the edges. Lighting is
// Beer-Lambert extinction with a short cone march toward the light, a dual-lobe phase function,
// Wrenninge's multiple-scattering octaves, a powder term, and height-graded sky ambient.

#ifndef CLOUDS_COMMON_GLSL
#define CLOUDS_COMMON_GLSL

layout(std140, binding = 3) uniform CloudBlock {
    vec4 uCloudLayer;      // x inner radius (km), y outer radius (km), z thickness (km), w coverage 0..1
    vec4 uCloudShape;      // x shape frequency (1/km), y detail frequency (1/km), z detail erosion, w extinction (1/km)
    vec4 uCloudWind;       // xy weather-map offset (km), zw shape-noise offset (km)
    vec4 uCloudLightDir;   // xyz toward the light that lights the clouds; w = cloud type 0..1
    vec4 uCloudLightIllum; // rgb that light's illuminance above the atmosphere; w = forward-scatter g
    vec4 uCloudAmbient;    // x ambient strength, y powder strength, z weather frequency (1/km), w multi-scatter strength
    vec4 uCloudMisc;       // x max march distance (km), y cirrus coverage, z cirrus altitude (km above ground), w cirrus frequency (1/km)
    vec4 uCloudMisc2;      // xy cirrus offset (km), z wind shear (km of lean over the layer), w seed offset
    vec4 uCloudWindDir;    // xy unit wind direction (x, z); z = anvil bulge, w = horizon haze strength
};

layout(binding = 4) uniform sampler3D uCloudShapeNoise;
layout(binding = 5) uniform sampler3D uCloudDetailNoise;
layout(binding = 6) uniform sampler2D uCloudWeather;

float Remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }
float Saturate(float x) { return clamp(x, 0.0, 1.0); }

// Vertical density profile for a cloud type, over the normalized layer height h (0 = base, 1 =
// top). 0 = stratus (a thin sheet low in the layer), 0.5 = cumulus (puffy, flat-bottomed),
// 1 = cumulonimbus (towers the full height).
// `top` receives the height where the profile reaches zero.
float HeightProfile(float h, float type, out float top) {
    const vec4 kStratus      = vec4(0.00, 0.06, 0.12, 0.22);
    const vec4 kCumulus      = vec4(0.00, 0.12, 0.40, 0.72);
    const vec4 kCumulonimbus = vec4(0.00, 0.08, 0.78, 1.00);
    vec4 g = type < 0.5 ? mix(kStratus, kCumulus, type * 2.0) : mix(kCumulus, kCumulonimbus, type * 2.0 - 1.0);
    top = g.w;
    return smoothstep(g.x, g.y, h) * (1.0 - smoothstep(g.z, g.w, h));
}

const float kWeatherTexels = 2048.0; // the weather map's size (VolumetricClouds.cpp)

// The weather map at p, from the mip matching a footprint of `footprintKm`.
vec4 SampleWeather(vec3 p, float footprintKm) {
    vec2 uv = (p.xz + uCloudWind.xy) * uCloudAmbient.z + uCloudMisc2.w;
    float texelKm = 1.0 / (uCloudAmbient.z * kWeatherTexels);
    return textureLod(uCloudWeather, uv, max(log2(footprintKm / texelKm), 0.0));
}

// Local coverage: the global Coverage slider moves the threshold on the weather map's field.
// The weather map decides where clouds may form - fields of cloud with clear sky between, a
// larger share of the sky as the slider rises - and how much of the base noise fills in there.
// Near 1 it closes up into an unbroken deck.
//
// `hTop` is the height within the cloud (0 = base, 1 = its type's top). Higher up, a point has
// to sit deeper inside the field, so a cloud narrows toward its top in a dome. Without that its
// sides rose as straight walls under a flat top, which at a distance read as rectangular slabs
// stacked along the horizon.
float LocalCoverage(vec4 weather, float hTop) {
    float c = uCloudLayer.w;
    if (c <= 0.001) return 0.0;
    float field = smoothstep(1.0 - c - 0.18, 1.0 - c + 0.18, weather.r);
    float dome = 0.7 * smoothstep(0.15, 1.0, hTop);
    field = Saturate(Remap(field, dome, 1.0, 0.0, 1.0));
    float fill = 0.25 + 0.45 * c;
    return mix(field * fill, 0.58, smoothstep(0.85, 1.0, c));
}

// Size (km) of one texel of the shape noise (128^3) and of the detail noise (32^3).
float ShapeTexelKm()  { return 1.0 / (uCloudShape.x * 128.0); }
float DetailTexelKm() { return 1.0 / (uCloudShape.y * 32.0); }

// Cloud extinction (1/km) at atmosphere-space point p. `detail` adds the high-frequency
// erosion - skipped by cheap passes (light march tail, shadow map) where it can't be seen.
// `footprintKm` is how much space one sample stands for (a pixel's width at that distance, or
// a light-march step): the noise is read from the mip that matches it, so distant clouds stay
// soft instead of sparkling with detail no pixel can resolve.
float CloudDensity(vec3 p, bool detail, float footprintKm) {
    float r = length(p);
    float h = (r - uCloudLayer.x) / uCloudLayer.z;
    if (h <= 0.0 || h >= 1.0) return 0.0;

    vec4 weather = SampleWeather(p, footprintKm);
    // A ragged base: the fine weather channel lifts it by up to a few percent of the layer.
    h -= (weather.a - 0.5) * 0.06;
    if (h <= 0.0) return 0.0;
    float type = Saturate(uCloudLightDir.w + (weather.g - 0.5) * 0.45);
    float top;
    float profile = HeightProfile(h, type, top);
    if (profile <= 0.0) return 0.0;
    float coverage = LocalCoverage(weather, h / top);
    if (coverage <= 0.0) return 0.0;

    // Clouds lean downwind with height, and the shape noise drifts with the wind.
    vec3 sp = p;
    sp.xz += uCloudWindDir.xy * (h * uCloudMisc2.z) + uCloudWind.zw;
    vec4 n = textureLod(uCloudShapeNoise, sp * uCloudShape.x, max(log2(footprintKm / ShapeTexelKm()), 0.0));
    float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float base = Remap(n.r, -(1.0 - fbm), 1.0, 0.0, 1.0);
    base *= profile;
    // Anvil: tall types spread out near the top.
    base = pow(max(base, 0.0), Remap(Saturate(h * type), 0.7, 0.8, 1.0, mix(1.0, 0.5, uCloudWindDir.z)));

    float cloud = Saturate(Remap(base, 1.0 - coverage, 1.0, 0.0, 1.0)) * coverage;
    if (cloud <= 0.0) return 0.0;

    if (detail) {
        vec3 dp = sp * uCloudShape.y + vec3(0.0, uCloudWind.z * 0.3, 0.0);
        vec3 d = textureLod(uCloudDetailNoise, dp, max(log2(footprintKm / DetailTexelKm()), 0.0)).rgb;
        float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
        // Wispy near the base, billowy toward the top.
        float erode = mix(dfbm, 1.0 - dfbm, Saturate(h * 5.0)) * uCloudShape.z;
        cloud = Saturate(Remap(cloud, erode, 1.0, 0.0, 1.0));
    }
    // Real cloud boundaries are sharp: the water content climbs quickly past the edge. Lifting
    // the thin outer values firms the silhouette into billows instead of a wide soft falloff.
    cloud = sqrt(cloud);
    // Soft bottom so the base isn't a hard slice.
    cloud *= Saturate(h * 12.0);
    return cloud * uCloudShape.w;
}

// Fixed offsets spreading the light march into a cone (Schneider): a fixed kernel rather than a
// hash of the position, whose planet-scale coordinates would lose the precision a hash needs and
// leave a static grain the temporal pass can't average away.
const vec3 kConeKernel[6] = vec3[](
    vec3( 0.38,  0.12, -0.29), vec3(-0.31,  0.27,  0.16), vec3( 0.09, -0.35,  0.33),
    vec3(-0.22, -0.18, -0.36), vec3( 0.41,  0.30,  0.08), vec3(-0.05,  0.44, -0.21));

// Optical depth from p toward the light: up to six steps of growing length through the layer,
// spread over a cone, the last two without detail erosion. `lightSteps` scales down for cheap
// passes; `footprintKm` is the view sample's own footprint (see CloudDensity).
float LightOpticalDepth(vec3 p, int lightSteps, float footprintKm) {
    vec3 L = uCloudLightDir.xyz;
    float od = 0.0;
    float stepLen = uCloudLayer.z * 0.035;
    float t = 0.0;
    for (int i = 0; i < lightSteps; ++i) {
        float len = stepLen * pow(1.9, float(i));
        t += len;
        vec3 q = p + L * (t - len * 0.5) + kConeKernel[i % 6] * (len * 0.6);
        od += CloudDensity(q, i < lightSteps - 2, max(footprintKm, ShapeTexelKm() * exp2(float(i) * 0.5))) * len;
    }
    return od;
}

// Dual-lobe Henyey-Greenstein: a strong forward peak (the silver lining when looking toward the
// sun) blended with mild back-scatter.
float CloudPhase(float c, float gScale) {
    float g = uCloudLightIllum.w;
    return mix(HenyeyGreenstein(g * gScale, c), HenyeyGreenstein(-0.25 * gScale, c), 0.3);
}

// Light scattered toward the viewer at a sample, per unit extinction. Wrenninge et al. 2013:
// successive octaves with weaker extinction and flatter phase approximate multiple scattering,
// which is what makes thick cloud interiors bright rather than black.
vec3 CloudLighting(float lightOD, float cosTheta, float density, vec3 lightRadiance, vec3 ambient, float h) {
    float msStrength = uCloudAmbient.w;
    float a = 1.0, b = 1.0, c = 1.0;
    float scatter = 0.0;
    for (int i = 0; i < 4; ++i) {
        scatter += a * exp(-lightOD * b) * CloudPhase(cosTheta, c);
        a *= 0.7 * msStrength;
        b *= 0.3;
        c *= 0.5;
    }
    // Deep inside thick cloud even the flattest octave is extinguished, yet light still diffuses
    // through: two-stream theory gives a diffuse transmittance of ~1 / (1 + 0.75 tau (1 - g)).
    // It's what makes the underside of a storm dark grey rather than black, and brighter where
    // the cloud above is thinner - the structure you see from below a deck.
    // That diffuse light leaves isotropically (~1/4pi). A separate cumulus loses much of it out
    // of its sides, so its base is distinctly grey; in a closed deck the neighbouring cloud
    // sends it back, so decks keep more (up to twice as much, at full coverage).
    float diffuse = 1.0 / (1.0 + 0.75 * lightOD * (1.0 - 0.85));
    float diffuseK = mix(0.12, 0.25, smoothstep(0.7, 1.0, uCloudLayer.w));
    scatter += diffuseK * msStrength * diffuse * smoothstep(2.0, 25.0, lightOD);
    // Cloud droplets barely absorb (albedo ~0.99): light scattered many times inside still comes
    // back out, which is what makes a cloud facing away from the sun read white rather than
    // grey. The octaves above undercount that for thick cloud; this restores it.
    scatter *= 2.0;
    // Powder: dark edges when looking away from the sun, from light that hasn't been scattered
    // in yet near the surface (Schneider).
    float powder = 1.0 - exp(-2.0 * lightOD - density * 0.25);
    powder = mix(1.0, powder, uCloudAmbient.y * Saturate(0.5 - 0.5 * cosTheta) * 2.0);
    vec3 direct = lightRadiance * scatter * powder;
    // Ambient: sky light from above, darker toward the base, which the cloud itself shadows.
    vec3 amb = ambient * mix(0.35, 1.0, h) * uCloudAmbient.x;
    return direct + amb;
}

struct CloudResult {
    vec3 radiance;       // light scattered toward the viewer (pre-multiplied)
    float transmittance; // fraction of what's behind the clouds that gets through
    float depth;         // transmittance-weighted distance to the clouds (km), for reprojection
};

// Sky light for the cloud ambient term: the sky-view LUT straight up (top) and around the
// horizon (bottom, standing in for the ground-lit underside) - averaged over azimuth, so the
// underside doesn't depend on which way the sun happens to sit relative to the world axes.
void CloudAmbientColors(out vec3 top, out vec3 bottom) {
    top = SkyAmbientTerm(1);
    bottom = SkyAmbientTerm(2);
    // Isotropic in-scattering of a uniform radiance field returns that radiance, so the sky's
    // radiance is the ambient term as-is; the underside mostly sees the darker horizon/ground.
    bottom *= 0.5;
    // Under heavy cover the clouds hide the blue sky from each other: the light inside a deck is
    // mostly scattered sunlight, greyer, and the base of it is dim.
    float c = uCloudLayer.w;
    top = mix(top, vec3(dot(top, vec3(0.2126, 0.7152, 0.0722))), c * 0.6);
    bottom = mix(bottom, vec3(dot(bottom, vec3(0.2126, 0.7152, 0.0722))), c * 0.6) * mix(1.0, 0.55, c * c);
}

// High-altitude cirrus: a thin 2D sheet at its own altitude, streaked along the wind.
// `pixelAngle` (radians) picks the weather map's mip at the sheet's distance.
vec4 CirrusLayer(vec3 ro, vec3 rd, vec3 lightRadianceTop, vec3 ambientTop, float pixelAngle) {
    if (uCloudMisc.y <= 0.001) return vec4(0.0, 0.0, 0.0, 1.0);
    float radius = uAtmRadii.x + uCloudMisc.z;
    float t0, t1;
    if (!RaySphere(ro, rd, radius, t0, t1)) return vec4(0.0, 0.0, 0.0, 1.0);
    float t = t0 > 0.0 ? t0 : t1;
    if (t <= 0.0 || t > uCloudMisc.x * 2.0) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 p = ro + rd * t;
    // Rotate into the wind frame so the streaks run along it.
    vec2 w = uCloudWindDir.xy;
    vec2 q = vec2(dot(p.xz, w), dot(p.xz, vec2(-w.y, w.x))) + uCloudMisc2.xy;
    // The sheet is seen at a grazing angle toward the horizon, which stretches a pixel's footprint.
    float footprint = t * pixelAngle / max(abs(dot(rd, normalize(p))), 0.05);
    float lod = log2(max(footprint * uCloudMisc.w * kWeatherTexels, 1.0));
    vec4 wm = textureLod(uCloudWeather, q * uCloudMisc.w, lod);
    vec4 wm2 = textureLod(uCloudWeather, q * uCloudMisc.w * 3.1 + 0.37, lod + log2(3.1));
    float field = wm.b * 0.7 + wm2.a * 0.3;
    float cov = uCloudMisc.y;
    float density = Saturate(Remap(field, 1.0 - cov, 1.0, 0.0, 1.0));
    density *= Saturate(Remap(wm2.b, 0.2, 0.8, 0.4, 1.0));
    if (density <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    // Thin ice: optical thickness below one, strongly forward scattering.
    float tau = density * 0.9;
    float T = exp(-tau);
    float c = dot(rd, uCloudLightDir.xyz);
    vec3 toLight = TransmittanceToSpace(ClampToAtmosphere(p), uCloudLightDir.xyz);
    float phase = mix(HenyeyGreenstein(0.7, c), HenyeyGreenstein(-0.1, c), 0.4);
    vec3 L = (lightRadianceTop * toLight * phase + ambientTop) * (1.0 - T);
    // Distant cirrus fades into the horizon haze.
    float fade = Saturate(Remap(dot(rd, normalize(ro)), 0.0, 0.08, 0.0, 1.0));
    return vec4(L * fade, mix(1.0, T, fade));
}

// March the cloud layer along ro + rd. `baseSteps` sets the quality; `jitter` in [0,1) offsets
// the start to trade banding for noise (resolved by temporal accumulation). `pixelAngle` is the
// angle one output pixel subtends (radians), which picks the noise mip at each distance.
// `ambTop` / `ambBottom` come from CloudAmbientColors().
CloudResult MarchClouds(vec3 ro, vec3 rd, int baseSteps, int lightSteps, float jitter, bool detail,
                        float pixelAngle, vec3 ambTop, vec3 ambBottom) {
    CloudResult res;
    res.radiance = vec3(0.0);
    res.transmittance = 1.0;
    res.depth = uCloudMisc.x;

    float Rb = uAtmRadii.x;
    float rIn = uCloudLayer.x, rOut = uCloudLayer.y;
    float camR = length(ro);

    // Clip the ray to the shell.
    float tStart, tEnd;
    float i0, i1, o0, o1;
    bool hitIn = RaySphere(ro, rd, rIn, i0, i1);
    bool hitOut = RaySphere(ro, rd, rOut, o0, o1);
    float g0, g1;
    bool hitGround = RaySphere(ro, rd, Rb, g0, g1) && g0 > 0.0;
    if (camR < rIn) {
        if (hitGround) return res;          // looking at the ground from below the clouds
        tStart = i1;
        tEnd = o1;
    } else if (camR > rOut) {
        if (!hitOut || o0 < 0.0) return res;
        tStart = o0;
        tEnd = (hitIn && i0 > 0.0) ? i0 : o1;
    } else {
        tStart = 0.0;
        tEnd = (hitIn && i0 > 0.0) ? i0 : o1;
    }
    tEnd = min(tEnd, tStart + uCloudMisc.x);
    if (tEnd <= tStart) return res;

    // More steps for long, grazing paths through the shell.
    float pathLen = tEnd - tStart;
    int steps = int(float(baseSteps) * mix(1.0, 2.0, Saturate(pathLen / (uCloudLayer.z * 6.0))));
    float dt = pathLen / float(steps);

    vec3 lightDir = uCloudLightDir.xyz;
    float cosTheta = dot(rd, lightDir);

    float T = 1.0;
    vec3 L = vec3(0.0);
    float depthWeight = 0.0, depthSum = 0.0;
    float t = tStart + dt * jitter;
    int emptySteps = 0;
    for (int i = 0; i < steps; ++i) {
        if (t >= tEnd) break;
        vec3 p = ro + rd * t;
        float footprint = max(t * pixelAngle, 1e-4);
        // Cheap probe first (a mip coarser); take full-detail samples only inside cloud.
        float probe = CloudDensity(p, false, max(footprint, ShapeTexelKm() * 2.0));
        if (probe <= 0.0) {
            ++emptySteps;
            t += dt * (emptySteps > 4 ? 1.5 : 1.0);
            continue;
        }
        emptySteps = 0;
        float density = detail ? CloudDensity(p, true, footprint) : probe;
        if (density > 0.0) {
            float h = Saturate((length(p) - rIn) / uCloudLayer.z);
            float od = LightOpticalDepth(p, lightSteps, footprint);
            // Sky-light occlusion: cloud above a point hides the sky from it, so interiors and
            // bases are darker than the tops - the shading that gives a cloud its volume. Two
            // cheap samples straight up.
            vec3 upDir = p / length(p);
            float upStep = uCloudLayer.z * 0.12;
            float odUp = (CloudDensity(p + upDir * upStep, false, max(footprint, ShapeTexelKm() * 2.0))
                        + CloudDensity(p + upDir * upStep * 3.0, false, max(footprint, ShapeTexelKm() * 4.0)) * 2.0) * upStep;
            vec3 lightRad = uCloudLightIllum.rgb * TransmittanceToSpace(ClampToAtmosphere(p), lightDir);
            vec3 amb = mix(ambBottom, ambTop, h) * mix(0.3, 1.0, exp(-odUp * 0.5));
            vec3 S = CloudLighting(od, cosTheta, density, lightRad, amb, h) * density;
            float sampleT = exp(-density * dt);
            vec3 Sint = (S - S * sampleT) / max(density, 1e-6);
            L += T * Sint;
            float absorbed = T * (1.0 - sampleT);
            depthSum += absorbed * t;
            depthWeight += absorbed;
            T *= sampleT;
            if (T < 0.005) { T = 0.0; break; }
        }
        t += dt;
    }
    res.radiance = L;
    res.transmittance = T;
    if (depthWeight > 1e-4) res.depth = depthSum / depthWeight;
    return res;
}

#endif // CLOUDS_COMMON_GLSL
