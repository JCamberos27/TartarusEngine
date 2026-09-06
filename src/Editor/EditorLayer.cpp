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
#include "GLStateCache.h"
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

    // --- Editor theme: a modern, dark-slate look (not pitch black), monochrome with one
    // restrained cool accent. Square-ish corners and compact spacing; the 3D viewport still
    // owns all the saturated colour (axis R/G/B, the warm selection outline). (#92)
    ImGuiStyle& style = ImGui::GetStyle();
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
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    // Palette (base greys + accent) is theme-dependent — set by ApplyEditorTheme() so the
    // Preferences > General theme combo can re-run it live. Sizes/rounding above are shared.
    ApplyEditorTheme();

    // Scale every size/padding/rounding set above (and ImGui's own defaults) by the monitor's
    // content scale, so spacing keeps its proportions instead of staying pinned to 96-DPI pixel
    // counts while the fonts below grow to match.
    style.ScaleAllSizes(m_UIScale);

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
        // Prism: static near-black chrome. Everything with colour is set by ApplyPrismAnimation,
        // called here so a fresh switch looks right and then again every frame from Draw().
        style.Colors[ImGuiCol_WindowBg]        = ImVec4(0.030f, 0.030f, 0.036f, 1.00f);
        style.Colors[ImGuiCol_PopupBg]         = ImVec4(0.020f, 0.020f, 0.026f, 0.98f);
        style.Colors[ImGuiCol_MenuBarBg]       = ImVec4(0.020f, 0.020f, 0.026f, 1.00f);
        style.Colors[ImGuiCol_TitleBg]         = ImVec4(0.012f, 0.012f, 0.016f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed]= ImVec4(0.012f, 0.012f, 0.016f, 1.00f);
        style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.045f, 0.045f, 0.055f, 1.00f);
        style.Colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.085f, 0.085f, 0.105f, 1.00f);
        style.Colors[ImGuiCol_FrameBgActive]   = ImVec4(0.120f, 0.120f, 0.150f, 1.00f);
        style.Colors[ImGuiCol_ChildBg]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        style.Colors[ImGuiCol_BorderShadow]    = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        style.Colors[ImGuiCol_ScrollbarBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        style.Colors[ImGuiCol_Tab]             = ImVec4(0.022f, 0.022f, 0.028f, 1.00f);
        style.Colors[ImGuiCol_TabSelected]     = ImVec4(0.050f, 0.050f, 0.062f, 1.00f);
        style.Colors[ImGuiCol_TabDimmed]       = ImVec4(0.016f, 0.016f, 0.020f, 1.00f);
        style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.030f, 0.030f, 0.038f, 1.00f);
        style.Colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        style.Colors[ImGuiCol_DockingEmptyBg]  = ImVec4(0.016f, 0.016f, 0.020f, 1.00f);
        style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.010f, 0.010f, 0.016f, 0.32f);
        ApplyPrismAnimation(m_ThemeHue);
        return;
    }

    // --- Dark Slate ------------------------------------------------------------------------
    // Panels sit a couple of steps above pure black; inputs recess a step below the panel;
    // popups/menus match the panel.
    style.Colors[ImGuiCol_WindowBg]        = ImVec4(0.137f, 0.137f, 0.145f, 1.00f);
    style.Colors[ImGuiCol_ChildBg]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    style.Colors[ImGuiCol_PopupBg]         = ImVec4(0.117f, 0.117f, 0.125f, 0.98f);
    style.Colors[ImGuiCol_MenuBarBg]       = ImVec4(0.117f, 0.117f, 0.125f, 1.00f);
    style.Colors[ImGuiCol_TitleBg]         = ImVec4(0.098f, 0.098f, 0.105f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.125f, 0.125f, 0.133f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]= ImVec4(0.098f, 0.098f, 0.105f, 1.00f);
    style.Colors[ImGuiCol_Border]          = ImVec4(0.290f, 0.290f, 0.320f, 0.50f);
    style.Colors[ImGuiCol_BorderShadow]    = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    style.Colors[ImGuiCol_Separator]       = ImVec4(0.240f, 0.240f, 0.270f, 0.55f);
    style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.100f, 0.100f, 0.108f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.160f, 0.160f, 0.172f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive]   = ImVec4(0.196f, 0.200f, 0.223f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    style.Colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.300f, 0.300f, 0.330f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.380f, 0.380f, 0.420f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.460f, 0.460f, 0.510f, 1.00f);
    style.Colors[ImGuiCol_Text]            = ImVec4(0.860f, 0.870f, 0.890f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]    = ImVec4(0.450f, 0.460f, 0.500f, 1.00f);

    // The one accent: a desaturated cool slate.
    ImVec4 accent(0.255f, 0.275f, 0.325f, 1.00f);
    ImVec4 accentHovered(0.325f, 0.350f, 0.415f, 1.00f);
    ImVec4 accentActive(0.400f, 0.435f, 0.520f, 1.00f);

    style.Colors[ImGuiCol_CheckMark]         = ImVec4(0.640f, 0.680f, 0.780f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]        = accentActive;
    style.Colors[ImGuiCol_SliderGrabActive]  = ImVec4(0.520f, 0.560f, 0.660f, 1.00f);
    style.Colors[ImGuiCol_Button]            = ImVec4(0.185f, 0.185f, 0.200f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered]     = ImVec4(0.245f, 0.250f, 0.275f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]      = ImVec4(0.300f, 0.310f, 0.345f, 1.00f);
    style.Colors[ImGuiCol_Header]            = accent;
    style.Colors[ImGuiCol_HeaderHovered]     = accentHovered;
    style.Colors[ImGuiCol_HeaderActive]      = accentActive;
    style.Colors[ImGuiCol_SeparatorHovered]  = accentHovered;
    style.Colors[ImGuiCol_SeparatorActive]   = accentActive;
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    style.Colors[ImGuiCol_ResizeGripHovered] = accentHovered;
    style.Colors[ImGuiCol_ResizeGripActive]  = accentActive;
    style.Colors[ImGuiCol_Tab]                        = ImVec4(0.130f, 0.130f, 0.138f, 1.00f);
    style.Colors[ImGuiCol_TabHovered]                 = ImVec4(0.220f, 0.225f, 0.245f, 1.00f);
    style.Colors[ImGuiCol_TabSelected]                = ImVec4(0.185f, 0.190f, 0.205f, 1.00f);
    style.Colors[ImGuiCol_TabDimmed]                  = ImVec4(0.110f, 0.110f, 0.118f, 1.00f);
    style.Colors[ImGuiCol_TabDimmedSelected]          = ImVec4(0.155f, 0.158f, 0.170f, 1.00f);
    style.Colors[ImGuiCol_TabSelectedOverline]        = ImVec4(0.450f, 0.490f, 0.600f, 0.90f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline]  = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    style.Colors[ImGuiCol_TextSelectedBg]   = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    style.Colors[ImGuiCol_NavCursor]        = accentActive;
    style.Colors[ImGuiCol_DockingPreview]   = ImVec4(accentActive.x, accentActive.y, accentActive.z, 0.55f);
    style.Colors[ImGuiCol_DockingEmptyBg]   = ImVec4(0.090f, 0.090f, 0.098f, 1.00f);
    // A quiet dark scrim, not ImGui's default 35% white wash — paired with the viewport frost
    // (main.cpp) it reads as a blurred backdrop behind modal dialogs.
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.020f, 0.020f, 0.028f, 0.32f);
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
            static const char* kThemeLabels[] = { "Dark Slate", "Prism", "Windows XP" };
            int theme = std::clamp(prefs.EditorTheme, 0, (int)IM_ARRAYSIZE(kThemeLabels) - 1);
            ImGui::SetNextItemWidth(kw);
            if (ImGui::Combo("Theme", &theme, kThemeLabels, IM_ARRAYSIZE(kThemeLabels))) {
                prefs.EditorTheme = theme;
                ApplyEditorTheme();      // colours only — applies immediately, no restart
                EditorSettings::Save();
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Dark Slate: monochrome greys + one cool accent.\n"
                                     "Prism: near-black chrome; the accent, buttons and text\n"
                                     "tint drift through the spectrum together.\n"
                                     "Windows XP: the Luna \"Blue\" scheme - beige chrome,\n"
                                     "black text, white fields, Luna-blue selection.");
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
        if (ImGui::DragFloat("Line spacing", &prefs.GridMinorSpacing, 0.05f, 0.05f, 50.0f, "%.2f")) {
            prefs.GridMinorSpacing = std::clamp(prefs.GridMinorSpacing, 0.05f, 50.0f);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("World units between minor lines. Also the step used by grid-snapped placement.");
        ImGui::SetNextItemWidth(kw);
        if (ImGui::DragInt("Major line every", &prefs.GridMajorEvery, 0.2f, 2, 50, "%d cells")) {
            prefs.GridMajorEvery = std::clamp(prefs.GridMajorEvery, 2, 50);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("A brighter major line is drawn every N minor cells.");
        ImGui::SetNextItemWidth(kw);
        if (ImGui::DragFloat("Fade distance", &prefs.GridFadeDistance, 1.0f, 10.0f, 1000.0f, "%.0f m")) {
            prefs.GridFadeDistance = std::clamp(prefs.GridFadeDistance, 10.0f, 1000.0f);
        }
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
        ImGui::DragFloat("Position snap", &m_SnapTranslation, 0.05f, 0.01f, 50.0f, "%.2f");
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Rotation snap", &m_SnapRotationDeg, 1.0f, 180.0f, "%.1f deg");
        ImGui::SetNextItemWidth(kw);
        ImGui::DragFloat("Scale snap", &m_SnapScale, 0.01f, 0.01f, 5.0f, "%.2f");
        ImGui::TextDisabled("The grid + snap on/off toggles are on the toolbar.");
        break;

    case 3: // Environment
        ImGui::SeparatorText("Environment");
        ImGui::ColorEdit3("Horizon color", &world.SkyHorizonColor.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Sky Color");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Sky colour at the horizon.");
        ImGui::ColorEdit3("Zenith color", &world.SkyZenithColor.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Sky Color");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Sky colour straight up.");
        // #196: the sky now lights the scene (irradiance + reflection probes baked from these
        // two colours), so this is the ambient control the engine previously had nowhere.
        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Ambient intensity", &world.SkyAmbientIntensity, 0.0f, 3.0f, "%.2f x");
        if (ImGui::IsItemActivated()) PushUndo(world, "Edit Ambient Intensity");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(
            "Strength of the image-based ambient light and reflections baked from the sky colours "
            "above. 1.0 is physically consistent; 0 disables environment lighting entirely.");
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

        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Exposure (EV)", &prefs.ExposureEV, -6.0f, 6.0f, "%+.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Photographic stops applied before the tone curve. 0 = neutral. Applies live.");

        static const char* kTonemapLabels[] = { "Reinhard", "ACES", "AgX" };
        int tm = std::clamp(prefs.TonemapOperator, 0, 2);
        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("Tone mapping", &tm, kTonemapLabels, IM_ARRAYSIZE(kTonemapLabels))) {
            prefs.TonemapOperator = tm;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Curve that maps linear HDR to display. ACES = punchy filmic; AgX = gentler, less hue shift.");

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

        if (ImGui::Checkbox("Cast sun shadows", &prefs.ShadowsEnabled)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Cascaded shadow maps for the Directional light. Point/spot shadows are a later milestone.");

        static const char* kShadowResLabels[] = { "1024", "2048", "4096" };
        static const int   kShadowResValues[] = { 1024, 2048, 4096 };
        int srIdx = 1;
        for (int i = 0; i < 3; ++i) if (kShadowResValues[i] == prefs.ShadowResolution) { srIdx = i; break; }
        if (!prefs.ShadowsEnabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("Shadow resolution", &srIdx, kShadowResLabels, IM_ARRAYSIZE(kShadowResLabels))) {
            prefs.ShadowResolution = kShadowResValues[srIdx];
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Per-cascade shadow map size. 4x memory + fill from 2048 to 4096.");

        static const char* kCascadeLabels[] = { "2", "3", "4" };
        int ccIdx = std::clamp(prefs.ShadowCascades - 2, 0, 2);
        ImGui::SetNextItemWidth(kw);
        if (ImGui::Combo("Cascades", &ccIdx, kCascadeLabels, IM_ARRAYSIZE(kCascadeLabels))) {
            prefs.ShadowCascades = ccIdx + 2;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Number of shadow cascades. Fewer = cheaper depth passes, coarser shadows far from the camera.");

        ImGui::SetNextItemWidth(kw);
        EditorUI::SliderFloat("Shadow distance", &prefs.ShadowDistance, 10.0f, 500.0f, "%.0f m");
        if (ImGui::IsItemDeactivatedAfterEdit()) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("How far from the camera the cascades cover. Shorter = crisper shadows.");
        if (!prefs.ShadowsEnabled) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextDisabled("Changes apply immediately. All of these persist in editor_prefs.json.");
        break;
    }

    case 6: { // Shortcuts
        ImGui::SeparatorText("Shortcuts");
        ImGui::SetNextItemWidth(-1.0f);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", m_PrefsShortcutFilter.c_str());
        if (ImGui::InputTextWithHint("##scfilter", ICON_FA_MAGNIFYING_GLASS "  Filter...", buf, sizeof(buf)))
            m_PrefsShortcutFilter = buf;
        static const std::pair<const char*, const char*> kShortcuts[] = {
            {"Fly camera", "hold RMB + WASDQE"},
            {"Zoom / dolly", "scroll wheel  ·  Alt+RMB drag"},
            {"Pan view", "middle-drag"},
            {"Orbit selection", "Alt + left-drag"},
            {"View presets", "1 / 3 / 7 / 0  (or numpad; Ctrl = opposite side)"},
            {"Toggle orthographic", "5  (or numpad 5)"},
            {"Frame selection", "F"},
            {"Quick add (Add menu at cursor)", "Shift+A"},
            {"Align selected Camera to view", "Ctrl+Shift+F"},
            {"Gizmo: move / rotate / scale / rect", "W / E / R / T"},
            {"Vertex grab", "hold V"},
            {"Multi-select", "Ctrl+Click  ·  drag a box"},
            {"Undo / Redo", "Ctrl+Z / Ctrl+Y"},
            {"Save / Save As", "Ctrl+S / Ctrl+Shift+S"},
            {"New / Open scene", "Ctrl+N / Ctrl+O"},
            {"Duplicate", "Ctrl+D"},
            {"Copy / Cut / Paste", "Ctrl+C / Ctrl+X / Ctrl+V"},
            {"Delete selection", "Delete"},
            {"Rename selection", "F2  (or double-click in Hierarchy)"},
            {"Open Preferences", "Ctrl+,"},
            {"Toggle fullscreen", "F11"},
            {"Screenshot (Capture tool)", "Print Screen"},
            {"Play / Stop", "F1"},
            {"Release mouse & keyboard from the running game", "Esc"},
        };
        if (ImGui::BeginTable("##sctable", 2,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [action, keys] : kShortcuts) {
                if (!MatchesFilter(m_PrefsShortcutFilter, std::string(action) + " " + keys)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(action);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", keys);
            }
            ImGui::EndTable();
        }
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
    DrawPreferencesWindow(world);
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
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
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

    if (m_ShowInspector) DrawInspector(world, assets, dt);
    // (Console, the Statistics HUD, the top toolbar, the Asset Browser and the Scene Hierarchy are
    // drawn by the reloadable editor module — see main.cpp's editorModule.Draw(), which runs
    // immediately after this call, still inside the same ImGui frame and dockspace. The Asset
    // Browser's grid and the Hierarchy's entity tree are still host code, reached back via
    // EditorModuleHostAPI::DrawAssetGridBody / DrawHierarchyTreeBody.)
    if (!m_HideOverlaysThisFrame) {
        // Exponential smoothing of the frame time: a raw per-frame ms figure flickers too fast
        // to read. Used by the viewport status bar just below and by the reloadable Stats HUD
        // (exposed through EditorModuleHostAPI::GetSmoothedFrameMs). Lived inside DrawStatsPanel
        // before that panel moved out; kept on the same !m_HideOverlaysThisFrame path so its
        // cadence is unchanged.
        const float frameMs = dt * 1000.0f;
        m_SmoothedFrameMs = m_SmoothedFrameMs * 0.92f + frameMs * 0.08f;

        DrawViewportStatusBar();
        DrawHistoryPanel(world, assets);
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

    if (!m_HideOverlaysThisFrame) {
        DrawEntityIcons(world, editorCamera);
        DrawLightGizmos(world, editorCamera);
    }

    // Drawn (and its hover/drag state refreshed) before picking runs below, so a click that
    // lands on the nav gizmo's rotate ring or tool buttons doesn't also start a viewport
    // box-select/pick underneath it.
    if (!m_HideOverlaysThisFrame) DrawViewGizmo(world, editorCamera);

    if (!vHeld) {
        UpdateLightHandles(world, editorCamera);
        HandleViewportPicking(world, editorCamera);
        if (m_ShowGizmos && !m_HideOverlaysThisFrame) DrawGizmo(world, editorCamera);
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

    if (!ImGui::GetIO().WantTextInput && !m_GameInputActive && !OtherWindowOwnsKeyboard()) {
        ImGuiIO& io = ImGui::GetIO();

        // W/E/R/T gizmo-tool shortcuts (Unity's own scheme) only when Right-drag isn't held —
        // WASDQE fly the camera during Right-drag instead (see main.cpp's UpdateEditorCamera),
        // so without this guard just walking forward with W would also switch tools every time.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_GizmoOp = GizmoOp::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_GizmoOp = GizmoOp::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_GizmoOp = GizmoOp::Scale;
            if (ImGui::IsKeyPressed(ImGuiKey_T)) m_GizmoOp = GizmoOp::Rect;
            // Shift+A quick-add (Blender's binding) — opens the Add menu as a popup at the
            // cursor. Guarded with the others so fly-mode's A (strafe left) doesn't trigger it.
            if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A)) {
                m_OpenQuickAdd = true;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo(world, assets);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo(world, assets);
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S)) {
            DoSaveAs(world, assets);
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            DoSave(world, assets); // prompts for a location if the scene is untitled (New Scene)
        }
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N)) {
            RequestNewScene(world, assets);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma)) m_ShowPreferences = true;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
            RequestOpenScene(world, assets, FileDialog::OpenFile("Scene Files\0*.json\0All Files\0*.*\0", m_Window));
        }
        if (HasAnySelection() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
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

        if (HasAnySelection() && !assetBrowserOwnsKeys && ImGui::IsKeyPressed(ImGuiKey_F)) FocusOnSelection(world, editorCamera);
        if (HasAnySelection() && !assetBrowserOwnsKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelection(world, assets);

        // Ctrl+Shift+F — snap the selected Camera entity to the editor viewport (Unity's Align
        // With View). Mirrors the Inspector's "Align to View" button.
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F) &&
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

        // Unity Project-window-style Asset Browser shortcuts — only while it has focus, so they
        // don't collide with the scene-selection F/Ctrl+D bindings above. Tab (two-column focus
        // switch), Ctrl+A (multi-select), and every OSX Cmd-key variant from Unity's manual are
        // deliberately not implemented — this browser has one grid+tree layout, no multi-select
        // model for assets, and this is a Windows-only engine.
        if (m_AssetBrowserFocused && m_RenamingAssetKey.empty()) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
                m_AssetSearchFocusRequested = true;
            } else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F) && !m_SelectedAssetKey.empty()) {
                // "Frame selected" — Unity shows the asset in its containing folder; here that
                // just means navigating the browser to it, since it's already always visible
                // once you're in the right folder.
                if (!m_SelectedAssetIsFolder) m_CurrentAssetFolder = assets.AssetFolder(m_SelectedAssetKey);
            } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !m_SelectedAssetKey.empty()) {
                DuplicateSelectedAsset(world, assets);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
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
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && HasAnySelection()) CopySelection(world);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X) && HasAnySelection()) {
            CopySelection(world);
            DeleteSelection(world);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) PasteClipboard(world, assets);

        // View presets — accepted on BOTH the number row and the numpad. Blender/Maya use the
        // numpad; laptops often don't have one; so bind both. Ctrl gets the opposite side.
        auto pressedDigit = [](ImGuiKey row, ImGuiKey pad) {
            return ImGui::IsKeyPressed(row) || ImGui::IsKeyPressed(pad);
        };
        if (pressedDigit(ImGuiKey_7, ImGuiKey_Keypad7)) {
            SnapToView(world, editorCamera, -90.0f, io.KeyCtrl ? 89.9f : -89.9f, true);
        }
        if (pressedDigit(ImGuiKey_1, ImGuiKey_Keypad1)) {
            SnapToView(world, editorCamera, io.KeyCtrl ? 90.0f : -90.0f, 0.0f, true);
        }
        if (pressedDigit(ImGuiKey_3, ImGuiKey_Keypad3)) {
            SnapToView(world, editorCamera, io.KeyCtrl ? 0.0f : 180.0f, 0.0f, true);
        }
        if (pressedDigit(ImGuiKey_0, ImGuiKey_Keypad0)) SnapToView(world, editorCamera, -45.0f, -35.264f, true);
        if (pressedDigit(ImGuiKey_5, ImGuiKey_Keypad5)) ToggleOrthographic(world, editorCamera);

        // F2 renames whichever selection is "live": a scene object takes priority over an Asset
        // Browser entry, matching which panel the user most likely just clicked in.
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
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
        ImGui::SeparatorText(ICON_FA_CUBES "  Add");
        DrawAddEntityItems(world, assets, editorCamera);
        ImGui::EndPopup();
    }
}
bool EditorLayer::AnyModalOpen() const {
    return ImGui::GetTopMostPopupModal() != nullptr;
}
