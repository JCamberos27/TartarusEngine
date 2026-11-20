#!/usr/bin/env python3
"""Guard rail for #302 / #184: every ECS component must be a conscious editor decision.

Every struct in the component headers (src/Game/Components*.h: Components.h is an include-only umbrella over
ComponentsCore.h, ComponentsRender.h, ...) whose name ends in "Component" or "Tag", and every such struct defined in
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
Self-test:    python tools/check_component_registration.py --self-test   (a scratch tree: a split header's
              unregistered component fails, the same one registered or allow-listed passes)
"""
import re
import sys
import tempfile
from pathlib import Path

from guard_common import parse_allowlist, strip_cpp_comments

ROOT = Path(__file__).resolve().parent.parent

STRUCT_RE = re.compile(r"^struct\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.MULTILINE)
# A definition (not a forward declaration) of a struct named like a component, in any header.
STRUCT_DEF_RE = re.compile(r"^struct\s+([A-Za-z_][A-Za-z0-9_]*(?:Component|Tag))\b[^;\n]*\{", re.MULTILINE)
# The entt calls that only take component types.
ECS_USE_RE = re.compile(
    r"\b(?:view|emplace|emplace_or_replace|get|try_get|all_of|any_of|none_of|remove|erase|patch|replace|storage|"
    r"on_construct|on_destroy|on_update)\s*<\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*(?:Component|Tag))\b")


def component_structs(text: str) -> list[str]:
    return [n for n in STRUCT_RE.findall(text) if n.endswith(("Component", "Tag"))]


def source_files(src: Path) -> list[Path]:
    return sorted(p for ext in ("*.h", "*.cpp", "*.inl") for p in src.rglob(ext))


def outside_components(sources: dict[Path, str], component_headers: set[Path]) -> list[str]:
    """Component-named structs defined outside the component headers that the code uses as ECS types."""
    used = set()
    for text in sources.values():
        used |= set(ECS_USE_RE.findall(text))
    out = set()
    for path, text in sources.items():
        if path.suffix == ".h" and path not in component_headers:
            out |= {n for n in STRUCT_DEF_RE.findall(text) if n in used}
    return sorted(out)


def registered_names(text: str) -> set[str]:
    names = set(re.findall(r"Register<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>", text))
    names |= set(re.findall(r"TARTARUS_REFLECT_FIELD\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", text))
    return names


def check(root: Path, quiet: bool = False) -> int:
    game = root / "src" / "Game"
    allowlist = root / "tools" / "component_registration_allowlist.txt"
    headers = sorted(game.glob("Components*.h"))  # Components.h (the umbrella) and its per-domain parts
    registry = sorted(game.glob("ComponentRegistry*.cpp"))  # the registry and its per-area registration files
    if not headers:
        sys.exit(f"missing: {game / 'Components*.h'}")
    if not registry:
        sys.exit(f"missing: {game / 'ComponentRegistry*.cpp'}")
    if not allowlist.is_file():
        sys.exit(f"missing: {allowlist}")
    headers_label = f"{game.relative_to(root).as_posix()}/Components*.h"
    registry_label = f"{game.relative_to(root).as_posix()}/ComponentRegistry*.cpp"
    allow_label = allowlist.relative_to(root).as_posix()

    sources = {p: strip_cpp_comments(p.read_text(encoding="utf-8", errors="replace")) for p in source_files(root / "src")}
    structs = []
    for h in headers:
        structs += [n for n in component_structs(sources[h]) if n not in structs]
    structs += [n for n in outside_components(sources, set(headers)) if n not in structs]
    registered = set()
    for cpp in registry:
        registered |= registered_names(sources[cpp])
    allowed = parse_allowlist(allowlist.read_text(encoding="utf-8"))

    errors = []

    for name in structs:
        in_reg = name in registered
        in_allow = name in allowed
        if not in_reg and not in_allow:
            errors.append(
                f"{name}: not registered with ComponentRegistry and not in the allow-list.\n"
                f"    Either register it in {registry_label} (gets serialization +\n"
                f"    Inspector + Add Component for free), or add it to\n"
                f"    {allow_label} with a reason."
            )
        elif in_reg and in_allow:
            errors.append(f"{name}: registered AND in the allow-list. Remove it from {allow_label}.")

    struct_set = set(structs)
    for name in allowed:
        if name not in struct_set:
            errors.append(
                f"{name}: in the allow-list but no such component (a struct in "
                f"{headers_label}, or one used as an ECS type elsewhere). Remove the stale entry."
            )

    if errors:
        if not quiet:
            print("Component registration check FAILED:\n")
            for e in errors:
                print(f"  - {e}\n")
        return 1

    if not quiet:
        reg = sorted(n for n in structs if n in registered)
        allow = sorted(n for n in structs if n in allowed)
        print(f"Component registration check OK: {len(reg)} registered, {len(allow)} allow-listed "
              f"({len(headers)} component headers).")
        print(f"  registered:  {', '.join(reg)}")
        print(f"  allow-listed: {', '.join(allow)}")
    return 0


def self_test() -> int:
    """A scratch tree shaped like the real one: the umbrella includes a split header holding a component nothing
    uses through get<>/view<> yet (what the old Components.h-only scan let through)."""
    def tree(base: Path, registry: str, allow: str) -> Path:
        game = base / "src" / "Game"
        game.mkdir(parents=True)
        (base / "tools").mkdir()
        (game / "Components.h").write_text('#pragma once\n#include "ComponentsCore.h"\n#include "ComponentsRender.h"\n')
        (game / "ComponentsCore.h").write_text("#pragma once\nstruct NameComponent { int Id = 0; };\n")
        (game / "ComponentsRender.h").write_text("#pragma once\nstruct GlowComponent { float Strength = 1.0f; };\n")
        (game / "ComponentRegistry.cpp").write_text("void RegisterAll() {\n" + registry + "}\n")
        (base / "tools" / "component_registration_allowlist.txt").write_text("NameComponent = identity\n" + allow)
        return base

    cases = [
        ("a split header's unregistered component", "", "", 1),
        ("the same component registered", "    Register<GlowComponent>(m);\n", "", 0),
        ("the same component allow-listed", "", "GlowComponent = test\n", 0),
        ("a registration only in a comment", "    // Register<GlowComponent>(m);\n", "", 1),
        ("an allow-list entry for a component that is gone", "    Register<GlowComponent>(m);\n", "GoneTag = test\n", 1),
    ]
    failed = 0
    for i, (what, registry, allow, want) in enumerate(cases):
        with tempfile.TemporaryDirectory() as tmp:
            got = check(tree(Path(tmp) / f"case{i}", registry, allow), quiet=True)
        ok = got == want
        failed += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {what}: exit {got} (want {want})")
    print("Component registration self-test " + ("OK" if not failed else f"FAILED ({failed})"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv[1:] else check(ROOT))
