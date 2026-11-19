#include "FirstPersonPresentation.h"
#include "FirstPersonPresentationInternal.h"
#include "FirstPersonMovementInput.h"
#include "GameRules.h"
#include "LiveReload.h"
#include "WeaponRules.h"
#include "WeaponPrefab.h"

#include "AnimationSystem.h"
#include "Audio/WeaponAudio.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "IK.h"
#include "Log.h"
#include "GameModuleAPI.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ParticleSystem.h"
#include "SceneSerializer.h"
#include "Core/Paths.h"
#include "RotationMath.h"
#include "World.h"
#include "TimeService.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <unordered_map>

using namespace FirstPersonPresentationDetail;

namespace {

bool FinitePositive(float value) {
    return std::isfinite(value) && value > 0.0f;
}

bool SameIKSetup(const WeaponIKSettings& a, const WeaponIKSettings& b) {
    return a.Enabled == b.Enabled && a.GunBone == b.GunBone && a.RightUpper == b.RightUpper &&
           a.RightLower == b.RightLower && a.RightHand == b.RightHand && a.LeftUpper == b.LeftUpper &&
           a.LeftLower == b.LeftLower && a.LeftHand == b.LeftHand;
}

} // namespace

namespace K = FirstPersonAnimatorContract;

void FirstPersonPresentation::SetError(const std::string& message) {
    m_LastError = message;
    Log::Error("First-person presentation: " + message);
}

bool FirstPersonPresentation::Start(World& world, AssetLibrary& assets,
                                    const FirstPersonControllerComponent& config) {
    Stop(world);
    m_LastError.clear();
    int configuredSlot=0;
    for (auto slot : {std::pair<const std::string*,const std::string*>{&config.PrimaryWeaponPrefab,&config.AnimationSet},
                      {&config.SecondaryWeaponPrefab,&config.SecondaryAnimationSet},
                      {&config.SidearmWeaponPrefab,&config.SidearmAnimationSet}}) {
        const int configured=configuredSlot++;
        std::string set=*slot.second;
        MuzzleEffectSettings muzzle;
        if(!slot.first->empty()) {
            std::string why;
            if(!Gameplay::ResolveWeaponPrefab(*slot.first,set,why,&muzzle)) {
                // A later slot failing leaves no half-filled slot lists behind (SlotCount() would report slots on an
                // inactive presentation).
                Stop(world);
                SetError(why);
                return false;
            }
        }
        if(!set.empty()) {
            m_SlotSets.push_back(set);m_SlotPrefabs.push_back(*slot.first);m_SlotMuzzles.push_back(muzzle);m_SlotConfigured.push_back(configured);
            std::error_code ec;
            m_SlotPrefabTimes.push_back(slot.first->empty()?std::filesystem::file_time_type{}:
                std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(*slot.first)),ec));
        }
    }
    if (m_SlotSets.empty()) return true;
    m_SlotAmmo.assign(m_SlotSets.size(), -1);
    m_SlotShoulderedButt.assign(m_SlotSets.size(), glm::vec3(0.0f));
    m_SlotHaveShoulderedButt.assign(m_SlotSets.size(), 0);
    m_SlotAttachmentSelections.assign(m_SlotSets.size(), {{-1, -1, -1}});
    m_Config = std::make_shared<FirstPersonControllerComponent>(config);
    m_SlotAssets = &assets;
    m_Slot = 0;
    m_PendingSlot = -1;
    m_WalkSpeed = config.MoveSpeed;
    m_SprintSpeed = config.MoveSpeed * config.SprintMultiplier;
    m_EyeRadius = config.EyeRadius;
    // The other slots' guns too (StartSet loads its own): a swap mustn't stall on the draw or on the first round.
    for (size_t i = 1; i < m_SlotSets.size(); ++i) WarmSlotAssets(assets, m_SlotSets[i]);
    PreloadClips(assets);
    return StartSet(world, assets, 0, false);
}

void FirstPersonPresentation::PreloadClips(AssetLibrary& assets) {
    // Every clip the slots' sets play is its own FBX (an arms and a weapon take per state, ~60 files for two guns).
    // Imported one by one as whole models on this thread they were most of Play's start (~18 s); clip-only, all at
    // once on worker threads, before the first set attaches them.
    const auto start = std::chrono::steady_clock::now();
    // Each set's clips once per version of its file.
    static std::unordered_map<std::string, std::filesystem::file_time_type> s_Preloaded;
    std::vector<std::string> files;
    for (const std::string& setRef : m_SlotSets) {
        const std::string setPath = ProjectPaths::Resolve(setRef);
        std::error_code tec;
        const auto setTime = std::filesystem::last_write_time(std::filesystem::u8path(setPath), tec);
        if (const auto it = s_Preloaded.find(setPath); it != s_Preloaded.end() && it->second == setTime) continue;
        s_Preloaded[setPath] = setTime;
        FirstPersonAnimationSet set;
        if (!FirstPersonAnimationSet::LoadFile(setPath, set, nullptr) || set.Controller.empty()) continue;
        const auto ctrl = GetAnimatorController(set.Controller);
        if (!ctrl) continue;
        auto add = [&](const std::string& clip) {
            const std::string path = clip.substr(0, clip.find('#'));
            if (path.empty()) return;
            std::string ext = std::filesystem::u8path(path).extension().u8string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".fbx" && ext != ".gltf" && ext != ".glb" && ext != ".dae") return;
            const std::string abs = std::filesystem::u8path(path).is_absolute() ? path : ProjectPaths::Resolve(path);
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::u8path(abs), ec)) files.push_back(abs);
        };
        for (const auto& layer : ctrl->Layers)
            for (const auto& state : layer.States)
                for (int track = 0; track < (int)ctrl->Tracks.size() || track < 2; ++track) {
                    const auto& m = state.MotionFor(track);
                    add(m.Clip);
                    for (const auto& child : m.Children) add(child.Clip);
                }
    }
    if (files.empty()) return;
    const int imported = assets.PreloadAnimationSources(files);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    char line[160];
    std::snprintf(line, sizeof(line), "Play start: first-person clips - %d file(s) imported clip-only in %.0f ms (%d already loaded).", imported,
                  ms, (int)files.size() - imported);
    Log::Info(line);
}

void FirstPersonPresentation::WarmSlotAssets(AssetLibrary& assets, const std::string& animationSet) {
    FirstPersonAnimationSet set;
    if (!FirstPersonAnimationSet::LoadFile(ProjectPaths::Resolve(animationSet), set, nullptr)) return;
    for (const std::string* model : {&set.ArmsModel, &set.WeaponModel})
        if (!model->empty()) assets.LoadModel(ProjectPaths::Resolve(*model));
    for (const auto* overrides : {&set.ArmsMaterials, &set.WeaponMaterials})
        for (const auto& [name, matPath] : *overrides) assets.LoadMaterial(ProjectPaths::Resolve(matPath));
    WarmEjectAssets(assets, set);
}

void FirstPersonPresentation::WarmEjectAssets(AssetLibrary& assets, const FirstPersonAnimationSet& set) {
    if (!set.Eject.Enabled || set.Eject.Model.empty()) return;
    assets.LoadModel(ProjectPaths::Resolve(set.Eject.Model));
    if (!set.Eject.LiveModel.empty()) assets.LoadModel(ProjectPaths::Resolve(set.Eject.LiveModel));
    if (!set.Eject.Material.empty()) assets.LoadMaterial(ProjectPaths::Resolve(set.Eject.Material));
}

void FirstPersonPresentation::Stop(World& world) {
    StopSet(world);
    m_SlotSets.clear();
    m_SlotPrefabs.clear();m_SlotMuzzles.clear();m_SlotPrefabTimes.clear();m_SlotConfigured.clear();
    m_SlotAmmo.clear();
    m_SlotAttachmentSelections.clear();
    m_Slot = 0;
    m_PendingSlot = -1;
    m_SlotAssets = nullptr;
    m_Config.reset();
    m_Ejections.clear();
}

