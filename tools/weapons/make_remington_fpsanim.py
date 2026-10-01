# Writes Remington870.fpsanim from the AKS-74U's weapon definition (its tuned ADS, bob and recoil
# shape), with the Remington's rigs, measured mount and muzzle, and 12-gauge gameplay.
#
#   python tools/weapons/make_remington_fpsanim.py
#
# Measured, not guessed:
#  - weaponMountRotation / weaponMountOffset: build/probes/socket_probe.exe (Quantum_Arms_FP + each clip,
#    weapon root Main): socket -> Main is t = (0, 0, -0.016) m and one fixed turn in every clip that
#    has a weapon track (its Y-X-Z Euler reads (-90, Y, Z) with Y + Z = 180: gimbal lock, the same
#    rotation - written as (-90, 90, 90)).
#  - muzzle: the barrel's front face in Main's space, from the .blend's mesh (Main's -Z is the
#    bore): the muzzle ring spans y 1.97..5.30 cm, x +-1.29 cm at z = -63.09 cm.
# Only the sight line is left to Play: aim, hold still ~2 s, Save Measured Sight Line.
import copy
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AK = os.path.join(REPO, "project/assets/Weapons/AKS74U/AKS74U.fpsanim")
OUT = os.path.join(REPO, "project/assets/Weapons/Remington870/Remington870.fpsanim")
BASE = "assets/Weapons/Remington870"

ak = json.load(open(AK))
d = copy.deepcopy(ak)
d["controller"] = BASE + "/Remington870.controller"
d["weaponModel"] = BASE + "/Weapon/Remington870_A_W_Idle.fbx"
d["weaponMaterials"] = {"02___Default": BASE + "/Materials/Remington870.mat"}
d["weaponRoot"] = "Main"
d["weaponSocket"] = "ik_hand_gun"
d["weaponMountRotation"] = [-90.0, 90.0, 90.0]
d["weaponMountOffset"] = [0.0, 0.0, -0.016]
d["viewRotation"] = [0.0, 180.0, 0.0]
d["spareMagazine"] = {"bones": [], "grabDistance": 0.21}
d["muzzle"] = {"auto": False, "origin": [0.0, 0.0363, -0.6309], "direction": [0.0, 0.0, -1.0]}
# The spent hull (tools/weapons/extract_casings.py) out of the ejection port on the right of the receiver
# (the .blend's port window: 4.5 cm behind Main, at bore height) when the pump is worked - the
# controller's Eject event - thrown right, up and a little back. Main's +X is the gun's right.
d["eject"] = {"model": BASE + "/Shell_12ga.fbx", "material": BASE + "/Materials/Remington870.mat",
              "origin": [0.02, 0.034, -0.062], "direction": [1.0, 0.4, 0.15], "speed": 2.5,
              "speedJitter": 0.15, "spread": 8.0, "spin": 15.0, "trigger": "event"}
d["laser"] = {"enabled": False, "color": [1.0, 0.0227, 0.0136], "beamBrightness": 1.1, "spotBrightness": 9.0}

g = d["gameplay"]
g.pop("sightLine", None)  # this gun's own is measured in Play
g.update({
    "magazine": 6,             # 5 in the tube + 1 in the chamber, as the game counts it
    "rpm": 70.0,               # unused semi-only; the pump sets the pace
    "allowFullAuto": False,
    "pellets": 8,              # 00 buckshot
    "spread": {"hip": 3.5, "ads": 2.2},
    "reload": "perRound",
    "cycle": {"enabled": True, "delay": 0.12},
    "impactImpulse": 14.0,     # the whole round's, shared by its pellets
    "impactMaxSpeed": 9.0,
    "bulletHoleRadius": 0.007,
    "zeroDistance": 15.0,
})

