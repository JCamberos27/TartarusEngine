"""The mix spec (recipes/mix.json) applied to the manifest: every entry gets `mix_db`.

  mix_db = spec level - (the file's LUFS-M - the shot reference)         (see recipes/mix.json, docs/AUDIO.md "Mix")

The reference is the player's own gunshot (close + sub + mech of a gun at their nominal layer offsets, summed from t = 0).
Shot layers (close / sub / mech / tail / far) are stored relative to that UNSCALED sum, because the engine multiplies every
layer of the player's report by Player Gain (0.75) itself; every other key plays at set volume 1.0, so it is stored relative to
the reference as played (unscaled sum + 20 log10(player_gain)). The engine does no other loudness math.

  python apply_mix.py      # rewrite mix_db on every entry of audio_manifest.json (build_*.py do this too, through abuild.update_manifest)
"""
import json
import math
import os
import re

import numpy as np
import soundfile as sf

import abuild
import adsp

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC_PATH = os.path.join(HERE, "recipes", "mix.json")
SPEC = json.load(open(SPEC_PATH))
SHOT_LAYERS = ("close", "sub", "mech")
SPACES = ("indoor_small", "indoor_large", "outdoor_urban", "outdoor_open")
TOLERANCE_DB = 0.1
PEAK_CAP_DB = -0.5        # a file's true peak + its mix_db (before the engine's Player Gain) never passes this


def _lvl():
    return SPEC["levels"]


def classify(e):
    """-> (family, spec level dB or None, relative to): 'shot-layer' (close / sub / mech: nominal offset + compensation),
    'shot' (the unscaled shot sum), 'played' (the reference as played = x Player Gain), 'none'."""
    key = e["key"]
    L, S = _lvl(), SPEC["surface_db"]
    m = re.fullmatch(r"snd\.(ak|870)\.fire_(close|sub|mech|far|tail)(?:_(\w+))?", key)
    if m:
        part, space = m.group(2), m.group(3)
        if part in SHOT_LAYERS:
            return "shot_" + part, SPEC["reference"]["shot_layer_db"][part], "shot-layer"
        if part == "far":
            return "shot_far", L["shot"]["far"], "shot"
        gun_tail = float(SPEC["reference"].get("gun_tail_db", {}).get(m.group(1), 0.0))
        return "shot_tail", L["shot"]["tail"] + SPEC["space_db"].get(space or "outdoor_urban", 0.0) + gun_tail, "shot"
    if key == "snd.flyby":
        return "flyby", L["flyby"], "played"
    m = re.fullmatch(r"snd\.impact\.(\w+)", key)
    if m:
        mat = m.group(1)
        if mat == "flesh":
            return "flesh", L["flesh"], "played"
        return "impact", L["impact"] + S.get(mat, 0.0), "played"
    m = re.fullmatch(r"snd\.casing\.(rifle|shell)\.(\w+)", key)
    if m:
        return "casing", L["casing"] + S.get(m.group(2), 0.0), "played"
    m = re.fullmatch(r"snd\.foley\.step_(\w+)\.(walk|run|land)", key)
    if m:
        return "step", L["step"][m.group(2)] + S.get(m.group(1), 0.0), "played"
    m = re.fullmatch(r"snd\.foley\.move\.(\w+)", key)
    if m and m.group(1) in L["move"]:
        return "move", L["move"][m.group(1)], "played"
    m = re.fullmatch(r"snd\.foley\.weapon\.(\w+)", key)
    if m and m.group(1) in L["gear"]:
        return "gear", L["gear"][m.group(1)], "played"
    m = re.fullmatch(r"snd\.(ak|870)\.(\w+)", key)
    if m:
        el = m.group(2)
        if el == "melee_hit":
            return "melee_hit", L["melee_hit"], "played"
        if el == "melee_swing":
            return "melee_swing", L["melee_swing"], "played"
        if el in L["elements"]:
            return "element", L["elements"][el], "played"
    m = re.fullmatch(r"snd\.ui\.(\w+)", key)
    if m and m.group(1) in L["ui"]:
        return "ui", L["ui"][m.group(1)], "played"
    if key == "snd.body_fall":
        return "body_fall", L["body_fall"], "played"
    m = re.fullmatch(r"snd\.amb\.(\w+)", key)
    if m and m.group(1) in L["ambience"]:
        return "ambience", L["ambience"][m.group(1)], "played"
    if re.fullmatch(r"ir\.\w+", key):
        return "ir", None, "none"
    return "unspecified", None, "none"


def read_wav(e):
    x, _ = sf.read(os.path.join(abuild.AUDIO_DIR, e["file"]), dtype="float32", always_2d=True)
    return x


def _layer_of(e):
    return classify(e)[0][5:]


def gun_db(e):
    """The gun's own offset on its report (reference.gun_db, e.g. the shotgun above the carbine); 0 for anything else."""
    m = re.match(r"snd\.(ak|870)\.fire_(close|sub|mech)\b", e["key"])
    if not m:
        return 0.0
    g = SPEC["reference"].get("gun_db", {}).get(m.group(1), 0.0)
    return float(g.get(m.group(2), 0.0) if isinstance(g, dict) else g)   # one number for the report, or per layer


