"""Original Combine-style radio barks for the enemy squad, into project/assets/Audio/Voice/combine/.

Clipped military-police jargon (unit chatter, sector codes) spoken by Windows SAPI (System.Speech via
PowerShell: no pip packages needed) and run through a pure-stdlib radio chain: pitch down, 300-3400 Hz
band-pass, compression, soft clip, a touch of bit-crush, a static bed, and a radio-on chirp / off
squelch with a static tail. Two voices (David, Zira) say every line. Writes one mono 22.05 kHz wav per
line and voice plus manifest.json (event -> priority, cooldown, lines with text and files), which
src/Game/AI/SquadVoice.cpp reads. Deterministic (seeded); every line here is original wording.

    python tools/gen_combine_voice.py           # all lines
    python tools/gen_combine_voice.py --only contact   # one event (for tuning)
"""
import json
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 22050
OUT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "project", "assets", "Audio", "Voice", "combine"))

# event -> (priority 0..10, per-squad cooldown seconds, [lines]). Priority decides who may cut the
# channel; the cooldown stops one event repeating.
EVENTS = {
    "contact": (7, 8.0, [
        "Contact. Hostile in sector four, engaging.",
        "Visual on target, sector three. Weapons free.",
        "Target acquired. Hostile confirmed, engaging.",
    ]),
    "contact_relay": (5, 6.0, [
        "Copy contact, moving to engage.",
        "Hostile confirmed. Closing on the marker.",
    ]),
    "gunfire": (6, 6.0, [
        "Shots fired, sector two. Investigating.",
        "Weapons discharge. All units, alert.",
        "Audible gunfire. Check your sectors.",
    ]),
    "flank_left": (5, 8.0, [
        "Flanking left. Hold your fire lane.",
        "Taking the left side. Cover me.",
    ]),
    "flank_right": (5, 8.0, [
        "Flanking right. Hold your fire lane.",
        "Taking the right side. Cover me.",
    ]),
    "moving_up": (4, 8.0, [
        "Advancing. Maintain suppression.",
        "Moving up. Stay on target.",
        "Pushing forward to the next marker.",
    ]),
    "falling_back": (6, 10.0, [
        "Falling back. Repositioning to the fallback marker.",
        "Unit compromised. Withdrawing.",
        "Breaking contact. Cover my retreat.",
    ]),
    "lost_target": (5, 10.0, [
        "Lost visual. Last known position marked.",
        "Target is gone. Sweep the sector.",
        "Contact broken. Search pattern, stay sharp.",
    ]),
    "man_down": (8, 5.0, [
        "Unit down! I repeat, unit down!",
        "Officer down. Sector is compromised.",
        "We have a casualty. Requesting support.",
    ]),
    "wounded": (9, 4.0, [
        "I am hit! Need assistance!",
        "Taking fire. Unit damaged, requesting medic.",
        "Armor breach. I need support.",
    ]),
    "reloading": (3, 10.0, [
        "Reloading. Cover me.",
        "Magazine empty. Changing.",
        "Dry. Reloading now.",
    ]),
    "covering": (4, 12.0, [
        "Covering fire. Keep your heads down.",
        "Suppressing, sector four.",
        "Laying down fire. Move on my mark.",
    ]),
    "target_down": (6, 20.0, [
        "Target neutralized. Sector secure.",
        "Hostile is down. Confirm kill.",
        "Subject eliminated. Resume patrol.",
    ]),
    "player_hurt": (3, 12.0, [
        "Hostile is hit. Keep the pressure on.",
        "Good hit. He is bleeding. Press him.",
        "Target wounded. Finish him.",
    ]),
    "suspicious": (4, 10.0, [
        "Movement detected. Possible hostile.",
        "Did you hear that? Check the area.",
        "Something is out there. Weapons ready.",
    ]),
    "investigating": (3, 10.0, [
        "Moving to investigate. Cover my approach.",
        "Checking the source. Stay behind me.",
        "Going in for a look. Hold position.",
    ]),
    "all_clear": (2, 15.0, [
        "Sector clear. Resume patrol.",
        "Nothing here. Standing down.",
        "All clear. Return to post.",
    ]),
    "idle": (1, 25.0, [
        "Unit on station. No activity.",
        "Quiet in sector five. Nothing to report.",
        "Patrol checkpoint reached. All nominal.",
        "Maintaining watch. Report any movement.",
    ]),
    "melee": (8, 3.0, [
        "Close contact! Break away!",
        "Get back! Get back!",
    ]),
    "copy": (0, 0.0, [
        "Copy.",
        "Affirmative.",
        "Acknowledged. Moving.",
        "Roger that. Executing.",
    ]),
}

