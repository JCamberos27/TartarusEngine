// Skin hiding: which body triangles an outfit's pieces cover (worked out off the main thread, cached on disk by the two
// models' files), the visible-triangle buffers UpdateHiding hands the renderer, and UpdateAttachments. (OutfitSystem.h)
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
        if (const auto* h = reg.try_get<HierarchyComponent>(e); h && reg.valid(h->Parent))
            xf = world.ComposeWorldTransform(h->Parent) * FitMatrix(*rc->ModelRef, p->Fit);
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
struct CoverageRead; // PrefetchCoverage's reads, one per pair
struct CoverageStore {
    std::map<CoverageKey, std::vector<std::uint8_t>> Done;
    std::map<CoverageKey, std::future<std::vector<std::uint8_t>>> Running;
    // PrefetchCoverage's batches still reading (or read, not yet taken), and the pairs they hold.
    std::vector<std::pair<std::shared_ptr<std::vector<CoverageRead>>, std::shared_future<void>>> Reading;
    std::set<CoverageKey> Queued;
};
CoverageStore& Coverage() {
    static CoverageStore s;
    return s;
}

constexpr std::uint32_t kCoverageVersion = 17; // bump when Covered/Erode or their settings change

struct FileStamp {
    std::uint64_t Size = 0;
    std::int64_t Time = 0;
};
// The model's and its .meta's: an import setting that changes the vertices (Simplify) makes the saved coverage stale too.
FileStamp StampOf(const std::string& rel) {
    FileStamp s;
    std::error_code ec;
    const fs::path p = ProjectPaths::Resolve(rel);
    s.Size = (std::uint64_t)fs::file_size(p, ec);
    if (ec) s.Size = 0;
    const auto t = fs::last_write_time(p, ec);
    if (!ec) s.Time = (std::int64_t)t.time_since_epoch().count();
    fs::path meta = p;
    meta += ".meta";
    const auto mt = fs::last_write_time(meta, ec);
    if (!ec) s.Time ^= (std::int64_t)mt.time_since_epoch().count() * 31;
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

struct CoverageRead {
    CoverageKey Key;
    std::string UnderModel, OverModel; // project-relative, lower case (StampOf)
    std::vector<std::uint8_t> Data;
    bool Hit = false;
};

// The thresholds are in metres, so a character scaled up or down has coverage of its own: `root`'s scale as the key
// spells it ("" at 1).
std::string ScaleTag(const World& world, entt::entity root) {
    if (!world.Registry.valid(root)) return {};
    const float s = glm::length(glm::vec3(world.ComposeWorldTransform(root)[1]));
    if (std::abs(s - 1.0f) <= 0.005f) return {};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "|x%.2f", s);
    return buf;
}

// A fitted piece (ItemOverride::Fit) covers differently: its fit is part of the key.
std::string FitTag(float fit) {
    if (fit <= 0.0f || std::abs(fit - 1.0f) < 1e-4f) return {};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "|fit%.3f", fit);
    return buf;
}

// `underModel` / `overModel`: project-relative, lower case.
CoverageKey MakeKey(const std::string& underModel, bool exposed, bool rigid, const std::string& scale, float underFit,
                    const std::string& overModel, float overFit) {
    return {underModel + (exposed ? "|exposed" : "") + (rigid ? "|rigid" : "") + scale + FitTag(underFit),
            overModel + FitTag(overFit)};
}

// How a piece worn with these flags is hidden: `exposed` = it's the head; `rigid` = headwear on the head bone.
bool ExposedPiece(int flags) { return (flags & OutfitPieceBodyPart) && (flags & OutfitPieceHeadAttached); }
bool RigidPiece(int flags) { return !(flags & OutfitPieceBodyPart) && (flags & OutfitPieceHeadAttached); }

// PrefetchCoverage's finished batches into Done (a pair it didn't find is left for PairCoverage to work out).
void TakeReads(CoverageStore& store) {
    for (auto it = store.Reading.begin(); it != store.Reading.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        for (CoverageRead& r : *it->first) {
            if (r.Hit) store.Done.emplace(r.Key, std::move(r.Data));
            store.Queued.erase(r.Key);
        }
        it = store.Reading.erase(it);
    }
}

