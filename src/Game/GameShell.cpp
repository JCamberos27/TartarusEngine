#include "GameShell.h"

namespace GameShell {
namespace {
Hooks g_Hooks;
} // namespace

void Set(const Hooks& hooks) { g_Hooks = hooks; }

bool ShortcutTriggered(const char* id) { return g_Hooks.ShortcutTriggered && g_Hooks.ShortcutTriggered(id); }

void PlaySound(Sound sound, float screenX01, float pitchScale) {
    if (g_Hooks.PlaySound) g_Hooks.PlaySound(sound, screenX01, pitchScale);
}

float LastItemScreenX() { return g_Hooks.LastItemScreenX ? g_Hooks.LastItemScreenX() : -1.0f; }

void FRowShow(const char* key, const std::string& title, const std::string& value, int step, int count) {
    if (g_Hooks.FRowShow) g_Hooks.FRowShow(key, title, value, step, count);
}

void FRowHold(const char* key, const std::string& title, const std::string& value) {
    if (g_Hooks.FRowHold) g_Hooks.FRowHold(key, title, value);
}

void FRowRelease(const std::string& value) {
    if (g_Hooks.FRowRelease) g_Hooks.FRowRelease(value);
}

LookPrefs LoadLook() { return g_Hooks.LoadLook ? g_Hooks.LoadLook() : LookPrefs{}; }

void SaveLook(const LookPrefs& prefs) {
    if (g_Hooks.SaveLook) g_Hooks.SaveLook(prefs);
}

std::string PrefsFilePath() { return g_Hooks.PrefsFilePath ? g_Hooks.PrefsFilePath() : std::string(); }

} // namespace GameShell
