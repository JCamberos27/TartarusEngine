"""Model .meta handling for the Quantum Casual Bundle import: FBX material names, materialRemap, GUID-keeping metas.

materialRemap keys
- The engine matches a model's materialRemap on Model::MeshMaterial(i).Name (AssetLibrary::ApplyMaterialRemap), which is
  Assimp's aiMaterial name (Model.cpp, every MaterialMode keeps it). That is NOT the UE slot name in manifest.json:
  the UE FBX exporter names each FBX material after the material ASSIGNED to the slot, e.g.
      SKM_Hoodie_Black          slot M_Hoodie        -> FBX material MI_Hoodie_Black
      SKM_Quantum_Body_Full     slot M_Quantum_Body  -> FBX material MI_Quantum_Body
  and when two slots use one material Assimp renames the later ones "<name>_<slot index>":
      SKM_Quantum_Body_Full     slots M_EyeRight, M_EyeLeft (both MI_EyeRefractive_Inst)
                                -> MI_EyeRefractive_Inst, MI_EyeRefractive_Inst_4
  predict_fbx_material_names() encodes that rule; fbx_material_names() asks the built engine (--model-report) for the
  names it actually sees, so the remap is always keyed on what the engine matches.

Metas
- write_model_meta() keeps the GUID and every importer key already in the .meta (simplify 0.4 on heads,
  skinIgnoresMeshNode on Quantum_Arms/Quantum_Hands, ...), replaces materialRemap wholesale and sets folder.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

from . import common

ENGINE_EXE = Path(os.environ.get("TARTARUS_ENGINE_EXE", str(common.REPO / "build" / "Release" / "TartarusEngine.exe")))
FBX_MATERIALS_CACHE = Path(__file__).resolve().parent / "fbx_materials.json"

# Windows caps one command line at 32767 characters; stay well under it when batching paths into one process.
_MAX_CMDLINE = 30000


# ------------------------------------------------------------------------------------- FBX material names ---
# "[ModelReport] <path> size x y z min x y z max x y z meshes N materials A|B|C" (src/App/CliTools.cpp ModelReport).
# The path may contain spaces ("Quantum - Modular Casual Bundle"), so the fixed tail anchors the parse.
_NUM = r"\S+"
_REPORT_LINE = re.compile(
    r"^\[ModelReport\] (?P<path>.+?) size " + " ".join([_NUM] * 3) + " min " + " ".join([_NUM] * 3) +
    " max " + " ".join([_NUM] * 3) + r" meshes (?P<meshes>\d+) materials ?(?P<mats>.*)$")
_FAILED_LINE = re.compile(r"^\[ModelReport\] FAILED (?P<path>.+)$")


# Cache keys never hold a user folder (the cache is tracked in a public repo): repo files are repo-relative,
# export files are "<bundle>/..." (relative to common.EXPORT_ROOT); anything else stays absolute.
_ROOTS = (("", common.REPO), ("<bundle>/", common.EXPORT_ROOT))


def _norm(path) -> str:
    """A file path as the cache stores it: forward slashes, case as given, relative to the repo or the export root."""
    p = Path(os.path.abspath(str(path)))
    for tag, root in _ROOTS:
        try:
            return tag + p.relative_to(os.path.abspath(str(root))).as_posix()
        except ValueError:
            continue
    return p.as_posix()


def _key(path) -> str:
    """Comparable form of a file path: _norm, case-folded on Windows (its paths are case-blind)."""
    k = _norm(path)
    return k.casefold() if os.name == "nt" else k


def parse_model_report(text: str) -> dict[str, list[str] | None]:
    """{path as printed: [material names in first-use order]} from --model-report output; None for a FAILED import.

    Lines that aren't model headers (tris/skin/bind detail, [warn] logs, the summary) are ignored."""
    out: dict[str, list[str] | None] = {}
    for raw in text.splitlines():
        line = raw.rstrip("\r\n")
        m = _REPORT_LINE.match(line)
        if m:
            mats = m.group("mats")
            out[m.group("path")] = [n for n in mats.split("|")] if mats else []
            continue
        f = _FAILED_LINE.match(line)
        if f:
            out[f.group("path")] = None
    return out


def _chunks(paths: list[str], fixed: int) -> list[list[str]]:
    """Split paths into batches whose quoted command line stays under _MAX_CMDLINE."""
    batches, cur, size = [], [], fixed
    for p in paths:
        cost = len(p) + 3
        if cur and size + cost > _MAX_CMDLINE:
            batches.append(cur)
            cur, size = [], fixed
        cur.append(p)
        size += cost
    if cur:
        batches.append(cur)
    return batches


