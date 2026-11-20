"""Import the complete Quantum Modular Casual Bundle over the old partial Quantum pack (docs/QUANTUM_SWAP.md).

    python tools/quantum/import_casual_bundle.py                 # dry run: plan + report, writes nothing
    python tools/quantum/import_casual_bundle.py --apply         # write meshes, textures, .mat, .meta, CSV rows; retire
    python tools/quantum/import_casual_bundle.py --apply --only "Male/Tops/*"   # just some item groups (no retire/CSV)

Inputs: tools/quantum/casual_bundle_map.csv (casual_bundle/build_map.py), the export's manifest.json + files
(common.EXPORT_ROOT), tools/quantum/casual_bundle/fbx_materials.json (real FBX material names, metas.py).
Output: tools/quantum/casual_bundle/last_import.json (everything decided, warnings, retire list, dangling refs).

Re-runnable: cooks skip up-to-date textures, metas keep their GUIDs, unchanged files aren't rewritten.
"""
from __future__ import annotations

import argparse
import filecmp
import fnmatch
import json
import re
import shutil
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from casual_bundle import common, csvrows, materials, metas, refs, textures  # noqa: E402
from casual_bundle.common import CAP_BODY, CAP_CLOTHING, QUANTUM  # noqa: E402

REPORT = Path(__file__).resolve().parent / "casual_bundle" / "last_import.json"
WRITE = ("replace", "new")


def leaf(pkg: str) -> str:
    return pkg.rstrip("/").rsplit("/", 1)[-1].split(".", 1)[0]


def is_skin_material(repo_path: str) -> bool:
    return "/Materials/Characters/" in repo_path


def old_slot_table() -> tuple[dict[str, dict[str, str]], dict[str, str]]:
    """Before anything is written: every existing Quantum model meta's materialRemap.
    Returns ({fbx repo path: {old FBX material name: .mat}}, {slot name: .mat} across all metas). The old Unity-era
    FBX named materials after their SLOTS (M_EyeLeft, M_Teeth...), so a slot the import has no material for (the
    eye/teeth/brow MetaHuman materials the engine keeps its own versions of) falls back to what the old file used."""
    per_file, by_slot = {}, {}
    for meta in (common.PROJECT / QUANTUM).rglob("*.fbx.meta"):
        try:
            remap = common.read_json(meta).get("materialRemap") or {}
        except ValueError:
            continue
        rel = meta.relative_to(common.PROJECT).as_posix()[:-5]
        per_file[rel] = dict(remap)
        for k, v in remap.items():
            by_slot.setdefault(k, v)
            by_slot.setdefault(slot_norm(k), v)
    return per_file, by_slot


def slot_norm(slot: str) -> str:
    """Slot names differ between exports only by a trailing counter (old MI_F_Teeth29, new MI_F_Teeth1)."""
    s = re.sub(r"\d+$", "", slot).casefold()
    return "mi_f_" + s[5:] if s.startswith("m__f_") else s          # new export's 'M__F_Lacrimal' = old 'MI_F_Lacrimal'


def plan_from_used_textures(ue_json: dict, tex_path: dict, name: str, ue_name: str) -> materials.MaterialPlan | None:
    """A base UE Material the converter has no family for (the glasses' translucent lenses, the female scalp, the
    braids' master): build an opaque/cut-out plan from the textures the graph samples, like the old Unity-era .mat
    files did (the engine has no translucency). Returns None when no colour texture is found."""
    used = ue_json.get("used_textures") or []
    plan = materials.MaterialPlan(ue_name=ue_name, family="master", name=name)
    for pkg in used:
        low = leaf(pkg).lower()
        repo = tex_path.get(pkg, "")
        if not repo:
            continue
        if "normal" in low:
            plan.textures.append(materials.TextureUse("_NormalMap", pkg, repo, "normal"))
            plan.normal_flip_y = True
        elif "occlusionroughnessmetallic" in low or low.endswith(("_orm", "_mra")):
            plan.textures.append(materials.TextureUse("_MetallicRoughnessMap", pkg, repo, "data"))
            plan.textures.append(materials.TextureUse("_AOMap", pkg, repo, "data"))
        elif ("basecolor" in low or "diffuse" in low or "albedo" in low) and not plan.albedo_path:
            plan.textures.append(materials.TextureUse("_AlbedoMap", pkg, repo, "color"))
            plan.albedo_path = repo
        else:
            plan.warnings.append(f"used texture {leaf(pkg)} not mapped")
    if not plan.albedo_path:
        # Hair-card style graph (the female scalp: alpha + root gradient + normal, colour from graph constants):
        # bake like hair, in the facial-hair MIs' dark brown.
        alpha = next((p for p in used if leaf(p).lower().endswith("_alpha") and tex_path.get(p)), None)
        root = next((p for p in used if leaf(p).lower().endswith("_root") and tex_path.get(p)), None)
        if not (alpha and root):
            return None
        plan.textures = [t for t in plan.textures if t.slot == "_NormalMap"]
        plan.bake = materials.Bake("hair", materials.default_baked_path(tex_path[alpha], ue_name, "BaseColor"),
                                   {"alpha": alpha, "root": root},
                                   {"root_color": (0.0118, 0.0039, 0.0), "tip_color": (0.0356, 0.016, 0.004)})
        plan.albedo_path = plan.bake.repo_path
        plan.double_sided, plan.metallic, plan.alpha_cutoff = True, 0.0, materials.HAIR_CUTOFF
        plan.warnings.append("hair-card base Material: albedo baked from alpha + root in the facial-hair dark brown")
        return plan
    plan.warnings.append(f"{ue_json.get('blend_mode')} base Material rebuilt from its textures (engine has no translucency)")
    return plan


