#pragma once

#include <cstdint>
#include <cstddef>

// The editor host owns ImGui, the renderer, scene data, undo history, and window lifetime.
// Reloadable editor modules describe their UI through this deliberately small callback surface.
// That keeps an in-flight scene safe when a DLL is replaced.
//
// Version history:
//   1 - status-panel placeholder only.
//   2 - ImGui context/allocator sharing, Log accessors, tooltip + save-dialog callbacks and the
//       host-owned Console panel state, so the Console panel itself can live in the module.
constexpr std::uint32_t kEditorModuleAPIVersion = 2;

// ImGui's own allocator signatures, spelled out here so this header stays free of <imgui.h>
// (the host and the module each compile their own ImGui translation units; only the context and
// the allocator pair need to be shared across the boundary).
using EditorModuleImGuiAllocFn = void* (*)(std::size_t size, void* userData);
using EditorModuleImGuiFreeFn  = void  (*)(void* ptr, void* userData);

// Mirrors LogLevel from Core/Log.h as a plain int-valued enum so the module never has to include
// (or link against) the engine's logging translation unit — see EditorModuleHostAPI::LogGetEntry.
enum EditorModuleLogLevel : int {
    EditorModuleLogLevel_Info    = 0,
    EditorModuleLogLevel_Warning = 1,
    EditorModuleLogLevel_Error   = 2,
};

// Console UI state. Deliberately POD with a fixed-size filter buffer: it lives in the HOST so it
// survives a module reload (rebuilding TartarusEditor.dll mustn't clear the user's filter text,
// level toggles or panel visibility), and the host's own toolbar needs the visibility flag for
// its "Toggle Console" button. Anything derived from this (e.g. the cached filtered index list)
// stays module-side and is simply rebuilt after a reload.
struct EditorConsoleState {
    bool Visible = true;
    bool ShowInfo = true;
    bool ShowWarning = true;
    bool ShowError = true;
    bool AutoScroll = true;
    bool ShowTimestamps = true;
    // Matches the 128-byte buffer the panel's InputTextWithHint has always used.
    char Filter[128] = {};
    // Only auto-scroll when Log actually gained an entry, rather than fighting the user's
    // scrollback every frame.
    unsigned int SeenRevision = 0;
};

struct EditorModuleHostAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*DrawStatusPanel)(const char* title, const char* message, const char* accent) = nullptr;

    // --- ImGui sharing ---------------------------------------------------------------------
    // The module compiles its own copy of ImGui's sources, so it must be pointed at the host's
    // single ImGuiContext (all ImGui state lives there) and the host's allocator pair before it
    // makes any ImGui call. This is ImGui's documented multi-module setup.
    void* (*GetImGuiContext)() = nullptr;
    void (*GetImGuiAllocators)(EditorModuleImGuiAllocFn* outAlloc,
                               EditorModuleImGuiFreeFn* outFree,
                               void** outUserData) = nullptr;

    // --- Log ------------------------------------------------------------------------------
    // Log's storage is a function-local static inside the host's Log.cpp. Compiling Log.cpp into
    // the module as well would give the module its own private, permanently empty log, so the
    // module never links it and reads everything through these host-side accessors instead.
    unsigned int (*LogRevision)() = nullptr;
    int (*LogEntryCount)() = nullptr;
    // Fills the out-params for one entry; the returned strings point at the host's own storage
    // and stay valid until the log next changes (i.e. for the duration of the caller's use
    // within a frame). Returns false for an out-of-range index.
    bool (*LogGetEntry)(int index, int* outLevel, const char** outMessage,
                        const char** outTime, int* outCount) = nullptr;
    int (*LogCountOf)(int level) = nullptr;
    void (*LogClear)() = nullptr;
    void (*LogInfo)(const char* message) = nullptr;
    void (*LogError)(const char* message) = nullptr;

    // --- Editor services --------------------------------------------------------------------
    // Routed through the host so the module doesn't duplicate EditorSettings (another singleton)
    // or need the GLFW window handle the native file dialog is parented to.
    void (*SetTooltip)(const char* text) = nullptr;
    // Writes the chosen path into `outPath`; returns false if the user cancelled (or the buffer
    // was too small). Plain char buffer rather than a std::string return: no STL across the ABI.
    bool (*SaveFileDialog)(const char* filter, const char* defaultExt,
                           char* outPath, int outPathSize) = nullptr;

    // Host-owned, reload-surviving Console panel state. Never null when Version matches.
    EditorConsoleState* (*ConsoleState)() = nullptr;
};

struct EditorModuleAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Draw)(const EditorModuleHostAPI& host) = nullptr;
};

using GetEditorModuleAPIFn = const EditorModuleAPI* (*)();
