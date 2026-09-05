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

namespace {

// Height of the top toolbar: the dropdown menu bar row + the one-click icon row, trimmed so the
// icons sit snug against the bottom edge instead of floating in a tall strip of dead space.
constexpr float kToolbarHeight = 52.0f;

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

// Where a newly added object goes: a few units in front of the editor camera. If the camera
// has somehow gone non-finite, fall back to the origin so the object is still findable rather
// than spawned at inf/NaN and lost.
inline glm::vec3 SafeSpawnInFrontOf(const Camera& cam, float distance = 5.0f) {
    glm::vec3 p = cam.Position + cam.Front() * distance;
    if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
    return glm::vec3(0.0f);
}

// Euler angles in degrees (X,Y,Z as stored in TransformComponent::RotationEuler) from a matrix,
// using the SAME Ry*Rx*Rz decomposition ComposeTransform builds it with — so a gizmo-set
// rotation and an Inspector-typed one agree and compose(decompose(M)) == M (#108). ImGuizmo's
// own DecomposeMatrixToComponents uses a different order, which made objects jump when a gizmo
// rotate was followed by an Inspector nudge (or vice versa).
inline glm::vec3 EulerYXZFromMatrix(const glm::mat4& m) {
    glm::mat3 b(m);
    for (int c = 0; c < 3; ++c) {
        float len = glm::length(b[c]);
        if (len > 1e-8f) b[c] /= len;
    }
    if (glm::determinant(b) < 0.0f) b[0] = -b[0]; // mirrored basis (negative scale) has no clean Euler
    float ey, ex, ez;
    glm::extractEulerAngleYXZ(glm::mat4(b), ey, ex, ez);
    glm::vec3 d = glm::degrees(glm::vec3(ex, ey, ez));
    for (int i = 0; i < 3; ++i) if (std::fabs(d[i]) < 1.0e-4f) d[i] = 0.0f; // kill dust / -0.0
    return d;
}

// Turn a freshly created light entity into a sun: same raking angle / intensity / disc size the
// SceneSerializer synthesises for a scene with no directional light, so an added one casts a
// readable shadow immediately rather than sitting near-overhead and washed out (#125).
inline void MakeDirectionalLight(World& world, entt::entity e) {
    if (e == entt::null || !world.Registry.valid(e)) return;
    auto& lc = world.Registry.get<LightComponent>(e);
    lc.Kind = LightComponent::Type::Directional;
    lc.Intensity = 6.0f;
    lc.AngularSizeDegrees = 2.0f;
    lc.Shadow.Enabled = true; // a freshly added sun casts shadows by default
    world.Registry.get<TransformComponent>(e).RotationEuler = glm::vec3(-36.25f, 53.13f, 0.0f);
}

// Reverse lookup for the Asset Browser: which placed objects reference a given asset.
// Compared by raw pointer (Model/Texture) since two placed objects can share one instance.
// Level-geometry entities excluded — their cube Model is private/unshared, never an "asset".
std::vector<std::string> FindModelUsages(const World& world, const Model* model) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : view) {
        const auto& [name, renderable] = view.get<const NameComponent, const RenderableComponent>(entity);
        if (renderable.ModelRef.get() == model) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

bool MaterialUsesTexture(const Material& mat, const Texture* tex) {
    return mat.AlbedoMap.get() == tex || mat.NormalMap.get() == tex || mat.MetallicRoughnessMap.get() == tex ||
           mat.MetallicMap.get() == tex || mat.RoughnessMap.get() == tex || mat.AOMap.get() == tex ||
           mat.EmissiveMap.get() == tex;
}

std::vector<std::string> FindTextureUsages(const World& world, const Texture* tex) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : view) {
        const auto& [name, renderable] = view.get<const NameComponent, const RenderableComponent>(entity);
        bool used = false;
        if (auto override_ = renderable.ModelRef->MaterialOverride()) {
            used = MaterialUsesTexture(*override_, tex);
        } else {
            for (int i = 0; i < renderable.ModelRef->MeshCount(); ++i) {
                if (MaterialUsesTexture(renderable.ModelRef->MeshMaterial(i), tex)) { used = true; break; }
            }
        }
        if (used) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

std::vector<std::string> FindSoundUsages(const World& world, const std::string& path) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const AudioSourceComponent>();
    for (auto entity : view) {
        const auto& [name, audio] = view.get<const NameComponent, const AudioSourceComponent>(entity);
        if (audio.SoundPath == path) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

// True only for entities the vertex-grab workflow applies to. Level-geometry boxes qualify now
// too — their cube primitive has real vertices to grab, same as any placed cube.
bool IsVertexDraggable(World& world, entt::entity entity) {
    if (entity == entt::null || !world.Registry.valid(entity)) return false;
    // There has to be a mesh to grab a vertex FROM — a light or empty has none. Without this,
    // holding V over a selected light/empty would reach FindVertexUnderCursor's unchecked
    // get<RenderableComponent>(), which is undefined behavior (a crash) on an entity that
    // doesn't have one.
    if (!world.Registry.all_of<RenderableComponent>(entity)) return false;
    // Vertex-drag math below works entirely in local space (matching TransformComponent for an
    // unparented entity); a parented entity's TransformComponent is local-relative-to-parent, so
    // mixing it with the world-space grab point would move the object to the wrong place. Not
    // supported for now.
    if (const auto* hier = world.Registry.try_get<HierarchyComponent>(entity)) {
        if (hier->Parent != entt::null) return false;
    }
    return true;
}

// The Hierarchy lists entities by OrderComponent (a stable per-entity sequence assigned at
// creation and preserved through save/load), so a snapshot round-trip — undo/redo, Play->Stop —
// no longer reshuffles the list. Entities predating OrderComponent (0) keep a stable relative
// order via the entity-handle tiebreak.
template <typename View>
std::vector<entt::entity> ViewInCreationOrder(const entt::registry& reg, View view) {
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::sort(entities.begin(), entities.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = reg.try_get<OrderComponent>(a);
        const auto* ob = reg.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b;
    });
    return entities;
}

// 1-based position of `entity` within its kind's creation-ordered list — the number behind the
// "Box 3" / "Object 7" fallback shown for entities the user never named (#21 P10).
int CreationOrdinal(const entt::registry& reg, entt::entity entity) {
    auto list = ViewInCreationOrder(reg, reg.view<const NameComponent>());
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == entity) return static_cast<int>(i) + 1;
    return 0;
}

// The gizmo drag and the Inspector's own number fields edit the same thing; give the History
// entry the same verb either way ("Move" / "Rotate" / "Scale") instead of a generic
// "Transform" from the gizmo path only (#19 P8).
const char* GizmoOpUndoLabel(GizmoOp op) {
    switch (op) {
        case GizmoOp::Rotate: return "Rotate";
        case GizmoOp::Scale:  return "Scale";
        case GizmoOp::Rect:   return "Edit Bounds";
        case GizmoOp::Translate:
        default:              return "Move";
    }
}

std::string UsageTooltip(const std::vector<std::string>& users) {
    if (users.empty()) return "Not currently used by anything in the scene.";
    std::string s = "Used by:\n";
    for (size_t i = 0; i < users.size() && i < 10; ++i) s += "  - " + users[i] + "\n";
    if (users.size() > 10) s += "  ...and " + std::to_string(users.size() - 10) + " more\n";
    s.pop_back(); // drop the trailing newline
    return s;
}

// Case-insensitive substring match — shared by the Console filter, Hierarchy filter, and (as
// one piece of the richer parser below) the Asset Browser search.
bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return toLower(text).find(toLower(filter)) != std::string::npos;
}

// Unity's Project-window search syntax: plain words match the asset name and are ANDed
// together ("coastal scene" needs both words); "t:Model" restricts by asset type, and several
// t: terms are ORed ("t:Model t:Texture" means either kind); "l:label" restricts by label, and
// several l: terms are ANDed (must carry every listed label). Typed directly or built by the
// Type/Label filter dropdown buttons - both just edit this same text.
struct ParsedAssetSearch {
    std::vector<std::string> nameTerms;
    std::vector<std::string> typeTerms;  // lowercased kind names: "model","texture","sound","scene","prefab","folder"
    std::vector<std::string> labelTerms; // lowercased
};

ParsedAssetSearch ParseAssetSearch(const std::string& filter) {
    ParsedAssetSearch result;
    std::istringstream iss(filter);
    std::string token;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    while (iss >> token) {
        if (token.size() > 2 && (token[0] == 't' || token[0] == 'T') && token[1] == ':') {
            result.typeTerms.push_back(toLower(token.substr(2)));
        } else if (token.size() > 2 && (token[0] == 'l' || token[0] == 'L') && token[1] == ':') {
            result.labelTerms.push_back(toLower(token.substr(2)));
        } else {
            result.nameTerms.push_back(token);
        }
    }
    return result;
}

// `kind` is the asset's type name (already lowercased: "model", "texture", ...), `labels` its
// current label set. Name terms AND, type terms OR (any one is enough), label terms AND (every
// one must be present) - matching the semantics Unity documents for t:/l:.
bool MatchesAssetSearch(const ParsedAssetSearch& parsed, const std::string& name, const std::string& kind,
    const std::set<std::string>& labels) {
    for (const auto& term : parsed.nameTerms) {
        if (!MatchesFilter(term, name)) return false;
    }
    if (!parsed.typeTerms.empty()) {
        bool anyTypeMatches = false;
        for (const auto& t : parsed.typeTerms) {
            if (t == kind) { anyTypeMatches = true; break; }
        }
        if (!anyTypeMatches) return false;
    }
    for (const auto& term : parsed.labelTerms) {
        bool found = false;
        for (const auto& lbl : labels) {
            if (MatchesFilter(term, lbl) && term.size() == lbl.size()) { found = true; break; } // exact, case-insensitive
        }
        if (!found) return false;
    }
    return true;
}

// Adds `token` (e.g. "t:Model") to `filter`'s text if it's not already there, or removes it if
// it is - how the Type/Label filter dropdown checkboxes edit the plain search text, since
// that's the one source of truth Unity's own filters work the same way against.
void ToggleSearchToken(std::string& filter, const std::string& token) {
    std::istringstream iss(filter);
    std::vector<std::string> tokens;
    std::string t;
    bool removed = false;
    while (iss >> t) {
        if (t == token) { removed = true; continue; }
        tokens.push_back(t);
    }
    if (!removed) tokens.push_back(token);
    filter.clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) filter += " ";
        filter += tokens[i];
    }
}

bool SearchHasToken(const std::string& filter, const std::string& token) {
    std::istringstream iss(filter);
    std::string t;
    while (iss >> t) {
        if (t == token) return true;
    }
    return false;
}

// Virtual folder paths are '/'-joined segments (e.g. "Props/Guns") — these split off the
// last segment, used throughout the Asset Browser for breadcrumbs, rename, and re-parenting.
std::string ParentFolderOf(const std::string& folderPath) {
    size_t slash = folderPath.find_last_of('/');
    return slash == std::string::npos ? std::string() : folderPath.substr(0, slash);
}
std::string LeafNameOf(const std::string& folderPath) {
    size_t slash = folderPath.find_last_of('/');
    return slash == std::string::npos ? folderPath : folderPath.substr(slash + 1);
}

// A rename field otherwise takes an arbitrary-length string with raw control bytes in it, which
// truncate oddly in the Hierarchy/Inspector and could reach a log or label path (#38 B12).
// Strip C0 controls + DEL, cap the length, and (on commit) trim the ends. UTF-8 multibyte
// (>= 0x80) is kept so CJK / emoji names still round-trip.
inline std::string SanitizeEntityName(const std::string& in, bool trimEnds = true) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in)
        if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));

    if (trimEnds) {
        size_t b = out.find_first_not_of(" \t");
        size_t e = out.find_last_not_of(" \t");
        out = (b == std::string::npos) ? std::string() : out.substr(b, e - b + 1);
    }

    constexpr size_t kMaxEntityNameLen = 64;
    if (out.size() > kMaxEntityNameLen) {
        out.resize(kMaxEntityNameLen);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back(); // don't leave a half UTF-8 sequence
    }
    return out;
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

// ============================================================================================
// Two — and only two — button treatments across the whole editor (#160):
//
//   ActionButton   flat: no body at rest, just the glyph (or glyph + short label), faint wash
//                  on hover. `active` gives it an accent-tinted body + a 2px bottom keyline for
//                  toggles that are "on". This is every toolbar tool, every panel toggle, every
//                  low-frequency icon action (eye, expand/collapse, breadcrumb, kelvin mode, +).
//                  DangerIconButton is the same treatment with a red-on-hover wash, for
//                  destructive icons (Delete, the component-remove ✕).
//
//   PrimaryButton  the one filled/emphasis style: modal-dialog buttons only (Save / Don't Save
//                  / Restore / Cancel …), where a raised body helps them read as the choice.
//
// Nothing else. No raw ImGui::Button / ImGui::SmallButton for chrome; no per-site colour pushes.
// ============================================================================================
bool ActionButton(const char* icon, const char* tooltip, bool active = false, ImVec2 size = ImVec2(0, 0)) {
    const ImVec4 keyline(0.55f, 0.60f, 0.72f, 1.0f);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.400f, 0.435f, 0.520f, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.400f, 0.435f, 0.520f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.400f, 0.435f, 0.520f, 0.60f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    }
    // Button() folds its label into its ID, so two buttons that ever show the same glyph would
    // collide — scope the ID to the (unique) tooltip string instead.
    ImGui::PushID(tooltip);
    bool clicked = ImGui::Button(icon, size);
    ImGui::PopID();
    if (active) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float y = mx.y - 2.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(mn.x + 3.0f, y), ImVec2(mx.x - 3.0f, mx.y - 1.0f),
                                                  ImGui::ColorConvertFloat4ToU32(keyline), 1.0f);
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tooltip);
    return clicked;
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

// The one filled treatment — theme accent body, for prominent/rare actions only.
bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_Header));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
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

// #152 — the one "is this object active" control, shared by the Hierarchy row and the Inspector
// header so both panels speak the same language. Borderless (matches ActionButton). It always
// occupies one frame-height slot so the layout never shifts:
//   inactive          -> a dim EYE_SLASH, always visible (this is the state worth surfacing)
//   active + hovered   -> a faint EYE the user can click to disable (row- or self-hover)
//   active + at rest   -> just a 2px dot, so the slot stays discoverable without adding chrome
// `rowHovered` is the caller's hit-test of the whole row/header line (the glyph is drawn before
// the rest of the row, so it can't rely on its own hover alone). Returns true on click.
// `alignTop`: place the glyph with its line-box at the row top (matches the Hierarchy's kind
// glyph). False = frame-centred, matching a framed widget on the same line (Inspector header).
bool ActiveToggle(const char* id, bool active, bool rowHovered, const char* tip, bool alignTop) {
    const float sz = ImGui::GetFrameHeight();
    // Width hugs the glyph (+2px) instead of a full sz-square, so the slot doesn't push the
    // rest of the row across the way the old sz-wide Button did (#152 follow-up).
    const float w = ImMax(ImGui::CalcTextSize(ICON_FA_EYE).x, ImGui::CalcTextSize(ICON_FA_EYE_SLASH).x) + 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##active", ImVec2(w, sz));
    bool selfHover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (selfHover) EditorUI::SetTooltip("%s", tip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Align to the Hierarchy's kind glyph, which is painted with its line-box top at the row
    // top (rowMin.y) rather than at FramePadding.y — so draw the eye the same way, not
    // frame-centred, or it rides ~FramePadding.y too low next to the cube/light icon.
    const float glyphTop = alignTop ? p0.y : p0.y + (sz - ImGui::GetTextLineHeight()) * 0.5f;
    const ImVec2 c(p0.x + w * 0.5f, glyphTop + ImGui::GetTextLineHeight() * 0.5f);
    auto glyph = [&](const char* g, ImU32 col) {
        const ImVec2 ts = ImGui::CalcTextSize(g);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, glyphTop), col, g);
    };
    if (!active) {
        glyph(ICON_FA_EYE_SLASH, ImGui::GetColorU32(ImGuiCol_TextDisabled, selfHover ? 1.0f : 0.90f));
    } else if (rowHovered || selfHover) {
        glyph(ICON_FA_EYE, ImGui::GetColorU32(ImGuiCol_Text, selfHover ? 0.85f : 0.45f));
    } else {
        dl->AddCircleFilled(c, ImMax(1.5f, sz * 0.06f), ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.50f));
    }
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
        gz.Colors[ImGuizmo::PLANE_X] = ImVec4(0.95f, 0.78f, 0.25f, 0.42f);
        gz.Colors[ImGuizmo::PLANE_Y] = ImVec4(0.95f, 0.78f, 0.25f, 0.42f);
        gz.Colors[ImGuizmo::PLANE_Z] = ImVec4(0.95f, 0.78f, 0.25f, 0.42f);
        gz.Colors[ImGuizmo::SELECTION] = ImVec4(1.00f, 0.55f, 0.10f, 0.60f);
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

// The editor's colour palette. Both themes share the sizes/rounding set in Init(); this only
// writes style.Colors[], so Preferences can swap it live with no font/size rebuild.
//   0 Dark Slate — monochrome greys + one desaturated cool-slate accent (the #92 default).
//   1 Prism      — near-black backgrounds; the accent, buttons, text tint and tab keyline are
//                  all hue-driven, spread across ~half the wheel and drifting through the
//                  spectrum together every frame (ApplyPrismAnimation, phase advanced in Draw()).
void EditorLayer::ApplyEditorTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

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

    for (auto& [model, tex] : m_ModelThumbnails) { (void)model; if (tex) glDeleteTextures(1, &tex); }
    m_ModelThumbnails.clear();
    if (m_ThumbnailBlitFbo) { glDeleteFramebuffers(1, &m_ThumbnailBlitFbo); m_ThumbnailBlitFbo = 0; }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// Rendered once per Model into its own small texture, then cached. Returns 0 while this frame's
// render budget is spent — the caller falls back to the type glyph and picks it up next frame.
unsigned int EditorLayer::ModelThumbnail(Model& model) {
    auto it = m_ModelThumbnails.find(&model);
    if (it != m_ModelThumbnails.end()) return it->second;
    if (m_ThumbnailBudgetThisFrame <= 0) return 0;
    m_ThumbnailBudgetThisFrame--;

    const int kSize = 128;
    const float dist = ModelPreviewRenderer::ComputeFramingDistance(model);
    const unsigned int src = m_ThumbnailPreview.Render(model, 0.7f, 0.5f, dist, kSize, kSize);

    unsigned int dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Copy the shared preview render into `dst` via a scratch read-FBO. Predates the 4.6
    // upgrade (4.3+ has glCopyImageSubData) but glCopyTexSubImage2D from a bound READ
    // framebuffer works fine and is already loaded, so it's kept as-is.
    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevDraw);
    if (!m_ThumbnailBlitFbo) glGenFramebuffers(1, &m_ThumbnailBlitFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ThumbnailBlitFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, kSize, kSize);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int)prevDraw);

    m_ModelThumbnails[&model] = dst;
    return dst;
}

void EditorLayer::InvalidateModelThumbnail(const Model* model) {
    if (!model) { // clear all — a reimport can rebuild any model in place
        for (auto& [m, tex] : m_ModelThumbnails) { (void)m; if (tex) glDeleteTextures(1, &tex); }
        m_ModelThumbnails.clear();
        return;
    }
    auto it = m_ModelThumbnails.find(model);
    if (it == m_ModelThumbnails.end()) return;
    if (it->second) glDeleteTextures(1, &it->second);
    m_ModelThumbnails.erase(it);
}

std::string EditorLayer::RecoveryPathFor(const std::string& scenePath) {
    std::filesystem::path p(scenePath);
    p.replace_extension(); // "…/scene.json" -> "…/scene"
    return p.string() + ".recovery.json";
}

void EditorLayer::WriteRecoverySnapshot(const World& world, const AssetLibrary& assets) {
    if (m_CurrentScenePath.empty()) return; // untitled scene (New Scene) has no sidecar location
    const std::string path = RecoveryPathFor(m_CurrentScenePath);
    if (SceneSerializer::Save(world, assets, path)) {
        Log::Info("Auto-save: wrote recovery snapshot (unsaved changes are safe if the editor closes unexpectedly).");
    }
}

void EditorLayer::ClearRecoverySnapshot() {
    if (m_CurrentScenePath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(RecoveryPathFor(m_CurrentScenePath), ec); // absent file is not an error
}

bool EditorLayer::DoSaveAs(World& world, AssetLibrary& assets) {
    std::string path = FileDialog::SaveFile("Scene Files\0*.json\0All Files\0*.*\0", "json", m_Window);
    if (path.empty()) return false; // user cancelled
    if (!SceneSerializer::Save(world, assets, path)) return false;
    ClearRecoverySnapshot();       // clears the snapshot for the PREVIOUS path (still current here)
    m_CurrentScenePath = path;
    EditorSettings::Get().LastScenePath = path; // reopen this one next launch (#95)
    EditorSettings::Save();
    m_Dirty = false;
    m_SavedUndoDepth = (int)m_UndoStack.size(); // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    return true;
}

void EditorLayer::DoSave(World& world, AssetLibrary& assets) {
    if (m_CurrentScenePath.empty()) {   // untitled -> must choose a location
        DoSaveAs(world, assets);
        return;
    }
    SceneSerializer::Save(world, assets, m_CurrentScenePath);
    m_Dirty = false;
    m_SavedUndoDepth = (int)m_UndoStack.size(); // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    ClearRecoverySnapshot();            // the real file is now current — the snapshot is stale
}

void EditorLayer::DrawRecoveryPrompt(World& world, AssetLibrary& assets) {
    if (!m_RecoveryPromptPending) return;

    if (!ImGui::IsPopupOpen("Recover Unsaved Changes?")) {
        ImGui::OpenPopup("Recover Unsaved Changes?");
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Recover Unsaved Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "A recovery snapshot newer than the saved scene was found - the editor\n"
            "likely closed before these changes were saved.\n\n"
            "Restore the unsaved changes, or discard them and keep the saved scene?");
        ImGui::Separator();

        if (PrimaryButton("Restore", ImVec2(120.0f, 0.0f))) {
            const std::string recoveryPath = RecoveryPathFor(m_CurrentScenePath);
            if (SceneSerializer::Load(world, assets, recoveryPath)) {
                ClearSelection();
                m_UndoStack.clear();
                m_RedoStack.clear();
                m_Dirty = true; // recovered content isn't in the real scene file yet
                m_SavedUndoDepth = -1;
                Log::Info("Restored unsaved changes from the recovery snapshot.");
            } else {
                Log::Error("Recovery snapshot could not be read - kept the saved scene instead.");
            }
            ClearRecoverySnapshot();
            m_RecoveryPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Discard", ImVec2(120.0f, 0.0f))) {
            ClearRecoverySnapshot();
            m_RecoveryPromptPending = false;
            Log::Info("Discarded the recovery snapshot; opened the saved scene.");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::DrawExitPrompt() {
    if (!m_ExitPromptPending) return;

    if (!ImGui::IsPopupOpen("Save changes?")) ImGui::OpenPopup("Save changes?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
        if (sceneName.empty()) sceneName = "Untitled";
        ImGui::Text("\"%s\" has unsaved changes.", sceneName.c_str());
        ImGui::TextUnformatted("Save them before closing?");
        ImGui::Separator();

        if (PrimaryButton("Save", ImVec2(110.0f, 0.0f))) {
            m_ExitDecision = ExitDecision::SaveAndExit;
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f, 0.0f))) {
            m_ExitDecision = ExitDecision::DiscardAndExit;
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_ExitDecision = ExitDecision::None;
            m_ExitPromptPending = false; // main sees ExitPromptActive() == false -> stays open
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::RequestNewScene(World& world, AssetLibrary& assets) {
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::New;
        m_PendingScenePath.clear();
        m_ScenePromptPending = true;
        return;
    }
    NewScene(world, assets);
}

void EditorLayer::RequestOpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    if (path.empty()) return; // dialog cancelled, or an empty drag payload
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::Open;
        m_PendingScenePath = path;
        m_ScenePromptPending = true;
        return;
    }
    OpenScene(world, assets, path);
}

// "Save changes?" before New/Open discard the current scene (#216) — same Save / Don't Save /
// Cancel shape as DrawExitPrompt, but for switching scenes rather than closing the window, so it
// runs the stashed New/Open instead of exiting. A distinct popup ID ("##SceneSwitch") keeps it
// independent of the exit prompt even though the visible title text matches.
void EditorLayer::DrawSceneSwitchPrompt(World& world, AssetLibrary& assets) {
    if (!m_ScenePromptPending) return;

    if (!ImGui::IsPopupOpen("Save changes?##SceneSwitch")) ImGui::OpenPopup("Save changes?##SceneSwitch");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Save changes?##SceneSwitch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
        if (sceneName.empty()) sceneName = "Untitled";
        ImGui::Text("\"%s\" has unsaved changes.", sceneName.c_str());
        ImGui::TextUnformatted("Save them before continuing?");
        ImGui::Separator();

        auto runPendingSwitch = [&]() {
            if (m_PendingSceneSwitch == PendingSceneSwitch::New) NewScene(world, assets);
            else if (m_PendingSceneSwitch == PendingSceneSwitch::Open) OpenScene(world, assets, m_PendingScenePath);
            m_PendingSceneSwitch = PendingSceneSwitch::None;
            m_PendingScenePath.clear();
        };

        if (PrimaryButton("Save", ImVec2(110.0f, 0.0f))) {
            // DoSaveAs returns false if the user cancels the file dialog — in that case the
            // switch stays pending and the prompt stays open, same as a fresh Cancel would.
            bool saved = true;
            if (m_CurrentScenePath.empty()) saved = DoSaveAs(world, assets);
            else DoSave(world, assets);
            if (saved) {
                runPendingSwitch();
                m_ScenePromptPending = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f, 0.0f))) {
            runPendingSwitch();
            m_ScenePromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_PendingSceneSwitch = PendingSceneSwitch::None;
            m_PendingScenePath.clear();
            m_ScenePromptPending = false; // abort entirely — nothing happens
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::DrawPreferencesWindow(World& world) {
    if (!m_ShowPreferences) return;

    ImGui::SetNextWindowSize(ImVec2(660.0f * m_UIScale, 440.0f * m_UIScale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_GEAR "  Preferences", &m_ShowPreferences)) { ImGui::End(); return; }

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
            static const char* kThemeLabels[] = { "Dark Slate", "Prism" };
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
                                     "tint drift through the spectrum together.");
        }

        ImGui::SeparatorText("Display");
        // 0 = auto (follow the monitor). Present the slider from 0.75; a value at/below the
        // floor snaps back to Auto so there's one obvious "let the OS decide" position.
        float uiScale = prefs.UiScaleOverride <= 0.0f ? m_UIScale : prefs.UiScaleOverride;
        ImGui::SetNextItemWidth(kw);
        bool isAuto = prefs.UiScaleOverride <= 0.0f;
        if (EditorUI::SliderFloat("UI scale", &uiScale, 0.70f, 2.50f,
                               isAuto ? "Auto (%.2f)" : "%.2f")) {
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
        if (EditorUI::SliderFloat("Opacity", &prefs.LightGizmoOpacity, 0.0f, 1.0f, "%.2f")) EditorSettings::Save();
        ImGui::SetNextItemWidth(kw);
        if (EditorUI::SliderFloat("Arrow / disc scale", &prefs.LightGizmoScale, 0.25f, 3.0f, "%.2fx")) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Screen size of the parts that aren't tied to a world measurement (the directional arrow, the sun disc).");
        if (!prefs.ShowLightGizmos) ImGui::EndDisabled();

        ImGui::SeparatorText("Corner monogram");
        if (ImGui::Checkbox("Show engine mark", &prefs.EngineMarkEnabled)) EditorSettings::Save();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("The spinning TE monogram in the viewport's bottom-left corner.");
        if (!prefs.EngineMarkEnabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(kw);
        if (EditorUI::SliderFloat("Spin speed", &prefs.EngineMarkSpinSpeed, 0.0f, 4.0f, "%.2f rad/s")) EditorSettings::Save();
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
        if (EditorUI::SliderFloat("Opacity", &prefs.GridOpacity, 0.0f, 1.0f, "%.2f")) EditorSettings::Save();
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
        if (EditorUI::SliderFloat("Axis line thickness", &prefs.GridAxisThickness, 0.5f, 4.0f, "%.1f px")) EditorSettings::Save();
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

std::vector<int> EditorLayer::CaptureSelectedOrders(const World& world) const {
    std::vector<int> orders;
    auto addIfValid = [&](entt::entity e) {
        if (e != entt::null && world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e)) {
            orders.push_back(world.Registry.get<OrderComponent>(e).Value);
        }
    };
    addIfValid(m_Selected);
    for (entt::entity e : m_ExtraSelection) addIfValid(e);
    return orders;
}

void EditorLayer::RestoreSelectionByOrder(World& world, const std::vector<int>& orders) {
    ClearSelection();
    if (orders.empty()) return;

    // OrderComponent values are assigned once per entity and unique (unlike NameComponent,
    // which two "Cube"s created via Add > Cube can share) and survive the scene JSON
    // round-trip Undo/Redo does, so - unlike a raw entt::entity, invalidated by the reload -
    // they reliably re-identify the same object. Same approach OnEnterPlayMode /
    // OnExitPlayMode already use to re-find the selection across a registry rebuild (#110).
    // Restoring by name instead let duplicate names collapse every match onto one entity, so
    // a multi-selected batch edit silently applied to the same object several times (#217).
    std::unordered_map<int, entt::entity> byOrder;
    for (auto e : world.Registry.view<OrderComponent>()) {
        byOrder[world.Registry.get<OrderComponent>(e).Value] = e;
    }

    bool first = true;
    for (int wantedOrder : orders) {
        auto it = byOrder.find(wantedOrder);
        if (it == byOrder.end()) continue; // that object doesn't exist at this point in history
        if (first) { m_Selected = it->second; first = false; }
        else m_ExtraSelection.push_back(it->second);
    }
}

void EditorLayer::PushUndo(const World& world, const std::string& label) {
    UndoEntry entry;
    entry.SceneJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                   : SceneSerializer::SaveToString(world);
    // Plenty of call sites fire on "field focused" / "gizmo grabbed" before anything actually
    // changes — a double-click-to-type on a Transform field lands here twice with no edit
    // between. Don't stack a byte-identical snapshot on the last one: it produced phantom
    // History entries and left extra Ctrl+Z presses that did nothing (#19 P8, #23 P23).
    if (!m_UndoStack.empty() && m_UndoStack.back().SceneJson == entry.SceneJson) {
        m_RedoStack.clear(); // still a fresh edit intent — a stale redo branch shouldn't survive it
        RefreshDirtyFromHistory();
        return;
    }
    entry.SelectedOrders = CaptureSelectedOrders(world);
    entry.Label = label;
    // Branching off a mid-history position discards the redo entries — the saved state may be
    // among them, in which case there's no longer a clean point to return to (#22 P22).
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    m_UndoStack.push_back(std::move(entry));
    if (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.erase(m_UndoStack.begin());
        if (m_SavedUndoDepth > 0) m_SavedUndoDepth--; // the whole stack shifted down by one
    }
    m_RedoStack.clear(); // a fresh edit invalidates whatever redo history existed
    RefreshDirtyFromHistory();
}

void EditorLayer::StageUndo(const World& world) {
    if (m_HasStagedUndo) return; // keep the FIRST (true pre-edit) snapshot of this interaction
    m_StagedUndoJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                   : SceneSerializer::SaveToString(world);
    m_StagedUndoSelectedOrders = CaptureSelectedOrders(world);
    m_HasStagedUndo = true;
}

void EditorLayer::CommitStagedUndo(const World& world, const std::string& label) {
    if (!m_HasStagedUndo) return;
    m_HasStagedUndo = false;

    // If the interaction ended on the same state it began — a no-op drag, a value typed back to
    // what it was, or an input the commit path rejected (non-finite) — record nothing, so it
    // neither adds a phantom History entry nor dirties the scene (#22 P22, #23 P23, #34 D4).
    const std::string currentJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                                : SceneSerializer::SaveToString(world);
    if (currentJson == m_StagedUndoJson) {
        m_StagedUndoJson.clear();
        m_StagedUndoSelectedOrders.clear();
        return;
    }

    UndoEntry entry;
    entry.SceneJson = std::move(m_StagedUndoJson);
    entry.SelectedOrders = std::move(m_StagedUndoSelectedOrders);
    entry.Label = label;
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    m_UndoStack.push_back(std::move(entry));
    if (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.erase(m_UndoStack.begin());
        if (m_SavedUndoDepth > 0) m_SavedUndoDepth--;
    }
    m_RedoStack.clear();
    RefreshDirtyFromHistory();
    m_HasStagedUndo = false;
}

void EditorLayer::Undo(World& world, AssetLibrary& assets) {
    if (m_UndoStack.empty()) return;

    UndoEntry redoEntry;
    redoEntry.SceneJson = SceneSerializer::SaveToString(world, assets);
    redoEntry.SelectedOrders = CaptureSelectedOrders(world);
    redoEntry.Label = m_UndoStack.back().Label; // the action Redo would re-apply from here
    m_RedoStack.push_back(std::move(redoEntry));

    UndoEntry entry = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, entry.SceneJson);
    RestoreSelectionByOrder(world, entry.SelectedOrders);
    InvalidateModelThumbnail(nullptr); // LoadFromString may rebuild the asset library
    RefreshDirtyFromHistory(); // "*" clears when history returns to the last-saved point (#22 P22)
}

void EditorLayer::Redo(World& world, AssetLibrary& assets) {
    if (m_RedoStack.empty()) return;

    UndoEntry undoEntry;
    undoEntry.SceneJson = SceneSerializer::SaveToString(world, assets);
    undoEntry.SelectedOrders = CaptureSelectedOrders(world);
    undoEntry.Label = m_RedoStack.back().Label;
    m_UndoStack.push_back(std::move(undoEntry));

    UndoEntry entry = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, entry.SceneJson);
    RestoreSelectionByOrder(world, entry.SelectedOrders);
    InvalidateModelThumbnail(nullptr); // LoadFromString may rebuild the asset library
    RefreshDirtyFromHistory(); // "*" clears when history returns to the last-saved point (#22 P22)
}

void EditorLayer::JumpToUndoEntry(World& world, AssetLibrary& assets, size_t undoStackIndex) {
    if (undoStackIndex >= m_UndoStack.size()) return;
    while (m_UndoStack.size() > undoStackIndex) {
        Undo(world, assets);
    }
}

void EditorLayer::JumpToRedoEntry(World& world, AssetLibrary& assets, size_t redoStackIndex) {
    if (redoStackIndex >= m_RedoStack.size()) return;
    size_t steps = m_RedoStack.size() - redoStackIndex;
    for (size_t i = 0; i < steps; ++i) Redo(world, assets);
}

void EditorLayer::OnEnterPlayMode(const World& world) {
    m_PlayModeSnapshot = SceneSerializer::SaveToString(world);
    // Remember what's selected by OrderComponent value, not entt id: Stop rebuilds the whole
    // registry and entt recycles ids, so a retained handle can pass valid() yet denote a
    // different object afterward (#110).
    m_PlaySelectionOrders.clear();
    for (entt::entity e : GetSelectedItems()) {
        if (const auto* o = world.Registry.try_get<OrderComponent>(e))
            m_PlaySelectionOrders.push_back(o->Value);
    }
    Log::Info("Entered play mode - scene state saved, changes will be reverted on exit.");
}

