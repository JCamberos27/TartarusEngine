#version 460 core
// Puddle-only screen-space reflections (PuddleSsr.h). Half resolution; one thread per output texel.
// Dry pixels leave after one depth fetch and the puddle noise (the same mask ApplyWetness floods), so a dry frame costs
// next to nothing. Puddle pixels march the reflected ray in screen space against this frame's depth pre-pass -
// linear steps, perspective-correct depth, then a binary refine - and fetch last frame's HDR colour at the hit,
// reprojected. Output: rgb = radiance, a = confidence (edge / distance / facing fades; 0 = missed, the probe or sky
// reflection stays).
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 32) uniform sampler2D uDepth;     // full-res depth (depth pre-pass, single sample)
layout(binding = 33) uniform sampler2D uPrevColor; // last frame's resolved HDR colour
layout(rgba16f, binding = 0) uniform writeonly image2D uOut;

uniform mat4 uProj;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform mat4 uPrevViewProj;
uniform float uPuddleAmount;
uniform float uPuddleScale;
uniform int   uSteps;        // linear steps (16-24)
uniform float uMaxDistance;  // metres along the ray
uniform int   uFrame;        // jitter seed

// ModelFragment.glsl's puddle noise, verbatim: the mask has to agree with the shading.
float WetHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float WetNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(WetHash(i), WetHash(i + vec2(1, 0)), u.x), mix(WetHash(i + vec2(0, 1)), WetHash(i + vec2(1, 1)), u.x), u.y);
}

ivec2 gDepthSize;
float DepthAt(vec2 uv) { return texelFetch(uDepth, clamp(ivec2(uv * vec2(gDepthSize)), ivec2(0), gDepthSize - 1), 0).r; }
vec3 ViewPos(vec2 uv, float d) {
    vec4 v = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}
float ViewZ(float d) { return -uProj[3][2] / ((d * 2.0 - 1.0) + uProj[2][2]); } // negative in front of the eye

