#pragma once

// What the FirstPersonPresentation*.cpp files share among themselves (not the interface: FirstPersonPresentation.h).

#include "AnimatorController.h"
#include "Audio/WeaponAudio.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace FirstPersonPresentationDetail {

// A weapon sound (an animator's snd.* event, or a gear sound like ads_in / dry_fire / equip), 2D on the player's own
// gun. `name` is a full key (snd.ak.mag_out) or an element (ads_in).
inline void WeaponSound(const std::filesystem::path& setFile, const std::string& name) {
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active()) return;
    wa.PlayEvent(name, WeaponAudioGunId(setFile.string()), glm::vec3(0.0f), true);
}

inline std::string TrackOr(const AnimatorController& ctrl, const char* wanted, int fallback) {
    for (const auto& t : ctrl.Tracks) if (t == wanted) return t;
    return ctrl.Tracks[std::clamp(fallback, 0, (int)ctrl.Tracks.size() - 1)];
}

} // namespace FirstPersonPresentationDetail
