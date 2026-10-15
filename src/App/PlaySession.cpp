#include "App/PlaySession.h"

#include "App/LaunchOptions.h"
#include "AssetLibrary.h"
#include "BloodRenderer.h"
#include "BulletHoles.h"
#include "Camera.h"
#include "CombatHud.h"
#include "Combat/BloodFx.h"
#include "Combat/CombatFx.h"
#include "Combat/FxSprites.h"
#include "Combat/PlayerVitals.h"
#include "Components.h"
#include "DevPanel.h"
#include "EditorLayer.h"
#include "EditorModuleAPI.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "FirstPersonWeaponTest.h"
#include "Flashlight.h"
#include "FxSpriteRenderer.h"
#include "GameModuleAPI.h"
#include "GameViewPanel.h"
#include "Game/Scripting/NpcDefinitions.h"
#include "Game/Scripting/PlayerDefinition.h"
#include "Game/Scripting/ScriptRuntime.h"
#include "GravityGun.h"
#include "HotReloadEditorModule.h"
#include "KnifeFxLibrary.h"
#include "Log.h"
#include "OutfitSystem.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "ProjectSettings.h"
#include "ShellCasings.h"
#include "TimeService.h"
#include "WeaponFxRenderer.h"
#include "WeaponLab.h"
#include "Window.h"
#include "World.h"
#include "AI/NpcDirector.h"
#include "AI/NpcTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

entt::entity FindFirstPersonController(World& world) {
    Scripting::SyncPlayerDefinitions(world);
    entt::entity best = entt::null;
    int bestOrder = 0x7fffffff;
    for (auto e : world.Registry.view<const FirstPersonControllerComponent, const TransformComponent>()) {
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        const auto* ord = world.Registry.try_get<OrderComponent>(e);
        int o = ord ? ord->Value : 0;
        if (o < bestOrder) { bestOrder = o; best = e; }
    }
    return best;
}

void YawPitchFromWorld(const glm::mat4& m, float& yawDeg, float& pitchDeg) {
    glm::vec3 f = -glm::vec3(m[2]);
    const float len = glm::length(f);
    f = len > 1e-6f ? f / len : glm::vec3(0.0f, 0.0f, -1.0f);
    yawDeg = glm::degrees(std::atan2(f.z, f.x));
    pitchDeg = std::clamp(glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f))), -89.0f, 89.0f);
}

entt::entity FindActiveSceneCamera(const World& world) {
    entt::entity best = entt::null;
    int bestOrder = 0x7fffffff;
    for (auto e : world.Registry.view<const CameraComponent, const TransformComponent>()) {
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        const auto* ord = world.Registry.try_get<OrderComponent>(e);
        int o = ord ? ord->Value : 0;
        if (o < bestOrder) { bestOrder = o; best = e; }
    }
    return best;
}

