#include "PlayerLook.h"
#include "OutfitSystem.h"
#include "Wardrobe.h"
#include "Components.h"
#include "World.h"
#include "AssetLibrary.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "FKeyRow.h"
#include "Shortcuts.h"
#include <chrono>
#include <string>
#include <vector>

namespace {

struct Look { Wardrobe::Gender Sex; std::string Race; };

const char* SexName(Wardrobe::Gender g) { return g == Wardrobe::Gender::Female ? "Female" : "Male"; }
std::string Key(const Look& l) { return std::string(SexName(l.Sex)) + "/" + l.Race; }
std::string Label(const Look& l) { return l.Race == "Skeleton" ? std::string("Skeleton") : std::string(SexName(l.Sex)) + " " + l.Race; }

// Every look the wardrobe has: each body's races, then the skeletons.
std::vector<Look> Looks(const Wardrobe::Wardrobe& w) {
    std::vector<Look> out, bones;
    for (Wardrobe::Gender g : {Wardrobe::Gender::Male, Wardrobe::Gender::Female})
        for (const Wardrobe::RaceDef& r : w.Body(g).Races) (r.Name == "Skeleton" ? bones : out).push_back({g, r.Name});
    if (!bones.empty()) out.push_back(bones.front()); // one skeleton: it is the same bones whichever body it was filed under
    return out;
}

entt::entity PlayerRoot(World& world) {
    auto view = world.Registry.view<FirstPersonBodyComponent, CharacterOutfitComponent>();
    return view.begin() != view.end() ? *view.begin() : entt::entity(entt::null);
}

void PutOn(World& world, AssetLibrary& assets, entt::entity root, const Look& l) {
    OutfitSystem::SetGender(world, assets, root, l.Sex); // (chains: the race goes on the request this one leaves waiting)
    OutfitSystem::SetRace(world, assets, root, l.Race);
}

// Dressed in style `style` (an index into the wardrobe's styles; their count = Bare), the look kept.
void Dress(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Wardrobe& w, int style, unsigned seed) {
    if (style >= (int)w.Styles.size()) { // Bare: every item off, the body parts stay
        for (const auto& [slot, piece] : OutfitSystem::Pieces(world, root))
            if (!(world.Registry.get<OutfitPieceComponent>(piece).Flags & OutfitPieceBodyPart)) OutfitSystem::Equip(world, assets, root, slot, "");
        return;
    }
    auto& outfit = world.Registry.get<CharacterOutfitComponent>(root);
    const std::string locks = outfit.Locks;
    outfit.Locks = locks.empty() ? std::string("Race") : locks + ",Race"; // Randomize keeps the race it is told to keep
    OutfitSystem::Randomize(world, assets, root, seed, style >= 0 ? w.Styles[(size_t)style].Name : std::string());
    outfit.Locks = locks;
}

bool StyleFits(const Wardrobe::Wardrobe& w, int style, Wardrobe::Gender g) {
    return style >= (int)w.Styles.size() || w.Styles[(size_t)style].Gender < 0 || w.Styles[(size_t)style].Gender == (int)g;
}

} // namespace

void PlayerLook::Update(World& world, AssetLibrary& assets, bool keys) {
    const entt::entity root = PlayerRoot(world);
    if (root == entt::null) return;
    auto& outfit = world.Registry.get<CharacterOutfitComponent>(root);
    const auto cat = OutfitSystem::LoadCatalog(assets, outfit.Wardrobe);
    if (!cat) return;
    const Wardrobe::Wardrobe& w = cat->W;
    const std::vector<Look> looks = Looks(w);
    if (looks.empty()) return;
    EditorSettings& es = EditorSettings::Get();
    const int bare = (int)w.Styles.size();

    // A new Play: the kept look and clothes go on.
    if (m_AppliedFor != root) {
        m_AppliedFor = root;
        for (const Look& l : looks)
            if (Key(l) == es.FRowLook) PutOn(world, assets, root, l);
        if (es.FRowStyle >= 0 && es.FRowStyle <= bare) Dress(world, assets, root, w, es.FRowStyle, es.FRowOutfitSeed);
    }
    if (!keys) return;

    // F7 / Shift+F7: the next / previous look.
    const bool next = Shortcuts::TriggeredGlfw("fkey.look"), prev = !next && Shortcuts::TriggeredGlfw("fkey.lookBack");
    if (next || prev) {
        int cur = -1;
        for (int i = 0; i < (int)looks.size(); ++i)
            if (Key(looks[(size_t)i]) == (es.FRowLook.empty() ? std::string(SexName((Wardrobe::Gender)outfit.Gender)) + "/" + outfit.Race : es.FRowLook)) cur = i;
        const int n = (int)looks.size();
        const int i = ((cur < 0 ? 0 : cur + (next ? 1 : -1)) % n + n) % n;
        const Look& l = looks[(size_t)i];
        PutOn(world, assets, root, l);
        // A style cut for one body only doesn't follow the switch: back to the wardrobe's own pick.
        if (es.FRowStyle >= 0 && !StyleFits(w, es.FRowStyle, l.Sex)) es.FRowStyle = -1;
        es.FRowLook = Key(l);
        EditorSettings::Save();
        EditorSound::Play(next ? EditorSound::Event::FKeyCycle : EditorSound::Event::FKeyCycleBack, -1.0f, 1.0f + 0.04f * (float)i);
        FKeyRow::Show("F7", "Look", Label(l), i, n);
    }

    // F8: a new random outfit in the current style. Shift+F8: the next style, dressed in it.
    const bool fresh = Shortcuts::TriggeredGlfw("fkey.outfit"), restyle = !fresh && Shortcuts::TriggeredGlfw("fkey.outfitStyle");
    if (fresh || restyle) {
        const Wardrobe::Gender sex = (Wardrobe::Gender)outfit.Gender;
        if (restyle) {
            int s = es.FRowStyle;
            for (int k = 0; k <= bare; ++k) { // the next one this body can wear (Bare always)
                s = s + 1 > bare ? 0 : s + 1;
                if (StyleFits(w, s, sex)) break;
            }
            es.FRowStyle = s;
        }
        es.FRowOutfitSeed = (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
        Dress(world, assets, root, w, es.FRowStyle, es.FRowOutfitSeed);
        EditorSettings::Save();
        const std::string style = es.FRowStyle < 0 ? std::string("Any style") : es.FRowStyle >= bare ? std::string("Bare") : w.Styles[(size_t)es.FRowStyle].Name;
        EditorSound::Play(EditorSound::Event::SpawnItem, -1.0f, restyle ? 1.12f : 1.0f);
        if (restyle) FKeyRow::Show("F8", "Style", style, std::max(es.FRowStyle, 0), bare + 1);
        else FKeyRow::Show("F8", "Outfit", "New " + style + " outfit");
    }
}
