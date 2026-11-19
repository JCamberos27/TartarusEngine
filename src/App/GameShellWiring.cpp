#include "GameShellWiring.h"

#include "EditorSettings.h"
#include "EditorSound.h"
#include "FKeyRow.h"
#include "GameShell.h"
#include "Shortcuts.h"

namespace {

EditorSound::Event ToEditorEvent(GameShell::Sound s) {
    using S = GameShell::Sound;
    using E = EditorSound::Event;
    switch (s) {
        case S::FKeyCycle: return E::FKeyCycle;
        case S::FKeyCycleBack: return E::FKeyCycleBack;
        case S::SpawnNpc: return E::SpawnNpc;
        case S::SpawnItem: return E::SpawnItem;
        case S::ClearSpawns: return E::ClearSpawns;
        case S::Blocked: return E::Blocked;
        case S::TimeFaster: return E::TimeFaster;
        case S::TimeSlower: return E::TimeSlower;
        case S::TimeReset: return E::TimeReset;
    }
    return E::Blocked;
}

} // namespace

void WireGameShell() {
    GameShell::Hooks h;
    h.ShortcutTriggered = [](const char* id) { return Shortcuts::TriggeredGlfw(id); };
    h.PlaySound = [](GameShell::Sound s, float screenX01, float pitchScale) {
        EditorSound::Play(ToEditorEvent(s), screenX01, pitchScale);
    };
    h.LastItemScreenX = [] { return EditorSound::LastItemScreenX(); };
    h.FRowShow = [](const char* key, const std::string& title, const std::string& value, int step, int count) {
        FKeyRow::Show(key, title, value, step, count);
    };
    h.FRowHold = [](const char* key, const std::string& title, const std::string& value) { FKeyRow::Hold(key, title, value); };
    h.FRowRelease = [](const std::string& value) { FKeyRow::Release(value); };
    h.LoadLook = [] {
        const EditorSettings& es = EditorSettings::Get();
        GameShell::LookPrefs p;
        p.FRowLook = es.FRowLook;
        p.FRowStyle = es.FRowStyle;
        p.FRowOutfitSeed = es.FRowOutfitSeed;
        return p;
    };
    h.SaveLook = [](const GameShell::LookPrefs& p) {
        EditorSettings& es = EditorSettings::Get();
        es.FRowLook = p.FRowLook;
        es.FRowStyle = p.FRowStyle;
        es.FRowOutfitSeed = p.FRowOutfitSeed;
        EditorSettings::Save();
    };
    h.PrefsFilePath = [] { return EditorSettings::PrefsFilePath(); };
    GameShell::Set(h);
}
