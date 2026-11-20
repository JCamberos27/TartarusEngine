#!/usr/bin/env python3
"""Generate the Casual Bundle mapping table (tools/quantum/casual_bundle_map.csv, see common.MAP_CSV).

One MapRow per UE mesh, material and texture in the export's manifest.json, plus keep-old rows for the existing repo
files that have no Casual Bundle equivalent (plan section 3). Retirement is NOT decided here (refs.py does that).

How it works (each step is a section below):
 1. Meshes. Every SkeletalMesh/StaticMesh is classified by its UE folder (body part, clothing slot, hair, facial hair,
    headwear) or skipped (presets, demo, Survival parts). Colour variants in one UE folder whose geometry is identical
    (same vertex count - or triangle count for static meshes - and same bounds to ~1 mm, from the engine's
    --model-report output) fold into ONE item: the base mesh gets replace/new, the other variants skip + base_of.
    Static meshes that duplicate a skeletal item are skipped ("SM duplicate"); SM-only headwear becomes rigid items.
 2. Repo paths. Items are matched to existing repo meshes through name normalisation (COLOUR_SYNONYMS, typo fixes,
    SKM_Male_ -> SKM_, F_ prefix <-> Female folder...) -> replace at the existing path; otherwise new at
    Models/Clothing/<Male|Female>/<SlotFolder>/SKM_[F_]<Item>.fbx (body parts: the race folder).
 3. Materials. Each MaterialInstance (or Material) used by an imported mesh becomes one .mat: an existing repo .mat with
    the same normalised name -> replace; otherwise a new colourway .mat in its owner item's material folder. Skins go to
    Materials/Characters, hair to Materials/Hair/<Item>, beards to Materials/FacialHair/<Item>. Eye/teeth/brow
    materials are skipped (MetaHuman shaders; the engine keeps its own).
 4. Textures. Every Texture2D referenced by a kept material goes next to it: Textures/<material sub-path>/<UE name>.png.
 5. keep-old rows for the plan's keep-list, and Unity-only colourways of items that get UE colours.

Usage:  python tools/quantum/casual_bundle/build_map.py [--report] [--out PATH] [--mesh-report PATH]
Deterministic: same inputs -> byte-identical CSV.
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

if __package__ in (None, ""):                       # run as a script: make "casual_bundle" importable
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from casual_bundle import common                # type: ignore
    from casual_bundle.common import MapRow         # type: ignore
else:
    from . import common
    from .common import MapRow

# ================================================================================================= constants ===
MESH_CLASSES = ("SkeletalMesh", "StaticMesh")
MATERIAL_CLASSES = ("MaterialInstanceConstant", "Material")
TEXTURE_CLASS = "Texture2D"
MAPPED_CLASSES = MESH_CLASSES + MATERIAL_CLASSES + (TEXTURE_CLASS,)

BOUNDS_TOL = 0.0015          # metres: the model report prints bounds in metres to 1 mm, so allow rounding either way
Q = common.QUANTUM           # "assets/Characters/Quantum"

# Spelling fixes on top of common.COLOUR_SYNONYMS (UE asset-name typos -> repo spelling). Applied token-wise.
TYPO_FIXES = {
    "Balck": "Black", "Blac": "Black", "Darck": "Dark", "Cose": "Close", "TotalBalack": "TotalBlack",
    "Vivan": "Vivian", "Wirst": "Wrist", "Qunatum": "Quantum", "Tshist": "Tshirt", "Classik": "Classic",
    "Snikers": "Sneakers", "Shoorts": "Shorts", "Commo": "Camo", "Shots": "Shorts", "BraidsBlond": "Braids_Blond", "Googles": "Goggles",
}
SYNONYMS = {**common.COLOUR_SYNONYMS, **TYPO_FIXES}
# Material word swaps between the old Unity names and UE (M_Jeans_Jacket_Beige_Fur <-> MI_Jeans_Jacket_Collar_Bege).
MAT_TOKEN_ALIASES = {"fur": "collar"}

# Lower-case tokens that name a colourway / print / material variant rather than a garment. Trailing runs of these are
# stripped to get an item's "colourless" name (SKM_Hoodie_Burgundy_Print -> SKM_Hoodie).
COLOUR_WORDS = {
    "beige", "black", "blue", "burgundy", "brown", "gray", "green", "orange", "red", "white", "yellow", "olive",
    "camel", "camo", "camodark", "multicam", "pixelsand", "pixelcamo", "street", "color", "colour", "rose", "violet",
    "purple", "pink", "blond", "silver", "gold", "print", "lines", "logo", "clear", "dark", "light", "cage",
    "redcage", "redwhite", "whitestrip", "strip", "leather", "fabric", "total", "totalblack", "blackline", "cubes",
    "pattern", "rude", "glass", "sunglass", "d",
}
# Variant words for body parts (skins/tattoos are materials on one mesh). Race words are kept on body parts.
BODY_VARIANT_WORDS = {"tatto", "ornament", "survival", "d"}
RACE_WORDS = {"afro", "old", "young", "asian"}
SIDE_TATTOO = re.compile(r"_Tatto(_L)?(_R)?$")     # SKM_F_Vivian_Arms_Tatto_L_R -> SKM_F_Vivian_Arms

# Tops vs Outerwear for UE Cloth/Clothes_Modules items that are not in the repo yet.
OUTERWEAR_TOKENS = {"bomber", "coat", "jacket", "vest", "puffer"}

# UE mesh folder (path after /Mesh/) -> (kind, slot folder). First match wins. kind: body | clothing | hair |
# facialhair | headwear | skip. A None slot for clothing means "Tops or Outerwear by item name".
FOLDER_RULES = [
    (r"^(Male|Female)/Presets?(/|$)", "skip", "Mesh/*/Preset*: merged preset character (not imported)"),
    (r"^(Male/Body/Quantum_FaceRig|Female/Body/Vivan_FaceRig)/", "skip",
     "face rig: >512 bones / duplicate of regular head (morph-target rig; the engine has no morph targets)"),
    (r"^Male/Arms/(Arms|Wrist)/", "body", None),
    (r"^Male/Arms/", "clothing", "Accessories"),
    (r"^Female/Arms_Modules/", "clothing", "Accessories"),
    (r"^(Male/Backpacks|Female/Backpack_Modules)/", "clothing", "Bags"),
    (r"^(Male/Chest|Female/Chest_Modules)/Bag_Banana", "clothing", "Bags"),
    (r"^(Male/Chest|Female/Chest_Modules)/", "clothing", "Outerwear"),
    (r"^(Male|Female)/Body(/|$)", "body", None),
    (r"^(Male/Cloth|Female/Clothes_Modules)/", "clothing", None),
    (r"^Male/Face_Modul/Beard/", "facialhair", "FacialHair"),
    (r"^(Male/Face_Modul|Female/Face_Modules)/Glasses", "clothing", "Glasses"),
    (r"^Male/Head_Modul/(Hairs|Haircut_Braids|Head_Scalp)/", "hair", "Hair"),
    (r"^Female/Hair/", "hair", "Hair"),
    (r"^Female/Head_Modules/SM/(Haircut_|Head_Scalp)", "hair", "Hair"),
    (r"^(Male/Head_Modul|Female/Head_Modules)/", "headwear", "Headwear"),
    (r"^(Male/Pants|Female/Pants_Modules)/", "clothing", "Pants"),
    (r"^(Male|Female)/Shoes/", "clothing", "Shoes"),
]

# Male body race folders under Models/ (UE folder name -> repo folder).
UE_RACE_FOLDERS = {"European": "European", "Afro": "Afro", "Asian": "Asian", "European_Old": "Old"}

# Known renames between the UE pack and the repo (AUDIT_REPORT appendix A: "Present (renamed)").
# (gender, canonical UE item stem) -> canonical repo stem.
MESH_ALIASES = {
    ("Male", "SKM_Watches_Military_L"): "SKM_Watch_L",
    ("Male", "SKM_Watches_Military_R"): "SKM_Watch_R",
}
# New items whose derived name would clash with an unrelated repo item. UE mesh folder -> new item stem.
NEW_NAME_OVERRIDES = {
    # A puffy vest worn over a built-in T-shirt (2nd slot MI_Tshirt_Tucked_White_D). The repo SKM_Puffy_Vest is the
    # Chest/Puffy_Vest one (its colours Beige/Black/Brown/Dark_Blue/Green/Yellow match that folder's MIs).
    "Male/Cloth/Puffy_Vest": "SKM_Puffy_Vest_Tshirt",
}

# keep-list (plan section 3): existing repo files that stay as they are. Paths relative to the Quantum root.
KEEP_MESHES = [
    "FirstPerson/Quantum_Arms_FP.fbx",
    "Models/European/Quantum_Arms.fbx",
    "Models/European/Quantum_Hands.fbx",
    "Models/European/Quantum_Arms_Hands.fbx",
    "Models/Clothing/Male/Balaclava/SM_Balaclava_Crime.fbx",
    "Models/Clothing/Female/Balaclava/SM_F_Balaclava_Crime.fbx",
    "Models/Clothing/Male/Pants/SKM_Jeans_Inboots.fbx",
    "Models/Clothing/Male/Pants/SKM_Pants_Inboots.fbx",
    "Models/Clothing/Male/Pants/SKM_Sport_Pants_Inboots.fbx",
    "Models/Clothing/Female/Pants/SKM_F_Sport_Pants_Inboots.fbx",
    "Models/Clothing/Male/Shoes/SKM_Boots_Socks.fbx",
    "Models/Clothing/Male/Outerwear/SKM_Coat_Hoodie_Hood.fbx",
    "Models/Clothing/Female/Outerwear/SKM_F_Coat_Hoodie_Hood.fbx",
    "Models/Clothing/Female/Shoes/SKM_F_Vivian_Feet_Shoes.fbx",
    "Models/Clothing/Female/Shoes/SKM_F_Vivian_Feet_Shoes_Long.fbx",
    "Models/Female/Body/Parts/SKM_F_Vivian_Parts.fbx",
    "Models/Clothing/Female/Outerwear/SKM_F_Puffer_Jacket_V2.fbx",
    "Models/Clothing/Female/Shoes/SKM_F_Socks_V2.fbx",
]
KEEP_FOLDERS = ["Materials/Clothing/Balaclava", "Textures/Clothing/Balaclava", "Materials/Characters/Eyes",
                "Textures/Eyes"]
# Eye / brow / teeth / saliva / cornea / cartilage engine materials (UE versions need MetaHuman shaders).
KEEP_MATERIAL_RE = re.compile(r"^Materials/Characters/M_(F_)?(Brows_Lashes|Cartilage|Eye|Saliva|Teeth)[^/]*\.mat$")

# UE materials whose shaders the engine can't reproduce (eyes, teeth, lacrimal, cartilage, eye occlusion, brows).
METAHUMAN_NAMES = {
    "M_Eye", "M_F_Eye", "M_EyeOcclusion", "M_F_EyeOcclusion", "M_lacrimal_fluid", "M_F_lacrimal_fluid", "M_Teeth",
    "M_F_Teeth", "M_Cartilage", "M_F_Cartilage", "M_FaceHair_Master", "M_F_FaceHair_Master", "M_Eye_Sclera",
    "M_Eyeball", "M_Eyelashesh", "M_Head_Baked_Quantum",
}
HAIR_PARENTS = {"M_Hair_Master", "M_F_Hair_Master"}
MASTER_MATERIAL = "M_Quantum_Master_Material"

# Texture parameters that the engine material can't use (hair wind/flow/dye helpers, the master material's disabled
# AO/Roughness/Metallic/Alpha placeholder slots). "Alpha_Map" counts only when an MI sets it explicitly.
IGNORED_TEXTURE_PARAMS = {
    "Wind_Gradient", "FlowMapTexture", "DyeMask", "Color Mask_1", "Color Mask_2", "AO_2ndUV", "2ndUVRootColor",
    "2ndUVTipColor", "Unique_Hair_Value", "AO_Texture", "Roughness_Texture", "Metallic_Texture", "Alpha_Map",
}


# ============================================================================================ name helpers ===
def tokens(stem: str) -> list[str]:
    """Split an asset stem into tokens with synonyms/typo fixes applied (BlueLight -> Light, Blue)."""
    out: list[str] = []
    for t in re.split(r"[_()\s.]+", stem):
        if not t:
            continue
        out.extend(x for x in SYNONYMS.get(t, t).split("_") if x)
    return out


def is_female_tokens(toks: list[str]) -> bool:
    return any(t == "F" for t in toks[:3])


def canon_stem(stem: str, gender: str) -> str:
    """Repo-convention stem: synonyms applied, SKM_Male_/SKM_M_ -> SKM_, female items get SKM_F_ / SM_F_."""
    toks = tokens(stem)
    prefix = ""
    if toks and toks[0] in ("SKM", "SM", "SK"):
        prefix = "SM" if toks[0] == "SM" else "SKM"
        toks = toks[1:]
    if toks and toks[0] in ("Male", "M"):
        toks = toks[1:]
    toks = [t for t in toks if t != "F"]
    if gender == "Female":
        toks = ["F"] + toks
    return "_".join(([prefix] if prefix else []) + toks)


def strip_variant(stem: str, words: set[str]) -> str:
    """Drop trailing variant tokens (colours, prints, races...) - keeps at least prefix + one token."""
    stem = SIDE_TATTOO.sub("", stem)
    toks = stem.split("_")
    keep = 2 if toks and toks[0] in ("SKM", "SM") else 1
    if len(toks) > keep and toks[keep - 1] == "F":
        keep += 1
    while len(toks) > keep and toks[-1].lower() in words:
        toks.pop()
    return "_".join(toks)


def colour_of(stem: str, words: set[str] = COLOUR_WORDS) -> str:
    """Trailing run of colour words, e.g. M_Hoodie_Burgundy_Print -> Burgundy_Print ('' if none)."""
    toks = tokens(stem)
    run: list[str] = []
    while toks and toks[-1].lower() in words and len(toks) > 1:
        run.insert(0, toks.pop())
    return "_".join(run)


def mesh_key(stem: str) -> str:
    """Matching key for meshes: canonical tokens without prefix/gender, lower-case, joined."""
    toks = [t for t in tokens(stem) if t not in ("SKM", "SM", "SK", "F")]
    if toks and toks[0] in ("Male", "M"):
        toks = toks[1:]
    return "".join(t.lower() for t in toks)


def mesh_key_sorted(stem: str) -> str:
    toks = [t for t in tokens(stem) if t not in ("SKM", "SM", "SK", "F")]
    if toks and toks[0] in ("Male", "M"):
        toks = toks[1:]
    return "+".join(sorted(t.lower() for t in toks))


def mat_tokens(stem: str) -> list[str]:
    """Material name tokens: MI_/M_ prefix, male marker and UE's trailing _D dropped; 'Woman' -> F."""
    toks = tokens(stem)
    if toks and toks[0] in ("MI", "M"):
        toks = toks[1:]
    if toks and toks[0] in ("M", "Male"):
        toks = toks[1:]
    toks = ["F" if t == "Woman" else t for t in toks]
    if len(toks) > 1 and toks[-1] in ("D", "Inst"):
        toks = toks[:-1]
    return toks


