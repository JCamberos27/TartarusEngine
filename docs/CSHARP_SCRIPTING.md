# C# gameplay and weapon prefabs

Start with the [scripting manual](SCRIPTING_MANUAL.md) for setup, tutorials, editor tools,
player/weapon customization and troubleshooting. The [documentation index](README.md)
links the complete reference and related native systems.

The complete gameplay and editor API, with examples and supported boundaries, is documented
in [SCRIPTING_API.md](SCRIPTING_API.md). Editor sources under `assets/Editor` compile separately
into `Tartarus.Editor.dll`; their windows and commands appear in the C# Tools panel.

Player and weapon gameplay now runs in `project/assets/Scripts/PlayerController.cs` and
`WeaponController.cs`. The editor, renderer, PhysX integration, animation evaluation, IK,
audio and procedural pose evaluation remain C++. Pellet spread, ray queries and impact-force
rules also run in the C# weapon controller, with native physics and effect services.
`FirstPersonPresentation` is the native
presentation adapter; it sends animator tags/events and descriptor values to the C# weapon
controller and executes its animation/audio/recoil commands.

Project C# also owns health/damage, scoring, gravity, enemy AI/spawning/squads, HUD/developer
content, sound policy, locomotion actions, outfit choices and content-pack catalogs. Native
adapters execute geometric, animation, audio and rendering work.

## Build and edit

Install the **x64 .NET 10 SDK** to build or edit scripts. A game requires the x64 .NET 10
runtime. This is a framework-dependent native host; the runtime isn't bundled into the game.

`cmake --build build --config Release --target TartarusEngine` builds both native and managed
code. `--target TartarusRuntime` builds the engine C# SDK/host; `--target TartarusScripts` builds
the sample C# projects and fixtures. SDK output is staged in `build/Release/Managed`; project
assemblies live in `project/Scripts/bin`. Configure `TARTARUS_BUILD_SAMPLE_GAME=OFF` to build
the engine without building the sample.

In the Asset Browser, right-click and choose **Create C# Script**. Scripts are ordinary `.cs`
files under project assets. Double-click to open the embedded [Script IDE](SCRIPT_IDE.md), or
choose **Open Externally** from the context menu. Drag an attachable script
onto an entity, or use **Add Component > Scripts > C# Script** and **Add Script** in its card.
The same object can have several behaviours, including several instances of one class.
Each has its own Enabled checkbox and editable fields. Choose its compiled type from the
Script dropdown. Source links to the `.cs` file; **Edit in Script IDE** opens its source.
For an unusual namespace, use the Script dropdown or Advanced > Class.

Save your `.cs` source to compile automatically. The project watcher coalesces saves before
starting a background build; saves made during a build queue another one. Compiler output goes to
`project/Scripts/build.log`. Successful builds reload automatically, including during Play.
Compiler errors appear in the console and keep the running assembly. You can also build
`project/Scripts/Tartarus.Gameplay.csproj` with `dotnet build`; the packaged editor command
supplies the relocated runtime SDK project reference automatically.
**File > Build C# Gameplay & Editor** is available for a manual rebuild.
On a project without a gameplay `.csproj`, the editor creates generic build files and an
empty source folder, preserving existing sources. It does not install FPS gameplay.

## Scripts and lifecycle

Derive from `Tartarus.MonoBehaviour` and use `override` on callbacks. The API is Unity-inspired,
with the `Tartarus` namespace and `System.Numerics` vectors. All configured script instances
are constructed before callbacks, and all active scripts receive Awake/OnEnable before any
Start/Update. `Awake()` runs once, `Start()` runs once before the first simulation callback,
`Update()` runs per frame, `FixedUpdate()` runs per physics step, and `LateUpdate()` runs after
all script Updates. `Time.deltaTime` supplies scaled elapsed time; `Time.fixedDeltaTime` is
the most recent fixed-step duration. The original `Script.Update(float dt)`,
`FixedUpdate(float dt)`, and `OnCreate()` remain supported (the default Awake calls OnCreate).

