"""The mastering EQ every shipped sound passes through (recipes/eq.json): per-category filters and a pre-onset noise gate, then
the file is levelled back to the loudness it had before (so mix.json's levels and the engine's mix keep their meaning).

abuild.emit() runs it on every file a build script writes; remaster.py runs it once on files cooked before it existed. Each
manifest entry records the EQ version it went through ("eq"), so nothing is equalised twice.

Filters are RBJ biquads (Audio EQ Cookbook): hp (Butterworth high-pass, order 2 or 4), low_shelf / high_shelf, peak. A loop is
filtered as three copies end to end and the middle kept, so its loop point stays seamless.
"""
import json
import math
import os
import re

import numpy as np
from scipy import signal

import adsp

SR = adsp.SR
HERE = os.path.dirname(os.path.abspath(__file__))
_CFG = None


def cfg():
    global _CFG
    if _CFG is None:
        _CFG = json.load(open(os.path.join(HERE, "recipes", "eq.json")))
        for r in _CFG["rules"]:
            r["_re"] = re.compile(r["match"])
    return _CFG


def rule_for(key):
    """The first rule whose `match` (a regex) finds the sound key; None when no rule or the rule is empty."""
    for r in cfg()["rules"]:
        if r["_re"].search(key):
            return r if (r.get("hp") or r.get("bands") or r.get("gate")) else None
    return None


def _biquad(kind, f0, q, db):
    a_ = 10 ** (db / 40.0)
    w0 = 2 * math.pi * f0 / SR
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2 * q)
    if kind == "peak":
        b = [1 + alpha * a_, -2 * cw, 1 - alpha * a_]
        a = [1 + alpha / a_, -2 * cw, 1 - alpha / a_]
    elif kind in ("low_shelf", "high_shelf"):
        sq = 2 * math.sqrt(a_) * alpha
        if kind == "low_shelf":
            b = [a_ * ((a_ + 1) - (a_ - 1) * cw + sq), 2 * a_ * ((a_ - 1) - (a_ + 1) * cw), a_ * ((a_ + 1) - (a_ - 1) * cw - sq)]
            a = [(a_ + 1) + (a_ - 1) * cw + sq, -2 * ((a_ - 1) + (a_ + 1) * cw), (a_ + 1) + (a_ - 1) * cw - sq]
        else:
            b = [a_ * ((a_ + 1) + (a_ - 1) * cw + sq), -2 * a_ * ((a_ - 1) + (a_ + 1) * cw), a_ * ((a_ + 1) + (a_ - 1) * cw - sq)]
            a = [(a_ + 1) - (a_ - 1) * cw + sq, 2 * ((a_ - 1) - (a_ + 1) * cw), (a_ + 1) - (a_ - 1) * cw - sq]
    else:
        raise ValueError(kind)
    return np.array([b[0] / a[0], b[1] / a[0], b[2] / a[0], 1.0, a[1] / a[0], a[2] / a[0]])


def sos_for(rule):
    secs = []
    if rule.get("hp"):
        secs.append(signal.butter(int(rule.get("hp_order", 2)), float(rule["hp"]), "hp", fs=SR, output="sos"))
    for b in rule.get("bands", []):
        secs.append(_biquad(b["type"], float(b["hz"]), float(b.get("q", 0.707)), float(b["db"]))[None, :])
    return np.vstack(secs) if secs else None


def gate_pre_onset(x, rule):
    """Silence the noise before the sound starts: everything until `pre_ms` before the onset (the first high-passed sample at
    -34 dB of the peak) fades from nothing over `ramp_ms`. Only when that lead-in is noise (its RMS `noise_db` under the peak)."""
    g = cfg()["gate"]
    on = adsp.first_onset(x, frac=10 ** (g["onset_db"] / 20.0), hp=150.0)
    start = on - int(g["pre_ms"] * 0.001 * SR)
    ramp = int(g["ramp_ms"] * 0.001 * SR)
    if start - ramp <= int(0.002 * SR):
        return x
    m = adsp.to_mono(x)
    lead = m[:start]
    if 20 * np.log10(np.sqrt(np.mean(lead ** 2)) + 1e-12) - 20 * np.log10(np.abs(m).max() + 1e-12) > g["noise_db"]:
        return x                                    # the lead-in is sound, not noise: leave it
    w = np.ones(len(x), np.float32)
    w[:start - ramp] = 0.0
    w[start - ramp:start] = np.sin(np.linspace(0.0, math.pi / 2, ramp)) ** 2  # check_audio: window
    return (np.asarray(x) * w[:, None]).astype(np.float32)


def shape_tail(x, tail_s):
    """One family's decay: the sound dies away by `tail_s` after its peak (a raised-cosine fade over the last 40 %, then cut), so a
    set of different recordings rings for the same time, as if from one library."""
    m = np.abs(adsp.to_mono(x))
    pk = int(np.argmax(m))
    end = pk + int(tail_s * SR)
    if end >= len(x):
        return x
    a = pk + int(0.6 * tail_s * SR)
    y = np.array(x[:end], np.float32, copy=True)
    y[a:end] *= (np.cos(np.linspace(0.0, np.pi, end - a)) * 0.5 + 0.5)[:, None]  # check_audio: window
    return y


def apply(x, key, layer, loop=False):
    """(the processed audio, the rule's name or None). Levels back to the input's LUFS-M max."""
    import abuild
    rule = rule_for(key)
    if rule is None:
        return x, None
    x = np.asarray(x, np.float32)
    if x.ndim == 1:
        x = x[:, None]
    before = adsp.lufs(x)[1]
    y = x
    sos = sos_for(rule)
    if sos is not None:
        if loop:
            n = len(y)
            y = signal.sosfilt(sos, np.concatenate([y, y, y]), axis=0)[n:2 * n]
        else:
            y = signal.sosfilt(sos, y, axis=0)
        y = y.astype(np.float32)
    if rule.get("gate") and not loop:
        y = gate_pre_onset(y, rule)
    if rule.get("tail_s") and not loop:
        y = shape_tail(y, float(rule["tail_s"]))
    y = abuild.finish(y, layer, target=before, trim_edges=False, loop=loop)
    return y, rule["name"]
