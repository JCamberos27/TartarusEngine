#pragma once
#include <vector>

// The KriptoFX blood prefabs' setups (docs/BLOOD_FX.md), generated into BloodFxPresets.inc by
// tools/gen_blood_presets.py: which sims each prefab plays where, and the floor decal it leaves.
// Engine axes; matrices are 3x4 row-major into the prefab's own frame (spray toward +X, gravity -Y).

// A Unity AnimationCurve: Hermite segments between keys {time, value, inSlope, outSlope}.
struct BloodCurve {
    int Count = 0;
    float Keys[8][4] = {};
    float Eval(float t) const;
};

struct BloodSprayDef {
    const char* Sim;
    float M[12];        // sim space -> prefab
    float TimeLimit;    // seconds of playback at AnimationSpeed 1
    float FramesCount;  // frames played (the sim's frame count - 1)
};

// BFX_DecalSettings: the decal box sits on the ground under the spray; how far the blood fell
// (prefab root height above the ground, over TimeHeightMax) stretches and slides it and delays it.
struct BloodDecalDef {
    const char* Set;    // decal texture set (BloodRenderer's atlas)
    float Parent[12];   // the box's parent -> prefab
    float Rot[4];       // local rotation xyzw
    float Pos[3];       // local position
    float Scale[3];     // local scale: x/z the footprint, y the projection depth
    float HeightMax, HeightMin;
    float ScaleMin[3], ScaleMax[3];
    float OffsetMin[3], OffsetMax[3];
    BloodCurve DelayByHeight; // seconds (at AnimationSpeed 1) vs fall height / HeightMax
    float RevealSeconds;      // GraphTimeMultiplier: the Reveal curve's time scale
    BloodCurve Reveal;        // mask cutout over time / RevealSeconds: 1 hidden .. 0 fully spread
};

struct BloodPresetDef {
    const char* Name;
    float RootScale;
    float AnimationSpeed;
    std::vector<BloodSprayDef> Sprays;
    std::vector<BloodDecalDef> Decals;
};

const std::vector<BloodPresetDef>& BloodPresets();
const BloodPresetDef* FindBloodPreset(const char* name); // null if unknown
