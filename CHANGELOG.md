# Changelog

Running history of notable changes to Tartarus Engine. Newest first.
Dates are `YYYY-MM-DD`. Each entry links the commit(s) that landed it.

---

## Unreleased

### Fixed — bug-tier sweep (issues #1–#10)
- New Scene no longer silently overwrites `scene.json` — it's now an untitled scene
  that must be Saved As (`e5234e5`, #1).
- Removing then re-adding a Mesh Renderer restores the original mesh instead of a
  default cube (`b04b320`, #2).
- Undo/redo/Play-Stop no longer reorders the Hierarchy — entities carry a stable
  `OrderComponent` serialized as `order` (`f2aafde`, #3).
- Play mode: persistent "Esc to release" hint + auto-release on focus loss (`e309e81`, #4).
- No camera whip when the cursor lock toggles (`039f781`, #5).
- View presets / nav-gizmo frame the whole scene when nothing is selected (`6291405`, #6).
- `primitive://` meshes no longer appear as Asset Browser entries (`6f7a02a`, #7).
- Duplicate offsets copies by (1,0,1), matching Paste (`03be6ce`, #8).
- Import-Settings model preview clamps zoom and has a Reset view button (`65018b3`, #9).
- Clicking an asset clears the scene selection so its Import Settings show (`4426832`, #10).

### Added
- **GL/GLFW diagnostics.** `glfwSetErrorCallback` now routes GLFW errors into the engine
  Log (previously unset, so every GLFW error was silent). Optional OpenGL debug-output
  plumbing (`GLDebug`) — inert unless a Debug build or `TARTARUS_GL_DEBUG=1`.
  <br>`301f865`
- **Crash-recovery auto-save.** The auto-save timer now writes a `<stem>.recovery.json`
  sidecar instead of overwriting the scene file. The real scene file changes only on an
  explicit Save / Save As / Ctrl+S (each of which, plus New Scene / Open / clean exit,
  deletes the stale snapshot). On launch, a recovery file newer than the scene file raises
  a Restore / Discard prompt.
  <br>`58641e5`
- **Editor QA sweep** documented in [`docs/BUGS_AND_POLISH.md`](docs/BUGS_AND_POLISH.md)
  — 3 data-loss issues, 8 bugs, 21 polish items, plus verified-working and
  manual-verification lists.
  <br>`72ed9fc`

### Fixed
- `Input::Update()` was calling `glfwGetKey()` for key codes `0..511` every frame; only
  `GLFW_KEY_SPACE..GLFW_KEY_LAST` are valid, so ~180 calls per frame were raising
  `GLFW_INVALID_VALUE`. Now polls just the valid range.
  <br>`301f865`
