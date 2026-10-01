"""Give every material slot of the Quantum character models a material.

The pack's textured parts were wired at import (each model's .meta "materialRemap": FBX material
name -> .mat). What's left is parts the pack ships no textures for - eyes, teeth, eyelid/tear
overlays, brows, hair, beards, fur, some clothing - plus a few slots whose .mat exists under a
different name. This fills those remaps:

  * a slot matching an existing .mat (Vivian Afro skin, caps, glasses, watches...) uses it;
  * everything else gets a plain PBR material (colour, roughness, transparency for the eye overlays,
    double-sided for hair cards): body parts in Materials/Characters/, clothing in
    Materials/Clothing/Generated/.

Slots already in a remap are left alone, so re-running only fills new gaps. Run from the repo root:
    python tools/quantum/fill_quantum_materials.py [--dry-run]
"""
import json
import os
import re
import secrets
import sys

ROOT = os.path.join("project", "assets", "Characters", "Quantum")
PROJECT_REL = "assets/Characters/Quantum"
BODY_PARTS = {"M_Eye", "M_Eye_Cornea", "M_Eye_Overlay", "M_Saliva", "M_Teeth", "M_Cartilage",
              "M_Brows_Lashes", "M_Hair_Dark", "M_Beard_Dark"}
MATS = PROJECT_REL + "/Materials"

# Generated materials: name -> (base colour, roughness, metallic, extra)
# extra: queue (0 opaque / 2 transparent), opacity, doubleSided, roughnessMap
GENERATED = {
    "M_Eye":            ((0.10, 0.07, 0.05), 0.08, 0.0, {}),  # no iris texture in the pack: a dark eye reads best
    "M_Eye_Cornea":     ((1.00, 1.00, 1.00), 0.02, 0.0, {"queue": 2, "opacity": 0.06}),
    "M_Eye_Overlay":    ((0.00, 0.00, 0.00), 1.00, 0.0, {"queue": 2, "opacity": 0.0}),  # edge / occlusion / tear line
    "M_Saliva":         ((1.00, 1.00, 1.00), 0.05, 0.0, {"queue": 2, "opacity": 0.0}),
    "M_Teeth":          ((0.86, 0.83, 0.76), 0.30, 0.0, {}),
    "M_Cartilage":      ((0.70, 0.48, 0.43), 0.45, 0.0, {}),
    "M_Brows_Lashes":   ((0.05, 0.035, 0.025), 0.60, 0.0, {"queue": 2, "opacity": 0.85, "doubleSided": True}),
    "M_Hair_Dark":      ((0.045, 0.03, 0.02), 0.55, 0.0, {"doubleSided": True}),
    "M_Beard_Dark":     ((0.05, 0.035, 0.025), 0.60, 0.0, {"doubleSided": True}),
    "M_Fur":            ((0.55, 0.47, 0.38), 0.90, 0.0, {"doubleSided": True,
                         "roughnessMap": PROJECT_REL + "/Textures/Clothing/Outerwear/Jacket_Winter/Fur/T_FurCollar_Roughness.png"}),
    "M_Headband":       ((0.05, 0.05, 0.05), 0.80, 0.0, {}),
    "M_Bandana":        ((0.45, 0.06, 0.05), 0.85, 0.0, {}),
    "M_Goggles":        ((0.08, 0.08, 0.08), 0.40, 0.0, {}),
    "M_Jacket_M65":     ((0.25, 0.27, 0.16), 0.85, 0.0, {}),
    "M_Flip_Flops":     ((0.10, 0.10, 0.10), 0.70, 0.0, {}),
    "M_Shirt_Adventure":((0.55, 0.50, 0.38), 0.85, 0.0, {}),
    "M_Pants_Inboots":  ((0.12, 0.12, 0.13), 0.85, 0.0, {}),
    "M_Tank_Top":       ((0.80, 0.80, 0.78), 0.90, 0.0, {}),
}

def gen_dir(name):
    return "Materials/Characters" if name in BODY_PARTS else "Materials/Clothing/Generated"

def gen(name):
    return f"{PROJECT_REL}/{gen_dir(name)}/{name}.mat"

def existing(rel):
    return f"{MATS}/{rel}"

