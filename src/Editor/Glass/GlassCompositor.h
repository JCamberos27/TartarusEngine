#pragma once
// Black glass for every floating ImGui window: popups, menus, combo lists, context menus, tooltips,
// modals, undocked windows and the viewport overlays (the action bar, the tool rail, the measure
// HUD). Things that float are glass; docked panels stay solid. GlassTokens.h has the material.
//
// ImGui has no blur, so the compositor renders the frame's draw lists itself (in place of the one
// ImGui_ImplOpenGL3_RenderDrawData call in EditorLayer::EndFrame), back to front:
//
//   for each draw list:
//     if it belongs to a glass surface:
//       render everything queued so far          (the framebuffer now holds what is behind it)
//       if anything drawn since the last capture overlaps it:
//         copy the framebuffer and blur it       (GlassBackdrop: a 6-level compute pyramid)
//       draw the glass plate under the window    (the blur sampled under its exact rect and
//                                                 rounding, then the 25% black tint)
//     queue the draw list
//
// so a menu over the Scene view shows the scene blurred, a menu over a panel shows the panel
// blurred, and a submenu over its parent menu shows that menu blurred under it, as on the
// Windows desktop. Captures are shared: two glass surfaces that don't overlap anything drawn
// between them (the action bar and the tool rail, say) blur once.
//
// The window's own solid colours are removed from its finished vertices first (its WindowBg or
// PopupBg and title bar become transparent, its hairline border becomes the 15% white edge), so
// nothing else in the editor has to know about glass. Where the compositor doesn't run (a window
// dragged out to its own OS window) those solid colours are the fallback, and look correct.
//
// It also plays the arrival motion (GlassTokens.h, category 4): glass surfaces ease in and catch
// the light on their rim, docked panels fade up when their tab is switched to. The motion is
// applied to the same finished vertices, so it covers every window without any panel's code
// changing, and holds no state once settled (Glass::Motion pulses, GlassMotion.h).
//
// Host-only, Glass theme only (EditorLayer::m_Glass owns one).

#include "GlassBackdrop.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <memory>
#include <unordered_set>
#include <vector>

