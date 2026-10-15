// The Glass theme's editor surfaces (opt-in: Preferences > General > Editor theme > Glass).
//
//   Objects      a scene tree with full-width rounded selection panels, an accent bar, a selection
//                panel that glides from row to row and a velocity-gated hover glow
//   Properties   accordion component cards whose bodies animate their real height (clip-guarded),
//                a flat red / green / blue X Y Z transform table and a staggered card entrance
//   Library      a responsive asset grid with hover lift and a diagonal wave reveal on navigation
//   Viewport     floating frosted-glass HUDs over the Scene view: the selection card, scene stats,
//                compile-error toasts (an under-damped pop and a shake) and a snap ring
//
// Everything here runs host-side only and only while the Glass theme is selected; with the default
// Tartarus theme none of it is drawn and m_Glass is never created. The spec (tokens, motion table,
// the frosted pipeline, the clipping guard and the input responsiveness policy) is
// docs/EDITOR_DESIGN_SYSTEM.md; the building blocks are in src/Editor/Glass/.

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPanels.h"
#include "EditorSettings.h"
#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "World.h"
#include "Components.h"
#include "ComponentRegistry.h"
#include "ComponentReflection.h"
#include "AssetLibrary.h"
#include "Model.h"
#include "Texture.h"
#include "AudioEngine.h"
#include "ProjectPaths.h"
#include "Log.h"

#include "Glass/GlassEditorState.h"
#include "Glass/GlassLayout.h"
#include "Glass/GlassMotion.h"
#include "Glass/GlassTheme.h"
#include "Glass/GlassTokens.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace EditorInternal;

namespace {

// --- Small drawing helpers --------------------------------------------------------------------------

// A token colour as ImU32, faded by `alpha` and by the current style alpha (so a card that is
// fading in fades everything it draws by hand, not just its widgets).
ImU32 Col(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImRect Scaled(const ImRect& r, float scale, ImVec2 offset) {
    const ImVec2 c = r.GetCenter();
    const ImVec2 h = r.GetSize() * (0.5f * scale);
    return ImRect(c - h + offset, c + h + offset);
}

// A ">" chevron centred on `c`, turned clockwise by turn01 * 90 degrees (so it points down when
// its section is open). Drawn, not a font glyph, so the turn is continuous.
void DrawChevron(ImDrawList* dl, ImVec2 c, float size, float turn01, ImU32 col) {
    const float a = turn01 * IM_PI * 0.5f;
    const float cs = std::cos(a), sn = std::sin(a);
    const float h = size * 0.5f;
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    ImVec2 pts[3] = {rot(-h * 0.35f, -h * 0.75f), rot(h * 0.40f, 0.0f), rot(-h * 0.35f, h * 0.75f)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, std::max(1.0f, Glass::Px(1.6f)));
}

// Text centred vertically in [y0, y0 + h], clipped with an ellipsis at maxX.
void DrawLabel(ImDrawList* dl, float x, float y0, float h, float maxX, ImU32 col, const char* text) {
    const float th = ImGui::GetTextLineHeight();
    const ImVec2 pos(x, y0 + (h - th) * 0.5f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    if (pos.x + size.x <= maxX) {
        dl->AddText(pos, col, text);
        return;
    }
    // ImGui's own ellipsis: draw through the window draw list with a temporary text colour.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(maxX, pos.y + th), maxX, text, nullptr, &size);
    ImGui::PopStyleColor();
}

bool ContainsNoCase(const std::string& hay, const char* needle) {
    const size_t n = std::strlen(needle);
    if (n == 0) return true;
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k])) ++k;
        if (k == n) return true;
    }
    return false;
}

const char* EntityGlyph(const entt::registry& r, entt::entity e, bool hasChildren) {
    if (r.all_of<LightComponent>(e)) return ICON_FA_LIGHTBULB;
    if (r.all_of<CameraComponent>(e)) return ICON_FA_VIDEO;
    if (r.all_of<RenderableComponent>(e)) return ICON_FA_CUBE;
    if (hasChildren) return ICON_FA_LAYER_GROUP;
    return ICON_FA_VECTOR_SQUARE;
}

const char* AssetGlyph(const AssetGridCell& cell) {
    using K = AssetGridCell::Kind;
    switch (cell.kind) {
        case K::Folder:      return ICON_FA_FOLDER;
        case K::Model:       return cell.model && cell.model->HasAnimations() ? ICON_FA_FILM : ICON_FA_CUBE;
        case K::Texture:     return ICON_FA_IMAGE;
        case K::Material:    return ICON_FA_DROPLET;
        case K::Sound:       return ICON_FA_MUSIC;
        case K::Scene:       return ICON_FA_MAP;
        case K::Prefab:      return ICON_FA_BOX_ARCHIVE;
        case K::Screenshot:  return ICON_FA_IMAGE;
        case K::Shader:      return ICON_FA_FILE_CODE;
        case K::Animator:    return ICON_FA_DIAGRAM_PROJECT;
        case K::Weapon:      return ICON_FA_CROSSHAIRS;
        case K::Hdri:        return ICON_FA_SUN;
        case K::Script:      return ICON_FA_SCROLL;
        case K::Recoil:      return ICON_FA_BURST;
        case K::CameraShake: return ICON_FA_VIDEO;
    }
    return ICON_FA_FILE;
}

// The drag payload type an asset tile offers (matches the Asset Browser's, so every existing drop
// target accepts tiles dragged from the Library). nullptr = not draggable.
const char* AssetPayloadType(AssetGridCell::Kind k) {
    using K = AssetGridCell::Kind;
    switch (k) {
        case K::Folder:   return "ASSET_FOLDER_PATH";
        case K::Model:    return "ASSET_MODEL_PATH";
        case K::Texture:
        case K::Hdri:     return "ASSET_TEXTURE_PATH";
        case K::Material: return "ASSET_MATERIAL_PATH";
        case K::Prefab:   return "ASSET_PREFAB_PATH";
        case K::Sound:    return "ASSET_SOUND_PATH";
        case K::Scene:
        case K::Screenshot:
        case K::Shader:   return nullptr; // not placeable
        default:          return "ASSET_FILE_PATH";
    }
}

// The structural-reveal cascade for whole cards (the Properties panel on a new selection): card
// `index` fades in and settles from RevealRowShiftPx below, RevealStaggerMs behind the one above.
// Constructed inside the card's table row (after BeginCard). The shift moves the layout cursor and
// stays in the layout, so the cards below are pushed down with it and settle together, and every
// hit rect is where its widget is drawn. The fade covers the card surface too (EndCard draws while
// this is alive).
class CardEntrance {
public:
    CardEntrance(int index, float elapsedSeconds) {
        if (elapsedSeconds == FLT_MAX) return;
        const float delay = Glass::StaggerDelay(index, Glass::RevealStaggerMs, Glass::RevealStaggerCap);
        const float p = Glass::EaseProgress(Glass::HoverIn, (elapsedSeconds - delay) / (Glass::RevealRowFadeMs * 0.001f));
        if (p >= 1.0f) return;
        m_Shift = Glass::Px(Glass::RevealRowShiftPx) * (1.0f - p);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m_Shift);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * p);
        m_Pushed = true;
        Glass::MarkLayoutInMotion();
    }
    ~CardEntrance() { if (m_Pushed) ImGui::PopStyleVar(); }
    CardEntrance(const CardEntrance&) = delete;
    CardEntrance& operator=(const CardEntrance&) = delete;
    float Shift() const { return m_Shift; }
private:
    float m_Shift = 0.0f;
    bool  m_Pushed = false;
};

// A square icon button in the Glass style (no ImGui::Button: a MotionButton with a hover glow).
bool GlassIconButton(const char* strId, const char* glyph, const char* tooltip, bool enabled = true) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImRect r(p, p + ImVec2(h, h));
    const ImGuiID id = ImGui::GetID(strId);
    if (!enabled) ImGui::BeginDisabled();
    bool hovered = false, held = false;
    const bool pressed = Glass::MotionButton(id, r, r, &hovered, &held);
    const float glow = Glass::Motion().HoverGlow(id, hovered && enabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float wash = held ? Glass::WashPressed.w : Glass::WashHover.w * glow;
    if (wash > 0.0f)
        Glass::DrawSquircle(dl, r.Min, r.Max, Col(Glass::WithAlpha(Glass::WashHover, wash)), Glass::Px(Glass::RadiusControl));
    const ImVec2 gs = ImGui::CalcTextSize(glyph);
    dl->AddText(r.GetCenter() - gs * 0.5f, Col(Glass::MixOklab(Glass::TextSecondary, Glass::TextPrimary, glow)), glyph);
    if (!enabled) ImGui::EndDisabled();
    if (enabled && tooltip && hovered) EditorUI::SetTooltip("%s", tooltip);
    return pressed && enabled;
}

// A card: one row of a single-column table, its rounded surface painted in the table's background
// channel under the content once the content's height is known. Pushes its own cell padding.
struct CardTable {
    bool open = false;
    bool Begin(const char* id) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(Glass::Px(Glass::Space1h), 0.0f));
        open = ImGui::BeginTable(id, 1, ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        return open;
    }
    void End() { if (open) ImGui::EndTable(); open = false; }
};

