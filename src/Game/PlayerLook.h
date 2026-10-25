#pragma once
#include <entt/entt.hpp>

class World;
class AssetLibrary;

// F7 / F8 in Play (FKeyRow.h): the player's own body. F7 steps through every look the wardrobe has - each body's races
// (Male European ... Female Young), then the Skeleton - and Shift+F7 steps back. F8 dresses the player in a new random
// outfit of the current style, keeping the look; Shift+F8 moves to the next style (Casual, Sport ... then Bare, no
// clothes) and dresses in it. The choice is kept (EditorSettings FRowLook / FRowStyle / FRowOutfitSeed) and put on again
// at the start of every Play. To keep the swaps quick, the whole wardrobe starts loading in the background when Play
// starts, and each press already loads what the next press of each key will put on (the next outfit is drawn ahead);
// while a swap still waits on loads, the pill counts them.
class PlayerLook {
public:
    // Once a frame in Play. `keys`: the F-row may act (the game has the keys, not the Script IDE).
    void Update(World& world, AssetLibrary& assets, bool keys);
    // Play ended: the next Play's body gets the kept look again.
    void Forget() { m_AppliedFor = entt::null; m_Dressing = false; }

private:
    void LoadAhead(World& world, AssetLibrary& assets, entt::entity root);
    entt::entity m_AppliedFor = entt::null;
    unsigned m_NextSeed = 0;  // the next F8's outfit, drawn (and loading) ahead
    bool m_Dressing = false;  // a swap is waiting on its loads: the pill counts them
    bool m_Ahead = false;     // LoadAhead is due once the swap has been submitted
    bool m_LookSwap = false;  // the swap waiting is F7's (the pill says Look), else F8's
};
