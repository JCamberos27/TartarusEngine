#include "World.h"
#include <cstring>
#include <atomic>
#include <execution>
#include <numeric>
#include "Model.h"
#include "ImpostorLibrary.h"
#include "Log.h"
#include "TimeService.h"
#include <string>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cstdint>        // #202 hierarchy cycle repair
#include <unordered_map>
#include <vector>
#include <limits>
#include <glm/gtc/quaternion.hpp>


// A default-constructed World is genuinely empty — no ground, no placeholder crates. Every
// caller that wants a truly blank scene (startup with no scene file, File > New Scene) goes
// through this constructor, so anything spawned here would show up whether they wanted it or
// not. Add starter content, if ever wanted again, as an explicit opt-in action instead (e.g. a
// "New Scene from template" menu item), not as constructor side effects.
World::World() {}

std::string World::NextPrimitivePath() {
    return "primitive://levelgeometry/" + std::to_string(m_NextPrimitiveId++);
}

entt::entity World::CreateBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& color,
    const glm::vec3& rotationEuler, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, center, rotationEuler, size);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());

    auto model = Model::CreatePrimitive("cube", NextPrimitivePath());
    model->MeshMaterial(0).BaseColor = color;
    Registry.emplace<RenderableComponent>(e, std::move(model));

    Registry.emplace<ColliderComponent>(e);
    Registry.emplace<LevelGeometryTag>(e);
    return e;
}

entt::entity World::CreateModelEntity(std::shared_ptr<Model> modelRef, const glm::vec3& position,
    const glm::vec3& rotationEuler, const glm::vec3& scale, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, position, rotationEuler, scale);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());
    Registry.emplace<RenderableComponent>(e, std::move(modelRef));
    return e;
}

entt::entity World::CreateEmptyEntity(const glm::vec3& position, const glm::vec3& rotationEuler,
    const glm::vec3& scale, const std::string& name) {
    entt::entity e = Registry.create();
    Registry.emplace<TransformComponent>(e, position, rotationEuler, scale);
    Registry.emplace<NameComponent>(e, name);
    Registry.emplace<OrderComponent>(e, AllocateOrder());
    return e;
}

glm::mat4 World::ComposeWorldTransform(entt::entity entity) const {
    if (!Registry.valid(entity)) return glm::mat4(1.0f);

    glm::mat4 world = ComposeTransform(Registry.get<TransformComponent>(entity));

    // Walk up the parent chain iteratively instead of recursing. AttachChildRaw (used by scene
    // loading) doesn't reject parentId cycles, so a malformed file used to recurse straight
    // through here into a stack overflow with no diagnostic. Cap the walk exactly as
    // RebuildWorldTransformCache does for the same reason, and log once if it trips — this is
    // called every frame for whatever's selected, so without the m_CycleWarned guard a cyclic
    // entity floods the Console at full frame rate instead of reporting once (audit #74).
    entt::entity walk = entity;
    for (size_t depth = 0; ; ++depth) {
        const auto* hier = Registry.try_get<HierarchyComponent>(walk);
        if (!hier || hier->Parent == entt::null || !Registry.valid(hier->Parent)) break;
        if (depth >= kMaxHierarchyDepth) {
            if (m_CycleWarned.insert(entity).second) {
                Log::Error("World::ComposeWorldTransform: parent chain exceeds " +
                           std::to_string(kMaxHierarchyDepth) +
                           " levels (cycle in parentId?); world transform truncated.");
            }
            break;
        }
        walk = hier->Parent;
        if (const auto* t = Registry.try_get<TransformComponent>(walk))
            world = ComposeTransform(*t) * world;
    }
    return world;
}

