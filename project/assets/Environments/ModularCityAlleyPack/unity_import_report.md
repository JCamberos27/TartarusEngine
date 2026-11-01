# ModularCityAlleyPack: Unity import report

Source: `C:\Users\jacob\Setup Guide In-Editor Tutorial\Assets\ModularCityAlleyPack`

## Counts

- architecture scaled x0.82: 3340
- baked-only lights (now realtime): 7
- built-in cubes as boxes: 17
- colliders: 5754
- colliders dropped from clutter and flat dressing (by name): 45
- colliders dropped from small props (under 35 cm): 49
- duplicate openables dropped (a twin door within 10 cm): 0
- flat box colliders given a 4 cm slab: 74
- free-standing props kept at 1.0: 814
- lights: 24
- material overrides on instances: 669
- materials: 43
- model variants (one mesh of a multi-mesh FBX): 27
- models: 400
- prefabs: 387
- prop mesh colliders made convex: 106
- props re-seated on their floor after the architecture scale: 82
- reflection probes: 4
- renderers: 4776
- scenes: 3
- scenes given the AAA look preset: 2
- scripts dropped (MonoBehaviour): 25
- textures: 166

## Not converted / approximated

### animated objects (doors and lifts became Openables; the rest stay static) (12)
- Example_Scene_Day: TreeDead_Anim
- Example_Scene_Day: TreeDead_Anim (1)
- Example_Scene_Day: TreeDead_Anim (2)
- Example_Scene_Day: TreeDead_Anim (3)
- Example_Scene_Day: TreeDead_Anim (4)
- Example_Scene_Night: TreeDead_Anim
- Example_Scene_Night: TreeDead_Anim (1)
- Example_Scene_Night: TreeDead_Anim (2)
- Example_Scene_Night: TreeDead_Anim (3)
- Example_Scene_Night: TreeDead_Anim (4)
- Prefabs: TreeDead_Anim
- TreeDead_Anim.prefab: TreeDead_Anim

### materials not converted (particle) (1)
- Particles_Steam

### materials not converted (skybox) (3)
- Sky
- Sky2
- Sky3

### particle systems left out (rebuild with the engine's Particle System) (17)
- Example_Scene_Day: Particles_AirVentSteam
- Example_Scene_Day: Particles_Dirt
- Example_Scene_Day: Particles_Dirt (1)
- Example_Scene_Day: Particles_Dirt (2)
- Example_Scene_Day: Particles_Dirt (3)
- Example_Scene_Day: Particles_Fog
- Example_Scene_Day: Particles_Fog (1)
- Example_Scene_Night: Particles_AirVentSteam
- Example_Scene_Night: Particles_Dirt
- Example_Scene_Night: Particles_Dirt (1)
- Example_Scene_Night: Particles_Dirt (2)
- Example_Scene_Night: Particles_Dirt (3)
- Example_Scene_Night: Particles_Fog
- Example_Scene_Night: Particles_Fog (1)
- Particles_AirVentSteam.prefab: Particles_AirVentSteam
- Particles_Dirt.prefab: Particles_Dirt
- Particles_Fog.prefab: Particles_Fog

### player rigs / cameras left out (the engine has its own) (3)
- Example_Scene_Day: FPS Controller
- Example_Scene_Night: FPS Controller
- FPS Controller.prefab: FPS Controller

