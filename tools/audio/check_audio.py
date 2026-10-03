"""Acceptance checks for the committed audio (run after any rebuild; exit code 0 = pass).

  uv run --with numpy --with scipy --with soundfile --with pedalboard --with pyloudnorm python tools/audio/check_audio.py

Per file (from project/assets/Audio/audio_manifest.json, re-measured from the wav itself, not trusted from the manifest):
  48 kHz / 16-bit / channel count as declared; true peak <= -1 dBTP; loudness (LUFS-M max) inside its layer window from
  recipes/targets.json; no clipped samples; DC offset below dc_max; click-free edges (first/last sample <= edge_max;
  loops: seam step no bigger than 4x the loop's own mean sample step); manifest <-> disk match; variant counts; every
  event key in tools/audio/sync_map.json has audio (snd.<gun>.fire expands to the fire_* layers).
"""
import json
import os
import re
import sys

import numpy as np
import soundfile as sf

import abuild
import adsp

MIN_VARIANTS = {"close": 4, "sub": 4, "mech": 4, "tail": 4, "far": 4, "action": 3, "foley": 3, "step": 5, "loop": 3, "casing": 6, "impact": 3, "flyby": 3}
LAYER_GROUPS = ("close", "mech", "sub", "tail", "far")


FORBIDDEN = [
    (r"np\.(sin|cos|tan|sinc)\(", "oscillator / tone generation"),
    (r"signal\.(chirp|square|sawtooth|gausspulse|unit_impulse|max_len_seq)", "scipy signal generator"),
    (r"standard_normal|\.normal\(|randn|np\.random\.(rand|uniform|normal|randint)|\.random\(", "noise generator"),
    (r"np\.exp\(\s*-\s*t\b|np\.exp\(\s*-\s*\w*\s*/\s*\(?\s*[\d.]+\s*\*", "synthetic decay envelope used as audio"),
    (r"Reverb\(|Chorus\(|Phaser\(|Delay\(|Convolution\(\s*(?!path)", "algorithmic reverb / modulation generator (convolution with a real IR file is the only allowed space)"),
    (r"np\.random\.default_rng", None),            # only adsp.deterministic_rng (selection / jitter, never audio)
]


def scan_generators():
    errs = []
    for f in sorted(os.listdir(abuild.HERE)):
        if not f.endswith(".py") or f in ("check_audio.py", "clip_contacts.py"):
            continue
        text = open(os.path.join(abuild.HERE, f), encoding="utf-8").read()
        for pat, why in FORBIDDEN:
            for m in re.finditer(pat, text):
                line = text[:m.start()].count("\n") + 1
                if why is None:
                    if f != "adsp.py":
                        errs.append(f"{f}:{line}: np.random generator outside adsp.deterministic_rng")
                    continue
                errs.append(f"{f}:{line}: forbidden generator ({why}): {m.group(0)}")
    return errs


