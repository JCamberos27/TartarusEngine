#include "OutfitSystem.h"

#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "SkinHideBuffer.h"
#include "ProjectPaths.h"
#include "World.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <set>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace OutfitSystem {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A path as the scene stores it: project-relative, forward slashes.
std::string Rel(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (!fs::path(p).is_absolute()) return p; // already project-relative
    // Lexically, when the path is under the project root: Relativize goes through
    // std::filesystem::relative, which hits the disk, and this runs for every material and piece of
    // an outfit change (it was a good part of the click's hitch).
    static const std::string root = [] {
        std::string r = ProjectPaths::Root();
        std::replace(r.begin(), r.end(), '\\', '/');
        while (!r.empty() && r.back() == '/') r.pop_back();
        return Lower(r) + "/";
    }();
    if (p.size() > root.size() && Lower(p.substr(0, root.size())) == root) return p.substr(root.size());
    p = ProjectPaths::Relativize(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

bool Same(const std::string& a, const std::string& b) { return Lower(Rel(a)) == Lower(Rel(b)); }

std::string Folder(const std::string& path) {
    const std::string p = Rel(path);
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

bool Exists(const std::string& rel) {
    std::error_code ec;
    return fs::exists(ProjectPaths::Resolve(rel), ec);
}

// The .mat files beside `matPath` (cached per folder).
const std::vector<std::string>& Siblings(const std::string& matPath) {
    static std::map<std::string, std::vector<std::string>> cache;
    const std::string dir = Folder(matPath);
    auto it = cache.find(dir);
    if (it != cache.end()) return it->second;
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ProjectPaths::Resolve(dir), ec))
        if (e.is_regular_file(ec) && Lower(e.path().extension().string()) == ".mat")
            out.push_back(dir + "/" + e.path().filename().string());
    return cache[dir] = std::move(out);
}

// The model's remapped material per slot (project-relative .mat, "" where none).
std::vector<std::string> SlotSources(AssetLibrary& assets, const Model& model) {
    const auto remap = assets.MaterialRemap(model.Path());
    std::vector<std::string> out((size_t)model.MeshCount());
    for (int i = 0; i < model.MeshCount(); ++i)
        if (auto it = remap.find(model.MeshMaterial(i).Name); it != remap.end()) out[(size_t)i] = Rel(it->second);
    return out;
}

std::shared_ptr<MaterialAsset> Material(AssetLibrary& assets, const std::string& rel) {
    if (rel.empty()) return nullptr;
    auto m = assets.LoadMaterial(ProjectPaths::Resolve(rel));
    return m && !m->Missing ? m : nullptr;
}

// The name an item has in either gender ("SKM_F_Hoodie" and "SKM_Hoodie" -> "hoodie").
std::string CutName(const std::string& stem) {
    std::string s = stem;
    for (const char* p : {"SKM_F_", "SKM_", "SM_"})
        if (Lower(s).rfind(Lower(p), 0) == 0) { s = s.substr(std::string(p).size()); break; }
    return Lower(s);
}

std::shared_ptr<const Catalog> CatalogFor(World& world, AssetLibrary& assets, entt::entity root, Result& r) {
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(root);
    if (!outfit) { r.Error = "no Character Outfit on the object"; return nullptr; }
    auto cat = LoadCatalog(assets, outfit->Wardrobe, false, &r.Error);
    return cat;
}

// The body's driving Animator (the first piece with one), for new pieces to follow.
const AnimatorControllerComponent* Driver(const World& world, entt::entity root) {
    const auto& reg = world.Registry;
    if (const auto* ac = reg.try_get<AnimatorControllerComponent>(root)) return ac;
    if (const auto* h = reg.try_get<HierarchyComponent>(root))
        for (entt::entity c : h->Children)
            if (reg.valid(c))
                if (const auto* ac = reg.try_get<AnimatorControllerComponent>(c)) return ac;
    return nullptr;
}

// A colourway picked on a piece: its remapped material and what was put in its place.
using Picks = std::vector<std::pair<std::string, std::string>>;

// The colourways picked on `rc` (slots holding another material than their remap's; skin excluded).
Picks PickedColourways(AssetLibrary& assets, const Wardrobe::Wardrobe& w, const RenderableComponent& rc) {
    Picks out;
    if (!rc.ModelRef) return out;
    const auto sources = SlotSources(assets, *rc.ModelRef);
    for (size_t i = 0; i < sources.size() && i < rc.Materials.size(); ++i)
        if (!sources[i].empty() && rc.Materials[i] && !Same(sources[i], rc.Materials[i]->Path) &&
            Wardrobe::SkinBase(w, sources[i]).empty())
            out.emplace_back(sources[i], Rel(rc.Materials[i]->Path));
    return out;
}

// Materials for a piece's model: the remap's, a colourway picked on the model it replaces where a slot's
// material is from the same family (folder: jeans -> the same jeans cut for boots), and the race's skin.
void DressPiece(AssetLibrary& assets, const Wardrobe::Wardrobe& w, const Wardrobe::RaceDef* race, RenderableComponent& rc,
                const Picks& previous) {
    if (!rc.ModelRef) return;
    const std::vector<std::string> sources = SlotSources(assets, *rc.ModelRef);
    std::vector<std::shared_ptr<MaterialAsset>> slots((size_t)rc.ModelRef->MeshCount());
    for (size_t i = 0; i < slots.size(); ++i) {
        std::string want = sources[i];
        if (!want.empty())
            for (const auto& [source, picked] : previous)
                if (Lower(Folder(source)) == Lower(Folder(want))) { want = picked; break; }
        if (race && !want.empty()) want = Wardrobe::SkinMaterialFor(w, *race, want, Exists);
        slots[i] = Material(assets, want);
    }
    assets.ApplyMaterialRemap(*rc.ModelRef, slots); // anything left empty
    rc.Materials = std::move(slots);
}

} // namespace

