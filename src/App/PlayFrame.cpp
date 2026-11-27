#include "PlayFrame.h"
#include "Core/DiagW1.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "AI/Npc.h"
#include "AI/NpcDirector.h"
#include "AI/NpcTest.h"
#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Audio/FoleyAudio.h"
#include "Audio/ImpactAudio.h"
#include "AudioEngine.h"
#include "BloodRenderer.h"
#include "BulletHoles.h"
#include "Camera.h"
#include "Combat/BloodFx.h"
#include "Combat/CombatFx.h"
#include "Combat/FxSprites.h"
#include "Combat/ImpactFx.h"
#include "Combat/PlayerVitals.h"
#include "CombatWiring.h"
#include "Components.h"
#include "Core/Env.h"
#include "DevPanel.h"
#include "DevSpawner.h"
#include "EditorLayer.h"
#include "EditorSettings.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "Flashlight.h"
#include "FxSpriteRenderer.h"
#include "GameModuleAPI.h" // kPlayerEntity
#include "GravityGun.h"
#include "HotReloadGameModule.h"
#include "Input.h"
#include "InputMap.h"
#include "Model.h"
#include "Openable.h"
#include "LightSwitch.h"
#include "ParticleSystem.h"
#include "PhysicsWorld.h"
#include "PlaySession.h"
#include "Player.h"
#include "Profiler.h"
#include "ShellCasings.h"
#include "Shortcuts.h"
#include "SprayCan.h"
#include "StillModelCache.h"
#include "WeaponLab.h"
#include "Window.h"
#include "World.h"

bool PlayFrame::GravityGunLive(const PlayFrameInputs& in) {
    return in.play->GravityGun && !(in.firstPersonPresentation->IsActive() && in.firstPersonPresentation->IsEquipped());
}

