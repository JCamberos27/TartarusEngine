#pragma once
#include <cstdint>
#include <vector>

#include "AI/NpcDirector.h" // PlayerSnapshot
#include "AI/NpcTest.h"     // NpcTest::BloodView
#include "BulletHoles.h"    // BulletHoleList::Placed

class AssetLibrary;
class BloodFx;
class Camera;
class CombatFx;
class CombatHud;
class CombatWiring;
class DevPanel;
class DevSpawner;
class EditorLayer;
class FirstPersonBody;
class FirstPersonPresentation;
struct Flashlight;
class FxSprites;
class GravityGun;
class HotReloadGameModule;
class ImpactFx;
class Player;
class PlayerVitals;
class ShellCasings;
class SpiderDamage;
class SpiderSystem;
class SprayCanSystem;
struct StillModelCache;
struct PlayState;
class WeaponLab;
class Window;
class World;

// What Play's per-frame simulation step reads, gathered once per frame (the Renderer/ShadowFrame pattern: pointers to
// main's long-lived objects, no ownership).
struct PlayFrameInputs {
    // This frame
    float dt = 0.0f;      // real time
    float gameDt = 0.0f;  // the game clock's step (time-scaled, 0 while paused)
    bool simThisFrame = false;
    bool gameHasInput = false;
    bool devKeys = false;
    std::uint64_t frameIndex = 0;
    Camera* gameCam = nullptr; // the camera the game view renders through (the default ears)
    PlayState* play = nullptr; // Play's state bits (App/PlaySession.h)

    World* world = nullptr;
    AssetLibrary* assets = nullptr;
    Window* window = nullptr;
    EditorLayer* editor = nullptr;
    HotReloadGameModule* gameModule = nullptr;
    StillModelCache* stillModels = nullptr; // the animation tick's gather (shared with the pre-skin pass)

    // The player
    Player* player = nullptr;
    FirstPersonPresentation* firstPersonPresentation = nullptr;
    FirstPersonBody* firstPersonBody = nullptr;
    PlayerVitals* playerVitals = nullptr;
    GravityGun* gravityGun = nullptr;
    Flashlight* flashlight = nullptr;
    WeaponLab* weaponLab = nullptr;           // --weapon-lab, else null
    NpcTest* npcTest = nullptr;               // --npc-test, else null
    NpcTest::BloodView* npcTestBlood = nullptr;

    // The enemies, the dev tools and combat's effects
    NpcDirector* npcDirector = nullptr;
    SpiderSystem* spiders = nullptr;
    SpiderDamage* spiderDamage = nullptr;
    DevSpawner* devSpawner = nullptr;
    DevPanel* devPanel = nullptr;
    CombatFx* combatFx = nullptr;
    CombatHud* combatHud = nullptr;
    const CombatWiring* combat = nullptr; // WorldImpact: a round into the world
    BloodFx* bloodFx = nullptr;
    ImpactFx* impactFx = nullptr;
    FxSprites* fxSprites = nullptr;
    BulletHoleList* bulletHoles = nullptr;
    ShellCasings* shellCasings = nullptr;
    SprayCanSystem* sprayCans = nullptr;
};

// Play's per-frame simulation: the gravity gun and dev spawns, the physics step, the player and first-person update,
// the enemy AI and spiders, the animators, first-person IK, the player's shots and their impacts, shell casings, the
// squad's impacts and flesh hits, combat FX and foley, blood, and the player's vitals. Then, every frame, the game
// module's tick, particles and the models' animation advance.
class PlayFrame {
public:
    PlayFrame();

    // The step itself: main calls it only when gameplay advances this frame (playing and not paused, or a Step).
    void Simulate(const PlayFrameInputs& in);

    // The game module's tick, particles and animations: every frame, playing or not (each picks its own clock).
    void Advance(const PlayFrameInputs& in);

private:
    // With a weapon (Animation Set) on the controller, the gravity gun is the unarmed slot:
    // live only while the weapon is holstered (2 / scroll / Holster), off while it's in hand.
    static bool GravityGunLive(const PlayFrameInputs& in);

    PlayerSnapshot m_PlayerSnap;                       // what the squad sees of the player this frame
    int m_PlayerAmmoSeen = -1, m_PlayerSlotSeen = -1;  // for "the player fired this frame"
    int m_BigHeadshots = 0;                            // big headshot kills (the gore sound), for --npc-test blood
    float m_NpcTestBloodGpuSum = 0.0f;                 // --npc-test blood: the blood sprays' GPU time
    int m_NpcTestBloodGpuFrames = 0;
    std::vector<BulletHoleList::Placed> m_PlacedHoles; // --npc-test blood: the holes resolved, reused frame to frame
    bool m_TraceSpiderHits = false;                    // TARTARUS_SPIDER_TRACE: log each spider hit on the player
};
