"""Turn an NPC test recording into videos with sound.

    NPC_TEST_RECORD=<dir> TartarusEngine.exe --npc-test fight      # frames + audio.txt into <dir>
    uv run --with numpy --with imageio-ffmpeg python tools/mix_npc_video.py <dir>

<dir> holds game_NNNNN.jpg / scene_NNNNN.jpg (30 fps) and audio.txt (CombatFx's log: "S" lines for
every sound started, "L" lines for the listener each frame). The soundtrack is mixed offline from
assets/Audio/Combat with the engine's inverse distance model and a simple pan, heard from the
player's head. Writes <dir>/npc_pov.mp4 (the player's view) and <dir>/npc_cinematic.mp4 (the
Scene-view camera).
"""
import os
import subprocess
import sys
import wave

import numpy as np

RATE = 44100
FPS = 30
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "project")


def load_wav(path, cache={}):
    if path not in cache:
        with wave.open(path, "rb") as w:
            data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
            if w.getnchannels() == 2:
                data = data.reshape(-1, 2).mean(axis=1)
        cache[path] = data
    return cache[path]


def mix(log_path, seconds):
    out = np.zeros((int(seconds * RATE) + RATE * 2, 2), dtype=np.float32)
    listener = []  # (t, pos, fwd)
    sounds = []
    for line in open(log_path):
        f = line.split()
        if not f:
            continue
        if f[0] == "L":
            listener.append((float(f[1]), np.array(list(map(float, f[2:5]))), np.array(list(map(float, f[5:8])))))
        elif f[0] == "S":
            sounds.append((float(f[1]), f[2], float(f[3]), float(f[4]), f[5] == "1", np.array(list(map(float, f[6:9]))),
                           float(f[9]), float(f[10])))
    times = np.array([l[0] for l in listener]) if listener else np.zeros(1)
    for t, name, vol, pitch, at2d, pos, mind, maxd in sounds:
        src = load_wav(os.path.join(ROOT, "assets", "Audio", "Combat", name))
        if abs(pitch - 1.0) > 1e-3:
            n = int(len(src) / pitch)
            src = np.interp(np.arange(n) * pitch, np.arange(len(src)), src).astype(np.float32)
        gl = gr = vol
        if not at2d and listener:
            i = int(np.clip(np.searchsorted(times, t), 0, len(listener) - 1))
            lp, lf = listener[i][1], listener[i][2]
            d = np.linalg.norm(pos - lp)
            d = min(max(d, mind), maxd)
            g = mind / (mind + (d - mind))  # miniaudio's inverse model, rolloff 1
            right = np.cross(lf, [0.0, 1.0, 0.0])
            right /= max(np.linalg.norm(right), 1e-6)
            p = float(np.dot((pos - lp) / max(np.linalg.norm(pos - lp), 1e-6), right))
            p *= 0.8  # never hard-panned
            gl = vol * g * np.sqrt((1 - p) / 2) * 1.4142
            gr = vol * g * np.sqrt((1 + p) / 2) * 1.4142
        s = int(t * RATE)
        if s >= len(out):
            continue
        e = min(len(out), s + len(src))
        out[s:e, 0] += src[: e - s] * gl
        out[s:e, 1] += src[: e - s] * gr
    # A gentle bus limiter: a firefight stacks a lot of reports.
    peak = np.max(np.abs(out)) if out.size else 1.0
    out = np.tanh(out * (1.6 / max(peak, 1e-6))) * 0.9
    return out[: int(seconds * RATE)]


def write_wav(path, stereo):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((np.clip(stereo, -1, 1) * 32767).astype(np.int16).tobytes())


def ffmpeg_exe():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        return "ffmpeg"


def encode(ff, d, stem, audio, out):
    if not os.path.exists(os.path.join(d, stem + "_00000.jpg")):
        return False
    cmd = [ff, "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", os.path.join(d, stem + "_%05d.jpg"), "-i", audio,
           "-vf", "scale=1920:-2", "-c:v", "libx264", "-preset", "slow", "-crf", "20", "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out]
    subprocess.run(cmd, check=True)
    return True


def main():
    d = sys.argv[1]
    frames = len([f for f in os.listdir(d) if f.startswith("game_") and f.endswith(".jpg")])
    seconds = frames / FPS
    audio = os.path.join(d, "soundtrack.wav")
    log = os.path.join(d, "audio.txt")
    write_wav(audio, mix(log, seconds) if os.path.exists(log) else np.zeros((int(seconds * RATE), 2)))
    ff = ffmpeg_exe()
    for stem, name in (("game", "npc_pov.mp4"), ("scene", "npc_cinematic.mp4")):
        if encode(ff, d, stem, audio, os.path.join(d, name)):
            print("wrote", os.path.join(d, name), "(%.1f s)" % seconds)


if __name__ == "__main__":
    main()
