// Animator Controllers in the editor.
//
//  - The Animator Controller component's Inspector section (DrawAnimatorControllerExtra): pick or
//    create a .controller, open it in the Animator window, and watch / poke its parameters live.
//  - The Animator window (DrawAnimatorWindow): Unity's node-graph editor. Layers and parameters on
//    the left, the layer's state machine as a pan/zoom graph in the middle (Entry / Any State /
//    Exit nodes, transitions as arrows), and the selection's properties on the right. Every edit
//    is saved straight to the file (a running Play picks it up within a second) and goes on the
//    window's own undo stack (Ctrl+Z / Ctrl+Y while the window is focused).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "CurveEditor.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "FirstPersonAnimation.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

using namespace EditorInternal;
namespace fs = std::filesystem;
using AC = AnimatorController;

namespace {

// std::string-backed InputText without imgui_stdlib. True once the edit is committed.
bool InputName(const char* id, std::string& s, float width, bool allowEmpty = false) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit() && s != buf && (allowEmpty || buf[0] != '\0')) { s = buf; return true; }
    return false;
}

bool RemoveButton(const char* tooltip) {
    return ActionButton(ICON_FA_XMARK, tooltip, false, ImVec2(ImGui::GetFrameHeight(), 0.0f));
}

std::string UniqueName(const std::string& base, const std::vector<std::string>& taken) {
    std::string name = base;
    for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n)
        name = base + " " + std::to_string(n);
    return name;
}

// Every clip this model can play, as (reference, label): its own, then compatible clips of the
// project's other model files (attached on first use, like the Animation component's picker).
std::vector<std::pair<std::string, std::string>> ClipChoices(Model& model, AssetLibrary& assets) {
    std::vector<std::pair<std::string, std::string>> out;
    for (int i = 0; i < model.OwnAnimationCount(); ++i) out.push_back({model.AnimationName(i), model.AnimationName(i)});
    for (const auto& src : assets.Models()) {
        if (!src || src->Path() == model.Path() || src->OwnAnimationCount() == 0) continue;
        for (int j = 0; j < src->OwnAnimationCount(); ++j) {
            const std::string ref = AnimationClipRef(*src, j);
            if (ResolveAnimationClip(model, ref, assets) >= 0) out.push_back({ref, AnimationClipLabel(*src, j)});
        }
    }
    return out;
}

// Without a rig to test against, every animation clip the project has loaded.
std::vector<std::pair<std::string, std::string>> AllClipChoices(AssetLibrary& assets) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& src : assets.Models()) {
        if (!src || src->OwnAnimationCount() == 0) continue;
        for (int j = 0; j < src->OwnAnimationCount(); ++j) out.push_back({AnimationClipRef(*src, j), AnimationClipLabel(*src, j)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

std::string ClipLabel(const std::string& ref) {
    if (ref.empty()) return "(none)";
    const size_t hash = ref.find('#');
    std::string label = fs::u8path(ref.substr(0, hash)).stem().u8string();
    if (hash != std::string::npos) label += " / " + ref.substr(hash + 1);
    return label.empty() ? ref : label;
}

const char* kParamTypeLabels = "Float\0Int\0Bool\0Trigger\0";
const char* kOpLabels = "Greater\0Less\0Equals\0Not Equal\0Is True\0Is False\0";

// Live parameter widgets (Inspector section and the Animator window's Parameters tab).
void LiveParamWidget(AnimatorParam& p, float width) {
    ImGui::PushID(p.Name.c_str());
    ImGui::SetNextItemWidth(width);
    switch (p.Type) {
        case 1: { int v = (int)p.Value; if (ImGui::DragInt(p.Name.c_str(), &v)) p.Value = (float)v; break; }
        case 2: { bool v = p.Value > 0.5f; if (EditorUIPrimitives::Checkbox(p.Name.c_str(), &v)) p.Value = v ? 1.0f : 0.0f; break; }
        case 3: {
            if (ActionButton(ICON_FA_BOLT, "Set this trigger", p.Value > 0.5f, ImVec2(ImGui::GetFrameHeight(), 0.0f))) p.Value = 1.0f;
            ImGui::SameLine();
            ImGui::TextUnformatted(p.Name.c_str());
            break;
        }
        default: ImGui::DragFloat(p.Name.c_str(), &p.Value, 0.05f); break;
    }
    ImGui::PopID();
}

// --- graph geometry --------------------------------------------------------------------------

constexpr float kStateW = 180.0f, kStateH = 42.0f;
constexpr float kSpecialW = 130.0f, kSpecialH = 34.0f;

enum class NodeKind { State, Entry, Any, Exit };
struct NodeRef {
    NodeKind Kind = NodeKind::State;
    int Index = -1;
    bool operator==(const NodeRef& o) const { return Kind == o.Kind && Index == o.Index; }
    bool operator<(const NodeRef& o) const { return Kind != o.Kind ? Kind < o.Kind : Index < o.Index; }
};

float Len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const ImVec2 ab = b - a;
    const float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return Len(p - (a + ab * t));
}

// Where the segment from the rect's centre toward `toward` leaves the rect.
ImVec2 RectExit(ImVec2 center, ImVec2 half, ImVec2 toward) {
    const ImVec2 d = toward - center;
    if (std::abs(d.x) < 1e-3f && std::abs(d.y) < 1e-3f) return center;
    const float tx = std::abs(d.x) > 1e-3f ? half.x / std::abs(d.x) : 1e9f;
    const float ty = std::abs(d.y) > 1e-3f ? half.y / std::abs(d.y) : 1e9f;
    const float t = std::min(1.0f, std::min(tx, ty));
    return center + d * t;
}

void Arrowhead(ImDrawList* dl, ImVec2 at, ImVec2 dir, float size, ImU32 col) {
    const float l = Len(dir);
    if (l < 1e-3f) return;
    dir = dir * (1.0f / l);
    const ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(at + dir * size, at - dir * size * 0.6f + n * size * 0.7f,
                          at - dir * size * 0.6f - n * size * 0.7f, col);
}

} // namespace

// --- window state -----------------------------------------------------------------------------

struct EditorLayer::AnimatorWindowState {
    std::string Rel, Abs;
    fs::file_time_type Stamp{};
    double LastStat = 0.0;
    AC Doc;
    bool Loaded = false;
    std::string Error;
    std::string Saved; // the document as last saved: the undo baseline
    std::vector<std::string> Undo, Redo;

    entt::entity Entity = entt::null; // rig for clip pickers / live view
    int Layer = 0;
    int LeftTab = 0;

    // Selection. States can be multi-selected; everything else is single.
    NodeKind SelKind = NodeKind::State;
    std::vector<int> SelStates;
    bool SelSpecial = false;     // Entry / Any / Exit selected (SelKind says which)
    int SelTransition = -1;

    ImVec2 Pan{0.0f, 0.0f};
    float Zoom = 1.0f;
    bool FramePending = true;

    bool Linking = false;
    NodeRef LinkFrom;
    bool BoxSelecting = false;
    ImVec2 BoxStart{};
    bool MovingNodes = false;
    NodeRef ContextNode;
    int ContextTransition = -1;
    glm::vec2 ContextWorld{0.0f};

    std::vector<std::pair<std::string, std::string>> Clips;
    const void* ClipsSource = nullptr;
    int ClipsModelCount = -1;

    AC::Layer& L() { return Doc.Layers[std::clamp(Layer, 0, (int)Doc.Layers.size() - 1)]; }
    void ClearSelection() { SelStates.clear(); SelSpecial = false; SelTransition = -1; }
    bool IsSelected(int s) const { return std::find(SelStates.begin(), SelStates.end(), s) != SelStates.end(); }

    bool Load(const std::string& rel) {
        Rel = rel;
        Abs = ProjectPaths::Resolve(rel);
        AC doc;
        std::string err;
        if (!AC::LoadFile(Abs, doc, &err)) { Loaded = false; Error = err; return false; }
        Doc = std::move(doc);
        Loaded = true;
        Error.clear();
        Saved = Doc.ToJsonString();
        std::error_code ec;
        Stamp = fs::last_write_time(fs::u8path(Abs), ec);
        Layer = std::clamp(Layer, 0, (int)Doc.Layers.size() - 1);
        ClearSelection();
        return true;
    }
    void Write() {
        if (!Doc.SaveFile(Abs)) { Log::Error("Animator: couldn't save " + Rel + "."); return; }
        std::error_code ec;
        Stamp = fs::last_write_time(fs::u8path(Abs), ec);
    }
    // Records the edit on the undo stack and saves. Call after any change is complete.
    void Commit() {
        std::string now = Doc.ToJsonString();
        if (now == Saved) return;
        Undo.push_back(std::move(Saved));
        if (Undo.size() > 200) Undo.erase(Undo.begin());
        Redo.clear();
        Saved = std::move(now);
        Write();
    }
    void Step(std::vector<std::string>& from, std::vector<std::string>& to) {
        if (from.empty()) return;
        to.push_back(Saved);
        Saved = std::move(from.back());
        from.pop_back();
        AC::FromJsonString(Saved, Doc);
        Layer = std::clamp(Layer, 0, (int)Doc.Layers.size() - 1);
        ClearSelection();
        Write();
    }
};

void EditorLayer::OpenAnimatorWindow(const std::string& controllerRel, entt::entity entity) {
    if (!m_AnimatorWin) m_AnimatorWin = std::make_shared<AnimatorWindowState>();
    auto& w = *m_AnimatorWin;
    if (w.Rel != controllerRel || !w.Loaded) {
        w.Undo.clear();
        w.Redo.clear();
        w.Layer = 0;
        w.FramePending = true;
        w.Load(controllerRel);
    }
    if (entity != entt::null) w.Entity = entity;
    m_ShowAnimator = true;
    ImGui::SetWindowFocus(ICON_FA_DIAGRAM_PROJECT "  Animator");
}

// --- Inspector section ------------------------------------------------------------------------

void EditorLayer::DrawAnimatorControllerExtra(World& world, entt::entity entity) {
    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(entity);
    if (!ac || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;
    const auto* rc = world.Registry.try_get<RenderableComponent>(entity);
    Model* model = rc ? rc->ModelRef.get() : nullptr;
    if (!model || model->NodeCount() == 0)
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                           "Needs a Mesh Renderer with a rigged model.");

    // --- Controller file picker + New + Open ---------------------------------------------------
    ImGui::TextUnformatted("Controller");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    const float btnW = ImGui::CalcTextSize(ICON_FA_PLUS " New").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##ctrlpick", ac->Controller.empty() ? "(none)" : ac->Controller.c_str())) {
        if (ImGui::Selectable("(none)", ac->Controller.empty())) { PushUndo(world, "Set Animator Controller"); ac->Controller.clear(); }
        for (const std::string& path : FindAnimatorControllers())
            if (ImGui::Selectable(path.c_str(), path == ac->Controller)) {
                PushUndo(world, "Set Animator Controller");
                ac->Controller = path;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The .controller file this object runs (any under the project folder).");
    ImGui::SameLine();
    if (ActionButton(ICON_FA_PLUS " New", "Create a controller for this object: one state per clip it can play, the first as the default")) {
        AC c;
        if (model) {
            std::vector<std::string> names;
            int i = 0;
            for (const auto& [ref, label] : ClipChoices(*model, assets)) {
                AC::State s;
                std::string nm = label;
                std::replace(nm.begin(), nm.end(), '/', '-');
                s.Name = UniqueName(nm, names);
                s.Motions.resize(c.Tracks.size());
                s.Motions[0].Clip = ref;
                s.Position = {(float)(i % 4) * 220.0f, (float)(i / 4) * 110.0f};
                ++i;
                names.push_back(s.Name);
                c.Layers[0].States.push_back(std::move(s));
            }
        }
        if (c.Layers[0].States.empty()) {
            AC::State s;
            s.Name = "Idle";
            s.Motions.resize(c.Tracks.size());
            c.Layers[0].States.push_back(std::move(s));
        }
        c.Layers[0].DefaultState = c.Layers[0].States.front().Name;
        const auto* nc = world.Registry.try_get<NameComponent>(entity);
        std::string base = nc && !nc->Name.empty() ? nc->Name : std::string("Animator");
        for (char& ch : base) if (std::strchr("<>:\"/\\|?*", ch)) ch = '_';
        std::error_code ec;
        fs::create_directories(fs::u8path(ProjectPaths::Resolve("animators")), ec);
        std::string rel = "animators/" + base + ".controller";
        for (int n = 2; fs::exists(fs::u8path(ProjectPaths::Resolve(rel)), ec); ++n)
            rel = "animators/" + base + " " + std::to_string(n) + ".controller";
        if (c.SaveFile(ProjectPaths::Resolve(rel))) {
            PushUndo(world, "New Animator Controller");
            ac->Controller = rel;
            InvalidateAnimationListing();
            Log::Info("Created Animator Controller " + rel + " with " + std::to_string(c.Layers[0].States.size()) + " state(s).");
            OpenAnimatorWindow(rel, entity);
        } else {
            Log::Error("Couldn't write " + rel + ".");
        }
    }
    if (ac->Controller.empty()) return;

    if (ActionButton(ICON_FA_DIAGRAM_PROJECT "  Open Animator", "Edit this controller's states, transitions, layers and parameters in the Animator window",
                     false, ImVec2(-FLT_MIN, 0.0f)))
        OpenAnimatorWindow(ac->Controller, entity);

    const auto ctrl = GetAnimatorController(ac->Controller);
    if (!ctrl) {
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", "The controller file is missing or unreadable.");
        return;
    }
    int states = 0, transitions = 0;
    for (const auto& L : ctrl->Layers) { states += (int)L.States.size(); transitions += (int)L.Transitions.size(); }
    ImGui::TextDisabled("%d layer(s), %d state(s), %d transition(s), %d parameter(s)", (int)ctrl->Layers.size(), states,
                        transitions, (int)ctrl->Parameters.size());

    // --- Live view while playing: current state + parameters you can poke ---------------------
    if (m_InPlayMode && ac->Started) {
        ImGui::SeparatorText("Live");
        ImGui::Text("State: %s", ac->StateName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f%%%s)", 100.0f * std::fmod(ac->StateTime, 1.0f), ac->InTransition ? ", blending" : "");
        for (auto& p : ac->Params) LiveParamWidget(p, ImGui::GetContentRegionAvail().x * 0.5f);
    }
}

void EditorLayer::DrawRootMotionExtra(World& world, entt::entity entity, RootMotionOptions& opts, Model* model) {
    if (opts.Mode == (int)RootMotionMode::Off) return;
    ImGui::SeparatorText(ICON_FA_PERSON_WALKING "  Root Motion");
    // Label column like the controller picker above.
    const float valueX = ImGui::GetContentRegionAvail().x * 0.35f;
    auto PropertyLabel = [&](const char* label, const char* tip) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
        ImGui::SameLine(valueX);
    };

    // The bone: auto, or any of the model's nodes (searchable - a rig has hundreds).
    const int autoNode = model ? model->FindRootMotionNode() : -1;
    const std::string autoLabel = autoNode >= 0 ? "Auto (" + model->NodeName(autoNode) + ")" : std::string("Auto (none found)");
    PropertyLabel("Root Bone", "The bone whose travel is the character's. Auto picks \"root\" when the rig has one\n"
                               "(Unreal, most game rigs), else the hips / pelvis (Mixamo).");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##rmbone", opts.Bone.empty() ? autoLabel.c_str() : opts.Bone.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char filter[96] = "";
        ClipFilterBox(filter, sizeof filter);
        if (!filter[0] && ImGui::Selectable(autoLabel.c_str(), opts.Bone.empty())) {
            PushUndo(world, "Set Root Motion Bone");
            opts.Bone.clear();
        }
        if (model)
            for (int n = 0; n < model->NodeCount(); ++n) {
                const std::string& name = model->NodeName(n);
                if (!ClipFilterMatch(filter, name)) continue;
                if (ImGui::Selectable((name + "##rmn" + std::to_string(n)).c_str(), name == opts.Bone)) {
                    PushUndo(world, "Set Root Motion Bone");
                    opts.Bone = name;
                }
            }
        ImGui::EndCombo();
    }
    const int node = model ? model->FindRootMotionNode(opts.Bone) : -1;
    if (!model || model->NodeCount() == 0)
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", "Needs a rigged model.");
    else if (node < 0)
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                           opts.Bone.empty() ? "No root, hips or pelvis bone found - pick the root bone."
                                             : "This model has no bone of that name.");

    // Who moves the object.
    auto& reg = world.Registry;
    if (opts.Mode == (int)RootMotionMode::Apply) {
        if (reg.all_of<FirstPersonControllerComponent>(entity)) {
            ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  The player moves itself: root motion is only reported here.");
        } else if (const auto* rb = reg.try_get<RigidbodyComponent>(entity); rb && !rb->IsKinematic) {
            ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  Drives the Rigidbody's velocity, so walls stop it.");
            if (opts.Rotation && !(rb->FreezeRotationX && rb->FreezeRotationZ))
                ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                                   "Freeze the Rigidbody's X and Z rotation, or the character can tip over.");
        }
    } else {
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  In Place: game code reads RootMotion.DeltaPosition / DeltaYaw.");
    }

    // Live: what the clips are doing right now.
    if (m_InPlayMode) {
        PropertyLabel("Live", "This frame's root motion (smoothed).");
        if (opts.ResolvedBone < 0) ImGui::TextDisabled("not running");
        else ImGui::Text("%.2f m/s   %+.0f deg/s", opts.Speed, opts.TurnRate);
    }
}

