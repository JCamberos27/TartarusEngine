# Tartarus C# scripting API

[Documentation index](README.md) · [Tutorial manual](SCRIPTING_MANUAL.md) ·
[Gameplay frame reference](SCRIPTING_FRAMES.md)

This reference describes the implemented scripting surface. Gameplay and editor extensions
are C#; the editor itself, scene ownership, renderer, animation evaluation, audio mixer and
PhysX remain C++. Use `Tartarus` for gameplay, `Tartarus.Editor` for editor extensions, and
`System.Numerics` for `Vector3` and `Quaternion`. This is a Unity-inspired API, not a
`UnityEngine` compatibility layer. Engine forward is **-Z**, up is +Y, distances are world
units, transform rotations use radians internally and `Transform.Rotate` takes degrees.

Project C# owns player/weapon rules and definitions, vitals/damage, scoring, gravity, NPC
AI/spawning/squads, HUD/developer content, sound policy, locomotion actions and outfit choices.

Physics events call `Script.OnTriggerEnter/Stay/Exit(GameObject)` and
`OnCollisionEnter/Stay/Exit(Collision)` once per completed physics batch, after Start and
before Update. Collision data includes Other, Point, Normal, Impulse and NormalSpeed.
Disabled, inactive and faulted scripts receive no callbacks. Callbacks may mutate the scene;
the receiver snapshot is revalidated before delivery. Render-only ticks do not repeat events.

`GameObject.activeInHierarchy`, `tag` and `SetMaterialEmission(strength, materialSlot)`
provide activation/tag queries and emission on assigned material slots (shared materials
remain shared). `Physics.TryGetBodyState`, `TryGetBodyRotation`, `TryGetActorPosition`
and `AddImpulseAtPosition` expose native body operations. `CarryConstraint.TryCreate`
returns a validated handle with `IsValid`, `SetTarget` and `Release`; stale handles cannot
operate on a subsequent constraint. The current backend supports one simultaneous carry
constraint. `Audio.PlayAtPosition` accepts an optional `pitch`.

`Script.OnReload` is called with scene services after state restoration when managed code
is replaced. Rebuild runtime reference caches there; Awake and Start are not repeated.

## Projects, source folders and compilation

* Gameplay source lives below `project/assets`, excluding folders named `Editor`.
* Editor source lives below `project/assets/Editor` or another `assets/**/Editor` folder.
* `project/Scripts/Tartarus.Gameplay.csproj` builds `Tartarus.Gameplay.dll`.
* `project/Scripts/Tartarus.Editor.csproj` independently builds `Tartarus.Editor.dll`.
* `Directory.Build.props` gives each project a separate intermediate directory.
* `managed/Tartarus.Runtime` contains the stable scripting SDK and managed host.

Use **Create C# Script** or **Create C# Editor Script** in the Asset Browser. Files open in
the [embedded Script IDE](SCRIPT_IDE.md); **Open Externally** remains available in the context
menu. Saves queue a background build after a 600 ms debounce. Gameplay builds
first, editor tools second. A save arriving during compilation queues another build. Output
is in `project/Scripts/bin`, diagnostics in `build.log` and `editor-build.log` in that same
Scripts directory. **File > Build C# Gameplay & Editor** forces a build.

The native editor loads gameplay and editor tools into **separate collectible contexts**.
Gameplay scripts never need an editor assembly reference. Successful builds reload without
locking the DLL. Compile errors keep the previously loaded assembly. Editor reload failures
retain existing tools. A runtime SDK change requires restarting the engine because the SDK
assembly is pinned by the native .NET host; ordinary project source edits reload live.

Install the x64 .NET 10 SDK to compile and the x64 .NET 10 runtime to run exported games.
`TartarusRuntime` builds the engine SDK/host; `TartarusScripts` builds the sample and fixture.
`TARTARUS_BUILD_SAMPLE_GAME=OFF` removes the sample dependency from `TartarusEngine`.
Exports exclude editor source/DLLs and copy the current project's gameplay output into
its Scripts/bin. Gameplay and editor loading never fall back to bundled sample assemblies.

## Attached behaviours

