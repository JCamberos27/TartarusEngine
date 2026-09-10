# Engine smoke / regression scenes

Version-controlled scenes for `TartarusEngine.exe --smoke-test` (audit issue #368 / TEST-101).

CMake stages this folder to `<exe dir>/assets/test-scenes/`. With no argument, `--smoke-test`
loads every `.json` here, renders 100 frames of each through the real per-frame path, and fails
the process (exit 1) if any scene fails to load, logs a new error, reports a new GL error, or
draws nothing (audit #356).

- `smoke_min` — ground + one shadow-casting cube + a directional light. PBR opaque path,
  cascaded sun shadows, procedural sky + IBL, frustum culling.
- `smoke_lighting` — directional + point + spot, each shadow-casting, over creased geometry.
  Clustered light culling, all three shadow-map paths, SSAO seams, bloom highlight.
- `smoke_stress` — ~64 primitive boxes + 4 point lights. Draw-call scaling, material-sorted
  draw list, cluster saturation, culling at volume.

`../smoke-scenes-invalid/` holds deliberately broken fixtures that are **not** staged next to
the exe; run them explicitly to confirm the harness reports failure and exits nonzero:

- `missing_model` — references a model file that does not exist (load-time asset error, #356).
- `parent_cycle` — `parentId` links form a cycle; proves `ComposeWorldTransform` bails and logs
  instead of overflowing the stack (audit BUG-202 / #371).

```
TartarusEngine.exe --smoke-test path/to/tests/smoke-scenes-invalid
```

When a bug from an audit finding is fixed, add the reproduction here as a new scene.
