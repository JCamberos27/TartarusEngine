"""Convert C# definition script slots in scenes and prefabs back to their native component blocks.

The project's PlayerDefinition, NpcDefinition, SquadDefinition, WeaponDefinition, attachment, audio and
effects scripts are native components again (First Person Controller, NPC Spawn, ...). This rewrites a
file's "C# Script" slots of those classes as the native blocks the editor saves, and drops the slots.
GUIDs are kept. Pass files or directories; --apply saves, otherwise it only reports.

The scene reader does the same conversion for older files on load (Scripting::ConvertScriptDefinitions),
so this is for committing converted content, not a requirement.
"""
import argparse
import json
import re
import sys
from pathlib import Path

CLASS_TO_COMPONENT = {
    "Tartarus.Gameplay.PlayerDefinition": "First Person Controller",
    "Tartarus.Gameplay.NpcDefinition": "NPC Spawn",
    "Tartarus.Gameplay.SquadDefinition": "Squad Settings",
    "Tartarus.Gameplay.EffectsDefinition": "FX & HUD Settings",
    "Tartarus.Gameplay.WeaponAudioDefinition": "Weapon Audio",
    "Tartarus.Gameplay.FoleyDefinition": "Foley Audio",
    "Tartarus.Gameplay.ImpactAudioDefinition": "Impact Audio",
    "Tartarus.Gameplay.WeaponDefinition": "Weapon Definition",
    "Tartarus.Gameplay.MuzzleAttachment": "Muzzle Attachment",
    "Tartarus.Gameplay.GripAttachment": "Grip Attachment",
    "Tartarus.Gameplay.OpticAttachment": "Optic Attachment",
}

