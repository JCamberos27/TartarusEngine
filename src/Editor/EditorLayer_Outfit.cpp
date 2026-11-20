// The Character Outfit component's editor: gender and race up top, a tab per slot,
// the slot's items as a list, a 3D preview of the item you point at (or the one worn) that you can spin, the
// equipped item's colourways by name, Randomize (with locks) and presets. Everything goes through
// OutfitSystem, after PushUndo.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "EditorSound.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "OutfitSystem.h"
#include "Core/Paths.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
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
std::unordered_map<entt::id_type, OutfitUiState> g_OutfitUi; // keyed by the whole entity value (id + version)
// The .outfit presets under assets/: listed when the presets menu opens, not walked every frame it is open.
std::vector<std::pair<std::string, std::string>> g_PresetFiles; // (project-relative path, menu label)
void ScanPresets() {
    g_PresetFiles.clear();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(ProjectPaths::Resolve("assets"), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".outfit")
            g_PresetFiles.emplace_back(ProjectPaths::Relativize(it->path().string()), ICON_FA_PERSON_DRESS "  " + it->path().stem().string());
}

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

// Case-insensitive substring test, without copying either string (it runs per item per frame while searching).
bool ContainsI(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const char* end = needle + std::strlen(needle);
    return std::search(hay.begin(), hay.end(), needle, end, [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    }) != hay.end();
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

// What every part of the editor works on, built once per frame by DrawCharacterOutfitEditor.
struct OutfitCtx {
    World& world;
    entt::registry& reg;
    AssetLibrary& assets;
    entt::entity root;
    CharacterOutfitComponent& outfit;
    const OutfitSystem::Catalog& cat;
    OutfitUiState& ui;
    std::function<void(const std::string&)> pushUndo; // EditorLayer::PushUndo on this world
    std::map<std::string, entt::entity> pieces;        // slot -> piece; re-read after any change
    Wardrobe::Gender sex = Wardrobe::Gender::Male;
    ImVec4 accent{};
    ImDrawList* dl = nullptr;
    const ImGuiStyle* style = nullptr;
    // What the open slot wears. Equip / Remove apply at once when everything is loaded, which can destroy
    // the old piece: they call ReadWorn so the rest of the frame never touches a gone entity.
    std::map<std::string, entt::entity>::iterator worn{};
    const Wardrobe::Item* wornItem = nullptr;

    void Report(const OutfitSystem::Result& r) {
        if (!r.Ok) Log::Warn("Character Outfit: " + r.Error);
        ui.Notes = r.Notes;
    }
    // The same, with the change's own sound (Blocked when it failed).
    void ReportSound(const OutfitSystem::Result& r, EditorSound::Event e) {
        EditorSound::Play(r.Ok ? e : EditorSound::Event::Blocked);
        Report(r);
    }
    void ReadPieces() { pieces = OutfitSystem::Pieces(world, root); }
    void ReadWorn() {
        worn = pieces.find(ui.Slot);
        const auto* piece = worn != pieces.end() ? reg.try_get<OutfitPieceComponent>(worn->second) : nullptr;
        const std::string current = piece ? piece->Item : std::string();
        wornItem = current.empty() ? nullptr : cat.Find(current);
    }
};

// No pieces yet: build a body or adopt the children already there.
void DrawOutfitStart(OutfitCtx& c) {
    ImGui::Spacing();
    ImGui::TextWrapped("No outfit yet. Build a body from the wardrobe, or adopt the pieces already under this object.");
    ImGui::Spacing();
    if (PrimaryButton(ICON_FA_PERSON "  Build body")) {
        c.pushUndo("Build Outfit Body");
        // Take over the body parts already under this object first (an existing Player Body's
        // Torso, Legs, Head ...). Apply only sees tagged pieces, so without this it built a
        // second body on top of the untagged one - a female body hidden inside the male one.
        const int adopted = OutfitSystem::AdoptExisting(c.world, c.assets, c.root);
        c.Report(OutfitSystem::Submit(c.world, c.assets, c.root, OutfitSystem::CurrentRequest(c.world, c.root)));
        if (adopted) c.ui.Notes.insert(c.ui.Notes.begin(), "Adopted " + std::to_string(adopted) + " existing pieces");
    }
    ImGui::SameLine();
    if (EditorUIPrimitives::SecondaryButton(ICON_FA_WAND_MAGIC_SPARKLES "  Adopt existing")) {
        c.pushUndo("Adopt Outfit Pieces");
        const int n = OutfitSystem::AdoptExisting(c.world, c.assets, c.root);
        c.ui.Notes = {"Adopted " + std::to_string(n) + " pieces"};
    }
}

// The "..." menu: save / load presets, adopt, take everything off, rescan, show fitted cuts.
void DrawOutfitMenu(OutfitCtx& c) {
    if (!ImGui::BeginPopup("##outfitMenu")) return;
    OutfitUiState& ui = c.ui;
    ImGui::TextDisabled("Presets");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##presetName", ui.PresetName, sizeof(ui.PresetName));
    ImGui::SameLine();
    bool save;
    {
        EditorSoundEvents::ScopedClickSilence quiet; // Save / Blocked instead of the click
        save = EditorUIPrimitives::PrimaryButton("Save");
    }
    if (save) {
        const std::string path = "assets/Characters/Outfits/" + std::string(ui.PresetName) + ".outfit";
        std::error_code ec;
        std::filesystem::create_directories(ProjectPaths::Resolve("assets/Characters/Outfits"), ec);
        std::string err;
        const bool saved = OutfitSystem::SavePreset(c.world, c.assets, c.root, path, &err);
        EditorSound::Play(saved ? EditorSound::Event::Save : EditorSound::Event::Blocked);
        ui.Notes = {saved ? "Saved " + path : err};
        if (saved) ScanPresets(); // the new preset shows in the list below at once
    }
    for (const auto& [rel, label] : g_PresetFiles) {
        if (ImGui::MenuItem(label.c_str())) {
            c.pushUndo("Load Outfit Preset");
            c.ReportSound(OutfitSystem::LoadPreset(c.world, c.assets, c.root, rel), EditorSound::Event::PresetApply);
            c.ReadPieces(); // the old pieces may be gone
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES "  Adopt untagged children")) {
        c.pushUndo("Adopt Outfit Pieces");
        ui.Notes = {"Adopted " + std::to_string(OutfitSystem::AdoptExisting(c.world, c.assets, c.root)) + " pieces"};
        c.ReadPieces();
    }
    if (ImGui::MenuItem(ICON_FA_ERASER "  Take everything off")) {
        c.pushUndo("Clear Outfit");
        Wardrobe::Request req = OutfitSystem::CurrentRequest(c.world, c.root);
        req.Items.clear();
        c.ReportSound(OutfitSystem::Submit(c.world, c.assets, c.root, req), EditorSound::Event::Unequip);
        c.ReadPieces();
    }
    if (ImGui::MenuItem(ICON_FA_ROTATE "  Rescan wardrobe")) OutfitSystem::LoadCatalog(c.assets, c.outfit.Wardrobe, true);
    ImGui::MenuItem("Show fitted cuts", nullptr, &ui.ShowVariants);
    ImGui::EndPopup();
}

// Identity: gender pills, Randomize + the menu, race swatches, the loading line.
void DrawOutfitIdentity(OutfitCtx& c) {
    const ImGuiStyle& style = *c.style;
    ImGui::Spacing();
    const char* labels[2] = {ICON_FA_MARS "  Male", ICON_FA_VENUS "  Female"};
    {
        int gender = c.outfit.Gender;
        if (EditorUIPrimitives::Segmented("##genderPills", &gender, labels, 2, nullptr, nullptr, EditorTheme::Px(96.0f))) {
            c.pushUndo("Change Gender");
            c.Report(OutfitSystem::SetGender(c.world, c.assets, c.root, (Wardrobe::Gender)gender));
            c.ReadPieces(); // a slot with no cut for the new gender is destroyed
        }
    }
    // Randomize and the menu on the right.
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(right - 2.0f * ImGui::GetFrameHeight() - style.ItemSpacing.x);
    bool randomize;
    {
        EditorSoundEvents::ScopedClickSilence quiet; // Randomize instead of the click
        randomize = EditorInternal::ActionButton(ICON_FA_DICE, "Randomize (locked slots stay)");
    }
    if (randomize) {
        c.pushUndo("Randomize Outfit");
        c.ReportSound(OutfitSystem::Randomize(c.world, c.assets, c.root,
                                              (std::uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()),
                      EditorSound::Event::Randomize);
        c.ReadPieces(); // Randomize can drop or replace pieces
    }
    ImGui::SameLine();
    if (EditorInternal::ActionButton(ICON_FA_ELLIPSIS_VERTICAL, "Presets and tools")) { ScanPresets(); ImGui::OpenPopup("##outfitMenu"); }
    DrawOutfitMenu(c);

    // Race: round skin-tone swatches.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Skin");
    ImGui::SameLine(60.0f);
    const float r = ImGui::GetFrameHeight() * 0.5f;
    for (const auto& race : c.cat.W.Body(c.sex).Races) {
        ImGui::PushID(race.Name.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##race", ImVec2(2.0f * r + 4.0f, 2.0f * r + 4.0f)) && race.Name != c.outfit.Race) {
            ClickSound(EditorSoundEvents::Event::SwatchPick);
            c.pushUndo("Change Race");
            c.Report(OutfitSystem::SetRace(c.world, c.assets, c.root, race.Name));
            c.ReadPieces();
        }
        const ImVec2 ctr(p.x + r + 2.0f, p.y + r + 2.0f);
        c.dl->AddCircleFilled(ctr, r, ImGui::GetColorU32(ImVec4(race.Tone.x, race.Tone.y, race.Tone.z, 1.0f)), 24);
        if (race.Name == c.outfit.Race) c.dl->AddCircle(ctr, r + 2.0f, Col(c.accent), 24, 2.5f);
        else if (ImGui::IsItemHovered()) c.dl->AddCircle(ctr, r + 1.5f, Col(style.Colors[ImGuiCol_Text], 0.5f), 24, 1.5f);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", race.Name.c_str());
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", c.outfit.Race.c_str());

    // A change still loading: the current pieces stay until everything it needs is in memory.
    int done = 0, total = 0;
    if (OutfitSystem::IsPending(c.world, c.root, &done, &total))
        ImGui::TextColored(InfoColor(), ICON_FA_SPINNER "  Loading... %d / %d", done, total);
}

// A text tab per slot that has items for this body, flowing onto more lines as needed; a dot marks the
// slots with something on. False when no slot has items.
bool DrawSlotTabs(OutfitCtx& c) {
    const ImGuiStyle& style = *c.style;
    std::vector<const Wardrobe::SlotDef*> slots;
    for (const auto& s : c.cat.W.Slots)
        if (!c.cat.ForSlot(s.Id, c.sex).empty()) slots.push_back(&s);
    if (slots.empty()) return false;
    if (std::none_of(slots.begin(), slots.end(), [&](const Wardrobe::SlotDef* s) { return s->Id == c.ui.Slot; }))
        c.ui.Slot = slots.front()->Id;
    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    for (const auto* s : slots) {
        const float w = ImGui::CalcTextSize(s->Label.c_str()).x + style.FramePadding.x * 2.0f + 8.0f;
        if (x > 0.0f && x + w > avail) x = 0.0f;
        else if (x > 0.0f) ImGui::SameLine(0.0f, 3.0f);
        x += w + 3.0f;
        const bool on = s->Id == c.ui.Slot, worn = c.pieces.count(s->Id) > 0;
        ImGui::PushID(s->Id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, on ? c.accent : style.Colors[ImGuiCol_FrameBg]);
        if (ImGui::Button(s->Label.c_str(), ImVec2(w, 0.0f))) c.ui.Slot = s->Id;
        ImGui::PopStyleColor();
        if (worn) {
            const ImVec2 mx = ImGui::GetItemRectMax();
            c.dl->AddCircleFilled(ImVec2(mx.x - 4.0f, ImGui::GetItemRectMin().y + 4.0f), 2.5f, Col(SuccessColor()));
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s%s", s->Label.c_str(), worn ? " (worn)" : "");
        ImGui::PopID();
    }
    return true;
}

// The open slot's header: its name and what it wears, Remove, the Randomize lock, the search box.
void DrawSlotHeader(OutfitCtx& c, const Wardrobe::SlotDef& slot) {
    const ImGuiStyle& style = *c.style;
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(slot.Label.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", c.wornItem ? c.wornItem->Name.c_str() : "none");
    const float lockX = ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight();
    if (c.pieces.count(c.ui.Slot)) {
        const float removeW = ImGui::CalcTextSize("Remove").x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(lockX - style.ItemSpacing.x - removeW);
        const ImVec4 danger = DangerColor();
        ImGui::PushStyleColor(ImGuiCol_Button, danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(danger.x + 0.12f, 1.0f), danger.y + 0.08f, danger.z + 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(danger.x * 0.8f, danger.y * 0.8f, danger.z * 0.8f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("Remove##outfitRemove")) {
            c.pushUndo("Remove " + slot.Label);
            c.ReportSound(OutfitSystem::Equip(c.world, c.assets, c.root, c.ui.Slot, std::string()), EditorSound::Event::Unequip);
            c.ReadPieces();
            c.ReadWorn();
        }
        ImGui::PopStyleColor(4);
    }
    {
        std::vector<std::string> locks = OutfitSystem::ParseLocks(c.outfit.Locks);
        const bool locked = std::find(locks.begin(), locks.end(), c.ui.Slot) != locks.end();
        ImGui::SameLine(lockX);
        if (EditorInternal::ActionButton(locked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, locked ? "Locked: Randomize keeps it" : "Randomize may change it")) {
            c.pushUndo("Lock Outfit Slot");
            if (locked) locks.erase(std::remove(locks.begin(), locks.end(), c.ui.Slot), locks.end());
            else locks.push_back(c.ui.Slot);
            std::string joined;
            for (const auto& l : locks) joined += (joined.empty() ? "" : ", ") + l;
            c.outfit.Locks = joined;
        }
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##outfitSearch", ICON_FA_MAGNIFYING_GLASS "  Search", c.ui.Search, sizeof(c.ui.Search));
}

// The slot's items, as a list. Pointing at one previews it below; clicking puts it on (Remove above takes it
// off). Returns the item pointed at, if any.
const Wardrobe::Item* DrawItemList(OutfitCtx& c) {
    const ImGuiStyle& style = *c.style;
    // What's worn in the other slots, to mark the items that don't go with it (Wardrobe::Conflicts).
    std::vector<const Wardrobe::Item*> others;
    for (const auto& [s, e] : c.pieces)
        if (s != c.ui.Slot)
            if (const auto* it = c.cat.Find(c.reg.get<OutfitPieceComponent>(e).Item)) others.push_back(it);
    // The first worn item `it` can't go with (hard: equipping takes it off) or only looks odd with.
    auto clashWith = [&](const Wardrobe::Item& it, bool& hard) -> const Wardrobe::Item* {
        for (const auto* o : others)
            if (Wardrobe::Conflicts(c.cat.W, it, *o, false)) { hard = true; return o; }
        for (const auto* o : others)
            if (Wardrobe::Conflicts(c.cat.W, it, *o, true)) { hard = false; return o; }
        return nullptr;
    };

    std::vector<const Wardrobe::Item*> items;
    for (const auto* it : c.cat.ForSlot(c.ui.Slot, c.sex))
        if ((c.ui.ShowVariants || !it->Variant || it == c.wornItem) && ContainsI(it->Name, c.ui.Search)) items.push_back(it);
    const Wardrobe::Item* hoveredItem = nullptr;
    const float row = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("##outfitList", ImVec2(0.0f, std::min(260.0f, row * (float)items.size() + 6.0f)), true);
    for (size_t i = 0; i < items.size(); ++i) {
        const Wardrobe::Item* it = items[i];
        ImGui::PushID((int)i);
        const bool selected = it == c.wornItem;
        if (ImGui::Selectable(it->Name.c_str(), selected, 0, ImVec2(0.0f, row - style.ItemSpacing.y)) && !selected) {
            c.pushUndo("Equip " + it->Name);
            c.ReportSound(OutfitSystem::Equip(c.world, c.assets, c.root, c.ui.Slot, it->Path), EditorSound::Event::Equip);
            c.ReadPieces();
            c.ReadWorn();
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
    return hoveredItem;
}

// The 3D preview of `shown` (the item pointed at in the list, else the one worn): drag to spin, scroll to
// zoom, shading and reset under it; a "Loading..." box until the model is in memory.
void DrawItemPreview(OutfitCtx& c, ModelPreviewRenderer& preview, const Wardrobe::Item& shown) {
    const ImGuiStyle& style = *c.style;
    OutfitUiState& ui = c.ui;
    const std::string abs = ProjectPaths::Resolve(shown.Path);
    std::shared_ptr<Model> model;
    auto load = g_PreviewLoads.find(abs);
    if (load == g_PreviewLoads.end()) load = g_PreviewLoads.emplace(abs, c.assets.RequestModelAsync(abs)).first;
    if (AssetLibrary::IsReady(load->second)) model = c.assets.LoadModel(abs);
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::clamp(width * 0.8f, 160.0f, 300.0f);
    if (!model) {
        ImGui::Dummy(ImVec2(width, height));
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, Col(style.Colors[ImGuiCol_FrameBg]), 4.0f);
        const char* msg = "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2((mn.x + mx.x - ts.x) * 0.5f, (mn.y + mx.y - ts.y) * 0.5f),
                                            Col(style.Colors[ImGuiCol_TextDisabled]), msg);
        return;
    }
    const float fit = ModelPreviewRenderer::ComputeFramingDistance(*model) * 0.72f;
    if (ui.PreviewFor != abs) { ui.PreviewFor = abs; ui.Distance = fit; }
    // The worn one with the piece's own materials (its colourway), anything else as it comes.
    const bool isWorn = &shown == c.wornItem;
    std::vector<std::shared_ptr<MaterialAsset>> slotsMats;
    if (isWorn && c.worn != c.pieces.end()) {
        if (const auto* rc = c.reg.try_get<RenderableComponent>(c.worn->second)) slotsMats = rc->Materials;
    } else {
        c.assets.ApplyMaterialRemap(*model, slotsMats);
    }
    const float scale = ImGui::GetIO().DisplayFramebufferScale.x > 0.0f ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f;
    const unsigned handle = preview.Render(*model, ui.Yaw, ui.Pitch, ui.Distance, (int)(width * scale),
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
                                        (shown.Name + (isWorn ? "  (worn)" : "")).c_str());
    // Shading and reset, under it.
    static const char* kShadingModes[] = {"Lit", "Unlit", "Wireframe"};
    EditorUIPrimitives::Segmented("##shadingMode", &ui.Shading, kShadingModes, 3);
    ImGui::SameLine();
    ImGui::TextDisabled("Drag to spin, scroll to zoom");
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Reset view").x - style.FramePadding.x * 2.0f);
    if (EditorUIPrimitives::SecondaryButton("Reset view")) { ui.Yaw = 0.6f; ui.Pitch = 0.25f; ui.Distance = fit; }
}

// The worn item's colourways, a row of named buttons per colour group.
void DrawColourways(OutfitCtx& c) {
    if (c.worn == c.pieces.end()) return;
    const ImGuiStyle& style = *c.style;
    const entt::entity piece = c.worn->second;
    const auto groups = OutfitSystem::ColourGroups(c.world, c.assets, piece);
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
            ImGui::PushStyleColor(ImGuiCol_Button, on ? c.accent : style.Colors[ImGuiCol_FrameBg]);
            if (ImGui::Button(labels[k].c_str(), ImVec2(w, 0.0f)) && !on) {
                c.pushUndo("Change Colourway");
                OutfitSystem::SubmitColourway(c.world, c.assets, c.root, piece, g.Source, g.Options[k]);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", Wardrobe::Stem(g.Options[k]).c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

// What the rules did, the skin hidden under the clothes, and a note when nothing animates the pieces.
void DrawOutfitFooter(OutfitCtx& c) {
    for (const auto& n : c.ui.Notes) ImGui::TextColored(InfoColor(), ICON_FA_CIRCLE_INFO "  %s", n.c_str());
    int hidden = 0, total = 0;
    for (const auto& [s, e] : c.pieces)
        if (const auto* h = c.reg.try_get<OutfitHideTag>(e)) { hidden += h->Hidden; total += h->Total; }
    bool autoHide = c.outfit.AutoHide;
    if (ImGui::Checkbox("Hide skin under clothes", &autoHide)) {
        c.pushUndo("Toggle Outfit Skin Hiding");
        c.outfit.AutoHide = autoHide;
    }
    if (total) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d vertices hidden)", hidden);
    }
    const bool animated = std::any_of(c.pieces.begin(), c.pieces.end(), [&](const auto& kv) {
        return c.reg.all_of<AnimatorControllerComponent>(kv.second);
    });
    if (!animated)
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  No piece has an Animator Controller: only motion matching (the player's body, the dummies) animates it.");
}

} // namespace

void EditorLayer::DrawCharacterOutfitEditor(World& world, entt::entity root) {
    auto& reg = world.Registry;
    auto* outfit = reg.try_get<CharacterOutfitComponent>(root);
    if (!outfit || !m_AssetsPtr) return;
    AssetLibrary& assets = *m_AssetsPtr;
    // Forget the panels of destroyed characters (an id is reused with a new version, so it can't inherit one).
    if (g_OutfitUi.size() > 32)
        for (auto it = g_OutfitUi.begin(); it != g_OutfitUi.end();)
            it = reg.valid(entt::entity(it->first)) ? std::next(it) : g_OutfitUi.erase(it);
    OutfitUiState& ui = g_OutfitUi[entt::to_integral(root)];

    std::string error;
    auto cat = OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, false, &error);
    if (!cat) {
        ImGui::TextColored(WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  %s", error.c_str());
        if (EditorUIPrimitives::SecondaryButton(ICON_FA_ROTATE "  Try again")) OutfitSystem::LoadCatalog(assets, outfit->Wardrobe, true);
        return;
    }
    OutfitCtx c{world, reg, assets, root, *outfit, *cat, ui,
                [this, &world](const std::string& label) { PushUndo(world, label); },
                OutfitSystem::Pieces(world, root)};
    if (c.pieces.empty()) { DrawOutfitStart(c); return; }
    c.sex = outfit->Gender == 1 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
    c.accent = AccentColor();
    c.dl = ImGui::GetWindowDrawList();
    c.style = &ImGui::GetStyle();

    DrawOutfitIdentity(c);
    if (!DrawSlotTabs(c)) return;
    const Wardrobe::SlotDef* slot = cat->W.Slot(ui.Slot);
    c.ReadWorn();
    DrawSlotHeader(c, *slot);
    const Wardrobe::Item* hoveredItem = DrawItemList(c);
    if (const Wardrobe::Item* shown = hoveredItem ? hoveredItem : c.wornItem) DrawItemPreview(c, m_OutfitPreview, *shown);
    DrawColourways(c);
    DrawOutfitFooter(c);
}
