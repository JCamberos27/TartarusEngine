# FPS First-Person Animation — System Reference

Branch: `feature/fps-first-person-animation`

This is the **how it works / what's in the tree** document: the asset pipeline, the
runtime, the three fixes this branch landed, and the invariants you have to keep to
avoid re-breaking them. The long forensic writeup of the original "fan of blades"
tear (what was measured, what was ruled out) lives separately in
`FPS_ANIMATION_INVESTIGATION.md` — read that before forming new hypotheses about
skinning.

---

## 1. TL;DR — what this branch fixed

| # | Symptom | Root cause | Fix |
|---|---------|-----------|-----|
| 1 | Arms rendered fine at bind pose, tore into a "fan of blades" the instant any clip played | `Model::ProcessMesh` skipped the owning node's world transform for skinned meshes, while Assimp's `mOffsetMatrix` values expect root-space vertices | `bake = nodeTransform` — bake the node transform for **every** mesh, skinned or not |
| 2 | Arms + weapon floated ~1 m above the player capsule / camera | The rig is authored *standing* (feet at `y=0`, head at `y≈1.56`) but the presentation parked the model's **root** at the camera | Anchor placement on a rig bone (`head`): solve for the root that puts the bone exactly on the camera |
| 3 | Arms and weapon rendered on the opposite side of the camera (behind it) | Blender character faces `-Y` → FBX converts it to model `+Z`; the engine camera looks down its own `-Z`. Exactly 180° out | New `viewRotation` field on the `.fpsanim` asset, `[0, 180, 0]` |
| 4 | The spare magazine existed in Blender but was invisible in the engine — reload showed no second mag in the hand | assimp's `aiProcess_JoinIdenticalVertices` keyed its dedup on position/normal/uv/colour and **ignored bone weights**, so the two coincident magazine islands (identical at rest, told apart only by the `magazine`/`mag2` bones) merged — and the losing bone was left holding 17594 stale vertex ids | Local assimp patch: skin-aware vertex identity + always rewrite `mNumWeights`. Applied at configure time from `tools/assimp_patches/` |

Fix 1 is a renderer bug affecting **any** skinned FBX, not just this rig. Fixes 2 and 3
are first-person-presentation concerns. Fix 4 is upstream of the engine entirely — see
§8 item 8 and `FPS_ANIMATION_INVESTIGATION.md` UPDATE 5; it will re-break silently if
assimp's `GIT_TAG` is bumped without rebasing the patch.

Everything below assumes you've read section 3 (the bake rule) — it is the invariant
most likely to be broken by accident.

---

## 2. The asset pipeline

```
C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend      (read-only ground truth)
        │  Blender export (frame ranges recorded in export_manifest.json)
        ▼
project/assets/fps/AKS74U/
├── AKS74U.fpsanim                       the semantic state machine (15 states)
├── export_manifest.json                 which .blend, which frame range per clip
├── verification_report.json             per-FBX import check ("ok": true for all)
├── FirstPerson/
│   ├── AKS-74U_A_FP_ADS.fbx             armsModel: mesh + skeleton + base node tree
│   └── AKS-74U_A_FP_<State>.fbx         one per state: baked animation only
└── Weapon/
    ├── AKS-74U_A_W_ADS.fbx              weaponModel: mesh + `AK` armature + base node tree
    └── AKS-74U_A_W_<State>.fbx          one per state (not every state has one)
```

### Export rule: pair the weapon action with the arms action

`CB_ik_hand_l` carries `CHILD_OF -> AK:magazine` at influence 1.0, so **the left arm is
glued to the weapon armature's magazine bone** — Blender's bake samples the weapon as
much as it samples the arms. An arms clip must therefore be exported while the weapon is
playing the action `AKS74U.fpsanim` will play alongside it:

| arms clip | weapon action during the bake |
|---|---|
| `A_FP_<x>` | `A_W_<x>`, when that action exists |
| Idle / Walk / Aim / Draw / Regrip (empty `weapon` in `.fpsanim`) | `A_W_ADS` — the weapon holds its bind pose, and the bind pose *is* the ADS export |