// --- Catalog ----------------------------------------------------------------------------------

const Wardrobe::Item* Catalog::Find(const std::string& path) const {
    for (const auto& it : Items) if (Same(it.Path, path)) return &it;
    return nullptr;
}

const std::vector<const Wardrobe::Item*>& Catalog::ForSlot(const std::string& slot, Wardrobe::Gender g) const {
    const auto key = std::make_pair(slot, (int)g);
    if (auto it = m_SlotLists.find(key); it != m_SlotLists.end()) return it->second;
    std::vector<const Wardrobe::Item*>& out = m_SlotLists[key];
    for (const auto& it : Items) if (it.Slot == slot && it.Sex == g) out.push_back(&it);
    std::sort(out.begin(), out.end(), [](const Wardrobe::Item* a, const Wardrobe::Item* b) { return Lower(a->Name) < Lower(b->Name); });
    return out;
}

std::shared_ptr<const Catalog> LoadCatalog(AssetLibrary& assets, const std::string& path, bool rescan, std::string* error) {
    static std::map<std::string, std::shared_ptr<const Catalog>> cache;
    const std::string key = Lower(Rel(path));
    if (!rescan)
        if (auto it = cache.find(key); it != cache.end()) return it->second;

    std::ifstream in(ProjectPaths::Resolve(Rel(path)), std::ios::binary);
    if (!in) {
        if (error) *error = "can't open wardrobe " + path;
        return nullptr;
    }
    std::stringstream text;
    text << in.rdbuf();
    auto cat = std::make_shared<Catalog>();
    cat->Path = Rel(path);
    std::string parseError;
    if (!Wardrobe::Parse(text.str(), cat->W, &parseError)) {
        if (error) *error = path + ": " + parseError;
        return nullptr;
    }
    for (const auto& folder : cat->W.ItemFolders) {
        std::error_code ec;
        const fs::path dir = ProjectPaths::Resolve(cat->W.Root + "/" + folder);
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = Lower(it->path().extension().string());
            if (ext != ".fbx" && ext != ".glb" && ext != ".gltf") continue;
            const std::string rel = Rel(it->path().string());
            std::vector<std::string> materials;
            for (const auto& [name, mat] : assets.MaterialRemap(it->path().string())) materials.push_back(Rel(mat));
            Wardrobe::Item item;
            if (Wardrobe::Classify(cat->W, rel, materials, item)) cat->Items.push_back(std::move(item));
        }
    }
    Wardrobe::MarkVariants(cat->W, cat->Items);
    Log::Info("Wardrobe " + cat->W.Name + ": " + std::to_string(cat->Items.size()) + " items");
    cache[key] = cat;
    return cat;
}

// --- Pieces -----------------------------------------------------------------------------------

std::map<std::string, entt::entity> Pieces(const World& world, entt::entity root) {
    std::map<std::string, entt::entity> out;
    const auto& reg = world.Registry;
    if (const auto* h = reg.try_get<HierarchyComponent>(root))
        for (entt::entity c : h->Children)
            if (reg.valid(c))
                if (const auto* p = reg.try_get<OutfitPieceComponent>(c)) out.emplace(p->Slot, c);
    return out;
}

Wardrobe::Request CurrentRequest(const World& world, entt::entity root) {
    Wardrobe::Request r;
    const auto& reg = world.Registry;
    if (const auto* o = reg.try_get<CharacterOutfitComponent>(root)) {
        r.Sex = o->Gender == 1 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
        r.Race = o->Race;
    }
    for (const auto& [slot, e] : Pieces(world, root)) {
        const auto& p = reg.get<OutfitPieceComponent>(e);
        if (!(p.Flags & OutfitPieceBodyPart)) r.Items[slot] = p.Item;
    }
    return r;
}

