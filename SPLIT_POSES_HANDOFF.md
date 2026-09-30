# Handoff: split first-person / world poses (Remington 870)

Last updated 2026-09-29. This covers the second half of the Remington 870 work: the hip pose, the stock
lock, and the split between what the player's own camera shows and what every other view shows.
For the weapon itself see `FPS_WEAPON_INTEGRATION.md` ("A shotgun: the Remington 870"). For the body, see
`BODY_SETUP.md`.

## 1. The problem

The first-person arms rig holds the shotgun the way view-model rigs do: the stock tucked in by the chin,
and the gun carried high across the chest when sprinting. That looks right from the eye. On the true
first-person body, which the Scene view, shadows and later other players see, the stock went through the
neck and hood:

- aiming, the gun's rear came within 2.5 cm of the neck bone;
- turning right fast, within 7 cm, because the body lags the view;
- sprinting, through the hood.

## 2. What was done, in order

1. **Hip pose offset** (`procedural.aim.hipPosition` / `hipRotation`). A procedural offset on the gun
   with the sights down: the whole rig moves rigidly, and it fades by the ADS zoom, so aimed pumps and
   reloads count as sights-up.
   - It is **no longer used by the Remington**: the user wants first person to keep the animations' own
     pose.
   - The engine feature and its Inspector rows ("Hip Pose" in the ADS section) are still there, so any
     weapon can opt in.
2. **`--stock-probe`**, a diagnostic run on the `--weapon-test` harness.
   - It holds the Remington through:
     - pitches from -75° to 60°;
     - turns at 90, 180, 360 and -180 °/s;
     - pitch sweeps;
     - fire and pump at three pitches;
     - aiming at three pitches.
   - It logs `[StockProbe]` lines for each case:
     - the butt against the right upper arm, clavicle, neck and head;
     - how close the rear 30 cm of the gun comes to the neck and head bones;
     - the world gun shift;
     - how far each world hand is from the gun.
   - With `--smoke-shots <dir>` it also saves captures (see §5 for why the Scene half is stale).
3. **Stock lock, first version:** slid the body under the gun. This worked, but it moved the camera
   relative to the body. It was replaced by the next step.
4. **Split poses**, the current design.
   - The player's own camera shows the animations' own first-person pose.
   - Every other view shows a world copy: the same animations, with the gun and the body's hands placed
     so the stock sits on the shoulder and clear of the head.

## 3. How split poses work

- **Render flags** (`Components.h`):
  - `OwnerViewOnlyTag`: drawn only by the player's own camera. No other view, no shadow, and no SSAO in
    other views.
  - `HiddenFromOwnerTag`: drawn by every view except the player's own camera, and casts the shadows.
  - They are applied in these places:
    - `SceneRenderer`'s camera pass;
    - the sun, spot and point shadow passes in `main.cpp`;
    - the SSAO depth pre-pass;
    - the Scene search tint.
- **Body twins** (`FirstPersonBody::MakeTwins` / `SyncTwins`):
  - At Play start, with Weapon Arms on, every body piece gets a `[Runtime] World <piece>` object. This
    covers skin, clothing and anything in the outfit, so any character and any outfit works.
  - Each twin is a `Model::CreateInstance()`, so it has its own pose. It copies the piece's materials,
    shadow flags, `OutfitHideTag` (skin under clothing), layer and Hidden Bones.
  - The twins are root objects, placed onto their pieces every frame, so the outfit's hierarchy never
    sees them.
  - An outfit change during Play re-runs Stop/Start, which rebuilds the twins.
- **Two arm solves a frame** (`FirstPersonBody::ArmsLateUpdate`):
  1. The twins take the pieces' pose as it stands: clips, spine and feet.
  2. The existing Weapon Arms solve runs on the pieces, onto the rig's hands. This is first person,
     unchanged.
  3. The world gun shift is computed (below).
  4. The same solve runs on the twins, onto the rig's hands moved by that shift. The twins have their own
     Arm Steadiness and elbow state.
  5. The twin's neck tilts the head toward the eye moved by the same shift (a cheek weld), at most Head
     Tilt, weighted by how shouldered the gun is.
- **World gun shift:**
  - While shouldered, the butt goes into the right shoulder pocket: `pocket` is an offset from
    `upperarm_r` in the chest frame.
  - Always, the rear `gunLength` of the gun is pushed clear of two keep-out spheres:
    - `neckRadius` around `neck_01`;
    - `headRadius` around the head bone plus 7 cm up (for the hood).
  - The shift is capped at `maxShift` and eased over 0.05 s.
