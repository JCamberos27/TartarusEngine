#!/usr/bin/env python3
"""Guard rail for #333 PR 1: every importable asset under project/ must have a committed
.meta sidecar with a well-formed 16-hex GUID, and no two .meta files may share a GUID.

Run locally:  python tools/check_asset_meta.py
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT_DIR = ROOT / "project"

# Extensions that must have a .meta sidecar.  Keep in sync with AssetDatabase.cpp.
KNOWN_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".tga", ".bmp",
    ".fbx", ".obj", ".gltf", ".glb",
    ".wav", ".mp3", ".ogg", ".flac",
    ".prefab",
}

GUID_RE = re.compile(r"^[0-9a-f]{16}$")


def check_meta(meta_path: Path) -> str | None:
    """Return an error string if the .meta file is malformed, else None."""
    try:
        data = json.loads(meta_path.read_text(encoding="utf-8"))
    except Exception as e:
        return f"{meta_path}: JSON parse error: {e}"

    if "metaVersion" not in data:
        return f"{meta_path}: missing 'metaVersion'"
    if "guid" not in data or not isinstance(data["guid"], str):
        return f"{meta_path}: missing or non-string 'guid'"
    if not GUID_RE.match(data["guid"]):
        return f"{meta_path}: malformed guid '{data['guid']}' (expected 16 lowercase hex chars)"
    if "type" not in data or not isinstance(data["type"], str):
        return f"{meta_path}: missing or non-string 'type'"
    return None


def main() -> int:
    if not PROJECT_DIR.is_dir():
        sys.exit(f"missing project directory: {PROJECT_DIR}")

    errors: list[str] = []
    guid_to_path: dict[str, Path] = {}
    missing_meta: list[Path] = []

    for asset in PROJECT_DIR.rglob("*"):
        if not asset.is_file():
            continue
        if asset.suffix.lower() not in KNOWN_EXTENSIONS:
            continue
        meta = Path(str(asset) + ".meta")
        if not meta.exists():
            missing_meta.append(asset)
            continue
        err = check_meta(meta)
        if err:
            errors.append(err)
            continue
        guid = json.loads(meta.read_text(encoding="utf-8"))["guid"]
        if guid in guid_to_path:
            errors.append(
                f"duplicate GUID {guid!r}: {guid_to_path[guid]} and {meta}"
            )
        else:
            guid_to_path[guid] = meta

    if missing_meta:
        for a in missing_meta:
            print(f"MISSING .meta: {a.relative_to(ROOT)}", file=sys.stderr)
        errors.append(f"{len(missing_meta)} asset(s) have no .meta sidecar")

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"\ncheck_asset_meta: {len(errors)} error(s) found.", file=sys.stderr)
        return 1

    print(f"check_asset_meta: OK — {len(guid_to_path)} asset(s) checked, all have valid .meta files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