// The surface normal from depth: on each axis the neighbour closer in depth, so silhouettes don't bend it.
vec3 DepthNormal(vec2 uv, vec3 p) {
    vec2 tx = 1.0 / vec2(gDepthSize);
    vec3 r = ViewPos(uv + vec2(tx.x, 0.0), DepthAt(uv + vec2(tx.x, 0.0))) - p;
    vec3 l = p - ViewPos(uv - vec2(tx.x, 0.0), DepthAt(uv - vec2(tx.x, 0.0)));
    vec3 u = ViewPos(uv + vec2(0.0, tx.y), DepthAt(uv + vec2(0.0, tx.y))) - p;
    vec3 d = p - ViewPos(uv - vec2(0.0, tx.y), DepthAt(uv - vec2(0.0, tx.y)));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(u.z) < abs(d.z) ? u : d;
    vec3 c = cross(dx, dy);
    if (!(dot(c, c) > 1e-20)) return vec3(0.0); // degenerate (no neighbours apart): no usable normal
    vec3 n = normalize(c);
    return dot(n, p) > 0.0 ? -n : n; // towards the eye
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uOut);
    if (any(greaterThanEqual(px, size))) return;
    gDepthSize = textureSize(uDepth, 0);
    vec2 uv = (vec2(px) + 0.5) / vec2(size);
    float d = DepthAt(uv);
    if (d >= 1.0) { imageStore(uOut, px, vec4(0.0)); return; }
    vec3 vp = ViewPos(uv, d);
    vec3 wp = (uInvView * vec4(vp, 1.0)).xyz;

    // Not a puddle (ApplyWetness: smoothstep(t, t + 0.04, n)), with a little margin for the half-res upsample.
    vec2 q = wp.xz / max(uPuddleScale, 0.05);
    float n = WetNoise(q) * 0.65 + WetNoise(q * 2.7 + 17.3) * 0.35;
    float t = 1.0 - uPuddleAmount * 0.75;
    if (n < t - 0.03) { imageStore(uOut, px, vec4(0.0)); return; }
    vec3 nv = DepthNormal(uv, vp);
    if (!((mat3(uInvView) * nv).y >= 0.7)) { imageStore(uOut, px, vec4(0.0)); return; } // puddles lie on flat ground

    vec3 rv = reflect(normalize(vp), nv);
    // Rays back at the camera leave through the near plane and see what the screen can't: fade them out.
    float facing = 1.0 - smoothstep(-0.1, 0.25, rv.z);
    if (facing <= 0.0) { imageStore(uOut, px, vec4(0.0)); return; }

    float len = uMaxDistance;
    if (vp.z + rv.z * len > -0.05) len = (-0.05 - vp.z) / rv.z;
    vec3 ev = vp + rv * len;
    vec4 h0 = uProj * vec4(vp, 1.0), h1 = uProj * vec4(ev, 1.0);
    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;
    vec2 s0 = h0.xy * k0 * 0.5 + 0.5, s1 = h1.xy * k1 * 0.5 + 0.5;
    // Clip the screen-space segment to the viewport so no step is spent off screen.
    vec2 ds = s1 - s0;
    float tMax = 1.0;
    if (ds.x > 1e-6) tMax = min(tMax, (1.0 - s0.x) / ds.x); else if (ds.x < -1e-6) tMax = min(tMax, -s0.x / ds.x);
    if (ds.y > 1e-6) tMax = min(tMax, (1.0 - s0.y) / ds.y); else if (ds.y < -1e-6) tMax = min(tMax, -s0.y / ds.y);
    tMax = clamp(tMax, 0.0, 1.0);
    float z0k = vp.z * k0, z1k = ev.z * k1;

    // Interleaved gradient noise: a per-pixel start offset TAA turns into a smooth result instead of bands.
    float jitter = fract(52.9829189 * fract(dot(vec2(px) + float(uFrame % 64) * vec2(5.588238, 5.588238), vec2(0.06711056, 0.00583715))));
    float prevT = 0.0, prevZ = vp.z;
    float hitT = -1.0;
    const int kMaxSteps = 32;
    int steps = clamp(uSteps, 4, kMaxSteps);
    for (int i = 1; i <= kMaxSteps; ++i) {
        if (i > steps) break;
        float tt = tMax * (float(i) - 1.0 + jitter) / float(steps);
        tt = max(tt, tMax * 0.02);
        vec2 s = mix(s0, s1, tt);
        float rayZ = mix(z0k, z1k, tt) / mix(k0, k1, tt);
        float sceneZ = ViewZ(DepthAt(s));
        float behind = sceneZ - rayZ; // > 0: the ray has passed behind the surface there
        float thickness = max(0.25, abs(rayZ - prevZ) * 1.5);
        if (behind > 0.0 && behind < thickness) {
            // Binary refine between the last step in front and this one.
            float a = prevT, b = tt;
            for (int r = 0; r < 5; ++r) {
                float m = 0.5 * (a + b);
                vec2 sm = mix(s0, s1, m);
                float rz = mix(z0k, z1k, m) / mix(k0, k1, m);
                if (ViewZ(DepthAt(sm)) - rz > 0.0) b = m; else a = m;
            }
            hitT = b;
            break;
        }
        prevT = tt;
        prevZ = rayZ;
    }
    if (hitT < 0.0) { imageStore(uOut, px, vec4(0.0)); return; }

    vec2 sh = mix(s0, s1, hitT);
    float dh = DepthAt(sh);
    if (dh >= 1.0) { imageStore(uOut, px, vec4(0.0)); return; }
    vec3 hv = ViewPos(sh, dh);
    // A surface facing away from the ray is a back side the depth buffer never held: a wrong hit.
    if (dot(DepthNormal(sh, hv), rv) > 0.0) { imageStore(uOut, px, vec4(0.0)); return; }
    // The ray rises off the water, so whatever it truly meets stands above the puddle. A hit on the ground around it
    // is the march passing under the street at a grazing angle - and reflecting the (wet, reflecting) ground fed the
    // last frame's puddles back into this one's until they glowed.
    vec3 hw = (uInvView * vec4(hv, 1.0)).xyz;
    if (hw.y < wp.y + 0.05) { imageStore(uOut, px, vec4(0.0)); return; }
    vec4 pc = uPrevViewProj * vec4(hw, 1.0);
    if (pc.w <= 0.0) { imageStore(uOut, px, vec4(0.0)); return; }
    vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
    if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) { imageStore(uOut, px, vec4(0.0)); return; }

    vec2 e = smoothstep(vec2(0.0), vec2(0.08), puv) * smoothstep(vec2(0.0), vec2(0.08), 1.0 - puv);
    vec2 eh = smoothstep(vec2(0.0), vec2(0.05), sh) * smoothstep(vec2(0.0), vec2(0.05), 1.0 - sh);
    float conf = facing * e.x * e.y * eh.x * eh.y * (1.0 - smoothstep(0.75, 1.0, hitT * len / uMaxDistance));
    vec3 col = min(textureLod(uPrevColor, puv, 0.0).rgb, vec3(32.0)); // no fireflies from a lamp's core
    // Never let a NaN or Inf through: the next frame would reflect it, and the one after that the reflection of it.
    if (any(isnan(col)) || any(isinf(col)) || isnan(conf) || isinf(conf)) { imageStore(uOut, px, vec4(0.0)); return; }
    imageStore(uOut, px, vec4(col, clamp(conf, 0.0, 1.0)));
}
