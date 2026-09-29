# Builds Remington870.controller from the AKS-74U's graph: the same states, priorities, fades and
# locomotion, re-pointed at the Remington's clips, with the AK's two magazine reloads replaced by a
# tube loaded a round at a time and a pump worked after every shot.
#
#   python work/make_remington_controller.py
#
# Per-round reload (gameplay.reload = perRound; the driver sets LastRound / StopReload):
#   Reload & Ammo < 0.5 -> ReloadStartEmpty (pumps a round into the chamber: LoadRound)
#   Reload & Ammo > 0.5 -> ReloadStart      (reaches for a shell)
#   then, at the end of each of those and of ReloadLoop, in this order:
#     StopReload -> ReloadEnd      (the trigger was pulled: put the hand back on the pump)
#     LastRound  -> ReloadLoopEnd  (load the last shell and finish)
#     otherwise  -> ReloadLoop     (load a shell: LoadRound, and reach for the next)
# Pump (gameplay.cycle; the driver sets Cycle after each round): Any State -> Pump, tagged Cycling.
# LoadRound times are where each clip's shell disappears into the gun (sampled from the .blend's
# Shell bone): ReloadLoop f42/70, ReloadLoopEnd f42/98, ReloadStartEmpty f120/160 (pump closed).
import copy
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AK = os.path.join(REPO, "project/assets/Weapons/AKS74U/AKS74U.controller")
OUT = os.path.join(REPO, "project/assets/Weapons/Remington870/Remington870.controller")
BASE = "assets/Weapons/Remington870"
MAGAZINE = 6


def guids():
    out = {}
    for sub in ("FirstPerson", "Weapon"):
        d = os.path.join(REPO, "project", BASE, sub)
        for f in os.listdir(d):
            if f.endswith(".fbx.meta"):
                out["%s/%s/%s" % (BASE, sub, f[:-5])] = json.load(open(os.path.join(d, f)))["guid"]
    return out


GUIDS = guids()


def motion(clip):
    path = "%s/%s" % (BASE, clip)
    if path not in GUIDS:
        raise SystemExit("no exported clip %s (run work/export_remington.py first)" % path)
    return {"clip": path, "clipGuid": GUIDS[path]}


def motions(arms, weapon="Idle"):
    return {"arms": motion("FirstPerson/Remington870_A_FP_%s.fbx" % arms),
            "weapon": motion("Weapon/Remington870_A_W_%s.fbx" % weapon)}


# AK state -> (arms clip, weapon clip). Every state gets a weapon clip: the Remington's rest pose
# leaves its loose shell in the port, A_W_Idle parks it.
CLIPS = {
    "Idle": ("Idle", "Idle"), "Walk": ("Walk", "Idle"), "Sprint": ("Sprint", "Idle"), "Aim": ("Aim", "Idle"),
    "IdleToSprint": ("IdleToSprint", "Idle"), "SprintToIdle": ("SprintToIdle", "Idle"), "Regrip": ("Regrip", "Idle"),
    "Fire": ("Fire", "Idle"), "Inspect": ("Inspect", "Inspect"), "MagCheck": ("Mag_Check", "Mag_Check"),
    "Melee": ("Melee", "Idle"), "Draw": ("Draw", "Idle"), "Holster": ("Holster", "Idle"), "Holstered": ("Holster", "Idle"),
}

ak = json.load(open(AK))
c = copy.deepcopy(ak)
for p in c["parameters"]:
    if p["name"] == "Ammo":
        p["default"] = float(MAGAZINE)
c["parameters"] += [
    {"default": 0.0, "name": "Cycle", "type": "trigger"},
    {"default": 0.0, "name": "LastRound", "type": "bool"},
    {"default": 0.0, "name": "StopReload", "type": "bool"},
]
L = c["layers"][0]
states = []
for s in L["states"]:
    if s["name"] in ("TacReload", "EmptyReload"):
        continue
    s["motions"] = motions(*CLIPS[s["name"]])
    states.append(s)


def state(name, clip, weapon, pos, events=(), tags=("Reload", "ADSCarry"), priority=3):
    s = {"loop": False, "motions": motions(clip, weapon), "name": name, "position": list(pos),
         "priority": priority, "speed": 1.0, "tags": list(tags)}
    if events:
        s["events"] = [{"name": n, "time": t} for n, t in events]
    return s


states += [
    state("Pump", "Pump", "Pump", (990, -330), tags=("Cycling", "ADSCarry")),
    state("ReloadStart", "Reload_Start", "Reload_Start", (660, -30)),
    state("ReloadStartEmpty", "Reload_Start_Empty", "Reload_Start_Empty", (660, 45), [("LoadRound", 0.74)]),
    state("ReloadLoop", "Reload_Loop", "Reload_Loop", (990, 5), [("LoadRound", 0.6)]),
    state("ReloadLoopEnd", "Reload_Loop_End", "Reload_Loop_End", (1320, -30), [("LoadRound", 0.43)]),
    state("ReloadEnd", "Reload_End", "Reload_End", (1320, 45)),
]
L["states"] = states


def cond(param, mode, threshold=0.0):
    return {"mode": mode, "param": param, "threshold": threshold}


def any_to(to, conds, duration):
    return {"canTransitionToSelf": True, "conditions": conds, "duration": duration, "exitTime": 1.0,
            "fromAny": True, "hasExitTime": False, "respectPriority": True, "to": to}


def after(frm, to, conds=(), duration=0.04):
    return {"conditions": list(conds), "duration": duration, "exitTime": 1.0, "from": frm, "hasExitTime": True, "to": to}


out = []
for t in L["transitions"]:
    to, frm = t.get("to"), t.get("from")
    if to in ("TacReload", "EmptyReload") or frm in ("TacReload", "EmptyReload"):
        if t.get("fromAny") and to == "EmptyReload":
            out.append(any_to("ReloadStartEmpty", [cond("Reload", "if"), cond("Ammo", "less", 0.5)], 0.05))
            out.append(any_to("ReloadStart", [cond("Reload", "if"), cond("Ammo", "greater", 0.5)], 0.05))
            # The pump: one Cycle per round, never restarting itself.
            pump = any_to("Pump", [cond("Cycle", "if")], 0.05)
            pump["canTransitionToSelf"] = False
            out.append(pump)
        continue
    out.append(t)
for frm in ("ReloadStart", "ReloadStartEmpty", "ReloadLoop"):
    out += [after(frm, "ReloadEnd", [cond("StopReload", "if")]),
            after(frm, "ReloadLoopEnd", [cond("LastRound", "if")]),
            after(frm, "ReloadLoop")]
out += [after("ReloadLoopEnd", "Exit", duration=0.25), after("ReloadEnd", "Exit", duration=0.25),
        after("Pump", "Exit", duration=0.1)]
L["transitions"] = out
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump(c, f, indent=2, sort_keys=True)
    f.write("\n")
print("wrote", OUT, len(states), "states", len(out), "transitions")
