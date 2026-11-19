"""Who references the Quantum pack: the index behind retirement and the dangling-reference report.

    python -B -m casual_bundle.refs            (from tools/quantum; read-only) prints the summary of the real repo

What counts as a reference (src/Game/SceneSerializer.cpp, AssetLibrary, OutfitSystem, Wardrobe):
- a quoted project path under assets/Characters/Quantum, in any scanned text file (exact, or matching only when case is
  ignored - Windows resolves those, git and other platforms don't). A path that names a folder is a "folder" reference
  (settings.json assetFolders, the wardrobe's itemFolders, code building paths): it keeps no file alive.
- a GUID of a Quantum .meta (scenes/prefabs store {"path", "pathGuid"} and library* lists; .mat has textureGuids).
- a .wardrobe's root-relative model paths ("Models/European/Quantum_Torso.fbx" under its "root").
- implicit, applied while marking (retire_candidates): a live clothing .mat keeps its sibling .mat files (colourways
  are "the materials beside it", OutfitSystem::ColourGroups; not in Materials/Characters or */Generated), and a live
  skin material keeps its race variants (<base><skin suffix>.mat, Wardrobe::SkinMaterialFor).

Scanned (relative to the repo root): project/**/{*.mat,*.meta,*.wardrobe,*.outfit,*.fpsanim,*.prefab,*.mmconfig,*.json},
tests/**/*.json, tools/**/{*.py,*.json,*.mmconfig,*.ps1}, src/**/{*.cpp,*.h,*.inl} - minus build output, the editor's
project/Library cache and this package (whose tests hold made-up paths).
"""
from __future__ import annotations

import csv
import json
import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import NamedTuple

from . import common

QUANTUM_PREFIX = common.QUANTUM + "/"
PAYLOAD_EXTS = (".fbx", ".png", ".mat", ".jpg", ".jpeg", ".tga", ".bmp")   # what retirement may delete

# (top folder, extensions) pairs to scan; folder names to skip anywhere below them.
SCAN = (
    ("project", (".mat", ".meta", ".wardrobe", ".outfit", ".fpsanim", ".prefab", ".mmconfig", ".json")),
    ("tests", (".json",)),
    ("tools", (".py", ".json", ".mmconfig", ".ps1")),
    ("src", (".cpp", ".h", ".inl")),
)
SKIP_DIRS = {".git", "build", "extern", "__pycache__", "node_modules", "Library", "bin", "obj"}
SKIP_PREFIXES = ("tools/quantum/casual_bundle/",)

# A quoted Quantum path, optionally written with the project/ prefix. Case-blind: a wrongly-cased reference is still one.
_PATH = re.compile(r"""["'](?:project/)?(assets/Characters/Quantum(?:/[^"'\r\n]*)?)""", re.IGNORECASE)
_GUID = re.compile(r'"([0-9a-f]{16})"')
# Lines whose 16-hex value is not an asset reference: a .meta's own "guid", a scene entity's "guid".
_OWN_GUID = re.compile(r'^\s*"guid"\s*:')
_META_FOLDER = re.compile(r'^\s*"folder"\s*:')
_ROOT_REL = re.compile(r'"([^"\r\n]+\.(?:fbx|mat|png))"', re.IGNORECASE)


class Ref(NamedTuple):
    file: str       # referencing file, repo-relative ("project/scenes/Forest.json", "src/Tests/UnitTests.cpp")
    line: int       # 1-based
    kind: str       # path | path-ci | guid | folder | missing   (missing = names no Quantum file today)
    target: str     # project-relative path referenced (canonical spelling when it exists; as written for missing)
    text: str       # what was written (the path, or the GUID)


@dataclass
class Wardrobe:
    file: str                     # project-relative path of the .wardrobe
    root: str
    skin_bases: list[str]
    skin_suffixes: list[str]
    item_folders: list[str]       # project-relative
    exclude_folders: list[str] = field(default_factory=list)   # folder names that hide an item (Wardrobe::Classify)

    def catalog(self, models) -> list[str]:
        """The models the outfit system lists as items: under an item folder, no excluded folder on the way."""
        out = []
        ex = {x.casefold() for x in self.exclude_folders}
        for p in models:
            for d in self.item_folders:
                if p.casefold().startswith(d.casefold() + "/"):
                    if not any(f.casefold() in ex for f in p.split("/")[:-1]):
                        out.append(p)
                    break
        return out


