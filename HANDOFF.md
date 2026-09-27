# Handoff: where the rendering work stands

Notes for whoever picks this up next (another Claude account or person). Last updated 2026-09-27.
For the engine as a whole, see `PROJECT_STATUS.md`; for the sky in depth, see `SKY.md`.

## 1. What's on `main`

The main pieces, in order:

- **#464 Performance and cleanup:**
  - engine internals;
  - Y Bots removed;
  - `.meta` files re-saved;
  - sun shadow pass optimized: one layered pass for all cascades, instanced depth draws.
- **#465 Physical sky:**
  - Hillaire atmosphere, volumetric clouds, time-of-day sun, moon and stars;
  - image-based lighting captured from the sky;
  - aerial perspective and cloud shadows.
- **#466 Quantum character assets:** from another session.
- **#467 Sky polish (this handoff):**
  - Wind bug fixed: wind offsets wrapped at a period that isn't a whole number of texture tiles,
    so at high frame rates the cloud field flipped between two layouts every frame and never
    drifted. This was the original "sky glitches" report.
  - Distant clouds: noise mips chosen by pixel footprint, and domed cloud tops.
  - A 2048² mipmapped weather map. At 512² its texels showed as straight edges.
  - Cloud shading:
    - the two-stream diffuse term was ~3× too strong and flattened every cloud to white;
    - sky-light occlusion;
    - sharper edges;
    - storm decks kept dark grey rather than black.
  - Cubic B-spline upsampling of the half-resolution clouds.
    - Sub-pixel jitter was tried and rejected: it made edges shake while the camera moved.
  - Stars:
    - anti-aliased (at least half a pixel wide, total light kept);
    - an 8-cell neighbourhood search so the wider glow isn't clipped at cell borders;
    - skipped by day.
  - Sun:
    - refraction flattening near the horizon;
    - brightness compression that keeps its colour, so the setting sun isn't bleached;
    - the output cap now scales the whole colour instead of clipping each channel.
  - Moon:
    - procedural near-side maria (a domain-warped blob field), plus Tycho and other bright craters;
    - Lommel-Seeliger lighting, opposition surge and earthshine;
    - disc brightness placed where the tone curve keeps the face visible;
    - washed out by day.
  - Ground below the horizon is lit by a hemisphere estimate instead of the zenith alone, which
    fixes the black band at dusk.
  - Sky ambient terms are computed once per view (`SkyAmbient.comp`), so the sky is cheaper than
    before.
  - Sandbox scene uses the physical sky at 16:00:
    - neutral colour grade (temperature −6);
    - sun disc at its true 0.53°;
    - moon drawn at 1.8×;
    - fixed-colour fog off.

GPU cost at Medium on an RTX 4060: about 0.6 ms for sky and clouds. The Sandbox runs at about
160 fps in Play.

## 2. Open issues (not fixed)

1. **Fountain water and other transmission materials look pink or orange at night.**
   - Materials: `project/materials/sandbox/water.mat`, `StandardAdvanced` with transmission 0.75.
   - Findings so far:
     - The colour comes from the transmission branch in `ModelFragment.glsl`: setting
       `_TransmissionStrength` to 0 makes the water look right (blue).
     - It isn't the refraction offset landing on nearby objects. Rejecting refraction samples in
       front of the surface, using the SSAO pre-pass depth, changed nothing.
     - It isn't the red car-paint sphere next to it: removing that sphere left the tint.
   - Suspects:
     - the opaque-colour capture (`OpaqueColorCopy`, unit 14): stale mip levels, or a mismatch
       between `screenUV = gl_FragCoord.xy / uScreenSize` and the capture's size or viewport
       offset;
     - the capture being shared between the Scene and Game views.
   - To reproduce:
     - copy `Sandbox.json`;
     - set the `sky` block to `timeOfDayHours` 0.5 and `moonPhaseOffset` 0.73;
     - move the `Fountain` root empty to (-3, 0, -5.5) so it's in front of the review camera;
     - render with `tools/sky-review/render.sh` and look at shot `_0`.
2. **Glossy and clear-coat spheres look very bright at night.** Probably just the orange lamp
   lights dominating a dim, moonlit scene, but check it alongside issue 1.
3. **Distant clouds at the horizon still stack into flat-based strips.** Partly realistic; could
   be improved with more base variation.

Ideas not started:

- Crepuscular rays: shadow the aerial-perspective volume with the cloud shadow map.
- Blending between environment re-captures: while clouds drift, the ambient light is re-captured
  every 4 s.

## 3. Build, test and review

- **Build:** `cmake --build build --config Release --target TartarusEngine`.
  - The desktop shortcut runs `run-editor.cmd`, which rebuilds whatever is checked out in
    `Desktop\TartarusEngine` and launches the editor.
  - Keep that checkout on `main`.
- **Tests:** `build\Release\TartarusEngine.exe --unit-tests` (1584 checks) and
  `--smoke-test tests\smoke-scenes` (19 scenes).
- **Visual review:** see `tools/sky-review/README.md`. Render a fixed set of scenes before and
  after a change and compare the screenshots; this is how all the sky tuning above was done.
- **Performance:** `TartarusEngine.exe --perf-bench <scene-dir>` prints per-pass CPU and GPU times.

## 4. Working notes (user preferences and gotchas)

- **Merge to `main`:** the user wants finished work merged there, so the desktop shortcut shows
  it. Open a PR, let CI pass, merge, and delete the merged branch.
- **Share the checkout:** another Claude session may work in the same `Desktop\TartarusEngine`
  checkout. Do bigger work in a separate worktree, as this handoff's work was done in
  `Desktop\TartarusEngine-sky`.
- **Don't take over the screen:** the user needs their computer while you work. Don't drive the
  editor with computer-use mid-task; use the offline review renders and batch any live checks.
- **Editor side effects:**
  - It re-saves every asset `.meta` file while running. Run `git checkout -- project` afterwards
    in any worktree the editor ran from.
  - It autosaves open scenes, and that resave is lossy: it drops `_comment` fields and reorders
    keys. Check `git status project/scenes` after any editor session.
- **Closing the editor:** a scene with unsaved changes (`*` in the title) shows a save prompt.
  Don't close the user's editor programmatically; ask them to. To rebuild while it's open, rename
  the running `TartarusEngine.exe` first (Windows allows it) and delete it afterwards.
- **Texture units:**
  - Units 30 and 31 are reserved for the sky (`SkyAtmosphere::kAerialUnit` / `kCloudShadowUnit`),
    and 10 is the sky-ambient texture during sky passes.
  - Material maps stop at unit 29 (`ShaderAsset.cpp`). An earlier collision on units 16/17
    broke every draw.
- **Editing shaders from scripts:** heredocs in Git Bash mangle backslashes, so write Python
  patch scripts with a file tool. Shaders use LF line endings; most C++ files use CRLF.
