# MC Core Motion (UE5 skeleton)

Animation-only FBX clips (no meshes), exported for Unreal 5's mannequin skeleton: `root`,
`pelvis`, `spine_01`-`spine_05`, `neck_01`-`neck_02` and so on. That is the Quantum character's
skeleton (`assets/quantum`), so the clips play on it as they are; the engine matches clips to a
character by bone name. They don't fit Y Bot (Mixamo bone names).

- `AM_` male, `AF_` female. 30 fps, except a few female run clips (check their speed in Play).
- `_No_Rm` = in place. The version without it carries root motion: the root really travels
  (up to ~10 m). The engine has no root-motion extraction yet, so on a character a root-motion
  clip drifts away from its object and snaps back each loop. Use the `_No_Rm` clips for now.
- `_Mirror` = the same move mirrored left/right.
- `Objects/Phone/SM_Prop_Phone.fbx` is the prop the Phone clips hold.

Folders: Conversation, Crouch, Dance, Idle, Jump, Kneel, Locomotion (with Locomotion_Mirror),
Locomotion_V2 (walk / jog / run / crouch-walk: starts, stops, pivots, 8 directions), Look_At,
Phone, Pickup, React_Bump, Ready, Turn, Wave.

The same pack also shipped for an older UE4-style skeleton (`Unity_Skel`, 3 spine bones, jaw and
eyes); that copy was left out. The originals are in `OpenGL Ready Assets\Animations\Mc_Core_Motion`.

The clips aren't registered in any scene, so they don't slow scene loads. Point an Animator
Controller state at one by path and it loads when used.
