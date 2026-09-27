#version 460 core
// Renders the physical sky (atmosphere + clouds, no sun or moon disc - the directional light is
// the sun) into the environment cube that IblProbe convolves into ambient light and
// reflections. Face orientation matches IblProbe's kFaces table exactly; get it wrong and
// reflections come out mirrored.
#define ATMOSPHERE_LUTS
#include "AtmosphereCommon.glsl"
#include "CloudsCommon.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rgba16f) writeonly uniform imageCube uOut;

uniform int uSize;
uniform int uClouds;

const vec3 kForward[6] = vec3[](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 kRight[6]   = vec3[](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(-1, 0, 0));
const vec3 kUp[6]      = vec3[](vec3(0, -1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, -1, 0), vec3(0, -1, 0));

void main() {
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (id.x >= uSize || id.y >= uSize || id.z >= 6) return;
    vec2 p = (vec2(id.xy) + 0.5) / float(uSize) * 2.0 - 1.0;
    vec3 dir = normalize(kForward[id.z] + p.x * kRight[id.z] + p.y * kUp[id.z]);

    vec3 cam = ClampToAtmosphere(uAtmCamera.xyz);
    vec3 L = texture(uSkyViewSunLut, SkyViewUv(cam, dir, uAtmSunDir.xyz)).rgb
           + texture(uSkyViewMoonLut, SkyViewUv(cam, dir, uAtmMoonDir.xyz)).rgb;
    // Below the horizon the sky-view LUT holds the atmosphere between the camera and the ground
    // but not the ground itself; add the sun- and sky-lit ground so the lower hemisphere of the
    // ambient light isn't black (a scene's own floor still occludes it through SSAO/probes).
    float gHit = RaySphereNearest(cam, dir, vec3(0.0), uAtmRadii.x);
    if (gHit >= 0.0) {
        vec3 up = normalize(cam);
        vec3 skyZenith = texture(uSkyViewSunLut, SkyViewUv(cam, up, uAtmSunDir.xyz)).rgb
                       + texture(uSkyViewMoonLut, SkyViewUv(cam, up, uAtmMoonDir.xyz)).rgb;
        L += GroundRadiance(cam, dir, gHit, skyZenith * PI);
    }
    L *= uAtmRadii.z;

    if (uClouds == 1) {
        CloudResult c = MarchClouds(cam, dir, 24, 3, 0.5, false);
        L = L * c.transmittance + c.radiance * uAtmRadii.z;
    }
    imageStore(uOut, id, vec4(min(L, vec3(500.0)), 1.0));
}
