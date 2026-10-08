# Editor interaction performance

### Editor interaction stalls (2026-10-07)

The global undo input handler serialized the entire scene on mouse/key activity and again
when the interaction ended. Right-click camera movement, WASD and scrollbar activation could
therefore pay two full snapshot costs despite changing no authored content. This was a CPU
stall independent of the renderer's steady-state fps.

History now caches the last committed authored scene and starts scene transactions from
actual edit requests. This also preserves controls that request undo after changing a value.
Navigation performs no scene serialization; file-only and selection entries reuse the delta
chain anchor, and equal anchors no longer spawn a diff worker. Commits refresh the cached
scene; scene/history/prefab changes and external project updates refresh its context.

Regression check: `TartarusEngine.exe --unit-tests "Editor navigation undo"`.
On this machine, a synthetic 1,500-empty-object scene's two saves took **20–25 ms** across
repeated runs; the new navigation/history test averaged **0.0016–0.0018 ms** per frame over 120 frames with **zero scene
snapshots**. This measures the isolated input/history path, not full rendered frame time or
the user's scene. The check also covers post-change value edits, multi-frame drag coalescing,
unchanged activation, selection, file-only/mixed transactions and undo/redo. Release build
and 11 relevant tests passed (2,755 checks).

**Undo Scene Snapshot** profiler scopes now identify cache initialization and scene-edit
commit costs. Real edits can still serialize a large scene, and thumbnail generation,
first-use GPU resources and imports can still cause separate stalls. Recheck live scrolling,
selection and camera movement with the rebuilt editor before treating all hitches as resolved.


The profiler and `--perf-bench` still measure native CPU/GPU scopes.
