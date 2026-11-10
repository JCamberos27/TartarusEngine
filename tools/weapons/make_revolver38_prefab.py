# Builds Revolver38.prefab from the Remington 870's: the same muzzle rig (flash, core, smoke empties, grip),
# re-pointed at the .38 Special's model, controller and animation set, with a revolver's gameplay.
#
#   python tools/weapons/make_revolver38_prefab.py
#
# Muzzle Flash sits at the barrel tip in the model's space (prefab +Z is the muzzle; the 870's is at z 0.63 m).
# Gameplay: six rounds, double action (semi only), magazine-style reload (one Refill when the crane shuts).
# Per hit it out-damages the 5.45 mm rifle at close range and falls off sooner; the numbers are a first pass
# for balance, not measurements.
import copy
import json
import os
import secrets

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "project/assets/Weapons/Remington870/Remington870.prefab")
DST = os.path.join(REPO, "project/assets/Weapons/Revolver38/Revolver38.prefab")
BASE = "assets/Weapons/Revolver38"


def guid(rel):
    with open(os.path.join(REPO, "project", rel + ".meta"), encoding="utf-8-sig") as fh:
        return json.load(fh)["guid"]


with open(SRC, encoding="utf-8-sig") as fh:
    prefab = json.load(fh)

model = prefab["models"][0]
model["name"] = ".38 Special Revolver"
model["path"] = BASE + "/Weapon/Revolver38_A_W_Idle.fbx"
model["pathGuid"] = guid(model["path"])
model["Animator Controller"]["Controller"] = {"path": BASE + "/Revolver38.controller",
                                              "pathGuid": guid(BASE + "/Revolver38.controller")}
wd = model["Weapon Definition"]
for k in ("Pellets", "Spread Hip", "Spread Ads", "Cycle After Shot", "Cycle Delay", "Reload"):
    wd.pop(k, None)
wd.update({
    "Description": ".38 Special double-action revolver. 6 rounds, semi only; the crane swings out and the "
                   "ejector dumps all six, reloaded two at a time.",
    "Animation Set": {"path": BASE + "/Revolver38.fpsanim", "pathGuid": guid(BASE + "/Revolver38.fpsanim")},
    "Magazine": 6,
    "Rounds Per Minute": 200.0,   # a double-action pull: about three aimed shots a second
    "Allow Full Auto": False,
    "Burst Rounds": 0,
    "Damage": 46.0,
    "Head Multiplier": 2.5,
    "Limb Multiplier": 0.75,
    "Falloff Start": 12.0,
    "Falloff End": 35.0,
    "Falloff Min": 0.45,
    "Impact Impulse": 7.0,
    "Impact Max Speed": 7.0,
    "Zero Distance": 15.0,
    "Bullet Hole Radius": 0.0045,  # .357 in bore
    # Sight line measured in Play (Editor.log, headless revolver test 2026-11-09, after SIGHT_CORRECTION):
    # the eye with the sights up, weapon-root space. Rounds cross it at Zero Distance.
    "Has Sight Line": True,
    "Sight Origin": [-0.00016, 0.08350, -0.37688],
    "Sight Direction": [0.000154, -0.045134, 0.998981],
})
# The muzzle as the AK's (user, 2026-11-09: the 870's 12-gauge burst - flash and core ~3x the AK's, thick smoke - read
# huge on a handgun): its flash, light and smoke settings, and its Muzzle Flash / Core / Smoke particles below.
with open(os.path.join(REPO, "project/assets/Weapons/AKS74U/AKS74U.prefab"), encoding="utf-8-sig") as fh:
    ak = json.load(fh)
ak_wd = ak["models"][0]["Weapon Definition"]
for k in ("Flash Time", "Light Intensity", "Light Range", "Light Color", "Player Flash Scale", "Smoke Alpha",
          "Smoke Lifetime Min", "Smoke Lifetime Max", "Smoke Scale"):
    if k in ak_wd:
        wd[k] = copy.deepcopy(ak_wd[k])
ak_fx = {e["name"]: e for e in ak["empties"]}
for e in prefab["empties"]:
    if e["name"] == "Muzzle Flash":
        e["position"] = [0.0, 0.0334, 0.114]
    if e["name"] in ("Muzzle Flash", "Muzzle Core", "Muzzle Smoke") and "Particle System" in ak_fx.get(e["name"], {}):
        e["Particle System"] = copy.deepcopy(ak_fx[e["name"]]["Particle System"])
    if "Muzzle Attachment" in e:
        e["Muzzle Attachment"]["Firing Sound Profile"] = "38"

with open(DST, "w", encoding="utf-8") as fh:
    json.dump(prefab, fh, indent=2)
    fh.write("\n")
if not os.path.exists(DST + ".meta"):
    with open(DST + ".meta", "w", encoding="utf-8") as fh:
        json.dump({"guid": secrets.token_hex(8), "metaVersion": 1, "type": "prefab"}, fh, indent=2)
        fh.write("\n")
print("wrote", os.path.relpath(DST, REPO))
