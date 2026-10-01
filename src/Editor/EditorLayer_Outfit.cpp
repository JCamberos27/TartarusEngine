// The Character Outfit component's editor (docs/CHARACTER_OUTFITS.md): gender and race up top, a tab per slot,
// the slot's items as a list, a 3D preview of the item you point at (or the one worn) that you can spin, the
// equipped item's colourways by name, Randomize (with locks) and presets. Everything goes through
// OutfitSystem, after PushUndo.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "OutfitSystem.h"
#include "ProjectPaths.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace EditorUIPrimitives;

namespace {

// Per-object editor state (not saved).
struct OutfitUiState {
    std::string Slot;               // the open tab
    char Search[64] = {};
    bool ShowVariants = false;      // list the cuts the rules pick (Bobcut_Cap, Jeans_Inboots) too
    std::vector<std::string> Notes; // what the last change's rules did
    char PresetName[64] = "My Outfit";
    // The item preview's orbit camera and shading; reframed when the previewed model changes.
    std::string PreviewFor;
    float Yaw = 0.6f, Pitch = 0.25f, Distance = 1.0f;
    bool Dragging = false;
    int Shading = 0; // ModelPreviewRenderer::Shading
};
std::unordered_map<entt::id_type, OutfitUiState> g_OutfitUi;

// Models the preview has asked for, loading in the background.
std::map<std::string, AssetLibrary::AsyncHandle> g_PreviewLoads;

// What the options of a colourway set have in common ("M_Pants_Cargo_Camo", "M_Pants_Cargo_Black" ->
// "Pants Cargo"), so each is labelled by what sets it apart ("Camo", "Black").
std::vector<std::string> ColourwayLabels(const std::vector<std::string>& options) {
    std::vector<std::vector<std::string>> words;
    for (const auto& o : options) {
        std::vector<std::string> w;
        std::string cur;
        for (char c : Wardrobe::PrettyName(Wardrobe::Stem(o)) + " ") {
            if (c == ' ') { if (!cur.empty()) w.push_back(cur); cur.clear(); }
            else cur += c;
        }
        words.push_back(std::move(w));
    }
    size_t common = words.empty() ? 0 : words[0].size();
    for (const auto& w : words) {
        size_t k = 0;
        while (k < common && k < w.size() && w[k] == words[0][k]) ++k;
        common = k;
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string label;
        for (size_t k = std::min(common, words[i].size() ? words[i].size() - 1 : 0); k < words[i].size(); ++k)
            label += (label.empty() ? "" : " ") + words[i][k];
        out.push_back(label.empty() ? Wardrobe::PrettyName(Wardrobe::Stem(options[i])) : label);
    }
    return out;
}

bool ContainsI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string h = hay, n = needle;
    for (char& c : h) c = (char)std::tolower((unsigned char)c);
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

bool SamePath(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = (char)std::tolower((unsigned char)a[i]), y = (char)std::tolower((unsigned char)b[i]);
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y) return false;
    }
    return true;
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

} // namespace