void World::SyncActiveInHierarchy() {
    std::vector<entt::entity> activate, deactivate;
    auto consider = [&](entt::entity e, bool inactive) {
        const bool has = Registry.all_of<InactiveTag>(e);
        if (inactive && !has) deactivate.push_back(e);
        else if (!inactive && has) activate.push_back(e);
    };
    // Each entity's answer is kept (by slot) as the walks reach it: a parent chain is walked once per frame, not once per
    // descendant (every frame, ~2,400 entities five levels deep in the apartment floor).
    static std::vector<std::uint8_t> known; // 0 = not yet, 1 = active, 2 = inactive
    static std::vector<entt::entity> chain;
    size_t slots = 0;
    for (entt::entity e : Registry.view<TransformComponent>()) slots = std::max(slots, (size_t)entt::to_entity(e) + 1);
    known.assign(slots, 0);
    auto slotOf = [&](entt::entity e) { const size_t s = (size_t)entt::to_entity(e); return s < known.size() ? s : (size_t)-1; };
    for (entt::entity e : Registry.view<TransformComponent>()) {
        bool inactive = false;
        chain.clear();
        int depth = 0;
        for (entt::entity walk = e; walk != entt::null && Registry.valid(walk) && depth <= kMaxHierarchyDepth; ++depth) {
            if (const size_t s = slotOf(walk); s != (size_t)-1 && known[s] && Registry.all_of<TransformComponent>(walk)) {
                inactive = known[s] == 2;
                break;
            }
            if (Registry.all_of<DeactivatedTag>(walk)) { inactive = true; break; }
            chain.push_back(walk);
            const auto* h = Registry.try_get<HierarchyComponent>(walk);
            walk = h ? h->Parent : entt::null;
        }
        for (entt::entity c : chain)
            if (const size_t s = slotOf(c); s != (size_t)-1 && Registry.all_of<TransformComponent>(c)) known[s] = inactive ? 2 : 1;
        consider(e, inactive);
    }
    // Entities without a transform have no hierarchy: only their own checkbox counts.
    for (entt::entity e : Registry.view<InactiveTag>(entt::exclude<TransformComponent>))
        consider(e, Registry.all_of<DeactivatedTag>(e));
    for (entt::entity e : Registry.view<DeactivatedTag>(entt::exclude<TransformComponent>))
        consider(e, true);
    for (entt::entity e : deactivate) Registry.emplace_or_replace<InactiveTag>(e);
    for (entt::entity e : activate) Registry.remove<InactiveTag>(e);
}

namespace {
// Order-independent fingerprint of a storage's entities (size, and a sum of mixed ids): SyncActiveInHierarchy's inputs.
std::uint64_t StorageFingerprint(const entt::sparse_set& st) {
    std::uint64_t h = 0;
    const auto* data = st.data();
    for (size_t i = 0, n = st.size(); i < n; ++i) {
        std::uint64_t v = (std::uint64_t)entt::to_integral(data[i]) * 0x9E3779B97F4A7C15ull;
        v ^= v >> 31;
        h += v * 0xBF58476D1CE4E5B9ull;
    }
    return h;
}
} // namespace

void World::ActiveSignature(std::uint64_t out[6]) {
    const entt::sparse_set& deactivated = Registry.storage<DeactivatedTag>();
    const entt::sparse_set& inactive = Registry.storage<InactiveTag>();
    out[0] = m_HierarchyVersion;
    out[1] = Registry.storage<TransformComponent>().size();
    out[2] = deactivated.size();
    out[3] = StorageFingerprint(deactivated);
    out[4] = inactive.size();
    out[5] = StorageFingerprint(inactive);
}

namespace {
// A transform's local position, rotation and scale folded into 64 bits (never 0, which marks "no transform"): the
// per-frame scan for moved entities compares these 8 bytes instead of a 48-byte snapshot, a forest's 200k
// transforms being a memory-bandwidth problem more than an arithmetic one.
std::uint64_t LocalHash(const TransformComponent& t) {
    std::uint32_t w[10];
    std::memcpy(w, &t.Position, 12);
    std::memcpy(w + 3, &t.Rotation, 16);
    std::memcpy(w + 7, &t.Scale, 12);
    // Five independent products summed (each pair of words times its own odd constant), not a chain where each step waits
    // on the last: the scan is ALU-latency bound otherwise (~40 cycles a transform, ~1 ms for the forest). A change in
    // any one word changes the sum (odd multipliers are invertible mod 2^64).
    auto pair = [&](int i) { return (std::uint64_t)w[i] | ((std::uint64_t)w[i + 1] << 32); };
    std::uint64_t h = pair(0) * 0xBF58476D1CE4E5B9ull + pair(2) * 0x94D049BB133111EBull + pair(4) * 0x9E3779B97F4A7C15ull +
                      pair(6) * 0xD6E8FEB86659FD93ull + pair(8) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    return h | 1u;
}
} // namespace

