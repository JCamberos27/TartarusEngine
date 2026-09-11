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


// --- Docked-panel tab-bar chrome text -------------------------------------------------------
// A dock node's tab bar — the tab labels, each tab's close ×, the ▼ window-list button, the
// node close × — is all drawn in ImGuiCol_Text, with no separate style colour (ImGui's own
// source says as much). On a light-chrome theme (Windows XP) that chrome wants to stay white
// against the coloured tabs / caption strip. The whole tab bar renders synchronously inside a
// docked window's ImGui::Begin(), so each panel wraps *just its Begin() call* in these:
//
//     PushTabChromeText();
//     bool open = ImGui::Begin("Panel", ...);
//     PopTabChromeText();
//     if (!open) { ImGui::End(); return; }
//     ... body draws in the theme's normal text colour, no wrapping needed ...
//
// "Light chrome" is detected from ImGuiCol_WindowBg luminance, so any future light theme works
// and the dark themes are untouched (no-op).
inline bool PanelChromeIsLight() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return (0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z) > 0.5f;
}
inline void PushTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.00f, 1.0f));
}
inline void PopTabChromeText() {
    if (PanelChromeIsLight()) ImGui::PopStyleColor();
}


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


// Trims a byte-length-truncated UTF-8 string back to the last complete codepoint boundary.
// A naive "pop trailing continuation bytes (10xxxxxx)" loop (the previous approach here) isn't
// enough: once a hard resize(N) lands mid-sequence, popping every trailing continuation byte can
// still leave a bare, dangling lead byte at the very end (a byte matching 110xxxxx / 1110xxxx /
// 11110xxx) with zero continuation bytes following it — itself an invalid, truncated sequence.
// Concretely: 97 ASCII bytes + one 4-byte emoji (F0 9F 98 80) = 101 bytes. resize(100) leaves
// "...F0 9F 98". Popping continuation bytes 0x98 then 0x9F stops at 0xF0 (not a continuation
// byte) and quits, leaving the lead byte 0xF0 dangling with nothing after it.
//
// This walks back from the end, finds the run of trailing continuation bytes and the lead byte
// (if any) just before it, and compares how many continuation bytes that lead byte's own pattern
// declares against how many are actually still present. Only a genuinely truncated sequence
// (fewer present than declared) gets dropped — a sequence that happens to end exactly at the
// truncation boundary (declared count == present count) is left untouched.
inline void TrimDanglingUtf8Lead(std::string& out) {
    if (out.empty()) return;
    // Walk back over the run of trailing continuation bytes (10xxxxxx), at most 3 of them (the
    // longest UTF-8 sequence is 4 bytes: 1 lead + 3 continuations).
    size_t contRun = 0;
    while (contRun < 3 && contRun < out.size() &&
           (static_cast<unsigned char>(out[out.size() - 1 - contRun]) & 0xC0) == 0x80)
        ++contRun;
    if (contRun == out.size()) return; // all continuation bytes, no lead byte in the string at all
    const size_t leadPos = out.size() - 1 - contRun;
    const unsigned char lead = static_cast<unsigned char>(out[leadPos]);
    size_t expected;
    if ((lead & 0x80) == 0x00)      expected = 0; // ASCII — no continuation bytes should follow
    else if ((lead & 0xE0) == 0xC0) expected = 1; // 2-byte sequence
    else if ((lead & 0xF0) == 0xE0) expected = 2; // 3-byte sequence
    else if ((lead & 0xF8) == 0xF0) expected = 3; // 4-byte sequence
    else { out.resize(leadPos); return; }         // stray continuation byte acting as a "lead" — drop it too

    if (contRun < expected)
        out.resize(leadPos); // truncated mid-sequence: drop the dangling lead byte and its partial tail
    // contRun >= expected: a complete sequence (or, for ASCII, no continuation bytes) — leave as-is.
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
        TrimDanglingUtf8Lead(out); // don't leave a half UTF-8 sequence
    }
    return out;
}

