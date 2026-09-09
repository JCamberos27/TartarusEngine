#include "EditorLayer.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "Screenshot.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Texture.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "LayerRegistry.h"
#include "ProjectSettings.h"
#include "Shortcuts.h"
#include "GLStateCache.h"
#include "PhysicsWorld.h" // #185 — the Physics debug panel + HUD read live sim state
#include "gl.h" // DrawEngineMark reads back a patch of the scene texture for its contrast-adaptive tint
#include "ScreenBlur.h"
#include "Framebuffer.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder* — only used once, to lay out the default dock tree on first run
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <ImGuizmo.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505) // ImViewGuizmo.h defines a couple of static helpers this TU doesn't call
#endif
#include <ImViewGuizmo.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <IconsFontAwesome6.h>
// One glyph = one meaning (#161). Where two unrelated actions used to share a picture:
//   Scale tool            ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER  (was EXPAND — clashed with
//                                                                     Frame All / Fullscreen)
//   View > Frame All      ICON_FA_MAGNIFYING_GLASS                    (pairs with Frame Selected's
//                                                                     magnifier-plus)
//   Fullscreen button     ICON_FA_EXPAND / ICON_FA_COMPRESS          (now the only EXPAND user)
//   Wireframe shading      ICON_FA_BORDER_NONE                        (was VECTOR_SQUARE — clashed
//                                                                     with the Rect tool)
//   Rect tool              ICON_FA_VECTOR_SQUARE                      (kept)
//   View > Iso             (no glyph — matches its text-only sibling view items)
//   Gizmo Local / World    ICON_FA_ARROWS_TO_DOT / ICON_FA_GLOBE      (was CUBE — clashed with
//                                                                     Box Collider / primitive cube)
//   Light Gizmos toggle    ICON_FA_CIRCLE_NODES                       (LIGHTBULB reserved for the
//                                                                     light entity / component)
//   Empty entity           ICON_FA_DIAGRAM_PROJECT                    (everywhere — Add menu,
//                                                                     Hierarchy, Inspector, viewport)
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // extractEulerAngleYXZ — must match ComposeTransform's order (#108)

#include <filesystem>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

#include "EditorLayerInternal.h"

using namespace EditorInternal;

EditorLayer::EditorLayer() = default;
EditorLayer::~EditorLayer() = default;

void EditorLayer::Init(GLFWwindow* window) {
    m_Window = window;

    // Editor preferences (currently just the tooltip toggle) are independent of any scene, so
    // they're loaded once here rather than as part of scene load/save.
    EditorSettings::Load();
    LayerRegistry::Load(); // slot names for LayerComponent (#236 A1); project/layers.json
    ProjectSettings::Load(); // physics + tag vocabulary (#236 A4); project/settings.json
    Shortcuts::Init(); // builtin key table + project/shortcuts.json overrides (#236 F)
    LoadAssetFavorites(); // project/asset_favorites.json (#236 G)

    // Authored content lives in the project folder, not the working directory (build/Release/)
    // — see ProjectPaths.h. Must match main.cpp's initial load: prefer the last-open scene if
    // it still exists, else the built-in default (#95).
    {
        std::error_code ec;
        const std::string& last = EditorSettings::Get().LastScenePath;
        m_CurrentScenePath = (!last.empty() && std::filesystem::exists(last, ec) && !ec)
            ? last : ProjectPaths::Resolve("scenes/Showcase.json");
    }

    // So Import / Open / Save dialogs start in the project folder instead of build/Release/,
    // then follow the user around from there (#15 P4).
    FileDialog::SetDefaultDirectory(ProjectPaths::Root());

    // Crash recovery: if the auto-save timer wrote a recovery snapshot in a previous session
    // that never got an explicit Save afterward, that file is now newer than the scene file
    // (which a clean exit would have re-saved, then deleted the snapshot). Note it here; Draw()
    // raises the Restore/Discard modal on the first editor frame, once ImGui + the World exist.
    {
        std::error_code ec;
        const std::string recoveryPath = RecoveryPathFor(m_CurrentScenePath);
        if (std::filesystem::exists(recoveryPath, ec) && !ec) {
            const bool sceneExists = std::filesystem::exists(m_CurrentScenePath, ec);
            auto recT = std::filesystem::last_write_time(recoveryPath, ec);
            if (!ec) {
                auto sceneT = sceneExists ? std::filesystem::last_write_time(m_CurrentScenePath, ec)
                                          : std::filesystem::file_time_type::min();
                if (!ec && (!sceneExists || recT > sceneT)) m_RecoveryPromptPending = true;
            }
        }
    }

    // Read the monitor's content scale (1.0 at 96 DPI, 2.0 at Windows' 200% scaling, which is
    // the common default on 4K displays) once at startup, and bake it into font pixel sizes and
    // the layout constants below rather than relying on ImGui's blurry FontGlobalScale — so the
    // UI reads at a consistent physical size instead of shrinking to illegible on a 4K monitor.
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    m_UIScale = xscale > 0.0f ? xscale : 1.0f;

    // Manual override (Preferences > General > "UI scale"). A project authored on a 4K panel at
    // 200% Windows scaling and then opened on a plain 1080p monitor gets m_UIScale 1.0 and the
    // whole editor reads half the physical size it used to — this lets the user pin it back
    // (0 = keep following the monitor). Clamped to the same range the slider offers.
    if (EditorSettings::Get().UiScaleOverride > 0.0f) {
        m_UIScale = std::clamp(EditorSettings::Get().UiScaleOverride, 0.75f, 2.5f);
    }

    // Asset Browser tree width / icon size: restore the user's last size, or fall back to a
    // roomy DPI-scaled default (folder names like "Chesterfield Sofa" fit without a manual drag,
    // and thumbnails start Large rather than as tiny 32px chips).
    {
        const EditorSettings& prefs = EditorSettings::Get();
        m_AssetTreeWidth = prefs.AssetBrowserTreeWidth > 0.0f ? prefs.AssetBrowserTreeWidth
                                                              : 230.0f * m_UIScale;
        m_AssetIconSize = prefs.AssetBrowserIconSize > 0.0f ? prefs.AssetBrowserIconSize
                                                            : 96.0f * m_UIScale;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Editor style — metrics (rounding / padding / borders) + palette, DPI-scaled. Bento (0) and
    // Prism (1) use the rounded-card metrics; Windows XP (2) keeps the compact baseline.
    // ApplyThemeStyle owns that split and the one-time ScaleAllSizes; re-run it live from
    // Preferences on a theme change. (#92, #234)
    ApplyThemeStyle();
    ImGuiStyle& style = ImGui::GetStyle();

    // ImGuizmo palette. Its stock plane-drag squares are the R/G/B axis colours at 38% alpha,
    // so PLANE_X reads as an off-palette pink/salmon over the dark viewport (audit #84). Give
    // all three plane handles one neutral amber instead, brighten the axis lines slightly so
    // they don't muddy against dark geometry, and point SELECTION at the editor accent so a
    // hovered handle matches the rest of the UI.
    {
        ImGuizmo::Style& gz = ImGuizmo::GetStyle();
        gz.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.86f, 0.24f, 0.28f, 1.00f);
        gz.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.35f, 0.78f, 0.30f, 1.00f);
        gz.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.28f, 0.52f, 0.92f, 1.00f);
        // Keep plane drag targets functional but unfilled: opaque/dark squares beside the
        // arrows made the compact gizmo look boxy and obscured nearby level geometry.
        gz.Colors[ImGuizmo::PLANE_X] = ImVec4(0.95f, 0.78f, 0.25f, 0.00f);
        gz.Colors[ImGuizmo::PLANE_Y] = ImVec4(0.95f, 0.78f, 0.25f, 0.00f);
        gz.Colors[ImGuizmo::PLANE_Z] = ImVec4(0.95f, 0.78f, 0.25f, 0.00f);
        gz.Colors[ImGuizmo::SELECTION] = ImVec4(1.00f, 0.55f, 0.10f, 0.60f);
        // A compact, sturdy manipulator is easier to read against level geometry than
        // ImGuizmo's thin default strokes. These are screen-space values, so follow the UI DPI.
        gz.TranslationLineThickness = 7.0f * m_UIScale;
        gz.TranslationLineArrowSize = 11.0f * m_UIScale;
        gz.ScaleLineThickness = 6.0f * m_UIScale;
        gz.ScaleLineCircleSize = 8.0f * m_UIScale;
        gz.CenterCircleSize = 9.0f * m_UIScale;
        gz.RotationLineThickness = 4.5f * m_UIScale;
        gz.RotationOuterLineThickness = 5.5f * m_UIScale;
    }

    // UI text font. Loaded straight from the Windows system font directory rather than
    // bundled into the repo (Segoe UI is Microsoft-licensed, not ours to redistribute).
    // Falls back to ImGui's built-in bitmap font if it's ever missing (e.g. running under
    // Wine, or a stripped-down Windows install), so this never hard-fails.
    // Baked at the monitor's content scale (not left at 1x + FontGlobalScale) so text stays
    // crisp instead of blurry-upscaled on high-DPI/4K displays.
    const float baseFontPx = 16.0f * m_UIScale;
    ImFontConfig baseFontConfig;
    baseFontConfig.SizePixels = baseFontPx;
    ImFont* uiFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", baseFontPx, &baseFontConfig);
    if (!uiFont) {
        ImFontConfig fallbackConfig;
        fallbackConfig.SizePixels = 15.0f * m_UIScale;
        io.Fonts->AddFontDefault(&fallbackConfig);
    }
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = baseFontPx;
    io.Fonts->AddFontFromFileTTF("assets/fonts/fa-solid-900.ttf", baseFontPx, &iconConfig, iconRanges);

    // CJK fallback: Segoe UI has no CJK glyphs, so entity names with Chinese/Japanese/Korean
    // text rendered as tofu boxes in the Hierarchy and Inspector (#50 P33). Merge a system CJK
    // face over the Japanese range (Kana + ~2000 common Kanji, which also covers most everyday
    // Simplified Chinese). Loaded from the Windows font dir like Segoe UI above; silently skipped
    // if absent (Wine / stripped install), so this never hard-fails.
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true;
    cjkConfig.PixelSnapH = true;
    const char* kCjkFonts[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",     // Microsoft YaHei (Simplified Chinese)
        "C:\\Windows\\Fonts\\msgothic.ttc", // MS Gothic (Japanese)
        "C:\\Windows\\Fonts\\malgun.ttf",   // Malgun Gothic (Korean)
    };
    for (const char* path : kCjkFonts) {
        if (io.Fonts->AddFontFromFileTTF(path, baseFontPx, &cjkConfig, io.Fonts->GetGlyphRangesJapanese())) break;
    }
    // NB: colour emoji (Segoe UI Emoji is COLR/CPAL) needs the FreeType backend with colour
    // glyphs enabled, which this build doesn't compile in — emoji in names still render as tofu.

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 460");

    // Wordmark removed in the #92 UI pass — only the corner monogram remains as branding.

    m_MarkTexture = std::make_unique<Texture>("assets/branding/tartarus_engine_mark.png");
    if (!m_MarkTexture->IsValid()) m_MarkTexture.reset();
}

