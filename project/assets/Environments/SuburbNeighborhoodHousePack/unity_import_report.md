# SuburbNeighborhoodHousePack: Unity import report

Source: `C:\Users\jacob\Setup Guide In-Editor Tutorial\Assets\SuburbNeighborhoodHousePack`

## Counts

- baked-only lights (now realtime): 154
- built-in cubes as boxes: 5
- colliders: 8096
- duplicate openables dropped (a twin door within 10 cm): 0
- lights: 173
- material overrides on instances: 1636
- materials: 108
- model variants (one mesh of a multi-mesh FBX): 102
- models: 537
- openable clips applied: 80
- openables: 64
- prefabs: 463
- reflection probes: 12
- renderers: 7619
- scenes: 3
- scripts dropped (MonoBehaviour): 2590
- textures: 477

## Not converted / approximated

### Foliage - revisit in P3 (parked: Standard cutout two-sided; wind shader later) (7)
- ClimberPlant
- Plant_BushGrass
- Plant_Flowers
- Plant_Grass
- Plant_Hedgebush
- Plant_Treebush
- Tree

### animated objects (doors and lifts became Openables; the rest stay static) (89)
- Demoscene_Day: Door_exterior_paneled_mesh
- Demoscene_Day: Door_exterior_white_mesh
- Demoscene_Day: Door_exterior_windowed_brown_mesh
- Demoscene_Day: Door_exterior_windowed_mesh
- Demoscene_Day: Door_garage_5x
- Demoscene_Day: Door_garage_brown_3x
- Demoscene_Day: Door_interior_mesh
- Demoscene_Day: ParticlesParent
- Demoscene_Day: Tree_skinned_large
- Demoscene_Day: Tree_skinned_large (1)
- Demoscene_Day: Tree_skinned_large (2)
- Demoscene_Day: Tree_skinned_large (3)
- Demoscene_Day: Tree_skinned_large (4)
- Demoscene_Day: Tree_skinned_large (5)
- Demoscene_Day: Tree_skinned_large (6)
- Demoscene_Day: Tree_skinned_large (7)
- Demoscene_Day: Tree_skinned_large (8)
- Demoscene_Day: Tree_skinned_large (9)
- Demoscene_Day: Tree_skinned_large_split
- Demoscene_Day: Tree_skinned_large_split (1)
- Demoscene_Day: Tree_skinned_large_split (2)
- Demoscene_Day: Tree_skinned_large_split (3)
- Demoscene_Day: Tree_skinned_large_split (4)
- Demoscene_Day: Tree_skinned_large_split (5)
- Demoscene_Day: Tree_skinned_large_split (6)
- Demoscene_Day: Tree_skinned_large_split (7)
- Demoscene_Day: Tree_skinned_large_split (8)
- Demoscene_Day: Tree_skinned_large_split (9)
- Demoscene_Day: Tree_skinned_small
- Demoscene_Day: Tree_skinned_small (1)
- Demoscene_Day: Tree_skinned_small (2)
- Demoscene_Day: Tree_skinned_small (3)
- Demoscene_Night: Door_exterior_paneled_mesh
- Demoscene_Night: Door_exterior_white_mesh
- Demoscene_Night: Door_exterior_windowed_brown_mesh
- Demoscene_Night: Door_exterior_windowed_mesh
- Demoscene_Night: Door_garage_5x
- Demoscene_Night: Door_garage_brown_3x
- Demoscene_Night: Door_interior_mesh
- Demoscene_Night: Tree_skinned_large
- Demoscene_Night: Tree_skinned_large (1)
- Demoscene_Night: Tree_skinned_large (2)
- Demoscene_Night: Tree_skinned_large (3)
- Demoscene_Night: Tree_skinned_large (4)
- Demoscene_Night: Tree_skinned_large (5)
- Demoscene_Night: Tree_skinned_large (6)
- Demoscene_Night: Tree_skinned_large (7)
- Demoscene_Night: Tree_skinned_large (8)
- Demoscene_Night: Tree_skinned_large (9)
- Demoscene_Night: Tree_skinned_large_split
- Demoscene_Night: Tree_skinned_large_split (1)
- Demoscene_Night: Tree_skinned_large_split (2)
- Demoscene_Night: Tree_skinned_large_split (3)
- Demoscene_Night: Tree_skinned_large_split (4)
- Demoscene_Night: Tree_skinned_large_split (5)
- Demoscene_Night: Tree_skinned_large_split (6)
- Demoscene_Night: Tree_skinned_large_split (7)
- Demoscene_Night: Tree_skinned_large_split (8)
- Demoscene_Night: Tree_skinned_large_split (9)
- Demoscene_Night: Tree_skinned_small
- Demoscene_Night: Tree_skinned_small (1)
- Demoscene_Night: Tree_skinned_small (2)
- Demoscene_Night: Tree_skinned_small (3)
- Door_exterior_paneled.prefab: Door_exterior_paneled_mesh
- Door_exterior_white.prefab: Door_exterior_white_mesh
- Door_exterior_windowed.prefab: Door_exterior_windowed_mesh
- Door_exterior_windowed_brown.prefab: Door_exterior_windowed_brown_mesh
- Door_garage_3x.prefab: Door_garage_3x
- Door_garage_5x.prefab: Door_garage_5x
- Door_garage_brown_3x.prefab: Door_garage_brown_3x
- Door_garage_brown_5x.prefab: Door_garage_brown_5x
- Door_interior.prefab: Door_interior_mesh
- Showcase: Door_exterior_paneled_mesh
- Showcase: Door_exterior_white_mesh
- Showcase: Door_exterior_windowed_brown_mesh
- Showcase: Door_exterior_windowed_mesh
- Showcase: Door_garage_3x
- Showcase: Door_garage_5x
- Showcase: Door_garage_brown_3x
- Showcase: Door_garage_brown_5x
- Showcase: Door_interior_mesh
- Showcase: ParticlesParent
- Showcase: Tree_skinned_large
- Showcase: Tree_skinned_large_split
- Showcase: Tree_skinned_small
- Tree_skinned_large.prefab: Tree_skinned_large
- Tree_skinned_large_split.prefab: Tree_skinned_large_split
- Tree_skinned_small.prefab: Tree_skinned_small
- Watersprinkler.prefab: ParticlesParent

