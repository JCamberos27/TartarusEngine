# Audio mix / reverb pass: handoff (paused 2026-10-03)

> **Superseded:** this pass is finished and merged to `main`; see `docs/AUDIO.md` (reference) and `docs/AUDIO_REVAMP_HANDOFF.md` (what came after).

Read this first, then `docs/AUDIO.md`. The base branch is `claude/audio-int`, the integration branch, which is not on main yet. **Ask the user before merging anything to main.**

## Why
The user's complaints were:
- The mix is inconsistent: some sounds too quiet, some too loud.
- Reverb is barely audible.

The user asked for:
- a full pass over every audio system and asset;
- a dedicated test scene.

## User rules (hard)
- **Nothing generated:** every sound is a real recording processed with tools. No oscillators, noise, synthetic IRs or algorithmic reverb. A runtime convolution with *real recorded* IRs is allowed.
- **Pitch shifting:** at most about ±2 semitones on assets. Variety comes from different takes, cuts or EQ, not pitch.
- **No paid sound packs:** the user declined them. Casings on metal/wood and water/wood impacts stay `proxy: true`.
- **Minimal testing:** `--unit-tests` plus ONE targeted run. When the user is home, build and launch the editor for them instead of recording.
- **NPC voice system:** removed. The user won't use it.
- **Orchestration:** bot work uses Haiku/Sonnet only (protocol in `C:\tb\q-protocol.md`). Commit on local branches; push only when the user asks.

## Branches (all pushed)
| Branch | State | What |
|---|---|---|
| `claude/q-VX-1` | **Done**, unit tests + `--npc-test default` pass | Removes the NPC voice / bark / subtitle system. Squad glance-at-callout behaviour kept in `NpcDirector` (`CallKind`, `Callout`). |
| `claude/q-SL-1` | **Done**, not run yet | `project/scenes/AudioLab.json`: warehouse hall (indoor_large) with 6 footstep strips and an impact wall; small concrete room (indoor_small) behind 2 portals (open 1.0 / 0.3); alley (outdoor_urban); 120 m field (outdoor_open). NPC squads are disabled via the entity `active:false` flag: verify that keeps them off. |
| `claude/q-SM-1` | Phase 1 done (56d2237e); phase 2 is **WIP** (9e4c774b) | Assets and mix. See below. |
| `claude/q-SE-1` | **WIP** (19aec7fc), may not build | Engine: convolution reverb, limiter, ambience, mix_db, `--audio-test`. See below. |

## SM: assets (tools/audio, project/assets/Audio)
**Phase 1 (done):**
- `recipes/mix.json`: a level hierarchy relative to the player's shot.
- `mix_db` on every manifest entry, applied by `abuild.update_manifest` / `apply_mix.py`.
- `check_audio.py` checks `mix_db`.
- Real UI hitmarkers (`snd.ui.hitmarker[_kill]`, `Audio/UI/`) and body fall (`snd.body_fall`, `Audio/Body/`).
- The generated `Audio/Combat/` and `tools/gen_combat_sounds.py` are deleted.
- `CombatFx.cpp` placeholder paths are removed. **Not compiled yet.**

**Phase 2 (WIP; continue):**
1. **Downloads, approved by the user.** Free items #1-#6 plus all free optional items in `C:\tb\audio-downloads-r2.md`: EchoThief IRs, OpenAIR warehouse/forest/Koli/R1/stairway, the Sonniss 2016 room tones, Hzandbits urban wind. Also extract day loops from `OneDrive\Desktop\ASSETS TO IMPORT\Field Ambience.rar`. Sources go to `C:\tb\audio-src` (local only, never committed). Check what already arrived there.
2. **Pitch cap.** Rework every recipe to |pitch| ≤ 2 st:
   - fire_sub: no octave drop;
   - casing/shell/impact proxies: unpitched real takes;
   - add a `check_audio.py` rule that fails on `pitch_st` > 2.
