# Gameplay frame and interop reference

[Documentation index](README.md) · [Scripting manual](SCRIPTING_MANUAL.md) ·
[Public API reference](SCRIPTING_API.md)

This document describes every member of the generated scripting structs and the meaning of
the weapon operation/flag contracts. They are defined by `tools/generate_script_abi.py` and
generated into `managed/Tartarus.Runtime/ScriptAbi.cs` and `src/Game/Scripting/ScriptAbi.h`.
The current ABI version is **2**. These structs use sequential layout with 32-bit int flags,
32-bit float values and native pointer-sized text addresses.

Frames are mutable per-call values, not independent scene components. The native adapter
populates input/configuration and prior state, invokes managed gameplay, and consumes outputs.
Some fields are carried for configuration/compatibility even when a particular managed path
does not use them directly. Changing a frame value does not automatically rewrite its authored
descriptor or scene component. Inspect native write-back when adding a new persistent result.

## Ownership and ABI changes

* Player: `Player::Update` prepares PlayerFrame and writes movement/camera results back.
* Weapons: `FirstPersonPresentation::RunGameplay` prepares WeaponFrame and executes commands.
* Shots: native presentation prepares ShotFrame; C# queries physics and emits trace/impact work.
* Attached behaviours: native scripting Tick sends EntityFrame lifecycle phases.
* Services: NativeRequest carries typed/UTF-8 arguments across the callback boundary.

When changing layout, edit the generator, bump the version, regenerate both definitions, build
native and managed code together, and restart. Reordering fields changes offsets. Do not substitute
C# bool for int or managed strings for text pointers. Version and frame-byte-size checks guard
dispatch; they cannot make mismatched semantics compatible. Use the public wrappers for ordinary
scripts rather than constructing internal lifecycle frames.

## PlayerFrame

Gravity and velocity are signed world-space values. Position is the camera/eye position;
RespawnFeet and PlayerCharacter positions are foot positions. Yaw/Pitch and look deltas are
degrees. Dt and timers are seconds. Boolean-like int fields use zero=false and nonzero=true.

| Member | Type | Meaning |
| --- | --- | --- |
| Dt | float | Scaled elapsed time for this player update, clamped nonnegative by the native adapter. |
| Yaw | float | Current/result camera heading in degrees. |
| Pitch | float | Current/result camera pitch in degrees. |
| MoveX | float | Lateral movement input; normalized planar input is written back. |
| MoveY | float | Forward movement input; normalized planar input is written back. |
| LookPitch | float | Native-prepared combined mouse/stick pitch delta. |
| LookYaw | float | Native-prepared combined mouse/stick yaw delta. |
| SizeX | float | Player width used to derive capsule radius. |
| SizeY | float | Standing height used to derive cylinder half-height. |
| EyeHeight | float | Standing eye offset above the feet. |
| MoveSpeed | float | Base planar movement speed. |
| SprintMultiplier | float | Speed multiplier while sprint is eligible. |
| JumpSpeed | float | Upward velocity assigned when a jump starts. |
| Gravity | float | Signed vertical acceleration, normally negative. |
| MouseSensitivity | float | Configuration carried by the adapter; native input preparation uses it. |
| StickLookDegPerSec | float | Configuration carried by the adapter; native stick look preparation uses it. |
| KillY | float | Eye-position threshold below which the player respawns. |
| RootMotionWeight | float | Blend weight between wish velocity and planar root-motion velocity; current algorithm clamps 0..1. |
| MaxYawRate | float | Optional yaw reach limiter in degrees per second. |
| YawFreeCenter | float | Heading center of the free-look/yaw reach region. |
| YawFreeRange | float | Allowed free yaw range in degrees. |
| CrouchHeight | float | Crouched total height; nonpositive disables the crouch-height behavior. |
| CrouchSpeedMultiplier | float | Planar speed multiplier while crouched. |
| JumpBufferTime | float | Duration a pressed jump can remain buffered. |
| CoyoteTime | float | Allowed time since grounded for a buffered jump to begin. |
| GroundAccelTime | float | Exponential response time while increasing grounded speed. |
| GroundDecelTime | float | Exponential response time while reducing grounded speed. |
| AirAccelTime | float | Exponential response time for planar airborne movement changes. |
| SinceGrounded | float | Mutable elapsed time since last grounded update. |
| JumpBuffer | float | Mutable remaining jump-buffer duration. |
| CrouchBlend | float | Mutable smoothed crouch blend used to calculate eye height. |
| YawDropped | float | Output amount of requested yaw discarded by the reach limiter. |
| Position | Vector3 | Mutable eye/camera world position. |
| Velocity | Vector3 | Mutable world movement velocity. |
| RespawnFeet | Vector3 | World foot-position spawn target. |
| RootMotionVelocity | Vector3 | Native animation-provided root-motion velocity; planar components affect movement. |
| WishVelocity | Vector3 | Output desired planar input velocity after speed modifiers. |
| ReadInput | int | Whether look/jump input is active for this update. |
| Sprint | int | Requested sprint state; aiming/crouch can suppress it. |
| JumpDown | int | One-frame jump press supplied by native input. |
| Crouch | int | Requested crouch state. |
| AimHeld | int | Whether aiming is held; current movement suppresses sprint while aiming. |
| Grounded | int | Prior/result grounded state. |
| Crouched | int | Prior/result crouched capsule state. |
| Jumped | int | Output flag that a jump began during this update. |

