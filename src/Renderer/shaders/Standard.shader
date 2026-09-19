// Standard PBR shader: the engine's default surface shader, laid out like Unity's Standard shader
// (Albedo, Metallic, Smoothness, Normal Map, Height Map, Occlusion, Emission, Tiling / Offset,
// Secondary Maps). Properties{} declarations drive the material inspector and data-driven
// BindMaterial; the order here is the order the Inspector shows them in.
//
// The advanced lobes (clear coat, anisotropy, sheen, subsurface, transmission) are [Hidden] here -
// Unity's Standard shader has none of them. StandardAdvanced.shader is the same GLSL with those
// shown; switch a material's Shader to it to use them. A hidden lobe a material already has set
// still renders (the variant is picked from the values, ShaderVariantKeyFor).
// The only copy (#104): materials reference it as engine://Standard.shader, and a project shader
// can use these stages the same way (Vertex { engine://ModelVertex.glsl }).

Properties {
    [Header(Main Maps)] [Tooltip(The base colour texture, tinted by Color.)] _AlbedoMap ("Albedo", Texture2D) = "white"
    [Tooltip(Tints the Albedo map, or is the surface colour when there is none.)] _BaseColor ("Color", Color) = (1, 1, 1)
    [Tooltip(Grayscale: white = metal. Multiplied by Metallic.)] _MetallicMap ("Metallic Map", Texture2D) = "white"
    [Tooltip(0 = non-metal such as wood, plastic or stone; 1 = bare metal.)] _Metallic ("Metallic", Range(0, 1)) = 0
    [Invert] [Tooltip(0 = rough and matte, 1 = mirror-smooth and glossy. Stored as Roughness = 1 - Smoothness.)] _Roughness ("Smoothness", Range(0, 1)) = 0.5
    [Tooltip(Grayscale: white = rough, black = smooth. Scales the roughness the Smoothness slider sets.)] _RoughnessMap ("Roughness Map", Texture2D) = "white"
    [Normal] [Tooltip(Fine surface detail such as bumps and grooves, without extra geometry.)] _NormalMap ("Normal Map", Texture2D) = "normal"
    [Tooltip(Scales the normal map's bumpiness. 0 = flat.)] _NormalStrength ("Normal Strength", Range(0, 2)) = 1
    [Tooltip(Grayscale height, white = high, for parallax: the surface appears to have depth.)] _HeightMap ("Height Map", Texture2D) = "white"
    [Tooltip(How deep the Height Map appears.)] _ParallaxScale ("Height Scale", Range(0, 0.1)) = 0.02
    [Tooltip(Ambient occlusion: darkens crevices and contact points.)] _AOMap ("Occlusion", Texture2D) = "white"
    [Header(Emission)] [Tooltip(Texture for glowing areas, tinted by Emission Color.)] _EmissiveMap ("Emission Map", Texture2D) = "black"
    [HDR] [Tooltip(The colour this surface glows, independent of scene lighting. Black = no emission.)] _EmissiveColor ("Emission Color", Color) = (0, 0, 0)
    [Tooltip(Brightness multiplier for the emission. Above 1 blooms.)] _EmissiveStrength ("Emission Intensity", Float) = 1
    [Header(Tiling and Offset)] [Tooltip(Repeats every map this many times across the mesh UVs.)] _UVTiling ("Tiling", Vec2) = (1, 1)
    _UVOffset             ("Offset",                Vec2)            = (0, 0)
    [Header(Secondary Maps)] [Tooltip(x2 detail: 50% grey leaves the colour unchanged, lighter brightens, darker darkens.)] _DetailAlbedoMap ("Detail Albedo x2", Texture2D) = "white"
    [Normal] [Tooltip(Fine detail blended on top of the Normal Map.)] _DetailNormalMap ("Detail Normal Map", Texture2D) = "normal"
    [Tooltip(How many times the detail maps repeat across the mesh UVs.)] _DetailTiling ("Detail Tiling", Vec2) = (4, 4)
    [Header(Advanced Options)] [Tooltip(Draw and light both sides: foliage, cloth, thin planes.)] _DoubleSided ("Double Sided", Bool) = 0
    [Tooltip(Multiply the colour and alpha by the mesh's vertex colours.)] _VertexColors ("Vertex Colors", Bool) = 0
    [Tooltip(For normal maps authored for DirectX, where green points down, e.g. from Unreal or Substance DX presets.)] _NormalFlipY ("Normal Map Is DirectX", Bool) = 0
    [Header(Forward Rendering Options)] [Tooltip(The shiny highlight lights leave on the surface. Off = fully matte under lights.)] _SpecularHighlights ("Specular Highlights", Bool) = 1
    [Tooltip(Reflections of the sky and reflection probes. Off = no reflections, whatever the Smoothness.)] _GlossyReflections ("Reflections", Bool) = 1

    // Engine-internal and advanced lobes: bound and saved as before, not shown.
    [Hidden] _Triplanar      ("Triplanar",        Bool)      = 0
    [Hidden] _TriplanarScale ("Triplanar Scale",  Float)     = 1
    [Hidden] _MetallicRoughnessMap ("Metallic Roughness", Texture2D) = "white"
    [Hidden] _ClearCoat           ("Clear Coat",           Range(0, 1))     = 0
    [Hidden] _ClearCoatRoughness  ("Clear Coat Roughness",  Range(0, 1))     = 0.5
    [Hidden] _ClearCoatMap        ("Clear Coat Map",        Texture2D) = "white"
    [Hidden] _Anisotropy          ("Anisotropy",            Range(-1, 1))     = 0
    [Hidden] _AnisotropyRotation  ("Anisotropy Rotation",   Range(0, 1))     = 0
    [Hidden] _Sheen               ("Sheen",                 Color)     = (0, 0, 0)
    [Hidden] _SheenRoughness      ("Sheen Roughness",        Range(0, 1))     = 0.5
    [Hidden] _SubsurfaceColor     ("Subsurface Color",       Color)     = (1, 0.8, 0.6)
    [Hidden] _Thickness           ("Thickness",              Range(0, 1))     = 0.5
    [Hidden] _ThicknessMap        ("Thickness Map",          Texture2D) = "white"
    [Hidden] _TransmissionStrength ("Transmission",          Range(0, 1))     = 0
    [Hidden] _IOR                  ("IOR",                   Range(1, 3))     = 1.5
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
