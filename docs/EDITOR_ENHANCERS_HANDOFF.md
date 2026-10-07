# Editor Enhancers: handoff for the next session

Status as of 2026-10-07. Phases 0 to 2 are done, merged to `main` and tested by the user. Phase 3 (vInspector) was designed but **no Phase 3 code was written yet**. This file is the full context needed to continue. Read it before you touch anything.

The user-facing docs are in [EDITOR_ENHANCERS.md](EDITOR_ENHANCERS.md). This file covers the remaining work.

## 1. The goal

Bring every feature of kubacho lab's Unity "Editor Enhancers" bundle into TartarusEngine's editor:

- vHierarchy 2
- vFolders 2
- vInspector 2
- vTabs 2
- vFavorites 2
- vRuler

The user asked for thorough, high-quality work that stays performance-minded and uses the engine's existing systems instead of duplicating them.

Some of this already existed before the project, and the plan extends that code rather than replacing it:

- Measure tool
- Selection history (Ctrl+[ / Ctrl+])
- Asset favorites
- Active, visibility and pick-lock toggles
- Single-component copy/paste and presets
- Folder Back/Forward
- Shortcut registry with rebinding

**Decisions already agreed with the user:**

- All six tools are in scope, with one commit per phase.
- Ctrl+Shift+T becomes "reopen closed tab". Statistics already moved to Alt+Shift+T in Phase 0.
- vTabs is a tab strip inside the Inspector and Asset Browser panels.
- Keeping play-mode changes is opt-in per component.

**How the user works:**

- They do all visual testing themselves. Never drive the editor with computer-use.
- After each phase:
  1. Build.
  2. Run the unit tests and the smoke test.
  3. Commit.
  4. Launch the editor with `run-editor.cmd`.
  5. Give the user a short test checklist.
- If the editor is running and blocks a build, close it with `taskkill /IM TartarusEngine.exe /F`. No need to ask.
- Leave the untracked files alone: `docs/REVIEW_LIST.md` and `project/editor_folders.json`. The second one is the user's own folder-style test data.

## 2. Build, test and repo conventions

**Build** (from PowerShell; git-bash mangles `/m`):

```
cmake --build build --config Release --target TartarusEngine -- /m /v:minimal
```

- New `.cpp` files are picked up through GLOB_RECURSE. Re-run `cmake -S . -B build` after adding one, or you get link errors.
- The compiler is C++17 under MSVC `/W4 /WX`, so every warning is an error.

**Unit tests:** `build\Release\TartarusEngine.exe --unit-tests [filter]`. The last full run had about 41,043 checks and 0 failures.

**Smoke test:** `build\Release\TartarusEngine.exe --smoke-test tests\smoke-scenes`. 19/19 passed.

**Guard scripts:**

- `python tools/check_button_styling.py`: use the `EditorUIPrimitives::ActionButton` / `SecondaryButton` / `DangerIconButton` helpers, never a raw `ImGui::Button` in new code.
- `python tools/check_component_registration.py`: any new component must be registered or allowlisted.

**Side effect of test runs:** they rewrite line endings in `project/*.meta`. Run `git checkout -- project/` afterwards, but keep the untracked `project/editor_folders.json`.

**Line endings:** the repo stores LF and the working copy is CRLF. Normalize every file you edit to CRLF; mixed endings showed up after Python edits.

**Editing tips:**

- Don't write Python heredocs containing backslashes through bash; one put a literal NUL into `SceneSerializer.cpp`.
- For large inserts, write the snippet with the Write tool into the scratchpad, then apply it.

**Host/module split:**

- The editor UI partly lives in a hot-reloadable module DLL (`src/Editor/EditorModule*.cpp`) that talks to the host only through the POD struct `EditorModuleHostAPI` (`EditorModuleAPI.h`).
- Adding a callback takes three steps:
  1. Bump `kEditorModuleAPIVersion`. It is **40** now.
  2. Add a line to the version-history comment.
  3. Add a trampoline in `HotReloadEditorModule.cpp` (around line 580).
