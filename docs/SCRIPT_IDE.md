# In-editor C# IDE

Open **Window > Script IDE**, double-click a `.cs` asset, or click **Edit in Script IDE** on an attached C# script. New gameplay scripts and editor tools open here automatically. **Open Externally** remains available in the Asset Browser context menu.

The IDE is a native dockable ImGui window. It has a searchable project source list, reorderable file tabs, a monospaced C# editor, line numbers, automatic indentation, text selection, clipboard editing, undo/redo, and a lower information area. The original source remains an ordinary `.cs` file; it can still be edited with Visual Studio or another application.

The IDE opens as a tab alongside **Scene** and **Game**. Drag its window tab to split or undock it; **Appearance > Dock alongside Scene / Game** returns it to that group. The Scene navigation gizmo belongs to Scene's draw layer and cannot appear or accept clicks through the IDE.

Drag the vertical divider immediately to the right of **Project Scripts** left/right to resize the source list. Drag the horizontal divider above **Scripts ready / Problems** down/up to resize the bottom information panel. The code expands into the space you free. Both divider positions are saved per project and remain usable when the IDE window or UI scale changes.

The code font is the bundled **JetBrains Mono**. **Appearance** offers twelve code palettes: Tartarus Dark, Dracula, Monokai, Nord, Gruvbox Dark, Solarized Dark, One Dark, Tokyo Night, Catppuccin Mocha, GitHub Light, Solarized Light, and High Contrast. Selection, caret, comments, numbers, syntax, background, and current-line colors follow the selected palette. The choice applies to every file tab and is saved with the project's IDE session.

Roslyn also colors resolved symbol declarations and uses: types (including `Vector3`), methods, properties, fields, parameters, namespaces, and enum members. These colors follow the selected theme. Keywords/comments/literals are colored immediately; semantic colors arrive with background analysis after a short pause. Editing clears outdated semantic spans until the matching source snapshot is analyzed. Unresolved symbols retain ordinary identifier coloring.

The separate **C# Tools** launcher is hidden by default. Open it through **Window > C# Tools**; its close button keeps it closed while individual editor tool windows continue to work.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| Ctrl+S | Save the active file and queue script compilation |
| Ctrl+Shift+S | Save all modified files |
| F6 | Save all and compile gameplay/editor assemblies |
| Ctrl+Z / Ctrl+Y | Undo/redo buffer edits while the code editor has focus |
| Ctrl+F | Find and optionally replace in the current document |
| Ctrl+Shift+F | Search text across project C# files, including open buffers |
| Ctrl+G | Go to a line |
| Ctrl+Space | Request semantic symbol/member completion |
| F12 | Go to source definition at the caret |
| Shift+F12 | Find bound references at the caret |
| F2 | Preview a symbol rename |
| Ctrl+Shift+I | Format the document |

Typing a dot also requests member completion. Choose a candidate in the popup; its tooltip lists signatures/overloads. The insertion is a normal undoable text edit. Symbol info displays the resolved signature, available overloads and engine API documentation. Holding the mouse over code requests a delayed semantic tooltip. Source definitions open another tab at the target line/column. Referenced assembly APIs expose signatures/documentation, but are not editable project source.

## Problems and compilation

Roslyn analyzes unsaved buffers after a brief debounce in a separate process. It sees all sources in the active assembly, open buffer overrides, normal implicit usings, and the staged engine API. Editor-folder files are analyzed separately from gameplay, matching the project compilation boundary. Analysis never executes project code.

**Problems** distinguishes live buffer diagnostics from the most recent compiler output. Click an error/warning to open its source location. Error markers also identify affected lines in the editor. **Build output** shows the gameplay/editor build logs. **References** lists semantic references; this is distinct from a text search, so unrelated identifiers with the same name are excluded.

Saving writes atomically through the existing file history and triggers the existing background compiler/reload pipeline. F6 saves every modified buffer before compiling. A failed build keeps the previous running assembly. Buffer edits are local text history; saving joins the editor's global asset history. Undoing a saved file through global history reloads a clean open buffer; if its buffer has newer unsaved work, the IDE keeps that work and displays a disk conflict instead of overwriting it.