3. **IRs.** `build_ir.py` / `recipes/ir.json` exist (WIP). Output goes to `Audio/IR/<class>_<n>.wav`, key `ir.<class>`, layer `ir`, with fields `rt60_s` and `predelay_ms`.
   - 48 kHz stereo, with the direct sound trimmed.
   - Length ≤ 2.5 s indoor, ≤ 3 s outdoor.
   - Credits go in docs/AUDIO.md: OpenAIR CC BY, and the EchoThief licence terms.
4. **Ambience.** `build_ambience.py` / `recipes/ambience.json` exist (WIP). Output is seamless loops at `Audio/Ambience/<class>_<n>.wav`, key `snd.amb.<class>`, `loop:true`.
5. Rebuild all, get `check_audio.py` to PASS, update docs.

Classes: `outdoor_open`, `outdoor_urban`, `indoor_small`, `indoor_large`.

## SE: engine (src/Audio, src/Game/Audio)
**Contract:** a voice plays at `set volume × 10^(mix_db/20)`, nothing more.
- PlayerGain 0.75 applies only to the player's shot layers.
- The Foley/Impact/Weapon component static volumes are re-defaulted to 1.0. Their per-speed scaling stays.
- Distance-referenced sets are calibrated to the spec at the distances in `mix.distance_refs_m`: npc_shot 10 m, impact 5 m, body_fall 5 m.

Tasks (WIP; check what's done in 19aec7fc):
1. **Convolution reverb replaces the FDN** (`ReverbFdn.*` and `ReverbRender.*` are deleted). Partitioned FFT (`src/Audio/Fft.h`), with a 256-frame head on the audio thread and the tail on a worker thread, ≤ 2% of a core per convolver.
   - Space → IR comes from manifest `ir.<class>`.
   - A change of space crossfades the 2 heaviest IRs.
   - Zone fields: IR, Wet dB, Pre-Delay, HF Damping, Low Cut. Old keys must still load.
   - Update the reverb path in `tools/mix_npc_video.py` (scipy convolve).
2. **Sends calibrated** so the wet/dry at the listener is about: small -6 dB, hall -4, urban -12, open -20. Remove `SendVoice`.
3. Apply `mix_db` once for every set. Write a rolloff table in docs.
4. **Fix the leftovers:**
   - `WeaponAudio.cpp` `Default(gun)` still names the deleted `Combat/*.wav`; remove it and update `UnitTests_Audio.cpp` around line 279.
   - `CombatFx.h` has a stale `PlaySound` declaration and comment.
   - `mix_npc_video.py` needs `W` log-line support.
5. **Master peak limiter** (`src/Audio/MasterLimiter.cpp`) on the endpoint: -1 dBFS ceiling, 1.5 ms lookahead, 80 ms release, reflected next to the Audio Mixer.
6. **Zone ambience:** ReverbZone fields "Ambience" (key) and "Ambience Volume", crossfaded by the listener's zone weights.
7. **Audio test:**
   - `AUDIO_CAPTURE=<wav>` taps the master output.
   - `--audio-test [AudioLab]` (`src/Game/Audio/AudioTest.*`) runs a scripted tour and writes a report: LUFS vs `mix.json`, wet/dry per zone, master peak. It exits 1 if a level is off by more than 2 dB, a wet/dry by more than 3 dB, or the peak is above -1 dBFS.
   - Its main.cpp dispatch lines are in the WIP commit.
8. Editor Audio debug panel (`EditorLayer_AudioDebug.cpp`, one menu line): meters, zone weights, active IRs, voice count.

**Tests:** `--unit-tests`, plus `--audio-test` once.

## To finish
1. Finish SE and SM phase 2, on their branches or in a fresh session.
2. Merge VX, SL, SM and SE into `claude/audio-int`.
   - Expected conflicts: `Components.h` / `ComponentRegistry.cpp` (VX removed `SubLinger`; SE edits the lane S block), `docs/AUDIO.md`, `CombatFx.cpp` (SM).
3. Build (`C:\tb\qI` is the integration build dir).
4. Run `--unit-tests` once, then `--audio-test AudioLab` once, and tune `mix.json` / sends from its report.
5. Launch the editor on AudioLab for the user to listen (`C:\tb\run-quality-pass.cmd` style: exe plus `--project <worktree>\project`).
6. Ask before merging `claude/audio-int` to main.
