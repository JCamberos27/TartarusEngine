#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>
#include <set>

class Camera;
class FirstPersonBody;
class FirstPersonPresentation;
class ShellCasings;
class World;

// `--weapon-test`: the Sandbox's weapons played through by script, in the real Play loop (the
// smoke-test harness: real scene, rigs, controllers, IK and physics raycasts) on a fixed step.
// Each frame Drive() stands in for the player's weapon input - trigger, aim, R, weapon keys - and
// checks what the weapon did: states, ammo, the pump, pellets, reloads, the sights, slot swaps.
// Prints one [WeaponTest] line per check; Failures() counts the failed ones.
//
// `--stock-probe [remington|ak]` runs a different script on the same harness: the weapon held while the view
// is pitched, turned, fired and aimed, logging where the gun's butt sits against the body's right
// shoulder, neck and head ([StockProbe] lines), with Scene + Game view captures under --smoke-shots.
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
        float Anchored = 0.0f;          // the last reload's highest hand-anchor weight (handInTheWay)
        std::vector<glm::vec3> Shell; // ... and the weapon's Shell bone, same frames (view space, m)
        // Spent cases out of the port this step: the state each left in, and how it was thrown.
        struct Ejection {
            std::string State;
            float Right = 0.0f;     // the throw along the camera's right, as a share of the throw (-1..1)
            float FromEye = 0.0f;   // the port from the camera, m
            glm::vec3 Port{0.0f}, Muzzle{0.0f}; // the port and the muzzle, camera frame (right, up, forward), m
        };
        std::vector<Ejection> Ejections;
        int Casings = 0, CasingsAsleep = 0; // on the ground now (ShellCasings)
        std::function<void(bool, const std::string&)> Check;
        Camera* Cam = nullptr;             // the play camera (the script may set its yaw / pitch)
        int LogEvery = 0;                  // stock probe: print a sample every this many frames (0 = off)
        std::string Label;                 // ... tagged with this
        std::string Shot;                  // a capture name for this frame (--smoke-shots)
        glm::vec2 Move{0.0f};              // the move keys (x right, y forward, -1..1), held until changed
        bool Sprint = false;               // ... and Sprint
        bool Crouch = false;               // ... and Crouch
        int View = 0;                      // stock probe's Scene camera: 0 on the stock, 1 right side, 2 front right, 3 front left,
                                           // 4 left, 5 back right, 6 back left (whole body); 7 / 8 the right / left arm close,
                                           // 9 the legs close from the side
        bool Trigger = false;              // the trigger held down (full auto), until changed
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

    explicit FirstPersonWeaponTest(bool stockProbe = false, bool probeAk = false);
    void SetCamera(Camera* cam) { m_Ctx.Cam = cam; }
    // Stock probe, once the body's arms are on the rig (after FirstPersonBody::ArmsLateUpdate):
    // measures this frame's pose and places the Scene camera on the gun and shoulder.
    void AfterPose(const World& world, const FirstPersonBody& body, const FirstPersonPresentation& p);
    // Where the Scene view should look from this frame (stock probe), false = leave it.
    bool SceneCamera(glm::vec3& position, float& yaw, float& pitch) const;
    // Once per Play frame, in place of the weapon input block (after the presentation's Update).
    void Drive(FirstPersonPresentation& p, float dt);
    bool Aim() const { return m_Ctx.Aim; }
    glm::vec2 Move() const { return m_Ctx.Move; }
    bool Sprint() const { return m_Ctx.Sprint; }
    bool Crouch() const { return m_Ctx.Crouch; }
    void OnHit(const glm::vec3& point);
    // Once per Play frame after the cases have moved: notes this frame's ejections and the pile.
    void OnCasings(const ShellCasings& casings);
    bool Done() const { return m_Step >= m_Steps.size(); }
    // The gait probe (STOCK_PROBE_GAIT): starts on the scene's most open floor, the player's body unclothed.
    bool GaitProbe() const { return m_GaitProbe; }
    // The gait and hold probes start on the open floor (room to walk every way, nothing in the shots).
    bool OpenFloor() const { return m_GaitProbe || m_BodyProbe; }
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
    int m_Ejected = 0; // the presentation's EjectedTotal last frame
    std::string m_Shot;
    bool m_Probe = false;
    bool m_ProbeAk = false;     // the probe holds the AKS-74U, not the Remington
    bool m_SprintProbe = false;
    std::set<std::string> m_SprintSamples;
    int m_PrintedGunSlot = -1;  // the slot the probe last printed its gun line (butt to muzzle) for
    struct Sample {
        bool Valid = false;
        float Pitch = 0.0f, Yaw = 0.0f, YawRate = 0.0f, TwistDeg = 0.0f;
        glm::vec3 EyeFromShoulder{0.0f};   // the camera - upperarm_r, the same frame
        float Roll = 0.0f;
        glm::vec3 StockFromShoulder{0.0f}; // butt - upperarm_r, view's flat frame (right, up, forward), m
        float Shoulder = 0.0f, Clavicle = 0.0f, Neck = 0.0f, Head = 0.0f; // butt to each, m
        float NeckGap = 0.0f, HeadGap = 0.0f; // nearest the gun's rear 30 cm comes to the neck / head bone, m
        float GunShift = 0.0f;                // the world gun off the first-person one, m
        float HandGap[2] = {0.0f, 0.0f};      // the world hands off the world gun's grips (L, R), m
        // The whole gun, butt to muzzle (GunLength m): nearest the neck bone, the hood keep-out's centre
        // (head + 7 cm up), and the drawn head / neck / hood mesh (MeshGap, on MeshPiece, MeshAlong m from the butt).
        float GunLength = 0.0f, WholeNeckGap = 0.0f, WholeHoodGap = 0.0f, MeshGap = -1.0f, MeshAlong = 0.0f;
        std::string MeshPiece;
        float Speed = 0.0f;                   // the player's planar speed, m/s
        float ElbowGap[2] = {-1.0f, -1.0f};   // each world elbow to the drawn torso (L, R), m (FirstPersonBody::ElbowTorsoGaps)
        float ElbowSwing[2] = {0.0f, 0.0f};   // ... and how far Elbow Clearance swung it out, degrees
        float TorsoGap = -1.0f, TorsoAlong = 0.0f; // the gun's rear GunLength to the drawn torso, m (and where, m from the butt)
        float HeadTilt = 0.0f, HeadTiltWeight = 0.0f; // the cheek weld: degrees, and its weight
        float HeadBend = 0.0f;                // the world head's bend off the chest line (spine_05 -> neck vs neck -> head), degrees
        // The camera's smoothness: the most its position accelerated (m/s^2, frame to frame) since the last
        // printed sample - a hitch in the eye shows as a spike.
        float EyeAccel = 0.0f;
        glm::vec3 Eye{0.0f};                  // where the camera is (world, m)
        std::string State;
    } m_Sample;
    glm::vec3 m_EyePrev[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    int m_EyeFrames = 0;
    mutable float m_EyeAccelMax = 0.0f;
    int m_Frame = 0;
    float m_LastYaw = 0.0f;
    bool m_HaveYaw = false;
    bool m_HaveSceneCam = false;
    glm::vec3 m_SceneCamPos{0.0f};
    float m_SceneCamYaw = 0.0f, m_SceneCamPitch = 0.0f;
    // Audio: each frame's animator state and phase beside what the weapon audio emitted, checked at the end
    // against the controllers' snd.* events (CheckAudio).
    struct AudioFrame {
        int Slot = 0;
        std::string State;
        float Phase = 0.0f;
        std::string Controller;
        std::vector<std::string> Keys; // emitted since the frame before
    };
    std::vector<AudioFrame> m_AudioFrames;
    size_t m_AudioSeen = 0;
    bool m_AudioChecked = false;
    void RecordAudioFrame(const World& world, const FirstPersonPresentation& p);
    void CheckAudio();
    void PrintSample(const std::string& label) const;
    void BuildProbe();
    void BuildPoseProbe(); // STOCK_PROBE_POSE=1: the third-person body standing / crouched, at the hip and on the sights
    void BuildSprintProbe(); // STOCK_PROBE_SPRINT=1: walk -> sprint -> walk, capturing entry/exit
    // STOCK_PROBE_GAIT=1: walk, jog and sprint every way and a crouch walk, the torso measured against the hips on the
    // world body (what every other view shows) and on the player's own (Spine Stability steadies it) - [Gait] lines.
    void BuildGaitProbe();
    // STOCK_PROBE_BODY=1: the one animation set, inside and out. Locomotion - jog / walk / crouch / sprint starts and
    // stops, a pivot, a tap's step, turns on the spot and a turning start, a jump, an idle fidget - each segment's
    // body states checked against what should play and its planted feet's slide measured ([Body] lines). The gun -
    // idle, the sights up and down, crouched, sprinting, firing, reloads, the actions, holster and draw - with both
    // bodies' hands on it ([Hold] lines). Captures of the world body from all round and of the arms and legs close up.
    void BuildBodyProbe();
    bool m_BodyProbe = false;
    std::string m_BodyOnly; // STOCK_PROBE_BODY=<text>: just the locomotion segments named with it
    struct BodyStats {
        std::vector<std::string> States;      // the body's states this segment, in order
        glm::vec3 PlantStart[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
        glm::vec3 LastFoot[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
        int PlantFrames[2] = {0, 0};
        float PlantSlide[2] = {0.0f, 0.0f};    // this plant's farthest drift so far
        std::vector<float> Slides;            // each finished plant's (m)
        std::string PlantLog;                 // each plant's slide (cm) and the state it ended in
        float EyeY[2] = {0.0f, 0.0f};         // the camera's height, last two frames
        int EyeFrames = 0;
        float EyeJerk = 0.0f;                 // the camera's largest vertical acceleration (m/s^2): a bump in the view
        float FeetLow = 1e9f, FeetHigh = -1e9f; // the ground it crossed (a climb = stairs: the view's bump is the terrain's)
        float HandGap = 0.0f, TwinHandGap = 0.0f, BodyDiff = 0.0f; // the worst this segment (m)
        int Frames = 0;
        bool Have = false;
    };
    BodyStats m_Body;
    bool m_BodyMeasure = false;
    float m_FidgetSince = -1.0f;              // the fidget step: when the body's fidget began
    float m_ActGap = 0.0f;                    // an action step's worst world hand gap
    float m_HoldGap[2] = {0.0f, 0.0f};        // the world body's hands' gaps this frame
    float m_Wrist[2] = {0.0f, 0.0f}, m_WristLeft[2] = {0.0f, 0.0f}; // ... each hand's roll about its forearm, and what's left at the wrist (deg)
    bool m_GaitProbe = false;
    struct GaitStats {
        int Frames = 0;
        float Lean = 0.0f, Side = 0.0f, Twist = 0.0f, Pelvis = 0.0f; // sums: trunk lean along / across the travel, chest-on-hips yaw, pelvis height (deg, m)
        float LeanMin = 1e9f, LeanMax = -1e9f, TwistMin = 1e9f, TwistMax = -1e9f;
    };
    GaitStats m_Gait[2]; // the player's own view (pieces), the world twins
    std::string m_GaitSegment; // measured while set (after the segment settles)
    glm::vec2 m_GaitTravel{0.0f};
};
