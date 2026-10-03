# Enemy AI

The enemy squad: soldiers that see and hear the player, take cover, flank, bound under covering fire, blind-fire,
fight hand to hand, talk on a Combine-style radio, and die into ragdolls. This page covers how it is built, what
can be tuned, and how it is tested.

## Putting a squad in a scene

- **NPC Spawn** (`NpcSpawnComponent`): one per soldier.
  - Weapon: AKS-74U, Remington 870, or either.
  - Squad number.
  - Skill (0..1): affects reaction, accuracy and aggression.
  - Outfit seed.
  - Brain: the squad AI or a training dummy.
- **Squad Settings** (`SquadSettingsComponent`): on any object in the scene; the first one counts.
  - Squad size.
  - Respawn delay.
  - Difficulty.
  - NPC damage scale.
  - Whether the dead respawn.
  - Groups for the combat numbers (defaults are the old hard-coded values): Damage / Stagger (heavy hit, stagger time),
    Wounded (bleed-out, crawl speed, limp speed and time), Corpses (corpse time, fall gravity), Melee (damage, strike
    and hit time), Distances / LOD (hitbox, foot IK and mesh-check ranges) and Cover (sample spacing, reach, probe
    heights, peek step).
- **Ragdoll Settings** (`RagdollSettingsComponent`, category AI): on any object; the first one counts. Without one the
  defaults below apply. Groups: Joints (anatomical limits on/off), Muscle Tone / Stagger / Settle / Lying / Directional Fall / Reactions / Corpse Hits (the powered ragdoll, below), Death Drive (the old passive fade, used with Powered Ragdoll off), Body Physics
  (damping, solver iterations, depenetration, sleep threshold), Contact (friction, restitution), Impulse Caps (part and
  chest shove speed, corpse-shot shove), Hit Flinch (see Hit flinch below), then per region (Pelvis, Spine, Neck, Head, Upper
  Arm, Forearm, Hand, Thigh, Calf, Foot; left and right share a value) a mass and a range of motion. Hitbox geometry
  (radius, length) is not exposed: it changes gameplay.

Both scenes have a squad. `scenes/Arena.json` is the test arena. `scenes/Sandbox.json` keeps its squad in the
"Enemies" group, west of the fountain.

## Play-mode tools

| Key | |
|---|---|
| F7 | Dev panel (`src/Game/DevPanel.*`). It holds:<br>• god mode<br>• infinite ammo<br>• freeze AI<br>• hold fire<br>• invisible to the AI<br>• difficulty slider<br>• time scale<br>• Kill All<br>• Respawn Squad<br>• the overlay toggle |
| F8 | God mode (a GOD tag shows on the HUD) |
| F9 | AI overlay. It draws, over the squad:<br>• behaviour and phase<br>• utility scores<br>• last decision and callout<br>• cover claims<br>• sight lines |

The combat HUD (`src/Game/CombatHud.*`, text by `src/Renderer/HudText.*`) shows:

- ammo
- kill feed and streaks
- radio subtitles
- awareness chevrons for soldiers that are suspicious but haven't found the player

## Architecture

| File | Role |
|---|---|
| `AI/NpcDirector.*` | Owns the soldiers. Spawns and respawns them, and runs each frame's perceive / decide / move / aim+fire / late pose. Also handles squads and their tokens, damage in and out, and deaths. |
| `AI/NpcBrain.*` | Decisions. A utility choice among behaviours (below), then each behaviour's own small state machine. |
| `AI/AiMath.*` | The pure rules, unit tested:<br>• detection rate<br>• target memory<br>• hit probability and reaction time<br>• utility curves<br>• tactics (`MayBound`, `WantsBlindFire`, `WantsMelee`, `DangerScale`) |
| `AI/CoverSystem.*`, `AI/NavMesh.*` | Cover points probed from the scene (low or high, with peek positions), and the Recast navmesh with a DetourCrowd. |
| `AI/SquadVoice.*`, `AI/NpcDirectorVoice.cpp` | The radio. Each squad gets one channel with priorities, cooldowns and "copy" responders. Callouts come from decisions, and reload, covering, kill and idle chatter come from state edges. |
| `Npc/NpcBody.*` | The soldier's body: outfit pieces on one Quantum skeleton. The torso piece drives; the rest copy its pose. Sprung aim, head look-at, cower, hand signals. |
| `Npc/NpcWeaponHold.cpp` | The weapon hold: the player's world body's solve, step for step and by the player's First Person Body numbers. The weapon's eye hangs off the shoulders as the player's camera does (Head Bob, Camera Smoothing, Eye Slack, Armed Eye Offset, Look Down Push). The gun goes into the shoulder and clear of the head, and the arms go onto the rig's hands, with elbow and cheek-weld clearance against the drawn body. Off the sights the gun is at the hip, the view level along the chest; reloads are worked there, the view level on the threat, the stance kept. `--npc-test reload` against `--stock-probe ak` compares the two. |
| `Npc/NpcHitboxes.*` | Per-bone hitboxes for live soldiers within 60 m: 11 query-only capsules posed from the skeleton. |
| `Npc/NpcRagdoll.*` | Ragdoll deaths, starting from the pose the soldier died in. |
| `AI/NpcTest.*` | `--npc-test` scenarios (below). |

