#pragma once

#include <string>

class AssetLibrary;

// `--outfit-audit [wardrobe] [report.csv]` checks a wardrobe two ways:
// - the rules: every outfit preset under assets/Characters/Outfits goes through them unchanged, and 5000
//   Randomize outfits per gender come out whole (a top, pants, shoes), with nothing the rules must take
//   off and no style clash;
// - clipping (`geometry`, skipped by `--outfit-rules`): every pair of pieces the rules let one character
//   wear where one is over the other (Wardrobe::Hides), in bind pose with the game's hiding: how many of
//   the lower piece's vertices still poke through (OutfitCoverage::PokeDepth). Needs GL.
// Returns the outfits with problems plus the pairs that clip.
namespace OutfitAudit {
int Run(AssetLibrary& assets, const std::string& wardrobe, const std::string& csvPath, bool geometry = true);
}
