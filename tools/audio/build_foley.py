"""Build player foley from recipes/foley.json: weapon handling, jump/land, footsteps x 6 surfaces.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_foley.py

Writes project/assets/Audio/Foley/<category>/<element>_<n>.wav, key snd.foley.<category>.<element>.
Categories: weapon (ads_in ads_out equip unequip firemode), move (jump land_light land_heavy),
step_<surface> (walk run land).
"""
import glob
import json
import os

import numpy as np
from scipy import signal

import abuild
import adsp
import build_elements as be

SR = adsp.SR


STEP_BANDS = ((40, 120), (120, 300), (300, 900), (900, 2500), (2500, 6000), (6000, 16000))


def step_features(seg):
    """(band profile dB re the total, unit envelope of the first 250 ms at 4 ms) of one footstep."""
    m = adsp.to_mono(seg)
    f, p = signal.welch(m, SR, nperseg=2048)
    tot = p[f >= 40].sum() + 1e-20
    bands = np.array([10 * np.log10(max(p[(f >= a) & (f < b)].sum() / tot, 1e-12)) for a, b in STEP_BANDS])
    env = np.abs(m)
    k = int(0.004 * SR)
    env = np.array([env[i:i + k].max() for i in range(0, min(len(env), int(0.25 * SR)), k)])
    return bands, env / (np.linalg.norm(env) + 1e-12)


def diverse_steps(x, peaks, n, dur, seed):
    """Of the isolated steps at `peaks` (sample indices), the n that sound most unlike each other while staying in one voice:
    steps whose band balance strays more than 3 dB from the pool's median (another shoe angle, a scuff, a knock) are dropped,
    then farthest-point selection on the analyzer's likeness (analyze_audio.similarity: fine envelope + the spectral character
    left after removing the pool's mean spectrum) picks the set, starting from the most typical step. Deterministic.
    Returns indices into `peaks`."""
    import analyze_audio as aa
    segs, feats = [], []
    for t in peaks:
        a = max(0, t - int(0.04 * SR))
        seg = np.asarray(x[a:a + int(dur * SR)])
        segs.append(seg)
        feats.append(step_features(seg))
    if len(feats) <= n:
        return list(range(len(feats)))
    B = np.array([b for b, _ in feats])
    med = np.median(B, axis=0)
    loud = med > -25.0                                   # only the bands a step actually has
    ok = [i for i in range(len(feats)) if np.all(np.abs(B[i] - med)[loud] <= 3.0)]
    if len(ok) < n:
        ok = sorted(range(len(feats)), key=lambda i: float(np.abs(B[i] - med)[loud].max()))[:max(n, len(ok))]
    group = np.mean([aa.log_spectrum(segs[i]) for i in ok], axis=0)
    memo = {}

    def dist(i, j):
        k = (min(i, j), max(i, j))
        if k not in memo:
            es, ss = aa.similarity(segs[i], segs[j], group)
            memo[k] = (1.0 - es) + 0.25 * (1.0 - ss)
        return memo[k]
    start = min(ok, key=lambda i: float(np.abs(B[i] - med)[loud].mean()))
    chosen = [start]
    while len(chosen) < n:
        best = max((i for i in ok if i not in chosen), key=lambda i: min(dist(i, c) for c in chosen))
        chosen.append(best)
    return sorted(chosen)


def pick_steps(x, n, dur, isolated_gap, seed, lo_db=-4.5):
    """Cut n isolated, similar-level footsteps out of a long walk/run sequence."""
    m = adsp.to_mono(x)
    h = np.abs(signal.sosfilt(signal.butter(2, 150, "hp", fs=SR, output="sos"), m))
    env = signal.sosfilt(*[signal.butter(2, 60, "lp", fs=SR, output="sos")], h)
    env = np.maximum(env, 0)
    pk, _ = signal.find_peaks(env, height=0.2 * env.max(), distance=int(0.2 * SR))
    lv = 20 * np.log10(env[pk] / env.max())
    good = []
    for i in range(1, len(pk) - 1):
        if (pk[i] - pk[i - 1]) / SR >= isolated_gap and (pk[i + 1] - pk[i]) / SR >= isolated_gap * 0.7 and lv[i] >= lo_db:
            good.append(i)
    good = [good[j] for j in diverse_steps(x, [pk[i] for i in good], n, dur, seed)]
    out = []
    for i in good:
        t = pk[i] / SR
        on = adsp.find_onset(x, t - 0.04, back_ms=45, fwd_ms=60, frac=0.25, hp=150)
        a = max(0, on - int(0.004 * SR))
        s = adsp.cut(x, a, a + int(dur * SR))
        out.append((adsp.fade(s, int(0.001 * SR), int(max(0.04, dur * 0.35) * SR)), t, adsp.take_uses()))
    return out


def surf_files(surface, kind):
    return sorted(glob.glob(os.path.join(adsp.BOOTS, surface, f"*Boots {kind}*")))