Result Apply(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request) {
    Result r;
    auto& reg = world.Registry;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return r;
    auto& outfit = reg.get<CharacterOutfitComponent>(root);
    const Wardrobe::RaceDef* race = Wardrobe::FindRace(cat->W, request.Sex, request.Race);
    const Wardrobe::Resolved resolved = Wardrobe::Resolve(cat->W, cat->Items, request);
    r.Notes = resolved.Notes;

    // What's there now: a piece whose model was swapped by hand counts as its model, so it's rebuilt.
    auto pieces = Pieces(world, root);
    std::map<std::string, std::string> current;
    for (const auto& [slot, e] : pieces) {
        const auto* rc = reg.try_get<RenderableComponent>(e);
        current[slot] = rc && rc->ModelRef ? Rel(rc->ModelRef->Path()) : std::string();
    }
    const Wardrobe::Diff diff = Wardrobe::MakeDiff(current, resolved.Pieces);

    for (const auto& slot : diff.Destroy) {
        world.DestroyEntityAndChildren(pieces[slot]);
        pieces.erase(slot);
        ++r.Removed;
    }
    const AnimatorControllerComponent* driver = Driver(world, root);
    AnimatorControllerComponent follower;
    if (driver) {
        follower.Controller = driver->Controller;
        follower.Speed = driver->Speed;
        follower.Track = driver->Track;
        follower.RootMotion = driver->RootMotion;
    }
    const auto* rootLayer = reg.try_get<LayerComponent>(root);
    const int layer = rootLayer ? rootLayer->Layer : 0;

    auto flagsOf = [](const Wardrobe::Piece& p) {
        return (p.BodyPart ? OutfitPieceBodyPart : 0) | (p.HeadAttached ? OutfitPieceHeadAttached : 0);
    };
    for (const auto& p : diff.Create) {
        auto model = assets.InstantiateModel(ProjectPaths::Resolve(p.Path));
        if (!model) { r.Notes.push_back("can't load " + p.Path); continue; }
        const entt::entity e = world.CreateModelEntity(model, glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), p.Slot);
        world.AttachChildRaw(e, root);
        if (layer) reg.emplace_or_replace<LayerComponent>(e, LayerComponent{layer});
        if (driver) reg.emplace<AnimatorControllerComponent>(e, follower);
        reg.emplace<OutfitPieceComponent>(e, OutfitPieceComponent{p.Slot, p.Path, flagsOf(p)});
        pieces[p.Slot] = e;
        ++r.Created;
    }
    for (const auto& p : diff.Remodel) {
        const entt::entity e = pieces[p.Slot];
        auto model = assets.InstantiateModel(ProjectPaths::Resolve(p.Path));
        if (!model) { r.Notes.push_back("can't load " + p.Path); continue; }
        auto& rc = reg.get<RenderableComponent>(e);
        const Picks previous = PickedColourways(assets, cat->W, rc);
        rc.ModelRef = model;
        rc.Materials.clear();
        DressPiece(assets, cat->W, race, rc, previous);
        ++r.Changed;
    }

    // Every piece: its record, and the race's skin (new pieces get their materials here).
    for (const auto& p : resolved.Pieces) {
        auto it = pieces.find(p.Slot);
        if (it == pieces.end()) continue;
        auto& piece = reg.get<OutfitPieceComponent>(it->second);
        piece.Item = p.Path;
        piece.Flags = flagsOf(p);
        auto& rc = reg.get<RenderableComponent>(it->second);
        if (rc.Materials.empty()) DressPiece(assets, cat->W, race, rc, {});
        else if (race)
            for (auto& m : rc.Materials) {
                if (!m) continue;
                const std::string want = Wardrobe::SkinMaterialFor(cat->W, *race, Rel(m->Path), Exists);
                if (!Same(want, m->Path))
                    if (auto skin = Material(assets, want)) m = skin;
            }
    }

    outfit.Gender = (int)request.Sex;
    if (race) outfit.Race = race->Name;
    ++outfit.Version;
    r.Ok = true;
    return r;
}

// --- Waiting changes --------------------------------------------------------------------------

