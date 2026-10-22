# Writes tests/smoke-scenes/smoke_decals.json, the clustered-decal smoke scene (docs/DECALS.md).
#   python tools/gen_smoke_decals.py tests/smoke-scenes/smoke_decals.json
import json, sys
A = "assets/Test/Decals/DA_Test_Grid.decal"
def decal(i, name, pos, rot=(0,0,0), size=(0.5,0.5,0.25), seed=0, sort=0, static=True, dynamic=True, jit=0.0):
    return {"name": name, "id": i, "parentId": -1, "order": i, "position": list(pos), "rotation": list(rot), "scale": [1,1,1],
            "Decal": {"Asset": A, "Size": list(size), "Seed": seed, "Sort Order": sort, "Affect Static": static,
                      "Affect Dynamic": dynamic, "Rotation Jitter": jit, "Scale Jitter": 0.15 if jit else 0.0, "Random Flip": bool(jit)}}
scene = {
  "formatVersion": 2,
  "_comment": ("Engine-owned smoke scene for clustered decals (docs/DECALS.md). Receivers: a static wall, a rigidbody "
               "(dynamic) box, a transmissive sphere (transparent pass). 300 overlapping, seed-jittered decals tile the wall; "
               "a stack of 270 decals on one spot exceeds the 256-per-cluster cap, so the cull must flag overflow bit 1 and "
               "never bit 2 (index list full). Textures are the committed prototype grids only, so CI needs no Drive assets. "
               "No skinned receiver: no skinned model is in git (smoke_min's pre-skin self-test covers the skinning shader). "
               "Regenerate with tools/gen_smoke_decals.py; checked by the smoke_decals block in main.cpp."),
  "exposureEV": 0.0, "tonemapOperator": 1, "msaaSamples": 4, "ssaoEnabled": False, "bloomEnabled": False,
  "bloomThreshold": 1.0, "bloomKnee": 0.5, "bloomIntensity": 0.25,
  "shadowsEnabled": True, "shadowResolution": 2048, "shadowCascades": 4, "shadowDistance": 100.0,
  "skyHorizonColor": [0.42, 0.50, 0.62], "skyZenithColor": [0.12, 0.24, 0.52], "skyAmbientIntensity": 1.0,
  "assetFolders": ["Scenes"], "assetMeta": [],
  "boxes": [
    {"center": [0, -0.1, 0], "size": [24, 0.2, 24], "color": [0.34, 0.34, 0.37], "rotation": [0,0,0], "name": "Ground", "id": 0, "parentId": -1, "order": 0},
    {"center": [0, 3, -3], "size": [12, 6, 0.2], "color": [0.6, 0.6, 0.6], "rotation": [0,0,0], "name": "Wall", "id": 1, "parentId": -1, "order": 1},
    {"center": [-3, 0.5, 1], "size": [1, 1, 1], "color": [0.5, 0.55, 0.6], "rotation": [0,0,0], "name": "DynamicBox", "id": 2, "parentId": -1, "order": 2,
     "collider": {"isTrigger": False, "friction": 1.0}, "Rigidbody": {}},
  ],
  "empties": [
    {"name": "Sun", "id": 3, "parentId": -1, "order": 3, "position": [6, 10, 6], "rotation": [52, -36, 0], "scale": [1,1,1],
     "light": {"kind": "directional", "color": [1.0, 0.97, 0.92], "intensity": 4.5, "range": 40.0, "angularSize": 1.0, "castShadows": True,
               "shadow": {"enabled": True, "bias": 1.0, "normalBias": 1.0, "softness": 0.3, "nearPlane": 0.05, "resolution": 0, "updateMode": 0}}},
    {"name": "Decal Grading", "id": 4, "parentId": -1, "order": 4, "position": [0,0,0], "rotation": [0,0,0], "scale": [1,1,1],
     "Decal Grading": {"Age": 0.25, "Wear": 0.25}},
  ],
  "models": [
    {"path": "primitive://sphere#5", "name": "TransparentSphere", "id": 5, "parentId": -1, "order": 5,
     "position": [3, 1.0, 1], "rotation": [0,0,0], "scale": [0.8,0.8,0.8],
     "materials": [{"path": "assets/Materials/Test/mat_transmission.mat"}]},
  ],
}
nid = 10
# 300 overlapping decals tiling the wall's front face (z = -2.9): 20 x 15, 0.45 m apart, 0.6 m wide.
for r in range(15):
    for c in range(20):
        x = -4.3 + c * 0.45; y = 0.4 + r * 0.38
        scene["empties"].append(decal(nid, f"WallDecal_{r:02d}_{c:02d}", (x, y, -2.9), seed=nid, sort=(r + c) % 4, size=(0.6, 0.6, 0.25), jit=25.0))
        nid += 1
# 270 decals on one spot (more than the 256 per-cluster cap).
for k in range(270):
    scene["empties"].append(decal(nid, f"OverCap_{k:03d}", (4.8, 5.4, -2.9), seed=nid, sort=k % 8, size=(0.3, 0.3, 0.25)))
    nid += 1
# Dynamic receiver: only moving things (the rigidbody box's front face, z = 1.5).
scene["empties"].append(decal(nid, "DynamicOnly", (-3, 0.5, 1.5), size=(0.8, 0.8, 0.3), static=False, dynamic=True)); nid += 1
# Static-only decal over the same box: must not show on it.
scene["empties"].append(decal(nid, "StaticOnlyOnDynamic", (-3, 0.5, 1.5), rot=(0,0,30), size=(0.8, 0.8, 0.3), static=True, dynamic=False)); nid += 1
# Transparent receiver: the transmissive sphere's front (z = 1.8).
scene["empties"].append(decal(nid, "OnTransparent", (3, 1.0, 1.8), size=(0.6, 0.6, 0.4))); nid += 1
# Floor decal facing up (+Z out of the surface = world +Y).
scene["empties"].append(decal(nid, "FloorDecal", (0, 0.0, 1), rot=(-90,0,0), size=(1.5, 1.5, 0.3))); nid += 1
with open(sys.argv[1], "w", newline="\n") as f:
    json.dump(scene, f, indent=1)
    f.write("\n")
print(nid - 10, "decals")