### Current movement sequence

1. Apply look deltas and optional yaw reach limiting.
2. Build and normalize planar wish direction from heading and input.
3. Fit/resize the crouch capsule when applicable and smooth the eye offset.
4. Apply sprint/crouch speed modifiers, blend root motion, and approach target velocity.
5. Advance jump buffer/coyote timing and begin an eligible jump if there is standing clearance.
6. Apply gravity; create/move the native player capsule when physics is active.
7. Reconcile collision-limited planar velocity, grounding, ceiling impacts and platform yaw.
8. Update eye position from capsule feet and handle falling below KillY.

Without an active physics scene, the controller integrates position directly and reports no
grounding. That fallback supports headless checks; it is not a replacement for level collision.
The player capsule is a singleton native service, not a Rigidbody attached to each behaviour.

## WeaponFrame

Float timers are seconds, Rpm is rounds per minute, and Random is a native-generated sample
used for idle-fidget scheduling. Ammo/Magazine are integer counts. State fields persist through
the native presenter's write-back; Commands and Result describe the current operation.

| Member | Type | Meaning |
| --- | --- | --- |
| Dt | float | Elapsed time for tick or reload-button timing. |
| Rpm | float | Descriptor firing cadence; successful trigger processing uses 60/max(1,Rpm). |
| CycleDelay | float | Delay before requesting a pump/cycle after a committed shot. |
| HoldSeconds | float | Reload-button hold threshold for magazine check. |
| RegripMin | float | Lower bound for randomized idle-fidget delay. |
| RegripMax | float | Upper bound for randomized idle-fidget delay. |
| Cooldown | float | Mutable remaining firing cooldown. |
| CycleWait | float | Mutable time until cycle may be requested. |
| IdleTime | float | Mutable idle duration since last fidget/active interruption. |
| RegripDelay | float | Mutable target idle duration for the next fidget. |
| SinceShot | float | Mutable duration since the last committed shot. |
| SinceUnhidden | float | Mutable duration outside Hidden; resets while hidden. |
| ReloadHeldSeconds | float | Mutable elapsed time holding reload. |
| Random | float | Input random sample for regrip delay selection; current code clamps 0..1. |
| Operation | int | WeaponOperation request number, defined below. |
| Ammo | int | Mutable loaded ammunition count. |
| Magazine | int | Descriptor maximum loaded count. |
| BurstRounds | int | Requested burst length when semi-style input starts a configured burst. |
| AllowFullAuto | int | Whether switching fire mode is allowed. |
| PerRound | int | Descriptor uses per-shell/per-round reload. |
| CycleAfterShot | int | A committed shot empties the chamber and requires a cycle. |
| HipProcedural | int | Procedural shot commit is allowed from applicable hip idle/ready states. |
| RecoilProfile | int | A referenced recoil profile selects the corresponding procedural fire path. |
| UnityRecoil | int | Native procedural recoil mode flag used by trigger continuation logic. |
| Equipped | int | Mutable equipped state supplied by presentation. |
| Chambered | int | Mutable chamber-ready flag. |
| CycleSeen | int | Mutable flag that Cycling was observed after the committed shot. |
| StopReload | int | Mutable request/state for interrupting per-round reload. |
| FullAuto | int | Mutable current full-auto versus semi fire mode. |
| BurstRemaining | int | Mutable rounds remaining in a requested burst. |
| WaitingShot | int | Mutable flag that an animation-triggered shot commit is pending. |
| Tags | int | Bitmask of native animator state tags, defined below. |
| InTransition | int | Native animator is currently transitioning; used by idle-fidget eligibility. |
| WallBlocked | int | Native obstruction system prevents firing. |
| Pressed | int | Operation-specific press/result input; not always a raw fire-button edge. |
| Held | int | Operation-specific held button state. |
| ReloadWasDown | int | Mutable previous reload-button state. |
| ReloadHoldFired | int | Mutable flag that the held magazine-check action has fired. |
| Events | int | Animation-event bitmask for loaded magazine/shell. |
| Commands | int | Output command bitmask for the native presentation adapter. |
| Result | int | Output decision, interpreted according to the operation. |

