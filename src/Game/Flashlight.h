#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "Audio/WeaponAudio.h"
#include "Camera.h"

// The player's flashlight (F). Not a scene entity: Play adds it to the frame's lights straight from the camera, as two cones of
// one cool-white LED - a tight hotspot that casts the shadows and a wide, faint spill ring around it, which is what reads as a
// real torch rather than a stage spot. It rides low and right of the eyes (a hand / chest-rig torch) and trails the view a
// little, so fast turns sweep the beam instead of it being glued to the crosshair.
struct Flashlight {
    bool On = false;
    bool Placed = false;
    glm::vec3 Position{0.0f};
    glm::vec3 Aim{0.0f, 0.0f, -1.0f};

    // ~7500 K LED.
    glm::vec3 Color{0.74f, 0.85f, 1.0f};
    float HotIntensity = 9.0f, HotRange = 16.0f, HotOuterDeg = 11.0f, HotInnerDeg = 3.5f;
    float SpillIntensity = 1.4f, SpillRange = 9.0f, SpillOuterDeg = 32.0f, SpillInnerDeg = 13.0f;
    float ShadowNear = 0.25f;                          // past the player's own arms and gun
    glm::vec3 Offset{0.17f, -0.14f, 0.12f};            // camera right / up / forward, metres
    float Follow = 20.0f;                              // per second: how fast the beam catches up with the view

    void Toggle(const glm::vec3& at) {
        On = !On;
        WeaponAudio& wa = WeaponAudio::Get();
        if (wa.Active()) wa.PlayEvent(On ? "snd.foley.weapon.flashlight_on" : "snd.foley.weapon.flashlight_off", "", at, true);
    }

    void Update(const Camera& cam, float dt) {
        const glm::vec3 front = cam.Front();
        Position = cam.Position + cam.Right() * Offset.x + cam.Up() * Offset.y + front * Offset.z;
        if (!Placed || dt <= 0.0f) {
            Aim = front;
            Placed = true;
            return;
        }
        Aim = glm::normalize(glm::mix(Aim, front, 1.0f - std::exp(-Follow * dt)));
    }
};