namespace {

using Then = std::function<void(World&, AssetLibrary&, entt::entity)>;

// A change waiting for its assets. HasRequest false = only `After` (a colourway step).
struct PendingChange {
    const World* W = nullptr;
    entt::entity Root = entt::null;
    bool HasRequest = true;
    Wardrobe::Request Request;
    std::vector<AssetLibrary::AsyncHandle> Tickets;
    Then After;
};

std::vector<PendingChange>& Waiting() {
    static std::vector<PendingChange> list;
    return list;
}

bool AllReady(const std::vector<AssetLibrary::AsyncHandle>& tickets) {
    return std::all_of(tickets.begin(), tickets.end(), [](const auto& t) { return AssetLibrary::IsReady(t); });
}

void RequestMaterial(AssetLibrary& assets, const std::string& rel, std::vector<AssetLibrary::AsyncHandle>& out) {
    if (!rel.empty() && Exists(rel)) out.push_back(assets.RequestMaterialAsync(ProjectPaths::Resolve(Rel(rel))));
}

// Starts loading what `request` will put on `root`: every piece's model, and the materials DressPiece
// and the race's skin will choose for them (their remap's, and its skin variant). Anything this misses
// still loads when the change is applied - correct, just not seamless.
std::vector<AssetLibrary::AsyncHandle> Prefetch(World& world, AssetLibrary& assets, entt::entity root, const Catalog& cat,
                                                const Wardrobe::Request& request, std::vector<std::string>& notes) {
    std::vector<AssetLibrary::AsyncHandle> out;
    const Wardrobe::RaceDef* race = Wardrobe::FindRace(cat.W, request.Sex, request.Race);
    const Wardrobe::Resolved resolved = Wardrobe::Resolve(cat.W, cat.Items, request);
    notes = resolved.Notes;
    std::set<std::string> mats;
    auto want = [&](const std::string& path) {
        if (path.empty()) return;
        const std::string rel = Rel(path);
        mats.insert(rel);
        if (race) mats.insert(Wardrobe::SkinMaterialFor(cat.W, *race, rel, Exists));
    };
    for (const auto& p : resolved.Pieces) {
        const std::string abs = ProjectPaths::Resolve(p.Path);
        out.push_back(assets.RequestModelAsync(abs));
        for (const auto& [name, mat] : assets.MaterialRemap(abs)) want(mat);
    }
    // A race change re-skins the materials already on the pieces.
    for (const auto& [slot, e] : Pieces(world, root))
        if (const auto* rc = world.Registry.try_get<RenderableComponent>(e))
            for (const auto& m : rc->Materials)
                if (m) want(m->Path);
    for (const auto& m : mats) RequestMaterial(assets, m, out);
    return out;
}

// Runs `after` now if every ticket is ready, else once they are (UpdatePending).
void WhenReady(World& world, AssetLibrary& assets, entt::entity root, std::vector<AssetLibrary::AsyncHandle> tickets,
               Then after) {
    if (AllReady(tickets)) { after(world, assets, root); return; }
    PendingChange pc;
    pc.W = &world;
    pc.Root = root;
    pc.HasRequest = false;
    pc.Tickets = std::move(tickets);
    pc.After = std::move(after);
    Waiting().push_back(std::move(pc));
}

} // namespace

Result Submit(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request, Then then) {
    Result r;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return r;
    // A newer change replaces whatever this outfit was still waiting for (clicking through items fast).
    auto& list = Waiting();
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const PendingChange& pc) { return pc.W == &world && pc.Root == root; }),
               list.end());

    PendingChange pc;
    pc.W = &world;
    pc.Root = root;
    pc.Request = request;
    pc.Tickets = Prefetch(world, assets, root, *cat, request, r.Notes);
    pc.After = std::move(then);
    if (AllReady(pc.Tickets)) {
        r = Apply(world, assets, root, request);
        if (r.Ok && pc.After) pc.After(world, assets, root);
        return r;
    }
    list.push_back(std::move(pc));
    r.Ok = true;
    r.Pending = true;
    return r;
}

bool IsPending(const World& world, entt::entity root, int* done, int* total) {
    int d = 0, t = 0;
    bool any = false;
    for (const auto& pc : Waiting()) {
        if (pc.W != &world || pc.Root != root) continue;
        any = true;
        t += (int)pc.Tickets.size();
        for (const auto& ticket : pc.Tickets) d += AssetLibrary::IsReady(ticket) ? 1 : 0;
    }
    if (done) *done = d;
    if (total) *total = t;
    return any;
}

void UpdatePending(World& world, AssetLibrary& assets) {
    auto& list = Waiting();
    for (size_t i = 0; i < list.size();) {
        PendingChange& pc = list[i];
        if (pc.W != &world) { ++i; continue; }
        const auto& reg = world.Registry;
        if (!reg.valid(pc.Root) || !reg.all_of<CharacterOutfitComponent>(pc.Root)) {
            list.erase(list.begin() + (std::ptrdiff_t)i);
            continue;
        }
        if (!AllReady(pc.Tickets)) { ++i; continue; }
        // Out of the list before running it: `After` may queue a follow-up step.
        PendingChange ready = std::move(pc);
        list.erase(list.begin() + (std::ptrdiff_t)i);
        if (ready.HasRequest) {
            const Result r = Apply(world, assets, ready.Root, ready.Request);
            if (!r.Ok) { Log::Warn("Character Outfit: " + r.Error); continue; }
        }
        if (ready.After) ready.After(world, assets, ready.Root);
    }
}

namespace {
// What the next change builds on: the change still waiting to load, if any, else what's worn now - so
// clicking Female then a hat before the body has loaded keeps both.
Wardrobe::Request BaseRequest(const World& world, entt::entity root) {
    for (const auto& pc : Waiting())
        if (pc.W == &world && pc.Root == root && pc.HasRequest) return pc.Request;
    return CurrentRequest(world, root);
}
} // namespace

void CancelPending(const World& world) {
    auto& list = Waiting();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const PendingChange& pc) { return pc.W == &world; }),
               list.end());
}

void SubmitColourway(World& world, AssetLibrary& assets, entt::entity root, entt::entity piece,
                     const std::string& source, const std::string& variant) {
    std::vector<AssetLibrary::AsyncHandle> tickets;
    RequestMaterial(assets, variant, tickets);
    WhenReady(world, assets, root, std::move(tickets), [piece, source, variant](World& w, AssetLibrary& a, entt::entity) {
        if (w.Registry.valid(piece)) SetColourway(w, a, piece, source, variant);
    });
}