// A Glass Workspace panel's placement, called before its Begin every frame. On its first open
// it becomes a selected tab in the dock node of its classic counterpart (the Objects panel next
// to the Hierarchy, and so on), so it takes that place in the layout instead of floating over
// it. The counterpart may not be docked yet on the first frames (the default layout is built
// during the frame), so this keeps trying for a while, floating at `fallbackPos` meanwhile.
// `placed` latches once it has docked, given up, or imgui.ini already had a placement for it
// (the user's own placement wins). A floating Glass panel draws a near-opaque background: over
// other windows a translucent one lets their text show through.
void PlaceGlassPanel(const char* glassName, const char* classicName, ImVec2 fallbackPos, ImVec2 size,
                     GlassEditorState::Placement& placed) {
    ImGui::SetNextWindowPos(fallbackPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    const ImGuiWindow* self = ImGui::FindWindowByName(glassName);
    if (!placed.Done) {
        if (!self && ImGui::FindWindowSettingsByID(ImHashStr(glassName))) {
            placed.Done = true; // known from imgui.ini: leave it where the user put it
        } else {
            const ImGuiWindow* classic = ImGui::FindWindowByName(classicName);
            if (classic && classic->DockId != 0) {
                ImGui::SetNextWindowDockID(classic->DockId, ImGuiCond_Always);
                ImGui::SetNextWindowFocus();
                placed.Done = true;
            } else if (++placed.Frames > 120) {
                placed.Done = true; // no docked counterpart (closed, or a custom layout): stay floating
            }
        }
    }
}

// Starts a card row: top padding. Returns the row's top (screen y).
float BeginCard() {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float top = ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(0.0f, Glass::Px(Glass::Space1) - ImGui::GetStyle().ItemSpacing.y));
    return top;
}

// Ends a card row: bottom padding, the gap to the next card, and the surface underneath.
void EndCard(float top) {
    const float gap = Glass::Px(Glass::Space1);
    ImGui::Dummy(ImVec2(0.0f, Glass::Px(Glass::Space1) - ImGui::GetStyle().ItemSpacing.y));
    const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));
    ImGuiTable* t = ImGui::GetCurrentTable();
    if (!t) return;
    ImGui::TablePushBackgroundChannel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mn(t->OuterRect.Min.x, top), mx(t->OuterRect.Max.x, bottom);
    Glass::DrawSquircle(dl, mn, mx, Col(Glass::Card), Glass::Px(Glass::RadiusCard));
    Glass::DrawSquircleBorder(dl, mn, mx, Col(Glass::Hairline), Glass::Px(Glass::RadiusCard), 1.0f);
    ImGui::TablePopBackgroundChannel();
}

// A full-width card header row: chevron (turning with the section), icon, title. Returns true on
// click. `turn` is the chevron's 0..1 openness.
bool SectionHeader(const char* strId, const char* icon, const char* title, float turn, bool* outHovered) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImRect r(p, p + ImVec2(w, h));
    const ImGuiID id = ImGui::GetID(strId);
    bool hovered = false, held = false;
    const bool pressed = Glass::MotionButton(id, r, r, &hovered, &held);
    const float glow = Glass::Motion().HoverGlow(id, hovered);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (glow > 0.0f || held) {
        const ImRect wash(r.Min - ImVec2(Glass::Px(Glass::SpaceHalf), 0.0f), r.Max + ImVec2(Glass::Px(Glass::SpaceHalf), 0.0f));
        Glass::DrawSquircle(dl, wash.Min, wash.Max,
                            Col(Glass::WithAlpha(Glass::WashHover, held ? Glass::WashPressed.w : Glass::WashHover.w * glow)),
                            Glass::Px(Glass::RadiusRow));
    }
    const float chevron = Glass::Px(10.0f);
    DrawChevron(dl, ImVec2(r.Min.x + chevron * 0.6f, r.GetCenter().y), chevron, turn,
                Col(Glass::MixOklab(Glass::TextTertiary, Glass::TextSecondary, glow)));
    float x = r.Min.x + chevron * 1.2f + Glass::Px(Glass::Space1);
    if (icon && *icon) {
        DrawLabel(dl, x, r.Min.y, h, r.Max.x, Col(Glass::TextSecondary), icon);
        x += ImGui::GetFontSize() * 1.25f + Glass::Px(Glass::SpaceHalf);
    }
    DrawLabel(dl, x, r.Min.y, h, r.Max.x, Col(Glass::TextPrimary), title);
    if (outHovered) *outHovered = hovered;
    return pressed;
}

constexpr ImGuiID kSelectionGlideSalt = 0x53454C47u; // "SELG"
constexpr ImGuiID kSnapPulseId = 0x534E4150u;        // "SNAP": the snap ring's timer

} // namespace

// --- Theme switching -------------------------------------------------------------------------------

bool EditorLayer::GlassThemeActive() const { return EditorSettings::Get().EditorThemeId == 1; }

GlassEditorState& EditorLayer::GlassState() {
    if (!m_Glass) m_Glass = std::make_unique<GlassEditorState>();
    return *m_Glass;
}

void EditorLayer::ApplyGizmoAccent() {
    ImGuizmo::GetStyle().Colors[ImGuizmo::SELECTION] = GlassThemeActive() ? Glass::AccentBright : EditorTheme::AccentBright;
}

void EditorLayer::ReleaseGlass() {
    if (m_Glass) {
        m_Glass->Backdrop.Release();
        m_Glass->Ambient.Release();
        m_Glass->Compositor.Release();
        m_Glass.reset();
    }
    Glass::Motion().Reset();
}

void EditorLayer::ApplyPendingTheme() {
    if (m_PendingThemeId < 0) return;
    const int themeId = std::clamp(m_PendingThemeId, 0, 1);
    m_PendingThemeId = -1;
    EditorSettings& prefs = EditorSettings::Get();
    if (prefs.EditorThemeId == themeId) return;
    prefs.EditorThemeId = themeId;
    if (!m_Headless) EditorSettings::Save(); // a headless run (smoke test) never writes prefs
    ApplyThemeStyle();
    ApplyGizmoAccent();
    if (themeId == 0) ReleaseGlass();
}

void EditorLayer::SetGlassThemeForTest(bool on) {
    struct Saved { bool Huds, Blur, Objects, Properties, Library; };
    static Saved s_Saved = {true, true, false, false, false};
    EditorSettings& p = EditorSettings::Get();
    if (on) {
        s_Saved = {p.GlassViewportHuds, p.GlassFrostedBlur, p.ShowGlassHierarchy, p.ShowGlassInspector, p.ShowGlassAssets};
        p.GlassViewportHuds = p.GlassFrostedBlur = true;
        p.ShowGlassHierarchy = p.ShowGlassInspector = p.ShowGlassAssets = true;
        m_PendingThemeId = 1;
    } else {
        p.GlassViewportHuds = s_Saved.Huds;
        p.GlassFrostedBlur = s_Saved.Blur;
        p.ShowGlassHierarchy = s_Saved.Objects;
        p.ShowGlassInspector = s_Saved.Properties;
        p.ShowGlassAssets = s_Saved.Library;
        // Always back to Tartarus, even when the user's own prefs have Glass selected: the smoke
        // check is "switching off releases everything", which restoring "on" would never test.
        // A headless run never writes prefs, so the user's saved choice is untouched.
        m_PendingThemeId = 0;
    }
}

unsigned int EditorLayer::GlassBackdropTextureForTest() const { return m_Glass ? m_Glass->BlurTexture : 0u; }
int EditorLayer::GlassSurfacesForTest() const { return m_Glass ? m_Glass->Compositor.GlassSurfaces() : 0; }
int EditorLayer::GlassCapturesForTest() const { return m_Glass ? m_Glass->Compositor.Captures() : 0; }

void EditorLayer::GlassFrameBegin() {
    if (!GlassThemeActive()) return;
    Glass::ImAnimContext& motion = Glass::Motion();
    motion.SetReduceMotion(EditorSettings::Get().ReduceMotion);
    motion.NewFrame();
    // A see-through window shows the blurred desktop behind every panel: nothing to paint.
    // Otherwise the ambient field: the Scene image, blurred until only colour is left.
    GlassEditorState& g = GlassState();
    if (DesktopShowsThrough()) {
        PollGlassNotifications();
        return;
    }
    unsigned int ambient = 0;
    if (EditorSettings::Get().GlassFrostedBlur && m_SceneColorTexture != 0) {
        const int w = m_SceneTexW > 0 ? m_SceneTexW : (int)m_ViewportSize.x;
        const int h = m_SceneTexH > 0 ? m_SceneTexH : (int)m_ViewportSize.y;
        ambient = g.Ambient.Update(m_SceneColorTexture, w, h, Glass::AmbientBlurLevels, Glass::AmbientSaturation,
                                   Glass::AmbientCeiling);
    }
    Glass::DrawWallpaper((ImTextureID)(intptr_t)ambient);
    PollGlassNotifications();
}

// --- Notifications ----------------------------------------------------------------------------------

void EditorLayer::PushGlassToast(const std::string& title, const std::string& body, bool error) {
    GlassEditorState& g = GlassState();
    // At most four on screen: the oldest still showing leaves to make room.
    int showing = 0;
    for (const auto& t : g.Toasts) showing += t.Dismissed < 0.0 ? 1 : 0;
    if (showing >= 4) {
        for (auto& t : g.Toasts) {
            if (t.Dismissed < 0.0) { t.Dismissed = Glass::Motion().Now(); break; }
        }
    }
    GlassEditorState::Toast t;
    t.Id = g.NextToastId++;
    t.Title = title;
    t.Body = body.size() > 220 ? body.substr(0, 217) + "..." : body;
    t.Error = error;
    t.Born = Glass::Motion().Now();
    t.Dismissed = -1.0;
    t.Y = -1.0f;
    g.Toasts.push_back(std::move(t));
}