a = d["ads"]
a["zoom"] = 1.2
# The shell comes off the belt: keyed relative to the gun, with the gun on the sights that spot
# would swing up in front of the face. Anchored, the hand fetches it from where it is at the hip.
a["handAnchor"] = {"near": 0.05, "far": 0.15, "bones": ["Shell"]}
# The reloads keep all of the clip's roll about the sights: the shells go in from underneath, so
# held level on the sights the left hand works hidden behind the right. Rolled as at the hip, the
# loading port and the hand come into view (the chain keeps the start's roll through the loop).
a["gunMotion"] = {
    "Pump": {"rotation": 0.35, "position": 0.4},
    "MagCheck": {"rotation": 0.6, "position": 0.2},
    "ReloadStart": {"rotation": 1.0, "position": 1.0},
    "ReloadStartEmpty": {"rotation": 1.0, "position": 1.0},
    "ReloadLoop": {"rotation": 1.0, "position": 1.0},
    "ReloadLoopEnd": {"rotation": 1.0, "position": 1.0},
    "ReloadEnd": {"rotation": 1.0, "position": 1.0},
}


# The world gun (split poses): first person shows the animations' own pose; every other view (the
# Scene view, other players, the shadow) shows the gun with its butt in the body's right shoulder pocket
# while shouldered, and kept clear of the neck and head (a hood) always - sprinting included. The rig
# holds the stock in by the chin, which on the body went through the neck and hood (--stock-probe).
d["stockLock"] = {"enabled": True, "tags": ["Idle", "Ready", "ADS", "Cycling"], "pocket": [-0.045, 0.03, 0.05],
                  "maxShift": 0.3, "headTilt": 25.0, "blendTime": 0.2,
                  "neckRadius": 0.09, "headRadius": 0.14, "gunLength": 0.45, "meshClearance": 0.05,
                  "releaseStart": -40.0, "releaseEnd": -70.0}


def scale_curve(keys, k):
    return [[t, v * k, i * k, o * k] for t, v, i, o in keys]


r = d["procedural"]["recoil"]
r["_comment"] = ("Remington 870, 12-gauge: one heavy shove straight back into the shoulder and a big muzzle "
                 "rise, slower to settle than the AK (the AK-74U curves, scaled up and stretched). Semi only, so "
                 "no burst wander or growth; most of the climb comes back on its own (aimRecovery). The pump is "
                 "the Pump clip (no procedural bolt).")
r["duration"] = 0.45
r["rotation"]["x"] = scale_curve(r["rotation"]["x"], 3.0)  # muzzle rise
r["rotation"]["y"] = scale_curve(r["rotation"]["y"], 1.6)
r["rotation"]["z"] = scale_curve(r["rotation"]["z"], 2.2)
r["position"]["z"] = scale_curve(r["position"]["z"], 2.6)  # back into the shoulder
r["position"]["y"] = scale_curve(r["position"]["y"], 2.0)
r["cameraPitch"] = scale_curve(r["cameraPitch"], 3.0)
r["cameraYaw"] = scale_curve(r["cameraYaw"], 1.5)
r.update({
    "hipScale": 1.2, "adsScale": 0.95,
    "cameraScale": 1.5,  # the AK's view punch is 1.8; the 12-gauge's curves are already 3x its
    "kickSpread": 10.0, "kickBias": 3.0, "timeJitter": 0.1, "firstShotScale": 1.0,
    "wander": 0.0, "burstGrowth": 0.0, "burstGrowthMax": 1.0,
    "aimPitch": [0.9, 1.3], "aimYaw": [-0.2, 0.25], "aimRecovery": 0.7,
    "aimRecoveryDelay": 0.18, "aimRecoverySpeed": 6.0,
    "shakeAmount": 0.7, "shakeMax": [0.6, 0.45, 1.4], "shakeDecay": 3.2,
    "hipProcedural": True, "boltCycle": 0.0, "boltBone": "",
    "fovPunch": 1.2,
})

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump(d, f, indent=2)
    f.write("\n")
print("wrote", OUT)
