// Standard PBR shader — 7 texture maps + 7 scalar/color properties.
// This is the default surface shader used by the engine's PBR forward renderer.
// Properties{} declarations drive the material inspector and data-driven BindMaterial.
// The GLSL sources are unchanged from PR7 (zero-keyword variant = byte-identical GLSL).
// The only copy (#104): materials reference it as engine://Standard.shader, and a project shader
// can use these stages the same way (Vertex { engine://ModelVertex.glsl }).

Properties {
    _BaseColor         ("Base Color",       Color)     = (1, 1, 1)
    _Metallic          ("Metallic",         Range(0, 1))     = 0
    _Roughness         ("Roughness",        Range(0, 1))     = 0.5
    [HDR] _EmissiveColor ("Emissive Color",   Color)     = (0, 0, 0)
    _EmissiveStrength  ("Emissive Strength",Float)     = 1
    [Hidden] _Triplanar      ("Triplanar",        Bool)      = 0
    [Hidden] _TriplanarScale ("Triplanar Scale",  Float)     = 1
    _AlbedoMap         ("Albedo",           Texture2D) = "white"
    _NormalMap         ("Normal",           Texture2D) = "normal"
    [Hidden] _MetallicRoughnessMap ("Metallic Roughness", Texture2D) = "white"
    _MetallicMap       ("Metallic Map",     Texture2D) = "white"
    _RoughnessMap      ("Roughness Map",    Texture2D) = "white"
    _AOMap             ("AO",               Texture2D) = "white"
    _EmissiveMap       ("Emissive",         Texture2D) = "black"
    // PR10: Clear Coat
    _ClearCoat           ("Clear Coat",           Range(0, 1))     = 0
    _ClearCoatRoughness  ("Clear Coat Roughness",  Range(0, 1))     = 0.5
    // #206: masks Clear Coat by its red channel
    _ClearCoatMap        ("Clear Coat Map",        Texture2D) = "white"
    // PR10: Anisotropy
    _Anisotropy          ("Anisotropy",            Range(-1, 1))     = 0
    _AnisotropyRotation  ("Anisotropy Rotation",   Range(0, 1))     = 0
    // PR11: Sheen/cloth
    _Sheen               ("Sheen",                 Color)     = (0, 0, 0)
    _SheenRoughness      ("Sheen Roughness",        Range(0, 1))     = 0.5
    // PR11: Subsurface translucency
    _SubsurfaceColor     ("Subsurface Color",       Color)     = (1, 0.8, 0.6)
    _Thickness           ("Thickness",              Range(0, 1))     = 0.5
    // #206: scales Thickness by its red channel
    _ThicknessMap        ("Thickness Map",          Texture2D) = "white"
    // PR12: Transmission + refraction
    _TransmissionStrength ("Transmission",          Range(0, 1))     = 0
    _IOR                  ("IOR",                   Range(1, 3))     = 1.5
    // #102 / #113 — surface options
    [Header(Surface Options)] [Tooltip(Repeats every map this many times across the mesh UVs.)] _UVTiling ("Tiling", Vec2) = (1, 1)
    _UVOffset             ("Offset",                Vec2)            = (0, 0)
    [Tooltip(Scales the normal map's bumpiness. 0 = flat.)] _NormalStrength ("Normal Strength", Range(0, 2)) = 1
    [Tooltip(For normal maps authored for DirectX (green channel points down), e.g. from Unreal or Substance DX presets.)] _NormalFlipY ("Normal Map Is DirectX", Bool) = 0
    [Tooltip(Draw and light both sides (foliage, cloth, thin planes).)] _DoubleSided ("Double Sided", Bool) = 0
    [Tooltip(Multiply the colour (and alpha) by the mesh's vertex colours.)] _VertexColors ("Vertex Colors", Bool) = 0
    [Tooltip(Grayscale height (white = high) for parallax occlusion mapping.)] _HeightMap ("Height", Texture2D) = "white"
    _ParallaxScale        ("Parallax Scale",        Range(0, 0.1))   = 0.02
    [Header(Detail Maps)] [Tooltip(x2 detail: 50% grey leaves the colour unchanged, lighter brightens, darker darkens.)] _DetailAlbedoMap ("Detail Albedo", Texture2D) = "white"
    _DetailNormalMap      ("Detail Normal",         Texture2D)       = "normal"
    _DetailTiling         ("Detail Tiling",         Vec2)            = (4, 4)
}

Keywords {
    _CLEARCOAT
    _ANISO
    _SHEEN
    _SUBSURFACE
    _TRANSMISSION
    _REFLECTION_PROBES
}

Vertex   { ModelVertex.glsl }
Fragment { ModelFragment.glsl }