`work/export_clip.py` derives this from the action name, pins the weapon's NLA off for
the duration, and restores both armatures in memory.

**Skipping it is not subtle.** The shipped `AKS-74U_A_FP_Idle.fbx` was baked with the
file still saved on `A_W_Tac_Reload`, whose magazine starts moving around frame 44 —
inside the idle's 0..128 range — so the left hand chased a magazine being pulled out of
the gun while the right hand sat still:

| clip | `hand_l` world path, before → after re-export |
|---|---|
| Idle | 223.6 mm → **2.1 mm** (right hand: 2.6 mm) |
| Empty_Reload | 2324 mm → 3062 mm |
| Inspect | 1140 mm → 881 mm |
| Melee | 1057 → 999 mm |

> **Correction — see `FPS_ANIMATION_INVESTIGATION.md` UPDATE 6.** An earlier revision of
> this section claimed `Mag_Check` "and the other twelve measured identical under both
> pairings and were left alone". That was **wrong for `Mag_Check`**, and the clip shipped
> visibly broken because of it. A clean A/B — same action, same `0..260` range, same
> `meshes=0`, *only* the paired weapon action differing — settles it:
>
> | MagCheck bake | `hand_l` world path |
> |---|---|
> | shipped (as found) | 1085.7 mm |
> | rebaked against `A_W_Tac_Reload` | **1086.3 mm** (Δ 0.6 — matches shipped) |
> | rebaked against `A_W_Mag_Check` | **1062.9 mm** (Δ **−22.8** — the fix) |
>
> Pairing alone moves the hand **23.4 mm**, and the shipped file is the `A_W_Tac_Reload`
> bake to within noise. The whole 4:00:42 batch has since been re-exported with correct
> pairing. Re-check any clip with `work/hand_probe.cpp` (section 7) or `work/bl_pair_check.py`.

**Clip FBXs are channel-only: always export with `meshes=0`.** Geometry comes from the
`ADS` armsModel, so a meshed export bolts on stray meshes (`Mesh.001`, `Mesh.003`, 267
nodes) and doubles the file for nothing. The clips that work are 265 nodes / 0 meshes.

Export arm clips with **`meshes 0`**: `Model::AttachClip` reads only the clip's channels,
so meshes add nothing but a material — and the material the meshed export brought in
referenced a missing `T_Quantum_Basemesh_Arms_Normal.1003.png`.

### The `.blend` standing rule

**Always ask before changing anything in `AKS-74U 60fps (Revised).blend`.** Read-only
inspection is fine and was the norm all session, but still give a courtesy heads-up.
Never open it in a connected interactive Blender instance that holds a dirty scene —
drive it headless instead:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" --background `
    "C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend" --python work\bl_inspect.py
```

`work/bl_inspect.py` and `work/bl_inspect2.py` are the read-only scripts used for the
world-space / evaluated bounding boxes and bone landmarks quoted below.

### `AKS74U.fpsanim` schema

```jsonc
{
  "armsModel":   "assets/fps/AKS74U/FirstPerson/AKS-74U_A_FP_ADS.fbx",
  "weaponModel": "assets/fps/AKS74U/Weapon/AKS-74U_A_W_ADS.fbx",
  "defaultState": "Idle",
  "viewRotation": [0.0, 180.0, 0.0],   // NEW: Y-X-Z degrees, see section 5
  "clips": [
    { "name": "Idle",  "arms": ".../FP_Idle.fbx", "loop": true, "fade": 0.12 },
    { "name": "Aim",   "armsBindPose": true, "loop": true, "fade": 0.10 },
    { "name": "Fire",  "arms": ".../FP_Fire.fbx", "weapon": ".../W_Fire.fbx", "fade": 0.03 }
    // ...
  ]
}
```

Parsed by `FirstPersonAnimationSet::FromJsonString`
(`src/Game/FirstPersonAnimation.cpp`). Per clip:

- `arms` **or** `armsBindPose: true` — required. `armsBindPose` means "use the base
  model's own bind pose as this state's pose" (used by `Aim`; the base file's node
  tree is the ADS pose).
- `weapon` — optional **by design**. The AK source has no Idle/Walk/Draw/Regrip
  weapon clips, so those states deliberately leave it empty; the runtime must honour
  that fallback rather than guessing a clip name.
- `loop`, `fade` (seconds, 0..5).
- Unknown/absent optional keys fall back to defaults, so older `.fpsanim` files keep
  loading.

`viewRotation` is validated for finiteness at load; a bad value fails the load with a
specific message rather than silently producing a NaN transform.

---

## 3. Vertex space and the bake rule (the important invariant)

### The spaces

```
mesh-local  --[ ProcessMesh `bake` ]-->  model ROOT space  --[ palette ]-->  ? 
                                                                           