void PlaySession::Start() {
    if (m_State.Playing) return;
    if (m_Editor.InPrefabMode()) m_Editor.ExitPrefabMode(m_World, m_Assets); // #176 - Play runs the scene
    if (EditorModuleHost::ConsoleState().ClearOnPlay) Log::Clear(); // #236 A5
    m_Editor.OnEnterPlayMode(m_World);
    // Character Outfit's Randomize On Play: dressed afresh, after the snapshot so Stop brings the
    // edited outfit back. It loads in the background; FirstPersonBody takes the new pieces once
    // they're on (OutfitChanged). A body not built yet (untagged parts) is adopted first, or
    // Randomize would build a second body over it.
    for (auto [e, outfit] : m_World.Registry.view<CharacterOutfitComponent>(entt::exclude<InactiveTag>).each()) {
        if (!outfit.RandomizeOnPlay) continue;
        if (OutfitSystem::Pieces(m_World, e).empty()) OutfitSystem::AdoptExisting(m_World, m_Assets, e);
        const auto r = OutfitSystem::Randomize(m_World, m_Assets, e,
                                               (std::uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() ^
                                                   (std::uint32_t)entt::to_integral(e));
        if (!r.Ok) Log::Warn("Character Outfit: Randomize On Play - " + r.Error);
    }
    // Bare Body (debug; the gait probe's and the animation sweep's player body): every item off, so the wardrobe dresses the character in its
    // own body parts - the animation and skinning seen without clothing. Undone by Stop like the above.
    for (auto [e, outfit] : m_World.Registry.view<CharacterOutfitComponent>(entt::exclude<InactiveTag>).each()) {
        const bool probeBody = m_WeaponTest && m_WeaponTest->BareBody() && m_World.Registry.all_of<FirstPersonBodyComponent>(e);
        if (!outfit.BareBody && !probeBody) continue;
        if (OutfitSystem::Pieces(m_World, e).empty()) OutfitSystem::AdoptExisting(m_World, m_Assets, e);
        Wardrobe::Request bare = OutfitSystem::CurrentRequest(m_World, e);
        bare.Items.clear();
        const auto r = OutfitSystem::Submit(m_World, m_Assets, e, bare);
        if (!r.Ok) Log::Warn("Character Outfit: Bare Body - " + r.Error);
    }
    Time::SetTimeScale(ProjectSettings::Time().TimeScale); // #144 - each run starts from the project's scale
    m_State.ErrPauseSeen = Log::CountOf(LogLevel::Error); // ignore errors that predate this run
    m_State.Playing = true;
    m_State.Maximized = false;
    m_State.EditorUIVisible = true;
    m_State.GameInputEngaged = false;
    m_GameView.OnPlayStateChanged(true);
    m_Editor.RequestGameTabFocus(); // show what you just started
    // #165 - pick what Play runs: a First Person Controller entity (spawn there, with its
    // settings), else the scene's Camera (no player), else the legacy default player
    // dropped in at the editor camera so Play inspects what you were just working on.
    // The editor camera is left where it is — the Scene tab stays usable during play.
    m_Player = Player{};
    m_Flashlight = Flashlight{};
    // The player's fall is its own game-feel value (the First Person Controller's Gravity,
    // 18 m/s^2 when there's none), not the physics world's Earth gravity.
    m_State.UsesPlayer = false;
    m_State.GravityGun = false;
    m_GravityGun.Settings = GravityGunSettings{};
    m_State.CameraEntity = entt::null;
    m_State.ControllerEntity = FindFirstPersonController(m_World);
    {   // the scene's effect and HUD tuning: the first FX / HUD Settings component, defaults when none
        FxHudSettingsComponent fx=Scripting::DefaultEffectsDefinition();
        if (const auto first = m_World.Registry.view<FxHudSettingsComponent>(); first.begin() != first.end()) { // the first one counts
            const entt::entity e = *first.begin();
            fx = m_World.Registry.get<FxHudSettingsComponent>(e);
        }
        m_CombatFx.Settings = fx;
        m_CombatHud.Settings = fx;
        m_WeaponFx.Settings = fx;
    }
    m_BloodFx.Clear();
    m_FxSprites.Clear();
    {   // the scene's blood tuning: the first Blood Settings component, defaults when none
        BloodSettingsComponent b;
        if (const auto all = m_World.Registry.view<BloodSettingsComponent>(); all.begin() != all.end())
            b = m_World.Registry.get<BloodSettingsComponent>(*all.begin());
        m_BloodFx.Config.Enabled = b.Enabled;
        m_BloodFx.Config.Size = b.Size;
        m_BloodFx.Config.MaxSprays = b.MaxSprays;
        m_BloodFx.Config.MaxDecals = b.MaxStains;
        m_BloodFx.Config.DecalLifetime = b.StainLifetime;
        m_BloodFx.Config.DrySeconds = b.DrySeconds;
        m_BloodFx.Config.Speed = b.Speed;
        m_BloodFx.Config.Pools = b.Pools;
        m_BloodFx.Config.BodySplats = b.BodySplats;
        m_BloodFx.Config.GearSpatter = b.GearSpatter;
        m_BloodFx.Config.EnergyScale = b.EnergyScale;
        m_BloodFx.Config.ImpactPuffs = b.ImpactPuffs;
        m_BloodFx.Config.Gore = b.Gore;
    }
    BloodRenderer::Get().Load(); // once; a missing import just leaves the blood off
    KnifeFxLibrary::Get().Load(); // likewise the Knife packs (--import-knife-fx)
    if (entt::entity ctrl = m_State.ControllerEntity; ctrl != entt::null) {
        m_State.UsesPlayer=true;
        m_GravityGun.Reset();
        const auto& fp = m_World.Registry.get<FirstPersonControllerComponent>(ctrl);
        m_Player.MoveSpeed = fp.MoveSpeed;
        m_Player.Gravity = -std::abs(fp.Gravity);
        m_Player.SprintMultiplier = fp.SprintMultiplier;
        m_Player.JumpSpeed = fp.JumpSpeed;
        m_Player.JumpBufferTime = fp.JumpBufferTime;
        m_Player.CoyoteTime = fp.CoyoteTime;
        m_Player.GroundAccelTime = fp.GroundAccelTime;
        m_Player.GroundDecelTime = fp.GroundDecelTime;
        m_Player.AirAccelTime = fp.AirAccelTime;
        m_Player.EyeHeight = fp.EyeHeight;
        m_Player.Size = glm::vec3(fp.CapsuleRadius * 2.0f, std::max(fp.CapsuleHeight, fp.CapsuleRadius * 2.0f + 0.1f),
                                fp.CapsuleRadius * 2.0f);
        m_Player.MouseSensitivity = fp.MouseSensitivity;
        m_State.BaseSensitivity = fp.MouseSensitivity;
        m_State.BaseFov = fp.VerticalFov();
        m_Player.InvertY = fp.InvertY;
        m_Player.StickLookDegPerSec = fp.StickLookDegPerSec;
        m_Player.KillY = fp.KillY;
        m_Player.Cam.Fov = fp.VerticalFov();
        m_State.GravityGun = fp.GravityGun;
        m_GravityGun.Settings.GrabRange = fp.GrabRange;
        m_GravityGun.Settings.AssistRange = fp.AssistRange;
        m_GravityGun.Settings.AssistConeDeg = fp.AssistConeDeg;
        m_GravityGun.Settings.ScrollTurnDeg = fp.ScrollTurnDeg;
        m_GravityGun.Settings.MinThrowSpeed = fp.MinThrowSpeed;
        m_GravityGun.Settings.MaxThrowSpeed = std::max(fp.MaxThrowSpeed, fp.MinThrowSpeed);
        m_GravityGun.Settings.ChargeTime = fp.ThrowChargeTime;
        m_GravityGun.Settings.BackspinRevPerSec = fp.ThrowBackspin;
        const glm::mat4 spawn = m_World.ComposeWorldTransform(ctrl);
        m_Player.RespawnFeet = glm::vec3(spawn[3]);
        m_Player.Cam.Position = m_Player.RespawnFeet + glm::vec3(0.0f, m_Player.EyeHeight, 0.0f);
        YawPitchFromWorld(spawn, m_Player.Cam.Yaw, m_Player.Cam.Pitch);
        // --stock-probe runs, strafes and turns for metres at a time: it starts on open ground (the
        // Sandbox's shooting range lane), wherever the scene's spawn is, so runs stay comparable.
        if (m_Opt.stockProbeMode) {
            m_Player.RespawnFeet = glm::vec3(45.0f, 0.1f, -2.5f);
            // The gait probe walks, jogs and sprints metres every way and films the whole body: the open floor
            // with the most room round it - rays out at knee and chest height in 16 directions, over ground at about 0.
            if (m_WeaponTest && m_WeaponTest->OpenFloor()) {
                QueryFilter f;
                f.HitTriggers = 0;
                float best = -1.0f;
                for (float gx = -54.0f; gx <= 54.0f; gx += 3.0f)
                    for (float gz = -54.0f; gz <= 54.0f; gz += 3.0f) {
                        RaycastHit h;
                        const float top[3] = {gx, 3.0f, gz}, down[3] = {0.0f, -1.0f, 0.0f};
                        if (!PhysicsWorld::RaycastSolid(top, down, 4.0f, f, h) || !h.Hit || std::abs(h.Point[1]) > 0.15f) continue;
                        float room = 14.0f;
                        for (int a = 0; a < 16 && room > best; ++a)
                            for (float y : {0.4f, 1.3f}) {
                                const float o[3] = {gx, h.Point[1] + y, gz};
                                const float d[3] = {std::cos((float)a * 0.3927f), 0.0f, std::sin((float)a * 0.3927f)};
                                RaycastHit side;
                                if (PhysicsWorld::RaycastSolid(o, d, room, f, side) && side.Hit) room = std::min(room, side.Distance);
                            }
                        if (room > best) { best = room; m_Player.RespawnFeet = glm::vec3(gx, h.Point[1] + 0.1f, gz); }
                    }
                std::printf("[Gait] start at (%.0f, %.0f), %.1f m clear all round\n", m_Player.RespawnFeet.x, m_Player.RespawnFeet.z, best);
            }
            m_Player.Cam.Position = m_Player.RespawnFeet + glm::vec3(0.0f, m_Player.EyeHeight, 0.0f);
        }
        // FPS presentation is opt-in on the controller (its Animation Set). With the
        // gravity gun also on, that becomes the unarmed slot - see gravityGunLive.
        m_Presentation.Start(m_World, m_Assets, fp);
        m_Body.Start(m_World, m_Player); // a First Person Body in the scene: root-motion movement
        // The weapon's walk / sprint clip rates follow what the player really moves at (the body's speeds).
        if (m_Body.IsActive() && m_Presentation.IsActive())
            m_Presentation.SetLocomotionSpeeds(m_Player.MoveSpeed, m_Player.MoveSpeed * m_Player.SprintMultiplier);
        PlayerVitalsSettings vitals;
        vitals.MaxHealth = fp.MaxHealth;
        vitals.RegenDelay = fp.RegenDelay;
        vitals.RegenRate = fp.RegenRate;
        vitals.RespawnDelay = fp.RespawnDelay;
        vitals.SpawnProtection = fp.SpawnProtection;
        m_PlayerVitals.Reset(vitals);
        // The enemy squad, when the scene has NPC Spawns - not under --weapon-test / --stock-probe, which put the
        // player's gun through its paces alone (a squad would shoot the player or step into the muzzle's line).
        const bool squadStarted = !m_WeaponTest && m_NpcDirector.Start(m_World, m_Assets, &fp);
        if (m_NpcTest) std::printf("[NpcTest] squad %s (%zu spawn points)\n", squadStarted ? "started" : "not started", m_NpcDirector.SpawnPoints().size());
        if (squadStarted) {
            m_DevPanel.Reset(m_PlayerVitals, m_NpcDirector);
            m_CombatHud.Reset();
            m_CombatFx.Start(m_World);
            if (m_NpcTest && !m_NpcTest->RecordDir().empty()) {
                std::error_code ec;
                std::filesystem::create_directories(m_NpcTest->RecordDir(), ec);
                m_CombatFx.SetAudioLog((std::filesystem::path(m_NpcTest->RecordDir()) / "audio.txt").string());
            }
        }
        // Play-mode audio (the guns' report layers, gear sounds, foley) and the muzzle effects don't wait for a squad:
        // a scene with no NPC Spawns, --weapon-test and --stock-probe have the player's gun too.
        if (!m_CombatFx.Active()) {
            m_CombatFx.Start(m_World);
            if (m_WeaponTest) {
                // WEAPON_TEST_AUDIO_LOG=<file>, or NPC_TEST_RECORD=<dir> (-> <dir>/audio.txt): every weapon / foley voice, timed.
#pragma warning(suppress : 4996)
                const char* logFile = std::getenv("WEAPON_TEST_AUDIO_LOG");
#pragma warning(suppress : 4996)
                const char* recordDir = std::getenv("NPC_TEST_RECORD");
                if (logFile && *logFile) m_CombatFx.SetAudioLog(logFile);
                else if (recordDir && *recordDir) {
                    std::error_code ec;
                    std::filesystem::create_directories(recordDir, ec);
                    m_CombatFx.SetAudioLog((std::filesystem::path(recordDir) / "audio.txt").string());
                }
            }
        }
    } else if ((m_State.CameraEntity = FindActiveSceneCamera(m_World)) != entt::null) {
        m_State.UsesPlayer = false;
        m_State.GravityGun = false;
    } else {
        m_Player.Cam.Position = m_EditorCamera.Position;
        m_Player.Cam.Yaw = m_EditorCamera.Yaw;
        m_Player.Cam.Pitch = m_EditorCamera.Pitch;
    }
    m_Window.SetCursorLocked(false); // click the Game view to take control
}

void PlaySession::Stop() {
    if (!m_State.Playing) return;
    if (m_WeaponLab) m_WeaponLab->OnPlayStopped();
    Scripting::Stop(&m_World,&m_Assets);
    // Runtime-only arms/weapon entities were created after the play snapshot. Remove
    // them before restoring the authored world so they can never be serialized or leak
    // into another scene after a stop.
    m_Presentation.RemoveViewKick(m_Player.Cam);
    m_CombatFx.Stop(m_World);
    m_NpcDirector.Stop(m_World);
    m_Player.Cam.Position -= m_State.DeathCamOffset;
    m_Player.Cam.Roll -= m_State.DeathCamRoll;
    m_State.DeathCamOffset = glm::vec3(0.0f);
    m_State.DeathCamRoll = 0.0f;
    m_Presentation.Stop(m_World);
    m_Body.Stop(m_World);
    m_BulletHoles.Clear();
    m_BloodFx.Clear();
    BloodRenderer::Get().BeginFrame(); // nothing of Play's blood left in the editor
    m_FxSprites.Clear();
    FxSpriteRenderer::Get().BeginFrame();
    m_ShellCasings.Clear(m_World);
    m_Editor.OnExitPlayMode(m_World, m_Assets);
    m_State.Playing = false;
    m_State.Paused = false;
    m_State.Maximized = false;
    m_State.EditorUIVisible = true;
    m_State.GameInputEngaged = false;
    m_GameView.OnPlayStateChanged(false);
    m_Editor.RequestSceneTabFocus();
    m_Window.SetCursorLocked(false);
}
