Multi-draw copies now read their reflection probes and GI volume mask from a per-slot GPU buffer instead of per-batch uniforms. Those two leave the opaque batch key, so groups in different rooms share a batch.

**Batches per view (opaque multi-draw, census):** Forest play-max 1573 -> 153, play 1511 -> 149, edit 588 -> 88. Apartment Hallway 17-18 and LocomotionLab 1 are unchanged.

## What
- `GpuScene::ObjectState`: one 80-byte entry per slot in an SSBO at binding 38 (std430). It holds the GI mask's low/high words, the probe count, and two probes (centre + layer, half size + blend): the same values `ReflectionProbeArray::Bind` and `GIVolumes::SetMask` set.
  - It is one buffer, not a probe table plus indices, because the model fragment program already has 8 SSBO blocks, the GL minimum guarantee.
- `SceneViewPass::BuildBatches` writes each multi-draw copy's state when the view draws it, from the draw list (`DrawItem::GIMask`, `ReflectionProbeArray::Resolve`). A CPU mirror is compared per slot, so only changed slots upload (in runs, like the transforms).
- With slot state on, the batch key and the per-draw hook drop the probe key and GI mask. The openable-leaf flag (`uGIFlatL1`) stays in the key. The hook sets `uSlotState` and skips the probe and mask uniforms. At the end of the opaque pass it resets `uSlotState` on every program it set, binding each one for the write.
- Shaders:
  - `ModelVertex.glsl`: passes the copy's remapped slot as `flat out uint vSlot`.
  - `ModelFragment.glsl`: adds `SlotStateBlock`, and the probe loop now reads locals (uniforms or slot state).
  - `GIVolumes.glsl`: `SampleGI` reads the mask through `GI_MASK_LO/HI`, which default to the uniforms, so its 7 other includers are unchanged.
  - BloodVat compiles the model fragment with `_NO_SLOT_STATE` (its vertex program has no `vSlot`).
- `TARTARUS_NO_SLOT_STATE=1` restores per-batch binds. `TARTARUS_SLOT_STATE_LEAD=1` (A/B only) gives every copy its group lead's state.

## Look change
Jacob approved the look change with crops 2026-11-25.

Each copy now uses its own probe blend instead of its batch lead's. Copies in one instance group already shared the probe pair and the blend to 1/64, and the GI mask exactly, so the difference is at most that blend bucket. Pixel A/B at 1920x1080, 2 runs per mode:
- **Apartment Hallway:** LEAD vs OFF 0 px over 8. ON vs OFF 1 / 531 px over 8, against a run-to-run floor of 0-1341.
- **Forest:** not deterministic run to run (OFF vs OFF 26k-756k px differ). OFF vs LEAD and OFF vs ON both fall inside the ON-vs-ON floor.
- Crops at the biggest OFF/ON difference windows (forest border 1 and 2, house, hallway) look identical. The differing pixels there are dithered foliage, as in the noise.
- **Forest border 2:** some runs show fine stipple on the distant trees instead of blocky cut-out edges. That stipple is the LOD cross-fade dither (`Ign(gl_FragCoord)` vs `uLodFade`) caught mid-fade.
  - A mid-fade object is never batched (`Batchable`: `LodFade != 0`). It draws through the uniform path with `uSlotState` 0, so this change cannot alter it.
  - ON runs hit it more often only because their frames are faster: the 30-frame shot settle ends sooner than the 0.6 s fade.
  - Check: border 2 captured three times, 30 frames apart, 3 runs per mode. ON runs 1-2 stipple at the first capture (detail ratio 0.773 / 0.743) and are blocky 30 frames later (0.715 / 0.714), like every OFF capture (0.712-0.716).
  - Settled ON vs OFF in that window: 3564 px over 8, against 3231 for OFF vs OFF.
  - Crops: `15_fade_on1_border2` (ON, mid-fade), `16_fade_on1_border2b` (ON, settled), `17_fade_off1_border2b` (OFF, settled).
- **Harness note:** LOD fades run on wall-clock time (`World::kLodFadeSeconds`, 0.6 s), but `--smoke-shots` captures a fixed 30 frames after each camera move. Whether a fade is still running at capture therefore follows each run's frame times. This is likely a large part of Forest's run-to-run pixel floor; the harness fix is tracked separately.

## Tests
- Unit tests: 569 run, 0 failures. New: `GpuScene: slot object state = what the per-draw hook binds`.
- smoke_min PASS. Forest and Hallway `--perf-bench` with GPU cull verify: 0 missing.
- Timed Forest ON/OFF/ON2 + Hallway pair, below.

## Timing
Quiet machine, `--perf-bench` 1920x1080, ON / OFF (`TARTARUS_NO_SLOT_STATE=1`) / ON again, avg ms unless noted:

| Forest | ON | OFF | ON 2 |
|---|---|---|---|
| Draws, play | 217 | 1662 | 217 |
| CPU Scene: Opaque, play | 0.70 | 2.67 | 0.60 |
| CPU Scene: Opaque, play-max | 0.61 | 2.53 | 0.57 |
| CPU Scene Draw, play | 3.98 | 5.49 | 3.50 |
| CPU Scene Draw, play-max | 3.36 | 5.01 | 3.21 |
| CPU Build Batches, play | 0.56 | 0.45 | 0.49 |
| GPU Opaque, play | 2.58 | 3.57 | 2.51 |
| GPU Opaque, play-max | 4.60 | 5.38 | 4.53 |
| GPU Depth Pre-pass, play-max | 2.03 | 1.98 | 1.99 |
| cpu.frame p50/p95, play | 11.87/18.47 | 13.08/17.20 | 11.06/13.72 |
| cpu.frame p50/p95, play-max | 10.62/11.59 | 11.22/13.54 | 10.41/11.05 |
| gpu.frame p50/p95, play-max | 10.63/11.21 | 11.24/11.81 | 10.41/10.91 |

Apartment Hallway (17 batches either way): CPU Opaque 0.19 / 0.19 ms (play), cpu.frame p50 play-max 9.14 / 9.25 ms. No change, as expected.

## Dead code removed
None.
