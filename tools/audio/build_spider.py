"""Build the enemy spider's sounds from recipes/spider.json: taps, hiss, screech, bite, pounce, hurt, death, thud, splat.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_spider.py

Writes project/assets/Audio/Creatures/spider/<element>_<n>.wav (mono, 48 kHz), key snd.foley.spider.<element>.
Sources are the Drive's Raw\\Audio folder (TARTARUS_AUDIO_SRC is its audio-src; the footstep libraries sit beside it):
the High Heels steps pitched up into chitin taps, the Sonniss / ColdBore / PMSFX hits and whooshes, the CC0 creature takes
in audio-src\\spider (credits in its CREDITS.md). A hiss is synthesised: band-passed noise with a rattle (tremolo) in it.
Each layer is cut at the onset at or after `t`, pitched, filtered, placed and summed; the sum loses its quiet tail, is
faded, and levelled per element (target_lufs_m).
"""
import json
import os

import numpy as np

import abuild
import adsp

SR = adsp.SR
RAW = os.path.dirname(adsp.SRC_ROOT.rstrip("\\/"))   # Raw\Audio: audio-src and Footsteps
_cache = {}


def src(name):
    if name not in _cache:
        _cache[name] = adsp.load(os.path.join(RAW, name))
    return _cache[name]


def noise(L, rng):
    n = int(L["noise"] * SR)
    x = rng.standard_normal((n, 2)).astype(np.float32) * 0.25
    lo, hi = L.get("band", [2500, 9000])
    x = adsp.filt(adsp.filt(x, "hp", lo, order=4), "lp", hi, order=4)
    t = np.arange(n) / SR
    env = np.minimum(1.0, t / max(L.get("attack", 0.02), 1e-3)) * np.exp(-np.maximum(0.0, t - L.get("attack", 0.02)) / max(L.get("release", 0.3), 1e-3))
    if L.get("tremolo", 0):
        env *= 0.6 + 0.4 * np.abs(np.sin(np.pi * L["tremolo"] * t))
    return (x * env[:, None]).astype(np.float32)


def layer(L, rng):
    if "noise" in L:
        s = noise(L, rng)
    else:
        x = src(L["src"])
        a = int(L["t"] * SR)
        on = adsp.first_onset(np.asarray(x[a:a + int(L["dur"] * SR)]), frac=0.05, hp=150.0)
        a0 = max(0, a + on - int(0.002 * SR))
        s = adsp.cut(x, a0, a0 + int(L["dur"] * SR))
        if L.get("pitch"):
            s = adsp.pitch(s, L["pitch"])
        s = adsp.fade(s, int(0.001 * SR), int(min(0.02, 0.3 * len(s) / SR) * SR))
    if L.get("hp"):
        s = adsp.filt(s, "hp", L["hp"])
    if L.get("lp"):
        s = adsp.filt(s, "lp", L["lp"])
    return s


def trim_tail(x, floor_db):
    m = np.abs(adsp.to_mono(x))
    h = int(0.01 * SR)
    n = len(m) // h
    env = m[:n * h].reshape(n, h).max(axis=1)
    above = np.nonzero(env >= env.max() * 10 ** (floor_db / 20))[0]
    end = min(len(x), (int(above[-1]) + 1) * h + int(0.02 * SR)) if len(above) else len(x)
    return x[:end]


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "spider.json")))
    entries = []
    for elem, variants in cfg["elements"].items():
        for n, v in enumerate(variants, 1):
            rng = adsp.deterministic_rng("spider", elem, n)
            parts = [(layer(L, rng), int(L.get("at", 0.0) * SR), L.get("db", 0.0)) for L in v]
            uses = adsp.take_uses()
            y = trim_tail(adsp.mix(parts), cfg["tail_floor_db"])
            y = adsp.fade(y, 0, int(min(0.06, 0.25 * len(y) / SR) * SR))
            y = abuild.finish(y, "foley", target=cfg["target_lufs_m"][elem])
            note = " + ".join(f"noise {L['band'][0]}-{L['band'][1]}" if "noise" in L else f"{os.path.basename(L['src'])}@{L['t']:.2f}{'' if not L.get('pitch') else ' %+gst' % L['pitch']}" for L in v)
            e = abuild.emit(f"Creatures/spider/{elem}_{n}.wav", y, f"snd.foley.spider.{elem}", "foley",
                            {"element": elem, "category": "spider", "variant": n, "source": note}, mono=True, sources=uses)
            entries.append(e)
            print(f"{e['file']:34s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  {note[:90]}")
    n = abuild.update_manifest(entries, ["Creatures/spider/"])
    print(f"{len(entries)} spider files; manifest {n} entries")


if __name__ == "__main__":
    main()