## Rename and formatting

F2 asks for a new identifier, then previews the bound references in the active assembly. Apply changes the affected buffers; it does not silently save them. Review the tabs and Save All/F6 when ready. Each document's edit can be undone with its text history. A preview is rejected if its source changed before application. Invalid identifiers, keywords and referenced assembly symbols cannot be renamed. Rename does not rewrite serialized field/class keys in scene files; preserve those authoring identities or migrate their data deliberately. Overrides and interface implementations can be distinct symbols; review the preview rather than assuming a solution-wide refactoring engine.

Format uses Roslyn's syntax tree and preserves comments and strings. It refuses documents with syntax errors. It uses four-space indentation and becomes one undoable buffer edit. The save layer preserves the file's UTF-8 BOM and its LF/CRLF convention. UTF-8 source, Unicode positions and tab-expanded caret coordinates are translated at the compiler boundary. Binary/UTF-16 source and files larger than 8 MB are refused with an explanation.

## Unsaved work and external changes

Tabs show an unsaved marker. Closing a modified tab, reloading it, or quitting with modified scripts offers Save/Discard/Cancel. Closing the IDE panel only hides it; buffers remain available. The engine's exit prompt includes unsaved script buffers as well as the scene. A failed save or disk conflict prevents Save-and-Exit.

Clean tabs reload external changes. Modified tabs keep their buffer and offer **Reload disk version** or **Overwrite disk with buffer**. Neither path silently overwrites conflicting work. Open tabs, the active document and recovery buffers are stored under `project/Library/IDE/session.json`, outside authored asset history. Recovery restores unsaved buffers on the next launch, including a conflict indicator when disk content changed meanwhile.

## Editor-tool API

```csharp
using Tartarus.Editor;
EditorUtility.OpenScript("assets/Scripts/Bob.cs", line: 12, column: 1);
EditorUtility.OpenScript(); // Show the IDE without selecting a file.
```

Editor tools run on the main thread and open the native window through the existing service bridge. Source/class selection and gameplay lifecycle are otherwise unchanged.

## Implementation and dependencies

* Native window/document/session integration: `src/Editor/ScriptIDE.cpp`.
* Code editing: the MIT-licensed [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit), pinned to the commit recorded in `extern/ImGuiColorTextEdit/UPSTREAM.md`. Local patches make programmatic replacement undoable, preserve exact final newlines, expose hover/caret coordinates, and remove one unused variable for the engine's warning policy.
* C# analysis: `managed/Tartarus.CodeAnalysis`, using the Roslyn compiler assemblies shipped with the installed .NET SDK. [Microsoft's semantic analysis documentation](https://learn.microsoft.com/en-us/dotnet/csharp/roslyn-sdk/get-started/semantic-analysis) explains the compilation/symbol model used here.
* Build staging: CMake builds the helper into `Managed/IDE`; no NuGet feed, web editor, or separate language-server installation is required. The helper is excluded from game exports.
* JSON analysis requests/results live in `Library/IDE`, with one process per engine session request, latest-buffer/caret validation, a timeout, and explicit cleanup. The source-file list is scanned on a worker, not in the rendering loop.

This implementation provides code authoring, semantic navigation and build/reload integration. It does not implement debugger attachment, executable breakpoints, step-through execution, or a full Visual Studio refactoring/package-management suite. A debugger must run outside the engine process: pausing the same process that draws the IDE would also pause its UI. No breakpoint buttons are presented as if debugging were active.

## Focused verification

The Roslyn self-test checks valid gameplay/editor compilation context, engine member completion, prefix filtering, unsaved diagnostics, source definition positions, cross-file references and rename previews, invalid/metadata rename rejection, CRLF offsets, Unicode comments and formatting safety. Native unit checks cover completion/replacement undo, read-only behavior, UTF-8, tabs and final-newline preservation. `--editor-shot <directory> --editor-shot-ide --project <small fixture>` takes one focused IDE layout capture using the engine's normal renderer.
