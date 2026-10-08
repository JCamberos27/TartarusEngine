# Engine and game ownership

Updated 2026-10-07. The five audited migrations and the accompanying HUD, audio,
locomotion, outfit and content-authoring policies now run in project C# or project assets.
Native execution adapters remain; their presence does not make a cached runtime view an
engine-authored gameplay component. Validation results are recorded below after each run.

## The ownership rule

Project C# owns game rules, gameplay defaults, loadouts, health, abilities, enemy decisions,
spawning, HUD content and project-specific authoring conventions. Project assets own models,
clips, sounds, tuning, animation graphs and content-import catalogs. C++ owns scene/resource
lifetime, rendering, physics and geometric queries, animation evaluation, IK, audio processing,
asset infrastructure, editor widgets and the managed host.

A puzzle project can start and export without the FPS integration, Sandbox, soldier or weapon
assets. Ordinary Script/MonoBehaviour assemblies do not implement a game dispatcher. The
SDK accepts zero or one optional `IProjectIntegration` with `Invoke`, `Request` and reload-state
methods; it assigns no game meaning to operation names. FPS frames and enums are private
sample contracts in `project/assets/Scripts/GameFrames.cs` and `GameplayContracts.cs`, generated
with `tools/generate_game_frames.py`. The generic lifecycle/service ABI has its own generator.

## The five migrations

| Audited candidate | Project owner | Native execution |
| --- | --- | --- |
| Weapon and player data | WeaponDefinition, WeaponData, PlayerDefinition, PlayerController, WeaponController, WeaponLoadout | Camera/character queries, rig construction, animator/recoil commands and asset resolution |
| Health, damage, death and respawn | Health/HealthRules, DamageRules, VitalsRules, NpcDamageRules, GameSession | Collider/bone hits, teleporting, ragdoll creation and effect submission |
| Basketball and contact sounds | BasketGoal, Scoreboard, ScoreDigit, ImpactSound | Completed contact/trigger batches, physics body queries, audio/particle/material operations |
| Gravity ability | GravityAbility | Geometric candidate/trajectory queries and a validated physics carry servo |
| Enemy AI, loadouts, spawning and squads | NpcDefinition, SquadDefinition, NpcConfigData, AiRules, NpcDecisions, NpcBehaviour, NpcCoverSelection, NpcBehaviourTransitions, NpcPerception, NpcSquads, NpcCombat, NpcLifecycle, NpcCallouts, NpcShotEffects | Recast/Detour/crowd work, ray/cover queries, capsule motion, posed hitboxes, batched skeleton/IK and ragdoll work |

Weapon definitions retain authored RPM, magazine, pellets, damage and asset GUIDs; `.fpsanim`
is presentation data. Legacy gameplay blocks import through project WeaponData. Native player,
NPC/squad and sound/effect configuration structs are unregistered resolved views, initialized
from C# fields. Disabling/removing a definition removes its view. HealthComponent and the
native scoring system have been removed. NPC health used by geometry/presentation is a
projection; damage cannot overwrite the authoritative C# health record.

GameSession chooses weapon input ownership, attachment/slot actions, cheat effects, hit
feedback and player respawn commands. Native main-loop code supplies inputs/geometric
results and executes those commands in the established frame order. WeaponLoadout owns
slot cycling, equip transitions, pending swaps and magazine refill policy.

NpcLifecycle chooses crowded spawn offsets, random loadouts, movement speed/stance,
bleed-out/corpse deadlines, player noises, camping state, near-miss suppression, morale,
respawn ranking and shot targets. NpcShotEffects chooses tracer/flyby cadence, reload/pump
cues and friendly-hit effects. Native director code retains actor/resource bookkeeping and
executes project requests. It does not provide an alternate native AI rule implementation.

## Other audited policies

* CombatHud owns kill-feed/streak text, ammo/reload layout and awareness/debug overlays.
  DevTools owns the developer panel, hotkeys, god/infinite-ammo/invisibility rules and
  kill/respawn actions. RuntimeCanvas and RuntimeGui expose scoped generic drawing/widgets.
* WeaponAudioDefinition, FoleyDefinition, ImpactAudioDefinition and EffectsDefinition own
  authored sound/effect settings. AudioPolicy, FoleyPolicy and ImpactPolicy own weapon/pack
  names, sound keys, shot layers, tail cadence, footsteps/landing choices, casing/flyby gains,
  surface fallback and combat cues. Decoding, voice pools, spatialization, occlusion, reverb,
  convolution, mixing and limiting remain native.
* BodyLocomotion owns start/stop/crouch/airborne/turn decisions and action parameters.
  BodyGraphAuthoring uses the project's locomotion template and clip mappings. Pose blending,
  root-motion extraction, foot/hand solving and mesh work remain native.
* WardrobePolicy owns pack naming/gender conventions, default layers and random appearance
  choices. Generic authored catalog/conflict rules, skeleton assembly, skin hiding and
  coverage baking remain native. The native RNG service preserves the selection stream.
* ContentTools and `assets/Editor/Content/{BloodImport,KnifeImport}.json` own pack catalogs
  and import destinations. Native import code converts the supplied VAT/texture data;
  renderers request optional project library locations. There are no required sample paths
  in ordinary-project startup or C# bootstrap.

## Project creation, builds and export

Startup comes from project settings/build scenes. The editor remembers a last scene only
when it belongs to the current project. No configured startup scene means an empty scene.
A new C# project receives generic build files and an empty source folder; FPS sources are
not copied automatically. Gameplay and editor assemblies load only from the current
project's `Scripts/bin`. A staged sample DLL never substitutes for that project.

