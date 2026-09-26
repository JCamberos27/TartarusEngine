// The Animator window's left panel: layers, parameters and (while playing) the transition history.
#include "EditorLayer.h"
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
#include "EditorLayer_AnimatorInternal.h"

using namespace EditorInternal;
using namespace AnimatorUI;

namespace AnimatorUI {

// Left: layers, parameters, and (while playing) the transition history.
void DrawLayersAndParameters(AnimCtx& cx) {
    World& world = cx.world;
    AnimatorWindowState& W = cx.W;
    AC& D = W.Doc;
    AssetLibrary* assets = cx.assets;
    AnimatorControllerComponent* live = cx.live;
    Model* rig = cx.rig;
    const float S = cx.S, leftW = cx.leftW, rightW = cx.rightW, bodyH = cx.bodyH;
    bool& changed = cx.changed;
    (void)world; (void)assets; (void)live; (void)rig; (void)S; (void)leftW; (void)rightW; (void)bodyH; (void)D;
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
        // Lint: the mistakes a controller loads fine with and then quietly doesn't do what it was built to.
        if (live && ImGui::BeginTabItem("History")) {
            ImGui::TextDisabled("Every transition this object's controller took, newest first (Play).");
            if (ActionButton(ICON_FA_TRASH " Clear", "Empty the list")) live->History.clear();
            if (live->History.empty()) ImGui::TextDisabled("Nothing yet.");
            for (int i = (int)live->History.size() - 1; i >= 0; --i) {
                const auto& h = live->History[i];
                ImGui::PushID(i);
                const std::string head = std::to_string(live->Clock - h.Time).substr(0, 4) + " s ago   " + h.From + "  " ICON_FA_ARROW_RIGHT "  " + h.To;
                ImGui::TextUnformatted(head.c_str());
                const bool ok = h.Layer < (int)D.Layers.size() && h.Transition < (int)D.Layers[h.Layer].Transitions.size();
                if (ok && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ok && ImGui::IsItemClicked()) { W.Layer = h.Layer; W.ClearSelection(); W.SelTransition = h.Transition; }
                ImGui::TextDisabled("    because: %s", h.Why.c_str());
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        {
            const std::vector<AnimatorLint::Issue> issues = AnimatorLint::Check(D);
            int problems = 0;
            for (const auto& i : issues) problems += i.Severity != AnimatorLint::Level::Info;
            char tab[48];
            std::snprintf(tab, sizeof tab, problems ? "Lint (%d)###lint" : "Lint###lint", problems);
            if (ImGui::BeginTabItem(tab)) {
                static int s_against = 0; // 0 none, 1 body, 2 weapon
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::Combo("##against", &s_against, "Check the graph only\0Also check it as a first-person body controller\0Also check it as a first-person weapon controller\0");
                std::vector<FPBody::Check> contract;
                if (s_against == 1) {
                    FirstPersonBodyComponent all;
                    all.TurnThreshold = 55.0f; all.StartStopClips = true; all.CrouchHeight = 1.2f; all.FootIK = true; all.WeaponArms = true;
                    FPBody::ValidationInput in;
                    in.Config = &all;
                    in.HasPieces = in.HasDriverPiece = in.ControllerSet = true;
                    in.Controller = &D;
                    contract = FPBody::Validate(in);
                } else if (s_against == 2) {
                    FirstPersonAnimationSet set;
                    set.Controller = W.Rel;
                    FirstPersonWeaponCheckInput in;
                    in.Set = &set;
                    in.Controller = &D;
                    contract = FirstPersonWeaponValidate(in);
                }
                if (issues.empty() && contract.empty()) {
                    ImGui::TextColored(EditorUIPrimitives::SuccessColor(), ICON_FA_CIRCLE_CHECK "  Nothing to report.");
                }
                for (const auto& iss : issues) {
                    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    const char* icon = ICON_FA_CIRCLE_INFO;
                    if (iss.Severity == AnimatorLint::Level::Error) { col = EditorUIPrimitives::DangerColor(); icon = ICON_FA_CIRCLE_XMARK; }
                    else if (iss.Severity == AnimatorLint::Level::Warning) { col = EditorUIPrimitives::WarningColor(); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
                    ImGui::PushID((int)(&iss - issues.data()) + 5000);
                    ImGui::TextColored(col, "%s", icon);
                    ImGui::SameLine();
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(iss.Message.c_str());
                    ImGui::PopTextWrapPos();
                    const bool clickable = iss.State >= 0 || iss.Transition >= 0;
                    if (clickable && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (clickable && ImGui::IsItemClicked()) {
                        if (iss.Layer >= 0 && iss.Layer < (int)D.Layers.size()) W.Layer = iss.Layer;
                        W.ClearSelection();
                        if (iss.Transition >= 0) W.SelTransition = iss.Transition;
                        else W.SelStates = {iss.State};
                    }
                    if (!iss.Hint.empty()) ImGui::TextDisabled("    %s", iss.Hint.c_str());
                    ImGui::PopID();
                }
                if (!contract.empty()) {
                    ImGui::SeparatorText(s_against == 1 ? "As a body controller" : "As a weapon controller");
                    for (const auto& c : contract) {
                        ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                        const char* icon = ICON_FA_CIRCLE_INFO;
                        if (c.Level == FPBody::Severity::Error) { col = EditorUIPrimitives::DangerColor(); icon = ICON_FA_CIRCLE_XMARK; }
                        else if (c.Level == FPBody::Severity::Warning) { col = EditorUIPrimitives::WarningColor(); icon = ICON_FA_TRIANGLE_EXCLAMATION; }
                        ImGui::TextColored(col, "%s", icon);
                        ImGui::SameLine();
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(c.Message.c_str());
                        if (!c.Hint.empty()) ImGui::TextDisabled("    %s", c.Hint.c_str());
                        ImGui::PopTextWrapPos();
                    }
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine();

}

} // namespace AnimatorUI
