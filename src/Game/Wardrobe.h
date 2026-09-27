#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

// Character outfits (CHARACTER_OUTFITS.md): what a character pack offers and how its pieces go
// together. Pure - no scene, GL or file access - so every rule is unit tested; OutfitSystem does the
// scanning and the scene work.
//
// A pack is described by a `.wardrobe` file (JSON) holding only what its folders can't say: the body
// each gender is built from (parts, races), the slots and the folders that fill them, and the pairing
// rules. The items themselves - every model under the item folders - are found by scanning, so a new
// hoodie dropped into Clothing/Male/Tops shows up with no edit here.
namespace Wardrobe {

enum class Gender { Male = 0, Female = 1 };
const char* GenderName(Gender g);

// A place on the character one item fills (Top, Hair, Wrist L ...).
struct SlotDef {
    std::string Id;                    // saved on the piece (OutfitPieceComponent::Slot)
    std::string Label;                 // the Inspector's tab / row
    std::string Icon;                  // a short icon name the Inspector maps to a glyph
    std::vector<std::string> Folders;  // folder names (any depth under an item folder) whose models fill it
    std::string NameSuffix;            // and whose file name ends with this (before .fbx), if set - e.g. "_L"
    bool HeadAttached = false;         // rides on the head: shadow-only in first person, like the head
};

// A race: the head it wears and the skin its body parts take.
struct RaceDef {
    std::string Name;
    std::string Head;                           // head model (path under the wardrobe's root)
    std::string SkinSuffix;                     // appended to a skin material's name ("_Afro"); "" = the base skin
    std::map<std::string, std::string> Parts;   // body parts this race replaces (part -> model), usually none
    glm::vec3 Tone{0.8f, 0.6f, 0.5f};           // the Inspector's swatch colour
};

// How one gender's body is built.
struct BodyDef {
    std::vector<std::pair<std::string, std::string>> Parts; // part -> model, in order (Torso, Arms, Legs...)
    std::vector<RaceDef> Races;
    std::map<std::string, std::string> Alternates;           // name -> model, for rules ("ShoeFeet" -> feet cut for shoes)
};

// A body part hidden, or swapped for an alternate, while an item is worn.
struct CoverRule {
    std::string Slot;
    std::vector<std::string> NameHasAny;   // the item's file name contains one of these (empty = any)
    std::vector<std::string> NameLacksAll; // ... and none of these
    int Skin = -1;                         // -1 any; 0 the item carries no skin of its own; 1 it does
    std::vector<std::string> Hide;         // parts not built
    std::string Replace, With;             // part -> body alternate (BodyDef::Alternates)
};

// A slot emptied by an item (a hat that brings its own hair, a jacket with its own shirt).
struct ClearRule {
    std::string Slot;
    std::vector<std::string> NameHasAny;
    std::vector<std::string> PathHasAny;
    std::string Clear;
};

// "Boots take the _Inboots pants": when `WhenSlot`'s item name contains `WhenNameHas`, the item in
// `Slot` is swapped for its sibling ending in `Suffix`; otherwise a sibling with the suffix is swapped
// back for the plain one.
struct PairRule {
    std::string WhenSlot, WhenNameHas, Slot, Suffix;
};

struct ItemOverride {
    std::string Path;       // under the wardrobe's root
    bool Hidden = false;    // not offered
    std::string Slot, Name; // "" = inferred
    int Gender = -1;        // -1 = inferred
};

struct Wardrobe {
    std::string Name;
    std::string Root;                     // project-relative, e.g. assets/Characters/Quantum
    std::vector<std::string> ItemFolders; // under Root, scanned recursively for models
    std::vector<std::string> ExcludeFolders;
    std::vector<SlotDef> Slots;
    BodyDef Bodies[2];
    std::vector<std::string> SkinMaterials; // base skin material names (M_Quantum_Body ...)
    std::vector<CoverRule> Covers;
    std::vector<ClearRule> Clears;
    std::vector<PairRule> Pairs;
    std::string HatSlot = "Hat", HairSlot = "Hair"; // a hat takes the haircut fitted to it
    std::vector<ItemOverride> Items;