`TartarusRuntime` builds the engine SDK/host. `TartarusScripts` builds this workspace's sample
and integration fixtures. `TARTARUS_BUILD_SAMPLE_GAME=OFF` removes the sample target from the
engine's dependencies. It defaults to ON for this sample workspace. Exports copy the current
project's script output into the exported project and exclude editor DLL/source and bundled
sample gameplay DLLs.

## Reload, migration and performance

Legacy component loading merges into C# fields while retaining existing overrides, unrelated
slots, stable slot IDs and GUID references. Saving writes the script representation; a second
migration is a no-op. Authored sample scenes are converted. Stop restores the authored world.

Project-wide HUD/health state and attached script state survive transactional reload. OnReload
runs with scene services after Awake/OnEnable without repeating Start. Physics callbacks are
delivered once per completed simulation batch after Start and before Update. Scoped handles
expire at callback return. Native services retain ownership of pointers and graphics objects.

Frequent policies use POD frames and batched data. Short UTF-8 service calls use stack storage,
and project operation names are reused without per-frame strings. Cold naming/profile requests
are cached by code generation. Native geometry, animation and audio loops stay batched.
The separate editor interaction-hitch fix is documented in [PERFORMANCE.md](PERFORMANCE.md).

## Further engine API work

The migration uses private sample adapters for bulk character/navigation/cover/body-part
services. A public, game-independent high-level character/nav/cover API can replace them in
future projects. The public carry backend currently supports one simultaneous constraint;
multiple constraints and a public batch trajectory API remain engine feature work. Managed
RaycastHit does not yet expose every posed body-part identifier. These are API limitations,
not alternate native gameplay owners.

## Validation

Migration checkpoint before upstream integration, verified 2026-10-07:

* Release builds passed, including `TartarusEngine` with `TARTARUS_BUILD_SAMPLE_GAME=OFF`.
  The workspace setting was restored to ON afterward.
* Project integration: 257 checks; managed lifecycle/reload/editor integration: 225 checks.
  Background compilation was tested with access to the installed NuGet configuration.
* Weapon/audio profile: 239; hit regions: 34; AI math: 41; player controller: 10;
  body graph: 12; FX/HUD: 20; outfit: 5532; foley: 347; full-auto audio: 38;
  animation-event audio: 14; manifest audio: 30; casing contact: 52;
  impact surface: 17; flyby radius: 12; environment audio: 25181. All passed.
* Empty-project bootstrap: 15 checks passed. It builds and executes an ordinary C# script
  without copying FPS source or loading an FPS integration.
* Isolated export succeeded; the exported gameplay DLL matches the project's DLL, and the
  exported Managed folder contains only the generic runtime. The exported executable passed
  all 9 ordinary-script execution and absent-FPS checks against its own project.
* NPC watch scenario: 45 simulated seconds, 11 checks passed, 21600 rendered frames,
  no new log errors. GL debug output was unavailable on this test context.
* Weapon play: 94 of 96 checks passed. The two AK ADS reload hand-anchor checks fail with
  the same 0.283308 maximum weight and zero fully anchored frames recorded in the earlier
  attachment/muzzle test logs. This migration does not change the hand-anchor implementation;
  these existing presentation failures remain unresolved.

Generated native/managed game frames match their generator. Component registration passed
with 28 registered components. Upstream integration adds HierarchyStyleComponent to the
explicit allow-list, bringing runtime views/tags to 30.

### Integration with upstream main

The merge includes upstream `eed924a6` and its editor Enhancers, Inspector attributes,
asset policy and navigation fixes. The generic scripting ABI is now version 3; rebuild
the native host and managed SDK together. Project operations retain dispatch 13–16;
Inspector method/read-out dispatch uses 17–18. Editor reference/color widgets use services
132–133, preserving upstream BeginDisabled/SameLine at 130–131.

The combined Release build and full native/managed unit suite pass: 41,510 checks,
zero failures. Fixtures restore the current project's assembly after Inspector attribute
tests and obtain game defaults from C#. New presentation descriptors omit legacy gameplay
unless explicitly imported. Managed validation errors reach the native caller and successful
builds/reloads clear stale errors.

Custom C# Inspector field results also enter the scene undo transaction before their slot
copy is applied. Explicit `Undo.RecordScene` labels are retained. Semantic JSON comparison
excludes formatting-only changes from history. Editor UI fixtures use the same history API
for their setup edits, rather than mutating the cached authored state
without notifying history.

All 30 rendered editor UI tests pass, including C# Inspector edits/callbacks/read-outs,
undo, Keep Changes After Play, tabs, favorites and ruler. All 19 committed rendering smoke
scenes pass with zero new GL/log errors. The merged host also passes the empty-project
bootstrap (15 checks), isolated exported ordinary-script host (9 checks), and active NPC
watch (11 checks, 45 simulated seconds, 21,600 rendered frames; zero new GL/log errors).
The authored Arena Enemies group remains inactive; NPC testing uses an ignored enabled copy.

The component and button-style structural checks pass. External asset verification covers
1,880 manifest entries with no missing or differing local files; payloads remain outside Git.
187 regenerated texture GUIDs/import settings and 107 corresponding material-only changes
were restored to upstream identities. Availability of the 20 newly listed payloads in the
team's private library still needs verification, as noted in [PROJECT_ASSETS.md](PROJECT_ASSETS.md).