void EditorLayer::DrawCharacterOutfitEditor(World& world, entt::entity root) {
    auto& reg = world.Registry;
    auto* outfit = reg.try_get<CharacterOutfitComponent>(root);
    if (!outfit || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;
    OutfitUiState& ui = g_OutfitUi[entt::to_integral(root)];

    std::string error;
    auto cat = OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, false, &error);
    if (!cat) {
        ImGui::TextColored(WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", error.c_str());
        if (ImGui::Button(ICON_FA_ROTATE "  Try again")) OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, true);
        return;
    }
    auto report = [&](const OutfitSystem::Result& r) {
        if (!r.Ok) Log::Warn("Character Outfit: " + r.Error);
        ui.Notes = r.Notes;
    };

    auto pieces = OutfitSystem::Pieces(world, root);
    if (pieces.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No outfit yet. Build a body from the wardrobe, or adopt the pieces already under this object.");
        ImGui::Spacing();
        if (PrimaryButton(ICON_FA_PERSON "  Build body")) {
            PushUndo(world, "Build Outfit Body");
            // Take over the body parts already under this object first (an existing Player Body's
            // Torso, Legs, Head ...). Apply only sees tagged pieces, so without this it built a
            // second body on top of the untagged one - a female body hidden inside the male one.
            const int adopted = OutfitSystem::AdoptExisting(world, assets, root);
            report(OutfitSystem::Submit(world, assets, root, OutfitSystem::CurrentRequest(world, root)));
            if (adopted) ui.Notes.insert(ui.Notes.begin(), "Adopted " + std::to_string(adopted) + " existing pieces");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES "  Adopt existing")) {
            PushUndo(world, "Adopt Outfit Pieces");
            const int n = OutfitSystem::AdoptExisting(world, assets, root);
            ui.Notes = {"Adopted " + std::to_string(n) + " pieces"};
        }
        return;
    }

    const Wardrobe::Gender sex = outfit->Gender == 1 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
    const ImVec4 accent = AccentColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();

    // --- Identity: gender pills, race swatches ---------------------------------------------------
    ImGui::Spacing();
    {
        const char* labels[2] = {ICON_FA_MARS "  Male", ICON_FA_VENUS "  Female"};
        for (int g = 0; g < 2; ++g) {
            if (g) ImGui::SameLine(0.0f, 2.0f);
            const bool on = outfit->Gender == g;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? accent : style.Colors[ImGuiCol_FrameBg]);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
            if (ImGui::Button(labels[g], ImVec2(96.0f, 0.0f)) && !on) {
                PushUndo(world, "Change Gender");
                report(OutfitSystem::SetGender(world, assets, root, (Wardrobe::Gender)g));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        // Randomize and the menu on the right.
        const float right = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(right - 2.0f * ImGui::GetFrameHeight() - style.ItemSpacing.x);
        if (EditorInternal::ActionButton(ICON_FA_DICE, "Randomize (locked slots stay)")) {
            PushUndo(world, "Randomize Outfit");
            report(OutfitSystem::Randomize(world, assets, root,
                                           (std::uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()));
        }
        ImGui::SameLine();
        if (EditorInternal::ActionButton(ICON_FA_ELLIPSIS_VERTICAL, "Presets and tools")) ImGui::OpenPopup("##outfitMenu");
        if (ImGui::BeginPopup("##outfitMenu")) {
            ImGui::TextDisabled("Presets");
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("##presetName", ui.PresetName, sizeof(ui.PresetName));
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                const std::string path = "assets/Characters/Outfits/" + std::string(ui.PresetName) + ".outfit";
                std::error_code ec;
                std::filesystem::create_directories(ProjectPaths::Resolve("assets/Characters/Outfits"), ec);
                std::string err;
                ui.Notes = {OutfitSystem::SavePreset(world, assets, root, path, &err) ? "Saved " + path : err};
            }
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(ProjectPaths::Resolve("assets"), ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->path().extension() != ".outfit") continue;
                const std::string rel = ProjectPaths::Relativize(it->path().string());
                if (ImGui::MenuItem((ICON_FA_PERSON_DRESS "  " + it->path().stem().string()).c_str())) {
                    PushUndo(world, "Load Outfit Preset");
                    report(OutfitSystem::LoadPreset(world, assets, root, rel));
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES "  Adopt untagged children")) {
                PushUndo(world, "Adopt Outfit Pieces");
                ui.Notes = {"Adopted " + std::to_string(OutfitSystem::AdoptExisting(world, assets, root)) + " pieces"};
            }
            if (ImGui::MenuItem(ICON_FA_ERASER "  Take everything off")) {
                PushUndo(world, "Clear Outfit");
                Wardrobe::Request req = OutfitSystem::CurrentRequest(world, root);
                req.Items.clear();
                report(OutfitSystem::Submit(world, assets, root, req));
            }
            if (ImGui::MenuItem(ICON_FA_ROTATE "  Rescan wardrobe")) OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, true);
            ImGui::MenuItem("Show fitted cuts", nullptr, &ui.ShowVariants);
            ImGui::EndPopup();
        }

        // Race: round skin-tone swatches.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Skin");
        ImGui::SameLine(60.0f);
        const float r = ImGui::GetFrameHeight() * 0.5f;
        for (const auto& race : cat->W.Body(sex).Races) {
            ImGui::PushID(race.Name.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##race", ImVec2(2.0f * r + 4.0f, 2.0f * r + 4.0f)) && race.Name != outfit->Race) {
                PushUndo(world, "Change Race");
                report(OutfitSystem::SetRace(world, assets, root, race.Name));
            }
            const ImVec2 c(p.x + r + 2.0f, p.y + r + 2.0f);
            dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImVec4(race.Tone.x, race.Tone.y, race.Tone.z, 1.0f)), 24);
            if (race.Name == outfit->Race) dl->AddCircle(c, r + 2.0f, Col(accent), 24, 2.5f);
            else if (ImGui::IsItemHovered()) dl->AddCircle(c, r + 1.5f, Col(style.Colors[ImGuiCol_Text], 0.5f), 24, 1.5f);
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", race.Name.c_str());
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PopID();
        }
        ImGui::TextDisabled("%s", outfit->Race.c_str());

        // A change still loading: the current pieces stay until everything it needs is in memory.
        int done = 0, total = 0;
        if (OutfitSystem::IsPending(world, root, &done, &total))
            ImGui::TextColored(InfoColor(), ICON_FA_SPINNER "  Loading... %d / %d", done, total);
    }

    // --- Slot tabs ---------------------------------------------------------------------------------
    std::vector<const Wardrobe::SlotDef*> slots;
    for (const auto& s : cat->W.Slots)
        if (!cat->ForSlot(s.Id, sex).empty()) slots.push_back(&s);
    if (slots.empty()) return;
    if (std::none_of(slots.begin(), slots.end(), [&](const Wardrobe::SlotDef* s) { return s->Id == ui.Slot; }))
        ui.Slot = slots.front()->Id;
    ImGui::Spacing();
    {
        // A text tab per slot, flowing onto more lines as needed; a dot marks the slots with something on.
        const float avail = ImGui::GetContentRegionAvail().x;
        float x = 0.0f;
        for (const auto* s : slots) {
            const float w = ImGui::CalcTextSize(s->Label.c_str()).x + style.FramePadding.x * 2.0f + 8.0f;
            if (x > 0.0f && x + w > avail) x = 0.0f;
            else if (x > 0.0f) ImGui::SameLine(0.0f, 3.0f);
            x += w + 3.0f;
            const bool on = s->Id == ui.Slot, worn = pieces.count(s->Id) > 0;
            ImGui::PushID(s->Id.c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, on ? accent : style.Colors[ImGuiCol_FrameBg]);
            if (ImGui::Button(s->Label.c_str(), ImVec2(w, 0.0f))) ui.Slot = s->Id;
            ImGui::PopStyleColor();
            if (worn) {
                const ImVec2 mx = ImGui::GetItemRectMax();
                dl->AddCircleFilled(ImVec2(mx.x - 4.0f, ImGui::GetItemRectMin().y + 4.0f), 2.5f, Col(SuccessColor()));
            }
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s%s", s->Label.c_str(), worn ? " (worn)" : "");
            ImGui::PopID();
        }
    }

    // --- The open slot -----------------------------------------------------------------------------
    const Wardrobe::SlotDef* slot = cat->W.Slot(ui.Slot);
    auto worn = pieces.find(ui.Slot);
    const std::string current = worn != pieces.end() ? reg.get<OutfitPieceComponent>(worn->second).Item : std::string();
    const Wardrobe::Item* wornItem = current.empty() ? nullptr : cat->Find(current);
    // What's worn in the other slots, to mark the cards that don't go with it (Wardrobe::Conflicts).
    std::vector<const Wardrobe::Item*> others;
    for (const auto& [s, e] : pieces)
        if (s != ui.Slot)
            if (const auto* it = cat->Find(reg.get<OutfitPieceComponent>(e).Item)) others.push_back(it);
    // The first worn item `it` can't go with (hard: equipping takes it off) or only looks odd with.
    auto clashWith = [&](const Wardrobe::Item& it, bool& hard) -> const Wardrobe::Item* {
        for (const auto* o : others)
            if (Wardrobe::Conflicts(cat->W, it, *o, false)) { hard = true; return o; }
        for (const auto* o : others)
            if (Wardrobe::Conflicts(cat->W, it, *o, true)) { hard = false; return o; }
        return nullptr;
    };
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(slot->Label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", wornItem ? wornItem->Name.c_str() : "none");
    const float lockX = ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight();
    if (pieces.count(ui.Slot)) {
        const float removeW = ImGui::CalcTextSize("Remove").x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(lockX - style.ItemSpacing.x - removeW);
        const ImVec4 danger = DangerColor();
        ImGui::PushStyleColor(ImGuiCol_Button, danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(danger.x + 0.12f, 1.0f), danger.y + 0.08f, danger.z + 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(danger.x * 0.8f, danger.y * 0.8f, danger.z * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Remove##outfitRemove")) {
            PushUndo(world, "Remove " + slot->Label);
            report(OutfitSystem::Equip(world, assets, root, ui.Slot, std::string()));
        }
        ImGui::PopStyleColor(4);
    }
    {
        std::vector<std::string> locks = OutfitSystem::ParseLocks(outfit->Locks);
        const bool locked = std::find(locks.begin(), locks.end(), ui.Slot) != locks.end();
        ImGui::SameLine(lockX);
        if (EditorInternal::ActionButton(locked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, locked ? "Locked: Randomize keeps it" : "Randomize may change it")) {
            PushUndo(world, "Lock Outfit Slot");
            if (locked) locks.erase(std::remove(locks.begin(), locks.end(), ui.Slot), locks.end());
            else locks.push_back(ui.Slot);
            std::string joined;
            for (const auto& l : locks) joined += (joined.empty() ? "" : ", ") + l;
            outfit->Locks = joined;
        }
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##outfitSearch", ICON_FA_MAGNIFYING_GLASS "  Search", ui.Search, sizeof(ui.Search));

    // The slot's items, as a list. Pointing at one previews it below; clicking puts it on (Remove above takes it off).
    std::vector<const Wardrobe::Item*> items;
    for (const auto* it : cat->ForSlot(ui.Slot, sex))
        if ((ui.ShowVariants || !it->Variant || it == wornItem) && ContainsI(it->Name, ui.Search)) items.push_back(it);
    const Wardrobe::Item* hoveredItem = nullptr;
    const float row = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("##outfitList", ImVec2(0.0f, std::min(260.0f, row * (float)items.size() + 6.0f)), true);
    for (size_t i = 0; i < items.size(); ++i) {
        const Wardrobe::Item* it = items[i];
        ImGui::PushID((int)i);
        const bool selected = it == wornItem;
        if (ImGui::Selectable(it->Name.c_str(), selected, 0, ImVec2(0.0f, row - ImGui::GetStyle().ItemSpacing.y)) && !selected) {
            PushUndo(world, "Equip " + it->Name);
            report(OutfitSystem::Equip(world, assets, root, ui.Slot, it->Path));
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) hoveredItem = it;
        // On the right: a clash with what's worn (amber: one comes off; grey: an odd pairing), a fitted cut.
        bool hardClash = false;
        const Wardrobe::Item* clash = !selected ? clashWith(*it, hardClash) : nullptr;
        std::string tag = it->Variant ? "fitted cut" : "";
        if (clash) tag = hardClash ? "takes off " + clash->Name : "odd with " + clash->Name;
        if (!tag.empty()) {
            const ImVec2 ts = ImGui::CalcTextSize(tag.c_str());
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - ts.x - 6.0f, (mn.y + mx.y - ts.y) * 0.5f),
                                                Col(clash && hardClash ? WarningColor() : style.Colors[ImGuiCol_TextDisabled]), tag.c_str());
        }
        if (hovered) EditorUI::SetTooltip("%s", it->Path.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    // --- Preview: the item pointed at in the list, else the one worn --------------------------------
    const Wardrobe::Item* shown = hoveredItem ? hoveredItem : wornItem;
    if (shown) {
        const std::string abs = ProjectPaths::Resolve(shown->Path);
        std::shared_ptr<Model> model;
        auto load = g_PreviewLoads.find(abs);
        if (load == g_PreviewLoads.end()) load = g_PreviewLoads.emplace(abs, assets.RequestModelAsync(abs)).first;
        if (AssetLibrary::IsReady(load->second)) model = assets.LoadModel(abs);
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = std::clamp(width * 0.8f, 160.0f, 300.0f);
        if (model) {
            const float fit = ModelPreviewRenderer::ComputeFramingDistance(*model) * 0.72f;
            if (ui.PreviewFor != abs) { ui.PreviewFor = abs; ui.Distance = fit; }
            // The worn one with the piece's own materials (its colourway), anything else as it comes.
            std::vector<std::shared_ptr<MaterialAsset>> slotsMats;
            if (shown == wornItem && worn != pieces.end()) {
                if (const auto* rc = reg.try_get<RenderableComponent>(worn->second)) slotsMats = rc->Materials;
            } else {
                assets.ApplyMaterialRemap(*model, slotsMats);
            }
            const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0.0f ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
            const unsigned handle = m_OutfitPreview.Render(*model, ui.Yaw, ui.Pitch, ui.Distance, (int)(width * scale),
                                                           (int)(height * scale), slotsMats,
                                                           (ModelPreviewRenderer::Shading)ui.Shading, true);
            ImGui::Image((ImTextureID)(intptr_t)handle, ImVec2(width, height), ImVec2(0, 1), ImVec2(1, 0));
            // Drag to spin, scroll to zoom (tracked by hand: an Image is never "active").
            const bool over = ImGui::IsItemHovered();
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ui.Dragging = true;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) ui.Dragging = false;
            if (over && ImGui::GetIO().MouseWheel != 0.0f) ui.Distance *= 1.0f - ImGui::GetIO().MouseWheel * 0.1f;
            if (ui.Dragging) {
                ui.Yaw += ImGui::GetIO().MouseDelta.x * 0.01f;
                ui.Pitch = std::clamp(ui.Pitch - ImGui::GetIO().MouseDelta.y * 0.01f, -1.4f, 1.4f);
            }
            ui.Distance = std::clamp(ui.Distance, fit * 0.2f, fit * 6.0f);
            // The name over the image's top-left corner.
            const ImVec2 mn = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 8.0f, mn.y + 6.0f), Col(style.Colors[ImGuiCol_Text]),
                                                (shown->Name + (shown == wornItem ? "  (worn)" : "")).c_str());
            // Shading and reset, under it.
            const char* modes[3] = {"Lit", "Unlit", "Wireframe"};
            for (int m = 0; m < 3; ++m) {
                if (m) ImGui::SameLine(0.0f, 2.0f);
                const bool on = ui.Shading == m;
                ImGui::PushStyleColor(ImGuiCol_Button, on ? accent : style.Colors[ImGuiCol_FrameBg]);
                if (ImGui::Button(modes[m])) ui.Shading = m;
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Drag to spin, scroll to zoom");
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Reset view").x - style.FramePadding.x * 2.0f);
            if (ImGui::Button("Reset view")) { ui.Yaw = 0.6f; ui.Pitch = 0.25f; ui.Distance = fit; }
        } else {
            ImGui::Dummy(ImVec2(width, height));
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, Col(style.Colors[ImGuiCol_FrameBg]), 4.0f);
            const char* msg = "Loading...";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            ImGui::GetWindowDrawList()->AddText(ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
                                                Col(style.Colors[ImGuiCol_TextDisabled]), msg);
        }
    }

    // --- Colourways of the worn item, by name ------------------------------------------------------
    if (worn != pieces.end()) {
        const auto groups = OutfitSystem::ColourGroups(world, assets, worn->second);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            ImGui::PushID(g.Source.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("%s", groups.size() > 1 ? ("Colour " + std::to_string(gi + 1)).c_str() : "Colour");
            const std::vector<std::string> labels = ColourwayLabels(g.Options);
            const float avail = ImGui::GetContentRegionAvail().x;
            float x = 0.0f;
            for (size_t k = 0; k < g.Options.size(); ++k) {
                const float w = ImGui::CalcTextSize(labels[k].c_str()).x + style.FramePadding.x * 2.0f + 6.0f;
                if (x > 0.0f && x + w > avail) x = 0.0f;
                else if (x > 0.0f) ImGui::SameLine(0.0f, 3.0f);
                x += w + 3.0f;
                ImGui::PushID((int)k);
                const bool on = SamePath(g.Options[k], g.Current);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? accent : style.Colors[ImGuiCol_FrameBg]);
                if (ImGui::Button(labels[k].c_str(), ImVec2(w, 0.0f)) && !on) {
                    PushUndo(world, "Change Colourway");
                    OutfitSystem::SubmitColourway(world, assets, root, worn->second, g.Source, g.Options[k]);
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", Wardrobe::Stem(g.Options[k]).c_str());
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }

    // What the rules did, and the skin hidden under the clothes.
    for (const auto& n : ui.Notes) ImGui::TextColored(InfoColor(), ICON_FA_CIRCLE_INFO "  %s", n.c_str());
    int hidden = 0, total = 0;
    for (const auto& [s, e] : pieces)
        if (const auto* h = reg.try_get<OutfitHideTag>(e)) { hidden += h->Hidden; total += h->Total; }
    bool autoHide = outfit->AutoHide;
    if (ImGui::Checkbox("Hide skin under clothes", &autoHide)) {
        PushUndo(world, "Toggle Outfit Skin Hiding");
        outfit->AutoHide = autoHide;
    }
    if (total) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d vertices hidden)", hidden);
    }
    const bool animated = std::any_of(pieces.begin(), pieces.end(), [&](const auto& kv) {
        return reg.all_of<AnimatorControllerComponent>(kv.second);
    });
    if (!animated)
        ImGui::TextColored(WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  No piece has an Animator Controller - the body won't animate.");
}
