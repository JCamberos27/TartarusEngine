#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

class AssetLibrary;
class BloodFx;
class BulletHoleList;
class Camera;
class CombatFx;
class CombatHud;
class DevPanel;
class EditorLayer;
class FirstPersonBody;
class FirstPersonPresentation;
class FirstPersonWeaponTest;
struct Flashlight;
class FxSprites;
class GameViewPanel;
class GravityGun;
class NpcDirector;
class NpcTest;
class Player;
class PlayerVitals;
class ShellCasings;
class WeaponFxRenderer;
class WeaponLab;
class Window;
class World;
struct LaunchOptions;

// The play-mode state bits main's frame loop reads. Play is not a whole-screen mode swap: three independent bits
// describe it -
//   EditorUIVisible - the editor panels + dockspace are shown (true whenever NOT maximized)
//   Playing         - the scene is simulating; the Game panel renders from the player camera
//   Maximized       - the Game view fills the window and the editor panels are hidden
// Pressing Play just flips Playing; the game runs inside the docked Game panel with the editor still fully usable.
// The Fullscreen button next to Stop toggles Maximized.
struct PlayState {
    bool EditorUIVisible = true;
    bool Playing = false;
    bool Paused = false; // #236: simulation frozen, rendering continues; Step advances one frame
    bool Maximized = false;
    // In-panel play is click-to-focus: the game only reads mouse/keyboard once the user has clicked inside the Game
    // view. Esc (or Stop) hands control back to the editor. Ignored while maximized - that path uses the cursor-lock
    // state directly, like the old Play mode.
    bool GameInputEngaged = false;
    // #165 - what Play renders through: the first-person player (UsesPlayer), or the scene's Camera entity
    // (CameraEntity), mirrored each frame into main's playSceneCam.
    bool UsesPlayer = true;
    bool GravityGun = true;
    float BaseFov = 58.7f;         // the player's vertical FOV / mouse sensitivity; the
    float BaseSensitivity = 0.1f;  // weapon's ADS zoom scales both from these each frame
    entt::entity CameraEntity = entt::null;
    // The scene's First Person Controller while Playing: it rides the player's feet and yaw, so selecting it (and
    // Shift+F in the Scene view) tracks the player. Play reverts it on Stop.
    entt::entity ControllerEntity = entt::null;
    glm::vec3 DeathCamOffset{0.0f}; // the death camera's drop, on the camera only after the late pose
    float DeathCamRoll = 0.0f;
    int ErrPauseSeen = 0; // #236 A5 - error count baseline for "Error Pause", re-armed each Play
};

// Entering and leaving Play. Unity's play-mode contract: entering Play snapshots the scene and leaving it restores
// that snapshot, so gameplay (shot crates, moved objects) never silently becomes an edit. EditorLayer owns the
// snapshot; PlayState holds the state bits; this sets up and tears down the systems Play runs.
class PlaySession {
public:
    PlaySession(PlayState& state, const LaunchOptions& opt, Window& window, World& world, AssetLibrary& assets,
                EditorLayer& editor, GameViewPanel& gameView, Camera& editorCamera, Player& player, Flashlight& flashlight,
                GravityGun& gravityGun, FirstPersonPresentation& presentation, FirstPersonBody& body,
                CombatFx& combatFx, CombatHud& combatHud, WeaponFxRenderer& weaponFx, BloodFx& bloodFx,
                FxSprites& fxSprites, PlayerVitals& playerVitals, NpcDirector& npcDirector, DevPanel& devPanel,
                BulletHoleList& bulletHoles, ShellCasings& shellCasings, FirstPersonWeaponTest* weaponTest,
                WeaponLab* weaponLab, NpcTest* npcTest)
        : m_State(state), m_Opt(opt), m_Window(window), m_World(world), m_Assets(assets), m_Editor(editor),
          m_GameView(gameView), m_EditorCamera(editorCamera), m_Player(player), m_Flashlight(flashlight),
          m_GravityGun(gravityGun), m_Presentation(presentation), m_Body(body), m_CombatFx(combatFx),
          m_CombatHud(combatHud), m_WeaponFx(weaponFx), m_BloodFx(bloodFx), m_FxSprites(fxSprites),
          m_PlayerVitals(playerVitals), m_NpcDirector(npcDirector), m_DevPanel(devPanel), m_BulletHoles(bulletHoles),
          m_ShellCasings(shellCasings), m_WeaponTest(weaponTest), m_WeaponLab(weaponLab), m_NpcTest(npcTest) {}

    void Start(); // no-op while playing
    void Stop();  // no-op while stopped
    void Toggle() { if (m_State.Playing) Stop(); else Start(); }

private:
    PlayState& m_State;
    const LaunchOptions& m_Opt;
    Window& m_Window;
    World& m_World;
    AssetLibrary& m_Assets;
    EditorLayer& m_Editor;
    GameViewPanel& m_GameView;
    Camera& m_EditorCamera;
    Player& m_Player;
    Flashlight& m_Flashlight;
    GravityGun& m_GravityGun;
    FirstPersonPresentation& m_Presentation;
    FirstPersonBody& m_Body;
    CombatFx& m_CombatFx;
    CombatHud& m_CombatHud;
    WeaponFxRenderer& m_WeaponFx;
    BloodFx& m_BloodFx;
    FxSprites& m_FxSprites;
    PlayerVitals& m_PlayerVitals;
    NpcDirector& m_NpcDirector;
    DevPanel& m_DevPanel;
    BulletHoleList& m_BulletHoles;
    ShellCasings& m_ShellCasings;
    FirstPersonWeaponTest* m_WeaponTest;
    WeaponLab* m_WeaponLab;
    NpcTest* m_NpcTest;
};

// #165 - the scene's First Person Controller (first active one in creation order), if any.
entt::entity FindFirstPersonController(World& world);
// The first active in-scene Camera entity (creation order), or entt::null.
entt::entity FindActiveSceneCamera(const World& world);
// #165 - yaw/pitch (Camera's convention) of an entity's forward (-Z) axis. Roll is dropped: Camera has none.
void YawPitchFromWorld(const glm::mat4& m, float& yawDeg, float& pitchDeg);
