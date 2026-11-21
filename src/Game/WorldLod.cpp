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

// World's LOD: which level of each LOD Group shows for a view (ApplyLod), the culled tags, the impostors and the cross-fades.

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
    // The records go with the tags: ForEachImpostor lists from them, and a record whose group is gone draws its
    // impostor at the origin (GetCachedWorldTransform of a dead entity is identity).
    m_LodImpostors = false;
    std::fill(m_LodFadeOwner.begin(), m_LodFadeOwner.end(), entt::null);
    m_LodFading.clear();
    m_LodState.clear();
    std::fill(m_LodIndex.begin(), m_LodIndex.end(), ~0u);
    m_LodValid = false;
}

int World::ApplyLod(const glm::vec3& viewPos, const glm::mat4& proj) {
    auto groups = Registry.view<LODGroupComponent, HierarchyComponent>();
    if (groups.begin() == groups.end()) {
        // No groups left (the last one deleted, or a scene without any opened - the loader clears the registry in
        // place, tags and all, so the tags alone don't tell): forget the last pass, records included.
        if (m_LodValid || !m_LodState.empty() || !Registry.view<LodCulledTag>().empty()) ClearLod();
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
    // Whether a recorded group tags its child `child` (not the level it uses, nor the one fading out of it).
    auto levelTagged = [](const LodGroupState& st, entt::entity child) {
        if (st.Active < -1) return false;
        for (int j = 0; j < st.Levels; ++j)
            if (st.Kids[j] == child) return j != st.Active && j != st.FadeFrom;
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
    // Octahedral impostors (ForEachImpostor): groups are matched to a set whenever sets exist, and drawn only while
    // enabled (Project Settings) - turned off, every start is out of reach and the next pass puts each back on its
    // levels. impostorOf: the set a group's LOD 0 matches (the models its renderers draw, as the bake gathered them -
    // Shadows Only left out), or -1.
    const bool impostors = ImpostorLibrary::Available();
    const float impostorStart = ImpostorLibrary::Enabled() ? ImpostorLibrary::StartDistance() : std::numeric_limits<float>::max();
    std::vector<const Model*>& lod0Models = m_LodModels;
    auto impostorOf = [&](entt::entity lod0) -> int {
        lod0Models.clear();
        stack.assign(1, lod0);
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            if (!Registry.valid(e) || Registry.all_of<InactiveTag>(e)) continue;
            if (const auto* r = Registry.try_get<RenderableComponent>(e);
                r && r->ModelRef && r->CastShadows != RenderableComponent::ShadowCasting::ShadowsOnly)
                lod0Models.push_back(r->ModelRef.get());
            if (const auto* h = Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
        return ImpostorLibrary::Match(lod0Models);
    };

    // One pass over the groups; returns false when the incremental pass meets a group it has no record of, and the
    // caller redoes everything in full. The records are dense, in the view's order, so the steady pass over 40k
    // groups reads them front to back.
    // LodSmooth cross-fades (see LodFadeOf): the fade value of every renderer under `root`.
    const double now = !LodSmooth ? 0.0 : LodClockOverride >= 0.0 ? LodClockOverride : Time::RealtimeSinceStartup();
    auto setFade = [&](entt::entity root, float v) {
        stack.assign(1, root);
        while (!stack.empty()) {
            const entt::entity e = stack.back();
            stack.pop_back();
            if (!Registry.valid(e)) continue;
            if (Registry.all_of<RenderableComponent>(e)) {
                const size_t s = (size_t)entt::to_entity(e);
                if (s >= m_LodFadeOwner.size()) {
                    m_LodFadeOwner.resize(s + 1, entt::null);
                    m_LodFadeValue.resize(s + 1, 0.0f);
                }
                m_LodFadeOwner[s] = v != 0.0f ? e : entt::null;
                m_LodFadeValue[s] = v;
            }
            if (const auto* h = Registry.try_get<HierarchyComponent>(e))
                stack.insert(stack.end(), h->Children.begin(), h->Children.end());
        }
    };
    // Ends a group's fade now: the old level tagged (unless it is the one shown again), both levels whole.
    auto finishFade = [&](LodGroupState& st) {
        if (st.FadeFrom < 0) return;
        const int from = st.FadeFrom;
        st.FadeFrom = -1;
        if (from < st.Levels) {
            if (from != st.Active) retagSubtree(st.Kids[from], true);
            setFade(st.Kids[from], 0.0f);
        }
        if (st.Active >= 0 && st.Active < st.Levels) setFade(st.Kids[st.Active], 0.0f);
        st.ImpostorFade = 0.0f;
    };
    // A group's level goes from `was` to `active` (st.Active still `was`): retag, starting a fade when smooth.
    auto changeLevel = [&](LodGroupState& st, int was, int active, bool nested) {
        if (LodSmooth) finishFade(st);
        st.Active = (std::int8_t)active; // first: the retag below reads this group's own record for nested ones
        if (nested) return;
        const int levels = st.Levels;
        // Fade only a group that had settled: one that changed level within the last fade window is flipping between
        // views that disagree (the Scene and Game views each apply their own levels every frame), and a fade begun on
        // every flip never finished - its leaves stayed dithered for as long as both views were up. Those switch at once.
        const bool settled = now - st.LastChange >= kLodFadeSeconds;
        if (LodSmooth) st.LastChange = now;
        const bool fade = LodSmooth && settled && ((was >= 0 && was < levels) || was == kLodImpostor) && was != active;
        for (int i = 0; i < levels; ++i) {
            const bool wasTagged = was >= -1 && i != was;
            const bool nowTagged = active >= -1 && i != active && !(fade && i == was);
            if (wasTagged != nowTagged) retagSubtree(st.Kids[i], nowTagged);
        }
        if (fade) {
            st.FadeFrom = (std::int8_t)was;
            st.FadeStart = now;
            if (was < levels) setFade(st.Kids[was], -1e-4f);
            else st.ImpostorFade = -1e-4f;
            if (active >= 0 && active < levels) setFade(st.Kids[active], 1e-4f);
            else if (active == kLodImpostor) st.ImpostorFade = 1e-4f;
            m_LodFading.push_back(st.Group);
        }
    };

    // A small structure change (an outfit piece created, attached or destroyed somewhere in a forest of 40k groups)
    // patches the records instead of the full clear-and-tag: the transform cache's structure delta says what came,
    // went or moved; the groups above each of those are measured again, and each new or moved subtree is re-tagged
    // to what the groups above it say. Returns false, having changed nothing, when only the full pass can tell: the
    // delta doesn't cover everything since the last pass, a LOD Group came, went or moved, a group's level children
    // changed, or the renderer count isn't what the delta explains (a renderer added or removed in place).
    auto patchStructure = [&]() -> bool {
        const TransformStructureDelta& d = m_StructureDelta;
        if (!m_LodValid || !d.Valid || m_TransformCacheStamp == 0 || m_LodCentreStamp != m_PrevTransformCacheStamp)
            return false;
        std::uint64_t structure[3];
        TransformStructure(structure); // nothing re-parented, created or destroyed since that rebuild
        if (!std::equal(structure, structure + 3, m_TransformStructure)) return false;
        const auto& groupPool = Registry.storage<LODGroupComponent>();
        if (signature[3] != m_LodSignature[3] || groupPool.size() != m_LodState.size()) return false;
        for (entt::entity e : d.Added)
            if (!Registry.valid(e) || groupPool.contains(e)) return false;
        for (const auto& [e, was] : d.Reparented)
            if (!Registry.valid(e) || groupPool.contains(e)) return false;
        // Every record still a group with the level children the full pass would read now (so, with the counts equal,
        // the same set of groups); read only, across the cores.
        const auto& hierarchyPool = Registry.storage<HierarchyComponent>();
        const bool same = std::all_of(std::execution::par, m_LodState.begin(), m_LodState.end(), [&](const LodGroupState& st) {
            if (!Registry.valid(st.Group) || !groupPool.contains(st.Group) || !hierarchyPool.contains(st.Group)) return false;
            const auto& children = hierarchyPool.get(st.Group).Children;
            if (std::min<size_t>(children.size(), 4) != st.Levels) return false;
            for (int i = 0; i < st.Levels; ++i)
                if (st.Kids[i] != children[i]) return false;
            return true;
        });
        if (!same) return false;
        // The renderers: what the last pass counted, plus the new ones, less the dropped ones that had one.
        const auto& renderPool = Registry.storage<RenderableComponent>();
        auto ownedRenderer = [&](entt::entity e) {
            const size_t s = (size_t)entt::to_entity(e);
            return s < m_LodRenderOwner.size() && m_LodRenderOwner[s] == e;
        };
        size_t added = 0, removed = 0;
        for (entt::entity e : d.Added) added += renderPool.contains(e) && !ownedRenderer(e) ? 1 : 0;
        for (const auto& [e, was] : d.Dropped) removed += ownedRenderer(e) ? 1 : 0;
        if (m_LodSignature[1] + added - removed != (std::uint64_t)renderPool.size()) return false;

        // The groups above a place something came, went or moved (`from` included): measured again below. False when
        // one has no record (it can't: the set is the same), as a guard.
        std::vector<std::pair<entt::entity, entt::entity>>& dropped = m_LodDroppedSorted;
        dropped.assign(d.Dropped.begin(), d.Dropped.end());
        std::sort(dropped.begin(), dropped.end());
        // A place that was destroyed too: up its recorded parents to the first one still alive.
        auto alive = [&](entt::entity p) {
            for (size_t depth = 0; p != entt::null && !Registry.valid(p) && depth <= kMaxHierarchyDepth; ++depth) {
                const auto it = std::lower_bound(dropped.begin(), dropped.end(), std::make_pair(p, entt::entity{entt::null}),
                                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                p = it != dropped.end() && it->first == p ? it->second : entt::entity{entt::null};
            }
            return p != entt::null && Registry.valid(p) ? p : entt::entity{entt::null};
        };
        std::vector<entt::entity>& remeasure = m_LodRemeasure;
        remeasure.clear();
        auto groupsAbove = [&](entt::entity from) {
            for (size_t depth = 0; from != entt::null && Registry.valid(from) && depth <= kMaxHierarchyDepth; ++depth) {
                if (groupPool.contains(from)) {
                    if (!recordOf(from)) return false;
                    remeasure.push_back(from);
                }
                from = hierarchyPool.contains(from) ? hierarchyPool.get(from).Parent : entt::entity{entt::null};
            }
            return true;
        };
        std::vector<entt::entity>& touched = m_LodTouched;
        touched.assign(d.Added.begin(), d.Added.end());
        for (const auto& [e, was] : d.Reparented) touched.push_back(e);
        for (entt::entity e : touched)
            if (!groupsAbove(e)) return false;
        for (const auto& [e, was] : d.Reparented)
            if (!groupsAbove(alive(was))) return false;
        for (const auto& [e, was] : d.Dropped)
            if (!groupsAbove(alive(was))) return false;

        // Checked: from here on it changes things.
        for (const auto& [e, was] : d.Dropped)
            if (ownedRenderer(e)) m_LodRenderOwner[(size_t)entt::to_entity(e)] = entt::null;
        for (entt::entity e : d.Added) {
            if (!renderPool.contains(e)) continue;
            const size_t s = (size_t)entt::to_entity(e);
            if (s >= m_LodRenderOwner.size()) m_LodRenderOwner.resize(s + 1, entt::null);
            m_LodRenderOwner[s] = e;
        }
        std::sort(remeasure.begin(), remeasure.end());
        remeasure.erase(std::unique(remeasure.begin(), remeasure.end()), remeasure.end());
        for (entt::entity g : remeasure) {
            LodGroupState& st = m_LodState[m_LodIndex[(size_t)entt::to_entity(g)]];
            measure(st, groupPool.get(g));
            if (impostors && st.Levels > 0) {
                st.Impostor = (std::int16_t)impostorOf(st.Kids[0]);
                m_LodImpostors |= st.Impostor >= 0;
            }
        }
        // Each new or moved subtree, tagged as the groups above it say: culled where any of them tags the child on the
        // path down to it (the groups' levels are the ones they had; the pass below changes those as usual).
        for (entt::entity e : touched) {
            bool culled = false;
            entt::entity child = e;
            for (size_t depth = 0; depth <= kMaxHierarchyDepth && !culled; ++depth) {
                const entt::entity parent = hierarchyPool.contains(child) ? hierarchyPool.get(child).Parent : entt::entity{entt::null};
                if (parent == entt::null || !Registry.valid(parent)) break;
                if (groupPool.contains(parent))
                    if (const LodGroupState* st = recordOf(parent)) culled = levelTagged(*st, child);
                child = parent;
            }
            if (LodSmooth) setFade(e, 0.0f); // a fading level it sits under sets it again below
            retagSubtree(e, culled);
        }
        return true;
    };
    // Under unchanged counts too: an entity destroyed and another created in its place since the last pass (the
    // transform cache lists both) is a structure change the signature can't see.
    if (!full && m_LodCentreStamp == m_PrevTransformCacheStamp && m_TransformCacheStamp != 0) {
        const TransformStructureDelta& d = m_StructureDelta;
        full = d.Valid ? !d.Added.empty() || !d.Reparented.empty() || !d.Dropped.empty() : m_TransformMembershipChanged;
    }
    bool structurePatched = false;
    if (full && patchStructure()) {
        full = false;
        structurePatched = true;
    }

    int evaluated = 0;
    auto pass = [&]() -> bool {
        evaluated = 0;
        if (full) {
            ClearLod(); // m_LodValid is set again once this pass is done
            ++m_LodFullPasses;
            std::fill(m_LodRenderOwner.begin(), m_LodRenderOwner.end(), entt::null);
            for (entt::entity e : static_cast<const entt::sparse_set&>(Registry.storage<RenderableComponent>())) {
                const size_t s = (size_t)entt::to_entity(e);
                if (s >= m_LodRenderOwner.size()) m_LodRenderOwner.resize(s + 1, entt::null);
                m_LodRenderOwner[s] = e;
            }
        } else if (m_LodCentreStamp != m_TransformCacheStamp || m_TransformCacheStamp == 0) {
            // Only what the transform cache moved since the records were measured, when it can say (after a structure
            // patch it lists the new and moved subtrees too; the groups above them were measured by the patch).
            const bool patch = m_TransformCacheStamp != 0 && m_LodCentreStamp == m_PrevTransformCacheStamp &&
                               (!m_TransformMembershipChanged || structurePatched);
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
            // LodSmooth: the level shown now holds kLodHysteresis past its switch points either way.
            const int cur = !LodSmooth || st.Active < -1 ? -100 : (st.Active == -1 ? (int)st.Levels : (int)st.Active);
            int level = -1; // culled
            for (int i = 0; i < st.Levels; ++i) {
                float thr = thresholds[i];
                if (cur != -100) thr *= i < cur ? 1.0f + kLodHysteresis : i == cur ? 1.0f - kLodHysteresis : 1.0f;
                if (height >= thr) {
                    level = i;
                    break;
                }
            }
            // Past the start distance, the impostor (wherever the levels draw anything); held half the hysteresis past
            // it either way, so a view standing at it doesn't flip.
            if (level >= 0 && st.Impostor >= 0 &&
                dist >= impostorStart * (cur == kLodImpostor ? 1.0f - 0.5f * kLodHysteresis : 1.0f + 0.5f * kLodHysteresis))
                return kLodImpostor;
            return level;
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
                changeLevel(st, was, active, culledFromAbove(st.Group));
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
                if (impostors && st.Levels > 0) {
                    st.Impostor = (std::int16_t)impostorOf(st.Kids[0]);
                    m_LodImpostors |= st.Impostor >= 0;
                }
            } else if (at >= m_LodState.size() || m_LodState[at].Group != g) {
                return false; // the children can't have changed without the signature changing
            }
            LodGroupState& st = m_LodState[at];
            const int levels = st.Levels;
            if (levels == 0) continue;
            const int active = full ? pickLevel(st, lod, evaluated) : m_LodNext[at];
            const int was = st.Active; // kLodSkipped (or a fresh record) tagged nothing
            if (!full && was == active) continue;
            if (full) {
                st.Active = (std::int8_t)active;
                for (int i = 0; i < levels; ++i)
                    if (i != active) tagSubtree(st.Kids[i]);
                continue;
            }
            changeLevel(st, was, active, culledFromAbove(g));
        }
        return index == m_LodState.size();
    };
    if (!pass()) {
        full = true;
        pass();
    }
    // The cross-fades under way: on with each, or done.
    if (!m_LodFading.empty()) {
        size_t keep = 0;
        for (size_t i = 0; i < m_LodFading.size(); ++i) {
            const entt::entity g = m_LodFading[i];
            const size_t slot = (size_t)entt::to_entity(g);
            LodGroupState* st = slot < m_LodIndex.size() && m_LodIndex[slot] < m_LodState.size() &&
                                m_LodState[m_LodIndex[slot]].Group == g ? &m_LodState[m_LodIndex[slot]] : nullptr;
            if (!st || st->FadeFrom < 0) continue;
            const float t = (float)((now - st->FadeStart) / kLodFadeSeconds);
            if (!LodSmooth || t >= 1.0f) {
                finishFade(*st);
                continue;
            }
            const float f = std::clamp(t, 1e-4f, 0.9999f);
            if (st->FadeFrom < st->Levels) setFade(st->Kids[st->FadeFrom], -f);
            else st->ImpostorFade = -f;
            if (st->Active >= 0 && st->Active < st->Levels) setFade(st->Kids[st->Active], f);
            else if (st->Active == kLodImpostor) st->ImpostorFade = f;
            m_LodFading[keep++] = g;
        }
        m_LodFading.resize(keep);
    }
    std::copy(signature, signature + 4, m_LodSignature);
    m_LodValid = true;
    m_LodMemo = {m_TransformCacheStamp, viewPos, yScale, ortho, inactiveCount, evaluated};
    return evaluated;
}
