#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

class Model;
class Shader;
struct MaterialAsset;
class VisibleIndexBuffer;

// GPU instancing of repeated static models (Project Settings > Quality > GPU instancing). A pass groups the copies of
// one model that draw the same way, puts their transforms in the per-frame instance buffer, and draws each group once
// with an instance count: ModelVertex.glsl and ShadowDepth.vert.glsl read the transforms (uInstanced) at binding 27.

// Items grouped by key. Groups keep the order of their first item, and items keep their order within a group: Order
// lists every item, group by group, and Groups[g] is the run of Order that group g takes.
struct InstanceGroups {
    struct Group {
        std::uint32_t First, Count;
    };
    std::vector<std::uint32_t> Order;
    std::vector<Group> Groups;
};

// `hash(i)` and `same(a, b)` describe item i's key; an item whose hash is kNoBatch is a group of its own.
constexpr std::uint64_t kNoBatch = ~0ull;
template <class Hash, class Same>
void GroupInstances(std::size_t count, Hash hash, Same same, InstanceGroups& out) {
    // Runs per pass on every frame, so nothing here allocates once the vectors have grown: an open-addressing table
    // from hash to the first group with it, and the groups that share a hash chained through `next`.
    static thread_local std::vector<std::uint64_t> slotHash; // kNoBatch = empty
    static thread_local std::vector<std::uint32_t> slotGroup, groupOf, leader, sizes, next;
    std::size_t cap = 16;
    while (cap < count * 2) cap <<= 1;
    slotHash.assign(cap, kNoBatch);
    slotGroup.resize(cap);
    groupOf.resize(count);
    leader.clear();
    sizes.clear();
    next.clear();
    constexpr std::uint32_t kEnd = ~0u;
    const auto newGroup = [&](std::uint32_t i) {
        leader.push_back(i);
        sizes.push_back(0);
        next.push_back(kEnd);
        return (std::uint32_t)leader.size() - 1;
    };
    for (std::uint32_t i = 0; i < (std::uint32_t)count; ++i) {
        const std::uint64_t h = hash(i);
        std::uint32_t g = kEnd;
        if (h == kNoBatch) {
            g = newGroup(i);
        } else {
            std::size_t at = (std::size_t)((h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull) & (cap - 1);
            while (slotHash[at] != kNoBatch && slotHash[at] != h) at = (at + 1) & (cap - 1);
            if (slotHash[at] == kNoBatch) {
                g = newGroup(i);
                slotHash[at] = h;
                slotGroup[at] = g;
            } else {
                std::uint32_t c = slotGroup[at], last = c;
                for (; c != kEnd; last = c, c = next[c])
                    if (same(leader[c], i)) { g = c; break; }
                if (g == kEnd) { // the same hash, a different key
                    g = newGroup(i);
                    next[last] = g;
                }
            }
        }
        groupOf[i] = g;
        ++sizes[g];
    }
    out.Groups.resize(leader.size());
    std::uint32_t at = 0;
    for (std::size_t g = 0; g < leader.size(); ++g) {
        out.Groups[g] = {at, 0};
        at += sizes[g];
    }
    out.Order.resize(count);
    for (std::uint32_t i = 0; i < (std::uint32_t)count; ++i) {
        auto& grp = out.Groups[groupOf[i]];
        out.Order[grp.First + grp.Count++] = i;
    }
}

// Whether two material slot lists draw alike: slot for slot the same asset, or one with the same shader, queue and values.
bool SameSlots(const std::vector<std::shared_ptr<MaterialAsset>>& a, const std::vector<std::shared_ptr<MaterialAsset>>& b);
// A hash of the slot list's assets, for GroupInstances.
std::uint64_t HashSlots(const std::vector<std::shared_ptr<MaterialAsset>>& slots, std::uint64_t seed);

namespace InstanceBuffer {
// The per-frame instance transforms, two mat4s per instance (the transform, then its normal matrix), bound at 27.
// Each Upload re-specifies the buffer (the draws before it keep reading what they were issued with).
std::vector<glm::mat4>& Staging();
void Upload();
}

// One caster in a depth-only pass: the SSAO and opaque depth pre-passes, and the sun, spot and point shadow maps.
struct DepthCaster {
    Model* Ref;
    const std::vector<std::shared_ptr<MaterialAsset>>* Slots;
    const VisibleIndexBuffer* Visible; // an outfit piece's uncovered triangles (OutfitHideTag)
    const glm::mat4* Xform;
    bool TwoSided = false;  // drawn without back-face culling (Cast Shadows: Two Sided)
    int LayerFirst = 0;     // the sun's single-pass cascades: its first cascade and how many in a row
    int Layers = 1;
};
// Draws the casters through a depth program (ShadowDepth.vert.glsl) whose uLightViewProj (or cascade matrices) the
// caller set. Copies of an unskinned, whole model with the same materials, sidedness and cascades draw as one
// instanced draw when GPU instancing is on; the rest one by one with uModel. `firstLoc`: the program's uCascadeFirst
// when drawing the sun's layered cascades, else -1. Returns the draw calls issued (one per model, not per mesh).
int DrawDepthCasters(Shader& shader, const std::vector<DepthCaster>& casters, int firstLoc = -1);
