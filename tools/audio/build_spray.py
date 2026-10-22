"""Cut the Graffiti Designer's spray sounds (resources/EditorSounds/Spray{Start,Loop,End,Drip}.wav) from one CC0 recording.

Source: "Aerosol spray.wav" by C-V, https://freesound.org/s/516815/ (CC0 1.0), 48 kHz 24-bit stereo, three bursts.
Usage: python tools/audio/build_spray.py <path to 516815__c-v__aerosol-spray.wav>
Writes 48 kHz 16-bit mono; one gain for all three puts the loop's peak at -3 dBFS. Credits: resources/EditorSounds/SPRAY_CREDITS.md.
"""
import math, struct, sys, wave
from pathlib import Path

R = 48000
OUT = Path(__file__).resolve().parents[2] / "resources" / "EditorSounds"


def load_mono(path):
    w = wave.open(str(path))
    assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (2, 3, R), "expected the 48 kHz 24-bit stereo original"
    raw = w.readframes(w.getnframes())
    return [(int.from_bytes(raw[i:i + 3], "little", signed=True) + int.from_bytes(raw[i + 3:i + 6], "little", signed=True))
            / 2 / 8388608 for i in range(0, len(raw), 6)]


def main(src):
    m = load_mono(src)
    sl = lambda a, b: m[int(a * R):int(b * R)]
    # Loop: the steadiest stretch of the middle burst (4.35-6.05 s); its last 300 ms is crossfaded (equal power) into the head.
    body, X = sl(4.35, 6.05), int(0.30 * R)
    L = len(body) - X
    loop = body[:L]
    for i in range(X):
        t = i / X
        loop[i] = body[L + i] * math.cos(t * math.pi / 2) + body[i] * math.sin(t * math.pi / 2)
    # Start: the first burst's attack (0.60-0.95 s), 80 ms fade-out into the loop.
    start, F = sl(0.60, 0.95), int(0.08 * R)
    for i in range(F):
        start[-1 - i] *= i / F
    # End: the middle burst's release (5.95-6.45 s), 40 ms fade-in, 30 ms fade-out to silence.
    end, F = sl(5.95, 6.45), int(0.04 * R)
    for i in range(F):
        end[i] *= i / F
    F = int(0.03 * R)
    for i in range(F):
        end[-1 - i] *= i / F
    # Drip: a short wet tick for a paint run - the loudest 110 ms of the first burst's attack, 4 ms fade-in, 70 ms fade-out.
    a, b = int(0.60 * R), int(0.95 * R)
    W = int(0.110 * R)
    peak = max(range(a, b - W), key=lambda i: sum(abs(x) for x in m[i:i + W:8]))
    drip = m[peak:peak + W]
    F = int(0.004 * R)
    for i in range(F):
        drip[i] *= i / F
    F = int(0.070 * R)
    for i in range(F):
        drip[-1 - i] *= i / F
    g = 10 ** (-3 / 20) / max(abs(x) for x in loop)
    for name, d in (("SprayLoop.wav", loop), ("SprayStart.wav", start), ("SprayEnd.wav", end), ("SprayDrip.wav", drip)):
        o = wave.open(str(OUT / name), "wb")
        o.setnchannels(1); o.setsampwidth(2); o.setframerate(R)
        o.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(x * g * 32767)))) for x in d))
        o.close()
        print(f"{name} {len(d) / R:.2f} s")


if __name__ == "__main__":
    main(sys.argv[1])