def run_model_report(inputs: list[str], exe: Path = ENGINE_EXE, timeout: float | None = 3600) -> str:
    """Runs `<exe> --model-report <inputs...>` (as few processes as the command-line limit allows) and returns stdout.

    TartarusEngine.exe is a GUI-subsystem program, but it inherits redirected std handles, so a piped subprocess
    captures its std::cout/printf (the same as PowerShell's Start-Process -RedirectStandardOutput). It runs from the
    exe's folder like every other CLI tool. The exit code is 1 when any model failed: that is reported per file,
    not raised."""
    if not exe.is_file():
        raise FileNotFoundError(f"engine not built: {exe} (build Release, or set TARTARUS_ENGINE_EXE)")
    text = []
    for batch in _chunks(list(inputs), len(str(exe)) + 32):
        proc = subprocess.run([str(exe), "--model-report", *batch], cwd=str(exe.parent), capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=timeout)
        if "[ModelReport]" not in proc.stdout:
            raise RuntimeError(f"--model-report printed nothing (exit {proc.returncode}): {proc.stderr[-2000:]}")
        text.append(proc.stdout)
    return "\n".join(text)


def _stamp(p: Path) -> list[int]:
    st = p.stat()
    return [st.st_size, st.st_mtime_ns]


def fbx_material_names(fbx_paths, cache: Path | None = FBX_MATERIALS_CACHE, exe: Path = ENGINE_EXE,
                       runner=run_model_report) -> dict[Path, list[str] | None]:
    """{fbx path: material names the engine sees, in first-use order} for each given FBX (None = failed to import).

    Every file not already in the cache (keyed by repo- or export-relative path, see _norm; invalidated by size +
    mtime) is reported by one batched --model-report run (one process per ~30000 characters of paths); results are
    merged back into the cache JSON. `runner(inputs, exe) -> report text` is injectable for tests."""
    paths = [Path(p) for p in fbx_paths]
    store: dict = {}
    if cache is not None and cache.exists():
        try:
            store = common.read_json(cache)
        except (OSError, ValueError):
            store = {}
    entries: dict = store.setdefault("files", {})
    index = {_key(k): k for k in entries}   # case-blind lookup on Windows

    def entry(p: Path) -> dict | None:
        k = index.get(_key(p))
        return entries.get(k) if k else None

    todo = [p for p in paths if not ((hit := entry(p)) and p.exists() and hit.get("stamp") == _stamp(p))]
    if todo:
        found = {_key(k): v for k, v in parse_model_report(runner([str(p) for p in todo], exe)).items()}
        for p in todo:
            if _key(p) not in found:
                raise RuntimeError(f"--model-report did not report {p}")
            old = index.get(_key(p))
            if old:
                entries.pop(old, None)
            entries[_norm(p)] = {"stamp": _stamp(p), "materials": found[_key(p)]}
            index[_key(p)] = _norm(p)
        if cache is not None:
            store["note"] = ("FBX material names as TartarusEngine --model-report sees them (the materialRemap keys); "
                             "written by tools/quantum/casual_bundle/metas.py fbx_material_names(). Keyed by "
                             "repo-relative path (<bundle>/ = the UE export root); stamp = [size, mtime_ns] - a "
                             "changed file is re-reported.")
            store["files"] = dict(sorted(entries.items()))
            common.write_json(cache, store)
    return {p: entry(p)["materials"] for p in paths}


def _material_leaf(ue_path: str) -> str:
    """'/Game/.../MI_Hoodie_Black' or '/Game/.../MI_Hoodie_Black.MI_Hoodie_Black' -> 'MI_Hoodie_Black'."""
    leaf = ue_path.rstrip("/").rsplit("/", 1)[-1]
    return leaf.split(".", 1)[0]


# What Assimp's FBX importer names the material of a mesh the file assigns none to. The UE exporter writes no
# material for a slot whose material didn't resolve (manifest "material": "" - e.g. the headwear SM_ meshes,
# whose materials live in a pack that isn't installed), so every such slot of one mesh lands on this ONE name.
FBX_DEFAULT_MATERIAL = "Fbx Default Material"


def slot_keys(material_slots: list[dict]) -> list[str]:
    """The key of each manifest slot in match_slots' pairs: its name ('slot<i>' when unnamed), and '<name>#<i>' for a
    name an EARLIER slot already used (matched or not), so the importer rebuilds the same keys."""
    keys, seen = [], set()
    for i, s in enumerate(material_slots):
        slot = s.get("slot", "") or f"slot{i}"
        keys.append(f"{slot}#{i}" if slot in seen else slot)
        seen.add(slot)
    return keys


