#include "World.h"
#include <cstring>
#include <atomic>
#include <execution>
#include <numeric>
#include "Model.h"
#include "Log.h"
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

namespace {
// Cycle guard for the world-transform cache's parent-chain walk: no legitimate scene nests this
// deep, so exceeding it means a malformed hierarchy, not a tall one.
constexpr size_t kMaxHierarchyDepth = 1024;

// The box/primitive path (World::CreateBox, "Add > Cube/Sphere/...") happens to work with a
// plain Position/Scale AABB because every built-in primitive's native mesh is pre-built to fit
// exactly inside a -0.5..0.5 unit cube — so "size == Scale" is coincidentally exact for them.
// An IMPORTED model's native bounds are whatever the source file's geometry actually spans
// (rarely a unit cube), so that same formula would place the collider nowhere near the visible
// mesh. Using the entity's own RenderableComponent bounds when one exists generalizes correctly
// to any mesh while staying bit-identical for boxes/primitives (verified: BoundsMin/Max for the
// cube/sphere/cylinder/cone/plane generators are exactly -0.5..0.5). Falls back to the old
// Scale-only box for a Collider added to a mesh-less entity (a light or empty), which has no
// bounds to derive from. Still axis-aligned/rotation-ignoring, matching the physics's existing,
// deliberate "simple gameplay collision" simplification (see ColliderComponent).
AABB ColliderWorldBounds(const entt::registry& registry, entt::entity entity, const TransformComponent& transform) {
    if (const auto* renderable = registry.try_get<RenderableComponent>(entity); renderable && renderable->ModelRef) {
        glm::vec3 worldMin = transform.Position + renderable->ModelRef->BoundsMin() * transform.Scale;
        glm::vec3 worldMax = transform.Position + renderable->ModelRef->BoundsMax() * transform.Scale;
        return AABB{glm::min(worldMin, worldMax), glm::max(worldMin, worldMax)}; // guards a negative Scale flipping min/max
    }
    return AABB::FromCenterSize(transform.Position, transform.Scale);
}
} // namespace

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

