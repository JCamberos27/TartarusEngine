"""Rewrite `mix_db` on every entry of audio_manifest.json from recipes/mix.json (no audio is touched).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/apply_mix.py
"""
import json

import abuild
import mixspec


def main():
    doc = json.load(open(abuild.MANIFEST))
    doc["mix"] = mixspec.apply_to(doc["files"])
    with open(abuild.MANIFEST, "w") as fh:
        json.dump(doc, fh, indent=1)
    m = doc["mix"]
    print(f"shot reference {m['shot_lufs_m']} LUFS-M (as played x{m['player_gain']}: {m['reference_lufs_m']}); {len(doc['files'])} entries")


if __name__ == "__main__":
    main()