Derive from `MonoBehaviour` or `Script`. Drag a `.cs` asset onto an object or add a C# Script
component and choose **Add Script**. Several scripts, including several of the same type, can
share one object. Every instance has a stable object-local `InstanceId`, separate field
values, and an `enabled` flag. Reordering/removing other slots does not reuse that identity.
`Entity` is the containing object's native generation-tagged ID.

```csharp
using System.Numerics;
using Tartarus;

namespace Tartarus.Gameplay;

public sealed class Lift : MonoBehaviour
{
    [Range(0, 10)] public float Speed = 2;
    [SerializeField, Tooltip("Direction in world space")]
    private Vector3 direction = Vector3.UnitY;

    public override void Update()
    {
        transform.Translate(direction * Speed * Time.deltaTime);
    }
}
```

### Lifecycle and instance state

| Callback | Behavior |
| --- | --- |
| Constructor | Establish defaults only. Avoid engine calls or scene mutations. |
| `Awake()` | Once on first activation. All configured instances exist before callbacks. |
| `OnEnable()` | On activation and subsequent re-enables. |
| `Start()` | Once before the first Update/FixedUpdate. |
| `Update()` | Per simulation frame; `Time.deltaTime` is this callback's scaled dt. |
| `FixedUpdate()` | Before a physics substep. Use for continuous body forces. |
| `LateUpdate()` | After all scripts' ordinary Updates. |
| `OnDisable()` | Deactivation; instance and fields remain alive. |
| `OnDestroy()` | Removal, object destruction or stopping Play, for initialized scripts. |
| `SaveState()` / `LoadState(json)` | Preserve state across successful assembly reload. |

Use `override`; methods with the same name that merely hide the base method will not be
dispatched. The legacy `Update(float dt)`, `FixedUpdate(float dt)` and `OnCreate()` remain
supported. Default `Awake` calls `OnCreate`. All active behaviours receive their initial
Awake/OnEnable before the first simulation callback. Exact ordering between independent
objects is unspecified. New objects/scripts created in callbacks begin at the next scripting
tick. Enabled/active changes suppress later callbacks as the native scheduler observes them;
the corresponding lifecycle transition is completed at a subsequent scripting tick.

Disabling preserves state; destroying/removing releases it. Stopping Play restores the
authored scene snapshot. Play edits are not implicitly saved. A lifecycle exception is logged
to the Console and faults that instance until source reload or a field/class change.

Default reload state consists of supported serialized fields, including private
`[SerializeField, HideInInspector]` state. Override both SaveState and LoadState for custom
state. Constructors run for replacement instances, while Awake/Start are not repeated after
successful hot reload. Do not retain SDK references to old script Types or instances in
static caches: those keep collectible assemblies alive.

### Inspector fields

Public writable instance fields and private `[SerializeField]` fields are saved. Supported
types: `float`, `int`, `bool`, `string`, `Vector3`, `AssetReference`, enums whose underlying type is `int`, and
`Dictionary<string, T>` with `T` one of `float`, `int`, `bool`, `string` (edited as a key/value
list). Static, readonly and `[NonSerialized]` fields are excluded. Properties, arrays, object
references, arbitrary structs and other collections are not Inspector field types.
Unsupported types are visibly labeled; they remain usable as ordinary runtime C# state.

* `[Range(min,max)]` renders an integer/float slider.
* `[AssetPath(".fpsanim")]` gives an `AssetReference` a filtered picker with asset drops; it stores `path` and `pathGuid`.
* `[Color]` renders a `Vector3` as a linear RGB color picker.
* `[Tooltip("text")]` supplies Inspector help.
* `[HideInInspector]` retains serialized data without showing a widget.
* `[Header("Movement")]` groups that field and subsequent fields in the Inspector.
* On strings, `[SceneReference("Transform", true)]` and `[SceneReference("Particle System", true)]`
  provide hierarchy pickers/drag targets. Values are relative hierarchy paths; `true` restricts
  selection to the owner's subtree. Renaming/reparenting targets can invalidate these paths.
* On strings, `[SoundReferences]` provides audio asset slots serialized as semicolon-separated
  project paths; `[InspectorChoices("", "ak", "870")]` provides a fixed string dropdown.
