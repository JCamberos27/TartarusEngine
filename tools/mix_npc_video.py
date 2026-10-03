"""Turn an NPC test recording into videos with sound.

    NPC_TEST_RECORD=<dir> TartarusEngine.exe --npc-test fight      # frames + audio.txt into <dir>
    uv run --with numpy --with imageio-ffmpeg python tools/mix_npc_video.py <dir>

<dir> holds game_NNNNN.jpg / scene_NNNNN.jpg (30 fps) and audio.txt (CombatFx's log: "S" lines for
every Combat/ placeholder sound started, "W" lines for every weapon / foley voice of Game/Audio (the file, project
relative, with the volume, pitch, 3D range and the seek offset it started at), "L" lines for the listener each frame).
The soundtrack is mixed offline with the engine's inverse distance model and a simple pan, heard from the player's head;
2D voices keep their stereo. WEAPON_TEST_AUDIO_LOG=<file> / NPC_TEST_RECORD record --weapon-test too (no L lines there:
the listener is the player and every voice is 2D). Writes <dir>/npc_pov.mp4 (the player's view) and <dir>/npc_cinematic.mp4 (the
Scene-view camera).
"""
import os
import subprocess
import sys
import wave

import numpy as np

RATE = 48000
FPS = 30
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "project")


def load_wav(path, cache={}):
    """(frames, 2) float32 at RATE, whatever the file's channels / rate / sample width."""
    if path not in cache:
        with wave.open(path, "rb") as w:
            width, ch, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
            raw = w.readframes(w.getnframes())
        if width == 2:
            data = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
        elif width == 3:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            data = (np.where(v >= 1 << 23, v - (1 << 24), v)).astype(np.float32) / 8388608.0
        elif width == 4:
            data = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
        else:
            data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
        data = data.reshape(-1, ch)
        if ch == 1:
            data = np.repeat(data, 2, axis=1)
        elif ch > 2:
            data = data[:, :2]
        if rate != RATE:
            n = int(len(data) * RATE / rate)
            x = np.arange(n) * rate / RATE
            data = np.stack([np.interp(x, np.arange(len(data)), data[:, c]) for c in range(2)], axis=1).astype(np.float32)
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
            sounds.append((float(f[1]), os.path.join("assets", "Audio", "Combat", f[2]), float(f[3]), float(f[4]), f[5] == "1",
                           np.array(list(map(float, f[6:9]))), float(f[9]), float(f[10]), 0.0))
        elif f[0] == "W" and len(f) >= 11 and f[3] != "-":
            # W t key file voices vol pitch 2d x y z [min max seek]
            more = list(map(float, f[11:14])) if len(f) >= 14 else [2.0, 40.0, 0.0]
            sounds.append((float(f[1]), f[3], float(f[5]), float(f[6]), f[7] == "1", np.array(list(map(float, f[8:11]))),
                           more[0], more[1], more[2]))
    times = np.array([l[0] for l in listener]) if listener else np.zeros(1)
    for t, name, vol, pitch, at2d, pos, mind, maxd, seek in sounds:
        path = os.path.join(ROOT, name)
        if not os.path.exists(path):
            print("missing", path)
            continue
        src = load_wav(path)
        if abs(pitch - 1.0) > 1e-3:  # the engine's pitch is a playback rate: shorter and higher
            n = int(len(src) / pitch)
            x = np.arange(n) * pitch
            src = np.stack([np.interp(x, np.arange(len(src)), src[:, c]) for c in range(2)], axis=1).astype(np.float32)
        if seek > 0.0:  # started that far into the file (a lead-in skipped to land the contact on its frame)
            src = src[int(seek * RATE / pitch):]
        if not at2d:
            src = src.mean(axis=1, keepdims=True).repeat(2, axis=1)  # a 3D voice is mono
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
        out[s:e, 0] += src[: e - s, 0] * gl
        out[s:e, 1] += src[: e - s, 1] * gr
    # The engine plays at its volumes (the weapon bus limiter is in the voices' gains); only a safety against clipping here.
    peak = np.max(np.abs(out)) if out.size else 1.0
    if peak > 0.98:
        out = np.tanh(out / peak * 1.2) * 0.9
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
