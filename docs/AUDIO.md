# Weapon and foley audio

Sound design for the two first-person guns (AKS-74U, Remington 870) and the player foley. Everything under
`project/assets/Audio/Weapons` and `project/assets/Audio/Foley` is **generated** from third-party source packs by the
scripts in `tools/audio/`; neither raw source audio nor derived recordings are committed. Fetch
the private payloads according to the root README. Style target: cinematic heavy (hard transient,
sub-bass punch, long environmental tail, mechanical layer), every sound with variants, every action sound cut so it
can be fired from an animator event on the contact frame of our own clip.

## Contract with the engine (lane S2)

| Item | Where | Meaning |
|---|---|---|
| `audio_manifest.json` | `project/assets/Audio/` | one entry per wav: `file` (relative to `project/assets/Audio`), `key`, `layer`, `variant`, `anchor_ms`, `lufs_m_max`, `true_peak_dbtp`, `length_s`, `channels`, `source`, `mix_db` (float dB, every entry; see "Mix"), optional `lead_ms`, `loop`. The top-level `mix` block holds the shot reference the `mix_db` values were derived from. |
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
  manifest `mix_db` (see "Mix": close ~0, sub -2, mech -3, tail -9 + space, far -15 relative to the shot sum); peaks are the master limiter's, no voice is ever ducked.
* **Full auto**: close/sub/mech on every shot, **tail every 2nd shot, far every 3rd** (the audition reel does exactly
  this). Stacking a 3 s tail per shot at 650 rpm sums to +10 dB and buries the transients. Even so ten AK shots sum to
  about +4 dBFS before the master limiter (-1 dBFS), which catches it; voices keep their levels.
* Far layers are mono (play them as a 3D / distance source); everything else is stereo 1P material.
* Foley keys: `snd.foley.weapon.{ads_in,ads_out,equip,unequip,firemode}`, `snd.foley.move.{jump,land_light,land_heavy}`,
  `snd.foley.step_<carpet|concrete|glass|metal|water|wood>.{walk,run,land}`.
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
build_tails.py      # environment tails         (recipes/tails.json; real recordings only, writes `sources` to the manifest)
build_foley.py      # foley + footsteps + loops (recipes/foley.json)
build_ui.py         # hitmarker ticks (UI/) + body fall (Body/)  (recipes/ui.json)
apply_mix.py        # rewrite mix_db on every manifest entry from recipes/mix.json (every build_*.py does this too)
check_audio.py      # acceptance checks, exit code 0 = pass
render_reel.py      # audition reels to C:\tb\audio-reel (not committed); `render_reel.py tails` for the environment tails
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
* **Gunfire layers** (`build_fire.py`), both guns from the Tactical Shooter Pack on one recipe: close = base report + a
  TR15 crack (high-passed 2-2.5 kHz) + a mid layer (150 Hz - 5/6 kHz) + low body (Mk14, low-passed 600-750 Hz), donors
  phase-aligned to the base (best lag and polarity in the band they contribute, +-1.2 / 2.5 ms), then HPF, EQ, transient
  shaper, compressor, saturation. AK: AK105 base, TR15 mid layer. 870: the SRM-12 shot as base (the Herrington takes start
  soft and clip flat 40-60 ms in), R08 / WK-11 / Mk14 mid layer, a 12-gauge low end (70 / 95 / 180 Hz), built denser
  (`close.target` -12, `max_gr_db` 8) and shaped by a `decay` envelope (hold 30 ms, -30 dB over 220 ms) so the hit is
  front-loaded. Sub = the low end of real shots plus real thuds (`donors`, onset-aligned, low-passed, <= 2 st), saturated;
  the 870's from front-loaded sources only (SRM-12, Mk14, Shapeforms / Gamemaster / Chris Alan thuds at full level up to
  150 Hz, drive 1.7) with its own decay (hold 80 ms, -24 dB over 380 ms). Mech = bolt / forend slam from the action
  sources, delayed 12-21 ms. Tail = the source from +0.12/0.16 s, lightly upward-compressed. Far = real distant shots
  (Pole Position, AK) or the tail low-passed (870), delayed 70-120 ms.
* **Variant counts**: close 8, sub 5, mech 4, tail 5, far 4-6 per gun; action elements 4-7; foley 3-4; footsteps 8-13.
* **Finishing** (`abuild.finish`): gain to the layer's loudness target with a limiting budget per layer (`GR_BUDGET_DB`:
  close 4, sub 1, tails 3, reload / handling clicks 0.5, others 1.5 dB - a file that stops short of its target is levelled
  by the mix); a downward expander under the noise floor; a bleeding second event of the source trimmed off
  (`trim_secondary`: never within 90 ms of the peak, a real event only from 60 ms on, 45 ms fade); a 0.8 ms fade-in and an
  end fade of up to 60 ms (20 % of the file). No file ends above -40 dB re its peak (last 5 ms). Every file is 16-bit.
