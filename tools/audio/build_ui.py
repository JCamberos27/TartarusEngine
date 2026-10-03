"""Build the recorded UI ticks and the body fall from recipes/ui.json (real recordings only; replaces the synthesised Combat/ placeholders).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/build_ui.py

  project/assets/Audio/UI/hitmarker_<n>.wav, hitmarker_kill_<n>.wav     keys snd.ui.hitmarker, snd.ui.hitmarker_kill   (layer ui)
  project/assets/Audio/Body/body_fall_<n>.wav                           key snd.body_fall                              (layer bodyfall)
"""
import json
import os

import abuild
import adsp
import build_elements as be


def main():
    recipe = json.load(open(os.path.join(abuild.HERE, "recipes", "ui.json")))
    entries = []
    plan = [("hitmarker", "UI", "hitmarker", "snd.ui.hitmarker", "ui"),
            ("hitmarker_kill", "UI", "hitmarker_kill", "snd.ui.hitmarker_kill", "ui"),
            ("body_fall", "Body", "body_fall", "snd.body_fall", "bodyfall")]
    for name, folder, stem, key, layer in plan:
        for n, v in enumerate(recipe[name], 1):
            adsp.take_uses()
            y = be.composite(v)
            uses = adsp.take_uses()
            y = abuild.finish(y, layer)
            rel = f"{folder}/{stem}_{n}.wav"
            e = abuild.emit(rel, y, key, layer, {"variant": n, "source": be.describe(v)}, sources=uses)
            entries.append(e)
            print(f"{rel:34s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}  {e['length_s']:.2f}s  anchor {e['anchor_ms']} ms")
    print("manifest files:", abuild.update_manifest(entries, ["UI/", "Body/"]))


if __name__ == "__main__":
    main()