bool FirstPersonPresentation::StartSet(World& world, AssetLibrary& assets, int slot, bool holstered) {
    StopSet(world);
    m_LastError.clear();
    const FirstPersonControllerComponent& config = *m_Config;
    const std::string& animationSet = m_SlotSets[slot];
    m_Slot = slot;

    const std::string setPath = ProjectPaths::Resolve(animationSet);
    m_SetFile = std::filesystem::u8path(setPath);
    {
        std::error_code ec;
        m_SetFileTime = std::filesystem::last_write_time(m_SetFile, ec);
    }
    if (!FirstPersonAnimationSet::LoadFile(setPath, m_Set, &m_LastError)) {
        SetError("could not load '" + animationSet + "': " + m_LastError);
        return false;
    }
    if(!m_SlotPrefabs[slot].empty() && !Gameplay::ResolveWeaponGameplay(m_SlotPrefabs[slot],m_Set.Gameplay,m_LastError)) {
        SetError(m_LastError);return false;
    }
    {
        std::error_code ec;
        m_RecoilFileTime = m_Set.RecoilProfile.empty() ? std::filesystem::file_time_type{} :
            std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(m_Set.RecoilProfile)), ec);
        ec.clear();
        m_ShakeFileTime=m_Set.CameraShakeProfile.empty()?std::filesystem::file_time_type{}:
            std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(m_Set.CameraShakeProfile)),ec);
    }
    if (!FinitePositive(config.ViewModelScale)) {
        SetError("View Model Scale must be finite and greater than zero");
        return false;
    }

    // The controller: the weapon's own .controller, or - for a v1 clip list - the standard
    // first-person graph built in memory.
    std::shared_ptr<const AnimatorController> ctrl;
    if (!m_Set.Controller.empty()) {
        m_ControllerPath = m_Set.Controller;
        ctrl = GetAnimatorController(m_ControllerPath);
        if (!ctrl) {
            SetError("could not load the Animator Controller '" + m_Set.Controller + "' named by '" + animationSet + "'");
            return false;
        }
    } else {
        SetError("'" + animationSet + "' names no Animator Controller");
        return false;
    }
    if (ctrl->Layers.empty() || ctrl->Layers[0].States.empty()) {
        SetError("the Animator Controller for '" + animationSet + "' has no states");
        return false;
    }

    const std::string armsPath = ProjectPaths::Resolve(m_Set.ArmsModel);
    const std::string weaponPath = ProjectPaths::Resolve(m_Set.WeaponModel);
    auto arms = assets.InstantiateModel(armsPath);
    auto weapon = assets.InstantiateModel(weaponPath);
    if (!arms || !weapon) {
        SetError("could not instantiate arms or weapon model from '" + animationSet + "'");
        return false;
    }

    // Created after the Play snapshot; Stop destroys them before restoring authored scene data.
    m_World = &world;
    m_ArmsModel = arms;
    m_WeaponModel = weapon;
    // StockWorld's vertices belong to the last weapon: a new one can be allocated where it was freed (a
    // weapon swap), so a pointer compare alone would read the old weapon's indices into the new one's mesh.
    m_StockModel = nullptr;
    m_StockVerts.clear();
    m_Arms = world.CreateModelEntity(std::move(arms), glm::vec3(0.0f), glm::vec3(0.0f),
                                     glm::vec3(config.ViewModelScale), "[Runtime] First Person Arms");
    m_Weapon = world.CreateModelEntity(std::move(weapon), glm::vec3(0.0f), glm::vec3(0.0f),
                                       glm::vec3(config.ViewModelScale), "[Runtime] First Person Weapon");
    for (entt::entity e : {m_Arms, m_Weapon}) {
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        // The weapon casts its shadow from where it really is (in the hands); the arms rig doesn't - with
        // a First Person Body its arms are the body's, which cast their own.
        renderable.CastShadows = e == m_Weapon ? RenderableComponent::ShadowCasting::On : RenderableComponent::ShadowCasting::Off;
        renderable.ReceiveShadows = e == m_Weapon;
        // Routes them into the renderer's view-model sub-pass (own FOV, depth cleared) rather
        // than the world pass. Runtime-only tag: these entities are created here and destroyed
        // by Stop(), so it never reaches a scene file.
        world.Registry.emplace_or_replace<ViewModelTag>(e);
    }
    // .fpsanim material overrides: every submesh whose imported material has the listed name.
    auto applyMaterials = [&](entt::entity e, const std::vector<std::pair<std::string, std::string>>& overrides) {
        auto& renderable = world.Registry.get<RenderableComponent>(e);
        for (const auto& [name, matPath] : overrides) {
            auto mat = assets.LoadMaterial(ProjectPaths::Resolve(matPath));
            if (!mat || mat->Missing) {
                Log::Warn("First-person: material '" + matPath + "' for '" + name + "' could not be loaded.",
                          LogContext::Asset(animationSet));
                continue;
            }
            bool used = false;
            for (int i = 0; i < renderable.ModelRef->MeshCount(); ++i) {
                if (renderable.ModelRef->MeshMaterial(i).Name != name) continue;
                if (i >= (int)renderable.Materials.size()) renderable.Materials.resize(i + 1);
                renderable.Materials[i] = mat;
                used = true;
            }
            if (!used)
                Log::Warn("First-person: no submesh uses a material named '" + name + "' (from '" +
                          animationSet + "').", LogContext::Asset(animationSet));
        }
    };
    applyMaterials(m_Arms, m_Set.ArmsMaterials);
    applyMaterials(m_Weapon, m_Set.WeaponMaterials);
    WarmEjectAssets(assets, m_Set);
    // The arms run the controller; the weapon mirrors it on its own track. Both keep running
    // while hidden (unarmed), or the controller could never leave its Hidden state.
    auto& armsAnim = world.Registry.emplace_or_replace<AnimatorControllerComponent>(m_Arms);
    armsAnim.Controller = m_ControllerPath;
    armsAnim.Track = TrackOr(*ctrl, "arms", 0);
    armsAnim.UpdateWhenInactive = true;
    auto& weaponAnim = world.Registry.emplace_or_replace<AnimatorControllerComponent>(m_Weapon);
    weaponAnim.Controller = m_ControllerPath;
    weaponAnim.Track = TrackOr(*ctrl, "weapon", 1);
    weaponAnim.Driver = m_Arms;
    weaponAnim.UpdateWhenInactive = true;

    m_Offset = config.ViewModelOffset;
    m_Rotation = config.ViewModelRotation;
    m_Scale = config.ViewModelScale;
    // Not validated here: MakePerspective is the engine's one guarded projection constructor
    // (#202) and corrects every degenerate FOV, so an out-of-range value costs a wrong-looking
    // view model, never a broken frame. The Inspector already clamps it to 20..150.
    m_ViewModelFov = config.ViewModelFov;
    m_CameraBone = config.CameraBone;
    m_Ammo = m_SlotAmmo[slot] >= 0 ? std::min(m_SlotAmmo[slot], m_Set.Gameplay.Magazine) : m_Set.Gameplay.Magazine;
    ResetReloadKey();
    m_RegripDelay = FirstPersonRegripDelay(std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng),
                                           m_Set.Gameplay.RegripMin, m_Set.Gameplay.RegripMax);
    armsAnim.SetInt(K::kAmmo, m_Ammo);
    armsAnim.SetBool(K::kEquipped, true);
    // A weapon swapped in comes out of its holster rather than appearing in the hands: straight into its Draw, so the frame
    // it arrives on is posed (Holstered for that frame showed the rig's bind pose - the gun and the world arms flashed up
    // into the hold between the holster and the draw).
    if (holstered && !AnimatorStartInState(*ctrl, armsAnim, "Draw", 0.0f) && !AnimatorStartInState(*ctrl, armsAnim, "Holstered"))
        Log::Warn("First-person presentation: '" + animationSet + "' has no 'Holstered' state; it appears without a draw.");

    if (!AttachAndValidate(assets, *ctrl)) {
        StopSet(world); // this weapon's rigs only: the slot list stays for a fallback
        return false;
    }
    // The third-person hold (ThirdPersonGunCorrection): two hidden rigs following the arms, one on the "arms" clips and
    // one on "arms3p", both raw. Not drawn, no shadow (PoseSourceTag). A 3P clip that won't attach costs only the 3P hold.
    if (std::find(ctrl->Tracks.begin(), ctrl->Tracks.end(), "arms3p") != ctrl->Tracks.end()) {
        bool ok = true;
        auto follower = [&](const char* track, const char* name, std::shared_ptr<Model>& model) {
            std::shared_ptr<Model> m = assets.InstantiateModel(armsPath);
            if (!m) { ok = false; return entt::entity(entt::null); }
            const int t = ctrl->TrackIndex(track);
            for (const auto& L : ctrl->Layers)
                for (const auto& s : L.States) {
                    const auto& motion = s.MotionFor(t);
                    if (!motion.Clip.empty() && ResolveAnimationClip(*m, motion.Clip, assets) < 0) ok = false;
                    for (const auto& ch : motion.Children)
                        if (!ch.Clip.empty() && ResolveAnimationClip(*m, ch.Clip, assets) < 0) ok = false;
                }
            model = m;
            const entt::entity e = world.CreateModelEntity(std::move(m), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(config.ViewModelScale), name);
            world.Registry.emplace_or_replace<PoseSourceTag>(e);
            auto& ac = world.Registry.emplace_or_replace<AnimatorControllerComponent>(e);
            ac.Controller = m_ControllerPath;
            ac.Track = track;
            ac.Driver = m_Arms;
            ac.UpdateWhenInactive = true;
            return e;
        };
        m_ArmsClip1P = follower("arms", "[Runtime] First Person Arms (1P clips)", m_ArmsClip1PModel);
        m_ArmsClip3P = follower("arms3p", "[Runtime] First Person Arms (3P clips)", m_ArmsClip3PModel);
        m_HaveRestGrip = false; // another gun: its own grip
        if (!ok) {
            Log::Warn("First-person presentation: '" + animationSet + "' has arms3p clips that won't attach; third person holds the gun as first person does.");
            for (entt::entity e : {m_ArmsClip1P, m_ArmsClip3P})
                if (e != entt::null && world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
            m_ArmsClip1P = m_ArmsClip3P = entt::null;
            m_ArmsClip1PModel.reset();
            m_ArmsClip3PModel.reset();
        }
    }
    m_Procedural.Reset();
    m_CameraShake.Reset(); m_ShakePose={};
    m_Procedural.Seed(m_Rng()); // a fresh recoil pattern every Play, not the same one each time
    m_UsesIK = SetupIK();
    SetupSway();
    SetupBolt(assets, *ctrl);
    m_Assets = &assets;
    m_Controller = ctrl;
    SetupAdsCarry();
    SetupWeaponParticles();
    int states = 0;
    for (const auto& L : ctrl->Layers) states += (int)L.States.size();
    Log::Info("First-person presentation loaded '" + animationSet + "' (" +
              (m_Set.Controller.empty() ? std::string("v1 clip list") : m_Set.Controller) + ", " +
              std::to_string(states) + " states).");
    return true;
}

