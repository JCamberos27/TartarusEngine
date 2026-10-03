"""Extract raw source audio to C:\\tb\\audio-src (never committed).

Sources (outside the repo):
  Tactical Shooter Pack.rar  -> C:\\tb\\audio-src\\tsp
  Boots.zip                  -> C:\\tb\\audio-src\\boots
Idempotent: skips a source whose output folder already holds wavs.
"""
import os
import subprocess
import zipfile

SRC_ROOT = r"C:\Users\jacob\OneDrive\Desktop\ASSETS TO IMPORT"
OUT = r"C:\tb\audio-src"
UNRAR = r"C:\Program Files\WinRAR\UnRAR.exe"


def has_wavs(d):
    return os.path.isdir(d) and any(
        f.lower().endswith(".wav") for _, _, fs in os.walk(d) for f in fs)


def main():
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
    n = sum(f.lower().endswith(".wav") for _, _, fs in os.walk(OUT) for f in fs)
    print("extracted wav count:", n)


if __name__ == "__main__":
    main()
