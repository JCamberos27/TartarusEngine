# FPS First-Person Animation — System Reference

Branch: `feature/fps-first-person-animation`

This is the **how it works** document: what the system is made of, how a frame flows
through it, the asset format, the gameplay rules, and the invariants that keep it from
re-breaking. Three companion documents:

| Document | Read it when |
|---|---|
| **`FPS_WEAPON_INTEGRATION.md`** | You are adding a new weapon (or re-exporting this one). Step-by-step workflow + checklist |
| **`FPS_ANIMATION_INVESTIGATION.md`** | You are about to form a new hypothesis about skinning/import. Everything already measured and ruled out, with numbers |
| `tools/assimp_patches/README.md` | You are bumping assimp, or a weapon's spare magazine vanished on import |

---

## 1. Overview

A player with a `FirstPersonController` whose **Animation Set** field names an `.fpsanim`
asset gets a camera-bound pair of rigs during Play: **arms** and **weapon**, each a
separately-animated skinned FBX. Leave the field empty and the controller behaves exactly
as it did before this branch (the Sandbox gravity gun is untouched).

```
                 .fpsanim (asset)                    FirstPersonController (scene)
   arms/weapon FBX · 15 named states · socket   Animation Set · Camera Bone · View Model
                          │                          Offset/Rotation/Scale/FOV
                          └──────────────┬───────────────┘
                                         ▼
                     FirstPersonPresentation   (src/Game, runtime only)
          Start ─ spawns 2 entities, attaches + validates every clip, plays defaultState
          Update ─ places arms on the camera bone, weapon on the arms' gun socket
          Tick ─ state machine: resting pose / transitions / one-shots, ammo, regrip, hide
          Stop ─ destroys both entities before the scene snapshot is restored
                                         │  ViewModelTag
                                         ▼
                     SceneRenderer view-model sub-pass (own FOV, depth cleared)
```

Nothing it creates is ever saved: the entities exist only between Play and Stop, and
`ViewModelTag` is runtime-only. The PhysX character stays the only physics authority —
the view model has no collider.

### Per-frame order (`src/main.cpp`, Play loop, `playUsesPlayer` branch)

