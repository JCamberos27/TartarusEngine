#pragma once

#include <cstdint>

// The editor host owns ImGui, the renderer, scene data, undo history, and window lifetime.
// Reloadable editor modules describe their UI through this deliberately small callback surface.
// That keeps an in-flight scene safe when a DLL is replaced.
constexpr std::uint32_t kEditorModuleAPIVersion = 1;

struct EditorModuleHostAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*DrawStatusPanel)(const char* title, const char* message, const char* accent) = nullptr;
};

struct EditorModuleAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Draw)(const EditorModuleHostAPI& host) = nullptr;
};

using GetEditorModuleAPIFn = const EditorModuleAPI* (*)();
