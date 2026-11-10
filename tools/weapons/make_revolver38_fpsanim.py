# Builds Revolver38.fpsanim, Revolver38.recoil and Revolver38.camerashake from the Remington 870's, for the
# .38 Special's rig and a handgun's handling.
#
#   python tools/weapons/make_revolver38_fpsanim.py
#
# Rig: the revolver's Root has the 870 Main's rest orientation in Blender (+Y up, +Z to the muzzle), but the 870's
# shipped FBXs came from a different exporter whose root node is that frame turned 180 deg about Y (its muzzle sits at
# root -Z). This exporter keeps the bone frame, so engine root space IS the bone frame (+Z to the muzzle) and the mount
# is the 870's [-90, 90, 90] composed with that half turn: [90, 0, 0] (engine Euler YXZ). Measured in Play: with the
# 870's mount the gun pointed back at the camera. The zeroed 0.64 cm bind translation becomes the mount offset (the
# 870's 1.6 cm is -0.016; it lies along the same socket axis).
#   muzzle: barrel tip 11.4 cm ahead (+Z), bore axis 3.34 cm up (the Cylinder bone's axis).
#   eject: the six chamber positions and their throw, from export_manifest.json's ejectTimeline.
# Handling: no stock (stock lock off, no spare magazine); the ADS hand anchor follows the cylinder.
# Recoil: a .38 from a ~0.8 kg revolver - well under the 12-gauge's shove into the shoulder but a sharper flip
# about the wrist, settling faster. The 870's curve shapes are kept, scaled in value and time.
import copy
import json
import os
import secrets

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "project/assets/Weapons/Remington870")
DST = os.path.join(REPO, "project/assets/Weapons/Revolver38")
BASE = "assets/Weapons/Revolver38"


def load(path):
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


def save(path, data, kind):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
    meta = path + ".meta"
    if not os.path.exists(meta):
        with open(meta, "w", encoding="utf-8") as fh:
            json.dump({"guid": secrets.token_hex(8), "metaVersion": 1, "type": kind}, fh, indent=2)
            fh.write("\n")
    return load(meta)["guid"]


def curve(c, v=1.0, t=1.0):
    """[time, value, in tangent, out tangent] keys: values x v, times x t (tangents follow)."""
    return [[k[0] * t, k[1] * v, k[2] * v / t, k[3] * v / t] for k in c]


def scale_curves(obj, v, t):
    if isinstance(obj, list) and obj and isinstance(obj[0], list) and obj[0] and isinstance(obj[0][0], (int, float)):
        return curve(obj, v, t)
    if isinstance(obj, list):
        return [scale_curves(x, v, t) for x in obj]
    if isinstance(obj, dict):
        return {k: scale_curves(x, v, t) for k, x in obj.items()}
    return obj


def span(r, s):
    return [x * s for x in r]


# ---------------------------------------------------------------- .recoil
rec = load(os.path.join(SRC, "Remington870.recoil"))
r = rec["recoil"]
r.update({
    "pitch": [-6.0, -3.6],                      # the flip: most of a handgun's recoil (the 870's x1.3, spread +-25%)
    # Shot to shot (user, 2026-11-09: every kick read the same): the flip goes 1.5-4 deg left or right and rolls 2-5 deg,
    # the view is kicked aside up to 0.8 deg, and the AK's noise on top. Absolute, not off the 870's (now varied itself).
    "yaw": [-4.0, -1.5, 1.5, 4.0],
    "roll": [-5.0, -2.0, 2.0, 5.0],
    "kickRight": [-0.004, 0.004],
    "horizontalRecoil": [-0.8, 0.8],
    "noiseX": [-0.015, 0.015], "noiseY": [-0.015, 0.015], "noiseScalar": 0.5,
    "kickback": span(r["kickback"], 0.42),      # arms, not a shoulder, take the shove
    "kickUp": span(r["kickUp"], 1.3),
    "verticalRecoil": span(r["verticalRecoil"], 3.2),  # the view's climb per shot, ~2.9-4.2 deg
    # Sights up it read stiff (2026-11-09): the kick peaked at 0.05 s and was gone by 0.15 s. Slower smoothing and
    # damping: it peaks near 0.08 s and settles over ~0.35 s.
    # Then it read slow (user): the kick snaps up and back - stiff smoothing and damping, the curves only x1.3 in time.
    "damping": 10.0,
    "smoothLoc": [40.0, 40.0, 40.0],
    "smoothRot": [40.0, 40.0, 40.0],
    # ... and the gun wobbles after each shot (the AK's recoil sway, kept more of on the sights).
    "recoilSway": dict(r["recoilSway"], pitchSway=[-1.0, -1.2], yawSway=[-0.4, 0.7], rollMultiplier=1.0,
                       acceleration=12.0, damping=12.0, adsScale=0.5, pivotOffset=[0.0, 0.0, 0.2]),
    # It pivots at the wrist under the bore, not at a shoulder 20 cm back.
    "hipPivotOffset": [0.0, -0.04, -0.02],
    "aimPivotOffset": [0.0, -0.04, -0.02],
    # Sights up it's to kick like the hip shot (measured: the hip's Fire clip flips the muzzle 28 deg and the gun 45 mm, peak
    # at 0.2 s, back by ~0.5 s; the sights had 4.6 deg / 25 mm). The hip shot runs this profile too, on top of its clip, so
    # the extra is the sights' alone: the flip x6 (measured ~25 deg), the shove x1.6, the rise x2.3.
    # Then x3 (user, 2026-11-10): ~25 deg tipped the grip's underside into view - the gap between the hand and the frame
    # the idle hold leaves, hidden at rest. The hand doesn't slip on the gun (NpcTest kick trace: 0.0 mm); it's the angle.
    "aimRot": [3.0, 1.0, 1.0],
    "aimLoc": [1.6, 2.3, 1.6],
})
# The kick's shape over time is these curves (not the smoothing): at the 870's x0.8 it peaked at 0.03 s and was spent by
# 0.2 s, a blip on the sights. x3.0: the flip peaks near 0.12-0.15 s and comes back over ~0.55 s, as the hip shot does.
r["recoilCurves"] = scale_curves(r["recoilCurves"], 1.0, 1.3)  # x3.0 then read slow: snappier
recoil_guid = save(os.path.join(DST, "Revolver38.recoil"), rec, "recoil")

