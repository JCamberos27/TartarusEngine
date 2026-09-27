// The Character Outfit component's editor (CHARACTER_OUTFITS.md): gender, race and a piece per slot,
// built and swapped by OutfitSystem.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "OutfitSystem.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <chrono>
#include <string>

using namespace EditorUIPrimitives;

void EditorLayer::DrawCharacterOutfitEditor(World& world, entt::entity root) {
    auto& reg = world.Registry;
    auto* outfit = reg.try_get<CharacterOutfitComponent>(root);
    if (!outfit || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;

    std::string error;
    auto cat = OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, false, &error);
    if (!cat) {
        ImGui::TextColored(WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", error.c_str());
        return;
    }
    auto report = [&](const OutfitSystem::Result& r) {
        if (!r.Ok) Log::Warn("Character Outfit: " + r.Error);
        for (const auto& n : r.Notes) Log::Info("Character Outfit: " + n);
    };

    const auto pieces = OutfitSystem::Pieces(world, root);
    if (pieces.empty()) {
        ImGui::TextWrapped("No outfit yet. Build a body, or adopt the pieces already under this object.");
        if (PrimaryButton(ICON_FA_PERSON "  Build body")) {
            PushUndo(world, "Build Outfit Body");
            report(OutfitSystem::Apply(world, assets, root, OutfitSystem::CurrentRequest(world, root)));
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES "  Adopt existing")) {
            PushUndo(world, "Adopt Outfit Pieces");
            const int n = OutfitSystem::AdoptExisting(world, assets, root);
            Log::Info("Character Outfit: adopted " + std::to_string(n) + " pieces");
        }
        return;
    }

    const Wardrobe::Gender sex = outfit->Gender == 1 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
    int gender = outfit->Gender;
    if (ImGui::Combo("Gender", &gender, "Male\0Female\0") && gender != outfit->Gender) {
        PushUndo(world, "Change Gender");
        report(OutfitSystem::SetGender(world, assets, root, (Wardrobe::Gender)gender));
    }
    if (ImGui::BeginCombo("Race", outfit->Race.c_str())) {
        for (const auto& race : cat->W.Body(sex).Races)
            if (ImGui::Selectable(race.Name.c_str(), race.Name == outfit->Race)) {
                PushUndo(world, "Change Race");
                report(OutfitSystem::SetRace(world, assets, root, race.Name));
            }
        ImGui::EndCombo();
    }

    for (const auto& slot : cat->W.Slots) {
        const auto items = cat->ForSlot(slot.Id, sex);
        if (items.empty()) continue;
        auto it = pieces.find(slot.Id);
        const std::string current = it != pieces.end() ? reg.get<OutfitPieceComponent>(it->second).Item : std::string();
        const Wardrobe::Item* worn = current.empty() ? nullptr : cat->Find(current);
        ImGui::PushID(slot.Id.c_str());
        if (ImGui::BeginCombo(slot.Label.c_str(), worn ? worn->Name.c_str() : "None")) {
            if (ImGui::Selectable("None", !worn)) {
                PushUndo(world, "Remove " + slot.Label);
                report(OutfitSystem::Equip(world, assets, root, slot.Id, std::string()));
            }
            for (const auto* item : items)
                if (ImGui::Selectable(item->Name.c_str(), item == worn)) {
                    PushUndo(world, "Equip " + item->Name);
                    report(OutfitSystem::Equip(world, assets, root, slot.Id, item->Path));
                }
            ImGui::EndCombo();
        }
        if (it != pieces.end())
            for (const auto& g : OutfitSystem::ColourGroups(world, assets, it->second)) {
                ImGui::PushID(g.Source.c_str());
                const std::string label = "   " + Wardrobe::PrettyName(Wardrobe::Stem(g.Current));
                if (ImGui::BeginCombo("##colour", label.c_str())) {
                    for (const auto& opt : g.Options)
                        if (ImGui::Selectable(Wardrobe::PrettyName(Wardrobe::Stem(opt)).c_str(), opt == g.Current)) {
                            PushUndo(world, "Change Colourway");
                            OutfitSystem::SetColourway(world, assets, it->second, g.Source, opt);
                        }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        ImGui::PopID();
    }

    if (ImGui::Button(ICON_FA_DICE "  Randomize")) {
        PushUndo(world, "Randomize Outfit");
        report(OutfitSystem::Randomize(world, assets, root,
                                       (std::uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE "  Rescan wardrobe")) OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, true);
}
