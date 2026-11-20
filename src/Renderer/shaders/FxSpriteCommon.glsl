// One flipbook particle as FxSprites uploads it (FxSpriteRenderer.cpp): shared by FxSprite.vert/.frag.
struct FxSprite {
    vec4 PosSize;  // xyz world, w height
    vec4 AxisRot;  // xyz axis (|axis| = length / height; 0 = camera-facing), w rotation
    vec4 Color;    // rgb linear, a opacity
    vec4 Params;   // x erosion, y softness, z aspect (w / h), w cell blend
    ivec4 Tex;     // x colour layer (-1 procedural), y normal layer (-1 none), z cell a, w cell b
    ivec4 Info;    // x cols, y rows, z mode (0 blood, 1 lit, 2 additive), w entry flags
    vec4 Extra;    // x smoothness, y seed
};
layout(std430, binding = 10) readonly buffer FxSprites { FxSprite uSprites[]; };
