#pragma once
#include <entt/entt.hpp>

class World;
class AssetLibrary;

// F7 / F8 in Play (FKeyRow.h): the player's own body. F7 steps through every look the wardrobe has - each body's races
// (Male European ... Female Young), then the Skeleton - and Shift+F7 steps back. F8 dresses the player in a new random
// outfit of the current style, keeping the look; Shift+F8 moves to the next style (Casual, Sport ... then Bare, no
// clothes) and dresses in it. The choice is kept (EditorSettings FRowLook / FRowStyle / FRowOutfitSeed) and put on again
// at the start of every Play.
class PlayerLook {
public:
    // Once a frame in Play. `keys`: the F-row may act (the game has the keys, not the Script IDE).
    void Update(World& world, AssetLibrary& assets, bool keys);
    // Play ended: the next Play's body gets the kept look again.
    void Forget() { m_AppliedFor = entt::null; }

private:
    entt::entity m_AppliedFor = entt::null;
};
