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

Extracted by `tools/audio/extract.py` to `C:\tb\audio-src` (`C:\Program Files\WinRAR\UnRAR.exe` for the .rar, `zipfile`
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

## Known weak spots in the source material

* The Herrington 11-87 pack has **no pump sound at all**, and its Draw / Holster / Reload_End / Reload_Start_Normal
  files are near silent (-42..-49 dBFS peak, cloth only). The 870 pump is therefore the SRM-12 bolt manipulation pitched
  down 3-3.5 st with a low-mid bump (the AK bolt as a fourth variant); 870 draw / holster = Herrington cloth +
  SRM-12 / TR15 / Viper thud. Worth replacing with a real pump recording if one turns up.
* The pack has **no melee, no dry fire** sounds: both are synthesised (`build_elements.py`: hammer click + knock;
  band-swept noise whoosh; body thump + slap lifted from an AK / SRM transient).
* The pack's masters are normalised to -2..-5 dBFS and many onsets cluster 30-50 ms apart; the slice boundaries were set
  from onset maps without auditioning each take. Listen to the reel and re-time `t` / `dur` in `recipes/elements.json`
  if a take includes a neighbour.
* The AK / SRM donors for the 870 shell sounds are Herrington only; shell_insert has the weakest low end.
* Water has only 5 walk steps (the sequence has few clean isolated steps); other surfaces 6-8.
* `conf: medium` sync events (hands) are heuristic; hand movement is small and noisy in several clips.
