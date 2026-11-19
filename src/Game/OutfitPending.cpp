// The outfit changes waiting for their assets: Submit queues one, UpdatePending applies it once everything has loaded,
// and the body refresh rebuilds an outfit whose body parts predate its wardrobe. (OutfitSystem.h)
#include "OutfitSystem.h"
#include "OutfitSystemInternal.h"

#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "Log.h"
#include "MaterialAsset.h"
#include "Model.h"
#include "OutfitCoverage.h"
#include "SkinHideBuffer.h"
#include "Core/Paths.h"
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
using namespace detail;

// --- Waiting changes --------------------------------------------------------------------------

namespace detail {

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
        std::error_code ec;
        if (!fs::exists(abs, ec)) continue; // not on this machine (a checkout without the Drive's assets): the apply says so
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

} // namespace detail

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

bool AnyPending() { return !Waiting().empty(); }

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

namespace {
unsigned g_LandedGeneration = 0;
} // namespace

unsigned LandedGeneration() { return g_LandedGeneration; }

bool LandingDue(const World& world) {
    for (const auto& pc : Waiting())
        if (pc.W == &world && AllReady(pc.Tickets)) return true;
    for (auto [root, outfit] : world.Registry.view<const CharacterOutfitComponent>().each())
        if (outfit.BodyChecked != WardrobeGeneration() && !IsPending(world, root)) return true;
    return false;
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
            ++g_LandedGeneration;
        }
        if (ready.After) { ready.After(world, assets, ready.Root); ++g_LandedGeneration; }
    }
    RefreshBodies(world, assets);
}

namespace {
// Whether `root`'s body parts aren't what its wardrobe builds for what it wears now: a scene or prefab saved before the
// wardrobe's body changed (the arms split into arms and hands) still has the old parts.
bool BodyStale(World& world, AssetLibrary& assets, entt::entity root) {
    const auto& reg = world.Registry;
    const auto pieces = Pieces(world, root);
    if (pieces.empty()) return false; // not built yet (or by hand): nothing to bring up to date
    Result r;
    const auto cat = CatalogFor(world, assets, root, r);
    if (!cat) return false;
    std::map<std::string, std::string> want, have;
    for (const auto& p : Wardrobe::Resolve(cat->W, cat->Items, CurrentRequest(world, root)).Pieces)
        if (p.BodyPart) want[p.Slot] = Lower(Rel(p.Path));
    for (const auto& [slot, e] : pieces)
        if (const auto& p = reg.get<OutfitPieceComponent>(e); p.Flags & OutfitPieceBodyPart) {
            const auto* rc = reg.try_get<RenderableComponent>(e);
            have[slot] = rc && rc->ModelRef ? Lower(Rel(rc->ModelRef->Path())) : std::string();
        }
    return want != have;
}
} // namespace

void RefreshBodies(World& world, AssetLibrary& assets) {
    auto& reg = world.Registry;
    std::vector<entt::entity> stale;
    for (auto [root, outfit] : reg.view<CharacterOutfitComponent>().each()) {
        if (outfit.BodyChecked == WardrobeGeneration()) continue;
        if (IsPending(world, root)) continue; // a change on its way builds the body anyway
        outfit.BodyChecked = WardrobeGeneration();
        if (BodyStale(world, assets, root)) stale.push_back(root);
    }
    for (entt::entity root : stale) {
        Log::Info("Character Outfit: rebuilding an outfit whose body parts predate its wardrobe.");
        Submit(world, assets, root, CurrentRequest(world, root));
        ++g_LandedGeneration; // when everything is loaded already, Submit applies it on the spot
    }
}

bool RefreshBody(World& world, AssetLibrary& assets, entt::entity root) {
    auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(root);
    if (!outfit) return false;
    outfit->BodyChecked = WardrobeGeneration();
    if (!BodyStale(world, assets, root)) return false;
    const Result r = Apply(world, assets, root, CurrentRequest(world, root));
    if (!r.Ok) Log::Warn("Character Outfit: " + r.Error);
    return r.Ok;
}

namespace detail {
// What the next change builds on: the change still waiting to load, if any, else what's worn now - so
// clicking Female then a hat before the body has loaded keeps both.
Wardrobe::Request BaseRequest(const World& world, entt::entity root) {
    for (const auto& pc : Waiting())
        if (pc.W == &world && pc.Root == root && pc.HasRequest) return pc.Request;
    return CurrentRequest(world, root);
}
} // namespace detail

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

} // namespace OutfitSystem
