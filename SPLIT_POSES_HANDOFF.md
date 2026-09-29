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

## 5. What still needs doing

1. **Sprint still clips the hood**, as the user reported after testing. The probe can't sprint, so this
   isn't measured yet. Next steps:
   - Add a sprint step to `--stock-probe`: drive the Sprint input plus movement, or `TriggerAction` into
     the sprint states.
   - Log the keep-out distance along the **whole** gun, not just the rear `gunLength`. Sprinting, the
     part of the gun near the hood may be further forward than 45 cm.
   - Candidate fixes:
     - raise `gunLength` toward the full gun (~1 m);
     - add a keep-out for the hood's back and sides: a capsule from the neck to above the head rather than
       two spheres;
     - a per-state world offset for Sprint (lower and forward);
     - check whether the sprint states are tagged `IKOff`, and whether the world solve reaches in them.
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
7. **Tests:** `--unit-tests` (7205 checks) and `--weapon-test` pass on this commit. Run `--stock-probe`
   after any change to the world gun or the twins.

## 6. Tools

```
build\Release\TartarusEngine.exe --weapon-test [--smoke-shots <dir>]   # scripted weapon checks, ~90 s
build\Release\TartarusEngine.exe --stock-probe [--smoke-shots <dir>]   # butt / neck / head / hands, ~90 s
build\Release\TartarusEngine.exe --unit-tests
```

Run them with `Start-Process -Wait -RedirectStandardOutput` so stdout is captured. Close the editor
first (`taskkill /F /IM TartarusEngine.exe`): a running editor locks the exe for the build.
