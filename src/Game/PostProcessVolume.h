#pragma once

#include <glm/glm.hpp>

class World;
struct PostSettings;

// #162 / #203 - how much a Post-process Volume affects a camera at `cameraPos`: its Weight for a
// Global volume; for a local one, Weight inside its (oriented, scaled) box, fading linearly to 0
// at Blend Distance outside it. `volumeWorld` is the volume entity's world transform.
float PostVolumeWeight(bool global, const glm::vec3& size, float blendDistance, float weight,
                       const glm::mat4& volumeWorld, const glm::vec3& cameraPos);

// Blends every active Post-process Volume in the scene into `p` (which starts as the scene's own
// settings), lowest Priority first so higher ones win.
void ApplyPostProcessVolumes(const World& world, const glm::vec3& cameraPos, PostSettings& p);
