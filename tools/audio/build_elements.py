"""Build the weapon action one-shot library from recipes/elements.json.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_elements.py

Writes project/assets/Audio/Weapons/<gun>/<element>_<n>.wav and updates audio_manifest.json. Deterministic.
"""
import json
import os
import sys

import numpy as np
from scipy import signal

import abuild
import adsp

SR = adsp.SR
_cache = {}


def src(path):
    if path not in _cache:
        _cache[path] = adsp.load(path)
    return _cache[path]


def apply_eq(x, v):
    from pedalboard import PeakFilter
    plugs = [PeakFilter(cutoff_frequency_hz=hz, gain_db=g, q=q) for kind, hz, g, q in v.get("eq", [])]
    if plugs:
        x = adsp.board(x, *plugs)
    if "hp" in v:
        x = adsp.filt(x, "hp", v["hp"])
    if "lp" in v:
        x = adsp.filt(x, "lp", v["lp"])
    return x


def slice_variant(v):
    x = src(v["src"])
    y = adsp.slice_at(x, v["t"], v["dur"], preroll_ms=v.get("lead_ms", v.get("preroll_ms", 3.0)),
                      fade_in_ms=v.get("fade_in_ms", 1.5), snap=v.get("snap", True))
    y = adsp.pitch(y, v.get("pitch", 0.0))
    return apply_eq(y, v)


def fit_variant(v):
    """cloth lead [t0, main) varispeed-fitted to main_at_ms, then the thud + its tail from `main` on."""
    x = src(v["src"])
    a = int(v["t0"] * SR)
    m = adsp.find_onset(x, v["main"]) - int(0.003 * SR)
    b = min(len(x), m + int((v["dur"] - (v["main"] - v["t0"])) * SR))
    pre = adsp.fade(x[a:m], int(0.025 * SR), 0)
    post = x[m:b]
    pre = adsp.stretch_to(pre, int(v["main_at_ms"] / 1000 * SR))
    y = np.concatenate([pre, post])
    y = adsp.fade(y, 0, int(max(0.03, 0.25 * (b - m) / SR) * SR))
    y = adsp.pitch(y, v.get("pitch", 0.0))
    return apply_eq(y, v)


def thud_variant(v):
    """cloth bed + a heavier thud layer dropped in at delay_ms (used where the pack has no usable thud)."""
    x = src(v["src"])
    cloth = adsp.slice_at(x, v["t0"], v["dur"], snap=False, fade_in_ms=25)
    cloth = adsp.normalize_peak(cloth, -6.0)          # the source cloth is near silent (-42 dBFS peak)
    th = v["thud"]
    t = adsp.slice_at(src(th["src"]), th["t"], th["dur"])
    t = adsp.pitch(t, th.get("pitch", 0.0))
    t = adsp.gain_db(t, th.get("gain_db", 0.0) - 6.0)
    # the thud carries the impact; cloth leads it and is ducked under it
    n = int(th["delay_ms"] / 1000 * SR)
    y = adsp.mix([(cloth, 0, 0.0), (t, n, 0.0)])
    return apply_eq(y, v)


# ---------------------------------------------------------------------------------------------------- synth one-shots
def synth_dry_fire(v):
    """Hammer fall on an empty chamber: tight metallic click + short body knock, no powder."""
    r = adsp.deterministic_rng("dry", v["seed"])
    n = int(0.16 * SR)
    t = np.arange(n) / SR
    click = r.standard_normal(n) * np.exp(-t / 0.0018)
    click = signal.sosfilt(signal.butter(2, [v["click_hz"] * 0.6, min(v["click_hz"] * 2.2, 20000)], "bp", fs=SR, output="sos"), click)
    ring = np.sin(2 * np.pi * v["click_hz"] * 0.9 * t) * np.exp(-t / 0.006) * 0.35     # small metallic ring
    body = np.sin(2 * np.pi * v["body_hz"] * t * (1 + 0.15 * np.exp(-t / 0.01))) * np.exp(-t / 0.028) * 0.9
    knock = r.standard_normal(n) * np.exp(-t / 0.004)
    knock = signal.sosfilt(signal.butter(2, 900, "lp", fs=SR, output="sos"), knock) * 0.5
    m = click * 0.9 + ring + body + knock
    y = np.stack([m, m * (1 + 0.04 * (r.random() - 0.5))], 1).astype(np.float32)
    return adsp.fade(y, int(0.0005 * SR), int(0.05 * SR))