def mat_key(stem: str) -> str:
    return "".join(t.lower() for t in mat_tokens(stem))


def item_tokens(stem: str) -> list[str]:
    """Lower-case garment tokens of an item stem (no prefix, no gender marker)."""
    return [t.lower() for t in tokens(stem) if t not in ("SKM", "SM", "SK", "F", "Male")]


def name_score(item_stem: str, mi_stem: str) -> tuple[float, int]:
    """How well an MI name fits an item: share of the item's tokens found in the MI name, and the count."""
    it = item_tokens(item_stem)
    mt = {t.lower() for t in mat_tokens(mi_stem)}
    if not it:
        return (0.0, 0)
    hit = sum(1 for t in it if t in mt)
    return (hit / len(it), hit)


def folder_overlap(a: str, b: str) -> int:
    """Shared lower-case tokens between two folder paths (tie-breaker)."""
    ta = {t.lower() for t in re.split(r"[/_\s]+", a) if t}
    tb = {t.lower() for t in re.split(r"[/_\s]+", b) if t}
    return len(ta & tb)


def ue_name(ue: str) -> str:
    return ue.rsplit("/", 1)[-1]


def ue_parent(ue: str) -> str:
    return ue.rsplit("/", 1)[0]


# ============================================================================================ model report ===
@dataclass
class Geo:
    verts: int | None = None       # skinned vertex count ("skin: N vertices"); None for static meshes
    tris: int | None = None
    bmin: tuple[float, ...] = ()
    bmax: tuple[float, ...] = ()

    def count(self) -> int | None:
        return self.verts if self.verts is not None else self.tris

    def same_shape(self, other: "Geo", tol: float = BOUNDS_TOL) -> bool:
        if self.count() is None or self.count() != other.count():
            return False
        return all(abs(a - b) <= tol for a, b in zip(self.bmin + self.bmax, other.bmin + other.bmax))


_REPORT_MODEL = re.compile(r"^\[ModelReport\] (.+?\.fbx) size [-\d. ]+ min ([-\d. ]+) max ([-\d. ]+) meshes")
_REPORT_TRIS = re.compile(r"^\[ModelReport\]\s+tris: (\d+)")
_REPORT_SKIN = re.compile(r"^\[ModelReport\]\s+skin: (\d+) vertices")


def export_key(path: str) -> str:
    """Normalise an export file path (absolute or manifest-relative) to 'export/...' lower-case with / separators."""
    p = path.replace("\\", "/").lower()
    i = p.find("export/")
    return p[i:] if i >= 0 else p