void EditorLayer::PollGlassNotifications() {
    GlassEditorState& g = GlassState();
    const std::vector<LogEntry>& entries = Log::Entries();
    const unsigned long long newest = entries.empty() ? 0ull : entries.back().Seq;
    if (!g.LogPrimed) {
        // Switching to Glass mid-session shouldn't replay every error since launch.
        g.LogPrimed = true;
        g.LastLogSeq = newest;
        return;
    }
    if (newest <= g.LastLogSeq) return;
    size_t first = entries.size();
    while (first > 0 && entries[first - 1].Seq > g.LastLogSeq) --first;
    for (size_t i = first; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (e.Level != LogLevel::Error) continue;
        const bool csharp = e.Message.rfind("C# error:", 0) == 0;
        const bool shader = ContainsNoCase(e.Message, "failed to compile") || ContainsNoCase(e.Message, "compile error");
        if (!csharp && !shader) continue;
        PushGlassToast(csharp ? "Script compile failed" : "Shader compile failed", e.Message, true);
    }
    g.LastLogSeq = newest;
}

void EditorLayer::GlassNotifySnap() {
    if (!GlassThemeActive() || !EditorSettings::Get().GlassViewportHuds) return;
    GlassEditorState& g = GlassState();
    g.SnapAt = ImGui::GetIO().MousePos;
    g.SnapShowing = true;
    Glass::Motion().Pulse(kSnapPulseId, Glass::SnapPop.durationMs * 0.001f);
}

// --- Workspace ----------------------------------------------------------------------------------

void EditorLayer::DrawGlassWorkspace(World& world, AssetLibrary& assets) {
    if (!GlassThemeActive()) return;
    DrawGlassHierarchy(world, assets);
    DrawGlassInspector(world, assets);
    DrawGlassLibrary(world, assets);
}

// Objects: the scene tree.
void EditorLayer::DrawGlassHierarchy(World& world, AssetLibrary& assets) {
    EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowGlassHierarchy) return;
    GlassEditorState& g = GlassState();
    auto& reg = world.Registry;

    {
        // First open: docked with the Hierarchy, or floating at the left of the editor.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        PlaceGlassPanel(EditorPanels::GlassHierarchy, EditorPanels::Hierarchy,
                        ImVec2(vp->WorkPos.x + Glass::Px(Glass::Space4), vp->WorkPos.y + Glass::Px(Glass::Space4) * 3.0f),
                        ImVec2(Glass::Px(300.0f), Glass::Px(520.0f)), g.PlaceObjects);
    }
    bool windowOpen = true;
    const bool visible = ImGui::Begin(EditorPanels::GlassHierarchy, &windowOpen);
    if (!windowOpen) {
        prefs.ShowGlassHierarchy = false;
        EditorSettings::Save();
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Search", g.HierarchyFilter, sizeof(g.HierarchyFilter));
    const bool filtering = g.HierarchyFilter[0] != '\0';

    // A new selection opens the path down to it (once, so the user can still fold it away).
    if (m_Selected != g.RevealedFor) {
        g.RevealedFor = m_Selected;
        if (reg.valid(m_Selected)) {
            const auto* h = reg.try_get<HierarchyComponent>(m_Selected);
            entt::entity p = h ? h->Parent : entt::null;
            for (int guard = 0; p != entt::null && reg.valid(p) && guard < 256; ++guard) {
                g.Expanded.insert(p);
                const auto* ph = reg.try_get<HierarchyComponent>(p);
                p = ph ? ph->Parent : entt::null;
            }
        }
    }

    // The visible rows: a depth-first walk of the expanded tree, or a flat list of matches.
    g.Rows.clear();
    if (filtering) {
        for (entt::entity e : reg.view<const NameComponent>()) {
            const std::string& n = reg.get<const NameComponent>(e).Name;
            const std::string shown = n.empty() ? "Object " + std::to_string(HierarchyCreationOrdinal(world, e)) : n;
            if (ContainsNoCase(shown, g.HierarchyFilter)) g.Rows.push_back({e, 0, false});
        }
        std::sort(g.Rows.begin(), g.Rows.end(), [&](const GlassEditorState::Row& a, const GlassEditorState::Row& b) {
            const auto* oa = reg.try_get<OrderComponent>(a.Entity);
            const auto* ob = reg.try_get<OrderComponent>(b.Entity);
            const int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
            return va != vb ? va < vb : a.Entity < b.Entity;
        });
    } else {
        std::function<void(entt::entity, int)> walk = [&](entt::entity parent, int depth) {
            for (entt::entity e : HierarchySiblingsInOrder(world, parent)) {
                if (!reg.valid(e)) continue;
                const auto* h = reg.try_get<HierarchyComponent>(e);
                const bool hasChildren = h && !h->Children.empty();
                g.Rows.push_back({e, depth, hasChildren});
                if (hasChildren && g.Expanded.count(e) && depth < 64) walk(e, depth + 1);
            }
        };
        walk(entt::null, 0);
    }

    ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    if (g.Rows.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
        ImGui::TextUnformatted(filtering ? "No matches" : "No objects");
        ImGui::PopStyleColor();
    } else {
        const float rowH = std::floor(ImGui::GetTextLineHeight() + Glass::Px(Glass::Space1h));
        const float indent = Glass::Px(Glass::Space2);
        const float chevronW = Glass::Px(Glass::Space2);
        const float inset = Glass::Px(Glass::SpaceHalf);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(Glass::Px(Glass::SpaceHalf), 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        if (ImGui::BeginTable("##tree", 2, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.5f);
            ImGuiTable* table = ImGui::GetCurrentTable();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImGuiIO& io = ImGui::GetIO();

            int selectedIndex = -1;
            for (int i = 0; i < (int)g.Rows.size(); ++i)
                if (g.Rows[(size_t)i].Entity == m_Selected) { selectedIndex = i; break; }

            float baseY = 0.0f;
            int baseIndex = -1;
            entt::entity toggle = entt::null;

            // Rows are transparent Selectables: the selection and hover surfaces are drawn
            // underneath, in the table's background channel, so they can be rounded and animate.
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
            ImGuiListClipper clipper;
            clipper.Begin((int)g.Rows.size(), rowH);
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const GlassEditorState::Row row = g.Rows[(size_t)i];
                    const entt::entity e = row.Entity;
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                    ImGui::TableSetColumnIndex(0);
                    const float y0 = table->RowPosY1;
                    if (baseIndex < 0) { baseIndex = i; baseY = y0; }
                    ImGui::PushID((int)entt::to_integral(e));

                    const bool selected = IsSelected(e);
                    const bool clicked = ImGui::Selectable("##row", selected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, rowH));
                    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    if (clicked) {
                        if (io.KeyShift) SelectHierarchyRange(world, e, io.KeyCtrl);
                        else SelectItem(e, io.KeyCtrl);
                    }
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) m_PendingFrameSelect = true;
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &e, sizeof(entt::entity));
                        const auto* nm = reg.try_get<NameComponent>(e);
                        ImGui::TextUnformatted(nm && !nm->Name.empty() ? nm->Name.c_str() : "Object");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginPopupContextItem("##ctx")) {
                        if (!IsSelected(e)) SelectItem(e, false);
                        DrawHierarchyContextMenu(world, assets, e);
                        ImGui::EndPopup();
                    }

                    // Hover glow (velocity-gated) and the static panel of a co-selected row. The
                    // primary selection's panel is drawn once after the loop, where it can glide.
                    const float glow = Glass::Motion().HoverGlow(ImGui::GetID("##glow"), hovered && !selected);
                    const ImRect rowRect(table->OuterRect.Min.x + inset, y0, table->OuterRect.Max.x - inset, y0 + rowH);
                    if (glow > 0.0f || (selected && e != m_Selected)) {
                        ImGui::TablePushBackgroundChannel();
                        if (selected && e != m_Selected)
                            Glass::DrawSquircle(dl, rowRect.Min, rowRect.Max, Col(Glass::AccentWash), Glass::Px(Glass::RadiusRow));
                        if (glow > 0.0f)
                            Glass::DrawSquircle(dl, rowRect.Min, rowRect.Max, Col(Glass::WithAlpha(Glass::WashHover, Glass::HoverWashAlpha * glow)),
                                                Glass::Px(Glass::RadiusRow));
                        ImGui::TablePopBackgroundChannel();
                    }

                    // Content, drawn by hand so the Selectable stays the row's item.
                    const bool inactive = reg.all_of<InactiveTag>(e);
                    const float x0 = ImGui::GetCursorScreenPos().x + (float)row.Depth * indent;
                    const float textMix = selected ? 1.0f : glow * Glass::HoverTextMix;
                    const ImVec4 textCol = inactive ? Glass::TextTertiary : Glass::MixOklab(Glass::TextSecondary, Glass::TextPrimary, textMix);
                    const float cellMaxX = table->Columns[0].ClipRect.Max.x;
                    float x = x0 + chevronW;
                    const char* glyph = EntityGlyph(reg, e, row.HasChildren);
                    DrawLabel(dl, x, y0, rowH, cellMaxX, Col(selected ? Glass::TextPrimary : Glass::TextTertiary), glyph);
                    x += ImGui::GetFontSize() * 1.25f + Glass::Px(Glass::SpaceHalf);
                    const auto* nm = reg.try_get<NameComponent>(e);
                    const std::string name = nm && !nm->Name.empty() ? nm->Name : "Object " + std::to_string(HierarchyCreationOrdinal(world, e));
                    DrawLabel(dl, x, y0, rowH, cellMaxX - Glass::Px(Glass::SpaceHalf), Col(textCol), name.c_str());

                    // Kind column: how many children a collapsed parent hides.
                    if (row.HasChildren && !g.Expanded.count(e) && !filtering) {
                        ImGui::TableSetColumnIndex(1);
                        const auto& kids = reg.get<HierarchyComponent>(e).Children;
                        char count[16];
                        std::snprintf(count, sizeof(count), "%d", (int)kids.size());
                        const ImRect cell = table->Columns[1].ClipRect;
                        const float cw = ImGui::CalcTextSize(count).x;
                        DrawLabel(dl, cell.Max.x - cw - Glass::Px(Glass::SpaceHalf), y0, rowH, cell.Max.x, Col(Glass::TextTertiary), count);
                        ImGui::TableSetColumnIndex(0);
                    }

                    // The disclosure chevron: its own small button on top of the row (AllowOverlap).
                    if (row.HasChildren && !filtering) {
                        const bool isOpen = g.Expanded.count(e) != 0;
                        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
                        if (ImGui::InvisibleButton("##chev", ImVec2(chevronW, rowH))) toggle = e;
                        const bool chevHovered = ImGui::IsItemHovered();
                        const bool flipped = toggle == e;
                        // Applied after the table; this frame already turns toward the new state.
                        const float turn = Glass::Motion().AnimateToggle(ImGui::GetID("##turn"), flipped ? !isOpen : isOpen,
                                                                         isOpen, Glass::ChevronTurn, Glass::ChevronTurn);
                        DrawChevron(dl, ImVec2(x0 + chevronW * 0.5f, y0 + rowH * 0.5f), Glass::Px(9.0f), turn,
                                    Col(chevHovered ? Glass::TextPrimary : Glass::TextTertiary));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopStyleColor(3);

            // The primary selection: a rounded panel with an accent bar that glides between rows.
            const ImGuiID glideId = ImGui::GetID("##selglide") ^ kSelectionGlideSalt;
            if (selectedIndex >= 0 && baseIndex >= 0) {
                const float target = (float)selectedIndex;
                const float prev = g.SelectionRowPrev < 0.0f ? target : g.SelectionRowPrev;
                const float at = Glass::Motion().Follow(glideId, target, prev, Glass::SelectionGlide);
                g.SelectionRowPrev = target;
                const float y = baseY + (at - (float)baseIndex) * rowH;
                const ImRect panel(table->OuterRect.Min.x + inset, y, table->OuterRect.Max.x - inset, y + rowH);
                // After the clipper loop no cell is current, so not TablePush/PopBackgroundChannel.
                Glass::TableBackground under;
                Glass::DrawSquircle(dl, panel.Min, panel.Max, Col(Glass::AccentWash), Glass::Px(Glass::RadiusRow));
                const float barW = Glass::Px(3.0f), barInset = Glass::Px(6.0f);
                dl->AddRectFilled(ImVec2(panel.Min.x + Glass::Px(Glass::SpaceHalf), panel.Min.y + barInset),
                                  ImVec2(panel.Min.x + Glass::Px(Glass::SpaceHalf) + barW, panel.Max.y - barInset),
                                  Col(Glass::Accent), barW * 0.5f);
            } else {
                g.SelectionRowPrev = -1.0f;
            }
            ImGui::EndTable();

            if (toggle != entt::null) {
                if (g.Expanded.count(toggle)) g.Expanded.erase(toggle);
                else g.Expanded.insert(toggle);
            }
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    ImGui::End();
}

