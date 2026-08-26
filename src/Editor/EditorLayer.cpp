#include "EditorLayer.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Texture.h"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder* — only used once, to lay out the default dock tree on first run
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <ImGuizmo.h>
#include <IconsFontAwesome6.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace {

// Height of the two-row top toolbar (dropdown menu bar + one-click toggle row) — shared so
// the Play/Stop button, which is positioned relative to it, stays in sync if that ever changes.
constexpr float kToolbarHeight = 84.0f;

// Reverse lookup for the Asset Browser: which placed objects reference a given asset.
// Compared by raw pointer (Model/Texture) since two placed objects can share one instance.
std::vector<std::string> FindModelUsages(const World& world, const Model* model) {
    std::vector<std::string> names;
    for (const auto& pm : world.Models) {
        if (pm.ModelRef.get() == model) names.push_back(pm.Name.empty() ? "(unnamed)" : pm.Name);
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
    for (const auto& pm : world.Models) {
        bool used = false;
        if (auto override_ = pm.ModelRef->MaterialOverride()) {
            used = MaterialUsesTexture(*override_, tex);
        } else {
            for (int i = 0; i < pm.ModelRef->MeshCount(); ++i) {
                if (MaterialUsesTexture(pm.ModelRef->MeshMaterial(i), tex)) { used = true; break; }
            }
        }
        if (used) names.push_back(pm.Name.empty() ? "(unnamed)" : pm.Name);
    }
    return names;
}

std::vector<std::string> FindSoundUsages(const World& world, const std::string& path) {
    std::vector<std::string> names;
    for (const auto& pm : world.Models) {
        if (pm.SoundPath == path) names.push_back(pm.Name.empty() ? "(unnamed)" : pm.Name);
    }
    return names;
}

std::string UsageTooltip(const std::vector<std::string>& users) {
    if (users.empty()) return "Not currently used by anything in the scene.";
    std::string s = "Used by:\n";
    for (size_t i = 0; i < users.size() && i < 10; ++i) s += "  - " + users[i] + "\n";
    if (users.size() > 10) s += "  ...and " + std::to_string(users.size() - 10) + " more\n";
    s.pop_back(); // drop the trailing newline
    return s;
}

// Case-insensitive substring match for the Asset Browser's search filter.
bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return toLower(text).find(toLower(filter)) != std::string::npos;
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

// Editable name field bound to a std::string, without depending on imgui_stdlib.h — copies
// into a fixed local buffer, writes back only on edit. Returns true the moment editing starts
// (for undo-snapshot timing), via activatedOut, same convention as DrawVec3Row below.
bool DrawNameField(const char* label, std::string& name, const char* placeholder, bool& activatedOut) {
    activatedOut = false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", name.c_str());
    bool changed = ImGui::InputTextWithHint(label, placeholder, buf, sizeof(buf));
    if (ImGui::IsItemActivated()) activatedOut = true;
    if (changed) name = buf;
    return changed;
}

// Icon-only action button with a tooltip carrying the full name — shared by DrawInspector and
// DrawMaterialEditor so both get the same compact single-row style instead of stacked
// full-width text buttons.
bool ActionButton(const char* icon, const char* tooltip) {
    bool clicked = ImGui::Button(icon);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool DeleteIconButton(const char* tooltip) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.22f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.26f, 0.26f, 1.00f));
    bool clicked = ImGui::Button(ICON_FA_TRASH);
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// Unity/Hazel-style vector row: a colored X/Y/Z button (click to zero that axis) glued to
// each drag field, instead of ImGui's plain unlabeled DragFloat3. `activatedOut` is set when
// any axis field starts being dragged this frame, for undo-snapshot timing at the call site.
bool DrawVec3Row(const char* label, glm::vec3& v, float speed, float minV, float maxV, bool& activatedOut) {
    activatedOut = false;
    bool changed = false;

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

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
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.0f, innerSpacing);
        ImGui::SetNextItemWidth(dragW);
        bool itemChanged = ImGui::DragFloat("##v", axes[i].value, speed, minV, maxV, "%.3f");
        if (ImGui::IsItemActivated()) activatedOut = true;
        changed |= itemChanged;

        ImGui::PopID();
        if (i < 2) ImGui::SameLine(0.0f, innerSpacing);
    }

    ImGui::PopID();
    return changed;
}

} // namespace

EditorLayer::EditorLayer() = default;
EditorLayer::~EditorLayer() = default;

void EditorLayer::Init(GLFWwindow* window) {
    m_Window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Layout/spacing polish: a bit more breathing room and softened corners read as more
    // deliberate than ImGui's sharp-cornered, tightly-packed defaults.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.24f, 0.60f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.015f, 0.015f, 0.015f, 0.97f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.03f, 0.03f, 0.98f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.4f);

    // Replace the default blue accent with a neutral dark grey across every widget that
    // uses it (checkmarks, sliders, buttons, selected tree/hierarchy rows, tabs, nav highlight).
    ImVec4 accent(0.32f, 0.32f, 0.34f, 1.00f);
    ImVec4 accentHovered(0.42f, 0.42f, 0.45f, 1.00f);
    ImVec4 accentActive(0.52f, 0.52f, 0.55f, 1.00f);

    style.Colors[ImGuiCol_CheckMark] = accentActive;
    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_SliderGrabActive] = accentActive;
    style.Colors[ImGuiCol_Button] = accent;
    style.Colors[ImGuiCol_ButtonHovered] = accentHovered;
    style.Colors[ImGuiCol_ButtonActive] = accentActive;
    style.Colors[ImGuiCol_Header] = accent;
    style.Colors[ImGuiCol_HeaderHovered] = accentHovered;
    style.Colors[ImGuiCol_HeaderActive] = accentActive;
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.32f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered] = accentHovered;
    style.Colors[ImGuiCol_SeparatorActive] = accentActive;
    style.Colors[ImGuiCol_ResizeGripHovered] = accentHovered;
    style.Colors[ImGuiCol_ResizeGripActive] = accentActive;
    style.Colors[ImGuiCol_TabSelected] = accent;
    style.Colors[ImGuiCol_TabHovered] = accentHovered;
    style.Colors[ImGuiCol_TabSelectedOverline] = accentActive;
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    style.Colors[ImGuiCol_NavCursor] = accentActive;

    // UI text font. Loaded straight from the Windows system font directory rather than
    // bundled into the repo (Segoe UI is Microsoft-licensed, not ours to redistribute).
    // Falls back to ImGui's built-in bitmap font if it's ever missing (e.g. running under
    // Wine, or a stripped-down Windows install), so this never hard-fails.
    ImFontConfig baseFontConfig;
    baseFontConfig.SizePixels = 16.0f;
    ImFont* uiFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &baseFontConfig);
    if (!uiFont) {
        ImFontConfig fallbackConfig;
        fallbackConfig.SizePixels = 15.0f;
        io.Fonts->AddFontDefault(&fallbackConfig);
    }
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = 16.0f;
    io.Fonts->AddFontFromFileTTF("assets/fonts/fa-solid-900.ttf", 16.0f, &iconConfig, iconRanges);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_LogoTexture = std::make_unique<Texture>("assets/branding/atrocity_exhibition_logo.png");
    if (!m_LogoTexture->IsValid()) m_LogoTexture.reset(); // missing file — just skip the watermark
}

void EditorLayer::Shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void EditorLayer::BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void EditorLayer::EndFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void EditorLayer::ApplyPanelPlacement(glm::vec2 pos, glm::vec2 size) const {
    ImGuiCond cond = m_LayoutLocked ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y), cond);
    ImGui::SetNextWindowSize(ImVec2(size.x, size.y), cond);
}

void EditorLayer::PushUndo(const World& world) {
    m_UndoStack.push_back(SceneSerializer::SaveToString(world));
    if (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.erase(m_UndoStack.begin());
    }
    m_RedoStack.clear(); // a fresh edit invalidates whatever redo history existed
    m_Dirty = true; // every discrete edit already snapshots here first, so this is the one spot that needs to set it
}

void EditorLayer::Undo(World& world, AssetLibrary& assets) {
    if (m_UndoStack.empty()) return;
    m_RedoStack.push_back(SceneSerializer::SaveToString(world));
    std::string snapshot = m_UndoStack.back();
    m_UndoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, snapshot);
    ClearSelection();
    m_Dirty = true;
}