// World::ResolveCollisions (the AABB push-out that moved the Play-mode Player) was removed in
// #185 PR 3 — the Player now sweeps the mover's kinematic capsule through PhysicsWorld's scene. The
// AABB Raycast below stays: the editor still uses it for drop-to-surface / snap queries.
entt::entity World::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDist, float& outDist) const {
    entt::entity best = entt::null;
    float bestT = maxDist;
    auto view = Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (auto entity : view) {
        const auto& collider = view.get<const ColliderComponent>(entity);
        if (collider.IsTrigger) continue;
        AABB b = ColliderWorldBounds(Registry, entity, WorldSpaceTransform(entity)); // #114
        float t;
        if (b.RayIntersect(origin, dir, t) && t < bestT) {
            bestT = t;
            best = entity;
        }
    }
    outDist = bestT;
    return best;
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

void World::SetLodCulled(entt::entity e, bool culled) {
    const size_t s = (size_t)entt::to_entity(e);
    if (culled) {
        Registry.emplace_or_replace<LodCulledTag>(e);
        if (s >= m_LodCulledOwner.size()) m_LodCulledOwner.resize(s + 1, entt::null);
        m_LodCulledOwner[s] = e;
    } else {
        Registry.remove<LodCulledTag>(e);
        if (s < m_LodCulledOwner.size() && m_LodCulledOwner[s] == e) m_LodCulledOwner[s] = entt::null;
    }
}

void World::ClearLod() {
    Registry.clear<LodCulledTag>();
    std::fill(m_LodCulledOwner.begin(), m_LodCulledOwner.end(), entt::null);
    m_LodValid = false;
}

int World::ApplyLod(const glm::vec3& viewPos, const glm::mat4& proj) {
    auto groups = Registry.view<LODGroupComponent, HierarchyComponent>();
    if (groups.begin() == groups.end()) {
        if (!Registry.view<LodCulledTag>().empty()) ClearLod();
        m_LodValid = false;
        return 0;
    }
    // Incremental (perf): each group remembers the level it last tagged for, and a call only re-tags the groups whose
    // level changed - a forest of 40k groups used to clear and re-emplace ~100k tags per view, three views a frame.
    // Anything that could change which renderers sit under which level (re-parenting, renderers or hierarchies added
    // or removed, a group's own children changing) falls back to the full clear-and-tag below.
    const std::uint64_t signature[4] = {m_HierarchyVersion, (std::uint64_t)Registry.storage<RenderableComponent>().size(),
                                        (std::uint64_t)Registry.storage<HierarchyComponent>().size(),
                                        (std::uint64_t)Registry.storage<LODGroupComponent>().size()};
    bool full = !m_LodValid || !std::equal(signature, signature + 4, m_LodSignature);
    const bool ortho = proj[3][3] == 1.0f; // glm::ortho leaves w = 1; perspective sets it 0
    const float yScale = proj[1][1];       // 1 / tan(fovY/2), or 1 / halfHeight for ortho
    // A frame asks again for the view it just applied (the spot shadows, the SSAO pre-pass and the draw all use the
    // game camera): nothing moved since (the transform cache stamp is per frame), so the tags already match.
    const std::uint64_t inactiveCount = (std::uint64_t)Registry.storage<InactiveTag>().size();
    if (!full && m_LodMemo.Stamp == m_TransformCacheStamp && m_TransformCacheStamp != 0 && m_LodMemo.ViewPos == viewPos &&
        m_LodMemo.YScale == yScale && m_LodMemo.Ortho == ortho && m_LodMemo.Inactive == inactiveCount)
        return m_LodMemo.Evaluated;
    std::vector<entt::entity>& stack = m_LodStack;
    // The recorded group at `e`, if any.
    auto recordOf = [&](entt::entity e) -> const LodGroupState* {
        const size_t s = (size_t)entt::to_entity(e);
        if (s >= m_LodIndex.size() || m_LodIndex[s] >= m_LodState.size()) return nullptr;
        const LodGroupState& st = m_LodState[m_LodIndex[s]];
        return st.Group == e ? &st : nullptr;
    };
    // Whether a recorded group tags its child `child` (not the level it uses).
    auto levelTagged = [](const LodGroupState& st, entt::entity child) {
        if (st.Active < -1) return false;
        for (int j = 0; j < st.Levels; ++j)
            if (st.Kids[j] == child) return j != st.Active;
        return false; // a fifth child or later: always drawn
    };
    // Full pass: tag a level's whole subtree (an untouched renderer stays tagged by whichever group tagged it).
    auto tagSubtree = [&](entt::entity root) {
        stack.assign(1, root);
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            if (!Registry.valid(e)) continue;
            if (Registry.all_of<RenderableComponent>(e)) SetLodCulled(e, true);
            if (const auto* h = Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
    };
    // Incremental pass: set a changed level's subtree to what every group above each renderer says - culled by this
    // level (`culled`) or by a level of any group nested inside it.
    std::vector<std::pair<entt::entity, bool>>& walk = m_LodWalk;
    auto retagSubtree = [&](entt::entity root, bool culled) {
        walk.assign(1, {root, culled});
        while (!walk.empty()) {
            const auto [e, cull] = walk.back();
            walk.pop_back();
            if (!Registry.valid(e)) continue;
            if (Registry.all_of<RenderableComponent>(e)) {
                SetLodCulled(e, cull);
            }
            const auto* h = Registry.try_get<HierarchyComponent>(e);
            if (!h) continue;
            const LodGroupState* nested = Registry.all_of<LODGroupComponent>(e) ? recordOf(e) : nullptr;
            for (entt::entity c : h->Children) walk.emplace_back(c, cull || (nested && levelTagged(*nested, c)));
        }
    };
    // A group sitting inside a level some group above it culls: everything under it is tagged already.
    auto culledFromAbove = [&](entt::entity g) {
        entt::entity child = g;
        for (size_t depth = 0; depth <= kMaxHierarchyDepth; ++depth) {
            const auto* h = Registry.try_get<HierarchyComponent>(child);
            const entt::entity parent = h ? h->Parent : entt::null;
            if (parent == entt::null || !Registry.valid(parent)) return false;
            if (Registry.all_of<LODGroupComponent>(parent))
                if (const LodGroupState* st = recordOf(parent); st && levelTagged(*st, child)) return true;
            child = parent;
        }
        return false;
    };
    // The automatic group size, as Unity's LODGroup: the largest axis of the subtree's renderers' bounds in the
    // group's own space, times the group's largest scale (a yawed bush's world box is up to 1.4x wider).
    auto subtreeSize = [&](entt::entity root, const glm::mat4& groupWorld) {
        const float scale = std::max(glm::length(glm::vec3(groupWorld[0])),
                                     std::max(glm::length(glm::vec3(groupWorld[1])), glm::length(glm::vec3(groupWorld[2]))));
        if (!(scale > 1e-8f) || std::abs(glm::determinant(glm::mat3(groupWorld))) < 1e-12f) return 0.0f;
        const glm::mat4 toGroup = glm::inverse(groupWorld);
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        stack.assign(1, root);
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            if (!Registry.valid(e)) continue;
            if (const auto* r = Registry.try_get<RenderableComponent>(e); r && r->ModelRef) {
                const glm::vec3 bmin = r->ModelRef->BoundsMin(), bmax = r->ModelRef->BoundsMax();
                if (bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z) {
                    const glm::mat4 m = toGroup * GetCachedWorldTransform(e);
                    for (int c = 0; c < 8; ++c) {
                        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                        const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
                        lo = glm::min(lo, w);
                        hi = glm::max(hi, w);
                    }
                }
            }
            if (const auto* h = Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
        // Unity's LODGroup size is the largest axis of its bounds (not the diagonal, up to 1.7x larger, which held
        // every level - and a forest's ground cover - on screen far longer than the pack was tuned for).
        if (lo.x > hi.x) return 0.0f;
        const glm::vec3 ext = hi - lo;
        return std::max(ext.x, std::max(ext.y, ext.z)) * scale;
    };

    // A group's centre and automatic size, from the transform cache.
    auto measure = [&](LodGroupState& st, const LODGroupComponent& lod) {
        const glm::mat4 groupWorld = GetCachedWorldTransform(st.Group);
        st.Centre = glm::vec3(groupWorld[3]);
        // Automatic size: LOD 0's bounds, measured again whenever the group itself moves.
        if (lod.Size <= 0.0f && st.Levels > 0) st.AutoSize = subtreeSize(st.Kids[0], groupWorld);
    };

    // One pass over the groups; returns false when the incremental pass meets a group it has no record of, and the
    // caller redoes everything in full. The records are dense, in the view's order, so the steady pass over 40k
    // groups reads them front to back.
    int evaluated = 0;
    auto pass = [&]() -> bool {
        evaluated = 0;
        if (full) {
            Registry.clear<LodCulledTag>();
            std::fill(m_LodCulledOwner.begin(), m_LodCulledOwner.end(), entt::null);
            m_LodState.clear();
            std::fill(m_LodIndex.begin(), m_LodIndex.end(), ~0u);
        } else if (m_LodCentreStamp != m_TransformCacheStamp || m_TransformCacheStamp == 0) {
            // Only what the transform cache moved since the records were measured, when it can say.
            const bool patch = m_TransformCacheStamp != 0 && m_LodCentreStamp == m_PrevTransformCacheStamp &&
                               !m_TransformMembershipChanged;
            if (patch) {
                for (entt::entity e : m_ChangedTransforms) {
                    const size_t s = (size_t)entt::to_entity(e);
                    if (s >= m_LodIndex.size() || m_LodIndex[s] == ~0u) continue;
                    LodGroupState& st = m_LodState[m_LodIndex[s]];
                    if (st.Group == e) measure(st, Registry.get<LODGroupComponent>(e));
                }
            } else {
                for (LodGroupState& st : m_LodState)
                    if (Registry.valid(st.Group) && Registry.all_of<LODGroupComponent>(st.Group))
                        measure(st, Registry.get<LODGroupComponent>(st.Group));
            }
        }
        m_LodCentreStamp = m_TransformCacheStamp;
        // The level each record wants, picked across the cores in fixed chunks (read only: the transform-cache centres,
        // the group components and the inactive tags); a steady forest of 40k groups took ~1 ms on one thread. The
        // serial walk below then only applies the changes. The full pass builds the records, so it picks inline.
        const auto pickLevel = [&](const LodGroupState& st, const LODGroupComponent& lod, int& counted) -> int {
            if (Registry.all_of<InactiveTag>(st.Group)) return kLodSkipped; // an inactive group tags nothing
            ++counted;
            const float size = lod.Size > 0.0f ? lod.Size : st.AutoSize;
            const float dist = std::max(glm::length(st.Centre - viewPos), 1e-4f);
            const float height = ortho ? size * yScale * 0.5f : size * yScale / (2.0f * dist);
            const float thresholds[4] = {lod.Lod0, lod.Lod1, lod.Lod2, lod.Lod3};
            for (int i = 0; i < st.Levels; ++i)
                if (height >= thresholds[i]) return i;
            return -1; // culled
        };
        if (!full) {
            const size_t count = m_LodState.size();
            constexpr size_t kChunk = 4096;
            const size_t chunks = (count + kChunk - 1) / kChunk;
            m_LodNext.resize(count);
            m_LodChunkEvaluated.assign(chunks, 0);
            m_LodChunkIds.resize(chunks);
            std::iota(m_LodChunkIds.begin(), m_LodChunkIds.end(), size_t(0));
            auto pick = [&](size_t c) {
                int counted = 0;
                const size_t end = std::min(count, (c + 1) * kChunk);
                for (size_t i = c * kChunk; i < end; ++i) {
                    const LodGroupState& st = m_LodState[i];
                    const LODGroupComponent* lod =
                        st.Levels > 0 && Registry.valid(st.Group) ? Registry.try_get<LODGroupComponent>(st.Group) : nullptr;
                    m_LodNext[i] = (std::int8_t)(lod ? pickLevel(st, *lod, counted) : kLodSkipped);
                }
                m_LodChunkEvaluated[c] = counted;
            };
            if (chunks > 1) std::for_each(std::execution::par, m_LodChunkIds.begin(), m_LodChunkIds.end(), pick);
            else if (chunks == 1) pick(0);
            for (int n : m_LodChunkEvaluated) evaluated += n;
            // Steady state: apply only the records whose level changed, read straight from the dense records. Walking
            // the groups view to reach them (and its per-group component reads) cost more than the picks. The records
            // are in the view's order, and the signature above sends any change of membership to the full pass.
            for (size_t at = 0; at < count; ++at) {
                LodGroupState& st = m_LodState[at];
                const int levels = st.Levels;
                const int active = m_LodNext[at];
                const int was = st.Active;
                if (levels == 0 || was == active) continue;
                st.Active = (std::int8_t)active;
                if (culledFromAbove(st.Group)) continue;
                for (int i = 0; i < levels; ++i) {
                    const bool wasTagged = was >= -1 && i != was;
                    const bool nowTagged = active >= -1 && i != active;
                    if (wasTagged != nowTagged) retagSubtree(st.Kids[i], nowTagged);
                }
            }
            return true;
        }
        size_t index = 0;
        for (entt::entity g : groups) {
            const auto& lod = groups.get<LODGroupComponent>(g);
            const size_t at = index++;
            if (full) {
                const auto& children = groups.get<HierarchyComponent>(g).Children;
                LodGroupState& st = m_LodState.emplace_back();
                st.Group = g;
                st.Levels = (std::uint8_t)std::min<size_t>(children.size(), 4);
                for (int i = 0; i < st.Levels; ++i) st.Kids[i] = children[i];
                const size_t slot = (size_t)entt::to_entity(g);
                if (slot >= m_LodIndex.size()) m_LodIndex.resize(slot + 1, ~0u);
                m_LodIndex[slot] = (std::uint32_t)at;
                measure(st, lod);
            } else if (at >= m_LodState.size() || m_LodState[at].Group != g) {
                return false; // the children can't have changed without the signature changing
            }
            LodGroupState& st = m_LodState[at];
            const int levels = st.Levels;
            if (levels == 0) continue;
            const int active = full ? pickLevel(st, lod, evaluated) : m_LodNext[at];
            const int was = st.Active; // kLodSkipped (or a fresh record) tagged nothing
            if (!full && was == active) continue;
            st.Active = (std::int8_t)active; // first: the retag below reads this group's own record for nested ones
            if (full) {
                for (int i = 0; i < levels; ++i)
                    if (i != active) tagSubtree(st.Kids[i]);
                continue;
            }
            if (culledFromAbove(g)) continue;
            for (int i = 0; i < levels; ++i) {
                const bool wasTagged = was >= -1 && i != was;
                const bool nowTagged = active >= -1 && i != active;
                if (wasTagged != nowTagged) retagSubtree(st.Kids[i], nowTagged);
            }
        }
        return index == m_LodState.size();
    };
    if (!pass()) {
        full = true;
        pass();
    }
    std::copy(signature, signature + 4, m_LodSignature);
    m_LodValid = true;
    m_LodMemo = {m_TransformCacheStamp, viewPos, yScale, ortho, inactiveCount, evaluated};
    return evaluated;
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
    if (RebuildChangedTransformsOnly()) return;
    size_t slots = 0;
    for (entt::entity entity : view) slots = std::max(slots, (size_t)entt::to_entity(entity) + 1);
    auto ensureSlots = [&](size_t n) {
        if (n <= m_WorldTransformKeys.size()) return;
        m_WorldTransformKeys.resize(n, entt::null);
        m_WorldTransformCache.resize(n);
        m_LocalSnapshots.resize(n);
        m_LocalHashes.resize(n, 0);
        m_RebuildDone.resize(n, 0);
        m_RebuildChanged.resize(n, 0);
    };
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
    TransformStructure(m_TransformStructure);
    m_TransformStructureValid = true;
}

void World::TransformStructure(std::uint64_t out[3]) {
    out[0] = m_HierarchyVersion;
    out[1] = Registry.storage<HierarchyComponent>().size();
    out[2] = Registry.storage<TransformComponent>().size();
}

// The common frame: the hierarchy is as the last full rebuild left it and the same entities have transforms, so each
// slot's recorded parent still holds. One linear pass over the transforms finds the ones whose local values moved;
// only those and their descendants are recomposed (parents first), everything else keeps its matrix. Returns false,
// having changed nothing, when the full walk is needed instead.
bool World::RebuildChangedTransformsOnly() {
    if (!m_TransformStructureValid) return false;
    auto view = Registry.view<TransformComponent>();
    std::uint64_t structure[3];
    TransformStructure(structure);
    if (!std::equal(structure, structure + 3, m_TransformStructure)) return false;
    std::vector<entt::entity>& dirty = m_DirtyScratch;
    dirty.clear();
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
    std::atomic<bool> newToCache{false};
    auto scan = [&](size_t c) {
        std::vector<entt::entity>& out = s_ChunkDirty[c];
        out.clear();
        const size_t end = std::min(count, (c + 1) * kChunk);
        for (size_t i = c * kChunk; i < end; ++i) {
            const entt::entity e = ents[(std::ptrdiff_t)i];
            const size_t s = (size_t)entt::to_entity(e);
            if (s >= m_WorldTransformKeys.size() || m_WorldTransformKeys[s] != e) { newToCache = true; return; }
            if (m_LocalHashes[s] != LocalHash(comps[(std::ptrdiff_t)i])) out.push_back(e); // 0 (no transform at the snapshot) never matches
        }
    };
    if (chunks > 1) std::for_each(std::execution::par, s_ChunkIds.begin(), s_ChunkIds.end(), scan);
    else if (chunks == 1) scan(0);
    if (newToCache) return false; // new to the cache
    for (size_t c = 0; c < chunks; ++c) dirty.insert(dirty.end(), s_ChunkDirty[c].begin(), s_ChunkDirty[c].end());
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