// Properties: the selection's components as accordion cards.
void EditorLayer::DrawGlassInspector(World& world, AssetLibrary& assets) {
    EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowGlassInspector) return;
    GlassEditorState& g = GlassState();
    auto& reg = world.Registry;

    {
        // First open: docked with the Inspector, or floating at the right of the editor.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        PlaceGlassPanel(EditorPanels::GlassInspector, EditorPanels::Inspector,
                        ImVec2(vp->WorkPos.x + vp->WorkSize.x - Glass::Px(360.0f) - Glass::Px(Glass::Space4),
                               vp->WorkPos.y + Glass::Px(Glass::Space4) * 3.0f),
                        ImVec2(Glass::Px(360.0f), Glass::Px(560.0f)), g.PlaceProperties);
    }
    bool windowOpen = true;
    const bool visible = ImGui::Begin(EditorPanels::GlassInspector, &windowOpen);
    if (!windowOpen) {
        prefs.ShowGlassInspector = false;
        EditorSettings::Save();
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    const entt::entity e = m_Selected;
    if (!reg.valid(e)) {
        g.InspectorEntity = entt::null;
        const char* msg = "Nothing selected";
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(std::max(0.0f, (avail.x - sz.x) * 0.5f), std::max(0.0f, avail.y * 0.4f)));
        ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    // A new selection: the cards cascade in.
    const ImGuiID entranceId = ImGui::GetID("##entrance");
    if (g.InspectorEntity != e) {
        g.InspectorEntity = e;
        Glass::Motion().Pulse(entranceId, Glass::StaggerDelay(Glass::RevealStaggerCap, Glass::RevealStaggerMs, Glass::RevealStaggerCap) +
                                              Glass::RevealRowFadeMs * 0.001f);
    }
    const float entrance = Glass::Motion().PulseElapsed(entranceId, FLT_MAX);
    Glass::Motion().PulseProgress(entranceId); // releases the timer the frame it completes
    ImGuiStorage* storage = ImGui::GetStateStorage();
    int card = 0;

    CardTable cards;
    if (cards.Begin("##cards")) {
        // --- Identity: name and what the object is --------------------------------------------------
        {
            const float top = BeginCard();
            CardEntrance in(card++, entrance);
            auto* nm = reg.try_get<NameComponent>(e);
            // The field shows the live name unless it's being typed into (a rename from the
            // Hierarchy or an undo shows up immediately). Typing writes through every keystroke,
            // like a drag edit, and the whole edit is one undo step.
            const ImGuiID nameId = ImGui::GetID("##name");
            if (nm && ImGui::GetActiveID() != nameId)
                std::snprintf(g.RenameBuffer, sizeof(g.RenameBuffer), "%s", nm->Name.c_str());
            const auto* h = reg.try_get<HierarchyComponent>(e);
            ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextSecondary);
            ImGui::TextUnformatted(EntityGlyph(reg, e, h && !h->Children.empty()));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (nm) {
                const std::string hint = "Object " + std::to_string(HierarchyCreationOrdinal(world, e));
                const bool edited = ImGui::InputTextWithHint("##name", hint.c_str(), g.RenameBuffer, sizeof(g.RenameBuffer));
                if (ImGui::IsItemActivated()) StageUndo(world);
                if (edited) nm->Name = g.RenameBuffer;
                if (ImGui::IsItemDeactivatedAfterEdit()) CommitStagedUndo(world, "Rename");
            }
            int count = 0;
            for (const auto& rc : ComponentRegistry::All()) count += rc.Has(reg, e) ? 1 : 0;
            ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
            if (HasGroupSelection())
                ImGui::Text("%d component%s  \xC2\xB7  %d objects selected, editing the primary", count, count == 1 ? "" : "s",
                            (int)m_ExtraSelection.size() + 1);
            else
                ImGui::Text("%d component%s%s", count, count == 1 ? "" : "s", reg.all_of<InactiveTag>(e) ? "  \xC2\xB7  inactive" : "");
            ImGui::PopStyleColor();
            EndCard(top + in.Shift());
        }

        // --- Transform: a 4-column table, label + flat X / Y / Z fields ----------------------------
        if (auto* tf = reg.try_get<TransformComponent>(e)) {
            const float top = BeginCard();
            CardEntrance in(card++, entrance);
            ImGui::PushID("##transform");
            const ImGuiID openId = ImGui::GetID("open");
            bool open = storage->GetBool(openId, true);
            const bool wasOpen = open;
            const float turn = Glass::Motion().AnimateToggle(ImGui::GetID("turn"), open, open, Glass::ChevronTurn, Glass::ChevronTurn);
            if (SectionHeader("##hdr", ICON_FA_UP_DOWN_LEFT_RIGHT, "Transform", turn, nullptr)) {
                open = !open;
                storage->SetBool(openId, open);
                Glass::Motion().AnimateToggle(ImGui::GetID("turn"), open, wasOpen, Glass::ChevronTurn, Glass::ChevronTurn);
            }
            if (Glass::BeginReveal(ImGui::GetID("body"), open, wasOpen)) {
                ImGui::Dummy(ImVec2(0.0f, Glass::Px(Glass::SpaceHalf)));
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(Glass::Px(Glass::SpaceHalf), Glass::Px(2.0f)));
                // Label | X | Y | Z on one line when the fields get room for their numbers; in a narrow
                // panel the label goes on its own line above the three fields instead.
                const float labelW = ImGui::CalcTextSize("Rotation").x + Glass::Px(Glass::Space1);
                const float cellPadX = Glass::Px(Glass::SpaceHalf) * 2.0f;
                const float fieldW = (ImGui::GetContentRegionAvail().x - labelW) / 3.0f - cellPadX;
                const bool stacked = fieldW < Glass::Px(76.0f);
                if (ImGui::BeginTable("##xyz", stacked ? 3 : 4, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX)) {
                    if (!stacked) ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW);
                    ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthStretch);
                    glm::vec3 euler = tf->EulerDegrees();
                    struct VecRow { const char* label; glm::vec3* v; float speed; const char* undo; };
                    const VecRow rows[3] = {{"Position", &tf->Position, 0.1f, "Move"},
                                            {"Rotation", &euler, 1.0f, "Rotate"},
                                            {"Scale", &tf->Scale, 0.05f, "Scale"}};
                    const ImVec4 axis[3] = {Glass::AxisX, Glass::AxisY, Glass::AxisZ};
                    for (int r = 0; r < 3; ++r) {
                        ImGui::PushID(r);
                        // Stacked: the label is a row of its own above the fields (same cascade step,
                        // so the two settle together); otherwise it's the first column.
                        if (stacked) ImGui::TableNextRow();
                        else ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
                        {
                            ImGui::TableSetColumnIndex(0);
                            Glass::RevealRow cascade(r);
                            if (!stacked) ImGui::AlignTextToFramePadding();
                            ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextSecondary);
                            ImGui::TextUnformatted(rows[r].label);
                            ImGui::PopStyleColor();
                        }
                        if (stacked) ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
                        const int firstField = stacked ? 0 : 1;
                        const glm::vec3 before = *rows[r].v;
                        bool activated = false, committed = false;
                        for (int a = 0; a < 3; ++a) {
                            ImGui::TableSetColumnIndex(a + firstField);
                            Glass::RevealRow cascade(r);
                            ImGui::PushID(a);
                            float& value = (*rows[r].v)[a];
                            const float old = value;
                            // The flat axis bar: red / green / blue, just left of the field. Outside it,
                            // because a DragFloat centres its number, and in a narrow field the number
                            // spans the whole frame, so no padding keeps a bar inside clear of it.
                            const ImVec2 p = ImGui::GetCursorScreenPos();
                            const float barW = Glass::Px(3.0f), fh = ImGui::GetFrameHeight();
                            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + fh * 0.2f), ImVec2(p.x + barW, p.y + fh * 0.8f),
                                                                      Col(axis[a]), barW * 0.5f);
                            ImGui::SetCursorScreenPos(ImVec2(p.x + barW + Glass::Px(Glass::SpaceHalf), p.y));
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            const bool changed = ImGui::DragFloat("##v", &value, rows[r].speed, 0.0f, 0.0f, "%.3f");
                            if (ImGui::IsItemActivated()) activated = true;
                            if (ImGui::IsItemDeactivatedAfterEdit()) committed = true;
                            if (changed) {
                                if (!std::isfinite(value)) value = old; // never a nan/inf transform
                                else if (value == 0.0f) value = 0.0f;   // no "-0.000"
                            }
                            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                                EditorUI::SetTooltip("%.9g\nDrag to change; double-click to type", value);
                            ImGui::PopID();
                        }
                        if (r == 1 && *rows[r].v != before) tf->SetRotationEuler(euler);
                        if (activated) StageUndo(world);
                        if (committed) CommitStagedUndo(world, rows[r].undo);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
                Glass::EndReveal();
            }
            ImGui::PopID();
            EndCard(top + in.Shift());
        }

        // --- Every component the generic inspector draws, as a card each ---------------------------
        std::string handCoded;
        const RegisteredComponent* removeComponent = nullptr;
        const RegisteredComponent* resetComponent = nullptr;
        for (const auto& rc : ComponentRegistry::All()) {
            if (!rc.Has(reg, e)) continue;
            if (!rc.Meta.GenericInspector) {
                if (!handCoded.empty()) handCoded += ", ";
                handCoded += rc.Meta.Name;
                continue;
            }
            const float top = BeginCard();
            CardEntrance in(card++, entrance);
            ImGui::PushID(rc.Meta.Name);
            const ImGuiID openId = ImGui::GetID("open");
            bool open = storage->GetBool(openId, true);
            const bool wasOpen = open;
            const float turn = Glass::Motion().AnimateToggle(ImGui::GetID("turn"), open, open, Glass::ChevronTurn, Glass::ChevronTurn);
            if (SectionHeader("##hdr", rc.Meta.Icon, rc.Meta.Name, turn, nullptr)) {
                open = !open;
                storage->SetBool(openId, open);
                Glass::Motion().AnimateToggle(ImGui::GetID("turn"), open, wasOpen, Glass::ChevronTurn, Glass::ChevronTurn);
            }
            if (rc.Meta.Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) EditorUI::SetTooltip("%s", rc.Meta.Tooltip);
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Reset")) resetComponent = &rc;
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove Component")) removeComponent = &rc;
                ImGui::EndPopup();
            }
            if (Glass::BeginReveal(ImGui::GetID("body"), open, wasOpen)) {
                ImGui::Dummy(ImVec2(0.0f, Glass::Px(Glass::SpaceHalf)));
                {
                    Glass::RevealRow cascade(0);
                    DrawReflectedComponentFields(world, assets, rc, e);
                }
                Glass::EndReveal();
            }
            ImGui::PopID();
            EndCard(top + in.Shift());
        }
        cards.End();

        // Removal and reset after the loop, so no component is torn down while its card draws.
        if (resetComponent) {
            PushUndo(world, std::string("Reset ") + resetComponent->Meta.Name);
            resetComponent->Add(reg, e);
        }
        if (removeComponent) {
            PushUndo(world, std::string("Remove ") + removeComponent->Meta.Name);
            removeComponent->Remove(reg, e);
        }

        if (!handCoded.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::Text("%s: edit in the Inspector.", handCoded.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0.0f, Glass::Px(Glass::SpaceHalf)));
        DrawAddComponentMenu(world, assets, e);
    }
    ImGui::End();
}