void World::RebuildWorldTransformCache() {
    static uint64_t s_Stamps = 0; // shared by every World, so two worlds never hand out the same stamp
    m_PrevTransformCacheStamp = m_TransformCacheStamp;
    m_TransformCacheStamp = ++s_Stamps;
    // Active-in-hierarchy only changes with the hierarchy, the Deactivated set or someone else's InactiveTag edits (all
    // in the signature): re-derived only then. A destroy-and-create of an unparented entity leaves it out, which is fine
    // - a fresh root without DeactivatedTag is active, which is what it starts as.
    {
        std::uint64_t sig[6];
        ActiveSignature(sig);
        if (!m_ActiveSignatureValid || !std::equal(sig, sig + 6, m_ActiveSignature)) {
            SyncActiveInHierarchy();
            ActiveSignature(m_ActiveSignature); // as Sync left it
            m_ActiveSignatureValid = true;
        }
    }
    auto view = Registry.view<TransformComponent>();
    m_ChangedTransforms.clear();
    m_TransformMembershipChanged = false;
    m_StructureDelta.Added.clear();
    m_StructureDelta.Reparented.clear();
    m_StructureDelta.Dropped.clear();
    m_StructureDelta.Valid = true;
    if (RebuildChangedTransformsOnly()) return;
    // The full walk keeps no lists (and the incremental pass may have started some before handing over).
    m_StructureDelta.Added.clear();
    m_StructureDelta.Reparented.clear();
    m_StructureDelta.Dropped.clear();
    m_StructureDelta.Valid = false;
    ++m_TransformFullWalks;
    size_t slots = 0;
    for (entt::entity entity : view) slots = std::max(slots, (size_t)entt::to_entity(entity) + 1);
    auto ensureSlots = [&](size_t n) { EnsureTransformSlots(n); };
    ensureSlots(slots);
    const std::uint32_t gen = ++m_RebuildGeneration;
    auto doneNow = [&](entt::entity e) {
        const size_t s = (size_t)entt::to_entity(e);
        return s < m_RebuildDone.size() && m_RebuildDone[s] == gen && m_WorldTransformKeys[s] == e;
    };

    // Chain scratch (entity + its parent), reused across entities so a deep hierarchy doesn't allocate per entity.
    std::vector<std::pair<entt::entity, entt::entity>> chain;
    for (entt::entity entity : view) {
        if (doneNow(entity)) continue; // already filled as some ancestor

        // Walk UP collecting the part of the parent chain this rebuild hasn't reached, then go DOWN from the topmost
        // one. Every entity is therefore visited exactly once per rebuild, no matter how many of its descendants the
        // view visits afterwards.
        chain.clear();
        glm::mat4 base(1.0f);
        bool baseChanged = false;
        for (entt::entity walk = entity; walk != entt::null; ) {
            if (doneNow(walk)) {
                const size_t s = (size_t)entt::to_entity(walk);
                base = m_WorldTransformCache[s];
                baseChanged = m_RebuildChanged[s] != 0;
                break;
            }
            const auto* h = Registry.try_get<HierarchyComponent>(walk);
            entt::entity next = h ? h->Parent : entt::null;
            if (next != entt::null && !Registry.valid(next)) next = entt::null;
            chain.emplace_back(walk, next);
            if (next == entt::null) break;
            // SetParent rejects cycles, but AttachChildRaw (used by scene loading) doesn't
            // validate — bail rather than spin forever on a malformed file. Previously silent;
            // now reported once per offending entity through the same guard
            // ComposeWorldTransform uses, so a cycle is never invisible just because this path
            // happened to touch it first (audit #74).
            if (chain.size() > kMaxHierarchyDepth) {
                if (m_CycleWarned.insert(entity).second) {
                    Log::Error("World::RebuildWorldTransformCache: parent chain exceeds " +
                               std::to_string(kMaxHierarchyDepth) +
                               " levels (cycle in parentId?); world transform truncated.");
                }
                chain.back().second = entt::null; // composed as a root, like the old truncation
                break;
            }
            walk = next;
        }

        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const entt::entity e = it->first;
            const size_t slot = (size_t)entt::to_entity(e);
            ensureSlots(slot + 1); // a Transform-less ancestor past the view's highest slot
            // An ancestor with no TransformComponent contributes identity rather than throwing:
            // every entity the editor can create has one, but the cache must not be the thing
            // that crashes on a hand-edited or future component layout.
            const auto* t = Registry.try_get<TransformComponent>(e);
            LocalSnapshot& snap = m_LocalSnapshots[slot];
            const bool member = m_WorldTransformKeys[slot] == e;
            const bool same = member && snap.Parent == it->second && snap.HasTransform == (t != nullptr) &&
                              (!t || (snap.Position == t->Position && snap.Rotation == t->Rotation && snap.Scale == t->Scale));
            const bool changed = baseChanged || !same;
            if (changed) {
                if (t) base = base * ComposeTransform(*t);
                m_WorldTransformCache[slot] = base;
                snap.Parent = it->second;
                snap.HasTransform = t != nullptr;
                m_LocalHashes[slot] = t ? LocalHash(*t) : 0;
                if (t) {
                    snap.Position = t->Position;
                    snap.Rotation = t->Rotation;
                    snap.Scale = t->Scale;
                }
                m_ChangedTransforms.push_back(e);
            } else {
                base = m_WorldTransformCache[slot];
            }
            if (!member) m_TransformMembershipChanged = true;
            m_WorldTransformKeys[slot] = e;
            m_RebuildDone[slot] = gen;
            m_RebuildChanged[slot] = changed ? 1 : 0;
            baseChanged = changed;
        }
    }
    // Whatever this rebuild didn't reach (destroyed, or lost its transform) leaves the cache.
    for (size_t s = 0; s < m_WorldTransformKeys.size(); ++s) {
        if (m_WorldTransformKeys[s] != entt::null && m_RebuildDone[s] != gen) {
            m_WorldTransformKeys[s] = entt::null;
            m_TransformMembershipChanged = true;
        }
    }
    CountKeyedWithTransform();
    TransformStructure(m_TransformStructure);
    m_TransformStructureValid = true;
}