palette[i] = GlobalInverseTransform * PosedGlobal[i] * BoneOffset[i]
```

`GlobalInverseTransform` is `inverse(rootNodeTransform)` (identity for these files).
`PosedGlobal` comes from `EvaluatePose()` walking `m_D->Nodes` parents-first.

The shader (`src/Renderer/shaders/ModelVertex.glsl`) does standard weighted-blend
skinning: `skinMat += uBones[id] * weight` over 4 slots, `totalWeight <= 0.0001 →
identity`.

### The rule

```cpp
const glm::mat4& bake = nodeTransform;   // ALWAYS — skinned or not
```

`Model::ProcessMesh` folds the owning node's world transform into the vertex data for
**every** mesh. The palette never mentions the mesh node's own frame, so this is the
only thing that puts the vertex into root space before skinning.

The winding-flip correction (for a negative-determinant node transform) now applies to
skinned meshes too, for the same reason.

### Why it read as "no bug at bind, tears when a clip plays"

At bind pose the entire palette collapses to `GlobalInverse * C`, a *single* matrix
shared by every bone — a missing node transform just uniformly reorients the mesh, so
nothing tears. Only once a clip plays does each bone apply its own delta to that
un-rotated vertex, blowing a 0.02 m edge apart into 0.5 m blades.

### `C` is not an error term — do not cancel it

Define `C = RestGlobal * BoneOffset`. It is tempting to treat `C` as residual error and
apply `bake = C⁻¹ * nodeTransform`. **That is wrong**, and it was tried:

- Assimp's FBX path builds `mOffsetMatrix = inverse(TransformLink) * absolute_transform`
  (`FBXConverter.cpp:1674`), where `absolute_transform` is the **root** node's transform
  — identity here.
- So `C` is the node tree's **pose-versus-bind delta**, not a residual:
  - arms base node tree = rest → `C == I`
  - clip FBX node trees = frame-0 pose → `C` varies per bone (measured spread ≈ 2.16
    for `FP_Idle`, ≈ 2.21 for `FP_Sprint`)
  - weapon base node tree = **ADS pose** → `C ≈ translate(-0.069, 1.503, 0.446)`, i.e.
    the gun-socket delta
- Cancelling it erased the pose Assimp had already baked into the node tree and dropped
  the weapon from the hands down to the model's origin (the floor).

Measurements that pinned this down (`work/pose_probe.cpp`, after the bake fix):

| file | bake | resulting root-space box |
|------|------|--------------------------|
| `FP_Idle` @ t=0 | `nodeTransform` | `x[-0.330, 0.217]  y[0.959, 1.538]  z[-0.088, 0.705]` |
| `FP_Idle` @ t=0 | `C⁻¹·nodeTransform` *(wrong)* | `x[-0.288, 0.219]  y[0.895, 1.699]  z[-0.117, 0.757]` |
| `W_ADS` | `C⁻¹·nodeTransform` *(wrong)* | ≈ origin — the gun on the floor |

`mag2`'s `C` is an outlier on purpose: the magazine bone moves independently. Not a bug.

---

## 4. Runtime placement — the camera-bone anchor

### Code map

| File | Role |
|------|------|
| `src/main.cpp` | `Start` ≈ line 1214; camera setup 705-707 / 1191-1221; `Update(world, player.Cam)` ≈ 2142; `Tick` ≈ 2158 |
| `src/Game/Components.h` | `FirstPersonControllerComponent` — the authored fields |
| `src/Game/FirstPersonPresentation.{h,cpp}` | `Start`/`Stop`/`SetState`/`Tick`/`TriggerAction`/`Update` |
| `src/Game/FirstPersonAnimation.{h,cpp}` | `.fpsanim` parsing + the tiered interrupt rules |
| `src/Renderer/Model.{h,cpp}` | import, skinning, pose evaluation, `NodeTransform` |

`Update()` gives the arms and the weapon the **identical** world pose every frame —
they are two rigs standing in one shared root space, so there is no separate weapon
placement logic to keep in sync.

### The placement math

These rigs are authored **standing in their own scene**: feet at `y = 0`, head at
`y ≈ 1.557`, arms spanning `y ≈ 0.96–1.54`. The player camera is at `EyeHeight = 1.6`
above the capsule base.

The old code parked the model's **root** at `camera.Position + offset`, i.e. at ~1.42 m
— and then the mesh added its own 0.96–1.54 m on top, landing the arms at 2.3–2.9 m
while the camera sat at 1.6 m. That is the "arms way above the collider" bug.

The fix solves for the root that puts a **rig bone** exactly on the camera:

```
world(local) = position + rotation * (scale * local)
⇒ position   = camera.Position - rotation * (scale * boneLocal)
```

- `rotation` still comes from the camera (including pitch), so the rig tracks look
  direction and the bone stays pinned through it.
- `ViewModelOffset` therefore becomes a **residual nudge in the camera's frame** and
  defaults to `{0, 0, 0}`.
- With `CameraBone = "head"` the shoulders land below and slightly behind the camera
  and the hands/gun land in front — a normal first-person frame.

### Config fields

```cpp
std::string CameraBone = "head";   // empty = fall back to root-anchored placement
glm::vec3   ViewModelOffset{0.0f}; // residual, camera frame
glm::vec3   ViewModelRotation{0.0f}; // per-scene Y-X-Z degrees, applied AFTER the asset's viewRotation
float       ViewModelScale = 1.0f;
```

Both new/reworded fields are registered in `src/Game/ComponentRegistry.cpp`, so they
serialize and show up in the Inspector automatically. If the named bone is missing the
presentation logs **one** warning and falls back to root-anchored placement — it does
not spam the console every frame.

### `Model::NodeTransform(name, out)`

```cpp
// Model-root-space transform of a named node, in the current pose (bind when no clip plays).
bool Model::NodeTransform(const std::string& name, glm::mat4& out) const;
```

- Returns the **node's** world matrix, *not* a skinning matrix. Multiply it by the
  entity's world transform to get a bone's world position.
- Returns `false` and leaves `out` untouched when the model has no such node.
- Posed path reads `m_NodeGlobals` (filled by `EvaluatePose`), gated on
  `m_NodeGlobals.size() == nodes.size()` so it never reads an un-initialised scratch
  buffer before the first evaluation or after a reimport.
- Bind path walks each ancestor's `BindLocal` up the parent chain.

**Important:** the `head` bone is present as a **node** (266 nodes, depth 11 under
`spine_05 → neck_01 → neck_02 → head`) but is **not one of the 52 skinned bones** —
so `BoneInfoMap`/`FinalBoneMatrix` will not find it. Only `NodeTransform` will. The arms
model imports as a **single mesh** (`SK_FP_Arms_Manneguin`, 20,411 verts, 52 bones), so
there is no head geometry for the camera to be trapped inside.

---

## 5. Orientation — `viewRotation`

The Manny rig comes out of Blender facing `-Y`. Blender's default FBX axis conversion
maps it to `(x, y, z) → (x, z, -y)`, so `-Y` becomes **`+Z`** in engine space. The
engine camera's local forward is **`-Z`**. The rig therefore faced exactly backwards:
the arms and weapon rendered behind the camera.

Rather than a magic number in scene data, the correction lives on the **asset**:

```jsonc
"viewRotation": [0.0, 180.0, 0.0]     // Y-X-Z degrees
```

because it describes the axis convention of the two FBXs the asset names. It would be
wrong to put it in `ModelImportSettings` (that's per-FBX, and the arms/weapon are
separate files that must stay in agreement) or in every scene that uses the set.

Composition order in `FirstPersonPresentation::Update`:

```cpp
rotation = cameraRotation * QuaternionFromEulerYXZ(m_Set.ViewRotation)  // asset
                                 * QuaternionFromEulerYXZ(m_Rotation);  // scene tweak
