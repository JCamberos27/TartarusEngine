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
#include "EditorLayer_AnimatorInternal.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AnimatorLint.h"
#include "ClipAnalysis.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonAnimation.h"
#include "Components.h"
#include "CurveEditor.h"
#include "AssetLibrary.h"
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
#include <unordered_map>

using namespace EditorInternal;
namespace fs = std::filesystem;
using AC = AnimatorController;

using namespace AnimatorUI;


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
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130.0f * S);
            ImGui::InputTextWithHint("##statesearch", ICON_FA_MAGNIFYING_GLASS " find state", g_stateSearch, sizeof g_stateSearch);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Highlights states whose name contains this text. Enter selects the first and pans to it.");
            if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter) && g_stateSearch[0]) {
                const AC::Layer& sl = W.L();
                for (int i = 0; i < (int)sl.States.size(); ++i)
                    if (ClipFilterMatch(g_stateSearch, sl.States[i].Name)) { W.ClearSelection(); W.SelStates = {i}; W.CenterOn = sl.States[i].Name; break; }
            }

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


    const float leftW = 250.0f * S, rightW = 320.0f * S;
    const float bodyH = ImGui::GetContentRegionAvail().y;

    AnimCtx ctx{world, W, assets, live, rig, S, leftW, rightW, bodyH};
    DrawLayersAndParameters(ctx);
    DrawGraphCanvas(ctx);
    DrawSelectionPanel(ctx);
    if (ctx.changed) W.Commit();
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
