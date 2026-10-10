#pragma once

#include "WeaponJankMeter.h"

#include <glm/glm.hpp>

#include <string>

class Camera;
class FirstPersonBody;
class FirstPersonPresentation;
class World;
struct GLFWwindow;

// This frame's third-person weapon pose for WeaponJankMeter: the world gun and the world hands with the third-person
// clips' own motion taken out (the clips' gun - the arms3p rig on the arms' place - against the world gun, and each world
// hand on the world gun against the clips' hand on theirs). Shared by --stock-probe's jank sweep and the Weapon Lab.
WeaponJankFrame WeaponJankFrameFor(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p, float dt,
                                   const std::string& label);

// `--weapon-lab` (run-weapon-lab.cmd): the WeaponLab scene with the Scene view (the world body: what everyone else sees)
// beside the Game view (the player's own), for working on the third-person weapon animations by hand. In Play the Scene
// camera follows the world body from a picked side, or circles it; the Weapon Lab panel picks the side, slows the game
// down, pauses and steps it, plays each weapon action, reads out the hold, and lists what WeaponJankMeter flags live.
// Keypad: 1 front right, 2 right, 3 front left, 4 back right, 5 left arm, 6 right arm, 7 circling, 0 free camera;
// + / - faster / slower, Enter pause, . step a frame.
class WeaponLab {
public:
    enum class View { FrontRight, Right, FrontLeft, BackRight, LeftArm, RightArm, Orbit, Free, Count };

    // Each Play frame once the body's arms are on the rig (beside FirstPersonWeaponTest::AfterPose).
    void AfterPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p, const Camera& player, float dt);
    // Where the Scene view looks from this frame; false = the user's own (Free, or nothing to follow).
    bool SceneCamera(glm::vec3& position, float& yaw, float& pitch) const;
    // The keypad shortcuts, once a frame.
    void HandleKeys(GLFWwindow* window);
    // The panel (inside the editor's ImGui frame). The action buttons drive `p`.
    void DrawPanel(FirstPersonPresentation* p, bool playing);
    // The game's speed (multiplies the editor's time scale), and pause / step requests for the Play loop.
    float TimeScale() const { return m_TimeScale; }
    bool ConsumePauseToggle() { const bool r = m_PauseToggle; m_PauseToggle = false; return r; }
    bool ConsumeStep() { const bool r = m_Step; m_Step = false; return r; }
    void OnPlayStopped();

private:
    View m_View = View::FrontRight;
    float m_TimeScale = 1.0f;
    bool m_PauseToggle = false, m_Step = false;
    bool m_HaveCam = false;
    glm::vec3 m_CamPos{0.0f}, m_CamTarget{0.0f};
    float m_OrbitDeg = 0.0f;
    bool m_KeyWas[16] = {};
    WeaponJankMeter m_Jank;
    std::string m_LastAction = "-";
    // The readouts.
    std::string m_State;
    float m_Lock = 0.0f, m_Anchor = 0.0f, m_HandGap[2] = {0.0f, 0.0f}, m_Spare = -1.0f;
    float m_GunJump = 0.0f; // this frame's gun residual change (cm), for the live trace
    float m_Trace[180] = {};
    int m_TraceAt = 0;
    glm::vec3 m_LastGun{0.0f};
    bool m_HaveLastGun = false;
};
