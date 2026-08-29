# Changelog

Running history of notable changes to Tartarus Engine. Newest first.
Dates are `YYYY-MM-DD`. Each entry links the commit(s) that landed it.

---

## Unreleased

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
