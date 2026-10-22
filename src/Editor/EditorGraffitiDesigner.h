#pragma once
// The Graffiti Designer's state (decals Phase 3, docs/GRAFFITI_DESIGNER.md). EditorLayer holds one
// (m_Graffiti) and EditorLayer_GraffitiDesigner.cpp drives it: the panel, the canvas, live strokes,
// bakes, and spraying onto a wall in the Scene view. Host-only (editor), main thread.
#include "../Graffiti/GraffitiCompositor.h"
#include "../Graffiti/GraffitiPaint.h"
#include "../Graffiti/GraffitiPiece.h"
#include "EditorSound.h"
#include "EditorFileHistory.h"

#include <entt/entt.hpp>
#include <imgui.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The stamp picker popup (thumbnail grid by folder, recents, search). Folders are scanned from disk
// each time it opens; thumbnails come from the editor's shared AssetThumbnailLoader and are dropped
// again when the Designer closes.
struct StampPicker {
    struct Folder {
        std::string Name;
        std::vector<std::string> Files; // project-relative, sorted
        std::vector<std::string> Lower; // LowerAscii(BaseName(Files[i])), for the search
    };
    bool WantOpen = false;
    int Target = -1;                    // layer index the pick goes to, or -1: only the Stamp tool's next stamp
    int Tab = 1;                        // 0 = Recent, i + 1 = Folders[i]
    char Search[64] = "";
    std::vector<Folder> Folders;
    std::vector<std::string> Recent;    // newest first, this session
    std::vector<std::string> Requested; // absolute paths asked of the thumbnail loader
    std::unordered_set<std::string> RequestedSet; // the same, for the membership test
    struct Names { std::string Abs, Key; };
    std::unordered_map<std::string, Names> NameCache; // rel path -> absolute path and PathKey, resolved once
};

struct GraffitiDesigner {
    enum class Tool : int { Move = 0, Stamp, Text, Spray, Eyedropper, Erase, Count };

    std::shared_ptr<Graffiti::Piece> Piece; // the open recipe (null: none)
    Graffiti::Compositor Comp;
    Graffiti::Program Program;      // what the canvas shows (the text cascade's reveals folded in)
    bool NeedsBuild = true;         // the recipe changed: rebuild the program and re-render
    bool PreviewOnly = false;       // only the preview shader's inputs changed (relief, show-through): re-preview
    Graffiti::BuildCache Cache;     // each layer's built output, rebuilt only where its inputs changed
    bool Dirty = false;             // unsaved recipe edits: saved once no widget is held
    int Selected = -1;              // layer index
    std::vector<uint32_t> LayerIds; // a stable id per layer (the reorder glide), parallel to Piece->Layers
    uint32_t NextLayerId = 1;
    std::unordered_map<uint32_t, float> PrevRow; // each layer's row last frame

    // Tools and brush.
    Tool Active = Tool::Spray;
    Graffiti::Cap Cap = Graffiti::Cap::Fat;
    glm::vec3 Color{0.9f, 0.16f, 0.12f}; // sRGB
    float Pressure = 0.85f;              // 0..1, scaled by the pointer's speed when VelocityPressure
    float DistanceCm = 15.0f;            // the can's distance from the wall
    float Flow = 1.0f;
    float NibAngle = 45.0f;
    bool VelocityPressure = true;
    std::vector<glm::vec3> Recent;       // recently used colours, newest first
    std::string StampTexture = "assets/Decals/Graffiti/Shapes/T_Decal_GraffitiShape_001.png";
    StampPicker Picker;

    // Canvas view.
    float Zoom = 1.0f;
    ImVec2 Pan{0.0f, 0.0f};
    bool PanTracking = false;
    ImVec2 CanvasMin{0, 0}, CanvasMax{0, 0}; // the image's rect last frame (screen)

    // A live stroke (canvas or Scene view).
    bool Stroking = false;
    bool StrokeInViewport = false;
    int StrokeLayer = -1;
    Graffiti::Stroke Live;
    Graffiti::StrokeBuilder Builder;
    std::vector<Graffiti::Splat> Pending; // emitted, not yet due
    std::vector<uint32_t> PendingSeq;
    uint32_t NextSeq = 0;
    double StrokeStart = 0.0, LastSampleAt = -1.0, LastDripSound = -10.0;
    glm::vec2 LastUv{0.0f};
    ImVec2 LastMouse{0, 0};
    float LivePressure = 0.0f;
    EditorSound::LoopHandle Hiss = -1;

    // Move tool.
    bool Moving = false;
    glm::vec2 MoveOffset{0.0f};

    // Bake: GPU render + PBO readback (stage 1), then PNG encode and the .decal on a worker (stage 2).
    int BakeStage = 0;
    bool BakeAgain = false;
    bool BakeUndoable = false, BakeAgainUndoable = false; // the Bake button's bake is an undo step of its own
    std::future<uint64_t> Encoding; // the four PNGs' FNV-1a hash; 0 when writing failed
    uint64_t LastBakeHash = 0;
    std::vector<EditorFileHistory::State> BakeBefore; // the maps and .decal as they were, for the bake's undo step
    std::vector<EditorFileHistory::State> PendingJournal; // a finished bake's pre-images, journaled next frame
    std::vector<std::string> BakedPaths;                  // .decal paths written since the library last looked
    std::shared_ptr<Graffiti::Piece> BakePiece; // what the bake in flight writes: paths and settings taken when it started
    int Bakes = 0;
    double BakeDoneAt = -10.0;

    // The recipe file: its write time as we last wrote or read it (undo / redo rewrite it).
    long long Stamp = 0;
    double LastStampCheck = 0.0;

    // Scene-view spraying.
    bool ViewportSpray = false;
    entt::entity Target = entt::null; // the decal entity showing this piece
    std::vector<uint8_t> OccVisible;  // the live overlay's cells not hidden by geometry in front of the wall
    glm::mat4 OccKey{0.0f};           // view * projection * box the cells were worked out for
    const void* OccPiece = nullptr;   // ...and for which piece and target entity
    entt::entity OccTarget = entt::null;
    double OccAt = -10.0;
    double OverlayUntil = -1.0;       // keep the live overlay until the re-baked decal is resident (or this time)

    // Text: letters cascade in as they're typed.
    int CascadeLayer = -1, CascadeFrom = 0;
    double CascadeStart = -10.0;

    // Radial (hold Space over the canvas): caps and recent colours.
    bool RadialOpen = false, RadialWasOpen = false;
    ImVec2 RadialCenter{0, 0};
    int RadialHovered = -1;

    std::string Status;
};