// The pair's covered vertices if they're known (memory, then disk); else starts working them out in the
// background and returns null - ask again next frame.
// `exposed`: `under` is the head; `rigid`: headwear on the head bone (OutfitCoverage::Hidden) - each its own
// entry, keyed apart.
const std::vector<std::uint8_t>* PairCoverage(const World& world, entt::entity under, entt::entity over,
                                              const std::string& underModel, const std::string& overModel, bool exposed,
                                              bool rigid) {
    CoverageStore& store = Coverage();
    const auto* h = world.Registry.try_get<HierarchyComponent>(under);
    const std::string scale = h ? ScaleTag(world, h->Parent) : std::string();
    auto fitOf = [&](entt::entity e) {
        const auto* p = world.Registry.try_get<OutfitPieceComponent>(e);
        return p ? p->Fit : 0.0f;
    };
    const CoverageKey key =
        MakeKey(Lower(Rel(underModel)), exposed, rigid, scale, fitOf(under), Lower(Rel(overModel)), fitOf(over));
    if (auto it = store.Done.find(key); it != store.Done.end()) return &it->second;
    if (auto it = store.Running.find(key); it != store.Running.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        auto done = store.Done.emplace(key, it->second.get()).first;
        store.Running.erase(it);
        return &done->second;
    }
    const FileStamp us = StampOf(Lower(Rel(underModel))), os = StampOf(Lower(Rel(overModel)));
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
    if (!Coverage().Reading.empty()) TakeReads(Coverage());
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
        for (const auto& [slot, e] : pieces) {
            auto& piece = reg.get<OutfitPieceComponent>(e);
            piece.Fit = wardrobe ? Wardrobe::FitOf(*wardrobe, piece.Item) : 1.0f;
        }
        std::map<entt::entity, Wardrobe::Layering> layers;
        for (const auto& [slot, e] : pieces) layers[e] = PieceLayer(wardrobe.get(), reg.get<OutfitPieceComponent>(e));

        // Each piece's rank: the longest run of pieces worn one over the next beneath it (a hat over a
        // balaclava over the head: 2). Drawn that many kLayerPull nearer the camera, it wins the depth test
        // against what it lies on.
        std::map<entt::entity, int> rank;
        std::function<int(entt::entity, const std::string&, int)> rankOf = [&](entt::entity over, const std::string& overSlot, int depth) {
            if (auto it = rank.find(over); it != rank.end()) return it->second;
            int r = 0;
            if (depth < (int)pieces.size())
                for (const auto& [slot, under] : pieces)
                    if (under != over && Wardrobe::Hides(layers[over], slot, layers[under], overSlot))
                        r = std::max(r, 1 + rankOf(under, slot, depth + 1));
            return rank[over] = r;
        };
        for (const auto& [slot, e] : pieces) {
            const float pull = kLayerPull * (float)std::min(rankOf(e, slot, 0), kMaxLayerRank);
            if (pull > 0.0f) reg.emplace_or_replace<OutfitLayerTag>(e).Pull = pull;
            else reg.remove<OutfitLayerTag>(e);
        }

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
                const bool exposed = ExposedPiece(flags), rigid = RigidPiece(flags);
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

std::shared_future<void> detail::PrefetchCoverage(const World& world, entt::entity root,
                                                  const std::vector<Wardrobe::Piece>& pieces) {
    const auto* outfit = world.Registry.try_get<CharacterOutfitComponent>(root);
    if (!outfit || !outfit->AutoHide || pieces.size() < 2) return {};
    // The keys UpdateHiding will ask for once these pieces are worn: layers and fits from the same wardrobe, the
    // root's scale, each piece's flags as Apply sets them.
    const auto wardrobe = LayerWardrobe(outfit->Wardrobe);
    const std::string scale = ScaleTag(world, root);
    struct Worn {
        std::string Slot, Model;
        int Flags = 0;
        float Fit = 1.0f;
        Wardrobe::Layering Layer;
    };
    std::vector<Worn> worn;
    for (const auto& p : pieces) {
        const int flags = (p.BodyPart ? OutfitPieceBodyPart : 0) | (p.HeadAttached ? OutfitPieceHeadAttached : 0);
        OutfitPieceComponent piece;
        piece.Slot = p.Slot;
        piece.Item = p.Path;
        piece.Flags = flags;
        worn.push_back({p.Slot, Lower(Rel(p.Path)), flags, wardrobe ? Wardrobe::FitOf(*wardrobe, p.Path) : 1.0f,
                        PieceLayer(wardrobe.get(), piece)});
    }
    CoverageStore& store = Coverage();
    auto reads = std::make_shared<std::vector<CoverageRead>>();
    for (const Worn& under : worn)
        for (const Worn& over : worn) {
            if (&under == &over || !Wardrobe::Hides(over.Layer, under.Slot, under.Layer, over.Slot)) continue;
            CoverageKey key = MakeKey(under.Model, ExposedPiece(under.Flags), RigidPiece(under.Flags), scale, under.Fit,
                                      over.Model, over.Fit);
            if (store.Done.count(key) || store.Running.count(key) || !store.Queued.insert(key).second) continue;
            CoverageRead r;
            r.Key = std::move(key);
            r.UnderModel = under.Model;
            r.OverModel = over.Model;
            reads->push_back(std::move(r));
        }
    if (reads->empty()) return {};
    std::shared_future<void> done = std::async(std::launch::async, [reads] {
                                        for (CoverageRead& r : *reads)
                                            r.Hit = LoadCoverage(r.Key, StampOf(r.UnderModel), StampOf(r.OverModel), r.Data);
                                    }).share();
    store.Reading.emplace_back(reads, done);
    return done;
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
            t.Rotation = glm::normalize(glm::quat_cast(rot));
            if (p->Fit > 0.0f && std::abs(p->Fit - 1.0f) > 1e-4f) { // bigger about its own centre
                const glm::mat4 fitted = follow * FitMatrix(*rc->ModelRef, p->Fit);
                t.Position = glm::vec3(fitted[3]);
                t.Scale = scale * p->Fit;
            } else {
                t.Position = pos;
                t.Scale = scale;
            }
        }
    }
}

glm::mat4 FitMatrix(const Model& model, float fit) {
    if (fit <= 0.0f || std::abs(fit - 1.0f) < 1e-4f) return glm::mat4(1.0f);
    const glm::vec3 c = (model.BoundsMin() + model.BoundsMax()) * 0.5f;
    return glm::translate(glm::mat4(1.0f), c) * glm::scale(glm::mat4(1.0f), glm::vec3(fit)) * glm::translate(glm::mat4(1.0f), -c);
}

} // namespace OutfitSystem