- **World gun** (`FirstPersonPresentation::PlaceWorldWeapon`):
  - A second entity drawing the same weapon `Model`, so it has the same pump and shell pose. It is placed
    at the first-person gun plus the shift.
  - It is tagged `HiddenFromOwnerTag` and casts the shadow. The first-person gun becomes
    `OwnerViewOnlyTag`.
  - It hides with the first-person gun (holstered or unarmed).
- **Settings** (`.fpsanim`, `stockLock`):

  ```json
  "stockLock": { "enabled": true, "tags": ["Idle", "Ready", "ADS", "Cycling"], "pocket": [-0.045, 0.03, 0.05],
                 "maxShift": 0.3, "headTilt": 25.0, "blendTime": 0.2,
                 "neckRadius": 0.09, "headRadius": 0.14, "gunLength": 0.45 }
  ```

  - `enabled` turns on the pocket lock only; the keep-outs apply to every weapon.
  - "Shouldered" means a state with one of `tags`, or anything carried on the sights.
  - The Remington's values come from `work/make_remington_fpsanim.py`. Regenerate the weapon with it
    rather than editing the JSON by hand.

## 4. Results (`--stock-probe`, 2026-09-29)

| Case | Before: gun rear to neck | After (world copy) |
|---|---|---|
| Hip idle, any pitch | 10–13 cm | 14.5–17 cm |
| Aim (pitch 0 / -45 / 30) | 2.5 cm | 15–16.5 cm |
| After fast right turns | ~7 cm | ~16 cm |
| World hands off the world gun | – | 0.0 cm in every sample |

The butt sits within about 1–3 cm of the pocket at every pitch, through turns and when aiming. The world
gun moves up to 14 cm from the first-person one when aiming, and 3–11 cm at the hip. First person is
unchanged.

### Sprint (2026-09-29, second pass)

- **Measured.** The probe now walks and sprints (below). Sprinting, the Remington's bore line went
  *through* the drawn head and hood (0.0–0.1 cm from their vertices, 7–22 cm from the butt, on the `Head`
  and `Top` pieces) while the bone spheres still read as clear: the hood sphere's centre stayed 12–15 cm
  off, so the shift was only 0–3 cm. The spheres were the wrong shape, not the length: `gunLength` was
  never the limit, and the sprint states aren't `IKOff`.
- **Fix: a mesh keep-out** (`stockLock.meshClearance`, default 0.05 m, 0 = off). After the spheres, the
  world gun's rear `gunLength` is pushed straight away from the hood sphere's centre until its bore line is
  `meshClearance` from every twin vertex skinned mostly to `neck_01`, `neck_02` or the head bone (the
  hood, collar, face - whatever the outfit), within `maxShift`. `FirstPersonBody::HeadPoints` skins them
  (indices cached per model, cleared at Stop); `FirstPersonBodyClearPush` is the search (unit-tested).
- **Results:** sprinting at pitch 0 / -30 / 20 and turning 90 °/s, the nearest head / hood vertex is now
  3.7–4.1 cm at worst, ~5.5 cm on average (was 0.0). Walk 4.7 cm (was 2.5). The world gun moves up to
  12 cm; the world hands stay on it (0.0 cm). Idle, turns, fire and aim are unchanged (already clear).