### area lights (baked only in Unity), skipped (4)
- Area Light Baked
- Area Light Bathroom Baked
- Area Light Window
- Area Light Window Garage

### materials approximated on Standard (water) (1)
- Water

### materials not converted (particle) (2)
- Particles_Steam
- Particles_WaterShower

### materials not converted (skybox) (2)
- Sky_day
- Sky_night

### missing materials (dangling in the source pack: asset deleted) (2)
- aa545f52cfdbe1b4886ead0da37cfe93 (used by Clothcloset_with_wall.FBX)
- b6d99d864dff8634695410822beb7e3f (used by Clothcloset_with_wall.FBX)

### missing textures (dangling in the source pack: asset deleted, nothing to convert) (1)
- 61943a46651cdc54686970488837e7f3 (used by material Background_Bush)

### particle systems left out (rebuild with the engine's Particle System) (5)
- Demoscene_Day: Particles_Shower
- Demoscene_Night: Particles_fog
- Particles_fog.prefab: Particles_fog
- Showcase: Particles_Shower
- Watersprinkler.prefab: Particles_Shower

### player rigs / cameras left out (the engine has its own) (2)
- Demoscene_Day: FPS Controller
- Demoscene_Night: FPS Controller

### reflection probes with a box offset (moved the probe, ignoring the parent rotation) (4)
- Reflection Probe House01
- Reflection Probe House03
- Reflection Probe House04
- Reflection Probe House05

