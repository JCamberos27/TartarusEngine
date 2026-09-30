#pragma once

#include <string>
#include <vector>

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

// `--outfit-cost [scene.json ...]`: what the wardrobe's models cost to draw (triangles, vertices, meshes,
// bones per model, by slot) and, for each scene (default: scenes/OutfitTest/*.json), the characters'
// pieces, draws and triangles and how much of that skin hiding throws away. The textures' side is
// tools/quantum/audit_quantum_assets.py. Returns 0, or -1 when the wardrobe can't be read.
// `--outfit-selftest`: drives the OutfitSystem API (Apply, Submit, Equip, SetGender/Race, Randomize, colourways,
// presets, AdoptExisting, hiding, CancelPending) on the real wardrobe and checks each result. Needs GL (it
// builds characters). Returns the failures, or -1 when the wardrobe can't be read.
int RunSelfTest(AssetLibrary& assets, const std::string& wardrobe);

int RunCost(AssetLibrary& assets, const std::string& wardrobe, const std::vector<std::string>& scenes);
}