# (model path regex or None, slot regex, target .mat). First match wins; slot names are matched
# after dropping " Slot #n" and trailing digits (the FBX exporter's duplicate suffixes).
RULES = [
    # Vivian Afro skin: named MI_F_Vivian_Basemesh_Afro_* in the FBX, M_F_Vivian_*_Afro.mat on disk.
    (None, r"MI_F_Vivian_Basemesh_Afro_Head", existing("Characters/M_F_Vivian_Head_Afro.mat")),
    (None, r"MI_F_Vivian_Basemesh_Afro_Body", existing("Characters/M_F_Vivian_Body_Afro.mat")),
    (None, r"MI_F_Vivian_Basemesh_Afro_Arms", existing("Characters/M_F_Vivian_Arms_Afro.mat")),
    # Eyes and face overlays.
    (None, r"M_Eye(Left|Right)|eye(Left|Right)_lod0_mesh", gen("M_Eye")),
    (None, r"M_Eyeshell", gen("M_Eye_Cornea")),
    (None, r"M_EyeEdge|MI_F_EyeOcclussion|MI_F_Lacrimal", gen("M_Eye_Overlay")),
    (None, r"M_Saliva|MI_F_Saliva", gen("M_Saliva")),
    (None, r"M_Teeth|MI_F_Teeth", gen("M_Teeth")),
    (None, r"M_Cartilage|MI_F_Cartilage", gen("M_Cartilage")),
    (None, r"M_Quantum_Body_Facial_Details|Face_Detals|MI_Hair_Eyebrows_Eyelids", gen("M_Brows_Lashes")),
    # Glasses sharing a hair model: their own textured materials.
    (None, r"M_Aviator(_Glass)?", existing("Clothing/Glasses/Glasses Aviator/M_Sunglasses_Brown.mat")),
    (None, r"M_Bracelet", existing("Clothing/Accessories/F_Bracelet/M_Bracelet.mat")),
    (None, r"M_Watch", existing("Clothing/Accessories/Watches_Military/M_Watches_Military_Black.mat")),
    # Untextured clothing.
    (None, r"MI_F_Headband|SKM_Headband|M_Elastic", gen("M_Headband")),
    (None, r"M_Bandana", gen("M_Bandana")),
    (r"Goggles", r".*", gen("M_Goggles")),
    (None, r"M_Jacket_M65", gen("M_Jacket_M65")),
    (None, r"M_Flip_Flops", gen("M_Flip_Flops")),
    (None, r"M_Shirt_Adventure|M_Police_Shirt_Short", gen("M_Shirt_Adventure")),
    (None, r"M_Pants_Inboots", gen("M_Pants_Inboots")),
    (None, r"M_F_Tank_Top", gen("M_Tank_Top")),
    (None, r"M_FurCollar|M_Jacket_Winter_Fur|Fur_New", gen("M_Fur")),
    # Hair and beards (no textures in the pack): every remaining slot of a hair model is hair.
    (None, r"M_Beard", gen("M_Beard_Dark")),
    (r"[\\/]Hair[\\/]|Haircut|_Hair\b|WithHair|Hat_Warm_Braids", r".*", gen("M_Hair_Dark")),
    (None, r"MI_F_Vivian_Head_Scalp", gen("M_Hair_Dark")),
]

def fbx_material_names(path):
    """Material object names in a binary FBX ("name\\x00\\x01Material")."""
    data = open(path, "rb").read().decode("latin-1")
    seen = []
    for m in re.finditer(r"([\x20-\x7e]{1,80})\x00\x01Material", data):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen

def normalise(slot):
    return re.sub(r"\d+$", "", re.sub(r" Slot #\d+$", "", slot))

def pick(model_path, slot):
    # Both spellings: the suffix-stripped one, and the raw one ("M_Jacket_M65" isn't a suffix).
    names = (normalise(slot), slot)
    for model_re, slot_re, target in RULES:
        if model_re and not re.search(model_re, model_path):
            continue
        if any(re.fullmatch(slot_re, s) or (slot_re != ".*" and re.match(slot_re, s)) for s in names):
            return target
    return None

def write_json(path, obj, dry):
    if dry:
        return
    # The engine writes no trailing newline; some imported files have one. Keep whichever it had.
    trailing = os.path.exists(path) and open(path, "rb").read().endswith(b"\n")
    tmp = path + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        if trailing:
            f.write("\n")
    os.replace(tmp, path)

def write_generated(name, dry):
    colour, rough, metal, extra = GENERATED[name]
    path = os.path.join(ROOT, *gen_dir(name).split("/"), name + ".mat")
    if os.path.exists(path):
        return False
    props = {
        "_AOMap": "", "_AlbedoMap": "", "_BaseColor": list(colour), "_DoubleSided": bool(extra.get("doubleSided")),
        "_EmissiveColor": [0.0, 0.0, 0.0], "_EmissiveMap": "", "_EmissiveStrength": 1.0,
        "_Metallic": metal, "_MetallicMap": "", "_MetallicRoughnessMap": "", "_NormalMap": "",
        "_Roughness": rough, "_RoughnessMap": extra.get("roughnessMap", ""),
        "_Triplanar": False, "_TriplanarScale": 1.0,
    }
    mat = {"factorsScaleMaps": True, "matVersion": 2, "name": name, "properties": props,
           "shader": "engine://Standard.shader"}
    if extra.get("queue"):
        mat["renderQueue"] = extra["queue"]
    if "opacity" in extra and extra["opacity"] != 1.0:
        mat["opacity"] = extra["opacity"]
    if not dry:
        os.makedirs(os.path.dirname(path), exist_ok=True)
    write_json(path, mat, dry)
    write_json(path + ".meta", {"folder": f"{PROJECT_REL}/{gen_dir(name)}", "guid": secrets.token_hex(8),
                                "metaVersion": 1, "type": "material"}, dry)
    return True

def main():
    dry = "--dry-run" in sys.argv
    created, filled, unmatched = set(), 0, []
    for dirpath, _, files in os.walk(os.path.join(ROOT, "Models")):
        for fn in sorted(files):
            if not fn.lower().endswith(".fbx"):
                continue
            model = os.path.join(dirpath, fn)
            meta_path = model + ".meta"
            meta = json.load(open(meta_path)) if os.path.exists(meta_path) else {}
            remap = dict(meta.get("materialRemap") or {})
            changed = False
            for slot in fbx_material_names(model):
                if slot in remap:
                    continue
                target = pick(model.replace("\\", "/"), slot)
                if not target:
                    unmatched.append(f"{model}: {slot}")
                    continue
                if target.rsplit("/", 1)[1][:-4] in GENERATED:
                    name = target.rsplit("/", 1)[1][:-4]
                    if write_generated(name, dry):
                        created.add(name)
                elif not os.path.exists(os.path.join("project", *target.split("/"))):
                    unmatched.append(f"{model}: {slot} (missing {target})")
                    continue
                remap[slot] = target
                filled += 1
                changed = True
            if changed:
                meta["materialRemap"] = remap
                write_json(meta_path, meta, dry)
    print(f"{'[dry run] ' if dry else ''}filled {filled} slot(s); created {len(created)} material(s): {', '.join(sorted(created))}")
    for u in unmatched:
        print("  unmatched:", u)

if __name__ == "__main__":
    main()