**Frame order:**

1. `Think` runs after the player moves:
   1. perceive
   2. squads
   3. brain
   4. crowd
   5. move
   6. aim and fire
2. `LateUpdate` runs after the animators: body pose, weapon rig, hold, hitboxes and ragdolls.

### Behaviours

| Behaviour | What it does |
|---|---|
| Idle | Patrols near its post. |
| Investigate | Goes to look at a noise or a glimpse. |
| Engage | Strafes and shoots in the open. A rifle closer than 5 m backs off. |
| Take Cover | Gets to cover. |
| Cover Fight | Hides, peeks and shoots, reloads behind cover, suppresses, blind-fires. |
| Flank | Works round the player's side to cover. |
| Push | Closes in, for example while the player reloads. |
| Search | Hunts after losing the player. |
| Retreat | Falls back when hurt. |
| Wounded | Down: crawls for cover and shoots poorly until it bleeds out. |

Each is scored about 4 times a second, with inertia for the current one.

Squad roles are worked out twice a second:

- **Anchor:** the nearest soldier, which holds.
- **Suppressor:** the rest.
- **Flanker:** one per squad, the healthiest soldier other than the nearest.

Tokens limit how many soldiers shoot at once (2-4 with difficulty), and allow one flanker and one pusher at a time.

### Tactics

- **Flanking a camper.** The flank's score rises once the player has held one spot (within 2.5 m) for 4 s, maxing at 10 s, so a camper gets worked round. A flank looks 16 candidates deep for cover round the side. Flanks are 8 s apart.
- **Pincer.** With four or more soldiers up and the first flanker on its way, a second flanker goes round the other side, to cover at least 80Â° round the player from the first's. Pincers are 12 s apart.
- **Pressing the hurt.** A player under 40% health seen by the squad gets a push called on them, at most every 10 s, as a player seen reloading does.
- **Startle.** Caught out (the player within 12 m, or more than 35Â° off to the side), a soldier may duck for a beat before reacting; steadier soldiers less often.
- **Fire and maneuver.** A flanker or pusher the player can see waits, down with the gun up, until a squadmate is shooting. If nobody is, a soldier in Cover Fight is ordered up to give covering fire ("Covering!"). The wait is capped at 1.6 s (0.8 s for a push).
- **Blind fire.** A soldier pinned for 1.5 s (suppression > 0.5) holds the gun out and sprays toward where the player was:
  - up over low cover
  - out past the edge of high cover

  It keeps its head down. Suppression comes from the player's rounds passing within 1.6 m of the head, including rounds that hit its cover.
- **Rifle-butt strike.** A player within 1.9 m and in front gets struck: 25 × difficulty damage, 1.5 s cooldown. The gun thrusts out with a twist.
- **Danger memory.** Cover within 5 m of where a squadmate fell scores lower for 30 s.
- **Fair play:**
  - reaction times before the first shot
  - the opening round goes wide
  - accuracy builds over about 1.4 s on target
  - attack tokens leave the player a moment to act

### Damage and deaths

