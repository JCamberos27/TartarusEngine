# MC Core Motion (UE5 skeleton)

Animation-only FBX clips (no meshes), exported for Unreal 5's mannequin skeleton: `root`,
`pelvis`, `spine_01`-`spine_05`, `neck_01`-`neck_02` and so on. That is the Quantum character's
skeleton (`assets/Characters/Quantum`), so the clips play on it as they are; the engine matches clips to a
character by bone name. They don't fit Y Bot (Mixamo bone names).

## Folders

- `RootMotion/` (379 clips): the root really travels (up to ~10 m). The player body is root-motion
  driven, so these are the clips controllers use.
- `InPlace/` (280 clips, the `_No_Rm` files): the same moves with the root held in place. Only
  Locomotion, Locomotion_V2, Crouch and Turn have in-place versions.
- `Objects/Phone/SM_Prop_Phone.fbx`: the prop the Phone clips hold.

Inside each, the pack's own categories: Conversation, Crouch, Dance, Idle, Jump, Kneel, Locomotion
(with `Locomotion_Mirror`), Locomotion_V2 (walk / jog / run / crouch-walk: starts, stops, pivots,
8 directions), Look_At, Phone, Pickup, React_Bump, Ready, Turn, Wave.

## Names

- `AM_` male, `AF_` female. 30 fps, except a few female run clips (check their speed in Play).
- `_No_Rm` = in place; `_Mirror` = the same move mirrored left/right.

## Notes

The same pack also shipped for an older UE4-style skeleton (`Unity_Skel`, 3 spine bones, jaw and
eyes); that copy was left out. The originals are in `OpenGL Ready Assets\Animations\Mc_Core_Motion`.

Clips are referenced by path (`"clip": "assets/Animations/Mocap/RootMotion/..."`), so moving or
renaming one means updating the controllers that point at it. The clips aren't registered in any
scene, so they don't slow scene loads; a clip loads when a controller state uses it.
