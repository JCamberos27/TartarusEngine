"""project/external_assets.csv and Drive "Used" bookkeeping for the Quantum swap.

    python -B -m casual_bundle.csvrows exclusions                       which asset_manifest.py exclusions hit the pack
    python -B -m casual_bundle.csvrows used-list <retired.txt> [--out F] write the Used-side paths to trash (Phase 4)
    python -B -m casual_bundle.csvrows mirror <paths.txt> --used <Used>  EXPLICIT export step: copy payloads to Used

- swap_rows(): the install_revolver38.py pattern (tools/weapons/install_revolver38.py:123-129) for a whole pack: drop the
  retired rows, (re)hash the added/changed payloads from disk, keep the CSV in asset_manifest.py build's order
  (case-blind by path) and report the diff. Running asset_manifest.py build on a clean checkout must give the same file.
- Nothing here touches the Drive by default. Only `mirror` with an explicit --used writes there (like
  install_revolver38.py --used); retired Used files are only LISTED (tools/quantum/retired_used_paths.txt) for the
  owner-approved Phase 4 clean-up.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import common

CSV_PATH = common.PROJECT / "external_assets.csv"
RETIRED_USED_LIST = Path(__file__).resolve().parent.parent / "retired_used_paths.txt"   # tools/quantum/
HEADER = ["path", "size", "sha256"]
ASSET_TOOLS = common.REPO / "tools" / "assets"


@dataclass
class Row:
    path: str      # project-relative, forward slashes
    size: int
    sha256: str


@dataclass
class SwapDiff:
    removed: list[str] = field(default_factory=list)     # rows dropped (retired / replaced folders)
    added: list[str] = field(default_factory=list)       # new rows
    changed: list[str] = field(default_factory=list)     # rows whose size or sha256 changed (same path)
    unchanged: list[str] = field(default_factory=list)   # add_paths already listed with the same content
    rows_before: int = 0
    rows_after: int = 0
    bytes_before: int = 0
    bytes_after: int = 0
    written: bool = False

    def summary(self) -> str:
        delta = (self.bytes_after - self.bytes_before) / (1 << 20)
        return (f"external_assets.csv: {self.rows_before} -> {self.rows_after} rows "
                f"({len(self.removed)} removed, {len(self.added)} added, {len(self.changed)} changed, "
                f"{len(self.unchanged)} unchanged), {delta:+.1f} MB" + ("" if self.written else " [dry run]"))


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_rows(csv_path: Path = CSV_PATH) -> list[Row]:
    with open(csv_path, encoding="utf-8", newline="") as f:
        return [Row(r["path"], int(r["size"]), r["sha256"]) for r in csv.DictReader(f)]


def sort_key(path: str) -> str:
    """asset_manifest.py build sorts by the 'project/...' path, lower-cased."""
    return ("project/" + path).lower()


def write_rows(rows: list[Row], csv_path: Path = CSV_PATH) -> None:
    """Writes exactly as asset_manifest.py build does: header, LF endings, csv quoting, sorted case-blind."""
    with open(csv_path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(HEADER)
        for r in sorted(rows, key=lambda r: sort_key(r.path)):
            w.writerow([r.path, r.size, r.sha256])


def _matches(path: str, removals: list[str]) -> bool:
    """A removal entry ending in '/' is a folder prefix; anything else is one path. Case-blind, like the CSV."""
    p = path.casefold()
    for r in removals:
        r = r.replace("\\", "/").casefold()
        if (r.endswith("/") and p.startswith(r)) or p == r:
            return True
    return False


def swap_rows(csv_path: Path, remove_prefixes_or_paths, add_paths, project: Path | None = None,
              dry_run: bool = False, canonical=None) -> SwapDiff:
    """Removes the rows of retired payloads and (re)lists `add_paths` with their size and sha256 from disk.

    - remove_prefixes_or_paths: project-relative paths, or folder prefixes ending in '/'. A row both removed and
      added (a replaced payload) ends up listed with its new content and counts as changed, not removed + added.
    - add_paths: project-relative payload paths that must be on disk under `project` (default: the CSV's folder).
      FileNotFoundError lists every missing one before anything is written.
    - canonical(paths) -> paths: optional respelling (e.g. asset_manifest.canonical_case) applied to added paths.
    - dry_run: compute the diff, write nothing."""
    csv_path = Path(csv_path)
    project = Path(project) if project else csv_path.parent
    removals = [str(r) for r in remove_prefixes_or_paths]
    adds = [str(p).replace("\\", "/") for p in add_paths]
    if canonical:
        adds = list(canonical(adds))

    missing = [p for p in adds if not (project / p).is_file()]
    if missing:
        raise FileNotFoundError(f"{len(missing)} payload(s) to list are not on disk: " + ", ".join(missing[:20]))

    before = read_rows(csv_path)
    diff = SwapDiff(rows_before=len(before), bytes_before=sum(r.size for r in before))
    old = {r.path.casefold(): r for r in before}
    kept = {r.path.casefold(): r for r in before if not _matches(r.path, removals)}

    for p in dict.fromkeys(adds):                       # de-duplicated, order kept
        full = project / p
        row = Row(p, full.stat().st_size, sha256(full))
        prev = old.get(p.casefold())
        if prev is None:
            diff.added.append(p)
        elif (prev.size, prev.sha256) != (row.size, row.sha256) or prev.path != row.path:
            diff.changed.append(p)
        else:
            diff.unchanged.append(p)
        kept[p.casefold()] = row

    diff.removed = sorted((r.path for k, r in old.items() if k not in kept), key=sort_key)
    after = list(kept.values())
    diff.rows_after = len(after)
    diff.bytes_after = sum(r.size for r in after)
    if not dry_run:
        write_rows(after, csv_path)
        diff.written = True
    return diff


def used_paths(paths) -> list[str]:
    """Used-side relative paths (Used\\project\\<path>, Windows separators) for project-relative payload paths.
    .meta files never live on the Drive, so they are left out."""
    out = {"Used\\project\\" + str(p).replace("/", "\\") for p in paths if not str(p).endswith(".meta")}
    return sorted(out, key=str.lower)


def write_retired_used_list(paths, out: Path = RETIRED_USED_LIST) -> list[str]:
    """Writes the Used-side paths of retired payloads, one per line, for the owner-approved Phase 4 clean-up
    (move each to Drive trash). Never touches the Drive itself. Merges with the list already there: a re-run
    retires only what is newly unreferenced, and the earlier runs' files still have to go. Returns the lines written."""
    lines = used_paths(paths)
    if out.is_file():
        with open(out, encoding="utf-8") as f:
            lines = sorted(set(lines) | {s.rstrip("\n") for s in f if s.strip() and not s.startswith("#")})
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Quantum swap: Drive \"Used\" files to move to Drive trash in Phase 4 (relative to \"Tartarus Assets\").\n")
        f.write("# Written by tools/quantum/casual_bundle/csvrows.py; ask the Drive owner before deleting anything.\n")
        for line in lines:
            f.write(line + "\n")
    return lines