- Only POD types cross the boundary.
- The Inspector body (`DrawInspectorBody`) is host-side, so Phase 3 should need **no** API change.

**Undo:**

- Scene edits: call `PushUndo(world, "Label")` *before* mutating. For drags, use `StageUndo` and then `CommitStagedUndo`.
- Project and user files written through `AtomicFile` are journaled by the global file undo (`EditorFileHistory`). The global undo observer in `EditorLayer_Scene.cpp` whitelists the enhancer files and gives them labels ("Edit Bookmarks", "Edit Style Palette", "Edit Folder Style"); `ReloadHistoryFiles` reloads them after an undo.

**Persistent identity:**

- An entity is named by its scene key plus its `OrderComponent` value: `EditorLayer::CurrentSceneKey()` (the GUID, else the path) and `FindEntityByOrder`.
- Never store an `entt::entity` across an undo, Play/Stop or scene load. entt recycles ids.

**Settings:** a new `EditorSettings` field needs the field itself plus its Load and Save lines. The UI goes in `EditorLayer::DrawEnhancerPreferences()` (`EditorLayer_Enhancers.cpp`), which is Settings category 6, "Editor Enhancers".

**Shortcuts** (`src/Editor/Shortcuts.cpp`):

- Register with `Register("id", "Label", Ctx_*, K(ImGuiKey_X))`. `Sk` adds Shift; `CSk` adds Ctrl+Shift.
- The hover contexts `Ctx_HierarchyHover`, `Ctx_ProjectHover` and `Ctx_InspectorHover` already exist. They are granted in `EditorLayer.cpp` (about line 3225) to whichever panel the mouse is over, only when `EnhancerHoverKeys` is on and no item is active. `Ctx_Viewport` is dropped while a hover context is granted.
- `SameConflictScope` treats each hover context as overlapping its panel's focus context.

## 3. What is done (commits on `main`)

| Commit | Phase | Contents |
|---|---|---|
| 550a5a1e | 0, shared infra | `src/Editor/Enhancers/`: `EnhancerCore` (EditorRef, AddUnique/RemoveRef/FindRef/MoveRef, `NavHistory<T>`, GlobMatch, FuzzyScore, RemapFolderKeys/RemapFolderPath, FormatLength, FA icon table via `tools/gen_fa_icon_table.py` → `FAIconTable.inc`, TreeLineChildMask). `EnhancerUserState`: per-user `enhancers_<hash>.json` with SceneBookmarks, EntityBookmarks, FolderBookmarks, **InspectorBookmarks (already declared, unused so far)** and DefaultParents. `Palette` (per-user `editor_palette.json`). `StyleWidgets` (PaletteColorRow, IconPickerGrid). Hover shortcut contexts. Settings page. Tests in `src/Tests/UnitTests_Enhancers.cpp`. Docs. |
| 86e2c366 | 1, vHierarchy | `HierarchyStyleComponent` (icon, colour, flat/gradient fill, separator), serialized as "hierStyle". Row wash, tree lines, zebra, minimal mode, component minimap (click jumps to the Inspector; Alt+click opens a **pinned component window**), default parent (D), hover keys E / Shift+E / Ctrl+Shift+E / A / F / X / D, nav bar (scene selector + selection Back/Forward + entity bookmark chips) via API v39 `DrawHierarchyNavBar`. Extracted `DrawReflectedComponentFields` (Inspector.cpp ~3043). This also fixed an old `continue` that skipped removal after a managed C# inspector drew. Inspector scroll-to plus flash. |
| 501f20b0 | 2, vFolders | `Enhancers/FolderStyles` (project file `project/editor_folders.json`: per-folder style, rules, automatic icons from content), `AssetLibrary::OnFolderPathChanged` hook, API v40 (EditorFolderVisual, folder tree flags, folder context menu, hover folder, folder nav bar). Tree and grid show styled icons, a content minimap, tree lines, zebra, wash and minimal mode. Hover keys `project.hover.*`. |

Two vFolders features already existed before the project and were kept as they were: two-line names in the grid, and folder Back/Forward.