// Asset Browser rename (folders and the per-asset display name, [#38 B12]) shares the same
// class of problem as entity names, plus one more: these names get '/'-joined into virtual
// folder paths (see ParentFolderOf/LeafNameOf above), so a stray '/' or '\' silently rewrites
// the hierarchy instead of just looking odd in a label. Strip C0 controls + DEL, strip the
// characters Windows forbids in a real filename (a folder rename doesn't touch disk today, but
// SetDisplayName round-trips through asset .meta JSON and both are one accidental future commit
// away from reaching the filesystem — never let one through here), trim the ends, and cap the
// length. Returns empty when nothing usable is left, which the caller treats as "reject".
inline std::string SanitizeAssetName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    static const std::string kIllegal = "<>:\"/\\|?*";
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) continue; // control chars
        if (kIllegal.find(static_cast<char>(c)) != std::string::npos) continue;
        out.push_back(static_cast<char>(c));
    }

    size_t b = out.find_first_not_of(" \t.");
    size_t e = out.find_last_not_of(" \t.");
    out = (b == std::string::npos) ? std::string() : out.substr(b, e - b + 1);

    constexpr size_t kMaxAssetNameLen = 100;
    if (out.size() > kMaxAssetNameLen) {
        out.resize(kMaxAssetNameLen);
        TrimDanglingUtf8Lead(out); // don't leave a half UTF-8 sequence
    }

    // Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9) are illegal as a
    // filename regardless of extension — reject them outright rather than silently mangling.
    // Windows keys this off the *stem* (everything before the first '.'), so "CON.txt" is just
    // as reserved as bare "CON" — compare against the stem, not the literal full string, or a
    // trailing extension slips a reserved name straight through.
    static const char* kReserved[] = { "CON", "PRN", "AUX", "NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9" };
    const size_t dot = out.find('.');
    std::string stem = (dot == std::string::npos) ? out : out.substr(0, dot);
    for (char& c : stem) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    for (const char* r : kReserved) if (stem == r) return std::string();

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
    // "On" toggles read in the same cyan the styled sliders use (ImGuiCol_SliderGrab).
    const ImVec4 acc = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(acc.x, acc.y, acc.z, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(acc.x, acc.y, acc.z, 0.34f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(acc.x, acc.y, acc.z, 0.46f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(acc.x, acc.y, acc.z, 1.0f));
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
                                                  ImGui::ColorConvertFloat4ToU32(acc), 1.0f);
    }
    ImGui::PopStyleColor(active ? 4 : 3);
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
//   inactive          -> a dim EYE_SLASH (the state worth surfacing)
//   active            -> a faint EYE, always visible, brightening on row- or self-hover
// `rowHovered` is the caller's hit-test of the whole row/header line (the glyph is drawn before
// the rest of the row, so it can't rely on its own hover alone). Returns true on click.
// `alignTop`: place the glyph with its line-box at the row top (matches the Hierarchy's kind
// glyph). False = frame-centred, matching a framed widget on the same line (Inspector header).
inline bool ActiveToggle(const char* id, bool active, bool rowHovered, const char* tip, bool alignTop) {
    const float sz = ImGui::GetFrameHeight();
    // Width hugs the glyph (+2px) instead of a full sz-square, so the slot doesn't push the
    // rest of the row across the way the old sz-wide Button did (#152 follow-up).
    const float w = ImMax(ImGui::CalcTextSize(ICON_FA_SQUARE_CHECK).x, ImGui::CalcTextSize(ICON_FA_SQUARE).x) + 2.0f;
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
    // "Active" reads as a checkbox now, not an eye — Unity's own convention and it frees the eye
    // metaphor for the SceneVis visibility toggle next to it (#236 B feedback).
    if (!active) {
        glyph(ICON_FA_SQUARE, ImGui::GetColorU32(ImGuiCol_TextDisabled, selfHover ? 1.0f : 0.80f));
    } else {
        glyph(ICON_FA_SQUARE_CHECK, ImGui::GetColorU32(ImGuiCol_Text, selfHover ? 1.0f : (rowHovered ? 0.85f : 0.65f)));
    }
    return clicked;
}

// #236 B — a hover-reveal glyph toggle for the Hierarchy's SceneVis columns (hide / lock).
// Nothing is drawn at rest unless `on`; hovering the row fades the glyph in so the column
// doesn't clutter every row. Returns true on click. Same top-aligned glyph metrics as
// ActiveToggle so the little cluster lines up with the eye.
inline bool SceneVisToggle(const char* id, const char* glyphOn, const char* glyphOff,
                           bool on, bool rowHovered, const char* tip) {
    const float sz = ImGui::GetFrameHeight();
    const float w  = ImMax(ImGui::CalcTextSize(glyphOn).x, ImGui::CalcTextSize(glyphOff).x) + 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##svis", ImVec2(w, sz));
    const bool selfHover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (selfHover) EditorUI::SetTooltip("%s", tip);

    // Always drawn: a quiet glyph at rest so the column is discoverable, brighter on row-hover,
    // brightest when set or directly hovered.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* g = on ? glyphOn : glyphOff;
    const ImVec2 ts = ImGui::CalcTextSize(g);
    const float a = on ? (selfHover ? 1.0f : 0.90f)
                       : (selfHover ? 0.85f : (rowHovered ? 0.60f : 0.32f));
    dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y),
                ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled, a), g);
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
