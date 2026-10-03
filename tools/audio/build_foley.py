"""Build player foley from recipes/foley.json: weapon handling, jump/land, sprint loop, footsteps x 6 surfaces.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_foley.py

Writes project/assets/Audio/Foley/<category>/<element>_<n>.wav, key snd.foley.<category>.<element>.
Categories: weapon (ads_in ads_out equip unequip firemode), move (jump land_light land_heavy sprint_loop),
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
    rng = adsp.deterministic_rng("steps", seed)
    rng.shuffle(good)
    # spread: take steps from different parts of the file
    good = sorted(good[:n * 2])[::2][:n] if len(good) >= n * 2 else sorted(good[:n])
    out = []
    for i in good:
        t = pk[i] / SR
        on = adsp.find_onset(x, t - 0.04, back_ms=45, fwd_ms=60, frac=0.25, hp=150)
        a = max(0, on - int(0.004 * SR))
        s = x[a:a + int(dur * SR)].copy()
        out.append((adsp.fade(s, int(0.001 * SR), int(max(0.04, dur * 0.35) * SR)), t))
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
            for s, t in pick_steps(x, st["walk"]["seq_variants"], st["walk"]["dur"], 0.55, surface + "w"):
                jobs["walk"].append((s, f"{os.path.basename(seqW[0])}@{t:.2f}s"))
        for f in singles[:st["walk"]["single_variants"]]:
            x = adsp.load(f)
            on = adsp.find_onset(x, 0.05, back_ms=50, fwd_ms=200, frac=0.25, hp=150)
            s = adsp.fade(x[max(0, on - int(0.004 * SR)):][:int(st["walk"]["dur"] * SR)], int(0.001 * SR),
                          int(0.12 * SR))
            jobs["walk"].append((s, os.path.basename(f)))
        if seqR:
            x = adsp.load(seqR[0])
            for s, t in pick_steps(x, st["run"]["seq_variants"], st["run"]["dur"], 0.22, surface + "r", lo_db=-6.0):
                jobs["run"].append((s, f"{os.path.basename(seqR[0])}@{t:.2f}s"))
        for f in stomps[:st["land"]["stomp_variants"]]:
            x = adsp.load(f)
            on = adsp.find_onset(x, 0.05, back_ms=50, fwd_ms=200, frac=0.25, hp=150)
            s = adsp.fade(x[max(0, on - int(0.004 * SR)):][:int(st["land"]["dur"] * SR)], int(0.001 * SR), int(0.18 * SR))
            jobs["land"].append((s, os.path.basename(f)))
        if surface == "Concrete":                          # the Tactical pack's own concrete steps as extra variants
            for kind in ("walk", "run"):
                for p in st["general_concrete"][kind]:
                    x = adsp.load(p)
                    on = adsp.find_onset(x, 0.03, back_ms=30, fwd_ms=150, frac=0.25, hp=150)
                    s = adsp.fade(x[max(0, on - int(0.004 * SR)):][:int(0.30 * SR)], int(0.001 * SR), int(0.1 * SR))
                    jobs[kind].append((s, p.replace("\\", "/").split("/")[-1]))
        for kind, lst in jobs.items():
            tgt = st[kind]["target_lufs"]
            for n, (s, note) in enumerate(lst, 1):
                y = abuild.finish(s, "step" if kind != "land" else "foley", target=tgt)
                e = abuild.emit(f"{base}/{kind}_{n}.wav", y, f"snd.foley.{cat}.{kind}", "step" if kind != "land" else "foley",
                                {"element": kind, "category": cat, "variant": n, "source": note, "surface": surface.lower()})
                entries.append(e)
                print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")


def land_heavy(v):
    y = be.slice_variant(v)
    n = len(y)
    t = np.arange(n) / SR
    thump = np.sin(2 * np.pi * v["thump_hz"] * (1 + 0.8 * np.exp(-t / 0.04)) * t) * np.exp(-t / 0.09)
    thump = np.stack([thump, thump], 1).astype(np.float32)
    r = adsp.slice_at(be.src(v["rattle"]["src"]), v["rattle"]["t"], 0.18)
    r = adsp.pitch(r, -4.0)
    r = adsp.gain_db(r, -4.0)
    y = adsp.mix([(y, 0, 0.0), (adsp.gain_db(thump, -3.0), 0, 0.0), (r, int(0.018 * SR), 0.0)], n)
    return adsp.fade(y, int(0.0005 * SR), int(0.12 * SR))


def sprint_loop(v):
    """Cloth bed + gear rattle with `steps` footfall pulses per loop; every operation is circular => seamless."""
    r = adsp.deterministic_rng("sprint", v["seed"])
    n = int(v["dur"] * SR)
    t = np.arange(n) / SR

    def circ_filter(sig, sos):
        return signal.sosfilt(sos, np.tile(sig, 3))[n:2 * n]
    lo, hi = v["bed_hz"]
    bed = circ_filter(r.standard_normal(n), signal.butter(2, [lo, hi], "bp", fs=SR, output="sos"))
    swish = circ_filter(r.standard_normal(n), signal.butter(2, [180, 420], "bp", fs=SR, output="sos")) * 0.8
    ph = r.random() * 2 * np.pi
    pulse = (0.5 + 0.5 * np.cos(2 * np.pi * v["steps"] * t / v["dur"] + ph)) ** 1.6
    out = (bed * (0.25 + 0.9 * pulse) + swish * pulse).astype(np.float32)
    out = np.stack([out, np.roll(out, 23)], 1)                 # tiny inter-channel offset = width
    donors = [("AK105/Actions/S_AK105_Reload_Empty.WAV", 3.994, 0.22), ("AK105/Actions/S_AK105_Reload_Empty.WAV", 3.926, 0.28),
              ("AK105/Actions/S_AK105_Inspect.WAV", 4.322, 0.30), ("AK105/Actions/S_AK105_MagCheck.WAV", 3.720, 0.25)]
    for k in range(v["rattle"]):
        p, tt, d = donors[int(r.integers(len(donors)))]
        s = adsp.slice_at(be.src(p), tt, d)
        s = adsp.normalize_peak(s, -9.0)
        s = adsp.gain_db(adsp.pitch(s, float(r.uniform(-3, 1))), float(r.uniform(-9, -3)))
        pos = int((k / v["rattle"] + r.uniform(-0.04, 0.04)) * n) % n
        padded = np.zeros((n, 2), np.float32)
        m = min(len(s), n)
        padded[:m] = s[:m]
        out += np.roll(padded, pos, axis=0)                     # wraps => circular
    return out


def main():
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "foley.json")))
    entries = []
    for cat, elems in cfg["one_shots"].items():
        for elem, variants in elems.items():
            for n, v in enumerate(variants, 1):
                if "thump_hz" in v:
                    y = land_heavy(v)
                else:
                    y = be.render_variant(v)
                y = abuild.finish(y, "foley")
                extra = {"element": elem, "category": cat, "variant": n, "source": be.describe(v)}
                if not elem.startswith("land"):
                    extra["anchor_ms"] = 0.0           # handling / jump cloth: starts with the action, no contact
                e = abuild.emit(f"Foley/{cat}/{elem}_{n}.wav", y, f"snd.foley.{cat}.{elem}", "foley", extra)
                entries.append(e)
                print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    for elem, variants in cfg["loops"].items():
        for n, v in enumerate(variants, 1):
            y = sprint_loop(v)
            y = abuild.finish(y, "loop", loop=True)
            e = abuild.emit(f"Foley/move/{elem}_{n}.wav", y, f"snd.foley.move.{elem}", "loop",
                            {"element": elem, "category": "move", "variant": n, "loop": True, "anchor_ms": 0.0,
                             "source": f"synth seed={v['seed']} steps_per_loop={v['steps']}"})
            entries.append(e)
            print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s loop")
    build_steps(cfg, entries)
    print("manifest files:", abuild.update_manifest(entries, ["Foley/"]))


if __name__ == "__main__":
    main()
