# Weapon and foley audio

Sound design for the two first-person guns (AKS-74U, Remington 870) and the player foley. Everything under
`project/assets/Audio/Weapons` and `project/assets/Audio/Foley` is **generated** from third-party source packs by the
scripts in `tools/audio/`; the raw source audio is never committed. Style target: cinematic heavy (hard transient,
sub-bass punch, long environmental tail, mechanical layer), every sound with variants, every action sound cut so it
can be fired from an animator event on the contact frame of our own clip.

## Contract with the engine (lane S2)

| Item | Where | Meaning |
|---|---|---|
| `audio_manifest.json` | `project/assets/Audio/` | one entry per wav: `file` (relative to `project/assets/Audio`), `key`, `layer`, `variant`, `anchor_ms`, `lufs_m_max`, `true_peak_dbtp`, `length_s`, `channels`, `source`, optional `lead_ms`, `mix_db`, `loop`. |
| `sync_map.json` | `tools/audio/` | `"<gun>/<Clip>": [{"key","frame","time","src","conf"}]` for every FP clip of both guns. |
| keys | | weapon `snd.<ak|870>.<element>`, foley `snd.foley.<category>.<element>`; variants of one key share it, pick randomly and never the previous take. |

* **frame** = the clip frame (0-based, 60 fps; Blender frame - 1) of the *contact* (press, impact, grab). `time` =
  `frame / clip length in frames` (`export_manifest.json` `frameEnd`).
* **anchor_ms** = where the contact transient sits inside the file. Start playback at
  `frame / 60 - anchor_ms / 1000` (clamp at the clip start). It is ~0-5 ms for clicks and impacts, 28-130 ms for pump /
  bolt strokes (slide lead-in kept in the file), ~300-360 ms for draw / holster (cloth lead-in, thud on arrival).
  `anchor_ms = 0` with no transient (cloth, melee swing, foley handling, loops) means "starts with the motion".
* `snd.<gun>.fire` in `sync_map.json` is not a file: it expands to the five layers `snd.<gun>.fire_close|sub|mech|tail|far`
  (all start at the shot instant; tails and far carry their own leading silence / delay). Play each layer at its
  manifest `mix_db` (close 0, sub -2, mech -3, tail -4, far -7) into a weapon bus with a limiter.
* **Full auto**: close/sub/mech on every shot, **tail every 2nd shot, far every 3rd** (the audition reel does exactly
  this). Stacking a 3 s tail per shot at 650 rpm sums to +10 dB and buries the transients. Even so ten AK shots sum to
  about +4 dBFS before the bus limiter, so the bus limiter (-1 dBTP) is required.
* Far layers are mono (play them as a 3D / distance source); everything else is stereo 1P material.
* Foley keys: `snd.foley.weapon.{ads_in,ads_out,equip,unequip,firemode}`, `snd.foley.move.{jump,land_light,land_heavy,sprint_loop}`
  (`sprint_loop` is seamless, `loop: true`), `snd.foley.step_<carpet|concrete|glass|metal|water|wood>.{walk,run,land}`.
* Weapon elements: AK `mag_release mag_out mag_in mag_tap bolt_back bolt_release handle cloth draw holster firemode
  dry_fire melee_swing melee_hit`; 870 `pump_back pump_fwd shell_insert shell_load_chamber handle cloth draw holster
  dry_fire melee_swing melee_hit`. Fire layers `fire_close fire_sub fire_mech fire_tail fire_far`. `dry_fire` has no
  animation clip (engine fires it on an empty trigger pull).

### Sync frames (60 fps, 0-based contact frames; `conf` high = part motion, medium = hand motion)