# ---------------------------------------------------------------- .camerashake
shake = load(os.path.join(SRC, "Remington870.camerashake"))
# Sights up the first pass (rotation x0.6, x0.65 on the sights: ~0.25 deg for 0.22 s) read as no kick at all.
shake["duration"] = round(shake["duration"], 3)
shake["rotationCurves"] = [curve(c, 1.5) for c in shake["rotationCurves"]]
shake["positionCurves"] = [curve(c, 0.8) for c in shake["positionCurves"]]
shake["adsScale"] = 1.0
shake_guid = save(os.path.join(DST, "Revolver38.camerashake"), shake, "camerashake")

# ---------------------------------------------------------------- .fpsanim
fp = load(os.path.join(SRC, "Remington870.fpsanim"))
manifest = load(os.path.join(DST, "export_manifest.json"))
to_engine = lambda v: [round(v[0], 4), round(v[1], 4), round(v[2], 4)]
chambers = [to_engine(e["origin"]) for e in manifest["ejectTimeline"]]
throw = [sum(to_engine(e["velocity"])[i] for e in manifest["ejectTimeline"]) / len(chambers) for i in range(3)]

fp["actionCamera"]["tags"] = ["Reload", "Busy"]
fp["cameraShakeProfile"], fp["cameraShakeProfileGuid"] = BASE + "/Revolver38.camerashake", shake_guid
fp["recoilProfile"], fp["recoilProfileGuid"] = BASE + "/Revolver38.recoil", recoil_guid
fp["hipFireClip"] = True  # hip shots play the DA Fire clip (trigger, hammer, cylinder); sights-up stays procedural
fp["controller"] = BASE + "/Revolver38.controller"
fp["ads"]["gunMotion"] = {"MagCheck": {"rotation": 0.6, "position": 0.2}, "Reload": {"rotation": 1.0, "position": 1.0}}
# Far: the reload's free hand stays with the gun (on the body, as the 870's shell pouch, it went into the belly in 3P).
fp["ads"]["handAnchor"] = {"near": 0.05, "far": 0.6, "bones": ["Cylinder"]}
fp["eject"] = {
    "enabled": True,
    "model": BASE + "/Casing_38Special.fbx",
    "liveModel": BASE + "/Round_38Special.fbx",
    "material": BASE + "/Materials/Bullet38.mat",
    "origin": [round(sum(c[i] for c in chambers) / len(chambers), 4) for i in range(3)],
    "origins": chambers,
    "direction": [round(x, 4) for x in throw],
    # They slide out of the chambers under the ejector (~0.5 m/s, muzzle up) and fall: no port throw.
    "speed": 0.6, "speedJitter": 0.3, "spread": 12.0, "spin": 8.0,
    "trigger": "event",
}
pr = fp["procedural"]["recoil"]
pr["_comment"] = (".38 Special revolver: a light gun with a high bore axis - a sharp flip about the wrist and a short "
                  "shove into the arms, a fraction of the 12-gauge's push, settled in about a third of a second. The 870's "
                  "curves, lighter and quicker. Double action, semi only: no burst wander or growth.")
pr["duration"] = 0.36
pr["rotation"] = scale_curves(pr["rotation"], 0.8, 0.8)
pr["position"] = scale_curves(pr["position"], 0.45, 0.8)
pr["cameraPitch"] = curve(pr["cameraPitch"], 0.75, 0.8)
pr["cameraYaw"] = curve(pr["cameraYaw"], 0.7, 0.8)
pr["pivot"] = [0.0, -0.04, 0.02]
pr["shakeAmount"], pr["fovPunch"] = 0.5, 0.7
fp["stockLock"]["enabled"] = False
# Sway with the sights up: the 870's turns it off there (a shouldered long gun); a handgun held out at arm's length
# moves with the view and the step. The AK's sights-up levels.
fp["procedural"]["sway"]["aimingSway"]["adsScale"] = 0.51
fp["procedural"]["sway"]["movementSway"]["adsScale"] = 0.18
fp["viewRotation"] = fp.get("viewRotation", [0.0, 180.0, 0.0])
fp["weaponModel"] = BASE + "/Weapon/Revolver38_A_W_Idle.fbx"
fp["weaponMaterials"] = {"Revolver": BASE + "/Materials/Revolver38.mat", "Bullet": BASE + "/Materials/Bullet38.mat"}
fp["weaponRoot"] = "Root"
fp["weaponMountRotation"] = [90.0, 0.0, 0.0]
fp["weaponMountOffset"] = [0.0, 0.0, -0.0064]
fp["spareMagazine"] = {"bones": [], "grabDistance": 0.21}
fp["muzzle"] = {"auto": False, "origin": [0.0, 0.0334, 0.114], "direction": [0.0, 0.0, 1.0]}
save(os.path.join(DST, "Revolver38.fpsanim"), fp, "firstpersonanimationset")
print("wrote Revolver38.fpsanim / .recoil / .camerashake; eject origins", chambers, "throw", [round(x, 3) for x in throw])