void FirstPersonPresentation::StopSet(World& world) {
    if (m_WorldWeapon != entt::null && world.Registry.valid(m_WorldWeapon)) world.DestroyEntityAndChildren(m_WorldWeapon);
    m_WorldWeapon = entt::null;
    m_WorldWeaponModel.reset();
    m_WorldWeaponDelta = glm::mat4(1.0f);
    if (m_Arms != entt::null && world.Registry.valid(m_Arms)) world.DestroyEntityAndChildren(m_Arms);
    if (m_Weapon != entt::null && world.Registry.valid(m_Weapon)) world.DestroyEntityAndChildren(m_Weapon);
    for (entt::entity e : {m_ArmsClip1P, m_ArmsClip3P})
        if (e != entt::null && world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    m_ArmsClip1P = m_ArmsClip3P = entt::null;
    m_ArmsClip1PModel.reset();
    m_ArmsClip3PModel.reset();
    m_WeaponParticles = m_WorldWeaponParticles = entt::null;
    m_WeaponEmitters.clear(); m_WorldWeaponEmitters.clear();
    m_WeaponAttachmentEntities.clear();
    m_OpticOffset = glm::vec3(0);
    m_OpticRotation = glm::quat(1, 0, 0, 0);
    m_World = nullptr;
    m_PendingShots = 0;
    m_PendingEjects = 0;
    m_RootSeenValid = false;
    m_Arms = entt::null;
    m_Weapon = entt::null;
    if (m_WeaponModel) m_WeaponModel->SetHiddenNodes({}); // the model outlives Play
    m_SpareMagHidden.clear();
    m_ArmsModel.reset();
    m_WeaponModel.reset();
    m_Set = {};
    m_ControllerPath.clear();
    m_Controller.reset();
    m_Assets = nullptr;
    m_AdsCarry = {};
    m_AdsHolding = false;
    m_CameraBone.clear();
    m_ViewModelFov = -1.0f;
    m_CameraBoneWarned = false;
    m_WeaponSocketWarned = false;
    m_Equipped = true;
    m_HiddenApplied = false;
    m_Chambered = true;
    m_HaveRestGrip = false; // the next gun's grip is its own
    m_SpareMagGap = -1.0f;
    m_CycleWait = 0.0f;
    m_CycleSeen = false;
    m_StopReload = false;
    m_FullAuto = false;
    m_BurstRemaining = 0;
    m_WaitingAnimatedShot = false;
    m_FireCooldown = 0.0f;
    m_IdleTime = 0.0f;
    ResetReloadKey();
    m_Procedural.Reset();
    m_CameraShake.Reset(); m_ShakePose={};
    m_UsesIK = false;
    m_Sway.Reset();
    m_SwayLookInput=m_SwayMoveInput=glm::vec2(0);
    m_HaveSwayMoveInput=false;
    m_HavePlayerLookBaseline = false;
    m_SetFile.clear();
    m_ReloadPoll = 0.0f;
    m_AdsHold = 0.0f;
    m_Zoom = m_ZoomRate = 0.0f;
}

void FirstPersonPresentation::SetupWeaponParticles() {
    if (!m_World || !m_Assets) return;
    auto& world = *m_World;
    for (auto e : {m_WeaponParticles, m_WorldWeaponParticles})
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    m_WeaponParticles = m_WorldWeaponParticles = entt::null;
    m_WeaponEmitters.clear(); m_WorldWeaponEmitters.clear();
    m_Attachments.Load(world, entt::null); m_WorldAttachments.Load(world, entt::null);
    m_WeaponAttachmentEntities.clear();
    if (m_Slot >= (int)m_SlotPrefabs.size() || m_SlotPrefabs[m_Slot].empty()) return;
    std::string error;
    m_WeaponParticles = Gameplay::InstantiateWeaponParticles(world, *m_Assets, m_SlotPrefabs[m_Slot], m_WeaponEmitters, error);
    if (!error.empty()) Log::Warn(error);
    if (!world.Registry.valid(m_WeaponParticles)) return;
    // Prefab child transforms are in model space. Apply the animated root's delta
    // from bind so placing the emitter in Prefab Mode also places it correctly in Play.
    std::vector<LocalTRS> bind;
    m_WeaponModel->BindLocalPose(bind);
    const int rootBone = m_WeaponModel->NodeIndex(m_Set.WeaponRoot.empty() ? "root" : m_Set.WeaponRoot);
    m_ParticleBindInverse = glm::inverse(m_WeaponModel->PoseNodeModelSpace(bind, rootBone));
    world.AttachChildRaw(m_WeaponParticles, m_Weapon);
    m_Attachments.Load(world, m_WeaponParticles, m_SlotAttachmentSelections[m_Slot]);
    m_SlotAttachmentSelections[m_Slot] = m_Attachments.Indices();
    ApplyMuzzleAttachment();
    std::vector<entt::entity> descendants{m_WeaponParticles};
    for (size_t i = 0; i < descendants.size(); ++i) {
        const auto e = descendants[i];
        if (const auto* h = world.Registry.try_get<HierarchyComponent>(e))
            descendants.insert(descendants.end(), h->Children.begin(), h->Children.end());
        world.Registry.emplace_or_replace<ViewModelTag>(e);
        if (world.Registry.valid(m_WorldWeapon)) world.Registry.emplace_or_replace<OwnerViewOnlyTag>(e);
    }
    m_WeaponAttachmentEntities = std::move(descendants);
    SetupWorldWeaponParticles();
}

void FirstPersonPresentation::SetupWorldWeaponParticles() {
    if (!m_World || !m_Assets || !m_World->Registry.valid(m_WorldWeapon) || !m_World->Registry.valid(m_WeaponParticles)) return;
    auto& world = *m_World;
    if (world.Registry.valid(m_WorldWeaponParticles)) world.DestroyEntityAndChildren(m_WorldWeaponParticles);
    m_WorldWeaponParticles = entt::null; m_WorldWeaponEmitters.clear();
    std::vector<entt::entity> created;
    if (!SceneSerializer::AppendEntitiesFromString(world, *m_Assets,
            SceneSerializer::SaveEntitiesToString(world, {m_WeaponParticles}), created)) return;
    for (auto e : created) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null) m_WorldWeaponParticles = e;
        world.Registry.remove<ViewModelTag, OwnerViewOnlyTag>(e);
        world.Registry.emplace_or_replace<HiddenFromOwnerTag>(e);
        if (world.Registry.all_of<ParticleSystemComponent>(e)) {
            m_WorldWeaponEmitters.push_back(e);
        }
    }
    if (world.Registry.valid(m_WorldWeaponParticles)) world.AttachChildRaw(m_WorldWeaponParticles, m_WorldWeapon);
    m_WorldAttachments.Load(world, m_WorldWeaponParticles, m_Attachments.Indices());
}