* **Reload / handling**: action slices are at least `min_dur_s` 0.16 s; mechanical contacts (not cloth, draw / holster,
  swings) go through a transient designer (`snap`: attack 0.85, sustain -0.4) for a hard, short click.
* **Draw / holster** (`motion_only`, `build_elements.motion_variant`): heard over the movement, nothing extra when the hands
  reach the idle pose. The gun's own Kinemation draw / holster recording (AK105 / TR15 / WK-11; Herrington / SRM-12 / Mk14
  for the 870) runs through the arrival, eased 15 dB down from 50 ms before it to 25 ms after (no thud, no cut), under a
  zip / webbing layer (Kinemation General: Equip / UnEquip / pistol holster / Pickup) level-matched to it; 250 ms end fade.
* **870 shell insert**: one event per shell at the port contact (frame 11 of Reload_Loop / Reload_Loop_End); every take is
  the Herrington 11-87 reload-loop shell load (push and click home 120 ms apart as recorded, the clip's are 117 ms), in four
  mild EQ / +-0.3 st colours. `anchor_ms` on a recipe variant sets its contact.
* **Impacts** start on the hit: each source is cut from its first sound (5 % of its peak) minus 2 ms; `check_audio.py`
  fails an impact whose first sound (-40 dB) is more than 5 ms in.
* **Flybys**: the whine riding on the whoosh is removed per frame (`adsp.detone`: bins 1.5-16 kHz over 6 dB above the
  local spectral median pulled to 2 dB over it).

## Mix (`recipes/mix.json`, `mixspec.py`)

Every file is normalised to a loudness target of its own layer, so the files alone say nothing about the hierarchy (a cloth
rustle and a bolt slam are both -21 LUFS-M). The hierarchy lives in one table, `recipes/mix.json`: the playback level of every key
in dB **relative to the player's own gunshot** (close + sub + mech as played = 0 dB). `mixspec.py` turns it into a `mix_db` on
every manifest entry:

    mix_db = spec level - (the file's LUFS-M - the shot reference)

**Contract with the engine**: a voice plays at (set volume) x 10^(mix_db / 20) and the engine does no other loudness math. The only
other factor is Player Gain 0.75 on the layers of the player's own report (it scales close / sub / mech / tail alike, so their
relative levels hold). Everything else plays at set volume 1.0.

**Reference** (stored as `mix.shot_lufs_m` in the manifest, re-measured by `check_audio.py`): the mean over every variant combination
of both guns of the LUFS-M max of close + sub + mech at their `mix_db`, summed from t = 0: about -12.8 LUFS-M; after Player Gain 0.75
(-2.5 dB) the shot as played is about -15.3. Shot layers (close, sub, mech, tail, far) are stored relative to the unscaled sum
(the engine applies Player Gain on top); every other key relative to the as-played reference. Rules on top of the formula, so a
click's LUFS-M (dominated by its length, not its punch) cannot make one variant far louder than its siblings: variants of one key stay
within +-6 dB of the key's median `mix_db` (short keys: see below); a file's true peak plus its `mix_db` never passes -0.5 dBFS; close / sub / mech are
their layer offset (0 / -2 / -3) plus a compensation to the layer's loudness target limited to -2..+1.5 dB (and by that peak rule),
so every shot variant plays equally loud.

Hierarchy (dB re the player's shot; surface and space offsets added where noted; `mix.json` is the source of truth). Tiers sit at
least ~4 dB apart so each layer reads; a 3D category's level holds at its distance model's reference distance (below):

| Tier | Sounds | Level |
|---|---|---|
| 0 anchor | player shot (close + sub + mech) / gun tail / far layer | 0 / -10 + space / -15 |
| 1 threat | soldier's shot at 10 m (close-up cap -3) / flyby at 1 m | -8 / -6 |
| 2 feedback | hitmarker / kill / melee_hit / flesh at 5 m | -14 / -11 / -8 / -12 |
| 3 your hands | bolt_release, mag_in, pump_fwd, shell_load_chamber | -12 |
| | mag_out, pump_back, shell_insert, bolt_back, mag_tap, mag_release | -16 |
| | dry_fire / draw, holster / handle / cloth / melee_swing | -14 / -15 / -28 / -27 / -18 |
| | equip, unequip, firemode / ADS in, out | -20 / -22 |
| 4 world | impacts at 5 m (+ surface; cap -9) / body fall at 5 m (cap -9) | -14 / -14 |
| 5 movement | your walk / run / land (+ surface) | -22 / -18 / -16 |
| | jump / move.land_light / move.land_heavy | -24 / -18 / -14 |
| | a soldier's step at 5 m: your step's level + 3 (walk -19, cap -15) | |
| 6 debris | casings (+ surface) | -26 |
| 7 beds | ambience: indoor small, indoor large / outdoor urban, open | -34 / -32 |
| | impulse responses | no level (mix_db 0) |

**Short keys** (no longer than `short_key_max_s` 0.9 s: clicks, steps, impacts, casings): within a key each file is levelled by
its 100 ms K-weighted loudness (`adsp.loudness_short`) rather than its LUFS-M, shifted by the key's median (LUFS-M - short) so
the key as a whole still sits at its spec level - a 60 ms click and a 300 ms rustle of one key read 6-10 dB apart by LUFS-M.
Variants stay within +-`variant_spread_db` (6) of the key's median `mix_db`.

**Per gun** (`reference`): `gun_db` puts a gun's report over the 0 dB reference, which is measured without it (one number, or per
layer: the 870 is close +3, sub +7.5, mech +2 - the 12-gauge hits harder than the carbine); `mixspec.py` measures each gun's
report as played into the manifest's `mix.gun_offset_db` (870 about +3.1), which `--audio-test` uses as that gun's spec.
`gun_tail_db` moves a gun's tails (870 -3: its recorded tails ring longer). `layer_peak_headroom_db` lets a shot layer's file
peak pass the -0.5 dBFS cap (870 sub +2; still under -1 dBFS after Player Gain, and the master limiter stays idle).

Surface offsets (footsteps, casings, impacts): metal +2, glass +1, wood 0, concrete 0, tile 0, dirt -2, carpet -5, water -1 (ice 0).
Tail space offsets: indoor_small -2, indoor_large +1, outdoor_urban 0 (also the generic `fire_tail`), outdoor_open -3.

**Distance models** (`mix.json` "distance", copied to the manifest's `mix.distance` by `apply_mix.py`). Every 3D category says how its
level holds in the world: the level holds at `ref_m`; closer it rises `slope_db` per halving up to `near_db` above it (the close-up
cap, so nothing but your own gun reaches 0 dB), farther it falls `slope_db` per doubling, held past `max_m`. `offset_db` is added to
the files' own level when they play in the world: a soldier's shot is the shot layer files 10.5 dB down (computed by `mixspec.py`:
npc_shot - the shot layers' level), a soldier's step is the step files 3 dB up (you have to hear them), a soldier's gear 2 dB down.

| Model | ref m | near dB (cap at) | slope dB / doubling | max m | offset dB |
|---|---|---|---|---|---|
| npc_shot (fire_* in the world) | 10 | 5 (5 m) | 5 | 250 | -10.5 |
| flyby | 1 | 0 | 4 | 15 | 0 |
| impact (incl. flesh) / body_fall | 5 | 5 (2.8 m) | 6 | 60 / 40 | 0 |
| npc_step (foley step_* in the world) | 5 | 4 (2.7 m) | 4.5 | 35 | +3 |
| npc_gear (gun and foley gear in the world) | 4 | 3 (2.8 m) | 6 | 25 | -2 |
| casing | 1.5 | 0 | 8 | 20 | 0 |

The engine derives each set's Min Distance (where the cap is reached: ref x 2^(-near / slope)) and an exponential rolloff (miniaudio's
`exponential` model, factor slope / 6.02), and starts the voice offset + near dB over its `mix_db`. The components' own 3D Min / Max
distances for these sounds are no longer in the inspector (the engine still reads them when no manifest is loaded).

Change a number in `mix.json`, run `apply_mix.py` (no audio is touched), run `check_audio.py`: it recomputes every `mix_db` from the
wavs themselves and fails on a mismatch (+-0.1 dB), a missing `mix_db`, a key `mix.json` does not cover, or a stale manifest.

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
| far | -25 | -29..-21 | | ui (hitmarker ticks) | -25 | -31..-19 |
| casing | -26 | -32..-20 | | bodyfall | -17 | -22..-12 |
| impact | -17 | -22..-12 | | flyby | -21 | -26..-16 |
| ambience (phase 2) | -30 | -34..-26 | | ir (phase 2) | no window | |

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

### Combat placeholders replaced (`build_ui.py`, `recipes/ui.json`)

`tools/gen_combat_sounds.py` synthesised the files in `Audio/Combat/` (hitmarker, body_fall, whizz, flesh_hit, dry_fire, reload,
shotgun_pump, ak_shot*, shotgun_shot*): they were louder than the real material and are gone, script and files. The recorded
replacements are `Impacts/flyby_*` (whizz), `snd.impact.flesh`, `snd.<gun>.dry_fire / pump_back / mag_out ...`, the gun layers, and:

| Key | Files | Real material |
|---|---|---|
| `snd.ui.hitmarker` | `UI/hitmarker_1..5.wav` | SculpTunes 9 mm brass casing ticks (8.9 kHz, 10-16 ms), high-passed, layered with Pole Position SAIGA-12 / Steyr TMP handling clicks |
| `snd.ui.hitmarker_kill` | `UI/hitmarker_kill_1..4.wav` | the same ticks, then a heavier second contact ~70 ms later (G36C handling pitched down 2-3 st) over a low Shapeforms thud |
| `snd.body_fall` | `Body/body_fall_1..4.wav` | The Chris Alan body slam / deep punch + Gamemaster body thump + Shapeforms low thud, a cloth rustle and two AK105 pack handling taps (the gear rattle) |

`CombatFx` plays them through `WeaponAudio::KeySet` / `PlayKeyed` (the manifest's `mix_db` is the level, no other scaling): hitmarkers 2D,
body fall 3D. The Pump / Reload / DryFire cues make no sound of their own any more (the animators' `snd.*` events do), the flesh cue is
`snd.impact.flesh`, the whizz is `snd.flyby`. Folders `Audio/Combat`, `UI`, `Body`, `Ambience` (phase 2), `IR` (phase 2) are all covered
by `check_audio.py`: a wav under any of them that the manifest does not list fails (so a `Combat/` placeholder cannot come back).

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
* Water walk has only 5 steps; there is no real water impact, shell-on-metal, shell-on-wood, shell-on-carpet or bullet-in-wood. Candidate real sources (water / wood impacts, shells on metal / wood, room tones, real IRs) are listed for approval in `C:\tb\audio-downloads-r2.md`.
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
  (There is no sprint gear loop: moving is footsteps only; metal handling sounds come only from gun actions.) A foley key on a weapon animation event plays the same way.

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

### Environment tails

The gunshot tail follows the space the shooter is in. Four classes, each its own tail set per gun (5 variants,
`snd.<gun>.fire_tail_<class>`, files `fire_tail_<class>_<n>.wav`): `outdoor_open`, `outdoor_urban`, `indoor_small`,
`indoor_large`. The generic `snd.<gun>.fire_tail` stays: it is the fallback for a class with no files, and what plays when
**Env Enabled** is off. The close / sub / mech layers do not change with the space.

Choosing the space (`WeaponAudio::ResolveSpace`), per tail that plays:

1. **Reverb Zone** components (Audio category; box or sphere placed by the entity's position and rotation, extents / radius in
   metres, Scale ignored). A zone has a Tail Class, a Tail Gain, a Priority and a Fade Distance. Its weight is 0 at the
   surface and rises (smoothstep) to 1 Fade Distance inside. Zones layer from the highest priority down, each taking its weight
   of what is still unclaimed, so an edge is a crossfade and a small room inside a hall sits on top of it. The zones also
   drive the runtime reverb and the ambience beds (see "Reverb bus" below). Zones that move, open or change are re-read each
   frame (`ReverbZones::Refresh`).
2. Whatever share of the mix no zone claims (all of it, outside every zone) is the **probe's**
   (`EnvironmentProbe`): rays from the muzzle through `PhysicsWorld::RaycastSolid` (statics and kinematic bodies; props, ragdolls
   and triggers do not count): one up, a diagonal ring at 45 degrees, a horizontal ring (Env Ray Count, default 12 = 1 + 4 + 7).
   Features: **cover** = half the up ray, half the share of diagonals that hit; **wall** = share of horizontal rays that hit
   within Env Urban Distance; **wall distance** = mean horizontal ray length (a miss counts as Env Max Distance). Class weights:
   `indoor = band(cover, IndoorCover +- BlendFraction)`, `large = band(wallDistance, LargeRoomDistance +- BlendDistance)`,
   `urban = band(wall, UrbanWall +- BlendFraction)` with `band` a smoothstep, then small = indoor(1-large), large = indoor x
   large, urban = (1-indoor) x urban, open = (1-indoor)(1-urban). The four sum to 1 and move continuously, so crossing a doorway is
   a crossfade, not a switch. A zone that fully covers the shooter skips the probe.
3. The probe is cached per shooter and refreshed every Env Refresh Interval (0.25 s) or when the shooter has moved
   Env Refresh Move Distance (1 m): never per shot (a 650 rpm burst probes about 4 times a second). Shooters are named by
   `CombatFx::Shot(..., shooter)` / `WeaponAudio::Shot(..., shooter)` (any stable id); unnamed shooters are the player (2D shots) or
   told apart by position (Env Match Radius).

Playback: the two heaviest classes (a class under 3 % is dropped) play together, equal-power (gain sqrt(weight), the tails are
different recordings), each at its Env Gain (per class) x the zones' Tail Gain; a class with no files plays the generic tail in its
place. Full-auto decimation (Tail Every), the tail voice cap, ducking and the min interval apply to the tail layer as before, counting
the voices of all the tail sets. The logical emission stays `snd.<gun>.fire_tail` (its `Space` field and the `[WeaponAudio] ...
space -> <class>` console lines say which; the audio log gets an `E` line per probe: `E <t> <shooter> <class> <4 weights>
cover wall wallDist enclosure`). Debug: `WeaponAudio::EnvironmentDebugLines` gives the zones' wire shapes and the probe rays
(7 floats per vertex, xyz rgba) when a gun has Env Debug Draw on; the editor overlay does not draw them yet.

Cost: 1.5 us per ray (17-18 us per 12-ray refresh in the Sandbox, max 37 us).

Sound: every tail is cut from **real recordings** (no synthesis, no generated or convolved impulse responses; `tools/audio/
recipes/tails.json`, `build_tails.py`): indoor_small = Audiobeast small room (H&K 416) + Pole Position catacombs; indoor_large =
Audiobeast medium warehouse (870 x8) + Pole Position long corridor; outdoor_urban = Pole Position valley behind buildings;
outdoor_open = Pole Position big open area + forest road, layered with the pack's long With_Tail recordings for the long decay.
Each is [shot onset + start_s, + end_s] of its source (the close layer carries the direct sound), faded, EQ'd, loudness-set into
the tail window, true peak <= -1 dBTP, mono-fold loss <= 4 dB. The manifest entry lists `sources` (file, start_s, end_s, gain_db,
pitch_st) and `check_audio.py` fails any tail without them. Sources live in C:\tb\audio-src (never committed).
`render_reel.py tails` renders the audition reels (a shot with each tail, a burst per space) to C:\tb\audio-reel\tails.

### Tuning

- **Weapon Audio** component (one per gun; `Gun` = `ak` / `870`; a gun without one uses the built-in defaults): volume,
  player gain, shot pitch, jitter, the distance blend, voice caps, the tail, and a `Data File`: JSON of per-layer and per-event
  `SoundSet` overrides: `{"layers": {"close": {...set..., "curve": {"nearDistance", "farDistance", "nearWeight",
  "farWeight"}, "player2d": true}}, "events": {"mag_out": {...set...}}, "tailMinInterval": 0, "tailDuckPerVoice": 0.3}`.
- **Foley Audio** component (one per scene): per-gait volume, jitter, pitch, stride scale, jump / land volume and the
  fall-speed range of a landing, the cloth loop, NPC step range, the default surface and the Surface Table
  (`surface=word,word;...`, matched against the ground's collider material path, tag and name; the surface is the foley category).

### Foley

Footsteps land on the animated feet (Foley Audio "Steps From Feet", on by default): every animated frame the body reports each
foot's height above its feet (`FirstPersonBody::FootHeights`, `NpcBody::FootHeights`) and a `FootContactDetector` plays a step
when a foot touches down - it must first rise Foot Lift Height (5 cm) above its planted height standing, or Foot Lift Moving
(2.5 cm; a walk's first stride barely lifts the ankle) when moving above Min Step Speed, then come back within Foot Contact
Height (2 cm) of it, or stop on the way down (a step up, foot IK). Each touch-down re-bases the planted height; standing weight
shifts stay silent and turning on the spot steps. Without foot bones (or with the option off) steps come from distance travelled,
one every half of the view bob's stride. Walk, run (sprinting, or over Run Speed) and crouch sets; the surface is a downward
ray's collider; a set with no files for a surface falls back to the default surface's. Landing is scaled by the fall speed
(silent below Land Min Speed) and re-bases the feet. Soldiers step on their own animated feet the same way, 3D, heard within NPC
Step Max Distance (`FoleyAudio::NpcFeet`, from `NpcDirector`'s late pose; animation LOD samples them every 2nd / 4th frame
far away). `FOLEY_FEET_LOG=1` prints the feet the steps are read from, a line a frame.

The detector rejects upward rebounds. Higher-ground contacts must reach near-zero vertical speed, and each foot has a 150 ms rearm guard against IK jitter.

A weapon switch is the old gun's holster and the new gun's draw (their clips' events); the shared gear rattle
(`snd.foley.weapon.equip / unequip`) plays only for a gun with no draw / holster takes of its own.

### Audio log

`WEAPON_TEST_AUDIO_LOG=<file>` (or `NPC_TEST_RECORD=<dir>`, writing `<dir>/audio.txt`) logs, with the existing `S` / `L` lines:
`W <sim time> <key> <file|-> <voices> <volume> <pitch> <2d> <x y z> <min> <max> <seek>` for every weapon and foley voice (`-` = the
key has no recorded file yet), and `R <sim time> <layers> (<ir> <weight> <wet dB> <pre-delay ms>)...` when the reverb's target
changes. `tools/mix_npc_video.py` mixes the `S` and `W` lines (dry). For the real thing, reverb and limiter included, set
`AUDIO_CAPTURE=<wav>`: the engine records its master output (after the limiter, 48 kHz float) for the whole session, and
`mix_npc_video.py` uses `<dir>/master.wav` instead of its own mix when it is there.

### Tests

Unit: round robin never repeats, steal-oldest (and the fade), jitter / pitch ranges, JSON round trips, distance blend
weights, the autofire voice cap, `snd.` event to set and 2D / 3D, manifest and layout loaders, footstep cadence vs speed,
surfaces, landing and cloth-loop rules; environment tails: the ray set, the classifier on analytic rooms (small room, hall,
courtyard, open ground), crossfade weights (sum to 1, continuous through every threshold), probe refresh throttling (per shooter,
by time and by distance), class selection and the fallback to the generic tail, Reverb Zone containment / priority / edge blend /
fallback to the probe; the convolver (exact against a direct convolution, threaded tail identical to inline, cost), the reverb's
crossfade (no step, equal power), level calibration and filters, the master limiter (never over the ceiling, transparent below it,
release), the loudness meter (BS.1770 reference tones), the offline engine end to end. `--audio-test` (below) checks the mix.
`--weapon-test` ends by replaying every frame's animator state against the
controllers' `snd.*` events: each crossed event must have been played, in order, within +-1 frame, plus counts for the
gear sounds (ADS, fire mode, dry fire, equip / unequip) and the shot layers.

## Audio Lab scene

`project/scenes/AudioLab.json` is a dedicated scene for hearing and testing every audio feature (no Sandbox clutter). Metres, Y up;
all structures are solid static box colliders, so the probe rays and occlusion see them. The player spawns at (0, 0.1, 8.5) facing -Z
with the AK (key 1), the Remington (key 2) and the gravity gun (key 3), exactly like the Sandbox.

| Area | Where | What to test |
| --- | --- | --- |
| Warehouse hall | x -15..15, z -10..10, y 0..10, roofed. Zone "Audio / Zone Hall" (Indoor Large, fade 2) | big-room tails, ambience `snd.amb.indoor_large` |
| Footstep strips | six 3 x 6 m strips, x centres -7.5 .. 7.5 step 3, z 0..6: concrete, wood, metal, glass, carpet, water | footsteps, jump / land per surface (name AND tag hold the surface word) |
| Impact wall | z -1.5 (10 m from spawn): 2 x 2 m panels at x -10 (concrete), -6 (metal), -2 (wood), 2 (glass), 6 (ice); dirt berm at x 10 | bullet impacts per surface; casings land on the strips |
| Small room | x -20.3..-15.3, z -2..2, y 0..3. Zone "Audio / Zone Small Room" (Indoor Small, priority 1, fade 1.5) | small-room tail, zone priority over the hall |
| Doorways | 1.2 x 2.2 m at (-15.15, z -1) and (-15.15, z +1); "Audio / Portal Small Room A" Open Amount 1.0, "... B" 0.3 | sound heard through an opening, open amount, diffraction |
| Alley | x 15.3..45, z -3..3, 12 m walls, no roof. Zone "Audio / Zone Alley" (Outdoor Urban, fade 3). Hall doorway 4 x 4 m at x 15.15 with "Audio / Portal Alley" | slap echo, hall-to-outside transition |
| Field | 120 x 120 ground centred (105, 0, 0), a few cover boxes. Zone "Audio / Zone Field" (Outdoor Open, fade 3) | open-air tails, distance |
| Settings | "Audio / Settings" holds one Reverb Bus, Foley Audio and Impact Audio at their defaults | tweak in the inspector |
| NPC squads | "Lab / NPC Squads (disabled)": two spawns in the small room, two 40 m out in the field facing the alley. Spawns are inactive (tick them active in the Hierarchy to enable) | flybys, soldier footsteps, shots heard through portals |

Surfaces are matched on the collider's physics material, tag and name against each component's Surface Table (see Foley / Impact above);
avoid naming other colliders with a table word (for example "Floor" reads as wood). The zones also set `Ambience` / `Ambience Volume`.

## Reverb bus, master limiter, ambience

Code: `src/Audio/` (`Convolution`, `ConvolutionReverb`, `MasterLimiter`, `LoudnessMeter`, wired in `AudioEngine`) and
`WeaponAudio` (what the reverb is told, the ambience beds).

**Output graph.** Voices -> their bus (SFX, Music, Ambient, UI, Voice; Project Settings > Audio) -> a meter per bus -> the
**master limiter** -> the device. A voice with a reverb send also feeds one of two reverb buses through a splitter (post fader,
post spatialisation, post occlusion): bus 0 is the listener's room, bus 1 the "remote room" of a voice heard through a portal.
The reverb returns go into the limiter too, at Reverb Bus > Return Level x the SFX bus volume.

**Convolution reverb.** Real recorded impulse responses only (manifest keys `ir.<class>`, `Audio/IR/`; credits with the sources
above). A mono sum of the send is convolved with the stereo IR: taps 0-255 directly, 256-8191 as 256-frame FFT partitions on the
audio thread, the rest as 4096-frame partitions on a worker thread with a whole block of slack (a late block is skipped and
counted, never waited for). Zero latency. IRs are energy-normalised when loaded, so a send of 1 into a space puts its return at
exactly that space's Wet dB under the dry sound. A space is up to two layers (the two heaviest IRs of the listener's zones and,
for what no zone claims, the listener's probe); a change of space is a linear crossfade of the layers' weights at equal power
(gain sqrt(weight)) over Glide Time, and only the weights move: a layer on its way out keeps its level and filters. A convolver
only runs while its layer is audible. Each layer: IR, Wet dB, Pre-Delay (glided at <= 2 % so it never pitch-shifts), HF Damping
(dB cut of a 4 kHz high shelf on the input), Low Cut (2nd-order high-pass on the input).

**Calibration** (Reverb Bus component). Wet / dry at the listener for a send of 1: Outdoor Open -20 dB, Outdoor Urban -12,
Indoor Small -6, Indoor Large -4 (Wet <class> dB), x Wet Scale, + the zone's Wet (dB) trim. Sends are per category and 1 by
default (foley, footsteps, gun actions, impacts / flybys, casings, the shot's close / mech / sub); the recorded tails send 0
(they already hold their room) and so do the ambience beds.

**Reverb Zone fields.** Reverb Mode "Class Default" = the class's IR at the class's level; "Custom": IR (a file; empty = the
class's), Wet (dB) trim, Pre-Delay, HF Damping (dB), Low Cut (Hz). Ambience (a sound key, e.g. `snd.amb.indoor_large`) and
Ambience Volume (linear). Scenes saved with the old feedback-network reverb still load: their Wet Level becomes a trim (its ratio
to the class's old default, +-12 dB) and HF Damping 0..1 becomes 0..10 dB; Room Size, Decay Time and Early/Late Mix are ignored
(the IR is the room).

**Ambience beds.** A box zone fades in across its walls and ceiling, never its floor (the listener stands on it). Each zone with an Ambience key adds a looping 2D bed on the Ambient bus at sqrt(the share the listener's zones
with that key claim) x their Ambience Volume (equal power across a zone edge), moving at most 1 per second (a teleport is a 1 s
fade). A bed starts when it becomes audible and stops once faded out.

**Master limiter.** -1 dBFS ceiling, 1.5 ms lookahead, 80 ms release (Reverb Bus > Master Limiter, Ceiling, Lookahead, Release):
the sample peak never exceeds the ceiling, the inter-sample peak stays within a few tenths of a dB of it, and below the ceiling the
signal passes bit for bit (delayed by the lookahead).

## Mix and 3D distance

A voice plays at `set volume x 10^(mix_db / 20)`, and in the world by its category's distance model (see "Mix": the level at the
reference distance, the close-up cap, the slope): `mix_db` is the manifest's per file (written by `tools/audio/apply_mix.py` from
`recipes/mix.json`), applied once. Component volumes (Foley, Impact, Weapon Audio) default to 1 and only scale (per gait, per fall
speed, ...). Player Gain (0.75) applies to the player's own shot layers only. Everything a voice needs (position, rolloff, pitch, its
duck gain) is set before it starts, so its first block is already placed (setting them after the start let the audio thread render a
block unspatialised at the voice's full calibrated gain: a click up to 10 dB loud on a soldier's shot).

Panning is constant-power (a patch in `extern/miniaudio.h`, marked TARTARUS PATCH): a 3D sound has the power it has played 2D, whichever way the listener faces; stock miniaudio played a source straight ahead 6 dB down and one at the side about 3 dB louder than that.

Sounds with no distance model (none at present among the recorded sets) keep their set's own Min / Max and logarithmic rolloff.

## Dynamic mix

The scene's **Audio Mix** component (one per scene; the defaults play without one; Window > Audio edits it live):

* **Mix groups.** Every voice has one, from its key and whether it is yours (2D) or in the world (3D): weapon (your gun), threat
  (soldiers' shots, flybys), feedback (hit markers, flesh hits), own foley (your steps, cloth, gear, reloads), npc foley (soldiers'
  steps, gear, reloads), world (impacts, bodies), debris (casings), bed (ambience).
* **Ducking.** A weapon or threat voice keys the duck with its level at the listener (the file's LUFS-M + its gain + the distance's,
  against the reference): from -20 dB (Duck Threshold) up to -6 dB (Duck Full) the groups under it go down by their depth - beds 6 dB,
  your foley 3, casings 3, impacts 0 - in 15 ms, hold 0.15 s after the last loud event and come back over 0.8 s. Threat, feedback and
  npc foley are never ducked: they are what you have to hear. The duck is each voice's fader (`AudioEngine::SetDuck`), separate from
  its volume.
* **Focus.** Aiming down sights (the player's own view) takes the beds 4 dB, your foley 3 and casings 2 down over 0.25 s.
* **Air.** Every 3D voice is low-passed with distance: open to 15 m, then 20 kHz x (15 / d)^0.6, never below 4 kHz (60 m: about
  8.7 kHz; 150 m: 5 kHz), folded into the same low-pass as occlusion (min of the two).
* **Master.** Trim -3 dB, then the glue compressor (linked peak, threshold -15 dBFS, 2:1, 6 dB knee, 15 ms attack so a shot's
  transient passes, 200 ms release), then the limiter. A sustained firefight integrates near -19 LUFS with a couple of dB of glue at
  most and the limiter idle (`--audio-test` part 5).
* **Wet levels.** The Reverb Bus's Wet <class> dB are what is heard: the impulse responses are calibrated on white noise and real
  material (impacts, shots, K-weighted) came back about 4 dB hotter, which Wet Trim dB (-4) takes back.

## Audio panel

Window > Audio (editor): peak and loudness meters for the master, each bus and the reverb return, the limiter's gain reduction,
the voice count, each reverb bus's running IRs and weights, and in Play the zones at the listener, the reverb's target and the
ambience beds. In Play also the **Mix**: the duck and focus envelopes, the glue's gain reduction, the live voices and duck gain per
mix group, and faders for the ducking, focus, air, master (trim, glue) and the rooms' wet levels, applied as you move them. **Keep
after Play** writes them to the scene's Audio Mix / Reverb Bus components when Play stops (as an undoable edit; save the scene);
**Defaults** puts the mix back to the shipped values.

## --audio-test

`TartarusEngine.exe --audio-test [scene] [--report <file>]` (default scene `AudioLab`): the real engine rendered offline (no device,
no window) under the real `WeaponAudio`, over the scene's Reverb Zones.

0. Tier ladder: the spec's levels and distance models against the hierarchy's rules (nothing but your gun reaches -3 dB close up; a
   soldier's walk at 10 m and your own walk 4 dB over the loudest bed; a soldier's walk at 5 m over yours). No audio.
1. Levels: every manifest key played alone, dry, without jitter or ducking, LUFS-M max against `recipes/mix.json` (reference + level +
   surface offset), 3D at its distance model's reference distance; the player's and a soldier's shot (close + sub + mech) at 10 m and
   close up (the cap); a soldier's step and reload (the offsets).
2. Wet / dry: an impact 2 m in front of the centre of each zone; reverb return energy against the dry signal's, against the zone's
   calibrated level x the impacts' send.
3. Ambience: each zone's bed at its centre against the spec's ambience level.
4. Peak: a 30-round burst with impacts and a soldier's shots in the first Indoor Large zone; the master never exceeds the ceiling.
5. Firefight: 20 s in the first outdoor zone with a bed - your bursts with impacts and casings, two soldiers answering from 20 and
   35 m, flybys, a third walking the flank at 8 m - with the scene's whole dynamic mix: integrated loudness (in [-22, -16] LUFS),
   short-term range, the glue's and the limiter's work (the limiter over 1 dB at most 5 % of the time), the beds' duck depth.

Exit 1 when a level is off by more than 2 dB, a wet / dry by more than 3 dB, the master peak is over the ceiling, a rule of the
ladder or a firefight bound fails, or a part could not run (no manifest reference, no spec, a zone with no IR).