- **Probe additions:** `Player::ScriptedMove` / `ScriptMove` / `ScriptSprint` let the harness hold the move
  keys (`Ctx::Move`, `Ctx::Sprint`; the presentation's Tick gets the sprint flag too). Each sample adds the
  speed and, along the whole gun (butt to muzzle): the neck bone, the hood sphere's centre, and the
  head / neck / hood mesh gap with the piece and where along the gun (`FirstPersonBody::HeadMeshGap`).
- **Also fixed:** a crash on weapon swap in `FirstPersonPresentation::StockWorld` - its butt vertices were
  cached by `Model*`, and the new weapon could be allocated where the old one was freed. The cache is now
  reset when the weapon model is set.
- **Not yet eyeballed:** the Scene capture is still stale (item 2), so this is by the numbers. Check it in
  the editor: sprint and watch the Scene view.

### Hands-on fixes (2026-09-29, from the user's recordings)

- **Balaclava off the face** (any rigid head wear): the world head tilts over the stock (Head Tilt), but a rigid
  head-attached piece was placed by `OutfitSystem::UpdateAttachments` on the *first-person* head and copied to its
  twin a frame late. `FirstPersonBody::PlaceHeadAttachedTwins` now puts those twins on the head twin's head bone
  after the tilt, the same way `UpdateAttachments` places the pieces.
- **Head swivelling with the view**: the upper body took the whole view twist (Spine Twist 1) over a 55-90 degree
  window before the feet turned. Sandbox's Player Body now has Turn Threshold 30 (was 55) and Turn Lag Floor 45
  (was 90).
- **Trigger elbow in the chest**: the world arms keep the rig's bend plane, and the view-model rig tucks its elbows
  to a narrower body - the right elbow's joint line sat 0.0-0.5 cm from the drawn torso in ADS (1.5-3 cm at the hip).
  New body setting **Elbow Clearance** (Camera & Arms (advanced), default 0.06 m, 0 = off): each world elbow swings
  about its shoulder-to-hand line (the hand stays on the gun) until the arm around it clears every twin vertex
  skinned mostly to the pelvis or spine (thinned to 3,000 per piece), eased in fast and out slower, keeping to one
  side. `FirstPersonBodyElbowGap` / `FirstPersonBodyElbowClearSwivel` are unit-tested. Probe: right elbow now
  6.3 cm+ everywhere; the swing is 20-40 degrees at the hip and 55-88 degrees on the sights while moving (the probe
  logs `elbow to torso` and `swung` per sample, and has ADS walk / strafe / back / turn steps). Needs judging by
  eye: a big swing reads as a raised trigger elbow; lower the clearance to trade clipping for less swing.

- **Head nodding in animations**: the cheek weld (Head Tilt) followed the pocket lock, which lets go in every
  state outside `tags` - so a hip reload, inspect, shell check or melee tilted the head 25 degrees off at its start
  and back on at its end. The weld now follows the sights only (`CheekWeld` = the eased ADS zoom): 0 at the hip,
  25 degrees on the sights (an ADS reload holds it). Probe: tilt 0.0 through every hip animation.
- **Stock in the chest looking down**: at -89 degrees the pocket still held the butt in the shoulder with the
  barrel at the feet, so the gun lay down the chest (bore line 1.8 cm from the torso). New `stockLock`
  `releaseStart` / `releaseEnd` (-40 / -70 degrees): the pocket lets go as the view pitches down, and let go the
  gun keeps `meshClearance` from the drawn torso too (`TorsoKeepOut`), pushed straight out from the chest.
  Probe: 4.7-5.4 cm at -89 / -80 / -75 (-85 only 3.0: the chest's Reach Lean comes after the push). At -89 the
  left hand ends 1.5 cm off the pushed gun (out of reach). The probe now pitches to -89 and logs the torso gap,
  the head tilt and the head's bend, and plays the hip reload, inspect, shell check, melee and an ADS reload.

## 5. What still needs doing

1. ~~Sprint still clips the hood~~ - fixed by the mesh keep-out above; confirm by eye.
2. **The probe's Scene capture is stale.**
   - `EditorLayer::RequestSceneGameSplit` asks for Scene and Game side by side, but in the headless run
     the Scene tab still didn't render during Play. The capture's left half is a frame from before Play.
   - The numbers aren't affected.
   - Check the Scene window's dock ID in the loaded `imgui.ini`, and `m_SceneViewportVisible` during
     Play.
3. **World muzzle flash and tracers** still come from the first-person gun. Rounds do too, which is the
   usual shooter approach. If the flash is visible in other views, give it the world gun's muzzle
   (first-person muzzle plus `WorldGunShift`).
4. **Vertical pocket error:** fixed by the split (the world gun moves in 3D). No action needed; noted
   because the old body-slide version couldn't.
5. **Feet:** unaffected. The body no longer moves.
6. **The AKS-74U** gets the keep-outs but not the pocket lock. Set `stockLock.enabled` in its `.fpsanim`
   if it should sit in the shoulder too.
7. **Tests:** `--unit-tests` (7221 checks), `--weapon-test` (45 checks) and `--stock-probe` pass with the fixes above. Run `--stock-probe`
   after any change to the world gun or the twins.

## 6. Tools

```
build\Release\TartarusEngine.exe --weapon-test [--smoke-shots <dir>]   # scripted weapon checks, ~90 s
build\Release\TartarusEngine.exe --stock-probe [--smoke-shots <dir>]   # butt / neck / head / hands, ~90 s
build\Release\TartarusEngine.exe --unit-tests
```

Run them with `Start-Process -Wait -RedirectStandardOutput` so stdout is captured. Close the editor
first (`taskkill /F /IM TartarusEngine.exe`): a running editor locks the exe for the build.
