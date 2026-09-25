# Animator — User Guide

The Animator Controller is the engine's animation state machine, modelled on Unity's. A
`.controller` file holds:

- **Parameters**: Float, Int, Bool and Trigger values that game code sets.
- **Tracks**: clip sets, for rigs that are animated together.
- **Layers**: each layer is one state machine.

In each layer:

- **States** play a clip or a 1D blend tree.
- **Transitions** crossfade from one state to another when their conditions hold.

An object runs a controller through its **Animator Controller** component. In Play, the
runtime evaluates every layer, blends the layers, and poses the object's model.

The first-person weapon uses this system. See `FPS_ANIMATION_SYSTEM.md` for that system and
`FPS_WEAPON_INTEGRATION.md` for adding a weapon.

---

## 1. Getting started

| To… | Do this |
|---|---|
| Create a controller | Asset Browser → right-click empty space → **Create Animator Controller**. This creates `project/animators/New Animator Controller.controller` and opens it. Alternatively, press **+ New** on an Animator Controller component, which fills in one state per clip the model can play. |
| Find controllers | Asset Browser → **Animation** folder. It lists every `.controller` and `.fpsanim` in the project. |
| Open the editor | Double-click a controller, use **Open Animator** on the component, or go to **Window → Animator**. |
| Use it on an object | Add an **Animator Controller** component to an object with a rigged Mesh Renderer, then pick the controller. |
| Drive it from code | Get the `AnimatorControllerComponent` and call `SetFloat / SetInt / SetBool / SetTrigger`. Read results with `InState(name)`, `HasTag(tag)`, `EventFired(name)`, `StateTime` and `InTransition`. |

---

## 2. The Animator window

