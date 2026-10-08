// Red-dot portion of ScopesPackage/Holo Sub Graph, recreated for Tartarus.
// The reticle is projected from tangent-space eye direction; lens geometry clips it.
Properties {
    [Header(Reticle)] _ReticleMap ("Reticle", Texture2D) = "white"
    [HDR] _ReticleColor ("Reticle Color", Color) = (1, 0, 0)
    _ReticleBrightness ("Reticle Brightness", Range(0, 30)) = 5
    [Tooltip(Angular projection size. This uses the Unity graph's reciprocal size calculation.)] _ReticleSize ("Reticle Size", Range(0.01, 2)) = 0.2
    _ReticleTiling ("Reticle Tiling", Vec2) = (1, 1)
    _ReticleOffset ("Reticle Offset", Vec2) = (0, 0)
    _UseTextureColor ("Use Texture Color", Bool) = 0
    [Header(Projection)] [Tooltip(Use a consistent lens plane for curved glass meshes.)] _FlatProjection ("Flat Lens Projection", Bool) = 0
    _ProjectionNormal ("Lens Normal (model space)", Vec3) = (0, 0, 1)
    _ProjectionUp ("Lens Up (model space)", Vec3) = (0, 1, 0)
    [Header(Glass)] _GlassTint ("Glass Tint", Color) = (0, 0.0646, 0.1415)
    _GlassOpacity ("Glass Opacity", Range(0, 1)) = 0.12
    [Header(Blur)] _BlurReticle ("Blur Reticle", Bool) = 0
    _BlurDistance ("Blur Distance", Range(0, 2)) = 0.29
    _BlurRange ("Blur Range", Range(0, 2)) = 1.248
    _BlurSamples ("Blur Samples", Int) = 8
}
Cull Off
ZWrite Off
ZTest LEqual
Blend One OneMinusSrcAlpha
Queue Transparent
Vertex { ModelVertex.glsl }
Fragment { RedDot.frag.glsl }