    const SlotDef* Slot(const std::string& id) const;
    const BodyDef& Body(Gender g) const { return Bodies[(int)g]; }
};

// Parses a .wardrobe document. False (and `error`) on malformed JSON or a missing body.
bool Parse(const std::string& jsonText, Wardrobe& out, std::string* error = nullptr);

// --- Items ------------------------------------------------------------------------------------

struct Item {
    std::string Path;                 // project-relative model path
    std::string Stem;                 // file name without extension
    std::string Name;                 // display name
    std::string Slot;
    Gender Sex = Gender::Male;
    bool Skin = false;                // carries skin of its own (a skin material among its materials)
    bool Variant = false;             // a cut made for another item (Bobcut_Cap, Jeans_Inboots): the rules pick it
    std::vector<std::string> Materials; // its remapped .mat paths (the colourway sources)
};

// The display name for a model file stem: "SKM_F_Hoodie_Zipper_Hood" -> "Hoodie Zipper Hood".
std::string PrettyName(const std::string& stem);
// The file name without folders and extension.
std::string Stem(const std::string& path);
// Whether `name` contains `token`, case-insensitively.
bool Contains(const std::string& name, const std::string& token);

// A skin material's base name for `matPath` ("M_Quantum_Body_Afro.mat" -> "M_Quantum_Body"), or ""
// when it isn't one of the wardrobe's skin materials (with any race's suffix).
std::string SkinBase(const Wardrobe& w, const std::string& matPath);

// Classifies one model found under the item folders. `materials` are its remapped .mat paths.
// False when it isn't offered (excluded folder, hidden override, no slot).
bool Classify(const Wardrobe& w, const std::string& path, const std::vector<std::string>& materials, Item& out);

// Marks the items the rules pick rather than the user: a haircut named for a hat, the pants cut a pair
// rule swaps in (when its plain cut exists too).
void MarkVariants(const Wardrobe& w, std::vector<Item>& items);

// --- Outfits ----------------------------------------------------------------------------------

// What the character asks for: a gender, a race and an item per slot (slot -> item path).
struct Request {
    Gender Sex = Gender::Male;
    std::string Race;
    std::map<std::string, std::string> Items;
};

// One piece to build: a body part or an item.
struct Piece {
    std::string Slot;          // part name (Torso ...) or item slot (Top ...)
    std::string Path;          // project-relative model
    bool BodyPart = false;
    bool HeadAttached = false;
};

struct Resolved {
    std::vector<Piece> Pieces;
    std::vector<std::string> Notes; // what the rules did, for the Inspector ("Feet -> ShoeFeet")
};

// The race called `name` for gender `g`, or the first one (nullptr if the body has none).
const RaceDef* FindRace(const Wardrobe& w, Gender g, const std::string& name);

// Applies the rules to a request: the gender's body with its race, the items for that gender, pairs
// (boots -> _Inboots pants, a hat -> its fitted haircut), clears and covers. `catalog` is every item.
Resolved Resolve(const Wardrobe& w, const std::vector<Item>& catalog, const Request& request);

// The material a slot should use for race `race`: a skin material becomes the race's variant (when
// `exists` says that file is there, else the base skin); anything else is returned unchanged.
std::string SkinMaterialFor(const Wardrobe& w, const RaceDef& race, const std::string& matPath,
                            const std::function<bool(const std::string&)>& exists);

// The colourways of material `matPath`: its folder's .mat files (`siblings`, any order), sorted by name.
std::vector<std::string> Colourways(const std::string& matPath, const std::vector<std::string>& siblings);

// The pieces to change to get from `current` (slot -> model path) to `desired`.
struct Diff {
    std::vector<Piece> Create;          // slots with no piece yet
    std::vector<Piece> Remodel;         // slots whose piece has another model
    std::vector<std::string> Destroy;   // slots no longer wanted
};
Diff MakeDiff(const std::map<std::string, std::string>& current, const std::vector<Piece>& desired);

} // namespace Wardrobe
