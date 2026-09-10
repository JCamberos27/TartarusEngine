# Changelog

Running history of notable changes to Tartarus Engine. Newest first.
Dates are `YYYY-MM-DD`. Each entry links the commit(s) that landed it.

---

## Unreleased

### Screen-space ambient occlusion & bloom (#333) — 2026-09-09

Two additive screen-space effects, both computed in linear HDR before the shared Tonemapper
pass so they scale correctly with exposure. Off by default; both toggle from
**Window ▸ Lighting ▸ Post-processing** (and the docked equivalent).

- **SSAO** — a depth-only pre-pass (the existing shadow-depth shader, reused with the scene
  camera's view-projection) feeds a 32-sample view-space hemisphere kernel rotated per-pixel by
  a tiled 4×4 noise texture, range-checked and written to an `R8` occlusion buffer, then
  softened with a 4×4 box blur. The model shader multiplies ambient light by the blurred
  occlusion sample at `gl_FragCoord`; `uSSAOEnabled == 0` (the GL default) is a no-op, so the
  effect costs nothing on shader variants that never opt in. Scene-view only.
- **Bloom** — a threshold pass extracts HDR energy above a tunable luminance cutoff at half
  resolution, preserving colour ratio; a separable 9-tap Gaussian (horizontal then vertical)
  blurs it into a glow buffer that the Tonemapper adds to the linear HDR colour before the tone
  curve runs, so bloom brightens correctly under every operator (Reinhard / ACES / AgX) and every
  exposure setting. `Threshold` and `Intensity` sliders in the Post-processing section.
- **New files** — `src/Renderer/Ssao.{h,cpp}`, `src/Renderer/Bloom.{h,cpp}`, and their GLSL
  passes (`Ssao.frag`, `SsaoBlur.frag`, `BloomThreshold.frag`, `BloomBlur.frag`); `Tonemapper::Apply`
  grew optional `bloomTexture` / `bloomIntensity` parameters (default off, existing call sites
  unchanged).

### Collision system → NVIDIA PhysX 5 (#185) — 2026-09-09

The hand-rolled sub-stepped AABB collision path is gone; the engine now runs a real
**NVIDIA PhysX 5** world, built **from source** as a CMake `ExternalProject` (tag
`107.3-physx-5.6.1`, patched + harvested by `tools/physx_{patch,harvest}.cmake`). PhysX is
confined to the one host translation unit `src/Physics/PhysicsWorld.cpp` behind an opaque
header — never the reloadable game / editor DLLs. The world is created on Play, torn down on
Stop, and every simulated change reverts from the scene snapshot like any other Play-mode
edit. Landed as one stack, PRs #327–#336.

- **Bodies & shapes** — `ColliderComponent` grew Box / Sphere / Capsule / Convex Hull / Mesh,
  half-extents, centre offset, `Is Trigger`, and surface `Bounciness` / `Friction` (one shared
  `PxMaterial` per distinct pair). `RigidbodyComponent` (mass, gravity, kinematic, initial
  velocity, linear / angular damping, continuous collision, and six axis-lock constraints)
  makes a collider a dynamic or kinematic body with pose write-back each frame. Convex-hull
  and triangle-mesh colliders are cooked straight from the entity's render geometry.
- **Player** — the first-person controller moves a `PxCapsuleController`; `World::ResolveCollisions`,
  `AABB::MTV` and `Player::BodyBounds` were deleted. The player pushes dynamic bodies out of
  the way and rides moving kinematic platforms (including their yaw).
- **Gameplay API** (host ABI `kGameModuleAPIVersion` 7) — `Raycast` / `SphereCast` /
  `OverlapSphere` (self-excluding the player capsule), `AddForce` / `AddTorque` /
  `AddForceAtPosition` / `AddExplosionForce` / `SetLinearVelocity`, `GetBodyState`,
  trigger enter/stay/exit events, solid-contact enter/stay/exit events with impact
  point / normal / impulse / closing speed.
- **Filtering & solver** — a custom `EngineFilterShader` applies an 8×8 project-level
  collision-layer matrix, per-body CCD, contact notifications, and enhanced determinism.
- **Joints** — `JointComponent`: Fixed / Hinge / Ball / Slider / Distance, connect to another
  body or the world, anchor + axis, break force / torque, optional motion limits. Editor
  entity-picker Inspector.
- **NaN guard** — a non-finite simulated pose freezes that body at its last good transform
  instead of poisoning the scene.
- **Editor & debug** — collider wireframe overlay (`ColliderGizmo`, on by default) drawing
  every shape incl. the player capsule and the real cooked hull / mesh edges. A lean physics
  **visual debugger**: fading red contact sparks with impulse-scaled normal arrows and a
  shockwave ring, red raycast / sweep traces with hit bursts, green velocity arrows, sleep
  markers — plus a **Window ▸ Physics** panel (live stats, channel toggles, a 0–2× slow-mo
  slider, single-substep Step) and a corner HUD. **F5** toggles the whole overlay over the
  game view during maximized play; **F6** toggles the panel. A Half-Life-2-style gravity gun
  in the physics-playground harness (right-click grab, scroll distance, left-click launch);
  the transform gizmo can drag a live body during Play. OmniPVD `.ovd` capture is wired
  behind `TARTARUS_PHYSX_OMNIPVD` (off by default). `project/scenes/PhysX Playground.json`
  exercises the whole surface.

### Component registration → prefab overrides (#302, #315) — 2026-09-08

- **#302 Part A** — native component **registration + reflection**. One `ComponentRegistry`
  declaration gives a component its JSON serialization, Inspector section, and Add-Component
  entry with no per-component editor code. Field types: bool / int / float / vec3 / string /
  color / enum / asset-reference, each with a drag speed, optional min/max, tooltip, and
  widget hints (slider, log scale, printf format, conditional visibility, collapsible
  sub-groups). Camera, Light (+ Shadows), and Audio Source were migrated onto it; a CI guard
  rail (`tools/check_component_registration.py`) fails the build if a component in
  `Components.h` is neither registered nor allow-listed with a reason. PRs #306–#310.
- **#302 Part B** — **per-field prefab-instance overrides**. A field changed on an instance
  child is kept as an override: the Inspector label is accent-tinted with a right-click
  **Revert to Prefab** / **Apply to Prefab**. Stored as a compact `{e,c,f,v}` delta list on
  the instance stub, diffed against the `.prefab` on save and replayed on load. A `--resave`
  CLI gives the save path headless coverage. PRs #311–#314.
- **#315** — the tail. Override markers extended to the Transform rows, the Name field, and
  the full multi-select Inspector across every field type (#316, #321, #323, #324).
  Add/remove-component overrides on an instance — tinted section header + Revert/Apply for
  the whole component (#317 data model, #319 Inspector). `RenderableComponent` ("Mesh
  Renderer") registered with new `GenericSerialize` / `GenericInspector` opt-out flags, so it
  counts for the guard rail while keeping its hand-coded serializer and section (#322). The
  prefab stage 1–2 manual checklist was run end to end.

### Editor UI audit (#145) — 2026-09-03

- **#155 (audit)** — The multi-select Inspector now draws its component groups through the
  same flat collapsible `BeginComponentSection` as the single-select Inspector (Transform,
  Object, Light, Shadows, Camera, Material), instead of bare `SeparatorText` rules — so the
  two selection modes read identically and every section collapses. `BeginComponentSection`
  lost its unused `world`/`entity` params. **Material is its own top-level section** now in
  both modes (was nested inside Mesh Renderer for single-select). The remove-component ✕
  stays always-visible on the header (not hover-revealed). The resizable property-table half
  of the audit (#154) is still open — an attempt was reverted as not an improvement.
- **#157 (audit)** — Asset Browser folder navigation is now tree-driven only. The toolbar
  breadcrumb went from a run of framed per-segment buttons with `/` glyphs to a single dim
  non-interactive `Assets / Sub / Folder` string (context, not a control). The `".."` go-up
  row at the top of every non-root folder grid is gone — the tree and the Backspace shortcut
  cover "go to parent". Selecting a folder from anywhere else (a folder tile, a search-result
  click) now force-opens its ancestors in the tree and scrolls that node into view, once per
  change so a manual collapse still sticks.
- **#162 (audit)** — Light grab-handle dots no longer hard-vanish the instant the transform
  gizmo is touched. While the gizmo is hovered they fade to ~28% and stop taking the cursor
  (the gizmo always wins the click); a dot within roughly one gizmo-arm's length of the
  light origin fades further on a radial falloff so it never fights the gizmo's own arrows.
  They still disappear entirely while the gizmo is actually being dragged, and the dot being
  dragged always stays full-opacity. Draw order pinned so the gizmo paints over the dots.
- **#152 (audit)** — The three different "is this object active" controls (a framed eye
  button on every Hierarchy row, an Inspector checkbox, a multi-select tri-state checkbox)
  are now one shared borderless eye. Inactive objects show a dim eye-slash; active objects
  show only a 2px hint dot until the row/header is hovered, then a faint clickable eye. The
  multi-select row keeps its tri-state (a "mixed" dash), styled to match. Hierarchy rows
  lose the always-on framed button, and the kind glyph + name pull in toward the eye.

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
