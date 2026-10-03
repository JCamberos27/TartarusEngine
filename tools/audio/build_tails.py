"""Build the environment-aware gunshot tails from recipes/tails.json (real recordings only, see the recipe's _doc).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_tails.py [ak|870]

Per gun and space class writes <dir>/fire_tail_<class>_<n>.wav (manifest key snd.<gun>.fire_tail_<class>, layer "tail", so
check_audio's tail loudness window and true-peak ceiling apply). Each is a slice (or a few layered slices) of real recordings,
timelined from the shot instant like the generic fire_tail, and its manifest entry lists the source slices (`sources`:
file relative to C:\\tb\\audio-src, start_s / end_s inside that file, gain_db, pitch semitones). Nothing is generated.
The generic fire_tail files are build_fire.py's and stay the fallback key; their manifest entries get `sources` here too
(annotate only, the wavs are not touched).
"""
import json
import os
import sys

import numpy as np

import abuild
import adsp
import build_fire

SR = adsp.SR
MIX_DB = -4.0          # same bus balance as the generic tail
PEAK_IN_DB = -12.0     # each slice is peak-normalised here before its gain_db, so layers of different recordings are comparable


def mono_fold_loss_db(y):
    """Power lost when L and R are summed to mono, against the average channel power (0 dB = fully correlated, 3 dB = decorrelated)."""
    l, r = y[:, 0].astype(np.float64), y[:, 1].astype(np.float64)
    p = 0.5 * (np.mean(l ** 2) + np.mean(r ** 2))
    pm = np.mean(((l + r) * 0.5) ** 2)
    return float(10 * np.log10(max(pm, 1e-12) / max(p, 1e-12)))


def load_source(rel):
    return build_fire.src(os.path.join(adsp.SRC_ROOT, rel.replace("/", os.sep)))


def shot_onset_at(x, t):
    """Sample of the blast's first rise near t seconds (+-). Packs' own shots (no t) use build_fire.shot_onset."""
    if t is None:
        return build_fire.shot_onset(x)
    return adsp.find_onset(x, t, back_ms=40.0, fwd_ms=80.0, frac=0.15)


def render_layer(layer, sources, cls):
    rel = sources[layer["src"]]
    x = load_source(rel)
    on = shot_onset_at(x, layer.get("t"))
    a_s, b_s = layer["start_s"], layer["end_s"]
    a, b = on + int(a_s * SR), min(len(x), on + int(b_s * SR))
    seg = x[a:b].copy()
    seg = adsp.pitch(seg, layer.get("pitch", 0.0))
    if "lp" in layer:
        seg = adsp.filt(seg, "lp", layer["lp"])
    seg = adsp.fade(seg, int(cls["fade_in_s"] * SR), int(cls["fade_out"] * len(seg)))
    seg = adsp.normalize_peak(seg, PEAK_IN_DB)
    seg = adsp.gain_db(seg, layer.get("gain_db", 0.0))
    src = {"file": rel, "start_s": round(a / SR, 3), "end_s": round(b / SR, 3), "gain_db": layer.get("gain_db", 0.0),
           "pitch_st": layer.get("pitch", 0.0)}
    if "lp" in layer:
        src["lp_hz"] = layer["lp"]
    return seg, int(a_s * SR), src


def render(v, cls, sources):
    parts, srcs = [], []
    for layer in v["layers"]:
        seg, off, s = render_layer(layer, sources, cls)
        parts.append((seg, off, 0.0))
        srcs.append(s)
    y = adsp.mix(parts)
    y = build_fire.chain(y, cls["chain"])
    return y, srcs


def build(gun, cfg, sources):
    entries = []
    for cname, c in cfg["classes"].items():
        for n, v in enumerate(c["variants"], 1):
            y, srcs = render(v, c, sources)
            y = abuild.finish(y, "tail", max_gr_db=7.0, target=c["target_lufs_m"])
            rel = f"{cfg['dir']}/fire_tail_{cname}_{n}.wav"
            note = " + ".join(f"{os.path.basename(s['file'])[:40]} {s['start_s']:.2f}-{s['end_s']:.2f}s" for s in srcs)
            e = abuild.emit(rel, y, f"snd.{gun}.fire_tail_{cname}", "tail",
                            {"gun": gun, "element": f"fire_tail_{cname}", "space": cname, "variant": n, "source": note,
                             "sources": srcs, "mix_db": MIX_DB, "mono_fold_loss_db": round(mono_fold_loss_db(y), 2)})
            entries.append(e)
            print(f"{rel:56s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  "
                  f"mono-fold {e['mono_fold_loss_db']:4.1f} dB  {note}")
    return entries


def annotate_generic():
    """Give the generic fire_tail entries their `sources` (from build_fire's recipes; the wavs are not rebuilt)."""
    doc = json.load(open(abuild.MANIFEST))
    changed = 0
    for gun, recipe in (("ak", "ak_fire.json"), ("870", "remington870_fire.json")):
        cfg = json.load(open(os.path.join(abuild.HERE, "recipes", recipe)))
        tail = cfg["tail"]
        for n, v in enumerate(tail["variants"], 1):
            rel = f"{cfg['dir']}/fire_tail_{n}.wav"
            for e in doc["files"]:
                if e["file"] != rel:
                    continue
                path = os.path.join(adsp.TSP, v["src"].replace("/", os.sep))
                on = build_fire.shot_onset(build_fire.src(path))
                a = on + int(tail["start_s"] * SR)
                b = min(len(build_fire.src(path)), on + int(tail["length_s"] * SR))
                e["sources"] = [{"file": "tsp/Tactical Shooter Pack/" + v["src"], "start_s": round(a / SR, 3), "end_s": round(b / SR, 3),
                                 "gain_db": 0.0, "pitch_st": v.get("pitch", 0.0)}]
                changed += 1
    with open(abuild.MANIFEST, "w") as fh:
        json.dump(doc, fh, indent=1)
    return changed


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "tails.json")))
    allent, owned = [], []
    for gun, g in cfg["guns"].items():
        if only and only != gun:
            continue
        allent += build(gun, g, cfg["sources"])
        owned += [f"{g['dir']}/fire_tail_{cn}_" for cn in g["classes"]]
    print("manifest files:", abuild.update_manifest(allent, owned))
    print("generic fire_tail entries annotated with sources:", annotate_generic())


if __name__ == "__main__":
    main()