def parse_mesh_report(text: str) -> dict[str, Geo]:
    """Parse the engine's --model-report output -> {export_key(file): Geo}."""
    geo: dict[str, Geo] = {}
    cur: Geo | None = None
    for line in text.splitlines():
        line = line.rstrip()
        m = _REPORT_MODEL.match(line)
        if m:
            cur = Geo(bmin=tuple(map(float, m.group(2).split())), bmax=tuple(map(float, m.group(3).split())))
            geo[export_key(m.group(1))] = cur
            continue
        if cur is None:
            continue
        m = _REPORT_TRIS.match(line)
        if m:
            cur.tris = int(m.group(1))
            continue
        m = _REPORT_SKIN.match(line)
        if m:
            cur.verts = int(m.group(1))
    return geo


def report_complete(text: str) -> bool:
    return re.search(r"\[ModelReport\] \d+ models, ", text) is not None


# =============================================================================================== repo scan ===
@dataclass
class Repo:
    """The existing pack: repo paths (assets/Characters/Quantum/...) of its fbx/mat/png files + mesh remaps."""
    files: set[str] = field(default_factory=set)
    remaps: dict[str, dict[str, str]] = field(default_factory=dict)        # fbx repo path -> {slot: mat path}
    mat_textures: dict[str, list[str]] = field(default_factory=dict)       # mat repo path -> referenced pngs

    @staticmethod
    def scan(project: Path = common.PROJECT) -> "Repo":
        root = project / Q
        repo = Repo()
        for p in sorted(root.rglob("*")):
            if p.suffix.lower() not in (".fbx", ".mat", ".png") or not p.is_file():
                continue
            rp = p.relative_to(project).as_posix()
            repo.files.add(rp)
            if p.suffix.lower() == ".fbx":
                meta = common.meta_path(p)
                if meta.exists():
                    try:
                        repo.remaps[rp] = dict(common.read_json(meta).get("materialRemap") or {})
                    except (OSError, ValueError):
                        pass
            elif p.suffix.lower() == ".mat":
                try:
                    props = common.read_json(p).get("properties") or {}
                except (OSError, ValueError):
                    props = {}
                repo.mat_textures[rp] = sorted({v for v in props.values()
                                                if isinstance(v, str) and v.lower().endswith(".png")})
        return repo

    def under(self, rel: str, ext: str) -> list[str]:
        pre = f"{Q}/{rel}/" if rel else f"{Q}/"
        return sorted(f for f in self.files if f.startswith(pre) and f.lower().endswith(ext))


def repo_gender(rp: str) -> str:
    stem = Path(rp).stem
    return "Female" if "/Female/" in rp or is_female_tokens(tokens(stem)) else "Male"


def repo_race(rp: str) -> str | None:
    m = re.match(rf"^{Q}/Models/(European|Afro|Asian|Old|Young)/", rp)
    return m.group(1) if m else None


# ================================================================================================== meshes ===
@dataclass
class UEMesh:
    ue: str
    cls: str
    name: str
    file: str
    folder: str                 # UE folder after /Mesh/, e.g. Male/Cloth/Hoodie
    gender: str
    kind: str                   # body | clothing | hair | facialhair | headwear | skip
    slot: str | None
    mats: list[str]             # material slot assignments (ue paths; '' when the slot's material is missing)
    geo: Geo | None
    slot_names: list[str] = field(default_factory=list)   # FBX material slot names, parallel to mats
    inferred: list[str] = field(default_factory=list)     # MIs guessed by name for slots with no material
    material_note: str = ""     # "material inferred: ..." / "NO MATERIAL ..." for meshes with empty slots
    canon: str = ""             # canon_stem(name, gender)
    skip_reason: str = ""
    item: "Item | None" = None  # the item this mesh belongs to (as base, colour variant or SM duplicate)
    sm_dup_of: "Item | None" = None


@dataclass
class Item:
    gender: str
    kind: str
    slot: str | None
    folder: str                 # UE folder
    members: list[UEMesh]
    race: str | None = None     # body parts (male): European/Afro/Asian/Old/Young
    stem: str = ""              # final file stem (no extension)
    repo_path: str = ""
    action: str = ""            # replace | new
    base: UEMesh | None = None
    notes: list[str] = field(default_factory=list)
    match_how: str = ""
    sm_extra: bool = False      # rigid SM item made of SM-only colours of an SKM item

    @property
    def is_sm(self) -> bool:
        return self.members[0].cls == "StaticMesh"

    def group(self) -> str:
        """item_group column: Male/Tops/Hoodie, Female/Hair/Haircut_Bobcut, Body/European, Body/Female."""
        if self.kind == "body":
            return f"Body/{self.race}" if self.gender == "Male" else "Body/Female"
        short = re.sub(r"^(SKM|SM)_(F_)?", "", self.stem)
        return f"{self.gender}/{self.slot}/{short}"

    def material_folder_name(self) -> str:
        return re.sub(r"^(SKM|SM)_", "", self.stem)


def classify_folder(folder: str) -> tuple[str, str | None]:
    for pattern, kind, slot in FOLDER_RULES:
        if re.search(pattern, folder + "/"):
            return kind, slot
    return "unclassified", None


def mesh_from_manifest(ue: str, info: dict, geo: dict[str, Geo]) -> UEMesh:
    name = ue_name(ue)
    rel = ue[len(common.UE_ROOT) + 1:]                      # Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black
    folder = ue_parent(rel)[len("Mesh/"):] if rel.startswith("Mesh/") else ue_parent(rel)
    gender = "Female" if folder.startswith("Female") else "Male"
    mats = [s.get("material") or "" for s in info.get("material_slots") or []]
    slot_names = [s.get("slot") or "" for s in info.get("material_slots") or []]
    m = UEMesh(ue=ue, cls=info["class"], name=name, file=info.get("file", "") or "", folder=folder, gender=gender,
               kind="", slot=None, mats=mats, geo=geo.get(export_key(info.get("file", "") or "")),
               slot_names=slot_names)
    m.canon = canon_stem(name, gender)
    if not rel.startswith("Mesh/"):
        top = rel.split("/", 1)[0]
        m.kind, m.skip_reason = "skip", (f"{top}/: demo content (not imported)" if top == "Demo"
                                         else f"outside Mesh/ ({top}/)")
        return m
    kind, slot = classify_folder(folder)
    m.kind, m.slot = kind, slot
    if kind == "skip":
        m.skip_reason = slot or "skip"
        m.slot = None
    elif "survival" in name.lower() or "survival" in folder.lower():
        m.kind, m.skip_reason = "skip", "Survival-only part (the Survival bundle is not in this pack)"
    elif kind == "unclassified":
        m.skip_reason = f"UNCLASSIFIED: no folder rule for Mesh/{folder}"
    return m


def cluster(meshes: list[UEMesh]) -> list[list[UEMesh]]:
    """Group meshes of one folder by identical geometry (count + bounds). Missing geometry -> own cluster."""
    clusters: list[list[UEMesh]] = []
    for m in sorted(meshes, key=lambda x: x.ue):
        for c in clusters:
            g0 = c[0].geo
            if g0 is not None and m.geo is not None and g0.same_shape(m.geo):
                c.append(m)
                break
        else:
            clusters.append([m])
    return clusters


def body_race(folder: str, stem: str) -> str:
    toks = {t.lower() for t in tokens(stem)}
    if "young" in toks:
        return "Young"
    if "old" in toks:
        return "Old"
    if "afro" in toks:
        return "Afro"
    if "asian" in toks:
        return "Asian"
    sub = folder.split("/")[2] if folder.count("/") >= 2 else ""
    return UE_RACE_FOLDERS.get(sub, "European")


def variant_words(kind: str) -> set[str]:
    return BODY_VARIANT_WORDS if kind == "body" else (COLOUR_WORDS | RACE_WORDS | {"survival"})


def derived_stem(item: Item) -> str:
    """Most common colourless member name (ties: fewest tokens, then alphabetical)."""
    words = variant_words(item.kind)
    names = [strip_variant(m.canon, words) for m in item.members]
    counts = collections.Counter(names)
    return sorted(counts, key=lambda n: (-counts[n], len(n.split("_")), n))[0]


def build_items(meshes: list[UEMesh]) -> list[Item]:
    """Cluster non-skipped meshes per UE folder into items; fold same-name same-shape body parts across folders."""
    by_folder: dict[str, list[UEMesh]] = collections.defaultdict(list)
    for m in meshes:
        if not m.skip_reason:
            by_folder[m.folder].append(m)
    items: list[Item] = []
    for folder in sorted(by_folder):
        for c in cluster(by_folder[folder]):
            it = Item(gender=c[0].gender, kind=c[0].kind, slot=c[0].slot, folder=folder, members=c)
            items.append(it)
    # Body parts duplicated in another race folder with identical geometry (e.g. European_Old/SKM_Quantum_Legs is the
    # European legs with the Old skin) fold into the first one.
    merged: list[Item] = []
    for it in items:
        if it.kind == "body":
            names = {m.name for m in it.members}
            twin = next((o for o in merged if o.kind == "body" and o.gender == it.gender
                         and names & {m.name for m in o.members}
                         and o.members[0].geo and it.members[0].geo
                         and o.members[0].geo.same_shape(it.members[0].geo)), None)
            if twin is not None:
                twin.members.extend(it.members)
                twin.notes.append(f"includes same-shape {it.folder} variant")
                continue
        merged.append(it)
    for it in merged:
        for m in it.members:
            m.item = it
        it.stem = derived_stem(it)
        if it.kind == "body":
            it.race = body_race(it.folder, it.stem) if it.gender == "Male" else None
    return merged


