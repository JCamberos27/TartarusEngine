"""Build the gunfire layers for both guns from recipes/ak_fire.json and recipes/remington870_fire.json.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_fire.py [ak|870]

Per gun and layer (close / mech / sub / tail / far) writes <dir>/fire_<layer>_<n>.wav. Every layer is timelined from the
shot instant, so the engine just starts them together; tails and far carry their own leading silence / delay.
Close layers are phase-aligned (best lag + polarity of the 60-1200 Hz band over the first 12 ms) before they are summed.
"""
import json
import os
import sys

import numpy as np
from scipy import signal

import abuild
import adsp

SR = adsp.SR
_cache = {}
# default bus balance of the five layers (every file is delivered at its own loudness target; the engine plays each
# layer at mix_db relative to that, then a limiter on the weapon bus). Starting point for S2, not a mastering decision.
# (superseded: mix_db now comes from recipes/mix.json via mixspec.py, applied by abuild.update_manifest)


def src(path):
    if path not in _cache:
        _cache[path] = adsp.load(path)
    return _cache[path]


def shot_onset(x, frac=0.12):
    """First sample of the muzzle blast: first crossing of `frac` of the peak of the 250 Hz high-passed signal."""
    h = np.abs(signal.sosfilt(signal.butter(2, 250, "hp", fs=SR, output="sos"), adsp.to_mono(x)))
    return int(np.argmax(h > frac * h.max()))


def onset_of(x, d):
    """The shot instant in a source: near d["t"] (seconds) in a recording of several shots, else the file's first blast."""
    if "t" in d:
        return adsp.find_onset(x, d["t"], back_ms=40.0, fwd_ms=80.0, frac=0.15)
    return shot_onset(x)


def chain(x, c):
    from pedalboard import PeakFilter, Compressor
    if "hp" in c:
        x = adsp.filt(x, "hp", c["hp"])
    if "lp" in c:
        x = adsp.filt(x, "lp", c["lp"], c.get("lp_order", 2))
    if c.get("eq"):
        x = adsp.board(x, *[PeakFilter(cutoff_frequency_hz=hz, gain_db=g, q=q) for hz, g, q in c["eq"]])
    if c.get("attack") or c.get("sustain"):
        x = adsp.transient_shape(x, c.get("attack", 0.0), c.get("sustain", 0.0))
    if c.get("comp"):
        cc = c["comp"]
        x = adsp.board(x, Compressor(threshold_db=cc["threshold_db"], ratio=cc["ratio"],
                                     attack_ms=cc["attack_ms"], release_ms=cc["release_ms"]))
    if c.get("drive"):
        x = adsp.soft_sat(x, c["drive"])
    return x


def _donor(d, ref):
    """Slice a donor shot from its own onset, filter it, phase-align it to `ref`, return (audio, lag, polarity)."""
    x = src(d["src"])
    on = onset_of(x, d)
    a = max(0, on - int(0.003 * SR))
    z = adsp.cut(x, a, a + int(d["dur"] * SR))
    z = adsp.fade(z, int(0.0008 * SR), int(max(0.02, d["dur"] * 0.3) * SR))
    if "hp" in d:
        z = adsp.filt(z, "hp", d["hp"])
    if "lp" in d:
        z = adsp.filt(z, "lp", d["lp"], 2)
    # align on the band the donor actually contributes: crack = presence band, body = low-mid
    band = (1500.0, 6500.0) if d.get("hp", 0) >= 1500 else (60.0, 700.0)
    z, lag, pol = adsp.phase_align(ref, z, band=band, max_lag_ms=1.2 if d.get("hp", 0) >= 1500 else 2.5)   # (a body's onset is less sharp)
    return adsp.gain_db(z, d.get("gain_db", 0.0)), lag, pol


