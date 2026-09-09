// Standard PBR shader — 7 texture maps + 7 scalar/color properties.
// This is the default surface shader used by the engine's PBR forward renderer.
// Properties{} declarations drive the material inspector and data-driven BindMaterial.
// The GLSL sources are unchanged from PR7 (zero-keyword variant = byte-identical GLSL).

Properties {
    _BaseColor         ("Base Color",       Color)     = (1, 1, 1)
    _Metallic          ("Metallic",         Float)     = 0
    _Roughness         ("Roughness",        Float)     = 0.5
    _EmissiveColor     ("Emissive Color",   Color)     = (0, 0, 0)
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
}

Keywords {
}

Vertex   { ModelVertex.glsl }
Fragment { ModelFragment.glsl }
