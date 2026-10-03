"""Render audition reels to C:\\tb\\audio-reel (NOT committed): listen before trusting the numbers.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/render_reel.py [tails]

`tails` renders only the environment-tail audition reels to C:\\tb\\audio-reel\\tails: per gun, <gun>_tails_single.wav (one shot with the
generic fire_tail, then with each built space class) and <gun>_tails_burst_<class>.wav (a 10-round AK burst / 3 pumped 870 shots per
space, tail policy of the engine: every 2nd shot).

Per gun:
  <gun>_elements.wav      every committed element/variant in order, 0.45 s apart, with <gun>_elements_index.txt (time -> file)
  <gun>_burst.wav         AK: 10-shot full-auto burst @ 650 rpm with tails;  870: shot, pump, shot, pump (+ 3 s of tail)
  <gun>_<clip>_sync.wav   every clip of sync_map.json rebuilt from its events exactly as the engine should play it:
                          start(file) = frame / 60 - anchor_ms / 1000  (a mistimed or mis-ordered event is audible here)
Foley: foley_steps.wav (walk then run on every surface), foley_misc.wav.
"""
import json
import os
import sys

import numpy as np
import soundfile as sf

import abuild
import adsp

OUT = r"C:\tb\audio-reel"
SR = adsp.SR
FPS = 60.0
LAYERS = ("close", "sub", "mech", "tail", "far")


def load_manifest():
    doc = json.load(open(abuild.MANIFEST))
    by_key = {}
    for e in doc["files"]:
        by_key.setdefault(e["key"], []).append(e)
    for v in by_key.values():
        v.sort(key=lambda e: e["variant"])
    return by_key


def wav(e):
    x, sr = sf.read(os.path.join(abuild.AUDIO_DIR, e["file"]), dtype="float32", always_2d=True)
    return np.repeat(x, 2, axis=1) if x.shape[1] == 1 else x


class Reel:
    def __init__(self, seconds):
        self.buf = np.zeros((int(seconds * SR) + SR, 2), np.float32)

    def put(self, x, t, gain_db=0.0):
        s = max(0, int(round(t * SR)))
        e = min(len(self.buf), s + len(x))
        if e > s:
            self.buf[s:e] += x[:e - s] * 10 ** (gain_db / 20)

    def save(self, name, ceiling=-1.0):
        os.makedirs(OUT, exist_ok=True)
        end = int(np.nonzero(np.abs(self.buf).max(axis=1) > 1e-4)[0].max()) + int(0.3 * SR)
        pre = adsp.sample_peak_db(self.buf[:end])
        y = adsp.limit(self.buf[:end], ceiling)
        adsp.write_wav(os.path.join(OUT, name), y)
        print(f"{name:32s} {len(y) / SR:6.1f}s  LUFS-M max {adsp.lufs(y)[1]:6.1f}  TP {adsp.true_peak_db(y):5.1f}"
              f"  pre-limiter peak {pre:5.1f} dBFS")


class Picker:
    """Variant picker that never repeats the previous take of a key (what the engine should do too)."""
    def __init__(self, by_key, seed=7):
        self.by_key, self.last = by_key, {}
        self.rng = np.random.default_rng(seed)

    def pick(self, key):
        v = self.by_key[key]
        i = int(self.rng.integers(len(v)))
        if len(v) > 1 and i == self.last.get(key):
            i = (i + 1) % len(v)
        self.last[key] = i
        return v[i]


def shot(reel, pk, gun, t, gain_jitter=0.7, layers=LAYERS):
    for layer in layers:
        e = pk.pick(f"snd.{gun}.fire_{layer}")
        reel.put(wav(e), t + float(pk.rng.uniform(-0.0008, 0.0008)),
                 e.get("mix_db", 0.0) + float(pk.rng.uniform(-gain_jitter, gain_jitter)))


def element_reel(by_key, gun_tag, dir_prefix):
    r = Reel(sum(len(v) for k, v in by_key.items() if k.startswith(f"snd.{gun_tag}.")) * 0.55 + 8 + 4 * 6)
    idx, t = [], 0.3
    for key in sorted(k for k in by_key if k.startswith(f"snd.{gun_tag}.") and "fire_" not in k):
        for e in by_key[key]:
            r.put(wav(e), t)
            idx.append(f"{t:7.2f}s  {e['file']}  (anchor {e['anchor_ms']} ms, {e['lufs_m_max']} LUFS-M)")
            t += max(0.45, e["length_s"] + 0.25)
    for key in sorted(k for k in by_key if k.startswith(f"snd.{gun_tag}.fire_")):
        for e in by_key[key]:
            r.put(wav(e), t)
            idx.append(f"{t:7.2f}s  {e['file']}  (layer solo, {e['lufs_m_max']} LUFS-M)")
            t += min(e["length_s"], 2.0) + 0.5
    r.save(f"{gun_tag}_elements.wav")
    open(os.path.join(OUT, f"{gun_tag}_elements_index.txt"), "w").write("\n".join(idx))


