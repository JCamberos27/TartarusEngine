#include "OutfitTestScene.h"

#include "AssetLibrary.h"
#include "Components.h"
#include "OutfitSystem.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "Wardrobe.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace OutfitTestScene {
namespace {

constexpr const char* kWardrobe = "assets/Characters/Quantum/Quantum.wardrobe";
constexpr float kSpacing = 1.1f; // metres between characters in a row
constexpr float kRowGap = 3.0f;  // and between rows

// One scene being built: its world, the rows placed so far, and how wide it got (for the ground).
struct Builder {
    World W;
    AssetLibrary& Assets;
    std::shared_ptr<const OutfitSystem::Catalog> Cat;
    float Z = 0.0f, MaxX = 0.0f;
    int Characters = 0, Failed = 0;

    explicit Builder(AssetLibrary& a, std::shared_ptr<const OutfitSystem::Catalog> cat) : Assets(a), Cat(std::move(cat)) {
        // A fresh World's sky is black (a loaded scene gets these defaults); the smoke scenes' daylight.
        W.SkyHorizonColor = glm::vec3(0.45f, 0.55f, 0.68f);
        W.SkyZenithColor = glm::vec3(0.15f, 0.30f, 0.60f);
    }

    entt::entity Group(const std::string& name) {
        const entt::entity g = W.CreateEmptyEntity(glm::vec3(0.0f, 0.0f, Z), glm::vec3(0.0f), glm::vec3(1.0f), name);
        Z -= kRowGap;
        return g;
    }

    // A character dressed as `req` at slot `index` of `row` (a group from Group()), random colourways
    // from `seed` if it's not 0.
    void Character(entt::entity row, int index, const std::string& name, const Wardrobe::Request& req, unsigned seed = 0) {
        const glm::vec3 rowPos = W.Registry.get<TransformComponent>(row).Position;
        const glm::vec3 pos(index * kSpacing, 0.0f, rowPos.z);
        const entt::entity e = W.CreateEmptyEntity(pos, glm::vec3(0.0f), glm::vec3(1.0f), name);
        auto& outfit = W.Registry.emplace<CharacterOutfitComponent>(e);
        outfit.Wardrobe = kWardrobe;
        outfit.Gender = (int)req.Sex;
        outfit.Race = req.Race;
        W.SetParent(e, row);
        const OutfitSystem::Result r = OutfitSystem::Apply(W, Assets, e, req);
        ++Characters;
        MaxX = std::max(MaxX, pos.x);
        if (!r.Ok) {
            ++Failed;
            std::cerr << "[OutfitTestScene] " << name << ": " << r.Error << "\n";
            return;
        }
        if (!seed) return;
        std::mt19937 rng(seed);
        for (const auto& [slot, piece] : OutfitSystem::Pieces(W, e))
            for (const auto& g : OutfitSystem::ColourGroups(W, Assets, piece))
                if (!g.Options.empty()) OutfitSystem::SetColourway(W, Assets, piece, g.Source, g.Options[rng() % g.Options.size()]);
    }

