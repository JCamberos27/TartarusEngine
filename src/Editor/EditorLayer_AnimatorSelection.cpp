// The Animator window's right panel: the selected state's, transition's or layer's properties.
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

// Right: the selection's properties (state, transition, or the layer's defaults).
void DrawSelectionPanel(AnimCtx& cx) {
    World& world = cx.world;
    AnimatorWindowState& W = cx.W;
    AC& D = W.Doc;
    AssetLibrary* assets = cx.assets;
    AnimatorControllerComponent* live = cx.live;
    Model* rig = cx.rig;
    const float S = cx.S, leftW = cx.leftW, rightW = cx.rightW, bodyH = cx.bodyH;
    bool& changed = cx.changed;
    (void)world; (void)assets; (void)live; (void)rig; (void)S; (void)leftW; (void)rightW; (void)bodyH; (void)D;
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
    // Length in seconds of a clip on the chosen rig (0 when there is none or it does not resolve).
    auto clipLength = [&](const std::string& r) {
        if (r.empty() || !assets || W.Entity == entt::null || !world.Registry.valid(W.Entity)) return 0.0f;
        const auto* rc = world.Registry.try_get<RenderableComponent>(W.Entity);
        if (!rc || !rc->ModelRef) return 0.0f;
        const int c = ResolveAnimationClip(*rc->ModelRef, r, *assets);
        return c >= 0 ? rc->ModelRef->AnimationLength(c) : 0.0f;
    };
    // Ground speed a clip travels at on the chosen rig, in m/s (-1 when it can't be measured).
    auto clipSpeed = [&](const std::string& r) {
        if (r.empty() || !assets || W.Entity == entt::null || !world.Registry.valid(W.Entity)) return -1.0f;
        const auto* rc = world.Registry.try_get<RenderableComponent>(W.Entity);
        if (!rc || !rc->ModelRef) return -1.0f;
        Model& mdl = *rc->ModelRef;
        const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(W.Entity);
        const int node = mdl.FindRootMotionNode(ac ? ac->RootMotion.Bone : std::string());
        const int c = ResolveAnimationClip(mdl, r, *assets);
        const float len = c >= 0 ? mdl.AnimationLength(c) : 0.0f;
        if (node < 0 || !(len > 0.0f)) return -1.0f;
        RootMotionSettings rms;
        if (ac) { rms.Rotation = ac->RootMotion.Rotation; rms.Vertical = ac->RootMotion.Vertical; }
        const RootMotionDelta d = mdl.ClipRootMotion(c, 0.0f, len, AnimationWrapMode::ClampForever, node, rms);
        return glm::length(glm::vec2(d.Translation.x, d.Translation.z)) / len;
    };
    auto clipSlot = [&](const char* id, std::string& ref) {
        bool edited = false;
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string preview = ClipLabel(ref);
        if (const float len = clipLength(ref); len > 0.0f) { char b[24]; std::snprintf(b, sizeof b, "  (%.2f s)", len); preview += b; }
        if (ImGui::BeginCombo("##clip", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            static char filter[128] = "";
            ClipFilterBox(filter, sizeof filter);
            if (!filter[0] && ImGui::Selectable("(none)", ref.empty())) { ref.clear(); edited = true; }
            for (const auto& [r, label] : W.Clips)
                if (ClipFilterMatch(filter, label + " " + r)) {
                    if (ImGui::Selectable((label + "##" + r).c_str(), r == ref)) { ref = r; edited = true; }
                    if (const float len = clipLength(r); len > 0.0f) {
                        char b[16];
                        std::snprintf(b, sizeof b, "%.2f s", len);
                        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(b).x - 8.0f);
                        ImGui::TextDisabled("%s", b);
                    }
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
                // Full measurement of one clip: travel, foot contacts, stride, loop seam and a speed plot.
                auto analyse = [&](const std::string& ref, const char* label) {
                    const int c = ResolveAnimationClip(*rigModel, ref, *assets);
                    if (c < 0) return;
                    struct Cached { const Model* M = nullptr; int Clip = -1, Node = -1; bool Rot = false, Vert = false; ClipAnalysis::Result R; };
                    static std::unordered_map<std::string, Cached> s_cache;
                    Cached& e = s_cache[std::string(label) + "|" + ref];
                    if (e.M != rigModel || e.Clip != c || e.Node != node || e.Rot != rms.Rotation || e.Vert != rms.Vertical) {
                        const std::string feet[2] = {FPBody::kBoneFoot[0], FPBody::kBoneFoot[1]};
                        std::vector<std::string> seam = {FPBody::kBoneFoot[0], FPBody::kBoneFoot[1], FPBody::kBoneHand[0], FPBody::kBoneHand[1], "head"};
                        e = {rigModel, c, node, rms.Rotation, rms.Vertical, ClipAnalysis::Analyze(*rigModel, c, node, rms, feet, seam)};
                    }
                    const ClipAnalysis::Result& a = e.R;
                    if (!a.Valid) return;
                    ImGui::PushID(label);
                    if (ImGui::TreeNode("##analysis", ICON_FA_CHART_LINE "  Analyse %s", label[0] ? label : "clip")) {
                        ImGui::Text("Length %.2f s   travel %.2f m   speed %.2f m/s   turn %+.0f deg", a.Length, a.Distance, a.Speed, a.YawDegrees);
                        if (std::abs(a.VerticalDelta) > 0.01f) ImGui::Text("Net height change %+.2f m", a.VerticalDelta);
                        if (a.LoopSeam > 0.0f) {
                            const bool bad = a.LoopSeam > 0.03f;
                            ImGui::TextColored(bad ? EditorUIPrimitives::WarningColor() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                                               "Loop seam %.1f cm (%s)%s", a.LoopSeam * 100.0f, a.LoopSeamBone.c_str(), bad ? "  - visible pop when it loops" : "");
                        }
                        ImGui::PlotLines("##speed", a.SpeedProfile.data(), (int)a.SpeedProfile.size(), 0, "ground speed over the clip", 0.0f,
                                         std::max(0.5f, 1.15f * *std::max_element(a.SpeedProfile.begin(), a.SpeedProfile.end())), ImVec2(-FLT_MIN, 50.0f));
                        // Contact strips: filled where the foot is planted.
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        const float w = ImGui::GetContentRegionAvail().x, h = 10.0f;
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        for (int f = 0; f < 2; ++f) {
                            const ClipAnalysis::Foot& ft = a.Feet[f];
                            const float y = origin.y + f * (h + 3.0f);
                            dl->AddRectFilled({origin.x, y}, {origin.x + w, y + h}, IM_COL32(60, 60, 60, 160));
                            if (!ft.Found) continue;
                            // Draw each contact as a bar from its start for ContactFraction / plants of the clip.
                            const float each = ft.ContactStarts.empty() ? 0.0f : ft.ContactFraction / ft.ContactStarts.size();
                            for (float st : ft.ContactStarts) {
                                const float x0 = origin.x + st * w, x1 = origin.x + std::min(1.0f, st + each) * w;
                                dl->AddRectFilled({x0, y}, {x1, y + h}, f == 0 ? IM_COL32(90, 170, 255, 220) : IM_COL32(255, 170, 90, 220));
                                if (st + each > 1.0f) dl->AddRectFilled({origin.x, y}, {origin.x + (st + each - 1.0f) * w, y + h}, f == 0 ? IM_COL32(90, 170, 255, 220) : IM_COL32(255, 170, 90, 220));
                            }
                        }
                        ImGui::Dummy({w, 2 * h + 3.0f});
                        for (int f = 0; f < 2; ++f) {
                            const ClipAnalysis::Foot& ft = a.Feet[f];
                            if (!ft.Found) { ImGui::TextDisabled("%s: bone not found on this rig", ft.Bone.c_str()); continue; }
                            std::string starts;
                            for (float st : ft.ContactStarts) { char b[16]; std::snprintf(b, sizeof b, "%s%.2f", starts.empty() ? "" : ", ", st); starts += b; }
                            ImGui::Text("%s: plants at %s (of 1.0), down %.0f%%, stride %.2f m", ft.Bone.c_str(), starts.empty() ? "-" : starts.c_str(), ft.ContactFraction * 100.0f, ft.Stride);
                        }
                        ImGui::TextDisabled("Blue = left foot planted, orange = right. Use the plant times for Stop transition offsets \nand Start exit times; the stride against the speed tells if the feet will slide.");
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                // Preview (Edit mode): pose the rig in this state's motion and scrub or play it. A blend tree is
                // shown at the parameter values below (their defaults to start with).
                if (!live) {
                    struct Preview { bool On = false, Play = false; float T = 0.0f; std::string Key; std::map<std::string, float> Params; };
                    static Preview pv;
                    const std::string key = W.Rel + "|" + s.Name + "|" + std::to_string(D.TrackIndex(rigAc ? rigAc->Track : std::string()));
                    if (pv.Key != key) { pv.Key = key; pv.Params.clear(); pv.T = 0.0f; }
                    ImGui::PushID("preview");
                    row("Preview");
                    if (EditorUIPrimitives::Checkbox("##pvon", &pv.On)) {
                        if (!pv.On) { pv.Play = false; rigModel->StopAnimation(); }
                    }
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Pose the rig in this state's motion so you can scrub it (Edit mode). Off restores the bind pose.");
                    if (pv.On) {
                        ImGui::SameLine();
                        if (ActionButton(pv.Play ? ICON_FA_PAUSE : ICON_FA_PLAY, pv.Play ? "Pause" : "Play")) pv.Play = !pv.Play;
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::SliderFloat("##pvt", &pv.T, 0.0f, 1.0f, "%.2f of the state");
                        // Blend-tree parameters.
                        auto paramOf = [&](const std::string& name) {
                            auto it = pv.Params.find(name);
                            if (it != pv.Params.end()) return it->second;
                            const auto* prm = D.FindParameter(name);
                            return prm ? prm->Default : 0.0f;
                        };
                        if (m.IsBlendTree()) {
                            for (const std::string* pn : {&m.BlendParam, &m.BlendParamY}) {
                                if (pn->empty()) continue;
                                float v = paramOf(*pn);
                                row(pn->c_str());
                                ImGui::SetNextItemWidth(-FLT_MIN);
                                ImGui::PushID(pn->c_str());
                                if (ImGui::DragFloat("##pvp", &v, 0.02f)) pv.Params[*pn] = v;
                                ImGui::PopID();
                            }
                        }
                        // The pose: each contributing clip at its own length times T (phase-synced), blended.
                        std::vector<float> weights;
                        std::vector<std::string> refs;
                        if (!m.IsBlendTree()) { weights = {1.0f}; refs = {m.Clip}; }
                        else {
                            weights = m.Is2D() ? AnimatorBlendWeights2D(m.Children, paramOf(m.BlendParam), paramOf(m.BlendParamY))
                                               : AnimatorBlendWeights(m.Children, paramOf(m.BlendParam));
                            for (const auto& ch : m.Children) refs.push_back(ch.Clip);
                        }
                        std::vector<LocalTRS> pose, tmp;
                        rigModel->BindLocalPose(pose);
                        float acc = 0.0f;
                        float lenForPlay = 0.0f;
                        for (size_t i = 0; i < refs.size() && i < weights.size(); ++i) {
                            if (weights[i] <= 0.0f) continue;
                            const int c = ResolveAnimationClip(*rigModel, refs[i], *assets);
                            const float len = c >= 0 ? rigModel->AnimationLength(c) : 0.0f;
                            if (!(len > 0.0f) || !rigModel->SampleLocalPose(c, pv.T * len, AnimationWrapMode::ClampForever, tmp)) continue;
                            if (s.RootMotion) rigModel->StripRootMotion(tmp, c, node, rms);
                            acc += weights[i];
                            const float w = weights[i] / acc; // running weighted average
                            if (acc == weights[i]) pose = tmp;
                            else for (size_t b = 0; b < pose.size() && b < tmp.size(); ++b) pose[b] = LocalTRS::Blend(pose[b], tmp[b], w);
                            lenForPlay = std::max(lenForPlay, len);
                        }
                        if (acc > 0.0f) rigModel->ApplyLocalPose(pose);
                        if (pv.Play && lenForPlay > 0.0f) {
                            pv.T += ImGui::GetIO().DeltaTime * std::max(0.05f, s.Speed) / lenForPlay;
                            if (pv.T > 1.0f) pv.T -= 1.0f;
                        }
                    }
                    ImGui::PopID();
                }
                row("");
                if (!m.IsBlendTree()) { describe(m.Clip, ""); row(""); analyse(m.Clip, ""); }
                else {
                    ImGui::TextDisabled("Per child:");
                    for (const auto& child : m.Children) {
                        row("");
                        const std::string prefix = ClipLabel(child.Clip) + ": ";
                        describe(child.Clip, prefix.c_str());
                        row("");
                        analyse(child.Clip, ClipLabel(child.Clip).c_str());
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
                if (!is2D) {
                    ImGui::SameLine();
                    if (ActionButton(ICON_FA_ARROW_DOWN_1_9 " Sort", "Order the children by threshold")) {
                        std::stable_sort(m.Children.begin(), m.Children.end(),
                                         [](const AC::BlendChild& a, const AC::BlendChild& b) { return a.Threshold < b.Threshold; });
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ActionButton(ICON_FA_GAUGE " Thresholds from clip speed",
                                     "Set each child's threshold to the ground speed its clip travels at, measured on the chosen rig\n"
                                     "(clips that stay in place or can't be measured keep theirs). For a tree driven by a speed parameter.")) {
                        for (auto& ch : m.Children) {
                            const float v = clipSpeed(ch.Clip);
                            if (v >= 0.05f) ch.Threshold = std::round(v * 100.0f) / 100.0f;
                        }
                        changed = true;
                    }
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
                if (is2D && !m.Children.empty()) {
                    // The blend space: children as dots (bigger = more weight), the current point as a cross.
                    // Dragging in it steers the live parameters in Play.
                    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    for (const auto& ch : m.Children) {
                        x0 = std::min(x0, ch.Threshold); x1 = std::max(x1, ch.Threshold);
                        y0 = std::min(y0, ch.ThresholdY); y1 = std::max(y1, ch.ThresholdY);
                    }
                    x0 = std::min(x0, v); x1 = std::max(x1, v); y0 = std::min(y0, vy); y1 = std::max(y1, vy);
                    const float padX = std::max(0.5f, (x1 - x0) * 0.15f), padY = std::max(0.5f, (y1 - y0) * 0.15f);
                    x0 -= padX; x1 += padX; y0 -= padY; y1 += padY;
                    const float side = std::min(ImGui::GetContentRegionAvail().x, 260.0f * S);
                    const ImVec2 o = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##space", ImVec2(side, side));
                    ImDrawList* pl = ImGui::GetWindowDrawList();
                    auto spaceToScreen = [&](float px, float py) {
                        return ImVec2(o.x + (px - x0) / (x1 - x0) * side, o.y + (1.0f - (py - y0) / (y1 - y0)) * side);
                    };
                    pl->AddRectFilled(o, o + ImVec2(side, side), IM_COL32(24, 24, 26, 255));
                    pl->AddRect(o, o + ImVec2(side, side), IM_COL32(90, 90, 96, 255));
                    if (x0 < 0 && x1 > 0) pl->AddLine(spaceToScreen(0, y0), spaceToScreen(0, y1), IM_COL32(255, 255, 255, 30));
                    if (y0 < 0 && y1 > 0) pl->AddLine(spaceToScreen(x0, 0), spaceToScreen(x1, 0), IM_COL32(255, 255, 255, 30));
                    for (size_t k = 0; k < m.Children.size(); ++k) {
                        const ImVec2 c = spaceToScreen(m.Children[k].Threshold, m.Children[k].ThresholdY);
                        pl->AddCircleFilled(c, 3.0f + 9.0f * w[k], IM_COL32(70, 140, 230, 200));
                        pl->AddText(c + ImVec2(6, -14), IM_COL32(210, 210, 215, 255), ClipLabel(m.Children[k].Clip).c_str());
                    }
                    const ImVec2 cur = spaceToScreen(v, vy);
                    pl->AddLine(cur - ImVec2(6, 0), cur + ImVec2(6, 0), IM_COL32(255, 210, 60, 255), 2.0f);
                    pl->AddLine(cur - ImVec2(0, 6), cur + ImVec2(0, 6), IM_COL32(255, 210, 60, 255), 2.0f);
                    if (live && ImGui::IsItemActive()) {
                        const ImVec2 mp = ImGui::GetIO().MousePos;
                        live->SetFloat(m.BlendParam, x0 + (mp.x - o.x) / side * (x1 - x0));
                        live->SetFloat(m.BlendParamY, y0 + (1.0f - (mp.y - o.y) / side) * (y1 - y0));
                    }
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip(live ? "Drag to steer the live parameters." : "Children as dots, the current parameter point as a cross.\nDrag in Play to steer.");
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
        ImGui::TextDisabled("%d states selected. Changes below apply to all of them.", (int)W.SelStates.size());
        auto each = [&](const std::function<void(AC::State&)>& f) {
            for (int i : W.SelStates)
                if (i >= 0 && i < (int)L.States.size()) f(L.States[i]);
            changed = true;
        };
        static float s_speed = 1.0f;
        static int s_priority = 0;
        static char s_tag[64] = "";
        ImGui::SeparatorText("Playback");
        row("Speed");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f * S);
        ImGui::DragFloat("##bspeed", &s_speed, 0.01f, 0.0f, 10.0f, "x%.2f");
        ImGui::SameLine();
        if (ActionButton("Apply##bs", "Set every selected state's Speed")) each([&](AC::State& st) { st.Speed = s_speed; });
        row("Loop");
        if (ActionButton("On##bl", "Loop every selected state")) each([](AC::State& st) { st.Loop = true; });
        ImGui::SameLine();
        if (ActionButton("Off##bl", "Stop looping every selected state")) each([](AC::State& st) { st.Loop = false; });
        row("Root Motion");
        if (ActionButton("On##br", "Let every selected state move the object by root motion")) each([](AC::State& st) { st.RootMotion = true; });
        ImGui::SameLine();
        if (ActionButton("Off##br", "Keep every selected state's travel in the pose")) each([](AC::State& st) { st.RootMotion = false; });
        row("Priority");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f * S);
        ImGui::InputInt("##bprio", &s_priority);
        ImGui::SameLine();
        if (ActionButton("Apply##bp", "Set every selected state's Priority")) each([&](AC::State& st) { st.Priority = s_priority; });
        ImGui::SeparatorText("Tags");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##btag", "tag name", s_tag, sizeof s_tag);
        const bool haveTag = s_tag[0] != 0;
        ImGui::BeginDisabled(!haveTag);
        if (ActionButton(ICON_FA_PLUS " Add to all", "Give every selected state this tag")) {
            each([&](AC::State& st) {
                if (std::find(st.Tags.begin(), st.Tags.end(), std::string(s_tag)) == st.Tags.end()) st.Tags.push_back(s_tag);
            });
        }
        ImGui::SameLine();
        if (ActionButton(ICON_FA_MINUS " Remove from all", "Take this tag off every selected state"))
            each([&](AC::State& st) { st.Tags.erase(std::remove(st.Tags.begin(), st.Tags.end(), std::string(s_tag)), st.Tags.end()); });
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (ActionButton(ICON_FA_TRASH " Delete", "Delete the selected states and their transitions")) DeleteStates(cx, L, W.SelStates);
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
            // Preview (Edit mode, needs a rig): the pose at a point in the crossfade - the source where it leaves
            // (its exit time, or its start), blended into the destination from its Offset. Blend trees at their defaults.
            if (!live && tr.FromKind == AC::Source::State && tr.To != AC::kExitState && W.Entity != entt::null && world.Registry.valid(W.Entity) && assets) {
                Model* pm = nullptr;
                if (auto* rc = world.Registry.try_get<RenderableComponent>(W.Entity)) pm = rc->ModelRef.get();
                const auto* pac = world.Registry.try_get<AnimatorControllerComponent>(W.Entity);
                const int fi = L.FindState(tr.From), ti = L.FindState(tr.To);
                const int pnode = pm ? pm->FindRootMotionNode(pac ? pac->RootMotion.Bone : std::string()) : -1;
                if (pm && fi >= 0 && ti >= 0) {
                    static bool s_on = false;
                    static float s_t = 50.0f; // percent
                    static std::string s_key;
                    const std::string key = W.Rel + "|" + std::to_string(W.SelTransition);
                    if (s_key != key) { s_key = key; s_t = 50.0f; }
                    ImGui::PushID("tpreview");
                    row("Preview");
                    if (EditorUIPrimitives::Checkbox("##tpon", &s_on) && !s_on) pm->StopAnimation();
                    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Pose the rig partway through this crossfade. Off restores the bind pose.");
                    if (s_on) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::SliderFloat("##tpt", &s_t, 0.0f, 100.0f, "%.0f%% through the fade");
                        const int track = D.TrackIndex(pac ? pac->Track : std::string());
                        RootMotionSettings rms;
                        if (pac) { rms.Rotation = pac->RootMotion.Rotation; rms.Vertical = pac->RootMotion.Vertical; }
                        std::vector<LocalTRS> a, b;
                        const std::map<std::string, float> none;
                        const float fromPhase = tr.HasExitTime ? std::min(tr.ExitTime, 1.0f) : 0.0f;
                        const bool haveA = SampleStatePose(*pm, *assets, D, L.States[fi], track, fromPhase, none, pnode, rms, a);
                        // The destination has advanced by the fade so far: t * Duration seconds of its own length.
                        float destLen = 0.0f;
                        {
                            const auto& dm = L.States[ti].MotionFor(track);
                            const std::string ref = dm.IsBlendTree() && !dm.Children.empty() ? dm.Children.front().Clip : dm.Clip;
                            const int c = ref.empty() ? -1 : ResolveAnimationClip(*pm, ref, *assets);
                            destLen = c >= 0 ? pm->AnimationLength(c) : 0.0f;
                        }
                        const float tf = s_t / 100.0f;
                        const float advance = destLen > 0.0f ? tf * tr.Duration / (destLen / std::max(0.01f, L.States[ti].Speed)) : 0.0f;
                        const bool haveB = SampleStatePose(*pm, *assets, D, L.States[ti], track, tr.Offset + advance, none, pnode, rms, b);
                        if (haveA && haveB) {
                            for (size_t k = 0; k < a.size() && k < b.size(); ++k) a[k] = LocalTRS::Blend(a[k], b[k], tf);
                            pm->ApplyLocalPose(a);
                        } else if (haveA || haveB) {
                            pm->ApplyLocalPose(haveA ? a : b);
                        }
                        ImGui::TextDisabled("%s at %.2f  " ICON_FA_ARROW_RIGHT "  %s from %.2f", tr.From.c_str(), fromPhase, tr.To.c_str(), tr.Offset);
                    }
                    ImGui::PopID();
                }
            }
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

}

} // namespace AnimatorUI
