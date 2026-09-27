# Sky review tools

How the physical sky was tuned: render a fixed set of scenes before and after a change and
compare the screenshots side by side, without opening the editor.

| File | What it's for |
|---|---|
| `scenes/r1_morning.json` … `r7_storm.json` | Seven times of day and weather, on engine primitives only. |
| `render.sh <out> [scenes]` | Copies the shaders into the build, renders 3 shots per scene, and undoes the editor's `.meta` churn. |
| `compare.py out.png <runA> <runB> …` | Contact sheet: one column per run, one row per shot. |

Typical loop, from the repository root in Git Bash:

```sh
tools/sky-review/render.sh ../review/before
# edit shaders (or rebuild after C++ changes)
tools/sky-review/render.sh ../review/after
python tools/sky-review/compare.py ../review/cmp.png ../review/before ../review/after
```

- **The camera:** it sits at (0, 2, 12), so scene objects near there are in shot. To inspect a
  real scene such as `project/scenes/Sandbox.json`, copy it to a scratch folder, move the objects
  of interest in front of the camera, and render that folder.
- **Timing:** clouds drift with the wind between runs, so compare shapes loosely. The shots are
  taken after 30+ frames at each pose, once temporal accumulation has settled.
- **Performance:** `TartarusEngine.exe --perf-bench <scene-dir>` prints per-pass GPU times, such
  as `GPU Sky Atmosphere` and `GPU Volumetric Clouds`.