Scripts run on the main game thread during Play. Disabling a slot or making its object inactive
calls OnDisable and preserves the instance; re-enabling calls OnEnable without repeating
Awake/Start. Enabled/active changes take effect at the next scripting tick. Removing the script,
destroying its object, or stopping Play calls OnDestroy for initialized instances. Stop then
restores the authored scene snapshot, so Play edits aren't saved into the scene.

Public writable fields appear as ordinary Inspector controls. Private fields marked
`[SerializeField]` appear too; `[HideInInspector]` hides a field while retaining serialized
state. `[Range(min,max)]` gives a slider and `[Tooltip("text")]` adds help text. Supported types
are float, int, bool, string, Vector3, AssetReference and enums backed by int. `[AssetPath(".fpsanim")]`
gives an AssetReference a filtered asset picker; its path and GUID survive serialization.
`[Color]` gives a Vector3 a linear RGB color picker. Static/readonly fields and
`[NonSerialized]` fields are excluded. Unsupported types are labeled in the Inspector.
Values are stored as JSON internally; you edit them through controls. Removed/renamed fields
are discarded and new fields use their C# defaults. Source, Class, Fields, Enabled and the
additional script slots serialize through reflection, including undo, clipboard, scene
snapshots, prefabs and prefab overrides. Slot IDs never get reused on the same component.
A script throwing during a lifecycle callback is logged and stops receiving callbacks until
rebuilt or its component changes. The Inspector's Advanced menu can reset its fields.

String fields also support `[SceneReference]`, `[SoundReferences]` and `[InspectorChoices]`
authoring widgets. Scene references store relative hierarchy paths, not permanent object IDs;
sound slots store project paths in one string. See [SCRIPTING_API.md](SCRIPTING_API.md) for
their arguments and limitations, and [EDITOR_HISTORY_AND_INSPECTORS.md](EDITOR_HISTORY_AND_INSPECTORS.md)
for custom Inspectors.

```csharp
using Tartarus;
using System.Numerics;

namespace Tartarus.Gameplay;

public sealed class Mover : MonoBehaviour
{
    [Range(0, 10)] public float Speed = 2;
    public override void Start()
    {
        GetComponent<Animator>()?.SetBool("Moving", true);
    }
    public override void Update()
    {
        transform.position += Vector3.UnitZ * Speed * Time.deltaTime;
    }
}
```

Save this as Mover.cs, attach it to an object, edit Speed in the Inspector, and press Play.