# ------------------------------------------------------------------------------------------ static meshes ---
def resolve_static(items: list[Item]) -> list[Item]:
    """SM items that duplicate an SKM item -> their meshes become SM duplicates of it. Headwear colours that exist only
    as SM become one rigid item (rule 3); SM-only items stay as they are. Returns the remaining item list."""
    skm_index: dict[tuple[str, str], Item] = {}
    for it in items:
        if it.is_sm:
            continue
        for m in it.members:
            skm_index.setdefault((it.gender, strip_variant(m.canon, variant_words(it.kind))), it)
            skm_index.setdefault((it.gender, m.canon), it)
        skm_index.setdefault((it.gender, it.stem), it)
    out: list[Item] = []
    for it in items:
        if not it.is_sm:
            out.append(it)
            continue
        as_skm = lambda s: re.sub(r"^SM_", "SKM_", s)            # noqa: E731
        twin = None
        for m in it.members:
            for key in (as_skm(m.canon), as_skm(strip_variant(m.canon, variant_words(it.kind)))):
                twin = twin or skm_index.get((it.gender, key))
        if twin is None:
            twin = skm_index.get((it.gender, as_skm(it.stem)))
        if twin is None:
            it.notes.append("SM-only item: imported as a rigid item")
            out.append(it)
            continue
        twin_colours = {colour_of(m.canon).lower() for m in twin.members}
        extras = [m for m in it.members if it.kind == "headwear" and colour_of(m.canon).lower() not in twin_colours]
        for m in it.members:
            if m not in extras:
                m.item, m.sm_dup_of = None, twin
        if extras:
            rigid = Item(gender=it.gender, kind=it.kind, slot=it.slot, folder=it.folder, members=extras,
                         stem=it.stem, sm_extra=True)
            rigid.notes.append(f"SM-only colours of {twin.stem} (no SKM variant): rigid item")
            for m in extras:
                m.item = rigid
            out.append(rigid)
    return out


# --------------------------------------------------------------------------------------- repo matching ---
@dataclass
class RepoMesh:
    path: str
    gender: str
    race: str | None
    keys: tuple[str, str, str]                     # exact, sorted-token, without trailing 01


def repo_meshes(repo: Repo, keep: set[str]) -> list[RepoMesh]:
    out = []
    for rp in repo.under("Models", ".fbx"):
        if rp in keep:
            continue
        stem = Path(rp).stem
        k = mesh_key(stem)
        out.append(RepoMesh(rp, repo_gender(rp), repo_race(rp), (k, mesh_key_sorted(stem), re.sub(r"0?1$", "", k))))
    return out


def item_candidates(it: Item) -> list[tuple[int, str]]:
    """(priority, stem) names to try against repo meshes; lower priority wins."""
    words = variant_words(it.kind)
    cands = [(0, it.stem)]
    for m in it.members:
        cands += [(1, m.canon), (1, strip_variant(m.canon, words))]
    alias = MESH_ALIASES.get((it.gender, it.stem))
    if alias:
        cands.insert(0, (0, alias))
    prefix = "SM" if it.is_sm else "SKM"
    folder_leaf = it.folder.rsplit("/", 1)[-1]
    if folder_leaf != "SM" and it.kind != "body":
        cands.append((3, canon_stem(f"{prefix}_{folder_leaf}", it.gender)))
    return cands


def colour_overlap(it: Item, rp: str, repo: Repo) -> int:
    """How many of the item's mesh colours exist as .mat colours in the repo mesh's material folder(s)."""
    folders = {v.rsplit("/", 1)[0] for v in repo.remaps.get(rp, {}).values()
               if "/Materials/Characters/" not in v and "/Generated/" not in v}
    repo_cols = {colour_of(Path(f).stem).lower() for d in folders for f in repo.files
                 if f.startswith(d + "/") and f.endswith(".mat") and f.count("/") == d.count("/") + 1}
    return len({colour_of(m.canon).lower() for m in it.members} & repo_cols)


def match_repo(items: list[Item], repo: Repo, keep: set[str]) -> None:
    """Pick replace targets: best (priority, key kind) match per item; conflicts go to the better colour overlap."""
    rms = repo_meshes(repo, keep)
    claims: dict[str, list[tuple[tuple, Item]]] = collections.defaultdict(list)
    for it in items:
        if it.sm_extra:
            continue                                   # rigid SM colour items are always new
        best: tuple | None = None
        for prio, stem in item_candidates(it):
            k, ks, k01 = mesh_key(stem), mesh_key_sorted(stem), re.sub(r"0?1$", "", mesh_key(stem))
            for rm in rms:
                if rm.gender != it.gender:
                    continue
                if it.kind == "body" and rm.race is not None and rm.race != it.race:
                    continue
                if it.kind != "body" and rm.race is not None:
                    continue
                if it.is_sm != Path(rm.path).stem.startswith("SM_"):
                    continue
                how = 0 if k == rm.keys[0] else 1 if ks == rm.keys[1] else 2 if k01 == rm.keys[2] else None
                if how is None:
                    continue
                score = (prio, how, rm.path)
                if best is None or score < best[0]:
                    best = (score, rm.path, stem)
        if best is not None:
            claims[best[1]].append((best[0], it))
            it.match_how = f"matched {best[2]}"
    for rp, lst in sorted(claims.items()):
        # name priority first, then which item's colours the repo's material folder already has, then key quality
        lst.sort(key=lambda x: (x[0][0], -colour_overlap(x[1], rp, repo), x[0][1], x[1].folder))
        winner = lst[0][1]
        winner.repo_path, winner.action, winner.stem = rp, "replace", Path(rp).stem
        for _, loser in lst[1:]:
            loser.notes.append(f"lost {Path(rp).name} to Mesh/{winner.folder}")
            loser.match_how = ""


def name_new_items(items: list[Item], repo: Repo) -> None:
    """Paths for items with no repo match, following repo conventions; resolves name clashes."""
    taken = {it.repo_path for it in items if it.repo_path} | repo.files
    per_folder = collections.Counter(it.folder for it in items if not it.sm_extra)
    for it in sorted(items, key=lambda i: (i.folder, i.members[0].ue)):
        if it.repo_path:
            continue
        stem = NEW_NAME_OVERRIDES.get(it.folder, it.stem)
        prefix = "SM" if it.is_sm else "SKM"
        leaf = it.folder.rsplit("/", 1)[-1]
        if it.kind != "body" and leaf != "SM" and per_folder[it.folder] == 1 and it.folder not in NEW_NAME_OVERRIDES:
            folder_stem = canon_stem(f"{prefix}_{leaf}", it.gender)
            if len(folder_stem.split("_")) > len(stem.split("_")):
                stem = folder_stem                       # SKM_Shirt (derived) -> SKM_Shirt_Tie (UE folder)
        if it.is_sm and not stem.startswith("SM_"):
            stem = re.sub(r"^SKM_", "SM_", stem)
        if it.kind == "body":
            # Quantum_Hands.fbx is the team's kept export -> the UE hands keep their SKM_ name next to it.
            candidates = [(stem, False), (stem, True)]
        else:
            candidates = [(stem, False), (canon_stem(f"{prefix}_{leaf}", it.gender) if leaf != "SM" else stem, False),
                          (it.members[0].canon, False)]
        candidates += [(f"{stem}_{i}", False) for i in range(2, 9)]
        for cand, keep_prefix in candidates:
            path = new_mesh_path(it, cand, keep_prefix)
            if path not in taken:
                break
        it.stem, it.repo_path, it.action = Path(path).stem, path, "new"
        taken.add(path)


def new_mesh_path(it: Item, stem: str, keep_prefix: bool = False) -> str:
    if it.kind == "body":
        if it.gender == "Female":
            sub = "Heads" if "head" in item_tokens(stem) else "Parts"
            return f"{Q}/Models/Female/Body/{sub}/{stem}.fbx"
        toks = re.sub(r"^SKM_", "", stem).split("_")       # repo male body parts have no SKM_ prefix
        if it.race not in ("European", None) and it.race.lower() not in {t.lower() for t in toks}:
            toks.insert(1 if toks[0] == "Quantum" else 0, it.race)        # Quantum_Legs (Old) -> Quantum_Old_Legs
        return f"{Q}/Models/{it.race}/{'SKM_' if keep_prefix else ''}{'_'.join(toks)}.fbx"
    slot = it.slot or ("Outerwear" if set(item_tokens(stem)) & OUTERWEAR_TOKENS else "Tops")
    it.slot = slot
    return f"{Q}/Models/Clothing/{it.gender}/{slot}/{stem}.fbx"


def finalize_slots(items: list[Item]) -> None:
    """Slot = the folder of the repo path (replace items keep their existing slot folder)."""
    for it in items:
        if it.kind == "body":
            continue
        m = re.match(rf"^{Q}/Models/Clothing/(Male|Female)/([^/]+)/", it.repo_path)
        if m:
            it.slot = m.group(2)


