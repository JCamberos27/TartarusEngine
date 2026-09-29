# Writes the Remington 870's textures, material and .meta sidecars (the shapes the editor writes
# for the AKS-74U's), after work/export_remington.py has produced the FBXs. Re-running keeps
# every GUID already issued.
#
#   python work/make_remington_assets.py
import json
import os
import secrets
import shutil

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
root = "project/assets/Weapons/Remington870"
SOURCE_TEXTURES = os.path.expanduser("~/Desktop/Remington_870_Textures")


def rel(p):
    return p.replace(os.sep, "/").split("project/", 1)[1]


def meta(path, obj):
    if os.path.exists(path + ".meta"):
        obj["guid"] = json.load(open(path + ".meta"))["guid"]
    obj.setdefault("guid", secrets.token_hex(8))
    obj["metaVersion"] = 1
    with open(path + ".meta", "w", newline="\n") as f:
        json.dump(dict(sorted(obj.items())), f, indent=2)
        f.write("\n")
    return obj["guid"]


os.makedirs(root + "/Textures", exist_ok=True)
os.makedirs(root + "/Materials", exist_ok=True)
# name -> (textureType: 0 default / 1 normal map, sRGB)
tex = {"BaseColor": (0, True), "Metallic": (0, False), "Roughness": (0, False), "AO": (0, False), "Normal": (1, False)}
guids = {}
for k, (tt, srgb) in tex.items():
    dst = "%s/Textures/Remington870_%s.png" % (root, k)
    src = os.path.join(SOURCE_TEXTURES, "Remington_870_%s.png" % k)
    if os.path.exists(src):
        shutil.copyfile(src, dst)
    guids[k] = meta(dst, {"folder": rel(root + "/Textures"), "type": "texture",
                          "importer": {"anisoLevel": 16, "compression": 2, "filterMode": 2, "generateMipmaps": True,
                                       "isSRGB": srgb, "maxTextureSize": 4096, "textureType": tt, "wrapMode": 0}})


def T(k):
    return "assets/Weapons/Remington870/Textures/Remington870_%s.png" % k


mat = {"factorsScaleMaps": True, "matVersion": 2, "name": "Remington 870", "properties": {
    "_AOMap": T("AO"), "_AlbedoMap": T("BaseColor"), "_BaseColor": [1.0, 1.0, 1.0], "_DoubleSided": True,
    "_EmissiveColor": [0.0, 0.0, 0.0], "_EmissiveMap": "", "_EmissiveStrength": 1.0,
    "_Metallic": 1.0, "_MetallicMap": T("Metallic"), "_MetallicRoughnessMap": "", "_NormalMap": T("Normal"),
    "_Roughness": 1.0, "_RoughnessMap": T("Roughness"), "_Triplanar": False, "_TriplanarScale": 1.0},
    "shader": "engine://Standard.shader",
    "textureGuids": {"_AOMap": guids["AO"], "_AlbedoMap": guids["BaseColor"], "_MetallicMap": guids["Metallic"],
                     "_NormalMap": guids["Normal"], "_RoughnessMap": guids["Roughness"]}}
mp = root + "/Materials/Remington870.mat"
with open(mp, "w", newline="\n") as f:
    json.dump(mat, f, indent=2)
    f.write("\n")
meta(mp, {"folder": rel(root + "/Materials"), "type": "material"})

fbx = {}
for sub in ("FirstPerson", "Weapon"):
    for fn in sorted(os.listdir("%s/%s" % (root, sub))):
        if not fn.endswith(".fbx"):
            continue
        m = {"folder": rel("%s/%s" % (root, sub)), "type": "model"}
        if fn == "Remington870_A_W_Idle.fbx":
            m["materialRemap"] = {"02___Default": "assets/Weapons/Remington870/Materials/Remington870.mat"}
        fbx["assets/Weapons/Remington870/%s/%s" % (sub, fn)] = meta("%s/%s/%s" % (root, sub, fn), m)
for f in ("Remington870.controller", "Remington870.fpsanim"):
    p = "%s/%s" % (root, f)
    if os.path.exists(p):
        meta(p, {"type": "animatorcontroller" if f.endswith(".controller") else "firstpersonanimationset"})
print(json.dumps(fbx, indent=1))
