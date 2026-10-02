#pragma once
// Every dockable editor panel's window name, in one place. Each is "<icon>  <title>###<id>": the
// tab shows the icon and title, and the stable id after "###" is what ImGui keys the window (and
// its saved dock position) by, so a title or icon can change without losing anyone's layout.
// Look a panel up with these constants, never a typed string.

#include <IconsFontAwesome6.h>

#include <string>

namespace EditorPanels {

inline constexpr const char* Scene        = ICON_FA_CUBE "  Scene###Scene";
inline constexpr const char* Game         = ICON_FA_GAMEPAD "  Game###Game";
inline constexpr const char* Hierarchy    = ICON_FA_LIST "  Hierarchy###Hierarchy";
inline constexpr const char* Inspector    = ICON_FA_SLIDERS "  Inspector###Inspector";
inline constexpr const char* Assets       = ICON_FA_FOLDER_OPEN "  Assets###Assets";
inline constexpr const char* Console      = ICON_FA_TERMINAL "  Console###Console";
inline constexpr const char* Statistics   = ICON_FA_CHART_SIMPLE "  Statistics###Statistics";
inline constexpr const char* History      = ICON_FA_CLOCK_ROTATE_LEFT "  History###History";
inline constexpr const char* Lighting     = ICON_FA_LIGHTBULB "  Lighting###Lighting";
inline constexpr const char* Settings     = ICON_FA_GEAR "  Settings###Settings";
inline constexpr const char* PhysicsDebug = ICON_FA_CUBES "  Physics Debug###PhysicsDebug";
inline constexpr const char* Animator     = ICON_FA_DIAGRAM_PROJECT "  Animator###Animator";
inline constexpr const char* AssetLibrary = ICON_FA_BOX_ARCHIVE "  Asset Library###AssetLibrary";

// Layouts saved before the panels had stable ids (imgui.ini, layout presets) name the windows by
// their old titles. Renames those [Window][...] sections so the docked positions carry over.
inline std::string MigrateIni(std::string ini) {
    static const char* const kRenames[][2] = {
        {"Scene", Scene},
        {"Game", Game},
        {"Scene Hierarchy", Hierarchy},
        {"Inspector", Inspector},
        {"Asset Browser", Assets},
        {ICON_FA_TERMINAL "  Console", Console},
        {ICON_FA_CHART_SIMPLE "  Statistics", Statistics},
        {ICON_FA_CLOCK_ROTATE_LEFT "  History", History},
        {ICON_FA_LIGHTBULB "  Lighting", Lighting},
        {ICON_FA_GEAR "  Settings", Settings},
        {ICON_FA_CUBES "  Physics Debug", PhysicsDebug},
        {ICON_FA_DIAGRAM_PROJECT "  Animator", Animator},
        {ICON_FA_BOX_ARCHIVE "  Asset Library", AssetLibrary},
    };
    for (const auto& r : kRenames) {
        const std::string from = std::string("[Window][") + r[0] + "]";
        const std::string to = std::string("[Window][") + r[1] + "]";
        for (size_t at = ini.find(from); at != std::string::npos; at = ini.find(from, at + to.size()))
            ini.replace(at, from.size(), to);
    }
    return ini;
}

} // namespace EditorPanels
