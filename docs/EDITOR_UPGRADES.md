# Editor upgrade recommendations

Reviewed 2026-10-07 before the upstream editor integration. These are proposed upgrades;
the initial review changed documentation only. Ranking weighs everyday authoring benefit,
existing infrastructure, implementation complexity and the risk of losing authored work.
Effort is relative: medium extends an existing subsystem; large crosses several systems.

Upstream integration now supplies per-component Keep Changes After Play, Inspector attributes,
tabs, favorites, hierarchy/folder styling and a ruler. Candidate 3 is consequently a refinement
of the shipped component-level feature: review individual properties and distinguish editor
edits from simulation changes. The ranking and review inventory below preserve the original
audit; some manuals in that inventory were subsequently retired by upstream.

**Interaction-hitch follow-up:** responsiveness is the immediate maintenance priority before
feature expansion. The input-driven undo snapshot stall has been fixed and regression-tested
([PERFORMANCE.md](PERFORMANCE.md)); the live scene still needs a retest. Given the reported
fleeting frame-time spikes, prioritize candidate 5 (profiler capture/comparison) next so any
remaining stalls can be retained and attributed. The table below retains the broader
authoring-benefit ranking from the initial review.

## The five best candidates

| Rank | Upgrade | Main benefit | Effort |
| --- | --- | --- | --- |
| 1 | Project validation panel | Find broken setups before Play or export, with direct navigation to the cause | Medium |
| 2 | Isolated weapon and character preview | Tune the complete animation/procedural result without repeatedly running the scene | Large |
| 3 | Selective Keep after Play | Retain deliberate scene tuning through a reviewed, undoable property diff | Medium to large |
| 4 | Indexed asset dependency browser | Understand dependencies, missing content and the impact of asset operations | Medium to large |
| 5 | Profiler capture and comparison | Inspect a hitch after it happens and measure a change against a saved baseline | Medium |

### 1. Project validation panel

**Current evidence:** body and weapon Inspectors already have Setup checks; Animator has
structural/contract lint; Script IDE has Problems; audio, outfit and import audits have
separate command-line tools. See [SCRIPT_IDE.md](SCRIPT_IDE.md),
[PROJECT_ASSETS.md](PROJECT_ASSETS.md) and [ENGINE_GAME_BOUNDARY.md](ENGINE_GAME_BOUNDARY.md).
[SetupChecksUI](../src/Editor/SetupChecksUI.h) displays local checks, but the reviewed
editor has no unified project report covering unopened scenes and dependent assets.

**First deliverable:** a dockable Problems/Validation panel with Selected Asset, Current
Scene and Project scopes. Reuse existing validators for missing files/GUIDs, controller
contracts, rig bones, script compilation and invalid references. Each result identifies
the asset, object/property, severity and corrective action, and opens the relevant editor.
Distinguish absent private pack content from malformed authored data. Run expensive imports
or external audits explicitly; lightweight scans should be asynchronous and cancellable.

**Acceptance:** a fixture with a missing texture, wrong controller parameter and invalid
script reference reports all three and navigates correctly. A repaired fixture clears the
results. Validation does not rewrite assets, block rendering or present stale results as current.