def inferred_material(row: common.MapRow) -> str:
    """'material inferred: /Game/...' in a mesh row's notes (build_map.py, for meshes with no slot material)."""
    tag = "material inferred:"
    if tag in row.notes:
        return row.notes.split(tag, 1)[1].split(";", 1)[0].strip()
    return ""


class Importer:
    def __init__(self, apply: bool, only: str | None, force: bool):
        self.apply, self.only, self.force = apply, only, force
        self.rows = common.read_map()
        self.manifest = common.load_manifest()["assets"]
        self.report: dict = {"apply": apply, "only": only, "problems": [], "warnings": {}, "counts": {}}
        self.problems: list[str] = self.report["problems"]

    # ------------------------------------------------------------------------------------------- selection ---
    def selected(self, r: common.MapRow) -> bool:
        return not self.only or fnmatch.fnmatch(r.item_group, self.only)

    def validate(self) -> None:
        seen = defaultdict(list)
        for r in self.rows:
            if r.action not in common.ACTIONS:
                self.problems.append(f"bad action {r.action!r}: {r.ue_asset or r.repo_path}")
            if r.action in WRITE:
                seen[r.repo_path.casefold()].append(r.ue_asset)
                if not r.repo_path.startswith(QUANTUM + "/"):
                    self.problems.append(f"repo path outside the pack: {r.repo_path}")
                if "/textures/clothing/hats/" in r.repo_path.casefold():
                    self.problems.append(f"repo path in the excluded Hats folder (.gitignore:82): {r.repo_path}")
                if not common.export_abs(r.source_file).exists():
                    self.problems.append(f"export file missing: {r.source_file}")
            if "UNCLASSIFIED" in r.notes:
                self.problems.append(f"unclassified: {r.ue_asset} ({r.notes})")
        for k, v in seen.items():
            if len(v) > 1:
                self.problems.append(f"{len(v)} rows write {k}: {v[:4]}")

    # ------------------------------------------------------------------------------------------- materials ---
    def plan_materials(self) -> None:
        self.tex_path = {r.ue_asset: r.repo_path for r in self.rows if r.role == "texture" and r.action in WRITE}
        self.mat_row = {r.ue_asset: r for r in self.rows if r.role == "material" and r.action in WRITE}
        self.plans: dict[str, materials.MaterialPlan] = {}
        for ue, r in sorted(self.mat_row.items()):
            if not self.selected(r):
                continue
            ue_json = materials.load_ue_material(common.export_abs(r.source_file))
            plan = materials.plan_material(ue_json, self.tex_path, Path(r.repo_path).stem, ue_name=leaf(ue))
            if not plan.supported and ue_json.get("class") == "Material":
                plan = plan_from_used_textures(ue_json, self.tex_path, Path(r.repo_path).stem, leaf(ue)) or plan
            if not plan.supported:
                self.problems.append(f"material {ue} mapped {r.action} but has no engine mapping: {plan.warnings}")
                continue
            self.plans[ue] = plan
            if plan.warnings:
                self.report["warnings"][r.repo_path] = plan.warnings

        # Textures to cook: (repo path) -> (UE texture, kind, cap). A texture shared by a skin and a garment keeps
        # the larger cap.
        self.cooks: dict[str, tuple[str, str, int]] = {}
        self.bakes: list[tuple[str, materials.MaterialPlan, int]] = []
        for ue, plan in self.plans.items():
            cap = CAP_BODY if is_skin_material(self.mat_row[ue].repo_path) else CAP_CLOTHING
            for t in plan.textures:
                prev = self.cooks.get(t.repo_path)
                self.cooks[t.repo_path] = (t.ue_texture, t.kind, max(cap, prev[2] if prev else 0))
            if plan.bake:
                self.bakes.append((ue, plan, cap))

    def source_file(self, ue_texture: str) -> Path:
        entry = self.manifest.get(ue_texture)
        if not entry or not entry.get("file"):
            raise FileNotFoundError(f"no exported file for {ue_texture}")
        return common.export_abs(entry["file"])

    # ---------------------------------------------------------------------------------------------- meshes ---
    def plan_meshes(self) -> None:
        self.old_remaps, self.slot_fallback = old_slot_table()
        mesh_rows = [r for r in self.rows if r.role == "mesh" and r.action in WRITE and self.selected(r)]
        names = metas.fbx_material_names([common.export_abs(r.source_file) for r in mesh_rows])
        names = {Path(k).resolve(): v for k, v in names.items()}
        self.remaps: dict[str, dict[str, str]] = {}
        self.not_importable: list[str] = []
        kept: list[common.MapRow] = []
        for r in mesh_rows:
            unresolved: list[str] = []
            entry = self.manifest.get(r.ue_asset, {})
            slots = entry.get("material_slots") or []
            fbx_names = names.get(common.export_abs(r.source_file).resolve())
            pairs, _unused, problems = metas.match_slots(slots, fbx_names)
            for p in problems:
                self.problems.append(f"{r.repo_path}: {p}")
            remap = {}
            fallback_mi = inferred_material(r)
            for i, (key, s) in enumerate(zip(metas.slot_keys(slots), slots)):   # the keys match_slots used
                slot = s.get("slot", "") or f"slot{i}"
                fbx_name = pairs.get(key)
                if not fbx_name:
                    continue                                    # slot without geometry: nothing to remap
                mi = (s.get("material") or fallback_mi).split(".", 1)[0]
                mat = self.mat_row.get(mi)
                target = mat.repo_path if mat else ""
                if not target:                                  # eye/teeth/brows: the engine's own materials
                    old = {slot_norm(k): v for k, v in self.old_remaps.get(r.repo_path, {}).items()}
                    # Pre-swap metas key the remap by slot name; metas this importer wrote key it by the FBX's
                    # material name, so a re-run finds the kept target under fbx_name.
                    target = (self.old_remaps.get(r.repo_path, {}).get(slot) or old.get(slot_norm(slot))
                              or self.old_remaps.get(r.repo_path, {}).get(fbx_name)
                              or self.slot_fallback.get(slot) or self.slot_fallback.get(slot_norm(slot), ""))
                if not target:
                    unresolved.append(f"slot {slot} (material {mi or 'none'})")
                    continue
                remap[fbx_name] = target
            if unresolved and "NO MATERIAL" in r.notes:
                # Its materials live in another Quantum pack (UE pointed the slot at an uninstalled package) and
                # the export has no textures for it: not importable from this bundle.
                self.not_importable.append(f"{r.repo_path}: {'; '.join(unresolved)}")
                continue
            for u in unresolved:
                self.problems.append(f"{r.repo_path}: {u} has no .mat")
            self.remaps[r.repo_path] = remap
            kept.append(r)
        self.mesh_rows = kept

    # ------------------------------------------------------------------------------------------- retirement ---
    def plan_retire(self) -> None:
        self.will_exist = set(self.remaps) | {self.mat_row[u].repo_path for u in self.plans} | set(self.cooks)
        self.will_exist |= {p.bake.repo_path for _, p, _ in self.bakes}
        self.keep = {r.repo_path for r in self.rows if r.action == "keep-old"}
        self.retired: list[str] = []
        self.dangling: list = []
        if self.only:
            return                                              # partial runs never retire
        index = refs.scan()
        rewrites = {path: set(remap.values()) for path, remap in self.remaps.items()}
        for ue, plan in self.plans.items():
            targets = {t.repo_path for t in plan.textures} | ({plan.albedo_path} if plan.albedo_path else set())
            rewrites[self.mat_row[ue].repo_path] = targets
        # Case-blind: on Windows a file tracked as T_Woman_Jersey_* IS the T_woman_jersey_* the import writes, and
        # deleting the "old" spelling deletes the new file.
        written_ci = {p.casefold() for p in self.will_exist}
        self.retired = [p for p in refs.retire_candidates(index, self.keep, self.will_exist, rewrites)
                        if p.startswith(QUANTUM + "/") and not p.endswith(".meta") and p.casefold() not in written_ci]
        # References read from files the import rewrites (a model's .meta, a .mat) are replaced, not left dangling.
        rewritten = {"project/" + p for p in rewrites} | {"project/" + p + ".meta" for p in rewrites}
        self.dangling = [d for d in refs.dangling(index, self.will_exist, self.retired)
                         if d.file.replace("\\", "/") not in rewritten]

    # ----------------------------------------------------------------------------------------------- write ---
    def write(self) -> None:
        guids: dict[str, str] = {}
        has_alpha: dict[str, bool] = {}
        n = Counter()
        for repo_path, (ue_tex, kind, cap) in sorted(self.cooks.items()):
            res = textures.cook_texture(self.source_file(ue_tex), common.repo_abs(repo_path), cap, kind, force=self.force)
            has_alpha[repo_path] = res.has_alpha
            guids[repo_path] = textures.write_texture_meta(common.repo_abs(repo_path), kind, cap)
            n["texture cooked" if not res.skipped else "texture up to date"] += 1
        for ue, plan, cap in self.bakes:
            dst = common.repo_abs(plan.bake.repo_path)
            res = materials.execute_bake(plan, self.source_file, dst, cap, force=self.force)
            has_alpha[plan.bake.repo_path] = bool(res and res.has_alpha)
            guids[plan.bake.repo_path] = textures.write_texture_meta(dst, "color", cap)
            n["texture baked"] += 1
        for ue, plan in self.plans.items():
            r = self.mat_row[ue]
            mat = materials.build_mat(plan, guids, has_alpha.get(plan.albedo_path, False))
            materials.write_mat(common.repo_abs(r.repo_path), mat, keep_guid=True)
            n["material " + r.action] += 1
        for r in self.mesh_rows:
            dst = common.repo_abs(r.repo_path)
            src = common.export_abs(r.source_file)
            if not dst.exists() or not filecmp.cmp(src, dst, shallow=False):
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                n["mesh " + r.action] += 1
            else:
                n["mesh unchanged"] += 1
            defaults = {"simplify": 0.4} if "_head" in dst.stem.lower() and "/Clothing/" not in r.repo_path else None
            # Every mesh here is an Unreal export: its axis turn sits on RootNode, so the bones must pose in the
            # frame the vertices were baked in (ModelImportSettings::RootNodeInModelSpace).
            metas.write_model_meta(r.repo_path, self.remaps.get(r.repo_path, {}), importer_defaults=defaults,
                                   importer_overrides={"rootNodeInModelSpace": True})
        for p in self.retired:
            for f in (common.repo_abs(p), common.meta_path(common.repo_abs(p))):
                if f.exists():
                    f.unlink()
            n["retired"] += 1
        if not self.only:
            payloads = sorted(p for p in self.will_exist if p.lower().endswith((".fbx", ".png")))
            diff = csvrows.swap_rows(common.PROJECT / "external_assets.csv",
                                     [p for p in self.retired if p.lower().endswith((".fbx", ".png"))], payloads)
            self.report["csv"] = diff.summary()
            csvrows.write_retired_used_list([p for p in self.retired if p.lower().endswith((".fbx", ".png"))])
        self.report["counts"]["written"] = dict(n)

    # ---------------------------------------------------------------------------------------------- report ---
    def run(self) -> int:
        self.validate()
        self.plan_materials()
        self.plan_meshes()
        self.plan_retire()
        c = self.report["counts"]
        c["map rows"] = dict(Counter(f"{r.role} {r.action}" for r in self.rows))
        c["meshes to write"] = len(self.mesh_rows)
        c["materials to write"] = len(self.plans)
        c["textures to cook"] = len(self.cooks)
        c["textures to bake"] = len(self.bakes)
        c["retire"] = len(self.retired)
        c["dangling refs"] = len(self.dangling)
        c["materials with warnings"] = len(self.report["warnings"])
        c["meshes not importable"] = len(self.not_importable)
        self.report["not_importable"] = self.not_importable
        self.report["retire"] = self.retired
        self.report["dangling"] = [f"{d.file}:{d.line} {d.kind} -> {d.target}" for d in self.dangling]
        self.report["remaps"] = self.remaps
        if self.apply:
            if self.problems:
                print(f"{len(self.problems)} problem(s) - nothing written. First ones:")
                for p in self.problems[:30]:
                    print("  ", p)
                self._save()
                return 1
            self.write()
        self._save()
        print(json.dumps(c, indent=2))
        print(f"problems: {len(self.problems)}" + ("" if not self.problems else " (see report)"))
        for p in self.problems[:15]:
            print("  ", p)
        print(f"report: {REPORT}")
        return 0 if not self.problems else 1

    def _save(self) -> None:
        common.write_json(REPORT, self.report)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--apply", action="store_true", help="write files (default: dry run)")
    ap.add_argument("--only", help="item_group glob, e.g. 'Male/Tops/*' (partial run: no retire, no CSV)")
    ap.add_argument("--force", action="store_true", help="re-cook textures even when up to date")
    a = ap.parse_args(argv)
    return Importer(a.apply, a.only, a.force).run()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
