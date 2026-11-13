#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>

class Model;

// Octahedral impostors (--bake-impostors, ImpostorBake.h): the baked sets the engine draws in place of a LOD group's
// levels beyond a distance (World::ApplyLod picks the level, ImpostorRenderer draws it). Each set folder under
// project/assets/Impostors holds an impostors.json and the atlases it names; an impostor stands in for every LOD group
// whose LOD 0 draws exactly its models. CPU only (paths and numbers): the renderer loads the textures.
struct ImpostorSet {
    std::string Name;                // "Cedrus_tree_var1"
    std::vector<std::string> Models; // the LOD 0's model paths, project-relative, sorted
    glm::vec3 Centre{0.0f};          // the bounding sphere in the group's own space (scale 1)
    float Radius = 0.0f;
    glm::vec3 BoundsMin{0.0f}, BoundsMax{0.0f}; // the LOD 0's box in the same space (the card's outline)
    float WindBend = 0.0f;   // the card's lean: the tree's WindStrength, metres at WindHeight (WindCommon.glsl's WindMainLean)
    float WindHeight = 1.0f;
    float WindFrequency = 1.0f; // the tree's sway rate, so the card rocks in step with its mesh
    int Frames = 8;          // frames per atlas side
    std::vector<glm::vec2> FrameExtents; // each frame's half-extents on its image plane (frame j x Frames + i)
    std::string Albedo, NormalDepth, Mask; // project-relative atlas paths
};

namespace ImpostorLibrary {
// The metres beyond which a matched LOD group draws its impostor: the sets' startDistance (40 m, Jacob's pick), or
// TARTARUS_IMPOSTOR_DISTANCE.
float StartDistance();
// Some set loaded and TARTARUS_NO_IMPOSTORS off: LOD groups are matched to their sets. Loads the sets on first use.
bool Available();
// Available(), Project Settings > Quality > Impostors on, and the renderer's atlases loaded (SetDrawable): matched
// groups draw their impostors. Until then they stay on their levels.
bool Enabled();
// Available() and the Quality toggle on: the renderer should get its atlases ready.
bool Wanted();
// The renderer has every set's atlases up (ImpostorRenderer).
void SetDrawable(bool drawable);
const std::vector<ImpostorSet>& Sets();
// The set drawn for a LOD group whose LOD 0 draws `lod0` (its renderers' models, any order), or -1. Cached per model
// list, so a full LOD pass over thousands of groups stays cheap.
int Match(const std::vector<const Model*>& lod0);
// Forgets the loaded sets and the match cache (a set added or re-baked while the editor runs).
void Reload();
// Unit tests: every LOD group matches set `set` (whatever its LOD 0 draws, models or none) with this start distance;
// set -1 ends it.
void TestOverride(int set, float startDistance);
}
