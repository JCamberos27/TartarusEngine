"""Shared contract for the Quantum Modular Casual Bundle import (see docs/QUANTUM_SWAP.md).

Every module in this package imports its paths, file formats and the mapping-table schema from here, so the
pieces (build_map, materials, textures, metas, refs, csvrows) agree without knowing each other.

Terms
- "UE asset": a package path from the export's manifest.json, e.g. /Game/Casual_Wear_Bundle/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black
- "export file": the exported file for a UE asset, relative to EXPORT_ROOT (manifest entry "file"), e.g.
  Export/Casual_Wear_Bundle/Mesh/Male/Cloth/Hoodie/SKM_Hoodie_Black.fbx
- "repo path": project-relative engine path with forward slashes, as .mat/.meta/CSV store them, e.g.
  assets/Characters/Quantum/Models/Clothing/Male/Tops/SKM_Hoodie.fbx
"""
from __future__ import annotations

import csv
import json
import os
import secrets
from dataclasses import dataclass, field, fields, asdict
from pathlib import Path

# ----------------------------------------------------------------------------------------------- locations ---
REPO = Path(__file__).resolve().parents[3]                    # TartarusEngine/
PROJECT = REPO / "project"                                    # repo paths are relative to this
QUANTUM = "assets/Characters/Quantum"                         # repo path of the pack root
EXPORT_ROOT = Path(os.environ.get("QUANTUM_BUNDLE_EXPORT", r"D:\Jacob\JCamberos\Quantum - Modular Casual Bundle"))
EXPORT_MANIFEST = EXPORT_ROOT / "manifest.json"
UE_ROOT = "/Game/Casual_Wear_Bundle"
MAP_CSV = Path(__file__).resolve().parent.parent / "casual_bundle_map.csv"   # tools/quantum/casual_bundle_map.csv
MESH_REPORT = Path(os.environ.get("QUANTUM_MESH_REPORT", str(Path(__file__).resolve().parent / "mesh_report.txt")))

# Texture caps (pixels) - match the existing Quantum metas and tools/quantum/audit_quantum_assets.py defaults.
CAP_CLOTHING = 2048   # clothing, hair, headwear, accessories
CAP_BODY = 4096       # body / head / arms skin textures


def repo_abs(repo_path: str) -> Path:
    """Absolute path on disk of a project-relative repo path."""
    return PROJECT / repo_path


def export_abs(export_file: str) -> Path:
    return EXPORT_ROOT / export_file


# --------------------------------------------------------------------------------------------- engine files ---
def new_guid() -> str:
    """Engine GUID format: random 64-bit, 16 lowercase hex chars (src/Assets/AssetDatabase.cpp:39-62)."""
    return secrets.token_hex(8)


def read_json(path: Path) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, data: dict) -> None:
    """Engine style: 2-space indent, sorted keys, LF line endings, trailing newline (.gitattributes keeps LF)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def meta_path(asset_abs: Path) -> Path:
    return asset_abs.with_name(asset_abs.name + ".meta")


def existing_guid(asset_abs: Path) -> str | None:
    m = meta_path(asset_abs)
    if m.exists():
        try:
            return read_json(m).get("guid")
        except (OSError, ValueError):
            return None
    return None


# ------------------------------------------------------------------------------------------- mapping table ---
ACTIONS = ("replace", "new", "keep-old", "skip", "retire")
# replace  = UE asset written over an EXISTING repo path (keep that path's .meta/GUID)
# new      = UE asset written to a NEW repo path (fresh GUID)
# keep-old = an existing repo file with no UE equivalent that stays as is (keep-list); ue_asset is empty
# skip     = UE asset not imported (presets, demo, maps, SM_ duplicates of SKM_ items, colour variants whose
#            geometry equals the item's base mesh, Survival-only parts, the Arafatka scarf...)
# retire   = an existing repo file that nothing will reference after the import (decided by refs.py scan)
ROLES = ("mesh", "material", "texture")


@dataclass
class MapRow:
    ue_asset: str = ""        # UE package path ("" for keep-old/retire rows)
    ue_class: str = ""        # SkeletalMesh | StaticMesh | MaterialInstanceConstant | Material | Texture2D
    role: str = ""            # mesh | material | texture
    item_group: str = ""      # e.g. Male/Tops/Hoodie, Female/Hair/Haircut_Bobcut, Body/European, Skin/Male
    repo_path: str = ""       # project-relative destination (or existing file for keep-old/retire)
    action: str = ""          # one of ACTIONS
    source_file: str = ""     # export file relative to EXPORT_ROOT ("" for keep-old/retire)
    base_of: str = ""         # for a skipped colour-variant mesh: the ue_asset of the base mesh it folds into
    notes: str = ""

    @staticmethod
    def columns() -> list[str]:
        return [f.name for f in fields(MapRow)]


def read_map(path: Path = MAP_CSV) -> list[MapRow]:
    with open(path, encoding="utf-8", newline="") as f:
        return [MapRow(**{k: (r.get(k) or "") for k in MapRow.columns()}) for r in csv.DictReader(f)]


def write_map(rows: list[MapRow], path: Path = MAP_CSV) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        w = csv.DictWriter(f, fieldnames=MapRow.columns(), lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(asdict(r))


# ---------------------------------------------------------------------------------------- name normalising ---
# UE spellings -> repo spellings (the old import's conventions). Applied token-wise to colour/variant names.
COLOUR_SYNONYMS = {
    "Bege": "Beige", "Beje": "Beige", "Bordo": "Burgundy", "Grey": "Gray", "BlueLight": "Light_Blue",
    "DarkBlue": "Dark_Blue", "BlueDark": "Blue_Dark", "Linen": "Lines", "Casua": "Casual",
}

# Engine wardrobe slot folders under Models/Clothing/<Male|Female>/ (existing + the three new ones).
SLOT_FOLDERS = ("Tops", "Outerwear", "Pants", "Shoes", "Bags", "Glasses", "Balaclava", "Accessories",
                "Hair", "FacialHair", "Headwear")


def load_manifest() -> dict:
    return read_json(EXPORT_MANIFEST)
