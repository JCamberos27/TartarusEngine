#pragma once
#include <string>
#include <vector>

// File-level helpers for bringing outside files into the project: which files the importer
// handles, and copying a whole dropped folder in with its layout intact. No GL, no editor state,
// so the unit tests drive them against scratch folders.
namespace AssetImport {

// "model" / "texture" / "sound" / "prefab" for a file the importer loads, "" otherwise. A
// dropped folder queues only these; everything else in it (.bin, .mtl, .mat, readmes) is copied
// along as companion data.
std::string ImportKind(const std::string& path);

// Authoring and archive files a dropped folder leaves behind (.blend, .spp, .psd, .zip, ...):
// the engine can't load them, and they are often most of an asset pack's size.
bool IsSourceOnlyFile(const std::string& path);

// What a texture holds, guessed from the map-type word at the end of its file name, the way
// asset packs name them: "Crate_Normal.png" / "Door_Nm.tga" -> Normal, "Crate_Roughness.png",
// "_Metallic", "_AO", "_Height", "_Mask" -> Data (linear), anything else -> Color (sRGB).
// Trailing qualifiers are skipped: "Arms_Unity_Normal.1003.png", "Sofa_Normal_OpenGL_4K.png".
enum class TextureKind { Color, Normal, Data };
TextureKind GuessTextureKind(const std::string& path);

struct FolderCopy {
    std::string Folder;             // the new folder in the project; empty on failure
    std::vector<std::string> Files; // every file copied, absolute
    int Skipped = 0;                // source-only files left behind
    std::string Error;
};

// Copies `sourceDir` to `destParent`/<its name>, adding " (2)", " (3)"... when that folder
// exists, and keeps the relative layout of everything under it, so a model still finds the
// textures its pack keeps in a sibling folder. Each copied file the importer loads is recorded
// as the editor's own write, so the project watcher doesn't import it a second time.
FolderCopy CopyFolderInto(const std::string& sourceDir, const std::string& destParent);

} // namespace AssetImport
