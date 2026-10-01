"""Placeholder combat sounds for the Arena, synthesised (stdlib only) into project/assets/Audio/Combat/.

Rough but readable: a rifle crack, a shotgun boom, a pump, a dry click, three bullet whizzes, a flesh
hit, a hitmarker tick and a kill tick, a body fall, a magazine reload. Deterministic (seeded), so
re-running gives identical files. Replace any of them with real recordings of the same name.

    python tools/gen_combat_sounds.py
"""
import math
import os
import random
import struct
import wave

RATE = 44100
OUT = os.path.join(os.path.dirname(__file__), "..", "project", "assets", "Audio", "Combat")


def write(name, samples):
    peak = max(1e-6, max(abs(s) for s in samples))
    gain = 0.89 / peak
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * gain)) * 32767)) for s in samples))


class OnePole:
    """Low-pass one-pole; high-pass is x - lowpass(x)."""
    def __init__(self, cutoff):
        self.a = math.exp(-2.0 * math.pi * cutoff / RATE)
        self.y = 0.0

    def __call__(self, x):
        self.y = (1.0 - self.a) * x + self.a * self.y
        return self.y


def noise(rng):
    return rng.uniform(-1.0, 1.0)


def env_exp(t, decay):
    return math.exp(-t / decay)


def gunshot(seed, length, crack_decay, body_decay, thump_hz, thump_decay, tail_decay, body_cut, tail_gain):
    rng = random.Random(seed)
    n = int(length * RATE)
    lp_body = OnePole(body_cut)
    lp_tail = OnePole(900.0)
    lp_tail2 = OnePole(500.0)
    hp = OnePole(2500.0)
    out = []
    for i in range(n):
        t = i / RATE
        x = noise(rng)
        crack = (x - hp(x)) * env_exp(t, crack_decay) * 1.4                     # supersonic snap
        body = lp_body(x) * env_exp(t, body_decay) * 2.2                        # muzzle blast
        thump = math.sin(2 * math.pi * thump_hz * t * (1.0 - 0.35 * min(1.0, t / 0.08))) * env_exp(t, thump_decay) * 0.9
        tail = lp_tail2(lp_tail(x)) * env_exp(t, tail_decay) * tail_gain       # the room / field echo
        attack = min(1.0, t / 0.0007)
        out.append((crack + body + thump + tail) * attack)
    return out


def click(seed, length, freq, decay, noise_gain=0.6):
    rng = random.Random(seed)
    out = []
    hp = OnePole(3000.0)
    for i in range(int(length * RATE)):
        t = i / RATE
        x = noise(rng)
        out.append((math.sin(2 * math.pi * freq * t) * 0.6 + (x - hp(x)) * noise_gain) * env_exp(t, decay))
    return out


def place(dst, src, at, gain=1.0):
    o = int(at * RATE)
    if len(dst) < o + len(src):
        dst.extend([0.0] * (o + len(src) - len(dst)))
    for i, s in enumerate(src):
        dst[o + i] += s * gain


def whizz(seed, length, f0, f1):
    """A round going past: band-limited noise swept down in pitch, swelling and falling away (Doppler)."""
    rng = random.Random(seed)
    n = int(length * RATE)
    lp = OnePole(f0)
    hp = OnePole(f0 * 0.4)
    out = []
    phase = 0.0
    for i in range(n):
        u = i / n
        cut = f0 + (f1 - f0) * u
        lp.a = math.exp(-2.0 * math.pi * cut / RATE)
        hp.a = math.exp(-2.0 * math.pi * cut * 0.35 / RATE)
        x = noise(rng)
        b = lp(x)
        b = b - hp(b)
        phase += 2 * math.pi * cut * 0.5 / RATE
        swell = math.exp(-((u - 0.32) ** 2) / 0.02)
        out.append((b * 3.0 + math.sin(phase) * 0.25) * swell)
    return out


def main():
    write("ak_shot", gunshot(1, 0.9, 0.006, 0.035, 95.0, 0.05, 0.28, 2200.0, 0.55))
    write("ak_shot_b", gunshot(2, 0.9, 0.006, 0.033, 100.0, 0.05, 0.3, 2400.0, 0.5))
    write("shotgun_shot", gunshot(3, 1.4, 0.008, 0.07, 62.0, 0.11, 0.45, 1300.0, 0.8))
    # Distant: the crack and the low end gone, mostly echo.
    write("ak_shot_far", gunshot(4, 1.2, 0.002, 0.02, 80.0, 0.02, 0.4, 700.0, 1.4))
    write("shotgun_shot_far", gunshot(5, 1.5, 0.002, 0.03, 55.0, 0.04, 0.55, 500.0, 1.6))

    pump = []
    place(pump, click(6, 0.08, 900.0, 0.012), 0.0)
    place(pump, click(7, 0.12, 300.0, 0.03, 0.9), 0.05, 0.8)       # the fore-end back
    place(pump, click(8, 0.08, 1300.0, 0.01), 0.28)
    place(pump, click(9, 0.14, 260.0, 0.035, 0.9), 0.31, 0.9)      # and home
    write("shotgun_pump", pump)

    write("dry_fire", click(10, 0.12, 2200.0, 0.008))

    reload_ = []
    place(reload_, click(11, 0.1, 700.0, 0.02), 0.0, 0.7)          # mag out
    place(reload_, click(12, 0.1, 500.0, 0.02, 0.8), 0.75, 1.0)    # mag in
    place(reload_, click(13, 0.1, 1500.0, 0.012), 0.95, 0.9)       # bolt
    place(reload_, click(14, 0.1, 1100.0, 0.012), 1.05, 0.9)
    write("reload", reload_)

    write("whizz_1", whizz(20, 0.32, 5200.0, 1800.0))
    write("whizz_2", whizz(21, 0.28, 6400.0, 2200.0))
    write("whizz_3", whizz(22, 0.36, 4600.0, 1500.0))

    rng = random.Random(30)
    lp = OnePole(500.0)
    hit = []
    for i in range(int(0.22 * RATE)):
        t = i / RATE
        hit.append((lp(noise(rng)) * 3.0 + math.sin(2 * math.pi * 120.0 * t) * 0.5) * env_exp(t, 0.035))
    write("flesh_hit", hit)

    write("hitmarker", click(31, 0.07, 2600.0, 0.012, 0.3))
    kill = []
    place(kill, click(32, 0.08, 2600.0, 0.014, 0.3), 0.0)
    place(kill, click(33, 0.1, 1700.0, 0.02, 0.3), 0.06)
    write("hitmarker_kill", kill)

    rng = random.Random(40)
    lp = OnePole(260.0)
    fall = []
    for i in range(int(0.7 * RATE)):
        t = i / RATE
        e = env_exp(t, 0.06) + 0.6 * env_exp(max(0.0, t - 0.18), 0.05) * (t > 0.18)
        fall.append(lp(noise(rng)) * 4.0 * e)
    write("body_fall", fall)
    print("wrote", len(os.listdir(OUT)), "files to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
