"""Run the mastering EQ (eqstage, recipes/eq.json) once over the shipped sounds cooked before it existed.

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/remaster.py [key-regex] [--dry]

For every manifest entry with no `eq` version whose key a rule matches: read its wav, equalise and gate it, level it back to its
loudness, write it in place (same channels), and update its measurements, its anchor and `eq` / `eq_rule`. An entry already
equalised (any version) is never touched again: rebuild it with its build script after a rule changes. Writes the manifest
(mix levels recomputed by mixspec as with every build).
"""
import os
import re
import sys

import numpy as np
import soundfile as sf

import abuild
import adsp
import eqstage


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = re.compile(args[0]) if args else None
    dry = "--dry" in sys.argv
    import json
    doc = json.load(open(abuild.MANIFEST))
    files = doc["files"]
    version = eqstage.cfg()["version"]
    done = skipped = 0
    for e in files:
        if only and not only.search(e["key"]):
            continue
        rule = eqstage.rule_for(e["key"])
        if rule is None:
            continue
        if "eq" in e:
            skipped += 1
            continue
        path = os.path.join(abuild.AUDIO_DIR, e["file"])
        x, sr = sf.read(path, dtype="float32", always_2d=True)
        if sr != adsp.SR:
            raise SystemExit(f"{e['file']}: {sr} Hz")
        if x.shape[1] == 1:
            x = np.repeat(x, 2, axis=1)
        y, name = eqstage.apply(x, e["key"], e["layer"], loop=bool(e.get("loop")))
        if dry:
            print(f"{e['file']:48s} {name}")
            done += 1
            continue
        adsp.write_wav(path, y, mono=e.get("channels", 2) == 1)
        z, _ = sf.read(path, dtype="float32", always_2d=True)
        z = np.repeat(z, 2, axis=1) if z.shape[1] == 1 else z
        e.update(abuild.measure(z))
        e["anchor_ms"] = round(adsp.anchor_ms(z), 1)
        e["eq"] = version
        e["eq_rule"] = name
        done += 1
        print(f"{e['file']:48s} {name:20s} LUFS-M {e['lufs_m_max']:6.1f}  TP {e['true_peak_dbtp']:5.1f}")
    print(f"{done} equalised, {skipped} already were")
    if not dry and done:
        print("manifest files:", abuild.update_manifest(files, [""]))


if __name__ == "__main__":
    main()
