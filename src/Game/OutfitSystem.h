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
class Model;
class World;

// Character outfits in the scene: builds, swaps and removes the Outfit Piece
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
// The worn race's Grip Inset (m) and Finger Curl Extra (degrees) on `root`'s Character Outfit, from a wardrobe already
// loaded (no file is read here: it runs every frame); zero when none is.
void RaceGrip(const World& world, entt::entity root, float& inset, float& curl);
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
// Whether the next UpdatePending(world) is about to change it: a waiting change whose assets have all loaded,
// or an outfit whose body it hasn't checked against the wardrobe yet (RefreshBodies). The editor journals
// an edit made without an undo step before then (EditorLayer::JournalBeforeOutfitLanding).
bool LandingDue(const World& world);
// Goes up each time UpdatePending changes a world: a waiting change landing, or a stale body being rebuilt. Neither
// is an edit of its own - one finishes an edit made frames earlier, the other brings a scene just opened up to
// date - so the editor folds them into its undo baseline (EditorLayer::BeginGlobalUndoFrame).
unsigned LandedGeneration();
// Rebuilds (through Submit) every outfit whose body parts aren't what its wardrobe now builds for what it
// wears - a scene saved before the wardrobe's body changed. Each outfit is checked once per wardrobe
// generation; UpdatePending calls this.
void RefreshBodies(World& world, AssetLibrary& assets);
// The same for one outfit, now (Apply: may load on the spot): for a body about to be read, like a soldier built from
// its prefab. True if it was rebuilt.
bool RefreshBody(World& world, AssetLibrary& assets, entt::entity root);
// Drops every waiting change for `world`: its registry is being replaced (scene load, undo, redo, Stop).
void CancelPending(const World& world);
// Puts `item` (a model path; "" = take it off) in `slot`.
Result Equip(World& world, AssetLibrary& assets, entt::entity root, const std::string& slot, const std::string& item);
// Switches gender, swapping each item for the other gender's cut of it where the wardrobe has one.
Result SetGender(World& world, AssetLibrary& assets, entt::entity root, Wardrobe::Gender gender);
Result SetRace(World& world, AssetLibrary& assets, entt::entity root, const std::string& race);
// A random outfit (and race, and colourways) for the current gender; the Character Outfit's Locks are kept.
// Dressed in one style (Wardrobe::Randomize): `style` names it, "" = picked by the wardrobe's weights.
Result Randomize(World& world, AssetLibrary& assets, entt::entity root, std::uint32_t seed,
                 const std::string& style = std::string());

// --- Loading ahead (F7 / F8: PlayerLook) -----------------------------------------------------------
// The outfit Randomize(root, seed, style) would dress `root` in, without dressing it: the same draw.
Wardrobe::Request PlanRandom(World& world, AssetLibrary& assets, entt::entity root, std::uint32_t seed,
                             const std::string& style = std::string());
// What `request` would put on `root`, its models and materials, started loading in the background; nothing changes.
// When the change is asked for, it lands in a frame. Returns how many loads were asked for (already loaded ones count).
int Preload(World& world, AssetLibrary& assets, entt::entity root, const Wardrobe::Request& request);
// Every item the wardrobe has for `gender` (and that body's race heads and parts) started loading in the background.
int PreloadWardrobe(AssetLibrary& assets, const Catalog& cat, Wardrobe::Gender gender);

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

// Keeps every outfit's hidden vertices in step with its pieces (OutfitHideTag): what pokes through a
// layer worn over it (Wardrobe::LayerOf - body, head, shirts, hair ...). Call once per frame, after the
// world transform cache is rebuilt; it only works when an outfit's pieces changed (an edit, an undo, a
// scene load), and remembers each pair. Also ranks the pieces for drawing (OutfitLayerTag).
void UpdateHiding(World& world);
// OutfitLayerTag: how much nearer the camera each layer a piece is worn over draws it, and the most layers
// counted. Cloth sways a few millimetres out through what's over it; much more and a hand resting on a
// jacket would sink into it.
constexpr float kLayerPull = 0.004f;
constexpr int kMaxLayerRank = 3;

// Rigid head wear's fit (Wardrobe::ItemOverride::Fit): `model` scaled by `fit` about its bounds' centre, in
// its own space. Identity for a fit of 1 (or 0, not looked up yet).
glm::mat4 FitMatrix(const Model& model, float fit);

// Rigid head wear (a static mesh in a head-attached slot, like a balaclava): no skeleton of its own, so each
// frame it's placed by the head bone's skinning matrix - modelled in the head's bind pose, that's where it sits.
// Once per frame after the animation (and the first-person body's late pose), before the transform cache.
void UpdateAttachments(World& world);

// The locked slots of a Locks string ("Hair, Top").
std::vector<std::string> ParseLocks(const std::string& locks);

} // namespace OutfitSystem