def mirror_to_used(paths, used: Path, project: Path = common.PROJECT) -> tuple[int, int]:
    """EXPLICIT export step (install_revolver38.py --used): copies each project-relative payload to
    <used>/project/<path> unless an identical copy is already there. Returns (copied, already current)."""
    copied = current = 0
    for p in paths:
        src = project / p
        dst = Path(used) / "project" / p
        if dst.is_file() and dst.stat().st_size == src.stat().st_size and sha256(dst) == sha256(src):
            current += 1
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1
    return copied, current


def manifest_exclusions(prefix: str = "project/" + common.QUANTUM) -> list[str]:
    """asset_manifest.py EXCLUDE_PREFIXES entries that would keep pack payloads out of the CSV (the
    Textures/Clothing/Hats/ one predates real headwear: build would silently drop the new hat textures)."""
    if str(ASSET_TOOLS) not in sys.path:
        sys.path.insert(0, str(ASSET_TOOLS))
    import asset_manifest  # noqa: E402  (tools/assets is not a package)
    p = prefix.casefold()
    return [x for x in asset_manifest.EXCLUDE_PREFIXES if x.casefold().startswith(p) or p.startswith(x.casefold())]


def _read_list(path: Path) -> list[str]:
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="csvrows", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("exclusions", help="asset_manifest.py exclusions that hit the pack")
    ul = sub.add_parser("used-list", help="write the Used paths of retired payloads")
    ul.add_argument("retired", type=Path, help="text file: one retired project-relative path per line")
    ul.add_argument("--out", type=Path, default=RETIRED_USED_LIST)
    mi = sub.add_parser("mirror", help="copy payloads to a Used folder (explicit Drive write)")
    mi.add_argument("paths", type=Path, help="text file: one project-relative payload path per line")
    mi.add_argument("--used", type=Path, required=True, help='e.g. "G:\\My Drive\\Tartarus Assets\\Used"')
    a = ap.parse_args(argv)
    if a.cmd == "exclusions":
        hits = manifest_exclusions()
        for x in hits:
            print("excluded by asset_manifest.py:", x)
        print(f"{len(hits)} exclusion(s) hit {common.QUANTUM}")
    elif a.cmd == "used-list":
        lines = write_retired_used_list(_read_list(a.retired), a.out)
        print(f"{a.out}: {len(lines)} Used paths")
    elif a.cmd == "mirror":
        copied, current = mirror_to_used(_read_list(a.paths), a.used)
        print(f"{a.used}: {copied} copied, {current} already current")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