* `[Foldout("Name")]` puts consecutive fields with the same name in one collapsible section.
* `[Tab("Name")]` puts fields (and `[Button]` methods) under one tab of a tab bar.
* `[ReadOnly]` shows the field greyed out; it is still saved.
* `[HideIf(nameof(Other), value)]` / `[DisableIf(nameof(Other), value)]` hide or grey out the
  field while `Other` equals `value` (default `true`).
* `[Variants(1f, 5f, 10f)]` adds one-click value chips under the field.
* `[OnValueChanged(nameof(Method))]` calls a parameterless method after the field is edited in
  the Inspector; anything the method changes in saved fields is kept.
* `[Button]` / `[Button("Label")]` on a parameterless method adds an Inspector button (one undo
  step). Outside Play it runs on a temporary instance holding the saved field values, and the
  changed values are saved back; in Play it runs on the live instance.
* `[ShowInInspector]` on a property or a non-saved field shows its current value read-only,
  refreshed a few times a second.
* Deleted/renamed fields are discarded; new fields use constructor/initializer defaults.
* Editing one field during Play updates only changed overrides, preserving unrelated state.
* Advanced > Reset Fields removes authored overrides. Class accepts a qualified type name.
  Once a class is chosen, the Script and Source rows also move under Advanced.

Fields, enabled flags and script slots participate in scenes, undo, clipboard, prefabs and
prefab overrides. Field names form the serialization contract: renaming is a schema change.
Do not declare two serialized fields with the same name in a class hierarchy.

## Component and object API

`Component` provides `Entity`, `gameObject`, `transform`, `GetComponent<T>()` and
`GetComponents<T>()`. Attached scripts derive from Component. Native wrappers do not own
native pointers; they resolve the object's ID on each operation.

`GetComponent<T>` returns the first matching component or null. `GetComponents<T>` returns
all matches, including disabled scripts. Missing native components return an empty array.
Native wrappers currently include `Transform`, `Rigidbody`, `Animator`, `Camera`, `Light`,
`Collider` and `AudioSource`. Other native data
is exposed through the catalog and `NativeComponent` API below.

### GameObject

| Member | Meaning |
| --- | --- |
| `new GameObject(uint id)` / `Id` | Value handle to an existing object, not a constructor that creates it. |
| `Invalid` / `IsValid` | Sentinel and generation-aware existence check. |
| `name` | Read/write native name. |
| `transform` | Transform view; a missing/deleted transform throws on use. |
| `activeSelf` / `SetActive(bool)` | Local active flag. Parent inactivity also suppresses child scripts. |
| `parent` | Nullable parent handle. Setter preserves world pose and rejects hierarchy cycles. |
| `children` | Snapshot of valid immediate children. |
| `Instantiate(prefab, position)` | Instantiate a project-relative prefab and return its root. |
| `Destroy(object)` | Destroy the object and its subtree. Check IsValid before later use. |
| `GetComponent<T>()` / `GetComponents<T>()` | Typed native or attached-script lookup. |
| `AddScript<T>()` | Queue an attachable Script and return its new slot ID; construction is next tick. |
| `RemoveScript(script)` | Remove a script belonging to this object. |
| `nativeComponents` | Names of this object's reflected native components. |
| `HasNativeComponent(name)` | Test reflected native component presence. |
| `GetNativeComponent(name)` | Read/write data view; missing component throws. |
| `AddNativeComponent(name)` / `AddComponent<T>()` | Add authored native data in Edit mode. |
| `RemoveNativeComponent(name)` | Remove authored native data in Edit mode. |

IDs are not durable scene references. Undo, scene load, Play/Stop and object destruction can
invalidate them. Reacquire objects after scene restoration. Use `Scene.Find` for an exact
name match; duplicate names make the first match unspecified.

### Scene

* `GetObjects()` returns a snapshot of objects with transforms.
* `Find(string)` returns an object or null.
* `Create(string name, Vector3 position = default)` creates an empty object.
* `FindObjectsOfType<T>()` scans typed components and scripts across objects.
* `FindObjectsWithComponent(string)` scans reflected native component names.

These are scene scans. Cache lookups for frequently used components, and validate/reacquire
those caches when objects or scenes change. Scene APIs work during attached-script callbacks
and editor-tool callbacks, when the native host has bound the current world.