void FirstPersonPresentation::ApplyMuzzleAttachment() {
    if (!m_World || m_Slot < 0 || m_Slot >= (int)m_SlotMuzzles.size()) return;
    auto& fx = m_SlotMuzzles[m_Slot];
    const auto* muzzle = m_Attachments.Selected(Gameplay::AttachmentKind::Muzzle);
    fx.FiringSounds = muzzle ? muzzle->FiringSounds : std::string{};
    fx.FiringSoundProfile = muzzle ? muzzle->FiringSoundProfile : std::string{};
    glm::mat4 pose(1);
    if (m_Attachments.Pose(*m_World, m_WeaponParticles, Gameplay::AttachmentKind::Muzzle, pose)) {
        pose = m_ParticleBindInverse * pose;
        m_MuzzleLocal = glm::vec3(pose[3]);
        m_BoreLocal = glm::normalize(glm::mat3(pose) * glm::vec3(0, 0, -1));
        m_HaveMuzzle = true;
    }
}

void FirstPersonPresentation::SetupSway() {
    if (m_UsesIK || !m_World || !m_World->Registry.valid(m_Arms)) return;
    // Sway is a bone modifier in its own right: a missing/disabled arm chain must not
    // prevent a valid gun bone from being animated. General procedural motion still
    // uses the presentation's existing whole-rig fallback in this case.
    auto& rig=m_World->Registry.get_or_emplace<IKRigComponent>(m_Arms);
    rig=IKRigComponent{};
    rig.LimbA.Enabled=rig.LimbB.Enabled=false;
    rig.SwayBeforeOffset=0;
}