Result Equip(World& world, AssetLibrary& assets, entt::entity root, const std::string& slot, const std::string& item) {
    Wardrobe::Request req = BaseRequest(world, root);
    if (item.empty()) req.Items.erase(slot);
    else req.Items[slot] = item;
    return Submit(world, assets, root, req);
}

Result SetGender(World& world, AssetLibrary& assets, entt::entity root, Wardrobe::Gender gender) {
    Result r;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return r;
    Wardrobe::Request req = BaseRequest(world, root);
    if (req.Sex == gender) return Submit(world, assets, root, req);
    Wardrobe::Request next;
    next.Sex = gender;
    next.Race = req.Race; // FindRace falls back to the first when the other body has no such race
    for (const auto& [slot, path] : req.Items) {
        const Wardrobe::Item* from = cat->Find(path);
        if (!from) continue;
        for (const auto* it : cat->ForSlot(slot, gender))
            if (CutName(it->Stem) == CutName(from->Stem)) { next.Items[slot] = it->Path; break; }
    }
    return Submit(world, assets, root, next);
}

Result SetRace(World& world, AssetLibrary& assets, entt::entity root, const std::string& race) {
    Wardrobe::Request req = BaseRequest(world, root);
    req.Race = race;
    return Submit(world, assets, root, req);
}

std::vector<std::string> ParseLocks(const std::string& locks) {
    std::vector<std::string> out;
    std::stringstream ss(locks);
    for (std::string s; std::getline(ss, s, ',');) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

Result Randomize(World& world, AssetLibrary& assets, entt::entity root, std::uint32_t seed) {
    Result r;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return r;
    std::mt19937 rng(seed);
    auto chance = [&](float p) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < p; };
    const auto locks = ParseLocks(world.Registry.get<CharacterOutfitComponent>(root).Locks);
    auto locked = [&](const std::string& s) { return std::find(locks.begin(), locks.end(), s) != locks.end(); };

    Wardrobe::Request req = BaseRequest(world, root);
    const auto& races = cat->W.Body(req.Sex).Races;
    if (!locked("Race") && !races.empty()) req.Race = races[rng() % races.size()].Name;
    // How often a slot is filled: the basics always, the rest now and then.
    static const std::map<std::string, float> kFill = {{"Hair", 0.9f}, {"Top", 1.0f}, {"Pants", 1.0f}, {"Shoes", 1.0f},
                                                       {"Outerwear", 0.45f}, {"Hat", 0.3f}, {"Glasses", 0.2f}, {"Beard", 0.4f},
                                                       {"Bag", 0.15f}, {"Collar", 0.1f}, {"Wrist L", 0.2f}, {"Wrist R", 0.2f}};
    for (const auto& slot : cat->W.Slots) {
        if (locked(slot.Id)) continue;
        const auto items = cat->ForSlot(slot.Id, req.Sex);
        const auto f = kFill.find(slot.Id);
        if (items.empty() || !chance(f == kFill.end() ? 0.3f : f->second)) { req.Items.erase(slot.Id); continue; }
        req.Items[slot.Id] = items[rng() % items.size()]->Path;
    }
    // Colourways once the pieces are on: pick them, load them in the background, then put them on.
    return Submit(world, assets, root, req, [rng, locks](World& w, AssetLibrary& a, entt::entity e) mutable {
        struct Pick { entt::entity Piece; std::string Source, Variant; };
        std::vector<Pick> picks;
        std::vector<AssetLibrary::AsyncHandle> tickets;
        for (const auto& [slot, piece] : Pieces(w, e)) {
            if (std::find(locks.begin(), locks.end(), slot) != locks.end() ||
                (w.Registry.get<OutfitPieceComponent>(piece).Flags & OutfitPieceBodyPart))
                continue;
            for (const auto& g : ColourGroups(w, a, piece)) {
                if (g.Options.empty()) continue;
                picks.push_back({piece, g.Source, g.Options[rng() % g.Options.size()]});
                RequestMaterial(a, picks.back().Variant, tickets);
            }
        }
        WhenReady(w, a, e, std::move(tickets), [picks](World& w2, AssetLibrary& a2, entt::entity) {
            for (const auto& p : picks)
                if (w2.Registry.valid(p.Piece)) SetColourway(w2, a2, p.Piece, p.Source, p.Variant);
        });
    });
}

// --- Colourways -------------------------------------------------------------------------------

std::vector<ColourGroup> ColourGroups(const World& world, AssetLibrary& assets, entt::entity piece) {
    std::vector<ColourGroup> out;
    const auto* rc = world.Registry.try_get<RenderableComponent>(piece);
    if (!rc || !rc->ModelRef) return out;
    const auto sources = SlotSources(assets, *rc->ModelRef);
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].empty()) continue;
        if (std::any_of(out.begin(), out.end(), [&](const ColourGroup& g) { return Same(g.Source, sources[i]); })) continue;
        ColourGroup g;
        g.Source = sources[i];
        g.Current = i < rc->Materials.size() && rc->Materials[i] ? Rel(rc->Materials[i]->Path) : sources[i];
        g.Options = Wardrobe::Colourways(sources[i], Siblings(sources[i]));
        if (g.Options.size() < 2) continue; // nothing to choose (skin, a single material)
        out.push_back(std::move(g));
    }
    return out;
}

