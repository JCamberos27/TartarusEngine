#include "PlayFrame.h"

#include <algorithm>
#include <cmath>
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
#include "CombatHud.h"
#include "CombatWiring.h"
#include "Components.h"
#include "Core/Env.h"
#include "Creatures/SpiderDamage.h"
#include "Creatures/SpiderSystem.h"
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

PlayFrame::PlayFrame() {
#pragma warning(suppress : 4996)
    m_TraceSpiderHits = Env::Flag(Env::Var::SpiderTrace);
}

bool PlayFrame::GravityGunLive(const PlayFrameInputs& in) {
    return in.play->GravityGun && !(in.firstPersonPresentation->IsActive() && in.firstPersonPresentation->IsEquipped());
}

void PlayFrame::Simulate(const PlayFrameInputs& in) {
    // main's names for what the step reads, so the step reads as it did there.
    World& world = *in.world;
    AssetLibrary& assets = *in.assets;
    Window& window = *in.window;
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
    SpiderSystem& spiders = *in.spiders;
    SpiderDamage& spiderDamage = *in.spiderDamage;
    DevSpawner& devSpawner = *in.devSpawner;
    DevPanel& devPanel = *in.devPanel;
    CombatFx& combatFx = *in.combatFx;
    CombatHud& combatHud = *in.combatHud;
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
    int& playerAmmoSeen = m_PlayerAmmoSeen;
    int& playerSlotSeen = m_PlayerSlotSeen;
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
        dc.Spiders = &spiders;
        dc.SpiderHurt = &spiderDamage;
        dc.Gun = &firstPersonPresentation;
        dc.Eye = player.Cam.Position;
        dc.Front = player.Cam.Front();
        dc.Dt = gameDt;
        dc.Keys = devKeys && playUsesPlayer && !editor.ScriptIDEHasKeyboardFocus();
        dc.Interact = gameHasInput && playUsesPlayer && !playerVitals.IsDead() && InputMap::GetButtonDown("Interact");
        dc.StartSquad = [&]() {
            auto fps = world.Registry.view<FirstPersonControllerComponent>();
            const FirstPersonControllerComponent* fp = fps.begin() != fps.end() ? &fps.get<FirstPersonControllerComponent>(*fps.begin()) : nullptr;
            return fp && npcDirector.Start(world, assets, fp, /*allowEmpty=*/true);
        };
        if (playUsesPlayer) devSpawner.Update(dc);
    }
    // Doors, drawers and lids (Openable): Interact opens the one under the crosshair; all of them move.
    Openable::Update(world, player.Cam.Position, player.Cam.Front(),
                     gameHasInput && playUsesPlayer && !playerVitals.IsDead() && InputMap::GetButtonDown("Interact"),
                     gameDt);
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
        player.Update(gameDt, world, window.Handle(), gameHasInput && !playerVitals.IsDead());
        // Before the body places itself: it is the controller's child, posed in world space.
        if (playControllerEntity != entt::null && world.Registry.valid(playControllerEntity)) {
            const float yaw = glm::radians(player.Cam.Yaw); // Camera forward = (cos, 0, sin)
            world.SetWorldPose(playControllerEntity, player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f),
                               glm::angleAxis(std::atan2(-std::cos(yaw), -std::sin(yaw)), glm::vec3(0.0f, 1.0f, 0.0f)));
        }
        if (firstPersonBody.OutfitChanged(world)) { // an outfit piece changed in Play: take the new pieces
            firstPersonBody.Stop(world);
                    firstPersonBody.Start(world, player);
        }
        firstPersonBody.SetHands(firstPersonPresentation.IsActive() && firstPersonPresentation.IsEquipped(),
                                 firstPersonPresentation.IsActive() && firstPersonPresentation.HandsBusy());
        firstPersonBody.Tick(world, player, player.Cam, gameDt);
        if (firstPersonPresentation.IsActive()) {
            if (!npcTest) firstPersonPresentation.SetPlayerLookInput(player.LookDeltaInput);
            if (!npcTest) firstPersonPresentation.SetPlayerMoveInput(player.MoveInput);
            firstPersonPresentation.Update(world, player.Cam);
            firstPersonPresentation.SetCrouch(player.CrouchBlend); // the crouched ADS pose eases in with the eye height
            // The weapon owns Fire1/Fire2/FireMode/Reload/Inspect/Melee while it's in
            // hand. With the gravity gun on the controller, holstering hands the mouse
            // to it instead (gravityGunLive), so the two never read the same frame.
            // Weapon1/Weapon2/Weapon3/Holster always switch between them; F is the flashlight.
            const bool weaponInput = gameHasInput && !GravityGunLive(in) && !playerVitals.IsDead();
            if (gameHasInput && !playerVitals.IsDead()) {
                const bool wasEquipped = firstPersonPresentation.IsEquipped();
                // 1 = the Animation Set, 2 = the Secondary Animation Set, 3 = unarmed (the gravity gun).
                if (InputMap::GetButtonDown("Weapon1")) firstPersonPresentation.SelectSlot(0);
                if (InputMap::GetButtonDown("Weapon2")) firstPersonPresentation.SelectSlot(1);
                if (InputMap::GetButtonDown("Weapon3")) firstPersonPresentation.SetEquipped(false);
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
            }
            // No lean key: aiming at a corner peeks around it (the presentation's corner peek).
            firstPersonPresentation.Tick(gameDt, player.Velocity, gameHasInput && InputMap::GetButton("Sprint"),
                                         (weaponInput && InputMap::GetButton("Fire2")) || (npcTest && npcTest->Aiming()), 0.0f,
                                         player.Grounded);
            // This frame's recoil on the view (Tick just worked it out), not last frame's.
            firstPersonPresentation.RefreshViewKick(player.Cam);
        }
        player.Cam.Fov = firstPersonPresentation.WorldFov(playBaseFov);
    }
    // The enemy squad (scenes with NPC Spawns): they see the player as they are after this
    // move, and decide, move and aim before the animators pose everyone.
    if (npcDirector.Active()) {
        PROFILE_SCOPE("Enemy AI");
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
            playerSnap.Crouched = player.Crouched;
            playerSnap.Dead = playerVitals.IsDead();
            playerSnap.Health = playerVitals.Health01();
            playerSnap.Sprinting = glm::length(glm::vec2(player.Velocity.x, player.Velocity.z)) > player.MoveSpeed * 1.1f;
            if (firstPersonPresentation.IsActive()) {
                const int ammo = firstPersonPresentation.Ammo(), slot = firstPersonPresentation.Slot();
                playerSnap.Fired = slot == playerSlotSeen && playerAmmoSeen >= 0 && ammo < playerAmmoSeen;
                playerAmmoSeen = ammo;
                playerSlotSeen = slot;
                playerSnap.Reloading = firstPersonPresentation.IsReloading();
            }
        }
        if (devPanel.Invisible) playerSnap.Valid = false; // dev panel: the squad can't see or hear the player
        npcDirector.Think(world, assets, gameDt, playerSnap);
        if (devPanel.InfiniteAmmo && firstPersonPresentation.IsActive()) {
            firstPersonPresentation.SetAmmo(firstPersonPresentation.MagazineSize());
            playerAmmoSeen = firstPersonPresentation.Ammo();
        }

    }
    // The Play-mode camera is the ears: positional sources (#201) attenuate and pan
    // against wherever the player is looking from, updated after the move so the
    // listener matches the frame that's about to be rendered.
    // #171 - or from an Audio Listener entity, when the scene has one.
    {
        entt::entity listener = entt::null;
        int bestOrder = 0x7fffffff;
        for (auto e : world.Registry.view<const AudioListenerComponent, const TransformComponent>()) {
            if (!world.Registry.get<AudioListenerComponent>(e).Enabled || world.Registry.all_of<InactiveTag>(e)) continue;
            const auto* ord = world.Registry.try_get<OrderComponent>(e);
            const int o = ord ? ord->Value : 0;
            if (o < bestOrder) { bestOrder = o; listener = e; }
        }
        if (listener != entt::null) {
            const glm::mat4 lm = world.ComposeWorldTransform(listener);
            const glm::vec3 fwd = -glm::vec3(lm[2]), up = glm::vec3(lm[1]);
            AudioEngine::SetListener(glm::vec3(lm[3]),
                                     glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0, 0, -1),
                                     glm::length(up) > 1e-6f ? glm::normalize(up) : glm::vec3(0, 1, 0));
        } else {
            AudioEngine::SetListener(gameCam->Position, gameCam->Front(), gameCam->Up());
        }
        editor.UpdatePlayModeAudio(world, gameDt,
            listener != entt::null ? glm::vec3(world.ComposeWorldTransform(listener)[3]) : gameCam->Position);
    }
    // The spiders move over the surfaces before the animators pose them; the hunters among them go after
    // the player, and what their bites and pounces do lands on the player now.
    {
        SpiderSystem::Target prey;
        prey.Valid = playUsesPlayer && PhysicsWorld::HasCharacter() && !devPanel.Invisible;
        if (prey.Valid) {
            float foot[3], r = 0.3f, cyl = 0.6f;
            PhysicsWorld::GetCharacterCapsule(foot, &r, &cyl);
            prey.Feet = glm::vec3(foot[0], foot[1], foot[2]);
            prey.Radius = r;
            prey.Eye = player.Cam.Position;
            prey.Velocity = player.Velocity;
            prey.Dead = playerVitals.IsDead();
            prey.Loud = glm::length(glm::vec2(player.Velocity.x, player.Velocity.z)) > player.MoveSpeed * 1.1f;
        }
        spiders.Think(world, assets, gameDt, &prey);
        for (const SpiderSystem::PlayerHit& h : spiders.TakePlayerHits()) {
            const float before = playerVitals.Health();
            playerVitals.ApplyDamage(h.Amount, h.Source);
            // The shove: along the ground, up to its speed - a bite on a player already reeling back doesn't add
            // to it, so a flurry staggers them rather than throwing them across the level.
            const glm::vec3 wasGoing = player.Velocity;
            player.Velocity = SpiderSystem::Shove(player.Velocity, h.Push);
            const bool traceHits = m_TraceSpiderHits;
            if (traceHits)
                std::printf("[Spider] hit the player at %.2f %.2f: %s, push %.2f %.2f, ground speed %.2f -> %.2f, grounded %d, decel %.2f s, air %.2f s\n",
                            player.Cam.Position.x, player.Cam.Position.z, h.Pounce ? "pounce" : "bite/drop", h.Push.x,
                            h.Push.z, glm::length(glm::vec2(wasGoing.x, wasGoing.z)),
                            glm::length(glm::vec2(player.Velocity.x, player.Velocity.z)), (int)player.Grounded,
                            player.GroundDecelTime, player.AirAccelTime);
            // The jolt: the view knocked up and aside, harder for a pounce.
            const float side = glm::dot(glm::normalize(glm::vec3(h.Push.x, 0.0f, h.Push.z) + glm::vec3(1e-4f, 0.0f, 0.0f)),
                                        player.Cam.Right());
            player.Cam.Pitch += h.Pounce ? 9.0f : 5.0f;
            player.Cam.Yaw += (side >= 0.0f ? 1.0f : -1.0f) * (h.Pounce ? 7.0f : 4.0f);
            if (playerVitals.Health() < before) combatFx.Play(CombatFx::Cue::FleshHit, prey.Eye, true, 0.8f);
        }
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
    // The spiders' legs onto this frame's footholds, over the pose their animators just gave them.
    spiders.LatePose(world, gameDt);
    spiderDamage.LateUpdate(world, spiders, gameDt); // flinches, hitboxes, the dead onto their ragdolls
    // The arms are posed now (clips + IK): seat the gun in this frame's hands.
    if (playUsesPlayer) {
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
        // third-person hold and by their chest.
        // The shoulder lock seats that world gun on the twin's shoulder (FirstPersonGunSeat).
        FirstPersonWorldGunInput worldGun;
        const bool haveWorldGun = firstPersonPresentation.IsActive() && firstPersonPresentation.WorldGunInput(worldGun);
        // A reload's pouch and shells: the world body's free hand held to its chest (FirstPersonBody's hand anchor).
        FirstPersonHandAnchor handAnchor;
        const bool haveAnchor = firstPersonPresentation.IsActive() && firstPersonPresentation.HandAnchor(handAnchor);
        firstPersonBody.ArmsLateUpdate(world, firstPersonPresentation.ArmsEntity(),
                                       firstPersonPresentation.ViewModelFov(), gameDt, &player.Cam,
                                       firstPersonPresentation.ThirdPersonGunCorrection(), haveWorldGun ? &worldGun : nullptr,
                                       firstPersonPresentation.ThirdPersonArms(), firstPersonPresentation.GunSocket(),
                                       haveAnchor ? &handAnchor : nullptr);
        if (firstPersonPresentation.IsActive()) {
            glm::mat4 holdingHand(1.0f);
            const bool haveHolding = haveAnchor && firstPersonBody.BoneWorldMatrix(world, handAnchor.Hand, holdingHand);
            firstPersonPresentation.PlaceWorldWeapon(world, firstPersonBody.SplitPoses(), firstPersonBody.WorldGunDelta(),
                                                     firstPersonBody.CarriedMove(), firstPersonBody.TwinAnchorWeight(),
                                                     firstPersonBody.CarriedHand(), haveHolding ? &holdingHand : nullptr);
        }
        if (weaponLab) weaponLab->AfterPose(world, firstPersonBody, firstPersonPresentation, player.Cam, gameDt);
        // This frame's rounds, down the bore from the muzzle: a hole where each struck.
        PROFILE_SCOPE("Player Shots + Impacts");
        for (const FirstPersonPresentation::ShotHit& hit : firstPersonPresentation.TakeShotHits()) {
            if (SpiderDamage::Result sr; spiderDamage.OnHit(world, spiders, hit.Entity, hit.Point, hit.Origin, hit.Direction,
                                                             firstPersonPresentation.Set().Gameplay, hit.Pellets, sr)) {
                bloodFx.OnFleshHit(SpiderDamage::Blood(sr)); // ichor, not a hole in the wall behind it
                if (sr.Living) {
                    playerVitals.MarkHit(sr.Killed, sr.Zone == Spider::PartZone::Head);
                    combatFx.Play(sr.Killed ? CombatFx::Cue::HitmarkerKill : CombatFx::Cue::Hitmarker, hit.Point);
                }
                continue;
            }
            if (npcDirector.Active()) {
                bool killed = false, head = false, aliveWhenHit = false;
                if (npcDirector.OnPlayerHit(world, hit.Entity, hit.Point, hit.Origin, hit.Direction,
                                            firstPersonPresentation.Set().Gameplay, &killed, &head, &aliveWhenHit)) {
                    if (aliveWhenHit) {
                        playerVitals.MarkHit(killed, head);
                        combatFx.Play(killed ? CombatFx::Cue::HitmarkerKill : CombatFx::Cue::Hitmarker, hit.Point);
                    }
                    if (killed) combatHud.OnKill(npcDirector, hit.Entity, head);
                    continue; // a soldier, not a wall: no hole
                }
            }
            combatFx.Impact(world, hit.Entity, glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]));
            worldImpact(hit.Entity, hit.Point, hit.Normal, glm::vec3(hit.Direction[0], hit.Direction[1], hit.Direction[2]), hit.HoleRadius);
        }
        // Every pellet's line: the report, and rounds cracking past soldiers' heads (suppression).
        for (const FirstPersonPresentation::ShotTrace& t : firstPersonPresentation.TakeShotTraces()) {
            if (npcDirector.Active()) npcDirector.OnPlayerShotLine(t.Origin, t.End);
            if (t.FirstPellet) spiders.HearShot(t.Origin);
            if (t.FirstPellet)
                combatFx.Shot(world, firstPersonPresentation.Set().Gameplay.Pellets > 1 ? CombatFx::Gun::Shotgun : CombatFx::Gun::Rifle,
                              t.Origin, t.End, true, false, 0, firstPersonPresentation.MuzzleEffects());
        }
        // The player's muzzle flames (this frame's too) ride the gun as it kicks and turns.
        if (glm::vec3 fpMuzzle, worldMuzzle, bore; combatFx.Active() && firstPersonPresentation.IsActive() &&
                                                   firstPersonPresentation.MuzzleFrames(fpMuzzle, worldMuzzle, bore))
            combatFx.FollowMuzzle(world, fpMuzzle, worldMuzzle, bore);
        // The spent cases out of the port, then every case on the ground moved on.
        for (const CasingSpawn& spawn : firstPersonPresentation.TakeEjections())
            shellCasings.Spawn(world, assets, spawn);
        {
        PROFILE_SCOPE("Shell Casings");
        shellCasings.Update(world, gameDt, player.Cam, player.Cam.Position);
        }
    }
    // The enemy squad's late pose: spines aimed, guns placed on their eye cameras, hands
    // onto the guns, this frame's rounds fired. Then what they hit.
    if (npcDirector.Active()) {
        PROFILE_SCOPE("Enemy AI Late");
        npcDirector.LateUpdate(world, gameDt, playerSnap);
        PROFILE_SCOPE("AI Impacts + Flesh Hits");
        for (const NpcDirector::Impact& imp : npcDirector.TakeImpacts()) {
            combatFx.Impact(world, imp.Entity, glm::vec3(imp.Point[0], imp.Point[1], imp.Point[2]));
            worldImpact(imp.Entity, imp.Point, imp.Normal, -glm::vec3(imp.Normal[0], imp.Normal[1], imp.Normal[2]), imp.Radius);
        }
        for (const CasingSpawn& spawn : npcDirector.TakeEjections()) shellCasings.Spawn(world, assets, spawn);
        for (const DamageEvent& e : npcDirector.TakePlayerDamage()) {
            const float before = playerVitals.Health();
            playerVitals.ApplyDamage(e.Amount, e.SourcePos);
            if (playerVitals.Health() < before) combatFx.Play(CombatFx::Cue::FleshHit, e.Point, true, 0.8f);
        }
        // Every round that went into a body this frame - theirs and the player's - bleeds.
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
        fi.Sprinting = !player.AimHeld && !player.SprintBlocked && (gameHasInput && InputMap::GetButton("Sprint"));
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
            npcDirector.OnPlayerRespawned();
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