void EditorLayer::OnExitPlayMode(World& world, AssetLibrary& assets) {
    if (m_PlayModeSnapshot.empty()) return;
    SceneSerializer::LoadFromString(world, assets, m_PlayModeSnapshot);
    m_PlayModeSnapshot.clear();

    // Drop every retained handle before it can rebind to a recycled id (#110).
    ClearSelection();
    m_HierarchyVisibleOrder.clear();
    m_HierarchyVisibleBuild.clear();

    // Re-resolve the pre-Play selection against the freshly rebuilt entities by OrderComponent.
    if (!m_PlaySelectionOrders.empty()) {
        std::unordered_map<int, entt::entity> byOrder;
        for (entt::entity e : world.Registry.view<OrderComponent>())
            byOrder[world.Registry.get<OrderComponent>(e).Value] = e;
        bool first = true;
        for (int ord : m_PlaySelectionOrders) {
            auto it = byOrder.find(ord);
            if (it == byOrder.end() || !world.Registry.valid(it->second)) continue;
            SelectItem(it->second, /*addToSelection=*/!first);
            first = false;
        }
        m_PlaySelectionOrders.clear();
    }
    // Deliberately does NOT set m_Dirty: the scene is back exactly as it was before Play, so
    // there's nothing new to save — the same reason Unity doesn't dirty a scene on play/stop.
    Log::Info("Exited play mode - scene state restored.");
}

void EditorLayer::CopySelection(World& world) {
    auto selection = GetSelectedItems();
    if (selection.empty()) return;
    m_Clipboard = SceneSerializer::SaveEntitiesToString(world, selection);
    Log::Info("Copied " + std::to_string(selection.size()) +
        (selection.size() == 1 ? " object." : " objects."));
}

void EditorLayer::PasteClipboard(World& world, AssetLibrary& assets) {
    if (m_Clipboard.empty()) return;
    PushUndo(world, "Paste");
    std::vector<entt::entity> pasted;
    if (!SceneSerializer::AppendEntitiesFromString(world, assets, m_Clipboard, pasted) || pasted.empty()) {
        Log::Warn("Paste failed: clipboard content could not be rebuilt.");
        return;
    }
    // Offset so a paste is visibly distinct from the original instead of landing exactly on top
    // of it — matching what Duplicate already does.
    for (entt::entity e : pasted) {
        if (auto* transform = world.Registry.try_get<TransformComponent>(e)) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
            bool isRoot = !hier || hier->Parent == entt::null;
            if (isRoot) transform->Position += glm::vec3(1.0f, 0.0f, 1.0f);
        }
    }
    ClearSelection();
    for (entt::entity e : pasted) AddToSelectionIfAbsent(e);
    if (m_Selected == entt::null && !pasted.empty()) SelectItem(pasted.front(), false);
}

void EditorLayer::ClearSelection() {
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_SelectionAnchor = entt::null;
    m_RenamingEntity = entt::null;
    m_LightHandleArmedFor = entt::null;
}

void EditorLayer::SelectItem(entt::entity entity, bool addToSelection) {
    // Any selection that isn't a viewport icon-click disarms the light grab handles;
    // HandleViewportPicking re-arms them right after it calls this for a light it picked.
    m_LightHandleArmedFor = entt::null;
    bool isPrimary = m_Selected == entity;

    if (!addToSelection) {
        m_ExtraSelection.clear();
        m_Selected = entity;
        m_SelectionAnchor = entity; // plain click (re)anchors range selection here
        if (m_FrameOnSelect && entity != entt::null) m_PendingFrameSelect = true;
        return;
    }

    // Any Ctrl+Click also moves the range anchor to the clicked row, matching Unity.
    m_SelectionAnchor = entity;

    if (isPrimary) {
        // Demote: promote the most recently added extra to primary, or clear if none left.
        if (!m_ExtraSelection.empty()) {
            m_Selected = m_ExtraSelection.back();
            m_ExtraSelection.pop_back();
        } else {
            m_Selected = entt::null;
        }
        return;
    }

    for (auto it = m_ExtraSelection.begin(); it != m_ExtraSelection.end(); ++it) {
        if (*it == entity) {
            m_ExtraSelection.erase(it); // already co-selected — toggle it back off
            return;
        }
    }

    if (!HasAnySelection()) {
        m_Selected = entity;
    } else {
        m_ExtraSelection.push_back(entity);
    }
}

void EditorLayer::AddToSelectionIfAbsent(entt::entity entity) {
    if (IsSelected(entity)) return; // leave already-selected items alone — don't toggle them off
    if (!HasAnySelection()) {
        m_Selected = entity;
    } else {
        m_ExtraSelection.push_back(entity);
    }
}

void EditorLayer::SelectAllVisibleInHierarchy() {
    if (m_HierarchyVisibleOrder.empty()) return;
    m_Selected = entt::null;
    m_ExtraSelection.clear();
    m_RenamingEntity = entt::null;
    for (entt::entity e : m_HierarchyVisibleOrder) AddToSelectionIfAbsent(e);
    m_SelectionAnchor = m_HierarchyVisibleOrder.front();
    // Deliberately no m_PendingFrameSelect here — snapping the camera to fit the entire scene
    // every time the user hits Ctrl+A would be more disruptive than helpful.
}

void EditorLayer::SelectHierarchyRange(World& world, entt::entity target, bool additive) {
    // No usable anchor (first click was Shift, or the anchor row was deleted / scrolled out of
    // the visible set): fall back to a plain pick so Shift+Click is never a dead input.
    const auto& order = m_HierarchyVisibleOrder;
    auto indexOf = [&](entt::entity e) -> int {
        for (int i = 0; i < (int)order.size(); ++i) if (order[i] == e) return i;
        return -1;
    };
    int ai = (m_SelectionAnchor != entt::null && world.Registry.valid(m_SelectionAnchor))
                 ? indexOf(m_SelectionAnchor) : -1;
    int ti = indexOf(target);
    if (ai < 0 || ti < 0) {
        SelectItem(target, additive);
        return;
    }
    int lo = ai < ti ? ai : ti;
    int hi = ai < ti ? ti : ai;

    std::vector<entt::entity> keep;
    if (additive) { // Ctrl+Shift: preserve whatever was already selected, then union the range in
        keep = m_ExtraSelection;
        if (m_Selected != entt::null) keep.push_back(m_Selected);
    }

    m_ExtraSelection.clear();
    m_Selected = target;              // the row just clicked is the active object
    m_RenamingEntity = entt::null;
    auto addUnique = [&](entt::entity e) {
        if (e == m_Selected) return;
        if (std::find(m_ExtraSelection.begin(), m_ExtraSelection.end(), e) == m_ExtraSelection.end())
            m_ExtraSelection.push_back(e);
    };
    for (int i = lo; i <= hi; ++i) addUnique(order[i]);
    for (entt::entity e : keep) if (world.Registry.valid(e)) addUnique(e);
    // m_SelectionAnchor intentionally left untouched — Unity keeps it fixed so the next
    // Shift+Click can grow or shrink the same range.
}

void EditorLayer::DeleteSelection(World& world) {
    PushUndo(world, "Delete");

    // entt::entity handles don't shift when another entity is destroyed (unlike the vector
    // indices this replaced), so unlike before there's no careful ordering needed here at all —
    // and no more separate "boxes soft-delete, models hard-erase" split, either.
    // Destroys children recursively too, so parenting one entity under another means deleting
    // the parent doesn't leave the child pointing at a dead entt::entity.
    int count = (m_Selected != entt::null ? 1 : 0) + (int)m_ExtraSelection.size();
    if (m_Selected != entt::null) world.DestroyEntityAndChildren(m_Selected);
    for (entt::entity e : m_ExtraSelection) {
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    }

    ClearSelection();
    Log::Info("Deleted " + std::to_string(count) + (count == 1 ? " object." : " objects."));
}

namespace {
// Unity-style duplicate naming: "Cube" -> "Cube (1)" -> "Cube (2)". Re-derives the base name
// from an already-numbered source first, so duplicating a duplicate produces "Cube (2)" instead
// of chaining into "Cube (1) (1)".
std::string NextDuplicateName(const World& world, const std::string& sourceName) {
    std::string base = sourceName;
    size_t open = base.find_last_of('(');
    if (open != std::string::npos && !base.empty() && base.back() == ')') {
        std::string inside = base.substr(open + 1, base.size() - open - 2);
        if (!inside.empty() && inside.find_first_not_of("0123456789") == std::string::npos) {
            base = base.substr(0, open);
            while (!base.empty() && base.back() == ' ') base.pop_back();
        }
    }
    if (base.empty()) base = "Object";

    std::set<std::string> existingNames;
    for (auto e : world.Registry.view<NameComponent>()) existingNames.insert(world.Registry.get<NameComponent>(e).Name);

    int n = 1;
    std::string candidate;
    do {
        candidate = base + " (" + std::to_string(n) + ")";
        n++;
    } while (existingNames.count(candidate));
    return candidate;
}

// Returns `desired` unchanged if nothing in the scene already holds that name, else the next
// free "desired (n)" via NextDuplicateName. Unlike calling NextDuplicateName directly (which
// always appends " (1)" or higher), this lets the FIRST object of a kind keep the plain name -
// e.g. the first Add > Cube is "Cube", only the second becomes "Cube (1)" - while still
// guaranteeing every creation path (Add menu, viewport drop, prefab instantiate, empty/light/
// camera spawn) hands out a name nothing else in the scene already has. Undo's selection
// restore stopped depending on unique names (#217, now keyed by OrderComponent instead), but
// the Inspector's per-entity collapse-state key and the Hierarchy search/tag filters still read
// more naturally when names actually are unique.
std::string UniqueNameFor(const World& world, const std::string& desired) {
    for (auto e : world.Registry.view<NameComponent>()) {
        if (world.Registry.get<NameComponent>(e).Name == desired) return NextDuplicateName(world, desired);
    }
    return desired;
}

// Same idea as UniqueNameFor, but for an entity that already carries the name to check (e.g. a
// freshly-instantiated prefab, authored with whatever name it was saved under) rather than a
// name string not yet assigned to anyone. Clears the entity's own name first so it doesn't
// collide with itself in the scan - otherwise the first instance of every prefab would get
// needlessly renamed "X (1)" on its very first placement.
void UniquifyName(World& world, entt::entity e) {
    auto* nc = world.Registry.try_get<NameComponent>(e);
    if (!nc) return;
    std::string desired = nc->Name;
    nc->Name.clear();
    nc->Name = UniqueNameFor(world, desired);
}
}

void EditorLayer::DuplicateSelection(World& world, AssetLibrary& assets) {
    if (!HasAnySelection()) return;
    PushUndo(world, "Duplicate");

    std::vector<entt::entity> source;
    if (m_Selected != entt::null) source.push_back(m_Selected);
    for (entt::entity e : m_ExtraSelection) source.push_back(e);

    std::vector<entt::entity> created;

    for (entt::entity srcEntity : source) {
        if (!world.Registry.valid(srcEntity)) continue;
        const auto& transform = world.Registry.get<TransformComponent>(srcEntity);
        const auto& name = world.Registry.get<NameComponent>(srcEntity);
        // Nudge the copy off the original by the same (1,0,1) offset Paste uses, so a duplicate
        // is visibly distinct in the viewport (not just by its unique Hierarchy name) and the two
        // paths behave consistently. A multi-selection duplicate shifts every copy by the same
        // amount, preserving the group's internal layout.
        glm::vec3 newPos = transform.Position + glm::vec3(1.0f, 0.0f, 1.0f);

        // Mesh-less entity (a light or empty) — nothing to clone via AssetLibrary, so it gets
        // its own branch instead of falling into the box/model paths below, both of which
        // unconditionally read RenderableComponent (would be undefined behavior — a crash —
        // on an entity that doesn't have one).
        if (!world.Registry.all_of<RenderableComponent>(srcEntity)) {
            std::string newName = NextDuplicateName(world, name.Name.empty() ? "Object" : name.Name);
            entt::entity newEntity = world.CreateEmptyEntity(newPos, transform.RotationEuler, transform.Scale, newName);
            if (const auto* light = world.Registry.try_get<LightComponent>(srcEntity)) {
                world.Registry.emplace<LightComponent>(newEntity, *light);
            }
            if (const auto* tag = world.Registry.try_get<TagComponent>(srcEntity)) {
                world.Registry.emplace<TagComponent>(newEntity, *tag);
            }
            if (world.Registry.all_of<StaticTag>(srcEntity)) world.Registry.emplace<StaticTag>(newEntity);
            if (world.Registry.all_of<InactiveTag>(srcEntity)) world.Registry.emplace<InactiveTag>(newEntity);
            created.push_back(newEntity);
            continue;
        }

        const auto& renderable = world.Registry.get<RenderableComponent>(srcEntity);

        if (world.Registry.all_of<LevelGeometryTag>(srcEntity)) {
            std::string baseName = NextDuplicateName(world, name.Name.empty() ? "Box" : name.Name);
            glm::vec3 color = renderable.ModelRef->MeshMaterial(0).BaseColor;
            created.push_back(world.CreateBox(newPos, transform.Scale, color, transform.RotationEuler, baseName));
        } else {
            // Its own Model instance (own material-override/animation state), not the same
            // shared_ptr as the original — otherwise recoloring one copy would recolor every
            // duplicate made from it, since Model (not the entity) owns the material override.
            auto clonedModel = assets.CloneModel(renderable.ModelRef);
            if (auto srcMat = renderable.ModelRef->MaterialOverride()) {
                clonedModel->SetMaterialOverride(std::make_shared<Material>(*srcMat));
            }
            std::string newName = NextDuplicateName(world, name.Name.empty() ? "Model" : name.Name);
            entt::entity newEntity = world.CreateModelEntity(clonedModel, newPos, transform.RotationEuler, transform.Scale, newName);
            if (const auto* audio = world.Registry.try_get<AudioSourceComponent>(srcEntity)) {
                world.Registry.emplace<AudioSourceComponent>(newEntity, audio->SoundPath);
            }
            created.push_back(newEntity);
        }
    }

    // Select the new duplicates instead of the originals, so you can immediately drag them
    // into place without having to re-pick them from the Hierarchy.
    ClearSelection();
    for (size_t i = 0; i < created.size(); ++i) {
        if (i == 0) m_Selected = created[i];
        else m_ExtraSelection.push_back(created[i]);
    }
    Log::Info("Duplicated " + std::to_string(created.size()) + (created.size() == 1 ? " object." : " objects."));
}

bool EditorLayer::ComputeSelectionBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const {
    if (!HasAnySelection()) return false;

    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);
    bool any = false;
    // Every placeable-in-the-world entity used to have a real Renderable Model (former-boxes
    // included, via the cube primitive) — that stopped being true once lights and empties were
    // added, so this can no longer assume RenderableComponent exists (it used to call the
    // unchecked get<>(), which is undefined behavior — a crash in practice — the instant a
    // light/empty was selected, since this runs every frame via GetSelectionCenter). A
    // mesh-less entity contributes a small nominal box around its own origin instead, matching
    // the fallback DrawGizmo already uses for the same case, so focus/orbit still center on it
    // sensibly rather than crashing or silently contributing nothing.
    auto expand = [&](entt::entity entity) {
        if (!world.Registry.valid(entity)) return;
        glm::mat4 m = world.ComposeWorldTransform(entity);
        glm::vec3 localMin(-0.5f), localMax(0.5f);
        if (const auto* renderable = world.Registry.try_get<RenderableComponent>(entity);
            renderable && renderable->ModelRef && renderable->ModelRef->MeshCount() > 0) {
            localMin = renderable->ModelRef->BoundsMin();
            localMax = renderable->ModelRef->BoundsMax();
        }
        AABB bounds = AABB{localMin, localMax}.Transformed(m);
        boundsMin = glm::min(boundsMin, bounds.Min);
        boundsMax = glm::max(boundsMax, bounds.Max);
        any = true;
    };
    expand(m_Selected);
    for (entt::entity e : m_ExtraSelection) expand(e);
    if (!any) return false;

    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

bool EditorLayer::GetSelectionCenter(World& world, glm::vec3& outCenter) const {
    glm::vec3 boundsMin, boundsMax;
    if (!ComputeSelectionBounds(world, boundsMin, boundsMax)) return false;
    outCenter = (boundsMin + boundsMax) * 0.5f;
    return true;
}

void EditorLayer::FrameSceneBounds(World& world, Camera& editorCamera) {
    glm::vec3 mn, mx;
    if (!ComputeSceneBounds(world, mn, mx)) return;
    glm::vec3 center = (mn + mx) * 0.5f;
    float radius = std::max(glm::length(mx - mn) * 0.5f, 0.5f);
    float distance = (radius / std::sin(glm::radians(editorCamera.Fov) * 0.5f)) * 1.35f;
    glm::vec3 target = center - editorCamera.Front() * distance;
    // Last line of defence: a bounds value that still went non-finite (huge scene, overflow)
    // must not strand the camera at inf/NaN — leave it where it is instead.
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return;
    editorCamera.Position = target;
}

void EditorLayer::FocusOnSelection(World& world, Camera& editorCamera) {
    glm::vec3 boundsMin, boundsMax;
    if (!ComputeSelectionBounds(world, boundsMin, boundsMax)) return;

    glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = glm::length(boundsMax - boundsMin) * 0.5f;
    radius = std::max(radius, 0.5f); // guard against a degenerate/zero-size bounds parking the camera inside it

    // Keep the camera's current aim, just slide it back along that same ray until the
    // selection's bounding sphere fits inside the vertical field of view, with a margin.
    float halfFov = glm::radians(editorCamera.Fov) * 0.5f;
    float distance = (radius / std::sin(halfFov)) * 1.35f;
    glm::vec3 targetPos = center - editorCamera.Front() * distance;
    if (!std::isfinite(targetPos.x) || !std::isfinite(targetPos.y) || !std::isfinite(targetPos.z)) return;

    // Glide there rather than teleport — same eased transition SnapToView uses (audit follow-up).
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
    m_ViewTransition.ToPos = targetPos;
    m_ViewTransition.ToYaw = editorCamera.Yaw;     // aim unchanged
    m_ViewTransition.ToPitch = editorCamera.Pitch;
    m_ViewTransition.ToOrthoHalfHeight = editorCamera.Orthographic
        ? (distance * std::tan(halfFov)) : editorCamera.OrthoHalfHeight;
}

bool EditorLayer::CanSnapSelectionToGround(World& world) const {
    return m_Selected != entt::null && world.Registry.valid(m_Selected) &&
        world.Registry.all_of<RenderableComponent>(m_Selected);
}

void EditorLayer::SnapSelectionToGround(World& world) {
    if (!CanSnapSelectionToGround(world)) return;
    PushUndo(world, "Snap to Ground");

    auto& transform = world.Registry.get<TransformComponent>(m_Selected);
    auto& renderable = world.Registry.get<RenderableComponent>(m_Selected);
    glm::mat4 m = ComposeTransform(transform);
    if (world.Registry.all_of<LevelGeometryTag>(m_Selected)) {
        AABB worldBounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(m);
        transform.Position.y -= worldBounds.Min.y;
    } else {
        transform.Position.y -= renderable.ModelRef->LowestVertexWorldY(m);
    }
}

bool EditorLayer::ComputeSceneBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const {
    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);
    bool any = false;
    for (auto entity : world.Registry.view<TransformComponent, RenderableComponent>()) {
        auto& renderable = world.Registry.get<RenderableComponent>(entity);
        // A model that failed to import contributes no geometry and carries a degenerate
        // (inverted-sentinel) bounds — folding it in poisons the whole scene AABB with
        // ±1e30, which then overflows to inf when FrameSceneBounds takes its length. Skip it.
        if (!renderable.ModelRef || renderable.ModelRef->MeshCount() == 0) continue;
        glm::mat4 m = world.ComposeWorldTransform(entity);
        AABB bounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(m);
        boundsMin = glm::min(boundsMin, bounds.Min);
        boundsMax = glm::max(boundsMax, bounds.Max);
        any = true;
    }
    if (!any) return false;
    outMin = boundsMin;
    outMax = boundsMax;
    return true;
}

void EditorLayer::ComputeViewPivot(World& world, Camera& editorCamera, glm::vec3& outPivot,
                                   float& outFrameRadius) const {
    outFrameRadius = 0.0f;

    if (GetSelectionCenter(world, outPivot)) return;

    float t;
    if (world.Raycast(editorCamera.Position, editorCamera.Front(), 1000.0f, t) != entt::null) {
        outPivot = editorCamera.Position + editorCamera.Front() * t;
        return;
    }

    // Nothing selected and not pointed at anything — frame the whole scene rather than orbiting
    // a fixed point 15 units ahead (which, if the camera had drifted off, left the viewport
    // black with no recovery).
    glm::vec3 sceneMin, sceneMax;
    if (ComputeSceneBounds(world, sceneMin, sceneMax)) {
        outPivot = (sceneMin + sceneMax) * 0.5f;
        outFrameRadius = std::max(glm::length(sceneMax - sceneMin) * 0.5f, 0.5f);
        return;
    }

    const float kDefaultFocusDistance = 15.0f;
    outPivot = editorCamera.Position + editorCamera.Front() * kDefaultFocusDistance;
}

void EditorLayer::SnapToView(World& world, Camera& editorCamera, float yaw, float pitch, bool orthographic) {
    glm::vec3 pivot;
    float frameRadius = 0.0f;
    ComputeViewPivot(world, editorCamera, pivot, frameRadius);

    // A "distance to pivot" that means the same thing regardless of the CURRENT projection mode.
    // In orthographic mode the camera's literal position is decoupled from zoom level (scroll
    // only changes OrthoHalfHeight, never Position — see UpdateEditorCamera in main.cpp), so
    // computing distance from Position here would use a stale, arbitrary number; derive the
    // perspective-equivalent distance from the current ortho size instead.
    float halfFovTan = tan(glm::radians(editorCamera.Fov) * 0.5f);
    float distance;
    if (frameRadius > 0.0f) {
        // Whole-scene fallback (no selection, not pointed at anything): don't keep the old
        // distance — it could be anywhere. Pull back far enough to fit the scene's bounding
        // sphere in view, same formula FocusOnSelection uses.
        distance = (frameRadius / std::sin(glm::radians(editorCamera.Fov) * 0.5f)) * 1.35f;
    } else {
        distance = editorCamera.Orthographic
            ? editorCamera.OrthoHalfHeight / halfFovTan
            : glm::length(editorCamera.Position - pivot);
    }
    distance = std::max(distance, 0.5f);

    glm::vec3 newFront;
    newFront.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront.y = sin(glm::radians(pitch));
    newFront.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    newFront = glm::normalize(newFront);

    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = m_ViewTransition.ToFov = editorCamera.Fov;
    m_ViewTransition.ToYaw = yaw;
    m_ViewTransition.ToPitch = pitch;
    m_ViewTransition.ToPos = pivot - newFront * distance;
    // Keep apparent scale continuous across a projection-mode switch instead of an arbitrary
    // jump in how big everything suddenly looks.
    m_ViewTransition.ToOrthoHalfHeight = orthographic ? (distance * halfFovTan) : editorCamera.OrthoHalfHeight;

    editorCamera.Orthographic = orthographic; // switches immediately; only angle/position/size animate
}

void EditorLayer::ToggleOrthographic(World& world, Camera& editorCamera) {
    // Same angle, just flips projection — SnapToView still handles the position/scale
    // conversion so perspective<->orthographic round-trips don't drift.
    SnapToView(world, editorCamera, editorCamera.Yaw, editorCamera.Pitch, !editorCamera.Orthographic);
}

void EditorLayer::UpdateViewTransition(Camera& editorCamera, float dt) {
    if (!m_ViewTransition.Active) return;

    // Any manual camera input hands control back to the user immediately instead of fighting
    // the animation to completion — the same feel as canceling the nav gizmo's own axis-snap
    // animation by moving the mouse mid-flight.
    ImGuiIO& io = ImGui::GetIO();
    bool manualInput = io.MouseWheel != 0.0f ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
        (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
        (!io.WantCaptureKeyboard && (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A) ||
            ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D) ||
            ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_E)));
    if (manualInput) {
        m_ViewTransition.Active = false;
        return;
    }

    const float kDuration = 0.28f;
    m_ViewTransition.T = std::min(1.0f, m_ViewTransition.T + dt / kDuration);
    float t = m_ViewTransition.T;
    float eased = t * t * (3.0f - 2.0f * t); // smoothstep

    auto lerpAngle = [](float from, float to, float f) {
        float delta = fmodf(to - from + 540.0f, 360.0f) - 180.0f; // shortest path, wrapped to [-180,180)
        return from + delta * f;
    };

    editorCamera.Yaw = lerpAngle(m_ViewTransition.FromYaw, m_ViewTransition.ToYaw, eased);
    editorCamera.Pitch = m_ViewTransition.FromPitch + (m_ViewTransition.ToPitch - m_ViewTransition.FromPitch) * eased;
    editorCamera.Position = glm::mix(m_ViewTransition.FromPos, m_ViewTransition.ToPos, eased);
    editorCamera.OrthoHalfHeight = m_ViewTransition.FromOrthoHalfHeight +
        (m_ViewTransition.ToOrthoHalfHeight - m_ViewTransition.FromOrthoHalfHeight) * eased;
    editorCamera.Fov = m_ViewTransition.FromFov +
        (m_ViewTransition.ToFov - m_ViewTransition.FromFov) * eased;

    if (m_ViewTransition.T >= 1.0f) m_ViewTransition.Active = false;
}

// --- Look through light (#140 phase 4) -------------------------------------------------------
void EditorLayer::LookThroughLight(World& world, Camera& editorCamera, entt::entity light) {
    if (light == entt::null || !world.Registry.valid(light) ||
        !world.Registry.all_of<TransformComponent, LightComponent>(light))
        return;

    // Stash the camera only on the FIRST entry — hopping between lights keeps the original.
    if (!m_LookThroughActive) {
        m_LookThroughRestore = {editorCamera.Position, editorCamera.Yaw, editorCamera.Pitch,
                                editorCamera.Fov, editorCamera.OrthoHalfHeight, editorCamera.Orthographic};
    }
    // Remember this light's pose so the first frame the flown camera diverges from it counts as
    // a real reposition and gets its own undo entry. Re-armed per light when hopping.
    {
        const auto& tf0 = world.Registry.get<const TransformComponent>(light);
        m_LookThroughStartPos = tf0.Position;
        m_LookThroughStartRot = tf0.RotationEuler;
        m_LookThroughMoved = false;
    }

    const auto& lc = world.Registry.get<const LightComponent>(light);
    glm::mat4 m = world.ComposeWorldTransform(light);
    glm::vec3 pos = glm::vec3(m[3]);
    glm::vec3 aim = glm::normalize(glm::vec3(m * glm::vec4(0, 0, -1, 0)));

    float yaw = glm::degrees(std::atan2(aim.z, aim.x));
    float pitch = glm::degrees(std::asin(std::clamp(aim.y, -1.0f, 1.0f)));

    bool ortho = lc.Kind == LightComponent::Type::Directional;
    float fov = lc.Kind == LightComponent::Type::Spot
                    ? std::clamp(lc.SpotAngleDegrees * 2.0f, 10.0f, 150.0f)
                : lc.Kind == LightComponent::Type::Point ? 90.0f
                : editorCamera.Fov;

    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = editorCamera.Fov;
    m_ViewTransition.ToPos = pos;
    m_ViewTransition.ToYaw = yaw;
    m_ViewTransition.ToPitch = pitch;
    m_ViewTransition.ToFov = fov;
    // Perspective near a light POV is fine as-is; for a directional, size the ortho box to its
    // range if it's a spot/point (n/a) — just pick a readable default for the sun.
    m_ViewTransition.ToOrthoHalfHeight = ortho ? 12.0f : editorCamera.OrthoHalfHeight;
    editorCamera.Orthographic = ortho; // matches SnapToView: switches now, the rest animates

    m_LookThroughActive = true;
    m_LookThroughLight = light;
    SelectItem(light, false);
}

void EditorLayer::ExitLookThrough(Camera& editorCamera) {
    if (!m_LookThroughActive) return;
    const CameraPose& r = m_LookThroughRestore;
    m_ViewTransition.Active = true;
    m_ViewTransition.T = 0.0f;
    m_ViewTransition.FromPos = editorCamera.Position;
    m_ViewTransition.FromYaw = editorCamera.Yaw;
    m_ViewTransition.FromPitch = editorCamera.Pitch;
    m_ViewTransition.FromOrthoHalfHeight = editorCamera.OrthoHalfHeight;
    m_ViewTransition.FromFov = editorCamera.Fov;
    m_ViewTransition.ToPos = r.Pos;
    m_ViewTransition.ToYaw = r.Yaw;
    m_ViewTransition.ToPitch = r.Pitch;
    m_ViewTransition.ToFov = r.Fov;
    m_ViewTransition.ToOrthoHalfHeight = r.OrthoHalfHeight;
    editorCamera.Orthographic = r.Ortho;
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
}

void EditorLayer::UpdateLookThrough(World& world, Camera& editorCamera) {
    if (!m_LookThroughActive) return;

    // Light deleted / scene swapped out from under us — just bail (camera stays where it is).
    if (m_LookThroughLight == entt::null || !world.Registry.valid(m_LookThroughLight) ||
        !world.Registry.all_of<LightComponent>(m_LookThroughLight)) {
        m_LookThroughActive = false;
        m_LookThroughLight = entt::null;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ExitLookThrough(editorCamera);
        return;
    }

    // Live-drive: once the entry glide has settled (or the user cancelled it by flying), the
    // light rides with the editor camera — WASD/look moves and re-aims it from its own POV.
    // main.cpp has already applied this frame's camera nav by the time Draw() calls us.
    if (!m_ViewTransition.Active && world.Registry.all_of<TransformComponent>(m_LookThroughLight)) {
        auto& tf = world.Registry.get<TransformComponent>(m_LookThroughLight);

        entt::entity parent = entt::null;
        if (auto* h = world.Registry.try_get<HierarchyComponent>(m_LookThroughLight)) parent = h->Parent;
        glm::mat4 invParent = parent != entt::null
            ? glm::inverse(world.ComposeWorldTransform(parent)) : glm::mat4(1.0f);

        glm::vec3 newPos = glm::vec3(invParent * glm::vec4(editorCamera.Position, 1.0f));
        glm::vec3 fwd = editorCamera.Front();
        glm::vec3 up = std::fabs(fwd.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        glm::vec3 newRot = EulerYXZFromMatrix(invParent *
            glm::inverse(glm::lookAt(glm::vec3(0.0f), fwd, up)));

        bool moved = glm::length(newPos - tf.Position) > 1e-4f ||
                     glm::length(newRot - tf.RotationEuler) > 1e-3f;
        if (moved) {
            if (!m_LookThroughMoved) {           // first real move -> one undo point, light still at entry pose
                PushUndo(world, "Reposition Light (Look Through)");
                m_LookThroughMoved = true;
            }
            tf.Position = newPos;
            tf.RotationEuler = newRot;
        }
    }

    // Banner across the top of the viewport.
    const char* nm = "light";
    if (const auto* n = world.Registry.try_get<const NameComponent>(m_LookThroughLight))
        if (!n->Name.empty()) nm = n->Name.c_str();
    char buf[160];
    snprintf(buf, sizeof(buf),
             ICON_FA_EYE "  Looking through \"%s\"  —  fly to move / re-aim it  ·  Esc to exit", nm);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    float padX = 14.0f * m_UIScale, padY = 6.0f * m_UIScale;
    ImVec2 c(m_ViewportPos.x + m_ViewportSize.x * 0.5f, m_ViewportPos.y + 14.0f * m_UIScale);
    ImVec2 bmin(c.x - ts.x * 0.5f - padX, c.y - padY);
    ImVec2 bmax(c.x + ts.x * 0.5f + padX, c.y + ts.y + padY);
    dl->AddRectFilled(bmin, bmax, IM_COL32(20, 22, 28, 225), 5.0f);
    dl->AddRect(bmin, bmax, IM_COL32(255, 210, 90, 220), 5.0f);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y), IM_COL32(240, 240, 245, 255), buf);
}

// --- Drop light to surface (#140 phase 4) --------------------------------------------------
bool EditorLayer::DropLightToSurface(World& world, entt::entity light) {
    if (light == entt::null || !world.Registry.valid(light) ||
        !world.Registry.all_of<TransformComponent, LightComponent>(light))
        return false;

    glm::vec3 origin = glm::vec3(world.ComposeWorldTransform(light)[3]);
    const glm::vec3 down(0.0f, -1.0f, 0.0f);

    float bestT = 1e30f;
    bool hit = false;
    for (auto ent : world.Registry.view<const RenderableComponent>()) {
        if (ent == light) continue;
        const auto& r = world.Registry.get<const RenderableComponent>(ent);
        if (!r.ModelRef) continue;
        glm::mat4 model = world.ComposeWorldTransform(ent);
        AABB wb = AABB{r.ModelRef->BoundsMin(), r.ModelRef->BoundsMax()}.Transformed(model);
        float t;
        if (wb.RayIntersect(origin, down, t) && t > 1e-3f && t < bestT) { bestT = t; hit = true; }
    }
    if (!hit) return false;

    PushUndo(world, "Drop Light to Surface");
    glm::vec3 landing = origin + down * bestT + glm::vec3(0.0f, 0.1f, 0.0f);

    // Write the LOCAL translation, so a parented light lands on the surface in world space.
    entt::entity parent = entt::null;
    if (auto* h = world.Registry.try_get<HierarchyComponent>(light)) parent = h->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);
    world.Registry.get<TransformComponent>(light).Position =
        glm::vec3(glm::inverse(parentWorld) * glm::vec4(landing, 1.0f));
    return true;
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

    // The toolbar is chrome, not a panel: hard-pinned to the top, full width, exact height, every
    // frame — independent of the Lock Layout toggle (which only governs the dock panels). Fixed
    // size + NoResize + size constraints keep it from ever being stretched down over the dock
    // panels' tab bars; NoMove/NoDocking keep it from being dragged off or docked; NoSavedSettings
    // stops a stale imgui.ini from repositioning it.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, toolbarH), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, toolbarH), ImVec2(w, toolbarH));
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    DrawTopToolbar(world, assets, editorCamera);

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
    m_SceneViewportVisible = ImGui::Begin("Scene", nullptr, sceneFlags);
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

    if (m_ShowHierarchy) DrawHierarchy(world, assets);
    if (m_ShowInspector) DrawInspector(world, assets, dt);
    if (m_ShowAssetBrowser) DrawAssetBrowser(world, assets);
    DrawConsole();
    if (!m_HideOverlaysThisFrame) {
        DrawStatsPanel(world, dt);
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
        auto& transform = world.Registry.get<TransformComponent>(m_Selected);
        glm::mat4 model = ComposeTransform(transform);
        glm::vec3 grabbedWorld = glm::vec3(model * glm::vec4(hoverLocal, 1.0f));
        m_VertexDragLocal = hoverLocal;
        m_VertexDragPlanePoint = grabbedWorld;
        m_VertexDragOffset = transform.Position - grabbedWorld;
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
        const auto& transform = world.Registry.get<TransformComponent>(m_Selected);
        glm::mat4 model = ComposeTransform(transform);
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
        const auto& transform = world.Registry.get<TransformComponent>(m_Selected);
        glm::mat4 model = ComposeTransform(transform);
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

float EditorLayer::SampleTextureLuminance(unsigned int colorTex, int texW, int texH,
                                          ImVec2 imgPos, ImVec2 imgSize, ImVec2 centerScreen, float boxPx) {
    if (colorTex == 0 || texW < 1 || texH < 1 || imgSize.x < 1.0f || imgSize.y < 1.0f) return -1.0f;

    const int kMaxPatch = 64;
    // screen box -> fraction of the displayed image -> texels (GL bottom-left origin). Handles
    // the Game view too, where the on-screen image is letterboxed and a different size than the
    // framebuffer it samples.
    const float sx = texW / imgSize.x, sy = texH / imgSize.y;
    int rw = (int)(boxPx * sx), rh = (int)(boxPx * sy);
    int rx = (int)((centerScreen.x - imgPos.x - boxPx * 0.5f) * sx);
    int ry = (int)((imgSize.y - ((centerScreen.y - imgPos.y) + boxPx * 0.5f)) * sy);
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > texW) rw = texW - rx;
    if (ry + rh > texH) rh = texH - ry;
    if (rw > kMaxPatch) { rx += (rw - kMaxPatch) / 2; rw = kMaxPatch; }
    if (rh > kMaxPatch) { ry += (rh - kMaxPatch) / 2; rh = kMaxPatch; }
    if (rx < 0 || ry < 0 || rw < 1 || rh < 1) return -1.0f;

    if (m_MarkSampleFbo == 0) glGenFramebuffers(1, &m_MarkSampleFbo);
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_MarkSampleFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    unsigned char px[kMaxPatch * kMaxPatch * 4];
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px);

    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevReadFbo);

    double sum = 0.0;
    const int n = rw * rh;
    for (int i = 0; i < n; ++i)
        sum += 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
    return (float)(sum / (n * 255.0)); // 0 = black behind the box, 1 = white
}