# The native fields' Inspector labels (their JSON keys), as registered in src/Game/ComponentRegistry.cpp.
NATIVE_FIELDS = {
    "First Person Controller": [
        "Primary Weapon Prefab", "Secondary Weapon Prefab", "Move Speed", "Sprint Multiplier", "Jump Speed", "Jump Buffer Time",
        "Coyote Time", "Ground Accel Time", "Ground Decel Time", "Air Accel Time", "Eye Height", "Capsule Radius",
        "Capsule Height", "Mouse Sensitivity", "Invert Y", "Stick Look Deg/Sec", "Eye Radius", "Field of View",
        "Kill Height", "Gravity", "Gravity Gun", "Min Throw Speed", "Max Throw Speed", "Throw Charge Time",
        "Throw Backspin", "Grab Range", "Assist Range", "Assist Cone Deg", "Scroll Turn Deg", "Animation Set",
        "Secondary Animation Set", "View Model Offset", "Camera Bone", "View Model Rotation", "View Model Scale", "View Model FOV",
        "Max Health", "Regen Delay", "Regen Rate", "Respawn Delay", "Spawn Protection",
    ],
    "NPC Spawn": [
        "Weapon", "Squad", "Skill", "Outfit Seed", "Brain", "Bare",
    ],
    "Squad Settings": [
        "Squad Size", "Respawn", "Respawn Delay", "Difficulty", "NPC Damage Scale", "Heavy Hit Damage",
        "Stagger Time", "Bleed-Out Time", "Crawl Speed", "Limp Speed Scale", "Limp Time", "Corpse Time",
        "Max Corpses", "Unseen Time", "Fall Gravity", "Melee Damage", "Melee Time", "Melee Hit Time",
        "Hitbox Range", "Foot IK Range", "Mesh Check Range", "Cover Sample Spacing", "Cover Reach", "Low Cover Height",
        "High Cover Height", "Cover Peek Step", "Body Prefab", "Rifle Prefab", "Shotgun Prefab", "Rifle Animation Set",
        "Shotgun Animation Set",
    ],
    "FX & HUD Settings": [
        "Flash Time", "Player Flash Scale", "Flame Glow", "Muzzle Style", "Flame Scale", "Beam Range",
        "Beam Half Width", "Beam Falloff", "Beam Bend", "Feed Life", "Streak Window",
    ],
    "Weapon Audio": [
        "Gun", "Enabled", "Volume", "Player Gain", "Shot Pitch Min", "Shot Pitch Max",
        "Volume Jitter dB", "Close Full Distance", "Close Zero Distance", "Far Min Weight", "Far Max Weight", "Max Distance",
        "Far Max Distance", "Shot Min Distance", "Bass Min Distance", "Event Min Distance", "Event Max Distance", "Shot Max Voices",
        "Tail Max Voices", "Tail Fade Time", "Tail Min Interval", "Tail Duck Per Voice", "Tail Every", "Far Every",
        "Burst Gap", "Data File", "Env Enabled", "Env Ray Count", "Env Max Distance", "Env Indoor Cover",
        "Env Urban Wall", "Env Urban Distance", "Env Large Room Distance", "Env Blend Fraction", "Env Blend Distance", "Env Refresh Interval",
        "Env Refresh Move Distance", "Env Match Radius", "Env Gain Outdoor Open", "Env Gain Outdoor Urban", "Env Gain Indoor Small", "Env Gain Indoor Large",
        "Env Debug Draw",
    ],
    "Foley Audio": [
        "Enabled", "Volume", "Walk Volume", "Run Volume", "Crouch Volume", "Volume Jitter dB",
        "Pitch Min", "Pitch Max", "Step Stride Scale", "Crouch Stride Scale", "Min Step Speed", "Run Speed",
        "Steps From Feet", "Foot Lift Height", "Foot Lift Moving", "Foot Contact Height", "Jump Volume", "Land Volume",
        "Land Min Speed", "Land Full Speed", "Land Heavy Speed", "NPC Step Volume", "NPC Step Min Distance", "NPC Step Max Distance",
        "Default Surface", "Surface Table",
    ],
    "Impact Audio": [
        "Enabled", "Surface Table", "Default Surface", "Casings Enabled", "Casing Max Contacts", "Casing Min Speed",
        "Casing Full Speed", "Casing Gain Min", "Casing Volume", "Casing Min Distance", "Casing Max Distance", "Casing Max Voices",
        "Shell Radius", "Impacts Enabled", "Impact Volume", "Impact Min Distance", "Impact Max Distance", "Impact Max Voices",
        "Impact Min Interval", "Flesh Uses Recordings", "Flesh Volume", "Flyby Enabled", "Flyby Radius", "Flyby Volume",
        "Flyby Min Interval", "Flyby Min Distance", "Flyby Max Distance", "Flyby Far Gain",
    ],
    "Weapon Definition": [
        "Description", "Animation Set", "Muzzle Enabled", "Flash Time", "Light Intensity", "Player Flash Scale",
        "Light Range", "Light Color", "Flame Texture", "Flame Glow", "Flame Scale", "Flame Color",
        "Flame Lifetime Min", "Flame Lifetime Max", "Flame Length Min", "Flame Length Max", "Flame Width Min", "Flame Width Max",
        "Muzzle Style", "Flash Sprite", "Flash Lifetime", "Flash Size Min", "Flash Size Max", "Flash Intensity",
        "Side Jets", "Core Glow", "Spark Count", "Smoke", "Afterfire Smoke", "Smoke Scale",
        "Smoke Lifetime Min", "Smoke Lifetime Max", "Smoke Alpha", "Magazine", "Rounds Per Minute", "Allow Full Auto",
        "Burst Rounds", "Reload Hold Seconds", "Regrip Min", "Regrip Max", "Impact Impulse", "Impact Max Speed",
        "Bullet Hole Radius", "Zero Distance", "Has Sight Line", "Sight Origin", "Sight Direction", "Pellets",
        "Spread Hip", "Spread Ads", "Reload", "Cycle After Shot", "Cycle Delay", "Damage",
        "Head Multiplier", "Limb Multiplier", "Falloff Start", "Falloff End", "Falloff Min",
    ],
    "Muzzle Attachment": [
        "Enabled", "Muzzle Transform", "Muzzle Flash", "Firing Sounds", "Firing Sound Profile",
    ],
    "Grip Attachment": [
        "Enabled",
    ],
    "Optic Attachment": [
        "Enabled", "Aim Point", "Default Optic", "Aim Blend Time",
    ],
}

