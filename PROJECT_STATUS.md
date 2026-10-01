# Tartarus Engine — Project Status

Plain-language snapshot of where things stand. No code reading required.

## What this is

Tartarus Engine is a custom C++ / OpenGL 3D game engine (GLFW, EnTT, PhysX,
Dear ImGui).
It has its own editor, and a first-person shooter test scene ("Sandbox")
used to develop and prove out the engine's systems.

## Where things stand today

| Area | State | Docs |
|---|---|---|
| Launch screen | Retro CRT splash (WPF) plays while `run-editor.cmd` builds and boots the editor | `tools/launcher/README.md` |
| Weapons | Data-driven (`.fpsanim` + Animator controller), procedural recoil / sway / IK on top. The Sandbox player carries the AKS-74U (key 1), the Remington 870 (key 2) and a gravity gun (key 3) | `docs/FPS_ANIMATION_SYSTEM.md`, `docs/FPS_WEAPON_INTEGRATION.md`, `docs/ANIMATOR.md`, `docs/PROCEDURAL_ANIMATION.md` |
| Player body (true first person, #405) | Root-motion body under the camera: walk / jog / run, jump and land, crouch, turn in place, starts and stops, foot placement. Its own arms hold the gun | `docs/BODY_SETUP.md` |
| Character outfits | Modular Quantum characters dressed from a wardrobe, with skin hiding and clash rules | `docs/CHARACTER_OUTFITS.md`, `docs/OUTFIT_TODO.md` |
| Sky | Physical sky: time of day, atmosphere, volumetric clouds | `docs/SKY.md` |
| Performance | Sandbox at 258 fps (1080p) / 192 fps (1440p), play mode maximized | `docs/PERFORMANCE.md` |

## What is next

- **The separate arms rig is now a hidden pose source.** It is not drawn
  and casts no shadow (the body's own arms do), but it still plays the
  weapon's animations and keeps the sights locked to the camera. Moving
  the gun onto the body's own gun-hand bone was looked at and set aside: the
  sights have to stay camera-locked, so a camera-space rig is still needed
  (see issue #424).
- **Small polish.** Feet have no toe bend, foot placement is off while airborne, and the
  camera during a jump out of a crouch could be smoother.
- **Known limits.** A stop clip covers one foot phase, so entering it can
  make the feet pop slightly. There is no weapon-spread bonus for standing
  still yet (the engine only has recoil kick).
- **Outfits:** clipping in motion, asset fixes and editor polish (`docs/OUTFIT_TODO.md`).
- **Performance:** the GPU and CPU items under "To do" in `docs/PERFORMANCE.md`.

## Working on the engine

- **Build:** `cmake --build build --config Release --target TartarusEngine`. `run-editor.cmd`
  (the desktop shortcut) rebuilds the checkout it lives in and launches the editor.
- **Tests:** `build\Release\TartarusEngine.exe --unit-tests` and
  `--smoke-test tests\smoke-scenes` (both gate CI). `--perf-bench <scene-dir>` prints per-pass
  CPU and GPU times; `tools/sky-review/` does before/after screenshot reviews.
- **Editor side effects:** the editor re-saves asset `.meta` files while running, and autosaves open
  scenes lossily (drops `_comment` fields, reorders keys). Check `git status project` after an
  editor session.
- **Rebuilding while the editor is open:** rename the running `TartarusEngine.exe` first (Windows
  allows it), then delete it afterwards.
- **Line endings:** shaders use LF; most C++ files use CRLF.