### Transform

`position` is world position. `localPosition`, `localScale`, and `localRotation` are local
to the parent. `localRotation` is a normalized System.Numerics quaternion. World position
writes convert through the parent matrix; a singular parent transform rejects the operation.
`TransformPoint` and `InverseTransformPoint` convert points including scale and translation.
`forward`, `right`, and `up` are normalized world axes (forward is -Z).
`Translate(Vector3)` adds a **world** displacement. `Rotate(localAxis, degrees)` appends a
local axis-angle rotation. Nonuniform parent scale can distort an axis before normalization.

Physics owns active dynamic-body poses. Set body velocity/forces during Play instead of
expecting arbitrary transform edits to override simulated motion permanently.

### Rigidbody and Animator

`Rigidbody.velocity` gets/sets active native body linear velocity. `mass` reads authored mass.
`AddForce(force, mode)` supports `Force`, `Impulse`, `VelocityChange`, and `Acceleration`.
`AddImpulseAtPosition(impulse, point)` applies an impulse at a world point. Velocity reads
throw when there is no active dynamic body. Kinematic bodies reject dynamic force writes.
Author collider, mass and body configuration before entering Play.

`Animator` provides `SetFloat`, `GetFloat`, `SetInteger`, `GetInteger`, `SetBool`, `GetBool`,
`SetTrigger` and `ResetTrigger`. `CurrentState`, `InState`, `HasTag`, and `EventFired` query
the native controller's last evaluation. Queries apply to its base layer. Events are transient
and cleared by the next animation update. Native parameters store integer values as floats.

### Typed data views

* `Camera`: `fieldOfView` (vertical degrees), `nearClipPlane`, `farClipPlane`.
* `Light`: `type` (`Point`, `Spot`, `Directional`), `color` (Vector3 RGB), `intensity`, `range`.
* `Collider`: `halfExtents` and its complete reflected `data` view.
* `AudioSource`: `clip`, `volume`, `loop`, `playOnStart` and `data`. Use Audio for explicit voices.
* Project `Tartarus.Gameplay.PlayerDefinition` is a script with authored movement, view,
  health, ability and weapon-slot fields. It replaces the native First Person Controller
  component and its former SDK wrapper.

Camera, Light and other ReflectedComponents all expose `NativeType` and `data`. These modify
native **authored data**, not every subsystem's already-created resources. Changing a body
mass/collider shape during Play does not rebuild PhysX. Changing the first-person descriptor
does not automatically reconstruct an equipped rig. Light color temperature can determine
rendered color independently of the RGB field. Read tooltips in the catalog for these rules.

## Complete native data catalog

`ComponentCatalog.GetAll()` returns every registered native component and its field metadata.
This includes player tuning, render/lighting settings, colliders and
joints, animation, particles, audio, reverb, foley, ragdoll settings and gameplay components.
It automatically expands when native components are registered; it is not a manually maintained
list of C# aliases. Mesh Renderer exposes no pointer-backed Model through this API.

`NativeComponentInfo` contains Name, Category and Fields. Each `NativeFieldInfo` contains Key,
Label, Kind, Min, Max, Tooltip and EnumLabels. Kind values follow the native reflection enum:
0 bool, 1 int, 2 float, 3 Vector3, 4 string, 5 RGB Vector3, 6 enum index, 7 asset-path string.
Native enum data is an **integer index**, not a C# enum name. Prefer stable Key over display Label.

```csharp
var definition = gameObject.GetComponent<Tartarus.Gameplay.WeaponDefinition>();
if (definition != null) Debug.Log(definition.Description);

foreach (var component in ComponentCatalog.GetAll())
    foreach (var field in component.Fields)
        Debug.Log($"{component.Name}.{field.Key}: {field.Tooltip}");
```

`Tartarus.Gameplay.WeaponDefinition` is a project C# script in `assets/Scripts/WeaponDefinition.cs`,
not a native component or Runtime SDK wrapper. Its `Description`, `AnimationSet` and muzzle
settings use ordinary script serialization and the project's custom Inspector. Native weapon
presentation resolves its C# defaults/overrides and consumes an effect snapshot. Older native
Weapon Definition data converts to a script on load; saving writes the C# representation.