def sync_reels(by_key, gun_tag, sm):
    pk = Picker(by_key, seed=11)
    for clip, evs in sm.items():
        if not clip.startswith(gun_tag + "/"):
            continue
        name = clip.split("/")[1]
        length = max(e["frame"] for e in evs) / FPS if evs else 0
        r = Reel(length + 8)
        used = 0
        for ev in evs:
            if ev["key"].endswith(".fire"):
                shot(r, pk, gun_tag, ev["frame"] / FPS + 0.2)
                used += 1
            elif ev["key"] in by_key:
                e = pk.pick(ev["key"])
                r.put(wav(e), ev["frame"] / FPS + 0.2 - e["anchor_ms"] / 1000.0)
                used += 1
        if used:
            r.save(f"{gun_tag}_{name}_sync.wav")


def tails_reels(by_key):
    global OUT
    base = OUT
    OUT = os.path.join(base, "tails")
    os.makedirs(OUT, exist_ok=True)
    classes = ["outdoor_open", "outdoor_urban", "indoor_small", "indoor_large"]
    for gun in ("ak", "870"):
        have = [c for c in classes if f"snd.{gun}.fire_tail_{c}" in by_key]
        # the engine's fallback for a class with no files is the generic tail, so the reel plays that for it too
        names = ["generic"] + classes
        r = Reel(len(names) * 5.5 + 4)
        idx, t = [], 0.3
        for name in names:
            key = f"snd.{gun}.fire_tail" if name == "generic" or name not in have else f"snd.{gun}.fire_tail_{name}"
            pk = Picker(by_key, seed=21)
            shot(r, pk, gun, t, layers=("close", "sub", "mech"))
            e = pk.pick(key)
            r.put(wav(e), t, e.get("mix_db", 0.0))
            idx.append(f"{t:6.2f}s  {name:14s} {e['file']}" + ("" if name == "generic" or name in have else "  (no files: generic fallback)"))
            t += 5.5
        r.save(f"{gun}_tails_single.wav")
        open(os.path.join(OUT, f"{gun}_tails_index.txt"), "w").write(chr(10).join(idx))
        for name in ["generic"] + have:
            key = f"snd.{gun}.fire_tail" if name == "generic" else f"snd.{gun}.fire_tail_{name}"
            pk = Picker(by_key, seed=5)
            shots, gap = (10, 60.0 / 650.0) if gun == "ak" else (3, 1.6)
            r = Reel(shots * gap + 6)
            for i in range(shots):
                shot(r, pk, gun, 0.2 + i * gap, layers=("close", "sub", "mech"))
                if i % 2 == 0:
                    e = pk.pick(key)
                    r.put(wav(e), 0.2 + i * gap, e.get("mix_db", 0.0))
            r.save(f"{gun}_tails_burst_{name}.wav")
    OUT = base


def main():
    by_key = load_manifest()
    if "tails" in sys.argv[1:]:
        tails_reels(by_key)
        return
    sm = json.load(open(os.path.join(abuild.HERE, "sync_map.json")))
    os.makedirs(OUT, exist_ok=True)
    pk = Picker(by_key, seed=3)
    for gun in ("ak", "870"):
        element_reel(by_key, gun, gun)
        sync_reels(by_key, gun, sm)
    # AK: 10-shot full-auto burst with tails
    r = Reel(10 * 0.093 + 6)
    # full-auto policy this reel demonstrates (and docs/AUDIO.md recommends): close/sub/mech on EVERY shot, tail on
    # every 2nd, far on every 3rd -- stacking ten 3 s tails sums to +10 dB and drowns the transients.
    for i in range(10):
        ls = ["close", "sub", "mech"] + (["tail"] if i % 2 == 0 else []) + (["far"] if i % 3 == 0 else [])
        shot(r, pk, "ak", 0.2 + i * 60.0 / 650.0, layers=ls)
    r.save("ak_burst10.wav")
    # 870: shot -> pump -> shot -> pump, tails ringing underneath
    r = Reel(14)
    t = 0.2
    for i in range(2):
        shot(r, pk, "870", t)
        pb, pf = pk.pick("snd.870.pump_back"), pk.pick("snd.870.pump_fwd")
        r.put(wav(pb), t + 0.62 - pb["anchor_ms"] / 1000)      # contact frames from the Pump clip (8 and 18 at 60 fps)
        r.put(wav(pf), t + 0.62 + 10 / FPS - pf["anchor_ms"] / 1000)
        t += 2.6
    r.save("870_shot_pump.wav")
    # foley
    r = Reel(90)
    t = 0.3
    for cat in sorted(k for k in {k.split(".")[2] for k in by_key if k.startswith("snd.foley.step_")}):
        for kind, gap in (("walk", 0.65), ("run", 0.30)):
            for n in range(6):
                e = pk.pick(f"snd.foley.{cat}.{kind}")
                r.put(wav(e), t)
                t += gap
            t += 0.5
    r.save("foley_steps.wav")
    r = Reel(40)
    t = 0.3
    for key in sorted(k for k in by_key if k.startswith("snd.foley.") and "step_" not in k):
        for e in by_key[key][:2]:
            r.put(wav(e), t)
            t += max(0.6, e["length_s"] + 0.3)
    r.save("foley_misc.wav")


if __name__ == "__main__":
    main()
