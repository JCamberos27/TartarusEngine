# Tartarus Engine — Project Status

Plain-language snapshot of where things stand. No code reading required.

## What this is

Tartarus Engine is a custom C++ / Vulkan 3D game engine, built partly on top of
the open-source Acid engine and adapted to this project's own conventions.
It has its own editor, and a first-person shooter test scene ("Sandbox")
used to develop and prove out the engine's systems.

## Where things stand today

**Launch screen** — done. A retro CRT-style splash screen (a separate WPF
window on Windows) plays while the engine boots, with a tube-warm-up shader,
progress bar, and build info.

**FPS weapon animation** — done and documented (`FPS_ANIMATION_SYSTEM.md`,
`ANIMATOR.md`, `FPS_WEAPON_INTEGRATION.md`). Weapons are data-driven
(`.fpsanim` files): a state machine controls which animation clip plays
when (idle, walk, fire, reload, aim down sights, etc.), and procedural
effects (recoil, sway, breathing) layer on top. The AKS-74U is the one
fully working weapon so far.

**Player locomotion, phase 1 — done.** The player has their own body in
the world (not just a floating camera) that walks, jogs, runs, jumps,
falls, and lands using real animation clips instead of just sliding the
camera around. This is what "root motion" means: the animation itself
supplies the movement, not a separate physics script guessing at how far
a walk cycle should move you.

**Player locomotion, phase 2 — not started.** Right now the gun and the
arms holding it are a completely separate model that floats in front of
the camera, not attached to the body at all. Phase 2 means giving the
player's own body real arms that hold the gun, so there's one connected
character instead of two overlapping ones.

**Player locomotion, phase 3 — not started.** Polish work: turning in
place without the body twisting oddly, smooth starts/stops instead of
snapping straight into a walk, crouching, and feet that plant properly on
slopes and stairs instead of clipping through them.

## Recent fix (this session)

The player's body has its own set of arms baked into its torso model
(inherited from the "Quantum" character asset), and they were never told
to hide themselves. So in Play mode you'd see two disconnected sets of
arms at once: the body's own arms (hanging in whatever pose the walk
animation left them in) and the separate gun-holding arms floating out in
front of the camera, with no relationship to each other.

The engine already has a setting built for exactly this ("Hidden Bones"
on the body's settings), it just wasn't filled in for this scene. It's now
set to hide the body's arms from the shoulder down, so only the
gun-holding arms show. This is a one-line data fix in the scene file, not
a code change — verified by reading the code that does the hiding and by
inspecting the actual character model file to confirm the bone names, but
**not verified by running the engine and looking at it**, since this cloud
session has no graphics hardware or Vulkan drivers to actually launch the
editor. Worth a quick look next time you're at your own machine to confirm
it looks right.

## Known limitations of this work session

This session runs in a cloud container with no GPU and no Vulkan SDK
installed, and the project's build/run workflow (`run-editor.cmd`) is
Windows-specific. That means changes to C++ code or shaders can be
written and reasoned through carefully, but can't be compiled or run here
to confirm they work — that has to happen on your own machine. Pure data
changes (like the scene fix above) can be verified more confidently by
reading the code that consumes them.