@dataclass
class RefIndex:
    repo_root: Path
    quantum_files: set[str] = field(default_factory=set)   # every file under Quantum on disk, every .meta, CSV payloads
    csv_paths: set[str] = field(default_factory=set)       # external_assets.csv rows under Quantum
    guid_to_path: dict[str, str] = field(default_factory=dict)
    path_to_guid: dict[str, str] = field(default_factory=dict)
    refs: list[Ref] = field(default_factory=list)
    refs_by_target: dict[str, list[Ref]] = field(default_factory=lambda: defaultdict(list))
    refs_by_guid: dict[str, list[Ref]] = field(default_factory=lambda: defaultdict(list))
    wardrobes: list[Wardrobe] = field(default_factory=list)
    scanned: Counter = field(default_factory=Counter)      # files scanned per source type
    _ci: dict[str, str] = field(default_factory=dict)      # casefold -> canonical, for quantum_files
    _dirs: dict[str, str] = field(default_factory=dict)    # casefold folder -> canonical folder

    # --------------------------------------------------------------------------------------------- queries ---
    @property
    def assets(self) -> set[str]:
        """Retirable payload paths: every fbx/png/mat (...) under Quantum on disk or in the CSV, plus the asset of
        every .meta whose payload is gone (an orphan .meta is retired by retiring its asset path)."""
        out = {p for p in self.quantum_files if p.lower().endswith(PAYLOAD_EXTS)}
        out |= {p[:-5] for p in self.quantum_files if p.endswith(".meta") and p[:-5].lower().endswith(PAYLOAD_EXTS)}
        return out

    def canon(self, path: str) -> str:
        """The on-disk/CSV spelling of a project-relative Quantum path (as given when unknown)."""
        return self._ci.get(path.casefold(), path)

    def resolve(self, ref: Ref) -> str:
        return self.guid_to_path.get(ref.text, ref.target) if ref.kind == "guid" else ref.target


# ------------------------------------------------------------------------------------------------- helpers ---
def source_type(file: str) -> str:
    """Bucket of a referencing file for reports: mat, meta, wardrobe, outfit, fpsanim, prefab, mmconfig, scene,
    ai, template, settings, json, src, tools."""
    f = file.replace("\\", "/")
    ext = os.path.splitext(f)[1].lower()
    if ext in (".mat", ".meta", ".wardrobe", ".outfit", ".fpsanim", ".prefab", ".mmconfig"):
        return ext[1:]
    if ext in (".cpp", ".h", ".inl"):
        return "src"
    if ext in (".py", ".ps1"):
        return "tools"
    if ext == ".json":
        if f == "project/settings.json":
            return "settings"
        if f.startswith(("project/scenes/", "tests/")):
            return "scene"
        if f.startswith("project/assets/AI/"):
            return "ai"
        if f.startswith("tools/scenes/"):
            return "template"
    return "json"


def owner_of(file: str) -> str | None:
    """The Quantum asset whose outgoing references `file` holds (an asset's .meta speaks for the asset), as a
    project-relative path; None for a file outside the pack (its references always count)."""
    f = file.replace("\\", "/")
    if not f.startswith("project/" + QUANTUM_PREFIX):
        return None
    rel = f[len("project/"):]
    return rel[:-5] if rel.endswith(".meta") else rel


def _iter_files(repo: Path):
    for top, exts in SCAN:
        base = repo / top
        if not base.is_dir():
            continue
        for parent, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for name in files:
                if name.lower().endswith(exts):
                    p = Path(parent) / name
                    rel = p.relative_to(repo).as_posix()
                    if not rel.startswith(SKIP_PREFIXES):
                        yield p, rel


def _read_lines(p: Path):
    with open(p, encoding="utf-8-sig", errors="replace") as f:
        for i, line in enumerate(f, 1):
            yield i, line