bool FirstPersonPresentation::SetupIK() {
    const WeaponIKSettings& k = m_Set.Procedural.IK;
    if (!k.Enabled || !m_World || !m_ArmsModel) return false;
    for (const std::string* bone : {&k.GunBone, &k.RightUpper, &k.RightLower, &k.RightHand, &k.LeftUpper, &k.LeftLower, &k.LeftHand}) {
        if (m_ArmsModel->NodeIndex(*bone) < 0) {
            Log::Warn("First-person presentation: arms model has no '" + *bone +
                      "' bone; procedural motion moves the whole view model instead of the gun (no IK).");
            return false;
        }
    }
    // Both hands keep the grip they have in the clip, relative to the gun bone, wherever the
    // procedural stack moves it.
    auto& rig = m_World->Registry.emplace_or_replace<IKRigComponent>(m_Arms);
    const auto limb = [&](IKLimb& l, const std::string& upper, const std::string& lower, const std::string& hand) {
        l.Enabled = true;
        l.Upper = upper;
        l.Lower = lower;
        l.End = hand;
        l.Target = k.GunBone;
        l.KeepAnimatedOffset = true;
        l.MatchRotation = true;
        l.Weight = 1.0f;
    };
    limb(rig.LimbA, k.RightUpper, k.RightLower, k.RightHand);
    limb(rig.LimbB, k.LeftUpper, k.LeftLower, k.LeftHand);
    // Two offsets on the gun bone: the ADS-action correction (about the camera bone) first, then
    // the procedural pose on top of wherever that put the gun.
    rig.Offsets.assign(2, IKBoneOffset{});
    rig.Offsets[kAdsOffset].Bone = k.GunBone;
    rig.Offsets[kAdsOffset].PivotBone = m_CameraBone;
    rig.Offsets[kProceduralOffset].Bone = k.GunBone;

    // Self-check on the real rig: pull the gun 10% of an arm's length toward the right shoulder
    // and tip it 5 degrees, solve, and measure how far each hand lands from its grip. Anything
    // beyond a tiny miss means these bones don't form the chains they're named as.
    {
        std::vector<LocalTRS> pose;
        m_ArmsModel->BindLocalPose(pose);
        std::vector<int> parents(pose.size());
        for (int i = 0; i < (int)pose.size(); ++i) parents[i] = m_ArmsModel->NodeParent(i);
        std::vector<glm::mat4> before, after;
        IK::ComputeGlobals(pose, parents, before);
        const int gun = m_ArmsModel->NodeIndex(k.GunBone);
        const int shoulder = m_ArmsModel->NodeIndex(k.RightUpper);
        const int hands[2] = {m_ArmsModel->NodeIndex(k.RightHand), m_ArmsModel->NodeIndex(k.LeftHand)};
        const float armLength = glm::length(IK::Position(before[hands[0]]) - IK::Position(before[shoulder]));
        IKRigComponent probe = rig;
        const glm::vec3 toShoulder = IK::Position(before[shoulder]) - IK::Position(before[gun]);
        probe.Offsets[kProceduralOffset].Position = glm::length(toShoulder) > 1e-6f ? glm::normalize(toShoulder) * armLength * 0.1f : glm::vec3(0.0f);
        probe.Offsets[kProceduralOffset].Rotation = glm::angleAxis(glm::radians(5.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        IK::ApplyRig(probe, *m_ArmsModel, pose);
        IK::ComputeGlobals(pose, parents, after);
        float worst = 0.0f;
        for (int h : hands) {
            const glm::mat4 want = after[gun] * glm::inverse(before[gun]) * before[h];
            worst = std::max(worst, glm::length(IK::Position(want) - IK::Position(after[h])) / std::max(armLength, 1e-6f));
        }
        char msg[192];
        std::snprintf(msg, sizeof msg, "First-person IK: hands stay on the gun to within %.3f%% of an arm's length (probe offset 10%%).",
                      worst * 100.0f);
        if (worst < 0.005f) Log::Info(msg);
        else Log::Warn(std::string(msg) + " Check the IK bone names in the weapon definition.");
    }
    return true;
}

// Camera-frame procedural pose -> the arms rig's model space. The arms entity is rotated
// camera * C (C = the asset's view rotation, then the scene's tweak) and scaled by m_Scale, so a
// camera-frame vector v is C^-1 v / scale in model space, and a rotation q is C^-1 q C.
// The procedural bolt rides along the travel the weapon's own Fire clip authors: sample that clip,
// and the bolt's farthest point from where it starts is the stroke (weapon model space).
void FirstPersonPresentation::SetupBolt(AssetLibrary& assets, const AnimatorController& ctrl) {
    m_BoltStroke = glm::vec3(0.0f);
    m_HaveMuzzle = false;
    const WeaponRecoilSettings& r = m_Set.Procedural.Recoil;
    if (!m_World || !m_WeaponModel) return;
    const int bolt = m_WeaponModel->NodeIndex(r.BoltBone);
    const int weaponTrack = ctrl.TrackIndex(TrackOr(ctrl, "weapon", 1));
    if (bolt >= 0 && glm::length(r.BoltTravel) > 1e-6f) {
        m_BoltStroke = r.BoltTravel;
        m_SightMeasured = m_SightLogged = false;
        m_SightSettled = 0.0f;
        m_Barrel = {};
        SetupMuzzle(bolt);
        if (r.BoltCycle > 0.0f) {
            auto& rig = m_World->Registry.emplace_or_replace<IKRigComponent>(m_Weapon);
            rig.Offsets.assign(1, IKBoneOffset{});
            rig.Offsets[0].Bone = r.BoltBone;
        }
        return;
    }
    // A fresh Play measures its own sight line (a saved one wins anyway).
    m_SightMeasured = m_SightLogged = false;
    m_SightSettled = 0.0f;
    m_Barrel = {};
    // The hip-fire clip: the state with the Shot event (else one named Fire) that moves the gun.
    int clip = -1;
    for (int pass = 0; pass < 2 && clip < 0; ++pass)
        for (const auto& L : ctrl.Layers)
            for (const auto& st : L.States) {
                const bool shot = std::any_of(st.Events.begin(), st.Events.end(),
                                              [](const AnimatorController::Event& ev) { return ev.Name == K::kEventShot; });
                if (clip < 0 && (pass == 0 ? shot : st.Name == "Fire") && !st.MotionFor(weaponTrack).Clip.empty())
                    clip = ResolveAnimationClip(*m_WeaponModel, st.MotionFor(weaponTrack).Clip, assets);
            }
    if (r.BoltBone.empty()) { // no bolt (a pump, a revolver): nothing to measure, the muzzle is set by hand
        SetupMuzzle(-1);
        return;
    }
    if (bolt < 0 || clip < 0) {
        Log::Warn("First-person presentation: no '" + r.BoltBone + "' bone or weapon hip-fire clip to measure the bolt from; no procedural bolt.");
        SetupMuzzle(-1);
        return;
    }
    {
        std::vector<int> parents(m_WeaponModel->NodeCount());
        for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
        std::vector<LocalTRS> pose;
        std::vector<glm::mat4> globals;
        glm::vec3 start(0.0f);
        const float length = m_WeaponModel->AnimationLength(clip);
        for (int k = 0; k <= 60; ++k) {
            m_WeaponModel->SampleLocalPose(clip, length * (float)k / 60.0f, AnimationWrapMode::ClampForever, pose);
            IK::ComputeGlobals(pose, parents, globals);
            const glm::vec3 at = IK::Position(globals[bolt]);
            if (k == 0) start = at;
            else if (glm::length(at - start) > glm::length(m_BoltStroke)) m_BoltStroke = at - start;
        }
    }
    SetupMuzzle(bolt);
    if (r.BoltCycle <= 0.0f) return;
    auto& rig = m_World->Registry.emplace_or_replace<IKRigComponent>(m_Weapon);
    rig.Offsets.assign(1, IKBoneOffset{});
    rig.Offsets[0].Bone = r.BoltBone;
}

// Auto: the bolt rides the bore, so its travel is the barrel's axis. The muzzle is the front face
// of the weapon mesh along that axis: the verts nearest the tip and within a few cm of the line,
// averaged (the booster's ring centres on the bore even if the bolt bone sits a little off it).
// Both are kept in the weapon root's space, which is what the socket moves. A hand-set muzzle
// (muzzle.auto off) wins; what Auto found is still reported, for the Inspector to copy.
void FirstPersonPresentation::SetupMuzzle(int bolt) {
    m_Barrel.BoltTravel = m_BoltStroke;
    m_HaveMuzzle = false;
    m_Barrel.Detected = m_Barrel.HasMuzzle = false;
    m_Barrel.Problem.clear();
    if (m_WeaponModel && bolt >= 0 && glm::length(m_BoltStroke) >= 1e-5f) {
        std::vector<int> parents(m_WeaponModel->NodeCount());
        for (int i = 0; i < (int)parents.size(); ++i) parents[i] = m_WeaponModel->NodeParent(i);
        const int root = m_WeaponModel->NodeIndex(m_Set.WeaponRoot.empty() ? std::string("root") : m_Set.WeaponRoot);
        std::vector<LocalTRS> bind;
        std::vector<glm::mat4> globals;
        m_WeaponModel->BindLocalPose(bind);
        IK::ComputeGlobals(bind, parents, globals);
        const glm::vec3 axis = -glm::normalize(m_BoltStroke);
        const glm::vec3 origin = IK::Position(globals[bolt]);
        std::vector<glm::vec3> verts;
        std::vector<unsigned int> indices;
        m_WeaponModel->CollisionGeometry(verts, indices);
        const float stroke = glm::length(m_BoltStroke); // ~9 cm on an AK: a scale-free yardstick
        const float radius = stroke * 0.45f;
        float tip = -1e30f;
        const auto offLine = [&](const glm::vec3& v, float t) { return glm::length(v - origin - axis * t); };
        for (const glm::vec3& v : verts) {
            const float t = glm::dot(v - origin, axis);
            if (offLine(v, t) < radius) tip = std::max(tip, t);
        }
        if (tip >= 0.0f) {
            glm::vec3 sum(0.0f);
            int n = 0;
            for (const glm::vec3& v : verts) {
                const float t = glm::dot(v - origin, axis);
                if (t > tip - stroke * 0.06f && offLine(v, t) < radius) { sum += v; ++n; }
            }
            const glm::mat4 rootInv = root >= 0 ? glm::inverse(globals[root]) : glm::mat4(1.0f);
            m_Barrel.Detected = true;
            m_Barrel.DetectedOrigin = glm::vec3(rootInv * glm::vec4(sum / (float)n, 1.0f));
            m_Barrel.DetectedDirection = glm::normalize(glm::mat3(rootInv) * axis);
            char msg[160];
            std::snprintf(msg, sizeof msg, "First-person: bolt stroke %.1f, muzzle %.1f ahead of the bolt (model units x100).",
                          stroke * 100.0f, tip * 100.0f);
            Log::Info(msg);
        }
    }
    const FirstPersonMuzzleSettings& mz = m_Set.Muzzle;
    if (!mz.Auto && glm::length(mz.Direction) > 1e-6f) {
        m_MuzzleLocal = mz.Origin;
        m_BoreLocal = glm::normalize(mz.Direction);
        m_HaveMuzzle = true;
    } else if (mz.Auto && m_Barrel.Detected) {
        m_MuzzleLocal = m_Barrel.DetectedOrigin;
        m_BoreLocal = m_Barrel.DetectedDirection;
        m_HaveMuzzle = true;
    } else {
        m_Barrel.Problem = bolt < 0 || glm::length(m_BoltStroke) < 1e-5f
                               ? "Auto needs a bolt bone and explicit Bolt Travel (or optional Fire clip measurement)."
                               : "Auto couldn't find the weapon mesh's front face along the bolt's travel.";
        Log::Warn("First-person: no muzzle - " + m_Barrel.Problem +
                  " Rounds hit nothing and there's no laser; set the muzzle by hand in the weapon Inspector (Barrel & Laser).");
    }
    m_Barrel.HasMuzzle = m_HaveMuzzle;
    PublishBarrelReport(m_SetFile.u8string(), m_Barrel);
}

void FirstPersonPresentation::ReloadIfChanged(float dt) {
    m_ReloadPoll += dt;
    if (m_SetFile.empty() || m_ReloadPoll < 0.25f) return;
    m_ReloadPoll = 0.0f;
    if (m_Slot < (int)m_SlotPrefabs.size() && !m_SlotPrefabs[m_Slot].empty()) {
        std::error_code prefabError;
        const auto time = std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(m_SlotPrefabs[m_Slot])), prefabError);
        if (!prefabError && time != m_SlotPrefabTimes[m_Slot]) {
            std::string set, why; MuzzleEffectSettings muzzle;
            if (Gameplay::ResolveWeaponPrefab(m_SlotPrefabs[m_Slot], set, why, &muzzle)) {
                std::string gameplayError;
                if(!Gameplay::ResolveWeaponGameplay(m_SlotPrefabs[m_Slot],m_Set.Gameplay,gameplayError))Log::Warn(gameplayError);
                m_SlotMuzzles[m_Slot] = muzzle; m_SlotPrefabTimes[m_Slot] = time;
                SetupWeaponParticles();
            } else Log::Warn("Weapon particle systems: " + why);
        }
    }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(m_SetFile, ec);
    if (ec) return;
    std::filesystem::file_time_type recoilStamp{};
    if (!m_Set.RecoilProfile.empty()) {
        recoilStamp = std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(m_Set.RecoilProfile)), ec);
        if (ec) recoilStamp = {};
    }
    std::filesystem::file_time_type shakeStamp{};
    if(!m_Set.CameraShakeProfile.empty()) {
        shakeStamp=std::filesystem::last_write_time(std::filesystem::u8path(ProjectPaths::Resolve(m_Set.CameraShakeProfile)),ec);
        if(ec) shakeStamp={};
    }
    if (stamp == m_SetFileTime && recoilStamp == m_RecoilFileTime && shakeStamp==m_ShakeFileTime) return;
    m_SetFileTime = stamp;
    m_RecoilFileTime = recoilStamp;
    m_ShakeFileTime=shakeStamp;
    FirstPersonAnimationSet fresh;
    std::string why;
    if (!FirstPersonAnimationSet::LoadFile(m_SetFile.u8string(), fresh, &why)) {
        Log::Warn("First-person presentation: kept the running weapon tuning; the edited file doesn't load: " + why);
        return;
    }
    if(!m_SlotPrefabs[m_Slot].empty() && !Gameplay::ResolveWeaponGameplay(m_SlotPrefabs[m_Slot],fresh.Gameplay,why)) {
        Log::Warn("Kept running project weapon stats: "+why);return;
    }
    // Numbers (and the IK bones) only: the rigs and controller need a restart of Play to change.
    const bool rebuildIK = !SameIKSetup(m_Set.Procedural.IK, fresh.Procedural.IK);
    const FirstPersonAdsSettings& oldAds = m_Set.Ads;
    const FirstPersonMuzzleSettings& oldMuzzle = m_Set.Muzzle;
    const bool remuzzle = oldMuzzle.Auto != fresh.Muzzle.Auto || oldMuzzle.Origin != fresh.Muzzle.Origin ||
                          oldMuzzle.Direction != fresh.Muzzle.Direction;
    // The saved sight line cleared (Re-measure in the Inspector): measure it afresh.
    const bool resight = m_Set.Gameplay.HasSightLine && !fresh.Gameplay.HasSightLine;
    const bool remeasure = rebuildIK || oldAds.ReferenceState != fresh.Ads.ReferenceState ||
                           oldAds.CarryTag != fresh.Ads.CarryTag || oldAds.MatchElbows != fresh.Ads.MatchElbows ||
                           oldAds.MatchTwist != fresh.Ads.MatchTwist || oldAds.ActionBones != fresh.Ads.ActionBones;
    m_Set.Gameplay = fresh.Gameplay;
    m_Set.Ads = fresh.Ads;
    m_Set.ViewPosition = fresh.ViewPosition;
    m_Set.AdsLocomotionScale = fresh.AdsLocomotionScale;
    m_Set.ActionCamera=fresh.ActionCamera;
    if(m_Set.CameraShakeProfile!=fresh.CameraShakeProfile) { m_CameraShake.Reset(); m_ShakePose={}; }
    m_Set.CameraShakeProfile=fresh.CameraShakeProfile;
    m_Set.CameraShakeProfileGuid=fresh.CameraShakeProfileGuid;
    m_Set.CameraShake=fresh.CameraShake;
    const bool rebolt = m_Set.Procedural.Recoil.BoltTravel != fresh.Procedural.Recoil.BoltTravel ||
        m_Set.Procedural.Recoil.BoltBone != fresh.Procedural.Recoil.BoltBone ||
        m_Set.Procedural.Recoil.BoltCycle != fresh.Procedural.Recoil.BoltCycle;
    if (m_Set.RecoilProfile != fresh.RecoilProfile) m_Procedural.Reset();
    m_Set.Procedural = fresh.Procedural;
    m_Set.RecoilProfile = fresh.RecoilProfile;
    m_Set.RecoilProfileGuid = fresh.RecoilProfileGuid;
    m_Set.HipFireClip = fresh.HipFireClip;
    m_Set.Muzzle = fresh.Muzzle;
    m_Set.Eject = fresh.Eject;
    m_Set.Laser = fresh.Laser;
    if (rebolt && m_Controller && m_Assets) SetupBolt(*m_Assets, *m_Controller);
    if (remuzzle && m_WeaponModel)
        SetupMuzzle(glm::length(m_BoltStroke) >= 1e-5f ? m_WeaponModel->NodeIndex(m_Set.Procedural.Recoil.BoltBone) : -1);
    ApplyMuzzleAttachment();
    if (resight) {
        m_SightMeasured = m_SightLogged = false;
        m_SightSettled = 0.0f;
        m_Barrel.SightMeasured = false;
        PublishBarrelReport(m_SetFile.u8string(), m_Barrel);
    }
    if (rebuildIK && m_World && m_World->Registry.valid(m_Arms)) {
        m_World->Registry.remove<IKRigComponent>(m_Arms);
        m_UsesIK = SetupIK();
        SetupSway();
    }
    if (remeasure) SetupAdsCarry();
    // The managed reload button reads the current descriptor on every input update.
    m_Ammo = std::min(m_Ammo, m_Set.Gameplay.Magazine);
}