### Operation values and results

| Value | WeaponOperation | Main result/command behavior |
| --- | --- | --- |
| 1 | RequestFire | Reject hidden/busy/unloaded/unchambered/blocked states; result 1 requests a procedural commit or animated fire trigger. |
| 2 | CommitShot | Consume one round, reset shot/idle timers and begin chamber/cycle state if configured. |
| 3 | CheckTrigger | Result indicates whether current pressed/held/burst state wants a firing attempt after cooldown. |
| 4 | TriggerResult | Set cadence, advance burst count and return whether firing should continue. Pressed carries the caller's firing result here. |
| 5 | RequestReload | Result 1 and ReloadTrigger command when equipped, not already reloading and below capacity. |
| 6 | ToggleFireMode | Toggle FullAuto only when equipped and allowed; result 1 indicates the mode changed. |
| 7 | Tick | Advance cooldown, hidden/shot timing, cycle/chamber observation and idle-fidget decisions. |
| 8 | AnimationEvents | Commit a magazine or shell using Events; does not itself play the reload animation. |
| 9 | ReloadButton | Track hold duration; release of a short press sets Result 1 for reload, hold emits MagazineCheck once. |

Result is not a universal success flag. In particular, TriggerResult uses it for continuation,
CheckTrigger for eligibility, and ReloadButton for the tap decision. Commands are cleared at
the beginning of every WeaponController.Update call, so consume each operation's commands
before submitting another request. Do not replay a command merely because native state persists.

### Animator tag bits

| Bit | WeaponTags | Meaning to gameplay |
| --- | --- | --- |
| 1 | Hidden | Weapon is hidden/unequipped in the animator state contract. |
| 2 | Reloading | Reload operation is active. |
| 4 | Busy | Other state prevents ordinary firing. |
| 8 | Cycling | Pump/cycle state is currently active. |
| 16 | Ads | Aimed state is active. |
| 32 | Idle | Hip idle state is active. |
| 64 | Ready | Hip ready state is active. |
| 128 | IkOff | Procedural IK/fire path is suppressed by the current state. |

The native presenter derives these bits from the authored animator tags; the flags do not
create or modify animator tags. HasTag on WeaponFrame tests whether **any** bits in the mask
are present. When requiring all bits, compare `(frame.Tags & mask) == mask` explicitly.

### Native command bits

| Bit | WeaponCommands | Action requested of presentation |
| --- | --- | --- |
| 1 | DryFire | Dry-fire presentation. |
| 2 | CommitShot | Execute the actual shot commit path. |
| 4 | FireTrigger | Trigger animated firing. |
| 8 | EndBurst | Reset/end native burst presentation state. |
| 16 | Cycle | Request pump/cycle presentation. |
| 32 | Fidget | Request idle regrip/fidget presentation. |
| 64 | ReloadTrigger | Trigger reload presentation. |
| 128 | MagazineCheck | Trigger held reload-button magazine check. |

HasCommand has the same any-bit semantics as HasTag. WeaponEvents uses MagazineLoaded=1 and
ShellLoaded=2. A magazine event sets Ammo to capacity; a shell event increments up to capacity
and can restore chamber state when initially empty. Author events once at the intended time.

### Shotgun state example

After CommitShot, Ammo decrements, Chambered becomes false, CycleSeen becomes false and
CycleWait receives CycleDelay. Tick eventually requests Cycle. Observing a Cycling tag sets
CycleSeen; after Cycling ends, Tick restores Chambered. This protects the 870 from firing
again merely because the cadence cooldown expired while the pump animation was unfinished.

PerRound firing during Reloading requests reload interruption when ammunition is available.
The animation/presentation contract must complete that interruption and return to a fireable
state. Changing C# eligibility without corresponding graph tags/events can leave these systems
disagreeing about whether a shell is loaded or the weapon is ready.

## ShotFrame

| Member | Type | Meaning |
| --- | --- | --- |
| Origin | Vector3 | World-space ray origin. |
| Direction | Vector3 | Intended shot direction; presenter supplies a usable direction. |
| Spread | float | Pellet spread angle in degrees used by the current sampling routine. |
| ImpactImpulse | float | Requested total impulse budget distributed across pellets. |
| ImpactMaxSpeed | float | Optional cap converted to mass-scaled impulse; positive values enable it. |
| BulletHoleRadius | float | Radius forwarded to native trace/decal presentation. |
| Range | float | Ray distance for every pellet. |
| Pellets | int | Number of rays; current controller uses at least one. |
| RandomSeed | int | Seed for repeatable pellet-direction samples for this shot. |