`Get<T>(field)` deserializes the native value; use the exact corresponding type.
`Set<T>(field,value)` validates type, finite numeric data, enum index and native field bounds.
Vector3 uses X/Y/Z. Native authoring limits clamp scalar/vector edits when Min < Max.
Unknown component/field, missing object, wrong value type and rejected mutation produce an
InvalidOperationException; invalid native input is logged without unwinding across the ABI.

Structural native additions/removals are rejected during active physics simulation. Mesh
Renderer and the C# Script data blob reject generic structural mutation. Use prefab
instantiation for runtime models and `AddScript`/`RemoveScript` for behaviours. Generic writes
to C# Script internals are prohibited so callers cannot corrupt slot identities.

## Physics, input, audio and time

### Physics queries

`Physics.IsActive` reports whether the native physics scene exists.
`Raycast(origin,direction,out hit,distance=1000,layerMask=uint.MaxValue,hitTriggers=false)`
and `SphereCast(origin,radius,direction,out hit,...)` normalize direction. Zero direction or
invalid distance rejects the cast. `RaycastHit` contains Object, Point, Normal and Distance.
Mask bit N selects layer N. Trigger colliders are ignored by default.

`OverlapSphere(center,radius,maxResults=128,layerMask=...,hitTriggers=false)` returns native
collider object handles, up to the cap. Caps must be 1..4096. Results are snapshots and ordering
is unspecified. No active scene produces no hits. These queries do not create physics actors.

```csharp
if (Physics.Raycast(transform.position, transform.forward, out var hit, 50))
    hit.Object.GetComponent<Rigidbody>()?.AddImpulseAtPosition(transform.forward * 5, hit.Point);
```

`PlayerCharacter` exposes the engine's **single existing player capsule**, not an arbitrary
per-object character controller: Exists, Create(radius,cylinderHalfHeight,feet), Feet,
Resize(height), Fits(height), Move(displacement,dt) and PlatformYawDelta. Move returns the
native contact flags: sides=1, ceiling=2, ground=4. Feet are world-space foot positions.
The supplied PlayerController uses this API for movement, crouching and platform yaw.

### Input

`Input.GetAxis(action)`, `GetButton(action)`, `GetButtonDown(action)` and `GetButtonUp(action)`
read names configured in Project Settings > Input. They are action names, not raw key codes.
Down/Up are frame transitions. Use ordinary Update for one-shot actions and retain desired
state for FixedUpdate if a simulation tick must consume it.

### Audio

`Audio.Play(path,volume=1,loop=false,bus=SFX)` starts a 2D voice.
`PlayAtPosition(path,position,...,minDistance=1,maxDistance=40)` starts a spatial voice.
Paths are project-relative. AudioBus values are SFX, Music, Ambient, UI and Voice.
Failure to load/play throws. The returned `AudioVoice` exposes Id, IsPlaying, Stop,
SetVolume and SetPosition. Finished handles are generation-tagged and safe to stop again.
Looping script voices are stopped when Play exits; editor previews stop at editor shutdown.
Move a voice explicitly to follow an object. The explicit Audio helpers use their supplied
settings; they do not automatically inherit every AudioSource/reverb/weapon-audio field.

### Time and utility services

`Time.deltaTime` and `fixedDeltaTime` are supplied by script dispatch. `GetSnapshot()` copies
the native time, unscaled time, realtime, frame count, unscaled frame dt and time scale in one
call. `time`, `unscaledTime`, `realtimeSinceStartup`, `unscaledDeltaTime`, and `frameCount`
are convenience accessors. Totals preserve double precision. Unit-test clocks may remain zero
because a headless test does not drive the full main loop.

`Application.isPlaying` reflects an active physics scene. `ResolveProjectPath(relative)`
resolves against the current project. `Debug.Log(object)` writes to the native Console.
`Debug.Assert` throws when a condition fails.

`Mathf` provides PI, Deg2Rad, Rad2Deg, Clamp, Clamp01, Lerp, LerpUnclamped, InverseLerp,
MoveTowards, Repeat, DeltaAngle, LerpAngle, Approximately and exponential Damp.
`RandomSource(seed)` offers deterministic Value and integer/float Range without changing
global random state. Prefer System.Numerics for vector/quaternion arithmetic.

