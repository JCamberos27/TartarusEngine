#pragma once
#include <glm/glm.hpp>
#include <algorithm>

// State selection responds to action axes immediately; playback/bob still use physical speed.
// The controller's moving/still threshold is 0.05. Keep the input dead zone identical to ADS
// walking and avoid parking exactly on a threshold where neither transition can fire.
inline float WeaponAnimationInputSpeed(float physicalSpeed,glm::vec2 moveInput,bool haveInput,float walkSpeed) {
    if (!haveInput) return physicalSpeed;
    const float input=glm::length(moveInput);
    return input<=0.05f?0.0f:std::max(input*std::max(walkSpeed,0.1f),0.051f);
}