void World::TransformStructure(std::uint64_t out[3]) {
    out[0] = m_HierarchyVersion;
    out[1] = Registry.storage<HierarchyComponent>().size();
    out[2] = Registry.storage<TransformComponent>().size();
}

// A structure change for the incremental pass: `dirty` holds the scan's finds (moved, new to the cache, re-parented).
// Checks first, then changes, so false (nothing changed) sends the frame to the full walk: a mostly new set (a scene
// load), a parent chain that loops, or a Transform-less ancestor new to the cache. On success the cache has dropped
// what was destroyed, keyed what is new, re-read the parents, and `dirty` also holds what lost its transform or sits
// under a re-parented Transform-less ancestor; the caller recomposes those subtrees.
bool World::ApplyTransformStructureChanges(std::vector<entt::entity>& dirty) {
    // The pools themselves: a registry lookup per call (all_of) cost ~1-2 ms over the forest's cache slots.
    const auto& hierarchy = Registry.storage<HierarchyComponent>();
    const auto& transforms = Registry.storage<TransformComponent>();
    const auto parentOf = [&](entt::entity e) {
        if (!hierarchy.contains(e)) return entt::entity{entt::null};
        const entt::entity p = hierarchy.get(e).Parent;
        return p != entt::null && Registry.valid(p) ? p : entt::entity{entt::null};
    };
    size_t fresh = 0;
    for (entt::entity e : dirty) {
        const bool cached = IsWorldTransformCached(e);
        fresh += cached ? 0 : 1;
        if (cached && parentOf(e) == m_LocalSnapshots[(size_t)entt::to_entity(e)].Parent) continue; // only moved
        int depth = 0;
        for (entt::entity p = parentOf(e); p != entt::null; p = parentOf(p)) {
            if (++depth > kMaxHierarchyDepth) return false;
            if (!IsWorldTransformCached(p) && !transforms.contains(p)) return false;
        }
    }
    if (fresh * 2 > transforms.size()) return false;

    // What left: destroyed (the slot is dropped) or lost its transform (kept as a Transform-less ancestor, like the
    // full walk keeps one, and recomposed with its subtree). A Transform-less ancestor isn't in the scan: its own
    // re-parenting is found here.
    bool dropped = false;
    for (size_t s = 0; s < m_WorldTransformKeys.size(); ++s) {
        const entt::entity k = m_WorldTransformKeys[s];
        if (k == entt::null) continue;
        if (!Registry.valid(k)) {
            m_StructureDelta.Dropped.emplace_back(k, m_LocalSnapshots[s].Parent);
            m_WorldTransformKeys[s] = entt::null;
            m_TransformMembershipChanged = true;
            dropped = true;
            continue;
        }
        LocalSnapshot& snap = m_LocalSnapshots[s];
        if (snap.HasTransform && !transforms.contains(k)) {
            snap.HasTransform = false;
            m_LocalHashes[s] = 0;
            dirty.push_back(k);
        } else if (!snap.HasTransform && parentOf(k) != snap.Parent) {
            dirty.push_back(k);
        }
    }
    // A destroyed parent orphans its children without touching their hierarchy entries. The scan only re-checks
    // parents when the structure signature moved, and a destroy balanced by a create in the same frame (registry
    // destroy + a new transform entity) leaves it unchanged: re-check every cached slot's parent here instead, so the
    // orphans are read as roots like the full walk reads them.
    if (dropped) {
        for (size_t s = 0; s < m_WorldTransformKeys.size(); ++s) {
            const entt::entity k = m_WorldTransformKeys[s];
            if (k != entt::null && m_LocalSnapshots[s].HasTransform && parentOf(k) != m_LocalSnapshots[s].Parent)
                dirty.push_back(k);
        }
    }
    for (entt::entity e : dirty) {
        const size_t s = (size_t)entt::to_entity(e);
        EnsureTransformSlots(s + 1);
        const entt::entity parent = parentOf(e);
        if (m_WorldTransformKeys[s] != e) {
            m_WorldTransformKeys[s] = e;
            m_LocalSnapshots[s].HasTransform = true; // new to the cache: found in the transform pool
            m_LocalHashes[s] = 0;
            m_TransformMembershipChanged = true;
            m_StructureDelta.Added.push_back(e);
        } else if (m_LocalSnapshots[s].Parent != parent) {
            m_StructureDelta.Reparented.emplace_back(e, m_LocalSnapshots[s].Parent); // listed once: the parent is now read
        }
        m_LocalSnapshots[s].Parent = parent;
        if (!m_LocalSnapshots[s].HasTransform && transforms.contains(e))
            m_LocalSnapshots[s].HasTransform = true; // a Transform-less ancestor given one again
    }
    CountKeyedWithTransform();
    TransformStructure(m_TransformStructure);
    return true;
}

