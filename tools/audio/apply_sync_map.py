#!/usr/bin/env python3
"""Puts the weapon-audio animator events (snd.<gun>.<element>@<lead ms>) into the weapons' .controller files.

  python tools/audio/apply_sync_map.py            # tools/audio/sync_map.json + project/assets/Audio/audio_manifest.json
  python tools/audio/apply_sync_map.py --check    # exit 1 when the controllers differ from what this would write

sync_map.json: {"<gun>/<Clip>": [{"key", "frame", "time", ...}]}, frame = the CONTACT frame (0-based, 60 fps) of the sound.
  <gun>   AKS74U or Remington870 -> ak / 870 (folder under project/assets/Weapons)
  <Clip>  Tac_Reload ... -> the controller state with the underscores removed (TacReload ...)

A sound file carries its contact transient `anchor_ms` into the file (pump / bolt lead-ins 28-130 ms, draw / holster ~300 ms),
so the sound has to START before the contact frame. Variants of one key have different anchors; the engine decides per played
file. This script writes one event per sound at   t_event = contact - lead   where lead = the key's largest anchor_ms
(clamped to the contact time, so the event is never before the clip start), as the event name suffix "@<lead ms>":
    snd.ak.mag_out@95      normalized time = (frame/60 - 0.095) / (clip frames / 60)
The engine (SoundPlayer, Request::LeadMs) starts the chosen variant (lead - its anchor) later, or - when the contact is
nearer than that variant's lead-in (clamped event) - starts it that far into the file (AudioEngine start offset), so every
variant's transient lands on the contact frame.

snd.<gun>.fire is not placed on the Fire state: shots play from the round (WeaponAudio::Shot expands it to the fire_* layers),
so hip and ADS fire sound the same. Every other event and field of a controller is left alone; the old snd.* events are
replaced everywhere. Files keep their formatting (indent 2, sorted keys, CRLF / trailing newline as found).
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
WEAPONS = ROOT / "project" / "assets" / "Weapons"
SYNC_MAP = Path(__file__).resolve().parent / "sync_map.json"
MANIFEST = ROOT / "project" / "assets" / "Audio" / "audio_manifest.json"
FPS = 60.0
GUNS = {"ak": "AKS74U", "870": "Remington870"}


def load_controller(path):
    raw = path.read_bytes().decode("utf-8")
    return json.loads(raw), "\r\n" in raw, raw.endswith("\n")


def dump_controller(data, crlf, trailing):
    text = json.dumps(data, indent=2, sort_keys=True)
    if trailing:
        text += "\n"
    if crlf:
        text = text.replace("\n", "\r\n")
    return text.encode("utf-8")


def max_anchors():
    out = {}
    if MANIFEST.is_file():
        for e in json.loads(MANIFEST.read_text(encoding="utf-8")).get("files", []):
            out[e["key"]] = max(out.get(e["key"], 0.0), float(e.get("anchor_ms", 0.0)))
    return out


def plan_from_sync_map(path, anchors):
    """{folder: {state: [(event name, normalized time)]}} and warnings."""
    table = json.loads(path.read_text(encoding="utf-8"))
    out, warnings = {}, []
    for clip_key, entries in table.items():
        if clip_key.startswith("_") or "/" not in clip_key:
            continue
        gun, clip = clip_key.split("/", 1)
        folder = GUNS.get(gun) or gun
        state = clip.replace("_", "")
        # The clip's length in frames: frame / time of any event past frame 0 (time = frame / frames).
        frames = next((round(e["frame"] / e["time"]) for e in entries if e.get("time") and e["frame"] > 0), None)
        for e in entries:
            key = e["key"]
            if key.endswith(".fire"):
                continue  # the shot layers play from the round
            if frames is None:
                warnings.append(f"{clip_key}: cannot derive the clip length; skipped")
                break
            contact = e["frame"] / FPS
            lead = min(anchors.get(key, 0.0) / 1000.0, contact)
            norm = (contact - lead) / (frames / FPS)
            out.setdefault(folder, {}).setdefault(state, []).append((f"{key}@{round(lead * 1000)}", round(norm, 4)))
    return out, warnings


def apply(folder, per_state):
    path = WEAPONS / folder / (folder + ".controller")
    old = path.read_bytes()
    data, crlf, trailing = load_controller(path)
    warnings = []
    states = {s.get("name"): s for layer in data.get("layers", []) for s in layer.get("states", [])}
    for s in states.values():
        kept = [ev for ev in s.get("events", []) if not str(ev.get("name", "")).startswith("snd.")]
        if kept:
            s["events"] = kept
        else:
            s.pop("events", None)
    for state_name, events in per_state.items():
        s = states.get(state_name)
        if s is None:
            warnings.append(f"{folder}: no state '{state_name}' in the controller")
            continue
        merged = s.get("events", []) + [{"name": n, "time": t} for n, t in events]
        s["events"] = sorted(merged, key=lambda ev: (ev["time"], ev["name"]))
    return dump_controller(data, crlf, trailing), old, warnings, path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 if the controllers would change")
    ap.add_argument("--sync-map", type=Path, default=SYNC_MAP)
    args = ap.parse_args()
    if not args.sync_map.is_file():
        print(f"no sync map at {args.sync_map}", file=sys.stderr)
        return 2
    plan, warnings = plan_from_sync_map(args.sync_map, max_anchors())
    changed = 0
    for folder in GUNS.values():
        if not (WEAPONS / folder / (folder + ".controller")).is_file():
            warnings.append(f"no controller for '{folder}'")
            continue
        new, old, w, path = apply(folder, plan.get(folder, {}))
        warnings += w
        if new != old:
            changed += 1
            if args.check:
                print(f"would update {path.relative_to(ROOT)}")
            else:
                path.write_bytes(new)
                print(f"updated {path.relative_to(ROOT)}")
    for w in warnings:
        print("warning:", w, file=sys.stderr)
    if args.check:
        return 1 if changed else 0
    if not changed:
        print("controllers already match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
