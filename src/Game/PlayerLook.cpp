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
    for (auto [e, body, outfit] : world.Registry.view<FirstPersonBodyComponent, CharacterOutfitComponent>().each()) return e;
    return entt::null;
}

void PutOn(World& world, AssetLibrary& assets, entt::entity root, const Look& l) {
    OutfitSystem::SetGender(world, assets, root, l.Sex); // (chains: the race goes on the request this one leaves waiting)
    OutfitSystem::SetRace(world, assets, root, l.Race);
}

// Dressed in style `style` (an index into the wardrobe's styles; their count = Bare), the look kept.
// Randomize and PlanRandom keep the race they are told to keep: the look stays while the clothes change.
struct KeepRace {
    CharacterOutfitComponent& O;
    std::string Was;
    explicit KeepRace(CharacterOutfitComponent& o) : O(o), Was(o.Locks) { O.Locks = Was.empty() ? std::string("Race") : Was + ",Race"; }
    ~KeepRace() { O.Locks = Was; }
};

unsigned NewSeed() { return (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() | 1u; }

void Dress(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Wardrobe& w, int style, unsigned seed) {
    if (style >= (int)w.Styles.size()) { // Bare: every item off, the body parts stay
        for (const auto& [slot, piece] : OutfitSystem::Pieces(world, root))
            if (!(world.Registry.get<OutfitPieceComponent>(piece).Flags & OutfitPieceBodyPart)) OutfitSystem::Equip(world, assets, root, slot, "");
        return;
    }
    KeepRace keep(world.Registry.get<CharacterOutfitComponent>(root));
    OutfitSystem::Randomize(world, assets, root, seed, style >= 0 ? w.Styles[(size_t)style].Name : std::string());
}

std::string StyleName(const Wardrobe::Wardrobe& w, int style) {
    return style < 0 ? std::string() : style >= (int)w.Styles.size() ? std::string("Bare") : w.Styles[(size_t)style].Name;
}

bool StyleFits(const Wardrobe::Wardrobe& w, int style, Wardrobe::Gender g) {
    return style >= (int)w.Styles.size() || w.Styles[(size_t)style].Gender < 0 || w.Styles[(size_t)style].Gender == (int)g;
}

int NextStyle(const Wardrobe::Wardrobe& w, int style, Wardrobe::Gender sex) {
    const int bare = (int)w.Styles.size();
    for (int k = 0; k <= bare; ++k) { // the next one this body can wear (Bare always)
        style = style + 1 > bare ? 0 : style + 1;
        if (StyleFits(w, style, sex)) break;
    }
    return style;
}

} // namespace

// What the next press of each key puts on, loading now: F8's next outfit (its seed drawn now), Shift+F8's next style,
// F7's next look.
void PlayerLook::LoadAhead(World& world, AssetLibrary& assets, entt::entity root) {
    auto& outfit = world.Registry.get<CharacterOutfitComponent>(root);
    const auto cat = OutfitSystem::LoadCatalog(assets, outfit.Wardrobe);
    if (!cat) return;
    const Wardrobe::Wardrobe& w = cat->W;
    const EditorSettings& es = EditorSettings::Get();
    const int bare = (int)w.Styles.size();
    m_NextSeed = NewSeed();
    {
        KeepRace keep(outfit);
        if (es.FRowStyle < bare)
            OutfitSystem::Preload(world, assets, root, OutfitSystem::PlanRandom(world, assets, root, m_NextSeed, StyleName(w, es.FRowStyle)));
        const int next = NextStyle(w, es.FRowStyle, (Wardrobe::Gender)outfit.Gender);
        if (next < bare)
            OutfitSystem::Preload(world, assets, root, OutfitSystem::PlanRandom(world, assets, root, m_NextSeed, StyleName(w, next)));
    }
    const std::vector<Look> looks = Looks(w);
    for (int i = 0; i < (int)looks.size(); ++i)
        if (looks[(size_t)i].Sex == (Wardrobe::Gender)outfit.Gender && looks[(size_t)i].Race == outfit.Race) {
            const Look& l = looks[(size_t)((i + 1) % (int)looks.size())];
            Wardrobe::Request req = OutfitSystem::CurrentRequest(world, root);
            req.Sex = l.Sex;
            req.Race = l.Race;
            OutfitSystem::Preload(world, assets, root, req);
            break;
        }
}

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

    // A new Play: the kept look and clothes go on, and the whole wardrobe starts loading behind them.
    if (m_AppliedFor != root) {
        m_AppliedFor = root;
        m_Ahead = true;
        for (const Look& l : looks)
            if (Key(l) == es.FRowLook) PutOn(world, assets, root, l);
        if (es.FRowStyle >= 0 && es.FRowStyle <= bare) Dress(world, assets, root, w, es.FRowStyle, es.FRowOutfitSeed);
        OutfitSystem::PreloadWardrobe(assets, *cat, (Wardrobe::Gender)outfit.Gender); // this body first, then the other
        OutfitSystem::PreloadWardrobe(assets, *cat, outfit.Gender == 0 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male);
    }
    // A swap waiting on loads: the pill counts them, then says it is done.
    int done = 0, total = 0;
    const bool pending = OutfitSystem::IsPending(world, root, &done, &total);
    if (m_Dressing && pending) FKeyRow::Hold(m_LookSwap ? "F7" : "F8", m_LookSwap ? "Look" : "Outfit", "Loading " + std::to_string(done) + "/" + std::to_string(total));
    if (m_Dressing && !pending) { m_Dressing = false; FKeyRow::Release("Ready"); }
    if (m_Ahead && !pending) { m_Ahead = false; LoadAhead(world, assets, root); }
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
        m_Dressing = m_Ahead = m_LookSwap = true;
    }

    // F8: a new random outfit in the current style. Shift+F8: the next style, dressed in it.
    const bool fresh = Shortcuts::TriggeredGlfw("fkey.outfit"), restyle = !fresh && Shortcuts::TriggeredGlfw("fkey.outfitStyle");
    if (fresh || restyle) {
        const Wardrobe::Gender sex = (Wardrobe::Gender)outfit.Gender;
        if (restyle) es.FRowStyle = NextStyle(w, es.FRowStyle, sex);
        es.FRowOutfitSeed = m_NextSeed ? m_NextSeed : NewSeed(); // the outfit drawn ahead: already loading
        Dress(world, assets, root, w, es.FRowStyle, es.FRowOutfitSeed);
        EditorSettings::Save();
        const std::string style = es.FRowStyle < 0 ? std::string("Any style") : es.FRowStyle >= bare ? std::string("Bare") : w.Styles[(size_t)es.FRowStyle].Name;
        EditorSound::Play(EditorSound::Event::SpawnItem, -1.0f, restyle ? 1.12f : 1.0f);
        if (restyle) FKeyRow::Show("F8", "Style", style, std::max(es.FRowStyle, 0), bare + 1);
        else FKeyRow::Show("F8", "Outfit", "New " + style + " outfit");
        m_Dressing = m_Ahead = true;
        m_LookSwap = false;
        m_NextSeed = 0;
    }
}
