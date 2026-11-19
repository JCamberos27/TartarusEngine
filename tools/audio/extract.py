"""Extract raw source audio to C:\\tb\\audio-src (never committed).

Sources (outside the repo):
  Tactical Shooter Pack.rar  -> C:\\tb\\audio-src\\tsp
  Boots.zip                  -> C:\\tb\\audio-src\\boots
  AE Master (Unity project)  -> C:\\tb\\audio-src\\ae\\<pack>   (the gore / flesh hits: ColdBore, Free Pack)
Idempotent: skips a source whose output folder already holds wavs.
"""
import os
import subprocess
import zipfile

# Where the purchased archives and the AE Master Unity project sit on this machine (outside the repo).
SRC_ROOT = os.environ.get("TARTARUS_AUDIO_IMPORT", "")
OUT = os.environ.get("TARTARUS_AUDIO_SRC", r"C:\tb\audio-src")  # see adsp.SRC_ROOT
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"
AE_ROOT = os.environ.get("TARTARUS_AE_ASSETS", "")  # the AE Master project's Assets folder
AE_PACKS = [
    ("ColdBore Flesh", r"ColdBore\ProjectileSystem\Demo\Assets\SurfaceAudio\Clips\Flesh",
     [f"FleshImpact_0{i}.wav" for i in range(1, 7)]),
    ("Free Pack", "Free Pack", ["Bloody punch.wav"]),
]


def has_wavs(d):
    return os.path.isdir(d) and any(
        f.lower().endswith(".wav") for _, _, fs in os.walk(d) for f in fs)


def main():
    if not SRC_ROOT or not AE_ROOT:
        raise SystemExit("set TARTARUS_AUDIO_IMPORT (the folder with Tactical Shooter Pack.rar and Boots.zip) and "
                         "TARTARUS_AE_ASSETS (the AE Master project's Assets folder)")
    os.makedirs(OUT, exist_ok=True)
    tsp = os.path.join(OUT, "tsp")
    if not has_wavs(tsp):
        os.makedirs(tsp, exist_ok=True)
        rar = os.path.join(SRC_ROOT, "Tactical Shooter Pack.rar")
        # x = extract with paths; -y yes to all; wavs only (skip .meta)
        subprocess.check_call([UNRAR, "x", "-y", "-inul", rar, "*.wav", tsp + "\\"])
    boots = os.path.join(OUT, "boots")
    if not has_wavs(boots):
        os.makedirs(boots, exist_ok=True)
        with zipfile.ZipFile(os.path.join(SRC_ROOT, "Boots.zip")) as z:
            for n in z.namelist():
                if n.lower().endswith(".wav"):
                    z.extract(n, boots)
    # The AE Master project's bullet-into-flesh and gore hits (Asset Store packs the user owns).
    import shutil
    for pack, folder, names in AE_PACKS:
        dst = os.path.join(OUT, "ae", pack)
        os.makedirs(dst, exist_ok=True)
        for n in names:
            s = os.path.join(AE_ROOT, folder, n)
            if os.path.exists(s) and not os.path.exists(os.path.join(dst, n)):
                shutil.copy2(s, dst)
    n = sum(f.lower().endswith(".wav") for _, _, fs in os.walk(OUT) for f in fs)
    print("extracted wav count:", n)


if __name__ == "__main__":
    main()