**Why first:** it brings existing checks together and prevents long debugging sessions before
adding another authoring surface. An established comparison is Unreal's asset validation
workflow with custom rules and selected-asset/project scopes, described in
[Epic's documentation](https://dev.epicgames.com/documentation/unreal-engine/data-validation-in-unreal-engine).
The Tartarus scope above is a recommendation inferred from this repository.

### 2. Isolated weapon and character preview

**Current evidence:** selected-rig state playback/scrubbing and transition previews already
exist in [EditorLayer_AnimatorSelection.cpp](../src/Editor/EditorLayer_AnimatorSelection.cpp).
They sample a motion and apply a pose to the selected scene model. Recoil and camera-shake
Inspectors have solver plots; outfits have an item model preview. These do not provide one
isolated preview of the complete weapon/body assembly, procedural layers and event timing.
See [PROCEDURAL_RECOIL.md](PROCEDURAL_RECOIL.md), [WEAPON_CAMERA.md](WEAPON_CAMERA.md)
and [ENGINE_GAME_BOUNDARY.md](ENGINE_GAME_BOUNDARY.md).

**First deliverable:** a dedicated preview world and model instances, sharing runtime
sampling/pose code. Show both weapon tracks with a timeline, frame stepping, speed and
parameter controls; expose event markers, bone/socket overlays and owner/world views.
Then add ADS, sway, recoil and body IK in the runtime's actual order. Sound/particle audition
should be an explicit control so scrubbing does not repeatedly emit effects.

**Acceptance:** AK and 870 reload/pump poses and event positions match runtime at fixed times.
Closing or switching preview leaves the authored scene, selection and asset bytes unchanged.
Test two preview sessions to ensure pose state is not shared accidentally.

**Why second:** the repository's heaviest authoring workflows concern animation, grip,
sights, clothing and procedural motion. A complete preview removes repeated Play/Stop work;
the value comes from integrating the existing tools, rather than adding basic clip scrubbing.

### 3. Selective Keep after Play

**Current evidence:** [OnExitPlayMode](../src/Editor/EditorLayer_Scene.cpp) restores the
pre-Play scene snapshot; Save is disabled during Play. The integrated Inspector now keeps
opted-in Transform and preset-capable components as one undo step, alongside the Audio
panel's mix/reverb retention. Controller/recoil/weapon file commits also persist during Play
with global history. See [EDITOR_ENHANCERS.md](EDITOR_ENHANCERS.md) and
[EDITOR_HISTORY_AND_INSPECTORS.md](EDITOR_HISTORY_AND_INSPECTORS.md).

**First deliverable:** refine component-level retention to deliberate Inspector edits on existing
authored objects. Track editor-origin property changes, show before/after values, and let
the user select what to retain. Restore the scene first, then apply approved values as one
global undo action. Start with reflected scalar/vector tuning; identify fields that require
restarting Play before their effect can be observed.

**Acceptance:** keep one edited field and discard another; Stop retains only the selected
change, and Undo/Redo round-trips it. Runtime movement, ammo and spawned entities remain
simulation state. Missing objects, prefab instances and changed script schemas produce a
clear unresolved entry instead of binding to a recycled runtime entity ID.

**Why third:** it removes a repeated manual transfer step while preserving the existing
Play/Stop ownership model. Reliable object/property identity and provenance are the main work;
a blind diff of the entire simulated world would capture far too much.

### 4. Indexed asset dependency browser

**Current evidence:** Asset Browser already has **Find References in Scene/Project** and
deletion reference warnings. [FindProjectReferences](../src/Editor/EditorLayer_AssetBrowser.cpp)
scans selected file extensions synchronously for GUID/path substrings, and project results
go to Console. It is a useful foundation, not a typed dependency index. C# strings, wardrobe
files and metadata remaps need deliberate coverage. See [PROJECT_ASSETS.md](PROJECT_ASSETS.md)
and [EditorLayer_ProjectSync.cpp](../src/Editor/EditorLayer_ProjectSync.cpp).

**First deliverable:** maintain an incremental index of authored references with a dockable
Depends On / Used By view. Parse supported formats and retain property locations; label
code/string-derived references as uncertain. Support transitive dependencies, missing targets
and operation-impact previews. Treat duplicate payloads and distinct logical asset identities
separately, as the balaclava example in Project Assets requires.

**Acceptance:** moving an asset with its GUID preserved updates the index without a full
scan; results navigate to the referencing property. A cold/warm query stays responsive.
Deletion/rename previews include unopened scenes and metadata. Never classify an asset as
safe to remove merely because no indexed reference was found: code-generated paths and
external content can be invisible to a static index.

**Why fourth:** it makes asset maintenance easier as weapons, scripts and private packs grow,
and can later supply dependency data to candidate 1. Start with direct references before
attempting a complete code-aware graph or automated cleanup.

### 5. Profiler capture and comparison

**Current evidence:** [EditorModuleStats.cpp](../src/Editor/EditorModuleStats.cpp) already
shows CPU/GPU scope bars and a 120-frame time graph. It reads the latest available scope
samples; GPU results arrive later than CPU results. The repeatable benchmark and statistical
CPU sampling live in command-line workflows described in [PERFORMANCE.md](PERFORMANCE.md).

**First deliverable:** add Record, Freeze and frame selection, retaining per-frame CPU scopes
and correctly associated GPU query results. Mark Play start, imports, builds/reloads and
asset warm-up events. Save captures and compare two runs with matching scene, resolution,
render scale and build metadata. First ship a frame browser with timing tables; a nested
timeline requires collecting timestamped scope events beyond today's aggregated samples.

**Acceptance:** a scripted import or Play-start hitch can be selected after it passes.
Export/reopen preserves timings and run metadata. CPU/GPU samples are associated with their
originating frames; missing/pending GPU queries are explicit. Recording overhead is measured
with the existing benchmark before accepting the change.

**Why fifth:** this turns fleeting live numbers into useful evidence. Record controls and
frame navigation are established profiler workflows; see
[Unity's Profiler window reference](https://docs.unity.com/en-us/engine/6000.0/manual/analysis/profiler/window).
The proposed Tartarus capture format and comparison behavior are repository-specific recommendations.

## Existing features and lower-priority alternatives

Global undo, multi-object reflected editing, asset reference searches, scene recovery,
semantic IDE completion/navigation/rename, shared curve tools and selected-rig animation
preview already exist. Extending them is more valuable than rebuilding them.

A command palette and the outfit Inspector's caching/style cleanup are useful smaller
follow-ups, but the five above address broader authoring bottlenecks. Typed collections,
stable object references and script field-rename migration are worthwhile SDK/schema work,
not a small Inspector change. A managed debugger is a separate process/architecture project;
the IDE already documents why pausing its own process would freeze its UI.

## Documentation review scope

All **46 Markdown files** present outside `.git/` and `build/` were read in full, including
hidden `.github` documentation, the ignored blood README and scratch/package documents.
Truncated command output was re-read in smaller portions. Build-tree dependency documentation
and archived files available only through Git history were excluded. Existing user changes
were retained; recommendations refer to this working tree rather than only committed HEAD.

Reviewed files, grouped for an auditable inventory:

* Root: `PROJECT_STATUS.md`, `THIRD-PARTY-NOTICES.md`.
* `.github`: `pull_request_template.md`.
* `docs`: `ANIMATOR.md`, `AUDIO.md`, `AUDIO_PASS_HANDOFF.md`, `AUDIO_REVAMP_HANDOFF.md`,
  `BLOOD_FX.md`, `BODY_SETUP.md`, `CAS_PARITY.md`, `CHARACTER_OUTFITS.md`, `CSHARP_SCRIPTING.md`,
  `CURVE_EDITOR.md`, `EDITOR_HISTORY_AND_INSPECTORS.md`, `EDITOR_UI.md`, `ENEMY_AI.md`,
  `FPS_ANIMATION_SYSTEM.md`, `FPS_WEAPON_INTEGRATION.md`, `OUTFIT_TODO.md`, `PARTICLE_SYSTEM.md`,
  `PERFORMANCE.md`, `PROCEDURAL_ANIMATION.md`, `PROCEDURAL_RECOIL.md`, `PROJECT_ASSETS.md`,
  `README.md`, `SCRIPT_IDE.md`, `SCRIPTING_API.md`, `SCRIPTING_FRAMES.md`, `SCRIPTING_MANUAL.md`,
  `SKY.md`, `WEAPON_CAMERA.md`, `WEAPON_REIMPORT.md`, `WEAPON_SWAY.md`.
* `extern`: `ImGuiColorTextEdit/UPSTREAM.md`, `imcurve/README.md`.
* `tools`: `assimp_patches/README.md`, `launcher/README.md`, `sky-review/README.md`.
* `tests`: `smoke-scenes/README.md`.
* `project/assets`: `Animations/Mocap/README.md`, `Characters/Quantum/_import/IMPORT_NOTES.md`,
  `Effects/Blood/README.md`, `Effects/Muzzle/README.md`.
* `work/pr-preparation`: `PR_DESCRIPTION.md`, and `THIRD-PARTY-NOTICES.md` in both
  `standalone-package/TartarusEngine-0.1.0-win64` and
  `standalone-package-final/TartarusEngine-0.1.0-win64`.

The review corrected contradictory IDE/custom-Inspector/reference-field guidance, undocumented
Animator previews, retired Animation-folder directions, stale audio/HUD gaps and the status
page's autosave description. Historical animation/parity material is marked as such so its
measurements remain available without defining current behavior.