def render_close(v, cfg):
    x = src(v["base"]["src"])
    on = onset_of(x, v["base"])
    n = int(cfg["length_s"] * SR)
    b0 = max(0, on - int(0.003 * SR))
    base = adsp.cut(x, b0, b0 + n)
    base = adsp.fade(base, int(0.0008 * SR), int(0.25 * n))
    notes = []
    layers = [(base, 0, 0.0)]
    for key in ("crack", "body", "layer"):
        if key in v:
            z, lag, pol = _donor(v[key], base)
            layers.append((z, 0, 0.0))
            notes.append(f"{key}:{os.path.basename(v[key]['src'])} lag={lag}smp pol={pol:+d}")
    y = adsp.mix(layers, n)
    y = adsp.pitch(y, v.get("pitch", 0.0))
    y = chain(y, cfg["chain"])
    return y, "base:" + os.path.basename(v["base"]["src"]) + " " + " ".join(notes)


def _low_donor(d, n):
    """The low end of another REAL recording (a shot, or a thud / thump): onset-aligned, low-passed, at most 2 st of pitch."""
    x = src(d["src"])
    if "start_s" in d:
        x = x[int(d["start_s"] * SR):]
    on = shot_onset(x) if d.get("shot", True) else adsp.find_onset(x, d.get("t", 0.0), back_ms=20, fwd_ms=30, hp=60.0)
    s0 = max(0, on - int(0.002 * SR))
    seg = adsp.cut(x, s0, s0 + int(n * 1.4))
    seg = adsp.pitch(seg, d.get("pitch", 0.0))
    seg = adsp.filt(seg, "lp", d["lp"], 4)[:n]
    seg = adsp.fade(seg, 0, int(0.3 * len(seg)))
    return adsp.gain_db(seg, d.get("gain_db", -3.0))


def render_sub(v, cfg, ref):
    """Sub punch from REAL recordings only: the low end of a real shot (4th-order low-pass at v.lp) plus the low end of other
    real shots / thuds (v.donors, each onset-aligned, low-passed, <= 2 st of pitch). Nothing is dropped an octave: the weight comes
    from layering real low-end energy, lightly saturated for audible harmonics, polarity / lag matched to the close."""
    x = src(v["src"])
    if "start_s" in v:
        x = x[int(v["start_s"] * SR):]
    on = shot_onset(x)
    n = int(cfg["length_s"] * SR)
    s0 = max(0, on - int(0.002 * SR))
    seg = adsp.cut(x, s0, s0 + int(n * 1.4))
    pad = lambda z: np.pad(z, ((0, max(0, n - len(z))), (0, 0)))
    y = pad(adsp.filt(seg, "lp", v["lp"], 4)[:n])
    for d in v.get("donors", []):
        y = y + pad(_low_donor(d, n))
    y = adsp.filt(y, "hp", 28.0, 2)
    y = np.repeat(adsp.to_mono(y)[:, None], 2, axis=1)               # low end is mono: no phase smear between the ears
    # a touch of saturation (audible harmonics on small speakers) without flattening the punch
    y = adsp.soft_sat(0.6 * y / (np.abs(y).max() + 1e-9), v.get("drive", 1.25)) / 0.6
    y = adsp.fade(y, int(0.0008 * SR), int(0.35 * n))
    y, lag, pol = adsp.phase_align(ref, y, band=(35.0, 160.0), win_ms=18.0, max_lag_ms=0.4)
    y = np.pad(y, ((0, max(0, n - len(y))), (0, 0)))[:n]
    return y, f"real {os.path.basename(v['src'])} lp={v['lp']} +{len(v.get('donors', []))} low donors lag={lag}smp pol={pol:+d}"


def render_mech(v, cfg):
    x = src(v["src"])
    y = adsp.slice_at(x, v["t"], v["dur"])
    y = adsp.pitch(y, v.get("pitch", 0.0))
    y = chain(y, cfg["chain"])
    n = int(cfg["length_s"] * SR)
    out = adsp.mix([(y, int(v["delay_ms"] / 1000 * SR), 0.0)], n)
    return out, f"{os.path.basename(v['src'])}@{v['t']} delay={v['delay_ms']}ms"


def render_tail(v, cfg):
    x = src(v["src"])
    on = shot_onset(x)
    st = int(cfg["start_s"] * SR)
    n = int(cfg["length_s"] * SR)
    seg = adsp.cut(x, on + st, on + n)
    seg = adsp.pitch(seg, v.get("pitch", 0.0))
    if "lp" in v:
        seg = adsp.filt(seg, "lp", v["lp"])
    seg = adsp.fade(seg, int(0.10 * SR), int(0.45 * len(seg)))      # in: hides the body that lives in the close layer
    y = chain(seg, cfg["chain"])
    out = np.concatenate([np.zeros((st, 2), np.float32), y])
    return out, f"{os.path.basename(v['src'])} tail from +{cfg['start_s']}s"