## Player and weapon gameplay contracts

`project/assets/Scripts/Gameplay.cs` implements the optional generic `IProjectIntegration` and delegates to
`PlayerController.cs` and `WeaponController.cs`. This is the active first-person gameplay
path. Those controllers are invoked through data frames rather than scene MonoBehaviours:
the player camera/capsule and equipped view models have existing native lifetimes.
Attaching a second movement behaviour is not required to activate the supplied player.

Project `Gameplay.Player(ref PlayerFrame)` owns movement, acceleration, sprint/crouch, jump buffer,
coyote time, gravity, root-motion blending and respawn logic. The native adapter collects
input/camera deltas and writes resulting position/velocity/camera values back.

Project `Gameplay.Weapon(ref WeaponFrame)` owns fire eligibility, ammunition/chamber state, fire
modes, burst scheduling, cooldown, pump timing, reload interruption, shell/magazine events,
reload tap/hold and idle fidget decisions. Native presentation submits animator tags/events
and descriptor values, then executes returned commands for recoil, audio and animation.

Project `Gameplay.Shot(ref ShotFrame)` owns pellet distribution, ray queries and capped impact
impulse rules. Native services draw decals/traces, run physics, and present impact audio.
AK and 870 share this implementation with different prefab/descriptor settings and clips.

WeaponOperation: RequestFire=1, CommitShot=2, CheckTrigger=3, TriggerResult=4, RequestReload=5,
ToggleFireMode=6, Tick=7, AnimationEvents=8, ReloadButton=9. Use these enum values when
editing the numeric frame dispatch. WeaponTags and WeaponCommands are flag enums matching
the generated ABI; HasTag/HasCommand extension methods test them. WeaponEvents describes
MagazineLoaded and ShellLoaded animation events. Flags in the ABI use 32-bit ints, not bool.

PlayerFrame contains dt, look/input, configuration and mutable movement state. WeaponFrame
contains the request operation, descriptor tuning, animator tags/events and mutable weapon
state; Commands/Result are outputs. ShotFrame contains origin/direction, spread, range,
pellet count, random seed and impact/decal values. These FPS types are project contracts, outside the engine SDK. See project GameFrames.cs for exact
member names. Add project layout fields to `tools/generate_game_frames.py`, regenerate both
languages, bump the ABI version and rebuild native plus managed code together.

## C# editor scripts

Derive from `Tartarus.Editor.EditorWindow` in an Editor folder. Native C++ owns its dockable
ImGui window. The **C# Tools** panel lists discovered windows and `[MenuItem("path")]`
commands. Commands are static void methods without parameters, exposed as buttons in that
panel. Paths label/group their intent; they do not replace the native main menu.

```csharp
using System.Numerics;
using Tartarus;
using Tartarus.Editor;

namespace Tartarus.EditorTools;

public sealed class PlaceMarker : EditorWindow
{
    private Vector3 position;
    public override string Title => "Place Marker";

    [MenuItem("Scene/Place Marker")]
    public static void OpenTool() => GetWindow<PlaceMarker>();

    public override void OnGUI()
    {
        EditorGUILayout.Vector3Field("Position", ref position);
        if (EditorGUILayout.Button("Create marker")) {
            Undo.RecordScene("Place marker");
            Selection.activeGameObject = Scene.Create("Marker", position);
        }
    }
}
```

EditorWindow members: Title, GetWindow<T>(), Close, OnEnable, OnDisable, Update(unscaledDt),
OnGUI, OnSelectionChanged, OnPlayModeChanged, SaveState and LoadState. Tool instances are
created/enabled when the assembly loads; Update runs even while their windows are closed.
OnDisable runs when replaced/unloaded, not on every panel close. OnGUI runs only for visible
open windows. Selection/play-mode changes are observed on the next editor draw. Override
SaveState/LoadState to retain custom tool state across reload; default state is `{}`.