The current sampling chooses a radius proportional to sqrt(random) on a spread disk and an
azimuth around the aim direction, then normalizes the resulting ray. Each pellet either emits
a hit trace with point/normal/entity or a miss trace ending at Origin + direction*Range.
Physical impact applies only to a suitable dynamic body. The capped impulse is divided by
pellet count so increasing pellets does not multiply total requested impact force.

Native service operation 29 consumes the emitted trace while InvokeShot has bound the shot
callback. Ordinary attached scripts should use Physics.Raycast and the public body API rather
than calling that internal shot-trace operation directly.

## EntityFrame: internal behaviour dispatch

| Member | Type | Meaning |
| --- | --- | --- |
| Dt | float | Callback dt; fixed phase also updates Time.fixedDeltaTime. |
| Entity | uint | Generation-tagged native owner ID. |
| Script | uint | Stable object-local script slot ID. |
| Phase | int | Lifecycle dispatch phase listed below. |
| Enabled | int | Slot's own enabled flag, separate from inherited object inactivity. |
| ClassName | nint / UTF-8 pointer | Qualified compiled script type; valid for the dispatch call. |
| Fields | nint / UTF-8 pointer | JSON field overrides; valid for the dispatch call. |

| Phase | Internal operation |
| --- | --- |
| 0 | Ensure initial Awake/OnEnable on an active instance. |
| 1 | Ensure Start and run Update. |
| 2 | Ensure Start and run FixedUpdate. |
| 3 | Remove and destroy the managed instance. |
| 4 | Disable callbacks without discarding the instance. |
| 5 | Construct/refresh configuration before any initial Awake/Start callbacks. |
| 6 | Run LateUpdate after ordinary Updates. |

The managed instance key combines Script and Entity into a 64-bit identity. Removing a slot
destroys that instance; IDs are not reused within the same native script component. Enabled
does not become false merely because an ancestor object is inactive. Native scheduling uses
inherited activity separately.

## NativeRequest: service payload

| Member | Type | Meaning |
| --- | --- | --- |
| A | Vector3 | Operation-specific vector argument/result, often position or point. |
| B | Vector3 | Operation-specific vector argument/result, often direction or normal. |
| C | Vector3 | Third vector argument/result, sometimes query radius or shot trace normal. |
| Value | float | Operation-specific scalar argument/result. |
| Entity | uint | Object ID, output object ID or audio voice handle according to operation. |
| Script | uint | Slot ID, parent ID or physics layer mask according to operation. |
| Result | int | Operation-specific bool/enum/control value. |
| Text | nint / UTF-8 pointer | Text/JSON request or copied-immediately native reply. |

Operation controls the meaning of all fields; they are not universally an entity/vector tuple.
Public wrappers package these consistently. Engine.TextCall temporarily pins a null-terminated
UTF-8 request. Some native services replace Text with a thread-local UTF-8 reply; wrappers copy
and deserialize it immediately. Never retain Text across later service calls or dispatches.

### Service families

| Operations | Native service family |
| --- | --- |
| 0 | Console logging. |
| 1..9 | Physics presence and single player character-capsule services. |
| 10..13 | Named axes and button held/down/up input. |
| 20..24 | Legacy transform position, prefab instantiate/destroy and animator trigger. |
| 25..29 | Shot raycasts, body mass/impulse and scoped shot-trace output. |
| 30 | Managed type-description reply to native editor. |
| 31..36 | Local scale/quaternion and world position transform services. |
| 39..51 | Native component presence, animator/body properties, active/enabled state. |
| 60..80 | Scene/object/hierarchy/reflected data/catalog and script-slot operations; not every number is assigned. |
| 81..84 | Animator base-state/tag/event queries. |
| 87..88 | World/local transform point conversion. |
| 90..92 | Filtered ray, sphere cast and overlap queries. |
| 93..98 | Clock snapshots and script audio voices. |
| 100..115 | Scoped C# editor GUI, selection, undo, dirty state and compilation services. |

Unused numbers are not callable features. A return of zero can mean a failed lookup, a valid
false predicate, an unchanged widget or no physics hit, depending on the wrapper contract.
Use exceptions/predicates exposed by typed APIs rather than interpreting all zero returns
as equivalent. Native services catch exceptions before returning across the managed boundary.
