#pragma once
// The decal editor tools' state (EditorLayer_DecalLibrary.cpp, EditorLayer_DecalTools.cpp): the
// library index, the active tool's brush and ghost, the stamp pops. One per EditorLayer, made on
// first use (EditorLayer::DecalEd). Private to those two files.
#include "DecalTools.h"
#include "../Game/Components.h"
#include "../Renderer/DecalPalette.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct DecalEditorState {
    // --- Library -----------------------------------------------------------------------------
    struct Entry {
        std::string Path;   // absolute, forward slashes: what the Asset Browser's payload carries
        std::string Rel;    // project-relative: what settings and palettes store
        std::string Name;   // "Tag_A": the file's stem without "DA_"
        std::string Search; // lower case: name, category and tags
        std::string Tags;   // as the .decal lists them, for the hover card
        std::string Albedo; // absolute albedo path, "" when it has none
        int Category = 0;   // index into Cats
        bool Mask = false, Invert = false; // luma / inverted-luma opacity: previews as paint over the swatch
        glm::vec3 Base{1.0f};
        glm::vec3 Size{1.0f, 1.0f, 0.25f};
        int Tier = 2048;
        uint8_t Channels = 0; // bit per albedo, normal, roughness, metal, ao, emissive
        bool Favorite = false;
    };
    struct Category {
        std::string Path; // "Graffiti/Alphabets/Tag"
        std::string Name; // "Tag"
        int Parent = -1;
        int Depth = 0;
        int Count = 0;    // decals in it and below it
        int End = 0;      // one past the last category in its subtree (the list is depth-first)
        bool Open = false;
    };
    std::vector<Entry> Entries;
    std::unordered_map<std::string, int> ByPath; // Entry::Path -> index into Entries (kept with Entries once the scan is done)
    std::vector<Category> Cats;
    std::vector<std::filesystem::path> ScanQueue; // .decal files not read yet
    size_t ScanAt = 0;
    bool ScanStarted = false, ScanDone = false;
    bool Dirty = true;            // the visible list needs rebuilding
    std::vector<int> Visible;     // entry indices after the category + search filter
    static constexpr int kAllCategories = -1, kFavorites = -2, kRecent = -3;
    int SelectedCategory = kAllCategories;
    int CascadeKey = 0;           // changes when the visible list changes shape: the tiles cascade in again
    char Search[96] = {};
    uint32_t SearchHash = 0;
    int WallTone = 1;             // the swatch behind the previews: 0 dark, 1 mid, 2 light

    // Palettes found under assets/Decals.
    std::vector<std::shared_ptr<DecalPalette>> Palettes;
    int ActivePalette = -1;       // index into Palettes, -1 none
    char PaletteName[48] = {};    // the "Save favourites as palette" popup

    // The tile under the cursor and how long, for the hover card.
    int HoverEntry = -1;
    double HoverSince = 0.0;
    int PreviewEntry = -1;        // the card being shown (stays through its fade-out)
    uint32_t WallTex = 0;         // the plaster swatch's GL texture (made on first use, lives for the process)

    // --- Tools -------------------------------------------------------------------------------
    std::string PlacePath;        // absolute .decal: the Place tool's decal, the brush's when no palette / category
    float PlaceSpin = 0.0f;       // radians, scroll turns it
    float PlaceScale = 1.0f;      // Shift+scroll
    struct AssetInfo { glm::vec3 Size{1.0f, 1.0f, 0.25f}; bool Ok = false; };
    std::unordered_map<std::string, AssetInfo> Assets; // by absolute path; read once (DecalEdit::AssetChanged drops one)
    std::unordered_map<std::string, std::string> Resolved; // palette entry path -> ProjectPaths::Resolve of it

    DecalTools::BrushSettings Brush;
    DecalTools::BrushStroke Stroke;
    bool Stroking = false, Erasing = false;
    int StrokeCount = 0;          // decals this stroke placed or erased
    uint32_t StrokeSeed = 0x2F6Eu;
    entt::entity PaintGroup = entt::null; // "Decal Paint"
    std::vector<entt::entity> Scratch;    // erase candidates
    std::vector<int> CategoryPool;        // entries the brush draws from when its source is a category
    std::string PoolFor;                  // the category the pool was built for
    int PoolEntries = -1;
    int PlaceSeed = 1;                    // the next stamp's seed, so the ghost shows the variant that lands

    // The cursor on a surface (the tool's raycast, once a frame).
    struct Cursor {
        bool Valid = false;
        glm::vec3 Point{0.0f}, Normal{0, 1, 0}, Right{1, 0, 0};
        entt::entity Receiver = entt::null;
    } Cur;

    // What the renderer draws as the ghost (main.cpp hands it to DecalSystem).
    struct Ghost {
        bool Active = false;
        DecalComponent Comp;
        glm::mat4 World{1.0f};
    } GhostView;

    // Stamp pops: a ring on the surface and the ghost settling.
    struct Pop {
        glm::vec3 Point{0.0f}, Normal{0, 1, 0}, Right{1, 0, 0};
        float RadiusM = 0.5f;
        bool Live = false;
    };
    static constexpr int kPops = 6;
    Pop Pops[kPops];
    int NextPop = 0;
    int LastPop = -1;             // the pop the ghost is settling from

    bool GhostWas = false, HudWas = false; // last frame, for the fade toggles

    // Library hearts popping.
    int HeartPops[4] = {-1, -1, -1, -1};
    int NextHeart = 0;

    // The radial's slots, built when it opens with the brush active.
    struct RadialSlot {
        enum Kind : int { Palette, Category, Recent } What = Recent;
        int Index = 0;            // palette / category / entry index
        std::string Path;      // Category / Recent: the category's or the decal's path (the indices shift when the library re-sorts)
        std::string Label;     // short, for the wedge
        std::string FullName;  // for the ring's centre while hovered
        const char* Icon = "";
        bool On = false;
    };
    std::vector<RadialSlot> Radial;

    // --- Auto-grime --------------------------------------------------------------------------
    DecalTools::GrimeSettings Grime;
    int GrimeSeed = 1;
    uint32_t GrimeKinds = DecalSites::All;
    bool GrimeReplace = true;
    std::string GrimeStatus;      // the last run's result line
    std::vector<DecalTools::GrimeSite> GrimeSites; // scratch, reused
};

// Shared by EditorLayer_DecalLibrary.cpp (which defines them) and EditorLayer_DecalTools.cpp.
namespace DecalEdit {
void BeginScan(DecalEditorState& ed);      // lists the .decal and .decalpalette files (once)
void ScanStep(DecalEditorState& ed);       // reads a slice of them
int FindEntry(const DecalEditorState& ed, const std::string& absPath); // -1 when not indexed
int SourceKind(const DecalEditorState& ed);                            // 0 a decal, 1 a category, 2 a palette
void NoteRecent(DecalEditorState& ed, const std::string& absPath);     // most recently used first
void SetPalette(DecalEditorState& ed, int index);                      // -1 none
void RefreshPool(DecalEditorState& ed);                                // CategoryPool for the brush's category
void SaveToolSettings(const DecalEditorState& ed);                     // brush + grime values -> preferences
// A .decal was written or created (a save, a duplicate, a Graffiti bake): drops what was cached about it
// and brings the library's entry for it up to date (adding it, in order, when it is new).
void AssetChanged(DecalEditorState& ed, const std::string& absPath);
} // namespace DecalEdit