The user tested Phase 2 in the editor. No bugs were reported before handoff.

## 4. Remaining work

The original plan file is `C:\Users\Jacob\.claude\plans\https-assetstore-unity-com-packages-tool-swift-popcorn.md` (310 lines). It is local to the user's machine; everything needed from it is restated below.

### Phase 3: vInspector (split into 3a UI, then 3b attributes)

#### Code map (verified line numbers, `src/Editor/EditorLayer_Inspector.cpp`, 5132 lines)

**`DrawInspectorBody`, l.2009:**

- Pushes flat button colours. `InspectorEnd()` pops them.
- Prunes the selection, then swaps in the locked selection (`m_InspectorLocked`).
- Multi-select branch `HasGroupSelection()`, l.2084–2372:
  - Transform, Object and every reflected component through `BeginComponentSection(..., false, mrm)`.
  - Material.
  - Footer Duplicate/Delete buttons at about l.2360.
- No entity selected (l.2374): asset inspectors keyed on `m_SelectedAssetKey`, else the empty state.
- Single entity (l.2410 onward):
  - `PushID(0x0DE00000 + order)` at l.2429.
  - Header card at l.2442: active eye, kind icon, name field, padlock, and the object ⋯ menu `##ObjectActionsMenu` (Duplicate / Save as Prefab / Delete) at l.2513.
  - Prefab banner, Tag/Static, Layer.
  - Transform section at l.2665 (Reset/Copy/Paste via `CopyComponentToClip`).
  - Mesh Renderer at l.2718, Material at l.2894.
  - **Reflected component loop at l.2928–3020.** Each `rc` in `ComponentRegistry::All()` with `GenericInspector` that the entity has gets `BeginComponentSection(rc.Meta.Icon, rc.Meta.Name, true, reflRemoved, tooltip, &reset, &copy, &paste, prefabRevert/Apply, &savePreset, &applyPreset)`. Copy builds a variant snapshot into `m_ComponentClipApply`. The body is `DrawReflectedComponentFields`. Removal runs after the section.
  - Then `DrawAddComponentMenu`.

**Other functions in the same file:**

- `DrawReflectedComponentFields`, l.3043: VisibleIf, groups as TreeNodes, `DrawManagedInspector` for C# scripts, Top/Bottom extras.
- `DrawReflectedField`, l.1775: the widget switch, shared by single- and multi-select.
- `BeginComponentSection`, l.3154–3355. Declared at `EditorLayer.h` l.2110 with default args.
  - The open state is `ImGui::GetStateStorage()` int at `GetID(icon + "  " + label)`, inside the entity's PushID scope.
  - `m_ExpandAllComponents` forces it open.
  - It handles the Hierarchy-minimap scroll-to and flash: `m_InspectorScrollToComponent`, `m_InspectorFlashComponent`.
  - The header is an `InvisibleButton` sized `hw × (FrameHeight + Px(4))` and drawn by hand (chevron, icon, ellipsized name).
  - The ⋯ button is `ActionButton(ICON_FA_ELLIPSIS_VERTICAL)` and opens the same popup as right-click (`header.c_str()` id, no extra PushID).
  - The × (`DangerIconButton`) shows only while the bar is hovered.
  - Popup items: Reset, Copy, Paste Values, Save/Apply Preset, Remove, Revert/Apply to Prefab.
  - The body is `BeginChild(label, AutoResizeY | Borders)`.
- `EndComponentSection`, l.3357: EndChild, then pops.
- `PasteComponentFromClip`, l.3090.
- Presets, l.3100–3152: `PresetsForComponent`, `SaveComponentPreset`.

**Supporting APIs:**

- `SceneSerializer::ComponentToPresetJson(world, e, name)` and `ApplyComponentPresetJson(world, assets, e, json)` (`src/Game/SceneSerializer.cpp` ~2451):
  - Only for `GenericSerialize` components.
  - Apply **adds the component if missing**.
  - The caller owns undo.
  - Use these for the multi-component clipboard and for keeping play-mode changes.