// Library: the Asset Browser's current folder as a responsive grid.
void EditorLayer::DrawGlassLibrary(World& world, AssetLibrary& assets) {
    EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.ShowGlassAssets) return;
    GlassEditorState& g = GlassState();

    {
        // First open: docked with the Asset Browser, or floating along the bottom centre.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        PlaceGlassPanel(EditorPanels::GlassAssets, EditorPanels::Assets,
                        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - Glass::Px(640.0f)) * 0.5f,
                               vp->WorkPos.y + vp->WorkSize.y - Glass::Px(360.0f) - Glass::Px(Glass::Space4)),
                        ImVec2(Glass::Px(640.0f), Glass::Px(360.0f)), g.PlaceLibrary);
    }
    bool windowOpen = true;
    const bool visible = ImGui::Begin(EditorPanels::GlassAssets, &windowOpen);
    if (!windowOpen) {
        prefs.ShowGlassAssets = false;
        EditorSettings::Save();
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    // The same cell list the Asset Browser builds (folder, search, favourites and all). Rebuilding
    // it is idempotent; when the Asset Browser is also open it simply rebuilds it again.
    AssetGridFrameBegin(world, assets);
    auto& cells = m_AssetGridCells;

    // --- Path bar: up, then the folder's breadcrumb -----------------------------------------------
    const std::string folder = m_CurrentAssetFolder;
    if (GlassIconButton("##up", ICON_FA_ARROW_UP, "Enclosing folder", !folder.empty())) {
        const size_t slash = folder.find_last_of('/');
        NavigateAssetFolder(slash == std::string::npos ? std::string() : folder.substr(0, slash));
    }
    ImGui::SameLine();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = ImGui::GetFrameHeight();
        std::vector<std::pair<std::string, std::string>> crumbs; // (label, path)
        crumbs.push_back({"Library", std::string()});
        size_t start = 0;
        while (start < folder.size()) {
            const size_t slash = folder.find('/', start);
            const size_t end = slash == std::string::npos ? folder.size() : slash;
            crumbs.push_back({folder.substr(start, end - start), folder.substr(0, end)});
            start = end + 1;
        }
        for (size_t i = 0; i < crumbs.size(); ++i) {
            const bool last = i + 1 == crumbs.size();
            const ImVec2 sz = ImGui::CalcTextSize(crumbs[i].first.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImRect r(p, p + ImVec2(sz.x + Glass::Px(Glass::Space1), h));
            ImGui::PushID((int)i);
            const ImGuiID id = ImGui::GetID("##crumb");
            bool hov = false, held = false;
            if (Glass::MotionButton(id, r, r, &hov, &held) && !last) NavigateAssetFolder(crumbs[i].second);
            const float glow = Glass::Motion().HoverGlow(id, hov && !last);
            ImGui::PopID();
            const ImVec4 tc = last ? Glass::TextPrimary : Glass::MixOklab(Glass::TextSecondary, Glass::TextPrimary, glow);
            DrawLabel(dl, r.Min.x + Glass::Px(Glass::SpaceHalf), r.Min.y, h, r.Max.x, Col(tc), crumbs[i].first.c_str());
            if (last) { // the current folder: an accent underline
                dl->AddRectFilled(ImVec2(r.Min.x + Glass::Px(Glass::SpaceHalf), r.Max.y - Glass::Px(2.0f)),
                                  ImVec2(r.Max.x - Glass::Px(Glass::SpaceHalf), r.Max.y), Col(Glass::Accent), Glass::Px(1.0f));
            } else {
                ImGui::SameLine(0.0f, 0.0f);
                const ImVec2 cp = ImGui::GetCursorScreenPos();
                const ImRect cr(cp, cp + ImVec2(Glass::Px(Glass::Space2), h));
                DrawChevron(dl, cr.GetCenter(), Glass::Px(7.0f), 0.0f, Col(Glass::TextTertiary));
                ImGui::Dummy(cr.GetSize());
                ImGui::SameLine(0.0f, 0.0f);
            }
        }
        if (!m_AssetSearchFilter.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
            ImGui::AlignTextToFramePadding();
            ImGui::Text(ICON_FA_MAGNIFYING_GLASS "  \"%s\"", m_AssetSearchFilter.c_str());
            ImGui::PopStyleColor();
        }
    }

    // --- The wave: a new folder (or the first items arriving) ripples in, diagonal by diagonal ----
    const ImGuiID waveId = ImGui::GetID("##wave");
    if (g.WaveFolder != folder || (g.WaveCount == 0 && !cells.empty())) {
        g.WaveFolder = folder;
        Glass::Motion().Pulse(waveId, Glass::StaggerDelay(Glass::GridWaveCap, Glass::GridWaveStaggerMs, Glass::GridWaveCap) +
                                          Glass::GridWaveFadeMs * 0.001f);
    }
    g.WaveCount = (int)cells.size();
    const float waveElapsed = Glass::Motion().PulseElapsed(waveId, FLT_MAX);
    Glass::Motion().PulseProgress(waveId);
    if (waveElapsed != FLT_MAX) Glass::MarkLayoutInMotion();

    ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    if (cells.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Glass::TextTertiary);
        ImGui::TextUnformatted(m_AssetSearchFilter.empty() ? "This folder is empty" : "No matches");
        ImGui::PopStyleColor();
    } else {
        // Tiles shrink (thumbnail 72 down to 32 px) so at least one whole row, names included,
        // fits a short panel: a Library docked in a low strip still shows what each tile is.
        const float labelH = ImGui::GetTextLineHeight() + Glass::Px(Glass::Space1);
        const float chromeH = Glass::Px(Glass::Space1) + Glass::Px(Glass::SpaceHalf) + labelH + Glass::Px(Glass::SpaceHalf) * 2.0f;
        const float thumb = std::clamp(ImGui::GetContentRegionAvail().y - chromeH, Glass::Px(32.0f), Glass::Px(72.0f));
        const float tileW = std::max(thumb + Glass::Px(Glass::Space2) * 2.0f, Glass::Px(88.0f));
        const float tileH = Glass::Px(Glass::Space1) + thumb + Glass::Px(Glass::SpaceHalf) + labelH;
        const float gapX = Glass::Px(Glass::Space1);
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)std::floor((avail + gapX) / (tileW + gapX)));
        const int count = (int)cells.size();
        const int rowsN = (count + cols - 1) / cols;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(gapX * 0.5f, Glass::Px(Glass::SpaceHalf)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginTable("##tiles", cols, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoPadOuterX)) {
            for (int c = 0; c < cols; ++c) ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, tileW);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImGuiIO& io = ImGui::GetIO();
            const float lift = Glass::Px(Glass::TileHoverLiftPx);
            const float rise = Glass::Px(Glass::GridWaveRisePx);
            ImGuiListClipper clipper;
            clipper.Begin(rowsN, tileH + Glass::Px(Glass::SpaceHalf) * 2.0f);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, tileH);
                    for (int col = 0; col < cols; ++col) {
                        const int index = row * cols + col;
                        if (index >= count) break;
                        ImGui::TableSetColumnIndex(col);
                        AssetGridCell& cell = cells[(size_t)index];
                        const bool isFolder = cell.kind == AssetGridCell::Kind::Folder;
                        ImGui::PushID(cell.key.c_str());

                        // Wave: alpha and a rise, applied to the drawn tile (the slot stays put).
                        float wave = 1.0f;
                        if (waveElapsed != FLT_MAX) {
                            const float delay = Glass::StaggerDelay(row + col, Glass::GridWaveStaggerMs, Glass::GridWaveCap);
                            wave = Glass::EaseProgress(Glass::HoverIn, (waveElapsed - delay) / (Glass::GridWaveFadeMs * 0.001f));
                        }
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const ImRect rest(p, p + ImVec2(tileW, tileH));
                        // Hit rect: the union of the resting tile and its fully hovered, lifted
                        // form (rule 2), so hovering its edge can't flicker as it grows.
                        const ImRect hoverMax = Scaled(rest, Glass::TileHoverScale, ImVec2(0.0f, -lift));
                        const ImGuiID id = ImGui::GetID("##tile");
                        bool hovered = false, held = false;
                        const bool pressed = Glass::MotionButton(id, rest, Glass::MotionHitRect(rest, hoverMax), &hovered, &held);
                        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        if (pressed && cell.unloaded) LoadUnloadedAsset(assets, cell);
                        const bool selected = IsAssetSelected(cell.key, isFolder);
                        if (pressed && !doubleClicked) {
                            ClearSelection(); // one selection context at a time, as in the Asset Browser
                            if (io.KeyShift && !m_SelectedAssetKey.empty()) {
                                ClearAssetSelection();
                                const size_t lo = std::min(m_AssetSelectionAnchorIndex, (size_t)index);
                                const size_t hi = std::min(std::max(m_AssetSelectionAnchorIndex, (size_t)index), cells.size() - 1);
                                for (size_t k = lo; k <= hi; ++k) {
                                    const bool f = cells[k].kind == AssetGridCell::Kind::Folder;
                                    if (k == lo) { m_SelectedAssetKey = cells[k].key; m_SelectedAssetIsFolder = f; }
                                    else m_ExtraAssetSelection.push_back({cells[k].key, f});
                                }
                            } else if (io.KeyCtrl) {
                                ToggleAssetSelection(cell.key, isFolder);
                                m_AssetSelectionAnchorIndex = (size_t)index;
                            } else {
                                ClearAssetSelection();
                                m_SelectedAssetKey = cell.key;
                                m_SelectedAssetIsFolder = isFolder;
                                m_AssetSelectionAnchorIndex = (size_t)index;
                            }
                            if (cell.kind == AssetGridCell::Kind::Sound && !io.KeyShift && !io.KeyCtrl) {
                                if (AudioEngine::IsPreviewPlaying(cell.key)) AudioEngine::StopPreview();
                                else AudioEngine::PlayPreview(cell.key);
                            }
                        }
                        if (doubleClicked) {
                            using K = AssetGridCell::Kind;
                            if (isFolder) NavigateAssetFolder(cell.key);
                            else if (cell.kind == K::Scene) RequestOpenScene(world, assets, cell.key);
                            else if (cell.kind == K::Screenshot) OpenScreenshotPreview(cell.key);
                            else if (cell.kind == K::Animator) OpenAnimatorWindow(ProjectPaths::Relativize(cell.key));
                            else if (cell.kind == K::Prefab) EnterPrefabMode(world, assets, cell.key);
                            else if (cell.kind == K::Script) OpenScriptIDE(cell.key);
                        }
                        if (const char* payload = AssetPayloadType(cell.kind)) {
                            if (ImGui::BeginDragDropSource()) {
                                ImGui::SetDragDropPayload(payload, cell.key.c_str(), cell.key.size() + 1);
                                ImGui::TextUnformatted(cell.display.c_str());
                                ImGui::EndDragDropSource();
                            }
                        }
                        if (isFolder && ImGui::BeginDragDropTarget()) {
                            static const char* kFileable[] = {"ASSET_MODEL_PATH", "ASSET_TEXTURE_PATH", "ASSET_SOUND_PATH",
                                                              "ASSET_PREFAB_PATH", "ASSET_MATERIAL_PATH", "ASSET_FILE_PATH"};
                            for (const char* type : kFileable) {
                                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(type)) {
                                    PushUndo(world, "Move Asset to Folder");
                                    assets.SetAssetFolder((const char*)pl->Data, cell.key);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                            EditorUI::SetTooltip("%s", cell.display.c_str());

                        // Hover: lift, a 3% scale and a shadow that comes up from nothing.
                        const float glow = Glass::Motion().HoverGlow(id, hovered);
                        const float scale = 1.0f + (Glass::TileHoverScale - 1.0f) * glow;
                        const ImRect drawn = Scaled(rest, scale, ImVec2(0.0f, -lift * glow + rise * (1.0f - wave)));
                        const float a = wave;
                        if (a > 0.0f) {
                            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * a);
                            const float radius = Glass::Px(Glass::RadiusCard);
                            if (glow > 0.0f)
                                Glass::DrawSoftShadow(dl, drawn.Min, drawn.Max, radius, Glass::TileShadowAlpha * glow * a, Glass::Px(Glass::Space1));
                            // A faint resting plate, so a tile (glyph and name) reads as one object.
                            const float restPlate = 0.45f;
                            const ImVec4 plate = selected ? Glass::Field
                                                          : Glass::WithAlpha(Glass::Field, Glass::Field.w * (restPlate + (1.0f - restPlate) * glow));
                            if (plate.w > 0.0f) Glass::DrawSquircle(dl, drawn.Min, drawn.Max, Col(plate), radius);

                            // The thumbnail (real image when there is one, else the kind glyph).
                            const float tScale = drawn.GetWidth() / rest.GetWidth();
                            const float ts = thumb * tScale;
                            const ImVec2 tMin(drawn.GetCenter().x - ts * 0.5f, drawn.Min.y + Glass::Px(Glass::Space1) * tScale);
                            const ImVec2 tMax = tMin + ImVec2(ts, ts);
                            unsigned int tex = 0;
                            bool flipV = false;
                            float aspect = 1.0f;
                            if (cell.unloaded || cell.kind == AssetGridCell::Kind::Hdri) {
                                using K = AssetGridCell::Kind;
                                if (cell.kind == K::Texture || cell.kind == K::Model || cell.kind == K::Material || cell.kind == K::Hdri) {
                                    const AssetThumbnailLoader::Thumb t = m_AssetThumbs.Get(cell.key,
                                        cell.kind == K::Texture || cell.kind == K::Hdri ? AssetThumbnailLoader::Source::Image
                                                                                         : AssetThumbnailLoader::Source::Cached);
                                    tex = t.Tex;
                                    flipV = t.FlipV;
                                }
                            } else if (cell.texture && (cell.kind == AssetGridCell::Kind::Texture || cell.kind == AssetGridCell::Kind::Screenshot)) {
                                tex = cell.texture->GLHandle();
                                aspect = cell.texture->Height() > 0 ? (float)cell.texture->Width() / (float)cell.texture->Height() : 1.0f;
                            } else if (cell.kind == AssetGridCell::Kind::Model && cell.model) {
                                tex = ModelThumbnail(*cell.model);
                                flipV = true; // GL render targets are bottom-up
                            } else if (cell.kind == AssetGridCell::Kind::Material) {
                                tex = MaterialThumbnail(cell.material);
                                flipV = true;
                            }
                            if (tex) {
                                ImVec2 iMin = tMin, iMax = tMax;
                                if (aspect > 1.0f) { const float hh = ts / aspect; iMin.y += (ts - hh) * 0.5f; iMax.y = iMin.y + hh; }
                                else if (aspect < 1.0f) { const float ww = ts * aspect; iMin.x += (ts - ww) * 0.5f; iMax.x = iMin.x + ww; }
                                dl->AddImageRounded((ImTextureID)(intptr_t)tex, iMin, iMax, ImVec2(0.0f, flipV ? 1.0f : 0.0f),
                                                    ImVec2(1.0f, flipV ? 0.0f : 1.0f), Col(ImVec4(1, 1, 1, 1)), Glass::Px(Glass::RadiusRow));
                            } else {
                                const char* glyph = AssetGlyph(cell);
                                const float gsz = ts * 0.56f;
                                const ImVec2 gs = ImGui::GetFont()->CalcTextSizeA(gsz, FLT_MAX, 0.0f, glyph);
                                dl->AddText(ImGui::GetFont(), gsz, ImVec2(tMin.x + (ts - gs.x) * 0.5f, tMin.y + (ts - gs.y) * 0.5f),
                                            Col(isFolder ? Glass::TextSecondary : Glass::MixOklab(Glass::TextTertiary, Glass::TextSecondary, glow)), glyph);
                            }

                            // The name: selected = white on an accent capsule (Finder's selection).
                            const float ly = drawn.Max.y - labelH;
                            const ImVec2 ns = ImGui::CalcTextSize(cell.display.c_str());
                            const float maxW = drawn.GetWidth() - Glass::Px(Glass::SpaceHalf) * 2.0f;
                            const float lw = std::min(ns.x, maxW - Glass::Px(Glass::Space1));
                            const float lx = drawn.GetCenter().x - lw * 0.5f;
                            if (selected) {
                                const float padX = Glass::Px(Glass::SpaceHalf + 2.0f);
                                const float pillH = ImGui::GetTextLineHeight() + Glass::Px(Glass::SpaceHalf);
                                const float py = ly + (labelH - pillH) * 0.5f;
                                dl->AddRectFilled(ImVec2(lx - padX, py), ImVec2(lx + lw + padX, py + pillH), Col(Glass::Accent), pillH * 0.5f);
                            }
                            DrawLabel(dl, lx, ly, labelH, lx + lw, Col(selected ? Glass::OnAccent : Glass::TextPrimary), cell.display.c_str());
                            ImGui::PopStyleVar();
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    ImGui::End();
}

// --- Viewport HUDs ------------------------------------------------------------------------------

void EditorLayer::DrawGlassViewportOverlays(World& world) {
    if (!GlassThemeActive() || m_HideOverlaysThisFrame) return;
    const EditorSettings& prefs = EditorSettings::Get();
    if (!prefs.GlassViewportHuds) return;
    GlassEditorState& g = GlassState();
    const ImVec2 vMin(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 vMax(vMin.x + m_ViewportSize.x, vMin.y + m_ViewportSize.y);
    if (m_ViewportSize.x < Glass::Px(200.0f) || m_ViewportSize.y < Glass::Px(120.0f)) return;

    // The blurred scene behind the plates. One compute chain per frame, only while a HUD shows.
    g.BlurTexture = 0;
    if (prefs.GlassFrostedBlur && m_SceneColorTexture != 0) {
        const int w = m_SceneTexW > 0 ? m_SceneTexW : (int)m_ViewportSize.x;
        const int h = m_SceneTexH > 0 ? m_SceneTexH : (int)m_ViewportSize.y;
        g.BlurTexture = g.Backdrop.Update(m_SceneColorTexture, w, h, Glass::GlassBlurLevels, Glass::GlassSaturation, Glass::GlassCeiling);
    }
    const ImTextureID blur = (ImTextureID)(intptr_t)g.BlurTexture;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The HUDs are placed with SetCursorScreenPos and reserve their rects as items (so they're
    // hit-tested and block viewport picking). Snapshot the window's layout cursor and restore it
    // exactly at the end, so they leave no trace in the Scene window's layout or content size.
    // (Restoring with SetCursorScreenPos would put the cursor past CursorMaxPos with no item
    // after it, which ImGui rejects at End().)
    ImGuiWindow* const sceneWindow = ImGui::GetCurrentWindow();
    struct LayoutCursor {
        ImVec2 pos, prevLine, max, idealMax, currLine, prevLineSize;
        float currBase, prevBase;
        bool sameLine, setPos;
    };
    const ImGuiWindowTempData& dc0 = sceneWindow->DC;
    const LayoutCursor savedLayout{dc0.CursorPos, dc0.CursorPosPrevLine, dc0.CursorMaxPos, dc0.IdealMaxPos,
                                   dc0.CurrLineSize, dc0.PrevLineSize, dc0.CurrLineTextBaseOffset,
                                   dc0.PrevLineTextBaseOffset, dc0.IsSameLine, dc0.IsSetPos};
    const float margin = Glass::Px(Glass::Space2);
    const float pad = Glass::Px(Glass::Space1h);
    const float radius = Glass::Px(Glass::GlassRadius); // floating surfaces: 14 px
    const float lineH = ImGui::GetTextLineHeight();
    Glass::ImAnimContext& motion = Glass::Motion();
    auto& reg = world.Registry;

    // --- Selection card, bottom-right --------------------------------------------------------------
    float stackBottom = vMax.y - margin; // the stats pill sits above whatever is in this corner
    // The engine mark (DrawEngineMark) owns the bottom-right corner: the HUD stack sits to its left.
    const bool markShown = EditorSettings::Get().EngineMarkEnabled && m_MarkTexture != nullptr;
    const float right = vMax.x - margin - (markShown ? 54.0f * m_UIScale + Glass::Px(Glass::Space1) : 0.0f);
    {
        const bool has = reg.valid(m_Selected);
        const ImGuiID presentId = ImGui::GetID("##hudSelPresent");
        const bool was = g.HudWasShown;
        g.HudWasShown = has;
        if (has) g.HudEntity = m_Selected;
        const entt::entity shown = has ? m_Selected : g.HudEntity; // fading out: the last selection
        const float t = motion.AnimateToggle(presentId, has, was, Glass::RevealOpen, Glass::RevealClose);
        if (!has && t <= 0.0f) g.HudEntity = entt::null;
        if (t > 0.0f && reg.valid(shown)) {
            const auto* nm = reg.try_get<NameComponent>(shown);
            const std::string name = nm && !nm->Name.empty() ? nm->Name : "Object " + std::to_string(HierarchyCreationOrdinal(world, shown));
            const auto* tf = reg.try_get<TransformComponent>(shown);
            const float w = Glass::Px(260.0f);
            const float h = pad * 2.0f + lineH * 2.0f + Glass::Px(Glass::SpaceHalf) + (tf ? lineH + Glass::Px(Glass::SpaceHalf) : 0.0f);
            const ImRect rest(ImVec2(right - w, vMax.y - margin - h), ImVec2(right, vMax.y - margin));
            const ImRect drawn = Scaled(rest, 0.96f + 0.04f * t, ImVec2(0.0f, Glass::Px(Glass::RevealRowShiftPx) * (1.0f - t)));
            ImGui::SetCursorScreenPos(rest.Min);
            bool hov = false, held = false;
            if (Glass::MotionButton(ImGui::GetID("##hudSel"), rest, drawn, &hov, &held) && has) m_PendingFrameSelect = true;
            if (hov) EditorUI::SetTooltip("Click to frame the selection");
            Glass::DrawFrostedPlate(dl, drawn.Min, drawn.Max, radius, blur, vMin, vMax, t);
            float y = drawn.Min.y + pad;
            const float x = drawn.Min.x + pad;
            const auto* hc = reg.try_get<HierarchyComponent>(shown);
            dl->AddText(ImVec2(x, y), Col(Glass::TextSecondary, t), EntityGlyph(reg, shown, hc && !hc->Children.empty()));
            DrawLabel(dl, x + ImGui::GetFontSize() * 1.5f, y, lineH, drawn.Max.x - pad, Col(Glass::TextPrimary, t), name.c_str());
            y += lineH + Glass::Px(Glass::SpaceHalf);
            char caption[96];
            int comps = 0;
            for (const auto& rc : ComponentRegistry::All()) comps += rc.Has(reg, shown) ? 1 : 0;
            if (HasGroupSelection() && has) std::snprintf(caption, sizeof(caption), "%d selected", (int)m_ExtraSelection.size() + 1);
            else std::snprintf(caption, sizeof(caption), "%d component%s", comps, comps == 1 ? "" : "s");
            dl->AddText(ImVec2(x, y), Col(Glass::TextTertiary, t), caption);
            y += lineH + Glass::Px(Glass::SpaceHalf);
            if (tf) {
                const ImVec4 axis[3] = {Glass::AxisX, Glass::AxisY, Glass::AxisZ};
                const char* names[3] = {"X", "Y", "Z"};
                const float colW = (drawn.GetWidth() - pad * 2.0f) / 3.0f;
                for (int a = 0; a < 3; ++a) {
                    char v[32];
                    std::snprintf(v, sizeof(v), "%.2f", tf->Position[a]);
                    const float cx = x + colW * (float)a;
                    dl->AddText(ImVec2(cx, y), Col(axis[a], t), names[a]);
                    dl->AddText(ImVec2(cx + ImGui::CalcTextSize("X ").x, y), Col(Glass::TextPrimary, t), v);
                }
            }
            stackBottom = drawn.Min.y - Glass::Px(Glass::Space1);
        }
    }

    // --- Scene stats pill, above the selection card ------------------------------------------------
    {
        char stats[96];
        const int objects = (int)reg.view<const NameComponent>().size();
        const float ms = std::max(0.01f, m_SmoothedFrameMs);
        std::snprintf(stats, sizeof(stats), "%d objects   %.0f fps   %.1f ms", objects, 1000.0f / ms, ms);
        const ImVec2 ts = ImGui::CalcTextSize(stats);
        const float h = lineH + Glass::Px(Glass::Space1) * 2.0f - Glass::Px(Glass::SpaceHalf);
        const ImRect r(ImVec2(right - ts.x - pad * 2.0f, stackBottom - h), ImVec2(right, stackBottom));
        ImGui::SetCursorScreenPos(r.Min);
        bool hov = false, held = false;
        Glass::MotionButton(ImGui::GetID("##hudStats"), r, r, &hov, &held);
        Glass::DrawFrostedPlate(dl, r.Min, r.Max, r.GetHeight() * 0.5f, blur, vMin, vMax);
        dl->AddText(ImVec2(r.Min.x + pad, r.GetCenter().y - lineH * 0.5f), Col(Glass::TextSecondary), stats);
    }

    // --- Toasts, bottom-centre, newest at the bottom -----------------------------------------------
    if (!g.Toasts.empty()) {
        const double now = motion.Now();
        const float lifetime = Glass::ToastLifetimeMs * 0.001f;
        const float exitDur = Glass::ToastExit.durationMs * 0.001f;
        const float popDur = Glass::ToastPop.durationMs * 0.001f;
        const float w = std::min(Glass::Px(380.0f), m_ViewportSize.x - margin * 2.0f);
        const float wrap = w - pad * 2.0f - Glass::Px(Glass::Space2);
        float bottom = vMax.y - margin;
        bool anyMoving = false;
        for (int i = (int)g.Toasts.size() - 1; i >= 0; --i) {
            GlassEditorState::Toast& toast = g.Toasts[(size_t)i];
            const float age = (float)(now - toast.Born);
            if (toast.Dismissed < 0.0 && age >= lifetime) toast.Dismissed = now;
            const float exit = toast.Dismissed < 0.0 ? 0.0f : std::min(1.0f, (float)(now - toast.Dismissed) / exitDur);
            const ImVec2 bodySize = ImGui::CalcTextSize(toast.Body.c_str(), nullptr, false, wrap);
            const float h = pad * 2.0f + lineH + Glass::Px(Glass::SpaceHalf) + bodySize.y;

            // Restack smoothly when a toast above or below leaves.
            const float targetY = bottom - h;
            const float prevY = toast.Y < 0.0f ? targetY : toast.Y;
            const float y = motion.Follow(ImGui::GetID((int)toast.Id), targetY, prevY, Glass::SelectionGlide);
            toast.Y = targetY;

            // Pop: an under-damped spring overshoots past full size and settles.
            const float pop = motion.ReduceMotion() ? 1.0f : Glass::EaseProgress(Glass::ToastPop, age / popDur);
            const float exitE = Glass::EaseProgress(Glass::ToastExit, exit);
            const float scale = (Glass::ToastPopFromScale + (1.0f - Glass::ToastPopFromScale) * pop) * (1.0f - 0.04f * exitE);
            const float alpha = std::min(1.0f, age / 0.12f) * (1.0f - exitE);
            float shake = 0.0f;
            if (toast.Error && !motion.ReduceMotion() && age * 1000.0f < Glass::ErrorShake.durationMs)
                shake = Glass::SpringImpulse(age, Glass::ErrorShake.mass, Glass::ErrorShake.stiffness, Glass::ErrorShake.damping,
                                             Glass::Px(Glass::ErrorShake.amplitudePx));
            const ImRect rest(ImVec2(vMin.x + (m_ViewportSize.x - w) * 0.5f, y), ImVec2(vMin.x + (m_ViewportSize.x + w) * 0.5f, y + h));
            const ImRect drawn = Scaled(rest, scale, ImVec2(shake, Glass::Px(Glass::ToastPopRisePx) * (1.0f - pop)));
            anyMoving = anyMoving || pop < 1.0f || exit > 0.0f || shake != 0.0f;

            ImGui::SetCursorScreenPos(rest.Min);
            bool hov = false, held = false;
            ImGui::PushID((int)toast.Id);
            if (Glass::MotionButton(ImGui::GetID("##toast"), rest, drawn, &hov, &held) && toast.Dismissed < 0.0) toast.Dismissed = now;
            ImGui::PopID();
            if (hov && toast.Dismissed < 0.0) {
                if (age > popDur) toast.Born += (double)ImGui::GetIO().DeltaTime; // hovering holds it on screen
                EditorUI::SetTooltip("Click to dismiss. The full message is in the Console.");
            }
            if (alpha > 0.0f) {
                Glass::DrawFrostedPlate(dl, drawn.Min, drawn.Max, radius, blur, vMin, vMax, alpha);
                const ImVec4 status = toast.Error ? Glass::Danger : Glass::Accent;
                const float barW = Glass::Px(3.0f);
                dl->AddRectFilled(ImVec2(drawn.Min.x + pad * 0.5f, drawn.Min.y + pad), ImVec2(drawn.Min.x + pad * 0.5f + barW, drawn.Max.y - pad),
                                  Col(status, alpha), barW * 0.5f);
                const float tx = drawn.Min.x + pad + Glass::Px(Glass::Space1);
                float ty = drawn.Min.y + pad;
                dl->AddText(ImVec2(tx, ty), Col(status, alpha), toast.Error ? ICON_FA_CIRCLE_EXCLAMATION : ICON_FA_CIRCLE_DOT);
                dl->AddText(ImVec2(tx + ImGui::GetFontSize() * 1.5f, ty), Col(Glass::TextPrimary, alpha), toast.Title.c_str());
                ty += lineH + Glass::Px(Glass::SpaceHalf);
                dl->AddText(nullptr, 0.0f, ImVec2(tx, ty), Col(Glass::TextSecondary, alpha), toast.Body.c_str(), nullptr, wrap);
            }
            if (exit < 1.0f) bottom = y - Glass::Px(Glass::Space1) * (1.0f - exitE);
        }
        g.Toasts.erase(std::remove_if(g.Toasts.begin(), g.Toasts.end(), [&](const GlassEditorState::Toast& t) {
            return t.Dismissed >= 0.0 && (float)(now - t.Dismissed) >= exitDur;
        }), g.Toasts.end());
        if (g.Toasts.empty()) g.Toasts.shrink_to_fit(); // nothing showing, nothing held
        (void)anyMoving;
    }

    // --- Snap ring: an elastic pop where a snapped gizmo drag landed -------------------------------
    if (g.SnapShowing) {
        const float p = motion.PulseProgress(kSnapPulseId);
        if (p >= 1.0f) {
            g.SnapShowing = false;
        } else {
            const float s = Glass::EaseProgress(Glass::SnapPop, p);
            const float r = Glass::Px(18.0f) * (Glass::SnapRingFromScale + (Glass::SnapRingToScale - Glass::SnapRingFromScale) * s);
            const float a = 1.0f - Glass::EaseProgress(Glass::HoverIn, p);
            dl->AddCircle(g.SnapAt, r, Col(Glass::Accent, a), 0, Glass::Px(2.0f));
            dl->AddCircleFilled(g.SnapAt, Glass::Px(3.0f), Col(Glass::Accent, a));
        }
    }

    ImGuiWindowTempData& dc = sceneWindow->DC;
    dc.CursorPos = savedLayout.pos;
    dc.CursorPosPrevLine = savedLayout.prevLine;
    dc.CursorMaxPos = savedLayout.max;
    dc.IdealMaxPos = savedLayout.idealMax;
    dc.CurrLineSize = savedLayout.currLine;
    dc.PrevLineSize = savedLayout.prevLineSize;
    dc.CurrLineTextBaseOffset = savedLayout.currBase;
    dc.PrevLineTextBaseOffset = savedLayout.prevBase;
    dc.IsSameLine = savedLayout.sameLine;
    dc.IsSetPos = savedLayout.setPos;
}
