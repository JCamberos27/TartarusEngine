# Editor UI

The editor looks like an old white-phosphor terminal. It uses:
- pitch-black surfaces;
- phosphor-white text and neutral greys;
- the phosphor at full drive as the one accent, so a selection or the primary button reads as inverse video;
- muted status, kind and axis colours;
- letter-spaced monospace capitals for headings.

Preferences > General > CRT screen draws the whole editor through the launch screen's tube shader
(`src/Renderer/CrtScreen.*`, `CrtScreen.frag.glsl`): scanlines, phosphor glow, a vignette, and
optional curved glass. With the glass on, the mouse is bent through the same curve before ImGui
reads it, so clicks land on what's drawn.

Everything a panel draws with comes from three headers. Don't type colour literals or one-off button styles into panel code.

## Files

| File | What it holds |
|------|---------------|
| `src/Editor/EditorTheme.h` | The tokens: surfaces, lines, text, accent, status colours, axis colours, per-kind tints, and the HUD plate. Font roles (`PushBody`, `PushSmall`, `PushMono`, `PushHeading`, `PushTitle`), `Px()` for UI-scaled lengths, tracked text, and the Inspector's shared label column (`PropertyLabelWidth`). |
| `src/Editor/EditorUIPrimitives.h` | The widgets (see below). |
| `src/Editor/EditorPanels.h` | Every dockable panel's window name (`"<icon>  <title>###<id>"`). Also `MigrateIni`, which renames layouts saved under the old titles. |

`ApplyThemeStyle` / `ApplyTartarusPalette` (`EditorLayer.cpp`) map the tokens onto `ImGuiStyle`. It also checks the important pairs against WCAG contrast floors at startup. A miss logs a `[Theme]` warning.

## Fonts

| Font | Use | Notes |
|------|-----|-------|
| Inter | UI text | |
| JetBrains Mono | Numbers, the Console, the status bar, headings | Headings use letter-spaced capitals |
| Font Awesome 6 Solid | Icons | Merged into both fonts |

- Both fonts are named in the atlas (`TE-UI`, `TE-Mono`), so the hot-reload module DLL finds them too.
- ImGui's dynamic atlas renders any size on demand, so each role is just a size.

## Widgets

| Widget | Use |
|--------|-----|
| `BeginPanelToolbar` / `EndPanelToolbar` | Panel header strip: Raised background, hairline bottom border, full width. Wraps a toolbar row; items inside lay out horizontally via SameLine. Used for Hierarchy/Asset Browser/Console panel headers. |
| `ActionButton` | Flat icon button: toolbar tools, panel toggles. `active` = on (the accent). |
| `SecondaryButton` | Raised text button: Cancel, Import..., Add Component. |
| `PrimaryButton` | Inverse video: the one confirming action of a dialog. |
| `DangerIconButton` | Flat, red on hover: delete / remove. |
| `Segmented` | Mutually exclusive options as one strip. |
| `SectionHeader` | Tracked capitals with a hairline. Replaces `SeparatorText`; a leading icon glyph becomes an accent icon. |
| `Foldout` | A collapsible section (replaces `CollapsingHeader`). |
| `SearchField` | Magnifier, hint and clear button, the same everywhere. |
| `Chip`, `EmptyState`, `DrawRowState` | Tags; what an empty panel shows; list selection (accent wash + accent bar). |

Host code reaches these through the forwarders in `EditorLayerInternal.h`, which route tooltips through the ShowTooltips preference.

## Button styling enforcement

`tools/check_button_styling.py` enforces the two-button-treatment rule (#160): every .cpp/.h in `src/Editor` must use `EditorUIPrimitives` button primitives instead of hand-rolling `PushStyleColor(ImGuiCol_Button*)` or raw `ImGui::Button()`/`SmallButton()` calls. The script flags both manual colour pushes and raw button calls. Exceptions are allow-listed by file with a reason when the use genuinely doesn't fit a primitive (axis-coloured gizmo buttons, HUD-styled controls, breadcrumb text links, etc.). Run locally: `python tools/check_button_styling.py`.

## Layout

- **Menu bar:** the monogram, then File / Edit / Create / View / Window / Help. The open scene's name is centred, with an accent dot while unsaved and a PLAYING tag in Play.
- **Status bar:** across the bottom of the window (`EditorLayer::DrawStatusBar`).
  - Left: the latest log line. Click it to open the Console.
  - Right: tool, selection, scene, frame readout and build.

## Checking it without driving the desktop

`--editor-shot <dir>` opens the editor at 1920x1080 and saves a PNG of the whole window for each view: `overview`, `inspector` (every component open), `console`, `game`, `settings`, `project-settings`, `lighting`, `animator`. Then it exits.

- It works on a copy of your prefs and layout in `%LOCALAPPDATA%\TartarusEngine-Shot`. The real folder is never written.
- Options:

  | Option | Effect |
  |--------|--------|
  | `--editor-shot-default-layout` | Start from the default dock layout. |
  | `--editor-shot-scale <s>` | Set the UI scale. |
  | `--editor-shot-select <name>` | Choose the selected entity. |
  | `--perf-res WxH` | Set the window size. |