# =============================================================================================== materials ===
@dataclass
class UEMat:
    ue: str
    cls: str
    name: str
    file: str
    data: dict
    users: list[UEMesh] = field(default_factory=list)       # imported meshes (base, colour variant, SM dup) using it
    preset_users: int = 0
    kind: str = ""              # skin | hair | facialhair | clothing | skip
    reason: str = ""
    owner: Item | None = None
    repo_path: str = ""
    action: str = ""
    notes: list[str] = field(default_factory=list)


def owning_item(m: UEMesh) -> Item | None:
    return m.item if m.item is not None else m.sm_dup_of


def classify_material(mat: UEMat, is_parent: bool) -> None:
    ue, name = mat.ue, mat.name
    base = ue_name(mat.data.get("base_material") or "") if mat.cls != "Material" else name
    rel = ue[len(common.UE_ROOT) + 1:]
    if rel.startswith("Demo/"):
        mat.kind, mat.reason = "skip", "Demo/: demo content (not imported)"
    elif "/Arafatka/" in ue:
        mat.kind, mat.reason = "skip", "Arafatka scarf (Survival bundle; textures missing from this pack)"
    elif "survival" in ue.lower():
        mat.kind, mat.reason = "skip", "Survival-only material"
    elif name in METAHUMAN_NAMES or base in METAHUMAN_NAMES or "FaceHair" in name or \
            re.search(r"/(Eye|Teeth|Facial_Details|Head_Material_Functional)(/|$)", ue_parent(ue)):
        mat.kind, mat.reason = "skip", "MetaHuman shader (eye/teeth/brow material; the engine's keep-old materials stay)"
    elif not mat.users:
        if mat.cls == "Material" and is_parent:
            mat.kind, mat.reason = "skip", "parent material (its instances are imported instead)"
        elif mat.preset_users:
            mat.kind, mat.reason = "skip", "used only by preset characters (not imported)"
        else:
            mat.kind, mat.reason = "skip", "not used by any imported mesh"
    elif base in HAIR_PARENTS or (mat.cls == "Material" and re.search(r"Hair|Scalp|Braids", name)):
        kinds = {owning_item(u).kind for u in mat.users if owning_item(u)}
        mat.kind = "facialhair" if kinds == {"facialhair"} else "hair"
    elif base == MASTER_MATERIAL and re.search(r"/Materials/(Male_Body|Female_Body)$", ue_parent(ue)):
        mat.kind = "skin"
    elif mat.cls in MATERIAL_CLASSES:
        mat.kind = "clothing"
    else:
        mat.kind, mat.reason = "skip", f"UNCLASSIFIED: material {ue}"


def family_key(mat: UEMat) -> tuple[str, str]:
    """Colourways of one material family: same UE folder, same name without colour words / trailing digits
    (MI_Hair_Long, MI_Hair_Long_Red -> Hair_Long; MI_TShirt_White_1 -> TShirt)."""
    toks = mat_tokens(mat.name)
    while len(toks) > 1 and (toks[-1].lower() in COLOUR_WORDS or toks[-1].isdigit()):
        toks.pop()
    return ue_parent(mat.ue), "_".join(toks)


def pick_owner(mat: UEMat, users: list[UEMesh] | None = None, name: str | None = None) -> Item | None:
    """The item whose colour folder this material belongs to: best name fit, then UE-folder fit, then gender."""
    items = {id(it): it for it in (owning_item(u) for u in (users if users is not None else mat.users))
             if it is not None}
    if not items:
        return None
    name = name or mat.name
    mt = mat_tokens(name)
    female_mi = bool(mt) and mt[0] == "F"
    pref_kind = {"hair": ("hair",), "facialhair": ("facialhair",)}.get(mat.kind, ())
    return sorted(items.values(), key=lambda it: (
        0 if not pref_kind or it.kind in pref_kind else 1,
        tuple(-x for x in name_score(it.stem, name)),
        -folder_overlap(it.folder, ue_parent(mat.ue)[len(common.UE_ROOT):]),
        0 if (it.gender == "Female") == female_mi else 1,
        0 if it.action == "replace" else 1,
        it.repo_path))[0]


def is_female_mat(stem: str) -> bool:
    return "F" in mat_tokens(stem)[:1]


def item_material_folder(it: Item, repo: Repo, female_only: bool = False) -> str:
    """Folder holding an item's colourway .mat files (repo-relative, no trailing /). A replace item keeps the folder
    of its old materialRemap, unless the item wears only F_ materials and that folder is the male one (the old
    import shared e.g. Shoes/Boots between SKM_Boots and SKM_F_Boots_Inboots)."""
    if it.action == "replace":
        folders = sorted({v.rsplit("/", 1)[0] for v in repo.remaps.get(it.repo_path, {}).values()
                          if "/Materials/Characters/" not in v and "/Generated/" not in v})
        if folders:
            best = sorted(folders, key=lambda f: (-folder_overlap(f, it.stem), f))[0]
            mats_in = [p for p in repo.files if p.startswith(best + "/") and p.endswith(".mat")]
            female_folder = best.rsplit("/", 1)[-1].startswith("F_") or any(is_female_mat(Path(p).stem)
                                                                             for p in mats_in)
            if not (female_only and it.gender == "Female" and not female_folder):
                return best
    name = it.material_folder_name()
    if it.kind == "hair":
        return f"{Q}/Materials/Hair/{name}"
    if it.kind == "facialhair":
        return f"{Q}/Materials/FacialHair/{name}"
    if it.kind == "body":
        return f"{Q}/Materials/Characters"
    return f"{Q}/Materials/Clothing/{it.slot}/{name}"


def normalised_mat_stem(name: str) -> str:
    """M_ + normalised MI name: MI_F_Backpack_Casual_Bordo -> M_F_Backpack_Casual_Burgundy."""
    return "M_" + "_".join(mat_tokens(name))


def skin_stem(name: str) -> str:
    """Skin naming: race/age suffix last so the engine's suffix lookup works (MI_Quantum_Old_Arms -> M_Quantum_Arms_Old)."""
    toks = mat_tokens(name)
    if "Old" in toks and toks[-1] != "Old" and toks[0] in ("Quantum", "F"):
        toks = [t for t in toks if t != "Old"] + ["Old"]
    return "M_" + "_".join(toks)


def sorted_mat_key(stem: str) -> str:
    return "+".join(sorted(MAT_TOKEN_ALIASES.get(t.lower(), t.lower()) for t in mat_tokens(stem)))


def primary_material(it: Item, m: UEMesh, mats: dict[str, "UEMat"]) -> str | None:
    """The garment's own material on a mesh: the slot MI whose name fits the item best (skins/shirts underneath lose)."""
    best = None
    for mu in m.mats + m.inferred:
        mat = mats.get(mu)
        if mat is None or mat.kind not in ("clothing", "hair", "facialhair"):
            continue
        sc = name_score(it.stem, mat.name)
        if sc[1] > 0 and (best is None or sc > best[0]):
            best = (sc, mu)
    return best[1] if best else None