float EditorLayer::SampleSceneLuminance(ImVec2 centerScreen, float boxPx) {
    // The editor Scene framebuffer is sized 1:1 with the on-screen viewport, so texW/texH ARE
    // the viewport size.
    return SampleTextureLuminance(m_SceneColorTexture, (int)m_ViewportSize.x, (int)m_ViewportSize.y,
                                  ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                                  ImVec2(m_ViewportSize.x, m_ViewportSize.y), centerScreen, boxPx);
}

void EditorLayer::DrawPlayStopButton(bool playing, bool maximized) {
    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww;

    // NoDocking: without it this is technically a dockable floating window, and Reset Layout's
    // DockBuilderRemoveNode + full dockspace rebuild (in Draw()) can knock an undocked-but-
    // dockable window out of the visible window list entirely.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;

    // Where the control lives: over the Scene viewport while editing, over the Game viewport while
    // playing. Both are the same flat "vector + label" treatment with the engine-mark contrast
    // readback (throttled ~10 Hz, eased per frame) so the glyph rides white-on-dark / dark-on-
    // light against whatever's rendered behind it.
    const bool overScene = !playing && m_ViewportSize.x > 1.0f && m_ViewportSize.y > 1.0f;
    const bool overGame  =  playing && m_GameViewImgSize.x > 1.0f && m_GameViewImgSize.y > 1.0f;

    int fgV = 235; // near-white until the first sample lands

    if (overScene || overGame) {
        const ImVec2 imgPos  = overGame ? m_GameViewImgPos  : ImVec2(m_ViewportPos.x, m_ViewportPos.y);
        const ImVec2 imgSize = overGame ? m_GameViewImgSize : ImVec2(m_ViewportSize.x, m_ViewportSize.y);
        const float cx = imgPos.x + imgSize.x * 0.5f;
        const float cy = imgPos.y + 8.0f * m_UIScale;

        m_PlayBtnSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_PlayBtnSampleAccum >= 0.1f) {
            m_PlayBtnSampleAccum = 0.0f;
            const ImVec2 probe(cx, cy + 14.0f * m_UIScale);
            const float box = 48.0f * m_UIScale;
            float lum = overGame
                ? SampleTextureLuminance(m_GameViewTex, m_GameViewTexW, m_GameViewTexH, imgPos, imgSize, probe, box)
                : SampleSceneLuminance(probe, box);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_PlayBtnContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_PlayBtnContrastLum += (m_PlayBtnContrastTarget - m_PlayBtnContrastLum) * k;
        fgV = (int)(m_PlayBtnContrastLum * 255.0f + 0.5f);
        fgV = fgV < 0 ? 0 : (fgV > 255 ? 255 : fgV);

        ImGui::SetNextWindowPos(ImVec2(cx, cy), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        flags |= ImGuiWindowFlags_NoBackground;
    } else {
        // No viewport rect to anchor to (maximized play before the first Game-panel frame, or
        // editor UI hidden) — float near the top of the window with a faint plate to stay legible.
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
    }

    ImGui::Begin("##PlayStopButton", nullptr, flags);
    // Forces this to the front of the display order every frame so a dock rebuild elsewhere
    // (Reset Layout) can't bury it behind whatever the freshly recreated dock host window
    // ends up as. Skipped while a popup is open (e.g. the toolbar's Capture options) so the
    // button doesn't punch through a menu that legitimately overlays the viewport top-centre.
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // Flat "vector" button: no body at rest, just the white (or dark) PLAY glyph + label; a faint
    // plate of the inverse grey on hover/press so it still reads as pressable.
    const float f  = fgV / 255.0f;
    const float iv = 1.0f - f;
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(f, f, f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(iv, iv, iv, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(iv, iv, iv, 0.28f));

    if (!playing) {
        if (ImGui::Button(ICON_FA_PLAY "  Play")) m_PlayStopRequested = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Play the scene in the Game panel (F1)");
    } else {
        if (ImGui::Button(ICON_FA_STOP "  Stop")) m_PlayStopRequested = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Stop and revert the scene (F1)");

        ImGui::SameLine();
        const char* fsLabel = maximized ? ICON_FA_COMPRESS "  Restore" : ICON_FA_EXPAND "  Fullscreen";
        if (ImGui::Button(fsLabel)) m_MaximizeToggleRequested = true;
        if (ImGui::IsItemHovered()) {
            EditorUI::SetTooltip(maximized
                ? "Back to windowed play (editor panels return)"
                : "Maximize the Game view over the editor panels");
        }
    }

    ImGui::PopStyleColor(4);
    ImGui::End();
}

void EditorLayer::NewScene(World& world, AssetLibrary& assets) {
    world = World();
    InvalidateModelThumbnail(nullptr);
    ClearSelection();
    m_SoloLights.clear();
    m_MutedLights.clear();
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
    m_LookThroughMoved = false;
    m_PendingLookThrough = entt::null;
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_AutoSaveTimer = 0.0f;

    // Write the fresh scene to disk right away — the first free "Untitled N.json" under
    // project/scenes/ — so it shows in the Asset Browser's Scenes folder immediately and can be
    // renamed / deleted / reopened like any other scene.
    std::error_code ec;
    const std::filesystem::path dir = ProjectPaths::Resolve("scenes");
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path scenePath = dir / "Untitled.json";
    for (int n = 2; std::filesystem::exists(scenePath, ec); ++n)
        scenePath = dir / ("Untitled " + std::to_string(n) + ".json");

    const std::string pathStr = scenePath.generic_string();
    if (SceneSerializer::Save(world, assets, pathStr)) {
        // Only now that the fresh scene is safely on disk do we drop the OUTGOING scene's
        // recovery snapshot (#216) — while m_CurrentScenePath still names the outgoing scene,
        // so a failed write below leaves that snapshot in place as the way back.
        ClearRecoverySnapshot();
        m_CurrentScenePath = pathStr;
        m_Dirty = false;                       // matches disk
        m_SavedUndoDepth = (int)m_UndoStack.size();
        EditorSettings::Get().LastScenePath = pathStr;
        EditorSettings::Save();
    } else {
        // Couldn't write (read-only project dir, …) — fall back to untitled-in-memory: Save
        // prompts for a location, main.cpp skips save-on-exit while the path is empty. The
        // outgoing scene's recovery snapshot is deliberately left alone: the fresh scene
        // couldn't be persisted, so it's still the only way back if this session is lost too.
        Log::Error("New Scene: couldn't create '" + pathStr + "' — scene is untitled/in-memory.");
        m_CurrentScenePath.clear();
        m_Dirty = true;
        m_SavedUndoDepth = -1;
    }
}

void EditorLayer::OpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    if (path.empty() || !SceneSerializer::Load(world, assets, path)) return;
    InvalidateModelThumbnail(nullptr);
    ClearRecoverySnapshot(); // drop the outgoing scene's snapshot before switching away from it
    m_CurrentScenePath = path;
    EditorSettings::Get().LastScenePath = path; // reopen this one next launch (#95)
    EditorSettings::Save();
    ClearSelection();
    m_SoloLights.clear();
    m_MutedLights.clear();
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
    m_LookThroughMoved = false;
    m_PendingLookThrough = entt::null;
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_Dirty = false;
    m_SavedUndoDepth = 0; // freshly loaded — empty history == on disk
    m_AutoSaveTimer = 0.0f;
    Log::Info("Opened scene '" + path + "'.");
}

// Imports one file (never a directory) into `targetFolder` - the virtual Asset Browser folder
// it should be filed under, '/'-joined the same way m_CurrentAssetFolder is. Factored out of
// HandleDroppedFiles so a dropped folder's contents can each land in their own mirrored
// subfolder instead of everything collapsing into whichever folder happened to be open.
void EditorLayer::ImportDroppedFile(World& world, AssetLibrary& assets, Camera& editorCamera,
    const std::string& path, const std::string& targetFolder) {
    (void)editorCamera; // kept for signature symmetry with HandleDroppedFiles; not needed here
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string name = std::filesystem::path(path).stem().string();

    Log::Info("Importing '" + path + "'...");

    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        // Imports into the library only - it shows up in the Asset Browser, nothing more.
        // Deliberately NOT placed into the scene: that used to happen automatically here, but
        // it meant every dropped/imported model needed an undo (or a manual delete) if you only
        // wanted it available to drag in later. Explicit placement is now always the Asset
        // Browser -> Viewport drag (DrawViewportDropTarget's live ghost preview).
        assets.LoadModel(path);
        assets.SetAssetFolder(path, targetFolder);
        Log::Info("Imported model '" + name + "'.");
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
        auto tex = assets.LoadTexture(path);
        if (tex) {
            assets.SetAssetFolder(path, targetFolder);
            Log::Info("Imported texture '" + name + "'.");
        } else {
            Log::Error("Failed to load texture '" + path + "'.");
        }
    } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        if (AudioEngine::Load(path)) {
            assets.RegisterSound(path);
            assets.SetAssetFolder(path, targetFolder);
            Log::Info("Imported sound '" + name + "'.");
        } else {
            Log::Error("Failed to load sound '" + path + "'.");
        }
    } else if (ext == ".json") {
        // A dropped scene file offers to open it rather than silently doing nothing —
        // "import" has no other meaning for a whole scene.
        RequestOpenScene(world, assets, path);
    } else if (ext == ".prefab") {
        entt::entity e = SceneSerializer::InstantiatePrefab(world, assets, path);
        if (e != entt::null) {
            UniquifyName(world, e);
            assets.RegisterPrefab(path);
            assets.SetAssetFolder(path, targetFolder);
            SelectItem(e, false);
            Log::Info("Instantiated prefab '" + name + "'.");
        } else {
            Log::Error("Failed to load prefab '" + path + "'.");
        }
    } else if (ext == ".tif" || ext == ".tiff") {
        Log::Warn("Skipped '" + path + "' - TIFF isn't supported directly. Convert it to PNG first (see the TifSplitter tool) and drop that instead.");
    } else {
        Log::Warn("Don't know how to import '" + path + "' (unrecognized extension \"" + ext + "\").");
    }
}

void EditorLayer::HandleDroppedFiles(World& world, AssetLibrary& assets, Camera& editorCamera,
    bool editorUIVisible, const std::vector<std::string>& paths) {
    (void)world; (void)editorCamera; // only queued here; the actual import (ImportDroppedFile) uses them later
    if (!editorUIVisible) {
        Log::Warn("Ignored " + std::to_string(paths.size()) + " dropped file(s) - restore the editor panels to import assets.");
        return;
    }

    // Expanding a dropped folder's structure and resolving every file's target virtual folder
    // is pure filesystem traversal (no GL calls) - cheap and safe to do synchronously, right
    // here. Only the actual per-file import (which DOES touch GL, via Texture/Model loading)
    // gets deferred to the queue, drained a few at a time from EditorLayer::Draw.
    std::vector<std::string> toEnqueue;
    auto queueFile = [&](const std::string& filePath, const std::string& targetFolder) {
        m_ImportTargetFolder[filePath] = targetFolder;
        toEnqueue.push_back(filePath);
    };

    for (const std::string& path : paths) {
        std::error_code isDirErr;
        if (!std::filesystem::is_directory(path, isDirErr) || isDirErr) {
            queueFile(path, m_CurrentAssetFolder);
            continue;
        }

        // A dropped folder mirrors its own structure into the Asset Browser rather than
        // dumping every file it contains flat into whichever folder is currently open - e.g.
        // dropping "BuildingKit" (containing Meshes/ and Textures/Walls/) creates matching
        // "BuildingKit", "BuildingKit/Meshes", "BuildingKit/Textures/Walls" virtual folders
        // under the current one, and files land in the folder that mirrors where they sat on
        // disk. GLFW hands directory drops through the exact same path list as files - nothing
        // upstream of this treats them differently, so without this branch a dropped folder
        // just fell through to the "unrecognized extension" case below (a folder path has no
        // extension) and silently did nothing.
        std::filesystem::path root(path);
        std::string rootFolder = m_CurrentAssetFolder.empty()
            ? root.filename().string()
            : m_CurrentAssetFolder + "/" + root.filename().string();
        assets.CreateFolder(rootFolder);

        std::error_code walkErr;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, walkErr)) {
            std::error_code relErr;
            std::filesystem::path rel = std::filesystem::relative(entry.path(), root, relErr);
            if (relErr) continue;

            // Virtual folder path mirroring this entry's position under rootFolder - '/'
            // regardless of the OS path separator, since that's what the Asset Browser expects.
            std::string relFolder;
            for (const auto& part : rel.parent_path()) {
                if (!relFolder.empty()) relFolder += "/";
                relFolder += part.string();
            }
            std::string virtualFolder = relFolder.empty() ? rootFolder : rootFolder + "/" + relFolder;

            if (entry.is_directory()) {
                assets.CreateFolder(virtualFolder + "/" + entry.path().filename().string());
            } else if (entry.is_regular_file()) {
                assets.CreateFolder(virtualFolder);
                queueFile(entry.path().string(), virtualFolder);
            }
        }
        if (walkErr) Log::Warn("Folder scan of '" + path + "' reported: " + walkErr.message());
    }

    m_ImportQueue.Enqueue(toEnqueue);
}

void EditorLayer::DrawConsole() {
    if (!m_ShowConsole) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (!ImGui::Begin(ICON_FA_TERMINAL "  Console", &m_ShowConsole, flags)) { ImGui::End(); return; }

    // Builds the plain-text dump of everything currently shown (respects the level + text
    // filters), used by "Save..." and the right-click "Copy all shown".
    auto buildShownText = [&]() {
        std::string out;
        for (const LogEntry& e : Log::Entries()) {
            bool lv = (e.Level == LogLevel::Info && m_ConsoleShowInfo) ||
                      (e.Level == LogLevel::Warning && m_ConsoleShowWarning) ||
                      (e.Level == LogLevel::Error && m_ConsoleShowError);
            if (!lv || !MatchesFilter(m_ConsoleFilter, e.Message)) continue;
            const char* tag = e.Level == LogLevel::Error ? "ERROR" : (e.Level == LogLevel::Warning ? "WARN " : "INFO ");
            out += "[" + e.Time + "] " + tag + "  " + e.Message;
            if (e.Count > 1) out += "  (x" + std::to_string(e.Count) + ")";
            out += "\n";
        }
        return out;
    };

    if (ActionButton(ICON_FA_TRASH "  Clear", "Remove every message from the console")) Log::Clear();
    ImGui::SameLine();
    if (ActionButton(ICON_FA_FLOPPY_DISK "  Save...", "Write the messages currently shown to a text file")) {
        std::string path = FileDialog::SaveFile("Log Files\0*.log;*.txt\0All Files\0*.*\0", "log", m_Window);
        if (!path.empty()) {
            std::ofstream f(path, std::ios::binary);
            if (f) { f << buildShownText(); Log::Info("Console saved to " + path); }
            else Log::Error("Couldn't write " + path);
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_ConsoleAutoScroll);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Automatically jump to the newest message as it arrives");
    ImGui::SameLine();
    ImGui::Checkbox("Timestamps", &m_ConsoleShowTimestamps);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show the HH:MM:SS each message first arrived");

    // Per-level toggles double as counters, the way Unity's console header does.
    EditorUI::VSeparator();
    char infoLabel[32], warnLabel[32], errorLabel[32];
    snprintf(infoLabel, sizeof(infoLabel), ICON_FA_CIRCLE_INFO " %d", Log::CountOf(LogLevel::Info));
    snprintf(warnLabel, sizeof(warnLabel), ICON_FA_TRIANGLE_EXCLAMATION " %d", Log::CountOf(LogLevel::Warning));
    snprintf(errorLabel, sizeof(errorLabel), ICON_FA_CIRCLE_EXCLAMATION " %d", Log::CountOf(LogLevel::Error));
    ImGui::Checkbox(infoLabel, &m_ConsoleShowInfo);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show/hide informational messages");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.30f, 1.0f));
    ImGui::Checkbox(warnLabel, &m_ConsoleShowWarning);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show/hide warnings");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.38f, 1.0f));
    ImGui::Checkbox(errorLabel, &m_ConsoleShowError);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show/hide errors");

    ImGui::SetNextItemWidth(-1.0f);
    char filterBuf[128];
    snprintf(filterBuf, sizeof(filterBuf), "%s", m_ConsoleFilter.c_str());
    if (ImGui::InputTextWithHint("##ConsoleFilter", ICON_FA_MAGNIFYING_GLASS "  Filter messages...", filterBuf, sizeof(filterBuf))) {
        m_ConsoleFilter = filterBuf;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) EditorUI::SetTooltip("Only show messages containing this text");

    ImGui::Separator();
    if (ImGui::BeginChild("##ConsoleScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const LogEntry& entry : Log::Entries()) {
            bool levelVisible =
                (entry.Level == LogLevel::Info && m_ConsoleShowInfo) ||
                (entry.Level == LogLevel::Warning && m_ConsoleShowWarning) ||
                (entry.Level == LogLevel::Error && m_ConsoleShowError);
            if (!levelVisible || !MatchesFilter(m_ConsoleFilter, entry.Message)) continue;

            ImVec4 color(0.82f, 0.84f, 0.86f, 1.0f);
            const char* icon = ICON_FA_CIRCLE_INFO;
            if (entry.Level == LogLevel::Warning) { color = ImVec4(1.0f, 0.80f, 0.30f, 1.0f); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
            else if (entry.Level == LogLevel::Error) { color = ImVec4(1.0f, 0.42f, 0.38f, 1.0f); icon = ICON_FA_CIRCLE_EXCLAMATION; }

            std::string tsPrefix = (m_ConsoleShowTimestamps && !entry.Time.empty()) ? ("[" + entry.Time + "]  ") : "";
            char rowLabel[1200];
            if (entry.Count > 1)
                snprintf(rowLabel, sizeof(rowLabel), "%s%s  %s  (x%d)##r%p", tsPrefix.c_str(), icon,
                         entry.Message.c_str(), entry.Count, (const void*)&entry);
            else
                snprintf(rowLabel, sizeof(rowLabel), "%s%s  %s##r%p", tsPrefix.c_str(), icon,
                         entry.Message.c_str(), (const void*)&entry);

            // A full-width Selectable (rather than a bare Text) so the whole row is a real item
            // with a hover rect — needed for a reliable right-click context menu.
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Selectable(rowLabel, false, ImGuiSelectableFlags_AllowDoubleClick);
            ImGui::PopStyleColor();

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy message")) ImGui::SetClipboardText(entry.Message.c_str());
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy all shown")) {
                    std::string all = buildShownText();
                    ImGui::SetClipboardText(all.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Clear console")) Log::Clear();
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Right-click for copy / clear");
        }

        // Only when something actually arrived, so scrolling back through history isn't yanked
        // to the bottom on every single frame.
        if (m_ConsoleAutoScroll && Log::Revision() != m_ConsoleSeenRevision) {
            ImGui::SetScrollHereY(1.0f);
        }
        m_ConsoleSeenRevision = Log::Revision();
    }
    ImGui::EndChild();

    ImGui::End();
}

void EditorLayer::PushAdaptiveHudText(ImVec2 centerScreen, float boxPx, float dt,
                                     float& easedLum, float& targetLum, float& sampleAccum) {
    // Throttled GPU->CPU readback (~10 Hz) — the sync is too costly to do every frame.
    sampleAccum += dt;
    if (sampleAccum >= 0.1f) {
        sampleAccum = 0.0f;
        float lum = SampleSceneLuminance(centerScreen, boxPx);
        if (lum >= 0.0f) {
            float t = (lum - 0.30f) / (0.62f - 0.30f);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            targetLum = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
        }
    }
    float k = 1.0f - expf(-dt / 0.15f);
    easedLum += (targetLum - easedLum) * k;
    int v = (int)(easedLum * 255.0f + 0.5f);
    v = v < 0 ? 0 : (v > 255 ? 255 : v);
    ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(v, v, v, 240));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(v, v, v, 150));
}

void EditorLayer::DrawStatsPanel(World& world, float dt) {
    // Exponential smoothing: a raw per-frame ms figure flickers too fast to read. Kept running
    // every frame (even while the panel is hidden) so the status-bar FPS/ms stays smooth too.
    float frameMs = dt * 1000.0f;
    m_SmoothedFrameMs = m_SmoothedFrameMs * 0.92f + frameMs * 0.08f;

    m_HideEngineMarkForStats = false;

    if (!EditorSettings::Get().SceneShowStats) return;
    // Tied to the Scene view — the numbers describe that render, and the pin needs a live
    // viewport rect to anchor to.
    if (!m_SceneViewportVisible || m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    int entityCount = 0, renderableCount = 0, lightCount = 0, colliderCount = 0, inactiveCount = 0;
    for (auto entity : world.Registry.view<TransformComponent>()) {
        entityCount++;
        if (world.Registry.all_of<RenderableComponent>(entity)) renderableCount++;
        if (world.Registry.all_of<LightComponent>(entity)) lightCount++;
        if (world.Registry.all_of<ColliderComponent>(entity)) colliderCount++;
        if (world.Registry.all_of<InactiveTag>(entity)) inactiveCount++;
    }

    // A compact HUD pinned to the viewport's top-left corner (#149): fully transparent so it
    // doesn't box off the scene, click-through (NoInputs) so it never eats camera-look. Height
    // is measured from its content and capped so it never spills past the status bar into the
    // panels below (same treatment as the History HUD); the profiler tail clips rather than
    // overflowing. Toggled via SceneShowStats; not dockable, not persisted — the pin wins.
    const float pad = 12.0f * m_UIScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;
    const ImGuiStyle& stStats = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int profN = (int)Profiler::GetLastFrame().size();
    const int profGpuN = (int)Profiler::GetLastFrameGpu().size();
    const bool hasGpuSection = profGpuN > 0;
    const int rows = 1 /*fps*/ + 3 /*draw/tri/vert*/ + (m_RenderStats.Culled > 0 ? 1 : 0)
                   + 4 /*ent/rend/coll/light*/ + (inactiveCount > 0 ? 1 : 0)
                   + profN + (hasGpuSection ? profGpuN : 0) + 2 /*shader/texture binds*/;
    const float chromeH = lineH * (2.0f + (hasGpuSection ? 1.0f : 0.0f))  // "Statistics" + "Profiler (CPU)" [+ "Profiler (GPU)"]
                        + (4.0f + (hasGpuSection ? 1.0f : 0.0f)) * (stStats.ItemSpacing.y + 2.0f) // Separator() rules
                        + stStats.WindowPadding.y * 2.0f + 4.0f;
    const float desiredH = chromeH + rows * lineH;
    const float maxH = std::max(120.0f * m_UIScale,
                                m_ViewportSize.y - statusBarH - 2.0f * pad);
    const float winH = std::min(desiredH, maxH);

    // If the box would reach down into the corner monogram, hide the monogram until it doesn't
    // (checked in Draw() at the DrawEngineMark call site).
    const float markTop = m_ViewportSize.y - statusBarH - 14.0f * m_UIScale - 54.0f * m_UIScale;
    m_HideEngineMarkForStats = (pad + winH + 8.0f * m_UIScale) > markTop;

    ImGui::SetNextWindowPos(
        ImVec2(m_ViewportPos.x + pad, m_ViewportPos.y + pad),
        ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 → auto-fit width; height clamped
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##Stats", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground)) {
        // Contrast-adaptive tint (like the corner mark): sample the scene behind the HUD so the
        // text stays legible white-on-dark / dark-on-light with no plate behind it.
        const ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
        PushAdaptiveHudText(ImVec2(wpos.x + wsz.x * 0.5f, wpos.y + wsz.y * 0.5f), 48.0f * m_UIScale,
                            dt, m_StatsHudContrastLum, m_StatsHudContrastTarget, m_StatsHudSampleAccum);
        ImGui::TextUnformatted(ICON_FA_CHART_SIMPLE "  Statistics");
        ImGui::Separator();
        ImGui::Text("%.1f FPS  (%.2f ms)", m_SmoothedFrameMs > 0.0001f ? 1000.0f / m_SmoothedFrameMs : 0.0f, m_SmoothedFrameMs);
        ImGui::Separator();
        ImGui::Text("Draw calls   %d", m_RenderStats.DrawCalls);
        ImGui::Text("Triangles    %d", m_RenderStats.Triangles);
        ImGui::Text("Vertices     %d", m_RenderStats.Vertices);
        if (m_RenderStats.Culled > 0) ImGui::TextDisabled("Culled       %d (outside view)", m_RenderStats.Culled);
        ImGui::Separator();
        ImGui::Text("Entities     %d", entityCount);
        ImGui::Text("Renderers    %d", renderableCount);
        ImGui::Text("Colliders    %d", colliderCount);
        ImGui::Text("Lights       %d", lightCount);
        if (inactiveCount > 0) ImGui::TextDisabled("Inactive     %d", inactiveCount);

        // Numbers from the frame that just finished (this frame's own "Scene Draw"/"ImGui
        // Render" scopes haven't run yet at this point) - same one-frame-behind convention the
        // smoothed FPS figure above already uses, so it's not called out as its own oddity.
        ImGui::SeparatorText("Profiler (CPU)");
        for (const auto& sample : Profiler::GetLastFrame()) {
            ImGui::Text("%-16s %.3f ms", sample.Name.c_str(), sample.Milliseconds);
        }
        // GPU timings land several frames later than their CPU counterparts (pipelining), so
        // these are "most recently completed", not "this frame" - the CPU section above already
        // reads one frame behind; this one just trails a little further (#197).
        const auto& gpuSamples = Profiler::GetLastFrameGpu();
        if (!gpuSamples.empty()) {
            ImGui::SeparatorText("Profiler (GPU)");
            for (const auto& sample : gpuSamples) {
                ImGui::Text("%-16s %.3f ms", sample.Name.c_str(), sample.Milliseconds);
            }
        }
        ImGui::Separator();
        const auto& gl = GLStateCache::GetFrameStats();
        ImGui::Text("Shader binds   %d (%d skipped)", gl.ProgramBinds, gl.ProgramBindsSkipped);
        ImGui::Text("Texture binds  %d (%d skipped)", gl.TextureBinds, gl.TextureBindsSkipped);
        ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
    }
    ImGui::End();
}

void EditorLayer::DrawViewportStatusBar() {
    if (!m_SceneViewportVisible || m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    const char* toolName =
        m_GizmoOp == GizmoOp::Translate ? "Translate" :
        m_GizmoOp == GizmoOp::Rotate    ? "Rotate"    :
        m_GizmoOp == GizmoOp::Scale     ? "Scale"     : "Rect";

    int selCount = (int)GetSelectedItems().size();
    float fps = m_SmoothedFrameMs > 0.0001f ? 1000.0f / m_SmoothedFrameMs : 0.0f;

    auto compact = [](int n) -> std::string {
        if (n >= 1000000) { char b[32]; snprintf(b, sizeof(b), "%.1fM", n / 1e6); return b; }
        if (n >= 1000)    { char b[32]; snprintf(b, sizeof(b), "%.1fk", n / 1e3); return b; }
        return std::to_string(n);
    };

    const float barH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;

    // No strip behind the readout any more — it's a bare line of text over the 3D view. To stay
    // legible it borrows the engine mark's contrast-adaptive trick: sample the scene luminance
    // behind it and steer the text white-on-dark / dark-on-light (throttled ~10 Hz, eased).
    {
        m_StatusBarSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_StatusBarSampleAccum >= 0.1f) {
            m_StatusBarSampleAccum = 0.0f;
            float lum = SampleSceneLuminance(
                ImVec2(m_ViewportPos.x + m_ViewportSize.x * 0.5f,
                       m_ViewportPos.y + m_ViewportSize.y - barH * 0.5f),
                barH);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_StatusBarContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_StatusBarContrastLum += (m_StatusBarContrastTarget - m_StatusBarContrastLum) * k;
    }
    int sbV = (int)(m_StatusBarContrastLum * 255.0f + 0.5f);
    sbV = sbV < 0 ? 0 : (sbV > 255 ? 255 : sbV);

    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x, m_ViewportPos.y + m_ViewportSize.y - barH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(m_ViewportSize.x, barH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * m_UIScale, 3.0f * m_UIScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text,         IM_COL32(sbV, sbV, sbV, 240));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(sbV, sbV, sbV, 150));
    if (ImGui::Begin("##ViewportStatusBar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs)) {

        auto sep = [&]() { ImGui::SameLine(0, 6); ImGui::TextDisabled("\xc2\xb7"); ImGui::SameLine(0, 6); };

        ImGui::AlignTextToFramePadding();
        ImGui::Text("%.0f FPS", fps);
        sep(); ImGui::TextDisabled("%.1f ms", m_SmoothedFrameMs);
        sep(); ImGui::Text("%s draws", compact(m_RenderStats.DrawCalls).c_str());
        sep(); ImGui::TextDisabled("%s tris", compact(m_RenderStats.Triangles).c_str());
        sep();
        if (selCount == 0) ImGui::TextDisabled("no selection");
        else               ImGui::Text("%d selected", selCount);

        // Active tool pinned to the right.
        char toolBuf[32]; snprintf(toolBuf, sizeof(toolBuf), "%s  %s", ICON_FA_UP_DOWN_LEFT_RIGHT, toolName);
        float tw = ImGui::CalcTextSize(toolBuf).x;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - tw);
        ImGui::TextUnformatted(toolBuf);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Unity-style Undo History: every recorded change, oldest to newest, with the current position
// highlighted. Clicking any entry jumps straight there via JumpToUndoEntry/JumpToRedoEntry -
// each step is still a single full-snapshot load (see PushUndo's own comment), not incremental
// command replay, so jumping several steps at once stays cheap regardless of distance.
void EditorLayer::DrawHistoryPanel(World& world, AssetLibrary& assets) {
    if (!m_ShowHistory) return;

    // Compact transparent HUD pinned to the viewport's bottom-right corner, matching the Stats
    // HUD at top-left (#149 follow-up): sized to its text, fully transparent, not dockable, not
    // persisted so the pin always wins. Unlike Stats it stays interactive — the rows are
    // click-to-jump.
    const float hpad = 12.0f * m_UIScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;
    // Height ceiling: grow upward from the pin only until a clear line below the top-right nav
    // cluster, so a long history never climbs into that corner (or past it into the toolbar).
    // Derived from DrawViewGizmo's layout: margin 14 + rotate-ring Ø128*0.5 + spacing 8 + the
    // dolly/pan button box + the "Persp" label, all *m_UIScale, plus headroom (~220px @ 1x).
    // Beyond the ceiling the list scrolls internally instead.
    const float gizmoZoneH = 220.0f * m_UIScale;
    const float maxH = std::max(120.0f * m_UIScale,
                                m_ViewportSize.y - hpad - statusBarH - gizmoZoneH);

    // Rows + chrome, measured directly so the window is exactly as tall as its content up to maxH.
    const int rowCount = (int)m_UndoStack.size() + 1 /*Current*/ + (int)m_RedoStack.size();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float chromeH = ImGui::GetTextLineHeightWithSpacing()       // "History" line
                        + st.ItemSpacing.y + 2.0f                     // separator
                        + st.WindowPadding.y * 2.0f;
    const float desiredH = chromeH + std::max(rowCount, 1) * ImGui::GetTextLineHeightWithSpacing();
    const float winH = std::min(desiredH, maxH);

    ImGui::SetNextWindowPos(
        ImVec2(m_ViewportPos.x + m_ViewportSize.x - hpad,
               m_ViewportPos.y + m_ViewportSize.y - hpad - statusBarH),
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(0.0f, winH)); // x=0 → auto-fit width, height clamped to the ceiling
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History", &m_ShowHistory, flags)) { ImGui::End(); return; }

    // Contrast-adaptive tint (like the corner mark): sample the scene behind the HUD so the text
    // stays legible white-on-dark / dark-on-light with no plate behind it.
    const ImVec2 wpos = ImGui::GetWindowPos(), wsz = ImGui::GetWindowSize();
    PushAdaptiveHudText(ImVec2(wpos.x + wsz.x * 0.5f, wpos.y + wsz.y * 0.5f), 48.0f * m_UIScale,
                        ImGui::GetIO().DeltaTime, m_HistoryHudContrastLum, m_HistoryHudContrastTarget,
                        m_HistoryHudSampleAccum);

    ImGui::TextUnformatted(ICON_FA_CLOCK_ROTATE_LEFT "  History");
    // Explanation on the heading tooltip (#156) — no persistent "(?)" glyph.
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(
        "Every recorded change, oldest to newest. Click any entry to jump\nstraight there - undoing or redoing everything in between automatically.");
    ImGui::Separator();

    if (m_UndoStack.empty() && m_RedoStack.empty()) {
        ImGui::TextDisabled("No changes yet.");
        ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
        ImGui::End();
        return;
    }

    for (size_t k = 0; k < m_UndoStack.size(); ++k) {
        ImGui::PushID((int)k);
        if (ImGui::Selectable(m_UndoStack[k].Label.c_str())) {
            JumpToUndoEntry(world, assets, k);
        }
        ImGui::PopID();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
    ImGui::Selectable(ICON_FA_LOCATION_DOT "  Current", true);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Where you are right now");

    // Newest-future-first won't read naturally, so this walks the redo stack back-to-front
    // (Redo() always consumes from .back()) to show it oldest-to-newest like everything above.
    for (size_t idx = m_RedoStack.size(); idx-- > 0;) {
        ImGui::PushID((int)(100000 + idx)); // distinct ID range from the undo rows above
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        bool clicked = ImGui::Selectable(m_RedoStack[idx].Label.c_str());
        ImGui::PopStyleColor();
        if (clicked) JumpToRedoEntry(world, assets, idx);
        ImGui::PopID();
    }

    ImGui::PopStyleColor(2); // adaptive Text + TextDisabled
    ImGui::End();
}

// The Add-menu body, shared verbatim by the File-menu-bar "Add" menu and the Shift+A quick-add
// popup (ImGui::MenuItem works inside BeginMenu and BeginPopup alike).
void EditorLayer::DrawAddEntityItems(World& world, AssetLibrary& assets, Camera& editorCamera) {
    auto spawnPrimitive = [&](const char* kind, const char* displayName) {
        PushUndo(world, std::string("Create ") + displayName);
        auto model = assets.CreatePrimitive(kind);
        glm::vec3 position = SafeSpawnInFrontOf(editorCamera);
        entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, displayName));
        SelectItem(e, false);
        Log::Info(std::string("Added ") + displayName + ".");
    };
    if (ImGui::MenuItem(ICON_FA_CUBE "  Cube")) spawnPrimitive("cube", "Cube");
    if (ImGui::MenuItem(ICON_FA_CIRCLE "  Sphere")) spawnPrimitive("sphere", "Sphere");
    if (ImGui::MenuItem(ICON_FA_DATABASE "  Cylinder")) spawnPrimitive("cylinder", "Cylinder");
    if (ImGui::MenuItem(ICON_FA_CAPSULES "  Capsule")) spawnPrimitive("capsule", "Capsule");
    if (ImGui::MenuItem(ICON_FA_FILTER "  Cone")) spawnPrimitive("cone", "Cone");
    if (ImGui::MenuItem(ICON_FA_MOUNTAIN "  Pyramid")) spawnPrimitive("pyramid", "Pyramid");
    if (ImGui::MenuItem(ICON_FA_LIFE_RING "  Torus")) spawnPrimitive("torus", "Torus");
    if (ImGui::MenuItem(ICON_FA_SQUARE "  Plane")) spawnPrimitive("plane", "Plane");
    if (ImGui::MenuItem(ICON_FA_STAIRS "  Wedge")) spawnPrimitive("wedge", "Wedge");

    ImGui::SeparatorText("Objects");
    if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Empty")) {
        CreateEmptyAt(world, &editorCamera, "Empty", false);
    }
    if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light")) {
        CreateEmptyAt(world, &editorCamera, "Point Light", true);
    }
    if (ImGui::MenuItem(ICON_FA_BULLSEYE "  Spot Light")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Spot Light", true);
        world.Registry.get<LightComponent>(e).Kind = LightComponent::Type::Spot;
    }
    if (ImGui::MenuItem(ICON_FA_SUN "  Directional Light")) {
        MakeDirectionalLight(world, CreateEmptyAt(world, &editorCamera, "Directional Light", true));
    }
    if (ImGui::MenuItem(ICON_FA_VIDEO "  Camera")) {
        entt::entity e = CreateEmptyAt(world, &editorCamera, "Camera", false);
        world.Registry.emplace<CameraComponent>(e);
        // Aim it back at the world origin so its Game-view preview isn't just black —
        // ComposeTransform rotates Y(yaw) then X(pitch), local -Z is forward.
        auto& t = world.Registry.get<TransformComponent>(e);
        glm::vec3 d = t.Position;
        if (glm::dot(d, d) > 1.0e-4f) {
            d = glm::normalize(-d); // direction from the camera toward the origin
            t.RotationEuler = glm::vec3(
                glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))),
                glm::degrees(std::atan2(-d.x, -d.z)),
                0.0f);
        }
    }
}