- **Hit regions.** The player's rounds hit the bone hitboxes, and the bone gives the region: head, torso, arm or leg.
- **Hit reactions:**
  - 40 damage or more staggers the soldier: a beat without aim and a shove.
  - A leg hit makes it limp for 6 s: 60% speed, no flanking or pushing.
- **Wounded.** Under 20% health from a leg or torso hit, there is a 35% chance the soldier goes down wounded. It crawls to cover, calls for help, and dies on the next hit or after 20 s.
- **Death.** The body hands over to the ragdoll on the frame it dies, from that frame's pose. Corpses can be shot, and stay for 14 s.
- **Death momentum.** Each part starts with its own bone's linear and angular velocity, the finite difference of the pose at the hit (captured in `Kill`) against the dying frame's over the real frame time, in the body's root frame, added to the body's velocity. A soldier shot mid-stride keeps his limb swing. Clamped by Max Limb Speed (8 m/s) and Max Limb Spin (30 rad/s); a held (LOD) frame adds nothing; Limb Velocity Scale 0 turns it off. The capsules' roll about their own axis is not part of the pose, so spin about the bone is not inherited.
- **Shaped inertia.** PhysX already derived each capsule's inertia from its shape; the pelvis and chest now turn like a box wider than deep (Torso Half Width / Depth, inertia only, hitboxes unchanged), so bending forward is easier than sideways. Inertia Scale multiplies every part's (stability knob). Off: Shaped Torso Inertia.
- **Powered ragdoll (muscle tone).** A death is no longer a passive rag (`NpcRagdollMotor`, Powered Ragdoll on by default; off restores the old Death Drive fade and physics). After death every joint keeps a slerp drive toward a target pose, `k = Tone Stiffness x s^2`, `c = max(Tone Damping x s, joint friction floor)`, where the strength `s` runs from 1 down to Tone Residual (6%, never zero) in `(1 - x)^2` per region: Legs, Spine, Neck (and head), Arms (and hands) each with their own Tone Time (0.6 / 1.1 / 1.4 / 1.4 s, legs after the stagger). The target blends from the pose the soldier died in to a procedural collapse over Collapse Blend Time (0.55 s): hips fold 35 deg, knees 70, shoulders 35 and elbows 50 (the arms brace when the body falls forward, less when it falls back), the spine curls 18 deg forward (arches a little back when falling back) and the neck tucks. Targets are in each anatomical joint's own frame (+Z = flexion), so they only apply with Anatomical Limits on.
- **Stagger.** The legs start at Stagger Leg Strength (0.7) and hold it for Stagger Time (0.35 s) before their tone decays, so the body is carried by the round and topples about its feet instead of dropping; the struck joint (the struck part's, both hips for a pelvis hit, neck and head for a head hit) starts at Hit Weakness (25%) and gives way. The round's shove is multiplied by Hit Impulse Scale (2.2, the old 50 N s was 0.7 m/s), and Hit Body Share (80%) of it pushes every part by mass (the whole soldier moves along the shot) while the rest stays on the struck part. The way the body will fall (momentum + shove) picks the forward or backward collapse pose. `NpcRagdoll::Launch` does the shove and the muscles and is what the tests call.
- **Settle.** Once the pelvis is below Down Height (50% of its standing height), or all parts are under Settle Speed after Settle Delay, damping (6 / 8), friction (2.5) and joint friction (30) ramp up over Settle Ramp (0.2 s), unwinding fast if the body is moved again. Parts under Rest Speed for Rest Time are put to sleep (`RagdollSleep`; drive-target writes no longer wake a sleeping ragdoll) and stay so until a corpse shot. Body Grip: Grip Floor max-combines the part's friction and multiplies its restitution (a slick floor can't make a corpse skate, nothing bounces), and the scene has PhysX stabilization on with a per-part Stabilization Threshold. In-game, a corpse is asleep 1.5 s after the kill (`--npc-test deaths` checks it). Not done: a stumble step, CCD (no tunnelling seen on the floor).
- **Measured** (`PoweredRagdollDeathsAgainstPassive` unit test: 12 kills on a floor, 50 N s round, standing and running at 4 m/s; head, chest and leg hits; front, side, back; six seconds each; the old passive death against the new, same shove path). Joint spin = mean joint angular speed in the first second; peak = most any joint turned in 100 ms; fall angle = angle between the pelvis's horizontal travel and the momentum; down = seconds until the pelvis is under 0.3 m; slide = pelvis travel after the pelvis, chest or head first touches the floor.

  | standing / running | old | new |
  |---|---|---|
  | joint spin (rad/s) | 4.81 / 3.25 | 2.50 / 2.55 |
  | peak joint turn per 100 ms (deg) | 101.5 / 107.0 | 65.6 / 68.2 |
  | fall angle off the shot (deg) | 28.6 / 2.9 | 3.9 / 1.0 |
  | pelvis travel (m) | 0.94 / 2.52 | 1.30 / 2.31 |
  | seconds until down | 1.12 / 2.32 | 0.69 / 0.58 |
  | seconds until still (mean) | 2.23 / 1.72 | 1.62 / 1.22 |
  | jitter (mm RMS) | 0.04 / 0.30 | 0.24 / 0.47 |
  | slide after first contact (m) | 0.37 / 0.59 | 0.13 / 0.19 |
