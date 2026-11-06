#pragma once
#include <algorithm>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

#include <entt/entt.hpp>

#include "Components.h"
#include "Model.h"
#include "World.h"

// Renderables whose models never animate (Model::NeverAnimates: no bones, no clips - every rock and fern), remembered
// by their slot in the renderable pool so the per-frame animation tick and the pre-skin gather skip them without
// touching the Model: reading ~150k of them a frame was 15 ms in the forest. Re-checked whenever the pool changes size
// or Model::AnimationEpoch() moves.
struct StillModelCache {
    std::vector<const Model*> Models;
    std::vector<std::uint8_t> Still;
    const World* Owner = nullptr;
    std::uint64_t Epoch = ~0ull;
    void Sync(const World& world, size_t poolSize) {
        if (Owner == &world && Models.size() == poolSize && Epoch == Model::AnimationEpoch()) return;
        Models.assign(poolSize, nullptr);
        Still.assign(poolSize, 0);
        Owner = &world;
        Epoch = Model::AnimationEpoch();
    }
    // Slot k of the pool holds `m`: true when it is known not to animate; otherwise (re)classifies it.
    bool Skip(size_t k, const Model* m) {
        if (k >= Models.size()) return false;
        if (m == Models[k] && Still[k]) return true;
        Models[k] = m;
        Still[k] = m && m->NeverAnimates() ? 1 : 0;
        return Still[k] != 0;
    }
    // The renderables that may animate (each one Skip doesn't rule out, with a model), in pool order. The pool is read
    // across the cores in fixed chunks - one thread reading ~150k components' ModelRef was ~1 ms of the forest's frame,
    // paid by the animation tick and again by the pre-skin gather. Skip only writes its own slot, so chunks don't meet.
    void Gather(World& world, std::vector<entt::entity>& out) {
        const auto& pool = world.Registry.storage<RenderableComponent>();
        const size_t count = pool.size();
        Sync(world, count);
        const auto comps = pool.cbegin();
        const auto ents = static_cast<const entt::sparse_set&>(pool).begin();
        constexpr size_t kChunk = 8192;
        const size_t chunks = (count + kChunk - 1) / kChunk;
        if (ChunkOut.size() < chunks) ChunkOut.resize(chunks);
        ChunkIds.resize(chunks);
        std::iota(ChunkIds.begin(), ChunkIds.end(), size_t(0));
        auto scan = [&](size_t c) {
            std::vector<entt::entity>& o = ChunkOut[c];
            o.clear();
            const size_t end = std::min(count, (c + 1) * kChunk);
            for (size_t i = c * kChunk; i < end; ++i) {
                const Model* m = comps[(std::ptrdiff_t)i].ModelRef.get();
                if (Skip(i, m) || !m) continue;
                o.push_back(ents[(std::ptrdiff_t)i]);
            }
        };
        if (chunks > 1) std::for_each(std::execution::par, ChunkIds.begin(), ChunkIds.end(), scan);
        else if (chunks == 1) scan(0);
        out.clear();
        for (size_t c = 0; c < chunks; ++c) out.insert(out.end(), ChunkOut[c].begin(), ChunkOut[c].end());
    }
    // Gather, reusing the last result when it was made this frame (same `frame`) over the same pool - same world, same
    // size and storage, same Model::AnimationEpoch(). The animation tick and the pre-skin gather read the same pool
    // twice a frame; the second one reuses the first. Entities can be destroyed in between, so a caller walking the
    // result must check each is still valid and still has a RenderableComponent.
    const std::vector<entt::entity>& GatherCached(World& world, std::uint64_t frame) {
        const auto& pool = world.Registry.storage<RenderableComponent>();
        const void* data = static_cast<const entt::sparse_set&>(pool).data();
        if (!(Memo.Valid && Memo.Frame == frame && Memo.Owner == &world && Memo.Size == pool.size() && Memo.Data == data &&
              Memo.Epoch == Model::AnimationEpoch())) {
            Gather(world, Memo.Result);
            Memo.Frame = frame; Memo.Owner = &world; Memo.Size = pool.size(); Memo.Data = data;
            Memo.Epoch = Model::AnimationEpoch(); Memo.Valid = true;
        }
        return Memo.Result;
    }
    struct MemoState {
        std::uint64_t Frame = 0;
        const World* Owner = nullptr;
        size_t Size = 0;
        const void* Data = nullptr;
        std::uint64_t Epoch = 0;
        bool Valid = false;
        std::vector<entt::entity> Result;
    } Memo;
    std::vector<std::vector<entt::entity>> ChunkOut;
    std::vector<size_t> ChunkIds;
};