// Fixed output sizes offered by the Capture popup's "Resolution" dropdown. Index 0 keeps the
// live viewport size (and lets Supersample apply); the rest force an exact render target.
// Shared by the toolbar popup and RequestCapture().
static const struct { const char* label; int w, h; } kCaptureRes[] = {
    { "Match viewport", 0,    0    },
    { "1280 x 720",     1280, 720  },
    { "1920 x 1080",    1920, 1080 },
    { "2560 x 1440",    2560, 1440 },
    { "3840 x 2160",    3840, 2160 },
};

void EditorLayer::DrawTopToolbar(World& world, AssetLibrary& assets, Camera& editorCamera) {
    // Always pinned regardless of Lock Layout — pos/size are forced every frame by the caller.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    // Tight vertical window padding so the icon row hugs the menu bar and the bottom edge — the
    // strip is only as tall as its two rows now (kToolbarHeight).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 3.0f));
    if (!ImGui::Begin("##Toolbar", nullptr, flags)) { ImGui::End(); ImGui::PopStyleVar(); return; }

    // Real dropdown menus for the stuff you reach for occasionally (import, add primitive,
    // scene save/load) — keeps the always-visible row below reserved for one-click toggles.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " File")) {
            if (ImGui::MenuItem(ICON_FA_FILE "  New Scene", "Ctrl+N")) {
                RequestNewScene(world, assets);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open...", "Ctrl+O")) {
                RequestOpenScene(world, assets, FileDialog::OpenFile(
                    "Scene Files\0*.json\0All Files\0*.*\0", m_Window));
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S")) {
                DoSave(world, assets);
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As...", "Ctrl+Shift+S")) {
                DoSaveAs(world, assets);
            }
            ImGui::Separator();
            {
                std::string cur = std::filesystem::path(m_CurrentScenePath).filename().string();
                if (cur.empty()) cur = "Untitled";
                ImGui::TextDisabled("Current: %s%s", cur.c_str(), m_Dirty ? " (unsaved)" : "");
            }
            if (EditorSettings::Get().AutoSaveEnabled) {
                float remaining = std::max(0.0f, EditorSettings::Get().AutoSaveIntervalMinutes * 60.0f - m_AutoSaveTimer);
                ImGui::TextDisabled("Next auto-save in %.0fs%s", remaining, m_Dirty ? "" : " (nothing to save)");
            }

            ImGui::Separator();
            if (ImGui::BeginMenu(ICON_FA_FILE_IMPORT "  Import")) {
                if (ImGui::MenuItem(ICON_FA_CUBE "  Model...")) {
                    std::string path = FileDialog::OpenFile(
                        "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0", m_Window);
                    // Imports into the library only — doesn't place an instance in the scene.
                    // Drag it from the Asset Browser into the Viewport to place one.
                    if (!path.empty()) assets.LoadModel(path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("FBX / OBJ / glTF - added to the asset library");
                if (ImGui::MenuItem(ICON_FA_IMAGE "  Texture...")) {
                    std::string path = FileDialog::OpenFile(
                        "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
                    if (!path.empty()) assets.LoadTexture(path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("PNG / JPG / TGA / BMP");
                if (ImGui::MenuItem(ICON_FA_MUSIC "  Sound...")) {
                    std::string path = FileDialog::OpenFile(
                        "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0", m_Window);
                    if (!path.empty() && AudioEngine::Load(path)) assets.RegisterSound(path);
                }
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("WAV / MP3 / OGG / FLAC");
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CUBES " Add")) {
            DrawAddEntityItems(world, assets, editorCamera);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CAMERA " View")) {
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS_PLUS "  Frame Selected", "F", false, HasAnySelection())) {
                FocusOnSelection(world, editorCamera);
            }
            if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Frame All")) {
                glm::vec3 mn, mx;
                if (ComputeSceneBounds(world, mn, mx)) {
                    glm::vec3 c = (mn + mx) * 0.5f;
                    float radius = std::max(glm::length(mx - mn) * 0.5f, 0.5f);
                    float dist = (radius / std::sin(glm::radians(editorCamera.Fov) * 0.5f)) * 1.35f;
                    glm::vec3 target = c - editorCamera.Front() * dist;
                    if (std::isfinite(target.x) && std::isfinite(target.y) && std::isfinite(target.z))
                        editorCamera.Position = target;
                }
            }

            ImGui::SeparatorText("Shading");
            if (ImGui::MenuItem("  Shaded", nullptr, m_ShadingMode == ShadingMode::Shaded))
                m_ShadingMode = ShadingMode::Shaded;
            if (ImGui::MenuItem("  Wireframe", nullptr, m_ShadingMode == ShadingMode::Wireframe))
                m_ShadingMode = ShadingMode::Wireframe;
            if (ImGui::MenuItem("  Unlit", nullptr, m_ShadingMode == ShadingMode::Unlit))
                m_ShadingMode = ShadingMode::Unlit;

            // Grid lives only on the toolbar now (#148) — the menu keeps just the toggles that
            // have no toolbar home.
            ImGui::SeparatorText("Options");
            ImGui::MenuItem(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform Gizmo", nullptr, &m_ShowGizmos);
            ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Frame on Select", nullptr, &m_FrameOnSelect);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Move the camera to frame each object as you select it.");
            if (ImGui::MenuItem(ICON_FA_BORDER_ALL "  Orthographic", "5", editorCamera.Orthographic)) {
                ToggleOrthographic(world, editorCamera);
            }

            ImGui::SeparatorText("Snap to view");
            if (ImGui::MenuItem("  Iso", "0")) {
                SnapToView(world, editorCamera, -45.0f, -35.264f, true);
            }
            if (ImGui::MenuItem("  Front", "1")) SnapToView(world, editorCamera, -90.0f, 0.0f, true);
            if (ImGui::MenuItem("  Back", "Ctrl+1")) SnapToView(world, editorCamera, 90.0f, 0.0f, true);
            if (ImGui::MenuItem("  Right", "3")) SnapToView(world, editorCamera, 180.0f, 0.0f, true);
            if (ImGui::MenuItem("  Left", "Ctrl+3")) SnapToView(world, editorCamera, 0.0f, 0.0f, true);
            if (ImGui::MenuItem("  Top", "7")) SnapToView(world, editorCamera, -90.0f, -89.9f, true);
            if (ImGui::MenuItem("  Bottom", "Ctrl+7")) SnapToView(world, editorCamera, -90.0f, 89.9f, true);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Window")) {
            ImGui::MenuItem(ICON_FA_SITEMAP "  Scene Hierarchy", nullptr, &m_ShowHierarchy);
            ImGui::MenuItem(ICON_FA_SLIDERS "  Inspector", nullptr, &m_ShowInspector);
            ImGui::MenuItem(ICON_FA_FOLDER_TREE "  Asset Browser", nullptr, &m_ShowAssetBrowser);
            ImGui::Separator();
            // Console / Statistics / History / Light Gizmos have dedicated toolbar toggles (#148);
            // the engine mark lives in Preferences ▸ Viewport.
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset Layout")) {
                m_ResetLayoutRequested = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(ICON_FA_GEAR " Preferences")) m_ShowPreferences = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Editor settings, environment, shortcuts (Ctrl+,)");

        // Custom window controls, right-aligned — the OS title bar is gone (Win32 custom frame,
        // Window.cpp), so minimize / maximize-restore / close live here instead.
        DrawWindowControls();

        ImGui::EndMenuBar();
    }

    // A spaced group separator — a real 1px rule with breathing room on both sides so the toolbar
    // reads as distinct clusters (history · tools · grid/snap · view · panels · lock) instead of
    // one dense left-jammed run of identical squares (audit #66 / #147).
    auto divider = []() { EditorUI::VSeparator(1.5f); };

    // Toolbar tools/toggles use the shared flat treatment (#160): ActionButton(icon, tip, active).
    auto iconButton = [](const char* icon, const char* tooltip, bool active = false) {
        return ActionButton(icon, tooltip, active);
    };

    if (iconButton(ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)")) Undo(world, assets);
    ImGui::SameLine();
    if (iconButton(ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)")) Redo(world, assets);

    divider();
    if (iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (W)", m_GizmoOp == GizmoOp::Translate)) m_GizmoOp = GizmoOp::Translate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_ARROWS_SPIN, "Rotate (E)", m_GizmoOp == GizmoOp::Rotate)) m_GizmoOp = GizmoOp::Rotate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale (R)", m_GizmoOp == GizmoOp::Scale)) m_GizmoOp = GizmoOp::Scale;
    ImGui::SameLine();
    if (iconButton(ICON_FA_VECTOR_SQUARE, "Rect — move + non-uniform scale via corner/edge handles (T)",
            m_GizmoOp == GizmoOp::Rect)) m_GizmoOp = GizmoOp::Rect;

    divider(); // transform tools | gizmo-space modifiers
    if (iconButton(m_GizmoLocalSpace ? ICON_FA_ARROWS_TO_DOT : ICON_FA_GLOBE,
            m_GizmoLocalSpace ? "Local space (click for World)" : "World space (click for Local)")) {
        m_GizmoLocalSpace = !m_GizmoLocalSpace;
    }
    ImGui::SameLine();
    if (iconButton(m_GizmoPivotCenter ? ICON_FA_CIRCLE_DOT : ICON_FA_CROSSHAIRS,
            m_GizmoPivotCenter
                ? "Center - gizmo sits on the bounding-box center (click for Pivot)"
                : "Pivot - gizmo sits on the object's own origin (click for Center)")) {
        m_GizmoPivotCenter = !m_GizmoPivotCenter;
    }

    divider();
    if (iconButton(ICON_FA_TABLE_CELLS, "Toggle Grid", m_ShowGrid)) m_ShowGrid = !m_ShowGrid;
    ImGui::SameLine();
    if (iconButton(ICON_FA_MAGNET, "Toggle Snap to Grid (hold Ctrl to invert while dragging)", m_GridSnapEnabled)) {
        m_GridSnapEnabled = !m_GridSnapEnabled;
    }
    ImGui::SameLine();
    {
        bool canSnap = CanSnapSelectionToGround(world);
        ImGui::BeginDisabled(!canSnap);
        if (iconButton(ICON_FA_DOWN_LONG, "Snap selection to ground")) SnapSelectionToGround(world);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        auto& prefs = EditorSettings::Get();
        if (iconButton(ICON_FA_CIRCLE_NODES, "Toggle Light Gizmos (range/cone/aim in the viewport)",
                prefs.ShowLightGizmos)) {
            prefs.ShowLightGizmos = !prefs.ShowLightGizmos;
            EditorSettings::Save();
        }
    }

    ImGui::SameLine();
    // Scene-view shading, cycling Shaded -> Wireframe -> Unlit like a draw-mode dropdown — part of
    // the same "what the viewport shows" cluster as grid / snap / light gizmos.
    {
        const char* shadingIcon = ICON_FA_CIRCLE_HALF_STROKE;
        const char* shadingTip = "Shaded (click for Wireframe)";
        if (m_ShadingMode == ShadingMode::Wireframe) {
            shadingIcon = ICON_FA_BORDER_NONE;
            shadingTip = "Wireframe (click for Unlit)";
        } else if (m_ShadingMode == ShadingMode::Unlit) {
            shadingIcon = ICON_FA_SUN;
            shadingTip = "Unlit (click for Shaded)";
        }
        if (iconButton(shadingIcon, shadingTip, m_ShadingMode != ShadingMode::Shaded)) {
            m_ShadingMode = m_ShadingMode == ShadingMode::Shaded ? ShadingMode::Wireframe
                : (m_ShadingMode == ShadingMode::Wireframe ? ShadingMode::Unlit : ShadingMode::Shaded);
        }
    }

    ImGui::SameLine();
    // Orthographic / perspective toggle (shortcut 5) — was menu-only; it has a distinct on/off
    // state so it belongs on the strip beside the shading mode (#148).
    if (iconButton(ICON_FA_BORDER_ALL,
            editorCamera.Orthographic ? "Orthographic (click for Perspective) — 5"
                                      : "Perspective (click for Orthographic) — 5",
            editorCamera.Orthographic)) {
        ToggleOrthographic(world, editorCamera);
    }

    divider();
    if (iconButton(ICON_FA_CHART_SIMPLE, "Toggle Statistics", EditorSettings::Get().SceneShowStats)) {
        EditorSettings::Get().SceneShowStats = !EditorSettings::Get().SceneShowStats;
        EditorSettings::Save();
    }
    ImGui::SameLine();
    if (iconButton(ICON_FA_TERMINAL, "Toggle Console", m_ShowConsole)) m_ShowConsole = !m_ShowConsole;
    ImGui::SameLine();
    if (iconButton(ICON_FA_CLOCK_ROTATE_LEFT, "Toggle History", m_ShowHistory)) m_ShowHistory = !m_ShowHistory;

    divider();
    // Capture: click = shoot with the current settings; the caret opens the options popup.
    {
        auto& cs = EditorSettings::Get();
        static const char* kModes[] = { "Full editor window", "Scene viewport", "Scene viewport (clean)", "Game view" };
        const int cm = std::clamp(cs.CaptureMode, 0, 3);
        char tip[128];
        snprintf(tip, sizeof(tip), "Capture screenshot — %s%s (Print Screen)",
                 kModes[cm], (cm == 1 || cm == 2) && cs.CaptureScale > 1 ? " x2+" : "");
        if (iconButton(ICON_FA_CAMERA_RETRO, tip)) RequestCapture();
        ImGui::SameLine(0.0f, 1.0f);
        ImGui::PushID("##capOpts");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_CARET_DOWN)) ImGui::OpenPopup("##CapturePopup");
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Capture options");
        if (ImGui::BeginPopup("##CapturePopup")) {
            ImGui::PushItemWidth(150.0f * m_UIScale);
            ImGui::TextDisabled("Capture");
            if (ImGui::Combo("Mode", &cs.CaptureMode, kModes, IM_ARRAYSIZE(kModes))) EditorSettings::Save();
            const bool viewportMode = (cs.CaptureMode == 1 || cs.CaptureMode == 2);
            ImGui::BeginDisabled(!viewportMode);
            const char* kResLabels[IM_ARRAYSIZE(kCaptureRes)];
            for (int i = 0; i < IM_ARRAYSIZE(kCaptureRes); ++i) kResLabels[i] = kCaptureRes[i].label;
            if (ImGui::Combo("Resolution", &cs.CaptureResPreset, kResLabels, IM_ARRAYSIZE(kResLabels)))
                EditorSettings::Save();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!viewportMode || cs.CaptureResPreset != 0);
            static const char* kScales[] = { "1x", "2x", "4x" };
            int si = cs.CaptureScale >= 4 ? 2 : (cs.CaptureScale >= 2 ? 1 : 0);
            if (ImGui::Combo("Supersample", &si, kScales, IM_ARRAYSIZE(kScales))) {
                cs.CaptureScale = si == 2 ? 4 : (si == 1 ? 2 : 1); EditorSettings::Save();
            }
            ImGui::EndDisabled();
            static const char* kFmt[] = { "PNG", "JPG" };
            if (ImGui::Combo("Format", &cs.CaptureFormat, kFmt, IM_ARRAYSIZE(kFmt))) EditorSettings::Save();
            if (ImGui::Checkbox("Flash", &cs.CaptureFlash)) EditorSettings::Save();
            ImGui::SameLine();
            if (ImGui::Checkbox("Sound", &cs.CaptureSound)) EditorSettings::Save();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CAMERA_RETRO "  Capture now")) { RequestCapture(); ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open screenshots folder"))
                Screenshot::ShowInFolder(m_LastCapturePath.empty() ? Screenshot::Dir() : m_LastCapturePath);
            ImGui::PopItemWidth();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }


    // The toolbar's empty space is the window drag handle (the OS caption is gone). True only
    // when the cursor is over this strip and not over any widget / open menu / active drag —
    // Window.cpp's WM_NCHITTEST reads this to return HTCAPTION.
    m_TitleBarDragHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
}

// Minimize / maximize-restore / close, right-aligned in the toolbar's menu-bar row. Operates
// directly on the GLFW window; the visual frame removal happens in Window.cpp.
void EditorLayer::DrawWindowControls() {
    const float h = ImGui::GetFrameHeight();
    const float bw = std::floor(h * 1.6f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float startX = ImGui::GetWindowWidth() - bw * 3.0f - st.WindowPadding.x;
    ImGui::SameLine(startX > 0.0f ? startX : 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, st.ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));

    ImGui::PushID("win_min");
    if (ImGui::Button(ICON_FA_MINUS, ImVec2(bw, 0.0f))) glfwIconifyWindow(m_Window);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Minimize");
    ImGui::PopID();
    ImGui::SameLine();

    const bool maxed = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) != 0;
    ImGui::PushID("win_max");
    if (ImGui::Button(maxed ? ICON_FA_COMPRESS : ICON_FA_EXPAND, ImVec2(bw, 0.0f))) {
        if (maxed) glfwRestoreWindow(m_Window); else glfwMaximizeWindow(m_Window);
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(maxed ? "Restore" : "Maximize");
    ImGui::PopID();
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.15f, 0.18f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.12f, 0.15f, 1.0f));
    ImGui::PushID("win_close");
    if (ImGui::Button(ICON_FA_XMARK, ImVec2(bw, 0.0f))) glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Close");
    ImGui::PopID();
    ImGui::PopStyleColor(2);

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
}

void EditorLayer::DrawHierarchy(World& world, AssetLibrary& assets) {
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (!ImGui::Begin("Scene Hierarchy", &m_ShowHierarchy, flags)) { ImGui::End(); return; }

    // Accumulates as each row is drawn (DrawHierarchyNode); published to m_HierarchyVisibleOrder
    // just before this function returns. See the header for why the two buffers are separate.
    m_HierarchyVisibleBuild.clear();

    // Expand / collapse every root's whole subtree at once (audit #71). Two flat glyph buttons
    // pinned to the right of the search row (#151) instead of a whole dedicated button row.
    auto setAllExpanded = [&](bool open) {
        for (auto e : world.Registry.view<const NameComponent>()) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(e);
            if (!h || h->Parent == entt::null) SetHierarchyExpandedRecursive(world, e, open);
        }
    };
    auto flatGlyphButton = [](const char* icon, const char* tip) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // flat at rest
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.245f, 0.250f, 0.275f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.300f, 0.310f, 0.345f, 1.0f));
        ImGui::PushID(tip);
        bool clicked = ImGui::Button(icon);
        ImGui::PopID();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
        return clicked;
    };

    // Search box. A bare string matches names; the "t:" prefix matches TagComponent instead,
    // the same shorthand Unity's Hierarchy search uses. Width leaves room for the two glyph
    // buttons + the spacing on either side of them.
    const ImGuiStyle& hstyle = ImGui::GetStyle();
    const float glyphBtnW = ImGui::CalcTextSize(ICON_FA_ANGLES_UP).x + hstyle.FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-(glyphBtnW * 2.0f + hstyle.ItemSpacing.x * 2.0f));
    char filterBuf[128];
    snprintf(filterBuf, sizeof(filterBuf), "%s", m_HierarchyFilter.c_str());
    if (ImGui::InputTextWithHint("##HierarchyFilter", ICON_FA_MAGNIFYING_GLASS "  Search (t:Tag to filter by tag)",
            filterBuf, sizeof(filterBuf))) {
        m_HierarchyFilter = filterBuf;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        EditorUI::SetTooltip("Type a name to filter the list below.\nType \"t:\" followed by a tag (e.g. t:Enemy) to filter by Tag instead.");
    }

    ImGui::SameLine();
    if (flatGlyphButton(ICON_FA_ANGLES_DOWN, "Expand all")) setAllExpanded(true);
    ImGui::SameLine();
    if (flatGlyphButton(ICON_FA_ANGLES_UP, "Collapse all")) setAllExpanded(false);

    const bool filtering = !m_HierarchyFilter.empty();

    // One flat list — every entity in creation order, no "Level Geometry" / "Objects" split.
    // A box, a model, a light and an empty are all just entities; the old grouping only ever
    // added a header to scroll past. LevelGeometryTag survives as an invisible serialization /
    // built-in-collider detail, nothing the Hierarchy shows.
    //
    // Tighter per-level indent than the editor-wide default — this panel is narrow, so a few
    // levels of nesting otherwise push names off the right edge fast (#153).
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 13.0f * m_UIScale);
    for (auto entity : ViewInCreationOrder(world.Registry, world.Registry.view<const NameComponent>())) {
        if (!MatchesHierarchyFilter(world, entity)) continue;
        // A parented entity draws nested under its parent, not as a sibling — except while
        // filtering, where the parent may be filtered out, so matches are shown flat instead.
        if (!filtering) {
            const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
            if (hier && hier->Parent != entt::null) continue;
        }
        DrawHierarchyNode(world, assets, entity, world.Registry.all_of<LevelGeometryTag>(entity));
    }
    ImGui::PopStyleVar();

    // Dropping onto empty space below the tree un-parents (Unity's "drag to the root") — and
    // right-clicking there opens the create/paste menu.
    // Dummy needs a real size (negative width isn't valid here), so this claims whatever space
    // is left below the tree as one big drop/right-click zone.
    ImVec2 remaining = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(remaining.x > 0.0f ? remaining.x : 1.0f, remaining.y > 0.0f ? remaining.y : 1.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            entt::entity dragged = *(const entt::entity*)payload->Data;
            if (world.Registry.valid(dragged)) {
                PushUndo(world, "Reparent");
                world.SetParent(dragged, entt::null);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##HierarchyEmptyContext")) {
        DrawHierarchyContextMenu(world, assets, entt::null);
        ImGui::EndPopup();
    }

    // Publish the row order this pass built, for next frame's click handlers (and the Ctrl+A
    // check just below, which runs after every row is in).
    m_HierarchyVisibleOrder = m_HierarchyVisibleBuild;

    // Ctrl+A — select every visible row, matching Unity's Hierarchy shortcut. Gated on the
    // panel (or one of its children) being focused, and skipped while a text field here has
    // the keyboard (so Ctrl+A still means "select all text" in the search box).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        SelectAllVisibleInHierarchy();
    }

    ImGui::End();
}

bool EditorLayer::MatchesHierarchyFilter(const World& world, entt::entity entity) const {
    if (m_HierarchyFilter.empty()) return true;

    if (m_HierarchyFilter.rfind("t:", 0) == 0) {
        std::string wanted = m_HierarchyFilter.substr(2);
        if (wanted.empty()) return true;
        const auto* tag = world.Registry.try_get<TagComponent>(entity);
        return MatchesFilter(wanted, tag ? tag->Tag : std::string("Untagged"));
    }

    const auto* name = world.Registry.try_get<NameComponent>(entity);
    return MatchesFilter(m_HierarchyFilter, name ? name->Name : std::string());
}

void EditorLayer::DrawHierarchyNode(World& world, AssetLibrary& assets, entt::entity entity, bool isLevelGeometry) {
    if (!world.Registry.valid(entity)) return;

    // This row is about to be drawn — record it in visible top-to-bottom order for Ctrl+A and
    // Shift+Click. Children append themselves in the recursive calls below, and only when this
    // node is expanded, so the list mirrors exactly what the user sees.
    m_HierarchyVisibleBuild.push_back(entity);

    auto& name = world.Registry.get<NameComponent>(entity);
    bool selected = IsSelected(entity);
    bool inactive = world.Registry.all_of<InactiveTag>(entity);
    const auto* hier = world.Registry.try_get<HierarchyComponent>(entity);
    bool hasChildren = hier && !hier->Children.empty() && m_HierarchyFilter.empty();

    ImGui::PushID((int)entt::to_integral(entity));

    // Leading eye toggle = Unity's active checkbox (#152: shared ActiveToggle). Drawn before the
    // row so clicking it never also changes the selection; hover-reveals on the whole row so an
    // active object carries no per-row chrome at rest.
    {
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ImGui::GetFrameHeight());
        const bool rowHovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);
        if (ActiveToggle("##rowactive", !inactive, rowHovered,
                         inactive ? "Inactive - click to enable" : "Active - click to disable",
                         /*alignTop=*/true)) {
            PushUndo(world, "Toggle Active");
            if (inactive) world.Registry.remove<InactiveTag>(entity);
            else world.Registry.emplace<InactiveTag>(entity);
        }
    }
    // Tight against the tree node — the node's own arrow gap already separates the eye from the
    // kind glyph, so the default ItemSpacing here just reads as a hole (#152 follow-up).
    ImGui::SameLine(0.0f, 2.0f);

    // Inline rename (F2 / context menu) replaces the row with an edit field in place.
    if (m_RenamingEntity == entity) {
        ImGui::SetNextItemWidth(-1.0f);
        if (m_EntityRenameJustStarted) {
            ImGui::SetKeyboardFocusHere();
            snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s", name.Name.c_str());
            m_EntityRenameJustStarted = false;
        }
        if (ImGui::InputText("##Rename", m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            PushUndo(world, "Rename");
            name.Name = SanitizeEntityName(m_EntityRenameBuffer);
            m_RenamingEntity = entt::null;
        }
        if (ImGui::IsItemDeactivated()) m_RenamingEntity = entt::null;
        ImGui::PopID();
        return; // children stay collapsed for the one frame a rename is open — deliberate, keeps the field stable
    }

    // Kind badge: ONE primary glyph (mesh > light > camera > empty priority) drawn in a fixed-
    // width leading slot so a name's left edge is identical on every row no matter how many kinds
    // it has (#153). A mesh that also carries a light still reads as both — the second kind shows
    // as a small badge inside that same slot (#27 P16) — without shifting the name; a rare third
    // kind is dropped (the Inspector lists every component anyway).
    const bool hasMesh   = world.Registry.all_of<RenderableComponent>(entity);
    const bool hasLight  = world.Registry.all_of<LightComponent>(entity);
    const bool hasCamera = world.Registry.all_of<CameraComponent>(entity);
    const char* primaryGlyph =
        hasMesh   ? ICON_FA_DRAW_POLYGON  :
        hasLight  ? ICON_FA_LIGHTBULB     :
        hasCamera ? ICON_FA_VIDEO         : ICON_FA_DIAGRAM_PROJECT;
    const char* secondaryGlyph =
        (hasMesh && hasLight)   ? ICON_FA_LIGHTBULB :
        (hasMesh && hasCamera)  ? ICON_FA_VIDEO     :
        (hasLight && hasCamera) ? ICON_FA_VIDEO     : nullptr;

    // Never-named entities get a positional fallback instead of a wall of identical
    // "(unnamed)" rows (#21 P10).
    std::string shownName = name.Name;
    if (shownName.empty())
        shownName = "Object " + std::to_string(CreationOrdinal(world.Registry, entity));

    // Only feeds the drag preview below — the row paints its own glyph + name after the node.
    std::string label = std::string(primaryGlyph) + "  " + shownName;

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
        (selected ? ImGuiTreeNodeFlags_Selected : 0) |
        (hasChildren ? ImGuiTreeNodeFlags_DefaultOpen : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen));

    // Empty label: the tree node owns the arrow / indent / full-row hitbox / open-close and every
    // interaction handler below; the glyph slot + name are painted afterward at a constant X.
    bool open = ImGui::TreeNodeEx("##node", nodeFlags, "%s", "");
    const ImVec2 rowMin = ImGui::GetItemRectMin();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift) {
            // Shift (or Ctrl+Shift) — contiguous range from the anchor to this row.
            SelectHierarchyRange(world, entity, io.KeyCtrl);
        } else {
            SelectItem(entity, io.KeyCtrl); // plain click replaces; Ctrl+Click toggles. Both re-anchor.
        }
        m_HierarchyRowHintDone = true; // learned the row interaction — stop showing the hint
    }
    if (ImGui::IsItemToggledOpen() && ImGui::GetIO().KeyAlt && hasChildren) {
        // `open` already reflects the state ImGui just toggled this entity's own row to —
        // cascade that same state to every descendant.
        for (entt::entity child : hier->Children) {
            SetHierarchyExpandedRecursive(world, child, open);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        BeginRenameEntity(entity);
    }
    if (!m_HierarchyRowHintDone && ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        EditorUI::SetTooltip("Click to select (Ctrl+Click to add/remove, Shift+Click for a range, Ctrl+A for all).\nDouble-click or F2 to rename. Drag onto another row to parent it.\nRight-click for more options.");
    }

    if (ImGui::BeginPopupContextItem("##RowContext")) {
        if (!IsSelected(entity)) SelectItem(entity, false);
        DrawHierarchyContextMenu(world, assets, entity);
        ImGui::EndPopup();
    }

    // Drag a row onto another row to re-parent it (Unity's core Hierarchy gesture).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(entt::entity));
        ImGui::Text("%s", label.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
            entt::entity dragged = *(const entt::entity*)payload->Data;
            if (world.Registry.valid(dragged) && dragged != entity) {
                PushUndo(world, "Reparent");
                // SetParent refuses cycles and Collider-bearing children on its own; report the
                // refusal rather than silently doing nothing, so the gesture never looks broken.
                if (!world.SetParent(dragged, entity)) {
                    Log::Warn("Can't parent that: level geometry has a collider that needs world-space "
                              "coordinates, or the target is already a child of the dragged object.");
                }
            }
        }
        // Existing behavior: dropping a texture from the Asset Browser assigns it as Albedo.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
            std::string texPath((const char*)payload->Data);
            if (auto* renderable = world.Registry.try_get<RenderableComponent>(entity)) {
                PushUndo(world, "Set Albedo Map");
                Model* model = renderable->ModelRef.get();
                auto override_ = model->MaterialOverride();
                if (!override_) {
                    override_ = std::make_shared<Material>();
                    if (model->MeshCount() > 0) {
                        const Material& imported = model->MeshMaterial(0);
                        override_->NormalMap = imported.NormalMap;
                        override_->MetallicMap = imported.MetallicMap;
                        override_->RoughnessMap = imported.RoughnessMap;
                        override_->AOMap = imported.AOMap;
                        override_->EmissiveMap = imported.EmissiveMap;
                    }
                    model->SetMaterialOverride(override_);
                }
                override_->AlbedoMap = assets.LoadTexture(texPath);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Paint the kind glyph(s) + name at a fixed X off the row's left edge — see the primaryGlyph
    // comment above. Drawn straight into the window draw list so it never becomes "the last item"
    // and disturb the interaction handlers, selection rect or drag/drop that key off the node.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fontSize = ImGui::GetFontSize();
        const float slotW    = fontSize * 1.45f;
        // Pull the kind glyph + name in toward the eye (#152 follow-up). Parent rows keep enough
        // lead for the disclosure arrow; leaf rows (no arrow) only need a hair of separation.
        const float labelX   = rowMin.x + (hasChildren ? fontSize * 1.15f : fontSize * 0.35f);
        const ImU32 col = ImGui::GetColorU32(inactive ? ImGuiCol_TextDisabled : ImGuiCol_Text);
        dl->AddText(ImVec2(labelX, rowMin.y), col, primaryGlyph);
        if (secondaryGlyph) {
            const float sub = fontSize * 0.68f;
            dl->AddText(ImGui::GetFont(), sub,
                        ImVec2(labelX + slotW - sub, rowMin.y + fontSize - sub),
                        (col & 0x00FFFFFFu) | 0x9E000000u, secondaryGlyph);
        }
        dl->AddText(ImVec2(labelX + slotW, rowMin.y), col, shownName.c_str());
    }

    if (hasChildren && open) {
        // Copied because a re-parent or delete triggered from a child's own context menu would
        // otherwise mutate this vector mid-iteration.
        std::vector<entt::entity> children = hier->Children;
        for (entt::entity child : children) {
            if (world.Registry.valid(child)) DrawHierarchyNode(world, assets, child, isLevelGeometry);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void EditorLayer::SetHierarchyExpandedRecursive(World& world, entt::entity entity, bool open) {
    if (!world.Registry.valid(entity)) return;
    ImGui::PushID((int)entt::to_integral(entity));
    ImGuiID nodeId = ImGui::GetID("##node");
    ImGui::GetStateStorage()->SetInt(nodeId, open ? 1 : 0);
    if (const auto* hier = world.Registry.try_get<HierarchyComponent>(entity)) {
        for (entt::entity child : hier->Children) {
            SetHierarchyExpandedRecursive(world, child, open);
        }
    }
    ImGui::PopID();
}

void EditorLayer::DrawHierarchyContextMenu(World& world, AssetLibrary& assets, entt::entity entity) {
    bool hasEntity = entity != entt::null && world.Registry.valid(entity);

    if (ImGui::BeginMenu(ICON_FA_PLUS "  Create")) {
        if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Empty")) CreateEmptyAt(world, nullptr, "Empty", false);
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light")) CreateEmptyAt(world, nullptr, "Point Light", true);
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Spot Light")) {
            world.Registry.get<LightComponent>(CreateEmptyAt(world, nullptr, "Spot Light", true)).Kind = LightComponent::Type::Spot;
        }
        if (ImGui::MenuItem(ICON_FA_SUN "  Directional Light")) {
            MakeDirectionalLight(world, CreateEmptyAt(world, nullptr, "Directional Light", true));
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Group into Empty Parent", nullptr, false, HasAnySelection())) {
        CreateEmptyParentForSelection(world);
    }
    if (ImGui::IsItemHovered() && HasAnySelection()) {
        EditorUI::SetTooltip("Create a new Empty at the selection's center and parent every\nselected object under it. Positions are preserved.");
    }
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, hasEntity)) CopySelection(world);
    if (ImGui::MenuItem(ICON_FA_SCISSORS "  Cut", "Ctrl+X", false, hasEntity)) {
        CopySelection(world);
        DeleteSelection(world);
    }
    if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, !m_Clipboard.empty())) {
        PasteClipboard(world, assets);
    }
    if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", "Ctrl+D", false, hasEntity)) {
        DuplicateSelection(world, assets);
    }

    const bool isLight = hasEntity && world.Registry.all_of<LightComponent>(entity);
    if (isLight) {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_DOWN_LONG "  Drop Light to Surface")) {
            if (!DropLightToSurface(world, entity))
                Log::Info("Drop to surface: nothing directly below this light.");
        }
    }
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_PEN "  Rename", "F2", false, hasEntity)) BeginRenameEntity(entity);
    if (ImGui::MenuItem(ICON_FA_BOX_ARCHIVE "  Save as Prefab...", nullptr, false, hasEntity)) {
        std::string path = FileDialog::SaveFile("Prefab Files\0*.prefab\0All Files\0*.*\0", "prefab", m_Window);
        if (!path.empty() && SceneSerializer::SavePrefab(world, entity, path)) {
            assets.RegisterPrefab(path);
        }
    }
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del", false, hasEntity)) DeleteSelection(world);
}

