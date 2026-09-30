// The Character Outfit component's editor (CHARACTER_OUTFITS.md): gender and race up top, a tab per slot,
// a card grid of the slot's items with thumbnails, the equipped item's colourways, Randomize (with locks)
// and presets. Everything goes through OutfitSystem, after PushUndo.
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
};
std::unordered_map<entt::id_type, OutfitUiState> g_OutfitUi;

// Thumbnails of items nothing has loaded: the cached render if there is one, else the model is loaded
// in the background (cards on screen only) and rendered once it's in - after that the cache has it.
struct ItemThumbs {
    std::map<std::string, int> AskedFrame;       // path -> the frame it was first asked for
    std::map<std::string, unsigned> Rendered;    // path -> ModelThumbnail texture
    std::map<std::string, AssetLibrary::AsyncHandle> Loading; // path -> its background load
    int Frame = 0, LoadsThisFrame = 0;
};
ItemThumbs g_Thumbs;

// Colourway swatches' materials, loaded in the background the first time they're shown.
std::map<std::string, AssetLibrary::AsyncHandle> g_SwatchLoads;

const char* SlotIcon(const std::string& icon) {
    if (icon == "mask") return ICON_FA_USER_NINJA;
    if (icon == "hair") return ICON_FA_SCISSORS;
    if (icon == "beard") return ICON_FA_USER_TIE;
    if (icon == "hat") return ICON_FA_HAT_COWBOY;
    if (icon == "glasses") return ICON_FA_GLASSES;
    if (icon == "shirt") return ICON_FA_SHIRT;
    if (icon == "jacket") return ICON_FA_VEST;
    if (icon == "collar") return ICON_FA_VEST_PATCHES;
    if (icon == "pants") return ICON_FA_PERSON;
    if (icon == "shoe") return ICON_FA_SHOE_PRINTS;
    if (icon == "bag") return ICON_FA_BAG_SHOPPING;
    if (icon == "watch") return ICON_FA_CLOCK;
    if (icon == "headphones") return ICON_FA_HEADPHONES;
    if (icon == "socks") return ICON_FA_SOCKS;
    return ICON_FA_CIRCLE;
}

bool ContainsI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    std::string h = hay, n = needle;
    for (char& c : h) c = (char)std::tolower((unsigned char)c);
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

} // namespace

