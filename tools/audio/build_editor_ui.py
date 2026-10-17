"""Build the editor UI sound effects (resources/EditorSounds/*.wav + LICENSE.md) from the CC BY-SA desktop sound themes.

  python -I tools/audio/build_editor_ui.py ["G:\\My Drive\\Tartarus Assets\\Raw\\Audio\\SFX"]

Needs only numpy and the standard `wave` module (any Python that has numpy: on the original machine, Blender 5.2's bundled
python.exe). adsp.py / abuild.py import scipy, soundfile and pedalboard at module level, so the few helpers this build needs
(Hann fades, onset finding, BS.1770 K-weighting and the LUFS-M momentary window, peak normalisation) are ported below in plain
numpy, with the same definitions.

Sources (the only ones this script reads; both are CC BY-SA, so every output is a CC BY-SA adaptation):
  <SFX>/KDE Ocean Sound Theme/WAV/*.wav      Ocean Sound Theme, Guilherme Marcal Silva (KDE), CC BY-SA 4.0
  <SFX>/Ubuntu Yaru Sound Theme/WAV/*.wav    WoodenBeaver / Touch-Remix by madsrh, via Ubuntu Yaru, CC BY-SA 3.0
An output never mixes the two sets, so each file carries exactly one license.

Output: 48 kHz, 16-bit, mono. Every file starts and ends at silence (pre-roll + Hann fade-in, Hann fade-out), peaks at most
-1 dBTP, and is checked against its tier's loudness (check_master). Three level tiers (all LIFT_DB above the first build):
  tick   (clicks, toggles, ticks, < 120 ms, transient at sample 0)  sample peak -20 dBFS (quieter for very frequent ones), but never
         louder than TICK_SHORT_CAP on the 100 ms K-weighted window: a pure-tone tick that rings for 80 ms is heard ~10 dB over
         a 13 ms key tick of the same peak, so the tonal ones come down to the cap
         and at least TICK_SHORT_MIN on it: the shortest, brightest ticks (select, hover, tree) read far quieter than the rest
         at the same peak, so they come up to the floor (peak-capped at PEAK_CEIL_DB)
  cue    (menus, play mode, undo / redo, save, drop)                 loudness-matched on the 100 ms window (CUE_SHORT): cues run
         105-640 ms, and the 400 ms momentary window would read the short ones by their length and push them up
  notify (info / success / warning / error)                          loudness-matched: LUFS-M max (400 ms momentary, BS.1770)
         NOTIFY_LUFS; all four are 440-860 ms long
Every gain is plain gain (no limiter, no compression); a sample peak above PEAK_CEIL_DB stops the gain there.

The build is deterministic: same sources in, bit-identical wavs out.
"""
import math
import os
import sys
import wave

import numpy as np

SR = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "EditorSounds"))
DEFAULT_SRC = r"G:\My Drive\Tartarus Assets\Raw\Audio\SFX"

# P6 remaster: the whole ladder came up 10 dB (it was mastered so low that Jacob had to max every slider to hear it). The
# relative balance between the tiers - and each recipe's own `peak=` - is unchanged: LIFT_DB is added to all of them.
LIFT_DB = 10.0
NOTIFY_LUFS = -28.0 + LIFT_DB     # notifications: loudest 400 ms momentary window (LUFS-M max)
CUE_SHORT = -27.0 + LIFT_DB       # menus / play mode / undo / redo / save / drop: loudest 100 ms K-weighted window
TICK_SHORT_CAP = -35.0 + LIFT_DB  # ticks: at most this on the 100 ms window, whatever their peak
TICK_SHORT_MIN = -36.0 + LIFT_DB  # ...and at least this (2 dB more lift: the ticks are what is heard most, and sat on this floor)
PEAK_CEIL_DB = -1.0               # no loudness gain past this true peak (4x oversampled), the usual -1 dBTP master ceiling
LOUDNESS_TOL_DB = 3.0             # every file lands within this of its tier's target, or the build fails

PRE_ROLL_MS = 1.5                 # each layer starts this long before its onset...
MIN_FADE_IN_MS = {"tick": 1.0, "cue": 2.0, "notify": 2.0}  # ...and fades in over the pre-roll plus at least this: no hard start
EDGE_HEAD = 0.002                 # |first sample| must stay under this (about -54 dBFS)
EDGE_TAIL = 0.001                 # ...and |last sample| under this, so nothing starts or stops with a click

SETS = {
    "K": {"dir": os.path.join("KDE Ocean Sound Theme", "WAV"), "license": "CC BY-SA 4.0",
          "credit": "Ocean Sound Theme by Guilherme Marçal Silva (KDE)"},
    "Y": {"dir": os.path.join("Ubuntu Yaru Sound Theme", "WAV"), "license": "CC BY-SA 3.0",
          "credit": "WoodenBeaver / Touch-Remix by madsrh, via Ubuntu Yaru"},
}


# ------------------------------------------------------------------------------------------------------------------ io
def read_wav(path):
    """PCM wav -> float32 (n, channels) in [-1, 1), sample rate."""
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / float(1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, "<i4").astype(np.float64) / 2.0 ** 31
    else:
        raise ValueError(f"{path}: {8 * sw}-bit wav not supported")
    return x.reshape(-1, ch), sr


