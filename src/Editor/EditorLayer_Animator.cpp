// #175 Part B - the Animator Controller component's Inspector: pick / create a .controller,
// watch and poke its parameters while playing, and edit its parameters, states and transitions
// (saved straight to the .controller file; a running Play picks the change up within a second).
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace EditorInternal;

namespace {

// std::string-backed InputText without imgui_stdlib. True once the edit is committed.
bool InputName(const char* id, std::string& s, float width) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(id, buf, sizeof(buf));
    if (ImGui::IsItemDeactivatedAfterEdit() && s != buf && buf[0] != '\0') { s = buf; return true; }
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

const char* kParamTypeLabels = "Float\0Int\0Bool\0Trigger\0";
const char* kOpLabels = "Greater\0Less\0Equals\0Not Equal\0Is True\0Is False\0";

} // namespace

void EditorLayer::DrawAnimatorControllerExtra(World& world, entt::entity entity) {
    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(entity);
    if (!ac || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;
    const auto* rc = world.Registry.try_get<RenderableComponent>(entity);
    Model* model = rc ? rc->ModelRef.get() : nullptr;
    if (!model || !model->HasAnimations())
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                           "Needs a Mesh Renderer with a rigged, animated model.");

    // --- Controller file picker + New ---------------------------------------------------------
    ImGui::TextUnformatted("Controller");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.35f);
    const float newW = ImGui::CalcTextSize(ICON_FA_PLUS " New").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - newW - ImGui::GetStyle().ItemSpacing.x);
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
        AnimatorController c;
        if (model) {
            std::vector<std::string> names;
            for (const auto& [ref, label] : ClipChoices(*model, assets)) {
                AnimatorController::State s;
                std::string nm = label;
                std::replace(nm.begin(), nm.end(), '/', '-');
                s.Name = UniqueName(nm, names);
                s.Clip = ref;
                names.push_back(s.Name);
                c.States.push_back(std::move(s));
            }
        }
        if (c.States.empty()) c.States.push_back({"Idle", "", 1.0f, true});
        c.DefaultState = c.States.front().Name;
        const auto* nc = world.Registry.try_get<NameComponent>(entity);
        std::string base = nc && !nc->Name.empty() ? nc->Name : std::string("Animator");
        for (char& ch : base) if (std::strchr("<>:\"/\\|?*", ch)) ch = '_';
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(ProjectPaths::Resolve("animators")), ec);
        std::string rel = "animators/" + base + ".controller";
        for (int n = 2; std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve(rel)), ec); ++n)
            rel = "animators/" + base + " " + std::to_string(n) + ".controller";
        if (c.SaveFile(ProjectPaths::Resolve(rel))) {
            PushUndo(world, "New Animator Controller");
            ac->Controller = rel;
            Log::Info("Created Animator Controller " + rel + " with " + std::to_string(c.States.size()) + " state(s).");
        } else {
            Log::Error("Couldn't write " + rel + ".");
        }
    }
    if (ac->Controller.empty()) return;

    // --- Load the working copy (re-read when the file changes under us) ------------------------
    const std::string abs = ProjectPaths::Resolve(ac->Controller);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(std::filesystem::u8path(abs), ec);
    if (ec) {
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", "The controller file is missing.");
        return;
    }
    if (m_CtrlEditPath != abs || m_CtrlEditStamp != stamp) {
        std::string err;
        AnimatorController loaded;
        if (!AnimatorController::LoadFile(abs, loaded, &err)) {
            ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  Can't read it: %s", err.c_str());
            return;
        }
        m_CtrlEdit = std::move(loaded);
        m_CtrlEditPath = abs;
        m_CtrlEditStamp = stamp;
    }
    AnimatorController& c = m_CtrlEdit;
    bool changed = false;

    // --- Live view while playing: current state + parameters you can poke ---------------------
    if (m_InPlayMode && ac->Started) {
        ImGui::SeparatorText("Live");
        const float len = model && model->CurrentAnimation() >= 0 ? model->AnimationLength(model->CurrentAnimation()) : 0.0f;
        ImGui::Text("State: %s", ac->StateName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f%%)", len > 0.0f ? 100.0f * ac->StateTime / len : 0.0f);
        for (auto& p : ac->Params) {
            ImGui::PushID(p.Name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
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
    }

    // --- Parameters -----------------------------------------------------------------------------
    if (ImGui::TreeNodeEx("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        int remove = -1;
        for (int i = 0; i < (int)c.Parameters.size(); ++i) {
            auto& p = c.Parameters[i];
            ImGui::PushID(i);
            if (RemoveButton("Remove this parameter")) remove = i;
            ImGui::SameLine();
            const std::string before = p.Name;
            if (InputName("##pname", p.Name, ImGui::GetContentRegionAvail().x * 0.4f)) {
                for (auto& t : c.Transitions) for (auto& cond : t.Conditions) if (cond.Param == before) cond.Param = p.Name;
                changed = true;
            }
            ImGui::SameLine();
            int type = (int)p.Type;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::Combo("##ptype", &type, kParamTypeLabels)) { p.Type = (AnimatorController::ParamType)type; changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (p.Type == AnimatorController::ParamType::Bool) {
                bool v = p.Default > 0.5f;
                if (EditorUIPrimitives::Checkbox("##pdef", &v)) { p.Default = v ? 1.0f : 0.0f; changed = true; }
            } else if (p.Type != AnimatorController::ParamType::Trigger) {
                ImGui::DragFloat("##pdef", &p.Default, 0.05f);
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            ImGui::PopID();
        }
        if (remove >= 0) { c.Parameters.erase(c.Parameters.begin() + remove); changed = true; }
        if (ActionButton(ICON_FA_PLUS " Parameter", "Add a parameter game code can set (SetFloat / SetBool / SetTrigger)")) {
            std::vector<std::string> names;
            for (const auto& p : c.Parameters) names.push_back(p.Name);
            c.Parameters.push_back({UniqueName("Param", names), AnimatorController::ParamType::Float, 0.0f});
            changed = true;
        }
        ImGui::TreePop();
    }

    // --- States ---------------------------------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> clips;
    if (model) clips = ClipChoices(*model, assets);
    if (ImGui::TreeNodeEx("States", ImGuiTreeNodeFlags_DefaultOpen)) {
        int remove = -1;
        for (int i = 0; i < (int)c.States.size(); ++i) {
            auto& s = c.States[i];
            ImGui::PushID(i);
            if (RemoveButton("Remove this state and its transitions")) remove = i;
            ImGui::SameLine();
            const bool isDefault = c.DefaultStateIndex() == i;
            if (ActionButton(isDefault ? ICON_FA_STAR : ICON_FA_CIRCLE, isDefault ? "The default (starting) state" : "Make this the default state",
                             isDefault, ImVec2(ImGui::GetFrameHeight(), 0.0f)) && !isDefault) {
                c.DefaultState = s.Name;
                changed = true;
            }
            ImGui::SameLine();
            const std::string before = s.Name;
            if (InputName("##sname", s.Name, ImGui::GetContentRegionAvail().x * 0.3f)) {
                for (auto& t : c.Transitions) {
                    if (t.From == before) t.From = s.Name;
                    if (t.To == before) t.To = s.Name;
                }
                if (c.DefaultState == before) c.DefaultState = s.Name;
                changed = true;
            }
            ImGui::SameLine();
            std::string clipLabel = s.Clip.empty() ? std::string("(first clip)") : s.Clip;
            for (const auto& [ref, label] : clips) if (ref == s.Clip) clipLabel = label;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            if (ImGui::BeginCombo("##sclip", clipLabel.c_str())) {
                for (const auto& [ref, label] : clips)
                    if (ImGui::Selectable(label.c_str(), ref == s.Clip)) { s.Clip = ref; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::DragFloat("##sspeed", &s.Speed, 0.01f, -10.0f, 10.0f, "x%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Playback speed");
            ImGui::SameLine();
            if (EditorUIPrimitives::Checkbox("Loop", &s.Loop)) changed = true;
            ImGui::PopID();
        }
        if (remove >= 0) {
            const std::string gone = c.States[remove].Name;
            c.States.erase(c.States.begin() + remove);
            c.Transitions.erase(std::remove_if(c.Transitions.begin(), c.Transitions.end(),
                                               [&](const auto& t) { return t.From == gone || t.To == gone; }),
                                c.Transitions.end());
            changed = true;
        }
        if (ActionButton(ICON_FA_PLUS " State", "Add a state")) {
            std::vector<std::string> names;
            for (const auto& s : c.States) names.push_back(s.Name);
            c.States.push_back({UniqueName("State", names), clips.empty() ? std::string() : clips.front().first, 1.0f, true});
            changed = true;
        }
        ImGui::TreePop();
    }

    // --- Transitions ----------------------------------------------------------------------------
    if (ImGui::TreeNodeEx("Transitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto stateCombo = [&](const char* id, std::string& value, bool allowAny) {
            bool edited = false;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            if (ImGui::BeginCombo(id, value.empty() ? "(pick)" : value.c_str())) {
                if (allowAny && ImGui::Selectable("Any State", value == AnimatorController::kAnyState)) {
                    value = AnimatorController::kAnyState;
                    edited = true;
                }
                for (const auto& s : c.States)
                    if (ImGui::Selectable(s.Name.c_str(), s.Name == value)) { value = s.Name; edited = true; }
                ImGui::EndCombo();
            }
            return edited;
        };
        int remove = -1;
        for (int i = 0; i < (int)c.Transitions.size(); ++i) {
            auto& t = c.Transitions[i];
            ImGui::PushID(i);
            if (RemoveButton("Remove this transition")) remove = i;
            ImGui::SameLine();
            changed |= stateCombo("##from", t.From, true);
            ImGui::SameLine();
            ImGui::TextUnformatted(ICON_FA_ARROW_RIGHT);
            ImGui::SameLine();
            changed |= stateCombo("##to", t.To, false);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##dur", &t.Duration, 0.01f, 0.0f, 5.0f, "fade %.2fs");
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            if (EditorUIPrimitives::Checkbox("Exit Time", &t.HasExitTime)) changed = true;
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Wait until the From clip has played this far (1 = its end) before leaving.");
            if (t.HasExitTime) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
                ImGui::DragFloat("##exit", &t.ExitTime, 0.01f, 0.0f, 10.0f, "%.2f");
                if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            }
            int removeCond = -1;
            for (int k = 0; k < (int)t.Conditions.size(); ++k) {
                auto& cond = t.Conditions[k];
                ImGui::PushID(k);
                if (RemoveButton("Remove this condition")) removeCond = k;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
                if (ImGui::BeginCombo("##cparam", cond.Param.empty() ? "(parameter)" : cond.Param.c_str())) {
                    for (const auto& p : c.Parameters)
                        if (ImGui::Selectable(p.Name.c_str(), p.Name == cond.Param)) {
                            cond.Param = p.Name;
                            // Bool / Trigger conditions only make sense as Is True / Is False.
                            if ((p.Type == AnimatorController::ParamType::Bool || p.Type == AnimatorController::ParamType::Trigger) &&
                                cond.Mode != AnimatorController::Op::If && cond.Mode != AnimatorController::Op::IfNot)
                                cond.Mode = AnimatorController::Op::If;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                int mode = (int)cond.Mode;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
                if (ImGui::Combo("##cmode", &mode, kOpLabels)) { cond.Mode = (AnimatorController::Op)mode; changed = true; }
                if (cond.Mode != AnimatorController::Op::If && cond.Mode != AnimatorController::Op::IfNot) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::DragFloat("##cthr", &cond.Threshold, 0.05f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                }
                ImGui::PopID();
            }
            if (removeCond >= 0) { t.Conditions.erase(t.Conditions.begin() + removeCond); changed = true; }
            if (ActionButton(ICON_FA_PLUS " Condition", "All conditions must hold for the transition to fire")) {
                t.Conditions.push_back({c.Parameters.empty() ? std::string() : c.Parameters.front().Name,
                                        AnimatorController::Op::Greater, 0.0f});
                changed = true;
            }
            if (t.Conditions.empty() && !t.HasExitTime)
                ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                                   "Never fires: add a condition or an exit time.");
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            ImGui::Separator();
            ImGui::PopID();
        }
        if (remove >= 0) { c.Transitions.erase(c.Transitions.begin() + remove); changed = true; }
        if (ActionButton(ICON_FA_PLUS " Transition", "Add a transition between two states")) {
            AnimatorController::Transition t;
            t.From = c.States.empty() ? std::string(AnimatorController::kAnyState) : c.States.front().Name;
            t.To = c.States.size() > 1 ? c.States[1].Name : t.From;
            c.Transitions.push_back(std::move(t));
            changed = true;
        }
        ImGui::TreePop();
    }

    if (changed) {
        if (c.SaveFile(abs)) m_CtrlEditStamp = std::filesystem::last_write_time(std::filesystem::u8path(abs), ec);
        else Log::Error("Couldn't save " + ac->Controller + ".");
    }
}