| Clip (length) | Events |
|---|---|
| ak/Tac_Reload (170) | mag_release 48, mag_out 61, mag_in 92 (seat), mag_tap 103; cloth 5/115, handle 19/28/131/149 |
| ak/Empty_Reload (220) | mag_release 24, mag_out 37, mag_in 102 (seat), bolt_back 146 (rear stop), bolt_release 156 (slam); cloth 5/114, handle 44/74/107/127/188 |
| ak/Mag_Check (260) | mag_release 24, mag_out 39, mag_in 196; cloth 5/29/210, handle 17/46 |
| ak/Inspect (282) | bolt_back 173 (eased pull), bolt_release 212 (slam); cloth 5/114/223, handle 31/148/163/249 |
| ak/Draw (46) / Holster (28) | draw 24 / holster 27 |
| ak/Fire (36) | fire 0 |
| ak/Melee (65) | melee_swing 0, melee_hit 6, cloth 23 |
| ak/Regrip (40) | cloth 11, handle 24/40 |
| 870/Pump (44) | pump_back 8 (rear stop), pump_fwd 18 (slam) |
| 870/Reload_Start_Empty (160) | pump_back 10, shell_load_chamber 54, pump_fwd 118; cloth 21/130, handle 31/45/97/152 |
| 870/Reload_Loop (70), Reload_Loop_End (98) | shell_insert 11 and 18 (port contact, then the soft latch click); cloth 1/54, handle 44-46/67/79 |
| 870/Reload_Start (30), Reload_End (50) | cloth 5 / 3, handle 29 / 27 |
| 870/Mag_Check (144) | pump_back 37, pump_fwd 78; cloth 6/32/73/88, handle 125 |
| 870/Inspect (300) | pump_back 193, pump_fwd 234; handle 31/188/254/281, cloth 171/228/244 |
| 870/Draw (46) / Holster (28) / Fire (42) / Melee (67) / Regrip (38) | draw 26 / holster 27 / fire 0 / swing 0, hit 8, cloth 17 / cloth 1, handle 24/37 |

Sequence order is asserted by the analyzer (release < out < in < bolt back < bolt release; pump back < shell < pump
forward). Hand-derived events (`conf: medium`) are placed on hand-motion starts/stops; they are good to about +-2
frames and are the first thing to tune by ear.

## Sources (never committed)

