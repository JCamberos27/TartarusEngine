#pragma once
// Internal helpers shared by more than one EditorLayer translation unit (#179).
//
// EditorLayer.cpp was split into per-panel .cpp files (Hierarchy / Inspector / AssetBrowser /
// Gizmos / Scene / Toolbar) that all define members of the same EditorLayer class. Most of the
// file-local helpers moved to whichever panel uses them; the handful below have callers in two
// or more of those files, so they live here instead of being duplicated. Nothing in here is
// part of EditorLayer's public interface - it is an implementation detail of those .cpp files.

#include "World.h"
#include "Texture.h"
#include "EditorUIHelpers.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax, used by ActiveToggle's glyph metrics
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace EditorInternal {

// Height of the top toolbar: the dropdown menu bar row + the one-click icon row, trimmed so the
// icons sit snug against the bottom edge instead of floating in a tall strip of dead space.
inline constexpr float kToolbarHeight = 52.0f;


// True only for entities the vertex-grab workflow applies to. Level-geometry boxes qualify now
// too — their cube primitive has real vertices to grab, same as any placed cube.
inline bool IsVertexDraggable(World& world, entt::entity entity) {
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


// Case-insensitive substring match — shared by the Console filter, Hierarchy filter, and (as
// one piece of the richer parser below) the Asset Browser search.
inline bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return toLower(text).find(toLower(filter)) != std::string::npos;
}


// Virtual folder paths are '/'-joined segments (e.g. "Props/Guns") — these split off the
// last segment, used throughout the Asset Browser for breadcrumbs, rename, and re-parenting.
inline std::string ParentFolderOf(const std::string& folderPath) {
    size_t slash = folderPath.find_last_of('/');
    return slash == std::string::npos ? std::string() : folderPath.substr(0, slash);
}

inline std::string LeafNameOf(const std::string& folderPath) {
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
inline bool ActionButton(const char* icon, const char* tooltip, bool active = false, ImVec2 size = ImVec2(0, 0)) {
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


// The one filled treatment — theme accent body, for prominent/rare actions only.
inline bool PrimaryButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_Header));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
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
inline bool ActiveToggle(const char* id, bool active, bool rowHovered, const char* tip, bool alignTop) {
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


// Unity-style duplicate naming: "Cube" -> "Cube (1)" -> "Cube (2)". Re-derives the base name
// from an already-numbered source first, so duplicating a duplicate produces "Cube (2)" instead
// of chaining into "Cube (1) (1)". Takes and updates a name set built once by the caller instead
// of rescanning the whole scene on every call — duplicating N objects used to be O(n^2).
inline std::string NextDuplicateName(std::set<std::string>& existingNames, const std::string& sourceName) {
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

    int n = 1;
    std::string candidate;
    do {
        candidate = base + " (" + std::to_string(n) + ")";
        n++;
    } while (existingNames.count(candidate));
    existingNames.insert(candidate);
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
inline std::string UniqueNameFor(const World& world, const std::string& desired) {
    bool collision = false;
    for (auto e : world.Registry.view<NameComponent>()) {
        if (world.Registry.get<NameComponent>(e).Name == desired) { collision = true; break; }
    }
    if (!collision) return desired;

    std::set<std::string> existingNames;
    for (auto e : world.Registry.view<NameComponent>())
        existingNames.insert(world.Registry.get<NameComponent>(e).Name);
    return NextDuplicateName(existingNames, desired);
}

// Same idea as UniqueNameFor, but for an entity that already carries the name to check (e.g. a
// freshly-instantiated prefab, authored with whatever name it was saved under) rather than a
// name string not yet assigned to anyone. Clears the entity's own name first so it doesn't
// collide with itself in the scan - otherwise the first instance of every prefab would get
// needlessly renamed "X (1)" on its very first placement.
inline void UniquifyName(World& world, entt::entity e) {
    auto* nc = world.Registry.try_get<NameComponent>(e);
    if (!nc) return;
    std::string desired = nc->Name;
    nc->Name.clear();
    nc->Name = UniqueNameFor(world, desired);
}

// A screenshot on disk already holds display-encoded (sRGB) bytes. Load it as literal color
// (IsSRGB = false) so the Asset Browser thumbnail and the lightbox render it at true brightness
// — the default sRGB path hardware-linearizes on sample, which is right for a 3D albedo map but
// visibly darkens a UI image.
//
// maxSize controls the two very different jobs this shares: the lightbox (DrawScreenshotPreview)
// wants the full-quality capture since the user can scroll-to-zoom into it, so it passes 0 (no
// cap, see TextureImportSettings::MaxTextureSize). The Asset Browser grid thumbnail renders at
// ~96px, so it caps small to cut decode/VRAM cost (#176) instead of loading (and nearest-
// downsampling, #207) the full-res capture just to shrink it back down for display.
inline std::shared_ptr<Texture> LoadScreenshotTexture(const std::string& path, int maxSize) {
    TextureImportSettings s;
    s.IsSRGB = false;
    s.WrapMode = TextureImportSettings::Wrap::ClampToEdge;
    s.MaxTextureSize = maxSize;
    return std::make_shared<Texture>(path, s);
}

} // namespace EditorInternal