bool FirstPersonPresentation::AttachAndValidate(AssetLibrary& assets, const AnimatorController& ctrl) {
    if (!m_ArmsModel || !m_WeaponModel) return false;
    // Resolve every clip up front. This is intentionally all-or-nothing: an incorrect path or
    // skeleton is reported when Play begins, not halfway through a reload animation.
    const int armsTrack = ctrl.TrackIndex(TrackOr(ctrl, "arms", 0));
    const int weaponTrack = ctrl.TrackIndex(TrackOr(ctrl, "weapon", 1));
    auto check = [&](Model& model, const std::string& clip, const std::string& state, const char* track) {
        if (clip.empty() || ResolveAnimationClip(model, clip, assets) >= 0) return true;
        SetError("state '" + state + "' cannot attach " + track + " clip '" + clip + "'");
        return false;
    };
    // What the driver expects of this weapon, as warnings (a missing Shot / Refill event, a tag or a bone silently
    // turns a feature off): the same list the Weapon Inspector shows.
    {
        FirstPersonWeaponCheckInput wi;
        wi.Set = &m_Set;
        wi.Controller = &ctrl;
        wi.HasArmsBone = [arms = m_ArmsModel.get()](const std::string& b) { return arms->NodeIndex(b) >= 0; };
        wi.HasWeaponBone = [weapon = m_WeaponModel.get()](const std::string& b) { return weapon->NodeIndex(b) >= 0; };
        for (const FPBody::Check& c : FirstPersonWeaponValidate(wi))
            if (c.Level == FPBody::Severity::Warning || c.Level == FPBody::Severity::Error)
                Log::Warn("First-person weapon: " + c.Message + " " + c.Hint);
    }
    for (const auto& L : ctrl.Layers)
        for (const auto& s : L.States)
            for (auto [model, track, name] : {std::tuple<Model*, int, const char*>{m_ArmsModel.get(), armsTrack, "arms"},
                                              std::tuple<Model*, int, const char*>{m_WeaponModel.get(), weaponTrack, "weapon"}}) {
                const auto& m = s.MotionFor(track);
                if (!check(*model, m.Clip, s.Name, name)) return false;
                for (const auto& ch : m.Children)
                    if (!check(*model, ch.Clip, s.Name, name)) return false;
            }
    return true;
}

AnimatorControllerComponent* FirstPersonPresentation::Animator() const {
    if (!m_World || m_Arms == entt::null || !m_World->Registry.valid(m_Arms)) return nullptr;
    return m_World->Registry.try_get<AnimatorControllerComponent>(m_Arms);
}

bool FirstPersonPresentation::HasTag(const char* tag) const {
    const auto* ac = Animator();
    return ac && ac->HasTag(tag);
}

bool FirstPersonPresentation::ActionBlocksSprint() const {
    const auto* ac = Animator();
    if (!ac || !m_Equipped) return false;
    for (const char* trigger : {K::kReload, K::kMagCheck, K::kInspect, K::kMelee})
        if (ac->GetFloat(trigger) > 0.0f) return true;
    if (ac->HasTag(K::kTagReload) || ac->HasTag(K::kTagBusy)) return true;
    if (!m_Controller || m_Controller->Layers.empty() || ac->Layers.empty()) return false;
    const auto& states = m_Controller->Layers[0].States;
    for (const auto& item : ac->Layers[0].Stack)
        if (item.State >= 0 && item.State < (int)states.size() && (states[item.State].HasTag(K::kTagReload) || states[item.State].HasTag(K::kTagBusy)))
            return true;
    return false;
}

const std::string& FirstPersonPresentation::CurrentState() const {
    static const std::string kNone;
    const auto* ac = Animator();
    return ac ? ac->StateName : kNone;
}