void EditorLayer::BeginRenameEntity(entt::entity entity) {
    if (entity == entt::null) return;
    m_RenamingEntity = entity;
    m_EntityRenameJustStarted = true;
}

void EditorLayer::CreateEmptyParentForSelection(World& world) {
    std::vector<entt::entity> sel = GetSelectedItems();
    sel.erase(std::remove_if(sel.begin(), sel.end(),
        [&](entt::entity e) { return !world.Registry.valid(e); }), sel.end());
    if (sel.empty()) return;

    // Nothing with a Box Collider can be re-parented yet (SetParent refuses it) — check up front
    // so we don't create a stray "Group" empty that ends up with no children.
    bool anyReparentable = false;
    for (entt::entity e : sel) {
        if (!world.Registry.all_of<ColliderComponent>(e)) { anyReparentable = true; break; }
    }
    if (!anyReparentable) {
        Log::Warn("Couldn't group: objects with a Box Collider can't be re-parented yet.");
        return;
    }

    glm::vec3 center(0.0f);
    if (!GetSelectionCenter(world, center)) center = glm::vec3(0.0f);

    PushUndo(world, "Create Empty Parent");
    entt::entity parent = world.CreateEmptyEntity(center, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, "Group"));

    // If every selected entity already shares one parent, slot the new group in under it so the
    // grouping doesn't yank the objects to the scene root.
    entt::entity commonParent = entt::null;
    bool sameParent = true;
    for (size_t i = 0; i < sel.size(); ++i) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(sel[i]);
        entt::entity p = h ? h->Parent : entt::null;
        if (i == 0) commonParent = p;
        else if (p != commonParent) { sameParent = false; break; }
    }
    if (sameParent && commonParent != entt::null) world.SetParent(parent, commonParent);

    int parented = 0;
    for (entt::entity e : sel) {
        if (world.SetParent(e, parent)) ++parented; // preserves world transform; refuses colliders
    }
    SelectItem(parent, false);
    Log::Info("Grouped " + std::to_string(parented) + " object(s) under a new Empty" +
              (parented < (int)sel.size() ? " (some couldn't be re-parented)." : "."));
}

entt::entity EditorLayer::CreateEmptyAt(World& world, Camera* editorCamera, const char* name, bool asLight) {
    PushUndo(world, std::string("Create ") + name);
    // Spawned in front of the camera when there is one (menu invoked from the viewport/toolbar),
    // else at the origin — the Hierarchy's own context menu has no camera to reference.
    glm::vec3 position = editorCamera ? SafeSpawnInFrontOf(*editorCamera) : glm::vec3(0.0f);
    entt::entity e = world.CreateEmptyEntity(position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, name));
    if (asLight) world.Registry.emplace<LightComponent>(e);
    SelectItem(e, false);
    Log::Info(std::string("Added ") + name + ".");
    return e;
}

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
                InvalidateModelThumbnail(nullptr); // re-render the Asset Browser preview
                m_ImportSettingsDirty = false;
            },
            [&]() {
                m_PendingModelSettings = assets.GetModelSettings(key);
                m_ImportSettingsDirty = false;
            });
    }
}

void EditorLayer::DrawInspector(World& world, AssetLibrary& assets, float dt) {
    (void)dt; // not needed today; kept for parity with the other Draw* panel signatures
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (!ImGui::Begin("Inspector", &m_ShowInspector, flags)) { ImGui::End(); return; }

    // Flat button language for the whole panel (#155/#156): no raised body at rest, a faint wash
    // on hover. Every ImGui::Button below inherits it; ActionButton / DangerIconButton push their
    // own on top. Popped before each ImGui::End() via InspectorEnd().
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    auto InspectorEnd = []() { ImGui::PopStyleColor(3); ImGui::End(); };

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
             allAudio = true, allAnimator = true;
        forEach([&](entt::entity e) {
            allMesh     &= world.Registry.all_of<RenderableComponent>(e);
            allLight    &= world.Registry.all_of<LightComponent>(e);
            allCamera   &= world.Registry.all_of<CameraComponent>(e);
            allCollider &= world.Registry.all_of<ColliderComponent>(e);
            allAudio    &= world.Registry.all_of<AudioSourceComponent>(e);
            allAnimator &= world.Registry.all_of<AnimatorComponent>(e);
        });
        {
            std::string common = "Transform";
            if (allMesh)     common += ", Mesh Renderer";
            if (allLight)    common += ", Light";
            if (allCamera)   common += ", Camera";
            if (allCollider) common += ", Box Collider";
            if (allAudio)    common += ", Audio Source";
            if (allAnimator) common += ", Animator";
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

        transformRow("Position", 0.05f, 0.0f, 0.0f,
            [](TransformComponent& t) -> glm::vec3& { return t.Position; },
            "Sets X / Y / Z on every selected object.", "Set Position");
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

        // ===== Material / PBR (only when every selected object has a mesh) =====
        if (allMesh) {
            ImGui::Spacing();
            bool matOpen = BeginComponentSection(ICON_FA_PALETTE, "Material", false, mrm,
                /*defaultOpen=*/true,
                "Shared PBR material and texture maps for every selected mesh.\n"
                "A field showing \xE2\x80\x94 differs across the selection.");
            if (matOpen) {
                DrawMultiMaterialEditor(world, assets, sel);
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
        DrawVec3Row("Position", transform.Position, 0.1f, 0.0f, 0.0f, rowActive, rowCommitted,
            "World-space position in units. Drag a number to change it, or\nclick a colored letter to zero that axis.");
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
            DrawMaterialEditor(world, assets);
            EndComponentSection();
        }
    }

    // --- Collider (collapsed by default: a single checkbox, rarely revisited) --------------
    if (auto* collider = registry.try_get<ColliderComponent>(entity)) {
        if (BeginComponentSection(ICON_FA_CUBE, "Box Collider", true, removed, /*defaultOpen=*/false,
                "Lets this object block movement and be hit by raycasts.\nBounds follow its Transform/mesh automatically.")) {
            PropertyLabel("Is Trigger", "If checked, this object doesn't block movement -\nit's solid (blocking) by default.");
            if (ImGui::Checkbox("##IsTrigger", &collider->IsTrigger)) PushUndo(world, "Edit Collider");
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
            EndComponentSection();
        }
        if (removed) {
            PushUndo(world, "Remove Audio Source");
            registry.remove<AudioSourceComponent>(entity);
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
    {
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
    // Indenting the body is what visually reads as "these fields belong to that header" instead
    // of every section's fields sitting flush with the header bars themselves — paired with
    // EndComponentSection(), which callers must call whenever this returns true.
    if (showBody) ImGui::Indent();
    return showBody;
}

void EditorLayer::EndComponentSection() {
    ImGui::Unindent();
    ImGui::Spacing();
}

bool EditorLayer::AnyModalOpen() const {
    return ImGui::GetTopMostPopupModal() != nullptr;
}

void EditorLayer::RequestCapture() {
    const auto& s = EditorSettings::Get();
    const int rp = std::clamp(s.CaptureResPreset, 0, (int)IM_ARRAYSIZE(kCaptureRes) - 1);
    m_CaptureReq = { /*pending*/ true, /*primed*/ false, s.CaptureMode, std::max(1, s.CaptureScale),
                     s.CaptureFormat, kCaptureRes[rp].w, kCaptureRes[rp].h };
}

void EditorLayer::OnCaptureDone(const std::string& path, int w, int h) {
    m_LastCapturePath = path;
    if (path.empty()) { m_CaptureToast = "Screenshot failed — see Console"; m_CaptureToastT = 3.0f; return; }
    const auto& s = EditorSettings::Get();
    if (s.CaptureFlash) m_CaptureFlashT = 1.0f;
    if (s.CaptureSound) {
        std::string clip = Screenshot::ShutterClipPath();
        if (!clip.empty()) AudioEngine::Play(clip, 0.6f);
    }
    m_CaptureToast = std::filesystem::path(path).filename().string() + "   " +
                     std::to_string(w) + "x" + std::to_string(h);
    m_CaptureToastT = 3.5f;
}

// Called once per frame from Draw() — the fading white flash over the Scene viewport and the
// little "saved" toast in its bottom-right.
void EditorLayer::DrawCaptureFeedback(float dt) {
    if (m_CaptureFlashT <= 0.0f && m_CaptureToastT <= 0.0f) return;
    if (m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* dl = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 mn(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 mx(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    dl->PushClipRect(mn, mx, true);

    if (m_CaptureFlashT > 0.0f) {
        m_CaptureFlashT -= dt / 0.35f;
        float a = m_CaptureFlashT;
        a = a < 0.0f ? 0.0f : a * a; // ease out
        dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, (int)(a * 220.0f)));
    }
    if (m_CaptureToastT > 0.0f) {
        m_CaptureToastT -= dt;
        float a = m_CaptureToastT > 0.4f ? 1.0f : m_CaptureToastT / 0.4f;
        const ImVec2 ts = ImGui::CalcTextSize(m_CaptureToast.c_str());
        const float pad = 8.0f * m_UIScale;
        ImVec2 p1(mx.x - ts.x - pad * 2.0f - 16.0f * m_UIScale, mx.y - ts.y - pad * 2.0f - 16.0f * m_UIScale);
        ImVec2 p2(mx.x - 16.0f * m_UIScale, mx.y - 16.0f * m_UIScale);
        dl->AddRectFilled(p1, p2, IM_COL32(20, 20, 24, (int)(a * 220.0f)), 4.0f);
        dl->AddText(ImVec2(p1.x + pad, p1.y + pad), IM_COL32(235, 238, 245, (int)(a * 255.0f)), m_CaptureToast.c_str());
    }
    dl->PopClipRect();
}

// A screenshot on disk already holds display-encoded (sRGB) bytes. Load it as literal color
// (IsSRGB = false) so the Asset Browser thumbnail and the lightbox render it at true brightness
// — the default sRGB path hardware-linearizes on sample, which is right for a 3D albedo map but
// visibly darkens a UI image.
static std::shared_ptr<Texture> LoadScreenshotTexture(const std::string& path) {
    TextureImportSettings s;
    s.IsSRGB = false;
    s.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
    return std::make_shared<Texture>(path, s);
}

void EditorLayer::OpenScreenshotPreview(const std::string& path) {
    m_ShotPreviewPath = path;
    m_ShotPreviewTex.reset();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto tex = LoadScreenshotTexture(path);
        if (tex->IsValid()) m_ShotPreviewTex = std::move(tex);
    }
    if (m_ShotPreviewTex) {
        m_ShotPreviewOpen = true;
        m_ShotPreviewAnim = 0.0f;
        m_ShotPreviewZoom = 1.0f;
        m_ShotPreviewPan = ImVec2(0.0f, 0.0f);
        m_ShotPreviewPanning = false;
        m_ShotPreviewPressOutside = false; // stale from a previous close would insta-dismiss
    } else {
        // Couldn't decode it here — fall back to the OS viewer rather than an empty window.
        m_ShotPreviewPath.clear();
        Screenshot::ShowInFolder(path);
    }
}

// A centred, chrome-light lightbox for a saved screenshot. The backdrop gets the same frosted
// blur as the delete-confirmation modal (EndFrame() blurs whatever's behind the top-most modal),
// so no extra dim is applied here. The window is freely movable and resizable; over the image,
// the scroll wheel zooms about the cursor and left-drag pans. Esc / X / a backdrop click closes.
void EditorLayer::DrawScreenshotPreview() {
    if (!m_ShotPreviewOpen) return;

    if (!ImGui::IsPopupOpen("##ShotPreview")) ImGui::OpenPopup("##ShotPreview");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Texture* tex = m_ShotPreviewTex.get();
    const float iw = (tex && tex->Width()  > 0) ? (float)tex->Width()  : 1.0f;
    const float ih = (tex && tex->Height() > 0) ? (float)tex->Height() : 1.0f;

    // First-show size: fit the native image into ~3/4 of the work area (never upscaled past 1:1),
    // plus room for padding and the header row. After that the user owns the window size.
    float initFit = std::min((vp->WorkSize.x * 0.74f) / iw, (vp->WorkSize.y * 0.80f) / ih);
    initFit = std::clamp(initFit, 0.05f, m_UIScale);
    const ImVec2 initSize(iw * initFit + 28.0f * m_UIScale, ih * initFit + 66.0f * m_UIScale);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(initSize, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * m_UIScale, 220.0f * m_UIScale),
                                        ImVec2(FLT_MAX, FLT_MAX));

    m_ShotPreviewAnim += (1.0f - m_ShotPreviewAnim) * 0.30f;
    if (m_ShotPreviewAnim > 0.999f) m_ShotPreviewAnim = 1.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * m_ShotPreviewAnim);

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginPopupModal("##ShotPreview", &open, flags)) {
        // Slim header: filename + native size + current zoom, then icon actions pinned right.
        const std::string name = std::filesystem::path(m_ShotPreviewPath).filename().string();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d", (int)iw, (int)ih);
        ImGui::SameLine();
        ImGui::TextDisabled("\xE2\x80\xA2  %.0f%%", m_ShotPreviewZoom * 100.0f);

        const float btnW = 26.0f * m_UIScale;
        float rightX = ImGui::GetContentRegionMax().x - btnW * 2.0f - 6.0f;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
        else ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN, ImVec2(btnW, 0.0f))) Screenshot::ShowInFolder(m_ShotPreviewPath);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show in folder");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_XMARK, ImVec2(btnW, 0.0f))) { open = false; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Close (Esc)");
        ImGui::PopStyleColor(2);

        ImGui::Spacing();

        // The image canvas fills the rest of the window. An InvisibleButton over it captures
        // hover / drag without turning the image into a "widget".
        ImVec2 canvas = ImGui::GetContentRegionAvail();
        canvas.x = std::max(canvas.x, 32.0f);
        canvas.y = std::max(canvas.y, 32.0f);
        const ImVec2 cpos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##shotcanvas", canvas,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool canvasHovered = ImGui::IsItemHovered();
        const bool canvasActive  = ImGui::IsItemActive();

        const float fit = std::min(canvas.x / iw, canvas.y / ih); // image fits canvas at zoom 1

        // Scroll wheel -> zoom about the cursor (keep the point under the pointer anchored).
        if (canvasHovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const float prev = m_ShotPreviewZoom;
                m_ShotPreviewZoom = std::clamp(m_ShotPreviewZoom * std::pow(1.15f, wheel), 0.1f, 16.0f);
                const float k = m_ShotPreviewZoom / prev;
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const ImVec2 imgCtr(cpos.x + canvas.x * 0.5f + m_ShotPreviewPan.x,
                                    cpos.y + canvas.y * 0.5f + m_ShotPreviewPan.y);
                m_ShotPreviewPan.x += (imgCtr.x - mouse.x) * (k - 1.0f);
                m_ShotPreviewPan.y += (imgCtr.y - mouse.y) * (k - 1.0f);
            }
        }
        // Left / middle drag pans.
        if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_ShotPreviewPan.x += d.x;
            m_ShotPreviewPan.y += d.y;
            m_ShotPreviewPanning = true;
        } else {
            m_ShotPreviewPanning = false;
        }
        // Double-click resets to fit.
        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_ShotPreviewZoom = 1.0f;
            m_ShotPreviewPan = ImVec2(0.0f, 0.0f);
        }
        if (canvasHovered && !m_ShotPreviewPanning)
            EditorUI::SetTooltip("Scroll to zoom  \xE2\x80\xA2  drag to pan  \xE2\x80\xA2  double-click to reset");

        const float dispW = iw * fit * m_ShotPreviewZoom;
        const float dispH = ih * fit * m_ShotPreviewZoom;
        const ImVec2 ic(cpos.x + canvas.x * 0.5f + m_ShotPreviewPan.x,
                        cpos.y + canvas.y * 0.5f + m_ShotPreviewPan.y);
        const ImVec2 a(ic.x - dispW * 0.5f, ic.y - dispH * 0.5f);
        const ImVec2 b(a.x + dispW, a.y + dispH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cpos, ImVec2(cpos.x + canvas.x, cpos.y + canvas.y), true);
        if (tex && tex->IsValid())
            dl->AddImage((ImTextureID)(intptr_t)tex->GLHandle(), a, b);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 26));
        dl->PopClipRect();

        // Backdrop dismissal: close only on a press-and-release that BOTH land on the dimmed
        // area outside the window. Tracking the press origin keeps a resize-grip drag (which
        // starts on the window edge and can wander outside) or a pan from closing the lightbox.
        const bool overWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_ShotPreviewPressOutside = !overWindow;
        if (m_ShotPreviewAnim > 0.5f && m_ShotPreviewPressOutside && !canvasActive &&
            !ImGui::IsAnyItemActive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !overWindow) {
            open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(4);

    if (!open) {
        m_ShotPreviewOpen = false;
        m_ShotPreviewTex.reset();
        m_ShotPreviewPath.clear();
        m_ShotPreviewAnim = 0.0f;
    }
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

    ImGui::EndPopup();
}