def map_materials(mats: dict[str, UEMat], items: list[Item], repo: Repo, keep_mats: set[str]) -> None:
    """Assign repo paths: exact name match to an existing .mat, then colour match in the owner's folder, then new."""
    repo_mats = [p for p in repo.under("Materials", ".mat")
                 if p not in keep_mats and "/Generated/" not in p]
    by_key: dict[str, list[str]] = collections.defaultdict(list)
    by_sorted: dict[str, list[str]] = collections.defaultdict(list)
    for p in repo_mats:
        by_key[mat_key(Path(p).stem)].append(p)
        by_sorted[sorted_mat_key(Path(p).stem)].append(p)
    claimed: dict[str, str] = {}

    def prefer(cands: list[str], mat: UEMat, folder: str) -> str | None:
        free = [c for c in cands if c not in claimed]
        if not free:
            return None
        ue_folder = ue_parent(mat.ue)[len(common.UE_ROOT):]
        return sorted(free, key=lambda c: (0 if c.rsplit("/", 1)[0] == folder else 1,
                                           -folder_overlap(c.rsplit("/", 1)[0], ue_folder), c))[0]

    def claim(mat: UEMat, hit: str, note: str = "") -> None:
        claimed[hit] = mat.ue
        mat.repo_path, mat.action = hit, "replace"
        if note:
            mat.notes.append(note)

    live = [m for m in sorted(mats.values(), key=lambda x: x.ue) if m.kind in ("skin", "hair", "facialhair", "clothing")]
    families: dict[tuple[str, str], list[UEMat]] = collections.defaultdict(list)
    for mat in live:
        families[family_key(mat)].append(mat)
    # owners: hair colourways follow their family (MI_Hair_Long_Red sits with MI_Hair_Long even when only one
    # haircut wears it); clothing follows its own users (MI_Shirt_RolledUp_Black_1 belongs to another mesh).
    for mat in live:
        if mat.kind in ("hair", "facialhair"):
            fam = families[family_key(mat)]
            mat.owner = pick_owner(mat, [u for f in fam for u in f.users], family_key(mat)[1])
        else:
            mat.owner = pick_owner(mat)
    # items that wear only F_ garment materials get a female colour folder
    female_only: dict[int, bool] = {}
    for it in items:
        prim = {primary_material(it, m, mats) for m in it.members} - {None}
        female_only[id(it)] = bool(prim) and all(is_female_mat(ue_name(p)) for p in prim)
    owner_folder = {}
    for mat in live:
        if mat.kind == "skin":
            owner_folder[mat.ue] = f"{Q}/Materials/Characters"
        elif mat.owner is not None:
            owner_folder[mat.ue] = item_material_folder(mat.owner, repo, female_only.get(id(mat.owner), False))
        else:
            owner_folder[mat.ue] = ""

    # 1) exact normalised name (skins: same tokens in any order, M_Quantum_Arms_Old == MI_Quantum_Old_Arms).
    #    Names that needed a UE suffix dropped (MI_Tshirt_Tucked_White_D) claim after the clean ones.
    for mat in sorted(live, key=lambda m: (tokens(m.name)[-1] in ("D", "Inst"), m.ue)):
        if mat.kind == "skin":
            cands = [c for c in by_sorted.get(sorted_mat_key(mat.name), []) if "/Materials/Characters/" in c]
        else:
            cands = [c for c in by_key.get(mat_key(mat.name), []) if "/Materials/Characters/" not in c]
        hit = prefer(cands, mat, owner_folder[mat.ue])
        if hit:
            claim(mat, hit)
    # 1b) same tokens in another order / known word swaps (MI_Jeans_Jacket_Collar_Bege == M_Jeans_Jacket_Beige_Fur),
    #     or the same name without the F_ marker inside the owner's folder (MI_F_Bracelet == F_Bracelet/M_Bracelet)
    for mat in live:
        if mat.repo_path or mat.kind == "skin":
            continue
        cands = [c for c in by_sorted.get(sorted_mat_key(mat.name), []) if "/Materials/Characters/" not in c]
        if not cands and is_female_mat(mat.name):
            plain = mat_key(mat.name)[1:]
            cands = [c for c in by_key.get(plain, []) if c.rsplit("/", 1)[0] == owner_folder[mat.ue]]
        hit = prefer(cands, mat, owner_folder[mat.ue])
        if hit:
            claim(mat, hit, f"matched {Path(hit).name} by name tokens")
    # family folder: a clothing colourway joins the folder where its matched siblings live, when they are worn by
    # the same item (MI_Tie_White -> Accessories/Tie with MI_Tie_Black/Blue/Red)
    for fam in families.values():
        homes = collections.Counter(m.repo_path.rsplit("/", 1)[0] for m in fam if m.repo_path)
        if not homes:
            continue
        home = sorted(homes, key=lambda f: (-homes[f], f))[0]
        worn_by = {id(owning_item(u)) for m in fam if m.repo_path for u in m.users}
        for mat in fam:
            if not mat.repo_path and mat.kind == "clothing" and mat.owner is not None and id(mat.owner) in worn_by:
                owner_folder[mat.ue] = home
    # 2) same trailing colour on an unclaimed .mat in the owner's folder whose garment words are a subset (or
    #    superset) of the MI's (MI_Shorts_Breeches_Flip_flops_Bege -> Pants/Shorts_Breeches/M_Shorts_Breeches_Beige)
    for mat in live:
        if mat.repo_path or mat.kind != "clothing":
            continue
        col = colour_of("_".join(mat_tokens(mat.name)))
        if not col:
            continue
        mt = {t.lower() for t in mat_tokens(mat.name)}
        cands = []
        for p in repo_mats:
            if p.rsplit("/", 1)[0] != owner_folder[mat.ue]:
                continue
            st = Path(p).stem
            if colour_of(st).lower() != col.lower() or is_female_mat(st) != is_female_mat(mat.name):
                continue
            garment = {t.lower() for t in mat_tokens(strip_variant(st, COLOUR_WORDS))}
            mi_garment = {t.lower() for t in mat_tokens(strip_variant(normalised_mat_stem(mat.name), COLOUR_WORDS))}
            if garment and mi_garment and (garment <= mt or mi_garment <= garment):
                cands.append(p)                      # MI_Sneakers_Black <-> Sneakers_Sport/M_Sneakers_Sport_Black
        hit = prefer(cands, mat, owner_folder[mat.ue])
        if hit:
            claim(mat, hit, f"matched by colour to {Path(hit).name}")
    # 3) new .mat in the owner's colour folder
    taken = set(repo.files) | {m.repo_path for m in live if m.repo_path}
    for mat in live:
        if mat.repo_path:
            continue
        folder = owner_folder[mat.ue]
        if not folder:
            mat.kind, mat.reason = "skip", f"UNCLASSIFIED: no owner item for {mat.name}"
            continue
        if mat.kind == "skin":
            options = [skin_stem(mat.name)]
        else:
            options = []
            siblings = [p for p in repo.files if p.startswith(folder + "/") and p.endswith(".mat")
                        and p.count("/") == folder.count("/") + 1]
            prefixes = {strip_variant(Path(p).stem, COLOUR_WORDS) for p in siblings}
            col = colour_of("_".join(mat_tokens(mat.name)))
            if len(prefixes) == 1 and col and all(is_female_mat(x) == is_female_mat(mat.name) for x in prefixes):
                options.append(f"{prefixes.pop()}_{col}")           # M_Shorts_ + Beige (sibling convention)
            stem = normalised_mat_stem(mat.name)
            options.append(re.sub(r"_1$", "", stem))
            options.append(stem)
            if mat.cls == "Material":
                options.append(stem + "_Material")
        for cand in options + [f"{options[-1]}_{i}" for i in range(2, 9)]:
            path = f"{folder}/{cand}.mat"
            if path not in taken:
                break
        mat.repo_path, mat.action = path, "new"
        taken.add(path)


# ================================================================================================ textures ===
def material_textures(data: dict, manifest: dict) -> list[str]:
    """Texture2D assets a material actually uses (see IGNORED_TEXTURE_PARAMS)."""
    explicit = data.get("textures") or {}
    out = set()
    for param, tex in (data.get("all_texture_params") or {}).items():
        if not tex:
            continue
        if param in IGNORED_TEXTURE_PARAMS and not (param == "Alpha_Map" and param in explicit):
            continue
        if manifest.get(tex, {}).get("class") == TEXTURE_CLASS:
            out.add(tex)
    for tex in data.get("used_textures") or []:         # base Materials (M_F_Scalp) list their textures here
        if manifest.get(tex, {}).get("class") == TEXTURE_CLASS:
            out.add(tex)
    for param, tex in explicit.items():                 # explicit overrides count, except the helper maps
        if not tex or (param in IGNORED_TEXTURE_PARAMS and param != "Alpha_Map"):
            continue
        if manifest.get(tex, {}).get("class") == TEXTURE_CLASS:
            out.add(tex)
    return sorted(out)


def texture_folder(mat: UEMat) -> str:
    """Textures/<the material's sub-path>; skins follow the existing Textures/<Race>/<Part> layout."""
    if mat.kind == "skin":
        toks = mat_tokens(mat.name)
        female = toks[0] == "F"
        part = next((t for t in toks if t in ("Arms", "Body", "Head")), "Body")
        race = next((r for r in ("Afro", "Asian", "Old", "Young") if r in toks), "European")
        return f"{Q}/Textures/" + ("Female/" if female else "") + f"{race}/{part}"
    folder = mat.repo_path.rsplit("/", 1)[0]
    return folder.replace(f"{Q}/Materials/", f"{Q}/Textures/", 1)


# ================================================================================================== build ===
@dataclass
class BuildResult:
    rows: list[MapRow]
    items: list[Item]
    unclassified: list[MapRow]
    conflicts: list[str]
    warnings: list[str]