def synth_melee_swing(v):
    """Air + cloth whoosh: band-passed noise whose centre sweeps f0 -> f1 with a rise/fall envelope."""
    r = adsp.deterministic_rng("swing", v["seed"])
    n = int(v["dur"] * SR)
    t = np.linspace(0, 1, n)
    noise = r.standard_normal(n)
    f = v["f0"] * (v["f1"] / v["f0"]) ** (t ** 1.4)               # exponential sweep
    # time-varying band-pass via a bank crossfade (cheap and artifact free for noise)
    out = np.zeros(n)
    bands = np.geomspace(v["f0"] * 0.6, v["f1"] * 1.4, 10)
    for i, fc in enumerate(bands):
        sos = signal.butter(2, [fc / 1.35, min(fc * 1.35, 20000)], "bp", fs=SR, output="sos")
        w = np.exp(-0.5 * ((np.log(f) - np.log(fc)) / 0.28) ** 2)
        out += signal.sosfilt(sos, noise) * w
    env = np.sin(np.pi * np.clip(t, 0, 1) ** 0.7) ** 2
    m = out * env
    lo = signal.sosfilt(signal.butter(2, [90, 260], "bp", fs=SR, output="sos"), r.standard_normal(n)) * env * 0.5
    m = m + lo
    y = np.stack([m, np.roll(m, int(0.0004 * SR))], 1).astype(np.float32)
    return adsp.fade(y, int(0.02 * SR), int(0.06 * SR))


def synth_melee_hit(v):
    """Blunt strike: pitched-down body thump + noise burst + a metal/handling slap lifted from the pack."""
    r = adsp.deterministic_rng("hit", v["seed"])
    n = int(0.45 * SR)
    t = np.arange(n) / SR
    body = np.sin(2 * np.pi * (v["body_hz"] * (1 + 1.2 * np.exp(-t / 0.03)) * t)) * np.exp(-t / 0.07)
    burst = signal.sosfilt(signal.butter(2, [200, 3000], "bp", fs=SR, output="sos"), r.standard_normal(n)) * np.exp(-t / 0.018)
    thump = (body * 1.0 + burst * 0.55)
    slap = adsp.slice_at(src(v["slap_src"]), v["slap_t"], 0.2)
    slap = adsp.to_mono(slap)
    slap = np.pad(slap, (0, max(0, n - len(slap))))[:n] * 0.5
    m = thump + slap
    y = np.stack([m, m], 1).astype(np.float32)
    y = adsp.soft_sat(y, 1.6)
    return adsp.fade(y, int(0.0005 * SR), int(0.12 * SR))


SYNTH = {"dry_fire": synth_dry_fire, "melee_swing": synth_melee_swing, "melee_hit": synth_melee_hit}


def render_variant(v):
    if "synth" in v:
        y = SYNTH[v["synth"]](v)
    elif "thud" in v:
        y = thud_variant(v)
    elif "main" in v:
        y = fit_variant(v)
    else:
        y = slice_variant(v)
    return adsp.gain_db(y, v.get("gain_db", 0.0))


def describe(v):
    if "synth" in v:
        return f"synth:{v['synth']}#{v['seed']}"
    s = v["src"].replace("\\", "/").split("/")[-1]
    return f"{s}@{v.get('t', v.get('t0'))}"


def main():
    recipe = json.load(open(os.path.join(abuild.HERE, "recipes", "elements.json")))
    entries = []
    for gun, g in recipe["guns"].items():
        for elem, variants in g["elements"].items():
            for n, v in enumerate(variants, 1):
                y = abuild.finish(render_variant(v), "action")
                rel = f"{g['dir']}/{elem}_{n}.wav"
                extra = {"gun": gun, "element": elem, "variant": n, "source": describe(v)}
                if "thud" in v:                      # contact = where the thud layer lands
                    extra["anchor_ms"] = float(v["thud"]["delay_ms"])
                elif "main_at_ms" in v:
                    extra["anchor_ms"] = float(v["main_at_ms"])
                elif v.get("synth") in ("melee_hit", "dry_fire", "melee_swing") or elem == "cloth":
                    extra["anchor_ms"] = 0.0          # no contact transient: the file starts when the motion starts
                if "lead_ms" in v:
                    extra["lead_ms"] = v["lead_ms"]
                entries.append(abuild.emit(rel, y, f"snd.{gun}.{elem}", "action", extra))
                e = entries[-1]
                print(f"{rel:48s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    total = abuild.update_manifest(entries, [f"{g['dir']}/{e}_" for g in recipe["guns"].values() for e in g["elements"]])
    print("manifest files:", total)


if __name__ == "__main__":
    main()
