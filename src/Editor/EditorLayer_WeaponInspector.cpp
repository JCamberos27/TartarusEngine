// The weapon definition (.fpsanim) Inspector.
//
// One collapsing section per job - Animation, Aim-Down-Sights, Gameplay, Barrel & Laser, Rigs &
// Mount, Recoil, Movement, IK - under an overview card that says at a glance whether the weapon is wired up
// (controller, socket, IK bones, ADS actions). Every committed edit is saved straight to the
// file, where a running Play picks it up, and goes on the Inspector's own undo stack (the arrows
// in the overview card).
#include "SetupChecksUI.h"
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorPropertyRows.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "CurveEditor.h"
#include "FirstPersonAdsCarry.h"
#include "FirstPersonAnimation.h"
#include "Log.h"
#include "Model.h"
#include "ProjectPaths.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace EditorInternal;
namespace fs = std::filesystem;
namespace K = FirstPersonAnimatorContract;
using Status = PropertyRows::Status;

namespace {

// What the sections can offer as choices and check against.
struct WeaponContext {
    std::shared_ptr<const AnimatorController> Controller;
    std::vector<std::string> States;         // base layer
    std::vector<std::string> Tags;           // used in the controller, plus the known ones
    std::vector<std::string> StatesAndTags;
    const Model* Arms = nullptr;             // null until the arms rig is loaded
    std::vector<std::string> Bones;
    bool CanLoadArms = false;
    bool LoadArms = false;                   // out: the user asked to load it
};

void SortUnique(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// A curve row: the label above a full-width curve editor.
void CurveRow(PropertyRows& r, const char* label, const char* id, Curve& c, const Curve& def, const char* fmt,
              float presetAmp, const char* tip) {
    ImGui::TextUnformatted(label);
    if (tip && ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
    CurveEditor::Options o;
    o.ValueFormat = fmt;
    o.PresetAmplitude = presetAmp;
    o.Default = &def;
    if (CurveEditor::Draw(id, c, ImVec2(0.0f, ImGui::GetFontSize() * 5.0f), o)) r.MarkChanged();
}

void Curve3Rows(PropertyRows& r, const char* id, FirstPersonCurve3& c, const FirstPersonCurve3& def, const char* xl,
                const char* yl, const char* zl, const char* fmtXY, const char* fmtZ, float ampXY, float ampZ, const char* tip) {
    ImGui::PushID(id);
    CurveRow(r, xl, "##x", c.X, def.X, fmtXY, ampXY, tip);
    CurveRow(r, yl, "##y", c.Y, def.Y, fmtXY, ampXY, tip);
    CurveRow(r, zl, "##z", c.Z, def.Z, fmtZ, ampZ, tip);
    ImGui::PopID();
}

// A shot's curve that doesn't come back to 0 by the end of the shot snaps the gun when the shot
// expires.
void EndsAtZero(const char* what, const Curve& c) {
    if (c.Empty()) return;
    float peak = 0.0f;
    for (const CurveKey& k : c.Keys) peak = std::max(peak, std::fabs(k.Value));
    const float end = c.Evaluate(1.0f);
    if (std::fabs(end) > std::max(peak * 0.02f, 1e-5f)) {
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s ends at %g, not 0: it snaps back as each shot ends", what, end);
        PropertyRows::Badge(Status::Warning, msg);
    }
}

bool Tree(const char* label, bool defaultOpen = false) {
    return ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth | (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
}

// --- Aim-Down-Sights ---------------------------------------------------------------------------

// How a state plays with the sights up.
enum class AdsMode { None, Reference, Authored, Carried };

AdsMode ModeOf(const AnimatorController::State& s, const FirstPersonAnimationSet& set, const std::string& reference) {
    if (s.Name == reference) return AdsMode::Reference;
    if (s.HasTag(K::kTagAds)) return AdsMode::Authored;
    if (s.HasTag(set.Ads.CarryTag)) return AdsMode::Carried;
    return AdsMode::None;
}

void DrawAds(PropertyRows& r, FirstPersonAnimationSet& s, const WeaponContext& ctx, const std::string& path) {
    static const WeaponProceduralSettings kDefaults = WeaponProceduralSettings::Defaults();
    static const FirstPersonAdsSettings kAdsDefaults{};
    FirstPersonAdsSettings& ads = s.Ads;

    r.Heading("View");
    r.Float("Zoom", ads.Zoom, 0.01f, 1.0f, 8.0f, "%.2fx", "How much the world magnifies with the sights up (1 = none). Mouse look slows to match.");
    r.Float("Gun Zoom", ads.ViewModelZoom, 0.01f, 1.0f, 4.0f, "%.2fx", "How much the gun itself magnifies with the sights up (1 = none).");
    r.Float("Zoom Time", ads.ZoomTime, 0.01f, 0.0f, 2.0f, "%.2f s", "Roughly how long the zoom takes to settle in or out (eased at both ends).");

    r.Heading("Sight Alignment");
    auto& aim = s.Procedural.Aim;
    r.Vec3("Position", aim.Position, 0.0005f, "%.4f", "Metres added to the gun with the sights up, on top of the aim clip.\n"
                                                      "Zero keeps the clip's sight picture exactly.");
    r.Vec3("Rotation", aim.Rotation, 0.05f, "%.2f", "Degrees (pitch, yaw, roll) added with the sights up.", "PYR");
    r.Float("Blend Time", aim.BlendTime, 0.005f, 0.0f, 2.0f, "%.3f s", "Seconds to blend that offset (and the ADS sway / bob / breathing scales) in and out.");
    if (Tree("Blend Easing")) {
        CurveRow(r, "Weight over the blend", "##aiblend", aim.Blend, kDefaults.Aim.Blend, "%.2f", 1.0f,
                 "Maps the 0..1 blend to the weight actually applied. Should run from 0 to 1.");
        ImGui::TreePop();
    }

    r.Heading("Actions");
    r.Note("Reloads, the mag check and other actions can play with the sights up two ways: a real ADS clip "
           "(a state tagged ADS), or the hip clip carried onto the sights (a state with the carry tag).");
    r.Name("Aim Pose", ads.ReferenceState, ctx.States, !ctx.States.empty(),
           "The aiming state carried actions are measured against. Empty = the first state tagged ADS.",
           "The controller has no state named '%s'.");
    r.Name("Carry Tag", ads.CarryTag, ctx.Tags, false,
           "States with this tag have their hip clip carried onto the sights while aiming.");
    r.Check("Match Elbows", ads.MatchElbows, "Swing the elbows so a carried action starts and ends exactly in the aim pose.");
    r.Check("Match Twist", ads.MatchTwist, "Also match the forearm / upper-arm twist bones, so the wrists don't re-roll after the action.");
    r.Float("Aim Hold Time", ads.AimHoldTime, 0.005f, 0.0f, 2.0f, "%.2f s",
            "Seconds for a carried action to rise onto (or drop off) the sights when aim is pressed or released partway through.");
    {
        // Typed as a comma-separated list.
        std::string list;
        for (const std::string& bone : ads.ActionBones) list += (list.empty() ? "" : ", ") + bone;
        if (r.Text("Action Bones", list,
                   "The bones a carried action plays on while aiming, each with everything under it (clavicle_l = "
                   "the left arm). The rest of the rig - the gun and the other hand - stays in the aim pose, and the "
                   "action's hand keeps its grip relative to the gun. Empty = the whole clip is carried onto the "
                   "sights instead (the gun moves as it does at the hip).")) {
            ads.ActionBones.clear();
            size_t start = 0;
            while (start <= list.size()) {
                size_t end = list.find(',', start);
                if (end == std::string::npos) end = list.size();
                std::string bone = list.substr(start, end - start);
                bone.erase(0, bone.find_first_not_of(" \t"));
                bone.erase(bone.find_last_not_of(" \t") + 1);
                if (!bone.empty()) ads.ActionBones.push_back(bone);
                start = end + 1;
            }
        }
    }

    // Every action-like state and how it plays with the sights up.
    const AdsCarryReport* report = FindAdsCarryReport(path);
    std::string reference = ads.ReferenceState;
    if (ctx.Controller && !ctx.Controller->Layers.empty()) {
        const auto& L = ctx.Controller->Layers[0];
        if (reference.empty())
            for (const auto& st : L.States)
                if (st.HasTag(K::kTagAds)) { reference = st.Name; break; }
        ImGui::Spacing();
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_PadOuterX;
        const bool holds = !ads.ActionBones.empty();
        if (ImGui::BeginTable("##adsactions", holds ? 4 : 3, flags)) {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("With the sights up", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            if (holds) ImGui::TableSetupColumn("Gun Motion", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Measured", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableHeadersRow();
            int rows = 0;
            for (const auto& st : L.States) {
                const AdsMode mode = ModeOf(st, s, reference);
                const bool action = st.HasTag(K::kTagReload) || st.HasTag(K::kTagBusy);
                if (mode == AdsMode::None && !action) continue;
                ++rows;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(st.Name.c_str());
                ImGui::TableNextColumn();
                const AdsCarryReport::Entry* entry = nullptr;
                if (report)
                    for (const auto& e : report->Entries)
                        if (e.State == st.Name) entry = &e;
                switch (mode) {
                    case AdsMode::Reference:
                        PropertyRows::Badge(Status::Info, "Aim pose", "The pose carried actions are measured against.");
                        break;
                    case AdsMode::Authored:
                        PropertyRows::Badge(Status::Ok, "ADS clip", "Tagged ADS: plays its own animation with the sights up.");
                        break;
                    case AdsMode::Carried:
                        PropertyRows::Badge(entry && !entry->Problem.empty() ? Status::Warning : Status::Ok, "Carried",
                                            "The hip clip, carried onto the sights while aiming.");
                        break;
                    default:
                        PropertyRows::Badge(Status::Info, "Drops to hip",
                                            "Neither tagged ADS nor carried: pressing it while aiming plays the hip clip.\n"
                                            "Add the carry tag, or a real ADS clip in a state tagged ADS.");
                        break;
                }
                if (holds) {
                    ImGui::TableNextColumn();
                    if (mode == AdsMode::Carried) {
                        // The share of the clip's own gun turn / movement kept on top of the hold.
                        const auto* existing = ads.GunMotionFor(st.Name);
                        float rot = existing ? existing->Rotation * 100.0f : 0.0f, pos = existing ? existing->Position * 100.0f : 0.0f;
                        ImGui::PushID(st.Name.c_str());
                        const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                        bool edited = false;
                        ImGui::SetNextItemWidth(w);
                        ImGui::DragFloat("##rot", &rot, 0.5f, 0.0f, 100.0f, "turn %.0f%%", ImGuiSliderFlags_AlwaysClamp);
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("How much of the clip's own gun turn plays on the sights (the mag check tipping\n"
                                                 "the mag into view), about the rear sight so the sights stay centred. 0 = locked.");
                        edited |= ImGui::IsItemDeactivatedAfterEdit();
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(w);
                        ImGui::DragFloat("##pos", &pos, 0.5f, 0.0f, 100.0f, "move %.0f%%", ImGuiSliderFlags_AlwaysClamp);
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("How much of the clip's own gun movement plays on the sights. 0 = locked.");
                        edited |= ImGui::IsItemDeactivatedAfterEdit();
                        const float oldRot = existing ? existing->Rotation : 0.0f, oldPos = existing ? existing->Position : 0.0f;
                        if (rot / 100.0f != oldRot || pos / 100.0f != oldPos) {
                            auto& list = ads.GunMotions;
                            auto it = std::find_if(list.begin(), list.end(), [&](const auto& m) { return m.State == st.Name; });
                            if (it == list.end()) it = list.insert(list.end(), {st.Name, 0.0f, 0.0f});
                            it->Rotation = rot / 100.0f;
                            it->Position = pos / 100.0f;
                            if (it->Rotation <= 0.0f && it->Position <= 0.0f) list.erase(it);
                        }
                        if (edited) r.MarkChanged();
                        ImGui::PopID();
                    } else {
                        ImGui::TextDisabled("-");
                    }
                }
                ImGui::TableNextColumn();
                if (mode == AdsMode::Carried) {
                    if (!entry) {
                        ImGui::TextDisabled(report ? "not measured (restart Play)" : "measured when you press Play");
                    } else if (!entry->Problem.empty()) {
                        ImGui::TextColored(EditorUIPrimitives::WarningColor(), "%s", entry->Problem.c_str());
                    } else if (report->HoldsAim) {
                        ImGui::Text("elbow %.0f deg, %d twist", entry->SwivelDeg[1], entry->MatchedBones);
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("The gun and the other hand stay in the aim pose; the action bones play the clip.\n"
                                                 "Elbow swing: right %.1f deg, left %.1f deg. Twist bones matched: %d.",
                                                 entry->SwivelDeg[0], entry->SwivelDeg[1], entry->MatchedBones);
                    } else {
                        ImGui::Text("gun %.1f cm, %.0f deg", entry->GunOffsetCm, entry->GunTurnDeg);
                        if (ImGui::IsItemHovered())
                            EditorUI::SetTooltip("How far the gun is carried from the clip's hold onto the sights.\n"
                                                 "Elbow swing: right %.1f deg, left %.1f deg. Twist bones matched: %d.",
                                                 entry->SwivelDeg[0], entry->SwivelDeg[1], entry->MatchedBones);
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            if (!rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("(no reload / action states)");
            }
            ImGui::EndTable();
        }
        if (report) {
            if (!report->UsesIK && !report->Entries.empty())
                PropertyRows::Badge(Status::Warning, "No arm IK: carried actions move the whole view model");
            for (const std::string& w : report->Warnings) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", w.c_str());
                ImGui::PopTextWrapPos();
            }
            r.Note("Measured in the last Play this session.");
        }
    } else {
        r.Note("Pick a controller in Animation to see how its actions play with the sights up.");
    }

    if (Tree(ICON_FA_BOOK_OPEN "  How to add ADS animations")) {
        r.Note("Real ADS clip (best): import it, add a state for it in the Animator (e.g. \"ADS TacReload\"), tag it "
               "ADS plus what the hip version has (Reload, Busy), and give it an Any State transition with the "
               "action's trigger AND the Aim condition, above the hip one. It then plays as animated.");
        ImGui::Spacing();
        r.Note("Hip clip only: tag the hip state with the carry tag (ADSCarry). While aiming, the gun is carried "
               "onto the aim pose's sight line and the arms follow it - it starts and ends exactly in the aim pose.");
        ImGui::Spacing();
        r.Note("Either way, press Play and check the table above.");
        ImGui::TreePop();
    }
    r.ResetButton("Aim-Down-Sights", [&] {
        ads = kAdsDefaults;
        s.Procedural.Aim = kDefaults.Aim;
    });
}

// --- Recoil ------------------------------------------------------------------------------------

void DrawRecoil(PropertyRows& r, WeaponRecoilSettings& rc, float rpm) {
    static const WeaponProceduralSettings kDefaults = WeaponProceduralSettings::Defaults();
    const WeaponRecoilSettings& def = kDefaults.Recoil;
    r.Check("Enabled", rc.Enabled, "Procedural recoil on every shot, hip and ADS.");
    r.Float("Hip Scale", rc.HipScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier for hip fire.");
    r.Float("ADS Scale", rc.AdsScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier with the sights up.");

    if (Tree("Kick", true)) {
        r.Float("Duration", rc.Duration, 0.005f, 0.02f, 3.0f, "%.3f s", "Seconds one shot's curves span. The curves are keyed over 0..1 of it.");
        r.Vec3("Pivot", rc.Pivot, 0.001f, "%.3f", "Metres: the rotation centre relative to the gun bone, camera frame.\n"
                                                   "Push it back (+Z) to pivot nearer the shoulder.");
        r.Spring("Smoothing", rc.Smoothing.Frequency, rc.Smoothing.Damping,
                 "The spring the summed kicks run through. Higher frequency = snappier; damping 1 = no overshoot.");
        r.Heading("Per-shot random scale");
        r.Range("Pitch", rc.PitchRange, 0.01f, "%.2f", "Each shot multiplies its pitch curve by a random value in this range.");
        r.Range("Yaw", rc.YawRange, 0.01f, "%.2f", "A negative min lets the shot kick either way.");
        r.Range("Roll", rc.RollRange, 0.01f, "%.2f", "A negative min lets the shot roll either way.");
        r.Range("Side", rc.SideRange, 0.01f, "%.2f", "A negative min lets the shot push either way.");
        r.Range("Up", rc.UpRange, 0.01f, "%.2f", "Random scale for the Up curve.");
        r.Range("Kickback", rc.KickRange, 0.01f, "%.2f", "Random scale for the Kickback curve.");
        if (Tree("Shot curves")) {
            const char* shotTip = "One shot's motion over its Duration (0 = the shot). Should end at 0. Shots overlap and add, so full-auto climbs.";
            Curve3Rows(r, "rot", rc.Rotation, def.Rotation, "Pitch (deg)", "Yaw (deg)", "Roll (deg)", "%.2f", "%.2f", 1.0f, 1.0f, shotTip);
            Curve3Rows(r, "pos", rc.Position, def.Position, "Side (m)", "Up (m)", "Kickback (m)", "%.4f", "%.4f", 0.002f, 0.01f, shotTip);
            ImGui::TreePop();
        }
        EndsAtZero("Pitch", rc.Rotation.X);
        EndsAtZero("Yaw", rc.Rotation.Y);
        EndsAtZero("Roll", rc.Rotation.Z);
        EndsAtZero("Side", rc.Position.X);
        EndsAtZero("Up", rc.Position.Y);
        EndsAtZero("Kickback", rc.Position.Z);
        ImGui::TreePop();
    }

    if (Tree("Variety")) {
        r.Note("Runtime randomness, so no two rounds - and no two bursts - leave the gun the same way.");
        r.Float("Kick Spread", rc.KickSpread, 0.1f, 0.0f, 90.0f, "%.1f deg", "Each round's muzzle rise leaves at its own angle off straight up (normal sigma).");
        r.Float("Kick Bias", rc.KickBias, 0.1f, -45.0f, 45.0f, "%.1f deg", "Shifts that angle's average (+ = right).");
        r.Float("Time Jitter", rc.TimeJitter, 0.005f, 0.0f, 0.9f, "%.2f", "Each round's curves play over Duration x [1 - j, 1 + j].");
        r.Float("First Shot", rc.FirstShotScale, 0.01f, 0.0f, 5.0f, "x%.2f", "The first round of a burst kicks this much harder.");
        r.Float("Wander", rc.Wander, 0.005f, 0.0f, 5.0f, "%.3f deg", "Full-auto sideways drift: a random walk, this sigma per round.");
        r.Float("Wander Return", rc.WanderReturn, 0.005f, 0.0f, 1.0f, "%.2f", "How much of the drift is pulled back toward centre each round.");
        r.Float("Burst Growth", rc.BurstGrowth, 0.005f, 0.0f, 2.0f, "%.3f", "Widens Kick Spread and Wander per round of a burst...");
        r.Float("Burst Growth Max", rc.BurstGrowthMax, 0.01f, 1.0f, 10.0f, "x%.2f", "...up to this multiple.");
        ImGui::TreePop();
    }

    if (Tree("Aim Climb")) {
        r.Note("Moves where the player actually aims (the view punch below only moves what they see).");
        r.Range("Pitch", rc.AimPitch, 0.005f, "%.3f deg", "Degrees up per round, a random pick in this range.");
        r.Range("Yaw", rc.AimYaw, 0.005f, "%.3f deg", "Degrees right per round (negative = left), a random pick in this range.");
        r.Float("Recovery", rc.AimRecovery, 0.01f, 0.0f, 1.0f, "%.2f", "How much of the climb eases back once the trigger rests (the rest is the player's to pull down).");
        r.Float("Recovery Delay", rc.AimRecoveryDelay, 0.005f, 0.0f, 2.0f, "%.3f s", "Rest this long before it starts easing back.");
        r.Float("Recovery Speed", rc.AimRecoverySpeed, 0.1f, 0.0f, 100.0f, "%.1f deg/s", "How fast it eases back.");
        ImGui::TreePop();
    }

    if (Tree("Camera Punch")) {
        const char* camTip = "View punch per shot, degrees: what the player sees, not where they aim. Recovers on its own.";
        CurveRow(r, "Pitch (deg)", "##cp", rc.CameraPitch, def.CameraPitch, "%.2f", 0.5f, camTip);
        CurveRow(r, "Yaw (deg)", "##cy", rc.CameraYaw, def.CameraYaw, "%.2f", 0.2f, camTip);
        EndsAtZero("Camera Pitch", rc.CameraPitch);
        EndsAtZero("Camera Yaw", rc.CameraYaw);
        r.Range("Yaw Range", rc.CameraYawRange, 0.01f, "%.2f", "Random scale for the camera yaw; a negative min kicks either way.");
        r.Float("Scale", rc.CameraScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier for the whole camera punch (0 = off).");
        r.Spring("Smoothing", rc.CameraSmoothing.Frequency, rc.CameraSmoothing.Damping,
                 "Smooths the summed punch so full auto reads as one rolling push. Frequency 0.1 is nearly off.");
        ImGui::TreePop();
    }

    if (Tree("Camera Shake")) {
        r.Float("Amount", rc.ShakeAmount, 0.005f, 0.0f, 1.0f, "%.3f", "Trauma each round adds (0 = no shake). The shake is trauma squared, so one tap is a tremor and full auto a rattle.");
        r.Vec3("Max", rc.ShakeMax, 0.01f, "%.2f", "Degrees of shake (pitch, yaw, roll) at full trauma.", "PYR");
        r.Float("Frequency", rc.ShakeFrequency, 0.1f, 0.0f, 60.0f, "%.1f Hz", "How fast the shake's noise moves.");
        r.Float("Decay", rc.ShakeDecay, 0.05f, 0.0f, 20.0f, "%.2f /s", "Trauma drained per second.");
        r.Float("ADS Scale", rc.ShakeAdsScale, 0.01f, 0.0f, 2.0f, "x%.2f", "Multiplier for the visible shake (and the roll and FOV punch below) with the sights up.");
        r.Float("Camera Roll", rc.CameraRoll, 0.01f, 0.0f, 10.0f, "%.2f deg", "Each round snaps the view's roll a random way by up to this much, and it springs back.");
        r.Float("FOV Punch", rc.FovPunch, 0.01f, -10.0f, 10.0f, "%.2f deg", "Each round pulses the field of view by about this much (+ widens), springing back.");
        r.Spring("Punch Spring", rc.PunchSpring.Frequency, rc.PunchSpring.Damping, "How the roll and FOV punch snap in and settle.");
        ImGui::TreePop();
    }

    if (Tree("Bolt & Hip Fire")) {
        r.Check("Hip Procedural", rc.HipProcedural, "Hip rounds kick procedurally too (wherever the gun is simply held) instead of replaying the Fire clip.");
        r.Float("Bolt Cycle", rc.BoltCycle, 0.001f, 0.0f, 1.0f, "%.3f s", "Seconds the bolt takes to slam back and return each round (0 = no procedural bolt).");
        r.Text("Bolt Bone", rc.BoltBone, "Bone on the weapon rig that cycles, along the travel its Fire clip authors.");
        ImGui::TreePop();
    }

    // What a burst looks like with these numbers: the gun's pitch over 1.5 s of a 10-round ADS
    // burst at the weapon's rpm, every random pick at its middle.
    WeaponProceduralSettings sim = kDefaults;
    sim.Recoil = rc;
    sim.Sway.Enabled = sim.Bob.Enabled = sim.Breath.Enabled = sim.Lean.Enabled = false;
    sim.StateOffsets.clear();
    auto& sr = sim.Recoil;
    for (glm::vec2* rg : {&sr.PitchRange, &sr.YawRange, &sr.RollRange, &sr.SideRange, &sr.UpRange, &sr.KickRange, &sr.CameraYawRange})
        *rg = glm::vec2((rg->x + rg->y) * 0.5f);
    sr.KickSpread = sr.TimeJitter = sr.Wander = 0.0f;
    WeaponProceduralState st;
    WeaponProceduralInput in;
    in.Dt = 1.0f / 120.0f;
    const float interval = rpm > 0.0f ? 60.0f / rpm : 0.1f;
    float pitch[180];
    float next = 0.0f, peak = 0.01f;
    int shots = 0;
    for (int f = 0; f < 180; ++f) {
        if (shots < 10 && f * in.Dt >= next) { st.OnShot(sim, true); next += interval; ++shots; }
        pitch[f] = st.Update(sim, in).Rotation.x;
        peak = std::max(peak, std::fabs(pitch[f]));
    }
    char overlay[64];
    std::snprintf(overlay, sizeof overlay, "10-round ADS burst: pitch peaks %.2f deg", peak);
    ImGui::Spacing();
    ImGui::PlotLines("##burst", pitch, 180, 0, overlay, -peak * 0.25f, peak * 1.15f, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 5.0f));
    if (ImGui::IsItemHovered())
        EditorUI::SetTooltip("The gun's pitch over 1.5 s of a 10-round burst at this weapon's rounds per minute,\n"
                             "with every random range at its middle. Updates as you edit.");
    r.ResetButton("Recoil", [&] { rc = def; });
}

// --- Movement ----------------------------------------------------------------------------------

void DrawMovement(PropertyRows& r, WeaponProceduralSettings& p, const WeaponContext& ctx) {
    static const WeaponProceduralSettings kDefaults = WeaponProceduralSettings::Defaults();
    if (Tree("Sway", true)) {
        auto& w = p.Sway;
        r.Check("Enabled", w.Enabled, "The gun lags behind turns and trails against movement.");
        r.Float("Look Rotation", w.LookRotation, 0.01f, 0.0f, 20.0f, "%.2f deg", "Rotation lag per 100 deg/s of turn.");
        r.Float("Look Position", w.LookPosition, 0.0001f, 0.0f, 0.1f, "%.4f m", "Position lag per 100 deg/s of turn.");
        r.Float("Max Rotation", w.MaxRotation, 0.05f, 0.0f, 45.0f, "%.1f deg", "Largest rotation sway, however fast the turn.");
        r.Float("Max Position", w.MaxPosition, 0.0005f, 0.0f, 0.2f, "%.3f m", "Largest position sway, however fast the turn or move.");
        r.Float("Move Position", w.MovePosition, 0.0001f, 0.0f, 0.05f, "%.4f m", "Trail per m/s of movement.");
        r.Float("Move Roll", w.MoveRoll, 0.01f, 0.0f, 10.0f, "%.2f deg", "Tilt into a strafe, per m/s.");
        r.Float("ADS Scale", w.AdsScale, 0.01f, 0.0f, 2.0f, "x%.2f", "Multiplier with the sights up (lower = steadier aim).");
        r.Spring("Spring", w.Spring.Frequency, w.Spring.Damping, "Damping under 1 overshoots, which reads as weight.");
        r.Float("Look Smoothing", w.LookSmoothing, 0.1f, 0.0f, 60.0f, w.LookSmoothing > 0.0f ? "%.1f Hz" : "raw",
                "Smooths the mouse's turn rate before it drives the sway, so the gun glides instead of buzzing. Lower = smoother, laggier.");
        r.ResetButton("Sway", [&] { p.Sway = kDefaults.Sway; });
        ImGui::TreePop();
    }
    if (Tree("Bob")) {
        auto& b = p.Bob;
        r.Check("Enabled", b.Enabled, "Walk and sprint bob, locked to the distance travelled.");
        r.Float("Walk Stride", b.WalkStride, 0.05f, 0.1f, 10.0f, "%.2f m", "Distance per cycle (two steps) while walking.");
        r.Float("Sprint Stride", b.SprintStride, 0.05f, 0.1f, 10.0f, "%.2f m", "Distance per cycle (two steps) while sprinting.");
        r.Float("Full Speed", b.WalkFullSpeed, 0.05f, 0.1f, 30.0f, "%.2f m/s", "Speed at which the bob reaches full amplitude.");
        r.Float("Hip Scale", b.HipScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier at the hip (the walk and sprint clips already bob there).");
        r.Float("ADS Scale", b.AdsScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier with the sights up.");
        r.Float("Ease", b.Ease, 0.1f, 0.1f, 50.0f, "%.1f /s", "How fast the bob fades in when you start moving and out when you stop.");
        if (Tree("Cycle curves")) {
            const char* cycleTip = "One stride (two steps), keyed over 0..1. Should start and end on the same value so it loops.";
            ImGui::SeparatorText("Walk");
            Curve3Rows(r, "walk", b.Walk, kDefaults.Bob.Walk, "Side (m)", "Up (m)", "Roll (deg)", "%.4f", "%.2f", 0.003f, 0.5f, cycleTip);
            ImGui::SeparatorText("Sprint");
            Curve3Rows(r, "sprint", b.Sprint, kDefaults.Bob.Sprint, "Side (m)", "Up (m)", "Roll (deg)", "%.4f", "%.2f", 0.008f, 1.0f, cycleTip);
            ImGui::TreePop();
        }
        r.ResetButton("Bob", [&] { p.Bob = kDefaults.Bob; });
        ImGui::TreePop();
    }
    if (Tree("Breathing")) {
        auto& br = p.Breath;
        r.Check("Enabled", br.Enabled, "A slow idle drift, always running.");
        r.Float("Period", br.Period, 0.05f, 0.2f, 20.0f, "%.2f s", "Seconds per breath.");
        r.Float("Hip Scale", br.HipScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier at the hip.");
        r.Float("ADS Scale", br.AdsScale, 0.01f, 0.0f, 5.0f, "x%.2f", "Multiplier with the sights up (lower = steadier aim).");
        r.Heading("Drift");
        r.Vec3("Position", br.DriftPosition, 0.00005f, "%.4f", "Metres of slow, never-repeating drift (side, up, back) on top of the breath.");
        r.Vec3("Rotation", br.DriftRotation, 0.005f, "%.3f", "Degrees of drift (pitch, yaw, roll).", "PYR");
        r.Float("Frequency", br.DriftFrequency, 0.01f, 0.0f, 5.0f, "%.2f Hz", "How fast the drift wanders.");
        r.Heading("Exertion");
        r.Float("Scale", br.ExertionScale, 0.01f, 1.0f, 10.0f, "x%.2f", "How much bigger the breath and drift get when winded from sprinting.");
        r.Float("Rate", br.ExertionRate, 0.01f, 1.0f, 5.0f, "x%.2f", "How much faster the breathing gets when winded.");
        r.Float("Build", br.ExertionBuild, 0.05f, 0.0f, 30.0f, "%.1f s", "Seconds of sprinting to be fully winded.");
        r.Float("Recover", br.ExertionRecover, 0.05f, 0.0f, 30.0f, "%.1f s", "Seconds to get the breath back after sprinting.");
        if (Tree("Breath curves")) {
            const char* tip = "One breath, keyed over 0..1. Should start and end on the same value so it loops.";
            CurveRow(r, "Side (m)", "##bx", br.Position.X, kDefaults.Breath.Position.X, "%.4f", 0.0005f, tip);
            CurveRow(r, "Up (m)", "##by", br.Position.Y, kDefaults.Breath.Position.Y, "%.4f", 0.0005f, tip);
            CurveRow(r, "Back (m)", "##bz", br.Position.Z, kDefaults.Breath.Position.Z, "%.4f", 0.0005f, tip);
            CurveRow(r, "Pitch (deg)", "##bp", br.Pitch, kDefaults.Breath.Pitch, "%.3f", 0.1f, tip);
            ImGui::TreePop();
        }
        r.ResetButton("Breathing", [&] { p.Breath = kDefaults.Breath; });
        ImGui::TreePop();
    }
    if (Tree("Jump & Land")) {
        auto& j = p.Jump;
        r.Check("Enabled", j.Enabled, "The gun lags against vertical speed in the air and kicks down on landing.");
        r.Float("Air Position", j.AirPosition, 0.0001f, 0.0f, 0.05f, "%.4f m", "Lag per m/s of vertical speed (rising pulls the gun down, falling floats it up).");
        r.Float("Air Pitch", j.AirPitch, 0.01f, 0.0f, 10.0f, "%.2f deg", "Muzzle lag per m/s of vertical speed.");
        r.Float("Max Position", j.MaxPosition, 0.0005f, 0.0f, 0.2f, "%.3f m", "Largest air lag.");
        r.Float("Max Pitch", j.MaxPitch, 0.05f, 0.0f, 45.0f, "%.1f deg", "Largest air pitch.");
        r.Float("Land Position", j.LandPosition, 0.0001f, 0.0f, 0.05f, "%.4f m", "How far a landing drops the gun, per m/s of fall speed.");
        r.Float("Land Pitch", j.LandPitch, 0.01f, 0.0f, 10.0f, "%.2f deg", "Muzzle dip per m/s of fall speed.");
        r.Float("Land Roll", j.LandRoll, 0.01f, 0.0f, 10.0f, "%.2f deg", "Roll a random way per m/s of fall speed.");
        r.Float("Min Impact", j.MinImpact, 0.05f, 0.0f, 20.0f, "%.2f m/s", "Softer touchdowns (steps, slopes) don't kick.");
        r.Float("Max Impact", j.MaxImpact, 0.05f, 0.0f, 50.0f, "%.2f m/s", "Harder landings kick no more than this.");
        r.Float("ADS Scale", j.AdsScale, 0.01f, 0.0f, 2.0f, "x%.2f", "Multiplier with the sights up.");
        r.Spring("Spring", j.Spring.Frequency, j.Spring.Damping, "How the gun settles after a landing (damping under 1 bounces).");
        r.ResetButton("Jump & Land", [&] { p.Jump = kDefaults.Jump; });
        ImGui::TreePop();
    }
    if (Tree("Camera Motion")) {
        auto& c = p.CameraMotion;
        r.Check("Enabled", c.Enabled, "The view itself moves with your steps, strafes and landings (never where you aim).");
        r.Vec2("Walk Bob", c.WalkBob, 0.0005f, "%.4f", "Head bob at full walk: height (m, down on each footfall), roll (deg, once per stride).");
        r.Vec2("Sprint Bob", c.SprintBob, 0.0005f, "%.4f", "Head bob at full sprint: height (m), roll (deg).");
        r.Float("Strafe Roll", c.StrafeRoll, 0.01f, 0.0f, 5.0f, "%.2f deg", "Roll into a strafe, per m/s of sideways speed.");
        r.Float("Land Dip", c.LandDip, 0.0005f, 0.0f, 0.1f, "%.4f m", "How far a landing drops the view, per m/s of fall speed.");
        r.Float("Land Pitch", c.LandPitch, 0.01f, 0.0f, 5.0f, "%.2f deg", "Nod per m/s of fall speed.");
        r.Float("ADS Scale", c.AdsScale, 0.01f, 0.0f, 2.0f, "x%.2f", "Multiplier with the sights up.");
        r.Spring("Spring", c.Spring.Frequency, c.Spring.Damping, "How the landing dip and strafe roll settle.");
        r.ResetButton("Camera Motion", [&] { p.CameraMotion = kDefaults.CameraMotion; });
        ImGui::TreePop();
    }
    if (Tree("Walls")) {
        auto& o = p.Obstruction;
        r.Check("Enabled", o.Enabled, "The gun pulls back off walls in front of you instead of clipping into them.");
        r.Float("Reach", o.Reach, 0.005f, 0.0f, 3.0f, "%.2f m", "Distance from the eye to just past the muzzle: anything nearer pushes the gun back.");
        r.Vec2("Probe Offset", o.ProbeOffset, 0.005f, "%.3f",
               "Where the second probe runs from, metres right and up of the eye (along the barrel), so walls on the gun's side count too.");
        r.Heading("1. Retract");
        r.Float("Max Retract", o.MaxRetract, 0.005f, 0.0f, 0.5f, "%.2f m",
                "First the gun slides straight back, one for one with how far the wall is in, so the muzzle stops at the surface and you can still aim. Up to this far.");
        r.Heading("2. Tuck");
        r.Float("Tuck Range", o.TuckRange, 0.005f, 0.0f, 1.0f, "%.2f m", "Past the retract, how much closer the wall has to get for a full tuck.");
        r.Vec3("Low Position", o.Position, 0.001f, "%.3f", "Metres the gun moves in a full low ready (side, up, back).");
        r.Vec3("Low Rotation", o.Rotation, 0.1f, "%.1f", "Degrees in a full low ready (pitch: - = muzzle down, yaw, roll).", "PYR");
        r.Vec3("High Position", o.HighPosition, 0.001f, "%.3f", "The same for a high ready: used when what's in the way faces up (a table, the top of a low wall).");
        r.Vec3("High Rotation", o.HighRotation, 0.1f, "%.1f", "Degrees in a full high ready (pitch: + = muzzle up).", "PYR");
        r.Vec3("Side Position", o.SidePosition, 0.001f, "%.3f",
               "For an edge beside the barrel (a corner you're peeking past, a door frame) on the right: metres the gun moves aside. Mirrored for the left.");
        r.Vec3("Side Rotation", o.SideRotation, 0.1f, "%.1f", "Degrees the gun turns away from that edge (yaw: + = muzzle left). Mirrored for the left.", "PYR");
        float block = o.BlockAt * 100.0f;
        r.Float("Block At", block, 0.5f, 0.0f, 100.0f, block > 0.0f ? "%.0f%% tucked" : "never",
                "Tucked this far, the gun won't fire and the sights drop. 0 = never.");
        o.BlockAt = block / 100.0f;
        r.Spring("Spring", o.Spring.Frequency, o.Spring.Damping, "How it eases on and off the wall.");
        r.ResetButton("Walls", [&] { p.Obstruction = kDefaults.Obstruction; });
        ImGui::TreePop();
    }
    if (Tree("Locomotion Rate")) {
        auto& l = p.Locomotion;
        r.Note("Sets the WalkRate / SprintRate parameters from the player's speed. Use them as the Walk and Sprint "
               "states' speed parameter so the clips step at the pace you move.");
        r.Check("Match Speed", l.MatchSpeed, "Off: both rates stay at 1.");
        r.Float("Walk Reference", l.WalkReference, 0.05f, 0.0f, 30.0f, l.WalkReference > 0.0f ? "%.2f m/s" : "auto",
                "Speed the walk clip plays at rate 1. 0 (auto) = the player controller's Move Speed.");
        r.Float("Sprint Reference", l.SprintReference, 0.05f, 0.0f, 30.0f, l.SprintReference > 0.0f ? "%.2f m/s" : "auto",
                "Speed the sprint clip plays at rate 1. 0 (auto) = Move Speed x Sprint Multiplier.");
        r.Float("Min Rate", l.MinRate, 0.01f, 0.0f, l.MaxRate, "x%.2f", "Slowest the clips play, however slowly you move.");
        r.Float("Max Rate", l.MaxRate, 0.01f, l.MinRate, 5.0f, "x%.2f", "Fastest the clips play, however fast you move.");
        r.ResetButton("Locomotion Rate", [&] { p.Locomotion = kDefaults.Locomotion; });
        ImGui::TreePop();
    }
    if (Tree("Lean")) {
        auto& l = p.Lean;
        r.Check("Enabled", l.Enabled, "The lean, driven by the corner peek below.");
        r.Check("Corner Peek", l.CornerPeek, "Aiming with cover just ahead leans out around its open side, as far as clearing the edge takes.");
        r.Float("Peek Range", l.PeekRange, 0.01f, 0.0f, 5.0f, "%.2f m", "How close the cover has to be ahead of you for aiming to peek around it.");
        r.Float("Peek Margin", l.PeekMargin, 0.005f, 0.0f, 0.5f, "%.2f m", "How far past the edge the view leans, beyond just clearing it.");
        r.Float("Camera Roll", l.Angle, 0.1f, 0.0f, 45.0f, "%.1f deg", "Camera roll at full lean.");
        r.Float("Camera Offset", l.Offset, 0.005f, 0.0f, 1.0f, "%.3f m",
                "Side step of the view at full lean. The head swings over on an arc, so it also drops a little. It stops short of walls.");
        r.Spring("Spring", l.Spring.Frequency, l.Spring.Damping, "How the lean goes out and comes back (damping under 1 settles with a slight overshoot).");
        r.Float("Weapon Roll", l.WeaponRoll, 0.1f, 0.0f, 45.0f, "%.1f deg", "Extra gun roll into the lean.");
        r.Float("Weapon Roll ADS", l.WeaponRollAds, 0.01f, 0.0f, 2.0f, "x%.2f", "Multiplier on that roll with the sights up (lower keeps the sights readable).");
        r.Spring("Weapon Spring", l.WeaponSpring.Frequency, l.WeaponSpring.Damping,
                 "The gun's roll follows on its own, slower spring, so it lags and settles after the head - weight.");
        r.Check("While Sprinting", l.WhileSprinting, "Off: sprinting straightens up.");
        r.ResetButton("Lean", [&] { p.Lean = kDefaults.Lean; });
        ImGui::TreePop();
    }
    if (Tree("State Offsets")) {
        r.Note("Tweak the gun's pose while a state (by name or tag) plays - e.g. lower it in Sprint.");
        for (int i = 0; i < (int)p.StateOffsets.size(); ++i) {
            auto& o = p.StateOffsets[i];
            ImGui::PushID(i);
            ImGui::Separator();
            r.Name("State or Tag", o.Match, ctx.StatesAndTags, !ctx.StatesAndTags.empty(),
                   "A state name or tag from the weapon's Animator Controller.",
                   "No state or tag in the controller is called '%s', so it never applies.");
            r.Vec3("Position", o.Position, 0.0005f, "%.4f", "Metres added to the gun while the state plays, camera frame.");
            r.Vec3("Rotation", o.Rotation, 0.05f, "%.2f", "Degrees (pitch, yaw, roll) added while the state plays.", "PYR");
            r.Float("Blend In", o.BlendIn, 0.005f, 0.0f, 5.0f, "%.3f s", "Seconds to blend in when the state starts.");
            r.Float("Blend Out", o.BlendOut, 0.005f, 0.0f, 5.0f, "%.3f s", "Seconds to blend out when it ends.");
            const bool remove = ActionButton(ICON_FA_TRASH_CAN "  Remove", "Remove this offset", false, ImVec2(-FLT_MIN, 0.0f));
            ImGui::PopID();
            if (remove) {
                p.StateOffsets.erase(p.StateOffsets.begin() + i);
                r.MarkChanged();
                break;
            }
        }
        if (ActionButton(ICON_FA_PLUS "  Add State Offset", "Add a pose tweak for a state or tag", false, ImVec2(-FLT_MIN, 0.0f))) {
            p.StateOffsets.push_back({"Walk", {}, {}, 0.2f, 0.25f});
            r.MarkChanged();
        }
        ImGui::TreePop();
    }
}

// --- IK ----------------------------------------------------------------------------------------

int MissingIKBones(const WeaponIKSettings& k, const Model& arms) {
    int missing = 0;
    for (const std::string* b : {&k.GunBone, &k.RightUpper, &k.RightLower, &k.RightHand, &k.LeftUpper, &k.LeftLower, &k.LeftHand})
        if (arms.NodeIndex(*b) < 0) ++missing;
    return missing;
}

void DrawIK(PropertyRows& r, WeaponIKSettings& k, WeaponContext& ctx) {
    static const WeaponProceduralSettings kDefaults = WeaponProceduralSettings::Defaults();
    r.Note("Two-bone IK keeps both hands on the gun bone while the procedural motion (and ADS carrying) moves it.");
    r.Check("Enabled", k.Enabled, "Off: the procedural motion moves the whole view model instead of the gun.");
    if (!ctx.Arms) {
        if (ctx.CanLoadArms) {
            if (ActionButton(ICON_FA_MAGNIFYING_GLASS "  Check Bones Against the Arms Rig",
                             "Load the arms model to pick bones from a list and flag names it doesn't have", false,
                             ImVec2(-FLT_MIN, 0.0f)))
                ctx.LoadArms = true;
        } else {
            r.Note("The arms model isn't available, so bone names can't be checked.");
        }
    }
    const bool validate = ctx.Arms != nullptr;
    const char* unknown = "The arms rig has no bone named '%s'.";
    r.Name("Gun Bone", k.GunBone, ctx.Bones, validate, "Arms-rig bone the procedural motion moves (the weapon rides it).", unknown);
    r.Heading("Right arm");
    r.Name("Upper", k.RightUpper, ctx.Bones, validate, "Right upper arm (shoulder joint).", unknown);
    r.Name("Lower", k.RightLower, ctx.Bones, validate, "Right forearm (elbow joint).", unknown);
    r.Name("Hand", k.RightHand, ctx.Bones, validate, "Right hand (wrist joint).", unknown);
    r.Heading("Left arm");
    r.Name("Upper##l", k.LeftUpper, ctx.Bones, validate, "Left upper arm (shoulder joint).", unknown);
    r.Name("Lower##l", k.LeftLower, ctx.Bones, validate, "Left forearm (elbow joint).", unknown);
    r.Name("Hand##l", k.LeftHand, ctx.Bones, validate, "Left hand (wrist joint).", unknown);
    if (validate) {
        const int missing = MissingIKBones(k, *ctx.Arms);
        char msg[128];
        if (missing) {
            std::snprintf(msg, sizeof msg, "%d bone%s not on the arms rig: IK is off", missing, missing == 1 ? " is" : "s are");
            PropertyRows::Badge(Status::Warning, msg, "The whole view model moves instead of the gun, and ADS actions carry the whole rig.");
        } else {
            PropertyRows::Badge(Status::Ok, "All IK bones found on the arms rig");
        }
    }
    r.Heading("Blending");
    r.Name("Off Tag", k.OffTag, ctx.Tags, false, "States with this tag play purely as authored: IK and the procedural motion fade out.");
    r.Float("Blend Time", k.BlendTime, 0.005f, 0.0f, 2.0f, "%.3f s", "Seconds to fade out and back in around Off-tagged states.");
    r.ResetButton("IK", [&] { k = kDefaults.IK; });
}

// Where rounds and the laser leave the gun, where they're zeroed to, the laser and the holes.
void DrawBarrel(PropertyRows& r, FirstPersonAnimationSet& s, const FirstPersonBarrelReport* report) {
    FirstPersonWeaponGameplay& g = s.Gameplay;
    FirstPersonMuzzleSettings& mz = s.Muzzle;
    auto vec = [](const glm::vec3& v, int decimals) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.*f, %.*f, %.*f", decimals, v.x, decimals, v.y, decimals, v.z);
        return std::string(buf);
    };

    r.Heading("Muzzle");
    if (report) {
        if (!report->HasMuzzle)
            PropertyRows::Badge(Status::Error, "No muzzle", (report->Problem + " Rounds hit nothing and there's no laser.").c_str());
        else
            PropertyRows::Badge(Status::Ok, mz.Auto ? "Muzzle found" : "Muzzle set by hand", "From the last Play.");
        ImGui::SameLine();
        PropertyRows::Badge(report->Detected ? Status::Ok : Status::Info, report->Detected ? "Bolt: barrel detected" : "Bolt: no barrel",
                            "Whether Auto could find the barrel from the procedural bolt's travel.");
    } else {
        PropertyRows::Badge(Status::Info, "Not checked yet", "Play the weapon once: the muzzle is found (or checked) at Start.");
    }
    r.Check("Auto", mz.Auto,
            "Find the barrel from the procedural bolt (Recoil > Bolt): its travel is the bore, the mesh's front face along\n"
            "it the muzzle. Off for a weapon whose bolt doesn't run down the bore (slide, pump, revolver) or has none.");
    if (!mz.Auto) {
        r.Vec3("Origin", mz.Origin, 0.001f, "%.4f", "The muzzle, in the weapon root bone's space (model units).");
        if (r.Vec3("Direction", mz.Direction, 0.001f, "%.4f", "Down the bore, in the weapon root bone's space.") &&
            glm::length(mz.Direction) > 1e-6f)
            mz.Direction = glm::normalize(mz.Direction);
    }
    if (report && report->Detected) {
        r.Value("Detected", vec(report->DetectedOrigin, 4).c_str(), "Where Auto found the muzzle (weapon root space).");
        if (ActionButton(ICON_FA_COPY "  Use Detected as Starting Point",
                         "Switch Auto off and start from what the bolt found, to nudge by hand", false, ImVec2(-FLT_MIN, 0.0f))) {
            mz.Auto = false;
            mz.Origin = report->DetectedOrigin;
            mz.Direction = report->DetectedDirection;
            r.MarkChanged();
        }
    }

    r.Heading("Zeroing");
    r.Float("Zero Distance", g.ZeroDistance, 0.5f, 0.0f, 500.0f, "%.0f m",
            "Rounds and the laser cross the sight line this far out: dead on the front post there, a little low closer, a little high past it. 0 = straight down the bore.");
    if (g.HasSightLine) {
        r.Value("Sight Line", "saved", ("Origin " + vec(g.SightOrigin, 4) + "\nDirection " + vec(g.SightDirection, 5)).c_str());
        if (ActionButton(ICON_FA_ROTATE "  Re-measure Sight Line",
                         "Forget the saved sight line: Play measures it again the next time the sights settle (aim, stand still, don't fire)",
                         false, ImVec2(-FLT_MIN, 0.0f))) {
            g.HasSightLine = false;
            r.MarkChanged();
        }
    } else if (report && report->SightMeasured) {
        r.Value("Sight Line", "measured, not saved", ("Origin " + vec(report->SightOrigin, 4) + "\nDirection " + vec(report->SightDirection, 5)).c_str());
        if (ActionButton(ICON_FA_FLOPPY_DISK "  Save Measured Sight Line", "Keep what Play measured, so the zero is right from the first shot",
                         false, ImVec2(-FLT_MIN, 0.0f))) {
            g.HasSightLine = true;
            g.SightOrigin = report->SightOrigin;
            g.SightDirection = report->SightDirection;
            r.MarkChanged();
        }
    } else {
        r.Value("Sight Line", "not measured",
                "In Play, aim and hold still for about two seconds without firing: the sight line is measured, and Save appears here.");
    }

    r.Heading("Laser");
    FirstPersonLaserSettings& l = s.Laser;
    r.Check("Enabled", l.Enabled, "A beam from the muzzle down the zeroed bore, and the dot where it lands.");
    if (l.Enabled) {
        r.Label("Color", "The laser's hue (linear). The beam and the dot draw at their brightness times it.");
        // The picker popup's drags live on other item IDs, so the swatch never reports its own
        // deactivation: save once nothing is being dragged any more.
        static bool s_ColorPending = false;
        if (ImGui::ColorEdit3("##laser", &l.Color.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs)) s_ColorPending = true;
        if (s_ColorPending && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            s_ColorPending = false;
            r.MarkChanged();
        }
        r.Float("Beam Brightness", l.BeamBrightness, 0.01f, 0.0f, 100.0f, "%.2f", "The beam through the air (dust makes it sparkle).");
        r.Float("Dot Brightness", l.SpotBrightness, 0.05f, 0.0f, 100.0f, "%.1f", "The dot on what it hits; high enough washes its core out to white.");
    }

    r.Heading("Bullet Holes");
    float mm = g.BulletHoleRadius * 2000.0f;
    if (r.Float("Hole Size", mm, 0.05f, 0.0f, 100.0f, "%.1f mm", "Across the hole each round leaves, torn ring included (a little over the calibre: 9 mm for 5.45 mm rounds). 0 = none."))
        g.BulletHoleRadius = mm / 2000.0f;
}

} // namespace

void EditorLayer::DrawWeaponDefinitionEditor(const std::string& path) {
    struct Cache {
        std::string Path;
        fs::file_time_type Stamp{};
        FirstPersonAnimationSet Set;
        std::string Error;
        std::string Saved;                    // the file's content as last loaded / saved
        std::vector<std::string> Undo, Redo;  // earlier / undone contents
    };
    static Cache cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(path), ec);
    if (cache.Path != path || cache.Stamp != stamp) {
        if (cache.Path != path) cache.Undo.clear(), cache.Redo.clear();
        cache.Path = path;
        cache.Stamp = stamp;
        cache.Error.clear();
        if (!FirstPersonAnimationSet::LoadFile(path, cache.Set, &cache.Error)) cache.Set = {};
        cache.Saved = cache.Error.empty() ? cache.Set.ToJsonString() : std::string();
    }
    auto save = [&](const FirstPersonAnimationSet& s) {
        if (s.SaveFile(path)) cache.Stamp = fs::last_write_time(fs::u8path(path), ec);
        else Log::Error("Couldn't save " + ProjectPaths::Relativize(path) + ".");
    };
    auto step = [&](std::vector<std::string>& from, std::vector<std::string>& to) {
        if (from.empty()) return;
        FirstPersonAnimationSet restored;
        std::string why;
        if (!FirstPersonAnimationSet::FromJsonString(from.back(), restored, &why)) { from.pop_back(); return; }
        to.push_back(cache.Saved);
        cache.Saved = from.back();
        from.pop_back();
        cache.Set = restored;
        save(cache.Set);
    };

    FirstPersonAnimationSet& s = cache.Set;
    const std::string name = fs::u8path(path).stem().u8string();

    // --- overview card -------------------------------------------------------------------------
    ImGui::SeparatorText(ICON_FA_CROSSHAIRS "  Weapon Definition");
    {
        const float buttonsW = 2.0f * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - buttonsW));
        ImGui::BeginDisabled(cache.Undo.empty());
        if (ActionButton(ICON_FA_ROTATE_LEFT, "Undo the last change to this weapon", false, ImVec2(ImGui::GetFrameHeight(), 0)))
            step(cache.Undo, cache.Redo);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(cache.Redo.empty());
        if (ActionButton(ICON_FA_ROTATE_RIGHT, "Redo", false, ImVec2(ImGui::GetFrameHeight(), 0))) step(cache.Redo, cache.Undo);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", ProjectPaths::Relativize(path).c_str());
    }
    if (!cache.Error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", cache.Error.c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    // Choices and checks: the controller's states and tags, the arms rig's bones once it is
    // loaded (only on request - a big FBX shouldn't load just because the Inspector opened).
    WeaponContext ctx;
    if (!s.Controller.empty()) ctx.Controller = GetAnimatorController(s.Controller);
    if (ctx.Controller) {
        for (size_t li = 0; li < ctx.Controller->Layers.size(); ++li)
            for (const auto& st : ctx.Controller->Layers[li].States) {
                if (li == 0) ctx.States.push_back(st.Name);
                ctx.StatesAndTags.push_back(st.Name);
                for (const auto& t : st.Tags) ctx.Tags.push_back(t), ctx.StatesAndTags.push_back(t);
            }
    }
    for (const auto& t : K::kKnownTags) ctx.Tags.push_back(t.Name);
    SortUnique(ctx.Tags);
    SortUnique(ctx.StatesAndTags);
    std::shared_ptr<Model> arms;
    const std::string armsPath = s.ArmsModel.empty() ? std::string() : ProjectPaths::Resolve(s.ArmsModel);
    if (!armsPath.empty() && m_AssetsPtr) {
        const fs::path want = fs::u8path(armsPath).lexically_normal();
        for (const auto& m : m_AssetsPtr->Models())
            if (m && fs::u8path(m->Path()).lexically_normal() == want) { arms = m; break; }
        ctx.CanLoadArms = !arms && fs::exists(want, ec);
    }
    if (arms) {
        ctx.Arms = arms.get();
        for (int i = 0; i < arms->NodeCount(); ++i) ctx.Bones.push_back(arms->NodeName(i));
    }

    // Status chips.
    {
        char buf[128];
        if (ctx.Controller) {
            int states = 0;
            for (const auto& L : ctx.Controller->Layers) states += (int)L.States.size();
            std::snprintf(buf, sizeof buf, "Controller: %d states", states);
            PropertyRows::Badge(Status::Ok, buf, s.Controller.c_str());
        } else {
            PropertyRows::Badge(s.Clips.empty() ? Status::Error : Status::Info, s.Clips.empty() ? "No controller" : "v1 clip list");
        }
        ImGui::SameLine();
        if (ctx.Arms) {
            const bool socketOk = s.WeaponSocket.empty() || ctx.Arms->NodeIndex(s.WeaponSocket) >= 0;
            const int missing = MissingIKBones(s.Procedural.IK, *ctx.Arms);
            PropertyRows::Badge(socketOk && !missing ? Status::Ok : Status::Warning,
                                socketOk && !missing ? "Rig OK" : "Rig: check bones",
                                "Weapon socket and IK bones checked against the arms rig.");
        } else {
            PropertyRows::Badge(Status::Info, "Rig not checked", "Load the arms rig from the IK section to check its bones.");
        }
        int carried = 0, authored = 0, problems = 0;
        std::string reference = s.Ads.ReferenceState;
        if (ctx.Controller && !ctx.Controller->Layers.empty())
            for (const auto& st : ctx.Controller->Layers[0].States) {
                const AdsMode m = ModeOf(st, s, reference);
                carried += m == AdsMode::Carried;
                authored += m == AdsMode::Authored;
            }
        if (const AdsCarryReport* rep = FindAdsCarryReport(path)) {
            problems = (int)rep->Warnings.size();
            for (const auto& e : rep->Entries) problems += !e.Problem.empty();
        }
        ImGui::SameLine();
        std::snprintf(buf, sizeof buf, "ADS: %d clip%s, %d carried", authored, authored == 1 ? "" : "s", carried);
        PropertyRows::Badge(problems ? Status::Warning : Status::Ok, buf, "See Aim-Down-Sights.");
    }
    // The setup checks: what the controller and rigs still lack for this weapon to work end to end.
    {
        FirstPersonWeaponCheckInput wi;
        wi.Set = &s;
        wi.Controller = ctx.Controller.get();
        if (ctx.Arms) wi.HasArmsBone = [arms = ctx.Arms](const std::string& b) { return arms->NodeIndex(b) >= 0; };
        ImGui::Spacing();
        SetupChecksUI::Draw(FirstPersonWeaponValidate(wi), "Setup: everything the driver needs is there", "##weaponsetup");
    }
    ImGui::Spacing();

    bool changed = false;
    PropertyRows r(150.0f * m_UIScale, changed);
    FirstPersonWeaponGameplay& g = s.Gameplay;

    char summary[128];
    std::snprintf(summary, sizeof summary, "%s", s.Controller.empty() ? "v1 clips" : fs::u8path(s.Controller).filename().u8string().c_str());
    if (r.Section(ICON_FA_DIAGRAM_PROJECT, "Animation", summary, true)) {
        r.Label("Controller", "The Animator Controller that animates both rigs: its \"arms\" track plays on the arms\n"
                              "model and its \"weapon\" track on the weapon model.");
        if (ImGui::BeginCombo("##wctrl", s.Controller.empty() ? "(v1 clip list)" : s.Controller.c_str())) {
            for (const std::string& c : FindAnimatorControllers())
                if (ImGui::Selectable(c.c_str(), c == s.Controller)) { s.Controller = c; changed = true; }
            ImGui::EndCombo();
        }
        if (!s.Controller.empty()) {
            if (ActionButton(ICON_FA_DIAGRAM_PROJECT "  Open in Animator", "Edit the weapon's states, tags and transitions", false,
                             ImVec2(-FLT_MIN, 0.0f)))
                OpenAnimatorWindow(s.Controller);
        } else if (!s.Clips.empty()) {
            char note[160];
            std::snprintf(note, sizeof note, "A v1 file: its %d clips run on the standard first-person graph built in memory.", (int)s.Clips.size());
            r.Note(note);
            if (ActionButton(ICON_FA_WAND_MAGIC_SPARKLES "  Create Controller from Clips",
                             "Write the standard first-person graph for these clips next to this file and point the weapon at it",
                             false, ImVec2(-FLT_MIN, 0.0f))) {
                const fs::path out = fs::u8path(path).replace_extension(".controller");
                if (BuildFirstPersonController(s).SaveFile(out.u8string())) {
                    s.Controller = ProjectPaths::Relativize(out.generic_u8string());
                    s.Clips.clear();
                    changed = true;
                    InvalidateAnimationListing();
                    OpenAnimatorWindow(s.Controller);
                }
            }
        }
    }

    std::snprintf(summary, sizeof summary, "zoom %.2fx", s.Ads.Zoom);
    if (r.Section(ICON_FA_BULLSEYE, "Aim-Down-Sights", summary, true)) DrawAds(r, s, ctx, path);

    std::snprintf(summary, sizeof summary, "%d rounds, %.0f rpm", g.Magazine, g.RoundsPerMinute);
    if (r.Section(ICON_FA_GUN, "Gameplay", summary, true)) {
        r.Int("Magazine", g.Magazine, 1, 1000, "Rounds per magazine.");
        r.Float("Rounds / Minute", g.RoundsPerMinute, 5.0f, 60.0f, 2000.0f, "%.0f", "Full-auto cadence.");
        r.Check("Full-Auto", g.AllowFullAuto, "Off for a semi-only weapon: the fire-mode key then does nothing.");
        r.Float("Reload Hold", g.ReloadHoldSeconds, 0.01f, 0.05f, 2.0f, "%.2f s", "R held this long checks the magazine instead of reloading.");
        glm::vec2 fidget(g.RegripMin, g.RegripMax);
        if (r.Range("Fidget After", fidget, 0.1f, "%.1f s", "Seconds of settled idle before the fidget plays (random between min and max)."))
            g.RegripMin = std::max(0.0f, fidget.x), g.RegripMax = std::max(g.RegripMin, fidget.y);
        r.Heading("Impacts");
        r.Float("Impulse", g.ImpactImpulse, 0.1f, 0.0f, 100.0f, "%.1f N*s", "How hard each round shoves the physics body it hits, at the hit point (0 = no push).");
        r.Float("Max Speed", g.ImpactMaxSpeed, 0.1f, 0.0f, 50.0f, "%.1f m/s", "Caps the velocity one round can add, so light props fly without rocketing off.");
    }

    const FirstPersonBarrelReport* barrel = FindBarrelReport(path);
    std::snprintf(summary, sizeof summary, "%s, %s", s.Muzzle.Auto ? "auto muzzle" : "set muzzle", s.Laser.Enabled ? "laser" : "no laser");
    if (r.Section(ICON_FA_LOCATION_CROSSHAIRS, "Barrel & Laser", summary, true)) DrawBarrel(r, s, barrel);

    std::snprintf(summary, sizeof summary, "socket %s", s.WeaponSocket.empty() ? "(shared pose)" : s.WeaponSocket.c_str());
    if (r.Section(ICON_FA_PERSON, "Rigs & Mount", summary)) {
        r.Value("Arms Model", s.ArmsModel.c_str(), "The arms FBX: mesh, skeleton and a rest bind pose.");
        r.Value("Weapon Model", s.WeaponModel.c_str(), "The weapon FBX: mesh and its own armature.");
        r.Vec3("View Rotation", s.ViewRotation, 0.5f, "%.1f", "Y-X-Z degrees that face the rigs down the camera's -Z (180 Y for a Blender -Y rig).");
        r.Name("Weapon Socket", s.WeaponSocket, ctx.Bones, ctx.Arms != nullptr,
               "Bone on the ARMS rig the gun rides (empty = the weapon shares the arms' pose).", "The arms rig has no bone named '%s'.");
        r.Text("Weapon Root", s.WeaponRoot, "Bone on the WEAPON rig that lands on the socket.");
        r.Vec3("Mount Rotation", s.WeaponMountRotation, 0.5f, "%.1f", "Fixed socket -> weapon root rotation, Y-X-Z degrees.");
    }

    const auto& rc = s.Procedural.Recoil;
    std::snprintf(summary, sizeof summary, "%s", rc.Enabled ? (rc.ShakeAmount > 0.0f ? "kick, climb, shake" : "kick, climb") : "off");
    if (r.Section(ICON_FA_BURST, "Recoil", summary)) DrawRecoil(r, s.Procedural.Recoil, g.RoundsPerMinute);

    if (r.Section(ICON_FA_PERSON_RUNNING, "Movement", "sway, bob, breathing, jump, camera, walls")) DrawMovement(r, s.Procedural, ctx);

    std::snprintf(summary, sizeof summary, "%s", s.Procedural.IK.Enabled ? "hands on the gun" : "off");
    if (r.Section(ICON_FA_HAND, "IK", summary)) DrawIK(r, s.Procedural.IK, ctx);

    if (ctx.LoadArms && m_AssetsPtr && !m_AssetsPtr->LoadModel(armsPath))
        Log::Warn("Couldn't load the arms model '" + s.ArmsModel + "' to check bone names.");

    ImGui::Spacing();
    r.Note("Edits save immediately. Numbers apply live in Play; the rigs and controller on the next Play.");
    if (changed) {
        cache.Undo.push_back(cache.Saved);
        if (cache.Undo.size() > 100) cache.Undo.erase(cache.Undo.begin());
        cache.Redo.clear();
        save(s);
        cache.Saved = s.ToJsonString();
    }
}