def build(manifest: dict, geo: dict[str, Geo], repo: Repo, load_material=None) -> BuildResult:
    """Pure core: manifest assets + model report + repo scan -> rows. load_material(ue, info) -> material JSON dict."""
    assets = manifest.get("assets", manifest)
    if load_material is None:
        load_material = lambda ue, info: common.read_json(common.export_abs(info["file"]))  # noqa: E731
    warnings: list[str] = []
    keep_meshes = {f"{Q}/{p}" for p in KEEP_MESHES}

    # ---- 1. meshes
    meshes = [mesh_from_manifest(ue, info, geo) for ue, info in sorted(assets.items())
              if info.get("class") in MESH_CLASSES]
    for m in meshes:
        if not m.skip_reason and m.geo is None:
            warnings.append(f"no geometry in the model report for {m.ue} (kept as its own item)")
    items = build_items(meshes)
    items = resolve_static(items)
    match_repo(items, repo, keep_meshes)
    name_new_items(items, repo)
    finalize_slots(items)

    # ---- 3. materials (before base selection: the base is the variant wearing the old default colour)
    mats: dict[str, UEMat] = {}
    for ue, info in sorted(assets.items()):
        if info.get("class") in MATERIAL_CLASSES:
            try:
                data = load_material(ue, info) if info.get("file") else {}
            except (OSError, ValueError) as e:
                data = {}
                warnings.append(f"cannot read material JSON for {ue}: {e}")
            mats[ue] = UEMat(ue=ue, cls=info["class"], name=ue_name(ue), file=info.get("file", "") or "", data=data)
    parents = {(m.data.get("parent") or "") for m in mats.values()}
    infer_missing_materials(meshes, mats)
    for m in meshes:
        imported = m.item is not None or m.sm_dup_of is not None
        for mu in m.mats + m.inferred:
            if mu in mats:
                if imported:
                    if m not in mats[mu].users:
                        mats[mu].users.append(m)
                elif "Preset" in m.folder:
                    mats[mu].preset_users += 1
    for mat in mats.values():
        classify_material(mat, mat.ue in parents)
    keep_mats = {p for p in repo.under("Materials", ".mat") if is_keep_material(p)}
    map_materials(mats, items, repo, keep_mats)
    mat_path = {m.ue: m.repo_path for m in mats.values() if m.repo_path}

    # ---- base mesh per item
    for it in items:
        it.base = choose_base(it, repo, mat_path)

    rows: list[MapRow] = []
    # mesh rows
    for m in meshes:
        rows.append(mesh_row(m))
    # material rows
    for mat in sorted(mats.values(), key=lambda x: x.ue):
        rows.append(material_row(mat))

    # ---- 4. textures
    tex_rows, tex_warn = texture_rows(assets, mats, repo)
    rows += tex_rows
    warnings += tex_warn

    # ---- 5. keep-old
    rows += keep_old_rows(repo, rows, keep_meshes, keep_mats)

    # ---- checks
    seen: dict[str, int] = collections.Counter(r.ue_asset for r in rows if r.ue_asset)
    dupes = [ue for ue, n in seen.items() if n > 1]
    missing = [ue for ue, info in assets.items() if info.get("class") in MAPPED_CLASSES and ue not in seen]
    if dupes:
        warnings.append(f"assets with more than one row: {dupes[:10]}")
    for ue in sorted(missing):
        r = MapRow(ue_asset=ue, ue_class=assets[ue]["class"], role=role_of(assets[ue]["class"]), action="skip",
                   source_file=assets[ue].get("file", ""), notes="UNCLASSIFIED: no rule produced a row")
        rows.append(r)
    rows.sort(key=row_sort_key)          # after the UNCLASSIFIED rows, so the CSV stays sorted
    targets = collections.defaultdict(list)
    for r in rows:
        if r.action in ("replace", "new", "keep-old"):
            targets[r.repo_path].append(r.ue_asset or "(keep-old)")
    conflicts = [f"{p}: {v}" for p, v in sorted(targets.items()) if len(v) > 1]
    for r in rows:
        if r.action in ("replace", "new") and "/Hats/" in r.repo_path:
            warnings.append(f"path under an ignored Hats/ folder: {r.repo_path}")
    unclassified = [r for r in rows if "UNCLASSIFIED" in r.notes]
    return BuildResult(rows=rows, items=items, unclassified=unclassified, conflicts=conflicts, warnings=warnings)


def is_keep_material(p: str) -> bool:
    rel = p[len(Q) + 1:]
    return bool(KEEP_MATERIAL_RE.match(rel)) or any(rel.startswith(f + "/") for f in KEEP_FOLDERS)


def choose_base(it: Item, repo: Repo, mat_path: dict[str, str]) -> UEMesh:
    """Base variant: wears the old default .mat (replace items), else the plain name, else non-race, else alphabetical."""
    default = set(repo.remaps.get(it.repo_path, {}).values()) if it.action == "replace" else set()
    default_cols = {colour_of(Path(v).stem).lower() for v in default if "/Materials/Characters/" not in v}
    words = variant_words(it.kind)
    plain = it.stem if it.action == "new" else strip_variant(it.members[0].canon, words)

    def key(m: UEMesh):
        wears = sum(1 for mu in m.mats if mat_path.get(mu) in default)
        racy = any(t.lower() in RACE_WORDS | {"tatto", "ornament"} for t in tokens(m.name)[2:])
        return (-wears, 0 if colour_of(m.canon).lower() in default_cols and default_cols else 1,
                0 if m.canon in (it.stem, plain) or mesh_key(m.canon) == mesh_key(it.stem) else 1,
                1 if racy else 0, m.ue)
    return sorted(it.members, key=key)[0]


GENERIC_SLOT = re.compile(r"^(lambert|blinn|phong|standardSurface|Default_Material|Fbx|Material)", re.I)


def infer_missing_materials(meshes: list[UEMesh], mats: dict[str, "UEMat"]) -> None:
    """Some meshes (mostly SM_ headwear) have material slots with no material in manifest.json (UE sets the colour
    elsewhere). Guess the colourway MI by name: every garment word of the slot name (or, for generic slot names,
    of the item name) must appear in the MI name, and the MI's colour must equal the mesh's colour
    (SM_F_Cap_02_Black, slot M_Cap_05 -> MI_Cap_Black). No unique guess -> "NO MATERIAL" for review."""
    candidates = [m for m in mats.values() if m.cls == "MaterialInstanceConstant"
                  and "/Arafatka/" not in m.ue and "survival" not in m.ue.lower()]
    for m in meshes:
        it = owning_item(m)
        if it is None or (m.mats and all(m.mats)):
            continue
        empty = [sn for sn, mu in zip(m.slot_names, m.mats) if not mu] or ["(no slots)"]
        colour = colour_of(m.canon).lower()
        found, missing = [], []
        has_material = any(m.mats)
        garment = set(item_tokens(it.stem))
        for sn in empty:
            generic = not sn or sn == "(no slots)" or GENERIC_SLOT.match(sn)
            if has_material and generic:
                continue                    # a spare slot on a mesh that already has its garment material
            base = it.stem if generic else sn
            want = {t.lower() for t in tokens(base) if not t.isdigit() and t not in ("M", "MI", "F", "SKM", "SM")}
            want = {re.sub(r"\d+$", "", t) for t in want} - {""}
            if has_material and not want & garment:
                continue                    # stray slot unrelated to the item (empty skin slot, M_Snaekers on a vest)
            hits = []
            for mat in candidates:
                mt = {t.lower() for t in mat_tokens(mat.name)}
                if not want or not want <= mt:
                    continue
                if colour_of("_".join(mat_tokens(mat.name))).lower() != colour:
                    continue
                hits.append((-folder_overlap(ue_parent(mat.ue), m.folder), len(mt), mat.ue))
            if hits:
                found.append(sorted(hits)[0][2])
            else:
                missing.append(sn)
        m.inferred = sorted(set(found))
        notes = []
        if m.inferred:
            notes.append("material inferred: " + ", ".join(m.inferred))
        if missing:
            notes.append("NO MATERIAL for slot " + ", ".join(missing) + " (review)")
        m.material_note = "; ".join(notes)


def role_of(cls: str) -> str:
    return "mesh" if cls in MESH_CLASSES else "material" if cls in MATERIAL_CLASSES else "texture"


def mesh_row(m: UEMesh) -> MapRow:
    r = MapRow(ue_asset=m.ue, ue_class=m.cls, role="mesh", source_file=m.file)
    it = m.item
    if m.skip_reason:
        r.action, r.notes = "skip", m.skip_reason
        r.item_group = "Skip/" + m.folder
        return r
    if m.sm_dup_of is not None:
        twin = m.sm_dup_of
        r.action, r.item_group, r.base_of = "skip", twin.group(), twin.base.ue if twin.base else ""
        r.notes = "; ".join(n for n in (f"SM duplicate of {twin.stem}", m.material_note) if n)
        return r
    assert it is not None
    r.item_group = it.group()
    if m is it.base:
        r.action, r.repo_path = it.action, it.repo_path
        notes = []
        if it.action == "replace":
            notes.append(f"replaces {Path(it.repo_path).name} ({it.match_how})" if it.match_how else "")
        if len(it.members) > 1:
            notes.append(f"base of {len(it.members) - 1} colour variant(s)")
        if it.is_sm:
            notes.append("static mesh: rigid item")
        if "FaceRig" in m.name:
            notes.append("face-rig head (morph targets dropped by the importer; >512 bones)")
        notes += it.notes + [m.material_note]
        r.notes = "; ".join(n for n in notes if n)
    else:
        r.action, r.base_of = "skip", it.base.ue if it.base else ""
        r.notes = "colour variant (same geometry as base)"
        if m.material_note:
            r.notes += "; " + m.material_note
        if "survival" in m.name.lower():
            r.notes += "; Survival variant"
    return r


