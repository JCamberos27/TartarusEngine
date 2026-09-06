// Inspector panel: the per-entity component sections, the shared property-row widgets they
// are built from, the asset import inspector, and the material editors. Split out of
// EditorLayer.cpp for build time (#179).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
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
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "ComponentRegistry.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax/ImFloor, ImGuiWindow, and the item-flag helpers the panels use
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // extractEulerAngleYXZ - must match ComposeTransform's order (#108)

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

using namespace EditorInternal;


namespace {

// Approximate blackbody colour (linear RGB, normalised so the brightest channel is 1) for a
// colour temperature in Kelvin. Cheap piecewise fit — good enough for authoring a warm lamp vs
// a cool overcast sky; not a physically exact locus. Clamped to 1000-40000 K.
inline glm::vec3 KelvinToRGB(float kelvin) {
    float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r, g, b;
    if (t <= 66.0f) {
        r = 1.0f;
        g = std::clamp(0.39008157f * std::log(std::max(t, 1e-3f)) - 0.63184144f, 0.0f, 1.0f);
    } else {
        r = std::clamp(1.29293618f * std::pow(t - 60.0f, -0.1332047592f), 0.0f, 1.0f);
        g = std::clamp(1.12989086f * std::pow(t - 60.0f, -0.0755148492f), 0.0f, 1.0f);
    }
    if (t >= 66.0f)      b = 1.0f;
    else if (t <= 19.0f) b = 0.0f;
    else                 b = std::clamp(0.54320679f * std::log(t - 10.0f) - 1.19625408f, 0.0f, 1.0f);
    return glm::vec3(r, g, b);
}

// The colour-temperature drag bar: the strip IS the 1500-15000 K blackbody spectrum, drag
// anywhere on it to set `k`. One InvisibleButton so it sits on the row PropertyLabel() opened
// (a plain slider under a Dummy drops to the window's left edge). Reserves room for a trailing
// button — caller draws that with SameLine. `mixed` shows an em-dash instead of a marker for a
// multi-selection whose temperatures differ. Shared by the single- and multi-light Inspectors.
struct KelvinBarResult { bool changed = false; bool activated = false; bool deactivated = false; };
inline KelvinBarResult KelvinBar(const char* id, float& k, bool mixed = false) {
    KelvinBarResult r;
    constexpr float kKMin = 1500.0f, kKMax = 15000.0f;
    k = std::clamp(k, kKMin, kKMax);
    const float innerSp = ImGui::GetStyle().ItemInnerSpacing.x;
    const float btnW = ImGui::CalcTextSize("RGB").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float barW = std::max(40.0f, ImGui::GetContentRegionAvail().x - btnW - innerSp);
    const float barH = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(barW, barH));
    r.activated = ImGui::IsItemActivated();
    if (ImGui::IsItemActive()) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / barW, 0.0f, 1.0f);
        k = kKMin + (kKMax - kKMin) * t;
        r.changed = true;
    }
    r.deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Drag to set colour temperature (1500-15000 K).");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int kSeg = 48;
    for (int i = 0; i < kSeg; ++i) {
        float t0 = (float)i / kSeg, t1 = (float)(i + 1) / kSeg;
        glm::vec3 c0 = KelvinToRGB(kKMin + (kKMax - kKMin) * t0);
        glm::vec3 c1 = KelvinToRGB(kKMin + (kKMax - kKMin) * t1);
        ImU32 lc = ImGui::ColorConvertFloat4ToU32(ImVec4(c0.r, c0.g, c0.b, 1.0f));
        ImU32 rc = ImGui::ColorConvertFloat4ToU32(ImVec4(c1.r, c1.g, c1.b, 1.0f));
        dl->AddRectFilledMultiColor(ImVec2(p.x + barW * t0, p.y),
                                    ImVec2(p.x + barW * t1, p.y + barH), lc, rc, rc, lc);
    }
    dl->AddRect(p, ImVec2(p.x + barW, p.y + barH), IM_COL32(0, 0, 0, 130));

    char label[24];
    if (mixed) snprintf(label, sizeof(label), "\xE2\x80\x94 mixed \xE2\x80\x94");
    else {
        float hx = p.x + barW * ((k - kKMin) / (kKMax - kKMin));
        dl->AddRectFilled(ImVec2(hx - 2.0f, p.y - 1.0f), ImVec2(hx + 2.0f, p.y + barH + 1.0f), IM_COL32(20, 20, 20, 255));
        dl->AddRectFilled(ImVec2(hx - 1.0f, p.y), ImVec2(hx + 1.0f, p.y + barH), IM_COL32(255, 255, 255, 255));
        snprintf(label, sizeof(label), "%.0f K", k);
    }
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(p.x + (barW - ts.x) * 0.5f, p.y + (barH - ts.y) * 0.5f);
    dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 190), label);
    dl->AddText(tp, IM_COL32(255, 255, 255, 255), label);
    return r;
}

// Editable name field bound to a std::string, without depending on imgui_stdlib.h — copies
// into a fixed local buffer, writes back only on edit. Returns true the moment editing starts
// (for undo-snapshot timing), via activatedOut, same convention as DrawVec3Row below.
bool DrawNameField(const char* label, std::string& name, const char* placeholder, bool& activatedOut) {
    activatedOut = false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", name.c_str());
    bool changed = ImGui::InputTextWithHint(label, placeholder, buf, sizeof(buf));
    if (ImGui::IsItemActivated()) activatedOut = true;
    // Strip control chars / cap length as the user types; leave end-trimming for when the field
    // is committed, so typing "Room " -> "Room 2" isn't fought mid-word.
    if (changed) name = SanitizeEntityName(buf, /*trimEnds=*/false);
    if (ImGui::IsItemDeactivatedAfterEdit()) name = SanitizeEntityName(name);
    return changed;
}

bool DangerIconButton(const char* icon, const char* tooltip, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.20f, 0.20f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.82f, 0.24f, 0.24f, 1.00f));
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tooltip);
    return clicked;
}

// The Unity/Blender/Unreal property-grid convention: a fixed-width label column so a field's
// name always sits at the same X position regardless of what that row's own value widget is —
// which is what actually made a component-heavy Inspector read as disorganized before this,
// since ImGui's default "label drawn after the widget" placed every row's text wherever that
// row's widget happened to end. Follow with a widget using "##..." as its own (invisible) label
// so it doesn't draw a second, differently-positioned label of its own.
// Draws `label`, then repositions the cursor `columnWidth` past wherever this line actually
// started — shared by PropertyLabel (single-widget rows) and DrawVec3Row (which needs a
// narrower column of its own, followed by three widgets instead of one). SameLine(x)'s x is
// measured from the window's left edge, NOT from the current indent, but component section
// bodies ARE indented (see BeginComponentSection) — capturing the real line-start position
// first (GetCursorPosX() already includes indent) is what keeps a column aligned at any
// indent depth instead of drifting left as sections nest.
void AlignToColumn(const char* label, float columnWidth, const char* tooltip = nullptr) {
    float lineStartX = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tooltip);
    ImGui::SameLine(lineStartX + columnWidth);
}

// `tooltip`, when given, shows on hovering the label text itself — this is how nearly every
// field in the Inspector explains what it does and how to use it, without needing a value
// permanently on screen for it.
void PropertyLabel(const char* label, const char* tooltip = nullptr) {
    // Sized to fit "Emissive Strength", the longest label actually used — every row sharing
    // this one constant is what makes their value widgets land in the same column regardless
    // of how long that particular row's own label is. Computed once (the UI font is fixed).
    static const float labelColumnWidth = ImGui::CalcTextSize("Emissive Strength").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    AlignToColumn(label, labelColumnWidth, tooltip);
    ImGui::SetNextItemWidth(-FLT_MIN); // fill exactly to the window's right edge
}

// Unity/Hazel-style vector row: a colored X/Y/Z button (click to zero that axis) glued to
// each drag field, instead of ImGui's plain unlabeled DragFloat3. `activatedOut` is set when
// any axis field starts being dragged this frame, for undo-snapshot timing at the call site.
bool DrawVec3Row(const char* label, glm::vec3& v, float speed, float minV, float maxV, bool& activatedOut,
    bool& committedOut, const char* tooltip = nullptr) {
    activatedOut = false;
    committedOut = false;
    bool changed = false;

    ImGui::PushID(label);
    // A dedicated, shorter column than PropertyLabel's (which is sized for spelled-out names
    // like "Emissive Strength") — "Position"/"Rotation"/"Scale" are short, and the XYZ triplet
    // that follows needs the width back more than it needs to share that wider column. Putting
    // the label on the SAME line as its row (instead of on its own line above it, as before) is
    // what actually removes the wasted vertical gap between each Position/Rotation/Scale block.
    static const float vec3LabelColumnWidth = ImGui::CalcTextSize("Rotation").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    AlignToColumn(label, vec3LabelColumnWidth, tooltip);

    float lineHeight = ImGui::GetFrameHeight();
    float buttonW = lineHeight + 4.0f;
    float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
    float fullWidth = ImGui::GetContentRegionAvail().x;
    float dragW = (fullWidth - 3.0f * buttonW - 3.0f * innerSpacing) / 3.0f;

    struct Axis { const char* name; float* value; ImVec4 color, hovered; };
    Axis axes[3] = {
        {"X", &v.x, ImVec4(0.66f, 0.20f, 0.20f, 1.0f), ImVec4(0.80f, 0.27f, 0.27f, 1.0f)},
        {"Y", &v.y, ImVec4(0.22f, 0.52f, 0.22f, 1.0f), ImVec4(0.30f, 0.68f, 0.30f, 1.0f)},
        {"Z", &v.z, ImVec4(0.18f, 0.38f, 0.72f, 1.0f), ImVec4(0.24f, 0.48f, 0.88f, 1.0f)},
    };

    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);

        ImGui::PushStyleColor(ImGuiCol_Button, axes[i].color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, axes[i].hovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, axes[i].color);
        if (ImGui::Button(axes[i].name, ImVec2(buttonW, lineHeight))) {
            *axes[i].value = 0.0f;
            changed = true;
            activatedOut = true;   // an instant, one-shot edit — stage + commit it as one step
            committedOut = true;
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Click to zero the %s axis", axes[i].name);
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.0f, innerSpacing);
        ImGui::SetNextItemWidth(dragW);
        const float before = *axes[i].value;
        // Big magnitudes get %g so they don't overflow the field as a ~30-digit decimal (#44
        // P27). Small values are left to "%.3f" — anything under 0.001 just reads as "0.000",
        // which is fine and far less alarming than "6.5e-09" of floating-point dust; the exact
        // value is still in the hover tooltip.
        const float mag = std::fabs(before);
        const char* fmt = (mag >= 1.0e6f) ? "%.4g" : "%.3f";
        bool itemChanged = ImGui::DragFloat("##v", axes[i].value, speed, minV, maxV, fmt);
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            EditorUI::SetTooltip("%.9g\nClick and drag to change; double-click to type a value", *axes[i].value);
        if (ImGui::IsItemActivated()) activatedOut = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) committedOut = true;
        if (itemChanged) {
            float& val = *axes[i].value;
            if (!std::isfinite(val)) {
                // nan / inf / -inf would poison the transform matrix and, on save, write
                // tokens that are not valid JSON — scene.json then fails to reload (#34 D4).
                Log::Warn(std::string("Inspector: ignored non-finite value typed into ") + label + "." + axes[i].name);
                val = before;
                itemChanged = false;
            } else if (val == 0.0f) {
                val = 0.0f; // collapse -0.0 so the field never shows "-0.000" (#24 P13)
            }
        }
        changed |= itemChanged;

        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0.0f, innerSpacing);
    }

    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Multi-object ("mixed value") editing helpers. Unity's rule: show the shared value, or an em
// dash when the selected objects disagree on that field; any edit writes an ABSOLUTE value to
// every selected object. Each returns, via out-params, whether an edit began this frame
// (activated — for StageUndo), finished (committed — for CommitStagedUndo), and which
// component(s) the caller should now write to the whole selection.

struct MultiEditResult {
    bool activated = false;   // an interaction started this frame
    bool committed = false;   // an interaction finished this frame (mouse released / Enter)
    bool changed = false;     // the value moved this frame (may fire many times mid-drag)
};

