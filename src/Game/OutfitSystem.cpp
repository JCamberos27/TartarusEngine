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

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
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
#include <stdexcept>
#include <thread>

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
std::map<std::string, std::vector<std::string>>& SiblingCache() {
    static std::map<std::string, std::vector<std::string>> cache;
    return cache;
}

const std::vector<std::string>& Siblings(const std::string& matPath) {
    auto& cache = SiblingCache();
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
    std::string s = Lower(stem);
    for (const char* p : {"skm_f_", "sm_f_", "skm_", "sm_"})
        if (s.rfind(p, 0) == 0) { s = s.substr(std::string(p).size()); break; }
    // A cut made for another item is the same item for the other gender ("jeans_inboots" -> "jeans").
    for (const char* suffix : {"_inboots"})
        if (s.size() > std::strlen(suffix) && s.compare(s.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
            s.resize(s.size() - std::strlen(suffix));
    return s;
}

// How alike two cut names are: shared underscore-separated words over all words (0..1).
float CutLikeness(const std::string& a, const std::string& b) {
    auto words = [](const std::string& s) {
        std::set<std::string> out;
        std::stringstream ss(s);
        for (std::string w; std::getline(ss, w, '_');)
            if (!w.empty()) out.insert(w);
        return out;
    };
    const auto wa = words(a), wb = words(b);
    size_t shared = 0;
    for (const auto& w : wa) shared += wb.count(w);
    const size_t all = wa.size() + wb.size() - shared;
    return all ? (float)shared / (float)all : 0.0f;
}

std::shared_ptr<const Catalog> CatalogFor(World& world, AssetLibrary& assets, entt::entity root, Result& r) {
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(root);
    if (!outfit) { r.Error = "no Character Outfit on the object"; return nullptr; }
    auto cat = LoadCatalog(assets, outfit->Wardrobe, false, &r.Error);
    return cat;
}

// The body's driving Animator (the first piece with one), for new pieces to follow.
// The entity whose Animator Controller drives the outfit: the root's own, else the first child's that
// isn't following another (First Person Body picks the same one).
entt::entity DriverEntity(const World& world, entt::entity root) {
    const auto& reg = world.Registry;
    if (reg.all_of<AnimatorControllerComponent>(root)) return root;
    if (const auto* h = reg.try_get<HierarchyComponent>(root))
        for (entt::entity c : h->Children)
            if (reg.valid(c))
                if (const auto* ac = reg.try_get<AnimatorControllerComponent>(c); ac && ac->Driver == entt::null) return c;
    return entt::null;
}

const AnimatorControllerComponent* Driver(const World& world, entt::entity root) {
    const entt::entity e = DriverEntity(world, root);
    return e == entt::null ? nullptr : world.Registry.try_get<AnimatorControllerComponent>(e);
}

// Every other piece with an Animator Controller mirrors the driver's (AnimatorController's follower mode):
// one state machine per character, the pieces always in step, and parameters set on the driver reach
// them all. Runtime only - a loaded scene is linked again by UpdateAttachments.
void LinkFollowers(World& world, entt::entity root, entt::entity driver) {
    auto& reg = world.Registry;
    if (driver == entt::null || !reg.valid(driver)) return;
    const auto* h = reg.try_get<HierarchyComponent>(root);
    if (!h) return;
    for (entt::entity c : h->Children)
        if (c != driver && reg.valid(c) && reg.all_of<OutfitPieceComponent>(c))
            if (auto* ac = reg.try_get<AnimatorControllerComponent>(c)) ac->Driver = driver;
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

namespace {

// The wardrobes the skin hiding reads its layers from, by lower-case project-relative path: a loaded
// catalog's (a rescan replaces it), else the .wardrobe parsed on its own - the hiding needs no item scan.
// Bumped whenever a wardrobe is (re)loaded, so what caches one knows to look again.
int& WardrobeGeneration() {
    static int g = 0;
    return g;
}

std::map<std::string, std::shared_ptr<const Wardrobe::Wardrobe>>& LayerWardrobes() {
    static std::map<std::string, std::shared_ptr<const Wardrobe::Wardrobe>> s;
    return s;
}

// Null when the file can't be read or parsed (then nothing is layered, only body parts are hidden).
std::shared_ptr<const Wardrobe::Wardrobe> LayerWardrobe(const std::string& path) {
    auto& all = LayerWardrobes();
    const std::string key = Lower(Rel(path));
    if (auto it = all.find(key); it != all.end()) return it->second;
    std::shared_ptr<Wardrobe::Wardrobe> w;
    std::ifstream in(ProjectPaths::Resolve(Rel(path)), std::ios::binary);
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        w = std::make_shared<Wardrobe::Wardrobe>();
        if (!Wardrobe::Parse(text.str(), *w)) w.reset();
    }
    return all[key] = w;
}

} // namespace

std::shared_ptr<const Catalog> LoadCatalog(AssetLibrary& assets, const std::string& path, bool rescan, std::string* error) {
    static std::map<std::string, std::shared_ptr<const Catalog>> cache;
    // Failures too: the Inspector asks every frame, and a missing or broken file would be read each time.
    static std::map<std::string, std::string> failed;
    const std::string key = Lower(Rel(path));
    if (!rescan) {
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        if (auto it = failed.find(key); it != failed.end()) {
            if (error) *error = it->second;
            return nullptr;
        }
    }
    failed.erase(key);
    if (rescan) {
        // New colourway .mat files and wardrobe edits are picked up too.
        SiblingCache().clear();
        LayerWardrobes().erase(key);
    }

    std::ifstream in(ProjectPaths::Resolve(Rel(path)), std::ios::binary);
    if (!in) {
        failed[key] = "can't open wardrobe " + path;
        if (error) *error = failed[key];
        return nullptr;
    }
    std::stringstream text;
    text << in.rdbuf();
    auto cat = std::make_shared<Catalog>();
    cat->Path = Rel(path);
    std::string parseError;
    if (!Wardrobe::Parse(text.str(), cat->W, &parseError)) {
        failed[key] = path + ": " + parseError;
        if (error) *error = failed[key];
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
    LayerWardrobes()[key] = std::shared_ptr<const Wardrobe::Wardrobe>(cat, &cat->W);
    ++WardrobeGeneration();
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
    const Wardrobe::RaceDef* race = Wardrobe::FindRace(cat->W, request.Sex, request.Race);
    const Wardrobe::Resolved resolved = Wardrobe::Resolve(cat->W, cat->Items, request);
    r.Notes = resolved.Notes;
    for (const auto& c : resolved.Clashes) r.Notes.push_back("Odd pairing: " + c);

    // A second piece in a slot (a duplicated child): Pieces() sees only the first, so nothing would ever
    // replace, hide or remove it.
    if (const auto* h = reg.try_get<HierarchyComponent>(root)) {
        std::set<std::string> seen;
        std::vector<entt::entity> extra;
        for (entt::entity c : h->Children)
            if (reg.valid(c))
                if (const auto* p = reg.try_get<OutfitPieceComponent>(c); p && !seen.insert(p->Slot).second) extra.push_back(c);
        for (entt::entity c : extra) {
            r.Notes.push_back("Removed a second " + reg.get<OutfitPieceComponent>(c).Slot + " piece.");
            world.DestroyEntityAndChildren(c);
        }
    }

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
    // Remember which entity drives (the component moves when the registry grows).
    const entt::entity driverEntity = DriverEntity(world, root);
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
    std::set<std::string> failed; // slots whose new model didn't load: they keep the old piece and its record
    for (const auto& p : diff.Remodel) {
        const entt::entity e = pieces[p.Slot];
        auto model = assets.InstantiateModel(ProjectPaths::Resolve(p.Path));
        if (!model) { r.Notes.push_back("can't load " + p.Path); failed.insert(p.Slot); continue; }
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
        if (it == pieces.end() || failed.count(p.Slot)) continue;
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

    LinkFollowers(world, root, driverEntity);

    // `outfit` again: creating pieces can move the component's storage.
    auto& done = reg.get<CharacterOutfitComponent>(root);
    done.Gender = (int)request.Sex;
    if (race) done.Race = race->Name;
    ++done.Version;
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
    for (const auto& c : resolved.Clashes) notes.push_back("Odd pairing: " + c);
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
    // Colourway-only steps (SubmitColourway, a preset's colours) stay: they apply to the pieces, whatever
    // the outfit becomes.
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const PendingChange& pc) { return pc.W == &world && pc.Root == root && pc.HasRequest; }),
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
    // Each item's cut for the other gender: the same name, else the most alike name in the slot. The pieces
    // an outfit can't do without (Randomize's whole-outfit slots) take the most alike even when nothing is
    // close, so a switch never leaves the character without pants.
    for (const auto& [slot, path] : req.Items) {
        const Wardrobe::Item* from = cat->Find(path);
        if (!from) continue;
        const std::string cut = CutName(from->Stem);
        const Wardrobe::Item* best = nullptr;
        float bestScore = -1.0f;
        for (const auto* it : cat->ForSlot(slot, gender)) {
            if (it->Variant) continue; // the rules pick those
            const float score = CutName(it->Stem) == cut ? 2.0f : CutLikeness(CutName(it->Stem), cut);
            if (score > bestScore) { bestScore = score; best = it; }
        }
        const bool essential = slot == "Top" || slot == "Pants" || slot == "Shoes";
        if (best && (bestScore >= 0.34f || essential)) next.Items[slot] = best->Path;
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

Result Randomize(World& world, AssetLibrary& assets, entt::entity root, std::uint32_t seed, const std::string& style) {
    Result r;
    auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return r;
    std::mt19937 rng(seed ^ 0x9e3779b9u); // the colourways' own stream
    const auto locks = ParseLocks(world.Registry.get<CharacterOutfitComponent>(root).Locks);
    // The outfit itself: a style, then slot by slot only what goes with the rest (Wardrobe::Randomize).
    std::string picked;
    const Wardrobe::Request req = Wardrobe::Randomize(cat->W, cat->Items, BaseRequest(world, root), seed, locks, style, &picked);
    if (!picked.empty()) r.Notes.push_back("Style: " + picked);
    // Colourways once the pieces are on: pick them, load them in the background, then put them on.
    Result submitted = Submit(world, assets, root, req, [rng, locks](World& w, AssetLibrary& a, entt::entity e) mutable {
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
    submitted.Notes.insert(submitted.Notes.begin(), r.Notes.begin(), r.Notes.end());
    return submitted;
}

// --- Colourways -------------------------------------------------------------------------------

std::vector<ColourGroup> ColourGroups(const World& world, AssetLibrary& assets, entt::entity piece) {
    std::vector<ColourGroup> out;
    const auto* rc = world.Registry.try_get<RenderableComponent>(piece);
    if (!rc || !rc->ModelRef) return out;
    // Body parts take the race's skin (Wardrobe::SkinMaterialFor), not a colourway: their materials share a
    // folder with every other skin, eye and teeth material, which aren't choices for them.
    if (const auto* p = world.Registry.try_get<OutfitPieceComponent>(piece); p && (p->Flags & OutfitPieceBodyPart)) return out;
    const auto sources = SlotSources(assets, *rc->ModelRef);
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].empty()) continue;
        if (std::any_of(out.begin(), out.end(), [&](const ColourGroup& g) { return Same(g.Source, sources[i]); })) continue;
        ColourGroup g;
        g.Source = sources[i];
        g.Current = i < rc->Materials.size() && rc->Materials[i] ? Rel(rc->Materials[i]->Path) : sources[i];
        // Colourways are the materials beside it (Materials/Clothing/<Category>/<Item>/),
        // except in shared folders: skin, eyes and teeth (Materials/Characters) are no one's colourway, and
        // Materials/Clothing/Generated holds one plain material per untextured item.
        const std::string folder = Lower(Folder(sources[i]));
        if (folder.find("/materials/characters") != std::string::npos || folder.find("/generated") != std::string::npos) continue;
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
    // What caches the outfit's look (the first-person body's twins, the editor) notices by the version.
    if (any)
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(piece); h && world.Registry.valid(h->Parent))
            if (auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(h->Parent)) ++outfit->Version;
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
            // An alternate stands in for the part a cover rule replaces with it ("ShoeFeet" for "Feet").
            for (const auto& [name, model] : body.Alternates)
                if (isPart(model)) {
                    piece.Slot = "Feet";
                    for (const auto& rule : cat->W.Covers)
                        if (rule.With == name && !rule.Replace.empty()) { piece.Slot = rule.Replace; break; }
                    outfit.Gender = g;
                }
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
    Wardrobe::Request req;
    std::map<std::string, json> colours;
    try {
        json j;
        in >> j;
        if (!j.is_object()) throw std::runtime_error("not a preset");
        auto text = [&](const json& o, const char* key) {
            return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
        };
        if (j.contains("gender") && !j["gender"].is_string()) throw std::runtime_error("\"gender\" isn't a name");
        req.Sex = Lower(text(j, "gender")) == "female" ? Wardrobe::Gender::Female : Wardrobe::Gender::Male;
        req.Race = text(j, "race");
        const std::string wardrobe = text(j, "wardrobe");
        if (const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(root);
            outfit && !wardrobe.empty() && !Same(wardrobe, outfit->Wardrobe))
            r.Notes.push_back("Made for " + wardrobe + ": its items may not be in this wardrobe.");
        if (j.contains("items") && j["items"].is_object())
            for (const auto& [slot, v] : j["items"].items()) {
                if (!v.is_object() || !v.contains("item")) continue;
                if (!v["item"].is_string()) throw std::runtime_error("the " + slot + " item isn't a path");
                req.Items[slot] = v["item"].get<std::string>();
                if (v.contains("colours")) colours[slot] = v["colours"];
            }
    } catch (const std::exception& e) {
        r.Error = path + ": " + e.what();
        return r;
    }
    const auto notes = r.Notes;
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
    r.Notes.insert(r.Notes.begin(), notes.begin(), notes.end());
    return r;
}

// --- Skin hiding ------------------------------------------------------------------------------

namespace {

// A piece's bind-pose geometry in the world (metres), as Model::CollisionGeometry lists it.
OutfitCoverage::Mesh Geometry(const World& world, entt::entity e) {
    OutfitCoverage::Mesh m;
    const auto& reg = world.Registry;
    const auto* rc = reg.try_get<RenderableComponent>(e);
    if (!rc || !rc->ModelRef) return m;
    rc->ModelRef->CollisionGeometry(m.Positions, m.Indices);
    // Rigid head wear is moved onto the head bone every frame (UpdateAttachments); modelled in the head's
    // bind pose, that pose is where it belongs - its place under the root, like every other piece.
    glm::mat4 xf = world.ComposeWorldTransform(e);
    if (const auto* p = reg.try_get<OutfitPieceComponent>(e);
        p && (p->Flags & OutfitPieceHeadAttached) && !(p->Flags & OutfitPieceBodyPart) && rc->ModelRef->BoneCount() == 0)
        if (const auto* h = reg.try_get<HierarchyComponent>(e); h && reg.valid(h->Parent)) xf = world.ComposeWorldTransform(h->Parent);
    for (auto& p : m.Positions) p = glm::vec3(xf * glm::vec4(p, 1.0f));
    return m;
}

// Where a piece is worn (Wardrobe::LayerOf); with no wardrobe, by the default layers of its slot.
Wardrobe::Layering PieceLayer(const Wardrobe::Wardrobe* w, const OutfitPieceComponent& p) {
    static const Wardrobe::Wardrobe none;
    return Wardrobe::LayerOf(w ? *w : none, p.Slot, p.Item, (p.Flags & OutfitPieceBodyPart) != 0);
}

// The pieces as the hiding last saw them: entities, models, which carry a hide tag (an undo drops them),
// and the wardrobe their layers came from (a rescan brings a new one).
// Straight off the root's children (no map built: it runs for every outfit, every frame).
std::uint64_t HideSignature(const World& world, const CharacterOutfitComponent& outfit, entt::entity root,
                            const Wardrobe::Wardrobe* w) {
    std::uint64_t sig = outfit.AutoHide ? 1469598103934665603ull : 7ull;
    auto mix = [&](std::uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix((std::uint64_t)(std::uintptr_t)w);
    auto mixText = [&](const std::string& t) {
        for (char c : t) mix((unsigned char)c);
        mix(0xffull);
    };
    const auto* h = world.Registry.try_get<HierarchyComponent>(root);
    if (!h) return sig;
    for (entt::entity e : h->Children) {
        if (!world.Registry.valid(e)) continue;
        const auto* p = world.Registry.try_get<OutfitPieceComponent>(e);
        if (!p) continue;
        mix((std::uint64_t)entt::to_integral(e));
        // What its layer comes from (PieceLayer): an edit in the Inspector changes these without a new model.
        const auto& piece = *p;
        mixText(piece.Slot);
        mixText(piece.Item);
        mix((std::uint64_t)piece.Flags);
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

constexpr std::uint32_t kCoverageVersion = 15; // bump when Covered/Erode or their settings change

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
// `exposed`: `under` is the head; `rigid`: headwear on the head bone (OutfitCoverage::Hidden) - each its own
// entry, keyed apart.
const std::vector<std::uint8_t>* PairCoverage(const World& world, entt::entity under, entt::entity over,
                                              const std::string& underModel, const std::string& overModel, bool exposed,
                                              bool rigid) {
    CoverageStore& store = Coverage();
    // The thresholds are in metres, so a character scaled up or down has coverage of its own.
    std::string scale;
    if (const auto* h = world.Registry.try_get<HierarchyComponent>(under); h && world.Registry.valid(h->Parent)) {
        const glm::mat4 root = world.ComposeWorldTransform(h->Parent);
        const float s = glm::length(glm::vec3(root[1]));
        if (std::abs(s - 1.0f) > 0.005f) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "|x%.2f", s);
            scale = buf;
        }
    }
    const CoverageKey key{Lower(Rel(underModel)) + (exposed ? "|exposed" : "") + (rigid ? "|rigid" : "") + scale, Lower(Rel(overModel))};
    if (auto it = store.Done.find(key); it != store.Done.end()) return &it->second;
    if (auto it = store.Running.find(key); it != store.Running.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        auto done = store.Done.emplace(key, it->second.get()).first;
        store.Running.erase(it);
        return &done->second;
    }
    const FileStamp us = StampOf(Lower(Rel(underModel))), os = StampOf(key.second);
    std::vector<std::uint8_t> loaded;
    if (LoadCoverage(key, us, os, loaded)) return &store.Done.emplace(key, std::move(loaded)).first->second;
    // A few at a time: a scene of new outfits with a cold Library/OutfitCoverage used to start a thread
    // per pair (hundreds). The rest are started as these finish (UpdateHiding asks again).
    static const size_t kMaxRunning = std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1;
    if (store.Running.size() >= kMaxRunning) {
        for (auto it = store.Running.begin(); it != store.Running.end();)
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                store.Done.emplace(it->first, it->second.get());
                it = store.Running.erase(it);
            } else {
                ++it;
            }
        if (store.Running.size() >= kMaxRunning) return nullptr;
    }
    // The geometry is copied here (main thread); the maths and the save run on their own thread.
    store.Running.emplace(key, std::async(std::launch::async,
                                          [key, us, os, exposed, rigid, body = Geometry(world, under), cloth = Geometry(world, over)] {
                                              std::vector<std::uint8_t> covered = OutfitCoverage::Hidden(body, cloth, exposed, rigid);
                                              SaveCoverage(key, us, os, covered);
                                              return covered;
                                          }));
    return nullptr;
}

// The triangles of `model` not covered entirely (a vertex of them visible), shared by every piece with the
// same model and bits - characters in the same outfit draw from one buffer.
std::shared_ptr<VisibleIndexBuffer> VisibleTriangles(const Model& model, const std::vector<std::uint32_t>& bits) {
    static std::map<std::pair<const void*, std::uint64_t>, std::weak_ptr<VisibleIndexBuffer>> cache;
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t w : bits) h = (h ^ w) * 1099511628211ull;
    // Instances share their mesh data (and its first mesh's index list), so that's the model's identity.
    const void* id = model.MeshCount() ? (const void*)model.MeshLocalIndices(0).data() : (const void*)&model;
    const auto key = std::make_pair(id, h ^ (std::uint64_t)bits.size());
    if (auto it = cache.find(key); it != cache.end())
        if (auto live = it->second.lock()) return live;
    auto hidden = [&](std::uint32_t v) { return (v >> 5) < bits.size() && ((bits[v >> 5] >> (v & 31)) & 1u); };
    std::vector<std::vector<std::uint32_t>> perMesh((size_t)model.MeshCount());
    std::uint32_t first = 0;
    size_t kept = 0, total = 0;
    for (int m = 0; m < model.MeshCount(); ++m) {
        const auto& idx = model.MeshLocalIndices(m);
        auto& out = perMesh[(size_t)m];
        out.reserve(idx.size());
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
            if (!hidden(first + idx[i]) || !hidden(first + idx[i + 1]) || !hidden(first + idx[i + 2]))
                out.insert(out.end(), {idx[i], idx[i + 1], idx[i + 2]});
        kept += out.size();
        total += idx.size();
        first += (std::uint32_t)model.MeshVertexCount(m);
    }
    if (kept == total) return nullptr; // nothing covered entirely: the mesh's own indices are the same
    auto buffer = std::make_shared<VisibleIndexBuffer>(perMesh);
    // Drop entries whose buffers are gone, now and then (outfits change; the map shouldn't grow forever).
    if (cache.size() > 256)
        for (auto it = cache.begin(); it != cache.end();) it = it->second.expired() ? cache.erase(it) : std::next(it);
    cache[key] = buffer;
    return buffer;
}

} // namespace

void UpdateHiding(World& world) {
    auto& reg = world.Registry;
    static std::uint64_t frame = 0;
    ++frame;
    // The wardrobe of the last outfit looked at: nearly every outfit in a scene shares one.
    static std::string lastPath;
    static std::shared_ptr<const Wardrobe::Wardrobe> lastWardrobe;
    static int lastGeneration = -1;
    for (entt::entity root : reg.view<CharacterOutfitComponent>()) {
        auto& outfit = reg.get<CharacterOutfitComponent>(root);
        if (outfit.Wardrobe != lastPath || !lastWardrobe || lastGeneration != WardrobeGeneration()) {
            lastPath = outfit.Wardrobe;
            lastWardrobe = LayerWardrobe(outfit.Wardrobe);
            lastGeneration = WardrobeGeneration();
        }
        const auto wardrobe = lastWardrobe;
        const std::uint64_t signature = HideSignature(world, outfit, root, wardrobe.get());
        if (signature == outfit.HideSignature) continue;
        // Still waiting for coverage worked out in the background: look again every few frames, not every one.
        if (signature == outfit.HideWaiting && frame < outfit.HideRetryFrame) continue;
        const auto pieces = Pieces(world, root);
        std::map<entt::entity, Wardrobe::Layering> layers;
        for (const auto& [slot, e] : pieces) layers[e] = PieceLayer(wardrobe.get(), reg.get<OutfitPieceComponent>(e));

        // Old tags go at once (a re-modelled piece must not keep bits for another mesh); new ones go
        // on when every pair they need is known. Until then the skin just isn't hidden.
        for (const auto& [slot, e] : pieces) reg.remove<OutfitHideTag>(e);
        bool waiting = false;
        std::vector<std::pair<entt::entity, std::vector<std::uint8_t>>> tags;
        if (outfit.AutoHide)
            for (const auto& [slot, under] : pieces) {
                const auto* urc = reg.try_get<RenderableComponent>(under);
                if (!urc || !urc->ModelRef) continue;
                const int flags = reg.get<OutfitPieceComponent>(under).Flags;
                const bool exposed = (flags & OutfitPieceBodyPart) && (flags & OutfitPieceHeadAttached);
                const bool rigid = !(flags & OutfitPieceBodyPart) && (flags & OutfitPieceHeadAttached);
                std::vector<std::uint8_t> hidden;
                for (const auto& [overSlot, over] : pieces) {
                    const auto* orc = reg.try_get<RenderableComponent>(over);
                    if (over == under || !orc || !orc->ModelRef || !Wardrobe::Hides(layers[over], slot, layers[under], overSlot)) continue;
                    const auto* covered = PairCoverage(world, under, over, urc->ModelRef->Path(), orc->ModelRef->Path(), exposed, rigid);
                    if (!covered) { waiting = true; continue; }
                    if (hidden.empty()) hidden = *covered;
                    else
                        for (size_t i = 0; i < hidden.size() && i < covered->size(); ++i) hidden[i] |= (*covered)[i];
                }
                tags.emplace_back(under, std::move(hidden));
            }
        if (waiting) { // this outfit's signature stays stale, so it's looked at again soon
            outfit.HideWaiting = signature;
            outfit.HideRetryFrame = frame + 8;
            continue;
        }

        for (auto& [under, hidden] : tags) {
            const int count = (int)std::count(hidden.begin(), hidden.end(), (std::uint8_t)1);
            if (!count) continue;
            auto& tag = reg.emplace<OutfitHideTag>(under);
            auto bits = std::make_shared<const std::vector<std::uint32_t>>(OutfitCoverage::Pack(hidden));
            tag.Buffer = std::make_shared<SkinHideBuffer>(*bits);
            if (const auto* rc = reg.try_get<RenderableComponent>(under); rc && rc->ModelRef)
                tag.Visible = VisibleTriangles(*rc->ModelRef, *bits);
            tag.Bits = std::move(bits);
            tag.Hidden = count;
            tag.Total = (int)hidden.size();
        }
        outfit.HideSignature = signature;
    }
}

void UpdateAttachments(World& world) {
    auto& reg = world.Registry;
    for (entt::entity root : reg.view<CharacterOutfitComponent>()) {
        // A loaded scene (or an undo) brings pieces back as independent animators: link them again.
        if (auto& outfit = reg.get<CharacterOutfitComponent>(root); outfit.LinkedVersion != outfit.Version) {
            LinkFollowers(world, root, DriverEntity(world, root));
            outfit.LinkedVersion = outfit.Version;
        }
        // The root's children as they are (no map: this runs for every outfit, every frame).
        const auto* h = reg.try_get<HierarchyComponent>(root);
        if (!h) continue;
        entt::entity head = entt::null;
        bool anyRigid = false;
        for (entt::entity c : h->Children) {
            const auto* p = reg.valid(c) ? reg.try_get<OutfitPieceComponent>(c) : nullptr;
            if (!p) continue;
            if (head == entt::null && p->Slot == "Head") head = c;
            if ((p->Flags & OutfitPieceHeadAttached) && !(p->Flags & OutfitPieceBodyPart)) anyRigid = true;
        }
        if (head == entt::null || !anyRigid) continue;
        const auto* hrc = reg.try_get<RenderableComponent>(head);
        static const std::string kHeadBone = "head";
        const int bone = hrc && hrc->ModelRef ? hrc->ModelRef->BoneId(kHeadBone) : -1;
        if (bone < 0) continue;
        // The head piece's own place under the root (identity as the outfit builds it), then its head bone's
        // skinning matrix: bind-pose model space -> where the head is now.
        const auto& ht = reg.get<TransformComponent>(head);
        const glm::mat4 follow = glm::translate(glm::mat4(1.0f), ht.Position) * glm::mat4_cast(ht.Rotation) *
                                 glm::scale(glm::mat4(1.0f), ht.Scale) * hrc->ModelRef->FinalBoneMatrix(bone);
        glm::vec3 pos(follow[3]);
        glm::vec3 scale(glm::length(glm::vec3(follow[0])), glm::length(glm::vec3(follow[1])), glm::length(glm::vec3(follow[2])));
        const glm::mat3 rot(glm::vec3(follow[0]) / scale.x, glm::vec3(follow[1]) / scale.y, glm::vec3(follow[2]) / scale.z);
        for (entt::entity e : h->Children) {
            const auto* p = reg.valid(e) ? reg.try_get<OutfitPieceComponent>(e) : nullptr;
            if (!p || !(p->Flags & OutfitPieceHeadAttached) || (p->Flags & OutfitPieceBodyPart)) continue;
            const auto* rc = reg.try_get<RenderableComponent>(e);
            if (!rc || !rc->ModelRef || rc->ModelRef->BoneCount() > 0) continue; // skinned: it follows by its bones
            auto& t = reg.get<TransformComponent>(e);
            t.Position = pos;
            t.Rotation = glm::normalize(glm::quat_cast(rot));
            t.Scale = scale;
        }
    }
}

} // namespace OutfitSystem