```

The anchor math uses this same `rotation`, so adding a `viewRotation` keeps the head
bone pinned to the camera — flipping the model flips the root's offset around with it,
which is what you want (the body ends up *behind* the camera, the arms in front).

### Axis reference used everywhere in this work

Blender (Z-up, cm) → engine (Y-up, m):

```
(x_b, y_b, z_b)  →  (x_b, z_b, -y_b) / 100
```

Bone world position in Blender: `arm.matrix_world @ pose_bone.matrix`.

---

## 6. Verifying changes

```powershell
# headless smoke test over every scene
.\build\Release\TartarusEngine.exe --smoke-test project\scenes
```

Expected for this branch: `FPS_Animation_smoke_play.json` →
`PASS ... frames=100 loadOk=1 newGlErrors=0 newLogErrors=0 playCycles=2`.
`Apartment` and `Sandbox` fail on **pre-existing missing assets** (`Y Bot.fbx` and
friends are gitignored per the Mixamo licence) — unrelated to this work.

No `DIAG` leftovers: `grep -c DIAG src\Renderer\Model.cpp` must return `0`.

### Editor / screenshot gotchas

- The editor auto-reopens the last scene from
  `C:\Users\jacob\AppData\Local\TartarusEngine\editor_prefs.json` (`lastScenePath`);
  back it up before letting a script drive the editor.
- Play mode auto-starts on launch for this setup; `PLAY MODE — changes revert on Stop`
  is in the title bar.
- The window is maximised at 2560×1440, and `read` on a full screenshot returns a
  **downscaled preview**. Click coordinates must come from full-res crops
  (`System.Drawing` crop → read the crop), not from preview pixels — a preview scale of
  ~1.28 makes tab/button clicks miss silently.
- Win32 mouse clicks: `mouse_event(0x02)` down / `0x04)` up via `Add-Type`.
- Use `Window → Console`, not the notification bell, to read logs (the bell truncates).
- Read the scene's `cam`/`yaw`/`pitch` debug overlay in the Game tab to confirm what the
  player camera is actually doing.

---

## 7. Standalone probes (`work/`)

These link directly against the **prebuilt** Assimp + glm under
`build\_deps\` — no engine rebuild, no editor, no UI. This is the preferred way to
test a hypothesis about import/skinning math.

```powershell
$wt = "$PWD\build\_deps"
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && ^
 cl /nologo /EHsc /std:c++17 /MD /O2 ^
 /I`"$wt\assimp-src\include`" /I`"$wt\assimp-build\include`" /I`"$wt\glm-src`" ^
 work\pose_probe.cpp /Fe:work\pose_probe.exe /link ^
 /LIBPATH:`"$wt\assimp-build\lib\Release`" /LIBPATH:`"$wt\assimp-build\contrib\zlib\Release`" ^
 assimp-vc145-mt.lib zlibstatic.lib"
```

