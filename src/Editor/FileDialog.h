#pragma once
#include <string>

struct GLFWwindow;

namespace FileDialog {
    // Opens the native Windows "Open File" dialog. `filter` uses the Win32 format,
    // e.g. "Model Files\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0".
    // Returns the selected path, or an empty string if the user canceled.
    std::string OpenFile(const char* filter, GLFWwindow* owner);

    // Opens the native Windows "Save File" dialog. `defaultExt` (no leading dot, e.g. "json")
    // is appended automatically if the typed filename doesn't already have an extension.
    // Returns the chosen path, or an empty string if the user canceled.
    std::string SaveFile(const char* filter, const char* defaultExt, GLFWwindow* owner);
}
