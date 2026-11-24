# W5 Kronos: queued GPU work (prep only, nothing run)

Saved 2026-11-24 ~20:10 UTC on DESKTOP-GQKO56V so a later start (likely Jacob's home PC, new sessions) can pick it up.
The team folder (`D:\Jacob\tblocks\descent\`) is not in git; this folder holds the scripts from it.

**main is now 02c4a63d (squash of #1213).** Branches made before the squash merge main first.
main already has the tent PCF (`ModelShading.glsl` is identical to dev/fix3-tent-pcf 471d1673) and GI Fade A.

## Paths
The scripts hard-code the desktop-gqko56v layout. On another PC, edit the variables at the top of each script:

| variable | here | meaning |
|---|---|---|
| `$g` | `D:\Jacob\tblocks\descent\wt-gpu` | GPU runner worktree, with `build\Release\TartarusEngine.exe` built at the sha under test |
| `$out` / `$k` | `...\gi-p2\kronos\famE`, `...\gi-p1\kronos` | output folders; the famE scripts must sit in `$out` |
| `locks\perf` | `D:\Jacob\tblocks\descent\locks\perf\owner.txt` | one GPU job at a time; `famE_ab.ps1` refuses to start unless the owner names W5 |
| sweep tour | `house-polish\briareos\sweep\sweep_tour_open.json` | a copy is in `famE/` (439 waypoints, doors all open) |

Rules: hidden + BelowNormal (`TARTARUS_HIDDEN=1`), never while Jacob's editor runs, D: floor 100 GB free. Perf numbers are
labelled "DESKTOP-GQKO56V, RAM 2133 (XMP off)"; any other PC needs its own A1/A2 noise floor.

## Queue (in order, each on the SM's GO)

### 1. Perf A/B: tent PCF + GI Fade A vs without (FIRST after the next start)
No script yet: build it from the `famE_ab.ps1` pattern.
- **Arms:** A = main (tent PCF + Fade A). B = the same exe and scene with both off. Prefer a runtime toggle, or else a
  second exe built at the parent without them (same compiler, same flags). Say which in the report.
- **Run:** interleaved A, B, A, B (>= 2 rounds), same tour, `--perf-res 1920x1080`. Drop the first 30 `settle` frames
  after each camera cut. Report per-view median `gpu_total_ms` / `gpu_opaque_ms`, B-A against the A1-vs-A2 spread.
- **Views:** the living-room sweep plus the facade/porch views (as famE), hall wedge (35594/35596), kitchen chandelier
  near the ceiling corner, den, and the landing.
- **Slot overflow:** from `tour_lights.csv`, count shadowed spots and points per view versus the budget (spot budget 8),
  and list the lights that miss a slot. This must be in the report, not only frame time.
- The PM's bar: 12 point lights with PCF must fit the frame budget.

### 2. famE A/B (`famE/`)
`powershell -NoProfile -File famE_ab.ps1 [-Repeats 2] [-TimeoutMin 10]`, run from `$out`. About 10 min of GPU time.
- A = wt-gpu's `Forest.json` as checked out. B = A + ONLY Atlas's family E from `ced5d6b7`
  (origin/dev/fix3-jambs): adds unshadowed strip fills 38103/38104/38105 and swaps in the trimmed 36392/36394/35609.
  The jamb Bounds edits on that branch are left out.
- `famE_make.py` builds `Forest_famE_A/B.json` (untracked, deleted at the end) and `tour_A/B.json`: 19 views =
  `jlook-facade-*`, `jlook-porch-front`, `jlook-jamb-35614_east`, `living-room_*`. Guards: it asserts that no id or
  `order` clashes (on 2026-11-24 main had none of 38103-38108).
- `famE_ab.py` writes `ab.md`: per-view frame time B-A vs the A1/A2 spread, and a spot-slot holder table per view (the
  input for item 4).
- Needs `ced5d6b7` fetched in wt-gpu (`git -C $g fetch origin dev/fix3-jambs`). Variant C (spot 36370 shadows) was dropped.

### 3. Bridges incremental bake: APPROVED (Jacob saw the seams)
On `dev/look-bridges` e079c65f (4 bridge GI volumes, ids 38106-38108 + 38112). Merge main into it first (squash).
```
& 'D:\Jacob\tblocks\descent\gi-p1\kronos\run-bake.ps1' -Tag bridges -Only 'GI bridge' -TimeoutMin 10
```
(the script is in `bake/` here). Before running: `git -C $g diff --stat <built sha> <bake sha> -- src tools CMakeLists.txt cmake`
must be empty, else rebuild wt-gpu. The script writes `Forest.bridges.gi` + summary next to itself and restores wt-gpu's
tracked `Forest.gi`. It refuses to run if another TartarusEngine is up (exit 6), D: < 100 GB (3), or Forest.gi is
already modified (5).

### 4. Street-lamp shadow-slot proposal (FOUND_ISSUES 2026-11-24 17:55)
- **Finding:** indoors all 8 spot slots are in use: hall 2 + kitchen 3 + upstairs bath 1-2 + 1-2 street lamps. The lamp
  slots are wasted, since no lamp reaches an interior view, and downstairs-bath views push one upstairs-bath spot out.
  About 13 lamps never get a slot.
- **Lamps** (child lights "Lamp Light", local pos (0,-0.12,0.25)): 34849, 34870, 34883, 34904, 34925, 34934, 34947,
  34968, 34977, 34990, 35011, 35020, 35041, 35054, 35083.
- **Steps:** get the outdoor slot use from the famE facade/porch views (`ab.md` slot table). Then propose (1) Cast
  Shadows OFF on lamps that never win a slot outdoors, or (2) indoor-first priority in `ShadowSlotScoring`, with an
  outdoor look at each option (GPU on SM GO).

## Tent PCF (shipped in main)
4x4 binomial grid of bilinear hardware taps, spacing <= 1 texel, with a centre-tap early-out. Points and spots share
`ShadowPlanePCF`. The **kernel cap is 2.5 texels for points AND spots** (`PointShadowMap::KernelTexels`;
point Softness 1.3 is already capped, about 1.2 cm radius at 2.5 m). Raising the cap (Talos asked for about 3 Softness on
the master-ceiling diagonal 35611) needs a bigger filter (5x5, VSM or PCSS: T1 engine work) or a scene fix (fill light,
shadows off, or moving the light).
Open follow-ups: Argus's spot VP-projection hoist; tent look gaps (#2 bathroom-doorway wall blotch, thin caster
close-up, point near a cube corner).
