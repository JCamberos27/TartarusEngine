#include "TerrainSystem.h"

#include "Components.h"
#include "World.h"
#include "Model.h"
#include "TerrainGrid.h"
#include "Log.h"

#include <cstdio>
#include <cstring>

namespace TerrainSystem {

std::uint64_t Key(const TerrainComponent& t) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(&t.SizeX, sizeof(float)); mix(&t.SizeZ, sizeof(float));
    mix(&t.ResX, sizeof(int)); mix(&t.ResZ, sizeof(int));
    mix(&t.HeightMin, sizeof(float)); mix(&t.HeightMax, sizeof(float)); mix(&t.UVTile, sizeof(float));
    const size_t n = t.Heights.size();
    mix(&n, sizeof(n));
    // Every 61st character plus the tail: any regenerated terrain differs somewhere in these.
    for (size_t i = 0; i < n; i += 61) mix(&t.Heights[i], 1);
    if (n) mix(t.Heights.data() + (n > 64 ? n - 64 : 0), n > 64 ? 64 : n);
    return h;
}

void Sync(World& world) {
    auto view = world.Registry.view<TerrainComponent, RenderableComponent>();
    for (auto [e, t, rc] : view.each()) {
        const std::uint64_t key = Key(t);
        if (key != t.BuiltKey || !t.Built) {
            t.BuiltKey = key;
            t.Built.reset();
            auto grid = std::make_shared<TerrainGrid>();
            if (TerrainGrid::Decode(t.ResX, t.ResZ, t.SizeX, t.SizeZ, t.HeightMin, t.HeightMax, t.Heights, *grid)) {
                t.Grid = grid;
                char path[64];
                std::snprintf(path, sizeof(path), "primitive://terrain#%016llx", (unsigned long long)key);
                t.Built = Model::CreateTerrain(*grid, t.UVTile, path);
            } else {
                t.Grid.reset();
                if (!t.Heights.empty())
                    Log::Warn("Terrain: the heights don't match Res X x Res Z (" + std::to_string(t.ResX) + " x " +
                              std::to_string(t.ResZ) + ") - drawn flat.");
            }
        }
        if (t.Built && rc.ModelRef != t.Built) rc.ModelRef = t.Built;
    }
}

std::shared_ptr<const TerrainGrid> Find(const World& world, glm::vec3& outOrigin, std::uint64_t* outKey) {
    auto view = world.Registry.view<const TerrainComponent>(entt::exclude<InactiveTag>);
    for (auto [e, t] : view.each()) {
        if (!t.Grid) continue;
        const glm::mat4 m = world.GetCachedWorldTransform(e);
        outOrigin = glm::vec3(m[3]);
        if (outKey) *outKey = t.BuiltKey;
        return t.Grid;
    }
    return nullptr;
}

} // namespace TerrainSystem
