// Standard (Advanced): the same GLSL as Standard.shader with its extra lobes shown - clear coat,
// anisotropy, sheen, subsurface and transmission. Unity's built-in Standard shader has none of these
// (HDRP's Lit / StackLit do). Use it for car paint, brushed metal, velvet, skin / wax / jade, glass
// and water; everything else is simpler on Standard. Switching a material between the two keeps
// every value.

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
    [Header(Grunge)] [Tooltip(Grime laid over the whole surface in world space, ignoring the UVs: white leaves it clean, dark dirties and roughens it.)] _GrungeMap ("Grunge Map", Texture2D) = "white"
    [Tooltip(How much of the Grunge Map shows. 0 = off.)] _GrungeStrength ("Grunge Strength", Range(0, 1)) = 0
    [Tooltip(Grunge Map repeats per metre.)] _GrungeScale ("Grunge Tiling", Float) = 0.5
    [Header(Cover Layer)] [Tooltip(A second material (moss, snow) laid over the up-facing surfaces in world space, drawn only when Cover is above 0.)] _CoverStrength ("Cover", Range(0, 1)) = 0
    [Tooltip(Colour of the cover layer.)] _CoverAlbedoMap ("Cover Albedo", Texture2D) = "white"
    [Normal] [Tooltip(Surface detail of the cover layer.)] _CoverNormalMap ("Cover Normal", Texture2D) = "normal"
    [Tooltip(Green = occlusion, alpha = smoothness, red = height (used to break the edge up).)] _CoverMaskMap ("Cover Mask", Texture2D) = "white"
    [Tooltip(Cover tiles per metre, projected from above.)] _CoverScale ("Cover Tiling", Float) = 0.5
    [Tooltip(How level a surface must be before it starts to cover: 1 = only dead flat, 0 = also vertical.)] _CoverThreshold ("Cover Threshold", Range(0, 1)) = 0.6
    [Tooltip(How hard the cover edge is.)] _CoverSharpness ("Cover Sharpness", Range(0.5, 30)) = 8
    [Tooltip(How much the cover height breaks the edge up. 0 = a smooth edge.)] _CoverHeight ("Cover Height Blend", Range(0, 12)) = 3
    [Header(Fur)] [Tooltip(Shell fur: copies of the surface pushed out along its normal and cut into strands, drawn after it. For short hair and fuzz, not long locks.)] _FurEnabled ("Fur", Bool) = 0
    [Tooltip(Metres from the skin to the longest tips, where the Fur Mask is white.)] _FurLength ("Fur Length", Range(0, 0.1)) = 0.015
    [Tooltip(Strands across the 0-1 UV square. Higher = finer, denser hair.)] _FurDensity ("Fur Density", Range(10, 1000)) = 220
    [Tooltip(The most shells drawn, up close. Fewer are drawn as it gets smaller on screen, none far away.)] _FurShells ("Fur Shells", Int) = 16
    [Tooltip(Red channel: where fur grows and how long. Black = bare. None = everywhere, full length.)] _FurMask ("Fur Mask", Texture2D) = "white"
    [Tooltip(How far the tips droop toward the ground, as a share of the length.)] _FurGravity ("Fur Droop", Range(0, 1)) = 0.3
    [Tooltip(Multiplies the albedo for the hair.)] _FurTint ("Fur Tint", Color) = (1, 1, 1)
    [Tooltip(Light caught along the silhouette, where the hair is seen edge-on.)] _FurRim ("Fur Rim", Range(0, 2)) = 0.35
    [Header(Advanced Options)] [Tooltip(Draw and light both sides: foliage, cloth, thin planes.)] _DoubleSided ("Double Sided", Bool) = 0
    [Tooltip(Multiply the colour and alpha by the mesh's vertex colours.)] _VertexColors ("Vertex Colors", Bool) = 0
    [Tooltip(For normal maps authored for DirectX, where green points down, e.g. from Unreal or Substance DX presets.)] _NormalFlipY ("Normal Map Is DirectX", Bool) = 0
    [Header(Forward Rendering Options)] [Tooltip(The shiny highlight lights leave on the surface. Off = fully matte under lights.)] _SpecularHighlights ("Specular Highlights", Bool) = 1
    [Tooltip(Reflections of the sky and reflection probes. Off = no reflections, whatever the Smoothness.)] _GlossyReflections ("Reflections", Bool) = 1
    [Tooltip(Decals project onto this surface. Off for glass, screens and anything decals should skip.)] _ReceiveDecals ("Receive Decals", Bool) = 1
    [Tooltip(Stays dry under the scene's rain Wetness and Puddles. On for interiors and anything under cover.)] _NoWetness ("No Wetness", Bool) = 0
    [Header(Clear Coat)] [Tooltip(A thin glossy varnish layer over the base: car paint, lacquered wood. 0 = none.)] _ClearCoat ("Clear Coat", Range(0, 1)) = 0
    [Tooltip(Roughness of the varnish layer: 0 = mirror, 1 = matte.)] _ClearCoatRoughness ("Clear Coat Roughness", Range(0, 1)) = 0.5
    [Tooltip(Masks Clear Coat by its red channel.)] _ClearCoatMap ("Clear Coat Map", Texture2D) = "white"
    [Header(Anisotropy)] [Tooltip(Stretches highlights along one direction: brushed metal, hair, satin. 0 = none.)] _Anisotropy ("Anisotropy", Range(-1, 1)) = 0
    [Tooltip(Turns the stretch direction around the surface normal, 0..1 = 0..180 degrees.)] _AnisotropyRotation ("Anisotropy Rotation", Range(0, 1)) = 0
    [Header(Sheen)] [Tooltip(Soft rim glow of fabric fibres: velvet, felt, cloth. Black = none.)] _Sheen ("Sheen", Color) = (0, 0, 0)
    [Tooltip(How wide the sheen glow spreads.)] _SheenRoughness ("Sheen Roughness", Range(0, 1)) = 0.5
    [Header(Subsurface)] [Tooltip(The colour light takes on passing through the surface, used when Subsurface is ticked below.)] _SubsurfaceColor ("Subsurface Color", Color) = (1, 0.8, 0.6)
    [Tooltip(How thick the object is: thin lets more light through.)] _Thickness ("Thickness", Range(0, 1)) = 0.5
    [Tooltip(Scales Thickness by its red channel.)] _ThicknessMap ("Thickness Map", Texture2D) = "white"
    [Header(Transmission)] [Tooltip(How much of what is behind shows through, refracted: glass, water. Use the Transparent render mode.)] _TransmissionStrength ("Transmission", Range(0, 1)) = 0
    [Tooltip(Index of refraction: 1.33 water, 1.5 glass, 2.4 diamond.)] _IOR ("IOR", Range(1, 3)) = 1.5

    // Engine-internal: bound and saved as before, not shown.
    [Hidden] _Triplanar      ("Triplanar",        Bool)      = 0
    [Hidden] _TriplanarScale ("Triplanar Scale",  Float)     = 1
    [Hidden] _MetallicRoughnessMap ("Metallic Roughness", Texture2D) = "white"
}

Keywords {
    _CLEARCOAT
    _ANISO
    _SHEEN
    _SUBSURFACE
    _TRANSMISSION
    _REFLECTION_PROBES
    _COVER
}

Vertex   { ModelVertex.glsl }
Fragment { ModelFragment.glsl }