void World::CountKeyedWithTransform() {
    m_KeyedWithTransform = 0;
    for (size_t s = 0; s < m_WorldTransformKeys.size(); ++s)
        m_KeyedWithTransform += m_WorldTransformKeys[s] != entt::null && m_LocalSnapshots[s].HasTransform ? 1 : 0;
}

void World::EnsureTransformSlots(size_t n) {
    if (n <= m_WorldTransformKeys.size()) return;
    m_WorldTransformKeys.resize(n, entt::null);
    m_WorldTransformCache.resize(n);
    m_LocalSnapshots.resize(n);
    m_LocalHashes.resize(n, 0);
    m_RebuildDone.resize(n, 0);
    m_RebuildChanged.resize(n, 0);
}

// The common frame: the hierarchy is as the last rebuild left it and the same entities have transforms, so each
// slot's recorded parent still holds. One linear pass over the transforms finds the ones whose local values moved;
// only those and their descendants are recomposed (parents first), everything else keeps its matrix.
// A frame whose structure changed by a few entities (an outfit piece landing: created, attached, a piece destroyed)
// takes the same pass, which then also finds the entities new to the cache and the ones whose parent changed; the
// cache drops what left it and recomposes only those subtrees, instead of walking every entity (2-5 ms on the forest,
// on each of the landing frames). Returns false, having changed nothing, when the full walk is needed instead.
bool World::RebuildChangedTransformsOnly() {
    if (!m_TransformStructureValid) return false;
    std::uint64_t structure[3];
    TransformStructure(structure);
    // Parents can only have changed when the hierarchy did (every re-parent goes through World and bumps its version).
    const bool reparents = !std::equal(structure, structure + 3, m_TransformStructure);
    std::vector<entt::entity>& dirty = m_DirtyScratch;
    dirty.clear();
    // The parent the full walk would use: none, or a destroyed parent's, read as a root.
    const auto& hierarchy = Registry.storage<HierarchyComponent>();
    const auto parentOf = [&](entt::entity e) {
        if (!hierarchy.contains(e)) return entt::entity{entt::null};
        const entt::entity p = hierarchy.get(e).Parent;
        return p != entt::null && Registry.valid(p) ? p : entt::entity{entt::null};
    };
    // The scan is split across the cores in fixed chunks of the transform pool (read only; each chunk keeps its own
    // list): one thread took ~1.7 ms over the forest's 200k transforms, every frame. Chunk order keeps the pool's order.
    const auto& pool = Registry.storage<TransformComponent>();
    const size_t count = pool.size();
    const auto comps = pool.cbegin();
    const auto ents = static_cast<const entt::sparse_set&>(pool).begin();
    constexpr size_t kChunk = 8192;
    const size_t chunks = (count + kChunk - 1) / kChunk;
    static std::vector<std::vector<entt::entity>> s_ChunkDirty;
    static std::vector<size_t> s_ChunkIds;
    if (s_ChunkDirty.size() < chunks) s_ChunkDirty.resize(chunks);
    s_ChunkIds.resize(chunks);
    std::iota(s_ChunkIds.begin(), s_ChunkIds.end(), size_t(0));
    static std::vector<size_t> s_ChunkKept; // per chunk: cached entities that still hold their transform
    s_ChunkKept.assign(chunks, 0);
    std::atomic<bool> newToCache{false};
    auto scan = [&](size_t c) {
        std::vector<entt::entity>& out = s_ChunkDirty[c];
        out.clear();
        size_t kept = 0;
        const size_t end = std::min(count, (c + 1) * kChunk);
        for (size_t i = c * kChunk; i < end; ++i) {
            const entt::entity e = ents[(std::ptrdiff_t)i];
            const size_t s = (size_t)entt::to_entity(e);
            if (s >= m_WorldTransformKeys.size() || m_WorldTransformKeys[s] != e) {
                newToCache = true;
                out.push_back(e);
                continue;
            }
            kept += m_LocalSnapshots[s].HasTransform ? 1 : 0;
            if (m_LocalHashes[s] != LocalHash(comps[(std::ptrdiff_t)i])) out.push_back(e); // 0 (no transform at the snapshot) never matches
            else if (reparents && parentOf(e) != m_LocalSnapshots[s].Parent) out.push_back(e); // re-parented
        }
        s_ChunkKept[c] = kept;
    };
    if (chunks > 1) std::for_each(std::execution::par, s_ChunkIds.begin(), s_ChunkIds.end(), scan);
    else if (chunks == 1) scan(0);
    // Something entered or left the cache, even under unchanged counts (one transform removed and another added).
    const bool structural = reparents || newToCache ||
                            std::accumulate(s_ChunkKept.begin(), s_ChunkKept.end(), size_t(0)) != m_KeyedWithTransform;
    for (size_t c = 0; c < chunks; ++c) dirty.insert(dirty.end(), s_ChunkDirty[c].begin(), s_ChunkDirty[c].end());
    if (structural && !ApplyTransformStructureChanges(dirty)) return false;
    if (dirty.empty()) return true;

    const std::uint32_t gen = ++m_RebuildGeneration;
    // Everything under a moved entity is recomposed too: mark the subtrees (m_RebuildChanged doubles as "pending").
    std::vector<entt::entity>& pending = m_PendingScratch;
    pending.clear();
    std::vector<entt::entity>& stack = m_LodStack;
    for (entt::entity d : dirty) {
        stack.assign(1, d);
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            const size_t s = (size_t)entt::to_entity(e);
            if (s >= m_WorldTransformKeys.size() || m_WorldTransformKeys[s] != e) continue;
            if (m_RebuildDone[s] == gen) continue; // already marked
            m_RebuildDone[s] = gen;
            m_RebuildChanged[s] = 1; // not composed yet
            pending.push_back(e);
            if (const auto* h = Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
    }
    std::vector<entt::entity>& chain = m_ChainScratch;
    auto isPending = [&](entt::entity e) {
        const size_t s = (size_t)entt::to_entity(e);
        return m_RebuildDone[s] == gen && m_RebuildChanged[s] != 0;
    };
    for (entt::entity e : pending) {
        if (!isPending(e)) continue; // composed already, as some descendant's ancestor
        chain.clear();
        entt::entity walk = e;
        while (walk != entt::null && isPending(walk)) {
            chain.push_back(walk);
            walk = m_LocalSnapshots[(size_t)entt::to_entity(walk)].Parent;
            if (chain.size() > kMaxHierarchyDepth) return false; // a parent cycle: the full walk truncates and logs it
        }
        glm::mat4 base = walk != entt::null ? m_WorldTransformCache[(size_t)entt::to_entity(walk)] : glm::mat4(1.0f);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const size_t s = (size_t)entt::to_entity(*it);
            LocalSnapshot& snap = m_LocalSnapshots[s];
            if (const auto* t = Registry.try_get<TransformComponent>(*it)) {
                base = base * ComposeTransform(*t);
                snap.Position = t->Position;
                snap.Rotation = t->Rotation;
                snap.Scale = t->Scale;
                m_LocalHashes[s] = LocalHash(*t);
            }
            m_WorldTransformCache[s] = base;
            m_RebuildChanged[s] = 0;
            m_ChangedTransforms.push_back(*it);
        }
    }
    return true;
}

