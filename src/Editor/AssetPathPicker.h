#pragma once
// A field that holds a project file's path (or a folder) is picked, never typed: a dropdown of
// the project's matching files with a search box at its top, a Browse... button for anything
// outside the list, and drops from the Assets panel. Host-side (EditorLayer*.cpp); the list is
// a cached scan of the project folder (Library/ skipped), refreshed every few seconds.

#include <string>

struct GLFWwindow;

struct AssetPathPickerOptions {
    // Lower-case extensions to list, e.g. {".png", ".jpg", nullptr}. nullptr = any file.
    const char* const* Extensions = nullptr;
    // Pick a folder instead of a file (the dropdown lists the project's folders).
    bool Folder = false;
    // Also accept this Assets-panel drag payload ("ASSET_TEXTURE_PATH", "ASSET_MODEL_PATH",
    // "ASSET_SOUND_PATH", "ASSET_FILE_PATH"...). nullptr = no drop target.
    const char* DragPayload = nullptr;
    // Offer an entry that clears the path, and what it reads as.
    bool AllowNone = true;
    const char* NoneLabel = "(none)";
    // The Browse... dialog's filter (Win32 "Name\0*.ext;*.ext\0\0" form). nullptr = all files.
    const char* DialogFilter = nullptr;
    // Paths are stored project-relative when the file is inside the project; absolute otherwise.
    // Set to keep absolute paths always.
    bool KeepAbsolute = false;
    // The dialog's owner window (centres it on the editor). May be null.
    GLFWwindow* Owner = nullptr;
    // Don't list the project's files / folders (when none of them is a valid choice, e.g. a build
    // output folder, which must be outside the project): the dropdown offers only the none entry
    // and the Browse dialog.
    bool BrowseOnly = false;
};

// Draws the picker at the current width (SetNextItemWidth applies to the whole widget: the
// dropdown plus its Browse button). Returns true on the frame `path` changes.
bool AssetPathPicker(const char* id, std::string& path, const AssetPathPickerOptions& options);

// Common extension lists.
namespace AssetExts {
extern const char* const Images[];   // .png .jpg .jpeg .tga .bmp .hdr .exr
extern const char* const Hdri[];     // .hdr .exr
extern const char* const Models[];   // .fbx .gltf .glb .obj .dae
extern const char* const Sounds[];   // .wav .mp3 .ogg .flac
extern const char* const Materials[];   // .mat
extern const char* const Animations[]; // .fbx .gltf .glb .dae (clip sources)
}