    bool Save(const std::string& file) {
        // Ground under every row, and a sun.
        const float width = MaxX + 4.0f, depth = -Z + 4.0f;
        W.CreateBox(glm::vec3(MaxX * 0.5f, -0.1f, Z * 0.5f + kRowGap * 0.5f), glm::vec3(width, 0.2f, depth),
                    glm::vec3(0.35f, 0.35f, 0.38f), glm::vec3(0.0f), "Ground");
        const entt::entity sun = W.CreateEmptyEntity(glm::vec3(0.0f, 10.0f, 6.0f), glm::vec3(50.0f, -35.0f, 0.0f), glm::vec3(1.0f), "Sun");
        auto& light = W.Registry.emplace<LightComponent>(sun);
        light.Kind = LightComponent::Type::Directional;
        light.Color = glm::vec3(1.0f, 0.96f, 0.9f);
        light.Intensity = 5.0f;
        light.AngularSizeDegrees = 1.0f;
        light.Shadow.Enabled = true;
        std::error_code ec;
        std::filesystem::create_directories(ProjectPaths::Resolve("scenes/OutfitTest"), ec);
        const std::string path = ProjectPaths::Resolve("scenes/OutfitTest/" + file);
        const bool ok = SceneSerializer::Save(W, Assets, path);
        std::cout << "[OutfitTestScene] " << (ok ? "wrote " : "FAILED to write ") << path << " (" << Characters << " characters"
                  << (Failed ? ", " + std::to_string(Failed) + " failed" : std::string()) << ")\n";
        return ok && !Failed;
    }
};

const Wardrobe::Item* ByStem(const OutfitSystem::Catalog& cat, Wardrobe::Gender g, const std::string& stem) {
    for (const auto& it : cat.Items)
        if (it.Sex == g && it.Stem == stem) return &it;
    return nullptr;
}

// The plain outfit the Items scene shows each item on.
Wardrobe::Request BaseOutfit(const OutfitSystem::Catalog& cat, Wardrobe::Gender g) {
    Wardrobe::Request req;
    req.Sex = g;
    req.Race = "European";
    const bool f = g == Wardrobe::Gender::Female;
    const std::pair<const char*, const char*> base[] = {
        {"Top", f ? "SKM_F_Tshirt_Tucked" : "SKM_Tshirt"}, {"Pants", f ? "SKM_F_Pants_Jeans" : "SKM_Jeans"},
        {"Shoes", f ? "SKM_F_Sneakers" : "SKM_Sneakers"}, {"Hair", f ? "SKM_F_Haircut_Bobcut" : "SKM_Hair_Short"}};
    for (const auto& [slot, stem] : base)
        if (const auto* it = ByStem(cat, g, stem)) req.Items[slot] = it->Path;
    return req;
}

bool Presets(AssetLibrary& assets, std::shared_ptr<const OutfitSystem::Catalog> cat) {
    Builder b(assets, cat);
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(ProjectPaths::Resolve("assets/Characters/Outfits/Quantum"), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".outfit") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const char* gender : {"Male", "Female"}) {
        const entt::entity row = b.Group(std::string("Artist Presets - ") + gender);
        int i = 0;
        for (const auto& f : files) {
            nlohmann::json j;
            try { std::ifstream(f) >> j; } catch (...) { continue; }
            if (j.value("gender", std::string()) != gender) continue;
            Wardrobe::Request req;
            req.Sex = std::string(gender) == "Female" ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
            req.Race = j.value("race", std::string());
            if (j.contains("items"))
                for (const auto& [slot, v] : j["items"].items()) req.Items[slot] = v.value("item", std::string());
            b.Character(row, i++, f.stem().string(), req);
        }
    }
    return b.Save("Presets.json");
}

bool Items(AssetLibrary& assets, std::shared_ptr<const OutfitSystem::Catalog> cat) {
    Builder b(assets, cat);
    for (int g = 0; g < 2; ++g) {
        const auto sex = (Wardrobe::Gender)g;
        const Wardrobe::Request base = BaseOutfit(*cat, sex);
        for (const auto& slot : cat->W.Slots) {
            const auto& items = cat->ForSlot(slot.Id, sex);
            if (items.empty()) continue;
            const entt::entity row = b.Group(std::string(Wardrobe::GenderName(sex)) + " - " + slot.Label);
            int i = 0;
            for (const auto* item : items) {
                if (item->Variant) continue; // the rules put these on (boots -> _Inboots pants): seen with their partner
                Wardrobe::Request req = base;
                req.Items[slot.Id] = item->Path;
                b.Character(row, i++, slot.Label + " - " + item->Name, req);
            }
        }
    }
    return b.Save("Items.json");
}

bool Randomized(AssetLibrary& assets, std::shared_ptr<const OutfitSystem::Catalog> cat) {
    Builder b(assets, cat);
    unsigned seed = 1;
    for (int g = 0; g < 2; ++g) {
        Wardrobe::Request base;
        base.Sex = (Wardrobe::Gender)g;
        for (const auto& style : cat->W.Styles) {
            if (style.Gender >= 0 && style.Gender != g) continue;
            const entt::entity row = b.Group(std::string(Wardrobe::GenderName(base.Sex)) + " - " + style.Name);
            for (int i = 0; i < 8; ++i, ++seed) {
                const auto req = Wardrobe::Randomize(cat->W, cat->Items, base, seed, {}, style.Name);
                b.Character(row, i, style.Name + " " + std::to_string(i + 1) + " (seed " + std::to_string(seed) + ")", req, seed);
            }
        }
    }
    return b.Save("Randomized.json");
}

} // namespace

int Generate(AssetLibrary& assets) {
    std::string err;
    auto cat = OutfitSystem::LoadCatalog(assets, kWardrobe, true, &err);
    if (!cat) {
        std::cerr << "[OutfitTestScene] " << err << "\n";
        return 2;
    }
    bool ok = Presets(assets, cat);
    ok = Items(assets, cat) && ok;
    ok = Randomized(assets, cat) && ok;
    return ok ? 0 : 1;
}

} // namespace OutfitTestScene