def _load_wardrobe(p: Path, rel_project: str) -> Wardrobe | None:
    try:
        with open(p, encoding="utf-8-sig") as f:
            j = json.load(f)
    except (OSError, ValueError):
        return None
    root = (j.get("root") or "").strip("/")
    suffixes = sorted({r.get("skin", "") for b in (j.get("bodies") or {}).values() for r in b.get("races", [])} - {""})
    return Wardrobe(rel_project, root, list(j.get("skinMaterials") or []), suffixes,
                    [f"{root}/{d}".strip("/") for d in (j.get("itemFolders") or [])],
                    list(j.get("excludeFolders") or []))


# ---------------------------------------------------------------------------------------------------- scan ---
def _quantum_inventory(index: RefIndex, project: Path) -> None:
    base = project / common.QUANTUM
    if base.is_dir():
        for parent, _, files in os.walk(base):
            for name in files:
                index.quantum_files.add((Path(parent) / name).relative_to(project).as_posix())
    csv_file = project / "external_assets.csv"
    if csv_file.exists():
        with open(csv_file, encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                p = row.get("path") or ""
                if p.casefold().startswith(QUANTUM_PREFIX.casefold()):
                    index.csv_paths.add(p)
    index.quantum_files |= index.csv_paths
    for p in sorted(index.quantum_files):
        index._ci.setdefault(p.casefold(), p)
        parts = p.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            index._dirs.setdefault(d.casefold(), d)
    for p in sorted(index.quantum_files):
        if p.endswith(".meta"):
            try:
                with open(project / p, encoding="utf-8-sig") as f:
                    g = json.load(f).get("guid")
            except (OSError, ValueError, AttributeError):
                continue
            if g:
                index.guid_to_path[g] = p[:-5]
                index.path_to_guid[p[:-5]] = g


def _classify(index: RefIndex, written: str) -> tuple[str, str]:
    # "Model.fbx#Clip" names a clip inside the model: the reference is to the model.
    p = re.sub(r"/+", "/", written.replace("\\", "/")).split("#", 1)[0].rstrip("/ ")
    if p in index.quantum_files:
        return "path", p
    hit = index._ci.get(p.casefold())
    if hit:
        return "path-ci", hit
    d = index._dirs.get(p.casefold())
    if d:
        return "folder", d
    return "missing", p


def _add(index: RefIndex, ref: Ref) -> None:
    index.refs.append(ref)
    if ref.kind == "guid":
        index.refs_by_guid[ref.text].append(ref)
        index.refs_by_target[index.guid_to_path[ref.text]].append(ref)
    else:
        index.refs_by_target[ref.target].append(ref)


def scan(repo_root: Path = common.REPO) -> RefIndex:
    """Reads every file that can reference the pack (read-only) and returns the RefIndex."""
    repo = Path(repo_root)
    project = repo / "project"
    index = RefIndex(repo)
    _quantum_inventory(index, project)
    guids = index.guid_to_path

    for p, rel in _iter_files(repo):
        kind = source_type(rel)
        index.scanned[kind] += 1
        is_meta = rel.endswith(".meta")
        wardrobe = None
        if rel.lower().endswith(".wardrobe"):
            wardrobe = _load_wardrobe(p, rel[len("project/"):] if rel.startswith("project/") else rel)
            if wardrobe:
                index.wardrobes.append(wardrobe)
        try:
            lines = list(_read_lines(p)) if p.stat().st_size < 4_000_000 else _read_lines(p)
        except OSError:
            continue
        for n, line in lines:
            low = line.lower()
            if "characters/quantum" in low:
                for m in _PATH.finditer(line):
                    if is_meta and _META_FOLDER.match(line):
                        continue
                    k, target = _classify(index, m.group(1))
                    _add(index, Ref(rel, n, k, target, m.group(1)))
            if '"' in line and not _OWN_GUID.match(line):
                for g in _GUID.findall(line):
                    if g in guids:
                        _add(index, Ref(rel, n, "guid", guids[g], g))
            if wardrobe and wardrobe.root:
                for m in _ROOT_REL.finditer(line):
                    v = m.group(1)
                    if not v.lower().startswith("assets/"):
                        k, target = _classify(index, f"{wardrobe.root}/{v}")
                        _add(index, Ref(rel, n, k, target, v))
        if wardrobe:
            for folder in wardrobe.item_folders:
                k, target = _classify(index, folder)
                _add(index, Ref(rel, 0, "folder" if k != "missing" else k, target, folder))
    return index


# ------------------------------------------------------------------------------------------------ marking ---
def _norm_set(index: RefIndex, paths) -> tuple[set[str], tuple[str, ...]]:
    """Canonical-spelled paths, plus folder prefixes (entries ending in '/') kept apart, case-folded."""
    files, prefixes = set(), []
    for p in paths or ():
        p = p.replace("\\", "/")
        if p.endswith("/"):
            prefixes.append(p.casefold())
        else:
            files.add(index.canon(p))
    return files, tuple(prefixes)


def _implicit(index: RefIndex, mat: str, universe_ci: dict[str, str]) -> list[str]:
    """Files a live .mat keeps alive without naming them: its colourway siblings, its skin race variants."""
    out = []
    folder, _, name = mat.rpartition("/")
    stem = name[:-4] if name.lower().endswith(".mat") else name
    low = ("/" + folder + "/").casefold()
    if "/materials/characters/" not in low and "/generated/" not in low:
        prefix = (folder + "/").casefold()
        out += [c for k, c in universe_ci.items()
                if k.startswith(prefix) and "/" not in k[len(prefix):] and k.endswith(".mat")]
    for w in index.wardrobes:
        for base in w.skin_bases:
            if stem.casefold() in {(base + s).casefold() for s in [""] + w.skin_suffixes}:
                for s in [""] + w.skin_suffixes:
                    hit = universe_ci.get(f"{folder}/{base}{s}.mat".casefold())
                    if hit:
                        out.append(hit)
    return out


def live_set(index: RefIndex, keep=(), will_exist=(), rewrites: dict[str, set[str]] | None = None,
             catalog_is_reference: bool = False) -> set[str]:
    """Every Quantum path reachable from a live root (mark phase of retire_candidates)."""
    universe = index.assets
    universe_ci = {p.casefold(): p for p in universe}
    keep_files, keep_prefixes = _norm_set(index, keep)
    will_files, _ = _norm_set(index, will_exist)

    edges: dict[str, set[str]] = defaultdict(set)
    roots: set[str] = set(keep_files) | will_files
    roots |= {p for p in universe if p.casefold().startswith(keep_prefixes)} if keep_prefixes else set()
    if catalog_is_reference:
        models = [p for p in universe if p.lower().endswith(".fbx")]
        for w in index.wardrobes:
            roots |= set(w.catalog(models))
    for ref in index.refs:
        if ref.kind in ("folder", "missing"):
            continue
        target = index.resolve(ref)
        owner = owner_of(ref.file)
        if owner is None or owner not in universe:
            roots.add(target)             # from outside the pack, or from the pack's .wardrobe: always counts
        else:
            edges[owner].add(target)
    for owner, targets in (rewrites or {}).items():
        edges[index.canon(owner)] = {index.canon(t) for t in targets}

    alive: set[str] = set()
    stack = list(roots)
    while stack:
        p = stack.pop()
        if p in alive:
            continue
        alive.add(p)
        stack.extend(edges.get(p, ()))
        if p.lower().endswith(".mat"):
            stack.extend(_implicit(index, p, universe_ci))
    return alive


def retire_candidates(index: RefIndex, keep=(), will_exist=(), rewrites: dict[str, set[str]] | None = None,
                      catalog_is_reference: bool = False) -> list[str]:
    """Quantum payloads (fbx/png/mat...) nothing live references after the import, sorted.

    NOTE the wardrobe lists clothing by FOLDER (itemFolders "Models/Clothing"): an item no outfit, scene or test
    names has no reference at all (today 25 clothing FBX, e.g. the Inboots pants and SKM_Boots_Socks from the
    keep-list). By default that folder catalog keeps nothing alive - old items the import doesn't rewrite are what
    should go - so every clothing item to keep must be on `keep` (the map's keep-old rows). catalog_is_reference=True
    instead treats every catalog model (minus excludeFolders) as referenced.

    Live = the keep-list (paths, or folder prefixes ending in '/'), every path the import writes (`will_exist`),
    and everything referenced from outside the pack; then whatever a live pack file references (its .meta's
    materialRemap, a .mat's textures and textureGuids, colourway siblings, skin variants), transitively. A file only
    referenced by retired files is retired too - this is the fixed point of "drop unreferenced, repeat", and it
    also retires cycles of files that only reference each other.
    `rewrites` {asset: {targets}} replaces an asset's outgoing references with what the import WILL write (a model's
    new materialRemap, a .mat's new textures), so a dry run can be answered before the files change.
    Each retired asset's .meta goes with it (the caller deletes both)."""
    alive = live_set(index, keep, will_exist, rewrites, catalog_is_reference)
    return sorted(index.assets - alive)


def dangling(index: RefIndex, will_exist=(), retired=()) -> list[Ref]:
    """References that will point at nothing after the import: their target is not on disk/CSV today (and not
    written by the import) or is retired, read from a file that survives. Folder references dangle when no file is
    left under the folder. Sorted by file and line."""
    will_files, _ = _norm_set(index, will_exist)
    retired_set = {index.canon(p) for p in retired}
    after = (index.quantum_files - retired_set - {p + ".meta" for p in retired_set}) | will_files
    after_ci = {p.casefold() for p in after}
    after_dirs = set()
    for p in after_ci:
        parts = p.split("/")[:-1]
        after_dirs |= {"/".join(parts[:i]) for i in range(1, len(parts) + 1)}
    out = []
    for ref in index.refs:
        owner = owner_of(ref.file)
        if owner is not None and owner in retired_set:
            continue
        target = index.resolve(ref).casefold()
        if ref.kind == "folder":
            if target not in after_dirs:
                out.append(ref)
        elif target not in after_ci:
            out.append(ref)
    return sorted(out, key=lambda r: (r.file, r.line, r.target))


# ------------------------------------------------------------------------------------------------- report ---
def summary(index: RefIndex, top: int = 15) -> dict:
    by_ext = Counter(os.path.splitext(p[:-5] if p.endswith(".meta") else p)[1].lower() + (".meta" if p.endswith(".meta") else "")
                     for p in index.quantum_files)
    refs_by_type = Counter(source_type(r.file) for r in index.refs)
    files_by_type = Counter()
    seen = set()
    for r in index.refs:
        if r.file not in seen:
            seen.add(r.file)
            files_by_type[source_type(r.file)] += 1
    per_file = Counter(r.file for r in index.refs)
    # References from outside the pack are what retirement and the swap must not break.
    external = Counter(r.file for r in index.refs if owner_of(r.file) is None)
    return {
        "quantum_files": len(index.quantum_files),
        "quantum_by_ext": dict(sorted(by_ext.items())),
        "assets": len(index.assets),
        "csv_rows": len(index.csv_paths),
        "csv_only": len([p for p in index.csv_paths if not (index.repo_root / "project" / p).exists()]),
        "guids": len(index.guid_to_path),
        "refs": len(index.refs),
        "refs_by_kind": dict(Counter(r.kind for r in index.refs)),
        "refs_by_source_type": dict(refs_by_type.most_common()),
        "referencing_files_by_type": dict(files_by_type.most_common()),
        "top_files": per_file.most_common(top),
        "top_external_files": external.most_common(top),
        "missing_today": sorted({(r.file, r.target) for r in index.refs if r.kind == "missing"}),
        "case_mismatch": sorted({(r.file, r.text) for r in index.refs if r.kind == "path-ci"}),
        "unreferenced_today": len(retire_candidates(index)),
        "unreferenced_today_with_catalog": len(retire_candidates(index, catalog_is_reference=True)),
    }


def main(argv: list[str]) -> int:
    s = summary(scan())
    for k, v in s.items():
        if isinstance(v, list):
            print(f"{k}: {len(v)}")
            for item in v[:40]:
                print("   ", item)
        else:
            print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
