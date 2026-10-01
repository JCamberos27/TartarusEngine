#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

// Character outfits (docs/CHARACTER_OUTFITS.md): what a character pack offers and how its pieces go
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
    int Layer = 0;                     // how far out it's worn (body parts are 0): it hides lower layers' pokes
    bool Hides = true;                 // false for see-through items (hair cards, beards, glasses): never hide
};

// A slot's layer changed for some of its items: a tucked shirt goes under the pants, boots over them,
// a hood that's up over the hair (`Over`: slots it hides whatever their layer).
struct LayerRule {
    std::string Slot;
    std::vector<std::string> NameHasAny; // empty = every item in the slot
    std::vector<std::string> NameLacksAll;
    int Layer = -1;                      // -1 = the slot's
    std::vector<std::string> Over;
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

// Which items a rule is about: in `Slot` (empty = any), whose file name has one of `NameHasAny` and none
// of `NameLacksAll`, with a material named like one of `MaterialHasAny`, carrying one of `Tags` and none
// of `LacksTags`. An empty list doesn't narrow it.
struct Match {
    std::string Slot;
    std::vector<std::string> NameHasAny, NameLacksAll, MaterialHasAny, Tags, LacksTags;
};

// Tags put on the items a match finds: styles ("Formal") and kinds the other rules use ("BuiltInTop").
struct TagRule {
    Match When;
    std::vector<std::string> Tags;
};

// Two items that don't go together. `B` is taken off when both are asked for. A `Soft` one (a style
// clash: a suit jacket with flip-flops) is only kept out of Randomize; asked for by hand, it stays.
struct ExcludeRule {
    Match A, B;
    bool Soft = false;
    std::string Why; // the Inspector's note
};

// A look Randomize dresses a character in: how likely each slot is filled, and only items tagged with
// the style's name (or with no style tag at all).
struct Style {
    std::string Name;
    float Weight = 1.0f;
    int Gender = -1;                    // -1 both, else only this Gender
    std::map<std::string, float> Fill;  // slot -> chance it's filled
    float DefaultFill = 0.0f;           // for slots not listed
};

// A slot emptied by an item (a hat that brings its own hair, a jacket with its own shirt).
struct ClearRule {
    std::string Slot;
    std::vector<std::string> NameHasAny;
    std::vector<std::string> PathHasAny;
    std::vector<std::string> Tags; // or the item carries one of these
    std::string Clear;
};

// "Boots take the _Inboots pants": when `WhenSlot`'s item name contains `WhenNameHas`, the item in
// `Slot` is swapped for its sibling ending in `Suffix`; otherwise a sibling with the suffix is swapped
// back for the plain one.
struct PairRule {
    std::string WhenSlot, WhenNameHas, Slot, Suffix;
    std::string WhenNameLacks; // ... unless it also contains this ("Boots_Inboots" go under plain pants)
};

struct ItemOverride {
    std::string Path;       // under the wardrobe's root
    bool Hidden = false;    // not offered
    std::string Slot, Name; // "" = inferred
    int Gender = -1;        // -1 = inferred
    // Rigid head wear drawn this much bigger about its own centre (OutfitSystem::FitMatrix): a balaclava
    // modelled a little small for the head it's worn on (the female one pokes the back of the head out).
    float Fit = 1.0f;
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
    std::vector<LayerRule> Layers;
    std::vector<TagRule> TagRules;
    std::vector<ExcludeRule> Excludes;
    std::vector<Style> Styles;
    std::vector<std::string> RandomOrder; // the order Randomize fills slots in (empty = Slots' order)
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
    std::vector<std::string> Tags;    // from the wardrobe's tag rules (TagRule)
    std::vector<std::string> Materials; // its remapped .mat paths (the colourway sources)
};

// An item's ItemOverride::Fit (1 when it has none). `path` project-relative.
float FitOf(const Wardrobe& w, const std::string& path);

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

// --- Layers -----------------------------------------------------------------------------------

// A slot's layer and whether it hides, when the .wardrobe doesn't say: body 0, shoes, tucked tops, pants,
// tops, outerwear, collars, bags and wrists, then hair, beards and glasses (which drape over it all but
// hide nothing), then hats. Wrists hide nothing either: a watch or bead bracelet shows the skin through
// its gaps, and the arm posed away from the bind pose slides the hidden band out from under it.
int DefaultLayer(const std::string& slot);
bool DefaultHides(const std::string& slot);

// Where a worn piece sits: its layer (after the rules), whether it hides, the slots it hides regardless.
struct Layering {
    int Layer = 0;
    bool Hides = false;
    std::vector<std::string> Over;
};
Layering LayerOf(const Wardrobe& w, const std::string& slot, const std::string& itemPath, bool bodyPart);
// Whether `over` hides the parts of `under` that poke through it (body parts never hide anything).
bool Hides(const Layering& over, const std::string& underSlot, const Layering& under, const std::string& overSlot = {});

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
    std::vector<std::string> Notes;   // what the rules did, for the Inspector ("Feet -> ShoeFeet")
    std::vector<std::string> Dropped; // slots asked for that the rules took off (clears, excludes)
    std::vector<std::string> Clashes; // soft excludes left on ("Jacket Classic Tie with Flip Flops")
};

// --- Compatibility ----------------------------------------------------------------------------

bool Matches(const Match& m, const Item& item);
bool HasTag(const Item& item, const std::string& tag);
// Whether `a` and `b` can't be worn together: an exclude between them (soft ones only if `soft`), or
// one clears the other's slot.
bool Conflicts(const Wardrobe& w, const Item& a, const Item& b, bool soft = true);
// Whether `style` may dress a character in `item`: tagged with it, or with no style at all.
bool InStyle(const Wardrobe& w, const Item& item, const Style& style);
const Style* FindStyle(const Wardrobe& w, const std::string& name);

// A random outfit for gender `g` from `seed`: a style (by weight, for the gender; `style` names one,
// "" = any), then slot by slot in RandomOrder, each filled by chance with an item of the style that
// isn't a variant and goes with everything chosen so far. The slots in `keep` stay as `base` has them.
// The chosen style's name goes to `styleOut`.
Request Randomize(const Wardrobe& w, const std::vector<Item>& catalog, const Request& base, unsigned seed,
                  const std::vector<std::string>& keep = {}, const std::string& style = std::string(),
                  std::string* styleOut = nullptr);

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
