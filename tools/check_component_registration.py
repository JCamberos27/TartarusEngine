#!/usr/bin/env python3
"""Guard rail for #302 / #184: every ECS component must be a conscious editor decision.

Every struct in src/Game/Components.h whose name ends in "Component" or "Tag" must be either:
  * registered with ComponentRegistry (a `Register<Name>` call or a `TARTARUS_REFLECT_FIELD(Name, ...)`
    line in src/Game/ComponentRegistry.cpp), which auto-generates its serialization + Inspector
    section + Add-Component entry, OR
  * listed in tools/component_registration_allowlist.txt with a reason.

Fails (exit 1) on: a component in neither; a stale allow-list entry that is now registered; an
allow-list entry for a struct that no longer exists. This is the structural enforcement of
docs/CONVENTIONS.md's "ship components fully wired, or not visible".

Run locally:  python tools/check_component_registration.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS_H = ROOT / "src" / "Game" / "Components.h"
REGISTRY_CPP = ROOT / "src" / "Game" / "ComponentRegistry.cpp"
ALLOWLIST = ROOT / "tools" / "component_registration_allowlist.txt"

STRUCT_RE = re.compile(r"^struct\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.MULTILINE)


def component_structs(text: str) -> list[str]:
    return [n for n in STRUCT_RE.findall(text) if n.endswith(("Component", "Tag"))]


def registered_names(text: str) -> set[str]:
    names = set(re.findall(r"Register<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>", text))
    names |= set(re.findall(r"TARTARUS_REFLECT_FIELD\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", text))
    return names


def parse_allowlist(text: str) -> dict[str, str]:
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):  # whole-line comments only; '#' is legal in reasons
            continue
        if "=" not in line:
            sys.exit(f"allowlist: malformed line (no '='): {line!r}")
        name, reason = (s.strip() for s in line.split("=", 1))
        if not reason:
            sys.exit(f"allowlist: {name} has no reason")
        out[name] = reason
    return out


def main() -> int:
    for p in (COMPONENTS_H, REGISTRY_CPP, ALLOWLIST):
        if not p.is_file():
            sys.exit(f"missing: {p}")

    structs = component_structs(COMPONENTS_H.read_text(encoding="utf-8"))
    registered = registered_names(REGISTRY_CPP.read_text(encoding="utf-8"))
    allowed = parse_allowlist(ALLOWLIST.read_text(encoding="utf-8"))

    errors = []

    for name in structs:
        in_reg = name in registered
        in_allow = name in allowed
        if not in_reg and not in_allow:
            errors.append(
                f"{name}: not registered with ComponentRegistry and not in the allow-list.\n"
                f"    Either register it in {REGISTRY_CPP.relative_to(ROOT)} (gets serialization +\n"
                f"    Inspector + Add Component for free), or add it to\n"
                f"    {ALLOWLIST.relative_to(ROOT)} with a reason."
            )
        elif in_reg and in_allow:
            errors.append(
                f"{name}: registered AND in the allow-list. Remove it from "
                f"{ALLOWLIST.relative_to(ROOT)}."
            )

    struct_set = set(structs)
    for name in allowed:
        if name not in struct_set:
            errors.append(
                f"{name}: in the allow-list but no such struct in "
                f"{COMPONENTS_H.relative_to(ROOT)}. Remove the stale entry."
            )

    if errors:
        print("Component registration check FAILED:\n")
        for e in errors:
            print(f"  - {e}\n")
        return 1

    reg = sorted(n for n in structs if n in registered)
    allow = sorted(n for n in structs if n in allowed)
    print(f"Component registration check OK: {len(reg)} registered, {len(allow)} allow-listed.")
    print(f"  registered:  {', '.join(reg)}")
    print(f"  allow-listed: {', '.join(allow)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