void FirstPersonPresentation::Tick(float dt, const glm::vec3& velocity, bool sprinting, bool aiming, float lean, bool grounded) {
    sprinting=sprinting && !aiming && !ActionBlocksSprint();
    m_PlayerVelocity = velocity;
    auto* ac = Animator();
    if (!ac) return;
    // Switching weapons: once the one in hand is put away, the other one's rigs come in.
    if(RunLoadout(4).Commands&1) {
        SwapToPendingSlot();
        return;
    }
    if (LiveReload::Enabled()) ReloadIfChanged(dt);
    RunGameplay(7,dt);
    const FirstPersonWeaponGameplay& g=m_Set.Gameplay;
    const float planarSpeed=glm::length(glm::vec2(velocity.x,velocity.z));
    ac->SetFloat(K::kSpeed,WeaponAnimationInputSpeed(planarSpeed,m_SwayMoveInput,m_HaveSwayMoveInput,m_WalkSpeed));
    m_PlanarSpeed=planarSpeed;
    ac->SetBool(K::kSprint, sprinting);
    // The aim press drives the corner peek, even up against cover; tucked off a wall with no
    // peek to lean out on, the sights can't come up.
    if (aiming != m_Aiming && m_Equipped && !ac->HasTag(K::kTagHidden)) WeaponSound(m_SetFile, aiming ? "ads_in" : "ads_out");
    WeaponAudio::Get().SetFocus(aiming && m_Equipped); // the mix's focus: the world comes forward down the sights
    m_Aiming = aiming;
    if (WallBlocked() && m_PeekSide == 0) aiming = false;
    ac->SetBool(K::kAim, aiming);
    const float aimBlendTime=m_Set.Procedural.Aim.BlendTime;
    ac->SetFloat("TransitionDuration:"+(m_Set.Ads.ReferenceState.empty()?std::string("Aim"):m_Set.Ads.ReferenceState),aimBlendTime);
    // Gated on a gun in hand, as the procedural's in.Ads is: holstered with aim held, the ADS hold (the carried pose, the
    // optic's correction, the locomotion layers' ADS scale) must not ease in on the empty hands.
    const bool adsHeld = aiming && m_Equipped;
    m_AdsHold = aimBlendTime > 0.0f
                    ? std::clamp(m_AdsHold + (adsHeld ? dt : -dt) / aimBlendTime, 0.0f, 1.0f)
                    : (adsHeld ? 1.0f : 0.0f);
    m_TickDt = dt;
    // The jog (Shift): the locomotion layer's run clip, as authored, on the feet's phase (GaitPhase); off while aiming, in a
    // reload / inspect / melee, and for a moment after a shot, so the gun comes up to fire.
    {
        m_JogHold = std::max(0.0f, m_JogHold - dt);
        const bool jog = m_Jogging && m_Equipped && !aiming && !ActionBlocksSprint() && m_JogHold <= 0.0f && planarSpeed > 0.5f;
        ac->SetBool(K::kJog, jog);
        // The walk / run sway's clock: the body's feet (negative - none - leaves the clips on their own).
        ac->SetFloat(K::kGaitPhase, m_HaveGait ? m_GaitPhase : -1.0f);
    }
    ac->SetFloat("LayerWeight:Weapon Locomotion",glm::mix(1.0f,m_Set.AdsLocomotionScale,m_AdsHold));
    ac->SetFloat("LayerWeight:Weapon Walk",glm::mix(1.0f,m_Set.AdsLocomotionScale,m_AdsHold));
    // ADS zoom: in while the sights are up - Aim, or a reload / mag check carried onto them -
    // and out otherwise. A critically damped spring eases both ends, and a re-press mid-way
    // turns around smoothly instead of restarting.
    {
        bool onSights = ac->HasTag(K::kTagAds);
        for (const AdsCarryAction& a : m_AdsCarry.Actions) onSights = onSights || (aiming && a.State == ac->State);
        const float target = onSights && m_Equipped ? 1.0f : 0.0f;
        const float time = m_Set.Ads.ZoomTime;
        if (time <= 0.0f) {
            m_Zoom = target;
            m_ZoomRate = 0.0f;
        } else if (dt > 0.0f) {
            const float w = 4.7f / time; // ~98% settled after `time`
            const float x = m_Zoom - target;
            const float e = std::exp(-w * dt);
            const float next = (x + (m_ZoomRate + w * x) * dt) * e;
            m_ZoomRate = (m_ZoomRate - (m_ZoomRate + w * x) * w * dt) * e;
            m_Zoom = std::clamp(target + next, 0.0f, 1.0f);
        }
    }
    // Stock lock: shouldered in the weapon's listed states, or anything carried on the sights. In the others (a reload, an
    // inspect) as much as the clip has the gun back where it sits shouldered (the butt in the view, learned at the hip):
    // the lock comes back with the clip's own return to the hold, not as a move of its own once the state has ended.
    {
        const FirstPersonStockLockSettings& sl = m_Set.StockLock;
        bool shouldered = false;
        float target = 0.0f;
        glm::vec3 butt, forward;
        const bool haveButt = StockWorld(butt, forward);
        const glm::vec3 buttInView = glm::vec3(m_View * glm::vec4(butt, 1.0f));
        if (sl.Enabled && m_Equipped && !ac->HasTag(K::kTagHidden)) {
            shouldered = m_Zoom > 0.5f;
            for (const std::string& tag : sl.Tags) shouldered = shouldered || ac->HasTag(tag.c_str());
            target = shouldered ? 1.0f : 0.0f;
            if (!shouldered && haveButt && m_HaveShoulderedButt) {
                const float t = std::clamp((glm::length(buttInView - m_ShoulderedButt) - 0.03f) / 0.05f, 0.0f, 1.0f);
                target = 1.0f - t * t * (3.0f - 2.0f * t); // all of it within 3 cm, none past 8
            } else if (!shouldered && !m_HaveShoulderedButt && CurrentState() == "Draw") {
                // A gun's first draw, its hold not learned yet: in over the draw's settle into the hold.
                const float t = std::clamp((ac->StateTime - 0.5f) / 0.5f, 0.0f, 1.0f);
                target = t * t * (3.0f - 2.0f * t);
            }
        }
        if (shouldered && haveButt && m_Zoom < 0.05f && m_StockLockWeight > 0.99f) {
            m_ShoulderedButt = m_HaveShoulderedButt ? glm::mix(m_ShoulderedButt, buttInView, 1.0f - std::exp(-dt * 2.0f)) : buttInView;
            m_HaveShoulderedButt = true;
        }
        const float step = sl.BlendTime > 0.0f ? dt / sl.BlendTime : 1.0f;
        m_StockLockWeight = std::clamp(m_StockLockWeight + std::clamp(target - m_StockLockWeight, -step, step), 0.0f, 1.0f);
    }
    ac->SetBool(K::kEquipped, m_Equipped);
    ac->SetInt(K::kAmmo, m_Ammo);
    // A tube loaded a round at a time: the load loop leaves on LastRound (load it and finish) or
    // StopReload (the trigger was pulled: finish the round in hand), which lasts the reload out.
    if (g.Reload == FirstPersonWeaponGameplay::ReloadMode::PerRound) {
        ac->SetBool(K::kLastRound, m_Ammo >= g.Magazine - 1);
        ac->SetBool(K::kStopReload, m_StopReload);
    }
    // The procedural stack, fed from what the controller is doing and how the camera moved.
    WeaponProceduralInput in;
    in.Dt = dt;
    in.Velocity = glm::vec3(glm::dot(velocity, m_FlatRight), 0.0f, -glm::dot(velocity, m_FlatForward));
    in.Sprinting = sprinting && planarSpeed > 0.1f;
    in.VerticalVelocity = velocity.y;
    in.Grounded = grounded;
    in.WallDistance = m_Equipped ? m_WallDistance : -1.0f;
    in.WallFacesUp = m_WallFacesUp;
    in.WallSide = m_WallSide;
    in.Ads = aiming && m_Equipped;
    // Aim intent starts both blends together; carried actions already ease through m_AdsHold.
    in.IKOff = ac->HasTag(K::kTagHidden) || (!m_Set.Procedural.IK.OffTag.empty() && ac->HasTag(m_Set.Procedural.IK.OffTag.c_str()));
    m_IKCurves = m_Controller ? SampleIKCurves(m_Set.Procedural.IK, *m_Controller, *ac) : IKCurveWeights{};
    in.IKCurve = m_IKCurves.All;
    in.Crouch = m_Crouch;
    in.Lean = m_Equipped ? std::clamp(lean + m_PeekLean, -1.0f, 1.0f) : 0.0f;
    in.WalkSpeed = m_WalkSpeed;
    in.SprintSpeed = m_SprintSpeed;
    in.HaveGaitPhase = m_HaveGait;
    in.CameraBobScale = m_CameraBobScale;
    in.GaitPhase = m_GaitPhase;
    in.StateName = &ac->StateName;
    in.StateTags = &ac->StateTags;
    const WeaponProceduralPose& pose = m_Procedural.Update(m_Set.Procedural, in);
    m_ShakePose=m_CameraShake.Update(dt);
    ac->SetFloat(K::kWalkRate, pose.WalkRate);
    ac->SetFloat(K::kSprintRate, pose.SprintRate);
    if (m_Set.Procedural.Sway.Enabled && m_ArmsModel) {
        glm::vec2 move=m_SwayMoveInput;
        if (!m_HaveSwayMoveInput) {
            move={in.Velocity.x,-in.Velocity.z};
            move/=std::max(m_WalkSpeed,0.01f);
            if (glm::length(move)>1) move=glm::normalize(move);
        }
        const bool valid=m_ArmsModel->NodeIndex(m_Set.Procedural.Sway.Unity.WeaponBone)>=0;
        const auto sway=m_Sway.Update(m_Set.Procedural.Sway.Unity,move,m_SwayLookInput,aiming,dt,pose.IKWeight,valid);
        if (auto* rig=m_World->Registry.try_get<IKRigComponent>(m_Arms)) rig->SwayPose=sway;
    }
    m_SwayLookInput=glm::vec2(0);
    m_HaveSwayMoveInput=false;
    WriteIK();
}

