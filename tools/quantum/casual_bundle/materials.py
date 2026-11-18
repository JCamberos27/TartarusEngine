"""Unreal material JSON (export) -> engine .mat for the Quantum Modular Casual Bundle (docs/QUANTUM_SWAP.md, Phase 1
step 3).

API
  classify(ue_json) -> "master" | "hair" | "facehair" | "eye" | "other"
  plan_material(ue_json, texture_repo_paths, name, baked_repo_path=None, texture_srgb=None,
                master_double_sided=False) -> MaterialPlan
      What the .mat needs: which exported textures go in which slot (and their cook kind), which bake (if any)
      produces the albedo, the factors, and every UE parameter it could not represent (warnings).
  execute_bake(plan, source_file, dst_abs, cap, force=False) -> CookResult | None
      Runs the plan's bake through textures.py into dst_abs. source_file(ue_texture_pkg) -> exported file Path.
  build_mat(plan, texture_guids, has_alpha) -> dict
      The .mat JSON. texture_guids: repo path -> GUID (from textures.write_texture_meta); has_alpha: whether the
      cooked/baked albedo kept an alpha channel (CookResult.has_alpha).
  write_mat(path, mat_dict, keep_guid=True) -> guid
      Writes <name>.mat (common.write_json) and its .mat.meta, keeping an existing GUID.

Typical caller loop:
  plan = plan_material(ue, tex_paths, "M_Hoodie_Blue")
  for t in plan.textures: res[t.repo_path] = cook_texture(src(t.ue_texture), repo_abs(t.repo_path), cap, t.kind)
  if plan.bake: res[plan.bake.repo_path] = execute_bake(plan, src, repo_abs(plan.bake.repo_path), cap)
  guids = {p: write_texture_meta(repo_abs(p), kind, cap) for each cooked/baked p}
  write_mat(repo_abs(mat_path), build_mat(plan, guids, res[plan.albedo_path].has_alpha))

How UE maps (from a UE 5.8 MaterialEditingLibrary dump of the master graphs; see textures.py for the bake maths)
- M_Quantum_Master_Material (Masked, two-sided, DefaultLit; 393 MIs):
    BaseColor = lerp(pow(desat(BaseColor.rgb, 1-Saturation) * Brightness, Contrast), ColorCorrection, CCValue)
    Metallic  = ORM.b * Metallic Intensity        Roughness = pow(ORM.g * Roughness Intensity, Roughness Contrast)
    AO        = ORM.r                             Normal    = FlattenNormal(Normal, 1 - Normal Strength)
    OpacityMask = BaseColor.a (Custom_Alpha_Map on) or Alpha_Map.r (off); clip value = UE default 0.3333 (the
    .uasset does not serialise OpacityMaskClipValue, so it is the default); Emissive = 0 (switch on in every MI).
  -> _AlbedoMap BaseColor (or the bake), _MetallicRoughnessMap + _AOMap = ORM (glTF G = rough, B = metal; AO reads
     .r: ModelFragment.glsl:216-230), _NormalMap + _NormalFlipY (UE normals are DirectX), _NormalStrength,
     _Metallic = Metallic Intensity, _Roughness = Roughness Intensity, factorsScaleMaps (factor * map, #102).
     Brightness alone (<= 1) becomes _BaseColor [b, b, b] (exact: the engine multiplies linear albedo by it,
     ModelFragment.glsl:206); any Saturation / Contrast / Color Correction, or Brightness > 1 (UE clamps the
     result to 1, the engine would not), is baked into a new albedo instead and _BaseColor stays 1.
- Hair masters (Masked, two-sided, Hair shading model): no normal/ORM maps. Albedo = baked (textures.bake_hair_albedo),
  _Roughness = clamp(Roughness), _Metallic 0 (the hair model's "Metallic" pin is Scatter, not metalness).
- Eye / teeth / lacrimal / eye-occlusion MIs use MetaHuman shaders: "eye", no .mat (the engine's own are kept).

Engine facts that shape the output (src/Renderer):
- Alpha cut-out needs renderQueue 1 (AlphaTest); alphaCutoff alone does nothing (MaterialAsset.cpp:251:
  AlphaClip = RenderQueue == AlphaTest). The cut-out reads the albedo map's alpha (ModelFragment.glsl:205), raw.
  The shadow pass honours it too (ShadowDepth.frag.glsl:10). There is no alpha-blended hair path used here.
- Outfit clothing is ALREADY drawn double-sided by the scene renderer (SceneRenderer.cpp:990,1059: DoubleSided =
  clothing) with its back faces darkened as a garment interior (albedo * 0.3, ao * 0.4; ModelFragment.glsl:233) -
  but only when the material is NOT _DoubleSided (Model.cpp:2878, "not hair cards"). So the master maps to
  _DoubleSided false by default (garment insides stay dark, as the existing Quantum .mat files have it) although
  UE's master is two-sided; pass master_double_sided=True for a non-outfit use. Hair/facial hair: _DoubleSided true.
- No factor is clamped on load or in the shader (_BaseColor, _Roughness > 1 go straight into lighting), so the
  importer never writes factors above 1.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from .common import existing_guid, meta_path, new_guid, read_json, write_json
from . import textures

SHADER = "engine://Standard.shader"
MASTER_CUTOFF = 0.3333     # UE's default OpacityMaskClipValue (not serialised in M_Quantum_Master_Material)
HAIR_CUTOFF = 0.35         # like M_Brows_Lashes; UE's hair masters also use the default 0.3333 (not serialised)

MASTER_BASES = {"M_Quantum_Master_Material"}
HAIR_BASES = {"M_Hair_Master", "M_F_Hair_Master"}
FACEHAIR_BASES = {"M_FaceHair_Master", "M_F_FaceHair_Master"}
EYE_BASES = {"M_Eye", "M_F_Eye", "M_EyeOcclusion", "M_F_EyeOcclusion", "M_lacrimal_fluid", "M_F_lacrimal_fluid",
             "M_Teeth", "M_F_Teeth", "M_Cartilage", "M_F_Cartilage", "M_Eye_Sclera", "M_Eyeball"}

# UE textures imported with sRGB OFF among those a bake reads (UE 5.8 dump of every MI-referenced texture). Every
# other bake source (all BaseColor maps, the other hair masks/gradients/IDs) is sRGB. Pass texture_srgb to override.
UE_LINEAR_TEXTURES = {
    "/Game/Casual_Wear_Bundle/Textures/Hair/Beard/T_Beard_root",
    "/Game/Casual_Wear_Bundle/Textures/Hair/Beard/T_Beard_Opacity",
    "/Game/Casual_Wear_Bundle/Textures/Cloth/Jacket_Winter/Jacket_Winter_Fur/T_FurCollar_Depth",
}

# --- parameters each family understands. Consumed = mapped/baked; ignored = no engine equivalent and no effect on
# --- the static look (listed in plan.ignored); anything else non-default lands in plan.warnings.
MASTER_SCALARS_CONSUMED = {"Brightness", "Saturation", "Contrast", "Color Correction Value", "Roughness Intensity",
                           "Normal Strength", "Metallic Intensity", "Roughness Contrast"}
MASTER_SCALARS_IGNORED = {"RefractionDepthBias"}          # no refraction in a Masked material
MASTER_VECTORS_CONSUMED = {"Color Correction"}
MASTER_TEXTURES_CONSUMED = {"BaseColor", "Normal", "ORM", "Alpha_Map"}
# Effective-texture params that are only read when a static switch is off (all switches are on in every MI).
MASTER_TEXTURES_SWITCHED = {"Emissive": "Emissive", "AO_Texture": "AO", "Roughness_Texture": "Roughness",
                            "Metallic_Texture": "Metallic"}   # param -> the switch that must be ON to ignore it
MASTER_SWITCH_EXPECTED = {"Emissive": True, "AO": True, "Metallic": True, "Roughness": True}  # Custom_Alpha_Map: both

HAIR_SCALARS_CONSUMED = {"Brightness", "Roughness", "RandomValueVariation", "Noisetiling"}
HAIR_SCALARS_IGNORED = {
    "Scatter", "LowQualityScatter", "EdgeMaskContrast", "EdgeMaskMin",          # hair shading / view-dependent fade
    "MipBias", "PixelDepthOffset", "PixelDepthOffset_Root", "OcclusionAmtRoot", "OcclusionAmtRootTip", "TipPower",
    "WindIntensity", "WindSpeed", "WindWeight", "RefractionDepthBias", "Specular", "SpecMaskMin", "SpecMaskMax",
    "SpecMaskBias", "AO_Min", "AO_Max", "AO_power", "AO_FacingExponent", "AODepthMasking", "AO_DepthMax",
    "Visibility", "Opacity",
}
HAIR_VECTORS_CONSUMED = {"RootColor", "TipColor"}
HAIR_VECTORS_IGNORED = {"Tangent", "TangentA", "TangentB"}                       # hair-model tangents
HAIR_TEXTURES_CONSUMED = {"Alpha", "T_Root", "Root", "Unique_Hair_Value"}
HAIR_TEXTURES_IGNORED = {"Depth", "FlowMapTexture", "Wind_Gradient", "AO_2ndUV", "2ndUVRootColor",
                         "2ndUVTipColor", "Diffuse", "DyeMask", "Color Mask_1", "Color Mask_2"}
# Static switches that change the BaseColor/OpacityMask chain: the value the bake assumes.
HAIR_SWITCH_EXPECTED = {"isDyed": False, "UseDiffuseBaseColor": False, "Use2ndChannelUVForBaseColorTextures": False,
                        "UV2ColorBlend": False, "BlendColorsWithParameter": False, "isHideable": False,
                        "HideHair": False, "isEmissive": False, "Uses Hair Flames": False, "isMultiColorShift": False}


@dataclass
class TextureUse:
    slot: str                 # engine property, e.g. "_AlbedoMap"
    ue_texture: str           # UE package path of the exported texture
    repo_path: str            # where the cooked copy goes
    kind: str                 # textures.KINDS: color | normal | data


@dataclass
class Bake:
    kind: str                 # "adjusted" (master albedo) | "hair"
    repo_path: str            # baked albedo destination (repo path)
    sources: dict             # role -> UE texture package ("base", "alpha_map" | "alpha", "root", "id")
    params: dict              # keyword arguments for the textures.bake_* function (colours, scalars, *_srgb)


@dataclass
class MaterialPlan:
    ue_name: str
    family: str                                       # classify() result
    name: str                                         # engine material name
    supported: bool = True                            # False for eye/other: no .mat is built
    textures: list[TextureUse] = field(default_factory=list)   # maps to cook (bake sources are not listed)
    bake: Bake | None = None
    albedo_path: str = ""                             # repo path that ends up in _AlbedoMap (cooked or baked)
    base_color: list[float] = field(default_factory=lambda: [1.0, 1.0, 1.0])
    metallic: float = 0.0
    roughness: float = 1.0
    normal_strength: float = 1.0
    normal_flip_y: bool = False
    double_sided: bool = False
    alpha_cutoff: float = MASTER_CUTOFF               # used only when the albedo has alpha
    warnings: list[str] = field(default_factory=list)  # UE params/switches the .mat cannot represent: review
    ignored: list[str] = field(default_factory=list)   # known params with no static-look effect (informational)


# ------------------------------------------------------------------------------------------------- helpers ---
def _leaf(pkg: str | None) -> str:
    return (pkg or "").rstrip("/").rsplit("/", 1)[-1]


def classify(ue_json: dict, ue_name: str = "") -> str:
    """"master" | "hair" | "facehair" | "eye" | "other", from the MI's base material. A base Material's JSON has no
    base_material (nor its own name), so pass its asset name as ue_name; the masters themselves are "other"
    (only their instances become .mat files)."""
    if ue_json.get("class") == "Material":
        return "eye" if ue_name in EYE_BASES else "other"
    base = _leaf(ue_json.get("base_material"))
    if base in MASTER_BASES:
        return "master"
    if base in HAIR_BASES:
        return "hair"
    if base in FACEHAIR_BASES:
        return "facehair"
    if base in EYE_BASES:
        return "eye"
    return "other"


def _effective_textures(ue: dict) -> dict:
    """Texture params as the MI renders them: the MI's overrides, falling back to the inherited defaults
    (all_texture_params). An override of None means "no texture" in UE, so the inherited value is used."""
    eff = dict(ue.get("all_texture_params") or {})
    for k, v in (ue.get("textures") or {}).items():
        if v:
            eff[k] = v
    return eff


def _srgb(pkg: str, overrides: dict | None) -> bool:
    if overrides and pkg in overrides:
        return bool(overrides[pkg])
    return pkg not in UE_LINEAR_TEXTURES


def _num(v) -> str:
    return f"{v:.4g}" if isinstance(v, (int, float)) else repr(v)


def _check_params(plan: MaterialPlan, ue: dict, scalars_ok: set, scalars_ignored: set, vectors_ok: set,
                  vectors_ignored: set, textures_ok: set, textures_ignored: set) -> None:
    """Sort every overridden parameter into consumed / ignored / warning."""
    for k, v in (ue.get("scalars") or {}).items():
        if k in scalars_ok:
            continue
        if k in scalars_ignored:
            plan.ignored.append(f"scalar {k}={_num(v)}")
        else:
            plan.warnings.append(f"unknown scalar {k}={_num(v)}")
    for k, v in (ue.get("vectors") or {}).items():
        if k in vectors_ok:
            continue
        if k in vectors_ignored:
            plan.ignored.append(f"vector {k}={[round(x, 4) for x in v]}")
        else:
            plan.warnings.append(f"unknown vector {k}={[round(x, 4) for x in v]}")
    for k, v in (ue.get("textures") or {}).items():
        if k in textures_ok:
            continue
        if k in textures_ignored:
            plan.ignored.append(f"texture {k}={_leaf(v) or None}")
        else:
            plan.warnings.append(f"unknown texture {k}={v}")


def _repo(plan: MaterialPlan, texture_repo_paths: dict, pkg: str, what: str) -> str:
    p = texture_repo_paths.get(pkg, "")
    if not p:
        plan.warnings.append(f"{what}: no repo path for {pkg} (slot left empty)")
    return p


def default_baked_path(near_repo_path: str, ue_name: str, suffix: str) -> str:
    """A baked texture next to its main source: <folder>/T_<MI name without MI_>_<suffix>.png."""
    stem = ue_name[3:] if ue_name.startswith("MI_") else ue_name
    folder = near_repo_path.rsplit("/", 1)[0] if "/" in near_repo_path else ""
    return f"{folder}/T_{stem}_{suffix}.png" if folder else f"T_{stem}_{suffix}.png"


# ---------------------------------------------------------------------------------------------- planning ---
def plan_material(ue_json: dict, texture_repo_paths: dict, name: str, ue_name: str = "",
                  baked_repo_path: str | None = None, texture_srgb: dict | None = None,
                  master_double_sided: bool = False) -> MaterialPlan:
    """Plan one engine .mat from one exported UE material JSON.

    texture_repo_paths: UE texture package -> repo path where cook_texture will put it (bake sources need an entry
      too: the baked file defaults to the folder of its main source).
    name: the engine material name (the .mat stem). ue_name: the UE asset name, for messages / default bake names.
    baked_repo_path: where a bake writes its albedo (default: default_baked_path(...)).
    texture_srgb: UE texture package -> whether UE imported it as sRGB (default: UE_LINEAR_TEXTURES are linear).
    """
    family = classify(ue_json, ue_name)
    plan = MaterialPlan(ue_name=ue_name or name, family=family, name=name)
    if family == "master":
        _plan_master(plan, ue_json, texture_repo_paths, baked_repo_path, texture_srgb, master_double_sided)
    elif family in ("hair", "facehair"):
        _plan_hair(plan, ue_json, texture_repo_paths, baked_repo_path, texture_srgb)
    else:
        plan.supported = False
        base = _leaf(ue_json.get("base_material")) or f"itself, a {ue_json.get('class', '?')}"
        plan.warnings.append(
            f"{family} material (base {base}): no engine mapping; keep the engine's existing material"
            if family == "eye" else f"unsupported material (base {base}, blend {ue_json.get('blend_mode')}): map by hand")
    return plan


def _plan_master(plan: MaterialPlan, ue: dict, paths: dict, baked: str | None, srgb: dict | None,
                 double_sided: bool) -> None:
    s = ue.get("scalars") or {}
    v = ue.get("vectors") or {}
    sw = ue.get("static_switches") or {}
    tex = _effective_textures(ue)
    _check_params(plan, ue, MASTER_SCALARS_CONSUMED, MASTER_SCALARS_IGNORED, MASTER_VECTORS_CONSUMED, set(),
                  MASTER_TEXTURES_CONSUMED, set())
    for k, want in MASTER_SWITCH_EXPECTED.items():
        if sw.get(k, want) != want:
            param = next(p for p, s_ in MASTER_TEXTURES_SWITCHED.items() if s_ == k)
            plan.warnings.append(f"static switch {k}={sw.get(k)}: UE reads {param}={_leaf(tex.get(param))} "
                                 f"instead of the ORM/constant; not mapped")
    for k in sw:
        if k not in MASTER_SWITCH_EXPECTED and k != "Custom_Alpha_Map":
            plan.warnings.append(f"unknown static switch {k}={sw[k]}")

    brightness = float(s.get("Brightness", 1.0))
    saturation = float(s.get("Saturation", 1.0))
    contrast = float(s.get("Contrast", 1.0))
    cc_amount = float(s.get("Color Correction Value", 0.0))
    cc_color = tuple((v.get("Color Correction") or [0.0, 0.0, 0.0, 1.0])[:3])
    custom_alpha_off = sw.get("Custom_Alpha_Map", True) is False

    base_pkg, normal_pkg, orm_pkg = tex.get("BaseColor"), tex.get("Normal"), tex.get("ORM")
    if not base_pkg:
        plan.warnings.append("no BaseColor texture")
    base_repo = _repo(plan, paths, base_pkg, "BaseColor") if base_pkg else ""

    needs_bake = (saturation != 1.0 or contrast != 1.0 or cc_amount != 0.0 or brightness > 1.0 or custom_alpha_off)
    if needs_bake and base_pkg:
        sources = {"base": base_pkg}
        params = {"saturation": saturation, "contrast": contrast, "brightness": brightness,
                  "cc_color": cc_color, "cc_amount": cc_amount}
        if custom_alpha_off:
            amap = tex.get("Alpha_Map")
            sources["alpha_map"] = amap
            params["alpha_src_srgb"] = _srgb(amap, srgb)
        dst = baked or default_baked_path(base_repo or "", plan.ue_name, "BaseColor_Baked")
        plan.bake = Bake("adjusted", dst, sources, params)
        plan.albedo_path = dst
    elif base_pkg:
        plan.textures.append(TextureUse("_AlbedoMap", base_pkg, base_repo, "color"))
        plan.albedo_path = base_repo
        plan.base_color = [brightness] * 3            # <= 1 here: exact, no clamp involved

    if normal_pkg:
        plan.textures.append(TextureUse("_NormalMap", normal_pkg, _repo(plan, paths, normal_pkg, "Normal"), "normal"))
        plan.normal_flip_y = True
    plan.normal_strength = float(s.get("Normal Strength", 1.0))
    if orm_pkg:
        orm_repo = _repo(plan, paths, orm_pkg, "ORM")
        plan.textures.append(TextureUse("_MetallicRoughnessMap", orm_pkg, orm_repo, "data"))
        plan.textures.append(TextureUse("_AOMap", orm_pkg, orm_repo, "data"))
    plan.metallic = float(s.get("Metallic Intensity", 1.0))
    ri = float(s.get("Roughness Intensity", 1.0))
    if ri > 1.0:
        plan.warnings.append(f"Roughness Intensity={ri:.4g} > 1: UE gives saturate(ORM.g * {ri:.4g}); the engine "
                             f"does not clamp roughness, so _Roughness is capped at 1 (slightly glossier than UE)")
    plan.roughness = min(ri, 1.0)
    if float(s.get("Roughness Contrast", 1.0)) != 1.0:
        plan.warnings.append(f"Roughness Contrast={s['Roughness Contrast']:.4g}: pow on roughness not representable")
    plan.double_sided = double_sided
    plan.alpha_cutoff = MASTER_CUTOFF


def _plan_hair(plan: MaterialPlan, ue: dict, paths: dict, baked: str | None, srgb: dict | None) -> None:
    s = ue.get("scalars") or {}
    v = ue.get("vectors") or {}
    sw = ue.get("static_switches") or {}
    tex = _effective_textures(ue)
    _check_params(plan, ue, HAIR_SCALARS_CONSUMED | {"RandomHueVariation", "VertexColorStrength", "Param"},
                  HAIR_SCALARS_IGNORED, HAIR_VECTORS_CONSUMED, HAIR_VECTORS_IGNORED,
                  HAIR_TEXTURES_CONSUMED, HAIR_TEXTURES_IGNORED)
    for k, want in HAIR_SWITCH_EXPECTED.items():
        if k in sw and sw[k] != want:
            plan.warnings.append(f"static switch {k}={sw[k]} changes the hair colour/opacity chain; not baked")
    variation = sw.get("Variation", True)
    if sw.get("UsesVertexColors", False) and variation:
        plan.warnings.append(f"UsesVertexColors: UE adds VertexColor * {float(s.get('VertexColorStrength', 1.0)):.4g} "
                             f"to the base colour; mesh vertex colours are not baked")
    if "Param" in s:
        plan.warnings.append(f"scalar Param={s['Param']:.4g}: not on the dumped base-colour/opacity chain; unknown use")
    if float(s.get("Noisetiling", 24.0)) <= 0:
        plan.warnings.append("Noisetiling <= 0: the (1 + noise) term is not its mean")

    alpha_pkg = tex.get("Alpha")
    root_pkg = tex.get("T_Root") or tex.get("Root")
    id_pkg = tex.get("Unique_Hair_Value")
    if not alpha_pkg or not root_pkg:
        plan.warnings.append(f"hair without Alpha ({alpha_pkg}) or root gradient ({root_pkg}): not baked")
        plan.supported = False
        return
    alpha_repo = _repo(plan, paths, alpha_pkg, "Alpha")
    # Variation off bypasses both per-strand terms (the switch's False input is the plain base colour).
    rvv = float(s.get("RandomValueVariation", 0.0)) if variation else 0.0
    rhv = float(s.get("RandomHueVariation", 0.0)) if variation else 0.0
    root = tuple((v.get("RootColor") or [0.1098, 0.0745, 0.0471, 1])[:3])   # master defaults if not overridden
    tip = tuple((v.get("TipColor") or [0.1098, 0.0745, 0.0510, 1])[:3])
    sources = {"alpha": alpha_pkg, "root": root_pkg}
    params = {"root_color": root, "tip_color": tip, "brightness": float(s.get("Brightness", 0.8)),
              "value_variation": rvv, "hue_variation": rhv,
              "alpha_srgb": _srgb(alpha_pkg, srgb), "root_srgb": _srgb(root_pkg, srgb)}
    if (rvv or rhv) and not id_pkg:
        plan.warnings.append("RandomValue/HueVariation set but no Unique_Hair_Value texture: variation not baked")
    if id_pkg and (rvv or rhv):
        sources["id"] = id_pkg
        params["id_srgb"] = _srgb(id_pkg, srgb)
    dst = baked or default_baked_path(alpha_repo or "", plan.ue_name, "Albedo")
    plan.bake = Bake("hair", dst, sources, params)
    plan.albedo_path = dst
    plan.roughness = min(max(float(s.get("Roughness", 0.38)), 0.0), 1.0)   # UE saturates the constant: exact
    plan.metallic = 0.0
    plan.double_sided = True
    plan.alpha_cutoff = HAIR_CUTOFF


# -------------------------------------------------------------------------------------------------- bakes ---
def execute_bake(plan: MaterialPlan, source_file: Callable[[str], Path], dst_abs: Path, cap: int,
                 force: bool = False) -> textures.CookResult | None:
    """Run plan.bake into dst_abs (normally common.repo_abs(plan.bake.repo_path)).
    source_file(ue_texture_pkg) -> the exported file for that texture."""
    b = plan.bake
    if b is None:
        return None
    if b.kind == "adjusted":
        alpha = b.sources.get("alpha_map")
        return textures.bake_adjusted_albedo(source_file(b.sources["base"]), dst_abs, cap,
                                             alpha_src=source_file(alpha) if alpha else None, force=force, **b.params)
    if b.kind == "hair":
        idp = b.sources.get("id")
        return textures.bake_hair_albedo(source_file(b.sources["alpha"]), source_file(b.sources["root"]), dst_abs,
                                         cap, id_tex=source_file(idp) if idp else None, force=force, **b.params)
    raise ValueError(f"unknown bake kind {b.kind!r}")


# ------------------------------------------------------------------------------------------------- output ---
SLOTS = ("_AlbedoMap", "_AOMap", "_EmissiveMap", "_MetallicMap", "_MetallicRoughnessMap", "_NormalMap",
         "_RoughnessMap")


def build_mat(plan: MaterialPlan, texture_guids: dict, has_alpha: bool) -> dict:
    """The engine .mat dict (v2, same key set as the existing Quantum mats).

    has_alpha: the albedo PNG kept an alpha channel -> AlphaTest queue + alphaCutoff (else opaque, no cutoff keys).
    texture_guids: repo path -> GUID; a slot whose path has no GUID is written without a textureGuids entry.
    """
    if not plan.supported:
        raise ValueError(f"{plan.ue_name}: no engine material for family {plan.family!r}: {plan.warnings}")
    props = {slot: "" for slot in SLOTS}
    if plan.albedo_path:
        props["_AlbedoMap"] = plan.albedo_path
    for t in plan.textures:
        props[t.slot] = t.repo_path
    props.update({
        "_BaseColor": [float(c) for c in plan.base_color],
        "_DoubleSided": plan.double_sided,
        "_EmissiveColor": [0.0, 0.0, 0.0],
        "_EmissiveStrength": 1.0,
        "_Metallic": float(plan.metallic),
        "_Roughness": float(plan.roughness),
        "_Triplanar": False,
        "_TriplanarScale": 1.0,
    })
    if plan.normal_flip_y:
        props["_NormalFlipY"] = True
    if plan.normal_strength != 1.0:
        props["_NormalStrength"] = float(plan.normal_strength)
    mat = {
        "factorsScaleMaps": True,
        "matVersion": 2,
        "name": plan.name,
        "properties": props,
        "shader": SHADER,
    }
    if has_alpha:
        mat["alphaCutoff"] = plan.alpha_cutoff
        mat["renderQueue"] = 1       # MaterialAsset::Queue::AlphaTest - the only queue that clips
    guids = {slot: texture_guids[p] for slot, p in props.items()
             if slot in SLOTS and p and p in texture_guids}
    if guids:
        mat["textureGuids"] = guids
    return mat


def write_mat(path: Path, mat_dict: dict, keep_guid: bool = True) -> str:
    """Write the .mat and its .mat.meta {"guid", "metaVersion": 1, "type": "material"}. Returns the GUID
    (the existing one when keep_guid and a readable meta exists, else a new one)."""
    path = Path(path)
    write_json(path, mat_dict)
    guid = (existing_guid(path) if keep_guid else None) or new_guid()
    write_json(meta_path(path), {"guid": guid, "metaVersion": 1, "type": "material"})
    return guid


def load_ue_material(path: Path) -> dict:
    return read_json(Path(path))