TransformComponent World::WorldSpaceTransform(entt::entity entity) const {
    const auto* local = Registry.try_get<TransformComponent>(entity);
    if (!local) return TransformComponent{};
    const auto* hier = Registry.try_get<HierarchyComponent>(entity);
    if (!hier || hier->Parent == entt::null) return *local;
    const glm::mat4 m = ComposeWorldTransform(entity);
    TransformComponent out = *local;
    out.Position = glm::vec3(m[3]);
    out.Scale = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    out.SetRotationQuaternion(QuaternionFromMatrix(m));
    return out;
}

void World::SetWorldPose(entt::entity entity, const glm::vec3& worldPosition, const glm::quat& worldRotation) {
    auto* tc = Registry.try_get<TransformComponent>(entity);
    if (!tc) return;
    const auto* hier = Registry.try_get<HierarchyComponent>(entity);
    if (!hier || hier->Parent == entt::null) {
        tc->Position = worldPosition;
        tc->SetRotationQuaternion(worldRotation);
        return;
    }
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), worldPosition) * glm::mat4_cast(worldRotation);
    const glm::mat4 local = glm::inverse(ComposeWorldTransform(hier->Parent)) * world;
    tc->Position = glm::vec3(local[3]);
    tc->SetRotationQuaternion(QuaternionFromMatrix(local));
}