// --- Animator window -------------------------------------------------------------------------

void EditorLayer::DrawAnimatorWindow(World& world) {
    if (!m_ShowAnimator) return;
    if (!m_AnimatorWin) m_AnimatorWin = std::make_shared<AnimatorWindowState>();
    AnimatorWindowState& W = *m_AnimatorWin;
    AssetLibrary* assets = m_AssetsPtr;

    ImGui::SetNextWindowSize(ImVec2(1100.0f * m_UIScale, 620.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(ICON_FA_DIAGRAM_PROJECT "  Animator", &m_ShowAnimator);
    PopTabChromeText();
    if (!open) { ImGui::End(); return; }
    const float S = m_UIScale;

    // --- top bar: file picker, undo/redo, live target -----------------------------------------
    {
        ImGui::SetNextItemWidth(260.0f * S);
        if (ImGui::BeginCombo("##animfile", W.Rel.empty() ? "(open a controller)" : W.Rel.c_str())) {
            for (const std::string& path : FindAnimatorControllers())
                if (ImGui::Selectable(path.c_str(), path == W.Rel)) OpenAnimatorWindow(path);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("The .controller being edited. Create one from the Asset Browser's right-click menu.");
        if (W.Loaded) {
            ImGui::SameLine();
            ImGui::BeginDisabled(W.Undo.empty());
            if (ActionButton(ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)")) W.Step(W.Undo, W.Redo);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(W.Redo.empty());
            if (ActionButton(ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)")) W.Step(W.Redo, W.Undo);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ActionButton(ICON_FA_EXPAND, "Frame all nodes (F)")) W.FramePending = true;
            ImGui::SameLine();
            ImGui::TextDisabled("%.0f%%", W.Zoom * 100.0f);

            // The rig the clip pickers test against and whose playback is shown live.
            ImGui::SameLine();
            std::string targetLabel = "(no rig)";
            if (W.Entity != entt::null && world.Registry.valid(W.Entity))
                if (const auto* nc = world.Registry.try_get<NameComponent>(W.Entity)) targetLabel = nc->Name;
            ImGui::SetNextItemWidth(220.0f * S);
            if (ImGui::BeginCombo("##animtarget", targetLabel.c_str())) {
                if (ImGui::Selectable("(no rig)", W.Entity == entt::null)) W.Entity = entt::null;
                const std::string absSel = ProjectPaths::Resolve(W.Rel);
                for (auto [e, ac] : world.Registry.view<AnimatorControllerComponent>().each()) {
                    if (ac.Controller.empty() || ProjectPaths::Resolve(ac.Controller) != absSel) continue;
                    const auto* nc = world.Registry.try_get<NameComponent>(e);
                    const std::string label = (nc ? nc->Name : std::string("Entity")) + "##" + std::to_string(entt::to_integral(e));
                    if (ImGui::Selectable(label.c_str(), e == W.Entity)) W.Entity = e;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("An object using this controller: its model's clips fill the pickers, and in Play its\n"
                                     "current state is highlighted in the graph.");
        }
    }
    if (!W.Loaded) {
        if (!W.Rel.empty())
            ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  Can't read %s: %s",
                               W.Rel.c_str(), W.Error.c_str());
        else
            ImGui::TextDisabled("Pick a controller above, double-click one in the Asset Browser's Animation folder,\n"
                                "or use Open Animator on an Animator Controller component.");
        ImGui::End();
        return;
    }

    // Reload when the file changed on disk under us (another tool, or version control).
    if (ImGui::GetTime() - W.LastStat > 0.5) {
        W.LastStat = ImGui::GetTime();
        std::error_code ec;
        const auto stamp = fs::last_write_time(fs::u8path(W.Abs), ec);
        if (!ec && stamp != W.Stamp && !W.MovingNodes) {
            const auto undo = W.Undo;
            if (W.Load(W.Rel)) W.Undo = undo;
        }
    }

    // Live runtime of the chosen rig (Play only).
    AnimatorControllerComponent* live = nullptr;
    if (m_InPlayMode && W.Entity != entt::null && world.Registry.valid(W.Entity))
        if (auto* ac = world.Registry.try_get<AnimatorControllerComponent>(W.Entity); ac && ac->Started) live = ac;
    if (m_InPlayMode && !live) {
        // Nothing picked: follow the first object running this controller.
        const std::string absSel = W.Abs;
        for (auto [e, ac] : world.Registry.view<AnimatorControllerComponent>().each())
            if (ac.Started && !ac.Controller.empty() && ProjectPaths::Resolve(ac.Controller) == absSel) {
                W.Entity = e;
                live = &ac;
                break;
            }
    }

    // Clip choices come from the rig when there is one, else every clip the project has loaded.
    Model* rig = nullptr;
    if (W.Entity != entt::null && world.Registry.valid(W.Entity))
        if (const auto* rc = world.Registry.try_get<RenderableComponent>(W.Entity)) rig = rc->ModelRef.get();
    if (assets && (W.ClipsSource != (const void*)rig || W.ClipsModelCount != (int)assets->Models().size())) {
        W.Clips = rig ? ClipChoices(*rig, *assets) : AllClipChoices(*assets);
        W.ClipsSource = rig;
        W.ClipsModelCount = (int)assets->Models().size();
    }

    AC& D = W.Doc;
    bool changed = false; // a discrete edit this frame: committed at the end

    const float leftW = 250.0f * S, rightW = 320.0f * S;
    const float bodyH = ImGui::GetContentRegionAvail().y;

    // ============================ left: layers / parameters ===================================
    ImGui::BeginChild("##animleft", ImVec2(leftW, bodyH), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##animlefttabs")) {
        if (ImGui::BeginTabItem("Layers")) {
            for (int i = 0; i < (int)D.Layers.size(); ++i) {
                ImGui::PushID(i);
                const auto& Ly = D.Layers[i];
                std::string label = Ly.Name;
                if (i > 0) label += Ly.Mode == AC::Blending::Additive ? "  (additive)" : "";
                if (ImGui::Selectable(label.c_str(), W.Layer == i)) { W.Layer = i; W.ClearSelection(); W.FramePending = true; }
                ImGui::PopID();
            }
            if (ActionButton(ICON_FA_PLUS " Layer", "Add a layer: a state machine blended over the ones above it")) {
                std::vector<std::string> names;
                for (const auto& Ly : D.Layers) names.push_back(Ly.Name);
                AC::Layer Ly;
                Ly.Name = UniqueName("New Layer", names);
                D.Layers.push_back(std::move(Ly));
                W.Layer = (int)D.Layers.size() - 1;
                W.ClearSelection();
                changed = true;
            }
            AC::Layer& Ly = W.L();
            ImGui::SeparatorText("Layer Settings");
            ImGui::PushID("layer");
            ImGui::TextUnformatted("Name");
            ImGui::SameLine(80.0f * S);
            if (InputName("##lname", Ly.Name, -FLT_MIN)) changed = true;
            if (W.Layer > 0) {
                ImGui::TextUnformatted("Weight");
                ImGui::SameLine(80.0f * S);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##lweight", &Ly.Weight, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                ImGui::TextUnformatted("Blending");
                ImGui::SameLine(80.0f * S);
                int mode = (int)Ly.Mode;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##lblend", &mode, "Override\0Additive\0")) { Ly.Mode = (AC::Blending)mode; changed = true; }
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Override replaces the pose beneath (by Weight, through the mask).\n"
                                         "Additive adds this layer's motion relative to each clip's first frame.");

                // Bone mask
                ImGui::SeparatorText("Bone Mask");
                ImGui::TextDisabled("Empty Include = whole rig. Children follow their parent.");
                auto maskList = [&](const char* title, std::vector<std::string>& list) {
                    ImGui::TextUnformatted(title);
                    int remove = -1;
                    for (int k = 0; k < (int)list.size(); ++k) {
                        ImGui::PushID(k);
                        if (RemoveButton("Remove")) remove = k;
                        ImGui::SameLine();
                        ImGui::TextUnformatted(list[k].c_str());
                        ImGui::PopID();
                    }
                    if (remove >= 0) { list.erase(list.begin() + remove); changed = true; }
                    ImGui::PushID(title);
                    if (rig && rig->NodeCount() > 0) {
                        if (ActionButton(ICON_FA_PLUS " From rig", "Pick a bone from the rig")) ImGui::OpenPopup("##maskpick");
                        if (ImGui::BeginPopup("##maskpick")) {
                            ImGui::BeginChild("##masklist", ImVec2(260.0f * S, 320.0f * S));
                            for (int n = 0; n < rig->NodeCount(); ++n) {
                                int depth = 0;
                                for (int p = rig->NodeParent(n); p >= 0; p = rig->NodeParent(p)) ++depth;
                                ImGui::Indent(depth * 8.0f * S + 1.0f);
                                if (ImGui::Selectable((rig->NodeName(n) + "##" + std::to_string(n)).c_str())) {
                                    list.push_back(rig->NodeName(n));
                                    changed = true;
                                }
                                ImGui::Unindent(depth * 8.0f * S + 1.0f);
                            }
                            ImGui::EndChild();
                            ImGui::EndPopup();
                        }
                    } else {
                        std::string add;
                        if (InputName("##maskadd", add, -FLT_MIN)) { list.push_back(add); changed = true; }
                        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Type a bone name and press Enter (pick a rig above to browse).");
                    }
                    ImGui::PopID();
                };
                maskList("Include", Ly.MaskInclude);
                maskList("Exclude", Ly.MaskExclude);
                ImGui::Spacing();
                if (W.Layer > 1 && ActionButton(ICON_FA_ARROW_UP " Up", "Evaluate this layer earlier")) {
                    std::swap(D.Layers[W.Layer], D.Layers[W.Layer - 1]);
                    --W.Layer;
                    changed = true;
                }
                ImGui::SameLine();
                if (W.Layer + 1 < (int)D.Layers.size() && ActionButton(ICON_FA_ARROW_DOWN " Down", "Evaluate this layer later")) {
                    std::swap(D.Layers[W.Layer], D.Layers[W.Layer + 1]);
                    ++W.Layer;
                    changed = true;
                }
                ImGui::SameLine();
                if (ActionButton(ICON_FA_TRASH " Delete Layer", "Remove this layer and its states")) {
                    D.Layers.erase(D.Layers.begin() + W.Layer);
                    W.Layer = std::max(0, W.Layer - 1);
                    W.ClearSelection();
                    changed = true;
                }
            } else {
                ImGui::TextDisabled("The base layer always plays at full weight.");
            }
            ImGui::PopID();

            // Transitions whose source or target state is gone are not drawn in the graph (so they can't be
            // selected): say so, and let them be removed.
            {
                AC::Layer& cur = D.Layers[std::clamp(W.Layer, 0, (int)D.Layers.size() - 1)];
                auto broken = [&](const AC::Transition& t) {
                    if (t.FromKind == AC::Source::State && cur.FindState(t.From) < 0) return true;
                    return t.To != AC::kExitState && cur.FindState(t.To) < 0;
                };
                int n = 0;
                for (const auto& t : cur.Transitions) n += broken(t);
                if (n > 0) {
                    ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %d transition%s point%s at a state that no longer exists.",
                                       n, n == 1 ? "" : "s", n == 1 ? "s" : "");
                    if (ActionButton(ICON_FA_TRASH " Remove them", "They are not drawn in the graph and never fire.")) {
                        cur.Transitions.erase(std::remove_if(cur.Transitions.begin(), cur.Transitions.end(), broken), cur.Transitions.end());
                        W.ClearSelection();
                        changed = true;
                    }
                }
            }

            // Tracks
            ImGui::SeparatorText("Tracks");
            ImGui::TextDisabled("Clip sets per state, for rigs animated together\n(e.g. arms + weapon). An object picks one with\nits component's Track field.");
            int removeTrack = -1;
            for (int t = 0; t < (int)D.Tracks.size(); ++t) {
                ImGui::PushID(1000 + t);
                ImGui::BeginDisabled(D.Tracks.size() <= 1);
                if (RemoveButton("Remove this track and its clips")) removeTrack = t;
                ImGui::EndDisabled();
                ImGui::SameLine();
                const std::string trackBefore = D.Tracks[t];
                if (InputName("##tname", D.Tracks[t], -FLT_MIN)) {
                    // Objects on this controller that picked the track by name follow the rename.
                    for (auto [e, ac] : world.Registry.view<AnimatorControllerComponent>().each())
                        if (ac.Controller == W.Rel && ac.Track == trackBefore) ac.Track = D.Tracks[t];
                    changed = true;
                }
                ImGui::PopID();
            }
            if (removeTrack >= 0) {
                // Objects that used it fall back to the first track (an unknown name did that silently).
                for (auto [e, ac] : world.Registry.view<AnimatorControllerComponent>().each())
                    if (ac.Controller == W.Rel && ac.Track == D.Tracks[removeTrack]) ac.Track.clear();
                D.Tracks.erase(D.Tracks.begin() + removeTrack);
                for (auto& Lr : D.Layers)
                    for (auto& s : Lr.States)
                        if (removeTrack < (int)s.Motions.size()) s.Motions.erase(s.Motions.begin() + removeTrack);
                changed = true;
            }
            if (ActionButton(ICON_FA_PLUS " Track", "Add a track")) {
                D.Tracks.push_back(UniqueName("track", D.Tracks));
                changed = true;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Parameters")) {
            if (live) {
                ImGui::SeparatorText("Live");
                for (auto& p : live->Params) LiveParamWidget(p, ImGui::GetContentRegionAvail().x * 0.5f);
                ImGui::SeparatorText("Defaults");
            }
            int remove = -1;
            for (int i = 0; i < (int)D.Parameters.size(); ++i) {
                auto& p = D.Parameters[i];
                ImGui::PushID(i);
                if (RemoveButton("Remove this parameter")) remove = i;
                ImGui::SameLine();
                const std::string before = p.Name;
                if (InputName("##pname", p.Name, ImGui::GetContentRegionAvail().x * 0.45f)) {
                    for (auto& Lr : D.Layers) {
                        for (auto& t : Lr.Transitions)
                            for (auto& cond : t.Conditions) if (cond.Param == before) cond.Param = p.Name;
                        for (auto& s : Lr.States) {
                            if (s.SpeedParam == before) s.SpeedParam = p.Name;
                            for (auto& m : s.Motions) {
                                if (m.BlendParam == before) m.BlendParam = p.Name;
                                if (m.BlendParamY == before) m.BlendParamY = p.Name;
                            }
                        }
                    }
                    changed = true;
                }
                ImGui::SameLine();
                int type = (int)p.Type;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                if (ImGui::Combo("##ptype", &type, kParamTypeLabels)) { p.Type = (AC::ParamType)type; changed = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (p.Type == AC::ParamType::Bool) {
                    bool v = p.Default > 0.5f;
                    if (EditorUIPrimitives::Checkbox("##pdef", &v)) { p.Default = v ? 1.0f : 0.0f; changed = true; }
                } else if (p.Type != AC::ParamType::Trigger) {
                    ImGui::DragFloat("##pdef", &p.Default, 0.05f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                }
                ImGui::PopID();
            }
            // What still uses a parameter: transition conditions, a state's speed parameter, blend-tree axes.
            auto usesOf = [&](const std::string& name, int& conditions, int& speeds, int& blends) {
                conditions = speeds = blends = 0;
                for (const auto& Lr : D.Layers) {
                    for (const auto& t : Lr.Transitions)
                        for (const auto& cond : t.Conditions) conditions += cond.Param == name;
                    for (const auto& st : Lr.States) {
                        speeds += st.SpeedParam == name;
                        for (const auto& m : st.Motions) blends += (m.BlendParam == name) + (m.BlendParamY == name);
                    }
                }
            };
            auto dropParam = [&](int index) {
                const std::string name = D.Parameters[index].Name;
                for (auto& Lr : D.Layers) {
                    for (auto& t : Lr.Transitions)
                        t.Conditions.erase(std::remove_if(t.Conditions.begin(), t.Conditions.end(),
                                                          [&](const AC::Condition& c) { return c.Param == name; }), t.Conditions.end());
                    for (auto& st : Lr.States) {
                        if (st.SpeedParam == name) st.SpeedParam.clear();
                        for (auto& m : st.Motions) {
                            if (m.BlendParam == name) m.BlendParam.clear();
                            if (m.BlendParamY == name) m.BlendParamY.clear();
                        }
                    }
                }
                D.Parameters.erase(D.Parameters.begin() + index);
                changed = true;
            };
            static int s_pendingRemove = -1;
            if (remove >= 0) {
                int c, sp, bl;
                usesOf(D.Parameters[remove].Name, c, sp, bl);
                if (c + sp + bl == 0) dropParam(remove);
                else { s_pendingRemove = remove; ImGui::OpenPopup("Remove parameter?"); }
            }
            if (ImGui::BeginPopupModal("Remove parameter?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (s_pendingRemove >= 0 && s_pendingRemove < (int)D.Parameters.size()) {
                    int c, sp, bl;
                    usesOf(D.Parameters[s_pendingRemove].Name, c, sp, bl);
                    ImGui::Text("'%s' is still used by:", D.Parameters[s_pendingRemove].Name.c_str());
                    if (c) ImGui::BulletText("%d transition condition%s (they are removed: the transition then fires on its other conditions)", c, c == 1 ? "" : "s");
                    if (sp) ImGui::BulletText("%d state speed parameter%s (cleared)", sp, sp == 1 ? "" : "s");
                    if (bl) ImGui::BulletText("%d blend-tree axis%s (cleared: the tree stops blending)", bl, bl == 1 ? "" : "es");
                    ImGui::Spacing();
                    if (ImGui::Button("Remove it and those uses")) { dropParam(s_pendingRemove); s_pendingRemove = -1; ImGui::CloseCurrentPopup(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel")) { s_pendingRemove = -1; ImGui::CloseCurrentPopup(); }
                } else {
                    s_pendingRemove = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (ActionButton(ICON_FA_PLUS " Parameter", "Add a parameter game code can set (SetFloat / SetInt / SetBool / SetTrigger)")) {
                std::vector<std::string> names;
                for (const auto& p : D.Parameters) names.push_back(p.Name);
                D.Parameters.push_back({UniqueName("Param", names), AC::ParamType::Float, 0.0f});
                changed = true;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ================================ middle: the graph =======================================
    AC::Layer& Ly = W.L();
    const float canvasW = std::max(100.0f, ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##animcanvas", ImVec2(canvasW, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 csz = ImGui::GetContentRegionAvail();
    const ImVec2 c1 = c0 + csz;
    ImGui::InvisibleButton("##canvasbtn", csz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                   ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImGuiIO& io = ImGui::GetIO();

    auto nodeWorldPos = [&](NodeRef n) -> glm::vec2& {
        switch (n.Kind) {
            case NodeKind::Entry: return Ly.EntryPosition;
            case NodeKind::Any:   return Ly.AnyPosition;
            case NodeKind::Exit:  return Ly.ExitPosition;
            default:              return Ly.States[n.Index].Position;
        }
    };
    auto nodeSize = [&](NodeRef n) { return n.Kind == NodeKind::State ? ImVec2(kStateW, kStateH) : ImVec2(kSpecialW, kSpecialH); };
    auto toScreen = [&](glm::vec2 p) { return c0 + W.Pan + ImVec2(p.x, p.y) * W.Zoom; };
    auto toWorld = [&](ImVec2 s) { const ImVec2 v = (s - c0 - W.Pan) * (1.0f / W.Zoom); return glm::vec2(v.x, v.y); };
    auto nodeRect = [&](NodeRef n, ImVec2& a, ImVec2& b) {
        a = toScreen(nodeWorldPos(n));
        b = a + nodeSize(n) * W.Zoom;
    };
    auto nodeCenter = [&](NodeRef n) { ImVec2 a, b; nodeRect(n, a, b); return (a + b) * 0.5f; };

    std::vector<NodeRef> nodes;
    for (int i = 0; i < (int)Ly.States.size(); ++i) nodes.push_back({NodeKind::State, i});
    nodes.push_back({NodeKind::Entry, -1});
    nodes.push_back({NodeKind::Any, -1});
    nodes.push_back({NodeKind::Exit, -1});

    if (W.FramePending && csz.x > 10.0f) {
        W.FramePending = false;
        glm::vec2 lo(1e9f), hi(-1e9f);
        for (NodeRef n : nodes) {
            const glm::vec2 p = nodeWorldPos(n);
            const ImVec2 sz = nodeSize(n);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p + glm::vec2(sz.x, sz.y));
        }
        const glm::vec2 ext = hi - lo + glm::vec2(80.0f);
        W.Zoom = std::clamp(std::min(csz.x / ext.x, csz.y / ext.y), 0.25f, 1.5f);
        const glm::vec2 mid = (lo + hi) * 0.5f;
        W.Pan = csz * 0.5f - ImVec2(mid.x, mid.y) * W.Zoom;
    }

    // Grid
    dl->PushClipRect(c0, c1, true);
    dl->AddRectFilled(c0, c1, IM_COL32(32, 32, 34, 255));
    {
        const float step = 40.0f * W.Zoom;
        if (step > 6.0f) {
            for (float x = std::fmod(W.Pan.x, step); x < csz.x; x += step)
                dl->AddLine(ImVec2(c0.x + x, c0.y), ImVec2(c0.x + x, c1.y), IM_COL32(255, 255, 255, 12));
            for (float y = std::fmod(W.Pan.y, step); y < csz.y; y += step)
                dl->AddLine(ImVec2(c0.x, c0.y + y), ImVec2(c1.x, c0.y + y), IM_COL32(255, 255, 255, 12));
        }
    }

    // --- edges -------------------------------------------------------------------------------
    struct Edge {
        NodeRef From, To;
        std::vector<int> Transitions;
        ImVec2 A, B;
    };
    std::map<std::pair<NodeRef, NodeRef>, int> edgeIndex;
    std::vector<Edge> edges;
    for (int t = 0; t < (int)Ly.Transitions.size(); ++t) {
        const auto& tr = Ly.Transitions[t];
        NodeRef from, to;
        if (tr.FromKind == AC::Source::Any) from = {NodeKind::Any, -1};
        else if (tr.FromKind == AC::Source::Entry) from = {NodeKind::Entry, -1};
        else {
            const int s = Ly.FindState(tr.From);
            if (s < 0) continue;
            from = {NodeKind::State, s};
        }
        if (tr.To == AC::kExitState) to = {NodeKind::Exit, -1};
        else {
            const int s = Ly.FindState(tr.To);
            if (s < 0) continue;
            to = {NodeKind::State, s};
        }
        auto key = std::make_pair(from, to);
        auto it = edgeIndex.find(key);
        if (it == edgeIndex.end()) {
            edgeIndex[key] = (int)edges.size();
            edges.push_back({from, to, {t}, {}, {}});
        } else {
            edges[it->second].Transitions.push_back(t);
        }
    }
    // The default-state arrow from Entry is implicit (no transition), drawn in orange.
    const int defaultState = Ly.DefaultStateIndex();

    int hoveredEdge = -1;
    float hoveredEdgeDist = 7.0f;
    for (int e = 0; e < (int)edges.size(); ++e) {
        Edge& E = edges[e];
        ImVec2 a = nodeCenter(E.From), b = nodeCenter(E.To);
        if (E.From == E.To) continue; // self-loop: drawn as an arc below
        // A two-way pair gets a small sideways offset so the arrows don't overlap.
        if (edgeIndex.count({E.To, E.From})) {
            ImVec2 d = b - a;
            const float l = std::max(Len(d), 1e-3f);
            const ImVec2 n(-d.y / l, d.x / l);
            a = a + n * (6.0f * W.Zoom);
            b = b + n * (6.0f * W.Zoom);
        }
        ImVec2 ra, rb;
        nodeRect(E.From, ra, rb);
        const ImVec2 hA = (rb - ra) * 0.5f;
        nodeRect(E.To, ra, rb);
        const ImVec2 hB = (rb - ra) * 0.5f;
        E.A = RectExit(a, hA, b);
        E.B = RectExit(b, hB, a);
        if (canvasHovered) {
            const float d = SegmentDistance(mouse, E.A, E.B);
            if (d < hoveredEdgeDist) { hoveredEdgeDist = d; hoveredEdge = e; }
        }
    }
    auto edgeIsSelected = [&](const Edge& E) {
        return W.SelTransition >= 0 && std::find(E.Transitions.begin(), E.Transitions.end(), W.SelTransition) != E.Transitions.end();
    };
    const int liveTransition = live && W.Layer < (int)live->Layers.size() ? live->Layers[W.Layer].Transition : -1;
    for (int e = 0; e < (int)edges.size(); ++e) {
        const Edge& E = edges[e];
        const bool sel = edgeIsSelected(E);
        const bool active = liveTransition >= 0 &&
                            std::find(E.Transitions.begin(), E.Transitions.end(), liveTransition) != E.Transitions.end();
        const ImU32 col = active ? IM_COL32(90, 170, 255, 255)
                        : sel ? ImGui::GetColorU32(EditorUIPrimitives::ActiveAccentColor())
                        : e == hoveredEdge ? IM_COL32(235, 235, 235, 255) : IM_COL32(190, 190, 190, 200);
        const float thick = (sel || active ? 3.0f : 2.0f) * std::max(W.Zoom, 0.6f);
        if (E.From == E.To) {
            ImVec2 a, b;
            nodeRect(E.From, a, b);
            const ImVec2 top((a.x + b.x) * 0.5f, a.y);
            const float r = 16.0f * W.Zoom;
            dl->AddBezierCubic(top + ImVec2(-r, 0), top + ImVec2(-r * 1.5f, -r * 2.5f), top + ImVec2(r * 1.5f, -r * 2.5f),
                               top + ImVec2(r, 0), col, thick);
            Arrowhead(dl, top + ImVec2(r, -2.0f * W.Zoom), ImVec2(0.3f, 1.0f), 7.0f * W.Zoom, col);
            if (canvasHovered && Len(mouse - (top + ImVec2(0, -r * 1.8f))) < r) hoveredEdge = e;
            continue;
        }
        dl->AddLine(E.A, E.B, col, thick);
        const ImVec2 mid = (E.A + E.B) * 0.5f, dir = E.B - E.A;
        const float l = std::max(Len(dir), 1e-3f);
        const float as = 7.0f * std::max(W.Zoom, 0.6f);
        if (E.Transitions.size() > 1) {
            // Several transitions between the same pair: three chevrons, like Unity.
            for (int k = -1; k <= 1; ++k) Arrowhead(dl, mid + dir * (k * 2.2f * as / l), dir, as, col);
        } else {
            Arrowhead(dl, mid, dir, as, col);
        }
    }
    if (defaultState >= 0) {
        const NodeRef entry{NodeKind::Entry, -1}, def{NodeKind::State, defaultState};
        if (!edgeIndex.count({entry, def})) {
            ImVec2 ra, rb;
            nodeRect(entry, ra, rb);
            const ImVec2 a0 = (ra + rb) * 0.5f, hA = (rb - ra) * 0.5f;
            nodeRect(def, ra, rb);
            const ImVec2 b0 = (ra + rb) * 0.5f, hB = (rb - ra) * 0.5f;
            const ImVec2 a = RectExit(a0, hA, b0), b = RectExit(b0, hB, a0);
            const ImU32 col = IM_COL32(230, 140, 40, 220);
            dl->AddLine(a, b, col, 2.0f * std::max(W.Zoom, 0.6f));
            Arrowhead(dl, (a + b) * 0.5f, b - a, 7.0f * std::max(W.Zoom, 0.6f), col);
        }
    }

    // --- nodes -------------------------------------------------------------------------------
    NodeRef hoveredNode{NodeKind::State, -2};
    for (NodeRef n : nodes) {
        ImVec2 a, b;
        nodeRect(n, a, b);
        if (canvasHovered && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) hoveredNode = n;
    }
    if (hoveredNode.Index != -2) hoveredEdge = -1;

    // Live: which states are contributing, and how far through the current one we are.
    std::map<int, float> liveWeight;
    int liveCurrent = -1;
    float liveProgress = 0.0f;
    if (live && W.Layer < (int)live->Layers.size()) {
        const auto& rt = live->Layers[W.Layer];
        for (const auto& it : rt.Stack) liveWeight[it.State] = std::max(liveWeight[it.State], it.Fade);
        if (!rt.Stack.empty()) {
            liveCurrent = rt.Stack.back().State;
            const float ph = rt.Stack.back().Phase;
            const bool loop = liveCurrent >= 0 && liveCurrent < (int)Ly.States.size() && Ly.States[liveCurrent].Loop;
            liveProgress = loop ? ph - std::floor(ph) : std::min(ph, 1.0f);
        }
    }

    const float fontScale = std::clamp(W.Zoom, 0.5f, 1.4f);
    for (NodeRef n : nodes) {
        ImVec2 a, b;
        nodeRect(n, a, b);
        const float round = 6.0f * W.Zoom;
        ImU32 fill = IM_COL32(70, 72, 78, 255);
        std::string label;
        bool selected = false;
        switch (n.Kind) {
            case NodeKind::Entry: fill = IM_COL32(40, 120, 60, 255); label = "Entry"; selected = W.SelSpecial && W.SelKind == NodeKind::Entry; break;
            case NodeKind::Any:   fill = IM_COL32(40, 130, 130, 255); label = "Any State"; selected = W.SelSpecial && W.SelKind == NodeKind::Any; break;
            case NodeKind::Exit:  fill = IM_COL32(150, 50, 50, 255); label = "Exit"; selected = W.SelSpecial && W.SelKind == NodeKind::Exit; break;
            default: {
                const auto& s = Ly.States[n.Index];
                label = s.Name;
                if (n.Index == defaultState) fill = IM_COL32(180, 100, 30, 255);
                bool blend = false;
                for (const auto& m : s.Motions) blend |= m.IsBlendTree();
                if (blend) label += "  " ICON_FA_SLIDERS;
                selected = W.IsSelected(n.Index);
                break;
            }
        }
        if (n.Kind == NodeKind::State) {
            auto lw = liveWeight.find(n.Index);
            if (lw != liveWeight.end()) {
                const float w = n.Index == liveCurrent ? 1.0f : lw->second * 0.6f;
                const ImVec4 base = ImGui::ColorConvertU32ToFloat4(fill);
                const ImVec4 blue(0.20f, 0.45f, 0.85f, 1.0f);
                fill = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x + (blue.x - base.x) * w, base.y + (blue.y - base.y) * w,
                                                             base.z + (blue.z - base.z) * w, 1.0f));
            }
        }
        dl->AddRectFilled(a + ImVec2(3, 4) * W.Zoom, b + ImVec2(3, 4) * W.Zoom, IM_COL32(0, 0, 0, 90), round);
        dl->AddRectFilled(a, b, fill, round);
        const ImU32 border = selected ? ImGui::GetColorU32(EditorUIPrimitives::ActiveAccentColor())
                           : (n == hoveredNode ? IM_COL32(220, 220, 220, 200) : IM_COL32(20, 20, 20, 200));
        dl->AddRect(a, b, border, round, 0, selected ? 2.5f : 1.0f);
        if (n.Kind == NodeKind::State && n.Index == liveCurrent) {
            const float h = 4.0f * W.Zoom;
            dl->AddRectFilled(ImVec2(a.x + round, b.y - h - 3.0f * W.Zoom),
                              ImVec2(a.x + round + (b.x - a.x - 2 * round) * liveProgress, b.y - 3.0f * W.Zoom),
                              IM_COL32(120, 200, 255, 255));
        }
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * fontScale;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
        // A state's tags ride under its name, small.
        std::string tagLine;
        if (n.Kind == NodeKind::State)
            for (const auto& t : Ly.States[n.Index].Tags) tagLine += (tagLine.empty() ? "" : "  ") + t;
        const float tfs = fs * 0.72f;
        const float lift = tagLine.empty() ? 0.0f : tfs * 0.55f;
        const ImVec2 tp((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f - lift);
        dl->PushClipRect(a, b, true);
        dl->AddText(font, fs, tp, IM_COL32(240, 240, 240, 255), label.c_str());
        if (!tagLine.empty()) {
            const ImVec2 tts = font->CalcTextSizeA(tfs, FLT_MAX, 0.0f, tagLine.c_str());
            dl->AddText(font, tfs, ImVec2((a.x + b.x - tts.x) * 0.5f, tp.y + ts.y), IM_COL32(190, 205, 225, 210), tagLine.c_str());
        }
        dl->PopClipRect();
    }

    // Link preview
    if (W.Linking) {
        const ImVec2 a = nodeCenter(W.LinkFrom);
        dl->AddLine(a, mouse, IM_COL32(255, 255, 255, 200), 2.0f);
        Arrowhead(dl, mouse, mouse - a, 8.0f, IM_COL32(255, 255, 255, 200));
    }
    // Box select
    if (W.BoxSelecting) {
        dl->AddRectFilled(W.BoxStart, mouse, IM_COL32(90, 150, 255, 40));
        dl->AddRect(W.BoxStart, mouse, IM_COL32(90, 150, 255, 160));
    }
    // Hint
    if (Ly.States.empty())
        dl->AddText(c0 + ImVec2(12, 10), IM_COL32(200, 200, 200, 160), "Right-click to create a state.");
    else if (W.Linking)
        dl->AddText(c0 + ImVec2(12, 10), IM_COL32(200, 200, 200, 200), "Click a state (or Exit) to finish the transition. Esc cancels.");
    dl->PopClipRect();

    // --- interaction -------------------------------------------------------------------------
    auto makeTransition = [&](NodeRef from, NodeRef to) {
        if (to.Kind == NodeKind::Entry || to.Kind == NodeKind::Any) return;
        if (from.Kind == NodeKind::Exit) return;
        if (from.Kind == NodeKind::Entry && to.Kind == NodeKind::Exit) return;
        AC::Transition t;
        if (from.Kind == NodeKind::Any) t.FromKind = AC::Source::Any;
        else if (from.Kind == NodeKind::Entry) t.FromKind = AC::Source::Entry;
        else t.From = Ly.States[from.Index].Name;
        t.To = to.Kind == NodeKind::Exit ? std::string(AC::kExitState) : Ly.States[to.Index].Name;
        // Leaving a one-shot is usually "when it ends": start with exit time on.
        if (from.Kind == NodeKind::State && !Ly.States[from.Index].Loop) { t.HasExitTime = true; t.ExitTime = 1.0f; }
        if (from.Kind == NodeKind::Entry) t.Duration = 0.0f;
        Ly.Transitions.push_back(std::move(t));
        W.ClearSelection();
        W.SelTransition = (int)Ly.Transitions.size() - 1;
        changed = true;
    };
    auto deleteStates = [&](std::vector<int> which) {
        std::sort(which.rbegin(), which.rend());
        for (int s : which) {
            if (s < 0 || s >= (int)Ly.States.size()) continue;
            const std::string gone = Ly.States[s].Name;
            Ly.States.erase(Ly.States.begin() + s);
            Ly.Transitions.erase(std::remove_if(Ly.Transitions.begin(), Ly.Transitions.end(),
                                                [&](const AC::Transition& t) {
                                                    return (t.FromKind == AC::Source::State && t.From == gone) || t.To == gone;
                                                }),
                                 Ly.Transitions.end());
            if (Ly.DefaultState == gone) Ly.DefaultState.clear();
        }
        W.ClearSelection();
        changed = true;
    };
    auto duplicateStates = [&]() {
        std::vector<std::string> names;
        for (const auto& s : Ly.States) names.push_back(s.Name);
        std::vector<int> fresh;
        for (int s : W.SelStates) {
            if (s < 0 || s >= (int)Ly.States.size()) continue;
            AC::State copy = Ly.States[s];
            copy.Name = UniqueName(copy.Name, names);
            names.push_back(copy.Name);
            copy.Position += glm::vec2(30.0f, 30.0f);
            Ly.States.push_back(std::move(copy));
            fresh.push_back((int)Ly.States.size() - 1);
        }
        W.ClearSelection();
        W.SelStates = fresh;
        changed = true;
    };
    auto createState = [&](glm::vec2 at, bool blendTree) {
        std::vector<std::string> names;
        for (const auto& s : Ly.States) names.push_back(s.Name);
        AC::State s;
        s.Name = UniqueName(blendTree ? "Blend Tree" : "New State", names);
        s.Motions.resize(D.Tracks.size());
        if (blendTree) {
            std::string param;
            for (const auto& p : D.Parameters) if (p.Type == AC::ParamType::Float) { param = p.Name; break; }
            s.Motions[0].BlendParam = param;
            s.Motions[0].Children = {{"", 0.0f, 1.0f}, {"", 1.0f, 1.0f}};
        }
        s.Position = at - glm::vec2(kStateW, kStateH) * 0.5f;
        Ly.States.push_back(std::move(s));
        if (Ly.States.size() == 1) Ly.DefaultState = Ly.States[0].Name;
        W.ClearSelection();
        W.SelStates = {(int)Ly.States.size() - 1};
        changed = true;
    };

    if (canvasHovered) {
        // Zoom about the mouse.
        if (io.MouseWheel != 0.0f) {
            const glm::vec2 before = toWorld(mouse);
            W.Zoom = std::clamp(W.Zoom * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.2f, 2.5f);
            const ImVec2 after = toScreen(before);
            W.Pan = W.Pan + (mouse - after);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (W.Linking) {
                if (hoveredNode.Index != -2) makeTransition(W.LinkFrom, hoveredNode);
                W.Linking = false;
            } else if (hoveredNode.Index != -2) {
                if (hoveredNode.Kind == NodeKind::State) {
                    if (io.KeyCtrl || io.KeyShift) {
                        auto it = std::find(W.SelStates.begin(), W.SelStates.end(), hoveredNode.Index);
                        if (it != W.SelStates.end()) W.SelStates.erase(it);
                        else W.SelStates.push_back(hoveredNode.Index);
                        W.SelSpecial = false;
                        W.SelTransition = -1;
                    } else if (!W.IsSelected(hoveredNode.Index)) {
                        W.ClearSelection();
                        W.SelStates = {hoveredNode.Index};
                    }
                } else {
                    W.ClearSelection();
                    W.SelSpecial = true;
                    W.SelKind = hoveredNode.Kind;
                }
                W.MovingNodes = true;
            } else if (hoveredEdge >= 0) {
                const Edge& E = edges[hoveredEdge];
                // Clicking a multi-transition arrow again cycles through its transitions.
                int pick = E.Transitions.front();
                auto it = std::find(E.Transitions.begin(), E.Transitions.end(), W.SelTransition);
                if (it != E.Transitions.end() && ++it != E.Transitions.end()) pick = *it;
                W.ClearSelection();
                W.SelTransition = pick;
            } else {
                if (!io.KeyCtrl && !io.KeyShift) W.ClearSelection();
                W.BoxSelecting = true;
                W.BoxStart = mouse;
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (W.Linking) {
                W.Linking = false;
            } else {
                W.ContextNode = hoveredNode;
                W.ContextTransition = hoveredEdge >= 0 ? edges[hoveredEdge].Transitions.front() : -1;
                W.ContextWorld = toWorld(mouse);
                if (hoveredNode.Index != -2 && hoveredNode.Kind == NodeKind::State && !W.IsSelected(hoveredNode.Index)) {
                    W.ClearSelection();
                    W.SelStates = {hoveredNode.Index};
                }
                ImGui::OpenPopup("##animctx");
            }
        }
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && ImGui::IsItemActive())
        W.Pan = W.Pan + io.MouseDelta;
    if (W.MovingNodes) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const glm::vec2 d(io.MouseDelta.x / W.Zoom, io.MouseDelta.y / W.Zoom);
            if (W.SelSpecial) nodeWorldPos({W.SelKind, -1}) += d;
            for (int s : W.SelStates) if (s >= 0 && s < (int)Ly.States.size()) Ly.States[s].Position += d;
        } else {
            W.MovingNodes = false;
            changed = true; // positions are saved (and undoable) once the drag ends
        }
    }
    if (W.BoxSelecting) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            W.BoxSelecting = false;
            const ImVec2 lo(std::min(W.BoxStart.x, mouse.x), std::min(W.BoxStart.y, mouse.y));
            const ImVec2 hi(std::max(W.BoxStart.x, mouse.x), std::max(W.BoxStart.y, mouse.y));
            if (hi.x - lo.x > 3.0f || hi.y - lo.y > 3.0f)
                for (int i = 0; i < (int)Ly.States.size(); ++i) {
                    ImVec2 a, b;
                    nodeRect({NodeKind::State, i}, a, b);
                    if (b.x >= lo.x && a.x <= hi.x && b.y >= lo.y && a.y <= hi.y && !W.IsSelected(i)) W.SelStates.push_back(i);
                }
        }
    }

    if (ImGui::BeginPopup("##animctx")) {
        const NodeRef n = W.ContextNode;
        if (n.Index != -2) {
            if (n.Kind != NodeKind::Exit && ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Make Transition")) {
                W.Linking = true;
                W.LinkFrom = n;
            }
            if (n.Kind == NodeKind::State) {
                if (ImGui::MenuItem(ICON_FA_STAR "  Set as Layer Default State", nullptr, false, n.Index != defaultState)) {
                    Ly.DefaultState = Ly.States[n.Index].Name;
                    changed = true;
                }
                if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate", "Ctrl+D")) duplicateStates();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del")) deleteStates(W.SelStates);
            }
        } else if (W.ContextTransition >= 0) {
            if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Transition")) {
                Ly.Transitions.erase(Ly.Transitions.begin() + W.ContextTransition);
                W.ClearSelection();
                changed = true;
            }
        } else {
            if (ImGui::MenuItem(ICON_FA_SQUARE_PLUS "  Create State")) createState(W.ContextWorld, false);
            if (ImGui::MenuItem(ICON_FA_SLIDERS "  Create Blend Tree State")) createState(W.ContextWorld, true);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_EXPAND "  Frame All", "F")) W.FramePending = true;
        }
        ImGui::EndPopup();
    }

    // Keyboard, while the window has focus and no text field does.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { W.Linking = false; W.BoxSelecting = false; }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (!W.SelStates.empty()) deleteStates(W.SelStates);
            else if (W.SelTransition >= 0 && W.SelTransition < (int)Ly.Transitions.size()) {
                Ly.Transitions.erase(Ly.Transitions.begin() + W.SelTransition);
                W.ClearSelection();
                changed = true;
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !W.SelStates.empty()) duplicateStates();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) W.Step(W.Redo, W.Undo); else W.Step(W.Undo, W.Redo); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) W.Step(W.Redo, W.Undo);
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) W.FramePending = true;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ============================ right: the selection's properties ===========================
    ImGui::BeginChild("##animright", ImVec2(0.0f, bodyH), ImGuiChildFlags_Borders);
    AC::Layer& L = W.L(); // may have changed layers above
    const float labelW = 110.0f * S;
    auto row = [&](const char* label) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
    };
    auto paramCombo = [&](const char* id, std::string& value, bool floatsOnly, bool allowNone) {
        bool edited = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, value.empty() ? "(none)" : value.c_str())) {
            if (allowNone && ImGui::Selectable("(none)", value.empty())) { value.clear(); edited = true; }
            for (const auto& p : D.Parameters) {
                if (floatsOnly && p.Type != AC::ParamType::Float && p.Type != AC::ParamType::Int) continue;
                if (ImGui::Selectable(p.Name.c_str(), p.Name == value)) { value = p.Name; edited = true; }
            }
            ImGui::EndCombo();
        }
        return edited;
    };
    // A clip slot: pick from the list, type a reference, or drop a model from the Asset Browser.
    auto clipSlot = [&](const char* id, std::string& ref) {
        bool edited = false;
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clip", ClipLabel(ref).c_str(), ImGuiComboFlags_HeightLarge)) {
            static char filter[128] = "";
            ClipFilterBox(filter, sizeof filter);
            if (!filter[0] && ImGui::Selectable("(none)", ref.empty())) { ref.clear(); edited = true; }
            for (const auto& [r, label] : W.Clips)
                if (ClipFilterMatch(filter, label + " " + r) && ImGui::Selectable((label + "##" + r).c_str(), r == ref)) {
                    ref = r;
                    edited = true;
                }
            // Files no scene has loaded yet (e.g. an imported animation pack).
            std::string picked = ref;
            if (ProjectClipFileList(filter, picked) && picked != ref) {
                ref = picked;
                edited = true;
            }
            if (edited) ImGui::CloseCurrentPopup();
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("%s\n\nPick a clip, or drag an animation file here from the Asset Browser.", ref.empty() ? "(none)" : ref.c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
                ref = ProjectPaths::Relativize(std::string((const char*)p->Data));
                edited = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        return edited;
    };

    const bool stateSel = W.SelStates.size() == 1 && W.SelStates[0] < (int)L.States.size();
    if (stateSel) {
        const int si = W.SelStates[0];
        AC::State& s = L.States[si];
        ImGui::SeparatorText(ICON_FA_SQUARE "  State");
        row("Name");
        const std::string before = s.Name;
        if (InputName("##sname", s.Name, -FLT_MIN)) {
            if (AC::IsReservedName(s.Name) || L.FindState(s.Name) != si) {
                s.Name = before; // reserved or taken
            } else {
                for (auto& t : L.Transitions) {
                    if (t.FromKind == AC::Source::State && t.From == before) t.From = s.Name;
                    if (t.To == before) t.To = s.Name;
                }
                if (L.DefaultState == before) L.DefaultState = s.Name;
                changed = true;
            }
        }
        row("Default");
        if (si == L.DefaultStateIndex()) ImGui::TextDisabled(ICON_FA_STAR "  Layer default");
        else if (ActionButton("Set as Default", "Make this the state the layer starts in")) { L.DefaultState = s.Name; changed = true; }
        row("Speed");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##sspeed", &s.Speed, 0.01f, 0.0f, 10.0f, "x%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        row("Speed Param");
        if (paramCombo("##sspeedp", s.SpeedParam, true, true)) changed = true;
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Optional: a Float parameter that multiplies Speed.");
        row("Loop");
        if (EditorUIPrimitives::Checkbox("##sloop", &s.Loop)) changed = true;
        row("Root Motion");
        if (EditorUIPrimitives::Checkbox("##srm", &s.RootMotion)) changed = true;
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("When the object's Animator Controller has Root Motion on, this state's travel moves\n"
                                 "the object. Off keeps the travel in the pose, as authored.");
        {
            // What the clip itself travels, measured on the rig (needs one picked above).
            Model* rigModel = nullptr;
            const AnimatorControllerComponent* rigAc = nullptr;
            if (W.Entity != entt::null && world.Registry.valid(W.Entity)) {
                if (const auto* rc = world.Registry.try_get<RenderableComponent>(W.Entity)) rigModel = rc->ModelRef.get();
                rigAc = world.Registry.try_get<AnimatorControllerComponent>(W.Entity);
            }
            const AC::Motion& m = s.MotionFor(rigAc ? D.TrackIndex(rigAc->Track) : 0);
            const int node = rigModel ? rigModel->FindRootMotionNode(rigAc ? rigAc->RootMotion.Bone : std::string()) : -1;
            if (assets && rigModel && node >= 0 && !m.Empty()) {
                RootMotionSettings rms;
                if (rigAc) { rms.Rotation = rigAc->RootMotion.Rotation; rms.Vertical = rigAc->RootMotion.Vertical; }
                auto describe = [&](const std::string& ref, const char* prefix) {
                    const int c = ResolveAnimationClip(*rigModel, ref, *assets);
                    const float len = c >= 0 ? rigModel->AnimationLength(c) : 0.0f;
                    if (!(len > 0.0f)) return;
                    const RootMotionDelta d = rigModel->ClipRootMotion(c, 0.0f, len, AnimationWrapMode::ClampForever, node, rms);
                    const float dist = glm::length(glm::vec2(d.Translation.x, d.Translation.z));
                    if (dist < 0.005f && std::abs(d.Yaw) < 0.01f) ImGui::TextDisabled("%sin place", prefix);
                    else ImGui::TextDisabled("%s%.2f m, %+.0f deg per pass  (%.2f m/s)", prefix, dist, glm::degrees(d.Yaw), dist / len);
                };
                row("");
                if (!m.IsBlendTree()) describe(m.Clip, "");
                else {
                    ImGui::TextDisabled("Per child:");
                    for (const auto& child : m.Children) {
                        row("");
                        const std::string prefix = ClipLabel(child.Clip) + ": ";
                        describe(child.Clip, prefix.c_str());
                    }
                }
                if (ImGui::IsItemHovered())
                    EditorUI::SetTooltip("The root's travel over one pass of the clip, measured on %s.\n"
                                         "Use the speeds to set blend thresholds and movement speeds that match the feet.",
                                         rigModel->NodeName(node).c_str());
            }
        }
        row("Priority");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("##sprio", &s.Priority)) changed = true;
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("Any State transitions with Respect Priority only enter a state of strictly higher\n"
                                 "priority than the current one - e.g. firing (2) can't cut a reload (3) short.");
        row("Tags");
        {
            // One chip per tag (click to remove), wrapping under the value column, then "+".
            const float colX = ImGui::GetCursorPosX();
            const float right = ImGui::GetWindowContentRegionMax().x;
            const ImGuiStyle& style = ImGui::GetStyle();
            int removeTag = -1;
            for (int k = 0; k < (int)s.Tags.size(); ++k) {
                ImGui::PushID(k);
                const std::string chip = s.Tags[k] + "  " ICON_FA_XMARK;
                const float w = ImGui::CalcTextSize(chip.c_str()).x + style.FramePadding.x * 2.0f;
                if (k > 0) {
                    ImGui::SameLine();
                    if (ImGui::GetCursorPosX() + w > right) { ImGui::NewLine(); ImGui::SetCursorPosX(colX); }
                }
                const char* desc = FirstPersonAnimatorContract::KnownTagDescription(s.Tags[k]);
                char tip[512];
                std::snprintf(tip, sizeof tip, "%s\n\nClick to remove.", desc ? desc : "A custom tag: game code reads it with HasTag().");
                if (ActionButton(chip.c_str(), tip, true)) removeTag = k;
                ImGui::PopID();
            }
            if (!s.Tags.empty()) {
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() + ImGui::GetFrameHeight() > right) { ImGui::NewLine(); ImGui::SetCursorPosX(colX); }
            }
            if (ActionButton(ICON_FA_PLUS "##addtag", "Add a tag", false, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                ImGui::OpenPopup("##tagpick");
            if (removeTag >= 0) { s.Tags.erase(s.Tags.begin() + removeTag); changed = true; }
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f), ImVec2(ImGui::GetFontSize() * 26.0f, FLT_MAX));
            if (ImGui::BeginPopup("##tagpick")) {
                ImGui::TextDisabled("Tags the first-person driver reads");
                for (const auto& t : FirstPersonAnimatorContract::kKnownTags) {
                    const bool has = std::find(s.Tags.begin(), s.Tags.end(), t.Name) != s.Tags.end();
                    if (ImGui::Selectable(t.Name, has, has ? ImGuiSelectableFlags_Disabled : 0)) {
                        s.Tags.push_back(t.Name);
                        changed = true;
                    }
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24.0f);
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", t.Description);
                    ImGui::Unindent();
                    ImGui::PopTextWrapPos();
                }
                ImGui::Separator();
                static char custom[64] = "";
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputTextWithHint("##customtag", "Custom tag, then Enter", custom, sizeof custom,
                                             ImGuiInputTextFlags_EnterReturnsTrue) && custom[0]) {
                    if (std::find(s.Tags.begin(), s.Tags.end(), custom) == s.Tags.end()) { s.Tags.push_back(custom); changed = true; }
                    custom[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // Motions, one per track.
        if ((int)s.Motions.size() < (int)D.Tracks.size()) s.Motions.resize(D.Tracks.size());
        for (int t = 0; t < (int)D.Tracks.size(); ++t) {
            AC::Motion& m = s.Motions[t];
            ImGui::PushID(t);
            ImGui::SeparatorText((ICON_FA_FILM "  Motion: " + D.Tracks[t]).c_str());
            row("Type");
            int kind = !m.IsBlendTree() ? 0 : m.Is2D() ? 2 : 1;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##mkind", &kind, "Clip\0Blend Tree 1D\0Blend Tree 2D\0")) {
                if (kind >= 1 && !m.IsBlendTree()) {
                    m.Children = {{m.Clip, 0.0f, 1.0f}, {"", 1.0f, 1.0f}};
                    m.Clip.clear();
                    for (const auto& p : D.Parameters) if (p.Type == AC::ParamType::Float) { m.BlendParam = p.Name; break; }
                } else if (kind == 0 && m.IsBlendTree()) {
                    m.Clip = m.Children.front().Clip;
                    m.Children.clear();
                    m.BlendParam.clear();
                    m.BlendParamY.clear();
                }
                if (kind == 2 && !m.Is2D()) {
                    // A second Float parameter for Y (the first that isn't X).
                    for (const auto& p : D.Parameters)
                        if (p.Type == AC::ParamType::Float && p.Name != m.BlendParam) { m.BlendParamY = p.Name; break; }
                    if (m.BlendParamY.empty()) m.BlendParamY = "MoveY";
                } else if (kind == 1) {
                    m.BlendParamY.clear();
                }
                changed = true;
            }
            if (!m.IsBlendTree()) {
                row("Clip");
                if (clipSlot("##mclip", m.Clip)) changed = true;
                if (m.Empty()) ImGui::TextDisabled(W.Layer == 0 ? "No clip: this track holds its bind pose." : "No clip: this layer leaves the pose alone.");
            } else {
                const bool is2D = m.Is2D();
                row(is2D ? "Parameter X" : "Parameter");
                if (paramCombo("##bparam", m.BlendParam, true, false)) changed = true;
                if (is2D) {
                    row("Parameter Y");
                    if (paramCombo("##bparamy", m.BlendParamY, true, false)) changed = true;
                    if (ImGui::IsItemHovered())
                        EditorUI::SetTooltip("2D freeform blend: each child sits at an (X, Y) point and blends by how\n"
                                             "close the two parameters are to it - e.g. MoveX / MoveY in m/s, with\n"
                                             "each clip at its own measured velocity.");
                }
                int remove = -1;
                if (ImGui::BeginTable("##children", is2D ? 5 : 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    ImGui::TableSetupColumn(is2D ? "X" : "Threshold", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    if (is2D) ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                    ImGui::TableHeadersRow();
                    for (int k = 0; k < (int)m.Children.size(); ++k) {
                        auto& ch = m.Children[k];
                        ImGui::PushID(k);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (clipSlot("##cclip", ch.Clip)) changed = true;
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::DragFloat("##cthr", &ch.Threshold, 0.01f);
                        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                        if (is2D) {
                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(-FLT_MIN);
                            ImGui::DragFloat("##cthry", &ch.ThresholdY, 0.01f);
                            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                        }
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::DragFloat("##cspd", &ch.Speed, 0.01f, 0.01f, 10.0f, "%.2f");
                        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                        ImGui::TableNextColumn();
                        if (RemoveButton("Remove this child")) remove = k;
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (remove >= 0 && m.Children.size() > 1) { m.Children.erase(m.Children.begin() + remove); changed = true; }
                if (ActionButton(ICON_FA_PLUS " Child", "Add a clip to the blend tree")) {
                    const float next = m.Children.empty() ? 0.0f : m.Children.back().Threshold + 1.0f;
                    m.Children.push_back({"", next, 1.0f});
                    changed = true;
                }
                // Preview of the weights at the live (or default) parameter values.
                auto valueOf = [&](const std::string& name) {
                    float v = 0.0f;
                    if (const auto* p = D.FindParameter(name)) v = p->Default;
                    if (live) v = live->GetFloat(name);
                    return v;
                };
                const float v = valueOf(m.BlendParam), vy = is2D ? valueOf(m.BlendParamY) : 0.0f;
                const auto w = is2D ? AnimatorBlendWeights2D(m.Children, v, vy) : AnimatorBlendWeights(m.Children, v);
                if (is2D)
                    ImGui::TextDisabled("At %s = %.2f, %s = %.2f:", m.BlendParam.empty() ? "?" : m.BlendParam.c_str(), v,
                                        m.BlendParamY.c_str(), vy);
                else
                    ImGui::TextDisabled("At %s = %.2f:", m.BlendParam.empty() ? "?" : m.BlendParam.c_str(), v);
                for (size_t k = 0; k < m.Children.size(); ++k) {
                    ImGui::ProgressBar(w[k], ImVec2(-FLT_MIN, 0.0f), ClipLabel(m.Children[k].Clip).c_str());
                }
            }
            ImGui::PopID();
        }

        // Events
        ImGui::SeparatorText(ICON_FA_FLAG "  Events");
        ImGui::TextDisabled("Game code reads these with EventFired(name).");
        int removeEvent = -1;
        for (int k = 0; k < (int)s.Events.size(); ++k) {
            auto& e = s.Events[k];
            ImGui::PushID(2000 + k);
            if (RemoveButton("Remove this event")) removeEvent = k;
            ImGui::SameLine();
            if (InputName("##ename", e.Name, ImGui::GetContentRegionAvail().x * 0.45f)) changed = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##etime", &e.Time, 0.0f, 1.0f, "at %.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::PopID();
        }
        if (removeEvent >= 0) { s.Events.erase(s.Events.begin() + removeEvent); changed = true; }
        if (ActionButton(ICON_FA_PLUS " Event", "Add an event at a normalized time in this state (0 = start, 1 = end)")) {
            s.Events.push_back({"Event", 1.0f});
            changed = true;
        }

        // Transitions out of this state, for quick access.
        ImGui::SeparatorText(ICON_FA_ARROW_RIGHT "  Transitions");
        for (int t = 0; t < (int)L.Transitions.size(); ++t) {
            const auto& tr = L.Transitions[t];
            if (tr.FromKind != AC::Source::State || tr.From != s.Name) continue;
            ImGui::PushID(3000 + t);
            if (ImGui::Selectable((s.Name + "  " ICON_FA_ARROW_RIGHT "  " + tr.To).c_str())) {
                W.ClearSelection();
                W.SelTransition = t;
            }
            ImGui::PopID();
        }
        if (ActionButton(ICON_FA_PLUS " Make Transition", "Then click the destination state in the graph")) {
            W.Linking = true;
            W.LinkFrom = {NodeKind::State, si};
        }
    } else if (W.SelStates.size() > 1) {
        ImGui::TextDisabled("%d states selected.", (int)W.SelStates.size());
        if (ActionButton(ICON_FA_TRASH " Delete", "Delete the selected states and their transitions")) deleteStates(W.SelStates);
    } else if (W.SelTransition >= 0 && W.SelTransition < (int)L.Transitions.size()) {
        AC::Transition& t = L.Transitions[W.SelTransition];
        const std::string fromLabel = t.FromKind == AC::Source::Any ? "Any State" : t.FromKind == AC::Source::Entry ? "Entry" : t.From;
        ImGui::SeparatorText(ICON_FA_ARROW_RIGHT "  Transition");
        ImGui::TextWrapped("%s  " ICON_FA_ARROW_RIGHT "  %s", fromLabel.c_str(), t.To.c_str());
        // Siblings between the same pair: evaluated top to bottom, the first that holds wins.
        {
            std::vector<int> sib;
            for (int k = 0; k < (int)L.Transitions.size(); ++k) {
                const auto& o = L.Transitions[k];
                if (o.FromKind == t.FromKind && o.From == t.From && o.To == t.To) sib.push_back(k);
            }
            if (sib.size() > 1) {
                ImGui::TextDisabled("%d transitions between these two:", (int)sib.size());
                for (int k : sib) {
                    ImGui::PushID(4000 + k);
                    std::string label = "#" + std::to_string(k);
                    for (const auto& c : L.Transitions[k].Conditions) label += "  " + c.Param;
                    if (L.Transitions[k].HasExitTime) label += "  (exit time)";
                    if (ImGui::Selectable(label.c_str(), k == W.SelTransition)) W.SelTransition = k;
                    ImGui::PopID();
                }
            }
        }
        row("Order");
        {
            const int idx = W.SelTransition;
            ImGui::BeginDisabled(idx == 0);
            if (ActionButton(ICON_FA_ARROW_UP, "Evaluate earlier (higher priority)")) {
                std::swap(L.Transitions[idx], L.Transitions[idx - 1]);
                W.SelTransition = idx - 1;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(idx + 1 >= (int)L.Transitions.size());
            if (ActionButton(ICON_FA_ARROW_DOWN, "Evaluate later")) {
                std::swap(L.Transitions[idx], L.Transitions[idx + 1]);
                W.SelTransition = idx + 1;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("#%d of %d", idx, (int)L.Transitions.size());
        }
        AC::Transition& tr = L.Transitions[W.SelTransition];
        row("To");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##tto", tr.To.c_str())) {
            if (tr.FromKind != AC::Source::Entry && ImGui::Selectable("Exit", tr.To == AC::kExitState)) { tr.To = AC::kExitState; changed = true; }
            for (const auto& s : L.States)
                if (ImGui::Selectable(s.Name.c_str(), s.Name == tr.To)) { tr.To = s.Name; changed = true; }
            ImGui::EndCombo();
        }
        if (tr.FromKind != AC::Source::Entry) {
            row("Has Exit Time");
            if (EditorUIPrimitives::Checkbox("##texit", &tr.HasExitTime)) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Wait until the source state has played this far (1 = its end) before leaving.");
            if (tr.HasExitTime) {
                row("Exit Time");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::DragFloat("##texitt", &tr.ExitTime, 0.01f, 0.0f, 10.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            row("Duration");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##tdur", &tr.Duration, 0.005f, 0.0f, 5.0f, "%.3f s");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            row("Offset");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##toff", &tr.Offset, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Where in the destination state to start (0 = its beginning).");
            row("Interruptible");
            if (EditorUIPrimitives::Checkbox("##tint", &tr.Interruptible)) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Off: nothing can interrupt this transition's crossfade once it starts.");
        } else {
            ImGui::TextDisabled("Entry transitions pick the state a layer starts in (and\nreturns to through Exit): the first one whose\nconditions hold, else the default state.");
        }
        if (tr.FromKind == AC::Source::Any) {
            row("Respect Priority");
            if (EditorUIPrimitives::Checkbox("##tprio", &tr.RespectPriority)) changed = true;
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("Only fire into a state of strictly higher Priority than the one playing.");
            row("To Self");
            if (EditorUIPrimitives::Checkbox("##tself", &tr.CanTransitionToSelf)) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("May restart the destination when it is already playing.");
        }

        ImGui::SeparatorText("Conditions");
        int removeCond = -1;
        for (int k = 0; k < (int)tr.Conditions.size(); ++k) {
            auto& cond = tr.Conditions[k];
            ImGui::PushID(k);
            if (RemoveButton("Remove this condition")) removeCond = k;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            if (ImGui::BeginCombo("##cparam", cond.Param.empty() ? "(parameter)" : cond.Param.c_str())) {
                for (const auto& p : D.Parameters)
                    if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Param)) {
                        cond.Param = p.Name;
                        // Bool / Trigger conditions only make sense as Is True / Is False.
                        if ((p.Type == AC::ParamType::Bool || p.Type == AC::ParamType::Trigger) &&
                            cond.Mode != AC::Op::If && cond.Mode != AC::Op::IfNot)
                            cond.Mode = AC::Op::If;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            int mode = (int)cond.Mode;
            ImGui::SetNextItemWidth(cond.Mode != AC::Op::If && cond.Mode != AC::Op::IfNot ? ImGui::GetContentRegionAvail().x * 0.55f : -FLT_MIN);
            if (ImGui::Combo("##cmode", &mode, kOpLabels)) { cond.Mode = (AC::Op)mode; changed = true; }
            if (cond.Mode != AC::Op::If && cond.Mode != AC::Op::IfNot) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::DragFloat("##cthr", &cond.Threshold, 0.05f);
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            ImGui::PopID();
        }
        if (removeCond >= 0) { tr.Conditions.erase(tr.Conditions.begin() + removeCond); changed = true; }
        if (ActionButton(ICON_FA_PLUS " Condition", "All conditions must hold for the transition to fire")) {
            AC::Condition cond;
            if (!D.Parameters.empty()) {
                cond.Param = D.Parameters.front().Name;
                if (D.Parameters.front().Type == AC::ParamType::Bool || D.Parameters.front().Type == AC::ParamType::Trigger)
                    cond.Mode = AC::Op::If;
            }
            tr.Conditions.push_back(cond);
            changed = true;
        }
        if (tr.Conditions.empty() && !tr.HasExitTime && tr.FromKind != AC::Source::Entry)
            ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               "Never fires: add a condition or an exit time.");
        ImGui::Spacing();
        if (ActionButton(ICON_FA_TRASH " Delete Transition", "Remove this transition")) {
            L.Transitions.erase(L.Transitions.begin() + W.SelTransition);
            W.ClearSelection();
            changed = true;
        }
    } else if (W.SelSpecial) {
        switch (W.SelKind) {
            case NodeKind::Entry:
                ImGui::SeparatorText("Entry");
                ImGui::TextWrapped("Where the layer starts, and where a transition to Exit comes back in. "
                                   "Entry transitions (right-click Entry > Make Transition) are checked in order; "
                                   "the first whose conditions hold wins, else the default state (orange).");
                row("Default State");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##edef", L.DefaultStateIndex() >= 0 ? L.States[L.DefaultStateIndex()].Name.c_str() : "(none)")) {
                    for (const auto& s : L.States)
                        if (ImGui::Selectable(s.Name.c_str(), s.Name == L.DefaultState)) { L.DefaultState = s.Name; changed = true; }
                    ImGui::EndCombo();
                }
                break;
            case NodeKind::Any:
                ImGui::SeparatorText("Any State");
                ImGui::TextWrapped("Transitions from Any State are checked every frame from whatever state is playing, "
                                   "before that state's own. Turn on Respect Priority to stop them cutting "
                                   "higher-priority states short.");
                break;
            case NodeKind::Exit:
                ImGui::SeparatorText("Exit");
                ImGui::TextWrapped("A transition to Exit leaves the current state and re-enters the layer through Entry, "
                                   "so the Entry transitions pick the next state from the current parameters.");
                break;
            default: break;
        }
    } else {
        ImGui::TextDisabled("Select a state or transition.");
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click the graph to create states.\nRight-click a state > Make Transition,\nthen click the destination.\n\n"
                            "Middle-drag pans, the wheel zooms, F frames.\nDel deletes, Ctrl+D duplicates,\nCtrl+Z / Ctrl+Y undo and redo.");
    }
    ImGui::EndChild();

    if (changed) W.Commit();
    ImGui::End();
}

// --- asset inspectors ------------------------------------------------------------------------

void EditorLayer::DrawControllerAssetInspector(const std::string& path) {
    const std::string rel = ProjectPaths::Relativize(path);
    ImGui::SeparatorText(ICON_FA_DIAGRAM_PROJECT "  Animator Controller");
    ImGui::TextWrapped("%s", rel.c_str());
    if (ActionButton(ICON_FA_DIAGRAM_PROJECT "  Open in Animator", "Edit it in the Animator window", false, ImVec2(-FLT_MIN, 0.0f)))
        OpenAnimatorWindow(rel);
    if (const auto ctrl = GetAnimatorController(rel)) {
        ImGui::Spacing();
        ImGui::Text("Tracks: %d   Parameters: %d", (int)ctrl->Tracks.size(), (int)ctrl->Parameters.size());
        for (const auto& L : ctrl->Layers)
            ImGui::BulletText("%s: %d states, %d transitions", L.Name.c_str(), (int)L.States.size(), (int)L.Transitions.size());
    } else {
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", "Can't read this controller.");
    }
}

// --- Clip pickers: project files ---------------------------------------------------------------

namespace EditorInternal {

const std::vector<std::string>& ProjectModelFiles() {
    static std::vector<std::string> files;
    static std::chrono::steady_clock::time_point scanned{};
    static bool once = false;
    const auto now = std::chrono::steady_clock::now();
    if (once && now - scanned < std::chrono::seconds(5)) return files;
    once = true;
    scanned = now;
    files.clear();
    std::error_code ec;
    const fs::path root(ProjectPaths::Root());
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory() && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
        if (ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".dae")
            files.push_back(it->path().lexically_relative(root).generic_u8string()); // no disk access per file
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool ClipFilterMatch(const char* filter, const std::string& text) {
    if (!filter || !*filter) return true;
    std::string hay = text;
    for (char& ch : hay) ch = (char)std::tolower((unsigned char)ch);
    const char* p = filter;
    while (*p) {
        while (*p == ' ') ++p;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        if (p == start) break;
        std::string word(start, p);
        for (char& ch : word) ch = (char)std::tolower((unsigned char)ch);
        if (hay.find(word) == std::string::npos) return false;
    }
    return true;
}

void ClipFilterBox(char* buf, size_t size) {
    if (ImGui::IsWindowAppearing()) { buf[0] = '\0'; ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##clipfilter", ICON_FA_MAGNIFYING_GLASS "  Search clips (e.g. walk fwd no_rm)", buf, size);
}

bool ProjectClipFileList(const char* filter, std::string& ref, int maxShown) {
    bool picked = false, header = false;
    int shown = 0, matched = 0;
    for (const std::string& path : ProjectModelFiles()) {
        if (!ClipFilterMatch(filter, path)) continue;
        ++matched;
        if (shown >= maxShown) continue;
        if (!header) {
            ImGui::SeparatorText("Project files");
            header = true;
        }
        const std::string label = fs::u8path(path).stem().u8string() + "##pf" + path;
        if (ImGui::Selectable(label.c_str(), path == ref)) { ref = path; picked = true; }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s\n\nLoads when picked (the file's first clip).", path.c_str());
        ++shown;
    }
    if (matched > shown) ImGui::TextDisabled("%d more - type to narrow the list", matched - shown);
    return picked;
}

} // namespace EditorInternal