`Selection.activeGameObject` is a nullable read/write selection. `Undo.RecordScene(label)`
takes a native scene/asset snapshot **before** a complete command changes anything. Record
one snapshot per deliberate command, not one every frame. `EditorUtility.SetDirty()` marks
unrecorded authored changes dirty. `RequestScriptCompilation()` queues compilation.
EditorUtility.IsPlaying allows a tool to decide whether to edit authored data during Play.
Play changes still revert on Stop.

`EditorGUILayout` exposes Label, Button, Toggle, FloatField, IntField, Vector3Field, Slider,
TextField and Separator. Editable fields take a `ref` value and return whether it changed.
Native widget IDs are scoped per window and widget order, so repeated labels do not conflict.
Keep widget ordering stable during a drag. GUI calls belong in OnGUI (or a menu command drawn
inside C# Tools), not constructors/Update/background jobs. Window Begin/End is managed by the
host and balanced even when OnGUI throws. Failed windows are logged and stop drawing until
the next source reload; C# Tools displays the failure notice.

The provided `assets/Editor/SceneTools.cs` demonstrates selection, undo, object creation and
catalog inspection. C# editor scripts extend the C++ editor; they do not replace it, execute
as game behaviours, or ship in game exports.

## Threading, validation and boundaries

All engine calls belong to the main thread inside a bound gameplay/editor callback. Managed
background tasks can compute plain data, then consume it on the next main-thread callback.
Calling the engine from a worker thread throws. Avoid async void lifecycle/GUI callbacks;
their continuation does not carry the engine callback context.

`Engine.Call`, `TextCall`, NativeRequest and generated frames are the lower-level interop
surface used by the supplied controllers. Prefer typed wrappers for user scripts. UTF-8
native replies are copied immediately and must not be retained as unmanaged pointers.
Native exceptions are caught before returning across the ABI. A GameObject/Component wrapper
is a value/ID view, not an ownership guarantee.

This API does not currently implement Unity packages, coroutines,
scriptable assets, typed serialized entity-reference fields, collision callbacks on behaviours, runtime
native-actor creation, or a managed debugger. The native systems can continue performing
those responsibilities through their existing editor/runtime interfaces. The implemented
catalog gives access to registered authored data without claiming every native subsystem has
a corresponding managed lifecycle.

Custom Inspector classes are implemented; see the section below. String-backed scene-reference
pickers are implemented, with the hierarchy-path limitations described above.

## Focused verification

Run `TartarusEngine.exe --unit-tests Managed` for short integration checks. These cover the
real C#/native ABI, player movement, AK/870 fire/reload decisions, ballistic queries, state
reload, multiple script instances, native data catalog/type views, hierarchy/world transforms,
background gameplay/editor builds and undo/selection/window command dispatch. They use a fake
widget host for editor commands; visual docking and actual audio playback require an editor
session. The long animated weapon playtest is separate and is not required on every script edit.

## Custom Inspector and global undo API

See [EDITOR_HISTORY_AND_INSPECTORS.md](EDITOR_HISTORY_AND_INSPECTORS.md) for CustomEditor, Editor, SerializedObject, SerializedProperty, Header, the additional EditorGUILayout widgets, and Undo.RecordObject / RecordAsset / PerformUndo / PerformRedo.

EditorUtility.OpenScript(path, line, column) opens the native Script IDE at a one-based source position. An empty path shows the window. See [SCRIPT_IDE.md](SCRIPT_IDE.md).

## Scoped runtime overlays

`RuntimeCanvas.current` is available during a supplied drawing scope. It provides Width,
Height, Scale, Measure, LineHeight, Text, Rect and Line with System.Numerics colors/vectors.
`RuntimeGui.current` is available during a supplied immediate UI scope: BeginWindow/EndWindow,
Separator, Checkbox, Slider, Button, SameLine and Label. Widgets return their edited values;
EndWindow is required even when BeginWindow returns false. Native scopes close remaining
windows on failure. Handles expire when the callback returns and reject later use. These APIs
are separate from Tartarus.Editor; project content chooses labels, layout and actions.

`Input.GetKeyDown(int key)` reads a raw GLFW key transition (0..348); named Input actions
remain the normal gameplay interface. AssetReference.ResolvePath follows a GUID and returns
an absolute path in the current project.