bool SetColourway(World& world, AssetLibrary& assets, entt::entity piece, const std::string& source, const std::string& variant) {
    auto* rc = world.Registry.try_get<RenderableComponent>(piece);
    if (!rc || !rc->ModelRef) return false;
    auto mat = Material(assets, variant);
    if (!mat) return false;
    const auto sources = SlotSources(assets, *rc->ModelRef);
    if (rc->Materials.size() < sources.size()) rc->Materials.resize(sources.size());
    bool any = false;
    for (size_t i = 0; i < sources.size(); ++i)
        if (!sources[i].empty() && Same(sources[i], source)) { rc->Materials[i] = mat; any = true; }
    return any;
}

// --- Adopt / presets --------------------------------------------------------------------------

int AdoptExisting(World& world, AssetLibrary& assets, entt::entity root) {
    Result r;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return 0;
    auto& reg = world.Registry;
    auto& outfit = reg.get<CharacterOutfitComponent>(root);
    const auto* h = reg.try_get<HierarchyComponent>(root);
    if (!h) return 0;
    int tagged = 0;
    for (entt::entity c : std::vector<entt::entity>(h->Children)) {
        if (!reg.valid(c) || reg.all_of<OutfitPieceComponent>(c)) continue;
        const auto* rc = reg.try_get<RenderableComponent>(c);
        if (!rc || !rc->ModelRef) continue;
        const std::string path = Rel(rc->ModelRef->Path());
        OutfitPieceComponent piece{std::string(), path, 0};
        for (int g = 0; g < 2 && piece.Slot.empty(); ++g) {
            const auto& body = cat->W.Bodies[g];
            auto isPart = [&](const std::string& model) { return Same(model, path); };
            for (const auto& [part, model] : body.Parts)
                if (isPart(model)) { piece.Slot = part; outfit.Gender = g; }
            for (const auto& race : body.Races) {
                if (isPart(race.Head)) { piece.Slot = "Head"; outfit.Gender = g; outfit.Race = race.Name; }
                for (const auto& [part, model] : race.Parts)
                    if (isPart(model)) { piece.Slot = part; outfit.Gender = g; }
            }
            for (const auto& [name, model] : body.Alternates)
                if (isPart(model)) piece.Slot = "Feet";
            if (!piece.Slot.empty()) piece.Flags = OutfitPieceBodyPart | (piece.Slot == "Head" ? OutfitPieceHeadAttached : 0);
        }
        if (piece.Slot.empty())
            if (const Wardrobe::Item* item = cat->Find(path)) {
                piece.Slot = item->Slot;
                const auto* slot = cat->W.Slot(item->Slot);
                piece.Flags = slot && slot->HeadAttached ? OutfitPieceHeadAttached : 0;
            }
        if (piece.Slot.empty()) continue;
        if (Pieces(world, root).count(piece.Slot)) continue; // one piece per slot
        reg.emplace<OutfitPieceComponent>(c, piece);
        ++tagged;
    }
    if (tagged) ++outfit.Version;
    return tagged;
}

bool SavePreset(const World& world, AssetLibrary& assets, entt::entity root, const std::string& path, std::string* error) {
    const auto& reg = world.Registry;
    const auto* outfit = reg.try_get<CharacterOutfitComponent>(root);
    if (!outfit) { if (error) *error = "no Character Outfit"; return false; }
    json j;
    j["wardrobe"] = outfit->Wardrobe;
    j["gender"] = Wardrobe::GenderName(outfit->Gender == 1 ? Wardrobe::Gender::Female : Wardrobe::Gender::Male);
    j["race"] = outfit->Race;
    json items = json::object();
    for (const auto& [slot, e] : Pieces(world, root)) {
        const auto& p = reg.get<OutfitPieceComponent>(e);
        if (p.Flags & OutfitPieceBodyPart) continue;
        json entry;
        entry["item"] = p.Item;
        json colours = json::object();
        for (const auto& g : ColourGroups(world, assets, e))
            if (!Same(g.Current, g.Source)) colours[g.Source] = g.Current;
        if (!colours.empty()) entry["colours"] = colours;
        items[slot] = entry;
    }
    j["items"] = items;
    std::ofstream out(ProjectPaths::Resolve(Rel(path)), std::ios::binary);
    if (!out) { if (error) *error = "can't write " + path; return false; }
    out << j.dump(2) << "\n";
    return true;
}

