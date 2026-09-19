#version 460 core
#include "IblCubeDir.glsl"
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnvMap;
uniform float uEnvResolution; // #277 - base face size of uEnvMap, for the sample-footprint mip
uniform float uEnvRotation; // #108 — HDRI Y rotation (radians), same convention as SkyHdri.frag
vec3 RotateEnv(vec3 d) {
    float c = cos(uEnvRotation), s = sin(uEnvRotation);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

uniform float uRadianceClamp; // #277 — >0: cap the brightest channel (keeps hue); removes an HDRI's sun
vec3 ClampRadiance(vec3 c) {
    float m = max(c.r, max(c.g, c.b));
    return (uRadianceClamp > 0.0 && m > uRadianceClamp) ? c * (uRadianceClamp / m) : c;
}

const float PI = 3.14159265359;

void main() {
    vec3 N = FaceDirection(vUV);

    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float sampleCount = 0.0;
    const float kStep = 0.025;
    // #277 - read each sample from the mip whose texel covers about the sample's solid angle
    // (dPhi * dTheta), not the base level. Point-sampling a 1k face at ~4000 directions mostly
    // missed a small bright source like an HDRI's sun and hit it on a few texels only, giving
    // blotchy ambient; the mip average integrates its energy properly.
    float saTexel = 4.0 * PI / (6.0 * uEnvResolution * uEnvResolution);
    float lod = max(0.5 * log2((kStep * 4.0) * kStep / saTexel), 0.0);
    for (float phi = 0.0; phi < 2.0 * PI; phi += kStep * 4.0) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += kStep) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 dir = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += ClampRadiance(textureLod(uEnvMap, RotateEnv(dir), lod).rgb) * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }
    // The PI cancels the 1/PI of the Lambert BRDF, so the model shader multiplies this by
    // albedo directly. For a uniform environment of radiance L this returns exactly L.
    irradiance = PI * irradiance / max(sampleCount, 1.0);
    FragColor = vec4(irradiance, 1.0);
}
