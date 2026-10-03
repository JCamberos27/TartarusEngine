"""Objective listening notes for the shipped audio (or any wavs): what each file sounds like in numbers, and which variants
stand out from their siblings.

  uv run --with numpy --with scipy --with soundfile --with pyloudnorm python tools/audio/analyze_audio.py [key-prefix ...] [--files a.wav b.wav]

Per file: LUFS-M max, true peak, crest (peak over the loudest 50 ms RMS), band balance (dB of each band against the file's total:
sub < 80 Hz, low 80-250, low-mid 250-800, mid 800-2.5k, presence 2.5-6k, air > 6k), spectral centroid, attack (10 -> 90 % of the
envelope peak, ms), noise before the onset and at the end (dB under the peak), flat-top density (samples within 0.1 dB of the
peak: a squashed / clipped shape), stereo correlation. Per key: the median profile, each variant's largest band deviation from
it, and pairs of variants so alike they will sound like one (machine-gunning).
Flags print with "!": overcompressed (crest < 9 dB on a transient sound), dull / harsh against the key's median, slow attack on a
contact sound, audible noise floor, near-duplicates, too-wide or phasey stereo (correlation < 0.2), clipped shapes.
"""
import json
import math
import os
import sys

import numpy as np
import soundfile as sf
from scipy import signal

import abuild
import adsp

SR = adsp.SR
BANDS = [("sub", 20, 80), ("low", 80, 250), ("lmid", 250, 800), ("mid", 800, 2500), ("pres", 2500, 6000), ("air", 6000, 20000)]
TRANSIENT_LAYERS = {"close", "mech", "action", "foley", "step", "casing", "impact", "ui", "bodyfall", "flyby"}   # (a sub thump's crest is low by nature)


def bands_db(x):
    m = adsp.to_mono(x)
    f, p = signal.welch(m, SR, nperseg=4096)
    tot = p[(f >= 20)].sum() + 1e-20
    return {n: 10 * math.log10(max(p[(f >= a) & (f < b)].sum() / tot, 1e-12)) for n, a, b in BANDS}, \
        float((f * p).sum() / (p.sum() + 1e-20))


def envelope(x, ms=1.0):
    m = np.abs(adsp.to_mono(x))
    a = math.exp(-1.0 / (ms * 0.001 * SR))
    return signal.lfilter([1 - a], [1, -a], m)