glm::mat4 World::GetCachedWorldTransform(entt::entity entity) const {
    if (IsWorldTransformCached(entity)) return m_WorldTransformCache[(size_t)entt::to_entity(entity)];
    return ComposeWorldTransform(entity); // spawned since the rebuild, or none has run yet
}

bool World::SetParent(entt::entity child, entt::entity parent) {
    if (!Registry.valid(child) || child == parent) return false;
    if (parent != entt::null && !Registry.valid(parent)) return false;
    // (#114/#114-W1: entities with a ColliderComponent used to be refused here, because physics,
    // the collider gizmo and the editor raycast read TransformComponent as world space. They
    // now work in world space (World::WorldSpaceTransform / SetWorldPose), so colliders parent
    // like anything else.)

    // Reject if `parent` is `child` or one of its own descendants — that would create a cycle.
    // #188 — bounded like the render / cache walks: a scene file that already contains a cycle
    // not involving `child` (A -> B -> A) used to spin here forever and hang the editor.
    int hops = 0;
    for (entt::entity walk = parent; walk != entt::null; ) {
        if (walk == child) return false;
        if (++hops > 1024) {
            Log::Error("Hierarchy: the parent chain above " + EntityLogRef(Registry, parent) +
                       " loops or is deeper than 1024 - reparent refused (the scene's hierarchy is malformed).", EntityLogContext(Registry, parent));
            return false;
        }
        const auto* h = Registry.try_get<HierarchyComponent>(walk);
        walk = h ? h->Parent : entt::null;
    }

    glm::mat4 worldBefore = ComposeWorldTransform(child);

    if (auto* childHier = Registry.try_get<HierarchyComponent>(child)) {
        if (childHier->Parent != entt::null) {
            if (auto* oldParentHier = Registry.try_get<HierarchyComponent>(childHier->Parent)) {
                auto& kids = oldParentHier->Children;
                kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
            }
        }
    }

    ++m_HierarchyVersion;
    auto& childHier = Registry.get_or_emplace<HierarchyComponent>(child);
    childHier.Parent = parent;
    if (parent != entt::null) {
        Registry.get_or_emplace<HierarchyComponent>(parent).Children.push_back(child);
    }

    // Re-express TransformComponent in the new frame (local if parented, world if detached) so
    // the entity doesn't visually jump when the parent link changes.
    glm::mat4 newParentWorld = parent != entt::null ? ComposeWorldTransform(parent) : glm::mat4(1.0f);
    glm::mat4 newLocal = glm::inverse(newParentWorld) * worldBefore;

    glm::vec3 pos, scale, skew; glm::vec4 persp; glm::quat rot;
    glm::decompose(newLocal, scale, rot, pos, skew, persp);

    auto& t = Registry.get<TransformComponent>(child);
    t.Position = pos;
    t.Scale = scale;
    t.SetRotationQuaternion(rot);
    return true;
}

void World::AttachChildRaw(entt::entity child, entt::entity parent) {
    if (!Registry.valid(child) || !Registry.valid(parent) || child == parent) return;
    ++m_HierarchyVersion;
    auto& childHier = Registry.get_or_emplace<HierarchyComponent>(child);
    childHier.Parent = parent;
    Registry.get_or_emplace<HierarchyComponent>(parent).Children.push_back(child);
}

