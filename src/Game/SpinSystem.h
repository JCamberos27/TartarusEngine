#pragma once

class World;

// Advances every SpinComponent while the scene is playing: rotates the entity's Transform
// around the component's local axis at its Speed (deg/s). Play -> Stop restores the authored
// pose from the scene snapshot, so this just accumulates per frame — no base-pose scratch.
void UpdateSpinners(World& world, float dt);
