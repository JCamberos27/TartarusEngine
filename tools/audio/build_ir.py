"""Build the room impulse responses from recipes/ir.json (real measurements only: EchoThief, OpenAIR).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_ir.py [--only <class> ...]

  project/assets/Audio/IR/<class>_<n>.wav     key ir.<class>, layer ir; manifest extras: rt60_s, predelay_ms, truncated, site, license
Sources live in C:\\tb\\audio-src\\ir (never committed).
"""
import glob
import json
import math
import os
import sys

import numpy as np
import soundfile as sf

import abuild
import adsp

SR = adsp.SR
IR_ROOT = os.path.join(adsp.SRC_ROOT, "ir")


def resolve(name):
    if name.lower().endswith(".wav"):
        return os.path.join(IR_ROOT, name.replace("/", os.sep))
    hits = glob.glob(os.path.join(IR_ROOT, "EchoThiefImpulseResponseLibrary", "**", glob.escape(name) + ".wav"), recursive=True)
    if len(hits) != 1:
        raise FileNotFoundError(f"EchoThief {name}: {len(hits)} matches")
    return hits[0]


def rt60_s(x, p):
    """Schroeder backward integration (T20 extrapolated to 60 dB) of the recording from its direct sound on; the decay is cut where it
    reaches the measurement's noise floor (+6 dB) so the floor does not flatten the slope."""
    m = x[p:].astype(np.float64)
    m = (m ** 2).mean(axis=1) if m.ndim == 2 else m ** 2
    k = max(1, int(0.01 * SR))
    sm = np.convolve(m, np.ones(k) / k, mode="same")
    noise = max(sm[int(len(sm) * 0.9):].mean(), 1e-14)
    db = 10 * np.log10(np.maximum(sm, 1e-14))
    below = np.nonzero(db < 10 * np.log10(noise) + 6.0)[0]
    end = int(below[int(0.02 * SR) < below][0]) if (below > int(0.02 * SR)).any() else len(m)
    end = max(end, int(0.05 * SR))
    edc = np.cumsum(m[:end][::-1])[::-1]
    edc_db = 10 * np.log10(np.maximum(edc / edc[0], 1e-14))
    t = np.arange(len(edc_db)) / SR
    for lo, hi, mult in ((-5, -25, 3.0), (-5, -15, 6.0), (-3, -10, 8.5)):
        ia, ib = np.argmax(edc_db <= lo), np.argmax(edc_db <= hi)
        if edc_db.min() <= hi and ib > ia + 8:
            slope = np.polyfit(t[ia:ib], edc_db[ia:ib], 1)[0]
            if slope < 0:
                return float(min(30.0, -60.0 / slope))
    return float(end / SR)


def build_one(spec, max_s, direct_ms):
    paths = [resolve(s) for s in spec["src"]]
    adsp.take_uses()
    xs = []
    for p in paths:
        if len(paths) == 1 and sf.info(p).channels < 2:
            raise ValueError(f"{p}: a single source must be stereo (pair two mono positions instead)")
        xs.append(adsp.load(p))
    chans = [(xs[0], 0), (xs[0], 1)] if len(xs) == 1 else [(xs[0], 0), (xs[1], 0)]
    peaks = []
    if len(xs) == 1:
        p0 = int(np.argmax(np.abs(xs[0]).max(axis=1)))
        peaks = [p0, p0]
    else:
        peaks = [int(np.argmax(np.abs(x[:, 0]))) for x in xs]
    d = int(direct_ms / 1000 * SR)
    cuts = []
    for (x, c), p in zip(chans, peaks):
        a = p + d
        cuts.append(adsp.cut(x, a, a + int((max_s + 1.0) * SR))[:, c])      # +1 s: room to drop the quiet lead-in
    n = min(len(c) for c in cuts)
    y = np.stack([c[:n] for c in cuts], 1)
    env = np.convolve(np.abs(y).max(axis=1), np.ones(int(0.001 * SR)) / int(0.001 * SR), mode="same")
    direct = max(float(abs(x[p, c])) for (x, c), p in zip(chans, peaks))
    hit = env > direct * 10 ** (-35 / 20)           # the first reflection: within 35 dB of the direct sound
    onset = int(np.argmax(hit)) if hit.any() else 0
    y = y[onset:onset + int(max_s * SR)]
    predelay = (d + onset) / SR * 1000.0
    rt = float(np.mean([rt60_s(x[:, c], p) for (x, c), p in zip(chans, peaks)]))
    fo = max(int(0.08 * SR), int(0.3 * len(y)))
    y = adsp.fade(y, int(0.001 * SR), fo)
    y = adsp.normalize_peak(y, -3.0)
    return y.astype(np.float32), round(rt, 2), round(predelay, 1), rt > max_s, adsp.take_uses()


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "ir.json")))
    # --only <class> [<class> ...]: rebuild just those classes and leave the other IRs (and their manifest rows) alone.
    only = sys.argv[sys.argv.index("--only") + 1:] if "--only" in sys.argv else list(cfg["classes"])
    entries = []
    for cls, c in cfg["classes"].items():
        if cls not in only:
            continue
        for n, spec in enumerate(c["irs"], 1):
            y, rt, pre, trunc, uses = build_one(spec, c["max_s"], cfg["direct_ms"])
            rel = f"IR/{cls}_{n}.wav"
            e = abuild.emit(rel, y, f"ir.{cls}", "ir",
                            {"variant": n, "site": spec["name"], "rt60_s": rt, "predelay_ms": pre, "truncated": trunc,
                             "license": cfg["licenses"][spec["license"]]}, sources=uses)
            entries.append(e)
            print(f"{rel:24s} {e['length_s']:.2f}s  rt60 {rt:5.2f}s  predelay {pre:6.1f} ms  {'(cut at max)' if trunc else ''}  {spec['name']}")
    print("manifest files:", abuild.update_manifest(entries, [f"IR/{cls}_" for cls in only]))


if __name__ == "__main__":
    main()