void EditorLayer::DrawMaterialEditor(World& world, AssetLibrary& assets) {
    if (m_Selected == entt::null || !world.Registry.valid(m_Selected)) return;
    Model* model = world.Registry.get<RenderableComponent>(m_Selected).ModelRef.get();

    // Primitives (cube/sphere/...) are generated, not imported, so "the source file" wording
    // doesn't apply to them (#16 P5).
    const bool isPrimitive = model->Path().rfind("primitive://", 0) == 0;

    bool useCustom = (bool)model->MaterialOverride();
    if (ImGui::Checkbox("Use Custom Material", &useCustom)) {
        PushUndo(world, "Edit Material");
        if (useCustom) {
            auto mat = std::make_shared<Material>();
            // Keep the imported texture maps (convenient starting point) but NOT the raw
            // diffuse-color/metallic/roughness factors — most FBX exporters leave a non-white
            // leftover diffuse-color factor that was never meant to multiply a real texture,
            // and it was tinting the albedo map (e.g. a reddish cast over the whole model).
            // A fresh custom material starts neutral: white tint, non-metal, mid roughness.
            if (model->MeshCount() > 0) {
                const Material& imported = model->MeshMaterial(0);
                mat->AlbedoMap = imported.AlbedoMap;
                mat->NormalMap = imported.NormalMap;
                mat->MetallicMap = imported.MetallicMap;
                mat->RoughnessMap = imported.RoughnessMap;
                mat->AOMap = imported.AOMap;
                mat->EmissiveMap = imported.EmissiveMap;
            }
            model->SetMaterialOverride(mat);
        } else {
            model->SetMaterialOverride(nullptr);
        }
    }
    if (ImGui::IsItemHovered()) {
        EditorUI::SetTooltip(isPrimitive
            ? "Override with a fully editable PBR material.\nUnchecking goes back to the default primitive material."
            : "Override with a fully editable PBR material.\nUnchecking goes back to whatever the source file imported.");
    }

    auto mat = model->MaterialOverride();
    if (!mat) {
        ImGui::TextDisabled(isPrimitive
            ? "Using the default primitive material."
            : "Using material(s) imported from the source file.");
        return;
    }

    // Stage-on-activate / commit-on-finish: a ColorEdit's popup re-activates the parent widget
    // several times during one pick, and the old "PushUndo on IsItemActivated" fired an undo
    // step for each. Now one edit session = one "Edit Material" step (issue #11), and opening a
    // picker without changing anything adds nothing.
    PropertyLabel("Base Color", "The surface's tint, multiplied with the Albedo map if one is set.");
    ImGui::ColorEdit3("##BaseColor", &mat->BaseColor.x, ImGuiColorEditFlags_DisplayHex);
    if (ImGui::IsItemActivated()) StageUndo(world);
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");
    PropertyLabel("Metallic", "0 = non-metal (plastic, wood, skin), 1 = pure metal.\nIgnored where a Metallic map is set.");
    EditorUI::SliderFloat("##Metallic", &mat->Metallic, 0.0f, 1.0f);
    if (ImGui::IsItemActivated()) StageUndo(world);
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");
    PropertyLabel("Roughness", "0 = mirror-smooth, 1 = fully matte.\nIgnored where a Roughness map is set.");
    EditorUI::SliderFloat("##Roughness", &mat->Roughness, 0.04f, 1.0f);
    if (ImGui::IsItemActivated()) StageUndo(world);
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");
    PropertyLabel("Emissive Color", "Color this surface glows, independent of scene lighting.");
    ImGui::ColorEdit3("##EmissiveColor", &mat->EmissiveColor.x, ImGuiColorEditFlags_DisplayHex);
    if (ImGui::IsItemActivated()) StageUndo(world);
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");
    PropertyLabel("Emissive Strength", "Brightness multiplier for the Emissive Color/map - above 1 for a strong glow.");
    EditorUI::SliderFloat("##EmissiveStrength", &mat->EmissiveStrength, 0.0f, 10.0f);
    if (ImGui::IsItemActivated()) StageUndo(world);
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Edit Material");

    ImGui::SeparatorText("Texture Maps");

    // Same one-button-does-everything pattern as the Mesh Renderer's "Mesh" row: a single
    // button shows the assigned filename (or "(none)"), click opens a file picker, dragging a
    // texture from the Asset Browser onto it assigns it, and hovering it previews the actual
    // image instead of a permanent inline thumbnail — plus every row now shares the exact same
    // PropertyLabel column as the rest of the Inspector, which is what actually fixes the old
    // layout's real problem: the Import button used to sit at a different X on every row
    // because it came right after each row's own (differently-sized) label.
    auto mapRow = [&](const char* label, std::shared_ptr<Texture>& slot, const char* helpText) {
        ImGui::PushID(label);
        PropertyLabel(label);

        bool hasSlot = (bool)slot;
        std::string preview = hasSlot ? std::filesystem::path(slot->Path()).filename().string() : std::string("(none)");
        // Negative width = "fill up to this many pixels before the right edge" (ImGui's own
        // convention) — reserves exactly the Clear button's width when there's one to reserve
        // for, or claims the full row when there isn't.
        float clearReserve = hasSlot ? (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) : 0.0f;
        if (ImGui::Button(preview.c_str(), ImVec2(hasSlot ? -clearReserve : -FLT_MIN, 0.0f))) {
            std::string path = FileDialog::OpenFile(
                "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
            if (!path.empty()) {
                PushUndo(world, std::string("Set ") + label + " Map");
                slot = assets.LoadTexture(path);
            }
        }
        if (ImGui::IsItemHovered()) {
            if (hasSlot) {
                ImGui::BeginTooltip();
                ImGui::Image((ImTextureID)(intptr_t)slot->GLHandle(), ImVec2(96, 96));
                ImGui::TextUnformatted(slot->Path().c_str());
                ImGui::EndTooltip();
            } else {
                EditorUI::SetTooltip("Click to import an image, or drag one here from the Asset Browser.");
            }
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                std::string texPath((const char*)payload->Data);
                PushUndo(world, std::string("Set ") + label + " Map");
                slot = assets.LoadTexture(texPath);
            }
            ImGui::EndDragDropTarget();
        }
        if (hasSlot) {
            ImGui::SameLine();
            if (ActionButton(ICON_FA_XMARK, "Clear", false, ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
                PushUndo(world, std::string("Clear ") + label + " Map");
                slot = nullptr;
            }
        }
        if (helpText) EditorUI::HelpMarker(helpText);
        ImGui::PopID();
    };

    mapRow("Albedo", mat->AlbedoMap, "The base color texture (also called diffuse/base color map).");
    mapRow("Normal", mat->NormalMap, "Adds fine surface detail (bumps, grooves) without extra geometry.");
    mapRow("Metallic", mat->MetallicMap, "Grayscale: white = metal, black = non-metal. Overrides the Metallic slider above.");
    mapRow("Roughness", mat->RoughnessMap, "Grayscale: white = matte, black = mirror-smooth. Overrides the Roughness slider above.");
    mapRow("AO", mat->AOMap, "Ambient occlusion - darkens crevices/contact points for added depth.");
    mapRow("Emissive", mat->EmissiveMap, "Texture for glowing areas (e.g. windows, screens). Tinted by Emissive Color.");
}

void EditorLayer::DrawMultiMaterialEditor(World& world, AssetLibrary& assets,
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

glm::vec3 EditorLayer::ComputeDropRayPosition(World& world, Camera& editorCamera) const {
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 invVP = glm::inverse(proj * view);
    ImVec2 mousePos = ImGui::GetMousePos();
    float ndcX = (2.0f * (mousePos.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * (mousePos.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    glm::vec3 origin = glm::vec3(nearP);
    glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    // Landing point: the nearest existing box collider, else the Y=0 ground plane, else a fixed
    // distance out (pointing at the sky / parallel to the ground, where neither hits).
    float bestT = 1e30f;
    float boxDist;
    if (world.Raycast(origin, dir, 500.0f, boxDist) != entt::null) {
        bestT = boxDist;
    }
    if (std::abs(dir.y) > 1e-5f) {
        float groundT = -origin.y / dir.y;
        if (groundT > 0.0f && groundT < bestT) bestT = groundT;
    }
    if (bestT >= 1e30f) bestT = 8.0f;
    glm::vec3 position = origin + dir * bestT;

    // Same grid-snap concept as the transform gizmo (m_GridSnapEnabled, Ctrl inverts it
    // momentarily) - XZ only, since Y is about to be re-derived from where the object actually
    // sits (either here directly, for a prefab, or bottom-aligned in ComputeModelDropPosition).
    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float step = EditorSettings::Get().GridMinorSpacing;
    if (snapActive && step > 0.0001f) {
        position.x = std::round(position.x / step) * step;
        position.z = std::round(position.z / step) * step;
    }
    return position;
}

glm::vec3 EditorLayer::ComputeModelDropPosition(World& world, Model& model, Camera& editorCamera) const {
    glm::vec3 position = ComputeDropRayPosition(world, editorCamera);

    // Rest the model's own bottom on the hit point instead of its (possibly arbitrary) pivot -
    // same technique SnapSelectionToGround() uses for an already-placed object: evaluate the
    // lowest vertex's world Y at a candidate transform, then shift by however far that missed
    // the target height. See SnapSelectionToGround's comment for the general formula this is a
    // special case of (targetY happens to equal the candidate's own Y here).
    float targetY = position.y;
    glm::mat4 candidate = ComposeTransform(position, glm::vec3(0.0f), glm::vec3(1.0f));
    float lowestY = model.LowestVertexWorldY(candidate);
    position.y += (targetY - lowestY);
    return position;
}

void EditorLayer::DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    bool isModelDrag = peek && peek->IsDataType("ASSET_MODEL_PATH");
    bool isPrefabDrag = peek && peek->IsDataType("ASSET_PREFAB_PATH");
    // Only active while a placeable asset is actually being dragged, so this fullscreen overlay
    // never otherwise sits over the viewport intercepting camera input.
    if (!isModelDrag && !isPrefabDrag) {
        m_DragPreview.Active = false;
        return;
    }

    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) {
        m_DragPreview.Active = false;
        return;
    }

    // Cover exactly the Scene viewport rect (not the whole window) and sit ON TOP of the "Scene"
    // panel for the duration of the drag. "Scene" is a normal docked window now — it used to be
    // the dockspace's passthru central node, which let mouse events fall through it to a
    // fullscreen catcher behind. A bottom-layer catcher is simply occluded by "Scene" and never
    // becomes g.HoveredWindowUnderMovingWindow, so BeginDragDropTarget() below refuses the drop.
    // Restricting the window to the viewport rect keeps drops over the side panels routing to
    // those panels' own drop targets.
    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x, m_ViewportPos.y));
    ImGui::SetNextWindowSize(ImVec2(m_ViewportSize.x, m_ViewportSize.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##ViewportDropTarget", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoDocking);
    // Only submitted while a placeable asset is mid-drag (see the early-out above), so forcing
    // it to the front just parks the catcher over the Scene image for that drag.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::InvisibleButton("##viewport_drop_zone", ImGui::GetContentRegionAvail());
    // Plain IsItemHovered() defaults to false here for the ENTIRE drag: the Asset Browser's
    // drag-source cell stays the "active" item the whole time the mouse button is held, and
    // IsItemHovered() normally excludes hover on anything else while a different item is active
    // (to stop drag interactions from also triggering hover-only affordances underneath). That
    // flag is exactly why this only lit up after release before - the active item cleared at
    // that point, so hover only ever registered on the very last frame.
    bool hoveringViewport = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Live preview: recomputed every frame while a model is being dragged over the viewport, so
    // main.cpp can render a translucent ghost at the exact spot that would be committed if the
    // mouse were released right now (TintOverlayRenderer, driven by GetDragPreview()). Prefabs
    // don't get a ghost - they aren't necessarily a single Model with bounds to preview.
    if (isModelDrag && hoveringViewport) {
        std::string path((const char*)peek->Data);
        auto model = assets.LoadModel(path); // cache hit - already imported to appear in the browser
        if (model) {
            m_DragPreview.Active = true;
            m_DragPreview.ModelRef = model;
            m_DragPreview.Position = ComputeModelDropPosition(world, *model, editorCamera);
        } else {
            m_DragPreview.Active = false;
        }
    } else {
        m_DragPreview.Active = false;
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* modelPayload = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH");
        const ImGuiPayload* prefabPayload = modelPayload ? nullptr : ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH");

        if (modelPayload || prefabPayload) {
            std::string path((const char*)(modelPayload ? modelPayload->Data : prefabPayload->Data));
            PushUndo(world, modelPayload ? "Place Model" : "Place Prefab Instance");

            if (modelPayload) {
                auto model = assets.InstantiateModel(path);
                glm::vec3 position = ComputeModelDropPosition(world, *model, editorCamera);
                std::string name = std::filesystem::path(path).stem().string();
                entt::entity e = world.CreateModelEntity(model, position, glm::vec3(0.0f), glm::vec3(1.0f), UniqueNameFor(world, name));
                SelectItem(e, false);
            } else {
                glm::vec3 position = ComputeDropRayPosition(world, editorCamera);
                // A prefab carries its own authored transform, so the instance is created first
                // and then moved to the drop point (children ride along, being local-space).
                entt::entity e = SceneSerializer::InstantiatePrefab(world, assets, path);
                if (e != entt::null) {
                    UniquifyName(world, e);
                    if (auto* transform = world.Registry.try_get<TransformComponent>(e)) {
                        transform->Position = position;
                    }
                    SelectItem(e, false);
                }
            }
            m_DragPreview.Active = false;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::HandleViewportPicking(World& world, Camera& editorCamera) {
    ImGuiIO& io = ImGui::GetIO();
    bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool leftPressed = leftDown && !m_PrevLeftMouseDown;
    m_PrevLeftMouseDown = leftDown;

    // All ray/screen math below is done in the actual viewport rect (the dockspace's central
    // node), not the full window — so picking, box-select, and gizmos stay correctly aligned
    // as the Hierarchy/Inspector/Asset Browser panels are resized around it.
    glm::vec2 vpPos = m_ViewportPos, vpSize = m_ViewportSize;
    float w = vpSize.x, h = vpSize.y;
    if (w <= 0 || h <= 0) return;

    const float kDragThreshold = 6.0f; // pixels of movement before a press-drag-release counts as a box select rather than a click

    if (leftPressed) {
        // ImGui panel, the transform gizmo, or the nav gizmo (rotate ring / dolly / pan
        // buttons) already owns this click; Alt+Left-drag is reserved for orbiting the camera
        // around the current selection (see main.cpp's UpdateEditorCamera).
        m_BoxSelectActive = !WantsCaptureMouse() && !m_GizmoEngaged && !m_ViewGizmoBlocking &&
                            !m_LightHandleEngaged && !io.KeyAlt;
        m_BoxSelectStart = {io.MousePos.x, io.MousePos.y};
        return; // click vs. drag is only decided on release, below
    }

    if (!m_BoxSelectActive) return;
    glm::vec2 current(io.MousePos.x, io.MousePos.y);
    bool isDragging = glm::length(current - m_BoxSelectStart) > kDragThreshold;

    if (leftDown) {
        if (isDragging) {
            ImVec2 a(m_BoxSelectStart.x, m_BoxSelectStart.y), b(current.x, current.y);
            ImGui::GetForegroundDrawList()->AddRectFilled(a, b, IM_COL32(255, 217, 77, 35));
            ImGui::GetForegroundDrawList()->AddRect(a, b, IM_COL32(255, 217, 77, 220));
        }
        return; // still held: nothing selected yet, just drawing the marquee
    }

    // Released this frame — commit.
    m_BoxSelectActive = false;
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix((float)w / (float)h);

    if (!isDragging) {
        // Plain click: single-object raycast pick straight through the cursor, same as before.
        glm::mat4 invVP = glm::inverse(proj * view);
        float ndcX = (2.0f * (current.x - vpPos.x)) / w - 1.0f;
        float ndcY = 1.0f - (2.0f * (current.y - vpPos.y)) / h;
        glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
        glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
        nearP /= nearP.w;
        farP /= farP.w;
        glm::vec3 origin = glm::vec3(nearP);
        glm::vec3 dir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

        float bestT = 1e30f;
        entt::entity best = entt::null;
        auto pickView = world.Registry.view<const RenderableComponent>(entt::exclude<InactiveTag>);
        for (auto entity : pickView) {
            const auto& renderable = pickView.get<const RenderableComponent>(entity);
            glm::mat4 model = world.ComposeWorldTransform(entity);
            AABB worldBounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(model);
            float t;
            if (worldBounds.RayIntersect(origin, dir, t) && t < bestT) {
                bestT = t; best = entity;
            }
        }

        // Mesh-less entities (lights, empties) have no geometry to hit, so they're picked by
        // proximity to their on-screen icon instead. The icon is a screen-space overlay drawn
        // ON TOP of everything, so a click that lands on it selects that entity even when a mesh
        // is in front — that's what you see, and what you want when deliberately clicking a
        // light's icon (#140). Grab the mesh by clicking it away from the icon. Nearest icon to
        // the cursor wins when several overlap.
        {
            const float kIconPickPixels = 12.0f * m_UIScale;
            glm::mat4 viewProj = proj * view;
            float bestPixelDist = kIconPickPixels;
            entt::entity iconHit = entt::null;
            float iconDepth = 1e30f;
            for (auto entity : world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent, InactiveTag>)) {
                glm::mat4 model = world.ComposeWorldTransform(entity);
                glm::vec3 worldPos = glm::vec3(model[3]);
                glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
                if (clip.w <= 0.0001f) continue;

                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                glm::vec2 screen(vpPos.x + (ndc.x * 0.5f + 0.5f) * w,
                                 vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
                float pixelDist = glm::length(screen - current);
                if (pixelDist > bestPixelDist) continue;

                bestPixelDist = pixelDist;
                iconHit = entity;
                iconDepth = glm::length(worldPos - origin);
            }
            if (iconHit != entt::null) { // an icon under the cursor beats any mesh behind it
                best = iconHit;
                bestT = iconDepth;
            }
        }

        if (best != entt::null) SelectItem(best, io.KeyCtrl);
        else if (!io.KeyCtrl) ClearSelection(); // clicked empty space -> deselect (unless Ctrl-clicking to preserve a group)

        // Grab handles arm only when the click resolved to a light IN THE VIEWPORT (#140
        // follow-up). SelectItem() cleared m_LightHandleArmedFor a moment ago; re-set it here.
        m_LightHandleArmedFor =
            (best != entt::null && world.Registry.all_of<LightComponent>(best)) ? best : entt::null;
        return;
    }

    // Box select: everything whose projected screen-space bounds overlap the drag rectangle.
    glm::vec2 rectMin = glm::min(m_BoxSelectStart, current);
    glm::vec2 rectMax = glm::max(m_BoxSelectStart, current);
    glm::mat4 viewProj = proj * view;

    auto projectedScreenRect = [&](const AABB& bounds, glm::vec2& outMin, glm::vec2& outMax) {
        outMin = glm::vec2(1e30f);
        outMax = glm::vec2(-1e30f);
        bool any = false;
        for (int c = 0; c < 8; ++c) {
            glm::vec3 corner(
                (c & 1) ? bounds.Max.x : bounds.Min.x,
                (c & 2) ? bounds.Max.y : bounds.Min.y,
                (c & 4) ? bounds.Max.z : bounds.Min.z);
            glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
            if (clip.w <= 0.0001f) continue; // behind the camera
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            glm::vec2 screen = vpPos + glm::vec2((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
            outMin = glm::min(outMin, screen);
            outMax = glm::max(outMax, screen);
            any = true;
        }
        return any;
    };
    auto rectsOverlap = [](glm::vec2 aMin, glm::vec2 aMax, glm::vec2 bMin, glm::vec2 bMax) {
        return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y;
    };

    if (!io.KeyCtrl) ClearSelection();

    auto entityView = world.Registry.view<const RenderableComponent>(entt::exclude<InactiveTag>);
    for (auto entity : entityView) {
        const auto& renderable = entityView.get<const RenderableComponent>(entity);
        glm::mat4 model = world.ComposeWorldTransform(entity);
        AABB bounds = AABB{renderable.ModelRef->BoundsMin(), renderable.ModelRef->BoundsMax()}.Transformed(model);
        glm::vec2 pMin, pMax;
        if (projectedScreenRect(bounds, pMin, pMax) && rectsOverlap(pMin, pMax, rectMin, rectMax)) {
            AddToSelectionIfAbsent(entity);
        }
    }

    // Mesh-less entities (lights, cameras, empties) have no AABB to project — marquee-select
    // them the same way a plain click does: test their on-screen icon position (same math as
    // the icon-proximity pick above) against the drag rectangle.
    for (auto entity : world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent, InactiveTag>)) {
        glm::mat4 model = world.ComposeWorldTransform(entity);
        glm::vec3 worldPos = glm::vec3(model[3]);
        glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 screen(vpPos.x + (ndc.x * 0.5f + 0.5f) * w,
                         vpPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
        if (screen.x >= rectMin.x && screen.x <= rectMax.x && screen.y >= rectMin.y && screen.y <= rectMax.y) {
            AddToSelectionIfAbsent(entity);
        }
    }
}

bool EditorLayer::FindVertexUnderCursor(World& world, Camera& editorCamera, glm::vec3& outLocalPos) const {
    if (!IsVertexDraggable(world, m_Selected)) return false;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return false;

    const auto& transform = world.Registry.get<TransformComponent>(m_Selected);
    const auto& renderable = world.Registry.get<RenderableComponent>(m_Selected);
    glm::mat4 model = ComposeTransform(transform);
    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    ImVec2 mouse = ImGui::GetIO().MousePos;
    glm::vec2 viewportMouse(mouse.x - m_ViewportPos.x, mouse.y - m_ViewportPos.y);
    return renderable.ModelRef->FindNearestVertexToScreenPoint(model, viewProj, {viewportMouse.x, viewportMouse.y},
        m_ViewportSize.x, m_ViewportSize.y, m_VertexPickPixels, outLocalPos);
}

void EditorLayer::UpdateVertexDrag(World& world, Camera& editorCamera) {
    if (!IsVertexDraggable(world, m_Selected)) {
        m_VertexDragActive = false;
        return;
    }
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    // Cast a ray through the current mouse position and intersect it with a camera-facing
    // plane fixed at the grabbed vertex's world position when the drag started — a standard
    // "grab" drag (Blender's G key): the object slides freely across the screen at constant
    // depth rather than needing an axis-constrained gizmo handle.
    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 invVP = glm::inverse(proj * view);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    float ndcX = (2.0f * (mouse.x - m_ViewportPos.x)) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - (2.0f * (mouse.y - m_ViewportPos.y)) / m_ViewportSize.y;
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farP = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    glm::vec3 rayOrigin = glm::vec3(nearP);
    glm::vec3 rayDir = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));

    glm::vec3 planeNormal = editorCamera.Front();
    float denom = glm::dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-5f) return; // looking edge-on along the drag plane; hold position this frame

    float t = glm::dot(m_VertexDragPlanePoint - rayOrigin, planeNormal) / denom;
    if (t < 0.0f) return; // plane point is behind the camera

    glm::vec3 grabbedWorld = rayOrigin + rayDir * t;
    glm::vec3 candidatePosition = grabbedWorld + m_VertexDragOffset;

    // Vertex snap: search every model OUTSIDE the current selection for whichever vertex sits
    // closest to the CURSOR ON SCREEN (other group members are excluded — they're moving
    // rigidly along with the grabbed vertex, so their relative distance to it never actually
    // changes, and "snapping" onto one would just lock the group to itself) and, if one is
    // within the pick radius, nudge the object so the grabbed vertex lands exactly on it.
    //
    // This is screen-space (pixels), matching the SAME m_VertexPickPixels threshold that
    // decides whether the yellow hover circle shows up in the first place — deliberately, so
    // "the circle is showing" and "this will snap" always agree. It used to be a fixed
    // world-space radius (m_VertexSnapRadius), which meant a vertex that looked perfectly
    // aligned on screen (because the camera was zoomed out) could still refuse to snap purely
    // because it was far away in 3D units — a mismatch between what you see and what the
    // check actually measured.
    auto isExcluded = [&](entt::entity entity) {
        if (entity == m_Selected) return true;
        for (entt::entity e : m_ExtraSelection) if (e == entity) return true;
        return false;
    };

    glm::mat4 viewProjSnap = proj * view;
    glm::vec2 cursorScreen(mouse.x - m_ViewportPos.x, mouse.y - m_ViewportPos.y);

    // Boxes were never valid snap targets before (only other models) — excluded here the same
    // way, via LevelGeometryTag.
    float bestPixelDist = m_VertexPickPixels;
    glm::vec3 bestTarget{};
    bool found = false;
    auto snapView = world.Registry.view<const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : snapView) {
        if (isExcluded(entity)) continue;
        const auto& otherRenderable = snapView.get<const RenderableComponent>(entity);
        glm::mat4 otherMatrix = world.ComposeWorldTransform(entity);
        glm::vec3 candidateLocal;
        // Passing the current best-so-far as this call's own cutoff narrows every subsequent
        // candidate model to "closer than whatever's already winning," so the loop converges
        // on the single nearest-on-screen vertex across ALL candidate models, not just the
        // nearest one within each model considered in isolation.
        if (!otherRenderable.ModelRef->FindNearestVertexToScreenPoint(otherMatrix, viewProjSnap, cursorScreen,
                m_ViewportSize.x, m_ViewportSize.y, bestPixelDist, candidateLocal)) {
            continue;
        }
        glm::vec4 clip = viewProjSnap * otherMatrix * glm::vec4(candidateLocal, 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        glm::vec2 screen((ndc.x * 0.5f + 0.5f) * m_ViewportSize.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        float pixelDist = glm::length(screen - cursorScreen);
        if (pixelDist < bestPixelDist) {
            bestPixelDist = pixelDist;
            bestTarget = glm::vec3(otherMatrix * glm::vec4(candidateLocal, 1.0f));
            found = true;
        }
    }
    if (found) {
        candidatePosition += (bestTarget - grabbedWorld);
    }

    // Move the primary by however much this frame actually resolved to (drag + snap), then
    // carry every other selected object along by that exact same delta so the whole group
    // moves together while only the primary's vertex does the snapping. Boxes and models both
    // just have a TransformComponent now, so there's no more per-kind branch needed here either.
    auto& primaryTransform = world.Registry.get<TransformComponent>(m_Selected);
    glm::vec3 delta = candidatePosition - primaryTransform.Position;
    primaryTransform.Position = candidatePosition;

    for (entt::entity e : m_ExtraSelection) {
        if (world.Registry.valid(e)) world.Registry.get<TransformComponent>(e).Position += delta;
    }
}

void EditorLayer::DrawEntityIcons(World& world, Camera& editorCamera) {
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    // Draw into the Scene window's own draw list (clipped to the viewport rect), NOT the
    // foreground list — the foreground list renders on top of every panel, so light/empty
    // icons would punch through the Inspector, Preferences, any window overlapping the
    // viewport. Appended to the Scene window's list, they sit at its z-order and a panel on
    // top correctly covers them.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    // Mesh-less entities would otherwise be invisible in the viewport — a light you can't see
    // is a light you can't select or aim.
    auto view = world.Registry.view<const TransformComponent>(entt::exclude<RenderableComponent>);
    for (auto entity : view) {
        glm::mat4 model = world.ComposeWorldTransform(entity);
        glm::vec4 clip = viewProj * glm::vec4(glm::vec3(model[3]), 1.0f);
        if (clip.w <= 0.0001f) continue; // behind the camera

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        ImVec2 screen(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                      m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);

        bool selected = IsSelected(entity);
        bool inactive = world.Registry.all_of<InactiveTag>(entity);
        const auto* light = world.Registry.try_get<LightComponent>(entity);
        const bool isCamera = world.Registry.all_of<CameraComponent>(entity);

        // One glyph per kind (#159), and per light type so point / spot / sun read apart at a
        // glance — matching the Add menu's glyphs.
        const char* glyph;
        if (light) {
            glyph = light->Kind == LightComponent::Type::Spot        ? ICON_FA_BULLSEYE
                  : light->Kind == LightComponent::Type::Directional ? ICON_FA_SUN
                                                                     : ICON_FA_LIGHTBULB; // Point
        } else {
            glyph = isCamera ? ICON_FA_VIDEO : ICON_FA_DIAGRAM_PROJECT;
        }

        ImU32 color;
        if (inactive) {
            color = IM_COL32(140, 140, 140, 170);
        } else if (light) {
            // Tinted with the light's own colour so the marker previews what it casts.
            glm::vec3 c = glm::clamp(light->Color, 0.0f, 1.0f) * 255.0f;
            color = IM_COL32((int)c.r, (int)c.g, (int)c.b, selected ? 255 : 235);
        } else {
            color = selected ? IM_COL32(230, 238, 245, 255) : IM_COL32(190, 200, 210, 220);
        }

        ImFont* font = ImGui::GetFont();
        const float iconPx = 16.0f * m_UIScale;
        const ImVec2 gs = font->CalcTextSizeA(iconPx, FLT_MAX, 0.0f, glyph);
        draw->AddText(font, iconPx, ImVec2(screen.x - gs.x * 0.5f, screen.y - gs.y * 0.5f), color, glyph);

        // Selected state = a subtle ring, not extra geometry inside the marker.
        if (selected)
            draw->AddCircle(screen, iconPx * 0.72f, IM_COL32(255, 150, 30, 230), 0, 1.5f * m_UIScale);
    }

    draw->PopClipRect();
}

void EditorLayer::DrawLightGizmos(World& world, Camera& editorCamera) {
    const EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowLightGizmos) return;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    const glm::mat4 view = editorCamera.ViewMatrix();
    const glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    const glm::mat4 viewProj = proj * view;

    // Same draw target + clipping as DrawEntityIcons: the Scene window's own list, bounded to
    // the viewport rect, so panels over the viewport cover the shapes instead of them bleeding.
    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 clipMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 clipMax(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    draw->PushClipRect(clipMin, clipMax, true);

    auto project = [&](const glm::vec3& wp, ImVec2& out) -> bool {
        glm::vec4 clip = viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 0.0001f) return false;               // at/behind the eye
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = ImVec2(m_ViewportPos.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        return true;
    };
    // Project a world-space polyline and stroke it, dropping any segment with an endpoint that
    // failed to project (behind the camera) rather than drawing a wild line across the screen.
    auto stroke = [&](const std::vector<glm::vec3>& pts, bool closed, ImU32 col, float thick) {
        const int n = (int)pts.size();
        for (int i = 0; i < n - (closed ? 0 : 1); ++i) {
            ImVec2 a, b;
            if (project(pts[i], a) && project(pts[(i + 1) % n], b)) draw->AddLine(a, b, col, thick);
        }
    };
    auto circle = [](const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, float r, int seg) {
        std::vector<glm::vec3> pts;
        pts.reserve(seg);
        for (int i = 0; i < seg; ++i) {
            float t = (float)i / (float)seg * 6.28318530718f;
            pts.push_back(c + (cosf(t) * u + sinf(t) * v) * r);
        }
        return pts;
    };
    auto basis = [](const glm::vec3& dir, glm::vec3& u, glm::vec3& v) {
        glm::vec3 up = std::fabs(dir.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        u = glm::normalize(glm::cross(dir, up));
        v = glm::cross(dir, u);
    };

    const float gscale = std::max(prefs.LightGizmoScale, 0.05f);

    for (auto entity : world.Registry.view<const TransformComponent, const LightComponent>()) {
        if (world.Registry.all_of<InactiveTag>(entity)) continue;
        const bool selected = IsSelected(entity);
        if (prefs.LightGizmoSelectedOnly && !selected) continue;

        const auto& light = world.Registry.get<const LightComponent>(entity);
        const glm::mat4 model = world.ComposeWorldTransform(entity);
        const glm::vec3 pos = glm::vec3(model[3]);
        const glm::vec3 dir = glm::normalize(glm::vec3(model * glm::vec4(0, 0, -1, 0)));

        glm::vec3 c = glm::clamp(light.Color, 0.0f, 1.0f);
        int a = (int)(std::clamp(prefs.LightGizmoOpacity, 0.0f, 1.0f) * 200.0f) + 25;
        if (selected) a = std::min(255, a + 100);
        const ImU32 col = IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a);
        const float thick = selected ? 2.0f : 1.3f;

        if (light.Kind == LightComponent::Type::Point) {
            // One clean view-facing ring for the range (like Unity), plus a faint ground circle
            // so it still reads as a sphere sitting in space rather than a flat disc.
            float r = std::max(light.Range, 0.01f);
            stroke(circle(pos, editorCamera.Right(), editorCamera.Up(), r, 56), true, col, thick);
            stroke(circle(pos, glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), r, 48), true,
                   IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a / 3), 1.0f);
        } else if (light.Kind == LightComponent::Type::Spot) {
            float range = std::max(light.Range, 0.05f);
            float half = glm::radians(std::clamp(light.SpotAngleDegrees, 1.0f, 89.0f));
            glm::vec3 u, v; basis(dir, u, v);
            glm::vec3 apex = pos;
            glm::vec3 center = pos + dir * range;
            float rimR = range * tanf(half);
            stroke(circle(center, u, v, rimR, 48), true, col, thick);
            // inner falloff cone, dimmer — matches main.cpp's 0.9 * SpotAngle inner cutoff
            float innerR = range * tanf(half * 0.9f);
            stroke(circle(center, u, v, innerR, 40), true, IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a / 2), 1.0f);
            for (int k = 0; k < 4; ++k) {
                float t = (float)k * 1.57079632679f;
                std::vector<glm::vec3> edge = { apex, center + (cosf(t) * u + sinf(t) * v) * rimR };
                stroke(edge, false, col, thick);
            }
        } else { // Directional
            float L = 2.6f * gscale;
            glm::vec3 u, v; basis(dir, u, v);
            // two parallel shafts to read as parallel rays, each with a small chevron head
            for (int s = -1; s <= 1; s += 2) {
                glm::vec3 o = pos + u * (0.5f * gscale * (float)s);
                glm::vec3 tip = o + dir * L;
                std::vector<glm::vec3> shaft = { o, tip };
                stroke(shaft, false, col, thick);
                std::vector<glm::vec3> head1 = { tip, tip - dir * (0.5f * gscale) + u * (0.28f * gscale) };
                std::vector<glm::vec3> head2 = { tip, tip - dir * (0.5f * gscale) - u * (0.28f * gscale) };
                stroke(head1, false, col, thick);
                stroke(head2, false, col, thick);
            }
            stroke(circle(pos, u, v, 0.38f * gscale, 32), true, col, thick); // sun disc
        }

        if (selected) {
            ImVec2 sp;
            if (project(pos, sp)) {
                char buf[96];
                if (light.Kind == LightComponent::Type::Spot)
                    snprintf(buf, sizeof(buf), "%.0f deg  |  range %.1f", light.SpotAngleDegrees, light.Range);
                else if (light.Kind == LightComponent::Type::Point)
                    snprintf(buf, sizeof(buf), "range %.1f", light.Range);
                else
                    snprintf(buf, sizeof(buf), "sun  %.2f deg", light.AngularSizeDegrees);
                ImVec2 tp(sp.x + 12.0f * m_UIScale, sp.y - 6.0f * m_UIScale);
                draw->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 180), buf);
                draw->AddText(tp, IM_COL32(255, 255, 255, 235), buf);
            }
        }
    }

    draw->PopClipRect();
}

void EditorLayer::UpdateLightHandles(World& world, Camera& editorCamera) {
    m_LightHandleEngaged = false;
    const EditorSettings& prefs = EditorSettings::Get();
    // #162 — de-conflict with the transform gizmo instead of hard-vanishing the dots.
    // While the gizmo is actually being dragged, the dots go away entirely. While it's only
    // hovered (m_GizmoEngaged carries last frame's ImGuizmo::IsOver() || IsUsing()), keep the
    // dots on screen but faded and non-interactive so the gizmo always wins the cursor.
    const bool gizmoDragging = ImGuizmo::IsUsing();
    const bool gizmoHot = m_GizmoEngaged;
    if (!prefs.ShowLightGizmos || m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f ||
        gizmoDragging || HasGroupSelection()) {
        m_LightHandleDragging = false;
        m_HotLightHandle = LightHandle::None;
        return;
    }
    entt::entity e = m_Selected;
    // Only for a light that was clicked in the viewport (its icon) — not one selected from the
    // Hierarchy / Lights panel / box-select. m_LightHandleArmedFor is set by HandleViewportPicking
    // and cleared by every other selection path (#140 follow-up).
    if (e == entt::null || e != m_LightHandleArmedFor || !world.Registry.valid(e) ||
        !world.Registry.all_of<LightComponent>(e)) {
        m_LightHandleDragging = false;
        m_HotLightHandle = LightHandle::None;
        return;
    }

    auto& light = world.Registry.get<LightComponent>(e);
    auto& xf = world.Registry.get<TransformComponent>(e);
    entt::entity parent = entt::null;
    if (auto* h = world.Registry.try_get<HierarchyComponent>(e)) parent = h->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);
    glm::mat4 model = world.ComposeWorldTransform(e);
    glm::vec3 pos = glm::vec3(model[3]);
    glm::vec3 dir = glm::normalize(glm::vec3(model * glm::vec4(0, 0, -1, 0)));

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);
    glm::mat4 vp = proj * view;
    auto project = [&](const glm::vec3& w, ImVec2& out) -> bool {
        glm::vec4 c = vp * glm::vec4(w, 1.0f);
        if (c.w <= 0.0001f) return false;
        glm::vec3 n = glm::vec3(c) / c.w;
        out = ImVec2(m_ViewportPos.x + (n.x * 0.5f + 0.5f) * m_ViewportSize.x,
                     m_ViewportPos.y + (1.0f - (n.y * 0.5f + 0.5f)) * m_ViewportSize.y);
        return true;
    };
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    glm::mat4 invVP = glm::inverse(vp);
    float ndcX = 2.0f * (mouse.x - m_ViewportPos.x) / m_ViewportSize.x - 1.0f;
    float ndcY = 1.0f - 2.0f * (mouse.y - m_ViewportPos.y) / m_ViewportSize.y;
    glm::vec4 rn = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f); rn /= rn.w;
    glm::vec4 rf = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);  rf /= rf.w;
    glm::vec3 rayO = glm::vec3(rn);
    glm::vec3 rayD = glm::normalize(glm::vec3(rf) - glm::vec3(rn));

    // Parameter t of the point on the line (P0 + A*t, A unit) closest to the cursor ray.
    auto lineParamNearestRay = [&](const glm::vec3& P0, const glm::vec3& A) -> float {
        glm::vec3 w0 = P0 - rayO;
        float b = glm::dot(A, rayD);
        float d = glm::dot(A, w0);
        float ee = glm::dot(rayD, w0);
        float denom = 1.0f - b * b;                 // a*c - b*b, with a=c=1
        if (std::fabs(denom) < 1e-5f) return -d;    // ray ~parallel to the axis
        return (b * ee - d) / denom;
    };

    // Build this light's dot list: {kind, world position, slide axis}.
    struct Dot { LightHandle kind; glm::vec3 world; glm::vec3 axis; };
    std::vector<Dot> dots;
    float range = std::max(light.Range, 0.05f);
    float halfDeg = std::clamp(light.SpotAngleDegrees, 1.0f, 89.0f);
    glm::vec3 u, v;
    {
        glm::vec3 up = std::fabs(dir.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        u = glm::normalize(glm::cross(dir, up));
        v = glm::cross(dir, u);
    }
    if (light.Kind == LightComponent::Type::Point) {
        // Four dots on the view-facing range ring (matches the single ring the gizmo now draws).
        const glm::vec3 rr = editorCamera.Right(), uu = editorCamera.Up();
        dots.push_back({LightHandle::Range, pos + rr * range, rr});
        dots.push_back({LightHandle::Range, pos - rr * range, -rr});
        dots.push_back({LightHandle::Range, pos + uu * range, uu});
        dots.push_back({LightHandle::Range, pos - uu * range, -uu});
    } else if (light.Kind == LightComponent::Type::Spot) {
        glm::vec3 center = pos + dir * range;
        float rimR = range * tanf(glm::radians(halfDeg));
        dots.push_back({LightHandle::Range, center, dir});
        dots.push_back({LightHandle::SpotAngle, center + u * rimR, u});
        dots.push_back({LightHandle::SpotAngle, center - u * rimR, -u});
        dots.push_back({LightHandle::SpotAngle, center + v * rimR, v});
        dots.push_back({LightHandle::SpotAngle, center - v * rimR, -v});
        dots.push_back({LightHandle::Aim, pos + dir * std::clamp(range * 0.4f, 1.0f, 6.0f), dir});
    } else { // Directional
        dots.push_back({LightHandle::Aim, pos + dir * (2.6f * std::max(prefs.LightGizmoScale, 0.05f)), dir});
    }

    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* draw = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    draw->PushClipRect(ImVec2(m_ViewportPos.x, m_ViewportPos.y),
                       ImVec2(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y), true);

    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (m_LightHandleDragging) {
        m_LightHandleEngaged = true;
        if (!leftDown) {
            m_LightHandleDragging = false;
            m_HotLightHandle = LightHandle::None;
        } else if (m_HotLightHandle == LightHandle::Range) {
            float t = lineParamNearestRay(pos, m_LightHandleGrabAxis);
            light.Range = std::clamp(t, 0.05f, 100000.0f);
        } else if (m_HotLightHandle == LightHandle::SpotAngle) {
            glm::vec3 center = pos + dir * std::max(light.Range, 0.05f);
            float t = lineParamNearestRay(center, m_LightHandleGrabAxis);
            float newHalf = glm::degrees(atanf(std::max(t, 1e-4f) / std::max(light.Range, 0.05f)));
            light.SpotAngleDegrees = std::clamp(newHalf, 1.0f, 89.0f);
        } else if (m_HotLightHandle == LightHandle::Aim) {
            glm::vec3 handlePos = pos + dir * m_LightHandleGrabParam;
            glm::vec3 nrm = -editorCamera.Front();
            float denom = glm::dot(rayD, nrm);
            if (std::fabs(denom) > 1e-5f) {
                float s = glm::dot(handlePos - rayO, nrm) / denom;
                glm::vec3 fwd = (rayO + rayD * s) - pos;
                if (glm::length(fwd) > 1e-4f) {
                    fwd = glm::normalize(fwd);
                    glm::vec3 up = std::fabs(fwd.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                    glm::mat4 rot = glm::inverse(glm::lookAt(glm::vec3(0.0f), fwd, up));
                    xf.RotationEuler = EulerYXZFromMatrix(glm::inverse(parentWorld) * rot);
                }
            }
        }
    } else {
        m_HotLightHandle = LightHandle::None;
        m_HotLightHandleIndex = -1;
        float best = 11.0f * m_UIScale;
        // #162 — the gizmo owns the cursor while it's hovered; don't let a dot under one of
        // its arrows claim the hover/click.
        for (int i = 0; !gizmoHot && i < (int)dots.size(); ++i) {
            ImVec2 sp;
            if (!project(dots[i].world, sp)) continue;
            float d = std::sqrt((sp.x - mouse.x) * (sp.x - mouse.x) + (sp.y - mouse.y) * (sp.y - mouse.y));
            if (d < best) { best = d; m_HotLightHandleIndex = i; m_HotLightHandle = dots[i].kind; }
        }
        if (m_HotLightHandleIndex >= 0) {
            m_LightHandleEngaged = true; // hovering a dot claims the click (no deselect / box-select)
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !WantsCaptureMouse()) {
                const Dot& g = dots[m_HotLightHandleIndex];
                m_LightHandleDragging = true;
                m_LightHandleGrabAxis = g.axis;
                if (g.kind == LightHandle::Aim) m_LightHandleGrabParam = glm::length(g.world - pos);
                PushUndo(world, g.kind == LightHandle::Aim ? "Aim Light"
                              : g.kind == LightHandle::SpotAngle ? "Set Spot Angle" : "Set Light Range");
            }
        }
    }

    // #162 — dots draw here, before DrawGizmo() runs later this frame into the same Scene
    // draw list, so the transform gizmo always paints on top. Two fades keep them from
    // fighting the gizmo visually: a flat dim while the gizmo is hovered, and a radial
    // falloff for any dot sitting within ~one gizmo-arm's length of the light's origin.
    ImVec2 originSP;
    const bool haveOrigin = project(pos, originSP);
    const float gizmoPx = std::max(m_GizmoSize * m_ViewportSize.y * 0.5f, 1.0f);
    const float baseAlpha = gizmoHot ? 0.28f : 1.0f;
    for (int i = 0; i < (int)dots.size(); ++i) {
        ImVec2 sp;
        if (!project(dots[i].world, sp)) continue;
        bool hot = m_LightHandleDragging
                       ? (dots[i].kind == m_HotLightHandle &&
                          glm::dot(dots[i].axis, m_LightHandleGrabAxis) > 0.999f)
                       : (i == m_HotLightHandleIndex);
        float alpha = baseAlpha;
        if (haveOrigin && !hot) {
            float dOrigin = std::sqrt((sp.x - originSP.x) * (sp.x - originSP.x) +
                                      (sp.y - originSP.y) * (sp.y - originSP.y));
            if (dOrigin < gizmoPx)
                alpha *= std::clamp(0.12f + 0.88f * (dOrigin / gizmoPx), 0.0f, 1.0f);
        }
        if (hot) alpha = 1.0f; // the dot you're dragging stays fully legible
        float r = (hot ? 6.0f : 4.0f) * m_UIScale;
        int fillA = (int)((hot ? 255.0f : 225.0f) * alpha);
        int lineA = (int)(190.0f * alpha);
        draw->AddCircleFilled(sp, r, hot ? IM_COL32(255, 200, 60, fillA) : IM_COL32(245, 245, 245, fillA));
        draw->AddCircle(sp, r, IM_COL32(0, 0, 0, lineA), 0, 1.5f);
    }

    draw->PopClipRect();
}

