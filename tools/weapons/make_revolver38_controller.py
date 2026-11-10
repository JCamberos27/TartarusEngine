# Builds Revolver38.controller from the Remington 870's graph: the same states, priorities, fades, locomotion
# layers and 3P track, re-pointed at the .38 Special's clips. The 870's pump and per-round tube reload go; the
# revolver reloads in one state (the crane swings out, the ejector throws all six, two rounds at a time go in).
#
#   python tools/weapons/make_revolver38_controller.py
#
# Event times come from the source .blend's weapon actions (frames at 60 fps; tools/weapons/export_revolver38.py
# writes the eject timeline to export_manifest.json):
#   Reload 0..372   crane out f14, ejector f52-62, six rounds clear the chambers f58 (Eject x6: the engine throws a
#                   case per chamber, eject.origins), pairs seated f132 / f210 / f288, crane shut f316-322 (Refill)
#   MagCheck 0..214 crane out f14, shut f156
#   Inspect 0..374  hammer cocked f138, lowered f182, crane out f232, cylinder spun f280, crane shut f320
# Sound events name the snd.revolver38.* sets (assets/Audio/Weapons/Revolver38); until recordings are supplied
# they resolve to no files and play nothing.
import copy
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "project/assets/Weapons/Remington870/Remington870.controller")
OUT = os.path.join(REPO, "project/assets/Weapons/Revolver38/Revolver38.controller")
BASE = "assets/Weapons/Revolver38"
CAPACITY = 6
DROP = {"Pump", "ReloadStart", "ReloadStartEmpty", "ReloadLoop", "ReloadLoopEnd", "ReloadEnd"}
DROP_PARAMS = {"Cycle", "LastRound", "StopReload"}

# state -> clip name (arms / 3P), weapon clip
CLIPS = {
    "Idle": ("Idle", "Idle"), "Walk": ("Idle", "Idle"), "Sprint": ("Idle", "Idle"), "Aim": ("Aim", "Idle"),
    "IdleToSprint": ("Idle", "Idle"), "SprintToIdle": ("Idle", "Idle"), "Regrip": ("Regrip", "Idle"),
    "Fire": ("Fire", "Fire"), "Inspect": ("Inspect", "Inspect"), "MagCheck": ("Mag_Check", "Mag_Check"),
    "Melee": ("Melee", "Idle"), "Draw": ("Draw", "Idle"), "Holster": ("Holster", "Idle"),
    "Holstered": ("Holster", "Idle"), "Reload": ("Reload", "Reload"),
}
# Weapon-locomotion layers use the arms' own locomotion clips.
LOCO = {"Idle": "Idle", "Walk": "Walk", "Sprint": "Sprint", "IdleToSprint": "IdleToSprint",
        "SprintToIdle": "SprintToIdle", "Off": "Idle"}


def t(frame, length):
    return round(frame / length, 4)


S = "snd.revolver38."
EVENTS = {
    "Fire": [{"name": "Shot", "time": t(7, 46)}],  # the hammer falls (export_revolver38.py F_FALL); the clip is 0..46
    "Reload": [
        {"name": S + "cloth", "time": 0.0},
        {"name": S + "crane_open", "time": t(14, 372)},
        {"name": S + "ejector", "time": t(52, 372)},
    ] + [{"name": "Eject", "time": t(58, 372)} for _ in range(CAPACITY)] + [
        {"name": S + "round_insert", "time": t(126, 372)},
        {"name": S + "round_insert", "time": t(204, 372)},
        {"name": S + "round_insert", "time": t(282, 372)},
        {"name": S + "crane_close", "time": t(316, 372)},
        {"name": "Refill", "time": t(322, 372)},
        {"name": S + "cloth", "time": t(340, 372)},
    ],
    "MagCheck": [
        {"name": S + "cloth", "time": 0.02},
        {"name": S + "crane_open", "time": t(14, 214)},
        {"name": S + "crane_close", "time": t(156, 214)},
        {"name": S + "cloth", "time": t(180, 214)},
    ],
    "Inspect": [
        {"name": S + "cloth", "time": 0.02},
        {"name": S + "hammer_cock", "time": t(138, 374)},
        {"name": S + "hammer_decock", "time": t(182, 374)},
        {"name": S + "crane_open", "time": t(232, 374)},
        {"name": S + "cylinder_spin", "time": t(280, 374)},
        {"name": S + "crane_close", "time": t(320, 374)},
        {"name": S + "cloth", "time": t(350, 374)},
    ],
    "Draw": [{"name": S + "draw", "time": 0.08}],
    "Holster": [{"name": S + "holster", "time": 0.3}],
    "Melee": [{"name": S + "melee_swing", "time": 0.0}, {"name": S + "melee_hit", "time": t(8, 69)},
              {"name": S + "cloth", "time": t(18, 69)}],
    "Regrip": [{"name": S + "cloth", "time": 0.03}, {"name": S + "handle", "time": t(28, 44)}],
}