- **Lying pose** (Lying group). The target does not stop at the collapse: from Relax Delay (0.3 s) it goes on over Relax Time (0.6 s) to a relaxed lying pose, each joint's way back to its neutral (straight legs, upright spine, arms most of the way, Lying Arms 0.8, to hanging) plus a slight bend (hips 8 deg, knees 10, elbows 25). The legs hold it at Relax Tone (45% strength) while it forms and a Relax Hold (0.5 s) after, then let go; the arms and spine only follow it loosely (held stiff, hanging arms prop a prone body up like a push-up). Relax Tone 0 is the old collapse-only death. A body is never put to sleep before the pose has formed, and when it is about to rest *not lying* (pelvis above Rest Pelvis Max 0.22 m, chest above Rest Chest Max 0.3 m, a knee bent past Rest Knee Max 55 deg or a hip past Rest Hip Max 80 deg: kneeling on a strut of friction, on its toes and hands, sat up) the settle is held to Relax Loose (30%) so the floor lets go of it and the legs push at Rest Fix Tone (90%), for up to Rest Fix Time (1.5 s) after the pose should have formed; after that it is left as it lies (propped against geometry stays propped). Measured over the 12 kills (`RagdollEndsLyingNotKneeling`, revamp -> now): knee at rest mean 25.5 -> 13.3 deg, worst 57 -> 34; hip mean 35.0 -> 15.3; pelvis worst 0.28 -> 0.17 m; knees past 45 deg 4 of 12 -> 0; settle worst 2.57 -> 2.55 s; jitter worst 0.83 -> 0.68 mm. The price is sliding: pelvis slide after first contact 0.06 -> 0.13 m standing and 0.15 -> 0.19 running (the passive death was 0.37 / 0.59), because the floor lets go of a body that is still up.
- **Directional falls.** `NpcRagdollMotor::Fall` reads the way the body goes (momentum + round) in the body's own axes: forward/back and left/right, plus a per-body lean from the ragdoll id that breaks the tie on a straight-on fall. The leg on the side the body falls toward (the far side from the shot) is the lead leg: its knee folds Buckle Asymmetry (0.5) further and the other's less, and its Stagger hold is shortened by Buckle Lead (50%), so it gives out first. A fall back folds the knees at Back Fall Knee Scale (45%) and the hips at Back Fall Hip Scale (150%): the body sits down; a fall forward folds the knees (the old 70 deg). Eight shots round a standing soldier (`RagdollKneesBuckleWithTheShot`, revamp -> now): pose variety half a second in (mean pairwise distance of the knee/hip angles) 38.8 -> 47.5 deg, knee asymmetry at 0.5 s 6.3 -> 15.1 deg, fall angle off the shot mean 2.9 -> 3.3 deg (worst 7.8, limit 35 mean). The lying pose pulls the end poses together (variety at rest 46 -> 22 deg).
- **Reactions** (group Reactions; a head kill has none and its arms and neck let go in Head Kill Limp, 40%, of their tone time). Brace Weight (1): the arms reach toward the fall: forward as before, back (arms thrown behind) on a back fall, and Brace Sideways (30 deg) toward a sideways fall, the arm on that side further. Wound Grab Weight (0.8): on a pelvis or chest hit the hand on the struck side (by where the round entered; centre line: the body's own lean) goes to the wound, Wound Shoulder 50 deg and Wound Elbow 85, up in a tenth of a second, held for Wound Grab Time (0.5 s), then the arm lets go. Head Tuck Weight (1): the head tucks forward as the body falls back (Neck Tuck degrees). In the bare-ragdoll test the hands move 11 cm further toward a sideways fall with the brace on (-0.16 -> -0.05 m from the pelvis at 0.45 s) and the struck-side hand is 6 cm nearer the chest at 0.3 s (0.78 -> 0.72 m; in a rifle-hold death pose the hand starts much nearer).
- **Corpse hits.** `NpcRagdoll::HitCorpse` (the director's corpse shot): a point impulse at the struck ragdoll part (Corpse Shot Base + Per Damage in m/s, capped at Corpse Shot Max Speed 4, times the part's mass), then `NpcRagdollMotor::Poke`: the body wakes, the settle is held off for Corpse Wake Time (0.45 s) with the muscles at their residual, so the limb the round struck flops, and it then settles and sleeps again (`RagdollCorpseHitReacts`: the struck forearm moves 14.5 -> 20.9 cm in 0.3 s with the loose time, the far hand 2 cm, the pelvis drifts 0.5 cm, asleep again after 1.1 s). The shot already plays the flesh-hit sound; the engine has no blood or body-impact decal / particle path (only bullet holes and laser spots in `WeaponFxRenderer`), so there is nothing more to trigger without a new renderer feature.
- **Per-region drive fade** (Powered Ragdoll off only). Drive Fade times a scale per region (Pelvis ... Calf Fade Scale, default 1 = the single 0.25 s fade); e.g. legs 0.6, spine 1.5 makes the legs give out first. Neck, Hand and Foot have scales too.
- **Ragdoll vs hitbox tables.** The hitboxes are exactly the old eleven capsules (`NpcPartDefOf`, indices 0-10: the hitbox ray test still names the same bones); the ragdoll has its own 16-part table (`NpcRagdollDefOf`) with the same eleven at the same indices, so a hit part names the same body part in both, then neck_01 (11), hand_l / hand_r (12, 13), foot_l / foot_r (14, 15). Forearms and calves now stop at the wrist and ankle (the hand and foot capsules carry the rest), and the head hangs off the neck (`CreateRagdoll` accepts any parent index, not only an earlier one). A hand reaches to `middle_01`, a foot to `ball`; a rig without those bones gets a stub along the forearm / forward from the ankle. Masses add to ~75 kg (pelvis 15, spine 19, neck 1, head 4.5, upper arm 2.5, forearm 1.6, hand 0.5, thigh 8, calf 4, foot 1, each side for limbs). Written back to the skin: every part's bones follow their capsule; spine_01 / spine_02 are blended between pelvis and chest by where the death pose had them, and the clavicles ride the chest, so nothing between two parts stretches. Velocity inheritance covers all 16 (the snapshot also holds the optional finger and toe bones).
- **Hit flinch.** A round that does not kill kicks the struck region's bones (`NpcFlinch`): a rotation about each bone's own joint, axis = bone direction x round direction (its far end goes where the round did), settling back through a critically damped spring within Flinch Duration (0.3 s; the peak is at a fifth of that). Peak = Flinch Angle (7 deg) x the region's scale (pelvis 0.7, chest 0.8, thigh 1.2, head 1.3, upper arm and calf 1.4, forearm 1.5) x the bone's share x damage / Flinch Damage Reference (40; clamped 0.3-2), rounds in a burst add up to Flinch Max Angle (28 deg). It is added after the late pose and after the hitboxes took theirs (aim, eye and hitboxes never see it), on top of the animator's own Hit reaction, only on frames where the pose was refreshed (a held LOD pose already carries the last one), and dropped when the soldier dies. Hit Flinch off disables it.
- **Joint limits.** The ragdoll's 16 capsules are joined by D6 joints with anatomical ranges (table in `NpcRagdoll.cpp`:
  AAOS / Kapandji active ROM, trimmed for one part standing for several joints). Elbows and knees are one-way hinges:
  flexion 145 / 140 degrees, no hyperextension, a few degrees of sideways slack and twist. Shoulders, hips, spine and
  head are asymmetric pyramids (flexion, extension, adduction, abduction) with separate inward and outward twist. The
  ranges are measured from a neutral pose (limb hanging, spine upright), so the death pose (a stride, an A pose) is
  just somewhere inside them, and a pose outside widens the range to hold it. Which way a hinge folds comes from the
  pose it died in when it was bent, else forward (elbow) / back (knee). `AnatomicalLimits` off restores the old
  symmetric cones. The cervical range is shared: neck 30 / 35 flexion / extension, head on neck 25 / 30 (they were 50 / 60
  for the head alone). Wrist: 75 palm side, 70 back, 25 / 25 sideways, 15 twist, neutral = the forearm straight on. Ankle:
  dorsiflexion 20, plantarflexion 50, toes in / out 15, inversion 35 / eversion 15, neutral = level, facing forward. The
  hands and feet are light end links that whip the forearm / calf through its limit (elbow overshoot 19.6 degrees in the 3 m/s per kg
  shove test): their inertia is multiplied by Distal Inertia Scale (6) and their joints carry Distal Joint Damping (40, always on,
  also after the drives fade), which brings the elbow to 4.4 and the knee to 3.0 degrees while the wrist still folds 64 and the
  ankle 75 degrees under the same shove. (Heavier hands were worse, more solver iterations did nothing.)