- `ComponentRegistry::All()` returns a vector of `RegisteredComponent {Meta (ReflectComponent: Name, Icon, Tooltip, Fields, GenericSerialize, GenericInspector), Has, Get, GetConst, Add, Remove}`.
- `ReflectField` is in `src/Game/ComponentReflection.h`, l.42. Its fields: Name, Type, Address, Tooltip, Min/Max, Slider, Logarithmic, Format, EnumLabels/EnumCount, `VisibleIfField/Value/Not`, Group, EditorHidden, Key, LegacyNames, AssetPath.
- Many components have a Bool field named exactly `"Enabled"`, e.g. C# Script, TransformController, IKRig, AudioListener and a few others. Add an inline helper like `const ReflectField* FindEnabledField(const ReflectComponent&)` (a Bool field with `Name == "Enabled"`) instead of editing every registration. It serves the A hover key and an optional dimmed header.

**Selection history** (`EditorLayer_Hierarchy.cpp` l.271–344; header `EditorLayer.h` l.902–925):

- `m_SelHistory` is `std::vector<std::vector<entt::entity>>`, with `m_SelHistoryPos` and `m_SelSnapshotLast`.
- `RecordSelectionHistory(world)` runs once per frame (`EditorLayer.cpp` l.3657). It compares entity snapshots, pushes a selection-only undo entry unless `m_EditPushedThisFrame` is set, and caps the history at 64.
- `ApplySelectionSnapshot` sets `m_SelHistoryNavigating`.
- **Bug to fix:** entries are raw entt handles. They go stale after an undo, Play/Stop or scene load, and can select a recycled, unrelated entity in another scene. Nothing clears the history on scene load.
- `RestoreSelectionByOrder(world, orders)` (`EditorLayer_Scene.cpp` l.570) already selects by order and sets `m_SelHistoryNavigating`. `CaptureSelectedOrders(world, entities)` is at l.559.
- Callers: shortcuts `select.historyBack/Forward` (`EditorLayer.cpp` l.3336), the Edit menu (`EditorLayer_Toolbar.cpp` l.693), and the Hierarchy nav bar (`EditorLayer_Enhancers.cpp` l.471).

**Asset selection:**

- Held in `m_SelectedAssetKey` and `m_SelectedAssetIsFolder`. It is written in many places (`EditorLayer_AssetBrowser.cpp` 520, 539, 985, 1490, 1784, 2082…2334, among others).
- The Inspector shows an asset only when no entity is selected.
- `ClearSelection()` (`Hierarchy.cpp` l.182) does **not** clear the asset key.

**Play mode** (`EditorLayer_Scene.cpp`):

- `OnEnterPlayMode`, l.868: snapshots the scene and stores the selection orders.
- `OnExitPlayMode`, l.948: `LoadFromString(snapshot)`, re-selects by order, then `ApplyKeptAudioMix(world)` at l.1001.
- `ApplyKeptAudioMix` (`EditorLayer_AudioDebug.cpp` l.43) is the pattern to copy: after the reload, `PushUndo("Keep …")`, then write.

**Hover-key model to copy:** `HandleHierarchyHoverKeys` (`EditorLayer_Hierarchy.cpp` ~l.2143) with `Shortcuts::Triggered("hierarchy.hover.*")`.

**Pins:**

- `OpenPinnedComponent(world, entity, componentName)` and `DrawPinnedComponentWindows` are in `EditorLayer_Enhancers.cpp` l.597–662.
- Calling `OpenPinnedComponent` again on an open pin re-places it under the cursor (`Placed = false`). Calling it every frame during an Alt-drag therefore makes the window follow the mouse, which is the intended "drag out" feel.

**Nav-bar building blocks:** in `EditorLayer_Enhancers.cpp` there is `NavChip` (anonymous namespace, l.370). `DrawHierarchyNavBar` (l.402) is the template to follow for the Inspector bar, including the overflow "+N" popup and the drop target through `BeginDragDropTargetCustom`. Drag payload types:

- `HIERARCHY_ENTITY` (an `entt::entity`)
- `ASSET_FOLDER_PATH`
- `ASSET_MODEL_PATH`

