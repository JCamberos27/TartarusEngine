#!/usr/bin/env python3
"""Puts the weapon-audio animator events (snd.<gun>.<element>) into the weapons' .controller files.

  python tools/audio/apply_sync_map.py --placeholders      # the built-in guessed times (no sync map yet)
  python tools/audio/apply_sync_map.py                     # tools/audio/sync_map.json, once lane S1 has measured it
  python tools/audio/apply_sync_map.py --check             # exit 1 when the controllers differ from what this would write

sync_map.json: {"<gun>/<StateClip>": [{"key": "snd.ak.mag_out", "frame": 38, "time": 0.31}, ...]}
  <gun>        AKS74U or Remington870 (the folder under project/assets/Weapons)
  <StateClip>  the controller state's name (TacReload, EmptyReload, Pump, ReloadLoop ...)
  time         0..1, normalized time in that state's clip (what the animator's events use). If it is missing the
               entry is skipped with a warning (frame alone can't be normalized without the clip length).

Only events whose name starts with "snd." are touched: they are replaced state by state (a state the map doesn't name
keeps the events it has), every other event (Shot, Refill, LoadRound, Eject) and every other field is left alone.
The files keep their formatting (indent 2, sorted keys, CRLF, trailing newline as found).
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
WEAPONS = ROOT / "project" / "assets" / "Weapons"
SYNC_MAP = Path(__file__).resolve().parent / "sync_map.json"

# Guessed times until the sync map exists: {gun: {state: [(key, normalized time), ...]}}.
PLACEHOLDERS = {
    "AKS74U": {
        "Draw": [("snd.ak.draw", 0.05)],
        "Holster": [("snd.ak.holster", 0.1)],
        "Regrip": [("snd.ak.regrip", 0.3)],
        "Inspect": [("snd.ak.inspect_move", 0.2), ("snd.ak.inspect_rattle", 0.6)],
        "MagCheck": [("snd.ak.mag_check_out", 0.25), ("snd.ak.mag_check_in", 0.7)],
        "Melee": [("snd.ak.melee_swing", 0.25)],
        "TacReload": [("snd.ak.mag_release", 0.2), ("snd.ak.mag_out", 0.3), ("snd.ak.mag_in", 0.62), ("snd.ak.mag_seat", 0.72)],
        "EmptyReload": [("snd.ak.mag_release", 0.15), ("snd.ak.mag_out", 0.25), ("snd.ak.mag_in", 0.5),
                        ("snd.ak.mag_seat", 0.58), ("snd.ak.bolt_release", 0.8)],
    },
    "Remington870": {
        "Draw": [("snd.870.draw", 0.05)],
        "Holster": [("snd.870.holster", 0.1)],
        "Regrip": [("snd.870.regrip", 0.3)],
        "Inspect": [("snd.870.inspect_move", 0.2), ("snd.870.inspect_rattle", 0.6)],
        "MagCheck": [("snd.870.shell_check", 0.4)],
        "Melee": [("snd.870.melee_swing", 0.25)],
        "Pump": [("snd.870.pump_back", 0.1), ("snd.870.pump_forward", 0.55)],
        "ReloadStart": [("snd.870.shell_grab", 0.3)],
        "ReloadStartEmpty": [("snd.870.pump_back", 0.15), ("snd.870.shell_grab", 0.45), ("snd.870.shell_insert", 0.74)],
        "ReloadLoop": [("snd.870.shell_grab", 0.15), ("snd.870.shell_insert", 0.6)],
        "ReloadLoopEnd": [("snd.870.shell_grab", 0.12), ("snd.870.shell_insert", 0.43)],
        "ReloadEnd": [("snd.870.pump_forward", 0.35)],
    },
}


def load_controller(path):
    raw = path.read_bytes().decode("utf-8")
    crlf = "\r\n" in raw
    trailing = raw.endswith("\n")
    return json.loads(raw), crlf, trailing


def dump_controller(data, crlf, trailing):
    text = json.dumps(data, indent=2, sort_keys=True)
    if trailing:
        text += "\n"
    if crlf:
        text = text.replace("\n", "\r\n")
    return text.encode("utf-8")


def states_of(data):
    for layer in data.get("layers", []):
        for state in layer.get("states", []):
            yield state


def apply(gun, per_state):
    """per_state: {state name: [(key, time), ...]}. Returns (new bytes, old bytes, warnings)."""
    path = WEAPONS / gun / (gun + ".controller")
    old = path.read_bytes()
    data, crlf, trailing = load_controller(path)
    warnings = []
    by_name = {s.get("name"): s for s in states_of(data)}
    for state_name, events in per_state.items():
        state = by_name.get(state_name)
        if state is None:
            warnings.append(f"{gun}: no state '{state_name}' in the controller")
            continue
        kept = [e for e in state.get("events", []) if not str(e.get("name", "")).startswith("snd.")]
        added = [{"name": key, "time": round(float(t), 4)} for key, t in events]
        merged = sorted(kept + added, key=lambda e: (e["time"], e["name"]))
        state["events"] = merged
    return dump_controller(data, crlf, trailing), old, warnings, path


def from_sync_map(path):
    table = json.loads(path.read_text(encoding="utf-8"))
    out = {}
    warnings = []
    for clip_key, entries in table.items():
        if "/" not in clip_key:
            warnings.append(f"sync map: '{clip_key}' isn't <gun>/<StateClip>")
            continue
        gun, state = clip_key.split("/", 1)
        for e in entries:
            if "time" not in e:
                warnings.append(f"sync map: {clip_key} {e.get('key')} has no normalized 'time'; skipped")
                continue
            out.setdefault(gun, {}).setdefault(state, []).append((e["key"], e["time"]))
    return out, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--placeholders", action="store_true", help="apply the built-in guessed times instead of the sync map")
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 if the controllers would change")
    ap.add_argument("--sync-map", type=Path, default=SYNC_MAP)
    args = ap.parse_args()

    warnings = []
    if args.placeholders:
        plan = PLACEHOLDERS
    else:
        if not args.sync_map.is_file():
            print(f"no sync map at {args.sync_map} (lane S1 writes it); use --placeholders for the guessed times", file=sys.stderr)
            return 2
        plan, warnings = from_sync_map(args.sync_map)

    changed = 0
    for gun, per_state in plan.items():
        if not (WEAPONS / gun / (gun + ".controller")).is_file():
            warnings.append(f"no controller for '{gun}'")
            continue
        new, old, w, path = apply(gun, per_state)
        warnings += w
        if new != old:
            changed += 1
            if not args.check:
                path.write_bytes(new)
                print(f"updated {path.relative_to(ROOT)}")
            else:
                print(f"would update {path.relative_to(ROOT)}")
    for w in warnings:
        print("warning:", w, file=sys.stderr)
    if args.check:
        return 1 if changed else 0
    if not changed:
        print("controllers already match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