// One X/Y/Z row. `mixedAxis[k]` true => that axis differs across the selection and shows "—"
// until the user grabs it. On any change, `value` holds the new absolute vector and
// `axisTouched[k]` marks which axes the caller should push to every selected object (so an
// untouched mixed axis is left alone rather than flattened to whatever axis 0 happened to be).
MultiEditResult MultiEditVec3Row(const char* label, glm::vec3& value, const bool mixedAxis[3],
                                 bool axisTouched[3], float speed, float minV, float maxV,
                                 const char* tooltip = nullptr) {
    MultiEditResult r;
    axisTouched[0] = axisTouched[1] = axisTouched[2] = false;

    ImGui::PushID(label);
    static const float col = ImGui::CalcTextSize("Rotation").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    AlignToColumn(label, col, tooltip);

    float lineHeight = ImGui::GetFrameHeight();
    float buttonW = lineHeight + 4.0f;
    float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
    float fullWidth = ImGui::GetContentRegionAvail().x;
    float dragW = (fullWidth - 3.0f * buttonW - 3.0f * innerSpacing) / 3.0f;

    struct Axis { const char* name; float* v; ImVec4 c, h; };
    Axis axes[3] = {
        {"X", &value.x, ImVec4(0.66f, 0.20f, 0.20f, 1.0f), ImVec4(0.80f, 0.27f, 0.27f, 1.0f)},
        {"Y", &value.y, ImVec4(0.22f, 0.52f, 0.22f, 1.0f), ImVec4(0.30f, 0.68f, 0.30f, 1.0f)},
        {"Z", &value.z, ImVec4(0.18f, 0.38f, 0.72f, 1.0f), ImVec4(0.24f, 0.48f, 0.88f, 1.0f)},
    };

    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, axes[i].c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, axes[i].h);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, axes[i].c);
        if (ImGui::Button(axes[i].name, ImVec2(buttonW, lineHeight))) {
            *axes[i].v = 0.0f;
            axisTouched[i] = true;
            r.changed = r.activated = r.committed = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0.0f, innerSpacing);

        ImGui::SetNextItemWidth(dragW);
        ImGuiID fieldId = ImGui::GetID("##v");
        bool editing = ImGui::GetActiveID() == fieldId;
        const char* fmt = (mixedAxis[i] && !editing) ? "\xE2\x80\x94" : "%.3f"; // em dash while untouched
        float before = *axes[i].v;
        bool fieldChanged = ImGui::DragFloat("##v", axes[i].v, speed, minV, maxV, fmt);
        if (ImGui::IsItemActivated()) r.activated = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) r.committed = true;
        if (fieldChanged) {
            if (!std::isfinite(*axes[i].v)) { *axes[i].v = before; }
            else { axisTouched[i] = true; r.changed = true; }
        }
        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0.0f, innerSpacing);
    }
    ImGui::PopID();
    return r;
}

// A single scalar row with the same mixed-value behaviour.
MultiEditResult MultiEditFloatRow(const char* label, float& value, bool mixed, float speed,
                                  float minV, float maxV, const char* tooltip = nullptr) {
    MultiEditResult r;
    ImGui::PushID(label);
    PropertyLabel(label, tooltip);
    ImGuiID fieldId = ImGui::GetID("##mf");
    bool editing = ImGui::GetActiveID() == fieldId;
    const char* fmt = (mixed && !editing) ? "\xE2\x80\x94" : "%.3f";
    float before = value;
    bool fieldChanged = ImGui::DragFloat("##mf", &value, speed, minV, maxV, fmt);
    if (ImGui::IsItemActivated()) r.activated = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) r.committed = true;
    if (fieldChanged) {
        if (!std::isfinite(value)) value = before;
        else r.changed = true;
    }
    ImGui::PopID();
    return r;
}

// Checkbox that renders a filled-square "mixed" state when the selection disagrees. Returns
// true when the user clicks it; `out` then holds the value to apply to every object (a click
// on a mixed box resolves the whole selection to checked, like Unity).
bool MultiEditCheckbox(const char* label, bool anyOn, bool mixed, bool& out) {
    bool value = anyOn;
    if (mixed) {
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    }
    bool clicked = ImGui::Checkbox(label, &value);
    if (mixed) ImGui::PopItemFlag();
    if (clicked) out = mixed ? true : value;
    return clicked;
}

// The same eye language for a labelled property row (multi-select Inspector). No hover-reveal —
// a property row is always "there" — but it keeps MultiEditCheckbox's tri-state semantics: a
// click on a mixed selection resolves everything to active. `out` receives the value to apply.
bool ActiveToggleRow(const char* label, bool anyActive, bool mixed, bool& out, const char* tip) {
    PropertyLabel(label, tip);
    const float sz = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##activerow", ImVec2(sz, sz));
    bool hover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hover) EditorUI::SetTooltip("%s", tip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + sz * 0.5f, p0.y + sz * 0.5f);
    const char* g = anyActive ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
    const ImVec2 ts = ImGui::CalcTextSize(g);
    float a = mixed ? 0.45f : (hover ? 0.9f : 0.7f);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text, a), g);
    if (mixed) // a short dash under the glyph = "the selection disagrees", matching the "—" fields
        dl->AddLine(ImVec2(c.x - ts.x * 0.30f, c.y + ts.y * 0.5f + 1.0f),
                    ImVec2(c.x + ts.x * 0.30f, c.y + ts.y * 0.5f + 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Text, 0.7f), 1.5f);
    if (clicked) out = mixed ? true : !anyActive;
    return clicked;
}

} // namespace


namespace {
std::string LowerExt(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}
}

