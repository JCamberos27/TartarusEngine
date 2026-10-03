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
    from pedalboard import PeakFilter, LowShelfFilter, HighShelfFilter
    kinds = {"peak": PeakFilter, "low_shelf": LowShelfFilter, "high_shelf": HighShelfFilter}
    plugs = [kinds[kind](cutoff_frequency_hz=hz, gain_db=g, q=q) for kind, hz, g, q in v.get("eq", [])]
    if plugs:
        x = adsp.board(x, *plugs)
    if "hp" in v:
        x = adsp.filt(x, "hp", v["hp"])
    if "lp" in v:
        x = adsp.filt(x, "lp", v["lp"])
    return x


MIN_DUR_S = 0.0   # set from the recipe's min_dur_s (main)


def slice_variant(v):
    x = src(v["src"])
    dur = max(v["dur"], MIN_DUR_S)   # the trim (abuild.finish trim_bleed) still stops it before the source's next event
    y = adsp.slice_at(x, v["t"], dur, preroll_ms=v.get("lead_ms", v.get("preroll_ms", 3.0)),
                      fade_in_ms=v.get("fade_in_ms", 1.5), snap=v.get("snap", True))
    y = adsp.pitch(y, v.get("pitch", 0.0))
    return apply_eq(y, v)


def fit_variant(v):
    """cloth lead [t0, main) fitted to main_at_ms WITHOUT any pitch / varispeed (the lead's start is dropped when it is too long, silence
    is put in front when it is too short), then the thud + its tail from `main` on."""
    x = src(v["src"])
    a = int(v["t0"] * SR)
    m = adsp.find_onset(x, v["main"]) - int(0.003 * SR)
    b = min(len(x), m + int((v["dur"] - (v["main"] - v["t0"])) * SR))
    pre = adsp.fade(adsp.cut(x, a, m), int(0.025 * SR), 0)
    post = adsp.cut(x, m, b)
    want = int(v["main_at_ms"] / 1000 * SR)
    pre = pre[len(pre) - want:] if len(pre) >= want else np.concatenate([np.zeros((want - len(pre), pre.shape[1]), np.float32), pre])
    pre = adsp.fade(pre, int(0.025 * SR), 0)
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


# ---------------------------------------------------------------------------------------------------- composites
def peak_aligned(v):
    """A swing / whoosh: cut so the loudest part of the whoosh sits at peak_at_ms (the strike lands ~100 ms in)."""
    x = src(v["src"])
    m = adsp.to_mono(x)
    sm = np.convolve(np.abs(m), np.ones(int(0.012 * SR)) / int(0.012 * SR), mode="same")
    pk = int(np.argmax(sm))
    a = max(0, pk - int(v["peak_at_ms"] / 1000 * SR))
    y = adsp.cut(x, a, a + int(v["dur"] * SR))
    y = adsp.fade(y, int(v.get("fade_in_ms", 6) / 1000 * SR), int(max(0.06, v["dur"] * 0.4) * SR))
    y = adsp.pitch(y, v.get("pitch", 0.0))
    return apply_eq(y, v)


def composite(v):
    """Strike / click built only from real recordings: every layer is a slice of a real source, dropped in at delay_ms
    (its own contact transient lands there), with its own pitch / filter / gain."""
    layers = []
    for l in v["layers"]:
        y = slice_variant(l)
        layers.append((adsp.gain_db(y, l.get("gain_db", 0.0)), int(l.get("delay_ms", 0.0) / 1000 * SR), 0.0))
    return apply_eq(adsp.mix(layers), v)


def render_variant(v):
    if "layers" in v:
        y = composite(v)
    elif "peak_at_ms" in v:
        y = peak_aligned(v)
    elif "thud" in v:
        y = thud_variant(v)
    elif "main" in v:
        y = fit_variant(v)
    else:
        y = slice_variant(v)
    return adsp.gain_db(y, v.get("gain_db", 0.0))


def describe(v):
    if "layers" in v:
        return "+".join(describe(l) for l in v["layers"])
    s = v["src"].replace("\\", "/").split("/")[-1]
    return f"{s}@{v.get('t', v.get('t0'))}"


def main():
    global MIN_DUR_S
    recipe = json.load(open(os.path.join(abuild.HERE, "recipes", "elements.json")))
    MIN_DUR_S = float(recipe.get("min_dur_s", 0.0))
    entries = []
    for gun, g in recipe["guns"].items():
        for elem, variants in g["elements"].items():
            for n, v in enumerate(variants, 1):
                adsp.take_uses()
                y = render_variant(v)
                uses = adsp.take_uses()
                snap = recipe.get("snap")
                if snap and elem not in snap.get("skip", []):   # mechanical contacts: harder attack, shorter ring
                    y = adsp.transient_shape(y, snap.get("attack", 0.0), snap.get("sustain", 0.0))
                y = abuild.finish(y, "action", trim_bleed="layers" not in v)   # (a composite's later layers are meant)
                rel = f"{g['dir']}/{elem}_{n}.wav"
                extra = {"gun": gun, "element": elem, "variant": n, "source": describe(v)}
                if "anchor_ms" in v:                 # the recipe says where the contact is (a composite of several)
                    extra["anchor_ms"] = float(v["anchor_ms"])
                elif "thud" in v:                    # contact = where the thud layer lands
                    extra["anchor_ms"] = float(v["thud"]["delay_ms"])
                elif "main_at_ms" in v:
                    extra["anchor_ms"] = float(v["main_at_ms"])
                elif elem in ("melee_hit", "dry_fire", "melee_swing", "cloth"):
                    extra["anchor_ms"] = 0.0
                elif "src" in v and "t" in v:        # sliced at the contact: anchor = the lead kept before it (pitch scales time)
                    extra["anchor_ms"] = round(v.get("lead_ms", v.get("preroll_ms", 3.0)) / 2 ** (v.get("pitch", 0.0) / 12), 1)
                if "lead_ms" in v:
                    extra["lead_ms"] = v["lead_ms"]
                entries.append(abuild.emit(rel, y, f"snd.{gun}.{elem}", "action", extra, sources=uses))
                e = entries[-1]
                print(f"{rel:48s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    total = abuild.update_manifest(entries, [f"{g['dir']}/{e}_" for g in recipe["guns"].values() for e in g["elements"]])
    print("manifest files:", total)


if __name__ == "__main__":
    main()