void PlayFrame::Simulate(const PlayFrameInputs& in) {
    // main's names for what the step reads, so the step reads as it did there.
    World& world = *in.world;
    AssetLibrary& assets = *in.assets;
    EditorLayer& editor = *in.editor;
    HotReloadGameModule& gameModule = *in.gameModule;
    Player& player = *in.player;
    FirstPersonPresentation& firstPersonPresentation = *in.firstPersonPresentation;
    FirstPersonBody& firstPersonBody = *in.firstPersonBody;
    PlayerVitals& playerVitals = *in.playerVitals;
    GravityGun& gravityGun = *in.gravityGun;
    Flashlight& flashlight = *in.flashlight;
    WeaponLab* const weaponLab = in.weaponLab;
    NpcTest* const npcTest = in.npcTest;
    NpcTest::BloodView& npcTestBlood = *in.npcTestBlood;
    NpcDirector& npcDirector = *in.npcDirector;
    DevSpawner& devSpawner = *in.devSpawner;
    DevPanel& devPanel = *in.devPanel;
    CombatFx& combatFx = *in.combatFx;
    BloodFx& bloodFx = *in.bloodFx;
    ImpactFx& impactFx = *in.impactFx;
    FxSprites& fxSprites = *in.fxSprites;
    BulletHoleList& bulletHoles = *in.bulletHoles;
    ShellCasings& shellCasings = *in.shellCasings;
    SprayCanSystem& sprayCans = *in.sprayCans;
    Camera* const gameCam = in.gameCam;
    const float gameDt = in.gameDt;
    const bool gameHasInput = in.gameHasInput;
    const bool devKeys = in.devKeys;
    PlayState& play = *in.play;
    const bool playUsesPlayer = play.UsesPlayer;
    const bool playGravityGun = play.GravityGun;
    const float playBaseFov = play.BaseFov;
    const float playBaseSensitivity = play.BaseSensitivity;
    const entt::entity playControllerEntity = play.ControllerEntity;
    glm::vec3& deathCamOffset = play.DeathCamOffset;
    float& deathCamRoll = play.DeathCamRoll;
    PlayerSnapshot& playerSnap = m_PlayerSnap;
    int& bigHeadshots = m_BigHeadshots;
    float& npcTestBloodGpuSum = m_NpcTestBloodGpuSum;
    int& npcTestBloodGpuFrames = m_NpcTestBloodGpuFrames;
    const CombatWiring& combat = *in.combat;
    auto worldImpact = [&combat](std::uint32_t entity, const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir,
                                 float radius) { combat.WorldImpact(entity, point, normal, dir, radius); };

    // The gravity gun (#185, promoted off its old debug harness) is a normal part of
    // Play mode, not a debug tool — always live whenever the game has input focus.
    if (gameHasInput && playUsesPlayer && GravityGunLive(in) && !playerVitals.IsDead()) {
        gravityGun.Update(gameDt, player);
    }
    // F9-F12 (DevSpawner.h) and the items they drop, picked up with Interact.
    {
        DevSpawner::Context dc;
        dc.WorldPtr = &world;
        dc.Assets = &assets;
        dc.Npcs = &npcDirector;
        dc.Gun = &firstPersonPresentation;
        dc.Eye = player.Cam.Position;
        dc.Front = player.Cam.Front();
        dc.Dt = gameDt;
        dc.Keys = devKeys && playUsesPlayer && !editor.ScriptIDEHasKeyboardFocus();
        dc.Interact = gameHasInput && playUsesPlayer && !playerVitals.IsDead() && InputMap::GetButtonDown("Interact");
        dc.StartSquad = [&]() {
            auto fps = world.Registry.view<FirstPersonControllerComponent>();
            const FirstPersonControllerComponent* fp = fps.begin() != fps.end() ? &fps.get<FirstPersonControllerComponent>(*fps.begin()) : nullptr;
            return fp && npcDirector.Start(world, assets, /*allowEmpty=*/true);
        };
        if (playUsesPlayer) devSpawner.Update(dc);
    }
    // Doors, drawers and lids (Openable) and light switches / lamps (LightSwitch): Interact works whichever of them is
    // nearest under the crosshair; the openables all move.
    {
        const bool interact = gameHasInput && playUsesPlayer && !playerVitals.IsDead() && InputMap::GetButtonDown("Interact");
        float doorAt = 1e30f, switchAt = 1e30f;
        if (interact) {
            if (Openable::Pick(world, player.Cam.Position, player.Cam.Front(), &doorAt) == entt::null) doorAt = 1e30f;
            if (LightSwitch::Pick(world, player.Cam.Position, player.Cam.Front(), &switchAt) == entt::null) switchAt = 1e30f;
        }
        Openable::Update(world, player.Cam.Position, player.Cam.Front(), interact && doorAt <= switchAt, gameDt);
        LightSwitch::Update(world, player.Cam.Position, player.Cam.Front(), interact && switchAt < doorAt);
    }
    // G = explosion shockwave at the player. A separate physics *debug/test* tool
    // (not part of the gravity gun above) — stays behind Gizmos > Physics debug input.
    if (gameHasInput && playUsesPlayer && EditorSettings::Get().PhysicsDebugInput) {
        if (Input::IsKeyPressed(GLFW_KEY_G)) {
            const float c[3] = {player.Cam.Position.x,
                                player.Cam.Position.y - player.EyeHeight,
                                player.Cam.Position.z};
            PhysicsWorld::AddExplosionForce(c, 7.0f, 22.0f, 0.4f);
        }
    }
    // Step the PhysX world (#185): kinematic bodies pushed from their Transforms,
    // then the sim, then dynamic bodies' poses written back into theirs.
    // Everything in this block runs on the time-scaled step (#169: slow-mo used to
    // reach physics only). The game module's FixedUpdate rides the physics sub-steps.
    {
    PROFILE_SCOPE("Physics Step");
    PhysicsWorld::Step(gameDt, world,
                       [&](float fixedDt) { gameModule.FixedTick(world, fixedDt); });
    }
    if (playUsesPlayer) {
        PROFILE_SCOPE("Player + FP Update");
        // The death camera's drop comes off before anything reads the camera as the player's.
        player.Cam.Position -= deathCamOffset;
        player.Cam.Roll -= deathCamRoll;
        deathCamOffset = glm::vec3(0.0f);
        deathCamRoll = 0.0f;
        // The weapon's view punch / lean comes off before the player integrates
        // its own look and position, and goes back on in Update below.
        firstPersonPresentation.RemoveViewKick(player.Cam);
        // Last frame's ADS zoom: the look slows with the view so the sights track the same.
        player.MouseSensitivity = playBaseSensitivity * firstPersonPresentation.LookScale(playBaseFov);
        firstPersonBody.BeforePlayerMove(player, player.Cam); // camera out of the head, root motion in
        if (npcTest) npcTest->Drive(player, firstPersonPresentation, npcDirector, playerVitals, gameDt);
        player.AimHeld=firstPersonPresentation.IsActive() && firstPersonPresentation.IsEquipped() && !playerVitals.IsDead() &&
            ((gameHasInput && InputMap::GetButton("Fire2")) || (npcTest && npcTest->Aiming()));
        player.SprintBlocked=firstPersonPresentation.ActionBlocksSprint();
        // TEMPORARY sizing (diag only): TARTARUS_DIAG_WALK=1 walks a fixed path from Play start (segments 0-5).
#pragma warning(suppress : 4996)
        static const bool diagWalk = std::getenv("TARTARUS_DIAG_WALK") != nullptr;
        static double diagT = 0.0;
        const glm::vec3 diagFrom = player.Cam.Position;
        const float diagYaw0 = player.Cam.Yaw;
        int diagSeg = -1;
        if (diagWalk) {
            diagT += gameDt;
            const double t = diagT;
            const int sg = t < 5.0 ? 0 : t < 15.0 ? 1 : t < 25.0 ? 2 : t < 37.0 ? 3 : t < 42.0 ? 4 : t < 47.0 ? 5 : 6;
            diagSeg = sg;
            player.ScriptedMove = true;
            player.ScriptMove = (sg >= 1 && sg <= 3) ? glm::vec2(0.0f, 1.0f) : glm::vec2(0.0f);
            player.ScriptSprint = player.ScriptRun = (sg == 3);
            player.Cam.Yaw += sg == 2 ? 30.0f * gameDt : sg == 4 ? 90.0f * gameDt : 0.0f;
        }
        player.Update(gameDt, gameHasInput && !playerVitals.IsDead());
        if (diagWalk) {
            const glm::vec3 d = player.Cam.Position - diagFrom;
            DiagW1Walk(diagSeg, std::sqrt(d.x * d.x + d.z * d.z), std::abs(std::remainder(player.Cam.Yaw - diagYaw0, 360.0f)), gameDt);
        }
        // Before the body places itself: it is the controller's child, posed in world space.
        if (playControllerEntity != entt::null && world.Registry.valid(playControllerEntity)) {
            const float yaw = glm::radians(player.Cam.Yaw); // Camera forward = (cos, 0, sin)
            world.SetWorldPose(playControllerEntity, player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f),
                               glm::angleAxis(std::atan2(-std::cos(yaw), -std::sin(yaw)), glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        // An outfit piece changed in Play: take the new pieces. A rebaked motion database: take the new database.
        if (firstPersonBody.OutfitChanged(world) || firstPersonBody.MotionStale()) {
            firstPersonBody.Restart(world, player);
        } else if (firstPersonBody.OutfitLookChanged(world)) { // only a colourway: the twins take it, no rebuild
            firstPersonBody.SyncLook(world);
        }
        firstPersonBody.Tick(world, player, player.Cam, gameDt);
        if (firstPersonPresentation.IsActive()) {
            if (!npcTest) firstPersonPresentation.SetPlayerLookInput(player.LookDeltaInput);
            if (!npcTest) firstPersonPresentation.SetPlayerMoveInput(player.MoveInput);
            firstPersonPresentation.Update(world, player.Cam);
            firstPersonPresentation.SetCrouch(player.CrouchBlend); // the crouched ADS pose eases in with the eye height
            // The weapon owns Fire1/Fire2/FireMode/Reload/Inspect/Melee while it's in
            // hand. With the gravity gun on the controller, holstering hands the mouse
            // to it instead (gravityGunLive), so the two never read the same frame.
            // Weapon1..Weapon4/Holster always switch between them; F is the flashlight.
            const bool weaponInput = gameHasInput && !GravityGunLive(in) && !playerVitals.IsDead();
            if (gameHasInput && !playerVitals.IsDead()) {
                const bool wasEquipped = firstPersonPresentation.IsEquipped();
                // 1 = the Animation Set, 2 = the secondary, 3 = the sidearm, 4 = unarmed (the gravity gun).
                static constexpr const char* kSlotKeys[] = {"Weapon1", "Weapon2", "Weapon3"};
                for (int key = 0; key < 3; ++key)
                    if (const int slot = firstPersonPresentation.SlotForKey(key); slot >= 0 && InputMap::GetButtonDown(kSlotKeys[key]))
                        firstPersonPresentation.SelectSlot(slot);
                if (InputMap::GetButtonDown("Weapon4")) firstPersonPresentation.SetEquipped(false);
                if (InputMap::GetButtonDown("Flashlight")) flashlight.Toggle(player.Cam.Position);
                // The wheel is the gravity gun's hold distance, so it only switches without one:
                // through the weapons, then unarmed.
                if (!playGravityGun && Input::GetScrollDeltaY() != 0.0)
                    firstPersonPresentation.CycleSlot(Input::GetScrollDeltaY() < 0.0 ? 1 : -1);
                if (InputMap::GetButtonDown("Holster"))
                    firstPersonPresentation.SetEquipped(!firstPersonPresentation.IsEquipped());
                // Drawing the weapon drops whatever the gravity gun was holding.
                if (playGravityGun && !wasEquipped && firstPersonPresentation.IsEquipped()) {
                    if (PhysicsWorld::IsGrabbing()) PhysicsWorld::ReleaseBody(false, nullptr);
                    gravityGun.Reset();
                }
            }
            if (weaponInput) {
                if (InputMap::GetButtonDown("FireMode")) firstPersonPresentation.ToggleFireMode();
                firstPersonPresentation.UpdateTrigger(InputMap::GetButtonDown("Fire1"),
                                                      InputMap::GetButton("Fire1"));
                // R is tap-to-reload, hold-to-check-the-magazine.
                firstPersonPresentation.UpdateReloadKey(InputMap::GetButton("Reload"), gameDt);
                if (InputMap::GetButtonDown("Inspect")) firstPersonPresentation.TriggerAction("Inspect");
                if (InputMap::GetButtonDown("Melee")) firstPersonPresentation.TriggerAction("Melee");
            } else {
                firstPersonPresentation.ResetReloadKey(); // focus lost mid-press: never resolve it as a tap
            }
            if (npcTest && !playerVitals.IsDead()) {
                firstPersonPresentation.UpdateTrigger(npcTest->Firing(), npcTest->Firing());
                if (npcTest->WantsReload()) firstPersonPresentation.Reload();
                // Here, not in Drive: Update (above) drops any trigger set before it.
                if (!npcTest->Action().empty()) firstPersonPresentation.TriggerAction(npcTest->Action());
            }
            // No lean key: aiming at a corner peeks around it (the presentation's corner peek).
            // Shift is the jog, and the gun stays up through it: the arms never take the lowered sprint. The jog's
            // pace shows in the walk layer's rate and the bob blending toward its jog settings by speed.
            {
                float gait = 0.0f;
                const bool haveGait = firstPersonBody.GaitPhase(gait);
                firstPersonPresentation.SetGaitPhase(haveGait, gait);
                // Riding the mocap head (MM Head Ride), the camera bobs as captured: no procedural bob on top.
                firstPersonPresentation.SetCameraBobScale(1.0f - firstPersonBody.HeadRide());
            }
            firstPersonPresentation.SetJogging(!player.Crouched && ((gameHasInput && InputMap::GetButton("Sprint")) ||
                                                                 (npcTest && player.ScriptedMove && player.ScriptSprint)));
            firstPersonPresentation.Tick(gameDt, player.Velocity, false,
                                         (weaponInput && InputMap::GetButton("Fire2")) || (npcTest && npcTest->Aiming()), 0.0f,
                                         player.Grounded);
            // This frame's recoil on the view (Tick just worked it out), not last frame's.
            firstPersonPresentation.RefreshViewKick(player.Cam);
        }
        player.Cam.Fov = firstPersonPresentation.WorldFov(playBaseFov);
    }
    // The training dummies (scenes with Dummy Spawns): they wander and move before the animators pose everyone.
    if (npcDirector.Active()) {
        PROFILE_SCOPE("Dummies");
        playerSnap = PlayerSnapshot{};
        playerSnap.Valid = playUsesPlayer && PhysicsWorld::HasCharacter();
        if (playerSnap.Valid) {
            float foot[3], r = 0.3f, cyl = 0.6f;
            PhysicsWorld::GetCharacterCapsule(foot, &r, &cyl);
            playerSnap.Feet = glm::vec3(foot[0], foot[1], foot[2]);
            playerSnap.Radius = r;
            playerSnap.Height = 2.0f * (r + cyl);
            playerSnap.Eye = player.Cam.Position;
            playerSnap.Velocity = player.Velocity;
            playerSnap.Forward = player.Cam.Front();
        }
        npcDirector.Think(world, assets, gameDt, playerSnap);
        if (devPanel.InfiniteAmmo && firstPersonPresentation.IsActive()) firstPersonPresentation.SetAmmo(firstPersonPresentation.MagazineSize());
    }
    // The Play-mode camera is the ears: positional sources (#201) attenuate and pan
    // against wherever the player is looking from, updated after the move so the
    // listener matches the frame that's about to be rendered.
    // #171 - or from an Audio Listener entity, when the scene has one.
    {
        PROFILE_SCOPE("Audio Listener");
        const entt::entity listener = FindActiveAudioListener(world);
        glm::vec3 earPos = gameCam->Position;
        if (listener != entt::null) {
            const glm::mat4 lm = world.ComposeWorldTransform(listener);
            earPos = glm::vec3(lm[3]);
            const glm::vec3 fwd = -glm::vec3(lm[2]), up = glm::vec3(lm[1]);
            AudioEngine::SetListener(glm::vec3(lm[3]),
                                     glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0, 0, -1),
                                     glm::length(up) > 1e-6f ? glm::normalize(up) : glm::vec3(0, 1, 0));
        } else {
            AudioEngine::SetListener(gameCam->Position, gameCam->Front(), gameCam->Up());
        }
        editor.UpdatePlayModeAudio(world, gameDt, earPos);
    }
    // Procedural spin/orbit/bob/light-hue. Play-only: edit mode keeps the authored
    // pose, and the play-mode snapshot restores everything this touched on Stop.
    UpdateAnimators(world, gameDt);
    {
    PROFILE_SCOPE("Animators");
    UpdateSkeletalAnimations(world, assets, gameDt); // #175 — Animation components drive their models' clips
    PROFILE_SCOPE("Animator Controllers");
    UpdateAnimatorControllers(world, assets, gameDt); // #175 Part B — state machines
    }
    // The arms are posed now (clips + IK): seat the gun in this frame's hands.
    if (playUsesPlayer) {
        // The pose work (IK, arms, the world gun) in one scope, the shots after it in their own: --perf-bench's HALF
        // report counts the first as anim and the second as sim.
        {
        PROFILE_SCOPE("First Person IK");
        {
        PROFILE_SCOPE("FP Body LateUpdate");
        firstPersonBody.LateUpdate(world, player.Cam, gameDt, firstPersonPresentation.ArmsEntity(),
                                   firstPersonPresentation.CameraBone()); // camera into the body's head
        }
        {
        PROFILE_SCOPE("FP Presentation LateUpdate");
        firstPersonPresentation.LateUpdate(world, player.Cam);
        }
        PROFILE_SCOPE("FP Arms + World Gun");
        // The body's hands onto the arms rig's, now that the rig is seated.
        // Split poses: the body's world twins hold the world gun, the first-person one carried to its
        // third-person hold.
        // A reload's pouch and shells: the world body's free hand held to its chest (FirstPersonBody's hand anchor).
        FirstPersonHandAnchor handAnchor;
        const bool haveAnchor = firstPersonPresentation.IsActive() && firstPersonPresentation.HandAnchor(handAnchor);
        firstPersonBody.ArmsLateUpdate(world, firstPersonPresentation.ArmsEntity(),
                                       firstPersonPresentation.ViewModelFov(), gameDt, &player.Cam,
                                       firstPersonPresentation.ThirdPersonGunCorrection(),
                                       firstPersonPresentation.ThirdPersonArms(), firstPersonPresentation.GunSocket());
        if (firstPersonPresentation.IsActive()) {
            glm::mat4 holdingHand(1.0f);
            const bool haveHolding = haveAnchor && firstPersonBody.BoneWorldMatrix(world, handAnchor.Hand, holdingHand);
            firstPersonPresentation.PlaceWorldWeapon(world, firstPersonBody.SplitPoses(), firstPersonBody.WorldGunDelta(),
                                                     haveHolding ? &holdingHand : nullptr);
        }
        if (weaponLab) weaponLab->AfterPose(world, firstPersonBody, firstPersonPresentation, player.Cam, gameDt);
        }
        // This frame's rounds, down the bore from the muzzle: a hole where each struck.
        PROFILE_SCOPE("Player Shots + Impacts");
        for (const FirstPersonPresentation::ShotHit& hit : firstPersonPresentation.TakeShotHits()) {
            if (npcDirector.Active()) {
                bool killed = false, head = false, aliveWhenHit = false;
                if (npcDirector.OnPlayerHit(world, hit.Entity, hit.Point, hit.Origin, hit.Direction,
                                            firstPersonPresentation.Set().Gameplay, &killed, &head, &aliveWhenHit)) {
                    if (aliveWhenHit) combatFx.Play(killed ? CombatFx::Cue::HitmarkerKill : CombatFx::Cue::Hitmarker, hit.Point);
                    continue; // a dummy, not a wall: no hole
                }
            }
            combatFx.Impact(world, hit.Entity, glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]));
            worldImpact(hit.Entity, hit.Point, hit.Normal, glm::vec3(hit.Direction[0], hit.Direction[1], hit.Direction[2]), hit.HoleRadius);
        }
        // Every pellet's line: the report.
        for (const FirstPersonPresentation::ShotTrace& t : firstPersonPresentation.TakeShotTraces()) {
            if (t.FirstPellet)
                combatFx.Shot(world, firstPersonPresentation.Set().Gameplay.Pellets > 1 ? CombatFx::Gun::Shotgun : CombatFx::Gun::Rifle,
                              t.Origin, t.End, true, false, 0, firstPersonPresentation.MuzzleEffects());
        }
        // The player's muzzle flames (this frame's too) ride the gun as it kicks and turns.
        if (glm::vec3 fpMuzzle, worldMuzzle, bore; combatFx.Active() && firstPersonPresentation.IsActive() &&
                                                   firstPersonPresentation.MuzzleFrames(fpMuzzle, worldMuzzle, bore))
            combatFx.FollowMuzzle(world, fpMuzzle, worldMuzzle, bore);
        // The spent cases out of the port, then every case on the ground moved on.
        for (const CasingSpawn& spawn : firstPersonPresentation.TakeEjections()) {
            if (npcTest) npcTest->OnPlayerEjection(spawn.Model);
            shellCasings.Spawn(world, assets, spawn);
        }
        {
        PROFILE_SCOPE("Shell Casings");
        shellCasings.Update(world, gameDt, player.Cam, player.Cam.Position);
        }
    }
    // The dummies' late pose (foot IK, the look, the hitboxes on the bones), then what the player's rounds did to them.
    if (npcDirector.Active()) {
        PROFILE_SCOPE("Dummies Late");
        npcDirector.LateUpdate(world, gameDt, playerSnap);
        PROFILE_SCOPE("Dummy Flesh Hits");
        // Every round that went into a dummy this frame bleeds.
        for (const NpcDirector::FleshHit& f : npcDirector.TakeFleshHits()) {
            BloodFx::Hit h;
            h.Point = f.Point;
            h.Direction = f.Direction;
            h.Entity = f.Entity;
            h.Part = f.Part;
            h.Damage = f.Damage;
            h.Killed = f.Killed;
            h.Head = f.Head;
            h.Corpse = f.Corpse;
            h.Player = f.Entity == kPlayerEntity;
            h.Pellets = f.Pellets;
            h.Origin = f.Origin;
            h.ByPlayer = f.ByPlayer;
            bloodFx.OnFleshHit(h);
            // A big headshot kill: the wet crunch over the flesh hit (AE Master's gore takes).
            if (bloodFx.Config.Enabled && BloodFx::BigHeadshot(h, bloodFx.LastEnergy(), bloodFx.Config.Gore)) {
                ImpactAudio::Get().Gore(h.Point, 1.0f);
                ++bigHeadshots;
            }
        }
        if (npcTest) npcTest->After(world, npcDirector, playerVitals, player);
    } else if (npcTest && npcTest->RunsWithoutSquad()) {
        npcTest->After(world, npcDirector, playerVitals, player);
    }
    combatFx.SetListener(player.Cam.Position, player.Cam.Front());
    {
    PROFILE_SCOPE("Combat FX + Foley");
    combatFx.Update(world, gameDt); // (a no-op until Start: runs with or without a squad)
    if (playUsesPlayer && combatFx.Active()) {
        FoleyPlayerInput fi;
        fi.Velocity = player.Velocity;
        fi.Feet = player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
        if (float foot[3], r = 0.3f, cyl = 0.6f; PhysicsWorld::HasCharacter()) {
            PhysicsWorld::GetCharacterCapsule(foot, &r, &cyl);
            fi.Feet = glm::vec3(foot[0], foot[1], foot[2]);
        }
        fi.Grounded = player.Grounded;
        fi.Sprinting = !player.Crouched && gameHasInput && InputMap::GetButton("Sprint"); // the jog's steps
        fi.Crouched = player.Crouched;
        fi.Jumped = player.Jumped;
        fi.HaveFootHeights = firstPersonBody.FootHeights(world, fi.FootHeight); // steps on the body's touch-downs
        if (firstPersonPresentation.IsActive()) {
            fi.WalkStride = firstPersonPresentation.Set().Procedural.Bob.WalkStride;
            fi.SprintStride = firstPersonPresentation.Set().Procedural.Bob.SprintStride;
        }
        FoleyAudio::Get().UpdatePlayer(world, gameDt, fi);
    }
    }
    {
        PROFILE_SCOPE("Blood");
        bloodFx.SetViewer(player.Cam.Position, player.Cam.Front()); // stains leave only out of view
        bloodFx.Update(gameDt);
        fxSprites.Update(gameDt);
        BloodRenderer::Get().BeginFrame();
        bloodFx.Submit(BloodRenderer::Get());
        bulletHoles.Submit(world); // every hole a clustered decal, following what it's in (after the blood's BeginFrame)
        if (playUsesPlayer && gameHasInput) sprayCans.PlayerKeys(world, player.Cam.Position, player.Cam.Front(), Shortcuts::TriggeredGlfw("dev.sprayCan"));
        sprayCans.Update(world, gameDt, player.Cam.Position);
        FxSpriteRenderer::Get().BeginFrame();
        fxSprites.Submit(FxSpriteRenderer::Get());
        if (npcTest) {
            npcTestBlood.Loaded = BloodRenderer::Get().Loaded();
            npcTestBlood.SpraysSpawned = bloodFx.SpraysSpawned();
            npcTestBlood.ActiveSprays = (int)bloodFx.Sprays().size();
            npcTestBlood.LastSprayClipped = bloodFx.LastSprayClipped();
            npcTestBlood.DecalsSpawned = bloodFx.DecalsSpawned();
            npcTestBlood.PoolsSpawned = bloodFx.PoolsSpawned();
            npcTestBlood.SplatsSpawned = bloodFx.SplatsSpawned();
            npcTestBlood.Puffs = bloodFx.PuffsSpawned();
            npcTestBlood.LastExited = bloodFx.LastExited();
            npcTestBlood.LastExitFound = bloodFx.LastExitFound();
            npcTestBlood.BigHeadshots = bigHeadshots;
            npcTestBlood.Impacts = impactFx.Spawned();
            npcTestBlood.TexturedHoles = 0;
            {
                std::vector<BulletHoleList::Placed>& all = m_PlacedHoles;
                bulletHoles.Resolve(world, all);
                for (const auto& hole : all) npcTestBlood.TexturedHoles += hole.Decal >= 0;
            }
            npcTestBlood.GearSplats = 0;
            for (const BloodFx::Splat& s : bloodFx.Splats()) npcTestBlood.GearSplats += s.Group == kPlayerEntity;
            for (const Profiler::Entry& e : Profiler::GetLastFrameGpu()) {
                if (e.Name != "Blood Sprays") continue;
                npcTestBloodGpuSum += e.Milliseconds;
                ++npcTestBloodGpuFrames;
                npcTestBlood.GpuMsMax = std::max(npcTestBlood.GpuMsMax, e.Milliseconds);
            }
            npcTestBlood.GpuMsAvg = npcTestBloodGpuFrames ? npcTestBloodGpuSum / (float)npcTestBloodGpuFrames : 0.0f;
        }
    }
    if (playUsesPlayer) {
        playerVitals.Tick(gameDt);
        if (playerVitals.JustDied()) {
            firstPersonPresentation.SetEquipped(false);
            if (PhysicsWorld::IsGrabbing()) PhysicsWorld::ReleaseBody(false, nullptr);
            gravityGun.Reset();
        }
        if (playerVitals.WantsRespawn()) {
            // Back at the spawn, full health and ammo, weapon drawn, a moment untouchable.
            const float f[3] = {player.RespawnFeet.x, player.RespawnFeet.y, player.RespawnFeet.z};
            PhysicsWorld::SetCharacterFootPosition(f);
            player.Velocity = glm::vec3(0.0f);
            player.Cam.Position = player.RespawnFeet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
            if (playControllerEntity != entt::null && world.Registry.valid(playControllerEntity))
                YawPitchFromWorld(world.ComposeWorldTransform(playControllerEntity), player.Cam.Yaw, player.Cam.Pitch);
            playerVitals.Respawned();
            firstPersonPresentation.RefillAmmo();
            firstPersonPresentation.SetEquipped(true);
        }
        // Dead: the view sinks to the floor and rolls over, on the camera only (taken off
        // again at the top of the next frame).
        if (playerVitals.IsDead()) {
            const float t = std::clamp(playerVitals.DeadTime() / 0.9f, 0.0f, 1.0f);
            const float ease = t * t * (3.0f - 2.0f * t);
            deathCamOffset = glm::vec3(0.0f, -(player.EyeHeight - 0.35f) * ease, 0.0f);
            deathCamRoll = 32.0f * ease;
            player.Cam.Position += deathCamOffset;
            player.Cam.Roll += deathCamRoll;
        }
    }
}