Grep the others with `SetDragDropPayload(` in `EditorLayer_AssetBrowser.cpp`.

#### Phase 3a design (agreed, not yet implemented)

**1. Selection history by identity.** Replace the element type with:

```cpp
struct SelectionHistoryEntry {
    std::string Scene;        // CurrentSceneKey() when recorded
    std::vector<int> Orders;  // OrderComponent values, primary first
    std::string Asset;        // asset key, only when no object was selected
    bool operator==(const SelectionHistoryEntry&) const;
};
```

- Keep the cheap per-frame entity-vector comparison (`m_SelSnapshotLast`). Add `m_SelAssetLast`, which holds `assetNow = (no entities && !m_SelectedAssetIsFolder) ? m_SelectedAssetKey : ""`.
- Push an undo entry only when the **entity** vector changed. An asset-only change records history without an undo entry.
- Convert entities to an entry with `CaptureSelectedOrders`.
- Back/Forward skip entries whose `Scene != CurrentSceneKey()` when `Orders` is non-empty. They may also skip entries whose orders resolve to nothing.
- Applying an entry:
  - Orders: `RestoreSelectionByOrder`.
  - Asset: `ClearSelection()`, then `m_SelectedAssetKey = key`, `IsFolder = false`, and `m_SelHistoryNavigating = true`.
- Update `CanSelectionHistoryBack/Forward` to match. They're called every frame, so keep them cheap; the history holds at most 64 entries.
- Test: the history survives `SceneSerializer::LoadFromString`. There is no EditorLayer test harness in `src/Tests`, so put the entry resolution or step logic in a pure helper, for example in EnhancerCore, and test that.

**2. Inspector nav bar.** Drawn at the top of `DrawInspectorBody`, host side, after the lock swap. Gate it on a new `EditorSettings::InspectorNavBar` (default true).

- Contents: `[<] [>] [bookmark toggle] [chips… +N]`.
- Back/Forward use the new history.
- The bookmark toggles the current object (`EditorRef::MakeEntity(sceneKey, order, name)`) or the current asset (`MakeAsset(key, filename)`) in `EnhancerUserState::InspectorBookmarks`, then `MarkDirty()`.
- Chips show this scene's entity bookmarks plus every asset bookmark. An entity chip uses its HierarchyStyle icon; an asset chip uses a file icon.
- Click selects. For an asset: `ClearSelection()` and set the asset key. Right-click removes the chip.
- Drop target: accepts `HIERARCHY_ENTITY` and asset payloads.

**3. Section context.** Add a trailing parameter `entt::entity entity = entt::null` to `BeginComponentSection`. It is non-null only on the single-select path; pass `entity` at l.2665, 2718, 2894 and 2941. When it is set and `label` names a ComponentRegistry component, the section gains the vInspector features below.

**4. Floating windows:**

- Add "Open in Window" (`ICON_FA_UP_RIGHT_FROM_SQUARE`) to the ⋯ popup. It calls `OpenPinnedComponent(world, entity, label)`. It needs `World&`: use `*m_WorldPtr` if one exists, or add a member set at the top of `DrawInspectorBody`.
- Alt + left-drag on the header past `Px(6)` (`IsItemActive() && io.KeyAlt && IsMouseDragging(0, Px(6))`) calls `OpenPinnedComponent` every frame of the drag.
- Releasing off the header does not toggle it, because the InvisibleButton only fires when the mouse is released over it.

**5. Multi-component clipboard:**

- State:
  - `std::set<std::string> m_InspectorPickedComponents`
  - `int m_InspectorPickedOrder` (clear the picks when the inspected order changes)
  - `std::vector<std::string> m_ComponentClipboard`, holding preset JSON
- Ctrl+click on a header toggles its pick instead of opening or closing it. Picked headers get an accent fill or outline.
- In the ⋯ popup:
  - "Copy N Selected Components" when picks exist.
  - The existing "Copy Component" also sets `m_ComponentClipboard = {json}`.
