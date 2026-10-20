"""Build the interior door foley from recipes/doors.json: open, close, latch, rattle.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_doors.py

Writes project/assets/Audio/Foley/door/<element>_<n>.wav (mono, 48 kHz), key snd.foley.door.<element>.
Sources live in C:\\tb\\audio-src\\doors (Freesound CC0 takes, never committed; credits in Foley/door/CREDITS.md).
Each layer is cut from its recording at the onset at or after `t` (4 ms pre-roll), high-passed, placed, summed; the sum is
cut where it falls `tail_floor_db` under its peak (no room-tone tail), faded, and levelled to the step_wood takes.
"""
import json
import os

import numpy as np

import abuild
import adsp

SR = adsp.SR
DOOR_ROOT = os.path.join(adsp.SRC_ROOT, "doors")
_cache = {}


def src(name):
    if name not in _cache:
        _cache[name] = adsp.load(os.path.join(DOOR_ROOT, name))
    return _cache[name]


def layer(L, hp_default):
    x = src(L["src"])
    a = int(L["t"] * SR)
    on = adsp.first_onset(np.asarray(x[a:a + int(L["dur"] * SR)]), frac=0.05, hp=150.0)   # a plain slice: not a recorded use
    a0 = max(0, a + on - int(0.004 * SR))
    s = adsp.cut(x, a0, a0 + int(L["dur"] * SR))
    s = adsp.filt(s, "hp", L.get("hp_hz", hp_default))
    return adsp.fade(s, int(0.001 * SR), 0)


def trim_tail(x, floor_db):
    """Cut the end where the 10 ms envelope last stands within floor_db of its peak (+ 25 ms), so no room tone trails."""
    m = np.abs(adsp.to_mono(x))
    h = int(0.01 * SR)
    n = len(m) // h
    env = m[:n * h].reshape(n, h).max(axis=1)
    above = np.nonzero(env >= env.max() * 10 ** (floor_db / 20))[0]
    end = min(len(x), (int(above[-1]) + 1) * h + int(0.025 * SR)) if len(above) else len(x)
    return x[:end]


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "doors.json")))
    entries = []
    for elem, variants in cfg["elements"].items():
        for n, v in enumerate(variants, 1):
            parts = [(layer(L, cfg["hp_hz"]), int(L.get("at", 0.0) * SR), L.get("db", 0.0)) for L in v]
            uses = adsp.take_uses()
            y = trim_tail(adsp.mix(parts), cfg["tail_floor_db"])
            fo = cfg.get("fade_out_ms", {}).get(elem)
            if fo:
                y = adsp.fade(y, 0, int(min(fo * 0.001, 0.4 * len(y) / SR) * SR))
            y = abuild.finish(y, "foley", target=cfg["target_lufs_m"])
            note = " + ".join(f"{L['src'].split('__')[0]}@{L['t']:.2f}" for L in v)
            e = abuild.emit(f"Foley/door/{elem}_{n}.wav", y, f"snd.foley.door.{elem}", "foley",
                            {"element": elem, "category": "door", "variant": n, "source": note}, mono=True, sources=uses)
            entries.append(e)
            print(f"{e['file']:28s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  {note}")
    n = abuild.update_manifest(entries, ["Foley/door/"])
    print(f"{len(entries)} door files; manifest {n} entries")


if __name__ == "__main__":
    main()
