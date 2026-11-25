#pragma once

#include <string>

class EditorLayer;
struct EditorSettings;

// What a harness run draws: always the normal look. The editor's draw mode (Shaded / Wireframe / Unlit / Normals /
// Cascades / Mip), F5's light views, F6's overlays and the other debug views persist in the user's prefs, and every
// harness run starts from a copy of them; the Game view follows the draw mode too. An Unlit pref once made every
// perf-bench measure without lighting. So each harness mode forces this at its start, logs it, checks it on every frame
// it measures or captures, and fails the run if it ever isn't (Log::Error + a failed scene / shot = non-zero exit).
// Nothing here writes the user's own prefs file: harness runs only ever save their scratch copy.
namespace HarnessView {

struct Mode {
    int Shading = 0;            // EditorLayer::ShadingMode index: 0 Shaded
    int LightView = 0;          // F5 light view (FKeyRow::LightView): 0 Off
    int Overlay = 0;            // F6 overlay (FKeyRow::Overlay): 0 Off
    bool PostFxOff = false;
    bool PlayDebugOverlay = false;
    unsigned PhysicsDebugDraw = 0;
    bool Colliders = false;
    bool BodyDebug = false;
};

// The view state as `shading` (the editor's draw mode) and `es` hold it.
Mode Read(int shading, const EditorSettings& es);
// Shaded, lit, no debug view or overlay.
bool Lit(const Mode& m);
// "Shaded, lit" or what isn't, e.g. "draw mode Unlit, F5 Albedo".
std::string Describe(const Mode& m);
// Puts `shading` and `es` in the Lit() state.
void Apply(int& shading, EditorSettings& es);

// The editor's views: Read / Apply on its draw mode and EditorSettings::Get(). Force prints "[tag] view: Shaded, lit"
// (and what it changed), and returns false with a Log::Error when the views still aren't lit afterwards.
Mode Current(const EditorLayer& editor);
bool Force(EditorLayer& editor, const char* tag);

} // namespace HarnessView
