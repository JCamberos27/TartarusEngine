#pragma once

#include "Wardrobe.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class AssetLibrary;
class World;

// Character outfits in the scene (CHARACTER_OUTFITS.md): builds, swaps and removes the Outfit Piece
// children of an object with a Character Outfit component, from its wardrobe's catalog. The Inspector's
// outfit editor calls these (after PushUndo); a future in-game character creator can call them too.
namespace OutfitSystem {

// A wardrobe and every item found under its item folders.
struct Catalog {
    Wardrobe::Wardrobe W;
    std::vector<Wardrobe::Item> Items;
    std::string Path;

    const Wardrobe::Item* Find(const std::string& path) const;
    std::vector<const Wardrobe::Item*> ForSlot(const std::string& slot, Wardrobe::Gender g) const;
};

// The wardrobe at `path` (project-relative), scanned once and cached; `rescan` reads it again. Null
// (and `error`) when the file can't be read or parsed.
std::shared_ptr<const Catalog> LoadCatalog(AssetLibrary& assets, const std::string& path, bool rescan = false,
                                           std::string* error = nullptr);

// The outfit's pieces: slot -> child entity.
std::map<std::string, entt::entity> Pieces(const World& world, entt::entity root);
// What the character wears now, as a request (its gender, race and item pieces).
Wardrobe::Request CurrentRequest(const World& world, entt::entity root);

struct Result {
    bool Ok = false;
    std::string Error;
    std::vector<std::string> Notes; // what the wardrobe's rules did
    int Created = 0, Removed = 0, Changed = 0;
};

// Builds `request`: pieces are created, re-modelled or removed to match it, the race's skin is put on
// every skin material, and the Character Outfit's gender and race are set to the request's.
Result Apply(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request);
// Puts `item` (a model path; "" = take it off) in `slot`.
Result Equip(World& world, AssetLibrary& assets, entt::entity root, const std::string& slot, const std::string& item);
// Switches gender, swapping each item for the other gender's cut of it where the wardrobe has one.
Result SetGender(World& world, AssetLibrary& assets, entt::entity root, Wardrobe::Gender gender);
Result SetRace(World& world, AssetLibrary& assets, entt::entity root, const std::string& race);
// A random outfit (and race, and colourways) for the current gender; the Character Outfit's Locks are kept.
Result Randomize(World& world, AssetLibrary& assets, entt::entity root, std::uint32_t seed);

// A piece's colourways: one group per material the piece's model is remapped to.
struct ColourGroup {
    std::string Source;                 // the remapped .mat (project-relative)
    std::string Current;                // what the piece's slots using it hold now
    std::vector<std::string> Options;   // that material's folder
};
std::vector<ColourGroup> ColourGroups(const World& world, AssetLibrary& assets, entt::entity piece);
// Puts colourway `variant` on every slot of `piece` whose remapped material is `source`.
bool SetColourway(World& world, AssetLibrary& assets, entt::entity piece, const std::string& source,
                  const std::string& variant);

// Tags the root's existing children that are wardrobe pieces (by model path) as Outfit Pieces, and
// sets the Character Outfit's gender and race from the body found. Returns how many were tagged.
int AdoptExisting(World& world, AssetLibrary& assets, entt::entity root);

// Presets: the outfit (gender, race, items and their colourways) as a small JSON file.
bool SavePreset(const World& world, AssetLibrary& assets, entt::entity root, const std::string& path,
                std::string* error = nullptr);
Result LoadPreset(World& world, AssetLibrary& assets, entt::entity root, const std::string& path);

// Keeps every outfit's hidden skin in step with its pieces (OutfitHideTag): body parts under clothing,
// and a top under outerwear. Call once per frame, after the world transform cache is rebuilt; it only
// works when an outfit's pieces changed (an edit, an undo, a scene load), and remembers each pair.
void UpdateHiding(World& world);

// The locked slots of a Locks string ("Hair, Top").
std::vector<std::string> ParseLocks(const std::string& locks);

} // namespace OutfitSystem
