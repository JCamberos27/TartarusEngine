"""Convert legacy sample-game components to project script slots without changing GUIDs.

Pass files/directories and --apply to save. Existing managed fields win over legacy values.
The native scene reader performs the same conversion for old external scenes on load.
"""
import argparse
import json
from pathlib import Path

TYPES = {"Impact Audio":"ImpactAudioDefinition","Weapon Audio":"WeaponAudioDefinition","Foley Audio":"FoleyDefinition","FX & HUD Settings":"EffectsDefinition","Goal Trigger": "BasketGoal", "Scoreboard": "Scoreboard", "Score Digit": "ScoreDigit", "Impact Sound": "ImpactSound", "First Person Controller": "PlayerDefinition", "Health": "Health", "NPC Spawn": "NpcDefinition", "Squad Settings": "SquadDefinition"}

def migrate(entity):
    changed = False
    for old, new in TYPES.items():
        if old not in entity:
            continue
        legacy = entity.pop(old)
        fields = {}
        for key, value in legacy.items():
            name = key.replace(" ", "")
            name={'VolumeJitterdB': 'VolumeJitterDb', 'EnvGainOutdoorOpen': 'EnvTailGainOutdoorOpen', 'EnvGainOutdoorUrban': 'EnvTailGainOutdoorUrban', 'EnvGainIndoorSmall': 'EnvTailGainIndoorSmall', 'EnvGainIndoorLarge': 'EnvTailGainIndoorLarge', 'NPCStepVolume': 'NpcStepVolume', 'NPCStepMinDistance': 'NpcStepMinDistance', 'NPCStepMaxDistance': 'NpcStepMaxDistance'}.get(name,name)
            if name.endswith("Guid"):
                continue
            name = {"KillHeight": "KillY", "StickLookDeg/Sec": "StickLookDegPerSec", "ViewModelFOV": "ViewModelFov", "FieldofView": "FieldOfView"}.get(name, name)
            if name in ("DataFile", "Clip", "ScoreSound", "PrimaryWeaponPrefab", "SecondaryWeaponPrefab", "AnimationSet", "SecondaryAnimationSet") and isinstance(value, str):
                value = {"path": value, "pathGuid": legacy.get(key + "Guid", "")}
            if name in ("ViewModelOffset", "ViewModelRotation") and isinstance(value, list):
                value = dict(zip(("X", "Y", "Z"), value))
            if name in ("Team", "Place") and isinstance(value, str):
                value = int(value in ("Away", "Tens"))
            name={'NPCDamageScale': 'NpcDamageScale', 'CoverSampleSpacing': 'CoverSpacing', 'LowCoverHeight': 'CoverKneeHeight', 'HighCoverHeight': 'CoverHeadHeight', 'CoverPeekStep': 'CoverStep', 'DamageScale': 'NpcDamageScale'}.get(name,name)
            if old=="NPC Spawn" and name in ("Weapon","Brain") and isinstance(value,str):
                value=({"Remington 870":1,"Random":2}.get(value,0) if name=="Weapon" else int(value=="Training Dummy"))
            fields[name] = value
        kind = "Tartarus.Gameplay." + new
        script = entity.get("C# Script")
        if script is None:
            entity["C# Script"] = {"Class": kind, "Source": "assets/Scripts/" + new + ".cs", "Fields JSON": json.dumps(fields, separators=(",", ":")), "Enabled": True, "Scripts": "[]", "Next Script ID": 1}
        else:
            slots = json.loads(script.get("Scripts", "[]"))
            found = False
            if script.get("Class") == kind:
                current = json.loads(script.get("Fields JSON", "{}"))
                script["Fields JSON"] = json.dumps(fields | current, separators=(",", ":"))
                found = True
            for slot in slots:
                if slot.get("class") == kind:
                    slot["fields"] = fields | slot.get("fields", {})
                    found = True
            if not found:
                next_id = max([1, script.get("Next Script ID", 1)] + [slot["id"] + 1 for slot in slots])
                slots.append({"id": next_id, "source": "assets/Scripts/" + new + ".cs", "class": kind, "fields": fields, "enabled": True})
                script["Next Script ID"] = next_id + 1
            script["Scripts"] = json.dumps(slots, separators=(",", ":"))
        changed = True
    return changed

def walk(value):
    count = 0
    if isinstance(value, dict):
        count += int(migrate(value))
        for child in value.values():
            count += walk(child)
    elif isinstance(value, list):
        count += sum(walk(child) for child in value)
    return count

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    files = set()
    for path in args.paths:
        files.update(p for p in path.rglob("*") if p.suffix in (".json", ".prefab")) if path.is_dir() else files.add(path)
    for path in sorted(files):
        original = path.read_text(encoding="utf-8-sig")
        if not any('"' + key + '"' in original for key in TYPES):
            continue
        data = json.loads(original)
        count = walk(data)
        if count:
            print(f"{path}: {count} entities" + (" migrated" if args.apply else " to migrate"))
            if args.apply:
                indent = 1 if '\n "' in original else 2
                path.write_text(json.dumps(data, indent=indent, ensure_ascii=False) + "\n", encoding="utf-8")
