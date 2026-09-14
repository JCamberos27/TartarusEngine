#!/usr/bin/env python3
"""Guard rail for #160 / Phase 1 item 7: "two — and only two — button treatments" across the
whole editor (EditorUIPrimitives::ActionButton and ::PrimaryButton; ::DangerIconButton is the one
sanctioned third-party-red variant of ActionButton). EditorUIPrimitives.h is the one place that's
allowed to define those treatments by pushing ImGuiCol_Button/ButtonHovered/ButtonActive style
colours directly — every other .cpp/.h in src/Editor should call the primitive instead of hand-
rolling its own copy of the same three PushStyleColor calls, which is exactly how #160 happened
the first time (three panel .cpp files each carrying their own near-identical ActionButton before
EditorUIPrimitives.h existed to share one implementation across the host/module DLL boundary).

This does NOT ban manual ImGuiCol_Button pushes outright — a handful of call sites are
legitimately not ActionButton-shaped (axis-coloured gizmo buttons, the window min/max/close
cluster's batched multi-button style, a colour-coded filter chip) and forcing them through a
primitive built for a different shape would be worse than the duplication. Those are
allow-listed by FILE with a reason, coarse-grained on purpose (this project's convention per
tools/check_component_registration.py): a new file introducing the pattern fails immediately; a
file already on the list can still be tightened to zero occurrences later without the lint
needing to change first.

Fails (exit 1) on: a manual push in a .cpp not on the allow-list; a stale allow-list entry for a
file that no longer has any manual push (the coarse grain means this can't detect a *partial*
migration within an already-listed file — that's a known limitation, not a bug).

Run locally:  python tools/check_button_styling.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EDITOR_DIR = ROOT / "src" / "Editor"
ALLOWLIST = ROOT / "tools" / "button_styling_allowlist.txt"
SANCTIONED_FILE = "EditorUIPrimitives.h"

PUSH_RE = re.compile(r"PushStyleColor\(\s*ImGuiCol_Button(Hovered|Active)?\s*,")


def parse_allowlist(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            sys.exit(f"allowlist: malformed line (no '='): {line!r}")
        name, reason = (s.strip() for s in line.split("=", 1))
        if not reason:
            sys.exit(f"allowlist: {name} has no reason")
        out[name] = reason
    return out


def main() -> int:
    if not EDITOR_DIR.is_dir():
        sys.exit(f"missing: {EDITOR_DIR}")
    if not ALLOWLIST.is_file():
        sys.exit(f"missing: {ALLOWLIST}")

    allowed = parse_allowlist(ALLOWLIST.read_text(encoding="utf-8"))

    violations: dict[str, list[int]] = {}
    for path in sorted(list(EDITOR_DIR.glob("*.cpp")) + list(EDITOR_DIR.glob("*.h"))):
        if path.name == SANCTIONED_FILE:
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        hits = [i + 1 for i, line in enumerate(lines) if PUSH_RE.search(line)]
        if hits:
            violations[path.name] = hits

    errors = []
    for name, hits in violations.items():
        if name not in allowed:
            where = ", ".join(f"L{n}" for n in hits)
            errors.append(
                f"{name}: {len(hits)} manual ImGuiCol_Button* push(es) ({where}), not on the "
                f"allow-list.\n    Either call EditorUIPrimitives::ActionButton/PrimaryButton/"
                f"DangerIconButton instead, or add '{name} = <reason>' to "
                f"{ALLOWLIST.relative_to(ROOT)} if this one genuinely isn't ActionButton-shaped."
            )

    for name in allowed:
        if name not in violations:
            errors.append(
                f"{name}: allow-listed but has no manual ImGuiCol_Button* push anymore. Remove "
                f"the stale entry from {ALLOWLIST.relative_to(ROOT)} (or, better, it means this "
                f"one finished migrating — nothing left to do)."
            )

    if errors:
        print("Button styling check FAILED:\n")
        for e in errors:
            print(f"  - {e}\n")
        return 1

    total = sum(len(h) for h in violations.values())
    print(f"Button styling check OK: {len(violations)} file(s) allow-listed, {total} total manual push site(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
