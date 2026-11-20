// The bone palette blend for a vertex's 8 influences (MAX_BONE_INFLUENCE): the includer declares the vertex's
// aBoneIDs / aBoneIDs2 / aWeights / aWeights2 and the uBones palette. One copy, so every program that skins a vertex
// (the lit pass, its depth pre-pass, the outline, fur, shadows) lands it in the same place to the bit.
mat4 SkinPalette() {
    int boneIds[8] = int[8](aBoneIDs.x, aBoneIDs.y, aBoneIDs.z, aBoneIDs.w, aBoneIDs2.x, aBoneIDs2.y, aBoneIDs2.z, aBoneIDs2.w);
    float boneWeights[8] = float[8](aWeights.x, aWeights.y, aWeights.z, aWeights.w, aWeights2.x, aWeights2.y, aWeights2.z, aWeights2.w);
    mat4 skinMat = mat4(0.0);
    float totalWeight = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (boneIds[i] >= 0) {
            skinMat += uBones[clamp(boneIds[i], 0, uBones.length() - 1)] * boneWeights[i]; // never out of range (#98)
            totalWeight += boneWeights[i];
        }
    }
    return totalWeight <= 0.0001 ? mat4(1.0) : skinMat;
}
