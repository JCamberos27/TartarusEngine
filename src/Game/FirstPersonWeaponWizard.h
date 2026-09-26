#pragma once

#include "FirstPersonAnimation.h"

#include <string>
#include <utility>
#include <vector>

// The core of the "New First-Person Weapon" wizard: matching a folder of animation files to the states
// the standard first-person graph has, and turning the picks into a weapon definition (.fpsanim) that
// BuildFirstPersonController then makes a controller for. Pure, so it is unit-tested.
namespace FPWizard {

struct StateSpec {
    const char* State;
    std::vector<const char*> Keywords; // normalised (lower case, no separators) name tails, best first
    bool Loop;
};
// The states the wizard fills, in graph order.
const std::vector<StateSpec>& States();

// The file in `files` (project-relative paths) that best matches `state`, or "" when none does. A file
// matches when the last one to three words of its name, joined, equal a keyword - so "..._A_FP_Mag_Check"
// is MagCheck, and "..._Sprint" is Sprint but "..._IdleToSprint" is not.
std::string PickClip(const std::string& state, const std::vector<std::string>& files);

struct Pick {
    std::string State;
    std::string ArmsClip;
    std::string WeaponClip;
};
// A weapon definition for the picks: the standard mount (socket ik_hand_gun, root "root", the Manny rigs'
// mount rotation and view rotation) and every state that has an arms or a weapon clip.
FirstPersonAnimationSet Build(const std::string& armsModel, const std::string& weaponModel, const std::vector<Pick>& picks);

} // namespace FPWizard