namespace Glass {

// --- Pure helpers (no GL; unit-tested) ---------------------------------------------------------------

// The colour ImGui filled `window`'s background with, read back from its finished draw list: the
// background is the first thing Begin draws, so it is the first vertex, and it starts on the
// window's left edge. 0 when the window drew none (NoBackground, or a zero background alpha).
ImU32 FindWindowBackground(const ImDrawList* dl, const ImRect& windowRect);

// Turns a floating window's solid look into glass, in place: every vertex coloured exactly one of
// `clear` (its background and title bar) becomes transparent, and every vertex coloured `border`
// (rgb, any alpha) takes `edge`'s colour at `edge`'s alpha times its own. 0 entries and a 0 border
// are skipped. Returns how many vertices were cleared.
int ConvertToGlass(ImDrawList* dl, const ImU32* clear, int clearCount, ImU32 border, ImU32 edge);

// Turns the Tartarus panels' own solid fills into glass, in place. Their surfaces (component
// headers, buttons, rows, hairlines) are opaque near-blacks the panel code picks itself, so they
// would sit on the glass as black slabs. Every opaque grey vertex at or below #2C becomes glass
// instead: black (#00-#06) a light black veil, raised surfaces (#07-#14) dark glass that stands
// apart from the panel, hover (#15-#20) and pressed / hairlines (#21-#2C) white lifts. Grey text
// and icons (GreyTextMin-GreyTextMax) are lifted by LiftGreyText so they read over a bright field.
// Anything with hue, primary text and anything already translucent is left alone. Returns how
// many vertices changed.
int GlassifySolids(ImDrawList* dl);

// Grey text on glass: Tartarus's Dim (#6A) and Secondary (#9C) sit on black, and over the lighter
// glass they lose contrast. The compositor maps that band up into #A4-#C8, keeping its order.
constexpr int GreyTextMin = 0x50;
constexpr int GreyTextMax = 0xB8;
constexpr int LiftGreyText(int v) { return 0x9C + (v - GreyTextMin) / 2; }

// A surface's arrival pose: scaled about `pivot`, then moved by `offset`, at `alpha` opacity.
struct SurfacePose {
    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    float  scale = 1.0f;
    ImVec2 offset = ImVec2(0.0f, 0.0f);
    float  alpha = 1.0f;
    bool   moveClip = true; // false: content moves inside its clip rects (a docked panel's body)
    bool Identity() const { return scale == 1.0f && offset.x == 0.0f && offset.y == 0.0f && alpha >= 1.0f; }
    ImVec2 Apply(ImVec2 p) const {
        return ImVec2(pivot.x + (p.x - pivot.x) * scale + offset.x, pivot.y + (p.y - pivot.y) * scale + offset.y);
    }
    ImRect Apply(const ImRect& r) const { return ImRect(Apply(r.Min), Apply(r.Max)); }
};

// Moves a finished draw list's vertices (and clip rects, if the pose says so) into `pose`, and fades
// its colours. Vertices coloured exactly `anchored` (non-zero: a panel's own background) stay put
// and opaque, so only the content arrives.
void ApplyPose(ImDrawList* dl, const SurfacePose& pose, ImU32 anchored = 0);

// The screen area a draw list can touch: the union of its non-empty commands' clip rects (the
// renderer scissors to them). Empty when it draws nothing.
ImRect DrawListBounds(const ImDrawList* dl);

// How a window takes part: glass (and which arrival), part of a glass surface, a docked panel, or
// neither (the menu bar, the status bar, overlays with no background).
enum class SurfaceKind : unsigned char { None, Popup, Tooltip, Window, Hud, Panel };

// --- The compositor ----------------------------------------------------------------------------------

class GlassCompositor {
public:
    GlassCompositor();
    ~GlassCompositor();
    GlassCompositor(const GlassCompositor&) = delete;
    GlassCompositor& operator=(const GlassCompositor&) = delete;

    // Renders `dd` (the main viewport's draw data, after ImGui::Render) to the bound framebuffer
    // (FBO 0) with glass under every floating window. `blur` false (Preferences > Frosted blur
    // off) paints GlassFallback under them instead and never touches the blur. Must run with the
    // ImGui context that produced `dd` current.
    void Render(ImDrawData* dd, bool blur);

    // Frees the GL objects (capture target and blur chain). The next Render recreates them.
    void Release();

    // Last frame's numbers, for the tests and the Statistics panel.
    int GlassSurfaces() const { return m_StatSurfaces; }
    int Captures() const { return m_StatCaptures; }

    static constexpr int kMaxCapturesPerFrame = 6; // past this, later surfaces reuse the last blur

private:
    struct ListInfo {
        ImGuiWindow* Window = nullptr;
        ImGuiWindow* Group = nullptr; // whose arrival pose this list follows (null = none)
        SurfaceKind  Kind = SurfaceKind::None;
        bool         Plate = false;   // draw a glass plate under this list
        bool         Overlay = false; // a Scene view overlay (HUD plate colour): chrome glass, no shadow
        ImRect       PlateRect;
        float        PlateRounding = 0.0f;
        float        Alpha = 1.0f;
    };

    SurfacePose PoseFor(ImGuiWindow* group, SurfaceKind kind, bool appeared, float* rimBoost);
    void Capture(int w, int h);
    void Flush(ImDrawData* dd, std::vector<ImDrawList*>& pending, bool& texturesSent);

    GlassBackdrop m_Blur;
    unsigned int m_CaptureTex = 0, m_CaptureFbo = 0; // RGBA8 copy of the framebuffer, blur source
    int m_CaptureW = 0, m_CaptureH = 0;
    unsigned int m_BlurTex = 0;
    std::vector<std::unique_ptr<ImDrawList>> m_Plates; // reused across frames
    std::unordered_set<ImGuiID> m_Seen, m_SeenNext;      // surfaces drawn last frame / this frame
    int m_StatSurfaces = 0, m_StatCaptures = 0;
};

} // namespace Glass
