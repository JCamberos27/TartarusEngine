// Project-local copy of src/Renderer/shaders/Standard.shader — keep in sync.
// Referenced by the #354 material fixtures (project/materials/*.mat, tests/smoke-scenes/smoke_materials.json).
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
    // PR10: Clear Coat
    _ClearCoat           ("Clear Coat",           Float)     = 0
    _ClearCoatRoughness  ("Clear Coat Roughness",  Float)     = 0.5
    [Hidden] _ClearCoatMap ("Clear Coat Map",      Texture2D) = "white"
    // PR10: Anisotropy
    _Anisotropy          ("Anisotropy",            Float)     = 0
    _AnisotropyRotation  ("Anisotropy Rotation",   Float)     = 0
    // PR11: Sheen/cloth
    _Sheen               ("Sheen",                 Color)     = (0, 0, 0)
    _SheenRoughness      ("Sheen Roughness",        Float)     = 0.5
    // PR11: Subsurface translucency
    _SubsurfaceColor     ("Subsurface Color",       Color)     = (1, 0.8, 0.6)
    _Thickness           ("Thickness",              Float)     = 0.5
    [Hidden] _ThicknessMap ("Thickness Map",        Texture2D) = "white"
    // PR12: Transmission + refraction
    _TransmissionStrength ("Transmission",          Float)     = 0
    _IOR                  ("IOR",                   Float)     = 1.5
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
