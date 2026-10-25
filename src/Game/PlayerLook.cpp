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
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

using Look = PlayerLook::Look;

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
void PlayerLook::LoadAhead(World& world, AssetLibrary& assets, entt::entity root, int step) {
    auto& outfit = world.Registry.get<CharacterOutfitComponent>(root);
    if (!m_Catalog) return;
    const Wardrobe::Wardrobe& w = m_Catalog->W;
    const EditorSettings& es = EditorSettings::Get();
    const int bare = (int)w.Styles.size();
    if (step == 0) m_NextSeed = NewSeed();
    if (step <= 1) {
        KeepRace keep(outfit);
        const int style = step == 0 ? es.FRowStyle : NextStyle(w, es.FRowStyle, (Wardrobe::Gender)outfit.Gender);
        if (style < bare) OutfitSystem::Preload(world, assets, root, OutfitSystem::PlanRandom(world, assets, root, m_NextSeed, StyleName(w, style)));
        return;
    }
    const std::vector<Look>& looks = m_Looks;
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

// A slice of the wardrobe warm-up: models started loading until about half a millisecond has gone (each one's first
// look reads its .meta here), so the Play-start warm-up never costs a frame.
void PlayerLook::WarmSome(AssetLibrary& assets) {
    // A few loads in flight at most: the loader serves in order, so a swap (or the next outfit drawn ahead) asked for
    // now would otherwise wait behind the whole wardrobe.
    constexpr int kMaxInFlight = 3;
    const auto start = std::chrono::steady_clock::now();
    while (m_WarmAt < m_Warm.size() && assets.AsyncInFlight() < kMaxInFlight) {
        OutfitSystem::PreloadModel(assets, m_Warm[m_WarmAt++]);
        if (std::chrono::steady_clock::now() - start > std::chrono::microseconds(500)) break;
    }
    if (m_WarmAt >= m_Warm.size()) { m_Warm.clear(); m_Warm.shrink_to_fit(); m_WarmAt = 0; }
}

void PlayerLook::Update(World& world, AssetLibrary& assets, bool keys) {
    // An idle frame (no key, nothing loading, the warm-up done) costs the root check and the key lookups.
    const bool next = keys && Shortcuts::TriggeredGlfw("fkey.look");
    const bool prev = keys && !next && Shortcuts::TriggeredGlfw("fkey.lookBack");
    const bool fresh = keys && Shortcuts::TriggeredGlfw("fkey.outfit");
    const bool restyle = keys && !fresh && Shortcuts::TriggeredGlfw("fkey.outfitStyle");
    entt::entity root = m_AppliedFor;
    if (root == entt::null || !world.Registry.valid(root) || !world.Registry.all_of<CharacterOutfitComponent>(root)) root = PlayerRoot(world);
    if (root == entt::null) return;
    if (root == m_AppliedFor && !next && !prev && !fresh && !restyle && !m_Dressing && !m_Ahead && m_Warm.empty()) return;
    if (!m_Warm.empty() && !m_Dressing) WarmSome(assets); // (a swap waiting on its loads goes first)
    auto& outfit = world.Registry.get<CharacterOutfitComponent>(root);
    if (!m_Catalog || m_WardrobePath != outfit.Wardrobe) {
        m_WardrobePath = outfit.Wardrobe;
        m_Catalog = OutfitSystem::LoadCatalog(assets, outfit.Wardrobe);
        m_Looks = m_Catalog ? Looks(m_Catalog->W) : std::vector<Look>{};
    }
    if (!m_Catalog || m_Looks.empty()) return;
    const auto& cat = m_Catalog;
    const Wardrobe::Wardrobe& w = cat->W;
    const std::vector<Look>& looks = m_Looks;
    EditorSettings& es = EditorSettings::Get();
    const int bare = (int)w.Styles.size();

    // A new Play: the kept look and clothes go on, and the whole wardrobe starts loading behind them.
    if (m_AppliedFor != root) {
        m_AppliedFor = root;
        m_Ahead = true;
        m_AheadStep = 0;
        for (const Look& l : looks)
            if (Key(l) == es.FRowLook) PutOn(world, assets, root, l);
        if (es.FRowStyle >= 0 && es.FRowStyle <= bare) Dress(world, assets, root, w, es.FRowStyle, es.FRowOutfitSeed);
        m_Warm.clear();
        m_WarmAt = 0;
        OutfitSystem::WardrobeModels(*cat, (Wardrobe::Gender)outfit.Gender, m_Warm); // this body first, then the other
        OutfitSystem::WardrobeModels(*cat, outfit.Gender == 0 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male, m_Warm);
    }
    // A swap waiting on loads: the pill counts them, then says it is done.
    int done = 0, total = 0;
    const bool pending = (m_Dressing || m_Ahead) && OutfitSystem::IsPending(world, root, &done, &total);
    if (m_Dressing && pending && (done != m_HeldDone || total != m_HeldTotal)) {
        m_HeldDone = done;
        m_HeldTotal = total;
        m_HoldText = "Loading " + std::to_string(done) + "/" + std::to_string(total);
        FKeyRow::Hold(m_LookSwap ? "F7" : "F8", m_LookSwap ? "Look" : "Outfit", m_HoldText);
    }
    if (m_Dressing && !pending) { m_Dressing = false; m_HeldDone = m_HeldTotal = -1; FKeyRow::Release("Ready"); }
    if (m_Ahead && !pending) {
        LoadAhead(world, assets, root, m_AheadStep);
        if (++m_AheadStep > 2) { m_Ahead = false; m_AheadStep = 0; }
    }

    // F7 / Shift+F7: the next / previous look.
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
        m_AheadStep = 0;
    }

    // F8: a new random outfit in the current style. Shift+F8: the next style, dressed in it.
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
        m_AheadStep = 0;
        m_LookSwap = false;
        m_NextSeed = 0;
    }
}
