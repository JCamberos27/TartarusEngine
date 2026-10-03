"""Small DSP toolkit shared by the Tartarus audio build scripts (numpy / scipy / pedalboard / pyloudnorm).

All audio is float32 (n, 2) at 48 kHz inside the tools; mono assets are written as 1 channel at export.
"""
import math
import os

import numpy as np
import soundfile as sf
from scipy import signal

SR = 48000
SRC_ROOT = r"C:\tb\audio-src"
TSP = os.path.join(SRC_ROOT, "tsp", "Tactical Shooter Pack")
BOOTS = os.path.join(SRC_ROOT, "boots", "Boots")


# ----------------------------------------------------------------------------------------------------------- io
def resolve_sonniss(spec):
    """'sonniss/<pack dir prefix>/<file name prefix>' -> the single matching wav under C:/tb/audio-src/sonniss."""
    import glob
    _, pack, name = spec.split("/", 2)
    hits = glob.glob(os.path.join(SRC_ROOT, "sonniss", glob.escape(pack) + "*", glob.escape(name) + "*.wav"))
    hits = [h for h in hits if h.lower().endswith(".wav")]
    if len(hits) != 1:
        raise FileNotFoundError(f"{spec}: {len(hits)} matches")
    return hits[0]


class Src(np.ndarray):
    """A loaded source recording: an ndarray that remembers its file and its offset inside it, so every cut made
    through adsp.cut() is recorded as provenance (file + start/end seconds)."""
    def __array_finalize__(self, obj):
        self.path = getattr(obj, "path", None)
        self.off = getattr(obj, "off", 0)

    def __getitem__(self, k):
        r = super().__getitem__(k)
        if isinstance(r, Src) and isinstance(k, slice) and k.start:
            r.off = self.off + (k.start if k.start >= 0 else len(self) + k.start)
        return r


_USES = []
_PITCH = []      # every pitch / varispeed applied since the last take_uses(): semitones (the 2 st cap is checked per source in check_audio.py)


def cut(x, a, b):
    """x[a:b] as a plain copy, recording (source file, start s, end s) when x is a loaded source."""
    a, b = max(0, int(a)), min(len(x), int(b))
    if isinstance(x, Src) and x.path and b > a:
        _USES.append({"file": x.path, "start": round((x.off + a) / SR, 3), "end": round((x.off + b) / SR, 3)})
    return np.asarray(x[a:b]).copy()


def take_uses():
    """Pop the provenance list (merged duplicates) of everything cut since the last call. Each use carries `pitch_st`: the largest
    pitch / varispeed shift (semitones, signed) applied while that file was rendered (a render that mixes several cuts reports its
    biggest shift on all of them: conservative, never under-reports)."""
    st = max(_PITCH, key=abs, default=0.0)
    out, seen = [], set()
    for u in _USES:
        k = (u["file"], u["start"], u["end"])
        if k not in seen:
            seen.add(k)
            out.append({**u, "pitch_st": round(float(st), 2)})
    _USES.clear()
    _PITCH.clear()
    return out


