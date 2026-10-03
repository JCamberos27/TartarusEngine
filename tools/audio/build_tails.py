"""Build the environment-aware gunshot tails from recipes/tails.json (real recordings only, see the recipe's _doc).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_tails.py [ak|870]

Per gun and space class writes <dir>/fire_tail_<class>_<n>.wav (manifest key snd.<gun>.fire_tail_<class>, layer "tail", so
check_audio's tail loudness window and true-peak ceiling apply). Each is a slice of a pack recording timelined from the shot
instant, like the generic fire_tail. The generic fire_tail files are build_fire.py's and stay the fallback key.
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


def mono_fold_loss_db(y):
    """Power lost when L and R are summed to mono, against the average channel power (0 dB = fully correlated, 3 dB = decorrelated)."""
    l, r = y[:, 0].astype(np.float64), y[:, 1].astype(np.float64)
    p = 0.5 * (np.mean(l ** 2) + np.mean(r ** 2))
    pm = np.mean(((l + r) * 0.5) ** 2)
    return float(10 * np.log10(max(pm, 1e-12) / max(p, 1e-12)))


def render(v, c):
    x = build_fire.src(v["src"])
    on = build_fire.shot_onset(x)
    st = int(v.get("start_s", c["start_s"]) * SR)
    n = int(v.get("length_s", c["length_s"]) * SR)
    seg = x[on + st:on + st + n].copy()
    seg = adsp.pitch(seg, v.get("pitch", 0.0))
    if "lp" in v:
        seg = adsp.filt(seg, "lp", v["lp"])
    chain = dict(c["chain"])
    if "hp" in v:
        chain["hp"] = v["hp"]
    seg = adsp.fade(seg, int(c["fade_in_s"] * SR), int(c["fade_out"] * len(seg)))
    y = build_fire.chain(seg, chain)
    return np.concatenate([np.zeros((st, 2), np.float32), y]), f"{os.path.basename(v['src'])} +{st / SR:.2f}s"


def build(gun, cfg):
    entries = []
    for cname, c in cfg["classes"].items():
        for n, v in enumerate(c["variants"], 1):
            y, note = render(v, c)
            y = abuild.finish(y, "tail", max_gr_db=7.0, target=c["target_lufs_m"])
            rel = f"{cfg['dir']}/fire_tail_{cname}_{n}.wav"
            e = abuild.emit(rel, y, f"snd.{gun}.fire_tail_{cname}", "tail",
                            {"gun": gun, "element": f"fire_tail_{cname}", "space": cname, "variant": n, "source": note,
                             "mix_db": MIX_DB, "mono_fold_loss_db": round(mono_fold_loss_db(y), 2)})
            entries.append(e)
            print(f"{rel:56s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  "
                  f"mono-fold {e['mono_fold_loss_db']:4.1f} dB  {note}")
    return entries


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    cfg = json.load(open(os.path.join(abuild.HERE, "recipes", "tails.json")))
    allent, owned = [], []
    for gun, g in cfg["guns"].items():
        if only and only != gun:
            continue
        allent += build(gun, g)
        owned += [f"{g['dir']}/fire_tail_{cn}_" for cn in g["classes"]]
    print("manifest files:", abuild.update_manifest(allent, owned))


if __name__ == "__main__":
    main()
