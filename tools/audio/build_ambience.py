"""Build the seamless ambience loops from recipes/ambience.json (real recordings only).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_ambience.py

  project/assets/Audio/Ambience/<class>_<n>.wav     key snd.amb.<class>, layer ambience, loop true
Sources live in C:\\tb\\audio-src\\amb (never committed).
"""
import json
import os

import numpy as np

import abuild
import adsp

SR = adsp.SR
AMB_ROOT = os.path.join(adsp.SRC_ROOT, "amb")
_cache = {}


def src(name):
    if name not in _cache:
        _cache[name] = adsp.load(os.path.join(AMB_ROOT, name))
    return _cache[name]


def steadiest(x, span, n_pick, block_s=0.5, stride_s=1.0):
    """Start samples of the `n_pick` steadiest non-overlapping stretches of `span` samples: the lowest spread of 0.5 s block loudness
    (no events, no gusts); the high-passed signal, so rumble does not decide."""
    m = adsp.to_mono(adsp.filt(np.asarray(x), "hp", 60.0))
    b = int(block_s * SR)
    nb = len(m) // b
    db = 10 * np.log10(np.maximum((m[:nb * b].reshape(nb, b) ** 2).mean(axis=1), 1e-12))
    k = int(span / b)
    cands = []
    for s in range(0, nb - k, max(1, int(stride_s / block_s))):
        w = db[s:s + k]
        cands.append((float(w.max() - w.min()) + 0.5 * float(np.abs(np.diff(w)).max()), s * b))
    cands.sort()
    picks = []
    for _, s in cands:
        if all(abs(s - q) >= span for q in picks):
            picks.append(s)
        if len(picks) == n_pick:
            break
    return picks


def loop_of(x, start, loop_n, xf_n):
    """loop_n samples that wrap seamlessly: the stretch's first xf_n samples are an equal-power blend of its head with the
    material that follows its end (equal power: sqrt of complementary raised-cosine windows)."""
    seg = adsp.cut(x, start, start + loop_n + xf_n)
    seg = adsp.filt(seg, "hp", 20.0)
    up = np.hanning(2 * xf_n)[:xf_n].astype(np.float32)          # rises 0 -> 1 (a window, not a signal)
    out = seg[:loop_n].copy()
    out[:xf_n] = out[:xf_n] * np.sqrt(up)[:, None] + seg[loop_n:loop_n + xf_n] * np.sqrt(1.0 - up)[:, None]
    return out


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "ambience.json")))
    loop_n, xf_n = int(cfg["loop_s"] * SR), int(cfg["xfade_s"] * SR)
    entries = []
    for cls, c in cfg["classes"].items():
        for n, v in enumerate(c["variants"], 1):
            adsp.take_uses()
            parts, notes = [], []
            for l in v["layers"]:
                x = src(l["src"])
                picks = steadiest(x, loop_n + xf_n, l.get("pick", 0) + 1)
                st = picks[min(l.get("pick", 0), len(picks) - 1)]
                parts.append(adsp.gain_db(loop_of(x, st, loop_n, xf_n), l.get("gain_db", 0.0)))
                notes.append(f"{l['src']}@{st / SR:.1f}s")
            y = parts[0]
            for p in parts[1:]:
                y = y + p
            uses = adsp.take_uses()
            y = abuild.finish(y, "ambience", loop=True)
            rel = f"Ambience/{cls}_{n}.wav"
            e = abuild.emit(rel, y, f"snd.amb.{cls}", "ambience",
                            {"variant": n, "loop": True, "class": cls, "source": " + ".join(notes),
                             "license": "Sonniss GDC bundle (royalty free) / Ocular The Provence (own library)"}, sources=uses)
            entries.append(e)
            print(f"{rel:30s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.1f}s  {e['source']}")
    print("manifest files:", abuild.update_manifest(entries, ["Ambience/"]))


if __name__ == "__main__":
    main()