def render_far(v, cfg):
    x = src(v["src"])
    on = onset_of(x, v)
    n = int(cfg["length_s"] * SR)
    seg = adsp.cut(x, on, on + n)
    seg = adsp.pitch(seg, v.get("pitch", 0.0))
    seg = np.repeat(adsp.to_mono(seg)[:, None], 2, axis=1)          # far is mono
    seg = adsp.fade(seg, int(0.012 * SR), int(0.5 * len(seg)))
    y = chain(seg, cfg["chain"])
    d = int(v.get("delay_ms", cfg["delay_ms"]) / 1000 * SR)
    return np.concatenate([np.zeros((d, 2), np.float32), y]), f"{os.path.basename(v['src'])} delay={d * 1000 // SR}ms"


def build(recipe_path, layers=None):
    cfg = json.load(open(recipe_path))
    gun, d = cfg["gun"], cfg["dir"]
    entries = []
    closes = []
    want = lambda l: layers is None or l in layers
    for n, v in enumerate(cfg["close"]["variants"], 1):
        adsp.take_uses()
        y, note = render_close(v, cfg["close"])
        closes.append(y)
        if want("close"):
            entries.append(("close", n, y, note, False, adsp.take_uses()))
    for n, v in enumerate(cfg["sub"]["variants"] if want("sub") else [], 1):
        ref = closes[(n - 1) % len(closes)]
        adsp.take_uses()
        y, note = render_sub(v, cfg["sub"], ref)
        entries.append(("sub", n, y, note, True, adsp.take_uses()))
    for n, v in enumerate(cfg["mech"]["variants"] if want("mech") else [], 1):
        adsp.take_uses()
        y, note = render_mech(v, cfg["mech"])
        entries.append(("mech", n, y, note, False, adsp.take_uses()))
    for n, v in enumerate(cfg["tail"]["variants"] if want("tail") else [], 1):
        adsp.take_uses()
        y, note = render_tail(v, cfg["tail"])
        entries.append(("tail", n, y, note, False, adsp.take_uses()))
    for n, v in enumerate(cfg["far"]["variants"] if want("far") else [], 1):
        adsp.take_uses()
        y, note = render_far(v, cfg["far"])
        entries.append(("far", n, y, note, True, adsp.take_uses()))
    out = []
    for layer, n, y, note, mono, uses in entries:
        y = abuild.finish(y, layer, trim_bleed=layer == "mech")   # (limiting budget per layer: abuild.GR_BUDGET_DB)
        rel = f"{d}/fire_{layer}_{n}.wav"
        e = abuild.emit(rel, y, f"snd.{gun}.fire_{layer}", layer,
                        {"gun": gun, "element": f"fire_{layer}", "variant": n, "source": note,
                         }, mono=mono, sources=uses)
        out.append(e)
        print(f"{rel:46s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  {note}")
    return out, [f"{d}/fire_{l}_" for l in (layers or LAYER_NAMES)]


LAYER_NAMES = ("close", "sub", "mech", "tail", "far")


def main():
    """build_fire.py [ak|870] [layer ...]   -- e.g.  build_fire.py ak sub   rebuilds only the AK sub layer."""
    only = sys.argv[1] if len(sys.argv) > 1 else None
    layers = sys.argv[2:] or None
    allent, prefixes = [], []
    for name, path in (("ak", "ak_fire.json"), ("870", "remington870_fire.json")):
        if only and only != name:
            continue
        e, p = build(os.path.join(abuild.HERE, "recipes", path), layers)
        allent += e
        prefixes += p
    # the environment tails (fire_tail_<space>_<n>, build_tails.py) share the fire_tail_ prefix but are not ours
    print("manifest files:", abuild.update_manifest(allent, prefixes, keep=lambda e: e.get("space") is not None))


if __name__ == "__main__":
    main()