- In the object ⋯ menu (`##ObjectActionsMenu`, l.2518):
  - "Copy All Components"
  - "Paste Components as New (N)": adds only the components the target lacks; skips and logs the rest.
  - "Paste Component Values (N)": only onto components the target already has.
- Each paste is one `PushUndo` for every selected entity.
- Add the same two paste items to a popup button in the multi-select footer (l.2360).
- Skip components that aren't `GenericSerialize`. Transform keeps its own Copy/Paste.

**6. Keep play-mode changes:**

- `struct PlayKeep { int Order; std::string Component; }; std::vector<PlayKeep> m_PlayKeep;`. Clear it in `OnEnterPlayMode`.
- During Play:
  - The ⋯ popup gets a "Keep Changes After Play" toggle (registry components plus "Transform").
  - The object ⋯ menu gets "Keep All Changes After Play".
  - Kept headers show a small `ICON_FA_THUMBTACK`.
- In `OnExitPlayMode`, **before** `LoadFromString`: for each keep, find the entity by order in the play world and capture `ComponentToPresetJson`, or a copy of the `TransformComponent`.
- After the reload and selection restore, next to `ApplyKeptAudioMix`: if anything was captured, `PushUndo(world, "Keep Play Mode Changes")`, then apply by order. Log a summary and clear the list.
- Test: capture, LoadFromString, apply against snapshot JSON, in a pure function where possible.

**7. Hover keys.** Register these in `Shortcuts.cpp` next to the hierarchy ones:

| Id | Key | Action |
|---|---|---|
| `inspector.hover.collapseAll` | Ctrl+Shift+E | Collapse all sections; if all are collapsed, expand all. |
| `inspector.hover.isolate` | Shift+E | This section open, the others closed. |
| `inspector.hover.toggleEnabled` | A | Flip the `Enabled` field, with undo. |
| `inspector.hover.remove` | X | Remove the component (removable sections only). |

- Track the hovered section in Begin/EndComponentSection: the header rect, and the child rect from `GetItemRectMin/Max` after `EndChild`. Store `m_HoverSectionLabel`, header id and removable flag for next frame's handler.
- Apply collapse, expand and isolate as a command consumed next frame inside `BeginComponentSection`, which sets each header's storage int as it is drawn. Section open state is per ImGui ID, so it can't be set from outside.
- Add the key names to the Settings help text in `DrawEnhancerPreferences` and to the docs.

**8. Polish:**

- Animations: a new `EditorSettings::InspectorAnimations`, default on.
- Collapse/expand: a 120 ms height ease.
  - Store each section's measured full body height in storage (`headerId ^ const`) whenever the section is fully open and not animating.
  - While animating, `BeginChild` uses a fixed height `full * ease(t)` with NoScrollbar.
  - While closing, it keeps returning true until `t = 1`.
  - Skip the animation when the full height is unknown.
- Remove: a 150 ms fade (`PushStyleVar(Alpha)`) and shrink, then report `removedOut = true`.
  - Route the ×, the menu Remove and the X key through `m_RemovingSection = headerId; m_RemovingStart = time`.
  - With animations off, the duration is 0.
- Minimal mode (`EditorSettings::InspectorMinimal`): the ⋯ button appears only on header hover, the same as ×.

#### Phase 3b: attributes

**C++ (`ComponentReflection.h`).** Append new members to `ReflectField` *at the end* so existing aggregate initializers keep compiling:

- `ReadOnly`
- `EnableIfField` / `EnableIfValue` / `EnableIfNot`
- `Condition` (fn ptr `bool(*)(const void* comp)`) and `ConditionDisables`
- `Tab` (const char*)
- `OnChanged` (`void(*)(World&, entt::entity, void* comp)`)
- `Variants` / `VariantLabels` / `VariantCount` (quick-pick chips)
- `NonSerialized`: display-only. The serializer loops at `SceneSerializer.cpp` ~601 and ~748 must skip it.

`ReflectComponent` gains:

- `Buttons`: a vector of `{Label, Icon, Color, Invoke(World&, entity, void*), optional Arg}`.
- `Statics`

