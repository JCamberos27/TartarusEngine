# OfficeAndPoliceStation: Unity import report

Source: `C:\Users\jacob\Setup Guide In-Editor Tutorial\Assets\OfficeAndPoliceStation`

## Counts

- baked-only lights (now realtime): 18
- built-in cubes as boxes: 1
- colliders: 4946
- duplicate openables dropped (a twin door within 10 cm): 10
- lights: 32
- material overrides on instances: 275
- materials: 65
- model variants (one mesh of a multi-mesh FBX): 65
- models: 232
- openable clips applied: 90
- openables: 76
- prefab scale overrides applied: 34
- prefabs: 221
- reflection probes: 9
- renderers: 2973
- scenes: 2
- scripts dropped (MonoBehaviour): 110
- textures: 274
- textures halved: 6

## Not converted / approximated

### animated objects (doors and lifts became Openables; the rest stay static) (24)
- Cell.prefab: Door_Cell_mesh
- Door_Bronze_Metal.prefab: Door_Bronze_Metal_mesh
- Door_Bronze_Windows.prefab: Door_Bronze_Windows_mesh
- Door_Cell.prefab: Door_Cell_mesh
- Door_Metal_Dark.prefab: Door_Metal_Dark_mesh
- Door_Metal_Gray.prefab: Door_Metal_Gray_mesh
- Door_Wooden_Black.prefab: Door_Metal_Gray_mesh
- Elevator.prefab: Elevator_Doors
- Elevator_Doors_ParentMain.prefab: Elevator_Doors
- Fan_table.prefab: Fan_table_blades
- OfficePack_Example: Door_Bronze_Metal_mesh
- OfficePack_Example: Door_Cell_mesh
- OfficePack_Example: Door_Metal_Dark_mesh
- OfficePack_Example: Door_Metal_Gray_mesh
- OfficePack_Example: Elevator_Doors
- OfficePack_Example: Fan_table_blades
- Showcase: Door_Bronze_Metal_mesh
- Showcase: Door_Bronze_Windows_mesh
- Showcase: Door_Cell_mesh
- Showcase: Door_Metal_Dark_mesh
- Showcase: Door_Metal_Gray_mesh
- Showcase: Elevator_Doors
- Showcase: Fan_table_blades
- W_3x_Elevator.prefab: Elevator_Doors

### area lights (baked only in Unity), skipped (34)
- Area Light Window
- Area Light Window (1)
- Area Light Window (10)
- Area Light Window (11)
- Area Light Window (12)
- Area Light Window (13)
- Area Light Window (14)
- Area Light Window (15)
- Area Light Window (16)
- Area Light Window (17)
- Area Light Window (2)
- Area Light Window (3)
- Area Light Window (4)
- Area Light Window (5)
- Area Light Window (6)
- Area Light Window (7)
- Area Light Window (8)
- Area Light Window (9)
- Area Light ceiling 2nd
- Area Light ceiling cell
- Area Light ceiling cells corridor
- Area Light ceiling conference room
- Area Light ceiling elevator
- Area Light ceiling embeded double
- Area Light ceiling embeded single
- Area Light ceiling embeded single 
- Area Light ceiling fluorescent desksroom
- Area Light ceiling lockers
- Area Light ceiling stairs
- Area Light ceiling toilet
- Area Light police area
- Area Light police area backside
- Area Light police area chief
- Area Light signs

### materials not converted (skybox) (1)
- Sky

### player rigs / cameras left out (the engine has its own) (3)
- FPS Controller.prefab: FPS Controller
- OfficePack_Example: FPS Controller
- Showcase: Main Camera

### reflection probes with a box offset (moved the probe, ignoring the parent rotation) (1)
- Reflection Probe (5)

