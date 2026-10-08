"""Build the flashlight's switch clicks from one real recording (a small reading flashlight switched on and off, public domain,
pdsounds.org via Wikimedia Commons "Dipswitch on off.ogg"; converted to wav in C:\\tb\\audio-src\\flashlight).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_flashlight.py

  project/assets/Audio/Gear/flashlight_on_<n>.wav, flashlight_off_<n>.wav     keys snd.foley.weapon.flashlight_on / _off (layer foley)
The recording has two clicks while switching on (~0.16 s, ~0.36 s) and two switching off (~2.04 s, ~2.32 s): each is one variant.
"""
import os

import numpy as np

import abuild
import adsp

SOURCE = os.path.join(adsp.SRC_ROOT, "flashlight", "Dipswitch_on_off.wav")
CLICKS = {"flashlight_on": (0.12, 0.32), "flashlight_off": (2.0, 2.28)}   # window starts: each click's peak is found inside
LICENSE = "Public domain (released by its author 'stephan', pdsounds.org; Wikimedia Commons File:Dipswitch_on_off.ogg)"


def main():
    entries = []
    for stem, starts in CLICKS.items():
        for n, t in enumerate(starts, 1):
            adsp.take_uses()
            x = adsp.load(SOURCE)
            a = int(t * adsp.SR)
            w = np.abs(x[a:a + int(0.12 * adsp.SR)]).max(axis=1)
            peak = a + int(np.argmax(w))
            y = adsp.cut(x, peak - int(0.004 * adsp.SR), peak + int(0.12 * adsp.SR))
            uses = adsp.take_uses()
            y = abuild.finish(y, "foley")
            rel = f"Gear/{stem}_{n}.wav"
            e = abuild.emit(rel, y, f"snd.foley.weapon.{stem}", "foley", {"variant": n, "license": LICENSE}, sources=uses)
            entries.append(e)
            print(f"{rel:30s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s")
    print("manifest files:", abuild.update_manifest(entries, ["Gear/flashlight_"]))


if __name__ == "__main__":
    main()