`Tartarus.Engine` exposes input axes/buttons, logging, entity positions, prefab instantiation,
entity/subtree destruction, animator triggers, physics raycasts, dynamic-body mass and
impulses. Entity IDs include their
generation, so a destroyed entity cannot accidentally refer to a recycled one. Calls on
missing entities return failure; Position throws a managed error. These services are scoped
to lifecycle callbacks (plus native gameplay adapters' physics services). Background-thread
calls are rejected by the managed bridge.

`GetComponent<T>()` returns an attached script or a native wrapper (Transform, Rigidbody,
Animator, Camera, Light, Collider or AudioSource), or null if
missing. `GetComponents<T>()` returns all matching behaviours, including disabled ones.
`gameObject` wraps the current entity; it supports GetComponent/GetComponents, SetActive,
Instantiate and Destroy. `transform.position` is world-space, `localPosition`, `localScale`
and `localRotation` are relative to the parent. Rigidbody exposes mass, velocity, AddForce
with ForceMode and AddImpulseAtPosition. Animator exposes float/int/bool parameters and
triggers. `Input.GetAxis`, `GetButton` and `GetButtonDown` read the project's action map.
These bindings cover the available engine services; this is not binary compatibility with
UnityEngine, and arbitrary Unity packages cannot be dropped in unchanged.

Serialized public and `[SerializeField]` values survive reload automatically. Override
`SaveState()` / `LoadState(string)` for other private runtime state that must survive a
reload, retaining base state if you also need automatic field persistence. These callbacks
should only serialize/restore private state. New types and serialized
fields are validated before swapping assemblies. The native Player/Weapon frame state stays
in the host, so ammunition, pump state, cooldowns and movement survive C# rebuilds. Bob
demonstrates private-state restoration. Reload does not repeat OnCreate or OnDestroy; a
replacement receives its saved state directly. Stop still calls OnDestroy on the current
instance. A script assembly may have zero or one concrete `IProjectIntegration` entry point;
the sample `Gameplay` handles project-owned player/weapon/shot operations. Ordinary script
assemblies need no FPS root. Project-wide state can use the integration SaveState/LoadState hooks. These built-in
player/weapon controllers are driven through native gameplay adapters; ordinary behaviours
you create use the attachable MonoBehaviour workflow above.

## Weapon prefabs

The two supplied prefabs are:

* `assets/Weapons/AKS74U/AKS74U.prefab`
* `assets/Weapons/Remington870/Remington870.prefab`

Each has a placed weapon model, an Animator Controller on its weapon track, and a Weapon
Definition with a description and GUID-tracked `.fpsanim` reference. That descriptor bundles
the arms/weapon rigs, materials, controller, recoil, camera shake, muzzle, ejection, audio
identity and presentation values. WeaponDefinition owns gameplay values. Drop a prefab into a scene or double-click for Prefab Mode;
the existing linked-instance overrides and Apply/Revert controls work on these components.

On the project PlayerDefinition, **Primary Weapon Prefab** and **Secondary Weapon Prefab** choose
the player's two slots. A supplied prefab takes priority over that slot's legacy Animation
Set field. Existing scenes using AK/870 descriptors now reference their corresponding
prefabs. Legacy `.fpsanim` slot references remain readable. The runtime resolves the bundle
and builds camera-bound rigs through the existing presentation path; it doesn't draw the
world-space prefab model as a second view model.

The 870 now has the AK's explicit aim/IK/camera/lean/obstruction/locomotion setup and additive
Weapon Locomotion animator layer, using its own clips. Its shotgun tuning stays separate:
6 shells, 8 pellets, semi-only fire, pump after each shot, and interruptible per-shell reload.
Its own recoil, camera shake, mount, sights, sound events and ADS carry remain authored assets.

## Interop and deployment

`managed/Tartarus.Runtime` is the stable host assembly. It loads the gameplay DLL from streams
in a collectible AssemblyLoadContext, avoiding a file lock on gameplay builds. A failed
candidate keeps the previous assembly. Native frame structs are blittable with 32-bit flags;
entry points validate the ABI version and frame byte size before dereferencing them. Regenerate
both ABI definitions with `tools/generate_script_abi.py` after changing its schema.

Game exports include Managed and the project's script sources/artifacts. The engine prefers
the current project's gameplay assembly in Scripts/bin; bundled sample output is never a fallback. CI explicitly
installs .NET 10. The unit suite covers the real managed/native boundary, player movement,
fire modes, cooldowns, shotgun cycling, shell loading, tap/hold reload, script state across
reloads, failed-reload rollback, pellet spread/capped impulse, background editor builds and
reflected prefab references. `--weapon-test` exercises
the animated AK and 870 in the real Play loop.
For a quick check of just this integration, use `--unit-tests Managed`.

## Research behind the implementation

* [Microsoft native hosting guide](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting)
  and [official GitHub hosting sample](https://github.com/dotnet/samples/tree/main/core/hosting):
  supported hostfxr initialization and unmanaged managed entry points.
* [Microsoft assembly unloadability guide](https://learn.microsoft.com/en-us/dotnet/standard/assembly/unloadability):
  collectible contexts unload cooperatively; the bridge must release script/type references.
* [Flax engine scripting sources](https://github.com/FlaxEngine/FlaxEngine/tree/master/Source/Engine/Scripting):
  reference for a native engine exposing managed gameplay bindings. No Flax code was copied.
* [Unity prefab instance overrides](https://docs.unity3d.com/Manual/PrefabInstanceOverrides.html):
  source assets plus per-instance changes, matching Tartarus's existing prefab machinery.
* [Redmond et al., OOPSLA 2025, Exploring the Theory and Practice of Concurrency in ECS](https://arxiv.org/abs/2508.15264):
  identity, component data and systems are separate concerns. This informed retaining EnTT
  ownership in C++ while exposing behavior through C#; concurrent scripts are not introduced.
