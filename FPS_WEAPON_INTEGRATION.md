# Adding a First-Person Weapon

The workflow for bringing a new weapon (arms + weapon rig + clips) into the
first-person system, built from what it took to ship the AKS-74U. Use the checklist at
the bottom as the PR checklist.

For how the runtime works, see **`FPS_ANIMATION_SYSTEM.md`**. This guide links into its
sections as `SYSTEM §n`.

---

## 0. What "a weapon" is to the engine

A weapon is one **`.fpsanim`** file plus the FBXs it names:

- an **arms base** FBX: arms mesh, skeleton, and a bind pose exported at **REST**
- a **weapon base** FBX: weapon mesh plus its own armature
- one **arms clip** FBX per state, channels only
- zero or one **weapon clip** FBX per state, channels only

A scene opts in by setting the player's **First Person Controller → Animation Set** to
the `.fpsanim` and turning **Gravity Gun** off.

No code change is needed for a weapon that fits the existing contract: the same 15
state names, one magazine, semi/full-auto. §6 lists what does need code.

---

## 1. Author and export (Blender)

**Rig requirements**

| Requirement | Why |
|---|---|
| Arms rig has a **camera bone**, e.g. `head` (a node is enough, it doesn't need to be skinned) | The camera is pinned to it (SYSTEM §5) |
| Arms rig has a **gun socket bone**, e.g. `ik_hand_gun` | The weapon root is solved onto it every frame |
| Weapon rig has a **root bone** that the socket carries rigidly | `weaponRoot` |
| Socket → weaponRoot is a **pure rotation**, with no translation, and the same for every clip | The mount is rotation-only by design (SYSTEM §5) |
| No more than **4 bone influences per vertex** matter | The engine keeps the 4 largest and renormalises |

**Export rules** (details in SYSTEM §7):

1. **Arms base:** `pose_position = REST`, `bake_anim = False`, with meshes. Never export it from a posed frame.
2. **Clips:** channels only (`meshes=0` / `object_types={"ARMATURE"}`), `bake_anim_simplify_factor=0`, `add_leaf_bones=False`, no NLA, no all-actions, and `axis_forward=-Z`, `axis_up=Y`.
3. **Pair the weapon action while baking arms clips.** If the arms rig has any constraint targeting the weapon rig (the AK's left hand is `CHILD_OF → AK:magazine`), bake `A_FP_<x>` while the weapon plays `A_W_<x>`. If no weapon action exists, bake against the weapon's bind/ADS action. Getting this wrong makes the hand drift, which is 22.8 mm on MagCheck and 22 cm on Idle.
4. Record each clip's frame range in the weapon's `export_manifest.json`.

**Tooling state:** `work/export_clip.py` implements rules 2–3, but it is AKS-specific. It hard-codes the object names `Armature` and `AK`, the `A_FP_`/`A_W_` prefixes and a mesh-name list. For a new rig:

- Copy it and change those names, or parameterise them.
- Weapon-rig clip exports have no tracked script.

**Ask before modifying any source `.blend`.** Drive Blender headless (`--background`).

**Folder layout** (mirror the AK):

```
project/assets/fps/<Weapon>/
├── <Weapon>.fpsanim
├── export_manifest.json
├── FirstPerson/<prefix>_FP_<Base>.fbx, <prefix>_FP_<State>.fbx ...
└── Weapon/<prefix>_W_<Base>.fbx,  <prefix>_W_<State>.fbx ...
```

Commit each FBX together with the `.meta` file the editor generates for it. `.meta` files are LF, per `.gitattributes`.

---

## 2. Write the `.fpsanim`

Start from a copy of `project/assets/fps/AKS74U/AKS74U.fpsanim`. For the schema and validation rules, see SYSTEM §2.

```jsonc
{
  "armsModel":   "assets/fps/<Weapon>/FirstPerson/<prefix>_FP_<Base>.fbx",
  "weaponModel": "assets/fps/<Weapon>/Weapon/<prefix>_W_<Base>.fbx",
  "defaultState": "Idle",
  "viewRotation": [0, 180, 0],          // 180 if the arms render behind the camera (Blender -Y-facing rig)
  "weaponSocket": "<arms socket bone>",
  "weaponRoot":   "<weapon root bone>",
  "weaponMountRotation": [0, 0, 0],     // measure it, don't guess (step 3)
  "clips": [ /* the 15 states below */ ]
}
```

**States.** The names are the API, so they must match exactly:

| Must exist | Optional (skipped if absent) | Optional in the file, but its key logs an error if missing |
|---|---|---|
| `Idle`, `Walk`, `Sprint`, `Aim` (all `"loop": true`) | `IdleToSprint`, `SprintToIdle`, `Regrip` | `Fire`, `TacReload`, `EmptyReload`, `Inspect`, `MagCheck`, `Melee`, `Draw`, `Holster` |

**Per state:**

- Give an `arms` clip for every state. Give a `weapon` clip only when the weapon moves by itself (bolt, magazine, trigger).
- A state without a `weapon` clip fades the weapon to its bind pose. Make sure that bind pose is a sensible resting pose.
- Suggested fades: resting states 0.08–0.12, transitions/equip 0.05, Fire 0.03.

---

## 3. Measure the mount

Don't eyeball `weaponMountRotation`.

1. Build the probe with `cmd /c "work\build_probe.bat socket_probe"`. The probe needs the configured, patched `build\_deps` (SYSTEM §11).
2. Run `work\socket_probe.exe <armsBase> <armsClip|-> <weaponBase> <weaponClip|->` over several clips.
3. Check that the socket-to-weaponRoot relation is the same rotation with ~0 translation in every clip's frame 0. If it isn't, fix the rig, not the engine.
4. Run `work\hand_probe.exe` on a couple of clips. A hand whose path is much longer than its partner's means a pairing mistake.

---

## 4. Scene setup

1. On the player's **First Person Controller**, set:
   - **Animation Set**: the `.fpsanim`
   - **Gravity Gun**: off
   - **Camera Bone**: your camera bone
   - **View Model FOV**: ~50–60
2. Press Play and check the basics:
   - arms in front, not behind (otherwise flip `viewRotation`)
   - not floating (otherwise check the camera bone)
   - gun in the hands through Sprint, Draw and Holster (otherwise check the socket and mount)
3. **Centre the ADS sights.** Hold `Fire2` and adjust **View Model Offset** (metres, camera frame) and **View Model Rotation** (degrees) until the sights sit on screen centre.
   - To solve it exactly instead, locate both sight points from the mesh with `work/sight_align_probe.cpp` (SYSTEM §5).
   - Play-mode edits revert on Stop, so write the final values into the scene file.
   - The offset also shifts hip fire. That's expected.
4. View-model fields are read at Play start, so Stop and Play again after every change.

---

## 5. Verify

Build, run the unit tests and run the smoke test as in SYSTEM §10. Then:

- `--smoke-test project\scenes`: your scene passes with `newGlErrors=0 newLogErrors=0`. A missing clip or a bad path fails here, because `Start()` attaches every clip up front.
- Console on Play shows: `First-person presentation loaded '<path>' with N semantic states.`
- **Manual pass** in the Game tab:
  - idle for 20 s (Regrip)
  - walk and sprint (transitions)
  - hip fire, then ADS fire semi and full (B)
  - walk while aiming
  - sprint out of ADS
  - R tap at partial and at 0 rounds, and R hold
  - F, Q
  - 1/2/scroll, including mid-reload
- Look at the spare magazine, if the weapon has one, during both reloads. If it's missing, see SYSTEM §8.6.

---

## 6. When a weapon needs code

These are compiled-in today and are the first things to move into the asset when a second weapon arrives:

| Hard-coded today | Where | To generalise |
|---|---|---|
| Magazine 30 | `FirstPersonPresentation::kMagazineSize` | `"magazine"` in `.fpsanim` |
| Fire rate 700 rpm, semi/full only | `kFullAutoInterval`, `ToggleFireMode` | `"rpm"`, `"fireModes": ["semi","auto","burst"]` |
| ADS recoil / walk-bob constants | top of `FirstPersonPresentation.cpp` | a `"procedural"` block |
| State → tier table | `FirstPersonTierOf` | an optional per-clip `"tier"`, keeping today's names as defaults |
| Two slots: 1 = the one set, 2 = unarmed | `main.cpp` input block | a list of Animation Sets on the controller, one presentation live at a time (Holster → swap set → Draw) |
| ADS offset/rotation per **scene** | `FirstPersonControllerComponent` | per weapon in `.fpsanim`, with the scene values as a nudge on top |
| No HUD | `Ammo()`, `IsFullAuto()` exist | a HUD overlay reading them |

Keep new rules as pure functions in `FirstPersonAnimation.h`, with checks in `TestFirstPersonAnimationFSM`. That is how every current rule is pinned.

If you add a new `*Tag` or `*Component` to `Components.h`, register it or allow-list it in `tools/component_registration_allowlist.txt`. CI's Debug job fails otherwise.

If you add a new input action:

- add it to `InputMap::Defaults()`
- add it to `project/settings.json`, because the saved list wins per action
- update the bindings table in SYSTEM §4

---

## Checklist

```markdown
### Weapon: <name>
**Assets**
- [ ] Arms base exported at REST, with meshes; clips exported channels-only
- [ ] Arms clips baked against the paired weapon action (hand_probe path lengths sane)
- [ ] Frame ranges recorded in export_manifest.json
- [ ] FBX + .meta committed under project/assets/fps/<Weapon>/
- [ ] Source .blend untouched, or the change was approved

**.fpsanim**
- [ ] Idle / Walk / Sprint / Aim present and looping
- [ ] All 15 state names spelled exactly, or the unused inputs removed
- [ ] weaponSocket / weaponRoot set; weaponMountRotation measured with socket_probe
- [ ] viewRotation correct (arms in front of the camera)

**Scene**
- [ ] Animation Set assigned, Gravity Gun off, Camera Bone set
- [ ] ADS sights centred; offset/rotation saved to the scene file (not just in Play)

**Verification**
- [ ] Unit tests exit 0
- [ ] Smoke: scene PASS, newLogErrors=0
- [ ] Manual pass: idle/regrip, walk, sprint, hip + ADS fire (semi/full), ADS walk,
      sprint-out-of-ADS, tac/empty reload, mag check, inspect, melee, draw/holster
- [ ] FPS_ANIMATION_SYSTEM.md updated if any rule, key or constant changed
```