**Must be `/MD`, not `/MT`** — the prebuilt Assimp lib is `MD_DynamicRelease` despite the
`-mt` in its filename (that suffix is Assimp's naming convention, not its runtime).
PowerShell 5.1 has no `&&`; chain inside `cmd /c "..."`. They are **not** built by the
main CMake project.

| Probe | What it answers |
|-------|-----------------|
| `pose_probe.cpp` | Decisive arms/weapon bind + posed bounding boxes — the tool that found the `C` misunderstanding and then verified the fix |
| `bake_audit_probe.cpp` | Per-skinned-mesh: what transform `ProcessMesh` actually bakes |
| `forward_skin_probe.cpp` | Forward (CPU) skinning result for a mesh |
| `clip_coverage_probe.cpp` | Which bones a clip actually drives |
| `bone_dump.cpp` | Per-bone `|A−I|`, `|C−I|`, `C`'s translation, hierarchy depth ≤3 |
| `nodes.cpp` | Node-name search across the hierarchy (how `head` was located); `--chain <node>` prints one node's full ancestor chain |
| `align_probe.cpp`, `diag_probe.cpp` | Axis alignment / ad-hoc diagnostics |
| `assimp_probe.cpp`, `full_skin_probe.cpp`, `bone_match_probe.cpp`, `dup_name_probe.cpp` | The earlier bone-math investigations — see `FPS_ANIMATION_INVESTIGATION.md` |
| `bl_inspect.py`, `bl_inspect2.py`, `bl_idle_probe.py`, `bl_idle_probe2.py`, `bl_pair_check.py` | Read-only Blender ground truth: world/evaluated AABBs, bone landmarks, per-bone motion under a given weapon pairing |
| `hand_probe.cpp` | Per-bone motion through a whole clip — left-vs-right path, per-sample rotation/translation step, loop seam, skinned-but-undriven bones. The tool that found the left-hand bug |
| `bind_probe.cpp` | The base FBX's **bind pose** against each clip, node by node: world positions of `head`, `ik_hand_gun` and the hands, plus the worst bind↔clip displacement. Answers "what does the engine paint when no clip is live?" — the tool that found the disappearing-arms bug |
| `fbx_info.cpp` | What is actually inside an FBX: nodes, meshes, and every take it carries — per-bone weight lists and per-take animated node names |
| `mag_probe.cpp` | Out-of-range skin-weight audit under the engine's exact flag set (`--scan` over every shipped FBX), a post-processing **flag bisect**, per-bone raw `aiBone` dumps, and forward-skin positions at chosen ticks. The tool that proved `aiProcess_JoinIdenticalVertices` was deleting the spare magazine |
| `build_probe.bat` | `cmd /c "work\build_probe.bat <name>"` — builds `work\<name>.cpp` with the right vcvars + `/MD` + assimp/glm include and lib paths, so a new probe is one command instead of the `cl` line above |
| `export_clip.py` | Blender-side clip export: applies the pairing rule above, pins the weapon NLA off, restores both armatures **in memory** (never saves the `.blend`), and prints `weapon action paired = …`. Run as `blender -b "<blend>" --python work\export_clip.py -- <action> <out.fbx> <start> <end> <meshes 0|1> [weapon\|auto]` |
| `extract_frames.py` | Frames from a video via Blender's VSE — there is no ffmpeg on this box, so this is how a Play-mode recording gets turned into PNGs/contact sheet for inspection |

**Probes link the *same* assimp the engine does**, which now carries
`tools/assimp_patches/*.patch` (see §8). If you build a probe against a re-cloned
`build\_deps` that hasn't been configured yet, run `cmake --build build` once first or
you'll be measuring unpatched assimp.

**Gotcha:** `AiToGlm` must be a *direct element copy*. `glm::make_mat4(&m.a1)` reads
column-major from Assimp's row-major matrix and silently transposes everything — that
bug sat in `bake_audit_probe.cpp` and made its output meaningless until it was fixed.
Copy the implementation from `work/pose_probe.cpp`.

---

## 8. Known gaps / next steps

1. **`Idle` and `Walk` have no weapon clips.** `AKS74U.fpsanim` leaves their `weapon`
   entries empty, and the runtime falls back explicitly (the weapon holds its current
   pose, which is the ADS/bind pose). Note the `A_W_Idle` / `A_W_Walk` **actions do not
   exist in the `.blend`** — verified with `work/bl_idle_probe2.py` — so this is not an
   export that was forgotten; closing the gap means *authoring* animation. **Ask before
   touching the `.blend`.** Until then the exporter bakes those arms clips against
   `A_W_ADS`, which is exactly what the engine renders alongside them.
2. **`View Model FOV` works, but is captured at Start.** A view-model sub-pass runs at
   the end of `SceneRenderer::RenderScene`, clears depth, re-projects `ViewModelTag`
   entities with their own perspective (near/far shared with the world pass) and
   re-Culls the froxel lists into its own FrameState. The world gather, the SSAO depth
   prepass and the Scene tab all keep the world projection. The value is read in
   `FirstPersonPresentation::Start()`, so it is **not live-tunable** — Stop/Play after
   changing it. `ViewModelFov()` returns −1 when no first-person presentation is active,
   which leaves the pass off; the Inspector clamps the field to 20–150.
3. **Materials/textures are out of scope** per the user. Untextured rendering is
   expected and correct; `Texture: failed to load ...` console errors are known noise,
   as are the `Y Bot.fbx` import failures.
4. **The camera sits on the head bone's origin**, which is the skull base, not the eye
   socket — 1.557 m vs `EyeHeight` 1.6 m. Close enough to read as correct; if you want
   exact eye placement, anchor on an eye bone (none exists on this rig) or nudge with
   `ViewModelOffset`.
5. **`FirstPersonPresentation::Tick`/state-transition logic** was exercised by the
   smoke test's 2 play cycles, not stress-tested across all 15 states in the editor.
   Worth a manual pass through Fire/Reload/Sprint/Melee with the Game tab open.
6. **A one-shot state must request `ClampForever`, never `Once`.** `Once` clears
   `Clip` at the end, which drops `UploadBoneMatrices` and `Model::NodeTransform` back
   to the base FBX's **bind pose** — and for these rigs that bind pose is the Manny
   T-pose (`work/bind_probe.cpp`: `ik_hand_gun` 678 mm from where the animation puts
   it), so the arms *and* the socket-parented weapon both blink out of frame, and the
   next crossfade then travels through the T-pose. `Play()` therefore uses `ClampForever`
   for non-looping states and reads completion with `Model::AnimationFinished()`, since
   a held clip never clears `IsPlayingAnimation()`. `Once`'s own "returns to bind pose"
   contract is untouched for every other caller. See `FPS_ANIMATION_INVESTIGATION.md`
   UPDATE 4 for the measurements.
7. **The weapon's `mag2` bone is 121.8 mm off bind at t=0 *and* tEnd of every weapon
   clip** (measured with `work/bind_probe.cpp`, node globals vs node globals — worst node
   in the rig, worst rotation 0.0°, i.e. a pure translation), and both magazine islands
   sit exactly on top of each other *at* bind. `Play()` used to call `StopAnimation()`
   whenever it entered a state with no weapon clip (Idle/Walk/Aim/Draw/Regrip), which is
   `PlayAnimation(-1)` with an implicit **fade of 0** — so the spare magazine hard-cut in
   and out at both ends of every weapon clip. **Fixed:** that branch now calls
   `PlayAnimation(-1, clip.Fade, wrap)`, a supported fade-to-bind. `Model::PlayAnimation`
   starts a fade whenever a clip is currently live, `NodeTransform` stays posed for the
   whole fade (so the socket tracks it) and lands on the bind walk exactly when the fade
   ends, and `m_ActionGateWeapon = !WeaponClip.empty()` means this branch never waits on
   `AnimationFinished()`, which would otherwise see `Clip == -1` and report done at once.
   The clip files were **not** touched, so if that snap was authored behaviour the data is
   exactly as it was.
8. **assimp is patched locally; a fresh clone depends on that patch.** assimp comes from
   github at a pinned `GIT_TAG` (`CMakeLists.txt`) rather than a fork, and stock v5.4.3's
   `aiProcess_JoinIdenticalVertices` silently destroyed the spare magazine: its dedup key
   ignored bone weights, so the two *coincident* magazine islands — modelled exactly on
   top of each other and told apart only by the `magazine` / `mag2` bones — merged into
   one, and the losing bone was then left holding 17594 stale vertex ids. Every engine
   path downstream (`ExtractBoneWeights`, `ModelVertex.glsl`) behaved correctly; it just
   received nothing. The fix rides along as
   `tools/assimp_patches/0001-join-vertices-keep-skin-bindings-apart.patch`, applied at
   configure time by `tools/apply_assimp_patches.cmake`. **All 11 weapon FBXs were
   affected** (193,534 dropped weights over 27 files); the arms files were clean.
   Consequences to remember: (a) `build/_deps` no longer survives a clean configure
   unpatched — that's what the hook is for; (b) if you ever bump assimp's `GIT_TAG`, the
   applier fails the configure loudly instead of building without the fix, and the patch
   will need rebasing; (c) don't "optimise" it away with the per-asset
   `importer.optimizeGraph=false` fallback — that keeps the mesh correct but costs
   29215 → 130947 verts (+348%) versus +18.7% for the patch. See
   `FPS_ANIMATION_INVESTIGATION.md` UPDATE 5.
9. **`EmptyReload` is authored but only reachable via a temporary debug key.** It has its
   own arms and weapon clips (`AKS-74U_A_FP_Empty_Reload.fbx` /
   `AKS-74U_A_W_Empty_Reload.fbx`) and is listed as a `Committed`-tier state, but
   `main.cpp` hardcodes the Reload key:
   `if (InputMap::GetButtonDown("Reload")) …TriggerAction("TacReload");` — gameplay code
   never calls `TriggerAction("EmptyReload")`, because gating it needs "magazine empty"
   and **there is no ammo system at all** (no ammo/rounds/counter anywhere in `src/`).
   **Interim:** `G` fires it (TEMPORARY DEBUG binding in `main.cpp` + `InputMap::Defaults()`);
   confirmed in Play mode. The real fix is an ammo system — **ask before adding one**.
   Two traps were cleared on the way: (a) the binding was dead on arrival because
   `ProjectSettings` let `project/settings.json`'s 13-entry `input` array *replace*
   `Defaults()` wholesale, so any newly-added default key read nothing (`Lookup()` warns
   once, then the key is silently dead) — `InputMap::MergeDefaults` now tops a saved list
   up with missing defaults, pinned by 4 assertions in `TestInputMap`; (b) the clip itself
   is fine — `TacReload`, sharing the same weapon-clip path, shows the spare magazine.

10. **Ten stale arm clips were re-exported — the "4:00:42 batch".** `Mag_Check` was the
    only one that was *visibly* wrong, but all ten now come from a single exporter run
    with correct weapon pairing and `meshes=0`. Measured `hand_l` deltas against what
    shipped: `Mag_Check` **−22.8 mm**, everything else **≤3.2 mm** — because
    `A_W_Tac_Reload`'s magazine only starts moving around frame 44 and every other stale
    clip's range ends by frame 46, so the wrong pairing was *latent* everywhere except
    `Mag_Check`'s 0..260 range. **`ADS` was deliberately not touched:** it is the base
    model (geometry + bind pose + node tree) every clip attaches to, and its animation is
    never played — rebaking it was the one genuinely break-everything risk here. Also
    untouched: `Idle`, `Aim`, `Empty_Reload`, `Inspect`, `Melee`, already exported
    correctly. See `FPS_ANIMATION_INVESTIGATION.md` UPDATE 6.

---

## 9. Things that will bite you

- **Two frames of reference, one pose.** Arms and weapon get *identical* world pose.
  Any arms/weapon separation is root-space mesh placement, never transform code.
- **`C = RestGlobal · BoneOffset` is pose-vs-bind, not error.** See section 3.
- **Don't re-check what `FPS_ANIMATION_INVESTIGATION.md` ruled out** without new
  evidence — each item there was killed with hard numbers, not guesswork.
- **The standalone probes replicate `Model.cpp`, they are not `Model.cpp`.** They
  omitted `ProcessMesh`'s vertex placement entirely — which is precisely where the bug
  was. A faithful replica that agrees with Blender proves *bone transforms*, not the
  whole pipeline.
- **Preview downscales; crops don't.** Derive every UI click from a full-resolution crop.
