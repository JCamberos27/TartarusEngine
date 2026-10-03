# Audio revamp — handoff (2026-10-03)

> **Status (2026-10-03, merged to `main`):** parts A and B below are done and merged together with a third session
> (branch `audio-fixes`): footsteps on the animated feet, impacts that start on the hit, the 870 rebuilt from the
> Tactical Shooter Pack with a cinematic low end (+3 dB over the AK), Kinemation shell inserts in one event per shell,
> draw / holster on the movement only with a zip layer, no gear rattle on weapon switches, no hard cut-offs, short-key
> variant levelling, the flyby whine removed, Sandbox reverb zones on their calibrated class defaults. `docs/AUDIO.md` is
> the reference for all of it. Still open: more free Sonniss GDC takes for impacts / casings / shell bounces / flybys (ask
> before each download), heavier sprint steps, the AK reload's two-contact events, room tails from more than one recording
> per space; the Hell2025 sounds are on hold (no licence in that repo).

Branch: `audio-int-trial` (pushed to GitHub). It is `audio-int` + the VX / SL / SM (WIP phase 2) / SE-2 lanes +
everything below. **Not merged to main — ask the user before merging anything to main.** Older context: `docs/AUDIO_PASS_HANDOFF.md`
(the original mix/reverb pass) and `docs/AUDIO.md` (the full reference, updated for everything in part A).

## Hard rules (from the user; never break)

* Every sound is a real recording (no synthesis, no generated noise / tones / IRs). `check_audio.py` scans the tools for generators
  and requires a `sources` list on outputs.
* Pitch shifts of any source ≤ ±2 semitones (variety from takes and cuts, not pitch).
* No paid sound packs. Sources live in `C:\tb\audio-src` (never committed): `tsp/Tactical Shooter Pack` (Kinemation), `boots/Boots`,
  `sonniss/<pack>` (a few hand-picked files per free GDC pack).
* Minimal testing: `--unit-tests` + at most ONE targeted run (`--audio-test`) per code change; never re-run a passing test without a
  change. Data/asset-only changes: tools checks (`check_audio.py`, `analyze_audio.py`) only, no engine run unless loading changed.
* Don't drive the desktop to test. Build and launch the editor for the user to listen (`C:\tb\run-audio-trial.cmd`).
* Never push / open PRs unless asked in that request. Bots: Haiku / Sonnet only.

## Build / run

* Build dir `C:\tb\qSE2` (source = this worktree). `cmake --build . --config Release --target TartarusEngine -- /m` from Git Bash
  needs `MSYS_NO_PATHCONV=1`. A new `.cpp` needs `cmake .` first. If the user's editor is running, rename
  `Release\TartarusEngine.exe` (Windows allows renaming a running exe) before building.
* Tests: `LOCALAPPDATA/TEMP/TMP=C:\tb\qSE2\run`, `TartarusEngine.exe --unit-tests --project <worktree>\project` and
  `--audio-test --project <worktree>\project --report C:\tb\qSE2\run\at.txt`.
* Audio tools (from `tools/audio`): `uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python <script>`.
  Order after recipe changes: `build_<x>.py` → `check_audio.py` → `analyze_audio.py <key prefixes>`.
