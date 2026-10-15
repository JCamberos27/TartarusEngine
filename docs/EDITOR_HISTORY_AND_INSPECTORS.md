# Editor history and Inspectors

Ctrl+Z and Ctrl+Y use one chronological editor history; Ctrl+Shift+Z also redoes. Scene edits, component changes, selection, committed curve edits, asset writes, imports, asset duplication/rename/deletion, script creation, project settings and saved editor preferences share this history. A continuous drag produces one entry. Unchanged writes do not create entries or discard redo. The history holds the latest 100 actions in this editor session.

File history records the original bytes before the first write in an action, including binary files and GUID sidecars. Undoing creation removes the exact file; undoing deletion restores it. Rename restores both original and destination paths. Redo restores the final bytes. File restoration rolls back if a write fails. Imported/model-derived caches and compiler outputs are regenerated rather than journaled.

History caches the last committed authored scene. Camera movement, scrolling, docking, search and ordinary selection do not serialize it. Scene controls request a transaction through PushUndo/StageUndo; commits refresh the cache, and scene replacement, history restoration and external asset changes refresh its context. File-only actions reuse the history anchor. The profiler's **Undo Scene Snapshot** scope identifies initialization and scene-edit commit costs. See [PERFORMANCE.md](PERFORMANCE.md) for the interaction regression check.

Text fields retain their normal character undo while typing; committed authored values join global history. Gameplay owns its input while the game viewport has captured the mouse. Release capture to use editor shortcuts. Asset undo remains available during Play, and its history survives Stop. Scene undo requires stopping Play because restoring the registry beneath live physics actors is unsafe. Runtime simulation is not an authored edit. Loading another scene or entering/exiting prefab editing creates a new scene history context. External IDE/Explorer changes and operating-system actions are outside this history.

## Recoil editing

Select a `.recoil` asset to tune its scalar settings and view read-only X/Y/Z curve thumbnails. Double-click a thumbnail to open a dedicated dockable curve window. Each window names its asset, group and axis. Drag keys/handles, change numeric values, or use presets there. Edits save on commit and use global undo. Multiple axes can be open together. A changed file is reloaded by the other windows. Existing asset formats and interpolation are preserved.

## Gameplay Inspectors

First Person Body, First Person Controller, Weapon Definition and the other gameplay components
draw from their reflected fields in `src/Game/ComponentRegistry.cpp`. Field groups become the
Inspector's sections, and every field keeps its asset picker, bounds, tooltip and conditional
visibility. Scenes saved while these were C# scripts convert to the native components on read.
See [Gameplay code](GAMEPLAY_CODE.md) for adding a component.

Right-clicking or left-clicking the game viewport during Play engages game input identically.
