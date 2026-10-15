# Editor smoke suites

`TartarusEngine.exe --editor-tests [filter]` runs every test in `src/Tests/EditorUiTests.cpp`
and the suites in this folder, in the real editor (1920x1080 window, scratch user folder, an empty
in-memory scene). CI runs it under Mesa llvmpipe on every push (`.github/workflows/build.yml`)
and uploads `editor-test-shots/` (screenshots plus `report.md`) as an artifact.

Each `<Suite>.inl` is `#include`d by `EditorUiTests.cpp` inside its anonymous namespace, so it
uses the harness helpers directly, and defines `void Register<Suite>Tests()`. Name every test
`"<suite>: <what it checks>"` (lower-case suite name) so `--editor-tests "<suite>:"` runs just it.

## Harness in one screen

A test is a list of steps run one per frame, before ImGui reads input. A step returns true when
done; it fails the test when it times out (default 240 frames).

| Step | What it does |
|---|---|
| `FreshScene()` | Empty scene with Alpha (empty), Beta (Light + Audio Source), Gamma (Light); clears undo, selection, tabs |
| `Do(name, fn)` | Run code once |
| `Wait(n)`, `WaitSeconds(s)`, `Until(name, pred, timeout)` | Wait |
| `Expect(name, fn -> std::string)` / `ExpectTrue(name, fn -> bool)` | Check; a non-empty string / false fails |
| `Click(q, mods, button, clicks)`, `RightClick(q)`, `DoubleClick(q)` | Waits for the item, then clicks its centre |
| `Key(ImGuiKey, mods)`, `KeyDown`, `KeyUp`, `TypeText("...")`, `Wheel(dy)` | Keyboard / wheel |
| `Drag(from, AtItem(to))`, `MoveToItem(q)`, `HoverPanel("###Scene")`, `MouseButton(b, down)` | Pointer |
| `Shot("name", {"###Scene"})` | Screenshot to `editor-test-shots/<test>/<name>.png`; the listed windows must not be blank |
| `AllowLogError("fragment")` | This test expects editor log errors containing the fragment |

Queries: `ByLabel("Save As")` (visible label contains), `ByExact(raw)`, `ByTag("row:Alpha")`
(`EditorTestTag` names, e.g. Hierarchy rows `row:<name>`, Inspector fields
`field:<Component>/<Field>`), `MenuItem("Duplicate")` (an item in an open menu or popup),
`PopupItem(...)`, `Header("Light")` (an Inspector component header). `Query::Root` restricts to a
window (`"###Inspector"`, `"###Hierarchy"`, `"###Assets"`, `"###Scene"`, `"##Toolbar"` for the
menu bar). Menu labels carry an icon prefix (`ICON_FA_FLOPPY_DISK "  Save"`), so match with
`ByLabel` / `MenuItem` on the words.

Every frame of every test is also checked by the health monitor: a new error in the editor log, a
GL debug error, an ImGui recoverable error, conflicting widget ids, or a NaN transform fails the
test. Warnings are listed in the report.

## Reaching editor internals

`EditorLayer` befriends one accessor struct per suite, `EditorSmokeAccess_<Suite>`. It has to be
defined at global scope, so close the anonymous namespace around it:

```cpp
} // namespace
struct EditorSmokeAccess_Menus {
    static bool& ShowLighting(EditorLayer& e) { return e.m_ShowLighting; }
};
namespace {
```

The shared `X` (`EditorUiTestAccess`) accessors in `EditorUiTests.cpp` are usable too.

## What a smoke test should do

Drive the feature the way a user does (menu, button, shortcut), check the editor state changed as
it should, undo it and check it's restored, and take a `Shot` of anything visual. A failing test
is a finding: either the editor is wrong (a bug to fix) or the test is.
