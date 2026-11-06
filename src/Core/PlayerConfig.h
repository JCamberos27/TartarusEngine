#pragma once
#include <string>
#include <vector>

// #174 - the fields a build hands to its player. Build Settings (project/settings.json "build")
// and player.json both carry exactly these, so they share one struct and one JSON reader/writer.
struct PlayerBuildFields {
    std::string ProductName = "My Game";
    std::vector<std::string> Scenes; // project-relative; [0] is loaded at startup
    int  Width = 1280, Height = 720; // windowed size
    bool Fullscreen = true;
    bool VSync = true;
    bool DevelopmentBuild = false;   // keep the stats overlay + physics debug keys in the player
    // Window/taskbar icon and startup splash. Empty means the engine's own defaults (the exe's
    // icon resource, no splash). Project-relative in Build Settings; in player.json they are
    // exe-relative (EnginePaths::Resolve), because the pipeline stages the images as
    // assets/branding/player_icon.png and player_splash.png.
    std::string IconPath;
    std::string SplashPath;
    // JSON reader/writer: PlayerConfigJson.h (kept out of this header, which ProjectSettings.h pulls in).
};

// What a built game runs: written by the Build pipeline as player.json next to the exported exe.
// An exe that finds one next to itself starts as the game (straight into Play, full window, no
// editor) instead of as the editor.
struct PlayerConfig : PlayerBuildFields {
    static constexpr const char* kFileName = "player.json";

    // False when the file is missing or unreadable (the caller then runs as the editor).
    static bool Load(const std::string& path, PlayerConfig& out);
    bool Save(const std::string& path) const;

    // The player's own %LOCALAPPDATA% folder name: ProductName with anything but letters, digits,
    // space, '-' and '_' turned into '_' ("TartarusGame" when that leaves nothing).
    std::string UserFolderName() const;
};
