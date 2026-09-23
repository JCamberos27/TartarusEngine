# Adding a First-Person Weapon

This is the workflow for bringing a new weapon (arms, a weapon rig and its clips) into the
first-person system, based on what it took to ship the AKS-74U. **No code is needed**
for a weapon that behaves like a normal firearm. The whole setup happens in the editor:

- an Animator Controller graph
- a weapon definition (`.fpsanim`)
- the scene's First Person Controller

Use the checklist at the bottom as the PR checklist.

References:
- **`ANIMATOR.md`**: the Animator window, states and transitions.
- **`FPS_ANIMATION_SYSTEM.md`** (cited below as `SYSTEM §n`): how the runtime works.

---

## 0. What "a weapon" is to the engine

| File | What it holds | Edited in |
|---|---|---|
| Arms base FBX | Arms mesh, skeleton, and a bind pose exported at **REST** | Blender |
| Weapon base FBX | Weapon mesh and its own armature | Blender |
| Clip FBXs | One per state, channels only | Blender |
| **`<Weapon>.controller`** | States, transitions, priorities, fades, events and tags. Each state has an `arms` clip and optionally a `weapon` clip | **Animator window** |
| **`<Weapon>.fpsanim`** | Which models and controller, the gun mount, and the gameplay numbers | **Inspector** (select it in the Asset Browser) |

A scene uses the weapon when the player's **First Person Controller → Animation Set** is set
to the `.fpsanim` and **Gravity Gun** is off.

---

## 1. Author and export (Blender)

**Rig requirements**