## Performance

Measured as the sum of AI and late-pose CPU time, Release, `--npc-test`, 4 soldiers alive:

| Scenario | ms per frame |
|---|---|
| watch | 0.73 |
| fight | 0.59 |
| tactics | 0.75 |

The goal was ≤ 1 ms for 5. What it relies on:

- **One skeleton solved.** The torso piece is sampled and solved (aim, arms, neck weld). The other outfit pieces copy its local pose (`CopyDriverPose`) and its solved rotations (`SyncPieces`).
- **Animation LOD:**
  - near and in view: every frame
  - beyond 25 m: every 2nd frame
  - beyond 50 m or out of view: every 4th frame
  - always full rate while firing, just hurt, or just spawned
- **Foot IK only where it shows.** For soldiers within 25 m and in view, feet go onto slopes, ramps and steps: a ground ray under each foot, the pelvis dropped to the lower foot, and both legs re-solved with the planted foot tilted to its slope. It's solved on the driver, and the pelvis and legs are copied to the other pieces. On flat ground it costs only the two rays.
- **Mesh clearance only up close.** The gun / elbow / cheek checks against the drawn surfaces run within 12 m, in view, on every third frame. Their skin tables are shared per model and built at spawn. Further out, the sphere keep-outs do the job.
- **No name lookups per frame in the hold.** Finger, arm and chest bones and the arm-shape node pairs are found once per weapon rig.
- **One cover search per frame across all squads.** A second soldier wanting one waits a frame.
- **Ragdolls:**
  - skipped once asleep
  - read from interpolated poses
