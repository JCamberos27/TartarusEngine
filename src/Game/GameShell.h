#pragma once

#include <string>

// What gameplay code asks of the application around it: a key's shortcut, a feedback sound, the F-row pill, the
// look and outfit kept between sessions. The editor provides all of these, but Game must not include Editor headers;
// the App wires them up at start-up (App/GameShellWiring.cpp). Unwired (the unit tests), every call is a quiet no-op:
// no shortcut fires, nothing plays or shows, the look prefs are the defaults and are not kept.
namespace GameShell {

// The feedback sounds gameplay plays (the editor's sound set answers them).
enum class Sound { FKeyCycle, FKeyCycleBack, SpawnNpc, SpawnItem, ClearSpawns, Blocked, TimeFaster, TimeSlower, TimeReset };

// The F7 / F8 look and outfit picked in Play, kept between sessions.
struct LookPrefs {
    std::string FRowLook;     // "Male/European", "Female/Afro", ... (empty: the scene's own)
    int FRowStyle = -1;       // the wardrobe's style index; its style count = Bare (no clothes); -1: the wardrobe's pick
    unsigned FRowOutfitSeed = 0;
};

struct Hooks {
    bool (*ShortcutTriggered)(const char* id) = nullptr;
    void (*PlaySound)(Sound sound, float screenX01, float pitchScale) = nullptr;
    float (*LastItemScreenX)() = nullptr; // the last UI item's x on screen, 0..1 (pans a button's sound)
    void (*FRowShow)(const char* key, const std::string& title, const std::string& value, int step, int count) = nullptr;
    void (*FRowHold)(const char* key, const std::string& title, const std::string& value) = nullptr;
    void (*FRowRelease)(const std::string& value) = nullptr;
    LookPrefs (*LoadLook)() = nullptr;
    void (*SaveLook)(const LookPrefs& prefs) = nullptr;
    std::string (*PrefsFilePath)() = nullptr; // the user's prefs file (old scenes migrate settings from it)
};

void Set(const Hooks& hooks);

bool ShortcutTriggered(const char* id);
void PlaySound(Sound sound, float screenX01 = -1.0f, float pitchScale = 1.0f);
float LastItemScreenX();
void FRowShow(const char* key, const std::string& title, const std::string& value, int step = 0, int count = 0);
void FRowHold(const char* key, const std::string& title, const std::string& value);
void FRowRelease(const std::string& value);
LookPrefs LoadLook();
void SaveLook(const LookPrefs& prefs);
std::string PrefsFilePath();

} // namespace GameShell