def material_row(mat: UEMat) -> MapRow:
    r = MapRow(ue_asset=mat.ue, ue_class=mat.cls, role="material", source_file=mat.file)
    if mat.kind == "skip" or not mat.repo_path:
        r.action, r.notes = "skip", mat.reason or "skip"
        r.item_group = "Skip/" + ue_parent(mat.ue)[len(common.UE_ROOT) + 1:]
        return r
    r.action, r.repo_path = mat.action, mat.repo_path
    if mat.kind == "skin":
        r.item_group = "Skin/" + ("Female" if mat_tokens(mat.name)[0] == "F" else "Male")
    elif mat.owner is not None:
        r.item_group = mat.owner.group()
    notes = [{"skin": "skin", "hair": "hair-shader colourway", "facialhair": "facial-hair colourway",
              "clothing": "colourway"}[mat.kind]]
    if mat.cls == "Material":
        notes.append("UE base Material (no instance)")
    notes += mat.notes
    r.notes = "; ".join(notes)
    return r


def texture_rows(assets: dict, mats: dict[str, UEMat], repo: Repo) -> tuple[list[MapRow], list[str]]:
    users: dict[str, list[UEMat]] = collections.defaultdict(list)
    skipped_users: dict[str, list[UEMat]] = collections.defaultdict(list)
    for mat in mats.values():
        for tex in material_textures(mat.data, assets):
            (users if mat.repo_path and mat.kind != "skip" else skipped_users)[tex].append(mat)
    rows, warnings, taken = [], [], {}
    for ue, info in sorted(assets.items()):
        if info.get("class") != TEXTURE_CLASS:
            continue
        r = MapRow(ue_asset=ue, ue_class=TEXTURE_CLASS, role="texture", source_file=info.get("file", "") or "")
        us = users.get(ue)
        if not us:
            r.action = "skip"
            r.item_group = "Skip/" + ue_parent(ue)[len(common.UE_ROOT) + 1:]
            if ue[len(common.UE_ROOT) + 1:].startswith("Demo/"):
                r.notes = "Demo/: demo content (not imported)"
            elif skipped_users.get(ue):
                reasons = sorted({m.reason.split(" (")[0] for m in skipped_users[ue]})
                r.notes = "used only by skipped materials: " + "; ".join(reasons)
            else:
                r.notes = "not referenced by any imported material"
            rows.append(r)
            continue
        ue_folder = ue_parent(ue)[len(common.UE_ROOT):]
        owner = sorted(us, key=lambda m: (-folder_overlap(texture_folder(m), ue_folder), m.repo_path))[0]
        folder = texture_folder(owner)
        path = f"{folder}/{ue_name(ue)}.png"
        if path in taken:                               # same texture name from another UE folder
            path = f"{folder}/{ue_parent(ue).rsplit('/', 1)[-1]}/{ue_name(ue)}.png"
            warnings.append(f"texture name clash at {folder}/{ue_name(ue)}.png: {ue} -> sub-folder")
            if path in taken:                           # the sub-folder is taken too: never overwrite silently
                warnings.append(f"texture name clash at {path} too: {ue} vs {taken[path]} (rename one by hand)")
                r.action, r.notes = "skip", f"CLASH: {path} already holds {taken[path]}"
                rows.append(r)
                continue
        taken[path] = ue
        r.repo_path, r.item_group = path, material_row(owner).item_group
        r.action = "replace" if path in repo.files else "new"
        notes = [f"used by {len(us)} material(s)" if len(us) > 1 else f"used by {owner.name}"]
        if (info.get("file") or "").lower().endswith(".bmp"):
            notes.append("bmp fallback -> png")
        r.notes = "; ".join(notes)
        rows.append(r)
    return rows, warnings


def keep_old_rows(repo: Repo, rows: list[MapRow], keep_meshes: set[str], keep_mats: set[str]) -> list[MapRow]:
    """Keep-list files (plan section 3) + Unity-only colourways of items that receive UE colours + their textures."""
    targeted = {r.repo_path for r in rows if r.action in ("replace", "new")}
    keep: dict[str, str] = {}

    def add(p: str, why: str):
        if p in repo.files and p not in targeted:
            keep.setdefault(p, why)

    for p in sorted(keep_meshes):
        add(p, "keep-list: no Casual Bundle equivalent")
        for mp in sorted(repo.remaps.get(p, {}).values()):
            if mp not in targeted and "/Materials/Characters/" not in mp:
                add(mp, f"material of kept {Path(p).name}")
    # Unity-only clothing models: an existing item no UE asset replaces has no Casual Bundle equivalent, so it stays
    # with its default material and that material's colourway siblings (e.g. SKM_Boots, SKM_F_Boots, SKM_F_Bomber).
    for p in repo.under("Models/Clothing", ".fbx"):
        if p in targeted:
            continue
        add(p, "Unity-only item: no Casual Bundle equivalent")
        for mp in sorted(repo.remaps.get(p, {}).values()):
            if mp in targeted:
                continue
            add(mp, f"material of kept {Path(p).name}")
            folder = mp.rsplit("/", 1)[0]
            for sib in repo.under(folder[len(Q) + 1:], ".mat"):
                if sib.rsplit("/", 1)[0] == folder:
                    add(sib, f"colourway of kept {Path(p).name}")
    for p in sorted(keep_mats):
        add(p, "keep-list: eye/brow/teeth/saliva/cornea/cartilage or balaclava material")
    for f in KEEP_FOLDERS:
        for p in repo.under(f, ".png"):
            add(p, "keep-list texture")
    # Unity-only colourways: a folder that receives UE colourways keeps its other (Unity) colours.
    ue_folders = {r.repo_path.rsplit("/", 1)[0] for r in rows
                  if r.role == "material" and r.action in ("replace", "new") and "/Materials/Clothing/" in r.repo_path}
    for p in repo.under("Materials/Clothing", ".mat"):
        if p.rsplit("/", 1)[0] in ue_folders:
            add(p, "Unity-only colourway (colour not in the Casual Bundle for this item)")
    # textures of every kept material
    for p in sorted(keep):
        if p.endswith(".mat"):
            for t in repo.mat_textures.get(p, []):
                add(t, f"texture of kept {Path(p).name}")
    out = []
    for p, why in sorted(keep.items()):
        role = "mesh" if p.endswith(".fbx") else "material" if p.endswith(".mat") else "texture"
        out.append(MapRow(role=role, item_group="KeepOld/" + keep_group(p), repo_path=p, action="keep-old",
                          notes=why))
    return out


def keep_group(p: str) -> str:
    rel = p[len(Q) + 1:]
    parts = rel.split("/")
    return "/".join(parts[:3]) if parts[0] in ("Materials", "Textures") else "/".join(parts[:-1])


def row_sort_key(r: MapRow):
    return (common.ROLES.index(r.role) if r.role in common.ROLES else 9, r.item_group, r.repo_path, r.ue_asset)


# ================================================================================================= report ===
def summary(res: BuildResult) -> str:
    lines = []
    counts = collections.Counter((r.role, r.action) for r in res.rows)
    lines.append(f"{len(res.rows)} rows")
    for role in common.ROLES:
        parts = [f"{a} {counts[(role, a)]}" for a in common.ACTIONS if counts[(role, a)]]
        lines.append(f"  {role:9s} " + ", ".join(parts))
    per_slot = collections.Counter()
    for it in res.items:
        if it.base is not None:
            per_slot[(it.gender, "Body" if it.kind == "body" else it.slot, it.action)] += 1
    lines.append("items (base meshes) per slot:")
    for g in ("Male", "Female"):
        slots = sorted({s for (gg, s, _) in per_slot if gg == g})
        lines.append(f"  {g}: " + ", ".join(
            f"{s} {per_slot[(g, s, 'replace')] + per_slot[(g, s, 'new')]}"
            f" ({per_slot[(g, s, 'replace')]} replace/{per_slot[(g, s, 'new')]} new)" for s in slots))
    lines.append(f"UNCLASSIFIED: {len(res.unclassified)}")
    for r in res.unclassified:
        lines.append(f"  {r.ue_asset}: {r.notes}")
    if res.conflicts:
        lines.append(f"repo paths targeted by more than one row: {len(res.conflicts)}")
        lines += [f"  {c}" for c in res.conflicts]
    if res.warnings:
        lines.append(f"warnings: {len(res.warnings)}")
        lines += [f"  {w}" for w in res.warnings]
    return "\n".join(lines)


def detailed_report(res: BuildResult) -> str:
    lines = ["", "items:"]
    for it in sorted(res.items, key=lambda i: (i.gender, i.kind, i.slot or "", i.stem)):
        lines.append(f"  [{it.action:7s}] {it.repo_path[len(Q) + 1:]}  <- {len(it.members)} mesh(es) from "
                     f"Mesh/{it.folder} (base {it.base.name if it.base else '-'})")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--report", action="store_true", help="also list every item with its repo path")
    ap.add_argument("--out", type=Path, default=common.MAP_CSV)
    ap.add_argument("--mesh-report", type=Path, default=common.MESH_REPORT)
    args = ap.parse_args(argv)
    text = args.mesh_report.read_text(encoding="utf-8", errors="replace")
    if not report_complete(text):
        print(f"warning: {args.mesh_report} has no closing 'N models, ' line (incomplete report?)", file=sys.stderr)
    res = build(common.load_manifest(), parse_mesh_report(text), Repo.scan())
    common.write_map(res.rows, args.out)
    print(f"wrote {args.out}")
    print(summary(res))
    if args.report:
        print(detailed_report(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
