#pragma once

#include "AnimatorController.h"

#include <string>
#include <vector>

// A structural check of an Animator Controller, for the Animator window's Lint tab: the mistakes that
// leave a controller loading fine and then quietly not doing what it was built to. Pure (no clips are
// resolved, no rig is needed), so it is cheap to run every frame and unit-testable.
namespace AnimatorLint {

enum class Level { Info = 0, Warning, Error };

struct Issue {
    Level Severity = Level::Warning;
    std::string Message;
    std::string Hint;
    // Where it is, so the window can select it: -1 when the issue is not about one thing.
    int Layer = -1;
    int State = -1;
    int Transition = -1;
};

// Every issue, worst first.
std::vector<Issue> Check(const AnimatorController& c);

} // namespace AnimatorLint