def shot_comp_db(e, lufs_m, extra_db=0.0):
    """Per-file compensation of a close / sub / mech variant to its layer's loudness target (clamped), so every variant plays
    equally loud; never so much that the file's true peak, played at this mix_db (+ extra_db), passes PEAK_CAP_DB."""
    lo, hi = SPEC["reference"]["shot_layer_comp_db"]
    off = SPEC["reference"]["shot_layer_db"][_layer_of(e)]
    tgt = abuild.TARGETS["layers"][e["layer"]]["target"]
    hi = min(hi, PEAK_CAP_DB - e["true_peak_dbtp"] - off - extra_db)
    return min(hi, max(lo, tgt - lufs_m))


def shot_layer_mix_db(e, lufs_m, with_gun=True):
    """The nominal layer offset + the compensation (+ the gun's offset; the reference is measured without it, so a louder
    shotgun doesn't move the rest of the mix)."""
    g = gun_db(e) if with_gun else 0.0
    return SPEC["reference"]["shot_layer_db"][_layer_of(e)] + shot_comp_db(e, lufs_m, g) + g


def compute_reference(files, lufs_of, with_gun=False):
    """Mean over variant combinations (both guns) of the LUFS-M of close + sub + mech at their mix_db, summed from t = 0.
    -> (unscaled shot LUFS-M, per-gun means). with_gun: as played, the guns' own offsets (reference.gun_db) included."""
    per_gun = {}
    for gun in ("ak", "870"):
        by = {l: [e for e in files if e["key"] == f"snd.{gun}.fire_{l}"] for l in SHOT_LAYERS}
        if not all(by.values()):
            continue
        res = []
        for i in range(max(len(v) for v in by.values())):
            mix = np.zeros((adsp.SR * 4, 2), np.float32)
            for lay, v in by.items():
                e = v[i % len(v)]
                x = read_wav(e)
                n = min(len(mix), len(x))
                mix[:n] += (x[:n] * 10 ** (shot_layer_mix_db(e, lufs_of(e), with_gun=with_gun) / 20)).astype(np.float32)
            res.append(adsp.lufs(mix)[1])
        per_gun[gun] = float(np.mean(res))
    if not per_gun:
        raise RuntimeError("no gun shot layers in the manifest: cannot measure the mix reference")
    return float(np.mean(list(per_gun.values()))), per_gun


def compute(files, lufs_of=None):
    """-> ({file: mix_db}, manifest `mix` block). lufs_of(e) = the file's LUFS-M (default: the manifest's own).
    Rule per entry: spec level - (file LUFS-M - reference). Then the variants of one key are held within +-variant_spread_db of the
    key's median (a click's LUFS-M is dominated by its length, not its punch), and nothing is pushed past PEAK_CAP_DB (file true
    peak + mix_db, before the engine's Player Gain). Close / sub / mech are their nominal offset + the clamped compensation."""
    lufs_of = lufs_of or (lambda e: e["lufs_m_max"])
    shot, per_gun = compute_reference(files, lufs_of)
    _, per_gun_played = compute_reference(files, lufs_of, with_gun=True)
    ref = {"shot": shot, "played": shot + 20 * math.log10(SPEC["reference"]["player_gain"])}
    raw = {}
    for e in files:
        fam, level, rel = classify(e)
        lm = lufs_of(e)
        if rel == "shot-layer":
            raw[e["file"]] = shot_layer_mix_db(e, lm)
        elif rel in ("shot", "played"):
            raw[e["file"]] = ref[rel] + level - lm
        elif fam == "ir":
            raw[e["file"]] = _lvl()["ir"]
        else:
            raise KeyError(f"{e['file']}: key {e['key']} is not covered by recipes/mix.json")
    spread = SPEC["reference"]["variant_spread_db"]
    groups = {}
    for e in files:
        if classify(e)[2] in ("shot", "played"):
            groups.setdefault(e["key"], []).append(e)
    out = dict(raw)
    for g in groups.values():
        med = float(np.median([raw[e["file"]] for e in g]))
        for e in g:
            v = min(med + spread, max(med - spread, raw[e["file"]]))
            out[e["file"]] = min(v, PEAK_CAP_DB - e["true_peak_dbtp"])
    out = {f: round(v, 2) for f, v in out.items()}
    meta = {"spec": "recipes/mix.json", "shot_lufs_m": round(shot, 2), "reference_lufs_m": round(ref["played"], 2),
            "player_gain": SPEC["reference"]["player_gain"], "per_gun_lufs_m": {g: round(v, 2) for g, v in per_gun.items()},
            # each gun's report as played over the 0 dB reference (its reference.gun_db, measured): --audio-test's spec for it
            "gun_offset_db": {g: round(per_gun_played[g] - shot, 2) for g in per_gun_played},
            "distance": distance_models(ref)}
    return out, meta


def distance_models(ref):
    """The manifest's `mix.distance`: recipes/mix.json "distance", with npc_shot's offset computed. A soldier's shot plays the shot layer
    files, whose mix_db is relative to the UNSCALED shot (ref["shot"]); the spec wants it at levels.npc_shot re the reference as played."""
    out = {}
    for name, m in SPEC["distance"].items():
        if name.startswith("_"):
            continue
        d = {k: v for k, v in m.items() if not k.startswith("_")}
        if name == "npc_shot":
            d["offset_db"] = round(_lvl()["npc_shot"] - (ref["shot"] - ref["played"]), 2)
        d.setdefault("offset_db", 0.0)
        out[name] = d
    return out


def apply_to(files):
    """Set e['mix_db'] on every entry (in place) and return the manifest `mix` block."""
    out, meta = compute(files)
    for e in files:
        e["mix_db"] = out[e["file"]]
    return meta