Extracted by `tools/audio/extract.py` (Sonniss GDC bundles are placed under `C:\tb\audio-src\sonniss\<pack>` by hand; recipes
address them as `sonniss/<pack prefix>/<file prefix>`) to `C:\tb\audio-src` (`C:\Program Files\WinRAR\UnRAR.exe` for the .rar, `zipfile`
for the .zip), from `C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT\`:

* `Tactical Shooter Pack.rar` (48 kHz stereo 16-bit): AK105 (fire + actions), Herrington 11-87 (fire + actions), Mk14EBR,
  TR15, WK-11 Viper, SRM-12, R08 (donors), General (aim, equip, fire mode, jump, land, concrete steps).
* `Boots.zip`: footsteps on Carpet / Concrete / Glass / Metal / Water / Wood (single steps, stomps, 30 s walk / run
  sequences).

The pack's action sounds were made for different, longer animations, so they are never played whole: every element is a
slice cut at the real contact transient (`recipes/elements.json`).

## Rebuild

```
uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python <script>   # from tools/audio
python tools/audio/extract.py                       # once: raw sources -> C:\tb\audio-src
blender.exe -b --python tools/audio/clip_contacts.py -- dump C:\tb\audio-work\motion     # headless Blender, reads the FBX only
uv run --with numpy python tools/audio/clip_contacts.py analyze C:\tb\audio-work\motion tools/audio/sync_map.json
build_elements.py   # weapon action one-shots   (recipes/elements.json)
build_fire.py       # five gunfire layers       (recipes/ak_fire.json, recipes/remington870_fire.json)
build_foley.py      # foley + footsteps + loops (recipes/foley.json)
check_audio.py      # acceptance checks, exit code 0 = pass
render_reel.py      # audition reels to C:\tb\audio-reel (not committed)
```

Builds are deterministic (seeded by recipe content, fixed dither seeds). After changing a recipe rebuild the affected
script, run `check_audio.py`, then listen to the reel.

## Method

* **Contact analysis** (`clip_contacts.py`): headless Blender imports each FP / Weapon FBX into an empty scene and
  samples the bone heads per frame (world and gun-root-relative). Magazine, mag release, bolt, pump, shell and the hands
  give speed profiles; a stop is an impact, a start is a press/grab. Mag-out = magazine 4 units clear of its seat, mag-in =
  first frame a magazine sits back on its seat position; shells: first frame after the peak below 40 % speed.
* **Slicing** (`adsp.slice_at`): onset snapped to the real transient (first crossing of 22 % of the local high-passed
  peak, +-12 ms), 3 ms pre-roll (or `lead_ms`), 1.5 ms cosine fade-in, natural cosine fade-out; `dur` is chosen from the
  source onset map so a slice never swallows the next event of the pack's animation.
* **Gunfire layers** (`build_fire.py`): close = pack report (AK105 / Herrington) + crack donor (TR15, high-passed
  2-2.5 kHz) + body donor (Mk14 / SRM-12, low-passed 650-1000 Hz), donors phase-aligned to the base (best lag and polarity
  in the band they contribute, +-1.2 ms), then HPF, EQ, transient shaper, compressor, tanh saturation, limiter. Sub =
  synthesised exponential down-sweep sine + low click, saturated for harmonics, polarity-matched to the close layer.
  Mech = bolt / forend slam from the action sources, delayed 12-21 ms, high-passed (AK) or pitched down 6 st (870
  receiver thump). Tail = the source from +0.12/0.16 s with a 100 ms fade-in (body stays in `close`), upward-compressed
  so the room decay is audible. Far = mono, low-passed 2.2-2.4 kHz (24 dB/oct), reverb, delayed 70-120 ms.
* **Variant counts**: close 6, sub 5, mech 4, tail 5, far 4 per gun; action elements 3-4; foley 3-4; footsteps 5-8.
* **Dither**: every file is written 16-bit with TPDF dither.

## Loudness targets (`recipes/targets.json`, enforced by `check_audio.py`)

Metric: **LUFS-M max** = loudest 400 ms momentary window (BS.1770 K-weighted). A one-shot is too short for an integrated
or 3 s short-term reading, so this is the stable number; `lufs_i` is stored for reference only. All layers: true peak
<= -1 dBTP (4x oversampled), no clipped samples, DC < 0.002, first/last sample < 0.004 (loops: seam step <= 4x the
loop's mean sample step).

| Layer | Target | Window | | Layer | Target | Window |
|---|---|---|---|---|---|---|
| close | -14 | -16.5..-12 | | action | -21 | -26..-18 |
| mech | -20 | -23..-17 | | foley | -21 | -24..-18 |
| sub | -19 | -23..-15 | | step (walk -24, run -22) | -23 | -28..-20 |
| tail | -22 | -26..-18 | | loop | -24 | -27..-21 |
| far | -25 | -29..-21 | | | | |

A limiter-aware loop (`abuild.finish`) measures what is actually shipped: gain to target, limiter only if the peak
needs it, at most 7-10 dB of limiter work, so spiky one-shots stop at their peak ceiling rather than turn to mush
(that is why a few click-only elements and soft water steps sit near the bottom of their window).

## No synthesis (hard rule) and provenance

Every sound is a recording, processed with tools: slicing, layering, EQ, filtering, compression, saturation, limiting,
pitch / time (varispeed), and convolution with real recorded IRs only. **Nothing is generated**: no oscillators, noise,
synthetic envelopes or impulse responses, no algorithmic reverb, not even as a sweetener, and no dither (16-bit output
is plain rounding).

* Every manifest entry has a `sources` list: `[{file, start, end}]` (path relative to `C:\tb\audio-src`, seconds) for every
  cut that went into that file. It is recorded automatically (`adsp.cut` is the only way a build script slices a source).
* `check_audio.py` FAILS if an output has no `sources` or a source file is missing, and statically scans every
  `tools/audio/*.py` (except the checker) for generator calls (`np.sin/cos`, scipy signal generators, `standard_normal` /
  `.random()` / `np.random.*`, `exp(-t ...)` decays, pedalboard `Reverb` / `Chorus` / ...) and for `np.random` outside
  `adsp.deterministic_rng` (selection / jitter only, never audio).
* `proxy: true` in the manifest = a real recording that is not a recording of exactly that thing (e.g. a small casing
  pitched down as a shotgun hull on carpet). See coverage below.

### Round 4 (S4) replacements: old source -> new source

| Element | Was | Now |
|---|---|---|
| `fire_sub` (both guns) | sine sweep + noise click | low-passed real shot (4th order, 100-130 Hz) + the same shot pitched down 10-14 st and low-passed again: AK105, Mk14, HK416 (Audiobeast), AK47 blanks (Pole Position) / Herrington, SRM-12, TS 12 ga, Audiobeast 870 x2 |
| `melee_swing` | band-swept noise | David Dumais swings (normal / high / low / designed) + Gamemaster knife whoosh, cut so the whoosh peak lands 70-100 ms in (the hit follows 6 / 8 frames later); 870 pitched down 1.5-4 st |
| `melee_hit` | sine thump + noise burst + slap | layered real strikes: Chris Alan deep punch / body slam, Shapeforms heavy punch + low thud, Gamemaster body thump / wet punch, plus the gun's own handling rattle 12-20 ms later (AK pack mag / handle taps; 870 Herrington handling, TS / Wav Junction pump, SAIGA) |
| `dry_fire` AK | synthetic click | Pole Position Steyr TMP dry-fire clicks (4 takes, pitch variants) |
| `dry_fire` 870 | synthetic click | Wav Junction safety clicks pitched down + Steyr clicks pitched down 3-4 st |
| 870 `pump_back` / `pump_fwd` | SRM-12 bolt pitched down | TS Quick Pump x2, Wav Junction pump, SAIGA-12 cocking x2 (low end +4 dB at 130 Hz) |
| 870 `shell_insert` / `shell_load_chamber` | Herrington only | TS "Shell Load" (3 contacts) + Herrington |
| 870 `draw` / `holster` | Herrington cloth + SRM-12 / TR15 / Viper thud | Shapeforms / Herrington cloth + real SAIGA-12 handling hits |
| Foley `land_heavy` | sine thump under the landing | Shapeforms low thud under the pack landing |
| Foley `sprint_loop` | filtered noise bed | real cloth slices (AK / Shapeforms / Herrington) + gear rattle, circular placement |
| `fire_far` | algorithmic reverb | no reverb: the real recorded tail, low-passed 2.2-2.4 kHz and compressed |
| all wavs | TPDF dither | none |

Every `anchor_ms` of a sliced element is now the *designed* contact lead (`lead_ms` / pitch ratio), not a detection
(draw / holster = the thud position, melee / dry fire / cloth = 0), so a controller sync stays on the contact frame.

### New sets: casings, impacts, flyby (`build_impacts.py`, `recipes/casings_impacts.json`)

Keys `snd.casing.<rifle|shell>.<concrete|wood|tile|carpet|metal|dirt>`, `snd.impact.<concrete|metal|wood|dirt|glass|flesh|ice>`,
`snd.flyby`; files `Audio/Casings/<kind>/<material>_<n>.wav`, `Audio/Impacts/<material>_<n>.wav`, `Audio/Impacts/flyby_<n>.wav`.
Casing long takes are cut into their individual fresh drops (onset after 0.3 s of near silence): "bounce" slices run to the
natural end of the roll (<= 1.6 s), "short" slices are the first 0.28 s; `short: true` in the manifest.

| Set | Variants | Real material |
|---|---|---|
| casing rifle concrete / wood / tile / carpet | 10 / 9 / 8 / 10 | SculpTunes (9 mm concrete, 6.35 wood, 7.62x39 tile, carpet) + Duffield wood drop |
| casing rifle dirt | 6 | **proxy**: one Duffield leaves bullet drop, pitch / EQ variants |
| casing rifle metal | 6 | **proxy**: Gamemaster heavy metal impact pitched up 5-9.5 st (no real casing-on-metal recording) |
| casing shell concrete | 8 | 3 real (Duffield shotgun shell, 2 pitch variants) + 5 **proxy** (SculpTunes -5 st) |
| casing shell tile | 8 | all **proxy** (Duffield lino shell / SculpTunes tile -5 st) |
| casing shell wood / carpet / dirt / metal | 7 / 8 / 6 / 6 | all **proxy** (pitched-down casings, bullet drops, metal ring) |
| impact concrete, metal, dirt, glass, ice | 5 / 5 / 5 / 5 / 4 | one real file each; variants = pitch +-1.5..3 st / EQ / shorter cut |
| impact flesh | 7 | PMSFX flesh, Gamemaster body thump + wet punch (3 real files) |
| impact wood | 5 | **proxy**: pitched-down Duffield wood drop + Shapeforms thud (no real bullet-in-wood) |
| impact water | - | none: no real material in the bundles |
| flyby | 6 | Gamemaster flyby fast 05 / 06 + pitch variants |

Loudness (LUFS-M max): casing -26 (window -32..-20), impact -17 (-22..-12), flyby -21 (-26..-16).

## Known weak spots in the source material

* Many onsets in the Tactical Shooter Pack cluster 30-50 ms apart; slice boundaries were set from onset maps, not by ear.
  Listen to the reels and re-time `t` / `dur` in `recipes/elements.json` if a take includes a neighbour.
* 870 pump strokes from TS / Wav Junction / SAIGA are quick cocks (rear and front contacts 80-200 ms apart); the rear-stop
  slices are short (80-90 ms of body) so they do not run into the forward stroke.
* Water walk has only 5 steps; there is no real water impact, shell-on-metal, shell-on-wood, shell-on-carpet or bullet-in-wood.
* `conf: medium` sync events (hands) are heuristic.
* Pole Position indoor / outdoor and the Audiobeast room recordings are reserved for the tail lane; only their direct
  (first ~0.4 s) body is used here, for `fire_sub`.

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