def write_wav(path, x):
    """Mono 16-bit PCM at 48 kHz, plain rounding (as adsp.write_wav: nothing generated, not even dither)."""
    y = np.clip(np.round(np.asarray(x, np.float64) * 32768.0), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(y.tobytes())


_SRC = {}


def source(src_root, key):
    """'K:button-pressed' -> dict(mono float64 at the file's own rate, sr, rel path, downmix note). Cached."""
    if key in _SRC:
        return _SRC[key]
    s, name = key.split(":", 1)
    rel = os.path.join(SETS[s]["dir"], name + ".wav")
    x, sr = read_wav(os.path.join(src_root, rel))
    if x.shape[1] == 1:
        m, how = x[:, 0], "mono source"
    else:
        L, R = x[:, 0], x[:, 1]
        mid = 0.5 * (L + R)
        keep = float(np.mean(mid ** 2) / max(1e-20, 0.5 * (np.mean(L ** 2) + np.mean(R ** 2))))
        if keep >= 0.9:
            m, how = mid, "mono: mean of L and R"
        else:   # a widened stereo file folds to mono with a comb / level loss: take its louder channel instead
            left = np.mean(L ** 2) >= np.mean(R ** 2)
            m, how = (L if left else R), f"mono: {'left' if left else 'right'} channel only (L+R fold keeps {keep:.0%} of the energy)"
    _SRC[key] = {"x": m, "sr": sr, "rel": rel.replace("\\", "/"), "set": s, "downmix": how}
    return _SRC[key]


# ------------------------------------------------------------------------------------------------------------------ dsp
def onset(x, frac):
    """First sample reaching `frac` of the file's peak (adsp.first_onset without its high-pass)."""
    a = np.abs(x)
    return int(np.argmax(a >= frac * a.max()))


def fade(x, n_in=0, n_out=0):
    """Raised-cosine (Hann half-window) fade in / out, as adsp.fade."""
    x = np.array(x, np.float64)
    if n_in > 0:
        n_in = min(n_in, len(x))
        x[:n_in] *= np.hanning(2 * n_in)[:n_in]
    if n_out > 0:
        n_out = min(n_out, len(x))
        x[-n_out:] *= np.hanning(2 * n_out)[n_out:]
    return x


def resample(x, sr_in, semitones=0.0):
    """Band-limited FFT resampling to 48 kHz with a pitch shift by playback rate (pitch and length change together, like
    adsp.pitch). The clip is zero-padded so the FFT's circular wrap never reaches it; the top 5 % of the kept band is
    tapered (raised cosine) instead of brick-walled, which keeps the Gibbs ringing out of the transient."""
    ratio = SR / sr_in / (2.0 ** (semitones / 12.0))      # output samples per input sample
    pad = 4096
    xp = np.concatenate([np.zeros(pad), x, np.zeros(pad)])
    n_in = len(xp)
    n_out = int(round(n_in * ratio))
    X = np.fft.rfft(xp)
    keep = min(len(X), n_out // 2 + 1)
    taper = np.ones(keep)
    t = max(8, keep // 20)
    taper[-t:] = 0.5 * (1 + np.cos(np.linspace(0, math.pi, t)))
    Y = np.zeros(n_out // 2 + 1, complex)
    Y[:keep] = X[:keep] * taper
    y = np.fft.irfft(Y, n_out) * (n_out / n_in)
    a = int(round(pad * ratio))
    return y[a:a + int(round(len(x) * ratio))]


def biquad(x, b, a):
    """Direct-form-I biquad (causal, so nothing rings before a transient)."""
    b0, b1, b2 = (v / a[0] for v in b)
    a1, a2 = a[1] / a[0], a[2] / a[0]
    y = np.empty(len(x))
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(np.asarray(x, np.float64).tolist()):
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, xi, y1, yi
        y[i] = yi
    return y


def rbj(kind, hz, q=0.7071):
    """RBJ cookbook 2nd-order high-pass / low-pass (Butterworth at q = 0.7071)."""
    w = 2 * math.pi * hz / SR
    c, al = math.cos(w), math.sin(w) / (2 * q)
    if kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    else:
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    return b, [1 + al, -2 * c, 1 - al]


def k_weight(x):
    """BS.1770 K-weighting at 48 kHz (high shelf + RLB high-pass); the coefficients of adsp._k_weight."""
    y = biquad(x, [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
    return biquad(y, [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])


def lufs_m_max(x, win_s=0.4, hop_s=0.1):
    """Loudest K-weighted window (default the 400 ms / 100 ms-hop BS.1770 momentary window, as adsp.lufs()[1]); a clip shorter
    than one window is zero padded to it. win_s=0.1 is adsp.loudness_short: what a click is heard at."""
    win, hop = int(win_s * SR), int(hop_s * SR)
    y = np.concatenate([x, np.zeros(max(0, win - len(x)))])
    e = k_weight(y) ** 2
    c = np.concatenate([[0.0], np.cumsum(e)])
    starts = np.arange(0, len(e) - win + 1, hop)
    ms = (c[starts + win] - c[starts]) / win
    return float(-0.691 + 10 * math.log10(max(float(ms.max()), 1e-12)))


def peak_db(x):
    return 20 * math.log10(max(float(np.abs(x).max()), 1e-12))


def true_peak_db(x, over=4):
    """Inter-sample peak: the clip upsampled 4x (band-limited FFT, zero padded so nothing wraps), as BS.1770's dBTP."""
    pad = 2048
    xp = np.concatenate([np.zeros(pad), np.asarray(x, np.float64), np.zeros(pad)])
    X = np.fft.rfft(xp)
    Y = np.zeros(len(xp) * over // 2 + 1, complex)
    Y[:len(X)] = X
    y = np.fft.irfft(Y, len(xp) * over) * over
    return 20 * math.log10(max(float(np.abs(y).max()), 1e-12))


def centroid_hz(x):
    p = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / SR)
    return float((f * p).sum() / max(p.sum(), 1e-20))


def lead_ms(x, below_db=-40.0):
    """Leading silence: time until the signal first reaches `below_db` under its own peak."""
    a = np.abs(x)
    return float(np.argmax(a >= a.max() * 10 ** (below_db / 20)) * 1000.0 / SR)


# ------------------------------------------------------------------------------------------------------------------ recipe
def L(src, start_ms=0.0, len_ms=60.0, st=0.0, at_ms=0.0, db=0.0, frac=0.03, fade_in_ms=0.2):
    """One layer: cut `len_ms` of `src` from `start_ms` after its onset (the first sample at `frac` of the file's peak),
    resampled to 48 kHz at `st` semitones, placed at `at_ms` in the output with `db` gain."""
    return dict(src=src, start_ms=start_ms, len_ms=len_ms, st=st, at_ms=at_ms, db=db, frac=frac, fade_in_ms=fade_in_ms)


def tick(layers, length_ms, fade_ms, peak=-20.0, hp=120.0, lp=None, variants=None, phase=1):
    return dict(tier="tick", layers=layers, length_ms=length_ms, fade_ms=fade_ms, level=peak + LIFT_DB, hp=hp, lp=lp,
                variants=variants, phase=phase)


def tone(tier, layers, length_ms, fade_ms, hp=70.0, lp=None, phase=1):
    return dict(tier=tier, layers=layers, length_ms=length_ms, fade_ms=fade_ms,
                level=NOTIFY_LUFS if tier == "notify" else CUE_SHORT, hp=hp, lp=lp, variants=None, phase=phase)


PRESS, MOD = "K:button-pressed", "K:button-pressed-modifier"     # KDE key ticks: a ~13 ms rounded "tock" at 527 / 785 Hz
VOL = "Y:audio-volume-change"                                    # Yaru volume tick: 300 Hz body + 1.1 kHz click, ~58 ms
BELL = "Y:bell"                                                  # Yaru bell: one pure D5 (587 Hz) ping, exponential decay
V3 = (0.0, 3.0, -3.0)          # variant pitch offsets, percent
V2 = (-2.0, 2.0)

EVENTS = {
    # --- clicks: KDE key ticks. Confirm = low tick then high (a rising fifth), cancel = the mirror, destructive = both
    #     ticks pitched down, falling, and low-passed (lower, duller).
    "Click": tick([L(PRESS)], 70, 35, variants=V3),
    "ClickConfirm": tick([L(PRESS), L(MOD, at_ms=40, db=-1.0)], 105, 40),
    "ClickCancel": tick([L(MOD), L(PRESS, at_ms=40, db=-1.0)], 105, 40),
    "ClickDestructive": tick([L(PRESS, st=-4.0, len_ms=50), L(PRESS, st=-9.0, len_ms=50, at_ms=45, db=-1.0)], 115, 45, lp=900.0),
    # --- toggles: the modifier tick, up a tone for on, down a major third for off
    "ToggleOn": tick([L(MOD, st=2.0)], 70, 35, variants=V2),
    "ToggleOff": tick([L(MOD, st=-4.0)], 75, 35, variants=V2),
    # --- tab: the attack of the Yaru bell, a short pure ping a fourth up (G5)
    "TabSelect": tick([L(BELL, len_ms=125, st=5.0)], 95, 55, peak=-21.0),
    # --- play mode: KDE plug-in arpeggio up (D4 A4 D5) / unplug down (A4 E4 A#3); pause / resume: one pure Yaru tone,
    #     D4 for pause, the octave above for resume; step: the Yaru volume tick a fourth up
    "PlayEnter": tone("cue", [L("K:power-plug", len_ms=600, frac=0.01, fade_in_ms=2.0)], 600, 260),
    "PlayExit": tone("cue", [L("K:power-unplug", len_ms=640, frac=0.01, fade_in_ms=2.0)], 640, 280),
    "Pause": tone("cue", [L("Y:dialog-error", len_ms=380, frac=0.01, fade_in_ms=1.0)], 380, 170),
    "Resume": tone("cue", [L(BELL, len_ms=300, frac=0.01, fade_in_ms=0.5)], 300, 140),
    "Step": tick([L(VOL, st=5.0, frac=0.2)], 55, 30),
    # --- notifications: four different shapes. Info: KDE's soft information chord (D5/A4 settling to D4/A3). Success: Yaru's
    #     two-chord cadence that resolves up. Warning: Yaru's single tritone dyad (G#4 + D4). Error: KDE's serious error, a
    #     falling octave (C5 -> C4) - the lowest and darkest of the four.
    "NotifyInfo": tone("notify", [L("K:dialog-information", len_ms=760, frac=0.01, fade_in_ms=2.0)], 760, 340),
    "NotifySuccess": tone("notify", [L("Y:complete", len_ms=720, frac=0.01, fade_in_ms=1.0)], 720, 260),
    "NotifyWarning": tone("notify", [L("Y:dialog-warning", len_ms=440, frac=0.01, fade_in_ms=1.0)], 440, 200),
    "NotifyError": tone("notify", [L("K:dialog-error-serious", len_ms=860, frac=0.01, fade_in_ms=2.0)], 860, 380),
    # --- edit: save = KDE's octave bell (B3 B4 B5, the glassiest sound in either set); redo = KDE's quick A4 -> D5 rise;
    #     undo = KDE's device-removed fall (A4/E4 -> E4 -> A3)
    "Save": tone("cue", [L("K:bell-window-system", len_ms=520, frac=0.01, fade_in_ms=2.0)], 520, 260),
    "Undo": tone("cue", [L("K:device-removed", len_ms=330, frac=0.01, fade_in_ms=2.0)], 330, 160),
    "Redo": tone("cue", [L("K:completion-success", len_ms=260, frac=0.01, fade_in_ms=2.0)], 260, 120),

    # ===== phase 2
    "Hover": tick([L(MOD, st=5.0, len_ms=45)], 50, 30, peak=-28.0, lp=3000.0, variants=V3, phase=2),
    "SliderTick": tick([L(VOL, frac=0.2)], 60, 30, peak=-22.0, variants=V3, phase=2),
    "SliderGrab": tick([L(VOL, st=-3.0, frac=0.2)], 70, 35, phase=2),
    "SliderRelease": tick([L(VOL, st=2.0, frac=0.2)], 60, 30, peak=-21.0, phase=2),
    # menus: the first two notes of Yaru's device-added (D4 -> A4) / device-removed (D5 -> A4), a fourth up so they are light
    "MenuOpen": tone("cue", [L("Y:device-added", len_ms=140, st=5.0, frac=0.01, fade_in_ms=0.5)], 105, 50, phase=2),
    "MenuClose": tone("cue", [L("Y:device-removed", len_ms=140, st=5.0, frac=0.01, fade_in_ms=0.5)], 105, 50, phase=2),
    # drag: a short low bell ping to pick up; drop accepted: Yaru's octave leap (D4 -> D5); drop rejected: two hits of Yaru's
    # question tone (a "nuh-uh") pitched down a minor third and low-passed
    "DragStart": tick([L(BELL, len_ms=100, st=-7.0)], 85, 50, peak=-21.0, phase=2),
    "DropAccept": tone("cue", [L("Y:message-new-instant", len_ms=300, frac=0.01, fade_in_ms=0.5)], 300, 150, phase=2),
    "DropReject": tone("cue", [L("Y:dialog-question", len_ms=140, st=-3.0, frac=0.01, fade_in_ms=1.0)], 166, 60,
                       lp=1200.0, phase=2),

    # ===== phase 5: actions. Objects: select = a light high tick, deselect the same a fifth lower; create = the whole of
    # Yaru's device-added rise, delete = its device-removed fall a fourth down and darkened; duplicate = a double tap;
    # copy / paste = two modifier ticks rising / one bell ping landing; rename = the volume tick up a fifth
    "Select": tick([L(MOD, st=7.0, len_ms=40)], 45, 28, peak=-25.0, lp=4500.0, variants=V3, phase=5),
    "Deselect": tick([L(MOD, len_ms=40)], 45, 28, peak=-27.0, lp=3500.0, phase=5),
    "Create": tone("cue", [L("Y:device-added", len_ms=300, frac=0.01, fade_in_ms=0.5)], 300, 140, phase=5),
    "Duplicate": tick([L(PRESS, st=3.0, len_ms=40), L(PRESS, st=8.0, len_ms=40, at_ms=50, db=-1.0)], 100, 35, phase=5),
    "Delete": tone("cue", [L("Y:device-removed", len_ms=280, st=-5.0, frac=0.01, fade_in_ms=0.5)], 300, 140, lp=1600.0,
                   phase=5),
    "Copy": tick([L(MOD, st=3.0, len_ms=35), L(MOD, st=10.0, len_ms=35, at_ms=35, db=-1.5)], 80, 30, peak=-22.0, phase=5),
    "Paste": tick([L(BELL, len_ms=100, st=2.0)], 95, 55, peak=-22.0, phase=5),
    "Rename": tick([L(VOL, st=7.0, frac=0.2)], 55, 30, peak=-22.0, phase=5),
    # components: add = Yaru's octave leap a fourth up, remove = Yaru's device-removed, both short. Prefabs: apply = KDE's
    # A4 -> D5 rise a fourth down (rounder than redo); revert = KDE's device-removed a tone up; enter / exit = the Play mode
    # arpeggios a fifth up and shorter, so editing a prefab sounds like a lighter cousin of Play
    "ComponentAdd": tone("cue", [L("Y:message-new-instant", len_ms=220, st=5.0, frac=0.01, fade_in_ms=0.5)], 220, 110,
                         phase=5),
    "ComponentRemove": tone("cue", [L("Y:device-removed", len_ms=220, frac=0.01, fade_in_ms=0.5)], 220, 110, lp=2200.0,
                            phase=5),
    "PrefabApply": tone("cue", [L("K:completion-success", len_ms=300, st=-5.0, frac=0.01, fade_in_ms=2.0)], 300, 140,
                        phase=5),
    "PrefabRevert": tone("cue", [L("K:device-removed", len_ms=300, st=2.0, frac=0.01, fade_in_ms=2.0)], 300, 140, phase=5),
    "PrefabEnter": tone("cue", [L("K:power-plug", len_ms=420, st=7.0, frac=0.01, fade_in_ms=2.0)], 420, 200, phase=5),
    "PrefabExit": tone("cue", [L("K:power-unplug", len_ms=440, st=7.0, frac=0.01, fade_in_ms=2.0)], 440, 200, phase=5),
    # tools & camera: tool switch = a high bell ping (the caller raises the pitch per tool: move, rotate, scale...);
    # snap / space = the volume tick up a sixth / down a tone; frame = a longer bell a fifth up; view preset = a plain ping
    "ToolSwitch": tick([L(BELL, len_ms=80, st=12.0)], 75, 45, peak=-25.0, phase=5),
    "SnapToggle": tick([L(VOL, st=9.0, frac=0.2)], 55, 30, peak=-22.0, phase=5),
    "SpaceToggle": tick([L(VOL, st=-2.0, frac=0.2)], 60, 30, peak=-22.0, phase=5),
    "FrameSelected": tone("cue", [L(BELL, len_ms=220, st=7.0, frac=0.01, fade_in_ms=0.5)], 220, 120, phase=5),
    "ViewPreset": tick([L(BELL, len_ms=100)], 95, 55, peak=-23.0, phase=5),
    # panels & trees: a dock panel opens / closes with the menu pair a tone lower; a tree node expands / collapses with the
    # modifier tick up a third / down a tone, quiet and dull (they come in runs). Scene open: KDE's service-login chime.
    "PanelOpen": tone("cue", [L("Y:device-added", len_ms=140, st=3.0, frac=0.01, fade_in_ms=0.5)], 105, 50, phase=5),
    "PanelClose": tone("cue", [L("Y:device-removed", len_ms=140, st=3.0, frac=0.01, fade_in_ms=0.5)], 105, 50, phase=5),
    "TreeExpand": tick([L(MOD, st=4.0, len_ms=35)], 40, 25, peak=-27.0, lp=3500.0, variants=V2, phase=5),
    "TreeCollapse": tick([L(MOD, st=-2.0, len_ms=35)], 40, 25, peak=-27.0, lp=3000.0, variants=V2, phase=5),
    "SceneOpen": tone("cue", [L("K:service-login", len_ms=700, frac=0.01, fade_in_ms=2.0)], 700, 320, phase=5),
    # builds & housekeeping: build start = KDE's message-sent whoosh-chime; success / failure = KDE's outcome pair (notify
    # tier, they finish long jobs); console clear = Yaru's trash-empty; asset import = KDE's contact-in blip
    "BuildStart": tone("cue", [L("K:message-sent-instant", len_ms=500, frac=0.01, fade_in_ms=2.0)], 500, 240, phase=5),
    "BuildSuccess": tone("notify", [L("K:outcome-success", len_ms=1000, frac=0.01, fade_in_ms=2.0)], 1000, 450, phase=5),
    "BuildFail": tone("notify", [L("K:outcome-failure", len_ms=1000, frac=0.01, fade_in_ms=2.0)], 1000, 450, phase=5),
    "ConsoleClear": tone("cue", [L("Y:trash-empty", len_ms=500, frac=0.01, fade_in_ms=1.0)], 500, 240, phase=5),

    # ===== phase 6
    # "done" chimes: one family - KDE's outcome-success rise, cut short - tinted per long task so they read as related but
    # tell apart: import = up a fourth, brighter; shader = up a fifth, short and dry; sync = as is, darkened; module reload =
    # up an octave, the shortest
    "AssetImport": tone("cue", [L("K:outcome-success", len_ms=560, st=5.0, frac=0.01)], 520, 240, phase=6),
    "ShaderReload": tone("cue", [L("K:outcome-success", len_ms=480, st=7.0, frac=0.01)], 440, 160, phase=6),
    "SyncDone": tone("cue", [L("K:outcome-success", len_ms=560, frac=0.01)], 520, 240, lp=2500.0, phase=6),
    "ModuleReload": tone("cue", [L("K:outcome-success", len_ms=420, st=12.0, frac=0.01)], 400, 180, phase=6),
    # assets: folder in = two modifier ticks rising a fourth, back = the mirror; open = KDE's highlight chime; move = KDE's
    # contact-out glide; reimport = KDE's rotation whirl; an empty search = a low muffled bell; favourite = a high bell ping,
    # unfavourite = the same a sixth lower and duller
    "FolderEnter": tick([L(MOD, st=3.0, len_ms=35), L(MOD, st=8.0, len_ms=35, at_ms=28, db=-2.0)], 70, 30, phase=6),
    "FolderBack": tick([L(MOD, st=8.0, len_ms=35), L(MOD, st=3.0, len_ms=35, at_ms=28, db=-2.0)], 70, 30, phase=6),
    "AssetOpen": tone("cue", [L("K:message-highlight", len_ms=320, frac=0.01)], 300, 140, phase=6),
    "AssetMove": tone("cue", [L("K:message-contact-out", len_ms=280, frac=0.2)], 260, 120, phase=6),   # past its soft swell
    "AssetReimport": tone("cue", [L("K:completion-rotation", len_ms=420, frac=0.01)], 400, 180, phase=6),
    "SearchEmpty": tone("cue", [L(BELL, len_ms=150, st=-12.0, frac=0.01)], 140, 70, lp=1500.0, phase=6),
    "FavoriteOn": tick([L(BELL, len_ms=110, st=9.0)], 100, 55, phase=6),
    "FavoriteOff": tick([L(BELL, len_ms=100, st=0.0)], 95, 50, lp=3000.0, phase=6),
    # objects: reset = Yaru's trash-empty sweep up a minor third, dulled; randomize = three key ticks tumbling (D, A, F#); equip / unequip =
    # the volume tick a third up / down
    "ResetDefault": tone("cue", [L("Y:trash-empty", len_ms=280, st=3.0, frac=0.01)], 260, 120, lp=3000.0, phase=6),
    "Randomize": tick([L(PRESS, st=2.0, len_ms=25), L(PRESS, st=9.0, len_ms=25, at_ms=28, db=-1.0),
                       L(PRESS, st=6.0, len_ms=30, at_ms=56, db=-2.0)], 105, 35, variants=V2, phase=6),
    "Equip": tick([L(VOL, st=4.0, frac=0.2)], 60, 30, phase=6),
    "Unequip": tick([L(VOL, st=-4.0, frac=0.2)], 65, 30, lp=2500.0, phase=6),
    # animator: states are the create / delete pair, lighter (shorter, a fifth up); transitions = two key ticks rising a fifth
    # (link) / falling and dulled (break); default state = a bell a fifth up; layers = the panel pair higher; parameters = two
    # modifier ticks rising / falling; the live state change = the quietest, highest bell tick
    "StateAdd": tone("cue", [L("Y:device-added", len_ms=170, st=7.0, frac=0.01)], 160, 70, phase=6),
    "StateDelete": tone("cue", [L("Y:device-removed", len_ms=170, st=2.0, frac=0.01)], 160, 70, lp=2000.0, phase=6),
    "TransitionLink": tick([L(PRESS, len_ms=35), L(PRESS, st=7.0, len_ms=35, at_ms=35, db=-1.0)], 90, 35, phase=6),
    "TransitionBreak": tick([L(PRESS, st=7.0, len_ms=35), L(PRESS, len_ms=35, at_ms=35, db=-1.0)], 90, 35, lp=1500.0,
                            phase=6),
    "DefaultState": tone("cue", [L(BELL, len_ms=200, st=7.0, frac=0.01)], 180, 90, phase=6),
    "LayerAdd": tone("cue", [L("Y:device-added", len_ms=130, st=10.0, frac=0.01)], 120, 55, phase=6),
    "LayerRemove": tone("cue", [L("Y:device-removed", len_ms=130, st=5.0, frac=0.01)], 120, 55, phase=6),
    "ParamAdd": tick([L(MOD, st=9.0, len_ms=35), L(MOD, st=14.0, len_ms=35, at_ms=30, db=-2.0)], 75, 30, phase=6),
    "ParamRemove": tick([L(MOD, st=14.0, len_ms=35), L(MOD, st=9.0, len_ms=35, at_ms=30, db=-2.0)], 75, 30, lp=3000.0,
                        phase=6),
    "LiveState": tick([L(BELL, len_ms=60, st=12.0)], 60, 35, lp=4000.0, phase=6),
    # curves: a key added = the volume tick up a sixth, deleted = down a fifth and dull; tangent mode = one modifier tick;
    # a preset = Yaru's message chime
    "KeyAdd": tick([L(VOL, st=9.0, frac=0.2)], 55, 30, phase=6),
    "KeyDelete": tick([L(VOL, st=-7.0, frac=0.2)], 65, 30, lp=2000.0, phase=6),
    "TangentMode": tick([L(MOD, len_ms=35)], 45, 25, phase=6),
    "PresetApply": tone("cue", [L("Y:message", len_ms=320, frac=0.01)], 300, 140, phase=6),
    # system: blocked = drop-reject's "nuh-uh", shorter; a dead shortcut = one key tick a fifth down, muffled; go to line =
    # the volume tick an octave up; screenshot = a shutter (a key tick a fifth down, then the modifier tick); resolution = the modifier
    # tick down a fourth; maximise / restore = the panel pair a fourth lower; cursor lock / release = the volume tick down /
    # up a fourth (the mouse is taken / given back)
    "Blocked": tone("cue", [L("Y:dialog-question", len_ms=110, st=-3.0, frac=0.01)], 120, 50, lp=1200.0, phase=6),
    "ShortcutInvalid": tick([L(PRESS, st=-7.0, len_ms=45)], 60, 30, lp=900.0, phase=6),
    "GotoLine": tick([L(VOL, st=12.0, frac=0.2)], 55, 30, phase=6),
    "Screenshot": tick([L(PRESS, st=-7.0, len_ms=30), L(MOD, len_ms=35, at_ms=40, db=-1.0)], 90, 35, phase=6),
    "ResolutionChange": tick([L(MOD, st=-5.0, len_ms=40)], 50, 28, phase=6),
    "ViewMaximize": tone("cue", [L("Y:device-added", len_ms=140, st=-2.0, frac=0.01)], 120, 55, phase=6),
    "ViewRestore": tone("cue", [L("Y:device-removed", len_ms=140, st=-2.0, frac=0.01)], 120, 55, phase=6),
    "CursorLock": tick([L(VOL, st=-5.0, frac=0.2)], 65, 30, phase=6),
    "CursorRelease": tick([L(VOL, st=5.0, frac=0.2)], 55, 30, phase=6),
}


# ------------------------------------------------------------------------------------------------------------------ render
def render(src_root, ev, pitch_pct=0.0):
    """-> (mono float64 at 48 kHz before levelling, provenance list, edit notes)."""
    n = int(round(ev["length_ms"] / 1000 * SR))
    out = np.zeros(n + SR)
    uses = []
    dpitch = 12 * math.log2(1 + pitch_pct / 100.0)
    for ly in ev["layers"]:
        s = source(src_root, ly["src"])
        x, sr = s["x"], s["sr"]
        on = onset(x, ly["frac"])
        cut = on + int(round(ly["start_ms"] / 1000 * sr))
        a = max(0, cut - int(round(PRE_ROLL_MS / 1000 * sr)))               # pre-roll: the fade-in runs over it
        b = min(len(x), cut + int(round(ly["len_ms"] / 1000 * sr)))
        fade_in_ms = max(ly["fade_in_ms"], MIN_FADE_IN_MS[ev["tier"]])
        seg = fade(x[a:b], (cut - a) + int(round(fade_in_ms / 1000 * sr)), 0)
        st = ly["st"] + dpitch
        y = resample(seg, sr, st) * 10 ** (ly["db"] / 20)
        at = int(round(ly["at_ms"] / 1000 * SR))
        e = min(len(out), at + len(y))
        out[at:e] += y[:e - at]
        uses.append(dict(file=s["rel"], set=s["set"], start=a / sr, end=b / sr, st=st, at_ms=ly["at_ms"], db=ly["db"],
                         downmix=s["downmix"], sr=sr))
    y = out[:n]
    y = biquad(y, *rbj("hp", ev["hp"]))                    # DC / sub rumble (KDE files carry some near 0 Hz)
    if ev["lp"]:
        y = biquad(y, *rbj("lp", ev["lp"]))
    y = fade(y, 0, int(ev["fade_ms"] / 1000 * SR))
    return y, uses


def level(y, ev):
    """Plain gain to the tier's target; returns (y, achieved note)."""
    if ev["tier"] == "tick":
        g = ev["level"] - peak_db(y)
        g_cap = TICK_SHORT_CAP - lufs_m_max(y, 0.1, 0.025)
        if g_cap < g:
            return y * 10 ** (g_cap / 20), f"{TICK_SHORT_CAP:.0f} LUFS on the 100 ms window (below its {ev['level']:.0f} dBFS peak target)"
        g_min = TICK_SHORT_MIN - lufs_m_max(y, 0.1, 0.025)
        if g_min > g:
            g_min = min(g_min, PEAK_CEIL_DB - true_peak_db(y))
            return y * 10 ** (g_min / 20), f"{TICK_SHORT_MIN:.0f} LUFS on the 100 ms window (above its {ev['level']:.0f} dBFS peak target)"
        g = min(g, PEAK_CEIL_DB - true_peak_db(y))
        return y * 10 ** (g / 20), f"sample peak {ev['level']:.0f} dBFS"
    if ev["tier"] == "notify":
        g, what = ev["level"] - lufs_m_max(y), f"LUFS-M max {ev['level']:.0f}"
    else:
        g, what = ev["level"] - lufs_m_max(y, 0.1, 0.025), f"{ev['level']:.0f} LUFS on the 100 ms window"
    if true_peak_db(y) + g > PEAK_CEIL_DB:
        g = PEAK_CEIL_DB - true_peak_db(y)
        what += f" (true-peak-capped at {PEAK_CEIL_DB:.0f} dBTP)"
    return y * 10 ** (g / 20), what


def edits_text(uses, ev, lvl_note):
    parts = []
    for u in uses:
        who = f"`{os.path.basename(u['file'])}` " if len(uses) > 1 else ""
        p = [f"{who}cut {u['start']:.4f}-{u['end']:.4f} s (lead-in silence dropped)", u["downmix"],
             f"resampled {u['sr'] / 1000:g} -> 48 kHz"]
        if abs(u["st"]) > 1e-6:
            p.append(f"pitch {u['st']:+.2f} st by playback rate")
        if u["at_ms"]:
            p.append(f"placed at +{u['at_ms']:g} ms")
        if u["db"]:
            p.append(f"{u['db']:+.1f} dB")
        parts.append(", ".join(p))
    tail = [f"high-pass {ev['hp']:g} Hz"]
    if ev["lp"]:
        tail.append(f"low-pass {ev['lp']:g} Hz")
    tail += [f"{ev['fade_ms']} ms fade-out, length {ev['length_ms']} ms", f"gain to {lvl_note}", "16-bit"]
    return ("; layered with ".join(parts) if len(parts) > 1 else parts[0]) + "; " + ", ".join(tail)


def main():
    src_root = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    for s in SETS.values():
        if not os.path.isdir(os.path.join(src_root, s["dir"])):
            sys.exit(f"missing source folder: {os.path.join(src_root, s['dir'])}")
    os.makedirs(OUT_DIR, exist_ok=True)
    for f in os.listdir(OUT_DIR):                         # this build owns every wav in the folder
        if f.lower().endswith(".wav"):
            os.remove(os.path.join(OUT_DIR, f))
    rows = []
    for name, ev in EVENTS.items():
        variants = ev["variants"] or (None,)
        for i, pct in enumerate(variants, 1):
            y, uses = render(src_root, ev, pct or 0.0)
            sets = {u["set"] for u in uses}
            assert len(sets) == 1, f"{name}: mixes source sets {sets}"
            y, note = level(y, ev)
            fn = f"{name}_{i}.wav" if ev["variants"] else f"{name}.wav"
            write_wav(os.path.join(OUT_DIR, fn), y)
            q = np.round(y * 32768.0) / 32768.0              # measure what was written
            row = dict(file=fn, event=name, phase=ev["phase"], tier=ev["tier"], set=sets.pop(), uses=uses,
                       ms=len(q) * 1000.0 / SR, peak=peak_db(q), lufs_m=lufs_m_max(q), short=lufs_m_max(q, 0.1, 0.025),
                       lead=lead_ms(q), cen=centroid_hz(q), pct=pct, edits=edits_text(uses, ev, note))
            if pct:
                row["edits"] = f"variant {pct:+g} % pitch; " + row["edits"]
            if ev["tier"] == "tick":
                assert row["ms"] < 120 and row["lead"] < PRE_ROLL_MS + 1.0, f"{fn}: {row['ms']:.0f} ms, lead {row['lead']:.2f} ms"
            else:
                assert row["ms"] < 1200, fn
            check_master(fn, q, ev, row)
            rows.append(row)
            print(f"{fn:24s} {row['tier']:6s} {row['set']} {row['ms']:5.0f} ms  peak {row['peak']:6.1f} dBFS  "
                  f"TP {row['tp']:5.1f}  LUFS-M {row['lufs_m']:6.1f}  100ms {row['short']:6.1f}  lead {row['lead']:.2f} ms  cen {row['cen']:5.0f} Hz")
    write_license(rows)
    print(f"{len(rows)} files -> {OUT_DIR}")


def check_master(fn, q, ev, row):
    """The mastering rules every written file must meet: click-free edges, -1 dBTP, and its tier's loudness."""
    head, tail = abs(float(q[0])), abs(float(q[-1]))
    assert head < EDGE_HEAD, f"{fn}: starts at {head:.4f} (a click)"
    assert tail < EDGE_TAIL, f"{fn}: ends at {tail:.4f} (a click)"
    tp = true_peak_db(q)
    assert tp <= PEAK_CEIL_DB + 0.1, f"{fn}: true peak {tp:.2f} dBTP"
    if ev["tier"] == "notify":
        got, lo, hi = row["lufs_m"], ev["level"], ev["level"]
    elif ev["tier"] == "cue":
        got, lo, hi = row["short"], ev["level"], ev["level"]
    else:
        got, lo, hi = row["short"], TICK_SHORT_MIN, TICK_SHORT_CAP
    off = lo - got if got < lo else (got - hi if got > hi else 0.0)
    capped = tp >= PEAK_CEIL_DB - 0.2 and got < lo      # held down by the ceiling: as loud as it can go
    assert off <= LOUDNESS_TOL_DB or capped, f"{fn}: {got:.1f} LUFS is {off:.1f} dB off its {ev['tier']} target"
    row["tp"] = tp


def write_license(rows):
    k, y = SETS["K"], SETS["Y"]
    lines = [
        "# Editor UI sounds: sources and licenses",
        "",
        "Generated by `tools/audio/build_editor_ui.py` (do not hand edit; rerun the script). Every `.wav` in this folder is an",
        "**adaptation** (cut, resampled, pitched, filtered, faded, levelled) of a sound from one of two Creative Commons",
        "BY-SA desktop sound themes, and is released under the same license as its source:",
        "",
        "| Source set | Outputs | License of the outputs |",
        "|---|---|---|",
        f"| KDE Ocean Sound Theme | {sum(r['set'] == 'K' for r in rows)} files | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) |",
        f"| Ubuntu Yaru Sound Theme | {sum(r['set'] == 'Y' for r in rows)} files | [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) |",
        "",
        "No file mixes the two sets. The source paths below are relative to the raw SFX folder (`Tartarus Assets\\Raw\\Audio\\SFX`",
        "on the shared Drive), where each theme's `WAV\\` folder holds 16-bit 44.1 kHz WAV conversions of the `.oga` files as shipped.",
        "",
        "## Attribution",
        "",
        "**KDE-derived files** (CC BY-SA 4.0):",
        "",
        "> Adapted from the *Ocean Sound Theme* by Guilherme Marçal Silva (KDE), 2023,",
        "> https://invent.kde.org/plasma/ocean-sound-theme, licensed under CC BY-SA 4.0",
        "> (https://creativecommons.org/licenses/by-sa/4.0/). Changes are listed per file below. These adaptations are",
        "> licensed under CC BY-SA 4.0 by the Tartarus Engine contributors.",
        "",
        "**Yaru-derived files** (CC BY-SA 3.0):",
        "",
        "> Adapted from sounds of the *WoodenBeaver* and *Touch-Remix* sound themes by madsrh",
        "> (https://github.com/madsrh/WoodenBeaver, https://github.com/madsrh/TouchRemix), as distributed in Ubuntu Yaru",
        "> (https://github.com/ubuntu/yaru, `sounds/src/stereo`), licensed under CC BY-SA 3.0",
        "> (https://creativecommons.org/licenses/by-sa/3.0/). Changes are listed per file below. These adaptations are",
        "> licensed under CC BY-SA 3.0 by the Tartarus Engine contributors.",
        "",
        "Credits line for the shipped editor: \"Editor sounds adapted from the Ocean Sound Theme by Guilherme Marçal Silva (KDE),",
        "CC BY-SA 4.0, and from WoodenBeaver / Touch-Remix by madsrh via Ubuntu Yaru, CC BY-SA 3.0.\"",
        "",
        "Both licenses come without warranties; see the license texts for the full terms. The build script itself is code and falls",
        "under the repository's own license; only the sounds are CC BY-SA.",
        "",
        "## Per file",
        "",
        "| File | Source file | Edits | License |",
        "|---|---|---|---|",
    ]
    for r in rows:
        srcs = "<br>".join(sorted({f"`{u['file']}`" for u in r["uses"]}))
        lic = SETS[r["set"]]["license"]
        lines.append(f"| `{r['file']}` | {srcs} | {r['edits']} | {lic} |")
    lines.append("")
    with open(os.path.join(OUT_DIR, "LICENSE.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


if __name__ == "__main__":
    main()