- **Spawns.** Clip-to-skeleton matches are shared between model instances (`Model::AttachClip`), and clip files are stat'ed once. A soldier's gun reuses the first one's parsed definition, bolt stroke, barrel and ADS carry (the player's own gun still measures). A gone corpse's entity tree is kept, hidden, for the next spawn (with its animators as built), and two spares are built at the start; a respawn costs about 0.8 ms (body 0.4, gun 0.4). The first soldier with each gun pays the full ~5 ms.

The profiler shows the costs as AI Perceive / Brain / Squads / Move / Aim+Fire / Body / Weapon / Hold / Ragdoll /
Hitbox. `--npc-test` prints them at the end.

## Tuning

- **Per soldier:** NPC Spawn Skill.
- **Per scene:** Squad Settings Difficulty (also on the F7 slider, 0.25-3). Difficulty affects:
  - reaction time
  - accuracy
  - attack tokens
  - melee damage
- **Numbers in code:**
  - Squad Settings / Ragdoll Settings: the combat, cover and ragdoll numbers above are inspector fields now, not constants.
  - `AiMath.cpp`: perception, accuracy and the tactics thresholds.
  - `NpcBrain.cpp`: behaviour scores.
- **Voice lines:** `tools/gen_combine_voice.py` renders them with Windows TTS and a radio chain into `project/assets/Audio/Voice/combine/`. Each event's lines, subtitle, priority and cooldown are in `manifest.json` there.

