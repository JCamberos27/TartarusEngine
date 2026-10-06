# ImCurve / Tartarus adaptation

Source: https://github.com/jsoulier/imcurve

Pinned upstream commit: `8cc8dc3a971593c6ce5aa8f418776545b2dbc3bb`.
Upstream license: Unlicense, retained in `LICENSE.txt`.

This is a local fork of the upstream two-header editor, not a network build dependency.
Local changes support Tartarus's C++17 toolchain and cubic Hermite slopes with independent
incoming/outgoing handles. The canvas is editable inline, uses the active ImGui theme and
font size, includes bounds on zoom and key movement, and retains at least one key on deletion.
Per-curve history, multi-selection, marquee selection, cursor-relative zoom and panning derive
from the upstream editor. Fit/history/synchronization APIs serve the engine adapter.

Upstream curve persistence in `imgui.ini` is deliberately removed: engine asset files are the
only authority. `src/Editor/ImCurveAdapter.h` converts without modifying coefficients or the
existing JSON format. `src/Editor/CurveEditor.cpp` handles asset edit commits, presets, numeric
fields, and context-scoped sessions. External asset reloads reset local curve history.