void PlayFrame::Advance(const PlayFrameInputs& in) {
    World& world = *in.world;
    HotReloadGameModule& gameModule = *in.gameModule;
    const float dt = in.dt, gameDt = in.gameDt;
    const bool simThisFrame = in.simThisFrame;
    const bool playing = in.play->Playing;
    const bool editorUIVisible = in.play->EditorUIVisible;
    const std::uint64_t frameIndex = in.frameIndex;

    // The gameplay DLL watches its freshly-built source copy even while editing, and
    // only runs game systems during Play. Rebuilding TartarusGame swaps the module
    // without closing the editor or discarding this World.
    {
        PROFILE_SCOPE("Game Module Tick");
        gameModule.Tick(world, gameDt, dt, simThisFrame);
    }

    // #177 - particles run while editing too (real time) so an emitter can be tuned live;
    // in Play they follow the game clock and freeze while paused.
    {
        PROFILE_SCOPE("Particles");
        UpdateParticleSystems(world, playing ? (simThisFrame ? gameDt : 0.0f) : dt);
    }

    // Animations advance whenever something is showing them: the editor viewport, or the
    // running game.
    if (editorUIVisible || playing) {
        PROFILE_SCOPE("Animation Update");
        // Advance each distinct Model once. Scene entities get their own instance
        // (AssetLibrary::InstantiateModel), so this is normally 1:1 — but dedupe
        // defensively so a future shared-Model path can't tick one player N*dt (#106).
        // Model::TickAnimationOnce() compares against its own m_LastTickedFrame instead
        // of this loop building a heap-allocated std::unordered_set<Model*> every frame.
        // Play: the game's scaled step, frozen while paused. Edit: real time, so a
        // preview isn't slowed by a time scale left over from the last run.
        const float animDt = playing ? (simThisFrame ? gameDt : 0.0f) : dt;
        // never-animating models left out (StillModelCache); Pre-Skin below reuses this gather
        for (auto entity : in.stillModels->GatherCached(world, frameIndex)) {
            if (world.Registry.all_of<InactiveTag>(entity)) continue; // #199 - an inactive object is paused, not just hidden
            world.Registry.get<RenderableComponent>(entity).ModelRef->TickAnimationOnce(frameIndex, animDt);
        }
    }
}
