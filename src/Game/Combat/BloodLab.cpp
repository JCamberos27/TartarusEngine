#include "BloodLab.h"

#include "BloodFx.h"
#include "ScreenBlood.h"

#include <imgui.h>

void BloodLab::Draw(BloodFx& blood, ScreenBlood& screen, const Stats& s) {
    if (!Open) return;
    ImGui::SetNextWindowPos(ImVec2(360.0f, 80.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Blood Lab", &Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!s.BloodData || !s.KnifeData)
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Missing data: %s%s (docs/BLOOD_FX.md)", s.BloodData ? "" : "--import-blood-fx ",
                           s.KnifeData ? "" : "--import-knife-fx");
    if (!s.GoreMeshes) ImGui::TextDisabled("No headshot gore meshes (tools/knife_gore_bake.py)");

    ImGui::SeparatorText("At the crosshair");
    auto button = [](const char* label, const std::function<void()>& fn) {
        if (ImGui::Button(label) && fn) fn();
    };
    if (ImGui::Button("Rifle round") && Actions.FireAtCrosshair) Actions.FireAtCrosshair(38.0f, 1, false);
    ImGui::SameLine();
    if (ImGui::Button("Shotgun (9 pellets)") && Actions.FireAtCrosshair) Actions.FireAtCrosshair(14.0f, 9, false);
    ImGui::SameLine();
    if (ImGui::Button("Headshot kill") && Actions.FireAtCrosshair) Actions.FireAtCrosshair(400.0f, 1, true);
    if (ImGui::Button("Graze (stays in)") && Actions.FireAtCrosshair) Actions.FireAtCrosshair(8.0f, 1, false);
    ImGui::SameLine();
    button("Surface impact", Actions.ImpactAtCrosshair);
    button("Pool", Actions.PoolAtCrosshair);
    ImGui::SameLine();
    button("Wall spatter + drips", Actions.DripsAtCrosshair);
    if (ImGui::Button("Hurt me (25)") && Actions.Hurt) Actions.Hurt(25.0f);
    ImGui::SameLine();
    if (ImGui::Button("Hurt me (60)") && Actions.Hurt) Actions.Hurt(60.0f);

    ImGui::SeparatorText("Settings (live)");
    BloodFx::Settings& c = blood.Config;
    ImGui::Checkbox("Blood", &c.Enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Impact puffs", &c.ImpactPuffs);
    ImGui::SameLine();
    ImGui::Checkbox("Screen blood", &screen.Enabled);
    static const char* const kGore[] = {"Off", "Mild", "Full"};
    ImGui::Combo("Gore", &c.Gore, kGore, 3);
    ImGui::SliderFloat("Spray size", &c.Size, 0.2f, 3.0f, "%.2f");
    ImGui::SliderFloat("Energy scale", &c.EnergyScale, 0.2f, 3.0f, "%.2f");
    ImGui::SliderFloat("Dry seconds", &c.DrySeconds, 5.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Stain lifetime", &c.DecalLifetime, 10.0f, 1800.0f, "%.0f");
    ImGui::SliderInt("Max sprays", &c.MaxSprays, 1, 128);
    ImGui::SliderInt("Max stains", &c.MaxDecals, 8, 2048);
    ImGui::Checkbox("Pools", &c.Pools);
    ImGui::SameLine();
    ImGui::Checkbox("Body splats", &c.BodySplats);
    ImGui::SameLine();
    ImGui::Checkbox("Gear spatter", &c.GearSpatter);

    ImGui::SeparatorText("Alive / cost");
    ImGui::Text("%d sprays  %d stains  %d splats  %d sprites  %d holes", s.Sprays, s.Stains, s.Splats, s.Sprites, s.Holes);
    ImGui::Text("GPU  sprays %.3f ms   decals %.3f ms   sprites %.3f ms", s.SprayMs, s.DecalMs, s.SpriteMs);
    ImGui::Text("last hit: energy %.2f, %s", blood.LastEnergy(),
                blood.LastExited() ? (blood.LastExitFound() ? "exit (on the body)" : "exit (guessed)") : "no exit");
    if (ImGui::Button("Clear all blood") && Actions.ClearAll) Actions.ClearAll();
    ImGui::End();
}
