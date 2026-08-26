# Tartarus Engine

A minimal C++/OpenGL engine skeleton suitable as a base for a first-person shooter:

- **Window/Input/Time** (`src/Core`) — GLFW window + context, polled keyboard/mouse with
  edge-triggered "just pressed" and mouse-delta look, delta-time clock.
- **Renderer** (`src/Renderer`) — hand-rolled OpenGL 3.3 core loader (`extern/glloader`, no
  Python/glad-generator dependency), a `Shader` wrapper, an indexed `Mesh` (cube/plane
  primitives with normals), and an FPS `Camera` (yaw/pitch, view/projection matrices).
- **Physics** (`src/Physics`) — `AABB` box with intersection test, minimum-translation-vector
  resolution, and a ray/box slab test for hitscan shooting.
- **Game** (`src/Game`) — `World` (static level geometry: ground + boxes, collision + raycast
  queries) and `Player` (WASD + sprint, mouse look, gravity/jump, AABB collision resolution
  against the world, and a hitscan `Shoot()`).

`src/main.cpp` wires these into a working demo: walk around a small box-filled arena,
jump, and left-click to "shoot" (destroys the box you're aiming at). Press Escape to
toggle mouse capture.

## Building (Windows, Visual Studio)

Requires CMake 3.16+ and a Visual Studio/MSVC toolset. GLFW, GLM, Assimp, Dear ImGui, and
ImGuizmo are fetched automatically via CMake `FetchContent` on first configure (needs
internet). Substitute your installed Visual Studio version for the generator name below
(e.g. `"Visual Studio 17 2022"`).

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

The executable lands at `build/Release/TartarusEngine.exe` (or `build/Debug/...` for a
Debug build). Run it from a terminal or double-click it — the Font Awesome icon font is
copied next to the exe automatically by a post-build step, no other assets/DLLs needed.

## Extending it

- Add new primitives/loaders to `Mesh` (e.g. OBJ import) for real level art.
- `World::Boxes` is a flat list of AABBs — swap in an actual BVH/octree once level
  geometry count grows.
- `Player::Shoot` currently just raycasts and deactivates a box; hook weapon logic
  (ammo, cooldown, damage, muzzle flash, hit decals) there.
- The renderer is intentionally single-shader/single-pass; add a material system,
  texturing (there's no image loader yet — vertex colors/uniform colors only), and a
  proper render queue as the game grows.
