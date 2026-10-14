#!/usr/bin/env python3
"""Compares two folders of PNG captures pixel by pixel (docs/JOB_SYSTEM.md, Phase 0).

    python tools/compare_shots.py <reference_dir> <candidate_dir> [--tolerance N] [--diff-dir DIR]

Every PNG in the reference folder must exist in the candidate folder with the same size, and every pixel must match
within --tolerance (per 8-bit channel; default 0, bit-exact). With --diff-dir, each mismatching pair also gets a
<name>_diff.png there: black where the pixels match, the difference scaled up where they don't.

Exit code 0 = everything matched; 1 = a mismatch, a missing file or an empty reference folder.

Made for the --fixed-step smoke captures: the same scenes rendered with jobs on and with --jobs 0 must come out
identical, so a threading change that alters a frame fails here. Standard library only (zlib), so it runs on a CI
runner with nothing installed.
"""

import argparse
import os
import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_png(path):
    """Returns (width, height, channels, rows) for an 8-bit, non-interlaced grey/RGB/RGBA PNG; rows are bytes."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != PNG_SIGNATURE:
        raise ValueError(f"{path}: not a PNG")
    pos, idat = 8, []
    width = height = depth = color = interlace = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ValueError(f"{path}: only 8-bit non-interlaced grey/RGB/RGBA PNGs are supported")
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        ftype, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, channels, rows


def write_png(path, width, height, rows):
    """Writes 8-bit RGB rows (bytes of width*3) as a PNG."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(PNG_SIGNATURE)
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def compare(ref_path, cand_path, tolerance, diff_path):
    """Returns None when the images match, else a one-line description of the difference."""
    # The common case, and the cheap one: the same pixels through the same encoder are the same file.
    with open(ref_path, "rb") as a, open(cand_path, "rb") as b:
        if a.read() == b.read():
            return None
    rw, rh, rc, rrows = read_png(ref_path)
    cw, ch, cc, crows = read_png(cand_path)
    if (rw, rh, rc) != (cw, ch, cc):
        return f"size {rw}x{rh}x{rc} vs {cw}x{ch}x{cc}"
    if rrows == crows:
        return None
    differing, worst = 0, 0
    diff_rows = []
    for ra, ca in zip(rrows, crows):
        out = bytearray(rw * 3)
        if ra != ca:
            for x in range(rw):
                d = max(abs(ra[x * rc + k] - ca[x * rc + k]) for k in range(rc))
                if d > tolerance:
                    differing += 1
                    worst = max(worst, d)
                    out[x * 3:x * 3 + 3] = bytes((min(255, d * 8),) * 3)
        diff_rows.append(bytes(out))
    if differing == 0:
        return None
    if diff_path:
        write_png(diff_path, rw, rh, diff_rows)
    return f"{differing} pixel(s) differ ({100.0 * differing / (rw * rh):.3f}%), max channel difference {worst}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--tolerance", type=int, default=0, help="allowed difference per 8-bit channel (default 0)")
    ap.add_argument("--diff-dir", help="write <name>_diff.png here for every mismatch")
    args = ap.parse_args()

    names = sorted(n for n in os.listdir(args.reference) if n.lower().endswith(".png"))
    if not names:
        print(f"compare_shots: no PNGs in {args.reference}")
        return 1
    if args.diff_dir:
        os.makedirs(args.diff_dir, exist_ok=True)
    failures = 0
    for name in names:
        cand = os.path.join(args.candidate, name)
        if not os.path.exists(cand):
            print(f"MISSING  {name}")
            failures += 1
            continue
        diff = os.path.join(args.diff_dir, name[:-4] + "_diff.png") if args.diff_dir else None
        problem = compare(os.path.join(args.reference, name), cand, args.tolerance, diff)
        print(f"{'MATCH   ' if problem is None else 'DIFFER  '} {name}" + ("" if problem is None else f"  {problem}"))
        failures += problem is not None
    extra = sorted(set(n for n in os.listdir(args.candidate) if n.lower().endswith(".png")) - set(names))
    for name in extra:
        print(f"EXTRA    {name} (not in the reference)")
    failures += len(extra)
    print(f"compare_shots: {len(names) - min(failures, len(names))} of {len(names)} match"
          + ("" if failures == 0 else f", {failures} problem(s)"))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
