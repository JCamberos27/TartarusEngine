#version 460 core
// Octahedral impostors in the sun's cascades (ImpostorRenderer::DrawSun): coverage only, on a card
// through the sphere's centre facing the sun (Impostor.vert.glsl, uOrtho). No depth offset: the fragment keeps the
// card's own depth, so early depth testing stays on - the canopy's near half then lights itself and its far half
// sits in its shadow, which is what the offset gave, at a sixth of the fetches.
uniform float uRadius;
uniform int uFrames;
uniform float uImpCutoff;
uniform float uMipAlphaScale;
layout(binding = 0) uniform sampler2D uImpAlbedo; // a coverage

in vec3 vObj;
in vec3 vRay;
flat in vec3 vFrameDir[3];
flat in vec3 vFrameRight[3];
flat in vec3 vFrameUp[3];
flat in vec2 vFrameCell[3];

// The heaviest frame only (Impostor.vert.glsl puts it first): the sun hardly moves, so a tree's shadow keeps one frame,
// and one coverage fetch halved this pass.
void main() {
    vec3 D = vFrameDir[0];
    float denom = dot(vRay, D);
    vec3 X = vObj + vRay * (-dot(vObj, D) / (abs(denom) > 1e-6 ? denom : 1e-6));
    vec2 f = vec2(dot(X, vFrameRight[0]), dot(X, vFrameUp[0])) / uRadius * 0.5 + 0.5;
    bool inside = all(greaterThanEqual(f, vec2(0.0))) && all(lessThanEqual(f, vec2(1.0)));
    vec2 uv = (vFrameCell[0] + vec2(clamp(f.x, 0.0, 1.0), 1.0 - clamp(f.y, 0.0, 1.0))) / float(uFrames);
    float alpha = texture(uImpAlbedo, uv).a * (inside ? 1.0 : 0.0); // fetched either way: derivatives stay defined
    float lod = max(textureQueryLod(uImpAlbedo, uv).y, 0.0);
    if (alpha * (1.0 + lod * uMipAlphaScale) < uImpCutoff) discard;
}