def predict_fbx_material_names(material_slots: list[dict]) -> list[str]:
    """The FBX material name of each manifest material slot, in slot order.

    - the assigned material's asset name ('/Game/.../MI_Hoodie_Black' -> 'MI_Hoodie_Black'), not the slot name;
    - a repeat of an earlier slot's material becomes '<name>_<slot index>' (Assimp's FBX importer de-duplicates
      material names with the material's index, and UE writes one FBX material per slot);
    - a slot with no material -> FBX_DEFAULT_MATERIAL (shared, never suffixed).
    Checked against --model-report for all 1075 exported meshes (2026-11-18): every reported name is predicted."""
    out, seen = [], set()
    for i, s in enumerate(material_slots):
        name = _material_leaf(s.get("material") or "")
        if not name:
            out.append(FBX_DEFAULT_MATERIAL)
            continue
        if name in seen:
            name = f"{name}_{i}"
        seen.add(name)
        out.append(name)
    return out


def match_slots(material_slots: list[dict], fbx_names: list[str] | None) -> tuple[dict[str, str], list[str], list[str]]:
    """Pairs each manifest slot with the FBX material name the engine reports for it.

    Returns (pairs {slot name: FBX material name}, unused slots, problems).
    - A slot name repeated within one mesh is keyed '<slot>#<index>'.
    - unused: slots whose predicted name the engine doesn't report - the slot has no geometry (UE keeps empty
      slots, e.g. 'WorldGridMaterial', 'lambert2'); harmless, nothing to remap.
    - problems: reported names no slot explains, or (None) a model that failed to import - the remap for that
      mesh must be checked by hand."""
    if fbx_names is None:
        return {}, [], ["model failed to import (--model-report FAILED)"]
    predicted = predict_fbx_material_names(material_slots)
    reported = list(fbx_names)
    pairs: dict[str, str] = {}
    unused: list[str] = []
    for i, (slot, name) in enumerate(zip(slot_keys(material_slots), predicted)):
        # The default material sometimes carries the slot index after a space ("Fbx Default Material 10", seen on
        # SKM_Quantum_FaceRig_Beard and two presets); accept either spelling.
        options = [name] + ([f"{name} {i}"] if name == FBX_DEFAULT_MATERIAL else [])
        hit = next((o for o in options if o in reported), None)
        if hit:
            pairs[slot] = hit
        else:
            unused.append(slot)
    explained = set(pairs.values())
    problems = [f"FBX material {name!r} matches no manifest slot" for name in reported if name not in explained]
    return pairs, unused, problems


# ------------------------------------------------------------------------------------------------- metas ---
def _read_meta(meta: Path) -> dict | None:
    """A .meta as a dict, tolerating a BOM; None when absent or unreadable (callers then write a fresh one)."""
    if not meta.exists():
        return None
    try:
        with open(meta, encoding="utf-8-sig") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _write_if_changed(meta: Path, data: dict, before: dict | None) -> bool:
    if before == data:
        return False
    common.write_json(meta, data)
    return True


def write_model_meta(fbx_repo_path: str, remap: dict[str, str], importer_defaults: dict | None = None,
                     importer_overrides: dict | None = None, project: Path = common.PROJECT) -> dict:
    """Writes <fbx>.meta for a model at project-relative `fbx_repo_path` and returns the written dict.

    - guid: the existing one, else a new one (common.new_guid).
    - importer: existing keys win over `importer_defaults` (defaults only fill what is missing); keys in
      `importer_overrides` replace either. Left out entirely when empty.
    - materialRemap: replaced wholesale by `remap` ({FBX material name: .mat repo path}); left out when empty.
    - folder: the FBX's project-relative folder. type "model", metaVersion kept (1 when new). Any other keys
      (displayName, labels...) are kept."""
    asset = project / fbx_repo_path
    meta = common.meta_path(asset)
    before = _read_meta(meta)
    data = dict(before or {})
    data["guid"] = data.get("guid") or common.new_guid()
    data["type"] = "model"
    data.setdefault("metaVersion", 1)
    data["folder"] = Path(fbx_repo_path).parent.as_posix()

    importer = dict(importer_defaults or {})
    existing = data.get("importer")
    if isinstance(existing, dict):
        importer.update(existing)
    importer.update(importer_overrides or {})
    if importer:
        data["importer"] = importer
    else:
        data.pop("importer", None)

    if remap:
        data["materialRemap"] = dict(sorted(remap.items()))
    else:
        data.pop("materialRemap", None)
    _write_if_changed(meta, data, before)
    return data
