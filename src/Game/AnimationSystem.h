#pragma once

class World;

// Advances every AnimatorComponent by dt: spin, orbit, bob, and light hue cycling. Call once
// per frame while the scene is playing (never in edit mode — the authored pose must stay put
// so a save never bakes a mid-animation frame). Play mode snapshots and restores the scene
// around itself, so the transforms/colors this mutates are reset on Stop for free.
void UpdateAnimators(World& world, float dt);
