#pragma once
#include <string>

struct GLFWwindow;

namespace FileDialog {
    // Seeds the folder the first dialog opens in (call once at startup with the project folder).
    // After that, each dialog remembers the folder of the file last picked. No-op if a folder
    // has already been remembered, or if `dir` isn't a real directory.
    void SetDefaultDirectory(const std::string& dir);

    // Opens the native Windows "Open File" dialog. `filter` uses the Win32 format,
    // e.g. "Model Files\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0".
    // Returns the selected path, or an empty string if the user canceled.
    std::string OpenFile(const char* filter, GLFWwindow* owner);

    // Opens the native Windows "Save File" dialog. `defaultExt` (no leading dot, e.g. "json")
    // is appended automatically if the typed filename doesn't already have an extension.
    // Returns the chosen path, or an empty string if the user canceled.
    std::string SaveFile(const char* filter, const char* defaultExt, GLFWwindow* owner);
}