void EditorLayer::Redo(World& world, AssetLibrary& assets) {
    if (m_RedoStack.empty()) return;
    m_UndoStack.push_back(SceneSerializer::SaveToString(world));
    std::string snapshot = m_RedoStack.back();
    m_RedoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, snapshot);
    ClearSelection();
    m_Dirty = true;
}

void EditorLayer::ClearSelection() {
    m_SelectedBox = -1;
    m_SelectedModel = -1;
    m_ExtraSelection.clear();
}

void EditorLayer::SelectItem(bool isModel, int index, bool addToSelection) {
    bool isPrimary = isModel ? (m_SelectedModel == index) : (m_SelectedBox == index);

    if (!addToSelection) {
        m_ExtraSelection.clear();
        if (isModel) { m_SelectedModel = index; m_SelectedBox = -1; }
        else { m_SelectedBox = index; m_SelectedModel = -1; }
        return;
    }

    if (isPrimary) {
        // Demote: promote the most recently added extra to primary, or clear if none left.
        if (!m_ExtraSelection.empty()) {
            SelectedItem promoted = m_ExtraSelection.back();
            m_ExtraSelection.pop_back();
            if (promoted.IsModel) { m_SelectedModel = promoted.Index; m_SelectedBox = -1; }
            else { m_SelectedBox = promoted.Index; m_SelectedModel = -1; }
        } else {
            m_SelectedBox = -1;
            m_SelectedModel = -1;
        }
        return;
    }

    for (auto it = m_ExtraSelection.begin(); it != m_ExtraSelection.end(); ++it) {
        if (it->IsModel == isModel && it->Index == index) {
            m_ExtraSelection.erase(it); // already co-selected — toggle it back off
            return;
        }
    }

    if (!HasAnySelection()) {
        if (isModel) m_SelectedModel = index; else m_SelectedBox = index;
    } else {
        m_ExtraSelection.push_back({isModel, index});
    }
}

void EditorLayer::AddToSelectionIfAbsent(bool isModel, int index) {
    if (IsSelected(isModel, index)) return; // leave already-selected items alone — don't toggle them off
    if (!HasAnySelection()) {
        if (isModel) m_SelectedModel = index; else m_SelectedBox = index;
    } else {
        m_ExtraSelection.push_back({isModel, index});
    }
}

void EditorLayer::DeleteSelection(World& world) {
    PushUndo(world);

    std::vector<int> modelIndices, boxIndices;
    auto collect = [&](bool isModel, int index) {
        if (index < 0) return;
        (isModel ? modelIndices : boxIndices).push_back(index);
    };
    collect(true, m_SelectedModel);
    collect(false, m_SelectedBox);
    for (const auto& item : m_ExtraSelection) collect(item.IsModel, item.Index);

    // Models are physically erased from the vector, so later indices must go first or an
    // earlier erase would shift the ones still queued for deletion out from under it.
    std::sort(modelIndices.rbegin(), modelIndices.rend());
    for (int idx : modelIndices) {
        if (idx < (int)world.Models.size()) world.Models.erase(world.Models.begin() + idx);
    }

    // Boxes use the existing "Alive" flag convention instead of erasing (matches the single-
    // object delete path and keeps World::Raycast/ResolveCollisions indices stable).
    for (int idx : boxIndices) {
        if (idx < (int)world.Boxes.size()) world.Boxes[idx].Alive = false;
    }

    ClearSelection();
}

void EditorLayer::DuplicateSelection(World& world, AssetLibrary& assets) {
    if (!HasAnySelection()) return;
    PushUndo(world);

    std::vector<SelectedItem> source;
    if (m_SelectedModel >= 0) source.push_back({true, m_SelectedModel});
    else if (m_SelectedBox >= 0) source.push_back({false, m_SelectedBox});
    for (const auto& item : m_ExtraSelection) source.push_back(item);

    // Nudge each copy sideways so it doesn't land exactly on top of the original — original
    // indices in `source` stay valid throughout since every new object is push_back'd (only
    // ever appended, never inserted/erased), so reallocation never invalidates them.
    const glm::vec3 kOffset(0.5f, 0.0f, 0.5f);
    std::vector<SelectedItem> created;

    for (const auto& item : source) {
        if (item.IsModel) {
            if (item.Index < 0 || item.Index >= (int)world.Models.size()) continue;
            const PlacedModel& src = world.Models[item.Index];

            PlacedModel copy = src;
            copy.Name = src.Name.empty() ? "Model Copy" : (src.Name + " Copy");
            copy.Position += kOffset;
            // Its own Model instance (own material-override/animation state), not the same
            // shared_ptr as the original — otherwise recoloring one copy would recolor every
            // duplicate made from it, since Model (not PlacedModel) owns the material override.
            copy.ModelRef = assets.CloneModel(src.ModelRef);
            if (auto srcMat = src.ModelRef->MaterialOverride()) {
                copy.ModelRef->SetMaterialOverride(std::make_shared<Material>(*srcMat));
            }

            world.Models.push_back(copy);
            created.push_back({true, (int)world.Models.size() - 1});
        } else {
            if (item.Index < 0 || item.Index >= (int)world.Boxes.size()) continue;
            const WorldBox& src = world.Boxes[item.Index];

            WorldBox copy = src;
            std::string baseName = src.Name.empty() ? ("Box " + std::to_string(item.Index)) : src.Name;
            copy.Name = baseName + " Copy";
            copy.Center += kOffset;

            world.Boxes.push_back(copy);
            created.push_back({false, (int)world.Boxes.size() - 1});
        }
    }

    // Select the new duplicates instead of the originals, so you can immediately drag them
    // into place without having to re-pick them from the Hierarchy.
    ClearSelection();
    for (size_t i = 0; i < created.size(); ++i) {
        if (i == 0) {
            if (created[i].IsModel) m_SelectedModel = created[i].Index;
            else m_SelectedBox = created[i].Index;
        } else {
            m_ExtraSelection.push_back(created[i]);
        }
    }
}

void EditorLayer::FocusOnSelection(World& world, Camera& editorCamera) {
    if (!HasAnySelection()) return;

    glm::vec3 boundsMin(1e30f), boundsMax(-1e30f);
    bool any = false;
    auto expand = [&](bool isModel, int index) {
        if (isModel) {
            if (index < 0 || index >= (int)world.Models.size()) return;
            PlacedModel& pm = world.Models[index];
            glm::mat4 m = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
            AABB bounds = AABB{pm.ModelRef->BoundsMin(), pm.ModelRef->BoundsMax()}.Transformed(m);
            boundsMin = glm::min(boundsMin, bounds.Min);
            boundsMax = glm::max(boundsMax, bounds.Max);
        } else {
            if (index < 0 || index >= (int)world.Boxes.size()) return;
            WorldBox& box = world.Boxes[index];
            glm::mat4 m = ComposeTransform(box.Center, box.RotationEuler, glm::vec3(1.0f));
            AABB bounds = AABB{-box.Size * 0.5f, box.Size * 0.5f}.Transformed(m);
            boundsMin = glm::min(boundsMin, bounds.Min);
            boundsMax = glm::max(boundsMax, bounds.Max);
        }
        any = true;
    };
    expand(m_SelectedModel >= 0, m_SelectedModel >= 0 ? m_SelectedModel : m_SelectedBox);
    for (const auto& item : m_ExtraSelection) expand(item.IsModel, item.Index);
    if (!any) return;

    glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    float radius = glm::length(boundsMax - boundsMin) * 0.5f;
    radius = std::max(radius, 0.5f); // guard against a degenerate/zero-size bounds parking the camera inside it

    // Keep the camera's current aim, just slide it back along that same ray until the
    // selection's bounding sphere fits inside the vertical field of view, with a margin.
    float distance = (radius / std::sin(glm::radians(editorCamera.Fov) * 0.5f)) * 1.35f;
    editorCamera.Position = center - editorCamera.Front() * distance;
}

