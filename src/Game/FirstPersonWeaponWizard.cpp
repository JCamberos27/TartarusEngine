#include "FirstPersonWeaponWizard.h"

#include <algorithm>
#include <cctype>

namespace FPWizard {

const std::vector<StateSpec>& States() {
    static const std::vector<StateSpec> s = {
        {"Idle", {"idle"}, true},
        {"Walk", {"walk"}, true},
        {"Sprint", {"sprint", "run"}, true},
        {"Aim", {"aim", "ads"}, true},
        {"IdleToSprint", {"idletosprint", "sprintstart"}, false},
        {"SprintToIdle", {"sprinttoidle", "sprintend"}, false},
        {"Regrip", {"regrip", "fidget"}, false},
        {"Fire", {"fire", "shoot"}, false},
        {"Inspect", {"inspect"}, false},
        {"MagCheck", {"magcheck"}, false},
        {"Melee", {"melee", "bash"}, false},
        {"TacReload", {"tacreload", "tacticalreload", "reload"}, false},
        {"EmptyReload", {"emptyreload", "reloadempty", "reloadfull"}, false},
        {"Draw", {"draw", "equip", "unholster"}, false},
        {"Holster", {"holster", "unequip"}, false},
    };
    return s;
}

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The words of a file's name, split on _ - space and dot (IdleToSprint stays one word on purpose: it is
// a name, not a phrase).
std::vector<std::string> Words(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string stem = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    std::vector<std::string> out;
    std::string cur;
    for (char c : stem) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!cur.empty()) out.push_back(Lower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(Lower(cur));
    return out;
}

} // namespace

std::string PickClip(const std::string& state, const std::vector<std::string>& files) {
    const StateSpec* spec = nullptr;
    for (const auto& s : States())
        if (state == s.State) spec = &s;
    if (!spec) return {};
    for (const char* keyword : spec->Keywords) { // earlier keywords win over later ones
        std::string best;
        size_t bestLen = (size_t)-1;
        for (const std::string& f : files) {
            const auto w = Words(f);
            for (size_t k = 1; k <= 3 && k <= w.size(); ++k) {
                std::string tail;
                for (size_t i = w.size() - k; i < w.size(); ++i) tail += w[i];
                if (tail == keyword && f.size() < bestLen) { best = f; bestLen = f.size(); break; }
            }
        }
        if (!best.empty()) return best;
    }
    return {};
}

FirstPersonAnimationSet Build(const std::string& armsModel, const std::string& weaponModel, const std::vector<Pick>& picks) {
    FirstPersonAnimationSet set;
    set.ArmsModel = armsModel;
    set.WeaponModel = weaponModel;
    set.ViewRotation = {0.0f, 180.0f, 0.0f};
    set.WeaponSocket = "ik_hand_gun";
    set.WeaponRoot = "root";
    set.WeaponMountRotation = {0.0f, 90.0f, 90.0f};
    set.DefaultState = "Idle";
    for (const Pick& p : picks) {
        if (p.ArmsClip.empty() && p.WeaponClip.empty()) continue;
        FirstPersonAnimationClip c;
        c.Name = p.State;
        c.ArmsClip = p.ArmsClip;
        c.WeaponClip = p.WeaponClip;
        for (const auto& s : States())
            if (p.State == s.State) c.Loop = s.Loop;
        set.Clips.push_back(std::move(c));
    }
    return set;
}

} // namespace FPWizard