void EditorLayer::DrawGizmo(World& world, Camera& editorCamera) {
    if (HasGroupSelection()) {
        DrawGroupGizmo(world, editorCamera);
        return;
    }

    if (m_Selected == entt::null || !world.Registry.valid(m_Selected)) {
        m_GizmoEngaged = false;
        return;
    }

    auto& transform = world.Registry.get<TransformComponent>(m_Selected);
    // Lights and empties have no mesh, so this can legitimately be null — everything below that
    // needs mesh extents (Rect-tool bounds, Center pivot) falls back to a small unit box.
    auto* renderablePtr = world.Registry.try_get<RenderableComponent>(m_Selected);
    bool registryHasRenderable = renderablePtr != nullptr;

    // A parented entity's TransformComponent is local space, but ImGuizmo has to manipulate a
    // world-space matrix (it's drawn and dragged against the world-space view/proj below) — so
    // the gizmo works in world space and the result gets converted back to local afterward.
    entt::entity parent = entt::null;
    if (auto* hier = world.Registry.try_get<HierarchyComponent>(m_Selected)) parent = hier->Parent;
    glm::mat4 parentWorld = parent != entt::null ? world.ComposeWorldTransform(parent) : glm::mat4(1.0f);

    glm::vec3 nativeBoundsMin = registryHasRenderable ? renderablePtr->ModelRef->BoundsMin() : glm::vec3(-0.5f);
    glm::vec3 nativeBoundsMax = registryHasRenderable ? renderablePtr->ModelRef->BoundsMax() : glm::vec3(0.5f);

    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0) return;

    // ImGuizmo's own hover/click hit-testing needs a real, hoverable ImGui window as the
    // "current window" — calling Manipulate() with no window active draws fine but never
    // registers clicks. A fullscreen transparent overlay gives it that context without
    // visually intruding or stealing focus from the other panels.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // NoInputs keeps this fullscreen window out of ImGui's own hover/capture bookkeeping —
    // otherwise it would make WantCaptureMouse true everywhere on screen and block the
    // editor fly-camera. ImGuizmo does its own hit-testing against raw mouse position, so
    // it isn't affected by this window's own input flags.
    ImGui::Begin("##GizmoOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    // NoInputs means this window can never be clicked to bring itself to front, so its z-order
    // is otherwise whatever position it happened to land in ImGui's window stack the first time
    // it was ever created - which put it BEHIND "Scene" once that became a real window (Scene is
    // newer, so it was appended in front). Forced to the front explicitly, every frame, so the
    // gizmo actually draws on top of the Scene image instead of being invisibly covered by it.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // ImGuizmo hit-tests against its own draw-list window (this NoInputs overlay), which is never
    // ImGui's g.HoveredWindow — so without this, hovering the actual "Scene" panel makes
    // IsHoveringWindow() return false and the handles draw but never grab. Registering "Scene" as
    // the alternative window is ImGuizmo's supported way to say "the user hovers there, not here".
    ImGuizmo::SetAlternativeWindow(ImGui::FindWindowByName("Scene"));

    ImGuizmo::SetOrthographic(editorCamera.Orthographic); // #107 — ortho views used to offset the handles
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
    ImGuizmo::SetGizmoSizeClipSpace(m_GizmoSize);
    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (m_GizmoOp == GizmoOp::Rotate) op = ImGuizmo::ROTATE;
    else if (m_GizmoOp == GizmoOp::Scale) op = ImGuizmo::SCALE;
    else if (m_GizmoOp == GizmoOp::Rect) op = ImGuizmo::OPERATION(ImGuizmo::TRANSLATE | ImGuizmo::BOUNDS);

    glm::mat4 matrix = parentWorld * ComposeTransform(transform);

    // Pivot/Center: in Center mode the gizmo is drawn at the bounding-box center instead of the
    // object's own origin. Only the gizmo's displayed frame shifts — the delta it produces is
    // applied back to the real transform below, so the object doesn't move when the mode flips.
    glm::vec3 centerOffset(0.0f);
    if (m_GizmoPivotCenter && registryHasRenderable) {
        AABB nativeBounds{nativeBoundsMin, nativeBoundsMax};
        AABB worldBounds = nativeBounds.Transformed(matrix);
        glm::vec3 worldCenter = (worldBounds.Min + worldBounds.Max) * 0.5f;
        centerOffset = worldCenter - glm::vec3(matrix[3]);
        matrix[3] += glm::vec4(centerOffset, 0.0f);
    }

    // Ctrl held inverts the checkbox for the duration of the drag — matches Blender's
    // momentary-snap convention while still giving snapping a persistent on/off default.
    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float snapValues[3] = {m_SnapTranslation, m_SnapTranslation, m_SnapTranslation};
    if (m_GizmoOp == GizmoOp::Rotate) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapRotationDeg;
    else if (m_GizmoOp == GizmoOp::Scale) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapScale;

    float bounds[6] = {nativeBoundsMin.x, nativeBoundsMin.y, nativeBoundsMin.z,
                        nativeBoundsMax.x, nativeBoundsMax.y, nativeBoundsMax.z};
    const float* boundsPtr = (m_GizmoOp == GizmoOp::Rect) ? bounds : nullptr;

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(matrix), nullptr, snapActive ? snapValues : nullptr, boundsPtr);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    bool isUsingNow = ImGuizmo::IsUsing();
    if (isUsingNow && !m_GizmoWasUsing) {
        // Drag just started this frame: snapshot the still-unmodified transform (pos/rot/scale
        // below haven't been written yet) so undo restores to exactly where the drag began.
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
    }
    m_GizmoWasUsing = isUsingNow;

    if (isUsingNow) {
        // Undo the Center-mode display shift before reading the result back, so the object's
        // real origin moves by the drag delta rather than jumping onto its bounds center.
        glm::mat4 adjusted = matrix;
        adjusted[3] -= glm::vec4(centerOffset, 0.0f);
        // Manipulate() edited the world-space matrix — convert back to local before writing it
        // to TransformComponent (a no-op conversion when unparented, since parentWorld is then
        // identity).
        glm::mat4 newLocal = glm::inverse(parentWorld) * adjusted;
        float nt[3], nr[3], ns[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newLocal), nt, nr, ns);
        glm::vec3 newPos{nt[0], nt[1], nt[2]};
        glm::vec3 newScale{ns[0], ns[1], ns[2]};
        // Rotation via the ComposeTransform-matching order, NOT ImGuizmo's decompose (#108).
        glm::vec3 newRot = EulerYXZFromMatrix(newLocal);

        // Write back only the channel this gizmo actually drives — ImGuizmo's decompose leaks
        // float noise into the other two, and a pure translate drag was nudging Rotation
        // 0.000 -> -0.000 every time, accumulating over many drags (#12 P1).
        switch (m_GizmoOp) {
            case GizmoOp::Translate: transform.Position = newPos;      break;
            case GizmoOp::Rotate:    transform.RotationEuler = newRot;  break;
            case GizmoOp::Scale:     transform.Scale = newScale;        break;
            default: // Rect / bounds edit resizes from a handle — position and scale both move
                transform.Position = newPos;
                transform.RotationEuler = newRot;
                transform.Scale = newScale;
                break;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::DrawViewGizmo(World& world, Camera& editorCamera) {
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0) return;

    // The library's own defaults (256px rotate ring, 50px tool buttons) are sized for a full
    // editor viewport; shrink them to sit unobtrusively in the corner instead.
    ImViewGuizmo::Style& style = ImViewGuizmo::GetStyle();
    style.scale = m_UIScale * 0.5f;

    float gizmoRadius = 128.0f * style.scale; // half of the library's fixed 256px rotate-ring box
    float toolRadius = style.toolButtonRadius * style.scale;
    float margin = 14.0f * m_UIScale;
    float spacing = 8.0f * m_UIScale;

    // Rotate's `position` param is the ring's CENTER, but Dolly/Pan's is the TOP-LEFT of their
    // button box (confirmed by reading ImViewGuizmo.h — the two widget kinds don't agree on
    // that convention despite the doc comments implying otherwise). Laying out from a shared
    // center point and converting only for Dolly/Pan keeps the whole cluster symmetric.
    ImVec2 rotateCenter(m_ViewportPos.x + m_ViewportSize.x - margin - gizmoRadius,
        m_ViewportPos.y + margin + gizmoRadius);
    float toolCenterY = rotateCenter.y + gizmoRadius + spacing + toolRadius;
    ImVec2 dollyCenter(rotateCenter.x - spacing * 0.5f - toolRadius, toolCenterY);
    ImVec2 panCenter(rotateCenter.x + spacing * 0.5f + toolRadius, toolCenterY);

    ImVec2 rotatePos = rotateCenter;
    ImVec2 dollyPos(dollyCenter.x - toolRadius, dollyCenter.y - toolRadius);
    ImVec2 panPos(panCenter.x - toolRadius, panCenter.y - toolRadius);

    // Contrast-adaptive tint for the dolly / pan tool buttons and the projection label: sample the
    // scene luminance behind the cluster and steer the glyphs white-on-dark / dark-on-light — the
    // same readback the corner engine mark and the viewport Play button use (throttled ~10 Hz,
    // eased per frame). The rotate ball keeps its own red/green/blue axis colours untouched.
    {
        m_NavGizmoSampleAccum += ImGui::GetIO().DeltaTime;
        if (m_NavGizmoSampleAccum >= 0.1f) {
            m_NavGizmoSampleAccum = 0.0f;
            float lum = SampleSceneLuminance(ImVec2(rotateCenter.x, toolCenterY), 48.0f * m_UIScale);
            if (lum >= 0.0f) {
                float t = (lum - 0.30f) / (0.62f - 0.30f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                m_NavGizmoContrastTarget = 1.0f - t * t * (3.0f - 2.0f * t); // 1 = white on dark, 0 = black on light
            }
        }
        float k = 1.0f - expf(-ImGui::GetIO().DeltaTime / 0.15f);
        m_NavGizmoContrastLum += (m_NavGizmoContrastTarget - m_NavGizmoContrastLum) * k;
    }
    int navV = (int)(m_NavGizmoContrastLum * 255.0f + 0.5f);
    navV = navV < 0 ? 0 : (navV > 255 ? 255 : navV);
    const int navInv = 255 - navV;
    style.toolButtonIconColor    = IM_COL32(navV, navV, navV, 235);
    style.toolButtonColor        = IM_COL32(navInv, navInv, navInv, 40);
    style.toolButtonHoveredColor = IM_COL32(navInv, navInv, navInv, 64);

    // Same fullscreen-transparent-overlay trick as DrawGizmo(): the library hit-tests against
    // raw mouse position within ImGui::GetWindowDrawList()'s owning window, so it needs a real
    // hoverable window as the "current window"; NoInputs keeps it from stealing
    // WantCaptureMouse everywhere else (which would otherwise block the editor fly-camera).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##ViewGizmoOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    // See DrawGizmo's identical call for why this is needed - without it, "Scene" (newer, so
    // higher in ImGui's window stack) covers this NoInputs overlay instead of the other way
    // around.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    glm::vec3 camPos = editorCamera.Position;
    glm::quat camRot = glm::quatLookAt(editorCamera.Front(), glm::vec3(0.0f, 1.0f, 0.0f));
    // Orbit pivot for the Rotate ring: the current selection's bounds center when there is one,
    // so dragging to orbit or clicking an axis handle to snap keeps the selected object(s)
    // centered instead of spinning around an arbitrary point. Falls back to a fixed distance in
    // front of the camera (the old behavior) when nothing's selected — the editor camera is a
    // plain fly-camera with no scene pivot of its own otherwise.
    glm::vec3 pivot;
    glm::vec3 selBoundsMin, selBoundsMax;
    if (ComputeSelectionBounds(world, selBoundsMin, selBoundsMax)) {
        pivot = (selBoundsMin + selBoundsMax) * 0.5f;
    } else {
        pivot = camPos + editorCamera.Front() * 6.0f;
    }

    bool modified = false;
    modified |= ImViewGuizmo::Rotate(camPos, camRot, pivot, rotatePos);
    modified |= ImViewGuizmo::Dolly(camPos, camRot, dollyPos);
    modified |= ImViewGuizmo::Pan(camPos, camRot, panPos);
    m_ViewGizmoBlocking = ImViewGuizmo::IsOver() || ImViewGuizmo::IsUsing();

    // Tooltips - the tool buttons above give no feedback on their own (they're manually
    // hit-tested inside the vendored gizmo library, not real ImGui widgets, so
    // ImGui::IsItemHovered() can't see them); read the library's own hover state instead.
    const auto& gizmoCtx = ImViewGuizmo::GetContext();
    if (gizmoCtx.hoveredAxisID == 6) {
        EditorUI::SetTooltip("Drag to orbit the view");
    } else if (gizmoCtx.hoveredAxisID >= 0 && gizmoCtx.hoveredAxisID <= 5) {
        static const char* kAxisTooltips[6] = {
            "Click to look along +X",
            "Click to look along -X",
            "Click to look straight down (+Y)",
            "Click to look straight up (-Y)",
            "Click to look along +Z",
            "Click to look along -Z",
        };
        EditorUI::SetTooltip("%s", kAxisTooltips[gizmoCtx.hoveredAxisID]);
    } else if (gizmoCtx.isZoomButtonHovered) {
        EditorUI::SetTooltip("Click and drag to dolly the view closer to/further from the pivot");
    } else if (gizmoCtx.isPanButtonHovered) {
        EditorUI::SetTooltip("Click and drag to pan the view");
    }

    // "Persp"/"Iso" label under the gizmo, mirroring Unity's own - click to switch the editor
    // camera between perspective and orthographic (isometric) projection without changing the
    // current viewing angle. Manually hit-tested against the raw mouse position, same as the
    // tool buttons above, since this overlay window is ImGuiWindowFlags_NoInputs.
    {
        // Name the view when the camera is aligned to a canonical axis (Front/Right/Top/...),
        // as set by the nav-gizmo axis handles or the numpad views — falling back to
        // Persp/Iso only when it's a free angle (#25 P14).
        const char* isoLabel = [&]() -> const char* {
            const glm::vec3 f = editorCamera.Front();
            const float k = 0.999f; // within ~2.5 degrees of dead-on
            if (f.z < -k) return "Front";
            if (f.z >  k) return "Back";
            if (f.x < -k) return "Right";
            if (f.x >  k) return "Left";
            if (f.y < -k) return "Top";
            if (f.y >  k) return "Bottom";
            return editorCamera.Orthographic ? "Iso" : "Persp";
        }();
        ImFont* font = ImGui::GetFont();
        float labelFontSize = ImGui::GetFontSize();
        ImVec2 textSize = font->CalcTextSizeA(labelFontSize, FLT_MAX, 0.0f, isoLabel);
        ImVec2 textPos(rotateCenter.x - textSize.x * 0.5f, toolCenterY + toolRadius + spacing);
        ImVec2 padding(4.0f * m_UIScale, 2.0f * m_UIScale);
        ImVec2 hitMin(textPos.x - padding.x, textPos.y - padding.y);
        ImVec2 hitMax(textPos.x + textSize.x + padding.x, textPos.y + textSize.y + padding.y);
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool isoHovered = mouse.x >= hitMin.x && mouse.x <= hitMax.x && mouse.y >= hitMin.y && mouse.y <= hitMax.y;
        ImDrawList* labelDl = ImGui::GetWindowDrawList();
        if (isoHovered) {
            labelDl->AddRectFilled(hitMin, hitMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 3.0f * m_UIScale);
            EditorUI::SetTooltip("Switch between Perspective and Isometric (orthographic) view");
            m_ViewGizmoBlocking = true;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ToggleOrthographic(world, editorCamera);
            }
        }
        // Same contrast-adaptive grey as the tool buttons above (full-strength on hover).
        ImU32 isoTextColor = IM_COL32(navV, navV, navV, isoHovered ? 255 : 200);
        labelDl->AddText(font, labelFontSize, textPos, isoTextColor, isoLabel);
    }

    if (modified) {
        glm::vec3 forward = glm::normalize(camRot * glm::vec3(0.0f, 0.0f, -1.0f));
        editorCamera.Pitch = glm::clamp(glm::degrees(asinf(glm::clamp(forward.y, -1.0f, 1.0f))), -89.0f, 89.0f);
        editorCamera.Yaw = glm::degrees(atan2f(forward.z, forward.x));
        editorCamera.Position = camPos;
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::DrawGroupGizmo(World& world, Camera& editorCamera) {
    struct Ref { glm::vec3* pos; glm::vec3* rot; glm::vec3* scale; };
    std::vector<Ref> refs;
    auto addRef = [&](entt::entity entity) {
        if (entity == entt::null || !world.Registry.valid(entity)) return;
        auto& transform = world.Registry.get<TransformComponent>(entity);
        refs.push_back({&transform.Position, &transform.RotationEuler, &transform.Scale});
    };
    addRef(m_Selected);
    for (entt::entity e : m_ExtraSelection) addRef(e);
    if (refs.empty()) { m_GizmoEngaged = false; return; }

    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0) return;

    // Same fullscreen-overlay approach as the single-object gizmo (see its comment) — needed
    // so ImGuizmo's hit-testing has a real window to test hover/click against.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##GroupGizmoOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    // See DrawGizmo's identical call for why this is needed.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // ImGuizmo hit-tests against its own draw-list window (this NoInputs overlay), which is never
    // ImGui's g.HoveredWindow — so without this, hovering the actual "Scene" panel makes
    // IsHoveringWindow() return false and the handles draw but never grab. Registering "Scene" as
    // the alternative window is ImGuizmo's supported way to say "the user hovers there, not here".
    ImGuizmo::SetAlternativeWindow(ImGui::FindWindowByName("Scene"));

    ImGuizmo::SetOrthographic(editorCamera.Orthographic); // #107 — ortho views used to offset the handles
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
    ImGuizmo::SetGizmoSizeClipSpace(m_GizmoSize);
    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

    // GizmoOp::Rect falls through to plain TRANSLATE here (no bounds-handle case below, unlike
    // DrawGizmo) — a group's members don't share one native mesh extent, so there's no single
    // coherent bounding box to hang scale handles on; translating the whole group still works.
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (m_GizmoOp == GizmoOp::Rotate) op = ImGuizmo::ROTATE;
    else if (m_GizmoOp == GizmoOp::Scale) op = ImGuizmo::SCALE;

    // Only re-center the pivot on the group's current average position when a drag ISN'T in
    // progress — while one is, m_GroupGizmoMatrix is the evolving frame of reference and
    // recomputing it from the (already partway-moved) objects would fight the drag.
    if (!m_GizmoWasUsing) {
        glm::vec3 pivot(0.0f);
        for (auto& r : refs) pivot += *r.pos;
        pivot /= (float)refs.size();
        m_GroupGizmoMatrix = glm::translate(glm::mat4(1.0f), pivot);
    }
    glm::mat4 matrixBefore = m_GroupGizmoMatrix;

    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float snapValues[3] = {m_SnapTranslation, m_SnapTranslation, m_SnapTranslation};
    if (m_GizmoOp == GizmoOp::Rotate) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapRotationDeg;
    else if (m_GizmoOp == GizmoOp::Scale) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapScale;

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(m_GroupGizmoMatrix), nullptr, snapActive ? snapValues : nullptr);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    bool isUsingNow = ImGuizmo::IsUsing();
    if (isUsingNow && !m_GizmoWasUsing) {
        PushUndo(world, GizmoOpUndoLabel(m_GizmoOp));
    }

    if (isUsingNow) {
        // The pivot moved by `delta` this frame — apply that same rigid transform to every
        // selected object's own world matrix, so the group rotates/scales around the shared
        // pivot instead of each object spinning in place around its own center.
        glm::mat4 delta = m_GroupGizmoMatrix * glm::inverse(matrixBefore);
        for (auto& r : refs) {
            glm::mat4 objMatrix = ComposeTransform(*r.pos, *r.rot, *r.scale);
            glm::mat4 newMatrix = delta * objMatrix;
            float nt[3], nr[3], ns[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(newMatrix), nt, nr, ns);
            *r.pos = {nt[0], nt[1], nt[2]};
            *r.rot = EulerYXZFromMatrix(newMatrix); // ComposeTransform order, not ImGuizmo's (#108)
            *r.scale = {ns[0], ns[1], ns[2]};
        }
    }
    m_GizmoWasUsing = isUsingNow;

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::BeginRenameAsset(const std::string& key, bool isFolder, const std::string& currentName) {
    m_RenamingAssetKey = key;
    m_RenamingIsFolder = isFolder;
    m_RenamingJustStarted = true;
    snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", currentName.c_str());
    m_SelectedAssetKey = key;
    m_SelectedAssetIsFolder = isFolder;
}

bool EditorLayer::IsAssetSelected(const std::string& key, bool isFolder) const {
    if (m_SelectedAssetKey == key && m_SelectedAssetIsFolder == isFolder) return true;
    for (const auto& e : m_ExtraAssetSelection) {
        if (e.Key == key && e.IsFolder == isFolder) return true;
    }
    return false;
}

void EditorLayer::ClearAssetSelection() {
    m_SelectedAssetKey.clear();
    m_ExtraAssetSelection.clear();
}

void EditorLayer::ToggleAssetSelection(const std::string& key, bool isFolder) {
    if (m_SelectedAssetKey.empty()) {
        m_SelectedAssetKey = key;
        m_SelectedAssetIsFolder = isFolder;
        return;
    }
    if (m_SelectedAssetKey == key && m_SelectedAssetIsFolder == isFolder) {
        // Toggling off the primary — promote an extra selection to take its place, if any.
        if (!m_ExtraAssetSelection.empty()) {
            m_SelectedAssetKey = m_ExtraAssetSelection.back().Key;
            m_SelectedAssetIsFolder = m_ExtraAssetSelection.back().IsFolder;
            m_ExtraAssetSelection.pop_back();
        } else {
            m_SelectedAssetKey.clear();
        }
        return;
    }
    for (auto it = m_ExtraAssetSelection.begin(); it != m_ExtraAssetSelection.end(); ++it) {
        if (it->Key == key && it->IsFolder == isFolder) {
            m_ExtraAssetSelection.erase(it); // already co-selected — toggle it back off
            return;
        }
    }
    m_ExtraAssetSelection.push_back({key, isFolder});
}

void EditorLayer::RequestDeleteAsset(World& world, AssetLibrary& assets, const std::string& key, bool isFolder, bool skipDialog) {
    if (key.empty()) return;
    RequestDeleteAssets(world, assets, { AssetKeyRef{key, isFolder} }, skipDialog);
}

void EditorLayer::RequestDeleteAssets(World& world, AssetLibrary& assets, const std::vector<AssetKeyRef>& items, bool skipDialog) {
    if (items.empty()) return;
    if (skipDialog) {
        m_DeleteError.clear();
        PushUndo(world, items.size() == 1
            ? (items[0].IsFolder ? "Delete Folder" : "Delete Asset")
            : "Delete Assets");
        for (const auto& item : items) PerformAssetDelete(world, assets, item.Key, item.IsFolder);
        ClearAssetSelection();
        if (!m_DeleteError.empty()) m_OpenDeleteErrorRequested = true;
        return;
    }
    m_PendingDelete = items;
    m_OpenDeleteConfirmRequested = true;
}

bool EditorLayer::PerformAssetDelete(World& world, AssetLibrary& assets, const std::string& key, bool isFolder) {
    (void)world; // undo is now pushed once by the caller, covering the whole batch (#212)
    InvalidateModelThumbnail(nullptr); // a freed Model could be reallocated at the same address
    const std::string leaf = std::filesystem::path(key).filename().string();
    if (isFolder) {
        assets.DeleteFolderRecursive(key);
        // Don't leave the browser pointed at a folder that no longer exists.
        if (m_CurrentAssetFolder == key || m_CurrentAssetFolder.rfind(key + "/", 0) == 0) {
            m_CurrentAssetFolder = ParentFolderOf(key);
        }
    } else {
        for (const auto& model : assets.Models()) {
            if (model->Path() == key) { assets.RemoveModel(model); break; }
        }
        for (const auto& tex : assets.Textures()) {
            if (tex->Path() == key) { assets.RemoveTexture(tex); break; }
        }
        for (const auto& sound : assets.Sounds()) {
            if (sound == key) { assets.RemoveSound(sound); break; }
        }
        for (const auto& prefab : assets.Prefabs()) {
            if (prefab == key) { assets.RemovePrefab(prefab); break; }
        }
        // A Scene entry is a real .json file, not an AssetLibrary asset — remove it from disk
        // directly, and report why if that fails.
        std::error_code ec;
        const bool isScene = std::filesystem::path(key).extension() == ".json" &&
                             std::filesystem::exists(key, ec);
        if (isScene) {
            // Accumulate reasons so a batch delete reports every item that couldn't be removed.
            auto fail = [&](const std::string& msg) {
                if (!m_DeleteError.empty()) m_DeleteError += "\n";
                m_DeleteError += "\xE2\x80\xA2 " + msg; // "• "
            };
            if (key == m_CurrentScenePath) {
                fail("\"" + leaf + "\" is the scene you have open.");
                return false;
            }
            std::string why;
            if (FileDialog::RecycleFile(key, why)) {
                Log::Info("Sent scene file to Recycle Bin: " + leaf);
                if (EditorSettings::Get().LastScenePath == key) {
                    EditorSettings::Get().LastScenePath.clear();
                    EditorSettings::Save();
                }
            } else {
                if (why.empty()) why = "the file may be open in another program or write-protected";
                fail("\"" + leaf + "\" — " + why + ".");
                Log::Error("Couldn't recycle scene file '" + key + "': " + why);
                return false;
            }
        }

        // A screenshot is also a plain file on disk (project/screenshots/).
        std::string ext = std::filesystem::path(key).extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if ((ext == ".png" || ext == ".jpg" || ext == ".jpeg") && std::filesystem::exists(key, ec)) {
            m_ShotThumbs.erase(key);
            std::string why;
            if (FileDialog::RecycleFile(key, why)) {
                Log::Info("Sent screenshot to Recycle Bin: " + leaf);
            } else {
                if (why.empty()) why = "the file may be open in another program or write-protected";
                if (!m_DeleteError.empty()) m_DeleteError += "\n";
                m_DeleteError += "\xE2\x80\xA2 \"" + leaf + "\" — " + why + ".";
                return false;
            }
        }
    }
    if (m_SelectedAssetKey == key) m_SelectedAssetKey.clear();
    return true;
}

void EditorLayer::DrawDeleteConfirmPopup(World& world, AssetLibrary& assets) {
    const char* kPopupId = "Delete Asset?";
    if (m_OpenDeleteConfirmRequested) {
        ImGui::OpenPopup(kPopupId);
        m_OpenDeleteConfirmRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(340.0f * m_UIScale, 0.0f));
    if (ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f * m_UIScale);
        if (m_PendingDelete.size() == 1) {
            ImGui::Text("Delete \"%s\"?", LeafNameOf(m_PendingDelete[0].Key).c_str());
        } else {
            ImGui::Text("Delete %d selected items?", (int)m_PendingDelete.size());
        }
        bool anyNonEmptyFolder = false;
        for (const auto& item : m_PendingDelete) {
            if (item.IsFolder && !assets.CanDeleteFolder(item.Key)) { anyNonEmptyFolder = true; break; }
        }
        if (anyNonEmptyFolder) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "One or more of these folders isn't empty - everything inside, including subfolders, will be removed too.");
        }
        // Scenes and screenshots are real files on disk, not AssetLibrary entries — deleting them
        // erases the file (sent to the Recycle Bin, not the "Undoable" history the line below
        // promises for library assets). Say so explicitly rather than a blanket "Undoable" (#211).
        size_t realFileCount = 0;
        for (const auto& item : m_PendingDelete) {
            if (item.IsFolder) continue;
            std::string ext = std::filesystem::path(item.Key).extension().string();
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".json" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") ++realFileCount;
        }
        if (realFileCount == m_PendingDelete.size()) {
            ImGui::TextDisabled(realFileCount == 1
                ? "This is a real file on disk, not a library asset - it will be sent to the Recycle Bin."
                : "These are real files on disk, not library assets - they will be sent to the Recycle Bin.");
        } else {
            ImGui::TextDisabled("Objects already placed in the scene keep working - this only removes it from the Asset Browser. Undoable.");
            if (realFileCount == 1) {
                ImGui::TextDisabled("1 of these is a real file on disk and will be sent to the Recycle Bin instead.");
            } else if (realFileCount > 1) {
                ImGui::TextDisabled("%zu of these are real files on disk and will be sent to the Recycle Bin instead.", realFileCount);
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // Enter or Delete confirms, Esc cancels — the prompt can be cleared without the mouse.
        const bool keyConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                ImGui::IsKeyPressed(ImGuiKey_Delete);
        const bool keyCancel  = ImGui::IsKeyPressed(ImGuiKey_Escape);

        float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (PrimaryButton("Cancel", ImVec2(buttonWidth, 0.0f)) || keyCancel) {
            m_PendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Delete", ImVec2(buttonWidth, 0.0f)) || keyConfirm) {
            m_DeleteError.clear();
            PushUndo(world, m_PendingDelete.size() == 1
                ? (m_PendingDelete[0].IsFolder ? "Delete Folder" : "Delete Asset")
                : "Delete Assets");
            for (const auto& item : m_PendingDelete) PerformAssetDelete(world, assets, item.Key, item.IsFolder);
            m_PendingDelete.clear();
            ClearAssetSelection();
            ImGui::CloseCurrentPopup();
            if (!m_DeleteError.empty()) m_OpenDeleteErrorRequested = true;
        }
        ImGui::EndPopup();
    }

    // Follow-up modal: something couldn't be deleted — explain why, OK to dismiss.
    const char* kErrPopupId = "Can't Delete";
    if (m_OpenDeleteErrorRequested) {
        ImGui::OpenPopup(kErrPopupId);
        m_OpenDeleteErrorRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(340.0f * m_UIScale, 0.0f));
    if (ImGui::BeginPopupModal(kErrPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f * m_UIScale);
        const bool several = m_DeleteError.find('\n') != std::string::npos;
        ImGui::TextUnformatted(several ? "Some items couldn't be deleted:" : "That item couldn't be deleted:");
        ImGui::Spacing();
        ImGui::TextUnformatted(m_DeleteError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const bool dismiss = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                             ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (PrimaryButton("OK", ImVec2(-FLT_MIN, 0.0f)) || dismiss) {
            m_DeleteError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::DuplicateSelectedAsset(World& world, AssetLibrary& assets) {
    std::vector<AssetKeyRef> targets;
    if (!m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) targets.push_back({m_SelectedAssetKey, false});
    for (const auto& e : m_ExtraAssetSelection) {
        if (!e.IsFolder) targets.push_back(e); // folders aren't duplicable — silently skipped
    }
    if (targets.empty()) return;

    // Duplicates one asset's file on disk to a numbered sibling and registers it the same way
    // importing it fresh would. Returns the new path, or empty if the file's gone, the copy
    // failed, or it's not an AssetLibrary-tracked kind (e.g. a Scene — file copied, nothing
    // further to register, so not worth reselecting).
    auto duplicateOne = [&](const std::string& key) -> std::string {
        std::filesystem::path srcPath(key);
        std::error_code existsErr;
        if (!std::filesystem::exists(srcPath, existsErr)) {
            Log::Error("Can't duplicate '" + key + "' - the file no longer exists on disk.");
            return {};
        }

        std::string stem = srcPath.stem().string();
        std::string ext = srcPath.extension().string();
        std::filesystem::path dir = srcPath.parent_path();
        std::filesystem::path candidate;
        int n = 1;
        do {
            candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
            n++;
        } while (std::filesystem::exists(candidate));

        std::error_code copyErr;
        std::filesystem::copy_file(srcPath, candidate, copyErr);
        if (copyErr) {
            Log::Error("Failed to duplicate '" + key + "': " + copyErr.message());
            return {};
        }

        std::string newPath = candidate.generic_string();
        std::string folder = assets.AssetFolder(key);

        bool registered = false;
        for (const auto& model : assets.Models()) {
            if (model->Path() == key) { assets.LoadModel(newPath); registered = true; break; }
        }
        if (!registered) for (const auto& tex : assets.Textures()) {
            if (tex->Path() == key) { assets.LoadTexture(newPath); registered = true; break; }
        }
        if (!registered) for (const auto& sound : assets.Sounds()) {
            if (sound == key) { if (AudioEngine::Load(newPath)) { assets.RegisterSound(newPath); registered = true; } break; }
        }
        if (!registered) for (const auto& prefab : assets.Prefabs()) {
            if (prefab == key) { assets.RegisterPrefab(newPath); registered = true; break; }
        }

        Log::Info("Duplicated '" + srcPath.filename().string() + "' -> '" + candidate.filename().string() + "'.");
        if (!registered) return {};
        assets.SetAssetFolder(newPath, folder);
        return newPath;
    };

    PushUndo(world, targets.size() > 1 ? "Duplicate Assets" : "Duplicate Asset");
    std::vector<std::string> newKeys;
    for (const auto& t : targets) {
        std::string newKey = duplicateOne(t.Key);
        if (!newKey.empty()) newKeys.push_back(newKey);
    }

    // Select the duplicates afterward, same as Unity's own Ctrl+D.
    if (!newKeys.empty()) {
        ClearAssetSelection();
        m_SelectedAssetKey = newKeys[0];
        m_SelectedAssetIsFolder = false;
        for (size_t i = 1; i < newKeys.size(); ++i) m_ExtraAssetSelection.push_back({newKeys[i], false});
    }
}

void EditorLayer::CommitRename(World& world, AssetLibrary& assets) {
    std::string newName = m_RenameBuffer;
    if (!newName.empty()) {
        if (m_RenamingIsFolder) {
            PushUndo(world, "Rename Folder");
            std::string parent = ParentFolderOf(m_RenamingAssetKey);
            std::string newPath = parent.empty() ? newName : (parent + "/" + newName);
            assets.RenameFolder(m_RenamingAssetKey, newPath);
            if (m_CurrentAssetFolder == m_RenamingAssetKey) {
                m_CurrentAssetFolder = newPath;
            } else if (m_CurrentAssetFolder.rfind(m_RenamingAssetKey + "/", 0) == 0) {
                m_CurrentAssetFolder = newPath + m_CurrentAssetFolder.substr(m_RenamingAssetKey.size());
            }
            if (m_SelectedAssetKey == m_RenamingAssetKey) m_SelectedAssetKey = newPath;
        } else {
            PushUndo(world, "Rename Asset");
            assets.SetDisplayName(m_RenamingAssetKey, newName);
        }
    }
    m_RenamingAssetKey.clear();
}

void EditorLayer::SetFolderExpandedRecursive(AssetLibrary& assets, const std::string& folderPath, bool expand, bool recursive) {
    if (expand) m_ExpandedAssetFolders.insert(folderPath);
    else m_ExpandedAssetFolders.erase(folderPath);
    if (!recursive) return;
    for (const auto& f : assets.Folders()) {
        if (ParentFolderOf(f) == folderPath) SetFolderExpandedRecursive(assets, f, expand, true);
    }
}

namespace {
// Draws `text` as at most `maxLines` lines that fit `wrapWidth`, each line horizontally
// centred within [pos.x, pos.x + wrapWidth] under the tile's icon. Breaks on word boundaries
// where possible; if the whole string still doesn't fit, the last line is trimmed at the
// character level and gets a trailing "…". Replaces ImDrawList::AddText's own wrap_width mode,
// which splits a single long word across lines ("Chesterfi / eld Sofa"). Returns true when
// anything was clipped, so the caller can add a hover tooltip with the full name.
bool DrawClampedGridLabel(ImDrawList* dl, ImVec2 pos, float wrapWidth, float lineHeight,
                          ImU32 color, const char* text, int maxLines = 2) {
    ImFont* font = ImGui::GetFont();
    const float sz = ImGui::GetFontSize();
    const char* const textEnd = text + strlen(text);
    const char* s = text;
    const float left = pos.x;

    // Draw one already-delimited run centred on this line, then advance to the next line.
    auto drawCentered = [&](const char* a, const char* b) {
        float w = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, a, b).x;
        dl->AddText(font, sz, ImVec2(left + (wrapWidth - w) * 0.5f, pos.y), color, a, b);
        pos.y += lineHeight;
    };

    for (int line = 0; line < maxLines; ++line) {
        if (s >= textEnd) return false;
        const bool lastLine = (line == maxLines - 1);

        // Whole remainder fits on this line?
        if (font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s, textEnd).x <= wrapWidth) {
            drawCentered(s, textEnd);
            return false;
        }

        if (!lastLine) {
            // Word-wrap break for this line, then continue with the rest below.
            const char* brk = font->CalcWordWrapPosition(sz, s, textEnd, wrapWidth);
            if (brk <= s) brk = s + 1; // guarantee forward progress on an unbreakable word
            drawCentered(s, brk);
            s = brk;
            while (s < textEnd && (*s == ' ' || *s == '\n')) ++s; // skip the breaking blank
            continue;
        }

        // Last line and it overflows: character-trim to leave room for the ellipsis, then
        // centre the visible text + "…" together.
        static const char* kEllipsis = "\xE2\x80\xA6";
        const float ellW = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, kEllipsis).x;
        const char* fitEnd = s;
        font->CalcTextSizeA(sz, ImMax(wrapWidth - ellW, 1.0f), 0.0f, s, textEnd, &fitEnd);
        if (fitEnd <= s) fitEnd = s + 1; // always show at least one glyph
        const float textW = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s, fitEnd).x;
        const float x0 = left + (wrapWidth - (textW + ellW)) * 0.5f;
        dl->AddText(font, sz, ImVec2(x0, pos.y), color, s, fitEnd);
        dl->AddText(font, sz, ImVec2(x0 + textW, pos.y), color, kEllipsis);
        return true;
    }
    return true;
}
} // namespace

// Unity Project window's left pane: a real folder hierarchy (not just the breadcrumb above the
// grid) with expand/collapse arrows. Alt+click recursively expands/collapses every descendant in
// one step - the same gesture Unity uses - which is why open/closed state is tracked in
// m_ExpandedAssetFolders rather than left to ImGui's own per-ID tree memory (that has no hook
// for "and everything under it too").
void EditorLayer::DrawFolderTreeNode(World& world, AssetLibrary& assets, const std::string& folderPath, bool isRoot) {
    std::vector<std::string> children;
    for (const auto& f : assets.Folders()) {
        if (ParentFolderOf(f) == folderPath) children.push_back(f);
    }
    std::sort(children.begin(), children.end(), [](const std::string& a, const std::string& b) {
        return LeafNameOf(a) < LeafNameOf(b);
    });

    bool hasChildren = !children.empty();
    bool wasExpanded = isRoot || m_ExpandedAssetFolders.count(folderPath) > 0;
    if (!isRoot) ImGui::SetNextItemOpen(wasExpanded);

    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_CurrentAssetFolder == folderPath) nodeFlags |= ImGuiTreeNodeFlags_Selected;
    if (!hasChildren) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isRoot) nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;

    std::string label = std::string(isRoot ? ICON_FA_FOLDER_TREE : ICON_FA_FOLDER) + "  " + (isRoot ? "Assets" : LeafNameOf(folderPath));

    ImGui::PushID(folderPath.c_str());
    bool open = ImGui::TreeNodeEx("##node", nodeFlags, "%s", label.c_str());

    // (#157) One-shot scroll so a folder selected from elsewhere lands in view.
    if (!isRoot && m_RevealAssetFolderInTree && folderPath == m_CurrentAssetFolder) {
        ImGui::SetScrollHereY(0.5f);
        m_RevealAssetFolderInTree = false;
    }

    if (!isRoot && open != wasExpanded) {
        // The user just clicked THIS node's arrow this frame (ImGui's own toggle already ran,
        // which is why `open` differs from what we told it to be) - Alt turns it into "and
        // every descendant folder too."
        SetFolderExpandedRecursive(assets, folderPath, open, ImGui::GetIO().KeyAlt);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_CurrentAssetFolder = folderPath;
        ClearAssetSelection();
        m_SelectedAssetKey = folderPath;
        m_SelectedAssetIsFolder = true;
    }

    if (!isRoot) {
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", folderPath.c_str(), folderPath.size() + 1);
            ImGui::TextUnformatted(LeafNameOf(folderPath).c_str());
            ImGui::EndDragDropSource();
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
            PushUndo(world, "Move Asset to Folder");
            assets.SetAssetFolder((const char*)p->Data, folderPath);
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
            PushUndo(world, "Move Asset to Folder");
            assets.SetAssetFolder((const char*)p->Data, folderPath);
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SOUND_PATH")) {
            PushUndo(world, "Move Asset to Folder");
            assets.SetAssetFolder((const char*)p->Data, folderPath);
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH")) {
            PushUndo(world, "Move Asset to Folder");
            assets.SetAssetFolder((const char*)p->Data, folderPath);
        }
        if (!isRoot) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                std::string src((const char*)p->Data);
                if (src != folderPath && folderPath.rfind(src + "/", 0) != 0) {
                    PushUndo(world, "Move Folder");
                    assets.RenameFolder(src, folderPath + "/" + LeafNameOf(src));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
        EditorUI::SetTooltip(isRoot
            ? "The root of every asset the editor knows about.\nAlt+click a folder's arrow to expand/collapse it and everything under it."
            : "Click to browse. Drag assets or other folders onto it to file them here.");
    }
    ImGui::PopID();

    if (open) {
        for (const auto& child : children) DrawFolderTreeNode(world, assets, child, false);
        // TreeNodeEx sets NoTreePushOnOpen (so no ID-stack push happens) whenever !hasChildren,
        // root or not - so TreePop() must be gated on hasChildren alone. The old `|| isRoot` here
        // called TreePop() for a root with zero folders even though nothing was pushed, a
        // mismatch that spammed "PopID() too many times" / "Missing TreePop()" every frame.
        if (hasChildren) ImGui::TreePop();
    }
}