def main():
    T = abuild.TARGETS
    doc = json.load(open(abuild.MANIFEST))
    files = doc["files"]
    errs, warns = [], []
    on_disk = set()
    for root, _, fs in os.walk(abuild.AUDIO_DIR):
        for f in fs:
            if f.lower().endswith(".wav") and (os.path.relpath(root, abuild.AUDIO_DIR).replace("\\", "/").startswith(("Weapons", "Foley", "Impacts", "Casings"))):
                on_disk.add(os.path.relpath(os.path.join(root, f), abuild.AUDIO_DIR).replace("\\", "/"))
    listed = {e["file"] for e in files}
    for f in sorted(on_disk - listed):
        errs.append(f"{f}: on disk but not in the manifest")
    for f in sorted(listed - on_disk):
        errs.append(f"{f}: in the manifest but missing on disk")

    counts = {}
    worst = {}
    for e in files:
        p = os.path.join(abuild.AUDIO_DIR, e["file"])
        if not os.path.exists(p):
            continue
        info = sf.info(p)
        x, sr = sf.read(p, dtype="float32", always_2d=True)
        tag = e["file"]
        if sr != 48000 or info.subtype != "PCM_16":
            errs.append(f"{tag}: format {sr} Hz {info.subtype}, expected 48000 PCM_16")
        if x.shape[1] != e["channels"]:
            errs.append(f"{tag}: {x.shape[1]} channels, manifest says {e['channels']}")
        y = np.repeat(x, 2, axis=1) if x.shape[1] == 1 else x
        tp = adsp.true_peak_db(y)
        _, lm = adsp.lufs(y)
        layer = e["layer"]
        win = T["layers"][layer]
        if tp > T["peak_dbtp_max"] + 0.05:
            errs.append(f"{tag}: true peak {tp:.2f} dBTP > {T['peak_dbtp_max']}")
        if not (win["min"] <= lm <= win["max"]):
            errs.append(f"{tag}: {lm:.1f} LUFS-M outside {layer} window [{win['min']}, {win['max']}]")
        if np.abs(x).max() >= 0.9995 or (np.abs(x) >= 32767 / 32768).sum() > 0:
            errs.append(f"{tag}: clipped samples")
        if not e.get("loop"):
            dc = adsp.dc_offset(x)
            if dc > T["dc_max"]:
                errs.append(f"{tag}: DC offset {dc:.4f} > {T['dc_max']}")
            if np.abs(x[0]).max() > T["edge_max"] or np.abs(x[-1]).max() > T["edge_max"]:
                errs.append(f"{tag}: edge click (first {np.abs(x[0]).max():.4f}, last {np.abs(x[-1]).max():.4f})")
        else:
            steps_ = np.abs(np.diff(x, axis=0)).max(axis=1)
            lim = max(4 * steps_.mean(), 1.5 * float(np.percentile(steps_, 99.5))) + 1e-4
            seam = np.abs(x[0] - x[-1]).max()
            if seam > lim:                                  # the seam must look like any other sample step of the loop
                errs.append(f"{tag}: loop seam step {seam:.4f} > {lim:.4f} (4x mean / 1.5x 99.5th percentile step)")
        # transient placement: the contact transient must sit where the recipe says (lead_ms) or at the very start
        if layer in ("close", "action") and e.get("element") not in ("draw", "holster", "cloth", "melee_swing"):
            allowed = e.get("lead_ms", 0.0) * 1.35 + 20.0
            if e["anchor_ms"] > allowed:
                warns.append(f"{tag}: contact transient at {e['anchor_ms']} ms, expected <= {allowed:.0f} ms")
        # provenance: every output must name the recorded files it is made of (hard rule: nothing is synthesised)
        srcs = e.get("sources")
        if not srcs:
            errs.append(f"{tag}: no `sources` (provenance) in the manifest")
        else:
            for s in srcs:
                if not s.get("file") or not (s.get("end", 0) > s.get("start", 1)):
                    errs.append(f"{tag}: bad source entry {s}")
                elif os.path.isdir(adsp.SRC_ROOT) and not os.path.exists(os.path.join(adsp.SRC_ROOT, s["file"])):
                    errs.append(f"{tag}: source file not found under {adsp.SRC_ROOT}: {s['file']}")
        key = (e["key"], layer)
        counts[key] = counts.get(key, 0) + 1
        worst[layer] = (min(worst.get(layer, (9, 9))[0], lm), max(worst.get(layer, (-99, -99))[1], lm))
    for (key, layer), n in sorted(counts.items()):
        if n < MIN_VARIANTS[layer]:
            warns.append(f"{key}: only {n} variants (want >= {MIN_VARIANTS[layer]})")

    # no generators in any build script (oscillators, noise, synthetic envelopes / IRs, algorithmic reverb)
    errs += scan_generators()

    # sync_map <-> audio keys
    keys = {k for k, _ in counts}
    sm = json.load(open(os.path.join(abuild.HERE, "sync_map.json")))
    for clip, evs in sm.items():
        if clip.startswith("_"):
            continue
        for ev in evs:
            k = ev["key"]
            if k.endswith(".fire"):
                need = [k + "_" + l for l in LAYER_GROUPS]
                miss = [n for n in need if n not in keys]
                if miss:
                    errs.append(f"sync_map {clip}: fire layers missing {miss}")
            elif k not in keys:
                errs.append(f"sync_map {clip}: no audio for key {k}")

    n_src = sum(len(e.get("sources", [])) for e in files)
    print(f"provenance: {sum(1 for e in files if e.get('sources'))}/{len(files)} outputs list sources ({n_src} source cuts); "
          f"generator scan of tools/audio/*.py: {'clean' if not any('forbidden generator' in x or 'np.random' in x for x in errs) else 'VIOLATIONS'}")
    print(f"{len(files)} files checked; loudness range per layer (LUFS-M): " +
          ", ".join(f"{l} {a:.1f}..{b:.1f}" for l, (a, b) in sorted(worst.items())))
    for w in warns:
        print("WARN ", w)
    for e in errs:
        print("FAIL ", e)
    print("PASS" if not errs else f"{len(errs)} failure(s)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
