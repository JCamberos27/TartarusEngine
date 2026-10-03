# Audio

(Lane S1 writes the asset side - recording, the manifest, the sync map. This file's "Engine" section is lane S2's.)

## Engine

Weapon and foley audio is data. Code lives in `src/Game/Audio/` (`WeaponAudio`, `FoleyAudio`); `CombatFx` starts and
updates it. Tests: `src/Tests/UnitTests_Audio.cpp`, and the audio section at the end of `--weapon-test`.

### Start-up

`CombatFx::Start` (Play mode) starts `WeaponAudio` and `FoleyAudio`. main.cpp starts `CombatFx` whenever Play has a
first-person player, with or without an enemy squad, so the player's gun is audible in scenes with no NPC Spawns and
under `--weapon-test` / `--stock-probe`.

### SoundSet

Everything playable is a `SoundSet`: files (variants), volume, volume jitter (dB), pitch min / max, bus, 3D min / max
distance and rolloff, no-immediate-repeat round robin, max voices (the oldest is stolen, faded out over Steal Fade
Time), loop. Sets are plain JSON (`SoundSet::FromJson` / `ToJson`):
`{"files": [...], "volume": 1, "volumeJitterDb": 0, "pitchMin": 1, "pitchMax": 1, "bus": "sfx", "minDistance": 2,
"maxDistance": 40, "rolloff": "log", "noImmediateRepeat": true, "maxVoices": 8, "stealFadeTime": 0.03, "loop": false}`.

A set with no files of its own is filled by key: first from `assets/Audio/audio_manifest.json` (every file with that
key = the variants), then from the file layout (`Audio/Weapons/<AKS74U|Remington870>/<element>_<n>.wav`,
`Audio/Foley/<category>/<element>_<n>.wav`). Dropping in a recorded take never needs a code or data change.
Gun shot layers are manifest entries with key `snd.<gun>.shot` and layer `close | mech | sub | tail | far` (or keys
`snd.<gun>.shot_<layer>`).

### Keys

- `snd.<gun>.<element>`: `ak` = AKS74U, `870` = Remington870. The element names the controllers use are in
  `tools/audio/apply_sync_map.py` (`mag_out`, `mag_in`, `pump_back`, `shell_insert`, ...). Gear sounds the presentation
  makes itself: `ads_in`, `ads_out`, `fire_mode`, `dry_fire`, `equip`, `unequip`.
- `snd.foley.<category>.<element>`: footsteps are `snd.foley.<surface>.walk | run | crouch | jump | land`;
  `snd.foley.cloth.sprint_loop` is the sprint gear loop. A foley key on a weapon animation event plays the same way.

### Animator events

Any animator event whose name starts with `snd.` plays that key's set at the weapon: 2D for the player's gun, 3D at the
world gun for a soldier's (`FirstPersonPresentation`: `m_Options.OwnerView`). The existing `Shot` / `Refill` /
`LoadRound` / `Eject` events are untouched. The Fire state carries no `snd.` event: the shot layers play from the round
itself, so hip and ADS fire sound the same.

`tools/audio/apply_sync_map.py` writes the events into the two `.controller` files:
`--placeholders` (guessed normalized times), no flag (`tools/audio/sync_map.json`, `{"<gun>/<StateClip>":
[{"key", "frame", "time"}]}`, `time` normalized 0..1 in the state's clip), `--check` (CI-style, exit 1 on drift). Only `snd.*`
events are replaced; run it again to re-sync.

### Shots

`WeaponAudio::Shot(gun, pos, at2D)` plays the gun's layers: `close` (the crack), `mech` (the action), `sub`, `tail`
(room / field decay), `far` (the distant report). The player's 2D shot plays every layer with `Player2D` (not `far`) at
Player Gain. A 3D shot weights each layer by its distance curve (`BlendCurve`: near / far distance and weight, linear
between): the defaults keep the old behaviour, the crack full to 18 m and gone by 43 m, the far report 0.1 up close
rising to 1. One pitch per shot is shared by the layers.

Full auto: close / mech / sub are capped by their set's Max Voices (steal oldest); the tail has its own cap
(Tail Max Voices, 3), the stolen tail fades over Tail Fade Time, a tail never starts within Tail Min Interval of the
last, and each tail still ringing ducks a new one by Tail Duck Per Voice, so 600+ rpm neither piles up nor clips.

### Tuning

- **Weapon Audio** component (one per gun; `Gun` = `ak` / `870`; a gun without one uses the built-in defaults): volume,
  player gain, shot pitch, jitter, the distance blend, voice caps, the tail, and a `Data File`: JSON of per-layer and per-event
  `SoundSet` overrides: `{"layers": {"close": {...set..., "curve": {"nearDistance", "farDistance", "nearWeight",
  "farWeight"}, "player2d": true}}, "events": {"mag_out": {...set...}}, "tailMinInterval": 0, "tailDuckPerVoice": 0.3}`.
- **Foley Audio** component (one per scene): per-gait volume, jitter, pitch, stride scale, jump / land volume and the
  fall-speed range of a landing, the cloth loop, NPC step range, the default surface and the Surface Table
  (`surface=word,word;...`, matched against the ground's collider material path, tag and name; the surface is the foley category).

### Foley

Player footsteps come from distance travelled, one every half of the view bob's stride (Bob Walk / Sprint Stride of the
weapon; the bob itself is untouched), so cadence follows speed exactly. Walk, run (sprinting, or over Run Speed) and crouch
sets; the surface is a downward ray's collider. A set with no files for a surface falls back to the default surface's. Landing is
scaled by the fall speed (silent below Land Min Speed); the sprint cloth loop's gain follows planar speed. Soldiers'
footsteps use the same stride rule per soldier, 3D, skipped beyond NPC Step Max Distance
(`FoleyAudio::NpcWalk`, called from `NpcDirector::AimAndFire`).

### Audio log

`WEAPON_TEST_AUDIO_LOG=<file>` (or `NPC_TEST_RECORD=<dir>`, writing `<dir>/audio.txt`) logs, with the existing `S` / `L` lines:
`W <sim time> <key> <file|-> <voices> <volume> <pitch> <2d> <x y z>` for every weapon and foley voice (`-` = the key has no
recorded file yet). `tools/mix_npc_video.py` reads only the `S` lines (Combat folder placeholders): it needs `W` support to
mix the new layers.

### Tests

Unit: round robin never repeats, steal-oldest (and the fade), jitter / pitch ranges, JSON round trips, distance blend
weights, the autofire voice cap, `snd.` event to set and 2D / 3D, manifest and layout loaders, footstep cadence vs speed,
surfaces, landing and cloth-loop rules. `--weapon-test` ends by replaying every frame's animator state against the
controllers' `snd.*` events: each crossed event must have been played, in order, within +-1 frame, plus counts for the
gear sounds (ADS, fire mode, dry fire, equip / unequip) and the shot layers.