Add `src/Game/ReflectAttributes.h`, a fluent builder: `Field(...).Tab("Motion").ReadOnly().OnChanged(&Fn)`.

Reset to default:

- A per-type `static const T kDefault{}` exposed via `RegisteredComponent::DefaultInstance()`.
- A "Reset to Default" item in the field's right-click menu.

`DrawReflectedField` (l.1775) gains tab bars, `BeginDisabled`, variant chips and buttons. Write a pure `EvaluateFieldState(rc, field, comp)` (visible / enabled / readOnly) and unit-test it.

**C#** (`managed/Tartarus.Runtime/ScriptFields.cs`): add these attributes:

- Button
- Foldout
- Tab
- ShowInInspector
- OnValueChanged
- ReadOnly
- Variants
- HideIf
- DisableIf
- `Dictionary<string, scalar>` editing

- `Describe` emits the new keys. The Inspector reader is at about l.3517–3566 of `EditorLayer_Inspector.cpp`; search for `Scripting::Describe`.
- **Cache the Describe JSON per class.** It is currently parsed every frame for every script. Invalidate the cache on assembly reload.
- `Entry.cs` gets op 13, `InvokeEditorMethod`, and op 14, `Statics`, through the existing JSON `NativeRequest`, so the ABI is unchanged. In edit mode the flow is: make a temporary instance, `Apply`, invoke, `Save`, then write back with undo.
- Move the C# Script/Source rows under an "Advanced" foldout once a class is set.

**Tests:**

- Field-state evaluation.
- Reset to default for each type.
- Paste-as-new vs paste-values.
- Keep-play against snapshot JSON.
- Parsing a canned Describe JSON containing every new key.
- History entries surviving LoadFromString.

Update `docs/EDITOR_ENHANCERS.md` with a vInspector section.

### Phase 4: vTabs

A tab strip inside the Inspector and the Asset Browser panels.

- Create tabs by drag-and-drop: folders, assets, entities and components.
- Shift+scroll switches tabs. Ctrl+Shift+scroll moves a tab. Scrolling is smooth.
- A "+" menu with fuzzy search (`Enhancers::FuzzyScore`) and starred tabs.
- Ctrl+T new, Ctrl+W close, **Ctrl+Shift+T reopen closed tab** (that key was freed in Phase 0).
- Tab icons come from the vHierarchy and vFolders styles.
- Tabs persist in `EnhancerUserState` as EditorRefs.

### Phase 5: vFavorites

- Hold Alt over the Assets panel to show the overlay; it can be locked open.
- Several renamable pages with an animated switch. Navigate with scroll, 1–9 or the arrow keys.
- Bookmarks for folders, assets and entities, shown with their style icons. Shortcuts are customizable.
- **Migrate the existing asset favorites into page 1.** Find them with grep `Favorite` in `EditorLayer_AssetBrowser.cpp` and `EditorSettings`.

### Phase 6: vRuler

Hold Shift+R in the viewport:

- Move: surface-to-surface distance.
- Click: the object's bounds size.
- Scroll: pick a reference object under the cursor.
- Units: metric or feet/inches, using `Enhancers::FormatLength`, which the Measure tool already uses.

Reuse the Measure tool's raycast and drawing code.

### Performance rules (all phases)

- Hierarchy and folder tree cost stays proportional to the visible rows (clipper).
- No per-row heap allocation: use reused buffers and `char` scratch.
- Folder summaries are rebuilt only when their signature changes, or at most once a second.
- User-state writes are debounced: `MarkDirty()` then a once-per-frame `Flush()`.
- The C# Describe result is cached.

### Per-phase checklist

1. Build clean.
2. Unit tests pass, including the new ones.
3. Smoke test passes.
4. Both guard scripts pass.
5. Docs updated.
6. Files normalized to CRLF.
7. `git checkout -- project/` (keep the untracked `editor_folders.json`).
8. Commit.
9. Launch with `run-editor.cmd`.
10. Give the user a short checklist.

Hot-reload the editor module whenever the API version changes. Hover keys must not trigger viewport tools, and nothing may fire while typing in a text field.
