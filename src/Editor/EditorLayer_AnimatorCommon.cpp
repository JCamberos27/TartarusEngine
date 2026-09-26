// The Animator window's shared widgets and helpers.
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

// std::string-backed InputText without imgui_stdlib. True once the edit is committed.
bool InputName(const char* id, std::string& s, float width, bool allowEmpty) {
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

char g_stateSearch[64] = "";

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

// Removes states (and every transition touching them) from `layer`.
void DeleteStates(AnimCtx& c, AC::Layer& Ly, std::vector<int> which) {
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
    c.W.ClearSelection();
    c.changed = true;
}

} // namespace AnimatorUI
