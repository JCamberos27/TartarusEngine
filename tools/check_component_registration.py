#!/usr/bin/env python3
"""Guard rail for #302 / #184: every ECS component must be a conscious editor decision.

Every struct in src/Game/Components.h whose name ends in "Component" or "Tag", and every such struct defined in
another src/ header that the code uses as an ECS type (view<>/get<>/emplace<>/...: FxHudSettingsComponent in
src/Renderer/WeaponFxSettings.h), must be either:
  * registered with ComponentRegistry (a `Register<Name>` call or a `TARTARUS_REFLECT_FIELD(Name, ...)`
    line in src/Game/ComponentRegistry*.cpp), which auto-generates its serialization + Inspector
    section + Add-Component entry, OR
  * listed in tools/component_registration_allowlist.txt with a reason.

Fails (exit 1) on: a component in neither; a stale allow-list entry that is now registered; an
allow-list entry for a component that no longer exists. Commented-out registrations don't count. This is the structural enforcement of the
"ship components fully wired, or not visible" rule at the top of src/Game/ComponentRegistry.cpp.

Run locally:  python tools/check_component_registration.py
"""
import re
import sys
from pathlib import Path

from guard_common import parse_allowlist, strip_cpp_comments

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS_H = ROOT / "src" / "Game" / "Components.h"
REGISTRY_CPP = ROOT / "src" / "Game" / "ComponentRegistry.cpp"
ALLOWLIST = ROOT / "tools" / "component_registration_allowlist.txt"

SRC = ROOT / "src"

STRUCT_RE = re.compile(r"^struct\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.MULTILINE)
# A definition (not a forward declaration) of a struct named like a component, in any header.
STRUCT_DEF_RE = re.compile(r"^struct\s+([A-Za-z_][A-Za-z0-9_]*(?:Component|Tag))\b[^;\n]*\{", re.MULTILINE)
# The entt calls that only take component types.
ECS_USE_RE = re.compile(
    r"\b(?:view|emplace|emplace_or_replace|get|try_get|all_of|any_of|none_of|remove|erase|patch|replace|storage|"
    r"on_construct|on_destroy|on_update)\s*<\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*(?:Component|Tag))\b")


def component_structs(text: str) -> list[str]:
    return [n for n in STRUCT_RE.findall(text) if n.endswith(("Component", "Tag"))]


def source_files() -> list[Path]:
    return sorted(p for ext in ("*.h", "*.cpp", "*.inl") for p in SRC.rglob(ext))


def outside_components(sources: dict[Path, str]) -> list[str]:
    """Component-named structs defined outside Components.h that the code uses as ECS types."""
    used = set()
    for text in sources.values():
        used |= set(ECS_USE_RE.findall(text))
    out = set()
    for path, text in sources.items():
        if path.suffix == ".h" and path != COMPONENTS_H:
            out |= {n for n in STRUCT_DEF_RE.findall(text) if n in used}
    return sorted(out)


def registered_names(text: str) -> set[str]:
    names = set(re.findall(r"Register<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>", text))
    names |= set(re.findall(r"TARTARUS_REFLECT_FIELD\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", text))
    return names


def main() -> int:
    for p in (COMPONENTS_H, REGISTRY_CPP, ALLOWLIST):
        if not p.is_file():
            sys.exit(f"missing: {p}")

    sources = {p: strip_cpp_comments(p.read_text(encoding="utf-8", errors="replace")) for p in source_files()}
    structs = component_structs(sources[COMPONENTS_H])
    structs += [n for n in outside_components(sources) if n not in structs]
    # the registry and its per-area registration files (ComponentRegistryGameplay.cpp, ...)
    registered = set()
    for cpp in sorted(REGISTRY_CPP.parent.glob("ComponentRegistry*.cpp")):
        registered |= registered_names(sources[cpp])
    allowed = parse_allowlist(ALLOWLIST.read_text(encoding="utf-8"))

    errors = []

    for name in structs:
        in_reg = name in registered
        in_allow = name in allowed
        if not in_reg and not in_allow:
            errors.append(
                f"{name}: not registered with ComponentRegistry and not in the allow-list.\n"
                f"    Either register it in {REGISTRY_CPP.parent.relative_to(ROOT)}/ComponentRegistry*.cpp (gets serialization +\n"
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
                f"{name}: in the allow-list but no such component (a struct in "
                f"{COMPONENTS_H.relative_to(ROOT)}, or one used as an ECS type elsewhere). Remove the stale entry."
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
