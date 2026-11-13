// Octahedral impostors (ImpostorRenderer.h): the fragment half shared by the depth program (pre-pass and sun cascades)
// and the lit program. It finds where the view ray meets each of the three blended frames, reads the atlases there,
// and puts the fragment at the blended surface's depth (gl_FragDepth). The pre-pass and the lit pass must agree on that
// depth to the bit (GL_EQUAL), so every step of it is `precise` and both programs call ImpostorResolve the same way.
// The includer declares uView (ModelShading.glsl does) and defines IMPOSTOR_HAS_VIEW, or this declares it.
#ifndef IMPOSTOR_HAS_VIEW
uniform mat4 uView;
#endif
uniform mat4 uProj;
uniform int uOrtho;
uniform mat4 uLightViewProj;
uniform float uRadius;
uniform int uFrames;
uniform float uImpCutoff;     // on the blended coverage
uniform float uMipAlphaScale; // coverage x (1 + mip x this): keeps the canopy's density as the atlas minifies
uniform float uDepthPush;     // metres pushed away from the viewer (the sun's cascades: the impostor's own shadow)
layout(binding = 0) uniform sampler2D uImpAlbedo;      // rgb albedo (sRGB), a coverage
layout(binding = 1) uniform sampler2D uImpNormalDepth; // rgb object-space normal, a depth (0 = the sphere's near side)
layout(binding = 2) uniform sampler2D uImpMask;        // r AO, g roughness, b translucency, a metallic

in vec3 vObj;
in vec3 vRay;
in vec3 vCardWorld;
in vec3 vEyeRay;
flat in vec3 vToViewerObj;
flat in vec3 vToViewerWorld;
flat in vec3 vFrameDir[3];
flat in vec3 vFrameRight[3];
flat in vec3 vFrameUp[3];
flat in vec2 vFrameCell[3];
flat in vec3 vWeights;
flat in vec4 vRot;
flat in float vScale;
flat in float vFade;

layout(depth_greater) out float gl_FragDepth;

struct ImpostorFrag {
    vec2 Uv[3];       // atlas coordinates, one per frame
    float Weight[3];  // barycentric weight x coverage (0: the ray misses that frame's image)
    float WeightSum;
    float Lod;        // the atlas mip here
    vec3 WorldPos;    // the surface (with the depth offset)
};

vec3 ImpostorQRot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// The LOD cross-fade's dither (ModelFragment.glsl's, the same pattern): false where this copy isn't drawn.
bool ImpostorDithered() {
    if (vFade == 0.0) return true;
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return vFade > 0.0 ? n < vFade : n >= -vFade;
}

// The fragment's frames and surface; false where the canopy has a hole (the caller discards). Writes gl_FragDepth.
bool ImpostorResolve(out ImpostorFrag o) {
    // Coverage first: most of a card is the gaps between branches, and they go before any other fetch.
    precise vec3 X[3];
    precise float alpha = 0.0;
    for (int k = 0; k < 3; ++k) {
        precise vec3 D = vFrameDir[k];
        precise float denom = dot(vRay, D);
        precise float t = -dot(vObj, D) / (abs(denom) > 1e-6 ? denom : 1e-6);
        X[k] = vObj + vRay * t; // on the frame's plane through the centre
        precise vec2 f = vec2(dot(X[k], vFrameRight[k]), dot(X[k], vFrameUp[k])) / uRadius * 0.5 + 0.5;
        bool inside = all(greaterThanEqual(f, vec2(0.0))) && all(lessThanEqual(f, vec2(1.0)));
        f = clamp(f, vec2(0.5 / 256.0), vec2(1.0 - 0.5 / 256.0));
        o.Uv[k] = (vFrameCell[k] + vec2(f.x, 1.0 - f.y)) / float(uFrames); // a frame's top row is its image's top
        float a = texture(uImpAlbedo, o.Uv[k]).a * (inside ? 1.0 : 0.0); // sampled either way: derivatives stay defined
        precise float w = vWeights[k] * a;
        o.Weight[k] = w;
        alpha += w;
    }
    // The mip, for every later fetch (they follow a discard, where implicit derivatives are undefined).
    o.Lod = max(textureQueryLod(uImpAlbedo, o.Uv[0]).y, 0.0);
    o.WeightSum = alpha;
    if (!ImpostorDithered()) return false; // after the fetches above: their derivatives need the whole quad
    // Coverage minifies toward grey and an alpha test then thins the canopy: scale it up with the mip.
    if (alpha * (1.0 + o.Lod * uMipAlphaScale) < uImpCutoff) return false;
    // The surface from the heaviest frame alone (the vertex stage puts it first): the blend's depth moved by
    // centimetres for twice the fetches.
    precise float depth = textureLod(uImpNormalDepth, o.Uv[0], o.Lod).a;
    precise vec3 Ps = X[0] + vFrameDir[0] * (uRadius * (1.0 - 2.0 * depth));
    // How far behind the card the surface lies, then that far along this pixel's own ray (keeps the depth monotone and
    // the fragment on its pixel).
    precise float behind = max(uRadius - dot(Ps, vToViewerObj), 0.0) * vScale + uDepthPush;
    precise vec3 ray = uOrtho == 1 ? -vToViewerWorld : normalize(vEyeRay);
    precise float slant = max(dot(ray, -vToViewerWorld), 0.2);
    o.WorldPos = vCardWorld + ray * (behind / slant);
    precise vec4 clip = uOrtho == 1 ? uLightViewProj * vec4(o.WorldPos, 1.0) : uProj * (uView * vec4(o.WorldPos, 1.0));
    gl_FragDepth = max(clip.z / clip.w * 0.5 + 0.5, gl_FragCoord.z);
    return true;
}