// #202 - see the header. Walks every entity to its root, marking the chain as it goes. A node
// reached twice within one walk closes a cycle, so the entity that closes it is detached and
// becomes a root. Entities already proven to reach a root are memoised, so this is linear in the
// hierarchy rather than quadratic on a deep one.
int World::RepairHierarchyCycles() {
    enum class State : std::uint8_t { Unknown, InProgress, Rooted };
    std::unordered_map<std::uint32_t, State> state;
    std::vector<entt::entity> chain;
    int broken = 0;

    for (entt::entity start : Registry.view<HierarchyComponent>()) {
        chain.clear();
        entt::entity walk = start;
        while (walk != entt::null && Registry.valid(walk)) {
            State& st = state[entt::to_integral(walk)];
            if (st == State::Rooted) break;              // this chain already proven acyclic
            if (st == State::InProgress) {
                // `walk` is its own ancestor: cut the link that closed the loop.
                auto& hier = Registry.get<HierarchyComponent>(walk);
                const entt::entity parent = hier.Parent;
                if (auto* parentHier = Registry.try_get<HierarchyComponent>(parent)) {
                    auto& kids = parentHier->Children;
                    kids.erase(std::remove(kids.begin(), kids.end(), walk), kids.end());
                }
                hier.Parent = entt::null;
                ++broken;
                ++m_HierarchyVersion;
                Log::Error("Hierarchy: " + EntityLogRef(Registry, walk) + " was its own ancestor - "
                           "the parent link was cut and it is now a root. The scene file's parent "
                           "ids form a loop (#202).", EntityLogContext(Registry, walk));
                break;
            }
            st = State::InProgress;
            chain.push_back(walk);
            const auto* h = Registry.try_get<HierarchyComponent>(walk);
            walk = h ? h->Parent : entt::null;
        }
        // Everything on this chain reaches a root now (the cycle, if any, was just cut).
        for (entt::entity e : chain) state[entt::to_integral(e)] = State::Rooted;
    }
    return broken;
}

void World::DestroyEntityAndChildren(entt::entity entity) {
    if (!Registry.valid(entity)) return;
    ++m_HierarchyVersion;
    if (auto* hier = Registry.try_get<HierarchyComponent>(entity)) {
        // Copy first — destroying a child mutates its parent's HierarchyComponent (this one),
        // so recursing over the live vector would invalidate the iteration.
        std::vector<entt::entity> children = hier->Children;
        for (entt::entity c : children) DestroyEntityAndChildren(c);
        if (hier->Parent != entt::null) {
            if (auto* parentHier = Registry.try_get<HierarchyComponent>(hier->Parent)) {
                auto& kids = parentHier->Children;
                kids.erase(std::remove(kids.begin(), kids.end(), entity), kids.end());
            }
        }
    }
    Registry.destroy(entity);
}

glm::mat4 ComposeTransform(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m *= glm::mat4_cast(NormalizeRotation(rotation));
    m = glm::scale(m, scale);
    return m;
}

bool World::CompareTag(entt::entity e, const std::string& tag) const {
    if (!Registry.valid(e)) return false;
    const auto* t = Registry.try_get<TagComponent>(e);
    return (t ? t->Tag : std::string("Untagged")) == tag;
}

entt::entity World::FindWithTag(const std::string& tag) const {
    for (auto e : Registry.view<const TagComponent>(entt::exclude<InactiveTag>))
        if (Registry.get<const TagComponent>(e).Tag == tag) return e;
    return entt::null;
}

std::vector<entt::entity> World::FindAllWithTag(const std::string& tag) const {
    std::vector<entt::entity> out;
    for (auto e : Registry.view<const TagComponent>(entt::exclude<InactiveTag>))
        if (Registry.get<const TagComponent>(e).Tag == tag) out.push_back(e);
    return out;
}

LogContext EntityLogContext(const entt::registry& registry, entt::entity e) {
    if (registry.valid(e))
        if (const auto* order = registry.try_get<OrderComponent>(e)) return LogContext::Entity(order->Value);
    return {};
}

std::string EntityLogRef(const entt::registry& registry, entt::entity e) {
    if (registry.valid(e))
        if (const auto* order = registry.try_get<OrderComponent>(e))
            return "entity #" + std::to_string(order->Value);
    return "entity id " + std::to_string(entt::to_integral(e));
}