def build_steps(cfg, entries):
    st = cfg["steps"]
    for surface in st["surfaces"]:
        cat = f"step_{surface.lower()}"
        base = f"Foley/{cat}"
        seqW = surf_files(surface, f"Walk {surface} Sequence")
        seqR = surf_files(surface, f"Run {surface} Sequence")
        singles = surf_files(surface, f"{surface} Single Step")
        stomps = surf_files(surface, f"{surface} Stomp")
        jobs = {"walk": [], "run": [], "land": []}
        if seqW:
            x = adsp.load(seqW[0])
            adsp.take_uses()
            for s, t, u in pick_steps(x, st["walk"]["seq_variants"], st["walk"]["dur"], 0.55, surface + "w"):
                jobs["walk"].append((s, f"{os.path.basename(seqW[0])}@{t:.2f}s", u))
        for f in singles[:st["walk"]["single_variants"]]:
            x = adsp.load(f)
            on = adsp.find_onset(x, 0.05, back_ms=50, fwd_ms=200, frac=0.25, hp=150)
            a0 = max(0, on - int(0.004 * SR))
            s = adsp.fade(adsp.cut(x, a0, a0 + int(st["walk"]["dur"] * SR)), int(0.001 * SR), int(0.12 * SR))
            jobs["walk"].append((s, os.path.basename(f), adsp.take_uses()))
        if seqR:
            x = adsp.load(seqR[0])
            for s, t, u in pick_steps(x, st["run"]["seq_variants"], st["run"]["dur"], 0.22, surface + "r", lo_db=-6.0):
                jobs["run"].append((s, f"{os.path.basename(seqR[0])}@{t:.2f}s", u))
        for f in stomps[:st["land"]["stomp_variants"]]:
            x = adsp.load(f)
            on = adsp.find_onset(x, 0.05, back_ms=50, fwd_ms=200, frac=0.25, hp=150)
            a0 = max(0, on - int(0.004 * SR))
            s = adsp.fade(adsp.cut(x, a0, a0 + int(st["land"]["dur"] * SR)), int(0.001 * SR), int(0.18 * SR))
            jobs["land"].append((s, os.path.basename(f), adsp.take_uses()))
        if surface == "Concrete":                          # the Tactical pack's own concrete steps as extra variants
            for kind in ("walk", "run"):
                for p in st["general_concrete"][kind]:
                    x = adsp.load(p)
                    on = adsp.find_onset(x, 0.03, back_ms=30, fwd_ms=150, frac=0.25, hp=150)
                    a0 = max(0, on - int(0.004 * SR))
                    s = adsp.fade(adsp.cut(x, a0, a0 + int(0.30 * SR)), int(0.001 * SR), int(0.1 * SR))
                    jobs[kind].append((s, p.replace("\\", "/").split("/")[-1], adsp.take_uses()))
        for kind, lst in jobs.items():
            tgt = st[kind]["target_lufs"]
            for n, (s, note, uses) in enumerate(lst, 1):
                y = abuild.finish(s, "step" if kind != "land" else "foley", target=tgt)
                e = abuild.emit(f"{base}/{kind}_{n}.wav", y, f"snd.foley.{cat}.{kind}", "step" if kind != "land" else "foley",
                                {"element": kind, "category": cat, "variant": n, "source": note, "surface": surface.lower()}, sources=uses)
                entries.append(e)
                print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")


def land_heavy(v):
    """Hard landing: the pack's landing + a REAL low thud (Shapeforms impact) under it + gear rattle from the AK pack."""
    y = be.slice_variant(v)
    n = len(y)
    th = be.slice_variant(v["thump"])
    r = adsp.slice_at(be.src(v["rattle"]["src"]), v["rattle"]["t"], 0.18)
    r = adsp.gain_db(adsp.pitch(r, -2.0), -4.0)
    y = adsp.mix([(y, 0, 0.0), (th, 0, v["thump"].get("gain_db", -2.0)), (r, int(0.018 * SR), 0.0)], n)
    return adsp.fade(y, int(0.0005 * SR), int(0.12 * SR))


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "foley.json")))
    entries = []
    for cat, elems in cfg["one_shots"].items():
        for elem, variants in elems.items():
            for n, v in enumerate(variants, 1):
                adsp.take_uses()
                if "thump" in v:
                    y = land_heavy(v)
                else:
                    y = be.render_variant(v)
                uses = adsp.take_uses()
                y = abuild.finish(y, "foley", trim_bleed=not elem.startswith("land"))   # (a landing's rattle after the thud is meant)
                extra = {"element": elem, "category": cat, "variant": n, "source": be.describe(v)}
                if not elem.startswith("land"):
                    extra["anchor_ms"] = 0.0           # handling / jump cloth: starts with the action, no contact
                e = abuild.emit(f"Foley/{cat}/{elem}_{n}.wav", y, f"snd.foley.{cat}.{elem}", "foley", extra, sources=uses)
                entries.append(e)
                print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    build_steps(cfg, entries)
    print("manifest files:", abuild.update_manifest(entries, ["Foley/"]))


if __name__ == "__main__":
    main()
