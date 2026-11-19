#pragma once
// OutfitSystem's helpers shared by its three files (OutfitSystem.cpp, OutfitPending.cpp, OutfitHiding.cpp). Not for
// anything else: use OutfitSystem.h.
#include "OutfitSystem.h"
#include "AssetLibrary.h"

#include <entt/entt.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class World;

namespace OutfitSystem::detail {

std::string Lower(std::string s);
// Project-relative, forward slashes.
std::string Rel(const std::string& path);
bool Exists(const std::string& rel);
std::shared_ptr<const Catalog> CatalogFor(World& world, AssetLibrary& assets, entt::entity root, Result& r);
entt::entity DriverEntity(const World& world, entt::entity root);
void LinkFollowers(World& world, entt::entity root, entt::entity driver);
// Bumped whenever a wardrobe is (re)loaded, so what caches one knows to look again.
int& WardrobeGeneration();
std::shared_ptr<const Wardrobe::Wardrobe> LayerWardrobe(const std::string& path);
// (OutfitPending.cpp) Asks for `rel`'s material when the file is there.
void RequestMaterial(AssetLibrary& assets, const std::string& rel, std::vector<AssetLibrary::AsyncHandle>& out);
// (OutfitPending.cpp) What runs once a change's assets have loaded.
using Then = std::function<void(World&, AssetLibrary&, entt::entity)>;
// (OutfitPending.cpp) Starts loading what `request` needs from `cat`; the tickets to wait on (notes: what's missing).
std::vector<AssetLibrary::AsyncHandle> Prefetch(World& world, AssetLibrary& assets, entt::entity root, const Catalog& cat,
                                                const Wardrobe::Request& request, std::vector<std::string>& notes);
// (OutfitPending.cpp) `after` now if every ticket is ready, else once they are (UpdatePending).
void WhenReady(World& world, AssetLibrary& assets, entt::entity root, std::vector<AssetLibrary::AsyncHandle> tickets, Then after);
// (OutfitPending.cpp) What the next change builds on: the one still waiting to load, else what's worn now.
Wardrobe::Request BaseRequest(const World& world, entt::entity root);

} // namespace OutfitSystem::detail