def load(path, mono=False):
    """Load a wav as float32 (n, 2) @ 48 kHz (resampled if needed); `path` may be relative to the TSP folder."""
    if path.startswith("sonniss/"):
        path = resolve_sonniss(path)
    elif not os.path.isabs(path):
        path = os.path.join(TSP, path)
    rel = os.path.relpath(path, SRC_ROOT).replace("\\", "/")
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    if sr != SR:
        g = math.gcd(sr, SR)
        x = signal.resample_poly(x, SR // g, sr // g, axis=0).astype(np.float32)
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    elif x.shape[1] > 2:
        x = x[:, :2]
    if mono:
        x = np.repeat(x.mean(axis=1, keepdims=True), 2, axis=1)
    x = np.ascontiguousarray(x).view(Src)
    x.path, x.off = rel, 0
    return x


def to_mono(x):
    return x.mean(axis=1) if x.ndim == 2 else x


def write_wav(path, x, mono=False, dither_seed=0):
    """16-bit PCM, plain rounding (no dither: nothing is generated, not even noise). mono=True writes 1 channel."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    y = to_mono(x)[:, None] if mono else x
    y = np.clip(np.asarray(y), -1.0, 32767.0 / 32768.0)
    sf.write(path, y, SR, subtype="PCM_16")


# ----------------------------------------------------------------------------------------------------------- measure
def true_peak_db(x):
    up = signal.resample_poly(x, 4, 1, axis=0)
    return 20 * math.log10(max(float(np.abs(up).max()), 1e-9))


def sample_peak_db(x):
    return 20 * math.log10(max(float(np.abs(x).max()), 1e-9))


def lufs(x):
    """(integrated LUFS, max momentary LUFS-M) -- BS.1770 K-weighting, 400 ms blocks; zero padded to one block."""
    import pyloudnorm as pyln
    meter = pyln.Meter(SR)
    y = x if x.ndim == 2 else x[:, None]
    n = int(0.4 * SR) + 1
    if len(y) < n:
        y = np.concatenate([y, np.zeros((n - len(y), y.shape[1]), np.float32)])
    try:
        integ = float(meter.integrated_loudness(y.astype(np.float64)))
    except Exception:
        integ = -120.0
    # momentary max: K-weighted mean square over a sliding 400 ms window, 100 ms hop
    kw = _k_weight(y)
    win, hop = int(0.4 * SR), int(0.1 * SR)
    best = -120.0
    for s in range(0, max(1, len(kw) - win + 1), hop):
        ms = float(np.mean(np.sum(kw[s:s + win] ** 2, axis=1) if kw.shape[1] > 1 else kw[s:s + win] ** 2))
        best = max(best, -0.691 + 10 * math.log10(max(ms, 1e-12)))
    return integ, best


def _k_weight(y):
    """BS.1770 K-weighting at 48 kHz (high shelf + RLB high-pass), per channel."""
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]
    a1 = [1.0, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]
    a2 = [1.0, -1.99004745483398, 0.99007225036621]
    out = signal.lfilter(b1, a1, y, axis=0)
    return signal.lfilter(b2, a2, out, axis=0)


def rms_db(x):
    return 10 * math.log10(max(float(np.mean(x ** 2)), 1e-12))


def dc_offset(x):
    return float(np.abs(x.mean(axis=0)).max())


# ----------------------------------------------------------------------------------------------------------- slicing
def find_onset(x, t, back_ms=12.0, fwd_ms=30.0, frac=0.22, hp=250.0):
    """Sample index of the transient near time t: first crossing of `frac` of the local peak of a high-passed
    envelope inside [t - back, t + fwd]. Keeps a clicky onset sample-accurate (<1 ms)."""
    m = to_mono(x)
    sos = signal.butter(2, hp, "hp", fs=SR, output="sos")
    h = np.abs(signal.sosfilt(sos, m))
    a = max(0, int((t - back_ms / 1000) * SR))
    b = min(len(h), int((t + fwd_ms / 1000) * SR))
    seg = h[a:b]
    if seg.size == 0 or seg.max() <= 0:
        return int(t * SR)
    idx = int(np.argmax(seg > frac * seg.max()))
    return a + idx


def anchor_ms(x, frac=0.5, hp=250.0):
    """Time (ms) of the contact transient in a rendered one-shot: first sample whose high-passed magnitude reaches
    `frac` of the file's high-passed peak. The engine starts the file at (event time - anchor_ms)."""
    h = np.abs(signal.sosfilt(signal.butter(2, hp, "hp", fs=SR, output="sos"), to_mono(x)))
    if h.max() <= 0:
        return 0.0
    return float(int(np.argmax(h >= frac * h.max())) * 1000.0 / SR)


def fade(x, n_in=0, n_out=0, curve="cos"):
    x = np.array(x, dtype=np.float32)
    if n_in > 0:
        n_in = min(n_in, len(x))
        w = np.hanning(2 * n_in)[:n_in].astype(np.float32)          # raised-cosine ramp (a window, not a signal)
        x[:n_in] *= w[:, None]
    if n_out > 0:
        n_out = min(n_out, len(x))
        w = np.hanning(2 * n_out)[n_out:].astype(np.float32)
        x[-n_out:] *= w[:, None]
    return x


def slice_at(x, t, dur, preroll_ms=3.0, fade_in_ms=1.5, fade_out_ms=None, snap=True, hp=250.0):
    """Cut [onset - preroll, onset + dur]. Short fade-in, natural (cosine) fade-out over the last 25 % (>= 25 ms)."""
    on = find_onset(x, t, hp=hp) if snap else int(t * SR)
    a = max(0, on - int(preroll_ms / 1000 * SR))
    b = min(len(x), on + int(dur * SR))
    y = cut(x, a, b)
    fo = int((fade_out_ms if fade_out_ms is not None else max(25.0, dur * 250.0)) / 1000 * SR)
    return fade(y, int(fade_in_ms / 1000 * SR), fo)


def remove_dc(x, hz=18.0):
    sos = signal.butter(2, hz, "hp", fs=SR, output="sos")
    return signal.sosfilt(sos, x - x.mean(axis=0), axis=0).astype(np.float32)


# ----------------------------------------------------------------------------------------------------------- shaping
def pitch(x, semitones):
    """Resample pitch shift (changes length) -- fine for one-shots / foley."""
    _PITCH.append(float(semitones))
    if abs(semitones) < 1e-6:
        return x
    r = 2 ** (semitones / 12.0)
    n = int(round(len(x) / r))
    t = np.linspace(0, len(x) - 1, n)
    return np.stack([np.interp(t, np.arange(len(x)), x[:, c]) for c in range(x.shape[1])], 1).astype(np.float32)


def stretch_to(x, n):
    """Resample a clip to exactly n samples (varispeed)."""
    _PITCH.append(12.0 * math.log2(len(x) / max(1, n)))
    t = np.linspace(0, len(x) - 1, n)
    return np.stack([np.interp(t, np.arange(len(x)), x[:, c]) for c in range(x.shape[1])], 1).astype(np.float32)


def filt(x, kind, hz, order=2):
    if kind == "bp":
        sos = signal.butter(order, hz, "bp", fs=SR, output="sos")
    else:
        sos = signal.butter(order, hz, kind, fs=SR, output="sos")
    return signal.sosfilt(sos, x, axis=0).astype(np.float32)


def board(x, *plugins):
    from pedalboard import Pedalboard
    return Pedalboard(list(plugins))(x.T.astype(np.float32), SR).T.astype(np.float32)


def transient_shape(x, attack=0.0, sustain=0.0):
    """Differential-envelope transient designer. attack/sustain in [-1, 1] (+ = more). Mono-linked."""
    m = np.abs(to_mono(x))

    def env(ms):
        a = math.exp(-1.0 / (ms * 0.001 * SR))
        return signal.lfilter([1 - a], [1, -a], m)
    fast, slow = env(1.0), env(30.0)
    diff = (fast - slow) / (fast + slow + 1e-6)          # +: attack phase, -: decay
    g = 1.0 + attack * np.clip(diff, 0, 1) * 1.6 + sustain * np.clip(-diff, 0, 1) * 1.2
    return (x * np.clip(g, 0.2, 4.0)[:, None]).astype(np.float32)


def soft_sat(x, drive=1.5):
    """tanh saturation normalised to unity small-signal gain."""
    return (np.tanh(x * drive) / drive).astype(np.float32)


def limit(x, ceiling_db=-1.0, release_ms=60.0):
    """Look-ahead limiter via pedalboard, then a hard trim so the 4x true peak is under the ceiling."""
    from pedalboard import Limiter
    y = board(x, Limiter(threshold_db=ceiling_db - 0.3, release_ms=release_ms))
    tp = true_peak_db(y)
    if tp > ceiling_db:
        y = (y * 10 ** ((ceiling_db - tp) / 20)).astype(np.float32)
    return y


def gain_db(x, db):
    return (x * 10 ** (db / 20)).astype(np.float32)


def normalize_peak(x, db):
    return gain_db(x, db - sample_peak_db(x))


def mix(layers, length=None):
    """layers: list of (audio, start_sample, gain_db). Returns float32 stereo sum."""
    n = length or max(s + len(a) for a, s, g in layers)
    out = np.zeros((n, 2), np.float32)
    for a, s, g in layers:
        e = min(n, s + len(a))
        if e > s:
            out[s:e] += a[:e - s] * 10 ** (g / 20)
    return out


def phase_align(ref, layer, max_lag_ms=2.0, band=(60.0, 1200.0), win_ms=12.0):
    """Align `layer` to `ref` at the transient: best lag (and polarity) of the band-passed first `win_ms`.
    Returns (aligned_layer, lag_samples, polarity)."""
    sos = signal.butter(2, band, "bp", fs=SR, output="sos")
    n = int(win_ms / 1000 * SR)
    L = int(max_lag_ms / 1000 * SR)
    r = signal.sosfilt(sos, to_mono(ref)[:n + L])
    l = signal.sosfilt(sos, to_mono(layer)[:n + 2 * L])
    best, best_lag, pol = 0.0, 0, 1
    for lag in range(-L, L + 1):
        a = r[L:L + n]
        s = L + lag
        if s < 0 or s + n > len(l):
            continue
        c = float(np.dot(a, l[s:s + n]))
        if abs(c) > abs(best):
            best, best_lag, pol = c, lag, (1 if c >= 0 else -1)
    out = layer * pol
    if best_lag > 0:       # layer is late: drop leading samples
        out = out[best_lag:]
    elif best_lag < 0:
        out = np.concatenate([np.zeros((-best_lag, 2), np.float32), out])
    return out.astype(np.float32), best_lag, pol


def deterministic_rng(*parts):
    import zlib
    return np.random.default_rng(zlib.crc32("|".join(map(str, parts)).encode()))
