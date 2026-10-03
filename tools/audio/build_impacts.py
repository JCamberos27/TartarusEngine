"""Build casing drops, bullet impacts and flybys from recipes/casings_impacts.json (real Sonniss recordings only).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_impacts.py

  project/assets/Audio/Casings/<rifle|shell>/<material>_<n>.wav   key snd.casing.<kind>.<material>
  project/assets/Audio/Impacts/<material>_<n>.wav                 key snd.impact.<material>
  project/assets/Audio/Impacts/flyby_<n>.wav                      key snd.flyby
Manifest extras: kind, material, proxy (true = not a recording of exactly that thing, see docs/AUDIO.md), short.
"""
import json
import math
import os

import numpy as np
from scipy import signal

import abuild
import adsp
import build_elements as be

SR = adsp.SR


def find_drops(x, quiet_ratio=0.18, thr_db=-38, hp=250.0, min_level_db=-22.0):
    """Fresh drops of a long take: onsets of the high-passed envelope preceded by 0.3 s of near silence
    (a bounce / roll is not 'fresh'). Returns (onset sample indices, envelope)."""
    m = adsp.to_mono(x)
    h = np.abs(signal.sosfilt(signal.butter(2, hp, "hp", fs=SR, output="sos"), m))
    a = math.exp(-1 / (0.003 * SR))      # envelope-follower coefficient (analysis only, never audio)
    env = signal.lfilter([1 - a], [1, -a], h)
    pk, _ = signal.find_peaks(env, height=env.max() * 10 ** (thr_db / 20), distance=int(0.04 * SR))
    out = []
    for p in pk:
        a0, a1 = max(0, p - int(0.30 * SR)), max(0, p - int(0.02 * SR))
        prev = env[a0:a1].max() if a1 > a0 else 0.0
        if prev < quiet_ratio * env[p] and 20 * np.log10(env[p] / env.max()) >= min_level_db:
            out.append(int(adsp.find_onset(x, p / SR - 0.004, back_ms=20, fwd_ms=30, frac=0.25, hp=hp)))
    return sorted(set(out)), env


def cut_drop(x, env, start, nxt, long):
    a = max(0, start - int(0.002 * SR))
    if long:
        end = min(nxt - int(0.01 * SR) if nxt else len(x), start + int(1.6 * SR), len(x))
        quiet = env[start:end] < env[start:end].max() * 10 ** (-55 / 20)          # natural end: 55 dB down for 100 ms
        run = int(0.1 * SR)
        cs = np.convolve(quiet.astype(np.int32), np.ones(run, np.int32), mode="valid")
        hit = np.nonzero(cs >= run)[0]
        if hit.size:
            end = min(end, start + int(hit[0]) + int(0.05 * SR))
        end = max(end, start + int(0.3 * SR))
        fo = int(0.15 * SR)
    else:
        end, fo = min(start + int(0.28 * SR), len(x)), int(0.08 * SR)
    return adsp.fade(adsp.cut(x, a, end), int(0.001 * SR), fo)


def post(y, v):
    y = adsp.pitch(y, v.get("pitch", 0.0))
    y = be.apply_eq(y, v)
    return adsp.gain_db(y, v.get("gain_db", 0.0))


def pick_spread(items, n):
    if n >= len(items):
        return list(items)
    return [items[int(round(i * (len(items) - 1) / max(1, n - 1)))] for i in range(n)]


def point_source(x):
    """Mono of a stereo take for a 3D voice. A spaced-pair recording of a small source is decorrelated (or out of phase): folding
    it to mid thins it and comb-filters, so such a take keeps its louder channel; a coherent one is folded."""
    l, r = x[:, 0], x[:, 1]
    corr = float(np.sum(l * r) / (math.sqrt(float(np.sum(l * l) * np.sum(r * r))) + 1e-20))
    if corr >= 0.5:
        m = adsp.to_mono(x)
    else:
        m = l if float(np.sum(l * l)) >= float(np.sum(r * r)) else r
    return np.repeat(np.asarray(m, np.float32)[:, None], 2, axis=1)