Result LoadPreset(World& world, AssetLibrary& assets, entt::entity root, const std::string& path) {
    Result r;
    std::ifstream in(ProjectPaths::Resolve(Rel(path)), std::ios::binary);
    if (!in) { r.Error = "can't open " + path; return r; }
    json j;
    try { in >> j; } catch (const std::exception& e) { r.Error = path + ": " + e.what(); return r; }
    Wardrobe::Request req;
    req.Sex = Lower(j.value("gender", std::string("Male"))) == "female" ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
    req.Race = j.value("race", std::string());
    std::map<std::string, json> colours;
    if (j.contains("items") && j["items"].is_object())
        for (const auto& [slot, v] : j["items"].items()) {
            if (!v.is_object() || !v.contains("item")) continue;
            req.Items[slot] = v["item"].get<std::string>();
            if (v.contains("colours")) colours[slot] = v["colours"];
        }
    std::vector<AssetLibrary::AsyncHandle> colourTickets;
    for (const auto& [slot, c] : colours)
        if (c.is_object())
            for (const auto& [source, variant] : c.items())
                if (variant.is_string()) RequestMaterial(assets, variant.get<std::string>(), colourTickets);
    r = Submit(world, assets, root, req, [colours, colourTickets](World& w, AssetLibrary& a, entt::entity e) {
        WhenReady(w, a, e, colourTickets, [colours](World& w2, AssetLibrary& a2, entt::entity e2) {
            const auto pieces = Pieces(w2, e2);
            for (const auto& [slot, c] : colours) {
                auto it = pieces.find(slot);
                if (it == pieces.end() || !c.is_object()) continue;
                for (const auto& [source, variant] : c.items())
                    if (variant.is_string()) SetColourway(w2, a2, it->second, source, variant.get<std::string>());
            }
        });
    });
    if (!r.Ok) return r;
    return r;
}

// --- Skin hiding ------------------------------------------------------------------------------

namespace {

// A piece's bind-pose geometry in the world (metres), as Model::CollisionGeometry lists it.
OutfitCoverage::Mesh Geometry(const World& world, entt::entity e) {
    OutfitCoverage::Mesh m;
    const auto* rc = world.Registry.try_get<RenderableComponent>(e);
    if (!rc || !rc->ModelRef) return m;
    rc->ModelRef->CollisionGeometry(m.Positions, m.Indices);
    const glm::mat4 xf = world.ComposeWorldTransform(e);
    for (auto& p : m.Positions) p = glm::vec3(xf * glm::vec4(p, 1.0f));
    return m;
}

// What covers what: body parts are covered by every item but those on the head; a top by outerwear.
bool Covers(const OutfitPieceComponent& over, const OutfitPieceComponent& under) {
    if (&over == &under || (over.Flags & (OutfitPieceBodyPart | OutfitPieceHeadAttached))) return false;
    if (under.Flags & OutfitPieceBodyPart) return !(under.Flags & OutfitPieceHeadAttached);
    return under.Slot == "Top" && over.Slot == "Outerwear";
}

// The pieces as the hiding last saw them: entities, models, which carry a hide tag (an undo drops them).
std::uint64_t HideSignature(const World& world, const CharacterOutfitComponent& outfit,
                            const std::map<std::string, entt::entity>& pieces) {
    std::uint64_t sig = outfit.AutoHide ? 1469598103934665603ull : 7ull;
    auto mix = [&](std::uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    for (const auto& [slot, e] : pieces) {
        mix((std::uint64_t)entt::to_integral(e));
        const auto* rc = world.Registry.try_get<RenderableComponent>(e);
        mix(rc ? (std::uint64_t)(std::uintptr_t)rc->ModelRef.get() : 0ull);
        mix(world.Registry.all_of<OutfitHideTag>(e) ? 2ull : 3ull);
    }
    return sig;
}

} // namespace

namespace {

// Coverage per (under model, over model) pair, by project-relative lower-case path. Worked out on a
// background thread and kept in Library/OutfitCoverage, so a pair is computed once per machine, not once
// per editor session, and never on the main thread.
using CoverageKey = std::pair<std::string, std::string>;
struct CoverageStore {
    std::map<CoverageKey, std::vector<std::uint8_t>> Done;
    std::map<CoverageKey, std::future<std::vector<std::uint8_t>>> Running;
};
CoverageStore& Coverage() {
    static CoverageStore s;
    return s;
}

constexpr std::uint32_t kCoverageVersion = 1; // bump when Covered/Erode or their settings change

struct FileStamp {
    std::uint64_t Size = 0;
    std::int64_t Time = 0;
};
FileStamp StampOf(const std::string& rel) {
    FileStamp s;
    std::error_code ec;
    const fs::path p = ProjectPaths::Resolve(rel);
    s.Size = (std::uint64_t)fs::file_size(p, ec);
    if (ec) s.Size = 0;
    const auto t = fs::last_write_time(p, ec);
    if (!ec) s.Time = (std::int64_t)t.time_since_epoch().count();
    return s;
}

fs::path CoverageFile(const CoverageKey& k) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::string* s : {&k.first, &k.second}) {
        for (char c : *s) h = (h ^ (unsigned char)c) * 1099511628211ull;
        h = (h ^ 0xffu) * 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.cov", (unsigned long long)h);
    return fs::path(ProjectPaths::Resolve("Library/OutfitCoverage")) / name;
}

struct CoverageHeader {
    char Magic[4] = {'T', 'O', 'C', 'V'};
    std::uint32_t Version = kCoverageVersion;
    FileStamp Under, Over;
    std::uint32_t Count = 0;
};

bool LoadCoverage(const CoverageKey& k, const FileStamp& under, const FileStamp& over, std::vector<std::uint8_t>& out) {
    std::ifstream in(CoverageFile(k), std::ios::binary);
    if (!in) return false;
    CoverageHeader h, want;
    if (!in.read(reinterpret_cast<char*>(&h), sizeof(h))) return false;
    if (std::memcmp(h.Magic, want.Magic, 4) != 0 || h.Version != kCoverageVersion || h.Under.Size != under.Size ||
        h.Under.Time != under.Time || h.Over.Size != over.Size || h.Over.Time != over.Time)
        return false;
    out.resize(h.Count);
    return (bool)in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)h.Count);
}

