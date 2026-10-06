# Curve editor

Recoil, firing camera, weapon procedural and Animator weight curves use the same editor.
Double-click a curve preview or click **Edit Curve** to open its own floating, dockable window.
You can keep several windows open and continue editing after selecting another asset.
The channel list opens other curves from that asset in their own windows.

| Tool | Use |
|---|---|
| Fit / Fit Selected | Frame the curve or selected keys |
| Select All / Deselect | Select every key or clear selection |
| Key selection | Click, Ctrl-click for multiple keys, or drag a marquee |
| Key movement | Drag selected keys together; keys cannot cross neighbours |
| Add Key | Insert at an exact time/value; double-click the graph to insert on the curve |
| Copy / Cut / Paste at Playhead | Copy keys between curves, preserving relative times and slopes; Ctrl+C/X/V |
| Delete Keys | Remove selected keys; Delete also works |
| Auto / Flat | Compute smooth slopes or flatten selected cubic tangents |
| Linear / Constant | Set interpolation from each selected key to its next key |
| Unify / Break / Link Handles | Join slopes or edit incoming/outgoing slopes independently |
| Snap | Snap dragged keys on release to the chosen time/value steps |
| Transform | Offset and scale selected times/values, with slope scaling |
| Invert Values | Flip values and slopes |
| Play / time slider | Loop or scrub a playhead and read the evaluated value |
| Key List / numeric fields | Select keys by row and edit exact time, value and slopes |
| Presets | Flat, line, ease, kick, sine, smoothing and defaults when available |
| Undo / Redo | Global asset history, also Ctrl+Z / Ctrl+Y |
| Reload | Reload the file, discarding the current unsaved draft |

Right-drag pans; the wheel zooms around the cursor. Graph navigation and selection do not save.
Curve edits save on commit (drag release, numeric edit completion, or a toolbar action).
Saves update just that curve in the latest file, retaining other asset fields. Validation failures
leave the draft in the window with an error and **Save Curve** button; correct it before closing.

Recoil times are seconds; **Edit end (s)** extends their editable range. Weapon procedural, firing camera and Animator curves use normalized
time 0..1. Camera curves must start and end at zero. Existing four-value keys remain cubic;
legacy two-value Animator keys remain linear. An optional fifth key value stores interpolation
(0 cubic, 1 linear, 2 constant). The plotted shape and runtime evaluate these modes consistently.
