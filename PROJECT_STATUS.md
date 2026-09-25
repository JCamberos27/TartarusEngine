# Tartarus Engine â€” Project Status

Plain-language snapshot of where things stand. No code reading required.

## What this is

Tartarus Engine is a custom C++ / Vulkan 3D game engine, built partly on top of
the open-source Acid engine and adapted to this project's own conventions.
It has its own editor, and a first-person shooter test scene ("Sandbox")
used to develop and prove out the engine's systems.

## Where things stand today

**Launch screen** â€” done. A retro CRT-style splash screen (a separate WPF
window on Windows) plays while the engine boots, with a tube-warm-up shader,
progress bar, and build info.

**FPS weapon animation** â€” done and documented (`FPS_ANIMATION_SYSTEM.md`,
`ANIMATOR.md`, `FPS_WEAPON_INTEGRATION.md`). Weapons are data-driven
(`.fpsanim` files): a state machine controls which animation clip plays
when (idle, walk, fire, reload, aim down sights, etc.), and procedural
effects (recoil, sway, breathing) layer on top. The AKS-74U is the one
fully working weapon so far.

**Player locomotion, phase 1 â€” done.** The player has their own body in
the world (not just a floating camera) that walks, jogs, runs, jumps,
falls, and lands using real animation clips instead of just sliding the
camera around. This is what "root motion" means: the animation itself
supplies the movement, not a separate physics script guessing at how far
a walk cycle should move you.

**Player locomotion, phase 2 — done.** The player's own body now has the
arms that hold the gun. The old separate floating arms are still there
behind the scenes (they animate the reload, inspect, aim and so on), but
what you see is the body's own arms following their hands, so it is one
connected character. Where the camera sits comes from the body's
shoulders, so the gun stays within the arms' reach: no stretched arms and
no hand pulling off the gun. Holstering hides the arms, draw and Play both
bring them straight onto the gun, and the camera keeps the same height
armed or unarmed.

**Player locomotion, phase 3 — done.** The polish pass:
- **Turning in place.** Turn the view far enough while standing still and
  the feet step around to face it, with the chest twisting to keep up. The
  turn speed is capped so the feet never slide, and the mouse feels
  normal while aiming.
- **Starts and stops.** Real start and stop clips play as you begin or end
  a run. Tapping a move key does not trigger a stop clip or a full
  push-off.
- **Crouch.** Hold Left Ctrl. The body drops with a shorter collision
  capsule and a lower camera, moves slower, and has its own stand-to-crouch,
  turn, start and stop clips. It only stands up when there is headroom.
- **Foot placement.** Feet are placed on the ground under them, so they no
  longer clip into slopes and stairs, and a planted foot stays put instead
  of skating. Going up or down small stairs no longer bounces the camera.
- **Jump and land.** You keep your speed through a landing. A jump pressed
  just before landing, or just after stepping off an edge, still counts.
  You can jump out of a crouch when there is room.

**Weapon states with the new arms — checked.** Aim down sights, reload,
inspect, melee, holster and draw, and firing while walking were played and
captured frame by frame. It turned up three bugs, all fixed: a pair of hands
flashing after the holster, the camera rising when you holstered, and the
gun sitting still while the hands did the equip animation at Play.

## What is next

- **Retire the separate arms model.** The gun should ride the body's own
  gun-hand bone directly, so the hidden second arms rig and the extra
  shadow it can cast go away. This is the biggest remaining piece.
- **Small polish.** The laser beam shows briefly at the start of a draw,
  feet have no toe bend, foot placement is off while airborne, and the
  camera during a jump out of a crouch could be smoother.
- **Known limits.** A stop clip covers one foot phase, so entering it can
  make the feet pop slightly. There is no weapon-spread bonus for standing
  still yet (the engine only has recoil kick).