def guid(path):
    with open(os.path.join(REPO, "project", path + ".meta"), encoding="utf-8-sig") as fh:
        return json.load(fh)["guid"]


def clip(kind, name):
    folder, tag = {"arms": ("FirstPerson", "FP"), "weapon": ("Weapon", "W"), "arms3p": ("ThirdPerson", "3P")}[kind]
    path = "%s/%s/Revolver38_A_%s_%s.fbx" % (BASE, folder, tag, name)
    return {"clip": path, "clipGuid": guid(path)}


def repoint(state, arms, weapon):
    m = state.get("motions") or {}
    for kind in list(m):
        m[kind] = clip(kind, weapon if kind == "weapon" else arms)
    state["motions"] = m


with open(SRC, encoding="utf-8-sig") as fh:
    ctrl = json.load(fh)

base = ctrl["layers"][0]
template = next(s for s in base["states"] if s["name"] == "ReloadStartEmpty")
pos = template.get("position")
base["states"] = [s for s in base["states"] if s["name"] not in DROP]
reload_state = copy.deepcopy(template)
reload_state.update({"name": "Reload", "tags": ["Reload", "ADSCarry"], "priority": 3, "speed": 1.0, "loop": False,
                     "position": pos})
base["states"].append(reload_state)
for s in base["states"]:
    arms, weapon = CLIPS[s["name"]]
    repoint(s, arms, weapon)
    if s["name"] == "Fire":
        # Snappy (user, 2026-11-09: the hip shot read slow): 1.6x - the round leaves 0.073 s after the trigger, the kick
        # peaks ~0.13 s and the clip is done in 0.48 s. The cylinder still turns over ~4 frames (15 deg a frame at 60 fps).
        s["speed"] = 1.6
    if s["name"] in EVENTS:
        s["events"] = copy.deepcopy(EVENTS[s["name"]])
    elif "events" in s:
        s["events"] = [e for e in s["events"] if not e["name"].startswith("snd.")]

keep = []
for tr in base["transitions"]:
    if tr.get("to") in DROP or tr.get("from") in DROP:
        continue
    if tr.get("fromAny") and tr.get("to") == "Fire":
        # A cut, not a crossfade: the Fire clip starts the cylinder one chamber back (+60, which looks the same
        # as 0) and turns it to 0. A blend in would swing it back those 60 degrees in sight first.
        tr["duration"] = 0.0
    keep.append(tr)
keep.append({"canTransitionToSelf": False, "conditions": [{"mode": "if", "param": "Reload", "threshold": 0.0},
                                                         {"mode": "less", "param": "Ammo", "threshold": CAPACITY - 0.5}],
             "duration": 0.05, "exitTime": 1.0, "fromAny": True, "hasExitTime": False, "respectPriority": True,
             "to": "Reload"})
keep.append({"conditions": [], "duration": 0.25, "exitTime": 1.0, "from": "Reload", "hasExitTime": True, "to": "Exit"})
base["transitions"] = keep

for layer in ctrl["layers"][1:]:
    # The locomotion layers keep the 870's choice of clip per state (its Weapon Locomotion "Walk" plays Idle;
    # the walk itself is the Weapon Walk layer's): the same clip name, the revolver's file.
    for s in layer["states"]:
        src = (s.get("motions") or {}).get("arms", {}).get("clip", "")
        name = src.rsplit("_A_FP_", 1)[-1][:-4] if "_A_FP_" in src else LOCO.get(s["name"], "Idle")
        repoint(s, name, "Idle")

ctrl["parameters"] = [p for p in ctrl["parameters"] if p["name"] not in DROP_PARAMS]
for p in ctrl["parameters"]:
    if p["name"] == "Ammo":
        p["default"] = float(CAPACITY)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as fh:
    json.dump(ctrl, fh, indent=2)
    fh.write("\n")
meta = OUT + ".meta"
if not os.path.exists(meta):
    import secrets
    with open(meta, "w", encoding="utf-8") as fh:
        json.dump({"guid": secrets.token_hex(8), "metaVersion": 1, "type": "animatorcontroller"}, fh, indent=2)
        fh.write("\n")
print("wrote", os.path.relpath(OUT, REPO), "states:", [s["name"] for s in base["states"]])