## Tests

- `--unit-tests`:
  - `AiMath`: perception, memory, accuracy, tactics
  - `SquadVoice`
  - `NpcHitRegions`
  - `NpcBodyParts`
  - `PoweredRagdollDeathsAgainstPassive` (the table above, asserted), `RagdollMuscleToneCurves`, `RagdollMusclesFoldTheKnees`, `RagdollSettlesAndSleeps`, `RagdollPoweredSettings`, `RagdollEndsLyingNotKneeling`, `RagdollKneesBuckleWithTheShot`, `RagdollReactionsLayerTheTarget`, `RagdollCorpseHitReacts`, `RagdollLyingFieldsRoundTrip`
- `--npc-test <scenario>` (Arena unless noted). Each prints its checks, tactic counts and costs; with `--smoke-shots <dir>` it saves screenshots.

| Scenario | What it does |
|---|---|
| watch | The squad notices the player, takes cover and shoots. |
| fight | The player shoots back. Losses and respawns. |
| die | The player is killed and respawns. |
| deaths | Rays name every bone. Then:<br>• head, chest, thigh and forearm kills<br>• limp and stagger<br>• no pose pop into the ragdoll<br>• a shot corpse<br>• a powered corpse is asleep within 7 s and lies (no knee past 60 deg)<br>• wounded crawl and bleed-out |
| feet | Foot IK: each foot's height over the ground on the flat, then standing across and up the ramp. Both stay within 4 cm of the flat's. |
| pose | The weapon hold close up: aim, hip, crouch, strafe, reload, sprint, signal. Checks clearances. |
| reload | An AK and a Remington soldier aimed, at the hip, reloading at each, an empty reload and the idle regrip. Logs the butt against the right shoulder and the eye off it as `--stock-probe ak` logs the player's body (the two should agree), checks the hands stay on the gun. |
| tactics | Pins a soldier: it blind-fires. Checks that bounds go under covering fire. Then steps up to a soldier and takes a rifle butt. |
| sandbox | The Sandbox squad spawns, uses the radio and dies to Kill All. |

`--weapon-test` and `--stock-probe` run without the enemy squad.

## First Play frame

On Play the director builds its navigation mesh (from `Library/NavCache` after the first Play), the cover points and the squad on the first frame: Sandbox 96 ms down to 27 ms. The open-edge merge in `NavMesh::BoundaryEdges` restarted a cubic search after every merge (~70 ms of it); it now keeps per-edge candidate lists found through a hash of edge starts and gives the same edges (6700 edges: 15 ms, `NavBoundaryEdgesMergeQuickly`). Cover probing skips the head and peek rays when the knee ray finds nothing (they could not change the result) and de-duplicates through its grid. What is left is the four soldiers and two spare bodies (~20 ms, first use of the rifle and shotgun rigs). Respawns and deaths show nothing above 2.3 ms in `--npc-test deaths`.

## The dropped weapon

When a soldier dies the gun leaves his hands (`Npc/NpcDroppedWeapon`): a copy of the weapon model on its own dynamic body (a box from the model bounds, 3.5 kg, friction 0.7, bounciness 0.1), thrown at the gun's velocity as `NpcBody::TrackGun` saw it each frame plus a capped share of the killing round's impulse, with some tumble. For `Collision Delay` (0.1 s) it flies without colliding, so it clears the falling body's arms and torso instead of being thrown off them; then the body is built, spun, and sleeps once at rest. `NpcBody::ReleaseWeaponHold` ends the arm solve for good. The gun lies as long as the corpse (or `Lifetime`). Tunables: the **Dropped Weapon Settings** component (Drop / Throw / Body Physics groups; the first in the scene counts, the defaults without one; `Enabled` off = the gun vanishes as it used to). Outfit pieces have no secondary motion (no cloth, jiggle or spring bones; they are skinned to the body's skeleton), so a hood or loose gear does not swing; nothing was built for it. Tests: `DroppedWeapon*` in `UnitTests_Ragdoll.cpp`.