def casing_takes(spec, used):
    """-> list of (audio, note, short, sources). `used` = drops (source, onset) already cut for this key: a key never gets one
    drop twice (a long and a short cut of the same drop sound like one variant), so it gets fewer variants when the take is short."""
    x = be.src(spec["src"])
    name = os.path.basename(adsp.resolve_sonniss(spec["src"]))
    out = []
    if spec.get("single"):
        for k, v in enumerate(spec.get("variants", [{}])):
            adsp.take_uses()
            y = adsp.cut(x, 0, len(x) if "dur" not in v else int(v["dur"] * SR))
            y = adsp.fade(y, int(0.001 * SR), int(0.12 * SR))
            out.append((post(y, {**spec, **v}), f"{name} {json.dumps(v)}", False, adsp.take_uses()))
        return out
    drops, env = find_drops(x)
    nxt = {d: (drops[i + 1] if i + 1 < len(drops) else None) for i, d in enumerate(drops)}
    nl, ns = spec["drops"]["long"], spec["drops"]["short"]
    free = [d for d in drops if (spec["src"], d) not in used]
    longs = pick_spread(free, nl)
    rest = [d for d in free if d not in longs]
    shorts = pick_spread(rest[::-1], ns)
    used.update((spec["src"], d) for d in longs + shorts)
    for d in longs:
        adsp.take_uses()
        out.append((post(cut_drop(x, env, d, nxt[d], True), spec), f"{name} drop@{d / SR:.2f}s bounce", False, adsp.take_uses()))
    for d in shorts:
        adsp.take_uses()
        out.append((post(cut_drop(x, env, d, nxt[d], False), spec), f"{name} drop@{d / SR:.2f}s short", True, adsp.take_uses()))
    return out


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "casings_impacts.json")))
    entries = []

    def emit(rel, y, key, layer, extra, uses):
        mono = layer == "casing"                         # a casing is a point source in 3D: ship it mono
        if mono:
            y = point_source(y)
        y = abuild.finish(y, layer)
        e = abuild.emit(rel, y, key, layer, extra, mono=mono, sources=uses)
        entries.append(e)
        print(f"{rel:44s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s"
              f"{'  proxy' if e.get('proxy') else ''}")

    for kind, mats in cfg["casings"].items():
        used = set()                                     # per kind: a proxy material takes the drops a real one left
        for mat, specs in mats.items():
            n = 0
            for spec in specs:
                for y, note, short, uses in casing_takes(spec, used):
                    n += 1
                    emit(f"Casings/{kind}/{mat}_{n}.wav", y, f"snd.casing.{kind}.{mat}", "casing",
                         {"kind": kind, "material": mat, "variant": n, "short": short, "anchor_ms": 0.0,
                          "proxy": bool(spec.get("proxy")), "source": note}, uses)
    for mat, m in cfg["impacts"].items():
        n = 0
        for spec in m["sources"]:
            x = be.src(spec["src"])
            for v in spec["variants"]:
                n += 1
                adsp.take_uses()
                y = adsp.cut(x, 0, len(x) if "dur" not in v else int(v["dur"] * SR))
                y = adsp.fade(y, int(0.0008 * SR), int(0.12 * SR))
                uses = adsp.take_uses()
                emit(f"Impacts/{mat}_{n}.wav", post(y, v), f"snd.impact.{mat}", "impact",
                     {"material": mat, "variant": n, "anchor_ms": 0.0, "proxy": bool(m.get("proxy")),
                      "source": f"{os.path.basename(adsp.resolve_sonniss(spec['src']))} {json.dumps(v)}"}, uses)
    n = 0
    for spec in cfg["flyby"]["sources"]:
        x = be.src(spec["src"])
        for v in spec["variants"]:
            n += 1
            adsp.take_uses()
            y = adsp.fade(adsp.cut(x, 0, len(x)), int(0.003 * SR), int(0.1 * SR))
            uses = adsp.take_uses()
            emit(f"Impacts/flyby_{n}.wav", post(y, v), "snd.flyby", "flyby",
                 {"variant": n, "anchor_ms": 0.0, "source": f"{os.path.basename(adsp.resolve_sonniss(spec['src']))} {json.dumps(v)}"}, uses)
    print("manifest files:", abuild.update_manifest(entries, ["Casings/", "Impacts/"]))


if __name__ == "__main__":
    main()
