#pragma once

#include "Core/FrameLoads.h"

#include <initializer_list>
#include <string>
#include <vector>

class World;
struct SceneDrawItem;

// --perf-bench asset cost table: TARTARUS_ASSET_COST=<file.csv> appends, per scene and phase, one row per model and
// LOD level drawn (triangles drawn per frame, averaged and worst over the phase), one per live texture (GPU MiB,
// format, uploaded size, mips) and what each file took to load (the FrameLoads stream the hitch report reads, summed
// per file since the scene started loading). Sorted by cost within each kind. A measuring tool only: off by default,
// and nothing here changes what is drawn or loaded.
namespace AssetCost {

bool Enabled();
// One view's draw lists after culling (SceneViewPass::Run). The Scene tab and the Game view are tallied apart; the
// edit phase reports the Scene tab, the play phases the Game view. Probe captures are not passed in.
void NoteView(World& world, bool editorView, std::initializer_list<const std::vector<SceneDrawItem>*> lists);
// The loads a frame took (PerfBench::TakeFrameMs), added to the scene's per-file totals.
void NoteLoads(const std::vector<FrameLoads::Event>& events);
// Appends the phase's rows to the CSV and clears the draw tally. The load totals run from the scene's load through
// its last phase (play-max), so each scene lists its own loads.
void WritePhase(const std::string& sceneFile, const char* mode);

// The LOD level `entity` draws for: the index among its LOD Group's first four children of the child it sits under,
// or -1 outside any LOD Group (or under a fifth child or later, which LOD never hides). Exposed for the unit test.
int LodLevelOf(const World& world, unsigned entity);

} // namespace AssetCost
