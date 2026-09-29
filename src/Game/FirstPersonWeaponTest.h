#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

class FirstPersonPresentation;

// `--weapon-test`: the Sandbox's weapons played through by script, in the real Play loop (the
// smoke-test harness: real scene, rigs, controllers, IK and physics raycasts) on a fixed step.
// Each frame Drive() stands in for the player's weapon input - trigger, aim, R, weapon keys - and
// checks what the weapon did: states, ammo, the pump, pellets, reloads, the sights, slot swaps.
// Prints one [WeaponTest] line per check; Failures() counts the failed ones.
class FirstPersonWeaponTest {
public:
    // What a step sees and does.
    struct Ctx {
        FirstPersonPresentation* P = nullptr;
        float Dt = 0.0f;
        float Time = 0.0f;                 // seconds into the current step
        std::vector<std::string> States;   // states entered during the current step, in order
        std::vector<glm::vec3> Hits;       // round hits during the current step
        std::vector<float> HitAngles;      // ... each one's angle off the (zeroed) bore at the muzzle, degrees
        int Pulls = 0;                     // trigger pulls this frame (a press)
        bool Aim = false;                  // held until a step changes it
        // Through reload states this step: the left hand in view space (m) and the ADS hand anchor's weight.
        std::vector<glm::vec4> Hand;
        std::vector<glm::vec3> Shell; // ... and the weapon's Shell bone, same frames (view space, m)
        std::function<void(bool, const std::string&)> Check;
        bool Saw(const std::string& state) const;
        const std::string& State() const;
    };
    struct Step {
        std::string Name;
        std::function<void(Ctx&)> Begin;   // once, on entry
        std::function<bool(Ctx&)> Until;   // every frame after Begin; true = done (null = done at once)
        float Timeout = 10.0f;             // seconds before it fails as timed out (and the test stops)
        std::function<void(Ctx&)> End;     // once, when Until holds: the step's checks
    };

    FirstPersonWeaponTest();
    // Once per Play frame, in place of the weapon input block (after the presentation's Update).
    void Drive(FirstPersonPresentation& p, float dt);
    bool Aim() const { return m_Ctx.Aim; }
    void OnHit(const glm::vec3& point);
    bool Done() const { return m_Step >= m_Steps.size(); }
    int Failures() const { return m_Failures; }
    int Checks() const { return m_Checks; }
    // With --smoke-shots: a name when the Game view should be saved this frame (every 10th frame
    // of the reload steps), else empty.
    const std::string& ShotName() const { return m_Shot; }

private:
    std::vector<Step> m_Steps;
    size_t m_Step = 0;
    bool m_Began = false;
    std::string m_LastState;
    Ctx m_Ctx;
    std::function<bool()> m_Held; // full auto: the trigger held down
    int m_Failures = 0, m_Checks = 0;
    std::string m_Shot;
};