// The editor's colour palette. All themes share the sizes/rounding set in Init(); this only
// writes style.Colors[], so Preferences can swap it live with no font/size rebuild.
//   0 Dark Slate — monochrome greys + one desaturated cool-slate accent (the #92 default).
//   1 Prism      — near-black backgrounds; the accent, buttons, text tint and tab keyline are
//                  all hue-driven, spread across ~half the wheel and drifting through the
//                  spectrum together every frame (ApplyPrismAnimation, phase advanced in Draw()).
//   2 Windows XP — the Luna "Blue" scheme: #ECE9D8 beige-grey chrome, black text, white input
//                  fields, and a Luna-blue selection/accent. Static, like Dark Slate.
// The metric baseline (raw 96-DPI values; ScaleAllSizes applies the monitor scale afterwards).
// Bento (0) and Prism (1) override a handful of these in ApplyThemeStyle for the rounded-card
// look; Windows XP (2) keeps this baseline.
static void SetSharedMetrics(ImGuiStyle& style) {
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;   // buttons, inputs, combos, checkboxes — the most visible one
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.PopupRounding = 4.0f;   // menus and tooltips
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(9.0f, 7.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 9.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
}

// One entry point for "apply the active theme" — colours (ApplyEditorTheme) AND metrics, then a
// single DPI scale. A full reset to the un-scaled baseline first means switching themes live
// (Preferences) never leaks Bento's larger radii into another theme or double-scales anything.
void EditorLayer::ApplyThemeStyle() {
    // The clean baseline: ImGui's own dark defaults + our shared metrics, captured once before
    // any DPI scaling. Non-Bento themes only override *some* Colors[], so they rely on this
    // baseline being the same one they were authored against.
    static bool s_haveBase = false;
    static ImGuiStyle s_baseStyle;
    if (!s_haveBase) {
        ImGui::StyleColorsDark(&s_baseStyle);
        SetSharedMetrics(s_baseStyle);
        s_haveBase = true;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style = s_baseStyle;

    // Bento (0) and Prism (1) share the SaaS-dashboard geometry; Windows XP (2) keeps the baseline.
    if (EditorSettings::Get().EditorTheme <= 1) {
        // SaaS-dashboard geometry: rounded cards, more air.
        style.WindowRounding    = 8.0f;
        style.ChildRounding     = 8.0f;
        style.PopupRounding     = 8.0f;
        style.FrameRounding     = 6.0f;
        style.TabRounding       = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding      = 12.0f; // pill sliders
        style.WindowPadding     = ImVec2(14.0f, 12.0f);
        style.FramePadding      = ImVec2(9.0f, 5.0f);
        style.ItemSpacing       = ImVec2(9.0f, 8.0f);
        style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
        // ChildBorderSize stays 0 until the opt-in card treatment (#234 layer 3) — with a
        // transparent ChildBg, a blanket 1px border just outlines every nested BeginChild.
        style.ChildBorderSize   = 0.0f;
        style.FrameBorderSize   = 0.0f;
        style.WindowBorderSize  = 1.0f;
        style.TabBarBorderSize  = 1.0f;
        style.SeparatorTextBorderSize = 1.0f;
    }

    ApplyEditorTheme();
    style.ScaleAllSizes(m_UIScale);
}

// Bento (dark SaaS-dashboard) palette — the default theme, and the static base Prism drifts on
// top of. Layered charcoal surfaces (window -> recessed input), hairline borders, and a
// disciplined accent split: cyan = selection / "you are here", blue = active / pressed, yellow =
// warning (a convention for host-drawn warning text — almost none of it is a style.Colors[] role;
// see docs/CONVENTIONS.md). Geometry (larger radii + padding) is set by ApplyThemeStyle. #234.
static void ApplyBentoPalette(ImGuiStyle& style) {
    auto rgb = [](int r, int g, int b, float a = 1.0f) {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    };
    const ImVec4 cyan   = rgb(61, 214, 208);        // #3DD6D0 — selection
    const ImVec4 cyanHi = rgb(61, 214, 208, 0.34f);
    const ImVec4 blue   = rgb(91, 157, 249);        // #5B9DF9 — active / pressed
    const ImVec4 white  = rgb(255, 255, 255);

    style.Colors[ImGuiCol_WindowBg]         = rgb(18, 18, 18);   // #121212
    // ChildBg transparent, like the other themes: the editor's panels nest BeginChild freely
    // (tree pane, toolbars, list) and a blanket fill turns every one into a stray lighter box.
    // The #1A1A1A card surface arrives with the opt-in card treatment (#234 layer 3), pushed
    // explicitly by BeginComponentSection etc.
    style.Colors[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_PopupBg]          = rgb(26, 26, 26, 0.98f); // menus/tooltips: a real surface
    style.Colors[ImGuiCol_MenuBarBg]        = rgb(22, 22, 22);   // #161616
    style.Colors[ImGuiCol_TitleBg]          = rgb(22, 22, 22);
    style.Colors[ImGuiCol_TitleBgActive]    = rgb(30, 30, 30);   // #1E1E1E
    style.Colors[ImGuiCol_TitleBgCollapsed] = rgb(22, 22, 22);
    style.Colors[ImGuiCol_Border]           = ImVec4(white.x, white.y, white.z, 0.10f);
    style.Colors[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Separator]        = ImVec4(white.x, white.y, white.z, 0.08f);
    style.Colors[ImGuiCol_SeparatorHovered] = cyan;
    style.Colors[ImGuiCol_SeparatorActive]  = cyan;
    style.Colors[ImGuiCol_FrameBg]          = rgb(14, 14, 14);   // #0E0E0E — recessed
    style.Colors[ImGuiCol_FrameBgHovered]   = rgb(23, 23, 23);
    style.Colors[ImGuiCol_FrameBgActive]    = rgb(30, 30, 30);
    style.Colors[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(white.x, white.y, white.z, 0.10f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(white.x, white.y, white.z, 0.18f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(white.x, white.y, white.z, 0.26f);
    style.Colors[ImGuiCol_Text]            = rgb(230, 230, 230); // #E6E6E6
    style.Colors[ImGuiCol_TextDisabled]    = rgb(110, 110, 110); // #6E6E6E
    style.Colors[ImGuiCol_TextSelectedBg]  = ImVec4(cyan.x, cyan.y, cyan.z, 0.30f);
    style.Colors[ImGuiCol_CheckMark]       = cyan;
    style.Colors[ImGuiCol_SliderGrab]       = cyan;
    style.Colors[ImGuiCol_SliderGrabActive] = blue;
    // Buttons stay near-neutral; the one filled/primary action is ImGuiCol_Header (PrimaryButton).
    style.Colors[ImGuiCol_Button]           = rgb(30, 30, 30);
    style.Colors[ImGuiCol_ButtonHovered]    = rgb(40, 40, 40);
    style.Colors[ImGuiCol_ButtonActive]     = rgb(50, 50, 50);
    style.Colors[ImGuiCol_Header]           = ImVec4(cyan.x, cyan.y, cyan.z, 0.22f);
    style.Colors[ImGuiCol_HeaderHovered]    = cyanHi;
    style.Colors[ImGuiCol_HeaderActive]     = blue;              // pressed selection -> blue
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ResizeGripHovered] = cyanHi;
    style.Colors[ImGuiCol_ResizeGripActive]  = blue;
    style.Colors[ImGuiCol_Tab]                       = rgb(26, 26, 26);
    style.Colors[ImGuiCol_TabHovered]                = rgb(36, 36, 36);
    style.Colors[ImGuiCol_TabSelected]               = rgb(30, 30, 30);
    style.Colors[ImGuiCol_TabDimmed]                 = rgb(22, 22, 22);
    style.Colors[ImGuiCol_TabDimmedSelected]         = rgb(26, 26, 26);
    style.Colors[ImGuiCol_TabSelectedOverline]       = cyan;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_NavCursor]        = cyan;
    style.Colors[ImGuiCol_DockingPreview]  = ImVec4(cyan.x, cyan.y, cyan.z, 0.35f);
    style.Colors[ImGuiCol_DockingEmptyBg]  = rgb(14, 14, 14);
    style.Colors[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.45f);
}

bool EditorLayer::UseBentoLayout() const { return EditorSettings::Get().EditorTheme <= 1; }

void EditorLayer::ApplyEditorTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

    if (EditorSettings::Get().EditorTheme == 2) {
        // --- Windows XP (Luna) ---------------------------------------------------------------
        // Approximates the XP desktop theme: #ECE9D8 beige chrome, black text, white input
        // fields. The Luna-blue caption bars and the toolbar's own #245EDC taskbar blue (pushed
        // locally in DrawTopToolbar) are the "blue"; the accent/selection roles are the XP
        // Start-button green, per request. ImGui paints one global text colour, so the caption
        // bars use a *lighter* Luna blue than the real #0A64D2 to stay legible under black text.
        // Geometry (rounding/padding) is shared across all themes — this stays ImGui's rounded
        // shape, not XP's near-square one.
        auto rgb = [](int r, int g, int b, float a = 1.0f) {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        };

        const ImVec4 face       = rgb(236, 233, 216); // #ECE9D8 ButtonFace / window chrome
        const ImVec4 faceLight  = rgb(244, 242, 232);
        const ImVec4 faceDark   = rgb(206, 202, 183);
        const ImVec4 edge       = rgb(172, 168, 153); // #ACA899 3D edge
        const ImVec4 lunaTitle  = rgb(110, 158, 222); // Luna blue caption / docked-tab-bar strip
        const ImVec4 lunaSoft   = rgb(153, 174, 199); // greyed Luna for the unfocused caption bar
        // XP Start-button green — the accent, per request.
        const ImVec4 green      = rgb( 78, 154,  46); // #4E9A2E face
        const ImVec4 greenHi    = rgb( 99, 179,  60); // #63B33C hovered
        const ImVec4 greenLo    = rgb( 58, 122,  34); // #3A7A22 pressed / checkmark

        style.Colors[ImGuiCol_WindowBg]         = face;
        style.Colors[ImGuiCol_ChildBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        style.Colors[ImGuiCol_PopupBg]          = rgb(252, 251, 245, 0.98f);
        style.Colors[ImGuiCol_MenuBarBg]        = face;
        style.Colors[ImGuiCol_TitleBg]          = lunaSoft;
        style.Colors[ImGuiCol_TitleBgActive]    = lunaTitle;
        style.Colors[ImGuiCol_TitleBgCollapsed] = lunaSoft;
        style.Colors[ImGuiCol_Border]           = edge;
        style.Colors[ImGuiCol_BorderShadow]     = ImVec4(1.0f, 1.0f, 1.0f, 0.35f); // fakes the raised bevel
        style.Colors[ImGuiCol_Separator]        = rgb(172, 168, 153, 0.65f);
        style.Colors[ImGuiCol_SeparatorHovered] = greenHi;
        style.Colors[ImGuiCol_SeparatorActive]  = green;
        style.Colors[ImGuiCol_FrameBg]          = rgb(255, 255, 255);
        style.Colors[ImGuiCol_FrameBgHovered]   = rgb(238, 246, 235);
        style.Colors[ImGuiCol_FrameBgActive]    = rgb(224, 240, 219);
        style.Colors[ImGuiCol_ScrollbarBg]      = rgb(241, 239, 226);
        style.Colors[ImGuiCol_ScrollbarGrab]        = rgb(212, 208, 200); // #D4D0C8
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = rgb(180, 214, 165);
        style.Colors[ImGuiCol_ScrollbarGrabActive]  = rgb(140, 190, 118);
        style.Colors[ImGuiCol_Text]            = rgb(23, 23, 23);
        style.Colors[ImGuiCol_TextDisabled]    = rgb(128, 128, 128); // #808080
        style.Colors[ImGuiCol_TextSelectedBg]  = ImVec4(green.x, green.y, green.z, 0.55f);
        style.Colors[ImGuiCol_CheckMark]       = greenLo;
        style.Colors[ImGuiCol_SliderGrab]       = greenHi;
        style.Colors[ImGuiCol_SliderGrabActive] = green;
        style.Colors[ImGuiCol_Button]           = faceLight;
        style.Colors[ImGuiCol_ButtonHovered]    = rgb(223, 240, 214); // XP "hot" — a green wash
        style.Colors[ImGuiCol_ButtonActive]     = rgb(204, 212, 189); // pressed / sunk
        style.Colors[ImGuiCol_Header]           = ImVec4(green.x, green.y, green.z, 0.85f);
        style.Colors[ImGuiCol_HeaderHovered]    = greenHi;
        style.Colors[ImGuiCol_HeaderActive]     = green;
        style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        style.Colors[ImGuiCol_ResizeGripHovered] = greenHi;
        style.Colors[ImGuiCol_ResizeGripActive]  = green;
        // Panel tabs in the Start-button green, per request: unselected a lighter green (black
        // tab text still clears AA on it), the selected/front tab the deeper #4E9A2E so it reads
        // as the active one.
        style.Colors[ImGuiCol_Tab]                       = rgb(119, 176,  72); // #77B048
        style.Colors[ImGuiCol_TabHovered]                = rgb(136, 194,  88);
        style.Colors[ImGuiCol_TabSelected]               = green;              // #4E9A2E front tab
        style.Colors[ImGuiCol_TabDimmed]                 = rgb( 96, 138,  62);
        style.Colors[ImGuiCol_TabDimmedSelected]         = rgb( 74, 132,  44);
        style.Colors[ImGuiCol_TabSelectedOverline]       = rgb(158, 220, 118, 0.95f);
        style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        style.Colors[ImGuiCol_NavCursor]        = green;
        style.Colors[ImGuiCol_DockingPreview]  = ImVec4(green.x, green.y, green.z, 0.45f);
        style.Colors[ImGuiCol_DockingEmptyBg]  = rgb(158, 154, 138);
        style.Colors[ImGuiCol_ModalWindowDimBg] = rgb(26, 31, 41, 0.28f);
        return;
    }

    if (EditorSettings::Get().EditorTheme == 1) {
        // Prism: Bento's charcoal surfaces + geometry, but the accent family (plus the text
        // tint, buttons and tab keyline) drift through the hue wheel together every frame via
        // ApplyPrismAnimation - a living-colour Bento. Called here so a fresh switch looks
        // right, then again every frame from Draw(). #234.
        ApplyBentoPalette(style);
        ApplyPrismAnimation(m_ThemeHue);
        return;
    }

    // Bento (dark SaaS-dashboard) is the default - EditorTheme 0, and any out-of-range value.
    ApplyBentoPalette(style);
}

// Prism theme, per-frame: every hue-carrying style colour is derived from one drifting phase
// `h`, so accent / buttons / text tint / tab keyline all slide around the wheel together (with
// fixed hue offsets between them, so they stay a coordinated set rather than one flat colour).
// Backgrounds are left as ApplyEditorTheme set them — near-black, stationary.
void EditorLayer::ApplyPrismAnimation(float phase) {
    ImGuiStyle& style = ImGui::GetStyle();
    auto hsv = [](float h, float s, float v, float a = 1.0f) {
        h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h); // clamp, never wrap — the wrap is the "rainbow"
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
        return ImVec4(r, g, b, a);
    };

    // Match the corner monogram's prism read: a NARROW left→right dispersion — the monogram
    // smears ~0.30 of the wheel from one side to the other (sat ~0.6, bright) and drifts the
    // whole smear through the spectrum. So every role here sits at a fixed fraction of that same
    // 0.30 span from the shared base phase: at any instant the editor is one coherent
    // adjacent-hue wash (blue→violet, green→cyan, …), never a full red-to-red rainbow, and it
    // slides around as a unit exactly like the mark.
    const float h = phase;
    const float kSpan = 0.30f;
    auto band = [&](float frac, float s, float v, float a = 1.0f) {
        return hsv(h + frac * kSpan, s, v, a);
    };

    // Body text: lightly tinted, full value — legible as the wash drifts.
    style.Colors[ImGuiCol_Text]            = band(0.50f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]    = band(0.50f, 0.12f, 0.55f);

    // Colourful roles at the monogram's saturation/brightness; each state a step along the span.
    style.Colors[ImGuiCol_Button]          = band(0.10f, 0.55f, 0.48f);
    style.Colors[ImGuiCol_ButtonHovered]   = band(0.35f, 0.60f, 0.62f);
    style.Colors[ImGuiCol_ButtonActive]    = band(0.60f, 0.62f, 0.74f);

    ImVec4 accent        = band(0.20f, 0.55f, 0.50f);
    ImVec4 accentHovered = band(0.45f, 0.58f, 0.62f);
    ImVec4 accentActive  = band(0.70f, 0.60f, 0.72f);
    style.Colors[ImGuiCol_Header]            = accent;
    style.Colors[ImGuiCol_HeaderHovered]     = accentHovered;
    style.Colors[ImGuiCol_HeaderActive]      = accentActive;
    style.Colors[ImGuiCol_SeparatorHovered]  = accentHovered;
    style.Colors[ImGuiCol_SeparatorActive]   = accentActive;
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = accentHovered;
    style.Colors[ImGuiCol_ResizeGripActive]  = accentActive;
    style.Colors[ImGuiCol_NavCursor]         = accentActive;
    style.Colors[ImGuiCol_DockingPreview]    = ImVec4(accentActive.x, accentActive.y, accentActive.z, 0.55f);
    style.Colors[ImGuiCol_TextSelectedBg]    = ImVec4(accent.x, accent.y, accent.z, 0.55f);

    style.Colors[ImGuiCol_CheckMark]         = band(1.00f, 0.62f, 1.00f); // far end of the smear, pops
    style.Colors[ImGuiCol_SliderGrab]        = band(0.45f, 0.58f, 0.66f);
    style.Colors[ImGuiCol_SliderGrabActive]  = band(0.80f, 0.62f, 0.88f);

    style.Colors[ImGuiCol_Border]            = band(0.30f, 0.48f, 0.55f, 0.45f);
    style.Colors[ImGuiCol_Separator]         = band(0.45f, 0.42f, 0.38f, 0.55f);
    style.Colors[ImGuiCol_ScrollbarGrab]        = band(0.10f, 0.34f, 0.38f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = band(0.35f, 0.46f, 0.50f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = band(0.60f, 0.55f, 0.62f);

    style.Colors[ImGuiCol_TitleBgActive]    = band(0.20f, 0.48f, 0.17f); // faint colour on the focused window bar
    style.Colors[ImGuiCol_TabHovered]       = band(0.45f, 0.44f, 0.22f);
    style.Colors[ImGuiCol_TabSelectedOverline] = band(1.00f, 0.78f, 1.00f, 0.95f); // far end of the smear
}

void EditorLayer::Shutdown() {
    // Shutdown() is only reached on a clean exit, and main.cpp saves the real scene file just
    // before calling it — so any recovery snapshot is now stale and would otherwise trigger a
    // spurious "restore unsaved changes?" prompt on the next launch.
    ClearRecoverySnapshot();

    if (m_MarkSampleFbo) { glDeleteFramebuffers(1, &m_MarkSampleFbo); m_MarkSampleFbo = 0; }

    // Async luminance-readback PBOs (#178) — one ping-ponged pair per independently-sampled HUD.
    for (AsyncLuminanceReadback* rb : { &m_PlayBtnReadback, &m_NavGizmoReadback, &m_StatusBarReadback,
                                        &m_StatsHudReadback, &m_HistoryHudReadback }) {
        if (rb->Pbo[0] || rb->Pbo[1]) glDeleteBuffers(2, rb->Pbo);
        rb->Pbo[0] = rb->Pbo[1] = 0;
        rb->Pending[0] = rb->Pending[1] = 0;
    }

    for (auto& [path, entry] : m_ModelThumbnails) { (void)path; if (entry.first) glDeleteTextures(1, &entry.first); }
    m_ModelThumbnails.clear();
    m_ThumbnailLRU.clear();
    if (m_ThumbnailBlitFbo) { glDeleteFramebuffers(1, &m_ThumbnailBlitFbo); m_ThumbnailBlitFbo = 0; }

    // Safety net: save preferences on clean shutdown, in case a future control forgets its own
    // save call. This is not a replacement for per-control saves; it's insurance against silent
    // data loss if someone adds a new preference and forgets to wire it up.
    EditorSettings::Save();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}
// --- Layout presets (#236 R2 toolbar tail) — named ImGui-ini snapshots in project/layouts/ ---
namespace {
std::string LayoutsDir() { return ProjectPaths::Resolve("layouts"); }
std::string SanitizeLayoutName(const std::string& in) {
    std::string s;
    for (char c : in) if (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') s += c;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > 48) s.resize(48);
    return s;
}
}

std::vector<std::string> EditorLayer::LayoutPresetNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(LayoutsDir(), ec)) {
        if (ec) break;
        if (e.is_regular_file() && e.path().extension() == ".ini")
            names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

void EditorLayer::SaveLayoutPreset(const std::string& rawName) {
    const std::string name = SanitizeLayoutName(rawName);
    if (name.empty()) { Log::Warn("Layout preset: name is empty after sanitising."); return; }
    std::error_code ec;
    std::filesystem::create_directories(LayoutsDir(), ec);
    const std::string path = LayoutsDir() + "/" + name + ".ini";
    std::ofstream out(path, std::ios::binary);
    if (!out) { Log::Error("Layout preset: couldn't write '" + path + "'."); return; }
    out << ImGui::SaveIniSettingsToMemory(nullptr);
    Log::Info("Saved layout preset: " + name);
}

void EditorLayer::RequestLoadLayoutPreset(const std::string& name) {
    const std::string path = LayoutsDir() + "/" + SanitizeLayoutName(name) + ".ini";
    std::ifstream in(path, std::ios::binary);
    if (!in) { Log::Error("Layout preset: '" + name + "' not found."); return; }
    std::stringstream ss; ss << in.rdbuf();
    m_PendingLayoutIni = ss.str(); // Draw() applies it before the dockspace is built
}

void EditorLayer::DeleteLayoutPreset(const std::string& name) {
    std::error_code ec;
    std::filesystem::remove(LayoutsDir() + "/" + SanitizeLayoutName(name) + ".ini", ec);
}

// --- Lighting panel + shared section helpers (#236 R2) -----------------------------------
void EditorLayer::DrawEnvironmentSettings(World& world, float w) {
    // PR13: sky source selector
    {
        int src = (int)world.SkySourceMode;
        bool changed = false;
        if (ImGui::RadioButton("Procedural", src == 0)) { src = 0; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("HDRI", src == 1)) { src = 1; changed = true; }
        if (changed) {
            PushUndo(world, "Change Sky Source");
            world.SkySourceMode = (World::SkySource)src;
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Sky source: procedural gradient or an equirectangular .hdr file.");
    }

    if (world.SkySourceMode == World::SkySource::Hdri) {
        // HDRI path input
        static char hdriPathBuf[1024] = {};
        // Sync buffer when path changes externally (scene load)
        if (world.SkyHdriPath.size() < sizeof(hdriPathBuf) &&
            std::strncmp(hdriPathBuf, world.SkyHdriPath.c_str(), sizeof(hdriPathBuf)) != 0) {
            strncpy_s(hdriPathBuf, sizeof(hdriPathBuf), world.SkyHdriPath.c_str(), _TRUNCATE);
        }
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputText("HDRI path", hdriPathBuf, sizeof(hdriPathBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            PushUndo(world, "Set HDRI Path");
            world.SkyHdriPath = hdriPathBuf;
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Absolute path to an equirectangular .hdr file.");

        // Rotation slider
        ImGui::SetNextItemWidth(w);
        EditorUI::SliderFloat("HDRI rotation", &world.SkyRotationDegrees, 0.0f, 360.0f, "%.1f deg");
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit HDRI Rotation");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Y-axis rotation of the HDRI environment in degrees.");
    } else {
        ImGui::ColorEdit3("Horizon color", &world.SkyHorizonColor.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Sky Color");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Sky colour at the horizon.");
        ImGui::ColorEdit3("Zenith color", &world.SkyZenithColor.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Sky Color");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Sky colour straight up.");
    }

    ImGui::SetNextItemWidth(w);
    EditorUI::SliderFloat("Ambient intensity", &world.SkyAmbientIntensity, 0.0f, 3.0f, "%.2f x");
    if (ImGui::IsItemActivated()) PushUndo(world, "Edit Ambient Intensity");
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(
        "Strength of the image-based ambient light and reflections baked from the sky colours "
        "above. 1.0 is physically consistent; 0 disables environment lighting entirely.");
}

void EditorLayer::DrawPostProcessSettings(float w) {
    auto& prefs = EditorSettings::Get();
    ImGui::SetNextItemWidth(w);
    EditorUI::SliderFloat("Exposure (EV)", &prefs.ExposureEV, -6.0f, 6.0f, "%+.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Photographic stops applied before the tone curve. 0 = neutral. Applies live.");

    static const char* kTonemapLabels[] = { "Reinhard", "ACES", "AgX" };
    int tm = std::clamp(prefs.TonemapOperator, 0, 2);
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Tone mapping", &tm, kTonemapLabels, IM_ARRAYSIZE(kTonemapLabels))) {
        prefs.TonemapOperator = tm;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Curve that maps linear HDR to display. ACES = punchy filmic; AgX = gentler, less hue shift.");
}

void EditorLayer::DrawShadowSettings(float w) {
    auto& prefs = EditorSettings::Get();
    if (ImGui::Checkbox("Cast sun shadows", &prefs.ShadowsEnabled)) EditorSettings::Save();
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Cascaded shadow maps for the Directional light. Point/spot shadows are a later milestone.");

    static const char* kShadowResLabels[] = { "1024", "2048", "4096" };
    static const int   kShadowResValues[] = { 1024, 2048, 4096 };
    int srIdx = 1;
    for (int i = 0; i < 3; ++i) if (kShadowResValues[i] == prefs.ShadowResolution) { srIdx = i; break; }
    if (!prefs.ShadowsEnabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Shadow resolution", &srIdx, kShadowResLabels, IM_ARRAYSIZE(kShadowResLabels))) {
        prefs.ShadowResolution = kShadowResValues[srIdx];
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Per-cascade shadow map size. 4x memory + fill from 2048 to 4096.");

    static const char* kCascadeLabels[] = { "2", "3", "4" };
    int ccIdx = std::clamp(prefs.ShadowCascades - 2, 0, 2);
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Cascades", &ccIdx, kCascadeLabels, IM_ARRAYSIZE(kCascadeLabels))) {
        prefs.ShadowCascades = ccIdx + 2;
        EditorSettings::Save();
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Number of shadow cascades. Fewer = cheaper depth passes, coarser shadows far from the camera.");

    ImGui::SetNextItemWidth(w);
    EditorUI::SliderFloat("Shadow distance", &prefs.ShadowDistance, 10.0f, 500.0f, "%.0f m");
    if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("How far from the camera the cascades cover. Shorter = crisper shadows.");
    if (!prefs.ShadowsEnabled) ImGui::EndDisabled();
}

void EditorLayer::DrawLightingPanel(World& world) {
    if (!m_ShowLighting) return;
    ImGui::SetNextWindowSize(ImVec2(340.0f * m_UIScale, 430.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(ICON_FA_LIGHTBULB "  Lighting", &m_ShowLighting);
    PopTabChromeText();
    if (!open) { ImGui::End(); return; }

    const float w = 170.0f * m_UIScale;
    ImGui::SeparatorText("Environment");
    DrawEnvironmentSettings(world, w);
    ImGui::Spacing();
    ImGui::SeparatorText("Post-processing");
    DrawPostProcessSettings(w);
    ImGui::Spacing();
    ImGui::SeparatorText("Shadows (Directional Sun)");
    DrawShadowSettings(w);
    ImGui::Spacing();
    ImGui::TextDisabled("Environment is per-scene; post-processing & shadows persist in editor_prefs.json.");
    ImGui::End();
}

void EditorLayer::DrawPreferencesWindow(World& world) {
    if (!m_ShowPreferences) return;

    ImGui::SetNextWindowSize(ImVec2(660.0f * m_UIScale, 440.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText(); // keep the (blue) title bar text white on XP; body text is unaffected
    const bool prefsOpen = ImGui::Begin(ICON_FA_GEAR "  Preferences", &m_ShowPreferences);
    PopTabChromeText();
    if (!prefsOpen) { ImGui::End(); return; }

    static const char* kCats[] = {
        ICON_FA_UNIVERSAL_ACCESS "  General",
        ICON_FA_CAMERA "  Viewport",
        ICON_FA_TABLE_CELLS "  Grid & Snapping",
        ICON_FA_SUN "  Environment",
        ICON_FA_CLOCK "  Auto-Save",
        ICON_FA_GAUGE_HIGH "  Performance",
        ICON_FA_KEYBOARD "  Shortcuts",
        ICON_FA_CIRCLE_INFO "  About",
    };
    const int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));
    m_PrefsCategory = std::clamp(m_PrefsCategory, 0, kCatCount - 1);

    ImGui::BeginChild("##PrefCats", ImVec2(150.0f * m_UIScale, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < kCatCount; ++i) {
        if (ImGui::Selectable(kCats[i], m_PrefsCategory == i)) m_PrefsCategory = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##PrefBody", ImVec2(0, 0), ImGuiChildFlags_Borders);

    EditorSettings& prefs = EditorSettings::Get();
    const float kw = 160.0f * m_UIScale;

    switch (m_PrefsCategory) {
    case 0: { // General
        ImGui::SeparatorText("General");
        if (ImGui::Checkbox("Show editor tooltips", &prefs.ShowTooltips)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hover hints on Inspector fields, Hierarchy rows and toolbar buttons.");

        {
            static const char* kThemeLabels[] = { "Bento", "Prism", "Windows XP" };
            int theme = std::clamp(prefs.EditorTheme, 0, (int)IM_ARRAYSIZE(kThemeLabels) - 1);
            ImGui::SetNextItemWidth(kw);
            if (ImGui::Combo("Theme", &theme, kThemeLabels, IM_ARRAYSIZE(kThemeLabels))) {
                prefs.EditorTheme = theme;
                ApplyThemeStyle();       // colours + metrics — applies immediately, no restart
                EditorSettings::Save();
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Bento: dark SaaS-dashboard look - layered charcoal surfaces,\n"
                                     "hairline borders, rounded corners; cyan selection / blue\n"
                                     "active accents. The default.\n"
                                     "Prism: the Bento layout, but the accent, buttons and text\n"
                                     "tint drift through the spectrum together every frame.\n"
                                     "Windows XP: the Luna \"Blue\" scheme - beige chrome, black\n"
                                     "text, white fields, Luna-blue selection; compact geometry.");
        }

        ImGui::SeparatorText("Display");
        // 0 = auto (follow the monitor). Present the slider from 0.75; a value at/below the
        // floor snaps back to Auto so there's one obvious "let the OS decide" position.
        float uiScale = prefs.UiScaleOverride <= 0.0f ? m_UIScale : prefs.UiScaleOverride;
        ImGui::SetNextItemWidth(kw);
        bool isAuto = prefs.UiScaleOverride <= 0.0f;
        EditorUI::SliderFloat("UI scale", &uiScale, 0.70f, 2.50f,
                           isAuto ? "Auto (%.2f)" : "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            prefs.UiScaleOverride = uiScale < 0.75f ? 0.0f : uiScale;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Scales the whole editor UI (fonts, panels, spacing).\n"
                                 "Auto follows the monitor's display-scaling %%. Override it when a\n"
                                 "layout built on a 4K panel opens too small on a 1080p monitor.\n"
                                 "Takes effect on the next launch.");
        if (!isAuto) {
            ImGui::SameLine();
            if (ActionButton("Auto##uiscale", "Reset to the monitor's detected scale")) { prefs.UiScaleOverride = 0.0f; EditorSettings::Save(); }
        }
        ImGui::TextDisabled("Active: %.2fx%s", m_UIScale, isAuto ? "  (from monitor)" : "  (override)");
        break;
    }

    case 1: // Viewport
        ImGui::SeparatorText("Viewport");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Gizmo size", &m_GizmoSize, 0.05f, 0.40f, "%.2f");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("On-screen size of the transform gizmo.");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Vertex pick radius (px)", &m_VertexPickPixels, 5.0f, 150.0f, "%.0f");
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("How close (screen pixels) the cursor must be to a vertex to hover/grab/snap it while holding V.");
        ImGui::Checkbox("Show grid", &m_ShowGrid);
        ImGui::Checkbox("Show transform gizmo", &m_ShowGizmos);
        ImGui::Checkbox("Frame camera on select", &m_FrameOnSelect);
        if (ImGui::Checkbox("Adaptive HUD contrast", &prefs.AdaptiveHudContrast)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("The transparent viewport HUDs (Stats, History, status line, nav gizmo,\n"
                                 "Play/Stop, corner monogram, Game-view overlays) sample the render behind\n"
                                 "themselves and ease their text/pill between light and dark to stay readable.\n"
                                 "Off: they all draw static near-white text with no backing pill.");

        ImGui::SeparatorText("Game view");
        if (ImGui::Checkbox("Maximize on Play", &prefs.GameViewMaximizeOnPlay)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Entering Play Mode expands the Game view to borderless fullscreen\ninstead of staying windowed.");

        ImGui::SeparatorText("Light gizmos");
        if (ImGui::Checkbox("Show light gizmos", &prefs.ShowLightGizmos)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("3D wireframe shapes in the viewport: range sphere for point lights, cone for spots, aim arrow for directional.");
        if (!prefs.ShowLightGizmos) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Only for the selected light", &prefs.LightGizmoSelectedOnly)) EditorSettings::Save();
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Opacity", &prefs.LightGizmoOpacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Arrow / disc scale", &prefs.LightGizmoScale, 0.25f, 3.0f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Screen size of the parts that aren't tied to a world measurement (the directional arrow, the sun disc).");
        if (!prefs.ShowLightGizmos) ImGui::EndDisabled();

        ImGui::SeparatorText("Corner monogram");
        if (ImGui::Checkbox("Show engine mark", &prefs.EngineMarkEnabled)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("The spinning TE monogram in the viewport's bottom-left corner.");
        if (!prefs.EngineMarkEnabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Spin speed", &prefs.EngineMarkSpinSpeed, 0.0f, 4.0f, "%.2f rad/s");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("How fast the monogram turns. 0 parks it; the default 0.52 is one revolution every ~12 s.");
        {
            // The Prism editor theme forces the monogram prism on too, so show it ticked and
            // locked while that theme is active.
            const bool themeForces = prefs.EditorTheme == 1;
            ImGui::BeginDisabled(themeForces);
            bool prismShown = prefs.EngineMarkPrism || themeForces;
            if (ImGui::Checkbox("Prism", &prismShown) && !themeForces) {
                prefs.EngineMarkPrism = prismShown;
                EditorSettings::Save();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip(themeForces
                    ? "On automatically while the Prism editor theme is selected."
                    : "Paint the monogram with a slowly-drifting spectral gradient instead of the contrast-adaptive grey.");
        }
        if (!prefs.EngineMarkEnabled) ImGui::EndDisabled();
        break;

    case 2: // Grid & Snapping
        ImGui::SeparatorText("Grid");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Opacity", &prefs.GridOpacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Master strength of the grid lines. The grid also fades out on its own as the view tilts toward the horizon.");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Line spacing", &prefs.GridMinorSpacing, 0.05f, 50.0f, "%.2f",
                              ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("World units between minor lines. Also the step used by grid-snapped placement.");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderInt("Major line every", &prefs.GridMajorEvery, 2, 50, "%d cells");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("A brighter major line is drawn every N minor cells.");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Fade distance", &prefs.GridFadeDistance, 10.0f, 1000.0f, "%.0f m",
                              ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Distance from the camera at which the grid has fully faded out.");
        if (ImGui::Checkbox("Show axis lines", &prefs.GridShowAxisLines)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("The coloured rules through the origin: X (red) and Z (blue) on the ground, and a green Y line straight up.");
        if (!prefs.GridShowAxisLines) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Axis line thickness", &prefs.GridAxisThickness, 0.5f, 4.0f, "%.1f px");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Screen-pixel width of the red / green / blue axis lines.");
        if (!prefs.GridShowAxisLines) ImGui::EndDisabled();

        ImGui::SeparatorText("Snapping");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Position snap", &m_SnapTranslation, 0.01f, 50.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Rotation snap", &m_SnapRotationDeg, 1.0f, 180.0f, "%.1f deg");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Scale snap", &m_SnapScale, 0.01f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("The grid + snap on/off toggles are on the toolbar.");
        break;

    case 3: // Environment
        ImGui::SeparatorText("Environment");
        DrawEnvironmentSettings(world, kw); // shared with Window ▸ Lighting (#236 R2)
        ImGui::Spacing();
        if (ImGui::SmallButton(ICON_FA_LIGHTBULB "  Open Lighting panel")) m_ShowLighting = true;
        break;

    case 4: // Auto-Save
        ImGui::SeparatorText("Auto-Save");
        if (ImGui::Checkbox("Enable auto-save", &prefs.AutoSaveEnabled)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Periodically writes the scene to its file while you work, on top of the save on exit. Only writes when there are unsaved changes.");
        if (!prefs.AutoSaveEnabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(kw);
        if (ImGui::DragFloat("Interval (minutes)", &prefs.AutoSaveIntervalMinutes, 0.5f, 1.0f, 60.0f, "%.1f")) {
            prefs.AutoSaveIntervalMinutes = std::clamp(prefs.AutoSaveIntervalMinutes, 1.0f, 60.0f);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (!prefs.AutoSaveEnabled) ImGui::EndDisabled();
        break;

    case 5: { // Performance
        ImGui::SeparatorText("Frame Pacing");

        static const char* kVSyncLabels[] = { "Off", "On", "Adaptive" };
        int vsync = std::clamp(prefs.VSyncMode, 0, 2);
        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("VSync", &vsync, kVSyncLabels, IM_ARRAYSIZE(kVSyncLabels))) {
            prefs.VSyncMode = vsync;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Off: render as fast as possible (use the FPS limit below).\n"
                                 "On: sync to the monitor refresh, no tearing.\n"
                                 "Adaptive: sync when frames are on time, tear instead of stalling when they're late.");

        // The FPS cap is what actually 'unlocks' or re-limits the framerate when VSync is Off.
        // It still works with VSync On (e.g. cap to 60 on a 144 Hz panel) but that pairing can
        // beat against the refresh, so it's presented as the VSync-Off companion.
        static const char* kFpsPresets[] = { "Unlimited", "30", "60", "120", "144", "240", "Custom" };
        static const int   kFpsValues[]  = { 0, 30, 60, 120, 144, 240, -1 };
        int fpsIdx = IM_ARRAYSIZE(kFpsValues) - 1; // "Custom" unless an exact preset matches
        for (int i = 0; i < IM_ARRAYSIZE(kFpsValues); ++i)
            if (kFpsValues[i] == prefs.FpsLimit) { fpsIdx = i; break; }

        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("FPS limit", &fpsIdx, kFpsPresets, IM_ARRAYSIZE(kFpsPresets))) {
            if (kFpsValues[fpsIdx] >= 0) prefs.FpsLimit = kFpsValues[fpsIdx];
            else if (prefs.FpsLimit <= 0) prefs.FpsLimit = 60; // seed Custom with something sane
            EditorSettings::Save();
        }
        if (fpsIdx == IM_ARRAYSIZE(kFpsValues) - 1) { // Custom: expose the raw number
            ImGui::SetNextItemWidth(kw);
            if (ImGui::DragInt("Target FPS", &prefs.FpsLimit, 1.0f, 1, 1000)) {
                prefs.FpsLimit = std::clamp(prefs.FpsLimit, 1, 1000);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("0 / Unlimited removes the software cap. Applies to the whole editor and the game simulation.");

        ImGui::Spacing();
        ImGui::SeparatorText("Rendering (HDR)");

        DrawPostProcessSettings(kw); // exposure + tone mapping — shared with Window ▸ Lighting

        static const char* kMsaaLabels[] = { "Off", "2x", "4x", "8x" };
        static const int   kMsaaValues[] = { 1, 2, 4, 8 };
        int msIdx = 2;
        for (int i = 0; i < 4; ++i) if (kMsaaValues[i] == prefs.MsaaSamples) { msIdx = i; break; }
        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("MSAA", &msIdx, kMsaaLabels, IM_ARRAYSIZE(kMsaaLabels))) {
            prefs.MsaaSamples = kMsaaValues[msIdx];
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Multisample level of the HDR scene/game target. Takes effect next frame.");

        ImGui::Spacing();
        ImGui::SeparatorText("Shadows (Directional Sun)");
        DrawShadowSettings(kw); // shared with Window ▸ Lighting

        ImGui::Spacing();
        ImGui::TextDisabled("Changes apply immediately. All of these persist in editor_prefs.json.");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_LIGHTBULB "  Lighting panel")) m_ShowLighting = true;
        break;
    }

    case 6: { // Shortcuts — compact press-to-bind editor over the Shortcuts registry (#236 F)
        auto ctxName = [](std::uint32_t c) -> const char* {
            switch (c) {
                case Shortcuts::Ctx_Viewport:  return "Viewport";
                case Shortcuts::Ctx_Hierarchy: return "Hierarchy";
                case Shortcuts::Ctx_Project:   return "Project";
                case Shortcuts::Ctx_Inspector: return "Inspector";
                case Shortcuts::Ctx_App:       return "App";
                default:                       return "Global";
            }
        };

        // Toolbar row: filter + a single icon button to restore every default.
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", m_PrefsShortcutFilter.c_str());
            ImGui::SetNextItemWidth(-28.0f * m_UIScale);
            if (ImGui::InputTextWithHint("##scfilter", ICON_FA_MAGNIFYING_GLASS "  Filter", buf, sizeof(buf)))
                m_PrefsShortcutFilter = buf;
            ImGui::SameLine(0.0f, 4.0f * m_UIScale);
            if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT "##resetall", ImVec2(-1.0f, 0.0f))) {
                Shortcuts::ResetAllToDefault();
                Shortcuts::Save();
                m_PrefsCapturingId.clear();
                m_PrefsCaptureStage = 0;
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Restore every shortcut to its default");
        }

        // Capture polling. One armed row at a time (m_PrefsCapturingId):
        //   stage 0 — waiting for the first key. First press is stashed, NOT committed.
        //   stage 1 — one combo captured; confirm as-is (Enter / click) or press a second key
        //             to turn it into a two-key sequence (press G, then S).
        // Esc cancels; Backspace/Delete on an empty field unbinds.
        auto commitCapture = [&](Shortcuts::Chord chord) {
            Shortcuts::SetChord(m_PrefsCapturingId.c_str(), chord);
            Shortcuts::Save();
            m_PrefsCapturingId.clear();
            m_PrefsCaptureStage = 0;
        };
        if (!m_PrefsCapturingId.empty() && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_PrefsCapturingId.clear();
                m_PrefsCaptureStage = 0;
            } else if (m_PrefsCaptureStage == 0 &&
                       (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
                commitCapture(Shortcuts::Chord{}); // unbind
            } else if (m_PrefsCaptureStage == 1 && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                commitCapture(m_PrefsCapturePrefix);
            } else {
                Shortcuts::Chord got;
                if (Shortcuts::CaptureChord(got)) {
                    if (m_PrefsCaptureStage == 0) {
                        m_PrefsCapturePrefix = got;   // provisional single-combo binding
                        m_PrefsCaptureStage = 1;
                    } else {
                        got.PrefixKey   = m_PrefsCapturePrefix.Key;   // chain: earlier combo -> prefix
                        got.PrefixCtrl  = m_PrefsCapturePrefix.Ctrl;
                        got.PrefixShift = m_PrefsCapturePrefix.Shift;
                        got.PrefixAlt   = m_PrefsCapturePrefix.Alt;
                        commitCapture(got);
                    }
                }
            }
        }

        ImGui::TextDisabled("Click a binding, press the key. A second key makes a sequence \xC2\xB7 "
                            "Enter confirms \xC2\xB7 Esc cancels \xC2\xB7 Backspace unbinds.");

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,  ImVec2(6.0f * m_UIScale, 2.0f * m_UIScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * m_UIScale, 2.0f * m_UIScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(4.0f * m_UIScale, 3.0f * m_UIScale));

        if (ImGui::BeginTable("##sctable", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoBordersInBody |
                ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("##act",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##bind", ImGuiTableColumnFlags_WidthFixed, 132.0f * m_UIScale);
            ImGui::TableSetupColumn("##rst",  ImGuiTableColumnFlags_WidthFixed, 20.0f * m_UIScale);

            const std::uint32_t order[] = { Shortcuts::Ctx_Global, Shortcuts::Ctx_App,
                Shortcuts::Ctx_Viewport, Shortcuts::Ctx_Hierarchy, Shortcuts::Ctx_Project,
                Shortcuts::Ctx_Inspector };

            for (std::uint32_t gctx : order) {
                bool wroteHeader = false;
                for (const auto& s : Shortcuts::All()) {
                    if (s.Ctx != gctx) continue;
                    if (!MatchesFilter(m_PrefsShortcutFilter, s.Label + " " + ctxName(s.Ctx) + " " +
                            Shortcuts::ToString(s.Current)))
                        continue;

                    if (!wroteHeader) {
                        wroteHeader = true;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", ctxName(gctx));
                        ImGui::TableNextColumn();
                        ImGui::TableNextColumn();
                    }

                    const bool capturing = (m_PrefsCapturingId == s.Id);
                    ImGui::TableNextRow();
                    ImGui::PushID(s.Id.c_str());

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(s.Label.c_str());
                    // Conflict marker sits with the label, not on its own line — keeps rows even.
                    if (!capturing && s.Current.IsBound()) {
                        auto clashes = Shortcuts::Conflicts(s.Id.c_str(), s.Current);
                        if (!clashes.empty()) {
                            ImGui::SameLine(0.0f, 6.0f * m_UIScale);
                            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION);
                            if (ImGui::IsItemHovered()) {
                                std::string names;
                                for (size_t i = 0; i < clashes.size(); ++i) {
                                    const Shortcuts::Shortcut* o = Shortcuts::Find(clashes[i].c_str());
                                    names += (o ? o->Label : clashes[i]);
                                    if (i + 1 < clashes.size()) names += ", ";
                                }
                                EditorUI::SetTooltip("Same keys as: %s", names.c_str());
                            }
                        }
                    }

                    ImGui::TableNextColumn();
                    // Every binding is the same keycap chip on every row, in every state. An
                    // always-on 1px border defines the chip even where its fill matches the
                    // row stripe — that mismatch was why some rows looked like bare text.
                    std::string label;
                    bool dim = false;
                    if (capturing && m_PrefsCaptureStage == 0)      label = "Press a key\xE2\x80\xA6";
                    else if (capturing)                              label = Shortcuts::ToString(m_PrefsCapturePrefix) + " +\xE2\x80\xA6";
                    else if (s.Current.IsBound())                    label = Shortcuts::ToString(s.Current);
                    else                                          { label = "Unbound"; dim = true; }

                    const ImVec4 accent(0.85f, 0.55f, 0.15f, 0.95f);
                    const ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    ImVec4 chipFill = capturing ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Button);
                    ImVec4 chipHov  = capturing ? ImVec4(accent.x, accent.y, accent.z, 1.0f)
                                                : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
                    ImVec4 chipTxt  = capturing ? ImVec4(1, 1, 1, 1)
                                    : dim       ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                : baseTxt;
                    ImVec4 chipBorder(baseTxt.x, baseTxt.y, baseTxt.z, 0.28f);

                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button,        chipFill);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chipHov);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  chipHov);
                    ImGui::PushStyleColor(ImGuiCol_Text,          chipTxt);
                    ImGui::PushStyleColor(ImGuiCol_Border,        chipBorder);
                    // Fixed width + right-aligned so every chip is identical and they line up
                    // in a clean column regardless of label length.
                    const float pillW = 124.0f * m_UIScale;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                        std::max(0.0f, ImGui::GetContentRegionAvail().x - pillW));
                    if (ImGui::Button((label + "###bind").c_str(), ImVec2(pillW, 0.0f))) {
                        if (capturing && m_PrefsCaptureStage == 1) commitCapture(m_PrefsCapturePrefix);
                        else if (capturing)                        { m_PrefsCapturingId.clear(); m_PrefsCaptureStage = 0; }
                        else                                       { m_PrefsCapturingId = s.Id; m_PrefsCaptureStage = 0; }
                    }
                    ImGui::PopStyleColor(5);
                    ImGui::PopStyleVar();
                    if (!capturing && ImGui::IsItemHovered())
                        EditorUI::SetTooltip("Click to rebind \xC2\xB7 second key = sequence");

                    ImGui::TableNextColumn();
                    if (s.Overridden) {
                        ImGui::AlignTextToFramePadding();
                        if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_LEFT)) {
                            Shortcuts::ResetToDefault(s.Id.c_str());
                            Shortcuts::Save();
                            if (capturing) { m_PrefsCapturingId.clear(); m_PrefsCaptureStage = 0; }
                        }
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("Reset to %s", Shortcuts::ToString(s.Default).c_str());
                    }

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(3);
        break;
    }

    case 7: { // About
        ImGui::SeparatorText("About");
        ImGui::TextUnformatted("Tartarus Engine");
        ImGui::TextDisabled("Hand-rolled C++17 / OpenGL 4.6 editor.");
        ImGui::Spacing();
        ImGui::SeparatorText("System");
        if (m_SystemReport.empty()) {
            ImGui::TextDisabled("(system report not available)");
        } else {
            ImGui::BeginChild("##sysreport", ImVec2(0, 220.0f * m_UIScale), ImGuiChildFlags_Borders,
                ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& line : m_SystemReport) ImGui::TextUnformatted(line.c_str());
            ImGui::EndChild();
            if (ImGui::Button(ICON_FA_COPY "  Copy report")) {
                std::string all;
                for (const std::string& line : m_SystemReport) all += line + "\n";
                ImGui::SetClipboardText(all.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Built with");
        ImGui::TextDisabled("Dear ImGui + ImGuizmo  ·  EnTT  ·  GLFW  ·  GLM  ·  Assimp  ·  nlohmann/json  ·  stb");
        break;
    }
    }

    ImGui::EndChild();
    ImGui::End();
}

// #236 A4 — Project Settings: project-scoped, saved to project/settings.json (Physics, Tags)
// and project/layers.json (layer names). Sibling of the per-user Preferences window above;
// same two-pane category layout.
void EditorLayer::DrawProjectSettingsWindow(World& /*world*/) {
    if (!m_ShowProjectSettings) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f * m_UIScale, 420.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(ICON_FA_GEARS "  Project Settings", &m_ShowProjectSettings);
    PopTabChromeText();
    if (!open) { ImGui::End(); return; }

    static const char* kCats[] = {
        ICON_FA_PERSON_FALLING_BURST "  Physics",
        ICON_FA_TAGS "  Tags & Layers",
    };
    const int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));
    m_ProjSettingsCategory = std::clamp(m_ProjSettingsCategory, 0, kCatCount - 1);

    ImGui::BeginChild("##ProjCats", ImVec2(160.0f * m_UIScale, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < kCatCount; ++i)
        if (ImGui::Selectable(kCats[i], m_ProjSettingsCategory == i)) m_ProjSettingsCategory = i;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ProjBody", ImVec2(0, 0), ImGuiChildFlags_Borders);

    const float kw = 200.0f * m_UIScale;

    if (m_ProjSettingsCategory == 0) { // Physics
        ProjectSettings::PhysicsSettings& p = ProjectSettings::MutablePhysics();

        ImGui::SeparatorText("Gravity");
        ImGui::SetNextItemWidth(kw);
        ImGui::DragFloat3("m/s\xC2\xB2##grav", &p.Gravity.x, 0.1f, -200.0f, 200.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ProjectSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("World gravity. Only the Y component is applied today — it drives the\n"
                                 "Play-mode walk collider. X/Z are stored for rigid bodies (#185).");

        ImGui::SeparatorText("Simulation (reserved for #185)");
        ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(kw);
        ImGui::DragFloat("Fixed timestep", &p.FixedTimestep, 0.0001f, 0.001f, 0.1f, "%.4f s");
        ImGui::SetNextItemWidth(kw);
        ImGui::DragInt("Solver iterations", &p.SolverIterations, 0.1f, 1, 64);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Stored in project/settings.json now; consumed once the rigid-body\n"
                                 "step lands (#185). The Play-mode walker uses its own safety substep.");

        // #185 hardening — Play-mode Player tuning.
        ImGui::SeparatorText("Player");
        ImGui::SetNextItemWidth(kw);
        ImGui::DragFloat("Push strength", &p.PlayerPushStrength, 0.05f, 0.0f, 50.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ProjectSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("How hard walking into a dynamic body shoves it. 0 = the Player passes through nothing but still can't push.");
        ImGui::SetNextItemWidth(kw);
        if (ImGui::SliderInt("Player layer", &p.PlayerLayer, 0, LayerRegistry::kCount - 1,
                             LayerRegistry::DisplayName(p.PlayerLayer).c_str()))
            ProjectSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Collision-matrix layer the Player capsule is on.");

        // #185 PR 8 — layer collision matrix. Lower triangle: cell (row r, col c) toggles
        // whether layers r and c collide. Applied at the next Play.
        ImGui::SeparatorText("Collision Matrix");
        ImGui::TextDisabled("Which layer pairs collide while playing. Name layers in Tags & Layers.");
        ImGui::Spacing();
        if (ImGui::BeginTable("##collmatrix", LayerRegistry::kCount + 1,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn(); // corner
            for (int c = 0; c < LayerRegistry::kCount; ++c) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::to_string(c).c_str());
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", LayerRegistry::DisplayName(c).c_str());
            }
            for (int r = 0; r < LayerRegistry::kCount; ++r) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(LayerRegistry::DisplayName(r).c_str());
                for (int c = 0; c < LayerRegistry::kCount; ++c) {
                    ImGui::TableNextColumn();
                    if (c > r) continue; // lower triangle only (symmetric)
                    bool on = p.LayersCollide(r, c);
                    char id[16]; std::snprintf(id, sizeof(id), "##m%d_%d", r, c);
                    if (ImGui::Checkbox(id, &on)) { p.SetLayersCollide(r, c, on); ProjectSettings::Save(); }
                }
            }
            ImGui::EndTable();
        }
    } else { // Tags & Layers
        ImGui::SeparatorText("Tags");
        ImGui::TextDisabled("Named tags offered in the Inspector's Tag dropdown, on top of tags already in use.");
        ImGui::Spacing();

        const auto& tags = ProjectSettings::Tags();
        std::string removeTag;
        for (const std::string& t : tags) {
            ImGui::PushID(t.c_str());
            if (ActionButton(ICON_FA_XMARK "##deltag", "Remove this tag", false,
                             ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                removeTag = t;
            ImGui::SameLine();
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopID();
        }
        if (!removeTag.empty()) { ProjectSettings::RemoveTag(removeTag); ProjectSettings::Save(); }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(kw);
        const bool entered = ImGui::InputTextWithHint("##newtag", "New tag name", m_NewTagBuf,
            sizeof(m_NewTagBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ActionButton("Add##tag", "Add this tag to the project") || entered) && m_NewTagBuf[0]) {
            ProjectSettings::AddTag(m_NewTagBuf);
            ProjectSettings::Save();
            m_NewTagBuf[0] = '\0';
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SeparatorText("Layers");
        ImGui::TextDisabled("Slot 0 is always \"Default\". Names save to project/layers.json.");
        ImGui::Spacing();
        for (int i = 0; i < LayerRegistry::kCount; ++i) {
            ImGui::PushID(i);
            ImGui::Text("%2d", i);
            ImGui::SameLine(0.0f, 12.0f);
            if (i == 0) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Default");
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s", LayerRegistry::Name(i).c_str());
                char hint[16];
                std::snprintf(hint, sizeof(hint), "Layer %d", i);
                ImGui::SetNextItemWidth(kw);
                if (ImGui::InputTextWithHint("##lname", hint, buf, sizeof(buf)))
                    LayerRegistry::SetName(i, buf);
                if (ImGui::IsItemDeactivatedAfterEdit()) LayerRegistry::Save();
            }
            ImGui::PopID();
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

void EditorLayer::BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    ImViewGuizmo::BeginFrame();
}

void EditorLayer::EndFrame() {
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_RenderDrawData(dd);

    // Frosted backdrop: with the whole editor frame now on FBO 0, if a modal dialog is open,
    // blur the entire framebuffer and redraw just the dialog window crisp on top. Nothing
    // behind a modal is interactive, so freezing + blurring it costs nothing the user misses.
    ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    if (modal && dd && dd->Valid && dd->DisplaySize.x > 1.0f && dd->DisplaySize.y > 1.0f) {
        const int w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
        const int h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
        if (w > 8 && h > 8) {
            if (!m_ModalBlur) m_ModalBlur = std::make_unique<ScreenBlur>();
            if (!m_FrostCapture) m_FrostCapture = std::make_unique<Framebuffer>();
            m_FrostCapture->Resize(w, h);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_FrostCapture->Handle());
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            m_ModalBlur->Apply(m_FrostCapture->ColorTexture(), w, h, /*dstFbo=*/0, /*iterations=*/4);

            ImDrawData sub;
            sub.DisplayPos = dd->DisplayPos;
            sub.DisplaySize = dd->DisplaySize;
            sub.FramebufferScale = dd->FramebufferScale;
            sub.OwnerViewport = dd->OwnerViewport;
            sub.Textures = dd->Textures;
            sub.AddDrawList(modal->DrawList);
            sub.Valid = true;
            ImGui_ImplOpenGL3_RenderDrawData(&sub);
            GLStateCache::Invalidate();
        }
    }
}
// #185 — a small physics visual debugger: sim stats, four viewport draw channels, a slow-mo
// slider and a single-substep button. That's the general-use set; nothing else.
void EditorLayer::DrawPhysicsDebugWindow(World& world) {
    if (!EditorSettings::Get().ShowPhysicsPanel) return;
    ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);
    bool open = EditorSettings::Get().ShowPhysicsPanel;
    const bool visible = ImGui::Begin(ICON_FA_CUBES "  Physics", &open);
    EditorSettings& es = EditorSettings::Get();

    if (visible) {
        const PhysicsWorld::PhysicsDebugStats st = PhysicsWorld::GetDebugStats();
        if (!st.Active) {
            ImGui::TextDisabled("Not playing.");
        } else {
            ImGui::Text("%d bodies  (%d awake)   %d contacts", st.DynamicBodies, st.AwakeBodies,
                        st.ContactsThisFrame);
            ImGui::TextDisabled("substep %.2f ms  x%d   gravity %.0f", st.StepMillis, st.Substeps,
                                st.Gravity[1]);
        }
        ImGui::Separator();

        auto chan = [&](const char* label, unsigned bit, const char* tip) {
            unsigned f = es.PhysicsDebugDrawFlags;
            if (ImGui::CheckboxFlags(label, &f, bit)) { es.PhysicsDebugDrawFlags = f; EditorSettings::Save(); }
            if (tip && ImGui::IsItemHovered()) EditorUI::SetTooltip(tip);
        };
        bool cg = EditorSettings::Get().ShowColliders;
        if (ImGui::Checkbox("Collider shapes", &cg)) { EditorSettings::Get().ShowColliders = cg; EditorSettings::Save(); }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Wireframe of every collider (also on the Gizmos toolbar).");
        chan("Contacts & impacts", PhysicsWorld::PDD_Contacts,
             "Every touch leaves a fading spark; a firm hit (bounce / drop) flashes brighter,\n"
             "with a normal arrow scaled by the impact impulse.");
        chan("Raycasts", PhysicsWorld::PDD_Raycasts,
             "Draws the last ~64 raycasts / sweeps: bright travelled segment, hit burst + hit\n"
             "normal, faint continuation on a miss. Fades over ~half a second.");
        chan("Velocities", PhysicsWorld::PDD_Velocity,
             "An arrow from each awake body's centre of mass, length and colour by speed.");
        chan("Sleeping bodies", PhysicsWorld::PDD_Sleep, "A dim marker over bodies the solver has put to sleep.");

        ImGui::Separator();
        float ts = es.PhysicsSimTimeScale;
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::SliderFloat("Slow-mo", &ts, 0.0f, 2.0f, "%.2fx")) { es.PhysicsSimTimeScale = ts; EditorSettings::Save(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("1x")) { es.PhysicsSimTimeScale = 1.0f; EditorSettings::Save(); }
        ImGui::BeginDisabled(!m_InPlayMode);
        ImGui::SameLine();
        if (ImGui::SmallButton("Step")) PhysicsWorld::StepOneSubstep(world);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Advance one fixed 1/60 s substep (works while paused).");
    }

    ImGui::End();
    if (open != EditorSettings::Get().ShowPhysicsPanel) { EditorSettings::Get().ShowPhysicsPanel = open; EditorSettings::Save(); }
}

// Corner text overlay while playing (#185 D) — anchored to the Scene viewport, or the main
// window when maximized (no Scene rect then).
void EditorLayer::DrawPhysicsHud(bool maximized) {
    if (!EditorSettings::Get().PhysicsHudOverlay || !m_InPlayMode) return;
    const PhysicsWorld::PhysicsDebugStats st = PhysicsWorld::GetDebugStats();
    if (!st.Active) return;
    ImVec2 anchor(m_ViewportPos.x + 12.0f, m_ViewportPos.y + 12.0f);
    if (maximized || m_ViewportSize.x < 2.0f) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        anchor = ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + 40.0f);
    }
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##physhud", nullptr, f)) {
        ImGui::Text("PHYSICS  x%.2f", st.TimeScale);
        ImGui::Text("substeps %d   %.2f ms", st.Substeps, st.StepMillis);
        ImGui::Text("bodies %d  (%d awake)", st.DynamicBodies, st.AwakeBodies);
        ImGui::Text("contacts %d   triggers %d", st.ContactsThisFrame, st.TriggerOverlaps);
        if (st.BrokenJoints) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%d joint(s) broken", st.BrokenJoints);
        if (PhysicsWorld::IsGrabbing()) ImGui::TextColored(ImVec4(0.4f, 0.9f, 1, 1), "grabbing");
    }
    ImGui::End();
}

// Maximized play skips editor.Draw() entirely; still surface the physics tooling so the sim
// stays inspectable and the draw channels adjustable without dropping back to the panels (#185).
void EditorLayer::DrawPlayModeOverlays(World& world) {
    DrawPhysicsDebugWindow(world);
    DrawPhysicsHud(/*maximized=*/true);
}

void EditorLayer::DrawEngineMark(float dt) {
    if (!m_MarkTexture) return;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    // Spin rate is user-set (Preferences > Viewport). Default 0.52 rad/s is a full turn every
    // ~12 s — a slow idle spin, not a dizzying logo-spinner; 0 parks it.
    const float kTwoPi = 6.28318530718f;
    float spinSpeed = EditorSettings::Get().EngineMarkSpinSpeed;
    m_MarkSpinAngle = fmodf(m_MarkSpinAngle + dt * spinSpeed, kTwoPi);
    if (m_MarkSpinAngle < 0.0f) m_MarkSpinAngle += kTwoPi; // stay in [0, 2pi) even for a negative speed
    // Prism mode's spectral band drifts through the wheel on its own clock, independent of spin
    // (so it still moves at spin speed 0). Gentle — this is ambient colour, not a strobe.
    m_MarkHue = fmodf(m_MarkHue + dt * 0.6f, 1.0f); // ~1.7 s per full sweep of the wheel

    float size = 54.0f * m_UIScale; // 25% down from the #19-P19 bump; still reads as a mark, less visual weight
    float margin = 14.0f * m_UIScale;
    float half = size * 0.5f;
    // DrawViewportStatusBar draws an overlay strip across the bottom of this same rect (its height
    // isn't subtracted from m_ViewportSize). Reserve it here so the mark's bottom inset matches its
    // left inset instead of tucking behind the strip. Height mirrors that function's barH.
    float statusBarH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;
    ImVec2 cornerC(m_ViewportPos.x + margin + half,
                   m_ViewportPos.y + m_ViewportSize.y - statusBarH - margin - half);

    // dt is used to drive motion/eases below — clamp it so a one-off hitch (first frame, a stall
    // elsewhere in the frame) can't teleport the mark. Motion is otherwise fully dt-scaled, so
    // it runs identically smooth at any refresh rate.
    float sdt = dt; if (sdt < 0.0f) sdt = 0.0f; if (sdt > 0.05f) sdt = 0.05f;

    // --- idle detection -> DVD-screensaver bounce ----------------------------------------------
    ImGuiIO& io = ImGui::GetIO();
    bool userActive =
        io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
        io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
        io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2] ||
        io.InputQueueCharacters.Size > 0 ||
        io.KeyCtrl || io.KeyShift || io.KeyAlt || io.KeySuper;
    if (!userActive) {
        for (int i = 0; i < ImGuiKey_NamedKey_COUNT; ++i)
            if (io.KeysData[i].Down) { userActive = true; break; }
    }
    if (userActive) m_MarkIdleTime = 0.0f; else m_MarkIdleTime += dt;

    const float kIdleDelay = 30.0f;
    if (!m_MarkPosValid) { m_MarkPos = {cornerC.x, cornerC.y}; m_MarkPosValid = true; }

    // Rect the mark centre must stay within so the whole quad fits inside the viewport.
    float minX = m_ViewportPos.x + half, maxX = m_ViewportPos.x + m_ViewportSize.x - half;
    float minY = m_ViewportPos.y + half, maxY = m_ViewportPos.y + m_ViewportSize.y - statusBarH - half;
    if (maxX < minX) maxX = minX;
    if (maxY < minY) maxY = minY;

    if (m_MarkIdleTime >= kIdleDelay) {
        if (!m_MarkBouncing) {
            m_MarkBouncing = true;
            // Launch from the current spot on a diagonal; the exact angle drifts with the spin
            // phase so it isn't identical every time, no RNG needed.
            const float kSpeed = 230.0f * m_UIScale; // px/sec
            float ang = 0.6f + 0.9f * m_MarkSpinAngle / kTwoPi + kTwoPi * 0.125f;
            m_MarkVel = {cosf(ang) * kSpeed, sinf(ang) * kSpeed};
            const float kMin = 90.0f * m_UIScale; // keep both components lively (no near-vertical/horizontal crawl)
            if (fabsf(m_MarkVel.x) < kMin) m_MarkVel.x = (m_MarkVel.x < 0.0f ? -kMin : kMin);
            if (fabsf(m_MarkVel.y) < kMin) m_MarkVel.y = (m_MarkVel.y < 0.0f ? -kMin : kMin);
        }
        m_MarkPos += m_MarkVel * sdt;
        if (m_MarkPos.x <= minX) { m_MarkPos.x = minX; m_MarkVel.x =  fabsf(m_MarkVel.x); }
        if (m_MarkPos.x >= maxX) { m_MarkPos.x = maxX; m_MarkVel.x = -fabsf(m_MarkVel.x); }
        if (m_MarkPos.y <= minY) { m_MarkPos.y = minY; m_MarkVel.y =  fabsf(m_MarkVel.y); }
        if (m_MarkPos.y >= maxY) { m_MarkPos.y = maxY; m_MarkVel.y = -fabsf(m_MarkVel.y); }
    } else {
        m_MarkBouncing = false;
        m_MarkVel = {0.0f, 0.0f};
        // Critically-damped-ish ease back to the corner — ~0.16 s to settle, no overshoot.
        float k = 1.0f - expf(-sdt / 0.16f);
        m_MarkPos += (glm::vec2{cornerC.x, cornerC.y} - m_MarkPos) * k;
    }
    // Keep it inside even across a viewport resize.
    m_MarkPos.x = m_MarkPos.x < minX ? minX : (m_MarkPos.x > maxX ? maxX : m_MarkPos.x);
    m_MarkPos.y = m_MarkPos.y < minY ? minY : (m_MarkPos.y > maxY ? maxY : m_MarkPos.y);
    ImVec2 center(m_MarkPos.x, m_MarkPos.y);

    // --- contrast-adaptive tint --------------------------------------------------------------
    // Read back the little patch of the already-rendered scene texture directly behind the mark
    // and steer the tint toward white over dark content / black over light content, so it stays
    // legible wherever it is. m_SceneColorTexture is this frame's finished editor-viewport render
    // (set by main.cpp right before Draw()), sized 1:1 with m_ViewportSize, GL bottom-left origin.
    // Sampled at ~10 Hz, NOT every frame: the readback's GPU->CPU sync would otherwise be the one
    // thing in here that could cost a frame. The per-frame ease below hides the low sample rate.
    m_MarkSampleAccum += dt;
    const float kSampleInterval = 0.1f;
    // Prism monogram: its own setting, OR forced on whenever the Prism editor theme is active.
    const bool prism = EditorSettings::Get().EngineMarkPrism || EditorSettings::Get().EditorTheme == 1;
    if (!prism && m_SceneColorTexture != 0 && m_MarkSampleAccum >= kSampleInterval) {
        m_MarkSampleAccum = 0.0f;
        int vw = (int)m_ViewportSize.x, vh = (int)m_ViewportSize.y;
        const int kMaxPatch = 64;
        // mark centre -> viewport-local top-left -> bottom-left-origin texels
        int rx = (int)(center.x - m_ViewportPos.x - half);
        int ry = (int)(m_ViewportSize.y - ((center.y - m_ViewportPos.y - half) + size));
        int rw = (int)size, rh = (int)size;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > vw) rw = vw - rx;
        if (ry + rh > vh) rh = vh - ry;
        if (rw > kMaxPatch) { rx += (rw - kMaxPatch) / 2; rw = kMaxPatch; }
        if (rh > kMaxPatch) { ry += (rh - kMaxPatch) / 2; rh = kMaxPatch; }
        if (rx >= 0 && ry >= 0 && rw >= 1 && rh >= 1) {
            if (m_MarkSampleFbo == 0) glGenFramebuffers(1, &m_MarkSampleFbo);
            GLint prevReadFbo = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_MarkSampleFbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_SceneColorTexture, 0);

            unsigned char px[kMaxPatch * kMaxPatch * 4];
            glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px);

            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFbo);

            double sum = 0.0;
            const int n = rw * rh;
            for (int i = 0; i < n; ++i)
                sum += 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
            float avgLum = (float)(sum / (n * 255.0)); // 0 = black behind the mark, 1 = white

            float t = (avgLum - 0.30f) / (0.62f - 0.30f);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            m_MarkContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // white on dark, black on light
        }
    }
    // Ease toward the target every frame — ~0.15 s time constant, a smooth cross-fade.
    {
        float k = 1.0f - expf(-sdt / 0.15f);
        m_MarkContrastLum += (m_MarkContrastTarget - m_MarkContrastLum) * k;
    }
    if (!EditorSettings::Get().AdaptiveHudContrast) m_MarkContrastLum = m_MarkContrastTarget = 1.0f; // #275 toggle
    int markV = (int)(m_MarkContrastLum * 255.0f + 0.5f);
    markV = markV < 0 ? 0 : (markV > 255 ? 255 : markV);

    // Appended to the Scene window's own draw list and clipped to the viewport rect — NOT the
    // foreground list, which paints over every panel (the mark would then sit on top of the
    // Preferences window, the Inspector, any popup overlapping the viewport). At the Scene
    // window's z-order a panel on top correctly covers it. Same treatment as DrawEntityIcons.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* dl = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    dl->PushClipRect(ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                     ImVec2(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y), true);

    // Spin about the vertical (Y) axis — a sign turning on a post. The horizontal extent
    // foreshortens by cos(angle), collapses to a line edge-on, then comes back mirrored. Full
    // height is kept; only the left/right edges move.
    float hx = half * cosf(m_MarkSpinAngle);
    ImVec2 p1(center.x - hx, center.y - half); // top-left
    ImVec2 p2(center.x + hx, center.y - half); // top-right
    ImVec2 p3(center.x + hx, center.y + half); // bottom-right
    ImVec2 p4(center.x - hx, center.y + half); // bottom-left
    const ImVec2 uv1(0, 0), uv2(1, 0), uv3(1, 1), uv4(0, 1);
    const ImTextureID tex = (ImTextureID)(intptr_t)m_MarkTexture->GLHandle();

    if (prism) {
        // A spectral band smeared left -> right across the mark, the whole band drifting slowly
        // through the wheel — dispersion through glass, not a flat strobing hue. Needs per-vertex
        // colour, so the quad is written into the draw list by hand (AddImageQuad is one colour).
        // In the Prism editor theme the whole UI is driven by m_ThemeHue — use it here too so the
        // mark's dispersion and the editor's sweep the spectrum in lock-step.
        const float markHue = (EditorSettings::Get().EditorTheme == 1) ? m_ThemeHue : m_MarkHue;
        float rL, gL, bL, rR, gR, bR;
        ImGui::ColorConvertHSVtoRGB(markHue,                        0.62f, 1.0f, rL, gL, bL);
        ImGui::ColorConvertHSVtoRGB(fmodf(markHue + 0.30f, 1.0f),   0.62f, 1.0f, rR, gR, bR);
        const int a = 205;
        ImU32 colL = IM_COL32((int)(rL*255+0.5f), (int)(gL*255+0.5f), (int)(bL*255+0.5f), a);
        ImU32 colR = IM_COL32((int)(rR*255+0.5f), (int)(gR*255+0.5f), (int)(bR*255+0.5f), a);
        dl->PushTextureID(tex);
        dl->PrimReserve(6, 4);
        ImDrawIdx v = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteIdx(v); dl->PrimWriteIdx((ImDrawIdx)(v + 1)); dl->PrimWriteIdx((ImDrawIdx)(v + 2));
        dl->PrimWriteIdx(v); dl->PrimWriteIdx((ImDrawIdx)(v + 2)); dl->PrimWriteIdx((ImDrawIdx)(v + 3));
        dl->PrimWriteVtx(p1, uv1, colL);
        dl->PrimWriteVtx(p2, uv2, colR);
        dl->PrimWriteVtx(p3, uv3, colR);
        dl->PrimWriteVtx(p4, uv4, colL);
        dl->PopTextureID();
    } else {
        dl->AddImageQuad(tex, p1, p2, p3, p4, uv1, uv2, uv3, uv4,
                         IM_COL32(markV, markV, markV, 190)); // contrast-adaptive grey
    }

    dl->PopClipRect();
}
bool EditorLayer::IsMouseOverSceneViewport() const {
    // Geometric test against the same rect picking/gizmos already trust (ViewportPos()/
    // ViewportSize(), zeroed whenever the "Scene" tab isn't the active one) — deliberately NOT
    // ImGui::IsWindowHovered(), which can read false in edge cases involving the Scene window's
    // own transparent gizmo-overlay children.
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return false;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool inRect = mouse.x >= m_ViewportPos.x && mouse.x <= m_ViewportPos.x + m_ViewportSize.x &&
                  mouse.y >= m_ViewportPos.y && mouse.y <= m_ViewportPos.y + m_ViewportSize.y;
    if (!inRect) return false;

    // ...but the viewport stands down whenever the mouse belongs to another window sitting on
    // top of it: a floating Preferences window (being dragged over the viewport, or just open
    // and hovered), a popup, an undocked panel. Without this the pure rect test reads "over the
    // viewport" and the fly-camera / scroll-zoom / picking / box-select all fire underneath the
    // window the user is actually interacting with.
    ImGuiContext& g = *GImGui;
    if (g.MovingWindow != nullptr) return false; // any window is being dragged right now
    ImGuiWindow* hovered = g.HoveredWindow;
    if (!hovered) return true; // nothing on top — genuinely over the viewport
    ImGuiWindow* scene = ImGui::FindWindowByName("Scene");
    if (!scene) return true;
    return hovered->RootWindowDockTree == scene->RootWindowDockTree;
}

bool EditorLayer::WantsCaptureMouse() const {
    // ImGui's own WantCaptureMouse is true while merely hovering the "Scene" window now that
    // it's a real docked/tabbed window rather than the dockspace's bare passthrough central
    // node — which would otherwise block camera navigation AND viewport picking/box-select
    // (both gate on this) everywhere inside the one place they need to work. Hovering the
    // viewport itself was never what this check was meant to guard against; hovering some
    // OTHER panel (Hierarchy, Inspector, a popup, ...) still is.
    //
    // While the running game owns input, the editor viewport stands down entirely — everything
    // that gates on !WantsCaptureMouse() (camera nav, picking, box-select, vertex grab, the
    // Asset-Browser drop target) then naturally no-ops.
    if (m_GameInputActive) return true;
    return ImGui::GetIO().WantCaptureMouse && !IsMouseOverSceneViewport();
}

bool EditorLayer::WantsCaptureKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard || m_GameInputActive;
}

bool EditorLayer::OtherWindowOwnsKeyboard() const {
    // True when a real floating window (Preferences, an undocked panel) or an open menu/popup
    // holds keyboard focus — its interactions shouldn't leak into the viewport as tool switches,
    // view snaps, framing, quick-add, etc. Docked panels (Hierarchy/Inspector/Console) are NOT
    // counted: pressing W right after clicking an object in the Hierarchy to switch to the Move
    // tool is a normal, expected flow.
    ImGuiContext& g = *GImGui;
    ImGuiWindow* nav = g.NavWindow ? g.NavWindow->RootWindow : nullptr;
    if (!nav) return false;
    if (nav->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) return false;
    if (nav->Flags & ImGuiWindowFlags_Popup) return true;      // a menu / popup owns the keys
    if (nav->DockIsActive || nav->DockNode) return false;      // a docked panel — allow shortcuts
    if (nav == ImGui::FindWindowByName("Scene")) return false; // the viewport itself
    if (nav == ImGui::FindWindowByName("##DockHost")) return false; // "the dockspace"
    return true;                                               // a real floating window has focus
}

void EditorLayer::KeepDockspaceAlive() {
    // Reuse the id Draw() already resolved inside "##DockHost" — NOT ImGui::GetID("EditorDockspace")
    // again, which from here (no window pushed) hashes to a different, phantom id and leaves the
    // real editor dockspace to go stale through Play Mode. Zero only before Draw() has ever run,
    // which can't happen before Play is reachable anyway.
    if (m_EditorDockspaceId == 0) return;
    ImGui::DockSpace(m_EditorDockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
}

namespace {
// Make `windowName`'s tab the selected one in whatever dock node it lives in. Returns false if
// the window has no live dock node yet (so the caller can keep the request pending and retry).
//
// ImGui::SetWindowFocus() does NOT select a specific tab in a shared dock node in this vendored
// version — its own source comment says so ("we avoid applying focus immediately before the
// tabbar is visible") and the line that would do it is commented out. It only affects nav/OS
// focus, so selecting a tab means poking the dock node's own TabBar state directly.
bool SelectDockedTab(const char* windowName) {
    ImGuiWindow* win = ImGui::FindWindowByName(windowName);
    if (!win || !win->DockNode) return false;
    ImGuiDockNode* node = win->DockNode;
    node->SelectedTabId = win->TabId;
    if (node->TabBar) {
        node->TabBar->SelectedTabId = win->TabId;
        node->TabBar->NextSelectedTabId = win->TabId;
    }
    return true;
}
} // namespace

void EditorLayer::ApplyPendingViewportTabFocus() {
    // A request stays pending until it actually lands on a live dock node — on the exact frame
    // Play/Stop is pressed, Draw() (and Scene's/Game's Begin) may not have run yet, so there's
    // nothing to select into; the next full editor-UI frame finishes the job. Game wins if both
    // are somehow set (Play is the more recent intent) and clears the stale Scene request.
    if (m_FocusGameTabRequested) {
        if (SelectDockedTab("Game")) {
            m_FocusGameTabRequested = false;
            m_FocusSceneTabRequested = false;
        }
    } else if (m_FocusSceneTabRequested) {
        if (SelectDockedTab("Scene")) m_FocusSceneTabRequested = false;
    }

    // Seed the bottom dock node on the Asset Browser tab (ImGui otherwise leaves Console, the
    // last one docked, active). Runs here — after every panel's Begin/End for the frame — for
    // the same reason the viewport tab focus does: an earlier poke gets overwritten. ImGui's
    // .ini dock restore also re-asserts its saved tab for the first few frames, so keep poking
    // for a short grace window rather than stopping at the first apparent success.
    // For the first stretch of editor frames, hold the bottom dock node on the Asset Browser tab
    // (ImGui's .ini restore keeps re-asserting its saved tab — Console — for several frames, so a
    // one-shot poke loses). After the counter runs out the user is free to switch tabs.
    if (m_SelectAssetBrowserTabFrames > 0) {
        --m_SelectAssetBrowserTabFrames;
        // DockNodeUpdateTabBar() snaps the node's selected tab back to whatever holds nav focus
        // every frame, so poking the tab bar alone (SelectDockedTab) loses to the Console window.
        // Focus the Asset Browser window itself — the tab selection follows — and stop as soon as
        // it's taken so we're not stealing focus for longer than the .ini restore needs.
        ImGuiWindow* ab = ImGui::FindWindowByName("Asset Browser");
        if (ab && ab->DockNode && ab->DockNode->SelectedTabId == ab->TabId) {
            m_SelectAssetBrowserTabFrames = 0;
        } else if (ab) {
            SelectDockedTab("Asset Browser");
            ImGui::FocusWindow(ab);
        }
    }
}
void EditorLayer::Draw(World& world, AssetLibrary& assets, Camera& editorCamera, float dt) {
    m_AssetsPtr = &assets; // see the member comment - lets PushUndo() snapshot AssetLibrary
                           // state without needing every one of its call sites to pass it in
    m_EditorCameraPtr = &editorCamera;

    m_ThumbnailBudgetThisFrame = 3; // at most this many new Asset Browser model thumbnails per frame
    m_ScreenshotThumbBudgetThisFrame = 8; // at most this many new Asset Browser screenshot thumbnails per frame (#176)

    if (m_AssetRefreshFlash > 0.0f) m_AssetRefreshFlash = std::max(0.0f, m_AssetRefreshFlash - dt); // #236 G

    // Prism theme: drift the palette's spectral phase and repaint the hue-driven style colours
    // before any window is submitted this frame. Slow — the band should look like it's tilting,
    // not spinning. Dark Slate: nothing to do.
    if (EditorSettings::Get().EditorTheme == 1) {
        // Same phase + rate as the corner monogram's dispersion, so the editor and the mark
        // sweep the spectrum in lock-step (DrawEngineMark reads m_ThemeHue in Prism mode too).
        m_ThemeHue = fmodf(m_ThemeHue + dt * 0.6f, 1.0f);
        ApplyPrismAnimation(m_ThemeHue);
    }

    // First editor frame after a crash-interrupted session: offer to restore the auto-saved
    // recovery snapshot. No-op unless Init() flagged one as newer than the scene file.
    DrawRecoveryPrompt(world, assets);
    DrawExitPrompt();
    DrawSceneSwitchPrompt(world, assets);
    DrawRevertScenePrompt(world, assets);
    DrawPreferencesWindow(world);
    DrawProjectSettingsWindow(world);
    DrawLightingPanel(world); // #236 R2
    DrawPhysicsDebugWindow(world); // #185 debug tooling
    DrawPhysicsHud();             // #185 D
    DrawScreenshotPreview();

    // Auto-save: only ticks here (Draw() is editor-mode-only, per main.cpp) so it never fires
    // mid-Play - the same reason OnExitPlayMode's revert-to-snapshot exists, autosaving
    // transient gameplay state would be wrong. Skipped entirely when nothing's actually unsaved,
    // so a session where you're just looking around never writes anything.
    //
    // Crucially it writes a RECOVERY SNAPSHOT (see WriteRecoverySnapshot), not the real scene
    // file, and does NOT clear m_Dirty: the timer is a crash safety net, and an unnoticed bad
    // edit must never be able to auto-overwrite the only saved copy. The scene file changes
    // only on an explicit Save / Save As.
    const EditorSettings& prefsForAutoSave = EditorSettings::Get();
    if (prefsForAutoSave.AutoSaveEnabled) {
        m_AutoSaveTimer += dt;
        float intervalSeconds = std::max(1.0f, prefsForAutoSave.AutoSaveIntervalMinutes * 60.0f);
        if (m_AutoSaveTimer >= intervalSeconds) {
            m_AutoSaveTimer = 0.0f;
            if (m_Dirty) WriteRecoverySnapshot(world, assets);
        }
    } else {
        m_AutoSaveTimer = 0.0f; // don't let it silently accumulate while disabled
    }

    // Drop a staged-but-never-committed undo snapshot once the interaction is definitely over
    // (its widget vanished mid-edit, e.g. the selection changed before IsItemDeactivatedAfterEdit
    // could fire) so the next StageUndo captures fresh state instead of a stale one.
    if (m_HasStagedUndo && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_HasStagedUndo = false;
        m_StagedUndoJson.clear();
        m_StagedUndoSelectedOrders.clear();
    }

    UpdateViewTransition(editorCamera, dt);

    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww, h = (float)wh;

    // Unity-ish default layout: a full-width toolbar above everything (File/Import/Add/
    // Settings dropdowns + a one-click toggle row), then a real ImGui dock space filling the
    // rest of the window — Hierarchy on the left, Inspector on the right, Asset Browser along
    // the bottom, and the center node left empty (passthrough) so the 3D viewport shows through
    // it. Because it's a real dock tree, dragging any panel's border resizes its dock node and
    // every neighbor sharing that border reacts too, and ImGui persists the whole arrangement
    // to imgui.ini across launches — DockBuilder below only runs once, to seed that arrangement
    // the very first time there's no saved layout yet.
    const float toolbarH = kToolbarHeight * m_UIScale;

    // The full-width toolbar strip is drawn by the reloadable editor module now (issue #229):
    // EditorModuleToolbar::Draw(), invoked from main.cpp's editorModule.Draw() immediately after
    // this method returns, still inside the same ImGui frame. It pins its own "##Toolbar" window
    // (same pos/size/constraints this used to set) and reaches host state through
    // EditorModuleHostAPI. The dock host below still starts at y = toolbarH.

    // (The top-right wordmark overlay was removed in the #92 UI pass — the corner monogram
    // (DrawEngineMark) is the only branding mark now.)

    ImGui::SetNextWindowPos(ImVec2(0, toolbarH));
    ImGui::SetNextWindowSize(ImVec2(w, h - toolbarH));
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar();

    ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    // Stash it while "##DockHost" is the current window — KeepDockspaceAlive() runs later with
    // no window pushed and can't re-derive the same id itself (see its comment).
    m_EditorDockspaceId = dockspaceId;
    // Layout preset requested (#236 R2): apply the saved ImGui-ini snapshot before the
    // dockspace is built this frame, so every docked window lands where the preset put it.
    if (!m_PendingLayoutIni.empty()) {
        ImGui::LoadIniSettingsFromMemory(m_PendingLayoutIni.c_str(), m_PendingLayoutIni.size());
        m_PendingLayoutIni.clear();
        m_ShowHierarchy = m_ShowInspector = m_ShowAssetBrowser = true; // never leave a core panel hidden
    }

    bool rebuildLayout = m_ResetLayoutRequested;
    m_ResetLayoutRequested = false;
    // Reset Layout also un-hides any panel the user closed — otherwise "restore the default
    // layout" would leave a panel missing with no obvious way to get it back — and re-derives
    // the Asset Browser's tree width / icon size from the current UI scale. Those are stored as
    // raw pixels, so a layout saved on a 4K panel leaves them oversized on a 1080p monitor;
    // this makes one button recover a display change end-to-end.
    if (rebuildLayout) {
        m_ShowHierarchy = m_ShowInspector = m_ShowAssetBrowser = true;
        m_AssetTreeWidth = 230.0f * m_UIScale;
        m_AssetIconSize = 96.0f * m_UIScale;
        EditorSettings::Get().AssetBrowserTreeWidth = 0.0f;
        EditorSettings::Get().AssetBrowserIconSize = 0.0f;
        EditorSettings::Save();
    }
    // One-time migration: builds before this seeded the Scene/Game viewport as the dockspace's
    // "central node". ImGui's DockNodeTreeUpdatePosSize() then hands every panel sharing a split
    // with the central node a FIXED pixel size on window resize and lets the viewport absorb all
    // the slack — so the side panels never scale with the window. The seed below uses no central
    // node (every split distributes by ratio → all panels keep their fraction, like Unity), so
    // if a persisted layout still has a central node, tear it down once and re-seed.
    if (!rebuildLayout && ImGui::DockBuilderGetNode(dockspaceId) &&
        ImGui::DockBuilderGetCentralNode(dockspaceId) != nullptr) {
        rebuildLayout = true;
    }
    if (rebuildLayout) {
        // Tear down the existing tree (whatever the user dragged panels into) so the block
        // below rebuilds the original default split from scratch, same as a first launch.
        ImGui::DockBuilderRemoveNode(dockspaceId);
    }
    if (!ImGui::DockBuilderGetNode(dockspaceId)) {
        // Deliberately NOT ImGuiDockNodeFlags_DockSpace here: that flag makes the leftover
        // center leaf a "central node", which ImGui then resizes by giving its split-siblings a
        // fixed pixel size and itself the remainder — so the surrounding panels wouldn't scale
        // when the window resizes. A plain node means every split redistributes by SizeRef
        // ratio, so all five regions keep their fraction of the window. The passthru central
        // node is unused anyway — the Scene view is an FBO shown via ImGui::Image, not an
        // empty see-through center (see SetSceneTexture).
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImVec2(w, h - toolbarH));

        // Proportions picked to give each panel room to breathe rather than a bare-minimum
        // strip: the Hierarchy needs width for longer object names, the Inspector for field
        // labels beside their values, and the Asset Browser — now a two-column tree+grid —
        // needs real height or its own content ends up cramped.
        ImGuiID center = dockspaceId;
        ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
        ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Asset Browser", bottom);
        // Console shares the bottom node as a tab beside the Asset Browser, the way Unity docks
        // Project and Console together.
        ImGui::DockBuilderDockWindow(ICON_FA_TERMINAL "  Console", bottom);
        // Scene and Game are Unity's own pair of tabs sharing one dock node — Scene is the
        // editor's 3D viewport, Game is the locked-aspect Play Mode preview; only whichever tab
        // is active actually shows/renders (see m_SceneViewportVisible below).
        ImGui::DockBuilderDockWindow("Scene", center);
        ImGui::DockBuilderDockWindow("Game", center);
        ImGui::DockBuilderFinish(dockspaceId);
        m_SceneGameDockNodeId = center;
        m_SelectAssetBrowserTabFrames = 90;  // land on Asset Browser, not Console
    }
    // NoWindowMenuButton drops the little "▼" tab-list button from every dock node's tab bar —
    // it only listed the tabs already visible right next to it, so it was pure clutter.
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::End();

    // Neither Scene nor Game is submitted while play is maximized (both gated on editorUIVisible) —
    // reapplying the node here, every editor-mode frame, guards against ImGui occasionally
    // failing to remember Game's dock assignment across that gap and popping it out into its own
    // floating window instead (observed after a Play/Stop cycle). ImGuiCond_Appearing makes this
    // a no-op for a window that's already being submitted continuously, so it never fights a
    // manual re-dock the user did on purpose.
    if (m_SceneGameDockNodeId != 0) {
        ImGui::SetNextWindowDockID(m_SceneGameDockNodeId, ImGuiCond_Appearing);
    }

    // "Scene" is a real dockable/tabbable ImGui window now (tabbed with "Game"). Its 3D content
    // is rendered by main.cpp into its own offscreen framebuffer beforehand (see
    // SetSceneTexture()'s comment for why — a docked window's host node always paints its own
    // near-opaque background over raw GL content drawn straight into the backbuffer, which is
    // NOT how this used to render: back when the viewport was the dockspace's bare passthrough
    // central node rather than a real named window, there was no host background to paint).
    // Begin() returns false when Scene isn't the active tab (Game is showing instead) — zeroing
    // the viewport rect in that case is what makes every existing ">0.0f" guard elsewhere
    // (picking, gizmos) just naturally no-op instead of needing an explicit visibility check.
    // (Pending Scene/Game tab focus is applied later, from main.cpp, after Game's Begin() has
    // also run this frame — see ApplyPendingViewportTabFocus for why it can't happen here.)
    // NoFocusOnAppearing: the ACTUAL root cause of the Play/Stop tab-selection bug, found by
    // reading ImGui's own source rather than guessing further. Both Scene and Game go many
    // frames without being submitted at all during Play Mode, which makes each one
    // "window_just_activated_by_user" the instant it's Begin()'d again after Stop — and without
    // this flag, THAT alone makes a window auto-focus itself (imgui.cpp's Begin(), "Apply window
    // focus" block), which a separate docking code path ("Apply NavWindow focus back to the tab
    // bar", DockNodeUpdateTabBar) then uses to force it to become the dock node's selected tab.
    // Since Game's Begin() always runs after Scene's in a frame, Game's auto-focus-on-reappear
    // was winning every single time, no matter what explicitly requested otherwise afterward.
    // With this flag on both windows, reappearing after Play never touches tab selection again -
    // ApplyPendingViewportTabFocus()'s explicit override is the only thing that still can.
    ImGuiWindowFlags sceneFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    PushTabChromeText(); // Scene/Game share a tab bar; keep its text white on XP (drawn in Begin())
    m_SceneViewportVisible = ImGui::Begin("Scene", nullptr, sceneFlags);
    PopTabChromeText();
    ImGui::PopStyleVar();
    // Track Scene's live dock node every frame (not just once) so the Play/Stop re-docking
    // safety net always targets the pair's CURRENT home — wherever the user last dragged the
    // Scene/Game tab group — instead of the stale first-run seed id. Only overwrite with a real
    // assignment: a docked Scene always reports a non-zero DockId; keep the last known one if it
    // ever reads zero (e.g. mid-undock) rather than blanking the safety net.
    if (ImGuiWindow* sceneWindow = ImGui::FindWindowByName("Scene")) {
        if (sceneWindow->DockId != 0) m_SceneGameDockNodeId = sceneWindow->DockId;
    }
    if (m_SceneViewportVisible) {
        ImVec2 contentMin = ImGui::GetWindowContentRegionMin();
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        m_ViewportPos = {windowPos.x + contentMin.x, windowPos.y + contentMin.y};
        m_ViewportSize = {contentSize.x, contentSize.y};
        m_LastSceneContentRegion = m_ViewportSize;

        if (m_SceneColorTexture != 0 && contentSize.x > 0.0f && contentSize.y > 0.0f) {
            // uv0=(0,1)/uv1=(1,0): OpenGL textures are bottom-left origin, ImGui::Image expects
            // top-left, so this flips the framebuffer's color attachment right-side up. Filled
            // exactly (no letterboxing) since the offscreen render is sized to match this exact
            // rect every frame, so absolute-screen-space gizmo/picking math keeps working
            // unchanged against ViewportPos()/ViewportSize().
            ImGui::Image((ImTextureID)(intptr_t)m_SceneColorTexture, contentSize, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
    } else {
        m_ViewportPos = {0.0f, 0.0f};
        m_ViewportSize = {0.0f, 0.0f};
    }
    ImGui::End();

    if (EditorSettings::Get().EngineMarkEnabled && !m_HideEngineMarkForStats && !m_HideOverlaysThisFrame)
        DrawEngineMark(dt);

    // (Every editor panel's window is drawn by the reloadable editor module now — Console, the
    // Statistics HUD, the top toolbar, the Asset Browser, the Scene Hierarchy and the Inspector.
    // See main.cpp's editorModule.Draw(), which runs immediately after this call, still inside the
    // same ImGui frame and dockspace. The Asset Browser's grid, the Hierarchy's entity tree and
    // the Inspector's body are still host code, reached back via
    // EditorModuleHostAPI::DrawAssetGridBody / DrawHierarchyTreeBody / DrawInspectorBody.)
    if (!m_HideOverlaysThisFrame) {
        // Exponential smoothing of the frame time: a raw per-frame ms figure flickers too fast
        // to read. Used by the viewport status bar just below and by the reloadable Stats HUD
        // (exposed through EditorModuleHostAPI::GetSmoothedFrameMs). Lived inside DrawStatsPanel
        // before that panel moved out; kept on the same !m_HideOverlaysThisFrame path so its
        // cadence is unchanged.
        const float frameMs = dt * 1000.0f;
        m_SmoothedFrameMs = m_SmoothedFrameMs * 0.92f + frameMs * 0.08f;

        DrawViewportStatusBar();
        // The Undo History HUD moved into TartarusEditor.dll (EditorModuleHistory.cpp, #229);
        // the module gates its own draw through EditorModuleHostAPI::GetHistoryHudFrame, which
        // re-checks m_HideOverlaysThisFrame so a clean capture still suppresses it.
    }
    DrawCaptureFeedback(dt);

    // "Look through light" (#140 phase 4): the Inspector/Lights-panel buttons can't see the
    // camera, so they queue a light here; act on it once, then run the per-frame Esc/banner.
    if (m_PendingLookThrough != entt::null) {
        LookThroughLight(world, editorCamera, m_PendingLookThrough);
        m_PendingLookThrough = entt::null;
    }
    UpdateLookThrough(world, editorCamera);

    // Drains a couple of queued imports per frame (see ImportQueueManager.h) and, while any
    // remain, draws the bottom-right progress window with the current file / X of N / Cancel.
    m_ImportQueue.Update([&](const std::string& path) {
        std::string folder = m_CurrentAssetFolder;
        auto it = m_ImportTargetFolder.find(path);
        if (it != m_ImportTargetFolder.end()) {
            folder = it->second;
            m_ImportTargetFolder.erase(it);
        }
        ImportDroppedFile(world, assets, editorCamera, path, folder);
    });
    m_ImportQueue.DrawProgressUI();

    DrawViewportDropTarget(world, assets, editorCamera);

    // Holding V is a dedicated mode: it takes over the mouse for vertex grab-and-drag, so the
    // normal click-to-select and transform gizmo stand down while it's held to avoid the two
    // systems fighting over the same click. The grabbed vertex always belongs to the PRIMARY
    // selected model, but if a group is selected, every other member rides along by the same
    // delta each frame (see UpdateVertexDrag) so the whole group snaps together via that one
    // vertex instead of only the primary moving.
    // Ctrl excluded so Ctrl+V (paste) doesn't also arm the vertex-grab mode this key normally
    // owns on its own.
    bool vHeld = !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && ImGui::IsKeyDown(ImGuiKey_V);

    if (m_VertexDragActive && (!vHeld || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        m_VertexDragActive = false; // dropped: releasing V or the mouse button leaves it exactly where it is
    }

    glm::vec3 hoverLocal;
    bool hasHover = vHeld && !m_VertexDragActive && !WantsCaptureMouse() &&
        FindVertexUnderCursor(world, editorCamera, hoverLocal);

    if (hasHover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        PushUndo(world, "Move Vertex");
        // Both the drag plane and the offset must be world-space: the ray they get intersected
        // against in UpdateVertexDrag is world-space, so deriving them from the local matrix (and
        // from the local TransformComponent.Position) put the drag in the parent's frame. (#224)
        glm::mat4 model = world.GetCachedWorldTransform(m_Selected);
        glm::vec3 grabbedWorld = glm::vec3(model * glm::vec4(hoverLocal, 1.0f));
        m_VertexDragLocal = hoverLocal;
        m_VertexDragPlanePoint = grabbedWorld;
        m_VertexDragOffset = glm::vec3(model[3]) - grabbedWorld;
        m_VertexDragActive = true;
    }

    if (m_VertexDragActive) {
        UpdateVertexDrag(world, editorCamera);
    }

    if (!m_HideOverlaysThisFrame && m_GizmosMasterVisible) {
        if (m_ShowEntityIcons) DrawEntityIcons(world, editorCamera); // DrawLightGizmos self-gates on EditorSettings::ShowLightGizmos
        DrawLightGizmos(world, editorCamera);
    }

    // Drawn (and its hover/drag state refreshed) before picking runs below, so a click that
    // lands on the nav gizmo's rotate ring or tool buttons doesn't also start a viewport
    // box-select/pick underneath it. Its overlay forces itself above the Scene image but then
    // re-fronts any floating window that could overlap it (see KeepFloatingWindowsAboveOverlay).
    if (!m_HideOverlaysThisFrame) DrawViewGizmo(world, editorCamera);

    if (!vHeld) {
        if (m_HandTool) {
            HandleHandToolPan(editorCamera); // Q — LMB-drag pans; no picking or gizmo (#236 E)
        } else {
            UpdateLightHandles(world, editorCamera);
            HandleViewportPicking(world, editorCamera);
            if (m_ShowGizmos && m_GizmosMasterVisible && !m_HideOverlaysThisFrame) DrawGizmo(world, editorCamera);
        }
    }
    UpdateLockViewToSelection(world, editorCamera); // Shift+F — camera follows the selection centroid (#236 E)
    if (m_MeasureTool || m_MeasureCount > 0) DrawMeasurement(editorCamera); // #236 R2 ruler
    if (EyedropperArmed()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(mp, 9.0f, IM_COL32(120, 220, 255, 235), 0, 2.0f);
        const char* h = ICON_FA_EYE_DROPPER "  Click a colour  (Esc cancels)";
        ImVec2 ts = ImGui::CalcTextSize(h);
        ImVec2 p(mp.x + 16.0f, mp.y + 14.0f);
        dl->AddRectFilled(ImVec2(p.x - 5.0f, p.y - 3.0f), ImVec2(p.x + ts.x + 5.0f, p.y + ts.y + 3.0f),
                          IM_COL32(15, 20, 28, 225), 3.0f);
        dl->AddText(p, IM_COL32(235, 245, 255, 255), h);
    }

    // Anchored to the actual viewport's top-center (a pivot, not a fixed-width guess) so it
    // stays centered over the 3D view itself as the Hierarchy/Inspector/Asset Browser panels
    // around it are resized, instead of centering on the full window. Only shown while V is
    // actually held (the idle "Hold V..." reminder moved to the Controls list instead of
    // sitting over the viewport all the time).
    if (m_VertexDragActive || vHeld) {
        ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x + m_ViewportSize.x * 0.5f, m_ViewportPos.y + 10.0f),
            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##hint", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        if (m_VertexDragActive) {
            const char* msg = HasGroupSelection()
                ? "Dragging vertex — rest of the group is riding along (release click or V to drop)"
                : "Dragging vertex — release click or V to drop";
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", msg);
        } else if (!IsVertexDraggable(world, m_Selected)) {
            ImGui::TextDisabled("Select a model first, then aim at one of its vertices");
        } else if (hasHover) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Click and hold to grab this vertex");
        } else {
            ImGui::TextDisabled("Aim closer to an edge or corner of the model");
        }
        ImGui::End();
    }

    // --- Shortcut dispatch (#236 F) ---------------------------------------------------------
    // The central table + press-to-bind editor live in Shortcuts.{h,cpp}. Here we decide which
    // context owns the keyboard this frame and let the dispatcher evaluate every chord once;
    // the call sites below then just ask Shortcuts::Triggered("editor.undo"). Bindings tagged
    // Ctx_Viewport (the Q/W/E/R/T/Y tools, Shift+A, F) are suppressed while a panel has focus
    // or Right-drag fly is held — that replaces the old hierarchyOwnsLetters / RMB guards for
    // the migrated ones.
    const bool keyboardFree =
        !ImGui::GetIO().WantTextInput && !m_GameInputActive && !OtherWindowOwnsKeyboard();
    {
        std::uint32_t sctx = 0;
        if (keyboardFree) {
            sctx = Shortcuts::Ctx_Global;
            ImGuiWindow* nr = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
            auto navIs = [&](const char* n) { return nr && nr == ImGui::FindWindowByName(n); };
            if (navIs("Scene Hierarchy"))                          sctx |= Shortcuts::Ctx_Hierarchy;
            else if (m_AssetBrowserFocused)                        sctx |= Shortcuts::Ctx_Project;
            else if (navIs("Inspector"))                           sctx |= Shortcuts::Ctx_Inspector;
            else if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))  sctx |= Shortcuts::Ctx_Viewport;
        }
        Shortcuts::BeginFrame(sctx);
    }

    if (keyboardFree) {
        ImGuiIO& io = ImGui::GetIO();

        // The Scene Hierarchy takes plain letters for type-to-select and the arrows for tree
        // nav (#236); while it holds focus, a bare W/E/R/T/A/F must not also reach the viewport
        // as a tool switch or frame. Ctrl-combos (save, undo, duplicate) are unaffected.
        ImGuiWindow* navRoot = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
        const bool hierarchyOwnsLetters = navRoot && navRoot == ImGui::FindWindowByName("Scene Hierarchy");

        // Q/W/E/R/T/Y viewport tools (Unity's scheme, + Y for the combined gizmo, #236 E).
        // Ctx_Viewport, so the dispatcher already withholds them while a panel owns the
        // keyboard or Right-drag fly is active. Selecting any transform tool exits the Hand tool.
        if (Shortcuts::Triggered("tools.hand"))      { m_HandTool = true; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.move"))      { m_GizmoOp = GizmoOp::Translate; m_HandTool = false; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.rotate"))    { m_GizmoOp = GizmoOp::Rotate;    m_HandTool = false; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.scale"))     { m_GizmoOp = GizmoOp::Scale;     m_HandTool = false; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.rect"))      { m_GizmoOp = GizmoOp::Rect;      m_HandTool = false; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.transform")) { m_GizmoOp = GizmoOp::Universal; m_HandTool = false; m_MeasureTool = false; }
        if (Shortcuts::Triggered("tools.measure"))   { m_MeasureTool = !m_MeasureTool; m_MeasureCount = 0; m_HandTool = false; }
        if (m_MeasureTool && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { m_MeasureTool = false; m_MeasureCount = 0; }
        // Shift+A quick-add (Blender's binding) — opens the Add menu as a popup at the cursor.
        if (Shortcuts::Triggered("gameobject.quickAdd")) m_OpenQuickAdd = true;

        // Ctrl+Z / Ctrl+Y walk the selection history while the last action was a selection
        // change (#236 R2); otherwise they're the scene undo/redo. Ctrl+[ / Ctrl+] stay pure
        // selection nav regardless.
        if (Shortcuts::Triggered("editor.undo")) {
            if (m_CtrlZSelectionMode && CanSelectionHistoryBack()) SelectionHistoryBack(world);
            else { Undo(world, assets); m_CtrlZSelectionMode = false; m_SelHistoryNavigating = true; }
        }
        if (Shortcuts::Triggered("editor.redo")) {
            if (m_CtrlZSelectionMode && CanSelectionHistoryForward()) SelectionHistoryForward(world);
            else { Redo(world, assets); m_CtrlZSelectionMode = false; m_SelHistoryNavigating = true; }
        }
        if (Shortcuts::Triggered("editor.saveAs"))     DoSaveAs(world, assets);
        else if (Shortcuts::Triggered("editor.save"))  DoSave(world, assets); // prompts for a location if untitled
        if (Shortcuts::Triggered("editor.newScene")) RequestNewScene(world, assets);
        // GameObject-menu parity (#236): Ctrl+Shift+N = Create Empty Child (of the active
        // selection, or a root Empty if nothing's selected); Alt+Shift+A = Toggle Active State.
        if (Shortcuts::Triggered("gameobject.createEmptyChild")) {
            CreateEmptyChild(world,
                (m_Selected != entt::null && world.Registry.valid(m_Selected)) ? m_Selected : entt::null);
        }
        if (Shortcuts::Triggered("gameobject.toggleActive")) ToggleSelectionActive(world);
        if (Shortcuts::Triggered("editor.preferences")) m_ShowPreferences = true;
        if (Shortcuts::Triggered("editor.openScene")) {
            RequestOpenScene(world, assets, FileDialog::OpenFile("Scene Files\0*.json\0All Files\0*.*\0", m_Window));
        }

        // Ctrl+1..4 — focus a panel (new with the Shortcuts Manager, #236 F).
        if (Shortcuts::Triggered("panel.focus.hierarchy")) ImGui::SetWindowFocus("Scene Hierarchy");
        if (Shortcuts::Triggered("panel.focus.inspector")) ImGui::SetWindowFocus("Inspector");
        if (Shortcuts::Triggered("panel.focus.project"))   ImGui::SetWindowFocus("Asset Browser");
        if (Shortcuts::Triggered("panel.focus.console"))   ImGui::SetWindowFocus("Console");
        if (HasAnySelection() && Shortcuts::Triggered("edit.delete")) {
            DeleteSelection(world);
        } else if (!m_SelectedAssetKey.empty() && m_RenamingAssetKey.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            std::vector<AssetKeyRef> toDelete;
            toDelete.push_back({m_SelectedAssetKey, m_SelectedAssetIsFolder});
            for (const auto& e : m_ExtraAssetSelection) toDelete.push_back(e);
            RequestDeleteAssets(world, assets, toDelete, io.KeyShift);
        }
        // The Asset Browser "owns" Ctrl+D / F / Delete only when it's focused AND actually has
        // an asset selected — otherwise those keys belong to the scene selection. Without the
        // second half, m_AssetBrowserFocused stays sticky-true after any Asset Browser click
        // (clicking the 3D viewport can't move ImGui focus off it — the viewport is just an
        // ImGui::Image), which silently swallowed scene-object Ctrl+D forever.
        bool assetBrowserOwnsKeys = m_AssetBrowserFocused &&
            (!m_SelectedAssetKey.empty() || !m_ExtraAssetSelection.empty());

        // F / Shift+F are Ctx_Viewport — the dispatcher already withholds them while a panel
        // owns the keyboard, so only the selection guard is needed here.
        if (HasAnySelection() && Shortcuts::Triggered("view.lockToSelection"))
            SetLockViewToSelection(!m_LockViewToSelection); // Shift+F — toggle camera-follow (#236 E)
        if (HasAnySelection() && Shortcuts::Triggered("view.frameSelection"))
            FocusOnSelection(world, editorCamera);
        if (HasAnySelection() && !assetBrowserOwnsKeys && Shortcuts::Triggered("edit.duplicate"))
            DuplicateSelection(world, assets, /*inPlace=*/true); // Ctrl+D duplicates without the (1,0,1) nudge (#236 F)
        if (HasAnySelection() && !assetBrowserOwnsKeys && Shortcuts::Triggered("edit.duplicateArray"))
            m_ShowArrayDuplicate = true;

        // Edit-menu selection ops (#236). The Hierarchy owns Ctrl+A when it's focused (select all
        // *visible* rows); elsewhere Ctrl+A selects every entity. Ctrl+Shift+A deselects, Ctrl+I
        // inverts. These are Ctx_Global, so the panel guards still matter.
        if (!assetBrowserOwnsKeys && !hierarchyOwnsLetters) {
            if (Shortcuts::Triggered("edit.deselectAll"))         ClearSelection();
            else if (Shortcuts::Triggered("edit.selectAll"))      SelectAllEntities(world);
            else if (Shortcuts::Triggered("edit.invertSelection")) InvertSelection(world);
        }

        if (Shortcuts::Triggered("select.historyBack"))    SelectionHistoryBack(world);
        if (Shortcuts::Triggered("select.historyForward")) SelectionHistoryForward(world);

        // Ctrl+Shift+F — snap the selected Camera entity to the editor viewport (Unity's Align
        // With View). Mirrors the Inspector's "Align to View" button.
        if (Shortcuts::Triggered("camera.alignToView") &&
                m_Selected != entt::null && world.Registry.valid(m_Selected) &&
                world.Registry.all_of<CameraComponent>(m_Selected)) {
            PushUndo(world, "Align Camera to View");
            auto& t = world.Registry.get<TransformComponent>(m_Selected);
            t.Position = editorCamera.Position;
            glm::vec3 d = glm::normalize(editorCamera.Front());
            t.RotationEuler = glm::vec3(
                glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                glm::degrees(std::atan2(-d.x, -d.z)), 0.0f);
            world.Registry.get<CameraComponent>(m_Selected).FovDegrees = editorCamera.Fov;
        }

        // Unity Project-window-style Asset Browser shortcuts. The four discrete actions are
        // Ctx_Project shortcuts (rebindable in Preferences); folder navigation (Enter /
        // Backspace / arrows) stays hard-wired — it's traversal, not a named command.
        if (m_AssetBrowserFocused && m_RenamingAssetKey.empty()) {
            if (Shortcuts::Triggered("project.focusSearch")) m_AssetSearchFocusRequested = true;
            if (Shortcuts::Triggered("project.refresh"))     RefreshAssetBrowser(); // #236 G
            if (Shortcuts::Triggered("project.frameSelected") && !m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) {
                // "Frame selected" — navigate the browser to the asset's containing folder.
                m_CurrentAssetFolder = assets.AssetFolder(m_SelectedAssetKey);
            }
            if (Shortcuts::Triggered("project.duplicate") && !m_SelectedAssetKey.empty()) {
                DuplicateSelectedAsset(world, assets);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                if (m_SelectedAssetIsFolder) m_CurrentAssetFolder = m_SelectedAssetKey;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                m_CurrentAssetFolder = ParentFolderOf(m_CurrentAssetFolder);
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !m_CurrentAssetFolder.empty()) {
                m_ExpandedAssetFolders.insert(m_CurrentAssetFolder);
            } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !m_CurrentAssetFolder.empty()) {
                if (m_ExpandedAssetFolders.count(m_CurrentAssetFolder)) m_ExpandedAssetFolders.erase(m_CurrentAssetFolder);
                else m_CurrentAssetFolder = ParentFolderOf(m_CurrentAssetFolder);
            }
        }

        // Clipboard. Cut is copy-then-delete, so a cancelled paste still leaves the objects
        // recoverable through undo rather than gone.
        if (Shortcuts::Triggered("edit.copy") && HasAnySelection()) CopySelection(world);
        if (Shortcuts::Triggered("edit.cut") && HasAnySelection()) {
            CopySelection(world);
            DeleteSelection(world);
        }
        if (Shortcuts::Triggered("edit.paste")) PasteClipboard(world, assets);

        // View presets (Ctx_Viewport; row/numpad interchangeable, handled in the dispatcher).
        // The three opposite faces are unbound by default — Ctrl+1..4 is panel focus now.
        if (Shortcuts::Triggered("view.top"))    SnapToView(world, editorCamera, -90.0f, -89.9f, true);
        if (Shortcuts::Triggered("view.bottom")) SnapToView(world, editorCamera, -90.0f,  89.9f, true);
        if (Shortcuts::Triggered("view.front"))  SnapToView(world, editorCamera, -90.0f,   0.0f, true);
        if (Shortcuts::Triggered("view.back"))   SnapToView(world, editorCamera,  90.0f,   0.0f, true);
        if (Shortcuts::Triggered("view.right"))  SnapToView(world, editorCamera, 180.0f,   0.0f, true);
        if (Shortcuts::Triggered("view.left"))   SnapToView(world, editorCamera,   0.0f,   0.0f, true);
        if (Shortcuts::Triggered("view.persp"))  SnapToView(world, editorCamera, -45.0f, -35.264f, true);
        if (Shortcuts::Triggered("view.toggleOrtho")) ToggleOrthographic(world, editorCamera);

        // F2 renames whichever selection is "live": a scene object takes priority over an Asset
        // Browser entry, matching which panel the user most likely just clicked in. In Play mode
        // F2 is Pause instead (#236) — rename is still one double-click away in the Hierarchy.
        if (!m_InPlayMode && Shortcuts::Triggered("edit.rename")) {
            if (HasAnySelection()) {
                BeginRenameEntity(m_Selected);
            } else if (!m_SelectedAssetKey.empty() && m_RenamingAssetKey.empty() && m_ExtraAssetSelection.empty()) {
                std::string currentName = m_SelectedAssetIsFolder ? LeafNameOf(m_SelectedAssetKey) : assets.DisplayName(m_SelectedAssetKey);
                BeginRenameAsset(m_SelectedAssetKey, m_SelectedAssetIsFolder, currentName);
            }
        }
    }

    if (hasHover) {
        // Not-yet-grabbed indicator: yellow circle at the vertex the cursor is closest to.
        // Screen position must be computed against the VIEWPORT sub-rect (m_ViewportPos/Size),
        // not the full window (w/h) — the viewport doesn't start at the window's top-left once
        // the Hierarchy/Inspector/Asset Browser panels are docked around it, and its aspect
        // ratio isn't the whole window's either. Using w/h here (as this used to) computed the
        // right NDC coordinates against the wrong rect, landing the dot wherever that rect
        // mismatch happened to put it instead of on the actual vertex.
        // World matrix — viewProj is world-space, so a parented model's dot landed off its
        // vertex when this composed the local transform instead. (#224)
        glm::mat4 model = world.GetCachedWorldTransform(m_Selected);
        glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();
        glm::vec4 clip = viewProj * model * glm::vec4(hoverLocal, 1.0f);
        if (clip.w > 0.0001f) {
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            ImVec2 screen(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                          m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
            ImGui::GetForegroundDrawList()->AddCircle(screen, 8.0f, IM_COL32(255, 217, 77, 230), 0, 2.5f);
        }
    }

    if (m_VertexDragActive && IsVertexDraggable(world, m_Selected)) {
        // Actively grabbed: filled yellow dot tracking the vertex's live (post-drag) position.
        // World matrix, same reason as the hover circle above. (#224)
        glm::mat4 model = world.GetCachedWorldTransform(m_Selected);
        glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();
        glm::vec4 clip = viewProj * model * glm::vec4(m_VertexDragLocal, 1.0f);
        if (clip.w > 0.0001f) {
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            ImVec2 screen(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                          m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
            ImGui::GetForegroundDrawList()->AddCircle(screen, 7.0f, IM_COL32(255, 217, 77, 255), 0, 2.0f);
            ImGui::GetForegroundDrawList()->AddCircleFilled(screen, 3.0f, IM_COL32(255, 217, 77, 255));
        }
    }

    // Frame-on-select (opt-in, #69): SelectItem set this when the selection changed.
    if (m_PendingFrameSelect) {
        m_PendingFrameSelect = false;
        if (HasAnySelection()) FocusOnSelection(world, editorCamera);
    }

    // Off-screen selection indicator (#69): if the selected object is outside the viewport,
    // draw a marker clamped to the nearest edge, pointing toward it, so a selection made in the
    // Hierarchy isn't invisible with no hint where it is.
    if (HasAnySelection() && m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f) {
        glm::vec3 center;
        if (GetSelectionCenter(world, center)) {
            glm::mat4 vp = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();
            glm::vec4 clip = vp * glm::vec4(center, 1.0f);
            bool behind = clip.w <= 0.0001f;
            glm::vec2 ndc = behind ? glm::vec2(0.0f) : glm::vec2(clip.x, clip.y) / clip.w;
            bool offscreen = behind || ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f;
            if (offscreen) {
                // Direction from viewport center toward the target, in screen space.
                glm::vec2 dir = behind ? glm::vec2(-ndc.x, ndc.y) : glm::vec2(ndc.x, -ndc.y);
                if (glm::dot(dir, dir) < 1.0e-6f) dir = glm::vec2(0.0f, 1.0f);
                dir = glm::normalize(dir);
                ImVec2 vpCenter(m_ViewportPos.x + m_ViewportSize.x * 0.5f, m_ViewportPos.y + m_ViewportSize.y * 0.5f);
                float mx = m_ViewportSize.x * 0.5f - 28.0f;
                float my = m_ViewportSize.y * 0.5f - 28.0f;
                // Scale the unit direction out to whichever axis hits the inset edge first.
                float sx = std::fabs(dir.x) > 1.0e-4f ? mx / std::fabs(dir.x) : 1.0e9f;
                float sy = std::fabs(dir.y) > 1.0e-4f ? my / std::fabs(dir.y) : 1.0e9f;
                float s = std::min(sx, sy);
                ImVec2 p(vpCenter.x + dir.x * s, vpCenter.y + dir.y * s);

                ImDrawList* dl = ImGui::GetForegroundDrawList();
                const ImU32 col = IM_COL32(255, 140, 26, 235);
                // A small triangle pointing along `dir`.
                ImVec2 perp(-dir.y, dir.x);
                ImVec2 tip(p.x + dir.x * 11.0f, p.y + dir.y * 11.0f);
                ImVec2 b1(p.x - dir.x * 6.0f + perp.x * 8.0f, p.y - dir.y * 6.0f + perp.y * 8.0f);
                ImVec2 b2(p.x - dir.x * 6.0f - perp.x * 8.0f, p.y - dir.y * 6.0f - perp.y * 8.0f);
                dl->AddCircleFilled(p, 13.0f, IM_COL32(20, 20, 20, 170));
                dl->AddTriangleFilled(tip, b1, b2, col);
            }
        }
    }

    // Quick-add popup — opened by Shift+A (shortcut handler above) or the Inspector empty
    // state's "Add to Scene" button. Handled here, at the very end of the frame's UI, so it
    // works no matter which earlier panel set the flag. Positioned at the cursor.
    if (m_OpenQuickAdd) {
        ImGui::OpenPopup("##QuickAdd");
        m_OpenQuickAdd = false;
    }
    if (ImGui::BeginPopup("##QuickAdd")) {
        ImGui::SeparatorText(ICON_FA_CUBES "  Create");
        DrawAddEntityItems(world, assets, editorCamera);
        ImGui::EndPopup();
    }

    // Play-mode tint (#236 R2): a warm border around the WHOLE editor window (not just the
    // Scene rect — that hides behind the Game tab) + a centred tag, so it's unmistakable that
    // edits now revert on Stop.
    if (m_InPlayMode) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImU32 col = IM_COL32(255, 140, 40, 230);
        const float t = 3.0f;
        ImVec2 a(vp->Pos.x + t * 0.5f, vp->Pos.y + t * 0.5f);
        ImVec2 b(vp->Pos.x + vp->Size.x - t * 0.5f, vp->Pos.y + vp->Size.y - t * 0.5f);
        dl->AddRect(a, b, col, 0.0f, 0, t);
        const char* tag = "PLAY MODE \xE2\x80\x94 changes revert on Stop";
        ImVec2 ts = ImGui::CalcTextSize(tag);
        ImVec2 tp(vp->Pos.x + vp->Size.x * 0.5f - ts.x * 0.5f, vp->Pos.y + 4.0f);
        dl->AddRectFilled(ImVec2(tp.x - 7.0f, tp.y - 2.0f), ImVec2(tp.x + ts.x + 7.0f, tp.y + ts.y + 3.0f),
                          IM_COL32(20, 20, 24, 210), 3.0f);
        dl->AddText(tp, col, tag);
    }

    // Transient fly-speed readout — large, centred toward the bottom of the viewport (#236 R2).
    // Poked by main.cpp on a scroll-driven speed change; fades over its last ~0.6s.
    if (m_FlySpeedHudTimer > 0.0f && m_ViewportSize.x > 4.0f) {
        m_FlySpeedHudTimer -= dt;
        const float a = std::clamp(m_FlySpeedHudTimer / 0.6f, 0.0f, 1.0f);
        char buf[48];
        std::snprintf(buf, sizeof(buf), ICON_FA_GAUGE_HIGH "  Fly speed  %.1f",
                      EditorSettings::Get().SceneCameraFlySpeed);
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * 1.7f;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
        ImVec2 c(m_ViewportPos.x + m_ViewportSize.x * 0.5f,
                 m_ViewportPos.y + m_ViewportSize.y - 64.0f);
        ImVec2 p(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(p.x - 16.0f, p.y - 9.0f), ImVec2(p.x + ts.x + 16.0f, p.y + ts.y + 9.0f),
                          IM_COL32(18, 22, 30, (int)(220 * a)), 8.0f);
        dl->AddText(font, fs, p, IM_COL32(240, 248, 255, (int)(255 * a)), buf);
    }

    DrawArrayDuplicateModal(world, assets); // #236 R2

    // Save-layout-preset name prompt (#236 R2).
    if (m_ShowSaveLayout) {
        if (!ImGui::IsPopupOpen("Save Layout##SaveLayout")) ImGui::OpenPopup("Save Layout##SaveLayout");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Save Layout##SaveLayout", &m_ShowSaveLayout, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputTextWithHint("##layoutname", "Preset name", m_SaveLayoutName,
                sizeof(m_SaveLayoutName), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Separator();
            const bool named = m_SaveLayoutName[0] != '\0';
            ImGui::BeginDisabled(!named);
            if (PrimaryButton("Save", ImVec2(110.0f, 0.0f)) || (enter && named)) {
                SaveLayoutPreset(m_SaveLayoutName);
                m_ShowSaveLayout = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (PrimaryButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_ShowSaveLayout = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Fold this frame's selection into the back/forward history (#236 R2). Last thing in Draw,
    // so it sees the net result of every panel and shortcut that ran this frame.
    RecordSelectionHistory();
}
bool EditorLayer::AnyModalOpen() const {
    return ImGui::GetTopMostPopupModal() != nullptr;
}
