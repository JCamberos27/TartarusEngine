#pragma once

#include <string>

class AssetLibrary;

// `--gen-outfit-scenes`: builds the outfit test scenes from the Quantum wardrobe, with the outfit system
// itself (OutfitSystem::Apply), and saves them under project/scenes/OutfitTest/:
// - Presets.json: every outfit preset under assets/Characters/Outfits/Quantum (the pack artist's 60);
// - Items.json: one character per item, on a plain base outfit, a row per slot and gender;
// - Randomized.json: eight Wardrobe::Randomize outfits per style and gender, with random colourways.
// Every character is a normal Character Outfit - select one and use the Inspector to re-roll it.
// Needs GL (models load through AssetLibrary). Returns 0 on success.
namespace OutfitTestScene {
int Generate(AssetLibrary& assets);
}
