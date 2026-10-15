# Script IDE

Open **Window > Script IDE**, double-click a shader or `.tescript` asset, or use **Open in Script IDE**
from the Asset Browser context menu. **Open Externally** remains available there too.

The IDE is a native dockable ImGui window for the engine's C++ sources, project shaders
(`.glsl`, `.vert`, `.frag`, `.comp`, `.geom`, `.shader`) and `.tescript` files. It has a searchable
source list, reorderable file tabs, a monospaced editor with syntax colouring for C++ and GLSL,
line numbers, automatic indentation, clipboard editing, undo/redo and a lower information area.
Files stay ordinary text files and can still be edited in Visual Studio or anything else.

The source list shows the engine's `src/` folder (only when the editor runs from a source build,
see below) followed by the project's own files. Project files are editable everywhere except
`Library`. Engine sources are editable only from a source build.

The IDE opens as a tab alongside **Scene** and **Game**. Drag its tab to split or undock it;
**Appearance > Dock alongside Scene / Game** returns it to that group. The source list and the
bottom panel have draggable dividers whose positions are saved per project.

The code font is the bundled **JetBrains Mono**. **Appearance** offers twelve palettes: Tartarus
Dark, Dracula, Monokai, Nord, Gruvbox Dark, Solarized Dark, One Dark, Tokyo Night, Catppuccin
Mocha, GitHub Light, Solarized Light and High Contrast. The choice is saved with the project's IDE
session.

## Keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| Ctrl+S | Save the active file |
| Ctrl+Shift+S | Save all modified files |
| F6 | Save all and rebuild the game and editor modules |
| Ctrl+Z / Ctrl+Y | Undo/redo buffer edits while the editor has focus |
| Ctrl+F | Find and optionally replace in the current document |
| Ctrl+Shift+F | Search text across the listed sources, including open buffers |
| Ctrl+G | Go to a line |
| Ctrl+Space | Complete the word at the caret from identifiers in open files |

## Rebuilding and hot reload

Gameplay and editor tools live in two reloadable modules, `TartarusGame` and `TartarusEditor`
(see [Gameplay code](GAMEPLAY_CODE.md)). Saving one of their sources, or pressing F6, runs
`cmake --build` for both targets in the background. The editor keeps running while it builds;
when the new DLLs land the engine loads them in place. A failed build keeps the modules that
are already running.

**Problems** lists the compiler's errors and warnings from the last build; click one to open its
file at that line. Error markers also show in the editor. **Build output** shows the full log
(`<build>/TartarusModules.build.log`). **Rebuild Game & Editor Modules** in the toolbar does the
same as F6 without the IDE.

Rebuilding needs the CMake build tree the editor was built from. A packaged game or an editor
copied away from its build has no compiler, so the engine sources are hidden and the rebuild
commands are disabled there; shaders and project files still edit and save normally. Changes to engine code
outside the two modules need a normal rebuild and restart.

## Unsaved work and external changes

Tabs show an unsaved marker. Closing a modified tab, reloading it, or quitting with modified files
offers Save/Discard/Cancel. Closing the IDE panel only hides it; buffers remain available. The
engine's exit prompt includes unsaved IDE buffers as well as the scene.

Clean tabs reload external changes. Modified tabs keep their buffer and offer **Reload disk
version** or **Overwrite disk with buffer**. Open tabs, the active document and recovery buffers
are stored under `project/Library/IDE/session.json`. Recovery restores unsaved buffers on the
next launch. Saves preserve the file's UTF-8 BOM and LF/CRLF convention; binary, UTF-16 and
files larger than 8 MB are refused with an explanation.

## Implementation and dependencies

* Window, documents and session: `src/Editor/ScriptIDE.cpp`.
* Background module build: `src/Editor/GameModuleBuild.cpp`.
* Code editing: the MIT-licensed [ImGuiColorTextEdit](https://github.com/BalazsJako/ImGuiColorTextEdit),
  pinned to the commit in `extern/ImGuiColorTextEdit/UPSTREAM.md`. Local patches make
  programmatic replacement undoable, preserve exact final newlines, expose hover/caret
  coordinates and remove one unused variable for the engine's warning policy.

There is no debugger or semantic C++ analysis; use Visual Studio for those. A debugger must run
outside the engine process, since pausing the process that draws the IDE also pauses its UI.

`--editor-shot <directory> --editor-shot-ide --project <small fixture>` takes one IDE capture
using the engine's normal renderer.