# voice name, SAPI rate, resample factor (<1 = lower pitch and slower), drive
VOICES = [
    ("Microsoft David Desktop", 4, 0.88, 2.4),
    ("Microsoft Zira Desktop", 4, 0.82, 2.8),
]
SUFFIX = ["a", "b"]


# --- speech ----------------------------------------------------------------------------------------

def synth_all(jobs, tmp):
    """jobs: list of (wav_path, voice_name, rate, text). One PowerShell run for all of them."""
    ps = os.path.join(tmp, "tts.ps1")
    data = os.path.join(tmp, "jobs.json")
    with open(data, "w", encoding="utf-8") as f:
        json.dump([{"path": p, "voice": v, "rate": r, "text": t} for p, v, r, t in jobs], f)
    with open(ps, "w", encoding="utf-8") as f:
        f.write(
            "Add-Type -AssemblyName System.Speech\n"
            "$jobs = Get-Content -Raw -Encoding UTF8 '" + data.replace("'", "''") + "' | ConvertFrom-Json\n"
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer\n"
            "$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(22050, "
            "[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)\n"
            "foreach ($j in $jobs) {\n"
            "  $s.SelectVoice($j.voice); $s.Rate = [int]$j.rate\n"
            "  $s.SetOutputToWaveFile($j.path, $fmt); $s.Speak($j.text); $s.SetOutputToNull()\n"
            "}\n")
    subprocess.check_call(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps])


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    n = len(raw) // 2
    return [s / 32768.0 for s in struct.unpack("<%dh" % n, raw)], rate


# --- DSP -------------------------------------------------------------------------------------------

class Biquad:
    def __init__(self, kind, fc, q=0.7071):
        w = 2.0 * math.pi * fc / RATE
        al = math.sin(w) / (2.0 * q)
        c = math.cos(w)
        if kind == "lp":
            b0, b1, b2 = (1 - c) / 2, 1 - c, (1 - c) / 2
        else:  # hp
            b0, b1, b2 = (1 + c) / 2, -(1 + c), (1 + c) / 2
        a0, a1, a2 = 1 + al, -2 * c, 1 - al
        self.b0, self.b1, self.b2 = b0 / a0, b1 / a0, b2 / a0
        self.a1, self.a2 = a1 / a0, a2 / a0
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def __call__(self, x):
        y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
        self.x2, self.x1 = self.x1, x
        self.y2, self.y1 = self.y1, y
        return y


def chain(samples, filters):
    out = []
    for x in samples:
        for f in filters:
            x = f(x)
        out.append(x)
    return out


def resample(samples, factor):
    """Plays the clip back at `factor` times the speed (<1 = slower, lower)."""
    n = int(len(samples) / factor)
    out = []
    last = len(samples) - 1
    for i in range(n):
        p = i * factor
        j = int(p)
        f = p - j
        a = samples[j]
        b = samples[min(j + 1, last)]
        out.append(a + (b - a) * f)
    return out


def trim(samples, thresh=0.008, pad=0.03):
    lo = 0
    while lo < len(samples) and abs(samples[lo]) < thresh:
        lo += 1
    hi = len(samples)
    while hi > lo and abs(samples[hi - 1]) < thresh:
        hi -= 1
    p = int(pad * RATE)
    return samples[max(0, lo - p):min(len(samples), hi + p)]


def compress(samples, thresh_db=-24.0, ratio=4.0, attack=0.004, release=0.09):
    ca = math.exp(-1.0 / (attack * RATE))
    cr = math.exp(-1.0 / (release * RATE))
    env = 0.0
    out = []
    for x in samples:
        a = abs(x)
        env = ca * env + (1 - ca) * a if a > env else cr * env + (1 - cr) * a
        db = 20.0 * math.log10(max(env, 1e-6))
        over = db - thresh_db
        gain_db = -over * (1.0 - 1.0 / ratio) if over > 0 else 0.0
        out.append(x * 10.0 ** ((gain_db + 8.0) / 20.0))
    return out


def soft_clip(samples, drive):
    norm = math.tanh(drive)
    return [math.tanh(drive * x) / norm for x in samples]


def bit_crush(samples, hold=2, bits=9):
    q = float(2 ** (bits - 1))
    out = []
    held = 0.0
    for i, x in enumerate(samples):
        if i % hold == 0:
            held = round(x * q) / q
        out.append(held)
    return out


