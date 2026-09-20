#pragma once
#include <string>
#include <vector>

// #174 - what a built game runs: written by the Build pipeline as player.json next to the
// exported exe. An exe that finds one next to itself starts as the game (straight into Play,
// full window, no editor) instead of as the editor.
struct PlayerConfig {
    std::string ProductName = "My Game";
    std::string CompanyName;
    std::string Version = "1.0";
    std::vector<std::string> Scenes; // project-relative; [0] is loaded at startup
    int  Width = 1280, Height = 720;
    bool Fullscreen = true;
    bool VSync = true;
    bool DevelopmentBuild = false;

    // #174 - product branding, written by the Build pipeline after it has copied the chosen
    // images into the player. Both are paths relative to the exe (EnginePaths::Resolve), not
    // project-relative like Scenes, because the source assets do not travel with the build:
    // the pipeline stages them as assets/branding/player_icon.png and player_splash.png.
    // Empty means "none", and the player then behaves exactly as before - the exe's own icon
    // resource for the window, and no splash.
    std::string IconPath;
    std::string SplashPath;

    static constexpr const char* kFileName = "player.json";

    // False when the file is missing or unreadable (the caller then runs as the editor).
    static bool Load(const std::string& path, PlayerConfig& out);
    bool Save(const std::string& path) const;
};
