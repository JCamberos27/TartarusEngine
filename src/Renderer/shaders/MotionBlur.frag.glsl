#version 460 core
// #162 - camera motion blur, by depth reprojection (Unity's Motion Blur with only camera motion).
//
// There is no velocity buffer: each pixel's world position is reconstructed from depth, projected
// with the PREVIOUS frame's view-projection, and the screen-space difference is the velocity. That
// captures camera translation and rotation - which is what a first-person view mostly needs - but
// NOT an object moving while the camera is still. Per-object blur needs a velocity target, which
// is a bigger change (see #162).
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform sampler2D uDepth;      // resolved depth, [0,1]
uniform mat4  uInvViewProj;    // this frame: clip -> world
uniform mat4  uPrevViewProj;   // last frame: world -> clip
uniform float uIntensity;      // 0..1, scales the sampled streak length
uniform int   uSamples;
uniform vec2  uTexel;
uniform float uMaxRadius;      // clamp in pixels, so a fast whip-pan can't smear the whole screen

void main() {
    vec3 here = texture(uHdr, vUV).rgb;
    float z = texture(uDepth, vUV).r;

    // Skybox / cleared depth: nothing to reproject against, and reconstructing from z=1 produces a
    // wildly wrong world position, so leave those pixels alone.
    if (z >= 1.0) { FragColor = vec4(here, 1.0); return; }

    vec4 clip = vec4(vUV * 2.0 - 1.0, z * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    world /= world.w;

    vec4 prevClip = uPrevViewProj * world;
    if (prevClip.w <= 0.0) { FragColor = vec4(here, 1.0); return; } // behind the previous camera
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    vec2 velocity = (vUV - prevUV) * uIntensity;

    // Clamp the streak so a fast rotation blurs strongly but stays bounded.
    float lenPx = length(velocity / uTexel);
    if (lenPx > uMaxRadius) velocity *= uMaxRadius / lenPx;
    if (lenPx < 0.5) { FragColor = vec4(here, 1.0); return; }   // still: skip the whole loop

    vec3 sum = here;
    float wsum = 1.0;
    for (int i = 1; i < uSamples; ++i) {
        float t = float(i) / float(uSamples - 1);       // 0..1 along the streak, trailing behind
        vec2 uv = vUV - velocity * t;
        if (uv != clamp(uv, vec2(0.0), vec2(1.0))) continue; // off-screen tap: don't drag an edge in
        sum += texture(uHdr, uv).rgb;
        wsum += 1.0;
    }
    FragColor = vec4(sum / wsum, 1.0);
}