void EditorLayer::DrawAssetBrowser(World& world, AssetLibrary& assets) {
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    // Tighter vertical padding than the default so the toolbar hugs the tab bar instead of
    // floating below a large gap.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    bool open = ImGui::Begin("Asset Browser", &m_ShowAssetBrowser, flags);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }
    m_AssetBrowserFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    auto makeNewFolder = [&]() {
        std::string base = m_CurrentAssetFolder.empty() ? "New Folder" : (m_CurrentAssetFolder + "/New Folder");
        std::string candidate = base;
        int n = 1;
        auto exists = [&](const std::string& p) {
            for (const auto& f : assets.Folders()) if (f == p) return true;
            return false;
        };
        while (exists(candidate)) candidate = base + " (" + std::to_string(n++) + ")";
        PushUndo(world, "Create Folder");
        assets.CreateFolder(candidate);
        BeginRenameAsset(candidate, true, LeafNameOf(candidate));
    };

    // Single toolbar row: New Folder, a breadcrumb, and a search box pinned to the right
    // edge — instead of the button and search box stacking on separate cramped lines. Zero
    // padding so the row hugs the tab bar directly above it instead of floating with a gap.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##AssetToolbar", ImVec2(0, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const float searchWidth = 200.0f;

    // One "+ Create / Import" entry point instead of New-Folder-only up front and Import buried
    // in the menu bar (audit #79).
    if (ActionButton(ICON_FA_PLUS, "Create / Import")) ImGui::OpenPopup("##AssetCreateMenu");
    if (ImGui::BeginPopup("##AssetCreateMenu")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) makeNewFolder();
        ImGui::Separator();
        auto importInto = [&](const char* filter) {
            std::string p = FileDialog::OpenFile(filter, m_Window);
            if (!p.empty() && m_EditorCameraPtr)
                ImportDroppedFile(world, assets, *m_EditorCameraPtr, p, m_CurrentAssetFolder);
        };
        if (ImGui::MenuItem(ICON_FA_CUBE "  Import Model..."))
            importInto("3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0");
        if (ImGui::MenuItem(ICON_FA_IMAGE "  Import Texture..."))
            importInto("Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0");
        if (ImGui::MenuItem(ICON_FA_MUSIC "  Import Sound..."))
            importInto("Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0");
        ImGui::EndPopup();
    }

    EditorUI::VSeparator();

    // Breadcrumb (#157): non-interactive path text for context only — "Assets / Sub / Folder".
    // The folder tree on the left is how you actually move; this just says where you are, so
    // it's a single dim string with no framed per-segment buttons or "/" glyphs.
    {
        std::string crumb = "Assets";
        for (char c : m_CurrentAssetFolder) {
            if (c == '/') crumb += " / ";
            else          crumb += c;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", crumb.c_str());
    }

    // Search box + its two filter buttons (Type, Label), pinned as one group to the toolbar's
    // right edge - falling back to a new line only if the breadcrumb has grown too long to
    // leave room for it.
    float filterButtonsWidth = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x) * 1.0f; // one "Filters" button now
    float groupWidth = searchWidth + filterButtonsWidth;
    float targetX = ImGui::GetWindowContentRegionMax().x - groupWidth;
    if (targetX > ImGui::GetCursorPosX()) ImGui::SameLine(targetX);
    else ImGui::NewLine();

    char filterBuf[64];
    snprintf(filterBuf, sizeof(filterBuf), "%s", m_AssetSearchFilter.c_str());
    ImGui::SetNextItemWidth(searchWidth);
    if (m_AssetSearchFocusRequested) {
        ImGui::SetKeyboardFocusHere();
        m_AssetSearchFocusRequested = false;
    }
    if (ImGui::InputTextWithHint("##AssetFilter", ICON_FA_MAGNIFYING_GLASS "  Search...", filterBuf, sizeof(filterBuf))) {
        m_AssetSearchFilter = filterBuf;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
        EditorUI::SetTooltip(
            "Search every asset by name, across all folders.\n"
            "Type multiple words to match all of them (AND).\n"
            "t:Model / t:Texture / t:Sound / t:Scene / t:Prefab / t:Folder\n"
            "  restricts by type - listing several ORs them together.\n"
            "l:label restricts by label (set in an asset's right-click\n"
            "  menu) - listing several ANDs them, requiring every one.");
    }

    // One "Filters" menu instead of a separate type-filter and label-filter button next to the
    // search box (audit #80). Both just toggle t:/l: tokens in the one search string, so folding
    // them together removes two near-identical affordances without losing anything. The lit dot
    // shows when any type/label token is active.
    bool anyFilterActive = false;
    for (const char* t : {"t:model", "t:texture", "t:sound", "t:scene", "t:prefab", "t:folder"})
        anyFilterActive |= SearchHasToken(m_AssetSearchFilter, t);
    for (const auto& lbl : assets.AllKnownLabels())
        anyFilterActive |= SearchHasToken(m_AssetSearchFilter, "l:" + lbl);

    ImGui::SameLine();
    if (anyFilterActive) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    bool openFilters = ImGui::Button(ICON_FA_FILTER);
    if (anyFilterActive) ImGui::PopStyleColor();
    if (openFilters) ImGui::OpenPopup("##AssetFilters");
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Filter by asset type or label");
    if (ImGui::BeginPopup("##AssetFilters")) {
        ImGui::SeparatorText("Type");
        static const std::pair<const char*, const char*> kTypes[] = {
            {"Model", "model"}, {"Texture", "texture"}, {"Sound", "sound"},
            {"Scene", "scene"}, {"Prefab", "prefab"}, {"Folder", "folder"},
        };
        for (const auto& [label, token] : kTypes) {
            std::string full = std::string("t:") + token;
            bool active = SearchHasToken(m_AssetSearchFilter, full);
            if (ImGui::MenuItem(label, nullptr, active)) ToggleSearchToken(m_AssetSearchFilter, full);
        }

        ImGui::SeparatorText("Label");
        std::set<std::string> allLabels = assets.AllKnownLabels();
        if (allLabels.empty()) {
            ImGui::TextDisabled("No labels yet - add one from an\nasset's right-click menu.");
        } else {
            ImGui::SetNextItemWidth(180.0f);
            char labelSearchBuf[64];
            snprintf(labelSearchBuf, sizeof(labelSearchBuf), "%s", m_AssetLabelMenuFilter.c_str());
            if (ImGui::InputTextWithHint("##LabelMenuFilter", ICON_FA_MAGNIFYING_GLASS "  Search labels...",
                    labelSearchBuf, sizeof(labelSearchBuf))) {
                m_AssetLabelMenuFilter = labelSearchBuf;
            }
            for (const auto& lbl : allLabels) {
                if (!MatchesFilter(m_AssetLabelMenuFilter, lbl)) continue;
                std::string full = "l:" + lbl;
                bool active = SearchHasToken(m_AssetSearchFilter, full);
                if (ImGui::MenuItem(lbl.c_str(), nullptr, active)) ToggleSearchToken(m_AssetSearchFilter, full);
            }
        }

        if (anyFilterActive) {
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_XMARK "  Clear all filters")) {
                // Strip every t:/l: token, leave any free-text search words.
                std::string kept;
                std::stringstream ss(m_AssetSearchFilter);
                std::string w;
                while (ss >> w) {
                    if (w.rfind("t:", 0) == 0 || w.rfind("l:", 0) == 0) continue;
                    kept += (kept.empty() ? "" : " ") + w;
                }
                m_AssetSearchFilter = kept;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild(); // ##AssetToolbar

    ImGui::Separator();

    // Unity Project-window layout: a folder tree on the left (real hierarchy navigation, not
    // just the breadcrumb above) and the current folder's contents as icons on the right,
    // split by a drag-resizable divider.
    float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;

    // No child borders on either pane (#158) — the splitter alone is the divider: a hairline in
    // ImGuiCol_Border at rest, brightening + thickening on hover/drag, inside a 6px hit target.
    // (#157) The tree is the only folder navigator now, so whenever the current folder changes
    // from anywhere else (a folder tile, a search-result click, Backspace), force its ancestors
    // open in the tree and ask the matching node to scroll itself into view. Only fires on an
    // actual change, so the user can still collapse things afterward.
    if (m_CurrentAssetFolder != m_AssetFolderTreeRevealed) {
        m_AssetFolderTreeRevealed = m_CurrentAssetFolder;
        m_RevealAssetFolderInTree = !m_CurrentAssetFolder.empty();
        for (size_t s = 0; s < m_CurrentAssetFolder.size();) {
            size_t slash = m_CurrentAssetFolder.find('/', s);
            m_ExpandedAssetFolders.insert(m_CurrentAssetFolder.substr(
                0, slash == std::string::npos ? m_CurrentAssetFolder.size() : slash));
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
    }

    ImGui::BeginChild("##AssetTree", ImVec2(m_AssetTreeWidth, contentHeight), ImGuiChildFlags_None);
    DrawFolderTreeNode(world, assets, "", true);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    ImGui::Button("##AssetTreeSplitter", ImVec2(6.0f, contentHeight));
    ImGui::PopStyleColor(3);
    {
        const bool active = ImGui::IsItemActive();
        const bool hot = active || ImGui::IsItemHovered();
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float cx = ImFloor((mn.x + mx.x) * 0.5f) + 0.5f;
        const ImU32 col = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                           : hot ? ImGuiCol_SeparatorHovered : ImGuiCol_Border);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, mn.y + 2.0f), ImVec2(cx, mx.y - 2.0f),
                                            col, hot ? 2.0f : 1.0f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) m_AssetTreeWidth += ImGui::GetIO().MouseDelta.x;
    m_AssetTreeWidth = std::clamp(m_AssetTreeWidth, 140.0f * m_UIScale, 460.0f * m_UIScale);
    if (ImGui::IsItemDeactivated()) { // drag finished — remember it
        EditorSettings::Get().AssetBrowserTreeWidth = m_AssetTreeWidth;
        EditorSettings::Save();
    }
    ImGui::SameLine();

    ImGui::BeginChild("##AssetList", ImVec2(0, contentHeight), ImGuiChildFlags_None);

    bool searching = !m_AssetSearchFilter.empty();
    ParsedAssetSearch parsedSearch = ParseAssetSearch(m_AssetSearchFilter);
    // A type/label filter still narrows results even inside a specific folder (not just while
    // searching by name) - e.g. "t:Texture" alone, browsing normally, should hide non-textures
    // right where they are rather than forcing a switch to whole-library search first.
    bool filtering = searching || !parsedSearch.typeTerms.empty() || !parsedSearch.labelTerms.empty();

    struct Cell {
        enum class Kind { Folder, Model, Texture, Sound, Scene, Prefab, Screenshot } kind;
        std::string key;
        std::string display;
        std::shared_ptr<Model> model;
        std::shared_ptr<Texture> texture;
    };
    std::vector<Cell> cells;

    // A special, filesystem-backed folder (not one of AssetLibrary's virtual reference
    // folders) listing every *.json under scenes/ on disk, so scenes can be browsed and
    // opened the same way models/textures/sounds are, instead of only via File > Open.
    static const std::string kScenesFolder = "Scenes";
    assets.CreateFolder(kScenesFolder);
    if (filtering || m_CurrentAssetFolder == kScenesFolder) {
        std::error_code ec;
        // Under the project folder, alongside scene.json — not the working directory (see
        // ProjectPaths.h), so saved scenes are tracked content rather than build output.
        const std::string scenesDir = ProjectPaths::Resolve("scenes");
        std::filesystem::create_directory(scenesDir, ec);
        for (const auto& entry : std::filesystem::directory_iterator(scenesDir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::string path = entry.path().generic_string();
            std::string name = entry.path().stem().string();
            if (!MatchesAssetSearch(parsedSearch, name, "scene", assets.Labels(path))) continue;
            cells.push_back({Cell::Kind::Scene, path, name, nullptr, nullptr});
        }
    }

    // Same idea for the Capture tool's output: a "Screenshots" folder listing every PNG/JPG
    // under project/screenshots/ (thumbnails loaded lazily, cached, and dropped when the file
    // disappears). Not AssetLibrary entries — real image files, browsable and deletable here.
    static const std::string kShotsFolder = "Screenshots";
    assets.CreateFolder(kShotsFolder);
    if (filtering || m_CurrentAssetFolder == kShotsFolder) {
        std::error_code ec;
        const std::string shotsDir = Screenshot::Dir();
        std::vector<std::string> seen;
        for (const auto& entry : std::filesystem::directory_iterator(shotsDir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") continue;
            if (entry.path().filename().string().rfind('.', 0) == 0) continue; // .shutter.wav etc.
            std::string path = entry.path().generic_string();
            std::string name = entry.path().stem().string();
            if (!MatchesAssetSearch(parsedSearch, name, "texture", {})) continue;
            seen.push_back(path);
            auto it = m_ShotThumbs.find(path);
            if (it == m_ShotThumbs.end())
                it = m_ShotThumbs.emplace(path, LoadScreenshotTexture(path)).first;
            cells.push_back({Cell::Kind::Screenshot, path, name, nullptr, it->second});
        }
        // Drop thumbnails for files that were deleted since last frame.
        for (auto it = m_ShotThumbs.begin(); it != m_ShotThumbs.end();)
            it = (std::find(seen.begin(), seen.end(), it->first) == seen.end())
                     ? m_ShotThumbs.erase(it) : std::next(it);
    }

    for (const auto& folder : assets.Folders()) {
        bool show = filtering ? MatchesAssetSearch(parsedSearch, LeafNameOf(folder), "folder", {})
            : (ParentFolderOf(folder) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Folder, folder, LeafNameOf(folder), nullptr, nullptr});
    }
    for (const auto& model : assets.Models()) {
        std::string path = model->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "model", assets.Labels(path))) continue;
        bool show = filtering || assets.AssetFolder(path) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Model, path, name, model, nullptr});
    }
    for (const auto& tex : assets.Textures()) {
        std::string path = tex->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "texture", assets.Labels(path))) continue;
        bool show = filtering || assets.AssetFolder(path) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Texture, path, name, nullptr, tex});
    }
    for (const auto& sound : assets.Sounds()) {
        std::string name = assets.DisplayName(sound);
        if (!MatchesAssetSearch(parsedSearch, name, "sound", assets.Labels(sound))) continue;
        bool show = filtering || assets.AssetFolder(sound) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Sound, sound, name, nullptr, nullptr});
    }
    for (const auto& prefab : assets.Prefabs()) {
        std::string name = assets.DisplayName(prefab);
        if (!MatchesAssetSearch(parsedSearch, name, "prefab", assets.Labels(prefab))) continue;
        bool show = filtering || assets.AssetFolder(prefab) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Prefab, prefab, name, nullptr, nullptr});
    }
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
        bool aFolder = a.kind == Cell::Kind::Folder, bFolder = b.kind == Cell::Kind::Folder;
        if (aFolder != bFolder) return aFolder;
        return a.display < b.display;
    });

    // Ctrl+A - select every currently-visible item (respecting the active search/filter, same
    // as Unity's own "select all visible items in list").
    if (m_AssetBrowserFocused && m_RenamingAssetKey.empty() && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_A) && !cells.empty()) {
        ClearAssetSelection();
        m_SelectedAssetKey = cells[0].key;
        m_SelectedAssetIsFolder = cells[0].kind == Cell::Kind::Folder;
        for (size_t i = 1; i < cells.size(); ++i) {
            m_ExtraAssetSelection.push_back({cells[i].key, cells[i].kind == Cell::Kind::Folder});
        }
    }

    // Below kListViewIconSize (DPI-scaled, matching the footer slider's minimum), the slider
    // switches to a compact list - Unity's "slide the icon size to the extreme left for list
    // view" behavior.
    bool gridMode = m_AssetIconSize > kListViewIconSize * m_UIScale;
    const float cellPadding = 8.0f;
    const float cellWidth = m_AssetIconSize + cellPadding * 2.0f;
    // One text line reserved under the thumbnail (#158) — long names truncate with "…" and show
    // the full string on hover; every grid row is a line shorter than the old two-line reserve.
    const float cellHeight = m_AssetIconSize + cellPadding + ImGui::GetTextLineHeightWithSpacing();

    // (#157) The ".." go-up row is gone — the folder tree and the Backspace shortcut cover
    // "go to parent"; it no longer costs a row at the top of every non-root folder.

    for (size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex) {
        const auto& cell = cells[cellIndex];
        ImGui::PushID(cell.key.c_str());

        bool isFolder = cell.kind == Cell::Kind::Folder;
        bool isSelected = IsAssetSelected(cell.key, isFolder);
        bool isRenaming = m_RenamingAssetKey == cell.key && m_RenamingIsFolder == isFolder;
        bool playing = cell.kind == Cell::Kind::Sound && AudioEngine::IsPreviewPlaying(cell.key);
        const char* icon = isFolder ? ICON_FA_FOLDER
            : cell.kind == Cell::Kind::Model ? (cell.model->HasAnimations() ? ICON_FA_FILM : ICON_FA_CUBE)
            : cell.kind == Cell::Kind::Scene ? ICON_FA_MAP
            : cell.kind == Cell::Kind::Prefab ? ICON_FA_BOX_ARCHIVE
            : cell.kind == Cell::Kind::Screenshot ? ICON_FA_IMAGE
            : (playing ? ICON_FA_STOP : ICON_FA_MUSIC);

        bool clicked = false;
        if (gridMode) {
            // The tile itself (Selectable for its background/hit-test, or a same-size Dummy
            // while renaming) stays "the last submitted item" for everything below (hover,
            // drag-drop, context menu) - icon and label are painted directly onto the draw
            // list afterward rather than as their own widgets, so they never steal that.
            ImVec2 tileMin = ImGui::GetCursorScreenPos();
            ImVec2 tileSize(cellWidth, cellHeight);
            if (!isRenaming) {
                clicked = ImGui::Selectable("##tile", isSelected, ImGuiSelectableFlags_None, tileSize);
            } else {
                ImGui::Dummy(tileSize);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            unsigned int modelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model) : 0u;
            if (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot) {
                float aspect = cell.texture->Height() > 0 ? (float)cell.texture->Width() / (float)cell.texture->Height() : 1.0f;
                ImVec2 imgSize = aspect >= 1.0f ? ImVec2(m_AssetIconSize, m_AssetIconSize / aspect) : ImVec2(m_AssetIconSize * aspect, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f + (m_AssetIconSize - imgSize.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)cell.texture->GLHandle(), imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
            } else if (modelThumb) {
                ImVec2 imgSize(m_AssetIconSize, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)modelThumb, imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
            } else {
                ImFont* font = ImGui::GetFont();
                ImVec2 glyphSize = font->CalcTextSizeA(m_AssetIconSize, FLT_MAX, 0.0f, icon);
                ImVec2 glyphPos(tileMin.x + (cellWidth - glyphSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f + (m_AssetIconSize - glyphSize.y) * 0.5f);
                dl->AddText(font, m_AssetIconSize, glyphPos, textColor, icon);
            }

            if (!isRenaming) {
                // Label box spans the whole cell (like the icon above it, which is centred in
                // cellWidth), with a small inset so a full-width line doesn't touch the edges —
                // DrawClampedGridLabel centres each line within this box.
                ImVec2 labelPos(tileMin.x + 3.0f, tileMin.y + m_AssetIconSize + cellPadding);
                bool truncated = DrawClampedGridLabel(dl, labelPos, cellWidth - 6.0f,
                    ImGui::GetTextLineHeightWithSpacing(), textColor, cell.display.c_str(), /*maxLines=*/1);
                if (truncated && ImGui::IsItemHovered()) {
                    EditorUI::SetTooltip(cell.display.c_str());
                }
            } else {
                ImGui::SetCursorScreenPos(ImVec2(tileMin.x + 2.0f, tileMin.y + m_AssetIconSize + cellPadding));
                ImGui::SetNextItemWidth(cellWidth - 4.0f);
                if (m_RenamingJustStarted) {
                    ImGui::SetKeyboardFocusHere();
                    m_RenamingJustStarted = false;
                }
                bool done = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) m_RenamingAssetKey.clear();
                ImGui::SetCursorScreenPos(ImVec2(tileMin.x, tileMin.y + tileSize.y));
            }
        } else {
            // List mode: little icon (real thumbnail for textures, a Font Awesome glyph
            // otherwise) followed by the name, mirroring how the Scene Hierarchy lists rows.
            float rowIconSize = ImGui::GetTextLineHeight();
            unsigned int rowModelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model) : 0u;
            if (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot) {
                ImGui::Image((ImTextureID)(intptr_t)cell.texture->GLHandle(), ImVec2(rowIconSize, rowIconSize));
            } else if (rowModelThumb) {
                ImGui::Image((ImTextureID)(intptr_t)rowModelThumb, ImVec2(rowIconSize, rowIconSize));
            } else {
                ImGui::TextUnformatted(icon);
            }
            ImGui::SameLine();

            if (isRenaming) {
                ImGui::SetNextItemWidth(-1);
                if (m_RenamingJustStarted) {
                    ImGui::SetKeyboardFocusHere();
                    m_RenamingJustStarted = false;
                }
                bool done = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) m_RenamingAssetKey.clear();
            } else {
                clicked = ImGui::Selectable(cell.display.c_str(), isSelected);
            }
        }

        if (clicked) {
            // One selection context at a time: clicking an asset drops the scene-entity
            // selection, so the Inspector shows this asset's Import Settings instead of staying
            // on whatever object was selected (it otherwise always wins, so an asset click
            // right after e.g. a Hierarchy rename appeared to do nothing).
            ClearSelection();

            ImGuiIO& assetIO = ImGui::GetIO();
            if (assetIO.KeyShift && !m_SelectedAssetKey.empty()) {
                // Range-select from the anchor to here, replacing the current selection —
                // Unity/Explorer-standard Shift-click behavior. The anchor index is only
                // meaningful against this same, currently-visible `cells` list.
                ClearAssetSelection();
                size_t lo = std::min(m_AssetSelectionAnchorIndex, cellIndex);
                size_t hi = std::min(std::max(m_AssetSelectionAnchorIndex, cellIndex), cells.size() - 1);
                for (size_t i = lo; i <= hi; ++i) {
                    bool f = cells[i].kind == Cell::Kind::Folder;
                    if (i == lo) { m_SelectedAssetKey = cells[i].key; m_SelectedAssetIsFolder = f; }
                    else m_ExtraAssetSelection.push_back({cells[i].key, f});
                }
                // Deliberately don't move the anchor, so repeated Shift-clicks keep extending
                // or shrinking the range from the same starting point.
            } else if (assetIO.KeyCtrl) {
                ToggleAssetSelection(cell.key, isFolder);
                m_AssetSelectionAnchorIndex = cellIndex;
            } else {
                ClearAssetSelection();
                m_SelectedAssetKey = cell.key;
                m_SelectedAssetIsFolder = isFolder;
                m_AssetSelectionAnchorIndex = cellIndex;
            }
            if (cell.kind == Cell::Kind::Sound && !assetIO.KeyShift && !assetIO.KeyCtrl) {
                if (playing) AudioEngine::StopPreview();
                else AudioEngine::PlayPreview(cell.key);
            }
        }
        if (isFolder && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_CurrentAssetFolder = cell.key;
        }
        if (cell.kind == Cell::Kind::Scene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            RequestOpenScene(world, assets, cell.key);
        }
        if (cell.kind == Cell::Kind::Screenshot && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenScreenshotPreview(cell.key); // centred in-editor lightbox (Show in folder is on the context menu)
        }
        if (cell.kind == Cell::Kind::Prefab && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            PushUndo(world, "Place Prefab Instance");
            entt::entity spawned = SceneSerializer::InstantiatePrefab(world, assets, cell.key);
            if (spawned != entt::null) {
                UniquifyName(world, spawned);
                SelectItem(spawned, false);
            }
        }

        if (isFolder) {
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SOUND_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                    std::string src((const char*)p->Data);
                    if (src != cell.key && cell.key.rfind(src + "/", 0) != 0) {
                        PushUndo(world, "Move Folder");
                        assets.RenameFolder(src, cell.key + "/" + LeafNameOf(src));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        } else if (cell.kind != Cell::Kind::Scene && cell.kind != Cell::Kind::Screenshot) { // not placeable — nothing to drag into the viewport
            const char* payloadType = cell.kind == Cell::Kind::Model ? "ASSET_MODEL_PATH"
                : cell.kind == Cell::Kind::Texture ? "ASSET_TEXTURE_PATH"
                : cell.kind == Cell::Kind::Prefab ? "ASSET_PREFAB_PATH" : "ASSET_SOUND_PATH";
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(payloadType, cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
        }

        if (!isRenaming && ImGui::IsItemHovered()) {
            if (cell.kind == Cell::Kind::Model) {
                EditorUI::SetTooltip("%s\n\nDrag into the viewport to place\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindModelUsages(world, cell.model.get())).c_str());
            } else if (cell.kind == Cell::Kind::Texture) {
                EditorUI::SetTooltip(
                    "%s\n\nDrag onto a model to set its Albedo map,\nor onto a map row in the Inspector's PBR Material.\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindTextureUsages(world, cell.texture.get())).c_str());
            } else if (cell.kind == Cell::Kind::Sound) {
                EditorUI::SetTooltip("%s\n\nClick to preview\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindSoundUsages(world, cell.key)).c_str());
            } else if (cell.kind == Cell::Kind::Scene) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Prefab) {
                EditorUI::SetTooltip("%s\n\nDrag into the viewport to place an instance,\nor double-click to place one at the origin.",
                    cell.display.c_str());
            } else {
                EditorUI::SetTooltip("%s\n\nDouble-click to open. Drag assets onto it to file them here.", cell.display.c_str());
            }
        }

        if (cell.kind == Cell::Kind::Scene) {
            // Scenes aren't AssetLibrary entries (no rename / remove-from-library — they're real
            // .json files on disk), so they get their own short context menu: Open + Delete.
            if (ImGui::BeginPopupContextItem()) {
                if (!IsAssetSelected(cell.key, false)) {
                    ClearAssetSelection();
                    m_SelectedAssetKey = cell.key;
                    m_SelectedAssetIsFolder = false;
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open")) RequestOpenScene(world, assets, cell.key);

                // Act on the whole selection when the right-clicked scene is part of a
                // multi-selection, same as the generic asset menu.
                std::vector<AssetKeyRef> scenesForAction;
                scenesForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) scenesForAction.push_back(e);
                const bool multi = scenesForAction.size() > 1;
                const bool anyOpen = std::any_of(scenesForAction.begin(), scenesForAction.end(),
                    [&](const AssetKeyRef& r) { return r.Key == m_CurrentScenePath; });
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete",
                                    nullptr, false, !(!multi && anyOpen)))
                    RequestDeleteAssets(world, assets, scenesForAction, /*skipDialog=*/false);
                if (!multi && anyOpen && ImGui::IsItemHovered())
                    EditorUI::SetTooltip("This scene is open — open a different scene first.");
                ImGui::EndPopup();
            }
        } else if (cell.kind == Cell::Kind::Screenshot) {
            // Also a real file on disk, not a library asset: Show in folder + Delete.
            if (ImGui::BeginPopupContextItem()) {
                if (!IsAssetSelected(cell.key, false)) {
                    ClearAssetSelection();
                    m_SelectedAssetKey = cell.key;
                    m_SelectedAssetIsFolder = false;
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in folder")) Screenshot::ShowInFolder(cell.key);
                std::vector<AssetKeyRef> shotsForAction;
                shotsForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) shotsForAction.push_back(e);
                const bool multi = shotsForAction.size() > 1;
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete"))
                    RequestDeleteAssets(world, assets, shotsForAction, /*skipDialog=*/false);
                ImGui::EndPopup();
            }
        } else if (ImGui::BeginPopupContextItem()) {
            // Right-clicking an item already part of the selection keeps the whole selection
            // (so Delete/Remove from Library below can act on all of it, Explorer-style);
            // right-clicking an unselected item replaces the selection with just this one.
            if (!IsAssetSelected(cell.key, isFolder)) {
                ClearAssetSelection();
                m_SelectedAssetKey = cell.key;
                m_SelectedAssetIsFolder = isFolder;
            }
            if (ImGui::MenuItem(ICON_FA_PEN "  Rename (F2)", nullptr, false, m_ExtraAssetSelection.empty())) {
                BeginRenameAsset(cell.key, isFolder, cell.display);
            }
            if (ImGui::MenuItem(ICON_FA_TAG "  Edit Labels...")) {
                std::string joined;
                for (const auto& lbl : assets.Labels(cell.key)) {
                    if (!joined.empty()) joined += ", ";
                    joined += lbl;
                }
                snprintf(m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer), "%s", joined.c_str());
                ImGui::OpenPopup("##EditLabels");
            }
            if (ImGui::BeginPopup("##EditLabels")) {
                ImGui::TextDisabled("Comma-separated labels - searchable as l:label");
                ImGui::SetNextItemWidth(240.0f);
                bool enter = ImGui::InputText("##LabelsBuf", m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                bool apply = enter || ImGui::Button("Apply");
                if (apply) {
                    std::set<std::string> labels;
                    std::stringstream ss(m_LabelsEditBuffer);
                    std::string part;
                    while (std::getline(ss, part, ',')) {
                        size_t b = part.find_first_not_of(" \t");
                        size_t e = part.find_last_not_of(" \t");
                        if (b != std::string::npos) labels.insert(part.substr(b, e - b + 1));
                    }
                    PushUndo(world, "Edit Labels");
                    assets.SetLabels(cell.key, labels);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            // Reimport straight from the context menu instead of only via Import Settings >
            // Apply (#28 P17). Single selection, real imported assets only.
            if (!isFolder && m_ExtraAssetSelection.empty() &&
                (cell.kind == Cell::Kind::Model || cell.kind == Cell::Kind::Texture)) {
                if (ImGui::MenuItem(ICON_FA_ROTATE "  Reimport")) {
                    if (cell.kind == Cell::Kind::Model) {
                        PushUndo(world, "Reimport Model");
                        if (assets.ReimportModel(cell.key)) Log::Info("Reimported model '" + cell.key + "'.");
                        else Log::Error("Reimport failed for '" + cell.key + "' - see Console.");
                        InvalidateModelThumbnail(nullptr);
                    } else {
                        PushUndo(world, "Reimport Texture");
                        if (assets.ReimportTexture(cell.key)) Log::Info("Reimported texture '" + cell.key + "'.");
                        else Log::Error("Reimport failed for '" + cell.key + "' - see Console.");
                    }
                }
            }

            // Everything currently selected, whenever the right-clicked item is part of a
            // multi-selection - so Delete/Remove from Library act on the whole group rather
            // than just the one cell that happened to receive the right-click.
            std::vector<AssetKeyRef> selectionForAction;
            selectionForAction.push_back({m_SelectedAssetKey, m_SelectedAssetIsFolder});
            for (const auto& e : m_ExtraAssetSelection) selectionForAction.push_back(e);
            const char* deleteLabel = selectionForAction.size() > 1
                ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete Folder";

            if (isFolder) {
                if (ImGui::MenuItem(deleteLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
            } else {
                if (cell.kind == Cell::Kind::Prefab && selectionForAction.size() == 1 && ImGui::MenuItem(ICON_FA_PLUS "  Place Instance")) {
                    PushUndo(world, "Place Prefab Instance");
                    entt::entity spawned = SceneSerializer::InstantiatePrefab(world, assets, cell.key);
                    if (spawned != entt::null) {
                        UniquifyName(world, spawned);
                        SelectItem(spawned, false);
                    }
                }
                const char* removeLabel = selectionForAction.size() > 1
                    ? ICON_FA_TRASH "  Remove Selected from Library" : ICON_FA_TRASH "  Remove from Library";
                if (ImGui::MenuItem(removeLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
                if (cell.kind == Cell::Kind::Prefab) ImGui::TextDisabled("The .prefab file stays on disk.");
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();

        // Wrap to the next row only when there's genuinely room for another tile - the
        // standard ImGui "wrapping button grid" idiom (imgui_demo.cpp's Layout section).
        if (gridMode && cellIndex + 1 < cells.size()) {
            float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            float nextTileX2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + cellWidth;
            if (nextTileX2 < windowVisibleX2) ImGui::SameLine();
        }
    }

    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ClearAssetSelection();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##BrowserBgContext");
    }
    if (ImGui::BeginPopup("##BrowserBgContext")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) makeNewFolder();
        ImGui::EndPopup();
    }

    ImGui::EndChild(); // ##AssetList

    // Footer: the selected item's name/path on the left (Unity shows the full path here only
    // while searching; a plain display name the rest of the time is enough for this browser's
    // scale), and the icon-size slider on the right - dragging it to the minimum switches the
    // grid above to the compact list view instead of tiles.
    ImGui::BeginChild("##AssetGridFooter", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    std::string footerLabel;
    if (!m_ExtraAssetSelection.empty()) {
        footerLabel = std::to_string(m_ExtraAssetSelection.size() + 1) + " items selected";
    } else if (!m_SelectedAssetKey.empty()) {
        footerLabel = m_SelectedAssetIsFolder ? m_SelectedAssetKey : assets.DisplayName(m_SelectedAssetKey);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", footerLabel.c_str());

    const float sliderWidth = 100.0f;
    float sliderX = ImGui::GetWindowContentRegionMax().x - sliderWidth;
    if (sliderX > ImGui::GetCursorPosX()) ImGui::SameLine(sliderX);
    else ImGui::NewLine();
    ImGui::SetNextItemWidth(sliderWidth);
    EditorUI::SliderFloat("##IconSize", &m_AssetIconSize,
        kListViewIconSize * m_UIScale, 128.0f * m_UIScale, "");
    if (ImGui::IsItemHovered()) {
        EditorUI::SetTooltip("Icon size - drag all the way to the left for a compact list view.");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { // slider released — remember it
        EditorSettings::Get().AssetBrowserIconSize = m_AssetIconSize;
        EditorSettings::Save();
    }
    ImGui::EndChild();

    DrawDeleteConfirmPopup(world, assets);

    ImGui::End();
}