* Launch for the user: `C:\tb\run-audio-trial.cmd` (opens this worktree's project; scene `scenes/AudioLab.json`, Window > Audio).

## A. DONE this session (engine + mix) — committed

Verified: `--unit-tests` 36633 checks / 0 failures; `--audio-test` 100 checks / 0 failures (before the sprint-loop removal and the
asset revamp; unit tests re-run after the loop removal: 0 failures).

1. **Removed the old SoundPlayer "bus limiter"** (gain ducking up to −20 dB on any voice within 150 ms of a loud one) — it was the
   main cause of "levels all over the place". Peaks are the master limiter's job.
2. **Bugs fixed:** 3D voices were started before their position / rolloff / pitch were set (first block unspatialised at full
   calibrated gain → up to +10 dB clicks); everything is now set in `AudioEngine::VoiceFx` before `ma_sound_start`. Reverb-zone fade
   counted the floor (at head height you got ~half the zone: beds −2.6 dB, reverb mixed with the probe) — boxes now fade across walls
   and ceiling only (`ReverbZoneVolume::Weight`). Soldiers' shots were +2.5 dB over your own gun at 10 m (spec −6 never applied).
3. **Static tier hierarchy** in `tools/audio/recipes/mix.json` (levels dB re your shot; tiers ≥ ~4 dB apart) and a **`distance`
   table** (per 3D category: `ref_m`, `near_db` close-up cap, `slope_db` per doubling, `max_m`, `offset_db`). `mixspec.py` writes it to
   the manifest `mix.distance` (npc_shot offset computed). Engine: `DistanceModel` (Game/Audio/AudioMix.h) → Min distance where the cap
   is reached, miniaudio exponential rolloff, start gain offset+near. Current values: walk −22 / run −18 / land −16 (raised after the
   user's "can't hear my footsteps"); soldier steps = yours +3 at 5 m; soldier shot −8 @10 m cap −3; impacts −14 @5 m cap −9; beds
   −32/−34; reloads −15/−19; casings −26; flyby −6 @1 m; hitmarker −14 / kill −11.
4. **Dynamic mix** (`Game/Audio/AudioMix.{h,cpp}`, `AudioMixComponent` "Audio Mix"): mix groups per voice (weapon, threat, feedback,
   own foley, npc foley, world, debris, bed); any weapon/threat voice keys the duck with its level at the listener (file LUFS +
   gain + distance vs `mix.reference_lufs_m`); beds −6, own foley −3, casings −3 under fire (attack 15 ms, hold 0.15 s, release
   0.8 s); threat/feedback/npc foley never ducked; ADS focus (beds −4, own foley −3) via `WeaponAudio::SetFocus` from
   `FirstPersonPresentation` (owner view); air absorption low-pass with distance (open to 15 m, 20 kHz·(15/d)^0.6, ≥ 4 kHz) folded
   into the occlusion LPF; duck = each voice's ma_sound fader (`AudioEngine::SetDuck`).
5. **Master:** trim −3 dB → glue compressor (linked peak, −15 dBFS, 2:1, 6 dB knee, 15/200 ms) → limiter (−1 dBFS). Settings handed to
   the audio thread with a try-lock (never blocks). Firefight: −18.6 LUFS integrated, limiter idle.
6. **Reverb:** `ReverbBusComponent::WetTrimDb = −4` (white-noise-calibrated IRs read ~4 dB hot on real material); wet/dry now within
   0.6 dB of the class targets.
7. **Editor Audio panel** (Window > Audio): live duck/focus/glue readouts, per-group voices and gains, sliders for ducking / focus /
   air / master / room wet levels, "Keep after Play" (writes Audio Mix + Reverb Bus to the scene on Stop as an undoable edit),
   "Defaults". Component distance fields owned by the spec are hidden in the inspector (`MixOwnsDistanceField`).
8. **`--audio-test`:** part 0 tier ladder (rules), levels at ref distance + close-up cap + soldier step / reload offsets, wet/dry,
   beds, master peak, part 5 scripted 20 s firefight (integrated LUFS in [−22, −16], limiter > 1 dB ≤ 5 % of the time, beds duck).
   `LoudnessMeter::Integrated` (BS.1770-4 gated) and `ShortTermRange` (EBU LRA) added.
9. **Removed the sprint gear loop entirely** (user: "metal clank when walking"; it was gun-handling slices on every stride): engine,
   component fields, registry, builder, recipe, spec, 3 wavs, docs. Moving = footsteps only; metal handling only on gun actions.
10. Volume jitter tightened (shot layers ±0.5 dB, others ±1 dB).

## B. IN PROGRESS — the sound revamp (user: "do a whole revamp … make sure they're all good and sound good; layer the TR15 shots;
go through motion sounds, steps, the shotgun and everything")

### B0. Tools added (committed)
* `tools/audio/analyze_audio.py [key-prefix …] | --files a.wav …` — objective "ears": per file LUFS-M, true peak, crest (peak vs
  loudest 50 ms RMS), attack (10→90 %), band balance (sub/low/lmid/mid/pres/air re total), centroid, noise at start/end re peak,
  flat-top density, stereo correlation; per key: band deviation vs siblings, near-duplicate pairs (envelope & spectrum > 0.97).
  Flags: squashed (crest < 9 on transient layers), soft attack, noise > −45 dB, phasey (< 0.2; < −0.1 for tails), outliers, dups.
  Notes: "soft attack" is expected for slices with `lead_ms` (pump/bolt strokes) and cloth/draw leads; far/tail near-dups are
  partly natural (same room / same distance). Baseline reports: `C:\tb\audio-work\analysis_before.txt` (weapons) and
  `analysis_before2.txt` (foley/steps/impacts/casings/ui/amb).
* `adsp.secondary_event / trim_secondary` (cut a slice before the next event of the source bleeding in) and `adsp.expand_floor`
  (downward expander 50 dB under the peak, up to 24 dB) — used by `abuild.finish`.

### Status 2026-10-03 (second session) — B1–B4 done, B5 partly; uncommitted until the user asks
* Every builder rebuilt with the new finish policy (tails now use the default 3 dB limiter budget, was 7). `check_audio.py` PASS
  (warn: rifle tile casings 5 variants — the 7.62 take has only 5 drops). `--audio-test` 99 checks / 0 failures, firefight
  −19.0 LUFS integrated, peak −4.1 dBFS. No engine code changed (no unit-test run needed). Editor launched for listening.
* `analyze_audio.py`: duplicate detector rewritten (2 ms envelope, best alignment ±10 ms, and spectral *character* = log
  1/6-octave spectrum minus the key's mean; pair = env > 0.95 and character > 0.8; keys of 2 files compare raw spectra, stricter).
  The old one called every footstep of one floor a duplicate. "noise" split into "noise before onset" (often a natural pre-roll:
  pump strokes, cloth) and "ends at" (a cut tail or the next event; expected on reload elements, which end where the next element
  starts). Low crest on whooshes / thuds / body falls is natural, not limiting (finish now limits ≤ 1.5 dB there).
* Fixed: 870 close 5 (Herrington 2 low body −4 dB instead of Mk14 — sub back in line); 870 mech 2 was the same recording as mech 1
  (SRM-12 ShotBoltOnly / Timed / ShotWithBolt are one take) → now the bolt-back event (ShotBoltOnly t 0.70, 0.13 s); 870
  shell_load_chamber 4 now holds both the click and the chamber slam (dur 0.30). Casings: a key never cuts one drop twice
  (long + short of one drop were duplicates), rifle metal proxy now uses the 9 mm drops concrete leaves (was the tile take
  re-EQ'd = duplicate of tile), carpet low-passed 6 kHz, casings ship **mono** (`point_source`: louder channel of a decorrelated
  spaced-pair take, else mid fold — stereo casings were phasey, corr down to −0.4). `check_audio.py` accepts `start/end` and
  `start_s/end_s` sources on tails.
* B4 done: AK +8 takes from AK-pattern / rocking-mag rifles (template matching `scratchpad match.py`, kept in the session only):
  mag_release + SRM-12 EmptyReload 0.524; mag_out + SRM-12 TacticalReload 0.731 (hp 300), SRM-12 MagCheck 0.941; mag_in + SRM-12
  EmptyReload 1.907, Mk14 MagCheck 3.027, SRM-12 MagCheck 2.58; bolt_back + SRM-12 EmptyReload 3.12; bolt_release + SRM-12
  EmptyReload 4.222. Sub outliers high-passed (mag_release 1, mag_tap 4, bolt_back 2, bolt_release 2, mag_in 4).
  Counts: release 5, out 6, in 7, tap 4, bolt back 4, bolt release 4.
* Footsteps: `diverse_steps` now uses `analyze_audio.similarity` for its farthest-point distance. Duplicates left (new metric):
  carpet run 3 pairs, concrete walk / glass walk / water run 1 each — real strides of one take; acceptable.
* Left as is (source-limited — only 2–4 files per Sonniss pack are on disk; the full free GDC bundles would fix them): casing
  dirt / shell metal / shell concrete pitch variants of one drop (near-dups), impact flesh 6~7, carpet casings with a bright ping
  (carpet 3), hitmarker variants differ in mid (+12–16 dB on the SAIGA-layer ones).
* **TODO (user approved 2026-10-03): fetch more of the free Sonniss GDC bundle packs** for bullet impacts (dirt, flesh, wood,
  metal, concrete, glass), casings / shells (dirt, metal, concrete, tile, wood) and flybys — e.g. PMSFX Bullet Bys & Impacts,
  Gamemaster Bullet Impact Sounds, SculpTunes Cartridges & Casings, Stuart Duffield Bullet SFX. Free packs only (never paid).
  Ask the user before each download (name, source, size); place files under `C:\tb\audio-src\sonniss\<pack>`; then add takes to
  `recipes/casings_impacts.json`, rebuild `build_impacts.py`, `check_audio.py`, `analyze_audio.py snd.casing snd.impact snd.flyby`
  (goal: ≥ 6 distinct real takes per key, no pitch-only variants).
* Next: the user's ear notes; then docs/AUDIO.md (finish policy, variant counts, mono casings, analyzer) and memory.

### B1. Finishing policy (code done, NOT yet rebuilt for all builders)
* `abuild.finish`: limiter budget per layer `GR_BUDGET_DB` (close 4, sub 1, tail/far 3, ambience/loop 2, everything else 1.5 dB).
  Old behaviour limited up to 7–10 dB just to hit a loudness target the mix then turned down again → squashed punch (melee hits crest
  4–8, subs 5–6, draws 8). One-shots get `expand_floor`; `trim_bleed=True` from build_elements (non-composites), build_fire (mech) and
  build_foley weapon/move one-shots except land_*.
* `recipes/targets.json` minimums loosened (files may end quieter than target; mix_db levels them).
* **TODO:** rebuild every builder so all assets get the new policy: `build_elements.py`, `build_foley.py`, `build_impacts.py`,
  `build_ui.py`, `build_tails.py` (tails keep max_gr 7 explicitly — consider 3), `build_ambience.py` (loop: untouched by the expander).
  Then `check_audio.py` (must PASS) and `analyze_audio.py` (compare with the baseline files above).

### B2. Gunfire (done: rebuilt; review numbers then listen)
* `build_fire.py`: `onset_of(x, d)` — any source/donor/far may give `"t"` (seconds) to pick a shot inside a multi-shot recording;
  donors get a `"layer"` slot besides `crack`/`body`; body/layer alignment search ±2.5 ms (crack ±1.2 ms); sub is mono (written as
  1-channel) with light saturation (0.6·x, drive 1.25).
* AK (`recipes/ak_fire.json`): 8 close variants = AK105 recordings 0/1/2 × a distinct TR15 crack (hp 2–2.5 kHz) + a distinct TR15 full
  shot "layer" (150 Hz–6 kHz, −8/−9 dB) on every variant + Mk14 body on some, pitch −0.5…+0.5; chain drive 1.4, comp 3:1. Result:
  no near-dups, crest 10–11, presence/air up. Far: 6 real AK shots 50 m away (Pole Position `AK5_valley`, t = 0.253, 45.672, 52.553,
  61.623, 7.412, 21.514), naturally similar to each other.
* 870 (`recipes/remington870_fire.json`): 8 close variants over 5 bases (Herrington 1/2/3, SRM-12 ShotOnly, TS Sound 12-gauge) each
  layered with a different real Remington 870 shot (Audiobeast medium warehouse 10 m, t = 22.39, 29.16, 34.48, 38.64, 43.20, 46.78,
  49.51, 52.92; 80 Hz–2.5 kHz, −5…−7 dB), SRM/Mk14 body or TS crack on some, pitch ±0.8; chain drive 1.35, comp 3:1. Last analysis:
  no near-dups; variant 5 (TS base) has −9 dB sub vs siblings → give it an SRM/Herrington low body or swap its base.
* Multi-shot sources found (shot times, s): Audiobeast 870 x8 (above); Pole Position `AK4_long_corridor` 10 singles (0.499, 7.764,
  15.572, 23.356, 31.5, 39.698, 47.606, 55.143, 62.687, 72.235); `AK47_big_open_area` singles 0.845, 5.572, 9.565, 13.87, 17.996 (+bursts
  23.4–25.2, 30.0–32.0); `AK5_valley` singles 0.253, 7.412, 14.898, 21.514, 27.569, 33.968, 39.558, 45.672, 52.553 (+burst 61.6–63.2);
  `AK47_catacombs` 0.698, 0.95, 1.316 (burst).
* TODO: AK/870 mech and tail layers untouched beyond the finish policy; consider 870 far from different sources (now Herrington /
  SRM tails, fine). Env tails (`build_tails.py`, `recipes/tails.json`) are near-identical within a class because each class uses one
  room — acceptable; indoor_small leans on a single H&K 416 shot (room416 t 3.82) → add catacombs shots 0.95 / 1.316 variants.

### B3. Footsteps (code done, NOT yet rebuilt)
* `build_foley.py`: `diverse_steps` — of all isolated steps in a Boots 30 s sequence, drop tonal outliers (any band > 3 dB from the
  pool median), then farthest-point selection on envelope shape + band balance (starts from the most typical step). Recipe:
  walk `seq_variants` 8 (+3 singles), run 10. Baseline problem: concrete run 10 near-dup pairs, metal run 11, carpet/wood squashed.
* TODO: rebuild (`build_foley.py`), check dups gone with `analyze_audio.py snd.foley.step_`; consider adding the Boots stairs
  sequences only if tonally consistent; `General/Walk|Run` concrete takes 1,3,5 / 2,4,5 are unused (6 more concrete takes).

### B4. AK handling — new takes from unused rifle recordings (NOT started)
* Unused Kinemation files: TR15 Actions (Draw, Holster, Inspect, Mag_Check), SRM-12 Actions (EmptyReload, TacticalReload, MagCheck,
  Inspect, Draw, Holster, Bipod*), WK-11 Viper Actions (Reload_Empty, Reload_Tactical, Mag_Check, Inspect), Mk14 (Draw, Holster,
  Inspect, MagCheck), Herrington Reload_Start_Normal. Pole Position "Various Gun Foley & Handling" G36C mag in & out (more takes).
* Plan: write a slicer helper that lists onsets of each new file and template-matches them (band profile + envelope) against the
  existing AK slices (`recipes/elements.json` has times for AK/TR15/Mk14 mag_release, mag_out, mag_in, mag_tap, bolt_back,
  bolt_release, handle) with order constraints (release → out → in → tap → bolt back → bolt release); add 2–3 takes per element so
  each key has 6+; keep pitch within ±2 st; `analyze_audio.py snd.ak.` must show no dups / tonal outliers.
* Baseline issues: bolt_release_3 squashed; mag_in_4 sub +11 dB & noise −32; bolt_back noise −32; handle tonal spread.

### B5. Shotgun handling, motion foley, impacts, casings, UI (NOT started; analysis baseline in analysis_before*.txt)
* 870: pumps noise −35…−42 dB (expander should fix), shell_load_chamber_4 noise −19 (bleed: trim_bleed), handle sub outliers, draw /
  holster cloth noisy (−34) and crest 8.
* Foley: ads/equip noise (−40s), land_heavy crest 5–8 (fixed by GR budget), land_light 3 near-dups (needs other source cuts).
* Impacts: noise on all (expander), flesh squashed, dirt 5 dups, wood one variant crest 4.8. Casings: dirt / shell metal soft & noisy,
  wood rifle 6 dups. Flyby noise. Body fall squashed. UI hitmarker noise flags (check after rebuild).

### B6. Finish
1. Rebuild all, `check_audio.py` PASS, `analyze_audio.py` (no squashed, no dups except tails/far, no noise > −45 on one-shots).
2. `apply_mix.py` is run by every builder; levels in `mix.json` unchanged by the revamp (they're relative to the shot reference,
   which shifts if the close layer changes — fine, everything follows).
3. Rebuild engine only if code changed; `--unit-tests`; ONE `--audio-test`; launch for the user; update `docs/AUDIO.md` (Method,
   variant counts, finish policy) and memory `project-audio-pass.md`.
4. Ask the user before merging anything to main.