bool EditorLayer::WantsCaptureMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool EditorLayer::WantsCaptureKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void EditorLayer::Draw(World& world, AssetLibrary& assets, Camera& editorCamera, float dt) {
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
    const float toolbarH = kToolbarHeight;

    ApplyPanelPlacement({0, 0}, {w, toolbarH});
    DrawTopToolbar(world, assets, editorCamera);

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
    if (m_ResetLayoutRequested) {
        // Tear down the existing tree (whatever the user dragged panels into) so the block
        // below rebuilds the original default split from scratch, same as a first launch.
        ImGui::DockBuilderRemoveNode(dockspaceId);
        m_ResetLayoutRequested = false;
    }
    if (!ImGui::DockBuilderGetNode(dockspaceId)) {
        // ImGuiDockNodeFlags_DockSpace is what marks the root as a real dockspace so its
        // empty center leaf survives as the CentralNode (visible-when-empty) after the splits
        // below, instead of being reclaimed by a neighbor — PassthruCentralNode alone (passed
        // to DockSpace() further down) only controls how that surviving central node renders.
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
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
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    // The dockspace's central node IS the viewport — read its live rect so picking, gizmos,
    // and the 3D render itself (main.cpp reads these via ViewportPos()/ViewportSize()) track
    // the Hierarchy/Inspector/Asset Browser panels being resized instead of assuming the
    // viewport always fills the whole window underneath them.
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        m_ViewportPos = {centralNode->Pos.x, centralNode->Pos.y};
        m_ViewportSize = {centralNode->Size.x, centralNode->Size.y};
    } else {
        m_ViewportPos = {0.0f, toolbarH};
        m_ViewportSize = {w, h - toolbarH};
    }

    DrawHierarchy(world, assets);
    DrawInspector(world, assets, dt);
    DrawAssetBrowser(world, assets);

    // Studio watermark: small, translucent, bottom-right of the whole window. Sits over
    // whatever the passthrough center (viewport) node currently occupies in the default
    // layout; a non-interactive overlay like the "##hint" one below, so it never steals
    // clicks from the gizmo/viewport underneath it.
    if (m_LogoTexture) {
        float logoH = 110.0f;
        float logoW = logoH * ((float)m_LogoTexture->Width() / (float)m_LogoTexture->Height());
        ImGui::SetNextWindowPos(ImVec2(w - logoW - 18.0f, h - logoH - 14.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(logoW, logoH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##BrandWatermark", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::ImageWithBg((ImTextureID)(intptr_t)m_LogoTexture->GLHandle(), ImVec2(logoW, logoH),
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 0.5f));
        ImGui::End();
        ImGui::PopStyleVar();
    }

    DrawViewportDropTarget(world, assets, editorCamera);

    // Holding V is a dedicated mode: it takes over the mouse for vertex grab-and-drag, so the
    // normal click-to-select and transform gizmo stand down while it's held to avoid the two
    // systems fighting over the same click. The grabbed vertex always belongs to the PRIMARY
    // selected model, but if a group is selected, every other member rides along by the same
    // delta each frame (see UpdateVertexDrag) so the whole group snaps together via that one
    // vertex instead of only the primary moving.
    bool vHeld = !ImGui::GetIO().WantTextInput && ImGui::IsKeyDown(ImGuiKey_V);

    if (m_VertexDragActive && (!vHeld || !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        m_VertexDragActive = false; // dropped: releasing V or the mouse button leaves it exactly where it is
    }

    glm::vec3 hoverLocal;
    bool hasHover = vHeld && !m_VertexDragActive && !ImGui::GetIO().WantCaptureMouse &&
        FindVertexUnderCursor(world, editorCamera, hoverLocal);

    if (hasHover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        PushUndo(world);
        PlacedModel& pm = world.Models[m_SelectedModel];
        glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
        glm::vec3 grabbedWorld = glm::vec3(model * glm::vec4(hoverLocal, 1.0f));
        m_VertexDragLocal = hoverLocal;
        m_VertexDragPlanePoint = grabbedWorld;
        m_VertexDragOffset = pm.Position - grabbedWorld;
        m_VertexDragActive = true;
    }

    if (m_VertexDragActive) {
        UpdateVertexDrag(world, editorCamera);
    }

    if (!vHeld) {
        HandleViewportPicking(world, editorCamera);
        DrawGizmo(world, editorCamera);
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
        } else if (m_SelectedModel < 0) {
            ImGui::TextDisabled("Select a model first, then aim at one of its vertices");
        } else if (hasHover) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Click and hold to grab this vertex");
        } else {
            ImGui::TextDisabled("Aim closer to an edge or corner of the model");
        }
        ImGui::End();
    }

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) m_GizmoOp = GizmoOp::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) m_GizmoOp = GizmoOp::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) m_GizmoOp = GizmoOp::Scale;

        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo(world, assets);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo(world, assets);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            SceneSerializer::Save(world, assets, m_CurrentScenePath);
            m_Dirty = false;
        }
        if (HasAnySelection() && ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelection(world);
        if (HasAnySelection() && ImGui::IsKeyPressed(ImGuiKey_F)) FocusOnSelection(world, editorCamera);
        if (HasAnySelection() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelection(world, assets);

        if (!m_SelectedAssetKey.empty() && m_RenamingAssetKey.empty() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            std::string currentName = m_SelectedAssetIsFolder ? LeafNameOf(m_SelectedAssetKey) : assets.DisplayName(m_SelectedAssetKey);
            BeginRenameAsset(m_SelectedAssetKey, m_SelectedAssetIsFolder, currentName);
        }
    }

    if (hasHover) {
        // Not-yet-grabbed indicator: yellow circle at the vertex the cursor is closest to.
        PlacedModel& pm = world.Models[m_SelectedModel];
        glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
        glm::mat4 viewProj = editorCamera.ProjectionMatrix(w / h) * editorCamera.ViewMatrix();
        glm::vec4 clip = viewProj * model * glm::vec4(hoverLocal, 1.0f);
        if (clip.w > 0.0001f) {
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            ImVec2 screen((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
            ImGui::GetForegroundDrawList()->AddCircle(screen, 8.0f, IM_COL32(255, 217, 77, 230), 0, 2.5f);
        }
    }

    if (m_VertexDragActive && m_SelectedModel >= 0 && m_SelectedModel < (int)world.Models.size()) {
        // Actively grabbed: filled yellow dot tracking the vertex's live (post-drag) position.
        PlacedModel& pm = world.Models[m_SelectedModel];
        glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
        glm::mat4 viewProj = editorCamera.ProjectionMatrix(w / h) * editorCamera.ViewMatrix();
        glm::vec4 clip = viewProj * model * glm::vec4(m_VertexDragLocal, 1.0f);
        if (clip.w > 0.0001f) {
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            ImVec2 screen((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
            ImGui::GetForegroundDrawList()->AddCircle(screen, 7.0f, IM_COL32(255, 217, 77, 255), 0, 2.0f);
            ImGui::GetForegroundDrawList()->AddCircleFilled(screen, 3.0f, IM_COL32(255, 217, 77, 255));
        }
    }
}

void EditorLayer::DrawPlayStopButton(bool editorMode) {
    int ww, wh;
    glfwGetWindowSize(m_Window, &ww, &wh);
    float w = (float)ww;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    if (editorMode) {
        // Centered in the toolbar strip's icon row (above the viewport, not over it) —
        // transparent background so it reads as part of that already-dark toolbar rather
        // than a floating card. Called after DrawTopToolbar this frame so it layers on top.
        float y = kToolbarHeight * 0.64f; // icon row's vertical center, below the menu bar row
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        flags |= ImGuiWindowFlags_NoBackground;
    } else {
        // No toolbar in play mode, so this floats near the top of the full window instead —
        // opaque enough to read clearly over the 3D scene.
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, 10.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
    }
    ImGui::Begin("##PlayStopButton", nullptr, flags);

    if (editorMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.24f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.68f, 0.30f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.80f, 0.36f, 1.00f));
        if (ImGui::Button(ICON_FA_PLAY "  Play")) m_PlayStopRequested = true;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Return to game (F1)");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.22f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.26f, 0.26f, 1.00f));
        if (ImGui::Button(ICON_FA_STOP "  Stop")) m_PlayStopRequested = true;
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Return to editor (F1)");
    }

    ImGui::End();
}

void EditorLayer::OpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    if (path.empty() || !SceneSerializer::Load(world, assets, path)) return;
    m_CurrentScenePath = path;
    ClearSelection();
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_Dirty = false;
}

void EditorLayer::DrawTopToolbar(World& world, AssetLibrary& assets, Camera& editorCamera) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar;
    if (m_LayoutLocked) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("##Toolbar", nullptr, flags)) { ImGui::End(); return; }

    // Real dropdown menus for the stuff you reach for occasionally (import, add primitive,
    // scene save/load) — keeps the always-visible row below reserved for one-click toggles.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " File")) {
            if (ImGui::MenuItem(ICON_FA_FILE "  New Scene")) {
                world = World();
                ClearSelection();
                m_UndoStack.clear();
                m_RedoStack.clear();
                m_Dirty = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open...")) {
                OpenScene(world, assets, FileDialog::OpenFile(
                    "Scene Files\0*.json\0All Files\0*.*\0", m_Window));
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save")) {
                SceneSerializer::Save(world, assets, m_CurrentScenePath);
                m_Dirty = false;
            }
            if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save As...")) {
                std::string path = FileDialog::SaveFile(
                    "Scene Files\0*.json\0All Files\0*.*\0", "json", m_Window);
                if (!path.empty()) {
                    SceneSerializer::Save(world, assets, path);
                    m_CurrentScenePath = path;
                    m_Dirty = false;
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Current: %s%s", std::filesystem::path(m_CurrentScenePath).filename().string().c_str(), m_Dirty ? " (unsaved)" : "");
            ImGui::TextDisabled("Also auto-saves on exit,\nauto-loads on launch.");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_FILE_IMPORT " Import")) {
            if (ImGui::MenuItem(ICON_FA_CUBE "  Model  (FBX / OBJ / glTF)...")) {
                std::string path = FileDialog::OpenFile(
                    "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0", m_Window);
                if (!path.empty()) {
                    PushUndo(world);
                    auto model = assets.LoadModel(path);
                    PlacedModel pm;
                    pm.ModelRef = model;
                    pm.Name = std::filesystem::path(path).stem().string();
                    pm.Position = editorCamera.Position + editorCamera.Front() * 5.0f;
                    world.Models.push_back(pm);
                    SelectItem(true, (int)world.Models.size() - 1, false);
                }
            }
            if (ImGui::MenuItem(ICON_FA_IMAGE "  Texture  (PNG / JPG / TGA)...")) {
                std::string path = FileDialog::OpenFile(
                    "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
                if (!path.empty()) assets.LoadTexture(path);
            }
            if (ImGui::MenuItem(ICON_FA_MUSIC "  Sound  (WAV / MP3 / OGG)...")) {
                std::string path = FileDialog::OpenFile(
                    "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0", m_Window);
                if (!path.empty() && AudioEngine::Load(path)) assets.RegisterSound(path);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CUBES " Add")) {
            auto spawnPrimitive = [&](const char* kind, const char* displayName) {
                PushUndo(world);
                auto model = assets.CreatePrimitive(kind);
                PlacedModel pm;
                pm.ModelRef = model;
                pm.Name = displayName;
                pm.Position = editorCamera.Position + editorCamera.Front() * 5.0f;
                world.Models.push_back(pm);
                SelectItem(true, (int)world.Models.size() - 1, false);
            };
            if (ImGui::MenuItem(ICON_FA_CUBE "  Cube")) spawnPrimitive("cube", "Cube");
            if (ImGui::MenuItem(ICON_FA_CIRCLE "  Sphere")) spawnPrimitive("sphere", "Sphere");
            if (ImGui::MenuItem(ICON_FA_SHAPES "  Cylinder")) spawnPrimitive("cylinder", "Cylinder");
            if (ImGui::MenuItem(ICON_FA_SHAPES "  Cone")) spawnPrimitive("cone", "Cone");
            if (ImGui::MenuItem(ICON_FA_SHAPES "  Plane")) spawnPrimitive("plane", "Plane");
            ImGui::Separator();
            ImGui::TextDisabled("Real mesh data — supports materials,\nvertex snapping, gizmos. No collision yet\n(same as imported models).");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_GEAR " Settings")) {
            ImGui::SeparatorText(ICON_FA_TABLE_COLUMNS "  Layout");
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset Layout")) {
                m_ResetLayoutRequested = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Puts Scene Hierarchy / Inspector / Asset Browser back\nin their default docked positions and sizes.");
            }

            ImGui::SeparatorText(ICON_FA_SUN "  Environment");
            ImGui::ColorEdit3("Horizon Color", &world.SkyHorizonColor.x, ImGuiColorEditFlags_DisplayHex);
            if (ImGui::IsItemActivated()) PushUndo(world);
            ImGui::ColorEdit3("Zenith Color", &world.SkyZenithColor.x, ImGuiColorEditFlags_DisplayHex);
            if (ImGui::IsItemActivated()) PushUndo(world);

            ImGui::SeparatorText(ICON_FA_TABLE_CELLS "  Grid & Snapping");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid/Snap toggles are in the row below.");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Grid Size", &m_GridSize, 0.05f, 0.05f, 50.0f, "%.2f");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Position Snap", &m_SnapTranslation, 0.05f, 0.01f, 50.0f, "%.2f");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Rotation Snap", &m_SnapRotationDeg, 1.0f, 180.0f, "%.1f°");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Scale Snap", &m_SnapScale, 0.01f, 0.01f, 5.0f, "%.2f");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Gizmo Size", &m_GizmoSize, 0.03f, 0.3f, "%.2f");

            ImGui::SeparatorText(ICON_FA_CIRCLE_DOT "  Vertex Snap");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Select a model, hold V near one of its\n"
                    "vertices (shown in yellow), click and\n"
                    "drag to move it — it snaps onto the\n"
                    "nearest vertex of any other model.\n"
                    "Release the click or V to drop it.\n"
                    "With a group selected, the rest of the\n"
                    "group rides along by the same offset.");
            }
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Snap Radius", &m_VertexSnapRadius, 0.1f, 5.0f, "%.2f");
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat("Pick Radius (px)", &m_VertexPickPixels, 5.0f, 150.0f, "%.0f");

            ImGui::SeparatorText(ICON_FA_KEYBOARD "  Controls");
            ImGui::TextDisabled(
                "Fly camera: WASD + right-drag to look\n"
                "Undo/Redo: Ctrl+Z / Ctrl+Y\n"
                "Save: Ctrl+S\n"
                "Gizmo mode: 1 / 2 / 3\n"
                "Vertex grab: hold V\n"
                "Multi-select: Ctrl+Click or drag a box\n"
                "Delete selection: Delete key\n"
                "Duplicate selection: Ctrl+D\n"
                "Focus selection: F\n"
                "Toggle fullscreen: F11\n"
                "Return to game / editor: F1");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    auto divider = []() { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); };

    // Icon-only + a tooltip carrying the full name/shortcut — keeps this row compact instead
    // of spelling every label out.
    auto iconButton = [](const char* icon, const char* tooltip, bool active = false) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        bool clicked = ImGui::Button(icon);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        return clicked;
    };

    if (iconButton(ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)")) Undo(world, assets);
    ImGui::SameLine();
    if (iconButton(ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)")) Redo(world, assets);

    divider();
    if (iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Translate (1)", m_GizmoOp == GizmoOp::Translate)) m_GizmoOp = GizmoOp::Translate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_ROTATE, "Rotate (2)", m_GizmoOp == GizmoOp::Rotate)) m_GizmoOp = GizmoOp::Rotate;
    ImGui::SameLine();
    if (iconButton(ICON_FA_EXPAND, "Scale (3)", m_GizmoOp == GizmoOp::Scale)) m_GizmoOp = GizmoOp::Scale;
    ImGui::SameLine();
    if (iconButton(m_GizmoLocalSpace ? ICON_FA_CUBE : ICON_FA_GLOBE,
            m_GizmoLocalSpace ? "Local space (click for World)" : "World space (click for Local)")) {
        m_GizmoLocalSpace = !m_GizmoLocalSpace;
    }

    divider();
    if (iconButton(ICON_FA_TABLE_CELLS, "Toggle Grid", m_ShowGrid)) m_ShowGrid = !m_ShowGrid;
    ImGui::SameLine();
    if (iconButton(ICON_FA_MAGNET, "Toggle Snap to Grid (hold Ctrl to invert while dragging)", m_GridSnapEnabled)) {
        m_GridSnapEnabled = !m_GridSnapEnabled;
    }

    divider();
    bool unlocked = !m_LayoutLocked;
    if (iconButton(unlocked ? ICON_FA_LOCK_OPEN : ICON_FA_LOCK,
            unlocked ? "Layout unlocked — click to lock panels in place" : "Layout locked — panels can still be resized; click to allow moving/rearranging too", unlocked)) {
        m_LayoutLocked = !m_LayoutLocked;
    }

    ImGui::End();
}

void EditorLayer::DrawHierarchy(World& world, AssetLibrary& assets) {
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = m_LayoutLocked
        ? (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)
        : ImGuiWindowFlags_None;
    if (!ImGui::Begin("Scene Hierarchy", nullptr, flags)) { ImGui::End(); return; }

    int aliveBoxes = 0;
    for (const auto& box : world.Boxes) if (box.Alive) aliveBoxes++;

    char geoHeader[32];
    snprintf(geoHeader, sizeof(geoHeader), "Level Geometry (%d)", aliveBoxes);
    if (ImGui::TreeNodeEx(geoHeader, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < (int)world.Boxes.size(); ++i) {
            if (!world.Boxes[i].Alive) continue;
            std::string label = ICON_FA_CUBE "  " + (world.Boxes[i].Name.empty() ? ("Box " + std::to_string(i)) : world.Boxes[i].Name);
            bool selected = IsSelected(false, i);
            if (ImGui::Selectable(label.c_str(), selected)) {
                SelectItem(false, i, ImGui::GetIO().KeyCtrl);
            }
        }
        ImGui::TreePop();
    }

    char modelHeader[32];
    snprintf(modelHeader, sizeof(modelHeader), "Models (%d)", (int)world.Models.size());
    if (ImGui::TreeNodeEx(modelHeader, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < (int)world.Models.size(); ++i) {
            bool selected = IsSelected(true, i);
            std::string label = ICON_FA_DRAW_POLYGON "  " + world.Models[i].Name;
            if (ImGui::Selectable(label.c_str(), selected)) {
                SelectItem(true, i, ImGui::GetIO().KeyCtrl);
            }

            // Drop a texture from the Asset Browser onto a model here to set it as that
            // model's Albedo map, creating a custom material override if it doesn't have one.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                    std::string texPath((const char*)payload->Data);
                    PushUndo(world);

                    Model* model = world.Models[i].ModelRef.get();
                    auto override_ = model->MaterialOverride();
                    if (!override_) {
                        override_ = std::make_shared<Material>();
                        if (model->MeshCount() > 0) {
                            const Material& imported = model->MeshMaterial(0);
                            override_->NormalMap = imported.NormalMap;
                            override_->MetallicRoughnessMap = imported.MetallicRoughnessMap;
                            override_->MetallicMap = imported.MetallicMap;
                            override_->RoughnessMap = imported.RoughnessMap;
                            override_->AOMap = imported.AOMap;
                            override_->EmissiveMap = imported.EmissiveMap;
                        }
                        model->SetMaterialOverride(override_);
                    }
                    override_->AlbedoMap = assets.LoadTexture(texPath);
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

void EditorLayer::DrawInspector(World& world, AssetLibrary& assets, float dt) {
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = m_LayoutLocked
        ? (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)
        : ImGuiWindowFlags_None;
    if (!ImGui::Begin("Inspector", nullptr, flags)) { ImGui::End(); return; }

    if (HasGroupSelection()) {
        int count = (m_SelectedBox >= 0 || m_SelectedModel >= 0 ? 1 : 0) + (int)m_ExtraSelection.size();
        ImGui::SeparatorText((std::to_string(count) + " objects selected").c_str());

        auto listOne = [&](bool isModel, int index) {
            if (isModel) {
                if (index >= 0 && index < (int)world.Models.size()) {
                    ImGui::BulletText("%s", world.Models[index].Name.c_str());
                }
            } else if (index >= 0 && index < (int)world.Boxes.size()) {
                const WorldBox& box = world.Boxes[index];
                if (box.Name.empty()) ImGui::BulletText("Box %d", index);
                else ImGui::BulletText("%s", box.Name.c_str());
            }
        };
        listOne(m_SelectedModel >= 0, m_SelectedModel >= 0 ? m_SelectedModel : m_SelectedBox);
        for (const auto& item : m_ExtraSelection) listOne(item.IsModel, item.Index);

        ImGui::TextDisabled("Move/rotate/scale together with the\nviewport gizmo. Ctrl+Click to add or\nremove objects from the selection.");
        ImGui::Spacing();
        if (ActionButton(ICON_FA_CLONE, "Duplicate (Ctrl+D)")) {
            DuplicateSelection(world, assets);
        }
        ImGui::SameLine();
        if (DeleteIconButton("Delete Selected")) {
            DeleteSelection(world);
        }
        ImGui::End();
        return;
    }

    if (m_SelectedBox >= 0 && m_SelectedBox < (int)world.Boxes.size()) {
        WorldBox& box = world.Boxes[m_SelectedBox];
        ImGui::SeparatorText((std::string("Box ") + std::to_string(m_SelectedBox)).c_str());

        bool activated;
        char defaultBoxName[32];
        snprintf(defaultBoxName, sizeof(defaultBoxName), "Box %d", m_SelectedBox);
        DrawNameField("Name", box.Name, defaultBoxName, activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Position", box.Center, 0.1f, 0.0f, 0.0f, activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Rotation", box.RotationEuler, 1.0f, 0.0f, 0.0f, activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Scale", box.Size, 0.1f, 0.1f, 100.0f, activated);
        if (activated) PushUndo(world);
        ImGui::ColorEdit3("Color", &box.Color.x, ImGuiColorEditFlags_DisplayHex);
        if (ImGui::IsItemActivated()) PushUndo(world);

        ImGui::Spacing();
        if (ActionButton(ICON_FA_DOWN_LONG, "Snap to Ground")) {
            PushUndo(world);
            glm::vec3 half = box.Size * 0.5f;
            glm::mat4 m = ComposeTransform(box.Center, box.RotationEuler, glm::vec3(1.0f));
            AABB worldBounds = AABB{-half, half}.Transformed(m);
            box.Center.y -= worldBounds.Min.y;
        }
        ImGui::SameLine();
        if (ActionButton(ICON_FA_CLONE, "Duplicate (Ctrl+D)")) {
            DuplicateSelection(world, assets);
        }
        ImGui::SameLine();
        if (DeleteIconButton("Delete Box")) {
            DeleteSelection(world);
        }
    } else if (m_SelectedModel >= 0 && m_SelectedModel < (int)world.Models.size()) {
        PlacedModel& pm = world.Models[m_SelectedModel];
        ImGui::SeparatorText(pm.Name.empty() ? "(unnamed)" : pm.Name.c_str());
        ImGui::TextDisabled("%s", pm.ModelRef->Path().c_str());
        ImGui::Spacing();

        bool activated;
        DrawNameField("Name", pm.Name, "Model", activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Position", pm.Position, 0.1f, 0.0f, 0.0f, activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Rotation", pm.RotationEuler, 1.0f, 0.0f, 0.0f, activated);
        if (activated) PushUndo(world);
        DrawVec3Row("Scale", pm.Scale, 0.05f, 0.01f, 100.0f, activated);
        if (activated) PushUndo(world);

        ImGui::Spacing();
        if (ActionButton(ICON_FA_DOWN_LONG, "Snap to Ground")) {
            PushUndo(world);
            glm::mat4 m = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
            pm.Position.y -= pm.ModelRef->LowestVertexWorldY(m);
        }
        ImGui::SameLine();
        if (ActionButton(ICON_FA_CLONE, "Duplicate (Ctrl+D)")) {
            DuplicateSelection(world, assets);
        }
        ImGui::SameLine();
        if (DeleteIconButton("Delete Model")) {
            DeleteSelection(world);
        }

        if (ImGui::CollapsingHeader(ICON_FA_FILM "  Animations")) {
            if (pm.ModelRef->HasAnimations()) {
                for (int i = 0; i < pm.ModelRef->AnimationCount(); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Button(ICON_FA_PLAY)) {
                        pm.ModelRef->PlayAnimation(i);
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(pm.ModelRef->AnimationName(i).c_str());
                    ImGui::PopID();
                }
                if (pm.ModelRef->IsPlayingAnimation() && ImGui::Button("Stop Animation")) {
                    pm.ModelRef->PlayAnimation(-1);
                }
            } else {
                ImGui::TextDisabled("No animations in this asset");
            }
        }

        if (ImGui::CollapsingHeader(ICON_FA_PALETTE "  PBR Material")) {
            DrawMaterialEditor(world, assets);
        }

        if (ImGui::CollapsingHeader(ICON_FA_VOLUME_HIGH "  Audio")) {
            const std::string preview = pm.SoundPath.empty() ? "(none)" : std::filesystem::path(pm.SoundPath).filename().string();
            if (ImGui::BeginCombo("Clip", preview.c_str())) {
                if (ImGui::Selectable("(none)", pm.SoundPath.empty())) { PushUndo(world); pm.SoundPath.clear(); }
                for (const auto& sound : assets.Sounds()) {
                    bool selected = (sound == pm.SoundPath);
                    if (ImGui::Selectable(std::filesystem::path(sound).filename().string().c_str(), selected)) {
                        PushUndo(world);
                        pm.SoundPath = sound;
                    }
                }
                ImGui::EndCombo();
            }
            if (!pm.SoundPath.empty() && ImGui::Button(ICON_FA_VOLUME_HIGH "  Play Sound")) {
                AudioEngine::Play(pm.SoundPath);
            }
        }
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Select something in the Scene Hierarchy");
    }

    ImGui::End();
}

void EditorLayer::DrawMaterialEditor(World& world, AssetLibrary& assets) {
    if (m_SelectedModel < 0 || m_SelectedModel >= (int)world.Models.size()) return;
    Model* model = world.Models[m_SelectedModel].ModelRef.get();

    bool useCustom = (bool)model->MaterialOverride();
    if (ImGui::Checkbox("Use Custom Material", &useCustom)) {
        PushUndo(world);
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
                mat->MetallicRoughnessMap = imported.MetallicRoughnessMap;
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

    auto mat = model->MaterialOverride();
    if (!mat) {
        ImGui::TextDisabled("Using material(s) imported from the source file.");
        return;
    }

    ImGui::ColorEdit3("Base Color", &mat->BaseColor.x, ImGuiColorEditFlags_DisplayHex);
    if (ImGui::IsItemActivated()) PushUndo(world);
    ImGui::SliderFloat("Metallic", &mat->Metallic, 0.0f, 1.0f);
    if (ImGui::IsItemActivated()) PushUndo(world);
    ImGui::SliderFloat("Roughness", &mat->Roughness, 0.04f, 1.0f);
    if (ImGui::IsItemActivated()) PushUndo(world);
    ImGui::ColorEdit3("Emissive Color", &mat->EmissiveColor.x, ImGuiColorEditFlags_DisplayHex);
    if (ImGui::IsItemActivated()) PushUndo(world);
    ImGui::SliderFloat("Emissive Strength", &mat->EmissiveStrength, 0.0f, 10.0f);
    if (ImGui::IsItemActivated()) PushUndo(world);

    ImGui::SeparatorText("Texture Maps");

    // One compact line per map: a thumbnail if it's set, the label, then icon-only
    // Import/Clear buttons — instead of a label+filename line followed by a full-width
    // button line each.
    auto mapRow = [&](const char* label, std::shared_ptr<Texture>& slot) {
        ImGui::PushID(label);
        if (slot) {
            ImGui::Image((ImTextureID)(intptr_t)slot->GLHandle(), ImVec2(20, 20));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", std::filesystem::path(slot->Path()).filename().string().c_str());
            }
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(label);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                std::string texPath((const char*)payload->Data);
                PushUndo(world);
                slot = assets.LoadTexture(texPath);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ActionButton(ICON_FA_FOLDER_OPEN, "Import...")) {
            std::string path = FileDialog::OpenFile(
                "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0", m_Window);
            if (!path.empty()) {
                PushUndo(world);
                slot = assets.LoadTexture(path);
            }
        }
        if (slot) {
            ImGui::SameLine();
            if (ActionButton(ICON_FA_XMARK, "Clear")) {
                PushUndo(world);
                slot = nullptr;
            }
        }
        ImGui::PopID();
    };

    mapRow("Albedo", mat->AlbedoMap);
    mapRow("Normal", mat->NormalMap);
    mapRow("Metallic-Roughness", mat->MetallicRoughnessMap);
    if (mat->MetallicRoughnessMap) {
        ImGui::TextDisabled("Packed (glTF-style) map above overrides the two below.");
    }
    mapRow("Metallic (standalone)", mat->MetallicMap);
    mapRow("Roughness (standalone)", mat->RoughnessMap);
    mapRow("AO", mat->AOMap);
    mapRow("Emissive", mat->EmissiveMap);
}

void EditorLayer::DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera) {
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    if (!peek || !peek->IsDataType("ASSET_MODEL_PATH")) return; // only active during a model drag

    int w, h;
    glfwGetWindowSize(m_Window, &w, &h);
    if (w <= 0 || h <= 0 || m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##ViewportDropTarget", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::InvisibleButton("##viewport_drop_zone", ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
            std::string path((const char*)payload->Data);
            PushUndo(world);

            // Drop where the cursor is pointing: raycast from the camera through the mouse,
            // landing on the nearest existing box or the ground plane, whichever is closer.
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

            float bestT = 1e30f;
            float boxDist;
            if (world.Raycast(origin, dir, 500.0f, boxDist) >= 0) {
                bestT = boxDist;
            }
            if (std::abs(dir.y) > 1e-5f) {
                float groundT = -origin.y / dir.y;
                if (groundT > 0.0f && groundT < bestT) bestT = groundT;
            }
            if (bestT >= 1e30f) bestT = 8.0f; // pointing at the sky / parallel to the ground

            auto model = assets.LoadModel(path);
            PlacedModel pm;
            pm.ModelRef = model;
            pm.Name = std::filesystem::path(path).stem().string();
            pm.Position = origin + dir * bestT;
            world.Models.push_back(pm);
            SelectItem(true, (int)world.Models.size() - 1, false);
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
        m_BoxSelectActive = !io.WantCaptureMouse && !m_GizmoEngaged; // ImGui panel or the gizmo itself already owns this click
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
        int bestBox = -1, bestModel = -1;
        for (int i = 0; i < (int)world.Boxes.size(); ++i) {
            if (!world.Boxes[i].Alive) continue;
            float t;
            if (world.Boxes[i].Bounds().RayIntersect(origin, dir, t) && t < bestT) {
                bestT = t; bestBox = i; bestModel = -1;
            }
        }
        for (int i = 0; i < (int)world.Models.size(); ++i) {
            PlacedModel& pm = world.Models[i];
            glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
            AABB worldBounds = AABB{pm.ModelRef->BoundsMin(), pm.ModelRef->BoundsMax()}.Transformed(model);
            float t;
            if (worldBounds.RayIntersect(origin, dir, t) && t < bestT) {
                bestT = t; bestModel = i; bestBox = -1;
            }
        }

        if (bestBox >= 0) SelectItem(false, bestBox, io.KeyCtrl);
        else if (bestModel >= 0) SelectItem(true, bestModel, io.KeyCtrl);
        else if (!io.KeyCtrl) ClearSelection(); // clicked empty space -> deselect (unless Ctrl-clicking to preserve a group)
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

    for (int i = 0; i < (int)world.Boxes.size(); ++i) {
        if (!world.Boxes[i].Alive) continue;
        glm::vec2 pMin, pMax;
        if (projectedScreenRect(world.Boxes[i].Bounds(), pMin, pMax) && rectsOverlap(pMin, pMax, rectMin, rectMax)) {
            AddToSelectionIfAbsent(false, i);
        }
    }
    for (int i = 0; i < (int)world.Models.size(); ++i) {
        PlacedModel& pm = world.Models[i];
        glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
        AABB bounds = AABB{pm.ModelRef->BoundsMin(), pm.ModelRef->BoundsMax()}.Transformed(model);
        glm::vec2 pMin, pMax;
        if (projectedScreenRect(bounds, pMin, pMax) && rectsOverlap(pMin, pMax, rectMin, rectMax)) {
            AddToSelectionIfAbsent(true, i);
        }
    }
}

bool EditorLayer::FindVertexUnderCursor(World& world, Camera& editorCamera, glm::vec3& outLocalPos) const {
    if (m_SelectedModel < 0 || m_SelectedModel >= (int)world.Models.size()) return false;
    if (m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return false;

    const PlacedModel& pm = world.Models[m_SelectedModel];
    glm::mat4 model = ComposeTransform(pm.Position, pm.RotationEuler, pm.Scale);
    glm::mat4 viewProj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y) * editorCamera.ViewMatrix();

    ImVec2 mouse = ImGui::GetIO().MousePos;
    glm::vec2 viewportMouse(mouse.x - m_ViewportPos.x, mouse.y - m_ViewportPos.y);
    return pm.ModelRef->FindNearestVertexToScreenPoint(model, viewProj, {viewportMouse.x, viewportMouse.y},
        m_ViewportSize.x, m_ViewportSize.y, m_VertexPickPixels, outLocalPos);
}

void EditorLayer::UpdateVertexDrag(World& world, Camera& editorCamera) {
    if (m_SelectedModel < 0 || m_SelectedModel >= (int)world.Models.size()) {
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

    // Vertex snap: search every model OUTSIDE the current selection for the nearest vertex
    // within radius (other group members are excluded — they're moving rigidly along with
    // the grabbed vertex, so their relative distance to it never actually changes, and
    // "snapping" onto one would just lock the group to itself) and, if one is close enough,
    // nudge the object so the grabbed vertex lands exactly on it.
    auto inSelection = [&](int modelIndex) {
        if (modelIndex == m_SelectedModel) return true;
        for (const auto& item : m_ExtraSelection) {
            if (item.IsModel && item.Index == modelIndex) return true;
        }
        return false;
    };

    float bestDist = m_VertexSnapRadius;
    glm::vec3 bestTarget{};
    bool found = false;
    for (int i = 0; i < (int)world.Models.size(); ++i) {
        if (inSelection(i)) continue;
        PlacedModel& other = world.Models[i];
        glm::mat4 otherMatrix = ComposeTransform(other.Position, other.RotationEuler, other.Scale);
        glm::vec3 candidateVertex;
        if (other.ModelRef->FindNearestVertexWorld(otherMatrix, grabbedWorld, bestDist, candidateVertex)) {
            bestTarget = candidateVertex;
            bestDist = glm::length(candidateVertex - grabbedWorld);
            found = true;
        }
    }
    if (found) {
        candidatePosition += (bestTarget - grabbedWorld);
    }

    // Move the primary by however much this frame actually resolved to (drag + snap), then
    // carry every other selected object along by that exact same delta so the whole group
    // moves together while only the primary's vertex does the snapping.
    glm::vec3 delta = candidatePosition - world.Models[m_SelectedModel].Position;
    world.Models[m_SelectedModel].Position = candidatePosition;

    for (const auto& item : m_ExtraSelection) {
        if (item.IsModel) {
            if (item.Index >= 0 && item.Index < (int)world.Models.size()) {
                world.Models[item.Index].Position += delta;
            }
        } else if (item.Index >= 0 && item.Index < (int)world.Boxes.size()) {
            world.Boxes[item.Index].Center += delta;
        }
    }
}

void EditorLayer::DrawGizmo(World& world, Camera& editorCamera) {
    if (HasGroupSelection()) {
        DrawGroupGizmo(world, editorCamera);
        return;
    }

    if (m_SelectedBox < 0 && m_SelectedModel < 0) {
        m_GizmoEngaged = false;
        return;
    }

    glm::vec3* pos = nullptr;
    glm::vec3* rot = nullptr;
    glm::vec3* scale = nullptr;

    if (m_SelectedBox >= 0 && m_SelectedBox < (int)world.Boxes.size()) {
        WorldBox& box = world.Boxes[m_SelectedBox];
        pos = &box.Center; rot = &box.RotationEuler; scale = &box.Size;
    } else if (m_SelectedModel >= 0 && m_SelectedModel < (int)world.Models.size()) {
        PlacedModel& pm = world.Models[m_SelectedModel];
        pos = &pm.Position; rot = &pm.RotationEuler; scale = &pm.Scale;
    }
    if (!pos) return;

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

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
    ImGuizmo::SetGizmoSizeClipSpace(m_GizmoSize);
    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (m_GizmoOp == GizmoOp::Rotate) op = ImGuizmo::ROTATE;
    else if (m_GizmoOp == GizmoOp::Scale) op = ImGuizmo::SCALE;

    float t[3] = {pos->x, pos->y, pos->z};
    float r[3] = {rot->x, rot->y, rot->z};
    float s[3] = {scale->x, scale->y, scale->z};
    glm::mat4 matrix;
    ImGuizmo::RecomposeMatrixFromComponents(t, r, s, glm::value_ptr(matrix));

    // Ctrl held inverts the checkbox for the duration of the drag — matches Blender's
    // momentary-snap convention while still giving snapping a persistent on/off default.
    bool snapActive = m_GridSnapEnabled != ImGui::GetIO().KeyCtrl;
    float snapValues[3] = {m_SnapTranslation, m_SnapTranslation, m_SnapTranslation};
    if (m_GizmoOp == GizmoOp::Rotate) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapRotationDeg;
    else if (m_GizmoOp == GizmoOp::Scale) snapValues[0] = snapValues[1] = snapValues[2] = m_SnapScale;

    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, gizmoMode,
        glm::value_ptr(matrix), nullptr, snapActive ? snapValues : nullptr);
    m_GizmoEngaged = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    bool isUsingNow = ImGuizmo::IsUsing();
    if (isUsingNow && !m_GizmoWasUsing) {
        // Drag just started this frame: snapshot the still-unmodified transform (pos/rot/scale
        // below haven't been written yet) so undo restores to exactly where the drag began.
        PushUndo(world);
    }
    m_GizmoWasUsing = isUsingNow;

    if (isUsingNow) {
        float nt[3], nr[3], ns[3];
        ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(matrix), nt, nr, ns);
        *pos = {nt[0], nt[1], nt[2]};
        *rot = {nr[0], nr[1], nr[2]};
        *scale = {ns[0], ns[1], ns[2]};
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EditorLayer::DrawGroupGizmo(World& world, Camera& editorCamera) {
    struct Ref { glm::vec3* pos; glm::vec3* rot; glm::vec3* scale; };
    std::vector<Ref> refs;
    auto addRef = [&](bool isModel, int index) {
        if (isModel) {
            if (index >= 0 && index < (int)world.Models.size()) {
                PlacedModel& pm = world.Models[index];
                refs.push_back({&pm.Position, &pm.RotationEuler, &pm.Scale});
            }
        } else if (index >= 0 && index < (int)world.Boxes.size()) {
            WorldBox& box = world.Boxes[index];
            refs.push_back({&box.Center, &box.RotationEuler, &box.Size});
        }
    };
    addRef(m_SelectedModel >= 0, m_SelectedModel >= 0 ? m_SelectedModel : m_SelectedBox);
    for (const auto& item : m_ExtraSelection) addRef(item.IsModel, item.Index);
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

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
    ImGuizmo::SetGizmoSizeClipSpace(m_GizmoSize);
    ImGuizmo::MODE gizmoMode = m_GizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    glm::mat4 view = editorCamera.ViewMatrix();
    glm::mat4 proj = editorCamera.ProjectionMatrix(m_ViewportSize.x / m_ViewportSize.y);

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
        PushUndo(world);
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
            *r.rot = {nr[0], nr[1], nr[2]};
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

void EditorLayer::CommitRename(AssetLibrary& assets) {
    std::string newName = m_RenameBuffer;
    if (!newName.empty()) {
        if (m_RenamingIsFolder) {
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
            assets.SetDisplayName(m_RenamingAssetKey, newName);
        }
    }
    m_RenamingAssetKey.clear();
}

void EditorLayer::DrawAssetBrowser(World& world, AssetLibrary& assets) {
    // Locked only blocks dragging the tab to move/undock/rearrange the panel — resizing its
    // dock node (and the neighbors that share that border) always works, locked or not.
    ImGuiWindowFlags flags = m_LayoutLocked
        ? (ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)
        : ImGuiWindowFlags_None;
    // Tighter vertical padding than the default so the toolbar hugs the tab bar instead of
    // floating below a large gap.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    bool open = ImGui::Begin("Asset Browser", nullptr, flags);
    ImGui::PopStyleVar();
    if (!open) { ImGui::End(); return; }

    auto makeNewFolder = [&]() {
        std::string base = m_CurrentAssetFolder.empty() ? "New Folder" : (m_CurrentAssetFolder + "/New Folder");
        std::string candidate = base;
        int n = 1;
        auto exists = [&](const std::string& p) {
            for (const auto& f : assets.Folders()) if (f == p) return true;
            return false;
        };
        while (exists(candidate)) candidate = base + " (" + std::to_string(n++) + ")";
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

    if (ActionButton(ICON_FA_FOLDER_PLUS, "New Folder")) makeNewFolder();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Breadcrumb: "Assets" root plus one clickable button per path segment.
    ImGui::AlignTextToFramePadding();
    if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN " Assets")) m_CurrentAssetFolder.clear();
    if (!m_CurrentAssetFolder.empty()) {
        std::string accum;
        size_t start = 0;
        while (start <= m_CurrentAssetFolder.size()) {
            size_t slash = m_CurrentAssetFolder.find('/', start);
            std::string part = m_CurrentAssetFolder.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            accum = accum.empty() ? part : accum + "/" + part;
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
            if (ImGui::SmallButton(part.c_str())) m_CurrentAssetFolder = accum;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }

    // Search box pinned to the toolbar's right edge, falling back to a new line only if the
    // breadcrumb has grown too long to leave room for it.
    float targetX = ImGui::GetWindowContentRegionMax().x - searchWidth;
    if (targetX > ImGui::GetCursorPosX()) ImGui::SameLine(targetX);
    else ImGui::NewLine();

    char filterBuf[64];
    snprintf(filterBuf, sizeof(filterBuf), "%s", m_AssetSearchFilter.c_str());
    ImGui::SetNextItemWidth(searchWidth);
    if (ImGui::InputTextWithHint("##AssetFilter", ICON_FA_MAGNIFYING_GLASS "  Search...", filterBuf, sizeof(filterBuf))) {
        m_AssetSearchFilter = filterBuf;
    }

    ImGui::EndChild(); // ##AssetToolbar

    ImGui::Separator();
    ImGui::BeginChild("##AssetList", ImVec2(0, 0), ImGuiChildFlags_Borders);

    bool searching = !m_AssetSearchFilter.empty();

    struct Cell {
        enum class Kind { Folder, Model, Texture, Sound, Scene } kind;
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
    if (searching || m_CurrentAssetFolder == kScenesFolder) {
        std::error_code ec;
        std::filesystem::create_directory("scenes", ec);
        for (const auto& entry : std::filesystem::directory_iterator("scenes", ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::string path = entry.path().generic_string();
            std::string name = entry.path().stem().string();
            if (!MatchesFilter(m_AssetSearchFilter, name)) continue;
            cells.push_back({Cell::Kind::Scene, path, name, nullptr, nullptr});
        }
    }

    for (const auto& folder : assets.Folders()) {
        bool show = searching ? MatchesFilter(m_AssetSearchFilter, LeafNameOf(folder)) : (ParentFolderOf(folder) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Folder, folder, LeafNameOf(folder), nullptr, nullptr});
    }
    for (const auto& model : assets.Models()) {
        std::string path = model->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesFilter(m_AssetSearchFilter, name)) continue;
        bool show = searching || assets.AssetFolder(path) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Model, path, name, model, nullptr});
    }
    for (const auto& tex : assets.Textures()) {
        std::string path = tex->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesFilter(m_AssetSearchFilter, name)) continue;
        bool show = searching || assets.AssetFolder(path) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Texture, path, name, nullptr, tex});
    }
    for (const auto& sound : assets.Sounds()) {
        std::string name = assets.DisplayName(sound);
        if (!MatchesFilter(m_AssetSearchFilter, name)) continue;
        bool show = searching || assets.AssetFolder(sound) == m_CurrentAssetFolder;
        if (show) cells.push_back({Cell::Kind::Sound, sound, name, nullptr, nullptr});
    }
    std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
        bool aFolder = a.kind == Cell::Kind::Folder, bFolder = b.kind == Cell::Kind::Folder;
        if (aFolder != bFolder) return aFolder;
        return a.display < b.display;
    });

    const float iconSize = ImGui::GetTextLineHeight();

    if (!searching && !m_CurrentAssetFolder.empty()) {
        if (ImGui::Selectable(ICON_FA_ARROW_UP "  ..")) {
            m_CurrentAssetFolder = ParentFolderOf(m_CurrentAssetFolder);
        }
    }

    for (const auto& cell : cells) {
        ImGui::PushID(cell.key.c_str());

        bool isFolder = cell.kind == Cell::Kind::Folder;
        bool isSelected = m_SelectedAssetKey == cell.key && m_SelectedAssetIsFolder == isFolder;
        bool isRenaming = m_RenamingAssetKey == cell.key && m_RenamingIsFolder == isFolder;
        bool playing = cell.kind == Cell::Kind::Sound && AudioEngine::IsPreviewPlaying(cell.key);

        // Little icon (real thumbnail for textures, a Font Awesome glyph otherwise) followed
        // by the name, mirroring how the Scene Hierarchy lists its rows — no button box.
        if (cell.kind == Cell::Kind::Texture) {
            ImGui::Image((ImTextureID)(intptr_t)cell.texture->GLHandle(), ImVec2(iconSize, iconSize));
        } else {
            const char* icon = isFolder ? ICON_FA_FOLDER
                : cell.kind == Cell::Kind::Model ? (cell.model->HasAnimations() ? ICON_FA_FILM : ICON_FA_CUBE)
                : cell.kind == Cell::Kind::Scene ? ICON_FA_MAP
                : (playing ? ICON_FA_STOP : ICON_FA_MUSIC);
            ImGui::TextUnformatted(icon);
        }
        ImGui::SameLine();

        bool clicked = false;
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
            if (done) CommitRename(assets);
            else if (cancel || lostFocus) m_RenamingAssetKey.clear();
        } else {
            clicked = ImGui::Selectable(cell.display.c_str(), isSelected);
        }

        if (clicked) {
            m_SelectedAssetKey = cell.key;
            m_SelectedAssetIsFolder = isFolder;
            if (cell.kind == Cell::Kind::Sound) {
                if (playing) AudioEngine::StopPreview();
                else AudioEngine::PlayPreview(cell.key);
            }
        }
        if (isFolder && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_CurrentAssetFolder = cell.key;
        }
        if (cell.kind == Cell::Kind::Scene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenScene(world, assets, cell.key);
        }

        if (isFolder) {
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) assets.SetAssetFolder((const char*)p->Data, cell.key);
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) assets.SetAssetFolder((const char*)p->Data, cell.key);
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SOUND_PATH")) assets.SetAssetFolder((const char*)p->Data, cell.key);
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                    std::string src((const char*)p->Data);
                    if (src != cell.key && cell.key.rfind(src + "/", 0) != 0) {
                        assets.RenameFolder(src, cell.key + "/" + LeafNameOf(src));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        } else if (cell.kind != Cell::Kind::Scene) { // scenes aren't placeable — nothing to drag into the viewport
            const char* payloadType = cell.kind == Cell::Kind::Model ? "ASSET_MODEL_PATH"
                : cell.kind == Cell::Kind::Texture ? "ASSET_TEXTURE_PATH" : "ASSET_SOUND_PATH";
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(payloadType, cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
        }

        if (!isRenaming && ImGui::IsItemHovered()) {
            if (cell.kind == Cell::Kind::Model) {
                ImGui::SetTooltip("%s\n\nDrag into the viewport to place\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindModelUsages(world, cell.model.get())).c_str());
            } else if (cell.kind == Cell::Kind::Texture) {
                ImGui::SetTooltip(
                    "%s\n\nDrag onto a model to set its Albedo map,\nor onto a map row in the Inspector's PBR Material.\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindTextureUsages(world, cell.texture.get())).c_str());
            } else if (cell.kind == Cell::Kind::Sound) {
                ImGui::SetTooltip("%s\n\nClick to preview\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindSoundUsages(world, cell.key)).c_str());
            } else if (cell.kind == Cell::Kind::Scene) {
                ImGui::SetTooltip("%s\n\nDouble-click to open", cell.display.c_str());
            } else {
                ImGui::SetTooltip("%s", cell.display.c_str());
            }
        }

        if (cell.kind == Cell::Kind::Scene) {
            // Scenes aren't AssetLibrary entries (no rename/remove-from-library — they're
            // real files on disk), so they get their own, much shorter context menu.
            if (ImGui::BeginPopupContextItem()) {
                m_SelectedAssetKey = cell.key;
                m_SelectedAssetIsFolder = false;
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open")) OpenScene(world, assets, cell.key);
                ImGui::EndPopup();
            }
        } else if (ImGui::BeginPopupContextItem()) {
            m_SelectedAssetKey = cell.key;
            m_SelectedAssetIsFolder = isFolder;
            if (ImGui::MenuItem(ICON_FA_PEN "  Rename (F2)")) {
                BeginRenameAsset(cell.key, isFolder, cell.display);
            }
            if (isFolder) {
                bool canDelete = assets.CanDeleteFolder(cell.key);
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Folder", nullptr, false, canDelete)) {
                    assets.DeleteFolder(cell.key);
                }
                if (!canDelete) ImGui::TextDisabled("Must be empty to delete");
            } else {
                if (ImGui::MenuItem(ICON_FA_TRASH "  Remove from Library")) {
                    if (cell.kind == Cell::Kind::Model) assets.RemoveModel(cell.model);
                    else if (cell.kind == Cell::Kind::Texture) assets.RemoveTexture(cell.texture);
                    else assets.RemoveSound(cell.key);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_SelectedAssetKey.clear();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##BrowserBgContext");
    }
    if (ImGui::BeginPopup("##BrowserBgContext")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) makeNewFolder();
        ImGui::EndPopup();
    }

    ImGui::EndChild(); // ##AssetList

    ImGui::End();
}
