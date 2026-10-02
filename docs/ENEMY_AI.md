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
  defaults below apply. Groups: Joints (anatomical limits on/off), Death Drive (stiffness, damping, fade), Body Physics
  (damping, solver iterations, depenetration, sleep threshold), Contact (friction, restitution), Impulse Caps (part and
  chest shove speed, corpse-shot shove), then per region (Pelvis, Spine, Head, Upper Arm, Forearm, Thigh, Calf; left and
  right share a value) a mass and a range of motion. Hitbox geometry (radius, length) is not exposed: it changes
  gameplay.

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
- **Per-region drive fade.** Drive Fade times a scale per region (Pelvis ... Calf Fade Scale, default 1 = the single 0.25 s fade); e.g. legs 0.6, spine 1.5 makes the legs give out first. Hands, feet and neck are not separate parts yet (a part in `kParts` is also a hitbox; the two tables would have to be split first).
- **Joint limits.** The ragdoll's 11 capsules are joined by D6 joints with anatomical ranges (table in `NpcRagdoll.cpp`:
  AAOS / Kapandji active ROM, trimmed for one part standing for several joints). Elbows and knees are one-way hinges:
  flexion 145 / 140 degrees, no hyperextension, a few degrees of sideways slack and twist. Shoulders, hips, spine and
  head are asymmetric pyramids (flexion, extension, adduction, abduction) with separate inward and outward twist. The
  ranges are measured from a neutral pose (limb hanging, spine upright), so the death pose (a stride, an A pose) is
  just somewhere inside them, and a pose outside widens the range to hold it. Which way a hinge folds comes from the
  pose it died in when it was bent, else forward (elbow) / back (knee). `AnatomicalLimits` off restores the old
  symmetric cones.

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
- `--npc-test <scenario>` (Arena unless noted). Each prints its checks, tactic counts and costs; with `--smoke-shots <dir>` it saves screenshots.

| Scenario | What it does |
|---|---|
| watch | The squad notices the player, takes cover and shoots. |
| fight | The player shoots back. Losses and respawns. |
| die | The player is killed and respawns. |
| deaths | Rays name every bone. Then:<br>• head, chest, thigh and forearm kills<br>• limp and stagger<br>• no pose pop into the ragdoll<br>• a shot corpse<br>• wounded crawl and bleed-out |
| feet | Foot IK: each foot's height over the ground on the flat, then standing across and up the ramp. Both stay within 4 cm of the flat's. |
| pose | The weapon hold close up: aim, hip, crouch, strafe, reload, sprint, signal. Checks clearances. |
| reload | An AK and a Remington soldier aimed, at the hip, reloading at each, an empty reload and the idle regrip. Logs the butt against the right shoulder and the eye off it as `--stock-probe ak` logs the player's body (the two should agree), checks the hands stay on the gun. |
| tactics | Pins a soldier: it blind-fires. Checks that bounds go under covering fire. Then steps up to a soldier and takes a rifle butt. |
| sandbox | The Sandbox squad spawns, uses the radio and dies to Kill All. |

`--weapon-test` and `--stock-probe` run without the enemy squad.
