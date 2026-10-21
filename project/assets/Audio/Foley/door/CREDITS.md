# Interior door foley: sources and licenses

Built by `tools/audio/build_doors.py` from `tools/audio/recipes/doors.json` (rerun the script, don't hand edit the wavs).
Every `.wav` here is cut, high-passed, faded and levelled from recordings on [Freesound](https://freesound.org)
released under **CC0 1.0** (public domain, <https://creativecommons.org/publicdomain/zero/1.0/>): no attribution is
required, and these edits are CC0 too, so they live in git. We credit the recordists anyway.

The source recordings, as downloaded on 2026-10-20, are on the shared Drive at `Raw\Audio\audio-src\doors` (set
`TARTARUS_AUDIO_SRC` to `Raw\Audio\audio-src` to rebuild). Format: 48 kHz, 16-bit, mono; each element is levelled to
-24 LUFS-M max like the `step_wood` takes, and `recipes/mix.json` (`levels.door`) sets the playback level.

Credits line: "Door sounds from Freesound (CC0) by SpliceSound, HowardV, EpicWizard, steeltowngaming, kyles, TRP and
MarcusGraham325."

## Sources

| ID | Recording | Recordist | License | Used in |
|---|---|---|---|---|
| 218324 | [Julian's Door - turn doorknob without latch](https://freesound.org/people/SpliceSound/sounds/218324/) | SpliceSound | CC0 1.0 | open (knob turn), latch |
| 393800 | [short creaks creek creaky door open close](https://freesound.org/people/HowardV/sounds/393800/) | HowardV | CC0 1.0 | open (hinge creak) |
| 345311 | [Cottage Wooden Doors - Assorted Squeaks and Creaks](https://freesound.org/people/EpicWizard/sounds/345311/) | EpicWizard | CC0 1.0 | open (hinge squeak) |
| 537737 | [Closet Door Close 01](https://freesound.org/people/steeltowngaming/sounds/537737/) | steeltowngaming | CC0 1.0 | close |
| 537736 | [Closet Door Close 02](https://freesound.org/people/steeltowngaming/sounds/537736/) | steeltowngaming | CC0 1.0 | close |
| 453960 | [door heavy office wood with metal handle close 3 times](https://freesound.org/people/kyles/sounds/453960/) | kyles | CC0 1.0 | close |
| 345312 | [Cottage Door Handles - Assorted Opening, Rattling and Closing](https://freesound.org/people/EpicWizard/sounds/345312/) | EpicWizard | CC0 1.0 | rattle |
| 577273 | [Door rattles, wood, 2011](https://freesound.org/people/TRP/sounds/577273/) | TRP | CC0 1.0 | rattle |
| 330829 | [Door Handle Rattling](https://freesound.org/people/MarcusGraham325/sounds/330829/) | MarcusGraham325 | CC0 1.0 | rattle |

Downloaded too but not used (kept on the Drive with the rest, all CC0 1.0): 505957 jedg "Door open close latch",
352328 / 352329 / 352330 MadMaxSFX "Wood Door Squeak" / "Wood Door Close with Metal Latch 01, 02" (noisy floor),
330828 MarcusGraham325 "Door Handle Click" (hum), 235473 jcdecha "Locked Door Handle Rattle" (very low level),
537740 steeltowngaming "Locking Metal Deadbolt on Wood Door" (a lock, kept for a future lock sound).

## Per file

| File | Key | Length | Cut from (source ID, seconds) |
|---|---|---|---|
| `close_1.wav` | `snd.foley.door.close` | 0.52 s | `537737` 0.42-1.42 s |
| `close_2.wav` | `snd.foley.door.close` | 0.56 s | `537736` 0.46-1.46 s |
| `close_3.wav` | `snd.foley.door.close` | 1.22 s | `453960` 0.37-1.67 s |
| `close_4.wav` | `snd.foley.door.close` | 1.27 s | `453960` 6.50-7.80 s |
| `latch_1.wav` | `snd.foley.door.latch` | 0.33 s | `218324` 1.97-2.32 s |
| `latch_2.wav` | `snd.foley.door.latch` | 0.29 s | `218324` 5.81-6.16 s |
| `latch_3.wav` | `snd.foley.door.latch` | 0.29 s | `218324` 9.83-10.13 s |
| `latch_4.wav` | `snd.foley.door.latch` | 0.30 s | `218324` 13.37-13.67 s |
| `open_1.wav` | `snd.foley.door.open` | 0.62 s | `218324` 0.25-0.61 s<br>`393800` 0.04-0.46 s |
| `open_2.wav` | `snd.foley.door.open` | 0.65 s | `218324` 4.01-4.33 s<br>`393800` 1.18-1.74 s |
| `open_3.wav` | `snd.foley.door.open` | 0.82 s | `218324` 8.20-8.50 s<br>`393800` 3.34-3.96 s |
| `open_4.wav` | `snd.foley.door.open` | 0.66 s | `218324` 12.69-12.94 s<br>`345311` 1.33-1.83 s |
| `rattle_1.wav` | `snd.foley.door.rattle` | 1.40 s | `345312` 6.49-7.89 s |
| `rattle_2.wav` | `snd.foley.door.rattle` | 1.34 s | `345312` 8.24-9.64 s |
| `rattle_3.wav` | `snd.foley.door.rattle` | 1.40 s | `577273` 2.17-3.57 s |
| `rattle_4.wav` | `snd.foley.door.rattle` | 1.40 s | `330829` 0.81-2.21 s |