// Unity-AssetImporter-style Import Settings panel, shown in the Inspector when an asset (not a
// scene entity) is selected in the Asset Browser. Only textures and models have any settings to
// show — sounds/prefabs/scenes just get a name/path readout, matching Unity (not every asset
// type has an importer with configurable options).
void EditorLayer::DrawAssetImportInspector(World& world, AssetLibrary& assets, const std::string& key) {
    std::string ext = LowerExt(key);
    bool isTexture = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp");
    bool isModel = (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb");

    ImGui::SeparatorText(assets.DisplayName(key).c_str());
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::Spacing();

    if (!isTexture && !isModel) {
        ImGui::TextWrapped("This asset type has no import settings.");
        return;
    }

    // Fresh copy from AssetLibrary whenever the selection changes to a different asset - so an
    // edit made (but not Applied) on one asset never bleeds onto the next one selected.
    if (m_ImportInspectorKey != key) {
        m_ImportInspectorKey = key;
        m_ImportSettingsDirty = false;
        m_ChannelPreviewChannel = -1; // back to "Combined" for a newly-selected asset
        if (isTexture) {
            m_PendingTextureSettings = assets.GetTextureSettings(key);
        } else {
            m_PendingModelSettings = assets.GetModelSettings(key);
            // Fresh orbit framing for the model preview below - a pleasant default angle, and
            // a distance that fits this particular model's own bounding box (a tiny prop and a
            // whole building shouldn't start at the same zoom level).
            m_ModelPreviewYaw = 0.6f;
            m_ModelPreviewPitch = 0.35f;
            if (auto model = assets.LoadModel(key)) { // cache hit if already loaded to appear in the browser
                m_ModelPreviewDistance = ModelPreviewRenderer::ComputeFramingDistance(*model);
            }
        }
    }

    if (isTexture) {
        auto tex = assets.LoadTexture(key); // cache hit - already imported to appear in the browser
        if (tex && tex->IsValid()) {
            const char* kFormats[] = {"", "R8", "", "RGB8", "RGBA8"}; // indexed by channel count (1/3/4); 0/2 unused
            int channels = tex->SourceChannels();
            ImGui::Text("%d x %d, %s, %d channel(s)", tex->Width(), tex->Height(),
                (channels >= 1 && channels <= 4 && kFormats[channels][0]) ? kFormats[channels] : "8-bit", channels);

            float previewSize = 160.0f;
            float aspect = tex->Height() > 0 ? (float)tex->Width() / (float)tex->Height() : 1.0f;
            ImVec2 previewDims = aspect >= 1.0f ? ImVec2(previewSize, previewSize / aspect) : ImVec2(previewSize * aspect, previewSize);

            // Channel isolation toggles - "Combined" shows the texture as normal; R/G/B/A each
            // broadcast that one channel to grayscale, e.g. to check what a MaskMap's alpha
            // (often Smoothness) actually contains without exporting it to a separate file first.
            auto channelButton = [&](const char* label, int channel) {
                bool active = m_ChannelPreviewChannel == channel;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(label)) m_ChannelPreviewChannel = channel;
                if (active) ImGui::PopStyleColor();
            };
            channelButton("Combined", -1);
            ImGui::SameLine();
            channelButton("R", 0);
            ImGui::SameLine();
            channelButton("G", 1);
            ImGui::SameLine();
            channelButton("B", 2);
            ImGui::SameLine();
            channelButton("A", 3);

            // Render (or reuse) the offscreen preview - only when the inspected asset or the
            // selected channel actually changed since the last frame, not unconditionally.
            int renderW = std::max(64, (int)previewDims.x * 2); // 2x the display size for a crisp
            int renderH = std::max(64, (int)previewDims.y * 2); // result when the panel is resized larger
            if (m_ChannelPreviewRenderedKey != key || m_ChannelPreviewRenderedChannel != m_ChannelPreviewChannel) {
                m_ChannelPreview.Render(*tex, m_ChannelPreviewChannel, renderW, renderH);
                m_ChannelPreviewRenderedKey = key;
                m_ChannelPreviewRenderedChannel = m_ChannelPreviewChannel;
            }
            ImGui::Image((ImTextureID)(intptr_t)m_ChannelPreview.Handle(), previewDims);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to load - see Console.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Texture Import Settings");
        AssetImporterInspector::DrawTextureSettings(m_PendingTextureSettings, m_ImportSettingsDirty);
        AssetImporterInspector::DrawApplyRevertFooter(m_ImportSettingsDirty,
            [&]() {
                PushUndo(world, "Reimport Texture");
                assets.SetTextureSettings(key, m_PendingTextureSettings);
                if (assets.ReimportTexture(key)) Log::Info("Reimported texture '" + key + "'.");
                else Log::Error("Reimport failed for '" + key + "' - see Console.");
                m_ImportSettingsDirty = false;
            },
            [&]() {
                m_PendingTextureSettings = assets.GetTextureSettings(key);
                m_ImportSettingsDirty = false;
            });
    } else {
        auto model = assets.LoadModel(key);
        if (model) {
            ImGui::Text("%u triangles, %u vertices, %d mesh(es)", model->TriangleCount(), model->VertexCount(), model->MeshCount());
            if (model->HasAnimations()) ImGui::Text("%d animation clip(s)", model->AnimationCount());

            float previewSize = 220.0f;
            ImVec2 previewDims(previewSize, previewSize);
            unsigned int handle = m_ModelPreview.Render(*model, m_ModelPreviewYaw, m_ModelPreviewPitch,
                m_ModelPreviewDistance, (int)previewDims.x * 2, (int)previewDims.y * 2);
            ImGui::Image((ImTextureID)(intptr_t)handle, previewDims);

            // Left-drag to orbit, scroll to zoom - the same mouse language as the main viewport's
            // own camera controls. ImGui::Image is a plain draw, not a clickable widget, so it
            // never becomes "active" the way a button would - IsItemActive() here would just
            // always read false. Tracking press/release ourselves (matching how the main
            // viewport's own camera drag is done, via raw IsMouseDown checks rather than
            // IsItemActive) is what actually works, and it also means the drag keeps tracking
            // correctly even once the cursor moves outside this small preview box mid-drag.
            bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_ModelPreviewDragging = true;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_ModelPreviewDragging = false;

            if (hovered) {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) m_ModelPreviewDistance *= (1.0f - wheel * 0.1f);
            }
            if (m_ModelPreviewDragging) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                m_ModelPreviewYaw += delta.x * 0.01f;
                m_ModelPreviewPitch = std::clamp(m_ModelPreviewPitch - delta.y * 0.01f, -1.5f, 1.5f);
            }
            // Keep the model in frame however hard you scroll: clamp the orbit distance to a
            // band around this model's own auto-framing distance, so it can't be zoomed to a
            // black preview with no way back.
            float fitDistance = ModelPreviewRenderer::ComputeFramingDistance(*model);
            m_ModelPreviewDistance = std::clamp(m_ModelPreviewDistance, fitDistance * 0.15f, fitDistance * 8.0f);

            ImGui::TextDisabled("Drag to orbit, scroll to zoom");
            ImGui::SameLine();
            if (ActionButton("Reset view", "Reset the preview camera")) {
                m_ModelPreviewYaw = 0.6f;
                m_ModelPreviewPitch = 0.35f;
                m_ModelPreviewDistance = fitDistance;
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Model Import Settings");
        AssetImporterInspector::DrawModelSettings(m_PendingModelSettings, m_ImportSettingsDirty);
        AssetImporterInspector::DrawApplyRevertFooter(m_ImportSettingsDirty,
            [&]() {
                PushUndo(world, "Reimport Model");
                assets.SetModelSettings(key, m_PendingModelSettings);
                if (assets.ReimportModel(key)) Log::Info("Reimported model '" + key + "'.");
                else Log::Error("Reimport failed for '" + key + "' - see Console.");
                InvalidateModelThumbnail(); // re-render the Asset Browser preview
                m_ImportSettingsDirty = false;
            },
            [&]() {
                m_PendingModelSettings = assets.GetModelSettings(key);
                m_ImportSettingsDirty = false;
            });
    }
}

// The Inspector's body — everything inside the panel window. The module
// (EditorModuleInspector.cpp, #229) owns Begin("Inspector") + End + visibility and calls this
// inside that window scope. The whole body — every component editor, the PBR material editor,
// add-component, per-field undo — stays here (EnTT + Components.h + material shared_ptr never
// cross the DLL boundary).
void EditorLayer::DrawInspectorBody(World& world, AssetLibrary& assets) {
    // Flat button language for the whole panel (#155/#156): no raised body at rest, a faint wash
    // on hover. Every ImGui::Button below inherits it; ActionButton / DangerIconButton push their
    // own on top. Popped before each early return (and the natural end) via InspectorEnd() — the
    // module owns ImGui::End() now, so this no longer closes the window.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    auto InspectorEnd = []() { ImGui::PopStyleColor(3); };

    // Prune handles for objects deleted since the selection was made, so the multi/single
    // Inspector split below (and everything downstream) sees an accurate count.
    m_ExtraSelection.erase(std::remove_if(m_ExtraSelection.begin(), m_ExtraSelection.end(),
        [&](entt::entity e) { return !world.Registry.valid(e) || e == m_Selected; }),
        m_ExtraSelection.end());
    if (m_Selected != entt::null && !world.Registry.valid(m_Selected)) {
        m_Selected = m_ExtraSelection.empty() ? entt::null : m_ExtraSelection.back();
        if (!m_ExtraSelection.empty()) m_ExtraSelection.pop_back();
    }

    if (HasGroupSelection()) {
        std::vector<entt::entity> sel;
        sel.push_back(m_Selected);
        for (entt::entity e : m_ExtraSelection) sel.push_back(e);
        const int count = (int)sel.size();
        auto forEach = [&](const std::function<void(entt::entity)>& fn) { for (entt::entity e : sel) fn(e); };

        ImGui::SeparatorText((std::to_string(count) + " objects selected").c_str());
        // Explanation moved onto the heading's own tooltip (#156) — no persistent "(?)" glyph.
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip(
            "Editing a field here writes it to EVERY selected object.\n"
            "A field showing \xE2\x80\x94 (an em dash) means the selected objects currently\n"
            "hold different values; set it to give them all the same value.\n\n"
            "Build the selection with Shift+Click (a range) or Ctrl+A (all) in the\n"
            "Hierarchy; Ctrl+Click adds or removes a single object.");

        if (ImGui::TreeNodeEx("Selected objects", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (entt::entity e : sel) {
                const auto& nm = world.Registry.get<NameComponent>(e);
                ImGui::BulletText("%s", nm.Name.empty() ? "(unnamed)" : nm.Name.c_str());
            }
            ImGui::TreePop();
        }

        // ---- Common components: those present on the WHOLE selection ----
        bool allMesh = true, allLight = true, allCamera = true, allCollider = true,
             allAudio = true;
        // #184: same "present on every selected object" test, generalized over every
        // reflection-registered component instead of one bool per hand-coded component.
        const auto& registeredComponents = ComponentRegistry::All();
        std::vector<bool> allReflected(registeredComponents.size(), true);
        forEach([&](entt::entity e) {
            allMesh     &= world.Registry.all_of<RenderableComponent>(e);
            allLight    &= world.Registry.all_of<LightComponent>(e);
            allCamera   &= world.Registry.all_of<CameraComponent>(e);
            allCollider &= world.Registry.all_of<ColliderComponent>(e);
            allAudio    &= world.Registry.all_of<AudioSourceComponent>(e);
            for (std::size_t i = 0; i < registeredComponents.size(); ++i)
                if (!registeredComponents[i].Has(world.Registry, e)) allReflected[i] = false;
        });
        {
            std::string common = "Transform";
            if (allMesh)     common += ", Mesh Renderer";
            if (allLight)    common += ", Light";
            if (allCamera)   common += ", Camera";
            if (allCollider) common += ", Box Collider";
            if (allAudio)    common += ", Audio Source";
            for (std::size_t i = 0; i < registeredComponents.size(); ++i)
                if (allReflected[i]) common += std::string(", ") + registeredComponents[i].Meta.Name;
            ImGui::TextDisabled("Common: %s", common.c_str());
        }

        // #155 — the multi-select sections go through the same flat collapsible
        // BeginComponentSection as the single-select Inspector, so the two read identically.
        // Nothing here is per-entity removable, so `mrm` is an ignored sink.
        bool mrm = false;

        // ===== Transform (absolute, mixed-value) =====
        // Edits the raw TransformComponent, exactly what the single-object Inspector shows for
        // each entity — i.e. local space for a parented object, world space otherwise.
        ImGui::Spacing();
        if (BeginComponentSection(ICON_FA_UP_DOWN_LEFT_RIGHT, "Transform", false, mrm)) {

        auto reduceVec3 = [&](const std::function<glm::vec3(const TransformComponent&)>& get,
                              glm::vec3& shared, bool mixed[3]) {
            mixed[0] = mixed[1] = mixed[2] = false;
            bool first = true;
            forEach([&](entt::entity e) {
                glm::vec3 v = get(world.Registry.get<TransformComponent>(e));
                if (first) { shared = v; first = false; return; }
                for (int a = 0; a < 3; ++a)
                    if (std::fabs(v[a] - shared[a]) > 1.0e-4f) mixed[a] = true;
            });
        };

        auto transformRow = [&](const char* label, float speed, float minV, float maxV,
                                const std::function<glm::vec3&(TransformComponent&)>& ref,
                                const char* tip, const char* undoLabel) {
            glm::vec3 shared(0.0f); bool mixed[3];
            reduceVec3([&](const TransformComponent& t) { return ref(const_cast<TransformComponent&>(t)); },
                       shared, mixed);
            glm::vec3 edit = shared; bool touched[3];
            MultiEditResult res = MultiEditVec3Row(label, edit, mixed, touched, speed, minV, maxV, tip);
            if (res.activated) StageUndo(world);
            if (res.changed) {
                forEach([&](entt::entity e) {
                    glm::vec3& target = ref(world.Registry.get<TransformComponent>(e));
                    for (int a = 0; a < 3; ++a) if (touched[a]) target[a] = edit[a];
                });
            }
            if (res.committed) CommitStagedUndo(world, undoLabel);
        };

        // #218 — label the Position row "Local Position" only when every selected object is
        // parented (each one's own TransformComponent.Position is then parent-relative); a
        // mixed selection of parented/unparented objects falls back to the neutral "Position"
        // since neither "World" nor "Local" would be true for the whole group.
        {
            bool allParented = true;
            forEach([&](entt::entity e) {
                const auto* h = world.Registry.try_get<HierarchyComponent>(e);
                allParented &= (h && h->Parent != entt::null);
            });
            const char* posLabel = allParented ? "Local Position" : "Position";
            const char* posTip = allParented
                ? "Sets X / Y / Z (relative to each object's parent) on every selected object."
                : "Sets X / Y / Z on every selected object.";
            transformRow(posLabel, 0.05f, 0.0f, 0.0f,
                [](TransformComponent& t) -> glm::vec3& { return t.Position; },
                posTip, "Set Position");
        }
        transformRow("Rotation", 0.5f, 0.0f, 0.0f,
            [](TransformComponent& t) -> glm::vec3& { return t.RotationEuler; },
            "Sets Euler rotation (degrees) on every selected object.", "Set Rotation");
        transformRow("Scale", 0.01f, 0.0f, 0.0f,
            [](TransformComponent& t) -> glm::vec3& { return t.Scale; },
            "Sets scale on every selected object.", "Set Scale");

        EndComponentSection();
        }

        // ===== Object: Active / Static / Tag, tri-state =====
        ImGui::Spacing();
        if (BeginComponentSection(ICON_FA_TAG, "Object", false, mrm)) {

        int nActive = 0, nStatic = 0;
        forEach([&](entt::entity e) {
            if (!world.Registry.all_of<InactiveTag>(e)) nActive++;
            if (world.Registry.all_of<StaticTag>(e)) nStatic++;
        });
        bool setVal = false;
        if (ActiveToggleRow("Active", nActive > 0, nActive != 0 && nActive != count, setVal,
                            "Active - inactive objects are not drawn and don't collide")) {
            PushUndo(world, "Toggle Active");
            forEach([&](entt::entity e) {
                if (setVal) world.Registry.remove<InactiveTag>(e);
                else        world.Registry.emplace_or_replace<InactiveTag>(e);
            });
        }
        if (MultiEditCheckbox("Static", nStatic > 0, nStatic != 0 && nStatic != count, setVal)) {
            PushUndo(world, "Set Static");
            forEach([&](entt::entity e) {
                if (setVal) world.Registry.emplace_or_replace<StaticTag>(e);
                else        world.Registry.remove<StaticTag>(e);
            });
        }

        {
            std::set<std::string> tagChoices{"Untagged"};
            for (auto e : world.Registry.view<const TagComponent>())
                tagChoices.insert(world.Registry.get<TagComponent>(e).Tag);
            std::string sharedTag; bool tagMixed = false, first = true;
            forEach([&](entt::entity e) {
                const auto* tc = world.Registry.try_get<TagComponent>(e);
                std::string t = tc ? tc->Tag : std::string("Untagged");
                if (first) { sharedTag = t; first = false; }
                else if (t != sharedTag) tagMixed = true;
            });
            PropertyLabel("Tag", "Sets the Tag on every selected object.");
            if (ImGui::BeginCombo("##mtag", tagMixed ? "\xE2\x80\x94" : sharedTag.c_str())) {
                for (const std::string& t : tagChoices) {
                    if (ImGui::Selectable(t.c_str(), !tagMixed && t == sharedTag)) {
                        PushUndo(world, "Set Tag");
                        forEach([&](entt::entity e) {
                            if (t == "Untagged") world.Registry.remove<TagComponent>(e);
                            else {
                                TagComponent tc; tc.Tag = t;
                                world.Registry.emplace_or_replace<TagComponent>(e, tc);
                            }
                        });
                    }
                }
                ImGui::EndCombo();
            }
        }

        EndComponentSection();
        }

        // ===== Light (only when every selected object has one) =====
        if (allLight) {
            ImGui::Spacing();
            if (BeginComponentSection(ICON_FA_LIGHTBULB, "Light", false, mrm)) {
            auto L = [&](entt::entity e) -> LightComponent& { return world.Registry.get<LightComponent>(e); };

            int kind = -1; bool kindMixed = false;
            forEach([&](entt::entity e) {
                int k = (int)L(e).Kind; // Point=0, Spot=1, Directional=2
                if (kind < 0) kind = k; else if (k != kind) kindMixed = true;
            });
            const char* kinds[] = {"Point", "Spot", "Directional"};
            PropertyLabel("Kind", "Sets the light type on every selected light.");
            if (ImGui::BeginCombo("##mlkind", kindMixed ? "\xE2\x80\x94" : kinds[kind < 0 ? 0 : kind])) {
                for (int k = 0; k < 3; ++k)
                    if (ImGui::Selectable(kinds[k], !kindMixed && k == kind)) {
                        PushUndo(world, "Set Light Kind");
                        forEach([&](entt::entity e) { L(e).Kind = (LightComponent::Type)k; });
                    }
                ImGui::EndCombo();
            }

            // Colour: an RGB swatch, or the shared Kelvin spectrum bar when every selected light
            // is colour-temperature driven. "K" / "RGB" flips the whole selection between the two.
            int nKelvin = 0; float kShared = 0.0f; bool kMixed = false, kf = true;
            forEach([&](entt::entity e) {
                float kv = L(e).ColorTempK;
                if (kv > 0.0f) nKelvin++;
                if (kf) { kShared = kv; kf = false; } else if (std::fabs(kv - kShared) > 0.5f) kMixed = true;
            });
            const float mlInnerSp = ImGui::GetStyle().ItemInnerSpacing.x;
            PropertyLabel("Color", "Sets the colour on every selected light. 'K' drives it from a temperature.");
            if (nKelvin == count && count > 0) {
                float k = (!kMixed && kShared > 0.0f) ? kShared : 6500.0f;
                KelvinBarResult kr = KelvinBar("##mlKelvin", k, kMixed);
                if (kr.activated) StageUndo(world);
                if (kr.changed) forEach([&](entt::entity e) { L(e).ColorTempK = k; L(e).Color = KelvinToRGB(k); });
                if (kr.deactivated) CommitStagedUndo(world, "Set Light Colour Temperature");
                ImGui::SameLine(0.0f, mlInnerSp);
                if (ActionButton("RGB##mlKelvinOff", "Switch this selection to a direct RGB colour")) {
                    PushUndo(world, "Set Light Colour");
                    forEach([&](entt::entity e) { L(e).ColorTempK = 0.0f; });
                }
            } else {
                glm::vec3 col(1.0f); bool colMixed = false, cf = true;
                forEach([&](entt::entity e) {
                    glm::vec3 c = L(e).Color;
                    if (cf) { col = c; cf = false; return; }
                    for (int a = 0; a < 3; ++a) if (std::fabs(c[a] - col[a]) > 1.0e-4f) colMixed = true;
                });
                glm::vec3 colEdit = col;
                bool colChanged = ImGui::ColorEdit3("##mlcol", &colEdit.x, ImGuiColorEditFlags_NoInputs);
                if (ImGui::IsItemActivated()) StageUndo(world);
                if (colChanged) forEach([&](entt::entity e) { L(e).Color = colEdit; });
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Set Light Color");
                ImGui::SameLine(0.0f, mlInnerSp);
                if (ActionButton("K##mlKelvinOn", "Drive this selection's colour from a temperature (Kelvin)")) {
                    PushUndo(world, "Set Light Colour Temperature");
                    forEach([&](entt::entity e) { L(e).ColorTempK = 6500.0f; L(e).Color = KelvinToRGB(6500.0f); });
                }
                if (colMixed) { ImGui::SameLine(); ImGui::TextDisabled("(mixed)"); }
            }

            auto lightFloatRow = [&](const char* label, float speed, float minV, float maxV,
                                     const std::function<float&(LightComponent&)>& ref,
                                     const char* tip, const char* undoLabel) {
                float shared = 0.0f; bool mixed = false, f = true;
                forEach([&](entt::entity e) {
                    float v = ref(L(e));
                    if (f) { shared = v; f = false; } else if (std::fabs(v - shared) > 1.0e-4f) mixed = true;
                });
                float edit = shared;
                MultiEditResult r = MultiEditFloatRow(label, edit, mixed, speed, minV, maxV, tip);
                if (r.activated) StageUndo(world);
                if (r.changed) forEach([&](entt::entity e) { ref(L(e)) = edit; });
                if (r.committed) CommitStagedUndo(world, undoLabel);
            };
            lightFloatRow("Intensity", 0.05f, 0.0f, 100.0f,
                [](LightComponent& l) -> float& { return l.Intensity; },
                "Brightness for every selected light.", "Set Light Intensity");
            lightFloatRow("Range", 0.1f, 0.0f, 500.0f,
                [](LightComponent& l) -> float& { return l.Range; },
                "Falloff distance for every selected light.", "Set Light Range");

            bool allSpot = true, anyDir = false, allDir = true;
            forEach([&](entt::entity e) {
                bool d = L(e).Kind == LightComponent::Type::Directional;
                allSpot &= L(e).Kind == LightComponent::Type::Spot;
                anyDir |= d;
                allDir &= d;
            });
            if (allSpot)
                lightFloatRow("Spot Angle", 0.5f, 1.0f, 89.0f,
                    [](LightComponent& l) -> float& { return l.SpotAngleDegrees; },
                    "Cone half-angle for every selected spot light.", "Set Spot Angle");
            if (allDir)
                lightFloatRow("Angular Size", 0.05f, 0.1f, 20.0f,
                    [](LightComponent& l) -> float& { return l.AngularSizeDegrees; },
                    "Sun disc diameter (deg) for every selected directional light.", "Set Angular Size");

            // ---- Shadows (mirrors the single-edit Shadows sub-block) ------------------------
            ImGui::Spacing();
            if (BeginComponentSection(ICON_FA_MOON, "Shadows", false, mrm)) {

            int nShadow = 0;
            forEach([&](entt::entity e) { if (L(e).Shadow.Enabled) nShadow++; });
            bool shVal = false;
            if (MultiEditCheckbox("Cast Shadows", nShadow > 0, nShadow != 0 && nShadow != count, shVal)) {
                PushUndo(world, "Toggle Cast Shadows");
                forEach([&](entt::entity e) { L(e).Shadow.Enabled = shVal; });
            }

            lightFloatRow("Bias", 0.02f, 0.0f, 4.0f,
                [](LightComponent& l) -> float& { return l.Shadow.Bias; },
                "Depth-bias multiplier for every selected light.", "Set Shadow Bias");
            lightFloatRow("Normal Bias", 0.02f, 0.0f, 4.0f,
                [](LightComponent& l) -> float& { return l.Shadow.NormalBias; },
                "Normal-offset multiplier for every selected light.", "Set Shadow Normal Bias");
            lightFloatRow("Softness", 0.02f, 0.0f, 4.0f,
                [](LightComponent& l) -> float& { return l.Shadow.Softness; },
                "PCF-radius multiplier for every selected light.", "Set Shadow Softness");
            if (!anyDir)
                lightFloatRow("Near Plane", 0.01f, 0.001f, 10.0f,
                    [](LightComponent& l) -> float& { return l.Shadow.NearPlane; },
                    "Perspective near distance for every selected spot/point light's depth pass.",
                    "Set Shadow Near Plane");

            EndComponentSection(); // Shadows
            }

            EndComponentSection(); // Light
            }
        }

        // ===== Camera (only when every selected object has one) =====
        if (allCamera) {
            ImGui::Spacing();
            if (BeginComponentSection(ICON_FA_VIDEO, "Camera", false, mrm)) {
            auto C = [&](entt::entity e) -> CameraComponent& { return world.Registry.get<CameraComponent>(e); };
            auto camFloatRow = [&](const char* label, float speed, float minV, float maxV,
                                   const std::function<float&(CameraComponent&)>& ref,
                                   const char* tip, const char* undoLabel) {
                float shared = 0.0f; bool mixed = false, f = true;
                forEach([&](entt::entity e) {
                    float v = ref(C(e));
                    if (f) { shared = v; f = false; } else if (std::fabs(v - shared) > 1.0e-4f) mixed = true;
                });
                float edit = shared;
                MultiEditResult r = MultiEditFloatRow(label, edit, mixed, speed, minV, maxV, tip);
                if (r.activated) StageUndo(world);
                if (r.changed) forEach([&](entt::entity e) { ref(C(e)) = edit; });
                if (r.committed) CommitStagedUndo(world, undoLabel);
            };
            camFloatRow("Field of View", 0.25f, 1.0f, 179.0f,
                [](CameraComponent& c) -> float& { return c.FovDegrees; },
                "Vertical FOV for every selected camera.", "Set Camera FOV");
            camFloatRow("Near", 0.01f, 0.001f, 100.0f,
                [](CameraComponent& c) -> float& { return c.NearPlane; },
                "Near clip plane for every selected camera.", "Set Camera Near");
            camFloatRow("Far", 1.0f, 0.1f, 100000.0f,
                [](CameraComponent& c) -> float& { return c.FarPlane; },
                "Far clip plane for every selected camera.", "Set Camera Far");

            EndComponentSection(); // Camera
            }
        }

        // ===== Reflection-registered components (#184) — the multi-select counterpart of the
        // single-select generic loop: one section per component present on the WHOLE selection,
        // one row per reflected field, using the same MultiEdit* mixed-value widgets as every
        // hand-coded section above. No per-component code here, same as the single-select pass.
        for (std::size_t ci = 0; ci < registeredComponents.size(); ++ci) {
            if (!allReflected[ci]) continue;
            const RegisteredComponent& rc = registeredComponents[ci];
            ImGui::Spacing();
            if (BeginComponentSection(rc.Meta.Icon, rc.Meta.Name, false, mrm, /*defaultOpen=*/true, rc.Meta.Tooltip)) {
                auto fieldPtr = [&](entt::entity e, const ReflectField& f) -> void* {
                    return f.Address(rc.Get(world.Registry, e));
                };
                for (const ReflectField& f : rc.Meta.Fields) {
                    ImGui::PushID(f.Name);
                    switch (f.Type) {
                        case ReflectFieldType::Bool: {
                            bool anyOn = false, mixed = false, first = true, firstVal = false;
                            forEach([&](entt::entity e) {
                                bool v = *reinterpret_cast<bool*>(fieldPtr(e, f));
                                if (first) { firstVal = v; first = false; } else if (v != firstVal) mixed = true;
                                anyOn |= v;
                            });
                            bool out = firstVal;
                            if (MultiEditCheckbox(f.Name, anyOn, mixed, out)) {
                                StageUndo(world);
                                forEach([&](entt::entity e) { *reinterpret_cast<bool*>(fieldPtr(e, f)) = out; });
                                CommitStagedUndo(world, std::string("Edit ") + rc.Meta.Name);
                            }
                            break;
                        }
                        case ReflectFieldType::Int: {
                            int shared = 0; bool mixed = false, first = true;
                            forEach([&](entt::entity e) {
                                int v = *reinterpret_cast<int*>(fieldPtr(e, f));
                                if (first) { shared = v; first = false; } else if (v != shared) mixed = true;
                            });
                            float edit = (float)shared;
                            MultiEditResult r = MultiEditFloatRow(f.Name, edit, mixed, f.DragSpeed, f.Min, f.Max, f.Tooltip);
                            if (r.activated) StageUndo(world);
                            if (r.changed) { int v = (int)edit; forEach([&](entt::entity e) { *reinterpret_cast<int*>(fieldPtr(e, f)) = v; }); }
                            if (r.committed) CommitStagedUndo(world, std::string("Edit ") + rc.Meta.Name);
                            break;
                        }
                        case ReflectFieldType::Float: {
                            float shared = 0.0f; bool mixed = false, first = true;
                            forEach([&](entt::entity e) {
                                float v = *reinterpret_cast<float*>(fieldPtr(e, f));
                                if (first) { shared = v; first = false; } else if (std::fabs(v - shared) > 1.0e-4f) mixed = true;
                            });
                            float edit = shared;
                            MultiEditResult r = MultiEditFloatRow(f.Name, edit, mixed, f.DragSpeed, f.Min, f.Max, f.Tooltip);
                            if (r.activated) StageUndo(world);
                            if (r.changed) forEach([&](entt::entity e) { *reinterpret_cast<float*>(fieldPtr(e, f)) = edit; });
                            if (r.committed) CommitStagedUndo(world, std::string("Edit ") + rc.Meta.Name);
                            break;
                        }
                        case ReflectFieldType::Vec3: {
                            glm::vec3 shared(0.0f); bool mixedAxis[3] = {false, false, false}; bool first = true;
                            forEach([&](entt::entity e) {
                                glm::vec3 v = *reinterpret_cast<glm::vec3*>(fieldPtr(e, f));
                                if (first) { shared = v; first = false; }
                                else for (int a = 0; a < 3; ++a) if (std::fabs(v[a] - shared[a]) > 1.0e-4f) mixedAxis[a] = true;
                            });
                            glm::vec3 edit = shared;
                            bool touched[3];
                            MultiEditResult r = MultiEditVec3Row(f.Name, edit, mixedAxis, touched, f.DragSpeed, f.Min, f.Max, f.Tooltip);
                            if (r.activated) StageUndo(world);
                            if (r.changed) forEach([&](entt::entity e) {
                                glm::vec3& v = *reinterpret_cast<glm::vec3*>(fieldPtr(e, f));
                                for (int a = 0; a < 3; ++a) if (touched[a]) v[a] = edit[a];
                            });
                            if (r.committed) CommitStagedUndo(world, std::string("Edit ") + rc.Meta.Name);
                            break;
                        }
                        case ReflectFieldType::String: {
                            std::string shared; bool mixed = false, first = true;
                            forEach([&](entt::entity e) {
                                const std::string& v = *reinterpret_cast<std::string*>(fieldPtr(e, f));
                                if (first) { shared = v; first = false; } else if (v != shared) mixed = true;
                            });
                            PropertyLabel(f.Name, f.Tooltip);
                            char buf[256];
                            snprintf(buf, sizeof(buf), "%s", mixed ? "" : shared.c_str());
                            bool changed = mixed
                                ? ImGui::InputTextWithHint("##v", "(multiple values)", buf, sizeof(buf))
                                : ImGui::InputText("##v", buf, sizeof(buf));
                            if (ImGui::IsItemActivated()) StageUndo(world);
                            if (changed) forEach([&](entt::entity e) { *reinterpret_cast<std::string*>(fieldPtr(e, f)) = buf; });
                            if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, std::string("Edit ") + rc.Meta.Name);
                            break;
                        }
                    }
                    ImGui::PopID();
                }
                EndComponentSection();
            }
        }

        // ===== Material / PBR (only when every selected object has a mesh) =====
        if (allMesh) {
            ImGui::Spacing();
            bool matOpen = BeginComponentSection(ICON_FA_PALETTE, "Material", false, mrm,
                /*defaultOpen=*/true,
                "Shared PBR material and texture maps for every selected mesh.\n"
                "A field showing \xE2\x80\x94 differs across the selection.");
            if (matOpen) {
                DrawMaterialEditor(world, assets, sel);
                EndComponentSection();
            }
        }

        ImGui::Spacing();
        // Compact, right-aligned, icon-only (#156) — names + shortcuts live in the tooltips.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
        {
            const float bw = ImGui::GetFrameHeight() + 8.0f;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 2.0f - ImGui::GetStyle().ItemSpacing.x);
            if (ActionButton(ICON_FA_CLONE, "Duplicate every selected object (Ctrl+D)", false, ImVec2(bw, 0.0f)))
                DuplicateSelection(world, assets);
            ImGui::SameLine();
            if (DangerIconButton(ICON_FA_TRASH, "Delete every selected object (Del)", ImVec2(bw, 0.0f)))
                DeleteSelection(world);
        }
        ImGui::PopStyleVar();
        InspectorEnd();
        return;
    }

    bool selectionValid = m_Selected != entt::null && world.Registry.valid(m_Selected);
    if (!selectionValid) {
        // No scene entity selected — fall back to whatever's selected in the Asset Browser, if
        // anything, and show its Import Settings instead of just an empty placeholder.
        if (!m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) {
            DrawAssetImportInspector(world, assets, m_SelectedAssetKey);
            InspectorEnd();
            return;
        }
        // Nothing selected anywhere — instead of a near-black void with one line of grey text,
        // show a short scene summary and a way to add something (audit #72).
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing selected");
        ImGui::Spacing();
        ImGui::TextUnformatted("Select an object in the Scene Hierarchy, or an\nasset in the Asset Browser, to edit it here.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int objects = 0, meshes = 0, lights = 0, cameras = 0;
        for (auto e : world.Registry.view<const NameComponent>()) { (void)e; ++objects; }
        for (auto e : world.Registry.view<const RenderableComponent>()) { (void)e; ++meshes; }
        for (auto e : world.Registry.view<const LightComponent>()) { (void)e; ++lights; }
        for (auto e : world.Registry.view<const CameraComponent>()) { (void)e; ++cameras; }
        ImGui::TextDisabled("Scene");
        ImGui::BulletText("%d object%s", objects, objects == 1 ? "" : "s");
        ImGui::BulletText("%d mesh%s", meshes, meshes == 1 ? "" : "es");
        ImGui::BulletText("%d light%s, %d camera%s", lights, lights == 1 ? "" : "s",
                          cameras, cameras == 1 ? "" : "s");

        ImGui::Spacing();
        ImGui::TextDisabled("Tip: press " ICON_FA_KEYBOARD " Shift+A in the viewport to add an object.");

        if (!m_LastSelectedName.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Last selected: %s", m_LastSelectedName.c_str());
        }
        InspectorEnd();
        return;
    }

    entt::entity entity = m_Selected;
    auto& registry = world.Registry;
    bool isLevelGeometry = registry.all_of<LevelGeometryTag>(entity);
    auto& transform = registry.get<TransformComponent>(entity);
    auto& name = registry.get<NameComponent>(entity);
    bool activated = false;
    m_LastSelectedName = name.Name; // remembered for the empty-state panel

    // Scopes every CollapsingHeader ID below to this entity — otherwise ImGui remembers a
    // header's open/closed state by its label text alone, so collapsing e.g. "Health" on one
    // object would also show it collapsed on the next, unrelated object that happens to have
    // the same component. Keyed by a value that SURVIVES an undo/redo snapshot reload (which
    // recreates the entity with a fresh handle) so expanded sections don't snap shut on every
    // undo (#20 P9): OrderComponent's stable creation order when there is one (preferred - two
    // entities can share a name, e.g. both named "Cube", but never an OrderComponent value;
    // #217), else the name, else the raw entity as a last resort.
    if (const auto* ord = registry.try_get<OrderComponent>(entity)) {
        ImGui::PushID(0x0DE00000 + ord->Value);
    } else if (!name.Name.empty()) {
        ImGui::PushID(name.Name.c_str());
    } else {
        ImGui::PushID((int)entt::to_integral(entity));
    }

    // --- Header: active checkbox + icon + name, then tag/static, matching Unity's Inspector
    // top block but with the same per-kind icon the Hierarchy already uses, so the two panels
    // read as one consistent visual language instead of the Inspector being icon-less.
    bool active = !registry.all_of<InactiveTag>(entity);
    {
        // #152 — same eye control as the Hierarchy. The header line hover-reveals it for an
        // active object; an inactive one always shows the dim eye-slash.
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ImGui::GetFrameHeight());
        const bool rowHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);
        if (ActiveToggle("##hdractive", active, rowHovered,
                         "Active - inactive objects are not drawn and don't collide",
                         /*alignTop=*/false)) {
            PushUndo(world, "Toggle Active");
            if (active) registry.emplace<InactiveTag>(entity);   // was active, now hide
            else        registry.remove<InactiveTag>(entity);
        }
    }
    ImGui::SameLine();

    const char* kindIcon = ICON_FA_DRAW_POLYGON;
    const char* kindTip = "Model - an imported or primitive mesh";
    if (registry.all_of<LightComponent>(entity)) { kindIcon = ICON_FA_LIGHTBULB; kindTip = "Light - casts light into the scene"; }
    else if (!registry.all_of<RenderableComponent>(entity)) { kindIcon = ICON_FA_DIAGRAM_PROJECT; kindTip = "Empty - a transform with no mesh, useful as a grouping pivot"; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kindIcon);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", kindTip);
    ImGui::SameLine();

    ImGui::SetNextItemWidth(-FLT_MIN);
    DrawNameField("##Name", name.Name, isLevelGeometry ? "Box" : "Object", activated);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) EditorUI::SetTooltip("Display name shown in the Hierarchy and here");
    if (activated) PushUndo(world, "Rename");

    {
        auto* tag = registry.try_get<TagComponent>(entity);
        std::string tagText = tag ? tag->Tag : std::string("Untagged");

        PropertyLabel("Tag", "Label for filtering/searching - e.g. search \"t:Enemy\" in the\nHierarchy to find every object tagged \"Enemy\".");
        // Leaves room for the Static checkbox after it instead of claiming the row's full width
        // the way a plain PropertyLabel field would — this row is the one place two fields
        // deliberately share a line, to keep the header block compact.
        const ImGuiStyle& st = ImGui::GetStyle();
        float staticReserve = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("Static").x
            + st.ItemSpacing.x + 6.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - staticReserve);

        auto setTag = [&](const std::string& t) {
            PushUndo(world, "Edit Tag");
            if (t.empty()) registry.remove<TagComponent>(entity);
            else registry.emplace_or_replace<TagComponent>(entity, t);
        };

        if (m_TagAdding) {
            // Inline "add a new tag" field, shown after picking "New tag..." in the combo.
            if (ImGui::IsWindowAppearing() || m_TagAddingJustOpened) {
                ImGui::SetKeyboardFocusHere();
                m_TagAddingJustOpened = false;
            }
            bool commit = ImGui::InputText("##NewTag", m_TagAddBuf, sizeof(m_TagAddBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (commit) {
                std::string t = SanitizeEntityName(m_TagAddBuf); // reuse the name scrubber - trims + strips control chars
                if (!t.empty()) setTag(t);
                m_TagAdding = false;
                m_TagAddBuf[0] = '\0';
            } else if (ImGui::IsItemDeactivated()) { // clicked away / Esc
                m_TagAdding = false;
                m_TagAddBuf[0] = '\0';
            }
        } else {
            // Gather the distinct tags already in use, sorted, for the dropdown.
            std::set<std::string> known;
            for (auto e : registry.view<const TagComponent>()) {
                const auto& tc = registry.get<TagComponent>(e);
                if (!tc.Tag.empty()) known.insert(tc.Tag);
            }
            if (ImGui::BeginCombo("##Tag", tagText.c_str())) {
                if (ImGui::Selectable("Untagged", !tag)) setTag("");
                if (!known.empty()) ImGui::Separator();
                for (const auto& t : known) {
                    if (ImGui::Selectable(t.c_str(), tag && tag->Tag == t)) setTag(t);
                }
                ImGui::Separator();
                if (ImGui::Selectable(ICON_FA_PLUS "  New tag...")) {
                    m_TagAdding = true;
                    m_TagAddingJustOpened = true;
                    m_TagAddBuf[0] = '\0';
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        bool isStatic = registry.all_of<StaticTag>(entity);
        if (ImGui::Checkbox("Static", &isStatic)) {
            PushUndo(world, "Toggle Static");
            if (isStatic) registry.emplace<StaticTag>(entity);
            else registry.remove<StaticTag>(entity);
        }
        if (ImGui::IsItemHovered()) {
            EditorUI::SetTooltip("Marks this object as never moving at runtime.\nDoesn't change behavior yet - just records the intent for later optimizations.");
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Transform (every entity has one; not removable, same as Unity) --------------------
    bool removed = false;
    if (BeginComponentSection(ICON_FA_UP_DOWN_LEFT_RIGHT, "Transform", false, removed,
            /*defaultOpen=*/true, "Position, rotation, and scale in the world. Every object has one.")) {

        // Stage on first touch, commit on release — one History entry per edit, and a
        // rejected (non-finite) or no-op edit records nothing (its snapshot dedupes away).
        bool rowActive = false, rowCommitted = false;
        // #218 — TransformComponent.Position is parent-relative once this entity has a parent
        // (World::SetParent re-expresses it into the new parent's local space), so the row's
        // label/tooltip need to say "Local" rather than claim world-space for those objects.
        const auto* posHier = registry.try_get<HierarchyComponent>(entity);
        bool hasParent = posHier && posHier->Parent != entt::null;
        DrawVec3Row(hasParent ? "Local Position" : "Position", transform.Position, 0.1f, 0.0f, 0.0f, rowActive, rowCommitted,
            hasParent
                ? "Position in units, relative to this object's parent. Drag a number to\nchange it, or click a colored letter to zero that axis."
                : "World-space position in units. Drag a number to change it, or\nclick a colored letter to zero that axis.");
        if (rowActive) StageUndo(world);
        if (rowCommitted) CommitStagedUndo(world, "Move");
        DrawVec3Row("Rotation", transform.RotationEuler, 1.0f, 0.0f, 0.0f, rowActive, rowCommitted,
            "Rotation in degrees around each axis.");
        if (rowActive) StageUndo(world);
        if (rowCommitted) CommitStagedUndo(world, "Rotate");
        DrawVec3Row("Scale", transform.Scale, isLevelGeometry ? 0.1f : 0.05f, 0.01f, 100.0f, rowActive, rowCommitted,
            "Size multiplier per axis - 1 is the original imported/created size.");
        if (rowActive) StageUndo(world);
        if (rowCommitted) CommitStagedUndo(world, "Scale");

        // Compact Reset / Copy / Paste for this Transform's values (audit #77). Small buttons so
        // they don't dominate the section.
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
        float tw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Reset##xf", ImVec2(tw, 0.0f))) {
            PushUndo(world, "Reset Transform");
            transform.Position = glm::vec3(0.0f);
            transform.RotationEuler = glm::vec3(0.0f);
            transform.Scale = glm::vec3(1.0f);
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Position 0, rotation 0, scale 1");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY "  Copy##xf", ImVec2(tw, 0.0f))) {
            m_TransformClipPos = transform.Position;
            m_TransformClipRot = transform.RotationEuler;
            m_TransformClipScale = transform.Scale;
            m_HasTransformClipboard = true;
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Copy this Transform's position/rotation/scale");
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_HasTransformClipboard);
        if (ImGui::Button(ICON_FA_PASTE "  Paste##xf", ImVec2(tw, 0.0f))) {
            PushUndo(world, "Paste Transform");
            transform.Position = m_TransformClipPos;
            transform.RotationEuler = m_TransformClipRot;
            transform.Scale = m_TransformClipScale;
        }
        if (ImGui::IsItemHovered() && m_HasTransformClipboard) EditorUI::SetTooltip("Paste the copied Transform values");
        ImGui::EndDisabled();
        ImGui::PopStyleVar();

        // Re-parenting lives in the Hierarchy panel (drag one row onto another), and Snap to
        // Ground lives in the toolbar now — neither duplicated here.
        EndComponentSection();
    }

    // --- Mesh Renderer ---------------------------------------------------------------------
    if (auto* renderable = registry.try_get<RenderableComponent>(entity)) {
        // Not removable on level geometry: a box IS its cube mesh, and removing it would leave
        // an invisible collider that the Hierarchy still lists under "Level Geometry".
        if (BeginComponentSection(ICON_FA_DRAW_POLYGON, "Mesh Renderer", !isLevelGeometry, removed,
                /*defaultOpen=*/true, "The mesh this object draws, and its material color/texture options.")) {
            std::string meshName = std::filesystem::path(renderable->ModelRef->Path()).filename().string();

            if (isLevelGeometry) {
                PropertyLabel("Color", "Solid tint for this box's surface. Click the swatch\nfor the full color picker, or type a hex value.");
                ImGui::ColorEdit3("##Color", &renderable->ModelRef->MeshMaterial(0).BaseColor.x, ImGuiColorEditFlags_DisplayHex);
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Color");
            }

            if (renderable->ModelRef->HasAnimations()) {
                for (int i = 0; i < renderable->ModelRef->AnimationCount(); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Button(ICON_FA_PLAY)) renderable->ModelRef->PlayAnimation(i);
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Play this animation clip");
                    ImGui::SameLine();
                    ImGui::TextUnformatted(renderable->ModelRef->AnimationName(i).c_str());
                    ImGui::PopID();
                }
                if (renderable->ModelRef->IsPlayingAnimation() && ImGui::Button("Stop Animation")) {
                    renderable->ModelRef->PlayAnimation(-1);
                }
            }

            // One row does everything: shows what's assigned, tells you its stats and full
            // path on hover, and lets you change it (click for a primitive, or drag a Model
            // from the Asset Browser onto it) — instead of a separate always-on info line above
            // plus a combo plus a drop button below, all doing pieces of the same job. Every
            // row in this section now shares the exact same label column, so nothing here
            // breaks the grid the rest of the Inspector already uses.
            PropertyLabel("Mesh");
            if (ImGui::Button(meshName.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                ImGui::OpenPopup("##ChangeMesh");
            }
            if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##ChangeMesh")) {
                EditorUI::SetTooltip("%s\n%u tris, %u verts\n\nClick to pick a primitive, or drag a Model here from the Asset Browser.",
                    renderable->ModelRef->Path().c_str(), renderable->ModelRef->TriangleCount(), renderable->ModelRef->VertexCount());
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
                    std::string path((const char*)payload->Data);
                    PushUndo(world, "Change Mesh");
                    renderable->ModelRef = assets.InstantiateModel(path);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopup("##ChangeMesh")) {
                auto pick = [&](const char* kind) {
                    PushUndo(world, "Change Mesh");
                    renderable->ModelRef = assets.CreatePrimitive(kind);
                };
                if (ImGui::Selectable(ICON_FA_CUBE "  Cube")) pick("cube");
                if (ImGui::Selectable(ICON_FA_CIRCLE "  Sphere")) pick("sphere");
                if (ImGui::Selectable(ICON_FA_SHAPES "  Cylinder")) pick("cylinder");
                if (ImGui::Selectable(ICON_FA_SHAPES "  Cone")) pick("cone");
                if (ImGui::Selectable(ICON_FA_SHAPES "  Plane")) pick("plane");
                ImGui::EndPopup();
            }

            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Renderer");
            // Keep the mesh on the entity (detached) so Add Component > Mesh Renderer can put
            // it back — otherwise re-adding gives a fresh cube and the original model is lost.
            registry.emplace_or_replace<DetachedMeshComponent>(entity, std::move(renderable->ModelRef));
            registry.remove<RenderableComponent>(entity);
        }
    }

    // --- Material -----------------------------------------------------------------------------
    // Its own top-level section now (not nested under Mesh Renderer), so single- and multi-select
    // present it the same way. Only meaningful when the object has a mesh to shade. Collapsed by
    // default — usually set once and left alone. Not removable (it's the model's material, not a
    // detachable component).
    if (registry.all_of<RenderableComponent>(entity)) {
        bool matRemoved = false;
        if (BeginComponentSection(ICON_FA_PALETTE, "Material", false, matRemoved, /*defaultOpen=*/false,
                "Surface appearance: color, metallic/roughness, emissive glow, and texture maps.")) {
            DrawMaterialEditor(world, assets, {m_Selected});
            EndComponentSection();
        }
    }

    // --- Collider (collapsed by default: a single checkbox, rarely revisited) --------------
    if (auto* collider = registry.try_get<ColliderComponent>(entity)) {
        if (BeginComponentSection(ICON_FA_CUBE, "Box Collider", true, removed, /*defaultOpen=*/false,
                "Lets this object block movement and be hit by raycasts.\nBounds follow its Transform/mesh automatically.")) {
            // Trigger volumes (non-blocking, enter/stay/exit events) aren't implemented — nothing
            // reads IsTrigger yet (#185). Per docs/CONVENTIONS.md rule 3 the toggle still shows
            // (a collider is where you'd look for it) but disabled, so it can't be set on an
            // assumption that isn't true. The field stays in the component + serializer for
            // forward-compat, and this re-wires to a live checkbox when #185 lands.
            PropertyLabel("Is Trigger", "Not implemented yet (#185): every collider blocks movement.\n"
                                        "Trigger volumes with enter/stay/exit events come with the\n"
                                        "collision-system work.");
            ImGui::BeginDisabled();
            ImGui::Checkbox("##IsTrigger", &collider->IsTrigger);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(not implemented)");
            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Collider");
            registry.remove<ColliderComponent>(entity);
        }
    }

    // --- Light (open by default — usually the main thing being tuned on a light entity) ----
    if (auto* light = registry.try_get<LightComponent>(entity)) {
        if (BeginComponentSection(ICON_FA_LIGHTBULB, "Light", true, removed,
            /*defaultOpen=*/true, "Casts light into the scene from this object's position.")) {
            // Slightly slimmer rows for this block — the light sliders read better less chunky.
            const ImVec2 lightFramePad(ImGui::GetStyle().FramePadding.x,
                                       std::max(1.0f, ImGui::GetStyle().FramePadding.y - 2.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, lightFramePad);

            PropertyLabel("Type", "Point: all directions. Spot: a cone. Directional: a sun (parallel rays, no position or range).");
            int kind = (int)light->Kind; // Point=0, Spot=1, Directional=2
            if (ImGui::Combo("##Type", &kind, "Point\0Spot\0Directional\0")) {
                PushUndo(world, "Edit Light");
                light->Kind = (LightComponent::Type)kind;
            }
            const bool isDir = light->Kind == LightComponent::Type::Directional;
            const bool isSpot = light->Kind == LightComponent::Type::Spot;
            const bool isPoint = light->Kind == LightComponent::Type::Point;

            // Color: raw swatch, or a Kelvin slider when ColorTempK > 0 (the swatch is then
            // driven, not authored). The K / RGB button flips between the two.
            PropertyLabel("Color", "The light's color. Toggle 'K' to drive it from a colour temperature instead.");
            if (light->ColorTempK > 0.0f) {
                float k = light->ColorTempK;
                KelvinBarResult kr = KelvinBar("##KelvinBar", k);
                if (kr.activated) PushUndo(world, "Edit Light");
                if (kr.changed) { light->ColorTempK = k; light->Color = KelvinToRGB(k); }
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                if (ActionButton("RGB##KelvinOff", "Set the colour directly (RGB)")) { PushUndo(world, "Edit Light"); light->ColorTempK = 0.0f; }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Switch back to a custom RGB swatch.");
            } else {
                ImGui::SetNextItemWidth(-60.0f);
                ImGui::ColorEdit3("##Color", &light->Color.x, ImGuiColorEditFlags_DisplayHex);
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
                ImGui::SameLine();
                if (ActionButton("K##KelvinOn", "Drive the colour from a temperature (Kelvin)")) {
                    PushUndo(world, "Edit Light");
                    light->ColorTempK = 6500.0f;
                    light->Color = KelvinToRGB(6500.0f);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Drive the colour from a temperature in Kelvin (1500-15000).");
            }

            PropertyLabel("Intensity", "Brightness multiplier - higher is brighter.");
            EditorUI::SliderFloat("##Intensity", &light->Intensity, 0.0f, 100.0f, "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
            if (!isDir) {
                PropertyLabel("Range", "Distance (in world units) at which the light's effect fades to zero.");
                EditorUI::SliderFloat("##Range", &light->Range, 0.1f, 200.0f, "%.1f",
                                      ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
            }
            if (isSpot) {
                PropertyLabel("Spot Angle", "Half-angle of the light cone, in degrees.\nThe cone points along the entity's -Z axis - use Rotation to aim it.");
                EditorUI::SliderFloat("##SpotAngle", &light->SpotAngleDegrees, 1.0f, 89.0f, "%.0f deg");
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
            }
            if (isDir) {
                PropertyLabel("Angular Size", "Apparent diameter of the sun disc, in degrees (~0.53 = Earth's sun).\nWider = softer shadows.\nAim the sun with the entity's Rotation (it shines along -Z).");
                EditorUI::SliderFloat("##AngularSize", &light->AngularSizeDegrees, 0.1f, 20.0f, "%.2f deg");
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
            }

            // --- Shadows sub-block, bound to light->Shadow --------------------------------
            auto& sh = light->Shadow;
            ImGui::Spacing();
            if (ImGui::TreeNodeEx(ICON_FA_MOON "  Shadows", ImGuiTreeNodeFlags_SpanAvailWidth)) {
                const char* castTip = isSpot
                    ? "Render a perspective shadow map for this spot light.\nEach casting spot is an extra full-scene depth pass; up to 4 at once."
                    : isPoint
                    ? "Render a 6-face cube shadow map for this point light.\nSix full-scene depth passes per casting light; up to 2 at once."
                    : "Render cascaded shadow maps for this sun.";
                PropertyLabel("Cast Shadows", castTip);
                bool en = sh.Enabled;
                if (ImGui::Checkbox("##LightCastShadows", &en)) { PushUndo(world, "Edit Light"); sh.Enabled = en; }
                PropertyLabel("Bias", "Multiplier on the shader's depth bias. >1 pushes the shadow off contact\n(fixes acne); <1 pulls it back toward the caster.");
                EditorUI::SliderFloat("##ShadowBias", &sh.Bias, 0.0f, 4.0f, "x%.2f");
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
                PropertyLabel("Normal Bias", "Multiplier on the normal-offset term (moves the sample along the surface\nnormal before comparing). Widen if edges show light bleed.");
                EditorUI::SliderFloat("##ShadowNormalBias", &sh.NormalBias, 0.0f, 4.0f, "x%.2f");
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
                PropertyLabel("Softness", "Multiplier on the PCF filter radius. Higher = wider, softer penumbra.");
                EditorUI::SliderFloat("##ShadowSoftness", &sh.Softness, 0.0f, 4.0f, "x%.2f");
                if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
                if (isSpot || isPoint) {
                    PropertyLabel("Near Plane", "Perspective near distance for this light's depth pass.\nRaise it to reclaim depth precision when the light sits far from what it lights.");
                    EditorUI::SliderFloat("##ShadowNear", &sh.NearPlane, 0.001f, 10.0f, "%.3f",
                                          ImGuiSliderFlags_Logarithmic);
                    if (ImGui::IsItemActivated()) PushUndo(world, "Edit Light");
                    PropertyLabel("Resolution", "Per-light shadow-map size (applies after per-light shadow maps).");
                    int res = sh.Resolution;
                    if (ImGui::Combo("##ShadowRes", &res, "Follow global\0" "512\0" "1024\0" "2048\0" "4096\0")) {
                        PushUndo(world, "Edit Light"); sh.Resolution = res;
                    }
                    PropertyLabel("Update Mode", "Dynamic re-renders every frame; Static bakes once (applies after per-light shadow maps).");
                    int um = sh.UpdateMode;
                    if (ImGui::Combo("##ShadowUpdate", &um, "Dynamic\0" "Static (bake once)\0" "Off\0")) {
                        PushUndo(world, "Edit Light"); sh.UpdateMode = um;
                    }
                }
                ImGui::TreePop();
            }

            // --- Workflow: look through this light / drop it to the surface below (#140 phase 4)
            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_EYE "  Look through", ImVec2(-FLT_MIN, 0.0f)))
                m_PendingLookThrough = entity;
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("View from this light's POV. Fly the camera to move / re-aim the\nlight from there. Spot uses its cone angle for FOV; Esc restores the camera.");
            if (ImGui::Button(ICON_FA_DOWN_LONG "  Drop to surface", ImVec2(-FLT_MIN, 0.0f))) {
                if (!DropLightToSurface(world, entity))
                    Log::Info("Drop to surface: nothing directly below this light.");
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Move the light straight down onto the nearest mesh surface below it.");

            ImGui::PopStyleVar(); // lightFramePad
            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Light");
            registry.remove<LightComponent>(entity);
        }
    }

    // --- Camera (the Game view previews through this while editing) ------------------------
    if (auto* cam = registry.try_get<CameraComponent>(entity)) {
        if (BeginComponentSection(ICON_FA_VIDEO, "Camera", true, removed, /*defaultOpen=*/true,
                "The Game view renders through this camera while editing, so you can frame a\nshot without walking there. Play mode still uses the first-person controller.")) {
            PropertyLabel("Field of View", "Vertical FOV in degrees.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            EditorUI::SliderFloat("##Fov", &cam->FovDegrees, 20.0f, 120.0f, "%.0f deg");
            if (ImGui::IsItemActivated()) PushUndo(world, "Edit Camera");
            PropertyLabel("Near", "Closest distance the camera renders.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##Near", &cam->NearPlane, 0.01f, 0.001f, 10.0f, "%.3f");
            if (ImGui::IsItemActivated()) PushUndo(world, "Edit Camera");
            PropertyLabel("Far", "Farthest distance the camera renders.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##Far", &cam->FarPlane, 1.0f, 1.0f, 100000.0f, "%.0f");
            if (ImGui::IsItemActivated()) PushUndo(world, "Edit Camera");
            if (cam->FarPlane <= cam->NearPlane) cam->FarPlane = cam->NearPlane + 1.0f;

            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_VIDEO "  Align to View", ImVec2(-FLT_MIN, 0.0f)) && m_EditorCameraPtr) {
                PushUndo(world, "Align Camera to View");
                const Camera& ec = *m_EditorCameraPtr;
                auto& t = registry.get<TransformComponent>(entity);
                t.Position = ec.Position;
                glm::vec3 d = glm::normalize(ec.Front());
                // Match the Add > Camera convention: ComposeTransform yaws (Y) then pitches (X),
                // local -Z is forward.
                t.RotationEuler = glm::vec3(
                    glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                    glm::degrees(std::atan2(-d.x, -d.z)),
                    0.0f);
                cam->FovDegrees = ec.Fov;
            }
            if (ImGui::IsItemHovered()) {
                EditorUI::SetTooltip("Snap this camera to the editor viewport's current position, aim and FOV.");
            }

            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Camera");
            registry.remove<CameraComponent>(entity);
        }
    }

    // --- Audio Source (collapsed by default: set-and-forget once a clip is chosen) ---------
    if (auto* audio = registry.try_get<AudioSourceComponent>(entity)) {
        if (BeginComponentSection(ICON_FA_VOLUME_HIGH, "Audio Source", true, removed, /*defaultOpen=*/false,
                "A sound clip that can be played from this object.")) {
            const std::string preview = audio->SoundPath.empty()
                ? "(none)" : std::filesystem::path(audio->SoundPath).filename().string();
            PropertyLabel("Clip", "Which imported sound this object plays.\nImport sounds via File > Import, or the Asset Browser.");
            if (ImGui::BeginCombo("##Clip", preview.c_str())) {
                for (const auto& sound : assets.Sounds()) {
                    bool isSelected = (sound == audio->SoundPath);
                    if (ImGui::Selectable(std::filesystem::path(sound).filename().string().c_str(), isSelected)) {
                        PushUndo(world, "Set Sound Clip");
                        audio->SoundPath = sound;
                    }
                }
                ImGui::EndCombo();
            }
            if (!audio->SoundPath.empty() &&
                ActionButton(ICON_FA_PLAY "  Preview", "Play the clip once, right now, to check how it sounds")) {
                AudioEngine::Play(audio->SoundPath);
            }

            PropertyLabel("Volume", "Playback volume - 1 is unattenuated.");
            if (ImGui::DragFloat("##AudioVolume", &audio->Volume, 0.01f, 0.0f, 1.0f, "%.2f")) {
                audio->Volume = glm::clamp(audio->Volume, 0.0f, 1.0f);
            }
            if (ImGui::IsItemActivated()) PushUndo(world, "Edit Audio Volume");

            bool loop = audio->Loop;
            if (ImGui::Checkbox("Loop", &loop)) {
                PushUndo(world, "Toggle Audio Loop");
                audio->Loop = loop;
            }
            ImGui::SameLine();
            bool playOnStart = audio->PlayOnStart;
            if (ImGui::Checkbox("Play On Start", &playOnStart)) {
                PushUndo(world, "Toggle Audio Play On Start");
                audio->PlayOnStart = playOnStart;
            }
            if (ImGui::IsItemHovered()) {
                EditorUI::SetTooltip("Plays this clip automatically the instant Play mode is entered.");
            }
            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Audio Source");
            registry.remove<AudioSourceComponent>(entity);
        }
    }

    // #184: sections for reflection-registered components (ComponentRegistry). One widget per
    // field, chosen by ReflectFieldType — no per-component code here; registering a component
    // gives it a section for free. Undo follows the same lightweight pattern the hand-coded
    // sections above use (push on the frame an edit starts).
    for (const auto& rc : ComponentRegistry::All()) {
        if (!rc.Has(registry, entity)) continue;
        bool reflRemoved = false;
        if (BeginComponentSection(rc.Meta.Icon, rc.Meta.Name, true, reflRemoved, /*defaultOpen=*/true, rc.Meta.Tooltip)) {
            void* fbase = rc.Get(registry, entity);
            for (const ReflectField& f : rc.Meta.Fields) {
                PropertyLabel(f.Name, f.Tooltip);
                ImGui::PushID(f.Name);
                bool started = false;
                switch (f.Type) {
                    case ReflectFieldType::Bool:
                        if (ImGui::Checkbox("##v", reinterpret_cast<bool*>(f.Address(fbase))))
                            PushUndo(world, std::string("Edit ") + rc.Meta.Name);
                        break;
                    case ReflectFieldType::Int:
                        ImGui::DragInt("##v", reinterpret_cast<int*>(f.Address(fbase)), f.DragSpeed,
                            (int)f.Min, (int)f.Max);
                        started = ImGui::IsItemActivated();
                        break;
                    case ReflectFieldType::Float:
                        ImGui::DragFloat("##v", reinterpret_cast<float*>(f.Address(fbase)), f.DragSpeed,
                            f.Min, f.Max, "%.3f");
                        started = ImGui::IsItemActivated();
                        break;
                    case ReflectFieldType::Vec3:
                        ImGui::DragFloat3("##v", reinterpret_cast<float*>(f.Address(fbase)), f.DragSpeed,
                            f.Min, f.Max);
                        started = ImGui::IsItemActivated();
                        break;
                    case ReflectFieldType::String: {
                        auto* s = reinterpret_cast<std::string*>(f.Address(fbase));
                        char buf[256];
                        snprintf(buf, sizeof(buf), "%s", s->c_str());
                        if (ImGui::InputText("##v", buf, sizeof(buf))) *s = buf;
                        started = ImGui::IsItemActivated();
                        break;
                    }
                }
                if (started) PushUndo(world, std::string("Edit ") + rc.Meta.Name);
                ImGui::PopID();
            }
            EndComponentSection();
        }
        if (reflRemoved) {
            PushUndo(world, std::string("Remove ") + rc.Meta.Name);
            rc.Remove(registry, entity);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Compact footer actions — smaller than the default so this row doesn't feel heavy.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));

    DrawAddComponentMenu(world, assets, entity);

    ImGui::Spacing();
    // Right-aligned flat icon row — Duplicate / Prefab / Delete, names + shortcuts in tooltips.
    const float ib = ImGui::GetFrameHeight() + 8.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ib * 3.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f);
    if (ActionButton(ICON_FA_CLONE, "Duplicate this object (Ctrl+D)", false, ImVec2(ib, 0.0f)))
        DuplicateSelection(world, assets);
    ImGui::SameLine();
    if (ActionButton(ICON_FA_BOX_ARCHIVE, "Save this object as a reusable .prefab asset", false, ImVec2(ib, 0.0f))) {
        std::string path = FileDialog::SaveFile("Prefab Files\0*.prefab\0All Files\0*.*\0", "prefab", m_Window);
        if (!path.empty() && SceneSerializer::SavePrefab(world, entity, path)) assets.RegisterPrefab(path);
    }
    ImGui::SameLine();
    if (DangerIconButton(ICON_FA_TRASH, "Delete this object (Del)", ImVec2(ib, 0.0f)))
        DeleteSelection(world);

    ImGui::PopStyleVar();

    ImGui::PopID();
    InspectorEnd();
}

bool EditorLayer::BeginComponentSection(const char* icon,
    const char* label, bool removable, bool& removedOut, bool defaultOpen, const char* tooltip) {
    removedOut = false;

    std::string header = std::string(icon) + "  " + label;
    // CollapsingHeader claims its ENTIRE row as one hit-test region by default, so without
    // AllowOverlap the "x" button drawn on top of that same row below never actually receives
    // the click — it lands on the header's own collapse-toggle instead, which is exactly why
    // pressing it only expanded/collapsed the section instead of removing anything.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap | (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    // Flat heading (#155): no filled bar — the triangle + icon + label sit on the panel, with a
    // hairline under them for separation. A barely-there wash marks hover.
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    bool open = ImGui::CollapsingHeader(header.c_str(), flags);
    ImGui::PopStyleColor(3);
    const bool headerHovered = ImGui::IsItemHovered();
    if (tooltip && headerHovered) EditorUI::SetTooltip("%s", tooltip);
    // Flat themes get a hairline under the header for separation; the Bento card's own border
    // replaces it (#234 layer 3).
    if (!UseBentoLayout()) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 0.5f), ImVec2(mx.x, mx.y - 0.5f),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
    }

    if (removable) {
        // Right-aligned remove control on the header's line, always visible so it's discoverable
        // (#155): flat at rest, red on hover — the shared destructive-icon treatment (#156).
        const float bw = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bw);
        ImGui::PushID(label);
        if (DangerIconButton(ICON_FA_XMARK, "Remove this component", ImVec2(bw, 0.0f)))
            removedOut = true;
        ImGui::PopID();
    }

    bool showBody = open && !removedOut;
    m_ComponentSectionIsCard = false;
    if (showBody && UseBentoLayout()) {
        // Bento: the section body sits in a rounded hairline-bordered card — delineation only, no
        // fill, so the whole Inspector stays one tone (the header stays outside the card).
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // no fill
        ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * m_UIScale, 10.0f * m_UIScale));
        ImGui::BeginChild(label, ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        m_ComponentSectionIsCard = true;
    } else if (showBody) {
        // Flat themes: indenting the body reads as "these fields belong to that header".
        ImGui::Indent();
    }
    return showBody; // callers must call EndComponentSection() whenever this returns true
}

void EditorLayer::EndComponentSection() {
    if (m_ComponentSectionIsCard) {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        m_ComponentSectionIsCard = false;
    } else {
        ImGui::Unindent();
    }
    ImGui::Spacing();
}
void EditorLayer::DrawAddComponentMenu(World& world, AssetLibrary& assets, entt::entity entity) {
    if (ActionButton(ICON_FA_PLUS "  Add Component", "Attach a new capability to this object",
                     false, ImVec2(-1.0f, 0.0f))) {
        ImGui::OpenPopup("##AddComponentPopup");
    }
    if (!ImGui::BeginPopup("##AddComponentPopup")) return;

    auto& registry = world.Registry;
    // Greyed out rather than hidden when already present, so the menu's contents (i.e. what the
    // engine actually supports) stay the same every time it's opened.
    auto entry = [&](const char* icon, const char* label, bool alreadyHas, const std::function<void()>& add) {
        std::string text = std::string(icon) + "  " + label;
        if (ImGui::MenuItem(text.c_str(), nullptr, false, !alreadyHas)) {
            PushUndo(world, std::string("Add ") + label);
            add();
        }
    };

    ImGui::SeparatorText("Rendering");
    // Restores the mesh the entity had before "Mesh Renderer" was removed (DetachedMeshComponent),
    // or a fresh cube if there's nothing to restore — the section's own drop target (drag a Model
    // from the Asset Browser onto it) then repoints it.
    entry(ICON_FA_DRAW_POLYGON, "Mesh Renderer", registry.all_of<RenderableComponent>(entity),
        [&] {
            std::shared_ptr<Model> mesh;
            if (auto* detached = registry.try_get<DetachedMeshComponent>(entity)) {
                mesh = std::move(detached->ModelRef);
                registry.remove<DetachedMeshComponent>(entity);
            }
            if (!mesh) mesh = assets.CreatePrimitive("cube");
            registry.emplace<RenderableComponent>(entity, std::move(mesh));
        });
    entry(ICON_FA_LIGHTBULB, "Light", registry.all_of<LightComponent>(entity),
        [&] { registry.emplace<LightComponent>(entity); });
    entry(ICON_FA_VIDEO, "Camera", registry.all_of<CameraComponent>(entity),
        [&] { registry.emplace<CameraComponent>(entity); });

    ImGui::SeparatorText("Physics");
    entry(ICON_FA_CUBE, "Box Collider", registry.all_of<ColliderComponent>(entity),
        [&] { registry.emplace<ColliderComponent>(entity); });

    ImGui::SeparatorText("Audio");
    entry(ICON_FA_VOLUME_HIGH, "Audio Source", registry.all_of<AudioSourceComponent>(entity),
        [&] { registry.emplace<AudioSourceComponent>(entity); });

    // #184: reflection-registered components. Adding one to ComponentRegistry puts it here with
    // no edit to this menu. Animator used to be its own hand-coded "Motion" entry here; now it's
    // just another entry in this list, same as Transform Controller and Spin.
    if (!ComponentRegistry::All().empty()) {
        ImGui::SeparatorText("Scripts");
        for (const auto& rc : ComponentRegistry::All()) {
            entry(rc.Meta.Icon, rc.Meta.Name, rc.Has(registry, entity),
                  [&] { rc.Add(registry, entity); });
        }
    }

    ImGui::EndPopup();
}

// Unified single-/multi-select material editor (#183): a size-1 `sel` is the degenerate case of
// the general multi-edit path below, so single-select gets the same tri-state "Use Custom
// Material" handling and mixed-value dashes (which simply never trigger for one object) instead
// of a second, drifting copy of every field.
void EditorLayer::DrawMaterialEditor(World& world, AssetLibrary& assets,
                                     const std::vector<entt::entity>& sel) {
    // Two placed instances of the same imported file share one Model — and therefore one
    // MaterialOverride. Collapse to the distinct Model* set so each override is read/written
    // exactly once (and one undo step covers the lot).
    std::vector<Model*> models;
    for (entt::entity e : sel) {
        if (!world.Registry.valid(e)) continue;
        auto* rc = world.Registry.try_get<RenderableComponent>(e);
        if (!rc || !rc->ModelRef) { ImGui::TextDisabled("A selected object has no mesh."); return; }
        if (rc->ModelRef->MeshCount() == 0) { ImGui::TextDisabled("A selected mesh failed to load."); return; }
        Model* m = rc->ModelRef.get();
        if (std::find(models.begin(), models.end(), m) == models.end()) models.push_back(m);
    }
    if (models.empty()) return;

    const int total = (int)models.size();
    int nCustom = 0;
    for (Model* m : models) if (m->MaterialOverride()) nCustom++;

    // Tri-state "Use Custom Material": create a fresh override on every model that lacks one
    // (keeping its imported texture maps, dropping the leftover diffuse factors — same recipe
    // as the single-object editor), or clear it from all.
    bool customMixed = nCustom != 0 && nCustom != total;
    {
        bool value = nCustom > 0;
        if (customMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        bool clicked = ImGui::Checkbox("Use Custom Material", &value);
        if (customMixed) ImGui::PopItemFlag();
        if (clicked) {
            bool enable = customMixed ? true : value;
            PushUndo(world, "Edit Material");
            for (Model* m : models) {
                if (enable && !m->MaterialOverride()) {
                    auto mat = std::make_shared<Material>();
                    if (m->MeshCount() > 0) {
                        const Material& imported = m->MeshMaterial(0);
                        mat->AlbedoMap = imported.AlbedoMap;
                        mat->NormalMap = imported.NormalMap;
                        mat->MetallicMap = imported.MetallicMap;
                        mat->RoughnessMap = imported.RoughnessMap;
                        mat->AOMap = imported.AOMap;
                        mat->EmissiveMap = imported.EmissiveMap;
                    }
                    m->SetMaterialOverride(mat);
                } else if (!enable) {
                    m->SetMaterialOverride(nullptr);
                }
            }
            nCustom = enable ? total : 0;
        }
    }
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("Override every selected object with one editable PBR material.\n"
                             "Unchecking restores each object's imported / default material.");

    if (nCustom != total) {
        ImGui::TextDisabled(nCustom == 0
            ? "Using imported / default materials.\nEnable a custom material to edit shared PBR properties."
            : "Only some selected objects use a custom material.\nEnable it on all of them to edit shared properties here.");
        return;
    }

    std::vector<Material*> mats;
    for (Model* m : models) mats.push_back(m->MaterialOverride().get());

    auto vec3Shared = [&](glm::vec3 Material::* field, glm::vec3& shared) {
        shared = mats[0]->*field;
        bool mixed = false;
        for (Material* mm : mats)
            for (int a = 0; a < 3; ++a)
                if (std::fabs((mm->*field)[a] - shared[a]) > 1.0e-4f) mixed = true;
        return mixed;
    };
    auto floatShared = [&](float Material::* field, float& shared) {
        shared = mats[0]->*field;
        bool mixed = false;
        for (Material* mm : mats) if (std::fabs(mm->*field - shared) > 1.0e-4f) mixed = true;
        return mixed;
    };

    // --- Colors: shared swatch, "(mixed)" tag when they disagree; one edit writes all. ---
    auto colorRow = [&](const char* label, glm::vec3 Material::* field, const char* tip) {
        glm::vec3 shared; bool mixed = vec3Shared(field, shared);
        PropertyLabel(label, tip);
        // PropertyLabel just set the widget to fill to the right edge; when a "(mixed)" tag
        // has to follow, claw back exactly its width so it isn't clipped off-panel.
        if (mixed) {
            float w = ImGui::GetContentRegionAvail().x -
                      ImGui::CalcTextSize(" (mixed)").x - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(w > 40.0f ? w : 40.0f);
        }
        glm::vec3 edit = shared;
        ImGui::PushID(label);
        bool changed = ImGui::ColorEdit3("##c", &edit.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) StageUndo(world);
        if (changed) for (Material* mm : mats) mm->*field = edit;
        if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");
        if (mixed) { ImGui::SameLine(); ImGui::TextDisabled("(mixed)"); }
        ImGui::PopID();
    };
    auto scalarRow = [&](const char* label, float Material::* field, float lo, float hi, const char* tip) {
        float shared; bool mixed = floatShared(field, shared);
        float edit = shared;
        MultiEditResult r = MultiEditFloatRow(label, edit, mixed, (hi - lo) * 0.004f, lo, hi, tip);
        if (r.activated) StageUndo(world);
        if (r.changed) for (Material* mm : mats) mm->*field = edit;
        if (r.committed) CommitStagedUndo(world, "Edit Material");
    };

    colorRow("Base Color", &Material::BaseColor,
             "Surface tint, multiplied with the Albedo map. Applied to every selected material.");
    scalarRow("Metallic", &Material::Metallic, 0.0f, 1.0f,
              "0 = non-metal, 1 = pure metal. Ignored where a Metallic map is set.");
    scalarRow("Roughness", &Material::Roughness, 0.04f, 1.0f,
              "0 = mirror-smooth, 1 = fully matte. Ignored where a Roughness map is set.");
    colorRow("Emissive Color", &Material::EmissiveColor,
             "Color this surface glows, independent of scene lighting.");
    scalarRow("Emissive Strength", &Material::EmissiveStrength, 0.0f, 10.0f,
              "Brightness multiplier for the Emissive Color / map.");

    ImGui::SeparatorText("Texture Maps");

    auto mapRow = [&](const char* label, std::shared_ptr<Texture> Material::* slot, const char* help) {
        ImGui::PushID(label);
        PropertyLabel(label);

        Texture* first = (mats[0]->*slot).get();
        bool mixed = false, anySet = false;
        for (Material* mm : mats) {
            Texture* t = (mm->*slot).get();
            if (t) anySet = true;
            if (t != first) mixed = true;
        }
        std::string preview = mixed ? std::string("\xE2\x80\x94  (mixed)")
                              : first ? std::filesystem::path(first->Path()).filename().string()
                                      : std::string("(none)");
        float clearReserve = anySet ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
        if (ImGui::Button(preview.c_str(), ImVec2(anySet ? -clearReserve : -FLT_MIN, 0.0f))) {
            std::string path = FileDialog::OpenFile(
                "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
            if (!path.empty()) {
                PushUndo(world, std::string("Set ") + label + " Map");
                auto tex = assets.LoadTexture(path);
                for (Material* mm : mats) mm->*slot = tex;
            }
        }
        if (ImGui::IsItemHovered()) {
            if (!mixed && first) {
                ImGui::BeginTooltip();
                ImGui::Image((ImTextureID)(intptr_t)first->GLHandle(), ImVec2(96, 96));
                ImGui::TextUnformatted(first->Path().c_str());
                ImGui::EndTooltip();
            } else {
                EditorUI::SetTooltip("Click to import an image, or drag one from the Asset Browser.\n"
                                     "Assigned to every selected material.");
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                std::string texPath((const char*)payload->Data);
                PushUndo(world, std::string("Set ") + label + " Map");
                auto tex = assets.LoadTexture(texPath);
                for (Material* mm : mats) mm->*slot = tex;
            }
            ImGui::EndDragDropTarget();
        }
        if (anySet) {
            ImGui::SameLine();
            if (ActionButton(ICON_FA_XMARK, "Clear on all", false, ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                PushUndo(world, std::string("Clear ") + label + " Map");
                for (Material* mm : mats) mm->*slot = nullptr;
            }
        }
        if (help) EditorUI::HelpMarker(help);
        ImGui::PopID();
    };

    mapRow("Albedo", &Material::AlbedoMap, "The base color texture (diffuse / base color map).");
    mapRow("Normal", &Material::NormalMap, "Fine surface detail (bumps, grooves) without extra geometry.");
    mapRow("Metallic", &Material::MetallicMap, "Grayscale: white = metal. Overrides the Metallic value above.");
    mapRow("Roughness", &Material::RoughnessMap, "Grayscale: white = matte. Overrides the Roughness value above.");
    mapRow("AO", &Material::AOMap, "Ambient occlusion - darkens crevices and contact points.");
    mapRow("Emissive", &Material::EmissiveMap, "Texture for glowing areas, tinted by Emissive Color.");
}
