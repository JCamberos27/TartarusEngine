# Engine smoke / regression scenes

Version-controlled scenes for `TartarusEngine.exe --smoke-test` (audit issue #368 / TEST-101).

CMake stages this folder to `<exe dir>/assets/test-scenes/`. With no argument, `--smoke-test`
loads every `.json` here, renders 100 frames of each through the real per-frame path, and fails
the process (exit 1) if any scene fails to load, logs a new error, reports a new GL error, or
draws nothing (audit #356). CI runs it on every push.

Every scene is also round-tripped through the undo snapshot path (`SaveToString` →
`LoadFromString`) right after loading; it must not throw or change the entity count or the
asset library (#81). A scene whose file name contains `smoke_play` also runs Play → Stop twice.
Each scene's `_comment` field says exactly what it checks.

| Scene | Covers |
|---|---|
| `smoke_min` | Ground, one shadow-casting cube, a sun: PBR opaque path, cascaded shadows, procedural sky + IBL, culling. Also the exact triangle raycast used by editor picking (#116) |
| `smoke_lighting` | Directional + point + spot, each shadow-casting: clustered culling, all three shadow paths, SSAO seams, bloom |
| `smoke_shadow_budget` | 16 shadowed spots + 8 shadowed points at the budget ceilings (#110) |
| `smoke_stress` | ~64 boxes + 4 point lights: draw-call scaling, material sorting, cluster saturation |
| `smoke_materials` | One sphere per `Standard.shader` variant, plus the Inspector's material preview (#107); PNGs go to `%TEMP%/TartarusSmokeMaterialPreview/` |
| `smoke_uv_transform` | glTF `KHR_texture_transform` import (#113) |
| `smoke_motion_blur` | Motion blur reprojection with a moving camera (#162) |
| `smoke_sky_atmosphere`, `smoke_sky_night` | The physical sky by day and by night (moon light, stars, clouds) |
| `smoke_play` | Play-mode coverage (#368) |
| `smoke_play_parented` | A rigidbody under a rotated, offset parent; prints each body's world position (#114/#115) |
| `smoke_play_controller` | Play spawns the first-person player at its controller (#165) |
| `smoke_play_scene_camera` | No player but a Camera: Play renders through it (#165) |
| `smoke_play_friction` | Physics materials on a ramp (#170/#204) |
| `smoke_play_basketball` | Ball bounce and a solved free throw that must score (Sandbox basketball) |
| `smoke_play_particles` | Additive and alpha-blended particle systems (#177) |
| `smoke_play_spin` | Diagonal-axis spin through the Animator and Transform Controller (#123) |
| `smoke_prefab_mode` | Saves a prefab, places instances, edits it in Prefab Mode (#176) |
| `smoke_project_sync` | Asset files written and moved on disk are picked up (#132) |

`../smoke-scenes-invalid/` holds deliberately broken fixtures that are **not** staged next to
the exe; run them explicitly to confirm the harness reports failure and exits nonzero:

- `missing_model` — references a model file that does not exist (load-time asset error, #356).
- `parent_cycle` — `parentId` links form a cycle; proves `ComposeWorldTransform` bails and logs
  instead of overflowing the stack (audit BUG-202 / #371).
- `wrong_types` — valid JSON with wrong-typed fields; proves a malformed scene fails to load with
  a logged error instead of crashing the editor (#83).

```
TartarusEngine.exe --smoke-test path/to/tests/smoke-scenes-invalid
```

When a bug from an audit finding is fixed, add the reproduction here as a new scene.
