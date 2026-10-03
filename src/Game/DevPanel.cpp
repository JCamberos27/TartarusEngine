#include "DevPanel.h"

#include "AI/NpcDirector.h"
#include "Combat/PlayerVitals.h"
#include "FirstPersonPresentation.h"
#include "TimeService.h"
#include "imgui.h"

#include <algorithm>

void DevPanel::Reset(PlayerVitals& vitals, NpcDirector& npcs) {
    Open = false;
    InfiniteAmmo = false;
    Invisible = false;
    AiOverlay = false;
    vitals.GodMode = false;
    npcs.Frozen = false;
    npcs.HoldFire = false;
}

int DevPanel::KillAll(World& world, NpcDirector& npcs) {
    int killed = 0;
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        npcs.Kill(world, *up, glm::vec3(0.0f, 0.2f, -1.0f), up->Feet + glm::vec3(0.0f, 1.2f, 0.0f), 20.0f);
        ++killed;
    }
    return killed;
}

int DevPanel::RespawnSquad(World& world, AssetLibrary& assets, NpcDirector& npcs) {
    int alive = 0;
    for (const auto& up : npcs.Npcs()) if (up && !up->Dead) ++alive;
    const int spawns = (int)npcs.SpawnPoints().size();
    if (spawns <= 0) return 0;
    int made = 0;
    for (int i = 0; alive + made < npcs.SquadSize() && i < npcs.SquadSize() * 2; ++i)
        if (npcs.Spawn(world, assets, i % spawns) >= 0) ++made;
    return made;
}

void DevPanel::Draw(const Context& c) {
    if (!Open || !c.Npcs || !c.Vitals) return;
    ImGui::SetNextWindowPos(ImVec2(24.0f, 80.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Dev (F7)", &Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    NpcDirector& d = *c.Npcs;
    ImGui::SeparatorText("Player");
    ImGui::Checkbox("God mode (F8)", &c.Vitals->GodMode);
    ImGui::Checkbox("Infinite ammo", &InfiniteAmmo);
    ImGui::Checkbox("Invisible to AI", &Invisible);
    ImGui::SeparatorText("Squad");
    ImGui::Checkbox("Freeze AI", &d.Frozen);
    ImGui::Checkbox("AI holds fire", &d.HoldFire);
    float difficulty = d.Difficulty();
    if (ImGui::SliderFloat("Difficulty", &difficulty, 0.25f, 3.0f, "%.2f")) d.SetDifficulty(difficulty);
    if (ImGui::Button("Kill all") && c.WorldPtr) KillAll(*c.WorldPtr, d);
    ImGui::SameLine();
    if (ImGui::Button("Respawn squad") && c.WorldPtr && c.Assets) {
        KillAll(*c.WorldPtr, d);
        RespawnSquad(*c.WorldPtr, *c.Assets, d);
    }
    int alive = 0, known = 0;
    for (const auto& up : d.Npcs()) {
        if (!up || up->Dead) continue;
        ++alive;
        if (up->Mem.Known) ++known;
    }
    ImGui::Text("%d alive, %d in combat", alive, known);
    ImGui::SeparatorText("World");
    float scale = Time::TimeScale();
    if (ImGui::SliderFloat("Time scale", &scale, 0.05f, 2.0f, "%.2f")) Time::SetTimeScale(scale);
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) Time::SetTimeScale(1.0f);
    ImGui::Checkbox("AI debug overlay (F9)", &AiOverlay);
    ImGui::TextDisabled("F7 panel   F8 god mode   F9 AI overlay");
    ImGui::End();
}
