#pragma once

#include "Wardrobe.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
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
    // Sorted by name; worked out once per slot and gender (the editor's grid asks every frame).
    const std::vector<const Wardrobe::Item*>& ForSlot(const std::string& slot, Wardrobe::Gender g) const;

private:
    mutable std::map<std::pair<std::string, int>, std::vector<const Wardrobe::Item*>> m_SlotLists;
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
    // The change is waiting for its models and materials to load (see Submit); it lands in one
    // frame from UpdatePending, and the pieces stay as they are until then.
    bool Pending = false;
};

// Builds `request` now: pieces are created, re-modelled or removed to match it, the race's skin is put on
// every skin material, and the Character Outfit's gender and race are set to the request's. Loads whatever
// isn't in memory yet on the spot - it can stall for seconds. Everything below goes through Submit instead.
Result Apply(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request);
// Apply without the stall: starts loading the models and materials the request needs in the background
// (AssetLibrary's async loading) and applies it, in one frame, once they are all in memory. If they
// already are, it applies right away. A newer request for the same outfit replaces a waiting one.
// `then` runs after the change is applied (Randomize's colourways, a preset's colours).
Result Submit(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request,
              std::function<void(World&, AssetLibrary&, entt::entity)> then = nullptr);
// Whether `root` has a change waiting to load, and how many of its loads are done / asked for.
bool IsPending(const World& world, entt::entity root, int* done = nullptr, int* total = nullptr);
// Applies every waiting change whose assets have all loaded. Call once per frame, after
// AssetLibrary::PumpAsync and before UpdateHiding.
void UpdatePending(World& world, AssetLibrary& assets);
// Drops every waiting change for `world`: its registry is being replaced (scene load, undo, redo, Stop).
void CancelPending(const World& world);
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
// SetColourway once `variant` has loaded in the background (right away if it already has). `root` is
// the piece's outfit, whose waiting changes it joins (IsPending).
void SubmitColourway(World& world, AssetLibrary& assets, entt::entity root, entt::entity piece,
                     const std::string& source, const std::string& variant);

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
