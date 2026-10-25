#pragma once
#include <entt/entt.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Wardrobe.h" // Wardrobe::Gender

namespace OutfitSystem { struct Catalog; }

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
    void Forget() { m_AppliedFor = entt::null; m_Dressing = m_Ahead = false; m_Warm.clear(); m_WarmAt = 0; }

    struct Look { Wardrobe::Gender Sex = Wardrobe::Gender::Male; std::string Race; };

private:
    // One of the three "next" swaps planned and its loads asked for (`step` 0 the next outfit, 1 the next style, 2 the
    // next look): one a frame, so no frame plans all three.
    void LoadAhead(World& world, AssetLibrary& assets, entt::entity root, int step);
    void WarmSome(AssetLibrary& assets);
    // The player's wardrobe and its looks, worked out when the wardrobe changes - not every frame.
    std::string m_WardrobePath;
    std::shared_ptr<const OutfitSystem::Catalog> m_Catalog;
    std::vector<Look> m_Looks;
    std::vector<std::string> m_Warm; // the wardrobe still to start loading (WarmSome, a slice a frame)
    std::size_t m_WarmAt = 0;
    std::string m_HoldText;          // the pill's last count (rebuilt only when the count moves)
    int m_HeldDone = -1, m_HeldTotal = -1;
    entt::entity m_AppliedFor = entt::null;
    unsigned m_NextSeed = 0;  // the next F8's outfit, drawn (and loading) ahead
    bool m_Dressing = false;  // a swap is waiting on its loads: the pill counts them
    bool m_Ahead = false;     // LoadAhead is due once the swap has landed
    int m_AheadStep = 0;      // ... which of its steps comes next
    bool m_LookSwap = false;  // the swap waiting is F7's (the pill says Look), else F8's
};