def static_noise(n, rng, level, lp=3800.0, hp=500.0):
    f = [Biquad("lp", lp), Biquad("hp", hp)]
    return [level * v for v in chain([rng.uniform(-1, 1) for _ in range(n)], f)]


def chirp_on(rng):
    n = int(0.085 * RATE)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / n
        f = 1700.0 - 700.0 * t
        ph += 2 * math.pi * f / RATE
        env = math.sin(math.pi * min(1.0, t * 1.15)) ** 0.6
        out.append(0.34 * env * math.sin(ph) + 0.12 * env * rng.uniform(-1, 1))
    return out


def squelch_off(rng):
    n = int(0.11 * RATE)
    burst = static_noise(n, rng, 1.0, 3600.0, 700.0)
    out = [burst[i] * 0.55 * math.exp(-i / (0.035 * RATE)) for i in range(n)]
    # a short falling blip then the static tail
    ph = 0.0
    for i in range(int(0.04 * RATE)):
        f = 1100.0 - 6000.0 * (i / RATE)
        ph += 2 * math.pi * max(300.0, f) / RATE
        out[i] += 0.25 * math.sin(ph) * (1 - i / (0.04 * RATE))
    tail_n = int(0.22 * RATE)
    tail = static_noise(tail_n, rng, 1.0, 3000.0, 600.0)
    out += [tail[i] * 0.10 * math.exp(-i / (0.08 * RATE)) for i in range(tail_n)]
    return out


def radio(samples, resample_factor, drive, seed):
    rng = random.Random(seed)
    s = trim(samples)
    s = resample(s, resample_factor)
    s = chain(s, [Biquad("hp", 300), Biquad("hp", 300), Biquad("lp", 3400), Biquad("lp", 3400)])
    s = compress(s)
    s = soft_clip(s, drive)
    s = bit_crush(s)
    s = chain(s, [Biquad("lp", 3600)])           # soften the crush edges
    bed = static_noise(len(s), rng, 0.035)
    s = [a + b for a, b in zip(s, bed)]
    peak = max(1e-6, max(abs(x) for x in s))
    s = [x * 0.82 / peak for x in s]
    lead = static_noise(int(0.02 * RATE), rng, 0.08)
    return chirp_on(rng) + lead + s + squelch_off(rng)


def write_wav(path, samples):
    peak = max(1e-6, max(abs(x) for x in samples))
    gain = min(1.0, 0.92 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, x * gain)) * 32767)) for x in samples))
    return len(samples) / RATE


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    os.makedirs(OUT, exist_ok=True)
    manifest_path = os.path.join(OUT, "manifest.json")
    manifest = {"sampleRate": RATE, "voices": len(VOICES), "events": {}}
    if only and os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)

    with tempfile.TemporaryDirectory() as tmp:
        jobs, plan = [], []
        for ev, (prio, cd, lines) in EVENTS.items():
            if only and ev != only:
                continue
            for li, text in enumerate(lines):
                for vi, (vname, vrate, _, _) in enumerate(VOICES):
                    raw = os.path.join(tmp, "%s_%d_%s.wav" % (ev, li + 1, SUFFIX[vi]))
                    jobs.append((raw, vname, vrate, text.replace(". ", " ").replace("? ", " ")))
                    plan.append((ev, li, vi, raw))
        print("speaking %d clips..." % len(jobs))
        synth_all(jobs, tmp)
        durations = {}
        for n, (ev, li, vi, raw) in enumerate(plan):
            samples, rate = read_wav(raw)
            assert rate == RATE, rate
            _, _, factor, drive = VOICES[vi]
            seed = sum(map(ord, ev)) * 131 + li * 17 + vi * 7
            name = "%s_%d_%s.wav" % (ev, li + 1, SUFFIX[vi])
            d = write_wav(os.path.join(OUT, name), radio(samples, factor, drive, seed))
            durations[(ev, li, vi)] = d
            print("  %-26s %.2f s" % (name, d))

    for ev, (prio, cd, lines) in EVENTS.items():
        if only and ev != only:
            continue
        manifest["events"][ev] = {
            "priority": prio,
            "cooldown": cd,
            "lines": [
                {
                    "text": text,
                    "files": ["%s_%d_%s.wav" % (ev, li + 1, SUFFIX[vi]) for vi in range(len(VOICES))],
                    "duration": [round(durations[(ev, li, vi)], 3) for vi in range(len(VOICES))],
                }
                for li, text in enumerate(lines)
            ],
        }
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print("wrote %s (%d events)" % (manifest_path, len(manifest["events"])))


if __name__ == "__main__":
    main()