// Any thread. Written beside and renamed into place, so a reader never sees half a file.
void SaveCoverage(const CoverageKey& k, const FileStamp& under, const FileStamp& over, const std::vector<std::uint8_t>& data) {
    const fs::path file = CoverageFile(k);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    static std::atomic<unsigned> counter{0};
    fs::path tmp = file;
    tmp += ".tmp" + std::to_string(counter++);
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        CoverageHeader h;
        h.Under = under;
        h.Over = over;
        h.Count = (std::uint32_t)data.size();
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        if (!out) { out.close(); fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

// The pair's covered vertices if they're known (memory, then disk); else starts working them out in the
// background and returns null - ask again next frame.
const std::vector<std::uint8_t>* PairCoverage(const World& world, entt::entity under, entt::entity over,
                                              const std::string& underModel, const std::string& overModel) {
    CoverageStore& store = Coverage();
    const CoverageKey key{Lower(Rel(underModel)), Lower(Rel(overModel))};
    if (auto it = store.Done.find(key); it != store.Done.end()) return &it->second;
    if (auto it = store.Running.find(key); it != store.Running.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        auto done = store.Done.emplace(key, it->second.get()).first;
        store.Running.erase(it);
        return &done->second;
    }
    const FileStamp us = StampOf(key.first), os = StampOf(key.second);
    std::vector<std::uint8_t> loaded;
    if (LoadCoverage(key, us, os, loaded)) return &store.Done.emplace(key, std::move(loaded)).first->second;
    // The geometry is copied here (main thread); the maths and the save run on their own thread.
    store.Running.emplace(key, std::async(std::launch::async,
                                          [key, us, os, body = Geometry(world, under), cloth = Geometry(world, over)] {
                                              std::vector<std::uint8_t> covered = OutfitCoverage::Covered(body, cloth);
                                              OutfitCoverage::Erode(body, covered, 1);
                                              SaveCoverage(key, us, os, covered);
                                              return covered;
                                          }));
    return nullptr;
}

} // namespace

void UpdateHiding(World& world) {
    auto& reg = world.Registry;
    for (entt::entity root : reg.view<CharacterOutfitComponent>()) {
        auto& outfit = reg.get<CharacterOutfitComponent>(root);
        const auto pieces = Pieces(world, root);
        if (HideSignature(world, outfit, pieces) == outfit.HideSignature) continue;

        // Old tags go at once (a re-modelled piece must not keep bits for another mesh); new ones go
        // on when every pair they need is known. Until then the skin just isn't hidden.
        for (const auto& [slot, e] : pieces) reg.remove<OutfitHideTag>(e);
        bool waiting = false;
        std::vector<std::pair<entt::entity, std::vector<std::uint8_t>>> tags;
        if (outfit.AutoHide)
            for (const auto& [slot, under] : pieces) {
                const auto& u = reg.get<OutfitPieceComponent>(under);
                const auto* urc = reg.try_get<RenderableComponent>(under);
                if (!urc || !urc->ModelRef) continue;
                std::vector<std::uint8_t> hidden;
                for (const auto& [overSlot, over] : pieces) {
                    const auto& o = reg.get<OutfitPieceComponent>(over);
                    const auto* orc = reg.try_get<RenderableComponent>(over);
                    if (!Covers(o, u) || !orc || !orc->ModelRef) continue;
                    const auto* covered = PairCoverage(world, under, over, urc->ModelRef->Path(), orc->ModelRef->Path());
                    if (!covered) { waiting = true; continue; }
                    if (hidden.empty()) hidden = *covered;
                    else
                        for (size_t i = 0; i < hidden.size() && i < covered->size(); ++i) hidden[i] |= (*covered)[i];
                }
                tags.emplace_back(under, std::move(hidden));
            }
        if (waiting) continue; // this outfit's signature stays stale, so it's looked at again next frame

        for (auto& [under, hidden] : tags) {
            const int count = (int)std::count(hidden.begin(), hidden.end(), (std::uint8_t)1);
            if (!count) continue;
            auto& tag = reg.emplace<OutfitHideTag>(under);
            tag.Buffer = std::make_shared<SkinHideBuffer>(OutfitCoverage::Pack(hidden));
            tag.Hidden = count;
            tag.Total = (int)hidden.size();
        }
        outfit.HideSignature = HideSignature(world, outfit, pieces);
    }
}

} // namespace OutfitSystem