1. `player.Update(...)` — movement + camera.
2. `firstPersonPresentation.Update(world, player.Cam)` — pose both rigs from the camera.
3. Weapon input (only when the game has input **and** the controller's gravity gun is off):
   `FireMode` → `ToggleFireMode`, `Fire1` → `UpdateTrigger`, `Reload` via
   `FirstPersonReloadButton` → `Reload` / `MagCheck`, `Inspect`, `Melee`,
   `Weapon1`/`Weapon2`/scroll/`Holster` → `SetEquipped`.
4. `firstPersonPresentation.Tick(dt, planarSpeed, sprinting, aiming)` — advance the
   state machine. `aiming` = `Fire2` held.

Because `Update` runs before `Tick`, a state change or hide/unhide shows on the next
frame — one frame of latency, by design, so a frame never mixes two placements.

### Code map

| File | Role |
|------|------|
| `src/Game/FirstPersonAnimation.{h,cpp}` | Pure, unit-tested rules: `.fpsanim` parsing/validation, tiers + interrupt rule, resting-state choice, transitions, reload choice, regrip delay, R tap/hold |
| `src/Game/FirstPersonPresentation.{h,cpp}` | The runtime driver: `Start`/`Stop`/`Update`/`Tick`, `Fire`/`Reload`/`SetEquipped`/`TriggerAction`, ADS recoil + walk bob |
| `src/Game/Components.h` | `FirstPersonControllerComponent` view-model fields, `ViewModelTag` |
| `src/Game/ComponentRegistry.cpp` | Inspector/serialization for those fields; `Animation Set` is an asset-path field |
| `src/Core/InputMap.{h,cpp}` | Default bindings + `MergeDefaults` |
| `src/Renderer/SceneRenderer.cpp`, `RenderFrameContext.h` | The view-model sub-pass (`ViewModelFov`) |
| `src/Renderer/Model.{h,cpp}` | Import, skinning, `NodeTransform`, `AnimationFinished` |
| `src/Assets/AssetDatabase.cpp`, `src/Editor/EditorLayer_AssetBrowser.cpp` | `.fpsanim` registered as an asset type and scanned for references |
| `src/Tests/UnitTests.cpp` | `TestFirstPersonAnimationSet`, `TestFirstPersonAnimationFSM`, `TestInputMap` |
| `tools/assimp_patches/` + `tools/apply_assimp_patches.cmake` | Local assimp fix, applied at configure time |
| `tools/component_registration_allowlist.txt` | `ViewModelTag` is allow-listed (runtime-only) — CI fails without it |

---

## 2. The `.fpsanim` asset

The asset is the contract between an exported weapon and the engine. The shipped one is
`project/assets/fps/AKS74U/AKS74U.fpsanim`.

```jsonc
{
  "armsModel":   "assets/fps/AKS74U/FirstPerson/AKS-74U_A_FP_ADS.fbx",  // required: mesh + skeleton + bind pose
  "weaponModel": "assets/fps/AKS74U/Weapon/AKS-74U_A_W_ADS.fbx",        // required
  "defaultState": "Idle",                   // optional; defaults to the first clip; must name a clip
  "viewRotation": [0.0, 180.0, 0.0],        // optional, Y-X-Z degrees: the FBXs' axis convention (§6)
  "weaponSocket": "ik_hand_gun",            // optional: bone on the ARMS rig the gun rides
  "weaponRoot":   "root",                   // bone on the WEAPON rig that lands on the socket
  "weaponMountRotation": [0.0, 90.0, 90.0], // Y-X-Z degrees, fixed socket -> weaponRoot rotation
  "clips": [
    { "name": "Idle", "arms": ".../AKS-74U_A_FP_Idle.fbx", "loop": true, "fade": 0.12 },
    { "name": "Fire", "arms": ".../AKS-74U_A_FP_Fire.fbx", "weapon": ".../AKS-74U_A_W_Fire.fbx", "fade": 0.03 }
    // ... one entry per state, see the table below
  ]
}
```

Validation (`FirstPersonAnimationSet::FromJsonString`) fails the load, with a specific
message, on: invalid JSON; missing `armsModel`/`weaponModel`; a non-finite
`viewRotation`/`weaponMountRotation`; `weaponSocket` without `weaponRoot` (or vice
versa); an empty `clips` array; a clip with no `name`, or with neither `arms` nor
`"armsBindPose": true`; `fade` outside 0..5; duplicate names; a `defaultState` that names
no clip. Then `Start()` attaches **every** clip up front and fails Play if any file
can't be attached — a bad path surfaces when Play starts, not mid-reload.

Per clip:

- `arms` **or** `armsBindPose: true` — required. `armsBindPose` means "use the base
  model's own bind pose for this state" (no clip). Nothing ships using it today; `Aim`
  has its own clip.
- `weapon` — optional **by design**. With no weapon clip the weapon crossfades to its
  bind pose (which, for this set, *is* the ADS pose) — see §8 item 5.
- `loop` (default false) — resting states loop; one-shots don't.
- `fade` (seconds, default 0.08) — crossfade *into* this state.

### The state contract

State **names** are the interface. The driver refers to them by name, so a new weapon
must use these exact names:

| State | Kind (tier) | Loop | How it's reached | AKS74U arms / weapon clip |
|---|---|---|---|---|
| `Idle` | resting | ✓ | not moving | `FP_Idle` / — |
| `Walk` | resting | ✓ | moving, not aiming | `FP_Walk` / — |
| `Sprint` | resting | ✓ | Sprint held **and** moving (beats aiming) | `FP_Sprint` / `W_Sprint` |
| `Aim` | resting | ✓ | Fire2 held (also while walking) | `FP_Aim` / — |
| `IdleToSprint` | Transition (0) | | Idle → Sprint | `FP_IdleToSprint` / `W_IdleToSprint` |
| `SprintToIdle` | Transition (0) | | Sprint → anything but Aim | `FP_SprintToIdle` / `W_SprintToIdle` |
| `Fire` | Action (1) | | Fire1 at the hip | `FP_Fire` / `W_Fire` |
| `Inspect` | Action (1) | | F | `FP_Inspect` / `W_Inspect` |
| `MagCheck` | Action (1) | | R held ≥ 0.35 s | `FP_Mag_Check` / `W_Mag_Check` |
| `Regrip` | Action (1) | | automatic, 10–20 s of settled Idle | `FP_Regrip` / — |
| `TacReload` | Committed (2) | | R tap, 1–29 rounds | `FP_Tac_Reload` / `W_Tac_Reload` |
| `EmptyReload` | Committed (2) | | R tap, 0 rounds | `FP_Empty_Reload` / `W_Empty_Reload` |
| `Melee` | Committed (2) | | Q | `FP_Melee` / `W_Melee` |
| `Draw` | Equip (3) | | 1, or scroll/H while unarmed | `FP_Draw` / — |
| `Holster` | Equip (3) | | 2, or scroll/H while armed | `FP_Holster` / `W_Holster` |

**Which states are required.** The four resting states (`Idle`, `Walk`, `Sprint`,
`Aim`) must exist — `Tick` switches to them unconditionally, and a missing one logs an
error every frame it's wanted. `IdleToSprint`, `SprintToIdle` and `Regrip` are optional
(looked up with `Find` first, skipped when absent). Every other one-shot is optional in
the file, but pressing its key without it logs `unknown semantic state` — so in
practice ship all 15, or remove the input.

---

## 3. The state machine

The rules are pure functions in `FirstPersonAnimation.h`, pinned by
`TestFirstPersonAnimationFSM`; `FirstPersonPresentation::Tick` applies them.

**Resting state** (`FirstPersonRestingState`), consulted only while no one-shot holds:

```
sprinting && moving  → Sprint      (no sprinting in ADS: sprint drops the sights)
aiming               → Aim         (walking too — procedural bob, §4)
moving               → Walk
otherwise            → Idle        (moving = planar speed > 0.05 m/s)
```

**Transitions** (`FirstPersonTransitionVia`): only the Idle↔Sprint pair was authored.
`Idle → Sprint` plays `IdleToSprint`; `Sprint → X` plays `SprintToIdle` unless `X` is
`Aim` (aiming out of a sprint goes straight to the sights). Everything else crossfades
directly using the destination's `fade`.

**One-shots and tiers** (`FirstPersonTierOf` / `FirstPersonCanInterrupt`): a request
takes over only if it **strictly outranks** what's playing, or re-requests the **same**
state (restart — Fire spam, re-tapping Melee):

```
Transition (0)  <  Action (1)  <  Committed (2)  <  Equip (3)
IdleToSprint       Fire             TacReload         Draw
SprintToIdle       Inspect          EmptyReload       Holster
                   MagCheck         Melee
                   Regrip
```

So: any real action pre-empts a sprint transition; Fire/Inspect/MagCheck/Regrip are
freely interruptible; a reload or melee can only be cut short by Draw/Holster; Draw and
Holster can't interrupt each other (1/2/scroll mid-swap is ignored). While unarmed only
`Draw` is accepted.

**Completion.** One-shots play with `ClampForever` (never `Once` — see §8 item 3) and
are "finished" when `Model::AnimationFinished()` is true for every rig that was given a
clip (a state with no weapon clip doesn't wait on the weapon). Then `Tick` clears the
action, refills the magazine if a reload landed, and resolves the resting state again —
so a reload started in ADS hands back to `Aim` if Fire2 is still held.

---

## 4. Weapon gameplay

All in `FirstPersonPresentation` (`Fire()` / `UpdateTrigger()` / `Reload()` /
`SetEquipped()` / `Tick()`); input is read in `main.cpp` (§1).

| Input (action name) | Default key | Does |
|---|---|---|
| `Fire1` | LMB / L-Ctrl | 1 round per shot. Hip: the `Fire` clip. ADS: procedural kick. Dry trigger does nothing |
| `Fire2` | RMB / L-Alt | Hold to aim |
| `FireMode` | B | Toggle Semi-Auto (default) / Full-Auto (~700 rpm, one round per 86 ms while held). Logged to the Console; resets to semi on Play |
| `Reload` tap | R | `TacReload` with rounds left, `EmptyReload` at 0; nothing when full or already reloading |
| `Reload` hold ≥ 0.35 s | R | `MagCheck` (fires at the threshold; the release then does nothing) |
| `Inspect` | F | `Inspect` |
| `Melee` | Q | `Melee` |
| `Weapon1` / `Weapon2` | 1 / 2 | Draw the weapon / Holster to unarmed |
| scroll wheel, `Holster` | wheel / H | Toggle between the two |
| `Sprint` | L-Shift | Sprint (drops ADS) |

Defaults live in `InputMap::Defaults()`; `project/settings.json` holds the saved list and
**wins** per action, and `InputMap::MergeDefaults` tops it up with any default it lacks.
So changing a default key needs both edits (or delete that action from settings.json).

- **Magazine:** 30 rounds (`kMagazineSize`), refilled when a reload clip *completes*;
  anything that cuts a reload short (Holster) leaves the count as it was. No reserve
  ammo. Running dry never auto-reloads — the player presses R for `EmptyReload`.
- **Regrip:** after 10–20 s (uniform random, re-rolled each time) of uninterrupted
  `Idle`; Action tier, so any input cuts it off. Never from Aim/Walk.
- **Unarmed:** there is no unarmed arms pose, so once `Holster` finishes both rigs get
  `DeactivatedTag` + `InactiveTag` (not drawn, clips paused), cleared again by `Draw`.
- **ADS fire:** the set has no ADS fire clip, and the hip `Fire` clip would pull the
  sights off centre every shot. Settled in `Aim`, `Fire()` keeps the pose and kicks the
  whole view model about the eye in camera space — 1.2° muzzle up plus 14 mm back / 2 mm
  up, 35 ms linear rise then an 80 ms exponential settle — returning exactly to the §5
  sight picture. Each shot re-enters the rise at the current kick, so full-auto chains
  into a shake instead of snapping between rounds.
- **ADS while walking:** there is no aim-walk clip, and the hip `Walk` clip would pull the
  sights off centre, so the `Aim` pose gets a procedural camera-plane figure-eight bob:
  3 mm side-to-side per stride, 1.5 mm vertical per step, stride 2.4 m, full amplitude at
  3.5 m/s, eased in/out at 8/s. Firing while walking in ADS is the kick on top of the bob.

Tuning constants: recoil and bob are at the top of `FirstPersonPresentation.cpp`;
magazine size and fire rate (`kMagazineSize`, `kFullAutoInterval`) in the header; the tap/
hold threshold in `FirstPersonReloadButton::kHoldSeconds`. They are **per-engine, not
per-weapon** today — see §9.

---

## 5. Placement: camera bone, weapon socket, ADS centring

### Camera-bone anchor

The rigs are authored **standing in their own scene** (feet at `y = 0`, head at
`y ≈ 1.557`). Parking the model's root on the camera floats the arms ~1 m overhead, so
`Update()` solves for the root that puts a **rig bone** exactly on the camera:

```
world(local) = position + rotation * (scale * local)
⇒ position   = camera.Position - rotation * (scale * boneLocal)
rotation     = cameraRotation * recoil * viewRotation(asset) * ViewModelRotation(scene)
position    += cameraRotation * (ViewModelOffset + recoilOffset + bob)
```

`boneLocal` comes from `Model::NodeTransform(CameraBone)` in the **current pose**, so the
bone stays pinned through the animation. `head` is a node, not one of the 52 skinned
bones — `BoneInfoMap` won't find it; only `NodeTransform` will. A missing bone logs one
warning and falls back to root-anchored placement.

### Weapon socket

The weapon is **not** given the arms' pose. It rides the arms rig's gun socket:

```
weaponEntity = armsEntity * (socket * mount * weaponRoot⁻¹)
  socket     = arms NodeTransform(weaponSocket)        posed
  weaponRoot = weapon NodeTransform(weaponRoot)         posed
  mount      = rotation-only weaponMountRotation        (0, 90, 90) for this set
```

Only the weapon's **root** is overridden; its own channels (bolt, trigger, magazine…)
still animate. Without the socket the gun barely follows the hands (measured
socket-vs-weapon error: Sprint 14.5 cm, Draw 18.2, Holster 21.7, Aim 67.8). The mount is
**rotation-only on purpose**: a translated mount (af4ad15, reverted in c01f451) pulled
the AK out of the hands.

### ADS sight centring — scene config, not code

With the rotation-only mount, `Aim` leaves the sight line ~5 cm left of and ~3 cm above
the `head` camera. That is corrected on the scene's controller:

```jsonc
// project/scenes/FPS_Animation_smoke_play.json, First Person Controller
"View Model Offset":   [0.0562, -0.032, 0.0]   // camera frame: +x right, +y up, +z back
"View Model Rotation": [-0.24, 0.39, 0.0]      // Y-X-Z degrees, pivots on the head bone = the camera
"View Model FOV":      50.0
"Camera Bone":         "head"
```

"Aligned" means the front post's tip sits centred in the rear U-notch, flush with its
top edge, exactly on screen centre. Both sights were located from the mesh
(`work/sight_align_probe.cpp`, 0.5 mm heightmaps), not from bones:

| sight point (raw weapon frame) | x | y | z |
|---|---|---|---|
| rear notch, centre at shoulder height | −0.0682 | 1.559 | 0.4865 |
| front post, centre of tip | −0.0698 | 1.558 | 0.7235 |

That line is 0.39° right and 0.24° down of camera forward — hence the small rotation;
offset alone leaves the aligned sights ~4 px right / 2 px low. With both, the sights land
within 0.1 px of centre (confirmed in Play at View Model FOV 20). The offset/rotation
apply in every state, so hip fire shifts by the same ~6 cm / ~0.5°, which reads as
normal. All view-model fields are read in `Start()` — **Stop and Play again** after
changing them.

To re-measure (e.g. after re-exporting the rig):
`work\build_probe.bat sight_align_probe`, then
`work\sight_align_probe.exe <A_FP_ADS> <A_FP_Aim> <A_W_ADS> - 0 90 90 <offset xyz> <rotation xyz>`;
re-read the two sight points off the heightmaps first.
Trap: `ads_sight_probe.cpp` (in af4ad15) picks the highest mid-plane vertex as the front
sight — that's the top of the right-hand protective ear, ~5 mm off. Use `sight_align_probe`.

### Scene fields (`FirstPersonControllerComponent`)

| Inspector field | Default | Meaning |
|---|---|---|
| Animation Set | empty | `.fpsanim` path; empty = no view model |
| Camera Bone | `head` | arms-rig node pinned to the camera; empty = root-anchored |
| View Model Offset | 0,0,0 | residual nudge, camera frame |
| View Model Rotation | 0,0,0 | Y-X-Z degrees, applied after the asset's `viewRotation` |
| View Model Scale | 1 | must be finite and > 0 |
| View Model FOV | 60 | vertical FOV of the view-model sub-pass, clamped 20–150 in the Inspector |
| Gravity Gun | — | must be **off**: weapon input is only read when it is |

### View-model render pass

Entities with `ViewModelTag` are drawn last in `SceneRenderer::RenderScene`, after a
depth clear, with their own perspective at `ViewModelFov` (near/far shared with the
world pass), so world geometry can't clip hands 0.3 m from the eye and the weapon
doesn't stretch with the world FOV. They're skipped by the SSAO depth prepass and cast/
receive no shadows. The editor **Scene** tab passes no FOV, so there they draw as
ordinary geometry.

---

## 6. Orientation — `viewRotation`

The Manny rig comes out of Blender facing `-Y`; Blender's FBX axis conversion
(`(x, y, z) → (x, z, -y)`) makes that model **`+Z`**, while the engine camera looks down
**`-Z`** — exactly backwards. The correction is `"viewRotation": [0, 180, 0]` on the
**asset**, because it describes the axis convention of the two FBXs it names — not in
`ModelImportSettings` (per-FBX, and the two files must agree) or in every scene.

Axis reference used throughout: Blender (Z-up, cm) → engine (Y-up, m) is
`(x_b, y_b, z_b) → (x_b, z_b, -y_b) / 100`; a bone's world position in Blender is
`arm.matrix_world @ pose_bone.matrix`.

---

## 7. The asset pipeline

```
C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend      (read-only ground truth)
        │  work/export_clip.py (headless Blender), ranges in export_manifest.json
        ▼
project/assets/fps/AKS74U/
├── AKS74U.fpsanim                       the state contract (15 states)
├── export_manifest.json                 which .blend, which frame range per clip
├── verification_report.json             per-FBX import check ("ok": true for all)
├── FirstPerson/
│   ├── AKS-74U_A_FP_ADS.fbx             armsModel: mesh + skeleton + REST bind pose
│   └── AKS-74U_A_FP_<State>.fbx         one per state: channels only (meshes=0)
└── Weapon/
    ├── AKS-74U_A_W_ADS.fbx              weaponModel: mesh + `AK` armature (bind = ADS pose)
    └── AKS-74U_A_W_<State>.fbx          one per state that has weapon motion (11 files)
```

### Export rules

1. **The base arms file is exported at REST** (`pose_position = REST`,
   `bake_anim = False`). Exporting it from a posed frame made the node tree disagree with
   the skin's bind data (`FPS_ANIMATION_INVESTIGATION.md`, fix 1).
2. **Clip FBXs are channel-only: `meshes=0`.** `Model::AttachClip` reads only channels;
   a meshed export adds stray meshes (`Mesh.001`, 267 nodes vs 265), doubles the file,
   and dragged in a material referencing a missing texture.
3. **Pair the weapon action with the arms action during the bake.** `CB_ik_hand_l`
   carries `CHILD_OF → AK:magazine` at influence 1.0 — the left hand is glued to the
   weapon's magazine bone, so the bake samples the weapon too:

   | arms clip | weapon action during the bake |
   |---|---|
   | `A_FP_<x>` | `A_W_<x>`, when that action exists |
   | Idle / Walk / Aim / Draw / Regrip (no weapon clip) | `A_W_ADS` — the weapon's bind pose *is* the ADS export |

   `work/export_clip.py` applies this automatically and prints `weapon action paired = …`.
   Getting it wrong is not subtle: the old Idle had the left hand chase a magazine
   being pulled out (223.6 mm of hand travel → **2.1 mm** after re-export), and
   `Mag_Check` shipped 22.8 mm off (`FPS_ANIMATION_INVESTIGATION.md` UPDATE 6). Check a
   clip with `work/hand_probe.cpp` or `work/bl_pair_check.py`.
4. **`export_manifest.json` is the frame-range record.** Its `frameStart`/`frameEnd` per
   action are what `export_clip.py` should be given. Nothing regenerates it or
   `verification_report.json`. Both date from the original export, so their `meshes` /
   `meshCount` fields predate rule 2. Keep the frame ranges accurate by hand when adding
   or re-cutting a clip.

### The `.blend` standing rule

**Always ask before changing anything in `AKS-74U 60fps (Revised).blend`.** Read-only
inspection is fine. Drive it headless — never through an interactive Blender holding a
dirty scene:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.1\blender.exe" --background `
    "C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend" --python work\bl_inspect.py
```

The file is saved on the neutral pairing `Armature` → `A_FP_Idle`, `AK` → `A_W_ADS`
(it used to be saved on `A_FP_Tac_Reload`/`A_W_Tac_Reload`, which baked the broken
batch). `export_clip.py` never saves it; `work/set_neutral_action.py` is the only
script that writes it, and `AKS-74U 60fps (Revised).blend.pre-neutral.bak` is the
rollback.

---

## 8. Invariants — the fixes this branch landed, and how to not undo them

| # | Symptom | Root cause | Fix (keep it) |
|---|---------|-----------|-----|
| 1 | Arms fine at bind pose, "fan of blades" the instant a clip played | `Model::ProcessMesh` skipped the node's world transform for skinned meshes, while Assimp's `mOffsetMatrix` expects root-space vertices | `bake = nodeTransform` for **every** mesh |
| 2 | Arms + weapon ~1 m above the camera | Root parked on the camera; rig is authored standing | Camera-bone anchor (§5) |
| 3 | Arms rendered behind the camera | Rig faces model `+Z`, camera looks `-Z` | `viewRotation [0,180,0]` (§6) |
| 4 | Arms + gun blinked to a T-pose at the end of every one-shot | `Once` clears the clip → bind pose (the Manny T-pose) | One-shots use `ClampForever` + `AnimationFinished()` |
| 5 | Spare magazine hard-cut in/out around weapon clips | `StopAnimation()` = fade 0 to bind; `mag2` is 121.8 mm off bind at both clip ends | No-weapon-clip states call `PlayAnimation(-1, clip.Fade, wrap)` — a fade to bind |
| 6 | Spare magazine invisible in the engine | assimp's `JoinIdenticalVertices` ignored bone weights, merged the two coincident magazine islands | Local assimp patch (`tools/assimp_patches/0001-…`) |
| 7 | Gun lagged the hands by up to 68 cm | Weapon clips never move the gun's root | Weapon socket (§5) |
| 8 | New default keys did nothing | Saved `settings.json` input list replaced `Defaults()` wholesale | `InputMap::MergeDefaults` |

### 1. The bake rule (the invariant most likely to be broken by accident)

```cpp
const glm::mat4& bake = nodeTransform;   // ALWAYS — skinned or not
palette[i] = GlobalInverseTransform * PosedGlobal[i] * BoneOffset[i]
```

The palette never mentions the mesh node's own frame, so the bake is the only thing
that puts vertices into root space before skinning. At bind pose the whole palette
collapses to one shared matrix, so a missing node transform just reorients the mesh;
only when a clip plays does each bone apply its own delta and tear it apart. The
winding-flip correction for negative-determinant nodes applies to skinned meshes too.

**`C = RestGlobal · BoneOffset` is not an error term — do not cancel it.** Assimp builds
`mOffsetMatrix = inverse(TransformLink) * absolute_transform`, so `C` is the node tree's
**pose-versus-bind delta**: `I` for the rest-exported arms base, varying per bone for
clip FBXs (frame-0 pose), and `≈ translate(-0.069, 1.503, 0.446)` for the weapon base
(the ADS pose — the gun-socket delta). Applying `C⁻¹` was tried and dropped the weapon
to the floor. `mag2`'s `C` is an outlier on purpose.

### 3. `ClampForever`, never `Once`

`Once` clears `Clip` at the end, so `UploadBoneMatrices` *and* `NodeTransform` fall back
to the base FBX's bind pose — the Manny T-pose (`ik_hand_gun` 678 mm off) — for a frame,
and the next crossfade travels through it. `Play()` uses `ClampForever` for non-looping
states and reads completion with `Model::AnimationFinished()`, since a held clip never
clears `IsPlayingAnimation()`. `Once`'s contract is untouched for other callers.

### 5. Fade to bind, don't cut

`m_ActionGateWeapon = !WeaponClip.empty()` means a no-weapon-clip state never waits on
the weapon's `AnimationFinished()` (which would see `Clip == -1` and report done at
once). The clip files were not touched.

### 6. The assimp patch — a fresh clone depends on it

assimp comes from GitHub at a pinned `GIT_TAG`. Stock v5.4.3's dedup key ignored bone
weights, so the two coincident magazine islands merged and the losing bone kept 17,594
stale vertex ids (193,534 dropped weights over 27 files; all 11 weapon FBXs affected,
arms clean). `tools/apply_assimp_patches.cmake` applies the patch at configure time.
Consequences: (a) `build/_deps` must be configured before probes link against it;
(b) bumping `GIT_TAG` fails configure loudly until the patch is rebased; (c) don't swap
it for `importer.optimizeGraph=false` — that costs +348% verts vs +18.7%. `*.patch` is
checked out LF (`.gitattributes`) because `git apply` needs it.

---

## 9. Known gaps / next steps

1. **One weapon per controller; gameplay constants are engine-wide.** Magazine size
   (30), fire rate (700 rpm), recoil, bob, the two slots (1 = the set, 2 = unarmed) and
   the tier table are compiled-in, not read from the `.fpsanim`. A second weapon today
   means a second scene/controller. `FPS_WEAPON_INTEGRATION.md` §6 lists what a real
   multi-weapon inventory needs.
2. **`Idle`, `Walk`, `Aim`, `Draw`, `Regrip` have no weapon clips** — the `A_W_Idle` /
   `A_W_Walk` actions don't exist in the `.blend` (verified with
   `work/bl_idle_probe2.py`), so closing the gap means authoring animation. Ask first.
3. **No ADS fire / aim-walk clips** — both are procedural (§4). If they're authored,
   the procedural paths in `Fire()` and `Tick()` should defer to them.
4. **View-model fields are captured at `Start()`**, not live-tunable — Stop/Play.
5. **The camera sits on the `head` bone origin** (skull base, 1.557 m) rather than the
   eyes; the ADS offset absorbs it. There is no eye bone on this rig.
6. **No HUD** — ammo and fire mode are Console-only (`Ammo()`, `IsFullAuto()` are there
   for one).
7. **The FPS scene is not in CI.** CI's smoke test runs `tests/smoke-scenes/`; the FPS
   scene lives in `project/scenes/` and is only smoke-tested locally. Adding an FPS smoke
   scene would gate `.fpsanim` loading and clip attachment on every push.
8. **Materials/textures are out of scope.** Untextured rendering is expected;
   `Texture: failed to load ...` and `Y Bot.fbx` import errors are known noise.

---

## 10. Verifying changes

```powershell
cmake --build build --config Release -- /m /v:minimal        # close the editor first (LNK1104)
$p = Start-Process build\Release\TartarusEngine.exe -ArgumentList "--unit-tests" -Wait -PassThru `
       -RedirectStandardOutput build\probe\ut.txt; $p.ExitCode   # 0 = all checks passed
$p = Start-Process build\Release\TartarusEngine.exe -ArgumentList "--smoke-test","project\scenes" -Wait -PassThru `
       -RedirectStandardOutput build\probe\smoke.txt
```

Release is a GUI-subsystem binary: run it through `Start-Process -Wait` with redirected
output or you get no output and no exit code.

Expected: unit tests exit 0; smoke `FPS_Animation_smoke_play.json` →
`PASS ... loadOk=1 newGlErrors=0 newLogErrors=0 playCycles=2`. `Apartment` and
`Sandbox` fail locally on **pre-existing missing assets** (Mixamo files are gitignored).

CI (`.github/workflows/build.yml`) additionally runs
`tools/check_component_registration.py`: any new `*Tag`/`*Component` struct in
`Components.h` must be registered or allow-listed.

Manual pass in the Game tab: hip fire, ADS fire (semi + full), walk while aiming, sprint
out of ADS, tap/hold R at 30 / 15 / 0 rounds, F, Q, 1/2/scroll mid-action, idle for 20 s.

### Editor / screenshot gotchas

- The editor reopens the last scene from `%LOCALAPPDATA%\TartarusEngine\editor_prefs.json`
  (`lastScenePath`); back it up before letting a script drive the editor.
- Play-mode edits revert on Stop — write tuned values into the scene file.
- A full screenshot read back is a **downscaled preview**; derive click coordinates from
  full-resolution crops.
- Use `Window → Console`, not the notification bell (it truncates).
- The Game tab's `cam`/`yaw`/`pitch` overlay shows what the player camera is really doing.

---

## 11. Standalone probes (`work/`)

C++ probes link directly against the **prebuilt, patched** Assimp + glm in
`build\_deps\` — no engine rebuild, no editor. Preferred for any import/skinning
hypothesis. Build one with `cmd /c "work\build_probe.bat <name>"` (vcvars, `/MD`,
include/lib paths). **Must be `/MD`**: the prebuilt lib is `MD_DynamicRelease` despite
the `-mt` in its name. Binaries, logs and re-exported FBXs under `work/` are gitignored;
only sources are tracked.

| Probe | What it answers |
|-------|-----------------|
| `pose_probe.cpp` | Arms/weapon bind + posed bounding boxes — found the `C` misunderstanding, verified the bake fix |
| `bake_audit_probe.cpp` | Per-skinned-mesh: what transform `ProcessMesh` actually bakes |
| `forward_skin_probe.cpp`, `full_skin_probe.cpp` | CPU forward skinning of a mesh |
| `clip_coverage_probe.cpp` | Which bones a clip drives |
| `bone_dump.cpp` | Per-bone `|A−I|`, `|C−I|`, `C`'s translation |
| `nodes.cpp` | Node-name search; `--chain <node>` prints an ancestor chain (how `head` was found) |
| `hand_probe.cpp` | Per-bone motion through a clip: left-vs-right path, loop seam, undriven bones. Found the left-hand pairing bug |
| `bind_probe.cpp` | Bind pose vs each clip, node by node. Found the T-pose blink |
| `socket_probe.cpp` | Socket-vs-weapon-root error per clip; derived `weaponMountRotation` |
| `sight_align_probe.cpp` | Sight heightmaps + solved ADS offset/rotation (§5) |
| `fbx_info.cpp` | Nodes, meshes, takes, per-bone weights inside an FBX |
| `mag_probe.cpp` | Out-of-range weight audit, post-process flag bisect. Proved the assimp vertex-join bug |
| `align_probe.cpp`, `diag_probe.cpp`, `assimp_probe.cpp`, `bone_match_probe.cpp`, `dup_name_probe.cpp` | Earlier investigations — see `FPS_ANIMATION_INVESTIGATION.md` |
| `bl_inspect*.py`, `bl_idle_probe*.py`, `bl_pair_check.py`, `bl_mag_probe.py` | Read-only Blender ground truth |
| `export_clip.py` | Clip export with the pairing rule: `blender -b "<blend>" --python work\export_clip.py -- <action> <out.fbx> <start> <end> <meshes 0\|1> [weapon\|auto]` |
| `inspect_blend.py`, `preflight_neutral.py`, `set_neutral_action.py` | Read the `.blend`'s saved state / (only with approval) park it on the neutral pairing |
| `extract_frames.py` | Frames from a Play recording via Blender's VSE (no ffmpeg here) |

**Gotcha:** `AiToGlm` must be a *direct element copy*; `glm::make_mat4(&m.a1)` silently
transposes Assimp's row-major matrix. Copy the one in `work/pose_probe.cpp`. And the
probes replicate `Model.cpp`, they are not it — they omitted `ProcessMesh`'s vertex
placement, which is exactly where the original bug was.

---

## 12. Things that will bite you

- **The weapon is socket-parented, not co-posed.** Arms get the camera-derived pose; the
  weapon's root is solved onto `weaponSocket` every frame. A gun that drifts from the
  hands is a socket/mount/pairing problem, not placement math.
- **State names are the API.** Renaming a state in an `.fpsanim` silently unhooks its
  input (and logs `unknown semantic state` on use).
- **Input defaults vs `settings.json`.** The saved list wins per action.
- **View-model fields don't hot-reload** — Stop/Play.
- **Don't re-check what `FPS_ANIMATION_INVESTIGATION.md` ruled out** without new evidence.
- **Never touch the `.blend` without asking.**