void FirstPersonPresentation::Update(World& world, Camera& camera) {
    if (!IsActive() || !world.Registry.valid(m_Arms) || !world.Registry.valid(m_Weapon)) return;

    // The player's own look, before any punch goes on: the sway reads how fast it turns.
    RemoveViewKick(camera);
    m_LookPitch = camera.Pitch;
    glm::vec2 controllerInput(0.0f);
    if (m_HavePlayerLookBaseline) {
        const glm::vec2 delta(camera.Pitch - m_PlayerLookBaseline.x,
                             std::remainder(camera.Yaw - m_PlayerLookBaseline.y, 360.0f));
        controllerInput = delta;
    }
    if (m_Set.Procedural.Recoil.UnityPort && m_HaveRawLookInput) controllerInput=m_RawLookInput;
    const glm::vec2 swayLook=m_HaveRawLookInput?m_RawLookInput:controllerInput;
    m_SwayLookInput+=glm::vec2(swayLook.y,swayLook.x);
    if (m_Set.Procedural.Recoil.UnityPort || m_Set.Procedural.Recoil.AimCompensation) m_Procedural.CompensateAim(controllerInput);
    m_HaveRawLookInput=false;
    {
        glm::vec3 f = camera.Front();
        f.y = 0.0f;
        m_FlatForward = glm::length(f) > 1e-5f ? glm::normalize(f) : glm::vec3(0.0f, 0.0f, -1.0f);
        m_FlatRight = glm::normalize(glm::cross(m_FlatForward, glm::vec3(0.0f, 1.0f, 0.0f)));
    }
    UpdateCornerPeek(camera);
    // Aim climb: unlike the punch below it stays - the player has to pull it back down.
    {
        const glm::vec2 climb = m_Procedural.ConsumeAimKick();
        const float before = camera.Pitch;
        camera.Pitch = std::clamp(camera.Pitch + climb.x, -89.0f, 89.0f);
        camera.Yaw += climb.y;
        m_Procedural.DiscardClampedAim(glm::vec2(climb.x - (camera.Pitch - before), 0.0f));
    }
    m_PlayerLookBaseline = glm::vec2(camera.Pitch, camera.Yaw);
    m_HavePlayerLookBaseline = true;
    ApplyViewPunch(camera);
    // What's in front of the gun, for it pulling back off walls - probed from where the eye
    // actually is this frame (leaned, bobbed) and along the rolled barrel line, so peeking past a
    // corner doesn't hit the corner the body is behind. The nearer hit wins, so a wall on the
    // gun's side or a door frame counts as well as what's dead ahead; its surface says which way
    // to tuck (up off a table, aside off an edge beside the barrel).
    m_WallDistance = -1.0f;
    m_WallFacesUp = false;
    m_WallSide = 0.0f;
    if (const auto& ob = m_Set.Procedural.Obstruction; ob.Enabled && ob.Reach > 0.0f) {
        const glm::vec3 f = camera.Front(), right = camera.Right(), up = camera.Up();
        const float d[3] = {f.x, f.y, f.z};
        QueryFilter filter;
        filter.HitTriggers = 0;
        for (const glm::vec3& from : {camera.Position, camera.Position + right * ob.ProbeOffset.x + up * ob.ProbeOffset.y}) {
            const float origin[3] = {from.x, from.y, from.z};
            RaycastHit hit;
            if (PhysicsWorld::SphereCastSolid(origin, d, 0.04f, ob.Reach, filter, hit) && hit.Hit &&
                (m_WallDistance < 0.0f || hit.Distance < m_WallDistance)) {
                const glm::vec3 n(hit.Normal[0], hit.Normal[1], hit.Normal[2]);
                m_WallDistance = hit.Distance;
                m_WallFacesUp = n.y > 0.6f;
                m_WallSide = glm::dot(n, right);
            }
        }
    }

    auto* ac = world.Registry.try_get<AnimatorControllerComponent>(m_Arms);
    if (ac) {
        // What the controller did last frame: rounds spent on hip fire, a reload that landed.
        if (m_WaitingAnimatedShot && ac->EventFired(K::kEventShot) && !m_Set.HipShotsProcedural() &&
            !ac->HasTag(K::kTagAds)) {
            m_WaitingAnimatedShot = false;
            CommitShot(false, false);
        }
        if (ac->EventFired(K::kEventEject) && m_Set.Eject.Enabled && m_Set.Eject.When == FirstPersonEjectSettings::Trigger::Event) {
            // The action worked: the spent hull comes out - one per Eject event, so a cylinder's six at one time throw six.
            const int n = std::max(1, (int)std::count(ac->FiredEvents.begin(), ac->FiredEvents.end(), std::string(K::kEventEject)));
            if (m_Set.Eject.Origins.size() > 1 && m_PendingEjects == 0) {
                // A new batch starts at the first chamber; the rounds still unfired come out live.
                m_EjectIndex = 0;
                m_LiveEjects = m_Set.Eject.LiveModel.empty() ? 0 : std::clamp(m_Ammo, 0, n);
            }
            m_PendingEjects += n;
        }
        RunGameplay(8,0.0f,false,false,(ac->EventFired(K::kEventRefill)?1:0)|(ac->EventFired(K::kEventLoadRound)?2:0));
        // The audio the controller's states carry: events named snd.<gun>.<element> play that set.
        for (const std::string& e : ac->FiredEvents)
            if (e.rfind("snd.", 0) == 0) WeaponSound(m_SetFile, e);
        ac->FiredEvents.clear();
        // Triggers live for exactly one controller update: an input the controller refused
        // (fire during a reload, say) is dropped rather than firing late.
        for (const char* t : {K::kFire, K::kReload, K::kMagCheck, K::kInspect, K::kMelee, K::kFidget, K::kCycle}) ac->ResetTrigger(t);
    }

    ApplyHidden(world);
    PlaceRigs(world, camera);
}

void FirstPersonPresentation::LateUpdate(World& world, Camera& camera) {
    if (!IsActive()) return;
    // Again once the animators have run: the frame Holster hands over to Holstered would
    // otherwise render visible, in Holstered's pose (Holster's first frame).
    ApplyHidden(world);
    Camera placementCamera=camera;
    if(m_ArmsModel && m_Controller && m_Assets) {
        CameraEffectPose action;
        if(const auto* ac=Animator())
            action=AnimatorActionCamera(*m_ArmsModel,m_WeaponModel.get(),*m_Assets,*m_Controller,*ac,
                m_Set.ActionCamera,m_CameraBone,QuaternionFromEulerYXZ(m_Set.ViewRotation)*QuaternionFromEulerYXZ(m_Rotation),m_Scale,m_Zoom);
        const glm::vec3 before(camera.Pitch,camera.Yaw,camera.Roll),position=camera.Position;
        // The rig already subtracts its animated eye anchor. Carry it through the authored
        // translation once; shake remains camera-only and must not drag the gun with it.
        placementCamera.Position+=glm::mat3(camera.Right(),camera.Up(),-camera.Front())*action.Position;
        ApplyCameraEffect(camera,action);
        ApplyCameraEffect(camera,m_ShakePose);
        m_KickAngles+=glm::vec3(camera.Pitch,camera.Yaw,camera.Roll)-before;
        m_KickOffset+=camera.Position-position;
        m_KickApplied=true;
    }
    PlaceRigs(world, camera,&placementCamera,/*finalPass=*/true);
    for (; m_PendingShots > 0; --m_PendingShots) FireShot();
    for (; m_PendingEjects > 0; --m_PendingEjects) Eject();
}

// Unarmed hides the rigs the way an unticked "active" box does. DeactivatedTag is what keeps
// InactiveTag from being recomputed away by World::SyncActiveInHierarchy; the animators keep
// running (UpdateWhenInactive) so Draw can bring them back.
void FirstPersonPresentation::ApplyHidden(World& world) {
    const auto* ac = Animator();
    const bool hidden = ac && ac->HasTag(K::kTagHidden);
    if (hidden == m_HiddenApplied) return;
    for (entt::entity e : {m_Arms, m_Weapon}) {
        if (hidden) {
            world.Registry.emplace_or_replace<DeactivatedTag>(e);
            world.Registry.emplace_or_replace<InactiveTag>(e);
        } else {
            world.Registry.remove<DeactivatedTag, InactiveTag>(e);
        }
    }
    m_HiddenApplied = hidden;
}