| Requirement | Why |
|---|---|
| The arms rig has a **camera bone**, e.g. `head` (a node is enough; it doesn't need to be skinned) | The camera is pinned to it (SYSTEM §5) |
| The arms rig has a **gun socket bone**, e.g. `ik_hand_gun` | The weapon's root is solved onto it every frame |
| The weapon rig has a **root bone** that the socket carries rigidly | This is `weaponRoot` |
| Socket → weaponRoot is a **pure rotation**, the same for every clip | The mount is rotation-only by design (SYSTEM §5) |
| At most **4 bone influences per vertex** matter | The engine keeps the 4 largest and renormalises |

**Export rules** (details in SYSTEM §7):

1. **Arms base:** set `pose_position = REST` and `bake_anim = False`, and export with meshes.
2. **Clips:** channels only (`object_types={"ARMATURE"}`), with:
   - `bake_anim_simplify_factor=0`
   - `add_leaf_bones=False`
   - no NLA and no all-actions
   - `axis_forward=-Z`, `axis_up=Y`
3. **Pair the weapon action while baking arms clips.** If the arms rig has a constraint that
   targets the weapon rig (the AK's left hand is `CHILD_OF → AK:magazine`), bake `A_FP_<x>`
   while the weapon plays `A_W_<x>`, or the weapon's bind/ADS action if there isn't one.
   Getting this wrong makes the hand drift: 22.8 mm on MagCheck, 22 cm on Idle.
4. Record each clip's frame range in the weapon's `export_manifest.json`.

**Tooling:**
- `work/export_clip.py` implements rules 2–3, but it hard-codes the AK's object names (`Armature`, `AK`), the `A_FP_` / `A_W_` prefixes and a mesh list. For a new rig, copy it and change those.
- Weapon-rig clip exports have no tracked script yet.
- **Ask before modifying any source `.blend`.** Drive Blender headless (`--background`).

**Folder layout** (mirror the AK):

```
project/assets/fps/<Weapon>/
├── <Weapon>.fpsanim
├── <Weapon>.controller
├── export_manifest.json
├── FirstPerson/<prefix>_FP_<Base>.fbx, <prefix>_FP_<State>.fbx ...
└── Weapon/<prefix>_W_<Base>.fbx,  <prefix>_W_<State>.fbx ...
```

Commit every FBX, `.controller` and `.fpsanim` together with the `.meta` file the editor
generates for it.

---

## 2. Build the animation graph (Animator window)

The fastest start is to **copy the AK's graph** and re-point its clips:

1. Duplicate `AKS74U.controller` (Asset Browser → Animation → right-click → Duplicate), and rename it `<Weapon>.controller`.
2. Double-click it to open the **Animator** window.
3. Click each state. In the right panel, set **Motion: arms** and **Motion: weapon** to the new weapon's clips. Pick them from the list, or drag the FBX from the Asset Browser onto the slot.
4. Leave **Motion: weapon** empty for states where the gun doesn't move by itself. The gun then holds its bind pose.
5. Adjust fades (transition **Duration**), add or remove states, and change priorities as the weapon needs. A pump shotgun might loop a `ReloadShell` state, for example. See `ANIMATOR.md` for the full editor.

Alternatively, if you have a flat list of 15 AK-style clips:
1. Write a v1 `.fpsanim` with a `clips` list.
2. Select it and press **Create Controller from Clips**. This generates the standard graph.

**What the graph must provide.** The driver only talks to the graph through these names
(SYSTEM §2):

| Must have | Why |
|---|---|
| Parameters `Speed` (Float), `Sprint` / `Aim` / `Equipped` (Bool), `Ammo` (Int), and triggers `Fire`, `Reload`, `MagCheck`, `Inspect`, `Melee`, `Fidget` | The driver sets these. A trigger with no transition just does nothing |
| Tag `ADS` on the aim state | Makes fire a procedural kick and turns on the walk bob |
| Tag `Reload` on reload states, plus a **`Refill` event at time 1** on each | Blocks R during a reload, and refills only when the reload completes |
| A **`Shot` event at time 0** on the hip-fire state | That's when a round is spent. Without it, hip fire is free |
| A state tagged `Hidden` reached when `Equipped` is false | Hides the rigs when unarmed |
| Tag `Idle` on the idle state | Enables the `Fidget` trigger (regrip) |

**Priorities.** Any State transitions with **Respect Priority** only take over a lower-priority state. The AK uses:

| Priority | States |
|---|---|
| 0 | locomotion |
| 1 | sprint transitions |
| 2 | fire, inspect, fidget |
| 3 | reloads, melee |
| 4 | draw, holster |
| 5 | holstered |

---

## 3. Write the weapon definition

1. Duplicate `AKS74U.fpsanim` and rename it `<Weapon>.fpsanim`.
2. Select it. In the Inspector, set:
   - **Controller**: your `<Weapon>.controller`.
   - **Weapon Socket / Weapon Root / Mount Rotation**. **Measure** the mount; don't guess:
     1. Build the probe: `cmd /c "work\build_probe.bat socket_probe"`.
     2. Run `work\socket_probe.exe <armsBase> <armsClip|-> <weaponBase> <weaponClip|->` over several clips.
     3. Socket → weaponRoot should be the same rotation, with about zero translation, in every clip. If it isn't, fix the rig, not the engine.
   - **View Rotation**: 180 Y if the arms render behind the camera (a Blender `-Y` rig).
   - **Gameplay**: magazine, rounds per minute, full-auto allowed, reload-hold time, fidget timing, ADS recoil and walk bob.
3. The arms and weapon model paths are shown read-only in the Inspector. Edit them in the file.

Every field is validated on load. A bad value shows its error in the Inspector, and Play refuses to start the weapon.

---

## 4. Scene setup

1. On the player's **First Person Controller**, set:
   - **Animation Set**: the `.fpsanim`
   - **Gravity Gun**: off
   - **Camera Bone**: your camera bone
   - **View Model FOV**: about 50–60
2. Press Play and check:
   - the arms are in front of the camera, not behind it (otherwise fix View Rotation)
   - they don't float (otherwise fix Camera Bone)
   - the gun stays in the hands through Sprint, Draw and Holster (otherwise fix the socket or mount)
3. **Centre the ADS sights.**
   - Hold `Fire2` and adjust **View Model Offset** and **View Model Rotation** until the sights sit on screen centre.
   - To solve it exactly, use `work/sight_align_probe.cpp` (SYSTEM §5).
   - Write the final values into the scene file: Play-mode edits revert.
4. View-model fields and the weapon definition are read when Play starts, so **Stop and Play again** after each change. Controller edits apply live.

---

## 5. Verify

Run the build, unit tests and smoke test as in SYSTEM §10. Then:

- `--smoke-test project\scenes`: your scene passes with `newGlErrors=0 newLogErrors=0`.
- The Console on Play shows: `First-person presentation loaded '<path>' (<controller>, N states).`
- **Watch it live.** Open the controller in the Animator during Play. The playing state is highlighted, with its progress bar.
- **Manual pass** in the Game tab:
  - idle for 20 s (fidget)
  - walk, sprint, and sprint out of ADS
  - hip fire, then ADS fire in semi and full (B)
  - walk while aiming
  - tap R at partial ammo and at 0 rounds, and hold R
  - F and Q
  - 1, 2 and scroll, including mid-reload
- If the weapon has a spare magazine, check it's visible during both reloads. If it's missing, see SYSTEM §8.6.

---

## 6. When a weapon does need code

| Still in code | Where | To generalise |
|---|---|---|
| Two slots: 1 = this weapon, 2 = unarmed | `main.cpp` input block, `FirstPersonPresentation` | A list of weapon definitions on the controller, swapped on Holster → Draw |
| The input → parameter mapping | `FirstPersonPresentation` (`FirstPersonAnimatorContract`) | New inputs, e.g. a fire-mode selector animation, need a new parameter name there |
| ADS fire and walk bob are procedural | `FirstPersonPresentation::Fire` / `Tick` / `Update` | If a weapon ships real ADS fire/walk clips, drop the `ADS` tag from its aim state and build the logic in the graph |
| No HUD | `Ammo()`, `IsFullAuto()` exist | A HUD overlay that reads them |

If you add a new `*Tag` or `*Component` to `Components.h`, register it or allow-list it in
`tools/component_registration_allowlist.txt`, or CI's Debug job fails. A new input action goes
in `InputMap::Defaults()` **and** `project/settings.json`.

---

## Checklist

```markdown
### Weapon: <name>
**Assets**
- [ ] Arms base exported at REST, with meshes; clips exported channels-only
- [ ] Arms clips baked against the paired weapon action (hand_probe path lengths look sane)
- [ ] Frame ranges recorded in export_manifest.json
- [ ] FBX + .controller + .fpsanim, each with its .meta, committed under project/assets/fps/<Weapon>/
- [ ] Source .blend untouched, or the change was approved

**Controller**
- [ ] Every state's arms clip set; weapon clips wherever the gun moves by itself
- [ ] Contract names present: Speed/Sprint/Aim/Equipped/Ammo + Fire/Reload/MagCheck/Inspect/Melee/Fidget
- [ ] Tags: ADS (aim), Reload (reloads), Hidden (holstered), Idle (idle)
- [ ] Events: Shot @0 on hip fire, Refill @1 on every reload
- [ ] Any State transitions: Respect Priority on; priorities make sense

**Weapon definition**
- [ ] Controller set; socket/root set; mount rotation measured with socket_probe
- [ ] View rotation correct (arms in front of the camera)
- [ ] Gameplay numbers set (magazine, rpm, full-auto, recoil, bob)

**Scene**
- [ ] Animation Set assigned, Gravity Gun off, Camera Bone set
- [ ] ADS sights centred; offset/rotation saved to the scene file (not just in Play)

**Verification**
- [ ] Unit tests exit 0
- [ ] Smoke: scene PASS, newLogErrors=0
- [ ] Manual pass: idle/fidget, walk, sprint, hip and ADS fire (semi/full), ADS walk,
      sprint out of ADS, tac/empty reload, mag check, inspect, melee, draw/holster
- [ ] Docs updated if any rule, key or contract name changed
```
