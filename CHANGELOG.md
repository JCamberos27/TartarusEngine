# Changelog

Running history of notable changes to Tartarus Engine. Newest first.
Dates are `YYYY-MM-DD`. Each entry links the commit(s) that landed it.

---

## Unreleased

### Lighting + HDR overhaul (branch `lighting-overhaul`)

_One uncommitted batch. Establishes the linear-HDR pipeline and the first real GL 4.6
feature use. Behind a draft PR into `main` for review._

**Rendering pipeline**

- The scene now renders into a **multisampled linear `RGBA16F` + `DEPTH_COMPONENT32F`
  target** (`HdrTarget`), resolves once, and a **single fullscreen tonemap pass**
  (`Tonemapper`) applies exposure → curve → gamma. The Reinhard+gamma that used to be
  baked into the model fragment shader is gone from the real scene path (kept only for the
  offscreen model-preview thumbnails, which have no tonemap pass — `uApplyTonemap`).
- Tone-mapping operators: **Reinhard / ACES (Narkowicz) / AgX**, switchable live.
- **Exposure (EV)** control, photographic stops, applied before the curve.
- **MSAA** for the HDR target is configurable again (Off/2×/4×/8×) — closes the regression
  where the offscreen-pass migration dropped MSAA (**#101**).
- New Preferences ▸ Performance section: *Rendering (HDR)* + *Shadows (Directional Sun)*.
  All six keys persist in `editor_prefs.json`.

**Lights**

- Every light (directional / point / spot) now uploads through one **`std430` SSBO**
  (`LightBuffer`, binding 0) instead of per-frame `snprintf`-built `uPointLightPos[i]`
  uniform arrays. This is the layout a clustered-forward cull pass will consume later
  (addresses the SSBO half of **#104**; the per-vertex `inverse(uModel)` half remains).
- **Directional light is now a real entity**, not a hard-coded `main.cpp` constant. Aim it
  with the entity's rotation (shines along −Z, same convention as a spot). A migration
  synthesises one on load of any scene that has none, so old scenes keep a sun.
  `LightComponent` gained `Type::Directional` and `AngularSizeDegrees` (sun disc size →
  soft-shadow penumbra width).

**Shadows**

- **Cascaded shadow maps** for the directional sun (`CascadedShadowMap`): 4 cascades,
  `DEPTH_COMPONENT32F` 2D-array, practical (log/uniform blend) split scheme, texel-snapped
  ortho frusta, hardware depth-compare (`sampler2DArrayShadow`).
- Fragment side: **16-tap Poisson-disk PCF**, rotated per-fragment (banding → noise),
  kernel radius driven by the sun's `AngularSizeDegrees` (`uShadowSoftness`), plus a smooth
  cross-fade across the cascade seam.
- Synthesised sun tuned so shadows actually read: ~36° raking elevation (was near-overhead),
  intensity 6.0, angular size 2.0°.

**GL loader**

- `extern/glloader` extended from a ~GL 3.3 set to the **DSA + buffer-storage + SSBO +
  MSAA-texture + `glGetStringi`** subset (`glCreateBuffers`, `glNamedBufferStorage`,
  `glCreate/TextureStorage*`, `glCreateFramebuffers`, `glNamedFramebuffer*`,
  `glBlitNamedFramebuffer`, `glBindBufferBase`, …). `GLLoader_Init()` now fails if any are
  missing, so a sub-4.6 driver stops at "Failed to load OpenGL functions" instead of a
  cryptic shader error (partial **#105**). This is the bulk of **#96** fix-option (b);
  `Texture`, `ModelMesh`, `Framebuffer` are not yet ported.
- `Shader::SetVec4` added.

**Testing done**

- Built Release, launched, driven via computer-use. No GL/console errors. Verified: HDR
  render in Scene + Game + Play; exposure slider; all three tonemap operators; MSAA toggle;
  shadow on/off, resolution, distance; 7-light SSBO scene renders correct colours/falloff;
  soft Poisson shadows on floor / box→pedestal / cones with no acne or peter-panning;
  ~3–5 FPS cost for the shadow pass; settings round-trip through `editor_prefs.json`.

**Known gaps / follow-ups** — filed as issues, see the PR.

- Shadow pass has no per-cascade frustum culling (draws the whole scene ×4).
- Shadow pass ignores materials — alpha-tested geometry casts a solid silhouette.
- `uShadowNormalBias` is a single world-space value, not cascade-aware.
- Cascade count hardcoded to 4 at the call site; no UI.
- Point / spot lights still cast no shadows.
- No clustered/tiled light culling yet — every fragment loops every light.
- `HdrTarget::ResolveTo` resolves colour only; a future depth-consuming post pass (SSAO)
  will need the MS depth texture or a depth resolve.

### Second polish sweep — remaining open issues

- **#29 (P18)** — Asset Browser now shows a rendered thumbnail for each model instead of a
  generic cube glyph. Each `Model` is drawn once into its own small GL texture and cached
  (a few per frame, so opening a big folder doesn't stall); the cache is dropped on
  reimport / scene change / undo. `glCopyTexSubImage2D` added to the GL loader for the copy.
- **#38 (B12)** — entity rename is sanitised on commit: C0 control chars + DEL stripped, length
  capped at 64, ends trimmed, UTF-8 multibyte kept intact. No printf-family call was using a
  name as a format string, so the `%n` concern was already moot.
- **#30 (P19)** — the spinning corner wordmark is bigger, and a **Window ▸ Engine Mark** toggle
  hides it for anyone who reads it as an unreadable glyph rather than branding.
- **#40 (P23)** — verified by hand: three separate Position edits are three History entries,
  and one Ctrl+Z reverts only the last. The earlier "reverts all four at once" report does
  not reproduce (covered by the no-op-edit dedup from the first sweep).
- **#35 (B9)** — restore-from-maximised ghost frame does not reproduce against the current
  build (tried both maximise→restore and restore→maximise; the window repaints cleanly).
- Not changed: **#46 (P29)** near-black sky (scene data, author's call); **#33 (MV)** still
  needs hand-testing for real key events / Play-mode movement.

### Polish + feature sweep (issues #11–#50)

_One uncommitted batch; issue IDs in parentheses._

**Data safety**
- Non-finite input (`nan` / `inf` / `-inf`) typed into an Inspector Transform field is now
  rejected with a Console warning instead of poisoning the transform and, on save, writing
  tokens that make `scene.json` fail to reload. A `FiniteOr()` scrub at the one point every
  vector passes through on save *and* load is the belt-and-braces guarantee (D4, #34).

**Bugs**
- Wireframe mode: the selected object's own wireframe is visible again — the selection
  highlight no longer paints a filled orange silhouette over it. The full-surface wash is
  skipped in Wireframe/Unlit, and lighter (α 0.22 → 0.10) in Shaded so imported meshes
  keep their shading (P2 #13, P24 #41).
- Restore-from-maximized ghost frame, camera drift across Play→Stop, and the "History count
  grows on undo-then-jump" report could not be reproduced against the current build — the
  first is covered by the `OrderComponent` snapshot fix, the others were automation timing
  (B9 #35 needs a hands-on check; B11 #37, B8 #11, P12 #23 → cannot-reproduce).
- Entity rename still accepts long / unsanitised strings — *not addressed this pass* (B12 #38).

**Polish**
- Gizmo translate-drag no longer leaks float noise into Rotation — only the channel the
  gizmo actually drives is written back, and sub-`1e-4` euler dust is zeroed (P1 #12).
- Environment / Material colour widgets already consistent; `-0.000` collapsed to `0.000`
  on commit (P3 #14, P13 #24).
- Import / Open / Save dialogs open in the project folder, then follow the last-used
  location, instead of `build/Release/` (P4 #15).
- Primitive Inspector says "the default primitive material", not "imported from the source
  file" (P5 #16).
- History panel opens clear of the Stats overlay instead of stacked on it (P6 #17).
- History jump no longer mis-restores selection to a random unnamed entity; the gizmo's
  undo step is now labelled "Move" / "Rotate" / "Scale" like the Inspector's, not
  "Transform"; a rejected or no-op edit records nothing (P7 #18, P8 #19, P23 #40).
- Undo no longer collapses an expanded Inspector component section — the section's open
  state is keyed to the entity's name / order, which survives the snapshot reload (P9 #20).
- Unnamed Hierarchy rows get a positional fallback ("Box 3", "Object 7"); a mesh that also
  carries a light shows both glyphs; a filtered Hierarchy header reads "matches / total"
  (P10 #21, P16 #27, P15 #26).
- Nav-gizmo corner label shows Front / Back / Left / Right / Top / Bottom when the camera
  is axis-aligned (P14 #25).
- Asset right-click menu has a Reimport item for models and textures (P17 #28).
- Origin axis lines are desaturated, dimmer, and fade out approaching the world origin so
  they stop cluttering the transform gizmo (P25 #42).
- Default Gizmo Size 0.10 → 0.15 (P26 #43).
- Extreme-magnitude Transform values (≥ 1e6) show in `%g` scientific form with the exact
  value in the hover tooltip, instead of overflowing the field (P27 #44).
- File-menu items show their shortcuts (Ctrl+N / O / S / Shift+S), and those shortcuts are
  now actually wired up (P28 #45).
- The File-menu auto-save blurb no longer implies crash-recovery protection when auto-save
  is switched off (P30 #47).
- Play / Stop tooltips already present; verified (P32 #49).
- The title's unsaved `*` clears when Undo returns the scene to the last-saved state
  (P22 #39).
- More Console logging: object add / delete / duplicate, non-finite input rejects (P20 #31).
- Left as noted: group-delete confirmation is intentionally omitted (scene deletes are
  instant + undoable) (P21 #32); the spinning corner mark needs an art asset (P19 #30);
  the colour-picker clip could not be reproduced at a normal window size (P11 #22);
  FBX/model thumbnails still show a glyph, not a render (P18 #29).

### Added — features (issues #36, #48, #50)
- **Camera entity.** `CameraComponent` (FOV / near / far), an **Add ▸ Camera** menu item and
  an Add-Component entry, an Inspector section, scene serialization, and a Hierarchy icon.
  The Game view renders through the first placed Camera while editing so a shot can be
  framed without walking there; a "No Camera in scene" hint shows over the view when there
  isn't one (B10 #36). Play mode still uses the first-person controller.
- **Batch Transform.** The multi-select Inspector has a Batch Transform block — relative
  Move / Rotate / Scale applied to every selected entity, then snapped back to 0 / ×1, one
  undo step per nudge (P31 #48).
- **CJK font fallback.** A system CJK face (Microsoft YaHei / MS Gothic / Malgun Gothic) is
  merged over the Japanese glyph range so Chinese / Japanese / Korean entity names render
  instead of tofu boxes. Colour emoji still can't — that needs the FreeType colour backend
  (P33 #50).

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
