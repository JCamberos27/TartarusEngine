"""Build the local "premium" editor UI sound overrides (project/assets/Audio/EditorUI/*.wav) from the paid Drive sets.

  "C:\\Program Files\\Blender Foundation\\Blender 5.2\\5.2\\python\\bin\\python.exe" -I tools/audio/build_editor_overrides.py
      ["G:\\My Drive\\Tartarus Assets\\Raw\\Audio\\SFX"] [--out DIR] [--cc DIR] [--blender PATH]

LICENSING: the sources are two paid sets (SoundShelf Studio SFX, The Sample Stop "Menu Sounds") licensed to the project owner
only. The outputs are local swaps that the editor loads in place of the CC set (EditorSound::Init looks in
assets/Audio/EditorUI first). `project/assets/**/*.wav` is gitignored; never commit, copy or upload what this writes. The picks
and the reasons for them are in resources/EditorSounds/overrides.md; this script only converts them.

Decoding: the SoundShelf picks are mp3 and no mp3 decoder ships with numpy / `wave`. Blender's `aud` module (Audaspace, built on
Blender's own ffmpeg) decodes them: when this script runs inside Blender (`aud` importable) it decodes in process; otherwise it
runs `blender --background --factory-startup --python <this file> -- --decode-worker ...`, which writes each decoded file as a
float32 .npy into a temporary folder that is deleted afterwards. Nothing is ever written next to the sources.
The mp3s carry ~25 ms of encoder-delay silence at the start; the onset trim below removes it.

Everything else reuses tools/audio/build_editor_ui.py (wav io, Hann fades, band-limited resampling, RBJ biquads, BS.1770
K-weighting, the 100 ms / 400 ms loudness windows), so the overrides are measured exactly like the CC set they replace.

Output: 48 kHz, 16-bit, mono, named as the engine loads them: <Event>.wav or <Event>_1.wav, _2.wav... for variants.
Level: each override is plain-gained to the measured loudness of the committed CC file(s) it replaces (resources/EditorSounds):
  tick   (clicks, toggles, tab, step, hover, slider grab, drag start; < 120 ms)  loudest 100 ms K-weighted window, as the CC
         build levels them (for clips this short the 400 ms LUFS-M then matches too)
  cue    (play mode, pause / resume, save, undo / redo, menus, drop; < 700 ms)    the midpoint of the gain that matches the loudest
  notify (info / success / error; < 1.2 s)                                        100 ms window and the gain that matches LUFS-M
         max (400 ms). A sustained tone and a short arpeggio with the same 100 ms loudness differ by up to 3.5 dB on the 400 ms
         window (and a hard-attack thump the other way round); the midpoint halves the worst mismatch on either measure.
The SoundShelf "clicks" are sine-like tones with a 30-50 ms attack ramp, not transients, so matching their peak to the CC
ticks' -20 dBFS would make them ~10 dB louder than the 13 ms CC key ticks; matching loudness keeps them at the CC level.
No limiter, no compression; a gain that would push the sample peak past PEAK_CEIL_DB fails the build instead.

Deterministic: same sources in, bit-identical wavs out.
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)   # python -I (and Blender) do not put the script's folder on sys.path
import build_editor_ui as B  # noqa: E402

SR = B.SR
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_SRC = B.DEFAULT_SRC
DEFAULT_OUT = os.path.join(REPO, "project", "assets", "Audio", "EditorUI")
DEFAULT_CC = os.path.join(REPO, "resources", "EditorSounds")
DEFAULT_BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
PEAK_CEIL_DB = -6.0

SS = "SoundShelf Studio SFX"
TSS = os.path.join("UX Desktop SFX", "Menu Sounds")
INT, NAV, NOTE = os.path.join(SS, "01 - Interaction"), os.path.join(SS, "02 - Navigation and Panels"), \
    os.path.join(SS, "03 - Notifications and Alerts")


def ev(src, tier, length_ms, fade_ms, frac=0.05, fade_in_ms=1.0, hp=70.0, lp=None, variants=None):
    """One override. The cut starts at the first sample reaching `frac` of the source's peak (minus 0.1 ms pre-roll), with a
    `fade_in_ms` Hann fade-in: for the slow-attack SoundShelf tones a frac of 0.3 (about -10 dB) starts the cut part-way up
    the ramp, which gives the tick an onset at sample 0 instead of a 40 ms swell."""
    return dict(src=src, tier=tier, length_ms=length_ms, fade_ms=fade_ms, frac=frac, fade_in_ms=fade_in_ms, hp=hp, lp=lp,
                variants=variants)


V3 = (0.0, 3.0, -3.0)   # variant pitch offsets, percent (by playback rate, so length changes with pitch by the same 3 %)

# The rows of resources/EditorSounds/overrides.md that name a file. Not here: SliderTick and SliderRelease (the doc says keep
# the CC files) and NotifyWarning ("UI - Warning Buzz.mp3" measures as a 65 Hz buzz: partials on a 65 Hz grid, 48 dB of
# amplitude modulation at ~65 Hz, two 300 ms bursts; the style brief rules buzzy out, so the CC file stays).
EVENTS = {
    # ---- ticks (< 120 ms)
    "Click": ev(os.path.join(INT, "UI - Click - Soft.mp3"), "tick", 70, 40, frac=0.3, hp=120.0, variants=V3),
    "ClickConfirm": ev(os.path.join(INT, "UI - Swipe - Confirm.mp3"), "tick", 105, 50, frac=0.3, hp=120.0),
    # "ClickCancel": dropped - see DROPPED below
    "ClickDestructive": ev(os.path.join(INT, "UI - Click - Deep.mp3"), "tick", 110, 50, frac=0.3, hp=80.0),
    # "ToggleOn": dropped - see DROPPED below
    # "ToggleOff": dropped - see DROPPED below
    "TabSelect": ev(os.path.join(NAV, "UI - Swipe - Navigation Soft.mp3"), "tick", 95, 50, frac=0.3, hp=120.0),
    # "Step": dropped - see DROPPED below
    "Hover": ev(os.path.join(INT, "UI - Hover - Sci-Fi.mp3"), "tick", 50, 30, frac=0.1, fade_in_ms=0.5, hp=120.0,
                variants=V3),
    # "SliderGrab": dropped - see DROPPED below
    # "DragStart": dropped - see DROPPED below
    # ---- cues (< 700 ms)
    # "PlayEnter": dropped - see DROPPED below
    # "PlayExit": dropped - see DROPPED below
    "Pause": ev(os.path.join(TSS, "094_Menu Sound 2 - The Sample Stop.wav"), "cue", 300, 100, fade_in_ms=0.5, hp=40.0),
    "Resume": ev(os.path.join(TSS, "092_Menu Sound 1  - The Sample Stop.wav"), "cue", 300, 100, fade_in_ms=0.5, hp=40.0),
    "Save": ev(os.path.join(TSS, "100_Menu Sound 8 - The Sample Stop.wav"), "cue", 600, 280, fade_in_ms=0.5),
    # "Undo": dropped - see DROPPED below
    # "Redo": dropped - see DROPPED below
    "MenuOpen": ev(os.path.join(NAV, "UI - Menu - Open.mp3"), "cue", 180, 90, frac=0.1),
    "MenuClose": ev(os.path.join(NAV, "UI - Modal - Close Soft Tap.mp3"), "cue", 180, 90, frac=0.3),
    "DropAccept": ev(os.path.join(NOTE, "UI - Notification - PC.mp3"), "cue", 400, 160),
    # "DropReject": dropped - see DROPPED below
    # ---- notifications (< 1.2 s)
    "NotifyInfo": ev(os.path.join(NOTE, "UI - Notification - App.mp3"), "notify", 1000, 450),
    "NotifySuccess": ev(os.path.join(NOTE, "UI - Glass Ping - Soft.mp3"), "notify", 650, 280),
    "NotifyError": ev(os.path.join(TSS, "093_Menu Sound 10 - The Sample Stop.wav"), "notify", 650, 300, fade_in_ms=0.5,
                      hp=40.0),
}
DROPPED = {  # picks whose analysis ruled them out; the built-in CC sound stays for these events
    "ClickCancel": "rises (258 -> 419 Hz) like Confirm: cancel has to fall",
    "Undo": "rises (950 -> 1343 Hz) like Redo: undo has to fall",
    "Step": "the same file as ToggleOn",
    "ToggleOn": "a pair with ToggleOff, which is dropped (131 Hz, too low): one set per pair",
    "ToggleOff": "131 Hz, two octaves under the CC toggle; laptop speakers lose it",
    "SliderGrab": "65 Hz; laptop speakers lose it",
    "DropReject": "a 59 Hz thump; laptop speakers lose it",
    "DragStart": "a 110 ms fragment of a slow 450 ms swell",
    "PlayEnter": "falls 756 -> 600 Hz: entering Play has to rise",
    "PlayExit": "its pair PlayEnter is dropped: one set per pair",
    "Redo": "its pair Undo is dropped: one set per pair",
}
MAX_MS = {"tick": 120.0, "cue": 700.0, "notify": 1200.0}


# ------------------------------------------------------------------------------------------------------------------ decode
def find_blender(arg):
    for p in (arg, os.environ.get("TARTARUS_BLENDER"), DEFAULT_BLENDER):
        if p and os.path.isfile(p):
            return p
    sys.exit("mp3 decoding needs Blender's `aud` module: pass --blender PATH or set TARTARUS_BLENDER")


def decode_mp3s(paths, blender):
    """{path: (float64 (n, ch), sr)} for every mp3, through Blender's aud (in process when available)."""
    out = {}
    if not paths:
        return out
    try:
        import aud   # noqa: F401  (only inside Blender)
        for p in paths:
            out[p] = _aud_decode(p)
        return out
    except ImportError:
        pass
    with tempfile.TemporaryDirectory(prefix="tb_mp3_") as tmp:
        lst = os.path.join(tmp, "list.txt")
        with open(lst, "w", encoding="utf-8") as fh:
            fh.write("\n".join(paths))
        r = subprocess.run([blender, "--background", "--factory-startup", "--python", os.path.abspath(__file__), "--",
                            "--decode-worker", lst, tmp], capture_output=True, text=True)
        for i, p in enumerate(paths):
            f = os.path.join(tmp, f"{i}.npy")
            if not os.path.isfile(f):
                sys.exit(f"Blender failed to decode {p}:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
            meta = np.load(os.path.join(tmp, f"{i}.sr.npy"))
            out[p] = (np.load(f).astype(np.float64), int(meta[0]))
    return out


def _aud_decode(path):
    import aud
    s = aud.Sound(path)
    sr = int(s.specs[0])
    return np.asarray(s.data(), np.float64), sr


def decode_worker(lst, tmp):
    with open(lst, encoding="utf-8") as fh:
        paths = [l for l in fh.read().split("\n") if l]
    for i, p in enumerate(paths):
        x, sr = _aud_decode(p)
        np.save(os.path.join(tmp, f"{i}.npy"), x.astype(np.float32))
        np.save(os.path.join(tmp, f"{i}.sr.npy"), np.array([sr]))


# ------------------------------------------------------------------------------------------------------------------ build
def to_mono(x):
    """As build_editor_ui.source: mean of L and R unless the fold loses > 10 % of the energy, then the louder channel."""
    if x.shape[1] == 1:
        return x[:, 0], "mono source"
    L, R = x[:, 0], x[:, 1]
    mid = 0.5 * (L + R)
    keep = float(np.mean(mid ** 2) / max(1e-20, 0.5 * (np.mean(L ** 2) + np.mean(R ** 2))))
    if keep >= 0.9:
        return mid, "mean of L and R"
    left = np.mean(L ** 2) >= np.mean(R ** 2)
    return (L if left else R), f"{'left' if left else 'right'} channel only (L+R fold keeps {keep:.0%})"


def render(x, sr, e, pct):
    on = B.onset(x, e["frac"])
    a = max(0, on - int(0.0001 * sr))                        # 0.1 ms pre-roll
    st = 12 * math.log2(1 + pct / 100.0)
    need = int(math.ceil(e["length_ms"] / 1000 * sr * 2 ** (st / 12))) + 64
    seg = B.fade(x[a:a + need], int(e["fade_in_ms"] / 1000 * sr), 0)
    y = B.resample(seg, sr, st)
    n = int(round(e["length_ms"] / 1000 * SR))
    y = np.concatenate([y, np.zeros(max(0, n - len(y)))])[:n]
    y = B.biquad(y, *B.rbj("hp", e["hp"]))
    if e["lp"]:
        y = B.biquad(y, *B.rbj("lp", e["lp"]))
    y = B.fade(y, 0, int(e["fade_ms"] / 1000 * SR))
    return y, a / sr


def gain_db(y, tier, cc):
    gs = cc["s"] - B.lufs_m_max(y, 0.1, 0.025)
    if tier == "tick":
        return gs
    return 0.5 * (gs + cc["m"] - B.lufs_m_max(y))


def cc_files(cc_dir, name):
    one = os.path.join(cc_dir, name + ".wav")
    if os.path.isfile(one):
        return [one]
    out, i = [], 1
    while os.path.isfile(os.path.join(cc_dir, f"{name}_{i}.wav")):
        out.append(os.path.join(cc_dir, f"{name}_{i}.wav"))
        i += 1
    return out


def cc_stats(cc_dir, name):
    fs = cc_files(cc_dir, name)
    if not fs:
        sys.exit(f"no CC file for {name} in {cc_dir}")
    xs = [B.read_wav(f)[0][:, 0] for f in fs]
    return dict(files=[os.path.basename(f) for f in fs], peak=max(B.peak_db(x) for x in xs), m=float(np.mean([B.lufs_m_max(x) for x in xs])),
                s=float(np.mean([B.lufs_m_max(x, 0.1, 0.025) for x in xs])), cen=float(np.mean([B.centroid_hz(x) for x in xs])))


def check(q, tier, fn):
    """The engine-ready checks; returns a dict of the measurements."""
    i16 = np.round(q * 32768.0)
    r = dict(ms=len(q) * 1000.0 / SR, peak=B.peak_db(q), lead=B.lead_ms(q), dc=float(np.mean(q)),
             clip=int(np.sum(np.abs(i16) >= 32767)), m=B.lufs_m_max(q), s=B.lufs_m_max(q, 0.1, 0.025), cen=B.centroid_hz(q),
             first=float(abs(q[0])), tail=float(np.abs(q[-int(0.002 * SR):]).max()))
    assert r["ms"] < MAX_MS[tier], f"{fn}: {r['ms']:.0f} ms over the {tier} limit"
    assert r["lead"] < 1.0, f"{fn}: transient {r['lead']:.2f} ms in"
    assert r["clip"] == 0 and r["peak"] <= PEAK_CEIL_DB, f"{fn}: peak {r['peak']:.1f} dBFS"
    assert abs(r["dc"]) < 1e-4, f"{fn}: DC {r['dc']:.2e}"
    assert r["tail"] < 10 ** (-60 / 20), f"{fn}: last 2 ms not faded ({B.peak_db(np.array([r['tail']])):.0f} dBFS)"
    return r


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if argv[:1] == ["--decode-worker"]:
        decode_worker(argv[1], argv[2])
        return
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", nargs="?", default=DEFAULT_SRC, help="raw SFX folder (holds 'SoundShelf Studio SFX', 'UX Desktop SFX')")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cc", default=DEFAULT_CC, help="the committed CC set the levels are matched to")
    ap.add_argument("--blender", default=None)
    a = ap.parse_args(argv)

    paths = sorted({os.path.join(a.src, e["src"]) for e in EVENTS.values()})
    for p in paths:
        if not os.path.isfile(p):
            sys.exit(f"missing source: {p}")
    mp3 = [p for p in paths if p.lower().endswith(".mp3")]
    dec = decode_mp3s(mp3, find_blender(a.blender) if mp3 else None)
    src = {}
    for p in paths:
        x, sr = dec[p] if p in dec else B.read_wav(p)
        m, how = to_mono(x)
        src[p] = (m, sr, how)

    os.makedirs(a.out, exist_ok=True)
    for name in list(EVENTS) + list(DROPPED):  # stale outputs (and a dropped pick's old files) of this build only (a single <Event>.wav would hide variants)
        for f in os.listdir(a.out):
            stem = f[:-4] if f.lower().endswith(".wav") else None
            if stem == name or (stem and stem.startswith(name + "_") and stem[len(name) + 1:].isdigit()):
                os.remove(os.path.join(a.out, f))

    print(f"{'file':22s} {'tier':6s} {'ms':>4s} {'peak':>6s} {'100ms':>6s} {'LUFS-M':>6s} {'cen':>5s} {'lead':>5s}  |  CC "
          f"{'peak':>6s} {'100ms':>6s} {'LUFS-M':>6s} {'cen':>5s}  | source")
    n_out = 0
    for name, e in EVENTS.items():
        x, sr, how = src[os.path.join(a.src, e["src"])]
        cc = cc_stats(a.cc, name)
        for i, pct in enumerate(e["variants"] or (0.0,), 1):
            y, cut = render(x, sr, e, pct)
            g = gain_db(y, e["tier"], cc)
            y = y * 10 ** (g / 20)
            fn = f"{name}_{i}.wav" if e["variants"] else f"{name}.wav"
            B.write_wav(os.path.join(a.out, fn), y)
            q = B.read_wav(os.path.join(a.out, fn))[0][:, 0]   # measure what was written
            r = check(q, e["tier"], fn)
            n_out += 1
            var = f" {pct:+g}%" if pct else ""
            print(f"{fn:22s} {e['tier']:6s} {r['ms']:4.0f} {r['peak']:6.1f} {r['s']:6.1f} {r['m']:6.1f} {r['cen']:5.0f} "
                  f"{r['lead']:5.2f}  |  {cc['peak']:6.1f} {cc['s']:6.1f} {cc['m']:6.1f} {cc['cen']:5.0f}  | "
                  f"{os.path.basename(e['src'])} @{cut * 1000:.1f} ms, {how}, {sr / 1000:g} kHz{var}")
    print(f"{n_out} files -> {a.out}")


if __name__ == "__main__":
    main()