```
┌ file ▾  ↶ ↷  ⤢  64%  rig ▾ ─────────────────────────────────────────────────────────┐
│ Layers | Parameters │               graph (pan / zoom)             │ selection     │
│  Base Layer         │   [Any State]──►                             │ properties    │
│  + Layer            │   [Entry]──►[Idle]──►[Walk]      [Exit]      │               │
│  layer settings     │                                              │               │
│  bone mask, tracks  │                                              │               │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

- **Top bar**
  - Controller file, undo/redo, frame all, and zoom.
  - **Rig**: an object that uses this controller. Its model's clips fill the clip pickers and its bone tree fills the mask picker. In Play, its live state is highlighted in the graph.
- **Graph**
  - Middle-drag pans. The mouse wheel zooms around the cursor. **F** frames everything.
  - **Right-click empty space** gives *Create State*, *Create Blend Tree State* and *Frame All*.
  - **Right-click a state** gives *Make Transition* (then click the target state or Exit), *Set as Layer Default State*, *Duplicate* and *Delete*.
  - Right-click **Entry** or **Any State** and choose *Make Transition* to make a transition from it.
  - Click to select. Ctrl/Shift+click adds to the selection. Drag on empty space to box-select. Drag a node to move it (all selected nodes move together).
  - Click a transition arrow to select it. An arrow with three chevrons holds several transitions; click it again to cycle through them.
  - **Del** deletes, **Ctrl+D** duplicates, **Ctrl+Z / Ctrl+Y** undo and redo, **Esc** cancels a transition you are drawing.
- **Colours**
  - Orange: the layer's default state. Green: Entry. Teal: Any State. Red: Exit.
  - In Play: blue for the playing state, with a progress bar. States still fading out are tinted, and the transition that is crossfading turns blue.
- **Saving**: every edit is saved to the file immediately and pushed onto the window's undo stack. A running Play picks up the change within half a second.

---

## 3. Concepts

### States

| Field | Meaning |
|---|---|
| Motion (one per track) | A **Clip**, or a **Blend Tree 1D**. Pick a clip from the list (type to search; **Project files** lists animation files no scene has loaded yet, such as `assets/animations/mc_core_motion`), or drag an FBX from the Asset Browser onto the slot. If there is no clip on the base layer, that track holds its bind pose. On a higher layer, no clip means the state adds nothing. |
| Speed / Speed Param | Playback rate. The optional Float parameter multiplies it. |
| Loop | Loops (on) or holds the last frame (off). |
| Root Motion | With the component's Root Motion on, this state's travel moves the object. Off keeps the travel in the pose. Under the checkbox, the window shows how far the clip travels and turns per pass, and its speed, measured on the selected rig. |
| Priority | Used by Any State transitions that have **Respect Priority** on (see below). |
| Tags | Comma-separated labels that game code reads with `HasTag` (e.g. `ADS`, `Reload`, `Hidden`). They keep code independent of state names. |
| Events | A name at a normalized time: 0 = entry, 1 = the end of one pass. Game code sees it through `EventFired(name)` on the frame it is crossed. Looping states fire their events every loop. |

### Transitions

| Field | Meaning |
|---|---|
| Conditions | All must hold. Float/Int use Greater, Less, Equals or Not Equal against a threshold. Bool/Trigger use Is True or Is False. A transition that fires **consumes** the triggers it tested. |
| Has Exit Time / Exit Time | Waits until the source state reaches this normalized time. 1 = its end, and values above 1 count loops. |
| Duration | Crossfade length in seconds. 0 cuts instantly. |
| Offset | Where to start in the destination state (normalized). |
| Interruptible | When off, nothing can interrupt this crossfade once it starts. |
| Respect Priority (Any State only) | Fires only into a state whose priority is **strictly higher** than the playing state's. |
| To Self (Any State only) | May restart the state that is already playing. |
| Order | Transitions are checked top to bottom (Any State first, then the playing state's own). The first one that holds wins. Use the ↑ ↓ buttons to reorder. |

A transition with no conditions and no exit time never fires, and the window warns you about it.

### Entry, Any State, Exit

- **Entry** decides where a layer starts. Its transitions are checked in order and the first
  whose conditions hold wins; otherwise the layer starts in the default (orange) state.
- A transition to **Exit** leaves the current state and goes back in through Entry, so the
  next state is picked from the current parameters. This is how a one-shot (fire, reload)
  returns to "whatever locomotion is right now" without a transition to every locomotion
  state.
- **Any State** transitions are checked every frame, whatever state is playing.

### Crossfades

Each layer keeps a crossfade stack, so a transition that interrupts another crossfade
starts from the blended pose that was showing. Nothing pops.

### 1D blend trees

A blend-tree state blends clips along one Float parameter:

- Each child clip has a **threshold**.
- At a given value, the two neighbouring children are mixed linearly. Below the lowest threshold or above the highest, the end clip plays alone.
- Children play phase-synced, so a walk and a run keep their feet in step.
- The properties panel previews the child weights at the parameter's current value.

### Layers and bone masks

Layer 0 is the base layer and always plays at full weight. Each higher layer has:

- **Weight** (0–1).
- **Blending**:
  - **Override** replaces the pose beneath it, by weight.
  - **Additive** adds the layer's motion relative to each clip's first frame.
- **Bone Mask**: bones in *Include* take part in the layer, along with their children. *Exclude* removes a branch inside that. An empty Include list means the whole rig. Use **+ From rig** to pick bones from the selected rig.

A typical example is an upper-body reload layer (Include `spine_01`) over lower-body locomotion.

### Tracks

A controller can hold several clip sets per state (for example `arms` and `weapon`). Each
object picks one with its component's **Track** field. An object whose `Driver` is set
(from code) mirrors that driver's states, times and crossfades exactly and evaluates no
transitions of its own. This is how the weapon rig stays locked to the arms.

A state lasts as long as the longest motion across every rig in the group, so a shorter clip
on one track holds its last frame instead of cutting the other track short.

### Root motion

A clip can author travel on its root bone: a walk that really moves forward, a turn that
really turns. With **Root Motion** on in the component (Animator Controller, or a plain
Animation component), that travel comes out of the pose and moves the object instead. The
character stays on its object, collision and cameras follow it, and a looping walk keeps
walking instead of snapping back each loop.

| Setting | Meaning |
|---|---|
| Root Motion: **Off** | The clips play exactly as authored. |
| Root Motion: **Apply** | The travel moves the object. On a simulated Rigidbody it becomes the body's velocity, so walls stop the character. Freeze the body's X and Z rotation so it can't tip over. A kinematic body, or an object with no Rigidbody, moves its Transform. |
| Root Motion: **In Place** | The pose stays in place, but the object doesn't move. Game code reads the motion and moves the object itself (for example through a character controller). This is also the mode for a second rig parented to one that applies it, such as a separate head mesh. |
| Root Bone | Whose travel counts. **Auto** picks `root` when the rig has one (Unreal and most game rigs), else the hips or pelvis (Mixamo). |
| Rotation | Turns in the clips turn the object. Off leaves the turn in the pose. |
| Vertical | Height changes move the object. Off (the usual choice) leaves jumps and crouches in the pose, so the object stays on the ground. |

How it works:
- **Ground plane only by default.** The root's travel along the ground and its heading become the object's movement. What's left in the pose is the bob, the lean and the hip sway.
- **Crossfades and blend trees mix their travel** in the same proportions as the poses, so walk-to-run speeds up smoothly.
- **Loops carry on** from where the last pass ended.
- **Only the base layer drives root motion.** Higher layers never move the object.
- **Per-state opt-out:** a state with **Root Motion** off in the Animator window keeps its travel in the pose.
- **Live readout:** while playing, the Inspector's Root Motion section shows the live speed (m/s) and turn rate (°/s).
- **Clip packs:** packs often ship each clip twice, with root motion and in place (`_No_Rm` in the MC Core Motion pack). With Root Motion on, use the version with root motion.
- **Try it:** the Sandbox's *Root Motion Demo* (x −42, z −30) has two Quantum characters running the same controller. One has Root Motion on and walks, stops, turns and walks back. The other has it off: its mesh walks away from its object and snaps back each loop.

---

## 4. For programmers

```cpp
auto& anim = registry.get<AnimatorControllerComponent>(entity);
anim.SetFloat("Speed", speed);
anim.SetBool("Aim", aiming);
anim.SetTrigger("Fire");                       // stays set until a transition consumes it
if (anim.HasTag("Reload")) { /* ... */ }
if (anim.EventFired("Refill")) ammo = magazine;
```

- **Update order.** The controllers update once per Play frame (`UpdateAnimatorControllers`,
  `src/main.cpp`), after the player and first-person code and before the game DLL. Values set
  by the game DLL therefore take effect on the next frame.
- **Triggers** are not cleared automatically. If an input must not fire later when it is
  refused (for example, fire pressed during a reload), reset it before setting it again next
  frame. The first-person driver does exactly this.
- **Live editing in Play.** The live Parameters panel writes straight to the component. Code
  that sets a parameter every frame overwrites whatever you enter there.
- **Tests.** `AdvanceAnimator(ctrl, component, dt, stateLength)` runs the state machine
  without a model. `TestAnimatorController` and `TestFirstPersonAnimationFSM` use it.
- **IK.** An `IKRigComponent` on the same entity runs on the blended pose just before it is
  applied (two-bone limbs, look-at, runtime bone offsets). See PROCEDURAL_ANIMATION.md.
- **Model API.** Posing goes through `Model::SampleLocalPose` / `Model::ApplyLocalPose`. The
  model's own `PlayAnimation` path is left untouched for every other caller.
- **Root motion.** The maths is in `src/Game/RootMotion.h`, and `TestRootMotion` covers it.
  - After each update the component's `RootMotion` holds the last frame's motion:
    - `DeltaPosition`: world metres, or the parent's space when the object is parented.
    - `DeltaYaw`: degrees.
    - `Speed` / `TurnRate`: smoothed.
  - In **In Place** mode, apply the motion yourself:
    ```cpp
    const auto& rm = registry.get<AnimatorControllerComponent>(e).RootMotion;
    controllerMove += rm.DeltaPosition;  // e.g. through your character controller
    yawDegrees    += rm.DeltaYaw;
    ```
  - `ApplyRootMotion` (`AnimationSystem.h`) is what **Apply** does.
  - The first-person player is never moved by it: its own controller owns its movement.

### File format (v2)

```jsonc
{
  "version": 2,
  "parameters": [{ "name": "Speed", "type": "float", "default": 0 }],
  "tracks": ["arms", "weapon"],
  "layers": [{
    "name": "Base Layer", "weight": 1, "blending": "override", "defaultState": "Idle",
    "maskInclude": [], "maskExclude": [],
    "states": [{
      "name": "Idle", "loop": true, "speed": 1, "priority": 0, "tags": ["Idle"],
      "position": [0, 0],
      "motions": { "arms": { "clip": "assets/.../FP_Idle.fbx", "clipGuid": "…" } },
      "events": [{ "name": "Refill", "time": 1.0 }]
    }],
    "transitions": [
      { "from": "Idle", "to": "Walk", "duration": 0.1, "hasExitTime": false, "exitTime": 0.9,
        "conditions": [{ "param": "Speed", "mode": "greater", "threshold": 0.05 }] },
      { "fromAny": true, "to": "Fire", "respectPriority": true, "canTransitionToSelf": true, … },
      { "fromEntry": true, "to": "Sprint", … },
      { "from": "Fire", "to": "Exit", "hasExitTime": true, "exitTime": 1.0, … }
    ]
  }]
}
```

- A blend-tree motion is written as `{ "blendParam": "Speed", "children": [{ "clip": …, "threshold": 0, "speed": 1 }] }`.
- v1 files (a flat `states` / `transitions` list, with `"from": "Any"`) still load as a single base layer.
- Floats are saved rounded to 6 decimals, so the files stay easy to hand-edit and diff.

---

## 5. Not supported yet

- 2D blend trees, sub-state machines and animation curves.
- Per-transition interruption source (Unity's *Current / Next / ordered* settings). This system has only *Interruptible* on/off plus priorities.
- A preview of motions in the window while in Edit mode.
- Root motion on higher layers, and extracting a curve (e.g. a speed parameter) from it.