# C# field names that are not the native label with its spaces taken out.
RENAMED = {
    "KillY": "Kill Height", "StickLookDegPerSec": "Stick Look Deg/Sec",
    "CoverSpacing": "Cover Sample Spacing", "CoverKneeHeight": "Low Cover Height",
    "CoverHeadHeight": "High Cover Height", "CoverStep": "Cover Peek Step",
    "EnvTailGainOutdoorOpen": "Env Gain Outdoor Open", "EnvTailGainOutdoorUrban": "Env Gain Outdoor Urban",
    "EnvTailGainIndoorSmall": "Env Gain Indoor Small", "EnvTailGainIndoorLarge": "Env Gain Indoor Large",
}
VECTORS = {"View Model Offset", "View Model Rotation", "Light Color", "Flame Color", "Sight Origin", "Sight Direction"}
# Enums are saved as their label, the way the editor writes them.
ENUMS = {
    ("NPC Spawn", "Weapon"): ["AKS-74U", "Remington 870", "Random"],
    ("NPC Spawn", "Brain"): ["Squad AI", "Training Dummy"],
    ("Weapon Definition", "Reload"): ["Magazine", "Per Round"],
}


def folded(name):
    return re.sub(r"[^0-9a-z]", "", name.lower())


def native_label(component, field):
    target = folded(RENAMED.get(field, field))
    for label in NATIVE_FIELDS[component]:
        if folded(label) == target:
            return label
    return None


def native_value(component, label, value):
    if label in VECTORS and isinstance(value, dict):
        return [value.get("X", 0.0), value.get("Y", 0.0), value.get("Z", 0.0)]
    if isinstance(value, dict) and "path" in value:
        guid = value.get("pathGuid", "")
        return {"path": value["path"], "pathGuid": guid} if guid else value["path"]
    labels = ENUMS.get((component, label))
    if labels and isinstance(value, int) and 0 <= value < len(labels):
        return labels[value]
    return value


def convert_slot(entity, class_name, fields, enabled, where, warnings):
    component = CLASS_TO_COMPONENT[class_name]
    block = entity.get(component, {})
    for name, value in (fields or {}).items():
        label = native_label(component, name)
        if label is None:
            warnings.append(f"{where}: {component} has no field for '{name}'; dropped")
            continue
        block[label] = native_value(component, label, value)
    if not enabled:
        if "Enabled" in NATIVE_FIELDS[component]:
            block["Enabled"] = False
        else:
            warnings.append(f"{where}: disabled {class_name} became an active {component}")
    entity[component] = block


def convert_entity(entity, where, warnings):
    script = entity.get("C# Script")
    if not isinstance(script, dict):
        return False
    changed = False
    if script.get("Class") in CLASS_TO_COMPONENT:
        convert_slot(entity, script["Class"], json.loads(script.get("Fields JSON", "{}")), script.get("Enabled", True), where, warnings)
        for key in ("Class", "Source", "SourceGuid", "Fields JSON", "Enabled"):
            script.pop(key, None)
        changed = True
    kept = []
    for slot in json.loads(script.get("Scripts", "[]")):
        if slot.get("class") in CLASS_TO_COMPONENT:
            convert_slot(entity, slot["class"], slot.get("fields", {}), slot.get("enabled", True), where, warnings)
            changed = True
        else:
            kept.append(slot)
    if not changed:
        return False
    if not script.get("Class") and "Source" not in script and not kept:
        del entity["C# Script"]
    else:
        script["Scripts"] = json.dumps(kept, separators=(",", ":"))
    return True


def convert_file(path, apply, warnings):
    text = path.read_text(encoding="utf-8")
    data = json.loads(text)
    count = 0
    for collection in ("boxes", "models", "empties"):
        for entity in data.get(collection, []):
            if convert_entity(entity, f"{path}: {entity.get('name', entity.get('id'))}", warnings):
                count += 1
    if count and apply:
        indent = 4 if text.startswith("{\n    ") else (2 if text.startswith("{\n  ") else None)
        out = json.dumps(data, indent=indent, ensure_ascii=False)
        path.write_text(out + ("\n" if text.endswith("\n") else ""), encoding="utf-8", newline="\n")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--apply", action="store_true", help="save the converted files")
    args = parser.parse_args()
    files = []
    for p in args.paths:
        files += sorted(q for q in p.rglob("*") if q.suffix in (".json", ".prefab")) if p.is_dir() else [p]
    warnings = []
    total = 0
    for f in files:
        try:
            n = convert_file(f, args.apply, warnings)
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
        if n:
            total += n
            print(f"{f}: {n} entit{'y' if n == 1 else 'ies'}" + ("" if args.apply else " (dry run)"))
    for w in warnings:
        print("warning: " + w, file=sys.stderr)
    print(f"{total} entities {'converted' if args.apply else 'to convert'}")


if __name__ == "__main__":
    main()