void EditorLayer::DrawCharacterOutfitEditor(World& world, entt::entity root) {
    auto& reg = world.Registry;
    auto* outfit = reg.try_get<CharacterOutfitComponent>(root);
    if (!outfit || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;
    OutfitUiState& ui = g_OutfitUi[entt::to_integral(root)];

    const int frame = ImGui::GetFrameCount();
    if (g_Thumbs.Frame != frame) { g_Thumbs.Frame = frame; g_Thumbs.LoadsThisFrame = 0; }

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
        const float size = ImGui::GetFrameHeight() + 6.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        float x = 0.0f;
        for (const auto* s : slots) {
            if (x > 0.0f && x + size > avail) x = 0.0f;
            else if (x > 0.0f) ImGui::SameLine(0.0f, 3.0f);
            x += size + 3.0f;
            const bool on = s->Id == ui.Slot, worn = pieces.count(s->Id) > 0;
            ImGui::PushID(s->Id.c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, on ? accent : style.Colors[ImGuiCol_FrameBg]);
            if (ImGui::Button(SlotIcon(s->Icon), ImVec2(size, size))) ui.Slot = s->Id;
            ImGui::PopStyleColor();
            if (worn) {
                const ImVec2 mx = ImGui::GetItemRectMax();
                dl->AddCircleFilled(ImVec2(mx.x - 5.0f, ImGui::GetItemRectMin().y + 5.0f), 3.0f, Col(SuccessColor()));
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
    ImGui::Text("%s  %s", SlotIcon(slot->Icon), slot->Label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", wornItem ? wornItem->Name.c_str() : "none");
    {
        std::vector<std::string> locks = OutfitSystem::ParseLocks(outfit->Locks);
        const bool locked = std::find(locks.begin(), locks.end(), ui.Slot) != locks.end();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
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

    // Card grid.
    const float card = 92.0f, thumb = 78.0f, gap = 6.0f;
    // Model thumbnails frame the model's bounding sphere with headroom (ModelPreviewRenderer::
    // ComputeFramingDistance): the sphere spans the middle 59% of the image, so the rest is empty
    // background. Show only the middle 64% - the item comes out ~1.6x bigger and nothing is cut off.
    static constexpr float kCrop = 0.18f;
    auto thumbUV = [](bool flip, ImVec2& uv0, ImVec2& uv1) {
        uv0 = ImVec2(kCrop, flip ? 1.0f - kCrop : kCrop);
        uv1 = ImVec2(1.0f - kCrop, flip ? kCrop : 1.0f - kCrop);
    };
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, (int)((width + gap) / (card + gap)));
    std::vector<const Wardrobe::Item*> items;
    for (const auto* it : cat->ForSlot(ui.Slot, sex))
        if ((ui.ShowVariants || !it->Variant || it == wornItem) && ContainsI(it->Name, ui.Search)) items.push_back(it);

    auto thumbFor = [&](const Wardrobe::Item& it, bool visible, bool& flip) -> unsigned {
        flip = true;
        if (auto r = g_Thumbs.Rendered.find(it.Path); r != g_Thumbs.Rendered.end()) return r->second;
        const AssetThumbnailLoader::Thumb t = m_AssetThumbs.Get(ProjectPaths::Resolve(it.Path), AssetThumbnailLoader::Source::Cached);
        if (t.Tex) { flip = t.FlipV; return t.Tex; }
        auto asked = g_Thumbs.AskedFrame.emplace(it.Path, frame).first;
        // No cached render after a moment: load the model in the background (on screen only), then
        // render it once it's in (one a frame - that's a draw, no loading).
        if (!visible || frame - asked->second <= 20) return 0;
        const std::string abs = ProjectPaths::Resolve(it.Path);
        auto loading = g_Thumbs.Loading.find(it.Path);
        if (loading == g_Thumbs.Loading.end()) loading = g_Thumbs.Loading.emplace(it.Path, assets.RequestModelAsync(abs)).first;
        if (AssetLibrary::IsReady(loading->second) && g_Thumbs.LoadsThisFrame < 1) {
            ++g_Thumbs.LoadsThisFrame;
            g_Thumbs.Loading.erase(loading);
            if (auto model = assets.LoadModel(abs))
                if (const unsigned tex = ModelThumbnail(*model)) { g_Thumbs.Rendered[it.Path] = tex; return tex; }
        }
        return 0;
    };

    ImGui::BeginChild("##outfitGrid", ImVec2(0.0f, std::min(360.0f, (card + 18.0f + gap) * (float)((items.size() + 1 + (size_t)columns - 1) / (size_t)columns) + 4.0f)),
                      false);
    ImDrawList* gdl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i <= items.size(); ++i) {
        const Wardrobe::Item* it = i == 0 ? nullptr : items[i - 1];
        if (i % (size_t)columns) ImGui::SameLine(0.0f, gap);
        ImGui::PushID((int)i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = it == wornItem;
        if (ImGui::InvisibleButton("##card", ImVec2(card, card + 18.0f)) && !selected) {
            PushUndo(world, it ? "Equip " + it->Name : "Remove " + slot->Label);
            report(OutfitSystem::Equip(world, assets, root, ui.Slot, it ? it->Path : std::string()));
        }
        const bool hovered = ImGui::IsItemHovered();
        const bool visible = ImGui::IsItemVisible();
        const ImVec2 q(p.x + card, p.y + card + 18.0f);
        gdl->AddRectFilled(p, q, Col(style.Colors[hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg]), 6.0f);
        if (selected) gdl->AddRect(p, q, Col(accent), 6.0f, 0, 2.5f);
        const ImVec2 t0(p.x + (card - thumb) * 0.5f, p.y + 6.0f), t1(t0.x + thumb, t0.y + thumb);
        if (it) {
            bool flip = true;
            if (const unsigned tex = thumbFor(*it, visible, flip)) {
                ImVec2 uv0, uv1;
                thumbUV(flip, uv0, uv1);
                gdl->AddImageRounded((ImTextureID)(intptr_t)tex, t0, t1, uv0, uv1, IM_COL32_WHITE, 4.0f);
            } else {
                const ImVec2 sz = ImGui::CalcTextSize(SlotIcon(slot->Icon));
                gdl->AddText(ImVec2((t0.x + t1.x - sz.x) * 0.5f, (t0.y + t1.y - sz.y) * 0.5f), Col(style.Colors[ImGuiCol_TextDisabled]), SlotIcon(slot->Icon));
            }
        } else {
            const char* none = ICON_FA_BAN;
            const ImVec2 sz = ImGui::CalcTextSize(none);
            gdl->AddText(ImVec2((t0.x + t1.x - sz.x) * 0.5f, (t0.y + t1.y - sz.y) * 0.5f), Col(style.Colors[ImGuiCol_TextDisabled]), none);
        }
        bool hardClash = false;
        const Wardrobe::Item* clash = it && !selected ? clashWith(*it, hardClash) : nullptr;
        if (clash) {
            const ImVec2 cs = ImGui::CalcTextSize(ICON_FA_TRIANGLE_EXCLAMATION);
            gdl->AddText(ImVec2(q.x - cs.x - 5.0f, p.y + 4.0f),
                         Col(hardClash ? WarningColor() : style.Colors[ImGuiCol_TextDisabled]), ICON_FA_TRIANGLE_EXCLAMATION);
        }
        const std::string label = it ? it->Name : "None";
        const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
        const ImVec4 clip(p.x + 3.0f, p.y, q.x - 3.0f, q.y);
        gdl->AddText(nullptr, 0.0f, ImVec2(std::max(p.x + 4.0f, p.x + (card - ls.x) * 0.5f), p.y + thumb + 10.0f),
                     Col(style.Colors[selected ? ImGuiCol_Text : ImGuiCol_TextDisabled]), label.c_str(), nullptr, 0.0f, &clip);
        if (hovered && it) {
            ImGui::BeginTooltip();
            bool flip = true;
            if (const unsigned tex = thumbFor(*it, true, flip)) {
                ImVec2 uv0, uv1;
                thumbUV(flip, uv0, uv1);
                ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(160.0f, 160.0f), uv0, uv1);
            }
            ImGui::TextUnformatted(it->Name.c_str());
            ImGui::TextDisabled("%s", it->Path.c_str());
            if (it->Variant) ImGui::TextDisabled("A fitted cut - usually picked by the wardrobe's rules");
            if (clash && hardClash) ImGui::TextColored(WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  Doesn't go with %s - one of them comes off", clash->Name.c_str());
            else if (clash) ImGui::TextDisabled(ICON_FA_TRIANGLE_EXCLAMATION "  An odd pairing with %s", clash->Name.c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Colourways of the worn item.
    if (worn != pieces.end()) {
        const auto groups = OutfitSystem::ColourGroups(world, assets, worn->second);
        for (const auto& g : groups) {
            ImGui::PushID(g.Source.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(ICON_FA_PALETTE);
            ImGui::SameLine();
            const float sw = 26.0f;
            const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for (size_t k = 0; k < g.Options.size(); ++k) {
                // Flow the swatches: on this line while they fit, else the next.
                if (k && ImGui::GetItemRectMax().x + 4.0f + sw <= rightEdge) ImGui::SameLine(0.0f, 4.0f);
                ImGui::PushID((int)k);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool on = g.Options[k] == g.Current;
                if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw)) && !on) {
                    PushUndo(world, "Change Colourway");
                    OutfitSystem::SubmitColourway(world, assets, root, worn->second, g.Source, g.Options[k]);
                }
                const ImVec2 c(p.x + sw * 0.5f, p.y + sw * 0.5f);
                // The swatch's material loads in the background; a plain ring until it's in.
                const std::string matPath = ProjectPaths::Resolve(g.Options[k]);
                auto load = g_SwatchLoads.find(matPath);
                if (load == g_SwatchLoads.end()) load = g_SwatchLoads.emplace(matPath, assets.RequestMaterialAsync(matPath)).first;
                auto mat = AssetLibrary::IsReady(load->second) ? assets.LoadMaterial(matPath) : nullptr;
                const unsigned tex = mat ? MaterialThumbnail(mat) : 0u;
                if (tex) dl->AddImageRounded((ImTextureID)(intptr_t)tex, ImVec2(c.x - sw * 0.5f + 2.0f, c.y - sw * 0.5f + 2.0f),
                                             ImVec2(c.x + sw * 0.5f - 2.0f, c.y + sw * 0.5f - 2.0f), ImVec2(0, 1), ImVec2(1, 0),
                                             IM_COL32_WHITE, sw * 0.5f);
                else if (mat) dl->AddCircleFilled(c, sw * 0.5f - 2.0f, ImGui::GetColorU32(ImVec4(mat->Mat.BaseColor.x, mat->Mat.BaseColor.y, mat->Mat.BaseColor.z, 1.0f)), 20);
                dl->AddCircle(c, sw * 0.5f - 1.0f, on ? Col(accent) : Col(style.Colors[ImGuiCol_Border]), 20, on ? 2.5f : 1.0f);
                if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", Wardrobe::PrettyName(Wardrobe::Stem(g.Options[k])).c_str());
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
