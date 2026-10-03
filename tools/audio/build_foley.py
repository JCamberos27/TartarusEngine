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


def sprint_loop(v):
    """Sprint gear loop from REAL cloth movement + gear rattle: `steps` footfall pulses per loop, each a real cloth slice
    (plus a half-step swish) and rattle hits; everything is placed with a circular wrap so the loop point is seamless."""
    r = adsp.deterministic_rng("sprint", v["seed"])
    n = int(v["dur"] * SR)
    cloths = [("AK105/Actions/S_AK105_Draw.WAV", 0.30, 0.45), ("AK105/Actions/S_AK105_Holster.WAV", 0.43, 0.45),
              ("AK105/Actions/S_AK105_Inspect.WAV", 0.24, 0.45), ("sonniss/Shapeforms/CLOTHING_MATERIAL_MOVEMENT_08", 0.0, 0.33),
              ("Herrington_11-87/Actions/S_Herrington_11-87_Draw.WAV", 0.10, 0.5)]
    out = np.zeros((n, 2), np.float32)

    def place(s, t, gain):
        padded = np.zeros((n, 2), np.float32)
        m = min(len(s), n)
        padded[:m] = s[:m]
        nonlocal out
        pos = int(t * n) % n
        if pos < int(0.04 * SR) or pos > n - int(0.04 * SR):     # no onset on the loop point itself
            pos = int(0.06 * SR)
        out += np.roll(padded, pos, axis=0) * 10 ** (gain / 20)
    for k in range(v["steps"]):
        for half, g in ((0.0, 0.0), (0.5, -5.0)):
            p, tt, d = cloths[int(r.integers(len(cloths)))]
            s = adsp.slice_at(be.src(p), tt, d, snap=False, fade_in_ms=40)
            s = adsp.normalize_peak(s, -6.0)
            s = adsp.pitch(s, float(r.uniform(-1.5, 1.5)))
            place(s, (k + half) / v["steps"] + float(r.uniform(-0.03, 0.03)), g + float(r.uniform(-2, 0)))
    donors = [("AK105/Actions/S_AK105_Reload_Empty.WAV", 3.994, 0.22), ("AK105/Actions/S_AK105_Reload_Empty.WAV", 3.926, 0.28),
              ("AK105/Actions/S_AK105_Inspect.WAV", 4.322, 0.30), ("AK105/Actions/S_AK105_MagCheck.WAV", 3.720, 0.25)]
    for k in range(v["rattle"]):
        p, tt, d = donors[int(r.integers(len(donors)))]
        s = adsp.slice_at(be.src(p), tt, d)
        s = adsp.normalize_peak(s, -9.0)
        s = adsp.pitch(s, float(r.uniform(-2, 1)))
        place(s, k / v["rattle"] + float(r.uniform(-0.04, 0.04)), float(r.uniform(-12, -6)) + 6)
    return out


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
                y = abuild.finish(y, "foley")
                extra = {"element": elem, "category": cat, "variant": n, "source": be.describe(v)}
                if not elem.startswith("land"):
                    extra["anchor_ms"] = 0.0           # handling / jump cloth: starts with the action, no contact
                e = abuild.emit(f"Foley/{cat}/{elem}_{n}.wav", y, f"snd.foley.{cat}.{elem}", "foley", extra, sources=uses)
                entries.append(e)
                print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    for elem, variants in cfg["loops"].items():
        for n, v in enumerate(variants, 1):
            adsp.take_uses()
            y = sprint_loop(v)
            uses = adsp.take_uses()
            y = abuild.finish(y, "loop", loop=True)
            e = abuild.emit(f"Foley/move/{elem}_{n}.wav", y, f"snd.foley.move.{elem}", "loop",
                            {"element": elem, "category": "move", "variant": n, "loop": True, "anchor_ms": 0.0,
                             "source": f"real cloth + rattle slices, seed={v['seed']} steps_per_loop={v['steps']}"},
                            sources=uses)
            entries.append(e)
            print(f"{e['file']:42s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s loop")
    build_steps(cfg, entries)
    print("manifest files:", abuild.update_manifest(entries, ["Foley/"]))


if __name__ == "__main__":
    main()