def describe(x):
    m = adsp.to_mono(x)
    pk = float(np.abs(x).max()) + 1e-12
    w = int(0.05 * SR)
    rms50 = max(math.sqrt(float(np.mean(m[i:i + w] ** 2))) for i in range(0, max(1, len(m) - w + 1), w // 4)) if len(m) > w else \
        math.sqrt(float(np.mean(m ** 2)))
    env = envelope(x)
    ip = int(np.argmax(env))
    e10 = int(np.argmax(env >= 0.1 * env[ip]))
    e90 = int(np.argmax(env >= 0.9 * env[ip]))
    pre = m[:max(0, e10 - int(0.002 * SR))]
    pre_db = 20 * math.log10(math.sqrt(float(np.mean(pre ** 2))) / pk + 1e-12) if len(pre) > int(0.004 * SR) else -99.0
    end = m[-int(0.03 * SR):]
    end_db = 20 * math.log10(math.sqrt(float(np.mean(end ** 2))) / pk + 1e-12)
    flat = float(np.mean(np.abs(m) >= pk * 10 ** (-0.1 / 20))) * 1000.0  # per mille
    corr = 1.0
    if x.ndim == 2 and x.shape[1] == 2:
        l, r = x[:, 0], x[:, 1]
        d = math.sqrt(float(np.sum(l * l) * np.sum(r * r))) + 1e-20
        corr = float(np.sum(l * r) / d)
    b, cen = bands_db(x)
    _, lm = adsp.lufs(x)
    return {"lufs_m": lm, "tp": adsp.true_peak_db(x), "crest": 20 * math.log10(pk / (rms50 + 1e-12)), "bands": b, "centroid": cen,
            "attack_ms": (e90 - e10) * 1000.0 / SR, "pre_db": pre_db, "end_db": end_db, "flat_pm": flat, "corr": corr,
            "len_s": len(m) / SR, "env": env}


def similarity(a, b):
    """Envelope (10 ms) and spectrum likeness of two files, 0..1 each."""
    def e(x):
        v = envelope(x, 10.0)[::int(0.005 * SR)]
        return v / (np.linalg.norm(v) + 1e-12)
    ea, eb = e(a), e(b)
    n = min(len(ea), len(eb))
    env_sim = float(np.max(signal.correlate(ea[:n], eb[:n], mode="full"))) if n > 4 else 0.0
    fa = signal.welch(adsp.to_mono(a), SR, nperseg=2048)[1]
    fb = signal.welch(adsp.to_mono(b), SR, nperseg=2048)[1]
    la, lb = np.log10(fa + 1e-14), np.log10(fb + 1e-14)
    la, lb = la - la.mean(), lb - lb.mean()
    spec_sim = float(np.dot(la, lb) / (np.linalg.norm(la) * np.linalg.norm(lb) + 1e-12))
    return env_sim, spec_sim


def report_key(key, files, layer):
    print(f"\n== {key}  ({len(files)} file(s), layer {layer})")
    rows = []
    audio = []
    for f in files:
        x, sr = sf.read(os.path.join(abuild.AUDIO_DIR, f) if not os.path.isabs(f) else f, dtype="float32", always_2d=True)
        audio.append(x)
        rows.append((f, describe(x)))
    med = {n: float(np.median([r["bands"][n] for _, r in rows])) for n, _, _ in BANDS}
    print(f"   {'file':34s} {'LUFS':>6s} {'TP':>5s} {'crest':>5s} {'atk':>5s} {'cent':>5s} " +
          " ".join(f"{n:>5s}" for n, _, _ in BANDS) + f" {'noise':>6s} {'corr':>5s}")
    transient = layer in TRANSIENT_LAYERS
    for f, r in rows:
        flags = []
        if transient and r["crest"] < 9.0:
            flags.append(f"squashed (crest {r['crest']:.1f})")
        if r["flat_pm"] > 2.0:
            flags.append(f"flat-topped ({r['flat_pm']:.1f} per mille at the peak)")
        dev = {n: r["bands"][n] - med[n] for n in med}
        if len(rows) >= 3:
            worst = max(dev, key=lambda n: abs(dev[n]))
            if abs(dev[worst]) > 4.0 and r["bands"][worst] > -30:
                flags.append(f"{worst} {dev[worst]:+.1f} dB vs siblings")
        if transient and r["attack_ms"] > 25 and layer not in ("sub",):
            flags.append(f"soft attack {r['attack_ms']:.0f} ms")
        if max(r["pre_db"], r["end_db"]) > -45 and layer not in ("tail", "far", "ambience", "loop", "ir", "sub"):
            flags.append(f"noise {max(r['pre_db'], r['end_db']):.0f} dB")
        if r["corr"] < (-0.1 if layer in ("tail", "far", "ambience") else 0.2):   # (a diffuse room is decorrelated by nature)
            flags.append(f"phasey stereo ({r['corr']:.2f})")
        print(f"   {os.path.basename(f):34s} {r['lufs_m']:6.1f} {r['tp']:5.1f} {r['crest']:5.1f} {r['attack_ms']:5.1f} {r['centroid'] / 1000:5.1f} " +
              " ".join(f"{r['bands'][n]:5.1f}" for n, _, _ in BANDS) + f" {max(r['pre_db'], r['end_db']):6.0f} {r['corr']:5.2f}" +
              ("  ! " + "; ".join(flags) if flags else ""))
    for i in range(len(audio)):
        for j in range(i + 1, len(audio)):
            es, ss = similarity(audio[i], audio[j])
            if es > 0.97 and ss > 0.97:
                print(f"   ! near-duplicates: {os.path.basename(files[i])} ~ {os.path.basename(files[j])} (envelope {es:.3f}, spectrum {ss:.3f})")
    return rows


def main():
    args = sys.argv[1:]
    if "--files" in args:
        files = args[args.index("--files") + 1:]
        report_key("files", files, "action")
        return
    prefixes = args or [""]
    doc = json.load(open(abuild.MANIFEST))
    by = {}
    for e in doc["files"]:
        if any(e["key"].startswith(p) for p in prefixes) and e.get("layer") != "ir":
            by.setdefault(e["key"], (e.get("layer", ""), []))[1].append(e["file"])
    for key in sorted(by):
        report_key(key, sorted(by[key][1]), by[key][0])


if __name__ == "__main__":
    main()
